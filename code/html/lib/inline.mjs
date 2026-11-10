/** @import { JSDOM } from 'jsdom' */

/** @typedef InlineOptions
 * @property {function(string, string, string): (Promise<string> | string)} [resolve]
 * process raw input src=..., generate a valid path for the load(...)
 * @property {function(string, string, string): (Promise<string> | string)} [load]
 * process fs path from src=... or load(...) and return the 'code' to-be injected into the resulting element
 * @property {function(string): (void | Promise<void>)} [post]
 * execute some action when code was successfuly loaded into the dom
 */

/** 
 * @param {JSDOM} dom 
 * @param {Element} elem 
 * @param {InlineOptions} options
 */
export async function maybeInline(dom, elem, {resolve, load, post} = {}) {
    let attr = '';

    let src = '';
    let tag = '';

    let type = elem.getAttribute('type') ?? '';

    switch (elem.tagName) {
    case 'LINK':
        attr = 'href';
        if (elem.getAttribute('rel') === 'stylesheet') {
            tag = 'STYLE';
        } else {
            tag = elem.tagName;      
        }
        src = elem.getAttribute(attr) ?? '';
        break;

    case 'SCRIPT':
        attr = 'src';
        tag = elem.tagName;
        src = elem.getAttribute(attr) ?? '';
        break;
    }

    if (tag && src) {
        if (src.startsWith('data:')) {
            return;
        }

        if (src.startsWith('/')) {
            src = src.slice(1);
        }

        if (!load) {
            return;
        }

        let resolved = src;
        if (resolve) {
            resolved = await Promise.resolve(resolve(src, tag, type));
        }

        const code = await Promise.resolve(load(resolved, tag, type));
        if (!code) {
            return;
        }

        switch (tag) {
        case 'LINK':
            if (elem.getAttribute('rel') !== 'icon') {
                return;
            }
            if (!code.startsWith('data:')) {
                return;
            }
            elem.setAttribute(attr, code);
            break;

        case 'STYLE':
        {
            const style = dom.window.document.createElement(tag);
            style.innerHTML = code;
            elem.parentElement?.replaceChild(style, elem);
            break;
        }

        case 'SCRIPT':
            elem.removeAttribute('crossorigin');
            elem.removeAttribute(attr);
            elem.innerHTML = code;
            break;

        default:
            return;
        }

        if (post) {
            await Promise.resolve(post(resolved));
        }
    }
}

/**
 *
 * ref. https://github.com/evanw/esbuild/issues/1895
 * from our side, html/src/*.mjs (with the exception of index.mjs) require 'init()' call to be actually set up and used
 * as the result, no code from the module should be bundled into the output when module was not initialized
 * however, since light module depends on iro.js and does not have `sideEffects: false` in package.json, it would still get bundled because of top-level import
 * (...and since module modifying something in global scope is not unheard of...)
 * @returns {import('esbuild').Plugin}
 */
export function forceNoSideEffects() {
    return {
        name: 'no-side-effects',
        setup(build) {
            build.onResolve({filter: /@jaames\/iro/, namespace: 'file'},
                async ({path, ...options}) => {
                    const result = await build.resolve(path, {...options, namespace: 'noRecurse'});
                    return {...result, sideEffects: false};
                });
        },
    };
}
