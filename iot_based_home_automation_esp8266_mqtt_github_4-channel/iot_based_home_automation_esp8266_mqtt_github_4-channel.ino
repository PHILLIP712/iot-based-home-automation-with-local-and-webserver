#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <EEPROM.h>

#define EEPROM_SIZE 300
#define STR_MAX_LEN 32
#define NAME_MAX_LEN 20

// EEPROM Map
#define ADDR_STATES     0   // 4 bytes (0-3)
#define ADDR_LOCKS      4   // 4 bytes (4-7)
#define ADDR_NAMES      8   // 80 bytes (8-87)
#define ADDR_STA_SSID   88  // 32 bytes (88-119)
#define ADDR_STA_PASS   120 // 32 bytes (120-151)
#define ADDR_ADMIN_USER 152 // 32 bytes (152-183)
#define ADDR_ADMIN_PASS 184 // 32 bytes (184-215)
#define ADDR_GUEST_PASS 216 // 32 bytes (216-247)
#define ADDR_AP_PASS    248 // 32 bytes (248-279)

// Default Configurations
const char* default_sta_ssid   = "Infinix";
const char* default_sta_pass   = "1234567890";
const char* default_admin_user = "admin";
const char* default_admin_pass = "1234567890";
const char* default_guest_pass = "GUEST123";
const char* ap_ssid            = "Infinix-Relay-Hub";
const char* default_ap_pass    = "1234567890";

// Cloud MQTT Credentials
const char* mqtt_broker   = "broker.hivemq.com";
const int   mqtt_port     = 1883;
const char* TOPIC_COMMAND = "hub_7f3b9c2a8e/cmd";
const char* TOPIC_STATE   = "hub_7f3b9c2a8e/state";
const char* AUTH_TOKEN    = "sec_k8912xL90";

// Hardware Pin Definition (D1, D2, D7, D6)
const int relayPins[4] = {5, 4, 13, 12};
bool relayStates[4]    = {false, false, false, false};
bool childLocks[4]     = {false, false, false, false};
String relayNames[4]   = {"Light", "Fan", "TV", "Socket"};

String sta_ssid, sta_pass, admin_user, admin_pass, guest_pass, ap_pass;

#define RELAY_ON  LOW
#define RELAY_OFF HIGH

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
WiFiClient espClient;
PubSubClient mqtt(espClient);

unsigned long lastMqttRetry = 0;
bool shouldReboot = false;
unsigned long rebootTimer = 0;

