import { sendAction } from './connection.mjs';
import { styleInject } from './core.mjs';
import { qrDraw } from './qrcode.mjs';
import { variableListeners } from './settings.mjs';

// <link> stylesheets are not stripped per-module by the build, keep the rules here
const STYLE = [
    ".matter-qr { display: block; image-rendering: pixelated; border: 1px solid #eee; }",
    ".matter-manual { font-family: 'Courier New', monospace; font-size: 150%; letter-spacing: 1px; }",
    ".matter-payload { font-family: 'Courier New', monospace; word-break: break-all; }",
    ".matter-controller { display: flex; align-items: center; flex-wrap: wrap; gap: 0.5em 1em; margin: 0.6em 0; }",
    ".matter-controller-node { color: #888; font-size: 85%; }",
    ".button-matter-remove { background: rgb(192, 0, 0); margin-left: auto; }",
];

/**
 * @param {string} selector
 * @returns {HTMLElement | null}
 */
function element(selector) {
    return document.querySelector(`#panel-matter ${selector}`);
}

/**
 * @param {string} payload
 */
function drawQr(payload) {
    const canvas = element(".matter-qr");
    if (!(canvas instanceof HTMLCanvasElement)) {
        return;
    }

    if (!payload) {
        canvas.width = 0;
        canvas.height = 0;
        return;
    }

    qrDraw(canvas, payload, 6);
}

// Vendor ids of the controllers (fabrics) people meet most; others show the number
const VENDORS = new Map([
    [0x132F, "Smart speaker"],
    [0x1349, "Apple Home"],
    [0x6006, "Google Home"],
    [0x1217, "Amazon Alexa"],
    [0x110A, "Samsung SmartThings"],
]);

/**
 * @param {number} vendor
 * @returns {string}
 */
export function vendorName(vendor) {
    const name = VENDORS.get(vendor);
    if (name) {
        return name;
    }

    // test vendor ids: python-matter-server, Home Assistant, chip-tool and other self-made controllers use them
    if ((vendor >= 0xFFF1) && (vendor <= 0xFFF4)) {
        return `Test vendor 0x${vendor.toString(16).toUpperCase()} (matter-server, Home Assistant, chip-tool...)`;
    }

    return `Vendor 0x${vendor.toString(16).toUpperCase().padStart(4, "0")}`;
}

/**
 * [[#, vendor id, "node id (hex)", "label"], ...]
 * @param {any} list
 */
function updateControllers(list) {
    const container = element(".matter-controllers");
    if (!container) {
        return;
    }

    container.replaceChildren();

    const entries = Array.isArray(list) ? list : [];
    if (!entries.length) {
        const empty = document.createElement("div");
        empty.classList.add("matter-controller");
        empty.textContent = "None: the device is not added anywhere yet.";
        container.append(empty);
        return;
    }

    for (const [index, vendor, node, label] of entries) {
        const name = vendorName(Number(vendor));

        const row = document.createElement("div");
        row.classList.add("matter-controller");

        const title = document.createElement("span");
        title.textContent = `${name}${label ? ` "${label}"` : ""}`;

        const details = document.createElement("span");
        details.classList.add("matter-controller-node");
        const node_id = String(node).replace(/^0+(?=.)/, "");
        details.textContent = `#${index}, device node 0x${node_id}`;

        const button = document.createElement("button");
        button.type = "button";
        button.classList.add("pure-button", "button-matter-remove");
        button.textContent = "Remove";
        button.addEventListener("click", (event) => {
            event.preventDefault();
            if (!window.confirm(`Remove ${name} from this device?\n\nIt stops controlling the device. To add the device to it again, open the pairing window below and add it in the app.`)) {
                return;
            }
            button.disabled = true;
            sendAction("matterRemove", {index: Number(index)});
        });

        row.append(title, details, button);
        container.append(row);
    }
}

/**
 * @param {number} seconds
 * @returns {string}
 */
export function windowText(seconds) {
    if (seconds <= 0) {
        return "Open pairing window";
    }

    const minutes = Math.floor(seconds / 60);
    const rest = String(seconds % 60).padStart(2, "0");
    return `Pairing window open, ${minutes}:${rest} left`;
}

/**
 * the button of the pairing window: closed, asked for (the device takes a few seconds to open it), open
 * @param {number} seconds
 * @param {boolean} opening
 * @param {boolean} started
 * @returns {{text: string, disabled: boolean}}
 */
export function windowButton(seconds, opening, started) {
    if (!started) {
        return {text: "Matter is not running", disabled: true};
    }

    if (seconds > 0) {
        return {text: windowText(seconds), disabled: true};
    }

    if (opening) {
        return {text: "Opening the pairing window...", disabled: true};
    }

    return {text: windowText(0), disabled: false};
}

/** @type {{left: number, timer: ReturnType<typeof setInterval> | null, opening: ReturnType<typeof setTimeout> | null, started: boolean}} */
const WINDOW = {
    left: 0,
    timer: null,
    opening: null,
    started: false,
};

// the device answers with the open window within seconds; without it the button comes back after this long
const OPENING_MS = 30000;

