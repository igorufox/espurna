// The install image espurna-<chip>.ota and the one install path of every chip (scripts/module_ota.py)
//
// The .ota holds the head(s) and the modules file (EMC2). Installing it: the head of this board first when the board
// runs another head build, then the Apply of the modules (restart without modules, POST /modules/apply with the names
// and the modules file, restart with them). The firmware panel (a release of the list) and the MODULES panel (a file,
// From GitHub) both go through installImage(); the chips differ only in the data of HEAD_ROUTES.

import { fetchJson, post, waitFor } from './net.mjs';

// "EOT1", count, entries of kind, name[16], offset, size, crc32; kind 1 a head, 2 the modules file
const OTA_MAGIC = 0x31544f45; // "EOT1"
const OTA_ENTRY = 32;
export const OTA_HEAD = 1;
export const OTA_MODULES = 2;

// the modules file: "EMC2", count, 72-byte entries (name16, offset, size, image, ram, iram, requires32, 4 reserved)
export const CONTAINER_MAGIC = 0x32434d45; // "EMC2"
export const CONTAINER_ENTRY = 72;

/** @typedef {{kind: number, name: string, data: Uint8Array}} OtaPart */

/**
 * @param {DataView} view
 * @param {number} at
 * @param {number} size
 * @returns {string} the bytes up to the first 0
 */
export function cString(view, at, size) {
    let out = "";
    for (let offset = 0; offset < size; ++offset) {
        const code = view.getUint8(at + offset);
        if (!code) {
            break;
        }
        out += String.fromCharCode(code);
    }

    return out;
}

/**
 * Whether the bytes start an install image
 * @param {ArrayBuffer} buffer the first 4 bytes or more
 * @returns {boolean}
 */
export function isInstallImage(buffer) {
    return (buffer.byteLength >= 4) && (new DataView(buffer).getUint32(0, true) === OTA_MAGIC);
}

/**
 * The parts of an install image
 * @param {ArrayBuffer} buffer
 * @returns {OtaPart[] | null} null: not an install image
 */
export function otaParts(buffer) {
    const view = new DataView(buffer);
    if ((buffer.byteLength < 8) || (view.getUint32(0, true) !== OTA_MAGIC)) {
        return null;
    }

    const count = view.getUint32(4, true);
    const out = [];
    for (let index = 0; index < count; ++index) {
        const at = 8 + (OTA_ENTRY * index);
        if (at + OTA_ENTRY > buffer.byteLength) {
            throw new Error("the install image is cut short");
        }

        const offset = view.getUint32(at + 20, true);
        const size = view.getUint32(at + 24, true);
        if (offset + size > buffer.byteLength) {
            throw new Error("the install image is cut short");
        }

        out.push({
            kind: view.getUint32(at, true),
            name: cString(view, at + 4, 16),
            data: new Uint8Array(buffer, offset, size),
        });
    }

    return out;
}

/**
 * The modules file (EMC2) out of an install image, or the buffer as it is
 * @param {ArrayBuffer} buffer
 * @returns {ArrayBuffer}
 */
export function modulesFileOf(buffer) {
    const parts = otaParts(buffer);
    if (!parts) {
        return buffer;
    }

    const modules = parts.find((part) => part.kind === OTA_MODULES);
    if (!modules) {
        throw new Error("the install image has no modules file");
    }

    return modules.data.slice().buffer;
}

/**
 * @param {string} list comma separated
 * @returns {string[]}
 */
export function names(list) {
    return list.split(",").map((x) => x.trim()).filter((x) => x.length > 0);
}

/** @typedef {{image: number, ram: number, iram: number, requires: string[], data: Uint8Array}} Package */

/**
 * The modules file (EMC2): magic, count, 72-byte entries (name16, offset, size, image, ram, iram, requires32,
 * 4 reserved), then the packages
 * @param {ArrayBuffer} buffer
 * @returns {Map<string, Package>}
 */
