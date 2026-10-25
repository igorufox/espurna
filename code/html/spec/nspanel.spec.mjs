import { expect, test } from 'vitest';

import {
    formatStatus,
    formatValidateResult,
    jsonErrorOffset,
    offsetToLineColumn,
    pageGrid,
    perScreen,
    previewCards,
    previewDesignGrids,
    previewGrid,
    previewSlots,
    previewTarget,
    validateJson,
} from '../src/nspanel.mjs';

test('preview splits long pages like the panel', () => {
    const items = Array.from({length: 9}, (_, index) => ({name: `t${index}`, topic: `x/${index}`}));
    const cards = previewCards({pages: [
        {title: 'Heating', items},
        {title: 'Short', items: items.slice(0, 2)},
        {title: 'Setup', type: 'settings'},
    ]});

    expect(cards.map((card) => card.title))
        .toEqual(['Heating 1/2', 'Heating 2/2', 'Short', 'Setup']);
    expect(cards[0].items.length).toEqual(8);
    expect(cards[1].items.length).toEqual(1);
});

test('preview: 3 columns put 6 tiles a screen in slots 1-3 and 5-7', () => {
    const rooms = Array.from({length: 9}, (_, index) => ({name: `r${index}`, type: 'thermostat', topic: 't'}));
    const cards = previewCards({pages: [{title: 'Heating', columns: 3, items: rooms}]});

    expect(cards.map((card) => card.title)).toEqual(['Heating 1/2', 'Heating 2/2']);
    expect(cards.map((card) => card.items.length)).toEqual([6, 3]);
    expect(previewSlots(cards[0]).map((item) => item?.name ?? null))
        .toEqual(['r0', 'r1', 'r2', null, 'r3', 'r4', 'r5', null]);
    expect(previewSlots(cards[1]).map((item) => item?.name ?? null))
        .toEqual(['r6', 'r7', 'r8', null, null, null, null, null]);
});

test('preview: per_screen splits explicitly, bad values are ignored', () => {
    const items = Array.from({length: 9}, (_, index) => ({name: `t${index}`, topic: `x/${index}`}));
    expect(perScreen({per_screen: 4})).toEqual(4);
    expect(perScreen({columns: 3, per_screen: 8})).toEqual(6);
    expect(perScreen({columns: 3, per_screen: 2})).toEqual(2);
    expect(perScreen({per_screen: 0})).toEqual(8);
    expect(perScreen({per_screen: 9, columns: 5})).toEqual(8);
    expect(perScreen({}, 6)).toEqual(6);

    const cards = previewCards({pages: [{title: 'P', per_screen: 4, items}]});
    expect(cards.map((card) => card.items.length)).toEqual([4, 4, 1]);
    expect(cards[2].title).toEqual('P 3/3');
    expect(previewSlots(cards[0]).map((item) => item?.name ?? null))
        .toEqual(['t0', 't1', 't2', 't3', null, null, null, null]);
});

test('preview: list pages are 7 rows a sheet, grids on Lovelace', () => {
    const items = Array.from({length: 9}, (_, index) => ({name: `t${index}`, topic: `x/${index}`}));
    const config = {pages: [{title: 'Yard', type: 'list', columns: 3, items}]};

    const cards = previewCards(config);
    expect(cards.map((card) => card.title)).toEqual(['Yard 1/2', 'Yard 2/2']);
    expect(cards.map((card) => card.items.length)).toEqual([7, 2]);
    expect(cards.every((card) => card.list)).toBe(true);
    expect(previewCards({pages: [{type: 'list', per_screen: 5, items}]}).map((card) => card.items.length))
        .toEqual([5, 4]);

    const lovelace = previewCards(config, 8, false);
    expect(lovelace.map((card) => card.list)).toEqual([false, false]);
    expect(lovelace.map((card) => card.items.length)).toEqual([6, 3]);
});

test('preview: layout picks the grid page of the custom TFT and its capacity', () => {
    const rooms = Array.from({length: 9}, (_, index) => ({name: `r${index}`, type: 'thermostat', topic: 't'}));
    /** @param {number} count */
    const tiles = (count) => rooms.slice(0, count);

    expect(pageGrid({layout: '3x3', columns: 4})).toEqual('3x3');
    expect(pageGrid({columns: 3})).toEqual('3x2');
    expect(pageGrid({layout: '5x5'})).toEqual('4x2');
    expect(['4x2', '3x3', '3x2', '2x2', '1x2'].map((layout) => perScreen({layout}, 8, false, true)))
        .toEqual([8, 9, 6, 4, 2]);
    expect(perScreen({layout: '3x3', per_screen: 7}, 8, false, true)).toEqual(7);
    expect(perScreen({layout: '2x2', per_screen: 9}, 8, false, true)).toEqual(4);

    const heating = previewCards({pages: [{title: 'Heating', layout: '3x3', items: rooms}]}, 8, true, true);
    expect(heating.map((card) => card.title)).toEqual(['Heating']);
    expect(previewGrid(heating[0], true)).toEqual({columns: 3, slots: rooms});

    const big = previewCards({pages: [{title: 'Big', layout: '2x2', items: tiles(5)}]}, 8, true, true);
    expect(big.map((card) => card.title)).toEqual(['Big 1/2', 'Big 2/2']);
    expect(previewGrid(big[1], true).columns).toEqual(2);
    expect(previewGrid(big[1], true).slots.map((item) => item?.name ?? null)).toEqual(['r4', null, null, null]);

    const wide = previewCards({pages: [{title: 'Wide', layout: '1x2', items: tiles(3)}]}, 8, true, true);
    expect(wide.map((card) => card.items.length)).toEqual([2, 1]);
    expect(previewGrid(wide[0], true)).toEqual({columns: 1, slots: tiles(2)});
});

