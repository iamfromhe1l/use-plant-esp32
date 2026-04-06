#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PubSubClient.h>

const char* apSSID = "PlantWatering-ESP32";
const char* apPassword = NULL;
const int DNS_PORT = 53;
const int HTTP_PORT = 80;

// MQTT настройки
const char* MQTT_SERVER = "192.168.0.105";
const int MQTT_PORT = 1883;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;

DNSServer dnsServer;
WebServer server(HTTP_PORT);
Preferences preferences;
HTTPClient http;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

String savedSSID = "";
String savedPassword = "";
String savedToken = "";
String savedDeviceSecret = "";
String savedBackendUrl = "http://192.168.0.105:4000";
bool shouldSaveConfig = false;
bool configMode = false;
unsigned long lastMqttReconnect = 0;
unsigned long lastTelemetrySend = 0;
const unsigned long TELEMETRY_INTERVAL = 60000; // 1 минута

// ===== Structs =====

struct SensorRule {
  char field[16];     // "temperature", "airHumidity", "soilMoisture"
  char op[4];         // "eq", "gt", "lt"
  float value;
};

struct Schedule {
  int hour;
  int minute;
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
};

struct BackendResponse {
  bool success;
  String deviceSecret;
  String error;
};

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
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < conditionCount; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["plantIndex"] = waterConditions[i].plantIndex;
    obj["type"] = waterConditions[i].type;
    obj["level"] = waterConditions[i].level;
    obj["interval"] = waterConditions[i].interval;
    obj["enabled"] = waterConditions[i].enabled;

    if (strcmp(waterConditions[i].type, "sensor") == 0) {
      JsonArray rules = obj.createNestedArray("rules");
      for (int j = 0; j < waterConditions[i].ruleCount; j++) {
        JsonObject rule = rules.createNestedObject();
        rule["field"] = waterConditions[i].rules[j].field;
        rule["operator"] = waterConditions[i].rules[j].op;
        rule["value"] = waterConditions[i].rules[j].value;
      }
    } else {
      JsonObject sched = obj.createNestedObject("schedule");
      sched["time"] = String(waterConditions[i].schedule.hour) + ":" +
                      (waterConditions[i].schedule.minute < 10 ? "0" : "") +
                      String(waterConditions[i].schedule.minute);
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

  StaticJsonDocument<2048> doc;
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
    c.ruleCount = 0;

    if (strcmp(c.type, "sensor") == 0 && obj.containsKey("rules")) {
      JsonArray rules = obj["rules"].as<JsonArray>();
      for (JsonObject rule : rules) {
        if (c.ruleCount >= 4) break;
        strlcpy(c.rules[c.ruleCount].field, rule["field"] | "", sizeof(c.rules[c.ruleCount].field));
        strlcpy(c.rules[c.ruleCount].op, rule["operator"] | "lt", sizeof(c.rules[c.ruleCount].op));
        c.rules[c.ruleCount].value = rule["value"] | 0.0f;
        c.ruleCount++;
      }
    } else if (strcmp(c.type, "schedule") == 0 && obj.containsKey("schedule")) {
      JsonObject sched = obj["schedule"].as<JsonObject>();
      String timeStr = sched["time"] | "08:00";
      int colonIdx = timeStr.indexOf(':');
      c.schedule.hour = timeStr.substring(0, colonIdx).toInt();
      c.schedule.minute = timeStr.substring(colonIdx + 1).toInt();
      memset(c.schedule.days, 0, sizeof(c.schedule.days));
      JsonArray days = sched["days"].as<JsonArray>();
      for (int d : days) {
        if (d >= 0 && d < 7) c.schedule.days[d] = true;
      }
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

void triggerWatering(int plantIndex, int level) {
  if (plantIndex == 1) {
    printSuccess("[MOCK] Авто-полив растения 1, уровень: " + String(level));
  } else {
    printSuccess("[MOCK] Авто-полив растения 2, уровень: " + String(level));
  }
}

void checkConditions() {
  unsigned long now = millis();

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
      // For schedule, we use millis-based rough time tracking
      // In a real implementation, use NTP or RTC
      // For now, check if we haven't triggered in the last interval
      if (c.lastTriggered > 0 && (now - c.lastTriggered) < 60000UL) {
        continue; // Don't trigger more than once per minute
      }

      // Without RTC, schedule conditions trigger based on stored time
      // This is a simplified version - with NTP sync it would check actual time
      // For MVP: just log that schedule would trigger
      // TODO: Add NTP time sync for accurate schedule checking
    }
  }
}

// ===== Sensors (mock) =====

float getTemperature(int plantIndex) {
  return 18.0 + (float)random(0, 120) / 10.0;
}

float getAirHumidity(int plantIndex) {
  return 40.0 + (float)random(0, 400) / 10.0;
}

float getSoilMoisture(int plantIndex) {
  return 10.0 + (float)random(0, 800) / 10.0;
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
  printSuccess("[MOCK] Полив растения 1, уровень: " + String(level));
}