// Local Embedded UI
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Hybrid Hub</title>
  <style>
    body { font-family: Arial, sans-serif; display: flex; justify-content: center; align-items: center; min-height: 90vh; background: #f0f2f5; margin: 0; padding: 1rem; }
    .card { background: #ffffff; padding: 1.8rem; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); text-align: center; width: 100%; max-width: 380px; }
    .top-bar { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1.2rem; }
    .badge { padding: 4px 8px; border-radius: 4px; font-size: 0.8rem; }
    .connected { background: #d4edda; color: #155724; }
    .disconnected { background: #f8d7da; color: #721c24; }
    .btn-icon { background: transparent; border: none; font-size: 1.2rem; cursor: pointer; text-decoration: none; }
    .master-controls { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 1.2rem; }
    .btn-master-on { background: #28a745; color: #fff; padding: 0.75rem; border-radius: 6px; font-weight: bold; border: none; cursor: pointer; }
    .btn-master-off { background: #dc3545; color: #fff; padding: 0.75rem; border-radius: 6px; font-weight: bold; border: none; cursor: pointer; }
    .relay-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
    .relay-item { background: #f8f9fa; padding: 0.9rem; border-radius: 8px; border: 1px solid #e9ecef; display: flex; flex-direction: column; align-items: center; }
    .title-wrapper { display: flex; align-items: center; justify-content: space-between; width: 100%; margin-bottom: 8px; }
    .relay-title { font-weight: bold; font-size: 0.9rem; color: #333; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 80px; }
    .btn { width: 100%; padding: 0.6rem; font-size: 0.9rem; font-weight: bold; border: none; border-radius: 6px; cursor: pointer; }
    .btn-off { background: #6c757d; color: white; }
    .btn-on { background: #007bff; color: white; }
    .btn-pending { background: #ffc107; color: #212529; cursor: not-allowed; }
    .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); justify-content: center; align-items: center; z-index: 100; }
    .modal-content { background: #fff; padding: 1.5rem; border-radius: 10px; width: 90%; max-width: 320px; text-align: left; }
    .form-group { margin-bottom: 10px; }
    .form-group label { display: block; font-size: 0.8rem; color: #555; margin-bottom: 3px; }
    .form-group input { width: 93%; padding: 8px; border: 1px solid #ccc; border-radius: 4px; font-size: 0.9rem; }
    .modal-actions { display: flex; gap: 10px; margin-top: 15px; }
    .btn-save { background: #007bff; color: #fff; flex: 1; padding: 8px; border: none; border-radius: 5px; cursor: pointer; font-weight: bold; }
    .btn-close { background: #6c757d; color: #fff; flex: 1; padding: 8px; border: none; border-radius: 5px; cursor: pointer; }
  </style>
</head>
<body>
  <div class="card">
    <div class="top-bar">
      <span id="conn-status" class="badge disconnected">Disconnected</span>
      <div>
        <a href="/update" class="btn-icon" title="OTA Firmware Update">🔄</a>
        <button class="btn-icon" onclick="openSettings()" title="Settings">⚙️</button>
      </div>
    </div>
    <h2>Smart Hub (Admin)</h2>
    <div class="master-controls">
      <button class="btn-master-on" onclick="setAll(1)">ALL ON</button>
      <button class="btn-master-off" onclick="setAll(0)">ALL OFF</button>
    </div>
    <div class="relay-grid">
      <div class="relay-item">
        <div class="title-wrapper">
          <span id="title-0" class="relay-title">Relay 1</span>
          <button id="lock-0" class="btn-icon" style="font-size:0.9rem;" onclick="toggleLock(0)">🔓</button>
        </div>
        <button id="btn-0" class="btn btn-off" onclick="toggleRelay(0)">OFF</button>
      </div>
      <div class="relay-item">
        <div class="title-wrapper">
          <span id="title-1" class="relay-title">Relay 2</span>
          <button id="lock-1" class="btn-icon" style="font-size:0.9rem;" onclick="toggleLock(1)">🔓</button>
        </div>
        <button id="btn-1" class="btn btn-off" onclick="toggleRelay(1)">OFF</button>
      </div>
      <div class="relay-item">
        <div class="title-wrapper">
          <span id="title-2" class="relay-title">Relay 3</span>
          <button id="lock-2" class="btn-icon" style="font-size:0.9rem;" onclick="toggleLock(2)">🔓</button>
        </div>
        <button id="btn-2" class="btn btn-off" onclick="toggleRelay(2)">OFF</button>
      </div>
      <div class="relay-item">
        <div class="title-wrapper">
          <span id="title-3" class="relay-title">Relay 4</span>
          <button id="lock-3" class="btn-icon" style="font-size:0.9rem;" onclick="toggleLock(3)">🔓</button>
        </div>
        <button id="btn-3" class="btn btn-off" onclick="toggleRelay(3)">OFF</button>
      </div>
    </div>
  </div>

  <div id="settings-modal" class="modal">
    <div class="modal-content">
      <h3 style="margin-top:0;">Configuration</h3>
      <div class="form-group"><label>Home Wi-Fi SSID</label><input type="text" id="cfg-sta-ssid"></div>
      <div class="form-group"><label>Home Wi-Fi Pass</label><input type="password" id="cfg-sta-pass"></div>
      <div class="form-group"><label>Admin User</label><input type="text" id="cfg-user"></div>
      <div class="form-group"><label>Admin Pass</label><input type="password" id="cfg-pass"></div>
      <div class="form-group"><label>Guest Passcode</label><input type="text" id="cfg-guest-pass"></div>
      <div class="modal-actions">
        <button class="btn-save" onclick="saveSettings()">Save & Reboot</button>
        <button class="btn-close" onclick="closeSettings()">Cancel</button>
      </div>
    </div>
  </div>

  <script>
    var websocket = new WebSocket(`ws://${window.location.host}/ws`);
    websocket.onopen = () => { document.getElementById('conn-status').className = 'badge connected'; document.getElementById('conn-status').innerText = 'Connected'; };
    websocket.onclose = () => { document.getElementById('conn-status').className = 'badge disconnected'; document.getElementById('conn-status').innerText = 'Disconnected'; setTimeout(() => location.reload(), 2000); };
    websocket.onmessage = (event) => {
      var data = event.data;
      if (data.startsWith('SYNC:')) {
        var parts = data.substring(5).split('|');
        parts[0].split(',').forEach(p => {
          var s = p.split(':');
          updateUI(s[0].replace('R',''), s[1] === '1');
        });
        parts[1].split(',').forEach(p => {
          var s = p.split(':');
          var idx = s[0].replace('L','');
          var locked = s[1] === '1';
          document.getElementById('lock-' + idx).innerText = locked ? '🔒' : '🔓';
        });
        parts[2].split(',').forEach((name, i) => {
          document.getElementById('title-' + i).innerText = name;
        });
      }
    };

    function updateUI(idx, state) {
      var btn = document.getElementById('btn-' + idx);
      btn.innerText = state ? 'ON' : 'OFF';
      btn.className = state ? 'btn btn-on' : 'btn btn-off';
      btn.disabled = false;
    }

    function toggleRelay(idx) {
      document.getElementById('btn-' + idx).className = 'btn btn-pending';
      websocket.send('ADMIN_TOGGLE:' + idx);
    }
    function toggleLock(idx) { websocket.send('TOGGLE_LOCK:' + idx); }
    function setAll(s) { websocket.send(s === 1 ? 'ADMIN_ALL_ON' : 'ADMIN_ALL_OFF'); }

    function openSettings() { document.getElementById('settings-modal').style.display = 'flex'; }
    function closeSettings() { document.getElementById('settings-modal').style.display = 'none'; }
    function saveSettings() {
      var p = 'CFG:' + document.getElementById('cfg-sta-ssid').value + '|' + document.getElementById('cfg-sta-pass').value + '|' +
              document.getElementById('cfg-user').value + '|' + document.getElementById('cfg-pass').value + '|' + document.getElementById('cfg-guest-pass').value;
      websocket.send(p);
      alert('Saved! Rebooting...');
      closeSettings();
    }
  </script>
</body>
</html>
)rawliteral";

// Embedded OTA Upload UI
const char ota_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta name="viewport" content="width=device-width, initial-scale=1"><title>OTA Update</title></head>
<body style="font-family:Arial;padding:2rem;text-align:center;background:#f0f2f5;">
  <div style="background:#fff;padding:2rem;max-width:340px;margin:auto;border-radius:10px;box-shadow:0 4px 10px rgba(0,0,0,0.1);">
    <h3>Firmware Over-The-Air Update</h3>
    <form method='POST' action='/update' enctype='multipart/form-data'>
      <input type='file' name='update' accept='.bin' style="margin-bottom:15px;width:100%;"><br>
      <input type='submit' value='Upload Firmware' style="background:#007bff;color:#fff;border:none;padding:10px 20px;border-radius:5px;font-weight:bold;cursor:pointer;width:100%;">
    </form>
  </div>
</body>
</html>
)rawliteral";

String readEEPROMString(int startAddr, int maxLen, const String& defaultVal) {
  char buf[maxLen + 1];
  for (int i = 0; i < maxLen; i++) buf[i] = EEPROM.read(startAddr + i);
  buf[maxLen] = '\0';
  if ((uint8_t)buf[0] == 0xFF || buf[0] == '\0') return defaultVal;
  return String(buf);
}

void writeEEPROMString(int startAddr, int maxLen, const String& val) {
  for (int i = 0; i < maxLen; i++) {
    if (i < val.length()) EEPROM.write(startAddr + i, val[i]);
    else EEPROM.write(startAddr + i, 0);
  }
}

String getFullPayload() {
  String payload = "SYNC:";
  for (int i = 0; i < 4; i++) {
    payload += "R" + String(i) + ":" + String(relayStates[i] ? "1" : "0");
    if (i < 3) payload += ",";
  }
  payload += "|";
  for (int i = 0; i < 4; i++) {
    payload += "L" + String(i) + ":" + String(childLocks[i] ? "1" : "0");
    if (i < 3) payload += ",";
  }
  payload += "|";
  for (int i = 0; i < 4; i++) {
    payload += relayNames[i];
    if (i < 3) payload += ",";
  }
  return payload;
}

void publishMqttState() {
  if (mqtt.connected()) {
    String payload = "";
    for (int i = 0; i < 4; i++) {
      payload += "R" + String(i) + ":" + String(relayStates[i] ? "1" : "0") + 
                 ":L" + String(childLocks[i] ? "1" : "0");
      if (i < 3) payload += ",";
    }
    mqtt.publish(TOPIC_STATE, payload.c_str(), true);
  }
}

void notifyAllClients() {
  ws.textAll(getFullPayload());
  publishMqttState();
}

void applyRelay(int idx, bool state) {
  relayStates[idx] = state;
  digitalWrite(relayPins[idx], state ? RELAY_ON : RELAY_OFF);
  EEPROM.write(ADDR_STATES + idx, state ? 1 : 0);
  EEPROM.commit();
}

void handleCommand(String role, String cmd) {
  bool isAdmin = (role == "ADMIN");

  if (cmd.startsWith("TOGGLE:")) {
    int idx = cmd.substring(7).toInt();
    if (idx >= 0 && idx < 4) {
      if (isAdmin || !childLocks[idx]) {
        applyRelay(idx, !relayStates[idx]);
        notifyAllClients();
      }
    }
  }
  else if (cmd == "ALL_ON" && isAdmin) {
    for (int i = 0; i < 4; i++) applyRelay(i, true);
    notifyAllClients();
  }
  else if (cmd == "ALL_OFF" && isAdmin) {
    for (int i = 0; i < 4; i++) applyRelay(i, false);
    notifyAllClients();
  }
  else if (cmd.startsWith("TOGGLE_LOCK:") && isAdmin) {
    int idx = cmd.substring(12).toInt();
    if (idx >= 0 && idx < 4) {
      childLocks[idx] = !childLocks[idx];
      EEPROM.write(ADDR_LOCKS + idx, childLocks[idx] ? 1 : 0);
      EEPROM.commit();
      notifyAllClients();
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String incoming = "";
  for (unsigned int i = 0; i < length; i++) incoming += (char)payload[i];

  int p1 = incoming.indexOf('|');
  int p2 = incoming.indexOf('|', p1 + 1);
  if (p1 == -1 || p2 == -1) return;

  String token = incoming.substring(0, p1);
  String role  = incoming.substring(p1 + 1, p2);
  String cmd   = incoming.substring(p2 + 1);

  if (token == AUTH_TOKEN) {
    if (cmd == "GET_STATUS") publishMqttState();
    else handleCommand(role, cmd);
  }
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;
    String msg = (char*)data;
    if (msg.startsWith("ADMIN_TOGGLE:")) handleCommand("ADMIN", "TOGGLE:" + msg.substring(13));
    else if (msg == "ADMIN_ALL_ON") handleCommand("ADMIN", "ALL_ON");
    else if (msg == "ADMIN_ALL_OFF") handleCommand("ADMIN", "ALL_OFF");
    else if (msg.startsWith("TOGGLE_LOCK:")) handleCommand("ADMIN", msg);
    else if (msg.startsWith("CFG:")) {
      String cfg = msg.substring(4);
      int p1 = cfg.indexOf('|'); int p2 = cfg.indexOf('|', p1 + 1);
      int p3 = cfg.indexOf('|', p2 + 1); int p4 = cfg.indexOf('|', p3 + 1);
      if (p1 != -1 && p2 != -1 && p3 != -1 && p4 != -1) {
        writeEEPROMString(ADDR_STA_SSID, STR_MAX_LEN, cfg.substring(0, p1));
        writeEEPROMString(ADDR_STA_PASS, STR_MAX_LEN, cfg.substring(p1 + 1, p2));
        writeEEPROMString(ADDR_ADMIN_USER, STR_MAX_LEN, cfg.substring(p2 + 1, p3));
        writeEEPROMString(ADDR_ADMIN_PASS, STR_MAX_LEN, cfg.substring(p3 + 1, p4));
        writeEEPROMString(ADDR_GUEST_PASS, STR_MAX_LEN, cfg.substring(p4 + 1));
        EEPROM.commit();
        shouldReboot = true;
        rebootTimer = millis() + 1500;
      }
    }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) client->text(getFullPayload());
  else if (type == WS_EVT_DATA) handleWebSocketMessage(arg, data, len);
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  for (int i = 0; i < 4; i++) {
    byte saved = EEPROM.read(ADDR_STATES + i);
    relayStates[i] = (saved == 1);
    byte locked = EEPROM.read(ADDR_LOCKS + i);
    childLocks[i] = (locked == 1);
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], relayStates[i] ? RELAY_ON : RELAY_OFF);
  }

  String defaults[4] = {"Light", "Fan", "TV", "Socket"};
  for (int i = 0; i < 4; i++) {
    relayNames[i] = readEEPROMString(ADDR_NAMES + (i * NAME_MAX_LEN), NAME_MAX_LEN, defaults[i]);
  }

  sta_ssid   = readEEPROMString(ADDR_STA_SSID, STR_MAX_LEN, default_sta_ssid);
  sta_pass   = readEEPROMString(ADDR_STA_PASS, STR_MAX_LEN, default_sta_pass);
  admin_user = readEEPROMString(ADDR_ADMIN_USER, STR_MAX_LEN, default_admin_user);
  admin_pass = readEEPROMString(ADDR_ADMIN_PASS, STR_MAX_LEN, default_admin_pass);
  guest_pass = readEEPROMString(ADDR_GUEST_PASS, STR_MAX_LEN, default_guest_pass);
  ap_pass    = readEEPROMString(ADDR_AP_PASS, STR_MAX_LEN, default_ap_pass);
  EEPROM.commit();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_pass.c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());

  if (MDNS.begin("infinix")) MDNS.addService("http", "tcp", 80);

  ws.setAuthentication(admin_user.c_str(), admin_pass.c_str());
  ws.onEvent(onEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->authenticate(admin_user.c_str(), admin_pass.c_str())) return request->requestAuthentication();
    request->send_P(200, "text/html", index_html);
  });

  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->authenticate(admin_user.c_str(), admin_pass.c_str())) return request->requestAuthentication();
    request->send_P(200, "text/html", ota_html);
  });

  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->authenticate(admin_user.c_str(), admin_pass.c_str())) return request->requestAuthentication();
    bool shouldRestart = !Update.hasError();
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", shouldRestart ? "<h3 style='text-align:center;font-family:sans-serif;'>Update Successful! Rebooting...</h3>" : "Update Failed!");
    response->addHeader("Connection", "close");
    request->send(response);
    if (shouldRestart) {
      shouldReboot = true;
      rebootTimer = millis() + 2000;
    }
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
    if (!request->authenticate(admin_user.c_str(), admin_pass.c_str())) return;
    if (!index) {
      Serial.printf("OTA Update Start: %s\n", filename.c_str());
      Update.runAsync(true);
      if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
        Update.printError(Serial);
      }
    }
    if (!Update.hasError()) {
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
      }
    }
    if (final) {
      if (Update.end(true)) {
        Serial.printf("OTA Update Success: %uB\n", index + len);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.begin();

  mqtt.setServer(mqtt_broker, mqtt_port);
  mqtt.setCallback(mqttCallback);
}

void loop() {
  MDNS.update();
  ws.cleanupClients();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      if (millis() - lastMqttRetry > 5000) {
        lastMqttRetry = millis();
        String cId = "ESPHybrid-" + String(random(0xffff), HEX);
        if (mqtt.connect(cId.c_str())) {
          mqtt.subscribe(TOPIC_COMMAND);
          publishMqttState();
        }
      }
    } else {
      mqtt.loop();
    }
  }

  if (shouldReboot && millis() > rebootTimer) ESP.restart();
}