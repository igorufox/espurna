import { afterEach, describe, expect, test, vi } from 'vitest';
import {
    APPLY_MAX,
    HEAD_ROUTES,
    OTA_MODULES,
    applyProblems,
    headFor,
    headName,
    installImage,
    isInstallImage,
    modulesFileOf,
    modulesHeadId,
    needsHead,
    otaParts,
    parseContainer,
    runningNames,
} from '../src/install.mjs';
import {
    applyRoute,
    canToggle,
    notRunning,
    dependentsOf,
    groupCounts,
    groupOf,
    switchOnOrder,
    meterLevel,
    meters,
    moduleRows,
    sectors,
    selectionAfter,
    sizeText,
} from '../src/modules.mjs';

/**
 * EMC2: 72-byte entries (name16, offset, size, image, ram, iram, requires32, 4 reserved)
 * @param {{name: string, image: number, ram: number, iram: number, data: number[], requires?: string}[]} list
 * @returns {ArrayBuffer}
 */
function container(list) {
    const size = 8 + (72 * list.length) + list.reduce((sum, x) => sum + x.data.length, 0);
    const buffer = new ArrayBuffer(size);
    const view = new DataView(buffer);
    view.setUint32(0, 0x32434d45, true);
    view.setUint32(4, list.length, true);

    let offset = 8 + (72 * list.length);
    list.forEach((x, index) => {
        const at = 8 + (72 * index);
        [...x.name].forEach((ch, n) => view.setUint8(at + n, ch.charCodeAt(0)));
        [...(x.requires ?? "")].forEach((ch, n) => view.setUint8(at + 36 + n, ch.charCodeAt(0)));
        view.setUint32(at + 16, offset, true);
        view.setUint32(at + 20, x.data.length, true);
        view.setUint32(at + 24, x.image, true);
        view.setUint32(at + 28, x.ram, true);
        view.setUint32(at + 32, x.iram, true);
        new Uint8Array(buffer, offset, x.data.length).set(x.data);
        offset += x.data.length;
    });

    return buffer;
}

/** @returns {import('../src/modules.mjs').Status} */
function status(extra = {}) {
    return {
        version: '26.11.0-git64c96b10', image: 'esp8266-1m-head', list: '', off: '',
        modules: [
            {name: 'webui', size: 50000, offset: 0xD0000, ram: 0, ram_in: 'dram', iram: 0, state: 'running'},
        ],
        area: 0xF9000, head: 598016, free: 300000, free_max: 200000,
        ram_pool: 5120, ram_used: 1000, ram_max: 4120,
        iram_pool: 2048, iram_used: 0, iram_max: 2048,
        iram_data: 17484, iram_data_used: 0, iram_data_max: 17484,
        heap: 11000, heap_boot: 30000,
        ...extra,
    };
}

test('meters: flash counts the slot of a module (its flash format), the rest is kept by the chip', () => {
    // a Beken module: 467520 bytes of image take 524288 in the flash (CRC, maps, links)
    const st = status({
        modules: [{name: 'matter', size: 467520, slot: 524288, offset: 0x14C000, ram: 0, ram_in: 'iram', iram: 0, state: 'running'}],
        area: 1826816, head: 753664, free: 77824,
    });
    const [flash] = meters(st, null);
    expect(flash.used).toBe(524288);
    expect(flash.legend).toEqual(['head 736.0 KB', 'modules 512.0 KB', 'kept by the chip 460.0 KB']);

    // an older head without the slot: the image size in whole sectors
    const [old] = meters(status(), null);
    expect(old.used).toBe(sectors(50000));
});

test('sizes are shown in bytes and kilobytes, flash goes by sectors', () => {
    expect(sizeText(512)).toBe('512 B');
    expect(sizeText(2048)).toBe('2.0 KB');
    expect(sectors(1)).toBe(4096);
    expect(sectors(4096)).toBe(4096);
    expect(sectors(4097)).toBe(8192);
});

