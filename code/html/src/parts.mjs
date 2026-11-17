// The parts of the WebUI the modules of the board bring (U31, html/parts.mjs): GET ui names them in the order the
// modules started, GET ui/part?name=<name> is the fragment of one, out of the module's flash. A fragment is
//   <template data-into="status|panels|templates">...</template>   put in before the marker [data-parts=...] of index.html
//   <style>...</style>                                               into the head
//   <script>...</script>                                             the part's script (gulp: its .mjs bundled, the
//                                                                    head's files from window.EspurnaUi), it calls
//                                                                    window.EspurnaUi.register(name, init)
//   <script data-src="relay.mjs"></script>                           the dev server's: the .mjs itself, imported
// Every part goes in before anything of the page starts (settings, templates, the WebSocket): the page is then the
// same as the one with every panel inline was.

import * as connection from './connection.mjs';
import * as core from './core.mjs';
import * as errors from './errors.mjs';
import * as notify from './notify.mjs';
import * as password from './password.mjs';
import * as password_utils from './password/utils.mjs';
import * as question from './question.mjs';
import * as settings from './settings.mjs';
import * as settings_change from './settings/change.mjs';
import * as settings_dataset from './settings/dataset.mjs';
import * as settings_enumerable from './settings/enumerable.mjs';
import * as settings_group_elem from './settings/group.elem.mjs';
import * as settings_group from './settings/group.mjs';
import * as settings_input from './settings/input.mjs';
import * as settings_name from './settings/name.mjs';
import * as settings_select from './settings/select.mjs';
import * as settings_span from './settings/span.mjs';
import * as settings_template from './settings/template.mjs';
import * as settings_utils from './settings/utils.mjs';
import * as settings_value from './settings/value.mjs';
import * as template from './template.mjs';
import * as validate from './validate.mjs';
import * as validate_utils from './validate/utils.mjs';

/**
 * @typedef {{name: string, init: (() => void) | undefined}} Registered
 */

/** @type {Registered[]} */
const registered = [];

/**
 * @param {string} name
 * @param {(() => void) | undefined} init
 */
export function register(name, init) {
    registered.push({name, init});
}

// the head's files of the page, one copy for every part (html/parts.mjs CORE, the same names)
export const EspurnaUi = {
    register,
    connection,
    core,
    errors,
    notify,
    password,
    password_utils,
    question,
    settings,
    settings_change,
    settings_dataset,
    settings_enumerable,
    settings_group_elem,
    settings_group,
    settings_input,
    settings_name,
    settings_select,
    settings_span,
    settings_template,
    settings_utils,
    settings_value,
    template,
    validate,
    validate_utils,
};

/** @type {any} */(window).EspurnaUi = EspurnaUi;

/**
 * @param {string} text
 * @param {URL} base
 */
async function insert(text, base) {
    const holder = document.createElement("template");
    holder.innerHTML = text;

    for (const elem of Array.from(holder.content.children)) {
        if (elem instanceof HTMLTemplateElement && elem.dataset["into"]) {
            const marker = document.querySelector(`[data-parts="${elem.dataset["into"]}"]`);
            marker?.before(elem.content);
        } else if (elem instanceof HTMLStyleElement) {
            document.head.appendChild(elem);
        } else if (elem instanceof HTMLScriptElement) {
            const src = elem.dataset["src"];
            if (src) {
                const loaded = await import(/* @vite-ignore */ new URL(src, base).href);
                register(src, loaded.init);
            } else {
                // a script made here runs right away (one put in by innerHTML never does)
                const script = document.createElement("script");
                script.textContent = elem.textContent;
                document.head.appendChild(script);
                script.remove();
            }
        }
    }
}

// a busy board (another tab loading, low heap) resets the connection or answers 5xx / 429: tried again a few times
// before the part is given up (W70: the second WebUI tab of an ESP8266 lost its relay part to one reset)
const FetchTries = 5;
const FetchPauseMs = 1000;

/**
 * @param {URL} url
 * @param {RequestInit} options
 * @param {string} what
 * @returns {Promise<{text: string | null, status: number}>} the body, null with the status of a final answer (4xx)
 */
async function fetchText(url, options, what) {
    /** @type {Error | null} */
    let last = null;
    for (let attempt = 0; attempt < FetchTries; ++attempt) {
        if (attempt) {
            await new Promise((resolve) => setTimeout(resolve, FetchPauseMs * attempt));
        }

        try {
            const response = await fetch(url, options);
            if (response.ok) {
                return {text: await response.text(), status: response.status};
            }

            if ((response.status !== 429) && (response.status < 500)) {
                return {text: null, status: response.status};
            }

            last = new Error(`${what}: HTTP ${response.status}`);
        } catch (e) {
            last = /** @type {Error} */(e);
        }
    }

    throw last ?? new Error(what);
}

/**
 * Every part of the board, one after another (an ESP8266 serves one request at a time well). An error leaves the page
 * without that part, the rest works
 * @param {URL} base
 * @returns {Promise<string[]>} the names of the parts that went in
 */
export async function loadParts(base) {
    /** @type {string[]} */
    const done = [];

    /** @type {string[]} */
    let names;
    try {
        const list = await fetchText(new URL("ui", base),
            {cache: "no-store", credentials: "same-origin"}, "the list of the WebUI parts");
        if (list.text === null) {
            return done;
        }
        names = JSON.parse(list.text);
    } catch (e) {
        notify.notifyError(/** @type {Error} */(e));
        return done;
    }

    for (const name of names) {
        try {
            const part = await fetchText(new URL(`ui/part?name=${encodeURIComponent(name)}`, base),
                {credentials: "same-origin"}, `the WebUI part of ${name}`);
            if (part.text === null) {
                throw new Error(`the WebUI part of ${name}: HTTP ${part.status}`);
            }
            await insert(part.text, base);
            done.push(name);
        } catch (e) {
            notify.notifyError(/** @type {Error} */(e));
        }
    }

    return done;
}

// after the head's own init(): the parts' init() in the order they came
export function initParts() {
    for (const part of registered) {
        part.init?.();
    }
}
