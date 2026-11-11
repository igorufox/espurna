# План реализации: унификация архитектур ESP8266 / ESP32 / Beken

Составлен 2026-11-10 по ревью ветки `esp32-port` (HEAD `f200c857`) против `upstream/dev`.
Откуда взяты задачи и номера находок (A1, B7, C1, D4 …): [review-unify-plan.md](review-unify-plan.md).

Ревью делалось чтением кода. Места, помеченные там «вероятно», в этом плане начинаются с шага «воспроизвести на
стенде»; если не воспроизводится — задача закрывается записью об этом, без правки.

## 1. Правила работы

| Правило | Что значит |
|---|---|
| одно дерево | без клонов, копий, своих папок сборки, флагов-переключателей и вторых вариантов кода |
| три env | `esp8266`, `esp32`, `beken`; задача готова, когда собираются все три |
| сборки | только через сессию-сборщик, одна сборка за раз |
| стенд | ESP8266 — 10.0.5.0 (Sonoff Basic 1 МБ); ESP32 — 10.0.5.66; Beken — 10.0.5.139 |
| платы в стенах | не трогаются этим планом; раскатка — отдельно и по слову владельца |
| новый путь загрузки или записи флеш | сначала на стенде с подключённым проводом, потом по воздуху |
| один шаг — одна тема | задача переводит все три чипа сразу; дерево не остаётся несобираемым между шагами |
| коммиты | после каждой проверенной задачи, по слову владельца |

Проверки, общие для всех задач («базовая проверка»):

1. собираются три env; размер образа и статическая RAM не выросли без причины (числа до и после — в записи задачи);
2. плата загружается, входит в Wi-Fi, WebUI открывается, MQTT подключён;
3. OTA на новую сборку и обратно сохраняет настройки;
4. heap через 10 минут работы не ниже, чем до задачи.

## 2. Порядок и зависимости

| Этап | Что | Зависит от | Меняет поведение |
|---|---|---|---|
| 0 | находки, грозящие кирпичом | — | да (исправления) |
| 1 | одно правило для интерфейсов чипа | 0 | нет |
| 2 | дубли ESP32 ↔ Beken в `arch/rtos` | 1 | нет |
| 3 | одна модель потоков | 1, 2 | да (ESP32, Beken) |
| 4 | ветки по чипу из общих файлов | 1; 4.1 — после 3 | нет |
| 5 | модули | 0, 1, 3 | да |
| 6 | Matter | 3 | нет |
| 7 | сборка, скрипты, CI | — (можно параллельно с 1–4) | нет |
| 8 | WebUI | — (можно параллельно) | да (мелко) |
| 9 | NSPanel | 3 | нет |
| 10 | шум диффа | последним | нет |

Этапы 7 и 8 не пересекаются по файлам с 1–6 и могут идти отдельной сессией.

Размер задач: **S** — до ~50 строк правки, **M** — до ~300, **L** — больше или затрагивает три чипа глубоко.

## 3. Этап 0 — находки, грозящие кирпичом

| # | Задача | Файлы | Что сделать | Проверка | Размер |
|---|---|---|---|---|---|
| 0.1 | Границы пакета модуля (A1) | `modules.cpp` (`packageCheck`, `roundUp`, `inArea`), `modules_esp8266.cpp`, `arch/esp32/modules_esp32.cpp` | верхняя граница `image_size` и `ram_size` по размеру области модулей; `roundUp` и `offset + size` без переполнения; отказ до первой записи | host-тест на `packageCheck` с размерами `0`, `4`, максимум, `0xFFFFF004`; загрузка такого пакета на стенд — отказ, каталог цел | S |
| 0.2 | Safeboot: возврат в app0 раньше всего (A2) | `safeboot-app/src/main.cpp` | `bootApp()` до `nvs_flash_init()`; при ошибке NVS работать без настроек (AP), а не перезагружаться | стенд с проводом: испортить NVS, войти в safeboot — следующая загрузка в app0 | S |
| 0.3 | Beken: неизвестный загрузчик ничего не пишет (A3) | `arch/beken/ota_beken.cpp`, `layout_beken.cpp` | `bekenUf2Allowed()` отказывает при `Layout::Unknown` | host-проверка логики; на стенде известный загрузчик — OTA проходит | S |
| 0.4 | Beken: сектор записи крэша по раскладке (A4) | `arch/beken/rtcmem_beken.cpp`, `layout_beken.*` | сначала по дампам из `firmware-dumps/` проверить, что `0x1EB000` пуст на всех известных раскладках; адрес записи брать из `flashLayout()`, при Unknown не писать | вызвать крэш на стенде, сравнить дамп KV до и после | M |
| 0.5 | Beken: копия RF-калибровки (A5) | `arch/beken/system_beken.cpp`, `storage_eeprom_beken.cpp` | при `Calibration::Failed` хранилище не использует слот, перекрывающий `0x1E3000` | стенд: принудительный отказ копии, 20 записей настроек, сектор калибровки цел | M |
| 0.6 | Каталог модулей переживает обрыв питания (A6) | `modules.cpp`, `modules_core.h`, `scripts/module_full_image.py` | CRC каталога и две копии с номером поколения; чтение берёт последнюю целую; проверка границ и NUL в именах записей | host-тест формата; стенд: обрыв питания во время переключения модуля ×20 — модули на месте | M |
| 0.7 | Сторож ловит первый модуль (A7) | `modules.cpp` | взводить до `arch::place` и первого `entry()` | тестовый модуль с падением в `entry()` — после трёх загрузок модули выключены | S |
| 0.8 | DNS: длина имени в пределах пакета (B7) | `lib/DNSServer/src/DNSServer.cpp` | `memchr` с длиной пакета вместо `strchr` | пакет без нулевого байта на AP стенда — без чтения за буфером, сервер отвечает дальше | S |
| 0.9 | Сторож зависания не рвёт OTA (A10) | `arch/esp32/system_esp32.cpp`, `ota_driver_esp32.h` | воспроизвести: ArduinoOTA дольше 60 с; кормить сторож из `arduino_ota_progress()` | медленная ArduinoOTA на 10.0.5.66 доходит до конца | S |
| 0.10 | Safeboot: запрос установки не переживает неудачу (A12) | `safeboot-app/src/main.cpp`, `arch/esp32/safeboot_esp32.cpp` | снимать запрос из NVS при входе, до Wi-Fi | стенд: запрос, вход без сети, второй вход — установки нет | S |

A8 (релокация ESP32) закрывается задачей 5.3, A9 (таблица разделов) — предупреждением в WebUI в задаче 8.3,
A11 (Matter-tail) — задачей 7.5 после решения владельца.

## 4. Этап 1 — одно правило для интерфейсов чипа

Только переименования и переносы. Образы до и после должны совпадать по размеру.

