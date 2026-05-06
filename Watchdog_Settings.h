/*----------------------------------------------------------------------------------------------------
  Settings for Solar Station Watchdog
  Adjust these values to match your weather station and network setup.
 ****************************************************************************/

// ----- WiFi -----
const char* ssid = "YOUR_SSID";
const char* pass = "YOUR_PASSWORD";

// ----- MQTT -----
// These must match the settings in your weather station's Settings26.h
const char* mqtt_server     = "broker.hivemq.com";
const int   mqtt_port       = 1883;
const char* mqtt_user       = "";
const char* mqtt_pass       = "";
const char* mqtt_topic      = "YOUR_TOPIC";         // same as weather station's mqtt_topic

// ----- NTP -----
const char* ntp_server      = "ch.pool.ntp.org";

// ----- Watchdog thresholds -----
#define ALARM_TIMEOUT_SEC     1500    // seconds without message before alarm (default: 25 min = 1500 s)
#define BATT_WARN_VOLTAGE     3.5     // battery voltage warning threshold
#define BATT_CRIT_VOLTAGE     3.4     // battery voltage critical threshold
#define HUMI_STUCK_THRESHOLD  99.5    // humidity above this for HUMI_STUCK_CYCLES = sensor problem
#define HUMI_STUCK_CYCLES     3       // how many consecutive readings at 100% before alarm
#define TEMP_ERROR_VALUE      (-88)   // DS18B20 returns this on bus error

// ----- Hardware pins -----
#define OLED_SDA        D2            // I2C SDA (default Wemos D1 Mini)
#define OLED_SCL        D1            // I2C SCL
#define LED_PIN         LED_BUILTIN   // onboard LED (active LOW on Wemos)
