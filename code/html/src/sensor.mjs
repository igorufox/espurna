import { sendAction } from './connection.mjs';

import { showPanelByName } from './core.mjs';

import { resetGroupElement } from './settings/group.elem.mjs';

import {
    addEnumerables,
    listenEnumerableName,
    listenEnumerableTarget,
    prepareEnumerableTarget,
} from './settings/enumerable.mjs';

import { setOriginalsFromValuesForNode } from './settings/dataset.mjs';

import {
    initElementOptions,
    setSelectValue,
} from './settings/select.mjs';

import { setInputValue } from './settings/input.mjs';

import { variableListeners } from './settings.mjs';

import {
    fromSchema,
    NumberInput,
} from './template.mjs';

import {
    loadTemplate,
    mergeTemplate,
} from './settings/template.mjs';

/**
 * @typedef Magnitude
 * @property {string} description
 * @property {number} index_global
 * @property {string} name
 * @property {number} type
 * @property {number} units
*/

/** @typedef {[number, string]} SupportedUnits */

const Magnitudes = {
    /** @type {Map<number, Magnitude>} */
    properties: new Map(),

    /** @type {Map<number, string>} */
    errors: new Map(),

    /** @type {Map<number, string>} */
    types: new Map(),

    /** @type {Map<number, string>} */
    units: new Map(),

    /** @type {Map<number, number[]>} */
    supportedUnits: new Map(),

    /** @type {Map<number, string>} */
    typePrefix: new Map(),

    /** @type {Map<string, number>} */
    prefixType: new Map(),

    /** @type {boolean} */
    pending: false,
};

/**
 * @param {number} type
 * @param {number} index
 * @param {string} name
 */
function magnitudeTypedKey(type, index, name) {
    const prefix = Magnitudes.typePrefix.get(type);
    return `${prefix}${name}${index}`;
}

/**
 * @param {number} type
 */
function magnitudeTypeName(type) {
    return Magnitudes.types.get(type)
        ?? type.toString();
}

/**
 * @param {number} type
 * @param {number} index
 */
function magnitudeName(type, index) {
    return `${magnitudeTypeName(type)} #${index}`;
}

/**
 * @param {number} type
 * @param {number} index
 * @returns {NodeListOf<HTMLInputElement>}
 */
function magnitudeValueSelector(type, index) {
    return document.querySelectorAll(`input[name='magnitude:${magnitudeTypedKey(type, index, "")}`)
}

/**
 * @param {HTMLElement} elem
 * @param {number} id
 */
function listenEnumerableMagnitudeDescription(elem, id) {
    prepareEnumerableTarget(elem, id, "magnitude");
    listenEnumerableName(elem, "magnitude",
        (elem) => {
            elem.textContent =
                Magnitudes.properties.get(id)?.description ?? "";
        });
}

/**
 * @param {string} prefix
 * @param {any[][]} values
 * @param {string[]} schema
 */
function initModuleMagnitudes(prefix, values, schema) {
    const container = document.getElementById(`${prefix}-magnitudes`);
    if (!container || container.childElementCount > 0) {
        return;
    }

    const template = new NumberInput();

    values.forEach((value, id) => {
        const magnitude = fromSchema(value, schema);
        const merged = mergeTemplate(container, template.with(
            (label, input, span) => {
                listenEnumerableTarget(label, id, "magnitude");

                input.min = "0";
                input.name = `${prefix}Magnitude`;
                input.required = true;
                input.value = /** @type {!number} */
                    (magnitude.index_module).toString();

                listenEnumerableMagnitudeDescription(span, id);
            }));
        setOriginalsFromValuesForNode(merged);
    });
}

// Poll remote for magnitudes initial setup
function pendingMagnitudes() {
    if (Magnitudes.pending) {
        sendAction("magnitudes-pending");
        window.setTimeout(pendingMagnitudes, 5000);
    }
}

