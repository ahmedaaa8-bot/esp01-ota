
/*
ESP32 Native Build
Recommended relay pins:
PUMP  -> GPIO26
LAMP  -> GPIO27
OUT3  -> GPIO25

Board:
ESP32 Dev Module

Partition:
Minimal SPIFFS (1.9MB APP with OTA)
*/

#include <WiFi.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ThingSpeak.h>
#include <EEPROM.h>
// ArduinoOTA removed in V1.1 PRO to save ESP01 flash. USB/Web OTA/GitHub OTA still work.
#include <WebServer.h>
#include <Update.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>

/*
  Project: ESP-01 ThingSpeak DHT11/DHT22 + 2 Outputs Controller
  Board  : ESP8266 ESP-01 / ESP-01S

  Notes:
  - This is an independent cleaned version of the uploaded project.
  - No ESP.restart() loop if WiFi fails.
  - EEPROM values are validated.
  - ThingSpeak command values are accepted only when they are 0 or 1.
  - Relay output polarity is configurable from RELAY_ACTIVE_HIGH (0 = Active LOW, 1 = Active HIGH).

  ESP-01 pin warning:
  - GPIO0 must be HIGH during boot.
  - GPIO2 must be HIGH during boot.
  Recommended pin layout in this version:
  - DHT11/DHT22 or DS18B20 = GPIO2 with pull-up to 3.3V.
  - Output 1 = GPIO0. Keep HIGH at boot; avoid loads that pull it LOW.
  - Output 2 = GPIO3/RX. This is the compromise pin; disconnect output load while uploading if needed.
  - GPIO1/TX used as Smart Output 3. Default mode is internet status LED. Serial debug is ON when DEBUG_MODE = 1.
  - Optional long-press button uses the same DHT data pin GPIO2 to GND. Do not hold it while powering/resetting.
  - OTA is optional and uses ArduinoOTA plus Web OTA. First upload must be by USB/Serial, then update by WiFi or browser.
*/

// ========================= USER SETTINGS =========================

// Firmware Version Control
// Change only these two lines for every new firmware release.
#define FW_VERSION         "1.0.0"
#define FW_BUILD           100
#define FW_NAME            "Ahmed Monem Smart Control System"
#define FW_DEVICE_NAME     "Weather and Control "
#define FW_FULL_NAME       "Ahmed Control System +201004608852 . V" FW_VERSION " EEPROM Backup Restore By Ahmed Monem "

// GitHub Auto OTA Update Settings
#define ENABLE_AUTO_UPDATE 1     // 1 = Check GitHub for newer firmware / 0 = OFF.
#define AUTO_UPDATE_ON_BOOT 1    // 1 = Check once after WiFi connection at boot.
#define AUTO_UPDATE_EVERY_HOUR 1 // 1 = Repeat GitHub check every hour while STA WiFi is connected.
const char* UPDATE_INFO_URL = "https://raw.githubusercontent.com/ahmedaaa8-bot/esp01-ota/main/version.json";
// version.json example:
// {
//   "version": "1.0.6",
//   "build": 106,
//   "bin": "https://raw.githubusercontent.com/ahmedaaa8-bot/esp01-ota/main/firmware.bin",
//   "md5": "",
//   "notes": "Short update note"
// }
const unsigned long AUTO_UPDATE_BOOT_DELAY_MS = 20000UL; // V1.1 PRO: safer boot delay before first check.
const unsigned long AUTO_UPDATE_INTERVAL_MS = 3600000UL;  // 1 hour periodic GitHub check.
#define REQUIRE_WEB_LOGIN_FOR_GITHUB_UPDATE 1 // Manual GitHub update button is protected by Admin role.
#define ENABLE_UPDATE_MD5  0     // SPACE OPT: 0 saves flash. Use 1 only if you really need md5 check in version.json.
#define ENABLE_UPDATE_NOTES 0    // SPACE OPT: 0 saves flash. Use 1 only if you want notes text from version.json.

#define DEBUG_MODE         0     // 1 = Serial Debug ON / LED OFF. 0 = Serial OFF / Internet LED ON.
#define DEBUG_SERIAL       DEBUG_MODE
#define ENABLE_STATUS_LED  (!DEBUG_MODE) // LED works only when DEBUG_MODE is 0.

// OTA Settings
#define ENABLE_OTA         0     // SPACE OPT: 0 disables ArduinoOTA to save flash. Web OTA + GitHub OTA still work.
const char* OTA_HOSTNAME = "ESP01-ThingSpeak";
const char* OTA_PASSWORD = "";  // Optional. Leave empty for no OTA password.

// Web OTA Settings
#define ENABLE_WEB_OTA     1     // 1 = Browser update page ON / 0 = OFF. Open http://ESP_IP/update
const char* WEB_OTA_PATH = "/update";
const char* WEB_OTA_USER = "admin";    // Legacy label only. V1.2.18 login uses WEB_USERS table.
const char* WEB_OTA_PASS = "admin";    // Legacy label only. Change users from User Management page.

// Web users / roles / permissions (V1.2.37 Users30)
// ESP-01 light storage: 30 named users in EEPROM, Basic Auth, role-based server-side protection.
#define WEB_ROLE_VIEWER   1  // View sensors/status/weather only
#define WEB_ROLE_OPERATOR 2  // Viewer + output ON/OFF control
#define WEB_ROLE_ENGINEER 3  // Operator + WiFi/ThingSpeak/sensor/smart settings
#define WEB_ROLE_ADMIN    4  // Full control + OTA + user management
#define WEB_USER_COUNT    30
#define WEB_USER_MAX_LEN  12
#define WEB_PASS_MAX_LEN  12
struct WebUserAccount { char user[WEB_USER_MAX_LEN + 1]; char pass[WEB_PASS_MAX_LEN + 1]; byte role; byte enabled; };
WebUserAccount WEB_USERS[WEB_USER_COUNT] = {
  {"admin", "admin", WEB_ROLE_ADMIN, 1},
  {"operator1", "1234", WEB_ROLE_OPERATOR, 1},
  {"guest", "0000", WEB_ROLE_VIEWER, 1}
};
byte currentWebRole = WEB_ROLE_VIEWER;
byte currentWebUserIndex = 255;
char lastLoginUser[WEB_USER_MAX_LEN + 1] = "none";

char lastLoginIp[16] = "-";

// Multi Session Cookie Login (V1.2.40)
// 5 simultaneous devices. A new login kicks the oldest active session.
#define MAX_WEB_SESSIONS 5
#define WEB_SESSION_TIMEOUT_MS 1800000UL

struct WebSessionSlot {
  String token;
  byte userIndex;
  unsigned long createdMs;
  unsigned long lastSeenMs;
  IPAddress ip;
  bool active;
};

WebSessionSlot webSessions[MAX_WEB_SESSIONS];
String webSessionToken = ""; // current request/session token for page rendering
unsigned long webSessionLastSeen = 0;
byte currentSessionSlot = 255;


// ESP01 Access Point / WiFi Manager Settings
const char* DEFAULT_AP_SSID = "Weather And Control System ";
const char* DEFAULT_AP_PASS = "12341234";
const char* WIFI_MANAGER_PATH = "/wifi";
const byte WIFI_MODE_AP_STA_ALWAYS = 0;
const byte WIFI_MODE_STA_FALLBACK  = 1;

// Pin Definitions
#define DHTPIN             2     // GPIO2. Best choice for DHT on ESP-01. Use 10k pull-up to 3.3V.
#define DHT_TYPE_SELECT    0     // 0 = DHT11 / 1 = DHT22

#if DHT_TYPE_SELECT
  #define DHTTYPE          DHT22
#else
  #define DHTTYPE          DHT11
#endif
#define OUTPUT1_PIN        0     // GPIO0. Output 1. pump Must stay HIGH during boot.
#define OUTPUT2_PIN        3     // GPIO3/RX. Output 2. lamp Disconnect load while uploading if upload fails.
#define STATUS_LED_PIN     1     // GPIO1/TX on many ESP-01 boards, active LOW blue LED.

#define RELAY_ACTIVE_HIGH  0     // 0 = Active LOW: ON=LOW, OFF=HIGH. 1 = Active HIGH: ON=HIGH, OFF=LOW.

// WiFi Credentials
const char* ssid     = "aresco2";
const char* password = "@resco2025";

// ThingSpeak Settings
const unsigned long DEFAULT_CHANNEL_ID = 803096UL;
const char* DEFAULT_WRITE_API_KEY = "VMQXNV3MVUQFCXT8";
const char* DEFAULT_READ_API_KEY  = "QC41GWAVWKOYDGT1";

// ThingSpeak Field Numbers
// V1.2.21+: every write sends all 8 fields together, not only the changed output.
#define FIELD1_OUTPUT1 1
#define FIELD2_OUTPUT2 2
#define FIELD3_TEMP    3
#define FIELD4_HUM     4
#define FIELD5_LOGIN_USER 5  // Logged web user name. Public/Application IP is intentionally not sent.
#define FIELD6_TEMP    6
#define FIELD7_OUTPUT1 7
#define FIELD8_OUTPUT2 8

// Timing Configuration
const unsigned long SENSOR_UPDATE_INTERVAL  = 60000UL; // 60 seconds
const unsigned long DEFAULT_COMMAND_CHECK_INTERVAL  = 20000UL; // default 20 seconds; runtime value is configurable from web
const unsigned long WIFI_RETRY_INTERVAL     = 60000UL; // V1.2.17: 60 seconds smart reconnect without blocking web
const unsigned long WIFI_CONNECT_TIMEOUT    = 8000UL;  // Kept for compatibility; V1.2.17 WiFi connect is non-blocking
const unsigned long DHT_MIN_READ_INTERVAL   = 2500UL;  // Safe spacing for DHT11/DHT22
const byte DHT_READ_RETRIES                 = 3;       // Try DHT read 3 times before using fail value
const unsigned long DHT_RETRY_DELAY_MS      = 250UL;   // Small delay between DHT retries
const unsigned long SMART_SENSOR_CONTROL_INTERVAL = 5000UL; // V1.2.26: refresh sensor for Temp/Humidity Out3 control without waiting for ThingSpeak
const unsigned long WEB_STATUS_SENSOR_CACHE_INTERVAL = 5000UL; // V1.2.35: web /status uses cached sensor values; refresh cache in loop
const unsigned long SENSOR_CONTROL_STALE_MS = 15000UL; // V1.2.35: if no fresh sensor read for Out3 control, force Out3 OFF

// Manual button on DHT data pin GPIO2
// Wiring: one side of push button to GPIO2/DHT DATA, the other side to GND.
// Keep the normal 10k pull-up from GPIO2/DHT DATA to 3.3V. Do not press during boot/reset.
#define ENABLE_DHT_PIN_BUTTON 1
const unsigned long BUTTON_LONG_PRESS_MS    = 3000UL;  // Hold for 3 seconds to toggle both outputs
const unsigned long OUTPUT_UPLOAD_RETRY_MS  = 20000UL; // Retry pending ThingSpeak output update. While pending, remote reads are blocked.
const unsigned long THINGSPEAK_MIN_WRITE_MS = 20000UL; // V1.2.21: keep at least 20 seconds between full ThingSpeak writes

// EEPROM Configuration
// EEPROM Backup / Restore (V1.2.42)
// Binary backup is light and exact. Restore requires same EEPROM_SIZE/map version.
#define EEPROM_BACKUP_MAGIC "ESP01CF"
#define EEPROM_BACKUP_MAP_VERSION 1252

// V1.2.41 EEPROM CLEAN MAP NOTE:
// EEPROM_SIZE increased to 2048 bytes to remove old overlaps.
// Important: after flashing this version, do Factory Reset once, then re-enter settings.
// Fixed overlaps:
// - Weather moved away from Smart Output block.
// - DuckDNS/WebPort moved away from Users block.
// - Clock/TZ/Static IP moved to a safe area after DuckDNS.

#define EEPROM_SIZE          2048
#define EEPROM_OUTPUT1_ADDR  0
#define EEPROM_OUTPUT2_ADDR  1
#define EEPROM_PENDING_ADDR  2
#define EEPROM_WIFI_MAGIC_ADDR  8
#define EEPROM_WIFI_MODE_ADDR   9
#define EEPROM_WIFI_SSID_ADDR   16
#define EEPROM_WIFI_PASS_ADDR   64
#define EEPROM_WIFI_MAGIC       0xA5
#define WIFI_SSID_MAX_LEN       32
#define WIFI_PASS_MAX_LEN       64

#define EEPROM_STATIC_MAGIC_ADDR 1490
#define EEPROM_STATIC_ENABLE_ADDR 1491
#define EEPROM_STATIC_IP_ADDR    1494
#define EEPROM_STATIC_GW_ADDR    1510
#define EEPROM_STATIC_SUB_ADDR   1526
#define EEPROM_STATIC_DNS_ADDR   1542
#define EEPROM_STATIC_MAGIC      0x5A
#define STATIC_IP_TEXT_MAX_LEN   15

#define EEPROM_TS_MAGIC_ADDR     132
#define EEPROM_TS_CHANNEL_ADDR   136
#define EEPROM_TS_WRITE_ADDR     160
#define EEPROM_TS_READ_ADDR      200
#define EEPROM_TS_MAGIC          0xB6
#define TS_CHANNEL_MAX_LEN       16
#define TS_KEY_MAX_LEN           32

#define EEPROM_DUCK_MAGIC_ADDR   1376
#define EEPROM_DUCK_DOMAIN_ADDR  1380
#define EEPROM_DUCK_TOKEN_ADDR   1416
#define EEPROM_DUCK_MAGIC        0xDB

#define EEPROM_DEV_MAGIC_ADDR    240
#define EEPROM_DEV_NAME_ADDR     244
#define EEPROM_DEV_APSSID_ADDR   280
#define EEPROM_DEV_APPASS_ADDR   320
#define EEPROM_DEV_MAGIC         0xC7
#define DEVICE_NAME_MAX_LEN      32
#define AP_SSID_MAX_LEN          32
#define AP_PASS_MAX_LEN          64

#define EEPROM_DHT_MAGIC_ADDR    392
#define EEPROM_DHT_TYPE_ADDR     393
#define EEPROM_DHT_MAGIC         0xD8
#define SENSOR_TYPE_DHT11        11
#define SENSOR_TYPE_DHT22        22
#define SENSOR_TYPE_DS18B20      18


// Web Server Port EEPROM Configuration (V1.2.39)
#define EEPROM_WEBPORT_MAGIC_ADDR  1368
#define EEPROM_WEBPORT_VALUE_ADDR  1370
#define EEPROM_WEBPORT_MAGIC       0x9B
uint16_t webServerPort = 80;

// External City Weather EEPROM Configuration
#define EEPROM_WEATHER_MAGIC_ADDR 500
#define EEPROM_WEATHER_CITY_ADDR  501
#define EEPROM_WEATHER_MAGIC      0xA9

// Smart Output 3 EEPROM Configuration (GPIO1/TX)
#define EEPROM_SMART_MAGIC_ADDR   400
#define EEPROM_SMART_DATA_ADDR    404
#define EEPROM_SMART_MAGIC        0xE6  // dual schedule slots per day

// Web User Management EEPROM Configuration (V1.2.37)
// 30 compact user records. EEPROM clock/timezone block moved after user area to avoid overlap.
#define EEPROM_USER_MAGIC_ADDR   520
#define EEPROM_USER_DATA_ADDR    524
#define EEPROM_USER_MAGIC        0xF2
#define EEPROM_USER_RECORD_SIZE  28
#define EEPROM_LAST_USER_ADDR    (EEPROM_USER_DATA_ADDR + (WEB_USER_COUNT * EEPROM_USER_RECORD_SIZE))

// Offline Schedule Clock EEPROM Configuration (V1.2.23)
// Stores the last known valid UTC epoch so schedule can continue without internet.
#define EEPROM_CLOCK_MAGIC_ADDR  1470
#define EEPROM_CLOCK_EPOCH_ADDR  1472
#define EEPROM_CLOCK_MAGIC       0xC3
const unsigned long OFFLINE_CLOCK_SAVE_INTERVAL_MS = 600000UL; // save valid clock at most every 10 minutes

// Egypt Summer/Winter Time Configuration (V1.2.28)
// 0 = Auto Egypt DST, 1 = Manual Winter UTC+2, 2 = Manual Summer UTC+3.
#define EEPROM_TZ_MAGIC_ADDR     1480
#define EEPROM_TZ_MODE_ADDR      1481
#define EEPROM_TZ_MAGIC          0xD7
// Web Performance Settings EEPROM Configuration (V1.2.52)
// Safe area after Static IP block. Does not overlap old settings.
#define EEPROM_WEBPERF_MAGIC_ADDR       1560
#define EEPROM_WEBPERF_GH_ENABLE_ADDR   1561
#define EEPROM_WEBPERF_GH_INTERVAL_ADDR 1562
#define EEPROM_WEBPERF_TS_INTERVAL_ADDR 1563
#define EEPROM_WEBPERF_MAGIC            0xAC

#define GH_AUTO_INTERVAL_MANUAL 0
#define GH_AUTO_INTERVAL_1H     1
#define GH_AUTO_INTERVAL_6H     2
#define GH_AUTO_INTERVAL_12H    3
#define GH_AUTO_INTERVAL_24H    4

#define TS_CMD_INTERVAL_20S     20
#define TS_CMD_INTERVAL_40S     40
#define TS_CMD_INTERVAL_60S     60
#define TS_CMD_INTERVAL_120S    120

#define EGYPT_TZ_AUTO            0
#define EGYPT_TZ_WINTER          1
#define EGYPT_TZ_SUMMER          2

// Smart Output 3 modes
#define SMART_MODE_INTERNET_LED   0
#define SMART_MODE_MANUAL_SCHED   1
#define SMART_MODE_TEMPERATURE    2
#define SMART_MODE_HUMIDITY       3
#define SMART_MODE_TIMER          4
#define SMART_MODE_MAX            SMART_MODE_TIMER

// NTP time for schedule mode
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
const long  LOCAL_GMT_OFFSET_SEC = 0; // V1.2.28: keep SNTP as UTC, apply Egypt DST manually for schedule/dashboard.
const int   LOCAL_DST_OFFSET_SEC = 0;

// External weather from Open-Meteo, no API key required. Used only for city display in the web UI.
#define ENABLE_CITY_WEATHER 1
const unsigned long CITY_WEATHER_INTERVAL_MS = 600000UL; // 10 minutes. Do not request too often on ESP01.
const unsigned long CITY_WEATHER_BOOT_DELAY_MS = 30000UL;

// Public/Application IP removed from ThingSpeak in V1.2.53.
// Keep this disabled to avoid boot delays and avoid sending IP to ThingSpeak.
#define ENABLE_PUBLIC_IP 0
const unsigned long PUBLIC_IP_INTERVAL_MS = 600000UL; // kept for compatibility only
const unsigned long PUBLIC_IP_BOOT_DELAY_MS = 15000UL; // kept for compatibility only
const char* PUBLIC_IP_URL = "";

// DuckDNS Auto Public IP Update (V1.2.36)
// 1) Make a domain at https://www.duckdns.org
// 2) Put ONLY the subdomain here, without .duckdns.org
//    Example: ahmedhome   -> opens as ahmedhome.duckdns.org
// 3) Put your DuckDNS token here.
// If either value is empty, DuckDNS update is safely skipped.
#define ENABLE_DUCKDNS 1
#define DUCK_DOMAIN_MAX_LEN 32
#define DUCK_TOKEN_MAX_LEN  48
const char* DEFAULT_DUCKDNS_DOMAIN = "amonem";   // optional fallback, without .duckdns.org
const char* DEFAULT_DUCKDNS_TOKEN  = "15514d54-46d5-47a7-a0dc-5a964856e992";   // optional fallback token
char duckDnsDomain[DUCK_DOMAIN_MAX_LEN + 1] = "";
char duckDnsToken[DUCK_TOKEN_MAX_LEN + 1] = "";
const unsigned long DUCKDNS_INTERVAL_MS = 600000UL; // 10 minutes
const unsigned long DUCKDNS_BOOT_DELAY_MS = 30000UL; // wait after boot/WiFi before first update

struct WeatherCity {
  const char* name;
  const char* lat;
  const char* lon;
};

const WeatherCity WEATHER_CITIES[] = {
  {"Cairo",        "30.0444", "31.2357"},
  {"Giza",         "30.0131", "31.2089"},
  {"Alexandria",   "31.2001", "29.9187"},
  {"Mansoura",     "31.0409", "31.3785"},
  {"Aswan",        "24.0889", "32.8998"},
  {"6 October",    "29.9285", "30.9188"},
  {"Ras Sudr",     "29.5940", "32.7180"},
  {"Helwan",       "29.8414", "31.3008"},
  {"Port Said",    "31.2653", "32.3019"},
  {"Marsa Matruh", "31.3543", "27.2373"},
  {"Hurghada",     "27.2579", "33.8116"},
  {"North Coast",  "30.8333", "28.9550"},
  {"Sharm Sheikh", "27.9158", "34.3300"},
  {"Suez",         "29.9668", "32.5498"},
  {"Ismailia",     "30.5965", "32.2715"}
};
const byte WEATHER_CITY_COUNT = sizeof(WEATHER_CITIES) / sizeof(WEATHER_CITIES[0]);

// Ras Sudr Tide button URL (مد وجزر رأس سدر)
const char* RAS_SEDR_TIDE_URL = "https://almadwaaljazer.com/eg/egypt-red-sea/ras-sedr/forecast/tides";

#define SCHEDULE_SLOTS_PER_DAY 2

struct SmartOutputConfig {
  byte mode;
  byte manualState;
  byte scheduleEnable;
  byte repeatWeekly;
  byte onceDone;
  byte dayEnabled[7];                 // tm_wday: 0=Sun ... 6=Sat. Master enable for the whole day.
  byte slotEnabled[7][SCHEDULE_SLOTS_PER_DAY]; // Independent ON/OFF windows inside the enabled day.
  uint16_t dayStartMin[7][SCHEDULE_SLOTS_PER_DAY]; // minutes from 00:00 for every day/slot
  uint16_t dayEndMin[7][SCHEDULE_SLOTS_PER_DAY];
  int16_t tempOnX10;
  int16_t tempOffX10;
  int16_t humOnX10;
  int16_t humOffX10;
};

// ========================= GLOBALS =========================

DHT dht11Sensor(DHTPIN, DHT11, 15);
DHT dht22Sensor(DHTPIN, DHT22, 15);
OneWire oneWireBus(DHTPIN);
DallasTemperature ds18b20Sensor(&oneWireBus);
WiFiClient client;
WebServer* webServerPtr = nullptr;
#define webServer (*webServerPtr)

byte currentOutput1State = 0;
byte currentOutput2State = 0;

unsigned long lastSensorUpdateTime = 0;
unsigned long lastCommandCheckTime = 0;
unsigned long lastWiFiAttemptTime = 0;
unsigned long lastDhtReadTime = 0;
unsigned long lastSmartSensorControlReadMs = 0;
unsigned long lastWebStatusSensorCacheMs = 0;
unsigned long lastValidSensorReadMs = 0;
unsigned long lastLedBlinkTime = 0;
bool ledBlinkState = false;
bool printedOfflineEepromMode = false;
bool printedOnlineThingSpeakMode = false;

unsigned long buttonPressStartTime = 0;
bool buttonLongPressHandled = false;
bool pendingOutputThingSpeakUpdate = false;
unsigned long lastOutputUpdateAttemptTime = 0;
unsigned long lastThingSpeakWriteTime = 0;
bool otaStarted = false;
bool webOtaStarted = false;
char activeSsid[WIFI_SSID_MAX_LEN + 1] = "";
char activePass[WIFI_PASS_MAX_LEN + 1] = "";
byte wifiManagerMode = WIFI_MODE_AP_STA_ALWAYS;
bool wifiStaticEnabled = false;
char wifiStaticIP[STATIC_IP_TEXT_MAX_LEN + 1] = "192.168.100.15";
char wifiStaticGateway[STATIC_IP_TEXT_MAX_LEN + 1] = "192.168.100.1";
char wifiStaticSubnet[STATIC_IP_TEXT_MAX_LEN + 1] = "255.255.255.0";
char wifiStaticDns[STATIC_IP_TEXT_MAX_LEN + 1] = "8.8.8.8";
bool apRunning = false;
char tsChannelId[TS_CHANNEL_MAX_LEN + 1] = "";
char tsWriteKey[TS_KEY_MAX_LEN + 1] = "";
char tsReadKey[TS_KEY_MAX_LEN + 1] = "";
char deviceName[DEVICE_NAME_MAX_LEN + 1] = "";
char configApSsid[AP_SSID_MAX_LEN + 1] = "";
char configApPass[AP_PASS_MAX_LEN + 1] = "";
byte dhtSensorType = SENSOR_TYPE_DHT11;
float currentTemperature = NAN;
float currentHumidity = NAN;
bool sensorReadOk = false;
bool ds18b20Present = false;

SmartOutputConfig smartCfg;
bool smartOutputState = false;
bool smartTimerActive = false;
unsigned long smartTimerStartMs = 0;
uint32_t smartTimerDurationSec = 0;
bool ntpStarted = false;
byte clockState = 0; // 0=NO_TIME, 1=ONLINE_SYNC, 2=OFFLINE_ESTIMATED
uint32_t offlineClockBaseEpoch = 0;
unsigned long offlineClockBaseMs = 0;
unsigned long lastOfflineClockSaveMs = 0;
bool offlineClockLoaded = false;
byte egyptTimeMode = EGYPT_TZ_AUTO;

bool autoUpdateChecked = false;
bool autoUpdateBusy = false;
unsigned long lastAutoUpdateCheckMs = 0;
String lastAutoUpdateMessage = "Not checked yet";

// Web-configurable performance/update options (saved in EEPROM).
bool githubAutoUpdateEnabled = false;       // default OFF to keep ESP01 web responsive
byte githubAutoUpdateIntervalMode = GH_AUTO_INTERVAL_MANUAL;
unsigned long githubAutoUpdateIntervalMs = 0;
unsigned long commandCheckIntervalMs = DEFAULT_COMMAND_CHECK_INTERVAL;

