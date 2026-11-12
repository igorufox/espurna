// MODULES panel, every chip: the parts of the firmware in the free flash behind the head (docs/modules.md). The board
// answers /modules with what it has; the modules themselves come from the install image (.ota) of the release (from
// the disk or from the release list). Nothing starts or stops while the board runs: tick, then Apply (install.mjs,
// the same install as the firmware panel's: the head of the image first when the board runs another head build, then
// the ticked modules, restarts). Only switching off goes without the image: flag off, out of the directory, restart.
// The head has its own small page with the same switches (/modules.html), it works without this WebUI.

import { fetchBytes, fetchJson, post as netPost, wait } from './net.mjs';
import { installImage, modulesFileOf, modulesHeadId, names, parseContainer, runningNames, waitModules } from './install.mjs';
import { listenAppConnected } from './connection.mjs';
import { FIRMWARE_URL, installFileOf } from './fwinstall.mjs';
import { pageReloadIn, styleInject } from './core.mjs';

// <link> stylesheets are not stripped per-module by the build, keep the rules here
const STYLE = [
    ".mod-overlay { position: fixed; inset: 0; z-index: 10000; background: rgba(0,0,0,.45); display: flex; align-items: center; justify-content: center; }",
    ".mod-dialog { background: #fff; border-radius: 6px; padding: 1.2em 1.5em; width: min(28em, 90vw); box-shadow: 0 4px 24px rgba(0,0,0,.3); }",
    ".mod-dialog strong { display: block; font-size: 115%; margin-bottom: .6em; }",
    ".mod-dialog .mod-dialog-step { margin-bottom: .6em; min-height: 1.4em; }",
    ".mod-dialog .mod-dialog-bar { height: .8em; border-radius: 4px; background: #e8f4e8; overflow: hidden; }",
    ".mod-dialog .mod-dialog-bar i { display: block; height: 100%; background: #1f8dd6; transition: width .3s; }",
    ".mod-dialog .mod-dialog-bar.mod-busy i { width: 30% !important; animation: mod-busy 1.2s ease-in-out infinite alternate; }",
    "@keyframes mod-busy { from { margin-left: 0; } to { margin-left: 70%; } }",
    ".mod-dialog .mod-dialog-note { margin-top: .8em; font-size: 85%; color: #777; }",
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
    "#panel-mod .mod-seg-dyn { background: #8cc6ee; }",
    "#panel-mod .mod-tight .mod-seg-mod, #panel-mod .mod-seg-mod.mod-tight { background: #f0b429; }",
    "#panel-mod .mod-seg-new { background: repeating-linear-gradient(45deg, #1f8dd6 0 6px, #7fbfea 6px 12px); }",
    "#panel-mod .mod-seg-new.mod-tight { background: repeating-linear-gradient(45deg, #f0b429 0 6px, #f7d891 6px 12px); }",
    "#panel-mod .mod-seg-new.mod-over { background: repeating-linear-gradient(45deg, #c00000 0 6px, #e88 6px 12px); }",
    "#panel-mod .mod-meter-legend { font-size: 85%; color: #666; }",
    "#panel-mod .mod-meter-legend strong { display: block; font-size: 115%; color: #333; margin-bottom: .15em; }",
    "#panel-mod .mod-meter-legend div { line-height: 1.4; }",
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
    "#panel-mod .mod-group { grid-column: 1 / -1; margin: .6em 0 0; font-weight: bold; color: #444;"
        + " border-bottom: 1px solid #ddd; padding: .3em .2em; display: flex; align-items: baseline; gap: .6em;"
        + " cursor: pointer; user-select: none; }",
    "#panel-mod .mod-group:hover { background: #f4f4f4; }",
    "#panel-mod .mod-group::before { content: '▸'; color: #888; width: 1em; }",
    "#panel-mod .mod-group.mod-open::before { content: '▾'; }",
    "#panel-mod .mod-group-count { margin-left: auto; font-weight: normal; font-size: 85%; color: #777; }",
    "#panel-mod .mod-group-count b { color: rgb(28, 184, 65); }",
    "#panel-mod .button-mod-remove { margin-left: auto; font-size: 85%; padding: .3em .9em;"
        + " background: rgb(202, 60, 60); color: #fff; border-radius: 4px; }",
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
    ["captive", "Captive portal on the board's own access point: every name resolves to the board, a phone opens the setup page by itself. Not used on the home network."],
    ["mqtt", "MQTT client: relays, buttons, LEDs and sensors on the broker, the MQTT panel. Home Assistant and Domoticz need it."],
]);

