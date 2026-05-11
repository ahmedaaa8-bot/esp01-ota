#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

/*
  ESP01 Pump Node V1.7 - Safe Router Preferred Keep Last State
  --------------------------------------
  Stable design for Ahmed Monem ESP01 Master:
  - Node connects to Master AP by default for the proven stable path.
  - In AUTO SAFE it tests Router every 60 seconds only after learning master router IP.
  - Master replies with PNCMD containing the final pump state.
  - Node applies the command and replies PNACK.
  - If network/master is lost after a valid command, node keeps the last pump state.
  - When connection returns, master overrides/synchronizes the node state.

  Master remains the only decision maker.
  Pump relay on the master remains working normally.
*/

// ========================= USER SETTINGS =========================

#define FW_VERSION "1.7"

// 0 = AUTO SAFE: start on Master AP, then test Router only when master router IP is known
// 1 = ROUTER_ONLY
// 2 = MASTER_AP_ONLY
#define NODE_LINK_MODE 0

// Router WiFi - used only if NODE_LINK_MODE is 0 or 1
const char* ROUTER_SSID = "ahmed-monem2";
const char* ROUTER_PASS = "Ahmed-2000";

// Master Access Point credentials - must match master AP exactly, including spaces.
const char* MASTER_AP_SSID = "Weather And Control System ";
const char* MASTER_AP_PASS = "12341234";

// Same key in master sketch.
const char* PUMP_NODE_SHARED_KEY = "PN1234";

// UDP port must match master.
const uint16_t PUMP_NODE_UDP_PORT = 4210;

// In Master AP mode, master AP IP is normally 192.168.4.1
IPAddress MASTER_AP_IP(192, 168, 4, 1);

// If using ROUTER_ONLY and you want direct send, put master router IP here.
// Leave 0.0.0.0 to use broadcast fallback on router mode.
IPAddress MASTER_ROUTER_IP(0, 0, 0, 0);
IPAddress learnedMasterRouterIP(0, 0, 0, 0);

// ESP-01 relay output.
// GPIO0 works, but must be HIGH during boot.
#define PUMP_RELAY_PIN 0

// Most ESP-01 relay boards are Active LOW: ON=LOW / OFF=HIGH.
// 0 = Active LOW, 1 = Active HIGH.
#define RELAY_ACTIVE_HIGH 0

// Safe boot default before receiving first master command.
// 0 = OFF on boot, 1 = ON on boot. After first command, network loss keeps last state.
#define PUMP_DEFAULT_ON_BOOT 0

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 12000UL;
const unsigned long HELLO_INTERVAL_MS       = 5000UL;
const unsigned long FAILSAFE_TIMEOUT_MS    = 60000UL;  // status only: do NOT turn pump OFF when expired
const unsigned long RECONNECT_INTERVAL_MS  = 10000UL;
const unsigned long AUTO_ROUTER_RETRY_MS   = 60000UL;  // while on Master AP, safely test Router every 60s
const unsigned long ROUTER_TEST_REPLY_MS   = 10000UL;  // Router is accepted only if master replies within this time
const unsigned long MASTER_MISSING_MS      = 45000UL;  // if no reply from master on Router, fallback to AP

// ========================= INTERNALS =========================

WiFiUDP udp;
byte currentPumpState = 0;
uint32_t lastCommandId = 0;
unsigned long lastMasterCommandMs = 0;
unsigned long lastHelloMs = 0;
unsigned long lastReconnectTryMs = 0;
bool udpStarted = false;
char activeMode[12] = "--";
bool routerWasSeen = false;
unsigned long lastRouterRetryMs = 0;
unsigned long lastLinkSwitchMs = 0;
unsigned long lastMasterContactMs = 0;

int relayLevelForState(byte state) {
#if RELAY_ACTIVE_HIGH
  return state ? HIGH : LOW;
#else
  return state ? LOW : HIGH;
#endif
}

void applyPump(byte state) {
  currentPumpState = state ? 1 : 0;
  digitalWrite(PUMP_RELAY_PIN, relayLevelForState(currentPumpState));
}

bool connectToWiFi(const char* ssid, const char* pass, const char* modeName) {
  if (!ssid || ssid[0] == '\0') return false;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect();
  delay(150);
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    strncpy(activeMode, modeName, sizeof(activeMode) - 1);
    activeMode[sizeof(activeMode) - 1] = '\0';
    lastLinkSwitchMs = millis();
    if (strcmp(modeName, "ROUTER") == 0) routerWasSeen = true;
    return true;
  }
  return false;
}

