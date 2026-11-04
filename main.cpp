#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include "config.h"

#if defined(__AVR)
  #define MAX_DISPLAY_BUFFER_SIZE 800
#else
  #define MAX_DISPLAY_BUFFER_SIZE 32768
#endif
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8) ? EPD::HEIGHT : MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8))

GxEPD2_BW<GxEPD2_290_T94_V2, MAX_HEIGHT(GxEPD2_290_T94_V2)> display(
  GxEPD2_290_T94_V2(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN));

WebServer server(80);
WebSocketsServer webSocket(81);

struct Todo {
  bool inUse = false;
  uint32_t id = 0;
  String text;
  time_t createdAt = 0;
  bool completed = false;
  time_t completedAt = 0;
};

Todo todos[MAX_TODOS];
uint32_t nextTodoId = 1;

const char* defaultQuotes[] = {
  "Small steps every day.",
  "Done is better than perfect.",
  "Focus on one thing now.",
  "Progress beats pressure.",
  "Start. Momentum follows."
};

bool displayDirty = true;
bool pendingWsPush = false;
bool bleClientConnected = false;

unsigned long lastClockTickMs = 0;
unsigned long lastWiFiRetryMs = 0;
unsigned long lastWsPushMs = 0;

time_t fallbackEpochBase = 1704067200; // 2024-01-01 00:00:00 UTC
unsigned long fallbackEpochSetMillis = 0;

// BLE globals
BLECharacteristic* bleStateChar = nullptr;
BLECharacteristic* bleCmdChar = nullptr;

time_t nowEpoch() {
  time_t t = time(nullptr);
  if (t > 1700000000) return t;
  return fallbackEpochBase + (millis() - fallbackEpochSetMillis) / 1000;
}

String isoTime(time_t t) {
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return String(buf);
}

String formatClock(time_t t) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &tmv);
  return String(buf);
}

String formatDate(time_t t) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[16];
  strftime(buf, sizeof(buf), "%d/%m/%Y", &tmv);
  return String(buf);
}

void markChanged() {
  displayDirty = true;
  pendingWsPush = true;
}

int activeTodoCount() {
  int c = 0;
  for (uint8_t i = 0; i < MAX_TODOS; i++) if (todos[i].inUse) c++;
  return c;
}

void cleanupExpiredTodos() {
  time_t t = nowEpoch();
  bool changed = false;
  for (uint8_t i = 0; i < MAX_TODOS; i++) {
    if (todos[i].inUse && (uint32_t)(t - todos[i].createdAt) >= TODO_EXPIRY_SECONDS) {
      todos[i].inUse = false;
      changed = true;
    }
  }
  if (changed) markChanged();
}

Todo* findTodoById(uint32_t id) {
  for (uint8_t i = 0; i < MAX_TODOS; i++) {
    if (todos[i].inUse && todos[i].id == id) return &todos[i];
  }
  return nullptr;
}

bool createTodo(const String& text, uint32_t forcedId = 0) {
  cleanupExpiredTodos();
  if (activeTodoCount() >= MAX_TODOS) return false;

  for (uint8_t i = 0; i < MAX_TODOS; i++) {
    if (!todos[i].inUse) {
      todos[i].inUse = true;
      todos[i].id = forcedId ? forcedId : nextTodoId++;
      if (forcedId && forcedId >= nextTodoId) nextTodoId = forcedId + 1;
      todos[i].text = text.length() ? text : "Untitled task";
      todos[i].createdAt = nowEpoch();
      todos[i].completed = false;
      todos[i].completedAt = 0;
      markChanged();
      return true;
    }
  }
  return false;
}

bool setTodoCompletion(uint32_t id, bool completed) {
  Todo* t = findTodoById(id);
  if (!t) return false;
  t->completed = completed;
  t->completedAt = completed ? nowEpoch() : 0;
  markChanged();
  return true;
}

bool setTodoText(uint32_t id, const String& text) {
  Todo* t = findTodoById(id);
  if (!t) return false;
  t->text = text;
  markChanged();
  return true;
}

String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o;
}

String buildTodosJson() {
  cleanupExpiredTodos();
  String out = "{\"now\":" + String((uint32_t)nowEpoch()) + ",\"todos\":[";
  bool first = true;
  for (uint8_t i = 0; i < MAX_TODOS; i++) {
    if (!todos[i].inUse) continue;
    if (!first) out += ",";
    first = false;
    out += "{";
    out += "\"id\":" + String(todos[i].id) + ",";
    out += "\"text\":\"" + jsonEscape(todos[i].text) + "\",";
    out += "\"completed\":" + String(todos[i].completed ? "true" : "false") + ",";
    out += "\"createdAt\":" + String((uint32_t)todos[i].createdAt);
    out += "}";
  }
  out += "]}";
  return out;
}

