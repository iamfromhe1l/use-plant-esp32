#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>

const char* apSSID = "PlantWatering-ESP32";
const char* apPassword = NULL;
const int DNS_PORT = 53;
const int HTTP_PORT = 80;

const char* LEGACY_BACKEND_HOST = "192.168.0.104";
const char* LEGACY_BACKEND_URL = "http://192.168.0.104:4000";
const char* DEFAULT_BACKEND_HOST = "72.56.240.75";
const char* DEFAULT_BACKEND_URL = "http://72.56.240.75:4000";
const int MQTT_PORT = 1883;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const int DEFAULT_TELEMETRY_INTERVAL_MINUTES = 5;
const int DHT_PIN = 4; // Board pin label: D4 / GPIO4
const int SOIL_SENSOR_PINS[2] = {34, 35}; // Board pin labels: D34 / D35
const int SOIL_DRY_VALUES[2] = {3000, 3000};
const int SOIL_WET_VALUES[2] = {1300, 1300};
const int SOIL_SAMPLE_COUNT = 8;
const unsigned long DHT_READ_INTERVAL = 2500;
const int DISPLAY_SDA_PIN = 21; // Board pin label: D21 / GPIO21
const int DISPLAY_SCL_PIN = 22; // Board pin label: D22 / GPIO22
const unsigned long DISPLAY_REFRESH_INTERVAL = 120;
const unsigned long SOIL_READ_INTERVAL = 1500;
const unsigned long WATERING_ANIMATION_DURATION = 5000;
const long GMT_OFFSET_SEC = 3 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";

DNSServer dnsServer;
WebServer server(HTTP_PORT);
Preferences preferences;
HTTPClient http;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
DHTesp dhtSensor;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

String savedSSID = "";
String savedPassword = "";
String savedToken = "";
String savedDeviceSecret = "";
String savedBackendUrl = DEFAULT_BACKEND_URL;
String savedMqttServer = DEFAULT_BACKEND_HOST;
bool shouldSaveConfig = false;
bool configMode = false;
unsigned long lastMqttReconnect = 0;
unsigned long lastTelemetrySend = 0;
unsigned long telemetryIntervalMs = (unsigned long)DEFAULT_TELEMETRY_INTERVAL_MINUTES * 60000UL;
unsigned long lastDhtRead = 0;
unsigned long lastDhtErrorLog = 0;
float lastAirTemperature = 22.0f;
float lastAirHumidity = 50.0f;
bool hasValidDhtReading = false;
unsigned long lastSoilRead = 0;
float lastSoilMoisture[2] = {0.0f, 0.0f};
int lastSoilRawValues[2] = {0, 0};
bool hasValidSoilReadings = false;
unsigned long lastDisplayRefresh = 0;
unsigned long wateringAnimationUntil = 0;
int wateringAnimationPlant = 1;
int wateringAnimationLevel = 5;

enum DisplayState {
  DISPLAY_BOOT,
  DISPLAY_WIFI_SETUP,
  DISPLAY_WIFI_CONNECTING,
  DISPLAY_REGISTERING,
  DISPLAY_MQTT_CONNECTING,
  DISPLAY_NORMAL,
  DISPLAY_ERROR
};

DisplayState currentDisplayState = DISPLAY_BOOT;
String displayPrimaryText = "usePlant";
String displaySecondaryText = "starting";

// ===== Structs =====

struct SensorRule {
  char field[16];     // "temperature", "airHumidity", "soilMoisture"
  char op[4];         // "eq", "gt", "lt"
  float value;
};

struct Schedule {
  int hours[6];
  int minutes[6];
  int timeCount;
  bool days[7]; // 0=Sun .. 6=Sat
};

struct WateringCondition {
  int plantIndex;
  char type[10];      // "sensor" or "schedule"
  int level;          // 1-10
  int interval;       // minutes
  SensorRule rules[4];
  int ruleCount;
  Schedule schedule;
  bool enabled;
  unsigned long lastTriggered; // millis when last watered
  long lastScheduleTriggerKey;
};

struct BackendResponse {
  bool success;
  String deviceSecret;
  String error;
};

String extractHostFromUrl(const String& url) {
  String host = url;

  int schemePos = host.indexOf("://");
  if (schemePos >= 0) {
    host = host.substring(schemePos + 3);
  }

  int slashPos = host.indexOf('/');
  if (slashPos >= 0) {
    host = host.substring(0, slashPos);
  }

  int colonPos = host.indexOf(':');
  if (colonPos >= 0) {
    host = host.substring(0, colonPos);
  }

  return host;
}

void setupClock() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
}

bool getCurrentLocalTimeInfo(struct tm* timeinfo) {
  return getLocalTime(timeinfo, 1000);
}

long buildScheduleTriggerKey(const struct tm& timeinfo, int hour, int minute) {
  return (long)timeinfo.tm_yday * 10000L + (long)hour * 100L + minute;
}

void resetSchedule(Schedule& schedule) {
  memset(schedule.hours, 0, sizeof(schedule.hours));
  memset(schedule.minutes, 0, sizeof(schedule.minutes));
  schedule.timeCount = 0;
  memset(schedule.days, 0, sizeof(schedule.days));
}

