import { listenAppConnected } from './connection.mjs';
import { styleInject } from './core.mjs';

// <link> stylesheets are not stripped per-module by the build, keep the rules
// here so that only the variant(s) built with MODULE_NSPANEL carry them
const STYLE = [
    `.nspanel-editor { font-family: 'Courier New', monospace; font-size: 85%; line-height: 1.3;
        height: 480px; resize: vertical; tab-size: 2; white-space: pre; }`,
    ".nspanel-buttons .pure-button { margin-right: 6px; }",
    ".nspanel-result { margin: 0; white-space: pre-wrap; word-break: break-word; }",
    ".nspanel-result.nspanel-ok { color: #1b7f1b; }",
    ".nspanel-result.nspanel-error { color: #c0392b; }",
    ".nspanel-card { border: 1px solid #ddd; border-radius: 6px; padding: 8px; margin: 0 0 12px; max-width: 520px; }",
    ".nspanel-card-title { font-weight: bold; margin: 0 0 6px; }",
    ".nspanel-card.nspanel-nested .nspanel-card-title { font-weight: normal; font-style: italic; }",
    ".nspanel-grid { display: grid; gap: 6px; grid-template-columns: repeat(4, 1fr); }",
    ".nspanel-grid.nspanel-shortcuts { grid-template-columns: repeat(7, 1fr); }",
    `.nspanel-tile { background: #f2f2f2; border-radius: 6px; padding: 6px 4px; text-align: center;
        font-size: 12px; min-height: 48px; overflow: hidden; word-break: break-word; }`,
    ".nspanel-tile.nspanel-bad { outline: 1px solid #c0392b; }",
    ".nspanel-tile-icon { color: #777; font-size: 10px; }",
    ".nspanel-tile-name { margin-top: 3px; }",
    ".nspanel-tile-type { color: #1f8dd6; font-size: 10px; }",
    ".nspanel-tile.nspanel-empty { background: transparent; border: 1px dashed #e4e4e4; }",
    ".nspanel-rows { display: flex; flex-direction: column; gap: 4px; }",
    `.nspanel-row { display: flex; gap: 8px; align-items: baseline; background: #f2f2f2; border-radius: 6px;
        padding: 6px 8px; font-size: 12px; }`,
    ".nspanel-row.nspanel-bad { outline: 1px solid #c0392b; }",
    ".nspanel-row .nspanel-tile-icon { flex: 0 0 25%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }",
    ".nspanel-row .nspanel-tile-name { flex: 1 1 auto; margin: 0; }",
];

// tiles per screen, OHpanel and Lovelace TFT >= 51 (a 4x2 grid of slots)
const PREVIEW_CAPACITY = 8;

// OHpanel custom TFT design with a page per grid layout (buttons33 / buttons32 / buttons22 / buttons12)
export const DESIGN_GRIDS = "d3731fbb";

// OHpanel designs that show every layout in full: the custom one on its pages, the stock ones
// (v0.9.2, v0.10.0-rc1) drawn at runtime on `buttons`
export const DESIGNS_FULL_GRIDS = [DESIGN_GRIDS, "bc629e1e", "4508cd3f"];

/**
 * "layout" of a grid page: [columns, rows]
 * @type {{[k: string]: [number, number]}}
 */
export const GRIDS = {
    "4x2": [4, 2],
    "3x3": [3, 3],
    "3x2": [3, 2],
    "2x2": [2, 2],
    "1x2": [1, 2],
};

/** preview full grids (OHpanel custom pages or drawn tiles), until the status names an unknown design */
let __grids = true;

// rows per sheet of a "type": "list" page on the OHpanel entities_rows page
export const LIST_ROWS = 7;

/**
 * device endpoints, relative to the page unless the connection says otherwise (?host=...)
 * @typedef {{config: URL, status: URL}} NspanelUrls
 */

/** @type {NspanelUrls | null} */
let __urls = null;