String getDefaultQuote() {
  time_t t = nowEpoch();
  uint8_t idx = (uint8_t)((t / 86400) % (sizeof(defaultQuotes) / sizeof(defaultQuotes[0])));
  return String(defaultQuotes[idx]);
}

String extractJsonString(const String& body, const String& key) {
  String pat = "\"" + key + "\"";
  int p = body.indexOf(pat);
  if (p < 0) return "";
  int colon = body.indexOf(':', p + pat.length());
  if (colon < 0) return "";
  int q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

long extractJsonInt(const String& body, const String& key, long defaultVal) {
  String pat = "\"" + key + "\"";
  int p = body.indexOf(pat);
  if (p < 0) return defaultVal;
  int colon = body.indexOf(':', p + pat.length());
  if (colon < 0) return defaultVal;
  int s = colon + 1;
  while (s < (int)body.length() && (body[s] == ' ' || body[s] == '\t')) s++;
  int e = s;
  while (e < (int)body.length() && (isDigit(body[e]) || body[e] == '-')) e++;
  if (e <= s) return defaultVal;
  return body.substring(s, e).toInt();
}

bool extractJsonBool(const String& body, const String& key, bool defaultVal) {
  String pat = "\"" + key + "\"";
  int p = body.indexOf(pat);
  if (p < 0) return defaultVal;
  int colon = body.indexOf(':', p + pat.length());
  if (colon < 0) return defaultVal;
  String tail = body.substring(colon + 1);
  tail.trim();
  if (tail.startsWith("true")) return true;
  if (tail.startsWith("false")) return false;
  return defaultVal;
}

String readBody() {
  if (server.hasArg("plain")) return server.arg("plain");
  return "";
}

void pushState() {
  String state = buildTodosJson();
  webSocket.broadcastTXT(state);
  if (bleClientConnected && bleStateChar) {
    bleStateChar->setValue(state.c_str());
    bleStateChar->notify();
  }
  pendingWsPush = false;
  lastWsPushMs = millis();
}

void drawWrappedText(int16_t x, int16_t y, int16_t maxWidth, const String& text, int16_t lineHeight) {
  String line;
  int16_t cx = x;
  int16_t cy = y;
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    String test = line + c;
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds(test, 0, 0, &tbx, &tby, &tbw, &tbh);
    if (tbw > maxWidth && line.length() > 0) {
      display.setCursor(cx, cy);
      display.print(line);
      line = c;
      cy += lineHeight;
      if (cy > 124) return;
    } else {
      line = test;
    }
  }
  if (line.length()) {
    display.setCursor(cx, cy);
    display.print(line);
  }
}

void renderDisplay() {
  cleanupExpiredTodos();
  time_t t = nowEpoch();
  String clockStr = formatClock(t);
  String dateStr = formatDate(t);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    display.setTextSize(2);
    display.setCursor(4, 20);
    display.print(clockStr);

    display.setTextSize(1);
    display.setCursor(220, 16);
    display.print(dateStr);
    display.drawLine(0, 24, 295, 24, GxEPD_BLACK);

    int count = activeTodoCount();
    if (count == 0) {
      display.setTextSize(1);
      display.setCursor(8, 44);
      display.print("No active todos");
      drawWrappedText(8, 62, 280, getDefaultQuote(), 14);
    } else {
      display.setTextSize(1);
      int y = 38;
      for (uint8_t i = 0; i < MAX_TODOS; i++) {
        if (!todos[i].inUse) continue;
        display.drawRect(4, y - 9, 10, 10, GxEPD_BLACK);
        if (todos[i].completed) {
          display.drawLine(5, y - 5, 12, y - 1, GxEPD_BLACK);
          display.drawLine(12, y - 1, 16, y - 10, GxEPD_BLACK);
        }

        String txt = todos[i].text;
        if (txt.length() > 40) txt = txt.substring(0, 40) + "...";
        display.setCursor(20, y);
        display.print(txt);

        if (todos[i].completed) {
          int16_t tbx, tby;
          uint16_t tbw, tbh;
          display.getTextBounds(txt, 20, y, &tbx, &tby, &tbw, &tbh);
          int16_t lineY = y - 4;
          display.drawLine(20, lineY, 20 + tbw, lineY, GxEPD_BLACK);
        }

        y += 13;
        if (y > 124) break;
      }
    }
  } while (display.nextPage());

  displayDirty = false;
}