test('the modules file is read, a foreign or cut file is refused', () => {
    const packages = parseContainer(container([
        {name: 'sensors', image: 90000, ram: 1200, iram: 0, data: [1, 2, 3]},
        {name: 'rf', image: 16000, ram: 1300, iram: 64, data: [4, 5]},
    ]));

    expect([...packages.keys()]).toEqual(['sensors', 'rf']);
    expect(packages.get('rf')).toMatchObject({image: 16000, ram: 1300, iram: 64});
    expect([...(packages.get('sensors')?.data ?? [])]).toEqual([1, 2, 3]);

    expect(() => parseContainer(new ArrayBuffer(16))).toThrow('not a modules file');

    const cut = container([{name: 'rf', image: 1, ram: 0, iram: 0, data: [1, 2, 3, 4]}]).slice(0, 82);
    expect(() => parseContainer(cut)).toThrow('cut short');
});

test('rows: installed first-hand, the rest say whether they fit and what is short', () => {
    const packages = parseContainer(container([
        {name: 'webui', image: 50000, ram: 0, iram: 0, data: [0]},
        {name: 'sensors', image: 90000, ram: 1200, iram: 0, data: [0]},
        {name: 'light', image: 300000, ram: 1500, iram: 0, data: [0]},
        {name: 'rf', image: 16000, ram: 5000, iram: 0, data: [0]},
    ]));

    const rows = moduleRows(status(), packages, new Map());
    expect(rows.map((x) => x.name)).toEqual(['light', 'rf', 'sensors', 'webui']);

    const byName = Object.fromEntries(rows.map((x) => [x.name, x]));
    expect(byName.webui.installed?.state).toBe('running');
    // the RAM goes into the fixed pool while it has room (as the Apply places it)
    expect(byName.sensors).toMatchObject({fits: true, short: [], ramIn: 'iram'});
    expect(byName.light).toMatchObject({fits: false, short: ['flash']});
    expect(byName.rf).toMatchObject({fits: true, ramIn: 'iram'});

    // no room left in the pool: the heap, 5000 is more than it gives
    const full = moduleRows(status({iram_data_max: 1000}), packages, new Map());
    expect(full.find((x) => x.name === 'rf')).toMatchObject({fits: false, short: ['RAM'], ramIn: 'dram'});
});

test('a module that takes the RAM to 85 % and more is tight', () => {
    const packages = parseContainer(container([
        {name: 'sensors', image: 9000, ram: 3500, iram: 0, data: [0]},
    ]));

    const [row] = moduleRows(status({modules: [], iram_data: 0}), packages, new Map());
    expect(row).toMatchObject({fits: true, tight: true});
});

test('meters: flash counts the head and whole sectors, the preview adds the module that is off', () => {
    const packages = parseContainer(container([
        {name: 'sensors', image: 9000, ram: 1200, iram: 100, data: [0]},
    ]));
    const st = status();
    const rows = moduleRows(st, packages, new Map());
    const preview = rows.find((x) => x.name === 'sensors') ?? null;

    const [flash, ram, pool] = meters(st, preview);
    expect(flash).toMatchObject({head: 598016, used: sectors(50000), added: sectors(9000)});
    // its statics and its IRAM code go into the IRAM tail
    expect(ram).toMatchObject({used: 1000, added: 0, total: 1000 + 11000});
    expect(pool).toMatchObject({title: 'IRAM tail', added: 1300});

    // the heap the modules took when they started: its own segment
    expect(meters(status({ram_dynamic: 3000}), null)[1]).toMatchObject({used: 1000, dynamic: 3000, total: 15000});
    expect(meters(status({iram_data: 0}), null)).toHaveLength(2);
    expect(meters(status({pool: 'TCM'}), null)[2]).toMatchObject({title: 'TCM'});

    expect(meterLevel({title: '', total: 100, head: 0, used: 50, added: 0, free: '', legend: []})).toBe('');
    expect(meterLevel({title: '', total: 100, head: 0, used: 85, added: 0, free: '', legend: []})).toBe('mod-tight');
    expect(meterLevel({title: '', total: 100, head: 0, used: 90, added: 20, free: '', legend: []})).toBe('mod-over');
});

