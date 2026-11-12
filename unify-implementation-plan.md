# План: унификация ESP8266 / ESP32 / Beken

<h1><span style="color:red">УНИФИКАЦИЯ. НЕ БЫВАЕТ «ТОЛЬКО ДЛЯ ESP8266», «ТОЛЬКО ДЛЯ ESP32», «ТОЛЬКО ДЛЯ BEKEN».</span></h1>

<h2><span style="color:red">КАЖДАЯ ЧАСТЬ — ОДНА НА ТРИ ЧИПА. ПО ЧИПУ ТОЛЬКО ТО, ЧЕГО НЕ ДАЁТ ЖЕЛЕЗО, С ПРИЧИНОЙ В КОДЕ. НАШЁЛ «ТОЛЬКО ДЛЯ» — ЭТО ЗАДАЧА В ЭТОТ ПЛАН, А НЕ ОПИСАНИЕ.</span></h2>

<h2><span style="color:red">ВЛАДЕЛЕЦ ПОВТОРЯЛ ЭТО ДВАДЦАТЬ РАЗ (11-12: «ПОЧЕМУ ОН ТОЛЬКО ДЛЯ ESP8266… ДВАДЦАТЫЙ РАЗ»).</span></h2>

Переписан 2026-11-12: выполненные этапы старого плана (0–10, раздел 14, журналы стендов 11-10/11-11) убраны — они в
истории git (`git log -- unify-implementation-plan.md`) и в [review-unify-plan.md](review-unify-plan.md). Здесь только
правила, что сделано и ждёт сборки, и что осталось.

## 1. Главное правило: одна версия всего

Владелец 11-11: «почему нельзя сделать одну версию всего, а не три с одинаковым интерфейсом», «не должно остаться копий
кода». Upstream больше не цель.

**Каждая функция существует в одном экземпляре.** По чипам расходится только тонкий слой, который диктует железо:
компилятор и набор команд; доступ к флешу (Beken: шифрование и CRC каждые 32 байта); карта памяти (ESP8266 — IRAM,
Beken — TCM); запуск кода из флеша; вызовы SDK Wi-Fi; исключения и дамп падения; разметка разделов загрузчика.
Исключение предлагается один раз, с цифрами, а не «пока так».

**Никаких режимов и флагов-вариантов.** Необязательная возможность — модуль (включается Apply с перезагрузкой), а не
`#define`. Сборок без `MODULES_SUPPORT` нет.

**Библиотеки — форками и одной версией.** Каждая библиотека и платформа один раз в `code/libraries.ini`; свои правки —
в форк `igorufox/*` с релизом, ini закрепляет тег.

Исключения, принятые с причиной (не задачи):

| Что | Почему |
|---|---|
| `os_timer` ESP8266 | срабатывает внутри `delay()`; на ESP32/Beken сделано так же своим кодом (`delay_unlocked()`), см. раздел 3 |
| свои `__atomic_*` Beken (`compat_beken.cpp`) | нужны готовой библиотеке Matter и модулям, ARMv5 без атомарных инструкций — уходят с задачей U5 |
| lwIP 2.1.3 на ESP8266 и migration image, 2.2.0 на остальных | решение владельца 11-12 (V1): ядро ESP8266 NONOS не пересобираем; спросит «почему» — объяснить сразу: последний выпуск ядра 3.1.2 = lwIP 2.1.3, переноса связки на 2.2 нет, свой перенос = два форка (связка + ядро) под выброс при переходе на RTOS SDK, нашему коду 2.2.0 ничего не даёт |

## 2. Правила работы

| Правило | Что значит |
|---|---|
| одно дерево | без клонов, копий, своих папок сборки, флагов-переключателей и вторых вариантов кода |
| три env | `esp8266`, `esp32`, `beken`; задача готова, когда собираются все три |
| сначала вся унификация | доделать все задачи раздела 4 за один проход, потом ОДНА сборка на чип и ОДИН регресс; без цикла «собрал — поправил» |
| сборки | только через сессию-сборщик, одна сборка за раз |
| стенд | ESP8266 — 10.0.5.0 и 10.0.5.58 (Sonoff Basic 1 МБ); ESP32 — 10.0.5.66; Beken — 10.0.5.139; их можно рисковать |
| платы в стенах | не трогаются этим планом; раскатка — отдельно и по слову владельца |
| новый путь загрузки или записи флеш | сначала на стенде с проводом, потом по воздуху |
| найденная ошибка | задача в раздел 5, никогда не «известная» |
| коммиты | по слову владельца |