bool connectNetwork() {
  bool ok = false;
#if NODE_LINK_MODE == 1
  ok = connectToWiFi(ROUTER_SSID, ROUTER_PASS, "ROUTER");
#elif NODE_LINK_MODE == 2
  ok = connectToWiFi(MASTER_AP_SSID, MASTER_AP_PASS, "AP");
#else
  // AUTO SAFE: keep the proven AP path first. Router is tested later only after
  // the node learns the master's router IP from a valid PNCMD while on AP.
  ok = connectToWiFi(MASTER_AP_SSID, MASTER_AP_PASS, "AP");
  if (!ok) ok = connectToWiFi(ROUTER_SSID, ROUTER_PASS, "ROUTER");
#endif

  if (WiFi.status() == WL_CONNECTED && !udpStarted) {
    udpStarted = udp.begin(PUMP_NODE_UDP_PORT);
  }
  return ok;
}

IPAddress routerBroadcastIP() {
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  return IPAddress((ip[0] & mask[0]) | (~mask[0] & 255),
                   (ip[1] & mask[1]) | (~mask[1] & 255),
                   (ip[2] & mask[2]) | (~mask[2] & 255),
                   (ip[3] & mask[3]) | (~mask[3] & 255));
}

bool isApLink() {
  return strcmp(activeMode, "AP") == 0;
}

bool isRouterLink() {
  return strcmp(activeMode, "ROUTER") == 0;
}

IPAddress targetMasterIP() {
  if (isApLink()) return MASTER_AP_IP;
  if (MASTER_ROUTER_IP != IPAddress(0, 0, 0, 0)) return MASTER_ROUTER_IP;
  if (learnedMasterRouterIP != IPAddress(0, 0, 0, 0)) return learnedMasterRouterIP;
  // Router-only fallback. AUTO SAFE will not intentionally switch to Router without a learned IP.
  return routerBroadcastIP();
}

void sendHelloTo(IPAddress ip) {
  char msg[128];
  unsigned long switchAgeSec = (millis() - lastLinkSwitchMs) / 1000UL;
  snprintf(msg, sizeof(msg), "PNHELLO,%s,%u,%d,%s,%lu,%u,%lu",
           PUMP_NODE_SHARED_KEY,
           (unsigned)currentPumpState,
           WiFi.RSSI(),
           activeMode,
           (unsigned long)(millis() / 1000UL),
           (unsigned)(routerWasSeen ? 1 : 0),
           switchAgeSec);

  udp.beginPacket(ip, PUMP_NODE_UDP_PORT);
  udp.write((const uint8_t*)msg, strlen(msg));
  udp.endPacket();
}

void sendHello() {
  if (WiFi.status() != WL_CONNECTED || !udpStarted) return;

  IPAddress ip = targetMasterIP();
  sendHelloTo(ip);

#if NODE_LINK_MODE == 1
  // Router-only fallback only. In AUTO SAFE, direct learned IP is preferred to avoid unstable broadcast behavior.
  if (isRouterLink() && MASTER_ROUTER_IP == IPAddress(0, 0, 0, 0) && learnedMasterRouterIP == IPAddress(0, 0, 0, 0)) {
    sendHelloTo(IPAddress(255, 255, 255, 255));
  }
#endif

  lastHelloMs = millis();
}

void sendAck(IPAddress masterIP, uint16_t masterPort, uint32_t cmdId) {
  if (!udpStarted) return;

  char msg[112];
  snprintf(msg, sizeof(msg), "PNACK,%s,%lu,%u,%d,%s",
           PUMP_NODE_SHARED_KEY,
           (unsigned long)cmdId,
           (unsigned)currentPumpState,
           WiFi.RSSI(),
           activeMode);

  udp.beginPacket(masterIP, masterPort);
  udp.write((const uint8_t*)msg, strlen(msg));
  udp.endPacket();
}

void handleUdpPacket() {
  if (!udpStarted) return;

  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  char buf[128];
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';

  // Expected: PNCMD,KEY,cmdId,state,masterMillis,masterStaIp(optional)
  char* p0 = strtok(buf, ",");
  char* p1 = strtok(nullptr, ",");
  char* p2 = strtok(nullptr, ",");
  char* p3 = strtok(nullptr, ",");
  char* p4 = strtok(nullptr, ",");
  char* p5 = strtok(nullptr, ",");

  if (!p0 || !p1 || !p2 || !p3) return;
  if (strcmp(p0, "PNCMD") != 0) return;
  if (strcmp(p1, PUMP_NODE_SHARED_KEY) != 0) return;

  uint32_t cmdId = strtoul(p2, nullptr, 10);
  byte state = (byte)atoi(p3);
  if (state != 0 && state != 1) return;

  if (p5 && p5[0] != '\0') {
    IPAddress ip;
    if (ip.fromString(p5) && ip != IPAddress(0, 0, 0, 0)) {
      learnedMasterRouterIP = ip;
    }
  }

  lastCommandId = cmdId;
  lastMasterCommandMs = millis();
  applyPump(state);
  lastMasterContactMs = millis();
  sendAck(udp.remoteIP(), udp.remotePort(), cmdId);
}

