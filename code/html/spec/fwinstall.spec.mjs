import { expect, test } from 'vitest';
import {
    firmwareHeader,
    firmwareRows,
    imageCandidates,
    installable,
    installOutcome,
    installFileOf,
    roundedSize,
    uploadOutcome,
} from '../src/fwinstall.mjs';

/**
 * @param {string} text
 * @returns {Uint8Array}
 */
function bytes(text) {
    return new Uint8Array([...text].map((x) => x.charCodeAt(0)));
}

test('image headers of every chip are recognised', () => {
    expect(firmwareHeader(new Uint8Array([0xe9, 0x03, 0x03, 0x20]))).toBe('esp');
    expect(firmwareHeader(new Uint8Array([0x1f, 0x8b, 0x08, 0x00]))).toBe('gzip');
    expect(firmwareHeader(bytes('UF2\nWQ]\x9e'))).toBe('uf2');
    // the container of both Beken bootloaders and the Matter code of the Tuya layout are gone: two .uf2 instead
    expect(firmwareHeader(bytes('ESPBEKEN'))).toBe(null);
    expect(firmwareHeader(bytes('ESPMTAIL'))).toBe(null);
    // the install image is not a firmware image: install.mjs takes it
    expect(firmwareHeader(bytes('EOT1\x02\x00\x00\x00'))).toBe(null);
    expect(firmwareHeader(bytes('<!DOCTYP'))).toBe(null);
    expect(firmwareHeader(new Uint8Array([]))).toBe(null);
});

test('OTA size is counted in whole sectors', () => {
    expect(roundedSize(1)).toBe(4096);
    expect(roundedSize(4095)).toBe(4096);
    expect(roundedSize(4097)).toBe(8192);
});

test('board answers to the upload', () => {
    expect(uploadOutcome(200, 'OK')).toBe('done');
    expect(uploadOutcome(200, 'OK\n')).toBe('done');
    expect(uploadOutcome(409, 'ERROR: modules installed, MODULES.OFF first')).toBe('error');
    expect(uploadOutcome(400, 'ERROR: No magic byte / invalid flash config')).toBe('error');
    expect(uploadOutcome(200, 'ERROR: something')).toBe('error');
});

const IMAGES = ['beken-quectel', 'beken-tuya', 'esp32', 'esp8266'];

test('running image gets its own files', () => {
    expect(imageCandidates(IMAGES, 'esp32', 'esp32')).toEqual([{image: 'esp32'}]);
    expect(imageCandidates(IMAGES, 'beken-quectel', 'beken')).toEqual([{image: 'beken-quectel'}]);
    expect(imageCandidates(IMAGES, 'esp8266', 'esp8266')).toEqual([{image: 'esp8266'}]);
});

test('local build without an image name lists every image of its chip', () => {
    expect(imageCandidates(IMAGES, '', 'beken').map((x) => x.image)).toEqual(['beken-quectel', 'beken-tuya']);
    expect(imageCandidates(IMAGES, '', '').length).toBe(IMAGES.length);
});

test('image the release does not have', () => {
    expect(imageCandidates(['beken-bk7231n', 'esp32'], 'beken-matter', 'beken')).toEqual([]);
});

/**
 * @param {string} name
 * @param {string} kind
 * @param {number} size
 * @returns {import('../src/fwinstall.mjs').FirmwareFile}
 */
function file(name, kind, size) {
    return {name, kind, size, url: `esp32-261111-abcdef1/${name}`, sha256: 'ab'.repeat(32)};
}

/** @type {import('../src/fwinstall.mjs').FirmwareManifest} */
const MANIFEST = {
    version: 1,
    repository: 'igorufox/espurna',
    generated: '2026-11-11T12:00:00Z',
    releases: [
        {
            tag: 'esp32-261111-abcdef1',
            date: '2026-11-11T10:00:00Z',
            commit: 'abcdef1234567890abcdef1234567890abcdef12',
            images: {
                // the .ota of the chip is listed under every image of it (one install image for both bootloaders)
                'beken-tuya': [file('espurna-beken-tuya.uf2', 'serial', 1400000), file('espurna-beken.ota', 'install', 2100000)],
                'beken-quectel': [file('espurna-beken-quectel.uf2', 'serial', 1400000), file('espurna-beken.ota', 'install', 2100000)],
                'esp32': [file('espurna-esp32.bin', 'serial', 1900000), file('espurna-esp32.ota', 'install', 2600000)],
                'esp8266': [file('espurna-esp8266.bin', 'serial', 500000), file('espurna-esp8266.ota', 'install', 1100000)],
            },
        },
        {
            tag: 'esp32-261105-1234567',
            date: '2026-11-05T10:00:00Z',
            commit: '1234567',
            images: {
                'beken-bk7231n': [file('espurna-beken-bk7231n.uf2', 'ota', 1400000)],
            },
        },
    ],
};

test('release rows carry the files of the running image, the install image is the one to install', () => {
    const rows = firmwareRows(MANIFEST, {image: 'beken-quectel', arch: 'beken', version: '26.11.0-gitabcdef12'});
    expect(rows.length).toBe(2);

    expect(rows[0].tag).toBe('esp32-261111-abcdef1');
    expect(rows[0].running).toBe(true);
    expect(rows[0].choices.map((x) => x.file.name)).toEqual(['espurna-beken-quectel.uf2', 'espurna-beken.ota']);
    // every chip installs the same way: the .ota, never a bare image
    expect(rows[0].choices.map((x) => installable(x.file))).toEqual([false, true]);

    // older release with the old Beken names has nothing for the 'beken-quectel' image
    expect(rows[1].running).toBe(false);
    expect(rows[1].choices).toEqual([]);

    for (const image of ['esp8266', 'esp32', 'beken-tuya']) {
        const row = firmwareRows(MANIFEST, {image, arch: '', version: ''})[0];
        expect(row.choices.filter((x) => installable(x.file)).map((x) => x.file.kind)).toEqual(['install']);
    }
});

test('broken manifest', () => {
    expect(() => firmwareRows(/** @type {any} */({}), {image: '', arch: '', version: ''}))
        .toThrow(/firmware.json/);
});

test('the MODULES panel takes the install image of the build the board runs', () => {
    const base = 'https://igorufox.github.io/espurna/firmware.json';
    expect(installFileOf(MANIFEST, {image: 'esp32', commit: 'abcdef1', base})).toMatchObject({
        name: 'espurna-esp32.ota', kind: 'install', size: 2600000,
        url: 'https://igorufox.github.io/espurna/esp32-261111-abcdef1/espurna-esp32.ota'});
    expect(installFileOf(MANIFEST, {image: 'esp32', commit: '7654321', base})).toBe(null);
});

test('the version after the reload against the build the install asked for', () => {
    expect(installOutcome('26.11.0-gitabcdef1', {commit: 'abcdef1'})).toBe('installed');
    expect(installOutcome('26.11.0-git1234567', {commit: 'abcdef1'})).toBe('other');
    expect(installOutcome('26.11.0-git1234567', {commit: ''})).toBe(null);
});
