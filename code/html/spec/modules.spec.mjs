import { expect, test } from 'vitest';
import {
    canMove,
    dependentsOf,
    groupCounts,
    groupOf,
    switchOnOrder,
    meterLevel,
    meters,
    moduleRows,
    parseContainer,
    sectors,
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
    expect(byName.sensors).toMatchObject({fits: true, short: [], ramIn: 'dram'});
    expect(byName.light).toMatchObject({fits: false, short: ['flash']});
    expect(byName.rf).toMatchObject({fits: false, short: ['RAM']});

    // its RAM in the IRAM tail: 5000 fits there
    const iram = moduleRows(status(), packages, new Map([['rf', 'iram']]));
    expect(iram.find((x) => x.name === 'rf')).toMatchObject({fits: true, ramIn: 'iram'});
});

test('a module that takes the RAM to 85 % and more is tight', () => {
    const packages = parseContainer(container([
        {name: 'sensors', image: 9000, ram: 3500, iram: 0, data: [0]},
    ]));

    const [row] = moduleRows(status({modules: []}), packages, new Map());
    expect(row).toMatchObject({fits: true, tight: true});
});

test('the RAM of a running module moves when a second copy fits next to the running one', () => {
    const packages = parseContainer(container([
        {name: 'sensors', image: 90000, ram: 2900, iram: 1100, data: [0]},
        {name: 'webui', image: 50000, ram: 0, iram: 0, data: [0]},
    ]));
    const installed = {name: 'sensors', size: 90000, offset: 0xE2000, ram: 2900, ram_in: 'dram', iram: 1100,
        state: 'running'};
    const board = status({modules: [installed], free_max: 180000});
    const row = (/** @type {import('../src/modules.mjs').Status} */ s) =>
        moduleRows(s, packages, new Map()).find((x) => x.name === 'sensors');

    expect(canMove(board, row(board))).toBe(true);
    // no gap for the second copy: off, restart, then on with the other choice
    const full = status({modules: [installed], free_max: 68000});
    expect(canMove(full, row(full))).toBe(false);
    // into DRAM: the RAM arena must hold it
    const inIram = status({modules: [{...installed, ram_in: 'iram'}], ram_max: 600});
    expect(canMove(inIram, row(inIram))).toBe(false);
    // a module without RAM has nothing to move, nor one without the modules file
    const webui = moduleRows(board, packages, new Map()).find((x) => x.name === 'webui');
    expect(canMove(board, webui)).toBe(false);
    expect(canMove(board, moduleRows(board, new Map(), new Map())[0])).toBe(false);
});

test('meters: flash counts the head and whole sectors, the preview adds the module that is off', () => {
    const packages = parseContainer(container([
        {name: 'sensors', image: 9000, ram: 1200, iram: 100, data: [0]},
    ]));
    const st = status();
    const rows = moduleRows(st, packages, new Map());
    const preview = rows.find((x) => x.name === 'sensors') ?? null;

    const [flash, ram, iram, data] = meters(st, preview);
    expect(flash).toMatchObject({head: 598016, used: sectors(50000), added: sectors(9000)});
    expect(ram).toMatchObject({used: 1000, added: 1200});
    expect(iram).toMatchObject({added: 100});
    expect(data).toMatchObject({title: 'RAM in IRAM', added: 0});

    expect(meters(status({iram_data: 0}), null)).toHaveLength(3);

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