byte weatherCityIndex = 0;
float cityWeatherTemp = NAN;
float cityWeatherHum = NAN;
bool cityWeatherOk = false;
unsigned long lastCityWeatherCheckMs = 0;
String lastCityWeatherMessage = "Not checked yet";
String lastCityWeatherTime = "--";

String publicIP = "Disabled";
int publicIPLastOctet = 0; // disabled in V1.2.53
bool publicIPOk = false;
unsigned long lastPublicIPCheckMs = 0;
unsigned long lastPublicIPCompareMs = 0;
bool publicIPBootCompareDone = false;
String lastThingSpeakPublicIP = "";
String lastPublicIPMessage = "Not checked yet";

unsigned long lastDuckDNSUpdateMs = 0;
String lastDuckDNSMessage = "DuckDNS not checked yet";


// ========================= DEBUG HELPERS =========================

#if DEBUG_SERIAL
  #define DBG_BEGIN(x)     Serial.begin(x)
  #define DBG_PRINT(x)     Serial.print(x)
  #define DBG_PRINTLN(x)   Serial.println(x)
#else
  #define DBG_BEGIN(x)
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
#endif

void debugDumpEepromBytes(const char* label) {
#if DEBUG_SERIAL
  DBG_PRINTLN("==== EEPROM RAW DUMP ====");
  DBG_PRINT("Label: ");
  DBG_PRINTLN(label);
  for (byte i = 0; i < EEPROM_SIZE; i++) {
    DBG_PRINT("Addr ");
    DBG_PRINT(i);
    DBG_PRINT(" = ");
    DBG_PRINTLN(EEPROM.read(i));
  }
  DBG_PRINTLN("=========================");
#endif
}

void debugPrintRuntimeState(const char* label) {
#if DEBUG_SERIAL
  DBG_PRINTLN("==== RUNTIME STATE ====");
  DBG_PRINT("Label: ");
  DBG_PRINTLN(label);
  DBG_PRINT("Output1 current = ");
  DBG_PRINTLN(currentOutput1State ? "ON" : "OFF");
  DBG_PRINT("Output2 current = ");
  DBG_PRINTLN(currentOutput2State ? "ON" : "OFF");
  DBG_PRINT("Pending sync    = ");
  DBG_PRINTLN(pendingOutputThingSpeakUpdate ? "YES" : "NO");
  DBG_PRINT("WiFi status     = ");
  DBG_PRINTLN(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
  DBG_PRINT("Relay mode      = ");
#if RELAY_ACTIVE_HIGH
  DBG_PRINTLN("ACTIVE HIGH / ON = HIGH");
#else
  DBG_PRINTLN("ACTIVE LOW / ON = LOW");
#endif
  DBG_PRINTLN("=======================");
#endif
}

// ========================= BASIC HELPERS =========================

bool isValidState(long value) {
  return value == 0 || value == 1;
}

byte sanitizeState(byte value, byte defaultValue = 0) {
  return (value == 0 || value == 1) ? value : defaultValue;
}

int outputLevelForState(byte state) {
#if RELAY_ACTIVE_HIGH
  return state == 1 ? HIGH : LOW;
#else
  return state == 1 ? LOW : HIGH;
#endif
}

void setRelay(uint8_t pin, byte state) {
  digitalWrite(pin, outputLevelForState(state));
}

void writeStatusLed(bool on) {
#if ENABLE_STATUS_LED
  // V1.2.27: GPIO1/TX is shared. The WiFi/internet LED is allowed to touch it
  // only in INTERNET_LED mode. In Manual/Schedule/Temp/Humidity/Timer modes,
  // Out3 owns the pin and must not be overwritten by WiFi reconnect/status code.
  if (smartCfg.mode != SMART_MODE_INTERNET_LED) return;
  digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
#endif
}

void applyOutputs() {
  setRelay(OUTPUT1_PIN, currentOutput1State);
  setRelay(OUTPUT2_PIN, currentOutput2State);

  DBG_PRINTLN("==== APPLY OUTPUTS ====");
  DBG_PRINT("Output1 GPIO");
  DBG_PRINT(OUTPUT1_PIN);
  DBG_PRINT(" = ");
  DBG_PRINTLN(currentOutput1State ? "ON" : "OFF");
  DBG_PRINT("Output2 GPIO");
  DBG_PRINT(OUTPUT2_PIN);
  DBG_PRINT(" = ");
  DBG_PRINTLN(currentOutput2State ? "ON" : "OFF");
  DBG_PRINTLN("=======================");
}

void saveOutputState(byte outputNumber, byte state) {
  int addr = (outputNumber == 1) ? EEPROM_OUTPUT1_ADDR : EEPROM_OUTPUT2_ADDR;
  byte oldValue = EEPROM.read(addr);

  DBG_PRINTLN("==== EEPROM SAVE REQUEST ====");
  DBG_PRINT("Output number : ");
  DBG_PRINTLN(outputNumber);
  DBG_PRINT("EEPROM address: ");
  DBG_PRINTLN(addr);
  DBG_PRINT("Old EEPROM val: ");
  DBG_PRINTLN(oldValue);
  DBG_PRINT("New state     : ");
  DBG_PRINTLN(state);

  if (oldValue != state) {
    EEPROM.write(addr, state);
    bool ok = EEPROM.commit();
    DBG_PRINT("EEPROM commit : ");
    DBG_PRINTLN(ok ? "OK" : "FAILED");
    DBG_PRINT("Verify read   : ");
    DBG_PRINTLN(EEPROM.read(addr));
  } else {
    DBG_PRINTLN("EEPROM write skipped: value already stored");
  }

  DBG_PRINTLN("=============================");
}

void loadOutputStates() {
  DBG_PRINTLN("==== EEPROM LOAD AT BOOT ====");

  byte rawOutput1 = EEPROM.read(EEPROM_OUTPUT1_ADDR);
  byte rawOutput2 = EEPROM.read(EEPROM_OUTPUT2_ADDR);

  DBG_PRINT("Raw EEPROM Output1 @addr ");
  DBG_PRINT(EEPROM_OUTPUT1_ADDR);
  DBG_PRINT(" = ");
  DBG_PRINTLN(rawOutput1);

  DBG_PRINT("Raw EEPROM Output2 @addr ");
  DBG_PRINT(EEPROM_OUTPUT2_ADDR);
  DBG_PRINT(" = ");
  DBG_PRINTLN(rawOutput2);

  currentOutput1State = sanitizeState(rawOutput1, 0);
  currentOutput2State = sanitizeState(rawOutput2, 0);

  if (rawOutput1 != currentOutput1State) {
    DBG_PRINTLN("Output1 EEPROM value invalid -> repaired to 0");
  }
  if (rawOutput2 != currentOutput2State) {
    DBG_PRINTLN("Output2 EEPROM value invalid -> repaired to 0");
  }

  DBG_PRINT("Loaded Output1 state = ");
  DBG_PRINTLN(currentOutput1State ? "ON" : "OFF");
  DBG_PRINT("Loaded Output2 state = ");
  DBG_PRINTLN(currentOutput2State ? "ON" : "OFF");

  // Repair empty/uninitialized EEPROM values.
  saveOutputState(1, currentOutput1State);
  saveOutputState(2, currentOutput2State);

  DBG_PRINTLN("EEPROM load completed. These states will be applied even if WiFi is offline.");
  DBG_PRINTLN("=============================");
}


void savePendingSyncFlag(bool pending) {
  byte value = pending ? 1 : 0;
  byte oldValue = EEPROM.read(EEPROM_PENDING_ADDR);

  DBG_PRINTLN("==== EEPROM PENDING FLAG SAVE ====");
  DBG_PRINT("Old pending flag: ");
  DBG_PRINTLN(oldValue);
  DBG_PRINT("New pending flag: ");
  DBG_PRINTLN(value);

  if (oldValue != value) {
    EEPROM.write(EEPROM_PENDING_ADDR, value);
    bool ok = EEPROM.commit();
    DBG_PRINT("Pending flag commit: ");
    DBG_PRINTLN(ok ? "OK" : "FAILED");
    DBG_PRINT("Verify pending flag: ");
    DBG_PRINTLN(EEPROM.read(EEPROM_PENDING_ADDR));
  } else {
    DBG_PRINTLN("Pending flag write skipped: already same value");
  }

  DBG_PRINTLN("==================================");
}

void loadPendingSyncFlag() {
  byte rawPending = EEPROM.read(EEPROM_PENDING_ADDR);
  pendingOutputThingSpeakUpdate = (rawPending == 1);

  DBG_PRINTLN("==== EEPROM PENDING FLAG LOAD ====");
  DBG_PRINT("Raw pending flag @addr ");
  DBG_PRINT(EEPROM_PENDING_ADDR);
  DBG_PRINT(" = ");
  DBG_PRINTLN(rawPending);

  if (rawPending != 0 && rawPending != 1) {
    DBG_PRINTLN("Pending flag invalid -> repaired to 0");
    pendingOutputThingSpeakUpdate = false;
    savePendingSyncFlag(false);
  }

  DBG_PRINT("Pending local sync loaded = ");
  DBG_PRINTLN(pendingOutputThingSpeakUpdate ? "YES" : "NO");
  if (pendingOutputThingSpeakUpdate) {
    DBG_PRINTLN("Local EEPROM state has priority. Remote ThingSpeak reads will be skipped until sync succeeds.");
  }
  DBG_PRINTLN("==================================");
}


// ========================= WIFI MANAGER STORAGE =========================

String readEepromString(int startAddr, int maxLen) {
  char buf[65];
  int safeLen = maxLen;
  if (safeLen > 64) safeLen = 64;
  for (int i = 0; i < safeLen; i++) {
    byte c = EEPROM.read(startAddr + i);
    if (c == 0 || c == 255) {
      buf[i] = '\0';
      return String(buf);
    }
    buf[i] = (char)c;
  }
  buf[safeLen] = '\0';
  return String(buf);
}

void writeEepromString(int startAddr, int maxLen, const String& value) {
  int n = value.length();
  if (n > maxLen - 1) n = maxLen - 1;
  for (int i = 0; i < maxLen; i++) {
    byte c = (i < n) ? value[i] : 0;
    EEPROM.write(startAddr + i, c);
  }
}

void loadWiFiManagerConfig() {
  byte magic = EEPROM.read(EEPROM_WIFI_MAGIC_ADDR);
  byte modeRaw = EEPROM.read(EEPROM_WIFI_MODE_ADDR);
  wifiManagerMode = (modeRaw == WIFI_MODE_STA_FALLBACK) ? WIFI_MODE_STA_FALLBACK : WIFI_MODE_AP_STA_ALWAYS;

  if (magic == EEPROM_WIFI_MAGIC) {
    String es = readEepromString(EEPROM_WIFI_SSID_ADDR, WIFI_SSID_MAX_LEN + 1);
    String ep = readEepromString(EEPROM_WIFI_PASS_ADDR, WIFI_PASS_MAX_LEN + 1);
    es.toCharArray(activeSsid, sizeof(activeSsid));
    ep.toCharArray(activePass, sizeof(activePass));
  } else {
    strncpy(activeSsid, ssid, WIFI_SSID_MAX_LEN);
    activeSsid[WIFI_SSID_MAX_LEN] = '\0';
    strncpy(activePass, password, WIFI_PASS_MAX_LEN);
    activePass[WIFI_PASS_MAX_LEN] = '\0';
  }

  DBG_PRINT("WiFi manager mode = ");
  DBG_PRINTLN(wifiManagerMode == WIFI_MODE_AP_STA_ALWAYS ? "0 AP+STA always" : "1 STA fallback");
  DBG_PRINT("Active SSID = ");
  DBG_PRINTLN(activeSsid[0] ? activeSsid : "<empty>");
}

void saveWiFiManagerConfig(const String& newSsid, const String& newPass, byte newMode) {
  wifiManagerMode = (newMode == WIFI_MODE_STA_FALLBACK) ? WIFI_MODE_STA_FALLBACK : WIFI_MODE_AP_STA_ALWAYS;
  newSsid.toCharArray(activeSsid, sizeof(activeSsid));
  newPass.toCharArray(activePass, sizeof(activePass));

  EEPROM.write(EEPROM_WIFI_MAGIC_ADDR, EEPROM_WIFI_MAGIC);
  EEPROM.write(EEPROM_WIFI_MODE_ADDR, wifiManagerMode);
  writeEepromString(EEPROM_WIFI_SSID_ADDR, WIFI_SSID_MAX_LEN + 1, newSsid);
  writeEepromString(EEPROM_WIFI_PASS_ADDR, WIFI_PASS_MAX_LEN + 1, newPass);
  EEPROM.commit();
}

void forgetWiFiManagerConfig() {
  activeSsid[0] = '\0';
  activePass[0] = '\0';
  wifiManagerMode = WIFI_MODE_AP_STA_ALWAYS;
  EEPROM.write(EEPROM_WIFI_MAGIC_ADDR, EEPROM_WIFI_MAGIC);
  EEPROM.write(EEPROM_WIFI_MODE_ADDR, wifiManagerMode);
  writeEepromString(EEPROM_WIFI_SSID_ADDR, WIFI_SSID_MAX_LEN + 1, "");
  writeEepromString(EEPROM_WIFI_PASS_ADDR, WIFI_PASS_MAX_LEN + 1, "");
  EEPROM.commit();
}

bool parseIPText(const char* txt, IPAddress& out) {
  if (txt == nullptr || txt[0] == '\0') return false;
  return out.fromString(String(txt));
}

void loadStaticIpConfig() {
  byte magic = EEPROM.read(EEPROM_STATIC_MAGIC_ADDR);
  if (magic == EEPROM_STATIC_MAGIC) {
    wifiStaticEnabled = (EEPROM.read(EEPROM_STATIC_ENABLE_ADDR) == 1);
    readEepromString(EEPROM_STATIC_IP_ADDR, STATIC_IP_TEXT_MAX_LEN + 1).toCharArray(wifiStaticIP, sizeof(wifiStaticIP));
    readEepromString(EEPROM_STATIC_GW_ADDR, STATIC_IP_TEXT_MAX_LEN + 1).toCharArray(wifiStaticGateway, sizeof(wifiStaticGateway));
    readEepromString(EEPROM_STATIC_SUB_ADDR, STATIC_IP_TEXT_MAX_LEN + 1).toCharArray(wifiStaticSubnet, sizeof(wifiStaticSubnet));
    readEepromString(EEPROM_STATIC_DNS_ADDR, STATIC_IP_TEXT_MAX_LEN + 1).toCharArray(wifiStaticDns, sizeof(wifiStaticDns));
  }

  if (wifiStaticIP[0] == '\0') strncpy(wifiStaticIP, "192.168.100.15", STATIC_IP_TEXT_MAX_LEN);
  if (wifiStaticGateway[0] == '\0') strncpy(wifiStaticGateway, "192.168.100.1", STATIC_IP_TEXT_MAX_LEN);
  if (wifiStaticSubnet[0] == '\0') strncpy(wifiStaticSubnet, "255.255.255.0", STATIC_IP_TEXT_MAX_LEN);
  if (wifiStaticDns[0] == '\0') strncpy(wifiStaticDns, "8.8.8.8", STATIC_IP_TEXT_MAX_LEN);

  wifiStaticIP[STATIC_IP_TEXT_MAX_LEN] = '\0';
  wifiStaticGateway[STATIC_IP_TEXT_MAX_LEN] = '\0';
  wifiStaticSubnet[STATIC_IP_TEXT_MAX_LEN] = '\0';
  wifiStaticDns[STATIC_IP_TEXT_MAX_LEN] = '\0';
}

bool saveStaticIpConfig(bool enabled, const String& ip, const String& gw, const String& sub, const String& dns) {
  IPAddress test;
  String sip = ip;  sip.trim();
  String sgw = gw;  sgw.trim();
  String ssb = sub; ssb.trim();
  String sdn = dns; sdn.trim();

  if (enabled) {
    if (!test.fromString(sip)) return false;
    if (!test.fromString(sgw)) return false;
    if (!test.fromString(ssb)) return false;
    if (!test.fromString(sdn)) return false;
  }

  wifiStaticEnabled = enabled;
  sip.toCharArray(wifiStaticIP, sizeof(wifiStaticIP));
  sgw.toCharArray(wifiStaticGateway, sizeof(wifiStaticGateway));
  ssb.toCharArray(wifiStaticSubnet, sizeof(wifiStaticSubnet));
  sdn.toCharArray(wifiStaticDns, sizeof(wifiStaticDns));

  EEPROM.write(EEPROM_STATIC_MAGIC_ADDR, EEPROM_STATIC_MAGIC);
  EEPROM.write(EEPROM_STATIC_ENABLE_ADDR, wifiStaticEnabled ? 1 : 0);
  writeEepromString(EEPROM_STATIC_IP_ADDR, STATIC_IP_TEXT_MAX_LEN + 1, sip);
  writeEepromString(EEPROM_STATIC_GW_ADDR, STATIC_IP_TEXT_MAX_LEN + 1, sgw);
  writeEepromString(EEPROM_STATIC_SUB_ADDR, STATIC_IP_TEXT_MAX_LEN + 1, ssb);
  writeEepromString(EEPROM_STATIC_DNS_ADDR, STATIC_IP_TEXT_MAX_LEN + 1, sdn);
  EEPROM.commit();
  return true;
}

bool applyStaticIpBeforeConnect() {
  if (!wifiStaticEnabled) {
    WiFi.config(
  IPAddress((uint32_t)0),
  IPAddress((uint32_t)0),
  IPAddress((uint32_t)0)
);
    return true;
  }

  IPAddress ip, gw, sub, dns;
  if (!parseIPText(wifiStaticIP, ip) ||
      !parseIPText(wifiStaticGateway, gw) ||
      !parseIPText(wifiStaticSubnet, sub) ||
      !parseIPText(wifiStaticDns, dns)) {
    DBG_PRINTLN("Static IP config invalid -> falling back to DHCP");
    WiFi.config(
  IPAddress((uint32_t)0),
  IPAddress((uint32_t)0),
  IPAddress((uint32_t)0)
);
    return false;
  }

  DBG_PRINT("Applying Static IP: ");
  DBG_PRINTLN(wifiStaticIP);
  return WiFi.config(ip, gw, sub, dns);
}


// ========================= THINGSPEAK + DEVICE STORAGE =========================

void loadThingSpeakConfig() {
  byte magic = EEPROM.read(EEPROM_TS_MAGIC_ADDR);
  if (magic == EEPROM_TS_MAGIC) {
    readEepromString(EEPROM_TS_CHANNEL_ADDR, TS_CHANNEL_MAX_LEN + 1).toCharArray(tsChannelId, sizeof(tsChannelId));
    readEepromString(EEPROM_TS_WRITE_ADDR, TS_KEY_MAX_LEN + 1).toCharArray(tsWriteKey, sizeof(tsWriteKey));
    readEepromString(EEPROM_TS_READ_ADDR, TS_KEY_MAX_LEN + 1).toCharArray(tsReadKey, sizeof(tsReadKey));
  } else {
    String(DEFAULT_CHANNEL_ID).toCharArray(tsChannelId, sizeof(tsChannelId));
    String(DEFAULT_WRITE_API_KEY).toCharArray(tsWriteKey, sizeof(tsWriteKey));
    String(DEFAULT_READ_API_KEY).toCharArray(tsReadKey, sizeof(tsReadKey));
  }
}

void saveThingSpeakConfig(const String& channel, const String& writeKey, const String& readKey) {
  String ch = channel; ch.trim();
  String wk = writeKey; wk.trim();
  String rk = readKey; rk.trim();
  ch.toCharArray(tsChannelId, sizeof(tsChannelId));
  wk.toCharArray(tsWriteKey, sizeof(tsWriteKey));
  rk.toCharArray(tsReadKey, sizeof(tsReadKey));
  EEPROM.write(EEPROM_TS_MAGIC_ADDR, EEPROM_TS_MAGIC);
  writeEepromString(EEPROM_TS_CHANNEL_ADDR, TS_CHANNEL_MAX_LEN + 1, ch);
  writeEepromString(EEPROM_TS_WRITE_ADDR, TS_KEY_MAX_LEN + 1, wk);
  writeEepromString(EEPROM_TS_READ_ADDR, TS_KEY_MAX_LEN + 1, rk);
  EEPROM.commit();
}

void clearThingSpeakConfig() {
  tsChannelId[0] = '\0';
  tsWriteKey[0] = '\0';
  tsReadKey[0] = '\0';
  EEPROM.write(EEPROM_TS_MAGIC_ADDR, EEPROM_TS_MAGIC);
  writeEepromString(EEPROM_TS_CHANNEL_ADDR, TS_CHANNEL_MAX_LEN + 1, "");
  writeEepromString(EEPROM_TS_WRITE_ADDR, TS_KEY_MAX_LEN + 1, "");
  writeEepromString(EEPROM_TS_READ_ADDR, TS_KEY_MAX_LEN + 1, "");
  EEPROM.commit();
}

unsigned long githubIntervalModeToMs(byte mode) {
  switch (mode) {
    case GH_AUTO_INTERVAL_1H:  return 3600000UL;
    case GH_AUTO_INTERVAL_6H:  return 21600000UL;
    case GH_AUTO_INTERVAL_12H: return 43200000UL;
    case GH_AUTO_INTERVAL_24H: return 86400000UL;
    default: return 0UL; // Manual only
  }
}

byte sanitizeGithubIntervalMode(byte mode) {
  if (mode > GH_AUTO_INTERVAL_24H) return GH_AUTO_INTERVAL_MANUAL;
  return mode;
}

byte sanitizeTsCommandIntervalSeconds(unsigned long seconds) {
  if (seconds == TS_CMD_INTERVAL_40S) return TS_CMD_INTERVAL_40S;
  if (seconds == TS_CMD_INTERVAL_60S) return TS_CMD_INTERVAL_60S;
  if (seconds == TS_CMD_INTERVAL_120S) return TS_CMD_INTERVAL_120S;
  return TS_CMD_INTERVAL_20S;
}

void applyWebPerformanceSettings() {
  githubAutoUpdateIntervalMode = sanitizeGithubIntervalMode(githubAutoUpdateIntervalMode);
  githubAutoUpdateIntervalMs = githubIntervalModeToMs(githubAutoUpdateIntervalMode);
  byte tsSec = sanitizeTsCommandIntervalSeconds(commandCheckIntervalMs / 1000UL);
  commandCheckIntervalMs = (unsigned long)tsSec * 1000UL;
}

void loadWebPerformanceSettings() {
  if (EEPROM.read(EEPROM_WEBPERF_MAGIC_ADDR) == EEPROM_WEBPERF_MAGIC) {
    githubAutoUpdateEnabled = EEPROM.read(EEPROM_WEBPERF_GH_ENABLE_ADDR) == 1;
    githubAutoUpdateIntervalMode = sanitizeGithubIntervalMode(EEPROM.read(EEPROM_WEBPERF_GH_INTERVAL_ADDR));
    byte tsSec = sanitizeTsCommandIntervalSeconds(EEPROM.read(EEPROM_WEBPERF_TS_INTERVAL_ADDR));
    commandCheckIntervalMs = (unsigned long)tsSec * 1000UL;
  } else {
    githubAutoUpdateEnabled = false;
    githubAutoUpdateIntervalMode = GH_AUTO_INTERVAL_MANUAL;
    commandCheckIntervalMs = DEFAULT_COMMAND_CHECK_INTERVAL;
  }
  applyWebPerformanceSettings();
}

void saveWebPerformanceSettings(bool ghEnabled, byte ghMode, byte tsSeconds) {
  githubAutoUpdateEnabled = ghEnabled;
  githubAutoUpdateIntervalMode = sanitizeGithubIntervalMode(ghMode);
  commandCheckIntervalMs = (unsigned long)sanitizeTsCommandIntervalSeconds(tsSeconds) * 1000UL;
  applyWebPerformanceSettings();

  EEPROM.write(EEPROM_WEBPERF_MAGIC_ADDR, EEPROM_WEBPERF_MAGIC);
  EEPROM.write(EEPROM_WEBPERF_GH_ENABLE_ADDR, githubAutoUpdateEnabled ? 1 : 0);
  EEPROM.write(EEPROM_WEBPERF_GH_INTERVAL_ADDR, githubAutoUpdateIntervalMode);
  EEPROM.write(EEPROM_WEBPERF_TS_INTERVAL_ADDR, (byte)(commandCheckIntervalMs / 1000UL));
  EEPROM.commit();

  // Reset timers so the new intervals are applied cleanly without a sudden burst.
  autoUpdateChecked = false;
  lastAutoUpdateCheckMs = millis();
  lastCommandCheckTime = millis();
}

String githubIntervalModeText(byte mode) {
  switch (sanitizeGithubIntervalMode(mode)) {
    case GH_AUTO_INTERVAL_1H: return F("1 hour");
    case GH_AUTO_INTERVAL_6H: return F("6 hours");
    case GH_AUTO_INTERVAL_12H: return F("12 hours");
    case GH_AUTO_INTERVAL_24H: return F("24 hours");
    default: return F("Manual only");
  }
}

void loadDuckDnsConfig() {
#if ENABLE_DUCKDNS
  byte magic = EEPROM.read(EEPROM_DUCK_MAGIC_ADDR);
  if (magic == EEPROM_DUCK_MAGIC) {
    readEepromString(EEPROM_DUCK_DOMAIN_ADDR, DUCK_DOMAIN_MAX_LEN + 1).toCharArray(duckDnsDomain, sizeof(duckDnsDomain));
    readEepromString(EEPROM_DUCK_TOKEN_ADDR, DUCK_TOKEN_MAX_LEN + 1).toCharArray(duckDnsToken, sizeof(duckDnsToken));
  } else {
    String(DEFAULT_DUCKDNS_DOMAIN).toCharArray(duckDnsDomain, sizeof(duckDnsDomain));
    String(DEFAULT_DUCKDNS_TOKEN).toCharArray(duckDnsToken, sizeof(duckDnsToken));
  }
#endif
}

void saveDuckDnsConfig(const String& domain, const String& token) {
#if ENABLE_DUCKDNS
  String d = domain; d.trim();
  String t = token;  t.trim();

  d.replace(".duckdns.org", "");
  d.replace("http://", "");
  d.replace("https://", "");
  int slash = d.indexOf('/');
  if (slash >= 0) d = d.substring(0, slash);

  d.toCharArray(duckDnsDomain, sizeof(duckDnsDomain));
  t.toCharArray(duckDnsToken, sizeof(duckDnsToken));

  EEPROM.write(EEPROM_DUCK_MAGIC_ADDR, EEPROM_DUCK_MAGIC);
  writeEepromString(EEPROM_DUCK_DOMAIN_ADDR, DUCK_DOMAIN_MAX_LEN + 1, d);
  writeEepromString(EEPROM_DUCK_TOKEN_ADDR, DUCK_TOKEN_MAX_LEN + 1, t);
  EEPROM.commit();

  lastDuckDNSUpdateMs = 0;
  lastDuckDNSMessage = "DuckDNS saved";
#endif
}

void clearDuckDnsConfig() {
#if ENABLE_DUCKDNS
  duckDnsDomain[0] = '\0';
  duckDnsToken[0] = '\0';

  EEPROM.write(EEPROM_DUCK_MAGIC_ADDR, EEPROM_DUCK_MAGIC);
  writeEepromString(EEPROM_DUCK_DOMAIN_ADDR, DUCK_DOMAIN_MAX_LEN + 1, "");
  writeEepromString(EEPROM_DUCK_TOKEN_ADDR, DUCK_TOKEN_MAX_LEN + 1, "");
  EEPROM.commit();

  lastDuckDNSUpdateMs = 0;
  lastDuckDNSMessage = "DuckDNS cleared";
#endif
}

unsigned long getThingSpeakChannelId() { return String(tsChannelId).toInt(); }

bool thingSpeakConfigured(bool needWrite) {
  if (getThingSpeakChannelId() == 0) return false;
  if (needWrite && tsWriteKey[0] == '\0') return false;
  if (!needWrite && tsReadKey[0] == '\0') return false;
  return true;
}

void loadDeviceConfig() {
  byte magic = EEPROM.read(EEPROM_DEV_MAGIC_ADDR);
  if (magic == EEPROM_DEV_MAGIC) {
    readEepromString(EEPROM_DEV_NAME_ADDR, DEVICE_NAME_MAX_LEN + 1).toCharArray(deviceName, sizeof(deviceName));
    readEepromString(EEPROM_DEV_APSSID_ADDR, AP_SSID_MAX_LEN + 1).toCharArray(configApSsid, sizeof(configApSsid));
    readEepromString(EEPROM_DEV_APPASS_ADDR, AP_PASS_MAX_LEN + 1).toCharArray(configApPass, sizeof(configApPass));
  }
  if (deviceName[0] == '\0') strncpy(deviceName, OTA_HOSTNAME, DEVICE_NAME_MAX_LEN);
  deviceName[DEVICE_NAME_MAX_LEN] = '\0';
  if (configApSsid[0] == '\0') strncpy(configApSsid, DEFAULT_AP_SSID, AP_SSID_MAX_LEN);
  configApSsid[AP_SSID_MAX_LEN] = '\0';
  if (configApPass[0] == '\0') strncpy(configApPass, DEFAULT_AP_PASS, AP_PASS_MAX_LEN);
  configApPass[AP_PASS_MAX_LEN] = '\0';
}

void saveDeviceConfig(const String& name, const String& apName, const String& apPassword) {
  String dn = name; dn.trim();
  String as = apName; as.trim();
  String ap = apPassword;
  if (dn.length() == 0) dn = OTA_HOSTNAME;
  if (as.length() == 0) as = DEFAULT_AP_SSID;
  if (ap.length() > 0 && ap.length() < 8) ap = DEFAULT_AP_PASS;
  dn.toCharArray(deviceName, sizeof(deviceName));
  as.toCharArray(configApSsid, sizeof(configApSsid));
  ap.toCharArray(configApPass, sizeof(configApPass));
  EEPROM.write(EEPROM_DEV_MAGIC_ADDR, EEPROM_DEV_MAGIC);
  writeEepromString(EEPROM_DEV_NAME_ADDR, DEVICE_NAME_MAX_LEN + 1, dn);
  writeEepromString(EEPROM_DEV_APSSID_ADDR, AP_SSID_MAX_LEN + 1, as);
  writeEepromString(EEPROM_DEV_APPASS_ADDR, AP_PASS_MAX_LEN + 1, ap);
  EEPROM.commit();
}

void factoryResetConfig() {
  forgetWiFiManagerConfig();
  clearThingSpeakConfig();
  saveDeviceConfig(OTA_HOSTNAME, DEFAULT_AP_SSID, DEFAULT_AP_PASS);
}

void startConfigAP() {
  if (apRunning) return;
  const char* apName = configApSsid[0] ? configApSsid : DEFAULT_AP_SSID;
  const char* apPassword = configApPass[0] ? configApPass : DEFAULT_AP_PASS;
  if (strlen(apPassword) >= 8) WiFi.softAP(apName, apPassword);
  else WiFi.softAP(apName);
  apRunning = true;
  DBG_PRINT("Config AP Ready: ");
  DBG_PRINT(apName);
  DBG_PRINT(" IP: ");
  DBG_PRINTLN(WiFi.softAPIP());
}
String wifiModeText() {
  return wifiManagerMode == WIFI_MODE_STA_FALLBACK ? "1 - Smart Local fallback" : "0 - AP + STA always";
}

bool isStaOnline() {
  return WiFi.status() == WL_CONNECTED;
}

String networkStatusText() {
  if (isStaOnline()) return String("ONLINE - Internet services active");
  if (activeSsid[0]) return String("LOCAL MODE - AP/web active, reconnect every 60 sec");
  return String("LOCAL MODE - no saved WiFi, AP/web active");
}

void ensurePermanentAP() {
  WiFiMode_t m = WiFi.getMode();
  if (m != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);
  startConfigAP();
}

// ========================= OTA =========================

void setupOTA() {
#if ENABLE_OTA
  if (otaStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  DBG_PRINTLN("Starting Arduino OTA...");
  ArduinoOTA.setHostname(deviceName[0] ? deviceName : OTA_HOSTNAME);

  if (OTA_PASSWORD != nullptr && OTA_PASSWORD[0] != '\0') {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    DBG_PRINTLN("OTA Start");
  });

  ArduinoOTA.onEnd([]() {
    DBG_PRINTLN("OTA End");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
#if DEBUG_SERIAL
    static byte lastPercent = 255;
    byte percent = (total == 0) ? 0 : (progress * 100U / total);
    if (percent != lastPercent && (percent % 10 == 0 || percent == 100)) {
      lastPercent = percent;
      DBG_PRINT("OTA Progress: ");
      DBG_PRINT(percent);
      DBG_PRINTLN("%");
    }
#endif
  });

  ArduinoOTA.onError([](ota_error_t error) {
    DBG_PRINT("OTA Error: ");
    DBG_PRINTLN((int)error);
  });

  ArduinoOTA.begin();
  otaStarted = true;
  DBG_PRINT("OTA Ready. Hostname: ");
  DBG_PRINTLN(OTA_HOSTNAME);
#endif
}

void handleOTA() {
#if ENABLE_OTA
  if (WiFi.status() == WL_CONNECTED) {
    if (!otaStarted) setupOTA();
    ArduinoOTA.handle();
  }
#endif
}


// ========================= USER MANAGEMENT =========================

bool webAuthCheck(bool adminOnly);

int webUserDataAddr(byte userIndex) {
  return EEPROM_USER_DATA_ADDR + userIndex * EEPROM_USER_RECORD_SIZE;
}

const __FlashStringHelper* roleName(byte role) {
  switch (role) {
    case WEB_ROLE_ADMIN: return F("Admin");
    case WEB_ROLE_ENGINEER: return F("Engineer");
    case WEB_ROLE_OPERATOR: return F("Operator");
    default: return F("Viewer");
  }
}

bool roleAtLeast(byte needRole) {
  return currentWebRole >= needRole;
}

bool requireRole(byte needRole) {
  if (!webAuthCheck(false)) return false;
  if (!roleAtLeast(needRole)) {
    webServer.send(403, "text/plain", "403 Access Denied - insufficient permission");
    return false;
  }
  return true;
}

bool isSafeText(const String& x, byte maxLen) {
  if (x.length() < 1 || x.length() > maxLen) return false;
  for (uint16_t i = 0; i < x.length(); i++) {
    char c = x[i];
    if (c < 33 || c > 126) return false;
  }
  return true;
}

bool isSafeUsername(const String& u) {
  if (!isSafeText(u, WEB_USER_MAX_LEN)) return false;
  for (uint16_t i = 0; i < u.length(); i++) {
    char c = u[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool isSafePassword(const String& p) {
  return isSafeText(p, WEB_PASS_MAX_LEN);
}

byte sanitizeRole(byte r) {
  if (r < WEB_ROLE_VIEWER || r > WEB_ROLE_ADMIN) return WEB_ROLE_VIEWER;
  return r;
}

bool usernameExists(const String& u, byte exceptIdx) {
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    if (i == exceptIdx) continue;
    if (!WEB_USERS[i].enabled && WEB_USERS[i].user[0] == 0) continue;
    if (u.equalsIgnoreCase(WEB_USERS[i].user)) return true;
  }
  return false;
}

void setWebUserName(byte idx, const String& u) {
  if (idx >= WEB_USER_COUNT) return;
  String uu = u; uu.trim();
  if (!isSafeUsername(uu)) return;
  if (usernameExists(uu, idx)) return;
  memset(WEB_USERS[idx].user, 0, sizeof(WEB_USERS[idx].user));
  uu.toCharArray(WEB_USERS[idx].user, WEB_USER_MAX_LEN + 1);
}

void setWebUserPassword(byte idx, const String& p) {
  if (idx >= WEB_USER_COUNT) return;
  String pp = p; pp.trim();
  if (!isSafePassword(pp)) return;
  memset(WEB_USERS[idx].pass, 0, sizeof(WEB_USERS[idx].pass));
  pp.toCharArray(WEB_USERS[idx].pass, WEB_PASS_MAX_LEN + 1);
}

void saveLastLogin(byte idx) {
  if (idx >= WEB_USER_COUNT) return;
  String ip = webServer.client().remoteIP().toString();
  bool changed = strcmp(lastLoginUser, WEB_USERS[idx].user) != 0 || ip != String(lastLoginIp);
  strncpy(lastLoginUser, WEB_USERS[idx].user, WEB_USER_MAX_LEN);
  lastLoginUser[WEB_USER_MAX_LEN] = 0;
  ip.toCharArray(lastLoginIp, sizeof(lastLoginIp));
  if (!changed) return;
  writeEepromString(EEPROM_LAST_USER_ADDR, WEB_USER_MAX_LEN + 1, String(lastLoginUser));
  writeEepromString(EEPROM_LAST_USER_ADDR + WEB_USER_MAX_LEN + 1, 16, String(lastLoginIp));
  EEPROM.commit();
}

void loadLastLogin() {
  readEepromString(EEPROM_LAST_USER_ADDR, WEB_USER_MAX_LEN + 1).toCharArray(lastLoginUser, sizeof(lastLoginUser));
  readEepromString(EEPROM_LAST_USER_ADDR + WEB_USER_MAX_LEN + 1, 16).toCharArray(lastLoginIp, sizeof(lastLoginIp));
  if (lastLoginUser[0] == 0 || lastLoginUser[0] == (char)0xFF) strcpy(lastLoginUser, "none");
  if (lastLoginIp[0] == 0 || lastLoginIp[0] == (char)0xFF) strcpy(lastLoginIp, "-");
}

void writeUserRecord(byte i) {
  int addr = webUserDataAddr(i);
  EEPROM.write(addr, WEB_USERS[i].enabled ? 1 : 0);
  EEPROM.write(addr + 1, sanitizeRole(WEB_USERS[i].role));
  for (byte j = 0; j <= WEB_USER_MAX_LEN; j++) EEPROM.write(addr + 2 + j, WEB_USERS[i].user[j]);
  for (byte j = 0; j <= WEB_PASS_MAX_LEN; j++) EEPROM.write(addr + 15 + j, WEB_USERS[i].pass[j]);
}

void readUserRecord(byte i) {
  int addr = webUserDataAddr(i);
  WEB_USERS[i].enabled = EEPROM.read(addr) == 1 ? 1 : 0;
  WEB_USERS[i].role = sanitizeRole(EEPROM.read(addr + 1));
  for (byte j = 0; j <= WEB_USER_MAX_LEN; j++) WEB_USERS[i].user[j] = (char)EEPROM.read(addr + 2 + j);
  for (byte j = 0; j <= WEB_PASS_MAX_LEN; j++) WEB_USERS[i].pass[j] = (char)EEPROM.read(addr + 15 + j);
  WEB_USERS[i].user[WEB_USER_MAX_LEN] = 0;
  WEB_USERS[i].pass[WEB_PASS_MAX_LEN] = 0;
}

bool loadedUserLooksValid(byte i) {
  return isSafeUsername(String(WEB_USERS[i].user)) && isSafePassword(String(WEB_USERS[i].pass));
}

void setDefaultUsers() {
  memset(WEB_USERS, 0, sizeof(WEB_USERS));
  strcpy(WEB_USERS[0].user, "admin"); strcpy(WEB_USERS[0].pass, "admin"); WEB_USERS[0].role = WEB_ROLE_ADMIN; WEB_USERS[0].enabled = 1;
  strcpy(WEB_USERS[1].user, "operator1"); strcpy(WEB_USERS[1].pass, "1234"); WEB_USERS[1].role = WEB_ROLE_OPERATOR; WEB_USERS[1].enabled = 1;
  strcpy(WEB_USERS[2].user, "guest"); strcpy(WEB_USERS[2].pass, "0000"); WEB_USERS[2].role = WEB_ROLE_VIEWER; WEB_USERS[2].enabled = 1;
}

void saveWebUsersConfig() {
  WEB_USERS[0].enabled = 1;
  WEB_USERS[0].role = WEB_ROLE_ADMIN;
  if (!isSafeUsername(String(WEB_USERS[0].user))) strcpy(WEB_USERS[0].user, "admin");
  if (!isSafePassword(String(WEB_USERS[0].pass))) strcpy(WEB_USERS[0].pass, "admin");
  EEPROM.write(EEPROM_USER_MAGIC_ADDR, EEPROM_USER_MAGIC);
  for (byte i = 0; i < WEB_USER_COUNT; i++) writeUserRecord(i);
  EEPROM.commit();
}

void loadWebUsersConfig() {
  bool ok = (EEPROM.read(EEPROM_USER_MAGIC_ADDR) == EEPROM_USER_MAGIC);
  if (!ok) {
    setDefaultUsers();
    saveWebUsersConfig();
    strcpy(lastLoginUser, "none");
    strcpy(lastLoginIp, "-");
    return;
  }
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    readUserRecord(i);
    if (!loadedUserLooksValid(i)) {
      WEB_USERS[i].enabled = 0;
      WEB_USERS[i].role = WEB_ROLE_VIEWER;
      WEB_USERS[i].user[0] = 0;
      WEB_USERS[i].pass[0] = 0;
    }
  }
  if (!loadedUserLooksValid(0)) {
    strcpy(WEB_USERS[0].user, "admin");
    strcpy(WEB_USERS[0].pass, "admin");
  }
  WEB_USERS[0].enabled = 1;
  WEB_USERS[0].role = WEB_ROLE_ADMIN;
  loadLastLogin();
}

String roleOptions(byte selected) {
  String x;
  for (byte r = WEB_ROLE_VIEWER; r <= WEB_ROLE_ADMIN; r++) {
    x += F("<option value='"); x += String(r); x += F("'");
    if (selected == r) x += F(" selected");
    x += F(">"); x += roleName(r); x += F("</option>");
  }
  return x;
}

String userManagementCardHtml() {
  String html;
  html.reserve(7600);
  html += F("<div class='card'><h1>User Management</h1><div class='sub'>Admin only. Max 30 users. Edit username, password, role, and enable state. User 1 stays Admin/enabled for safety.</div>");
  html += F("<form method='POST' action='/saveusers'>");
  html += sessionHiddenInput();
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    html += F("<div class='smsec'><b>#"); html += String(i + 1); html += F("</b> <span class='pill'>"); html += roleName(WEB_USERS[i].role); html += F("</span>");
    if (i > 0) {
      html += F("<label class='small'><input type='checkbox' name='en"); html += String(i); html += F("' "); if (WEB_USERS[i].enabled) html += F("checked"); html += F("> Enabled</label>");
    } else {
      html += F("<div class='sub'>Main Admin is always enabled.</div>");
    }
    html += F("<label class='small'>Username</label><input maxlength='"); html += String(WEB_USER_MAX_LEN); html += F("' name='un"); html += String(i); html += F("' value='"); html += WEB_USERS[i].user; html += F("' placeholder='empty user slot'>");
    html += F("<label class='small'>New Password</label><input type='password' maxlength='"); html += String(WEB_PASS_MAX_LEN); html += F("' name='pw"); html += String(i); html += F("' placeholder='Leave empty to keep current password'>");
    html += F("<label class='small'>Role</label><select name='ro"); html += String(i); html += F("'"); if (i == 0) html += F(" disabled"); html += F(">"); html += roleOptions(WEB_USERS[i].role); html += F("</select>");
    html += F("</div>");
    yield();
  }
  html += F("<button class='btn' type='submit'>Save Users</button></form><div class='sub warn'>Username: letters/numbers/_/-. Password: 1-"); html += String(WEB_PASS_MAX_LEN); html += F(" printable characters, no spaces. Default first login: admin / admin.</div></div>");
  return html;
}

void sendUsersPage() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;

  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");

  webServer.sendContent(htmlHeader("User Management"));
  webServer.sendContent(F("<div class='card'><h1>User Management</h1><a class='btn btn2' href='/'>Back</a></div>"));

  webServer.sendContent(F("<div class='card'><h1>User Management</h1><div class='sub'>Admin only. Max 30 users. Edit username, password, role, and enable state. User 1 stays Admin/enabled for safety.</div>"));
  webServer.sendContent(F("<form method='POST' action='/saveusers'>"));
  webServer.sendContent(sessionHiddenInput());

  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    String row;
    row.reserve(520);

    row += F("<div class='smsec'><b>#"); row += String(i + 1); row += F("</b> <span class='pill'>"); row += roleName(WEB_USERS[i].role); row += F("</span>");

    if (i > 0) {
      row += F("<label class='small'><input type='checkbox' name='en"); row += String(i); row += F("' ");
      if (WEB_USERS[i].enabled) row += F("checked");
      row += F("> Enabled</label>");
    } else {
      row += F("<div class='sub'>Main Admin is always enabled.</div>");
    }

    row += F("<label class='small'>Username</label><input maxlength='"); row += String(WEB_USER_MAX_LEN); row += F("' name='un"); row += String(i); row += F("' value='");
    row += WEB_USERS[i].user;
    row += F("' placeholder='empty user slot'>");

    row += F("<label class='small'>New Password</label><input type='password' maxlength='"); row += String(WEB_PASS_MAX_LEN); row += F("' name='pw"); row += String(i); row += F("' placeholder='Leave empty to keep current password'>");

    row += F("<label class='small'>Role</label><select name='ro"); row += String(i); row += F("'");
    if (i == 0) row += F(" disabled");
    row += F(">");
    row += roleOptions(WEB_USERS[i].role);
    row += F("</select></div>");

    webServer.sendContent(row);
    yield();
  }

  webServer.sendContent(F("<button class='btn' type='submit'>Save Users</button></form><div class='sub warn'>Username: letters/numbers/_/-. Password: 1-"));
  webServer.sendContent(String(WEB_PASS_MAX_LEN));
  webServer.sendContent(F(" printable characters, no spaces. Default first login: admin / admin.</div></div>"));
  webServer.sendContent(htmlFooter());
  webServer.sendContent("");
}

