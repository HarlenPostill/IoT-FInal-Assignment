# ESP32 #2: SG90 button presser and independent door monitor

This target is `actuator_board`, implemented in `src/actuator_main.cpp`. The
microphone remains the `upesy_wrover` target. Both use the same ignored
`include/secrets.h` Wi-Fi/group settings and MQTT broker. Keep Telegraf and
Grafana running on the computer for local database storage and display.

## Behaviour

| Function | Initial configuration |
| --- | --- |
| Servo noise trigger | At least 90 dB for 3 seconds of successive fresh MQTT readings |
| Default motion | Rest 110° → press 70° over 0.5 s → hold 0.5 s → return 110° over 0.5 s (40° each way) |
| Repeat protection | One press per noise episode; after returning, 3 seconds below 88 dB rearms |
| Door | Open above 20 cm; an open door closes at or below 18 cm |
| Door filtering | Read every 100 ms, median of 5 readings, stable candidate for 600 ms |
| Missing ultrasonic echo | Unknown, distance omitted; never interpret no echo as closed |
| Missing noise | Pending qualification cancelled after a >2.5 s receive gap or connection loss |

The door function **does not enable, block, or trigger the servo**. It only
reports distance, door state and sensor health. A fixed sensor aimed at a fixed
wall cannot detect a moving door: mount the sensor on the moving door facing
that wall, or mount it in the box facing the door's inside surface. Closed
must produce a valid distance below the configured threshold; open must produce
another valid echo above it. If the opened door moves out of the beam entirely,
state becomes unknown. Check this geometry with the local page before mounting.

The servo receives the microphone's **already corrected** `estimated_spl_db`
(including its 15 dB subtraction). It does not subtract it again. Its default
90 dB trigger is separate from the microphone's 95 dB flashing-red threshold.

The decision uses live MQTT telemetry at the server/broker, **not an InfluxDB
or Grafana query**. Telegraf stores the same microphone stream independently.
It subscribes to exactly:

```
iot2026/harlen-group01/noise/noise-mic-F42DC976B9E4/telemetry
```

`CabinetConfig::SOURCE_DEVICE` in `include/cabinet_config.h` identifies your
existing microphone. If you replace that board, copy the new microphone device
ID from serial/MQTT and rebuild this target. Group/topic/device/location,
sensor health, numeric level, sample age, boot ID and increasing sequence are
checked. Duplicate/backwards messages do not advance the timer. A new boot,
invalid reading, broken connection or queue overflow cancels qualification.
The subscriber uses live `/telemetry`, not retained `/status`. A lone retained
or duplicate reading cannot satisfy the sustained trigger.

Qualification represents successive approximately 1 Hz observations, not
proof that every instant between them exceeded the threshold. An already
started servo action finishes returning even if Internet access fails. The
latch survives a connection outage, but resets on a board reboot; sustained
fresh noise after a reboot can trigger another press. Press count also resets
on reboot. There is no contact sensor to prove that the physical button was hit.

## Wiring the second breadboard

These connections assume a **four-pin HC-SR04 ultrasonic sensor**. The SG90 is
confirmed; check that the sensor is labelled VCC/TRIG/ECHO/GND. Use printed
GPIO labels rather than header positions. Unplug USB and the external supply
before changing wires. Build this around a mock button and a non-electrical
box; this firmware is not a protection system for a real mains breaker.

Power the ESP32 through its USB connector. Use a regulated **5 V external
supply with around 2 A capacity** for the servo and sensor. Servo current surges
can reset an ESP32 if powered through its 3V3 regulator or an inadequate USB
rail. Join the external supply's **negative** to ESP32 GND. Leave the external
5 V positive separate from the ESP32 USB/VIN/3V3 rails in this wiring plan.
A breadboard supply module still needs an input adapter and enough current;
a 9 V rectangular battery is unsuitable for the servo. No power resistor is
needed for the SG90.

| Component pin / wire | Connection |
| --- | --- |
| SG90 orange/yellow signal | ESP32 GPIO18 directly |
| SG90 red (+) | External regulated +5 V |
| SG90 brown/black (−) | Common GND |
| HC-SR04 VCC | External regulated +5 V |
| HC-SR04 GND | Common GND |
| HC-SR04 TRIG | ESP32 GPIO26 directly |
| HC-SR04 ECHO | Voltage divider below, then GPIO27 |
| ESP32 GND | External supply negative / breadboard GND rail |
| ESP32 USB | Computer or appropriate USB power source |

**Do not connect the 5 V ECHO signal directly to the ESP32.** Using your
available resistor values, make an 11 kΩ upper leg and a 20 kΩ lower leg:

```
HC-SR04 ECHO ──[10 kΩ]──[1 kΩ]──●── ESP32 GPIO27
                                │
                             [10 kΩ]
                                │
                             [10 kΩ]
                                │
                              GND
```

