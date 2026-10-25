# Building noise monitor and breaker-box controller

The first ESP32 measures sound outside a building, gives immediate feedback on
eight RGB LEDs, and publishes numeric sound levels and sensor health to the
MQTT broker. Telegraf stores these messages in the local Docker InfluxDB 2
bucket; Grafana presents the trends and state changes. No audio is uploaded.
The second ESP32 adds an SG90 button presser triggered by sustained noise and an
independent ultrasonic door monitor. See [second-board wiring, upload and operation](docs/SECOND_BOARD.md).
Use the `actuator_board` environment for that board; `upesy_wrover` remains the microphone.

Local communication is a Wi-Fi web page/API that adjusts noise thresholds and
the calibration offset. Changes affect actual embedded control and telemetry,
and survive a restart.

| State | Trigger on rising sound | All eight LEDs |
| --- | --- | --- |
| Within configured range | Below 80 dB (e.g. 65 dB) | Green |
| Near limit | At least 80, below 95 dB | Orange |
| Over limit | At least 95 dB | Flashing red: 250 ms on, 250 ms off |
| Invalid/no varying microphone data | No valid estimate | Purple |

This uses your requested 15 dB spacing: 65 dB is a green example, 80 dB triggers
orange, 95 dB triggers red. Green covers quieter levels too. A 2 dB reset
hysteresis prevents rapid state changes: red clears below 93 dB, orange below 78 dB.
All eight LEDs flash together continuously while the red state is active.
The flash uses a non-blocking timer so microphone sensing and networking continue.
Adjust `Config::RED_FLASH_HALF_PERIOD_MS` to change the flash speed.
These are configurable project thresholds, not a hearing-safety classification.

## Assignment project layout

| Path | Assignment component |
| --- | --- |
| `src/main.cpp` | Microphone ESP32 firmware: sound processing, LEDs and MQTT |
| `src/actuator_main.cpp` | Actuator ESP32 firmware: ultrasonic door sensing, servo and MQTT |
| `include/` | Shared configuration, control logic and local web pages |
| `platformio.ini` | Build/upload environments for both ESP32s |
| `cloud/` | Docker services, Telegraf configuration and Grafana dashboards |
| `tools/mock_microphone/` | Browser interface for testing the actuator without the microphone board |
| `tools/` | Local setup and end-to-end pipeline checks |
| `tests/` | Control-logic tests and telemetry fixtures/parser checks |
| `docs/SECOND_BOARD.md` | Actuator wiring, configuration and demonstration guide |
| `output/visuals/` | Assignment wiring and system diagrams |