function waitPendingMagnitudes() {
    Magnitudes.pending = true;
    pendingMagnitudes();
}

/**
 * @param {any} types
 * @param {any} errors
 * @param {any} units
 */
function initMagnitudes(types, errors, units) {
    /** @type {[number, string, string][]} */
    (types.values).forEach((value) => {
        const info = fromSchema(value, types.schema);

        const type = /** @type {number} */(info.type);
        Magnitudes.types.set(type,
            /** @type {string} */(info.name));

        const prefix = /** @type {string} */(info.prefix);
        Magnitudes.typePrefix.set(type, prefix);
        Magnitudes.prefixType.set(prefix, type);

        const supported_units = /** @type {number[]} */(info.units);
        Magnitudes.supportedUnits.set(type, supported_units);
    });

    /** @type {[number, string][]} */
    (errors.values).forEach((value) => {
        const error = fromSchema(value, errors.schema);
        Magnitudes.errors.set(
            /** @type {number} */(error.type),
            /** @type {string} */(error.name));
    });

    /** @type {SupportedUnits[]} */
    (units.values).forEach((value) => {
        const unit = fromSchema(value, units.schema);
        Magnitudes.units.set(
            /** @type {number} */(unit.type),
            /** @type {string} */(unit.name));
    });
}

/**
 * @typedef {function(number, Magnitude): void} MagnitudeCallback
 * @param {any[]} values
 * @param {string[]} schema
 * @param {MagnitudeCallback[]} callbacks
 */
function initMagnitudesList(values, schema, callbacks) {
    /** @import { EnumerableNames } from './settings/enumerable.mjs' */

    /** @type {EnumerableNames} */
    const names = {};

    values.forEach((value, id) => {
        const magnitude = fromSchema(value, schema);

        const description = /** @type {string} */
            (magnitude.description);
        const index_global = /** @type {number} */
            (magnitude.index_global);
        const type = /** @type {number} */
            (magnitude.type);
        const units = /** @type {number} */
            (magnitude.units);

        const name = magnitudeName(type, index_global);

        /** @type {Magnitude} */
        const result = {
            description,
            index_global,
            name,
            type,
            units,
        };

        names[id.toString()] = name;

        Magnitudes.properties.set(id, result);
        callbacks.forEach((callback) => {
            callback(id, result);
        });
    });

    addEnumerables("magnitude", names);
    Magnitudes.pending = false;
}

/**
 * @param {number} _id
 * @param {Magnitude} magnitude
 */
function createMagnitudeInfo(_id, magnitude) {
    const container = document.getElementById("magnitudes");
    if (!container) {
        return;
    }

    const line = loadTemplate("magnitude-status");

    const label = /** @type {!HTMLLabelElement} */
        (line.querySelector("label"));
    label.textContent = magnitude.name;

    const input = /** @type {!HTMLInputElement} */
        (line.querySelector("input[name='magnitude:']"));
    input.name +=
        magnitudeTypedKey(magnitude.type, magnitude.index_global, "");

    const info = /** @type {!HTMLSpanElement} */
        (line.querySelector(".magnitude-info"));
    info.style.display = "none";

    const description = /** @type {!HTMLSpanElement} */
        (line.querySelector(".magnitude-description"));
    description.textContent = magnitude.description;

    mergeTemplate(container, line);
}

/**
 * @param {number} _id
 * @param {Magnitude} magnitude
 */
function createMagnitudeUnitSelector(_id, magnitude) {
    const container = document.getElementById("magnitude-units");
    if (!container) {
        return;
    }

    const line = loadTemplate("magnitude-units");

    const label = /** @type {!HTMLLabelElement} */
        (line.querySelector("label"));
    label.textContent =
        magnitudeName(magnitude.type, magnitude.index_global);

    const select = /** @type {!HTMLSelectElement} */
        (line.querySelector("select"));
    select.setAttribute("name",
        magnitudeTypedKey(magnitude.type, magnitude.index_global, "Units"));

    const options = (Magnitudes.supportedUnits.get(magnitude.type) ?? [])
        .map((type) => ({
            'value': type.toString(),
            'text': Magnitudes.units.get(type) ?? type.toString()
        }));

    initElementOptions(select, options);
    setSelectValue(select, magnitude.units);

    container?.parentElement?.classList?.remove("maybe-hidden");
    const merged = mergeTemplate(container, line);
    setOriginalsFromValuesForNode(merged);
}