test('switching on takes what a module needs first, the deepest first; others that need it go off with it', () => {
    const packages = parseContainer(container([
        {name: 'sensors', image: 9000, ram: 100, iram: 0, data: [0]},
        {name: 'dht', image: 3000, ram: 50, iram: 0, data: [0], requires: 'sensors'},
        {name: 'thingspeak', image: 4000, ram: 60, iram: 0, data: [0], requires: 'dht, mqtt'},
    ]));
    expect(packages.get('thingspeak')?.requires).toEqual(['dht', 'mqtt']);

    // nothing installed: sensors, dht, then thingspeak; mqtt is in neither place
    const none = status({modules: []});
    expect(switchOnOrder('dht', none, packages)).toEqual({order: ['sensors', 'dht'], missing: []});
    expect(switchOnOrder('thingspeak', none, packages).missing).toEqual(['mqtt']);

    // sensors installed and on: only what is off or missing is switched
    const board = status({modules: [
        {name: 'sensors', size: 9000, offset: 0xE0000, ram: 100, ram_in: 'dram', iram: 0, state: 'running',
            enabled: 1, requires: ''},
        {name: 'dht', size: 3000, offset: 0xD0000, ram: 50, ram_in: 'dram', iram: 0, state: 'off', enabled: 0,
            requires: 'sensors'},
    ]});
    expect(switchOnOrder('dht', board, packages).order).toEqual(['dht']);

    // who needs sensors: dht (off) - all installed ones, or only those switched on
    expect(dependentsOf('sensors', board, packages, false)).toEqual(['dht']);
    expect(dependentsOf('sensors', board, packages, true)).toEqual([]);
});

test('modules are grouped for the panel', () => {
    expect(groupOf('webui')).toBe('Interface');
    expect(groupOf('dht')).toBe('Sensors');
    expect(groupOf('homeassistant')).toBe('Integrations');
    expect(groupOf('something')).toBe('Other');
});

test('a group counts its modules: on, in flash, all', () => {
    const packages = parseContainer(container([
        {name: 'alexa', image: 9000, ram: 100, iram: 0, data: [0]},
        {name: 'domoticz', image: 9000, ram: 100, iram: 0, data: [0]},
        {name: 'mdns', image: 9000, ram: 100, iram: 0, data: [0]},
    ]));
    const board = status({modules: [
        {name: 'alexa', size: 9000, offset: 0xE0000, ram: 100, ram_in: 'dram', iram: 0, state: 'running', enabled: 1},
        {name: 'domoticz', size: 9000, offset: 0xD0000, ram: 100, ram_in: 'dram', iram: 0, state: 'off', enabled: 0},
    ]});
    const rows = moduleRows(board, packages, new Map()).filter((row) => groupOf(row.name) === 'Integrations');
    expect(groupCounts(rows)).toEqual({on: 1, installed: 2, all: 3});
});

describe('install image and the ticked set', async () => {
    const { selectionAfter, runningSet } = await import('../src/modules.mjs');

    /** an install image: one head and one modules file */
    function ota(head, modules) {
        const count = 2;
        const table = 8 + 32 * count;
        const out = new Uint8Array(table + head.length + modules.length);
        const view = new DataView(out.buffer);
        view.setUint32(0, 0x31544f45, true);
        view.setUint32(4, count, true);
        const parts = [[1, 'esp32', head], [2, 'modules', modules]];
        let offset = table;
        parts.forEach(([kind, name, data], index) => {
            const at = 8 + 32 * index;
            view.setUint32(at, kind, true);
            [...name].forEach((c, i) => view.setUint8(at + 4 + i, c.charCodeAt(0)));
            view.setUint32(at + 20, offset, true);
            view.setUint32(at + 24, data.length, true);
            out.set(data, offset);
            offset += data.length;
        });
        return out.buffer;
    }

    test('the parts of an install image', () => {
        const buffer = ota(new Uint8Array([0xe9, 1, 2]), new Uint8Array([0x45, 0x4d, 0x43, 0x32, 0, 0, 0, 0]));
        const parts = otaParts(buffer);
        expect(parts.map((x) => [x.kind, x.name, x.data.length])).toEqual([[1, 'esp32', 3], [2, 'modules', 8]]);
        expect(new Uint8Array(modulesFileOf(buffer))).toEqual(parts.find((x) => x.kind === OTA_MODULES).data);
        expect(isInstallImage(buffer)).toBe(true);
    });

    test('not an install image: the buffer as it is', () => {
        const plain = new Uint8Array([0x45, 0x4d, 0x43, 0x32]).buffer;
        expect(otaParts(plain)).toBe(null);
        expect(modulesFileOf(plain)).toBe(plain);
        expect(isInstallImage(plain)).toBe(false);
    });

    test('ticking a module ticks what it needs, unticking unticks what needs it', () => {
        const status = {modules: [{name: 'mqtt', enabled: 1, requires: ''}, {name: 'captive', enabled: 1, requires: ''}]};
        const packages = new Map([
            ['domoticz', {requires: ['mqtt']}],
            ['homeassistant', {requires: ['mqtt']}],
        ]);
        const now = runningSet(status);
        expect([...now].sort()).toEqual(['captive', 'mqtt']);
        expect(runningNames({modules: [...status.modules, {name: 'rf', enabled: 0}]})).toEqual(['mqtt', 'captive']);

        const on = selectionAfter(new Set(['captive']), 'domoticz', true, status, packages);
        expect([...on].sort()).toEqual(['captive', 'domoticz', 'mqtt']);

        const more = selectionAfter(on, 'homeassistant', true, status, packages);
        const off = selectionAfter(more, 'mqtt', false, status, packages);
        expect([...off]).toEqual(['captive']);
    });
});

