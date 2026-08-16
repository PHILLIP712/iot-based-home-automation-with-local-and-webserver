#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <EEPROM.h>

#define EEPROM_SIZE 256
#define STR_MAX_LEN 32
#define NAME_MAX_LEN 20

// EEPROM Addresses
#define ADDR_STATES    0
#define ADDR_NAMES     4
#define ADDR_STA_SSID  84
#define ADDR_STA_PASS  116
#define ADDR_HTTP_USER 148
#define ADDR_HTTP_PASS 180
#define ADDR_AP_PASS   212

// Default Credentials
const char* default_sta_ssid  = "Infinix";
const char* default_sta_pass  = "1234567890";
const char* default_http_user = "Infinix";
const char* default_http_pass = "1234567890";
const char* ap_ssid           = "Infinix-Relay-Hub";
const char* default_ap_pass   = "1234567890";

// Cloud MQTT Setup (Must match index.html)
const char* mqtt_broker   = "broker.hivemq.com";
const int   mqtt_port     = 1883;
const char* TOPIC_COMMAND = "hub_7f3b9c2a8e/cmd";
const char* TOPIC_STATE   = "hub_7f3b9c2a8e/state";
const char* AUTH_TOKEN    = "sec_k8912xL90";

// Pins: D1, D2, D7, D6
const int relayPins[4] = {5, 4, 13, 12};
bool relayStates[4]    = {false, false, false, false};
String relayNames[4]   = {"Light", "Fan", "TV", "Socket"};

String sta_ssid, sta_pass, http_user, http_pass, ap_pass;

#define RELAY_ON  LOW
#define RELAY_OFF HIGH

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
WiFiClient espClient;
PubSubClient mqtt(espClient);

unsigned long lastMqttRetry = 0;
bool shouldReboot = false;
unsigned long rebootTimer = 0;

