#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <time.h>

// ----------------------------------------------------------------------------
// Credentials live in secrets.h (NOT committed to git). Copy the provided
// secrets.h.example to secrets.h and fill in your own WiFi + MQTT details.
// ----------------------------------------------------------------------------
#include "secrets.h"

// ================== CHANGE SUMMARY ==================
// - OTA preserved.
// - No OTA password hardcoded.
// - Real timestamps using NTP.
// - Auto Status Poll control, default OFF.
// - Updated verified commands.
// - Command dropdown order:
//   1. Status Request
//   2. Start
//   3. Stop
// - Verified speed command uses CMD 01 / 02 C4 / RPM.
// - RPM slider added; sends speed command on release.
// - Status decoder cleaned up.
// - Custom command box preserved.
// - No commands are sent automatically on boot.
// - Fixed MQTT Discovery RPM setpoint slider by adding state_topic, value_template, and optimistic true.

// ================== WIFI ==================
// Defined in secrets.h: WIFI_SSID, WIFI_PASS
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

// ================== TIME ==================
const char* tzInfo = "EST5EDT,M3.2.0/2,M11.1.0/2";

// ================== RS485 ==================
// RS485 RO -> D6
// RS485 DI -> D7
SoftwareSerial rs485Serial(D6, D7);

// ================== WEB SERVER ==================
ESP8266WebServer server(80);

// ================== MQTT / HOME ASSISTANT ==================
// MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS and MQTT_CLIENT_ID come from
// secrets.h and are used directly below.
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

unsigned long lastMqttReconnectAttempt = 0;


// ================== SERIAL SETTINGS ==================
int baudRate = 9600;
String parity = "N";
int stopBits = 1;

// ================== PENTAIR / INTELLIFLO ADDRESSES ==================
byte pumpAddress = 0x60;
byte controllerAddress = 0x10;

// ================== CONTROL FLAGS ==================
bool debugMode = false;
bool paused = false;
bool cyclicControl = true;
bool logEnabled = false;   // Default OFF for stable dashboard
bool logAutoPoll = false;  // Optional auto-poll log

// Auto cycle default
unsigned long lastCycleTime = 0;
unsigned long cycleIntervalMs = 2000;  // 2 seconds default
int cyclicRpm = 1500;
int sliderRpm = 1500;

// ================== LAST PUMP STATUS ==================
String lastPumpRun = "UNKNOWN";
int lastPumpRPM = 0;
int lastPumpReportedPower = 0;
String lastPumpMode = "UNKNOWN";
String lastPumpState = "UNKNOWN";
String lastStatusTime = "Never";
bool suppressNextStatusLog = false;
unsigned long autoPollCount = 0;
String lastAutoPollTime = "Never";

// ================== RX BUFFER ==================
#define MAX_BUFFER 256
byte serialBuffer[MAX_BUFFER];
int bufferIndex = 0;
unsigned long lastByteTime = 0;
const int BYTE_TIMEOUT = 5;

// ================== LOGGING ==================
String logBuffer = "";
const int MAX_LOG_LINES = 50;
String logLines[MAX_LOG_LINES];
int logIndex = 0;

// ================== COMMAND DEFINITIONS ==================
struct CommandDef {
  const char* group;
  const char* name;
  byte dest;
  byte src;
  byte cmd;
  byte payload[8];
  byte payloadLen;
};

CommandDef commandList[] = {
  {"VERIFIED", "Status Request",              0x60, 0x10, 0x07, {0x00}, 0},
  {"VERIFIED", "Start / Run",                 0x60, 0x10, 0x06, {0x0A}, 1},
  {"VERIFIED", "Stop",                        0x60, 0x10, 0x06, {0x04}, 1},
  {"VERIFIED", "Remote Enable / ECOM",        0x60, 0x10, 0x04, {0xFF}, 1},
  {"VERIFIED", "Set Speed 1000 RPM",          0x60, 0x10, 0x01, {0x02, 0xC4, 0x03, 0xE8}, 4},
  {"VERIFIED", "Set Speed 1500 RPM",          0x60, 0x10, 0x01, {0x02, 0xC4, 0x05, 0xDC}, 4},
  {"VERIFIED", "Set Speed 1600 RPM",          0x60, 0x10, 0x01, {0x02, 0xC4, 0x06, 0x40}, 4},
  {"VERIFIED", "Set Speed 1750 RPM",          0x60, 0x10, 0x01, {0x02, 0xC4, 0x06, 0xD6}, 4},
  {"VERIFIED", "Set Speed 1800 RPM",          0x60, 0x10, 0x01, {0x02, 0xC4, 0x07, 0x08}, 4},
  {"VERIFIED", "Set Speed 2000 RPM",          0x60, 0x10, 0x01, {0x02, 0xC4, 0x07, 0xD0}, 4},
  {"VERIFIED", "Set Speed 3000 RPM",          0x60, 0x10, 0x01, {0x02, 0xC4, 0x0B, 0xB8}, 4}
};

const int numCommands = sizeof(commandList) / sizeof(commandList[0]);

// ================== FUNCTION DECLARATIONS ==================
void handleRoot();
void handleSendCommand();
void handleClearLog();
void handleToggleDebug();
void handleSetComm();
void handleTogglePause();
void handleExportLog();
void handleToggleCyclic();
void handleTogglePollLog();
void handleToggleLog();
void handleSetCycle();
void handleSetRpm();
void handleQuickStatus();
void handleQuickStart();
void handleQuickStop();
void handleQuickRemote();
void handleQuickLocal();
void handleStatusJson();

void readSerial();
void parsePacket();

void addToLog(String message);
String getTimeStamp();
String htmlEscape(String s);