let __loaded = false;

/** @type {{[k in keyof NspanelUrls]: string}} */
const RELATIVE = {
    config: "nspanel.json",
    status: "nspanel-status",
};

/**
 * @param {keyof NspanelUrls} name
 * @returns {string}
 */
function endpoint(name) {
    return __urls ? __urls[name].href : RELATIVE[name];
}

/** @type {RequestInit} */
const FETCH_OPTIONS = {
    credentials: "same-origin",
    cache: "no-store",
};

/**
 * 1-based line and column of the offset in text
 * @param {string} text
 * @param {number} offset
 * @returns {{line: number, column: number}}
 */
export function offsetToLineColumn(text, offset) {
    const before = text.slice(0, Math.max(0, offset));
    const lines = before.split("\n");

    return {
        line: lines.length,
        column: (lines[lines.length - 1] ?? "").length + 1,
    };
}

/**
 * @param {string} text
 * @param {number} line
 * @param {number} column
 * @returns {number}
 */
function lineColumnToOffset(text, line, column) {
    let offset = 0;
    for (let current = 1; current < line; ++current) {
        const next = text.indexOf("\n", offset);
        if (next < 0) {
            return text.length;
        }

        offset = next + 1;
    }

    return Math.min(text.length, offset + Math.max(0, column - 1));
}

/**
 * offset of the first syntax error, for messages w/o position
 * (V8 'Unexpected token' only quotes the surrounding text)
 * @param {string} text
 * @returns {number}
 */
export function jsonErrorOffset(text) {
    let pos = 0;

    const fail = () => {
        throw pos;
    };

    const ws = () => {
        while ((pos < text.length) && " \t\n\r".includes(text[pos])) {
            ++pos;
        }
    };

    /** @param {string} expected */
    const take = (expected) => {
        if (text[pos] !== expected) {
            fail();
        }
        ++pos;
    };

    const string = () => {
        take('"');
        while (pos < text.length) {
            const c = text.charCodeAt(pos);
            if (c === 0x22) {
                ++pos;
                return;
            }
            if (c < 0x20) {
                fail();
            }
            if (c === 0x5c) {
                ++pos;
                if (text[pos] === "u") {
                    if (!/^[0-9a-fA-F]{4}$/.test(text.slice(pos + 1, pos + 5))) {
                        fail();
                    }
                    pos += 4;
                } else if (!"\"\\/bfnrt".includes(text[pos] ?? "?")) {
                    fail();
                }
            }
            ++pos;
        }
        fail();
    };

    /** @returns {void} */
    const value = () => {
        ws();
        const c = text[pos];
        if (c === "{" || c === "[") {
            const close = (c === "{") ? "}" : "]";
            ++pos;
            ws();
            if (text[pos] === close) {
                ++pos;
                return;
            }
            for (;;) {
                if (c === "{") {
                    ws();
                    string();
                    ws();
                    take(":");
                }
                value();
                ws();
                if (text[pos] === ",") {
                    ++pos;
                    continue;
                }
                take(close);
                return;
            }
        }
        if (c === '"') {
            string();
            return;
        }
        const literal = text.slice(pos).match(/^(?:true|false|null|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?)/);
        if (!literal) {
            fail();
        } else {
            pos += literal[0].length;
        }
    };

    try {
        value();
        ws();
        if (pos < text.length) {
            fail();
        }
    } catch (e) {
        if (typeof e === "number") {
            return e;
        }
        throw e;
    }

    return -1;
}

/**
 * @typedef {{ok: true} | {ok: false, message: string, line?: number, column?: number, offset?: number}} ValidateResult
 */

/**
 * JSON syntax check, error position from either browser message format
 * - Chrome / Node: '... in JSON at position 42 (line 3 column 5)' or only '... at position 42'
 * - Firefox: '... at line 3 column 5 of the JSON data'
 * @param {string} text
 * @returns {ValidateResult}
 */