void handleSaveUsers() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    String name = webServer.arg(String("un") + i); name.trim();
    String pass = webServer.arg(String("pw") + i); pass.trim();
    byte role = sanitizeRole((byte)webServer.arg(String("ro") + i).toInt());

    if (i == 0) {
      WEB_USERS[i].enabled = 1;
      WEB_USERS[i].role = WEB_ROLE_ADMIN;
      if (isSafeUsername(name) && !usernameExists(name, i)) setWebUserName(i, name);
      if (pass.length() > 0) setWebUserPassword(i, pass);
      continue;
    }

    if (name.length() == 0) {
      WEB_USERS[i].enabled = 0;
      WEB_USERS[i].user[0] = 0;
      WEB_USERS[i].pass[0] = 0;
      WEB_USERS[i].role = WEB_ROLE_VIEWER;
      continue;
    }

    if (isSafeUsername(name) && !usernameExists(name, i)) setWebUserName(i, name);
    if (pass.length() > 0) setWebUserPassword(i, pass);
    if (WEB_USERS[i].pass[0] == 0) strcpy(WEB_USERS[i].pass, "1234");
    WEB_USERS[i].role = role;
    WEB_USERS[i].enabled = webServer.hasArg(String("en") + i) ? 1 : 0;
  }
  saveWebUsersConfig();
  String html = htmlHeader("Users Saved");
  html += F("<div class='card'><h1>Users Saved</h1><div class='sub'>Users/roles/passwords saved. If your current password changed, login again.</div><a class='btn btn2' href='"); html += sessionUrl("/users"); html += F("'>Back to Users</a><a class='btn btn2' href='/'>Home</a></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}


String getCookieValue(const String& name) {
  String cookie = webServer.header("Cookie");
  String key = name + "=";
  int p = cookie.indexOf(key);
  if (p < 0) return "";
  p += key.length();
  int e = cookie.indexOf(';', p);
  if (e < 0) e = cookie.length();
  String v = cookie.substring(p, e);
  v.trim();
  return v;
}

String makeSessionToken() {
  String t = String(((uint32_t)ESP.getEfuseMac()), HEX);
  t += String(micros(), HEX);
  t += String(millis(), HEX);
  t += String(random(0xFFFF), HEX);
  t += String(random(0xFFFF), HEX);
  return t;
}

String sessionHiddenInput() {
  if (webSessionToken.length() == 0) return "";
  return String("<input type='hidden' name='sid' value='") + webSessionToken + String("'>");
}

String sessionUrl(const String& path) {
  if (webSessionToken.length() == 0) return path;
  if (path.indexOf('?') >= 0) return path + "&sid=" + webSessionToken;
  return path + "?sid=" + webSessionToken;
}

bool findWebUser(const String& user, const String& pass, byte& foundIndex) {
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    if (!WEB_USERS[i].enabled) continue;
    if (WEB_USERS[i].user[0] == 0 || WEB_USERS[i].pass[0] == 0) continue;
    if (user.equals(String(WEB_USERS[i].user)) && pass.equals(String(WEB_USERS[i].pass))) {
      foundIndex = i;
      return true;
    }
  }
  return false;
}

void clearSessionSlot(byte slot) {
  if (slot >= MAX_WEB_SESSIONS) return;
  webSessions[slot].token = "";
  webSessions[slot].userIndex = 255;
  webSessions[slot].createdMs = 0;
  webSessions[slot].lastSeenMs = 0;
  webSessions[slot].ip = IPAddress(0, 0, 0, 0);
  webSessions[slot].active = false;
}

byte findOldestSessionSlot() {
  byte best = 0;
  unsigned long oldest = 0xFFFFFFFFUL;
  for (byte i = 0; i < MAX_WEB_SESSIONS; i++) {
    if (!webSessions[i].active) return i;
    if (webSessions[i].lastSeenMs < oldest) {
      oldest = webSessions[i].lastSeenMs;
      best = i;
    }
  }
  return best;
}

void setLoggedInUser(byte i) {
  byte slot = findOldestSessionSlot();
  String token = makeSessionToken();

  webSessions[slot].token = token;
  webSessions[slot].userIndex = i;
  webSessions[slot].createdMs = millis();
  webSessions[slot].lastSeenMs = millis();
  webSessions[slot].ip = webServer.client().remoteIP();
  webSessions[slot].active = true;

  currentSessionSlot = slot;
  currentWebRole = sanitizeRole(WEB_USERS[i].role);
  currentWebUserIndex = i;
  webSessionToken = token;
  webSessionLastSeen = webSessions[slot].lastSeenMs;

  saveLastLogin(i);
}

void clearWebSession() {
  String s = getCookieValue("ESPSESS");
  if (s.length() == 0 && webServer.hasArg("sid")) s = webServer.arg("sid");

  for (byte i = 0; i < MAX_WEB_SESSIONS; i++) {
    if (webSessions[i].active && webSessions[i].token.equals(s)) {
      clearSessionSlot(i);
      break;
    }
  }

  webSessionToken = "";
  webSessionLastSeen = 0;
  currentSessionSlot = 255;
  currentWebRole = WEB_ROLE_VIEWER;
  currentWebUserIndex = 255;
}

bool hasValidSession() {
  String s = getCookieValue("ESPSESS");
  if (s.length() == 0 && webServer.hasArg("sid")) s = webServer.arg("sid");
  if (s.length() == 0) return false;

  unsigned long now = millis();
  for (byte i = 0; i < MAX_WEB_SESSIONS; i++) {
    if (!webSessions[i].active) continue;
    if (!webSessions[i].token.equals(s)) continue;

    if (now - webSessions[i].lastSeenMs > WEB_SESSION_TIMEOUT_MS) {
      clearSessionSlot(i);
      return false;
    }

    byte u = webSessions[i].userIndex;
    if (u >= WEB_USER_COUNT || !WEB_USERS[u].enabled) {
      clearSessionSlot(i);
      return false;
    }

    webSessions[i].lastSeenMs = now;
    currentSessionSlot = i;
    currentWebUserIndex = u;
    currentWebRole = sanitizeRole(WEB_USERS[u].role);
    webSessionToken = webSessions[i].token;
    webSessionLastSeen = now;
    return true;
  }

  return false;
}

void redirectToLogin() {
  String ret = webServer.uri();
  webServer.sendHeader("Location", "/login?r=" + ret, true);
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.send(302, "text/plain", "");
}



void loadWebPortConfig() {
  byte magic = EEPROM.read(EEPROM_WEBPORT_MAGIC_ADDR);
  if (magic == EEPROM_WEBPORT_MAGIC) {
    uint16_t p = ((uint16_t)EEPROM.read(EEPROM_WEBPORT_VALUE_ADDR) << 8) | EEPROM.read(EEPROM_WEBPORT_VALUE_ADDR + 1);
    webServerPort = (p >= 1 && p <= 65535) ? p : 80;
  } else {
    webServerPort = 80;
  }
}