test('matter keeps mdns ticked: one mDNS responder, Matter publishes through it (W38)', () => {
    const beken = status({image: 'beken-tuya'});
    expect(selectionAfter(new Set(['mdns', 'mqtt']), 'matter', true, beken, new Map()))
        .toEqual(new Set(['mdns', 'mqtt', 'matter']));
});

test('the head id of the modules file and the head of an install image for the board', () => {
    const data = new Array(48).fill(0);
    for (let n = 0; n < 16; ++n) {
        data[24 + n] = n + 1;
    }
    const file = container([{name: 'mqtt', image: 9000, ram: 100, iram: 0, data}]);
    expect(modulesHeadId(file)).toBe('0102030405060708090a0b0c0d0e0f10');
    expect(modulesHeadId(container([]))).toBe('');
    expect(modulesHeadId(new ArrayBuffer(4))).toBe('');

    // the head goes on unless the board runs the build the modules were linked against
    expect(needsHead({md5: '0102030405060708090a0b0c0d0e0f10'}, file)).toBe(false);
    expect(needsHead({md5: 'ffff'}, file)).toBe(true);
    expect(needsHead({}, file)).toBe(true);

    // one Beken head for both bootloaders
    const parts = [
        {kind: 1, name: 'beken', data: new Uint8Array(1)},
        {kind: 2, name: 'modules', data: new Uint8Array(3)},
    ];
    expect(headFor(parts, 'beken-quectel')?.name).toBe('beken');
    expect(headFor(parts, 'beken-tuya')?.name).toBe('beken');
    expect(headFor(parts, 'esp32')).toBeNull();
});

test('every head of an install image has its route, the chips differ only in that data', () => {
    expect(['esp8266', 'esp32', 'beken-tuya', 'beken-quectel', 'esp32-matter'].map(headName))
        .toEqual(['esp8266', 'esp32', 'beken', 'beken', 'esp32']);
    for (const name of ['esp8266', 'beken']) {
        expect(HEAD_ROUTES[name]).toBeDefined();
    }
    // the ESP32 head is not replaced over the network (no safeboot)
    expect(HEAD_ROUTES.esp32).toBeUndefined();
    expect(HEAD_ROUTES.beken).toEqual(HEAD_ROUTES.esp8266);
});