/**
 * @typedef SettingInfo
 * @property {number} id
 * @property {number} index_global
 * @property {string} key
 * @property {string} name
 * @property {string} prefix
 * @property {string} description
 */

/**
 * @param {number} id
 * @param {string} suffix
 * @returns {SettingInfo | null}
 */
function magnitudeSettingInfo(id, suffix) {
    const props = Magnitudes.properties.get(id);
    if (!props) {
        return null;
    }

    const prefix = Magnitudes.typePrefix.get(props.type);
    if (!prefix) {
        return null;
    }

    return {
        id: id,
        index_global: props.index_global,
        key: `${prefix}${suffix}${props.index_global}`,
        name: props.name,
        prefix,
        description: props.description,
    };
}

/**
 * @param {number} id
 * @returns {SettingInfo | null}
 */
function emonRatioInfo(id) {
    return magnitudeSettingInfo(id, "Ratio");
}

/** @typedef {{required?: boolean, min?: string, max?: string}} MagnitudeNumberOptions */

/**
 * @param {string} containerId
 * @param {number} id
 * @param {string} keySuffix
 * @param {number} value
 * @param {MagnitudeNumberOptions} options
 */
function initMagnitudeNumberSetting(containerId, id, keySuffix, value, {required = true, min = "", max = ""} = {}) {
    const container = document.getElementById(containerId);
    if (!container) {
        return;
    }

    const info = magnitudeSettingInfo(id, keySuffix);
    if (!info) {
        return;
    }

    container?.parentElement?.classList?.remove("maybe-hidden");

    const template = new NumberInput();
    const merged = mergeTemplate(container, template.with(
        (label, input, span) => {
            label.textContent = info.name;
            label.htmlFor = info.key;

            input.id = info.key;
            input.name = info.key;
            input.required = required;
            input.min = min;
            input.max = max;
            input.value = value.toString();

            resetGroupElement(input); // TODO info key w/o index
            listenEnumerableMagnitudeDescription(span, id);
        }));
    setOriginalsFromValuesForNode(merged);
}

/**
 * @param {number} id
 */
function initMagnitudesExpected(id) {
    const container = document.getElementById("emon-expected");
    if (!container) {
        return;
    }

    const info = emonRatioInfo(id);
    if (!info) {
        return;
    }

    const template = loadTemplate("emon-expected");

    const [magnitude, result, expected] =
        template.querySelectorAll("input");

    /** @type {!HTMLLabelElement} */
        (magnitude.previousElementSibling).textContent = info.name;
    /** @type {!HTMLSpanElement} */
        (magnitude.nextElementSibling).textContent = info.description;
    magnitude.name +=
        `${info.prefix}${info.index_global}`;

    result.name += info.key;
    result.id = result.name;

    expected.name += info.key;
    expected.id = expected.name;
    expected.dataset["id"] = info.id.toString();
    expected.dataset["action"] = "none";

    const [message] = /** @type {NodeListOf<HTMLSpanElement>} */
        (template.querySelectorAll(`span.emon-expected-${info.prefix}`));
    if (message) {
        message.classList.replace("hidden", "visible");
    }

    mergeTemplate(container, template);
}

function emonCalculateRatios() {
    const expected = document.getElementById("emon-expected")
        ?.querySelectorAll("input.emon-expected-input");
    if (!expected) {
        return;
    }

    /** @type {NodeListOf<HTMLInputElement>} */
    (expected).forEach((input) => {
        if (!input.value || !input.dataset["id"]) {
            return;
        }

        sendAction("emon-expected", {
            id: parseInt(input.dataset["id"], 10),
            expected: parseFloat(input.value) });
    });
}