bool parseScheduleTime(const String& timeStr, int& hour, int& minute) {
  if (timeStr.length() != 5 || timeStr.charAt(2) != ':') {
    return false;
  }

  hour = timeStr.substring(0, 2).toInt();
  minute = timeStr.substring(3).toInt();

  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

void addScheduleTime(Schedule& schedule, const String& timeStr) {
  if (schedule.timeCount >= 6) {
    return;
  }

  int hour = 0;
  int minute = 0;
  if (!parseScheduleTime(timeStr, hour, minute)) {
    return;
  }

  for (int i = 0; i < schedule.timeCount; i++) {
    if (schedule.hours[i] == hour && schedule.minutes[i] == minute) {
      return;
    }
  }

  schedule.hours[schedule.timeCount] = hour;
  schedule.minutes[schedule.timeCount] = minute;
  schedule.timeCount++;
}

void loadScheduleFromJson(JsonObjectConst sched, Schedule& schedule) {
  resetSchedule(schedule);

  if (sched.containsKey("times")) {
    JsonArrayConst times = sched["times"].as<JsonArrayConst>();
    for (JsonVariantConst value : times) {
      addScheduleTime(schedule, value.as<const char*>());
    }
  }

  if (schedule.timeCount == 0 && sched.containsKey("time")) {
    addScheduleTime(schedule, sched["time"].as<const char*>());
  }

  if (schedule.timeCount == 0) {
    addScheduleTime(schedule, "08:00");
  }

  bool hasAnyDay = false;
  if (sched.containsKey("days")) {
    JsonArrayConst days = sched["days"].as<JsonArrayConst>();
    for (JsonVariantConst value : days) {
      int d = value.as<int>();
      if (d >= 0 && d < 7) {
        schedule.days[d] = true;
        hasAnyDay = true;
      }
    }
  }

  if (!hasAnyDay) {
    for (int d = 0; d < 7; d++) {
      schedule.days[d] = true;
    }
  }
}

void setDisplayState(DisplayState state, const String& primary = "", const String& secondary = "") {
  currentDisplayState = state;

  if (primary.length() > 0 || state == DISPLAY_NORMAL) {
    displayPrimaryText = primary;
  }

  if (secondary.length() > 0 || state == DISPLAY_NORMAL) {
    displaySecondaryText = secondary;
  }
}

void startWateringAnimation(int plantIndex, int level) {
  wateringAnimationPlant = plantIndex;
  wateringAnimationLevel = level;
  wateringAnimationUntil = millis() + WATERING_ANIMATION_DURATION;
}

void drawCenteredText(int y, const String& text) {
  int width = oled.getStrWidth(text.c_str());
  int x = (128 - width) / 2;
  if (x < 0) x = 0;
  oled.drawStr(x, y, text.c_str());
}

void drawSoilCard(int x, int y, const char* label, float moisturePercent) {
  char valueBuffer[8];
  int value = (int)(moisturePercent + 0.5f);
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  snprintf(valueBuffer, sizeof(valueBuffer), "%d%%", value);

  oled.drawRFrame(x, y, 58, 26, 8);
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(x + 6, y + 11, label);
  oled.setFont(u8g2_font_7x13B_tf);
  oled.drawStr(x + 6, y + 23, valueBuffer);

  int barWidth = 20;
  int fillWidth = (int)((barWidth * value) / 100.0f);
  oled.drawRFrame(x + 31, y + 16, barWidth, 6, 3);
  if (fillWidth > 0) {
    oled.drawRBox(x + 32, y + 17, fillWidth - (fillWidth == barWidth ? 0 : 1), 4, 2);
  }
}

void drawStatusScene(const String& title, const String& subtitle) {
  oled.drawRFrame(6, 6, 116, 52, 10);
  oled.drawRFrame(14, 14, 100, 36, 8);
  oled.setFont(u8g2_font_7x13B_tf);
  drawCenteredText(27, title);
  oled.setFont(u8g2_font_6x12_tf);
  drawCenteredText(40, subtitle);
}

void drawWateringScene() {
  char plantBuffer[20];
  char levelBuffer[20];

  snprintf(plantBuffer, sizeof(plantBuffer), "Растение %d", wateringAnimationPlant);
  snprintf(levelBuffer, sizeof(levelBuffer), "Полив %d/10", wateringAnimationLevel);

  oled.drawRFrame(2, 2, 124, 60, 12);
  oled.drawRFrame(8, 8, 112, 48, 10);

  oled.setFont(u8g2_font_7x13B_tf);
  drawCenteredText(18, "Полив");
  oled.setFont(u8g2_font_6x12_tf);
  drawCenteredText(31, plantBuffer);
  drawCenteredText(42, levelBuffer);
  oled.drawRFrame(28, 47, 72, 8, 4);
  oled.drawRBox(30, 49, 48, 4, 2);
}

void drawDashboard() {
  char airTempBuffer[12];
  char airHumidityBuffer[12];

  snprintf(airTempBuffer, sizeof(airTempBuffer), "%.1fC", getTemperature(1));
  snprintf(airHumidityBuffer, sizeof(airHumidityBuffer), "%.0f%%", getAirHumidity(1));

  oled.setFont(u8g2_font_7x13B_tf);
  drawCenteredText(14, "usePlant");

  drawSoilCard(4, 22, "Почва 1", getSoilMoisture(1));
  drawSoilCard(66, 22, "Почва 2", getSoilMoisture(2));

  oled.drawRFrame(4, 50, 120, 12, 6);
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(10, 59, airTempBuffer);
  oled.drawStr(56, 59, airHumidityBuffer);
  oled.drawDisc(104, 56, 2);
  oled.drawCircle(112, 56, 3);
}

void updateDisplay(bool force = false) {
  unsigned long now = millis();

  if (!force && (now - lastDisplayRefresh) < DISPLAY_REFRESH_INTERVAL) {
    return;
  }

  lastDisplayRefresh = now;

  oled.clearBuffer();

  if (wateringAnimationUntil > now) {
    drawWateringScene();
  } else {
    switch (currentDisplayState) {
      case DISPLAY_BOOT:
      case DISPLAY_WIFI_SETUP:
      case DISPLAY_WIFI_CONNECTING:
      case DISPLAY_REGISTERING:
      case DISPLAY_MQTT_CONNECTING:
      case DISPLAY_ERROR:
        drawStatusScene(displayPrimaryText, displaySecondaryText);
        break;
      case DISPLAY_NORMAL:
      default:
        drawDashboard();
        break;
    }
  }

  oled.sendBuffer();
}

// ===== Logging =====

void printDebug(const String& message) {
  Serial.println("[DEBUG] " + message);
}

void printError(const String& message) {
  Serial.println("[ERROR] " + message);
}

void printSuccess(const String& message) {
  Serial.println("[SUCCESS] " + message);
}

void printWarning(const String& message) {
  Serial.println("[WARNING] " + message);
}

void printJson(const String& label, const String& json) {
  Serial.println("[JSON] " + label + ": " + json);
}

// ===== Watering Conditions =====

const int MAX_CONDITIONS = 8;
WateringCondition waterConditions[MAX_CONDITIONS];
int conditionCount = 0;
unsigned long lastConditionCheck = 0;
const unsigned long CONDITION_CHECK_INTERVAL = 10000; // check every 10 seconds

void saveConditionsToNVS() {
  StaticJsonDocument<4096> doc;
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < conditionCount; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["plantIndex"] = waterConditions[i].plantIndex;
    obj["type"] = waterConditions[i].type;
    obj["level"] = waterConditions[i].level;
    obj["interval"] = waterConditions[i].interval;
    obj["enabled"] = waterConditions[i].enabled;

    if (waterConditions[i].ruleCount > 0) {
      JsonArray rules = obj.createNestedArray("rules");
      for (int j = 0; j < waterConditions[i].ruleCount; j++) {
        JsonObject rule = rules.createNestedObject();
        rule["field"] = waterConditions[i].rules[j].field;
        rule["operator"] = waterConditions[i].rules[j].op;
        rule["value"] = waterConditions[i].rules[j].value;
      }
    }

    if (strcmp(waterConditions[i].type, "schedule") == 0) {
      JsonObject sched = obj.createNestedObject("schedule");
      JsonArray times = sched.createNestedArray("times");
      for (int t = 0; t < waterConditions[i].schedule.timeCount; t++) {
        char timeBuffer[6];
        snprintf(
          timeBuffer,
          sizeof(timeBuffer),
          "%02d:%02d",
          waterConditions[i].schedule.hours[t],
          waterConditions[i].schedule.minutes[t]
        );
        times.add(timeBuffer);
      }
      if (waterConditions[i].schedule.timeCount > 0) {
        char primaryTime[6];
        snprintf(
          primaryTime,
          sizeof(primaryTime),
          "%02d:%02d",
          waterConditions[i].schedule.hours[0],
          waterConditions[i].schedule.minutes[0]
        );
        sched["time"] = primaryTime;
      }
      JsonArray days = sched.createNestedArray("days");
      for (int d = 0; d < 7; d++) {
        if (waterConditions[i].schedule.days[d]) days.add(d);
      }
    }
  }

  String json;
  serializeJson(doc, json);

  preferences.begin("conditions", false);
  preferences.putString("data", json);
  preferences.end();

  printSuccess("Условия сохранены в NVS (" + String(conditionCount) + " шт.)");
}