export function validateJson(text) {
    if (!text.trim().length) {
        return {ok: false, message: "empty configuration"};
    }

    try {
        const parsed = JSON.parse(text);
        if ((parsed === null) || (typeof parsed !== "object") || Array.isArray(parsed)) {
            return {ok: false, message: "top level value must be a JSON object"};
        }
    } catch (e) {
        const message = (e instanceof Error) ? e.message : String(e);

        const lineColumn = message.match(/line (\d+) column (\d+)/);
        if (lineColumn) {
            const line = parseInt(lineColumn[1], 10);
            const column = parseInt(lineColumn[2], 10);
            return {
                ok: false, message, line, column,
                offset: lineColumnToOffset(text, line, column),
            };
        }

        const position = message.match(/position (\d+)/);
        if (position) {
            const offset = parseInt(position[1], 10);
            return {ok: false, message, offset, ...offsetToLineColumn(text, offset)};
        }

        let offset = jsonErrorOffset(text);
        if (offset < 0) {
            offset = text.length;
        }

        return {ok: false, message, offset, ...offsetToLineColumn(text, offset)};
    }

    return {ok: true};
}

/**
 * @param {ValidateResult} result
 * @returns {string}
 */
export function formatValidateResult(result) {
    if (result.ok) {
        return "JSON is valid";
    }

    if ((result.line !== undefined) && (result.column !== undefined)) {
        return `Line ${result.line}, column ${result.column}: ${result.message}`;
    }

    return result.message;
}

/**
 * list: rows of a "type": "list" page; grid: the page's "layout" ("4x2", "3x3", "3x2", "2x2", "1x2");
 * depth: 0 for the swipe row (top-level "pages"), 1+ for a nested page (an item's "page", opened by its tile)
 * @typedef {{title: string, items: any[], list: boolean, grid: string, depth: number}} PreviewCard
 */

/**
 * an item's nested page ("page": {...}), null when it has none
 * @param {any} item
 * @returns {any}
 */
export function nestedPage(item) {
    const page = item?.page;
    return ((typeof page === "object") && (page !== null) && !Array.isArray(page)) ? page : null;
}

/**
 * the name a tile shows: its "name", else the title of the page it opens
 * @param {any} item
 * @returns {string}
 */
export function itemName(item) {
    if (typeof item === "string") {
        return item;
    }
    const name = String(item?.name ?? "");
    return name.length ? name : String(nestedPage(item)?.title ?? "");
}

/**
 * "layout" as the firmware takes it, else "columns": 3 = "3x2", else "4x2"
 * @param {any} page
 * @returns {string}
 */
export function pageGrid(page) {
    const layout = page?.layout;
    if ((typeof layout === "string") && Object.prototype.hasOwnProperty.call(GRIDS, layout)) {
        return layout;
    }

    return (page?.columns === 3) ? "3x2" : "4x2";
}

/**
 * "per_screen" as the firmware takes it: 1..9, anything else is 0 (as many as fit)
 * @param {any} page
 * @returns {number}
 */
function pagePerScreen(page) {
    const value = page?.per_screen;
    return (Number.isInteger(value) && (value >= 1) && (value <= 9)) ? value : 0;
}

/**
 * tiles or rows of a page on one screen, as nspanel.cpp gridCapacity() / buildCards()
 * - grids (OHpanel, known design): columns x rows tiles (own page or drawn)
 * - else the layout's columns of the 4x2 slots, at most the driver's capacity
 * @param {any} page
 * @param {number} capacity
 * @param {boolean} list
 * @param {boolean} grids
 * @returns {number}
 */
export function perScreen(page, capacity = PREVIEW_CAPACITY, list = false, grids = false) {
    const per = pagePerScreen(page);
    if (list) {
        return per ? Math.min(per, LIST_ROWS) : LIST_ROWS;
    }

    const [columns, rows] = GRIDS[pageGrid(page)];
    let out = grids ? (columns * rows) : Math.min(capacity, 2 * columns);
    if (per) {
        out = Math.min(out, per);
    }
    return Math.max(out, 1);
}

