// Firmware install from GitHub (ADMIN, OTA)
//
// CI copies the files of the kept releases to GitHub Pages with a firmware.json (`webFwUrl` replaces the default one).
// The release files themselves cannot be used: GitHub serves them without CORS, so the browser cannot download them.
// Every chip installs the same way: this browser downloads the install image espurna-<chip>.ota of the release
// (SHA-256 of firmware.json) and hands it to install.mjs installImage, the same install as the MODULES panel's Apply:
// the head of this board out of it (the chips differ only in the data of HEAD_ROUTES), then the modules the board runs
// now out of the same file. The other files of a release (the initial images for the wire) are download links.

import { fetchBytes, fetchJson, safeHref } from './net.mjs';
import { installImage, readModules, runningNames } from './install.mjs';
import { notifyError } from './notify.mjs';
import { listenAppConnected } from './connection.mjs';
import { variableListeners } from './settings.mjs';
import { pageReloadIn } from './core.mjs';

export const FIRMWARE_URL = 'https://igorufox.github.io/espurna/firmware.json';

/**
 * @typedef {{name: string, kind: string, size: number, url: string, sha256?: string}} FirmwareFile
 * @typedef {{tag: string, date: string, commit: string, release?: string, images: {[k: string]: FirmwareFile[]}}} FirmwareRelease
 * @typedef {{version: number, repository?: string, generated?: string, releases: FirmwareRelease[]}} FirmwareManifest
 */

/**
 * @typedef {{image: string, file: FirmwareFile}} FirmwareChoice
 * @typedef {{tag: string, date: string, commit: string, release: string, running: boolean, choices: FirmwareChoice[]}} FirmwareRow
 */

// ---------------------------------------------------------------------------
// image headers, used by Browse / Upgrade
// ---------------------------------------------------------------------------

/**
 * @param {Uint8Array} buffer
 * @param {string} text
 * @returns {boolean}
 */
function startsWithText(buffer, text) {
    if (buffer.length < text.length) {
        return false;
    }

    for (let index = 0; index < text.length; ++index) {
        if (buffer[index] !== text.charCodeAt(index)) {
            return false;
        }
    }

    return true;
}

/**
 * What kind of image the first bytes belong to
 * - 'esp' - ESP8266 / ESP32 application (magic 0xE9), buffer[2] is the flash mode
 * - 'gzip' - compressed ESP8266 image
 * - 'uf2' - LibreTiny UF2 (Beken)
 * @param {Uint8Array} buffer first 8 bytes (or more) of the file
 * @returns {string | null}
 */
export function firmwareHeader(buffer) {
    if ((buffer.length >= 2) && (0x1f === buffer[0]) && (0x8b === buffer[1])) {
        return 'gzip';
    }

    if ((buffer.length >= 1) && (0xe9 === buffer[0])) {
        return 'esp';
    }

    if (startsWithText(buffer, 'UF2\n')) {
        return 'uf2';
    }

    return null;
}

/**
 * OTA space the image takes, in whole flash sectors
 * @param {number} size
 * @returns {number}
 */
export function roundedSize(size) {
    return (size - (size % 4096)) + 4096;
}

/**
 * What the board answered to an upload to /upgrade (Browse / Upgrade)
 * - 'done' - written, the board reboots into it
 * - 'error'
 * @param {number} status
 * @param {string} text
 * @returns {string}
 */
export function uploadOutcome(status, text) {
    return ((status === 200) && (text.trim() === 'OK')) ? 'done' : 'error';
}

// ---------------------------------------------------------------------------
// release list
// ---------------------------------------------------------------------------

/**
 * Images of a release the board can be updated to: the running image; no image (local build): every image of its chip
 * @param {string[]} images names in the release
 * @param {string} image running one, '' when not known
 * @param {string} arch esp8266, esp32, beken
 * @returns {{image: string}[]}
 */
export function imageCandidates(images, image, arch) {
    if (!image) {
        return images
            .filter((name) => !arch || name.startsWith(arch))
            .map((name) => ({image: name}));
    }

    return images.includes(image) ? [{image}] : [];
}

/**
 * Rows of the release list, newest release first, files the board can be updated with
 * @param {FirmwareManifest} manifest
 * @param {{image: string, arch: string, version: string}} board
 * @returns {FirmwareRow[]}
 */
export function firmwareRows(manifest, {image, arch, version}) {
    if (!manifest || !Array.isArray(manifest.releases)) {
        throw new Error('not a firmware.json (no releases)');
    }

    return manifest.releases.map((release) => {
        const images = release.images || {};
        const candidates = imageCandidates(Object.keys(images), image, arch);

        /** @type {FirmwareChoice[]} */
        const choices = [];
        for (const candidate of candidates) {
            for (const file of images[candidate.image] || []) {
                choices.push({...candidate, file});
            }
        }

        const commit = String(release.commit || '');
        const short = commit.slice(0, 7);

        return {
            tag: String(release.tag),
            date: String(release.date || ''),
            commit,
            release: String(release.release || ''),
            running: (short.length === 7) && version.includes(short),
            choices,
        };
    });
}

