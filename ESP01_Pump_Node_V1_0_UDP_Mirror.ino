#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

/*
  ESP01 Pump Node V1.0 - UDP Mirror
  ---------------------------------
  وظيفته بسيطة وآمنة:
  - يستقبل أمر البامب النهائي من ESP01 Master فقط.
  - يشغل/يفصل ريلاي البامب الفرعي أو لمبة بيان أو كونتاكتور بعيد.
  - يرد ACK للماستر ليظهر Connected / Offline في صفحة الويب.
  - لو الاتصال بالماستر انقطع أكثر من FAILSAFE_TIMEOUT_MS يفصل الخرج تلقائيًا.

  مهم:
  - الماستر يظل صاحب القرار الأساسي.
  - هذا النود لا يقرأ ThingSpeak ولا يقرر بنفسه.
*/

// ========================= USER SETTINGS =========================

#define FW_VERSION "1.0"

// اختار وضع الاتصال:
// 0 = AUTO: جرّب الراوتر الأول، ولو فشل اتصل على Access Point بتاع الماستر
// 1 = ROUTER_ONLY: اتصل على الراوتر فقط
// 2 = MASTER_AP_ONLY: اتصل على Access Point بتاع الماستر فقط
#define NODE_LINK_MODE 0

// بيانات الراوتر - لو هتخلي النود والماستر على نفس شبكة البيت
const char* ROUTER_SSID = "Amonem";
const char* ROUTER_PASS = "19772018";

// بيانات Access Point الخاص بالماستر
// لازم تطابق اسم وباسورد AP الموجودين في كود الماستر أو صفحة WiFi الخاصة به.
const char* MASTER_AP_SSID = "Weather And Control System ";
const char* MASTER_AP_PASS = "12341234";

// لازم نفس القيمة الموجودة في كود الماستر
const char* PUMP_NODE_SHARED_KEY = "PN1234";

// نفس البورت الموجود في الماستر
const uint16_t PUMP_NODE_UDP_PORT = 4210;

// خرج الريلاي في ESP-01
// GPIO0 مناسب لكن لازم يكون HIGH وقت الإقلاع.
#define PUMP_RELAY_PIN 0

// أغلب ريلاي ESP-01 Active LOW: ON = LOW / OFF = HIGH
// لو الريلاي عندك Active HIGH خليها 1.
#define RELAY_ACTIVE_HIGH 0

// وضع الأمان: لو مفيش أمر من الماستر خلال 60 ثانية يفصل البامب الفرعي.
const unsigned long FAILSAFE_TIMEOUT_MS = 60000UL;

// زمن محاولة الاتصال بكل شبكة
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 12000UL;

// ========================= INTERNALS =========================

WiFiUDP udp;
byte currentPumpState = 0;
uint32_t lastCommandId = 0;
unsigned long lastMasterCommandMs = 0;
char activeMode[10] = "--";

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
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    strncpy(activeMode, modeName, sizeof(activeMode) - 1);
    activeMode[sizeof(activeMode) - 1] = '\0';
    return true;
  }
  return false;
}

void connectNetwork() {
#if NODE_LINK_MODE == 1
  connectToWiFi(ROUTER_SSID, ROUTER_PASS, "ROUTER");
#elif NODE_LINK_MODE == 2
  connectToWiFi(MASTER_AP_SSID, MASTER_AP_PASS, "AP");
#else
  if (!connectToWiFi(ROUTER_SSID, ROUTER_PASS, "ROUTER")) {
    connectToWiFi(MASTER_AP_SSID, MASTER_AP_PASS, "AP");
  }
#endif
}

void sendAck(IPAddress masterIP, uint16_t masterPort, uint32_t cmdId) {
  char msg[96];
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
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  char buf[128];
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';

  // Expected command: PNCMD,KEY,cmdId,state,masterMillis
  char* p0 = strtok(buf, ",");
  char* p1 = strtok(nullptr, ",");
  char* p2 = strtok(nullptr, ",");
  char* p3 = strtok(nullptr, ",");
  if (!p0 || !p1 || !p2 || !p3) return;
  if (strcmp(p0, "PNCMD") != 0) return;
  if (strcmp(p1, PUMP_NODE_SHARED_KEY) != 0) return;

  uint32_t cmdId = strtoul(p2, nullptr, 10);
  byte state = (byte)atoi(p3);
  if (state != 0 && state != 1) return;

  lastCommandId = cmdId;
  lastMasterCommandMs = millis();
  applyPump(state);
  sendAck(udp.remoteIP(), udp.remotePort(), cmdId);
}

void maintainConnection() {
  static unsigned long lastReconnectTry = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastReconnectTry < 10000UL) return;
  lastReconnectTry = millis();
  connectNetwork();
  if (WiFi.status() == WL_CONNECTED) udp.begin(PUMP_NODE_UDP_PORT);
}

void applyFailsafe() {
  if (lastMasterCommandMs == 0) return;
  if (millis() - lastMasterCommandMs > FAILSAFE_TIMEOUT_MS) {
    if (currentPumpState != 0) applyPump(0);
  }
}

void setup() {
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  applyPump(0); // Safe boot: pump node output OFF

  connectNetwork();
  udp.begin(PUMP_NODE_UDP_PORT);
}

void loop() {
  yield();
  maintainConnection();
  handleUdpPacket();
  applyFailsafe();
}