// the panel shows the modules by group, in this order; a module not listed goes to the last one
export const GROUPS = [
    ["Interface", ["webui"]],
    ["Inputs and outputs", ["relay", "button", "led", "rf", "ir", "encoder", "mcp23s08", "tuya", "dual", "stm",
        "lightfox", "ifan", "kingart"]],
    ["Light", ["light", "pwm", "my92xx"]],
    ["Sensors", ["sensors", "dht", "dallas", "bmx280", "hlw8012", "cse7766", "pzem004t", "analog", "ntc", "digital"]],
    ["Automation", ["scheduler"]],
    ["Integrations", ["mqtt", "homeassistant", "alexa", "domoticz", "thingspeak", "mdns", "prometheus", "matter"]],
];

/**
 * @param {string} name
 * @returns {string}
 */
/**
 * How many modules of a group are on, installed, listed
 * @param {Row[]} rows the rows of one group
 * @returns {{on: number, installed: number, all: number}}
 */
export function groupCounts(rows) {
    const installed = rows.filter((row) => row.installed);
    return {
        on: installed.filter((row) => row.installed?.enabled !== 0).length,
        installed: installed.length,
        all: rows.length,
    };
}

export function groupOf(name) {
    for (const [group, members] of GROUPS) {
        if (members.includes(name)) {
            return group;
        }
    }

    return "Other";
}

// From this share on a meter is yellow: everything is taken, but the board still lives
export const TIGHT = 0.85;
const SECTOR = 4096;

/** @typedef {{name: string, size: number, offset: number, ram: number, ram_in: string, iram: number, state: string, enabled?: number, requires?: string}} Installed */
/** @typedef {import('./install.mjs').Package} Package */

/**
 * @typedef {{
 *   version: string, image: string, md5?: string, list: string, off: string, modules: Installed[],
 *   area: number, head: number, free: number, free_max: number,
 *   ram_arena: number, ram_used: number, ram_max: number,
 *   iram_arena: number, iram_used: number, iram_max: number,
 *   iram_data: number, iram_data_used: number, iram_data_max: number,
 *   ram_dynamic?: number, pool?: string, iram_code?: number,
 *   heap: number, heap_boot: number, chip?: number, oom?: number,
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
 * What a module needs, by name: from the board for an installed one, else from the modules file
 * @param {string} name
 * @param {Status} status
 * @param {Map<string, Package>} packages
 * @returns {string[]}
 */
export function requiresOf(name, status, packages) {
    const installed = status.modules.find((x) => x.name === name);
    if (installed && (installed.requires !== undefined)) {
        return names(installed.requires);
    }

    return packages.get(name)?.requires ?? [];
}

/**
 * The modules to switch on before this one, the deepest first, the module itself last; a module that is neither
 * installed nor in the modules file stops it
 * @param {string} name
 * @param {Status} status
 * @param {Map<string, Package>} packages
 * @returns {{order: string[], missing: string[]}}
 */