/**
 * The install image (.ota) of one build in the release list, its link made absolute (http/https only): the one place
 * that reads the list for the modules panel too
 * @param {FirmwareManifest} manifest
 * @param {{image: string, commit: string, base: string}} board
 * @returns {FirmwareFile | null}
 */
export function installFileOf(manifest, {image, commit, base}) {
    if (!manifest || !Array.isArray(manifest.releases)) {
        throw new Error('not a firmware.json (no releases)');
    }

    for (const release of manifest.releases) {
        if (commit && !String(release.commit ?? '').startsWith(commit)) {
            continue;
        }

        for (const file of (release.images ?? {})[image] ?? []) {
            const url = installable(file) ? safeHref(file.url, base) : '';
            if (url) {
                return {...file, url};
            }
        }
    }

    return null;
}

/**
 * Whether the Install button takes the file: the install image (.ota), every chip
 * @param {FirmwareFile} file
 * @returns {boolean}
 */
export function installable(file) {
    return file.kind === 'install';
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

const state = {
    url: '',
    image: '',
    imageKnown: false,
    arch: '',
    version: '',
    busy: false,
    /** @type {import('./connection.mjs').ConnectionUrls | null} */
    urls: null,
};

/** @returns {string} */
function manifestUrl() {
    return state.url.trim() || FIRMWARE_URL;
}

/**
 * @param {string} text
 * @param {string} [className]
 */
function setStatus(text, className = '') {
    const status = document.getElementById('firmware-status');
    if (status) {
        status.textContent = text;
        status.className = className;
    }
}

/** @param {boolean} disabled */
function releaseButtons(disabled) {
    for (const button of document.querySelectorAll('#firmware-releases button')) {
        /** @type {HTMLButtonElement} */(button).disabled = disabled;
    }
}

/**
 * @param {FirmwareRow} row
 * @param {FirmwareChoice} choice
 */
async function install(row, choice) {
    const urls = state.urls;
    if (!urls) {
        alert('Not connected');
        return;
    }

    if (state.busy) {
        return;
    }

    const {file} = choice;
    const warning = state.imageKnown ? ''
        : '\n\nThis build does not say which image it is: make sure it is the one for this board.';
    if (!window.confirm(`Install ${file.name} of ${releaseName(row)} on this board?\n\n`
        + 'Its head goes on first, then the modules the board runs now, out of the same file. The board restarts a few'
        + ` times, keep it powered until it is back.${warning}`))
    {
        return;
    }

    state.busy = true;
    releaseButtons(true);

    try {
        const root = new URL('./', urls.auth);
        const status = await readModules(root);
        const buffer = await fetchBytes(safeHref(file.url, manifestUrl()), file, (text) => setStatus(text));
        await installImage(root, buffer, status, runningNames(status), (text) => setStatus(text));

        // "installed" only once the page sees the build it asked for (installCheck)
        expectBuild(String(row.commit || '').slice(0, 7), file.name);
        setStatus(`${file.name} written, the board is back. Reloading the page to check its version...`);
        pageReloadIn(2000);
    } catch (e) {
        setStatus(`Install stopped: ${/** @type {Error} */(e).message}`, 'firmware-error');
        notifyError(/** @type {Error} */(e));
        state.busy = false;
        releaseButtons(false);
    }
}

const EXPECT_KEY = 'fwinstall.expect';

/**
 * @param {string} commit
 * @param {string} name
 */
function expectBuild(commit, name) {
    try {
        sessionStorage.setItem(EXPECT_KEY, JSON.stringify({commit, name}));
    } catch {
        // no storage: no check after the reload
    }
}

/**
 * The version after the reload against the build the install asked for
 * @param {string} version
 * @param {{commit: string}} expect
 * @returns {'installed' | 'other' | null}
 */
export function installOutcome(version, {commit}) {
    if (!commit) {
        return null;
    }

    return version.includes(commit) ? 'installed' : 'other';
}

/** @param {string} version */
function installCheck(version) {
    let expect;
    try {
        expect = JSON.parse(sessionStorage.getItem(EXPECT_KEY) || 'null');
        sessionStorage.removeItem(EXPECT_KEY);
    } catch {
        return;
    }
    if (!expect) {
        return;
    }

    const outcome = installOutcome(version, expect);
    if (outcome === 'installed') {
        setStatus(`${expect.name} installed, the board runs ${version}`, 'firmware-ok');
    } else if (outcome === 'other') {
        setStatus(`${expect.name} was written, but the board runs ${version}, not build ${expect.commit}`, 'firmware-error');
    }
}

/**
 * @param {number} size
 * @returns {string}
 */
function sizeText(size) {
    return `${Math.round(size / 1024)} KB`;
}

/**
 * @param {FirmwareRow} row
 * @param {FirmwareChoice} choice
 * @returns {HTMLLIElement}
 */
function choiceItem(row, choice) {
    const item = document.createElement('li');
    const {file} = choice;

    const name = document.createElement('code');
    name.textContent = file.name;
    item.appendChild(name);

    const info = [sizeText(file.size)];
    if (!installable(file)) {
        info.push((file.kind === 'serial') ? 'serial flash only' : file.kind);
    }

    item.appendChild(document.createTextNode(` (${info.join(', ')}) `));

    if (installable(file)) {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'pure-button button-firmware-install';
        button.textContent = 'Install';
        button.disabled = state.busy;
        button.addEventListener('click', (event) => {
            event.preventDefault();
            install(row, choice);
        });
        item.appendChild(button);
    } else {
        const href = safeHref(file.url, manifestUrl());
        if (href) {
            const link = document.createElement('a');
            link.href = href;
            link.textContent = 'download';
            item.appendChild(link);
        }
    }

    return item;
}

/**
 * The build date names a release: CI tags start with 'esp32-' for every chip
 * @param {FirmwareRow} row
 * @returns {string}
 */
export function releaseName(row) {
    return row.date
        ? row.date.replace('T', ' ').replace(/:\d\dZ$/, ' UTC')
        : row.tag;
}

/**
 * @param {HTMLElement} list
 * @param {FirmwareRow[]} rows
 */
function showRows(list, rows) {
    list.replaceChildren();

    for (const row of rows) {
        const item = document.createElement('li');

        // the commit links to the release page
        const title = document.createElement('strong');
        title.textContent = releaseName(row);
        item.appendChild(title);

        if (row.commit) {
            item.appendChild(document.createTextNode(', commit '));
            const short = row.commit.slice(0, 7);
            const release = safeHref(row.release);
            if (release) {
                const link = document.createElement('a');
                link.href = release;
                link.target = '_blank';
                link.rel = 'noopener';
                link.textContent = short;
                item.appendChild(link);
            } else {
                item.appendChild(document.createTextNode(short));
            }
        }

        if (row.running) {
            item.appendChild(document.createTextNode(', running now'));
        }

        const files = document.createElement('ul');
        // 'app': ESPurna alone (ESP32), only for installing over Tasmota by hand, not a choice here
        for (const choice of row.choices.filter((other) => other.file.kind !== 'app')) {
            files.appendChild(choiceItem(row, choice));
        }

        item.appendChild(files);
        list.appendChild(item);
    }
}

/** @param {HTMLElement} list */
async function loadReleases(list) {
    if (state.busy) {
        return;
    }

    const url = manifestUrl();
    setStatus(`Downloading ${url}...`);

    try {
        // releases without a file for this board are not listed
        const rows = firmwareRows(await fetchJson(url), {
            image: state.image, arch: state.arch, version: state.version})
            .filter((row) => row.choices.length > 0);
        showRows(list, rows);

        const count = rows.length;
        setStatus(!count
            ? `No release has files of the ${state.image || state.arch || 'this'} image.`
            : state.imageKnown
            ? `${count} release(s), files of the ${state.image} image.`
            : `${count} release(s). This build does not say which image it is (local build): all ${state.arch || ''} images are listed, pick the one built for this board.`,
            state.imageKnown ? '' : 'firmware-error');
    } catch (e) {
        list.replaceChildren();
        setStatus(`Could not load ${url}: ${/** @type {Error} */(e).message}`, 'firmware-error');
    }
}

export function init() {
    const button = document.querySelector('.button-firmware-list');
    const list = document.getElementById('firmware-releases');
    const image = document.getElementById('firmware-image');
    const url = /** @type {HTMLInputElement | null} */
        (document.querySelector('input[name="webFwUrl"]'));

    if (!(button instanceof HTMLButtonElement) || !list || !image || !url) {
        return;
    }

    variableListeners({
        'fwImage': (_, value) => {
            state.image = String(value);
            state.imageKnown = state.image.length > 0;
            image.textContent = state.imageKnown ? state.image : 'not known (local build)';
        },
        'arch': (_, value) => {
            state.arch = String(value);
        },
        'app_version': (_, value) => {
            state.version = String(value);
            installCheck(state.version);
        },
        'webFwUrl': (_, value) => {
            state.url = String(value);
        },
    });

    // used as typed, Save keeps it on the device for the next time
    url.addEventListener('change', () => {
        state.url = url.value;
    });

    listenAppConnected((urls) => {
        state.urls = urls;
    });

    button.addEventListener('click', (event) => {
        event.preventDefault();
        loadReleases(list);
    });
}