Базовая проверка каждой задачи (на регрессе):

1. собираются три env; размер образа и статическая RAM не выросли без причины (числа до и после);
2. плата загружается, входит в Wi-Fi, WebUI открывается, MQTT подключён;
3. OTA на новую сборку и обратно сохраняет настройки;
4. heap через 10 минут работы не ниже, чем до задачи.

## 3. Сделано в коде, ждёт сборки

Последняя сборка трёх чипов — 11-11 ночью (этапы старого плана 0–10). Всё ниже сделано после неё, **не собрано и на
платах не проверено**; проверяется одной сборкой и регрессом (раздел 6).

| # | Что | Где |
|---|---|---|
| С1 | Один HTTP-клиент на три чипа, без Arduino: lwIP raw TCP (`libs/tcp.h`), BearSSL; голова — статический RSA + AES-GCM, модуль `tls` (`module_tls.cpp`) добавляет ECDHE/ECDSA/ChaCha20/X.509 через слоты `Api::HttpTlsExtend`/`HttpTlsKey`; migration image и safeboot — тот же код (`libs/net_impl.h`). На нём: OTA по ссылке на трёх чипах (`ota_fetch.cpp`: WebUI, терминал `ota <url> [sha256]`, MQTT `<root>/ota`), погода/URL/TFT NSPanel, ThingSpeak, InfluxDB, safeboot ESP32 (без `esp_http_client` и mbedTLS-TLS) | `libs/http_client*.h`, `net.cpp` |
| С2 | Один веб-сервер на lwIP (`libs/http_server.h`): голова трёх чипов, migration image, safeboot-app; ESPAsyncWebServer, `ESP8266WebServer` (`ota_basicweb.cpp`), сервер migration image и `esp_http_server` убраны. Переведены `web.cpp`, API, терминал, `ota_web.cpp`, `modules.cpp`, `captive.cpp`, `prometheus.cpp`, `ssdp.cpp`, `nspanel_web.cpp`, `safeboot_esp32.cpp`, `alexa.cpp` | `libs/http_server*.h` |
| С3 | Модуль `ws`: сервер WebSocket (`libs/websocket*.h`, `ws_server.cpp`, `module_ws.cpp`); в голове колбэки `wsRegister()`, помощники и переадресация (`ws.cpp`, `modules_ws.h`); `webui` требует `ws` (`DATA_MODULE_REQUIRES`), страница головы ставит зависимости галочкой | `ws*.cpp` |
| С4 | Один вход: Digest сервера (MD5 BearSSL, stale-nonce; Basic принимается) в голове, migration image и safeboot-app; safeboot узнаётся по realm «<hostname> safeboot» | `http_server_impl.h` |
| С5 | Форк fauxmoESP `3.4.1-espurna.2`: ответ через колбэк, без своего TCP-сервера и AsyncTCP; ALEXA тянет WEB | `alexa.cpp` |
| С6 | Форк BearSSL `igorufox/bearssl-esp8266` `0.6.0-espurna.1` для ESP32, Beken, safeboot | `libraries.ini` |
| С7 | `code/libraries.ini`: все версии один раз, пять конфигов через `${libraries.*}`, `ci_ini_lint.py` проверяет; AsyncMqttClient на ESP8266 — тот же форк esphome | `libraries.ini` |
| С8 | Одна функция JSON-экранирования, URL и base64 (`libs/text.h`, `libs/url_parse.h`); SHA-256/MD5 safeboot и Digest migration image — BearSSL | `libs/text.h` |
| С9 | Модуль `uart` одинаковый на трёх чипах, голова — переадресация; nspanel зависит от `uart tls` | `module_uart.cpp` |
| С10 | `system::arch` → `system::chip`, `time::arch` → `time::chip`; таймеры ESP32/Beken в `loop` и внутри `delay()` как ESP8266; `std::atomic` → `espurna::system::Shared`; делитель SPI RFM69 → `chip::rfm69_spi_divider()` | `system*.cpp` |
| С11 | Wi-Fi: один lwIP-слой в `wifi.cpp`, gratuitous ARP и netwatch общие (netwatch работает, где lwIP со счётчиками; ESP32 — проверить) | `wifi.cpp` |
| С12 | MQTT и telnet без AsyncTCP (11-12): TLS вынесен из HTTP-клиента в общий поток `libs/tls.h` + `tls_impl.h` (TCP, TLS 1.2 BearSSL, проверка отпечатка SHA-1 сертификата, MFLN; хуки модуля `tls` — `espurna::tls::extend/key`, `Api::TlsExtend/TlsKey`, `libs/tls_full.h` вместо `http_client_tls.h`); HTTP-клиент на нём. Свой клиент MQTT 3.1.1 `libs/mqtt_client.h` (QoS 0/1/2 в обе стороны, will, keep alive, PINGREQ, сообщение целиком до `MQTT_BUFFER_MAX_SIZE`) — `mqtt.cpp` на нём, всё в loop, без отложенных событий; `MQTT_LIBRARY`, `SECURE_CLIENT`, `SecureClientHelpers.h`, `sslFingerPrint*` убраны; AsyncMqttClient, arduino-mqtt, PubSubClient, AsyncTCP, ESPAsyncTCP убраны из `libraries.ini` и трёх конфигов (и `CONFIG_ASYNC_TCP_*` Beken). telnet на `tcp::Listener` / `tcp::Connection` (вход, обратное соединение — имя разрешает само соединение) | `libs/tls*.h`, `libs/mqtt_client*.h`, `mqtt.cpp`, `telnet.cpp` |