export function switchOnOrder(name, status, packages) {
    /** @type {string[]} */
    const order = [];
    /** @type {string[]} */
    const missing = [];
    /** @type {Set<string>} */
    const seen = new Set();

    /** @param {string} current */
    const visit = (current) => {
        if (seen.has(current)) {
            return;
        }
        seen.add(current);
        for (const needed of requiresOf(current, status, packages)) {
            visit(needed);
        }

        const installed = status.modules.find((x) => x.name === current);
        if (!installed && !packages.has(current)) {
            missing.push(current);
            return;
        }
        if ((current === name) || !installed || (installed.enabled === 0)) {
            order.push(current);
        }
    };

    visit(name);
    return {order, missing};
}

/**
 * The ticked set after one tick: on - the module and what it needs; off - the module and the ticked ones that need it
 * @param {Set<string>} selected
 * @param {string} name
 * @param {boolean} on
 * @param {Status} status
 * @param {Map<string, Package>} packages
 * @returns {Set<string>}
 */
export function selectionAfter(selected, name, on, status, packages) {
    const out = new Set(selected);
    // ticking a module unticks the ones that cannot run next to it on this chip
    if (on) {
        for (const other of conflictsOf(name, status)) {
            out.delete(other);
        }
    }
    /** @type {Set<string>} */
    const seen = new Set();
    if (on) {
        /** @param {string} current */
        const add = (current) => {
            if (seen.has(current)) {
                return;
            }
            seen.add(current);
            out.add(current);
            for (const needed of requiresOf(current, status, packages)) {
                add(needed);
            }
        };
        add(name);
        return out;
    }

    /** @param {string} current */
    const drop = (current) => {
        if (seen.has(current)) {
            return;
        }
        seen.add(current);
        out.delete(current);
        for (const other of [...out]) {
            if (requiresOf(other, status, packages).includes(current)) {
                drop(other);
            }
        }
    };
    drop(name);
    return out;
}

/**
 * Modules that cannot run next to this one on the board's chip: with Matter the mDNS responder is Matter's (Beken: both
 * want port 5353; ESP32: both on the one IDF mdns component). The ESP8266 has no Matter
 * @param {string} name
 * @param {Status} status
 * @returns {string[]}
 */
export function conflictsOf(name, status) {
    if ((status?.image ?? "esp8266").startsWith("esp8266")) {
        return [];
    }

    return (name === "matter") ? ["mdns"] : [];
}

/**
 * Ticked modules that keep this one off (conflictsOf the other way round)
 * @param {string} name
 * @param {Set<string>} selected
 * @param {Status} status
 * @returns {string[]}
 */
export function blockedBy(name, selected, status) {
    return [...selected].filter((other) => conflictsOf(other, status).includes(name));
}

/**
 * Installed modules that need this one (switched on, or all of them)
 * @param {string} name
 * @param {Status} status
 * @param {Map<string, Package>} packages
 * @param {boolean} onlyOn
 * @returns {string[]}
 */
