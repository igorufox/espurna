Builder script is using **Gulp**

- https://gulpjs.com
- https://www.npmjs.com/package/gulp

Additional tools used
- [eslint](https://eslint.org) to validate javascript files
- [html-validate](https://html-validate.org) to validate html file
- [vitest](https://vitest.dev/) testing framework

All commands must be called from `code/`
```console
$ pwd
/home/dev/espurna/code
```

When `gulp` command is not available globally
```console
$ ./node_modules/.bin/gulp
```

# Files

- `code/espurna/static/index.html.ipp` - the WebUI, used in the firmware code
- `code/gulpfile.mjs` - builder script entrypoint
- `code/vite.config.mjs` - (experimental) vite dev server configuration
- `code/html/index.html` - main entrypoint, index.html template
- `code/html/build` - intermediate build results (`index.html`, `index.html.gz`)
- `code/html/mode` - `@build-mode` constants: `webui.mjs` for the firmware, `dev.mjs` for the dev server and tests
- `code/html/spec` - vitest 'spec' files
- `code/html/src` - source .mjs, .html, etc.
- `code/node_modules` - build dependencies
- `code/package{,-lock}.json` - build dependencies metadata

# Installation

*(recommended)* Install **exact** dependencies listed in the `package-lock.json`, from the time the builder code was last updated
```console
$ npm ci
```

*(optional)* Install dependencies listed in the `package.json`, possibly updating packages listed in the `package-lock.json`
```console
$ npm install
```

# Building

One WebUI for every chip and board. Every panel is in it, hidden by its `module module-<name>` class until the
device names `<name>` in the `modulesVisible` message of the WebSocket connection (only the modules the board
runs, the installable ones included).

Run tests and build the .html.ipp (`default` task)

```console
$ gulp
```

Build only (does not run tests)

```console
$ gulp build
```

List all available tasks
```console
$ gulp --tasks
```

# Development

Launches development server on port 8080 with the resulting .html bundle. No minification or compression, `html/mode/dev.mjs`: every panel shown, sample data instead of a device

```console
$ gulp dev
```

Only the `default` task depends on the test and lint tasks, call them manually

```console
$ gulp test
```
```console
$ gulp eslint html-validate vitest
```
```console
$ gulp eslint
$ gulp html-validate
$ gulp vitest
```

Gulp usually does not allow any task arguments, run these tools manually to select specific files or change command line arguments

```console
$ npm exec --no -- eslint gulpfile.mjs html/src/*.mjs html/spec/*.mjs
```
```console
$ npm exec --no -- html-validate html/src/*.html
```
```console
$ npm exec --no -- vitest --environment jsdom --dir html/spec --run
```

Experimental support of [Vite](https://vitejs.dev/) dev server is also included. Only `--mode dev` is expected to work locally

```console
$ vite --mode dev
```
