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
float   wx_trend_val  = 0;        // numeric trend value for arrow direction

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
  wx_trend_val  = doc["trend"]            | wx_trend_val;
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
// ----- Custom icons drawn with drawLine/fillTriangle -----

// Draw a trend arrow at position (x, y), size ~12x12 px
// direction: 1=up, 0=steady, -1=down
// fast: true = double arrow (fast rise/fall)
void drawTrendArrow(int x, int y, int direction, bool fast) {
  if (direction == 1) {
    // Arrow pointing up-right (↗)
    display.drawLine(x, y + 11, x + 11, y, SSD1306_WHITE);       // shaft
    display.fillTriangle(x + 5, y, x + 11, y, x + 11, y + 6, SSD1306_WHITE); // head
    if (fast) {  // second arrow behind
      display.drawLine(x, y + 14, x + 8, y + 6, SSD1306_WHITE);
    }
  } else if (direction == -1) {
    // Arrow pointing down-right (↘)
    display.drawLine(x, y, x + 11, y + 11, SSD1306_WHITE);       // shaft
    display.fillTriangle(x + 5, y + 11, x + 11, y + 5, x + 11, y + 11, SSD1306_WHITE);
    if (fast) {
      display.drawLine(x, y - 3, x + 8, y + 5, SSD1306_WHITE);
    }
  } else {
    // Steady: horizontal arrow (→)
    display.drawLine(x, y + 6, x + 11, y + 6, SSD1306_WHITE);   // shaft
    display.fillTriangle(x + 8, y + 3, x + 11, y + 6, x + 8, y + 9, SSD1306_WHITE);
  }
}

// Draw a warning triangle icon at (x, y), size w x h
void drawWarningIcon(int x, int y, int w, int h) {
  // Outer triangle
  display.drawTriangle(x + w / 2, y, x, y + h - 1, x + w - 1, y + h - 1, SSD1306_WHITE);
  display.drawTriangle(x + w / 2, y + 1, x + 1, y + h - 1, x + w - 2, y + h - 1, SSD1306_WHITE);
  // Exclamation mark inside
  int cx = x + w / 2;
  int ey = y + h / 3;
  display.drawLine(cx, ey, cx, ey + h / 4, SSD1306_WHITE);      // stem
  display.drawPixel(cx, y + h - h / 4, SSD1306_WHITE);           // dot
}

// Get trend direction and speed from numeric trend value
void getTrendInfo(float val, int &direction, bool &fast) {
  if      (val >  1.5) { direction =  1; fast = true;  }   // rising fast
  else if (val >  0.25){ direction =  1; fast = false; }   // rising / rising slow
  else if (val < -1.5) { direction = -1; fast = true;  }   // falling fast
  else if (val < -0.25){ direction = -1; fast = false; }   // falling / falling slow
  else                 { direction =  0; fast = false; }   // steady
}

void updateDisplay() {
  display.clearDisplay();

  if (!ever_received) {
    // ----- Waiting screen -----
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 12);
    display.println("Waiting for");
    display.setCursor(10, 24);
    display.println("station data...");
    unsigned long wait = millis() / 1000;
    display.setCursor(10, 40);
    display.print(wait);
    display.print("s");
    display.display();
    return;
  }

  unsigned long age_sec = (millis() - last_msg_time) / 1000;

  // =============================================
  // ALARM SCREENS (full-screen, visible from far)
  // =============================================
  if (alarm_offline) {
    // Large warning triangle centered
    drawWarningIcon(48, 2, 32, 28);

    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 34);
    display.print("OFFLINE!");

    display.setTextSize(1);
    display.setCursor(20, 54);
    display.print("last: ");
    display.print(timeAgo(age_sec));

    display.display();
    return;
  }

  if (alarm_batt_crit) {
    drawWarningIcon(48, 2, 32, 28);

    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(4, 34);
    display.print("BATT CRIT");

    display.setTextSize(1);
    display.setCursor(30, 54);
    display.print(wx_batt, 2);
    display.print("V  ");
    display.print(wx_batt_pct);
    display.print("%");

    display.display();
    return;
  }

  if (alarm_temp_err) {
    drawWarningIcon(48, 2, 32, 28);

    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(4, 34);
    display.print("TEMP ERR!");

    display.setTextSize(1);
    display.setCursor(16, 54);
    display.print("DS18B20 bus fault");

    display.display();
    return;
  }

  if (alarm_humi_stuck) {
    drawWarningIcon(48, 2, 32, 28);

    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(4, 34);
    display.print("HUMI 100%");

    display.setTextSize(1);
    display.setCursor(10, 54);
    display.print("sensor stuck (");
    display.print(humi_stuck_count);
    display.print("x)");

    display.display();
    return;
  }

  // =============================================
  // NORMAL DISPLAY (data dashboard)
  // =============================================

  // ----- Row 1: Temperature + Humidity (large, eye-catching) -----
  //
  // Layout:  18.4°C        67%
  //          ^^^^           ^^^
  //          size 2         size 2
  //
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  if (wx_temp >= 0 && wx_temp < 10) display.print(" ");     // right-align single digit
  display.print(wx_temp, 1);
  display.setTextSize(1);
  display.setCursor(display.getCursorX(), 0);
  display.print("\xF8""C");                                   // °C in small font

  display.setTextSize(2);
  display.setCursor(84, 0);
  if (wx_humi < 10) display.print("  ");
  else if (wx_humi < 100) display.print(" ");
  display.print(wx_humi, 0);
  display.setTextSize(1);
  display.setCursor(display.getCursorX(), 0);
  display.print("%");

  // ----- Separator line -----
  display.drawLine(0, 19, 127, 19, SSD1306_WHITE);

  // ----- Row 2: Pressure + Trend arrow + Battery -----
  //
  // Layout:  1018 hPa  [↗]  3.9V
  //
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.print(wx_pressure);
  display.print(" hPa");

  // Trend arrow (drawn between pressure and battery)
  int trend_dir;
  bool trend_fast;
  getTrendInfo(wx_trend_val, trend_dir, trend_fast);
  drawTrendArrow(62, 22, trend_dir, trend_fast);

  // Battery voltage (right-aligned)
  display.setCursor(90, 24);
  display.print(wx_batt, 1);
  display.print("V");
  if (alarm_batt_warn) {
    display.print("!");
  }

  // ----- Row 3: Dewpoint spread + Last seen (status bar) -----
  //
  // Layout:  Dew:12.3°C  Sp:6.1   3m ago
  //
  display.setCursor(0, 38);
  display.print("Dew:");
  display.print(wx_dewpoint, 1);
  display.print("\xF8");

  float spread = wx_temp - wx_dewpoint;
  display.setCursor(60, 38);
  display.print("Sp:");
  display.print(spread, 1);

  display.setCursor(0, 52);
  display.print("Last: ");
  display.print(timeAgo(age_sec));

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
