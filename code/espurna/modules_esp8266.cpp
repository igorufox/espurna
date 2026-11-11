/*

PROTOTYPE: the ESP8266 backend of the module loader (modules_core.h; the core is modules.cpp; docs/modules.md)

Flash: modules lie under the directory sector (the sector under the settings), placed from the top down; the head
grows from 0x0 up. A module runs from the flash mapped at 0x40200000. The IRAM after the head's code (48 KB IRAM, 16 KB
flash cache) is the fixed pool: the Apply puts the IRAM functions of the modules at its start, then their RAM, largest
first (32-bit access only: the core's non32xfer handler emulates bytes, not from an interrupt, so a module whose RAM
takes byte access from an interrupt goes to the heap block); the rest of the RAM in the core's heap block. Nothing kept
in reserve. The relocation is done as the package arrives (the core's word writer): every word gets the delta of its
kind (flash, RAM, IRAM) from the map before its chunk.

*/

#include "espurna.h"
#include "modules.h"

#if MODULES_SUPPORT && MODULES_CORE && defined(ESP8266)

#include "espurna_ota.h"
#include "storage_eeprom.h"
#include "modules_core.h"
#include "web.h"

#include <Updater.h>
#include <spi_flash.h>
#include <umm_malloc/umm_malloc.h>

#include <cstring>

// heap the head keeps for itself (Wi-Fi, the web server) when the Apply sizes the modules' heap block
#ifndef MODULES_HEAP_KEEP
#define MODULES_HEAP_KEEP 4096
#endif

namespace espurna {
namespace modules {
namespace {

constexpr uint32_t FlashMapped { 0x40200000 };
constexpr uint32_t DirectoryMagic { 0x35524445 }; // "EDR5": the shared entry, 16 entries, two copies with a CRC
constexpr uint32_t PackageVersion { 1 };          // kinds 1 flash, 2 RAM, 3 IRAM

// Entry::arch on the ESP8266
constexpr size_t IramOffset { 0 };  // its IRAM code in the IRAM tail
constexpr size_t IramSize { 1 };
constexpr size_t RamAddress { 2 };  // the address its RAM words were relocated for

// the IRAM after the head's code, with the 48 KB IRAM layout (flash cache 16 KB): module IRAM code and module RAM
// (RamSecondArena). 32-bit access only in hardware: the core's non32xfer handler emulates byte and half word access
// (slow, and not from an interrupt)
extern "C" void _text_end(void);

uint32_t iramTailStart() {
    return (reinterpret_cast<uint32_t>(&_text_end) + 32 + 7) & ~7u;
}

uint32_t iramTailSize() {
#if defined(MMU_IRAM_SIZE) && (MMU_IRAM_SIZE > 0x8000) && defined(NON32XFER_HANDLER)
    const uint32_t end = 0x40100000 + MMU_IRAM_SIZE;
    return (end > iramTailStart()) ? (end - iramTailStart()) : 0;
#else
    return 0;
#endif
}

// where the RAM of the module is this boot: the IRAM tail, or the core's heap block
uint32_t ramAddress(const Entry& entry) {
    const uint32_t offset = entry.ram_offset & ~RamSecondArena;
    return ((entry.ram_offset & RamSecondArena) ? iramTailStart() : heapBlockBase()) + offset;
}

// the sector under the settings: EEPROM_Rotate keeps them in its base sector and the pool under it (251 and 250 on a
// 1 MB chip, 8 sectors on 4 MB); the directory at 0xFA000 was in that pool and the settings writes erased it
uint32_t directoryOffset() {
    static uint32_t offset { 0 };
    if (!offset) {
        auto& eeprom = eepromInstance();
        offset = (eeprom.base() - eeprom.size()) * SectorSize;
    }

    return offset;
}

uint32_t headEnd() {
    return roundUp(ESP.getSketchSize(), SectorSize);
}

// --- the modules file in flash: 4 MB boards flashed with the full image carry it at 1 MB, behind the 1 MB layout
// the head runs in. The SDK refuses reads past the size in the image header (1 MB): the size is raised for the read

constexpr uint32_t OnboardFile { 0x100000 };
constexpr uint32_t ContainerMagic { 0x32434D45 }; // "EMC2"
constexpr size_t ContainerEntry { 72 };

bool readPastHeader(uint32_t address, uint32_t* data, size_t size) {
    const uint32_t real = ESP.getFlashChipRealSize();
    if ((address + size) > real) {
        return false;
    }

    const uint32_t saved = flashchip->chip_size;
    flashchip->chip_size = real;
    const bool ok = (spi_flash_read(address, data, size) == SPI_FLASH_RESULT_OK);
    flashchip->chip_size = saved;
    return ok;
}

uint32_t onboardFileSize() {
    static uint32_t size { 0xffffffff };
    if (size != 0xffffffff) {
        return size;
    }

    size = 0;
    if (ESP.getFlashChipRealSize() <= OnboardFile) {
        return size;
    }

    uint32_t head[2];
    if (!readPastHeader(OnboardFile, head, sizeof(head)) || (head[0] != ContainerMagic) || !head[1]
        || (head[1] > DirectoryEntries))
    {
        return size;
    }

    uint32_t end = 8 + (head[1] * ContainerEntry);
    for (uint32_t index = 0; index < head[1]; ++index) {
        uint32_t entry[ContainerEntry / 4];
        if (!readPastHeader(OnboardFile + 8 + (index * ContainerEntry), entry, sizeof(entry))) {
            return size;
        }
        // name[16], offset, size
        end = std::max(end, entry[4] + entry[5]);
    }

    if ((OnboardFile + end) <= ESP.getFlashChipRealSize()) {
        size = end;
    }

    return size;
}

// a modules file (EMC2) at `base`: its count of packages, 0 - none there
uint32_t containerCount(uint32_t base) {
    uint32_t head[2];
    if (!readPastHeader(base, head, sizeof(head)) || (head[0] != ContainerMagic) || !head[1]
        || (head[1] > DirectoryEntries))
    {
        return 0;
    }

    return head[1];
}

// one entry of it: name[16], offset (from `base`), size, ...
bool containerEntry(uint32_t base, uint32_t index, uint32_t (&entry)[ContainerEntry / 4]) {
    return readPastHeader(base + 8 + (index * ContainerEntry), entry, sizeof(entry));
}

// the package of a module in a modules file: its flash address and size
bool containerPackage(uint32_t base, const char* name, uint32_t& offset, uint32_t& size) {
    const uint32_t count = containerCount(base);
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t entry[ContainerEntry / 4];
        if (!containerEntry(base, index, entry)) {
            return false;
        }
        if (std::strncmp(reinterpret_cast<const char*>(entry), name, 16) == 0) {
            offset = base + entry[4];
            size = entry[5];
            return true;
        }
    }

