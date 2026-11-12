import { expect, test } from 'vitest';
import { driverFieldOptions, parseDriverSpec } from '../src/sensor.mjs';

test('driver spec: fields, defaults and options', () => {
    const fields = parseDriverSpec(
        "dhtPin|gpio|GPIO|;dhtType|select|Type|22|11=DHT11,22=DHT22");
    expect(fields).toStrictEqual([
        {key: "dhtPin", type: "gpio", label: "GPIO", def: "", options: ""},
        {key: "dhtType", type: "select", label: "Type", def: "22", options: "11=DHT11,22=DHT22"},
    ]);
    expect(driverFieldOptions(fields[1])).toStrictEqual([["11", "DHT11"], ["22", "DHT22"]]);
});

test('driver spec: repeated fields get the index', () => {
    const fields = parseDriverSpec("eventsPin#|gpio|GPIO #|||3");
    expect(fields.map((field) => field.key)).toStrictEqual(["eventsPin0", "eventsPin1", "eventsPin2"]);
    expect(fields.map((field) => field.label)).toStrictEqual(["GPIO 1", "GPIO 2", "GPIO 3"]);
});

test('driver spec: an empty spec has no fields', () => {
    expect(parseDriverSpec("")).toStrictEqual([]);
});

test('driver fields: UART ports and I2C addresses', () => {
    const [port] = parseDriverSpec("pmsPort|uart|UART port|0");
    expect(driverFieldOptions(port)[0]).toStrictEqual(["0", "Off"]);

    const [address] = parseDriverSpec("sht3xAddr|i2c|Address|0");
    const options = driverFieldOptions(address);
    expect(options[0]).toStrictEqual(["0", "Find it"]);
    expect(options).toContainEqual(["68", "0x44"]);
});