| # | Задача | Файлы | Что сделать | Размер |
|---|---|---|---|---|
| 1.1 | Один выбор чипа | новый `espurna_arch.h`; `*_driver.h`, `espurna_chip.h`, `espurna_tasks.h`, `espurna_isr.h`; три ini | один заголовок определяет чип и подключает `arch/<chip>/…`; `.cpp` чипа отбирает `src_filter`, обёртки `#if defined(CHIP)` вокруг целых файлов убрать | M |
| 1.2 | Одно имя пространства | `system.h`, `system_rtos.h`, `wifi_driver.h`, `ntp_driver.h`, `mdns_driver.h`, `ota_driver.h`, `uart_driver.h`, `crash.h` и их реализации | везде `espurna::<подсистема>::chip`; `system::arch` и `time::arch` слить в `system::chip` | M |
| 1.3 | Одна форма | те же | свободные функции; inline-заголовок только там, где файл собирается как объект модуля (uart, mdns); `ota_driver_plain.h` влить в `ota_driver.h` как умолчания | M |
| 1.4 | Остатки старой схемы | `rtcmem_orch.h`, `wifi_orch.h`, `pgmspace_orch.h` и ~10 мест подключения | удалить, подключать `rtcmem.h`, `espurna_wifi.h`, `<pgmspace.h>` | S |
| 1.5 | Файлы на свои места | `wifi_driver_rtos.*` → `arch/rtos/`; `modules_esp8266.cpp`, `heap_reserve_esp8266.cpp` → `arch/esp8266/`; `modulesMigrateSettings` → storage ESP8266 | перенос без правок | S |
| 1.6 | Мелочи | `compat_rtos.h:56,126,127`; `system_rtos.h:135`; `MODULES_CORE`; `storage_eeprom.cpp:237`; `chip_esp32.cpp:84` | удалить дубли; `chip::setup()` вынести из-под `TERMINAL_SUPPORT` | S |

Готово, когда: поиск `_orch.h`, `system::arch`, `time::arch` по `code/espurna` пуст; базовая проверка пройдена.

## 5. Этап 2 — дубли ESP32 ↔ Beken в `arch/rtos`

| # | Задача | Файлы | Что сделать | Размер |
|---|---|---|---|---|
| 2.1 | Общий `RtcmemData` | `rtcmem_esp32.h`, `rtcmem_beken.h` → `rtcmem.h` | одна структура и макросы; заодно `gpio_ignore` 64 бита на ESP32 (D10) | S |
| 2.2 | GPIO | `gpio_esp32.cpp`, `gpio_beken.cpp` → `arch/rtos/gpio_rtos.cpp` | `HardwarePin`, `pin()`, `hardwareGpio()` общие; чипу — `gpio::chip::{valid, valid_output, pin_mode, write}` и таблица пинов | S |
| 2.3 | mDNS | `mdns_esp32.h`, `mdns_beken.h` → `arch/rtos/mdns_rtos.h` | чипу остаются `enable_arduino` и `query`; `add_txt()` в драйвер | S |
| 2.4 | Сторож зависания | `system_esp32.cpp:72-131`, `system_beken.cpp`, `rtcmem_beken.*` → `system_rtos.cpp` | сначала сверить две реализации; один сторож + `chip::on_stall()`; `RtcmemCrash::Stall` в `getResetInfo()` Beken | M |
| 2.5 | Chip id | `compat_esp32.cpp`, `compat_beken.cpp` → `compat_rtos.h` | через `system::chip::mac()` — серийник SSDP совпадает с `systemChipId()` | S |
| 2.6 | Мелкие дубли | `used()` UART → `uart.cpp`; `arduino_ota_begin` → `ota_driver.h`; `expired()` → `wifi_driver_rtos.h`; `crashRestart()` ×3 → `system::chip`; память сторожа модулей → `MODULES_NOINIT` | перенос | S |

Проверка сверх базовой: реле на GPIO32/33 ESP32 не щёлкает при перезагрузке; принудительное зависание `loop()` на
ESP32 и Beken даёт запись о stall и причину сброса в `info`.

## 6. Этап 3 — одна модель потоков

Главный этап. Закрывает C1–C9. На ESP8266 все новые обёртки компилируются в прямой вызов.

| # | Задача | Файлы | Что сделать | Проверка | Размер |
|---|---|---|---|---|---|
| 3.1 | Примитивы | `arch/rtos/tasks_rtos.h`, `arch/esp8266/tasks_esp8266.h`, `system_rtos.cpp:342-426` | `Mutex` (рекурсивный, FreeRTOS), `EventQueue<T>` (очередь FreeRTOS фиксированной длины), `runInLoop()` с лимитом длины и счётчиком отказов (C9) | host-тест на вариант ESP8266; стенд — базовая | M |
| 3.2 | События Wi-Fi через очередь (C1, C2) | `arch/rtos/wifi_driver_rtos.cpp`, `wifi.cpp` | событие целиком (тип, bssid, reason, время) в очередь из задачи SDK; `loop()` разбирает по порядку; отмена попытки — событие, автомат выходит из `WaitConnected`; таймаут на `WaitConnected` | стенд ESP32 и Beken: 50 циклов `wifiDisconnect()` посреди попытки; deauth с AP во время DHCP; смена AP; 12 ч без зависания автомата | L |
| 3.3 | Убрать atomics | `wifi_driver_rtos.cpp:70-81`, `system_rtos.cpp:347`, `system_rtos.h:151`, `tasks_rtos.h:38,98`, `telnet.cpp:1436`, `compat_beken.cpp:31-107` | на примитивы 3.1; свой libatomic Beken удалить | Beken собирается без `__atomic_*`; базовая | M |
| 3.4 | Обработчики web через одну обёртку (C3, C4, C8) | `ws.cpp`, `web.cpp`, `captive.cpp`, `ota_web.cpp`, `ota_basicweb.cpp`, `api.cpp` | обёртка «выполнить под блокировкой loop или ответить 503»; `locked()` проверяется внутри неё; `onDisconnect` / `onFile` OTA под блокировкой; состояние загрузки (`pending`, `owner`) привязать к запросу; `_wsOutputPending` не держит указатель на клиента между вызовами | стенд: 100 сессий WS, загрузка OTA с обрывом ×10, две загрузки одновременно — вторая получает отказ | L |
| 3.5 | Каталог модулей под мьютексом (C5) | `modules.cpp` | чтение и запись каталога под `Mutex`; `Directory` не на стеке задачи таймера | переключение модуля из WebUI во время работы сторожа ×50 | S |
| 3.6 | Matter (C6) | `arch/beken/matter_beken.cpp`, `arch/esp32/matter_esp32.cpp`, `matter.cpp` | колбэки сети — через `EventQueue`; `matter.cpp` берёт `Mutex` из слоя чипа; DAC-провайдер ESP32 ставить под `LockChipStack` до `Matter.begin()` | сопряжение на обоих чипах; WebUI остаётся подключённым во время pairing | M |
| 3.7 | NTP на Beken (C7) | `arch/rtos/ntp_driver_rtos.cpp` | `init / stop / server / dhcp` через `tcpip_callback`, как уже сделан рестарт | `NTP.SYNC` ×100 и reload настроек на 10.0.5.139 | S |
| 3.8 | MQTT: остановка клиента | `mqtt.cpp:~1104,~2280` | воспроизвести; при таймауте и смене настроек останавливать `AsyncMqttClient`, новый `connect()` — после закрытия pcb | 270-символьный сервер → смена на рабочий, подключение без перезагрузки на трёх чипах | M |