Риски первой сборки (смотреть сразу):

- свой `inc/pgmspace.h` форка BearSSL рядом с `pgmspace.h` ядер ESP32/LibreTiny;
- `extra_configs = libraries.ini` в ini Beken;
- `lib_deps` BearSSL в сборке ESP-IDF safeboot-app (`lib_compat_mode = off`); перед сборкой удалить старый
  `sdkconfig.esp32-safeboot`;
- модуль `ws`: `br_sha1_*` при линковке модуля — из головы или из библиотеки;
- экспорты головы для новых модулей (`ws`, `tls`) — `module_link.py` допишет `modules_exports.txt`, голова
  пересоберётся.

## 3а. Разные версии и реализации одного и того же (опись 11-12)

Владелец 11-12: «у нас один файл зависимостей — не может быть разных версий», «одна реализация для всего». То, что
приходит внутри платформы чипа, тоже зависимость: версия — в `code/libraries.ini`, сборка проверяет.

| # | Что | Сейчас | Во что | Задача |
|---|---|---|---|---|
| V1 | lwIP | ESP8266 2.1.3 (lwip2 ядра), ESP32 2.2.0 (esp-lwip, "2.2.0d"), Beken 2.2.0 (`library-lwip`) | **решено владельцем 11-12: две версии, так и оставить** («остаёмся на 2.2 и 2.1, да разнобой, но ядро не пересобираем»). `libraries.ini`: `lwip = 2.2.0` (ESP32, Beken, safeboot), `lwip_esp8266 = 2.1.3` (ESP8266, migration image); `libs/net_impl.h` проверяет при сборке. Почему: ядро ESP8266 — NONOS, его последний выпуск 3.1.2 (март 2023) и master — lwIP 2.1.3 из связки esp82xx-nonos-linklayer, переноса связки на 2.2 нет ни у кого; 2.2.0 там = форк связки + форк ядра с пересобранными `liblwip2-*.a` и заголовками (ядро компилируется против них) — работа, которую выбросит переход на ESP8266_RTOS_SDK (план владельца на следующую неделю); нашему коду из 2.2.0 ничего не нужно (raw TCP API, DNS, `tcpip_api_call` одинаковы) | решено |
| V2 | TCP-библиотека | было: ESPAsyncTCP (ESP8266) и AsyncTCP (ESP32, Beken) | сделано 11-12 (не собрано): `libs/tcp.h` у всех; ESPAsyncTCP, AsyncTCP, `espurna_asynctcp.h`, `AsyncClientHelpers.h` удалены | С12 |
| V3 | MQTT | было: у ESP8266 три библиотеки (AsyncMqttClient, arduino-mqtt, PubSubClient), у ESP32/Beken одна | сделано 11-12 (не собрано): один клиент `libs/mqtt_client.h` | С12 |
| V4 | BearSSL | ESP8266 — из ядра, ESP32/Beken/safeboot — наш форк `0.6.0-espurna.1` | форк везде | U11 |
| V5 | Компилятор GCC | ESP8266 10.3 (`toolchain-xtensa 2.100300`), ESP32 14.2 (`xtensa-esp-elf`), Beken 10.3 (`gccarmnoneeabi 1.100301`; 14.2 = `1.140201` стоит рядом) | одна версия GCC: набор команд у чипов разный (lx106 / lx6 / ARM968) — это железо, версия — нет. Выяснить: GCC 14 для lx106 (сборка esp-quick-toolchain / crosstool-NG Espressif), Beken на `1.140201`; версия — в `libraries.ini` `[platforms]` | новая U25 |
| V6 | Стандарт C++ | ESP8266 gnu++17 (ядро), Beken gnu++17 (наш флаг), ESP32 gnu++2b (IDF 5.5) | один `-std` во всех пяти конфигах, значение в `libraries.ini` | новая U26 |
| V7 | FreeRTOS | ESP32 10.5.1 (IDF), Beken 9.0.0 (`library-freertos`, BDK собран под него); ESP8266 без ОС | версия ядра ОС — в `libraries.ini` с проверкой; Beken на 10.5.1, если BDK соберётся (бинарные части BDK) — выяснить с цифрами | новая U27 |
| V8 | Matter | ESP32 — esp_matter в библиотеках Arduino-ESP32, Beken — наша `matter-bk7231n` | одна версия connectedhomeip в обоих, версия — в `libraries.ini` | U5 |
| V9 | mDNS | ESP8266mDNS ядра, компонент `mdns` IDF, mDNS LibreTiny — три реализации за `mdns_driver.h` | одна (lwIP `apps/mdns` 2.2.0 есть в исходниках lwIP — после V1) | новая U28 |
| V10 | Хранилище настроек | `storage_eeprom_esp8266.cpp` (EEPROM_Rotate), `_esp32`, `_beken`, `_rtos` | одно хранилище над флеш-драйвером чипа (этап 2 старого плана, не доделан); EEPROM_Rotate — только если общий код возьмёт его на всех чипах | новая U29 |
| V11 | Запись прошивки | Updater ядра ESP8266, `Update` Arduino-ESP32 / safeboot `esp_ota_*`, `Update` LibreTiny | один приём образа, у чипа только запись | U1 |
| V12 | Библиотеки датчиков | только у ESP8266 (15 библиотек) | у всех трёх | U12 |

