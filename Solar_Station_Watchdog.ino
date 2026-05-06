/*----------------------------------------------------------------------------------------------------
  Solar Station Watchdog V1.0
  
  A lightweight ESP8266 watchdog that monitors a Solar WiFi Weather Station
  via MQTT. Displays live data on a 0.96" SSD1306 OLED and alerts locally
  (LED blink + display warning) when the station goes silent or reports
  suspicious sensor data.
  
  Features:
  - Subscribes to weather station MQTT topic
  - Parses incoming JSON messages (temperature, humidity, battery, etc.)
  - OLED dashboard: live data, last-seen timer, status indicators
  - Alarm states:
      OFFLINE   - no message received for > ALARM_TIMEOUT_SEC
      BATT_WARN - battery voltage below warning threshold
      BATT_CRIT - battery voltage below critical threshold
      HUMI_STUCK- humidity stuck at 100% for N consecutive cycles (sensor dead)
      TEMP_ERR  - DS18B20 bus error (-88°C)
  - Onboard LED blinks on active alarm
  
  Hardware:
  - Wemos D1 Mini (or any ESP8266)
  - SSD1306 OLED 128x64 via I2C (address 0x3C)
  - USB power (runs continuously, no deep sleep)
  
  Required libraries (Arduino Library Manager):
  - Adafruit SSD1306
  - Adafruit GFX Library
  - ArduinoJson
  - PubSubClient
  - EasyNTPClient
  - Time (by Michael Margolis / Paul Stoffregen)
  
  Author: Marc Stähli
  Repository: https://github.com/3KUdelta/Solar_Station_Watchdog
 ****************************************************************************/

#include "Watchdog_Settings.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <EasyNTPClient.h>
#include <TimeLib.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ----- OLED setup -----
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDR       0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ----- Network objects -----
WiFiClient espClient;
PubSubClient client(espClient);
WiFiUDP udp;
EasyNTPClient ntpClient(udp, ntp_server, 0);

// ----- State variables -----
unsigned long last_msg_time      = 0;     // millis() when last MQTT message arrived
unsigned long last_station_epoch = 0;     // UTC epoch from station's "timestamp" field
unsigned long ntp_epoch          = 0;     // last known NTP time
unsigned long ntp_millis_ref     = 0;     // millis() at last NTP sync

// Latest weather data from station
float   wx_temp       = 0;
float   wx_humi       = 0;
float   wx_batt       = 0;
int     wx_batt_pct   = 0;
int     wx_pressure   = 0;
float   wx_dewpoint   = 0;
String  wx_zambretti  = "---";
String  wx_trend      = "---";
int     wx_rssi       = 0;

// Alarm tracking
bool    alarm_offline    = false;
bool    alarm_batt_warn  = false;
bool    alarm_batt_crit  = false;
bool    alarm_humi_stuck = false;
bool    alarm_temp_err   = false;
int     humi_stuck_count = 0;
bool    ever_received    = false;   // true after first MQTT message

// LED blink state
unsigned long last_blink = 0;
bool led_state = HIGH;              // HIGH = off (active low on Wemos)

// ----- Forward declarations -----
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void updateDisplay();
void checkAlarms();
void blinkLED();
unsigned long currentEpoch();
String timeAgo(unsigned long seconds);
String formatTime(unsigned long epoch);

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Solar Station Watchdog V1.0 ===");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // LED off (active low)

  // ----- OLED init -----
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 not found!");
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Solar Station");
  display.println("Watchdog V1.0");
  display.println();
  display.println("Connecting...");
  display.display();

  // ----- WiFi -----
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.print("WiFi ");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected");

  // ----- NTP -----
  Serial.print("NTP sync ");
  int attempts = 0;
  while (!ntpClient.getUnixTime() && attempts < 20) {
    yield();
    delay(100);
    attempts++;
    Serial.print(".");
  }
  ntp_epoch = ntpClient.getUnixTime();
  ntp_millis_ref = millis();
  Serial.print(" OK: ");
  Serial.println(ntp_epoch);

  // ----- MQTT -----
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(512);
  reconnectMQTT();

  // ----- Ready -----
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Waiting for");
  display.println("station data...");
  display.display();

  Serial.println("Watchdog running. Waiting for MQTT messages...");
}