Готово, когда: поиск `std::atomic` по `code/espurna` даёт только обоснованные места (список в записи этапа);
soak 24 ч на 10.0.5.66 и 10.0.5.139 с WebUI, MQTT и переключениями AP без зависаний и без роста heap.

## 7. Этап 4 — ветки по чипу из общих файлов

| # | Задача | Файлы | Что сделать | Размер |
|---|---|---|---|---|
| 4.1 | Транспорт telnet | `telnet.cpp:303-1551` → `telnet_transport.h`, `arch/esp8266/telnet_transport_esp8266.cpp`, `arch/rtos/telnet_transport_rtos.cpp` | в `telnet.cpp` остаётся протокол и терминал; транспорт — открыть, принять, записать, закрыть | L |
| 4.2 | PWM | `pwm.cpp:123-378` → `pwm_driver.h`, `arch/<chip>/pwm_<chip>.cpp` | LEDC и LibreTiny за драйвер | M |
| 4.3 | Хуки Wi-Fi Beken | `wifi_driver_rtos.cpp:167-349` | `chip::map_reason()`, `chip::attempt_event()`, `chip::scan_finished()` вместо рантайм-веток | S |
| 4.4 | NTP | `ntp_driver_rtos.cpp` | `ntp::chip::{watch_setup, halt, running, configure}` вместо `ESPURNA_NTP_IDF_SNTP` | S |
| 4.5 | Типы и строки | `types.h:396-465`, `wifi_driver.h:33-61`, `espurna_wifi.h:13-50` | `operator+`, `PROGMEM_STRING` → `compat_rtos.h`; типы SDK → `arch/<chip>/wifi_types_<chip>.h` | S |
| 4.6 | UART | `uart_driver.h:50-95`, `uart_esp32.h` | пары пинов ESP8266 → `uart_esp8266.h`; на ESP32 аппаратный порт на любых пинах | M |
| 4.7 | АЦП | `sensors/AnalogSensor.h`, `NTCSensor.h`, `ws.cpp` (VCC) | новый `adc::chip::{bits, millivolts, attenuation, vcc}` | M |
| 4.8 | Заголовок TCP | `api.cpp`, `mqtt.cpp`, `ota_asynctcp.cpp`, `thingspeak.cpp` | один `espurna_asynctcp.h` | S |
| 4.9 | Разовые вызовы SDK | `web.cpp` (`Hash.h`), `garland.cpp` (WDT), `terminal.cpp` (флеш), `debug.cpp` (`uart_set_debug`), `rfm69.cpp` (HSPI-пины), `config/all.h` | в `system::chip`, `storage::chip`, `gpio::chip`, `uart::chip` | S |
| 4.10 | Возможности вместо веток | `ota.cpp`, `ota_web.cpp`, `ota_fetch.cpp`; `config/general.h`, `sensors.h`, `dependencies.h` | «умею fetch» — константа драйвера OTA; три `*_GENERIC` → один признак; `WEB_GUARD_*_DEFAULT` → `system::chip` | M |

Готово, когда: поиск `ESP32|ESP8266|LIBRETINY|ARDUINO_ARCH` в `#if` по `code/espurna` вне `arch/`, `config/` и
заголовков выбора чипа пуст либо каждое оставшееся место записано с причиной. Сейчас таких файлов около 40.

Проверка сверх базовой: telnet на трёх чипах (вход, команда, обрыв клиента ×20); PWM-свет на ESP32 и Beken;
аналоговый датчик и VCC на трёх чипах.

## 8. Этап 5 — модули

| # | Задача | Файлы | Что сделать | Проверка | Размер |
|---|---|---|---|---|---|
| 5.1 | API из одной таблицы | `modules_api_esp8266.cpp` → `modules_api.cpp`, `modules_exports*.txt` | X-macro таблица даёт enum, форвардер и `modulesProvide`; один файл экспортов с общей частью и добавками чипа | образы модулей байт в байт те же, что до задачи, либо отличаются только порядком таблицы | L |
| 5.2 | Общая установка | `modules.cpp`, три бэкенда | `placeUpload()`, `fillEntry()`, `relocateWords()` в ядре; бэкенду — raw read / erase / write и адресные пространства | установка, замена, удаление каждого модуля на трёх чипах | M |
| 5.3 | Статическая арена ESP32 (A8) | `arch/esp32/modules_esp32.cpp`, `scripts/esp32_module.ld` | арена фиксированного адреса, релокация один раз при установке; нужно решение владельца №2 | обрыв питания во время установки ×20 — модуль либо старый, либо новый; «starting» соответствует факту | L |
| 5.4 | Учёт состояния и RAM | `modules.cpp:1091-1103,1933,626-656`, `modules_esp8266.cpp:565-569` | `status[]` по индексу записи каталога; RAM работающего удалённого модуля занята до перезагрузки; `runs()` ESP8266 — границы своей записи | удалить работающий модуль и сразу поставить другой — без падения | M |
| 5.5 | Причина рестарта на Beken | `arch/beken/modules_beken.cpp:1011` | обрыв питания не считается падением | три выключения питания в первые 60 с — модули включены | S |
| 5.6 | Скрипты | `scripts/beken_full_image.py`, `*_module.ld`, `module_sections.ld` | `beken_full_image` — наследник `FullImage`; общие фрагменты `.ld`; сверка размеров структур C++ ↔ Python при сборке | full-образы трёх чипов загружаются на стенде | M |
| 5.7 | Одинаковый набор | `module_test.cpp`, `config/head.h` | тестовый модуль на всех чипах; `MODULES_RAM_ARENA` в `head.h`; символы `__mod_code_start/end` в трёх `.ld` | — | S |
| 5.8 | JSON `/modules` | `modules.cpp:1486-1496` | экранирование строк | имя с кавычкой не ломает панель | S |

## 9. Этап 6 — Matter