/**
 * screens as the panel shows them, long pages split into 'Title 1/2', 'Title 2/2'; the nested pages
 * of a page's tiles follow its sheets (depth + 1), they are not in the swipe row
 * @param {any} config
 * @param {number} capacity tiles of a full 4x2 grid screen
 * @param {boolean} lists "type": "list" pages as rows (OHpanel); false: as grids (Lovelace)
 * @param {boolean} grids the TFT has a page per layout (OHpanel custom design)
 * @returns {PreviewCard[]}
 */
export function previewCards(config, capacity = PREVIEW_CAPACITY, lists = true, grids = false) {
    /** @type {PreviewCard[]} */
    const out = [];

    /**
     * @param {any} page
     * @param {number} depth
     * @param {string} fallback title when the page has none (the name of its tile)
     */
    const add = (page, depth, fallback) => {
        const title = String(page?.title ?? "") || fallback;
        if (page?.type === "settings") {
            out.push({title: title || "Settings", items: [], list: false, grid: "4x2", depth});
            return;
        }

        const list = lists && (page?.type === "list");
        const per = perScreen(page, capacity, list, grids);
        const grid = list ? "4x2" : pageGrid(page);
        const items = Array.isArray(page?.items) ? page.items : [];
        const count = Math.max(1, Math.ceil(items.length / per));
        for (let index = 0; index < count; ++index) {
            out.push({
                title: (count > 1) ? `${title} ${index + 1}/${count}` : title,
                items: items.slice(index * per, (index + 1) * per),
                list,
                grid,
                depth,
            });
        }

        if (depth < 8) {
            for (const item of items) {
                const nested = nestedPage(item);
                if (nested) {
                    add(nested, depth + 1, String(item?.name ?? ""));
                }
            }
        }
    };

    const pages = Array.isArray(config?.pages) ? config.pages : [];
    for (const page of pages) {
        add(page, 0, "");
    }

    return out;
}

/**
 * titles of the swipe row after home, as the panel swipes them (nested pages are not in it)
 * @param {any} config
 * @param {number} capacity
 * @param {boolean} lists
 * @param {boolean} grids
 * @returns {string[]}
 */
export function previewRow(config, capacity = PREVIEW_CAPACITY, lists = true, grids = false) {
    return previewCards(config, capacity, lists, grids)
        .filter((card) => !card.depth)
        .map((card) => card.title);
}

/**
 * the tile slots of a grid card, null where no tile is shown, and the columns they are drawn in
 * - grids (OHpanel, known design): columns x rows slots (own page or drawn)
 * - else the 4x2 slots of the `buttons` page (tile1..tile8), the layout's columns of each row:
 *   3x3 / 3x2 fill slots 1-3 and 5-7, 2x2 slots 1, 2, 5, 6, 1x2 slots 1 and 5
 * @param {PreviewCard} card
 * @param {boolean} grids
 * @returns {{columns: number, slots: any[]}}
 */
export function previewGrid(card, grids = false) {
    const [columns, rows] = GRIDS[card.grid] ?? GRIDS["4x2"];
    if (grids) {
        /** @type {any[]} */
        const slots = Array(columns * rows).fill(null);
        card.items.slice(0, slots.length)
            .forEach((item, index) => {
                slots[index] = item;
            });
        return {columns, slots};
    }

    /** @type {any[]} */
    const slots = Array(PREVIEW_CAPACITY).fill(null);
    card.items.slice(0, 2 * columns)
        .forEach((item, index) => {
            slots[(Math.floor(index / columns) * 4) + (index % columns)] = item;
        });

    return {columns: 4, slots};
}

/**
 * the 4x2 tile slots of a grid card on the stock OHpanel design
 * @param {PreviewCard} card
 * @returns {any[]}
 */
