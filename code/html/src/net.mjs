// Network helpers shared by the firmware install, the modules panel and the device templates

/**
 * @param {number} ms
 * @returns {Promise<void>}
 */
export function wait(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

/**
 * A link out of data that came from the network (a release list, a template list): only http and https, everything
 * else (javascript:, data:, a relative path that is not one) gives an empty string, the link is not shown then
 * @param {string | undefined | null} value
 * @param {string | URL} [base]
 * @returns {string}
 */
export function safeHref(value, base) {
    if (!value) {
        return '';
    }

    try {
        const url = (base !== undefined)
            ? new URL(String(value), base)
            : new URL(String(value));
        if ((url.protocol === 'http:') || (url.protocol === 'https:')) {
            return url.href;
        }
    } catch {
        // not an URL
    }

    return '';
}

/**
 * A GET that only goes on with a 2xx answer, never from the browser cache
 * @param {string | URL} url
 * @param {RequestInit} [options]
 * @param {string} [what] names the request in the error
 * @returns {Promise<Response>}
 */
export async function fetchOk(url, options = {}, what = '') {
    const response = await fetch(url, {cache: 'no-store', ...options});
    if (!response.ok) {
        throw new Error(`${what ? `${what}: ` : ''}HTTP ${response.status}`);
    }

    return response;
}

/**
 * @param {string | URL} url
 * @param {RequestInit} [options]
 * @param {string} [what] names the request in the error
 * @returns {Promise<any>}
 */
export async function fetchJson(url, options = {}, what = '') {
    return (await fetchOk(url, options, what)).json();
}

/**
 * A file of the release list: its size and its SHA-256 as the list names them, a different file never reaches the
 * board (B4). Every download of firmware goes through here
 * @param {string} url absolute http(s), safeHref() of the list entry
 * @param {{name: string, size?: number, sha256?: string}} file
 * @param {(text: string) => void} [step]
 * @returns {Promise<ArrayBuffer>}
 */
export async function fetchBytes(url, {name, size, sha256}, step = () => {}) {
    if (!url) {
        throw new Error(`${name}: the release list has no http(s) link to it`);
    }
    if (!sha256) {
        throw new Error(`${name}: the release list names no SHA-256 of it`);
    }

    step(`Downloading ${name}...`);
    const buffer = await (await fetchOk(url, {}, `download of ${name}`)).arrayBuffer();
    if (size && (buffer.byteLength !== size)) {
        throw new Error(`download of ${name}: ${buffer.byteLength} bytes instead of ${size}`);
    }

    step(`Checking ${name}...`);
    const actual = sha256Hex(new Uint8Array(buffer));
    if (actual !== sha256.toLowerCase()) {
        throw new Error(`download of ${name}: SHA-256 ${actual} instead of ${sha256}`);
    }

    return buffer;
}

/**
 * A form POST, the answer text; an error with that text when the board refuses
 * @param {string | URL} url
 * @param {FormData | URLSearchParams} body
 * @param {number} [timeout] ms, 0 - none: a board that does not answer is an error then
 * @returns {Promise<string>}
 */
export async function post(url, body, timeout = 0) {
    /** @type {RequestInit} */
    const options = {method: 'POST', body, credentials: 'same-origin'};
    if (timeout) {
        options.signal = AbortSignal.timeout(timeout);
    }
    let response;
    try {
        response = await fetch(url, options);
    } catch (error) {
        if (error instanceof Error && (error.name === 'TimeoutError')) {
            throw new Error(`${new URL(url, 'http://x/').pathname}: no answer in ${Math.round(timeout / 1000)} s`, { cause: error });
        }
        throw error;
    }
    const text = await response.text();
    if (!response.ok) {
        throw new Error(text || `${response.status}`);
    }

    return text;
}

/**
 * Waits for a restarting board: asks `url` until it answers with `status` (first after 4 s, then every 2 s)
 * @param {URL} url
 * @param {number} status
 * @param {number} timeout ms
 * @returns {Promise<Response>} that answer
 */
export async function waitFor(url, status = 200, timeout = 90000) {
    const start = Date.now();
    await wait(4000);
    while ((Date.now() - start) < timeout) {
        try {
            const response = await fetch(url, {cache: 'no-store', credentials: 'same-origin'});
            if (response.status === status) {
                return response;
            }
        } catch {
            // restarting
        }
        await wait(2000);
    }

    throw new Error(`the board did not come back in ${Math.round(timeout / 1000)} s`);
}

/**
 * The board refuses a firmware update while module code runs from the area the update is written into (409
 * "MODULES.OFF first", ESP8266): restart it without the modules (POST modules/off, they stay installed but off) and
 * wait until it answers again
 * @param {URL} upgrade the /upgrade URL of the board
 * @param {number} timeout ms
 */
export async function modulesOffForUpdate(upgrade, timeout = 90000) {
    await post(new URL('modules/off', upgrade), new FormData());
    await waitFor(new URL('modules', upgrade), 200, timeout);
}

/**
 * @param {number} status
 * @param {string} text
 * @returns {boolean}
 */
export function modulesBlockUpdate(status, text) {
    return (status === 409) && text.includes("MODULES.OFF");
}

const SHA256_K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

/**
 * SHA-256 in plain JS: the WebUI of a board is plain http, no crypto.subtle there
 * @param {Uint8Array} data
 * @returns {string} hex
 */
export function sha256Hex(data) {
    const h = new Uint32Array([
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    const w = new Uint32Array(64);
    const length = data.length;
    const blocks = Math.ceil((length + 9) / 64);
    const last = new Uint8Array(128);
    const tail = length - (length % 64);
    last.set(data.subarray(tail));
    last[length - tail] = 0x80;
    const lastView = new DataView(last.buffer);
    const bits = length * 8;
    const end = (blocks * 64) - tail;
    lastView.setUint32(end - 8, Math.floor(bits / 0x100000000));
    lastView.setUint32(end - 4, bits >>> 0);
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);

    for (let block = 0; block < blocks; ++block) {
        const at = block * 64;
        for (let i = 0; i < 16; ++i) {
            w[i] = (at < tail)
                ? view.getUint32(at + (i * 4))
                : lastView.getUint32((at - tail) + (i * 4));
        }
        for (let i = 16; i < 64; ++i) {
            const a = w[i - 15];
            const b = w[i - 2];
            const s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
            const s1 = ((b >>> 17) | (b << 15)) ^ ((b >>> 19) | (b << 13)) ^ (b >>> 10);
            w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
        }

        let [a, b, c, d, e, f, g, hh] = h;
        for (let i = 0; i < 64; ++i) {
            const s1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
            const ch = (e & f) ^ (~e & g);
            const t1 = (hh + s1 + ch + SHA256_K[i] + w[i]) | 0;
            const s0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
            const maj = (a & b) ^ (a & c) ^ (b & c);
            const t2 = (s0 + maj) | 0;
            hh = g; g = f; f = e; e = (d + t1) | 0;
            d = c; c = b; b = a; a = (t1 + t2) | 0;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    return Array.from(h, (value) => value.toString(16).padStart(8, '0')).join('');
}
