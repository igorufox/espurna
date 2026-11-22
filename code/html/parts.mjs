// The parts of the WebUI the modules bring (U31, owner 2026-11-12: «разделить webui на модули»): every panel, status
// block, template, style and script of a module is in its part, not in index.html. The firmware module of the same
// name carries the part in its flash (gulp build: html/build/ui-<name>.html.gz, espurna/static/ui_<name>.ipp, the
// module adds it with modulesUi()); the page of the webui module asks the board for its parts (GET ui) and puts them in
// before it connects (src/parts.mjs).
//
//   status    - src/status-*.html, into the STATUS panel (above the head's own lines)
//   panels    - src/panel-*.html, into the content (the menu entries stay in src/menu-list.html, hidden until the
//               device names them: modulesVisible)
//   templates - src/template-*.html, into the body
//   css       - src/*.css
//   script    - the module's .mjs: its init() runs once the part is in, before the WebSocket connects. Imports of the
//               head's files (CORE below) are the page's own (window.EspurnaUi), everything else is bundled into the part
//
// The order of the parts is the order the modules start in (the board's list).

/**
 * @typedef {{status?: string[], panels?: string[], templates?: string[], css?: string[], script?: string}} Part
 */

/** @type {{[name: string]: Part}} */
export const PARTS = {
    api: {
        panels: ['src/panel-api.html'],
        script: 'src/api.mjs',
    },
    button: {
        script: 'src/button.mjs',
    },
    curtain_kingart: {
        status: ['src/status-curtain.html'],
        panels: ['src/panel-curtain.html'],
        templates: ['src/template-curtain.html'],
        css: ['src/curtain.css'],
        script: 'src/curtain.mjs',
    },
    domoticz: {
        panels: ['src/panel-dcz.html'],
        script: 'src/domoticz.mjs',
    },
    garland: {
        panels: ['src/panel-garland.html'],
        script: 'src/garland.mjs',
    },
    homeassistant: {
        panels: ['src/panel-ha.html'],
        script: 'src/ha.mjs',
    },
    influxdb: {
        panels: ['src/panel-idb.html'],
    },
    led: {
        panels: ['src/panel-led.html'],
        templates: ['src/template-led.html'],
        script: 'src/led.mjs',
    },
    light: {
        status: ['src/status-light.html'],
        panels: ['src/panel-light.html'],
        templates: ['src/template-light.html'],
        css: ['src/light.css'],
        script: 'src/light.mjs',
    },
    lightfox: {
        panels: ['src/panel-lightfox.html'],
        script: 'src/lightfox.mjs',
    },
    matter: {
        panels: ['src/panel-matter.html'],
        script: 'src/matter.mjs',
    },
    mqtt: {
        panels: ['src/panel-mqtt.html'],
    },
    nspanel: {
        panels: ['src/panel-nspanel.html'],
        script: 'src/nspanel.mjs',
    },
    ntp: {
        panels: ['src/panel-ntp.html'],
        script: 'src/ntp.mjs',
    },
    relay: {
        status: ['src/status-relay.html'],
        panels: ['src/panel-relay.html'],
        templates: ['src/template-relay.html'],
        css: ['src/relay.css'],
        script: 'src/relay.mjs',
    },
    rf: {
        panels: ['src/panel-rfb.html'],
        templates: ['src/template-rfb.html'],
        css: ['src/rfbridge.css'],
        script: 'src/rfbridge.mjs',
    },
    rfm69: {
        status: ['src/status-rfm69.html'],
        panels: ['src/panel-rfm69-mapping.html', 'src/panel-rfm69-messages.html'],
        templates: ['src/template-rfm69.html'],
        css: ['src/rfm69.css'],
        script: 'src/rfm69.mjs',
    },
    oled: {
        panels: ['src/panel-oled.html'],
        css: ['src/oled.css'],
        script: 'src/oled.mjs',
    },
    rpnrules: {
        panels: ['src/panel-rpn.html'],
        templates: ['src/template-rpn.html'],
        script: 'src/rules.mjs',
    },
    scheduler: {
        panels: ['src/panel-sch.html'],
        templates: ['src/template-sch.html'],
        script: 'src/schedule.mjs',
    },
    sensors: {
        status: ['src/status-sns.html'],
        panels: ['src/panel-sns.html'],
        templates: ['src/template-sns.html'],
        css: ['src/sensor.css'],
        script: 'src/sensor.mjs',
    },
    thermostat: {
        panels: ['src/panel-thermostat.html'],
        css: ['src/thermostat.css'],
        script: 'src/thermostat.mjs',
    },
    thingspeak: {
        panels: ['src/panel-tspk.html'],
        script: 'src/thingspeak.mjs',
    },
    uart: {
        panels: ['src/panel-uart.html'],
        script: 'src/uart.mjs',
    },
};

// the head's files of the page (index.html, src/index.mjs): a part imports them from the page, never bundles its own
// copy (one state: the settings listeners, the enumerables, the connection, the templates)
export const CORE = [
    'src/connection.mjs',
    'src/core.mjs',
    'src/errors.mjs',
    'src/notify.mjs',
    'src/password.mjs',
    'src/password/utils.mjs',
    'src/question.mjs',
    'src/settings.mjs',
    'src/settings/change.mjs',
    'src/settings/dataset.mjs',
    'src/settings/enumerable.mjs',
    'src/settings/group.elem.mjs',
    'src/settings/group.mjs',
    'src/settings/input.mjs',
    'src/settings/name.mjs',
    'src/settings/select.mjs',
    'src/settings/span.mjs',
    'src/settings/template.mjs',
    'src/settings/utils.mjs',
    'src/settings/value.mjs',
    'src/template.mjs',
    'src/validate.mjs',
    'src/validate/utils.mjs',
];

/**
 * the property of window.EspurnaUi a head's file is: src/settings/group.elem.mjs -> settings_group_elem
 * @param {string} file
 */
export function coreName(file) {
    return file.replace(/^src\//, '').replace(/\.mjs$/, '').replace(/[^a-zA-Z0-9]/g, '_');
}