    return false;
}

// the package of a module in the modules file the board carries (a 4 MB board with the full image)
bool onboardPackage(const char* name, uint32_t& offset, uint32_t& size) {
    return onboardFileSize() && containerPackage(OnboardFile, name, offset, size);
}

// --- the modules the head's image brings along (custom_modules_on: the captive portal) -----------------------------
// espurna-esp8266.bin is the head and, from the sector behind it, a modules file (EMC2) of them. A serial flash and an
// OTA (eboot writes the whole file from 0x0) both put that file at headEnd(), where the modules area starts. It is read
// once, when the board has no directory of this head (arch::imageModules); the space is free for modules after that

bool tailPackage(const char* name, uint32_t& offset, uint32_t& size) {
    return containerPackage(headEnd(), name, offset, size);
}

// --- settings of the old 4 MB layout -------------------------------------------------------------------------------
// EEPROM_Rotate keeps the settings in a pool of sectors under the SDK's 4 at the end of the flash the image header
// names: the 1 MB head reads sectors 251 and 250, an image of the 4 MB layout wrote 1019..1012 of the real chip. A
// sector is a copy when its sum of bytes (but the CRC and the counter at EepromRotateOffset) is the stored CRC; the
// newest copy has the highest counter (mod 256). Copied once: only when the head's own pool has no valid copy

uint16_t settingsCrc(const uint8_t* data) {
    uint16_t crc = 0;
    for (size_t address = 0; address < EepromSize; ++address) {
        if ((address >= EepromRotateOffset) && (address <= EepromRotateOffset + 2)) {
            continue;
        }
        crc += data[address];
    }
    return crc;
}

bool settingsValid(const uint8_t* data) {
    const uint16_t stored = (data[EepromRotateOffset] << 8) | data[EepromRotateOffset + 1];
    return stored == settingsCrc(data);
}

size_t settingsPool(uint32_t last) {
    return (last > 4000) ? 32 : (last > 2000) ? 16 : (last > 1000) ? 8 : (last > 500) ? 4 : 2;
}

} // namespace
} // namespace modules
} // namespace espurna

void modulesMigrateSettings() {
    using namespace espurna::modules;
    const uint32_t real = ESP.getFlashChipRealSize();
    const uint32_t mapped = ESP.getFlashChipSize();
    if (real <= mapped) {
        return;
    }

    auto* data = static_cast<uint8_t*>(malloc(EepromSize));
    if (!data) {
        return;
    }

    // the head's pool first: any valid copy there, nothing to do
    const uint32_t head_last = (mapped / SectorSize) - 5;
    for (size_t index = 0; index < settingsPool(head_last); ++index) {
        if (ESP.flashRead((head_last - index) * SectorSize, reinterpret_cast<uint32_t*>(data), EepromSize)
            && settingsValid(data))
        {
            free(data);
            return;
        }
    }

    // the newest valid copy of the old layout's pool
    const uint32_t old_last = (real / SectorSize) - 5;
    bool found = false;
    uint32_t best_sector = 0;
    uint8_t best_value = 0;
    for (size_t index = 0; index < settingsPool(old_last); ++index) {
        const uint32_t sector = old_last - index;
        if (!readPastHeader(sector * SectorSize, reinterpret_cast<uint32_t*>(data), EepromSize) || !settingsValid(data)) {
            continue;
        }
        const uint8_t value = data[EepromRotateOffset + 2];
        const bool newer = !found || ((value < best_value) && ((best_value - value) > 128))
            || ((value > best_value) && ((value - best_value) < 128));
        if (newer) {
            found = true;
            best_sector = sector;
            best_value = value;
        }
    }

    if (found && readPastHeader(best_sector * SectorSize, reinterpret_cast<uint32_t*>(data), EepromSize)
        && ESP.flashEraseSector(head_last))
    {
        ESP.flashWrite(head_last * SectorSize, reinterpret_cast<const uint32_t*>(data), EepromSize);
    }

    free(data);
}

