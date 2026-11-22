import { variableListeners } from './settings.mjs';

/**
 * @typedef {{width: number, height: number, on: boolean, pixels: string}} OledMirror
 */

/**
 * The pixels of the screen as the board has them (oled.cpp mirror()): hex of the buffer, a byte is 8 rows of one
 * column, the pages of 8 rows one after the other
 * @param {string} _key
 * @param {OledMirror} value
 */
function onMirror(_key, value) {
    const canvas = document.getElementById("oled-mirror");
    if (!(canvas instanceof HTMLCanvasElement)) {
        return;
    }

    const state = document.getElementById("oled-mirror-state");
    if (state) {
        state.textContent = value.on ? "" : "blanked";
    }

    canvas.width = value.width;
    canvas.height = value.height;

    const ctx = canvas.getContext("2d");
    if (!ctx) {
        return;
    }

    const image = ctx.createImageData(value.width, value.height);
    for (let y = 0; y < value.height; ++y) {
        for (let x = 0; x < value.width; ++x) {
            const index = x + Math.floor(y / 8) * value.width;
            const byte = parseInt(value.pixels.substring(index * 2, index * 2 + 2), 16);
            const lit = ((byte >> (y & 7)) & 1) && value.on;
            const offset = (x + y * value.width) * 4;
            image.data[offset] = lit ? 0x9f : 0x10;
            image.data[offset + 1] = lit ? 0xe8 : 0x14;
            image.data[offset + 2] = lit ? 0xff : 0x1c;
            image.data[offset + 3] = 0xff;
        }
    }

    ctx.putImageData(image, 0, 0);
}

export function init() {
    variableListeners({
        oledMirror: onMirror,
    });
}
