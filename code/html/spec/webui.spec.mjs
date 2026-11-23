import { beforeAll, describe, expect, test } from 'vitest';

import { modulesVisible } from '../src/core.mjs';
import { CORE, PARTS, coreName } from '../parts.mjs';

import { readFile, readdir } from 'node:fs/promises';

// One WebUI for every chip and board: index.html carries every panel, the device decides which ones are shown

const HTML_DIR = `${import.meta.dirname}/..`;

/** @param {string} name */
async function read(name) {
    return (await readFile(`${HTML_DIR}/${name}`)).toString();
}

describe('index.html', () => {
    test('every panel, status block and template of src/ is in index.html or in one part of a module', async () => {
        const index = await read('index.html');
        const menu = await read('src/menu-list.html');

        const sources = new Set(
            Array.from(index.matchAll(/<inline-source\s+src="([^"]+)"/g))
                .map((x) => x[1].replace(/^\//, '')));

        /** @type {Map<string, string[]>} */
        const owners = new Map();
        for (const [name, part] of Object.entries(PARTS)) {
            for (const file of [...(part.status ?? []), ...(part.panels ?? []), ...(part.templates ?? [])]) {
                owners.set(file, [...(owners.get(file) ?? []), name]);
            }
        }

        const files = (await readdir(`${HTML_DIR}/src`))
            .filter((x) => /^(panel|status|template)-.*\.html$/.test(x) || x === 'menu-list.html');

        for (const file of files) {
            const where = [...(sources.has(`src/${file}`) ? ['index.html'] : []), ...(owners.get(`src/${file}`) ?? [])];
            expect(where, file).toHaveLength(1);
        }

        for (const marker of ['status', 'panels', 'templates']) {
            expect(index).toContain(`data-parts="${marker}"`);
        }

        expect(menu).toContain('data-panel="mod"');
    });

    test('every style of src/ is in index.html or in one part', async () => {
        const index = await read('index.html');
        const linked = new Set(Array.from(index.matchAll(/<link rel="stylesheet" href="(src\/[^"]+)"/g)).map((x) => x[1]));
        const owned = Object.values(PARTS).flatMap((part) => part.css ?? []);

        const files = (await readdir(`${HTML_DIR}/src`)).filter((x) => x.endsWith('.css')).map((x) => `src/${x}`);
        for (const file of files) {
            expect([linked.has(file), ...owned.filter((x) => x === file)].filter((x) => x), file).toHaveLength(1);
        }
    });
});

describe('parts', () => {
    test('the page gives the parts every head file of html/parts.mjs CORE, by the same name', async () => {
        const loader = await read('src/parts.mjs');
        for (const file of CORE) {
            const relative = file.replace(/^src\//, './');
            expect(loader, file).toContain(`import * as ${coreName(file)} from '${relative}';`);
            expect(loader, file).toMatch(new RegExp(`^    ${coreName(file)},$`, 'm'));
        }
    });

    test('a part script imports no other part script (it would bundle a second copy of it)', async () => {
        const scripts = new Set(Object.values(PARTS).map((part) => part.script).filter((x) => x));
        for (const script of scripts) {
            const text = await read(/** @type {string} */(script));
            for (const [, imported] of text.matchAll(/from '\.\/([^']+)'/g)) {
                expect(scripts.has(`src/${imported}`), `${script} imports ${imported}`).toBeFalsy();
            }
        }
    });

    test('every part names a module of the firmware (config/head.h ESPURNA_MODULES)', async () => {
        const head = (await readFile(`${HTML_DIR}/../espurna/config/head.h`)).toString();
        const list = head.slice(head.indexOf('ESPURNA_MODULES'), head.indexOf('ESPURNA_MODULES*/'));
        const modules = new Set(Array.from(list.matchAll(/^([a-z0-9_]+) \|/gm)).map((x) => x[1]));
        for (const name of Object.keys(PARTS)) {
            expect(modules.has(name), name).toBeTruthy();
        }
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
        for (const module of ['relay', 'light', 'sns', 'matter', 'pages', 'nextion', 'curtain', 'mod', 'mqtt']) {
            expect(shown(module), module).toBeFalsy();
        }
    });

    test('only the modules of the device are shown', () => {
        modulesVisible(['relay', 'mqtt', 'mod']);

        for (const module of ['relay', 'mqtt', 'mod']) {
            expect(shown(module), module).toBeTruthy();
        }

        for (const module of ['light', 'sns', 'matter', 'pages', 'nextion', 'curtain', 'garland', 'thermostat']) {
            expect(shown(module), module).toBeFalsy();
        }
    });
});