This uses **three 10 kΩ resistors and one 1 kΩ resistor**. The junction ● must
share one breadboard node with GPIO27. Resistors on either side of a node must
span different connected rows; putting both legs in the same five-hole strip
shorts the resistor. The divider gives about **3.23 V** at GPIO27 for 5 V ECHO.
220 Ω resistors are not needed on this board. If you do not have enough of the
listed resistors, do not omit the divider—use an appropriate level shifter or
another correctly calculated divider.

Some breadboard power rails are split halfway. Bridge only matching +5 V
sections and matching ground sections. Confirm with labels/metre; never bridge
+5 V to 3V3. Use short common-ground wires. A 470–1000 µF capacitor across
servo +5 V/GND near the servo can help with surges if available, observing
polarity; it does not replace an adequate supply.

SG90 command angles are configurable from 0–180° but actual range varies by
servo. The firmware uses conservative 50 Hz, 1000–2000 µs PWM. Therefore
**40° of command change is a starting point, not a calibrated mechanical
measurement**. Check unloaded travel with the horn removed, then set rest and
press endpoints to fit your button without stalling. Reverse direction by
swapping endpoints, for example rest 70° / press 110°. Existing boards keep
their saved angles after a firmware upload: set rest to 110° and press to 70°
on the local configuration page and save to apply this motion. If travel needs pulse
calibration, edit `SERVO_MIN_US` / `SERVO_MAX_US` in `cabinet_config.h` cautiously
within the actual servo's specification, then rebuild. Do not force the horn
against an end stop.

## Upload and local configuration

1. Disconnect the microphone ESP32 USB so the upload cannot select it.
2. Connect **only the second ESP32** by USB. In PlatformIO select
   **Project Tasks → actuator_board → Upload**. The project default is still
   the microphone; the generic default Upload task will install microphone
   firmware instead. Alternatively use the explicit commands below.
3. Open the second board's serial monitor at **115200 baud**, then press EN.
   Copy `Cabinet control page: http://<board-ip>/` into a browser on the same
   LAN. This IP is separate from the microphone's and may change after restart.
4. Initially leave the servo unloaded. Use **Test one button press**, check
   direction and return, adjust endpoints/time, then install the horn.
5. On the page set noise trigger, sustained duration, rest angle, press angle,
   travel time, hold time and door threshold. A save is confirmed only after
   the board acknowledges persistence. Saves and test presses are rejected
   while a servo action is in progress. Restart to verify saved settings.
6. Reconnect/power the microphone board so it publishes live readings.

```sh
# If pio is not on PATH, use ~/.platformio/penv/bin/pio
pio device list
pio run -e actuator_board
# Substitute the actual SECOND board's serial device, not the literal example:
pio run -e actuator_board -t upload --upload-port /dev/cu.YOUR_SECOND_BOARD
pio device monitor -b 115200 -p /dev/cu.YOUR_SECOND_BOARD
```

The local page uses `GET /api/status`, `POST /api/config` and deliberate
`POST /api/press`. Its LAN control remains usable if the broker is unavailable
(subject to brief TCP retry delays). The local API and the public lab broker
are unauthenticated: anyone with access can affect this bench prototype.
Use an authenticated private broker and protected local control for any actual
access-control or electrical application.

## MQTT Explorer, InfluxDB and Grafana

Use the same MQTT Explorer connection: `mqtt://broker.emqx.io`, TCP **1883**,
username **emqx**, password **public**, TLS off; use a unique client ID.
Subscribe to `iot2026/harlen-group01/#` to see both boards, or
`iot2026/harlen-group01/cabinet/#` for the second board only.

The second board prints its MAC-based device ID, such as `cabinet-XXXXXXXXXXXX`.
Its topics are:

```
iot2026/<group>/cabinet/<device>/telemetry     unretained, about 1 Hz + state changes
iot2026/<group>/cabinet/<device>/status        retained current snapshot
iot2026/<group>/cabinet/<device>/availability retained online / offline LWT
```

Door fields: `distance_cm` when valid, `door_state` open/closed/unknown,
`door_state_code` 1/0/2, `sensor_ok`, configured threshold/hysteresis.
Servo/noise fields: phase/code, command angle, boot-local press count, armed,
noise freshness, last accepted source sequence, trigger/rearm threshold,
sustained qualification milliseconds, endpoint/timing settings. Unavailable
noise/distance values are omitted, not replaced by zero. No audio is sent.

`cloud/telegraf.conf` now has two consumers: the existing `noise_monitor` and
**`cabinet_monitor`** measurement. Both write the local InfluxDB bucket,
using different MQTT client IDs. Influx uses Telegraf receipt timestamps.
Refresh Grafana to see **IoT project → Breaker box door and button controller**:

**http://localhost:3000/d/breaker-box**

