import { MODULE_DEV } from '@build-mode';

import { notifyError, notifyErrorEvent } from './notify.mjs';
window.addEventListener("error", (event) => {
    notifyErrorEvent(event);
    console.error(event.error);
});

import {
    onMenuLinkClick,
    onPanelTargetClick,
    pageReloadIn,
    modulesVisible,
    showPanelByName,
} from './core.mjs';

import { askAndCall } from './question.mjs';

import {
    init as initSettings,
    askSaveSettings,
    updateVariables,
    variableListeners,
} from './settings.mjs';
import { init as initPassword } from './password.mjs';

import { init as initWiFi } from './wifi.mjs';
import { init as initGpio } from './gpio.mjs';
import {
    init as initConnection,
    connect,
    sendAction,
} from './connection.mjs';

import { init as initDebug } from './debug.mjs';
import { init as initModules, modulesApplyRunning } from './modules.mjs';
import { init as initOta } from './ota.mjs';
import { installRunning } from './install.mjs';

import { init as initDev } from './dev.mjs';
import { initParts, loadParts } from './parts.mjs';
import { init as initTasmota } from './tasmota.mjs';
import { init as initTemplates } from './templates.mjs';

/** @type {number | null} */
let KeepTime = null;

/** @type {number} */
let Ago = 0;

/**
 * @typedef {{date: Date | null, offset: string}} NowType
 * @type NowType
 */
const Now = {
    date: null,
    offset: "",
};

/**
 * @type {{[k: string]: string}}
 */
const __title_cache = {
    hostname: "?",
    app_name: "ESPurna",
    app_version: "0.0.0",
};

/**
 * @param {string} key
 * @param {string} value
 */
function documentTitle(key, value) {
    __title_cache[key] = value;
    document.title = `${__title_cache.hostname} - ${__title_cache.app_name} ${__title_cache.app_version}`;
}

function modulesVisibleAll() {
    document.querySelectorAll("[class*=module-]")
        .forEach((elem) => {
            /** @type {HTMLElement} */(elem).style.display = "revert";
        });
}

/**
 * @param {string} value
 */
function deviceNow(value) {
    try {
        Now.date = normalizedDate(value);
        Now.offset = timestampOffset(value);
    } catch (e) {
        notifyError(/** @type {Error} */(e));
    }
}

/**
 * @param {string} value
 */
function onAction(value) {
    if ("reload" === value) {
        pageReloadIn(1000);
    }
}

/**
 * @param {string} value
 */
function onMessage(value) {
    window.alert(value);
}

/**
 * While the page has no data from the board yet (the layout stays hidden until then): what is going on, and a way out
 * when the web socket never comes
 */
function connectingShow() {
    const box = document.createElement("div");
    box.id = "connecting";
    box.style.cssText = "font: 120% sans-serif; color: #555; text-align: center; margin-top: 30vh;";
    box.textContent = "Connecting to the board...";
    document.body.appendChild(box);
    window.setTimeout(() => connectingFailed("No answer from the board yet."), 15000);
}

/** @param {string} text */
function connectingFailed(text) {
    const box = document.getElementById("connecting");
    if (!box) {
        return;
    }
    box.textContent = text;
    const retry = document.createElement("button");
    retry.className = "pure-button";
    retry.style.cssText = "display: block; margin: 1em auto;";
    retry.textContent = "Retry";
    retry.addEventListener("click", () => window.location.reload());
    box.appendChild(retry);
}

/**
 * @param {number} value
 */
function initWebMode(value) {
    document.getElementById("connecting")?.remove();
    const layout = /** @type {!HTMLElement} */
        (document.getElementById("layout"));
    layout.style.display = "inherit";

    if (1 === value) {
        layout.classList.add("initial");
    }

    showPanelByName(
        (1 === value) ? "password" : "status");
}

/**
 * @param {string} value
 * @returns {string}
 */
function timestampDatetime(value) {
    return value.slice(0, 19);
}

/**
 * @param {string} value
 * @returns {string}
 */
function timestampOffset(value) {
    if (value.endsWith("Z")) {
        return "Z";
    }

    return value.slice(-6);
}

/**
 * @param {NowType} now
 * @returns {string}
 */
function displayDatetime(now) {
    if (now.date) {
        let datetime = timestampDatetime(now.date.toISOString());
        datetime = datetime.replace("T", " ");
        datetime = `${datetime} ${now.offset}`;
        return datetime;
    }

    return "?";
}

/**
 * @param {string} value
 * @returns {string}
 */
function normalizedTimestamp(value) {
    return `${timestampDatetime(value)}Z`;
}

/**
 * @param {string} value
 * @returns {Date}
 */
function normalizedDate(value) {
    return new Date(normalizedTimestamp(value));
}


function keepTime() {
    const ago = document.querySelector("span[data-key='app:ago']");
    if (ago) {
        ago.textContent = Ago.toString();
    }

    ++Ago;

    if (null !== Now.date) {
        const now = document.querySelector("span[data-key='app:now']");
        if (now) {
            now.textContent = displayDatetime(Now);
        }

        Now.date = new Date(Now.date.valueOf() + 1000);
    }
}