function showWindow() {
    const button = element(".button-matter-window");
    if (!(button instanceof HTMLButtonElement)) {
        return;
    }

    const state = windowButton(WINDOW.left, WINDOW.opening !== null, WINDOW.started);
    button.disabled = state.disabled;
    button.textContent = state.text;
}

function stopOpening() {
    if (WINDOW.opening !== null) {
        clearTimeout(WINDOW.opening);
        WINDOW.opening = null;
    }
}

/**
 * seconds left of the open pairing window (0: closed), counted down here between the updates of the device
 * @param {number} seconds
 */
function updateWindow(seconds) {
    WINDOW.left = Math.max(0, Math.floor(seconds));
    if (WINDOW.left > 0) {
        stopOpening();
    }
    if (WINDOW.timer !== null) {
        clearInterval(WINDOW.timer);
        WINDOW.timer = null;
    }

    if (WINDOW.left > 0) {
        WINDOW.timer = setInterval(() => {
            WINDOW.left = Math.max(0, WINDOW.left - 1);
            if (!WINDOW.left && (WINDOW.timer !== null)) {
                clearInterval(WINDOW.timer);
                WINDOW.timer = null;
            }
            showWindow();
        }, 1000);
    }

    showWindow();
}

/**
 * @param {boolean} started
 */
function updateStarted(started) {
    const button = element(".button-matter-start");
    if (button instanceof HTMLButtonElement) {
        button.disabled = started;
    }

    const pairing = element(".matter-commissioning");
    if (pairing) {
        pairing.style.display = started ? "" : "none";
    }

    WINDOW.started = started;
    if (!started) {
        stopOpening();
    }
    showWindow();
}

// ---------------------------------------------------------------------------------------------------------------------
// Identity (`matterIdentity`): factory / espurna / custom

/**
 * PEM ("-----BEGIN ...-----" + base64) to DER bytes; anything else (DER, raw key bytes) as it is
 * @param {Uint8Array} bytes
 * @returns {Uint8Array}
 */
export function pemToDer(bytes) {
    const text = new TextDecoder("latin1").decode(bytes);
    const match = text.match(/-----BEGIN [^-]+-----([\s\S]*?)-----END [^-]+-----/);
    if (!match) {
        return bytes;
    }

    const binary = atob((match[1] ?? "").replace(/\s+/g, ""));
    const out = new Uint8Array(binary.length);
    for (let index = 0; index < binary.length; ++index) {
        out[index] = binary.charCodeAt(index);
    }

    return out;
}

/**
 * @param {Uint8Array} bytes
 * @returns {string}
 */
export function toBase64(bytes) {
    let binary = "";
    for (const byte of bytes) {
        binary += String.fromCharCode(byte);
    }

    return btoa(binary);
}

/**
 * the options of the identity choice: which can be picked, and why not
 * @param {boolean} factory the board has factory data
 * @param {string} custom VID / PID of the uploaded custom identity, empty when none
 * @returns {{factory: {disabled: boolean, note: string}, espurna: {disabled: boolean, note: string}, custom: {disabled: boolean, note: string}}}
 */
export function identityOptions(factory, custom) {
    return {
        factory: {
            disabled: !factory,
            note: factory ? "" : "This board has no factory data (no stock Matter firmware tables)",
        },
        espurna: {
            disabled: false,
            note: "",
        },
        custom: {
            disabled: !custom,
            note: custom ? `Uploaded: ${custom}` : "Nothing uploaded yet: choose the files below",
        },
    };
}

const IDENTITY_NAMES = {
    factory: "the factory identity",
    espurna: "the ESPurna development identity",
    custom: "the custom identity",
};

/**
 * @param {string} use
 * @returns {string}
 */
export function identityConfirmText(use) {
    const name = IDENTITY_NAMES[/** @type {keyof typeof IDENTITY_NAMES} */ (use)] ?? use;
    return `Switch Matter to ${name}?\n\n`
        + "Every controller (app, hub) this device was added to checked the old device certificate: the device is "
        + "removed from all of them, and the board restarts. Add the device to the controllers again afterwards.";
}

/** @type {{use: string, factory: boolean, custom: string}} */
const IDENTITY = {
    use: "",
    factory: false,
    custom: "",
};

function showIdentity() {
    const select = element(".matter-identity-use");
    if (!(select instanceof HTMLSelectElement)) {
        return;
    }

    const options = identityOptions(IDENTITY.factory, IDENTITY.custom);
    for (const option of select.options) {
        const state = options[/** @type {keyof typeof options} */ (option.value)];
        if (state) {
            option.disabled = state.disabled;
        }
    }

    if (IDENTITY.use) {
        select.value = IDENTITY.use;
    }

    showIdentityNote();
}

function showIdentityNote() {
    const select = element(".matter-identity-use");
    const note = element(".matter-identity-note");
    if (!(select instanceof HTMLSelectElement) || !note) {
        return;
    }

    const options = identityOptions(IDENTITY.factory, IDENTITY.custom);
    const notes = [];
    if (IDENTITY.use) {
        notes.push(`In use: ${IDENTITY.use}.`);
    }
    if (options.factory.disabled) {
        notes.push(`Factory: ${options.factory.note}.`);
    }
    notes.push(`Custom: ${options.custom.note}.`);
    note.textContent = notes.join(" ");
}

