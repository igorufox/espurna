import { afterEach, describe, expect, test, vi } from 'vitest';

vi.mock('../src/connection.mjs', async (importOriginal) => ({
    ...(await importOriginal()),
    send: vi.fn(),
    sendAction: vi.fn(),
}));

const { send, sendAction } = await import('../src/connection.mjs');
const { DEL_CHUNK, applyHardware, hardwareFrames, hardwareMessage } = await import('../src/applyHardware.mjs');

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

    test('long values are left out and named, 64 characters still go', () => {
        const {set, long} = hardwareMessage({sysDevice: 'x'.repeat(65), sysManufacturer: 'y'.repeat(64)});
        expect(set).toEqual({sysManufacturer: 'y'.repeat(64)});
        expect(long).toEqual(['sysDevice']);
    });

    test('keys without an index go away too: relayDummy, btnAcFreq, the global btnRepDel', () => {
        const {del} = hardwareMessage({relayGpio0: '12'});
        expect(del).toContain('relayDummy');
        expect(del).toContain('btnAcFreq');
        expect(del).toContain('btnRepDel');
        expect(hardwareMessage({relayDummy: '1'}).del).not.toContain('relayDummy');
    });
});

describe('hardwareFrames', () => {
    test('the deletions in small frames first, the values last, nothing lost', () => {
        const message = hardwareMessage({relayGpio0: '12'});
        expect(message.del.length).toBeGreaterThan(500);
        const frames = hardwareFrames(message);
        expect(frames.length).toBe(Math.ceil(message.del.length / DEL_CHUNK) + 1);
        expect(frames.every((frame) => frame.length < 1500)).toBe(true);
        const parsed = frames.map((frame) => JSON.parse(frame).settings);
        expect(parsed.slice(0, -1).flatMap((x) => x.del)).toEqual(message.del);
        expect(parsed.at(-1)).toEqual({set: {relayGpio0: '12'}, del: []});
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

    test('the question names the values that are too long', () => {
        const questions = [];
        vi.stubGlobal('confirm', (/** @type {string} */text) => {
            questions.push(text);
            return false;
        });
        applyHardware({sysDevice: 'x'.repeat(70)}, 'Apply?');
        expect(questions[0]).toContain('longer than 64 characters: sysDevice');
    });

    test('the settings frames, then the reboot question', () => {
        vi.useFakeTimers();
        const questions = [];
        vi.stubGlobal('confirm', (/** @type {string} */text) => {
            questions.push(text);
            return true;
        });

        expect(applyHardware({relayGpio0: '12'}, 'Apply "Sonoff Basic"?')).toBe(true);
        const frames = hardwareFrames(hardwareMessage({relayGpio0: '12'}));
        expect(vi.mocked(send).mock.calls.map((call) => call[0])).toEqual(frames);

        vi.advanceTimersByTime(500);
        expect(questions.length).toBe(2);
        expect(questions[0]).toContain('Apply "Sonoff Basic"?');
        expect(sendAction).toHaveBeenCalledWith('reboot');
    });
});