// =====================================================================
void loop() {
  // Keep MQTT alive
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  // Periodic NTP re-sync (every 30 min)
  if (millis() - ntp_millis_ref > 1800000UL) {
    unsigned long t = ntpClient.getUnixTime();
    if (t > 0) {
      ntp_epoch = t;
      ntp_millis_ref = millis();
    }
  }

  // Check alarm conditions
  checkAlarms();

  // Blink LED if any alarm is active
  blinkLED();

  // Update OLED (~2 Hz to keep it responsive without flicker)
  static unsigned long last_display = 0;
  if (millis() - last_display > 500) {
    updateDisplay();
    last_display = millis();
  }

  yield();
}

// =====================================================================
// MQTT callback: parse incoming weather station JSON
// =====================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT message on ");
  Serial.print(topic);
  Serial.print(": ");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.f_str());
    return;
  }

  // Extract fields (matching the weather station's JSON format)
  wx_temp       = doc["temperature"]      | wx_temp;
  wx_humi       = doc["humidity"]         | wx_humi;
  wx_batt       = doc["battery"]          | wx_batt;
  wx_batt_pct   = doc["batterypercentage"]| wx_batt_pct;
  wx_pressure   = doc["relativepressure"] | wx_pressure;
  wx_dewpoint   = doc["dewpoint"]         | wx_dewpoint;
  wx_rssi       = doc["wifi_strength"]    | wx_rssi;

  if (doc["zambrettisays"].is<const char*>())
    wx_zambretti = doc["zambrettisays"].as<const char*>();
  if (doc["trendinwords"].is<const char*>())
    wx_trend = doc["trendinwords"].as<const char*>();
  if (doc["timestamp"].is<unsigned long>())
    last_station_epoch = doc["timestamp"];

  last_msg_time = millis();
  ever_received = true;

  // Humidity stuck detection
  if (wx_humi >= HUMI_STUCK_THRESHOLD) {
    humi_stuck_count++;
  } else {
    humi_stuck_count = 0;
  }

  Serial.print("T=");    Serial.print(wx_temp);
  Serial.print(" H=");   Serial.print(wx_humi);
  Serial.print(" B=");   Serial.print(wx_batt);
  Serial.print(" P=");   Serial.println(wx_pressure);
}

// =====================================================================
// Check all alarm conditions
// =====================================================================
void checkAlarms() {
  if (!ever_received) return;      // don't alarm before first message

  unsigned long age_sec = (millis() - last_msg_time) / 1000;

  // Offline detection
  alarm_offline    = (age_sec > ALARM_TIMEOUT_SEC);

  // Battery
  alarm_batt_crit  = (wx_batt > 0 && wx_batt < BATT_CRIT_VOLTAGE);
  alarm_batt_warn  = (wx_batt > 0 && wx_batt < BATT_WARN_VOLTAGE && !alarm_batt_crit);

  // Humidity stuck at 100% (sensor failure like the HDC1080 story)
  alarm_humi_stuck = (humi_stuck_count >= HUMI_STUCK_CYCLES);

  // Temperature sensor bus error
  alarm_temp_err   = (wx_temp <= TEMP_ERROR_VALUE);
}

// =====================================================================
// LED blink pattern: off = all OK, slow = warning, fast = critical
// =====================================================================
void blinkLED() {
  bool any_critical = alarm_offline || alarm_batt_crit || alarm_temp_err;
  bool any_warning  = alarm_batt_warn || alarm_humi_stuck;

  if (!any_critical && !any_warning) {
    digitalWrite(LED_PIN, HIGH);     // LED off, all good
    return;
  }

  unsigned long interval = any_critical ? 200 : 1000;   // fast or slow blink

  if (millis() - last_blink > interval) {
    led_state = !led_state;
    digitalWrite(LED_PIN, led_state);
    last_blink = millis();
  }
}