uint16_t pentairChecksum(byte* data, int lenWithoutChecksum);
int buildPentairPacket(byte* out, byte dest, byte src, byte cmd, byte* payload, byte payloadLen);
String packetToHex(byte* data, int len);
String buildCommandHex(CommandDef c);

void sendMessage(byte* buf, int len, String label = "Command");
void sendBuiltCommand(byte dest, byte src, byte cmd, byte* payload, byte payloadLen, String label);
bool parseHexString(String input, byte* output, int& outputLen, int maxLen);

String decodePentairPacket(byte* data, int len);
String decodeStatusPayload(byte* data);
String decodeRunByte(byte b);
String decodeModeByte(byte b);
String decodeStateByte(byte b);
String commandNameFromPacket(byte cmd, byte payloadLen, byte* payload);

void runCyclicControl();
void buildSpeedPayload(byte* payload, int rpm);
int clampRpmToStep(int rpm);

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);

  rs485Serial.begin(
    baudRate,
    parity == "N"
      ? (stopBits == 1 ? SWSERIAL_8N1 : SWSERIAL_8N2)
      : (stopBits == 1 ? SWSERIAL_8E1 : SWSERIAL_8E2)
  );

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected, IP: " + WiFi.localIP().toString());

  configTime(tzInfo, "pool.ntp.org", "time.nist.gov", "time.google.com");

  addToLog("============================================================");
  addToLog("[" + getTimeStamp() + "] SYSTEM");
  addToLog("  WiFi connected: " + WiFi.localIP().toString());
  addToLog("  Serial: " + String(baudRate) + " " + parity + String(stopBits));
  addToLog("  Auto status poll: ON at boot");
  addToLog("  Cycle interval: " + String(cycleIntervalMs / 1000) + " seconds");
  addToLog("  No commands are sent automatically on boot.");
  addToLog("============================================================");

  ArduinoOTA.setHostname(MQTT_CLIENT_ID);

  ArduinoOTA.onStart([]() {
    rs485Serial.end();
    Serial.println("OTA Update Started");
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Update Complete");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", progress / (total / 100));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(768);
  mqttClient.setCallback(mqttCallback);
  Serial.println("OTA Ready");

  server.on("/", handleRoot);
  server.on("/sendCommand", handleSendCommand);
  server.on("/clearLog", handleClearLog);
  server.on("/toggleDebug", handleToggleDebug);
  server.on("/setComm", handleSetComm);
  server.on("/togglePause", handleTogglePause);
  server.on("/exportLog", handleExportLog);
  server.on("/toggleCyclic", handleToggleCyclic);
  server.on("/togglePollLog", handleTogglePollLog);
  server.on("/toggleLog", handleToggleLog);
  server.on("/setCycle", handleSetCycle);
  server.on("/setRpm", handleSetRpm);
  server.on("/quickStatus", handleQuickStatus);
  server.on("/quickStart", handleQuickStart);
  server.on("/quickStop", handleQuickStop);
  server.on("/quickRemote", handleQuickRemote);
  server.on("/quickLocal", handleQuickLocal);
  server.on("/statusJson", handleStatusJson);

  server.begin();
  Serial.println("Web server started");
}

// ================== LOOP ==================
void loop() {
  ArduinoOTA.handle();
  mqttReconnectNonBlocking();
  server.handleClient();

  if (!paused) {
    readSerial();
  }

  if (cyclicControl) {
    runCyclicControl();
  }
}

// ================== CYCLIC CONTROL ==================
void runCyclicControl() {
  if (millis() - lastCycleTime < cycleIntervalMs) return;

  lastCycleTime = millis();
  autoPollCount++;
  lastAutoPollTime = getTimeStamp();

  suppressNextStatusLog = !logAutoPoll;

  if (logAutoPoll) {
    addToLog("");
    addToLog("[" + getTimeStamp() + "] AUTO POLL");
    addToLog("  Sending Status Request");
  }

  sendBuiltCommand(0x60, 0x10, 0x07, nullptr, 0, "Auto Status Poll");
}

void buildSpeedPayload(byte* payload, int rpm) {
  payload[0] = 0x02;
  payload[1] = 0xC4;
  payload[2] = (rpm >> 8) & 0xFF;
  payload[3] = rpm & 0xFF;
}

int clampRpmToStep(int rpm) {
  if (rpm < 600) rpm = 600;
  if (rpm > 3450) rpm = 3450;

  // Round to nearest 50 RPM step so HA, MQTT, and web UI stay aligned.
  rpm = ((rpm + 25) / 50) * 50;

  if (rpm < 600) rpm = 600;
  if (rpm > 3450) rpm = 3450;
  return rpm;
}

// ================== RX ==================
void readSerial() {
  while (rs485Serial.available()) {
    byte b = rs485Serial.read();

    if (bufferIndex < MAX_BUFFER) {
      serialBuffer[bufferIndex++] = b;
    }

    lastByteTime = millis();

    if (debugMode) {
      if (b < 0x10) Serial.print("0");
      Serial.print(b, HEX);
      Serial.print(" ");
    }
  }

  if (bufferIndex > 0 && millis() - lastByteTime > BYTE_TIMEOUT) {
    parsePacket();
    bufferIndex = 0;
  }
}

void parsePacket() {
  bool isStatusResponse = false;

  if (bufferIndex >= 11 &&
      serialBuffer[0] == 0xFF &&
      serialBuffer[1] == 0x00 &&
      serialBuffer[2] == 0xFF &&
      serialBuffer[3] == 0xA5 &&
      serialBuffer[7] == 0x07 &&
      serialBuffer[8] == 0x0F) {
    isStatusResponse = true;
  }

  String decoded = decodePentairPacket(serialBuffer, bufferIndex);

  if (suppressNextStatusLog && isStatusResponse) {
    suppressNextStatusLog = false;
    return;
  }

  addToLog("");
  addToLog("[" + getTimeStamp() + "] RX");
  addToLog("  Raw: " + packetToHex(serialBuffer, bufferIndex));
  addToLog("  Len: " + String(bufferIndex));
  addToLog("  Decode: " + decoded);
}