void handleQuotesDefault() {
  String quote = getDefaultQuote();
  String resp = "{\"quote\":\"" + jsonEscape(quote) + "\"}";
  server.send(200, "application/json", resp);
}

void handleDataPath(const String& timeId) {
  if (server.method() == HTTP_GET) {
    String resp = "{\"timeID\":\"" + jsonEscape(timeId) + "\",\"epoch\":" + String((uint32_t)nowEpoch()) + ",\"iso\":\"" + isoTime(nowEpoch()) + "\"}";
    server.send(200, "application/json", resp);
    return;
  }

  if (server.method() == HTTP_POST || server.method() == HTTP_PUT) {
    String body = readBody();
    long epochVal = extractJsonInt(body, "epoch", 0);
    if (server.hasArg("epoch")) epochVal = server.arg("epoch").toInt();
    if (epochVal > 1700000000) {
      struct timeval tv = { (time_t)epochVal, 0 };
      settimeofday(&tv, nullptr);
      fallbackEpochBase = epochVal;
      fallbackEpochSetMillis = millis();
      markChanged();
      server.send(200, "application/json", "{\"ok\":true}");
    } else {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing or invalid epoch\"}");
    }
    return;
  }

  server.send(405, "application/json", "{\"error\":\"method not allowed\"}");
}

void handleTodosPath(const String& todosId) {
  if (server.method() == HTTP_GET) {
    uint32_t id = (uint32_t)todosId.toInt();
    Todo* t = findTodoById(id);
    if (!t) {
      server.send(404, "application/json", "{\"error\":\"todo not found\"}");
      return;
    }
    String resp = "{\"id\":" + String(t->id) + ",\"text\":\"" + jsonEscape(t->text) + "\",\"completed\":" + String(t->completed ? "true" : "false") + ",\"createdAt\":" + String((uint32_t)t->createdAt) + "}";
    server.send(200, "application/json", resp);
    return;
  }

  if (server.method() == HTTP_POST || server.method() == HTTP_PUT) {
    String body = readBody();
    String text = extractJsonString(body, "text");
    if (text.length() == 0 && server.hasArg("text")) text = server.arg("text");

    if (todosId == "new" || todosId == "0") {
      if (!createTodo(text)) {
        server.send(409, "application/json", "{\"ok\":false,\"error\":\"daily limit reached\"}");
        return;
      }
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }

    uint32_t id = (uint32_t)todosId.toInt();
    Todo* t = findTodoById(id);
    if (!t) {
      if (!createTodo(text.length() ? text : "Untitled task", id)) {
        server.send(409, "application/json", "{\"ok\":false,\"error\":\"daily limit reached\"}");
        return;
      }
      server.send(201, "application/json", "{\"ok\":true,\"created\":true}");
      return;
    }

    if (text.length()) setTodoText(id, text);
    if (body.indexOf("\"completed\"") >= 0 || server.hasArg("completed")) {
      bool done = extractJsonBool(body, "completed", false);
      if (server.hasArg("completed")) done = (server.arg("completed") == "1" || server.arg("completed") == "true");
      setTodoCompletion(id, done);
    }

    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }

  server.send(405, "application/json", "{\"error\":\"method not allowed\"}");
}

