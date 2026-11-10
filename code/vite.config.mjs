import * as path from 'node:path';
import * as fs from 'node:fs/promises';

import { minify as htmlMinifyImpl } from 'html-minifier-terser';

import { maybeInline } from './html/lib/inline.mjs';
import { defineConfig } from 'vite';
import { JSDOM } from 'jsdom';

import {
    BUILD_DIR,
    HTML_DIR,
    MODE_DEV,
    MODE_WEBUI,
    VENDOR_DIR,
    WEBUI_NAME,
    modeFile,
} from './gulpfile.mjs';

// vite build pipeline is simplified compared to the gulp one
// assets are mostly handled through the rollup <-> vite
//
// one part is not
// - index.html pre-generation has to happen before build starts
//
// compared to (some) of the external plugins
// - https://www.npmjs.com/package/vite-plugin-singlefile
//   https://www.npmjs.com/package/vite-plugin-inline-source
//
//   html assets are sourced from the OutputBundle after the build
//   no extra config modifications; html inline handled differently
//
// - https://www.npmjs.com/package/vite-plugin-html
//   https://www.npmjs.com/package/vite-plugin-html-inject
//
//   inline-source custom element used instead of templating
//   package JSDOM dependency instead of plugin parser(s)

/** @import { PluginOption } from 'vite' */

/**
 * @returns {PluginOption}
 */
function inlineHtmlPre() {
    const sources = [];

    return {
        name: inlineHtmlPre.name,
        enforce: 'pre',
        configureServer(server) {
            server.watcher.on('change', (file) => {
                if (file.endsWith('.html')) {
                    server.ws.send({
                        type: 'full-reload',
                        path: '*',
                    });
                }
            });
        },
        async transformIndexHtml(html) {
            const dom = new JSDOM(html);
            const doc = dom.window.document;

            for (const elem of Array.from(doc.querySelectorAll('inline-source'))) {
                const src = elem.getAttribute('src');
                if (!src) {
                    continue;
                }

                const data = await fs.readFile(path.join(HTML_DIR, src));
                elem.outerHTML = data.toString();

                sources.push(src);
            }

            return dom.serialize();
        }
    };
}

/**
 * @returns {PluginOption}
 */
function inlineAssetsPost() {
    return {
        name: inlineAssetsPost.name,
        apply: 'build',
        enforce: 'post',
        async transformIndexHtml(html, ctx) {
            // possibly, everything was already handled by some other plugin
            if (!ctx.bundle) {
                return html;
            }

            /**
             * vite is expected to clean-up src=..., so no need to resolve(src)
             * bundle at this point should contain every remaining asset on the page
             * @param {string} src
             */
            function load(src) {
                const output = (ctx.bundle ?? {})[src];
                if (!output) {
                    return '';
                }

                let code = '';
                if (output.type === 'asset' && typeof output.source === 'string') {
                    code = output.source;
                }
                if (output.type === 'chunk') {
                    code = output.code;
                }

                return code;
            }

            /**
             * output bundle should be kept empty, only index.html is written
             * @param {string} src
             */
            function post(src) {
                delete (ctx.bundle ?? {})[src];
            }

            const dom = new JSDOM(html);
            const doc = dom.window.document;

            for (const elem of Array.from(doc.querySelectorAll('link,script'))) {
                await maybeInline(dom, elem, {load, post});
            }

            return dom.serialize();
        }
    };
}

/**
 * @returns {PluginOption}
 */
function htmlMinify() {
    return {
        name: htmlMinify.name,
        apply: 'build',
        enforce: 'post',
        async transformIndexHtml(html) {
            html = await htmlMinifyImpl(html, {
                collapseWhitespace: true,
                removeComments: true,
                minifyCSS: false,
                minifyJS: false,
            });

            return html;
        },
    };
}

/**
 * @returns {PluginOption}
 */
function rename() {
    return {
        name: rename.name,
        apply: 'build',
        enforce: 'post',
        generateBundle(_, bundle) {
            bundle['index.html'].fileName = WEBUI_NAME;
        },
    };
}

// `vite --mode dev`: the dev constants (html/mode/dev.mjs), any other mode builds the WebUI of the firmware
export default defineConfig(({ mode }) => {
    const buildMode = (mode === MODE_DEV) ? MODE_DEV : MODE_WEBUI;

    return {
        plugins: [
            inlineHtmlPre(),
            inlineAssetsPost(),
            htmlMinify(),
            rename(),
        ],
        // vite-specific overrides, both for the build and importmap helper script
        resolve: {
            alias: [
                {find: '/vendor', replacement: VENDOR_DIR},
                {find: '@build-mode', replacement: modeFile(buildMode)},
            ],
        },
        // one should be very careful when injecting relative paths in the html / js / configs
        // step into the HTML_DIR by default (aka `vite ... html/` in the cli)
        root: HTML_DIR,
        // rollup settings stay mostly the same, with the exception of the @jaames/iro workaround
        build: {
            modulePreload: {
                polyfill: false,
            },
            emptyOutDir: true,
            outDir: BUILD_DIR,
            rollupOptions: {
                treeshake: {
                    moduleSideEffects: () => false,
                },
            },
        },
    };
});
