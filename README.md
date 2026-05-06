# Solar Station Watchdog

A lightweight ESP8266 watchdog that monitors a [Solar WiFi Weather Station](https://github.com/3KUdelta/Solar_WiFi_Weather_Station) via MQTT and displays live data on a small OLED screen.

![Watchdog concept](concept.png)

## What it does

The watchdog subscribes to the weather station's MQTT topic and tracks incoming messages. A 0.96" OLED shows live weather data, battery status, and a "last seen" timer. If the station goes silent or reports suspicious values, the onboard LED starts blinking and the display shows a warning.

### Alarm conditions

| Alarm | Trigger | LED pattern |
|---|---|---|
| **OFFLINE** | No MQTT message for > 25 minutes (configurable) | fast blink |
| **BATT CRIT** | Battery voltage below 3.4 V | fast blink |
| **BATT LOW** | Battery voltage below 3.5 V | slow blink |
| **HUMI STUCK** | Humidity ≥ 99.5% for 3 consecutive readings | slow blink |
| **TEMP ERR** | Temperature = -88°C (DS18B20 bus error) | fast blink |

The humidity-stuck detection catches exactly the kind of sensor failure that prompted the HDC1080 → SHT45 migration in the weather station: a dying humidity sensor drifts to a permanent 100% reading over days, which is easy to miss when only looking at dashboards.

### OLED display layout

```
┌──────────────────────┐
│ Last: 3m ago   -67dB │   ← time since last message + station WiFi RSSI
│ T:18.4°C  H:67%      │   ← temperature + humidity
│ 1018hPa steigend     │   ← pressure + trend
│ Batt: 3.92V 74% OK   │   ← battery voltage + percentage + status
│ Schönes Wetter       │   ← Zambretti forecast
└──────────────────────┘
```

When a station goes offline, the top line inverts to:
```
┌──────────────────────┐
│ !! STATION OFFLINE !!│
│ ...                  │
```


## Hardware

| Part | Notes |
|---|---|
| Wemos D1 Mini (ESP8266) | Any ESP8266 board works |
| SSD1306 OLED 128×64 | I²C, address 0x3C, connected to D1 (SCL) and D2 (SDA) |
| USB cable + power supply | Runs continuously, no battery needed |

Total cost: ~5 €. No soldering required if using pre-wired OLED breakout.

### Wiring

```
Wemos D1 Mini          SSD1306 OLED
  D1 (GPIO5)  ──────── SCL
  D2 (GPIO4)  ──────── SDA
  3.3V        ──────── VCC
  GND         ──────── GND
```


## Setup

1. Install the required libraries via Arduino Library Manager:
   - `Adafruit SSD1306`
   - `Adafruit GFX Library`
   - `ArduinoJson`
   - `PubSubClient`
   - `EasyNTPClient`
   - `Time` (by Michael Margolis)

2. Edit `Watchdog_Settings.h`:
   - Set your WiFi credentials (`ssid`, `pass`)
   - Set your MQTT broker and topic (must match the weather station's settings)
   - Adjust alarm thresholds if needed

3. Flash the sketch to your Wemos D1 Mini

4. Power via USB — the watchdog runs continuously


## Configuration

All settings are in `Watchdog_Settings.h`:

| Setting | Default | Description |
|---|---|---|
| `ALARM_TIMEOUT_SEC` | 1500 (25 min) | Seconds without MQTT message before OFFLINE alarm |
| `BATT_WARN_VOLTAGE` | 3.5 | Battery warning threshold (V) |
| `BATT_CRIT_VOLTAGE` | 3.4 | Battery critical threshold (V) |
| `HUMI_STUCK_THRESHOLD` | 99.5 | Humidity (%) above which stuck-detection counts |
| `HUMI_STUCK_CYCLES` | 3 | Consecutive readings at 100% before alarm |
| `TEMP_ERROR_VALUE` | -88 | DS18B20 returns this on bus error |

### Why 25 minutes?

The weather station wakes every 10 minutes. A single missed cycle can happen (slow WiFi, NTP retry). Two missed cycles (20 min) with some margin = 25 minutes means something is genuinely wrong: dead battery, hardware failure, or network issue.


## Compatibility

This watchdog works with the Solar WiFi Weather Station V2.4 and later. It parses the standard JSON message format:

```json
{
  "temperature": 18.4,
  "humidity": 67.2,
  "battery": 3.92,
  "batterypercentage": 74,
  "relativepressure": 1018,
  "dewpoint": 12.3,
  "zambrettisays": "Schönes Wetter",
  "trendinwords": "steigend",
  "wifi_strength": -67,
  "timestamp": 1745923200
}
```

Any additional fields in the message are silently ignored — forward-compatible.


## License

Same license as the Solar WiFi Weather Station project.

---

*HiFiLabor.ch — Marc Stähli, 2026*
