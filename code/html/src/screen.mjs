import { sendAction } from './connection.mjs';
import { variableListeners } from './settings.mjs';
import { init as initPages } from './pages.mjs';

// The SCREEN panel (the screen module, screen.cpp): the mirror of what the board draws, its keys, the lines. The tile
// pages of the same module are the PAGES panel (src/pages.mjs)

// the icons of the board (screen_icons.h) are from this font: its code point - 0xF0001 + 0xE000 on the board
const MDI_URL = "https://cdn.jsdelivr.net/gh/bruestel/ohpanel@ba9361982399414d1612aea85504d9707b329e75/boards/nspanel/nextion/assets/fonts/materialdesignicons-webfont.ttf";
const MDI_SHIFT = 0xF0001 - 0xE000;

// ScreenFont of screen.h: the height of a line, the pixel size and weight of DejaVu on the board
export const FONTS = [
    {height: 13, size: 10, weight: "normal"},
    {height: 17, size: 14, weight: "normal"},
    {height: 23, size: 19, weight: "normal"},
    {height: 33, size: 28, weight: "bold"},
    {height: 52, size: 44, weight: "bold"},
];

/**
 * @param {number} value RGB565
 * @returns {[number, number, number]}
 */
export function rgb565(value) {
    const v = Number(value) & 0xFFFF;
    const r = (v >> 11) & 0x1F;
    const g = (v >> 5) & 0x3F;
    const b = v & 0x1F;
    return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)];
}

/**
 * the colour on the canvas: RGB565 as it is, a 1-bit panel lit (brighter than half) or not, as oled.cpp does
 * @param {number} value
 * @param {boolean} colour
 * @returns {string}
 */
export function cssColour(value, colour) {
    const [r, g, b] = rgb565(value);
    if (colour) {
        return `rgb(${r},${g},${b})`;
    }
    return ((r * 30 + g * 59 + b * 11) >= 12800) ? "#9fe8ff" : "#10141c";
}

/**
 * one command of the board (screen.cpp, mirror::): a letter and its numbers, a text after '|'
 * @param {string} line
 * @returns {{op: string, args: number[], text: string}}
 */
export function parseCommand(line) {
    const bar = line.indexOf("|");
    const head = (bar >= 0) ? line.substring(0, bar) : line;
    const text = (bar >= 0) ? line.substring(bar + 1) : "";
    const parts = head.trim().split(/\s+/);
    return {op: parts[0] ?? "", args: parts.slice(1).map(Number), text};
}

let __colour = true;
let __on = true;

/** @type {Promise<void> | null} */
let __mdi = null;

function loadIcons() {
    if (!__mdi && (typeof FontFace !== "undefined")) {
        const face = new FontFace("screen-mdi", `url(${MDI_URL})`);
        __mdi = face.load()
            .then((loaded) => {
                document.fonts.add(loaded);
            })
            .catch(() => {});
    }
    return __mdi;
}

/**
 * @param {CanvasRenderingContext2D} ctx
 * @param {{op: string, args: number[], text: string}} command
 */
function draw(ctx, command) {
    const a = command.args;
    switch (command.op) {
    case "Z": {
        const canvas = ctx.canvas;
        canvas.width = a[0] || 128;
        canvas.height = a[1] || 64;
        __colour = a[2] === 1;
        __on = a[3] !== 0;
        canvas.style.width = `${Math.min(640, canvas.width * ((canvas.width <= 128) ? 3 : 2))}px`;
        ctx.fillStyle = "#000";
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        break;
    }
    case "P":
        __on = a[0] !== 0;
        break;
    case "F":
        ctx.fillStyle = cssColour(a[4], __colour);
        ctx.fillRect(a[0], a[1], a[2], a[3]);
        break;
    case "R": {
        const [x, y, w, h, t] = a;
        ctx.fillStyle = cssColour(a[5], __colour);
        ctx.fillRect(x, y, w, t);
        ctx.fillRect(x, y + h - t, w, t);
        ctx.fillRect(x, y, t, h);
        ctx.fillRect(x + w - t, y, t, h);
        break;
    }
    case "L":
        ctx.strokeStyle = cssColour(a[4], __colour);
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(a[0] + 0.5, a[1] + 0.5);
        ctx.lineTo(a[2] + 0.5, a[3] + 0.5);
        ctx.stroke();
        break;
    case "C":
        ctx.beginPath();
        ctx.arc(a[0], a[1], a[2], 0, 2 * Math.PI);
        if (a[4]) {
            ctx.fillStyle = cssColour(a[3], __colour);
            ctx.fill();
        } else {
            ctx.strokeStyle = cssColour(a[3], __colour);
            ctx.stroke();
        }
        break;
    case "T": {
        const [x, y, w, h, font, fore, back, align] = a;
        if (back >= 0) {
            ctx.fillStyle = cssColour(back, __colour);
            ctx.fillRect(x, y, w, h);
        }
        const spec = FONTS[font] ?? FONTS[1];
        ctx.save();
        ctx.beginPath();
        ctx.rect(x, y, w, h);
        ctx.clip();
        ctx.font = `${spec.weight} ${spec.size}px "DejaVu Sans Condensed", "DejaVu Sans", Arial, sans-serif`;
        ctx.fillStyle = cssColour(fore, __colour);
        ctx.textBaseline = "middle";
        ctx.textAlign = (align === 1) ? "center" : (align === 2) ? "right" : "left";
        const tx = (align === 1) ? (x + w / 2) : (align === 2) ? (x + w) : x;
        ctx.fillText(command.text, tx, y + h / 2 + 1);
        ctx.restore();
        break;
    }
    case "I": {
        const [x, y, size, code, fore, back] = a;
        if (back >= 0) {
            ctx.fillStyle = cssColour(back, __colour);
            ctx.fillRect(x, y, size, size);
        }
        ctx.font = `${size}px screen-mdi`;
        ctx.fillStyle = cssColour(fore, __colour);
        ctx.textBaseline = "top";
        ctx.textAlign = "left";
        ctx.fillText(String.fromCodePoint(code + MDI_SHIFT), x, y);
        break;
    }
    }
}

/**
 * the drawing commands of the board, one a line (screen.cpp mirror::)
 * @param {string} _key
 * @param {string} value
 */
async function onMirror(_key, value) {
    const canvas = document.getElementById("screen-mirror");
    if (!(canvas instanceof HTMLCanvasElement) || (typeof value !== "string")) {
        return;
    }

    const ctx = canvas.getContext("2d");
    if (!ctx) {
        return;
    }

    if (value.includes("\nI ") || value.startsWith("I ")) {
        await loadIcons();
    }

    for (const line of value.split("\n")) {
        if (line.length) {
            draw(ctx, parseCommand(line));
        }
    }

    const state = document.getElementById("screen-mirror-state");
    if (state) {
        state.textContent = __on ? "" : "blanked";
    }
}

export function init() {
    variableListeners({
        screenMirror: onMirror,
    });

    document.querySelectorAll(".button-screen-key")
        .forEach((elem) => {
            elem.addEventListener("click", () => {
                const key = (elem instanceof HTMLElement) ? elem.dataset["key"] : "";
                if (key) {
                    sendAction("screenKey", {key});
                }
            });
        });

    // the tile pages are the same module's
    initPages();
}
