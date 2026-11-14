import { createNodeList } from './template.mjs';
import { variableListeners } from './settings.mjs';

/** @returns {import('./settings.mjs').KeyValueListeners} */
function listeners() {
    return {
        "dczRelays": (_, value) => {
            createNodeList("dcz-relays", value, "dczRelayIdx");
        },
    };
}

export function init() {
    variableListeners(listeners());
}
