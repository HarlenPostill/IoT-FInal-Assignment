#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_system.h>
#include <freertos/event_groups.h>
#include "cabinet_config.h"
#include "cabinet_logic.h"
#include "cabinet_page.h"

namespace {

Cabinet::Settings settings;
Cabinet::NoiseGate gate;
Cabinet::ServoCycle servo;
Cabinet::DoorMonitor door;

Preferences preferences;
bool storageReady = false;
`
uint32_t sequence = 0, commandID = 0;
char commandResult[64] = "ready";

struct NoiseEvent {
  float db;
  uint32_t receivedAt, sequence;
  bool ok, reset;
};

struct Command {
  Cabinet::Settings settings;
  uint32_t id;
  bool manual;
};

struct Snapshot {
  Cabinet::Settings settings;
  float distance, noise, angle;
  uint32_t at, sequence, presses, qualifiedMs, sourceSequence, commandID;
  Cabinet::Door door;
  Cabinet::Phase phase;
  bool fresh, armed;
  char commandResult[64];
};

QueueHandle_t snapshots, noiseEvents, commands;
EventGroupHandle_t events;

constexpr EventBits_t SOURCE_INTERRUPTED = BIT0;
uint32_t sourceSequence = 0;

WiFiClient transport;
PubSubClient mqtt(transport);
WebServer server(80);
String telemetryTopic, statusTopic, availabilityTopic, sourceTopic;
char deviceID[48], bootID[17];
Cabinet::SequenceGuard sourceGuard;
bool haveSnapshot = false;
Snapshot latest{};
uint32_t nextCommandID = 0;

void writeServo(float degrees) {
  const float pulse =
      CabinetConfig::SERVO_MIN_US +
      degrees / 180.0f * (CabinetConfig::SERVO_MAX_US - CabinetConfig::SERVO_MIN_US);
  ledcWrite(CabinetConfig::PWM_CHANNEL,
            uint32_t(pulse * 65535.0f / 20000.0f + .5f)); // Pulse width to 16-bit duty at 50 Hz.
}

void loadSettings() {
  storageReady = preferences.begin("cabinet-v1", false); // Open persistent settings storage.
  if (!storageReady) {
    Serial.println("NVS unavailable; configuration saves disabled");
    return;
  }
  if (preferences.getBytesLength("settings") == sizeof(settings)) {
    Cabinet::Settings saved;
    preferences.getBytes("settings", &saved, sizeof(saved));
    if (Cabinet::valid(saved))
      settings = saved;
  }
}

void applyCommand() {
  Command cmd;
  if (xQueueReceive(commands, &cmd, 0) != pdTRUE)
    return;
  commandID = cmd.id;
  const char *result;
  if (servo.phase != Cabinet::Phase::Idle)
    result = "busy: wait for servo to return";
  else if (cmd.manual) {
    servo.start(millis(), settings);
    gate.latch();
    result = "test press started";
    Serial.println("Servo: local manual test press");
  } else if (!Cabinet::valid(cmd.settings))
    result = "invalid settings";

  else if (!storageReady ||
           preferences.putBytes("settings", &cmd.settings, sizeof(settings)) != sizeof(settings))
    result = "save failed: previous settings remain";
  else {
    settings = cmd.settings; // Apply only after saving the settings blob.
    servo.angle = settings.restAngle;

    gate.invalidate();
    door.reset();
    result = "saved";
    Serial.println("Cabinet settings saved");
  }
  snprintf(commandResult, sizeof(commandResult), "%s", result);
}

float readDistance() {
  digitalWrite(CabinetConfig::TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(CabinetConfig::TRIG_PIN, HIGH);
  delayMicroseconds(10); // Send the ultrasonic trigger pulse.
  digitalWrite(CabinetConfig::TRIG_PIN, LOW);
  const unsigned long duration =
      pulseIn(CabinetConfig::ECHO_PIN, HIGH, CabinetConfig::ECHO_TIMEOUT_US);
  return duration ? duration / 58.0f : NAN; // Echo time to cm; timeout means unknown.
}

void emitSnapshot() {
  Snapshot s{};
  s.settings = settings;
  s.distance = door.distance;
  s.noise = gate.db;
  s.angle = servo.angle;
  s.at = millis();
  s.sequence = ++sequence;
  s.presses = servo.pressCount;
  s.qualifiedMs = gate.qualifiedMs;
  s.sourceSequence = sourceSequence;
  s.commandID = commandID;
  s.door = door.state;
  s.phase = servo.phase;
  s.fresh = gate.fresh;
  s.armed = gate.armed;
  snprintf(s.commandResult, sizeof(s.commandResult), "%s", commandResult);
  xQueueOverwrite(snapshots, &s); // Keep only the newest snapshot.
}

void receiveNoise(char *topic, byte *payload, unsigned int length) {

  if (sourceTopic != topic || length > 1536)
    return;
  JsonDocument doc;
  NoiseEvent e{};
  e.receivedAt = millis();

  if (deserializeJson(doc, payload, length) || String(doc["group"] | "") != Config::GROUP_ID ||
      String(doc["device"] | "") != CabinetConfig::SOURCE_DEVICE ||
      String(doc["location"] | "") != "building_exterior" || !doc["sequence"].is<uint32_t>() ||
      !doc["boot_id"].is<const char *>()) {
    xEventGroupSetBits(events, SOURCE_INTERRUPTED);
    return;
  }

  const char *boot = doc["boot_id"];
  e.sequence = doc["sequence"];

  if (!sourceGuard.accept(boot, e.sequence, e.reset)) // Reject duplicate or older readings.
    return;

  e.ok = doc["sensor_ok"].is<bool>() && doc["sensor_ok"].as<bool>() &&
         doc["sample_age_ms"].is<uint32_t>() &&
         doc["sample_age_ms"].as<uint32_t>() <= Config::STALE_MS &&
         doc["estimated_spl_db"].is<float>();
  e.db = e.ok ? doc["estimated_spl_db"].as<float>() : NAN;
  e.ok = e.ok && isfinite(e.db) && e.db >= 0 && e.db <= 150; // Reject invalid sound levels.

  if (xQueueSend(noiseEvents, &e, 0) != pdTRUE)
    xEventGroupSetBits(events, SOURCE_INTERRUPTED);
}

void fillDocument(JsonDocument &doc) {
  const bool current = haveSnapshot && uint32_t(millis() - latest.at) < 2000;
  const bool fresh = current && latest.fresh;
  const bool distanceOk = current && isfinite(latest.distance);
  doc["group"] = Config::GROUP_ID;
  doc["device"] = deviceID;
  doc["location"] = CabinetConfig::LOCATION;
  doc["firmware"] = CabinetConfig::FIRMWARE;
  doc["boot_id"] = bootID;
  doc["source_device"] = CabinetConfig::SOURCE_DEVICE;

  doc["sensor_ok"] = distanceOk;
  doc["door_state"] = Cabinet::doorName(current ? latest.door : Cabinet::Door::Unknown);
  doc["door_state_code"] = uint8_t(current ? latest.door : Cabinet::Door::Unknown);
  if (distanceOk)
    doc["distance_cm"] = latest.distance;
  doc["door_threshold_cm"] = latest.settings.doorThresholdCm;
  doc["door_hysteresis_cm"] = CabinetConfig::DOOR_HYSTERESIS_CM;
  doc["noise_fresh"] = fresh;
  if (fresh)
    doc["noise_db"] = latest.noise;
  doc["source_sequence"] = latest.sourceSequence;
  doc["trigger_db"] = latest.settings.triggerDb;
  doc["rearm_db"] = latest.settings.triggerDb - 2;
  doc["sustain_ms"] = latest.settings.sustainMs;
  doc["qualified_ms"] = latest.qualifiedMs;
  doc["armed"] = latest.armed;
  doc["servo_phase"] = Cabinet::phaseName(latest.phase);
  doc["servo_phase_code"] = uint8_t(latest.phase);
  doc["servo_angle"] = latest.angle;
  doc["rest_angle"] = latest.settings.restAngle;
  doc["press_angle"] = latest.settings.pressAngle;
  doc["travel_ms"] = latest.settings.travelMs;
  doc["hold_ms"] = latest.settings.holdMs;
  doc["press_count"] = latest.presses;
  doc["sequence"] = latest.sequence;
  doc["uptime_ms"] = millis();
  doc["rssi_dbm"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
}

void reply(int status, const char *message) {
  JsonDocument doc;
  doc["message"] = message;
  String json;
  serializeJson(doc, json);
  server.send(status, "application/json", json);
}

void queueCommand(Command cmd) {
  cmd.id = ++nextCommandID;
  if (xQueueSend(commands, &cmd, 0) != pdTRUE) {
    reply(409, "Another command is pending");
    return;
  }
  JsonDocument doc;
  doc["command_id"] = cmd.id;
  doc["message"] = "queued";
  String json;
  serializeJson(doc, json);
  server.send(202, "application/json", json); // Queued; /api/status confirms completion.
}

void configureWeb() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", CABINET_PAGE); });

  server.on("/api/status", HTTP_GET, [] {
    JsonDocument doc;
    fillDocument(doc);
    doc["mqtt_connected"] = mqtt.connected();
    doc["storage_ready"] = storageReady;
    doc["command_id"] = latest.commandID;
    doc["command_result"] = latest.commandResult;
    doc["source_topic"] = sourceTopic;
    doc["telemetry_topic"] = telemetryTopic;
    String json;
    serializeJson(doc, json);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
  });

  server.on("/api/config", HTTP_POST, [] {
    if (server.arg("plain").length() > 512) {
      reply(413, "Request too large");
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) || !doc.is<JsonObject>() ||
        !doc["trigger_db"].is<float>() || !doc["rest_angle"].is<float>() ||
        !doc["press_angle"].is<float>() || !doc["door_threshold_cm"].is<float>() ||
        !doc["sustain_ms"].is<uint32_t>() || !doc["travel_ms"].is<uint32_t>() ||
        !doc["hold_ms"].is<uint32_t>()) {
      reply(400, "Expected all seven numeric configuration fields");
      return;
    }
    Command cmd{};
    cmd.settings.triggerDb = doc["trigger_db"];
    cmd.settings.restAngle = doc["rest_angle"];
    cmd.settings.pressAngle = doc["press_angle"];
    cmd.settings.doorThresholdCm = doc["door_threshold_cm"];
    cmd.settings.sustainMs = doc["sustain_ms"];
    cmd.settings.travelMs = doc["travel_ms"];
    cmd.settings.holdMs = doc["hold_ms"];
    if (!Cabinet::valid(cmd.settings)) {
      reply(400, "Invalid values; check the limits on the page");
      return;
    }
    if (!storageReady) {
      reply(503, "Settings storage unavailable");
      return;
    }
    queueCommand(cmd);
  });

  server.on("/api/press", HTTP_POST, [] {
    Command cmd{};
    cmd.manual = true;
    queueCommand(cmd);
  });
  server.onNotFound([] { reply(404, "Not found"); });
  server.begin();
}

bool publishSnapshot() {
  JsonDocument doc;
  fillDocument(doc);
  char json[1536];
  if (measureJson(doc) >= sizeof(json))
    return false;
  serializeJson(doc, json, sizeof(json));
  const bool telemetry =
      mqtt.publish(telemetryTopic.c_str(), json, false); // Unretained data for Telegraf; QoS 0.
  const bool status =
      mqtt.publish(statusTopic.c_str(), json, true); // Retain the latest status for viewers.
  return telemetry && status;
}

void networkTask(void *) {
  configureWeb();
  mqtt.setServer(Config::MQTT_HOST, Config::MQTT_PORT);
  mqtt.setCallback(receiveNoise);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(1);
  transport.setTimeout(1);
  const bool ready = mqtt.setBufferSize(2048);
  if (!ready)
    Serial.println("MQTT buffer allocation failed");
  const bool configured = Config::WIFI_SSID[0] && Config::GROUP_ID[0] &&
                          !strstr(Config::WIFI_SSID, "YOUR_") && !strstr(Config::GROUP_ID, "YOUR_");
  if (configured)
    WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);
  uint32_t wifiRetry = millis(), mqttRetry = millis() - 3000, publishedAt = 0;
  uint8_t publishedDoor = 255, publishedPhase = 255;
  bool wasWifi = false, wasMqtt = false, force = true;
  for (;;) {
    if (xQueueReceive(snapshots, &latest, 0) == pdTRUE)
      haveSnapshot = true;
    server.handleClient();
    const uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      wasWifi = false;
      if (mqtt.connected())
        mqtt.disconnect();
      if (configured && uint32_t(now - wifiRetry) >= 10000) {
        wifiRetry = now;
        WiFi.reconnect();
      }
    } else {
      if (!wasWifi) {
        Serial.printf("Cabinet control page: http://%s/\n", WiFi.localIP().toString().c_str());
        wasWifi = true;
      }
      if (ready && !mqtt.connected() && uint32_t(now - mqttRetry) >= 3000) {
        mqttRetry = now;

        if (mqtt.connect(deviceID, Config::MQTT_USER, Config::MQTT_PASSWORD,
                         availabilityTopic.c_str(), 1, true, "offline")) {

          if (!mqtt.subscribe(sourceTopic.c_str(), 0)) // Listen to live microphone telemetry.
            mqtt.disconnect();
          else {
            mqtt.publish(availabilityTopic.c_str(), "online", true);
            force = true;
            Serial.println("MQTT connected; listening for fresh microphone readings");
          }
        } else
          Serial.printf("MQTT connect failed, rc=%d\n", mqtt.state());
      }
      if (ready && mqtt.connected()) {

        mqtt.loop();
        if (xQueueReceive(snapshots, &latest, 0) == pdTRUE)
          haveSnapshot = true;

        if (haveSnapshot &&
            (force || uint32_t(millis() - publishedAt) >= 1000 ||
             publishedDoor != uint8_t(latest.door) || publishedPhase != uint8_t(latest.phase))) {
          if (publishSnapshot()) {
            force = false;
            publishedDoor = uint8_t(latest.door);
            publishedPhase = uint8_t(latest.phase);
          }
          publishedAt = millis();
        }
      }
    }

    if (wasMqtt && !mqtt.connected())
      xEventGroupSetBits(events, SOURCE_INTERRUPTED);
    wasMqtt = mqtt.connected();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
}

void setup() {
  Serial.begin(115200);
  loadSettings();

  snapshots = xQueueCreate(1, sizeof(Snapshot));
  noiseEvents = xQueueCreate(32, sizeof(NoiseEvent));
  commands = xQueueCreate(1, sizeof(Command));
  events = xEventGroupCreate();
  if (!snapshots || !noiseEvents || !commands || !events)
    abort();
  pinMode(CabinetConfig::TRIG_PIN, OUTPUT);
  digitalWrite(CabinetConfig::TRIG_PIN, LOW);
  pinMode(CabinetConfig::ECHO_PIN, INPUT);

  if (!ledcSetup(CabinetConfig::PWM_CHANNEL, 50, 16)) {
    Serial.println("Servo PWM setup failed");
    abort();
  }
  ledcAttachPin(CabinetConfig::SERVO_PIN, CabinetConfig::PWM_CHANNEL);
  servo.angle = settings.restAngle; // 110 deg default for button pressing :)
  writeServo(servo.angle);

  WiFi.mode(WIFI_STA);
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  snprintf(deviceID, sizeof(deviceID), "cabinet-%s", mac.c_str());
  snprintf(bootID, sizeof(bootID), "%08lx%08lx", (unsigned long)esp_random(),
           (unsigned long)esp_random());

  const String base = String("iot2026/") + Config::GROUP_ID + "/cabinet/" + deviceID;
  telemetryTopic = base + "/telemetry";
  statusTopic = base + "/status";
  availabilityTopic = base + "/availability";
  sourceTopic = String("iot2026/") + Config::GROUP_ID + "/noise/" + CabinetConfig::SOURCE_DEVICE +
                "/telemetry";
  Serial.printf("\nCabinet board %s\nMQTT: %s\nNoise source: %s\n", deviceID,
                telemetryTopic.c_str(), sourceTopic.c_str());
  emitSnapshot();

  if (xTaskCreatePinnedToCore(networkTask, "cabinet-network", 10240, nullptr, 1, nullptr, 0) !=
      pdPASS)
    Serial.println("Network task failed; local door sensing and servo return remain active");
}

void loop() {

  if (xEventGroupClearBits(events, SOURCE_INTERRUPTED) & SOURCE_INTERRUPTED) {
    xQueueReset(noiseEvents);
    gate.invalidate(); // Cancel qualification after a source interruption.
  }

  applyCommand();

  if (servo.phase != Cabinet::Phase::Idle)
    gate.latch();

  NoiseEvent e;
  while (xQueueReceive(noiseEvents, &e, 0) == pdTRUE) {
    const uint32_t now = millis();
    if (e.reset)
      gate.invalidate();
    sourceSequence = e.sequence;
    if (!e.ok || uint32_t(now - e.receivedAt) > Config::STALE_MS)
      gate.invalidate();
    else if (gate.sample(e.db, e.receivedAt, settings, CabinetConfig::NOISE_STALE_MS)) {
      if (servo.start(now, settings))
        Serial.printf("Servo: sustained %.1f dB -> press and return\n", e.db);
    }
  }

  gate.expire(millis(), CabinetConfig::NOISE_STALE_MS); // Noise becomes stale after a 2.5 s gap.
  servo.tick(millis()); // Advance the servo without a long motion delay.
  writeServo(servo.angle);

  static uint32_t sampledAt = millis() - CabinetConfig::SAMPLE_MS, snapshotAt = 0, printedAt = 0;
  if (uint32_t(millis() - sampledAt) >= CabinetConfig::SAMPLE_MS) {
    sampledAt = millis();
    door.sample(readDistance(), millis(), settings.doorThresholdCm,
                CabinetConfig::DOOR_HYSTERESIS_CM, CabinetConfig::DOOR_SETTLE_MS);
  }

  if (uint32_t(millis() - snapshotAt) >= 50) {
    snapshotAt = millis();
    emitSnapshot();
  }

  if (uint32_t(millis() - printedAt) >= 1000) {
    printedAt = millis();
    Serial.printf("Door %s | %.1f cm | noise %s %.1f dB | servo %s | presses %lu\n",
                  Cabinet::doorName(door.state), door.distance, gate.fresh ? "fresh" : "STALE",
                  gate.db, Cabinet::phaseName(servo.phase), (unsigned long)servo.pressCount);
  }

  delay(5);
}
