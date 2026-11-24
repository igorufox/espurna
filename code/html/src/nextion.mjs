import { listenAppConnected, sendAction } from './connection.mjs';
import { styleInject } from './core.mjs';
import { variableListeners } from './settings.mjs';
import { DESIGN, Screen } from './nextion-screen.mjs';

// The NEXTION panel (the nextion module: a Nextion display on a UART, the NSPanel's): the active driver, the TFT upload
// and the mirror of the display. The pages it shows are the PAGES panel's (src/pages.mjs)

const STYLE = [
    // the NSPanel EU's frame hides the rightmost 30 px of the 480x320 display
    `.nextion-screen-frame { width: 100%; max-width: 675px; aspect-ratio: 450 / 320; overflow: hidden;
        border: 10px solid #222; border-radius: 12px; background: #000; }`,
    ".nextion-screen { display: block; width: 106.667%; aspect-ratio: 3 / 2; touch-action: none; cursor: pointer; }",
    ".nextion-screen-note { color: #777; font-size: 12px; margin: 4px 0 0; }",
];

/** @type {{status: URL} | null} */
let __urls = null;

const FETCH_OPTIONS = {
    credentials: "same-origin",
    cache: "no-store",
};

/**
 * @param {any} status
 * @returns {string}
 */
function formatDriver(status) {
    const parts = [String(status?.driver ?? "none")];
    if (status?.connected) {
        parts.push(`connected, ${status.version ?? "?"}`);
    } else if (status?.probing) {
        parts.push("detecting");
    } else {
        parts.push("not connected");
    }
    if (status?.design) {
        parts.push(String(status.design));
    }
    return parts.join(", ");
}

async function updateStatus() {
    const elem = document.querySelector("#panel-nextion .nextion-status");
    if (!__urls || !(elem instanceof HTMLElement)) {
        return;
    }

    try {
        const response = await fetch(__urls.status, FETCH_OPTIONS);
        if (!response.ok) {
            elem.textContent = `${response.status} ${response.statusText}`;
            return;
        }

        const status = await response.json();
        __status = status;
        elem.textContent = formatDriver(status);
        showFlash(status);
    } catch (e) {
        elem.textContent = `unavailable (${(e instanceof Error) ? e.message : String(e)})`;
    }
}

// -----------------------------------------------------------------------------
// Flash the screen: the TFT by its URL (nextion_flash.cpp), the progress from the status
// -----------------------------------------------------------------------------

/** @type {ReturnType<typeof setTimeout> | null} */
let __flash_timer = null;

/**
 * @param {any} status
 */
function showFlash(status) {
    const elem = document.querySelector("#panel-nextion .nextion-flash");
    if (elem instanceof HTMLElement) {
        elem.textContent = String(status?.flash ?? "?");
    }

    if (__flash_timer) {
        clearTimeout(__flash_timer);
        __flash_timer = null;
    }

    // every 3 s while it runs
    if (status?.flashing) {
        __flash_timer = setTimeout(() => {
            __flash_timer = null;
            updateStatus();
        }, 3000);
    }
}

function onFlash() {
    const input = document.querySelector("#panel-nextion .nextion-tft-url");
    const url = (input instanceof HTMLInputElement) ? input.value.trim() : "";
    if (!url) {
        return;
    }

    if (!window.confirm(`Write ${url} into the display? What the screen has now is gone, it can not be read back.`)) {
        return;
    }

    sendAction("nspanelFlash", {url});
}

/**
 * the board's answer to "nspanelFlash": "started", or why not
 * @param {string} _
 * @param {any} value
 */
function onFlashAnswer(_, value) {
    const elem = document.querySelector("#panel-nextion .nextion-flash");
    if (elem instanceof HTMLElement) {
        elem.textContent = String(value);
    }

    if (value === "started") {
        setTimeout(updateStatus, 1000);
    }
}

// -----------------------------------------------------------------------------
// Screen mirror (nextion-screen.mjs): what the display shows, touched like the display
// -----------------------------------------------------------------------------

/** @type {any} */
let __status = null;

/** @type {Screen | null} */
let __screen = null;

/** @type {Promise<void> | null} */
let __screen_loading = null;

/** @param {string} text */
function screenNote(text) {
    const note = document.querySelector("#panel-nextion .nextion-screen-note");
    if (note instanceof HTMLElement) {
        note.textContent = text;
    }
}

/**
 * whether the mirror can draw the panel's display: the OHpanel driver on the stock design it knows
 * @param {any} status
 * @returns {string} empty when it can, else why not
 */
export function screenUnsupported(status) {
    if (status?.driver !== "ohpanel") {
        return "The screen mirror needs the OHpanel display driver.";
    }
    if (status?.version !== DESIGN) {
        return `The screen mirror knows the stock OHpanel design ${DESIGN}, the display has ${status?.version ?? "?"}.`;
    }
    return "";
}

async function screenStart() {
    const canvas = document.querySelector("#panel-nextion .nextion-screen");
    if (!(canvas instanceof HTMLCanvasElement) || !(canvas.parentElement instanceof HTMLElement) || !__status) {
        return;
    }

    const why = screenUnsupported(__status);
    canvas.parentElement.style.display = why ? "none" : "";
    screenNote(why);
    if (why) {
        return;
    }

    if (!__screen) {
        __screen = new Screen(canvas, sendAction);
        __screen_loading = __screen.load().catch((e) => {
            screenNote(`The design's pictures and fonts could not be loaded: ${(e instanceof Error) ? e.message : String(e)}`);
        });
    }

    await __screen_loading;
    sendAction("nspanelScreen");
}

/**
 * @param {string} _
 * @param {any} value
 */
function onScreenCommands(_, value) {
    if (__screen && (typeof value === "string")) {
        __screen.feed(value);
    }
}

function onPanelShown() {
    updateStatus().then(screenStart);
}

export function init() {
    styleInject(STYLE);

    listenAppConnected((urls) => {
        __urls = {
            status: new URL("nspanel-status", urls.config),
        };
    });

    document.querySelector(".button-nextion-flash")
        ?.addEventListener("click", onFlash);
    // a choice of the list goes into the URL field, which also takes any other .tft
    document.querySelector(".nextion-tft-choice")
        ?.addEventListener("change", (event) => {
            const input = document.querySelector("#panel-nextion .nextion-tft-url");
            if ((event.target instanceof HTMLSelectElement) && (input instanceof HTMLInputElement)) {
                input.value = event.target.value;
            }
        });

    variableListeners({
        nspanelTft: onScreenCommands,
        nspanelFlash: onFlashAnswer,
    });

    document.querySelectorAll("[data-panel='nextion']")
        .forEach((elem) => {
            elem.addEventListener("click", onPanelShown);
        });
}