// =====================================================================
// OLED display update
// =====================================================================
void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (!ever_received) {
    // Waiting screen
    display.setCursor(0, 0);
    display.println("Waiting for");
    display.println("station data...");
    unsigned long wait = (millis() / 1000);
    display.print("(");
    display.print(wait);
    display.println("s)");
    display.display();
    return;
  }

  // ----- Line 0: Status bar -----
  display.setCursor(0, 0);
  if (alarm_offline) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);   // inverted = alarm
    display.print(" !! STATION OFFLINE !! ");
    display.setTextColor(SSD1306_WHITE);
  } else {
    // Time since last message
    unsigned long age_sec = (millis() - last_msg_time) / 1000;
    display.print("Last: ");
    display.print(timeAgo(age_sec));
    display.print("  ");
    // WiFi strength of station
    display.print(wx_rssi);
    display.println("dB");
  }

  // ----- Line 1: Temperature + Humidity -----
  display.setCursor(0, 14);
  if (alarm_temp_err) {
    display.print("T: ERR!");
  } else {
    display.print("T:");
    display.print(wx_temp, 1);
    display.print("\xF8""C");                    // ° symbol
  }
  display.print("  H:");
  if (alarm_humi_stuck) {
    display.println("STUCK!");
  } else {
    display.print(wx_humi, 0);
    display.println("%");
  }

  // ----- Line 2: Pressure + Trend -----
  display.setCursor(0, 26);
  display.print(wx_pressure);
  display.print("hPa ");
  // Truncate trend to fit screen
  String trend_short = wx_trend;
  if (trend_short.length() > 12) trend_short = trend_short.substring(0, 12);
  display.println(trend_short);

  // ----- Line 3: Battery -----
  display.setCursor(0, 38);
  display.print("Batt: ");
  display.print(wx_batt, 2);
  display.print("V ");
  display.print(wx_batt_pct);
  display.print("% ");
  if (alarm_batt_crit) {
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.print("CRIT");
    display.setTextColor(SSD1306_WHITE);
  } else if (alarm_batt_warn) {
    display.print("LOW");
  } else {
    display.print("OK");
  }

  // ----- Line 4: Zambretti forecast -----
  display.setCursor(0, 50);
  // Truncate to fit 128px width (~21 chars at size 1)
  String forecast = wx_zambretti;
  if (forecast.length() > 21) forecast = forecast.substring(0, 21);
  display.print(forecast);

  display.display();
}

// =====================================================================
// MQTT reconnect with 3 retries
// =====================================================================
void reconnectMQTT() {
  int attempts = 0;
  while (!client.connected() && attempts < 3) {
    Serial.print("MQTT connecting (try ");
    Serial.print(attempts + 1);
    Serial.print("/3)...");
    String clientId = "Watchdog-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println(" connected");
      client.subscribe(mqtt_topic);
      Serial.print("Subscribed to: ");
      Serial.println(mqtt_topic);
      return;
    } else {
      Serial.print(" failed (rc=");
      Serial.print(client.state());
      Serial.println(")");
      delay(2000);
      attempts++;
    }
  }
  Serial.println("MQTT failed - will retry in loop");
}

// =====================================================================
// Helper: current epoch from NTP + millis offset
// =====================================================================
unsigned long currentEpoch() {
  return ntp_epoch + ((millis() - ntp_millis_ref) / 1000);
}

// =====================================================================
// Helper: human-readable "time ago" string
// =====================================================================
String timeAgo(unsigned long seconds) {
  if (seconds < 60)   return String(seconds) + "s ago";
  if (seconds < 3600)  return String(seconds / 60) + "m ago";
  if (seconds < 86400) return String(seconds / 3600) + "h ago";
  return String(seconds / 86400) + "d ago";
}

// =====================================================================
// Helper: format epoch as HH:MM:SS
// =====================================================================
String formatTime(unsigned long epoch) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
           hour(epoch), minute(epoch), second(epoch));
  return String(buf);
}
