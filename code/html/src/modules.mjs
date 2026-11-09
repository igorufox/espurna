// MODULES panel: the parts of the ESP8266 1 MB firmware that are added to the free flash behind the head
// (docs/esp8266-modules.md). The board answers /modules with what it has; the modules themselves come from the modules
// file of the running release (espurna-esp8266-1m-modules.bin, from the disk or from the release list). Switching a
// module on uploads its package to /modules/upload, off: /modules/remove. Both apply at the next restart.
// The head has its own small page with the same switches (/modules.html), it works without this WebUI.

import { listenAppConnected } from './connection.mjs';
import { FIRMWARE_URL } from './fwinstall.mjs';
import { pageReloadIn, styleInject } from './core.mjs';

// <link> stylesheets are not stripped per-module by the build, keep the rules here
const STYLE = [
    "#panel-mod .mod-off { background: #fde2e1; border: 1px solid #d9534f; border-radius: 4px; padding: .8em 1em; margin-bottom: 1.5em; }",
    "#panel-mod .mod-meter { margin: 0 0 1.2em; }",
    "#panel-mod .mod-meter-title { display: flex; justify-content: space-between; align-items: baseline; gap: 1em; }",
    "#panel-mod .mod-meter-title strong { font-size: 105%; }",
    "#panel-mod .mod-meter-pct { color: #777; font-size: 90%; }",
    "#panel-mod .mod-meter-pct.mod-tight { color: #b07800; font-weight: bold; }",
    "#panel-mod .mod-meter-pct.mod-over { color: #c00000; font-weight: bold; }",
    "#panel-mod .mod-bar { display: flex; height: 1.4em; border-radius: 4px; overflow: hidden; background: #e8f4e8; margin: .35em 0 .3em; box-shadow: inset 0 0 0 1px rgba(0,0,0,.06); }",
    "#panel-mod .mod-bar i { display: block; height: 100%; transition: width .25s ease-in-out; }",
    "#panel-mod .mod-seg-head { background: #8090a8; }",
    "#panel-mod .mod-seg-mod { background: #1f8dd6; }",
    "#panel-mod .mod-tight .mod-seg-mod, #panel-mod .mod-seg-mod.mod-tight { background: #f0b429; }",
    "#panel-mod .mod-seg-new { background: repeating-linear-gradient(45deg, #1f8dd6 0 6px, #7fbfea 6px 12px); }",
    "#panel-mod .mod-seg-new.mod-tight { background: repeating-linear-gradient(45deg, #f0b429 0 6px, #f7d891 6px 12px); }",
    "#panel-mod .mod-seg-new.mod-over { background: repeating-linear-gradient(45deg, #c00000 0 6px, #e88 6px 12px); }",
    "#panel-mod .mod-meter-legend { font-size: 85%; color: #666; display: flex; flex-wrap: wrap; gap: .2em 1.2em; }",
    "#panel-mod .mod-meter-legend b { display: inline-block; width: .8em; height: .8em; border-radius: 2px; margin-right: .35em; vertical-align: -.05em; }",
    "#panel-mod .mod-cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(17em, 1fr)); gap: .8em; }",
    "#panel-mod .mod-card { border: 1px solid #ddd; border-left: 4px solid #ccc; border-radius: 4px; padding: .6em .8em; background: #fff; transition: box-shadow .2s; }",
    "#panel-mod .mod-card:hover { box-shadow: 0 2px 6px rgba(0,0,0,.12); }",
    "#panel-mod .mod-card.mod-on { border-left-color: #00c000; }",
    "#panel-mod .mod-card.mod-pending { border-left-color: #f0ad4e; }",
    "#panel-mod .mod-card.mod-cant { border-left-color: #c00000; background: #fcfcfc; }",
    "#panel-mod .mod-card-head { display: flex; flex-wrap: wrap; align-items: center; gap: .4em .6em; }",
    "#panel-mod .mod-card-head strong { font-size: 110%; flex: 1 0 auto; }",
    "#panel-mod .mod-card-head input.checkbox-toggle { margin: 0; }",
    "#panel-mod input.checkbox-toggle::before { content: 'OFF'; }",
    "#panel-mod input.checkbox-toggle:checked::before { content: 'ON'; }",
    "#panel-mod .mod-desc { color: #666; font-size: 88%; margin: .35em 0 .5em; min-height: 2.4em; }",
    "#panel-mod .mod-sizes { display: flex; flex-wrap: wrap; gap: .3em; align-items: center; font-size: 85%; }",
    "#panel-mod .mod-chip { background: #f0f0f0; border-radius: 1em; padding: .1em .6em; white-space: nowrap; }",
    "#panel-mod .mod-chip.mod-short { background: #fde2e1; color: #c00000; }",
    "#panel-mod .mod-sizes select { font-size: 100%; padding: 0 .2em; height: auto; width: auto; border-radius: 1em; }",
    "#panel-mod .mod-state { font-size: 78%; border-radius: 1em; padding: .1em .6em; background: #eee; color: #555; white-space: nowrap; }",
    "#panel-mod .mod-state-running { background: #dff3df; color: #007000; }",
    "#panel-mod .mod-state-pending { background: #fcefd9; color: #9a6000; }",
    "#panel-mod .mod-state-bad { background: #fde2e1; color: #c00000; }",
    "#panel-mod .mod-state-tight { background: #fcefd9; color: #b07800; }",
    "#panel-mod .mod-source { display: flex; flex-wrap: wrap; gap: .6em 1em; align-items: center; margin: 1em 0 .4em; }",
    "#panel-mod .mod-source input[type=file] { max-width: 100%; }",
    "#panel-mod .mod-message { font-size: 88%; color: #555; min-height: 1.3em; }",
    "#panel-mod .mod-message.mod-error { color: #c00000; }",
    "#panel-mod .mod-why { font-size: 88%; color: #c00000; margin-top: .4em; }",
    "#panel-mod .mod-restart { display: none; align-items: center; gap: 1em; flex-wrap: wrap; background: #fcefd9; border: 1px solid #f0ad4e; border-radius: 4px; padding: .6em 1em; margin-top: 1em; }",
    "#panel-mod .mod-restart.mod-show { display: flex; }",
    "#panel-mod .mod-restart .pure-button { margin: 0; }",
    "#panel-mod .button-mod-restart, #panel-mod .button-mod-on { background: rgb(192, 0, 0); }",
    "#panel-mod .button-mod-github { background: rgb(0, 192, 0); }",
    "#panel-mod .button-mod-refresh { background: rgb(31, 141, 214); }",
    "#panel-mod .mod-source .pure-button { margin: 0; }",
];

