# ESP32-ACCELSTEPPER-MQTT-CONTROL — move a stepper motor to MQTT-commanded positions with smooth acceleration

IoT example by [Fluidwire](https://fluidwire.com/).

## What it does

Connects an ESP32 to WiFi and an MQTT broker and drives a NEMA17 stepper through
an A4988 or DRV8825 driver. Send an integer to a command topic and the motor
moves to that **absolute position in steps**, ramping up and down with
[AccelStepper](https://www.airspayce.com/mikem/arduino/AccelStepper/) so it
doesn't stall or skip steps. The motor is stepped with a non-blocking `run()` on
every pass of `loop()`, so WiFi and MQTT stay responsive mid-move.

Three commands are understood: a step count (e.g. `1600`), `home` (drive toward a
limit switch, zero the position on contact, back off a few steps) and `stop`
(decelerate to a halt). Position is reported to the broker while moving and at
standstill, and a status topic says `homing`, `moving`, `idle` or `error`. Targets
outside `POS_MIN`..`POS_MAX` are rejected, and a homing run that never finds the
switch times out and disables the driver.

It's the actuation block for feeders, linear sliders, dispensers and turntables —
the kind of moving part that shows up in automation theses.

## Required hardware

- An ESP32 dev board (any variant with WiFi)
- An A4988 or DRV8825 stepper driver module
- A NEMA17 stepper motor
- A 12 V supply rated for the motor's current (a 2 A supply is fine for most NEMA17s)
- A 100 µF (or larger) electrolytic capacitor across the driver's VMOT/GND
- A limit switch (normally-open, wired to GND) for homing
- Jumper wires

## Required libraries

Install via the Arduino IDE Library Manager:

- **AccelStepper** by Mike McCauley
- **PubSubClient** by Nick O'Leary

`WiFi.h` ships with the ESP32 Arduino core, so no extra install is needed for it.

## Wiring / setup

| Driver pin      | Connect to                                  |
|-----------------|---------------------------------------------|
| STEP            | ESP32 **GPIO26**                            |
| DIR             | ESP32 **GPIO27**                            |
| EN              | ESP32 **GPIO25** (active LOW)               |
| RESET + SLEEP   | tied together (or both to 3.3 V)            |
| VDD             | ESP32 3.3 V                                 |
| GND (logic)     | ESP32 GND                                   |
| VMOT / GND      | 12 V supply, GND shared with the ESP32      |

The limit switch goes between **GPIO33** and GND; the internal pull-up is used, so
no resistor is needed. Wire the motor coils to the driver's 1A/1B and 2A/2B pins
(use a multimeter to find the two coil pairs if the wires are unmarked).

**Set the driver's current limit before running a real load.** Turn the trimpot on
the driver so the motor gets no more than its rated coil current. A4988:
`Vref = Imax × 8 × Rsense`. DRV8825: `Vref = Imax / 2`. Too high and the motor
and driver overheat; too low and it skips steps. And never connect or disconnect
the motor while the driver is powered — you can destroy it.

The microstepping pins (MS1–MS3) are left to your wiring; the defaults in the
sketch assume 16 microsteps (3200 steps per revolution), so `POS_MAX` is two turns.
Change `POS_MIN`/`POS_MAX` to the real travel of your mechanism.

Then:

1. Copy `config.h.example` to `config.h`.
2. Fill in your WiFi credentials, MQTT broker, and topics.
3. Flash the board and open the Serial Monitor at **115200** baud.

Drive it from any MQTT client, e.g. the mosquitto CLI:

```
mosquitto_pub -h broker.hivemq.com -t "fluidwire/demo/stepper/cmd" -m "home"
mosquitto_pub -h broker.hivemq.com -t "fluidwire/demo/stepper/cmd" -m "1600"
mosquitto_pub -h broker.hivemq.com -t "fluidwire/demo/stepper/cmd" -m "stop"
mosquitto_sub -h broker.hivemq.com -t "fluidwire/demo/stepper/#"
```

Send `home` first after every power-up: the firmware has no idea where the axis is
until it touches the limit switch.

## Expected serial output

```
Stepper ready -- send 'home' before absolute moves
WiFi: connecting to your-wifi-ssid .... OK, ip=192.168.1.42
MQTT: connecting to broker.hivemq.com:1883 as fluidwire-esp32-stepper ... OK
Subscribed to fluidwire/demo/stepper/cmd
Position -> 0 steps
Status -> idle
Cmd on fluidwire/demo/stepper/cmd: 'home'
Homing: seeking limit switch
Status -> homing
Homing: switch hit, backing off
Homing: done, position zeroed
Position -> 0 steps
Status -> idle
Cmd on fluidwire/demo/stepper/cmd: '1600'
Moving to 1600 steps
Status -> moving
Position -> 412 steps
Position -> 1600 steps
Status -> idle
Cmd on fluidwire/demo/stepper/cmd: '99999'
Rejected 99999 -- allowed range is 0..6400 steps
Status -> error
```

## Troubleshooting

- **Motor doesn't move at all.** Check that EN is wired (or tied LOW) and that the
  12 V supply is on. Check the coil wiring — the two coils must go to separate
  driver output pairs.
- **Motor buzzes or skips steps.** Lower `MAX_SPEED` and `ACCELERATION`, or raise
  the driver current limit slightly. Steppers lose torque quickly as speed rises.
- **Homing drives away from the switch.** Swap one coil's wires, or invert the
  direction by changing the sign of `HOMING_SPEED` in `startHoming()`.
- **Homing times out.** The switch isn't reaching GND when pressed, or it's on the
  wrong side of the travel. Check `LIMIT_PIN` wiring.
- **Driver gets very hot.** Current limit too high, or no heatsink. Add a heatsink
  and/or a fan, and reduce Vref.
- **`MQTT: ... FAIL rc=-2`.** Can't reach the broker. Check the host/port and that
  your network allows outbound port 1883.
- **Command ignored.** The payload must be a bare integer, `home` or `stop` — not
  `1600 steps` or `{"pos":1600}`.

## Security note

This sample uses a plaintext MQTT connection (port 1883) on a public test broker so
it runs out of the box — fine for a demo, but **anyone** can command a public topic,
and this one moves physical hardware. For anything real:

- Use a private broker with username/password auth (fill `MQTT_USER` /
  `MQTT_PASSWORD`).
- Use TLS — see the companion sample
  [`esp32-mqtt-tls-secure`](https://github.com/fluidwire/esp32-mqtt-tls-secure).
- Choose hard-to-guess topic names, and never expose an actuator that can pinch,
  crush or jam something to an unauthenticated public broker.
- Keep `POS_MIN`/`POS_MAX` tight and add a second physical end-stop, so that even a
  malicious or buggy command can't drive the mechanism into its frame.
- Keep your WiFi and broker credentials in `config.h`, which is git-ignored.

## License

MIT — see [LICENSE](LICENSE). Use it, fork it, ship it.

## Got stuck?

We build IoT and embedded hardware for a living. If you're prototyping something
and hit a wall, reach out at [fluidwire.com](https://fluidwire.com/).