void stopUdpForSwitch() {
  if (udpStarted) {
    udp.stop();
    udpStarted = false;
  }
}

void switchToMasterAp() {
#if NODE_LINK_MODE != 1
  stopUdpForSwitch();
  connectToWiFi(MASTER_AP_SSID, MASTER_AP_PASS, "AP");
  if (WiFi.status() == WL_CONNECTED) udpStarted = udp.begin(PUMP_NODE_UDP_PORT);
  sendHello();
#endif
}

bool waitForMasterReply(unsigned long timeoutMs) {
  unsigned long start = millis();
  unsigned long contactBefore = lastMasterContactMs;
  while (millis() - start < timeoutMs) {
    yield();
    handleUdpPacket();
    if (lastMasterContactMs != 0 && lastMasterContactMs != contactBefore) return true;
    delay(20);
  }
  return false;
}

void tryReturnToRouter() {
#if NODE_LINK_MODE == 0
  if (!isApLink()) return;
  if (millis() - lastRouterRetryMs < AUTO_ROUTER_RETRY_MS) return;
  lastRouterRetryMs = millis();

  IPAddress routerMaster = MASTER_ROUTER_IP;
  if (routerMaster == IPAddress(0, 0, 0, 0)) routerMaster = learnedMasterRouterIP;

  // Stable rule: do not leave AP to Router unless we already know where the master is on Router.
  // The master sends this IP inside PNCMD while the node is connected to Master AP.
  if (routerMaster == IPAddress(0, 0, 0, 0)) {
    routerWasSeen = false;
    return;
  }

  stopUdpForSwitch();
  if (connectToWiFi(ROUTER_SSID, ROUTER_PASS, "ROUTER")) {
    routerWasSeen = true;
    udpStarted = udp.begin(PUMP_NODE_UDP_PORT);
    sendHelloTo(routerMaster);
    lastHelloMs = millis();

    // Accept Router only after a real master reply. Otherwise return immediately to AP.
    if (waitForMasterReply(ROUTER_TEST_REPLY_MS)) {
      return;
    }
  }

  // Router failed or master did not answer. Return to the proven AP path and keep pump state unchanged.
  stopUdpForSwitch();
  connectToWiFi(MASTER_AP_SSID, MASTER_AP_PASS, "AP");
  if (WiFi.status() == WL_CONNECTED) udpStarted = udp.begin(PUMP_NODE_UDP_PORT);
  sendHello();
#endif
}

void maintainConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!udpStarted) udpStarted = udp.begin(PUMP_NODE_UDP_PORT);

#if NODE_LINK_MODE == 0
    // If router path does not get replies from master, fallback to Master AP.
    if (isRouterLink() && ((lastMasterContactMs > 0 && millis() - lastMasterContactMs > MASTER_MISSING_MS) || (lastMasterContactMs == 0 && millis() - lastLinkSwitchMs > MASTER_MISSING_MS))) {
      switchToMasterAp();
      return;
    }
    tryReturnToRouter();
#endif
    return;
  }

  if (millis() - lastReconnectTryMs < RECONNECT_INTERVAL_MS) return;
  lastReconnectTryMs = millis();
  stopUdpForSwitch();
  if (!connectNetwork()) {
    switchToMasterAp();
  }
}

void applyFailsafe() {
  // V1.5 Keep Last State:
  // Do NOT turn the pump OFF just because Router/AP/Master disappeared.
  // The relay remains at the last valid master command.
  // When communication returns, the master sends the current final pump state and overrides the node.
  if (lastMasterCommandMs == 0) return;
  if (millis() - lastMasterCommandMs > FAILSAFE_TIMEOUT_MS) {
    // Intentionally no GPIO change. Keep currentPumpState.
  }
}

void setup() {
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  applyPump(PUMP_DEFAULT_ON_BOOT ? 1 : 0); // boot default; after first command, network loss keeps last state

  connectNetwork();
  sendHello();
}

void loop() {
  yield();
  maintainConnection();
  handleUdpPacket();

  if (WiFi.status() == WL_CONNECTED && millis() - lastHelloMs >= HELLO_INTERVAL_MS) {
    sendHello();
  }

  applyFailsafe();
}