void loadConditionsFromNVS() {
  preferences.begin("conditions", true);
  String json = preferences.getString("data", "[]");
  preferences.end();

  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    printError("Ошибка загрузки условий: " + String(error.c_str()));
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  conditionCount = 0;

  for (JsonObject obj : arr) {
    if (conditionCount >= MAX_CONDITIONS) break;

    WateringCondition& c = waterConditions[conditionCount];
    c.plantIndex = obj["plantIndex"] | 1;
    strlcpy(c.type, obj["type"] | "sensor", sizeof(c.type));
    c.level = obj["level"] | 5;
    c.interval = obj["interval"] | 60;
    c.enabled = obj["enabled"] | true;
    c.lastTriggered = 0;
    c.lastScheduleTriggerKey = -1;
    c.ruleCount = 0;

    resetSchedule(c.schedule);

    if (obj.containsKey("rules")) {
      JsonArray rules = obj["rules"].as<JsonArray>();
      for (JsonObject rule : rules) {
        if (c.ruleCount >= 4) break;
        strlcpy(c.rules[c.ruleCount].field, rule["field"] | "", sizeof(c.rules[c.ruleCount].field));
        strlcpy(c.rules[c.ruleCount].op, rule["operator"] | "lt", sizeof(c.rules[c.ruleCount].op));
        c.rules[c.ruleCount].value = rule["value"] | 0.0f;
        c.ruleCount++;
      }
    }

    if (strcmp(c.type, "schedule") == 0 && obj.containsKey("schedule")) {
      JsonObject sched = obj["schedule"].as<JsonObject>();
      loadScheduleFromJson(sched, c.schedule);
    }

    conditionCount++;
  }

  printSuccess("Загружено условий: " + String(conditionCount));
}

