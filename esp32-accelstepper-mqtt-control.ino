/*
 * ESP32 Stepper Motor Control over MQTT
 * -------------------------------------
 * Moves a NEMA17 stepper (via an A4988 or DRV8825 driver) to absolute
 * positions commanded over MQTT, using AccelStepper's acceleration ramps so
 * the motor does not stall or skip steps. run() is called every pass of
 * loop(), so WiFi and MQTT stay responsive while the motor moves.
 *
 * Commands (plain text on MQTT_CMD_TOPIC):
 *   <integer>  move to that absolute position in steps
 *   home       drive toward the limit switch, zero the position on contact
 *   stop       decelerate to a halt
 * Position (steps) is published to MQTT_STATE_TOPIC while moving and when
 * stopped; "homing" / "moving" / "idle" / "error" goes to MQTT_STATUS_TOPIC.
 *
 * Use it for feeders, sliders, dispensers and turntables in automation theses.
 *
 * Required libraries (install via Library Manager):
 *   - "AccelStepper" by Mike McCauley
 *   - "PubSubClient" by Nick O'Leary
 *
 * Board: any ESP32 dev board (ESP32 core for Arduino installed via Boards Mgr)
 *
 * Wiring (A4988 / DRV8825):
 *   Driver STEP -> GPIO26      Driver DIR -> GPIO27
 *   Driver EN   -> GPIO25      (active LOW: LOW = motor powered)
 *   Driver RESET and SLEEP tied together (or both to 3.3V)
 *   Driver VDD  -> 3.3V, GND -> ESP32 GND
 *   Driver VMOT -> 12V supply (+), GND -> supply (-) and ESP32 GND
 *   Put a 100uF+ capacitor across VMOT/GND, close to the driver.
 *   Limit switch (NO) between GPIO33 and GND  (internal pull-up is used)
 *   NEVER connect or disconnect the motor while the driver is powered.
 *
 * Set the driver's current limit with its trimpot BEFORE running a heavy
 * load (see the README).
 *
 * Setup:
 *   1. Copy config.h.example to config.h
 *   2. Edit config.h with your WiFi creds + MQTT broker + topics
 *   3. Flash. Open Serial Monitor at 115200.
 *
 * Try it with mosquitto from a PC:
 *   mosquitto_pub -h broker.hivemq.com -t "fluidwire/demo/stepper/cmd" -m "home"
 *   mosquitto_pub -h broker.hivemq.com -t "fluidwire/demo/stepper/cmd" -m "1600"
 *   mosquitto_sub -h broker.hivemq.com -t "fluidwire/demo/stepper/#"
 *
 * (c) 2026 Fluidwire. Released under the MIT License.
 * https://github.com/fluidwire
 */

#include <WiFi.h>
#include <AccelStepper.h>
#include <PubSubClient.h>
#include "config.h"

#define STEP_PIN       26
#define DIR_PIN        27
#define ENABLE_PIN     25      // active LOW on A4988 / DRV8825
#define LIMIT_PIN      33      // limit switch to GND, internal pull-up

#define MAX_SPEED      1000.0  // steps/s -- lower this if the motor stalls
#define ACCELERATION   500.0   // steps/s^2
#define HOMING_SPEED   300.0   // steps/s, slow and gentle toward the switch
#define HOMING_BACKOFF 80      // steps to back off the switch after contact
#define HOMING_TIMEOUT_MS 60000UL  // give up if the switch is never hit

// Mechanical travel limits in steps from the home position. Set these to what
// your slider/feeder can physically survive, not what the motor could do.
#define POS_MIN        0L
#define POS_MAX        6400L   // 200 steps/rev x 16 microsteps x 2 revolutions

#define REPORT_MS      500UL   // position report interval while moving

AccelStepper stepper(AccelStepper::DRIVER, STEP_PIN, DIR_PIN);
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

enum Mode { IDLE, HOMING_SEEK, HOMING_BACKOFF_MOVE, MOVING };
Mode          mode = IDLE;
unsigned long homingStartMs = 0;
unsigned long lastReportMs  = 0;

void publishStatus(const char *s) {
  mqtt.publish(MQTT_STATUS_TOPIC, s, true);
  Serial.printf("Status -> %s\n", s);
}

void publishPosition() {
  char msg[16];
  snprintf(msg, sizeof(msg), "%ld", stepper.currentPosition());
  mqtt.publish(MQTT_STATE_TOPIC, msg, true);
  Serial.printf("Position -> %s steps\n", msg);
}

void startHoming() {
  Serial.println("Homing: seeking limit switch");
  digitalWrite(ENABLE_PIN, LOW);
  stepper.setMaxSpeed(HOMING_SPEED);
  stepper.setSpeed(-HOMING_SPEED);      // negative = toward the switch
  homingStartMs = millis();
  mode = HOMING_SEEK;
  publishStatus("homing");
}

