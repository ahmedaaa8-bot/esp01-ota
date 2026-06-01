
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
#include <esp_wifi.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <ThingSpeak.h>
#include <EEPROM.h>
// ArduinoOTA removed in V1.1 PRO to save ESP01 flash. USB/Web OTA/GitHub OTA still work.
#include <WebServer.h>
#include <Update.h>
#include <esp_system.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <DNSServer.h>
#include <WebSocketsServer.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <SPIFFS.h>

/*
  Project: ESP32 ThingSpeak DHT/DS18B20 + 4 Outputs + 2 Inputs Controller
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
#define FW_VERSION         "1.7.1"
#define FW_BUILD           171
#define FW_NAME            "Ahmed Monem Smart Control System"
#define FW_DEVICE_NAME     "Weather and Control "
#define FW_FULL_NAME       "Ahmed Control System +201004608852 . V1.7.1 Build 171 Web Arabic + DS3231 RTC + Persistent Storage Rescue + WiFi Low-Level + Strict Static IP + User Page Storage + Telegram Language Force Fix + Auto Backup + Activation Codes Storage Fix By Ahmed Monem"
#define ESP32_WS_INTERNAL_PORT 81
#define ENABLE_LEGACY_OUTPUT_TIMERS 0  // 0 = hide/disable old runtime timers; schedules remain active.

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

// Scheduled safety restart. ESP32 will restart automatically after 24 hours of uptime.
#define ENABLE_AUTO_RESTART_24H 1
const unsigned long AUTO_RESTART_INTERVAL_MS = 86400000UL; // 24 hours
unsigned long autoRestartBootMs = 0;
bool autoRestart24hFired = false;
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

// Monthly user subscription control.
// At the first detected run of each new month, all non-admin users are disabled once.
// Admin stays enabled so you can re-enable paid users from User Management.
#define ENABLE_MONTHLY_USER_SUBSCRIPTION_DISABLE 1
#define SUBSCRIPTION_WARNING_DAYS 5
uint32_t lastSubscriptionDisableYm = 0;
unsigned long lastSubscriptionMaintenanceMs = 0;
bool monthlyUserAutoDisableEnabled = (ENABLE_MONTHLY_USER_SUBSCRIPTION_DISABLE != 0);

// Login page intro message. Admin can edit it from User Management / Subscription card.
#define LOGIN_INTRO_MAX_LEN 1500
bool loginIntroEnabled = false;
String loginIntroText = "";
Preferences loginIntroPrefs;

// One-time general activation codes for monthly subscription renewal.
// Admin can generate a pool of codes. Any disabled non-admin user can consume one code once from Web or Telegram.
// Used codes keep audit info: who used the code, when, and from Web/Telegram.
#define ACTIVATION_CODE_COUNT 30
#define ACTIVATION_CODE_LEN   8
#define ACTIVATION_USED_AT_MAX_LEN 24
#define ACTIVATION_METHOD_MAX_LEN 10
char activationCodes[ACTIVATION_CODE_COUNT][ACTIVATION_CODE_LEN + 1];
byte activationCodeUsed[ACTIVATION_CODE_COUNT];
char activationCodeUsedBy[ACTIVATION_CODE_COUNT][WEB_USER_MAX_LEN + 1];
char activationCodeUsedAt[ACTIVATION_CODE_COUNT][ACTIVATION_USED_AT_MAX_LEN + 1];
char activationCodeMethod[ACTIVATION_CODE_COUNT][ACTIVATION_METHOD_MAX_LEN + 1];
char activationCodeCreatedAt[ACTIVATION_CODE_COUNT][ACTIVATION_USED_AT_MAX_LEN + 1];
uint32_t activationCodeCreatedYm[ACTIVATION_CODE_COUNT];
bool activationCodesExpireEndMonth = false;
Preferences activationCodePrefs;

#define ACTIVATION_LOG_COUNT 10
char activationLogCode[ACTIVATION_LOG_COUNT][ACTIVATION_CODE_LEN + 1];
char activationLogUser[ACTIVATION_LOG_COUNT][WEB_USER_MAX_LEN + 1];
char activationLogAt[ACTIVATION_LOG_COUNT][ACTIVATION_USED_AT_MAX_LEN + 1];
char activationLogMethod[ACTIVATION_LOG_COUNT][ACTIVATION_METHOD_MAX_LEN + 1];
Preferences activationLogPrefs;

// V1.7.1: Activation codes still use the same generation/consumption logic,
// but they now also have a SPIFFS rescue store because Preferences/NVS was not
// covered by the persistent storage rescue layer.
#define ACTIVATION_CODE_STORE_PATH    "/act_codes_v171.bin"
#define ACTIVATION_CODE_STORE_MAGIC   0x41435432UL  // ACT2
#define ACTIVATION_CODE_STORE_VERSION 1
struct ActivationCodeStoreRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t checksum;
  char codes[ACTIVATION_CODE_COUNT][ACTIVATION_CODE_LEN + 1];
  byte used[ACTIVATION_CODE_COUNT];
  char usedBy[ACTIVATION_CODE_COUNT][WEB_USER_MAX_LEN + 1];
  char usedAt[ACTIVATION_CODE_COUNT][ACTIVATION_USED_AT_MAX_LEN + 1];
  char method[ACTIVATION_CODE_COUNT][ACTIVATION_METHOD_MAX_LEN + 1];
  char createdAt[ACTIVATION_CODE_COUNT][ACTIVATION_USED_AT_MAX_LEN + 1];
  uint32_t createdYm[ACTIVATION_CODE_COUNT];
  byte expireEndMonth;
};

bool activationCodeStoreLoadedAtBoot = false;
bool activationCodeStoreLastSaveOk = false;
uint32_t activationCodeStoreSaveCount = 0;
char activationCodeStoreLastError[36] = "none";

#define ACTIVATION_MAX_WRONG_ATTEMPTS 5
#define ACTIVATION_LOCK_MS 600000UL
#define ACTIVATION_ATTEMPT_SLOT_COUNT 40
#define ACTIVATION_ATTEMPT_KEY_MAX_LEN 32
struct ActivationAttemptSlot { char key[ACTIVATION_ATTEMPT_KEY_MAX_LEN + 1]; byte fails; unsigned long lockUntilMs; unsigned long lastSeenMs; };
ActivationAttemptSlot activationAttempts[ACTIVATION_ATTEMPT_SLOT_COUNT];

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
  byte lang; // 0 unknown, 1 Arabic, 2 English. Session-level fallback for web language.
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
#define DHTPIN             4     // ESP32: DHT/DS18B20 data pin. Use 10k pull-up to 3.3V.
#define DHT_TYPE_SELECT    0     // 0 = DHT11 / 1 = DHT22

#if DHT_TYPE_SELECT
  #define DHTTYPE          DHT22
#else
  #define DHTTYPE          DHT11
#endif
#define OUTPUT1_PIN        26    // ESP32 safe output pin: Pump relay
#define OUTPUT2_PIN        27    // ESP32 safe output pin: Lamp relay
#define OUTPUT5_PIN        14    // Extra output synced with ThingSpeak Field 5
#define OUTPUT6_PIN        13    // Extra output synced with ThingSpeak Field 6
#define INPUT7_PIN         32    // Digital input uploaded to ThingSpeak Field 7
#define INPUT8_PIN         33    // Digital input uploaded to ThingSpeak Field 8
#define INPUT9_PIN         16    // Digital input local only Field 9, NOT uploaded to ThingSpeak
#define INPUT10_PIN        17    // Digital input local only Field 10, NOT uploaded to ThingSpeak
#define STATUS_LED_PIN     25    // ESP32 safe output pin: Smart Output 3 / status relay
#define INTERNET_LED_PIN   18    // Dedicated internet/WiFi LED, independent from Output 3
#define INTERNET_LED_ACTIVE_HIGH 1 // 1 = LED ON when GPIO18 is HIGH. Change to 0 for active-low LED module.

#define RELAY_ACTIVE_HIGH  0     // 0 = Active LOW: ON=LOW, OFF=HIGH. 1 = Active HIGH: ON=HIGH, OFF=LOW.
#define DIGITAL_INPUT_ACTIVE_LOW 1 // 1 = input ON when pin is LOW to GND with INPUT_PULLUP.

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
#define FIELD5_OUTPUT5 5   // Extra output on GPIO14
#define FIELD6_OUTPUT6 6   // Extra output on GPIO13
#define FIELD7_INPUT7  7   // Digital input on GPIO32, uploaded to ThingSpeak
#define FIELD8_INPUT8  8   // Digital input on GPIO33, uploaded to ThingSpeak
#define FIELD9_INPUT9  9   // Digital input on GPIO16, local/WebSocket/Telegram only
#define FIELD10_INPUT10 10 // Digital input on GPIO17, local/WebSocket/Telegram only

// Timing Configuration
const unsigned long SENSOR_UPDATE_INTERVAL  = 60000UL; // 60 seconds
const unsigned long DEFAULT_COMMAND_CHECK_INTERVAL  = 40000UL; // Stability: default 40 seconds; runtime value is configurable from web
const unsigned long WIFI_RETRY_INTERVAL     = 60000UL; // V1.2.17: 60 seconds smart reconnect without blocking web
const unsigned long WIFI_CONNECT_TIMEOUT    = 8000UL;  // Kept for compatibility; V1.2.17 WiFi connect is non-blocking
const unsigned long DHT_MIN_READ_INTERVAL   = 2500UL;  // Safe spacing for DHT11/DHT22
const byte DHT_READ_RETRIES                 = 2;       // Stability: fewer blocking DHT retries
const unsigned long DHT_RETRY_DELAY_MS      = 100UL;   // Stability: shorter delay between DHT retries
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
#define EEPROM_BACKUP_MAP_VERSION 1254

// V1.2.41 EEPROM CLEAN MAP NOTE:
// EEPROM_SIZE increased to 2048 bytes to remove old overlaps.
// Important: after flashing this version, do Factory Reset once, then re-enter settings.
// Fixed overlaps:
// - Weather moved away from Smart Output block.
// - DuckDNS/WebPort moved away from Users block.
// - Clock/TZ/Static IP moved to a safe area after DuckDNS.

#define EEPROM_SIZE          4096
#define EEPROM_OUTPUT1_ADDR  0
#define EEPROM_OUTPUT2_ADDR  1
#define EEPROM_PENDING_ADDR  2
#define EEPROM_OUTPUT5_ADDR  3
#define EEPROM_OUTPUT6_ADDR  4
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

// ESP32 Integration Settings EEPROM Configuration (V1.1.1 ESP32 EEPROM)
// Stores MQTT, Telegram and sensor calibration settings permanently.
// Safe EEPROM area: 1600..1955, after WebPerf/Static-IP blocks and before EEPROM_SIZE=2048 end.
#define EEPROM_ESP32CFG_MAGIC_ADDR        1600
#define EEPROM_ESP32CFG_MQTT_ENABLE_ADDR  1601
#define EEPROM_ESP32CFG_TG_ENABLE_ADDR    1602
#define EEPROM_ESP32CFG_HOST_ADDR         1604  // 64 bytes
#define EEPROM_ESP32CFG_PORT_ADDR         1668  // uint16_t, 2 bytes
#define EEPROM_ESP32CFG_USER_ADDR         1670  // 32 bytes
#define EEPROM_ESP32CFG_PASS_ADDR         1702  // 32 bytes
#define EEPROM_ESP32CFG_BASE_ADDR         1734  // 64 bytes
#define EEPROM_ESP32CFG_DISC_ADDR         1798  // 32 bytes
#define EEPROM_ESP32CFG_TGBOT_ADDR        1830  // 80 bytes
#define EEPROM_ESP32CFG_TGCHAT_ADDR       1910  // 32 bytes
#define EEPROM_ESP32CFG_TGHIGH_ADDR       1944  // float, 4 bytes
#define EEPROM_ESP32CFG_TEMPCAL_ADDR      1948  // float, 4 bytes
#define EEPROM_ESP32CFG_HUMCAL_ADDR       1952  // float, 4 bytes
#define EEPROM_ESP32CFG_WS_PUBLIC_PORT_ADDR 1956 // uint16_t, 2 bytes. External router port that forwards to ESP32 WebSocket internal port 81.
#define EEPROM_ESP32CFG_WS_MAGIC_ADDR     1958  // byte magic for WebSocket external port setting.
#define EEPROM_ESP32CFG_WS_MAGIC          0x5C
#define EEPROM_ESP32CFG_MAGIC             0x3A

// Field / Input / Output label names EEPROM configuration (Admin only)
// Stored after the old 2048-byte map. EEPROM_SIZE is now 4096 for safe expansion.
#define FIELD_LABEL_MAX_LEN                 20
#define FIELD_LABEL_COUNT                   10
#define EEPROM_FIELD_LABEL_MAGIC_ADDR       2000
#define EEPROM_FIELD_LABEL_DATA_ADDR        2004
#define EEPROM_FIELD_LABEL_SLOT_SIZE        (FIELD_LABEL_MAX_LEN + 1)
#define EEPROM_FIELD_LABEL_MAGIC            0x4C

// Per-user field visibility/control permissions (Admin editable).
// Stored separately from user/password records so old user data stays compatible.
#define EEPROM_USERPERM_MAGIC_ADDR           2300
#define EEPROM_USERPERM_DATA_ADDR            2304
#define EEPROM_USERPERM_RECORD_SIZE          4
#define EEPROM_USERPERM_MAGIC                0xA7
#define USER_FIELD_VISIBLE_DEFAULT_MASK      0x03F3  // Fields 1,2,5,6,7,8,9,10
#define USER_FIELD_CONTROL_DEFAULT_MASK      0x0033  // Outputs 1,2,5,6
#define USER_FIELD_OUTPUT_MASK               0x0033

// Per-user Telegram notification settings (Admin editable).
// Stored in Preferences/NVS so old EEPROM user/password records stay compatible.
#define USER_TELEGRAM_CHAT_ID_MAX_LEN        31
#define USER_TELEGRAM_NVS_MAGIC              0x6B

// Output automation for outputs 1,2,5,6. Output 3 keeps its existing Smart Output page/config.
#define OUTPUT_AUTO_COUNT                    4
#define EEPROM_OUTPUT_AUTO_MAGIC_ADDR        2500
#define EEPROM_OUTPUT_AUTO_DATA_ADDR         2504
#define EEPROM_OUTPUT_AUTO_MAGIC             0xB4
#define EEPROM_OUTPUT_AUTO_SYNC_MAGIC_ADDR   2950
#define EEPROM_OUTPUT_AUTO_SYNC_DATA_ADDR    2951
#define EEPROM_OUTPUT_AUTO_SYNC_MAGIC        0x5E

// Persistent health diagnostics.
// Saved only on important events / throttled to protect EEPROM wear.
#define EEPROM_DIAG_MAGIC_ADDR               3000
#define EEPROM_DIAG_DATA_ADDR                3004
#define EEPROM_DIAG_MAGIC                    0xD9
#define EEPROM_DIAG_VERSION                  1
#define EEPROM_DIAG_SAVE_MIN_GAP_MS          60000UL

// Manual Auto-OFF options are stored in a dedicated EEPROM block.
// This is the primary source of truth; NVS Preferences are kept only as a compatibility mirror.
#define EEPROM_MANUAL_AUTO_MAGIC_ADDR        3300
#define EEPROM_MANUAL_AUTO_DATA_ADDR         3304
#define EEPROM_MANUAL_AUTO_MAGIC             0xAF
#define EEPROM_MANUAL_AUTO_VERSION           1

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

// DS3231 RTC backup clock.
// Wiring: VCC 3.3V, GND, SDA -> GPIO21, SCL -> GPIO22. Address is normally 0x68.
// Time source priority: NTP -> DS3231 RTC -> EEPROM saved clock + millis().
#define ENABLE_DS3231_RTC 1
#define RTC_SDA_PIN       21
#define RTC_SCL_PIN       22
#define DS3231_I2C_ADDRESS 0x68
const unsigned long RTC_READ_INTERVAL_MS = 60000UL;        // refresh DS3231 cache every 60 seconds
const unsigned long RTC_PROBE_INTERVAL_MS = 30000UL;       // retry if module is missing
const unsigned long RTC_NTP_SYNC_INTERVAL_MS = 21600000UL; // write NTP time to RTC every 6 hours
const uint32_t RTC_MIN_VALID_EPOCH = 1700000000UL;

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
WiFiClient mqttNetClient;
PubSubClient mqttClient(mqttNetClient);
DNSServer captiveDnsServer;
WebSocketsServer wsServer(ESP32_WS_INTERNAL_PORT);
WebServer* webServerPtr = nullptr;
#define webServer (*webServerPtr)

byte currentOutput1State = 0;
byte currentOutput2State = 0;
byte currentOutput5State = 0;
byte currentOutput6State = 0;

unsigned long lastSensorUpdateTime = 0;
unsigned long lastCommandCheckTime = 0;
unsigned long lastWiFiAttemptTime = 0;
// V1.6.6: Strict Static IP mode for port forwarding. If Static is ON, never fall back to DHCP.
// V1.6.7C: User Management page gets its own SPIFFS rescue store for users, permissions, Telegram IDs, subscription settings and 1500-char intro.
// V1.5.7: deferred/forced WiFi reconnect so saving a new network cannot be skipped by stale WL_CONNECTED state.
bool wifiReconnectPending = false;
unsigned long wifiReconnectDueMs = 0;
bool wifiForceReconnectPending = false;
bool wifiDhcpRescueMode = false;
bool wifiLastAttemptUsedStatic = false;
unsigned long wifiConnectAttemptStartedMs = 0;
char lastWifiActionText[96] = "boot";
int lastStaStatusCode = -1;
unsigned long lastWifiBeginMs = 0;
unsigned long lastWifiConnectedMs = 0;
unsigned long lastWifiDisconnectedMs = 0;
unsigned long lastWifiSaveMs = 0;
unsigned long lastWifiSavedPassLen = 0;

// V1.6.0: STA deep diagnostics. These values show why the router refused/disconnected the ESP32.
uint8_t lastStaDisconnectReason = 0;
char lastStaDisconnectReasonText[64] = "none";
bool lastStaScanFound = false;
int lastStaScanChannel = 0;
int lastStaScanRssi = -999;
char lastStaScanBssid[24] = "none";
int lastStaLowLevelSetConfigErr = 0;
int lastStaLowLevelConnectErr = 0;

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
byte clockState = 0; // 0=NO_TIME, 1=ONLINE_SYNC, 2=OFFLINE_ESTIMATED, 3=RTC_DS3231_BACKUP
uint32_t offlineClockBaseEpoch = 0;
unsigned long offlineClockBaseMs = 0;
unsigned long lastOfflineClockSaveMs = 0;
bool offlineClockLoaded = false;

// DS3231 RTC runtime state. The RTC stores raw UTC, then Egypt UTC+2/+3 is applied by getDeviceEpoch().
bool rtcWireStarted = false;
bool rtcPresent = false;
bool rtcTimeValid = false;
bool rtcLostPower = false;
bool rtcSyncedFromNtp = false;
uint32_t rtcLastUtcEpoch = 0;
unsigned long rtcLastReadMs = 0;
unsigned long rtcLastProbeMs = 0;
unsigned long rtcLastNtpSyncMs = 0;
String lastRtcMessage = "RTC not started";

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


// ========================= ESP32 UPGRADE FEATURES =========================
// Realtime dashboard websocket, event log, timed manual override,
// MQTT/Home Assistant, captive portal, Telegram alerts, calibration and
// mandatory admin password change. These settings are intentionally grouped
// here so they can be moved later to Preferences/NVS.

#define ESP32_FEATURES_ENABLED 1
#define ESP32_LOG_SIZE 50
#define ESP32_CAPTIVE_DNS_PORT 53
//#define ESP32_WS_INTERNAL_PORT 81
#define ENABLE_LEGACY_OUTPUT_TIMERS 0  // 0 = hide/disable old runtime timers; schedules remain active.

// Browser WebSocket port used when dashboard is opened from outside the LAN.
// Router must forward this external port to ESP32 internal TCP port 81.
uint16_t wsPublicPort = ESP32_WS_INTERNAL_PORT;

// MQTT / Home Assistant. Fill these from the web page /esp32 or hard-code them.
bool mqttEnabled = false;
char mqttHost[64] = "";
uint16_t mqttPort = 1883;
char mqttUser[32] = "";
char mqttPass[32] = "";
char mqttBaseTopic[64] = "ahmed/esp32";
char mqttDiscoveryPrefix[32] = "homeassistant";
unsigned long lastMqttReconnectMs = 0;
unsigned long lastMqttPublishMs = 0;
bool haDiscoverySent = false;

// Telegram alerts. Put Bot Token and Chat ID from /esp32.
bool telegramEnabled = false;
char telegramBotToken[80] = "";
char telegramChatId[32] = "";
float telegramHighTempC = 45.0f;
char telegramBotLink[128] = "";
unsigned long lastTelegramTempAlertMs = 0;
unsigned long lastTelegramWifiAlertMs = 0;
bool lastWifiOnlineForAlert = false;

// Telegram remote control polling.
// Notifications are sent to telegramChatId saved in /esp32. Commands are accepted from any Telegram chat if username/password are valid.
unsigned long lastTelegramCommandPollMs = 0;
long telegramLastUpdateId = 0;
bool telegramCommandPollReady = false;
// Telegram polling modes: Fast = every 10 seconds. Smart = 60 seconds idle, 10 seconds for 2 minutes after activity.
const unsigned long TELEGRAM_COMMAND_POLL_FAST_MS = 10000UL;
const unsigned long TELEGRAM_COMMAND_POLL_SMART_IDLE_MS = 60000UL;
const unsigned long TELEGRAM_COMMAND_POLL_SMART_ACTIVE_MS = 10000UL;
const unsigned long TELEGRAM_COMMAND_POLL_SMART_ACTIVE_WINDOW_MS = 120000UL;
bool telegramSmartPollingEnabled = false;
unsigned long telegramSmartActiveUntilMs = 0;
const unsigned long TELEGRAM_HTTP_TIMEOUT_MS = 3000UL;
const unsigned long TELEGRAM_MIN_SEND_GAP_MS = 5000UL;
unsigned long lastTelegramSendAttemptMs = 0;
bool telegramSendBusy = false;
bool telegramPendingSend = false;
char pendingTelegramText[256] = "";
int lastTelegramSendCode = 0;
int lastTelegramPollCode = 0;

// Telegram interactive login + inline buttons.
// Telegram has no hidden password field inside bot chats, so the bot asks the user to type
// username then password and shows a ForceReply input placeholder. Sessions are RAM-only.
#define TELEGRAM_SESSION_COUNT 5
#define TELEGRAM_LOGIN_TIMEOUT_MS 1800000UL
#define TG_STAGE_IDLE 0
#define TG_STAGE_WAIT_USER 1
#define TG_STAGE_WAIT_PASS 2

struct TelegramSessionSlot {
  char chatId[32];
  char pendingUser[WEB_USER_MAX_LEN + 1];
  byte userIndex;
  byte stage;
  byte lang; // V1.6.8: RAM language cache so Telegram changes apply immediately even if NVS is busy/unavailable.
  bool loggedIn;
  unsigned long lastSeenMs;
};

TelegramSessionSlot telegramSessions[TELEGRAM_SESSION_COUNT];

int lastThingSpeakReadStatus = 0;
int lastThingSpeakWriteStatus = 0;

const unsigned long WS_BROADCAST_MIN_INTERVAL_MS = 1000UL;
unsigned long lastWsBroadcastMs = 0;
bool wsBroadcastPending = false;

// Network self-healing / web priority diagnostics.
const unsigned long AP_WATCHDOG_INTERVAL_MS = 10000UL;
const unsigned long WEB_PRIORITY_GAP_MS = 150UL;
const unsigned long LOOP_GAP_WARN_MS = 2500UL;
unsigned long lastApWatchdogMs = 0;
unsigned long lastWebPriorityMs = 0;
unsigned long lastLoopTickMs = 0;
unsigned long maxLoopGapMs = 0;
unsigned long lastLongTaskMs = 0;
char lastLongTaskName[32] = "None";
uint16_t apWatchdogRestartCount = 0;
char lastApWatchdogText[48] = "AP watchdog not run yet";

struct PersistentDiagRecord {
  uint16_t version;
  uint32_t bootCount;
  uint32_t lastSavedUptimeSec;
  uint32_t maxLoopGapMs;
  uint32_t lastLongTaskMs;
  char lastLongTaskName[32];
  uint16_t apRestartCount;
  uint32_t minFreeHeap;
  int32_t rssi;
  char lastResetReason[40];
  char lastEvent[72];
  char lastEventTime[32];
};

PersistentDiagRecord persistentDiag;
unsigned long lastPersistentDiagSaveMs = 0;

// Calibration offsets.
float tempCalibrationOffset = 0.0f;
float humCalibrationOffset = 0.0f;

// Last command details shown in the WebSocket realtime JSON/page.
char lastCommandText[96] = "No command yet";
char lastCommandTime[32] = "--";
char lastCommandUser[32] = "--";
char lastCommandSource[24] = "--";
unsigned long lastCommandMs = 0;

// Custom labels for outputs/inputs shown in WebSocket, web pages, MQTT discovery and Telegram.
// Index uses the field/output number directly: 1,2,3,5,6,7,8,9,10.
char fieldLabels[FIELD_LABEL_COUNT + 1][FIELD_LABEL_MAX_LEN + 1];
uint16_t webUserVisibleMask[WEB_USER_COUNT];
uint16_t webUserControlMask[WEB_USER_COUNT];
char webUserTelegramChatId[WEB_USER_COUNT][USER_TELEGRAM_CHAT_ID_MAX_LEN + 1];
byte webUserTelegramNotify[WEB_USER_COUNT];
Preferences userTelegramPrefs;

// Per Telegram chat language preference. Stored in Preferences/NVS by Telegram Chat ID hash.
#define TG_LANG_UNKNOWN 0
#define TG_LANG_AR      1
#define TG_LANG_EN      2
Preferences telegramLangPrefs;

// V1.6.9: Telegram language runtime override.
// This makes the selected language take effect in the very next reply/keyboard,
// even if Preferences/NVS is busy or the RAM session slot is refreshed.
String telegramRuntimeLangChatId = "";
byte telegramRuntimeLang = TG_LANG_UNKNOWN;
unsigned long telegramRuntimeLangUntilMs = 0;

// Per web user language preference. Stored in Preferences/NVS by user slot.
// The same preference is synced to Telegram language when the user has a Telegram Chat ID saved.
Preferences webLangPrefs;

void loadFieldLabelsConfig();
void saveFieldLabelsConfig();
void resetFieldLabelsToDefaults();
void setFieldLabel(byte fieldNumber, const String& label);
String fieldLabel(byte fieldNumber);
String fieldLabelHtml(byte fieldNumber);
void handleSaveFieldLabels();
void setDefaultUserFieldPermissions();
void loadUserFieldPermissions();
void saveUserFieldPermissions();
void setDefaultUserTelegramNotifications();
void loadUserTelegramNotifications();
void saveUserTelegramNotifications();
bool isSafeTelegramChatIdText(const String& id);
void setUserTelegramChatId(byte idx, const String& chatId);
bool canUserSeeField(byte userIndex, byte fieldNumber);
bool canUserControlField(byte userIndex, byte fieldNumber);
bool canCurrentUserSeeField(byte fieldNumber);
bool canCurrentUserControlField(byte fieldNumber);
bool userHasAnyVisibleIoField();
String userFieldPermissionsHtml(byte userIndex);

void loadEsp32IntegrationConfig();
void saveEsp32IntegrationConfig();
void clearEsp32IntegrationConfig();

// Timed manual override for relays 1/2, smart output 3, and extra outputs 5/6.
bool manualOverrideActive[7] = {false};
byte manualOverrideState[7] = {0};
unsigned long manualOverrideEndMs[7] = {0};

#define DIGITAL_INPUT_DEBOUNCE_MS 250UL
byte stableInput7State = 0;
byte stableInput8State = 0;
byte stableInput9State = 0;
byte stableInput10State = 0;
byte lastRawInput7State = 0;
byte lastRawInput8State = 0;
byte lastRawInput9State = 0;
byte lastRawInput10State = 0;
unsigned long input7ChangedMs = 0;
unsigned long input8ChangedMs = 0;
unsigned long input9ChangedMs = 0;
unsigned long input10ChangedMs = 0;
bool inputDebounceReady = false;

struct OutputAutoConfig {
  byte enabled;
  byte scheduleEnable;
  byte timerMode;
  byte repeatWeekly;
  byte dayEnabled[7];
  byte slotEnabled[7][SCHEDULE_SLOTS_PER_DAY];
  uint16_t dayStartMin[7][SCHEDULE_SLOTS_PER_DAY];
  uint16_t dayEndMin[7][SCHEDULE_SLOTS_PER_DAY];
  uint32_t timerDurationSec;
};

const byte OUTPUT_AUTO_FIELDS[OUTPUT_AUTO_COUNT] = {1, 2, 5, 6};
OutputAutoConfig outputAutoCfg[OUTPUT_AUTO_COUNT];
byte outputAutoSyncThingSpeak[OUTPUT_AUTO_COUNT] = {0, 0, 0, 0};
bool outputAutoTimerActive[OUTPUT_AUTO_COUNT] = {false, false, false, false};
unsigned long outputAutoTimerStartMs[OUTPUT_AUTO_COUNT] = {0, 0, 0, 0};
unsigned long lastOutputAutoLiveBroadcastMs = 0;

// Manual Auto-OFF at selected time.
// This is independent from timers/schedules. It only closes outputs that were last turned ON manually
// from Web, Telegram, ThingSpeak, MQTT, Button or Timed Override, and only for outputs with this option enabled.
#define MANUAL_AUTO_OFF9_DEFAULT_MINUTE 540  // 09:00 default, editable from Automation page.
#define MANUAL_AUTO_OFF9_WINDOW_MIN 5
#define MANUAL_AUTO_OFF9_CHECK_MS 20000UL
uint16_t manualAutoOff9Minute = MANUAL_AUTO_OFF9_DEFAULT_MINUTE;
byte manualAutoOff9Enabled[OUTPUT_AUTO_COUNT] = {0, 0, 0, 0};
byte manualAutoOff9Out3Enabled = 0;
byte outputLastManualOn[OUTPUT_AUTO_COUNT] = {0, 0, 0, 0};
byte output3LastManualOn = 0;
uint32_t manualAutoOff9LastYmd = 0;
unsigned long lastManualAutoOff9CheckMs = 0;
Preferences manualAutoOff9Prefs;


// Manual Auto-OFF after a selected duration.
// Starts only when an output is turned ON manually. Schedules and automation do not start it.
#define MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN 30
#define MANUAL_AUTO_OFF_DURATION_MIN 1
#define MANUAL_AUTO_OFF_DURATION_MAX_MIN 1440
#define MANUAL_AUTO_OFF_DURATION_CHECK_MS 5000UL
byte manualAutoOffDurEnabled[OUTPUT_AUTO_COUNT] = {0, 0, 0, 0};
uint16_t manualAutoOffDurMin[OUTPUT_AUTO_COUNT] = {MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN, MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN, MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN, MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN};
uint32_t manualAutoOffDurDeadlineEpoch[OUTPUT_AUTO_COUNT] = {0, 0, 0, 0};
unsigned long manualAutoOffDurDeadlineMs[OUTPUT_AUTO_COUNT] = {0, 0, 0, 0};
byte manualAutoOffDurOut3Enabled = 0;
uint16_t manualAutoOffDurOut3Min = MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN;
uint32_t manualAutoOffDurOut3DeadlineEpoch = 0;
unsigned long manualAutoOffDurOut3DeadlineMs = 0;
unsigned long lastManualAutoOffDurCheckMs = 0;
Preferences manualAutoOffDurPrefs;
Preferences manualAutoUnifiedPrefs;

struct ManualAutoStoreRecord {
  uint16_t version;
  uint16_t offMinute;
  uint32_t offMask;
  uint32_t offLastYmd;
  uint32_t durMask;
  uint16_t durMin[OUTPUT_AUTO_COUNT];
  uint32_t durDeadlineEpoch[OUTPUT_AUTO_COUNT];
  byte out3Off9Enabled;
  byte out3DurEnabled;
  uint16_t out3DurMin;
  uint32_t out3DurDeadlineEpoch;
};

bool manualAutoLoadedFromEeprom = false;

struct Esp32LogEntry {
  unsigned long ms;
  char time[32];
  char text[88];
};
Esp32LogEntry esp32Log[ESP32_LOG_SIZE];
byte esp32LogHead = 0;
byte esp32LogCount = 0;

bool mustChangeAdminPassword() {
  return WEB_USERS[0].enabled && String(WEB_USERS[0].user) == "admin" && String(WEB_USERS[0].pass) == "admin";
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"') out += F("\\\"");
    else if (c == '\\') out += F("\\\\");
    else if (c == '\n' || c == '\r') out += ' ';
    else out += c;
  }
  return out;
}

String currentDeviceDateTimeText();
void initRtcDs3231();
void maintainRtcDs3231();
String rtcStatusText();
String resetReasonText();
String formatUptimeText(unsigned long seconds);
void telegramSend(const String& text);
void telegramSendToChat(const String& targetChatId, const String& text);
int telegramNotifyUsersForField(byte fieldNumber, const String& text);
String telegramStatusText(const String& title);
void telegramSendMainMenu(const String& chatId, byte userIndex, const String& title);
int telegramSessionIndexForChat(const String& chatId, bool createIfMissing);
void checkTelegramCommands();
void processPendingTelegramSend();
void processPendingWsBroadcast();
void sendSystemHealthPage();
void loadPersistentDiagnostics();
void markBootPersistentDiagnostics();
void savePersistentDiagnostics(const String& event, bool force);
void resetPersistentDiagnosticsRecord();
bool findWebUser(const String& user, const String& pass, byte& foundIndex);
bool findWebUserAnyState(const String& user, const String& pass, byte& foundIndex);
void loadMonthlySubscriptionState();
void saveMonthlySubscriptionState(uint32_t ym);
void saveMonthlySubscriptionSettings();
bool isTimeValidNow();
uint32_t getDeviceEpoch();
byte daysInMonth(int y, byte m);
bool getDeviceDateParts(int& year, byte& month, byte& day);
uint32_t currentYearMonthKey();
byte subscriptionDaysLeftThisMonth();
bool subscriptionWarningActive(byte& daysLeft);
String subscriptionDisabledTextWeb(const String& user);
String subscriptionWarningTextWeb(const String& user, byte daysLeft);
String subscriptionDisabledTextTelegram(const String& chatId, const String& user);
String subscriptionWarningTextTelegram(const String& chatId, const String& user, byte daysLeft);
void notifyUserSubscriptionMessage(byte idx, const String& text);
void sendSubscriptionDisabledPage(const String& user);
void sendSubscriptionWarningPage(const String& user, byte daysLeft, const String& ret);
void maintainMonthlyUserSubscriptionDisable(bool force = false);
void loadActivationCodes();
void saveActivationCodes();
void loadActivationLog();
void saveActivationLog();
void addActivationLog(const String& code, const String& user, const String& at, const String& method);
String generateOneTimeActivationCode();
bool consumeActivationCode(const String& codeIn, const String& usedBy, const String& method);
bool activationCodeIsExpired(byte i);
byte findWebUserByTelegramChatId(const String& chatId);
String activationCodesAdminCardHtml();
String activationLogAdminHtml();
String activationCodesTextReport();
void handleGenerateActivationCodes();
void handleClearUsedActivationCodes();
void handleDownloadActivationCodesTxt();
void handleSaveSubscriptionSettings();
void handleWebActivationCode();
String telegramActivationCodesKeyboardJson(const String& chatId);
String telegramActivationCodesText(const String& chatId);
String telegramGenerateActivationCodesText(const String& chatId, byte count);
void telegramSendActivationCodesMenu(const String& chatId, const String& title);
void loadTelegramUiConfig();
void saveTelegramUiConfig();
String normalizedTelegramBotLink();
String telegramBotLinkHomeCardHtml();
unsigned long telegramCurrentPollIntervalMs();
void telegramMarkActivePolling();
String activationAttemptKey(const String& prefix, const String& id);
bool activationAttemptIsLocked(const String& key, String& msg, bool ar);
String activationAttemptFailureMessage(const String& key, bool ar);
void activationAttemptClear(const String& key);
void telegramHandleActivationCode(const String& chatId, const String& codeIn);
String manualAutoOff9TimeText();
uint16_t parseTimeToMinutesStrict(const String& text, uint16_t fallback);
bool isSafePassword(const String& p);
void setWebUserPassword(byte idx, const String& p);
void saveWebUsersConfig();
void saveUserFieldPermissions();
byte findWebUserIndexByName(const String& user);
bool setWebUserPasswordByIndex(byte idx, const String& newPass);
String telegramUsersListText();
String telegramUsersKeyboardJson();
void telegramSendUsersMenu(const String& chatId, const String& title);
bool setWebUserEnabledByIndex(byte idx, bool enabled);
byte readInput7State();
byte readInput8State();
byte readInput9State();
byte readInput10State();
void maintainDigitalInputs();
void loadOutputAutoConfigs();
void saveOutputAutoConfigs();
void loadOutputAutoSyncConfig();
void saveOutputAutoSyncConfig();
void loadManualAutoOff9Config();
void saveManualAutoOff9Config();
void loadManualAutoOffDurationConfig();
void saveManualAutoOffDurationConfig();
bool loadManualAutoStoreFromEeprom();
void saveManualAutoStoreToEeprom();
uint16_t clampManualAutoOffDurationMin(uint32_t minutes);
void startManualAutoOffDurationForOutput(byte outputNumber);
void cancelManualAutoOffDurationForOutput(byte outputNumber);
void startManualAutoOffDurationForOut3();
void cancelManualAutoOffDurationForOut3();
uint32_t manualAutoOffDurationRemainingSec(byte idx);
uint32_t manualAutoOffDurationOut3RemainingSec();
void maintainManualAutoOffDuration();
void loadLoginIntroConfig();
void saveLoginIntroConfig();
bool loadUserPagePersistentConfig();
bool saveUserPagePersistentConfig(const char* reason);
String userPageStorageStatusText();
void recordOutputManualControl(byte outputNumber, byte newState);
void recordSmartOutput3ManualControl(byte newState);
void writeSmartOutput(bool on);
void saveSmartOutputConfig();
void maintainManualAutoOff9();
void maintainOutputAutoControl();
void sendOutputAutoPage();
void handleSaveOutputAuto();
void serviceWebCore(bool force = false);
void apWatchdog(bool force = false);
void noteTaskDuration(const char* name, unsigned long startMs);
uint16_t eepromBackupChecksum();
void loadAutoBackupConfig();
void saveAutoBackupConfig();
bool createAutoBackupNow(const char* reason, bool notifyTelegram);
void autoBackupAfterSave(const char* reason);
void maintainAutoBackup();
String autoBackupStatusText();
void handleAutoBackupNow();
void handleSaveAutoBackupOptions();
void handleDownloadLatestAutoBackup();
void handleRestoreLatestAutoBackup();

void addEsp32Log(const String& msg) {
  Esp32LogEntry &e = esp32Log[esp32LogHead];
  e.ms = millis();
  String ts = currentDeviceDateTimeText();
  if (ts.length() == 0) ts = "--";
  ts.substring(0, sizeof(e.time)-1).toCharArray(e.time, sizeof(e.time));
  msg.substring(0, sizeof(e.text)-1).toCharArray(e.text, sizeof(e.text));
  esp32LogHead = (esp32LogHead + 1) % ESP32_LOG_SIZE;
  if (esp32LogCount < ESP32_LOG_SIZE) esp32LogCount++;
}

String actionSourceBase(const String& source) {
  String s = source;
  s.trim();
  int p = s.indexOf('(');
  if (p > 0) {
    String base = s.substring(0, p);
    base.trim();
    if (base.length()) return base;
  }
  return s.length() ? s : String("Unknown");
}

String actionUserFromSource(const String& source) {
  String s = source;
  s.trim();
  int p1 = s.indexOf('(');
  int p2 = s.lastIndexOf(')');
  if (p1 >= 0 && p2 > p1) {
    String u = s.substring(p1 + 1, p2);
    u.trim();
    if (u.length()) return u;
  }

  String base = actionSourceBase(s);
  if ((base.equalsIgnoreCase("Web") || base.equalsIgnoreCase("Timed Override")) && currentWebUserIndex < WEB_USER_COUNT && WEB_USERS[currentWebUserIndex].enabled && WEB_USERS[currentWebUserIndex].user[0]) {
    return String(WEB_USERS[currentWebUserIndex].user);
  }
  if (base.equalsIgnoreCase("Timed Expire")) return "System";
  if (base.equalsIgnoreCase("ThingSpeak")) return "ThingSpeak";
  if (base.equalsIgnoreCase("MQTT")) return "MQTT";
  if (base.equalsIgnoreCase("Button")) return "Button";
  return base;
}

String telegramOutputChangeText(const String& srcBase, const String& actionUser, byte outputNumber, byte newState, const String& ts) {
  String msg;
  msg.reserve(180);
  msg += "Output Changed";
  msg += "\nOutput: "; msg += fieldLabel(outputNumber);
  msg += "\nState: "; msg += newState ? "ON" : "OFF";
  msg += "\nSource: "; msg += srcBase;
  msg += "\nUser: "; msg += actionUser;
  msg += "\nTime: "; msg += ts.length() ? ts : String("--");
  return msg;
}


String telegramInputChangeText(byte fieldNumber, byte state, const String& ts) {
  String msg;
  msg.reserve(150);
  msg += "Input Changed";
  msg += "\nInput: "; msg += fieldLabel(fieldNumber);
  msg += "\nState: "; msg += state ? "ON" : "OFF";
  msg += "\nTime: "; msg += ts.length() ? ts : String("--");
  return msg;
}

bool telegramUserNotificationAllowed(byte userIndex, byte fieldNumber) {
  if (!telegramEnabled || !telegramBotToken[0] || WiFi.status() != WL_CONNECTED) return false;
  if (userIndex >= WEB_USER_COUNT) return false;
  if (!WEB_USERS[userIndex].enabled || !WEB_USERS[userIndex].user[0]) return false;
  if (!webUserTelegramNotify[userIndex] || !webUserTelegramChatId[userIndex][0]) return false;
  if (!canUserSeeField(userIndex, fieldNumber)) return false;
  return true;
}

int telegramNotifyUsersForField(byte fieldNumber, const String& text) {
  int sent = 0;
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    if (!telegramUserNotificationAllowed(i, fieldNumber)) continue;
    bool duplicate = false;
    for (byte j = 0; j < i; j++) {
      if (telegramUserNotificationAllowed(j, fieldNumber) && strcmp(webUserTelegramChatId[j], webUserTelegramChatId[i]) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    telegramSendToChat(String(webUserTelegramChatId[i]), text);
    sent++;
    yield();
  }
  return sent;
}

void rememberOutputCommand(const char* source, byte outputNumber, byte newState, bool changed) {
  // Do not update Last Command or add log lines for repeated commands that do not change any output.
  // WebSocket status still broadcasts normally, but the event log stays clean.
  if (!changed) return;

  String srcRaw = source ? String(source) : String("Unknown");
  String srcBase = actionSourceBase(srcRaw);
  String actionUser = actionUserFromSource(srcRaw);

  lastCommandMs = millis();
  String ts = currentDeviceDateTimeText();
  if (ts.length() == 0) ts = "--";
  ts.substring(0, sizeof(lastCommandTime)-1).toCharArray(lastCommandTime, sizeof(lastCommandTime));
  actionUser.substring(0, sizeof(lastCommandUser)-1).toCharArray(lastCommandUser, sizeof(lastCommandUser));
  srcBase.substring(0, sizeof(lastCommandSource)-1).toCharArray(lastCommandSource, sizeof(lastCommandSource));

  String txt = srcBase + " by " + actionUser + " " + fieldLabel(outputNumber) + " " + (newState ? "ON" : "OFF");
  txt.substring(0, sizeof(lastCommandText)-1).toCharArray(lastCommandText, sizeof(lastCommandText));

  addEsp32Log(lastCommandText);

  // Any real output change is sent to Telegram users who can see this output.
  // If no per-user Telegram ID is configured yet, keep the old global admin notification fallback.
  String notifyText = telegramOutputChangeText(srcBase, actionUser, outputNumber, newState, ts);
  if (telegramNotifyUsersForField(outputNumber, notifyText) == 0) telegramSend(notifyText);
}

String outputStatusJson(bool changed, bool filterForCurrentUser = true);
bool outputAutoScheduleActiveNow(byte idx);
bool setOutputStateInternal(byte outputNumber, byte newState, const char* source, bool saveToEeprom, bool syncThingSpeak);
bool setOutputFromWeb(byte outputNumber, byte newState, const char* source = "Web");
bool setOutputFromAutomation(byte outputNumber, byte newState, const char* source = "Output Automation");

String esp32LogJson() {
  String j = F("[");
  for (byte i = 0; i < esp32LogCount; i++) {
    byte idx = (esp32LogHead + ESP32_LOG_SIZE - esp32LogCount + i) % ESP32_LOG_SIZE;
    if (i) j += ',';
    j += F("{\"ms\":"); j += esp32Log[idx].ms;
    j += F(",\"time\":\""); j += jsonEscape(String(esp32Log[idx].time)); j += F("\"");
    j += F(",\"text\":\""); j += jsonEscape(String(esp32Log[idx].text)); j += F("\"}");
  }
  j += F("]");
  return j;
}

String esp32RealtimeJson() {
  String j = outputStatusJson(false, false);
  if (j.endsWith("}")) j.remove(j.length()-1);
  j += F(",\"rssi\":"); j += (WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  j += F(",\"ip\":\""); j += jsonEscape(WiFi.localIP().toString()); j += F("\"");
  j += F(",\"apIp\":\""); j += jsonEscape(WiFi.softAPIP().toString()); j += F("\"");
  j += F(",\"apClients\":"); j += WiFi.softAPgetStationNum();
  j += F(",\"apRestarts\":"); j += apWatchdogRestartCount;
  j += F(",\"maxLoopGap\":"); j += maxLoopGapMs;
  j += F(",\"lastSlowTask\":\""); j += jsonEscape(String(lastLongTaskName)); j += F("\"");
  j += F(",\"lastSlowMs\":"); j += lastLongTaskMs;
  j += F(",\"mqtt\":"); j += mqttClient.connected() ? F("true") : F("false");
  j += F(",\"adminDefault\":"); j += mustChangeAdminPassword() ? F("true") : F("false");
  j += F(",\"uptime\":"); j += (millis() / 1000UL);
  j += F(",\"heap\":"); j += ESP.getFreeHeap();
  j += F(",\"minHeap\":"); j += ESP.getMinFreeHeap();
  j += F(",\"log\":"); j += esp32LogJson();
  j += F("}");
  return j;
}

void wsBroadcastStatus() {
  unsigned long now = millis();
  if (lastWsBroadcastMs != 0 && (unsigned long)(now - lastWsBroadcastMs) < WS_BROADCAST_MIN_INTERVAL_MS) {
    wsBroadcastPending = true;
    return;
  }
  lastWsBroadcastMs = now;
  wsBroadcastPending = false;
  String payload = esp32RealtimeJson();
  wsServer.broadcastTXT(payload);
}

void processPendingWsBroadcast() {
  if (!wsBroadcastPending) return;
  unsigned long now = millis();
  if (lastWsBroadcastMs == 0 || (unsigned long)(now - lastWsBroadcastMs) >= WS_BROADCAST_MIN_INTERVAL_MS) {
    wsBroadcastStatus();
  }
}

void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    String wsPayload = esp32RealtimeJson();
    wsServer.sendTXT(num, wsPayload);
  }
}

void setupWebSocketDashboard() {
  wsServer.begin();
  wsServer.onEvent(handleWebSocketEvent);
  addEsp32Log(String("WebSocket realtime dashboard started on internal port ") + String(ESP32_WS_INTERNAL_PORT));
}

void startCaptivePortalDns() {
  captiveDnsServer.start(ESP32_CAPTIVE_DNS_PORT, "*", WiFi.softAPIP());
  addEsp32Log("Captive Portal DNS started");
}

String telegramUrlEncode(const String& value) {
  String encoded;
  encoded.reserve(value.length() * 3 + 8);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); i++) {
    uint8_t c = (uint8_t)value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

void telegramSend(const String& text) {
  if (!telegramEnabled || !telegramBotToken[0] || !telegramChatId[0] || WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (telegramSendBusy || (lastTelegramSendAttemptMs != 0 && (unsigned long)(now - lastTelegramSendAttemptMs) < TELEGRAM_MIN_SEND_GAP_MS)) {
    text.substring(0, sizeof(pendingTelegramText) - 1).toCharArray(pendingTelegramText, sizeof(pendingTelegramText));
    telegramPendingSend = true;
    return;
  }

  telegramSendBusy = true;
  lastTelegramSendAttemptMs = now;
  lastTelegramSendCode = 0;

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + telegramBotToken + "/sendMessage";
  if (http.begin(secure, url)) {
    http.setTimeout(TELEGRAM_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String body = String("chat_id=") + telegramUrlEncode(String(telegramChatId)) + "&text=" + telegramUrlEncode(text);
    lastTelegramSendCode = http.POST(body);
    http.end();
  } else {
    lastTelegramSendCode = -1000;
  }

  telegramSendBusy = false;
}


void telegramSendToChat(const String& targetChatId, const String& text) {
  if (!telegramEnabled || !telegramBotToken[0] || !targetChatId.length() || WiFi.status() != WL_CONNECTED) return;

  telegramSendBusy = true;
  lastTelegramSendAttemptMs = millis();
  lastTelegramSendCode = 0;

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + telegramBotToken + "/sendMessage";
  if (http.begin(secure, url)) {
    http.setTimeout(TELEGRAM_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String body = String("chat_id=") + telegramUrlEncode(targetChatId) + "&text=" + telegramUrlEncode(text);
    lastTelegramSendCode = http.POST(body);
    http.end();
  } else {
    lastTelegramSendCode = -1000;
  }

  telegramSendBusy = false;
}

void telegramSendToChatWithReplyMarkup(const String& targetChatId, const String& text, const String& replyMarkupJson) {
  if (!telegramEnabled || !telegramBotToken[0] || !targetChatId.length() || WiFi.status() != WL_CONNECTED) return;

  telegramSendBusy = true;
  lastTelegramSendAttemptMs = millis();
  lastTelegramSendCode = 0;

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + telegramBotToken + "/sendMessage";
  if (http.begin(secure, url)) {
    http.setTimeout(TELEGRAM_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String body = String("chat_id=") + telegramUrlEncode(targetChatId) + "&text=" + telegramUrlEncode(text);
    if (replyMarkupJson.length()) body += String("&reply_markup=") + telegramUrlEncode(replyMarkupJson);
    lastTelegramSendCode = http.POST(body);
    http.end();
  } else {
    lastTelegramSendCode = -1000;
  }

  telegramSendBusy = false;
}

void telegramEditMessageTextWithReplyMarkup(const String& targetChatId, const String& messageId, const String& text, const String& replyMarkupJson) {
  if (!telegramEnabled || !telegramBotToken[0] || !targetChatId.length() || !messageId.length() || WiFi.status() != WL_CONNECTED) return;
  if (telegramSendBusy) return;
  telegramSendBusy = true;
  lastTelegramSendAttemptMs = millis();
  lastTelegramSendCode = 0;

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + telegramBotToken + "/editMessageText";
  if (http.begin(secure, url)) {
    http.setTimeout(TELEGRAM_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String body = String("chat_id=") + telegramUrlEncode(targetChatId)
      + "&message_id=" + telegramUrlEncode(messageId)
      + "&text=" + telegramUrlEncode(text);
    if (replyMarkupJson.length()) body += String("&reply_markup=") + telegramUrlEncode(replyMarkupJson);
    lastTelegramSendCode = http.POST(body);
    http.end();
  } else {
    lastTelegramSendCode = -1000;
  }
  telegramSendBusy = false;
}

void telegramAnswerCallback(const String& callbackId, const String& text) {
  if (!telegramEnabled || !telegramBotToken[0] || !callbackId.length() || WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + telegramBotToken + "/answerCallbackQuery";
  if (http.begin(secure, url)) {
    http.setTimeout(TELEGRAM_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String shortText = text.substring(0, 80);
    String body = String("callback_query_id=") + telegramUrlEncode(callbackId) + "&text=" + telegramUrlEncode(shortText) + "&show_alert=false";
    lastTelegramSendCode = http.POST(body);
    http.end();
  }
}

void processPendingTelegramSend() {
  if (!telegramPendingSend || !pendingTelegramText[0]) return;
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (lastTelegramSendAttemptMs != 0 && (unsigned long)(now - lastTelegramSendAttemptMs) < TELEGRAM_MIN_SEND_GAP_MS) return;

  String msg = String(pendingTelegramText);
  pendingTelegramText[0] = '\0';
  telegramPendingSend = false;
  telegramSend(msg);
}

byte telegramOutputState(byte outputNumber) {
  if (outputNumber == 1) return currentOutput1State;
  if (outputNumber == 2) return currentOutput2State;
  if (outputNumber == 5) return currentOutput5State;
  if (outputNumber == 6) return currentOutput6State;
  return 0;
}

const char* telegramRoleText(byte role) {
  if (role >= WEB_ROLE_ADMIN) return "Admin";
  if (role >= WEB_ROLE_ENGINEER) return "Engineer";
  if (role >= WEB_ROLE_OPERATOR) return "Operator";
  return "Viewer";
}

byte findWebUserIndexByName(const String& user) {
  String u = user;
  u.trim();
  if (!u.length()) return 255;
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    if (WEB_USERS[i].user[0] && u.equalsIgnoreCase(String(WEB_USERS[i].user))) return i;
  }
  return 255;
}

bool setWebUserEnabledByIndex(byte idx, bool enabled) {
  if (idx == 0 || idx >= WEB_USER_COUNT) return false;
  if (!WEB_USERS[idx].user[0] || !WEB_USERS[idx].pass[0]) return false;
  WEB_USERS[idx].enabled = enabled ? 1 : 0;
  saveWebUsersConfig();
  addEsp32Log(String("User state changed: ") + WEB_USERS[idx].user + (enabled ? " enabled" : " disabled"));
  return true;
}

bool setWebUserPasswordByIndex(byte idx, const String& newPass) {
  if (idx >= WEB_USER_COUNT) return false;
  if (!WEB_USERS[idx].user[0]) return false;
  String p = newPass;
  p.trim();
  if (!isSafePassword(p)) return false;
  setWebUserPassword(idx, p);
  saveWebUsersConfig();
  addEsp32Log(String("Telegram password changed for user: ") + WEB_USERS[idx].user);
  return true;
}

uint32_t telegramChatIdHash(const String& chatId) {
  uint32_t h = 2166136261UL;
  for (uint16_t i = 0; i < chatId.length(); i++) {
    h ^= (uint8_t)chatId[i];
    h *= 16777619UL;
  }
  return h;
}

byte telegramLanguageForChat(const String& chatId) {
  if (!chatId.length()) return TG_LANG_UNKNOWN;

  // V1.6.9: immediate runtime language override has highest priority.
  if (telegramRuntimeLangChatId.length() && telegramRuntimeLangChatId == chatId &&
      (telegramRuntimeLang == TG_LANG_AR || telegramRuntimeLang == TG_LANG_EN) &&
      (telegramRuntimeLangUntilMs == 0 || (long)(telegramRuntimeLangUntilMs - millis()) > 0)) {
    return telegramRuntimeLang;
  }

  // V1.6.8: first use the active Telegram session cache.
  // This fixes the inline Language button changing only the title while the menu body/buttons
  // stayed in the old language when NVS/Preferences was unavailable or delayed.
  for (byte i = 0; i < TELEGRAM_SESSION_COUNT; i++) {
    if (telegramSessions[i].chatId[0] && String(telegramSessions[i].chatId) == chatId) {
      byte cachedLang = telegramSessions[i].lang;
      if (cachedLang == TG_LANG_AR || cachedLang == TG_LANG_EN) return cachedLang;
      break;
    }
  }

  char key[12];
  snprintf(key, sizeof(key), "l%08lx", (unsigned long)telegramChatIdHash(chatId));
  if (!telegramLangPrefs.begin("tg_lang", true)) return TG_LANG_UNKNOWN;
  byte lang = telegramLangPrefs.getUChar(key, TG_LANG_UNKNOWN);
  telegramLangPrefs.end();
  return (lang == TG_LANG_AR || lang == TG_LANG_EN) ? lang : TG_LANG_UNKNOWN;
}

void telegramSetLanguageForChat(const String& chatId, byte lang) {
  if (!chatId.length()) return;
  if (!(lang == TG_LANG_AR || lang == TG_LANG_EN)) return;

  // V1.6.9: force current chat language immediately for all next text/keyboard rendering.
  telegramRuntimeLangChatId = chatId;
  telegramRuntimeLang = lang;
  telegramRuntimeLangUntilMs = millis() + 86400000UL;

  // V1.6.8: update RAM session immediately before trying persistent storage.
  // So the next message/keyboard in the same callback is rendered in the selected language.
  bool sessionLangUpdated = false;
  for (byte i = 0; i < TELEGRAM_SESSION_COUNT; i++) {
    if (telegramSessions[i].chatId[0] && String(telegramSessions[i].chatId) == chatId) {
      telegramSessions[i].lang = lang;
      sessionLangUpdated = true;
      break;
    }
  }
  if (!sessionLangUpdated) {
    int sidx = telegramSessionIndexForChat(chatId, true);
    if (sidx >= 0 && sidx < TELEGRAM_SESSION_COUNT) telegramSessions[sidx].lang = lang;
  }

  char key[12];
  snprintf(key, sizeof(key), "l%08lx", (unsigned long)telegramChatIdHash(chatId));
  if (!telegramLangPrefs.begin("tg_lang", false)) return;
  telegramLangPrefs.putUChar(key, lang);
  telegramLangPrefs.end();
}

bool telegramChatIsArabic(const String& chatId) {
  byte lang = telegramLanguageForChat(chatId);
  return lang != TG_LANG_EN;
}

String telegramText(const String& chatId, const char* en, const char* ar) {
  return telegramChatIsArabic(chatId) ? String(ar) : String(en);
}

String telegramStateText(const String& chatId, bool state) {
  if (telegramChatIsArabic(chatId)) return state ? String("يعمل") : String("متوقف");
  return state ? String("ON") : String("OFF");
}

String telegramRoleTextLang(byte role, const String& chatId) {
  if (!telegramChatIsArabic(chatId)) return String(telegramRoleText(role));
  if (role >= WEB_ROLE_ADMIN) return String("مدير");
  if (role >= WEB_ROLE_ENGINEER) return String("مهندس");
  if (role >= WEB_ROLE_OPERATOR) return String("مشغل");
  return String("مشاهدة فقط");
}

// Telegram keyboard helper forward declarations
// Needed because telegramLanguageKeyboardJson() is above the helper implementations.
String telegramButtonJson(const String& text, const String& data, const String& style);
String telegramUrlButtonJson(const String& text, const String& url, const String& style);
void telegramKeyboardAddRow(String& keyboard, const String& b1);
void telegramKeyboardAddRow2(String& keyboard, const String& b1, const String& b2);

String telegramLanguageKeyboardJson() {
  String k = "{\"inline_keyboard\":[";
  telegramKeyboardAddRow2(k,
    telegramButtonJson("🇪🇬 العربية", "LANG:AR", "primary"),
    telegramButtonJson("🇬🇧 English", "LANG:EN", "primary")
  );
  k += "]}";
  return k;
}

void telegramSendLanguageMenu(const String& chatId) {
  String msg = "اختر اللغة / Choose Language";
  msg += "\n\n🇪🇬 العربية";
  msg += "\n🇬🇧 English";
  telegramSendToChatWithReplyMarkup(chatId, msg, telegramLanguageKeyboardJson());
}

String telegramSensorTextForChat(const String& chatId) {
  bool ar = telegramChatIsArabic(chatId);
  String msg;
  if (ar) {
    msg += "🌡️ قراءة الحساس";
    msg += "\nدرجة الحرارة: ";
    msg += isnan(currentTemperature) ? String("--") : String(currentTemperature, 1) + " °C";
    msg += "\nالرطوبة: ";
    msg += isnan(currentHumidity) ? String("--") : String(currentHumidity, 1) + " %";
    msg += "\nالوقت: "; msg += currentDeviceDateTimeText();
  } else {
    msg += "🌡️ Sensor Reading";
    msg += "\nTemperature: ";
    msg += isnan(currentTemperature) ? String("--") : String(currentTemperature, 1) + " °C";
    msg += "\nHumidity: ";
    msg += isnan(currentHumidity) ? String("--") : String(currentHumidity, 1) + " %";
    msg += "\nTime: "; msg += currentDeviceDateTimeText();
  }
  return msg;
}

String telegramStatusTextForUserIndexLang(const String& chatId, const String& title, byte userIndex) {
  bool ar = telegramChatIsArabic(chatId);
  String msg;
  if (title.length()) { msg += title; msg += "\n"; }
  msg += ar ? "الوقت: " : "Time: "; msg += currentDeviceDateTimeText();
  bool all = (userIndex >= WEB_USER_COUNT || WEB_USERS[userIndex].role >= WEB_ROLE_ADMIN);
  if (all || canUserSeeField(userIndex, 1)) { msg += "\n"; msg += fieldLabel(1); msg += ": "; msg += telegramStateText(chatId, currentOutput1State); }
  if (all || canUserSeeField(userIndex, 2)) { msg += "\n"; msg += fieldLabel(2); msg += ": "; msg += telegramStateText(chatId, currentOutput2State); }
  if (all || canUserSeeField(userIndex, 5)) { msg += "\n"; msg += fieldLabel(5); msg += ": "; msg += telegramStateText(chatId, currentOutput5State); }
  if (all || canUserSeeField(userIndex, 6)) { msg += "\n"; msg += fieldLabel(6); msg += ": "; msg += telegramStateText(chatId, currentOutput6State); }
  if (all || canUserSeeField(userIndex, 7)) { msg += "\n"; msg += fieldLabel(7); msg += ": "; msg += telegramStateText(chatId, readInput7State()); }
  if (all || canUserSeeField(userIndex, 8)) { msg += "\n"; msg += fieldLabel(8); msg += ": "; msg += telegramStateText(chatId, readInput8State()); }
  if (all || canUserSeeField(userIndex, 9)) { msg += "\n"; msg += fieldLabel(9); msg += ": "; msg += telegramStateText(chatId, readInput9State()); }
  if (all || canUserSeeField(userIndex, 10)) { msg += "\n"; msg += fieldLabel(10); msg += ": "; msg += telegramStateText(chatId, readInput10State()); }
  if (all) { msg += "\n"; msg += fieldLabel(3); msg += ": "; msg += telegramStateText(chatId, smartOutputState); }
  msg += ar ? "\nدرجة الحرارة: " : "\nTemp: ";
  msg += isnan(currentTemperature) ? String("--") : String(currentTemperature, 1) + " C";
  msg += ar ? "\nالرطوبة: " : "\nHum: ";
  msg += isnan(currentHumidity) ? String("--") : String(currentHumidity, 1) + " %";
  if (all) {
    msg += "\nIP: "; msg += WiFi.localIP().toString();
    msg += "\nRSSI: "; msg += (WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String("offline"));
    msg += ar ? "\nآخر أمر: " : "\nLast Command: "; msg += lastCommandText;
    msg += ar ? "\nوقت الأمر: " : "\nCommand Date: "; msg += lastCommandTime;
  }
  return msg;
}

int telegramSessionIndexForChat(const String& chatId, bool createIfMissing) {
  if (!chatId.length()) return -1;
  unsigned long now = millis();
  int freeSlot = -1;
  int oldestSlot = 0;
  unsigned long oldestSeen = 0xFFFFFFFFUL;

  for (byte i = 0; i < TELEGRAM_SESSION_COUNT; i++) {
    if (telegramSessions[i].chatId[0]) {
      if (String(telegramSessions[i].chatId) == chatId) {
        if (telegramSessions[i].lastSeenMs != 0 && (unsigned long)(now - telegramSessions[i].lastSeenMs) > TELEGRAM_LOGIN_TIMEOUT_MS) {
          telegramSessions[i].loggedIn = false;
          telegramSessions[i].stage = TG_STAGE_IDLE;
          telegramSessions[i].userIndex = 255;
          telegramSessions[i].pendingUser[0] = '\0';
        }
        telegramSessions[i].lastSeenMs = now;
        return i;
      }
      if (telegramSessions[i].lastSeenMs < oldestSeen) {
        oldestSeen = telegramSessions[i].lastSeenMs;
        oldestSlot = i;
      }
    } else if (freeSlot < 0) {
      freeSlot = i;
    }
  }

  if (!createIfMissing) return -1;
  int slot = (freeSlot >= 0) ? freeSlot : oldestSlot;
  memset(&telegramSessions[slot], 0, sizeof(telegramSessions[slot]));
  chatId.substring(0, sizeof(telegramSessions[slot].chatId) - 1).toCharArray(telegramSessions[slot].chatId, sizeof(telegramSessions[slot].chatId));
  telegramSessions[slot].userIndex = 255;
  telegramSessions[slot].stage = TG_STAGE_IDLE;
  telegramSessions[slot].lang = TG_LANG_UNKNOWN;
  // Load saved Telegram language into RAM once. If Preferences is unavailable,
  // selecting a language still works immediately through telegramSetLanguageForChat().
  {
    char key[12];
    snprintf(key, sizeof(key), "l%08lx", (unsigned long)telegramChatIdHash(chatId));
    if (telegramLangPrefs.begin("tg_lang", true)) {
      byte savedLang = telegramLangPrefs.getUChar(key, TG_LANG_UNKNOWN);
      telegramLangPrefs.end();
      if (savedLang == TG_LANG_AR || savedLang == TG_LANG_EN) telegramSessions[slot].lang = savedLang;
    }
  }
  telegramSessions[slot].loggedIn = false;
  telegramSessions[slot].lastSeenMs = now;
  return slot;
}

bool telegramSessionLoggedIn(int slot) {
  if (slot < 0 || slot >= TELEGRAM_SESSION_COUNT) return false;
  if (!telegramSessions[slot].loggedIn) return false;
  byte idx = telegramSessions[slot].userIndex;
  if (idx >= WEB_USER_COUNT || !WEB_USERS[idx].enabled || !WEB_USERS[idx].user[0]) {
    telegramSessions[slot].loggedIn = false;
    telegramSessions[slot].stage = TG_STAGE_IDLE;
    telegramSessions[slot].userIndex = 255;
    return false;
  }
  return true;
}

void telegramLogoutSession(int slot) {
  if (slot < 0 || slot >= TELEGRAM_SESSION_COUNT) return;
  telegramSessions[slot].loggedIn = false;
  telegramSessions[slot].stage = TG_STAGE_IDLE;
  telegramSessions[slot].userIndex = 255;
  telegramSessions[slot].pendingUser[0] = '\0';
}

String telegramButtonJson(const String& text, const String& data, const String& style = "") {
  String b = String("{\"text\":\"") + jsonEscape(text) + "\",\"callback_data\":\"" + jsonEscape(data) + "\"";
  if (style.length()) b += String(",\"style\":\"") + jsonEscape(style) + "\"";
  b += "}";
  return b;
}

String telegramUrlButtonJson(const String& text, const String& url, const String& style = "") {
  String b = String("{\"text\":\"") + jsonEscape(text) + "\",\"url\":\"" + jsonEscape(url) + "\"";
  if (style.length()) b += String(",\"style\":\"") + jsonEscape(style) + "\"";
  b += "}";
  return b;
}

void telegramKeyboardAddRow(String& keyboard, const String& b1) {
  if (!keyboard.endsWith("[")) keyboard += ',';
  keyboard += '[';
  keyboard += b1;
  keyboard += ']';
}

void telegramKeyboardAddRow2(String& keyboard, const String& b1, const String& b2) {
  if (!keyboard.endsWith("[")) keyboard += ',';
  keyboard += '[';
  keyboard += b1;
  keyboard += ',';
  keyboard += b2;
  keyboard += ']';
}

String telegramActivationCodesKeyboardJson(const String& chatId) {
  String k = "{\"inline_keyboard\":[";
  telegramKeyboardAddRow2(k,
    telegramButtonJson(telegramText(chatId, "➕ Generate 5", "➕ توليد 5"), "GENCODES5", "success"),
    telegramButtonJson(telegramText(chatId, "➕ Generate 10", "➕ توليد 10"), "GENCODES10", "success")
  );
  telegramKeyboardAddRow2(k,
    telegramButtonJson(telegramText(chatId, "📋 Show Codes", "📋 عرض الأكواد"), "CODESHOW", "primary"),
    telegramButtonJson(telegramText(chatId, "🔙 Back", "🔙 رجوع"), "MENU", "primary")
  );
  k += "]}";
  return k;
}

String telegramActivationCodesText(const String& chatId) {
  bool ar = telegramChatIsArabic(chatId);
  String msg;
  msg.reserve(3200);
  byte avail = 0, used = 0, expired = 0;
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    if (!activationCodes[i][0]) continue;
    if (activationCodeUsed[i]) used++;
    else if (activationCodeIsExpired(i)) expired++;
    else avail++;
  }
  msg += ar ? "🔐 أكواد التفعيل" : "🔐 Activation Codes";
  msg += "\n"; msg += ar ? "المتاح: " : "Available: "; msg += String(avail);
  msg += " / "; msg += String(ACTIVATION_CODE_COUNT);
  msg += "\n"; msg += ar ? "المستخدم: " : "Used: "; msg += String(used);
  if (expired) { msg += "\n"; msg += ar ? "المنتهي: " : "Expired: "; msg += String(expired); }
  msg += "\n"; msg += ar ? "انتهاء الصلاحية: " : "Expiry: "; msg += activationCodesExpireEndMonth ? (ar ? "آخر الشهر" : "End of month") : (ar ? "بدون انتهاء" : "No expiry");
  msg += "\n\n"; msg += ar ? "الأكواد المتاحة:" : "Available codes:";
  byte shown = 0;
  for (byte i = 0; i < ACTIVATION_CODE_COUNT && shown < 20; i++) {
    if (activationCodes[i][0] && !activationCodeUsed[i] && !activationCodeIsExpired(i)) {
      msg += "\n"; msg += activationCodes[i];
      shown++;
    }
  }
  if (!shown) msg += ar ? "\nلا يوجد" : "\nNone";
  msg += "\n\n"; msg += ar ? "آخر التفعيلات:" : "Last activations:";
  shown = 0;
  for (byte i = 0; i < ACTIVATION_LOG_COUNT && shown < 5; i++) {
    if (!activationLogCode[i][0]) continue;
    msg += "\n"; msg += String(activationLogUser[i]); msg += " - "; msg += String(activationLogAt[i]); msg += " - "; msg += String(activationLogMethod[i]);
    shown++;
  }
  if (!shown) msg += ar ? "\nلا يوجد" : "\nNone";
  return msg;
}

String telegramGenerateActivationCodesText(const String& chatId, byte count) {
  if (count < 1) count = 1;
  if (count > 10) count = 10;
  bool ar = telegramChatIsArabic(chatId);
  String msg;
  msg.reserve(600);
  msg += ar ? "تم توليد الأكواد:" : "Generated codes:";
  byte made = 0;
  for (byte i = 0; i < count; i++) {
    String c = generateOneTimeActivationCode();
    if (!c.length()) break;
    msg += "\n"; msg += c;
    made++;
  }
  if (!made) return ar ? String("لا توجد أماكن فارغة. امسح الأكواد المستخدمة أو استخدم الأكواد المتاحة.") : String("No free slots. Clear used codes or use existing available codes.");
  msg += ar ? "\n\nكل كود يستخدم مرة واحدة فقط." : "\n\nEach code works once only.";
  return msg;
}

void telegramSendActivationCodesMenu(const String& chatId, const String& title) {
  String msg = title.length() ? title + "\n\n" : String("");
  msg += telegramActivationCodesText(chatId);
  telegramSendToChatWithReplyMarkup(chatId, msg, telegramActivationCodesKeyboardJson(chatId));
}

String telegramLoggedOutKeyboardJson(const String& chatId) {
  String k = "{\"inline_keyboard\":[";
  telegramKeyboardAddRow2(k,
    telegramButtonJson(telegramText(chatId, "🔵 Login", "🔵 تسجيل الدخول"), "LOGIN", "primary"),
    telegramButtonJson(telegramText(chatId, "🌐 Language", "🌐 اللغة"), "LANG", "primary")
  );
  telegramKeyboardAddRow2(k,
    telegramButtonJson(telegramText(chatId, "🔵 Chat ID", "🔵 رقم تيليجرام"), "ID", "primary"),
    telegramButtonJson(telegramText(chatId, "🔵 Help", "🔵 المساعدة"), "HELP", "primary")
  );
  k += "]}";
  return k;
}

String telegramLoggedOutKeyboardJson() {
  return telegramLoggedOutKeyboardJson(String(""));
}

String telegramMainMenuKeyboardJson(byte userIndex, const String& chatId) {
  String k = "{\"inline_keyboard\":[";
  telegramKeyboardAddRow2(k,
    telegramButtonJson(telegramText(chatId, "🔵 Status", "🔵 الحالة"), "STATUS", "primary"),
    telegramButtonJson(telegramText(chatId, "🔵 Refresh", "🔵 تحديث"), "MENU", "primary")
  );
  const byte outs[4] = {1, 2, 5, 6};
  for (byte i = 0; i < 4; i++) {
    byte out = outs[i];
    if (!canUserControlField(userIndex, out)) continue;
    String lbl = fieldLabel(out);
    if (lbl.length() > 20) lbl = lbl.substring(0, 20);
    bool isOn = telegramOutputState(out);
    String stateText = telegramStateText(chatId, isOn);
    String stateEmoji = isOn ? "🟢 " : "🔴 ";
    String stateStyle = isOn ? "success" : "danger";
    telegramKeyboardAddRow(k, telegramButtonJson(stateEmoji + lbl + ": " + stateText, String("TOGGLE:") + String(out), stateStyle));
  }
  telegramKeyboardAddRow2(k,
    telegramUrlButtonJson(telegramText(chatId, "🌊 Tide Page", "🌊 صفحة المد والجزر"), RAS_SEDR_TIDE_URL, "primary"),
    telegramButtonJson(telegramText(chatId, "🌡️ Temp/Humidity", "🌡️ الحرارة والرطوبة"), "SENSOR", "primary")
  );
  if (userIndex < WEB_USER_COUNT && WEB_USERS[userIndex].role >= WEB_ROLE_ADMIN) {
    telegramKeyboardAddRow2(k,
      telegramButtonJson(telegramText(chatId, "🔵 Users Management", "🔵 إدارة المستخدمين"), "USERS", "primary"),
      telegramButtonJson(telegramText(chatId, "🔐 Activation Codes", "🔐 أكواد التفعيل"), "CODES", "primary")
    );
    telegramKeyboardAddRow(k, telegramButtonJson(telegramText(chatId, "🔴 Restart Device", "🔴 إعادة تشغيل الجهاز"), "RESTART", "danger"));
  }
  telegramKeyboardAddRow2(k,
    telegramButtonJson(telegramText(chatId, "🔴 Logout", "🔴 خروج"), "LOGOUT", "danger"),
    telegramButtonJson(telegramText(chatId, "🌐 Language", "🌐 اللغة"), "LANG", "primary")
  );
  telegramKeyboardAddRow(k, telegramButtonJson(telegramText(chatId, "🔵 Help", "🔵 المساعدة"), "HELP", "primary"));
  k += "]}";
  return k;
}

String telegramMainMenuKeyboardJson(byte userIndex) {
  return telegramMainMenuKeyboardJson(userIndex, String(""));
}

void telegramSendLoggedOutMenu(const String& chatId, const String& title) {
  bool ar = telegramChatIsArabic(chatId);
  String msg = title.length() ? title : telegramText(chatId, "Device Telegram Control", "تحكم  فى الجهاز من تيليجرام");
  msg += ar ? "\nاضغط تسجيل الدخول ثم اكتب اسم المستخدم وكلمة المرور.\nتقدر تبعت: /id أو /lang" : "\nPress Login, then type username and password.\nYou can also send: /id or /lang";
  telegramSendToChatWithReplyMarkup(chatId, msg, telegramLoggedOutKeyboardJson(chatId));
}

void telegramSendMainMenu(const String& chatId, byte userIndex, const String& title) {
  if (userIndex >= WEB_USER_COUNT) {
    telegramSendLoggedOutMenu(chatId, title);
    return;
  }
  bool ar = telegramChatIsArabic(chatId);
  String msg = title.length() ? title : telegramText(chatId, "Device Control Menu", "قائمة تحكم فى الجهاز");
  msg += ar ? "\nالمستخدم: " : "\nUser: "; msg += WEB_USERS[userIndex].user;
  msg += ar ? "\nالصلاحية: " : "\nRole: "; msg += telegramRoleTextLang(WEB_USERS[userIndex].role, chatId);
  msg += ar ? "\nاختر من الأزرار بالأسفل." : "\nChoose from buttons below.";
  telegramSendToChatWithReplyMarkup(chatId, msg, telegramMainMenuKeyboardJson(userIndex, chatId));
}

String telegramUsersListText() {
  String msg = "Users Management";
  msg += "\nAdmin only.";
  msg += "\nPassword is hidden for security.";
  msg += "\nCommands:";
  msg += "\n/users";
  msg += "\n/enable username";
  msg += "\n/disable username";
  msg += "\n/setpass username new_password";
  msg += "\n\nUsers:";
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    if (!WEB_USERS[i].user[0]) continue;
    msg += "\n#"; msg += String(i + 1);
    msg += " "; msg += WEB_USERS[i].enabled ? "[Enabled] " : "[Disabled] ";
    msg += WEB_USERS[i].user;
    msg += " / "; msg += telegramRoleText(WEB_USERS[i].role);
    msg += webUserTelegramNotify[i] && webUserTelegramChatId[i][0] ? " / TG Notify ON" : " / TG Notify OFF";
    if (i == 0) msg += " / main admin";
  }
  return msg;
}

String telegramUsersKeyboardJson() {
  String k = "{\"inline_keyboard\":[";
  bool any = false;
  for (byte i = 1; i < WEB_USER_COUNT; i++) {
    if (!WEB_USERS[i].user[0]) continue;
    String name = String(WEB_USERS[i].user);
    if (name.length() > 18) name = name.substring(0, 18);
    String label = String(WEB_USERS[i].enabled ? "Disable " : "Enable ") + name;
    telegramKeyboardAddRow(k, telegramButtonJson(label, String("USERTOG:") + String(i)));
    any = true;
  }
  if (!any) telegramKeyboardAddRow(k, telegramButtonJson("No other users", "USERS"));
  telegramKeyboardAddRow2(k, telegramButtonJson("Refresh", "USERS"), telegramButtonJson("Back", "MENU"));
  k += "]}";
  return k;
}

void telegramSendUsersMenu(const String& chatId, const String& title) {
  String msg = title.length() ? title : telegramUsersListText();
  if (title.length()) {
    msg += "\n\n";
    msg += telegramUsersListText();
  }
  telegramSendToChatWithReplyMarkup(chatId, msg, telegramUsersKeyboardJson());
}

void telegramStartLogin(const String& chatId, int slot) {
  if (slot < 0) return;
  telegramSessions[slot].loggedIn = false;
  telegramSessions[slot].userIndex = 255;
  telegramSessions[slot].pendingUser[0] = '\0';
  telegramSessions[slot].stage = TG_STAGE_WAIT_USER;
  String forceReply = "{\"force_reply\":true,\"input_field_placeholder\":\"Username\"}";
  telegramSendToChatWithReplyMarkup(chatId, telegramText(chatId, "Login step 1/2\nType the web username:", "تسجيل الدخول 1/2\nاكتب اسم المستخدم:"), forceReply);
}

String telegramStatusText(const String& title) {
  String msg;
  if (title.length()) {
    msg += title;
    msg += "\n";
  }
  msg += "Time: "; msg += currentDeviceDateTimeText();
  msg += "\nClock: "; msg += clockStateText();
  msg += "\nRTC: "; msg += rtcStatusText();
  msg += "\n"; msg += fieldLabel(1); msg += ": "; msg += currentOutput1State ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(2); msg += ": "; msg += currentOutput2State ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(5); msg += ": "; msg += currentOutput5State ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(6); msg += ": "; msg += currentOutput6State ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(7); msg += ": "; msg += readInput7State() ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(8); msg += ": "; msg += readInput8State() ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(9); msg += ": "; msg += readInput9State() ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(10); msg += ": "; msg += readInput10State() ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(3); msg += ": "; msg += smartOutputState ? "ON" : "OFF";
  msg += "\nTemp: "; msg += isnan(currentTemperature) ? String("--") : String(currentTemperature, 1) + " C";
  msg += "\nHum: "; msg += isnan(currentHumidity) ? String("--") : String(currentHumidity, 1) + " %";
  msg += "\nIP: "; msg += WiFi.localIP().toString();
  msg += "\nRSSI: "; msg += (WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String("offline"));
  msg += "\nLast Command: "; msg += lastCommandText;
  msg += "\nCommand Date: "; msg += lastCommandTime;
  return msg;
}

String telegramStatusTextForUserIndex(const String& title, byte userIndex) {
  if (userIndex >= WEB_USER_COUNT || WEB_USERS[userIndex].role >= WEB_ROLE_ADMIN) return telegramStatusText(title);
  String msg;
  if (title.length()) { msg += title; msg += "\n"; }
  msg += "Time: "; msg += currentDeviceDateTimeText();
  msg += "\nClock: "; msg += clockStateText();
  if (canUserSeeField(userIndex, 1)) { msg += "\n"; msg += fieldLabel(1); msg += ": "; msg += currentOutput1State ? "ON" : "OFF"; }
  if (canUserSeeField(userIndex, 2)) { msg += "\n"; msg += fieldLabel(2); msg += ": "; msg += currentOutput2State ? "ON" : "OFF"; }
  if (canUserSeeField(userIndex, 5)) { msg += "\n"; msg += fieldLabel(5); msg += ": "; msg += currentOutput5State ? "ON" : "OFF"; }
  if (canUserSeeField(userIndex, 6)) { msg += "\n"; msg += fieldLabel(6); msg += ": "; msg += currentOutput6State ? "ON" : "OFF"; }
  if (canUserSeeField(userIndex, 7)) { msg += "\n"; msg += fieldLabel(7); msg += ": "; msg += readInput7State() ? "ON" : "OFF"; }
  if (canUserSeeField(userIndex, 8)) { msg += "\n"; msg += fieldLabel(8); msg += ": "; msg += readInput8State() ? "ON" : "OFF"; }
  if (canUserSeeField(userIndex, 9)) { msg += "\n"; msg += fieldLabel(9); msg += ": "; msg += readInput9State() ? "ON" : "OFF"; }
  if (canUserSeeField(userIndex, 10)) { msg += "\n"; msg += fieldLabel(10); msg += ": "; msg += readInput10State() ? "ON" : "OFF"; }
  msg += "\nTemp: "; msg += isnan(currentTemperature) ? String("--") : String(currentTemperature, 1) + " C";
  msg += "\nHum: "; msg += isnan(currentHumidity) ? String("--") : String(currentHumidity, 1) + " %";
  return msg;
}

String telegramHelpText() {
  String msg = "Device Telegram Control\n";
  msg += "New button mode:\n";
  msg += "1) Send /start\n";
  msg += "2) Press Login\n";
  msg += "3) Type web username, then password\n";
  msg += "4) Press the output toggle button. It shows the current state and changes after every press.\n\n";
  msg += "Text command mode still works:\n";
  msg += "username password command\n\n";
  msg += "Examples:\n";
  msg += "admin admin /status\n";
  msg += "operator1 1234 /o1on\n";
  msg += "operator1 1234 /o1off\n";
  msg += "operator1 1234 /o2on\n";
  msg += "operator1 1234 /o2off\n";
  msg += "operator1 1234 /o5on\n";
  msg += "operator1 1234 /o5off\n";
  msg += "operator1 1234 /o6on\n";
  msg += "operator1 1234 /o6off\n";
  msg += "operator1 1234 output5 on\n";
  msg += "operator1 1234 field6 off\n\n";
  msg += "After Login you may also send commands without username/password:\n";
  msg += "/status, /o1on, /o1off, /o2on, /o2off, /o5on, /o5off, /o6on, /o6off\n\n";
  msg += "Other commands:\n";
  msg += "/myid, /id, /start, /lang, /login, /logout, /help\n";
  msg += "Admin commands after login: /users, /enable username, /disable username, /setpass username new_password, /restart\n";
  msg += "Output control needs Operator, Engineer or Admin role and field permission from the web Users page.";
  return msg;
}

String telegramExtractJsonString(const String& src, const char* marker) {
  int p = src.indexOf(marker);
  if (p < 0) return "";
  p += strlen(marker);
  String out;
  bool esc = false;
  for (int i = p; i < (int)src.length(); i++) {
    char c = src[i];
    if (esc) {
      if (c == 'n') out += '\n';
      else if (c == 'r') out += '\r';
      else if (c == 't') out += '\t';
      else out += c;
      esc = false;
    } else if (c == '\\') {
      esc = true;
    } else if (c == '"') {
      break;
    } else {
      out += c;
    }
  }
  return out;
}

// Telegram ForceReply updates include the bot question inside reply_to_message before
// the user's actual typed text. Reading the first "text" field can therefore read
// "Login step..." instead of the username/password. For normal message handling,
// use the last text value in the update block, which is the user's message text.
String telegramExtractLastJsonString(const String& src, const char* marker) {
  int p = -1;
  int from = 0;
  while (true) {
    int found = src.indexOf(marker, from);
    if (found < 0) break;
    p = found;
    from = found + 1;
  }
  if (p < 0) return "";
  p += strlen(marker);
  String out;
  bool esc = false;
  for (int i = p; i < (int)src.length(); i++) {
    char c = src[i];
    if (esc) {
      if (c == 'n') out += '\n';
      else if (c == 'r') out += '\r';
      else if (c == 't') out += '\t';
      else out += c;
      esc = false;
    } else if (c == '\\') {
      esc = true;
    } else if (c == '"') {
      break;
    } else {
      out += c;
    }
  }
  return out;
}

String telegramExtractCallbackId(const String& src) {
  int q = src.indexOf("\"callback_query\"");
  if (q < 0) return "";
  return telegramExtractJsonString(src.substring(q), "\"id\":\"");
}

String telegramExtractCallbackMessageId(const String& src) {
  int q = src.indexOf("\"callback_query\"");
  if (q < 0) return "";
  int m = src.indexOf("\"message\"", q);
  if (m < 0) return "";
  int p = src.indexOf("\"message_id\":", m);
  if (p < 0) return "";
  p += 13;
  while (p < (int)src.length() && (src[p] == ' ' || src[p] == '\t')) p++;
  String id;
  while (p < (int)src.length()) {
    char c = src[p];
    if (c >= '0' && c <= '9') id += c;
    else break;
    p++;
  }
  return id;
}

String telegramExtractChatId(const String& src) {
  int chatPos = src.indexOf("\"chat\"");
  if (chatPos < 0) return "";
  int idPos = src.indexOf("\"id\":", chatPos);
  if (idPos < 0) return "";
  int i = idPos + 5;
  while (i < (int)src.length() && (src[i] == ' ' || src[i] == '\t')) i++;
  String id;
  while (i < (int)src.length()) {
    char c = src[i];
    if ((c >= '0' && c <= '9') || c == '-') id += c;
    else break;
    i++;
  }
  return id;
}

long telegramExtractUpdateIdAt(const String& src, int pos) {
  int p = src.indexOf("\"update_id\":", pos);
  if (p < 0) return -1;
  p += 12;
  while (p < (int)src.length() && (src[p] == ' ' || src[p] == '\t')) p++;
  String id;
  while (p < (int)src.length()) {
    char c = src[p];
    if (c >= '0' && c <= '9') id += c;
    else break;
    p++;
  }
  return id.length() ? id.toInt() : -1;
}

bool telegramParseAuthenticatedCommand(String text, String& authUser, String& commandText, byte& userRole, byte& userIndex) {
  text.trim();
  text.replace("\r", " ");
  text.replace("\n", " ");
  text.replace("\t", " ");
  text.replace(",", " ");
  text.replace("|", " ");

  while (text.indexOf("  ") >= 0) text.replace("  ", " ");
  text.trim();

  String upper = text;
  upper.toUpperCase();
  if (upper.startsWith("/CMD ")) text = text.substring(5);
  else if (upper.startsWith("CMD ")) text = text.substring(4);
  else if (upper.startsWith("/CONTROL ")) text = text.substring(9);
  else if (upper.startsWith("CONTROL ")) text = text.substring(8);
  text.trim();

  int p1 = text.indexOf(' ');
  if (p1 <= 0) return false;
  int p2 = text.indexOf(' ', p1 + 1);
  if (p2 <= p1 + 1) return false;

  String u = text.substring(0, p1);
  String p = text.substring(p1 + 1, p2);
  String c = text.substring(p2 + 1);
  u.trim();
  p.trim();
  c.trim();
  if (!u.length() || !p.length() || !c.length()) return false;

  byte idx = 255;
  if (!findWebUser(u, p, idx)) return false;
  if (idx >= WEB_USER_COUNT || !WEB_USERS[idx].enabled) return false;

  authUser = String(WEB_USERS[idx].user);
  userRole = WEB_USERS[idx].role;
  userIndex = idx;
  commandText = c;
  return true;
}

bool telegramControlOutputForUser(const String& chatId, byte authIndex, byte out, byte state, bool showMenuAfter) {
  if (authIndex >= WEB_USER_COUNT) return false;
  String authUser = String(WEB_USERS[authIndex].user);
  byte authRole = WEB_USERS[authIndex].role;

  if (authRole < WEB_ROLE_OPERATOR || !canUserControlField(authIndex, out)) {
    telegramSendToChat(chatId, telegramText(chatId, "Access denied for user: ", "غير مسموح للمستخدم: ") + authUser + telegramText(chatId, "\nYou are not allowed to control ", "\nغير مسموح لك بالتحكم في ") + fieldLabel(out) + ".");
    addEsp32Log(String("Telegram denied: ") + authUser + " field permission");
    return false;
  }

  String source = String("Telegram(") + authUser + ")";
  bool changed = setOutputFromWeb(out, state, source.c_str());
  String msg;
  if (changed) msg = telegramText(chatId, "OK. ", "تم. ") + fieldLabel(out) + telegramText(chatId, " is now ", " الآن ") + telegramStateText(chatId, state);
  else msg = telegramText(chatId, "No change. ", "لا يوجد تغيير. ") + fieldLabel(out) + telegramText(chatId, " already ", " بالفعل ") + telegramStateText(chatId, state);

  if (showMenuAfter) telegramSendMainMenu(chatId, authIndex, msg);
  else telegramSendToChat(chatId, msg);
  return true;
}

bool telegramToggleOutputForUserAndEditMenu(const String& chatId, const String& messageId, byte authIndex, byte out) {
  if (authIndex >= WEB_USER_COUNT) return false;
  String authUser = String(WEB_USERS[authIndex].user);
  byte authRole = WEB_USERS[authIndex].role;

  if (authRole < WEB_ROLE_OPERATOR || !canUserControlField(authIndex, out)) {
    telegramSendToChat(chatId, telegramText(chatId, "Access denied for user: ", "غير مسموح للمستخدم: ") + authUser + telegramText(chatId, "\nYou are not allowed to control ", "\nغير مسموح لك بالتحكم في ") + fieldLabel(out) + ".");
    addEsp32Log(String("Telegram denied: ") + authUser + " field permission");
    return false;
  }

  byte newState = telegramOutputState(out) ? 0 : 1;
  String source = String("Telegram(") + authUser + ")";
  bool changed = setOutputFromWeb(out, newState, source.c_str());
  String msg;
  if (changed) msg = telegramText(chatId, "OK. ", "تم. ") + fieldLabel(out) + telegramText(chatId, " is now ", " الآن ") + telegramStateText(chatId, newState);
  else msg = telegramText(chatId, "No change. ", "لا يوجد تغيير. ") + fieldLabel(out) + telegramText(chatId, " already ", " بالفعل ") + telegramStateText(chatId, newState);

  bool ar = telegramChatIsArabic(chatId);
  String menuText = msg;
  menuText += ar ? "\nالمستخدم: " : "\nUser: "; menuText += WEB_USERS[authIndex].user;
  menuText += ar ? "\nالصلاحية: " : "\nRole: "; menuText += telegramRoleTextLang(WEB_USERS[authIndex].role, chatId);
  menuText += ar ? "\nاختر من الأزرار بالأسفل." : "\nChoose from buttons below.";

  if (messageId.length()) telegramEditMessageTextWithReplyMarkup(chatId, messageId, menuText, telegramMainMenuKeyboardJson(authIndex, chatId));
  else telegramSendMainMenu(chatId, authIndex, msg);
  return true;
}

void telegramRestartEsp32FromTelegram(const String& chatId, byte authIndex) {
  if (authIndex >= WEB_USER_COUNT || WEB_USERS[authIndex].role < WEB_ROLE_ADMIN) {
    telegramSendToChat(chatId, "Access denied. Admin only.");
    return;
  }

  String authUser = String(WEB_USERS[authIndex].user);
  addEsp32Log(String("Telegram restart requested by ") + authUser);

  lastCommandMs = millis();
  String ts = currentDeviceDateTimeText();
  if (ts.length() == 0) ts = "--";
  ts.substring(0, sizeof(lastCommandTime) - 1).toCharArray(lastCommandTime, sizeof(lastCommandTime));
  authUser.substring(0, sizeof(lastCommandUser) - 1).toCharArray(lastCommandUser, sizeof(lastCommandUser));
  String srcBase = "Telegram";
  srcBase.toCharArray(lastCommandSource, sizeof(lastCommandSource));
  String txt = String("Telegram restart by ") + authUser;
  txt.substring(0, sizeof(lastCommandText) - 1).toCharArray(lastCommandText, sizeof(lastCommandText));

  telegramSendToChat(chatId, String("Restarting Device now...\nUser: ") + authUser);
  delay(900);
  ESP.restart();
}

void telegramHandleAuthorizedCommand(const String& chatId, byte authIndex, String cmdText) {
  maintainMonthlyUserSubscriptionDisable(true);

  if (authIndex >= WEB_USER_COUNT) {
    telegramSendLoggedOutMenu(chatId, telegramText(chatId, "Please login first.", "سجل الدخول أولاً."));
    return;
  }
  String authUser = String(WEB_USERS[authIndex].user);
  byte authRole = WEB_USERS[authIndex].role;

  if (!WEB_USERS[authIndex].enabled) {
    int s = telegramSessionIndexForChat(chatId, false);
    if (s >= 0) telegramLogoutSession(s);
    telegramSendLoggedOutMenu(chatId, subscriptionDisabledTextTelegram(chatId, authUser));
    return;
  }

  if (mustChangeAdminPassword() && authUser.equalsIgnoreCase(String(WEB_USERS[0].user))) {
    telegramSendToChat(chatId, "Telegram blocked for security. Change default admin password from User Management first.");
    addEsp32Log("Telegram command blocked: default admin password");
    return;
  }

  String cmd = cmdText;
  cmd.trim();
  cmd.toUpperCase();

  // Remove Telegram bot username suffix, e.g. /status@MyBot.
  int at = cmd.indexOf('@');
  if (at >= 0) {
    int sp = cmd.indexOf(' ', at);
    if (sp < 0) cmd = cmd.substring(0, at);
    else cmd = cmd.substring(0, at) + cmd.substring(sp);
  }

  String compact = cmd;
  compact.replace(" ", "");
  compact.replace("\r", "");
  compact.replace("\n", "");
  compact.replace("\t", "");
  compact.replace("/", "");
  compact.replace("_", "");
  compact.replace("-", "");

  if (compact == "START" || compact == "MENU") {
    telegramSendMainMenu(chatId, authIndex, "Device Control Menu");
    return;
  }

  if (compact == "HELP") {
    telegramSendToChatWithReplyMarkup(chatId, telegramHelpText(), telegramMainMenuKeyboardJson(authIndex, chatId));
    return;
  }

  if (compact == "STATUS" || compact == "STATE" || compact == "INFO") {
    telegramSendToChatWithReplyMarkup(chatId, telegramStatusTextForUserIndexLang(chatId, telegramText(chatId, "Device Status - User: ", "حالة الجهاز - المستخدم: ") + authUser, authIndex), telegramMainMenuKeyboardJson(authIndex, chatId));
    return;
  }

  if (compact == "RESTART" || compact == "REBOOT" || compact == "ESP32RESTART" || compact == "ESP32REBOOT") {
    telegramRestartEsp32FromTelegram(chatId, authIndex);
    return;
  }

  if (compact == "CODES" || compact == "ACTIVATIONCODES" || compact == "SHOWCODES") {
    if (authRole < WEB_ROLE_ADMIN) { telegramSendToChat(chatId, "Access denied. Admin only."); return; }
    telegramSendActivationCodesMenu(chatId, "");
    return;
  }

  if (compact == "GENCODE" || compact == "GENERATECODE" || compact == "ACTIVATIONCODE" || compact == "GENCODES5" || compact == "GENCODES10") {
    if (authRole < WEB_ROLE_ADMIN) {
      telegramSendToChat(chatId, "Access denied. Admin only.");
      return;
    }
    byte n = compact.endsWith("10") ? 10 : (compact.endsWith("5") ? 5 : 1);
    telegramSendToChatWithReplyMarkup(chatId, telegramGenerateActivationCodesText(chatId, n), telegramActivationCodesKeyboardJson(chatId));
    return;
  }

  String cmdNorm = cmdText;
  cmdNorm.trim();
  cmdNorm.replace("\r", " ");
  cmdNorm.replace("\n", " ");
  cmdNorm.replace("\t", " ");
  while (cmdNorm.indexOf("  ") >= 0) cmdNorm.replace("  ", " ");
  String cmdUpperNorm = cmdNorm;
  cmdUpperNorm.toUpperCase();

  if (compact == "USERS" || compact == "USERSTATUS" || compact == "USERSTATE") {
    if (authRole < WEB_ROLE_ADMIN) {
      telegramSendToChat(chatId, "Access denied. Admin only.");
      return;
    }
    telegramSendUsersMenu(chatId, "");
    return;
  }

  if (cmdUpperNorm.startsWith("/ENABLE ") || cmdUpperNorm.startsWith("ENABLE ") || cmdUpperNorm.startsWith("/DISABLE ") || cmdUpperNorm.startsWith("DISABLE ")) {
    if (authRole < WEB_ROLE_ADMIN) {
      telegramSendToChat(chatId, "Access denied. Admin only.");
      return;
    }
    bool en = cmdUpperNorm.startsWith("/ENABLE ") || cmdUpperNorm.startsWith("ENABLE ");
    int sp = cmdNorm.indexOf(' ');
    String target = (sp >= 0) ? cmdNorm.substring(sp + 1) : String("");
    target.trim();
    byte ti = findWebUserIndexByName(target);
    if (ti == 0) {
      telegramSendUsersMenu(chatId, "Main admin cannot be disabled.");
      return;
    }
    if (ti >= WEB_USER_COUNT) {
      telegramSendUsersMenu(chatId, String("User not found: ") + target);
      return;
    }
    setWebUserEnabledByIndex(ti, en);
    telegramSendUsersMenu(chatId, String(WEB_USERS[ti].user) + (en ? " enabled." : " disabled."));
    return;
  }

  if (cmdUpperNorm.startsWith("/SETPASS ") || cmdUpperNorm.startsWith("SETPASS ") || cmdUpperNorm.startsWith("/PASS ") || cmdUpperNorm.startsWith("PASS ")) {
    if (authRole < WEB_ROLE_ADMIN) {
      telegramSendToChat(chatId, "Access denied. Admin only.");
      return;
    }
    int sp1 = cmdNorm.indexOf(' ');
    int sp2 = (sp1 >= 0) ? cmdNorm.indexOf(' ', sp1 + 1) : -1;
    if (sp1 < 0 || sp2 < 0) {
      telegramSendToChat(chatId, "Wrong format. Use: /setpass username new_password");
      return;
    }
    String target = cmdNorm.substring(sp1 + 1, sp2);
    String newPass = cmdNorm.substring(sp2 + 1);
    target.trim();
    newPass.trim();
    byte ti = findWebUserIndexByName(target);
    if (ti >= WEB_USER_COUNT) {
      telegramSendUsersMenu(chatId, String("User not found: ") + target);
      return;
    }
    if (!isSafePassword(newPass)) {
      telegramSendToChat(chatId, String("Invalid password. Use 1 to ") + String(WEB_PASS_MAX_LEN) + " visible ASCII characters, no spaces.");
      return;
    }
    if (!setWebUserPasswordByIndex(ti, newPass)) {
      telegramSendToChat(chatId, "Password change failed.");
      return;
    }
    telegramSendUsersMenu(chatId, String("Password changed successfully for user: ") + WEB_USERS[ti].user);
    return;
  }

  byte out = 0;
  int state = -1;

  if (compact == "O1ON" || compact == "OUTPUT1ON" || compact == "1ON" || compact == "PUMPON") { out = 1; state = 1; }
  else if (compact == "O1OFF" || compact == "OUTPUT1OFF" || compact == "1OFF" || compact == "PUMPOFF") { out = 1; state = 0; }
  else if (compact == "O2ON" || compact == "OUTPUT2ON" || compact == "2ON" || compact == "LAMPON" || compact == "LIGHTON") { out = 2; state = 1; }
  else if (compact == "O2OFF" || compact == "OUTPUT2OFF" || compact == "2OFF" || compact == "LAMPOFF" || compact == "LIGHTOFF") { out = 2; state = 0; }
  else if (compact == "O5ON" || compact == "OUTPUT5ON" || compact == "FIELD5ON" || compact == "5ON") { out = 5; state = 1; }
  else if (compact == "O5OFF" || compact == "OUTPUT5OFF" || compact == "FIELD5OFF" || compact == "5OFF") { out = 5; state = 0; }
  else if (compact == "O6ON" || compact == "OUTPUT6ON" || compact == "FIELD6ON" || compact == "6ON") { out = 6; state = 1; }
  else if (compact == "O6OFF" || compact == "OUTPUT6OFF" || compact == "FIELD6OFF" || compact == "6OFF") { out = 6; state = 0; }

  if (out == 0) {
    telegramSendToChatWithReplyMarkup(chatId, String("Unknown command: ") + cmdText + "\n\n" + telegramHelpText(), telegramMainMenuKeyboardJson(authIndex, chatId));
    return;
  }

  telegramControlOutputForUser(chatId, authIndex, out, (byte)state, true);
}

void telegramHandleLoginText(const String& chatId, int slot, String text) {
  if (slot < 0 || slot >= TELEGRAM_SESSION_COUNT) return;
  text.trim();

  if (telegramSessions[slot].stage == TG_STAGE_WAIT_USER) {
    text.substring(0, sizeof(telegramSessions[slot].pendingUser) - 1).toCharArray(telegramSessions[slot].pendingUser, sizeof(telegramSessions[slot].pendingUser));
    telegramSessions[slot].stage = TG_STAGE_WAIT_PASS;
    String forceReply = "{\"force_reply\":true,\"input_field_placeholder\":\"Password\"}";
    telegramSendToChatWithReplyMarkup(chatId, String("Login step 2/2\nUsername: ") + telegramSessions[slot].pendingUser + "\nType the password:", forceReply);
    return;
  }

  if (telegramSessions[slot].stage == TG_STAGE_WAIT_PASS) {
    maintainMonthlyUserSubscriptionDisable(true);

    byte idx = 255;
    String u = String(telegramSessions[slot].pendingUser);
    if (findWebUser(u, text, idx) && idx < WEB_USER_COUNT && WEB_USERS[idx].enabled) {
      if (mustChangeAdminPassword() && String(WEB_USERS[idx].user).equalsIgnoreCase(String(WEB_USERS[0].user))) {
        telegramLogoutSession(slot);
        telegramSendLoggedOutMenu(chatId, "Security block: change default admin/admin from User Management first.");
        addEsp32Log("Telegram login blocked: default admin password");
        return;
      }
      telegramSessions[slot].loggedIn = true;
      telegramSessions[slot].userIndex = idx;
      telegramSessions[slot].stage = TG_STAGE_IDLE;
      telegramSessions[slot].pendingUser[0] = '\0';
      addEsp32Log(String("Telegram login OK: ") + WEB_USERS[idx].user + " chat " + chatId);

      byte daysLeft = 0;
      if (idx > 0 && subscriptionWarningActive(daysLeft)) {
        telegramSendToChat(chatId, subscriptionWarningTextTelegram(chatId, String(WEB_USERS[idx].user), daysLeft));
      }
      telegramSendMainMenu(chatId, idx, telegramText(chatId, "Login successful.", "تم تسجيل الدخول."));
      return;
    }

    byte disabledIdx = 255;
    if (findWebUserAnyState(u, text, disabledIdx) && disabledIdx < WEB_USER_COUNT && !WEB_USERS[disabledIdx].enabled) {
      telegramLogoutSession(slot);
      addEsp32Log(String("Telegram login disabled user: ") + WEB_USERS[disabledIdx].user);
      telegramSendLoggedOutMenu(chatId, subscriptionDisabledTextTelegram(chatId, String(WEB_USERS[disabledIdx].user)));
      return;
    }

    telegramLogoutSession(slot);
    addEsp32Log(String("Telegram login failed from chat ") + chatId);
    telegramSendLoggedOutMenu(chatId, telegramText(chatId, "Login failed. Wrong username or password.", "فشل تسجيل الدخول. اسم المستخدم أو كلمة المرور خطأ."));
  }
}

void handleTelegramCallback(const String& chatId, const String& callbackId, const String& messageId, String data) {
  telegramMarkActivePolling();
  maintainMonthlyUserSubscriptionDisable(true);

  data.trim();
  String compact = data;
  compact.toUpperCase();

  int slot = telegramSessionIndexForChat(chatId, true);
  if (callbackId.length()) telegramAnswerCallback(callbackId, "OK");

  if (compact == "ID") {
    telegramSendToChat(chatId, telegramText(chatId, "Your Telegram Chat ID is:\n", "رقم Telegram Chat ID الخاص بك:\n") + chatId);
    return;
  }

  if (compact == "LOGIN") {
    telegramStartLogin(chatId, slot);
    return;
  }

  if (compact == "LOGOUT") {
    telegramLogoutSession(slot);
    telegramSendLoggedOutMenu(chatId, telegramText(chatId, "Logged out.", "تم تسجيل الخروج."));
    return;
  }

  if (compact == "HELP") {
    if (telegramSessionLoggedIn(slot)) telegramSendToChatWithReplyMarkup(chatId, telegramHelpText(), telegramMainMenuKeyboardJson(telegramSessions[slot].userIndex, chatId));
    else telegramSendToChatWithReplyMarkup(chatId, telegramHelpText(), telegramLoggedOutKeyboardJson(chatId));
    return;
  }

  if (compact == "LANG") {
    telegramSendLanguageMenu(chatId);
    return;
  }

  if (compact == "LANG:AR" || compact == "LANG:EN") {
    bool arSel = compact.endsWith("AR");
    byte selectedLang = arSel ? TG_LANG_AR : TG_LANG_EN;
    telegramSetLanguageForChat(chatId, selectedLang);
    telegramRuntimeLangChatId = chatId;
    telegramRuntimeLang = selectedLang;
    telegramRuntimeLangUntilMs = millis() + 86400000UL;
    int langSlot = telegramSessionIndexForChat(chatId, true);
    if (telegramSessionLoggedIn(langSlot)) telegramSendMainMenu(chatId, telegramSessions[langSlot].userIndex, arSel ? "تم اختيار اللغة العربية." : "English selected.");
    else telegramSendLoggedOutMenu(chatId, arSel ? "تم اختيار اللغة العربية." : "English selected.");
    return;
  }

  byte oldIdx = (slot >= 0 && slot < TELEGRAM_SESSION_COUNT) ? telegramSessions[slot].userIndex : 255;
  bool oldLogged = (slot >= 0 && slot < TELEGRAM_SESSION_COUNT) ? telegramSessions[slot].loggedIn : false;
  if (!telegramSessionLoggedIn(slot)) {
    if (oldLogged && oldIdx < WEB_USER_COUNT && !WEB_USERS[oldIdx].enabled) {
      telegramSendLoggedOutMenu(chatId, subscriptionDisabledTextTelegram(chatId, String(WEB_USERS[oldIdx].user)));
    } else {
      telegramSendLoggedOutMenu(chatId, telegramText(chatId, "Please login first.", "سجل الدخول أولاً."));
    }
    return;
  }

  byte idx = telegramSessions[slot].userIndex;

  if (compact == "MENU") {
    telegramSendMainMenu(chatId, idx, telegramText(chatId, "Device Control Menu", "قائمة تحكم فى الجهاز"));
    return;
  }

  if (compact == "STATUS") {
    telegramSendToChatWithReplyMarkup(chatId, telegramStatusTextForUserIndexLang(chatId, telegramText(chatId, "Device Status - User: ", "حالة الجهاز - المستخدم: ") + WEB_USERS[idx].user, idx), telegramMainMenuKeyboardJson(idx, chatId));
    return;
  }

  if (compact == "SENSOR") {
    telegramSendToChatWithReplyMarkup(chatId, telegramSensorTextForChat(chatId), telegramMainMenuKeyboardJson(idx, chatId));
    return;
  }

  if (compact == "RESTART") {
    telegramRestartEsp32FromTelegram(chatId, idx);
    return;
  }

  if (compact == "USERS") {
    if (WEB_USERS[idx].role < WEB_ROLE_ADMIN) {
      telegramSendMainMenu(chatId, idx, "Access denied. Admin only.");
      return;
    }
    telegramSendUsersMenu(chatId, "");
    return;
  }

  if (compact == "CODES" || compact == "CODESHOW") {
    if (WEB_USERS[idx].role < WEB_ROLE_ADMIN) {
      telegramSendMainMenu(chatId, idx, "Access denied. Admin only.");
      return;
    }
    telegramSendActivationCodesMenu(chatId, "");
    return;
  }

  if (compact == "GENCODES5" || compact == "GENCODES10") {
    if (WEB_USERS[idx].role < WEB_ROLE_ADMIN) {
      telegramSendMainMenu(chatId, idx, "Access denied. Admin only.");
      return;
    }
    byte n = (compact == "GENCODES10") ? 10 : 5;
    telegramSendToChatWithReplyMarkup(chatId, telegramGenerateActivationCodesText(chatId, n), telegramActivationCodesKeyboardJson(chatId));
    return;
  }

  if (compact.startsWith("USERTOG:")) {
    if (WEB_USERS[idx].role < WEB_ROLE_ADMIN) {
      telegramSendMainMenu(chatId, idx, "Access denied. Admin only.");
      return;
    }
    byte targetIdx = (byte)compact.substring(8).toInt();
    if (targetIdx == 0) {
      telegramSendUsersMenu(chatId, "Main admin cannot be disabled.");
      return;
    }
    if (targetIdx >= WEB_USER_COUNT || !WEB_USERS[targetIdx].user[0]) {
      telegramSendUsersMenu(chatId, "User not found.");
      return;
    }
    bool newState = !WEB_USERS[targetIdx].enabled;
    setWebUserEnabledByIndex(targetIdx, newState);
    telegramSendUsersMenu(chatId, String(WEB_USERS[targetIdx].user) + (newState ? " enabled." : " disabled."));
    return;
  }

  if (compact.startsWith("TOGGLE:")) {
    byte out = (byte)compact.substring(7).toInt();
    if (out == 1 || out == 2 || out == 5 || out == 6) {
      telegramToggleOutputForUserAndEditMenu(chatId, messageId, idx, out);
      return;
    }
  }

  if (compact.startsWith("OUT:")) {
    int p1 = compact.indexOf(':');
    int p2 = compact.indexOf(':', p1 + 1);
    if (p1 > 0 && p2 > p1) {
      byte out = (byte)compact.substring(p1 + 1, p2).toInt();
      byte state = (byte)compact.substring(p2 + 1).toInt();
      if ((out == 1 || out == 2 || out == 5 || out == 6) && (state == 0 || state == 1)) {
        telegramControlOutputForUser(chatId, idx, out, state, true);
        return;
      }
    }
  }

  telegramSendMainMenu(chatId, idx, telegramText(chatId, "Unknown button.", "زر غير معروف."));
}


void telegramHandleActivationCode(const String& chatId, const String& codeIn) {
  String code = codeIn;
  code.trim();
  byte idx = findWebUserByTelegramChatId(chatId);
  bool ar = telegramChatIsArabic(chatId);

  if (idx == 255 || idx >= WEB_USER_COUNT || !WEB_USERS[idx].user[0] || idx == 0) {
    telegramSendToChat(chatId, telegramText(chatId,
      "This Telegram Chat ID is not linked to a normal user. Send your Chat ID to admin:",
      "رقم تيليجرام هذا غير مربوط بيوزر عادي. أرسل هذا الرقم للأدمن:" ) + "\n" + chatId);
    return;
  }

  if (WEB_USERS[idx].enabled) {
    telegramSendToChat(chatId, telegramText(chatId, "Your user is already enabled.", "اليوزر الخاص بك مفعل بالفعل."));
    return;
  }

  String attemptKey = activationAttemptKey("T", chatId);
  String lockMsg;
  if (activationAttemptIsLocked(attemptKey, lockMsg, ar)) {
    telegramSendToChat(chatId, lockMsg);
    return;
  }

  if (!consumeActivationCode(code, String(WEB_USERS[idx].user), String("Telegram"))) {
    telegramSendToChat(chatId, activationAttemptFailureMessage(attemptKey, ar));
    return;
  }

  activationAttemptClear(attemptKey);
  setWebUserEnabledByIndex(idx, true);
  telegramSendLoggedOutMenu(chatId, telegramText(chatId,
    "User activated successfully. You can login now.",
    "تم تفعيل اليوزر بنجاح. يمكنك تسجيل الدخول الآن."));
}

void handleTelegramCommand(const String& chatId, String text) {
  telegramMarkActivePolling();
  maintainMonthlyUserSubscriptionDisable(true);

  text.trim();
  if (!text.length()) return;

  int slot = telegramSessionIndexForChat(chatId, true);

  String raw = text;
  String rawCompact = raw;
  rawCompact.trim();
  rawCompact.toUpperCase();
  rawCompact.replace(" ", "");
  rawCompact.replace("\r", "");
  rawCompact.replace("\n", "");
  rawCompact.replace("\t", "");

  if (rawCompact == "/ID" || rawCompact == "ID" || rawCompact == "MYID" || rawCompact == "/MYID" || rawCompact == "CHATID" || rawCompact == "/CHATID") {
    telegramSendToChat(chatId, telegramText(chatId, "Your Telegram Chat ID is:\n", "رقم Telegram Chat ID الخاص بك:\n") + chatId);
    addEsp32Log(String("Telegram /id requested from chat ") + chatId);
    return;
  }

  if (rawCompact == "/LANG" || rawCompact == "LANG" || rawCompact == "/LANGUAGE" || rawCompact == "LANGUAGE" || rawCompact == "اللغة") {
    telegramSendLanguageMenu(chatId);
    return;
  }

  // V1.6.8: also accept typed language choices, not only inline buttons.
  if (rawCompact == "EN" || rawCompact == "ENGLISH" || rawCompact == "🇬🇧ENGLISH") {
    telegramSetLanguageForChat(chatId, TG_LANG_EN);
    if (telegramSessionLoggedIn(slot)) telegramSendMainMenu(chatId, telegramSessions[slot].userIndex, "English selected.");
    else telegramSendLoggedOutMenu(chatId, "English selected.");
    return;
  }
  if (rawCompact == "AR" || rawCompact == "ARABIC" || rawCompact == "عربي" || rawCompact == "العربية" || rawCompact == "🇪🇬العربية") {
    telegramSetLanguageForChat(chatId, TG_LANG_AR);
    if (telegramSessionLoggedIn(slot)) telegramSendMainMenu(chatId, telegramSessions[slot].userIndex, "تم اختيار اللغة العربية.");
    else telegramSendLoggedOutMenu(chatId, "تم اختيار اللغة العربية.");
    return;
  }

  // Easy activation: a disabled user can send the 8-digit activation code only.
  if (activationCodeTextValid(rawCompact)) {
    telegramHandleActivationCode(chatId, rawCompact);
    return;
  }

  {
    String t = text;
    t.trim();
    t.replace("\r", " "); t.replace("\n", " "); t.replace("\t", " ");
    while (t.indexOf("  ") >= 0) t.replace("  ", " ");
    String up = t;
    up.toUpperCase();
    bool actCmd = up.startsWith("/ACTIVATE ") || up.startsWith("ACTIVATE ") || t.startsWith("تفعيل ");
    if (actCmd) {
      int sp = t.indexOf(' ');
      String code = (sp >= 0) ? t.substring(sp + 1) : String("");
      code.trim();
      telegramHandleActivationCode(chatId, code);
      return;
    }
  }

  if (rawCompact == "/START" || rawCompact == "START") {
    telegramSendLanguageMenu(chatId);
    return;
  }

  if (rawCompact == "MENU" || rawCompact == "/MENU") {
    if (telegramSessionLoggedIn(slot)) telegramSendMainMenu(chatId, telegramSessions[slot].userIndex, telegramText(chatId, "Device Control Menu", "قائمة تحكم الجهاز"));
    else {
      telegramLogoutSession(slot);
      telegramSendLoggedOutMenu(chatId, telegramText(chatId, "Device Telegram Control", "تحكم فى الجهاز من تيليجرام"));
    }
    return;
  }

  if (rawCompact == "/LOGIN" || rawCompact == "LOGIN") {
    telegramStartLogin(chatId, slot);
    return;
  }

  if (rawCompact == "/LOGOUT" || rawCompact == "LOGOUT") {
    telegramLogoutSession(slot);
    telegramSendLoggedOutMenu(chatId, telegramText(chatId, "Logged out.", "تم تسجيل الخروج."));
    return;
  }

  if (telegramSessions[slot].stage == TG_STAGE_WAIT_USER || telegramSessions[slot].stage == TG_STAGE_WAIT_PASS) {
    telegramHandleLoginText(chatId, slot, text);
    return;
  }

  String authUser;
  String cmdText;
  byte authRole = WEB_ROLE_VIEWER;
  byte authIndex = 255;

  if (telegramParseAuthenticatedCommand(text, authUser, cmdText, authRole, authIndex)) {
    telegramSessions[slot].loggedIn = true;
    telegramSessions[slot].userIndex = authIndex;
    telegramSessions[slot].stage = TG_STAGE_IDLE;
    telegramSessions[slot].pendingUser[0] = '\0';
    telegramHandleAuthorizedCommand(chatId, authIndex, cmdText);
    return;
  }

  // Direct format: username password command. If the credentials are correct but the user is disabled,
  // show the subscription message instead of a generic login failure.
  {
    String tmp = text;
    tmp.trim();
    tmp.replace("\r", " "); tmp.replace("\n", " "); tmp.replace("\t", " "); tmp.replace(",", " "); tmp.replace("|", " ");
    while (tmp.indexOf("  ") >= 0) tmp.replace("  ", " ");
    int p1 = tmp.indexOf(' ');
    int p2 = (p1 > 0) ? tmp.indexOf(' ', p1 + 1) : -1;
    if (p1 > 0 && p2 > p1 + 1) {
      String du = tmp.substring(0, p1);
      String dp = tmp.substring(p1 + 1, p2);
      byte disabledIdx = 255;
      if (findWebUserAnyState(du, dp, disabledIdx) && disabledIdx < WEB_USER_COUNT && !WEB_USERS[disabledIdx].enabled) {
        telegramLogoutSession(slot);
        telegramSendLoggedOutMenu(chatId, subscriptionDisabledTextTelegram(chatId, String(WEB_USERS[disabledIdx].user)));
        return;
      }
    }
  }

  {
    byte oldIdx = (slot >= 0 && slot < TELEGRAM_SESSION_COUNT) ? telegramSessions[slot].userIndex : 255;
    bool oldLogged = (slot >= 0 && slot < TELEGRAM_SESSION_COUNT) ? telegramSessions[slot].loggedIn : false;
    if (telegramSessionLoggedIn(slot)) {
      telegramHandleAuthorizedCommand(chatId, telegramSessions[slot].userIndex, text);
      return;
    }
    if (oldLogged && oldIdx < WEB_USER_COUNT && !WEB_USERS[oldIdx].enabled) {
      telegramSendLoggedOutMenu(chatId, subscriptionDisabledTextTelegram(chatId, String(WEB_USERS[oldIdx].user)));
      return;
    }
  }

  telegramSendLoggedOutMenu(chatId, String("Access denied or not logged in.\nPress Login or send: username password command\n\n") + telegramHelpText());
  addEsp32Log(String("Telegram command rejected: no login/bad auth from chat ") + chatId);
}

unsigned long telegramCurrentPollIntervalMs() {
  if (!telegramSmartPollingEnabled) return TELEGRAM_COMMAND_POLL_FAST_MS;
  unsigned long now = millis();
  if (telegramSmartActiveUntilMs != 0 && (long)(telegramSmartActiveUntilMs - now) > 0) return TELEGRAM_COMMAND_POLL_SMART_ACTIVE_MS;
  return TELEGRAM_COMMAND_POLL_SMART_IDLE_MS;
}

void telegramMarkActivePolling() {
  if (!telegramSmartPollingEnabled) return;
  telegramSmartActiveUntilMs = millis() + TELEGRAM_COMMAND_POLL_SMART_ACTIVE_WINDOW_MS;
}

void processTelegramUpdates(const String& payload) {
  int pos = 0;
  bool ready = telegramCommandPollReady;
  bool anyUpdate = false;

  while (true) {
    int updPos = payload.indexOf("\"update_id\":", pos);
    if (updPos < 0) break;
    anyUpdate = true;
    long updId = telegramExtractUpdateIdAt(payload, updPos);
    if (updId > telegramLastUpdateId) telegramLastUpdateId = updId;

    int nextUpd = payload.indexOf("\"update_id\":", updPos + 12);
    String block = (nextUpd >= 0) ? payload.substring(updPos, nextUpd) : payload.substring(updPos);
    String chatId = telegramExtractChatId(block);

    if (ready && chatId.length()) {
      if (block.indexOf("\"callback_query\"") >= 0) {
        String callbackId = telegramExtractCallbackId(block);
        String callbackMessageId = telegramExtractCallbackMessageId(block);
        String callbackData = telegramExtractJsonString(block, "\"data\":\"");
        if (callbackData.length()) handleTelegramCallback(chatId, callbackId, callbackMessageId, callbackData);
      } else {
        String text = telegramExtractLastJsonString(block, "\"text\":\"");
        if (text.length()) handleTelegramCommand(chatId, text);
      }
    }

    if (nextUpd < 0) break;
    pos = nextUpd;
  }

  // First successful poll only records the latest update id to avoid executing old Telegram commands after reboot.
  telegramCommandPollReady = true;
}

void checkTelegramCommands() {
  if (!telegramEnabled || !telegramBotToken[0] || WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastTelegramCommandPollMs < telegramCurrentPollIntervalMs()) return;
  lastTelegramCommandPollMs = now;

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + telegramBotToken + "/getUpdates?limit=5&timeout=0";
  if (telegramLastUpdateId > 0) url += String("&offset=") + String(telegramLastUpdateId + 1);

  if (http.begin(secure, url)) {
    http.setTimeout(TELEGRAM_HTTP_TIMEOUT_MS);
    int code = http.GET();
    lastTelegramPollCode = code;
    if (code == HTTP_CODE_OK) {
      processTelegramUpdates(http.getString());
    }
    http.end();
  }
}

void checkTelegramAlerts() {
  bool online = WiFi.status() == WL_CONNECTED;
  if (telegramEnabled && lastWifiOnlineForAlert && !online && millis() - lastTelegramWifiAlertMs > 300000UL) {
    lastTelegramWifiAlertMs = millis();
    telegramSend("ESP32 WiFi disconnected");
    addEsp32Log("Telegram alert: WiFi disconnected");
  }
  lastWifiOnlineForAlert = online;
  if (telegramEnabled && sensorReadOk && !isnan(currentTemperature) && currentTemperature >= telegramHighTempC && millis() - lastTelegramTempAlertMs > 600000UL) {
    lastTelegramTempAlertMs = millis();
    telegramSend(String("High temperature alert: ") + String(currentTemperature,1) + " C");
    addEsp32Log("Telegram alert: high temperature");
  }
}

void publishMqtt(const char* subtopic, const String& payload, bool retained = true) {
  if (!mqttClient.connected()) return;
  String topic = String(mqttBaseTopic) + "/" + subtopic;
  mqttClient.publish(topic.c_str(), payload.c_str(), retained);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t(topic), p;
  for (unsigned int i=0;i<length;i++) p += (char)payload[i];
  p.trim(); p.toUpperCase();
  byte state = (p == "ON" || p == "1" || p == "TRUE") ? 1 : 0;
  if (t.endsWith("/output1/set")) setOutputFromWeb(1, state, "MQTT");
  if (t.endsWith("/output2/set")) setOutputFromWeb(2, state, "MQTT");
  if (t.endsWith("/output5/set")) setOutputFromWeb(5, state, "MQTT");
  if (t.endsWith("/output6/set")) setOutputFromWeb(6, state, "MQTT");
  addEsp32Log(String("MQTT command: ") + t + " = " + p);
  wsBroadcastStatus();
}

void sendHomeAssistantDiscovery() {
  if (!mqttClient.connected() || haDiscoverySent) return;
  String node = String(deviceName[0] ? deviceName : "ahmed_esp32");
  node.replace(" ", "_");
  node.toLowerCase();
  String base = String(mqttDiscoveryPrefix);
  mqttClient.publish((base+"/sensor/"+node+"/temperature/config").c_str(), (String("{\"name\":\"")+node+" Temperature\",\"state_topic\":\""+mqttBaseTopic+"/temperature\",\"unit_of_measurement\":\"C\",\"device_class\":\"temperature\"}").c_str(), true);
  mqttClient.publish((base+"/sensor/"+node+"/humidity/config").c_str(), (String("{\"name\":\"")+node+" Humidity\",\"state_topic\":\""+mqttBaseTopic+"/humidity\",\"unit_of_measurement\":\"%\",\"device_class\":\"humidity\"}").c_str(), true);
  mqttClient.publish((base+"/switch/"+node+"/output1/config").c_str(), (String("{\"name\":\"")+node+" Output 1\",\"state_topic\":\""+mqttBaseTopic+"/output1\",\"command_topic\":\""+mqttBaseTopic+"/output1/set\"}").c_str(), true);
  mqttClient.publish((base+"/switch/"+node+"/output2/config").c_str(), (String("{\"name\":\"")+node+" Output 2\",\"state_topic\":\""+mqttBaseTopic+"/output2\",\"command_topic\":\""+mqttBaseTopic+"/output2/set\"}").c_str(), true);
  mqttClient.publish((base+"/switch/"+node+"/output5/config").c_str(), (String("{\"name\":\"")+node+" Output Field5\",\"state_topic\":\""+mqttBaseTopic+"/output5\",\"command_topic\":\""+mqttBaseTopic+"/output5/set\"}").c_str(), true);
  mqttClient.publish((base+"/switch/"+node+"/output6/config").c_str(), (String("{\"name\":\"")+node+" Output Field6\",\"state_topic\":\""+mqttBaseTopic+"/output6\",\"command_topic\":\""+mqttBaseTopic+"/output6/set\"}").c_str(), true);
  mqttClient.publish((base+"/binary_sensor/"+node+"/input7/config").c_str(), (String("{\"name\":\"")+node+" Input Field7\",\"state_topic\":\""+mqttBaseTopic+"/input7\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"}").c_str(), true);
  mqttClient.publish((base+"/binary_sensor/"+node+"/input8/config").c_str(), (String("{\"name\":\"")+node+" Input Field8\",\"state_topic\":\""+mqttBaseTopic+"/input8\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"}").c_str(), true);
  mqttClient.publish((base+"/binary_sensor/"+node+"/input9/config").c_str(), (String("{\"name\":\"")+node+" Input Field9\",\"state_topic\":\""+mqttBaseTopic+"/input9\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"}").c_str(), true);
  mqttClient.publish((base+"/binary_sensor/"+node+"/input10/config").c_str(), (String("{\"name\":\"")+node+" Input Field10\",\"state_topic\":\""+mqttBaseTopic+"/input10\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"}").c_str(), true);
  haDiscoverySent = true;
  addEsp32Log("Home Assistant discovery published");
}

void maintainMqtt() {
  if (!mqttEnabled || !mqttHost[0] || WiFi.status() != WL_CONNECTED) return;
  if (!mqttClient.connected() && millis() - lastMqttReconnectMs > 5000UL) {
    lastMqttReconnectMs = millis();
    mqttClient.setServer(mqttHost, mqttPort);
    mqttClient.setCallback(mqttCallback);
    String clientId = String("ESP32-") + String((uint32_t)ESP.getEfuseMac(), HEX);
    bool ok = mqttUser[0] ? mqttClient.connect(clientId.c_str(), mqttUser, mqttPass) : mqttClient.connect(clientId.c_str());
    if (ok) {
      addEsp32Log("MQTT connected");
      mqttClient.subscribe((String(mqttBaseTopic)+"/output1/set").c_str());
      mqttClient.subscribe((String(mqttBaseTopic)+"/output2/set").c_str());
      mqttClient.subscribe((String(mqttBaseTopic)+"/output5/set").c_str());
      mqttClient.subscribe((String(mqttBaseTopic)+"/output6/set").c_str());
      haDiscoverySent = false;
      sendHomeAssistantDiscovery();
    } else {
      addEsp32Log("MQTT connect failed");
    }
  }
  mqttClient.loop();
  if (mqttClient.connected() && millis() - lastMqttPublishMs > 10000UL) {
    lastMqttPublishMs = millis();
    publishMqtt("temperature", isnan(currentTemperature) ? String("") : String(currentTemperature,1));
    publishMqtt("humidity", isnan(currentHumidity) ? String("") : String(currentHumidity,1));
    publishMqtt("output1", currentOutput1State ? "ON" : "OFF");
    publishMqtt("output2", currentOutput2State ? "ON" : "OFF");
    publishMqtt("output5", currentOutput5State ? "ON" : "OFF");
    publishMqtt("output6", currentOutput6State ? "ON" : "OFF");
    publishMqtt("input7", readInput7State() ? "ON" : "OFF");
    publishMqtt("input8", readInput8State() ? "ON" : "OFF");
    publishMqtt("input9", readInput9State() ? "ON" : "OFF");
    publishMqtt("input10", readInput10State() ? "ON" : "OFF");
    publishMqtt("status", esp32RealtimeJson(), false);
  }
}

void handleManualOverrideExpiry() {
  for (byte o = 1; o <= 6; o++) {
    if (manualOverrideActive[o] && (long)(millis() - manualOverrideEndMs[o]) >= 0) {
      manualOverrideActive[o] = false;
      if (o == 1) setOutputFromWeb(1, 0, "Timed Expire");
      else if (o == 2) setOutputFromWeb(2, 0, "Timed Expire");
      else if (o == 5) setOutputFromWeb(5, 0, "Timed Expire");
      else if (o == 6) setOutputFromWeb(6, 0, "Timed Expire");
      else if (o == 3) { writeSmartOutput(false); recordSmartOutput3ManualControl(0); rememberOutputCommand("Timed Expire", o, 0, true); }
      addEsp32Log(String("Manual override expired for output ") + o);
      wsBroadcastStatus();
    }
  }
}

void sendEsp32ToolsPage() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  String html = htmlHeader("ESP32 Features");
  html += F("<div class='card'><h1>ESP32 Realtime Dashboard</h1><div class='sub'>WebSocket live status + last 50 events.</div><div id='live'>Connecting...</div><pre id='log' style='white-space:pre-wrap;background:#0b1220;padding:10px;border-radius:12px'></pre></div>");
  html += F("<div class='card'><h1>Timed Manual Override</h1><form method='POST' action='/override'>"); html += sessionHiddenInput();
  html += F("<select name='out'>");
  html += F("<option value='1'>"); html += fieldLabelHtml(1); html += F(" / GPIO26</option>");
  html += F("<option value='2'>"); html += fieldLabelHtml(2); html += F(" / GPIO27</option>");
  html += F("<option value='3'>"); html += fieldLabelHtml(3); html += F(" / GPIO25</option>");
  html += F("<option value='5'>"); html += fieldLabelHtml(5); html += F(" / GPIO14</option>");
  html += F("<option value='6'>"); html += fieldLabelHtml(6); html += F(" / GPIO13</option>");
  html += F("</select><select name='state'><option value='1'>ON</option><option value='0'>OFF</option></select><input name='sec' type='number' min='1' max='86400' value='600'><button class='btn'>Start timed override</button></form></div>");
  html += F("<div class='card'><h1>Field Names</h1><div class='sub'>Admin only. These names appear in WebSocket, web pages, MQTT discovery and Telegram messages.</div><form method='POST' action='/savelabels'>"); html += sessionHiddenInput();
  html += F("<label>Output 1 Name</label><input maxlength='"); html += String(FIELD_LABEL_MAX_LEN); html += F("' name='fl1' value='"); html += fieldLabelHtml(1); html += F("'>");
  html += F("<label>Output 2 Name</label><input maxlength='"); html += String(FIELD_LABEL_MAX_LEN); html += F("' name='fl2' value='"); html += fieldLabelHtml(2); html += F("'>");
  html += F("<label>Output 3 Smart Name</label><input maxlength='"); html += String(FIELD_LABEL_MAX_LEN); html += F("' name='fl3' value='"); html += fieldLabelHtml(3); html += F("'>");
  html += F("<label>Output 5 Name</label><input maxlength='"); html += String(FIELD_LABEL_MAX_LEN); html += F("' name='fl5' value='"); html += fieldLabelHtml(5); html += F("'>");
  html += F("<label>Output 6 Name</label><input maxlength='"); html += String(FIELD_LABEL_MAX_LEN); html += F("' name='fl6' value='"); html += fieldLabelHtml(6); html += F("'>");
  html += F("<label>Input 7 Name</label><input maxlength='"); html += String(FIELD_LABEL_MAX_LEN); html += F("' name='fl7' value='"); html += fieldLabelHtml(7); html += F("'>");
  html += F("<label>Input 8 Name</label><input maxlength='"); html += String(FIELD_LABEL_MAX_LEN); html += F("' name='fl8' value='"); html += fieldLabelHtml(8); html += F("'>");
  html += F("<label>Input 9 Name</label><input maxlength='"); html += String(FIELD_LABEL_MAX_LEN); html += F("' name='fl9' value='"); html += fieldLabelHtml(9); html += F("'>");
  html += F("<label>Input 10 Name</label><input maxlength='"); html += String(FIELD_LABEL_MAX_LEN); html += F("' name='fl10' value='"); html += fieldLabelHtml(10); html += F("'><button class='btn'>Save Field Names</button></form></div>");
  html += F("<div class='card'><h1>Integrations</h1><form method='POST' action='/savesp32'>"); html += sessionHiddenInput();
  html += F("<label><input type='checkbox' name='mqtten' value='1' "); if(mqttEnabled) html+=F("checked"); html += F("> MQTT Enable</label><input name='mqtthost' placeholder='MQTT host' value='"); html += mqttHost; html += F("'><input name='mqttport' type='number' value='"); html += mqttPort; html += F("'><input name='mqttuser' placeholder='MQTT user' value='"); html += mqttUser; html += F("'><input name='mqttpass' placeholder='MQTT pass' value='"); html += mqttPass; html += F("'><input name='mqttbase' placeholder='Base topic' value='"); html += mqttBaseTopic; html += F("'>");
  html += F("<label><input type='checkbox' name='tgen' value='1' "); if(telegramEnabled) html+=F("checked"); html += F("> Telegram Enable</label><input name='tgbot' placeholder='Telegram bot token' value='"); html += telegramBotToken; html += F("'><input name='tgchat' placeholder='Telegram chat id' value='"); html += telegramChatId; html += F("'><input name='tglink' placeholder='Telegram bot link or username, e.g. https://t.me/YourBot' value='"); html += htmlEscapeText(String(telegramBotLink)); html += F("'><input name='tghigh' type='number' step='0.1' value='"); html += String(telegramHighTempC,1); html += F("'>");
  html += F("<div class='sub'>Telegram buttons: send /start to the bot, press Login, then type web username and password. The Telegram bot link appears on Home for users. Chat ID is only needed for admin notifications; command polling works with Bot Token + Enable.</div>");
  html += F("<h1>Calibration</h1><input name='toff' type='number' step='0.1' value='"); html += String(tempCalibrationOffset,1); html += F("'><input name='hoff' type='number' step='0.1' value='"); html += String(humCalibrationOffset,1); html += F("'>");
  html += F("<h1>External WebSocket</h1><div class='sub'>Internal Device WebSocket port is 81. From outside LAN, router must forward this external TCP port to Device internal TCP port 81.</div><input name='wsport' type='number' min='1' max='65535' value='"); html += String(wsPublicPort); html += F("'><button class='btn'>Save Device Settings</button></form></div>");
  html += F("<script>const WS_PORT="); html += String(wsPublicPort); html += F(";function setLive(t){let e=document.getElementById('live');if(e)e.innerHTML=t;}function connectWs(){let proto=(location.protocol==='https:')?'wss://':'ws://';let url=proto+location.hostname+':'+WS_PORT+'/';setLive('Connecting WebSocket: '+url);let ws=new WebSocket(url);ws.onopen=()=>setLive('WebSocket connected');ws.onmessage=e=>{let d=JSON.parse(e.data);document.getElementById('live').innerHTML='Temp: '+d.temp+' / Hum: '+d.hum+' / '+(d.l1||'O1')+': '+d.o1+' / '+(d.l2||'O2')+': '+d.o2+' / '+(d.l5||'O5')+': '+d.o5+' / '+(d.l6||'O6')+': '+d.o6+' / '+(d.l7||'I7')+': '+d.i7+' / '+(d.l8||'I8')+': '+d.i8+' / '+(d.l9||'I9')+': '+d.i9+' / '+(d.l10||'I10')+': '+d.i10+' / MQTT: '+d.mqtt+' / RSSI: '+d.rssi+' / IP: '+d.ip+'<br>Last Command: '+(d.lastCmd||'--')+'<br>Command User: '+(d.lastCmdUser||'--')+'<br>Command Source: '+(d.lastCmdSource||'--')+'<br>Command Date: '+(d.lastCmdTime||'--');document.getElementById('log').textContent=(d.log||[]).map(x=>(x.time?x.time+' - ':'')+Math.floor(x.ms/1000)+'s - '+x.text).join('\\n');};ws.onerror=()=>setLive('WebSocket error. From outside LAN forward external TCP port '+WS_PORT+' to ESP32 internal port 81.');ws.onclose=()=>setTimeout(connectWs,3000);}connectWs();</script>");
  html += htmlFooter();
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.sendHeader("Expires", "0");
  webServer.send(200, "text/html", html);
}

void handleSaveEsp32Settings() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  mqttEnabled = webServer.hasArg("mqtten");
  webServer.arg("mqtthost").toCharArray(mqttHost, sizeof(mqttHost));
  mqttPort = (uint16_t)webServer.arg("mqttport").toInt(); if (!mqttPort) mqttPort = 1883;
  webServer.arg("mqttuser").toCharArray(mqttUser, sizeof(mqttUser));
  webServer.arg("mqttpass").toCharArray(mqttPass, sizeof(mqttPass));
  webServer.arg("mqttbase").toCharArray(mqttBaseTopic, sizeof(mqttBaseTopic));
  telegramEnabled = webServer.hasArg("tgen");
  webServer.arg("tgbot").toCharArray(telegramBotToken, sizeof(telegramBotToken));
  webServer.arg("tgchat").toCharArray(telegramChatId, sizeof(telegramChatId));
  if (webServer.hasArg("tglink")) {
    String linkArg = webServer.arg("tglink");
    linkArg.trim();
    linkArg.substring(0, sizeof(telegramBotLink) - 1).toCharArray(telegramBotLink, sizeof(telegramBotLink));
  }
  telegramHighTempC = webServer.arg("tghigh").toFloat();
  tempCalibrationOffset = webServer.arg("toff").toFloat();
  humCalibrationOffset = webServer.arg("hoff").toFloat();
  if (webServer.hasArg("wsport")) {
    uint32_t wp = (uint32_t)webServer.arg("wsport").toInt();
    wsPublicPort = (wp >= 1 && wp <= 65535) ? (uint16_t)wp : ESP32_WS_INTERNAL_PORT;
  }
  saveEsp32IntegrationConfig();
  saveTelegramUiConfig();
  autoBackupAfterSave("Integrations saved");
  addEsp32Log("Device settings saved to EEPROM/NVS");
  webServer.sendHeader("Location", sessionUrl("/esp32"), true);
  webServer.send(302, "text/plain", "");
}

void handleTimedOverride() {
  if (!requireRole(WEB_ROLE_OPERATOR)) return;
  byte out = (byte)webServer.arg("out").toInt();
  byte state = (byte)webServer.arg("state").toInt();
  unsigned long sec = webServer.arg("sec").toInt();
  if (!((out >= 1 && out <= 3) || out == 5 || out == 6) || !isValidState(state) || sec < 1) {
    webServer.send(400, "text/plain", "bad request");
    return;
  }
  manualOverrideActive[out] = true;
  manualOverrideState[out] = state;
  manualOverrideEndMs[out] = millis() + sec * 1000UL;
  if (out == 1) setOutputFromWeb(1, state, (String("Timed Override(") + currentWebUserName() + ")").c_str());
  else if (out == 2) setOutputFromWeb(2, state, (String("Timed Override(") + currentWebUserName() + ")").c_str());
  else if (out == 5) setOutputFromWeb(5, state, (String("Timed Override(") + currentWebUserName() + ")").c_str());
  else if (out == 6) setOutputFromWeb(6, state, (String("Timed Override(") + currentWebUserName() + ")").c_str());
  else { writeSmartOutput(state); recordSmartOutput3ManualControl(state); rememberOutputCommand((String("Timed Override(") + currentWebUserName() + ")").c_str(), out, state, true); }
  addEsp32Log(String("Timed manual override output ") + out + (state ? " ON " : " OFF ") + sec + " sec");
  wsBroadcastStatus();
  webServer.sendHeader("Location", sessionUrl("/esp32"), true);
  webServer.send(302, "text/plain", "");
}



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

// EEPROM stays the main RAM map for all settings.  On some ESP32/Arduino core builds
// EEPROM.commit() can fail after flash erase or large EEPROM maps because it is emulated through NVS.
// To avoid losing user settings, every EEPROM save also writes an exact 4096-byte image to SPIFFS.
// At boot, if the image exists, it is copied back into the EEPROM RAM buffer before any load*() function runs.
#define STORAGE_FS_BACKUP_PATH "/eeprom_image_v156.bin"


// V1.6.7C: Dedicated rescue store for the whole User Management page.
// This keeps user/password/permissions/Telegram IDs/subscription options/login intro persistent
// even when EEPROM/NVS commit paths are unreliable after flash/partition changes.
#define USER_PAGE_STORE_PATH    "/user_page_v167.bin"
#define USER_PAGE_STORE_MAGIC   0x55504731UL  // UPG1
#define USER_PAGE_STORE_VERSION 1

struct UserPagePersistentRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t checksum;
  WebUserAccount users[WEB_USER_COUNT];
  uint16_t visibleMask[WEB_USER_COUNT];
  uint16_t controlMask[WEB_USER_COUNT];
  char telegramChatId[WEB_USER_COUNT][USER_TELEGRAM_CHAT_ID_MAX_LEN + 1];
  byte telegramNotify[WEB_USER_COUNT];
  uint32_t lastDisableYm;
  byte monthlyAuto;
  byte codeExpiry;
  byte telegramSmart;
  byte introEnabled;
  uint16_t introLen;
  char introText[LOGIN_INTRO_MAX_LEN + 1];
};

bool userPageStoreLoadedAtBoot = false;
bool userPageStoreLastSaveOk = false;
uint32_t userPageStoreSaveCount = 0;
uint32_t userPageStoreLoadCount = 0;
char userPageStoreLastError[40] = "none";

bool eepromBeginOk = false;
bool lastEepromCommitOk = false;
bool lastStorageSaveOk = false;
uint32_t eepromCommitAttempts = 0;
uint32_t eepromCommitOkCount = 0;
unsigned long lastEepromCommitMs = 0;
char lastEepromCommitReason[32] = "none";

bool storageFsBeginOk = false;
bool storageFsLoadedAtBoot = false;
bool storageFsLastSaveOk = false;
uint32_t storageFsSaveCount = 0;
uint32_t storageFsLoadCount = 0;
char storageFsLastError[40] = "none";

// V1.7.0: Automatic daily/event backup saved inside SPIFFS.
// Backup filenames include firmware version/build and device date for easy tracking.
#define AUTO_BACKUP_MAGIC       "AHMDBK2"
#define AUTO_BACKUP_CFG_PATH    "/auto_backup_v170.cfg"
#define AUTO_BACKUP_PREFIX      "/backup_"
#define AUTO_BACKUP_TMP_USER    "/restore_user_tmp.bin"
#define AUTO_BACKUP_TMP_ACT     "/restore_act_tmp.bin"
#define AUTO_BACKUP_FORMAT_VER  2
bool autoBackupDailyEnabled = false;
bool autoBackupOnSaveEnabled = true;
bool autoBackupTelegramNotify = true;
byte autoBackupHour = 3;
byte autoBackupMinute = 0;
byte autoBackupKeepCount = 7;
uint32_t autoBackupLastDailyYmd = 0;
unsigned long autoBackupLastCheckMs = 0;
unsigned long autoBackupLastEventMs = 0;
bool autoBackupLastOk = false;
uint32_t autoBackupCreateCount = 0;
String autoBackupLatestFile = "";
String autoBackupLastReason = "none";
String autoBackupLastMessage = "No backup yet";

void setStorageFsError(const char* msg) {
  if (!msg || !msg[0]) msg = "none";
  strncpy(storageFsLastError, msg, sizeof(storageFsLastError) - 1);
  storageFsLastError[sizeof(storageFsLastError) - 1] = '\0';
}

bool beginStorageFs() {
  storageFsBeginOk = SPIFFS.begin(true);
  if (storageFsBeginOk) setStorageFsError("none");
  else setStorageFsError("SPIFFS.begin failed");
  DBG_PRINT("SPIFFS.begin: "); DBG_PRINTLN(storageFsBeginOk ? "OK" : "FAILED");
  return storageFsBeginOk;
}

bool saveEepromImageToFs(const char* reason) {
  (void)reason;
  storageFsLastSaveOk = false;
  if (!storageFsBeginOk) {
    setStorageFsError("FS not mounted");
    return false;
  }

  File f = SPIFFS.open(STORAGE_FS_BACKUP_PATH, FILE_WRITE);
  if (!f) {
    setStorageFsError("open write failed");
    return false;
  }

  uint8_t buf[128];
  for (int off = 0; off < EEPROM_SIZE; off += (int)sizeof(buf)) {
    int n = EEPROM_SIZE - off;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    for (int i = 0; i < n; i++) buf[i] = EEPROM.read(off + i);
    size_t w = f.write(buf, n);
    if (w != (size_t)n) {
      f.close();
      setStorageFsError("write short");
      return false;
    }
    yield();
  }
  f.flush();
  f.close();
  storageFsLastSaveOk = true;
  storageFsSaveCount++;
  setStorageFsError("none");
  DBG_PRINTLN("SPIFFS EEPROM image save: OK");
  return true;
}

bool loadEepromImageFromFs() {
  storageFsLoadedAtBoot = false;
  if (!storageFsBeginOk) {
    setStorageFsError("FS not mounted");
    return false;
  }
  if (!SPIFFS.exists(STORAGE_FS_BACKUP_PATH)) {
    setStorageFsError("no backup file");
    return false;
  }

  File f = SPIFFS.open(STORAGE_FS_BACKUP_PATH, FILE_READ);
  if (!f) {
    setStorageFsError("open read failed");
    return false;
  }
  if (f.size() != EEPROM_SIZE) {
    f.close();
    setStorageFsError("bad backup size");
    return false;
  }

  uint8_t buf[128];
  for (int off = 0; off < EEPROM_SIZE; off += (int)sizeof(buf)) {
    int n = EEPROM_SIZE - off;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    int r = f.read(buf, n);
    if (r != n) {
      f.close();
      setStorageFsError("read short");
      return false;
    }
    for (int i = 0; i < n; i++) EEPROM.write(off + i, buf[i]);
    yield();
  }
  f.close();
  storageFsLoadedAtBoot = true;
  storageFsLoadCount++;
  setStorageFsError("none");
  DBG_PRINTLN("SPIFFS EEPROM image load at boot: YES");
  return true;
}

void removeStorageFsBackup() {
  if (storageFsBeginOk && SPIFFS.exists(STORAGE_FS_BACKUP_PATH)) {
    SPIFFS.remove(STORAGE_FS_BACKUP_PATH);
  }
  storageFsLoadedAtBoot = false;
  storageFsLastSaveOk = false;
  setStorageFsError("backup removed");
}

bool commitEeprom(const char* reason) {
  eepromCommitAttempts++;
  lastEepromCommitMs = millis();
  if (reason && reason[0]) {
    strncpy(lastEepromCommitReason, reason, sizeof(lastEepromCommitReason) - 1);
    lastEepromCommitReason[sizeof(lastEepromCommitReason) - 1] = '\0';
  } else {
    strncpy(lastEepromCommitReason, "eeprom-save", sizeof(lastEepromCommitReason) - 1);
    lastEepromCommitReason[sizeof(lastEepromCommitReason) - 1] = '\0';
  }

  lastEepromCommitOk = EEPROM.commit();
  if (lastEepromCommitOk) eepromCommitOkCount++;

  // Safety rescue: save the exact same EEPROM RAM image to a normal flash file too.
  bool fsOk = saveEepromImageToFs(reason);
  lastStorageSaveOk = lastEepromCommitOk || fsOk;

  DBG_PRINT("EEPROM.commit reason="); DBG_PRINT(lastEepromCommitReason);
  DBG_PRINT(" eeprom="); DBG_PRINT(lastEepromCommitOk ? "OK" : "FAILED");
  DBG_PRINT(" fsBackup="); DBG_PRINTLN(fsOk ? "OK" : "FAILED");
  return lastStorageSaveOk;
}

String eepromStorageStatusText() {
  String s;
  s.reserve(220);
  s += F("EEPROM.begin="); s += eepromBeginOk ? F("OK") : F("FAILED");
  s += F(" / LastCommit="); s += lastEepromCommitOk ? F("OK") : F("FAILED/none");
  s += F(" / Attempts="); s += String(eepromCommitAttempts);
  s += F(" / OK="); s += String(eepromCommitOkCount);
  s += F(" / FS="); s += storageFsBeginOk ? F("OK") : F("FAILED");
  s += F(" / FsLoad="); s += storageFsLoadedAtBoot ? F("YES") : F("NO");
  s += F(" / LastFsSave="); s += storageFsLastSaveOk ? F("OK") : F("FAILED/none");
  s += F(" / FsSaves="); s += String(storageFsSaveCount);
  s += F(" / Saved="); s += lastStorageSaveOk ? F("YES") : F("NO");
  s += F(" / Err="); s += storageFsLastError;
  return s;
}

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
  // Dedicated internet LED on GPIO18. It is independent from Output 3 on GPIO25.
#if INTERNET_LED_ACTIVE_HIGH
  digitalWrite(INTERNET_LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(INTERNET_LED_PIN, on ? LOW : HIGH);
#endif
#endif
}

void applyOutputs() {
  setRelay(OUTPUT1_PIN, currentOutput1State);
  setRelay(OUTPUT2_PIN, currentOutput2State);
  setRelay(OUTPUT5_PIN, currentOutput5State);
  setRelay(OUTPUT6_PIN, currentOutput6State);

  DBG_PRINTLN("==== APPLY OUTPUTS ====");
  DBG_PRINT("Output1 GPIO"); DBG_PRINT(OUTPUT1_PIN); DBG_PRINT(" = "); DBG_PRINTLN(currentOutput1State ? "ON" : "OFF");
  DBG_PRINT("Output2 GPIO"); DBG_PRINT(OUTPUT2_PIN); DBG_PRINT(" = "); DBG_PRINTLN(currentOutput2State ? "ON" : "OFF");
  DBG_PRINT("Output5 GPIO"); DBG_PRINT(OUTPUT5_PIN); DBG_PRINT(" = "); DBG_PRINTLN(currentOutput5State ? "ON" : "OFF");
  DBG_PRINT("Output6 GPIO"); DBG_PRINT(OUTPUT6_PIN); DBG_PRINT(" = "); DBG_PRINTLN(currentOutput6State ? "ON" : "OFF");
  DBG_PRINTLN("=======================");
}

void saveOutputState(byte outputNumber, byte state) {
  int addr = EEPROM_OUTPUT1_ADDR;
  if (outputNumber == 1) addr = EEPROM_OUTPUT1_ADDR;
  else if (outputNumber == 2) addr = EEPROM_OUTPUT2_ADDR;
  else if (outputNumber == 5) addr = EEPROM_OUTPUT5_ADDR;
  else if (outputNumber == 6) addr = EEPROM_OUTPUT6_ADDR;
  else return;

  byte oldValue = EEPROM.read(addr);

  DBG_PRINTLN("==== EEPROM SAVE REQUEST ====");
  DBG_PRINT("Output number : "); DBG_PRINTLN(outputNumber);
  DBG_PRINT("EEPROM address: "); DBG_PRINTLN(addr);
  DBG_PRINT("Old EEPROM val: "); DBG_PRINTLN(oldValue);
  DBG_PRINT("New state     : "); DBG_PRINTLN(state);

  if (oldValue != state) {
    EEPROM.write(addr, state);
    bool ok = commitEeprom("eeprom-save");
    DBG_PRINT("EEPROM commit : "); DBG_PRINTLN(ok ? "OK" : "FAILED");
    DBG_PRINT("Verify read   : "); DBG_PRINTLN(EEPROM.read(addr));
  } else {
    DBG_PRINTLN("EEPROM write skipped: value already stored");
  }

  DBG_PRINTLN("=============================");
}

void loadOutputStates() {
  DBG_PRINTLN("==== EEPROM LOAD AT BOOT ====");

  byte rawOutput1 = EEPROM.read(EEPROM_OUTPUT1_ADDR);
  byte rawOutput2 = EEPROM.read(EEPROM_OUTPUT2_ADDR);
  byte rawOutput5 = EEPROM.read(EEPROM_OUTPUT5_ADDR);
  byte rawOutput6 = EEPROM.read(EEPROM_OUTPUT6_ADDR);

  DBG_PRINT("Raw EEPROM Output1 @addr "); DBG_PRINT(EEPROM_OUTPUT1_ADDR); DBG_PRINT(" = "); DBG_PRINTLN(rawOutput1);
  DBG_PRINT("Raw EEPROM Output2 @addr "); DBG_PRINT(EEPROM_OUTPUT2_ADDR); DBG_PRINT(" = "); DBG_PRINTLN(rawOutput2);
  DBG_PRINT("Raw EEPROM Output5 @addr "); DBG_PRINT(EEPROM_OUTPUT5_ADDR); DBG_PRINT(" = "); DBG_PRINTLN(rawOutput5);
  DBG_PRINT("Raw EEPROM Output6 @addr "); DBG_PRINT(EEPROM_OUTPUT6_ADDR); DBG_PRINT(" = "); DBG_PRINTLN(rawOutput6);

  currentOutput1State = sanitizeState(rawOutput1, 0);
  currentOutput2State = sanitizeState(rawOutput2, 0);
  currentOutput5State = sanitizeState(rawOutput5, 0);
  currentOutput6State = sanitizeState(rawOutput6, 0);

  if (rawOutput1 != currentOutput1State) DBG_PRINTLN("Output1 EEPROM value invalid -> repaired to 0");
  if (rawOutput2 != currentOutput2State) DBG_PRINTLN("Output2 EEPROM value invalid -> repaired to 0");
  if (rawOutput5 != currentOutput5State) DBG_PRINTLN("Output5 EEPROM value invalid -> repaired to 0");
  if (rawOutput6 != currentOutput6State) DBG_PRINTLN("Output6 EEPROM value invalid -> repaired to 0");

  DBG_PRINT("Loaded Output1 state = "); DBG_PRINTLN(currentOutput1State ? "ON" : "OFF");
  DBG_PRINT("Loaded Output2 state = "); DBG_PRINTLN(currentOutput2State ? "ON" : "OFF");
  DBG_PRINT("Loaded Output5 state = "); DBG_PRINTLN(currentOutput5State ? "ON" : "OFF");
  DBG_PRINT("Loaded Output6 state = "); DBG_PRINTLN(currentOutput6State ? "ON" : "OFF");

  saveOutputState(1, currentOutput1State);
  saveOutputState(2, currentOutput2State);
  saveOutputState(5, currentOutput5State);
  saveOutputState(6, currentOutput6State);

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
    bool ok = commitEeprom("eeprom-save");
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
  char buf[129];
  int safeLen = maxLen;
  if (safeLen > 128) safeLen = 128;
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

bool isFiniteFloatValue(float value, float minValue, float maxValue) {
  return !isnan(value) && isfinite(value) && value >= minValue && value <= maxValue;
}

void writeEepromUInt16(int addr, uint16_t value) {
  EEPROM.write(addr, (byte)((value >> 8) & 0xFF));
  EEPROM.write(addr + 1, (byte)(value & 0xFF));
}

uint16_t readEepromUInt16(int addr, uint16_t defaultValue) {
  uint16_t value = ((uint16_t)EEPROM.read(addr) << 8) | EEPROM.read(addr + 1);
  return value == 0xFFFF ? defaultValue : value;
}

void writeEepromFloatValue(int addr, float value) {
  EEPROM.put(addr, value);
}

float readEepromFloatValue(int addr, float defaultValue, float minValue, float maxValue) {
  float value;
  EEPROM.get(addr, value);
  return isFiniteFloatValue(value, minValue, maxValue) ? value : defaultValue;
}

void loadEsp32IntegrationConfig() {
  if (EEPROM.read(EEPROM_ESP32CFG_MAGIC_ADDR) == EEPROM_ESP32CFG_MAGIC) {
    mqttEnabled = EEPROM.read(EEPROM_ESP32CFG_MQTT_ENABLE_ADDR) == 1;
    telegramEnabled = EEPROM.read(EEPROM_ESP32CFG_TG_ENABLE_ADDR) == 1;

    readEepromString(EEPROM_ESP32CFG_HOST_ADDR, sizeof(mqttHost)).toCharArray(mqttHost, sizeof(mqttHost));
    mqttPort = readEepromUInt16(EEPROM_ESP32CFG_PORT_ADDR, 1883);
    if (mqttPort == 0 || mqttPort > 65535) mqttPort = 1883;
    readEepromString(EEPROM_ESP32CFG_USER_ADDR, sizeof(mqttUser)).toCharArray(mqttUser, sizeof(mqttUser));
    readEepromString(EEPROM_ESP32CFG_PASS_ADDR, sizeof(mqttPass)).toCharArray(mqttPass, sizeof(mqttPass));
    readEepromString(EEPROM_ESP32CFG_BASE_ADDR, sizeof(mqttBaseTopic)).toCharArray(mqttBaseTopic, sizeof(mqttBaseTopic));
    readEepromString(EEPROM_ESP32CFG_DISC_ADDR, sizeof(mqttDiscoveryPrefix)).toCharArray(mqttDiscoveryPrefix, sizeof(mqttDiscoveryPrefix));

    readEepromString(EEPROM_ESP32CFG_TGBOT_ADDR, sizeof(telegramBotToken)).toCharArray(telegramBotToken, sizeof(telegramBotToken));
    readEepromString(EEPROM_ESP32CFG_TGCHAT_ADDR, sizeof(telegramChatId)).toCharArray(telegramChatId, sizeof(telegramChatId));
    telegramHighTempC = readEepromFloatValue(EEPROM_ESP32CFG_TGHIGH_ADDR, 45.0f, -40.0f, 125.0f);
    tempCalibrationOffset = readEepromFloatValue(EEPROM_ESP32CFG_TEMPCAL_ADDR, 0.0f, -20.0f, 20.0f);
    humCalibrationOffset = readEepromFloatValue(EEPROM_ESP32CFG_HUMCAL_ADDR, 0.0f, -50.0f, 50.0f);
    if (EEPROM.read(EEPROM_ESP32CFG_WS_MAGIC_ADDR) == EEPROM_ESP32CFG_WS_MAGIC) {
      wsPublicPort = readEepromUInt16(EEPROM_ESP32CFG_WS_PUBLIC_PORT_ADDR, ESP32_WS_INTERNAL_PORT);
      if (wsPublicPort < 1 || wsPublicPort > 65535) wsPublicPort = ESP32_WS_INTERNAL_PORT;
    } else {
      wsPublicPort = ESP32_WS_INTERNAL_PORT;
    }

    if (mqttBaseTopic[0] == '\0') strncpy(mqttBaseTopic, "ahmed/esp32", sizeof(mqttBaseTopic) - 1);
    if (mqttDiscoveryPrefix[0] == '\0') strncpy(mqttDiscoveryPrefix, "homeassistant", sizeof(mqttDiscoveryPrefix) - 1);
    mqttBaseTopic[sizeof(mqttBaseTopic) - 1] = '\0';
    mqttDiscoveryPrefix[sizeof(mqttDiscoveryPrefix) - 1] = '\0';

    addEsp32Log("Device MQTT/Telegram/Calibration loaded from EEPROM");
  } else {
    mqttEnabled = false;
    telegramEnabled = false;
    mqttHost[0] = '\0';
    mqttPort = 1883;
    mqttUser[0] = '\0';
    mqttPass[0] = '\0';
    strncpy(mqttBaseTopic, "ahmed/esp32", sizeof(mqttBaseTopic) - 1);
    strncpy(mqttDiscoveryPrefix, "homeassistant", sizeof(mqttDiscoveryPrefix) - 1);
    telegramBotToken[0] = '\0';
    telegramChatId[0] = '\0';
    telegramHighTempC = 45.0f;
    tempCalibrationOffset = 0.0f;
    humCalibrationOffset = 0.0f;
    wsPublicPort = ESP32_WS_INTERNAL_PORT;
  }
}

void saveEsp32IntegrationConfig() {
  if (mqttPort == 0) mqttPort = 1883;
  if (mqttBaseTopic[0] == '\0') strncpy(mqttBaseTopic, "ahmed/esp32", sizeof(mqttBaseTopic) - 1);
  if (mqttDiscoveryPrefix[0] == '\0') strncpy(mqttDiscoveryPrefix, "homeassistant", sizeof(mqttDiscoveryPrefix) - 1);
  if (!isFiniteFloatValue(telegramHighTempC, -40.0f, 125.0f)) telegramHighTempC = 45.0f;
  if (!isFiniteFloatValue(tempCalibrationOffset, -20.0f, 20.0f)) tempCalibrationOffset = 0.0f;
  if (!isFiniteFloatValue(humCalibrationOffset, -50.0f, 50.0f)) humCalibrationOffset = 0.0f;
  if (wsPublicPort < 1 || wsPublicPort > 65535) wsPublicPort = ESP32_WS_INTERNAL_PORT;

  EEPROM.write(EEPROM_ESP32CFG_MAGIC_ADDR, EEPROM_ESP32CFG_MAGIC);
  EEPROM.write(EEPROM_ESP32CFG_MQTT_ENABLE_ADDR, mqttEnabled ? 1 : 0);
  EEPROM.write(EEPROM_ESP32CFG_TG_ENABLE_ADDR, telegramEnabled ? 1 : 0);
  writeEepromString(EEPROM_ESP32CFG_HOST_ADDR, sizeof(mqttHost), String(mqttHost));
  writeEepromUInt16(EEPROM_ESP32CFG_PORT_ADDR, mqttPort);
  writeEepromString(EEPROM_ESP32CFG_USER_ADDR, sizeof(mqttUser), String(mqttUser));
  writeEepromString(EEPROM_ESP32CFG_PASS_ADDR, sizeof(mqttPass), String(mqttPass));
  writeEepromString(EEPROM_ESP32CFG_BASE_ADDR, sizeof(mqttBaseTopic), String(mqttBaseTopic));
  writeEepromString(EEPROM_ESP32CFG_DISC_ADDR, sizeof(mqttDiscoveryPrefix), String(mqttDiscoveryPrefix));
  writeEepromString(EEPROM_ESP32CFG_TGBOT_ADDR, sizeof(telegramBotToken), String(telegramBotToken));
  writeEepromString(EEPROM_ESP32CFG_TGCHAT_ADDR, sizeof(telegramChatId), String(telegramChatId));
  writeEepromFloatValue(EEPROM_ESP32CFG_TGHIGH_ADDR, telegramHighTempC);
  telegramLastUpdateId = 0;
  telegramCommandPollReady = false;
  writeEepromFloatValue(EEPROM_ESP32CFG_TEMPCAL_ADDR, tempCalibrationOffset);
  writeEepromFloatValue(EEPROM_ESP32CFG_HUMCAL_ADDR, humCalibrationOffset);
  EEPROM.write(EEPROM_ESP32CFG_WS_MAGIC_ADDR, EEPROM_ESP32CFG_WS_MAGIC);
  writeEepromUInt16(EEPROM_ESP32CFG_WS_PUBLIC_PORT_ADDR, wsPublicPort);
  commitEeprom("eeprom-save");

  haDiscoverySent = false;
  mqttClient.disconnect();
}

void clearEsp32IntegrationConfig() {
  mqttEnabled = false;
  telegramEnabled = false;
  mqttHost[0] = '\0';
  mqttPort = 1883;
  mqttUser[0] = '\0';
  mqttPass[0] = '\0';
  strncpy(mqttBaseTopic, "ahmed/esp32", sizeof(mqttBaseTopic) - 1);
  strncpy(mqttDiscoveryPrefix, "homeassistant", sizeof(mqttDiscoveryPrefix) - 1);
  telegramBotToken[0] = '\0';
  telegramChatId[0] = '\0';
  telegramHighTempC = 45.0f;
  tempCalibrationOffset = 0.0f;
  humCalibrationOffset = 0.0f;
  wsPublicPort = ESP32_WS_INTERNAL_PORT;
  saveEsp32IntegrationConfig();
}

const char* defaultFieldLabel(byte fieldNumber) {
  switch (fieldNumber) {
    case 1: return "Pump";
    case 2: return "Light";
    case 3: return "Smart Output";
    case 4: return "Humidity";
    case 5: return "Field5 Output";
    case 6: return "Field6 Output";
    case 7: return "Input7";
    case 8: return "Input8";
    case 9: return "Input9";
    case 10: return "Input10";
    default: return "Field";
  }
}

String sanitizeFieldLabel(String label, byte fieldNumber) {
  label.replace("\r", " ");
  label.replace("\n", " ");
  label.trim();
  if (label.length() == 0) label = defaultFieldLabel(fieldNumber);
  if (label.length() > FIELD_LABEL_MAX_LEN) label = label.substring(0, FIELD_LABEL_MAX_LEN);
  return label;
}

String htmlEscapeText(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
  return out;
}

void setFieldLabel(byte fieldNumber, const String& label) {
  if (fieldNumber < 1 || fieldNumber > FIELD_LABEL_COUNT) return;
  String clean = sanitizeFieldLabel(label, fieldNumber);
  clean.toCharArray(fieldLabels[fieldNumber], FIELD_LABEL_MAX_LEN + 1);
}

String fieldLabel(byte fieldNumber) {
  if (fieldNumber < 1 || fieldNumber > FIELD_LABEL_COUNT) return String("Field") + String(fieldNumber);
  if (fieldLabels[fieldNumber][0] == '\0') return String(defaultFieldLabel(fieldNumber));
  return String(fieldLabels[fieldNumber]);
}

String fieldLabelHtml(byte fieldNumber) {
  return htmlEscapeText(fieldLabel(fieldNumber));
}

void resetFieldLabelsToDefaults() {
  for (byte i = 1; i <= FIELD_LABEL_COUNT; i++) setFieldLabel(i, String(defaultFieldLabel(i)));
}

void loadFieldLabelsConfig() {
  resetFieldLabelsToDefaults();
  if (EEPROM.read(EEPROM_FIELD_LABEL_MAGIC_ADDR) != EEPROM_FIELD_LABEL_MAGIC) return;
  for (byte i = 1; i <= FIELD_LABEL_COUNT; i++) {
    int addr = EEPROM_FIELD_LABEL_DATA_ADDR + ((i - 1) * EEPROM_FIELD_LABEL_SLOT_SIZE);
    setFieldLabel(i, readEepromString(addr, EEPROM_FIELD_LABEL_SLOT_SIZE));
  }
}

void saveFieldLabelsConfig() {
  EEPROM.write(EEPROM_FIELD_LABEL_MAGIC_ADDR, EEPROM_FIELD_LABEL_MAGIC);
  for (byte i = 1; i <= FIELD_LABEL_COUNT; i++) {
    int addr = EEPROM_FIELD_LABEL_DATA_ADDR + ((i - 1) * EEPROM_FIELD_LABEL_SLOT_SIZE);
    writeEepromString(addr, EEPROM_FIELD_LABEL_SLOT_SIZE, fieldLabel(i));
  }
  commitEeprom("eeprom-save");
  haDiscoverySent = false;
}

void handleSaveFieldLabels() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  byte editableFields[] = {1, 2, 3, 5, 6, 7, 8, 9, 10};
  for (byte n = 0; n < sizeof(editableFields); n++) {
    byte f = editableFields[n];
    String argName = String("fl") + String(f);
    if (webServer.hasArg(argName)) setFieldLabel(f, webServer.arg(argName));
  }
  saveFieldLabelsConfig();
  autoBackupAfterSave("Field labels saved");
  addEsp32Log("Field names saved by Admin");
  wsBroadcastStatus();
  webServer.sendHeader("Location", sessionUrl("/esp32"), true);
  webServer.send(302, "text/plain", "");
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
  lastWifiSaveMs = millis();
  lastWifiSavedPassLen = strlen(activePass);
  commitEeprom("wifi-save");
  setLastWifiAction(String("WiFi credentials saved: ") + String(activeSsid));
}

void forgetWiFiManagerConfig() {
  activeSsid[0] = '\0';
  activePass[0] = '\0';
  wifiManagerMode = WIFI_MODE_AP_STA_ALWAYS;
  EEPROM.write(EEPROM_WIFI_MAGIC_ADDR, EEPROM_WIFI_MAGIC);
  EEPROM.write(EEPROM_WIFI_MODE_ADDR, wifiManagerMode);
  writeEepromString(EEPROM_WIFI_SSID_ADDR, WIFI_SSID_MAX_LEN + 1, "");
  writeEepromString(EEPROM_WIFI_PASS_ADDR, WIFI_PASS_MAX_LEN + 1, "");
  lastWifiSaveMs = millis();
  lastWifiSavedPassLen = 0;
  commitEeprom("wifi-forget");
  setLastWifiAction("WiFi credentials forgotten");
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

  // V1.6.6: keep the saved Static/DHCP selection.
  // If Static is ON, connection is Static-only. No DHCP fallback is used,
  // because port forwarding needs the fixed router IP.
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
  commitEeprom("eeprom-save");
  return true;
}

bool applyStaticIpBeforeConnect() {
  if (!wifiStaticEnabled) {
    // DHCP path: do not call WiFi.config(0,0,0).
    // On some ESP32 Arduino core builds that leaves STA stuck in WL_DISCONNECTED.
    return true;
  }

  IPAddress ip, gw, sub, dns;
  if (!parseIPText(wifiStaticIP, ip) ||
      !parseIPText(wifiStaticGateway, gw) ||
      !parseIPText(wifiStaticSubnet, sub) ||
      !parseIPText(wifiStaticDns, dns)) {
    DBG_PRINTLN("Static IP config invalid -> AP only, no DHCP fallback");
    return false;
  }

  DBG_PRINT("Applying strict Static IP: ");
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
  commitEeprom("eeprom-save");
}

void clearThingSpeakConfig() {
  tsChannelId[0] = '\0';
  tsWriteKey[0] = '\0';
  tsReadKey[0] = '\0';
  EEPROM.write(EEPROM_TS_MAGIC_ADDR, EEPROM_TS_MAGIC);
  writeEepromString(EEPROM_TS_CHANNEL_ADDR, TS_CHANNEL_MAX_LEN + 1, "");
  writeEepromString(EEPROM_TS_WRITE_ADDR, TS_KEY_MAX_LEN + 1, "");
  writeEepromString(EEPROM_TS_READ_ADDR, TS_KEY_MAX_LEN + 1, "");
  commitEeprom("eeprom-save");
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
  commitEeprom("eeprom-save");

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
  commitEeprom("eeprom-save");

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
  commitEeprom("eeprom-save");

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
  commitEeprom("eeprom-save");
}

void factoryResetConfig() {
  removeStorageFsBackup();
  forgetWiFiManagerConfig();
  clearThingSpeakConfig();
  saveDeviceConfig(OTA_HOSTNAME, DEFAULT_AP_SSID, DEFAULT_AP_PASS);
}


void applyWiFiStabilitySettings() {
  // Keep WiFi radio fully awake for stable Web/AP/STA/WebSocket operation.
  // This prevents ESP32 power-save from adding high ping times or dropping AP/STA briefly.
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
}

void startConfigAP() {
  const char* apName = configApSsid[0] ? configApSsid : DEFAULT_AP_SSID;
  const char* apPassword = configApPass[0] ? configApPass : DEFAULT_AP_PASS;

  // Do not trust apRunning alone. If the WiFi stack lost AP internally,
  // re-assert AP+STA and start softAP again.
  WiFiMode_t mode = WiFi.getMode();
  IPAddress apIp = WiFi.softAPIP();
  bool apLooksAlive = ((mode == WIFI_AP || mode == WIFI_AP_STA) && apIp != IPAddress(0,0,0,0));
  if (apRunning && apLooksAlive) return;

  WiFi.mode(WIFI_AP_STA);
  applyWiFiStabilitySettings();
  bool ok = false;
  if (strlen(apPassword) >= 8) ok = WiFi.softAP(apName, apPassword);
  else ok = WiFi.softAP(apName);
  delay(20);
  apRunning = ok && WiFi.softAPIP() != IPAddress(0,0,0,0);

  DBG_PRINT("Config AP Ready: ");
  DBG_PRINT(apName);
  DBG_PRINT(" IP: ");
  DBG_PRINTLN(WiFi.softAPIP());
}
void setLastWifiAction(const String& msg) {
  String t = msg;
  t.replace("\r", " ");
  t.replace("\n", " ");
  t.trim();
  if (t.length() == 0) t = "none";
  t.toCharArray(lastWifiActionText, sizeof(lastWifiActionText));
}

String wifiStatusName(wl_status_t st) {
  switch (st) {
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    default: return String("WL_STATUS_") + String((int)st);
  }
}

String wifiDisconnectReasonName(uint8_t reason) {
  switch (reason) {
    case 0: return "none";
    case 1: return "UNSPECIFIED";
    case 2: return "AUTH_EXPIRE";
    case 3: return "AUTH_LEAVE";
    case 4: return "ASSOC_EXPIRE";
    case 5: return "ASSOC_TOOMANY";
    case 8: return "ASSOC_LEAVE";
    case 14: return "MIC_FAILURE";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";
    case 16: return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17: return "IE_IN_4WAY_DIFFERS";
    case 18: return "GROUP_CIPHER_INVALID";
    case 19: return "PAIRWISE_CIPHER_INVALID";
    case 20: return "AKMP_INVALID";
    case 21: return "UNSUPP_RSN_IE_VERSION";
    case 22: return "INVALID_RSN_IE_CAP";
    case 23: return "802_1X_AUTH_FAILED";
    case 24: return "CIPHER_SUITE_REJECTED";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    case 206: return "AP_TSF_RESET";
    case 207: return "ROAMING";
    default: return String("REASON_") + String(reason);
  }
}

void storeWifiDisconnectReason(uint8_t reason) {
  lastStaDisconnectReason = reason;
  String r = wifiDisconnectReasonName(reason);
  r.toCharArray(lastStaDisconnectReasonText, sizeof(lastStaDisconnectReasonText));
}

void onArduinoWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    lastWifiConnectedMs = millis();
    storeWifiDisconnectReason(0);
    setLastWifiAction(String("Event GOT_IP: ") + WiFi.localIP().toString());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiDisconnectedMs = millis();
    storeWifiDisconnectReason((uint8_t)info.wifi_sta_disconnected.reason);
    setLastWifiAction(String("Event DISCONNECTED reason=") + String(lastStaDisconnectReason) + String(" ") + String(lastStaDisconnectReasonText));
  }
}

bool findSavedSsidInScan(uint8_t bestBssid[6]) {
  lastStaScanFound = false;
  lastStaScanChannel = 0;
  lastStaScanRssi = -999;
  strncpy(lastStaScanBssid, "none", sizeof(lastStaScanBssid));
  if (!activeSsid[0]) return false;

  int n = WiFi.scanNetworks(false, true);
  int best = -1;
  int bestRssi = -999;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == String(activeSsid)) {
      int r = WiFi.RSSI(i);
      if (best < 0 || r > bestRssi) { best = i; bestRssi = r; }
    }
    yield();
  }

  if (best >= 0) {
    lastStaScanFound = true;
    lastStaScanRssi = WiFi.RSSI(best);
    lastStaScanChannel = WiFi.channel(best);
    uint8_t* b = WiFi.BSSID(best);
    for (byte i = 0; i < 6; i++) bestBssid[i] = b[i];
    snprintf(lastStaScanBssid, sizeof(lastStaScanBssid), "%02X:%02X:%02X:%02X:%02X:%02X", bestBssid[0], bestBssid[1], bestBssid[2], bestBssid[3], bestBssid[4], bestBssid[5]);
  }
  WiFi.scanDelete();
  return lastStaScanFound;
}

void scheduleWifiReconnect(unsigned long delayMs, bool forceReconnect) {
  wifiReconnectPending = true;
  wifiReconnectDueMs = millis() + delayMs;
  wifiForceReconnectPending = wifiForceReconnectPending || forceReconnect;
  setLastWifiAction(String("Reconnect scheduled in ") + String(delayMs) + String(" ms") + (forceReconnect ? " forced" : ""));
}

String wifiDiagnosticText() {
  wl_status_t st = WiFi.status();
  String s;
  s.reserve(260);
  s += F("STA="); s += wifiStatusName(st); s += F("("); s += String((int)st); s += F(")");
  s += F(" / SavedSSID="); s += String(activeSsid[0] ? activeSsid : "none");
  s += F(" / PassLen="); s += String(lastWifiSavedPassLen ? lastWifiSavedPassLen : strlen(activePass));
  s += F(" / IP="); s += (st == WL_CONNECTED ? WiFi.localIP().toString() : String("none"));
  s += F(" / RSSI="); s += (st == WL_CONNECTED ? String(WiFi.RSSI()) : String("offline"));
  s += F(" / Static="); s += wifiStaticEnabled ? F("ON") : F("OFF");
  s += F(" / Try=");
  if (wifiLastAttemptUsedStatic) s += F("LOW-LEVEL-ESP-WIFI-STATIC-STRICT");
  else s += F("LOW-LEVEL-ESP-WIFI-DHCP");
  s += F(" / LLset="); s += String(lastStaLowLevelSetConfigErr);
  s += F(" / LLconnect="); s += String(lastStaLowLevelConnectErr);
  s += F(" / Scan=");
  if (lastStaScanFound) {
    s += F("FOUND ch="); s += String(lastStaScanChannel);
    s += F(" rssi="); s += String(lastStaScanRssi);
    s += F(" bssid="); s += String(lastStaScanBssid);
  } else {
    s += F("NOT_FOUND");
  }
  s += F(" / Reason="); s += String(lastStaDisconnectReason); s += F(" "); s += String(lastStaDisconnectReasonText);
  s += F(" / STA-MAC="); s += WiFi.macAddress();
  s += F(" / Pending="); s += wifiReconnectPending ? F("YES") : F("NO");
  s += F(" / Last="); s += String(lastWifiActionText);
  return s;
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
  if (m != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
    applyWiFiStabilitySettings();
    apRunning = false;
  }
  if (WiFi.softAPIP() == IPAddress(0,0,0,0)) apRunning = false;
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

const __FlashStringHelper* roleNameForLang(byte role, bool ar) {
  if (!ar) return roleName(role);
  switch (role) {
    case WEB_ROLE_ADMIN: return F("أدمن");
    case WEB_ROLE_ENGINEER: return F("مهندس");
    case WEB_ROLE_OPERATOR: return F("مشغل");
    default: return F("مشاهد");
  }
}

bool roleAtLeast(byte needRole) {
  return currentWebRole >= needRole;
}

bool requireRole(byte needRole) {
  if (!webAuthCheck(false)) return false;
  // ESP32 hardening: default admin/admin is allowed only to open User Management
  // so the first action after flashing must be changing the password.
  if (mustChangeAdminPassword() && String(WEB_USERS[currentWebUserIndex].user) == "admin") {
    String u = webServer.uri();
    if (u != "/users" && u != "/saveusers" && u != "/logout") {
      webServer.sendHeader("Location", sessionUrl("/users"), true);
      webServer.send(302, "text/plain", "Change default admin password first");
      return false;
    }
  }
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
  commitEeprom("eeprom-save");
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
  commitEeprom("eeprom-save");
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

uint16_t fieldBit(byte fieldNumber) {
  if (fieldNumber < 1 || fieldNumber > FIELD_LABEL_COUNT) return 0;
  return (uint16_t)(1U << (fieldNumber - 1));
}

bool isUserOutputField(byte fieldNumber) {
  return fieldNumber == 1 || fieldNumber == 2 || fieldNumber == 5 || fieldNumber == 6;
}

uint16_t sanitizeVisibleMask(uint16_t m) {
  return (uint16_t)(m & USER_FIELD_VISIBLE_DEFAULT_MASK);
}

uint16_t sanitizeControlMask(uint16_t m) {
  return (uint16_t)(m & USER_FIELD_CONTROL_DEFAULT_MASK);
}

bool canUserSeeField(byte userIndex, byte fieldNumber) {
  if (userIndex >= WEB_USER_COUNT) return false;
  if (WEB_USERS[userIndex].role >= WEB_ROLE_ADMIN) return true;
  return (webUserVisibleMask[userIndex] & fieldBit(fieldNumber)) != 0;
}

bool canUserControlField(byte userIndex, byte fieldNumber) {
  if (userIndex >= WEB_USER_COUNT) return false;
  if (!isUserOutputField(fieldNumber)) return false;
  if (WEB_USERS[userIndex].role >= WEB_ROLE_ADMIN) return true;
  if (WEB_USERS[userIndex].role < WEB_ROLE_OPERATOR) return false;
  return (webUserControlMask[userIndex] & fieldBit(fieldNumber)) != 0;
}

bool canCurrentUserSeeField(byte fieldNumber) {
  if (currentWebRole >= WEB_ROLE_ADMIN) return true;
  return canUserSeeField(currentWebUserIndex, fieldNumber);
}

bool canCurrentUserControlField(byte fieldNumber) {
  if (currentWebRole >= WEB_ROLE_ADMIN) return isUserOutputField(fieldNumber);
  return canUserControlField(currentWebUserIndex, fieldNumber);
}

bool userHasAnyVisibleIoField() {
  return canCurrentUserSeeField(1) || canCurrentUserSeeField(2) || canCurrentUserSeeField(5) || canCurrentUserSeeField(6) || canCurrentUserSeeField(7) || canCurrentUserSeeField(8) || canCurrentUserSeeField(9) || canCurrentUserSeeField(10);
}

void setDefaultUserFieldPermissions() {
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    webUserVisibleMask[i] = USER_FIELD_VISIBLE_DEFAULT_MASK;
    webUserControlMask[i] = (WEB_USERS[i].role >= WEB_ROLE_OPERATOR) ? USER_FIELD_CONTROL_DEFAULT_MASK : 0;
  }
  webUserVisibleMask[0] = USER_FIELD_VISIBLE_DEFAULT_MASK;
  webUserControlMask[0] = USER_FIELD_CONTROL_DEFAULT_MASK;
}

void sanitizeAllUserFieldPermissions() {
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    webUserVisibleMask[i] = sanitizeVisibleMask(webUserVisibleMask[i]);
    webUserControlMask[i] = sanitizeControlMask(webUserControlMask[i]);
    if (WEB_USERS[i].role < WEB_ROLE_OPERATOR) webUserControlMask[i] = 0;
  }
  webUserVisibleMask[0] = USER_FIELD_VISIBLE_DEFAULT_MASK;
  webUserControlMask[0] = USER_FIELD_CONTROL_DEFAULT_MASK;
}

void loadUserFieldPermissions() {
  if (EEPROM.read(EEPROM_USERPERM_MAGIC_ADDR) != EEPROM_USERPERM_MAGIC) {
    setDefaultUserFieldPermissions();
    saveUserFieldPermissions();
    return;
  }
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    int addr = EEPROM_USERPERM_DATA_ADDR + (i * EEPROM_USERPERM_RECORD_SIZE);
    webUserVisibleMask[i] = readEepromUInt16(addr, USER_FIELD_VISIBLE_DEFAULT_MASK);
    webUserControlMask[i] = readEepromUInt16(addr + 2, USER_FIELD_CONTROL_DEFAULT_MASK);
  }
  sanitizeAllUserFieldPermissions();
}

void saveUserFieldPermissions() {
  sanitizeAllUserFieldPermissions();
  EEPROM.write(EEPROM_USERPERM_MAGIC_ADDR, EEPROM_USERPERM_MAGIC);
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    int addr = EEPROM_USERPERM_DATA_ADDR + (i * EEPROM_USERPERM_RECORD_SIZE);
    writeEepromUInt16(addr, webUserVisibleMask[i]);
    writeEepromUInt16(addr + 2, webUserControlMask[i]);
  }
  commitEeprom("eeprom-save");
}


bool isSafeTelegramChatIdText(const String& idIn) {
  String id = idIn;
  id.trim();
  if (id.length() == 0) return true;
  if (id.length() > USER_TELEGRAM_CHAT_ID_MAX_LEN) return false;
  for (uint16_t i = 0; i < id.length(); i++) {
    char c = id[i];
    if (i == 0 && c == '-') continue;
    if (c < '0' || c > '9') return false;
  }
  return !(id.length() == 1 && id[0] == '-');
}

void setUserTelegramChatId(byte idx, const String& chatIdIn) {
  if (idx >= WEB_USER_COUNT) return;
  String id = chatIdIn;
  id.trim();
  if (!isSafeTelegramChatIdText(id)) return;
  memset(webUserTelegramChatId[idx], 0, sizeof(webUserTelegramChatId[idx]));
  id.toCharArray(webUserTelegramChatId[idx], sizeof(webUserTelegramChatId[idx]));
}

void setDefaultUserTelegramNotifications() {
  memset(webUserTelegramChatId, 0, sizeof(webUserTelegramChatId));
  memset(webUserTelegramNotify, 0, sizeof(webUserTelegramNotify));
  // Keep old single-admin notification behavior on first upgrade if a global Chat ID already exists.
  if (telegramChatId[0]) {
    String id = String(telegramChatId);
    if (isSafeTelegramChatIdText(id)) {
      setUserTelegramChatId(0, id);
      webUserTelegramNotify[0] = 1;
    }
  }
}

void saveUserTelegramNotifications() {
  if (!userTelegramPrefs.begin("user_tg", false)) return;
  userTelegramPrefs.putUChar("magic", USER_TELEGRAM_NVS_MAGIC);
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    char keyId[8];
    char keyEn[8];
    snprintf(keyId, sizeof(keyId), "id%u", i);
    snprintf(keyEn, sizeof(keyEn), "en%u", i);
    userTelegramPrefs.putString(keyId, String(webUserTelegramChatId[i]));
    userTelegramPrefs.putUChar(keyEn, webUserTelegramNotify[i] ? 1 : 0);
  }
  userTelegramPrefs.end();
}

void loadUserTelegramNotifications() {
  if (!userTelegramPrefs.begin("user_tg", true)) {
    setDefaultUserTelegramNotifications();
    saveUserTelegramNotifications();
    return;
  }
  byte magic = userTelegramPrefs.getUChar("magic", 0);
  if (magic != USER_TELEGRAM_NVS_MAGIC) {
    userTelegramPrefs.end();
    setDefaultUserTelegramNotifications();
    saveUserTelegramNotifications();
    return;
  }
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    char keyId[8];
    char keyEn[8];
    snprintf(keyId, sizeof(keyId), "id%u", i);
    snprintf(keyEn, sizeof(keyEn), "en%u", i);
    webUserTelegramNotify[i] = userTelegramPrefs.getUChar(keyEn, 0) == 1 ? 1 : 0;
    String id = userTelegramPrefs.getString(keyId, "");
    if (isSafeTelegramChatIdText(id)) setUserTelegramChatId(i, id);
    else webUserTelegramChatId[i][0] = 0;
  }
  userTelegramPrefs.end();
}

String userFieldCheck(byte userIndex, byte fieldNumber, bool control) {
  String name = control ? String("c") : String("v");
  name += String(userIndex); name += F("_"); name += String(fieldNumber);
  bool checked = control ? canUserControlField(userIndex, fieldNumber) : canUserSeeField(userIndex, fieldNumber);
  String html = F("<label class='small'><input type='checkbox' name='");
  html += name;
  html += F("' ");
  if (checked) html += F("checked ");
  if (userIndex == 0) html += F("disabled ");
  html += F("> ");
  html += fieldLabelHtml(fieldNumber);
  html += F("</label>");
  return html;
}

String userFieldPermissionsHtml(byte userIndex) {
  String html;
  html.reserve(900);
  html += F("<div class='smsec'><b>Field Permissions</b><div class='sub'>Visible = appears for this user. Control = can switch outputs.</div>");
  html += F("<div class='small'>Visible fields</div><div class='grid2'>");
  html += userFieldCheck(userIndex, 1, false);
  html += userFieldCheck(userIndex, 2, false);
  html += userFieldCheck(userIndex, 5, false);
  html += userFieldCheck(userIndex, 6, false);
  html += userFieldCheck(userIndex, 7, false);
  html += userFieldCheck(userIndex, 8, false);
  html += userFieldCheck(userIndex, 9, false);
  html += userFieldCheck(userIndex, 10, false);
  html += F("</div><div class='small'>Control outputs</div><div class='grid2'>");
  html += userFieldCheck(userIndex, 1, true);
  html += userFieldCheck(userIndex, 2, true);
  html += userFieldCheck(userIndex, 5, true);
  html += userFieldCheck(userIndex, 6, true);
  html += F("</div></div>");
  return html;
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
    html += F("<div class='smsec'><b>Telegram Notifications</b><div class='sub'>Add this user's Telegram Chat ID. Alerts are sent only for fields this user can see.</div>");
    html += F("<label class='small'>Telegram Chat ID</label><input maxlength='"); html += String(USER_TELEGRAM_CHAT_ID_MAX_LEN); html += F("' name='tgid"); html += String(i); html += F("' value='"); html += htmlEscapeText(String(webUserTelegramChatId[i])); html += F("' placeholder='Use /myid in Telegram'>");
    html += F("<label class='small'><input type='checkbox' name='tgn"); html += String(i); html += F("' "); if (webUserTelegramNotify[i]) html += F("checked"); html += F("> Enable Telegram Notifications</label></div>");
    html += userFieldPermissionsHtml(i);
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

  webServer.sendContent(htmlHeader(currentWebLanguage() == TG_LANG_AR ? String("إدارة المستخدمين") : String("User Management")));
  webServer.sendContent(F("<div class='card'><h1>User Management</h1><a class='btn btn2' href='/'>Back</a></div>"));
  webServer.sendContent(activationCodesAdminCardHtml());

  webServer.sendContent(F("<div class='card'><h1>User Management</h1><div class='sub'>Admin only. Max 30 users. Edit username, password, role, and enable state. User 1 stays Admin/enabled for safety.</div>"));
  webServer.sendContent(F("<form method='POST' action='/saveusers'>"));
  webServer.sendContent(sessionHiddenInput());

  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    String row;
    row.reserve(1700);

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
    row += F("</select>");

    row += F("<div class='smsec'><b>Telegram Notifications</b><div class='sub'>Add this user's Telegram Chat ID. Alerts are sent only for fields this user can see.</div>");
    row += F("<label class='small'>Telegram Chat ID</label><input maxlength='"); row += String(USER_TELEGRAM_CHAT_ID_MAX_LEN); row += F("' name='tgid"); row += String(i); row += F("' value='");
    row += htmlEscapeText(String(webUserTelegramChatId[i]));
    row += F("' placeholder='Use /myid in Telegram'>");
    row += F("<label class='small'><input type='checkbox' name='tgn"); row += String(i); row += F("' ");
    if (webUserTelegramNotify[i]) row += F("checked");
    row += F("> Enable Telegram Notifications</label></div>");

    row += userFieldPermissionsHtml(i);
    row += F("</div>");

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
  bool simpleUsersMode = webServer.hasArg("simpleusers");
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    String name = webServer.arg(String("un") + i); name.trim();
    String pass = webServer.arg(String("pw") + i); pass.trim();
    String tgId = webServer.arg(String("tgid") + i); tgId.trim();
    bool tgNotify = webServer.hasArg(String("tgn") + i);
    String roleArgName = String("ro") + i;
    byte role = WEB_USERS[i].role;
    if (webServer.hasArg(roleArgName)) role = sanitizeRole((byte)webServer.arg(roleArgName).toInt());
    if (role < WEB_ROLE_VIEWER || role > WEB_ROLE_ADMIN) role = (i == 0) ? WEB_ROLE_ADMIN : WEB_ROLE_OPERATOR;
    bool wasEmpty = (WEB_USERS[i].user[0] == 0);

    if (i == 0) {
      WEB_USERS[i].enabled = 1;
      WEB_USERS[i].role = WEB_ROLE_ADMIN;
      if (isSafeUsername(name) && !usernameExists(name, i)) setWebUserName(i, name);
      if (pass.length() > 0) setWebUserPassword(i, pass);
      if (isSafeTelegramChatIdText(tgId)) setUserTelegramChatId(i, tgId);
      webUserTelegramNotify[i] = tgNotify ? 1 : 0;
      continue;
    }

    if (name.length() == 0) {
      WEB_USERS[i].enabled = 0;
      WEB_USERS[i].user[0] = 0;
      WEB_USERS[i].pass[0] = 0;
      WEB_USERS[i].role = WEB_ROLE_VIEWER;
      webUserTelegramChatId[i][0] = 0;
      webUserTelegramNotify[i] = 0;
      continue;
    }

    if (isSafeUsername(name) && !usernameExists(name, i)) setWebUserName(i, name);
    if (pass.length() > 0) setWebUserPassword(i, pass);
    if (WEB_USERS[i].pass[0] == 0) strcpy(WEB_USERS[i].pass, "1234");
    WEB_USERS[i].role = role;
    WEB_USERS[i].enabled = webServer.hasArg(String("en") + i) ? 1 : 0;
    if (isSafeTelegramChatIdText(tgId)) setUserTelegramChatId(i, tgId);
    webUserTelegramNotify[i] = tgNotify ? 1 : 0;

    if (wasEmpty && WEB_USERS[i].user[0] && simpleUsersMode) {
      if (WEB_USERS[i].role < WEB_ROLE_OPERATOR) WEB_USERS[i].role = WEB_ROLE_OPERATOR;
      webUserVisibleMask[i] = USER_FIELD_VISIBLE_DEFAULT_MASK;
      webUserControlMask[i] = USER_FIELD_CONTROL_DEFAULT_MASK;
    }

    if (!simpleUsersMode) {
      uint16_t vm = 0;
      uint16_t cm = 0;
      byte visibleFields[] = {1, 2, 5, 6, 7, 8, 9, 10};
      for (byte k = 0; k < sizeof(visibleFields); k++) {
        byte f = visibleFields[k];
        if (webServer.hasArg(String("v") + i + "_" + f)) vm |= fieldBit(f);
      }
      byte controlFields[] = {1, 2, 5, 6};
      for (byte k = 0; k < sizeof(controlFields); k++) {
        byte f = controlFields[k];
        if (webServer.hasArg(String("c") + i + "_" + f)) cm |= fieldBit(f);
      }
      webUserVisibleMask[i] = vm;
      webUserControlMask[i] = (WEB_USERS[i].role >= WEB_ROLE_OPERATOR) ? cm : 0;
    }
  }
  webUserVisibleMask[0] = USER_FIELD_VISIBLE_DEFAULT_MASK;
  webUserControlMask[0] = USER_FIELD_CONTROL_DEFAULT_MASK;
  saveWebUsersConfig();
  saveUserFieldPermissions();
  saveUserTelegramNotifications();
  saveUserPagePersistentConfig("save-users-page");
  autoBackupAfterSave("Users page saved");
  String html = htmlHeader("Users Saved");
  html += F("<div class='card'><h1>Users Saved</h1><div class='sub'>Users, roles, passwords, field permissions and Telegram notifications saved. If your current password changed, login again.</div>");
  html += F("<div class='row'><span class='k'>User Page Storage</span><span class='v'>"); html += htmlEscapeText(userPageStorageStatusText()); html += F("</span></div>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/users"); html += F("'>Back to Users</a><a class='btn btn2' href='/'>Home</a></div>");
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

bool findWebUserAnyState(const String& user, const String& pass, byte& foundIndex) {
  String uu = user;
  String pp = pass;
  uu.trim();
  pp.trim();
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    if (WEB_USERS[i].user[0] == 0 || WEB_USERS[i].pass[0] == 0) continue;
    // Username is case-insensitive like the duplicate-name check in User Management.
    // Password remains exact, except for accidental leading/trailing spaces from Telegram/web forms.
    if (uu.equalsIgnoreCase(String(WEB_USERS[i].user)) && pp.equals(String(WEB_USERS[i].pass))) {
      foundIndex = i;
      return true;
    }
  }
  return false;
}

bool findWebUser(const String& user, const String& pass, byte& foundIndex) {
  byte idx = 255;
  if (!findWebUserAnyState(user, pass, idx)) return false;
  if (idx >= WEB_USER_COUNT || !WEB_USERS[idx].enabled) return false;
  foundIndex = idx;
  return true;
}

void clearSessionSlot(byte slot) {
  if (slot >= MAX_WEB_SESSIONS) return;
  webSessions[slot].token = "";
  webSessions[slot].userIndex = 255;
  webSessions[slot].createdMs = 0;
  webSessions[slot].lastSeenMs = 0;
  webSessions[slot].ip = IPAddress(0, 0, 0, 0);
  webSessions[slot].lang = 0;
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
  webSessions[slot].lang = 0;
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
  commitEeprom("eeprom-save");
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

byte webLanguageFromCode(const String& code, byte fallback) {
  String c = code;
  c.trim();
  c.toLowerCase();
  if (c == "ar" || c == "arabic") return TG_LANG_AR;
  if (c == "en" || c == "english") return TG_LANG_EN;
  return fallback;
}

byte webLanguageForUserIndex(byte idx) {
  if (idx >= WEB_USER_COUNT) return TG_LANG_EN;
  char key[8];
  snprintf(key, sizeof(key), "u%02u", idx);
  byte lang = TG_LANG_UNKNOWN;
  if (webLangPrefs.begin("web_lang", true)) {
    lang = webLangPrefs.getUChar(key, TG_LANG_UNKNOWN);
    webLangPrefs.end();
  }
  if (lang == TG_LANG_AR || lang == TG_LANG_EN) return lang;

  // If the user already selected Telegram language, reuse it as the web default.
  if (webUserTelegramChatId[idx][0]) {
    lang = telegramLanguageForChat(String(webUserTelegramChatId[idx]));
    if (lang == TG_LANG_AR || lang == TG_LANG_EN) return lang;
  }

  // Keep old web behavior as default until the user chooses Arabic.
  return TG_LANG_EN;
}

bool webUserIsArabic(byte idx) {
  return webLanguageForUserIndex(idx) == TG_LANG_AR;
}

byte currentWebLanguage() {
  // Prefer the current session language so the Home page changes immediately
  // after pressing the language button, even before/without NVS reload.
  if (currentSessionSlot < MAX_WEB_SESSIONS && webSessions[currentSessionSlot].active) {
    byte l = webSessions[currentSessionSlot].lang;
    if (l == TG_LANG_AR || l == TG_LANG_EN) return l;
  }
  if (currentWebUserIndex < WEB_USER_COUNT) return webLanguageForUserIndex(currentWebUserIndex);
  return TG_LANG_EN;
}

String webTextForLang(byte lang, const char* en, const char* ar) {
  return (lang == TG_LANG_AR) ? String(ar) : String(en);
}

void webSetLanguageForUserIndex(byte idx, byte lang) {
  if (idx >= WEB_USER_COUNT) return;
  if (!(lang == TG_LANG_AR || lang == TG_LANG_EN)) return;
  char key[8];
  snprintf(key, sizeof(key), "u%02u", idx);
  if (webLangPrefs.begin("web_lang", false)) {
    webLangPrefs.putUChar(key, lang);
    webLangPrefs.end();
  }
  if (currentSessionSlot < MAX_WEB_SESSIONS && webSessions[currentSessionSlot].active && webSessions[currentSessionSlot].userIndex == idx) {
    webSessions[currentSessionSlot].lang = lang;
  }
  if (webUserTelegramChatId[idx][0]) {
    telegramSetLanguageForChat(String(webUserTelegramChatId[idx]), lang);
  }
}

String webLanguageCardHtml() {
  byte lang = currentWebLanguage();
  bool ar = (lang == TG_LANG_AR);
  String html;
  html.reserve(520);
  html += F("<div class='card'><h1>");
  html += ar ? F("اللغة") : F("Language");
  html += F("</h1><div class='sub'>");
  html += ar ? F("اللغة الحالية: العربية. اختر لغة صفحة الويب وسيتم حفظها لهذا المستخدم ومزامنتها مع تيليجرام إذا كان له Chat ID.") : F("Current language: English. Choose web page language. It is saved for this user and synced with Telegram when a Chat ID exists.");
  html += F("</div><div class='grid2'>");
  html += F("<a class='btn btn2"); if (ar) html += F(" onbtn"); html += F("' href='"); html += sessionUrl("/setweblang?lang=ar"); html += F("'>🇪🇬 العربية</a>");
  html += F("<a class='btn btn2"); if (!ar) html += F(" onbtn"); html += F("' href='"); html += sessionUrl("/setweblang?lang=en"); html += F("'>🇬🇧 English</a>");
  html += F("</div></div>");
  return html;
}

void handleSetWebLanguage() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  byte currentLang = webLanguageForUserIndex(currentWebUserIndex);
  byte lang = webLanguageFromCode(webServer.arg("lang"), currentLang);
  webSetLanguageForUserIndex(currentWebUserIndex, lang);

  // Force the browser to reload Home after changing language.
  // Some phones keep the previous Home HTML in cache when the URL is still exactly "/".
  String back = String("/?langset=") + (lang == TG_LANG_AR ? String("ar") : String("en")) + String("&t=") + String(millis());
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.sendHeader("Location", sessionUrl(back), true);
  webServer.send(302, "text/plain", "");
}


void sendLoginPage() {
  byte lang = webLanguageFromCode(webServer.arg("lang"), TG_LANG_EN);
  bool ar = (lang == TG_LANG_AR);
  String html = htmlHeader(ar ? String("تسجيل الدخول") : String("Login"));
  html.reserve(3600);
  html += F("<div class='card'><h1>"); html += ar ? F("تسجيل الدخول") : F("Login"); html += F("</h1>");
  html += F("<div class='sub'>");
  html += ar ? F("تسجيل دخول متعدد الجلسات. الحد الأقصى ") : F("Multi Session login. Max ");
  html += String(MAX_WEB_SESSIONS);
  html += ar ? F(" أجهزة. عند الامتلاء يتم إخراج أقدم جهاز.") : F(" devices. New login kicks oldest device if full.");
  html += F("</div>");
  html += F("<div class='grid2' style='margin:10px 0'>");
  html += F("<a class='btn btn2"); if (ar) html += F(" onbtn"); html += F("' href='/login?lang=ar'>🇪🇬 العربية</a>");
  html += F("<a class='btn btn2"); if (!ar) html += F(" onbtn"); html += F("' href='/login?lang=en'>🇬🇧 English</a>");
  html += F("</div>");
  if (loginIntroEnabled && loginIntroText.length() > 0) {
    html += F("<div class='card'><h1>"); html += ar ? F("عن الجهاز") : F("About this device"); html += F("</h1><div class='sub' style='font-size:15px;line-height:1.7;color:#cbd5e1'>");
    html += htmlEscapeText(loginIntroText);
    html += F("</div></div>");
  }
  if (webServer.hasArg("bad")) html += ar ? F("<div class='msg warn'>اسم المستخدم أو كلمة المرور غير صحيحة</div>") : F("<div class='msg warn'>Wrong username or password</div>");
  if (webServer.hasArg("out")) html += ar ? F("<div class='msg ok'>تم تسجيل الخروج بنجاح</div>") : F("<div class='msg ok'>Logged out successfully</div>");
  html += F("<form method='POST' action='/dologin'>");
  html += F("<input type='hidden' name='lang' value='"); html += ar ? F("ar") : F("en"); html += F("'>");
  if (webServer.hasArg("r")) { html += F("<input type='hidden' name='r' value='"); html += webServer.arg("r"); html += F("'>"); }
  html += F("<label class='small'>"); html += ar ? F("اسم المستخدم") : F("Username"); html += F("</label><input name='u' maxlength='");
  html += String(WEB_USER_MAX_LEN);
  html += F("' autocomplete='username'>");
  html += F("<label class='small'>"); html += ar ? F("كلمة المرور") : F("Password"); html += F("</label><input type='password' name='p' maxlength='");
  html += String(WEB_PASS_MAX_LEN);
  html += F("' autocomplete='current-password'>");
  html += F("<button class='btn' type='submit'>"); html += ar ? F("دخول") : F("Login"); html += F("</button>");
  html += F("</form></div>");
  html += htmlFooter();

  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.send(200, "text/html", html);
}

void handleDoLogin() {
  maintainMonthlyUserSubscriptionDisable(true);

  String u = webServer.arg("u"); u.trim();
  String p = webServer.arg("p"); p.trim();

  byte idx = 255;
  if (findWebUser(u, p, idx)) {
    setLoggedInUser(idx);
    if (webServer.hasArg("lang")) {
      byte selectedWebLang = webLanguageFromCode(webServer.arg("lang"), webLanguageForUserIndex(idx));
      webSetLanguageForUserIndex(idx, selectedWebLang);
    } else if (currentSessionSlot < MAX_WEB_SESSIONS) {
      webSessions[currentSessionSlot].lang = webLanguageForUserIndex(idx);
    }
    String cookie = "ESPSESS=" + webSessionToken + "; Path=/";
    webServer.sendHeader("Set-Cookie", cookie);
    String ret = webServer.arg("r");
    if (ret.length() == 0 || ret.startsWith("/login") || ret.startsWith("/logout")) ret = "/";

    byte daysLeft = 0;
    if (idx > 0 && subscriptionWarningActive(daysLeft)) {
      String tgMsg = subscriptionWarningTextTelegram(String(webUserTelegramChatId[idx]), String(WEB_USERS[idx].user), daysLeft);
      notifyUserSubscriptionMessage(idx, tgMsg);
      sendSubscriptionWarningPage(String(WEB_USERS[idx].user), daysLeft, ret);
      return;
    }

    webServer.sendHeader("Location", ret, true);
    webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    webServer.send(302, "text/plain", "");
    return;
  }

  byte disabledIdx = 255;
  if (findWebUserAnyState(u, p, disabledIdx) && disabledIdx < WEB_USER_COUNT && !WEB_USERS[disabledIdx].enabled) {
    String tgMsg = subscriptionDisabledTextTelegram(String(webUserTelegramChatId[disabledIdx]), String(WEB_USERS[disabledIdx].user));
    notifyUserSubscriptionMessage(disabledIdx, tgMsg);
    sendSubscriptionDisabledPage(String(WEB_USERS[disabledIdx].user));
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
  h.reserve(1700);
  bool arPage = (currentWebLanguage() == TG_LANG_AR);
  h += F("<!doctype html><html lang='");
  if (arPage) h += F("ar' dir='rtl'");
  else h += F("en' dir='ltr'");
  h += F("><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>"); h += title; h += F("</title>");
  h += F("<style>");
  h += F("*{box-sizing:border-box}body{margin:0;font-family:Arial,Tahoma,sans-serif;background:#0f172a;color:#e5e7eb}.wrap{max-width:520px;margin:auto;padding:16px}.card{background:#111c33;border:1px solid #334155;border-radius:18px;padding:16px;margin-top:14px}h1{font-size:24px;margin:4px 0}.sub,.k,.small,.msg{color:#94a3b8}.row{display:flex;justify-content:space-between;border-bottom:1px solid #26364f;padding:9px 0;font-size:14px}.v{font-weight:700;text-align:right}input,select,textarea{width:100%;padding:12px;border-radius:12px;border:1px solid #475569;background:#0b1220;color:#e5e7eb;margin:8px 0}textarea{min-height:140px;resize:vertical;line-height:1.5}.btn{width:100%;border:0;border-radius:14px;padding:13px;font-size:16px;font-weight:700;background:#22c55e;color:#001018;margin-top:8px}.btn2{display:block;text-align:center;text-decoration:none;background:#1e293b;color:#e5e7eb;border:1px solid #475569}.bar{height:12px;background:#0b1220;border-radius:99px;overflow:hidden;border:1px solid #334155;margin-top:12px}.fill{height:100%;width:0;background:#22c55e}.warn{color:#fbbf24}.ok,.on{color:#22c55e}.bad,.off{color:#ef4444}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}.pill{display:inline-block;padding:4px 10px;border-radius:99px;background:#0b1220;border:1px solid #334155;font-weight:700}.oc{display:grid;gap:12px;margin-top:12px}.ocbox{padding:14px;border-radius:18px;background:#0b1220;border:1px solid #334155}.octop{display:flex;align-items:center;justify-content:space-between;gap:10px}.onbtn{background:#22c55e!important;color:#03140a!important}.offbtn{background:#ef4444!important;color:#fff!important}.dot{width:18px;height:18px;border-radius:50%;display:inline-block;margin-left:8px;box-shadow:0 0 10px currentColor}.dot.on{background:#22c55e}.dot.off{background:#ef4444}.bigstate{font-size:22px;font-weight:900}.small{font-size:12px}input[type=checkbox],input[type=radio]{width:auto;margin:6px}.modepick{display:grid;gap:8px;margin:10px 0}.modepick label{padding:10px;border:1px solid #334155;border-radius:12px;background:#0b1220}.smsec{border:1px solid #26364f;border-radius:14px;padding:10px;margin:10px 0}.smsec.disabled{opacity:.38}.dayrow{display:grid;grid-template-columns:74px 74px 1fr 1fr;gap:7px;align-items:center}.dayrow .daylbl{grid-row:span 2}.dayhdr{display:grid;grid-template-columns:74px 74px 1fr 1fr;gap:7px;align-items:center}.timergrid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}.topnav{margin-bottom:8px}.homebtn{margin-top:0;background:#334155;color:#e5e7eb}.navgrid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}.navcard{display:block;text-decoration:none;color:#e5e7eb;padding:14px;border:1px solid #334155;border-radius:16px;background:#0b1220}.navcard b{display:block;font-size:15px;margin-bottom:4px}.navcard span{display:block;color:#94a3b8;font-size:12px}.statgrid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.stat{background:#0b1220;border:1px solid #334155;border-radius:16px;padding:12px}.stat .n{font-size:20px;font-weight:900}.sectiontitle{margin-top:18px;color:#cbd5e1;font-weight:900}.syncbox{border:1px solid #334155;border-radius:14px;padding:12px;background:#0b1220;margin:10px 0}@media(min-width:800px){.wrap{max-width:860px}.oc{grid-template-columns:1fr 1fr}.navgrid{grid-template-columns:repeat(3,1fr)}}");
  h += F(".sensorcard{background:linear-gradient(135deg,#111c33,#0b1f3a);border-color:#2563eb}.sensegrid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:12px}.senseitem{background:#0b1220;border:1px solid #334155;border-radius:18px;padding:16px;text-align:center}.senseicon{font-size:30px;line-height:1}.senselabel{color:#94a3b8;font-size:13px;margin-top:6px}.senseval{font-size:34px;font-weight:900;margin-top:6px;letter-spacing:-1px}.senseval small{font-size:17px;color:#cbd5e1}.tempc{color:#fb923c}.humc{color:#38bdf8}.statuslist{display:grid;gap:8px;margin-top:12px}.statusrow2{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:11px 12px;border:1px solid #26364f;border-radius:14px;background:#0b1220}.stname{font-weight:800}.statepill{display:inline-block;min-width:76px;text-align:center;padding:6px 11px;border-radius:99px;font-weight:900;border:1px solid #334155}.onpill{background:rgba(34,197,94,.16);color:#22c55e;border-color:#15803d}.offpill{background:rgba(239,68,68,.16);color:#ef4444;border-color:#b91c1c}.switchgrid{display:grid;gap:10px;margin-top:12px}.switchcard{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:13px;border-radius:16px;background:#0b1220;border:1px solid #334155}.sw{position:relative;display:inline-block;width:74px;height:40px;flex:0 0 auto}.sw input{opacity:0;width:0;height:0}.sld{position:absolute;cursor:pointer;inset:0;background:#475569;border-radius:999px;transition:.2s;box-shadow:inset 0 0 0 1px #64748b}.sld:before{position:absolute;content:'';height:32px;width:32px;left:4px;top:4px;background:#e5e7eb;border-radius:50%;transition:.2s;box-shadow:0 2px 8px rgba(0,0,0,.35)}.sw input:checked+.sld{background:#22c55e}.sw input:checked+.sld:before{transform:translateX(34px)}.techcard{opacity:.95}@media(max-width:420px){.sensegrid{grid-template-columns:1fr}.senseval{font-size:30px}.switchcard{align-items:flex-start}.sw{margin-top:2px}}");
  if (arPage) h += F("html,body{direction:rtl;text-align:right}.v{text-align:left}.row{direction:rtl}.topnav,.btn,.navcard{text-align:center}.switchcard,.octop,.statusrow2{direction:rtl}.dot{margin-left:0;margin-right:8px}");
  h += F("</style>");
  if (arPage) {
    h += F("<script>(function(){const M={\"Home\":\"الرئيسية\",\"Back\":\"رجوع\",\"Logout\":\"تسجيل الخروج\",\"Restart\":\"إعادة تشغيل\",\"Restart Device\":\"إعادة تشغيل الجهاز\",\"Restart Now\":\"إعادة التشغيل الآن\",\"Navigation\":\"التنقل\",\"Dashboard\":\"لوحة التحكم\",\"Communications\":\"الاتصالات\",\"System Health\":\"صحة النظام\",\"Users\":\"المستخدمون\",\"Update / Backup\":\"التحديث والنسخ الاحتياطي\",\"Realtime / IO\":\"البيانات اللحظية / المداخل والمخارج\",\"Automation\":\"الأتمتة\",\"Automation / Schedules\":\"الأتمتة والجداول\",\"Temperature & Humidity\":\"درجة الحرارة والرطوبة\",\"Temperature\":\"درجة الحرارة\",\"Humidity\":\"الرطوبة\",\"Sensor\":\"الحساس\",\"Current Temperature\":\"درجة الحرارة الحالية\",\"Current Humidity\":\"الرطوبة الحالية\",\"Current Sensor\":\"الحساس الحالي\",\"Sensor Type\":\"نوع الحساس\",\"Sensor Settings\":\"إعدادات الحساس\",\"Save Sensor Type\":\"حفظ نوع الحساس\",\"Allowed Status\":\"الحالات المصرح بها\",\"Quick Control\":\"التحكم السريع\",\"Ready\":\"جاهز\",\"ON\":\"يعمل");
    h += F("\",\"OFF\":\"متوقف\",\"ACTIVE\":\"نشط\",\"Pending\":\"قيد المزامنة\",\"Synced\":\"تمت المزامنة\",\"View only\":\"عرض فقط\",\"VIEW ONLY\":\"عرض فقط\",\"Technical Status\":\"الحالة الفنية\",\"Network\":\"الشبكة\",\"Network State\":\"حالة الشبكة\",\"WiFi Mode\":\"وضع الواي فاي\",\"Mode\":\"الوضع\",\"Saved SSID\":\"الشبكة المحفوظة\",\"STA IP\":\"عنوان STA IP\",\"AP IP\":\"عنوان AP IP\",\"AP SSID\":\"اسم شبكة AP\",\"AP Clients\":\"أجهزة AP\",\"AP Watchdog\":\"مراقبة AP\",\"AP Restart Count\":\"عدد إعادة تشغيل AP\",\"Max Loop Gap\":\"أكبر تأخير في اللوب\",\"Last Slow Task\":\"آخر مهمة بطيئة\",\"STA IP Mode\":\"وضع IP للـ STA\",\"Signal\":\"الإشارة\",\"Device Name\":\"اسم الجهاز\",\"Flash Size\":\"حجم الفلاش\",\"Free Sketch Space\":\"مساحة البرنامج المتاحة\",\"Firmware\":\"البرنامج\",\"Chip ID\":\"معرف الشريحة\",\"GitHub OTA\":\"تحديث GitHub\",\"Auto Check\":\"الفحص التلقائي\",\"Current User\":\"المستخدم الحالي\",\"Login Role\":\"صلاحية الدخول\",\"Last Login\":\"آخر دخول\",\"User\":\"المستخدم\",\"Role\":\"الصلاحية\",\"Interface L");
    h += F("anguage\":\"لغة الواجهة\",\"Admin Quick Actions\":\"إجراءات الأدمن السريعة\",\"User Management\":\"إدارة المستخدمين\",\"Users Saved\":\"تم حفظ المستخدمين\",\"Save Users\":\"حفظ المستخدمين\",\"Enabled\":\"مفعل\",\"Username\":\"اسم المستخدم\",\"New Password\":\"كلمة مرور جديدة\",\"Password\":\"كلمة المرور\",\"Telegram Notifications\":\"تنبيهات تيليجرام\",\"Telegram Chat ID\":\"معرف محادثة تيليجرام\",\"Enable Telegram Notifications\":\"تفعيل تنبيهات تيليجرام\",\"Field Permissions\":\"صلاحيات الحقول\",\"Visible fields\":\"الحقول الظاهرة\",\"Control outputs\":\"التحكم في المخارج\",\"Visible = appears for this user. Control = can switch outputs.\":\"ظاهر = يظهر لهذا المستخدم. تحكم = يستطيع تشغيل وإيقاف المخارج.\",\"Main Admin is always enabled.\":\"الأدمن الرئيسي مفعل دائمًا.\",\"empty user slot\":\"مكان مستخدم فارغ\",\"Leave empty to keep current password\":\"اتركها فارغة للإبقاء على كلمة المرور الحالية\",\"Use /myid in Telegram\":\"استخدم /myid في تيليجرام\",\"Subscript");
    h += F("ion / Activation\":\"الاشتراك والتفعيل\",\"Monthly Auto Disable Users\":\"تعطيل المستخدمين شهريًا تلقائيًا\",\"Activation Codes\":\"أكواد التفعيل\",\"Available Codes\":\"الأكواد المتاحة\",\"Used Codes\":\"الأكواد المستخدمة\",\"Expired Codes\":\"الأكواد المنتهية\",\"Generate Codes\":\"توليد أكواد\",\"Download Codes TXT\":\"تحميل الأكواد TXT\",\"No available codes.\":\"لا توجد أكواد متاحة.\",\"No used codes yet.\":\"لا توجد أكواد مستخدمة بعد.\",\"Created at\":\"تاريخ الإنشاء\",\"Used by\":\"استخدم بواسطة\",\"Used at\":\"وقت الاستخدام\",\"Method\":\"الطريقة\",\"Clear Used Codes\":\"مسح الأكواد المستخدمة\",\"How many codes?\":\"عدد الأكواد؟\",\"Firmware Update\":\"تحديث البرنامج\",\"Update Firmware\":\"تحديث البرنامج\",\"Waiting for file...\":\"في انتظار الملف...\",\"GitHub Auto Update Options\":\"خيارات تحديث GitHub التلقائي\",\"GitHub Auto Update\":\"تحديث GitHub التلقائي\",\"Auto Check Interval\":\"فاصل الفحص التلقائي\",\"Manual only\":\"يدوي فقط\",\"Current Auto Mode\":\"وضع التح");
    h += F("ديث الحالي\",\"Last Update Check\":\"آخر فحص تحديث\",\"Last Message\":\"آخر رسالة\",\"Save Update Options\":\"حفظ خيارات التحديث\",\"Manual GitHub Check\":\"فحص GitHub يدوي\",\"Backup & Restore\":\"نسخ احتياطي واستعادة\",\"Download Settings Backup\":\"تحميل نسخة احتياطية للإعدادات\",\"Upload / Restore Backup\":\"رفع / استعادة النسخة\",\"Restore Backup\":\"استعادة نسخة احتياطية\",\"Back Home\":\"رجوع للرئيسية\",\"WiFi / Network Setup\":\"إعداد الشبكة والواي فاي\",\"WiFi, ThingSpeak, DuckDNS, device and network settings. AP access:\":\"إعدادات الواي فاي و ThingSpeak و DuckDNS والجهاز والشبكة. دخول AP:\",\"DuckDNS Settings\":\"إعدادات DuckDNS\",\"Domain\":\"الدومين\",\"Token\":\"التوكن\",\"Last Update\":\"آخر تحديث\",\"Save DuckDNS\":\"حفظ DuckDNS\",\"Clear DuckDNS\":\"مسح DuckDNS\",\"Update DuckDNS Now\":\"تحديث DuckDNS الآن\",\"Web Server Port\":\"منفذ صفحة الويب\",\"Port\":\"المنفذ\",\"Save Web Port\":\"حفظ منفذ الويب\",\"Network Setup\":\"إعداد الشبكة\",\"Scan Networks\":\"بحث");
    h += F(" عن الشبكات\",\"No scan yet\":\"لم يتم البحث بعد\",\"SSID\":\"اسم الشبكة\",\"Use Static STA IP\":\"استخدام IP ثابت للـ STA\",\"Static IP\":\"IP ثابت\",\"Gateway\":\"البوابة\",\"Subnet\":\"قناع الشبكة\",\"DNS\":\"DNS\",\"Use Current IP\":\"استخدام IP الحالي\",\"Save & Connect\":\"حفظ واتصال\",\"Delete / Forget WiFi\":\"حذف / نسيان الواي فاي\",\"ThingSpeak Settings\":\"إعدادات ThingSpeak\",\"Channel ID\":\"رقم القناة\",\"Write API Key\":\"مفتاح الكتابة\",\"Read API Key\":\"مفتاح القراءة\",\"Command Read Interval\":\"فاصل قراءة الأوامر\",\"Status\":\"الحالة\",\"Configured\":\"تم الإعداد\",\"Not configured\":\"غير مضبوط\",\"Command Interval\":\"فاصل الأوامر\",\"Save ThingSpeak\":\"حفظ ThingSpeak\",\"Clear ThingSpeak\":\"مسح ThingSpeak\",\"Device Settings\":\"إعدادات الجهاز\",\"AP Name\":\"اسم AP\",\"AP Password\":\"كلمة مرور AP\",\"Save Device Settings\":\"حفظ إعدادات الجهاز\",\"Open User Management\":\"فتح إدارة المستخدمين\",\"System Control\":\"تحكم النظام\",\"Factory Reset\":\"إعادة ضبط المصنع\",\"Ou");
    h += F("tput Schedules / Manual Options\":\"جداول المخارج / الخيارات اليدوية\",\"Device Date / Time\":\"تاريخ / وقت الجهاز\",\"Schedule Day\":\"يوم الجدول\",\"Manual Auto-OFF Time\":\"وقت الإغلاق اليدوي التلقائي\",\"Manual Auto-OFF Enabled\":\"الإغلاق اليدوي التلقائي مفعل\",\"Last Auto-OFF Run\":\"آخر تشغيل للإغلاق التلقائي\",\"Select Output\":\"اختر المخرج\",\"Current State\":\"الحالة الحالية\",\"Manual Timer Remaining\":\"المتبقي من المؤقت اليدوي\",\"Manual Options\":\"الخيارات اليدوية\",\"Manual Options Storage\":\"تخزين خيارات التشغيل اليدوي\",\"EEPROM manual block + NVS mirror\":\"مخزن موحد NVS باسم manAuto + نسخة توافق قديمة\",\"Close manual ON at selected daily time\":\"إغلاق التشغيل اليدوي في الوقت اليومي المحدد\",\"Close after manual ON duration\":\"إغلاق بعد مدة التشغيل اليدوي\",\"Manual Auto-Off Timer duration (minutes)\":\"مدة مؤقت الإغلاق اليدوي بالدقائق\",\"Schedule Automation\":\"أتمتة الجدول\",\"Enable automation for this output\":\"تفعيل الأتمتة لهذا المخرج\",\"Enable weekly schedule\":\"تفعيل الجدول الأسبوعي\",\"Repeat\":\"التكرار\",\"Weekly Repeat\":\"تكرار أسبوعي\",\"Once only\":\"مرة واحدة ");
    h += F("فقط\",\"Automation Sync to ThingSpeak\":\"مزامنة الأتمتة مع ThingSpeak\",\"Current Automation Sync\":\"حالة مزامنة الأتمتة الحالية\",\"Day\":\"اليوم\",\"State\":\"الحالة\",\"Save Schedule / Manual Options\":\"حفظ الجدول والخيارات اليدوية\",\"Output 3 Smart\":\"المخرج 3 الذكي\",\"Current Mode\":\"الوضع الحالي\",\"Clock\":\"الساعة\",\"Disabled / LED separate\":\"معطل / LED مستقل\",\"Manual / Schedule\":\"يدوي / جدول\",\"Timer\":\"مؤقت\",\"Control what you wish\":\"تحكم فيما تريد\",\"ThingSpeak Sync\":\"مزامنة ThingSpeak\",\"Ras Sedr Tide\":\"مد وجزر رأس سدر\",\"Open Tide / المد والجزر\":\"فتح المد والجزر\",\"Telegram Bot\":\"بوت تيليجرام\",\"Open Telegram\":\"فتح تيليجرام\",\"Language\":\"اللغة\",\"ONLINE - Internet services active\":\"متصل - خدمات الإنترنت تعمل\",\"LOCAL MODE - AP/web active, reconnect every 60 sec\":\"وضع محلي - صفحة الويب تعمل وسيعاد الاتصال كل 60 ثانية\",\"LOCAL MODE - no saved WiFi, AP/web active\":\"وضع محلي - لا توجد شبكة محفوظة وصفحة الويب تعمل\",\"");
    h += F("Not saved\":\"غير محفوظ\",\"Not connected\":\"غير متصل\",\"Static\":\"ثابت\",\"DHCP\":\"تلقائي DHCP\",\"OFF - schedule/timer changes are local only\":\"إيقاف - تغييرات الجدول/المؤقت محلية فقط\",\"ON - schedule/timer changes can sync to ThingSpeak\":\"تشغيل - تغييرات الجدول/المؤقت يمكن مزامنتها مع ThingSpeak\",\"Restarting\":\"جاري إعادة التشغيل\",\"Restarting...\":\"جاري إعادة التشغيل...\",\"Web Port Saved\":\"تم حفظ منفذ الويب\",\"WiFi Saved\":\"تم حفظ الواي فاي\",\"WiFi Deleted\":\"تم حذف الواي فاي\",\"Settings saved. AP/web stayed active. STA reconnect started. Static IP is strict when enabled.\":\"تم حفظ الإعدادات. شبكة AP والويب مستمرة. بدأ إعادة اتصال STA. عند تفعيل IP ثابت لن يتم التحويل إلى DHCP.\",\"Saved network deleted. Local Mode AP/web is still active on 192.168.4.1 without restart.\":\"تم حذف الشبكة المحفوظة. الوضع المحلي وصفحة الويب ما زالا يعملان على 192.168.4.1 بدون إعادة تشغيل.\",\"Back to WiFi\":\"رجوع للواي فاي\",\"Sending...\":\"جاري ال");
    h += F("إرسال...\",\"Applied\":\"تم التنفيذ\",\"No change\":\"لا يوجد تغيير\",\"Failed/session\":\"فشل/الجلسة\",\"Connection error\":\"خطأ اتصال\",\"Update failed\":\"فشل التحديث\",\"Scan failed\":\"فشل البحث\",\"Scanning...\":\"جاري البحث...\",\"Select network\":\"اختر شبكة\"};function A(n){return n&&/^(SCRIPT|STYLE|TEXTAREA)$/.test(n.nodeName)}function R(s){return M[s]||s}function T(x){if(!x)return;let t=x.nodeValue, q=t.trim();if(q&&M[q])x.nodeValue=t.replace(q,M[q])}function E(e){['placeholder','title','value'].forEach(function(a){let v=e.getAttribute&&e.getAttribute(a);if(v&&M[v])e.setAttribute(a,M[v])})}function W(){document.documentElement.dir='rtl';document.documentElement.lang='ar';if(document.body){document.body.style.direction='rtl';document.body.style.textAlign='right'}let w=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT,{acceptNode:function(n){return A(n.parentNode)?NodeFilter.FILTER_REJECT:NodeFilter");
    h += F(".FILTER_ACCEPT}}),n,arr=[];while(n=w.nextNode())arr.push(n);arr.forEach(T);document.querySelectorAll('input,textarea,option,button,a').forEach(E)}document.addEventListener('DOMContentLoaded',function(){W();setTimeout(W,400);setTimeout(W,1500);});})();</script>");
  }
  h += F("</head><body><div class='wrap'>");
  h += F("<div class='topnav'><a class='btn btn2 homebtn' href='");
  h += sessionUrl("/");
  h += F("'>");
  if (currentWebLanguage() == TG_LANG_AR) h += F("الرئيسية");
  else h += F("Home");
  h += F("</a></div>");
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

void loadMonthlySubscriptionState() {
  lastSubscriptionDisableYm = 0;
  monthlyUserAutoDisableEnabled = (ENABLE_MONTHLY_USER_SUBSCRIPTION_DISABLE != 0);
  Preferences p;
  if (p.begin("usr_sub", true)) {
    lastSubscriptionDisableYm = p.getUInt("lastYM", 0);
    monthlyUserAutoDisableEnabled = p.getBool("autoEn", monthlyUserAutoDisableEnabled);
    activationCodesExpireEndMonth = p.getBool("codeExp", activationCodesExpireEndMonth);
    telegramSmartPollingEnabled = p.getBool("tgSmart", telegramSmartPollingEnabled);
    p.end();
  }
}

void saveMonthlySubscriptionState(uint32_t ym) {
  lastSubscriptionDisableYm = ym;
  saveMonthlySubscriptionSettings();
}

void saveMonthlySubscriptionSettings() {
  Preferences p;
  if (p.begin("usr_sub", false)) {
    p.putUInt("lastYM", lastSubscriptionDisableYm);
    p.putBool("autoEn", monthlyUserAutoDisableEnabled);
    p.putBool("codeExp", activationCodesExpireEndMonth);
    p.putBool("tgSmart", telegramSmartPollingEnabled);
    p.end();
  }
}

void loadLoginIntroConfig() {
  loginIntroEnabled = false;
  loginIntroText = "";
  if (loginIntroPrefs.begin("loginIntro", true)) {
    loginIntroEnabled = loginIntroPrefs.getBool("en", false);
    loginIntroText = loginIntroPrefs.getString("txt", "");
    loginIntroPrefs.end();
  }
  if (loginIntroText.length() > LOGIN_INTRO_MAX_LEN) loginIntroText = loginIntroText.substring(0, LOGIN_INTRO_MAX_LEN);
}

void saveLoginIntroConfig() {
  if (loginIntroText.length() > LOGIN_INTRO_MAX_LEN) loginIntroText = loginIntroText.substring(0, LOGIN_INTRO_MAX_LEN);
  if (loginIntroPrefs.begin("loginIntro", false)) {
    loginIntroPrefs.putBool("en", loginIntroEnabled);
    loginIntroPrefs.putString("txt", loginIntroText);
    loginIntroPrefs.end();
  }
}


void setUserPageStoreError(const char* msg) {
  if (!msg || !msg[0]) msg = "none";
  strncpy(userPageStoreLastError, msg, sizeof(userPageStoreLastError) - 1);
  userPageStoreLastError[sizeof(userPageStoreLastError) - 1] = '\0';
}

uint32_t checksumUserPageBytes(const uint8_t* p, size_t len) {
  // Kept generic on purpose: Arduino auto-prototype generation can fail when a
  // function parameter uses a custom struct type declared later in the .ino file.
  uint32_t sum = 2166136261UL;
  for (size_t i = 0; i < len; i++) {
    sum ^= p[i];
    sum *= 16777619UL;
  }
  return sum;
}

void sanitizeLoadedUserPageData() {
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    WEB_USERS[i].user[WEB_USER_MAX_LEN] = 0;
    WEB_USERS[i].pass[WEB_PASS_MAX_LEN] = 0;
    WEB_USERS[i].role = sanitizeRole(WEB_USERS[i].role);
    WEB_USERS[i].enabled = WEB_USERS[i].enabled ? 1 : 0;
    if (!isSafeUsername(String(WEB_USERS[i].user)) || !isSafePassword(String(WEB_USERS[i].pass))) {
      if (i == 0) {
        strcpy(WEB_USERS[0].user, "admin");
        strcpy(WEB_USERS[0].pass, "admin");
        WEB_USERS[0].role = WEB_ROLE_ADMIN;
        WEB_USERS[0].enabled = 1;
      } else {
        WEB_USERS[i].user[0] = 0;
        WEB_USERS[i].pass[0] = 0;
        WEB_USERS[i].role = WEB_ROLE_VIEWER;
        WEB_USERS[i].enabled = 0;
      }
    }
    webUserTelegramChatId[i][USER_TELEGRAM_CHAT_ID_MAX_LEN] = 0;
    if (!isSafeTelegramChatIdText(String(webUserTelegramChatId[i]))) webUserTelegramChatId[i][0] = 0;
    webUserTelegramNotify[i] = webUserTelegramNotify[i] ? 1 : 0;
  }
  WEB_USERS[0].enabled = 1;
  WEB_USERS[0].role = WEB_ROLE_ADMIN;
  sanitizeAllUserFieldPermissions();
  if (loginIntroText.length() > LOGIN_INTRO_MAX_LEN) loginIntroText = loginIntroText.substring(0, LOGIN_INTRO_MAX_LEN);
}

bool saveUserPagePersistentConfig(const char* reason) {
  (void)reason;
  userPageStoreLastSaveOk = false;
  if (!storageFsBeginOk) beginStorageFs();
  if (!storageFsBeginOk) {
    setUserPageStoreError("FS not mounted");
    return false;
  }

  sanitizeLoadedUserPageData();
  UserPagePersistentRecord r;
  memset(&r, 0, sizeof(r));
  r.magic = USER_PAGE_STORE_MAGIC;
  r.version = USER_PAGE_STORE_VERSION;
  r.size = sizeof(UserPagePersistentRecord);
  memcpy(r.users, WEB_USERS, sizeof(WEB_USERS));
  memcpy(r.visibleMask, webUserVisibleMask, sizeof(webUserVisibleMask));
  memcpy(r.controlMask, webUserControlMask, sizeof(webUserControlMask));
  memcpy(r.telegramChatId, webUserTelegramChatId, sizeof(webUserTelegramChatId));
  memcpy(r.telegramNotify, webUserTelegramNotify, sizeof(webUserTelegramNotify));
  r.lastDisableYm = lastSubscriptionDisableYm;
  r.monthlyAuto = monthlyUserAutoDisableEnabled ? 1 : 0;
  r.codeExpiry = activationCodesExpireEndMonth ? 1 : 0;
  r.telegramSmart = telegramSmartPollingEnabled ? 1 : 0;
  r.introEnabled = loginIntroEnabled ? 1 : 0;
  String intro = loginIntroText;
  if (intro.length() > LOGIN_INTRO_MAX_LEN) intro = intro.substring(0, LOGIN_INTRO_MAX_LEN);
  r.introLen = (uint16_t)intro.length();
  intro.toCharArray(r.introText, sizeof(r.introText));
  r.checksum = 0;
  r.checksum = checksumUserPageBytes((const uint8_t*)&r, sizeof(r));

  File f = SPIFFS.open(USER_PAGE_STORE_PATH, FILE_WRITE);
  if (!f) {
    setUserPageStoreError("open write failed");
    return false;
  }
  size_t w = f.write((const uint8_t*)&r, sizeof(r));
  f.flush();
  f.close();
  if (w != sizeof(r)) {
    setUserPageStoreError("write short");
    return false;
  }
  userPageStoreLastSaveOk = true;
  userPageStoreSaveCount++;
  setUserPageStoreError("none");
  return true;
}

bool loadUserPagePersistentConfig() {
  userPageStoreLoadedAtBoot = false;
  if (!storageFsBeginOk) beginStorageFs();
  if (!storageFsBeginOk) {
    setUserPageStoreError("FS not mounted");
    return false;
  }
  if (!SPIFFS.exists(USER_PAGE_STORE_PATH)) {
    setUserPageStoreError("no user store");
    return false;
  }
  File f = SPIFFS.open(USER_PAGE_STORE_PATH, FILE_READ);
  if (!f) {
    setUserPageStoreError("open read failed");
    return false;
  }
  if (f.size() != sizeof(UserPagePersistentRecord)) {
    f.close();
    setUserPageStoreError("bad store size");
    return false;
  }
  UserPagePersistentRecord r;
  int rd = f.read((uint8_t*)&r, sizeof(r));
  f.close();
  if (rd != (int)sizeof(r)) {
    setUserPageStoreError("read short");
    return false;
  }
  uint32_t expected = r.checksum;
  r.checksum = 0;
  uint32_t actualChecksum = checksumUserPageBytes((const uint8_t*)&r, sizeof(r));
  r.checksum = expected;
  if (r.magic != USER_PAGE_STORE_MAGIC || r.version != USER_PAGE_STORE_VERSION || r.size != sizeof(UserPagePersistentRecord) || actualChecksum != expected) {
    setUserPageStoreError("bad checksum");
    return false;
  }

  memcpy(WEB_USERS, r.users, sizeof(WEB_USERS));
  memcpy(webUserVisibleMask, r.visibleMask, sizeof(webUserVisibleMask));
  memcpy(webUserControlMask, r.controlMask, sizeof(webUserControlMask));
  memcpy(webUserTelegramChatId, r.telegramChatId, sizeof(webUserTelegramChatId));
  memcpy(webUserTelegramNotify, r.telegramNotify, sizeof(webUserTelegramNotify));
  lastSubscriptionDisableYm = r.lastDisableYm;
  monthlyUserAutoDisableEnabled = r.monthlyAuto ? true : false;
  activationCodesExpireEndMonth = r.codeExpiry ? true : false;
  telegramSmartPollingEnabled = r.telegramSmart ? true : false;
  loginIntroEnabled = r.introEnabled ? true : false;
  r.introText[LOGIN_INTRO_MAX_LEN] = 0;
  loginIntroText = String(r.introText);
  if (loginIntroText.length() > LOGIN_INTRO_MAX_LEN) loginIntroText = loginIntroText.substring(0, LOGIN_INTRO_MAX_LEN);
  sanitizeLoadedUserPageData();
  userPageStoreLoadedAtBoot = true;
  userPageStoreLoadCount++;
  setUserPageStoreError("none");
  return true;
}

String userPageStorageStatusText() {
  String s;
  s.reserve(140);
  s += F("Load="); s += userPageStoreLoadedAtBoot ? F("YES") : F("NO");
  s += F(" / LastSave="); s += userPageStoreLastSaveOk ? F("OK") : F("FAILED/none");
  s += F(" / Saves="); s += String(userPageStoreSaveCount);
  s += F(" / Err="); s += userPageStoreLastError;
  return s;
}

bool getDeviceDateParts(int& year, byte& month, byte& day) {
  if (!isTimeValidNow()) return false;
  time_t now = (time_t)getDeviceEpoch();
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  year = tmNow.tm_year + 1900;
  month = (byte)(tmNow.tm_mon + 1);
  day = (byte)tmNow.tm_mday;
  if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31) return false;
  return true;
}

uint32_t currentYearMonthKey() {
  int y = 0; byte m = 0; byte d = 0;
  if (!getDeviceDateParts(y, m, d)) return 0;
  return (uint32_t)y * 100UL + (uint32_t)m;
}

byte subscriptionDaysLeftThisMonth() {
  int y = 0; byte m = 0; byte d = 0;
  if (!getDeviceDateParts(y, m, d)) return 0;
  byte dim = daysInMonth(y, m);
  if (d > dim) return 0;
  return (byte)(dim - d + 1);
}

bool subscriptionWarningActive(byte& daysLeft) {
  if (!monthlyUserAutoDisableEnabled) {
    daysLeft = 0;
    return false;
  }
  daysLeft = subscriptionDaysLeftThisMonth();
  return (daysLeft >= 1 && daysLeft <= SUBSCRIPTION_WARNING_DAYS);
}

String subscriptionDisabledTextWeb(const String& user) {
  String msg;
  msg.reserve(180);
  msg += "تم تعطيل اليوزر ";
  msg += user;
  msg += "<br>لو سمحت ادفع الاشتراك حتى تتمكن من استعادة تفعيل اليوزر ";
  msg += user;
  return msg;
}

String subscriptionWarningTextWeb(const String& user, byte daysLeft) {
  String msg;
  msg.reserve(220);
  msg += "تنبيه يا ";
  msg += user;
  msg += "<br>باقي ";
  msg += String(daysLeft);
  msg += (daysLeft == 1 ? " يوم" : " أيام");
  msg += " على تعطيل اليوزر ";
  msg += user;
  msg += "<br>برجاء دفع الاشتراك لتجنب تعطيل اليوزر ";
  msg += user;
  return msg;
}

String subscriptionDisabledTextTelegram(const String& chatId, const String& user) {
  bool ar = telegramChatIsArabic(chatId);
  String msg;
  msg.reserve(220);
  if (ar) {
    msg += "تم تعطيل اليوزر ";
    msg += user;
    msg += "\nلو سمحت ادفع الاشتراك حتى تتمكن من استعادة تفعيل اليوزر ";
    msg += user;
  } else {
    msg += "User disabled: ";
    msg += user;
    msg += "\nPlease pay the subscription to restore user access: ";
    msg += user;
  }
  return msg;
}

String subscriptionWarningTextTelegram(const String& chatId, const String& user, byte daysLeft) {
  bool ar = telegramChatIsArabic(chatId);
  String msg;
  msg.reserve(260);
  if (ar) {
    msg += "تنبيه يا ";
    msg += user;
    msg += "\nباقي ";
    msg += String(daysLeft);
    msg += (daysLeft == 1 ? " يوم" : " أيام");
    msg += " على تعطيل اليوزر ";
    msg += user;
    msg += "\nبرجاء دفع الاشتراك لتجنب تعطيل اليوزر ";
    msg += user;
  } else {
    msg += "Subscription warning for ";
    msg += user;
    msg += "\n";
    msg += String(daysLeft);
    msg += (daysLeft == 1 ? " day" : " days");
    msg += " left before disabling user ";
    msg += user;
    msg += "\nPlease pay the subscription to avoid disabling this user.";
  }
  return msg;
}

void notifyUserSubscriptionMessage(byte idx, const String& text) {
  if (idx >= WEB_USER_COUNT) return;
  if (!webUserTelegramNotify[idx]) return;
  if (!webUserTelegramChatId[idx][0]) return;
  telegramSendToChat(String(webUserTelegramChatId[idx]), text);
}

bool activationCodeTextValid(const String& codeIn) {
  String c = codeIn;
  c.trim();
  if (c.length() != ACTIVATION_CODE_LEN) return false;
  for (byte i = 0; i < c.length(); i++) {
    if (c[i] < '0' || c[i] > '9') return false;
  }
  return true;
}

String safeActivationDateTimeText() {
  String ts = currentDeviceDateTimeText();
  ts.trim();
  if (!ts.length() || ts == "--") return String("Time not synced");
  return ts;
}

void clearActivationSlot(byte i) {
  if (i >= ACTIVATION_CODE_COUNT) return;
  activationCodes[i][0] = '\0';
  activationCodeUsed[i] = 0;
  activationCodeUsedBy[i][0] = '\0';
  activationCodeUsedAt[i][0] = '\0';
  activationCodeMethod[i][0] = '\0';
  activationCodeCreatedAt[i][0] = '\0';
  activationCodeCreatedYm[i] = 0;
}


void setActivationCodeStoreError(const char* msg) {
  if (!msg || !msg[0]) msg = "none";
  strncpy(activationCodeStoreLastError, msg, sizeof(activationCodeStoreLastError) - 1);
  activationCodeStoreLastError[sizeof(activationCodeStoreLastError) - 1] = '\0';
}

uint32_t checksumActivationCodeBytes(const uint8_t* p, size_t len) {
  uint32_t sum = 2166136261UL;
  for (size_t i = 0; i < len; i++) {
    sum ^= p[i];
    sum *= 16777619UL;
  }
  return sum;
}

byte activationCodeCountInRam() {
  byte n = 0;
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) if (activationCodes[i][0]) n++;
  return n;
}

bool saveActivationCodesToFs() {
  activationCodeStoreLastSaveOk = false;
  if (!storageFsBeginOk && !beginStorageFs()) {
    setActivationCodeStoreError("FS mount failed");
    return false;
  }
  ActivationCodeStoreRecord r;
  memset(&r, 0, sizeof(r));
  r.magic = ACTIVATION_CODE_STORE_MAGIC;
  r.version = ACTIVATION_CODE_STORE_VERSION;
  r.size = sizeof(ActivationCodeStoreRecord);
  memcpy(r.codes, activationCodes, sizeof(activationCodes));
  memcpy(r.used, activationCodeUsed, sizeof(activationCodeUsed));
  memcpy(r.usedBy, activationCodeUsedBy, sizeof(activationCodeUsedBy));
  memcpy(r.usedAt, activationCodeUsedAt, sizeof(activationCodeUsedAt));
  memcpy(r.method, activationCodeMethod, sizeof(activationCodeMethod));
  memcpy(r.createdAt, activationCodeCreatedAt, sizeof(activationCodeCreatedAt));
  memcpy(r.createdYm, activationCodeCreatedYm, sizeof(activationCodeCreatedYm));
  r.expireEndMonth = activationCodesExpireEndMonth ? 1 : 0;
  r.checksum = 0;
  r.checksum = checksumActivationCodeBytes((const uint8_t*)&r, sizeof(r));

  File f = SPIFFS.open(ACTIVATION_CODE_STORE_PATH, FILE_WRITE);
  if (!f) {
    setActivationCodeStoreError("open write failed");
    return false;
  }
  size_t w = f.write((const uint8_t*)&r, sizeof(r));
  f.flush();
  f.close();
  if (w != sizeof(r)) {
    setActivationCodeStoreError("write short");
    return false;
  }
  activationCodeStoreLastSaveOk = true;
  activationCodeStoreSaveCount++;
  setActivationCodeStoreError("none");
  return true;
}

bool loadActivationCodesFromFs() {
  activationCodeStoreLoadedAtBoot = false;
  if (!storageFsBeginOk && !beginStorageFs()) {
    setActivationCodeStoreError("FS mount failed");
    return false;
  }
  if (!SPIFFS.exists(ACTIVATION_CODE_STORE_PATH)) {
    setActivationCodeStoreError("no act store");
    return false;
  }
  File f = SPIFFS.open(ACTIVATION_CODE_STORE_PATH, FILE_READ);
  if (!f) {
    setActivationCodeStoreError("open read failed");
    return false;
  }
  if (f.size() != sizeof(ActivationCodeStoreRecord)) {
    f.close();
    setActivationCodeStoreError("bad size");
    return false;
  }
  ActivationCodeStoreRecord r;
  int rd = f.read((uint8_t*)&r, sizeof(r));
  f.close();
  if (rd != (int)sizeof(r)) {
    setActivationCodeStoreError("read short");
    return false;
  }
  uint32_t expected = r.checksum;
  r.checksum = 0;
  uint32_t actual = checksumActivationCodeBytes((const uint8_t*)&r, sizeof(r));
  r.checksum = expected;
  if (r.magic != ACTIVATION_CODE_STORE_MAGIC || r.version != ACTIVATION_CODE_STORE_VERSION || r.size != sizeof(ActivationCodeStoreRecord) || actual != expected) {
    setActivationCodeStoreError("bad checksum");
    return false;
  }

  memcpy(activationCodes, r.codes, sizeof(activationCodes));
  memcpy(activationCodeUsed, r.used, sizeof(activationCodeUsed));
  memcpy(activationCodeUsedBy, r.usedBy, sizeof(activationCodeUsedBy));
  memcpy(activationCodeUsedAt, r.usedAt, sizeof(activationCodeUsedAt));
  memcpy(activationCodeMethod, r.method, sizeof(activationCodeMethod));
  memcpy(activationCodeCreatedAt, r.createdAt, sizeof(activationCodeCreatedAt));
  memcpy(activationCodeCreatedYm, r.createdYm, sizeof(activationCodeCreatedYm));
  activationCodesExpireEndMonth = r.expireEndMonth ? true : false;

  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    activationCodes[i][ACTIVATION_CODE_LEN] = '\0';
    if (!activationCodeTextValid(String(activationCodes[i]))) clearActivationSlot(i);
    activationCodeUsedBy[i][WEB_USER_MAX_LEN] = '\0';
    activationCodeUsedAt[i][ACTIVATION_USED_AT_MAX_LEN] = '\0';
    activationCodeMethod[i][ACTIVATION_METHOD_MAX_LEN] = '\0';
    activationCodeCreatedAt[i][ACTIVATION_USED_AT_MAX_LEN] = '\0';
    activationCodeUsed[i] = activationCodeUsed[i] ? 1 : 0;
  }
  activationCodeStoreLoadedAtBoot = true;
  setActivationCodeStoreError("none");
  return true;
}

void loadActivationCodes() {
  memset(activationCodes, 0, sizeof(activationCodes));
  memset(activationCodeUsed, 0, sizeof(activationCodeUsed));
  memset(activationCodeUsedBy, 0, sizeof(activationCodeUsedBy));
  memset(activationCodeUsedAt, 0, sizeof(activationCodeUsedAt));
  memset(activationCodeMethod, 0, sizeof(activationCodeMethod));
  memset(activationCodeCreatedAt, 0, sizeof(activationCodeCreatedAt));
  memset(activationCodeCreatedYm, 0, sizeof(activationCodeCreatedYm));

  if (activationCodePrefs.begin("actcodes", true)) {
    for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    String idx = String(i);
    String c = activationCodePrefs.getString((String("c") + idx).c_str(), "");
    c.trim();
    if (!activationCodeTextValid(c)) continue;
    c.toCharArray(activationCodes[i], sizeof(activationCodes[i]));
    activationCodeUsed[i] = activationCodePrefs.getUChar((String("u") + idx).c_str(), 0) ? 1 : 0;
    String by = activationCodePrefs.getString((String("by") + idx).c_str(), "");
    String at = activationCodePrefs.getString((String("at") + idx).c_str(), "");
    String mt = activationCodePrefs.getString((String("m") + idx).c_str(), "");
    String cr = activationCodePrefs.getString((String("cr") + idx).c_str(), "");
    activationCodeCreatedYm[i] = activationCodePrefs.getUInt((String("ym") + idx).c_str(), 0);
    by.substring(0, WEB_USER_MAX_LEN).toCharArray(activationCodeUsedBy[i], sizeof(activationCodeUsedBy[i]));
    at.substring(0, ACTIVATION_USED_AT_MAX_LEN).toCharArray(activationCodeUsedAt[i], sizeof(activationCodeUsedAt[i]));
    mt.substring(0, ACTIVATION_METHOD_MAX_LEN).toCharArray(activationCodeMethod[i], sizeof(activationCodeMethod[i]));
    if (!cr.length()) cr = "Old code";
    cr.substring(0, ACTIVATION_USED_AT_MAX_LEN).toCharArray(activationCodeCreatedAt[i], sizeof(activationCodeCreatedAt[i]));
    }
    activationCodePrefs.end();
  }

  byte nvsCount = activationCodeCountInRam();
  bool fsLoaded = loadActivationCodesFromFs();
  if (!fsLoaded && nvsCount > 0) saveActivationCodesToFs();
}

void saveActivationCodes() {
  bool nvsOk = false;
  if (activationCodePrefs.begin("actcodes", false)) {
    nvsOk = true;
    for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    String idx = String(i);
    activationCodePrefs.putString((String("c") + idx).c_str(), String(activationCodes[i]));
    activationCodePrefs.putUChar((String("u") + idx).c_str(), activationCodeUsed[i] ? 1 : 0);
    activationCodePrefs.putString((String("by") + idx).c_str(), String(activationCodeUsedBy[i]));
    activationCodePrefs.putString((String("at") + idx).c_str(), String(activationCodeUsedAt[i]));
    activationCodePrefs.putString((String("m") + idx).c_str(), String(activationCodeMethod[i]));
    activationCodePrefs.putString((String("cr") + idx).c_str(), String(activationCodeCreatedAt[i]));
    activationCodePrefs.putUInt((String("ym") + idx).c_str(), activationCodeCreatedYm[i]);
    }
    activationCodePrefs.end();
  }
  bool fsOk = saveActivationCodesToFs();
  if (!nvsOk && !fsOk) addEsp32Log("Activation codes save failed");
}

bool activationCodeExists(const String& codeIn) {
  String c = codeIn;
  c.trim();
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    if (activationCodes[i][0] && c.equals(String(activationCodes[i]))) return true;
  }
  return false;
}

String generateOneTimeActivationCode() {
  byte slot = ACTIVATION_CODE_COUNT;
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    if (!activationCodes[i][0]) { slot = i; break; }
  }
  if (slot >= ACTIVATION_CODE_COUNT) return String("");

  String code;
  for (byte tries = 0; tries < 40; tries++) {
    uint32_t n = 10000000UL + (esp_random() % 90000000UL);
    code = String(n);
    if (!activationCodeExists(code)) break;
  }
  clearActivationSlot(slot);
  code.toCharArray(activationCodes[slot], sizeof(activationCodes[slot]));
  safeActivationDateTimeText().toCharArray(activationCodeCreatedAt[slot], sizeof(activationCodeCreatedAt[slot]));
  activationCodeCreatedYm[slot] = currentYearMonthKey();
  saveActivationCodes();
  autoBackupAfterSave("Activation code generated");
  addEsp32Log(String("Activation code generated: ") + code);
  return code;
}

bool activationCodeIsExpired(byte i) {
  if (i >= ACTIVATION_CODE_COUNT) return true;
  if (!activationCodesExpireEndMonth) return false;
  if (!activationCodes[i][0] || activationCodeUsed[i]) return false;
  uint32_t nowYm = currentYearMonthKey();
  if (nowYm == 0) return false; // If time is not synced, do not wrongly expire valid codes.
  if (activationCodeCreatedYm[i] == 0) return false; // Old codes remain valid until used/cleared.
  return activationCodeCreatedYm[i] != nowYm;
}

void loadActivationLog() {
  memset(activationLogCode, 0, sizeof(activationLogCode));
  memset(activationLogUser, 0, sizeof(activationLogUser));
  memset(activationLogAt, 0, sizeof(activationLogAt));
  memset(activationLogMethod, 0, sizeof(activationLogMethod));
  if (!activationLogPrefs.begin("actlog", true)) return;
  for (byte i = 0; i < ACTIVATION_LOG_COUNT; i++) {
    String idx = String(i);
    String c = activationLogPrefs.getString((String("c") + idx).c_str(), "");
    String u = activationLogPrefs.getString((String("u") + idx).c_str(), "");
    String at = activationLogPrefs.getString((String("at") + idx).c_str(), "");
    String m = activationLogPrefs.getString((String("m") + idx).c_str(), "");
    c.substring(0, ACTIVATION_CODE_LEN).toCharArray(activationLogCode[i], sizeof(activationLogCode[i]));
    u.substring(0, WEB_USER_MAX_LEN).toCharArray(activationLogUser[i], sizeof(activationLogUser[i]));
    at.substring(0, ACTIVATION_USED_AT_MAX_LEN).toCharArray(activationLogAt[i], sizeof(activationLogAt[i]));
    m.substring(0, ACTIVATION_METHOD_MAX_LEN).toCharArray(activationLogMethod[i], sizeof(activationLogMethod[i]));
  }
  activationLogPrefs.end();
}

void saveActivationLog() {
  if (!activationLogPrefs.begin("actlog", false)) return;
  for (byte i = 0; i < ACTIVATION_LOG_COUNT; i++) {
    String idx = String(i);
    activationLogPrefs.putString((String("c") + idx).c_str(), String(activationLogCode[i]));
    activationLogPrefs.putString((String("u") + idx).c_str(), String(activationLogUser[i]));
    activationLogPrefs.putString((String("at") + idx).c_str(), String(activationLogAt[i]));
    activationLogPrefs.putString((String("m") + idx).c_str(), String(activationLogMethod[i]));
  }
  activationLogPrefs.end();
}

void addActivationLog(const String& code, const String& user, const String& at, const String& method) {
  for (int i = ACTIVATION_LOG_COUNT - 1; i > 0; i--) {
    strncpy(activationLogCode[i], activationLogCode[i - 1], sizeof(activationLogCode[i]) - 1); activationLogCode[i][sizeof(activationLogCode[i]) - 1] = '\0';
    strncpy(activationLogUser[i], activationLogUser[i - 1], sizeof(activationLogUser[i]) - 1); activationLogUser[i][sizeof(activationLogUser[i]) - 1] = '\0';
    strncpy(activationLogAt[i], activationLogAt[i - 1], sizeof(activationLogAt[i]) - 1); activationLogAt[i][sizeof(activationLogAt[i]) - 1] = '\0';
    strncpy(activationLogMethod[i], activationLogMethod[i - 1], sizeof(activationLogMethod[i]) - 1); activationLogMethod[i][sizeof(activationLogMethod[i]) - 1] = '\0';
  }
  code.substring(0, ACTIVATION_CODE_LEN).toCharArray(activationLogCode[0], sizeof(activationLogCode[0]));
  user.substring(0, WEB_USER_MAX_LEN).toCharArray(activationLogUser[0], sizeof(activationLogUser[0]));
  at.substring(0, ACTIVATION_USED_AT_MAX_LEN).toCharArray(activationLogAt[0], sizeof(activationLogAt[0]));
  method.substring(0, ACTIVATION_METHOD_MAX_LEN).toCharArray(activationLogMethod[0], sizeof(activationLogMethod[0]));
  saveActivationLog();
}

bool consumeActivationCode(const String& codeIn, const String& usedBy, const String& method) {
  String c = codeIn;
  c.trim();
  if (!activationCodeTextValid(c)) return false;
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    if (activationCodes[i][0] && !activationCodeUsed[i] && !activationCodeIsExpired(i) && c.equals(String(activationCodes[i]))) {
      activationCodeUsed[i] = 1;
      String by = usedBy; by.trim();
      String mt = method; mt.trim();
      String at = safeActivationDateTimeText();
      by.substring(0, WEB_USER_MAX_LEN).toCharArray(activationCodeUsedBy[i], sizeof(activationCodeUsedBy[i]));
      at.substring(0, ACTIVATION_USED_AT_MAX_LEN).toCharArray(activationCodeUsedAt[i], sizeof(activationCodeUsedAt[i]));
      mt.substring(0, ACTIVATION_METHOD_MAX_LEN).toCharArray(activationCodeMethod[i], sizeof(activationCodeMethod[i]));
      saveActivationCodes();
      autoBackupAfterSave("Activation code used");
      addActivationLog(c, by, at, mt);
      addEsp32Log(String("Activation code used: ") + c + String(" by ") + by + String(" via ") + mt);
      return true;
    }
  }
  return false;
}

String activationAttemptKey(const String& prefix, const String& id) {
  String k = prefix;
  k += ":";
  k += id;
  k.trim();
  if (k.length() > ACTIVATION_ATTEMPT_KEY_MAX_LEN) k = k.substring(0, ACTIVATION_ATTEMPT_KEY_MAX_LEN);
  return k;
}

int activationAttemptSlotForKey(const String& keyIn, bool createIfMissing) {
  String key = keyIn;
  key.trim();
  if (!key.length()) return -1;
  unsigned long now = millis();
  int freeSlot = -1;
  int oldestSlot = 0;
  unsigned long oldestSeen = 0xFFFFFFFFUL;

  for (byte i = 0; i < ACTIVATION_ATTEMPT_SLOT_COUNT; i++) {
    if (activationAttempts[i].key[0]) {
      if (String(activationAttempts[i].key) == key) {
        activationAttempts[i].lastSeenMs = now;
        return i;
      }
      if (activationAttempts[i].lastSeenMs < oldestSeen) {
        oldestSeen = activationAttempts[i].lastSeenMs;
        oldestSlot = i;
      }
    } else if (freeSlot < 0) {
      freeSlot = i;
    }
  }

  if (!createIfMissing) return -1;
  int slot = (freeSlot >= 0) ? freeSlot : oldestSlot;
  memset(&activationAttempts[slot], 0, sizeof(activationAttempts[slot]));
  key.toCharArray(activationAttempts[slot].key, sizeof(activationAttempts[slot].key));
  activationAttempts[slot].lastSeenMs = now;
  return slot;
}

bool activationAttemptIsLocked(const String& key, String& msg, bool ar) {
  int slot = activationAttemptSlotForKey(key, false);
  if (slot < 0) return false;
  unsigned long now = millis();
  if (activationAttempts[slot].lockUntilMs == 0) return false;
  if ((long)(activationAttempts[slot].lockUntilMs - now) <= 0) {
    activationAttempts[slot].fails = 0;
    activationAttempts[slot].lockUntilMs = 0;
    return false;
  }
  unsigned long leftSec = (activationAttempts[slot].lockUntilMs - now + 999UL) / 1000UL;
  unsigned long leftMin = (leftSec + 59UL) / 60UL;
  if (ar) {
    msg = "تم إيقاف محاولات التفعيل مؤقتًا. حاول مرة أخرى بعد ";
    msg += String(leftMin);
    msg += " دقيقة.";
  } else {
    msg = "Activation attempts are locked. Try again after ";
    msg += String(leftMin);
    msg += " minute(s).";
  }
  return true;
}

String activationAttemptFailureMessage(const String& key, bool ar) {
  int slot = activationAttemptSlotForKey(key, true);
  if (slot < 0) return ar ? String("كود التفعيل غير صحيح.") : String("Invalid activation code.");
  unsigned long now = millis();
  if (activationAttempts[slot].lockUntilMs != 0 && (long)(activationAttempts[slot].lockUntilMs - now) > 0) {
    String m;
    activationAttemptIsLocked(key, m, ar);
    return m;
  }
  activationAttempts[slot].fails++;
  activationAttempts[slot].lastSeenMs = now;
  if (activationAttempts[slot].fails >= ACTIVATION_MAX_WRONG_ATTEMPTS) {
    activationAttempts[slot].lockUntilMs = now + ACTIVATION_LOCK_MS;
    activationAttempts[slot].fails = ACTIVATION_MAX_WRONG_ATTEMPTS;
    return ar ? String("كود التفعيل غير صحيح. تم إيقاف المحاولات لمدة 10 دقائق.") : String("Invalid activation code. Attempts locked for 10 minutes.");
  }
  byte left = ACTIVATION_MAX_WRONG_ATTEMPTS - activationAttempts[slot].fails;
  String m = ar ? String("كود التفعيل غير صحيح أو مستخدم من قبل. المحاولات المتبقية: ") : String("Invalid or already used activation code. Remaining attempts: ");
  m += String(left);
  return m;
}

void activationAttemptClear(const String& key) {
  int slot = activationAttemptSlotForKey(key, false);
  if (slot < 0) return;
  activationAttempts[slot].fails = 0;
  activationAttempts[slot].lockUntilMs = 0;
  activationAttempts[slot].lastSeenMs = millis();
}

String activationLogAdminHtml() {
  String html;
  html.reserve(1600);
  html += F("<h1>Last 10 Activations</h1>");
  bool any = false;
  for (byte i = 0; i < ACTIVATION_LOG_COUNT; i++) {
    if (!activationLogCode[i][0]) continue;
    any = true;
    html += F("<div class='smsec'><b>"); html += htmlEscapeText(String(activationLogUser[i])); html += F("</b>");
    html += F("<div class='row'><span class='k'>Code</span><span class='v'>"); html += htmlEscapeText(String(activationLogCode[i])); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>Used at</span><span class='v'>"); html += htmlEscapeText(String(activationLogAt[i])); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>Method</span><span class='v'>"); html += htmlEscapeText(String(activationLogMethod[i])); html += F("</span></div></div>");
  }
  if (!any) html += F("<div class='sub'>No activation log yet.</div>");
  return html;
}

String activationCodesAdminCardHtml() {
  String html;
  html.reserve(7200);
  byte avail = 0, used = 0, expired = 0;
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    if (!activationCodes[i][0]) continue;
    if (activationCodeUsed[i]) used++;
    else if (activationCodeIsExpired(i)) expired++;
    else avail++;
  }

  html += F("<div class='card'><h1>Subscription & Activation</h1><div class='sub'>Generate one-time codes and control monthly subscription auto-disable.</div>");
  html += F("<form method='POST' action='/savesubscription'>");
  html += sessionHiddenInput();
  html += F("<label class='small'><input type='checkbox' name='autoMonthly' value='1' "); if (monthlyUserAutoDisableEnabled) html += F("checked"); html += F("> Enable monthly user auto-disable on the first day of each month</label>");
  html += F("<div class='sub'>If OFF, last 5-day warnings and automatic monthly disabling stop. Existing disabled users stay disabled until admin or activation code enables them.</div>");
  html += F("<h1>Activation Code Expiry</h1><div class='modepick'>");
  html += F("<label><input type='radio' name='codeExpiry' value='0' "); if (!activationCodesExpireEndMonth) html += F("checked"); html += F("> No expiry</label>");
  html += F("<label><input type='radio' name='codeExpiry' value='1' "); if (activationCodesExpireEndMonth) html += F("checked"); html += F("> End of current month</label></div>");
  html += F("<h1>Telegram Polling</h1><div class='modepick'>");
  html += F("<label><input type='radio' name='tgPoll' value='fast' "); if (!telegramSmartPollingEnabled) html += F("checked"); html += F("> Fast - every 10 seconds</label>");
  html += F("<label><input type='radio' name='tgPoll' value='smart' "); if (telegramSmartPollingEnabled) html += F("checked"); html += F("> Smart - 60 seconds idle, 10 seconds for 2 minutes after activity</label></div>");
  html += F("<h1>Login Page Intro</h1>");
  html += F("<label class='small'><input type='checkbox' name='loginIntroEnabled' value='1' "); if (loginIntroEnabled) html += F("checked"); html += F("> Show device intro on login page</label>");
  html += F("<div class='sub'>Maximum 1500 characters. Text is escaped for safety.</div>");
  html += F("<textarea name='loginIntroText' maxlength='"); html += String(LOGIN_INTRO_MAX_LEN); html += F("' placeholder='Write a short description of the device and features...'>");
  html += htmlEscapeText(loginIntroText);
  html += F("</textarea>");
  html += F("<button class='btn btn2' type='submit'>Save Subscription Settings</button></form>");
  html += F("<div class='row'><span class='k'>Monthly Auto-disable</span><span class='v'>"); html += (monthlyUserAutoDisableEnabled ? "ON" : "OFF"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Code Expiry</span><span class='v'>"); html += (activationCodesExpireEndMonth ? "End of month" : "No expiry"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Telegram Polling</span><span class='v'>"); html += (telegramSmartPollingEnabled ? "Smart" : "Fast 10s"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>User Page Storage</span><span class='v'>"); html += htmlEscapeText(userPageStorageStatusText()); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Activation Codes Storage</span><span class='v'>Load="); html += activationCodeStoreLoadedAtBoot ? F("YES") : F("NO"); html += F(" / LastSave="); html += activationCodeStoreLastSaveOk ? F("OK") : F("FAILED/none"); html += F(" / Saves="); html += String(activationCodeStoreSaveCount); html += F(" / Err="); html += htmlEscapeText(String(activationCodeStoreLastError)); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Intro Length</span><span class='v'>"); html += String(loginIntroText.length()); html += F(" / "); html += String(LOGIN_INTRO_MAX_LEN); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Available</span><span class='v'>"); html += String(avail); html += F(" / "); html += String(ACTIVATION_CODE_COUNT); html += F("</span></div>");
  if (expired) { html += F("<div class='row'><span class='k'>Expired</span><span class='v warn'>"); html += String(expired); html += F("</span></div>"); }
  html += F("<form method='POST' action='/gencodes'>");
  html += sessionHiddenInput();
  html += F("<label class='small'>How many codes?</label><input type='number' name='count' min='1' max='10' value='5'>");
  html += F("<button class='btn' type='submit'>Generate Codes</button></form>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/codes.txt"); html += F("'>Download Codes TXT</a>");

  html += F("<h1>Available Codes</h1>");
  if (avail == 0) {
    html += F("<div class='sub'>No available codes.</div>");
  } else {
    for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
      if (activationCodes[i][0] && !activationCodeUsed[i] && !activationCodeIsExpired(i)) {
        html += F("<div class='smsec'><b style='font-size:22px;letter-spacing:2px'>");
        html += activationCodes[i];
        html += F("</b><div class='row'><span class='k'>Created at</span><span class='v'>");
        html += htmlEscapeText(String(activationCodeCreatedAt[i][0] ? activationCodeCreatedAt[i] : "Old code"));
        html += F("</span></div></div>");
      }
    }
  }

  if (expired) {
    html += F("<h1>Expired Codes</h1>");
    for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
      if (activationCodes[i][0] && !activationCodeUsed[i] && activationCodeIsExpired(i)) {
        html += F("<div class='smsec'><b>"); html += activationCodes[i]; html += F("</b><div class='row'><span class='k'>Created at</span><span class='v'>");
        html += htmlEscapeText(String(activationCodeCreatedAt[i][0] ? activationCodeCreatedAt[i] : "Old code"));
        html += F("</span></div><div class='msg warn'>Expired by end-of-month setting.</div></div>");
      }
    }
  }

  html += F("<h1>Used Codes</h1>");
  if (used == 0) {
    html += F("<div class='sub'>No used codes yet.</div>");
  } else {
    for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
      if (activationCodes[i][0] && activationCodeUsed[i]) {
        html += F("<div class='smsec'><b>"); html += activationCodes[i]; html += F("</b>");
        html += F("<div class='row'><span class='k'>Created at</span><span class='v'>"); html += htmlEscapeText(String(activationCodeCreatedAt[i][0] ? activationCodeCreatedAt[i] : "Old code")); html += F("</span></div>");
        html += F("<div class='row'><span class='k'>Used by</span><span class='v'>"); html += htmlEscapeText(String(activationCodeUsedBy[i])); html += F("</span></div>");
        html += F("<div class='row'><span class='k'>Used at</span><span class='v'>"); html += htmlEscapeText(String(activationCodeUsedAt[i])); html += F("</span></div>");
        html += F("<div class='row'><span class='k'>Method</span><span class='v'>"); html += htmlEscapeText(String(activationCodeMethod[i])); html += F("</span></div></div>");
      }
    }
    html += F("<form method='POST' action='/clearusedcodes' onsubmit=\"return confirm('Clear used activation code history? Activation Log will remain.');\">");
    html += sessionHiddenInput();
    html += F("<button class='btn btn2' type='submit'>Clear Used Codes</button></form>");
  }
  html += activationLogAdminHtml();
  html += F("</div>");
  return html;
}

String activationCodesTextReport() {
  String txt;
  txt.reserve(6000);
  txt += FW_FULL_NAME; txt += "\n";
  txt += "Generated at: "; txt += safeActivationDateTimeText(); txt += "\n\n";
  txt += "Available Codes\n";
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    if (activationCodes[i][0] && !activationCodeUsed[i] && !activationCodeIsExpired(i)) {
      txt += activationCodes[i]; txt += "  Created: "; txt += activationCodeCreatedAt[i][0] ? activationCodeCreatedAt[i] : "Old code"; txt += "\n";
    }
  }
  txt += "\nExpired Codes\n";
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    if (activationCodes[i][0] && !activationCodeUsed[i] && activationCodeIsExpired(i)) {
      txt += activationCodes[i]; txt += "  Created: "; txt += activationCodeCreatedAt[i][0] ? activationCodeCreatedAt[i] : "Old code"; txt += "\n";
    }
  }
  txt += "\nUsed Codes\n";
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    if (activationCodes[i][0] && activationCodeUsed[i]) {
      txt += activationCodes[i]; txt += "  Created: "; txt += activationCodeCreatedAt[i][0] ? activationCodeCreatedAt[i] : "Old code";
      txt += "  Used by: "; txt += activationCodeUsedBy[i]; txt += "  Used at: "; txt += activationCodeUsedAt[i]; txt += "  Method: "; txt += activationCodeMethod[i]; txt += "\n";
    }
  }
  txt += "\nLast 10 Activations\n";
  for (byte i = 0; i < ACTIVATION_LOG_COUNT; i++) {
    if (!activationLogCode[i][0]) continue;
    txt += activationLogCode[i]; txt += "  User: "; txt += activationLogUser[i]; txt += "  At: "; txt += activationLogAt[i]; txt += "  Method: "; txt += activationLogMethod[i]; txt += "\n";
  }
  return txt;
}

void handleGenerateActivationCodes() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  int requested = webServer.hasArg("count") ? webServer.arg("count").toInt() : 1;
  if (requested < 1) requested = 1;
  if (requested > 10) requested = 10;
  String codes;
  codes.reserve(120);
  byte made = 0;
  for (byte i = 0; i < requested; i++) {
    String c = generateOneTimeActivationCode();
    if (!c.length()) break;
    if (codes.length()) codes += "\n";
    codes += c;
    made++;
  }
  String html = htmlHeader("Activation Codes");
  html += F("<div class='card'><h1>Activation Codes Generated</h1>");
  if (made == 0) {
    html += F("<div class='msg bad'>No free activation code slots. Clear used codes or use existing available codes.</div>");
  } else {
    html += F("<div class='msg ok'><pre style='font-size:24px;letter-spacing:2px;white-space:pre-wrap'>");
    html += htmlEscapeText(codes);
    html += F("</pre></div>");
  }
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/users"); html += F("'>Back to Users</a></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void handleClearUsedActivationCodes() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    if (activationCodes[i][0] && activationCodeUsed[i]) clearActivationSlot(i);
  }
  saveActivationCodes();
  autoBackupAfterSave("Used activation codes cleared");
  webServer.sendHeader("Location", sessionUrl("/users"), true);
  webServer.send(302, "text/plain", "");
}

void handleDownloadActivationCodesTxt() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  String txt = activationCodesTextReport();
  webServer.sendHeader("Content-Disposition", "attachment; filename=activation_codes.txt");
  webServer.send(200, "text/plain; charset=utf-8", txt);
}

void handleSaveSubscriptionSettings() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  bool newEnabled = webServer.hasArg("autoMonthly");
  bool wasEnabled = monthlyUserAutoDisableEnabled;
  monthlyUserAutoDisableEnabled = newEnabled;
  activationCodesExpireEndMonth = (webServer.arg("codeExpiry") == "1");
  telegramSmartPollingEnabled = (webServer.arg("tgPoll") == "smart");
  loginIntroEnabled = webServer.hasArg("loginIntroEnabled");
  if (webServer.hasArg("loginIntroText")) {
    loginIntroText = webServer.arg("loginIntroText");
    if (loginIntroText.length() > LOGIN_INTRO_MAX_LEN) loginIntroText = loginIntroText.substring(0, LOGIN_INTRO_MAX_LEN);
  }

  // When turning the feature ON, set the current month as baseline so users are not disabled immediately after saving.
  if (newEnabled && !wasEnabled) {
    uint32_t ym = currentYearMonthKey();
    if (ym != 0) lastSubscriptionDisableYm = ym;
  }
  saveMonthlySubscriptionSettings();
  saveLoginIntroConfig();
  saveActivationCodes();
  saveUserPagePersistentConfig("save-subscription-intro");
  autoBackupAfterSave("Subscription and login intro saved");
  addEsp32Log(String("Monthly auto-disable ") + (monthlyUserAutoDisableEnabled ? "enabled" : "disabled"));
  webServer.sendHeader("Location", sessionUrl("/users"), true);
  webServer.send(302, "text/plain", "");
}

byte findWebUserByTelegramChatId(const String& chatIdIn) {
  String c = chatIdIn;
  c.trim();
  if (!c.length()) return 255;
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    if (!WEB_USERS[i].user[0]) continue;
    if (String(webUserTelegramChatId[i]).equals(c)) return i;
  }
  return 255;
}

void handleWebActivationCode() {
  maintainMonthlyUserSubscriptionDisable(true);
  String u = webServer.arg("u"); u.trim();
  String p = webServer.arg("p"); p.trim();
  String c = webServer.arg("code"); c.trim();

  byte idx = 255;
  if (!findWebUserAnyState(u, p, idx) || idx == 0 || idx >= WEB_USER_COUNT || WEB_USERS[idx].enabled) {
    String html = htmlHeader("Activation Failed");
    html += F("<div class='card'><h1>فشل التفعيل</h1><div class='msg bad'>اسم المستخدم أو كلمة المرور غير صحيحة، أو اليوزر مفعل بالفعل.</div><a class='btn btn2' href='/login'>رجوع</a></div>");
    html += htmlFooter();
    webServer.send(403, "text/html", html);
    return;
  }

  String attemptKey = activationAttemptKey("W", String(WEB_USERS[idx].user));
  String lockMsg;
  if (activationAttemptIsLocked(attemptKey, lockMsg, true)) {
    String html = htmlHeader("Activation Locked");
    html += F("<div class='card'><h1>محاولات التفعيل متوقفة</h1><div class='msg bad'>");
    html += htmlEscapeText(lockMsg);
    html += F("</div><a class='btn btn2' href='/login'>رجوع</a></div>");
    html += htmlFooter();
    webServer.send(429, "text/html", html);
    return;
  }

  if (!consumeActivationCode(c, String(WEB_USERS[idx].user), String("Web"))) {
    String msg = activationAttemptFailureMessage(attemptKey, true);
    String html = htmlHeader("Activation Failed");
    html += F("<div class='card'><h1>فشل التفعيل</h1><div class='msg bad'>");
    html += htmlEscapeText(msg);
    html += F("</div><a class='btn btn2' href='/login'>رجوع</a></div>");
    html += htmlFooter();
    webServer.send(403, "text/html", html);
    return;
  }

  activationAttemptClear(attemptKey);
  setWebUserEnabledByIndex(idx, true);
  setLoggedInUser(idx);
  String cookie = "ESPSESS=" + webSessionToken + "; Path=/";
  webServer.sendHeader("Set-Cookie", cookie);
  String html = htmlHeader("Activated");
  html += F("<div class='card'><h1>تم التفعيل</h1><div class='msg ok'>تم تفعيل اليوزر ");
  html += htmlEscapeText(String(WEB_USERS[idx].user));
  html += F(" بنجاح.</div><a class='btn' href='");
  html += sessionUrl("/");
  html += F("'>دخول</a></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void sendSubscriptionDisabledPage(const String& user) {
  String html = htmlHeader("User Disabled");
  html += F("<div class='card'><h1>تم تعطيل اليوزر</h1><div class='msg bad'>");
  html += subscriptionDisabledTextWeb(user);
  html += F("</div><a class='btn btn2' href='/login'>رجوع لتسجيل الدخول</a></div>");
  html += F("<div class='card'><h1>تفعيل الاشتراك بالكود</h1><div class='sub'>بعد الدفع، أدخل كود التفعيل الذي أرسله الأدمن. الكود صالح لمستخدم واحد فقط ويستخدم مرة واحدة.</div>");
  html += F("<form method='POST' action='/activate'>");
  html += F("<label class='small'>Username</label><input name='u' maxlength='"); html += String(WEB_USER_MAX_LEN); html += F("' value='"); html += htmlEscapeText(user); html += F("' required>");
  html += F("<label class='small'>Password</label><input type='password' name='p' maxlength='"); html += String(WEB_PASS_MAX_LEN); html += F("' required>");
  html += F("<label class='small'>Activation Code</label><input name='code' maxlength='"); html += String(ACTIVATION_CODE_LEN); html += F("' placeholder='8 digits' required>");
  html += F("<button class='btn' type='submit'>Activate User</button></form></div>");
  html += htmlFooter();
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.send(403, "text/html", html);
}

void sendSubscriptionWarningPage(const String& user, byte daysLeft, const String& ret) {
  String html = htmlHeader("Subscription Warning");
  html += F("<div class='card'><h1>تنبيه الاشتراك</h1><div class='msg warn'>");
  html += subscriptionWarningTextWeb(user, daysLeft);
  html += F("</div><a class='btn' href='");
  html += sessionUrl(ret.length() ? ret : String("/"));
  html += F("'>متابعة الدخول</a><a class='btn btn2' href='/logout'>خروج</a></div>");
  html += htmlFooter();
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.send(200, "text/html", html);
}

void maintainMonthlyUserSubscriptionDisable(bool force) {
  if (!monthlyUserAutoDisableEnabled) return;

  unsigned long nowMs = millis();
  if (!force && lastSubscriptionMaintenanceMs != 0 && (unsigned long)(nowMs - lastSubscriptionMaintenanceMs) < 60000UL) return;
  lastSubscriptionMaintenanceMs = nowMs;

  uint32_t ym = currentYearMonthKey();
  if (ym == 0) return;

  // First ever boot after installing this feature: record the current month only.
  // This prevents surprising mid-month disabling immediately after flashing.
  if (lastSubscriptionDisableYm == 0) {
    saveMonthlySubscriptionState(ym);
    addEsp32Log(String("Subscription month baseline set: ") + String(ym));
    return;
  }

  if (ym == lastSubscriptionDisableYm) return;

  byte disabledCount = 0;
  for (byte i = 1; i < WEB_USER_COUNT; i++) {
    if (!WEB_USERS[i].user[0] || !WEB_USERS[i].pass[0]) continue;
    if (WEB_USERS[i].enabled) {
      WEB_USERS[i].enabled = 0;
      disabledCount++;
    }
  }

  if (disabledCount > 0) {
    saveWebUsersConfig();
    for (byte s = 0; s < MAX_WEB_SESSIONS; s++) {
      if (webSessions[s].active && webSessions[s].userIndex > 0) clearSessionSlot(s);
    }
    addEsp32Log(String("Monthly subscription disabled users: ") + String(disabledCount));
  } else {
    addEsp32Log("Monthly subscription check: no active users to disable");
  }

  saveMonthlySubscriptionState(ym);
}


String mainMenuCardsHtml() {
  String html;
  html.reserve(2300);
  bool admin = webIsAdmin();
  bool canEngineer = roleAtLeast(WEB_ROLE_ENGINEER);
  byte lang = currentWebLanguage();
  bool ar = (lang == TG_LANG_AR);

  html += F("<div class='card'><h1>");
  html += ar ? F("التنقل") : F("Navigation");
  html += F("</h1><div class='sub'>");
  html += ar ? F("الصفحات مرتبة حسب الوظيفة لتسهيل الاستخدام من الموبايل أو الكمبيوتر.") : F("Pages are grouped by function for easier operation from mobile or laptop.");
  html += F("</div><div class='navgrid'>");

  html += F("<a class='navcard' href='"); html += sessionUrl("/"); html += F("'><b>");
  html += ar ? F("الرئيسية") : F("Home");
  html += F("</b><span>");
  html += ar ? F("حالة سريعة وتحكم مباشر") : F("Fast status and quick control");
  html += F("</span></a>");

  html += F("<a class='navcard' href='"); html += sessionUrl("/dashboard"); html += F("'><b>");
  html += ar ? F("لوحة التحكم") : F("Dashboard");
  html += F("</b><span>");
  html += ar ? F("مراقبة كاملة، الطقس، ولوحة التحديث") : F("Full monitor, weather and update panel");
  html += F("</span></a>");

  if (canEngineer) {
    html += F("<a class='navcard' href='"); html += sessionUrl("/esp32"); html += F("'><b>");
    html += ar ? F("البيانات اللحظية / المداخل والمخارج") : F("Realtime / IO");
    html += F("</b><span>");
    html += ar ? F("WebSocket و MQTT وتيليجرام وأسماء الحقول") : F("WebSocket, MQTT, Telegram and labels");
    html += F("</span></a>");
  }

  if (canEngineer) {
    html += F("<a class='navcard' href='"); html += sessionUrl("/outputauto"); html += F("'><b>");
    html += ar ? F("الأتمتة والجداول") : F("Automation");
    html += F("</b><span>");
    html += ar ? F("وقت الإغلاق اليدوي، مؤقت التشغيل اليدوي، الجداول، ومزامنة ThingSpeak") : F("Manual Auto-OFF time, manual timer, schedules and ThingSpeak sync");
    html += F("</span></a>");
  }

  if (canEngineer) {
    html += F("<a class='navcard' href='"); html += sessionUrl("/wifi"); html += F("'><b>");
    html += ar ? F("الاتصالات") : F("Communications");
    html += F("</b><span>");
    html += ar ? F("واي فاي، ThingSpeak، DuckDNS، والوقت") : F("WiFi, ThingSpeak, DuckDNS and NTP settings");
    html += F("</span></a>");
  }

  if (canEngineer) {
    html += F("<a class='navcard' href='"); html += sessionUrl("/health"); html += F("'><b>");
    html += ar ? F("صحة النظام") : F("System Health");
    html += F("</b><span>");
    html += ar ? F("الرام، مدة التشغيل، الأخطاء، وحالة الشبكة") : F("RAM, uptime, errors and network status");
    html += F("</span></a>");
  }

  if (admin) {
    html += F("<a class='navcard' href='"); html += sessionUrl("/users"); html += F("'><b>");
    html += ar ? F("المستخدمون") : F("Users");
    html += F("</b><span>");
    html += ar ? F("الصلاحيات، الرؤية، والتحكم") : F("Roles, field visibility and control permissions");
    html += F("</span></a>");
  }

  if (admin) {
    html += F("<a class='navcard' href='"); html += sessionUrl("/update"); html += F("'><b>");
    html += ar ? F("التحديث والنسخ الاحتياطي") : F("Update / Backup");
    html += F("</b><span>");
    html += ar ? F("تحديث البرنامج، النسخ الاحتياطي، والاستعادة") : F("Firmware update, backup and restore");
    html += F("</span></a>");
  }

  html += F("<a class='navcard' href='/logout'><b>");
  html += ar ? F("تسجيل الخروج") : F("Logout");
  html += F("</b><span>");
  html += ar ? F("إنهاء الجلسة الحالية") : F("End current session");
  html += F("</span></a>");

  html += F("</div></div>");
  return html;
}

String outputAutoSyncLabel(byte idx) {
  if (idx >= OUTPUT_AUTO_COUNT) return String("Unknown");
  return outputAutoSyncThingSpeak[idx] ? String("ON - schedule/timer changes can sync to ThingSpeak") : String("OFF - schedule/timer changes are local only");
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


String normalizedTelegramBotLink() {
  String link = String(telegramBotLink);
  link.trim();
  if (!link.length()) return String("");
  if (link.startsWith("http://") || link.startsWith("https://")) return link;
  if (link.startsWith("@")) link = link.substring(1);
  return String("https://t.me/") + link;
}

void loadTelegramUiConfig() {
  telegramBotLink[0] = '\0';
  Preferences p;
  if (p.begin("tg_ui", true)) {
    String link = p.getString("botlink", "");
    link.substring(0, sizeof(telegramBotLink) - 1).toCharArray(telegramBotLink, sizeof(telegramBotLink));
    p.end();
  }
}

void saveTelegramUiConfig() {
  Preferences p;
  if (p.begin("tg_ui", false)) {
    p.putString("botlink", String(telegramBotLink));
    p.end();
  }
}

String telegramBotLinkHomeCardHtml() {
  String link = normalizedTelegramBotLink();
  if (!link.length()) return String("");
  byte lang = currentWebLanguage();
  bool ar = (lang == TG_LANG_AR);
  String html;
  html.reserve(560);
  html += F("<div class='card'><h1>");
  html += ar ? F("بوت تيليجرام") : F("Telegram Bot");
  html += F("</h1><div class='sub'>");
  html += ar ? F("افتح بوت تيليجرام للتحكم والتنبيهات.") : F("Open Telegram bot for control and alerts.");
  html += F("</div><a class='btn btn2' target='_blank' rel='noopener' href='");
  html += htmlEscapeText(link);
  html += F("'>");
  html += ar ? F("فتح تيليجرام") : F("Open Telegram");
  html += F("</a></div>");
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
  commitEeprom("eeprom-save");
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
      currentTemperature = smoothValue(currentTemperature, temp + tempCalibrationOffset);
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
      currentTemperature = smoothValue(currentTemperature, temp + tempCalibrationOffset);
      currentHumidity = smoothValue(currentHumidity, hum + humCalibrationOffset);
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


byte readDigitalInputState(uint8_t pin) {
  byte raw = (digitalRead(pin) == HIGH) ? 1 : 0;
#if DIGITAL_INPUT_ACTIVE_LOW
  return raw ? 0 : 1;
#else
  return raw;
#endif
}

byte readInput7State() {
  return readDigitalInputState(INPUT7_PIN);
}

byte readInput8State() {
  return readDigitalInputState(INPUT8_PIN);
}

byte readInput9State() {
  return readDigitalInputState(INPUT9_PIN);
}

byte readInput10State() {
  return readDigitalInputState(INPUT10_PIN);
}


void notifyInputChanged(byte fieldNumber, byte state) {
  String msg = String("Input changed: ") + fieldLabel(fieldNumber) + " " + (state ? "ON" : "OFF");
  addEsp32Log(msg);
  String ts = currentDeviceDateTimeText();
  if (ts.length() == 0) ts = "--";
  String notifyText = telegramInputChangeText(fieldNumber, state, ts);
  if (telegramNotifyUsersForField(fieldNumber, notifyText) == 0) telegramSend(telegramStatusText(msg));
  if (mqttClient.connected()) {
    if (fieldNumber == 7) publishMqtt("input7", state ? "ON" : "OFF");
    else if (fieldNumber == 8) publishMqtt("input8", state ? "ON" : "OFF");
    else if (fieldNumber == 9) publishMqtt("input9", state ? "ON" : "OFF");
    else if (fieldNumber == 10) publishMqtt("input10", state ? "ON" : "OFF");
  }
  wsBroadcastStatus();
}

void maintainOneInput(byte fieldNumber, byte rawState, byte &stableState, byte &lastRawState, unsigned long &lastRawChangeMs) {
  unsigned long now = millis();
  if (!inputDebounceReady) {
    stableState = rawState;
    lastRawState = rawState;
    lastRawChangeMs = now;
    return;
  }
  if (rawState != lastRawState) {
    lastRawState = rawState;
    lastRawChangeMs = now;
    return;
  }
  if (rawState != stableState && (unsigned long)(now - lastRawChangeMs) >= DIGITAL_INPUT_DEBOUNCE_MS) {
    stableState = rawState;
    notifyInputChanged(fieldNumber, stableState);
  }
}

void maintainDigitalInputs() {
  maintainOneInput(7, readInput7State(), stableInput7State, lastRawInput7State, input7ChangedMs);
  maintainOneInput(8, readInput8State(), stableInput8State, lastRawInput8State, input8ChangedMs);
  maintainOneInput(9, readInput9State(), stableInput9State, lastRawInput9State, input9ChangedMs);
  maintainOneInput(10, readInput10State(), stableInput10State, lastRawInput10State, input10ChangedMs);
  if (!inputDebounceReady) inputDebounceReady = true;
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
  return String("Disabled / LED separate");
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


byte outputAutoIndex(byte outputNumber) {
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
    if (OUTPUT_AUTO_FIELDS[i] == outputNumber) return i;
  }
  return 255;
}

byte outputStateByNumber(byte outputNumber) {
  if (outputNumber == 1) return currentOutput1State;
  if (outputNumber == 2) return currentOutput2State;
  if (outputNumber == 5) return currentOutput5State;
  if (outputNumber == 6) return currentOutput6State;
  return 0;
}

uint32_t currentYmdKey() {
  int y = 0; byte m = 0; byte d = 0;
  if (!getDeviceDateParts(y, m, d)) return 0;
  return (uint32_t)y * 10000UL + (uint32_t)m * 100UL + (uint32_t)d;
}

String ymdKeyText(uint32_t ymd) {
  if (ymd < 20240101UL) return String("Never");
  uint16_t y = (uint16_t)(ymd / 10000UL);
  byte m = (byte)((ymd / 100UL) % 100UL);
  byte d = (byte)(ymd % 100UL);
  char buf[12];
  snprintf(buf, sizeof(buf), "%04u-%02u-%02u", y, m, d);
  return String(buf);
}

String manualAutoOff9TimeText() {
  return minuteToTime(manualAutoOff9Minute);
}

uint16_t parseTimeToMinutesStrict(const String& text, uint16_t fallback) {
  return parseTimeToMinutes(text, fallback);
}

byte manualAutoOff9EnabledCount() {
  byte c = 0;
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) if (manualAutoOff9Enabled[i]) c++;
  if (manualAutoOff9Out3Enabled) c++;
  return c;
}

void recordOutputManualControl(byte outputNumber, byte newState) {
  byte idx = outputAutoIndex(outputNumber);
  if (idx >= OUTPUT_AUTO_COUNT) return;
  outputLastManualOn[idx] = newState ? 1 : 0;
  if (newState) startManualAutoOffDurationForOutput(outputNumber);
  else cancelManualAutoOffDurationForOutput(outputNumber);
}

void recordSmartOutput3ManualControl(byte newState) {
  output3LastManualOn = newState ? 1 : 0;
  if (newState) startManualAutoOffDurationForOut3();
  else cancelManualAutoOffDurationForOut3();
}

bool outputAutomationActiveNow(byte idx) {
  if (idx >= OUTPUT_AUTO_COUNT) return false;
#if ENABLE_LEGACY_OUTPUT_TIMERS
  if (outputAutoTimerActive[idx]) return true;
#endif
  OutputAutoConfig &cfg = outputAutoCfg[idx];
  if (cfg.enabled && cfg.scheduleEnable && outputAutoScheduleActiveNow(idx)) return true;
  return false;
}

uint32_t manualAutoBuildOffMask() {
  uint32_t mask = 0;
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) if (manualAutoOff9Enabled[i]) mask |= (1UL << i);
  if (manualAutoOff9Out3Enabled) mask |= (1UL << 4);
  return mask;
}

uint32_t manualAutoBuildDurMask() {
  uint32_t mask = 0;
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) if (manualAutoOffDurEnabled[i]) mask |= (1UL << i);
  if (manualAutoOffDurOut3Enabled) mask |= (1UL << 4);
  return mask;
}

void manualAutoApplyOffMask(uint32_t mask) {
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
    manualAutoOff9Enabled[i] = (mask & (1UL << i)) ? 1 : 0;
    outputLastManualOn[i] = outputStateByNumber(OUTPUT_AUTO_FIELDS[i]) ? 1 : 0;
  }
  manualAutoOff9Out3Enabled = (mask & (1UL << 4)) ? 1 : 0;
  output3LastManualOn = (smartCfg.mode == SMART_MODE_MANUAL_SCHED && !smartCfg.scheduleEnable && smartCfg.manualState == 1) ? 1 : 0;
}

void manualAutoApplyDurMask(uint32_t mask) {
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) manualAutoOffDurEnabled[i] = (mask & (1UL << i)) ? 1 : 0;
  manualAutoOffDurOut3Enabled = (mask & (1UL << 4)) ? 1 : 0;
}

bool loadManualAutoStoreFromEeprom() {
  if (EEPROM.read(EEPROM_MANUAL_AUTO_MAGIC_ADDR) != EEPROM_MANUAL_AUTO_MAGIC) return false;

  ManualAutoStoreRecord r;
  EEPROM.get(EEPROM_MANUAL_AUTO_DATA_ADDR, r);
  if (r.version != EEPROM_MANUAL_AUTO_VERSION) return false;
  if (r.offMinute > 1439) return false;
  if (r.out3Off9Enabled > 1 || r.out3DurEnabled > 1) return false;

  manualAutoOff9Minute = r.offMinute;
  manualAutoOff9LastYmd = r.offLastYmd;
  manualAutoApplyOffMask(r.offMask);
  manualAutoApplyDurMask(r.durMask);
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
    manualAutoOffDurMin[i] = clampManualAutoOffDurationMin(r.durMin[i]);
    manualAutoOffDurDeadlineEpoch[i] = r.durDeadlineEpoch[i];
    manualAutoOffDurDeadlineMs[i] = 0;
  }
  manualAutoOff9Out3Enabled = r.out3Off9Enabled ? 1 : 0;
  manualAutoOffDurOut3Enabled = r.out3DurEnabled ? 1 : 0;
  manualAutoOffDurOut3Min = clampManualAutoOffDurationMin(r.out3DurMin);
  manualAutoOffDurOut3DeadlineEpoch = r.out3DurDeadlineEpoch;
  manualAutoOffDurOut3DeadlineMs = 0;
  return true;
}

void saveManualAutoStoreToEeprom() {
  ManualAutoStoreRecord r;
  memset(&r, 0, sizeof(r));
  r.version = EEPROM_MANUAL_AUTO_VERSION;
  r.offMinute = manualAutoOff9Minute;
  r.offLastYmd = manualAutoOff9LastYmd;
  r.offMask = manualAutoBuildOffMask();
  r.durMask = manualAutoBuildDurMask();
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
    r.durMin[i] = clampManualAutoOffDurationMin(manualAutoOffDurMin[i]);
    r.durDeadlineEpoch[i] = manualAutoOffDurDeadlineEpoch[i];
  }
  r.out3Off9Enabled = manualAutoOff9Out3Enabled ? 1 : 0;
  r.out3DurEnabled = manualAutoOffDurOut3Enabled ? 1 : 0;
  r.out3DurMin = clampManualAutoOffDurationMin(manualAutoOffDurOut3Min);
  r.out3DurDeadlineEpoch = manualAutoOffDurOut3DeadlineEpoch;

  EEPROM.write(EEPROM_MANUAL_AUTO_MAGIC_ADDR, EEPROM_MANUAL_AUTO_MAGIC);
  EEPROM.put(EEPROM_MANUAL_AUTO_DATA_ADDR, r);
  commitEeprom("eeprom-save");
}

bool loadManualAutoOffFromUnifiedNvs() {
  bool ok = false;
  uint32_t mask = 0;
  manualAutoUnifiedPrefs.begin("manAuto", true);
  ok = manualAutoUnifiedPrefs.isKey("offMask") || manualAutoUnifiedPrefs.isKey("offMin");
  if (ok) {
    mask = manualAutoUnifiedPrefs.getUInt("offMask", 0);
    manualAutoOff9LastYmd = manualAutoUnifiedPrefs.getUInt("offLast", 0);
    manualAutoOff9Minute = (uint16_t)manualAutoUnifiedPrefs.getUInt("offMin", MANUAL_AUTO_OFF9_DEFAULT_MINUTE);
  }
  manualAutoUnifiedPrefs.end();
  if (!ok) return false;
  if (manualAutoOff9Minute > 1439) manualAutoOff9Minute = MANUAL_AUTO_OFF9_DEFAULT_MINUTE;
  manualAutoApplyOffMask(mask);
  return true;
}

bool loadManualAutoDurFromUnifiedNvs() {
  bool ok = false;
  uint32_t mask = 0;
  manualAutoUnifiedPrefs.begin("manAuto", true);
  ok = manualAutoUnifiedPrefs.isKey("durMask");
  if (ok) {
    mask = manualAutoUnifiedPrefs.getUInt("durMask", 0);
    for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
      char key[12];
      snprintf(key, sizeof(key), "durM%u", (unsigned)i);
      manualAutoOffDurMin[i] = clampManualAutoOffDurationMin(manualAutoUnifiedPrefs.getUInt(key, MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN));
      snprintf(key, sizeof(key), "durE%u", (unsigned)i);
      manualAutoOffDurDeadlineEpoch[i] = manualAutoUnifiedPrefs.getUInt(key, 0);
      manualAutoOffDurDeadlineMs[i] = 0;
    }
    manualAutoOffDurOut3Min = clampManualAutoOffDurationMin(manualAutoUnifiedPrefs.getUInt("durO3M", MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN));
    manualAutoOffDurOut3DeadlineEpoch = manualAutoUnifiedPrefs.getUInt("durO3E", 0);
    manualAutoOffDurOut3DeadlineMs = 0;
  }
  manualAutoUnifiedPrefs.end();
  if (!ok) return false;
  manualAutoApplyDurMask(mask);
  return true;
}

bool loadManualAutoOffFromLegacyNvs() {
  uint32_t mask = 0;
  manualAutoOff9Prefs.begin("manOff9", true);
  bool ok = manualAutoOff9Prefs.isKey("mask") || manualAutoOff9Prefs.isKey("minute");
  if (ok) {
    mask = manualAutoOff9Prefs.getUInt("mask", 0);
    manualAutoOff9LastYmd = manualAutoOff9Prefs.getUInt("lastYmd", 0);
    manualAutoOff9Minute = (uint16_t)manualAutoOff9Prefs.getUInt("minute", MANUAL_AUTO_OFF9_DEFAULT_MINUTE);
  }
  manualAutoOff9Prefs.end();
  if (!ok) return false;
  if (manualAutoOff9Minute > 1439) manualAutoOff9Minute = MANUAL_AUTO_OFF9_DEFAULT_MINUTE;
  manualAutoApplyOffMask(mask);
  return true;
}

bool loadManualAutoDurFromLegacyNvs() {
  uint32_t mask = 0;
  manualAutoOffDurPrefs.begin("manDur", true);
  bool ok = manualAutoOffDurPrefs.isKey("mask");
  if (ok) {
    mask = manualAutoOffDurPrefs.getUInt("mask", 0);
    for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
      char key[8];
      snprintf(key, sizeof(key), "m%u", (unsigned)i);
      manualAutoOffDurMin[i] = clampManualAutoOffDurationMin(manualAutoOffDurPrefs.getUInt(key, MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN));
      snprintf(key, sizeof(key), "e%u", (unsigned)i);
      manualAutoOffDurDeadlineEpoch[i] = manualAutoOffDurPrefs.getUInt(key, 0);
      manualAutoOffDurDeadlineMs[i] = 0;
    }
    manualAutoOffDurOut3Min = clampManualAutoOffDurationMin(manualAutoOffDurPrefs.getUInt("m3o", MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN));
    manualAutoOffDurOut3DeadlineEpoch = manualAutoOffDurPrefs.getUInt("e3o", 0);
    manualAutoOffDurOut3DeadlineMs = 0;
  }
  manualAutoOffDurPrefs.end();
  if (!ok) return false;
  manualAutoApplyDurMask(mask);
  return true;
}

void saveManualAutoOff9Config() {
  uint32_t mask = manualAutoBuildOffMask();

  // V1.5.2: NVS is the primary reader for manual checkboxes, because it is independent
  // from the older EEPROM map. EEPROM remains as a mirror/backward-compatible copy.
  manualAutoUnifiedPrefs.begin("manAuto", false);
  manualAutoUnifiedPrefs.putUInt("offMask", mask);
  manualAutoUnifiedPrefs.putUInt("offLast", manualAutoOff9LastYmd);
  manualAutoUnifiedPrefs.putUInt("offMin", manualAutoOff9Minute);
  manualAutoUnifiedPrefs.end();

  manualAutoOff9Prefs.begin("manOff9", false);
  manualAutoOff9Prefs.putUInt("mask", mask);
  manualAutoOff9Prefs.putUInt("lastYmd", manualAutoOff9LastYmd);
  manualAutoOff9Prefs.putUInt("minute", manualAutoOff9Minute);
  manualAutoOff9Prefs.end();

  saveManualAutoStoreToEeprom();
  manualAutoLoadedFromEeprom = true;
}

void saveManualAutoOffDurationConfig() {
  uint32_t mask = manualAutoBuildDurMask();

  manualAutoUnifiedPrefs.begin("manAuto", false);
  manualAutoUnifiedPrefs.putUInt("durMask", mask);
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
    char key[12];
    snprintf(key, sizeof(key), "durM%u", (unsigned)i);
    manualAutoUnifiedPrefs.putUInt(key, manualAutoOffDurMin[i]);
    snprintf(key, sizeof(key), "durE%u", (unsigned)i);
    manualAutoUnifiedPrefs.putUInt(key, manualAutoOffDurDeadlineEpoch[i]);
  }
  manualAutoUnifiedPrefs.putUInt("durO3M", manualAutoOffDurOut3Min);
  manualAutoUnifiedPrefs.putUInt("durO3E", manualAutoOffDurOut3DeadlineEpoch);
  manualAutoUnifiedPrefs.end();

  manualAutoOffDurPrefs.begin("manDur", false);
  manualAutoOffDurPrefs.putUInt("mask", mask);
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "m%u", (unsigned)i);
    manualAutoOffDurPrefs.putUInt(key, manualAutoOffDurMin[i]);
    snprintf(key, sizeof(key), "e%u", (unsigned)i);
    manualAutoOffDurPrefs.putUInt(key, manualAutoOffDurDeadlineEpoch[i]);
  }
  manualAutoOffDurPrefs.putUInt("m3o", manualAutoOffDurOut3Min);
  manualAutoOffDurPrefs.putUInt("e3o", manualAutoOffDurOut3DeadlineEpoch);
  manualAutoOffDurPrefs.end();

  saveManualAutoStoreToEeprom();
  manualAutoLoadedFromEeprom = true;
}

void loadManualAutoOff9Config() {
  manualAutoOff9Minute = MANUAL_AUTO_OFF9_DEFAULT_MINUTE;
  manualAutoOff9LastYmd = 0;

  // V1.5.5: EEPROM is the primary and authoritative store again.
  // NVS is used only as a fallback if the EEPROM manual block does not exist yet.
  manualAutoLoadedFromEeprom = loadManualAutoStoreFromEeprom();
  if (manualAutoLoadedFromEeprom) return;

  bool nvsOk = loadManualAutoOffFromUnifiedNvs();
  if (!nvsOk) nvsOk = loadManualAutoOffFromLegacyNvs();

  if (!nvsOk) {
    manualAutoApplyOffMask(0);
  }
}

uint16_t clampManualAutoOffDurationMin(uint32_t minutes) {
  if (minutes < MANUAL_AUTO_OFF_DURATION_MIN) return MANUAL_AUTO_OFF_DURATION_MIN;
  if (minutes > MANUAL_AUTO_OFF_DURATION_MAX_MIN) return MANUAL_AUTO_OFF_DURATION_MAX_MIN;
  return (uint16_t)minutes;
}

void loadManualAutoOffDurationConfig() {
  // If the EEPROM manual block was loaded by loadManualAutoOff9Config(), it already
  // contains both daily-close and duration settings. Do not let stale NVS override it.
  if (manualAutoLoadedFromEeprom) return;

  bool nvsOk = loadManualAutoDurFromUnifiedNvs();
  if (!nvsOk) nvsOk = loadManualAutoDurFromLegacyNvs();

  if (!nvsOk) {
    manualAutoApplyDurMask(0);
    for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
      manualAutoOffDurMin[i] = MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN;
      manualAutoOffDurDeadlineEpoch[i] = 0;
      manualAutoOffDurDeadlineMs[i] = 0;
    }
    manualAutoOffDurOut3Min = MANUAL_AUTO_OFF_DURATION_DEFAULT_MIN;
    manualAutoOffDurOut3DeadlineEpoch = 0;
    manualAutoOffDurOut3DeadlineMs = 0;
  }
}

void startManualAutoOffDurationForOutput(byte outputNumber) {
  byte idx = outputAutoIndex(outputNumber);
  if (idx >= OUTPUT_AUTO_COUNT) return;
  if (!manualAutoOffDurEnabled[idx]) {
    manualAutoOffDurDeadlineEpoch[idx] = 0;
    manualAutoOffDurDeadlineMs[idx] = 0;
    saveManualAutoOffDurationConfig();
    return;
  }
  uint32_t seconds = (uint32_t)clampManualAutoOffDurationMin(manualAutoOffDurMin[idx]) * 60UL;
  manualAutoOffDurDeadlineMs[idx] = millis() + seconds * 1000UL;
  manualAutoOffDurDeadlineEpoch[idx] = isTimeValidNow() ? (getDeviceEpoch() + seconds) : 0;
  saveManualAutoOffDurationConfig();
}

void cancelManualAutoOffDurationForOutput(byte outputNumber) {
  byte idx = outputAutoIndex(outputNumber);
  if (idx >= OUTPUT_AUTO_COUNT) return;
  manualAutoOffDurDeadlineEpoch[idx] = 0;
  manualAutoOffDurDeadlineMs[idx] = 0;
  saveManualAutoOffDurationConfig();
}

void startManualAutoOffDurationForOut3() {
  if (!manualAutoOffDurOut3Enabled) {
    manualAutoOffDurOut3DeadlineEpoch = 0;
    manualAutoOffDurOut3DeadlineMs = 0;
    saveManualAutoOffDurationConfig();
    return;
  }
  uint32_t seconds = (uint32_t)clampManualAutoOffDurationMin(manualAutoOffDurOut3Min) * 60UL;
  manualAutoOffDurOut3DeadlineMs = millis() + seconds * 1000UL;
  manualAutoOffDurOut3DeadlineEpoch = isTimeValidNow() ? (getDeviceEpoch() + seconds) : 0;
  saveManualAutoOffDurationConfig();
}

void cancelManualAutoOffDurationForOut3() {
  manualAutoOffDurOut3DeadlineEpoch = 0;
  manualAutoOffDurOut3DeadlineMs = 0;
  saveManualAutoOffDurationConfig();
}

uint32_t manualAutoOffDurationRemainingSec(byte idx) {
  if (idx >= OUTPUT_AUTO_COUNT) return 0;
  byte out = OUTPUT_AUTO_FIELDS[idx];
  if (!manualAutoOffDurEnabled[idx] || outputStateByNumber(out) == 0) return 0;
  if (manualAutoOffDurDeadlineEpoch[idx] > 0 && isTimeValidNow()) {
    uint32_t now = getDeviceEpoch();
    if (now >= manualAutoOffDurDeadlineEpoch[idx]) return 0;
    return manualAutoOffDurDeadlineEpoch[idx] - now;
  }
  if (manualAutoOffDurDeadlineMs[idx] > 0) {
    unsigned long nowMs = millis();
    if ((long)(nowMs - manualAutoOffDurDeadlineMs[idx]) >= 0) return 0;
    return (uint32_t)((manualAutoOffDurDeadlineMs[idx] - nowMs) / 1000UL);
  }
  return 0;
}

uint32_t manualAutoOffDurationOut3RemainingSec() {
  if (!manualAutoOffDurOut3Enabled || !smartOutputState) return 0;
  if (manualAutoOffDurOut3DeadlineEpoch > 0 && isTimeValidNow()) {
    uint32_t now = getDeviceEpoch();
    if (now >= manualAutoOffDurOut3DeadlineEpoch) return 0;
    return manualAutoOffDurOut3DeadlineEpoch - now;
  }
  if (manualAutoOffDurOut3DeadlineMs > 0) {
    unsigned long nowMs = millis();
    if ((long)(nowMs - manualAutoOffDurOut3DeadlineMs) >= 0) return 0;
    return (uint32_t)((manualAutoOffDurOut3DeadlineMs - nowMs) / 1000UL);
  }
  return 0;
}

void maintainManualAutoOffDuration() {
  if (millis() - lastManualAutoOffDurCheckMs < MANUAL_AUTO_OFF_DURATION_CHECK_MS) return;
  lastManualAutoOffDurCheckMs = millis();
  byte offCount = 0;

  for (byte idx = 0; idx < OUTPUT_AUTO_COUNT; idx++) {
    if (!manualAutoOffDurEnabled[idx]) continue;
    if (!outputLastManualOn[idx]) continue;
    if (outputAutomationActiveNow(idx)) continue;
    byte out = OUTPUT_AUTO_FIELDS[idx];
    if (outputStateByNumber(out) == 0) { cancelManualAutoOffDurationForOutput(out); outputLastManualOn[idx] = 0; continue; }
    if (manualAutoOffDurationRemainingSec(idx) == 0 && (manualAutoOffDurDeadlineEpoch[idx] || manualAutoOffDurDeadlineMs[idx])) {
      String msg = String("Manual Auto-Off Timer ") + String(manualAutoOffDurMin[idx]) + String(" min");
      bool changed = setOutputStateInternal(out, 0, msg.c_str(), true, true);
      if (changed) offCount++;
      outputLastManualOn[idx] = 0;
      cancelManualAutoOffDurationForOutput(out);
    }
    yield();
  }

  if (manualAutoOffDurOut3Enabled && output3LastManualOn) {
    bool smartManualOwner = (smartCfg.mode == SMART_MODE_MANUAL_SCHED && !smartCfg.scheduleEnable);
    if (!smartManualOwner || !smartOutputState) {
      if (!smartOutputState) cancelManualAutoOffDurationForOut3();
    } else if (manualAutoOffDurationOut3RemainingSec() == 0 && (manualAutoOffDurOut3DeadlineEpoch || manualAutoOffDurOut3DeadlineMs)) {
      smartCfg.manualState = 0;
      saveSmartOutputConfig();
      writeSmartOutput(false);
      recordSmartOutput3ManualControl(0);
      rememberOutputCommand("Manual Auto-Off Timer", 3, 0, true);
      wsBroadcastStatus();
      offCount++;
    }
  }

  if (offCount > 0) addEsp32Log(String("Manual Auto-Off Timer closed outputs: ") + String(offCount));
}

void maintainManualAutoOff9() {
  if (millis() - lastManualAutoOff9CheckMs < MANUAL_AUTO_OFF9_CHECK_MS) return;
  lastManualAutoOff9CheckMs = millis();
  if (!isTimeValidNow()) return;

  time_t now = (time_t)getDeviceEpoch();
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  uint16_t nowMin = (uint16_t)(tmNow.tm_hour * 60 + tmNow.tm_min);
  uint16_t endMin = (uint16_t)(manualAutoOff9Minute + MANUAL_AUTO_OFF9_WINDOW_MIN);
  bool inWindow = false;
  if (endMin <= 1440) inWindow = (nowMin >= manualAutoOff9Minute && nowMin < endMin);
  else inWindow = (nowMin >= manualAutoOff9Minute || nowMin < (endMin - 1440));
  if (!inWindow) return;

  uint32_t ymd = currentYmdKey();
  if (ymd == 0 || manualAutoOff9LastYmd == ymd) return;

  byte offCount = 0;
  for (byte idx = 0; idx < OUTPUT_AUTO_COUNT; idx++) {
    if (!manualAutoOff9Enabled[idx]) continue;
    if (!outputLastManualOn[idx]) continue;
    if (outputAutomationActiveNow(idx)) continue; // Never override timer/schedule ownership.
    byte out = OUTPUT_AUTO_FIELDS[idx];
    if (outputStateByNumber(out) == 1) {
      String msg = String("Manual Auto OFF ") + manualAutoOff9TimeText();

bool changed = setOutputStateInternal(
    out,
    0,
    msg.c_str(),
    true,
    true
);
      if (changed) offCount++;
    }
    outputLastManualOn[idx] = 0;
    yield();
  }

  if (manualAutoOff9Out3Enabled && output3LastManualOn) {
    bool smartManualOwner = (smartCfg.mode == SMART_MODE_MANUAL_SCHED && !smartCfg.scheduleEnable);
    if (smartManualOwner && smartOutputState) {
      smartCfg.manualState = 0;
      saveSmartOutputConfig();
      writeSmartOutput(false);
      recordSmartOutput3ManualControl(0);
      String cmd = String("Manual Auto OFF ") + manualAutoOff9TimeText();

rememberOutputCommand(
    cmd.c_str(),
    3,
    0,
    true
);
      wsBroadcastStatus();
      offCount++;
    } else if (smartManualOwner) {
      recordSmartOutput3ManualControl(0);
    }
  }

  manualAutoOff9LastYmd = ymd;
  saveManualAutoOff9Config();
  if (offCount > 0) addEsp32Log(String("Manual Auto OFF ") + manualAutoOff9TimeText() + String(" closed outputs: ") + String(offCount));
  else addEsp32Log(String("Manual Auto OFF ") + manualAutoOff9TimeText() + String(" checked: nothing to close"));
}

void setOutputAutoDefaults(byte idx) {
  if (idx >= OUTPUT_AUTO_COUNT) return;
  outputAutoCfg[idx].enabled = 0;
  outputAutoCfg[idx].scheduleEnable = 0;
  outputAutoCfg[idx].timerMode = 0;
  outputAutoCfg[idx].repeatWeekly = 1;
  outputAutoCfg[idx].timerDurationSec = 600UL;
  for (byte d = 0; d < 7; d++) {
    outputAutoCfg[idx].dayEnabled[d] = 1;
    outputAutoCfg[idx].slotEnabled[d][0] = 0;
    outputAutoCfg[idx].slotEnabled[d][1] = 0;
    outputAutoCfg[idx].dayStartMin[d][0] = 8 * 60;
    outputAutoCfg[idx].dayEndMin[d][0] = 9 * 60;
    outputAutoCfg[idx].dayStartMin[d][1] = 18 * 60;
    outputAutoCfg[idx].dayEndMin[d][1] = 19 * 60;
  }
}

bool outputAutoConfigLooksValid(const OutputAutoConfig &c) {
  if (c.enabled > 1 || c.scheduleEnable > 1 || c.timerMode > 1 || c.repeatWeekly > 1) return false;
  if (c.timerDurationSec > 86399UL) return false;
  for (byte d = 0; d < 7; d++) {
    if (c.dayEnabled[d] > 1) return false;
    for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
      if (c.slotEnabled[d][sl] > 1) return false;
      if (c.dayStartMin[d][sl] > 1439 || c.dayEndMin[d][sl] > 1439) return false;
    }
  }
  return true;
}

void loadOutputAutoConfigs() {
  bool ok = (EEPROM.read(EEPROM_OUTPUT_AUTO_MAGIC_ADDR) == EEPROM_OUTPUT_AUTO_MAGIC);
  if (ok) EEPROM.get(EEPROM_OUTPUT_AUTO_DATA_ADDR, outputAutoCfg);
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
    if (!ok || !outputAutoConfigLooksValid(outputAutoCfg[i])) setOutputAutoDefaults(i);
    outputAutoTimerActive[i] = false;
  }
  if (!ok) saveOutputAutoConfigs();
}

void saveOutputAutoConfigs() {
  EEPROM.write(EEPROM_OUTPUT_AUTO_MAGIC_ADDR, EEPROM_OUTPUT_AUTO_MAGIC);
  EEPROM.put(EEPROM_OUTPUT_AUTO_DATA_ADDR, outputAutoCfg);
  commitEeprom("eeprom-save");
}

void loadOutputAutoSyncConfig() {
  bool ok = (EEPROM.read(EEPROM_OUTPUT_AUTO_SYNC_MAGIC_ADDR) == EEPROM_OUTPUT_AUTO_SYNC_MAGIC);
  if (ok) {
    for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
      byte v = EEPROM.read(EEPROM_OUTPUT_AUTO_SYNC_DATA_ADDR + i);
      outputAutoSyncThingSpeak[i] = (v == 1) ? 1 : 0;
    }
  } else {
    for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) outputAutoSyncThingSpeak[i] = 0;
    saveOutputAutoSyncConfig();
  }
}

void saveOutputAutoSyncConfig() {
  EEPROM.write(EEPROM_OUTPUT_AUTO_SYNC_MAGIC_ADDR, EEPROM_OUTPUT_AUTO_SYNC_MAGIC);
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
    EEPROM.write(EEPROM_OUTPUT_AUTO_SYNC_DATA_ADDR + i, outputAutoSyncThingSpeak[i] ? 1 : 0);
  }
  commitEeprom("eeprom-save");
}

bool outputAutoScheduleActiveNow(byte idx) {
  if (idx >= OUTPUT_AUTO_COUNT) return false;
  OutputAutoConfig &cfg = outputAutoCfg[idx];
  if (!cfg.enabled || !cfg.scheduleEnable) return false;
  if (!isTimeValidNow()) return false;
  time_t now = (time_t)getDeviceEpoch();
  struct tm *tmNow = gmtime(&now);
  if (!tmNow) return false;
  byte today = (byte)tmNow->tm_wday;
  byte prevDay = (today == 0) ? 6 : (today - 1);
  uint16_t nowMin = (uint16_t)(tmNow->tm_hour * 60 + tmNow->tm_min);
  if (cfg.dayEnabled[today]) {
    for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
      if (!cfg.slotEnabled[today][sl]) continue;
      uint16_t st = cfg.dayStartMin[today][sl];
      uint16_t en = cfg.dayEndMin[today][sl];
      if (st == en) continue;
      if (st < en && nowMin >= st && nowMin < en) return true;
      if (st > en && nowMin >= st) return true;
    }
  }
  if (cfg.dayEnabled[prevDay]) {
    for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
      if (!cfg.slotEnabled[prevDay][sl]) continue;
      uint16_t st = cfg.dayStartMin[prevDay][sl];
      uint16_t en = cfg.dayEndMin[prevDay][sl];
      if (st > en && nowMin < en) return true;
    }
  }
  return false;
}

uint32_t outputAutoTimerRemainingSec(byte idx) {
#if !ENABLE_LEGACY_OUTPUT_TIMERS
  return 0;
#endif
  if (idx >= OUTPUT_AUTO_COUNT || !outputAutoTimerActive[idx]) return 0;
  uint32_t elapsed = (uint32_t)((millis() - outputAutoTimerStartMs[idx]) / 1000UL);
  if (elapsed >= outputAutoCfg[idx].timerDurationSec) return 0;
  return outputAutoCfg[idx].timerDurationSec - elapsed;
}

void startOutputAutoTimer(byte idx) {
#if !ENABLE_LEGACY_OUTPUT_TIMERS
  if (idx < OUTPUT_AUTO_COUNT) outputAutoTimerActive[idx] = false;
  return;
#endif
  if (idx >= OUTPUT_AUTO_COUNT) return;
  outputAutoCfg[idx].timerDurationSec = clampTimerSeconds(outputAutoCfg[idx].timerDurationSec);
  outputAutoTimerStartMs[idx] = millis();
  outputAutoTimerActive[idx] = true;
  byte out = OUTPUT_AUTO_FIELDS[idx];
  setOutputFromAutomation(out, 1, "Output Timer");
}

void stopOutputAutoTimer(byte idx, bool forceOff) {
  if (idx >= OUTPUT_AUTO_COUNT) return;
  bool wasActive = outputAutoTimerActive[idx];
  outputAutoTimerActive[idx] = false;
  if (forceOff && wasActive) {
    byte out = OUTPUT_AUTO_FIELDS[idx];
    setOutputFromAutomation(out, 0, "Output Timer Expire");
  }
}

void maintainOutputAutoControl() {
  bool anyTimerActive = false;

  for (byte idx = 0; idx < OUTPUT_AUTO_COUNT; idx++) {
    byte out = OUTPUT_AUTO_FIELDS[idx];
    OutputAutoConfig &cfg = outputAutoCfg[idx];

#if ENABLE_LEGACY_OUTPUT_TIMERS
    // Runtime timer must be maintained even if the user later opens the page while automation checkbox is off.
    // Timer/Schedule output changes are runtime changes only: they do NOT save the output state to EEPROM.
    if (outputAutoTimerActive[idx]) {
      anyTimerActive = true;
      uint32_t elapsed = (uint32_t)((millis() - outputAutoTimerStartMs[idx]) / 1000UL);
      if (elapsed >= cfg.timerDurationSec) {
        stopOutputAutoTimer(idx, true);
      } else {
        if (outputStateByNumber(out) != 1) setOutputFromAutomation(out, 1, "Output Timer");
      }
      continue;
    }
#else
    outputAutoTimerActive[idx] = false;
#endif

    if (!cfg.enabled) continue;

    if (cfg.scheduleEnable) {
      byte desired = outputAutoScheduleActiveNow(idx) ? 1 : 0;
      if (outputStateByNumber(out) != desired) setOutputFromAutomation(out, desired, "Output Schedule");
    }
  }

  // While a runtime timer is active, broadcast live remaining time about once per second.
  // This keeps the WebSocket dashboard and timer page fresh without changing outputs or writing EEPROM.
  if (anyTimerActive && millis() - lastOutputAutoLiveBroadcastMs >= 1000UL) {
    lastOutputAutoLiveBroadcastMs = millis();
    wsBroadcastStatus();
  }
}

bool systemNtpTimeValid() {
  time_t now = time(nullptr);
  return now > 1700000000UL;
}

#if ENABLE_DS3231_RTC
byte rtcBcdToDec(byte v) {
  return (byte)(((v >> 4) * 10) + (v & 0x0F));
}

byte rtcDecToBcd(byte v) {
  return (byte)(((v / 10) << 4) | (v % 10));
}

bool rtcBeginWireOnce() {
  if (!rtcWireStarted) {
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    Wire.setClock(100000UL);
    rtcWireStarted = true;
  }
  return true;
}

bool rtcProbeDs3231() {
  rtcBeginWireOnce();
  Wire.beginTransmission((uint8_t)DS3231_I2C_ADDRESS);
  bool ok = (Wire.endTransmission() == 0);
  rtcPresent = ok;
  rtcLastProbeMs = millis();
  if (!ok) {
    rtcTimeValid = false;
    lastRtcMessage = "DS3231 not found on GPIO21/GPIO22";
  }
  return ok;
}

bool rtcReadRegister(byte reg, byte &value) {
  if (!rtcPresent && !rtcProbeDs3231()) return false;
  Wire.beginTransmission((uint8_t)DS3231_I2C_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) { rtcPresent = false; return false; }
  if (Wire.requestFrom((uint8_t)DS3231_I2C_ADDRESS, (uint8_t)1) != 1) { rtcPresent = false; return false; }
  value = Wire.read();
  return true;
}

bool rtcWriteRegister(byte reg, byte value) {
  if (!rtcPresent && !rtcProbeDs3231()) return false;
  Wire.beginTransmission((uint8_t)DS3231_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  if (Wire.endTransmission() != 0) { rtcPresent = false; return false; }
  return true;
}

bool rtcReadLostPowerFlag() {
  byte st = 0;
  if (!rtcReadRegister(0x0F, st)) return true;
  return (st & 0x80) != 0; // OSF bit
}

void rtcClearLostPowerFlag() {
  byte st = 0;
  if (rtcReadRegister(0x0F, st)) {
    st &= 0x7F;
    rtcWriteRegister(0x0F, st);
  }
}

int32_t rtcDaysFromCivil(int y, unsigned m, unsigned d) {
  // Howard Hinnant civil calendar algorithm. Returns days from 1970-01-01.
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

uint32_t rtcEpochFromUtcParts(int y, byte m, byte d, byte hh, byte mm, byte ss) {
  if (y < 2023 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 59) return 0;
  static const byte mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  byte maxd = mdays[m - 1];
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) maxd = 29;
  if (d > maxd) return 0;
  int32_t days = rtcDaysFromCivil(y, m, d);
  if (days < 0) return 0;
  return (uint32_t)days * 86400UL + (uint32_t)hh * 3600UL + (uint32_t)mm * 60UL + ss;
}

bool rtcReadUtcNowRaw(uint32_t &epoch) {
  epoch = 0;
  if (!rtcPresent && !rtcProbeDs3231()) return false;

  Wire.beginTransmission((uint8_t)DS3231_I2C_ADDRESS);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) { rtcPresent = false; rtcTimeValid = false; return false; }

  if (Wire.requestFrom((uint8_t)DS3231_I2C_ADDRESS, (uint8_t)7) != 7) {
    rtcPresent = false;
    rtcTimeValid = false;
    return false;
  }

  byte sec = rtcBcdToDec(Wire.read() & 0x7F);
  byte minute = rtcBcdToDec(Wire.read() & 0x7F);
  byte hourReg = Wire.read();
  byte hour = 0;
  if (hourReg & 0x40) {
    // 12-hour mode. Convert to 0..23.
    hour = rtcBcdToDec(hourReg & 0x1F);
    bool pm = (hourReg & 0x20) != 0;
    if (hour == 12) hour = 0;
    if (pm) hour += 12;
  } else {
    hour = rtcBcdToDec(hourReg & 0x3F);
  }
  (void)Wire.read(); // day-of-week register
  byte day = rtcBcdToDec(Wire.read() & 0x3F);
  byte monthReg = Wire.read();
  byte month = rtcBcdToDec(monthReg & 0x1F);
  byte year2 = rtcBcdToDec(Wire.read());
  int year = 2000 + year2;

  rtcLostPower = rtcReadLostPowerFlag();
  if (rtcLostPower) {
    rtcTimeValid = false;
    lastRtcMessage = "DS3231 oscillator stop flag set; waiting NTP sync";
    return false;
  }

  epoch = rtcEpochFromUtcParts(year, month, day, hour, minute, sec);
  if (epoch <= RTC_MIN_VALID_EPOCH) {
    rtcTimeValid = false;
    lastRtcMessage = "DS3231 time invalid; waiting NTP sync";
    return false;
  }

  rtcTimeValid = true;
  rtcLastUtcEpoch = epoch;
  rtcLastReadMs = millis();
  lastRtcMessage = "DS3231 OK - UTC backup clock";
  return true;
}

bool rtcWriteUtcFromNtp(uint32_t utcEpoch) {
  if (utcEpoch <= RTC_MIN_VALID_EPOCH) return false;
  if (!rtcPresent && !rtcProbeDs3231()) return false;

  time_t t = (time_t)utcEpoch;
  struct tm tmUtc;
  gmtime_r(&t, &tmUtc);

  byte year2 = (byte)((tmUtc.tm_year + 1900) - 2000);
  if (year2 > 99) return false;

  Wire.beginTransmission((uint8_t)DS3231_I2C_ADDRESS);
  Wire.write((uint8_t)0x00);
  Wire.write(rtcDecToBcd((byte)tmUtc.tm_sec));
  Wire.write(rtcDecToBcd((byte)tmUtc.tm_min));
  Wire.write(rtcDecToBcd((byte)tmUtc.tm_hour)); // 24-hour mode
  Wire.write(rtcDecToBcd((byte)(tmUtc.tm_wday + 1))); // DS3231 day 1..7
  Wire.write(rtcDecToBcd((byte)tmUtc.tm_mday));
  Wire.write(rtcDecToBcd((byte)(tmUtc.tm_mon + 1)));
  Wire.write(rtcDecToBcd(year2));
  if (Wire.endTransmission() != 0) {
    rtcPresent = false;
    return false;
  }

  rtcClearLostPowerFlag();
  rtcLostPower = false;
  rtcTimeValid = true;
  rtcSyncedFromNtp = true;
  rtcLastUtcEpoch = utcEpoch;
  rtcLastReadMs = millis();
  rtcLastNtpSyncMs = millis();
  lastRtcMessage = "DS3231 synced from NTP";
  addEsp32Log("DS3231 RTC synced from NTP");
  return true;
}

uint32_t getRtcUtcEpochCached(bool forceRead) {
  if (!rtcPresent) {
    if (!forceRead && rtcLastProbeMs != 0 && millis() - rtcLastProbeMs < RTC_PROBE_INTERVAL_MS) return 0;
    if (!rtcProbeDs3231()) return 0;
  }

  if (forceRead || rtcLastReadMs == 0 || millis() - rtcLastReadMs >= RTC_READ_INTERVAL_MS) {
    uint32_t e = 0;
    if (!rtcReadUtcNowRaw(e)) return 0;
  }

  if (!rtcTimeValid || rtcLastUtcEpoch <= RTC_MIN_VALID_EPOCH) return 0;
  return rtcLastUtcEpoch + (uint32_t)((millis() - rtcLastReadMs) / 1000UL);
}

void initRtcDs3231() {
  rtcBeginWireOnce();
  if (rtcProbeDs3231()) {
    uint32_t e = 0;
    if (rtcReadUtcNowRaw(e)) {
      lastRtcMessage = "DS3231 detected and time valid";
      // Make the RTC immediately available before WiFi/NTP starts.
      if (!systemNtpTimeValid()) {
        offlineClockBaseEpoch = e;
        offlineClockBaseMs = millis();
        clockState = 3;
      }
    } else {
      lastRtcMessage = rtcLostPower ? "DS3231 detected but battery/time invalid" : "DS3231 detected but time invalid";
    }
  }
}

void maintainRtcDs3231() {
  if (!rtcPresent) {
    getRtcUtcEpochCached(false);
  }

  if (systemNtpTimeValid()) {
    uint32_t utcNow = (uint32_t)time(nullptr);
    bool needSync = rtcPresent && (rtcLastNtpSyncMs == 0 || millis() - rtcLastNtpSyncMs >= RTC_NTP_SYNC_INTERVAL_MS || rtcLostPower || !rtcTimeValid);
    if (needSync) rtcWriteUtcFromNtp(utcNow);
  } else {
    // Refresh the cache while offline so schedules continue from the hardware RTC.
    getRtcUtcEpochCached(false);
  }
}

String rtcStatusText() {
  if (!rtcPresent) return String("DS3231 not found on GPIO21/GPIO22");
  if (rtcLostPower) return String("DS3231 battery/time invalid - waiting NTP sync");
  if (rtcTimeValid) {
    String s = String("DS3231 OK");
    if (rtcSyncedFromNtp) s += String(" / synced from NTP");
    s += String(" / SDA21 SCL22");
    return s;
  }
  return String("DS3231 detected - waiting valid time");
}
#else
void initRtcDs3231() {}
void maintainRtcDs3231() {}
String rtcStatusText() { return String("RTC disabled"); }
uint32_t getRtcUtcEpochCached(bool forceRead) { (void)forceRead; return 0; }
#endif

uint32_t getRawUtcEpoch() {
  // Raw UTC source priority: NTP -> DS3231 RTC -> last saved UTC + millis().
  if (systemNtpTimeValid()) return (uint32_t)time(nullptr);
  uint32_t rtcUtc = getRtcUtcEpochCached(false);
  if (rtcUtc > RTC_MIN_VALID_EPOCH) return rtcUtc;
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
  else if (clockState == 3) s = String("RTC DS3231 Backup");
  else if (clockState == 2) s = String("Offline Estimated");
  else s = String("No Time");
  s += String(" / ") + egyptTimeModeText();
  return s;
}

void saveEgyptTimeMode() {
  if (egyptTimeMode > EGYPT_TZ_SUMMER) egyptTimeMode = EGYPT_TZ_AUTO;
  EEPROM.write(EEPROM_TZ_MAGIC_ADDR, EEPROM_TZ_MAGIC);
  EEPROM.write(EEPROM_TZ_MODE_ADDR, egyptTimeMode);
  commitEeprom("eeprom-save");
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
  commitEeprom("eeprom-save");
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
  } else {
    uint32_t rtcUtc = getRtcUtcEpochCached(false);
    if (rtcUtc > RTC_MIN_VALID_EPOCH) {
      offlineClockBaseEpoch = rtcUtc;
      offlineClockBaseMs = millis();
      clockState = 3;
      if (lastOfflineClockSaveMs == 0 || millis() - lastOfflineClockSaveMs >= OFFLINE_CLOCK_SAVE_INTERVAL_MS) {
        saveOfflineClockNow(rtcUtc);
      }
    } else if (offlineClockBaseEpoch > 1700000000UL) {
      clockState = 2;
    } else {
      clockState = 0;
    }
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
  setRelay(STATUS_LED_PIN, on ? 1 : 0);
#endif
}

void saveSmartOutputConfig() {
  EEPROM.write(EEPROM_SMART_MAGIC_ADDR, EEPROM_SMART_MAGIC);
  EEPROM.put(EEPROM_SMART_DATA_ADDR, smartCfg);
  commitEeprom("eeprom-save");
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
    // V1.2.4 Telegram Output Notify Stable:
    // Out3 is force-written every loop for reliability, but Telegram/log are updated
    // only when the logical state really changes.
    bool changed = (desired != smartOutputState);
    String smartSrc = "Output3 Auto";
    if (scheduleOwnsOut3) smartSrc = "Output3 Schedule";
    else if (smartCfg.mode == SMART_MODE_MANUAL_SCHED) smartSrc = "Output3 Manual";
    else if (smartCfg.mode == SMART_MODE_TEMPERATURE) smartSrc = "Output3 Temperature";
    else if (smartCfg.mode == SMART_MODE_HUMIDITY) smartSrc = "Output3 Humidity";
    else if (smartCfg.mode == SMART_MODE_TIMER) smartSrc = "Output3 Timer";
    writeSmartOutput(desired);
    if (changed) rememberOutputCommand(smartSrc.c_str(), 3, desired ? 1 : 0, true);
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
    commitEeprom("eeprom-save");
  }
}

void saveCityWeatherConfig() {
  EEPROM.write(EEPROM_WEATHER_MAGIC_ADDR, EEPROM_WEATHER_MAGIC);
  EEPROM.write(EEPROM_WEATHER_CITY_ADDR, sanitizeWeatherCityIndex(weatherCityIndex));
  commitEeprom("eeprom-save");
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
  http.setTimeout(3000);
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
  html += F("<div class='card'><h1>City Weather</h1><div class='sub'>Manual refresh only to keep ESP-01 web fast. This does not use extra Device pins.</div>");
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
    autoBackupAfterSave("Weather city saved");
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
  if (!isTimeValidNow()) return String("Waiting for Time");
  time_t now = (time_t)getDeviceEpoch();
  struct tm *tmNow = gmtime(&now);
  if (!tmNow) return String("Waiting for Time");
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



String outputAutoDayScheduleRow(byte idx, byte d) {
  String x;
  if (idx >= OUTPUT_AUTO_COUNT) return x;
  OutputAutoConfig &cfg = outputAutoCfg[idx];
  x += F("<div class='dayrow'><label class='small daylbl'><input type='checkbox' name='aen");
  x += String(d);
  x += F("' ");
  if (cfg.dayEnabled[d]) x += F("checked");
  x += F("> ");
  x += dayName(d);
  x += F("</label>");
  for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
    byte shown = sl + 1;
    x += F("<label class='small'><input type='checkbox' name='as");
    x += String(d); x += String(sl);
    x += F("' ");
    if (cfg.slotEnabled[d][sl]) x += F("checked");
    x += F("> State "); x += String(shown); x += F("</label>");
    x += F("<input type='time' name='ast");
    x += String(d); x += String(sl);
    x += F("' value='");
    x += minuteToTime(cfg.dayStartMin[d][sl]);
    x += F("'><input type='time' name='aet");
    x += String(d); x += String(sl);
    x += F("' value='");
    x += minuteToTime(cfg.dayEndMin[d][sl]);
    x += F("'>");
  }
  x += F("</div>");
  return x;
}


String outputAutoCheckboxForceJs(byte idx, byte out) {
  String js;
  js.reserve(3600);
  if (idx >= OUTPUT_AUTO_COUNT) return js;
  OutputAutoConfig &cfg = outputAutoCfg[idx];
  js += F("<script>(function(){");
  js += F("function q(n){return document.querySelector(\"[name='\"+n+\"']\")}function cb(n,v){var e=q(n);if(e)e.checked=!!v}function val(n,v){var e=q(n);if(e)e.value=v}");
  js += F("document.querySelectorAll('form,input,select').forEach(function(e){e.setAttribute('autocomplete','off')});");
  js += F("val('out','"); js += String(out); js += F("');");
  js += F("val('manoff9time','"); js += manualAutoOff9TimeText(); js += F("');");
  js += F("cb('manoff9',"); js += (manualAutoOff9Enabled[idx] ? "1" : "0"); js += F(");");
  js += F("cb('mandur',"); js += (manualAutoOffDurEnabled[idx] ? "1" : "0"); js += F(");");
  js += F("val('mandurmin','"); js += String(manualAutoOffDurMin[idx]); js += F("');");
  js += F("cb('ena',"); js += (cfg.enabled ? "1" : "0"); js += F(");");
  js += F("cb('sche',"); js += (cfg.scheduleEnable ? "1" : "0"); js += F(");");
  js += F("cb('syncTS',"); js += (outputAutoSyncThingSpeak[idx] ? "1" : "0"); js += F(");");
  js += F("val('rep','"); js += (cfg.repeatWeekly ? "1" : "0"); js += F("');");
  for (byte d = 0; d < 7; d++) {
    js += F("cb('aen"); js += String(d); js += F("',"); js += (cfg.dayEnabled[d] ? "1" : "0"); js += F(");");
    for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
      js += F("cb('as"); js += String(d); js += String(sl); js += F("',"); js += (cfg.slotEnabled[d][sl] ? "1" : "0"); js += F(");");
      js += F("val('ast"); js += String(d); js += String(sl); js += F("','"); js += minuteToTime(cfg.dayStartMin[d][sl]); js += F("');");
      js += F("val('aet"); js += String(d); js += String(sl); js += F("','"); js += minuteToTime(cfg.dayEndMin[d][sl]); js += F("');");
    }
  }
  js += F("})();</script>");
  return js;
}

void sendOutputAutoPage() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  byte out = webServer.hasArg("out") ? (byte)webServer.arg("out").toInt() : 1;
  byte idx = outputAutoIndex(out);
  if (idx >= OUTPUT_AUTO_COUNT) { out = 1; idx = outputAutoIndex(1); }
  OutputAutoConfig &cfg = outputAutoCfg[idx];
  uint32_t durRemain = manualAutoOffDurationRemainingSec(idx);
  bool ar = (currentWebLanguage() == TG_LANG_AR);

  String html = htmlHeader(ar ? String("جداول المخارج / الخيارات اليدوية") : String("Output Schedules / Manual Options"));
  html += F("<div class='card'><h1>Automation / Schedules</h1><div class='sub'>Output 3 keeps its own Smart Output page. Outputs 1, 2, 5 and 6 have weekly schedule controls here. Legacy runtime timers are hidden/disabled; use Manual Auto-Off Timer instead.</div>");
  html += F("<div class='row'><span class='k'>Device Date / Time</span><span class='v'>"); html += currentDeviceDateTimeText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Schedule Day</span><span class='v'>"); html += currentDeviceDayText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Manual Auto-OFF Time</span><span class='v'>"); html += manualAutoOff9TimeText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Manual Auto-OFF Enabled</span><span class='v'>"); html += String(manualAutoOff9EnabledCount()); html += F(" / "); html += String(OUTPUT_AUTO_COUNT + 1); html += F(" outputs enabled</span></div>");
  html += F("<div class='row'><span class='k'>Last Auto-OFF Run</span><span class='v'>"); html += ymdKeyText(manualAutoOff9LastYmd); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Manual Options Storage</span><span class='v'>EEPROM primary + SPIFFS rescue backup</span></div>");
  html += F("<div class='row'><span class='k'>Storage Health</span><span class='v'>"); html += eepromStorageStatusText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Selected Output Memory</span><span class='v'>Field "); html += String(out); html += F(" / index "); html += String(idx); html += F(" / Daily="); html += (manualAutoOff9Enabled[idx] ? F("ON") : F("OFF")); html += F(" / Timer="); html += (manualAutoOffDurEnabled[idx] ? F("ON") : F("OFF")); html += F(" / Minutes="); html += String(manualAutoOffDurMin[idx]); html += F("</span></div>");
  if (webServer.hasArg("saved")) { html += F("<div class='msg ok'>Saved. Check Storage Health: Attempts must increase and Saved=YES. If LastCommit fails, LastFsSave=OK keeps settings safe.</div>"); }
  html += F("<form method='GET' action='/outputauto' autocomplete='off'><label>Select Output</label><select name='out' autocomplete='off' onchange='this.form.submit()'>");
  for (byte i = 0; i < OUTPUT_AUTO_COUNT; i++) {
    byte f = OUTPUT_AUTO_FIELDS[i];
    html += F("<option value='"); html += String(f); html += F("'"); if (f == out) html += F(" selected"); html += F(">"); html += fieldLabelHtml(f); html += F(" / Field "); html += String(f); html += F("</option>");
  }
  html += F("</select>"); html += sessionHiddenInput(); html += F("</form>");
  html += F("<div class='row'><span class='k'>Current State</span><span class='v' id='autoState'>"); html += outputStateText(outputStateByNumber(out)); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Manual Timer Remaining</span><span class='v' id='autoRemain'>"); html += formatHMS(durRemain); html += F("</span></div>");
  html += F("</div>");

  html += F("<div class='card'><h1>"); html += fieldLabelHtml(out); html += F("</h1><form method='POST' action='/saveoutauto' autocomplete='off'>"); html += sessionHiddenInput();
  html += F("<input type='hidden' name='out' value='"); html += String(out); html += F("'>");
  html += F("<div class='syncbox'><h1>Manual Options</h1><label class='small'>Manual Auto-OFF Time</label><input type='time' name='manoff9time' value='"); html += manualAutoOff9TimeText(); html += F("'>");
  html += F("<label><input type='checkbox' name='manoff9' value='1' "); if (manualAutoOff9Enabled[idx]) html += F("checked"); html += F("> Close manual ON at selected daily time</label>");
  html += F("<label><input type='checkbox' name='mandur' value='1' "); if (manualAutoOffDurEnabled[idx]) html += F("checked"); html += F("> Close after manual ON duration</label>");
  html += F("<label class='small'>Manual Auto-Off Timer duration (minutes)</label><input type='number' min='1' max='1440' step='1' name='mandurmin' value='"); html += String(manualAutoOffDurMin[idx]); html += F("'>");
  html += F("<div class='sub'>Manual options only apply when this output is turned ON manually from Web, Telegram, ThingSpeak, MQTT, Button or Timed Override. Weekly schedules are not changed.</div></div>");
  html += F("<h1>Schedule Automation</h1>");
  html += F("<label><input type='checkbox' name='ena' value='1' "); if (cfg.enabled) html += F("checked"); html += F("> Enable automation for this output</label>");
  html += F("<label><input type='checkbox' name='sche' value='1' "); if (cfg.scheduleEnable) html += F("checked"); html += F("> Enable weekly schedule</label>");
  html += F("<label class='small'>Repeat</label><select name='rep'><option value='1'"); if(cfg.repeatWeekly) html += F(" selected"); html += F(">Weekly Repeat</option><option value='0'"); if(!cfg.repeatWeekly) html += F(" selected"); html += F(">Once only</option></select>");
  html += F("<div class='syncbox'><label><input type='checkbox' name='syncTS' value='1' "); if (outputAutoSyncThingSpeak[idx]) html += F("checked"); html += F("> Automation Sync to ThingSpeak</label><div class='sub'>OFF = schedule changes only update the real output, WebSocket, Log and Telegram. ON = schedule changes also create ThingSpeak pending sync when internet is available.</div></div>");
  html += F("<div class='row'><span class='k'>Current Automation Sync</span><span class='v'>"); html += outputAutoSyncLabel(idx); html += F("</span></div>");
  html += F("<div class='sub'>Enable the day, then enable State 1 and/or State 2. ON at start time, OFF exactly at end time. Overnight periods like 22:00 to 02:00 are supported.</div>");
  html += F("<div class='dayhdr small'><b>Day</b><b>State</b><b>ON</b><b>OFF</b></div>");
  html += outputAutoDayScheduleRow(idx, 6); html += outputAutoDayScheduleRow(idx, 0); html += outputAutoDayScheduleRow(idx, 1); html += outputAutoDayScheduleRow(idx, 2); html += outputAutoDayScheduleRow(idx, 3); html += outputAutoDayScheduleRow(idx, 4); html += outputAutoDayScheduleRow(idx, 5);
  html += F("<button class='btn' type='submit' name='save' value='1'>Save Schedule / Manual Options</button>");
  html += F("</form></div>");
  html += outputAutoCheckboxForceJs(idx, out);
  html += F("<script>const autoOut="); html += String(out); html += F(",LON='"); html += ar ? F("يعمل") : F("ON"); html += F("',LOFF='"); html += ar ? F("متوقف") : F("OFF"); html += F("';function autoPaint(d){let s=d['o'+autoOut];let st=document.getElementById('autoState');if(st){st.innerHTML=s?LON:LOFF;st.className='v '+(s?'on':'off')}let r=document.getElementById('autoRemain');if(r){r.innerHTML=d['o'+autoOut+'AutoRemain']||'00:00:00'}let dt=document.getElementById('devtime');if(dt)dt.innerHTML=d.deviceTime||'--';let dd=document.getElementById('devday');if(dd)dd.innerHTML=d.deviceDay||'--'}function autoRefresh(){fetch('/status?sid="); html += webSessionToken; html += F("').then(r=>r.json()).then(autoPaint).catch(e=>{})}setInterval(autoRefresh,1000);autoRefresh();</script>");
  html += htmlFooter();
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.sendHeader("Expires", "0");
  webServer.send(200, "text/html", html);
}

void handleSaveOutputAuto() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  byte out = (byte)webServer.arg("out").toInt();
  byte idx = outputAutoIndex(out);
  if (idx >= OUTPUT_AUTO_COUNT) {
    webServer.send(400, "text/plain", "bad output");
    return;
  }
  OutputAutoConfig &cfg = outputAutoCfg[idx];
  if (webServer.hasArg("manoff9time")) manualAutoOff9Minute = parseTimeToMinutes(webServer.arg("manoff9time"), manualAutoOff9Minute);
  manualAutoOff9Enabled[idx] = webServer.hasArg("manoff9") ? 1 : 0;
  manualAutoOffDurEnabled[idx] = webServer.hasArg("mandur") ? 1 : 0;
  if (webServer.hasArg("mandurmin")) manualAutoOffDurMin[idx] = clampManualAutoOffDurationMin(webServer.arg("mandurmin").toInt());
  if (!manualAutoOffDurEnabled[idx]) cancelManualAutoOffDurationForOutput(out);
  else if (outputLastManualOn[idx] && outputStateByNumber(out) == 1 && manualAutoOffDurationRemainingSec(idx) == 0) startManualAutoOffDurationForOutput(out);
  cfg.enabled = webServer.hasArg("ena") ? 1 : 0;
  cfg.scheduleEnable = webServer.hasArg("sche") ? 1 : 0;
  if (webServer.hasArg("rep")) cfg.repeatWeekly = webServer.arg("rep").toInt() ? 1 : 0;
  outputAutoSyncThingSpeak[idx] = webServer.hasArg("syncTS") ? 1 : 0;
  for (byte d = 0; d < 7; d++) {
    cfg.dayEnabled[d] = webServer.hasArg(String("aen") + d) ? 1 : 0;
    for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
      String si = String(d) + String(sl);
      cfg.slotEnabled[d][sl] = webServer.hasArg(String("as") + si) ? 1 : 0;
      cfg.dayStartMin[d][sl] = parseTimeToMinutes(webServer.arg(String("ast") + si), cfg.dayStartMin[d][sl]);
      cfg.dayEndMin[d][sl] = parseTimeToMinutes(webServer.arg(String("aet") + si), cfg.dayEndMin[d][sl]);
    }
  }
#if ENABLE_LEGACY_OUTPUT_TIMERS
  uint32_t th = constrain(webServer.arg("th").toInt(), 0, 23);
  uint32_t tm = constrain(webServer.arg("tm").toInt(), 0, 59);
  uint32_t ts = constrain(webServer.arg("ts").toInt(), 0, 59);
  cfg.timerDurationSec = clampTimerSeconds(th * 3600UL + tm * 60UL + ts);
  if (webServer.hasArg("starttimer")) cfg.enabled = 1;
#endif
  saveOutputAutoConfigs();
  saveOutputAutoSyncConfig();
  saveManualAutoOff9Config();
  saveManualAutoOffDurationConfig();
  autoBackupAfterSave("Output automation saved");
  addEsp32Log(String("Manual options saved: field ") + String(out) + String(" daily=") + String(manualAutoOff9Enabled[idx]) + String(" timer=") + String(manualAutoOffDurEnabled[idx]) + String(" min=") + String(manualAutoOffDurMin[idx]));
#if ENABLE_LEGACY_OUTPUT_TIMERS
  if (webServer.hasArg("starttimer")) startOutputAutoTimer(idx);
  if (webServer.hasArg("stoptimer")) stopOutputAutoTimer(idx, true);
#else
  outputAutoTimerActive[idx] = false;
#endif
  addEsp32Log(String("Output automation saved for ") + fieldLabel(out));
  wsBroadcastStatus();
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Location", sessionUrl(String("/outputauto?out=") + String(out) + String("&saved=1")), true);
  webServer.send(302, "text/plain", "");
}

String smartOutputCardHtml() {
  String html;
  html.reserve(6200);
  // Timer duration is runtime-only. After restart it is not restored from EEPROM.
  uint32_t dur = smartTimerActive ? smartTimerDurationSec : 0UL;
  byte th = dur / 3600UL;
  byte tm = (dur % 3600UL) / 60UL;
  byte ts = dur % 60UL;

  html += F("<div class='card'><h1>Output 3 Smart</h1><div class='sub'>GPIO25 active LOW. Output 3 is independent from the Internet LED on GPIO18. Select one mode only.</div>");
  html += F("<div class='row'><span class='k'>Current Mode</span><span class='v'>"); html += smartModeText(smartCfg.mode); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Output 3</span><span class='v'><span id='o3' class='"); html += smartOutputState ? F("on") : F("off"); html += F("'>"); html += smartOutputState ? F("ON") : F("OFF"); html += F("</span></span></div>");
  html += F("<div class='row'><span class='k'>Clock</span><span class='v' id='ntpstate'>"); html += clockStateText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>RTC DS3231</span><span class='v' id='rtcstate'>"); html += rtcStatusText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Time Zone</span><span class='v'>"); html += egyptTimeModeText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Device Date / Time</span><span class='v' id='devtime'>"); html += currentDeviceDateTimeText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Schedule Day</span><span class='v' id='devday'>"); html += currentDeviceDayText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Timer Remaining</span><span class='v' id='tremain'>"); html += formatHMS(smartTimerRemainingSec()); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Manual Timer Remaining</span><span class='v'>"); html += formatHMS(manualAutoOffDurationOut3RemainingSec()); html += F("</span></div>");

  html += F("<form method='POST' action='/savesmart'>");
  html += sessionHiddenInput();
  html += F("<div class='syncbox'><h1>Manual Options</h1><label><input type='checkbox' name='manoff9o3' value='1' "); if (manualAutoOff9Out3Enabled) html += F("checked"); html += F("> Close Output 3 manual ON at selected Auto-OFF time: "); html += manualAutoOff9TimeText(); html += F("</label>");
  html += F("<label><input type='checkbox' name='manduro3' value='1' "); if (manualAutoOffDurOut3Enabled) html += F("checked"); html += F("> Close Output 3 after manual ON duration</label>");
  html += F("<label class='small'>Output 3 Manual Auto-Off duration (minutes)</label><input type='number' min='1' max='1440' step='1' name='manduro3min' value='"); html += String(manualAutoOffDurOut3Min); html += F("'>");
  html += F("<div class='sub'>These options apply only when Output 3 is in Manual / Schedule mode and the weekly schedule is OFF. They do not override timer, schedule, temperature or humidity modes.</div></div>");
  html += F("<div><label class='small'>Egypt Time</label><select name='tzmode'><option value='0'"); if(egyptTimeMode==EGYPT_TZ_AUTO) html+=F(" selected"); html+=F(">Auto Egypt DST</option><option value='1'"); if(egyptTimeMode==EGYPT_TZ_WINTER) html+=F(" selected"); html+=F(">Winter UTC+2</option><option value='2'"); if(egyptTimeMode==EGYPT_TZ_SUMMER) html+=F(" selected"); html+=F(">Summer UTC+3</option></select></div>");
  html += F("<div class='sub'>DST applies to NTP, DS3231 RTC, Offline Clock, Dashboard, and Schedule. Manual override is available if the official rule changes.</div>");
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
  manualAutoOff9Out3Enabled = webServer.hasArg("manoff9o3") ? 1 : 0;
  manualAutoOffDurOut3Enabled = webServer.hasArg("manduro3") ? 1 : 0;
  if (webServer.hasArg("manduro3min")) manualAutoOffDurOut3Min = clampManualAutoOffDurationMin(webServer.arg("manduro3min").toInt());
  if (!manualAutoOffDurOut3Enabled) cancelManualAutoOffDurationForOut3();
  saveManualAutoOff9Config();
  saveManualAutoOffDurationConfig();
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
  if (smartCfg.mode == SMART_MODE_MANUAL_SCHED && !smartCfg.scheduleEnable) recordSmartOutput3ManualControl(smartCfg.manualState);
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
  autoBackupAfterSave("Output 3 smart settings saved");
  applySmartOutputControl();
  webServer.sendHeader("Location", "/");
  webServer.send(303, "text/plain", "Saved");
}

String outputControlCardHtml() {
  String html;
  html.reserve(4200);
  html += F("<div class='card'><h1>Control what you wish</h1><div class='sub'>Admin can choose exactly which fields each user can see/control. Inputs 7/8 are uploaded to ThingSpeak. Inputs 9/10 are local only and are sent to WebSocket/Telegram.</div>");
  if (!userHasAnyVisibleIoField()) {
    html += F("<div class='msg warn'>No fields are visible for this user. Ask Admin to enable field permissions.</div></div>");
    return html;
  }
  html += F("<div class='oc'>");

  if (canCurrentUserSeeField(1)) {
    html += F("<div class='ocbox'><div class='octop'><div><b>"); html += fieldLabelHtml(1); html += F("</b><div class='small'>Output 1 / GPIO26</div></div><div><span id='o1' class='bigstate "); html += outputClass(currentOutput1State); html += F("'>"); html += outputStateText(currentOutput1State); html += F("</span><span id='d1' class='dot "); html += outputClass(currentOutput1State); html += F("'></span></div></div>");
    if (canCurrentUserControlField(1)) html += F("<button id='b1' class='btn' onclick='toggleOut(1)'>...</button>"); else html += F("<div class='sub'>View only</div>");
    html += F("</div>");
  }

  if (canCurrentUserSeeField(2)) {
    html += F("<div class='ocbox'><div class='octop'><div><b>"); html += fieldLabelHtml(2); html += F("</b><div class='small'>Output 2 / GPIO27</div></div><div><span id='o2' class='bigstate "); html += outputClass(currentOutput2State); html += F("'>"); html += outputStateText(currentOutput2State); html += F("</span><span id='d2' class='dot "); html += outputClass(currentOutput2State); html += F("'></span></div></div>");
    if (canCurrentUserControlField(2)) html += F("<button id='b2' class='btn' onclick='toggleOut(2)'>...</button>"); else html += F("<div class='sub'>View only</div>");
    html += F("</div>");
  }

  if (canCurrentUserSeeField(5)) {
    html += F("<div class='ocbox'><div class='octop'><div><b>"); html += fieldLabelHtml(5); html += F("</b><div class='small'>Output Field 5 / GPIO14</div></div><div><span id='o5' class='bigstate "); html += outputClass(currentOutput5State); html += F("'>"); html += outputStateText(currentOutput5State); html += F("</span><span id='d5' class='dot "); html += outputClass(currentOutput5State); html += F("'></span></div></div>");
    if (canCurrentUserControlField(5)) html += F("<button id='b5' class='btn' onclick='toggleOut(5)'>...</button>"); else html += F("<div class='sub'>View only</div>");
    html += F("</div>");
  }

  if (canCurrentUserSeeField(6)) {
    html += F("<div class='ocbox'><div class='octop'><div><b>"); html += fieldLabelHtml(6); html += F("</b><div class='small'>Output Field 6 / GPIO13</div></div><div><span id='o6' class='bigstate "); html += outputClass(currentOutput6State); html += F("'>"); html += outputStateText(currentOutput6State); html += F("</span><span id='d6' class='dot "); html += outputClass(currentOutput6State); html += F("'></span></div></div>");
    if (canCurrentUserControlField(6)) html += F("<button id='b6' class='btn' onclick='toggleOut(6)'>...</button>"); else html += F("<div class='sub'>View only</div>");
    html += F("</div>");
  }

  html += F("</div>");
  if (canCurrentUserSeeField(7)) { html += F("<div class='row'><span class='k'>"); html += fieldLabelHtml(7); html += F("</span><span class='v'><span id='i7'>"); html += readInput7State() ? F("ON") : F("OFF"); html += F("</span> / GPIO32</span></div>"); }
  if (canCurrentUserSeeField(8)) { html += F("<div class='row'><span class='k'>"); html += fieldLabelHtml(8); html += F("</span><span class='v'><span id='i8'>"); html += readInput8State() ? F("ON") : F("OFF"); html += F("</span> / GPIO33</span></div>"); }
  if (canCurrentUserSeeField(9)) { html += F("<div class='row'><span class='k'>"); html += fieldLabelHtml(9); html += F("</span><span class='v'><span id='i9'>"); html += readInput9State() ? F("ON") : F("OFF"); html += F("</span> / GPIO16</span></div>"); }
  if (canCurrentUserSeeField(10)) { html += F("<div class='row'><span class='k'>"); html += fieldLabelHtml(10); html += F("</span><span class='v'><span id='i10'>"); html += readInput10State() ? F("ON") : F("OFF"); html += F("</span> / GPIO17</span></div>"); }
  html += F("<div class='row'><span class='k'>ThingSpeak Sync</span><span class='v'><span id='pend' class='pill "); html += pendingOutputThingSpeakUpdate ? F("warn") : F("ok"); html += F("'>"); html += pendingOutputThingSpeakUpdate ? F("Pending") : F("Synced"); html += F("</span></span></div>");
  html += F("<div class='row'><span class='k'>Sensor</span><span class='v'><span id='dhttype'>"); html += dhtTypeText(); html += F("</span></span></div>");
  html += F("<div class='row'><span class='k'>Temperature</span><span class='v'><span id='temp'>--</span> &deg;C</span></div>");
  html += F("<div class='row'><span class='k'>Humidity</span><span class='v'><span id='hum'>--</span> %</span></div>");
  html += F("<div class='msg' id='outmsg'>Ready. Ultra-lite status refresh every 5 seconds. No full page reload.</div></div>");
  return html;
}

String outputControlJs() {
  bool ar = (currentWebLanguage() == TG_LANG_AR);
  String js;
  js.reserve(2600);
  js += F("var outState={1:0,2:0,5:0,6:0};");
  js += F("var L_ON='"); js += ar ? F("يعمل") : F("ON"); js += F("',L_OFF='"); js += ar ? F("متوقف") : F("OFF"); js += F("',L_VIEW='"); js += ar ? F("عرض فقط") : F("VIEW ONLY"); js += F("',L_PENDING='"); js += ar ? F("قيد المزامنة") : F("Pending"); js += F("',L_SYNCED='"); js += ar ? F("تمت المزامنة") : F("Synced"); js += F("';");
  js += F("var L_TON='"); js += ar ? F("تشغيل ") : F("TURN ON "); js += F("',L_TOFF='"); js += ar ? F("إيقاف ") : F("TURN OFF "); js += F("',L_SEND='"); js += ar ? F("جاري الإرسال...") : F("Sending..."); js += F("',L_APPLIED='"); js += ar ? F("تم التنفيذ. حالة ThingSpeak قيد المزامنة/تمت.") : F("Applied. Full ThingSpeak state pending/synced."); js += F("',L_NOCHANGE='"); js += ar ? F("لا يوجد تغيير.") : F("No change."); js += F("',L_FAIL='"); js += ar ? F("فشل أمر المخرج/الجلسة") : F("Output command failed/session"); js += F("';");
  js += F("var outLabels={1:\""); js += jsonEscape(fieldLabel(1)); js += F("\",2:\""); js += jsonEscape(fieldLabel(2)); js += F("\",5:\""); js += jsonEscape(fieldLabel(5)); js += F("\",6:\""); js += jsonEscape(fieldLabel(6)); js += F("\"};");
  js += F("function outName(i){return outLabels[i]||('Output '+i)}function paintOne(i,v){outState[i]=v?1:0;let e=document.getElementById('o'+i),d=document.getElementById('d'+i),b=document.getElementById('b'+i);if(e){e.innerHTML=v?L_ON:L_OFF;e.className='bigstate '+(v?'on':'off')}if(d){d.className='dot '+(v?'on':'off')}if(b){b.innerHTML=v?(L_TOFF+outName(i)):(L_TON+outName(i));b.className='btn '+(v?'offbtn':'onbtn')}}");
  js += F("function paintOut(d){if(d.l1)outLabels[1]=d.l1;if(d.l2)outLabels[2]=d.l2;if(d.l5)outLabels[5]=d.l5;if(d.l6)outLabels[6]=d.l6;paintOne(1,d.o1);paintOne(2,d.o2);paintOne(5,d.o5);paintOne(6,d.o6);[1,2,5,6].forEach(function(x){let b=document.getElementById('b'+x);if(b&&d['c'+x]===false){b.disabled=true;b.innerHTML=L_VIEW;b.className='btn btn2';}});let i7=document.getElementById('i7');if(i7)i7.innerHTML=d.i7?L_ON:L_OFF;let i8=document.getElementById('i8');if(i8)i8.innerHTML=d.i8?L_ON:L_OFF;let i9=document.getElementById('i9');if(i9)i9.innerHTML=d.i9?L_ON:L_OFF;let i10=document.getElementById('i10');if(i10)i10.innerHTML=d.i10?L_ON:L_OFF;let p=document.getElementById('pend');if(p){p.innerHTML=d.pending?L_PENDING:L_SYNCED;p.className='pill '+(d.pending?'warn':'ok')}let t=document.getElementById('temp');if(t)t.innerHTML=(d.temp===null||d.temp===undefined)?'--':d.temp;let h=document.getElementById('hum');if(h)h.innerHTML=(d.hum===null||d.hum===undefined)?'--':d.hum;let dt=document.getElementById('dhttype');if(dt)dt.innerHTML=d.dht||'--';let ns=document.getElementById('ntpstate');if(ns)ns.innerHTML=d.clockState||'--';let rs=document.getElementById('rtcstate');if(rs)rs.innerHTML=d.rtcStatus||'--';let ds=document.getElementById('devtime');if(ds)ds.innerHTML=d.deviceTime||'--';let dd=document.getElementById('devday');if(dd)dd.innerHTML=d.deviceDay||'--';let tr=document.getElementById('tremain');if(tr)tr.innerHTML=d.timerRemain||'00:00:00';let oc=document.getElementById('owcity');if(oc)oc.innerHTML=d.city||'--';let ot=document.getElementById('owtemp');if(ot)ot.innerHTML=(d.owTemp===null||d.owTemp===undefined)?'--':d.owTemp;let oh=document.getElementById('owhum');if(oh)oh.innerHTML=(d.owHum===null||d.owHum===undefined)?'--':d.owHum;let os=document.getElementById('owstatus');if(os)os.innerHTML=d.owStatus||'--';let ou=document.getElementById('owtime');if(ou)ou.innerHTML=d.owTime||'--'}");
  js += F("function refreshOut(){fetch('/status?sid="); js += webSessionToken; js += F("').then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(paintOut).catch(e=>{})}");
  js += F("function setOut(o,s){let m=document.getElementById('outmsg');if(m)m.innerHTML=L_SEND;fetch('/setoutput',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'out='+o+'&state='+s+'&sid="); js += webSessionToken; js += F("'}).then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(d=>{paintOut(d);if(m)m.innerHTML=d.changed?L_APPLIED:L_NOCHANGE;}).catch(e=>{if(m)m.innerHTML='<span class=bad>'+L_FAIL+'</span>'})}");
  js += F("function toggleOut(o){setOut(o,outState[o]?0:1)}");
  js += F("setInterval(refreshOut,5000);refreshOut();");
  return js;
}

String outputStatusJson(bool changed, bool filterForCurrentUser) {
  bool show1 = !filterForCurrentUser || canCurrentUserSeeField(1);
  bool show2 = !filterForCurrentUser || canCurrentUserSeeField(2);
  bool show5 = !filterForCurrentUser || canCurrentUserSeeField(5);
  bool show6 = !filterForCurrentUser || canCurrentUserSeeField(6);
  bool show7 = !filterForCurrentUser || canCurrentUserSeeField(7);
  bool show8 = !filterForCurrentUser || canCurrentUserSeeField(8);
  bool show9 = !filterForCurrentUser || canCurrentUserSeeField(9);
  bool show10 = !filterForCurrentUser || canCurrentUserSeeField(10);
  String json = F("{\"o1\":");
  json += show1 ? String(currentOutput1State) : String("null");
  json += F(",\"o2\":");
  json += show2 ? String(currentOutput2State) : String("null");
  json += F(",\"o5\":");
  json += show5 ? String(currentOutput5State) : String("null");
  json += F(",\"o6\":");
  json += show6 ? String(currentOutput6State) : String("null");
  json += F(",\"i7\":");
  json += show7 ? String(readInput7State()) : String("null");
  json += F(",\"i8\":");
  json += show8 ? String(readInput8State()) : String("null");
  json += F(",\"i9\":");
  json += show9 ? String(readInput9State()) : String("null");
  json += F(",\"i10\":");
  json += show10 ? String(readInput10State()) : String("null");
  json += F(",\"l1\":\""); if (show1) json += jsonEscape(fieldLabel(1)); json += F("\"");
  json += F(",\"l2\":\""); if (show2) json += jsonEscape(fieldLabel(2)); json += F("\"");
  json += F(",\"l3\":\""); json += jsonEscape(fieldLabel(3)); json += F("\"");
  json += F(",\"l5\":\""); if (show5) json += jsonEscape(fieldLabel(5)); json += F("\"");
  json += F(",\"l6\":\""); if (show6) json += jsonEscape(fieldLabel(6)); json += F("\"");
  json += F(",\"l7\":\""); if (show7) json += jsonEscape(fieldLabel(7)); json += F("\"");
  json += F(",\"l8\":\""); if (show8) json += jsonEscape(fieldLabel(8)); json += F("\"");
  json += F(",\"l9\":\""); if (show9) json += jsonEscape(fieldLabel(9)); json += F("\"");
  json += F(",\"l10\":\""); if (show10) json += jsonEscape(fieldLabel(10)); json += F("\"");
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
  json += jsonEscape(clockStateText());
  json += F("\"");
  json += F(",\"rtcStatus\":\"");
  json += jsonEscape(rtcStatusText());
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
  json += F(",\"field5Output\":");
  json += show5 ? String(currentOutput5State) : String("null");
  json += F(",\"field6Output\":");
  json += show6 ? String(currentOutput6State) : String("null");
  json += F(",\"o1AutoRemain\":\""); json += formatHMS(manualAutoOffDurationRemainingSec(outputAutoIndex(1))); json += F("\"");
  json += F(",\"o2AutoRemain\":\""); json += formatHMS(manualAutoOffDurationRemainingSec(outputAutoIndex(2))); json += F("\"");
  json += F(",\"o5AutoRemain\":\""); json += formatHMS(manualAutoOffDurationRemainingSec(outputAutoIndex(5))); json += F("\"");
  json += F(",\"o6AutoRemain\":\""); json += formatHMS(manualAutoOffDurationRemainingSec(outputAutoIndex(6))); json += F("\"");
  json += F(",\"o1AutoActive\":"); json += (manualAutoOffDurationRemainingSec(outputAutoIndex(1)) > 0) ? F("true") : F("false");
  json += F(",\"o2AutoActive\":"); json += (manualAutoOffDurationRemainingSec(outputAutoIndex(2)) > 0) ? F("true") : F("false");
  json += F(",\"o5AutoActive\":"); json += (manualAutoOffDurationRemainingSec(outputAutoIndex(5)) > 0) ? F("true") : F("false");
  json += F(",\"o6AutoActive\":"); json += (manualAutoOffDurationRemainingSec(outputAutoIndex(6)) > 0) ? F("true") : F("false");
  json += F(",\"duckdns\":\"");
  json += lastDuckDNSMessage;
  json += F("\"");
  json += F(",\"role\":\"");
  json += webIsAdmin() ? F("admin") : F("user");
  json += F("\"");
  json += F(",\"c1\":"); json += canCurrentUserControlField(1) ? F("true") : F("false");
  json += F(",\"c2\":"); json += canCurrentUserControlField(2) ? F("true") : F("false");
  json += F(",\"c5\":"); json += canCurrentUserControlField(5) ? F("true") : F("false");
  json += F(",\"c6\":"); json += canCurrentUserControlField(6) ? F("true") : F("false");
  json += F(",\"wifiOnline\":");
  json += isStaOnline() ? F("true") : F("false");
  json += F(",\"lastCmd\":\"");
  json += jsonEscape(String(lastCommandText));
  json += F("\"");
  json += F(",\"lastCmdTime\":\"");
  json += jsonEscape(String(lastCommandTime));
  json += F("\"");
  json += F(",\"lastCmdUser\":\"");
  json += jsonEscape(String(lastCommandUser));
  json += F("\"");
  json += F(",\"lastCmdSource\":\"");
  json += jsonEscape(String(lastCommandSource));
  json += F("\"");
  json += F(",\"lastCmdMs\":");
  json += lastCommandMs;
  json += F(",\"networkState\":\"");
  json += networkStatusText();
  json += F("\"");
  json += F("}");
  return json;
}

bool setOutputStateInternal(byte outputNumber, byte newState, const char* source, bool saveToEeprom, bool syncThingSpeak) {
  if (!isValidState(newState)) return false;
  bool changed = false;

  if (outputNumber == 1) {
    if (currentOutput1State != newState) {
      currentOutput1State = newState;
      setRelay(OUTPUT1_PIN, currentOutput1State);
      if (saveToEeprom) saveOutputState(1, currentOutput1State);
      changed = true;
    }
  } else if (outputNumber == 2) {
    if (currentOutput2State != newState) {
      currentOutput2State = newState;
      setRelay(OUTPUT2_PIN, currentOutput2State);
      if (saveToEeprom) saveOutputState(2, currentOutput2State);
      changed = true;
    }
  } else if (outputNumber == 5) {
    if (currentOutput5State != newState) {
      currentOutput5State = newState;
      setRelay(OUTPUT5_PIN, currentOutput5State);
      if (saveToEeprom) saveOutputState(5, currentOutput5State);
      changed = true;
    }
  } else if (outputNumber == 6) {
    if (currentOutput6State != newState) {
      currentOutput6State = newState;
      setRelay(OUTPUT6_PIN, currentOutput6State);
      if (saveToEeprom) saveOutputState(6, currentOutput6State);
      changed = true;
    }
  } else {
    return false;
  }

  if (saveToEeprom) recordOutputManualControl(outputNumber, newState);
  rememberOutputCommand(source, outputNumber, newState, changed);
  if (changed && syncThingSpeak) {
    markOutputThingSpeakUpdatePending(saveToEeprom ? "Web output manual control" : "Output automation runtime control");
  }
  wsBroadcastStatus();
  return changed;
}

bool setOutputFromWeb(byte outputNumber, byte newState, const char* source) {
  // Manual/Web/MQTT/Telegram/ThingSpeak controls are saved as the last output state.
  return setOutputStateInternal(outputNumber, newState, source, true, true);
}

bool setOutputFromAutomation(byte outputNumber, byte newState, const char* source) {
  // Timer/Schedule controls are runtime-only. They update the real output, WebSocket and dashboard,
  // but they do NOT overwrite the saved manual output state in EEPROM.
  // ThingSpeak sync is now optional per output from the Automation page.
  byte idx = outputAutoIndex(outputNumber);
  bool syncTS = (idx < OUTPUT_AUTO_COUNT) && (outputAutoSyncThingSpeak[idx] == 1);
  return setOutputStateInternal(outputNumber, newState, source, false, syncTS);
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
  if ((out != 1 && out != 2 && out != 5 && out != 6) || !isValidState(state)) {
    webServer.send(400, "application/json", F("{\"error\":\"bad request\"}"));
    return;
  }
  if (!canCurrentUserControlField(out)) {
    webServer.send(403, "application/json", F("{\"error\":\"field denied\"}"));
    return;
  }
  String src = String("Web(") + currentWebUserName() + ")";
  bool changed = setOutputFromWeb(out, state, src.c_str());
  webServer.send(200, "application/json", outputStatusJson(changed));
}


void sendHomePage() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  bool canEngineer = roleAtLeast(WEB_ROLE_ENGINEER);
  bool admin = webIsAdmin();
  if (webServer.hasArg("langset")) {
    byte forcedLang = webLanguageFromCode(webServer.arg("langset"), currentWebLanguage());
    webSetLanguageForUserIndex(currentWebUserIndex, forcedLang);
  }
  byte webLang = currentWebLanguage();
  bool ar = (webLang == TG_LANG_AR);

  // V1.3.8: Simpler Home UI for Operator/Viewer.
  // Technical network/AP/watchdog rows are hidden from normal users.
  // Home control uses dashboard-style switches and status cards filtered by permissions.
  String html = htmlHeader(ar ? String("الرئيسية") : String("Device Control Home"));
  html.reserve(9800);

  if (ar) html += F("<div dir='rtl' style='text-align:right'>");

  html += F("<div class='card'><h1>AHMED MONEM SYSTEM</h1><div class='sub'>");
  html += ar ? F("صفحة تحكم بسيطة. تصميم أحمد منعم / +201004608852.") : F("Simple control home. Design By Ahmed Monem / +201004608852.");
  html += F("</div>");
  html += F("<div class='row'><span class='k'>"); html += ar ? F("المستخدم") : F("User"); html += F("</span><span class='v'>"); html += currentWebUserName(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>"); html += ar ? F("الصلاحية") : F("Role"); html += F("</span><span class='v'>"); html += roleNameForLang(currentWebRole, ar); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>"); html += ar ? F("لغة الواجهة") : F("Interface Language"); html += F("</span><span class='v'>"); html += ar ? F("العربية") : F("English"); html += F("</span></div>");
  if (admin) { html += F("<a class='btn btn2' href='"); html += sessionUrl("/esp32"); html += F("'>ESP32 Realtime / MQTT / Telegram</a>"); }
  if (canEngineer) { html += F("<a class='btn btn2' href='"); html += sessionUrl("/outputauto"); html += F("'>"); html += ar ? F("إعدادات المخارج والجداول") : F("Output Schedules / Manual Options"); html += F("</a>"); }
  if (mustChangeAdminPassword()) html += ar ? F("<div class='card bad'><b>تنبيه أمان:</b> غيّر كلمة مرور admin/admin الافتراضية من إدارة المستخدمين.</div>") : F("<div class='card bad'><b>Security:</b> change default admin/admin from User Management.</div>");
  html += F("</div>");

  html += webLanguageCardHtml();
  html += telegramBotLinkHomeCardHtml();

  html += F("<div class='card sensorcard'><h1>"); html += ar ? F("درجة الحرارة والرطوبة") : F("Temperature & Humidity"); html += F("</h1><div class='sub'>"); html += ar ? F("قراءات الحساس الحالية") : F("Live sensor readings"); html += F("</div><div class='sensegrid'>");
  html += F("<div class='senseitem'><div class='senseicon'>🌡️</div><div class='senselabel'>"); html += ar ? F("درجة الحرارة") : F("Temperature"); html += F("</div><div class='senseval tempc'><span id='temp'>--</span><small> °C</small></div></div>");
  html += F("<div class='senseitem'><div class='senseicon'>💧</div><div class='senselabel'>"); html += ar ? F("الرطوبة") : F("Humidity"); html += F("</div><div class='senseval humc'><span id='hum'>--</span><small> %</small></div></div>");
  html += F("</div></div>");

  if (userHasAnyVisibleIoField()) {
    html += F("<div class='card'><h1>"); html += ar ? F("الحالات المصرح بها") : F("Allowed Status"); html += F("</h1><div class='sub'>"); html += ar ? F("يتم عرض المخارج والمداخل المسموح بها لهذا المستخدم فقط.") : F("Only the outputs and inputs allowed for this user are shown here."); html += F("</div><div class='statuslist'>");
    if (canCurrentUserSeeField(1)) { html += F("<div class='statusrow2'><span class='stname'>"); html += fieldLabelHtml(1); html += F("</span><span id='s1' class='statepill offpill'>--</span></div>"); }
    if (canCurrentUserSeeField(2)) { html += F("<div class='statusrow2'><span class='stname'>"); html += fieldLabelHtml(2); html += F("</span><span id='s2' class='statepill offpill'>--</span></div>"); }
    if (canCurrentUserSeeField(5)) { html += F("<div class='statusrow2'><span class='stname'>"); html += fieldLabelHtml(5); html += F("</span><span id='s5' class='statepill offpill'>--</span></div>"); }
    if (canCurrentUserSeeField(6)) { html += F("<div class='statusrow2'><span class='stname'>"); html += fieldLabelHtml(6); html += F("</span><span id='s6' class='statepill offpill'>--</span></div>"); }
    if (canCurrentUserSeeField(7)) { html += F("<div class='statusrow2'><span class='stname'>"); html += fieldLabelHtml(7); html += F("</span><span id='si7' class='statepill offpill'>--</span></div>"); }
    if (canCurrentUserSeeField(8)) { html += F("<div class='statusrow2'><span class='stname'>"); html += fieldLabelHtml(8); html += F("</span><span id='si8' class='statepill offpill'>--</span></div>"); }
    if (canCurrentUserSeeField(9)) { html += F("<div class='statusrow2'><span class='stname'>"); html += fieldLabelHtml(9); html += F("</span><span id='si9' class='statepill offpill'>--</span></div>"); }
    if (canCurrentUserSeeField(10)) { html += F("<div class='statusrow2'><span class='stname'>"); html += fieldLabelHtml(10); html += F("</span><span id='si10' class='statepill offpill'>--</span></div>"); }
    html += F("</div></div>");
  } else {
    html += F("<div class='card'><h1>"); html += ar ? F("الحالات المصرح بها") : F("Allowed Status"); html += F("</h1><div class='msg warn'>"); html += ar ? F("لا توجد مخارج أو مداخل ظاهرة لهذا المستخدم. اطلب من الأدمن تفعيل الصلاحيات.") : F("No outputs or inputs are visible for this user. Ask Admin to enable field permissions."); html += F("</div></div>");
  }

  if (canCurrentUserControlField(1) || canCurrentUserControlField(2) || canCurrentUserControlField(5) || canCurrentUserControlField(6)) {
    html += F("<div class='card'><h1>"); html += ar ? F("التحكم السريع") : F("Quick Control"); html += F("</h1><div class='sub'>"); html += ar ? F("استخدم المفاتيح لتشغيل أو إيقاف المخارج المسموح بها.") : F("Use the switches to turn allowed outputs ON or OFF."); html += F("</div><div class='switchgrid'>");
    if (canCurrentUserControlField(1)) { html += F("<div class='switchcard'><div><b>"); html += fieldLabelHtml(1); html += F("</b><div id='ctl1' class='small'>--</div></div><label class='sw'><input id='sw1' type='checkbox' onchange='setOut(1,this.checked?1:0)'><span class='sld'></span></label></div>"); }
    if (canCurrentUserControlField(2)) { html += F("<div class='switchcard'><div><b>"); html += fieldLabelHtml(2); html += F("</b><div id='ctl2' class='small'>--</div></div><label class='sw'><input id='sw2' type='checkbox' onchange='setOut(2,this.checked?1:0)'><span class='sld'></span></label></div>"); }
    if (canCurrentUserControlField(5)) { html += F("<div class='switchcard'><div><b>"); html += fieldLabelHtml(5); html += F("</b><div id='ctl5' class='small'>--</div></div><label class='sw'><input id='sw5' type='checkbox' onchange='setOut(5,this.checked?1:0)'><span class='sld'></span></label></div>"); }
    if (canCurrentUserControlField(6)) { html += F("<div class='switchcard'><div><b>"); html += fieldLabelHtml(6); html += F("</b><div id='ctl6' class='small'>--</div></div><label class='sw'><input id='sw6' type='checkbox' onchange='setOut(6,this.checked?1:0)'><span class='sld'></span></label></div>"); }
    html += F("</div><div class='msg' id='outmsg'>"); html += ar ? F("جاهز") : F("Ready"); html += F("</div></div>");
  }

  if (admin) {
    html += F("<div class='card techcard'><h1>Technical Status</h1>");
    html += F("<div class='row'><span class='k'>Network</span><span class='v'>"); html += networkStatusText(); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>Clock</span><span class='v'>"); html += clockStateText(); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>RTC DS3231</span><span class='v'>"); html += rtcStatusText(); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>AP IP</span><span class='v'>"); html += WiFi.softAPIP().toString(); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>STA IP</span><span class='v'>"); html += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Not connected")); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>AP Clients</span><span class='v'>"); html += String(WiFi.softAPgetStationNum()); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>AP Watchdog</span><span class='v'>"); html += String(lastApWatchdogText); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>AP Restart Count</span><span class='v'>"); html += String(apWatchdogRestartCount); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>Max Loop Gap</span><span class='v'>"); html += String(maxLoopGapMs); html += F(" ms</span></div>");
    html += F("<div class='row'><span class='k'>Last Slow Task</span><span class='v'>"); html += String(lastLongTaskName); html += F(" / "); html += String(lastLongTaskMs); html += F(" ms</span></div>");
    html += F("<div class='row'><span class='k'>STA IP Mode</span><span class='v'>"); html += wifiStaticEnabled ? String("Static") : String("DHCP"); html += F("</span></div>");
    html += F("</div>");
  }

  html += mainMenuCardsHtml();

  if (admin) {
    html += F("<div class='card'><b>"); html += ar ? F("إجراءات الأدمن السريعة") : F("Admin Quick Actions"); html += F("</b>");
    html += F("<a class='btn btn2' href='"); html += sessionUrl("/restart"); html += F("'>Restart</a>");
    html += F("</div>");
  }

  html += F("<script>");
  html += F("var outState={1:0,2:0,5:0,6:0};");
  html += F("var TXT_ON='"); html += ar ? F("يعمل") : F("ON"); html += F("',TXT_OFF='"); html += ar ? F("متوقف") : F("OFF"); html += F("',TXT_ACTIVE='"); html += ar ? F("نشط") : F("ACTIVE"); html += F("',TXT_CUR_ON='"); html += ar ? F("يعمل الآن") : F("Currently ON"); html += F("',TXT_CUR_OFF='"); html += ar ? F("متوقف الآن") : F("Currently OFF"); html += F("';");
  html += F("function setPill(id,on,onText,offText){let e=document.getElementById(id);if(!e)return;e.innerHTML=on?onText:offText;e.className='statepill '+(on?'onpill':'offpill')}function paintOutOne(i,v){if(v===null||v===undefined)return;outState[i]=v?1:0;setPill('s'+i,v,TXT_ON,TXT_OFF);let sw=document.getElementById('sw'+i);if(sw)sw.checked=!!v;let c=document.getElementById('ctl'+i);if(c){c.innerHTML=v?TXT_CUR_ON:TXT_CUR_OFF;c.className='small '+(v?'ok':'bad')}}function paintInput(i,v){if(v===null||v===undefined)return;setPill('si'+i,v,TXT_ACTIVE,TXT_OFF)}");
  html += F("function paint(d){let t=document.getElementById('temp'),h=document.getElementById('hum');if(t)t.innerHTML=(d.temp==null?'--':d.temp);if(h)h.innerHTML=(d.hum==null?'--':d.hum);paintOutOne(1,d.o1);paintOutOne(2,d.o2);paintOutOne(5,d.o5);paintOutOne(6,d.o6);paintInput(7,d.i7);paintInput(8,d.i8);paintInput(9,d.i9);paintInput(10,d.i10)}");
  html += F("function refresh(){fetch('/status?sid="); html += webSessionToken; html += F("').then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(paint).catch(e=>{})}function setOut(o,s){let m=document.getElementById('outmsg');if(m)m.innerHTML='"); html += ar ? F("جاري الإرسال...") : F("Sending..."); html += F("';fetch('/setoutput',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'out='+o+'&state='+s+'&sid="); html += webSessionToken; html += F("'}).then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(d=>{paint(d);if(m)m.innerHTML=d.changed?'"); html += ar ? F("تم التنفيذ") : F("Applied"); html += F("':'"); html += ar ? F("لا يوجد تغيير") : F("No change"); html += F("';}).catch(e=>{let sw=document.getElementById('sw'+o);if(sw)sw.checked=!!outState[o];if(m)m.innerHTML='<span class=bad>"); html += ar ? F("فشل/الجلسة") : F("Failed/session"); html += F("</span>';})}setInterval(refresh,7000);refresh();");
  html += F("</script>");
  if (ar) html += F("</div>");
  html += htmlFooter();
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.send(200, "text/html", html);
}


// ========================= AUTO DAILY / EVENT BACKUP =========================

String backupTwoDigits(byte v) {
  return (v < 10) ? (String("0") + String(v)) : String(v);
}

String firmwareVersionForFile() {
  String v = FW_VERSION;
  v.replace(".", "_");
  return v;
}

String backupTimestampForFile() {
  if (isTimeValidNow()) {
    time_t now = (time_t)getDeviceEpoch();
    struct tm tmNow;
    gmtime_r(&now, &tmNow);
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
             tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
             tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
    return String(buf);
  }
  return String("uptime_") + String(millis() / 1000UL);
}

String autoBackupMakeFileName() {
  String fn = AUTO_BACKUP_PREFIX;
  fn += backupTimestampForFile();
  fn += F("_v");
  fn += firmwareVersionForFile();
  fn += F("_b");
  fn += String(FW_BUILD);
  fn += F(".bin");
  return fn;
}

String autoBackupDownloadName(const String& path) {
  if (path.startsWith("/")) return path.substring(1);
  return path;
}

bool isAutoBackupFile(const String& name) {
  return name.startsWith(AUTO_BACKUP_PREFIX) && name.endsWith(".bin");
}

uint32_t fsFileChecksum32(const String& path, uint16_t& fileSize) {
  fileSize = 0;
  if (!storageFsBeginOk && !beginStorageFs()) return 0;
  if (!SPIFFS.exists(path)) return 0;
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return 0;
  uint32_t sum = 2166136261UL;
  while (f.available()) {
    int b = f.read();
    if (b < 0) break;
    sum ^= (uint8_t)b;
    sum *= 16777619UL;
    if (fileSize < 65535) fileSize++;
    yield();
  }
  f.close();
  return sum;
}

void fileWriteU16(File& f, uint16_t v) {
  uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)(v & 0xFF)};
  f.write(b, 2);
}

void fileWriteU32(File& f, uint32_t v) {
  uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)(v & 0xFF)};
  f.write(b, 4);
}

uint16_t fileReadU16(File& f) {
  int a = f.read();
  int b = f.read();
  if (a < 0 || b < 0) return 0xFFFF;
  return ((uint16_t)a << 8) | (uint16_t)b;
}

uint32_t fileReadU32(File& f) {
  uint32_t v = 0;
  for (byte i = 0; i < 4; i++) {
    int b = f.read();
    if (b < 0) return 0xFFFFFFFFUL;
    v = (v << 8) | (uint8_t)b;
  }
  return v;
}

bool copyFileBytesTo(File& out, const String& path) {
  if (!SPIFFS.exists(path)) return true;
  File in = SPIFFS.open(path, FILE_READ);
  if (!in) return false;
  uint8_t buf[128];
  while (in.available()) {
    int n = in.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (out.write(buf, n) != (size_t)n) { in.close(); return false; }
    yield();
  }
  in.close();
  return true;
}

String findLatestAutoBackupFile() {
  if (!storageFsBeginOk && !beginStorageFs()) return String("");
  File root = SPIFFS.open("/");
  if (!root) return String("");
  String latest = "";
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    if (isAutoBackupFile(name) && (latest.length() == 0 || name > latest)) latest = name;
    f.close();
    f = root.openNextFile();
    yield();
  }
  root.close();
  return latest;
}

int countAutoBackupFiles(String& oldest) {
  oldest = "";
  if (!storageFsBeginOk && !beginStorageFs()) return 0;
  File root = SPIFFS.open("/");
  if (!root) return 0;
  int count = 0;
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    if (isAutoBackupFile(name)) {
      count++;
      if (oldest.length() == 0 || name < oldest) oldest = name;
    }
    f.close();
    f = root.openNextFile();
    yield();
  }
  root.close();
  return count;
}

void pruneAutoBackupFiles() {
  if (!storageFsBeginOk && !beginStorageFs()) return;
  byte keep = autoBackupKeepCount;
  if (keep < 1) keep = 1;
  if (keep > 14) keep = 14;
  while (true) {
    String oldest;
    int count = countAutoBackupFiles(oldest);
    if (count <= keep || oldest.length() == 0) break;
    SPIFFS.remove(oldest);
    yield();
  }
}

String autoBackupTimeText() {
  return backupTwoDigits(autoBackupHour) + String(":") + backupTwoDigits(autoBackupMinute);
}

String autoBackupStatusText() {
  String s;
  s.reserve(240);
  s += F("Daily="); s += autoBackupDailyEnabled ? F("ON") : F("OFF");
  s += F(" / Time="); s += autoBackupTimeText();
  s += F(" / OnSave="); s += autoBackupOnSaveEnabled ? F("ON") : F("OFF");
  s += F(" / Keep="); s += String(autoBackupKeepCount);
  s += F(" / Last="); s += autoBackupLastOk ? F("OK") : F("FAILED/none");
  s += F(" / Count="); s += String(autoBackupCreateCount);
  s += F(" / Reason="); s += autoBackupLastReason;
  if (autoBackupLatestFile.length()) { s += F(" / File="); s += autoBackupDownloadName(autoBackupLatestFile); }
  return s;
}

void saveAutoBackupConfig() {
  if (!storageFsBeginOk && !beginStorageFs()) return;
  File f = SPIFFS.open(AUTO_BACKUP_CFG_PATH, FILE_WRITE);
  if (!f) return;
  f.println(autoBackupDailyEnabled ? "1" : "0");
  f.println(String(autoBackupHour));
  f.println(String(autoBackupMinute));
  f.println(String(autoBackupKeepCount));
  f.println(autoBackupOnSaveEnabled ? "1" : "0");
  f.println(autoBackupTelegramNotify ? "1" : "0");
  f.println(String(autoBackupLastDailyYmd));
  f.println(autoBackupLatestFile);
  f.println(autoBackupLastReason);
  f.println(autoBackupLastMessage);
  f.println(String(autoBackupCreateCount));
  f.close();
}

void loadAutoBackupConfig() {
  autoBackupDailyEnabled = false;
  autoBackupOnSaveEnabled = true;
  autoBackupTelegramNotify = true;
  autoBackupHour = 3;
  autoBackupMinute = 0;
  autoBackupKeepCount = 7;
  autoBackupLastDailyYmd = 0;
  autoBackupLatestFile = "";
  autoBackupLastReason = "none";
  autoBackupLastMessage = "No backup yet";
  autoBackupCreateCount = 0;
  if (!storageFsBeginOk && !beginStorageFs()) return;
  if (!SPIFFS.exists(AUTO_BACKUP_CFG_PATH)) return;
  File f = SPIFFS.open(AUTO_BACKUP_CFG_PATH, FILE_READ);
  if (!f) return;
  String line;
  line = f.readStringUntil('\n'); line.trim(); autoBackupDailyEnabled = (line == "1");
  line = f.readStringUntil('\n'); line.trim(); autoBackupHour = constrain(line.toInt(), 0, 23);
  line = f.readStringUntil('\n'); line.trim(); autoBackupMinute = constrain(line.toInt(), 0, 59);
  line = f.readStringUntil('\n'); line.trim(); autoBackupKeepCount = constrain(line.toInt(), 1, 14);
  line = f.readStringUntil('\n'); line.trim(); autoBackupOnSaveEnabled = (line != "0");
  line = f.readStringUntil('\n'); line.trim(); autoBackupTelegramNotify = (line != "0");
  line = f.readStringUntil('\n'); line.trim(); autoBackupLastDailyYmd = (uint32_t)line.toInt();
  autoBackupLatestFile = f.readStringUntil('\n'); autoBackupLatestFile.trim();
  autoBackupLastReason = f.readStringUntil('\n'); autoBackupLastReason.trim();
  autoBackupLastMessage = f.readStringUntil('\n'); autoBackupLastMessage.trim();
  line = f.readStringUntil('\n'); line.trim(); autoBackupCreateCount = (uint32_t)line.toInt();
  f.close();
  if (autoBackupLatestFile.length() == 0 || !SPIFFS.exists(autoBackupLatestFile)) autoBackupLatestFile = findLatestAutoBackupFile();
  autoBackupLastOk = (autoBackupLatestFile.length() > 0 && SPIFFS.exists(autoBackupLatestFile));
}

bool createAutoBackupNow(const char* reason, bool notifyTelegram) {
  autoBackupLastOk = false;
  autoBackupLastReason = (reason && reason[0]) ? String(reason) : String("manual");
  if (!storageFsBeginOk && !beginStorageFs()) {
    autoBackupLastMessage = "FS mount failed";
    saveAutoBackupConfig();
    return false;
  }

  // Make sure the user page/intro and activation-code rescue files are current before packing the full backup.
  saveUserPagePersistentConfig("auto-backup-pack");
  saveActivationCodesToFs();

  uint16_t userSize = 0;
  uint32_t userChecksum = fsFileChecksum32(USER_PAGE_STORE_PATH, userSize);
  uint16_t actSize = 0;
  uint32_t actChecksum = fsFileChecksum32(ACTIVATION_CODE_STORE_PATH, actSize);
  uint16_t eepChecksum = eepromBackupChecksum();
  String fn = autoBackupMakeFileName();
  File f = SPIFFS.open(fn, FILE_WRITE);
  if (!f) {
    autoBackupLastMessage = "Backup open failed";
    saveAutoBackupConfig();
    return false;
  }

  char magic[8] = AUTO_BACKUP_MAGIC;
  f.write((const uint8_t*)magic, 8);
  fileWriteU16(f, AUTO_BACKUP_FORMAT_VER);
  fileWriteU16(f, EEPROM_BACKUP_MAP_VERSION);
  fileWriteU16(f, (uint16_t)FW_BUILD);
  fileWriteU16(f, EEPROM_SIZE);
  fileWriteU16(f, eepChecksum);
  fileWriteU16(f, userSize);
  fileWriteU32(f, userChecksum);
  fileWriteU16(f, actSize);
  fileWriteU32(f, actChecksum);

  uint8_t buf[128];
  for (uint16_t off = 0; off < EEPROM_SIZE; off += sizeof(buf)) {
    uint16_t n = EEPROM_SIZE - off;
    if (n > sizeof(buf)) n = sizeof(buf);
    for (uint16_t i = 0; i < n; i++) buf[i] = EEPROM.read(off + i);
    if (f.write(buf, n) != n) { f.close(); autoBackupLastMessage = "EEPROM write short"; saveAutoBackupConfig(); return false; }
    yield();
  }
  if (!copyFileBytesTo(f, USER_PAGE_STORE_PATH)) { f.close(); autoBackupLastMessage = "User store copy failed"; saveAutoBackupConfig(); return false; }
  if (!copyFileBytesTo(f, ACTIVATION_CODE_STORE_PATH)) { f.close(); autoBackupLastMessage = "Activation store copy failed"; saveAutoBackupConfig(); return false; }
  f.flush();
  f.close();

  autoBackupLatestFile = fn;
  autoBackupCreateCount++;
  autoBackupLastOk = true;
  autoBackupLastMessage = String("Backup OK: ") + autoBackupDownloadName(fn);
  pruneAutoBackupFiles();
  saveAutoBackupConfig();
  addEsp32Log(String("Auto backup OK: ") + autoBackupLastReason);
  if (notifyTelegram && autoBackupTelegramNotify) telegramSend(String("Backup OK\nReason: ") + autoBackupLastReason + "\nFile: " + autoBackupDownloadName(fn) + "\nTime: " + currentDeviceDateTimeText());
  return true;
}

void autoBackupAfterSave(const char* reason) {
  if (!autoBackupOnSaveEnabled) return;
  // Avoid double backups when one Save handler writes several internal blocks.
  if (autoBackupLastEventMs != 0 && (unsigned long)(millis() - autoBackupLastEventMs) < 5000UL) return;
  autoBackupLastEventMs = millis();
  createAutoBackupNow(reason, true);
}

uint32_t currentYmdKeyForBackup(uint16_t& nowMinute) {
  nowMinute = 0;
  if (!isTimeValidNow()) return 0;
  time_t now = (time_t)getDeviceEpoch();
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  nowMinute = (uint16_t)(tmNow.tm_hour * 60 + tmNow.tm_min);
  return (uint32_t)(tmNow.tm_year + 1900) * 10000UL + (uint32_t)(tmNow.tm_mon + 1) * 100UL + (uint32_t)tmNow.tm_mday;
}

void maintainAutoBackup() {
  if (!autoBackupDailyEnabled) return;
  unsigned long nowMs = millis();
  if (autoBackupLastCheckMs != 0 && (unsigned long)(nowMs - autoBackupLastCheckMs) < 30000UL) return;
  autoBackupLastCheckMs = nowMs;
  uint16_t nowMinute = 0;
  uint32_t ymd = currentYmdKeyForBackup(nowMinute);
  if (ymd == 0) return;
  uint16_t target = (uint16_t)autoBackupHour * 60 + autoBackupMinute;
  if (nowMinute >= target && autoBackupLastDailyYmd != ymd) {
    if (createAutoBackupNow("Daily scheduled backup", true)) {
      autoBackupLastDailyYmd = ymd;
      saveAutoBackupConfig();
    }
  }
}

bool restoreAutoBackupFile(const String& path, String& msg) {
  if (!storageFsBeginOk && !beginStorageFs()) { msg = "FS mount failed"; return false; }
  if (!SPIFFS.exists(path)) { msg = "Backup file not found"; return false; }
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) { msg = "Backup open failed"; return false; }

  char magic[8];
  if (f.read((uint8_t*)magic, 8) != 8 || memcmp(magic, AUTO_BACKUP_MAGIC, 8) != 0) { f.close(); msg = "Bad backup magic"; return false; }
  uint16_t fmt = fileReadU16(f);
  uint16_t mapVer = fileReadU16(f);
  uint16_t build = fileReadU16(f);
  (void)build;
  uint16_t eepSize = fileReadU16(f);
  uint16_t eepChecksum = fileReadU16(f);
  uint16_t userSize = fileReadU16(f);
  uint32_t userChecksum = fileReadU32(f);
  uint16_t actSize = 0;
  uint32_t actChecksum = 0;
  if (fmt >= 2) {
    actSize = fileReadU16(f);
    actChecksum = fileReadU32(f);
  }
  if (fmt < 1 || fmt > AUTO_BACKUP_FORMAT_VER) { f.close(); msg = "Format mismatch"; return false; }
  if (mapVer != EEPROM_BACKUP_MAP_VERSION || eepSize != EEPROM_SIZE) { f.close(); msg = "EEPROM map mismatch"; return false; }

  uint16_t run = 0;
  for (uint16_t off = 0; off < EEPROM_SIZE; off++) {
    int b = f.read();
    if (b < 0) { f.close(); msg = "Incomplete EEPROM data"; return false; }
    EEPROM.write(off, (byte)b);
    run = (uint16_t)(run + (byte)b);
    yield();
  }
  if (run != eepChecksum) { f.close(); msg = "EEPROM checksum mismatch"; return false; }

  if (userSize > 0) {
    if (SPIFFS.exists(AUTO_BACKUP_TMP_USER)) SPIFFS.remove(AUTO_BACKUP_TMP_USER);
    File uf = SPIFFS.open(AUTO_BACKUP_TMP_USER, FILE_WRITE);
    if (!uf) { f.close(); msg = "User temp open failed"; return false; }
    uint32_t usum = 2166136261UL;
    for (uint16_t i = 0; i < userSize; i++) {
      int b = f.read();
      if (b < 0) { uf.close(); f.close(); msg = "Incomplete user data"; return false; }
      byte by = (byte)b;
      usum ^= by;
      usum *= 16777619UL;
      uf.write(&by, 1);
      yield();
    }
    uf.flush();
    uf.close();
    if (usum != userChecksum) { f.close(); SPIFFS.remove(AUTO_BACKUP_TMP_USER); msg = "User checksum mismatch"; return false; }
    if (SPIFFS.exists(USER_PAGE_STORE_PATH)) SPIFFS.remove(USER_PAGE_STORE_PATH);
    SPIFFS.rename(AUTO_BACKUP_TMP_USER, USER_PAGE_STORE_PATH);
  }

  if (fmt >= 2 && actSize > 0) {
    if (SPIFFS.exists(AUTO_BACKUP_TMP_ACT)) SPIFFS.remove(AUTO_BACKUP_TMP_ACT);
    File af = SPIFFS.open(AUTO_BACKUP_TMP_ACT, FILE_WRITE);
    if (!af) { f.close(); msg = "Activation temp open failed"; return false; }
    uint32_t asum = 2166136261UL;
    for (uint16_t i = 0; i < actSize; i++) {
      int b = f.read();
      if (b < 0) { af.close(); f.close(); msg = "Incomplete activation data"; return false; }
      byte by = (byte)b;
      asum ^= by;
      asum *= 16777619UL;
      af.write(&by, 1);
      yield();
    }
    af.flush();
    af.close();
    if (asum != actChecksum) { f.close(); SPIFFS.remove(AUTO_BACKUP_TMP_ACT); msg = "Activation checksum mismatch"; return false; }
    if (SPIFFS.exists(ACTIVATION_CODE_STORE_PATH)) SPIFFS.remove(ACTIVATION_CODE_STORE_PATH);
    SPIFFS.rename(AUTO_BACKUP_TMP_ACT, ACTIVATION_CODE_STORE_PATH);
  }
  f.close();
  commitEeprom("restore-auto-backup");
  msg = "Restore OK";
  return true;
}

void handleAutoBackupNow() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  createAutoBackupNow("Manual backup from web", true);
  webServer.sendHeader("Location", String(WEB_OTA_PATH) + "?backup=now", true);
  webServer.send(302, "text/plain", "");
}

void handleSaveAutoBackupOptions() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  autoBackupDailyEnabled = webServer.hasArg("abdaily");
  autoBackupOnSaveEnabled = webServer.hasArg("abonsave");
  autoBackupTelegramNotify = webServer.hasArg("abtg");
  String t = webServer.arg("abtime");
  int c = t.indexOf(':');
  if (c > 0) {
    autoBackupHour = constrain(t.substring(0, c).toInt(), 0, 23);
    autoBackupMinute = constrain(t.substring(c + 1).toInt(), 0, 59);
  }
  autoBackupKeepCount = constrain(webServer.arg("abkeep").toInt(), 1, 14);
  saveAutoBackupConfig();
  pruneAutoBackupFiles();
  webServer.sendHeader("Location", String(WEB_OTA_PATH) + "?backup=saved", true);
  webServer.send(302, "text/plain", "");
}

void handleDownloadLatestAutoBackup() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  if (!storageFsBeginOk && !beginStorageFs()) { webServer.send(500, "text/plain", "FS not mounted"); return; }
  String path = autoBackupLatestFile;
  if (path.length() == 0 || !SPIFFS.exists(path)) path = findLatestAutoBackupFile();
  if (path.length() == 0 || !SPIFFS.exists(path)) { webServer.send(404, "text/plain", "No auto backup found"); return; }
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) { webServer.send(500, "text/plain", "Open backup failed"); return; }
  webServer.sendHeader("Content-Disposition", "attachment; filename=\"" + autoBackupDownloadName(path) + "\"");
  webServer.streamFile(f, "application/octet-stream");
  f.close();
}

void handleRestoreLatestAutoBackup() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  String path = autoBackupLatestFile;
  if (path.length() == 0 || !SPIFFS.exists(path)) path = findLatestAutoBackupFile();
  String msg;
  bool ok = (path.length() > 0) && restoreAutoBackupFile(path, msg);
  String html = htmlHeader("Restore Latest Auto Backup");
  html += F("<div class='card'><h1>Restore Latest Auto Backup</h1><div class='sub'>");
  html += htmlEscapeText(msg);
  html += F("</div>");
  if (ok) {
    html += F("<div class='msg ok'>Backup restored. Device will restart now.</div><script>setTimeout(function(){location.href='/login';},12000);</script>");
  } else {
    html += F("<a class='btn btn2' href='"); html += sessionUrl(String(WEB_OTA_PATH)); html += F("'>Back</a>");
  }
  html += F("</div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
  if (ok) { delay(800); ESP.restart(); }
}

// ========================= EEPROM BACKUP / RESTORE =========================

uint16_t eepromBackupChecksum() {
  uint16_t sum = 0;
  for (uint16_t i = 0; i < EEPROM_SIZE; i++) sum = (uint16_t)(sum + EEPROM.read(i));
  return sum;
}

String eepromBackupFileName() {
  String fn = "esp32_settings_";
  fn += backupTimestampForFile();
  fn += "_v";
  fn += firmwareVersionForFile();
  fn += "_b";
  fn += String(FW_BUILD);
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
    commitEeprom("eeprom-save");
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
    html += F("<div class='msg ok'>Settings restored. Device will restart now.</div>");
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
  String html = htmlHeader(currentWebLanguage() == TG_LANG_AR ? String("تحديث البرنامج") : String("Firmware Update"));
  html += F("<div class='card'><h1>Firmware Update</h1><div class='sub'>Upload ESP8266/ESP-01 .bin file only.</div>");
  html += F("<form method='POST' action='"); html += WEB_OTA_PATH; html += F("' enctype='multipart/form-data' id='upForm'>");
  html += F("<input type='file' name='firmware' accept='.bin' required><button class='btn' type='submit'>Update Firmware</button>");
  html += F("<div class='bar'><div class='fill' id='fill'></div></div><div class='msg' id='msg'>Waiting for file...</div></form><a class='btn btn2' href='/'>Back</a></div>");
  html += F("<script>const f=document.getElementById('upForm'),bar=document.getElementById('fill'),msg=document.getElementById('msg');f.onsubmit=e=>{e.preventDefault();let d=new FormData(f),x=new XMLHttpRequest();x.upload.onprogress=p=>{if(p.lengthComputable){let n=Math.round(p.loaded*100/p.total);bar.style.width=n+'%';msg.innerHTML='Uploading '+n+'%';}};x.onload=()=>{bar.style.width='100%';msg.innerHTML=x.status==200?'<span class=ok>'+x.responseText+'</span>':'<span class=bad>Update failed</span>';};x.onerror=()=>msg.innerHTML='<span class=bad>Connection error</span>';x.open('POST','"); html += WEB_OTA_PATH; html += F("');x.send(d);};</script>");

  html += F("<div class='card'><h1>GitHub Auto Update Options</h1>");
  html += F("<div class='sub'>Keep this OFF or Manual when you need the fastest Device web response. Manual Check button still works.</div>");
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
  html += F("<div class='sub'>Manual backup download names include firmware version/build and the device date. Auto Backup files also include user page storage and the 1500-char intro.</div>");
  html += F("<div class='row'><span class='k'>Auto Backup</span><span class='v'>"); html += autoBackupStatusText(); html += F("</span></div>");
  html += F("<form method='POST' action='/savebackupopts'>"); html += sessionHiddenInput();
  html += F("<label class='small'><input type='checkbox' name='abdaily' value='1'"); if (autoBackupDailyEnabled) html += F(" checked"); html += F("> Auto daily backup</label>");
  html += F("<label class='small'>Daily backup time</label><input type='time' name='abtime' value='"); html += autoBackupTimeText(); html += F("'>");
  html += F("<label class='small'>Keep last backups</label><input type='number' name='abkeep' min='1' max='14' value='"); html += String(autoBackupKeepCount); html += F("'>");
  html += F("<label class='small'><input type='checkbox' name='abonsave' value='1'"); if (autoBackupOnSaveEnabled) html += F(" checked"); html += F("> Backup after important Save events</label>");
  html += F("<label class='small'><input type='checkbox' name='abtg' value='1'"); if (autoBackupTelegramNotify) html += F(" checked"); html += F("> Telegram notify admin when backup is created</label>");
  html += F("<button class='btn' type='submit'>Save Backup Options</button></form>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/autobackupnow"); html += F("'>Create Backup Now</a>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/downloadlatestbackup"); html += F("'>Download Latest Auto Backup</a>");
  html += F("<form method='POST' action='/restorelatestbackup' onsubmit=\"return confirm('Restore latest auto backup and restart?');\">"); html += sessionHiddenInput();
  html += F("<button class='btn btn2' type='submit'>Restore Latest Auto Backup</button></form>");
  html += F("<hr><div class='sub'>Legacy EEPROM backup is still available for compatibility.</div>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/backup"); html += F("'>Download Current EEPROM Backup</a>");
  html += F("<form method='POST' action='/restore' enctype='multipart/form-data'>");
  html += sessionHiddenInput();
  html += F("<input type='file' name='backup' accept='.bin' required>");
  html += F("<button class='btn' type='submit' onclick=\"return confirm('Restore EEPROM settings and restart?');\">Upload / Restore Legacy EEPROM Backup</button>");
  html += F("</form><div class='sub warn'>Keep Erase Flash disabled unless you intentionally want a full factory reset.</div></div>");

  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void sendDashboardPage() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  bool admin = webIsAdmin();
  bool canOperate = roleAtLeast(WEB_ROLE_OPERATOR);
  bool canEngineer = roleAtLeast(WEB_ROLE_ENGINEER);

  String html = htmlHeader(currentWebLanguage() == TG_LANG_AR ? String("لوحة التحكم") : String("Device OTA Pro"));
  html += F("<div class='card'><h1>AHMED MONEM Controller</h1>");
  html += F("<div class='sub'>Secure professional OTA + WiFi Manager panel. Upload only a valid compiled <b>.bin</b> firmware file +201004608852.</div>");
  html += F("<div class='row'><span class='k'>Current User</span><span class='v'>"); html += currentWebUserName(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Login Role</span><span class='v'>"); html += roleName(currentWebRole); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Last Login</span><span class='v'>"); html += String(lastLoginUser); html += F(" / "); html += String(lastLoginIp); html += F("</span></div>");
  if (canEngineer) html += F("<a class='btn btn2' href='"); html += sessionUrl("/wifi"); html += F("'>WiFi / Network Setup</a>");
  html += F("</div>");
  html += mainMenuCardsHtml();
  html += F("<div class='card'>");
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
  if (userHasAnyVisibleIoField()) html += outputControlCardHtml();
  if (canEngineer) html += smartOutputCardHtml();
  html += cityWeatherCardHtml();
  html += tideButtonCardHtml();
  if (admin) {
  html += F("<div class='card'><form method='POST' action='"); html += WEB_OTA_PATH; html += F("' enctype='multipart/form-data' id='upForm'>");
  html += F("<b>Firmware Update</b><div class='sub'>Choose the exported firmware .bin file, then press update. Do not power off during upload.</div>");
  html += F("<input type='file' name='firmware' accept='.bin' required><button class='btn' type='submit'>Update Firmware</button>");
  html += F("<div class='bar'><div class='fill' id='fill'></div></div><div class='msg' id='msg'>Waiting for file...</div></form></div>");
  html += F("<div class='card'><a class='btn btn2' href='"); html += sessionUrl("/restart"); html += F("'>Restart Device</a><div class='sub warn' style='margin-top:10px'>Admin protected. Default first login: admin / admin</div></div>");
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
  String html = htmlHeader(currentWebLanguage() == TG_LANG_AR ? String("الاتصالات / إعداد الواي فاي") : String("Communications / WiFi Manager"));
  html += F("<div class='card'><h1>Communications</h1><div class='sub'>WiFi, ThingSpeak, DuckDNS, device and network settings. AP access: <b>192.168.4.1</b></div></div>");
  html += mainMenuCardsHtml();
  html += F("<div class='card'>");
  html += F("<div class='row'><span class='k'>Mode</span><span class='v'>"); html += wifiModeText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Network State</span><span class='v'>"); html += networkStatusText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Saved SSID</span><span class='v'>"); html += String(activeSsid[0] ? activeSsid : "Not saved"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>STA IP</span><span class='v'>"); html += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Not connected")); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>WiFi Diagnostics</span><span class='v'>"); html += wifiDiagnosticText(); html += F("</span></div>");
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

  
  html += F("<div class='card'><b>Web Server Port</b><div class='sub'>Change Device web port. Restart required after saving.</div>");
  html += F("<form method='POST' action='/savewebport'>");
  html += sessionHiddenInput();
  html += F("<label class='sub'>Port</label><input name='port' type='number' min='1' max='65535' value='");
  html += String(webServerPort);
  html += F("'>");
  html += F("<button class='btn' type='submit'>Save Web Port</button></form>");
  html += F("<div class='sub warn'>If you change this, update router Port Forward Internal Port too.</div></div>");

  html += F("<div class='card'><b>Network Setup</b><div class='sub'>Press Scan then choose a network. The chosen SSID will stay in the box.</div>");
  html += F("<button class='btn' onclick='scanWifi()'>Scan Networks</button><select id='nets' onchange='pickNet()'><option value=''>No scan yet</option></select>");
  html += F("<form method='POST' action='/savewifi' autocomplete='off'><label class='sub'>SSID</label><input id='ssid' name='ssid' maxlength='32' value='"); html += String(activeSsid); html += F("' required>");
  html += sessionHiddenInput();
  html += F("<label class='sub'>Password</label><input id='pass' name='pass' maxlength='64' type='password' autocomplete='new-password' placeholder='Leave empty only when keeping same SSID'>");
  html += F("<div class='sub'>Saved password length: "); html += String(strlen(activePass)); html += F(" chars. For a new SSID, enter the router password again.</div>");
  html += F("<label class='sub'>WiFi Mode</label><select name='mode'><option value='0'"); if (wifiManagerMode == WIFI_MODE_AP_STA_ALWAYS) html += F(" selected"); html += F(">0 - AP + STA Always</option><option value='1'"); if (wifiManagerMode == WIFI_MODE_STA_FALLBACK) html += F(" selected"); html += F(">1 - STA Only + Fallback AP</option></select>");
  html += F("<label class='small'><input type='checkbox' name='staticen' value='1'"); if (wifiStaticEnabled) html += F(" checked"); html += F("> Use Static STA IP</label>");
  html += F("<div class='sub'>Static IP is strict for port forwarding. If Static is enabled and fails, the device stays on AP/local mode so the forwarded IP never changes.</div>");
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
  bool sameSsidAsBefore = (ns == String(activeSsid));
  if (np.length() == 0 && sameSsidAsBefore) np = String(activePass);
  if (np.length() == 0 && !sameSsidAsBefore) {
    setLastWifiAction(String("Warning: new SSID saved with empty password: ") + ns);
  }

  // V1.6.6: Static IP option restored in strict mode.
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
  autoBackupAfterSave("WiFi settings saved");
  wifiDhcpRescueMode = false;
  wifiLastAttemptUsedStatic = false;
  // Do not reconnect before sending the HTTP response. Schedule a forced reconnect so stale WL_CONNECTED
  // status from the previous router cannot skip WiFi.begin(newSsid,newPass).
  scheduleWifiReconnect(1200UL, true);
  String html = htmlHeader("WiFi Saved");
  html += F("<div class='card'><h1>WiFi Saved</h1><div class='sub'>Settings saved. ESP32 will reconnect using strict Static IP when enabled, otherwise DHCP. AP/web remains active.</div>");
  html += F("<div class='row'><span class='k'>Saved SSID</span><span class='v'>"); html += String(activeSsid); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Password Length</span><span class='v'>"); html += String(strlen(activePass)); html += F(" chars</span></div>");
  html += F("<div class='row'><span class='k'>Storage Health</span><span class='v'>"); html += eepromStorageStatusText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>WiFi Diagnostics</span><span class='v'>"); html += wifiDiagnosticText(); html += F("</span></div>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/wifi"); html += F("'>Back to WiFi</a></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void handleForgetWifi() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  forgetWiFiManagerConfig();
  saveStaticIpConfig(false, String(wifiStaticIP), String(wifiStaticGateway), String(wifiStaticSubnet), String(wifiStaticDns));
  autoBackupAfterSave("WiFi forgotten");
  WiFi.disconnect(false);
  scheduleWifiReconnect(1000UL, true);
  ensurePermanentAP();
  String html = htmlHeader("WiFi Deleted");
  html += F("<div class='card'><h1>WiFi Deleted</h1><div class='sub'>Saved network deleted. Local Mode AP/web is still active on 192.168.4.1 without restart.</div><a class='btn btn2' href='"); html += sessionUrl("/wifi"); html += F("'>Back to WiFi</a></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void sendRestartPage() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;

  String html = htmlHeader(currentWebLanguage() == TG_LANG_AR ? String("جاري إعادة التشغيل") : String("Restarting"));
  html += F("<div class='card'><h1>Restarting...</h1>");
  html += F("<div class='sub'>Device will reboot now. This page will return to Home automatically.</div>");
  html += F("<div class='sub'>Please wait <span id='c'>12</span> seconds...</div>");
  html += F("<a class='btn btn2' href='/'>Back Home</a>");
  html += F("</div>");
  html += F("<script>");
  html += F("var n=12;setInterval(function(){n--;document.getElementById('c').innerHTML=n;if(n<=0){location.replace('/');}},1000);");
  html += F("</script>");
  html += htmlFooter();

  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  autoBackupAfterSave("Before restart");
  webServer.send(200, "text/html", html);
  delay(800);
  ESP.restart();
}



void handleSaveWebPort() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  uint16_t p = (uint16_t)webServer.arg("port").toInt();
  if (p < 1 || p > 65535) p = 80;
  saveWebPortConfig(p);
  autoBackupAfterSave("Web port saved");

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
  autoBackupAfterSave("DuckDNS saved");
  webServer.sendHeader("Location", "/wifi", true);
  webServer.send(302, "text/plain", "");
}

void handleClearDuckDns() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  clearDuckDnsConfig();
  autoBackupAfterSave("DuckDNS cleared");
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
  autoBackupAfterSave("Update options saved");
  webServer.sendHeader("Location", WEB_OTA_PATH, true);
  webServer.send(302, "text/plain", "");
}

void handleSaveThingSpeak() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  saveThingSpeakConfig(webServer.arg("channel"), webServer.arg("writekey"), webServer.arg("readkey"));
  autoBackupAfterSave("ThingSpeak saved");
  if (webServer.hasArg("tscmd")) {
    saveWebPerformanceSettings(githubAutoUpdateEnabled, githubAutoUpdateIntervalMode, (byte)webServer.arg("tscmd").toInt());
  }
  webServer.sendHeader("Location", "/wifi", true);
  webServer.send(302, "text/plain", "");
}

void handleClearThingSpeak() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  clearThingSpeakConfig();
  autoBackupAfterSave("ThingSpeak cleared");
  webServer.sendHeader("Location", "/wifi", true);
  webServer.send(302, "text/plain", "");
}

void handleSaveDht() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  byte t = SENSOR_TYPE_DHT11;
  if (webServer.arg("type") == "22") t = SENSOR_TYPE_DHT22;
  else if (webServer.arg("type") == "18") t = SENSOR_TYPE_DS18B20;
  saveDhtConfig(t);
  autoBackupAfterSave("Sensor type saved");
  readDhtSensorNow(true);
  webServer.sendHeader("Location", "/wifi", true);
  webServer.send(302, "text/plain", "");
}

void handleSaveDevice() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  saveDeviceConfig(webServer.arg("devname"), webServer.arg("apssid"), webServer.arg("appass"));
  autoBackupAfterSave("Device settings saved");
  String html = htmlHeader("Device Saved");
  html += F("<div class='card'><h1>Device Settings Saved</h1><div class='sub'>Device will restart now to apply AP/hostname changes.</div></div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
  delay(700);
  ESP.restart();
}

void handleFactoryReset() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  factoryResetConfig();
  String html = htmlHeader("Factory Reset");
  html += F("<div class='card'><h1>Factory Reset Done</h1><div class='sub'>All saved WiFi, ThingSpeak, and device settings were erased. Device will restart.</div></div>");
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
  secureClient.setTimeout(4000);
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
    binClient.setTimeout(10000);
    ret = httpUpdate.update(binClient, binUrl);
  } else {
    WiFiClient binClient;
    binClient.setTimeout(10000);
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
  webServer.on("/setweblang", HTTP_GET, handleSetWebLanguage);
  webServer.on("/dashboard", HTTP_GET, sendDashboardPage);
  webServer.on("/health", HTTP_GET, sendSystemHealthPage);
  webServer.on("/esp32", HTTP_GET, sendEsp32ToolsPage);
  webServer.on("/outputauto", HTTP_GET, sendOutputAutoPage);
  webServer.on("/saveoutauto", HTTP_POST, handleSaveOutputAuto);
  webServer.on("/savesp32", HTTP_POST, handleSaveEsp32Settings);
  webServer.on("/savelabels", HTTP_POST, handleSaveFieldLabels);
  webServer.on("/override", HTTP_POST, handleTimedOverride);
  webServer.on("/restart", HTTP_GET, sendRestartPage);
  webServer.on("/backup", HTTP_GET, sendEepromBackup);
  webServer.on("/restore", HTTP_POST, handleRestoreBackupDone, handleRestoreBackupUpload);
  webServer.on("/autobackupnow", HTTP_GET, handleAutoBackupNow);
  webServer.on("/savebackupopts", HTTP_POST, handleSaveAutoBackupOptions);
  webServer.on("/downloadlatestbackup", HTTP_GET, handleDownloadLatestAutoBackup);
  webServer.on("/restorelatestbackup", HTTP_POST, handleRestoreLatestAutoBackup);
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
  webServer.on("/gencodes", HTTP_POST, handleGenerateActivationCodes);
  webServer.on("/clearusedcodes", HTTP_POST, handleClearUsedActivationCodes);
  webServer.on("/codes.txt", HTTP_GET, handleDownloadActivationCodesTxt);
  webServer.on("/savesubscription", HTTP_POST, handleSaveSubscriptionSettings);
  webServer.on("/activate", HTTP_POST, handleWebActivationCode);
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
  webServer.onNotFound([](){ webServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/wifi", true); webServer.send(302, "text/plain", "Captive Portal"); });

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

void serviceWebCore(bool force) {
  unsigned long now = millis();
  if (!force && (now - lastWebPriorityMs) < WEB_PRIORITY_GAP_MS) return;
  lastWebPriorityMs = now;
#if ENABLE_WEB_OTA
  if (!webOtaStarted) setupWebOTA();
  webServer.handleClient();
#endif
  wsServer.loop();
  captiveDnsServer.processNextRequest();
  yield();
}

void apWatchdog(bool force) {
  unsigned long now = millis();
  if (!force && (now - lastApWatchdogMs) < AP_WATCHDOG_INTERVAL_MS) return;
  lastApWatchdogMs = now;

  WiFiMode_t m = WiFi.getMode();
  IPAddress apIp = WiFi.softAPIP();
  bool apOk = ((m == WIFI_AP || m == WIFI_AP_STA) && apIp != IPAddress(0,0,0,0));

  if (!apOk) {
    apWatchdogRestartCount++;
    snprintf(lastApWatchdogText, sizeof(lastApWatchdogText), "AP restart #%u", apWatchdogRestartCount);
    addEsp32Log(String("AP watchdog restart #") + String(apWatchdogRestartCount));
    savePersistentDiagnostics(String("AP watchdog restart #") + String(apWatchdogRestartCount), true);
    apRunning = false;
    WiFi.mode(WIFI_AP_STA);
    applyWiFiStabilitySettings();
    startConfigAP();
  } else {
    snprintf(lastApWatchdogText, sizeof(lastApWatchdogText), "AP OK %s clients:%d", apIp.toString().c_str(), WiFi.softAPgetStationNum());
  }
}

void resetPersistentDiagnosticsRecord() {
  memset(&persistentDiag, 0, sizeof(persistentDiag));
  persistentDiag.version = EEPROM_DIAG_VERSION;
  strncpy(persistentDiag.lastLongTaskName, "None", sizeof(persistentDiag.lastLongTaskName) - 1);
  strncpy(persistentDiag.lastResetReason, "--", sizeof(persistentDiag.lastResetReason) - 1);
  strncpy(persistentDiag.lastEvent, "No saved diagnostic event", sizeof(persistentDiag.lastEvent) - 1);
  strncpy(persistentDiag.lastEventTime, "--", sizeof(persistentDiag.lastEventTime) - 1);
}

void loadPersistentDiagnostics() {
  if (EEPROM.read(EEPROM_DIAG_MAGIC_ADDR) == EEPROM_DIAG_MAGIC) {
    EEPROM.get(EEPROM_DIAG_DATA_ADDR, persistentDiag);
    if (persistentDiag.version != EEPROM_DIAG_VERSION) {
      resetPersistentDiagnosticsRecord();
    }
  } else {
    resetPersistentDiagnosticsRecord();
  }
}

void savePersistentDiagnostics(const String& event, bool force) {
  unsigned long now = millis();
  if (!force && lastPersistentDiagSaveMs != 0 && (unsigned long)(now - lastPersistentDiagSaveMs) < EEPROM_DIAG_SAVE_MIN_GAP_MS) {
    return;
  }

  persistentDiag.version = EEPROM_DIAG_VERSION;
  persistentDiag.lastSavedUptimeSec = now / 1000UL;
  if (maxLoopGapMs > persistentDiag.maxLoopGapMs) persistentDiag.maxLoopGapMs = maxLoopGapMs;
  if (lastLongTaskMs > 0) {
    persistentDiag.lastLongTaskMs = lastLongTaskMs;
    strncpy(persistentDiag.lastLongTaskName, lastLongTaskName, sizeof(persistentDiag.lastLongTaskName) - 1);
    persistentDiag.lastLongTaskName[sizeof(persistentDiag.lastLongTaskName) - 1] = '\0';
  }
  persistentDiag.apRestartCount = apWatchdogRestartCount;
  persistentDiag.minFreeHeap = ESP.getMinFreeHeap();
  persistentDiag.rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  String rr = resetReasonText();
  rr.toCharArray(persistentDiag.lastResetReason, sizeof(persistentDiag.lastResetReason));
  event.toCharArray(persistentDiag.lastEvent, sizeof(persistentDiag.lastEvent));
  currentDeviceDateTimeText().toCharArray(persistentDiag.lastEventTime, sizeof(persistentDiag.lastEventTime));

  EEPROM.write(EEPROM_DIAG_MAGIC_ADDR, EEPROM_DIAG_MAGIC);
  EEPROM.put(EEPROM_DIAG_DATA_ADDR, persistentDiag);
  commitEeprom("eeprom-save");
  lastPersistentDiagSaveMs = now;
}

void markBootPersistentDiagnostics() {
  persistentDiag.version = EEPROM_DIAG_VERSION;
  persistentDiag.bootCount++;
  String rr = resetReasonText();
  rr.toCharArray(persistentDiag.lastResetReason, sizeof(persistentDiag.lastResetReason));
  EEPROM.write(EEPROM_DIAG_MAGIC_ADDR, EEPROM_DIAG_MAGIC);
  EEPROM.put(EEPROM_DIAG_DATA_ADDR, persistentDiag);
  commitEeprom("eeprom-save");
}

void noteTaskDuration(const char* name, unsigned long startMs) {
  unsigned long elapsed = millis() - startMs;
  if (elapsed >= LOOP_GAP_WARN_MS) {
    lastLongTaskMs = elapsed;
    strncpy(lastLongTaskName, name, sizeof(lastLongTaskName) - 1);
    lastLongTaskName[sizeof(lastLongTaskName) - 1] = '\0';
    addEsp32Log(String("Slow task: ") + name + " " + String(elapsed) + " ms");
    savePersistentDiagnostics(String("Slow task: ") + name + " " + String(elapsed) + " ms", elapsed > persistentDiag.lastLongTaskMs);
  }
}

// ========================= WIFI =========================

void startWiFiAttempt(bool forceReconnect = false) {
  lastWiFiAttemptTime = millis();
  lastWifiBeginMs = millis();
  wifiConnectAttemptStartedMs = millis();

  // V1.6.6 WiFi:
  // Keep the low-level esp_wifi_connect path that fixed STA connection.
  // Static IP is strict: if Static is ON, never retry with DHCP.
  ensurePermanentAP();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  applyWiFiStabilitySettings();

  if (!forceReconnect && WiFi.status() == WL_CONNECTED) {
    lastStaStatusCode = (int)WiFi.status();
    lastWifiConnectedMs = millis();
    storeWifiDisconnectReason(0);
    setLastWifiAction(String("STA already online: ") + WiFi.SSID() + " IP " + WiFi.localIP().toString());
    writeStatusLed(true);
    setupOTA();
    setupWebOTA();
    return;
  }

  if (activeSsid[0] == '\0') {
    lastStaStatusCode = (int)WiFi.status();
    setLastWifiAction("No saved WiFi - AP only");
    writeStatusLed(false);
    return;
  }

  lastWifiSavedPassLen = strlen(activePass);
  lastStaScanFound = false;
  lastStaScanChannel = 0;
  lastStaScanRssi = -999;
  strncpy(lastStaScanBssid, "not-used", sizeof(lastStaScanBssid) - 1);
  lastStaScanBssid[sizeof(lastStaScanBssid) - 1] = '\0';

  // Strict behavior for port forwarding:
  // Static ON  = Static only. No DHCP fallback/rescue.
  // Static OFF = DHCP only.
  bool useStaticThisTry = wifiStaticEnabled;
  wifiDhcpRescueMode = false;
  wifiLastAttemptUsedStatic = false;

  DBG_PRINT("Low-level STA connect: ");
  DBG_PRINTLN(activeSsid);

  WiFi.mode(WIFI_AP_STA);
  WiFi.enableSTA(true);
  applyWiFiStabilitySettings();
  startConfigAP();

  esp_wifi_disconnect();
  delay(80);

  if (useStaticThisTry) {
    bool staticOk = applyStaticIpBeforeConnect();
    wifiLastAttemptUsedStatic = true;
    if (!staticOk) {
      lastStaLowLevelSetConfigErr = -100;
      lastStaLowLevelConnectErr = -100;
      lastStaStatusCode = (int)WiFi.status();
      setLastWifiAction("STATIC_INVALID - AP ONLY, no DHCP fallback");
      writeStatusLed(false);
      return;
    }
  }

  wifi_config_t staCfg;
  memset(&staCfg, 0, sizeof(staCfg));
  strncpy((char*)staCfg.sta.ssid, activeSsid, sizeof(staCfg.sta.ssid) - 1);
  strncpy((char*)staCfg.sta.password, activePass, sizeof(staCfg.sta.password) - 1);
  staCfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  staCfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  staCfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
  staCfg.sta.pmf_cfg.capable = true;
  staCfg.sta.pmf_cfg.required = false;

  esp_err_t modeErr = esp_wifi_set_mode(WIFI_MODE_APSTA);
  (void)modeErr;
  esp_err_t cfgErr = esp_wifi_set_config(WIFI_IF_STA, &staCfg);
  esp_err_t connErr = esp_wifi_connect();
  lastStaLowLevelSetConfigErr = (int)cfgErr;
  lastStaLowLevelConnectErr = (int)connErr;
  lastStaStatusCode = (int)WiFi.status();

  setLastWifiAction(String("Low-level esp_wifi_connect ") +
                    (wifiLastAttemptUsedStatic ? String("STATIC-STRICT ") : String("DHCP ")) +
                    String(activeSsid) + String(" cfg=") + String((int)cfgErr) +
                    String(" conn=") + String((int)connErr));

  applyWiFiStabilitySettings();
  writeStatusLed(false);
}


void maintainWiFi() {
  ensurePermanentAP();

  if (wifiReconnectPending && (long)(millis() - wifiReconnectDueMs) >= 0) {
    bool force = wifiForceReconnectPending;
    wifiReconnectPending = false;
    wifiForceReconnectPending = false;
    startWiFiAttempt(force);
    return;
  }

  wl_status_t currentStaStatus = WiFi.status();
  lastStaStatusCode = (int)currentStaStatus;

  if (currentStaStatus == WL_CONNECTED) {
    if (!printedOnlineThingSpeakMode) {
      DBG_PRINTLN("WiFi connected -> Online Mode restored. ThingSpeak / Weather / GitHub OTA active.");
      DBG_PRINTLN("AP/web remains active for local control.");
      printedOnlineThingSpeakMode = true;
      printedOfflineEepromMode = false;
      lastWifiConnectedMs = millis();
      setLastWifiAction(String("Connected: ") + WiFi.SSID() + " IP " + WiFi.localIP().toString());
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
    lastWifiDisconnectedMs = millis();
    setLastWifiAction(String("Disconnected: ") + wifiStatusName(currentStaStatus));
    printedOfflineEepromMode = true;
    printedOnlineThingSpeakMode = false;
  }

  updateStatusLed();

  // Strict static behavior for port forwarding:
  // If Static is enabled and connection fails, do NOT retry DHCP.
  // The AP/web interface stays available on 192.168.4.1 so settings can be corrected.

  if (millis() - lastWiFiAttemptTime >= WIFI_RETRY_INTERVAL) {
    startWiFiAttempt(false);
  }
}

// ========================= STATUS LED =========================

void updateStatusLed() {
#if ENABLE_STATUS_LED
  // Dedicated internet LED is independent from Output 3.
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

  for (byte i = 0; i < 1; i++) {
    long value = ThingSpeak.readLongField(getThingSpeakChannelId(), field, tsReadKey);
    int status = ThingSpeak.getLastReadStatus();
    lastThingSpeakReadStatus = status;

    if (status == 200) {
      return value;
    }

    DBG_PRINT("ThingSpeak read failed, field ");
    DBG_PRINT(field);
    DBG_PRINT(", status: ");
    DBG_PRINTLN(status);

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
  currentOutput5State = newState;
  currentOutput6State = newState;
  applyOutputs();
  saveOutputState(1, currentOutput1State);
  saveOutputState(2, currentOutput2State);
  saveOutputState(5, currentOutput5State);
  saveOutputState(6, currentOutput6State);
  recordOutputManualControl(1, newState);
  recordOutputManualControl(2, newState);
  recordOutputManualControl(5, newState);
  recordOutputManualControl(6, newState);
  rememberOutputCommand("Button", 1, newState, true);
  rememberOutputCommand("Button", 2, newState, true);
  rememberOutputCommand("Button", 5, newState, true);
  rememberOutputCommand("Button", 6, newState, true);

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
      byte newState = (currentOutput1State == 1 && currentOutput2State == 1 && currentOutput5State == 1 && currentOutput6State == 1) ? 0 : 1;
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
    bool changed = (newState != currentOutput1State);
    rememberOutputCommand("ThingSpeak", outputNumber, newState, changed);
    if (!changed) { DBG_PRINTLN("Output1 command unchanged -> no EEPROM write"); wsBroadcastStatus(); return; }
    currentOutput1State = newState;
    setRelay(OUTPUT1_PIN, currentOutput1State);
    saveOutputState(1, currentOutput1State);
    recordOutputManualControl(1, newState);
    wsBroadcastStatus();
  } else if (outputNumber == 2) {
    bool changed = (newState != currentOutput2State);
    rememberOutputCommand("ThingSpeak", outputNumber, newState, changed);
    if (!changed) { DBG_PRINTLN("Output2 command unchanged -> no EEPROM write"); wsBroadcastStatus(); return; }
    currentOutput2State = newState;
    setRelay(OUTPUT2_PIN, currentOutput2State);
    saveOutputState(2, currentOutput2State);
    recordOutputManualControl(2, newState);
    wsBroadcastStatus();
  } else if (outputNumber == 5) {
    bool changed = (newState != currentOutput5State);
    rememberOutputCommand("ThingSpeak", outputNumber, newState, changed);
    if (!changed) { DBG_PRINTLN("Output5 command unchanged -> no EEPROM write"); wsBroadcastStatus(); return; }
    currentOutput5State = newState;
    setRelay(OUTPUT5_PIN, currentOutput5State);
    saveOutputState(5, currentOutput5State);
    recordOutputManualControl(5, newState);
    wsBroadcastStatus();
  } else if (outputNumber == 6) {
    bool changed = (newState != currentOutput6State);
    rememberOutputCommand("ThingSpeak", outputNumber, newState, changed);
    if (!changed) { DBG_PRINTLN("Output6 command unchanged -> no EEPROM write"); wsBroadcastStatus(); return; }
    currentOutput6State = newState;
    setRelay(OUTPUT6_PIN, currentOutput6State);
    saveOutputState(6, currentOutput6State);
    recordOutputManualControl(6, newState);
    wsBroadcastStatus();
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
  long output5State = readThingSpeakField(FIELD5_OUTPUT5);
  long output6State = readThingSpeakField(FIELD6_OUTPUT6);

  DBG_PRINT("ThingSpeak Field1 Output1 read = "); DBG_PRINTLN(output1State);
  DBG_PRINT("ThingSpeak Field2 Output2 read = "); DBG_PRINTLN(output2State);
  DBG_PRINT("ThingSpeak Field5 Output5 read = "); DBG_PRINTLN(output5State);
  DBG_PRINT("ThingSpeak Field6 Output6 read = "); DBG_PRINTLN(output6State);

  if (!isValidState(output1State)) DBG_PRINTLN("Invalid or failed Output1 command. Keeping last state.");
  else setOutputState(1, (byte)output1State);

  if (!isValidState(output2State)) DBG_PRINTLN("Invalid or failed Output2 command. Keeping last state.");
  else setOutputState(2, (byte)output2State);

  if (!isValidState(output5State)) DBG_PRINTLN("Invalid or failed Output5 command. Keeping last state.");
  else setOutputState(5, (byte)output5State);

  if (!isValidState(output6State)) DBG_PRINTLN("Invalid or failed Output6 command. Keeping last state.");
  else setOutputState(6, (byte)output6State);
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
  http.setTimeout(2500);
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
  http.setTimeout(2500);
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

  String savedIP = readThingSpeakFieldTextPublic(FIELD5_OUTPUT5);
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
  http.setTimeout(2500);

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
  ThingSpeak.setStatus(String("User: ") + tsUser);
  ThingSpeak.setField(FIELD5_OUTPUT5, currentOutput5State);
  ThingSpeak.setField(FIELD6_OUTPUT6, currentOutput6State);
  ThingSpeak.setField(FIELD7_INPUT7, readInput7State());
  ThingSpeak.setField(FIELD8_INPUT8, readInput8State());

  int status = ThingSpeak.writeFields(getThingSpeakChannelId(), tsWriteKey);
  lastThingSpeakWriteStatus = status;
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


String resetReasonText() {
  esp_reset_reason_t r = esp_reset_reason();
  switch (r) {
    case ESP_RST_POWERON: return "Power on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software restart";
    case ESP_RST_PANIC: return "Panic/exception";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_BROWNOUT: return "Brownout / weak power";
    case ESP_RST_SDIO: return "SDIO reset";
    default: return String("Unknown ") + String((int)r);
  }
}

String formatUptimeText(unsigned long seconds) {
  unsigned long days = seconds / 86400UL;
  seconds %= 86400UL;
  byte hours = seconds / 3600UL;
  seconds %= 3600UL;
  byte minutes = seconds / 60UL;
  byte secs = seconds % 60UL;
  String out;
  if (days) { out += String(days); out += "d "; }
  if (hours < 10) out += '0'; out += String(hours); out += ':';
  if (minutes < 10) out += '0'; out += String(minutes); out += ':';
  if (secs < 10) out += '0'; out += String(secs);
  return out;
}

void sendSystemHealthPage() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  String html = htmlHeader("Device System Health");
  html += F("<div class='card'><h1>System Health</h1><div class='sub'>Stability / RAM / network diagnostics.</div>");
  html += F("<div class='row'><span class='k'>Uptime</span><span class='v'>"); html += formatUptimeText(millis() / 1000UL); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Free Heap</span><span class='v'>"); html += String(ESP.getFreeHeap()); html += F(" bytes</span></div>");
  html += F("<div class='row'><span class='k'>Min Free Heap</span><span class='v'>"); html += String(ESP.getMinFreeHeap()); html += F(" bytes</span></div>");
  html += F("<div class='row'><span class='k'>Max Alloc Heap</span><span class='v'>"); html += String(ESP.getMaxAllocHeap()); html += F(" bytes</span></div>");
  html += F("<div class='row'><span class='k'>Restart Reason</span><span class='v'>"); html += resetReasonText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>WiFi</span><span class='v'>"); html += networkStatusText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Clock Source</span><span class='v'>"); html += clockStateText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>RTC DS3231</span><span class='v'>"); html += rtcStatusText(); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>RSSI</span><span class='v'>"); html += String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0); html += F(" dBm</span></div>");
  html += F("<div class='row'><span class='k'>STA IP</span><span class='v'>"); html += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Not connected")); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>MQTT</span><span class='v'>"); html += mqttClient.connected() ? String("Connected") : String("Disconnected"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Telegram Poll Code</span><span class='v'>"); html += String(lastTelegramPollCode); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Telegram Send Code</span><span class='v'>"); html += String(lastTelegramSendCode); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Telegram Pending</span><span class='v'>"); html += telegramPendingSend ? String("YES") : String("NO"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>ThingSpeak Read</span><span class='v'>"); html += String(lastThingSpeakReadStatus); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>ThingSpeak Write</span><span class='v'>"); html += String(lastThingSpeakWriteStatus); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>TS Command Interval</span><span class='v'>"); html += String(commandCheckIntervalMs / 1000UL); html += F(" sec</span></div>");
  html += F("<div class='row'><span class='k'>Telegram Poll Interval</span><span class='v'>"); html += String(telegramCurrentPollIntervalMs() / 1000UL); html += F(" sec</span></div>");
  html += F("<div class='row'><span class='k'>WS Broadcast Min</span><span class='v'>"); html += String(WS_BROADCAST_MIN_INTERVAL_MS); html += F(" ms</span></div>");
  html += F("<div class='row'><span class='k'>Automation TS Sync O1</span><span class='v'>"); html += outputAutoSyncLabel(outputAutoIndex(1)); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Automation TS Sync O2</span><span class='v'>"); html += outputAutoSyncLabel(outputAutoIndex(2)); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Automation TS Sync O5</span><span class='v'>"); html += outputAutoSyncLabel(outputAutoIndex(5)); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Automation TS Sync O6</span><span class='v'>"); html += outputAutoSyncLabel(outputAutoIndex(6)); html += F("</span></div>");
  html += F("</div>");

  html += F("<div class='card'><h2>Persistent Health - survives restart</h2><div class='sub'>Saved in EEPROM only on important events or throttled to protect EEPROM.</div>");
  html += F("<div class='row'><span class='k'>Boot Count</span><span class='v'>"); html += String(persistentDiag.bootCount); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Last Reset Reason</span><span class='v'>"); html += String(persistentDiag.lastResetReason); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Last Saved Event</span><span class='v'>"); html += String(persistentDiag.lastEvent); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Event Date</span><span class='v'>"); html += String(persistentDiag.lastEventTime); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Saved Uptime</span><span class='v'>"); html += formatUptimeText(persistentDiag.lastSavedUptimeSec); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Saved Max Loop Gap</span><span class='v'>"); html += String(persistentDiag.maxLoopGapMs); html += F(" ms</span></div>");
  html += F("<div class='row'><span class='k'>Saved Slow Task</span><span class='v'>"); html += String(persistentDiag.lastLongTaskName); html += F(" / "); html += String(persistentDiag.lastLongTaskMs); html += F(" ms</span></div>");
  html += F("<div class='row'><span class='k'>Saved AP Restarts</span><span class='v'>"); html += String(persistentDiag.apRestartCount); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Saved Min Heap</span><span class='v'>"); html += String(persistentDiag.minFreeHeap); html += F(" bytes</span></div>");
  html += F("<div class='row'><span class='k'>Saved RSSI</span><span class='v'>"); html += String(persistentDiag.rssi); html += F(" dBm</span></div>");
  html += F("</div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

// ========================= ARDUINO =========================


void handleAutoRestart24h() {
#if ENABLE_AUTO_RESTART_24H
  unsigned long now = millis();
  if (!autoRestart24hFired && (unsigned long)(now - autoRestartBootMs) >= AUTO_RESTART_INTERVAL_MS) {
    autoRestart24hFired = true;
    addEsp32Log("Scheduled 24h auto restart");
    savePersistentDiagnostics("Scheduled 24h auto restart", true);
    telegramSend(String("Device scheduled auto restart after 24 hours\nTime: ") + currentDeviceDateTimeText());
    wsBroadcastStatus();
    delay(800);
    ESP.restart();
  }
#endif
}

void setup() {
  DBG_BEGIN(115200);
  delay(100);
  autoRestartBootMs = millis();
  autoRestart24hFired = false;
  DBG_PRINTLN();
  DBG_PRINTLN(FW_FULL_NAME);
  DBG_PRINTLN("Mode: DEBUG SERIAL ON / INTERNET LED OFF");
  DBG_PRINTLN("After testing, set DEBUG_MODE to 0 to disable Serial and enable TX internet LED.");
  WiFi.onEvent(onArduinoWiFiEvent);

  DBG_PRINTLN("Starting EEPROM...");
  eepromBeginOk = EEPROM.begin(EEPROM_SIZE);
  DBG_PRINT("EEPROM.begin: "); DBG_PRINTLN(eepromBeginOk ? "OK" : "FAILED");
  beginStorageFs();
  loadEepromImageFromFs();
  loadPersistentDiagnostics();
  markBootPersistentDiagnostics();
  loadWebPortConfig();
  loadWebUsersConfig();
  loadUserFieldPermissions();
  loadOutputStates();
  loadPendingSyncFlag();
  loadWiFiManagerConfig();
  loadStaticIpConfig();
  loadThingSpeakConfig();
  loadWebPerformanceSettings();
  loadEsp32IntegrationConfig();
  loadTelegramUiConfig();
  loadUserTelegramNotifications();
  loadActivationCodes();
  loadActivationLog();
  loadFieldLabelsConfig();
  loadDuckDnsConfig();
  loadDhtConfig();
  loadDeviceConfig();
  loadSmartOutputConfig();
  loadOutputAutoConfigs();
  loadOutputAutoSyncConfig();
  loadManualAutoOff9Config();
  loadManualAutoOffDurationConfig();
  loadEgyptTimeMode();
  loadOfflineClock();
  initRtcDs3231();
  loadMonthlySubscriptionState();
  loadLoginIntroConfig();
  loadUserPagePersistentConfig();
  loadAutoBackupConfig();
  loadCityWeatherConfig();
  debugDumpEepromBytes("After EEPROM load at boot");
  debugPrintRuntimeState("After EEPROM load, before applying outputs");

  DBG_PRINTLN("Configuring outputs...");
  pinMode(OUTPUT1_PIN, OUTPUT);
  pinMode(OUTPUT2_PIN, OUTPUT);
  pinMode(OUTPUT5_PIN, OUTPUT);
  pinMode(OUTPUT6_PIN, OUTPUT);
  pinMode(INPUT7_PIN, INPUT_PULLUP);
  pinMode(INPUT8_PIN, INPUT_PULLUP);
  pinMode(INPUT9_PIN, INPUT_PULLUP);
  pinMode(INPUT10_PIN, INPUT_PULLUP);
  pinMode(INTERNET_LED_PIN, OUTPUT);
  writeStatusLed(false);
#if ENABLE_STATUS_LED
  pinMode(STATUS_LED_PIN, OUTPUT);
  setRelay(STATUS_LED_PIN, 0); // Output 3 safe OFF at boot; internet LED is on GPIO18.
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
  applyWiFiStabilitySettings();
  startConfigAP();
  apWatchdog(true);
  setupWebOTA();
  setupWebSocketDashboard();
  startCaptivePortalDns();
  addEsp32Log("Boot complete - WiFi low-level connect + storage rescue active");
  startWiFiAttempt();
  setupOTA();

  // Run the first command check shortly after boot, then normal intervals.
  lastCommandCheckTime = millis() - commandCheckIntervalMs + 3000UL;
  lastSensorUpdateTime = millis() - SENSOR_UPDATE_INTERVAL + 5000UL;

  DBG_PRINTLN("Setup complete. EEPROM debug is active.");
}

void loop() {
  unsigned long loopStartMs = millis();
  if (lastLoopTickMs != 0) {
    unsigned long gap = loopStartMs - lastLoopTickMs;
    if (gap > maxLoopGapMs) maxLoopGapMs = gap;
    if (gap >= LOOP_GAP_WARN_MS) {
      addEsp32Log(String("Loop gap warning: ") + String(gap) + " ms");
      savePersistentDiagnostics(String("Loop gap warning ") + String(gap) + " ms", gap > persistentDiag.maxLoopGapMs);
    }
  }
  lastLoopTickMs = loopStartMs;

  yield();
  serviceWebCore(true);

  unsigned long t = millis();
  handleOTA();
  noteTaskDuration("OTA", t);
  serviceWebCore();

  t = millis();
  apWatchdog(false);
  noteTaskDuration("AP watchdog", t);
  serviceWebCore();

  t = millis();
  maintainWiFi();
  noteTaskDuration("WiFi", t);
  serviceWebCore();

  t = millis();
  startNtpIfNeeded();
  maintainRtcDs3231();
  noteTaskDuration("NTP/RTC", t);
  serviceWebCore();

  maintainOfflineClock();
  maintainAutoBackup();
  maintainMonthlyUserSubscriptionDisable(false);

  t = millis();
  handleAutoUpdateCheck();
  noteTaskDuration("GitHub update", t);
  serviceWebCore();

  // V1.2.19 Web Speed Fix: external weather is manual only to keep ESP-01 web responsive.
  // Use the Refresh Weather button when needed; no automatic HTTPS weather call in loop.
  // Public/Application IP sync is disabled in V1.2.53.
  t = millis();
  updateDuckDNS(false);
  noteTaskDuration("DuckDNS", t);
  serviceWebCore();

  updateStatusLed();
  updateWebStatusSensorCache();
  applySmartOutputControl();
  handleDhtPinButton();

  t = millis();
  processPendingOutputThingSpeakUpdate();
  noteTaskDuration("ThingSpeak pending", t);
  serviceWebCore();

  t = millis();
  maintainMqtt();
  noteTaskDuration("MQTT", t);
  serviceWebCore();

  checkTelegramAlerts();

  t = millis();
  checkTelegramCommands();
  noteTaskDuration("Telegram poll", t);
  serviceWebCore();

  t = millis();
  processPendingTelegramSend();
  noteTaskDuration("Telegram send", t);
  serviceWebCore();

  processPendingWsBroadcast();
  handleManualOverrideExpiry();
  maintainManualAutoOff9();
  maintainManualAutoOffDuration();
  maintainOutputAutoControl();
  maintainDigitalInputs();
  handleAutoRestart24h();

  unsigned long now = millis();

  if (now - lastCommandCheckTime >= commandCheckIntervalMs) {
    lastCommandCheckTime = now;
    t = millis();
    checkRemoteCommands();
    noteTaskDuration("ThingSpeak read", t);
    serviceWebCore(true);
  }

  if (now - lastSensorUpdateTime >= SENSOR_UPDATE_INTERVAL) {
    t = millis();
    bool sensorSent = readAndSendSensorData();
    noteTaskDuration("Sensor/TS write", t);
    serviceWebCore(true);
    if (sensorSent) {
      lastSensorUpdateTime = millis();
    } else {
      // Retry sooner after failure, without blocking or restarting.
      lastSensorUpdateTime = millis() - SENSOR_UPDATE_INTERVAL + 10000UL;
    }
  }
}



