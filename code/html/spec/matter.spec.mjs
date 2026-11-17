import { expect, test } from 'vitest';
import { identityConfirmText, identityOptions, pemToDer, toBase64, vendorName, windowText } from '../src/matter.mjs';

test('identity: PEM becomes DER, DER and raw key bytes stay', () => {
    const der = new Uint8Array([0x30, 0x03, 0x02, 0x01, 0x01]);
    const pem = `-----BEGIN CERTIFICATE-----\n${toBase64(der)}\n-----END CERTIFICATE-----\n`;
    expect(Array.from(pemToDer(new TextEncoder().encode(pem)))).toEqual(Array.from(der));
    expect(pemToDer(der)).toBe(der);

    const raw = new Uint8Array(32).fill(0xA5);
    expect(pemToDer(raw)).toBe(raw);
    expect(toBase64(new Uint8Array([0xff, 0x00, 0x10]))).toBe("/wAQ");
});

test('identity: factory and custom only when the board has them', () => {
    const none = identityOptions(false, "");
    expect(none.factory.disabled).toBe(true);
    expect(none.factory.note).toContain("no factory data");
    expect(none.espurna.disabled).toBe(false);
    expect(none.custom.disabled).toBe(true);

    const both = identityOptions(true, "VID 0x1234 PID 0x0001");
    expect(both.factory.disabled).toBe(false);
    expect(both.custom.disabled).toBe(false);
    expect(both.custom.note).toContain("VID 0x1234");
});

test('identity: the confirmation says the controllers are lost', () => {
    expect(identityConfirmText("espurna")).toContain("development identity");
    expect(identityConfirmText("custom")).toContain("removed from all of them");
});

test('pairing window button text', () => {
    expect(windowText(0)).toBe("Open pairing window");
    expect(windowText(900)).toBe("Pairing window open, 15:00 left");
    expect(windowText(61)).toBe("Pairing window open, 1:01 left");
});

test('known controller vendors by name', () => {
    expect(vendorName(0x132F)).toBe("Smart speaker");
    expect(vendorName(0x1349)).toBe("Apple Home");
});

test('test vendor ids name the usual self-made controllers', () => {
    expect(vendorName(0xFFF1)).toContain("matter-server");
    expect(vendorName(0xFFF4)).toContain("0xFFF4");
});

test('other vendors as a number', () => {
    expect(vendorName(0x10)).toBe("Vendor 0x0010");
});