Ядро Arduino (ESP8266 3.1.2, Arduino-ESP32 3.3.x, LibreTiny) и SDK Wi-Fi — платформа чипа, их версии закреплены
`[platforms]`; ESP-IDF головы ESP32 и safeboot — одна (5.5.5).

### Сборка 11-12 (только компиляция)

| Сборка | Итог |
|---|---|
| safeboot-app | собрано: RAM 32108, образ 704000 из 851968 (0xD0000) |
| ESP32 (голова + модули) | собрано: голова RAM 54836, флеш 1315181; все модули слинкованы (nspanel: `FS` добавлен в `custom_module_libs` — раньше его приносил ESPAsyncWebServer) |
| Beken (голова + модули + `.uf2` + `.ota`) | собрано: RAM 91236, флеш 789168; 17 модулей, `.ota` 8271128 |
| ESP8266 | собрано после решения V1: голова RAM 39428, флеш 570403; 16 модулей, `.ota` 897826 (модуль `tls` берёт BearSSL из `libbearssl.a` ядра: `-lbearssl` в `esp8266_module_link.py`) |
| migration-app | собрано: RAM 31944, флеш 315519 |

Исправлено по сборке: комментарий в `http_client.h`, `tls::chip::random` в сервере, `send()` с C-строкой в сервере, `br_x509_decoder_init` форка (5 аргументов), `esp_netif_str_to_ip4` в safeboot, `tcp::Listener()` вне заголовка, `loop` в `web.cpp`, `UART_MODULE` в `head.h`. Форк BearSSL: свой `inc/pgmspace.h` закрывал `pgmspace.h` ядра ESP32 (нет `strlen_P`) — переименован в `bearssl_pgmspace.h` в локальной копии библиотеки; в форк — 11-12, релиз `0.6.0-espurna.2`, `libraries.ini` на нём. `ci_ini_lint.py`: конфиги safeboot-app / migration-app читаются из своей папки (их `../libraries.ini`), `lwip*` — версия платформы. U10: связка lwip2 ядра (esp82xx-nonos-linklayer) есть только под 2.1.3, под 2.2.0 её нет ни у кого — переносить в своём форке.