float getSensorValue(int plantIndex, const char* field) {
  if (strcmp(field, "temperature") == 0) return getTemperature(plantIndex);
  if (strcmp(field, "airHumidity") == 0) return getAirHumidity(plantIndex);
  if (strcmp(field, "soilMoisture") == 0) return getSoilMoisture(plantIndex);
  return 0;
}

bool evaluateRule(const SensorRule& rule, int plantIndex) {
  float current = getSensorValue(plantIndex, rule.field);
  if (strcmp(rule.op, "lt") == 0) return current < rule.value;
  if (strcmp(rule.op, "gt") == 0) return current > rule.value;
  if (strcmp(rule.op, "eq") == 0) return abs(current - rule.value) < 0.5;
  return false;
}

void publishWateringEvent(int plantIndex, int level, const char* source) {
  if (!mqttClient.connected()) {
    printWarning("Полив выполнен, но MQTT недоступен: событие не отправлено");
    return;
  }

  String deviceId = String((uint32_t)ESP.getEfuseMac(), HEX);
  String topic = "devices/" + deviceId + "/watering";

  StaticJsonDocument<256> doc;
  doc["deviceId"] = deviceId;
  doc["plantIndex"] = plantIndex;
  doc["level"] = level;
  doc["source"] = source;
  doc["timestamp"] = millis();

  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(topic.c_str(), payload.c_str());
}

void performWatering(int plantIndex, int level, const char* source) {
  startWateringAnimation(plantIndex, level);
  publishWateringEvent(plantIndex, level, source);

  String sourceLabel = "manual";
  if (strcmp(source, "condition_sensor") == 0) {
    sourceLabel = "auto:sensor";
  } else if (strcmp(source, "condition_schedule") == 0) {
    sourceLabel = "auto:schedule";
  }

  printSuccess(
    String("[MOCK] Полив растения ") + String(plantIndex) +
    ", уровень: " + String(level) +
    ", источник: " + sourceLabel
  );
}

void triggerWatering(int plantIndex, int level) {
  performWatering(plantIndex, level, "condition_sensor");
}

void checkConditions() {
  unsigned long now = millis();
  struct tm timeinfo;
  bool hasLocalTime = getCurrentLocalTimeInfo(&timeinfo);

  for (int i = 0; i < conditionCount; i++) {
    WateringCondition& c = waterConditions[i];
    if (!c.enabled) continue;

    if (strcmp(c.type, "sensor") == 0) {
      // Check interval
      if (c.lastTriggered > 0 && (now - c.lastTriggered) < (unsigned long)c.interval * 60000UL) {
        continue;
      }

      // Evaluate all rules (AND logic)
      bool allMatch = true;
      for (int j = 0; j < c.ruleCount; j++) {
        if (!evaluateRule(c.rules[j], c.plantIndex)) {
          allMatch = false;
          break;
        }
      }

      if (allMatch && c.ruleCount > 0) {
        printDebug("Условие #" + String(i) + " сработало (датчик)");
        triggerWatering(c.plantIndex, c.level);
        c.lastTriggered = now;
      }
    } else if (strcmp(c.type, "schedule") == 0) {
      if (!hasLocalTime) {
        continue;
      }

      if (!c.schedule.days[timeinfo.tm_wday]) {
        continue;
      }

      bool rulesMatch = true;
      for (int j = 0; j < c.ruleCount; j++) {
        if (!evaluateRule(c.rules[j], c.plantIndex)) {
          rulesMatch = false;
          break;
        }
      }

      if (!rulesMatch) {
        continue;
      }

      for (int t = 0; t < c.schedule.timeCount; t++) {
        int scheduledHour = c.schedule.hours[t];
        int scheduledMinute = c.schedule.minutes[t];

        if (timeinfo.tm_hour != scheduledHour || timeinfo.tm_min != scheduledMinute) {
          continue;
        }

        long triggerKey = buildScheduleTriggerKey(timeinfo, scheduledHour, scheduledMinute);
        if (c.lastScheduleTriggerKey == triggerKey) {
          continue;
        }

        printDebug("Условие #" + String(i) + " сработало (расписание)");
        performWatering(c.plantIndex, c.level, "condition_schedule");
        c.lastTriggered = now;
        c.lastScheduleTriggerKey = triggerKey;
        break;
      }
    }
  }
}

// ===== Sensors (mock) =====

int getSoilSensorIndex(int plantIndex) {
  if (plantIndex >= 1 && plantIndex <= 2) {
    return plantIndex - 1;
  }

  if (plantIndex >= 0 && plantIndex < 2) {
    return plantIndex;
  }

  return 0;
}

int readSoilMoistureRaw(int plantIndex) {
  int sensorIndex = getSoilSensorIndex(plantIndex);
  int pin = SOIL_SENSOR_PINS[sensorIndex];
  long total = 0;

  for (int i = 0; i < SOIL_SAMPLE_COUNT; i++) {
    total += analogRead(pin);
    delay(5);
  }

  return (int)(total / SOIL_SAMPLE_COUNT);
}