void saveWebPortConfig(uint16_t p) {
  if (p < 1 || p > 65535) p = 80;
  webServerPort = p;
  EEPROM.write(EEPROM_WEBPORT_MAGIC_ADDR, EEPROM_WEBPORT_MAGIC);
  EEPROM.write(EEPROM_WEBPORT_VALUE_ADDR, (byte)(p >> 8));
  EEPROM.write(EEPROM_WEBPORT_VALUE_ADDR + 1, (byte)(p & 0xFF));
  EEPROM.commit();
}

// ========================= WEB OTA PROFESSIONAL =========================

bool webAuthCheck(bool adminOnly) {
#if ENABLE_WEB_OTA
  if (hasValidSession() && currentWebUserIndex < WEB_USER_COUNT && WEB_USERS[currentWebUserIndex].enabled) {
    currentWebRole = sanitizeRole(WEB_USERS[currentWebUserIndex].role);
    if (adminOnly && currentWebRole != WEB_ROLE_ADMIN) {
      webServer.send(403, "text/plain", "403 Forbidden - Admin only");
      return false;
    }
    return true;
  }

  redirectToLogin();
  return false;
#else
  currentWebRole = WEB_ROLE_ADMIN;
  currentWebUserIndex = 0;
  return true;
#endif
}

bool webAnyAuthOk() { return webAuthCheck(false); }
bool webOtaAuthOk() { return webAuthCheck(true); }
bool webIsAdmin() { return currentWebRole == WEB_ROLE_ADMIN; }
String currentWebUserName() { return (currentWebUserIndex < WEB_USER_COUNT) ? String(WEB_USERS[currentWebUserIndex].user) : String("-"); }


void sendLoginPage() {
  String html = htmlHeader("Login");
  html += F("<div class='card'><h1>Login</h1><div class='sub'>Multi Session login. Max ");
  html += String(MAX_WEB_SESSIONS);
  html += F(" devices. New login kicks oldest device if full.</div>");
  if (webServer.hasArg("bad")) html += F("<div class='msg warn'>Wrong username or password</div>");
  if (webServer.hasArg("out")) html += F("<div class='msg ok'>Logged out successfully</div>");
  html += F("<form method='POST' action='/dologin'>");
  if (webServer.hasArg("r")) { html += F("<input type='hidden' name='r' value='"); html += webServer.arg("r"); html += F("'>"); }
  html += F("<label class='small'>Username</label><input name='u' maxlength='");
  html += String(WEB_USER_MAX_LEN);
  html += F("' autocomplete='username'>");
  html += F("<label class='small'>Password</label><input type='password' name='p' maxlength='");
  html += String(WEB_PASS_MAX_LEN);
  html += F("' autocomplete='current-password'>");
  html += F("<button class='btn' type='submit'>Login</button>");
  html += F("</form></div>");
  html += htmlFooter();

  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.send(200, "text/html", html);
}

void handleDoLogin() {
  String u = webServer.arg("u"); u.trim();
  String p = webServer.arg("p"); p.trim();

  byte idx = 255;
  if (findWebUser(u, p, idx)) {
    setLoggedInUser(idx);
    String cookie = "ESPSESS=" + webSessionToken + "; Path=/";
    webServer.sendHeader("Set-Cookie", cookie);
    String ret = webServer.arg("r");
    if (ret.length() == 0 || ret.startsWith("/login") || ret.startsWith("/logout")) ret = "/";
    webServer.sendHeader("Location", ret, true);
    webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    webServer.send(302, "text/plain", "");
    return;
  }

  webServer.sendHeader("Location", "/login?bad=1", true);
  webServer.send(302, "text/plain", "");
}

void sendLogoutPage() {
  clearWebSession();
  webServer.sendHeader("Set-Cookie", "ESPSESS=deleted; Path=/; Max-Age=0");
  webServer.sendHeader("Location", "/login?out=1", true);
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.send(302, "text/plain", "");
}


String htmlHeader(const String& title) {
  String h;
  h.reserve(1050);
  h += F("<!doctype html><html><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>"); h += title; h += F("</title>");
  h += F("<style>");
  h += F("*{box-sizing:border-box}body{margin:0;font-family:Arial,Tahoma,sans-serif;background:#0f172a;color:#e5e7eb}.wrap{max-width:520px;margin:auto;padding:16px}.card{background:#111c33;border:1px solid #334155;border-radius:18px;padding:16px;margin-top:14px}h1{font-size:24px;margin:4px 0}.sub,.k,.small,.msg{color:#94a3b8}.row{display:flex;justify-content:space-between;border-bottom:1px solid #26364f;padding:9px 0;font-size:14px}.v{font-weight:700;text-align:right}input,select{width:100%;padding:12px;border-radius:12px;border:1px solid #475569;background:#0b1220;color:#e5e7eb;margin:8px 0}.btn{width:100%;border:0;border-radius:14px;padding:13px;font-size:16px;font-weight:700;background:#22c55e;color:#001018;margin-top:8px}.btn2{display:block;text-align:center;text-decoration:none;background:#1e293b;color:#e5e7eb;border:1px solid #475569}.bar{height:12px;background:#0b1220;border-radius:99px;overflow:hidden;border:1px solid #334155;margin-top:12px}.fill{height:100%;width:0;background:#22c55e}.warn{color:#fbbf24}.ok,.on{color:#22c55e}.bad,.off{color:#ef4444}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}.pill{display:inline-block;padding:4px 10px;border-radius:99px;background:#0b1220;border:1px solid #334155;font-weight:700}.oc{display:grid;gap:12px;margin-top:12px}.ocbox{padding:14px;border-radius:18px;background:#0b1220;border:1px solid #334155}.octop{display:flex;align-items:center;justify-content:space-between;gap:10px}.onbtn{background:#22c55e!important;color:#03140a!important}.offbtn{background:#ef4444!important;color:#fff!important}.dot{width:18px;height:18px;border-radius:50%;display:inline-block;margin-left:8px;box-shadow:0 0 10px currentColor}.dot.on{background:#22c55e}.dot.off{background:#ef4444}.bigstate{font-size:22px;font-weight:900}.small{font-size:12px}input[type=checkbox],input[type=radio]{width:auto;margin:6px}.modepick{display:grid;gap:8px;margin:10px 0}.modepick label{padding:10px;border:1px solid #334155;border-radius:12px;background:#0b1220}.smsec{border:1px solid #26364f;border-radius:14px;padding:10px;margin:10px 0}.smsec.disabled{opacity:.38}.dayrow{display:grid;grid-template-columns:74px 74px 1fr 1fr;gap:7px;align-items:center}.dayrow .daylbl{grid-row:span 2}.dayhdr{display:grid;grid-template-columns:74px 74px 1fr 1fr;gap:7px;align-items:center}.timergrid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}");
  h += F("</style></head><body><div class='wrap'>");
  return h;
}

String htmlFooter() {
  String f;
  f.reserve(140);
  f += F("<div class='sub' style='text-align:center;margin-top:14px'>");
  f += FW_FULL_NAME;
  f += F(" / build ");
  f += String(FW_BUILD);
  f += F("</div></div></body></html>");
  return f;
}

String thingSpeakLoginUserText() {
  String u = String(lastLoginUser);
  u.trim();
  if (u.length() == 0 || u == "-" || u == "none") {
    String cu = currentWebUserName();
    cu.trim();
    if (cu.length() > 0) u = cu;
  }
  if (u.length() == 0) u = "none";
  return u;
}

String tideButtonCardHtml() {
  String html;
  html.reserve(420);
  html += F("<div class='card'><h1>Ras Sedr Tide</h1>");
  html += F("<div class='sub'>مد وجزر رأس سدر - جنوب سيناء. يفتح الرابط الخارجي في صفحة جديدة.</div>");
  html += F("<a class='btn btn2' target='_blank' rel='noopener' href='");
  html += RAS_SEDR_TIDE_URL;
  html += F("'>Open Tide / المد والجزر</a></div>");
  return html;
}


String dhtTypeText() {
  if (dhtSensorType == SENSOR_TYPE_DHT22) return String("DHT22");
  if (dhtSensorType == SENSOR_TYPE_DS18B20) return String("DS18B20");
  return String("DHT11");
}

bool isDhtSensorSelected() {
  return (dhtSensorType == SENSOR_TYPE_DHT11 || dhtSensorType == SENSOR_TYPE_DHT22);
}

DHT& activeDhtSensor() {
  if (dhtSensorType == SENSOR_TYPE_DHT22) return dht22Sensor;
  return dht11Sensor;
}

void loadDhtConfig() {
  byte t = EEPROM.read(EEPROM_DHT_TYPE_ADDR);
  if (EEPROM.read(EEPROM_DHT_MAGIC_ADDR) == EEPROM_DHT_MAGIC &&
      (t == SENSOR_TYPE_DHT11 || t == SENSOR_TYPE_DHT22 || t == SENSOR_TYPE_DS18B20)) {
    dhtSensorType = t;
  } else {
#if DHT_TYPE_SELECT
    dhtSensorType = SENSOR_TYPE_DHT22;
#else
    dhtSensorType = SENSOR_TYPE_DHT11;
#endif
  }
}

void beginSelectedSensor() {
  dht11Sensor.begin();
  dht22Sensor.begin();
  ds18b20Sensor.begin();
  ds18b20Present = (ds18b20Sensor.getDeviceCount() > 0);
  if (dhtSensorType == SENSOR_TYPE_DS18B20) {
    ds18b20Sensor.setResolution(12);
  }
}

void saveDhtConfig(byte t) {
  if (t != SENSOR_TYPE_DHT11 && t != SENSOR_TYPE_DHT22 && t != SENSOR_TYPE_DS18B20) t = SENSOR_TYPE_DHT11;
  dhtSensorType = t;
  EEPROM.write(EEPROM_DHT_MAGIC_ADDR, EEPROM_DHT_MAGIC);
  EEPROM.write(EEPROM_DHT_TYPE_ADDR, dhtSensorType);
  EEPROM.commit();
  beginSelectedSensor();
  currentTemperature = NAN;
  currentHumidity = NAN;
  sensorReadOk = false;
}

float smoothValue(float oldValue, float newValue) {
  if (isnan(newValue)) return oldValue;
  if (isnan(oldValue)) return newValue;
  return (oldValue * 0.70f) + (newValue * 0.30f);
}

bool validDs18b20Temp(float t) {
  if (isnan(t)) return false;
  if (t <= -126.0f) return false;
  if (t == 85.0f) return false;
  if (t < -55.0f || t > 125.0f) return false;
  return true;
}

bool readDhtSensorNow(bool force) {
  if (!force && millis() - lastDhtReadTime < DHT_MIN_READ_INTERVAL) {
    return sensorReadOk;
  }

#if ENABLE_DHT_PIN_BUTTON
  if (digitalRead(DHTPIN) == LOW) {
    DBG_PRINTLN("Sensor read skipped: manual button is pressed on sensor data pin");
    return false;
  }
#endif

  lastDhtReadTime = millis();
  DBG_PRINT("Reading sensor: ");
  DBG_PRINTLN(dhtTypeText());

  if (dhtSensorType == SENSOR_TYPE_DS18B20) {
    ds18b20Sensor.requestTemperatures();
    float temp = ds18b20Sensor.getTempCByIndex(0);
    DBG_PRINT("DS18B20 raw Temp: ");
    DBG_PRINTLN(temp);

    if (validDs18b20Temp(temp)) {
      currentTemperature = smoothValue(currentTemperature, temp);
      currentHumidity = NAN;
      sensorReadOk = true;
      lastValidSensorReadMs = millis();
      ds18b20Present = true;
      return true;
    }

    DBG_PRINTLN("DS18B20 read invalid -> keep display value but mark control not fresh");
    ds18b20Present = (ds18b20Sensor.getDeviceCount() > 0);
    sensorReadOk = false;
    return false;
  }

  float temp = NAN;
  float hum = NAN;

  for (byte attempt = 1; attempt <= DHT_READ_RETRIES; attempt++) {
    temp = activeDhtSensor().readTemperature();
    hum = activeDhtSensor().readHumidity();

    DBG_PRINT("DHT attempt ");
    DBG_PRINT(attempt);
    DBG_PRINT("/");
    DBG_PRINT(DHT_READ_RETRIES);
    DBG_PRINT(" -> Temp: ");
    DBG_PRINT(temp);
    DBG_PRINT(" Hum: ");
    DBG_PRINTLN(hum);

    if (!isnan(temp) && !isnan(hum)) {
      currentTemperature = smoothValue(currentTemperature, temp);
      currentHumidity = smoothValue(currentHumidity, hum);
      sensorReadOk = true;
      lastValidSensorReadMs = millis();
      return true;
    }

    if (attempt < DHT_READ_RETRIES) {
      delay(DHT_RETRY_DELAY_MS);
      yield();
    }
  }

  sensorReadOk = false;
  return false;
}

bool sensorFreshForSmartControl(byte mode) {
  if (!sensorReadOk) return false;
  if (lastValidSensorReadMs == 0) return false;
  if (millis() - lastValidSensorReadMs > SENSOR_CONTROL_STALE_MS) return false;
  if (mode == SMART_MODE_TEMPERATURE) return !isnan(currentTemperature);
  if (mode == SMART_MODE_HUMIDITY) return !isnan(currentHumidity);
  return true;
}

String sensorValueJson(float v) {
  if (isnan(v)) return String("null");
  return String(v, 1);
}

String outputStateText(byte state) {
  return state ? String("ON") : String("OFF");
}

String outputClass(byte state) {
  return state ? String("on") : String("off");
}


String smartModeText(byte mode) {
  if (mode == SMART_MODE_MANUAL_SCHED) return String("Manual / Schedule");
  if (mode == SMART_MODE_TEMPERATURE) return String("Temperature");
  if (mode == SMART_MODE_HUMIDITY) return String("Humidity");
  if (mode == SMART_MODE_TIMER) return String("Timer");
  return String("Internet LED");
}

String minuteToTime(uint16_t m) {
  if (m > 1439) m = 0;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
  return String(buf);
}

uint16_t parseTimeToMinutes(const String& t, uint16_t fallback) {
  int c = t.indexOf(':');
  if (c < 0) return fallback;
  int h = t.substring(0, c).toInt();
  int m = t.substring(c + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return fallback;
  return (uint16_t)(h * 60 + m);
}

uint16_t clampDayMinute(uint16_t v, uint16_t fallback) {
  return (v <= 1439) ? v : fallback;
}

uint32_t clampTimerSeconds(uint32_t sec) {
  if (sec < 1UL) return 1UL;
  if (sec > 86399UL) return 86399UL;
  return sec;
}

String formatHMS(uint32_t sec) {
  if (sec > 86399UL) sec = 86399UL;
  char buf[9];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", sec / 3600UL, (sec % 3600UL) / 60UL, sec % 60UL);
  return String(buf);
}

uint32_t smartTimerRemainingSec() {
  if (smartCfg.mode != SMART_MODE_TIMER || !smartTimerActive) return 0;
  uint32_t elapsed = (uint32_t)((millis() - smartTimerStartMs) / 1000UL);
  if (elapsed >= smartTimerDurationSec) return 0;
  return smartTimerDurationSec - elapsed;
}

void startSmartTimer(uint32_t durationSec) {
  smartTimerDurationSec = clampTimerSeconds(durationSec);
  smartTimerStartMs = millis();
  smartTimerActive = true;
}

void stopSmartTimer() {
  smartTimerActive = false;
  smartTimerDurationSec = 0;
}

bool systemNtpTimeValid() {
  time_t now = time(nullptr);
  return now > 1700000000UL;
}

uint32_t getRawUtcEpoch() {
  // Raw UTC source: NTP when online, otherwise last saved UTC + millis().
  if (systemNtpTimeValid()) return (uint32_t)time(nullptr);
  if (offlineClockBaseEpoch > 1700000000UL) {
    return offlineClockBaseEpoch + (uint32_t)((millis() - offlineClockBaseMs) / 1000UL);
  }
  return 0;
}

bool isLeapYear(int y) {
  return ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0));
}

byte daysInMonth(int y, byte m) {
  static const byte mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (m == 2 && isLeapYear(y)) return 29;
  return mdays[m - 1];
}

byte weekdayOfDate(int y, byte m, byte d) {
  // Returns 0=Sunday ... 6=Saturday. Sakamoto algorithm.
  static const byte t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (byte)((y + y/4 - y/100 + y/400 + t[m-1] + d) % 7);
}

byte lastWeekdayOfMonth(int y, byte m, byte weekday) {
  byte d = daysInMonth(y, m);
  while (weekdayOfDate(y, m, d) != weekday) d--;
  return d;
}

bool isEgyptDstAutoActive(uint32_t utcEpoch) {
  if (utcEpoch <= 1700000000UL) return false;
  time_t stdLocal = (time_t)(utcEpoch + 2UL * 3600UL); // Egypt winter base for date decision
  struct tm tmStd;
  gmtime_r(&stdLocal, &tmStd);
  int y = tmStd.tm_year + 1900;
  byte m = tmStd.tm_mon + 1;
  byte d = tmStd.tm_mday;
  byte h = tmStd.tm_hour;

  if (m < 4 || m > 10) return false;
  if (m > 4 && m < 10) return true;

  byte lastFriApr = lastWeekdayOfMonth(y, 4, 5); // Friday, 0=Sun...5=Fri
  byte lastThuOct = lastWeekdayOfMonth(y, 10, 4); // Thursday

  if (m == 4) {
    // Egypt summer time starts from the last Friday of April at 00:00 local time.
    return (d > lastFriApr) || (d == lastFriApr && h >= 0);
  }

  // It remains active through the last Thursday of October, then ends at Friday 00:00.
  return (d <= lastThuOct);
}

long egyptUtcOffsetSecFor(uint32_t utcEpoch) {
  if (egyptTimeMode == EGYPT_TZ_SUMMER) return 3L * 3600L;
  if (egyptTimeMode == EGYPT_TZ_WINTER) return 2L * 3600L;
  return isEgyptDstAutoActive(utcEpoch) ? (3L * 3600L) : (2L * 3600L);
}

String egyptTimeModeText() {
  if (egyptTimeMode == EGYPT_TZ_WINTER) return String("Manual Winter UTC+2");
  if (egyptTimeMode == EGYPT_TZ_SUMMER) return String("Manual Summer UTC+3");
  return String("Auto Egypt DST");
}

uint32_t getDeviceEpoch() {
  // Returns local Egypt-adjusted epoch. Use gmtime_r() with it for display/schedule.
  uint32_t rawUtc = getRawUtcEpoch();
  if (rawUtc <= 1700000000UL) return 0;
  return rawUtc + (uint32_t)egyptUtcOffsetSecFor(rawUtc);
}

bool isTimeValidNow() {
  return getDeviceEpoch() > 1700000000UL;
}

String clockStateText() {
  String s;
  if (clockState == 1) s = String("Online Sync");
  else if (clockState == 2) s = String("Offline Estimated");
  else s = String("No Time");
  s += String(" / ") + egyptTimeModeText();
  return s;
}

void saveEgyptTimeMode() {
  if (egyptTimeMode > EGYPT_TZ_SUMMER) egyptTimeMode = EGYPT_TZ_AUTO;
  EEPROM.write(EEPROM_TZ_MAGIC_ADDR, EEPROM_TZ_MAGIC);
  EEPROM.write(EEPROM_TZ_MODE_ADDR, egyptTimeMode);
  EEPROM.commit();
}

void loadEgyptTimeMode() {
  egyptTimeMode = EGYPT_TZ_AUTO;
  if (EEPROM.read(EEPROM_TZ_MAGIC_ADDR) == EEPROM_TZ_MAGIC) {
    byte m = EEPROM.read(EEPROM_TZ_MODE_ADDR);
    if (m <= EGYPT_TZ_SUMMER) egyptTimeMode = m;
  }
}

void saveOfflineClockNow(uint32_t utcNow) {
  if (utcNow <= 1700000000UL) return;
  EEPROM.write(EEPROM_CLOCK_MAGIC_ADDR, EEPROM_CLOCK_MAGIC);
  EEPROM.put(EEPROM_CLOCK_EPOCH_ADDR, utcNow);
  EEPROM.commit();
  lastOfflineClockSaveMs = millis();
}

void loadOfflineClock() {
  offlineClockLoaded = false;
  offlineClockBaseEpoch = 0;
  offlineClockBaseMs = millis();
  if (EEPROM.read(EEPROM_CLOCK_MAGIC_ADDR) == EEPROM_CLOCK_MAGIC) {
    EEPROM.get(EEPROM_CLOCK_EPOCH_ADDR, offlineClockBaseEpoch);
    if (offlineClockBaseEpoch > 1700000000UL) {
      offlineClockLoaded = true;
      clockState = 2;
    }
  }
}

void maintainOfflineClock() {
  if (systemNtpTimeValid()) {
    uint32_t utcNow = (uint32_t)time(nullptr);
    offlineClockBaseEpoch = utcNow;
    offlineClockBaseMs = millis();
    clockState = 1;
    if (lastOfflineClockSaveMs == 0 || millis() - lastOfflineClockSaveMs >= OFFLINE_CLOCK_SAVE_INTERVAL_MS) {
      saveOfflineClockNow(utcNow);
    }
  } else if (offlineClockBaseEpoch > 1700000000UL) {
    clockState = 2;
  } else {
    clockState = 0;
  }
}

void startNtpIfNeeded() {
  if (ntpStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;
  configTime(LOCAL_GMT_OFFSET_SEC, LOCAL_DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  ntpStarted = true;
}

bool timeInWindow(uint16_t nowMin, uint16_t startMin, uint16_t endMin) {
  // OFF is always exclusive: ON at start minute, OFF exactly when nowMin reaches endMin.
  // This prevents Output 3 from staying ON one extra window/slot.
  if (startMin == endMin) return false;
  if (startMin < endMin) return (nowMin >= startMin && nowMin < endMin);
  return (nowMin >= startMin || nowMin < endMin); // overnight window: e.g. 22:00 -> 02:00
}

bool smartScheduleActiveNow() {
  if (!smartCfg.scheduleEnable) return false;
  if (!isTimeValidNow()) return false;

  time_t now = (time_t)getDeviceEpoch();
  struct tm *tmNow = gmtime(&now);
  if (!tmNow) return false;

  byte today = (byte)tmNow->tm_wday; // tm_wday: 0=Sun ... 6=Sat
  byte prevDay = (today == 0) ? 6 : (today - 1);
  uint16_t nowMin = (uint16_t)(tmNow->tm_hour * 60 + tmNow->tm_min);

  // Same-day slots, for example Mon Slot1 08:00 -> 12:00 and Slot2 18:00 -> 20:00.
  if (smartCfg.dayEnabled[today]) {
    for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
      if (!smartCfg.slotEnabled[today][sl]) continue;
      uint16_t st = smartCfg.dayStartMin[today][sl];
      uint16_t en = smartCfg.dayEndMin[today][sl];
      if (st == en) continue; // disabled invalid slot safety
      if (st < en && timeInWindow(nowMin, st, en)) return true;
      if (st > en && nowMin >= st) return true; // today starts an overnight window
    }
  }

  // Previous day overnight continuation, for example Mon 22:00 -> Tue 02:00.
  if (smartCfg.dayEnabled[prevDay]) {
    for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
      if (!smartCfg.slotEnabled[prevDay][sl]) continue;
      uint16_t st = smartCfg.dayStartMin[prevDay][sl];
      uint16_t en = smartCfg.dayEndMin[prevDay][sl];
      if (st > en && nowMin < en) return true;
    }
  }

  return false;
}

void writeSmartOutput(bool on) {
#if ENABLE_STATUS_LED
  smartOutputState = on;
  // ESP-01 blue LED / most active-low relay modules: LOW = ON, HIGH = OFF.
  digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
#endif
}

void saveSmartOutputConfig() {
  EEPROM.write(EEPROM_SMART_MAGIC_ADDR, EEPROM_SMART_MAGIC);
  EEPROM.put(EEPROM_SMART_DATA_ADDR, smartCfg);
  EEPROM.commit();
}

void setSmartDefaults() {
  smartCfg.mode = SMART_MODE_INTERNET_LED;
  smartCfg.manualState = 0;
  smartCfg.scheduleEnable = 0;
  smartCfg.repeatWeekly = 1;
  smartCfg.onceDone = 0;
  for (byte i = 0; i < 7; i++) {
    smartCfg.dayEnabled[i] = 1;
    smartCfg.slotEnabled[i][0] = 1;
    smartCfg.dayStartMin[i][0] = 18 * 60;
    smartCfg.dayEndMin[i][0] = 23 * 60;
    smartCfg.slotEnabled[i][1] = 0;
    smartCfg.dayStartMin[i][1] = 6 * 60;
    smartCfg.dayEndMin[i][1] = 8 * 60;
  }
  smartCfg.tempOnX10 = 300;
  smartCfg.tempOffX10 = 270;
  smartCfg.humOnX10 = 700;
  smartCfg.humOffX10 = 600;
}

bool smartConfigLooksValid() {
  if (smartCfg.mode > SMART_MODE_MAX) return false;
  for (byte i = 0; i < 7; i++) {
    if (smartCfg.dayEnabled[i] > 1) return false;
    for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
      if (smartCfg.slotEnabled[i][sl] > 1) return false;
      if (smartCfg.dayStartMin[i][sl] > 1439 || smartCfg.dayEndMin[i][sl] > 1439) return false;
    }
  }
  return true;
}

void loadSmartOutputConfig() {
  bool ok = (EEPROM.read(EEPROM_SMART_MAGIC_ADDR) == EEPROM_SMART_MAGIC);
  if (ok) EEPROM.get(EEPROM_SMART_DATA_ADDR, smartCfg);
  if (!ok || !smartConfigLooksValid()) {
    setSmartDefaults();
    saveSmartOutputConfig();
  }
  // Safety rule: timer is runtime-only and is never restored from EEPROM.
  // After power-up/reset, Timer mode output stays OFF and Remaining shows 00:00:00
  // until the user saves/starts Timer from the web UI.
  stopSmartTimer();
}