// What a module brings, for the cards; a module that is not here shows its name only
const DESCRIPTIONS = new Map([
    ["webui", "This web interface. Off: the board shows its small modules page instead."],
    ["sensors", "Sensor drivers (temperature, power meters, analog, I2C, 1-Wire) and the SENSORS panel."],
    ["light", "Lights: PWM channels, colour and brightness, the LIGHTS panel."],
    ["rf", "RF bridge: learn and send 433 MHz codes, the RFBRIDGE panel."],
    ["scheduler", "Timers that switch the relays and lights at set times, the SCHEDULE panel."],
    ["homeassistant", "Home Assistant MQTT discovery, the HASS panel."],
    ["alexa", "Lets a smart speaker hub find and switch the relays (fauxmo)."],
    ["domoticz", "Domoticz MQTT, the DOMOTICZ panel."],
    ["thingspeak", "Sends relays and sensors to ThingSpeak, the THINGSPEAK panel."],
    ["mdns", "Answers <hostname>.local on the network (mDNS)."],
]);

// From this share on a meter is yellow: everything is taken, but the board still lives
export const TIGHT = 0.85;
const SECTOR = 4096;
const CONTAINER_MAGIC = 0x31434d45; // "EMC1"
const FULL_MODULES = 0x100000;

/** @typedef {{name: string, size: number, offset: number, ram: number, ram_in: string, iram: number, state: string}} Installed */
/** @typedef {{image: number, ram: number, iram: number, data: Uint8Array}} Package */