void updateSoilReadings(bool force = false) {
  unsigned long now = millis();

  if (!force && (now - lastSoilRead) < SOIL_READ_INTERVAL) {
    return;
  }

  lastSoilRead = now;

  for (int i = 0; i < 2; i++) {
    int rawValue = readSoilMoistureRaw(i + 1);
    lastSoilRawValues[i] = rawValue;
    lastSoilMoisture[i] = convertSoilRawToPercent(rawValue, i + 1);
  }

  hasValidSoilReadings = true;
}

float convertSoilRawToPercent(int rawValue, int plantIndex) {
  int sensorIndex = getSoilSensorIndex(plantIndex);
  int dryValue = SOIL_DRY_VALUES[sensorIndex];
  int wetValue = SOIL_WET_VALUES[sensorIndex];

  if (dryValue == wetValue) {
    return 0.0f;
  }

  float percent = ((float)(dryValue - rawValue) * 100.0f) / (float)(dryValue - wetValue);

  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;

  return percent;
}

void updateAirSensorReadings(bool force = false) {
  unsigned long now = millis();

  if (!force && (now - lastDhtRead) < DHT_READ_INTERVAL) {
    return;
  }

  lastDhtRead = now;

  TempAndHumidity data = dhtSensor.getTempAndHumidity();

  if (isnan(data.temperature) || isnan(data.humidity)) {
    if ((now - lastDhtErrorLog) > 30000) {
      lastDhtErrorLog = now;
      printWarning("Не удалось прочитать DHT22, используются последние корректные значения");
    }
    return;
  }

  lastAirTemperature = data.temperature;
  lastAirHumidity = data.humidity;
  hasValidDhtReading = true;
}

float getTemperature(int plantIndex) {
  (void)plantIndex;
  updateAirSensorReadings();
  return hasValidDhtReading ? lastAirTemperature : 22.0f;
}

float getAirHumidity(int plantIndex) {
  (void)plantIndex;
  updateAirSensorReadings();
  return hasValidDhtReading ? lastAirHumidity : 50.0f;
}

float getSoilMoisture(int plantIndex) {
  int sensorIndex = getSoilSensorIndex(plantIndex);
  updateSoilReadings();

  if (!hasValidSoilReadings) {
    return 0.0f;
  }

  return lastSoilMoisture[sensorIndex];
}

// ===== Command Handlers =====
// Чтобы добавить новую команду:
// 1. Добавить функцию handleCmd_<имя>(JsonObject& payload)
// 2. Зарегистрировать в registerCommands()

struct CommandEntry {
  const char* type;
  void (*handler)(JsonObject& payload);
};

const int MAX_COMMANDS = 16;
CommandEntry commandRegistry[MAX_COMMANDS];
int commandCount = 0;

void registerCommand(const char* type, void (*handler)(JsonObject& payload)) {
  if (commandCount < MAX_COMMANDS) {
    commandRegistry[commandCount].type = type;
    commandRegistry[commandCount].handler = handler;
    commandCount++;
    printDebug("Команда зарегистрирована: " + String(type));
  }
}

void handleCmd_waterPlant1(JsonObject& payload) {
  int level = payload["level"] | 5;
  if (level < 1) level = 1;
  if (level > 10) level = 10;
  performWatering(1, level, "manual");
}

void handleCmd_waterPlant2(JsonObject& payload) {
  int level = payload["level"] | 5;
  if (level < 1) level = 1;
  if (level > 10) level = 10;
  performWatering(2, level, "manual");
}

void handleCmd_setTelemetryInterval(JsonObject& payload) {
  int minutes = payload["minutes"] | DEFAULT_TELEMETRY_INTERVAL_MINUTES;
  if (minutes < 5) minutes = 5;
  if (minutes > 60) minutes = 60;

  telemetryIntervalMs = (unsigned long)minutes * 60000UL;

  preferences.begin("wifi", false);
  preferences.putUInt("telemetryMin", minutes);
  preferences.end();

  printSuccess("Интервал телеметрии обновлен: " + String(minutes) + " мин");
}

void handleCmd_setConditions(JsonObject& payload) {
  if (!payload.containsKey("conditions")) {
    printError("set_conditions: нет поля conditions");
    return;
  }

  JsonArray arr = payload["conditions"].as<JsonArray>();
  conditionCount = 0;

  for (JsonObject obj : arr) {
    if (conditionCount >= MAX_CONDITIONS) break;

    WateringCondition& c = waterConditions[conditionCount];
    c.plantIndex = obj["plantIndex"] | 1;
    strlcpy(c.type, obj["type"] | "sensor", sizeof(c.type));
    c.level = obj["level"] | 5;
    c.interval = obj["interval"] | 60;
    c.enabled = obj["enabled"] | true;
    c.lastTriggered = 0;
    c.lastScheduleTriggerKey = -1;
    c.ruleCount = 0;

    resetSchedule(c.schedule);

    if (obj.containsKey("rules")) {
      JsonArray rules = obj["rules"].as<JsonArray>();
      for (JsonObject rule : rules) {
        if (c.ruleCount >= 4) break;
        strlcpy(c.rules[c.ruleCount].field, rule["field"] | "", sizeof(c.rules[c.ruleCount].field));
        strlcpy(c.rules[c.ruleCount].op, rule["operator"] | "lt", sizeof(c.rules[c.ruleCount].op));
        c.rules[c.ruleCount].value = rule["value"] | 0.0f;
        c.ruleCount++;
      }
    }

    if (strcmp(c.type, "schedule") == 0 && obj.containsKey("schedule")) {
      JsonObject sched = obj["schedule"].as<JsonObject>();
      loadScheduleFromJson(sched, c.schedule);
    }

    conditionCount++;
  }

  saveConditionsToNVS();
  printSuccess("Получено условий: " + String(conditionCount));
}