bool hysteresisDecision(float value, int16_t onX10, int16_t offX10, bool currentState) {
  if (isnan(value)) return currentState;
  float onV = onX10 / 10.0;
  float offV = offX10 / 10.0;
  if (onX10 >= offX10) {
    if (!currentState && value >= onV) return true;
    if (currentState && value <= offV) return false;
  } else {
    if (!currentState && value <= onV) return true;
    if (currentState && value >= offV) return false;
  }
  return currentState;
}


void refreshSmartSensorForOut3Control() {
  // V1.2.26: Temperature/Humidity modes must not wait for the 60s ThingSpeak upload.
  // Read the local sensor at a safe lightweight interval, then apply hysteresis immediately.
  if (smartCfg.mode != SMART_MODE_TEMPERATURE && smartCfg.mode != SMART_MODE_HUMIDITY) return;
  unsigned long now = millis();
  if (now - lastSmartSensorControlReadMs < SMART_SENSOR_CONTROL_INTERVAL) return;
  lastSmartSensorControlReadMs = now;
  readDhtSensorNow(false);
}

void updateWebStatusSensorCache() {
  // V1.2.35: Keep sensor values fresh in the background.
  // /status must be a fast cache-only response and must never trigger a DHT/DS18B20 read.
  unsigned long now = millis();
  if (now - lastWebStatusSensorCacheMs < WEB_STATUS_SENSOR_CACHE_INTERVAL) return;
  lastWebStatusSensorCacheMs = now;
  readDhtSensorNow(false);
}

void applySmartOutputControl() {
#if ENABLE_STATUS_LED
  refreshSmartSensorForOut3Control();
  // V1.2.26: Output 3 schedule must have a real GPIO owner.
  // If Schedule Enable is ON, it drives GPIO1/TX directly and disables the internet LED behavior.
  bool scheduleOwnsOut3 = (smartCfg.mode == SMART_MODE_MANUAL_SCHED && smartCfg.scheduleEnable);
  if (smartCfg.mode == SMART_MODE_INTERNET_LED && !scheduleOwnsOut3) return; // updateStatusLed() owns GPIO1 only in LED mode.

  bool desired = smartOutputState;

  if (scheduleOwnsOut3) {
    // Schedule owns Output 3 completely: inside any enabled slot = ON, outside all slots = forced OFF.
    // OFF minute is exclusive, so it turns OFF exactly at the programmed end time.
    bool active = smartScheduleActiveNow();
    desired = active;
    if (!smartCfg.repeatWeekly && smartOutputState && !active) {
      smartCfg.onceDone = 1;
      saveSmartOutputConfig();
    }
    if (smartCfg.onceDone && !smartCfg.repeatWeekly) desired = false;
  } else if (smartCfg.mode == SMART_MODE_MANUAL_SCHED) {
    desired = (smartCfg.manualState == 1);
  } else if (smartCfg.mode == SMART_MODE_TEMPERATURE) {
    // V1.2.27: control Out3 directly from the latest local temperature reading.
    // Fail safe: if the sensor value is invalid, force Out3 OFF instead of keeping a stale ON.
    desired = sensorFreshForSmartControl(SMART_MODE_TEMPERATURE) ? hysteresisDecision(currentTemperature, smartCfg.tempOnX10, smartCfg.tempOffX10, smartOutputState) : false;
  } else if (smartCfg.mode == SMART_MODE_HUMIDITY) {
    // V1.2.27: control Out3 directly from the latest local humidity reading.
    // DS18B20 has no humidity; invalid humidity forces Out3 OFF.
    desired = sensorFreshForSmartControl(SMART_MODE_HUMIDITY) ? hysteresisDecision(currentHumidity, smartCfg.humOnX10, smartCfg.humOffX10, smartOutputState) : false;
  } else if (smartCfg.mode == SMART_MODE_TIMER) {
    if (!smartTimerActive) {
      desired = false;
    } else {
      uint32_t elapsed = (uint32_t)((millis() - smartTimerStartMs) / 1000UL);
      if (elapsed >= smartTimerDurationSec) {
        stopSmartTimer();
        desired = false;
      } else {
        desired = true;
      }
    }
  }

  if (smartCfg.mode != SMART_MODE_INTERNET_LED) {
    // V1.2.27: Force-write GPIO1/TX every loop while Out3 owns the pin.
    // Root fix: WiFi reconnect/status code or boot/TX activity may change the physical pin
    // without changing smartOutputState. If we only write when the RAM state changes,
    // Temp/Humidity/Timer can appear to stop controlling Out3.
    writeSmartOutput(desired);
  } else if (desired != smartOutputState) {
    writeSmartOutput(desired);
  }
#endif
}

String modeRadio(byte m) {
  String x;
  x += F("<label><input type='radio' name='mode' value='");
  x += String(m);
  x += F("' ");
  if (smartCfg.mode == m) x += F("checked");
  x += F(" onchange='modeChanged()'> ");
  x += smartModeText(m);
  x += F("</label>");
  return x;
}

// ========================= EXTERNAL CITY WEATHER =========================

String currentDeviceDateTimeText();

byte sanitizeWeatherCityIndex(byte idx) {
  return (idx < WEATHER_CITY_COUNT) ? idx : 0;
}

const char* selectedWeatherCityName() {
  return WEATHER_CITIES[sanitizeWeatherCityIndex(weatherCityIndex)].name;
}

void loadCityWeatherConfig() {
  if (EEPROM.read(EEPROM_WEATHER_MAGIC_ADDR) == EEPROM_WEATHER_MAGIC) {
    weatherCityIndex = sanitizeWeatherCityIndex(EEPROM.read(EEPROM_WEATHER_CITY_ADDR));
  } else {
    weatherCityIndex = 0;
    EEPROM.write(EEPROM_WEATHER_MAGIC_ADDR, EEPROM_WEATHER_MAGIC);
    EEPROM.write(EEPROM_WEATHER_CITY_ADDR, weatherCityIndex);
    EEPROM.commit();
  }
}

void saveCityWeatherConfig() {
  EEPROM.write(EEPROM_WEATHER_MAGIC_ADDR, EEPROM_WEATHER_MAGIC);
  EEPROM.write(EEPROM_WEATHER_CITY_ADDR, sanitizeWeatherCityIndex(weatherCityIndex));
  EEPROM.commit();
}

float parseLastJsonFloat(const String& payload, const char* key) {
  int idx = payload.lastIndexOf(key);
  if (idx < 0) return NAN;
  int colon = payload.indexOf(':', idx);
  if (colon < 0) return NAN;
  int start = colon + 1;
  while (start < (int)payload.length() && (payload[start] == ' ' || payload[start] == '\t' || payload[start] == '"')) start++;
  int end = start;
  while (end < (int)payload.length()) {
    char c = payload[end];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
      end++;
      continue;
    }
    break;
  }
  if (end <= start) return NAN;
  return payload.substring(start, end).toFloat();
}

String openMeteoUrlForCity(byte idx) {
  idx = sanitizeWeatherCityIndex(idx);
  String url = F("https://api.open-meteo.com/v1/forecast?latitude=");
  url += WEATHER_CITIES[idx].lat;
  url += F("&longitude=");
  url += WEATHER_CITIES[idx].lon;
  url += F("&current=temperature_2m,relative_humidity_2m&timezone=auto");
  return url;
}

bool updateCityWeather(bool force) {
#if ENABLE_CITY_WEATHER
  if (WiFi.status() != WL_CONNECTED) {
    cityWeatherOk = false;
    lastCityWeatherMessage = "No WiFi";
    return false;
  }

  unsigned long now = millis();
  if (!force) {
    if (now < CITY_WEATHER_BOOT_DELAY_MS) return false;
    if (lastCityWeatherCheckMs != 0 && now - lastCityWeatherCheckMs < CITY_WEATHER_INTERVAL_MS) return cityWeatherOk;
  }

  WiFiClientSecure wc;
  wc.setInsecure();
  HTTPClient http;
  http.setTimeout(7000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  String url = openMeteoUrlForCity(weatherCityIndex);
  if (!http.begin(wc, url)) {
    cityWeatherOk = false;
    lastCityWeatherMessage = "Weather begin failed";
    lastCityWeatherCheckMs = now;
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    cityWeatherOk = false;
    lastCityWeatherMessage = String("Weather HTTP ") + code;
    http.end();
    lastCityWeatherCheckMs = now;
    return false;
  }

  String payload = http.getString();
  http.end();

  float t = parseLastJsonFloat(payload, "\"temperature_2m\"");
  float h = parseLastJsonFloat(payload, "\"relative_humidity_2m\"");

  if (isnan(t) || isnan(h)) {
    cityWeatherOk = false;
    lastCityWeatherMessage = "Weather parse failed";
    lastCityWeatherCheckMs = now;
    return false;
  }

  cityWeatherTemp = t;
  cityWeatherHum = h;
  cityWeatherOk = true;
  lastCityWeatherMessage = "OK";
  lastCityWeatherTime = currentDeviceDateTimeText();
  lastCityWeatherCheckMs = now;
  return true;
#else
  return false;
#endif
}

String cityWeatherSelectHtml() {
  String x;
  x += F("<select name='city'>");
  for (byte i = 0; i < WEATHER_CITY_COUNT; i++) {
    x += F("<option value='");
    x += String(i);
    x += F("'");
    if (i == sanitizeWeatherCityIndex(weatherCityIndex)) x += F(" selected");
    x += F(">");
    x += WEATHER_CITIES[i].name;
    x += F("</option>");
  }
  x += F("</select>");
  return x;
}

String cityWeatherCardHtml() {
  String html;
  html.reserve(1400);
  html += F("<div class='card'><h1>City Weather</h1><div class='sub'>Manual refresh only to keep ESP-01 web fast. This does not use extra ESP01 pins.</div>");
  html += F("<form method='POST' action='/saveweather'><label class='small'>City</label>");
  html += sessionHiddenInput();
  html += cityWeatherSelectHtml();
  html += F("<button class='btn' type='submit'>Save City</button></form>");
  html += F("<div class='row'><span class='k'>City</span><span class='v' id='owcity'>");
  html += selectedWeatherCityName();
  html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Outdoor Temp</span><span class='v'><span id='owtemp'>");
  html += isnan(cityWeatherTemp) ? String("--") : String(cityWeatherTemp, 1);
  html += F("</span> &deg;C</span></div>");
  html += F("<div class='row'><span class='k'>Outdoor Humidity</span><span class='v'><span id='owhum'>");
  html += isnan(cityWeatherHum) ? String("--") : String(cityWeatherHum, 0);
  html += F("</span> %</span></div>");
  html += F("<div class='row'><span class='k'>Weather Status</span><span class='v' id='owstatus'>");
  html += lastCityWeatherMessage;
  html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Last Update</span><span class='v' id='owtime'>");
  html += lastCityWeatherTime;
  html += F("</span></div>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/weatherrefresh"); html += F("'>Refresh Weather</a>");
  html += F("</div>");
  return html;
}

void handleSaveWeatherCity() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  if (webServer.hasArg("city")) {
    weatherCityIndex = sanitizeWeatherCityIndex((byte)webServer.arg("city").toInt());
    saveCityWeatherConfig();
    cityWeatherOk = false;
    lastCityWeatherCheckMs = 0;
    lastCityWeatherMessage = "Saved - press Refresh";
    lastCityWeatherTime = "--";
  }
  webServer.sendHeader("Location", "/");
  webServer.send(303, "text/plain", "Saved");
}

void handleWeatherRefresh() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  updateCityWeather(true);
  webServer.sendHeader("Location", "/");
  webServer.send(303, "text/plain", "Weather refreshed");
}

const char* dayName(byte d) {
  switch (d) {
    case 0: return "Sun";
    case 1: return "Mon";
    case 2: return "Tue";
    case 3: return "Wed";
    case 4: return "Thu";
    case 5: return "Fri";
    default: return "Sat";
  }
}

String currentDeviceDateTimeText() {
  if (!isTimeValidNow()) return String("Waiting for NTP");
  time_t now = (time_t)getDeviceEpoch();
  struct tm *tmNow = gmtime(&now);
  if (!tmNow) return String("Waiting for NTP");
  char buf[32];
  snprintf(buf, sizeof(buf), "%s %04d-%02d-%02d %02d:%02d:%02d",
           dayName((byte)tmNow->tm_wday),
           tmNow->tm_year + 1900,
           tmNow->tm_mon + 1,
           tmNow->tm_mday,
           tmNow->tm_hour,
           tmNow->tm_min,
           tmNow->tm_sec);
  return String(buf);
}

String currentDeviceDayText() {
  if (!isTimeValidNow()) return String("--");
  time_t now = (time_t)getDeviceEpoch();
  struct tm *tmNow = gmtime(&now);
  if (!tmNow) return String("--");
  return String(dayName((byte)tmNow->tm_wday));
}

String dayScheduleRow(byte d) {
  String x;
  x += F("<div class='dayrow'><label class='small daylbl'><input type='checkbox' name='en");
  x += String(d);
  x += F("' ");
  if (smartCfg.dayEnabled[d]) x += F("checked");
  x += F("> ");
  x += dayName(d);
  x += F("</label>");
  for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
    byte shown = sl + 1;
    x += F("<label class='small'><input type='checkbox' name='s");
    x += String(d); x += String(sl);
    x += F("' ");
    if (smartCfg.slotEnabled[d][sl]) x += F("checked");
    x += F("> State "); x += String(shown); x += F("</label>");
    x += F("<input type='time' name='st");
    x += String(d); x += String(sl);
    x += F("' value='");
    x += minuteToTime(smartCfg.dayStartMin[d][sl]);
    x += F("'><input type='time' name='et");
    x += String(d); x += String(sl);
    x += F("' value='");
    x += minuteToTime(smartCfg.dayEndMin[d][sl]);
    x += F("'>");
  }
  x += F("</div>");
  return x;
}

String smartOutputCardHtml() {
  String html;
  html.reserve(6200);
  // Timer duration is runtime-only. After restart it is not restored from EEPROM.
  uint32_t dur = smartTimerActive ? smartTimerDurationSec : 0UL;
  byte th = dur / 3600UL;
  byte tm = (dur % 3600UL) / 60UL;
  byte ts = dur % 60UL;

  html += F("<div class='card'><h1>Output 3 Smart</h1><div class='sub'>GPIO1/TX active LOW. Select one mode only; the other mode settings are disabled automatically.</div>");
  html += F("<div class='row'><span class='k'>Current Mode</span><span class='v'>"); html += smartModeText(smartCfg.mode); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Output 3</span><span class='v'><span id='o3' class='"); html += smartOutputState ? F("on") : F("off"); html += F("'>"); html += smartOutputState ? F("ON") : F("OFF"); html += F("</span></span></div>");
  html += F("<div class='row'><span class='k'>Clock</span><span class='v' id='ntpstate'>"); html += clockStateText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Time Zone</span><span class='v'>"); html += egyptTimeModeText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Device Date / Time</span><span class='v' id='devtime'>"); html += currentDeviceDateTimeText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Schedule Day</span><span class='v' id='devday'>"); html += currentDeviceDayText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Timer Remaining</span><span class='v' id='tremain'>"); html += formatHMS(smartTimerRemainingSec()); html += F("</span></div>");

  html += F("<form method='POST' action='/savesmart'>");
  html += sessionHiddenInput();
  html += F("<div><label class='small'>Egypt Time</label><select name='tzmode'><option value='0'"); if(egyptTimeMode==EGYPT_TZ_AUTO) html+=F(" selected"); html+=F(">Auto Egypt DST</option><option value='1'"); if(egyptTimeMode==EGYPT_TZ_WINTER) html+=F(" selected"); html+=F(">Winter UTC+2</option><option value='2'"); if(egyptTimeMode==EGYPT_TZ_SUMMER) html+=F(" selected"); html+=F(">Summer UTC+3</option></select></div>");
  html += F("<div class='sub'>DST applies to NTP, Offline Clock, Dashboard, and Schedule. Manual override is available if the official rule changes.</div>");
  html += F("<div class='modepick'>");
  html += modeRadio(SMART_MODE_INTERNET_LED);
  html += modeRadio(SMART_MODE_MANUAL_SCHED);
  html += modeRadio(SMART_MODE_TEMPERATURE);
  html += modeRadio(SMART_MODE_HUMIDITY);
  html += modeRadio(SMART_MODE_TIMER);
  html += F("</div>");

  html += F("<div class='smsec' data-mode='1'><h1>Manual / Weekly Schedule</h1>");
  html += F("<div class='grid2'><div><label class='small'>Manual State</label><select name='manual'><option value='0'"); if(!smartCfg.manualState) html+=F(" selected"); html+=F(">OFF</option><option value='1'"); if(smartCfg.manualState) html+=F(" selected"); html+=F(">ON</option></select></div>");
  html += F("<div><label class='small'>Schedule Enable</label><select name='sche'><option value='0'"); if(!smartCfg.scheduleEnable) html+=F(" selected"); html+=F(">OFF</option><option value='1'"); if(smartCfg.scheduleEnable) html+=F(" selected"); html+=F(">ON</option></select></div></div>");
  html += F("<label class='small'>Repeat</label><select name='rep'><option value='1'"); if(smartCfg.repeatWeekly) html+=F(" selected"); html+=F(">Weekly Repeat</option><option value='0'"); if(!smartCfg.repeatWeekly) html+=F(" selected"); html+=F(">Once only</option></select>");
  html += F("<div class='sub'>Enable the day, then enable State 1 and/or State 2. Each state has its own ON/OFF time. Overnight periods like 22:00 to 02:00 are supported.</div>");
  html += F("<div class='dayhdr small'><b>Day</b><b>State</b><b>ON</b><b>OFF</b></div>");
  html += dayScheduleRow(6); html += dayScheduleRow(0); html += dayScheduleRow(1); html += dayScheduleRow(2); html += dayScheduleRow(3); html += dayScheduleRow(4); html += dayScheduleRow(5);
  html += F("</div>");

  html += F("<div class='smsec' data-mode='2'><h1>Temperature Control</h1>");
  html += F("<div class='grid2'><div><label class='small'>Temp ON C</label><input type='number' step='0.1' name='ton' value='"); html += String(smartCfg.tempOnX10 / 10.0, 1); html += F("'></div><div><label class='small'>Temp OFF C</label><input type='number' step='0.1' name='toff' value='"); html += String(smartCfg.tempOffX10 / 10.0, 1); html += F("'></div></div>");
  html += F("<div class='sub'>ON > OFF = cooling style. ON < OFF = heating style.</div></div>");

  html += F("<div class='smsec' data-mode='3'><h1>Humidity Control</h1>");
  html += F("<div class='grid2'><div><label class='small'>Hum ON %</label><input type='number' step='0.1' name='hon' value='"); html += String(smartCfg.humOnX10 / 10.0, 1); html += F("'></div><div><label class='small'>Hum OFF %</label><input type='number' step='0.1' name='hoff' value='"); html += String(smartCfg.humOffX10 / 10.0, 1); html += F("'></div></div>");
  html += F("<div class='sub'>ON > OFF = dehumidifier style. ON < OFF = humidifier style.</div></div>");

  html += F("<div class='smsec' data-mode='4'><h1>Timer</h1>");
  html += F("<div class='timergrid'><div><label class='small'>Hours</label><input type='number' min='0' max='23' step='1' name='th' value='"); html += String(th); html += F("'></div>");
  html += F("<div><label class='small'>Minutes</label><input type='number' min='0' max='59' step='1' name='tm' value='"); html += String(tm); html += F("'></div>");
  html += F("<div><label class='small'>Seconds</label><input type='number' min='0' max='59' step='1' name='ts' value='"); html += String(ts); html += F("'></div></div>");
  html += F("<div class='sub'>Range: 00:00:01 to 23:59:59. Timer is runtime-only: after ESP restart, output stays OFF and remaining time resets to 00:00:00. The timer duration is not saved to EEPROM.</div></div>");

  html += F("<button class='btn' type='submit'>Save Output 3 Settings</button></form>");
  html += F("<script>function modeChanged(){var m=document.querySelector('input[name=mode]:checked').value;document.querySelectorAll('.smsec').forEach(function(s){var on=s.getAttribute('data-mode')==m;s.classList.toggle('disabled',!on);s.querySelectorAll('input,select').forEach(function(e){e.disabled=!on;});});}modeChanged();</script>");
  html += F("</div>");
  return html;
}

void handleSaveSmartOutput() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  if (webServer.hasArg("tzmode")) {
    byte newTz = constrain(webServer.arg("tzmode").toInt(), 0, EGYPT_TZ_SUMMER);
    if (newTz != egyptTimeMode) { egyptTimeMode = newTz; saveEgyptTimeMode(); }
  }
  byte oldMode = smartCfg.mode;
  smartCfg.mode = constrain(webServer.arg("mode").toInt(), 0, SMART_MODE_MAX);
  if (smartCfg.mode != SMART_MODE_MANUAL_SCHED) {
    // V1.2.26: schedule is valid only inside Manual / Weekly Schedule mode.
    // This prevents a hidden stale schedule flag from being ignored or conflicting with other modes.
    smartCfg.scheduleEnable = 0;
  }
  if (webServer.hasArg("manual")) smartCfg.manualState = webServer.arg("manual").toInt() ? 1 : 0;
  if (smartCfg.mode == SMART_MODE_MANUAL_SCHED && webServer.hasArg("sche")) smartCfg.scheduleEnable = webServer.arg("sche").toInt() ? 1 : 0;
  if (webServer.hasArg("rep")) smartCfg.repeatWeekly = webServer.arg("rep").toInt() ? 1 : 0;
  if (smartCfg.repeatWeekly) smartCfg.onceDone = 0;
  if (oldMode != smartCfg.mode) {
    smartCfg.onceDone = 0;
    if (smartCfg.mode == SMART_MODE_TEMPERATURE || smartCfg.mode == SMART_MODE_HUMIDITY) {
      smartOutputState = false;
      writeSmartOutput(false);
      lastSmartSensorControlReadMs = 0;
    }
  }

  if (webServer.hasArg("st00") || webServer.hasArg("et00")) {
    for (byte i = 0; i < 7; i++) {
      smartCfg.dayEnabled[i] = webServer.hasArg(String("en") + i) ? 1 : 0;
      for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
        String si = String(i) + String(sl);
        smartCfg.slotEnabled[i][sl] = webServer.hasArg(String("s") + si) ? 1 : 0;
        smartCfg.dayStartMin[i][sl] = parseTimeToMinutes(webServer.arg(String("st") + si), smartCfg.dayStartMin[i][sl]);
        smartCfg.dayEndMin[i][sl] = parseTimeToMinutes(webServer.arg(String("et") + si), smartCfg.dayEndMin[i][sl]);
      }
    }
  }

  if (webServer.hasArg("ton")) smartCfg.tempOnX10 = (int16_t)(webServer.arg("ton").toFloat() * 10.0);
  if (webServer.hasArg("toff")) smartCfg.tempOffX10 = (int16_t)(webServer.arg("toff").toFloat() * 10.0);
  if (webServer.hasArg("hon")) smartCfg.humOnX10 = (int16_t)(webServer.arg("hon").toFloat() * 10.0);
  if (webServer.hasArg("hoff")) smartCfg.humOffX10 = (int16_t)(webServer.arg("hoff").toFloat() * 10.0);

  uint32_t requestedTimerSec = 0;
  bool hasTimerArgs = (webServer.hasArg("th") || webServer.hasArg("tm") || webServer.hasArg("ts"));
  if (hasTimerArgs) {
    uint32_t th = constrain(webServer.arg("th").toInt(), 0, 23);
    uint32_t tm = constrain(webServer.arg("tm").toInt(), 0, 59);
    uint32_t ts = constrain(webServer.arg("ts").toInt(), 0, 59);
    requestedTimerSec = clampTimerSeconds(th * 3600UL + tm * 60UL + ts);
  }

  if (smartCfg.mode == SMART_MODE_TIMER) {
    startSmartTimer(hasTimerArgs ? requestedTimerSec : 1UL);
  } else {
    stopSmartTimer();
  }

  saveSmartOutputConfig();
  applySmartOutputControl();
  webServer.sendHeader("Location", "/");
  webServer.send(303, "text/plain", "Saved");
}

String outputControlCardHtml() {
  String html;
  html.reserve(2100);
  html += F("<div class='card'><h1>Control what you wish</h1><div class='sub'>Output 1 / Output 2 live control. The state below is read from the ESP01 itself; any change is saved, applied to GPIO, and marked for ThingSpeak sending.</div>");
  html += F("<div class='oc'>");

  html += F("<div class='ocbox'><div class='octop'><div><b>PUMP</b><div class='small'>Output 1 / GPIO0</div></div><div><span id='o1' class='bigstate "); html += outputClass(currentOutput1State); html += F("'>"); html += outputStateText(currentOutput1State); html += F("</span><span id='d1' class='dot "); html += outputClass(currentOutput1State); html += F("'></span></div></div>");
  html += F("<button id='b1' class='btn' onclick='toggleOut(1)'>...</button></div>");

  html += F("<div class='ocbox'><div class='octop'><div><b>LIGHT</b><div class='small'>Output 2 / GPIO3 RX</div></div><div><span id='o2' class='bigstate "); html += outputClass(currentOutput2State); html += F("'>"); html += outputStateText(currentOutput2State); html += F("</span><span id='d2' class='dot "); html += outputClass(currentOutput2State); html += F("'></span></div></div>");
  html += F("<button id='b2' class='btn' onclick='toggleOut(2)'>...</button></div>");

  html += F("</div>");
  html += F("<div class='row'><span class='k'>ThingSpeak Sync</span><span class='v'><span id='pend' class='pill "); html += pendingOutputThingSpeakUpdate ? F("warn") : F("ok"); html += F("'>"); html += pendingOutputThingSpeakUpdate ? F("PENDING") : F("OK"); html += F("</span></span></div>");
  html += F("<div class='row'><span class='k'>Sensor</span><span class='v'><span id='dhttype'>"); html += dhtTypeText(); html += F("</span></span></div>");
  html += F("<div class='row'><span class='k'>Temperature</span><span class='v'><span id='temp'>--</span> &deg;C</span></div>");
  html += F("<div class='row'><span class='k'>Humidity</span><span class='v'><span id='hum'>--</span> %</span></div>");
  html += F("<div class='msg' id='outmsg'>Ready. Ultra-lite status refresh every 5 seconds. No full page reload.</div></div>");
  return html;
}