/**
 * @typedef {{
 *   version: string, image: string, list: string, off: string, modules: Installed[],
 *   area: number, head: number, free: number, free_max: number,
 *   ram_arena: number, ram_used: number, ram_max: number,
 *   iram_arena: number, iram_used: number, iram_max: number,
 *   iram_data: number, iram_data_used: number, iram_data_max: number,
 *   heap: number, heap_boot: number, chip?: number, file?: number,
 * }} Status
 */

/**
 * @param {number} size
 * @returns {number}
 */
export function sectors(size) {
    return Math.ceil(size / SECTOR) * SECTOR;
}

/**
 * @param {number} size
 * @returns {string}
 */
export function sizeText(size) {
    if (size < 1024) {
        return `${size} B`;
    }

    return `${(size / 1024).toFixed(1)} KB`;
}

/**
 * The modules file (EMC1): magic, count, 48-byte entries (name16, offset, size, image, ram, iram, 12 reserved), then
 * the packages
 * @param {ArrayBuffer} buffer
 * @returns {Map<string, Package>}
 */
export function parseContainer(buffer) {
    const view = new DataView(buffer);

    // the full image (espurna-esp8266-1m-full.bin): an ESP image at 0x0, the modules file behind its first 1 MB
    let base = 0;
    if ((buffer.byteLength > FULL_MODULES + 8) && (view.getUint8(0) === 0xE9)
        && (view.getUint32(FULL_MODULES, true) === CONTAINER_MAGIC))
    {
        base = FULL_MODULES;
    }

    if ((buffer.byteLength < base + 8) || (view.getUint32(base, true) !== CONTAINER_MAGIC)) {
        throw new Error("not a modules file (or the full image)");
    }

    const out = new Map();
    const count = view.getUint32(base + 4, true);
    for (let index = 0; index < count; ++index) {
        const at = base + 8 + (48 * index);
        if (at + 48 > buffer.byteLength) {
            throw new Error("the modules file is cut short");
        }

        let name = "";
        for (let offset = 0; offset < 16; ++offset) {
            const code = view.getUint8(at + offset);
            if (!code) {
                break;
            }
            name += String.fromCharCode(code);
        }

        const offset = base + view.getUint32(at + 16, true);
        const size = view.getUint32(at + 20, true);
        if (offset + size > buffer.byteLength) {
            throw new Error(`${name}: the modules file is cut short`);
        }

        out.set(name, {
            image: view.getUint32(at + 24, true),
            ram: view.getUint32(at + 28, true),
            iram: view.getUint32(at + 32, true),
            data: new Uint8Array(buffer, offset, size),
        });
    }

    return out;
}

/**
 * @typedef {{
 *   name: string, installed: Installed | null, package: Package | null,
 *   flash: number, ram: number, iram: number, ramIn: string,
 *   fits: boolean, tight: boolean, short: string[],
 * }} Row
 */

/**
 * One row per module, installed or in the modules file: what it takes and whether it fits into what is free now
 * @param {Status} status
 * @param {Map<string, Package>} packages
 * @param {Map<string, string>} ramIn where the RAM of a module that is not installed yet goes, 'dram' or 'iram'
 * @returns {Row[]}
 */