void handleTodosCompletionPath(const String& todosId) {
  uint32_t id = (uint32_t)todosId.toInt();
  Todo* t = findTodoById(id);
  if (!t) {
    server.send(404, "application/json", "{\"error\":\"todo not found\"}");
    return;
  }

  bool completed = true;
  if (server.method() == HTTP_POST || server.method() == HTTP_PUT) {
    String body = readBody();
    completed = extractJsonBool(body, "completed", true);
    if (server.hasArg("completed")) completed = (server.arg("completed") == "1" || server.arg("completed") == "true");
    setTodoCompletion(id, completed);
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (server.method() == HTTP_GET) {
    String resp = "{\"id\":" + String(id) + ",\"completed\":" + String(t->completed ? "true" : "false") + "}";
    server.send(200, "application/json", resp);
    return;
  }

  server.send(405, "application/json", "{\"error\":\"method not allowed\"}");
}

void handleNotFound() {
  String uri = server.uri();

  if (uri.startsWith("/v1/data/")) {
    handleDataPath(uri.substring(String("/v1/data/").length()));
    return;
  }

  if (uri.startsWith("/v1/todos/completion/")) {
    handleTodosCompletionPath(uri.substring(String("/v1/todos/completion/").length()));
    return;
  }

  if (uri.startsWith("/v1/todos/")) {
    handleTodosPath(uri.substring(String("/v1/todos/").length()));
    return;
  }

  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

void processCommand(const String& cmd) {
  // Supported:
  // ADD:<text>
  // DONE:<id>:<0|1>
  // TEXT:<id>:<text>
  // or JSON with keys action/id/text/completed
  if (cmd.startsWith("ADD:")) {
    createTodo(cmd.substring(4));
    return;
  }
  if (cmd.startsWith("DONE:")) {
    int p = cmd.indexOf(':', 5);
    if (p > 0) {
      uint32_t id = (uint32_t)cmd.substring(5, p).toInt();
      bool done = cmd.substring(p + 1).toInt() != 0;
      setTodoCompletion(id, done);
    }
    return;
  }
  if (cmd.startsWith("TEXT:")) {
    int p = cmd.indexOf(':', 5);
    if (p > 0) {
      uint32_t id = (uint32_t)cmd.substring(5, p).toInt();
      setTodoText(id, cmd.substring(p + 1));
    }
    return;
  }

  // Basic JSON command
  String action = extractJsonString(cmd, "action");
  if (action == "add") {
    String text = extractJsonString(cmd, "text");
    createTodo(text);
  } else if (action == "complete") {
    uint32_t id = (uint32_t)extractJsonInt(cmd, "id", 0);
    bool done = extractJsonBool(cmd, "completed", true);
    if (id) setTodoCompletion(id, done);
  } else if (action == "text") {
    uint32_t id = (uint32_t)extractJsonInt(cmd, "id", 0);
    String text = extractJsonString(cmd, "text");
    if (id) setTodoText(id, text);
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    String state = buildTodosJson();
    webSocket.sendTXT(num, state);
  } else if (type == WStype_TEXT) {
    String msg;
    msg.reserve(length);
    for (size_t i = 0; i < length; i++) msg += (char)payload[i];
    processCommand(msg);
  }
}

class WatchBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    (void)pServer;
    bleClientConnected = true;
  }
  void onDisconnect(BLEServer* pServer) override {
    (void)pServer;
    bleClientConnected = false;
    BLEDevice::startAdvertising();
  }
};

class WatchBleCmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    std::string v = characteristic->getValue();
    if (!v.empty()) {
      String cmd = String(v.c_str());
      processCommand(cmd);
    }
  }
};

void setupBLE() {
  BLEDevice::init("ESP32-ToDo-Watch");
  BLEServer* serverBle = BLEDevice::createServer();
  serverBle->setCallbacks(new WatchBleServerCallbacks());

  BLEService* service = serverBle->createService(BLE_SERVICE_UUID);

  bleStateChar = service->createCharacteristic(
    BLE_STATE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  bleStateChar->addDescriptor(new BLE2902());
  String initial = buildTodosJson();
  bleStateChar->setValue(initial.c_str());

  bleCmdChar = service->createCharacteristic(
    BLE_CMD_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  bleCmdChar->setCallbacks(new WatchBleCmdCallbacks());

  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->start();
}

void setupWiFiAndServer() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  server.on("/v1/quotes/default", HTTP_GET, handleQuotesDefault);
  server.onNotFound(handleNotFound);
  server.begin();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  fallbackEpochSetMillis = millis();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  SPI.begin(EPD_CLK_PIN, 19, EPD_DIN_PIN, EPD_CS_PIN);
  display.init(115200, true, 50, false);
  display.setRotation(1);

  setupTime();
  setupBLE();
  setupWiFiAndServer();

  createTodo("Welcome! Add your first task.");
  displayDirty = true;
}

void loop() {
  unsigned long nowMs = millis();

  if (WiFi.status() != WL_CONNECTED && nowMs - lastWiFiRetryMs >= WIFI_RETRY_MS) {
    lastWiFiRetryMs = nowMs;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  server.handleClient();
  webSocket.loop();
  cleanupExpiredTodos();

  if (nowMs - lastClockTickMs >= CLOCK_REDRAW_MS) {
    lastClockTickMs = nowMs;
    static String lastMinute;
    String m = formatClock(nowEpoch());
    if (m != lastMinute) {
      lastMinute = m;
      displayDirty = true;
    }
  }

  if (pendingWsPush && (nowMs - lastWsPushMs >= WS_PUSH_DEBOUNCE_MS)) {
    pushState();
  }

  if (displayDirty) {
    renderDisplay();
  }

  delay(5);
}