export function previewSlots(card) {
    return previewGrid(card, false).slots;
}

/**
 * what a tile does, for the tooltip and the error outline
 * @param {any} item
 * @param {any} groups
 * @returns {{type: string, target: string, bad: boolean}}
 */
export function previewTarget(item, groups) {
    if (typeof item === "string") {
        return {type: "ref", target: item, bad: false};
    }

    const type = String(item?.type ?? "switch");
    if (type === "scene") {
        const group = String(item?.group ?? "");
        return {
            type,
            target: `group ${group} ${item?.action ?? "toggle"}`,
            bad: !groups || !(group in groups),
        };
    }

    if (type === "thermostat") {
        return {type, target: String(item?.room ?? item?.topic ?? ""), bad: !item?.topic};
    }

    const nested = nestedPage(item);
    if (nested) {
        return {type: "page", target: `page ${itemName(item)}`, bad: !Array.isArray(nested.items)};
    }

    if (item?.page !== undefined) {
        return {type: "page", target: "\"page\" must be an object", bad: true};
    }

    if (item?.open !== undefined) {
        return {type: "open", target: `open ${item.open}`, bad: false};
    }

    if (item?.relay !== undefined) {
        return {type, target: `relay ${item.relay}`, bad: false};
    }

    const topic = String(item?.topic ?? "");
    return {type, target: topic, bad: !topic.length};
}

/**
 * @param {any} item
 * @param {any} groups
 * @param {boolean} row
 * @returns {HTMLElement}
 */
function previewTile(item, groups, row = false) {
    const tile = document.createElement("div");
    if (item === null) {
        tile.classList.add("nspanel-tile", "nspanel-empty");
        return tile;
    }

    const what = previewTarget(item, groups);
    tile.classList.add(row ? "nspanel-row" : "nspanel-tile");
    tile.classList.toggle("nspanel-bad", what.bad);
    tile.title = what.target;

    const icon = document.createElement("div");
    icon.classList.add("nspanel-tile-icon");
    icon.textContent = (typeof item === "string") ? "" : String(item?.icon ?? "");

    const name = document.createElement("div");
    name.classList.add("nspanel-tile-name");
    name.textContent = itemName(item);

    const type = document.createElement("div");
    type.classList.add("nspanel-tile-type");
    type.textContent = what.type;

    tile.append(icon, name, type);
    return tile;
}

/**
 * @param {string} title
 * @param {any[]} items
 * @param {any} groups
 * @param {"grid" | "shortcuts" | "rows"} kind
 * @param {number} columns grid columns
 * @param {number} depth nested page depth: indented under its tile, "↳ Title"
 * @returns {HTMLElement}
 */
function previewCard(title, items, groups, kind = "grid", columns = 4, depth = 0) {
    const card = document.createElement("div");
    card.classList.add("nspanel-card");
    card.classList.toggle("nspanel-nested", depth > 0);
    if (depth > 0) {
        card.style.marginLeft = `${depth * 24}px`;
    }

    const head = document.createElement("div");
    head.classList.add("nspanel-card-title");
    head.textContent = (depth > 0) ? `\u21b3 ${title}` : title;

    const grid = document.createElement("div");
    grid.classList.add((kind === "rows") ? "nspanel-rows" : "nspanel-grid");
    grid.classList.toggle("nspanel-shortcuts", kind === "shortcuts");
    if ((kind === "grid") && (columns !== 4)) {
        grid.style.gridTemplateColumns = `repeat(${columns}, 1fr)`;
    }
    for (const item of items) {
        grid.appendChild(previewTile(item, groups, kind === "rows"));
    }

    card.append(head, grid);
    return card;
}

