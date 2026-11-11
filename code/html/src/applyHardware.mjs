import { send, sendAction } from './connection.mjs';

// One way to put a board description on the device: the device templates (templates.mjs) and the Tasmota import
// (tasmota.mjs). The relay, button and LED keys of the old board go away (an old relay #3 must not stay on a
// board with two), values longer than MaxValue are left out

export const PIN_KEYS = new Set([
    'relayGpio', 'relayResetGpio', 'btnGpio', 'ledGpio', 'ltDimmerGPIO', 'ltEnableGPIO',
    'uartTx', 'uartRx', 'rfbRX', 'rfbTX', 'irRx', 'irTx', 'i2cSDA', 'i2cSCL',
    // sensor.cpp load(), hlw8012CF also stands for hlw8012CF1
    'hlw8012SEL', 'hlw8012CF', 'ech1560CLK', 'ech1560MISO', 'dhtPin', 'dallasPin', 'digitalPin',
    'eventsPin', 'pulsemeterPin', 'geigerPin', 'analogPin', 'ntcPin', 'ldrPin',
]);

/**
 * Settings describing the board: pins and their modes, button actions, drivers and their
 * calibration, and the board names. Indexed keys are listed without the trailing index
 * (ntcR is ntcR0). Network, passwords, MQTT, topics, sync and the rest are never exported
 * and never applied from a downloaded list
 */
export const HARDWARE_KEYS = new Set([
    ...PIN_KEYS,
    'relayGpioType', 'relayType', 'relayProv', 'relayDummy',
    'btnGpioType', 'btnProv', 'btnMode', 'btnPinMode', 'btnDefVal', 'btnRelay', 'btnLevel',
    'btnPress', 'btnRlse', 'btnClick', 'btnDclk', 'btnTclk', 'btnLclk', 'btnLLclk',
    'btnLclkDel', 'btnLLclkDel', 'btnDebDel', 'btnRepDel',
    'btnFreq', 'btnMinPulses', 'btnMinDuty', 'btnAcConfirm', 'btnAcFreq',
    'ledInv', 'ledMode', 'ledRelay',
    'ltInv',
    'uartBaud', 'uartDataBits', 'uartParity', 'uartStopBits', 'uartInv',
    'rfbProv',
    'irRxPullup', 'irTxInv', 'irRxPreset',
    // sensor.cpp load(): pin options, ADC parameters, I2C addresses, UART port numbers
    'hlw8012SelCurr', 'hlw8012IntMode', 'ech1560Inv', 'dhtType', 'digitalPinMode', 'digitalDefState',
    'eventsPullUp', 'eventsIntMode', 'eventsDebounce', 'pulsemeterIntMode', 'pulsemeterDebounce',
    'ntcBeta', 'ntcRUp', 'ntcRDown', 'ntcR', 'ldrType', 'ldrOnGround', 'ldrResistor',
    'ade7953Addr', 'bmx280Number', 'bmx280Address',
    'cse7766Port', 'v9261fPort', 'pzemPort', 'pzemv30Port', 'mhz19Port', 'pmsPort', 'pm1006Port',
    'sds011Port', 'senseairPort', 'sm300d2Port', 't6613Port', 'ezophPort', 'a02yyuPort',
    // energy meter calibration of the board
    'currRatio', 'voltRatio', 'pwrPRatio',
    'sysManufacturer', 'sysDevice',
]);

// values of a board description are pins, modes and short names
const MAX_VALUE = 64;

// how many relays / buttons / LEDs an old board may have left behind
const MAX_INDEX = 16;

const RESET_PREFIXES = ['relay', 'btn', 'led'];

/**
 * @typedef {{[key: string]: string}} Settings
 */

/**
 * The settings message: set what the description says, delete the relay / button / LED keys it does not
 * @param {Settings} settings
 * @returns {{set: Settings, del: string[]}}
 */
export function hardwareMessage(settings) {
    /** @type {Settings} */
    const set = {};
    for (const [key, value] of Object.entries(settings ?? {})) {
        const text = String(value);
        if (text.length <= MAX_VALUE) {
            set[key] = text;
        }
    }

    /** @type {string[]} */
    const del = [];
    for (const key of HARDWARE_KEYS) {
        if (!RESET_PREFIXES.some((prefix) => key.startsWith(prefix))) {
            continue;
        }
        for (let index = 0; index < MAX_INDEX; ++index) {
            const name = `${key}${index}`;
            if (!(name in set)) {
                del.push(name);
            }
        }
    }

    return {set, del};
}

/**
 * Asks, sends, offers the reboot
 * @param {Settings} settings
 * @param {string} question
 */
export function applyHardware(settings, question) {
    if (!window.confirm(`${question}\n\nNote: existing relay/button/LED settings will be replaced.`)) {
        return false;
    }

    send(JSON.stringify({settings: hardwareMessage(settings)}));

    setTimeout(() => {
        if (window.confirm('Settings have been sent to the device!\n\nReboot now to apply?')) {
            sendAction('reboot');
        }
    }, 500);

    return true;
}
