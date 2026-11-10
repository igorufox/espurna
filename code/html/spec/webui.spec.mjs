import { beforeAll, describe, expect, test } from 'vitest';

import { modulesVisible } from '../src/core.mjs';

import { readFile, readdir } from 'node:fs/promises';

// One WebUI for every chip and board: index.html carries every panel, the device decides which ones are shown

const HTML_DIR = `${import.meta.dirname}/..`;

/** @param {string} name */
async function read(name) {
    return (await readFile(`${HTML_DIR}/${name}`)).toString();
}

describe('index.html', () => {
    test('includes every panel and status block of src/', async () => {
        const index = await read('index.html');
        const menu = await read('src/menu-list.html');

        const sources = new Set(
            Array.from(index.matchAll(/<inline-source\s+src="([^"]+)"/g))
                .map((x) => x[1].replace(/^\//, '')));

        const files = (await readdir(`${HTML_DIR}/src`))
            .filter((x) => /^(panel|status|template)-.*\.html$/.test(x) || x === 'menu-list.html');

        for (const file of files) {
            expect(sources, file).toContain(`src/${file}`);
        }

        expect(menu).toContain('data-panel="mod"');
    });

    test('no build preset left', async () => {
        const index = await read('index.html');
        expect(index).not.toMatch(/@build-preset/);

        const html = await readdir(HTML_DIR);
        expect(html).not.toContain('preset');
    });
});

describe('modulesVisible', () => {
    beforeAll(async () => {
        const style = document.createElement('style');
        style.textContent = '.module { display: none; }';
        document.head.appendChild(style);

        document.body.innerHTML = `<ul>${await read('src/menu-list.html')}</ul>`;
    });

    /** @param {string} module */
    function shown(module) {
        const elem = document.querySelector(`.module-${module}`);
        expect(elem, module).not.toBeNull();
        return window.getComputedStyle(/** @type {Element} */(elem)).display !== 'none';
    }

    test('every module menu entry is hidden until the device names it', () => {
        for (const module of ['relay', 'light', 'sns', 'matter', 'nspanel', 'curtain', 'mod', 'mqtt']) {
            expect(shown(module), module).toBeFalsy();
        }
    });

    test('only the modules of the device are shown', () => {
        modulesVisible(['relay', 'mqtt', 'mod']);

        for (const module of ['relay', 'mqtt', 'mod']) {
            expect(shown(module), module).toBeTruthy();
        }

        for (const module of ['light', 'sns', 'matter', 'nspanel', 'curtain', 'garland', 'thermostat']) {
            expect(shown(module), module).toBeFalsy();
        }
    });
});