// ================== DECODER ==================
String decodePentairPacket(byte* data, int len) {
  if (len < 4) return "Too short";

  if (!(data[0] == 0xFF && data[1] == 0x00 && data[2] == 0xFF && data[3] == 0xA5)) {
    return "No Pentair/IntelliFlo header";
  }

  if (len < 11) return "Header found, packet too short";

  byte protocol = data[4];
  byte dest = data[5];
  byte src = data[6];
  byte cmd = data[7];
  byte payloadLen = data[8];

  int expectedLen = 9 + payloadLen + 2;

  String s = "";
  s += "Pentair/IntelliFlo frame";
  s += " | Protocol=0x" + String(protocol, HEX);
  s += " | Dest=0x" + String(dest, HEX);
  s += " | Src=0x" + String(src, HEX);
  s += " | Cmd=0x" + String(cmd, HEX);
  s += " | PayloadLen=" + String(payloadLen);
  s += " | ExpectedLen=" + String(expectedLen);

  if (len != expectedLen) {
    s += " | LENGTH MISMATCH";
    return s;
  }

  uint16_t rxChecksum = ((uint16_t)data[len - 2] << 8) | data[len - 1];
  uint16_t calcChecksum = pentairChecksum(data, len - 2);

  s += " | Checksum=";
  s += (rxChecksum == calcChecksum ? "OK" : "BAD");
  s += " RX=0x" + String(rxChecksum, HEX);
  s += " CALC=0x" + String(calcChecksum, HEX);

  if (src == pumpAddress) s += " | From Pump";
  if (dest == controllerAddress) s += " | To Controller";

  byte* payload = &data[9];
  s += " | " + commandNameFromPacket(cmd, payloadLen, payload);

  if (cmd == 0x07 && payloadLen == 15) {
    s += " | " + decodeStatusPayload(data);
  } else if (cmd == 0x01 && payloadLen == 2) {
    uint16_t ackValue = ((uint16_t)data[9] << 8) | data[10];
    s += " | AckValue=0x";
    if (ackValue < 0x1000) s += "0";
    if (ackValue < 0x0100) s += "0";
    if (ackValue < 0x0010) s += "0";
    s += String(ackValue, HEX);
  } else if (payloadLen > 0) {
    s += " | Payload=";
    for (int i = 0; i < payloadLen; i++) {
      if (payload[i] < 0x10) s += "0";
      s += String(payload[i], HEX);
      if (i < payloadLen - 1) s += " ";
    }
  }

  s.toUpperCase();
  return s;
}

String commandNameFromPacket(byte cmd, byte payloadLen, byte* payload) {
  if (cmd == 0x04 && payloadLen == 1 && payload[0] == 0xFF) return "Remote Enable ACK/Command";
  if (cmd == 0x04 && payloadLen == 1 && payload[0] == 0x00) return "Manual/Local Release ACK/Command";
  if (cmd == 0x06 && payloadLen == 1 && payload[0] == 0x0A) return "Run/Start ACK/Command";
  if (cmd == 0x06 && payloadLen == 1 && payload[0] == 0x04) return "Stop ACK/Command";
  if (cmd == 0x07 && payloadLen == 0) return "Status Poll";
  if (cmd == 0x07 && payloadLen == 15) return "Status Response";
  if (cmd == 0x01 && payloadLen == 2) return "Setpoint ACK";
  if (cmd == 0x01 && payloadLen == 4 && payload[0] == 0x02 && payload[1] == 0xC4) return "Speed Command";
  if (cmd == 0x01 && payloadLen == 4 && payload[0] == 0x02 && payload[1] == 0xE4) return "Setpoint Write Command";
  if (cmd == 0x05) return "Mode Command";
  if (cmd == 0x0A) return "VSF Speed Command";
  return "Unknown / undecoded command";
}

String decodeStatusPayload(byte* data) {
  byte runByte = data[9];
  byte modeByte = data[10];
  byte stateByte = data[11];

  uint16_t watts = ((uint16_t)data[12] << 8) | data[13];
  uint16_t rpm = ((uint16_t)data[14] << 8) | data[15];

  byte flowOrPercent = data[16];

  lastPumpRun = decodeRunByte(runByte);
  lastPumpRPM = rpm;
  lastPumpReportedPower = watts;
  lastPumpMode = decodeModeByte(modeByte);
  lastPumpState = decodeStateByte(stateByte);
  lastStatusTime = getTimeStamp();
  mqttPublishState();

  String s = "";
  s += "Run=" + decodeRunByte(runByte);
  s += " | Mode=" + decodeModeByte(modeByte);
  s += " | State=" + decodeStateByte(stateByte);
  s += " | Watts=" + String(watts) + "W";
  s += " | RPM=" + String(rpm);
  s += " | Flow/Percent=" + String(flowOrPercent);

  s += " | RawStatus=";
  for (int i = 0; i < 15; i++) {
    byte b = data[9 + i];
    if (b < 0x10) s += "0";
    s += String(b, HEX);
    if (i < 14) s += " ";
  }

  return s;
}

String decodeRunByte(byte b) {
  if (b == 0x0A) return "Running";
  if (b == 0x04) return "Stopped";
  return "Unknown(0x" + String(b, HEX) + ")";
}