void handleCmd_deviceReset(JsonObject& payload) {
  printWarning("Выполняется полный сброс устройства...");

  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();

  printSuccess("Настройки очищены. Перезагрузка...");
  delay(1000);
  ESP.restart();
}

void registerCommands() {
  registerCommand("water_plant_1", handleCmd_waterPlant1);
  registerCommand("water_plant_2", handleCmd_waterPlant2);
  registerCommand("set_telemetry_interval", handleCmd_setTelemetryInterval);
  registerCommand("device_reset", handleCmd_deviceReset);
  registerCommand("set_conditions", handleCmd_setConditions);
}

void executeCommand(const String& type, JsonObject& payload) {
  for (int i = 0; i < commandCount; i++) {
    if (type == commandRegistry[i].type) {
      printDebug("Выполнение команды: " + type);
      commandRegistry[i].handler(payload);
      return;
    }
  }
  printWarning("Неизвестная команда: " + type);
}

// ===== MQTT =====

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  printDebug("MQTT сообщение [" + String(topic) + "]: " + message);

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    printError("Ошибка парсинга MQTT сообщения: " + String(error.c_str()));
    return;
  }

  String commandType = doc["type"].as<String>();
  JsonObject commandPayload = doc["payload"].as<JsonObject>();

  executeCommand(commandType, commandPayload);
}

void sendTelemetry() {
  if (!mqttClient.connected()) return;

  String deviceId = String((uint32_t)ESP.getEfuseMac(), HEX);
  String topic = "devices/" + deviceId + "/telemetry";

  StaticJsonDocument<512> doc;
  doc["deviceId"] = deviceId;
  doc["timestamp"] = millis();

  JsonArray plants = doc.createNestedArray("plants");

  for (int i = 0; i < 2; i++) {
    JsonObject plant = plants.createNestedObject();
    plant["index"] = i + 1;
    plant["temperature"] = getTemperature(i + 1);
    plant["airHumidity"] = getAirHumidity(i + 1);
    plant["soilMoisture"] = getSoilMoisture(i + 1);
  }

  String payload;
  serializeJson(doc, payload);

  mqttClient.publish(topic.c_str(), payload.c_str());
  printDebug("Телеметрия отправлена");
}

void connectMqtt() {
  if (savedDeviceSecret.length() == 0) {
    return;
  }

  String deviceId = String((uint32_t)ESP.getEfuseMac(), HEX);
  String clientId = "esp32-" + deviceId;
  String commandsTopic = "devices/" + deviceId + "/commands";

  printDebug("Подключение к MQTT: " + savedMqttServer);
  setDisplayState(DISPLAY_MQTT_CONNECTING, "Подключение", "восстанавливаем связь");

  if (mqttClient.connect(clientId.c_str())) {
    printSuccess("MQTT подключен!");

    mqttClient.subscribe(commandsTopic.c_str(), 1);
    printSuccess("Подписка на топик: " + commandsTopic);
    setDisplayState(DISPLAY_NORMAL, "", "");
  } else {
    printError("MQTT ошибка подключения, rc=" + String(mqttClient.state()));
  }
}

// ===== Backend =====

BackendResponse parseBackendResponse(const String& jsonResponse) {
  printDebug("Парсинг ответа от бэкенда");
  printJson("Получен JSON", jsonResponse);

  BackendResponse response;
  response.success = false;

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, jsonResponse);

  if (error) {
    response.error = "Failed to parse response: " + String(error.c_str());
    printError("Ошибка парсинга JSON: " + String(error.c_str()));
    return response;
  }

  bool state = doc["state"] | false;

  if (state) {
    if (doc["data"].is<JsonObject>()) {
      response.success = true;
      response.deviceSecret = doc["data"]["deviceSecret"].as<String>();
      printSuccess("Получен deviceSecret");
    } else {
      response.success = true;
    }
  } else if (doc["error"].is<JsonObject>()) {
    response.error = doc["error"]["message"].as<String>();
    printError("Ошибка от бэкенда: " + response.error);
  }

  return response;
}

BackendResponse registerDeviceWithBackend(const String& deviceId, const String& token) {
  printDebug("Регистрация устройства на бэкенде");

  BackendResponse response;
  response.success = false;

  String url = savedBackendUrl + "/devices/register";
  printDebug("URL запроса: " + url);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + token);

  StaticJsonDocument<200> doc;
  doc["deviceId"] = deviceId;

  String requestBody;
  serializeJson(doc, requestBody);

  int httpCode = http.POST(requestBody);
  printDebug("HTTP код ответа: " + String(httpCode));

  if (httpCode == 200) {
    String responseBody = http.getString();
    response = parseBackendResponse(responseBody);
  } else {
    response.error = "HTTP Error: " + String(httpCode);
    printError(response.error);
  }

  http.end();
  return response;
}

// ===== HTTP Responses =====

void sendSuccessResponse(const JsonDocument& data) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");

  StaticJsonDocument<512> responseDoc;
  responseDoc["state"] = true;
  responseDoc["data"] = data.as<JsonVariantConst>();

  String response;
  serializeJson(responseDoc, response);
  server.send(200, "application/json", response);
}

