import { describe, expect, test } from 'vitest';
import {
    blockedBy,
    conflictsOf,
    dependentsOf,
    groupCounts,
    groupOf,
    switchOnOrder,
    meterLevel,
    meters,
    moduleRows,
    parseContainer,
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
        ram_arena: 5120, ram_used: 1000, ram_max: 4120,
        iram_arena: 2048, iram_used: 0, iram_max: 2048,
        iram_data: 17484, iram_data_used: 0, iram_data_max: 17484,
        heap: 11000, heap_boot: 30000,
        ...extra,
    };
}

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

    // the full image: an ESP image at 0x0, the same modules file behind 1 MB
    const inner = container([{name: 'rf', image: 16000, ram: 1300, iram: 64, data: [7, 8, 9]}]);
    const full = new Uint8Array(0x100000 + inner.byteLength).fill(0xff);
    full[0] = 0xe9;
    full.set(new Uint8Array(inner), 0x100000);
    const fromFull = parseContainer(full.buffer);
    expect([...fromFull.keys()]).toEqual(['rf']);
    expect([...(fromFull.get('rf')?.data ?? [])]).toEqual([7, 8, 9]);

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

    expect(meterLevel({title: '', total: 100, head: 0, used: 50, added: 0, legend: ''})).toBe('');
    expect(meterLevel({title: '', total: 100, head: 0, used: 85, added: 0, legend: ''})).toBe('mod-tight');
    expect(meterLevel({title: '', total: 100, head: 0, used: 90, added: 20, legend: ''})).toBe('mod-over');
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
    const { otaParts, modulesFileOf, selectionAfter, runningSet, OTA_MODULES } = await import('../src/modules.mjs');

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
    });

    test('not an install image: the buffer as it is', () => {
        const plain = new Uint8Array([0x45, 0x4d, 0x43, 0x32]).buffer;
        expect(otaParts(plain)).toBe(null);
        expect(modulesFileOf(plain)).toBe(plain);
    });

    test('ticking a module ticks what it needs, unticking unticks what needs it', () => {
        const status = {modules: [{name: 'mqtt', enabled: 1, requires: ''}, {name: 'captive', enabled: 1, requires: ''}]};
        const packages = new Map([
            ['domoticz', {requires: ['mqtt']}],
            ['homeassistant', {requires: ['mqtt']}],
        ]);
        const now = runningSet(status);
        expect([...now].sort()).toEqual(['captive', 'mqtt']);

        const on = selectionAfter(new Set(['captive']), 'domoticz', true, status, packages);
        expect([...on].sort()).toEqual(['captive', 'domoticz', 'mqtt']);

        const more = selectionAfter(on, 'homeassistant', true, status, packages);
        const off = selectionAfter(more, 'mqtt', false, status, packages);
        expect([...off]).toEqual(['captive']);
    });
});

test('matter unticks mdns on the ESP32 and the Beken, not on the ESP8266', () => {
    const beken = status({image: 'beken'});
    expect(selectionAfter(new Set(['mdns', 'mqtt']), 'matter', true, beken, new Map())).toEqual(new Set(['mqtt', 'matter']));
    expect(blockedBy('mdns', new Set(['matter']), beken)).toEqual(['matter']);
    expect(blockedBy('mdns', new Set(['matter']), status())).toEqual([]);
    expect(conflictsOf('matter', status())).toEqual([]);
});