export function moduleRows(status, packages, ramIn) {
    const names = new Set([
        ...status.modules.map((x) => x.name),
        ...packages.keys()]);

    return [...names].sort().map((name) => {
        const installed = status.modules.find((x) => x.name === name) ?? null;
        const pkg = packages.get(name) ?? null;
        if (installed) {
            return {
                name, installed, package: pkg,
                flash: installed.size, ram: installed.ram, iram: installed.iram,
                ramIn: installed.ram_in || "dram",
                fits: true, tight: false, short: [],
            };
        }

        const where = (status.iram_data && pkg && pkg.ram && (ramIn.get(name) === "iram")) ? "iram" : "dram";
        const flash = pkg ? pkg.image : 0;
        const ram = pkg ? pkg.ram : 0;
        const iram = pkg ? pkg.iram : 0;

        /** @type {string[]} */
        const short = [];
        if (sectors(flash) > status.free_max) {
            short.push("flash");
        }
        if ((where === "iram") ? (ram > (status.iram_data_max || 0)) : (ram > status.ram_max)) {
            short.push((where === "iram") ? "RAM in IRAM" : "RAM");
        }
        if (iram > status.iram_max) {
            short.push("IRAM");
        }

        const fits = !short.length;
        const ramTaken = (where === "iram")
            ? ((status.iram_data_used + ram) >= TIGHT * status.iram_data)
            : ((status.ram_used + ram) >= TIGHT * status.ram_arena);
        const tight = fits && (ramTaken
            || (iram && ((status.iram_used + iram) >= TIGHT * status.iram_arena))
            || ((status.area - status.free + sectors(flash)) >= TIGHT * status.area));

        return {
            name, installed: null, package: pkg,
            flash, ram, iram, ramIn: where,
            fits, tight: !!tight, short,
        };
    });
}

/**
 * The RAM of an installed module moves to the other memory with a new copy of it: the running copy keeps its flash
 * until the restart, so the new copy needs a free gap of its size, and the other memory room for its RAM
 * @param {Status} status
 * @param {Row} row
 * @returns {boolean}
 */
export function canMove(status, row) {
    const installed = row.installed;
    if (!installed || !row.package || !row.ram || !status.iram_data) {
        return false;
    }

    if ((installed.state !== "running") && (installed.state !== "not started")) {
        return false;
    }

    const to = (installed.ram_in === "iram") ? "dram" : "iram";
    return (sectors(row.package.image) <= status.free_max)
        && ((to === "iram") ? (row.package.ram <= (status.iram_data_max || 0)) : (row.package.ram <= status.ram_max));
}

/**
 * @typedef {{title: string, total: number, head: number, used: number, added: number, legend: string}} Meter
 */

/**
 * The four meters: flash, RAM, IRAM (code) and RAM in IRAM; `preview` adds what that module would take
 * @param {Status} status
 * @param {Row | null} preview
 * @returns {Meter[]}
 */
export function meters(status, preview) {
    const used = status.modules.reduce((sum, x) => sum + sectors(x.size), 0);
    const add = (preview && !preview.installed) ? preview : null;

    /** @type {Meter[]} */
    const out = [
        {
            title: "Flash", total: status.area, head: status.head, used,
            added: add ? sectors(add.flash) : 0,
            legend: `head ${sizeText(status.head)}, modules ${sizeText(used)}, free ${sizeText(status.free)} of ${sizeText(status.area)}`,
        },
        {
            title: "RAM", total: status.ram_arena, head: 0, used: status.ram_used,
            added: (add && add.ramIn === "dram") ? add.ram : 0,
            legend: `modules ${sizeText(status.ram_used)} of ${sizeText(status.ram_arena)}; free heap ${sizeText(status.heap)} (${sizeText(status.heap_boot)} after boot)`,
        },
        {
            title: "IRAM (code)", total: status.iram_arena, head: 0, used: status.iram_used,
            added: add ? add.iram : 0,
            legend: `module code ${sizeText(status.iram_used)} of ${sizeText(status.iram_arena)}`,
        },
    ];

    if (status.iram_data) {
        out.push({
            title: "RAM in IRAM", total: status.iram_data, head: 0, used: status.iram_data_used,
            added: (add && add.ramIn === "iram") ? add.ram : 0,
            legend: `module RAM ${sizeText(status.iram_data_used)} of ${sizeText(status.iram_data)}; 32-bit memory, byte access is emulated and slower`,
        });
    }

    return out;
}