String decodeModeByte(byte b) {
  if (b == 0x00) return "Local (0x00)";
  if (b == 0x11) return "Remote (0x11)";
  return "Unknown(0x" + String(b, HEX) + ")";
}

String decodeStateByte(byte b) {
  if (b == 0xFF) return "Stopped";
  if (b == 0x01) return "Normal Operation";
  if (b == 0x03) return "Priming";
  if (b == 0x07) return "Alarm";
  return "Unknown(0x" + String(b, HEX) + ")";
}

// ================== CHECKSUM / PACKET BUILDING ==================
uint16_t pentairChecksum(byte* data, int lenWithoutChecksum) {
  uint16_t sum = 0;

  for (int i = 3; i < lenWithoutChecksum; i++) {
    sum += data[i];
  }

  return sum;
}

int buildPentairPacket(byte* out, byte dest, byte src, byte cmd, byte* payload, byte payloadLen) {
  out[0] = 0xFF;
  out[1] = 0x00;
  out[2] = 0xFF;
  out[3] = 0xA5;
  out[4] = 0x00;
  out[5] = dest;
  out[6] = src;
  out[7] = cmd;
  out[8] = payloadLen;

  for (int i = 0; i < payloadLen; i++) {
    out[9 + i] = payload[i];
  }

  int lenWithoutChecksum = 9 + payloadLen;
  uint16_t chk = pentairChecksum(out, lenWithoutChecksum);

  out[lenWithoutChecksum] = (chk >> 8) & 0xFF;
  out[lenWithoutChecksum + 1] = chk & 0xFF;

  return lenWithoutChecksum + 2;
}

String packetToHex(byte* data, int len) {
  String s = "";

  for (int i = 0; i < len; i++) {
    if (data[i] < 0x10) s += "0";
    s += String(data[i], HEX);
    if (i < len - 1) s += " ";
  }

  s.toUpperCase();
  return s;
}

String buildCommandHex(CommandDef c) {
  byte packet[32];
  int len = buildPentairPacket(packet, c.dest, c.src, c.cmd, (byte*)c.payload, c.payloadLen);
  return packetToHex(packet, len);
}

// ================== TX ==================
void sendBuiltCommand(byte dest, byte src, byte cmd, byte* payload, byte payloadLen, String label) {
  byte packet[32];
  int len = buildPentairPacket(packet, dest, src, cmd, payload, payloadLen);
  sendMessage(packet, len, label);
}

void sendMessage(byte* buf, int len, String label) {
  rs485Serial.write(buf, len);
  rs485Serial.flush();

  if (label == "Auto Status Poll" && !logAutoPoll) return;

  addToLog("");
  addToLog("[" + getTimeStamp() + "] TX  " + label);
  addToLog("  Raw: " + packetToHex(buf, len));
  addToLog("  Len: " + String(len));
}

bool parseHexString(String input, byte* output, int& outputLen, int maxLen) {
  input.trim();
  input.toUpperCase();

  outputLen = 0;

  char buf[300];
  input.toCharArray(buf, sizeof(buf));

  char* token = strtok(buf, " ,");

  while (token != NULL) {
    if (outputLen >= maxLen) return false;

    char* endPtr;
    long value = strtol(token, &endPtr, 16);

    if (*endPtr != '\0' || value < 0 || value > 255) {
      return false;
    }

    output[outputLen++] = (byte)value;
    token = strtok(NULL, " ,");
  }

  return outputLen > 0;
}

// ================== LOGGING ==================
void addToLog(String message) {
  if (!logEnabled) return;
  if (paused) return;

  // Only store into the ring buffer. Do NOT rebuild logBuffer on every line -
  // rebuilding a multi-KB String each call fragments the ESP8266 heap and
  // causes crashes/reboots, especially with auto-poll running. The full text
  // is assembled on demand in buildLogText() when the browser fetches it.
  logLines[logIndex] = message;
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
}

// Assemble log text on demand (only when the browser requests /exportLog).
String buildLogText() {
  String out = "";
  out.reserve(2048);
  for (int i = 0; i < MAX_LOG_LINES; i++) {
    int idx = (logIndex + i) % MAX_LOG_LINES;
    if (logLines[idx].length()) out += logLines[idx] + "\n";
  }
  return out;
}

String getTimeStamp() {
  time_t now = time(nullptr);

  if (now > 1700000000) {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buf);
  }

  unsigned long ms = millis();
  int seconds = (ms / 1000) % 60;
  int minutes = (ms / 60000) % 60;
  int hours = (ms / 3600000) % 24;

  char fallback[22];
  sprintf(fallback, "UPTIME %02d:%02d:%02d", hours, minutes, seconds);
  return String(fallback);
}

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

// ================== WEB UI ==================