/** @param {string} text */
function renderPreview(text) {
    const area = document.querySelector("#panel-nspanel .nspanel-preview-area");
    if (!(area instanceof HTMLElement)) {
        return;
    }

    let config;
    try {
        config = JSON.parse(text);
    } catch (_) {
        return; // keep the last good preview while typing
    }

    const groups = config?.groups;
    const cards = [];

    const shortcuts = config?.screensaver?.shortcuts;
    if (Array.isArray(shortcuts) && shortcuts.length) {
        cards.push(previewCard("Home: shortcuts", shortcuts.slice(0, 7), groups, "shortcuts"));
    }

    for (const card of previewCards(config, PREVIEW_CAPACITY, true, __grids)) {
        if (card.list) {
            cards.push(previewCard(card.title, card.items, groups, "rows", 4, card.depth));
            continue;
        }

        const {columns, slots} = previewGrid(card, __grids);
        cards.push(previewCard(`${card.title} (${card.grid})`, slots, groups, "grid", columns, card.depth));
    }

    area.replaceChildren(...cards);
}

/** @type {ReturnType<typeof setTimeout> | null} */
let __previewTimer = null;

function onEditorInput() {
    if (__previewTimer !== null) {
        clearTimeout(__previewTimer);
    }

    __previewTimer = setTimeout(() => {
        __previewTimer = null;
        renderPreview(editor()?.value ?? "");
    }, 300);
}

/** @returns {HTMLTextAreaElement | null} */
function editor() {
    const out = document.getElementById("nspanel-config");
    return (out instanceof HTMLTextAreaElement) ? out : null;
}

/**
 * @param {string} text
 * @param {"ok" | "error" | ""} kind
 */
function showResult(text, kind = "") {
    const elem = document.querySelector("#panel-nspanel .nspanel-result");
    if (!(elem instanceof HTMLElement)) {
        return;
    }

    elem.textContent = text;
    elem.classList.toggle("nspanel-ok", kind === "ok");
    elem.classList.toggle("nspanel-error", kind === "error");
}

/** @param {boolean} value */
function setBusy(value) {
    document.querySelectorAll("#panel-nspanel button")
        .forEach((elem) => {
            if (elem instanceof HTMLButtonElement) {
                elem.disabled = value;
            }
        });
}

/**
 * @param {Response} response
 * @returns {Promise<string>}
 */
async function responseError(response) {
    const text = (await response.text()).trim();
    return `HTTP ${response.status}${text.length ? `: ${text}` : ` ${response.statusText}`}`;
}

/**
 * pretty-print when possible, device stores the file as uploaded
 * @param {string} text
 * @returns {string}
 */
function prettyJson(text) {
    try {
        return JSON.stringify(JSON.parse(text), null, 2);
    } catch (_) {
        return text;
    }
}

async function onLoad() {
    const textarea = editor();
    if (!textarea) {
        return;
    }

    setBusy(true);
    showResult("Loading...");

    try {
        const response = await fetch(endpoint("config"), FETCH_OPTIONS);
        if (!response.ok) {
            showResult(await responseError(response), "error");
            return;
        }

        textarea.value = prettyJson(await response.text());
        __loaded = true;
        renderPreview(textarea.value);
        showResult(`Loaded ${textarea.value.length} characters`, "ok");
    } catch (e) {
        showResult(`Load failed: ${(e instanceof Error) ? e.message : String(e)}`, "error");
    } finally {
        setBusy(false);
    }
}

/** @returns {boolean} */
function onValidate() {
    const textarea = editor();
    if (!textarea) {
        return false;
    }

    const result = validateJson(textarea.value);
    showResult(formatValidateResult(result), result.ok ? "ok" : "error");

    if (!result.ok && (result.offset !== undefined)) {
        textarea.focus();
        textarea.setSelectionRange(result.offset, Math.min(textarea.value.length, result.offset + 1));
    }

    return result.ok;
}