| # | Задача | Что сделать | Размер |
|---|---|---|---|
| 6.1 | Один C-API | `matter_app_*`: start, plugs, set_onoff, codes, fabric_count / info / remove, open_window, ping и колбэки — одинаковый на ESP32 и Beken; `matter.cpp` знает только его | M |
| 6.2 | Общий исходник поверх CHIP | имена мостовых розеток, список и удаление fabric, окно — один файл; на Beken это сейчас внутри внешней библиотеки `igorufox/matter-bk7231n`, её исходник в ревью не попал — сначала прочитать | L |
| 6.3 | Идентичность | `matter_identity_esp32.cpp` и `espurna-id` Beken — одна реализация | M |
| 6.4 | Мелочи | `start()` ESP32 возвращает реальный результат; `fabrics()` не держит `LockChipStack` из loop дольше попытки; `quiet()` не включает MQTT, выключенный во время pairing; проверка `offset + length` без переполнения (B9); `lt_wdt_enable` — константа из `system_beken` | S |

Проверка: сопряжение, управление, удаление fabric на обоих чипах; порядок выпуска «сначала библиотека, потом
espurna» сохраняется.

## 10. Этап 7 — сборка, скрипты, CI

| # | Задача | Файлы | Что сделать | Размер |
|---|---|---|---|---|
| 7.1 | Одна версия сборки | `pio_main.py`, `beken_version.py`, `platformio_esp32.ini` | один `post:`-скрипт на три чипа; `APP_VERSION` ESP32 с git-ревизией | S |
| 7.2 | Сверка ini | `ci_ini_lint.py`, три ini, `safeboot-app/platformio.ini`, `mini-app` | версии библиотек и `lib_ignore` равны на трёх чипах (в Beken ini `extra_configs` нельзя — поэтому проверка, а не общая секция); лишний `-DARDUINO_ARCH_ESP32`; флаги `ESPURNA_IMAGE` в одном виде | S |
| 7.3 | Общий HTTP-клиент | `mini-app/src/main.cpp:538-1151`, `ota_fetch.cpp` | один исходник в обе сборки | M |
| 7.4 | Общие заголовки | читатель Embedis (3 копии), разбор таблицы разделов (4 места), `validUrl` / `validSha256`, `crc32` Beken | по одному заголовку; `Entries[]` генерировать из csv | M |
| 7.5 | Matter-tail | `beken_matter_tail.py`, `beken_link_tail.py`, ветки `MATTER_TAIL_SUPPORT` | по решению владельца №1: удалить (~630 строк) либо подключить в `platformio_beken.ini` и выпускать `espurna-beken-matter.bin` | M |
| 7.6 | Мёртвое | `platformio_ota.ini` в `extra_configs`, таблица старых core, `platform_elf2bin_3_20704_0.py`, `memanalyzer.py`, `code/safeboot/` и кандидат в `esp32_merge.py:132-138` | удалить; merged-образ без safeboot — ошибка сборки, а не молчание | S |
| 7.7 | CI: права и версии (B6) | `.github/workflows/esp32.yml`, ini | `contents: write` только на шаге публикации; токен только сборке Beken, на чтение; платформа и Matter-библиотека приколоты к версии и хэшу | S |
| 7.8 | CI: тесты | `esp32.yml`; `release.yml`, `release-preview.yml`, `push.yml`, `generate_release_sh.py`, `test_build.py` | host unit-тесты, vitest WebUI и `ci_ini_lint.py` — шаги `esp32.yml`; наследие upstream удалить | M |
| 7.9 | Safeboot и mini: вход | `safeboot-app/src/main.cpp`, `mini-app/src/main.cpp` | **решено 2026-11-10**: пароль по умолчанию `fibonacci` остаётся («кто может узнать MAC устройства в стене»); буфер base64 по длине пароля; POST с проверкой Origin (B5, CSRF) | M |

## 11. Этап 8 — WebUI

| # | Задача | Файлы | Что сделать | Размер |
|---|---|---|---|---|
| 8.1 | `net.mjs` | `fwinstall.mjs`, `templates.mjs`, `modules.mjs`, `tasmota.mjs` | `fetchJson`, `post`, `wait`, `safeHref` (только http/https — B3 и ссылки шаблонов); `TASMOTA_LINK + entry.link` через `URL` | M |
| 8.2 | `applyHardware.mjs` | `templates.mjs`, `tasmota.mjs` | один путь применения шаблона; `del` содержит старые ключи реле, кнопок, LED; предел длины значений | M |
| 8.3 | Установка прошивки | `fwinstall.mjs` | проверка `sha256` скачанного файла (B4); «установлено» только после сверки версии; таблица стратегий по чипу вместо `if`; запрос `/ota/fetch` — после успешной загрузки mini; предупреждение о питании перед сменой разметки (A9) | M |
| 8.4 | Один разбор манифеста | `fwinstall.mjs`, `modules.mjs:1063-1077` | общая функция | S |
| 8.5 | Тесты | `spec/` | на `safeHref`, `applyHardware`, `installEsp32` / `waitFor*`, `qrcode.mjs` | M |
| 8.6 | Сервер: `/ota/fetch` (B2) | `ota_web.cpp`, `ota_basicweb.cpp` | проверка Origin на POST, меняющих прошивку; `systemHostname()` экранировать | S |

## 12. Этап 9 — NSPanel

| # | Задача | Что сделать | Размер |
|---|---|---|---|
| 9.1 | Находки | `weekday()` с проверкой месяца; предел размера ответа погоды; `weather::last_error` под `Mutex` (B8) | S |
| 9.2 | Раздел файла | `nspanel.cpp` → `nspanel_config.cpp` (с host-тестом разбора), `_storage`, `_net`, `_link`, `_flash`, `_web`; общее состояние — через заголовок с явными функциями | L |
| 9.3 | Драйверы | `nspanel_lovelace.h`, `nspanel_ohpanel.h` → отдельные `.cpp` с интерфейсом `Driver` вместо восьми `switch (link::active)` | M |
| 9.4 | `nspanel_port` | uart, storage, http_get, heap, buzzer, task за тонкий слой — только если панель нужна на втором чипе | M |

Проверка: панель на стенде показывает те же экраны; прошивка TFT проходит; зеркало экрана в WebUI работает.

## 13. Этап 10 — шум диффа

| # | Задача | Что сделать |
|---|---|---|
| 10.1 | Пустые строки | вернуть конец файла как в upstream в ~134 файлах `code/espurna` (и ~100 затронутых содержательно) — один отдельный коммит, без других правок |
| 10.2 | Сверка | `git diff --stat upstream/dev...HEAD` не содержит файлов с правкой в одну строку без причины |

## 14. Поведение ESP8266 против upstream — отдельный список

Не часть унификации; каждая строка ждёт решения владельца №5 «оставить / вернуть».