export function dependentsOf(name, status, packages, onlyOn) {
    /** @type {string[]} */
    const out = [];
    /** @param {string} current */
    const visit = (current) => {
        for (const other of status.modules) {
            if (out.includes(other.name) || (onlyOn && (other.enabled === 0))) {
                continue;
            }
            if (requiresOf(other.name, status, packages).includes(current)) {
                out.push(other.name);
                visit(other.name);
            }
        }
    };

    visit(name);
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

        // as the Apply places it: the fixed pool while it has room, else the heap
        const where = (status.iram_data && pkg && pkg.ram && (pkg.ram <= (status.iram_data_max || 0))
            && (ramIn.get(name) !== "dram")) ? "iram" : "dram";
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
 * @typedef {{title: string, total: number, head: number, used: number, dynamic?: number, added: number, free: string, legend: string[]}} Meter
 */

/**
 * The meters: flash; the heap (the statics of the modules in their heap block, the heap they took when they started,
 * free); the chip's fixed pool (ESP8266: the IRAM tail with the module code and RAM, Beken: the free TCM). `preview`
 * adds what that module would take
 * @param {Status} status
 * @param {Row | null} preview
 * @returns {Meter[]}
 */
export function meters(status, preview) {
    const used = status.modules.reduce((sum, x) => sum + sectors(x.size), 0);
    const add = (preview && !preview.installed) ? preview : null;
    const dynamic = status.ram_dynamic ?? 0;

    /** @type {Meter[]} */
    const out = [
        {
            title: "Flash", total: status.area, head: status.head, used,
            added: add ? sectors(add.flash) : 0,
            free: `free ${sizeText(status.free)} of ${sizeText(status.area)}`,
            legend: [`head ${sizeText(status.head)}`, `modules ${sizeText(used)}`],
        },
        {
            title: "RAM", total: status.ram_used + dynamic + status.heap, head: 0, used: status.ram_used, dynamic,
            added: (add && add.ramIn === "dram") ? add.ram : 0,
            free: `free ${sizeText(status.heap)} of ${sizeText(status.ram_used + dynamic + status.heap)}`,
            legend: [
                `module statics ${sizeText(status.ram_used)}`
                    + (status.iram_data ? ` (the ones not in the ${(status.pool === "TCM") ? "TCM" : "IRAM tail"})` : ""),
                `heap the modules took when they started ${sizeText(dynamic)}`,
                `free heap after boot ${sizeText(status.heap_boot)}`,
                "what a module allocates later is not counted",
            ],
        },
    ];

    // the fixed pool, where the chip has one (not the ESP32)
    if (status.iram_data) {
        const pool = status.pool ?? "IRAM";
        const code = status.iram_code ?? 0;
        out.push({
            title: (pool === "TCM") ? "TCM" : "IRAM tail", total: status.iram_data, head: 0, used: status.iram_data_used,
            added: add ? ((add.ramIn === "iram") ? add.ram : 0) + add.iram : 0,
            free: `free ${sizeText(status.iram_data - status.iram_data_used)} of ${sizeText(status.iram_data)}`,
            legend: (pool === "TCM")
                ? [`module statics ${sizeText(status.iram_data_used)}`, "the modules' RAM goes here first, the largest first"]
                : [`module code ${sizeText(code)}`, `module statics ${sizeText(status.iram_data_used - code)}`,
                    "32-bit memory, byte access is emulated and slower"],
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
    /** @type {ArrayBuffer | null} the install image (.ota) or the modules file the packages come from: *Apply* installs it */
    image: null,
    /** @type {ArrayBuffer | null} the modules file of it */
    file: null,
    /** @type {Set<string> | null} the ticked modules; null - as the board runs them */
    selected: null,
    /** @type {Map<string, string>} */
    ramIn: new Map(),
    /** @type {Set<string>} modules switched off in this visit, they keep running until the restart */
    removed: new Set(),
    /** @type {string | null} */
    preview: null,
    busy: false,
    /** @type {Map<string, boolean>} groups opened or closed by a click (else: open while a module of it is on) */
    open: new Map(),
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
    dialogStep(text);
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
function post(path, body) {
    return netPost(url(path), body);
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
            return;
        } catch (error) {
            last = `${error}`;
        }
    }

    throw new Error(`the board did not answer /modules (${last || "empty"}), low on memory? Press Refresh`);
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
        [meter.used, "mod-seg-mod", "module statics"],
        [meter.dynamic ?? 0, "mod-seg-dyn", "heap the modules took when they started"],
        [Math.min(meter.added, Math.max(0, meter.total - meter.head - meter.used - (meter.dynamic ?? 0))), "mod-seg-new", "this module"],
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
    const free = document.createElement("strong");
    free.textContent = meter.free;
    legend.appendChild(free);
    for (const line of meter.legend) {
        const item = document.createElement("div");
        item.textContent = line;
        legend.appendChild(item);
    }

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
    const installed = row.installed;
    // installed and switched on; an installed module switched off stays in flash (on again: the Apply, a restart)
    const on = !!installed && (installed.enabled !== 0);
    const offRunning = !!installed && !on && (installed.state === "running");

    const card = document.createElement("div");
    card.classList.add("mod-card");
    if (on) {
        card.classList.add((installed?.state === "running") ? "mod-on" : "mod-pending");
    } else if (removed || offRunning) {
        card.classList.add("mod-pending");
    } else if (!installed && (!row.fits || !row.package)) {
        card.classList.add("mod-cant");
    }

    const head = document.createElement("div");
    head.classList.add("mod-card-head");

    const toggle = document.createElement("input");
    toggle.type = "checkbox";
    toggle.classList.add("checkbox-toggle");
    // ticked here, written by *Apply*: the whole module area anew from the modules file
    toggle.checked = selection(status).has(row.name);
    const blockers = blockedBy(row.name, selection(status), status);
    toggle.disabled = state.busy || !row.package || (blockers.length > 0);
    toggle.addEventListener("change", () => {
        state.selected = selectionAfter(selection(status), row.name, toggle.checked, status, state.packages);
        draw();
    });

    const name = document.createElement("strong");
    name.textContent = row.name;

    const badge = document.createElement("span");
    badge.classList.add("mod-state");
    if (on) {
        const text = installed?.state ?? "";
        badge.textContent = (text === "not started") ? "after restart" : text;
        badge.classList.add(stateClass(text));
    } else if (removed || offRunning) {
        badge.textContent = "off after restart";
        badge.classList.add("mod-state-pending");
    } else if (installed) {
        badge.textContent = "off, in flash";
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
    if (installed && !on) {
        // switched off: Remove frees its flash and RAM (on again needs the modules file then)
        const remove = document.createElement("button");
        remove.type = "button";
        remove.classList.add("pure-button", "button-mod-remove");
        remove.textContent = "Remove";
        remove.disabled = state.busy;
        remove.addEventListener("click", () => {
            removeModule(row)
                .catch((error) => message(`${row.name}: ${error.message ?? error}`, true))
                .finally(() => {
                    state.busy = false;
                    loadAndShow();
                });
        });
        head.appendChild(remove);
    }

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

    const needs = requiresOf(row.name, status, state.packages);
    if (needs.length) {
        sizes.append(chip(`needs ${needs.join(", ")}`));
    }

    // one mDNS responder per chip: with Matter it is Matter's (mdns.cpp start())
    if (blockers.length) {
        sizes.append(chip(`off while ${blockers.join(", ")} is on: it answers mDNS on this chip`, true));
    }

    if (row.ram && status.iram_data) {
        sizes.append(chip(on ? `RAM in ${(row.ramIn === "iram") ? (status.pool ?? "IRAM") : "the heap"}` : "the Apply picks its RAM"));
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
        /** @type {HTMLElement[]} */
        const elements = [];
        for (const group of [...GROUPS.map(([name]) => name), "Other"]) {
            const members = rows.filter((row) => groupOf(row.name) === group);
            if (!members.length) {
                continue;
            }
            const counts = groupCounts(members);
            const open = state.open.get(group) ?? (counts.on > 0);

            const heading = document.createElement("div");
            heading.classList.add("mod-group");
            heading.classList.toggle("mod-open", open);
            heading.textContent = group;
            const count = document.createElement("span");
            count.classList.add("mod-group-count");
            const on = document.createElement("b");
            on.textContent = `${counts.on} on`;
            count.append(on, ` · ${counts.installed} in flash · ${counts.all} modules`);
            heading.appendChild(count);
            heading.addEventListener("click", () => {
                state.open.set(group, !open);
                draw();
            });

            elements.push(heading);
            if (open) {
                elements.push(...members.map((row) => cardElement(status, row)));
            }
        }
        cards.replaceChildren(...elements);
    }

    const pending = changes(status);
    const restart = element(".mod-restart");
    if (restart) {
        restart.classList.toggle("mod-show", pending.length > 0);
        const text = element(".mod-restart-text");
        if (text) {
            const adds = pending.some((x) => x.startsWith("+"));
            const keeps = selection(status).size > 0;
            const headId = state.file ? modulesHeadId(state.file) : "";
            const newHead = !!(status.md5 && headId && (headId !== status.md5));
            text.textContent = (state.file || !keeps)
                ? `${pending.join(", ")}: Apply writes the ticked modules anew, packed, and restarts the board`
                    + " (twice: without modules, then with them). This frees the flash of the modules switched off."
                    + (newHead ? " The file is for another head build: Apply puts its head on first (one more restart)." : "")
                : adds
                    ? `${pending.join(", ")}: adding a module needs the install image (.ota): pick it below.`
                    : `${pending.join(", ")}: Apply switches them off and restarts the board, no file needed.`
                        + " Their flash becomes a gap: an Apply with the install image (.ota) packs the area.";
        }
    }
}

/**
 * Ticked modules: what the user ticked, else what the board runs
 * @param {Status} status
 * @returns {Set<string>}
 */
function selection(status) {
    if (!state.selected) {
        state.selected = runningSet(status);
    }

    return state.selected;
}

/**
 * @param {Status} status
 * @returns {Set<string>}
 */
export function runningSet(status) {
    return new Set(runningNames(status));
}

/**
 * What *Apply* changes: +name on, -name off
 * @param {Status} status
 * @returns {string[]}
 */
function changes(status) {
    const now = runningSet(status);
    const ticked = selection(status);
    return [
        ...[...ticked].filter((name) => !now.has(name)).map((name) => `+${name}`),
        ...[...now].filter((name) => !ticked.has(name)).map((name) => `-${name}`),
    ];
}

let applying = false;

/** @type {{root: HTMLElement | null, step: HTMLElement | null, bar: HTMLElement | null}} */
const dialog = {root: null, step: null, bar: null};

/** @param {BeforeUnloadEvent} event */
function holdPage(event) {
    event.preventDefault();
    event.returnValue = "";
}

/**
 * The Apply dialog over the whole page: the step, its progress, nothing to click; leaving the page asks first
 * @param {string} title
 */
function dialogShow(title) {
    const root = document.createElement("div");
    root.classList.add("mod-overlay");
    const box = document.createElement("div");
    box.classList.add("mod-dialog");
    const head = document.createElement("strong");
    head.textContent = title;
    const step = document.createElement("div");
    step.classList.add("mod-dialog-step");
    const bar = document.createElement("div");
    bar.classList.add("mod-dialog-bar", "mod-busy");
    bar.appendChild(document.createElement("i"));
    const note = document.createElement("div");
    note.classList.add("mod-dialog-note");
    note.textContent = "Keep this page open and the board powered until it is done.";
    box.append(head, step, bar, note);
    root.appendChild(box);
    document.body.appendChild(root);
    Object.assign(dialog, {root, step, bar});
    window.addEventListener("beforeunload", holdPage);
}

/**
 * @param {string} text
 * @param {number | null} fraction 0..1, null - no measure (a restart)
 */
function dialogStep(text, fraction = null) {
    if (!dialog.root || !dialog.step || !dialog.bar) {
        return;
    }
    dialog.step.textContent = text;
    dialog.bar.classList.toggle("mod-busy", fraction === null);
    const fill = /** @type {HTMLElement} */(dialog.bar.firstChild);
    fill.style.width = `${Math.round(100 * (fraction ?? 0))}%`;
}

function dialogHide() {
    window.removeEventListener("beforeunload", holdPage);
    dialog.root?.remove();
    Object.assign(dialog, {root: null, step: null, bar: null});
}

/**
 * An Apply runs: the page waits for the restarts itself (index.mjs: no reload question on the closed socket)
 * @returns {boolean}
 */
export function modulesApplyRunning() {
    return applying;
}

/**
 * *Apply*: the install image (install.mjs installImage, as the firmware panel): its head first when the board runs
 * another head build, then the board restarts without modules, writes the ticked ones packed and restarts with them
 */
async function applyNow() {
    applying = true;
    dialogShow("Applying the modules");
    try {
        await applySteps();
    } finally {
        applying = false;
        dialogHide();
    }

    // the socket closed with the restarts: a fresh page connects again
    window.setTimeout(() => window.location.reload(), 3000);
}

async function applySteps() {
    const status = state.status;
    if (!status) {
        return;
    }

    const names = [...selection(status)];

    // only switched off, no file: out of the directory, one restart. The others stay where they are, the flash of the
    // ones switched off is freed by the next Apply with the install image (it writes the area anew)
    const running = runningSet(status);
    const removed = [...running].filter((name) => !selection(status).has(name));
    const added = names.filter((name) => !running.has(name));
    if (!state.image && names.length && !added.length && removed.length) {
        state.busy = true;
        draw();
        for (const name of removed) {
            // the board removes only a module that is switched off (400 "switch it off first")
            const off = new FormData();
            off.append("name", name);
            off.append("on", "0");
            message(`${name}: switching off...`);
            await post("modules/enable", off);
            const form = new FormData();
            form.append("name", name);
            await post("modules/remove", form);
        }
        message("Restarting...");
        await post("modules/restart", new FormData());
        await waitModules(url("./"));
        state.selected = null;
        message(`Switched off: ${removed.join(", ")}. Their flash is a gap until an Apply with the install image (.ota)`);
        return;
    }

    if (names.length && !state.image) {
        throw new Error("pick the install image (.ota) first, or take it From GitHub");
    }

    state.busy = true;
    draw();

    const root = url("./");
    if (state.image) {
        await installImage(root, state.image, status, names, (text, fraction) => {
            message(text);
            dialogStep(text, fraction ?? null);
        });
    } else {
        // nothing ticked, no image: the board restarts without modules
        message("Restarting without modules...");
        await post("modules/off", new FormData());
        await waitModules(root);
    }

    state.selected = null;
    message(`Applied: ${names.join(", ") || "no modules"}`);
}

/**
 * Out of the flash: a module that is off; its code keeps running until the restart when it ran
 * @param {Row} row
 */
async function removeModule(row) {
    const status = state.status;
    const dependents = status ? dependentsOf(row.name, status, state.packages, false) : [];
    if (dependents.length) {
        throw new Error(`${dependents.join(", ")} ${(dependents.length > 1) ? "need" : "needs"} it: remove ${(dependents.length > 1) ? "them" : "it"} first`);
    }

    state.busy = true;
    draw();

    const form = new FormData();
    form.append("name", row.name);
    message(`${row.name}: removing...`);
    const text = await post("modules/remove", form);
    if (row.installed?.state === "running") {
        state.removed.add(row.name);
    }
    message(`${row.name}: ${text}`);
}

/**
 * @param {ArrayBuffer} buffer
 */
function usePackages(buffer) {
    // the install image (.ota): its head goes on first when the board runs another head build (installImage)
    const file = modulesFileOf(buffer);
    state.packages = parseContainer(file);
    state.file = file;
    state.image = buffer;
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

    const base = status.list || FIRMWARE_URL;
    const found = installFileOf(await fetchJson(base, {}, base), {image: status.image, commit, base});
    if (!found) {
        throw new Error(`no install image of ${commit || status.version} in the release list, pick the file from the disk`);
    }

    usePackages(await fetchBytes(found.url, found, (text) => message(text)));
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
        ?.addEventListener("click", () => {
            const status = state.status;
            if (!status || !changes(status).length) {
                restartNow();
                return;
            }

            applyNow()
                .catch((error) => message(`${error.message ?? error}`, true))
                .finally(() => {
                    state.busy = false;
                    loadAndShow();
                });
        });
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
