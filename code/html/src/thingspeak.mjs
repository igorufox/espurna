import { createNodeList } from './template.mjs';
import { variableListeners } from './settings.mjs';

/** @returns {import('./settings.mjs').KeyValueListeners} */
function listeners() {
    return {
        "tspkRelays": (_, value) => {
            createNodeList("tspk-relays", value, "tspkRelay");
        },
    };
}

export function init() {
    variableListeners(listeners());
}