/**
 * @param {Meter} meter
 * @returns {"" | "mod-tight" | "mod-over"}
 */
export function meterLevel(meter) {
    const taken = meter.head + meter.used + meter.added;
    if (taken > meter.total) {
        return "mod-over";
    }

    return (taken >= TIGHT * meter.total) ? "mod-tight" : "";
}

/**
 * @param {string} state
 * @returns {string}
 */
function stateClass(state) {
    if (state === "running") {
        return "mod-state-running";
    }

    if (state === "not started") {
        return "mod-state-pending";
    }

    return "mod-state-bad";
}

const state = {
    /** @type {URL | null} */
    root: null,
    /** @type {Status | null} */
    status: null,
    /** @type {Map<string, Package>} */
    packages: new Map(),
    /** @type {Map<string, string>} */
    ramIn: new Map(),
    /** @type {Set<string>} modules switched off in this visit, they keep running until the restart */
    removed: new Set(),
    /** @type {string | null} */
    preview: null,
    busy: false,
};

/**
 * @param {string} selector
 * @returns {HTMLElement | null}
 */
function element(selector) {
    return document.querySelector(`#panel-mod ${selector}`);
}

/**
 * @param {string} text
 * @param {boolean} error
 */
function message(text, error = false) {
    const elem = element(".mod-message");
    if (elem) {
        elem.textContent = text;
        elem.classList.toggle("mod-error", error);
    }
}

/**
 * @param {string} path
 * @returns {URL}
 */
function url(path) {
    return new URL(path, state.root ?? window.location.href);
}

/**
 * @param {string} path
 * @param {FormData} body
 * @returns {Promise<string>}
 */
async function post(path, body) {
    const response = await fetch(url(path), {method: "POST", body, credentials: "same-origin"});
    const text = await response.text();
    if (!response.ok) {
        throw new Error(text || `${response.status}`);
    }

    return text;
}

/**
 * @param {number} ms
 */