String outputControlJs() {
  String js;
  js.reserve(1500);
  js += F("var outState={1:0,2:0};");
  js += F("function paintOne(i,v){outState[i]=v?1:0;let e=document.getElementById('o'+i),d=document.getElementById('d'+i),b=document.getElementById('b'+i);if(e){e.innerHTML=v?'ON':'OFF';e.className='bigstate '+(v?'on':'off')}if(d){d.className='dot '+(v?'on':'off')}if(b){b.innerHTML=v?'TURN '+(i==1?'PUMP':'LIGHT')+' OFF':'TURN '+(i==1?'PUMP':'LIGHT')+' ON';b.className='btn '+(v?'offbtn':'onbtn')}}");
  js += F("function paintOut(d){paintOne(1,d.o1);paintOne(2,d.o2);let p=document.getElementById('pend');if(p){p.innerHTML=d.pending?'PENDING':'OK';p.className='pill '+(d.pending?'warn':'ok')}let t=document.getElementById('temp'),h=document.getElementById('hum'),dt=document.getElementById('dhttype');if(t)t.innerHTML=(d.temp===null||d.temp===undefined)?'--':d.temp;if(h)h.innerHTML=(d.hum===null||d.hum===undefined)?'--':d.hum;if(dt)dt.innerHTML=d.dht||'';let o3=document.getElementById('o3');if(o3){o3.innerHTML=d.o3?'ON':'OFF';o3.className=d.o3?'on':'off'}let ns=document.getElementById('ntpstate');if(ns)ns.innerHTML=d.clockState||'No Time';let dv=document.getElementById('devtime');if(dv)dv.innerHTML=d.deviceTime||'Waiting for NTP';let dd=document.getElementById('devday');if(dd)dd.innerHTML=d.deviceDay||'--';let tr=document.getElementById('tremain');if(tr)tr.innerHTML=d.timerRemain||'00:00:00';let oc=document.getElementById('owcity');if(oc)oc.innerHTML=d.city||'--';let ot=document.getElementById('owtemp');if(ot)ot.innerHTML=(d.owTemp===null||d.owTemp===undefined)?'--':d.owTemp;let oh=document.getElementById('owhum');if(oh)oh.innerHTML=(d.owHum===null||d.owHum===undefined)?'--':d.owHum;let os=document.getElementById('owstatus');if(os)os.innerHTML=d.owStatus||'--';let ou=document.getElementById('owtime');if(ou)ou.innerHTML=d.owTime||'--'}");
  js += F("function refreshOut(){fetch('/status?sid="); js += webSessionToken; js += F("').then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(paintOut).catch(e=>{})}");
  js += F("function setOut(o,s){let m=document.getElementById('outmsg');if(m)m.innerHTML='Sending...';fetch('/setoutput',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'out='+o+'&state='+s+'&sid="); js += webSessionToken; js += F("'}).then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(d=>{paintOut(d);if(m)m.innerHTML=d.changed?'Applied. Full ThingSpeak state pending/synced.':'No change.';}).catch(e=>{if(m)m.innerHTML='<span class=bad>Output command failed/session</span>'})}");
  js += F("function toggleOut(o){setOut(o,outState[o]?0:1)}");
  js += F("setInterval(refreshOut,5000);refreshOut();");
  return js;
}

String outputStatusJson(bool changed = false) {
  String json = F("{\"o1\":");
  json += currentOutput1State;
  json += F(",\"o2\":");
  json += currentOutput2State;
  json += F(",\"pending\":");
  json += pendingOutputThingSpeakUpdate ? F("true") : F("false");
  json += F(",\"changed\":");
  json += changed ? F("true") : F("false");
  json += F(",\"temp\":");
  json += sensorValueJson(currentTemperature);
  json += F(",\"hum\":");
  json += sensorValueJson(currentHumidity);
  json += F(",\"dht\":\"");
  json += dhtTypeText();
  json += F("\",\"sensorOk\":");
  json += sensorReadOk ? F("true") : F("false");
  json += F(",\"dsPresent\":");
  json += ds18b20Present ? F("true") : F("false");
  json += F(",\"o3\":");
  json += smartOutputState ? F("1") : F("0");
  json += F(",\"smartMode\":");
  json += smartCfg.mode;
  json += F(",\"timerActive\":");
  json += smartTimerActive ? F("true") : F("false");
  json += F(",\"timerRemain\":\"");
  json += formatHMS(smartTimerRemainingSec());
  json += F("\"");
  json += F(",\"timeOk\":");
  json += isTimeValidNow() ? F("true") : F("false");
  json += F(",\"clockState\":\"");
  json += clockStateText();
  json += F("\"");
  json += F(",\"timeZone\":\"");
  json += egyptTimeModeText();
  json += F("\"");
  json += F(",\"deviceTime\":\"");
  json += currentDeviceDateTimeText();
  json += F("\"");
  json += F(",\"deviceDay\":\"");
  json += currentDeviceDayText();
  json += F("\"");
  json += F(",\"city\":\"");
  json += selectedWeatherCityName();
  json += F("\"");
  json += F(",\"owTemp\":");
  json += sensorValueJson(cityWeatherTemp);
  json += F(",\"owHum\":");
  json += sensorValueJson(cityWeatherHum);
  json += F(",\"owStatus\":\"");
  json += lastCityWeatherMessage;
  json += F("\"");
  json += F(",\"owTime\":\"");
  json += lastCityWeatherTime;
  json += F("\"");
  json += F(",\"field5User\":\"");
  json += thingSpeakLoginUserText();
  json += F("\"");
  json += F(",\"duckdns\":\"");
  json += lastDuckDNSMessage;
  json += F("\"");
  json += F(",\"role\":\"");
  json += webIsAdmin() ? F("admin") : F("user");
  json += F("\"");
  json += F(",\"wifiOnline\":");
  json += isStaOnline() ? F("true") : F("false");
  json += F(",\"networkState\":\"");
  json += networkStatusText();
  json += F("\"");
  json += F("}");
  return json;
}

bool setOutputFromWeb(byte outputNumber, byte newState) {
  if (!isValidState(newState)) return false;
  bool changed = false;
  if (outputNumber == 1) {
    if (currentOutput1State != newState) {
      currentOutput1State = newState;
      setRelay(OUTPUT1_PIN, currentOutput1State);
      saveOutputState(1, currentOutput1State);
      changed = true;
    }
  } else if (outputNumber == 2) {
    if (currentOutput2State != newState) {
      currentOutput2State = newState;
      setRelay(OUTPUT2_PIN, currentOutput2State);
      saveOutputState(2, currentOutput2State);
      changed = true;
    }
  }

  if (changed) {
    // V1.2.35: GPIO + EEPROM happen immediately, but ThingSpeak is background/pending.
    // This keeps local AP control fast even when internet/ThingSpeak is slow or offline.
    markOutputThingSpeakUpdatePending("Web output manual control");
  }
  return changed;
}

void sendStatusJson() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  // V1.2.35: cache-only status endpoint for fast AP/web response.
  // Sensor reading and Out3 smart control are handled in loop(), independent of the web page.
  webServer.send(200, "application/json", outputStatusJson(false));
}

void handleSetOutput() {
  if (!requireRole(WEB_ROLE_OPERATOR)) return;
  byte out = (byte)webServer.arg("out").toInt();
  byte state = (byte)webServer.arg("state").toInt();
  if ((out != 1 && out != 2) || !isValidState(state)) {
    webServer.send(400, "application/json", F("{\"error\":\"bad request\"}"));
    return;
  }
  bool changed = setOutputFromWeb(out, state);
  webServer.send(200, "application/json", outputStatusJson(changed));
}


void sendHomePage() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  bool canOperate = roleAtLeast(WEB_ROLE_OPERATOR);
  bool canEngineer = roleAtLeast(WEB_ROLE_ENGINEER);
  bool admin = webIsAdmin();

  // V1.2.20 Emergency Web Load Fix:
  // Root page is intentionally very small so ESP-01 can always answer fast,
  // even in Local Mode or when internet/weather/GitHub services are unavailable.
  String html = htmlHeader("ESP01 Lite Home");
  html += F("<div class='card'><h1>AHMED MONEM SYSTEM</h1><div class='sub'>Fast loading page. Design By Ahmed Monem / +201004608852.</div>");
  html += F("<div class='row'><span class='k'>User</span><span class='v'>"); html += currentWebUserName(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Role</span><span class='v'>"); html += roleName(currentWebRole); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Network</span><span class='v'>"); html += networkStatusText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>AP IP</span><span class='v'>"); html += WiFi.softAPIP().toString(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>STA IP</span><span class='v'>"); html += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Not connected")); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>STA IP Mode</span><span class='v'>"); html += wifiStaticEnabled ? String("Static") : String("DHCP"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Temperature</span><span class='v'><span id='temp'>--</span> &deg;C</span></div>");
  html += F("<div class='row'><span class='k'>Humidity</span><span class='v'><span id='hum'>--</span> %</span></div>");
  html += F("<div class='row'><span class='k'>PUMP</span><span class='v'><span id='o1'>--</span></span></div>");
  html += F("<div class='row'><span class='k'>LIGHT</span><span class='v'><span id='o2'>--</span></span></div>");
  html += F("</div>");

  if (canOperate) {
    html += F("<div class='card'><b>Quick Control</b><div class='grid2'><button class='btn' onclick='toggleOut(1)'>Toggle Pump</button><button class='btn' onclick='toggleOut(2)'>Toggle Light</button></div><div class='msg' id='outmsg'>Ready</div></div>");
  }

  html += F("<div class='card'><b>Pages</b>");
  html += F("<a class='btn btn2' href='/logout'>Logout</a>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/dashboard"); html += F("'>Full Dashboard</a>");
  if (canEngineer) html += F("<a class='btn btn2' href='"); html += sessionUrl("/wifi"); html += F("'>WiFi / Settings</a>");
  if (admin) html += F("<a class='btn btn2' href='"); html += sessionUrl("/users"); html += F("'>User Management</a><a class='btn btn2' href='"); html += sessionUrl("/update"); html += F("'>Firmware Update</a><a class='btn btn2' href='"); html += sessionUrl("/restart"); html += F("'>Restart</a>");
  html += F("</div>");

  html += F("<script>");
  html += F("var outState={1:0,2:0};function paint(d){outState[1]=d.o1?1:0;outState[2]=d.o2?1:0;let t=document.getElementById('temp'),h=document.getElementById('hum'),a=document.getElementById('o1'),b=document.getElementById('o2');if(t)t.innerHTML=(d.temp==null?'--':d.temp);if(h)h.innerHTML=(d.hum==null?'--':d.hum);if(a)a.innerHTML=d.o1?'ON':'OFF';if(b)b.innerHTML=d.o2?'ON':'OFF';}");
  html += F("function refresh(){fetch('/status?sid="); html += webSessionToken; html += F("').then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(paint).catch(e=>{})}function setOut(o,s){let m=document.getElementById('outmsg');if(m)m.innerHTML='Sending...';fetch('/setoutput',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'out='+o+'&state='+s+'&sid="); html += webSessionToken; html += F("'}).then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(d=>{paint(d);if(m)m.innerHTML='Done';}).catch(e=>{if(m)m.innerHTML='Failed/session';})}function toggleOut(o){setOut(o,outState[o]?0:1)}setInterval(refresh,7000);refresh();");
  html += F("</script>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}


// ========================= EEPROM BACKUP / RESTORE =========================

uint16_t eepromBackupChecksum() {
  uint16_t sum = 0;
  for (uint16_t i = 0; i < EEPROM_SIZE; i++) sum = (uint16_t)(sum + EEPROM.read(i));
  return sum;
}

String eepromBackupFileName() {
  String fn = "esp01_settings_v";
  fn += FW_VERSION;
  fn.replace(".", "_");
  fn += ".bin";
  return fn;
}

void sendEepromBackup() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;

  uint16_t checksum = eepromBackupChecksum();
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.sendHeader("Content-Type", "application/octet-stream");
  webServer.sendHeader("Content-Disposition", "attachment; filename=\"" + eepromBackupFileName() + "\"");
  webServer.send(200, "application/octet-stream", "");

  const char magic[8] = EEPROM_BACKUP_MAGIC;
  webServer.sendContent_P(magic, 8);

  char h[6];
  h[0] = (char)(EEPROM_BACKUP_MAP_VERSION >> 8);
  h[1] = (char)(EEPROM_BACKUP_MAP_VERSION & 0xFF);
  h[2] = (char)(EEPROM_SIZE >> 8);
  h[3] = (char)(EEPROM_SIZE & 0xFF);
  h[4] = (char)(checksum >> 8);
  h[5] = (char)(checksum & 0xFF);
  webServer.sendContent_P(h, 6);

  char buf[64];
  for (uint16_t i = 0; i < EEPROM_SIZE; i += sizeof(buf)) {
    uint16_t n = EEPROM_SIZE - i;
    if (n > sizeof(buf)) n = sizeof(buf);
    for (uint16_t j = 0; j < n; j++) buf[j] = (char)EEPROM.read(i + j);
    webServer.sendContent_P(buf, n);
    yield();
  }
  webServer.sendContent("");
}

bool restoreBackupActive = false;
uint16_t restoreBackupIndex = 0;
uint16_t restoreBackupExpectedSize = 0;
uint16_t restoreBackupExpectedChecksum = 0;
uint16_t restoreBackupRunningChecksum = 0;
bool restoreBackupHeaderOk = false;
String restoreBackupMessage = "No restore yet";

void handleRestoreBackupUpload() {
  if (!webAuthCheck(true)) return;
  HTTPUpload& up = webServer.upload();

  if (up.status == UPLOAD_FILE_START) {
    restoreBackupActive = true;
    restoreBackupIndex = 0;
    restoreBackupExpectedSize = 0;
    restoreBackupExpectedChecksum = 0;
    restoreBackupRunningChecksum = 0;
    restoreBackupHeaderOk = false;
    restoreBackupMessage = "Restore started";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!restoreBackupActive) return;
    for (uint16_t i = 0; i < up.currentSize; i++) {
      byte b = up.buf[i];
      if (restoreBackupIndex < 8) {
        const char* magic = EEPROM_BACKUP_MAGIC;
        if (b != (byte)magic[restoreBackupIndex]) { restoreBackupActive = false; restoreBackupMessage = "Restore failed: bad backup magic"; return; }
      } else if (restoreBackupIndex == 8) {
        restoreBackupExpectedSize = ((uint16_t)b) << 8;
      } else if (restoreBackupIndex == 9) {
        uint16_t mapVer = restoreBackupExpectedSize | b;
        restoreBackupExpectedSize = 0;
        if (mapVer != EEPROM_BACKUP_MAP_VERSION) { restoreBackupActive = false; restoreBackupMessage = "Restore failed: EEPROM map version mismatch"; return; }
      } else if (restoreBackupIndex == 10) {
        restoreBackupExpectedSize = ((uint16_t)b) << 8;
      } else if (restoreBackupIndex == 11) {
        restoreBackupExpectedSize |= b;
        if (restoreBackupExpectedSize != EEPROM_SIZE) { restoreBackupActive = false; restoreBackupMessage = "Restore failed: EEPROM size mismatch"; return; }
      } else if (restoreBackupIndex == 12) {
        restoreBackupExpectedChecksum = ((uint16_t)b) << 8;
      } else if (restoreBackupIndex == 13) {
        restoreBackupExpectedChecksum |= b;
        restoreBackupHeaderOk = true;
      } else {
        if (!restoreBackupHeaderOk) { restoreBackupActive = false; restoreBackupMessage = "Restore failed: header error"; return; }
        uint16_t addr = restoreBackupIndex - 14;
        if (addr < EEPROM_SIZE) {
          EEPROM.write(addr, b);
          restoreBackupRunningChecksum = (uint16_t)(restoreBackupRunningChecksum + b);
        }
      }
      restoreBackupIndex++;
      yield();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!restoreBackupActive) return;
    uint16_t dataLen = restoreBackupIndex >= 14 ? (restoreBackupIndex - 14) : 0;
    if (dataLen != EEPROM_SIZE) { restoreBackupActive = false; restoreBackupMessage = "Restore failed: incomplete backup"; return; }
    if (restoreBackupRunningChecksum != restoreBackupExpectedChecksum) { restoreBackupActive = false; restoreBackupMessage = "Restore failed: checksum mismatch"; return; }
    EEPROM.commit();
    restoreBackupActive = false;
    restoreBackupMessage = "Restore OK - restarting";
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    restoreBackupActive = false;
    restoreBackupMessage = "Restore aborted";
  }
}

void handleRestoreBackupDone() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  String html = htmlHeader("Restore Backup");
  html += F("<div class='card'><h1>Restore Backup</h1><div class='sub'>");
  html += restoreBackupMessage;
  html += F("</div>");
  if (restoreBackupMessage.startsWith("Restore OK")) {
    html += F("<div class='msg ok'>Settings restored. ESP01 will restart now.</div>");
    html += F("<script>setTimeout(function(){location.href='/login';},12000);</script>");
  } else {
    html += F("<a class='btn btn2' href='"); html += sessionUrl(String(WEB_OTA_PATH)); html += F("'>Back</a>");
  }
  html += F("</div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
  if (restoreBackupMessage.startsWith("Restore OK")) { delay(700); ESP.restart(); }
}


void sendUpdatePage() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  String html = htmlHeader("Firmware Update");
  html += F("<div class='card'><h1>Firmware Update</h1><div class='sub'>Upload ESP8266/ESP-01 .bin file only.</div>");
  html += F("<form method='POST' action='"); html += WEB_OTA_PATH; html += F("' enctype='multipart/form-data' id='upForm'>");
  html += F("<input type='file' name='firmware' accept='.bin' required><button class='btn' type='submit'>Update Firmware</button>");
  html += F("<div class='bar'><div class='fill' id='fill'></div></div><div class='msg' id='msg'>Waiting for file...</div></form><a class='btn btn2' href='/'>Back</a></div>");
  html += F("<script>const f=document.getElementById('upForm'),bar=document.getElementById('fill'),msg=document.getElementById('msg');f.onsubmit=e=>{e.preventDefault();let d=new FormData(f),x=new XMLHttpRequest();x.upload.onprogress=p=>{if(p.lengthComputable){let n=Math.round(p.loaded*100/p.total);bar.style.width=n+'%';msg.innerHTML='Uploading '+n+'%';}};x.onload=()=>{bar.style.width='100%';msg.innerHTML=x.status==200?'<span class=ok>'+x.responseText+'</span>':'<span class=bad>Update failed</span>';};x.onerror=()=>msg.innerHTML='<span class=bad>Connection error</span>';x.open('POST','"); html += WEB_OTA_PATH; html += F("');x.send(d);};</script>");

  html += F("<div class='card'><h1>GitHub Auto Update Options</h1>");
  html += F("<div class='sub'>Keep this OFF or Manual when you need the fastest ESP01 web response. Manual Check button still works.</div>");
  html += F("<form method='POST' action='/saveupdateopts'>");
  html += sessionHiddenInput();
  html += F("<label class='sub'>GitHub Auto Update</label><select name='ghauto'><option value='0'");
  if (!githubAutoUpdateEnabled) html += F(" selected");
  html += F(">OFF</option><option value='1'");
  if (githubAutoUpdateEnabled) html += F(" selected");
  html += F(">ON</option></select>");
  html += F("<label class='sub'>Auto Check Interval</label><select name='ghint'>");
  html += F("<option value='0'");
  if (githubAutoUpdateIntervalMode == GH_AUTO_INTERVAL_MANUAL) html += F(" selected");
  html += F(">Manual only</option><option value='1'");
  if (githubAutoUpdateIntervalMode == GH_AUTO_INTERVAL_1H) html += F(" selected");
  html += F(">1 hour</option><option value='2'");
  if (githubAutoUpdateIntervalMode == GH_AUTO_INTERVAL_6H) html += F(" selected");
  html += F(">6 hours</option><option value='3'");
  if (githubAutoUpdateIntervalMode == GH_AUTO_INTERVAL_12H) html += F(" selected");
  html += F(">12 hours</option><option value='4'");
  if (githubAutoUpdateIntervalMode == GH_AUTO_INTERVAL_24H) html += F(" selected");
  html += F(">24 hours</option></select>");
  html += F("<div class='row'><span class='k'>Current Auto Mode</span><span class='v'>");
  html += githubAutoUpdateEnabled ? F("ON / ") : F("OFF / ");
  html += githubIntervalModeText(githubAutoUpdateIntervalMode);
  html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Last Update Check</span><span class='v'>");
  html += lastAutoUpdateCheckMs ? (String((millis() - lastAutoUpdateCheckMs) / 1000UL) + F(" sec ago")) : String("Not checked");
  html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Last Message</span><span class='v'>");
  html += lastAutoUpdateMessage;
  html += F("</span></div>");
  html += F("<button class='btn' type='submit'>Save Update Options</button> ");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/checkupdate"); html += F("'>Manual GitHub Check</a>");
  html += F("</form></div>");

  html += F("<div class='card'><h1>Backup & Restore</h1>");
  html += F("<div class='sub'>Download or restore full EEPROM settings. Use this before Erase Flash / Factory Reset.</div>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/backup"); html += F("'>Download Settings Backup</a>");
  html += F("<form method='POST' action='/restore' enctype='multipart/form-data'>");
  html += sessionHiddenInput();
  html += F("<input type='file' name='backup' accept='.bin' required>");
  html += F("<button class='btn' type='submit' onclick=\"return confirm('Restore EEPROM settings and restart?');\">Upload / Restore Backup</button>");
  html += F("</form><div class='sub warn'>Restore only backups made by the same EEPROM map version.</div></div>");

  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void sendDashboardPage() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  bool admin = webIsAdmin();
  bool canOperate = roleAtLeast(WEB_ROLE_OPERATOR);
  bool canEngineer = roleAtLeast(WEB_ROLE_ENGINEER);

  String html = htmlHeader("ESP01 OTA Pro");
  html += F("<div class='card'><h1>AHMED MONEM Controller</h1>");
  html += F("<div class='sub'>Secure professional OTA + WiFi Manager panel. Upload only a valid compiled <b>.bin</b> firmware file +201004608852.</div>");
  html += F("<div class='row'><span class='k'>Current User</span><span class='v'>"); html += currentWebUserName(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Login Role</span><span class='v'>"); html += roleName(currentWebRole); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Last Login</span><span class='v'>"); html += String(lastLoginUser); html += F(" / "); html += String(lastLoginIp); html += F("</span></div>");
  if (canEngineer) html += F("<a class='btn btn2' href='"); html += sessionUrl("/wifi"); html += F("'>WiFi / Network Setup</a>");
  html += F("</div><div class='card'>");
  html += F("<div class='row'><span class='k'>WiFi Mode</span><span class='v'>"); html += wifiModeText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Network State</span><span class='v'>"); html += networkStatusText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Saved SSID</span><span class='v'>"); html += String(activeSsid[0] ? activeSsid : "Not saved"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>STA IP</span><span class='v'>"); html += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Not connected")); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>DuckDNS</span><span class='v'>"); html += lastDuckDNSMessage; html += F("</span></div>");
  html += F("<div class='row'><span class='k'>AP IP</span><span class='v'>"); html += WiFi.softAPIP().toString(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Device Name</span><span class='v'>"); html += String(deviceName); html += F("</span></div>");
  if (admin) {
  html += F("<div class='row'><span class='k'>Chip ID</span><span class='v'>"); html += String(((uint32_t)ESP.getEfuseMac()), HEX); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Flash Size</span><span class='v'>"); html += String(ESP.getFlashChipSize() / 1024); html += F(" KB</span></div>");
  html += F("<div class='row'><span class='k'>Free Sketch Space</span><span class='v'>"); html += String(ESP.getFreeSketchSpace() / 1024); html += F(" KB</span></div>");
  html += F("<div class='row'><span class='k'>Firmware</span><span class='v'>"); html += FW_FULL_NAME; html += F(" / build "); html += String(FW_BUILD); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>GitHub OTA</span><span class='v'>"); html += lastAutoUpdateMessage; html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Auto Check</span><span class='v'>Boot + every 1 hour</span></div>");
  html += F("<div class='row'><span class='k'>Signal</span><span class='v'>"); html += (WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + String(" dBm") : String("-")); html += F("</span></div>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/checkupdate"); html += F("'>Check Ahmed Device Update</a>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/users"); html += F("'>User Management</a>");
  }
  html += F("</div>");
  if (canOperate) html += outputControlCardHtml();
  if (canEngineer) html += smartOutputCardHtml();
  html += cityWeatherCardHtml();
  html += tideButtonCardHtml();
  if (admin) {
  html += F("<div class='card'><form method='POST' action='"); html += WEB_OTA_PATH; html += F("' enctype='multipart/form-data' id='upForm'>");
  html += F("<b>Firmware Update</b><div class='sub'>Choose the exported firmware .bin file, then press update. Do not power off during upload.</div>");
  html += F("<input type='file' name='firmware' accept='.bin' required><button class='btn' type='submit'>Update Firmware</button>");
  html += F("<div class='bar'><div class='fill' id='fill'></div></div><div class='msg' id='msg'>Waiting for file...</div></form></div>");
  html += F("<div class='card'><a class='btn btn2' href='"); html += sessionUrl("/restart"); html += F("'>Restart ESP01</a><div class='sub warn' style='margin-top:10px'>Admin protected. Default first login: admin / admin</div></div>");
  html += F("<script>");
  html += F("const f=document.getElementById('upForm'),bar=document.getElementById('fill'),msg=document.getElementById('msg');f.onsubmit=e=>{e.preventDefault();let d=new FormData(f),x=new XMLHttpRequest();x.upload.onprogress=p=>{if(p.lengthComputable){let n=Math.round(p.loaded*100/p.total);bar.style.width=n+'%';msg.innerHTML='Uploading '+n+'%';}};x.onload=()=>{bar.style.width='100%';msg.innerHTML=x.status==200?'<span class=ok>'+x.responseText+'</span>':'<span class=bad>Update failed</span>';};x.onerror=()=>msg.innerHTML='<span class=bad>Connection error</span>';x.open('POST','"); html += WEB_OTA_PATH; html += F("');x.send(d);};");
  html += F("</script>");
  }
  html += F("<script>");
  html += outputControlJs();
  html += F("</script>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void sendWiFiPage() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  bool admin = webIsAdmin();
  bool canOperate = roleAtLeast(WEB_ROLE_OPERATOR);
  bool canEngineer = roleAtLeast(WEB_ROLE_ENGINEER);
  String html = htmlHeader("WiFi Manager");
  html += F("<div class='card'><h1>WiFi Manager</h1><div class='sub'>Scan, select network, save password, choose mode, or delete saved WiFi. AP access: <b>192.168.4.1</b></div><a class='btn btn2' href='/'>OTA Home</a></div>");
  html += F("<div class='card'>");
  html += F("<div class='row'><span class='k'>Mode</span><span class='v'>"); html += wifiModeText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Network State</span><span class='v'>"); html += networkStatusText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Saved SSID</span><span class='v'>"); html += String(activeSsid[0] ? activeSsid : "Not saved"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>STA IP</span><span class='v'>"); html += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Not connected")); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>AP SSID</span><span class='v'>"); html += String(configApSsid); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>AP IP</span><span class='v'>"); html += WiFi.softAPIP().toString(); html += F("</span></div>");
  html += F("</div>");