/**
 * @param {string} selector
 * @returns {Promise<string>} base64 of the DER bytes, empty without a file
 */
async function fileBase64(selector) {
    const input = element(selector);
    if (!(input instanceof HTMLInputElement) || !input.files || !input.files.length) {
        return "";
    }

    const buffer = await input.files[0].arrayBuffer();
    return toBase64(pemToDer(new Uint8Array(buffer)));
}

async function uploadIdentity() {
    const [dac, key, pai, cd] = await Promise.all([
        fileBase64(".matter-identity-dac"),
        fileBase64(".matter-identity-key"),
        fileBase64(".matter-identity-pai"),
        fileBase64(".matter-identity-cd"),
    ]);

    if (!dac || !key || !pai) {
        window.alert("Choose the DAC, its private key and the PAI first (the declaration is optional).");
        return;
    }

    if (!window.confirm(identityConfirmText("custom"))) {
        return;
    }

    sendAction("matterIdentityUpload", {dac, key, pai, cd});

    // the key file is not kept in the page
    const input = element(".matter-identity-key");
    if (input instanceof HTMLInputElement) {
        input.value = "";
    }
}

// the pairing needs about this much free heap (PASE and CASE sessions, certificates): below it a warning (W93)
export const PAIRING_HEAP = 15 * 1024;

const HEAP = {now: 0, min: 0};

/**
 * The free heap line under the pairing button
 * @param {number} now
 * @param {number} min the lowest since boot
 * @returns {{text: string, low: boolean}}
 */
export function heapLine(now, min) {
    const kb = (/** @type {number} */ value) => `${Math.round(value / 1024)} KB`;
    const low = (now > 0) && (now < PAIRING_HEAP);
    let text = `Free memory ${kb(now)}, the lowest since boot ${kb(min)}.`;
    if (low) {
        text += ` Pairing needs about ${kb(PAIRING_HEAP)} and may fail: switch heavy modules off on the MODULES panel.`;
    }
    return {text, low};
}

function showHeap() {
    const out = document.querySelector(".matter-heap");
    if (!(out instanceof HTMLElement) || !HEAP.now) {
        return;
    }
    const {text, low} = heapLine(HEAP.now, HEAP.min);
    out.textContent = text;
    out.style.color = low ? "#c0392b" : "";
}

export function init() {
    styleInject(STYLE);

    variableListeners({
        matterQr: (_, value) => {
            drawQr(String(value ?? ""));
        },
        matterStarted: (_, value) => {
            updateStarted(value === true || value === "yes");
        },
        matterControllers: (_, value) => {
            updateControllers(value);
        },
        matterWindow: (_, value) => {
            updateWindow(Number(value) || 0);
        },
        matterHeap: (_, value) => {
            HEAP.now = Number(value) || 0;
            showHeap();
        },
        matterHeapMin: (_, value) => {
            HEAP.min = Number(value) || 0;
            showHeap();
        },
        matterIdUse: (_, value) => {
            IDENTITY.use = String(value ?? "");
            showIdentity();
        },
        matterIdFactory: (_, value) => {
            IDENTITY.factory = (value === true) || (value === "true") || (value === 1);
            showIdentity();
        },
        matterIdCustom: (_, value) => {
            IDENTITY.custom = String(value ?? "");
            showIdentity();
        },
    });

    element(".matter-identity-use")
        ?.addEventListener("change", showIdentityNote);

    element(".button-matter-identity")
        ?.addEventListener("click", (event) => {
            event.preventDefault();
            const select = element(".matter-identity-use");
            if (!(select instanceof HTMLSelectElement)) {
                return;
            }

            const use = select.value;
            if (use === IDENTITY.use) {
                window.alert(`Matter uses ${use} already.`);
                return;
            }

            if (!window.confirm(identityConfirmText(use))) {
                select.value = IDENTITY.use;
                return;
            }

            sendAction("matterIdentity", {use});
        });

    element(".button-matter-identity-upload")
        ?.addEventListener("click", (event) => {
            event.preventDefault();
            uploadIdentity()
                .catch((error) => window.alert(`Reading the files failed: ${error?.message ?? error}`));
        });

    showIdentity();

    element(".button-matter-start")
        ?.addEventListener("click", (event) => {
            event.preventDefault();
            sendAction("matterStart");
        });

    element(".button-matter-window")
        ?.addEventListener("click", (event) => {
            event.preventDefault();
            if (!WINDOW.started || (WINDOW.left > 0) || (WINDOW.opening !== null)) {
                return;
            }
            // "Opening..." at once, the countdown comes with the open event of the device
            WINDOW.opening = setTimeout(() => {
                WINDOW.opening = null;
                showWindow();
            }, OPENING_MS);
            showWindow();
            sendAction("matterWindow");
        });

    updateStarted(false);
}