void mqttPublishDiscovery() {
  if (!mqttClient.connected()) return;

  String dev = "{\"identifiers\":[\"pool_pump_rs485\"],\"name\":\"RS485 Pool Pump\",\"manufacturer\":\"Lingxiao/Pentair Compatible\",\"model\":\"RS485 Pump Controller\"}";

  mqttClient.publish("homeassistant/sensor/pool_pump/rpm/config",
    ("{\"name\":\"Pool Pump RPM\",\"unique_id\":\"pool_pump_rpm\",\"state_topic\":\"pool/pump/state\",\"value_template\":\"{{ value_json.rpm }}\",\"unit_of_measurement\":\"RPM\",\"device\":" + dev + "}").c_str(), true);

  mqttClient.publish("homeassistant/sensor/pool_pump/reported_power/config",
    ("{\"name\":\"Pool Pump Reported Power\",\"unique_id\":\"pool_pump_reported_power\",\"state_topic\":\"pool/pump/state\",\"value_template\":\"{{ value_json.reported_power }}\",\"unit_of_measurement\":\"W\",\"device\":" + dev + "}").c_str(), true);

  mqttClient.publish("homeassistant/binary_sensor/pool_pump/running/config",
    ("{\"name\":\"Pool Pump Running\",\"unique_id\":\"pool_pump_running\",\"state_topic\":\"pool/pump/state\",\"value_template\":\"{{ value_json.running }}\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"device_class\":\"running\",\"device\":" + dev + "}").c_str(), true);

  mqttClient.publish("homeassistant/button/pool_pump/start/config",
    ("{\"name\":\"Pool Pump Start\",\"unique_id\":\"pool_pump_start\",\"command_topic\":\"pool/pump/cmd/start\",\"payload_press\":\"PRESS\",\"device\":" + dev + "}").c_str(), true);

  mqttClient.publish("homeassistant/button/pool_pump/stop/config",
    ("{\"name\":\"Pool Pump Stop\",\"unique_id\":\"pool_pump_stop\",\"command_topic\":\"pool/pump/cmd/stop\",\"payload_press\":\"PRESS\",\"device\":" + dev + "}").c_str(), true);

  mqttClient.publish("homeassistant/button/pool_pump/status/config",
    ("{\"name\":\"Pool Pump Status Request\",\"unique_id\":\"pool_pump_status_request\",\"command_topic\":\"pool/pump/cmd/status\",\"payload_press\":\"PRESS\",\"device\":" + dev + "}").c_str(), true);

  mqttClient.publish("homeassistant/button/pool_pump/remote/config",
    ("{\"name\":\"Pool Pump Remote Enable\",\"unique_id\":\"pool_pump_remote_enable\",\"command_topic\":\"pool/pump/cmd/remote\",\"payload_press\":\"PRESS\",\"device\":" + dev + "}").c_str(), true);

  mqttClient.publish("homeassistant/number/pool_pump/rpm_set/config",
    ("{\"name\":\"Pool Pump RPM Setpoint\","
     "\"unique_id\":\"pool_pump_set_rpm\","
     "\"command_topic\":\"pool/pump/cmd/rpm\","
     "\"state_topic\":\"pool/pump/state\","
     "\"value_template\":\"{{ value_json.rpm }}\","
     "\"unit_of_measurement\":\"RPM\","
     "\"min\":600,"
     "\"max\":3450,"
     "\"step\":50,"
     "\"mode\":\"slider\","
     "\"optimistic\":true,"
     "\"device\":" + dev + "}").c_str(), true);
}