The project contains the assignment only. PlatformIO regenerates `.pio/` when
building. Local credentials stay in ignored `include/secrets.h` and `cloud/.env`.
Open **http://localhost:8765** for the mock microphone interface; see the
[actuator guide](docs/SECOND_BOARD.md#test-without-the-microphone-board) for testing steps.

## Breadboard wiring

Unplug USB before wiring. Use **printed GPIO/pin labels**, not physical header
positions: ESP32 dev-board layouts differ. Place the board so its two header
rows occupy separate breadboard strips. If it fills the breadboard width, use
jumper leads or a second breadboard rather than shorting pins.

Make three **separate** rails: **3.3 V**, **5 V**, and **GND**. Never join the
positive rails. Some rails are split in the middle; bridge only matching rail
sections where needed. Both modules share ESP32 ground.

| INMP441 pin | ESP32 connection | Purpose |
| --- | --- | --- |
| VDD / VCC | **3V3** rail | Mic supply; never 5 V |
| GND | GND rail | Shared reference |
| SCK / BCLK | **GPIO26** | I2S bit clock from ESP32 |
| WS / LRCLK | **GPIO25** | I2S channel clock from ESP32 |
| SD / DOUT | **GPIO33** | Digital audio into ESP32 |
| L/R | **GND** rail | Left channel, matching firmware |

| Freenove **IN** connector pin | Connection |
| --- | --- |
| V | ESP32 board **5V / USB-powered VBUS** rail |
| G | GND rail |
| S | **GPIO2 → 220 Ω resistor → IN S** |
| OUT connector | Leave unconnected |

Use a board pin documented as USB-powered 5V/VBUS; do not assume every pin
labelled VIN supplies power. Otherwise use a regulated 5 V panel supply with
its ground connected to ESP32 ground. Do not connect an external 5 V source
into the ESP32's USB supply simultaneously. LED brightness is 10/255.

**Your resistors:** use one **220 Ω** resistor in series with the LED data wire
(either direction). Neither **1 kΩ** nor **10 kΩ** is required for this wiring.
Do not put resistors in the power rails or a resistor divider on the LED data.
GPIO33's internal pull-down is enabled. The INMP441 datasheet's optional
external SD pull-down is 100 kΩ, which is not in your kit. Keep mic wires short
and its sound hole clear.

The Freenove module specifies a 3.5–5.5 V supply, so power it from 5 V. Direct
3.3 V ESP32 data often works on a short bench connection, but a **74AHCT125
powered at 5 V** provides reliable signal conversion if needed. Resistors cannot
boost 3.3 V logic to 5 V. With that part: GPIO2 → selected channel input, its
output → 220 Ω → IN S; ground that channel's active-low OE; add 100 nF supply
decoupling. Do not feed 5 V into any ESP32 GPIO.

GPIO2 matches the supplied example but is a boot-strapping pin. If the panel
prevents boot/upload, disconnect IN S while uploading, or move it to GPIO27 and
change `Config::LED_PIN` in [noise_config.h](include/noise_config.h).

The eight LEDs, GRB ordering and RMT control follow the supplied PDF and
[Freenove library](https://github.com/Freenove/Freenove_WS2812_Lib_for_ESP32).
Supply and IN/OUT markings are in
[Freenove Chapter 6](https://docs.freenove.com/projects/fnk0047/en/latest/fnk0047/codes/C/6_LEDPixel.html).

## Firmware setup

The project retains `espressif32@7.0.1`, `upesy_wrover`, Arduino ESP32 2.0.17.
Freenove v1.0.5 is pinned for Arduino 2.x compatibility. MQTT/JSON use
PubSubClient 2.8 and ArduinoJson 7.2.0.

Your existing ignored `include/secrets.h` is reused. On a new installation,
copy `include/secrets.h.example` and set `LAB_WIFI_SSID`, `LAB_WIFI_PASSWORD`
and `LAB_GROUP_ID`. Use 2.4 GHz Wi-Fi and a unique group identifier without
MQTT `/`, `+` or `#`. Do not overwrite an existing secrets file.

```sh
pio run -e upesy_wrover
pio run -e upesy_wrover -t upload
pio device monitor -b 115200
```

If `pio` is not on PATH, use `~/.platformio/penv/bin/pio`. Close the serial
monitor before uploading. Serial prints the exact topic and
`Local page: http://<board-IP>/` after Wi-Fi connects.

Open that address on a phone/laptop on the **same Wi-Fi network**. Adjust
thresholds/offset and wait for **Settings applied and saved**. The page shows
live SPL, state, clipping and MQTT health. Networks with client isolation may
block this; use a suitable lab network/hotspot. The board does not create an AP.

```sh
curl http://<board-IP>/api/status
curl -X POST http://<board-IP>/api/config \
  -H 'Content-Type: application/json' \
  -d '{"warning_db":80,"limit_db":95,"offset_db":0,"calibrated":false}'
```

POST requires finite numbers, red > orange + 2 dB, thresholds within 30–120 dB,
and offset within −30..30 dB. HTTP 202 means queued; the following status
confirms saved settings. NVS stores one blob to prevent mixed threshold pairs
on a power cut. Configuration access assumes a trusted demonstration LAN.

## Required global cloud chain

**ESP32 → Wi-Fi/Internet → MQTT → Telegraf → InfluxDB → Grafana**

The MQTT broker is `broker.emqx.io:1883`, user `emqx`, password `public`.
InfluxDB 2, Telegraf and Grafana run locally in Docker. The ESP32s continue
using the existing MQTT broker; they do not make InfluxDB HTTP writes or need
the database token. New telemetry goes to the local database. Existing cloud
history is not copied.

```sh
# Generates local credentials; backs up an existing cloud configuration.
# Safe to repeat: an already configured local .env is preserved.
python3 tools/configure_cloud.py --local
# Start Docker Desktop, then:
docker compose --env-file cloud/.env -f cloud/docker-compose.yml up -d
docker compose --env-file cloud/.env -f cloud/docker-compose.yml ps
docker compose --env-file cloud/.env -f cloud/docker-compose.yml logs --tail=30 telegraf
# Verify both telemetry paths using isolated synthetic devices:
python3 tools/check_pipeline.py
python3 tools/check_cabinet_pipeline.py
```

Open **http://localhost:8086** for InfluxDB. Its username and password are
`INFLUX_ADMIN_USER` and `INFLUX_ADMIN_PASSWORD` in the ignored `cloud/.env`.
Host scripts use `http://localhost:8086`; containers use `http://influxdb:8086`.
Both HTTP interfaces bind to localhost. Database data and configuration are
stored in named Docker volumes and survive restarts and `docker compose down`.
Use the same Compose flags as above to stop the stack with `down`; avoid
`down -v` unless you intend to delete the saved database and Grafana data.

For manual setup use `cloud/.env.example`. The generated local setup token is
used by Telegraf and Grafana for development. Initial InfluxDB credentials are
applied only to an empty volume; editing `.env` does not reset an existing
InfluxDB account. Previous configurations are saved in ignored
`cloud/.env.*.backup` files; `include/secrets.h` is preserved.

Open **http://localhost:3000/d/building-noise**. User: `admin`; password:
`GRAFANA_ADMIN_PASSWORD` in your local `cloud/.env`. The Flux data source and
dashboard are automatically provisioned. Select your group/mic. Grafana shows
latest level, LED state, sensor health, clipping, sound/threshold trends, state
history and Wi-Fi signal. Current-value panels use only the last 15 seconds;
a stopped data path eventually shows **No data**. Grafana binds to this
computer's localhost; the ESP32 local page is separate.

Open MQTT Explorer and create a connection named `Building noise`:
protocol `mqtt://`, host `broker.emqx.io`, port `1883`, username `emqx`,
password `public`, encryption/TLS **off**. In Advanced settings, replace the
default broad `#` subscription with `iot2026/harlen-group01/noise/#` for this
workspace (use your own `LAB_GROUP_ID` if changed). Keep a unique client ID;
do not copy the ESP32 or Telegraf client ID. Save and connect.

Expand the topic tree to your `noise-mic-*` device, then select `telemetry`.
Watch `estimated_spl_db`, `state`, `sensor_ok`, `sequence`, `warning_db` and
`limit_db` update. `dbfs` is the raw digital level, not the corrected SPL.
MQTT Explorer observes the data; control thresholds through the ESP32 local
page. This firmware does not subscribe to MQTT control commands.

To inspect storage independently, open **http://localhost:8086**, log in using
your local InfluxDB credentials, open Data Explorer, and select
`environment_lab`, measurement `noise_monitor`, location `building_exterior`,
field `estimated_spl_db`, and a recent time range. A Flux equivalent is:

```flux
from(bucket: "environment_lab")
  |> range(start: -15m)
  |> filter(fn: (r) => r._measurement == "noise_monitor")
  |> filter(fn: (r) => r.location == "building_exterior")
  |> filter(fn: (r) => r._field == "estimated_spl_db")
```

InfluxDB, Telegraf and Grafana run **on this computer in Docker**. The EMQX
MQTT broker is remote; the database bucket is local.
Keep this computer, Docker Desktop and Telegraf running for fresh database
ingestion. Previously stored data remains in InfluxDB when this computer stops.

| Topic under `iot2026/<group>/noise/<device>/` | Purpose | Retained? |
| --- | --- | --- |
| `telemetry` | Telegraf ingestion and board 2 live noise subscription | No |
| `status` | Retained snapshot for viewers | Yes |
| `availability` | `online`, MQTT last will `offline` | Yes |

Device ID is `noise-mic-` plus ESP32 station MAC. Messages include
`estimated_spl_db`, `dbfs`, `state`, `state_code`, `sensor_ok`, `clipped`, actual
thresholds, offset, calibration flag, sequence, boot ID, sample age, uptime and
RSSI. Group/device/location are tags. Fault messages **omit** level fields
while preserving state/health; no zero-dB value is invented. Telegraf's
`json_v2` parser preserves numeric, boolean and string types. It consumes only
unretained telemetry so old retained status cannot become a fresh DB reading.

Telemetry is approximately 1 Hz plus state/config changes. A separate network
task keeps audio/control running during TCP/MQTT stalls. Reconnects are
automatic. The latest reading replaces older samples: **outages create gaps;
there is no offline history replay**. PubSubClient publishes at QoS 0, so loss
is possible. An ESP32 publish success is not proof of storage; check the full
chain. Telegraf timestamps at receipt, not capture; age/sequence expose
freshness. This is not a lossless recorder.

The current MQTT broker is public and unencrypted. A real deployment should use a
private authenticated TLS broker and protected local configuration. Board 2 checks fresh telemetry, sequence/boot updates and connection state; it
does not use retained status for automatic triggering. See [its guide](docs/SECOND_BOARD.md).

## Measurement and calibration

Signed 24-bit I2S audio occupies the high bits of 32-bit words. Each 8,000-sample
(500 ms) window removes DC power and computes RMS power relative to a
full-scale sine wave.

`estimated_spl_db = dbfs + 120 + offset_db - SPL_REDUCTION_DB`

`Config::SPL_REDUCTION_DB` in `include/noise_config.h` defaults to **15 dB**.
This correction applies to the final estimate used by the LEDs, local page,
serial output and MQTT/cloud data. It is separate from the saved calibration
offset, so existing saved settings still receive the 15 dB reduction. Raw
`dbfs` remains unchanged.

The +120 constant follows nominal −26 dBFS sensitivity at 94 dB SPL in the
[TDK datasheet](https://product.tdk.cn/system/files/dam/doc/product/sw_piezo/mic/mems-mic/data_sheet/inmp441.pdf).
These are **estimated, unweighted SPL** readings, not dBA, exposure dose or
certified compliance measurements. 16 kHz sampling limits usable bandwidth to
below 8 kHz. Outdoors, wind/enclosures affect readings; use a suitable
windscreen and protect the electronics from rain.

Set offset to zero, compare steady sound with a reference meter beside the mic,
then enter `reference dB − displayed dB`. Tick the reference checkbox only after
performing that comparison. One offset does not provide A-weighting or correct
all frequency response differences. Digital clipping is flagged at 99% FS.

## Tests and physical demonstration

```sh
c++ -std=c++11 -Wall -Wextra -Werror -Iinclude tests/noise_logic_test.cpp -o /tmp/noise_logic_test
/tmp/noise_logic_test
python3 tests/test_cloud_schema.py /path/to/telegraf
# Requires running Telegraf and cloud/.env; writes 4 labelled synthetic points:
python3 tools/check_pipeline.py
```

The pipeline check publishes safe/warning/high/fault diagnostic messages and
queries InfluxDB to confirm Telegraf storage. They use `location=software_test`
and a unique `software-validation-*` device, excluded by the real-building
dashboard. These verify software integration, not microphone measurements.

Physical demonstration:

1. Wire/upload the ESP32. Show readings responding to speech and normal green
   LEDs. Explain what is measured and the accuracy limitations.
2. Use the local page to temporarily set thresholds around the observed room
   readings. Demonstrate green/orange/red using ordinary sounds or changed
   thresholds; no need to generate 95 dB in the room. Show hysteresis.
3. Show MQTT Explorer subscribed to `iot2026/<group>/noise/#`, the state change
   in JSON, running Telegraf, a `noise_monitor` Influx query, then Grafana.
4. Restart and show saved settings. Block Internet/broker access while retaining
   the LAN: mic/LEDs and local configuration continue, cloud data stops, then
   reconnect and show recovery.
5. With USB unplugged, disconnect the mic's SD or power, then power up to show
   purple/fault telemetry rather than a false green reading.
6. Restore orange **80 dB** and red **95 dB** after testing.

For the ≤5-minute group video: problem/value 40 s, architecture/choices 50 s,
mic/LED/local interaction 70 s, complete cloud chain 80 s, reliability/roles
60 s. Record actual prototype results; synthetic tests cannot replace the
ESP32 demonstration.

Before submission confirm your assigned domain:
`(last-two-digits student A + last-two-digits student B) % 5`.
Building comfort fits index 1; other allocations need domain-specific scope or
teaching-team approval. Student IDs were not supplied for checking this.

Proposed responsibility split — replace with actual names and work performed:

| Student | Primary technical responsibility | Shared responsibility |
| --- | --- | --- |
| A | Microphone DSP, LED control, calibration and embedded logic | Integration, interfaces, fault tests |
| B | MQTT, Telegraf/Influx schema, Grafana and local connectivity | Integration, interfaces, fault tests |

Individual videos should explain actual owned code, calculations/protocols,
design choices, testing and troubleshooting in the full-system context.
Include the second-board servo logic, door filtering, MQTT subscription and dashboard work in the actual responsibility declaration.
The [actuator individual video script](docs/INDIVIDUAL_VIDEO_SCRIPT.md) provides
sentence-by-sentence filming cues and exact code line numbers.

## Troubleshooting

- Purple/no varying audio: check mic 3V3, SD, GND, L/R=GND and GPIO assignments.
- Dark/flickering LEDs: check **IN**, 5 V, common GND, GPIO2 and series resistor;
  shorten wires or add level conversion. Wrong colours suggest byte order;
  this module uses `TYPE_GRB`.
- Older firmware printed `LED RMT write failed` on successful writes: Freenove
  v1.0.5 returns the RMT boolean result through an `esp_err_t` signature.
  The check is corrected; re-upload the current firmware to remove the false warning.
- No local page: check serial IP, same LAN, client isolation and Wi-Fi settings.
  Broker retries can briefly delay HTTP responses; audio is independent.
- MQTT data but no Influx points: check Telegraf group/topic, logs, endpoint,
  token and bucket. `/status` deliberately is not ingested.
- Grafana no data: check read permission, group/device, range and actual
  `location=building_exterior` samples. Software diagnostics are excluded.