void sendSuccessMessage(const String& message) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");

  StaticJsonDocument<256> responseDoc;
  responseDoc["state"] = true;
  responseDoc["data"] = message;

  String response;
  serializeJson(responseDoc, response);
  server.send(200, "application/json", response);
}

void sendErrorResponse(const String& message, const String& code = "") {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");

  StaticJsonDocument<256> responseDoc;
  responseDoc["state"] = false;

  JsonObject error = responseDoc.createNestedObject("error");
  error["message"] = message;
  if (code.length() > 0) {
    error["code"] = code;
  }

  String response;
  serializeJson(responseDoc, response);
  server.send(200, "application/json", response);
}

void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(200);
}

// ===== Access Point =====

void startAccessPoint() {
  printSuccess("Запуск точки доступа...");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID);

  IPAddress apIP = WiFi.softAPIP();
  printSuccess("IP точки доступа: " + apIP.toString());
  setDisplayState(DISPLAY_WIFI_SETUP, "Режим настройки", "откройте сеть устройства");

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/status", HTTP_GET, []() {
    StaticJsonDocument<256> dataDoc;
    dataDoc["deviceId"] = String((uint32_t)ESP.getEfuseMac(), HEX);
    dataDoc["configured"] = (savedSSID.length() > 0);
    dataDoc["mode"] = "config";
    dataDoc["ssid"] = savedSSID;
    dataDoc["backendUrl"] = savedBackendUrl;
    dataDoc["mqttServer"] = savedMqttServer;

    sendSuccessResponse(dataDoc);
  });

  server.on("/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks();

    if (n == WIFI_SCAN_FAILED) {
      sendErrorResponse("Ошибка сканирования сетей", "SCAN_FAILED");
      return;
    }

    StaticJsonDocument<1024> dataDoc;
    JsonArray networks = dataDoc.createNestedArray("networks");

    for (int i = 0; i < n; ++i) {
      JsonObject net = networks.createNestedObject();
      net["ssid"] = WiFi.SSID(i);
      net["rssi"] = WiFi.RSSI(i);
      net["encrypted"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }

    dataDoc["count"] = n;
    sendSuccessResponse(dataDoc);
  });

  server.on("/configure", HTTP_POST, []() {
    printDebug("Получен POST /configure");

    if (!server.hasArg("plain")) {
      sendErrorResponse("Нет данных", "NO_DATA");
      return;
    }

    String body = server.arg("plain");

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
      sendErrorResponse("Ошибка парсинга JSON", "PARSE_ERROR");
      return;
    }

    const char* ssid = doc["ssid"];
    const char* password = doc["password"];
    const char* token = doc["token"];
    String backendUrl = doc["backendUrl"] | DEFAULT_BACKEND_URL;
    String mqttServer = doc["mqttServer"] | "";

    if (!ssid || !password || !token) {
      sendErrorResponse("SSID, пароль и токен обязательны", "MISSING_FIELDS");
      return;
    }

    if (mqttServer.length() == 0) {
      mqttServer = extractHostFromUrl(backendUrl);
    }

    printDebug("Сохранение настроек WiFi");

    // Сохраняем настройки, но НЕ регистрируемся на бэкенде
    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putString("token", token);
    preferences.putString("backendUrl", backendUrl);
    preferences.putString("mqttServer", mqttServer);
    preferences.end();

    savedSSID = ssid;
    savedPassword = password;
    savedToken = token;
    savedBackendUrl = backendUrl;
    savedMqttServer = mqttServer;

    sendSuccessMessage("Настройки сохранены. Устройство перезагружается...");
    shouldSaveConfig = true;
  });

  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      handleOptions();
    } else {
      server.send(404, "application/json", "{\"state\":false,\"error\":{\"message\":\"Not found\"}}");
    }
  });

  server.begin();
  configMode = true;
  printSuccess("Сервер конфигурации запущен");
}

void clearWiFiSettings() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();

  savedSSID = "";
  savedPassword = "";
  savedToken = "";
  savedDeviceSecret = "";
  savedBackendUrl = DEFAULT_BACKEND_URL;
  savedMqttServer = DEFAULT_BACKEND_HOST;

  printSuccess("WiFi настройки очищены");
}

// ===== Setup & Loop =====

