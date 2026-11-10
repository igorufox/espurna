import {
    MODE_DEV,
    SPEC_DIR,
    VENDOR_DIR,
    modeFile,
} from './gulpfile.mjs';

import { defineConfig } from 'vitest/config';

export default defineConfig({
    test: {
        environment: 'jsdom',
        dir: SPEC_DIR,
    },
    // vite-specific overrides, both for the build and importmap helper script
    resolve: {
        alias: [
            {find: '/vendor', replacement: VENDOR_DIR},
            {find: '@build-mode', replacement: modeFile(MODE_DEV)},
        ],
    },
});