| # | Что | Где | Если возвращать |
|---|---|---|---|
| D1 | captive-портал только в модуле | `web.cpp`, `captive.cpp` | зависит от решения №3 |
| D2 | лимит 3 соединения и порог heap 5120 | `web_guard.cpp:134-158` | не считать закрывающиеся pcb и ws; порог из стендовых замеров |
| D3 | очередь WS без таймаута | `ws.cpp:~1234` | таймаут головы очереди |
| D4 | `digitalPin` → `digitalPin0` | `sensors/DigitalSensor.h` | миграция ключа при загрузке |
| D5 | `ENCODER2` без `ENCODER1` | `encoder.cpp:~60` | `continue` вместо `break` |
| D6 | Switch с пустыми действиями; динамическая кнопка на старом пине | `button.cpp:1750,1868` | не подменять пустые действия; не создавать кнопку на занятом пине |
| D7 | чтение пина каждую миллисекунду | `DebounceEvent.cpp:124-130` | для analog и MCP23S08 — прежние два чтения |
| D8 | rc-switch без сравнения протокола | `rfbridge.cpp:~530` | настройка |

## 15. Решения владельца, которые блокируют задачи

| № | Вопрос | Блокирует |
|---|---|---|
| 1 | Matter-tail для загрузчика Quectel: удалить или вернуть в сборку | 7.5 — **решено 2026-11-10**: Matter-tail удалить, Matter — модуль, модули ставятся в основную часть или в OTA-раздел. В релизе 4 initial-образа (ESP8266, ESP32, Beken Tuya, Beken Quectel) и 3 полных (по одному на чип) |
| 2 | Арена модулей ESP32 статическая | 5.2–5.4 — **решено 2026-11-10 (заменяет «статическую арену»)**: постоянного резерва нет нигде. Модули включаются / выключаются в панели и применяются кнопкой «Применить» с перезагрузкой, на ходу ничего не запускается. По «Применить» ядро раскладывает статику включённых модулей: по убыванию размера, в первый пул с постоянным адресом, где влезает, остальное — куча; переписывает во флеше только тех, у кого сдвинулся адрес (сначала новая копия, потом переключение — A8); пишет каталог, перезагрузка. Пулы чипа: Beken — TCM (быстрее, свободна), ESP8266 — хвост IRAM, ESP32 — RTC FAST + хвост IRAM (размеры измерить), затем куча. Блок в куче берётся первым при загрузке; если адрес сдвинулся несколько загрузок подряд — модули выключаются с причиной (без перезаписи флеша по кругу) |
| 3 | Нужны ли сборки без `MODULES_SUPPORT` | D1, часть 4.10 |
| 4 | До какого момента живёт mini-app | 7.3 — **решено 2026-11-10**: mini живёт всегда (миграция старой ESPurna на 1 МБ, где голова в свободное место не влезает); значит HTTP-клиент mini и `ota_fetch.cpp` — один исходник (7.3 делается) |
| 5 | D2–D8: оставить новое поведение или вернуть | раздел 14 — **решено 2026-11-10**: пользователь этого не просил — всё D2–D8 вернуть как в upstream |

Остальные задачи решений не ждут.

### Стенд 2026-11-10, безопасные проверки (сборка 22:2x)

| Плата | Что | Итог |
|---|---|---|
| 10.0.5.139 Beken (Tuya) | `MODULES.OFF`, OTA `espurna-beken.uf2` (ныне `-tuya`) | OK: «Reboot after a successful OTA update», system OK, `keys` до = после, Wi-Fi foxy −31, heap 43272, TCM 58296 / 60928 |
| 10.0.5.139 | OTA `espurna-beken-quectel.uf2` | отказ HTTP 400 «This .uf2 is for the Quectel bootloader…», прошивка прежняя |
| 10.0.5.139 | модуль mqtt (`/modules/upload`) | running; сервера MQTT на стенде нет («No server configured»); heap 42392 через 31 мин |
| 10.0.5.66 ESP32 | `/safeboot/install` нового safeboot (A2 / A12) | OK, sha256 94f91c90… совпал |
| 10.0.5.66 | `/safeboot/update` по URL + sha256 | OK за 48 с, «SHA-256 checked», счётчик загрузок 3 → 1, `keys` = (кроме `sbUrl`), heap 138336 |
| 10.0.5.66 | mqtt на ходу | «broken - no RAM arena» (арена загрузки только под captive); после перезагрузки — running, арена 2304 — подтверждает модель «Применить + перезагрузка» |
| 10.0.5.0 ESP8266 | — | после OTA головы 20:24 держала AP ESPURNA-074685: нестабильный режим — падение при установке captive из хвоста initial-образа. Причина — моя: каталог (16 записей ≈1 КБ) лежал на стеке в двух копиях в read/writeDirectory плюс у вызывающего → переполнение стека loop (4 КБ), `cont_check` в `loop_wrapper` (разобрано по ELF, повтор на сборке 22:31). Исправлено: одна статическая копия каталога, сверка записи кусками по 64 байта (+1036 байт статической RAM). Восстановлена по сети через её AP: ArduinoOTA головы `.bin.gz` (несжатая 584 КБ не помещается), затем initial с captive — OK, captive running со второй загрузки |
| 10.0.5.0 | «Применить»: `POST /modules/off` → `POST /modules/apply` (captive, mqtt из `.ota`) | OK: перезагрузка 28 с, запись 23 с, mqtt `0xee000` и captive `0xec000` подряд, оба running, heap 10888. Digest сервера сверяет путь без query — имена идут полем формы перед файлом |

| 10.0.5.139 Beken | «Применить» captive, mqtt, sensors, light из `espurna-beken.ota` | OK: запись 10 с, все running, heap 36584 |
| 10.0.5.66 ESP32 | то же из `espurna-esp32.ota` | 1-й раз: после записи (27 с) плата ~4 мин не открывала TCP 80 (ping был), затем «Reboot after changing stability counter», модули running; дамп ядра — старый (не от этой сборки). 2-й раз: OK за 14 с, все running. Не воспроизвелось — смотреть на длительном прогоне |

Сделано для схемы «Применить» (этап 1): `POST /modules/off`, `POST /modules/apply` (поле `names` перед файлом), `scripts/module_ota.py` + `ota_heads()` чипов → `espurna-<чип>.ota` в каждой сборке; CI выпускает `.ota` вместо `-modules.bin` / `-full.bin` (вид `install`, на все образы чипа); WebUI: галочки + «Apply» (`selectionAfter`, `applyNow`), `.ota` в панели MODULES и «From GitHub» (`otaParts`, `modulesFileOf`, `modulesFileUrl`: install > modules > full). Этап 2 (пулы TCM / IRAM / RTC + куча, без постоянного резерва) — следующий.

### Файлы релиза и установка модулей (решение владельца 2026-11-10, заменяет «4 initial + 3 full» и `-modules.bin` / `-full.bin`)

| Файл | Что в нём | Для чего |
|---|---|---|
| `espurna-esp8266.bin`, `espurna-esp32.bin`, `espurna-beken-tuya.uf2`, `espurna-beken-quectel.uf2` | голова + модуль captive (DNS), включён | первая прошивка по проводу |
| `espurna-<чип>.ota` (Beken — один на оба загрузчика, обе головы внутри) | голова + пакеты всех модулей | обновление головы, установка модулей; расширение `.ota` — чтобы его не прошили во флеш по ошибке |