void setup() {
  Serial.begin(115200);
  delay(1000);
  randomSeed(analogRead(0));
  Wire.begin(DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
  oled.begin();
  oled.setContrast(180);
  setDisplayState(DISPLAY_BOOT, "usePlant", "запуск");
  updateDisplay(true);

  Serial.println();
  Serial.println("=================================");
  Serial.println("🌱 PlantWatering ESP32");
  Serial.println("=================================");

  String deviceId = String((uint32_t)ESP.getEfuseMac(), HEX);
  printSuccess("Device ID: " + deviceId);
  dhtSensor.setup(DHT_PIN, DHTesp::DHT22);
  updateAirSensorReadings(true);
  printSuccess("DHT22 инициализирован на пине D4/GPIO4");
  pinMode(SOIL_SENSOR_PINS[0], INPUT);
  pinMode(SOIL_SENSOR_PINS[1], INPUT);
  updateSoilReadings(true);
  printSuccess("Датчики влажности почвы инициализированы на D34 и D35");
  printSuccess("OLED инициализирован на D21/D22 (I2C)");
  updateDisplay(true);

  registerCommands();
  loadConditionsFromNVS();

  // Загружаем настройки
  preferences.begin("wifi", false);
  savedSSID = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  savedToken = preferences.getString("token", "");
  savedDeviceSecret = preferences.getString("deviceSecret", "");
  savedBackendUrl = preferences.getString("backendUrl", DEFAULT_BACKEND_URL);
  savedMqttServer = preferences.getString("mqttServer", extractHostFromUrl(savedBackendUrl));
  unsigned int savedTelemetryIntervalMinutes =
    preferences.getUInt("telemetryMin", DEFAULT_TELEMETRY_INTERVAL_MINUTES);

  if (savedBackendUrl == LEGACY_BACKEND_URL) {
    savedBackendUrl = DEFAULT_BACKEND_URL;
    preferences.putString("backendUrl", savedBackendUrl);
  }

  if (savedMqttServer.length() == 0 || savedMqttServer == LEGACY_BACKEND_HOST) {
    savedMqttServer = extractHostFromUrl(savedBackendUrl);
    preferences.putString("mqttServer", savedMqttServer);
  }

  preferences.end();
  if (savedTelemetryIntervalMinutes < 5) savedTelemetryIntervalMinutes = 5;
  if (savedTelemetryIntervalMinutes > 60) savedTelemetryIntervalMinutes = 60;
  telemetryIntervalMs = (unsigned long)savedTelemetryIntervalMinutes * 60000UL;
  printSuccess("Интервал телеметрии: " + String(savedTelemetryIntervalMinutes) + " мин");

  if (savedSSID.length() > 0 && savedPassword.length() > 0) {
    printDebug("Попытка подключения к WiFi: " + savedSSID);
    setDisplayState(DISPLAY_WIFI_CONNECTING, "Подключение", savedSSID);
    updateDisplay(true);

    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPassword.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
      updateDisplay();
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      printSuccess("Подключено к WiFi!");
      printDebug("IP адрес: " + WiFi.localIP().toString());
      setupClock();
      printSuccess("NTP-синхронизация времени запрошена");

      // Если есть токен, но нет deviceSecret - регистрируемся
      if (savedToken.length() > 0 && savedDeviceSecret.length() == 0) {
        printDebug("Устройство не зарегистрировано. Регистрация на бэкенде...");
        setDisplayState(DISPLAY_REGISTERING, "Подготовка", "сохраняем данные");
        updateDisplay(true);

        String deviceId = String((uint32_t)ESP.getEfuseMac(), HEX);
        BackendResponse response = registerDeviceWithBackend(deviceId, savedToken);

        if (response.success) {
          printSuccess("Устройство зарегистрировано!");

          // Сохраняем deviceSecret и удаляем токен
          preferences.begin("wifi", false);
          preferences.putString("deviceSecret", response.deviceSecret);
          preferences.remove("token");
          preferences.end();

          savedDeviceSecret = response.deviceSecret;
          savedToken = "";
        } else {
          printError("Ошибка регистрации: " + response.error);
          setDisplayState(DISPLAY_ERROR, "Ошибка", "проверьте приложение");
          updateDisplay(true);
        }
      } else if (savedDeviceSecret.length() > 0) {
        printSuccess("Устройство уже зарегистрировано");
      }

      // Настраиваем MQTT и переходим в нормальный режим
      mqttClient.setServer(savedMqttServer.c_str(), MQTT_PORT);
      mqttClient.setCallback(onMqttMessage);
      mqttClient.setBufferSize(2048);
      connectMqtt();

      configMode = false;
      printSuccess("Запуск нормального режима");
      if (mqttClient.connected()) {
        setDisplayState(DISPLAY_NORMAL, "", "");
      }
      updateDisplay(true);

    } else {
      printError("Не удалось подключиться к WiFi");
      setDisplayState(DISPLAY_ERROR, "Нет связи", "откройте настройку");
      updateDisplay(true);
      startAccessPoint();
    }
  } else {
    printDebug("Нет сохраненных настроек WiFi");
    startAccessPoint();
  }
}

void loop() {
  if (configMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    updateDisplay();

    if (shouldSaveConfig) {
      printSuccess("Перезагрузка для применения настроек...");
      delay(2000);
      ESP.restart();
    }
  } else {
    // Нормальный режим работы
    if (WiFi.status() != WL_CONNECTED) {
      printError("Потеряно соединение с WiFi!");
      startAccessPoint();
      updateDisplay(true);
      return;
    }

    // MQTT reconnect
    if (!mqttClient.connected()) {
      setDisplayState(DISPLAY_MQTT_CONNECTING, "Подключение", "восстанавливаем связь");
      unsigned long now = millis();
      if (now - lastMqttReconnect > MQTT_RECONNECT_INTERVAL) {
        lastMqttReconnect = now;
        connectMqtt();
      }
    } else if (currentDisplayState != DISPLAY_NORMAL) {
      setDisplayState(DISPLAY_NORMAL, "", "");
    }

    mqttClient.loop();

    // Отправка телеметрии по настроенному интервалу
    unsigned long now2 = millis();
    if (now2 - lastTelemetrySend > telemetryIntervalMs) {
      lastTelemetrySend = now2;
      sendTelemetry();
    }

    // Проверка условий полива
    if (now2 - lastConditionCheck > CONDITION_CHECK_INTERVAL) {
      lastConditionCheck = now2;
      checkConditions();
    }

    updateDisplay();
  }

  delay(10);
}