export function parseContainer(buffer) {
    const view = new DataView(buffer);
    if ((buffer.byteLength < 8) || (view.getUint32(0, true) !== CONTAINER_MAGIC)) {
        throw new Error("not a modules file");
    }

    const out = new Map();
    const count = view.getUint32(4, true);
    for (let index = 0; index < count; ++index) {
        const at = 8 + (CONTAINER_ENTRY * index);
        if (at + CONTAINER_ENTRY > buffer.byteLength) {
            throw new Error("the modules file is cut short");
        }

        const name = cString(view, at, 16);
        const offset = view.getUint32(at + 16, true);
        const size = view.getUint32(at + 20, true);
        if (offset + size > buffer.byteLength) {
            throw new Error(`${name}: the modules file is cut short`);
        }

        out.set(name, {
            image: view.getUint32(at + 24, true),
            ram: view.getUint32(at + 28, true),
            iram: view.getUint32(at + 32, true),
            requires: names(cString(view, at + 36, 32)),
            data: new Uint8Array(buffer, offset, size),
        });
    }

    return out;
}

/**
 * The head the modules of a modules file were linked against: the head id of its first package (16 bytes at 24, the
 * MD5 of the head image or the Beken's build id), as the board shows its own in /modules ("md5")
 * @param {ArrayBuffer} file the modules file (EMC2)
 * @returns {string} hex, "" - no package
 */
export function modulesHeadId(file) {
    let first;
    try {
        first = parseContainer(file).values().next().value;
    } catch {
        return "";
    }
    if (!first || (first.data.byteLength < 40)) {
        return "";
    }

    return [...first.data.subarray(24, 40)].map((x) => x.toString(16).padStart(2, "0")).join("");
}

/**
 * The name of the head a board takes out of the install image: its chip ("esp8266", "esp32", "beken": the one Beken head
 * of both bootloaders, "beken-tuya" and "beken-quectel")
 * @param {string} image the board's image (/modules "image", fwImage)
 * @returns {string}
 */
export function headName(image) {
    // one Beken head for both bootloaders: the OTA package (RBL) is the same, only the wire images differ
    return image.split("-")[0];
}

/**
 * The head of an install image for this board
 * @param {OtaPart[]} parts
 * @param {string} image the board's image
 * @returns {OtaPart | null}
 */
export function headFor(parts, image) {
    const name = headName(image);
    return parts.find((part) => (part.kind === OTA_HEAD) && (part.name === name)) ?? null;
}

/**
 * The modules the board has switched on (/modules), the names an install keeps
 * @param {{modules: {name: string, enabled?: number}[]}} status
 * @returns {string[]}
 */
export function runningNames(status) {
    return status.modules.filter((x) => x.enabled !== 0).map((x) => x.name);
}

/**
 * @typedef {{restart: string, away: number, upload: string, field: string}} HeadRoute
 */

/**
 * How a head reaches the flash, by head name: the restart that frees the place of the head (`away` - what /modules
 * answers once there: 200 ESPurna without modules, 404 the ESP32 safeboot), where the head goes, the form field of it
 * ("" - the file as the body). The ESP32 runs from its only application slot, its safeboot writes the head
 * @type {{[name: string]: HeadRoute}}
 */
export const HEAD_ROUTES = {
    esp8266: {restart: "modules/off", away: 200, upload: "upgrade", field: "upgrade"},
    esp32: {restart: "safeboot/switch", away: 404, upload: "upload", field: ""},
    beken: {restart: "modules/off", away: 200, upload: "upgrade", field: "upgrade"},
};

/**
 * @typedef {(text: string, fraction?: number | null) => void} InstallStep
 */

/**
 * A POST with the upload progress (fetch has none): a form, or a file as the body
 * @param {URL} url
 * @param {FormData | Blob} body
 * @param {(fraction: number) => void} progress
 * @returns {Promise<string>}
 */
export function postWithProgress(url, body, progress) {
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open("POST", url.href);
        xhr.upload.addEventListener("progress", (event) => {
            if (event.lengthComputable) {
                progress(event.loaded / event.total);
            }
        });
        xhr.addEventListener("load", () => {
            if ((xhr.status >= 200) && (xhr.status < 300)) {
                resolve(xhr.responseText);
            } else {
                reject(new Error(xhr.responseText || `${xhr.status}`));
            }
        });
        xhr.addEventListener("error", () => reject(new Error("the upload broke off")));
        xhr.send(body);
    });
}

/**
 * /modules of the board: what it runs, its image and head id
 * @param {URL} root
 * @returns {Promise<any>}
 */
export function readModules(root) {
    return fetchJson(new URL("modules", root), {credentials: "same-origin"}, "/modules");
}