#if ENABLE_DUCKDNS
  html += F("<div class='card'><b>DuckDNS Settings</b><div class='sub'>Enter subdomain only, without .duckdns.org. Example: ahmedhome</div>");
  html += F("<div class='row'><span class='k'>Domain</span><span class='v'>"); html += String(duckDnsDomain[0] ? duckDnsDomain : "Not set"); html += F(".duckdns.org</span></div>");
  html += F("<div class='row'><span class='k'>Last Update</span><span class='v'>"); html += lastDuckDNSMessage; html += F("</span></div>");
  html += F("<form method='POST' action='/saveduck'><label class='sub'>Domain</label><input name='domain' maxlength='"); html += String(DUCK_DOMAIN_MAX_LEN); html += F("' value='"); html += String(duckDnsDomain); html += F("' placeholder='your-subdomain'>");
  html += sessionHiddenInput();
  html += F("<label class='sub'>Token</label><input name='token' maxlength='"); html += String(DUCK_TOKEN_MAX_LEN); html += F("' value='"); html += String(duckDnsToken); html += F("' placeholder='DuckDNS token'>");
  html += F("<button class='btn' type='submit'>Save DuckDNS</button></form>");
  html += F("<form method='POST' action='/clearduck'>"); html += sessionHiddenInput(); html += F("<button class='btn btn2' type='submit'>Clear DuckDNS</button></form>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/duckupdate"); html += F("'>Update DuckDNS Now</a>");
  html += F("</div>");
#endif

  
  html += F("<div class='card'><b>Web Server Port</b><div class='sub'>Change ESP01 web port. Restart required after saving.</div>");
  html += F("<form method='POST' action='/savewebport'>");
  html += sessionHiddenInput();
  html += F("<label class='sub'>Port</label><input name='port' type='number' min='1' max='65535' value='");
  html += String(webServerPort);
  html += F("'>");
  html += F("<button class='btn' type='submit'>Save Web Port</button></form>");
  html += F("<div class='sub warn'>If you change this, update router Port Forward Internal Port too.</div></div>");

  html += F("<div class='card'><b>Network Setup</b><div class='sub'>Press Scan then choose a network. The chosen SSID will stay in the box.</div>");
  html += F("<button class='btn' onclick='scanWifi()'>Scan Networks</button><select id='nets' onchange='pickNet()'><option value=''>No scan yet</option></select>");
  html += F("<form method='POST' action='/savewifi'><label class='sub'>SSID</label><input id='ssid' name='ssid' maxlength='32' value='"); html += String(activeSsid); html += F("' required>");
  html += sessionHiddenInput();
  html += F("<label class='sub'>Password</label><input id='pass' name='pass' maxlength='64' type='password' placeholder='Leave empty to keep old password'>");
  html += F("<label class='sub'>WiFi Mode</label><select name='mode'><option value='0'"); if (wifiManagerMode == WIFI_MODE_AP_STA_ALWAYS) html += F(" selected"); html += F(">0 - AP + STA Always</option><option value='1'"); if (wifiManagerMode == WIFI_MODE_STA_FALLBACK) html += F(" selected"); html += F(">1 - STA Only + Fallback AP</option></select>");
  html += F("<label class='small'><input type='checkbox' name='staticen' value='1' "); if (wifiStaticEnabled) html += F("checked"); html += F("> Use Static STA IP</label>");
  html += F("<div class='sub'>Static IP is optional. Leave unchecked for DHCP Auto IP.</div>");
  html += F("<label class='sub'>Static IP</label><input id='st_ip' name='stip' maxlength='15' value='"); html += String(wifiStaticIP); html += F("' placeholder='192.168.100.15'>");
  html += F("<label class='sub'>Gateway</label><input id='st_gw' name='stgw' maxlength='15' value='"); html += String(wifiStaticGateway); html += F("' placeholder='192.168.100.1'>");
  html += F("<label class='sub'>Subnet</label><input id='st_sub' name='stsub' maxlength='15' value='"); html += String(wifiStaticSubnet); html += F("' placeholder='255.255.255.0'>");
  html += F("<label class='sub'>DNS</label><input id='st_dns' name='stdns' maxlength='15' value='"); html += String(wifiStaticDns); html += F("' placeholder='8.8.8.8'>");
  html += F("<button class='btn btn2' type='button' onclick='useCurrentIp()'>Use Current IP</button>");
  html += F("<button class='btn' type='submit'>Save & Connect</button></form>");
  html += F("<form method='POST' action='/forgetwifi' onsubmit=\"return confirm('Delete saved WiFi?');\">"); html += sessionHiddenInput(); html += F("<button class='btn btn2' type='submit'>Delete / Forget WiFi</button></form><div class='msg' id='scanmsg'></div></div>");
  // V1.2.20: WiFi page stays light. Dashboard/control/weather/users are separate pages.
  if (admin) {
  html += F("<div class='card'><b>ThingSpeak Settings</b><div class='sub'>Change Channel ID and API keys without editing the sketch.</div>");
  html += F("<form method='POST' action='/savets'><label class='sub'>Channel ID</label><input name='channel' maxlength='16' value='"); html += String(tsChannelId); html += F("'>");
  html += sessionHiddenInput();
  html += F("<label class='sub'>Write API Key</label><input name='writekey' maxlength='32' value='"); html += String(tsWriteKey); html += F("'>");
  html += F("<label class='sub'>Read API Key</label><input name='readkey' maxlength='32' value='"); html += String(tsReadKey); html += F("'>");
  html += F("<label class='sub'>Command Read Interval</label><select name='tscmd'>");
  html += F("<option value='20'");
  if (commandCheckIntervalMs == 20000UL) html += F(" selected");
  html += F(">20 seconds</option><option value='40'");
  if (commandCheckIntervalMs == 40000UL) html += F(" selected");
  html += F(">40 seconds</option><option value='60'");
  if (commandCheckIntervalMs == 60000UL) html += F(" selected");
  html += F(">60 seconds</option><option value='120'");
  if (commandCheckIntervalMs == 120000UL) html += F(" selected");
  html += F(">120 seconds</option></select>");
  html += F("<div class='row'><span class='k'>Status</span><span class='v'>"); html += thingSpeakConfigured(true) ? String("Configured") : String("Not configured"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Command Interval</span><span class='v'>"); html += String(commandCheckIntervalMs / 1000UL); html += F(" sec</span></div>");
  html += F("<button class='btn' type='submit'>Save ThingSpeak</button></form>");
  html += F("<form method='POST' action='/clearts' onsubmit=\"return confirm('Clear ThingSpeak settings only?');\">"); html += sessionHiddenInput(); html += F("<button class='btn btn2' type='submit'>Clear ThingSpeak</button></form></div>");
  html += F("<div class='card'><b>Sensor Settings</b><div class='sub'>Choose DHT11, DHT22, or DS18B20. DS18B20 shows temperature only and humidity becomes -- in the web page.</div>");
  html += F("<form method='POST' action='/savedht'><label class='sub'>Sensor Type</label><select name='type'><option value='11'"); if (dhtSensorType == SENSOR_TYPE_DHT11) html += F(" selected"); html += F(">DHT11</option><option value='22'"); if (dhtSensorType == SENSOR_TYPE_DHT22) html += F(" selected"); html += F(">DHT22</option><option value='18'"); if (dhtSensorType == SENSOR_TYPE_DS18B20) html += F(" selected"); html += F(">DS18B20</option></select>");
  html += sessionHiddenInput();
  html += F("<div class='row'><span class='k'>Current Sensor</span><span class='v'><span id='dhttype'>"); html += dhtTypeText(); html += F("</span></span></div>");
  html += F("<div class='row'><span class='k'>Current Temperature</span><span class='v'><span id='temp'>--</span> &deg;C</span></div>");
  html += F("<div class='row'><span class='k'>Current Humidity</span><span class='v'><span id='hum'>--</span> %</span></div>");
  html += F("<button class='btn' type='submit'>Save Sensor Type</button></form></div>");
  html += F("<div class='card'><b>Device Settings</b><div class='sub'>Rename device and Access Point.</div>");
  html += F("<form method='POST' action='/savedevice'><label class='sub'>Device Name</label><input name='devname' maxlength='32' value='"); html += String(deviceName); html += F("'>");
  html += sessionHiddenInput();
  html += F("<label class='sub'>AP Name</label><input name='apssid' maxlength='32' value='"); html += String(configApSsid); html += F("'>");
  html += F("<label class='sub'>AP Password</label><input name='appass' maxlength='64' value='"); html += String(configApPass); html += F("'><div class='sub warn'>AP password must be 8 characters or more.</div>");
  html += F("<button class='btn' type='submit'>Save Device Settings</button></form></div>");
  html += F("<div class='card'><b>User Management</b><div class='sub'>Loaded separately to keep WiFi page fast.</div><a class='btn btn2' href='"); html += sessionUrl("/users"); html += F("'>Open User Management</a></div>");
  html += F("<div class='card'><b>System Control</b><a class='btn btn2' href='"); html += sessionUrl("/restart"); html += F("'>Restart Device</a><form method='POST' action='/factoryreset' onsubmit=\"return confirm('Factory reset will erase WiFi, ThingSpeak, and device names. Continue?');\">"); html += sessionHiddenInput(); html += F("<button class='btn btn2' type='submit'>Factory Reset</button></form></div>");
  }
  html += F("<script>");
  html += F("function useCurrentIp(){");
  html += F("document.getElementById('st_ip').value='"); html += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(wifiStaticIP)); html += F("';");
  html += F("document.getElementById('st_gw').value='"); html += (WiFi.status() == WL_CONNECTED ? WiFi.gatewayIP().toString() : String(wifiStaticGateway)); html += F("';");
  html += F("document.getElementById('st_sub').value='"); html += (WiFi.status() == WL_CONNECTED ? WiFi.subnetMask().toString() : String(wifiStaticSubnet)); html += F("';");
  html += F("document.getElementById('st_dns').value='"); html += (WiFi.status() == WL_CONNECTED ? WiFi.dnsIP().toString() : String(wifiStaticDns)); html += F("';}");
  html += F("function scanWifi(){let m=document.getElementById('scanmsg'),n=document.getElementById('nets');m.innerHTML='Scanning...';fetch('/scan?sid="); html += webSessionToken; html += F("').then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(a=>{n.innerHTML='<option value=\\\"\\\">Select network</option>';a.forEach(w=>{let o=document.createElement('option');o.value=w.s;o.text=w.s+' ('+w.r+' dBm)';n.add(o)});m.innerHTML='Found '+a.length+' networks';}).catch(e=>m.innerHTML='<span class=bad>Scan failed</span>')}");
  html += F("function pickNet(){let v=document.getElementById('nets').value;if(v)document.getElementById('ssid').value=v;}");
  // Output live JS removed from WiFi page in V1.2.19.
  html += F("</script>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void sendScanJson() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  int n = WiFi.scanNetworks(false, true);
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i) json += ',';
    String name = WiFi.SSID(i);
    name.replace("\\", "\\\\");
    name.replace("\"", "\\\"");
    json += "{\"s\":\"" + name + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
    yield();
  }
  json += "]";
  WiFi.scanDelete();
  webServer.send(200, "application/json", json);
}

void handleSaveWifi() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  String ns = webServer.arg("ssid");
  String np = webServer.arg("pass");
  byte nm = (webServer.arg("mode") == "1") ? WIFI_MODE_STA_FALLBACK : WIFI_MODE_AP_STA_ALWAYS;
  ns.trim();
  if (ns.length() == 0 || ns.length() > WIFI_SSID_MAX_LEN || np.length() > WIFI_PASS_MAX_LEN) {
    webServer.send(400, "text/plain", "Invalid SSID or password length");
    return;
  }
  if (np.length() == 0 && ns == String(activeSsid)) np = String(activePass);

  bool stEnable = webServer.hasArg("staticen");
  String stIp = webServer.arg("stip"); stIp.trim();
  String stGw = webServer.arg("stgw"); stGw.trim();
  String stSub = webServer.arg("stsub"); stSub.trim();
  String stDns = webServer.arg("stdns"); stDns.trim();

  if (!saveStaticIpConfig(stEnable, stIp, stGw, stSub, stDns)) {
    webServer.send(400, "text/plain", "Invalid Static IP / Gateway / Subnet / DNS");
    return;
  }

  saveWiFiManagerConfig(ns, np, nm);
  WiFi.disconnect(false);
  startWiFiAttempt();
  String html = htmlHeader("WiFi Saved");
  html += F("<div class='card'><h1>WiFi Saved</h1><div class='sub'>Settings saved. AP/web stayed active. STA reconnect started. Static IP applies when enabled.</div><a class='btn btn2' href='"); html += sessionUrl("/wifi"); html += F("'>Back to WiFi</a></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void handleForgetWifi() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  forgetWiFiManagerConfig();
  saveStaticIpConfig(false, String(wifiStaticIP), String(wifiStaticGateway), String(wifiStaticSubnet), String(wifiStaticDns));
  WiFi.disconnect(false);
  ensurePermanentAP();
  String html = htmlHeader("WiFi Deleted");
  html += F("<div class='card'><h1>WiFi Deleted</h1><div class='sub'>Saved network deleted. Local Mode AP/web is still active on 192.168.4.1 without restart.</div><a class='btn btn2' href='"); html += sessionUrl("/wifi"); html += F("'>Back to WiFi</a></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void sendRestartPage() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;

  String html = htmlHeader("Restarting");
  html += F("<div class='card'><h1>Restarting...</h1>");
  html += F("<div class='sub'>ESP01 will reboot now. This page will return to Home automatically.</div>");
  html += F("<div class='sub'>Please wait <span id='c'>12</span> seconds...</div>");
  html += F("<a class='btn btn2' href='/'>Back Home</a>");
  html += F("</div>");
  html += F("<script>");
  html += F("var n=12;setInterval(function(){n--;document.getElementById('c').innerHTML=n;if(n<=0){location.replace('/');}},1000);");
  html += F("</script>");
  html += htmlFooter();

  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.send(200, "text/html", html);
  delay(800);
  ESP.restart();
}



void handleSaveWebPort() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  uint16_t p = (uint16_t)webServer.arg("port").toInt();
  if (p < 1 || p > 65535) p = 80;
  saveWebPortConfig(p);

  String html = htmlHeader("Web Port Saved");
  html += F("<div class='card'><h1>Web Port Saved</h1>");
  html += F("<div class='sub'>New port saved: <b>");
  html += String(webServerPort);
  html += F("</b></div><div class='sub'>Restart required. After restart update router Internal Port if needed.</div>");
  html += F("<a class='btn' href='"); html += sessionUrl("/restart"); html += F("'>Restart Now</a>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/wifi"); html += F("'>Back</a></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void handleSaveDuckDns() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  saveDuckDnsConfig(webServer.arg("domain"), webServer.arg("token"));
  webServer.sendHeader("Location", "/wifi", true);
  webServer.send(302, "text/plain", "");
}

void handleClearDuckDns() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  clearDuckDnsConfig();
  webServer.sendHeader("Location", "/wifi", true);
  webServer.send(302, "text/plain", "");
}

void handleDuckDnsUpdateNow() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  updateDuckDNS(true);
  webServer.sendHeader("Location", "/wifi", true);
  webServer.send(302, "text/plain", "");
}

void handleSaveUpdateOptions() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  bool ghEnabled = webServer.hasArg("ghauto") && webServer.arg("ghauto") == "1";
  byte ghMode = (byte)webServer.arg("ghint").toInt();
  byte tsSec = (byte)webServer.arg("tscmd").toInt();
  saveWebPerformanceSettings(ghEnabled, ghMode, tsSec);
  webServer.sendHeader("Location", WEB_OTA_PATH, true);
  webServer.send(302, "text/plain", "");
}

void handleSaveThingSpeak() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  saveThingSpeakConfig(webServer.arg("channel"), webServer.arg("writekey"), webServer.arg("readkey"));
  if (webServer.hasArg("tscmd")) {
    saveWebPerformanceSettings(githubAutoUpdateEnabled, githubAutoUpdateIntervalMode, (byte)webServer.arg("tscmd").toInt());
  }
  webServer.sendHeader("Location", "/wifi", true);
  webServer.send(302, "text/plain", "");
}

void handleClearThingSpeak() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  clearThingSpeakConfig();
  webServer.sendHeader("Location", "/wifi", true);
  webServer.send(302, "text/plain", "");
}

void handleSaveDht() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  byte t = SENSOR_TYPE_DHT11;
  if (webServer.arg("type") == "22") t = SENSOR_TYPE_DHT22;
  else if (webServer.arg("type") == "18") t = SENSOR_TYPE_DS18B20;
  saveDhtConfig(t);
  readDhtSensorNow(true);
  webServer.sendHeader("Location", "/wifi", true);
  webServer.send(302, "text/plain", "");
}

void handleSaveDevice() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  saveDeviceConfig(webServer.arg("devname"), webServer.arg("apssid"), webServer.arg("appass"));
  String html = htmlHeader("Device Saved");
  html += F("<div class='card'><h1>Device Settings Saved</h1><div class='sub'>ESP01 will restart now to apply AP/hostname changes.</div></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
  delay(700);
  ESP.restart();
}

void handleFactoryReset() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  factoryResetConfig();
  String html = htmlHeader("Factory Reset");
  html += F("<div class='card'><h1>Factory Reset Done</h1><div class='sub'>All saved WiFi, ThingSpeak, and device settings were erased. ESP01 will restart.</div></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
  delay(900);
  ESP.restart();
}

String jsonStringValue(const String& payload, const String& key) {
  String token = String("\"") + key + String("\"");
  int k = payload.indexOf(token);
  if (k < 0) return String("");
  int colon = payload.indexOf(':', k);
  if (colon < 0) return String("");
  int q1 = payload.indexOf('\"', colon + 1);
  if (q1 < 0) return String("");
  int q2 = payload.indexOf('\"', q1 + 1);
  if (q2 < 0) return String("");
  return payload.substring(q1 + 1, q2);
}

long jsonLongValue(const String& payload, const String& key) {
  String token = String("\"") + key + String("\"");
  int k = payload.indexOf(token);
  if (k < 0) return -1;
  int colon = payload.indexOf(':', k);
  if (colon < 0) return -1;
  int end = payload.indexOf(',', colon + 1);
  if (end < 0) end = payload.indexOf('}', colon + 1);
  if (end < 0) return -1;
  String num = payload.substring(colon + 1, end);
  num.trim();
  return num.toInt();
}

bool runAutoUpdateCheck(bool manual) {
#if ENABLE_AUTO_UPDATE
  if (autoUpdateBusy) {
    lastAutoUpdateMessage = "Update check already running";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    lastAutoUpdateMessage = "No STA internet/WiFi connection";
    return false;
  }

  autoUpdateBusy = true;
  lastAutoUpdateMessage = "Checking GitHub...";
  DBG_PRINTLN(lastAutoUpdateMessage);

  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // Needed for GitHub HTTPS on ESP8266 without certificate bundle.
  secureClient.setTimeout(20000);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  if (!http.begin(secureClient, UPDATE_INFO_URL)) {
    lastAutoUpdateMessage = "Could not open version URL";
    autoUpdateBusy = false;
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    lastAutoUpdateMessage = String("Version check HTTP error: ") + httpCode;
    http.end();
    autoUpdateBusy = false;
    return false;
  }

  String payload = http.getString();
  http.end();
  secureClient.stop();
  delay(250);
  yield();

  long newBuild = jsonLongValue(payload, "build");
  String newVersion = jsonStringValue(payload, "version");
  String binUrl = jsonStringValue(payload, "bin");
  #if ENABLE_UPDATE_MD5
  String md5 = jsonStringValue(payload, "md5");      // Optional: add firmware MD5 in version.json for stronger safety.
#endif
#if ENABLE_UPDATE_NOTES
  String notes = jsonStringValue(payload, "notes");  // Optional: short changelog text.
#endif

  if (newBuild < 0 || binUrl.length() < 8) {
    lastAutoUpdateMessage = "Bad version.json format";
    autoUpdateBusy = false;
    return false;
  }

  if (newBuild <= FW_BUILD) {
    lastAutoUpdateMessage = String("Already latest: ") + FW_VERSION + " build " + FW_BUILD;
    lastAutoUpdateCheckMs = millis();
    DBG_PRINTLN(lastAutoUpdateMessage);
    autoUpdateBusy = false;
    return false;
  }

  lastAutoUpdateMessage = String("New firmware ") + newVersion + " build " + newBuild + ". Updating...";
#if ENABLE_UPDATE_NOTES
  if (notes.length()) lastAutoUpdateMessage += String(" Notes: ") + notes;
#endif
  DBG_PRINTLN(lastAutoUpdateMessage);

  //WiFiUDP::stopAll();
  delay(500);
  yield();
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
#if ENABLE_UPDATE_MD5
  if (md5.length() == 32) {
    httpUpdate.setMD5sum(md5.c_str());
  }
#endif

  t_httpUpdate_return ret;
  if (binUrl.startsWith("https://")) {
    WiFiClientSecure binClient;
    binClient.setInsecure();
    binClient.setTimeout(30000);
    ret = httpUpdate.update(binClient, binUrl);
  } else {
    WiFiClient binClient;
    binClient.setTimeout(30000);
    ret = httpUpdate.update(binClient, binUrl);
  }

  if (ret == HTTP_UPDATE_FAILED) {
    lastAutoUpdateMessage = String("Update failed [") + httpUpdate.getLastError() + "]: " + httpUpdate.getLastErrorString();
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    lastAutoUpdateMessage = "No update from server";
  } else if (ret == HTTP_UPDATE_OK) {
    lastAutoUpdateMessage = "Update OK. Rebooting...";
  }

  lastAutoUpdateCheckMs = millis();
  DBG_PRINTLN(lastAutoUpdateMessage);
  autoUpdateBusy = false;
  return (ret == HTTP_UPDATE_OK);
#else
  lastAutoUpdateMessage = "Auto update disabled";
  return false;
#endif
}

void handleAutoUpdateCheck() {
#if ENABLE_AUTO_UPDATE
  if (!githubAutoUpdateEnabled) return;
  if (autoUpdateBusy) return;
  if (WiFi.status() != WL_CONNECTED) return;

#if AUTO_UPDATE_ON_BOOT
  if (!autoUpdateChecked) {
    if (millis() < AUTO_UPDATE_BOOT_DELAY_MS) return;
    autoUpdateChecked = true;
    runAutoUpdateCheck(false);
    return;
  }
#endif

#if AUTO_UPDATE_EVERY_HOUR
  if (githubAutoUpdateIntervalMs == 0) return; // Manual only
  if (lastAutoUpdateCheckMs == 0) {
    lastAutoUpdateCheckMs = millis();
    return;
  }
  if (millis() - lastAutoUpdateCheckMs >= githubAutoUpdateIntervalMs) {
    runAutoUpdateCheck(false);
  }
#endif
#endif
}