Панель MODULES: отметить модули → «Применить» → область модулей стирается и пишется заново, выбранные подряд (без
фрагментации), статика по пулам (TCM / IRAM / RTC, потом куча), каталог последним, перезагрузка; captive из initial
переписывается вместе со всеми. Голова и модули делят место (ESP8266, Beken — OTA-раздел; ESP32 — хвост app0): браузер
отдаёт `.ota` дважды — голова, перезагрузка, модули. Полных образов нет.

## 16. Учёт выполнения

Статус каждой задачи ведётся в этом файле: «—», «в работе», «готово (коммит, плата, числа)».

Журнал (2026-11-10, сессия «Unify implementation plan»; «код» = правка внесена, стенд ещё не проверял):

| # | Статус | Что сделано |
|---|---|---|
| 0.1 | код, host-проверка | `modules_package.h` (roundUp без переполнения, `inside()`, `sizesCheck()` по размеру области и 256 КиБ RAM), `packageCheck`/`inArea` на них; тест `test/unit/src/modules` |
| 0.2 | код | safeboot: `bootApp()` первым, без NVS — AP без настроек, Wi-Fi без NVS |
| 0.3 | код | `bekenUf2Allowed()` отказывает при `Layout::Unknown` |
| 0.4 | код | дампы: `0x1EB000` пуст на CB3S, CBU ×2, слот-плате, square QFN, XH-CB2S ×2; адрес — `FlashLayout::crash_offset`, у загрузчика 3.00 (MOES, дампа нет) — 0, записи нет |
| 0.5 | код | `bekenCalibrationKeep()`: пока калибровка не на месте (Unchecked / Failed), слот настроек над `0x1E3000` не стирается |
| 0.6 | код | каталог: две копии (последний сектор области и под ним), `generation` + CRC-32, проверка NUL в именах; старый формат читается и пишется на месте, пока его слот занимает второй сектор; скрипты full-образов пишут новый формат; Beken: метки — `writeDirectoryPrimary()`, XIP-тест на третьем снизу секторе |
| 0.7 | код | `guardArm()` в `run()` перед конструкторами и `entry()`, в `placeNow()` перед `arch::place` |
| 0.8 | код | `memchr` по длине пакета |
| 0.9 | код | `arduino_ota_progress()` ESP32 считает проход для сторожа зависания |
| 0.10 | код | запрос установки снимается из NVS при входе, до Wi-Fi; без сети — `failed: no network` |
| 7.7 | код | `contents: read` на workflow, `write` только release/promote; токен только сборке Beken. Решение владельца 2026-11-10 «закрепи»: платформа ESP32 `55.03.312` (platformio_esp32.ini, safeboot-app), Matter-библиотека `libmatter-20261107-06fec34` + sha256 (platformio_beken.ini); новый релиз библиотеки — правкой двух строк |
| 7.8 | код | `push.yml`, `release.yml`, `release-preview.yml`, `generate_release_sh.py`, `test_build.py` удалены; проверка `arduino.h` — шаг `ini-lint` в `esp32.yml` |
| — | код | модули ESP32/Beken: OneWire, hlw8012 (на Beken — запасной режим библиотеки через pinMode/digitalRead), my92xx (`-Dos_delay_us=delayMicroseconds` в `custom_module_flags` Beken), `Wire`; `light.cpp`: модель MY92xx разбирается без `Enumeration` (у LibreTiny `-fshort-enums`, enum библиотеки — 1 байт) |
| 2.1–2.2 | — (схема; черновики вне дерева удалены, делать сразу в дереве) | `rtcmem.h` общий (`gpio_ignore_lo/hi`: старое 32-битное поле = младшее слово, magic тот же — состояние реле переживает обновление); `arch/rtos/gpio_rtos.*` + `gpio::chip::{Pins, valid, valid_output, pin_mode, write}`; удержание GPIO32..39 на ESP32 (D10) |
| 1.2 | схема | `system::arch` → `system::chip`; нижние хуки RTOS (`setup/loop/reboot/enable_loop_wdt/feed_loop_wdt`) → `system::chip::sdk`; `time::arch` → `time::chip`; `arduino_ota_progress()` → `sdk::loop()` |
| 2.4 | сверка | ESP32: задача считает проходы `loop()`, 60 с → запись в RTC + `abort()`; Beken: задача следит за кормлением аппаратного WDT (обёртки `lt_wdt_*`), за 1,5 с до него — запись задач во флеш. Общее: задача, срок, `chip::on_stall()` |
| 7.2 | частично | `ci_ini_lint.py`: версии одной библиотеки в `lib_deps` / `shared_lib_deps` трёх ini равны (git — по ref); ArduinoJson ESP8266 `^5.13.4` → `5.13.4` (установлена та же). Не сделано: лишний `-DARDUINO_ARCH_ESP32`, вид `ESPURNA_IMAGE`, safeboot-app / mini-app |
| 7.5 | код | Matter-tail удалён: ветки `MATTER_TAIL_SUPPORT` (matter_beken.cpp, matter.cpp, espurna_matter.h), `matterSkip` / `matterRebootForUpload`, контейнер `ESPBEKEN` (ota_beken.cpp переписан, ota_beken.h удалён), `beken_matter_tail.py`, `beken_link_tail.py`; общий OTA без `own_*` / `upload_blocked` / «OK, again» — у драйвера одна `upload_check()`. Beken: второй initial-образ `espurna-beken-quectel.uf2` (`scripts/beken_quectel_image.py`, из того же `raw_firmware.bin`); у UF2 своя таблица FAL (`FAL_PTABLE`), LibreTiny пишет `download` по ней — плата сверяет `download` файла с раскладкой загрузчика и отказывает чужому (проверено на файлах: Tuya `0x12A000+0xA6000`, Quectel `0x286000+0x15A000`); `chip::image()` — `fwImage` «beken-quectel» на Quectel; WebUI без шагов Matter-кода и панели «Matter code»; CI: в релизе `espurna-beken-quectel.uf2` и `espurna-beken-full.bin` (полного Beken раньше в релизе не было), `beken-matter` → `beken` в списке старых образов |
| D2–D8 | код | решение владельца — как upstream. D2: сторож соединений на ESP8266 выключен умолчаниями (все пороги 0; ESP32 / Beken — как были, там их падения с Matter); D3: проверки heap перед отправкой WS и сброс сообщений убраны (таймаут очереди 10 с есть и в upstream); D4: ключи `digitalPin` / `digitalPinMode` / `digitalDefState` без индекса переносятся в `…0` при загрузке (откатывать чтение без индекса нельзя — это ошибка upstream, все датчики на одном пине); D5: `continue`; D6: Switch с пустыми действиями ничего не делает, динамическая кнопка `relayBtnGpio` не создаётся на занятом пине (сама `relayBtnGpio` — функция форка с полем в WebUI, оставлена); D7: для `AnalogPin` / `McpGpioPin` два чтения, как в upstream; для GPIO 80%-фильтр остался — это исправление CORIDOR12 (9875c609), раскатанное на плату в стене; D8: протокол сравнивается, `rfbAnyProto 1` — без него |
| сборка 21:3x | — | esp8266 SUCCESS (RAM 43256, Flash 579883); beken SUCCESS — `espurna-beken-tuya.uf2`, `espurna-beken-quectel.uf2`, `-modules.bin`, `-full.bin` (Matter 480 КБ в full не входит — только в файле модулей); esp32 — голова RAM 54552 / Flash 1443784, модули: sensors не линкуется (`I2C0`/`I2C1` — добавлены в экспорт, пересборка). Beken: на BK72xx у LibreTiny нет тел `noInterrupts()/interrupts()` и `pulseIn()` — сделаны в `compat_beken.cpp` |
| 5.3 | данные | RAM модулей ESP32: matter 61564, nspanel 5240, mdns 2088, mqtt 1992, rf 944, light 500, thingspeak 272, alexa 232, homeassistant 216, scheduler 152, domoticz 120, captive 108, test 72 (sensors — после линковки); без Matter ≈ 12 КБ. Beken: matter 36780, все вместе 41704 при арене 37888 (TCM) |
| 7.6 | код | удалены `platform_elf2bin_3_20704_0.py` (платформа 4.2 = 3.30102), `memanalyzer.py`, `platformio_ota.ini` из `extra_configs`; merged ESP32 без safeboot — ошибка сборки (кандидат `code/safeboot/` убран; сама папка не в git, не трогал) |
| 8.1 | код | `html/src/net.mjs`: `wait`, `post`, `fetchJson`, `safeHref`; ссылки релизов и шаблонов через `safeHref` (B3) |
| 8.2 | код | `applyHardware.mjs`: ключи железа там, `del` — ключи relay/btn/led 0..15 не из шаблона, значения длиннее 64 — нет; шаблоны и импорт Tasmota через него |
| 8.3 | код | `sha256` скачанного образа (B4, свой SHA-256: WebUI по http, `crypto.subtle` нет); предупреждение о питании перед сменой разметки (A9); «installed» только после сверки коммита в `app_version` после перезагрузки (`installOutcome`); `INSTALL_STRATEGIES` по чипу. Порядок `/ota/fetch` перед загрузкой mini оставлен: mini читает запрос при старте, после загрузки его поставить некуда |
| 8.4 | код | `modulesFileUrl()` в `fwinstall.mjs`, панель модулей на нём |
| 8.5 | частично | `spec/net.spec.mjs` (safeHref, sha256, modulesFileUrl, hardwareMessage); vitest 137/137, eslint чист |
| RAM-пулы (11-11) | код + стенды ESP8266/Beken | Фиксированных арен нет ни на одном чипе. `arch::pools()`: пул с фиксированным адресом (ESP8266 — хвост IRAM после кода головы ≈19,4 КБ, код модулей с начала, 32-битный; Beken — свободная data TCM от `_tcmbss_end` до `_espurna_tcm_end` ≈39,5 КБ; ESP32 — нет: RTC FAST видна только PRO CPU) + один блок кучи (`modulesRamSetup()` сразу после `eepromSetup()`, размер по каталогу, без модулей — проба адреса). Apply (`applyPlan`) до стирания каталога: флеш всех модулей вместе (`arch::slotFor`), IRAM-код, затем статика от крупных к мелким в пул, остальное в кучу; флаг `ModuleByteRam` в EMC2 (`custom_module_byte_ram = light rf` на ESP8266) не пускает в 32-битный пул. ESP8266/Beken: блок кучи сдвинулся — модуль не стартует «Apply again»; ESP32 — релокация по картам. Запас кучи головы: ESP8266 4 КБ от `heap_loop`, Beken 8 КБ от наибольшего блока (`lt_heap_get_max_alloc` на Beken = заглушка 0 → замер делением через malloc), ESP32 32 КБ. Панель: статика / куча на старте модулей / свободно, полоса пула «IRAM tail» / «TCM», выбор DRAM/IRAM убран, кнопка Apply, confirm о закрытом сокете во время Apply не показывается (он вешал вкладку). |
| стенды 11-11 | — | 10.0.5.58 ESP8266: mqtt+sensors+captive в хвосте IRAM (6,5/19,4 КБ, код sensors 1,1 КБ), light в куче 1,5 КБ, heap_loop 23 КБ после загрузки (было 18,7 с ареной). 10.0.5.139 Beken: sensors/mqtt/light/captive в TCM 2,9 КБ; matter+light+captive в TCM 37,3/39,5 КБ, всё running, куча 41,5 КБ; matter + 4 модуля не влезают по флешу (660 КБ) — Apply отказывает заранее. Открыто: 10.0.5.0 (ESP8266) пропал через 46 с после OTA сборки без исправления чтения каталога и не вернулся; 10.0.5.66 (ESP32) не вышел из safeboot-обновления (на ПК висел второй http.server на 16065 с другой папкой); Beken после рестарта от Apply с Matter один раз работал без сети до передёргивания питания, не повторилось. |
| 11-11 позже | код + стенды | Apply проверяет флеш всех выбранных модулей вместе ДО стирания каталога (`arch::slotFor`; Beken: matter + 4 модуля не влезают в 660 КБ — отказ без порчи каталога). Beken: `lt_heap_get_max_alloc()` = заглушка 0 → наибольший блок кучи замеряется делением через `malloc`; запас головы 8 КБ. ESP8266: запас кучи считается от `heap_loop` (4 КБ), не от кучи внутри запроса загрузки. `modulesRamSetup()` читает каталог в свою копию (раньше вторая копия затирала первую в `scratch`). |
| mdns ↔ matter | код + Beken | Один mDNS-ответчик на чип: с Matter он у Matter. Beken: респондер LibreTiny (модуль mdns) занимал 5353 → `Failed to initialize advertiser: 3000008` (lwIP ERR_USE), сопряжение висело на первом шаге. ESP32: оба на компоненте IDF mdns, второй `mdns_init()` получает отказ (не проверено на плате). `modulesEnabled(name)` в голове (экспорт ESP32/Beken); модуль mdns на ESP32/Beken не стартует при включённом matter (`[MDNS] Off: the matter module answers mDNS on this chip`, проверено на 10.0.5.139). Панель: `conflictsOf` / `blockedBy` — галочка matter снимает mdns, переключатель mdns заблокирован с пояснением (ESP8266 без Matter — без ограничений). С Matter имя `ESPURNA-xxxx.local` не отвечает (у Matter свой hostname из MAC), заходить по IP. |
| WebUI / страница головы | код | Описание модуля captive (DNS своей точки доступа, к mdns отношения не имеет, зависимостей между ними нет). Страница головы ESP8266 (`modules_esp8266.cpp`): относительные пути (`modules`, `modules/file`, …) — через прокси `igoru.us.to/espurna/<ip>/` `/modules` уходил мимо платы ("Unexpected token '<'"). vitest 142/142. |
| ревью 11-11 (review-open-items.md) | код + стенды | Исправлено настоящее: 3.3 Beken — запас кучи по свободной куче, без пробных `malloc` (каждый отказ писал «malloc failed … async_tcp» и кончался WDT; после правки записей нет); 3.4 ESP8266 — `Directory` / `Spaces` в веб-обработчиках, `prepare`, `addEntry`, `removeEntry`, `setEnabled`, `offReason` — в куче (`boxed<T>()`), Apply на 10.0.5.58 прошёл под параллельным `GET /modules` каждые 2 с; 1.1 — `/modules/off` ставит отметку `off` вместо стирания каталога, Apply строит новый каталог в памяти и пишет его одним разом в конце (оборванный Apply оставляет старые модули выключенными, «Start the modules again» их возвращает); 2.2 — WebUI при 409 «MODULES.OFF first» сам перезапускает плату без модулей и заливает прошивку снова (`ota.mjs`, `fwinstall.mjs`); 4.7 — `modulesEnabled` не считает модули при отметке `off`; 4.8 — `modules/apply` от корня платы; 2.5 — `-full.bin` больше не пишутся (проверка «всё влезает» осталась). Matter: `matterAuto` по умолчанию 1 (модуль включён = Matter работает), форма MATTER с `form-settings` (Save видит «Start at boot»). Не делалось: 1.9, 1.10, 4.6, раздел 10. |
| восстановление 11-11 | код | Три рестарта за 60 с от Apply переводили плату в нестабильный режим (ESP8266 и ESP32 на регрессе): `CustomResetReason::Modules` (off, Apply, on, рестарт панели) сбрасывает счётчик, как OTA — проверено тремя рестартами подряд на 10.0.5.0. Ступени: (1) норма; (2) без модулей с сетью — отметка `off` после падений сторожа, когда загрузка после падения и следующее падение даст нестабильный режим (`SYSTEM_CHECK_MAX`), после трёх включений короче 10 с подряд (пары слов в последней четверти сектора каталога, без стирания), при выходе из нестабильного режима (`modulesOffMark` из `exit_unstable`); (3) своя точка доступа без модулей. `MODULES.OFF` в терминале ставит отметку, а не стирает каталог. Способы восстановления без паяльника — `docs/modules.md`. На стендах не проверено: три включения, выход из нестабильного режима с отметкой. |
| ESP32 + Matter: память | найдено, не исправлено | Регресс 11-11: с matter свободно 24–27 КБ, минимум 632 Б; через 15 с после Apply `bad_alloc` в `AsyncWebSocket::makeBuffer` ← `wsSend` → abort, перезагрузка. Статика Matter 61,5 КБ в блоке кучи. Проверка кучи в `ws.cpp` снята вместе с D3 (решение — как upstream, но для ESP8266); на ESP32/Beken её нет. Ревью 8.2 об этом предупреждало. |
| регресс 11-11 (потоки) | частично | ESP8266 10.0.5.0: Apply, Apply под опросом status, remove, три рестарта, 18,5 мин нагрузки — без падений (минимум кучи 3,6 КБ). ESP32 10.0.5.66: Apply без matter — OK; с matter `bad_alloc` в `wsSend` → вернул проверку кучи веб-сокета только на ESP32/Beken (`WS_HEAP_GUARD`), после — 3,5 мин с открытым веб-сокетом и Matter без падений; Matter через matter-server (узел 9, реле на endpoint 2/3) включает и выключает реле. Ступень 2 на ESP32: два падения подряд → в сети, модули off с причиной, `/modules/on` вернул все 7; на прежней сборке после выхода из нестабильного режима — в сети, отметка стоит. Исправлено по регрессу: отметка ступени 2 сбрасывает счётчик стабильности (`systemForceStable`), иначе второе падение уводило в точку доступа; панель выключает модуль без файла через `enable on=0` + `remove` (remove включённого = 400); тексты: оборванный Apply может испортить старые модули (их не запускает контрольная сумма, нужен повторный Apply), выключенный без файла модуль оставляет дыру во флеше. Не проверено: три включения выключателем; Beken — прогон идёт. |
| регресс | задача, не сделано | Прогнать на всех стендах (10.0.5.58 ESP8266, 10.0.5.139 Beken, 10.0.5.0 и 10.0.5.66 после провода): (1) Apply из WebUI напрямую и через прокси `igoru.us.to/espurna/<ip>/` — 11-11 через прокси на 10.0.5.58 плата оборвала `POST /modules/apply` («upstream prematurely closed», каталог уже стёрт → все модули слетели), напрямую скриптом тот же набор проходит; в записи падения Exception 29 с epc в литералах после `onStatus` (вероятно переполнение стека в SYS-контексте: `Directory` + `Spaces` на стеке в `onStatus`/`prepare`). (2) Сопряжение Matter с Алисой на Beken: 11-11 прошло только с третьей попытки (первые — Алиса висит на первом шаге; на плате остаётся старый fabric, окно сопряжения после него само не открывается). (3) Рестарт Beken после Apply идёт через `malloc failed … async_tcp` + WDT вместо чистого; WDT «not fed 8500 ms» при свободной куче 17,9 КБ после 36 мин с Matter. (4) Белый экран WebUI, когда websocket не подключился (11-11 добавлены «Connecting…» и Retry — проверить). (5) Выключение модулей без файла, попап Apply, mdns ↔ matter на ESP32. |

Сборка 2026-11-10 20:21–20:28 (сборщик, текущее дерево с этапом 0): голова собирается на трёх чипах — esp8266 RAM 43208 / Flash 579871 (SUCCESS целиком); esp32 RAM 54488 / Flash 1437780, beken Flash 899908 — FAILED на объектах модулей (было и до этапа 0): нет `OneWire.h`, `Wire.h` (ESP32, Beken), `my92xx.h` (Beken). Исправление (не собрано): OneWire, my92xx в `lib_deps` + `lib_ignore` + `custom_module_libs` ESP32 и Beken, `Wire` в `custom_module_libs` ESP32; у LibreTiny BK7231N нет Wire — `libs/SoftWire.h` (программный I2C), `i2c.cpp` берёт его, когда у фреймворка нет `<Wire.h>`.

| Этап | Задач | Готово |
|---|---|---|
| 0 | 10 | 0 |
| 1 | 6 | 0 |
| 2 | 6 | 0 |
| 3 | 8 | 0 |
| 4 | 10 | 0 |
| 5 | 8 | 0 |
| 6 | 4 | 0 |
| 7 | 9 | 0 |
| 8 | 6 | 0 |
| 9 | 4 | 0 |
| 10 | 2 | 0 |