// Embedded Local Offline Web Interface
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Relay Hub</title>
  <style>
    body { font-family: Arial, sans-serif; display: flex; justify-content: center; align-items: center; min-height: 90vh; background: #f0f2f5; margin: 0; padding: 1rem; }
    .card { background: #ffffff; padding: 1.8rem; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); text-align: center; width: 100%; max-width: 360px; position: relative; }
    .top-bar { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1.2rem; }
    .badge { padding: 4px 8px; border-radius: 4px; font-size: 0.8rem; }
    .connected { background: #d4edda; color: #155724; }
    .disconnected { background: #f8d7da; color: #721c24; }
    .btn-settings-icon { background: transparent; border: none; font-size: 1.2rem; cursor: pointer; }
    .master-controls { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 1.2rem; }
    .btn-master-on { background: #28a745; color: #fff; padding: 0.75rem; border-radius: 6px; font-weight: bold; border: none; cursor: pointer; font-size: 0.9rem; }
    .btn-master-off { background: #dc3545; color: #fff; padding: 0.75rem; border-radius: 6px; font-weight: bold; border: none; cursor: pointer; font-size: 0.9rem; }
    .relay-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
    .relay-item { background: #f8f9fa; padding: 0.9rem; border-radius: 8px; border: 1px solid #e9ecef; display: flex; flex-direction: column; align-items: center; }
    .title-wrapper { display: flex; align-items: center; justify-content: center; gap: 5px; width: 100%; margin-bottom: 8px; }
    .relay-title { font-weight: bold; font-size: 0.95rem; color: #333; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 100px; }
    .btn-rename { background: transparent; border: none; cursor: pointer; font-size: 0.8rem; opacity: 0.6; padding: 0; }
    .btn { width: 100%; padding: 0.6rem; font-size: 0.9rem; font-weight: bold; border: none; border-radius: 6px; cursor: pointer; transition: background 0.2s; }
    .btn-off { background: #6c757d; color: white; }
    .btn-on { background: #007bff; color: white; }
    .btn-pending { background: #ffc107; color: #212529; cursor: not-allowed; animation: pulse 1s infinite alternate; }
    @keyframes pulse { from { opacity: 0.7; } to { opacity: 1; } }
    .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); justify-content: center; align-items: center; z-index: 100; }
    .modal-content { background: #fff; padding: 1.5rem; border-radius: 10px; width: 90%; max-width: 320px; text-align: left; }
    .modal-content h3 { margin-top: 0; }
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
      <button class="btn-settings-icon" onclick="openSettings()">⚙️</button>
    </div>
    <h2>Smart Relay Hub</h2>
    <div class="master-controls">
      <button id="btn-master-on" class="btn-master-on" onclick="setAll(1)">ALL ON</button>
      <button id="btn-master-off" class="btn-master-off" onclick="setAll(0)">ALL OFF</button>
    </div>
    <div class="relay-grid">
      <div class="relay-item">
        <div class="title-wrapper"><span id="title-0" class="relay-title">Relay 1</span><button class="btn-rename" onclick="editName(0)">✏️</button></div>
        <button id="btn-0" class="btn btn-off" onclick="toggleRelay(0)">OFF</button>
      </div>
      <div class="relay-item">
        <div class="title-wrapper"><span id="title-1" class="relay-title">Relay 2</span><button class="btn-rename" onclick="editName(1)">✏️</button></div>
        <button id="btn-1" class="btn btn-off" onclick="toggleRelay(1)">OFF</button>
      </div>
      <div class="relay-item">
        <div class="title-wrapper"><span id="title-2" class="relay-title">Relay 3</span><button class="btn-rename" onclick="editName(2)">✏️</button></div>
        <button id="btn-2" class="btn btn-off" onclick="toggleRelay(2)">OFF</button>
      </div>
      <div class="relay-item">
        <div class="title-wrapper"><span id="title-3" class="relay-title">Relay 4</span><button class="btn-rename" onclick="editName(3)">✏️</button></div>
        <button id="btn-3" class="btn btn-off" onclick="toggleRelay(3)">OFF</button>
      </div>
    </div>
  </div>

  <div id="settings-modal" class="modal">
    <div class="modal-content">
      <h3>Wi-Fi & Security Setup</h3>
      <div class="form-group"><label>Home Wi-Fi SSID</label><input type="text" id="cfg-sta-ssid"></div>
      <div class="form-group"><label>Home Wi-Fi Password</label><input type="password" id="cfg-sta-pass"></div>
      <div class="form-group"><label>Web Login Username</label><input type="text" id="cfg-user"></div>
      <div class="form-group"><label>Web Login Password</label><input type="password" id="cfg-pass"></div>
      <div class="modal-actions">
        <button class="btn-save" onclick="saveSettings()">Save & Reboot</button>
        <button class="btn-close" onclick="closeSettings()">Cancel</button>
      </div>
    </div>
  </div>

  <script>
    var gateway = `ws://${window.location.host}/ws`;
    var websocket;

    function initWebSocket() {
      websocket = new WebSocket(gateway);
      websocket.onopen = function() {
        var badge = document.getElementById('conn-status');
        badge.innerText = 'Local Connected';
        badge.className = 'badge connected';
      };
      websocket.onclose = function() {
        var badge = document.getElementById('conn-status');
        badge.innerText = 'Disconnected';
        badge.className = 'badge disconnected';
        setTimeout(initWebSocket, 2000);
      };
      websocket.onmessage = function(event) {
        var data = event.data;
        if (data.startsWith('ACK:')) {
          var parts = data.substring(4).split(':');
          updateButtonUI(parts[0], parts[1] === '1');
        } else if (data.startsWith('ACK_ALL:')) {
          var states = data.substring(8).split(',');
          states.forEach(function(item) {
            var parts = item.split(':');
            updateButtonUI(parts[0], parts[1] === '1');
          });
          resetMasterButtons();
        } else if (data.startsWith('SYNC:')) {
          var sections = data.substring(5).split('|');
          var pairs = sections[0].split(',');
          pairs.forEach(function(pair) {
            var parts = pair.split(':');
            updateButtonUI(parts[0].replace('R', ''), parts[1] === '1');
          });
          var names = sections[1].split(',');
          names.forEach(function(name, index) {
            var titleElem = document.getElementById('title-' + index);
            if (titleElem) titleElem.innerText = name;
          });
        }
      };
    }

    function updateButtonUI(index, state) {
      var btn = document.getElementById('btn-' + index);
      if (btn) {
        btn.innerText = state ? 'ON' : 'OFF';
        btn.className = state ? 'btn btn-on' : 'btn btn-off';
        btn.disabled = false;
      }
    }

    function resetMasterButtons() {
      var mOn = document.getElementById('btn-master-on');
      var mOff = document.getElementById('btn-master-off');
      if (mOn) { mOn.innerText = 'ALL ON'; mOn.disabled = false; }
      if (mOff) { mOff.innerText = 'ALL OFF'; mOff.disabled = false; }
    }

    function toggleRelay(index) {
      var btn = document.getElementById('btn-' + index);
      if (btn) {
        btn.innerText = '⏳ Verifying...';
        btn.className = 'btn btn-pending';
        btn.disabled = true;
      }
      websocket.send('TOGGLE:' + index);
    }

    function setAll(state) {
      for (var i = 0; i < 4; i++) {
        var btn = document.getElementById('btn-' + i);
        if (btn) {
          btn.innerText = '⏳ Verifying...';
          btn.className = 'btn btn-pending';
          btn.disabled = true;
        }
      }
      websocket.send(state === 1 ? 'ALL_ON' : 'ALL_OFF');
    }

    function editName(index) {
      var currentName = document.getElementById('title-' + index).innerText;
      var newName = prompt('Enter name for appliance ' + (index + 1) + ' (Max 18 chars):', currentName);
      if (newName !== null && newName.trim() !== '') {
        var sanitized = newName.trim().substring(0, 18).replace(/[,|:]/g, '');
        websocket.send('RENAME:' + index + ':' + sanitized);
      }
    }

    function openSettings() { document.getElementById('settings-modal').style.display = 'flex'; }
    function closeSettings() { document.getElementById('settings-modal').style.display = 'none'; }

    function saveSettings() {
      var sSSID = document.getElementById('cfg-sta-ssid').value.trim();
      var sPass = document.getElementById('cfg-sta-pass').value.trim();
      var uUser = document.getElementById('cfg-user').value.trim();
      var uPass = document.getElementById('cfg-pass').value.trim();

      if (!sSSID || !uUser || !uPass) {
        alert('Please fill out all fields.');
        return;
      }
      websocket.send('CFG:' + sSSID + '|' + sPass + '|' + uUser + '|' + uPass);
      alert('Settings saved! Rebooting...');
      closeSettings();
    }

    window.onload = initWebSocket;
  </script>
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
    payload += relayNames[i];
    if (i < 3) payload += ",";
  }
  return payload;
}

void publishMqttState() {
  if (mqtt.connected()) {
    String payload = "";
    for (int i = 0; i < 4; i++) {
      payload += "R" + String(i) + ":" + String(relayStates[i] ? "1" : "0");
      if (i < 3) payload += ",";
    }
    mqtt.publish(TOPIC_STATE, payload.c_str(), true);
  }
}

void notifyAllClients() {
  ws.textAll(getFullPayload());
  publishMqttState();
}

void executeRelayCommand(String command) {
  if (command.startsWith("TOGGLE:")) {
    int idx = command.substring(7).toInt();
    if (idx >= 0 && idx < 4) {
      relayStates[idx] = !relayStates[idx];
      digitalWrite(relayPins[idx], relayStates[idx] ? RELAY_ON : RELAY_OFF);
      EEPROM.write(ADDR_STATES + idx, relayStates[idx] ? 1 : 0);
      EEPROM.commit();
      
      String ack = "ACK:" + String(idx) + ":" + String(relayStates[idx] ? "1" : "0");
      ws.textAll(ack);
      publishMqttState();
    }
  }
  else if (command == "ALL_ON") {
    for (int i = 0; i < 4; i++) {
      relayStates[i] = true;
      digitalWrite(relayPins[i], RELAY_ON);
      EEPROM.write(ADDR_STATES + i, 1);
    }
    EEPROM.commit();
    ws.textAll("ACK_ALL:0:1,1:1,2:1,3:1");
    publishMqttState();
  }
  else if (command == "ALL_OFF") {
    for (int i = 0; i < 4; i++) {
      relayStates[i] = false;
      digitalWrite(relayPins[i], RELAY_OFF);
      EEPROM.write(ADDR_STATES + i, 0);
    }
    EEPROM.commit();
    ws.textAll("ACK_ALL:0:0,1:0,2:0,3:0");
    publishMqttState();
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String incoming = "";
  for (unsigned int i = 0; i < length; i++) incoming += (char)payload[i];

  int delimiter = incoming.indexOf('|');
  if (delimiter == -1) return;

  String token = incoming.substring(0, delimiter);
  String cmd   = incoming.substring(delimiter + 1);

  if (token == AUTH_TOKEN) {
    if (cmd == "GET_STATUS") publishMqttState();
    else executeRelayCommand(cmd);
  }
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;
    String message = (char*)data;

    if (message.startsWith("TOGGLE:") || message == "ALL_ON" || message == "ALL_OFF") {
      executeRelayCommand(message);
    }
    else if (message.startsWith("RENAME:")) {
      int c1 = message.indexOf(':');
      int c2 = message.indexOf(':', c1 + 1);
      if (c2 != -1) {
        int idx = message.substring(c1 + 1, c2).toInt();
        String newName = message.substring(c2 + 1);
        if (idx >= 0 && idx < 4 && newName.length() > 0) {
          relayNames[idx] = newName;
          int addr = ADDR_NAMES + (idx * NAME_MAX_LEN);
          writeEEPROMString(addr, NAME_MAX_LEN, newName);
          EEPROM.commit();
          notifyAllClients();
        }
      }
    }
    else if (message.startsWith("CFG:")) {
      String cfgData = message.substring(4);
      int p1 = cfgData.indexOf('|');
      int p2 = cfgData.indexOf('|', p1 + 1);
      int p3 = cfgData.indexOf('|', p2 + 1);

      if (p1 != -1 && p2 != -1 && p3 != -1) {
        writeEEPROMString(ADDR_STA_SSID, STR_MAX_LEN, cfgData.substring(0, p1));
        writeEEPROMString(ADDR_STA_PASS, STR_MAX_LEN, cfgData.substring(p1 + 1, p2));
        writeEEPROMString(ADDR_HTTP_USER, STR_MAX_LEN, cfgData.substring(p2 + 1, p3));
        writeEEPROMString(ADDR_HTTP_PASS, STR_MAX_LEN, cfgData.substring(p3 + 1));
        EEPROM.commit();

        shouldReboot = true;
        rebootTimer = millis() + 1500;
      }
    }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) client->text(getFullPayload());
  else if (type == WS_EVT_DATA) handleWebSocketMessage(arg, data, len);
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  for (int i = 0; i < 4; i++) {
    byte saved = EEPROM.read(ADDR_STATES + i);
    if (saved == 0xFF) { saved = 0; EEPROM.write(ADDR_STATES + i, 0); }
    relayStates[i] = (saved == 1);
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], relayStates[i] ? RELAY_ON : RELAY_OFF);
  }

  String defaults[4] = {"Light", "Fan", "TV", "Socket"};
  for (int i = 0; i < 4; i++) {
    relayNames[i] = readEEPROMString(ADDR_NAMES + (i * NAME_MAX_LEN), NAME_MAX_LEN, defaults[i]);
  }
  sta_ssid  = readEEPROMString(ADDR_STA_SSID, STR_MAX_LEN, default_sta_ssid);
  sta_pass  = readEEPROMString(ADDR_STA_PASS, STR_MAX_LEN, default_sta_pass);
  http_user = readEEPROMString(ADDR_HTTP_USER, STR_MAX_LEN, default_http_user);
  http_pass = readEEPROMString(ADDR_HTTP_PASS, STR_MAX_LEN, default_http_pass);
  ap_pass   = readEEPROMString(ADDR_AP_PASS, STR_MAX_LEN, default_ap_pass);
  EEPROM.commit();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_pass.c_str());

  WiFi.setAutoReconnect(true);
  WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());

  if (MDNS.begin("infinix")) MDNS.addService("http", "tcp", 80);

  ws.setAuthentication(http_user.c_str(), http_pass.c_str());
  ws.onEvent(onEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->authenticate(http_user.c_str(), http_pass.c_str())) {
      return request->requestAuthentication();
    }
    request->send_P(200, "text/html", index_html);
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

  if (shouldReboot && millis() > rebootTimer) {
    ESP.restart();
  }
}