There is also a link between the existing noise dashboard and the new one.
Panels show current door state, distance, microphone freshness, press count,
distance/threshold trend, door history, received-noise/trigger trend and servo
activity history. Stat panels use only the last 15 seconds, then show No data
rather than an indefinitely old state. A remaining recent distance can be the
last valid echo during a sensor fault; door state/health are authoritative.
Telegraf flushes every 5 seconds and Grafana refreshes every 5 seconds, so allow
roughly 10 seconds for a new event to appear. No second-board hardware data
appears until this firmware is uploaded and running.

If bringing the stack up later:

```sh
docker compose --env-file cloud/.env -f cloud/docker-compose.yml up -d
# After changing Telegraf schema:
docker compose --env-file cloud/.env -f cloud/docker-compose.yml restart telegraf
docker compose --env-file cloud/.env -f cloud/docker-compose.yml logs --tail 30 telegraf
```

Influx Data Explorer example:

```flux
from(bucket: "environment_lab")
  |> range(start: -15m)
  |> filter(fn: (r) => r._measurement == "cabinet_monitor" and r.location == "breaker_box")
  |> filter(fn: (r) => r._field == "distance_cm" or r._field == "door_state_code" or r._field == "press_count")
```

## Test without the microphone board

Open **http://localhost:8765** for the mock microphone interface. It starts with
publishing stopped. If needed, launch it with:

```sh
docker compose --env-file cloud/.env -f cloud/docker-compose.yml up -d mock-microphone
```

Keep the real microphone off while the mock is running. Select 65 dB and Start
for a fresh reading every second. Select 100 dB to test the default sustained
noise trigger. After the servo returns, select 65 dB for at least 4 seconds to
rearm, then try high noise again. Stop tests the stale-reading path; Sensor
fault tests invalid readings. The page shows the actuator's real MQTT feedback,
including freshness, servo phase, armed status and press count.

The mock reads the source device and broker from the firmware configuration
headers, and the group from Compose settings. It publishes unretained telemetry
with a fresh 16-character boot ID, increasing sequence and zero sample age.
Mock payloads include `simulated: true` and firmware `mock-interface-1.0`, but
use the real source identity and location required by the actuator. Consequently
they also appear in the normal Grafana noise dashboard and local database.
The stream continues with the tab closed; press Stop before using the real
microphone. Restarting the container always starts with publishing stopped.

## Demonstration and verification

1. Show local distance with a flat object about 10 cm away: closed. Move it to
   30 cm: open after filtering. Show the matching MQTT JSON and Grafana change.
   Keep this target aimed so it gives a valid echo in both positions.
2. Change door threshold locally and show the changed decision and cloud value.
   This must not move the servo.
3. Run one local servo test and observe the button press and return. Show
   press count and the phase history in MQTT/Grafana.
4. For the integrated noise test, temporarily set the **second-board trigger**
   to a value a little above ordinary room readings. A brief clap should not
   press; sustained moderate sound above that test threshold should press once
   and return. Keep the sound high: no repeat. Drop it below trigger − 2 dB
   for 3 seconds after return, then demonstrate a second episode. Restore
   **90 dB / 3 seconds** afterwards. Do not deliberately create 90 dB noise.
5. Stop the microphone or disconnect Internet: source becomes stale and
   pending qualification cancels; door sensing/local control still work.
   Restore connection and demonstrate recovery using new readings.
6. Check for a fault: with power disconnected remove sensor power/ECHO, then
   restart. Door shows unknown and distance is absent. Restore correct wiring.

Software checks (separate from physical demonstration):

```sh
pio run -e upesy_wrover -e actuator_board
c++ -std=c++11 -Wall -Wextra -Werror -Iinclude tests/cabinet_logic_test.cpp -o /tmp/cabinet_logic_test
/tmp/cabinet_logic_test
python3 tests/test_cloud_schema.py /path/to/telegraf
python3 tools/check_cabinet_pipeline.py
```

The cloud smoke test publishes three clearly labelled synthetic cabinet states
under a unique device with `location=software_test`, excluded from the real
Grafana dashboards. It does not publish to the microphone source topic or
command a servo. Physical servo travel, power stability, real echoes and
button-contact accuracy still require your breadboard demonstration.

## Hardware references

- [HC-SR04 datasheet](https://cdn.sparkfun.com/datasheets/Sensors/Proximity/HCSR04.pdf): 10 µs trigger, echo µs / 58 = cm, cycle >60 ms.
- [TowerPro SG90 manufacturer](https://towerpro.com.tw/product/sg90-7/): servo power and travel vary; use an adequate external supply.
- [Espressif GPIO voltage guidance](https://docs.espressif.com/projects/esp-faq/en/latest/hardware-related/hardware-design.html): ESP32 GPIO tolerance is 3.6 V; reduce a 5 V signal before connecting it.
