import { afterEach, describe, expect, test, vi } from 'vitest';
import { createHash } from 'node:crypto';

import { fetchBytes, safeHref, sha256Hex, waitFor } from '../src/net.mjs';
import { waitModules } from '../src/install.mjs';

describe('safeHref', () => {
    test('http and https only', () => {
        expect(safeHref('https://example.com/a')).toBe('https://example.com/a');
        expect(safeHref('http://example.com/a')).toBe('http://example.com/a');
        expect(safeHref('javascript:alert(1)')).toBe('');
        expect(safeHref('JavaScript:alert(1)')).toBe('');
        expect(safeHref('data:text/html,x')).toBe('');
        expect(safeHref('')).toBe('');
        expect(safeHref(undefined)).toBe('');
    });

    test('relative to a base', () => {
        expect(safeHref('/espurna.bin', 'https://h/r/list.json')).toBe('https://h/espurna.bin');
        expect(safeHref('espurna.bin', 'https://h/r/list.json')).toBe('https://h/r/espurna.bin');
        expect(safeHref('javascript:alert(1)', 'https://h/')).toBe('');
        expect(safeHref('no url')).toBe('');
    });
});

describe('sha256Hex', () => {
    test('matches node:crypto around the block borders', () => {
        for (const size of [0, 1, 55, 56, 63, 64, 65, 119, 120, 128, 4097]) {
            const data = new Uint8Array(size);
            for (let index = 0; index < size; ++index) {
                data[index] = (index * 31 + 7) & 0xff;
            }
            expect(sha256Hex(data)).toBe(createHash('sha256').update(data).digest('hex'));
        }
    });
});

describe('installFileOf', async () => {
    const { installFileOf } = await import('../src/fwinstall.mjs');
    const manifest = {releases: [
        {commit: 'abcdef0123', images: {esp8266: [
            {name: 'espurna-esp8266.bin', kind: 'serial', url: 'espurna-esp8266.bin'},
            {name: 'espurna-esp8266.ota', kind: 'install', url: 'espurna-esp8266.ota', sha256: 'aa'},
        ]}},
        {commit: '1111111222', images: {esp8266: [{name: 'x.ota', kind: 'install', url: 'javascript:alert(1)'}]}},
    ]};

    test('the install image of the commit, relative to the list', () => {
        expect(installFileOf(manifest, {image: 'esp8266', commit: 'abcdef0', base: 'https://h/r/firmware.json'}))
            .toEqual({name: 'espurna-esp8266.ota', kind: 'install', url: 'https://h/r/espurna-esp8266.ota', sha256: 'aa'});
    });

    test('no link that is not http(s), no other kind of file', () => {
        expect(installFileOf(manifest, {image: 'esp8266', commit: '1111111', base: 'https://h/'})).toBe(null);
        const serialOnly = {releases: [{commit: 'a', images: {esp8266: [{name: 'b', kind: 'serial', url: 'b'}]}}]};
        expect(installFileOf(serialOnly, {image: 'esp8266', commit: '', base: 'https://h/'})).toBe(null);
    });

    test('not a list', () => {
        expect(() => installFileOf(/** @type {any} */({}), {image: 'esp8266', commit: '', base: 'https://h/'})).toThrow();
    });
});

describe('fetchBytes', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
    });

    const data = new Uint8Array([1, 2, 3, 4, 5]);
    const sha256 = createHash('sha256').update(data).digest('hex');

    /** @param {number} status */
    function serve(status = 200) {
        vi.stubGlobal('fetch', async () => new Response(data, {status}));
    }

    test('the file of the list, size and SHA-256 checked', async () => {
        serve();
        const steps = [];
        const buffer = await fetchBytes('https://h/a.ota', {name: 'a.ota', size: 5, sha256: sha256.toUpperCase()},
            (text) => steps.push(text));
        expect([...new Uint8Array(buffer)]).toEqual([...data]);
        expect(steps).toEqual(['Downloading a.ota...', 'Checking a.ota...']);
    });

    test('another file never comes back', async () => {
        serve();
        await expect(fetchBytes('https://h/a.ota', {name: 'a.ota', sha256: 'ab'.repeat(32)})).rejects.toThrow('SHA-256');
        await expect(fetchBytes('https://h/a.ota', {name: 'a.ota', size: 6, sha256})).rejects.toThrow('5 bytes instead of 6');
    });

    test('no SHA-256, no link, an HTTP error', async () => {
        serve(404);
        await expect(fetchBytes('https://h/a.ota', {name: 'a.ota'})).rejects.toThrow('no SHA-256');
        await expect(fetchBytes('', {name: 'a.ota', sha256})).rejects.toThrow('no http(s) link');
        await expect(fetchBytes('https://h/a.ota', {name: 'a.ota', sha256})).rejects.toThrow('download of a.ota: HTTP 404');
    });
});

describe('waitFor: the waits of every install', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.unstubAllGlobals();
    });

    test('asks after 4 s, then every 2 s, until the answer has the status', async () => {
        vi.useFakeTimers();
        /** @type {number[]} */
        const asked = [];
        const answers = [() => { throw new Error('down'); }, () => new Response('', {status: 503}),
            () => new Response('{"modules":[]}', {status: 200})];
        vi.stubGlobal('fetch', async () => {
            asked.push(Date.now());
            return answers[asked.length - 1]();
        });

        const start = Date.now();
        const done = waitModules(new URL('http://board/'));
        await vi.runAllTimersAsync();
        expect(await done).toEqual({modules: []});
        expect(asked.map((at) => at - start)).toEqual([4000, 6000, 8000]);
    });

    test('the ESP32 safeboot answers 404, that is the wait for it', async () => {
        vi.useFakeTimers();
        vi.stubGlobal('fetch', async () => new Response('', {status: 404}));
        const done = waitModules(new URL('http://board/'), 404);
        await vi.runAllTimersAsync();
        expect(await done).toBe(null);
    });

    test('a board that does not come back stops the install', async () => {
        vi.useFakeTimers();
        vi.stubGlobal('fetch', async () => { throw new Error('down'); });
        const done = waitFor(new URL('http://board/modules'), 200, 10000);
        const outcome = expect(done).rejects.toThrow('did not come back in 10 s');
        await vi.runAllTimersAsync();
        await outcome;
    });
});

describe('installOutcome', async () => {
    const { installOutcome } = await import('../src/fwinstall.mjs');

    test('the version names the commit the install asked for', () => {
        expect(installOutcome('1.16.0-dev-gitabcdef0', {commit: 'abcdef0'})).toBe('installed');
        expect(installOutcome('1.16.0-dev-git1234567', {commit: 'abcdef0'})).toBe('other');
        expect(installOutcome('1.16.0-dev', {commit: ''})).toBe(null);
    });
});