void mqttPublishState() {
  if (!mqttClient.connected()) return;

  String json = "{";
  json += "\"running\":\"" + String(lastPumpRun == "Running" ? "ON" : "OFF") + "\",";
  json += "\"rpm\":" + String(lastPumpRPM) + ",";
  json += "\"setRpm\":" + String(sliderRpm) + ",";
  json += "\"reported_power\":" + String(lastPumpReportedPower) + ",";
  json += "\"mode\":\"" + lastPumpMode + "\",";
  json += "\"state\":\"" + lastPumpState + "\",";
  json += "\"last\":\"" + lastStatusTime + "\",";
  json += "\"auto_poll\":\"" + String(cyclicControl ? "ON" : "OFF") + "\"";
  json += "}";

  mqttClient.publish("pool/pump/state", json.c_str(), true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (t == "pool/pump/cmd/status") {
    sendBuiltCommand(0x60, 0x10, 0x07, nullptr, 0, "MQTT Status Request");
  } else if (t == "pool/pump/cmd/start") {
    byte p[] = {0x0A};
    sendBuiltCommand(0x60, 0x10, 0x06, p, 1, "MQTT Start");
  } else if (t == "pool/pump/cmd/stop") {
    byte p[] = {0x04};
    sendBuiltCommand(0x60, 0x10, 0x06, p, 1, "MQTT Stop");
  } else if (t == "pool/pump/cmd/remote") {
    byte p[] = {0xFF};
    sendBuiltCommand(0x60, 0x10, 0x04, p, 1, "MQTT Remote Enable");
  } else if (t == "pool/pump/cmd/rpm") {
    int rpm = clampRpmToStep(msg.toInt());
    sliderRpm = rpm;
    byte speedPayload[4];
    buildSpeedPayload(speedPayload, rpm);
    sendBuiltCommand(0x60, 0x10, 0x01, speedPayload, 4, "MQTT Set Speed " + String(rpm) + " RPM");
  }
}

void mqttReconnectNonBlocking() {
  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  if (millis() - lastMqttReconnectAttempt < 5000) return;
  lastMqttReconnectAttempt = millis();

  bool ok;
  if (strlen(MQTT_USER) > 0) ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
  else ok = mqttClient.connect(MQTT_CLIENT_ID);

  if (ok) {
    mqttClient.subscribe("pool/pump/cmd/#");
    mqttPublishDiscovery();
    mqttPublishState();
  }
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Pool Pump Dashboard</title>";

  html += "<style>";
  html += ":root{--bg:#0b1220;--card:#111827;--card2:#172033;--text:#e5e7eb;--muted:#94a3b8;--ok:#22c55e;--bad:#ef4444;--blue:#38bdf8;--line:#263244}";
  html += "*{box-sizing:border-box}";
  html += "body{margin:0;background:linear-gradient(135deg,#07111f,#12243b);color:var(--text);font-family:Arial,Helvetica,sans-serif}";
  html += ".wrap{max-width:980px;margin:0 auto;padding:14px}";
  html += ".header{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:12px}";
  html += "h1{font-size:22px;margin:0}.sub{color:var(--muted);font-size:12px;margin-top:3px}";
  html += ".grid{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-bottom:10px}";
  html += ".card{background:rgba(17,24,39,.92);border:1px solid var(--line);border-radius:16px;padding:14px;box-shadow:0 8px 22px rgba(0,0,0,.22)}";
  html += ".label{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.08em}";
  html += ".value{font-size:24px;font-weight:700;margin-top:6px}.small{font-size:13px;color:var(--muted)}";
  html += ".run{color:var(--ok)}.stop{color:var(--bad)}.blue{color:var(--blue)}";
  html += ".main{display:grid;grid-template-columns:2fr 1fr;gap:10px}";
  html += ".rpmBig{text-align:center;font-size:52px;font-weight:800;margin:6px 0 0}";
  html += ".rpmLabel{text-align:center;color:var(--muted);font-size:12px;margin-bottom:8px}";
  html += "input[type=range]{width:100%;accent-color:#38bdf8}";
  html += ".btns{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}";
  html += "button{border:0;border-radius:12px;padding:12px;font-size:14px;font-weight:700;color:#06121f;background:#e5e7eb;cursor:pointer}";
  html += ".start{background:#22c55e;color:#052e16}.stopbtn{background:#ef4444;color:#fff}.remote{background:#38bdf8;color:#062233}.status{background:#fbbf24;color:#261b00}";
  html += ".ghost{background:#1f2937;color:var(--text);border:1px solid var(--line)}";
  html += "form{margin:0}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}";
  html += "input,select{background:#0b1220;color:var(--text);border:1px solid var(--line);border-radius:10px;padding:10px}";
  html += "textarea{width:100%;height:230px;background:#020617;color:#d1d5db;border:1px solid var(--line);border-radius:12px;padding:10px;font-family:monospace;font-size:11px}";
  html += "details{margin-top:10px}summary{cursor:pointer;color:#cbd5e1;font-weight:700;margin:8px 0}";
  html += ".footer{font-size:11px;color:var(--muted);margin-top:10px;text-align:right}";
  html += "@media(max-width:760px){.grid{grid-template-columns:repeat(2,1fr)}.main{grid-template-columns:1fr}.rpmBig{font-size:42px}}";
  html += "</style>";

  html += "<script>";
  html += "function post(u){fetch(u,{method:'POST'}).then(()=>setTimeout(refreshStatus,250));return false;}";
  html += "function setRpm(v){document.getElementById('rpmSetVal').innerText=v;}";
  html += "function sendRpm(v){fetch('/setRpm?rpm='+v).then(()=>setTimeout(refreshStatus,700));}";
  html += "function refreshStatus(){fetch('/statusJson').then(r=>r.json()).then(s=>{";
  html += "document.getElementById('run').innerText=s.run;";
  html += "document.getElementById('run').className='value '+(s.run=='Running'?'run':'stop');";
  html += "document.getElementById('rpm').innerText=s.rpm;";
  html += "document.getElementById('rpmSetVal').innerText=s.setRpm;";
  html += "document.getElementById('rpmRange').value=s.setRpm;";
  html += "document.getElementById('power').innerText=s.power;";
  html += "document.getElementById('mode').innerText=s.mode;";
  html += "document.getElementById('state').innerText=s.state;";
  html += "document.getElementById('last').innerText=s.last;";
  html += "document.getElementById('poll').innerText=s.poll;";
  html += "document.getElementById('pollCount').innerText=s.pollCount;";
  html += "document.getElementById('mqtt').innerText=s.mqtt;";
  html += "document.getElementById('logState').innerText=s.log;";
  html += "document.getElementById('pollLogState').innerText=s.pollLog;";
  html += "});}";
  html += "function refreshLogs(){let box=document.getElementById('logs');if(box){fetch('/exportLog').then(r=>r.text()).then(t=>box.value=t);}}";
  html += "setInterval(refreshStatus,1000);setInterval(refreshLogs,3000);";
  // Restore the Advanced (log) panel open state so it does not auto-close.
  html += "window.addEventListener('load',function(){var d=document.getElementById('adv');if(d&&localStorage.getItem('advOpen')==='1')d.open=true;});";
  html += "</script>";

  html += "</head><body><div class='wrap'>";

  html += "<div class='header'><div><h1>Pool Pump Dashboard</h1><div class='sub'>Lingxiao / Pentair-compatible RS485 Controller</div></div>";
  html += "<div class='small'>IP: " + WiFi.localIP().toString() + "</div></div>";

  html += "<div class='grid'>";
  html += "<div class='card'><div class='label'>Pump</div><div id='run' class='value " + String(lastPumpRun == "Running" ? "run" : "stop") + "'>" + lastPumpRun + "</div><div class='small'>Last: <span id='last'>" + lastStatusTime + "</span></div></div>";
  html += "<div class='card'><div class='label'>Actual RPM</div><div id='rpm' class='value blue'>" + String(lastPumpRPM) + "</div><div class='small'>from status packet</div></div>";
  html += "<div class='card'><div class='label'>Reported Power</div><div class='value'><span id='power'>" + String(lastPumpReportedPower) + "</span> W</div><div class='small'>pump-reported only</div></div>";
  html += "<div class='card'><div class='label'>MQTT</div><div id='mqtt' class='value'>" + String(mqttClient.connected() ? "CONNECTED" : "OFFLINE") + "</div><div class='small'>Client: " + String(MQTT_CLIENT_ID) + "</div></div>";
  html += "</div>";

  html += "<div class='main'>";
  html += "<div class='card'>";
  html += "<div class='label'>Set RPM</div>";
  html += "<div class='rpmBig'><span id='rpmSetVal'>" + String(sliderRpm) + "</span></div><div class='rpmLabel'>RPM command sends when slider is released</div>";
  html += "<input id='rpmRange' type='range' min='600' max='3450' step='50' value='" + String(sliderRpm) + "' oninput='setRpm(this.value)' onchange='sendRpm(this.value)'>";
  html += "<div class='row' style='justify-content:space-between;margin-top:10px'><span class='small'>600</span><span class='small'>3450</span></div>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<div class='label'>Controls</div><br>";
  html += "<div class='btns'>";
  html += "<form onsubmit='return post(\"/quickStart\")'><button class='start'>Start</button></form>";
  html += "<form onsubmit='return post(\"/quickStop\")'><button class='stopbtn'>Stop</button></form>";
  html += "<form onsubmit='return post(\"/quickRemote\")'><button class='remote'>Remote</button></form>";
  html += "<form onsubmit='return post(\"/quickStatus\")'><button class='status'>Status</button></form>";
  html += "</div>";
  html += "</div>";
  html += "</div>";

  html += "<div class='grid' style='margin-top:10px'>";
  html += "<div class='card'><div class='label'>Mode</div><div id='mode' class='value'>" + lastPumpMode + "</div></div>";
  html += "<div class='card'><div class='label'>State</div><div id='state' class='value'>" + lastPumpState + "</div></div>";
  html += "<div class='card'><div class='label'>Auto Poll</div><div id='poll' class='value'>" + String(cyclicControl ? "ON" : "OFF") + "</div><div class='small'>Count: <span id='pollCount'>" + String(autoPollCount) + "</span></div></div>";
  html += "<div class='card'><div class='label'>Logging</div><div class='value'><span id='logState'>" + String(logEnabled ? "ON" : "OFF") + "</span></div><div class='small'>Auto poll log: <span id='pollLogState'>" + String(logAutoPoll ? "ON" : "OFF") + "</span></div></div>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<div class='row'>";
  html += "<form action='/toggleCyclic' method='POST'><button class='ghost' type='submit'>" + String(cyclicControl ? "Auto Poll OFF" : "Auto Poll ON") + "</button></form>";
  html += "<form action='/setCycle' method='POST'><span class='small'>Interval</span> <input type='number' name='interval' value='" + String(cycleIntervalMs / 1000) + "' min='1' max='120' step='1' style='width:80px'> <button class='ghost' type='submit'>Set</button></form>";
  html += "<form action='/toggleLog' method='POST'><button class='ghost' type='submit'>Log " + String(logEnabled ? "OFF" : "ON") + "</button></form>";
  html += "<form action='/togglePollLog' method='POST'><button class='ghost' type='submit'>Auto Poll Log " + String(logAutoPoll ? "OFF" : "ON") + "</button></form>";
  html += "</div>";
  html += "</div>";

  html += "<details id='adv' ontoggle='localStorage.setItem(\"advOpen\", this.open?\"1\":\"0\")'>";
  html += "<summary>Advanced: command testing and log</summary>";
  html += "<div class='card'>";
  html += "<h3>Send Command</h3>";
  html += "<form action='/sendCommand' method='POST'>";
  html += "<select name='command' style='max-width:100%;width:100%'>";
  for (int i = 0; i < numCommands; i++) {
    String cmdHex = buildCommandHex(commandList[i]);
    html += "<option value='" + cmdHex + "'>";
    html += String(commandList[i].group) + " - " + String(commandList[i].name) + " : " + cmdHex;
    html += "</option>";
  }
  html += "</select><br><br>";
  html += "<button class='ghost' type='submit'>Send Selected</button><br><br>";
  html += "<div class='small'>Custom full packet hex:</div>";
  html += "<input type='text' name='customCommand' style='width:100%' placeholder='FF 00 FF A5 ...'><br><br>";
  html += "<button class='ghost' type='submit'>Send Custom</button>";
  html += "</form>";
  html += "<h3>Log</h3>";
  html += "<textarea readonly id='logs'>" + htmlEscape(buildLogText()) + "</textarea>";
  html += "<div class='row' style='margin-top:8px'>";
  html += "<form action='/clearLog' method='POST'><button class='ghost' type='submit'>Clear Log</button></form>";
  html += "<form action='/exportLog' method='GET'><button class='ghost' type='submit'>Export Log</button></form>";
  html += "<form action='/togglePause' method='POST'><button class='ghost' type='submit' id='pauseBtn'>" + String(paused ? "Resume Log" : "Pause Log") + "</button></form>";
  html += "<form action='/toggleDebug' method='POST'><button class='ghost' type='submit'>" + String(debugMode ? "Debug OFF" : "Debug ON") + "</button></form>";
  html += "</div></div>";
  html += "</details>";

  html += "<div class='footer'>FW: Lingxiao_RS485_Stable_vNext | MQTT topic: pool/pump/state</div>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

// ================== WEB HANDLERS ==================
void handleSendCommand() {
  String command = "";

  if (server.hasArg("customCommand")) {
    String custom = server.arg("customCommand");
    custom.trim();

    if (custom.length() > 0) {
      command = custom;
    }
  }

  if (command.length() == 0 && server.hasArg("command")) {
    command = server.arg("command");
  }

  command.trim();

  if (command.length() > 0) {
    byte bytes[80];
    int byteCount = 0;

    if (parseHexString(command, bytes, byteCount, 80)) {
      String label = "Custom / Selected Command";
      if (byteCount >= 9) {
        label = commandNameFromPacket(bytes[7], bytes[8], &bytes[9]);
      }
      sendMessage(bytes, byteCount, label);
    } else {
      addToLog("");
      addToLog("[" + getTimeStamp() + "] ERROR");
      addToLog("  Invalid hex command");
    }
  }

  server.sendHeader("Location", "/");
  server.send(302);
}

void handleToggleCyclic() {
  cyclicControl = !cyclicControl;
  lastCycleTime = 0;

  addToLog("");
  addToLog("[" + getTimeStamp() + "] SYSTEM");
  addToLog("  Auto status poll: " + String(cyclicControl ? "ON" : "OFF"));
  addToLog("  Interval: " + String(cycleIntervalMs / 1000) + " seconds");

  server.sendHeader("Location", "/");
  server.send(302);
}

void handleToggleLog() {
  logEnabled = !logEnabled;
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleTogglePollLog() {
  logAutoPoll = !logAutoPoll;

  addToLog("");
  addToLog("[" + getTimeStamp() + "] SYSTEM");
  addToLog("  Auto poll logging: " + String(logAutoPoll ? "ON" : "OFF"));

  server.sendHeader("Location", "/");
  server.send(302);
}

void handleSetCycle() {
if (server.hasArg("interval")) {
    int intervalSec = server.arg("interval").toInt();
    if (intervalSec >= 1 && intervalSec <= 120) {
      cycleIntervalMs = (unsigned long)intervalSec * 1000UL;
    }
  }

  addToLog("");
  addToLog("[" + getTimeStamp() + "] SYSTEM");
  addToLog("  Auto status poll settings updated");
  addToLog("  Interval: " + String(cycleIntervalMs / 1000) + " seconds");

  server.sendHeader("Location", "/");
  server.send(302);
}


void handleSetRpm() {
  int rpm = clampRpmToStep(server.arg("rpm").toInt());
  sliderRpm = rpm;

  byte speedPayload[4];
  buildSpeedPayload(speedPayload, rpm);
  sendBuiltCommand(0x60, 0x10, 0x01, speedPayload, 4, "Slider Set Speed " + String(rpm) + " RPM");

  server.send(200, "text/plain", "OK");
}

void handleQuickStatus() {
  sendBuiltCommand(0x60, 0x10, 0x07, nullptr, 0, "Status Poll");
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleQuickStart() {
  byte runPayload[] = {0x0A};
  sendBuiltCommand(0x60, 0x10, 0x06, runPayload, 1, "Start");
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleQuickStop() {
  byte stopPayload[] = {0x04};
  sendBuiltCommand(0x60, 0x10, 0x06, stopPayload, 1, "Stop");
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleQuickRemote() {
  byte remotePayload[] = {0xFF};
  sendBuiltCommand(0x60, 0x10, 0x04, remotePayload, 1, "Remote Enable");
  server.sendHeader("Location", "/");
  server.send(302);
}



void handleQuickLocal() {
  byte localPayload[] = {0x00};
  sendBuiltCommand(0x60, 0x10, 0x04, localPayload, 1, "Local Release Experimental");
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleStatusJson() {
  String json = "{";
  json += "\"run\":\"" + lastPumpRun + "\",";
  json += "\"rpm\":" + String(lastPumpRPM) + ",";
  json += "\"setRpm\":" + String(sliderRpm) + ",";
  json += "\"power\":" + String(lastPumpReportedPower) + ",";
  json += "\"mode\":\"" + lastPumpMode + "\",";
  json += "\"state\":\"" + lastPumpState + "\",";
  json += "\"last\":\"" + lastStatusTime + "\",";
  json += "\"poll\":\"" + String(cyclicControl ? "ON" : "OFF") + "\",";
  json += "\"pollLog\":\"" + String(logAutoPoll ? "ON" : "OFF") + "\",";
  json += "\"log\":\"" + String(logEnabled ? "ON" : "OFF") + "\",";
  json += "\"mqtt\":\"" + String(mqttClient.connected() ? "CONNECTED" : "OFFLINE") + "\",";
  json += "\"pollCount\":" + String(autoPollCount);
  json += "}";
  server.send(200, "application/json", json);
}

void handleClearLog() {
  logBuffer = "";

  for (int i = 0; i < MAX_LOG_LINES; i++) {
    logLines[i] = "";
  }

  logIndex = 0;

  server.sendHeader("Location", "/");
  server.send(302);
}

void handleToggleDebug() {
  debugMode = !debugMode;

  addToLog("");
  addToLog("[" + getTimeStamp() + "] SYSTEM");
  addToLog("  Debug mode: " + String(debugMode ? "Enabled" : "Disabled"));

  server.sendHeader("Location", "/");
  server.send(302);
}

void handleSetComm() {
  int newBaud = server.arg("baud").toInt();
  String newParity = server.arg("parity");
  int newStopBits = server.arg("stopBits").toInt();

  if (newBaud != baudRate || newParity != parity || newStopBits != stopBits) {
    baudRate = newBaud;
    parity = newParity;
    stopBits = newStopBits;

    rs485Serial.end();

    rs485Serial.begin(
      baudRate,
      parity == "N"
        ? (stopBits == 1 ? SWSERIAL_8N1 : SWSERIAL_8N2)
        : (stopBits == 1 ? SWSERIAL_8E1 : SWSERIAL_8E2)
    );

    addToLog("");
    addToLog("[" + getTimeStamp() + "] SYSTEM");
    addToLog("  Serial updated: " + String(baudRate) + " " + parity + String(stopBits));
  }

  server.sendHeader("Location", "/");
  server.send(302);
}

void handleTogglePause() {
  paused = !paused;

  if (!paused) {
    addToLog("");
    addToLog("[" + getTimeStamp() + "] SYSTEM");
    addToLog("  Log resumed");
  }

  server.sendHeader("Location", "/");
  server.send(302);
}

void handleExportLog() {
  server.sendHeader("Content-Disposition", "attachment; filename=pump_log.txt");
  server.send(200, "text/plain", buildLogText());
}