function wait(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

// with all modules on the board has 5-10 KB of heap: an answer may come empty or cut, ask again a few times
async function load() {
    let last = "";
    for (let attempt = 0; attempt < 4; ++attempt) {
        if (attempt) {
            await wait(1500);
        }

        try {
            const response = await fetch(url("modules"), {cache: "no-store", credentials: "same-origin"});
            const text = await response.text();
            if (!response.ok) {
                last = `${response.status} ${text}`;
                continue;
            }

            state.status = /** @type {Status} */(JSON.parse(text));
            draw();
            if (state.status.file && !state.packages.size) {
                await fromBoard();
            }
            return;
        } catch (error) {
            last = `${error}`;
        }
    }

    throw new Error(`the board did not answer /modules (${last || "empty"}), low on memory? Press Refresh`);
}

// 4 MB boards flashed with the full image carry the modules file at 1 MB: no file to pick
async function fromBoard() {
    const response = await fetch(url("modules/file"), {cache: "no-store", credentials: "same-origin"});
    if (!response.ok) {
        return;
    }

    state.packages = parseContainer(await response.arrayBuffer());
    message(`Modules file: on this board (${[...state.packages.keys()].join(", ")})`);
    draw();
}

function loadAndShow() {
    load().catch((error) => {
        // the switches were disabled while a change was sent, give them back
        draw();
        message(`${error.message ?? error}`, true);
    });
}

/**
 * @param {Meter} meter
 * @returns {HTMLElement}
 */
function meterElement(meter) {
    const level = meterLevel(meter);
    const taken = meter.head + meter.used + meter.added;

    const out = document.createElement("div");
    out.classList.add("mod-meter");
    if (level === "mod-tight") {
        out.classList.add("mod-tight");
    }

    const title = document.createElement("div");
    title.classList.add("mod-meter-title");
    const name = document.createElement("strong");
    name.textContent = meter.title;
    const pct = document.createElement("span");
    pct.classList.add("mod-meter-pct");
    if (level) {
        pct.classList.add(level);
    }
    const share = meter.total ? Math.round(100 * taken / meter.total) : 0;
    pct.textContent = `${share} %`
        + ((level === "mod-tight") ? ", almost full" : "")
        + ((level === "mod-over") ? ", does not fit" : "");
    title.append(name, pct);

    const bar = document.createElement("div");
    bar.classList.add("mod-bar");
    for (const [size, className, label] of [
        [meter.head, "mod-seg-head", "head"],
        [meter.used, "mod-seg-mod", "modules"],
        [Math.min(meter.added, Math.max(0, meter.total - meter.head - meter.used)), "mod-seg-new", "this module"],
    ]) {
        if (!size || !meter.total) {
            continue;
        }

        const seg = document.createElement("i");
        seg.classList.add(/** @type {string} */(className));
        if ((className === "mod-seg-new") && level) {
            seg.classList.add(level);
        }
        seg.style.width = `${100 * /** @type {number} */(size) / meter.total}%`;
        seg.title = `${label} ${sizeText(/** @type {number} */(size))}`;
        bar.appendChild(seg);
    }

    const legend = document.createElement("div");
    legend.classList.add("mod-meter-legend");
    legend.textContent = meter.legend;

    out.append(title, bar, legend);
    return out;
}

/**
 * @param {string} text
 * @param {boolean} short
 * @returns {HTMLElement}
 */
function chip(text, short = false) {
    const out = document.createElement("span");
    out.classList.add("mod-chip");
    if (short) {
        out.classList.add("mod-short");
    }
    out.textContent = text;
    return out;
}

/**
 * @param {Status} status
 * @param {Row} row
 * @returns {HTMLElement}
 */
function cardElement(status, row) {
    const removed = state.removed.has(row.name);
    const on = !!row.installed;

    const card = document.createElement("div");
    card.classList.add("mod-card");
    if (on) {
        card.classList.add((row.installed?.state === "running") ? "mod-on" : "mod-pending");
    } else if (removed) {
        card.classList.add("mod-pending");
    } else if (!row.fits || !row.package) {
        card.classList.add("mod-cant");
    }

    const head = document.createElement("div");
    head.classList.add("mod-card-head");

    const toggle = document.createElement("input");
    toggle.type = "checkbox";
    toggle.classList.add("checkbox-toggle");
    toggle.checked = on;
    toggle.disabled = state.busy || (!on && (!row.fits || !row.package));
    toggle.addEventListener("change", () => {
        switchModule(row, toggle.checked)
            .catch((error) => message(`${row.name}: ${error.message ?? error}`, true))
            .finally(() => {
                state.busy = false;
                loadAndShow();
            });
    });

    const name = document.createElement("strong");
    name.textContent = row.name;

    const badge = document.createElement("span");
    badge.classList.add("mod-state");
    if (on) {
        const text = row.installed?.state ?? "";
        badge.textContent = (text === "not started") ? "after restart" : text;
        badge.classList.add(stateClass(text));
    } else if (removed) {
        badge.textContent = "off after restart";
        badge.classList.add("mod-state-pending");
    } else if (!row.package) {
        badge.textContent = "not in the modules file";
    } else if (!row.fits) {
        badge.textContent = "does not fit";
        badge.classList.add("mod-state-bad");
    } else if (row.tight) {
        badge.textContent = "fits, tight";
        badge.classList.add("mod-state-tight");
    } else {
        badge.textContent = "can be added";
    }

    head.append(toggle, name, badge);

    const desc = document.createElement("div");
    desc.classList.add("mod-desc");
    desc.textContent = DESCRIPTIONS.get(row.name) ?? "";

    const sizes = document.createElement("div");
    sizes.classList.add("mod-sizes");
    if (row.installed || row.package) {
        sizes.append(
            chip(`Flash ${sizeText(row.flash)}`, row.short.includes("flash")),
            chip(`RAM ${sizeText(row.ram)}`, row.short.includes("RAM") || row.short.includes("RAM in IRAM")),
            chip(`IRAM ${sizeText(row.iram)}`, row.short.includes("IRAM")));
    }

    const movable = on && canMove(status, row);
    if (on && !movable) {
        sizes.append(chip(`RAM in ${row.ramIn.toUpperCase()}`));
    } else if (status.iram_data && row.package && row.ram) {
        const select = document.createElement("select");
        select.title = "Where the RAM of this module goes: the normal RAM (DRAM) or the spare IRAM";
        for (const [value, text] of [["dram", "RAM in DRAM"], ["iram", "RAM in IRAM"]]) {
            const option = document.createElement("option");
            option.value = value;
            option.textContent = text;
            option.selected = (row.ramIn === value);
            select.appendChild(option);
        }
        select.disabled = state.busy;
        select.addEventListener("change", () => {
            state.ramIn.set(row.name, select.value);
            if (!movable) {
                draw();
                return;
            }

            moveRam(row, select.value)
                .catch((error) => message(`${row.name}: ${error.message ?? error}`, true))
                .finally(() => {
                    state.busy = false;
                    loadAndShow();
                });
        });
        sizes.appendChild(select);
    }

    card.append(head, desc, sizes);
    if (row.short.length) {
        const why = document.createElement("div");
        why.classList.add("mod-why", "mod-error");
        why.textContent = `Not enough ${row.short.join(", ")} for it`;
        card.appendChild(why);
    }

    if (!on && row.package) {
        card.addEventListener("mouseenter", () => {
            state.preview = row.name;
            drawMeters();
        });
        card.addEventListener("mouseleave", () => {
            state.preview = null;
            drawMeters();
        });
    }

    return card;
}

function drawMeters() {
    const status = state.status;
    const container = element(".mod-meters");
    if (!status || !container) {
        return;
    }

    const rows = moduleRows(status, state.packages, state.ramIn);
    const preview = rows.find((x) => x.name === state.preview) ?? null;
    container.replaceChildren(...meters(status, preview).map(meterElement));
}

function draw() {
    const status = state.status;
    if (!status) {
        return;
    }

    const version = element(".mod-version");
    if (version) {
        version.textContent = `${status.version}, ${status.image}`;
    }

    const off = element(".mod-off");
    if (off) {
        off.style.display = status.off ? "" : "none";
        const why = element(".mod-off-reason");
        if (why) {
            why.textContent = status.off;
        }
    }

    drawMeters();

    const cards = element(".mod-cards");
    if (cards) {
        const rows = moduleRows(status, state.packages, state.ramIn);
        cards.replaceChildren(...rows.map((row) => cardElement(status, row)));
    }

    const pending = state.removed.size
        + status.modules.filter((x) => x.state === "not started").length;
    const restart = element(".mod-restart");
    if (restart) {
        restart.classList.toggle("mod-show", pending > 0);
        const text = element(".mod-restart-text");
        if (text) {
            text.textContent = (pending === 1)
                ? "1 change waits for a restart."
                : `${pending} changes wait for a restart.`;
        }
    }
}

/**
 * @param {Row} row
 * @param {boolean} on
 */
async function switchModule(row, on) {
    state.busy = true;
    draw();

    const form = new FormData();
    if (on) {
        if (!row.package) {
            throw new Error("not in the modules file");
        }

        form.append("module", new Blob([row.package.data]), `${row.name}.module`);
        message(`${row.name}: uploading ${sizeText(row.package.data.byteLength)}...`);
        const where = (row.ramIn === "iram") ? "?ram=iram" : "";
        const text = await post(`modules/upload${where}`, form);
        state.removed.delete(row.name);
        message(`${row.name}: ${text}`);
        return;
    }

    if ((row.name === "webui")
        && !window.confirm("Without the webui module the board shows only its small modules page after the restart. Switch it off?"))
    {
        return;
    }

    form.append("name", row.name);
    message(`${row.name}: removing...`);
    const text = await post("modules/remove", form);
    // only running code stays until the restart; one that never started is gone now
    if (row.installed?.state === "running") {
        state.removed.add(row.name);
    }
    message(`${row.name}: ${text}`);
}

/**
 * A new copy of the module with its RAM in the other memory: the board writes it next to the running one and swaps the
 * two in its directory only when the copy is written and checked; one restart applies it
 * @param {Row} row
 * @param {string} where 'dram' or 'iram'
 */
async function moveRam(row, where) {
    if (!row.package) {
        throw new Error("not in the modules file");
    }

    state.busy = true;
    draw();

    const form = new FormData();
    form.append("module", new Blob([row.package.data]), `${row.name}.module`);
    message(`${row.name}: moving its RAM to ${where.toUpperCase()}, uploading ${sizeText(row.package.data.byteLength)}...`);
    const text = await post(`modules/upload?replace=1${(where === "iram") ? "&ram=iram" : ""}`, form);
    message(`${row.name}: RAM in ${where.toUpperCase()} after the restart; ${text}`);
}

/**
 * @param {ArrayBuffer} buffer
 */
function usePackages(buffer) {
    state.packages = parseContainer(buffer);
    message(`Modules file: ${[...state.packages.keys()].join(", ")}`);
    draw();
}

async function fromGithub() {
    const status = state.status;
    if (!status) {
        return;
    }

    const commit = (status.version.split("-git")[1] ?? "").slice(0, 7);
    message(`Looking for the modules of ${commit || status.version}...`);

    const list = await (await fetch(status.list || FIRMWARE_URL, {cache: "no-store"})).json();
    /** @type {string | null} */
    let found = null;
    for (const release of list.releases ?? []) {
        if (commit && !String(release.commit ?? "").startsWith(commit)) {
            continue;
        }

        for (const file of (release.images ?? {})[status.image] ?? []) {
            // the modules file, else the full image (it carries the same file behind 1 MB)
            if ((file.kind === "modules") || ((file.kind === "full") && !found)) {
                found = new URL(file.url, status.list || FIRMWARE_URL).href;
            }
        }
    }

    if (!found) {
        throw new Error(`no modules file of ${commit || status.version} in the release list, pick the file from the disk`);
    }

    message(`Downloading ${found}...`);
    const response = await fetch(found);
    if (!response.ok) {
        throw new Error(`${found}: ${response.status}`);
    }

    usePackages(await response.arrayBuffer());
}

function restartNow() {
    post("modules/restart", new FormData())
        .then(() => {
            message("Restarting, the page comes back in 15 seconds...");
            pageReloadIn(15000);
        })
        .catch((error) => message(`${error.message ?? error}`, true));
}

function modulesOn() {
    post("modules/on", new FormData())
        .then((text) => {
            message(`${text}; the page comes back in 15 seconds...`);
            pageReloadIn(15000);
        })
        .catch((error) => message(`${error.message ?? error}`, true));
}

export function init() {
    styleInject(STYLE);

    // /modules is asked only when the panel is opened: with all modules on, the heap is 5-10 KB and the first page
    // load (page, WebSocket dump) needs all of it
    listenAppConnected((urls) => {
        state.root = new URL("./", urls.auth);
    });

    document.querySelectorAll(".menu-link-mod, #panel-mod .button-mod-refresh")
        .forEach((elem) => {
            elem.addEventListener("click", loadAndShow);
        });

    element(".button-mod-restart")
        ?.addEventListener("click", restartNow);
    element(".button-mod-on")
        ?.addEventListener("click", modulesOn);
    element(".button-mod-github")
        ?.addEventListener("click", () => {
            fromGithub().catch((error) => message(`${error.message ?? error}`, true));
        });

    const file = element(".mod-file");
    if (file instanceof HTMLInputElement) {
        file.addEventListener("change", () => {
            const picked = file.files?.[0];
            if (!picked) {
                return;
            }

            picked.arrayBuffer()
                .then(usePackages)
                .catch((error) => message(`${error.message ?? error}`, true));
        });
    }
}