namespace espurna {
namespace modules {
namespace {

// --- the guard's state: survives a crash restart, not a power cut (.noinit is not cleared at boot) ----------------

__attribute__((section(".noinit")))
GuardSaved guard_memory;

os_timer_t guard_timer;

void guardTimerCallback(void*) {
    guardTick();
}

// --- upload: placed when its header is in (the arenas: RAM, RAM in IRAM, IRAM code), relocated word by word --------

struct Upload : WordUpload {
    bool ram_in_iram { false }; // ?ram=iram
    uint32_t ram_offset { 0 };
    uint32_t iram_offset { 0 };

    // placed when switched on (arch::place): the package from the board's own modules file, into the entry's slot
    const Directory* local_dir { nullptr };
    const Entry* local_entry { nullptr };
};

// the RAM and the IRAM code where the Apply placed them; else (a module the head's image brings, an old single upload)
// into what the IRAM tail has left: the heap block of this boot was sized before, it has no room for more. The deltas
const char* arenas(Upload& up, Spaces& space, bool) {
    const auto& header = up.header;
    const uint32_t ram_size = roundUp(header.ram_size, 8);
    if (up.planned) {
        up.ram_in_iram = (up.planned_ram & RamSecondArena) != 0;
        up.ram_offset = ram_size ? (up.planned_ram & ~RamSecondArena) : 0;
        up.iram_offset = up.planned_iram;
    } else if (ram_size) {
        if (!space.ram_second.place(ram_size, false, up.ram_offset)) {
            return PSTR("no room in the IRAM tail for its RAM: Apply in the MODULES panel");
        }
        up.ram_in_iram = true;
    } else {
        up.ram_offset = 0;
        up.ram_in_iram = false;
    }

    const uint32_t iram_size = roundUp(header.iram_size, 4);
    if (!up.planned && iram_size) {
        if (!space.iram.place(iram_size, false, up.iram_offset)) {
            return PSTR("not enough IRAM in the tail");
        }
    } else if (!up.planned) {
        up.iram_offset = 0;
    }

    up.delta[0] = 0;
    up.delta[1] = (FlashMapped + up.slot) - header.link_flash;
    const uint32_t ram_base = up.ram_in_iram ? iramTailStart() : heapBlockBase();
    up.delta[2] = (ram_base + up.ram_offset) - header.link_ram;
    up.delta[3] = (iramTailStart() + up.iram_offset) - header.link_iram;

    return nullptr;
}

const char* prepare(PackageReader& reader) {
    auto& up = static_cast<Upload&>(reader);
    const auto& header = up.header;

    auto dir_box = boxed<Directory>();
    if (!dir_box) {
        return PSTR("no memory");
    }
    auto& dir = *dir_box;
    bool installed = false;
    const auto* error = packageAccept(up, PackageVersion, dir, installed);
    if (error) {
        return error;
    }

    // a new copy of an installed module: the arenas are filled at boot, so the old copy's RAM and IRAM are free for
    // it; its flash is not (running code, or the copy that stays when this upload fails)
    auto space_box = boxed<Spaces>();
    if (!space_box) {
        return PSTR("no memory");
    }
    auto& space = *space_box;
    spaces(dir, true, space, installed ? header.name : nullptr);

    if (!space.flash.place(roundUp(header.image_size, SectorSize), true, up.slot)) {
        return PSTR("not enough flash");
    }

    return arenas(up, space, false);
}

constexpr PackageHooks Hooks { prepare, wordBytes };

// the package of a module that is in flash already, RAM not placed (EntryUnplaced): its own slot, the arenas now
const char* prepareLocal(PackageReader& reader) {
    auto& up = static_cast<Upload&>(reader);
    const auto& header = up.header;
    const auto& entry = *up.local_entry;

    const auto* error = packageCheck(header, PackageVersion);
    if (error) {
        return error;
    }
    if (std::strncmp(header.name, entry.name, sizeof(entry.name)) != 0) {
        return PSTR("the package of another module");
    }
    if (roundUp(header.image_size, SectorSize) > entry.slot) {
        return PSTR("bad image size");
    }

    // the arenas the placed modules leave (this one is not counted: it is EntryUnplaced)
    auto space_box = boxed<Spaces>();
    if (!space_box) {
        return PSTR("no memory");
    }
    auto& space = *space_box;
    spaces(*up.local_dir, true, space);
    up.slot = entry.offset;
    up.ram_in_iram = (entry.ram_offset & RamSecondArena) != 0;
    return arenas(up, space, true);
}

constexpr PackageHooks LocalHooks { prepareLocal, wordBytes };

// the RAM and IRAM fields of the entry, from where the upload placed it
void placed(const Upload& up, Entry& out) {
    out.ram_offset = up.ram_offset | (up.ram_in_iram ? RamSecondArena : 0);
    out.ram_size = roundUp(up.header.ram_size, 8);
    out.arch[IramOffset] = up.iram_offset;
    out.arch[IramSize] = roundUp(up.header.iram_size, 4);
    out.arch[RamAddress] = ramAddress(out);
}

// --- the modules page ------------------------------------------------------------------------------

// /modules.html, and / while there is no WebUI module: a switch per module, with what it takes of the flash, the RAM
// and the IRAM (TCM) arenas, and what is taken already. The modules come from the modules file of the running release
// (espurna-esp8266-1m-modules.bin, from the disk or from the release list); on: its package is uploaded to
// /modules/upload, off: /modules/remove. Both apply at the next restart
alignas(4) static constexpr char ModulesPage[] PROGMEM = R"~(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>ESPurna modules</title>
<style>body{font:15px system-ui,sans-serif;margin:0 auto;padding:16px;max-width:760px;color:#222;background:#fff}
h2{margin:.2em 0}.sub{color:#666;font-size:13px;margin-bottom:12px}
.bar{display:flex;height:22px;border-radius:4px;overflow:hidden;background:#e6e6e6;margin:4px 0 2px}
.bar i{display:block;height:100%}.h{background:#8090a8}.m{background:#2f80c8}.n{background:#a6d8a6}
.y{background:#f0b429}.tight{color:#b07800}
.lg{font-size:13px;color:#444;margin-bottom:12px}
table{border-collapse:collapse;width:100%}td,th{padding:6px 4px;border-bottom:1px solid #eee;text-align:left;font-size:14px}
.r{text-align:right}.st{font-size:12px;color:#666}.bad{color:#c00}
button{font-size:14px;padding:6px 12px;margin:4px 4px 4px 0}input[type=checkbox]{width:20px;height:20px}
#msg{white-space:pre-wrap;background:#f4f4f4;padding:8px;min-height:1em;font-size:13px}</style>
<h2>Modules</h2><div class=sub id=ver></div>
<div id=off style="display:none;background:#fde2e1;border:1px solid #d9534f;padding:8px;margin-bottom:12px">
<b>Modules are off</b>: <span id=why></span><br><button id=on>Start the modules again</button></div><div id=bars></div>
<table><thead><tr><th></th><th>Module</th><th class=r>Flash</th><th class=r>RAM</th><th class=r>IRAM</th>
<th>RAM in</th><th>State</th></tr></thead><tbody id=list></tbody></table>
<p>Modules file or full image: <input type=file id=file accept=.bin> <button id=gh>From GitHub</button></p>
<p><button id=rs>Restart</button> <a href="./">WebUI</a></p><div id=msg></div>
<script>
var S,A={},W={},$=function(i){return document.getElementById(i)};
function kb(v){return (v/1024).toFixed(1)+' KB'}
function sec(v){return Math.ceil(v/4096)*4096}
function msg(t){$('msg').textContent=t}
var TIGHT=0.85;
function bar(t,total,parts,lg){var taken=0,h,y;parts.forEach(function(p){if(p[0]!='n')taken+=p[1]});y=taken>=TIGHT*total;
h='<b>'+t+'</b>'+(y?' <span class=tight>almost full</span>':'')+'<div class=bar>';parts.forEach(function(p){if(p[1]>0)
h+='<i class='+(y&&p[0]=='m'?'y':p[0])+' title="'+p[2]+'" style="width:'+(100*p[1]/total)+'%"></i>'});
return h+'</div><div class=lg>'+lg+'</div>'}
function load(){return fetch('modules',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){S=j;draw();
if(S.file&&!Object.keys(A).length)return fetch('modules/file').then(function(r){if(!r.ok)throw r.status;return r.arrayBuffer()})
.then(function(b){parse(b);msg('modules file: from this board ('+Object.keys(A).join(', ')+')')})})}
function find(n){return S.modules.filter(function(x){return x.name==n})[0]}
function draw(){var used=0,rf=S.ram_arena-S.ram_used,irf=S.iram_arena-S.iram_used,names={},h='';
S.modules.forEach(function(m){used+=sec(m.size);names[m.name]=1});Object.keys(A).forEach(function(n){names[n]=1});
$('ver').textContent=S.version+', '+S.image;$('off').style.display=S.off?'block':'none';$('why').textContent=S.off;
$('bars').innerHTML=bar('Flash',S.area,[['h',S.head,'head'],['m',used,'modules'],['n',S.free,'free']],
'head '+kb(S.head)+' + modules '+kb(used)+' of '+kb(S.area)+', free '+kb(S.free))+
bar('RAM',S.ram_arena,[['m',S.ram_used,'modules'],['n',rf,'free']],'modules '+S.ram_used+' of '+S.ram_arena+
' bytes, free '+rf+'; heap free '+kb(S.heap)+' (after boot '+kb(S.heap_boot)+')')+
bar('IRAM (TCM)',S.iram_arena,[['m',S.iram_used,'modules'],['n',irf,'free']],'module code '+S.iram_used+' of '+
S.iram_arena+' bytes, free '+irf)+(S.iram_data?bar('RAM in IRAM',S.iram_data,[['m',S.iram_data_used,'modules'],
['n',S.iram_data-S.iram_data_used,'free']],'module RAM '+S.iram_data_used+' of '+S.iram_data+' bytes, free '+
(S.iram_data-S.iram_data_used)+' (32-bit memory: byte access is emulated, slower)'):'');
Object.keys(names).sort().forEach(function(n){var m=find(n),a=A[n],fit=!!m,tight=false;
var why=[],ii=W[n]=='iram';if(!m){if(sec(a.image)>S.free_max)why.push('flash');
if(ii?a.ram>(S.iram_data_max||0):a.ram>S.ram_max)why.push(ii?'IRAM for its RAM':'RAM');
if(a.iram>S.iram_max)why.push('IRAM');fit=!why.length;
tight=fit&&((S.ram_used+a.ram)>=TIGHT*S.ram_arena||(S.iram_used+a.iram)>=TIGHT*S.iram_arena||
(S.area-S.free+sec(a.image))>=TIGHT*S.area)}
var on=m&&m.enabled!==0;
h+='<tr><td><input type=checkbox data-n="'+n+'"'+(on?' checked':'')+(fit?'':' disabled')+'></td><td>'+n+
(m&&!on?' <button data-x="'+n+'">Remove</button>':'')+
'</td><td class=r>'+kb(m?m.size:a.image)+'</td><td class=r>'+(m?m.ram:a.ram)+'</td><td class=r>'+(m?m.iram:a.iram)+
'</td><td>'+cell(n,m,a)+
'</td><td class="st'+(fit?(tight?' tight':''):' bad')+'">'+(m?(on?m.state:(m.state=='running'?'off after restart':
'off, in flash')):(fit?(tight?'fits, tight':'can be added'):
'no '+why.join(', ')+' for it'))+'</td></tr>'});
$('list').innerHTML=h;Array.prototype.forEach.call(document.querySelectorAll('input[data-n]'),function(e){
e.onchange=function(){toggle(e)}});Array.prototype.forEach.call(document.querySelectorAll('button[data-x]'),function(e){
e.onclick=function(){var n=e.getAttribute('data-x'),d=new FormData();d.append('name',n);msg(n+': removing...');
post('modules/remove',d).then(function(t){msg(n+': '+t)},function(x){msg(n+': '+x)}).then(load)}});
Array.prototype.forEach.call(document.querySelectorAll('select[data-r]'),function(e){
e.onchange=function(){var n=e.getAttribute('data-r');if(e.getAttribute('data-m'))move(n,e.value);else{W[n]=e.value;draw()}}})}
function canMove(m,a){var to=m.ram_in=='iram'?'dram':'iram';return (m.state=='running'||m.state=='not started')&&
sec(a.image)<=S.free_max&&(to=='iram'?a.ram<=(S.iram_data_max||0):a.ram<=S.ram_max)}
function cell(n,m,a){var w=m?(m.ram_in||'dram'):(W[n]=='iram'?'iram':'dram');if(!S.iram_data||!a||!a.ram||(m&&!canMove(m,a)))
return w.toUpperCase();return '<select data-r="'+n+'"'+(m?' data-m=1':'')+'><option value=dram'+(w=='dram'?' selected':'')+
'>DRAM<option value=iram'+(w=='iram'?' selected':'')+'>IRAM</select>'}
function move(n,v){var u=new FormData();u.append('module',new Blob([A[n].data]),n+'.module');
msg(n+': moving its RAM to '+v.toUpperCase()+'...');post('modules/upload?replace=1'+(v=='iram'?'&ram=iram':''),u).then(function(t){
msg(n+': '+t);$('rs').style.background='#f0ad4e'},function(x){msg(n+': '+x)}).then(load)}
function post(u,d){return fetch(u,{method:'POST',body:d}).then(function(r){return r.text().then(function(t){
if(!r.ok)throw t||r.status;return t})})}
function toggle(e){var n=e.getAttribute('data-n'),d=new FormData(),m=find(n),u;e.disabled=true;
if(m){d.append('name',n);d.append('on',e.checked?'1':'0');msg(n+': switching '+(e.checked?'on':'off')+'...');u='modules/enable'}
else{if(!A[n]){msg(n+': not in the modules file');return load()}
d.append('module',new Blob([A[n].data]),n+'.module');msg(n+': uploading...');u='modules/upload'+(W[n]=='iram'?'?ram=iram':'')}
post(u,d).then(function(t){msg(n+': '+t)},function(x){msg(n+': '+x)}).then(function(){
return new Promise(function(r){setTimeout(r,1500)})}).then(load)}
function parse(b){var v=new DataView(b),o={},c,i,k,j,nm,ch,z=0;if(b.byteLength>0x100008&&v.getUint8(0)==0xE9&&
v.getUint32(0x100000,true)==0x32434d45)z=0x100000;if(b.byteLength<z+8||v.getUint32(z,true)!=0x32434d45)
throw 'not a modules file (or the full image)';c=v.getUint32(z+4,true);for(i=0;i<c;i++){k=z+8+72*i;nm='';
for(j=0;j<16;j++){ch=v.getUint8(k+j);if(!ch)break;nm+=String.fromCharCode(ch)}o[nm]={image:v.getUint32(k+24,true),
ram:v.getUint32(k+28,true),iram:v.getUint32(k+32,true),data:new Uint8Array(b,z+v.getUint32(k+16,true),
v.getUint32(k+20,true))}}
A=o;msg('modules file: '+Object.keys(o).join(', '));draw()}
$('file').onchange=function(){var f=this.files[0];if(f)f.arrayBuffer().then(parse).catch(function(x){msg(''+x)})};
$('gh').onclick=function(){var rev=(S.version.split('-git')[1]||'').slice(0,7);msg('looking for the modules of '+rev+'...');
fetch(S.list||'https://igorufox.github.io/espurna/firmware.json').then(function(r){return r.json()}).then(function(j){
var u=null;(j.releases||[]).forEach(function(r){if(String(r.commit||'').indexOf(rev)==0)((r.images||{})[S.image]||[])
.forEach(function(f){if(f.kind=='modules'||(f.kind=='full'&&!u))u=f.url})});if(!u)throw 'no modules file of '+rev+
' in the release list, pick the file from the disk';return fetch(u).then(function(r){return r.arrayBuffer()}).then(parse)})
.catch(function(x){msg(''+x)})};
$('on').onclick=function(){post('modules/on',new FormData()).then(function(t){msg(t);
setTimeout(function(){location.reload()},15000)})};
$('rs').onclick=function(){post('modules/restart',new FormData()).then(function(){msg('restarting...');
setTimeout(function(){location.reload()},15000)})};
load().catch(function(x){msg(''+x)});
</script>
)~";

} // namespace

// --- the contract (modules_core.h) -----------------------------------------------------------------------------

namespace arch {

// the SDK reserve and the TCP accept gate (heap_reserve_esp8266.cpp)
void setup() {
    modulesHeapSetup();
}

uint32_t directoryMagic() {
    return DirectoryMagic;
}

// the Flash meter from 0: the head, then the modules up to the directory
Area area() {
    return Area{0, headEnd(), directoryOffset()};
}

bool rawRead(uint32_t offset, void* data, size_t size) {
    return ESP.flashRead(offset, static_cast<uint8_t*>(data), size);
}

bool rawErase(uint32_t offset, uint32_t size) {
    for (uint32_t done = 0; done < size; done += SectorSize) {
        if (!ESP.flashEraseSector((offset + done) / SectorSize)) {
            return false;
        }
    }

    return true;
}

// the callers' buffers are word aligned
bool rawWrite(uint32_t offset, const void* data, size_t size) {
    return ESP.flashWrite(offset, static_cast<const uint32_t*>(data), size);
}

// MD5 of the running sketch, as the module link took it from the head .bin
bool headId(uint8_t out[16]) {
    return hexId(ESP.getSketchMD5(), out);
}

// from the mapped flash, RAM in the IRAM tail or the heap block, IRAM code copied into the IRAM tail; word access only
const char* runs(const Entry& entry, const Header&, Runs& out) {
    // relocated for another address of the heap block (the boot before the Apply took it elsewhere)
    if (entry.ram_size && (ramAddress(entry) != entry.arch[RamAddress])) {
        return PSTR("the heap block moved since the Apply: Apply again");
    }
    if (entry.ram_size && !(entry.ram_offset & RamSecondArena)
        && ((entry.ram_offset + entry.ram_size) > heapBlockSize()))
    {
        return PSTR("not in this boot's heap block: Apply again");
    }

    out.flash_lo = FlashMapped + entry.offset;
    out.flash_hi = out.flash_lo + entry.size;
    out.entry_lo = out.flash_lo;
    out.entry_hi = out.flash_hi;

    out.ram_lo = ramAddress(entry);
    out.ram_hi = out.ram_lo + entry.ram_size;

    out.iram_lo = iramTailStart() + entry.arch[IramOffset];
    out.iram_hi = out.iram_lo + entry.arch[IramSize];
    out.iram_copy = true;
    out.words = true;
    return nullptr;
}

PackageReader* uploadNew(AsyncWebServerRequest* request) {
    auto* out = new (std::nothrow) Upload();
    if (out) {
        out->hooks = &Hooks;
        const auto* ram = request->getParam(String(F("ram")));
        out->ram_in_iram = ram && ram->value().equals(F("iram"));
    }

    return out;
}

const char* uploadFinish(PackageReader& reader, Entry& out) {
    auto& up = static_cast<Upload&>(reader);
    uint32_t crc;
    const auto* error = wordFinish(up, crc);
    if (error) {
        return error;
    }

    out.offset = up.slot;
    out.slot = roundUp(up.written, SectorSize);
    out.size = up.written;
    out.crc32 = crc;
    placed(up, out);
    return nullptr;
}

bool placeable(const Entry& entry) {
    // nothing in the arenas: nothing to give back
    if (!entry.ram_size && !entry.arch[IramSize]) {
        return false;
    }

    uint32_t offset;
    uint32_t size;
    PackageHeader header;
    return onboardPackage(entry.name, offset, size) && (size >= sizeof(header))
        && readPastHeader(offset, reinterpret_cast<uint32_t*>(&header), sizeof(header))
        && (header.magic == PackageMagic) && sameHead(header.head_md5)
        && (roundUp(header.image_size, SectorSize) <= entry.slot);
}

// the package from the board's modules file through the upload's relocation, into the entry's own slot
const char* place(const Directory& dir, Entry& entry) {
    uint32_t offset;
    uint32_t size;
    // the modules file at 1 MB, else the one in the head's tail (the modules the head brings, at their install)
    if (!onboardPackage(entry.name, offset, size) && !tailPackage(entry.name, offset, size)) {
        return PSTR("its package is not on the board, upload it again");
    }

    auto* up = new (std::nothrow) Upload();
    if (!up) {
        return PSTR("no memory");
    }
    up->hooks = &LocalHooks;
    up->local_dir = &dir;
    up->local_entry = &entry;

    alignas(4) uint8_t part[256];
    for (uint32_t done = 0; (done < size) && !up->error; done += sizeof(part)) {
        const uint32_t length = std::min<uint32_t>(sizeof(part), size - done);
        if (!readPastHeader(offset + done, reinterpret_cast<uint32_t*>(part), (length + 3) & ~3u)) {
            up->error = PSTR("package read");
            break;
        }
        packageFeed(*up, part, length);
        if ((done % SectorSize) == 0) {
            yield();
        }
    }

    uint32_t crc;
    const auto* error = wordFinish(*up, crc);
    if (!error) {
        entry.size = up->written;
        entry.crc32 = crc;
        placed(*up, entry);
    }

    delete up;
    return error;
}

// no directory of this head: the modules of the head's tail go in, switched on, RAM not placed (the boot places it from
// the tail, place() above). A directory of this head stays as it is: a module the user removed stays removed
void imageModules(Directory& dir, bool& valid) {
    const uint32_t base = headEnd();
    const uint32_t count = containerCount(base);
    if (!count) {
        return;
    }

    if (valid) {
        if (!dir.count) {
            return;
        }
        for (size_t index = 0; index < dir.count; ++index) {
            Header header;
            if (readHeader(dir.entries[index], header) && (header.magic == HeaderMagic)
                && sameHead(header.head_md5))
            {
                return;
            }
        }
    }

    // the tail's own sectors stay out of the slots until the modules are placed from it
    uint32_t tail_end = 8 + (count * ContainerEntry);
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t entry[ContainerEntry / 4];
        if (!containerEntry(base, index, entry)) {
            return;
        }
        tail_end = std::max(tail_end, entry[4] + entry[5]);
    }

    Directory fresh;
    std::memset(&fresh, 0, sizeof(fresh));
    fresh.magic = DirectoryMagic;

    Space flash;
    flash.lo = base + roundUp(tail_end, SectorSize);
    // the directory's second copy under it (DirectorySectors)
    flash.hi = directoryOffset() - SectorSize;

    for (uint32_t index = 0; (index < count) && (fresh.count < DirectoryEntries); ++index) {
        uint32_t entry[ContainerEntry / 4];
        PackageHeader header;
        if (!containerEntry(base, index, entry)
            || !readPastHeader(base + entry[4], reinterpret_cast<uint32_t*>(&header), sizeof(header))
            || (header.magic != PackageMagic) || (header.version != 1) || !sameHead(header.head_md5))
        {
            continue;
        }

        const uint32_t slot = roundUp(header.image_size, SectorSize);
        uint32_t at;
        if (!flash.place(slot, true, at)) {
            continue;
        }
        flash.add(at, slot);

        auto& out = fresh.entries[fresh.count++];
        std::memcpy(out.name, header.name, sizeof(out.name));
        out.name[sizeof(out.name) - 1] = '\0';
        out.offset = at;
        out.slot = slot;
        out.size = header.image_size;
        out.ram_size = roundUp(header.ram_size, 4);
        out.arch[IramSize] = roundUp(header.iram_size, 4);
        out.flags = EntryUnplaced;
        DEBUG_MSG_P(PSTR("[MODULES] %s comes with the head, on (0x%05x)\n"), out.name, at);
    }

    if (fresh.count && writeDirectory(fresh)) {
        dir = fresh;
        valid = true;
    }
}

uint32_t slotFor(uint32_t image) {
    return roundUp(image, SectorSize);
}

// the IRAM tail is the fixed pool (module code first, then module RAM), the rest of the RAM in the heap block
void pools(Pools& out) {
    // the free heap the loop sees (not inside this request, whose buffers take a part of it): the block comes out of
    // it at the next boot, the head keeps MODULES_HEAP_KEEP of it
    const uint32_t free = modulesHeapLoop();
    out.heap = heapBlockSize() + ((free > MODULES_HEAP_KEEP) ? (free - MODULES_HEAP_KEEP) : 0);
    out.fixed = iramTailSize();
    out.fixed_words = true;
    out.code_in_fixed = true;
}

// one IRAM tail for the code (iram) and the RAM in it (ram_second): both spaces span it, each counts both
void arenas(const Directory& dir, bool valid, Spaces& out, const char* except) {
    out.iram.lo = 0;
    out.iram.hi = iramTailSize();
    out.ram_second.lo = 0;
    out.ram_second.hi = iramTailSize();
    for (size_t index = 0; valid && (index < dir.count); ++index) {
        const auto& entry = dir.entries[index];
        if (!counted(entry, except)) {
            continue;
        }
        out.iram.add(entry.arch[IramOffset], entry.arch[IramSize]);
        out.ram_second.add(entry.arch[IramOffset], entry.arch[IramSize]);
        if (entry.ram_offset & RamSecondArena) {
            out.iram.add(entry.ram_offset & ~RamSecondArena, entry.ram_size);
        }
    }
}

void statusJson(String& out) {
    out += F(",\"pool\":\"IRAM\"");
    jsonField(out, F("reserve"), modulesHeapReserve());
    jsonField(out, F("reserve_used"), modulesHeapReserveUsed());
    jsonField(out, F("refused"), modulesAcceptRefused());
    // the free heap the loop sees, outside this request ("heap" is measured inside it)
    jsonField(out, F("heap_loop"), modulesHeapLoop());
    jsonField(out, F("heap_low"), modulesHeapLow());
    jsonField(out, F("chip"), ESP.getFlashChipRealSize());
}

uint32_t entryIram(const Entry& entry) {
    return entry.arch[IramSize];
}

void listHead(Print& out) {
    out.printf_P(PSTR("loader esp8266 rev 4 (core), head image %u bytes\n"), ESP.getSketchSize());
}

void listEntry(Print& out, const Entry& entry) {
    out.printf_P(PSTR("%s, IRAM %u @%u"), (entry.ram_offset & RamSecondArena) ? " in IRAM" : "",
        entry.arch[IramSize], entry.arch[IramOffset]);
}

uint32_t fileSize() {
    return onboardFileSize();
}

void fileSend(AsyncWebServerRequest* request) {
    const uint32_t size = onboardFileSize();
    auto* response = request->beginResponse(String(F("application/octet-stream")), size,
        [size](uint8_t* buffer, size_t max, size_t index) -> size_t {
            if (index >= size) {
                return 0;
            }

            // aligned reads of at most 256 bytes, copied out at the asked offset
            alignas(4) uint8_t part[260];
            const uint32_t address = OnboardFile + index;
            const uint32_t aligned = address & ~3u;
            const size_t skip = address - aligned;
            const size_t want = std::min<size_t>(std::min<size_t>(max, size - index), sizeof(part) - 4);
            if (!readPastHeader(aligned, reinterpret_cast<uint32_t*>(part), (skip + want + 3) & ~3u)) {
                return 0;
            }

            std::memcpy(buffer, part + skip, want);
            return want;
        });
    response->addHeader(String(F("Cache-Control")), String(F("no-store")));
    request->send(response);
}

bool page(AsyncWebServerRequest* request) {
    auto* response = request->beginResponse_P(200, String(F("text/html")),
        reinterpret_cast<const uint8_t*>(ModulesPage), sizeof(ModulesPage) - 1);
    request->send(response);
    return true;
}

bool crashRestart() {
    const auto* info = ESP.getResetInfoPtr();
    return info && ((info->reason == REASON_WDT_RST)
        || (info->reason == REASON_EXCEPTION_RST)
        || (info->reason == REASON_SOFT_WDT_RST));
}

GuardState& guardState() {
    return guardStateIn(guard_memory);
}

void guardTimer() {
    os_timer_setfn(&guard_timer, guardTimerCallback, nullptr);
    os_timer_arm(&guard_timer, 1000, true);
}

void guardTimerStop() {
    os_timer_disarm(&guard_timer);
}

void restartNow() {
    system_restart();
}

uint32_t freeHeap() {
    return ESP.getFreeHeap();
}

uint32_t failedAllocations() {
    return umm_get_oom_count();
}

// the web upload, and ArduinoOTA (Update) too
bool otaBusy() {
    return otaWebUploading() || Update.isRunning();
}

} // namespace arch
} // namespace modules
} // namespace espurna

#endif // MODULES_SUPPORT && MODULES_CORE && ESP8266
