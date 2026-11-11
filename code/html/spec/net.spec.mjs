import { describe, expect, test } from 'vitest';
import { createHash } from 'node:crypto';

import { safeHref, sha256Hex } from '../src/net.mjs';

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

describe('modulesFileUrl', async () => {
    const { modulesFileUrl } = await import('../src/fwinstall.mjs');
    const manifest = {releases: [
        {commit: 'abcdef0123', images: {esp8266: [
            {kind: 'full', url: 'full.bin'},
            {kind: 'modules', url: 'modules.bin'},
        ]}},
        {commit: '1111111222', images: {esp8266: [{kind: 'full', url: 'javascript:alert(1)'}]}},
    ]};

    test('the modules file of the commit, relative to the list', () => {
        expect(modulesFileUrl(manifest, {image: 'esp8266', commit: 'abcdef0', base: 'https://h/r/firmware.json'}))
            .toBe('https://h/r/modules.bin');
    });

    test('no link that is not http(s)', () => {
        expect(modulesFileUrl(manifest, {image: 'esp8266', commit: '1111111', base: 'https://h/'})).toBe(null);
    });

    test('not a list', () => {
        expect(() => modulesFileUrl({}, {image: 'esp8266', commit: '', base: 'https://h/'})).toThrow();
    });
});

describe('hardwareMessage', async () => {
    const { hardwareMessage } = await import('../src/applyHardware.mjs');

    test('old relay, button and LED keys go away, the new ones stay', () => {
        const {set, del} = hardwareMessage({relayGpio0: '12', btnGpio0: '0', ledGpio0: '13'});
        expect(set).toEqual({relayGpio0: '12', btnGpio0: '0', ledGpio0: '13'});
        expect(del).toContain('relayGpio1');
        expect(del).toContain('btnGpio3');
        expect(del).toContain('ledInv0');
        expect(del).not.toContain('relayGpio0');
        expect(del).not.toContain('btnGpio0');
    });

    test('long values are left out', () => {
        const {set} = hardwareMessage({sysDevice: 'x'.repeat(65), sysManufacturer: 'ITEAD'});
        expect(set).toEqual({sysManufacturer: 'ITEAD'});
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

describe('modulesFileUrl takes the install image first', async () => {
    const { modulesFileUrl } = await import('../src/fwinstall.mjs');
    test('install over modules over full', () => {
        const manifest = {releases: [{commit: 'abcdef0', images: {esp32: [
            {kind: 'full', url: 'full.bin'}, {kind: 'install', url: 'espurna-esp32.ota'}, {kind: 'modules', url: 'm.bin'},
        ]}}]};
        expect(modulesFileUrl(manifest, {image: 'esp32', commit: 'abcdef0', base: 'https://h/'})).toBe('https://h/espurna-esp32.ota');
    });
});