/**
 * Waits until /modules answers with `status` (the board restarted); the JSON when that is 200
 * @param {URL} root
 * @param {number} status
 * @param {number} timeout ms
 * @returns {Promise<any>}
 */
export async function waitModules(root, status = 200, timeout = 90000) {
    const response = await waitFor(new URL("modules", root), status, timeout);
    return (status === 200) ? await response.json() : null;
}

/**
 * @param {number} size
 * @returns {string}
 */
function kb(size) {
    return `${(size / 1024).toFixed(1)} KB`;
}

/**
 * The head onto the board, every chip: the restart of its route, the head uploaded, the board back with it
 * @param {URL} root the board's base URL
 * @param {OtaPart} head
 * @param {InstallStep} step
 * @returns {Promise<any>} /modules of the new head
 */
export async function installHead(root, head, step) {
    const route = HEAD_ROUTES[head.name];
    if (!route) {
        throw new Error(`no install route for the head ${head.name}`);
    }

    step(`Restarting for the new head (${head.name})...`);
    await post(new URL(route.restart, root), new FormData());
    await waitModules(root, route.away, 120000);

    const blob = new Blob([head.data]);
    /** @type {FormData | Blob} */
    let body = blob;
    if (route.field) {
        body = new FormData();
        body.append(route.field, blob, `espurna-${head.name}`);
    }

    const size = kb(head.data.byteLength);
    const text = await postWithProgress(new URL(route.upload, root), body, (fraction) => {
        step(`New head (${head.name}): ${Math.round(100 * fraction)} % of ${size}`, fraction);
    });
    if (route.field && (text.trim() !== "OK")) {
        throw new Error(`the board answered the head: ${text.trim() || "(nothing)"}`);
    }

    step("The board restarts with the new head...");
    return await waitModules(root, 200, 180000);
}

/**
 * The Apply: the board restarts without modules, writes the named ones out of the modules file and restarts with them
 * @param {URL} root
 * @param {ArrayBuffer} file the modules file (EMC2)
 * @param {string[]} names
 * @param {InstallStep} step
 * @returns {Promise<any>} /modules after it
 */
export async function applyModules(root, file, names, step) {
    step("Restarting without modules...");
    await post(new URL("modules/off", root), new FormData());
    await waitModules(root);

    const list = names.join(", ") || "no modules";
    step(`Writing ${list}...`);
    // the names before the file: the board reads them when the file starts
    const form = new FormData();
    form.append("names", names.join(","));
    form.append("modules", new Blob([file]), "modules.bin");
    const size = kb(file.byteLength);
    const text = await postWithProgress(new URL("modules/apply", root), form, (fraction) => {
        step(`Writing ${list}: ${Math.round(100 * fraction)} % of ${size}`, fraction);
    });
    step(`${text}...`);
    return await waitModules(root);
}

/**
 * Whether the head of the image goes on first: the board runs another head build than the modules were linked against
 * @param {{md5?: string}} status /modules
 * @param {ArrayBuffer} file the modules file
 * @returns {boolean}
 */
export function needsHead(status, file) {
    const id = modulesHeadId(file);
    return !id || !status.md5 || (id !== status.md5);
}

/**
 * The install image onto the board, every chip and both panels: its head when the board runs another head build,
 * then the named modules out of it
 * @param {URL} root
 * @param {ArrayBuffer} buffer the install image (.ota), or a modules file alone (no head then)
 * @param {{md5?: string, image: string}} status /modules of the board
 * @param {string[]} names the modules to write
 * @param {InstallStep} step
 * @returns {Promise<any>} /modules after it
 */
export async function installImage(root, buffer, status, names, step) {
    const parts = otaParts(buffer);
    const file = modulesFileOf(buffer);
    const head = (parts && needsHead(status, file)) ? headFor(parts, status.image) : null;
    if (!head && modulesHeadId(file) && status.md5 && (modulesHeadId(file) !== status.md5)) {
        throw new Error(`the modules file is for another head build and has no head for ${status.image}: `
            + "take the install image (.ota) of the release");
    }

    ++running;
    try {
        if (head) {
            await installHead(root, head, step);
        }
        return await applyModules(root, file, names, step);
    } finally {
        --running;
    }
}

let running = 0;

/**
 * An install runs: the board restarts on its own, the page waits for it itself (index.mjs: no reload question on the
 * closed socket)
 * @returns {boolean}
 */
export function installRunning() {
    return running > 0;
}
