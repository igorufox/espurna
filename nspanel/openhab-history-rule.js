// openHAB rule (JS Scripting, ECMAScript 2021+) feeding the NSPanel thermostat history chart.
// Trigger: time cron "0 */15 * * * ?". Every 15 min it reads the last 24 h of each room from persistence,
// averages them into 96 points of 15 min (degrees x10, null when missing) and publishes them retained on
// istabai/history/<room>; a thermostat with "history": "istabai/history/<room>" shows them on the panel.
// Payload: {"from": <unix seconds of the first point>, "step": 900, "t": [<temperature>...], "s": [<setpoint>...]}

// room (the key the panel's "history" topic ends with) -> [temperature item, setpoint item]
const ROOMS = {
  'Living room': ['GF_LivingRoom_Temperature', 'GF_LivingRoom_Setpoint'],
  'Bedroom': ['FF_Bedroom_Temperature', 'FF_Bedroom_Setpoint'],
};
const TOPIC = 'istabai/history/';
const BROKER = 'mqtt:broker:mqtt';     // the MQTT broker thing
const SERVICE = 'rrd4j';               // persistence service
const STEP = 900;
const POINTS = 96;

const end = Math.floor(Date.now() / 1000 / STEP) * STEP + STEP;   // end of the current slot
const from = end - POINTS * STEP;
const begin = time.ZonedDateTime.ofInstant(time.Instant.ofEpochSecond(from), time.ZoneId.systemDefault());
const now = time.ZonedDateTime.now();
const mqtt = actions.get('mqtt', BROKER);

function seconds(p) {
  return p.instant ? p.instant.epochSecond() : p.timestamp.toEpochSecond();
}

function series(name) {
  const sum = new Array(POINTS).fill(0);
  const count = new Array(POINTS).fill(0);
  for (const p of items.getItem(name).persistence.getAllStatesBetween(begin, now, SERVICE)) {
    const value = p.numericState;
    const slot = Math.floor((seconds(p) - from) / STEP);
    if ((value === null) || isNaN(value) || (slot < 0) || (slot >= POINTS)) {
      continue;
    }
    sum[slot] += value;
    count[slot] += 1;
  }
  return sum.map((s, i) => (count[i] ? Math.round(s / count[i] * 10) : null));
}

for (const room of Object.keys(ROOMS)) {
  try {
    const [temperature, setpoint] = ROOMS[room];
    const payload = JSON.stringify({ from: from, step: STEP, t: series(temperature), s: series(setpoint) });
    mqtt.publishMQTT(TOPIC + room, payload, true);
  } catch (e) {
    console.warn('NSPanel history ' + room + ': ' + e);
  }
}