describe('one install for every chip', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.unstubAllGlobals();
    });

    /**
     * the board: /modules answers 200; every request is logged
     * @param {string[]} log
     */
    function board(log) {
        vi.stubGlobal('fetch', async (url, options = {}) => {
            const path = new URL(url).pathname.slice(1);
            log.push(`${options.method ?? 'GET'} ${path}`);
            return new Response((path === 'modules') ? '{"modules":[]}' : 'OK', {status: 200});
        });
        vi.stubGlobal('XMLHttpRequest', class {
            constructor() {
                this.upload = {addEventListener() {}};
                this.handlers = {};
            }
            open(method, url) {
                this.path = new URL(url).pathname.slice(1);
                log.push(`${method} ${this.path}`);
            }
            addEventListener(name, handler) {
                this.handlers[name] = handler;
            }
            send(body) {
                log.push((body instanceof FormData) ? `form ${[...body.keys()].join(',')}` : 'body');
                this.status = 200;
                this.responseText = 'OK';
                setTimeout(() => this.handlers.load(), 0);
            }
        });
    }

    /** @param {string} name the head in the image */
    function image(name) {
        const data = new Array(48).fill(0);
        data[24] = 0xaa;
        const modules = new Uint8Array(container([{name: 'webui', image: 9000, ram: 0, iram: 0, data}]));
        const head = new Uint8Array([0xe9, 1, 2, 3]);
        const count = 2;
        const table = 8 + 32 * count;
        const out = new Uint8Array(table + head.length + modules.length);
        const view = new DataView(out.buffer);
        view.setUint32(0, 0x31544f45, true);
        view.setUint32(4, count, true);
        let offset = table;
        [[1, name, head], [2, 'modules', modules]].forEach(([kind, part, bytes], index) => {
            const at = 8 + 32 * index;
            view.setUint32(at, kind, true);
            [...part].forEach((c, i) => view.setUint8(at + 4 + i, c.charCodeAt(0)));
            view.setUint32(at + 20, offset, true);
            view.setUint32(at + 24, bytes.length, true);
            out.set(bytes, offset);
            offset += bytes.length;
        });
        return out.buffer;
    }

    /**
     * @param {string} chip the board's image
     * @param {string} head
     * @param {string} md5 the board's head id
     */
    async function run(chip, head, md5) {
        vi.useFakeTimers();
        /** @type {string[]} */
        const log = [];
        board(log);
        const done = installImage(new URL('http://board/'), image(head), {image: chip, md5}, ['webui'], () => {});
        await vi.runAllTimersAsync();
        await done;
        return log;
    }

    const APPLY = ['POST modules/off', 'GET modules', 'POST modules/apply', 'form names,modules', 'GET modules'];

    test('ESP8266 and Beken: the head to /upgrade after a restart without modules, then the Apply', async () => {
        const head = ['POST modules/off', 'GET modules', 'POST upgrade', 'form upgrade', 'GET modules'];
        expect(await run('esp8266', 'esp8266', 'ff')).toEqual([...head, ...APPLY]);
        expect(await run('beken-quectel', 'beken', 'ff')).toEqual([...head, ...APPLY]);
    });

    test('ESP32: another head is refused, the head is not replaced over the network', async () => {
        vi.useFakeTimers();
        const log = [];
        board(log);
        const done = installImage(new URL('http://board/'), image('esp32'), {image: 'esp32', md5: 'ff'}, ['webui'], () => {});
        const outcome = expect(done).rejects.toThrow('the ESP32 head is not replaced over the network');
        await vi.runAllTimersAsync();
        await outcome;
        expect(log).toEqual([]);
    });

    test('the board runs the head of the image: the Apply alone', async () => {
        expect(await run('esp32', 'esp32', 'aa' + '00'.repeat(15))).toEqual(APPLY);
    });

    test('a plan the board would refuse goes nowhere: no restart, no request at all', async () => {
        /** @type {string[]} */
        const log = [];
        board(log);
        const md5 = 'aa' + '00'.repeat(15);
        await expect(installImage(new URL('http://board/'), image('esp8266'), {image: 'esp8266', md5},
            ['webui', 'relay'], () => {})).rejects.toThrow('nothing changed on the board: not in the modules file: relay');
        // the same with a new head: refused before the head's restart (/modules/off on the ESP8266)
        await expect(installImage(new URL('http://board/'), image('esp8266'), {image: 'esp8266', md5: 'ff'},
            ['relay'], () => {})).rejects.toThrow('not in the modules file: relay');
        // too big for the flash of this head
        await expect(installImage(new URL('http://board/'), image('esp8266'),
            {image: 'esp8266', md5, area: 0x100000, free: 4096, modules: []}, ['webui'], () => {}))
            .rejects.toThrow('not enough flash');
        expect(log).toEqual([]);
    });
});