## 4. Осталось сделать (по порядку)

| # | Задача | Что сделать | Статус |
|---|---|---|---|
| U1 | Запись прошивки (R3) | сделано 11-12: один приёмник образа `libs/image_receiver.h` (`image::Receiver`: поток кусками, первый кусок — цели на проверку заголовка до записи, размер и полнота, отказ/обрыв → `abort()`; он же `http::Sink` для скачивания) + `image::sameOrigin()`. У образа только своя цель записи (`image::Target`): голова — `Update` + проверки драйвера чипа (`ota.cpp` `otaUpdateTarget()`, на нём `/upgrade` и `ota_fetch`), migration image — свободное место + eboot с чтением обратно, safeboot-app — app0 через `esp_ota` (обычный и merged-образ). Проверка Origin теперь везде: голова `/upgrade`, `/ota/fetch`, `/safeboot/*` ESP32; migration image `/upgrade`, `/ota/fetch`; safeboot-app `/upload`, `/url`, `/boot` | сделано, собрано 11-12 |
| U2 | telnet на TCP-слое (R12) | сделано 11-12 (не собрано), см. С12 | собрать |
| U3 | MQTT и TLS (R4, R13, R5) | код сделан 11-12 (С12). Хвосты сделаны 11-12: проверка брокера по CA (`mqttScCheck 2`, решение владельца — вариант 3): PEM из `mqttCA`, пусто — вшитый корень ISRG Root X1 (Let's Encrypt, до 2035; X2 локально нет — не вшит); проверка цепочки `br_x509_minimal` (SHA-1..512, RSA, ECDSA, время по NTP — до синхронизации отказ) в модуле `tls` (`libs/tls_full.h` `caCheck`, слот `Api::TlsCaCheck`), в голове только хук и флаг `tls::Options::ca`; отказы `Untrusted` / `NoCa` с текстом; WebUI MQTT: выбор проверки и поле CA (PEM без переводов строк собирается обратно). `MFLN.PROBE <URL> <SIZE>` на общем TLS (ответ в журнале). `AsyncGuard`, `Deferred`, `lock(timeout)` убраны (`ParallelTasks` остаётся: Wi-Fi/SNTP/Matter в своих задачах) | сделано, собрано 11-12 |
| U4 | Опознание головы (R6) | сделано 11-12: GNU build id на трёх чипах (`scripts/module_head.py`: ESP8266 — `.irom0.text` сгенерированного `local.eagle.app.v6.common.ld` перед линковкой; ESP32 — копия `sections.ld` в папке сборки первой на LIBPATH, `.flash.rodata`; Beken — как было); `module_link.py` `head_id()` один на все чипы; голова читает id из флеша (`modules.cpp`, `memcpy_P`), MD5 образа при работе (`getSketchMD5`) больше не считается, `EspCompat::getSketchMD5` и `arch::headId` убраны; `custom_head_build_id` убран (всегда). Ключ `md5` в `/modules` и строке `info` оставлен (WebUI, страница головы, `bench_regress.py`), значение — build id. Закрывает W2 | сделано, собрано 11-12 |
| U5 | Библиотека Matter Beken (3.3, 6.3) | `igorufox/matter-bk7231n` — наша: одна identity как на ESP32 (`matter_identity_esp32.cpp`), общие примитивы вместо `__atomic_*`; свои `__atomic_*` в `compat_beken.cpp` убрать | делать |
| U6 | NSPanel на трёх чипах (9.4) | Модуль собирается и работает на Beken и ESP8266 (убрать из `custom_modules_skip`), пины экрана в настройках. Осталось только для ESP32: пищалка на LEDC (`nspanel.cpp`), LittleFS на разделе ESP32 (`nspanel_storage.cpp`), `ESP.getFreeHeap` | делать |
| U7 | Выбор чипа и файлы на местах (1.1, 1.5) | `modules_esp8266.cpp`, `heap_reserve_esp8266.cpp` → `arch/esp8266/`; `.cpp` чипа отбирает `build_src_filter` (у ESP8266 сейчас не исключены `arch/beken`), обёртки `#if defined(CHIP)` вокруг целых файлов в `arch/` (18 из 35) убрать | делать |
| U8 | Ветки по чипу в общих файлах (этап 4) | 28 файлов вне `arch/`, `config/`, `libs/` с `#if ESP32/ESP8266/LIBRETINY/ARDUINO_ARCH`: каждое место — в слой чипа или запись причины здесь | делать |
| U9 | Wi-Fi ESP8266 на общий драйвер | ESP8266 не переведён на общий драйвер (`arch/rtos/wifi_driver_rtos`): тайм-аут 15 с и фильтр отключений меняют поведение плат в стенах — решить с владельцем, как свести | вопрос владельцу |
| U10 | lwIP 2.2.0 на ESP8266 (R10) | **закрыто решением владельца 11-12** — см. V1 (две версии, причины в `code/libraries.ini`). Форк `igorufox/esp82xx-nonos-linklayer` удалён 11-12. Вместо этого — переход ESP8266 на ESP8266_RTOS_SDK (план на следующую неделю; там свой lwIP — esp-lwip, по имеющимся данным 2.1.2, проверить) | закрыто |
| U11 | BearSSL ESP8266 на форке (R11) | сделано 11-12: ESP8266 и migration image берут `bearssl-esp8266` из `libraries.ini` (`shared_lib_deps`, `lib_deps` migration image), `libbearssl.a` ядра убран из линковки (`scripts/pio_main.py`, `migration-app/scripts/nofloat.py`); заголовок везде `<bearssl.h>` форка; линковщик модулей ESP8266 берёт BearSSL из архивов сборки (без `-lbearssl`) | сделано, собрано 11-12 |
| U12 | Датчики на трёх чипах (R15) | Библиотеки датчиков и драйверов только у ESP8266 (`shared_lib_deps`): NewPing, VEML6075, VL53L1X, MAX6675, SI1145+BusIO, BSEC, ssd1306, NeoPixel, IRremoteESP8266, RFM69, RPNlib, nofuss → в `lib_deps` трёх чипов; код собрать и починить на ESP32 и Beken (не пропускать). RFM69 на Beken: в LibreTiny нет `SPI.h` | делать |
| U13 | `i2c` — модуль (R16) | `i2c.cpp` из головы в модуль `i2c` на трёх чипах, как `uart`; датчики I2C и термостат с экраном зависят от него | делать |
| U14 | Модули sensors и light (M2, M3) | sensors «на кучу»: ядро отдельно, драйверы модулями (группами по шине/типу), шины `i2c`, `onewire` — свои модули; light «на два»: ядро и провайдер(ы) вывода (pwm …) | делать |
| U15 | Функции вне головы и модулей (M4) | tuya, uartmqtt, ir, rfm69, garland, thermostat, influxdb, prometheus, ssdp, llmnr, netbios, nofuss, rpnrules, curtain_kingart, ifan, lightfox, encoder, ac_switch, mcp23s08 — куда | вопрос владельцу |
| U16 | Из головы в модули (M5) | telnet / API / NTP / OTA по URL; relay / button / LED? | вопрос владельцу |
| U17 | Результат OTA на ESP32 (H9) | сделано 11-12: один результат — тот, что safeboot пишет в NVS `safeboot/result` (`safebootResult()`, `safebootUrl()`); `/ota/status` (`ota_fetch.cpp`) показывает его и URL из NVS, как `/safeboot/status` | сделано, собрано 11-12 |
| U18 | Питание перед сменой разметки (8.3, A9) | сделано 11-12: на странице `/safeboot` ESP32 красное предупреждение у миграции разметки и у установки safeboot, `confirm()` перед обоими | сделано, собрано 11-12 |
| U19 | Сверка ini (7.2) | вид `ESPURNA_IMAGE` одинаковый в трёх ini и в safeboot-app/migration-app | проверить |
| U20 | Библиотеки без пользователей (R14) | `lowpowerlab/SPIFlash`; в `custom_module_libs` ESP32 — `HTTPClient`, `NetworkClientSecure`, `Ticker`. Проверить и убрать; `Adafruit BusIO` остаётся с SI1145 | делать |
| U25 | Одна версия GCC (V5) | см. раздел 3а | выяснить, затем делать |
| U26 | Один стандарт C++ (V6) | см. раздел 3а | делать |
| U27 | Одна версия FreeRTOS (V7) | см. раздел 3а | выяснить с цифрами |
| U28 | Один mDNS (V9) | см. раздел 3а | после U10 |
| U29 | Одно хранилище настроек (V10) | см. раздел 3а | делать |
| U21 | ОДНА сборка на чип | после U1–U20 и U25–U29 (кроме вопросов владельцу): три env + safeboot-app + migration-app через сборщика; риски раздела 3 | после U1–U20 |
| U22 | MD5: BearSSL или ПЗУ чипа (R17) | по цифрам U21: migration image укладывается в запас — оставляем `br_md5`; нет — MD5 из ПЗУ чипа (`MD5Builder` / `esp_rom_md5`), SHA-256 из BearSSL | по сборке |
| U23 | Регресс | раздел 6 | после U21 |
| U24 | Ещё раз библиотеки | владелец 11-12: «после унификации ещё раз смотрим библиотеки, должно всё упроститься сильно» | после U23 |

После унификации (владелец 11-11: «рассмотреть после того, как унификацию сделаем; миграцию — на последнюю версию»):
миграция с чужих прошивок (замена чужого safeboot на ESP32 — Tasmota на NSPanel — была в панели прошивки, убрана при
объединении установки головы). Делать один раз, под итоговую версию.

## 5. Ошибки стенда — задачи

| # | Что | Что известно | Статус |
|---|---|---|---|
| W1 | Beken: перезагрузка по WDT вскоре после автозапуска Matter (`matterAuto 1`) | 11-07 MOES: loop не кормился 8,7 с, задача CHIP писала KVS во флеш (0x1D3C80); 11-11 10.0.5.139: WDT каждые ~99 с, запись CHIP на 0x1eb400, loop стоит ~8,6 с; WDT после Apply с matter. Причина не найдена | открыто, первое после регресса |
| W2 | Beken: голова не отдаёт md5 (`info` → `md5: None`), `bench_regress info` FAIL | закрывается U4: `info` печатает build id головы на всех чипах | проверить на регрессе |
| W3 | Beken: lwIP-куча после старта модулей `max 16148 из 16384`, 15 ошибок выделения (без модулей 3416, 0) | возможно связано с W1 | открыто |
| W4 | ESP32: модули не стартовали после Apply («not in this boot's heap block») | исправлено 11-11 (ленивый `areaReady()`), на плате не проверено | проверить на 10.0.5.66 |
| W5 | telnet: при `connect()` == false объект удалялся, AsyncTCP мог оставить колбэки на pcb | AsyncClient ушёл (U2): соединение `libs/tcp.h`, всё в loop | проверить `TELNET.REVERSE` на недоступный адрес на регрессе |
| W7 | `.ota` Beken 8,3 МБ (Quectel UF2 внутри 4,5 МБ): принимает ли установка головы из `.ota` и `ota_beken.cpp` | не проверено | установкой из WebUI на 10.0.5.139 |
| W8 | MQTT: приватный `AsyncMqttClient::_client`; смена сервера с нерабочего на рабочий без перезагрузки | библиотека ушла (U3): попытка останавливается `stop()` своего клиента | проверить на регрессе |
| W10 | CI: модуль `webui` из `code/html/build/index.html.gz` (в .gitignore) — получает ли CI файл до сборки | не проверено | проверить workflow |
| W13 | Matter: matter-server, затем сопряжение со станцией на ESP32 и Beken | | после W1 |
| W14 | Свежие сборки на парк | | после регресса, волнами, с разрешения владельца |

## 6. Регресс (после U21)

На 10.0.5.0 / 10.0.5.58 (ESP8266), 10.0.5.66 (ESP32), 10.0.5.139 (Beken), каждое — на трёх чипах, где применимо:

1. базовая проверка (раздел 2);
2. веб-сервер и вход: WebUI, Digest, API, терминал, `/upgrade`, страница головы без `webui`, `/safeboot` ESP32 и вход
   в safeboot (realm), migration image 1 МБ ESP8266 (страница, `/upgrade`, `/ota/fetch`);
3. модуль `ws`: WebUI с `ws`, без `ws` (страница головы, галочка webui ставит ws), 100 сессий WS, журнал, Matter-сопряжение
   с открытым WebUI (сокеты не рвутся);
4. HTTP-клиент: OTA по ссылке http и https (с `tls` и без) на трёх чипах из WebUI, терминала и MQTT; safeboot ESP32 по
   ссылке; погода, URL-плитки и TFT NSPanel; ThingSpeak, InfluxDB;
5. Apply из WebUI напрямую и через прокси `igoru.us.to/espurna/<ip>/` (11-11 через прокси плата оборвала
   `POST /modules/apply`, Exception 29 — вероятно стек в `onStatus`/`prepare`);
6. модули: выключение без файла, mdns ↔ matter на ESP32, три включения выключателем (ступень восстановления 2), выход из
   нестабильного режима с отметкой;
7. Alexa (fauxmo через веб-сервер): обнаружение и переключение;
8. Beken: чистый рестарт после Apply (11-11 шёл через `malloc failed … async_tcp` + WDT), Matter-сопряжение, W1, W3, W7;
9. ESP32: W4, netwatch (`LWIP_STATS` IDF), Matter + WebUI по памяти (11-11 `bad_alloc` в `wsSend`; теперь запас кучи
   перед отправкой WS на всех чипах);
10. белый экран WebUI, когда WebSocket не подключился («Connecting…», Retry).

## 7. Действующие решения владельца

| Решение | Когда |
|---|---|
| Модули включаются и выключаются только Apply с перезагрузкой; на ходу ничего не запускается | 11-10 |
| Постоянного резерва RAM нет: статика модулей по пулам (Beken TCM, ESP8266 хвост IRAM, ESP32 — нет), остальное — блок кучи | 11-10, 11-11 |
| Файлы релиза: initial по проводу (`espurna-esp8266.bin`, `espurna-esp32.bin`, `espurna-beken-tuya.uf2`, `espurna-beken-quectel.uf2`) — голова + `captive`; `espurna-<чип>.ota` — голова + все модули; полных образов нет | 11-10 |
| WebUI — модуль `webui` на всех трёх чипах, в initial не входит; страница головы одна на все чипы | 11-11 |
| Matter-tail удалён, Matter — модуль | 11-10 |
| migration image живёт всегда (миграция старой ESPurna на 1 МБ) | 11-10 |
| Сборок без `MODULES_SUPPORT` нет (решение №3 старого плана) | 11-10 |
| Пароль по умолчанию `fibonacci` остаётся | 11-10 |
| D2 (лимит соединений и порог heap) и D7 (чтение пина каждую мс, 80 %-фильтр CORIDOR12) оставлены; D3–D6, D8 — как upstream | 11-11 |
| Прошивку через прокси lion не грузить; nginx и проверку Origin не трогать (W6) | 11-11 |
| Платформа ESP32 `55.03.312`, Matter-библиотека Beken `libmatter-20261107-06fec34` + sha256 закреплены | 11-10 |
| Одна реализация TLS; хеши — SHA-256 из BearSSL; MD5 — последний пункт по сборке (U22) | 11-12 |
| HTTP-клиент и веб-сервер — свои, на lwIP; WebSocket и полный TLS — модули-расширения | 11-12 |