function emonApplyRatios() {
    const results = document.getElementById("emon-expected")
        ?.querySelectorAll("input.emon-expected-result");

    /** @type {NodeListOf<HTMLInputElement>} */
    (results).forEach((result) => {
        if (!result.value) {
            return;
        }

        let next = result.name
            .replace("result:", "");

        const ratio = document.getElementById(next);
        if (!(ratio instanceof HTMLInputElement)) {
            return;
        }

        ratio.value = result.value;
        ratio.dispatchEvent(new Event("change"));

        result.value = "";

        next = result.name
            .replace("result:", "expected:");
        const expected = document.getElementById(next);
        if (!(expected instanceof HTMLInputElement)) {
            return;
        }

        expected.value = "";
    });

    showPanelByName("sns");
}

/**
 * @param {any[]} values
 * @param {string[]} schema
 */
function initMagnitudesSettings(values, schema) {
    values.forEach((value, id) => {
        const settings = fromSchema(value, schema);

        if (typeof settings.Ratio === "number") {
            initMagnitudeNumberSetting(
                "emon-ratios", id,
                "Ratio", settings.Ratio);
            initMagnitudesExpected(id);
        }

        if (typeof settings.Correction === "number") {
            initMagnitudeNumberSetting(
                "magnitude-corrections", id,
                "Correction", settings.Correction);
        }

        if (typeof settings.MinDelta === "number") {
            initMagnitudeNumberSetting(
                "magnitude-min-deltas", id,
                "MinDelta", settings.MinDelta, {min: "0"});
        }

        if (typeof settings.MaxDelta === "number") {
            initMagnitudeNumberSetting(
                "magnitude-max-deltas", id,
                "MaxDelta", settings.MaxDelta, {min: "0"});
        }

        for (let type of ["Min", "Max", "Zero"]) {
            const key = `${type}Threshold`;
            const threshold =
                (typeof settings[key] === "number")
                    ? settings[key] :
                (typeof settings[key] === "string")
                    ? NaN : null;

            if (typeof threshold === "number") {
                initMagnitudeNumberSetting(
                    `magnitude-${type.toLowerCase()}-thresholds`, id,
                    key, threshold, {
                        required: false,
                        min: (type === "Zero") ? "0" : undefined
                    });
            }
        }
    });
}

/**
 * @param {any[]} values
 * @param {string[]} schema
 */
function updateMagnitudes(values, schema) {
    values.forEach((value, id) => {
        const props = Magnitudes.properties.get(id);
        if (!props) {
            return;
        }

        const magnitude = fromSchema(value, schema);
        if (typeof magnitude.units === "number") {
            props.units = magnitude.units;
        }

        let inputValue = "";
        if (typeof magnitude.error === "number" && 0 !== magnitude.error) {
            inputValue =
                Magnitudes.errors.get(magnitude.error) ?? "Unknown error";
        } else if (typeof magnitude.value === "string") {
            const units = Magnitudes.units.get(
                /** @type {number} */(magnitude.units)) ?? "";
            inputValue = `${magnitude.value}${units}`;
        } else {
            inputValue = magnitude.value?.toString() ?? "?";
        }

        magnitudeValueSelector(props.type, props.index_global)
            .forEach((input) => { input.value = inputValue; });
    });
}

/**
 * @param {HTMLInputElement} input
 * @param {string} saved
 */
function updateEnergyInput(input, saved) {
    const info = input.nextElementSibling;
    if (info instanceof HTMLElement && info.matches("span.magnitude-info")) {
        info.style.display = "inherit";
        info.textContent = saved;
    }
}

/**
 * @param {any[]} values
 * @param {string[]} schema
 */