test('preview: layouts on the stock TFT use the columns of the 4x2 slots', () => {
    const rooms = Array.from({length: 9}, (_, index) => ({name: `r${index}`, type: 'thermostat', topic: 't'}));
    /** @param {import('../src/nspanel.mjs').PreviewCard} card */
    const names = (card) => previewSlots(card).map((item) => item?.name ?? null);

    expect(['4x2', '3x3', '3x2', '2x2', '1x2'].map((layout) => perScreen({layout})))
        .toEqual([8, 6, 6, 4, 2]);

    const heating = previewCards({pages: [{title: 'Heating', layout: '3x3', items: rooms}]});
    expect(heating.map((card) => card.items.length)).toEqual([6, 3]);
    expect(names(heating[0])).toEqual(['r0', 'r1', 'r2', null, 'r3', 'r4', 'r5', null]);

    const big = previewCards({pages: [{title: 'Big', layout: '2x2', items: rooms.slice(0, 4)}]});
    expect(names(big[0])).toEqual(['r0', 'r1', null, null, 'r2', 'r3', null, null]);

    const wide = previewCards({pages: [{title: 'Wide', layout: '1x2', items: rooms.slice(0, 2)}]});
    expect(names(wide[0])).toEqual(['r0', null, null, null, 'r1', null, null, null]);
    expect(previewGrid(wide[0]).columns).toEqual(4);

    // Lovelace: no grid pages either, the same capacity
    expect(previewCards({pages: [{layout: '2x2', items: rooms}]}, 8, false).map((card) => card.items.length))
        .toEqual([4, 4, 1]);
});

test('preview follows the design the panel reports', () => {
    expect(previewDesignGrids({driver: 'ohpanel', version: 'd3731fbb'}, false)).toBe(true);
    expect(previewDesignGrids({driver: 'ohpanel', version: 'bc629e1e'}, true)).toBe(false);
    expect(previewDesignGrids({driver: 'ohpanel', version: 'unknown'}, true)).toBe(true);
    expect(previewDesignGrids({driver: 'lovelace', version: '53'}, true)).toBe(true);
    expect(previewDesignGrids(null, false)).toBe(false);
});

test('preview flags tiles without a target', () => {
    const groups = {house: []};
    expect(previewTarget({type: 'scene', group: 'house'}, groups).bad).toBe(false);
    expect(previewTarget({type: 'scene', group: 'nope'}, groups).bad).toBe(true);
    expect(previewTarget({name: 'x'}, groups).bad).toBe(true);
    expect(previewTarget({relay: 0}, groups).target).toEqual('relay 0');
    expect(previewTarget('Кухня', groups).type).toEqual('ref');
});

test('offset to line and column', () => {
    const text = '{\n  "a": 1,\n  "b": x\n}';
    expect(offsetToLineColumn(text, 0)).toEqual({line: 1, column: 1});
    expect(offsetToLineColumn(text, text.indexOf('x'))).toEqual({line: 3, column: 8});
});

test('valid json object', () => {
    const result = validateJson('{"pages": []}');
    expect(result.ok).toBeTruthy();
    expect(formatValidateResult(result)).toEqual('JSON is valid');
});

test('json must be an object', () => {
    for (const text of ['[]', '1', 'null', '   ']) {
        expect(validateJson(text).ok).toBeFalsy();
    }
});

test('syntax error has line and column', () => {
    const text = '{\n  "a": 1,\n  "b": x\n}';
    const result = validateJson(text);
    expect(result.ok).toBeFalsy();
    if (result.ok) {
        return;
    }

    expect(result.line).toEqual(3);
    expect(result.column).toEqual(8);
    expect(result.offset).toEqual(text.indexOf('x'));
    expect(formatValidateResult(result)).toMatch(/^Line 3, column 8: /);
});

test('truncated json points at the end', () => {
    const text = '{\n  "a": [1, 2';
    const result = validateJson(text);
    expect(result.ok).toBeFalsy();
    if (result.ok) {
        return;
    }

    expect(result.line).toEqual(2);
});

test('error offset scanner', () => {
    const valid = [
        '{}', '[]', '{"a": [1, -2.5e3, true, false, null, "x\\"\\u00e9"], "b": {"c": {}}}',
        ' \n{ "a" : 0 }\n ',
    ];
    for (const text of valid) {
        expect(jsonErrorOffset(text)).toEqual(-1);
        expect(() => JSON.parse(text)).not.toThrow();
    }

    /** @type {[string, number][]} */
    const invalid = [
        ['{"a": 1,}', 8],
        ['{"a" 1}', 5],
        ['{a: 1}', 1],
        ['[1 2]', 3],
        ['{"a": 01}', 7],
        ['{"a": "x\ny"}', 8],
        ['{"a": 1} x', 9],
        ['{"a": tru}', 6],
    ];
    for (const [text, offset] of invalid) {
        expect(() => JSON.parse(text)).toThrow();
        expect(jsonErrorOffset(text)).toEqual(offset);
    }
});

test('status line', () => {
    expect(formatStatus(null)).toEqual('?');
    expect(formatStatus({
        display: 'auto', driver: 'lovelace', connected: true,
        probing: false, version: '53', model: 'NSPanel', flashing: false,
    })).toEqual('lovelace, (config: auto), model NSPanel, version 53, connected');

    expect(formatStatus({
        display: 'auto', driver: 'ohpanel', connected: true, version: 'd3731fbb',
        design: 'custom ESPurna grids: layouts 4x2 3x3 3x2 2x2 1x2, background aurora only',
    })).toEqual('ohpanel, (config: auto), version d3731fbb, '
        + 'custom ESPurna grids: layouts 4x2 3x3 3x2 2x2 1x2, background aurora only, connected');
});