describe('Apply checks before /modules/off (W65)', () => {
    /** @param {{name: string, image?: number, ram?: number, iram?: number, flags?: number}[]} list */
    function file(list) {
        const buffer = container(list.map((x) => ({image: 4096, ram: 0, iram: 0, data: new Array(48).fill(0), ...x})));
        const view = new DataView(buffer);
        list.forEach((x, index) => view.setUint32(8 + (72 * index) + 68, x.flags ?? 0, true));
        return buffer;
    }

    test('names in the file, at most APPLY_MAX of them', () => {
        const f = file([{name: 'relay'}, {name: 'button'}]);
        expect(applyProblems(status(), f, ['relay', 'button'])).toEqual([]);
        expect(applyProblems(status(), f, ['relay', 'mqtt']).join()).toContain('not in the modules file: mqtt');
        const many = Array.from({length: APPLY_MAX + 1}, (_, index) => `m${index}`);
        const big = file(many.map((name) => ({name})));
        expect(applyProblems(status({free: 1 << 30}), big, many).join()).toContain(`the board takes 1..${APPLY_MAX}`);
        expect(applyProblems(status({free: 1 << 30}), big, many.slice(0, APPLY_MAX))).toEqual([]);
    });

    test('the file is for this head', () => {
        const data = new Array(48).fill(0);
        data[24] = 0xbb;
        const f = container([{name: 'relay', image: 4096, ram: 0, iram: 0, data}]);
        const md5 = 'bb' + '00'.repeat(15);
        expect(applyProblems(status({md5}), f, ['relay'])).toEqual([]);
        expect(applyProblems(status({md5: 'cc' + '00'.repeat(15)}), f, ['relay']).join()).toContain('another head build');
        // a new head goes on first: its own /modules is checked then
        expect(applyProblems(status({md5: 'cc' + '00'.repeat(15)}), f, ['relay'], false)).toEqual([]);
    });

    test('the flash: free plus what the installed modules take, the data sectors count', () => {
        // webui 50000 installed (52 KB in sectors) + 300000 free
        const fits = Math.floor((300000 + sectors(50000)) / 4096) * 4096;
        expect(applyProblems(status(), file([{name: 'a', image: fits}]), ['a'])).toEqual([]);
        expect(applyProblems(status(), file([{name: 'a', image: fits + 1}]), ['a']).join()).toContain('not enough flash');
        expect(applyProblems(status(), file([{name: 'a', image: fits, flags: 1 << 8}]), ['a']).join())
            .toContain('not enough flash');
    });

    test('the RAM: the IRAM code in the fixed pool, the statics in the pool and the heap block', () => {
        // iram_data 17484 + ram_pool 5120
        expect(applyProblems(status(), file([{name: 'a', ram: 22000}]), ['a'])).toEqual([]);
        expect(applyProblems(status(), file([{name: 'a', ram: 23000}]), ['a']).join()).toContain('not enough RAM');
        expect(applyProblems(status({iram_data: 0}), file([{name: 'a', iram: 100}]), ['a']).join())
            .toContain('not enough IRAM');
    });

    test('a running module goes off without the file, on needs it; nothing while busy', () => {
        const running = new Set(['relay']);
        const row = (/** @type {string} */name, pkg = false) => /** @type {any} */({name, package: pkg ? {} : null});
        expect(canToggle(row('relay'), running, false)).toBe(true);
        expect(canToggle(row('mqtt'), running, false)).toBe(false);
        expect(canToggle(row('mqtt', true), running, false)).toBe(true);
        expect(canToggle(row('relay', true), running, true)).toBe(false);
    });

    test('the route of an Apply: off without a file when nothing is added', () => {
        const running = new Set(['relay', 'button', 'webui']);
        const packages = new Map([['relay', {}], ['button', {}]]);
        // webui is not in the file: switching button off goes without it
        expect(applyRoute(['relay', 'webui'], running, /** @type {any} */(packages))).toBe('off');
        expect(applyRoute(['relay', 'webui'], running, null)).toBe('off');
        expect(applyRoute(['relay'], new Set(['relay', 'button']), /** @type {any} */(packages))).toBe('image');
        expect(applyRoute(['relay', 'mqtt'], running, null)).toBe('file');
        expect(applyRoute([], running, null)).toBe('none');
    });

    test('"Applied" only when every chosen module runs', () => {
        const after = {modules: [
            {name: 'relay', state: 'running'}, {name: 'button', state: 'not started'}]};
        expect(notRunning(['relay'], /** @type {any} */(after))).toEqual([]);
        expect(notRunning(['relay', 'button', 'mqtt'], /** @type {any} */(after)))
            .toEqual(['button (not started)', 'mqtt (missing)']);
    });
});