void handleManualUpdateCheck() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  bool ok = runAutoUpdateCheck(true);
  String html = htmlHeader("Ahmed Device Update Check");
  html += F("<div class='card'><h1>Ahmed Device Update Check</h1>");
  html += F("<div class='sub'>Current firmware: <b>"); html += FW_FULL_NAME; html += F("</b> / build <b>"); html += String(FW_BUILD); html += F("</b></div>");
  html += F("<div class='msg "); html += ok ? F("ok") : F("warn"); html += F("'>"); html += lastAutoUpdateMessage; html += F("</div>");
  html += F("<div class='sub'>Protected by web login. Optional JSON fields supported: <b>md5</b> and <b>notes</b>. For rollback, point version.json to an older known-good firmware and use a higher build number.</div>");
  html += F("<a class='btn btn2' href='/'>Back</a></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void setupWebOTA() {
#if ENABLE_WEB_OTA
  if (webOtaStarted) return;
  DBG_PRINTLN("Starting Secure Web OTA + WiFi Manager server...");
  if (!webServerPtr) webServerPtr = new WebServer(webServerPort);
  const char* headerkeys[] = {"Cookie"};
webServer.collectHeaders(headerkeys, 1);

  webServer.on("/", HTTP_GET, sendHomePage);
  webServer.on("/logout", HTTP_GET, sendLogoutPage);
  webServer.on("/login", HTTP_GET, sendLoginPage);
  webServer.on("/dologin", HTTP_POST, handleDoLogin);
  webServer.on("/dashboard", HTTP_GET, sendDashboardPage);
  webServer.on("/restart", HTTP_GET, sendRestartPage);
  webServer.on("/backup", HTTP_GET, sendEepromBackup);
  webServer.on("/restore", HTTP_POST, handleRestoreBackupDone, handleRestoreBackupUpload);
  webServer.on("/savewebport", HTTP_POST, handleSaveWebPort);
  webServer.on(WEB_OTA_PATH, HTTP_GET, sendUpdatePage);
  webServer.on(WIFI_MANAGER_PATH, HTTP_GET, sendWiFiPage);
  webServer.on("/scan", HTTP_GET, sendScanJson);
  webServer.on("/status", HTTP_GET, sendStatusJson);
  webServer.on("/checkupdate", HTTP_GET, handleManualUpdateCheck);
  webServer.on("/setoutput", HTTP_POST, handleSetOutput);
  webServer.on("/savesmart", HTTP_POST, handleSaveSmartOutput);
  webServer.on("/saveweather", HTTP_POST, handleSaveWeatherCity);
  webServer.on("/users", HTTP_GET, sendUsersPage);
  webServer.on("/saveusers", HTTP_POST, handleSaveUsers);
  webServer.on("/weatherrefresh", HTTP_GET, handleWeatherRefresh);
  webServer.on("/savewifi", HTTP_POST, handleSaveWifi);
  webServer.on("/forgetwifi", HTTP_POST, handleForgetWifi);
  webServer.on("/saveupdateopts", HTTP_POST, handleSaveUpdateOptions);
  webServer.on("/savets", HTTP_POST, handleSaveThingSpeak);
  webServer.on("/clearts", HTTP_POST, handleClearThingSpeak);
  webServer.on("/saveduck", HTTP_POST, handleSaveDuckDns);
  webServer.on("/clearduck", HTTP_POST, handleClearDuckDns);
  webServer.on("/duckupdate", HTTP_GET, handleDuckDnsUpdateNow);
  webServer.on("/savedht", HTTP_POST, handleSaveDht);
  webServer.on("/savedevice", HTTP_POST, handleSaveDevice);
  webServer.on("/factoryreset", HTTP_POST, handleFactoryReset);

  webServer.on(WEB_OTA_PATH, HTTP_POST, []() {
    if (!requireRole(WEB_ROLE_ADMIN)) return;

    bool ok = !Update.hasError();
    if (ok) {
      webServer.send(200, "text/plain", "Update success. Rebooting...");
      delay(700);
      ESP.restart();
    } else {
      webServer.send(500, "text/plain", "Update failed. Check file type, board, and free OTA space.");
    }
  }, []() {
    if (!requireRole(WEB_ROLE_ADMIN)) return;

    HTTPUpload& upload = webServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
      DBG_PRINT("Web OTA upload start: ");
      DBG_PRINTLN(upload.filename);

      //WiFiUDP::stopAll();
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace, U_FLASH)) {
#if DEBUG_SERIAL
        Update.printError(Serial);
#endif
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
#if DEBUG_SERIAL
        Update.printError(Serial);
#endif
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        DBG_PRINT("Web OTA upload complete. Size: ");
        DBG_PRINTLN(upload.totalSize);
      } else {
#if DEBUG_SERIAL
        Update.printError(Serial);
#endif
      }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.end();
      DBG_PRINTLN("Web OTA upload aborted");
    }
    yield();
  });

  webServer.onNotFound([]() {
    if (!requireRole(WEB_ROLE_ADMIN)) return;
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
  });

  webServer.begin();
  webOtaStarted = true;

  DBG_PRINT("Secure Web OTA Ready: http://");
  DBG_PRINT(WiFi.localIP());
  DBG_PRINTLN("/update");
#endif
}

void handleWebOTA() {
#if ENABLE_WEB_OTA
  if (!webOtaStarted) setupWebOTA();
  webServer.handleClient();
#endif
}

// ========================= WIFI =========================

void startWiFiAttempt() {
  lastWiFiAttemptTime = millis();

  // V1.2.17 No Internet Boot Fix:
  // AP is always kept alive, and STA connection is started without waiting.
  // Never use while(WiFi.status()...) here, because that blocks the web page
  // when the saved router is missing or has no internet.
  ensurePermanentAP();

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false); // we control retry timing explicitly every 60 seconds
  WiFi.setSleep(false);

  if (WiFi.status() == WL_CONNECTED) {
    DBG_PRINTLN("STA already online. AP/web still active.");
    writeStatusLed(true);
    setupOTA();
    setupWebOTA();
    return;
  }

  if (activeSsid[0] == '\0') {
    DBG_PRINTLN("No saved WiFi -> permanent Local Mode AP/web only.");
    writeStatusLed(false);
    return;
  }

  DBG_PRINT("Non-blocking STA reconnect attempt: ");
  DBG_PRINTLN(activeSsid);
  WiFi.disconnect(false);   // drop stale STA state only; keep AP mode alive
  delay(1);
  applyStaticIpBeforeConnect();
  WiFi.begin(activeSsid, activePass);
  writeStatusLed(false);
}

void maintainWiFi() {
  ensurePermanentAP();

  if (WiFi.status() == WL_CONNECTED) {
    if (!printedOnlineThingSpeakMode) {
      DBG_PRINTLN("WiFi connected -> Online Mode restored. ThingSpeak / Weather / GitHub OTA active.");
      DBG_PRINTLN("AP/web remains active for local control.");
      printedOnlineThingSpeakMode = true;
      printedOfflineEepromMode = false;
      writeStatusLed(true);
      setupOTA();
      setupWebOTA();
    }
    return;
  }

  ntpStarted = false; // allow SNTP re-init when STA reconnects after Local Mode

  if (!printedOfflineEepromMode) {
    DBG_PRINTLN("WiFi disconnected -> Local Mode. AP/web/control remain active.");
    DBG_PRINTLN("Reconnect will be retried every 60 seconds without blocking the page.");
    DBG_PRINT("EEPROM/Current Output1 = ");
    DBG_PRINTLN(currentOutput1State ? "ON" : "OFF");
    DBG_PRINT("EEPROM/Current Output2 = ");
    DBG_PRINTLN(currentOutput2State ? "ON" : "OFF");
    printedOfflineEepromMode = true;
    printedOnlineThingSpeakMode = false;
  }

  updateStatusLed();

  if (millis() - lastWiFiAttemptTime >= WIFI_RETRY_INTERVAL) {
    startWiFiAttempt();
  }
}

// ========================= STATUS LED =========================

void updateStatusLed() {
#if ENABLE_STATUS_LED
  // V1.2.26: never let the WiFi status LED logic touch GPIO1 while schedule/manual/temp/humidity/timer owns Out3.
  if (smartCfg.mode != SMART_MODE_INTERNET_LED) return;
  if (WiFi.status() == WL_CONNECTED) {
    writeStatusLed(true);
    return;
  }

  if (millis() - lastLedBlinkTime >= 500UL) {
    lastLedBlinkTime = millis();
    ledBlinkState = !ledBlinkState;
    writeStatusLed(ledBlinkState);
  }
#endif
}

// ========================= THINGSPEAK =========================

long readThingSpeakField(byte field) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  if (!thingSpeakConfigured(false)) { DBG_PRINTLN("ThingSpeak read skipped: not configured"); return -1; }

  for (byte i = 0; i < 3; i++) {
    long value = ThingSpeak.readLongField(getThingSpeakChannelId(), field, tsReadKey);
    int status = ThingSpeak.getLastReadStatus();

    if (status == 200) {
      return value;
    }

    DBG_PRINT("ThingSpeak read failed, field ");
    DBG_PRINT(field);
    DBG_PRINT(", status: ");
    DBG_PRINTLN(status);

    delay(250);
    yield();
  }

  return -1;
}

void markOutputThingSpeakUpdatePending(const char* reason) {
  if (!pendingOutputThingSpeakUpdate) {
    pendingOutputThingSpeakUpdate = true;
    savePendingSyncFlag(true);
  }
  DBG_PRINT("Output ThingSpeak update pending: ");
  DBG_PRINTLN(reason);
  DBG_PRINTLN("Remote ThingSpeak command reading is blocked until this local state is uploaded successfully.");
}

bool readAndSendSensorData();
bool sendFullThingSpeakState(const char* reason, bool respectSpacing, bool markPendingOnFail);

bool uploadOutputStatesOnly(const char* reason) {
  // V1.2.21: never send output fields alone.
  // Any lamp/pump/timer/local command now uploads the complete channel state
  // so mobile apps do not see missing/empty fields.
  return sendFullThingSpeakState(reason, true, true);
}

void processPendingOutputThingSpeakUpdate() {
  if (!pendingOutputThingSpeakUpdate) return;
  if (millis() - lastOutputUpdateAttemptTime < OUTPUT_UPLOAD_RETRY_MS) return;

  lastOutputUpdateAttemptTime = millis();
  uploadOutputStatesOnly("Pending output update retry");
}

void setAllOutputsFromButton(byte newState) {
  DBG_PRINTLN("==== MANUAL LONG PRESS ACTION ====");
  DBG_PRINT("New all-outputs state: ");
  DBG_PRINTLN(newState ? "ALL ON" : "ALL OFF");

  currentOutput1State = newState;
  currentOutput2State = newState;
  applyOutputs();
  saveOutputState(1, currentOutput1State);
  saveOutputState(2, currentOutput2State);

  markOutputThingSpeakUpdatePending("Manual 3-second button toggle");
  uploadOutputStatesOnly("Manual 3-second button toggle");
  DBG_PRINTLN("==================================");
}

void handleDhtPinButton() {
#if ENABLE_DHT_PIN_BUTTON
  bool pressed = (digitalRead(DHTPIN) == LOW);

  if (pressed) {
    if (buttonPressStartTime == 0) {
      buttonPressStartTime = millis();
      buttonLongPressHandled = false;
      DBG_PRINTLN("DHT pin button pressed - hold 3 seconds");
    }

    if (!buttonLongPressHandled && millis() - buttonPressStartTime >= BUTTON_LONG_PRESS_MS) {
      byte newState = (currentOutput1State == 1 && currentOutput2State == 1) ? 0 : 1;
      setAllOutputsFromButton(newState);
      buttonLongPressHandled = true;
    }
  } else {
    if (buttonPressStartTime != 0) {
      if (!buttonLongPressHandled) {
        DBG_PRINTLN("DHT pin button released before 3 seconds -> ignored");
      } else {
        DBG_PRINTLN("DHT pin button released after long-press action");
      }
    }
    buttonPressStartTime = 0;
    buttonLongPressHandled = false;
  }
#endif
}

void setOutputState(byte outputNumber, byte newState) {
  if (!isValidState(newState)) return;

  if (outputNumber == 1) {
    if (newState == currentOutput1State) {
      DBG_PRINTLN("Output1 command received but state is unchanged -> no EEPROM write");
      return;
    }
    DBG_PRINTLN("ThingSpeak command changed Output1 -> applying and saving EEPROM");
    DBG_PRINT("Old Output1 = ");
    DBG_PRINTLN(currentOutput1State);
    DBG_PRINT("New Output1 = ");
    DBG_PRINTLN(newState);
    currentOutput1State = newState;
    setRelay(OUTPUT1_PIN, currentOutput1State);
    saveOutputState(1, currentOutput1State);
    DBG_PRINT("Output1 GPIO");
    DBG_PRINT(OUTPUT1_PIN);
    DBG_PRINT(": ");
    DBG_PRINTLN(currentOutput1State ? "ON" : "OFF");
  } else if (outputNumber == 2) {
    if (newState == currentOutput2State) {
      DBG_PRINTLN("Output2 command received but state is unchanged -> no EEPROM write");
      return;
    }
    DBG_PRINTLN("ThingSpeak command changed Output2 -> applying and saving EEPROM");
    DBG_PRINT("Old Output2 = ");
    DBG_PRINTLN(currentOutput2State);
    DBG_PRINT("New Output2 = ");
    DBG_PRINTLN(newState);
    currentOutput2State = newState;
    setRelay(OUTPUT2_PIN, currentOutput2State);
    saveOutputState(2, currentOutput2State);
    DBG_PRINT("Output2 GPIO");
    DBG_PRINT(OUTPUT2_PIN);
    DBG_PRINT(": ");
    DBG_PRINTLN(currentOutput2State ? "ON" : "OFF");
  }
}

void checkRemoteCommands() {
  if (pendingOutputThingSpeakUpdate) {
    DBG_PRINTLN("Skip remote command check: local button change is pending ThingSpeak sync");
    DBG_PRINTLN("Priority rule: upload local EEPROM/output state first, then read ThingSpeak commands.");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    DBG_PRINTLN("Skip command check: WiFi disconnected");
    return;
  }

  DBG_PRINTLN("Checking remote commands from ThingSpeak...");

  long output1State = readThingSpeakField(FIELD1_OUTPUT1);
  long output2State = readThingSpeakField(FIELD2_OUTPUT2);

  DBG_PRINT("ThingSpeak Field1 Output1 read = ");
  DBG_PRINTLN(output1State);
  DBG_PRINT("ThingSpeak Field2 Output2 read = ");
  DBG_PRINTLN(output2State);

  if (!isValidState(output1State)) {
    DBG_PRINTLN("Invalid or failed Output1 command. Keeping last state.");
  } else {
    setOutputState(1, (byte)output1State);
  }

  if (!isValidState(output2State)) {
    DBG_PRINTLN("Invalid or failed Output2 command. Keeping last state.");
  } else {
    setOutputState(2, (byte)output2State);
  }
}


int parsePublicIPLastOctet(const String& ip) {
  int p = ip.lastIndexOf('.');
  if (p < 0) return 0;
  int v = ip.substring(p + 1).toInt();
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return v;
}

void updatePublicIP(bool force) {
#if ENABLE_PUBLIC_IP
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (!force && lastPublicIPCheckMs != 0 && now - lastPublicIPCheckMs < PUBLIC_IP_INTERVAL_MS) return;
  lastPublicIPCheckMs = now;

  WiFiClient ipClient;
  HTTPClient http;
  http.setTimeout(4000);
  if (!http.begin(ipClient, PUBLIC_IP_URL)) {
    publicIPOk = false;
    lastPublicIPMessage = "IP begin failed";
    return;
  }
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String ip = http.getString();
    ip.trim();
    if (isValidPublicIP(ip)) {
      publicIP = ip;
      publicIPLastOctet = parsePublicIPLastOctet(publicIP);
      publicIPOk = true;
      lastPublicIPMessage = "IP OK";
    } else {
      publicIPOk = false;
      lastPublicIPMessage = "Bad IP";
    }
  } else {
    publicIPOk = false;
    lastPublicIPMessage = String("HTTP ") + code;
  }
  http.end();
#endif
}

bool isValidPublicIP(String ip) {
  ip.trim();
  if (ip.length() < 7 || ip.length() > 15) return false;
  byte dots = 0;
  for (unsigned int i = 0; i < ip.length(); i++) {
    char c = ip.charAt(i);
    if (c == '.') dots++;
    else if (c < '0' || c > '9') return false;
  }
  return dots == 3;
}

String normalizeIPText(String ip) {
  ip.trim();
  ip.replace("\r", "");
  ip.replace("\n", "");
  ip.trim();
  return ip;
}

String readThingSpeakFieldTextPublic(byte field) {
  if (WiFi.status() != WL_CONNECTED) return "";
  if (getThingSpeakChannelId() == 0) return "";

  String url = String("http://api.thingspeak.com/channels/") + String(getThingSpeakChannelId()) +
               String("/fields/") + String(field) + String("/last.txt");

  // Public channel should not need a read key, but keep a fallback if a key exists.
  if (tsReadKey[0] != '\0') {
    // Public URL is tried first by using the plain endpoint above.
  }

  WiFiClient tsClient;
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(tsClient, url)) return "";

  int code = http.GET();
  String value = "";
  if (code == HTTP_CODE_OK) {
    value = http.getString();
    value = normalizeIPText(value);
  }
  http.end();

  // Optional fallback for private channel or temporary public read failure.
  if (value.length() == 0 && tsReadKey[0] != '\0') {
    url += String("?api_key=") + String(tsReadKey);
    if (http.begin(tsClient, url)) {
      code = http.GET();
      if (code == HTTP_CODE_OK) {
        value = http.getString();
        value = normalizeIPText(value);
      }
      http.end();
    }
  }

  return value;
}

void checkPublicIPAgainstThingSpeak(bool force) {
#if ENABLE_PUBLIC_IP
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (!force) {
    if (!publicIPBootCompareDone) {
      if (now < PUBLIC_IP_BOOT_DELAY_MS) return;
    } else if (lastPublicIPCompareMs != 0 && now - lastPublicIPCompareMs < PUBLIC_IP_INTERVAL_MS) {
      return;
    }
  }

  lastPublicIPCompareMs = now;
  publicIPBootCompareDone = true;

  updatePublicIP(true);
  if (!publicIPOk || !isValidPublicIP(publicIP)) {
    lastPublicIPMessage = "Current IP invalid";
    return;
  }

  String savedIP = readThingSpeakFieldTextPublic(FIELD5_LOGIN_USER);
  savedIP = normalizeIPText(savedIP);
  lastThingSpeakPublicIP = savedIP;

  if (savedIP == publicIP) {
    lastPublicIPMessage = "IP unchanged";
    return;
  }

  if (lastThingSpeakWriteTime != 0 && now - lastThingSpeakWriteTime < THINGSPEAK_MIN_WRITE_MS) {
    lastPublicIPMessage = "IP changed; wait TS spacing";
    return;
  }

  lastPublicIPMessage = String("IP changed: ") + (savedIP.length() ? savedIP : String("empty")) + String(" -> ") + publicIP;
  readAndSendSensorData(); // Sends all fields; field5 is logged user in V1.2.53.
#endif
}

bool duckDnsConfigured() {
#if ENABLE_DUCKDNS
  return (duckDnsDomain[0] != '\0' && duckDnsToken[0] != '\0');
#else
  return false;
#endif
}

void updateDuckDNS(bool force) {
#if ENABLE_DUCKDNS
  if (WiFi.status() != WL_CONNECTED) return;

  if (!duckDnsConfigured()) {
    lastDuckDNSMessage = "DuckDNS skipped: add domain/token";
    return;
  }

  unsigned long now = millis();
  if (!force) {
    if (lastDuckDNSUpdateMs == 0) {
      if (now < DUCKDNS_BOOT_DELAY_MS) return;
    } else if (now - lastDuckDNSUpdateMs < DUCKDNS_INTERVAL_MS) {
      return;
    }
  }

  // Make sure publicIP is fresh enough before sending it to DuckDNS.
  updatePublicIP(false);

  String url = String("http://www.duckdns.org/update?domains=") + String(duckDnsDomain) +
               String("&token=") + String(duckDnsToken) + String("&ip=");
  if (publicIPOk && isValidPublicIP(publicIP)) {
    url += publicIP;
  }

  WiFiClient duckClient;
  HTTPClient http;
  http.setTimeout(5000);

  if (!http.begin(duckClient, url)) {
    lastDuckDNSMessage = "DuckDNS begin failed";
    return;
  }

  int code = http.GET();
  String payload = "";
  if (code > 0) {
    payload = http.getString();
    payload.trim();
  }
  http.end();

  lastDuckDNSUpdateMs = millis();

  if (code == HTTP_CODE_OK && payload == "OK") {
    lastDuckDNSMessage = String("DuckDNS OK: ") + String(duckDnsDomain) + String(".duckdns.org -> ") + publicIP;
  } else {
    lastDuckDNSMessage = String("DuckDNS failed HTTP ") + String(code) + String(" ") + payload;
  }
#endif
}

bool sendFullThingSpeakState(const char* reason, bool respectSpacing, bool markPendingOnFail) {
  if (!thingSpeakConfigured(true)) {
    DBG_PRINTLN("ThingSpeak full-state upload skipped: not configured");
    if (markPendingOnFail) markOutputThingSpeakUpdatePending(reason);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    DBG_PRINTLN("Skip ThingSpeak full-state upload: WiFi disconnected");
    if (markPendingOnFail) markOutputThingSpeakUpdatePending(reason);
    return false;
  }

  unsigned long now = millis();
  if (respectSpacing && lastThingSpeakWriteTime != 0 && now - lastThingSpeakWriteTime < THINGSPEAK_MIN_WRITE_MS) {
    DBG_PRINTLN("ThingSpeak 20-second spacing not ready -> full-state update kept pending");
    if (markPendingOnFail) markOutputThingSpeakUpdatePending(reason);
    return false;
  }

  bool sensorOk = readDhtSensorNow(false);
  applySmartOutputControl();
  float temp = currentTemperature;
  float hum = currentHumidity;

  if (!sensorOk || isnan(temp)) {
    DBG_PRINTLN("Sensor final failed -> sending 99 temperature fallback");
    temp = 99.0;
  }

  if (dhtSensorType == SENSOR_TYPE_DS18B20) {
    hum = 0.0;  // DS18B20 has no humidity. ThingSpeak field remains numeric.
  } else if (isnan(hum)) {
    DBG_PRINTLN("Humidity invalid -> sending 99 humidity fallback");
    hum = 99.0;
  }

  DBG_PRINTLN("==== THINGSPEAK FULL STATE UPDATE ====");
  DBG_PRINT("Reason: ");
  DBG_PRINTLN(reason);
  DBG_PRINT("Temperature: ");
  DBG_PRINT(temp);
  DBG_PRINT(" C, Humidity: ");
  DBG_PRINT(hum);
  DBG_PRINTLN(dhtSensorType == SENSOR_TYPE_DS18B20 ? " (DS18B20 no humidity)" : " %");

  ThingSpeak.setField(FIELD1_OUTPUT1, currentOutput1State);
  ThingSpeak.setField(FIELD2_OUTPUT2, currentOutput2State);
  ThingSpeak.setField(FIELD3_TEMP, temp);
  ThingSpeak.setField(FIELD4_HUM, hum);
  String tsUser = thingSpeakLoginUserText();
  ThingSpeak.setField(FIELD5_LOGIN_USER, tsUser);
  ThingSpeak.setStatus(String("User: ") + tsUser);
  ThingSpeak.setField(FIELD6_TEMP, temp);
  ThingSpeak.setField(FIELD7_OUTPUT1, currentOutput1State);
  ThingSpeak.setField(FIELD8_OUTPUT2, currentOutput2State);

  int status = ThingSpeak.writeFields(getThingSpeakChannelId(), tsWriteKey);
  if (status == 200) {
    lastThingSpeakWriteTime = millis();
    if (pendingOutputThingSpeakUpdate) {
      DBG_PRINTLN("Pending output update satisfied by normal sensor upload");
      pendingOutputThingSpeakUpdate = false;
      savePendingSyncFlag(false);
      DBG_PRINTLN("Pending flag cleared after confirmed sensor upload success");
    }
    DBG_PRINTLN("ThingSpeak full-state upload OK");
    DBG_PRINTLN("=====================================");
    return true;
  }

  DBG_PRINT("ThingSpeak full-state upload failed. Status: ");
  DBG_PRINTLN(status);
  if (markPendingOnFail) markOutputThingSpeakUpdatePending(reason);
  DBG_PRINTLN("=====================================");
  return false;
}

bool readAndSendSensorData() {
  return sendFullThingSpeakState("Periodic sensor/full-state upload", true, false);
}

// ========================= ARDUINO =========================

void setup() {
  DBG_BEGIN(115200);
  delay(100);
  DBG_PRINTLN();
  DBG_PRINTLN(FW_FULL_NAME);
  DBG_PRINTLN("Mode: DEBUG SERIAL ON / INTERNET LED OFF");
  DBG_PRINTLN("After testing, set DEBUG_MODE to 0 to disable Serial and enable TX internet LED.");

  DBG_PRINTLN("Starting EEPROM...");
  EEPROM.begin(EEPROM_SIZE);
  loadWebPortConfig();
  loadWebUsersConfig();
  loadOutputStates();
  loadPendingSyncFlag();
  loadWiFiManagerConfig();
  loadStaticIpConfig();
  loadThingSpeakConfig();
  loadWebPerformanceSettings();
  loadDuckDnsConfig();
  loadDhtConfig();
  loadDeviceConfig();
  loadSmartOutputConfig();
  loadEgyptTimeMode();
  loadOfflineClock();
  loadCityWeatherConfig();
  debugDumpEepromBytes("After EEPROM load at boot");
  debugPrintRuntimeState("After EEPROM load, before applying outputs");

  DBG_PRINTLN("Configuring outputs...");
  pinMode(OUTPUT1_PIN, OUTPUT);
  pinMode(OUTPUT2_PIN, OUTPUT);
#if ENABLE_STATUS_LED
  pinMode(STATUS_LED_PIN, OUTPUT);
  writeStatusLed(false);
#endif

  applyOutputs();
  applySmartOutputControl(); // V1.2.27: immediately apply the saved Out3 owner/state at boot.
  debugPrintRuntimeState("After applyOutputs() in setup");

  DBG_PRINT("Starting sensor: " );
  DBG_PRINTLN(dhtTypeText());
  beginSelectedSensor();
  readDhtSensorNow(true);
#if ENABLE_DHT_PIN_BUTTON
  pinMode(DHTPIN, INPUT_PULLUP);
  DBG_PRINTLN("DHT pin long-press button enabled on GPIO2 with pull-up. Button must go to GND.");
#endif
  DBG_PRINTLN("Starting ThingSpeak client...");
  ThingSpeak.begin(client);

  // V1.2.17: start AP and web first, before any STA/internet work.
  // This guarantees 192.168.4.1 remains usable even if the saved WiFi is absent.
  WiFi.mode(WIFI_AP_STA);
  startConfigAP();
  setupWebOTA();
  startWiFiAttempt();
  setupOTA();

  // Run the first command check shortly after boot, then normal intervals.
  lastCommandCheckTime = millis() - commandCheckIntervalMs + 3000UL;
  lastSensorUpdateTime = millis() - SENSOR_UPDATE_INTERVAL + 5000UL;

  DBG_PRINTLN("Setup complete. EEPROM debug is active.");
}

void loop() {
  yield();
  handleOTA();
  handleWebOTA();
  maintainWiFi();
  startNtpIfNeeded();
  maintainOfflineClock();
  handleAutoUpdateCheck();
  // V1.2.19 Web Speed Fix: external weather is manual only to keep ESP-01 web responsive.
  // Use the Refresh Weather button when needed; no automatic HTTPS weather call in loop.
  // Public/Application IP sync is disabled in V1.2.53.
  updateDuckDNS(false);
  updateStatusLed();
  updateWebStatusSensorCache();
  applySmartOutputControl();
  handleDhtPinButton();
  processPendingOutputThingSpeakUpdate();

  unsigned long now = millis();

  if (now - lastCommandCheckTime >= commandCheckIntervalMs) {
    lastCommandCheckTime = now;
    checkRemoteCommands();
  }

  if (now - lastSensorUpdateTime >= SENSOR_UPDATE_INTERVAL) {
    if (readAndSendSensorData()) {
      lastSensorUpdateTime = now;
    } else {
      // Retry sooner after failure, without blocking or restarting.
      lastSensorUpdateTime = now - SENSOR_UPDATE_INTERVAL + 10000UL;
    }
  }
}