/** @import { QuestionWrapper } from './question.mjs' */

/** @type {QuestionWrapper} */
function askDisconnect(ask) {
    return ask("Are you sure you want to disconnect from the current WiFi network?");
}

/** @type {QuestionWrapper} */
function askReboot(ask) {
    return ask("Are you sure you want to reboot the device?");
}

/** @param {CloseEvent} event */
function askReload(event) {
    /** @type {QuestionWrapper} */
    return function(ask) {
        return ask(`Connection lost with the device - ${event.reason}. Click OK to refresh the page.`);
    };
}

function askAndCallReconnect() {
    askAndCall([askSaveSettings, askDisconnect], () => {
        sendAction("reconnect");
    });
}

function askAndCallReboot() {
    askAndCall([askSaveSettings, askReboot], () => {
        sendAction("reboot");
    });
}

/** @param {Event} event */
function askAndCallSimpleAction(event) {
    const target = event.target;
    if (!(target instanceof HTMLButtonElement)) {
        return;
    }

    /** @type {QuestionWrapper} */
    const wrapper =
        (ask) => ask(`Confirm the action: "${target.textContent}"`);

    askAndCall([wrapper], () => {
        sendAction(target.name);
    });
}

// TODO https://github.com/microsoft/TypeScript/issues/58969
// at-import'ed type becomes 'unused' for some reason
// until fixed, prefer direct import vs. typedef

/**
 * @returns {import('./settings.mjs').KeyValueListeners}
 */
function listeners() {
    return {
        "action": (_, value) => {
            onAction(value);
        },
        "app_name": documentTitle,
        "app_version": documentTitle,
        "hostname": documentTitle,
        "message": (_, value) => {
            onMessage(value);
        },
        "modulesVisible": (_, value) => {
            modulesVisible(value);
        },
        "now": (_, value) => {
            deviceNow(value);
        },
        "webMode": (_, value) => {
            initWebMode(value);
        },
    };
}

/**
 * @param {CloseEvent} event
 */
function onConnectionClose(event) {
    // an install (firmware panel, Browse / Upgrade of a .ota, MODULES Apply) waits for the reboots itself
    if (installRunning()) {
        return;
    }

    // the MODULES Apply restarts the board twice and waits for it itself: a confirm() here would block that page
    if (modulesApplyRunning()) {
        return;
    }

    // never got any data: no question, the Retry button
    if (document.getElementById("connecting")) {
        connectingFailed("The connection to the board closed.");
        return;
    }

    askAndCall([askReload(event)], () => {
        pageReloadIn(1000);
    });
}

/**
 * @param {MessageEvent<any>} event
 */
function onJsonPayload(event) {
    Ago = 0;

    if (!KeepTime) {
        KeepTime = window.setInterval(keepTime, 1000);
    }

    try {
        const parsed = JSON.parse(
            event.data
                .replace(/:Infinity/g, ':"inf"')
                .replace(/:-Infinity/g, ':"-inf"')
                .replace(/:NaN/g, ':"nan"')
                .replace(/\n/g, "\\n")
                .replace(/\r/g, "\\r")
                .replace(/\t/g, "\\t"));
        updateVariables(parsed);
    } catch (e) {
        notifyError(/** @type {Error} */(e));
    }
}

async function init() {
    connectingShow();

    // the parts of the modules first: the panels, templates and scripts the rest of the page sets up
    await loadParts(new URL("./", window.location.href));

    // Sidebar menu & buttons
    document.querySelector(".menu-link")
        ?.addEventListener("click", onMenuLinkClick);
    document.querySelectorAll("[data-panel]")
        .forEach((elem) => {
            elem.addEventListener("click", onPanelTargetClick);
        });

    document.querySelector(".button-reconnect")
        ?.addEventListener("click", askAndCallReconnect);
    document.querySelectorAll(".button-reboot")
        .forEach((elem) => {
            elem.addEventListener("click", askAndCallReboot);
        });

    // Generic action sender
    document.querySelectorAll(".button-simple-action")
        .forEach((elem) => {
            elem.addEventListener("click", askAndCallSimpleAction);
        });

    variableListeners(listeners());

    if (!MODULE_DEV) {
        initConnection();
    }

    initSettings();
    initPassword();
    initWiFi();
    initGpio();
    initTasmota();
    initTemplates();

    // the head's own panels (firmware, MODULES); every other panel is the part of a module (src/parts.mjs)
    initOta();
    initDebug();
    initModules();
    initParts();

    if (MODULE_DEV) {
        initDev();
        KeepTime = window.setInterval(keepTime, 1000);
        modulesVisibleAll();
        return;
    }

    // don't autoconnect w/ localhost or file://
    connect({onclose: onConnectionClose, onmessage: onJsonPayload});
}

document.addEventListener("DOMContentLoaded", init);