async function onSave() {
    const textarea = editor();
    if (!textarea || !onValidate()) {
        return;
    }

    setBusy(true);
    showResult("Saving...");

    try {
        const response = await fetch(endpoint("config"), {
            ...FETCH_OPTIONS,
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: textarea.value,
        });

        const text = (await response.text()).trim();
        if (response.ok) {
            showResult(text.length ? text : "OK", "ok");
            await updateStatus();
        } else {
            showResult(`HTTP ${response.status}: ${text.length ? text : response.statusText}`, "error");
        }
    } catch (e) {
        showResult(`Save failed: ${(e instanceof Error) ? e.message : String(e)}`, "error");
    } finally {
        setBusy(false);
    }
}

/**
 * @param {any} status
 * @returns {string}
 */
export function formatStatus(status) {
    if (!status || (typeof status !== "object")) {
        return "?";
    }

    const parts = [String(status.driver ?? "?")];
    if (status.display && (status.display !== status.driver)) {
        parts.push(`(config: ${status.display})`);
    }

    if (status.model) {
        parts.push(`model ${status.model}`);
    }

    if (status.version) {
        parts.push(`version ${status.version}`);
    }

    if (status.design) {
        parts.push(String(status.design));
    }

    parts.push(status.connected ? "connected" : "not connected");

    if (status.probing) {
        parts.push("probing");
    }

    if (status.flashing) {
        parts.push("flashing TFT");
    }

    return parts.join(", ");
}

/**
 * which design the preview draws: full grids (custom pages or drawn tiles) unless the panel reports an
 * unknown OHpanel design (the layouts then use the 4x2 slots)
 * @param {any} status
 * @param {boolean} current
 * @returns {boolean}
 */
export function previewDesignGrids(status, current = true) {
    if ((status?.driver !== "ohpanel") || !status?.version || (status.version === "unknown")) {
        return current;
    }

    return DESIGNS_FULL_GRIDS.includes(status.version);
}

async function updateStatus() {
    const elem = document.querySelector("#panel-nspanel .nspanel-status");
    if (!(elem instanceof HTMLElement)) {
        return;
    }

    try {
        const response = await fetch(endpoint("status"), FETCH_OPTIONS);
        if (!response.ok) {
            elem.textContent = await responseError(response);
            return;
        }

        const status = await response.json();
        elem.textContent = formatStatus(status);

        const grids = previewDesignGrids(status, __grids);
        if (grids !== __grids) {
            __grids = grids;
            renderPreview(editor()?.value ?? "");
        }
    } catch (e) {
        elem.textContent = `unavailable (${(e instanceof Error) ? e.message : String(e)})`;
    }
}

function onPanelShown() {
    updateStatus();
    if (!__loaded) {
        onLoad();
    }
}

/** @param {KeyboardEvent} event */
function onEditorKey(event) {
    const textarea = event.target;
    if (!(textarea instanceof HTMLTextAreaElement)) {
        return;
    }

    if ((event.key === "s") && (event.ctrlKey || event.metaKey)) {
        event.preventDefault();
        onSave();
        return;
    }

    if ((event.key !== "Tab") || event.shiftKey || event.ctrlKey || event.altKey || event.metaKey) {
        return;
    }

    event.preventDefault();

    const start = textarea.selectionStart;
    const end = textarea.selectionEnd;
    textarea.setRangeText("  ", start, end, "end");
}

export function init() {
    styleInject(STYLE);

    listenAppConnected((urls) => {
        __urls = {
            config: new URL(RELATIVE.config, urls.config),
            status: new URL(RELATIVE.status, urls.config),
        };
    });

    document.querySelector(".button-nspanel-load")
        ?.addEventListener("click", onLoad);
    document.querySelector(".button-nspanel-validate")
        ?.addEventListener("click", onValidate);
    document.querySelector(".button-nspanel-save")
        ?.addEventListener("click", onSave);

    editor()?.addEventListener("keydown", onEditorKey);
    editor()?.addEventListener("input", onEditorInput);

    document.querySelectorAll("[data-panel='nspanel']")
        .forEach((elem) => {
            elem.addEventListener("click", onPanelShown);
        });
}
