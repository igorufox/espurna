import { expect, test } from 'vitest';

import { qrEncode } from '../src/qrcode.mjs';

// An independent reader after ISO/IEC 18004 (versions 1 to 6, byte mode, level M): the format bits, the data read
// back in the zigzag order without the mask, the Reed-Solomon syndromes of every block

/** version: [EC codewords per block, blocks, data codewords per block], level M */
const ECC_M = {1: [10, 1, 16], 2: [16, 1, 28], 3: [26, 1, 44], 4: [18, 2, 32], 5: [24, 2, 43], 6: [16, 4, 27]};

const EXP = new Array(512);
const LOG = new Array(256);
{
    let value = 1;
    for (let i = 0; i < 255; ++i) {
        EXP[i] = value;
        LOG[value] = i;
        value <<= 1;
        if (value & 0x100) {
            value ^= 0x11d;
        }
    }
    for (let i = 255; i < 512; ++i) {
        EXP[i] = EXP[i - 255];
    }
}

/**
 * @param {number} a
 * @param {number} b
 */
function mul(a, b) {
    return (a && b) ? EXP[LOG[a] + LOG[b]] : 0;
}

/**
 * @param {number} mask
 * @param {number} x column
 * @param {number} y row
 */
function maskBit(mask, x, y) {
    return [
        (x + y) % 2 === 0,
        y % 2 === 0,
        x % 3 === 0,
        (x + y) % 3 === 0,
        (Math.floor(y / 2) + Math.floor(x / 3)) % 2 === 0,
        ((x * y) % 2) + ((x * y) % 3) === 0,
        (((x * y) % 2) + ((x * y) % 3)) % 2 === 0,
        (((x + y) % 2) + ((x * y) % 3)) % 2 === 0,
    ][mask];
}

/** @param {boolean[][]} m */
function read(m) {
    const size = m.length;
    const version = (size - 17) / 4;
    const at = (/** @type {number} */x, /** @type {number} */y) => (m[y][x] ? 1 : 0);

    // the finder patterns in three corners
    for (const [cx, cy] of [[3, 3], [size - 4, 3], [3, size - 4]]) {
        for (let dy = -3; dy <= 3; ++dy) {
            for (let dx = -3; dx <= 3; ++dx) {
                const ring = Math.max(Math.abs(dx), Math.abs(dy));
                expect(at(cx + dx, cy + dy)).toBe((ring === 2) ? 0 : 1);
            }
        }
    }

    // the two copies of the format bits, BCH (15,5), level M
    let first = 0;
    let second = 0;
    const one = [];
    for (let i = 0; i <= 5; ++i) one.push(at(8, i));
    one.push(at(8, 7), at(8, 8), at(7, 8));
    for (let i = 9; i < 15; ++i) one.push(at(14 - i, 8));
    const two = [];
    for (let i = 0; i < 8; ++i) two.push(at(size - 1 - i, 8));
    for (let i = 8; i < 15; ++i) two.push(at(8, size - 15 + i));
    one.forEach((bit, i) => { first |= bit << i; });
    two.forEach((bit, i) => { second |= bit << i; });
    expect(first).toBe(second);
    expect(at(8, size - 8)).toBe(1);

    const format = first ^ 0x5412;
    let rem = (format >> 10) << 10;
    for (let i = 14; i >= 10; --i) {
        if (rem & (1 << i)) {
            rem ^= 0x537 << (i - 10);
        }
    }
    expect(rem).toBe(format & 0x3ff);
    expect(format >> 13).toBe(0); // M
    const mask = (format >> 10) & 7;

    // the timing patterns
    for (let i = 8; i < size - 8; ++i) {
        expect(at(i, 6)).toBe((i % 2) ? 0 : 1);
        expect(at(6, i)).toBe((i % 2) ? 0 : 1);
    }

    const align = size - 7;
    const isFunction = (/** @type {number} */x, /** @type {number} */y) =>
        ((x < 9) && (y < 9)) || ((x < 9) && (y >= size - 8)) || ((x >= size - 8) && (y < 9))
        || (x === 6) || (y === 6)
        || ((version >= 2) && (Math.abs(x - align) <= 2) && (Math.abs(y - align) <= 2));

    /** @type {number[]} */
    const bits = [];
    for (let right = size - 1; right >= 1; right -= 2) {
        if (right === 6) {
            right = 5;
        }
        for (let vert = 0; vert < size; ++vert) {
            for (let j = 0; j < 2; ++j) {
                const x = right - j;
                const y = (((right + 1) & 2) === 0) ? (size - 1 - vert) : vert;
                if (!isFunction(x, y)) {
                    bits.push(at(x, y) ^ (maskBit(mask, x, y) ? 1 : 0));
                }
            }
        }
    }

    const [ec, blocks, data] = ECC_M[/** @type {keyof typeof ECC_M} */(version)];
    const total = blocks * (data + ec);
    const codewords = [];
    for (let i = 0; i < total; ++i) {
        codewords.push(bits.slice(i * 8, (i * 8) + 8).reduce((out, bit) => (out << 1) | bit, 0));
    }

    // de-interleave: every block's data, then every block's EC; each block's syndromes are 0
    /** @type {number[]} */
    const stream = [];
    for (let b = 0; b < blocks; ++b) {
        const block = [];
        for (let i = 0; i < data; ++i) block.push(codewords[(i * blocks) + b]);
        for (let i = 0; i < ec; ++i) block.push(codewords[(blocks * data) + (i * blocks) + b]);
        for (let root = 0; root < ec; ++root) {
            expect(block.reduce((s, byte) => mul(s, EXP[root]) ^ byte, 0)).toBe(0);
        }
        stream.push(...block.slice(0, data));
    }

    const dataBits = stream.flatMap((byte) => [7, 6, 5, 4, 3, 2, 1, 0].map((i) => (byte >> i) & 1));
    const take = (/** @type {number} */from, /** @type {number} */count) =>
        dataBits.slice(from, from + count).reduce((out, bit) => (out << 1) | bit, 0);
    expect(take(0, 4)).toBe(0b0100); // byte mode
    const length = take(4, 8);
    const out = [];
    for (let i = 0; i < length; ++i) {
        out.push(take(12 + (i * 8), 8));
    }

    return {version, text: new TextDecoder().decode(new Uint8Array(out))};
}

test('the Matter setup payload reads back', () => {
    const text = 'MT:Y.K9042C00KA0648G00';
    expect(read(qrEncode(text))).toEqual({version: 2, text});
});

test('every version, from 1 to 6, reads back at its edges', () => {
    const cases = [[1, 1], [14, 1], [15, 2], [26, 2], [27, 3], [42, 3], [43, 4], [62, 4], [63, 5], [84, 5], [85, 6], [106, 6]];
    for (const [length, version] of cases) {
        const text = Array.from({length}, (_, i) => String.fromCharCode(33 + ((i * 7) % 90))).join('');
        const matrix = qrEncode(text);
        expect(matrix.length).toBe(17 + (4 * version));
        expect(read(matrix)).toEqual({version, text});
    }
});

test('text over version 6 is refused', () => {
    expect(() => qrEncode('x'.repeat(107))).toThrow('too long');
});
