import { afterEach, describe, expect, test, vi } from 'vitest';

vi.mock('../src/connection.mjs', async (importOriginal) => ({
    ...(await importOriginal()),
    send: vi.fn(),
    sendAction: vi.fn(),
}));

const { send, sendAction } = await import('../src/connection.mjs');
const { applyHardware, hardwareMessage } = await import('../src/applyHardware.mjs');

describe('hardwareMessage', () => {
    test('old relay, button and LED keys go away, the new ones stay', () => {
        const {set, del} = hardwareMessage({relayGpio0: '12', btnGpio0: '0', ledGpio0: '13'});
        expect(set).toEqual({relayGpio0: '12', btnGpio0: '0', ledGpio0: '13'});
        expect(del).toContain('relayGpio1');
        expect(del).toContain('relayGpio15');
        expect(del).toContain('btnGpio3');
        expect(del).toContain('btnRelay0');
        expect(del).toContain('ledInv0');
        expect(del).not.toContain('relayGpio0');
        expect(del).not.toContain('btnGpio0');
        expect(del).not.toContain('ledGpio0');
    });

    test('only relay, button and LED keys are deleted, never the network or the sensors', () => {
        const {del} = hardwareMessage({});
        expect(del.every((key) => /^(relay|btn|led)/.test(key))).toBe(true);
        expect(del).not.toContain('dhtPin0');
    });

    test('long values are left out, 64 characters still go', () => {
        const {set} = hardwareMessage({sysDevice: 'x'.repeat(65), sysManufacturer: 'y'.repeat(64)});
        expect(set).toEqual({sysManufacturer: 'y'.repeat(64)});
    });
});

describe('applyHardware', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.unstubAllGlobals();
        vi.mocked(send).mockClear();
        vi.mocked(sendAction).mockClear();
    });

    test('nothing goes out when the question is answered no', () => {
        vi.stubGlobal('confirm', () => false);
        expect(applyHardware({relayGpio0: '12'}, 'Apply?')).toBe(false);
        expect(send).not.toHaveBeenCalled();
    });

    test('one settings message, then the reboot question', () => {
        vi.useFakeTimers();
        const questions = [];
        vi.stubGlobal('confirm', (/** @type {string} */text) => {
            questions.push(text);
            return true;
        });

        expect(applyHardware({relayGpio0: '12'}, 'Apply "Sonoff Basic"?')).toBe(true);
        expect(send).toHaveBeenCalledTimes(1);
        const message = JSON.parse(vi.mocked(send).mock.calls[0][0]);
        expect(message.settings).toEqual(hardwareMessage({relayGpio0: '12'}));

        vi.advanceTimersByTime(500);
        expect(questions.length).toBe(2);
        expect(questions[0]).toContain('Apply "Sonoff Basic"?');
        expect(sendAction).toHaveBeenCalledWith('reboot');
    });
});