function updateEnergy(values, schema) {
    values.forEach((value) => {
        const energy = fromSchema(value, schema);

        const props = Magnitudes.properties.get(
            /** @type {!number} */(energy.id));
        if (!props) {
            return;
        }

        const saved = /** @type {!string} */(energy.saved);
        magnitudeValueSelector(props.type, props.index_global)
            .forEach((input) => updateEnergyInput(input, saved));
    });
}

// --- the sensor drivers: one module each, the settings of every installed one (espurna::sensor::Driver of sensor.h)

/**
 * @typedef {{key: string, type: string, label: string, def: string, options: string}} DriverField
 */

/**
 * "key|type|label|default|options|repeat" separated by ';', a '#' in key and label is the index (key 0.., label 1..)
 * @param {string} spec
 * @returns {DriverField[]}
 */
export function parseDriverSpec(spec) {
    /** @type {DriverField[]} */
    const out = [];

    for (const field of spec.split(";")) {
        if (!field.length) {
            continue;
        }

        const [key = "", type = "text", label = "", def = "", options = "", repeat = ""] = field.split("|");
        if (!key.length) {
            continue;
        }

        if (!key.includes("#")) {
            out.push({key, type, label, def, options});
            continue;
        }

        const count = parseInt(repeat) || 0;
        for (let index = 0; index < count; ++index) {
            out.push({
                key: key.replace("#", index.toString()),
                type,
                label: label.replace("#", (index + 1).toString()),
                def,
                options,
            });
        }
    }

    return out;
}

/**
 * select options of a field: [value, text]
 * @param {DriverField} field
 * @returns {[string, string][]}
 */
export function driverFieldOptions(field) {
    switch (field.type) {
    case "select":
        return field.options.split(",")
            .filter((option) => option.length)
            .map((option) => {
                const [value = "", text = value] = option.split("=");
                return [value, text];
            });

    case "uart":
        return [["0", "Off"], ["1", "Port 1"], ["2", "Port 2"], ["3", "Port 3"]];

    case "i2c": {
        /** @type {[string, string][]} */
        const out = [["0", "Find it"]];
        for (let address = 0x08; address <= 0x77; ++address) {
            out.push([address.toString(), `0x${address.toString(16).padStart(2, "0")}`]);
        }
        return out;
    }

    }

    return [];
}

// what the GPIO list (gpio.mjs) calls 'none'
const GPIO_NONE = "153";

/**
 * @param {DriverField} field
 * @param {string} value the stored one, empty when not set
 * @returns {HTMLElement}
 */
function driverFieldControl(field, value) {
    const current = value.length ? value : field.def;

    /** @type {HTMLInputElement | HTMLSelectElement} */
    let control;

    switch (field.type) {
    case "gpio": {
        const select = document.createElement("select");
        // the GPIO list of the board comes with gpioConfig, maybe later: the stored value stays the original
        initElementOptions(select, [{value: current || GPIO_NONE, text: `GPIO${current || GPIO_NONE}`}]);
        setSelectValue(select, current || GPIO_NONE);
        select.dataset["enumerable"] = "gpio-hardware";
        control = select;
        break;
    }

    case "select":
    case "uart":
    case "i2c": {
        const select = document.createElement("select");
        initElementOptions(select, driverFieldOptions(field)
            .map(([value, text]) => ({value, text})));
        setSelectValue(select, current);
        control = select;
        break;
    }

    case "bool": {
        const input = document.createElement("input");
        input.type = "checkbox";
        input.classList.add("checkbox-toggle");
        setInputValue(input, current);
        control = input;
        break;
    }

    default: {
        // numbers as text: some are fractions (0.1 Ohm), the number input of the WebUI keeps integers only
        const input = document.createElement("input");
        input.type = "text";
        if (field.type === "number" || field.type === "adc") {
            input.inputMode = "decimal";
        }
        if (field.type === "adc") {
            input.placeholder = "none (ESP8266: 17 = A0)";
        } else if (field.def.length) {
            input.placeholder = field.def;
        }
        if (field.type === "number" && field.options.includes("-")) {
            input.title = `${field.options}`;
        }
        setInputValue(input, value);
        control = input;
        break;
    }

    }

    control.name = field.key;
    control.id = `sns-driver-${field.key}`;
    // the drivers load once, when they start
    control.dataset["action"] = "reboot";

    return control;
}