void startMove(long target) {
  if (target < POS_MIN || target > POS_MAX) {
    Serial.printf("Rejected %ld -- allowed range is %ld..%ld steps\n",
                  target, POS_MIN, POS_MAX);
    publishStatus("error");
    return;
  }
  digitalWrite(ENABLE_PIN, LOW);
  stepper.setMaxSpeed(MAX_SPEED);
  stepper.moveTo(target);
  mode = MOVING;
  Serial.printf("Moving to %ld steps\n", target);
  publishStatus("moving");
}

void onMessage(char *topic, byte *payload, unsigned int len) {
  char cmd[24];
  unsigned int n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
  memcpy(cmd, payload, n);
  cmd[n] = '\0';
  Serial.printf("Cmd on %s: '%s'\n", topic, cmd);

  if (strcmp(cmd, "home") == 0) {
    startHoming();
  } else if (strcmp(cmd, "stop") == 0) {
    stepper.stop();                     // sets a target that ramps to a halt
    if (mode == HOMING_SEEK || mode == HOMING_BACKOFF_MOVE) {
      stepper.moveTo(stepper.currentPosition());
      mode = IDLE;
      publishStatus("error");           // homing aborted -- position unknown
    } else if (mode == MOVING) {
      // keep running until decelerated; loop() reports idle at standstill
    }
  } else {
    // strtol() returns 0 for "banana", which would silently slam the axis to
    // position 0, so require the whole payload to be a valid integer.
    char *end = nullptr;
    long target = strtol(cmd, &end, 10);
    if (end == cmd || *end != '\0') {
      Serial.printf("Ignored '%s' -- expected <steps>, home or stop\n", cmd);
      return;
    }
    if (mode == HOMING_SEEK || mode == HOMING_BACKOFF_MOVE) {
      Serial.println("Ignored move -- homing in progress");
      return;
    }
    startMove(target);
  }
}

void connectWiFi() {
  Serial.printf("WiFi: connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    stepper.run();                      // keep any move in progress going
    if (millis() - t0 > 20000UL) {
      Serial.println(" timeout, retrying");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      t0 = millis();
    }
    delay(1);
  }
  Serial.printf(" OK, ip=%s\n", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  while (!mqtt.connected()) {
    Serial.printf("MQTT: connecting to %s:%u as %s ... ",
                  MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID);
    bool ok = strlen(MQTT_USER) > 0
      ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)
      : mqtt.connect(MQTT_CLIENT_ID);
    if (ok) {
      Serial.println("OK");
      mqtt.subscribe(MQTT_CMD_TOPIC);
      Serial.printf("Subscribed to %s\n", MQTT_CMD_TOPIC);
      publishPosition();
      publishStatus("idle");
    } else {
      Serial.printf("FAIL rc=%d, retry in 5s\n", mqtt.state());
      unsigned long t0 = millis();
      while (millis() - t0 < 5000UL) {  // wait without freezing the motor
        stepper.run();
        delay(1);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(ENABLE_PIN, OUTPUT);
  digitalWrite(ENABLE_PIN, HIGH);       // driver off until first command
  pinMode(LIMIT_PIN, INPUT_PULLUP);

  stepper.setMaxSpeed(MAX_SPEED);
  stepper.setAcceleration(ACCELERATION);
  stepper.setCurrentPosition(0);        // unknown until "home" is issued
  Serial.println("Stepper ready -- send 'home' before absolute moves");

  connectWiFi();
  connectMQTT();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  switch (mode) {
    case HOMING_SEEK:
      if (digitalRead(LIMIT_PIN) == LOW) {
        Serial.println("Homing: switch hit, backing off");
        stepper.setCurrentPosition(0);
        stepper.setMaxSpeed(HOMING_SPEED);
        stepper.moveTo(HOMING_BACKOFF);
        mode = HOMING_BACKOFF_MOVE;
      } else if (millis() - homingStartMs > HOMING_TIMEOUT_MS) {
        Serial.println("Homing: TIMEOUT, limit switch never triggered");
        digitalWrite(ENABLE_PIN, HIGH);
        mode = IDLE;
        publishStatus("error");
      } else {
        stepper.runSpeed();             // constant speed toward the switch
      }
      break;

    case HOMING_BACKOFF_MOVE:
      stepper.run();
      if (stepper.distanceToGo() == 0) {
        stepper.setCurrentPosition(0);  // the backed-off point is home
        stepper.setMaxSpeed(MAX_SPEED);
        mode = IDLE;
        Serial.println("Homing: done, position zeroed");
        publishPosition();
        publishStatus("idle");
      }
      break;

    case MOVING:
      stepper.run();
      if (millis() - lastReportMs >= REPORT_MS) {
        lastReportMs = millis();
        publishPosition();
      }
      if (stepper.distanceToGo() == 0) {
        mode = IDLE;
        publishPosition();
        publishStatus("idle");
      }
      break;

    case IDLE:
    default:
      break;
  }
}