void handleCmd_waterPlant2(JsonObject& payload) {
  int level = payload["level"] | 5;
  if (level < 1) level = 1;
  if (level > 10) level = 10;
  printSuccess("[MOCK] Полив растения 2, уровень: " + String(level));
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
    c.ruleCount = 0;

    if (strcmp(c.type, "sensor") == 0 && obj.containsKey("rules")) {
      JsonArray rules = obj["rules"].as<JsonArray>();
      for (JsonObject rule : rules) {
        if (c.ruleCount >= 4) break;
        strlcpy(c.rules[c.ruleCount].field, rule["field"] | "", sizeof(c.rules[c.ruleCount].field));
        strlcpy(c.rules[c.ruleCount].op, rule["operator"] | "lt", sizeof(c.rules[c.ruleCount].op));
        c.rules[c.ruleCount].value = rule["value"] | 0.0f;
        c.ruleCount++;
      }
    } else if (strcmp(c.type, "schedule") == 0 && obj.containsKey("schedule")) {
      JsonObject sched = obj["schedule"].as<JsonObject>();
      String timeStr = sched["time"] | "08:00";
      int colonIdx = timeStr.indexOf(':');
      c.schedule.hour = timeStr.substring(0, colonIdx).toInt();
      c.schedule.minute = timeStr.substring(colonIdx + 1).toInt();
      memset(c.schedule.days, 0, sizeof(c.schedule.days));
      JsonArray days = sched["days"].as<JsonArray>();
      for (int d : days) {
        if (d >= 0 && d < 7) c.schedule.days[d] = true;
      }
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
    plant["temperature"] = getTemperature(i);
    plant["airHumidity"] = getAirHumidity(i);
    plant["soilMoisture"] = getSoilMoisture(i);
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

  printDebug("Подключение к MQTT: " + String(MQTT_SERVER));

  if (mqttClient.connect(clientId.c_str())) {
    printSuccess("MQTT подключен!");

    mqttClient.subscribe(commandsTopic.c_str(), 1);
    printSuccess("Подписка на топик: " + commandsTopic);
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

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/status", HTTP_GET, []() {
    StaticJsonDocument<256> dataDoc;
    dataDoc["deviceId"] = String((uint32_t)ESP.getEfuseMac(), HEX);
    dataDoc["configured"] = (savedSSID.length() > 0);
    dataDoc["mode"] = "config";
    dataDoc["ssid"] = savedSSID;

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

    if (!ssid || !password || !token) {
      sendErrorResponse("SSID, пароль и токен обязательны", "MISSING_FIELDS");
      return;
    }

    printDebug("Сохранение настроек WiFi");

    // Сохраняем настройки, но НЕ регистрируемся на бэкенде
    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putString("token", token);
    preferences.end();

    savedSSID = ssid;
    savedPassword = password;
    savedToken = token;

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

  printSuccess("WiFi настройки очищены");
}

// ===== Setup & Loop =====

void setup() {
  Serial.begin(115200);
  delay(1000);
  randomSeed(analogRead(0));

  Serial.println();
  Serial.println("=================================");
  Serial.println("🌱 PlantWatering ESP32");
  Serial.println("=================================");

  String deviceId = String((uint32_t)ESP.getEfuseMac(), HEX);
  printSuccess("Device ID: " + deviceId);

  registerCommands();
  loadConditionsFromNVS();

  // Загружаем настройки
  preferences.begin("wifi", false);
  savedSSID = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  savedToken = preferences.getString("token", "");
  savedDeviceSecret = preferences.getString("deviceSecret", "");
  preferences.end();

  if (savedSSID.length() > 0 && savedPassword.length() > 0) {
    printDebug("Попытка подключения к WiFi: " + savedSSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPassword.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      printSuccess("Подключено к WiFi!");
      printDebug("IP адрес: " + WiFi.localIP().toString());

      // Если есть токен, но нет deviceSecret - регистрируемся
      if (savedToken.length() > 0 && savedDeviceSecret.length() == 0) {
        printDebug("Устройство не зарегистрировано. Регистрация на бэкенде...");

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
        }
      } else if (savedDeviceSecret.length() > 0) {
        printSuccess("Устройство уже зарегистрировано");
      }

      // Настраиваем MQTT и переходим в нормальный режим
      mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
      mqttClient.setCallback(onMqttMessage);
      mqttClient.setBufferSize(2048);
      connectMqtt();

      configMode = false;
      printSuccess("Запуск нормального режима");

    } else {
      printError("Не удалось подключиться к WiFi");
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
      return;
    }

    // MQTT reconnect
    if (!mqttClient.connected()) {
      unsigned long now = millis();
      if (now - lastMqttReconnect > MQTT_RECONNECT_INTERVAL) {
        lastMqttReconnect = now;
        connectMqtt();
      }
    }

    mqttClient.loop();

    // Отправка телеметрии каждые 5 секунд
    unsigned long now2 = millis();
    if (now2 - lastTelemetrySend > TELEMETRY_INTERVAL) {
      lastTelemetrySend = now2;
      sendTelemetry();
    }

    // Проверка условий полива
    if (now2 - lastConditionCheck > CONDITION_CHECK_INTERVAL) {
      lastConditionCheck = now2;
      checkConditions();
    }
  }

  delay(10);
}
