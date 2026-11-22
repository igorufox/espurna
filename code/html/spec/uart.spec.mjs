import { expect, test } from 'vitest';
import { PIN_NONE, fallbackText, pinOptions, releaseText } from '../src/uart.mjs';

test('pin options: the build pin first, the other pins of the chip, none', () => {
    expect(pinOptions(3).map((x) => x.value)).toStrictEqual(['3', `${PIN_NONE}`]);
    expect(pinOptions(11)[0].text).toContain('GPIO11');
    expect(pinOptions(3, [3, 13]).map((x) => x.value)).toStrictEqual(['3', '13', `${PIN_NONE}`]);
    expect(pinOptions(1, [1, 2, 15]).map((x) => x.value)).toStrictEqual(['1', '2', '15', `${PIN_NONE}`]);
});

test('fallback text only when the saved pair has no UART', () => {
    expect(fallbackText({tx: 1, rx: 3, txUser: '', rxUser: ''})).toBe('');
    expect(fallbackText({tx: 1, rx: 3, txUser: '', rxUser: '', fallback: true}))
        .toContain('stays on GPIO1 / GPIO3');
});

test('release text names the setting that took the pin', () => {
    expect(releaseText({tx: 1, rx: 3, txUser: '', rxUser: ''})).toBe('');
    expect(releaseText({tx: 1, rx: 3, txUser: '', rxUser: 'btnGpio2'}))
        .toBe('GPIO3 (RX) is used by btnGpio2: the serial port leaves it, nothing to set here.');
    expect(releaseText({tx: 11, rx: 10, txUser: 'ledGpio0', rxUser: 'relayGpio0'}))
        .toContain('GPIO11 (TX) is used by ledGpio0, GPIO10 (RX) is used by relayGpio0');
});