/**
 * @param {string} id of the container
 * @param {{name: string, spec: string, values: {[k: string]: string}}[]} drivers
 */
function initDrivers(id, drivers) {
    const container = document.getElementById(id);
    if (!container) {
        return;
    }

    // every module sends its own entries (the i2c and the spi bus each), an entry replaces the one of its name only
    const names = new Set(drivers.map((driver) => driver.name));
    for (const node of Array.from(container.children)) {
        if (names.has(/** @type {HTMLElement} */(node).dataset["name"] ?? "")) {
            container.removeChild(node);
        }
    }

    for (const driver of drivers) {
        const fieldset = document.createElement("fieldset");
        fieldset.dataset["name"] = driver.name;

        const legend = document.createElement("legend");
        legend.textContent = (id === "sns-buses") ? `Bus: ${driver.name}` : `Driver: ${driver.name}`;
        fieldset.appendChild(legend);

        const fields = parseDriverSpec(driver.spec);
        if (!fields.length) {
            const span = document.createElement("span");
            span.classList.add("pure-form-message");
            span.textContent = "Nothing to set up.";
            fieldset.appendChild(span);
        }

        const group = document.createElement("div");
        group.classList.add("pure-form-aligned");

        for (const field of fields) {
            const row = document.createElement("div");
            row.classList.add("pure-control-group");

            const control = driverFieldControl(field, driver.values[field.key] ?? "");

            const label = document.createElement("label");
            label.textContent = field.label;
            label.htmlFor = control.id;

            row.appendChild(label);
            row.appendChild(control);
            group.appendChild(row);
        }

        fieldset.appendChild(group);
        container.appendChild(fieldset);

        setOriginalsFromValuesForNode(fieldset);
        fieldset.querySelectorAll("select[data-enumerable]")
            .forEach((elem) => {
                listenEnumerableName(/** @type {HTMLSelectElement} */(elem), "gpio-hardware");
            });
    }

    if (drivers.length && (id === "sns-drivers")) {
        const note = document.createElement("span");
        note.classList.add("pure-form-message");
        note.textContent = "The sensor drivers read these at boot: Save, then reboot.";
        container.appendChild(note);
    }
}

/**
 * @returns {import('./settings.mjs').KeyValueListeners}
 */
function listeners() {
    return {
        "snsDrivers": (_, value) => {
            initDrivers("sns-drivers", value);
        },
        "snsBus": (_, value) => {
            initDrivers("sns-buses", value);
        },
        "magnitudes-pending": () => {
            waitPendingMagnitudes();
        },
        "magnitudes-init": (_, value) => {
            initMagnitudes(
                value.types, value.errors, value.units);
        },
        "magnitudes-module": (_, value) => {
            initModuleMagnitudes(
                value.prefix, value.values, value.schema);
        },
        "magnitudes-list": (_, value) => {
            initMagnitudesList(value.values, value.schema, [
                createMagnitudeUnitSelector, createMagnitudeInfo]);
        },
        "magnitudes-settings": (_, value) => {
            initMagnitudesSettings(value.values, value.schema);
        },
        "magnitudes": (_, value) => {
            updateMagnitudes(value.values, value.schema);
        },
        "energy": (_, value) => {
            updateEnergy(value.values, value.schema);
        },
    };
}

export function init() {
    variableListeners(listeners());

    document.querySelector(".button-emon-expected-calculate")
        ?.addEventListener("click", emonCalculateRatios);
    document.querySelector(".button-emon-expected-apply")
        ?.addEventListener("click", emonApplyRatios);
}
