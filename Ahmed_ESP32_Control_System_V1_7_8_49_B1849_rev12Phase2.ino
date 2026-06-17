
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
#define FW_VERSION         "1.7.8.49"
#define FW_BUILD           1849
#define FW_NAME            "Ahmed Monem Smart Control System"
#define FW_DEVICE_NAME     "Weather and Control "
#define FW_FULL_NAME       "Ahmed Control System +201004608852 . V1.7.8.49 Build 1849  By Ahmed Monem"
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

// V1.7.8.20: Prayer Times added to Home and Telegram with admin governorate selection and cached AlAdhan API data.
// V1.7.8.21: Prayer Times API HTTP 301/302 redirect fix: dated endpoint + HTTPClient strict redirects.
// V1.7.8.24: One admin-selected area is used for Prayer Times, Adhan and Internet Weather; Home weather card shows live sensor + Internet weather; Adhan Telegram message hides GPIO.
// V1.7.8.25: Fix per-user Adhan Notify persistence by adding it to the User Management SPIFFS rescue store V3.
// V1.7.8.28: Input Cards Sensor Mode UI/storage and localized 12-hour display time (AR: ص/م, EN: AM/PM).
// V1.7.8.29: Compile fix for localized 12-hour Home date/time helper ar parameter.
// V1.7.8.30: Prayer Times page and Telegram prayer list display cached API times in localized 12-hour format.
// V1.7.8.32: Fix Smart/Input Cards EEPROM persistence by moving the smart input data block to 3400..4095 and moving input-card permission masks to the safe 2220..2281 gap.
// V1.7.8.33: Home Input Cards expanded details now show Sensor mode correctly instead of falling back to Digital, with adaptive Sensor/Analog/Digital detail rows.
// V1.7.8.34: Fix mobile Home Input Cards layout so titles/details wrap normally and action pills stay inside the card.
// V1.7.8.49: Creative UI Phase 2 - premium dashboard glass polish, quick link tiles, table/card hover and mobile refinement only.
// V1.7.8.43: Quick Links now has 3 configurable buttons; each button can be CSV/Image/Text/PDF with per-user access.
// V1.7.8.48: Creative UI phase 1 - global glass/dark theme polish for all pages; CSS/HTML only, no logic changes.
// V1.7.8.35: Add Telegram admin System Health button and admin Home section show/hide settings.
// V1.7.8.26: Home UI separates live device sensor readings from Internet weather readings into two clear cards.
// V1.7.8.27: Organized unified area list with shared coordinates for Prayer/Weather, plus per-user Startup Telegram Report.
// V1.7.8.22: Dedicated Adhan GPIO output with duration/prayer selection and per-user Adhan Telegram notify checkbox.
// V1.7.8.19: Home now shows public external links for all users: Ras Sedr Tide and configured Telegram bot link.
// V1.7.8.18: Admin-only UI Branding Settings page controls Home hero/header texts and public Telegram bot link.
// V1.7.8.17: Mobile fix for Home Allowed Status output switches: no horizontal overflow; ON/OFF pill and switch stay fully visible on narrow phones.
// V1.7.8.16: Home Allowed Status is shown before Input Cards; Quick Control panel removed because output switches are now inside Allowed Status.
// V1.7.8.15: Home Outputs list now shows the ON/OFF switch beside each output status/details; Quick Control remains available.
// V1.7.8.11: Creative Dark UI for Home page only; glass cards, neon status colors, hero header and mobile polish.
// V1.7.8.10: Compile fix for accidental zeros after activationCodeCreatedAt declaration; keeps V1.7.8.9 UI Home Polish only changes.
// V1.7.8.4: Storage Safe Boot. Corrupted SPIFFS rescue files are ignored so AP/web can still start.
// V1.7.8.3: Activation Log rescue store. This does NOT change activation-code generation;
// it only protects the last activation audit records from being lost after restart/backup/restore.
#define ACTIVATION_LOG_STORE_PATH    "/act_log_v1783.bin"
#define ACTIVATION_LOG_STORE_MAGIC   0x41434C33UL  // ACL3
#define ACTIVATION_LOG_STORE_VERSION 1
struct ActivationLogStoreRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t checksum;
  char code[ACTIVATION_LOG_COUNT][ACTIVATION_CODE_LEN + 1];
  char user[ACTIVATION_LOG_COUNT][WEB_USER_MAX_LEN + 1];
  char at[ACTIVATION_LOG_COUNT][ACTIVATION_USED_AT_MAX_LEN + 1];
  char method[ACTIVATION_LOG_COUNT][ACTIVATION_METHOD_MAX_LEN + 1];
};
bool activationLogStoreLoadedAtBoot = false;
bool activationLogStoreLastSaveOk = false;
uint32_t activationLogStoreSaveCount = 0;
char activationLogStoreLastError[36] = "none";

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
#define OUTPUT7_PIN        19    // Aux output 7 for Smart Input Cards only, no ThingSpeak schedule
#define OUTPUT8_PIN        23    // Aux output 8 for Smart Input Cards only, no ThingSpeak schedule
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
const char* ssid     = "ahmed-monem2";
const char* password = "Ahmed-2000";

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
#define EEPROM_OUTPUT7_ADDR  5   // Aux output 7 state
#define EEPROM_OUTPUT8_ADDR  6   // Aux output 8 state
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
const unsigned long OFFLINE_CLOCK_SAVE_INTERVAL_MS = 86400000UL; // save valid clock at most every 24 hours

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


// Smart Input Cards (six configurable input cards).
// Admin/Engineer can choose Digital/Analog, pin, label, filtering, Telegram notification and output action.
// Stored in EEPROM and protected by the existing SPIFFS EEPROM-image rescue layer.
#define SMART_INPUT_COUNT                   6
#define SMART_INPUT_NAME_MAX_LEN            20
#define SMART_INPUT_UNIT_MAX_LEN            8
#define SMART_INPUT_MSG_MAX_LEN             32
// V1.7.8.32 EEPROM map fix:
// Runtime SmartInputCardConfig is 116 bytes on ESP32.  Six cards need 696 bytes.
// Old map used magic=3400 and data=3404, which ended at 4099 and overlapped
// input-card permission masks at 4030.  New map stores the magic byte at 3399
// and the six-card data array at 3400..4095 exactly.
#define EEPROM_SMART_INPUT_OLD_MAGIC_ADDR   3400
#define EEPROM_SMART_INPUT_OLD_DATA_ADDR    3404
#define EEPROM_SMART_INPUT_MAGIC_ADDR       3399
#define EEPROM_SMART_INPUT_DATA_ADDR        3400
#define EEPROM_SMART_INPUT_MAGIC            0xC6
#define EEPROM_SMART_INPUT_VERSION          3
#define SMART_INPUT_MODE_DIGITAL            0
#define SMART_INPUT_MODE_ANALOG             1
#define SMART_INPUT_MODE_SENSOR             2
#define SMART_INPUT_SENSOR_DHT11            0
#define SMART_INPUT_SENSOR_DHT22            1
#define SMART_INPUT_SENSOR_DS18B20          2
#define SMART_INPUT_SENSOR_METRIC_TEMP      0
#define SMART_INPUT_SENSOR_METRIC_HUM       1
#define SMART_INPUT_TRIGGER_ON              0
#define SMART_INPUT_TRIGGER_OFF             1
#define SMART_INPUT_TRIGGER_CHANGE          2
#define SMART_INPUT_TRIGGER_WHILE_ON        3
#define SMART_INPUT_ACTION_NONE             0
#define SMART_INPUT_ACTION_ON               1
#define SMART_INPUT_ACTION_OFF              2
#define SMART_INPUT_ACTION_TOGGLE           3
#define SMART_INPUT_ACTION_PULSE            4
#define SMART_INPUT_ACTION_FOLLOW           5
#define SMART_INPUT_READ_INTERVAL_MS        500UL
#define SMART_INPUT_DEFAULT_DEBOUNCE_MS     250
#define SMART_INPUT_DEFAULT_COOLDOWN_SEC    60
#define SMART_INPUT_DEFAULT_HYSTERESIS      10.0f
// Moved from 4030 because old location overlapped the six Smart Input Cards.
// Field-label block ends at 2213, user permissions start at 2300, so 2220..2281 is safe.
#define EEPROM_INPUT_CARD_PERM_OLD_MAGIC_ADDR 4030
#define EEPROM_INPUT_CARD_PERM_OLD_DATA_ADDR  4032
#define EEPROM_INPUT_CARD_PERM_MAGIC_ADDR     2220
#define EEPROM_INPUT_CARD_PERM_DATA_ADDR      2222
#define EEPROM_INPUT_CARD_PERM_MAGIC          0x7C
#define SMART_INPUT_ALL_MASK                0x3F

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
byte currentOutput7State = 0;
byte currentOutput8State = 0;

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
char mqttBaseTopic[64] = "ahmed/device";
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
#define CUSTOM_LINK_COUNT 3
#define CUSTOM_LINK_LABEL_MAX_LEN 24
#define CUSTOM_LINK_URL_MAX_LEN 180
#define CUSTOM_LINK_TYPE_CSV   0
#define CUSTOM_LINK_TYPE_IMAGE 1
#define CUSTOM_LINK_TYPE_TEXT  2
#define CUSTOM_LINK_TYPE_PDF   3
char customLinkLabel[CUSTOM_LINK_COUNT][CUSTOM_LINK_LABEL_MAX_LEN + 1] = {"data", "Image 1", "Image 2"};
char customLinkUrl[CUSTOM_LINK_COUNT][CUSTOM_LINK_URL_MAX_LEN + 1] = {"", "", ""};
byte customLinkType[CUSTOM_LINK_COUNT] = {CUSTOM_LINK_TYPE_CSV, CUSTOM_LINK_TYPE_IMAGE, CUSTOM_LINK_TYPE_IMAGE};
byte webUserCustomLinkAccess[WEB_USER_COUNT][CUSTOM_LINK_COUNT] = {{1, 1, 1}};
#define HOME_KICKER_MAX_LEN 48
#define HOME_TITLE_MAX_LEN 64
#define HOME_SUBTITLE_MAX_LEN 160
#define HOME_BADGE_MAX_LEN 48
#define HOME_UI_CONFIG_PATH "/home_ui_v17818.cfg"
#define HOME_UI_CONFIG_OLD_PATH "/home_ui_v17814.cfg"
#define HOME_UI_CONFIG_OLDER_PATH "/home_ui_v17812.cfg"
const char HOME_DEFAULT_KICKER[] = "SMART BUILDING CONTROL";
const char HOME_DEFAULT_TITLE[] = "AHMED MONEM SYSTEM";
const char HOME_DEFAULT_SUBTITLE[] = "Creative smart building dashboard with clear status colors and organized IO cards.";
const char HOME_DEFAULT_BADGE[] = "Device";
char homeHeroKicker[HOME_KICKER_MAX_LEN + 1] = "SMART BUILDING CONTROL";
char homeHeroTitle[HOME_TITLE_MAX_LEN + 1] = "AHMED MONEM SYSTEM";
char homeHeroSubtitle[HOME_SUBTITLE_MAX_LEN + 1] = "Creative smart building dashboard with clear status colors and organized IO cards.";
char homeBadgeText[HOME_BADGE_MAX_LEN + 1] = "Device";
char customBtnName[33] = "LIDO Bulding";
char customDataUrl[129] = "";
byte homeShowWeatherSections = 1;
byte homeShowQuickLinksSection = 1;
byte homeShowAllowedStatusSection = 1;
byte homeShowInputCardsSection = 1;
byte homeShowTechnicalSection = 1;

#define PRAYER_GOV_MAX_LEN 24
#define PRAYER_CACHE_PATH "/prayer_times_v17820.cfg"
#define PRAYER_API_METHOD 5
#define PRAYER_CACHE_REFRESH_MS 21600000UL

struct PrayerGovernorateInfo {
  const char* id;
  const char* groupEn;
  const char* groupAr;
  const char* en;
  const char* ar;
  const char* city;
  const char* lat;
  const char* lon;
};

// V1.7.8.27: one organized area list for Prayer Times, Adhan and Internet Weather.
// Every area has one fixed latitude/longitude pair; the same coordinates are used by both APIs.
const PrayerGovernorateInfo PRAYER_GOVS[] = {
  {"cairo", "Greater Cairo", "القاهرة الكبرى", "Cairo", "القاهرة", "Cairo", "30.0444", "31.2357"},
  {"new_cairo", "Greater Cairo", "القاهرة الكبرى", "New Cairo", "القاهرة الجديدة", "New Cairo", "30.0055", "31.4779"},
  {"tagamoa_5", "Greater Cairo", "القاهرة الكبرى", "5th Settlement", "التجمع الخامس", "5th Settlement", "30.0033", "31.4261"},
  {"madinaty", "Greater Cairo", "القاهرة الكبرى", "Madinaty", "مدينتي", "Madinaty", "30.0967", "31.6625"},
  {"rehab", "Greater Cairo", "القاهرة الكبرى", "Al Rehab", "الرحاب", "Al Rehab City", "30.0671", "31.4896"},
  {"obour", "Greater Cairo", "القاهرة الكبرى", "Obour", "العبور", "Obour City", "30.2283", "31.4799"},

  {"giza", "Giza Governorate", "محافظة الجيزة", "Giza", "الجيزة", "Giza", "30.0131", "31.2089"},
  {"october", "Giza Governorate", "محافظة الجيزة", "6 October", "6 أكتوبر", "6th of October", "29.9285", "30.9188"},
  {"sheikh_zayed", "Giza Governorate", "محافظة الجيزة", "Sheikh Zayed", "الشيخ زايد", "Sheikh Zayed City", "30.0560", "30.9766"},

  {"zagazig", "Sharqia / Monufia", "الشرقية والمنوفية", "Zagazig", "الزقازيق", "Zagazig", "30.5877", "31.5020"},
  {"tenth_ramadan", "Sharqia / Monufia", "الشرقية والمنوفية", "10th of Ramadan", "العاشر من رمضان", "10th of Ramadan City", "30.2927", "31.7423"},
  {"sadat", "Sharqia / Monufia", "الشرقية والمنوفية", "Sadat City", "السادات", "Sadat City", "30.4064", "30.5624"},

  {"ismailia", "Canal Cities", "مدن القناة", "Ismailia", "الإسماعيلية", "Ismailia", "30.5965", "32.2715"},
  {"suez", "Canal Cities", "مدن القناة", "Suez", "السويس", "Suez", "29.9668", "32.5498"},
  {"port_said", "Canal Cities", "مدن القناة", "Port Said", "بورسعيد", "Port Said", "31.2653", "32.3019"},

  {"ras_sedr", "South Sinai", "جنوب سيناء", "Ras Sedr", "رأس سدر", "Ras Sudr", "29.5940", "32.7180"},
  {"el_tor", "South Sinai", "جنوب سيناء", "El Tor", "طور سيناء", "El Tor", "28.2417", "33.6222"},
  {"sharm", "South Sinai", "جنوب سيناء", "Sharm El Sheikh", "شرم الشيخ", "Sharm El Sheikh", "27.9158", "34.3300"},
  {"dahab", "South Sinai", "جنوب سيناء", "Dahab", "دهب", "Dahab", "28.5097", "34.5136"},
  {"nuweiba", "South Sinai", "جنوب سيناء", "Nuweiba", "نويبع", "Nuweiba", "29.0356", "34.6634"},
  {"st_catherine", "South Sinai", "جنوب سيناء", "Saint Catherine", "سانت كاترين", "Saint Catherine", "28.5619", "33.9493"},

  {"alexandria", "Alexandria / North Coast", "الإسكندرية والساحل", "Alexandria", "الإسكندرية", "Alexandria", "31.2001", "29.9187"},
  {"north_coast_marina", "Alexandria / North Coast", "الإسكندرية والساحل", "North Coast - Marina", "الساحل الشمالي - مارينا", "Marina El Alamein", "30.8333", "28.9550"},
  {"new_alamein", "Alexandria / North Coast", "الإسكندرية والساحل", "New Alamein", "العلمين الجديدة", "New Alamein", "30.8481", "28.9093"},
  {"marsa_matruh", "Alexandria / North Coast", "الإسكندرية والساحل", "Marsa Matruh", "مرسى مطروح", "Marsa Matruh", "31.3543", "27.2373"},

  {"mansoura", "Delta", "الدلتا", "Mansoura", "المنصورة", "Mansoura", "31.0409", "31.3785"},
  {"tanta", "Delta", "الدلتا", "Tanta", "طنطا", "Tanta", "30.7865", "31.0004"},
  {"damietta", "Delta", "الدلتا", "Damietta", "دمياط", "Damietta", "31.4165", "31.8133"},
  {"kafr_el_sheikh", "Delta", "الدلتا", "Kafr El Sheikh", "كفر الشيخ", "Kafr El Sheikh", "31.1117", "30.9399"},
  {"damanhur", "Delta", "الدلتا", "Damanhur", "دمنهور", "Damanhur", "31.0341", "30.4682"},

  {"fayoum", "North Upper Egypt", "شمال الصعيد", "Fayoum", "الفيوم", "Fayoum", "29.3073", "30.8404"},
  {"beni_suef", "North Upper Egypt", "شمال الصعيد", "Beni Suef", "بني سويف", "Beni Suef", "29.0661", "31.0994"},
  {"minya", "North Upper Egypt", "شمال الصعيد", "Minya", "المنيا", "Minya", "28.1099", "30.7503"},

  {"assiut", "Upper Egypt", "الصعيد", "Assiut", "أسيوط", "Assiut", "27.1809", "31.1837"},
  {"sohag", "Upper Egypt", "الصعيد", "Sohag", "سوهاج", "Sohag", "26.5569", "31.6948"},
  {"qena", "Upper Egypt", "الصعيد", "Qena", "قنا", "Qena", "26.1551", "32.7160"},
  {"luxor", "Upper Egypt", "الصعيد", "Luxor", "الأقصر", "Luxor", "25.6872", "32.6396"},
  {"aswan", "Upper Egypt", "الصعيد", "Aswan", "أسوان", "Aswan", "24.0889", "32.8998"},

  {"hurghada", "Red Sea", "البحر الأحمر", "Hurghada", "الغردقة", "Hurghada", "27.2579", "33.8116"},
  {"safaga", "Red Sea", "البحر الأحمر", "Safaga", "سفاجا", "Safaga", "26.7491", "33.9389"},
  {"el_quseir", "Red Sea", "البحر الأحمر", "El Quseir", "القصير", "El Quseir", "26.1043", "34.2779"},
  {"marsa_alam", "Red Sea", "البحر الأحمر", "Marsa Alam", "مرسى علم", "Marsa Alam", "25.0676", "34.8789"}
};
const byte PRAYER_GOV_COUNT = sizeof(PRAYER_GOVS) / sizeof(PRAYER_GOVS[0]);
char prayerGovernorateId[PRAYER_GOV_MAX_LEN + 1] = "ras_sedr";

#define ADHAN_SETTINGS_PATH "/adhan_output_v17822.cfg"
#define ADHAN_DEFAULT_GPIO 5
#define ADHAN_CHECK_INTERVAL_MS 20000UL
#define ADHAN_PRAYER_FAJR    0x01
#define ADHAN_PRAYER_DHUHR   0x02
#define ADHAN_PRAYER_ASR     0x04
#define ADHAN_PRAYER_MAGHRIB 0x08
#define ADHAN_PRAYER_ISHA    0x10
#define ADHAN_PRAYER_ALL     0x1F

bool adhanOutputEnabled = false;
byte adhanOutputPin = ADHAN_DEFAULT_GPIO;
byte adhanOutputActiveHigh = 1;
uint16_t adhanOutputDurationSec = 60;
byte adhanPrayerMask = ADHAN_PRAYER_ALL;
bool adhanTelegramNotifyEnabled = true;
byte adhanOutputState = 0;
bool adhanOutputPulseActive = false;
unsigned long adhanOutputOffDueMs = 0;
uint32_t adhanLastExecutionKey = 0;
unsigned long lastAdhanCheckMs = 0;

struct PrayerTimesCache {
  bool valid;
  bool fromCache;
  char dateKey[12];
  char govId[PRAYER_GOV_MAX_LEN + 1];
  char city[32];
  char fajr[8];
  char sunrise[8];
  char dhuhr[8];
  char asr[8];
  char maghrib[8];
  char isha[8];
  char updatedAt[32];
  int lastHttpCode;
  char lastError[56];
};
PrayerTimesCache prayerTimesCache;
unsigned long lastPrayerFetchMs = 0;
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
bool adminLoginNotificationEnabled = true;

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
const unsigned long LOOP_GAP_LOG_MIN_MS = 300000UL; // V1.7.3: do not flood realtime events with repeated ThingSpeak stalls
const unsigned long THINGSPEAK_CLIENT_TIMEOUT_MS = 1500UL; // V1.7.3: shorter network timeout to protect web loop
unsigned long lastApWatchdogMs = 0;
unsigned long lastWebPriorityMs = 0;
unsigned long lastLoopTickMs = 0;
unsigned long maxLoopGapMs = 0;
unsigned long lastLoopGapLogMs = 0;
unsigned long lastSlowTaskLogMs = 0;
unsigned long lastLongTaskMs = 0;
char lastLongTaskName[32] = "None";
byte thingSpeakRemoteCommandStep = 0; // V1.7.3: read one remote command field per slice, not all 4 fields in one loop
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
byte webUserAdhanNotify[WEB_USER_COUNT];
byte webUserStartupNotify[WEB_USER_COUNT];
Preferences userTelegramPrefs;

#define STARTUP_TELEGRAM_REPORT_DELAY_MS 60000UL
#define STARTUP_TELEGRAM_REPORT_RETRY_MS 10000UL
bool startupTelegramReportSent = false;
unsigned long lastStartupTelegramReportTryMs = 0;

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
String outputDisplayLabel(byte outputNumber);
String outputDisplayLabelHtml(byte outputNumber);
bool isThingSpeakCommandOutput(byte outputNumber);
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
void setDefaultUserInputCardPermissions();
void loadUserInputCardPermissions();
void saveUserInputCardPermissions();
bool canUserSeeSmartInputCard(byte userIndex, byte cardIndex);
bool canCurrentUserSeeSmartInputCard(byte cardIndex);
bool userHasAnyVisibleSmartInputCard();

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


struct SmartInputCardConfigV2 {
  uint16_t version;
  uint16_t debounceMs;
  uint16_t delaySec;
  uint16_t minOffSec;
  uint16_t actionDurationSec;
  uint16_t notifyCooldownSec;
  float factor;
  float threshold;
  float offset;
  float hysteresis;
  byte enabled;
  byte mode;
  byte pin;
  byte activeHigh;
  byte telegramNotify;
  byte outputAction;
  byte actionOutput;
  byte offOnClear;
  byte triggerMode;
  byte actionMode;
  byte notifyOnActive;
  byte notifyOnClear;
  byte smoothing;
  char name[SMART_INPUT_NAME_MAX_LEN + 1];
  char unit[SMART_INPUT_UNIT_MAX_LEN + 1];
  char customMsg[SMART_INPUT_MSG_MAX_LEN + 1];
};

struct SmartInputCardConfig {
  uint16_t version;
  uint16_t debounceMs;
  uint16_t delaySec;
  uint16_t minOffSec;
  uint16_t actionDurationSec;
  uint16_t notifyCooldownSec;
  float factor;
  float threshold;
  float offset;
  float hysteresis;
  float sensorTempThreshold;
  float sensorHumThreshold;
  byte enabled;
  byte mode;
  byte pin;
  byte activeHigh;
  byte telegramNotify;
  byte outputAction;
  byte actionOutput;
  byte offOnClear;
  byte triggerMode;
  byte actionMode;
  byte notifyOnActive;
  byte notifyOnClear;
  byte smoothing;
  byte sensorType;
  byte sensorMetric;
  char name[SMART_INPUT_NAME_MAX_LEN + 1];
  char unit[SMART_INPUT_UNIT_MAX_LEN + 1];
  char customMsg[SMART_INPUT_MSG_MAX_LEN + 1];
};

struct SmartInputCardConfigV1 {
  uint16_t version;
  byte enabled;
  byte mode;
  byte pin;
  byte activeHigh;
  byte telegramNotify;
  byte outputAction;
  byte actionOutput;
  byte offOnClear;
  char name[SMART_INPUT_NAME_MAX_LEN + 1];
  float factor;
  float threshold;
};

SmartInputCardConfig smartInputCfg[SMART_INPUT_COUNT];
int smartInputRaw[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
float smartInputValue[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
float smartInputFilteredValue[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
byte smartInputFilterReady[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
byte smartInputActive[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
byte smartInputLastActive[SMART_INPUT_COUNT] = {255,255,255,255,255,255};
byte smartInputStableActive[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
byte smartInputRawActive[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
byte smartInputLastRawActive[SMART_INPUT_COUNT] = {255,255,255,255,255,255};
unsigned long smartInputRawChangeMs[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
unsigned long smartInputActiveStartMs[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
unsigned long smartInputLastOffMs[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
unsigned long smartInputPulseOffMs[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
byte smartInputPulseOutput[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
unsigned long smartInputLastReadMs = 0;
unsigned long smartInputLastNotifyMs[SMART_INPUT_COUNT] = {0,0,0,0,0,0};
uint8_t webUserInputCardVisibleMask[WEB_USER_COUNT];
uint8_t webUserInputCardNotifyMask[WEB_USER_COUNT];
bool smartInputConfigLoaded = false;
bool smartInputLastSaveOk = false;
char smartInputLastAction[64] = "none";
char smartInputLastCardAction[SMART_INPUT_COUNT][64] = {"none","none","none","none","none","none"};


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

String jsQuote(const String& in) {
  String out = "\"";
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\\') out += F("\\\\");
    else if (c == '"') out += F("\\\"");
    else if (c == '\n' || c == '\r') out += ' ';
    else out += c;
  }
  out += "\"";
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
bool activationCodeAlreadyReservedForUser(const String& codeIn, const String& usedBy);
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
byte sanitizeCustomLinkType(int value);
String customLinkTypeName(byte type, bool ar);
String customLinkTypeIcon(byte type);
bool currentUserCanOpenCustomLink(byte linkIndex);
String customLinkLabelDisplayText(byte linkIndex);
String normalizedCustomLinkUrl(byte linkIndex);
void sendCustomLinkPage(byte linkIndex);
void sendCustomLink1Page();
void sendCustomLink2Page();
void sendCustomLink3Page();
String uiSingleLineClean(String value, const String& fallback);
void uiCopySingleLineToChar(String value, char* dest, size_t destLen, const char* fallback);
String normalizedTelegramBotLink();
String homeHeroKickerDisplayText();
String homeHeroTitleDisplayText();
String homeHeroSubtitleDisplayText();
String homeBadgeDisplayText();
void applyDefaultHomeHeaderTexts();
void sendUiSettingsPage();
String telegramBotLinkHomeCardHtml();
String homeExternalLinksCardHtml();
String prayerGovernorateSelectHtml(bool ar);
void setPrayerGovernorateFromId(String id);
String prayerGovernorateName(bool ar);
String prayerGovernorateCity();
String prayerGovernorateLat();
String prayerGovernorateLon();
String prayerWeatherAreaName(bool ar);
String prayerTodayDateKey();
void loadPrayerTimesCacheFromFs();
void savePrayerTimesCacheToFs();
bool ensurePrayerTimesFresh(bool forceRefresh);
void prayerComputeNext(bool ar, String& nextName, uint32_t& secondsLeft);
String prayerFormatDuration(uint32_t sec, bool ar);
bool updateCityWeather(bool force);
String clockStateText();
String prayerTimesTelegramText(const String& chatId, bool forceRefresh);
void sendPrayerTimesPage();
void handlePrayerTimesRefresh();
void loadAdhanOutputConfig();
void saveAdhanOutputConfig();
void setupAdhanOutputPin();
void maintainAdhanOutputAutomation();
void handleTestAdhanOutput();
String adhanOutputSettingsCardHtml(bool ar);
String startupTelegramReportText(const String& chatId);
int telegramNotifyStartupUsers();
void maintainStartupTelegramReport();
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
void setSmartInputDefaults();
void loadSmartInputCardsConfig();
void saveSmartInputCardsConfig();
void applySmartInputPinModes();
void maintainSmartInputCards();
void sendInputCardsPage();
void handleSaveInputCards();
void sendSmartInputStatusJson();
String smartInputStorageStatusText();
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
void sendStorageMaintenancePage();
void handleStorageMaintenanceAction();
void handleDownloadStorageFile();

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
  msg += "\nOutput: "; msg += outputDisplayLabel(outputNumber);
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

  String txt = srcBase + " by " + actionUser + " " + outputDisplayLabel(outputNumber) + " " + (newState ? "ON" : "OFF");
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
  if (outputNumber == 7) return currentOutput7State;
  if (outputNumber == 8) return currentOutput8State;
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
  // V1.7.2: the User Page SPIFFS rescue store is loaded after EEPROM at boot.
  // Therefore any user enable/disable done outside the User Management page
  // must update the rescue store too, otherwise reboot can restore an old
  // disabled/enabled state while activation codes remain correctly reserved.
  saveUserPagePersistentConfig(enabled ? "user-enabled" : "user-disabled");
  autoBackupAfterSave(enabled ? "User enabled" : "User disabled");
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


// Forward declarations for storage health flags used by Telegram health text.
// The real definitions remain in the storage section below.
extern bool eepromBeginOk;
extern bool storageFsBeginOk;

String telegramHealthTextForChat(const String& chatId) {
  bool ar = telegramChatIsArabic(chatId);
  String msg;
  msg.reserve(900);
  msg += ar ? "🛠 صحة النظام" : "🛠 System Health";
  msg += ar ? "\nالوقت: " : "\nTime: "; msg += currentDeviceDateTimeText();
  msg += "\nWiFi: "; msg += WiFi.status() == WL_CONNECTED ? "Connected" : "Offline";
  msg += "\nIP: "; msg += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  msg += "\nRSSI: "; msg += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm" : String("offline");
  msg += ar ? "\nمصدر الوقت: " : "\nTime Source: "; msg += clockStateText();
  msg += "\nRTC: "; msg += rtcStatusText();
  msg += "\nFree Heap: "; msg += String(ESP.getFreeHeap()); msg += " bytes";
  msg += "\nFlash: "; msg += String(ESP.getFlashChipSize()); msg += " bytes";
  msg += "\nUptime: "; msg += String(millis() / 1000UL); msg += " sec";
  msg += "\nMQTT: "; msg += mqttEnabled ? (mqttClient.connected() ? "Connected" : "Enabled / offline") : "Disabled";
  msg += "\nThingSpeak: "; msg += thingSpeakConfigured(true) ? "Configured" : "Not configured";
  msg += "\nTelegram: "; msg += telegramEnabled ? "Enabled" : "Disabled";
  msg += "\nStorage: "; msg += (eepromBeginOk && storageFsBeginOk) ? "OK" : "Check";
  msg += "\nAP Clients: "; msg += String(WiFi.softAPgetStationNum());
  msg += "\nMax Loop Gap: "; msg += String(maxLoopGapMs); msg += " ms";
  msg += "\nLast Slow Task: "; msg += String(lastLongTaskName); msg += " / "; msg += String(lastLongTaskMs); msg += " ms";
  msg += ar ? "\nآخر أمر: " : "\nLast Command: "; msg += lastCommandText;
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
    telegramButtonJson(telegramText(chatId, "🕌 Prayer Times", "🕌 مواقيت الصلاة"), "PRAYER", "primary")
  );
  telegramKeyboardAddRow(k, telegramButtonJson(telegramText(chatId, "🌡️ Temp/Humidity", "🌡️ الحرارة والرطوبة"), "SENSOR", "primary"));
  if (userIndex < WEB_USER_COUNT && WEB_USERS[userIndex].role >= WEB_ROLE_ADMIN) {
    telegramKeyboardAddRow2(k,
      telegramButtonJson(telegramText(chatId, "🔵 Users Management", "🔵 إدارة المستخدمين"), "USERS", "primary"),
      telegramButtonJson(telegramText(chatId, "🔐 Activation Codes", "🔐 أكواد التفعيل"), "CODES", "primary")
    );
    telegramKeyboardAddRow(k, telegramButtonJson(telegramText(chatId, "🛠 System Health", "🛠 صحة النظام"), "HEALTH", "primary"));
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
    msg += webUserAdhanNotify[i] && webUserTelegramChatId[i][0] ? " / Adhan ON" : " / Adhan OFF";
    msg += webUserStartupNotify[i] && webUserTelegramChatId[i][0] ? " / Startup ON" : " / Startup OFF";
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

  if (compact == "PRAYER" || compact == "PRAYERS" || compact == "PRAYERTIMES" || compact == "SALAH" || compact == "AZAN" || compact == "ADHAN" || compact == "مواقيتالصلاة" || compact == "الصلاة") {
    telegramSendToChatWithReplyMarkup(chatId, prayerTimesTelegramText(chatId, false), telegramMainMenuKeyboardJson(authIndex, chatId));
    return;
  }

  if (compact == "HEALTH" || compact == "SYSTEMHEALTH" || compact == "DEVICEHEALTH" || compact == "صحةالنظام") {
    if (authRole < WEB_ROLE_ADMIN) { telegramSendToChat(chatId, telegramText(chatId, "Access denied. Admin only.", "مرفوض. للأدمن فقط.")); return; }
    telegramSendToChatWithReplyMarkup(chatId, telegramHealthTextForChat(chatId), telegramMainMenuKeyboardJson(authIndex, chatId));
    return;
  }

  if (compact == "RESTART" || compact == "REBOOT" || compact == "ESP32RESTART" || compact == "ESP32REBOOT") {
    telegramRestartEsp32FromTelegram(chatId, authIndex);
    return;
  }

  // --- أمر اختبار حارس الهاردوير (للأدمن فقط) ---
  if (compact == "FREEZE" || compact == "TESTWDT") {
    if (authRole < WEB_ROLE_ADMIN) { 
      telegramSendToChat(chatId, "Access denied. Admin only."); 
      return; 
    }
    
    telegramSendToChat(chatId, "❄️ جاري تجميد النظام لاختبار الحارس (Watchdog)...\nإذا كان الحارس يعمل بكفاءة، سيعيد الجهاز تشغيل نفسه إجبارياً بعد 30 ثانية.");
    delay(1000); // إعطاء فرصة لإرسال الرسالة قبل التجميد
    
    while(true) { 
      // حلقة مفرغة متعمدة: المعالج سيعلق هنا ولن يقوم بإطعام الحارس
    }
  }
  // ----------------------------------------------

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
    
    // التعديل هنا: نتحقق من صحة البيانات بغض النظر عن حالة الحساب (مفعل أو موقوف)
    if (findWebUserAnyState(u, text, idx) && idx < WEB_USER_COUNT) {
      
      // حفظ وربط الـ Chat ID فوراً إذا كان فارغاً أو مختلفاً
      if (String(webUserTelegramChatId[idx]) != chatId) {
        setUserTelegramChatId(idx, chatId);
        saveUserTelegramNotifications();
        saveUserPagePersistentConfig("tg-id-linked");
      }

      if (WEB_USERS[idx].enabled) {
        // إذا كان الحساب مفعلاً، أكمل الدخول الطبيعي
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
        
        notifyAdminOnUserLogin(idx); // <-- سطر التنبيه الجديد

        byte daysLeft = 0;
        if (idx > 0 && subscriptionWarningActive(daysLeft)) {
          telegramSendToChat(chatId, subscriptionWarningTextTelegram(chatId, String(WEB_USERS[idx].user), daysLeft));
        }
        telegramSendMainMenu(chatId, idx, telegramText(chatId, "Login successful.", "تم تسجيل الدخول."));
        return;
      } else {
        // إذا كان الحساب موقوفاً، اخرجه واطلب منه الكود (لكن الـ ID تم حفظه خلاص!)
        telegramLogoutSession(slot);
        addEsp32Log(String("Telegram login disabled user: ") + WEB_USERS[idx].user);
        telegramSendLoggedOutMenu(chatId, subscriptionDisabledTextTelegram(chatId, String(WEB_USERS[idx].user)));
        return;
      }
    }

    // إذا كانت الباسورد خطأ
    telegramLogoutSession(slot);
    addEsp32Log(String("Telegram login failed from chat ") + chatId);
    telegramSendLoggedOutMenu(chatId, telegramText(chatId, "Login failed. Wrong username or password.", "فشل تسجيل الدخول. اسم المستخدم أو كلمة المرور خطأ."));
  }
}
void telegramCheckAutoLogin(const String& chatId, int slot) {
  if (slot < 0 || slot >= TELEGRAM_SESSION_COUNT) return;
  
  byte linkedIdx = findWebUserByTelegramChatId(chatId);
  
  // التحقق: هل هذا الـ ID مسجل ومربوط بحساب موجود بالفعل؟
  if (linkedIdx < WEB_USER_COUNT && WEB_USERS[linkedIdx].user[0]) {
    
    if (WEB_USERS[linkedIdx].enabled) {
     // الحساب مفعل: دخول تلقائي
      if (!telegramSessions[slot].loggedIn || telegramSessions[slot].userIndex != linkedIdx) {
        telegramSessions[slot].loggedIn = true;
        telegramSessions[slot].userIndex = linkedIdx;
        telegramSessions[slot].stage = TG_STAGE_IDLE;
        telegramSessions[slot].pendingUser[0] = '\0';
        
        notifyAdminOnUserLogin(linkedIdx); // <-- سطر التنبيه الجديد
      }
    } else {
      // الحساب موقوف (Disabled): إخراجه لمنعه من التحكم
      if (telegramSessions[slot].loggedIn && telegramSessions[slot].userIndex == linkedIdx) {
        telegramSessions[slot].loggedIn = false;
        telegramSessions[slot].stage = TG_STAGE_IDLE;
      }
    }
  }
}

void handleTelegramCallback(const String& chatId, const String& callbackId, const String& messageId, String data) {
  telegramMarkActivePolling();
  maintainMonthlyUserSubscriptionDisable(true);

  data.trim();
  String compact = data;
  compact.toUpperCase();

  int slot = telegramSessionIndexForChat(chatId, true);

  // استدعاء دالة الفحص
  telegramCheckAutoLogin(chatId, slot);

  if (callbackId.length()) telegramAnswerCallback(callbackId, "OK");

  if (compact == "ID") {
    telegramSendToChat(chatId, telegramText(chatId, "Your Telegram Chat ID is:\n", "رقم Telegram Chat ID الخاص بك:\n") + chatId);
    return;
  }

 if (compact == "LOGIN") {
    byte linkedIdx = findWebUserByTelegramChatId(chatId);
    // إذا كان الحساب موجوداً ولكنه موقوف، اعرض له رسالة التفعيل
    if (linkedIdx < WEB_USER_COUNT && WEB_USERS[linkedIdx].user[0] && !WEB_USERS[linkedIdx].enabled) {
      telegramSendLoggedOutMenu(chatId, subscriptionDisabledTextTelegram(chatId, String(WEB_USERS[linkedIdx].user)));
      return;
    }
    // أما إذا كان شخصاً غريباً، اسمح له بالدخول العادي
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

  if (compact == "PRAYER") {
    telegramSendToChatWithReplyMarkup(chatId, prayerTimesTelegramText(chatId, false), telegramMainMenuKeyboardJson(idx, chatId));
    return;
  }

  // V1.7.8.37: handle the inline System Health button callback.
  // The text-command path already accepted HEALTH, but the callback path
  // did not, so pressing the button returned "Unknown button".
  if (compact == "HEALTH" || compact == "SYSTEMHEALTH" || compact == "DEVICEHEALTH") {
    if (WEB_USERS[idx].role < WEB_ROLE_ADMIN) {
      telegramSendMainMenu(chatId, idx, telegramText(chatId, "Access denied. Admin only.", "مرفوض. للأدمن فقط."));
      return;
    }
    telegramSendToChatWithReplyMarkup(chatId, telegramHealthTextForChat(chatId), telegramMainMenuKeyboardJson(idx, chatId));
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
      
    if (idx == 255) { // لو شخص غريب
      notifyAdminOnUnknownUser(chatId, "حاول إدخال كود تفعيل: " + code);
    }
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

 bool activationAccepted = false;
  if (consumeActivationCode(code, String(WEB_USERS[idx].user), String("Telegram"))) {
    activationAccepted = true;
  }
  if (!activationAccepted) {
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

  // استدعاء دالة الفحص
  telegramCheckAutoLogin(chatId, slot);

  String raw = text;
  String rawCompact = raw;
  rawCompact.trim();
  rawCompact.toUpperCase();
  rawCompact.replace(" ", "");
  rawCompact.replace("\r", "");
  rawCompact.replace("\n", "");
  rawCompact.replace("\t", "");

  // V1.7.8.8: Login stage must have priority over generic text handlers.
  // Before this fix, an 8-digit Telegram password could be treated as an
  // activation code, so the bot asked for the password and then did not finish login.
  if (telegramSessions[slot].stage == TG_STAGE_WAIT_USER || telegramSessions[slot].stage == TG_STAGE_WAIT_PASS) {
    if (rawCompact == "/LOGOUT" || rawCompact == "LOGOUT" || rawCompact == "/CANCEL" || rawCompact == "CANCEL") {
      telegramLogoutSession(slot);
      telegramSendLoggedOutMenu(chatId, telegramText(chatId, "Login cancelled.", "تم إلغاء تسجيل الدخول."));
      return;
    }
    telegramHandleLoginText(chatId, slot, text);
    return;
  }

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
    byte linkedIdx = findWebUserByTelegramChatId(chatId);
    if (linkedIdx == 255) { // 255 يعني رقم مش موجود في قاعدة البيانات
      notifyAdminOnUnknownUser(chatId, "ضغط على زر /start");
    }
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

  if (rawCompact == "PRAYER" || rawCompact == "/PRAYER" || rawCompact == "PRAYERTIMES" || rawCompact == "/PRAYERTIMES" || rawCompact == "SALAH" || rawCompact == "AZAN" || rawCompact == "ADHAN" || rawCompact == "مواقيتالصلاة" || rawCompact == "الصلاة") {
    if (telegramSessionLoggedIn(slot)) telegramSendToChatWithReplyMarkup(chatId, prayerTimesTelegramText(chatId, false), telegramMainMenuKeyboardJson(telegramSessions[slot].userIndex, chatId));
    else telegramSendLoggedOutMenu(chatId, telegramText(chatId, "Please login first.", "سجل الدخول أولاً."));
    return;
  }

 if (rawCompact == "/LOGIN" || rawCompact == "LOGIN") {
    byte linkedIdx = findWebUserByTelegramChatId(chatId);
    if (linkedIdx < WEB_USER_COUNT && WEB_USERS[linkedIdx].user[0] && !WEB_USERS[linkedIdx].enabled) {
      telegramSendLoggedOutMenu(chatId, subscriptionDisabledTextTelegram(chatId, String(WEB_USERS[linkedIdx].user)));
      return;
    }
    telegramStartLogin(chatId, slot);
    return;
  }

  if (rawCompact == "/LOGOUT" || rawCompact == "LOGOUT") {
    telegramLogoutSession(slot);
    telegramSendLoggedOutMenu(chatId, telegramText(chatId, "Logged out.", "تم تسجيل الخروج."));
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
    
    notifyAdminOnUserLogin(authIndex); // <-- سطر التنبيه الجديد
    
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

 // التعديل هنا: التنبيه قبل ما يبعت رسالة الرفض
  notifyAdminOnUnknownUser(chatId, "حاول إرسال أمر وهو غير مسجل دخول"); 

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
    telegramSend("Device WiFi disconnected");
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
  String node = String(deviceName[0] ? deviceName : "ahmed_device");
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
    String clientId = String("Device-") + String((uint32_t)ESP.getEfuseMac(), HEX);
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
  String html = htmlHeader("Device Features");
  html += F("<div class='card'><h1>Device Realtime Dashboard</h1><div class='sub'>WebSocket live status + last 50 events.</div><div id='live'>Connecting...</div><pre id='log' style='white-space:pre-wrap;background:#0b1220;padding:10px;border-radius:12px'></pre></div>");
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
  html += F("<div class='sub'>Home header texts and Telegram bot public link moved to the admin-only UI Branding Settings page.</div>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/uisettings"); html += F("'>🎨 UI Branding Settings</a>");
  html += F("<label><input type='checkbox' name='mqtten' value='1' "); if(mqttEnabled) html+=F("checked"); html += F("> MQTT Enable</label><input name='mqtthost' placeholder='MQTT host' value='"); html += htmlEscapeText(String(mqttHost)); html += F("'><input name='mqttport' type='number' value='"); html += mqttPort; html += F("'><input name='mqttuser' placeholder='MQTT user' value='"); html += htmlEscapeText(String(mqttUser)); html += F("'><input name='mqttpass' placeholder='MQTT pass' value='"); html += htmlEscapeText(String(mqttPass)); html += F("'><input name='mqttbase' placeholder='Base topic' value='"); html += htmlEscapeText(String(mqttBaseTopic)); html += F("'>");
  html += F("<label><input type='checkbox' name='tgen' value='1' "); if(telegramEnabled) html+=F("checked"); html += F("> Telegram Enable</label><input name='tgbot' placeholder='Telegram bot token' value='"); html += htmlEscapeText(String(telegramBotToken)); html += F("'><input name='tgchat' placeholder='Telegram chat id' value='"); html += htmlEscapeText(String(telegramChatId)); html += F("'><input name='tghigh' type='number' step='0.1' value='"); html += String(telegramHighTempC,1); html += F("'>");
  html += F("<div class='sub'>Telegram token and chat id control the bot connection. The public bot link shown in UI settings is only for easy opening of the bot.</div>");
  html += F("<h1>Calibration</h1><input name='toff' type='number' step='0.1' value='"); html += String(tempCalibrationOffset,1); html += F("'><input name='hoff' type='number' step='0.1' value='"); html += String(humCalibrationOffset,1); html += F("'>");
  html += F("<h1>External WebSocket</h1><div class='sub'>Internal Device WebSocket port is 81. From outside LAN, router must forward this external TCP port to Device internal TCP port 81.</div><input name='wsport' type='number' min='1' max='65535' value='"); html += String(wsPublicPort); html += F("'><button class='btn'>Save Device Settings</button></form></div>");
  html += F("<script>const WS_PORT="); html += String(wsPublicPort); html += F(";function setLive(t){let e=document.getElementById('live');if(e)e.innerHTML=t;}function connectWs(){let proto=(location.protocol==='https:')?'wss://':'ws://';let url=proto+location.hostname+':'+WS_PORT+'/';setLive('Connecting WebSocket: '+url);let ws=new WebSocket(url);ws.onopen=()=>setLive('WebSocket connected');ws.onmessage=e=>{let d=JSON.parse(e.data);document.getElementById('live').innerHTML='Temp: '+d.temp+' / Hum: '+d.hum+' / '+(d.l1||'O1')+': '+d.o1+' / '+(d.l2||'O2')+': '+d.o2+' / '+(d.l5||'O5')+': '+d.o5+' / '+(d.l6||'O6')+': '+d.o6+' / '+(d.l7||'I7')+': '+d.i7+' / '+(d.l8||'I8')+': '+d.i8+' / '+(d.l9||'I9')+': '+d.i9+' / '+(d.l10||'I10')+': '+d.i10+' / MQTT: '+d.mqtt+' / RSSI: '+d.rssi+' / IP: '+d.ip+'<br>Last Command: '+(d.lastCmd||'--')+'<br>Command User: '+(d.lastCmdUser||'--')+'<br>Command Source: '+(d.lastCmdSource||'--')+'<br>Command Date: '+(d.lastCmdTime||'--');document.getElementById('log').textContent=(d.log||[]).map(x=>(x.time?x.time+' - ':'')+Math.floor(x.ms/1000)+'s - '+x.text).join('\\n');};ws.onerror=()=>setLive('WebSocket error. From outside LAN forward external TCP port '+WS_PORT+' to Device internal port 81.');ws.onclose=()=>setTimeout(connectWs,3000);}connectWs();</script>");
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


void handleSaveHomeUiSettings() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;

  if (webServer.hasArg("resetui")) {
    applyDefaultHomeHeaderTexts();
  } else {
    if (webServer.hasArg("hkicker")) {
      String v = webServer.arg("hkicker");
      uiCopySingleLineToChar(v, homeHeroKicker, sizeof(homeHeroKicker), HOME_DEFAULT_KICKER);
    }
    if (webServer.hasArg("htitle")) {
      String v = webServer.arg("htitle");
      uiCopySingleLineToChar(v, homeHeroTitle, sizeof(homeHeroTitle), HOME_DEFAULT_TITLE);
    }
    if (webServer.hasArg("hsub")) {
      String v = webServer.arg("hsub");
      uiCopySingleLineToChar(v, homeHeroSubtitle, sizeof(homeHeroSubtitle), HOME_DEFAULT_SUBTITLE);
    }
    if (webServer.hasArg("hbadge")) {
      String v = webServer.arg("hbadge");
      uiCopySingleLineToChar(v, homeBadgeText, sizeof(homeBadgeText), HOME_DEFAULT_BADGE);
    }
    if (webServer.hasArg("tglink")) {
      String v = webServer.arg("tglink");
      uiCopySingleLineToChar(v, telegramBotLink, sizeof(telegramBotLink), "");
    }
    for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) {
      String idx = String(k);
      if (webServer.hasArg(String("clabel") + idx)) {
        String v = webServer.arg(String("clabel") + idx);
        uiCopySingleLineToChar(v, customLinkLabel[k], sizeof(customLinkLabel[k]), (k == 0) ? "data" : "Link");
      }
      if (webServer.hasArg(String("curl") + idx)) {
        String v = webServer.arg(String("curl") + idx);
        uiCopySingleLineToChar(v, customLinkUrl[k], sizeof(customLinkUrl[k]), "");
      }
      if (webServer.hasArg(String("ctype") + idx)) {
        customLinkType[k] = sanitizeCustomLinkType(webServer.arg(String("ctype") + idx).toInt());
      }
    }
    if (webServer.hasArg("pgov")) {
      setPrayerGovernorateFromId(webServer.arg("pgov"));
      prayerTimesCache.valid = false;
      lastPrayerFetchMs = 0;
      cityWeatherOk = false;
      lastCityWeatherCheckMs = 0;
      lastCityWeatherMessage = "Area changed - refresh Internet weather";
      lastCityWeatherTime = "--";
    }
    if (webServer.hasArg("homesec")) {
      homeShowWeatherSections = webServer.hasArg("hweather") ? 1 : 0;
      homeShowQuickLinksSection = webServer.hasArg("hlinks") ? 1 : 0;
      homeShowAllowedStatusSection = webServer.hasArg("hallowed") ? 1 : 0;
      homeShowInputCardsSection = webServer.hasArg("hcards") ? 1 : 0;
      homeShowTechnicalSection = webServer.hasArg("htech") ? 1 : 0;
      addEsp32Log("Home sections visibility saved");
    }
    if (webServer.hasArg("adhcfg")) {
      adhanOutputEnabled = webServer.hasArg("adhen");
      int gp = webServer.arg("adhgpio").toInt();
      if (gp < 0 || gp > 33 || gp == 1 || gp == 3 || gp == 6 || gp == 7 || gp == 8 || gp == 9 || gp == 10 || gp == 11 || gp == 18 || gp == 21 || gp == 22 || gp == 25 || gp == 26 || gp == 27 || gp == 32 || gp == 33) gp = ADHAN_DEFAULT_GPIO;
      adhanOutputPin = (byte)gp;
      adhanOutputActiveHigh = webServer.arg("adhlevel").toInt() ? 1 : 0;
      long dur = webServer.arg("adhdur").toInt();
      if (dur < 1) dur = 1;
      if (dur > 3600) dur = 3600;
      adhanOutputDurationSec = (uint16_t)dur;
      byte mask = 0;
      if (webServer.hasArg("adhfajr")) mask |= ADHAN_PRAYER_FAJR;
      if (webServer.hasArg("adhdhuhr")) mask |= ADHAN_PRAYER_DHUHR;
      if (webServer.hasArg("adhasr")) mask |= ADHAN_PRAYER_ASR;
      if (webServer.hasArg("adhmaghrib")) mask |= ADHAN_PRAYER_MAGHRIB;
      if (webServer.hasArg("adhisha")) mask |= ADHAN_PRAYER_ISHA;
      adhanPrayerMask = mask ? mask : ADHAN_PRAYER_ALL;
      adhanTelegramNotifyEnabled = webServer.hasArg("adhtg");
      setupAdhanOutputPin();
      saveAdhanOutputConfig();
      addEsp32Log(String("Adhan output settings saved GPIO") + String(adhanOutputPin));
    }
  }

  saveTelegramUiConfig();
  autoBackupAfterSave("UI branding saved");
  addEsp32Log(String("UI branding saved: title=") + String(homeHeroTitle));
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Location", sessionUrl("/uisettings"), true);
  webServer.send(302, "text/plain", "");
}

void sendUiSettingsPage() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  byte lang = currentWebLanguage();
  bool ar = (lang == TG_LANG_AR);
  String html = htmlHeader(ar ? String("إعدادات عنوان الهوم") : String("UI Branding Settings"));
  html.reserve(7600);
  if (ar) html += F("<div dir='rtl' style='text-align:right'>");

  // --- بداية التعديل: إضافة لوجو المشروع ---
  html += F("<div class='card heroCard'><div class='heroTop'>");
  html += F("<img src='https://raw.githubusercontent.com/ahmedaaa8-bot/esp01-ota/main/lido_icon_256.png' style='width: 55px; height: 55px; border-radius: 8px; margin-inline-end: 15px;'>");
  html += F("<div><div class='heroKicker'>");
  // --- نهاية التعديل ---
  html += homeHeroKickerDisplayText();
  html += F("</div><h1>");
  html += homeHeroTitleDisplayText();
  html += F("</h1><div class='sub heroSub'>");
  html += homeHeroSubtitleDisplayText();
  html += F("</div></div><div class='heroBadge'>");
  html += homeBadgeDisplayText();
  html += F("</div></div></div>");

  html += F("<div class='card'><h1>🎨 ");
  html += ar ? F("إعدادات عنوان الصفحة الرئيسية") : F("Home Header Texts");
  html += F("</h1><div class='sub'>");
  html += ar ? F("غير النصوص الموجودة في كارت العنوان أعلى الهوم. التغييرات محفوظة وتظهر بعد الحفظ.") : F("Edit the text shown in the Home hero header. Changes are saved and shown after saving.");
  html += F("</div><form method='POST' action='/savehomeui'>");
  html += sessionHiddenInput();

  html += F("<label>"); html += ar ? F("السطر الصغير فوق العنوان") : F("Small Header Label"); html += F("</label>");
  html += F("<input maxlength='"); html += String(HOME_KICKER_MAX_LEN); html += F("' name='hkicker' value='"); html += htmlEscapeText(String(homeHeroKicker)); html += F("'>");

  html += F("<label>"); html += ar ? F("العنوان الكبير") : F("Main System Title"); html += F("</label>");
  html += F("<input maxlength='"); html += String(HOME_TITLE_MAX_LEN); html += F("' name='htitle' value='"); html += htmlEscapeText(String(homeHeroTitle)); html += F("'>");

  html += F("<label>"); html += ar ? F("الوصف أسفل العنوان") : F("Subtitle / Description"); html += F("</label>");
  html += F("<input maxlength='"); html += String(HOME_SUBTITLE_MAX_LEN); html += F("' name='hsub' value='"); html += htmlEscapeText(String(homeHeroSubtitle)); html += F("'>");

  html += F("<label>"); html += ar ? F("نص الشارة اليمين") : F("Right Badge Text"); html += F("</label>");
  html += F("<input maxlength='"); html += String(HOME_BADGE_MAX_LEN); html += F("' name='hbadge' value='"); html += htmlEscapeText(String(homeBadgeText)); html += F("'>");

  html += F("<label>"); html += ar ? F("لينك أو اسم بوت تيليجرام") : F("Telegram Bot Link / Username"); html += F("</label>");
  html += F("<input maxlength='120' name='tglink' placeholder='https://t.me/YourBot or YourBot' value='"); html += htmlEscapeText(String(telegramBotLink)); html += F("'>");

  html += F("<div class='smsec'><b>"); html += ar ? F("أزرار الروابط المخصصة") : F("Custom Quick Link Buttons"); html += F("</b><div class='sub'>");
  html += ar ? F("كل زر يمكن أن يعرض CSV كجدول، صورة، ملف نصي، أو PDF. يفضل روابط GitHub Raw المباشرة.") : F("Each button can display CSV as a table, Image, Text, or PDF. Direct GitHub Raw links are recommended.");
  html += F("</div>");
  for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) {
    html += F("<div class='smsec'><b>Link "); html += String(k + 1); html += F("</b>");
    html += F("<label>"); html += ar ? F("اسم الزر") : F("Button Name"); html += F("</label>");
    html += F("<input maxlength='"); html += String(CUSTOM_LINK_LABEL_MAX_LEN); html += F("' name='clabel"); html += String(k); html += F("' value='"); html += htmlEscapeText(String(customLinkLabel[k])); html += F("' placeholder='"); html += (k == 0 ? F("data") : F("Link")); html += F("'>");
    html += F("<label>"); html += ar ? F("نوع العرض") : F("Display Type"); html += F("</label><select name='ctype"); html += String(k); html += F("'>");
    for (byte t = 0; t <= CUSTOM_LINK_TYPE_PDF; t++) {
      html += F("<option value='"); html += String(t); html += F("'");
      if (customLinkType[k] == t) html += F(" selected");
      html += F(">"); html += customLinkTypeName(t, ar); html += F("</option>");
    }
    html += F("</select>");
    html += F("<label>URL</label>");
    html += F("<input maxlength='"); html += String(CUSTOM_LINK_URL_MAX_LEN); html += F("' name='curl"); html += String(k); html += F("' value='"); html += htmlEscapeText(String(customLinkUrl[k])); html += F("' placeholder='https://raw.githubusercontent.com/user/repo/main/file.csv'>");
    html += F("</div>");
    yield();
  }
  html += F("</div>");

  html += F("<button class='btn' type='submit'>"); html += ar ? F("حفظ الإعدادات") : F("Save Settings"); html += F("</button>");
  html += F("<button class='btn btn2' type='submit' name='resetui' value='1'>");
  html += ar ? F("استرجاع نصوص الهيدر الافتراضية") : F("Restore Default Header Texts");
  html += F("</button></form></div>");

  html += F("<div class='card'><h1>🕌 ");
  html += ar ? F("إعدادات المنطقة الواحدة") : F("Unified Area Settings");
  html += F("</h1><div class='sub'>");
  html += ar ? F("اختيار واحد فقط يستخدم لمواقيت الصلاة والأذان وطقس الإنترنت في صفحة الهوم.") : F("One admin-selected area is used for Prayer Times, Adhan and Internet Weather on Home.");
  html += F("</div><form method='POST' action='/savehomeui'>");
  html += sessionHiddenInput();
  html += F("<label>"); html += ar ? F("المدينة / المنطقة") : F("City / Area"); html += F("</label>");
  html += prayerGovernorateSelectHtml(ar);
  html += F("<div class='sub'>"); html += ar ? F("نفس الاختيار يستخدم في مواعيد الصلاة، خرج الأذان، ودرجة الحرارة/الرطوبة من الإنترنت.") : F("The same selection is used for prayer times, Adhan output, and Internet temperature/humidity."); html += F("</div>");
  html += F("<button class='btn' type='submit'>"); html += ar ? F("حفظ المنطقة") : F("Save Area"); html += F("</button></form></div>");

  html += adhanOutputSettingsCardHtml(ar);

  html += F("<div class='card'><h1>🏠 ");
  html += ar ? F("أقسام الصفحة الرئيسية") : F("Home Sections");
  html += F("</h1><div class='sub'>");
  html += ar ? F("اختر الأقسام التي تظهر في الهوم. هذا يجعل الصفحة أخف وأنظف على الموبايل.") : F("Choose which sections appear on Home. This keeps the page lighter and cleaner on mobile.");
  html += F("</div><form method='POST' action='/savehomeui'>");
  html += sessionHiddenInput();
  html += F("<input type='hidden' name='homesec' value='1'>");
  html += F("<label><input type='checkbox' name='hweather' value='1' "); if (homeShowWeatherSections) html += F("checked"); html += F("> "); html += ar ? F("إظهار كروت الحساس والطقس") : F("Show Sensor & Internet Weather cards"); html += F("</label>");
  html += F("<label><input type='checkbox' name='hlinks' value='1' "); if (homeShowQuickLinksSection) html += F("checked"); html += F("> "); html += ar ? F("إظهار الروابط السريعة") : F("Show Quick Links"); html += F("</label>");
  html += F("<label><input type='checkbox' name='hallowed' value='1' "); if (homeShowAllowedStatusSection) html += F("checked"); html += F("> "); html += ar ? F("إظهار الحالات المصرح بها") : F("Show Allowed Status"); html += F("</label>");
  html += F("<label><input type='checkbox' name='hcards' value='1' "); if (homeShowInputCardsSection) html += F("checked"); html += F("> "); html += ar ? F("إظهار كروت المداخل") : F("Show Input Cards"); html += F("</label>");
  html += F("<label><input type='checkbox' name='htech' value='1' "); if (homeShowTechnicalSection) html += F("checked"); html += F("> "); html += ar ? F("إظهار الحالة الفنية للأدمن") : F("Show Admin Technical Status"); html += F("</label>");
  html += F("<button class='btn' type='submit'>"); html += ar ? F("حفظ أقسام الهوم") : F("Save Home Sections"); html += F("</button></form></div>");

  html += F("<div class='card'><h1>🤖 ");
  html += ar ? F("معلومة التليجرام") : F("Telegram Note");
  html += F("</h1><div class='sub'>");
  html += ar ? F("هذه الخانة تحفظ رابط/اسم البوت للعرض وسهولة الفتح فقط. توكن البوت ورقم الشات يظلان في صفحة Device Realtime / MQTT / Telegram.") : F("This field stores only the public bot link/username for easy opening. Bot token and chat id remain in Device Realtime / MQTT / Telegram.");
  html += F("</div>");
  String link = normalizedTelegramBotLink();
  if (link.length()) { html += F("<a class='btn btn2' target='_blank' rel='noopener' href='"); html += htmlEscapeText(link); html += F("'>"); html += ar ? F("فتح بوت تيليجرام") : F("Open Telegram Bot"); html += F("</a>"); }
  html += F("</div>");

  html += F("<div class='card'><a class='btn btn2' href='"); html += sessionUrl("/"); html += F("'>"); html += ar ? F("رجوع للهوم") : F("Back to Home"); html += F("</a></div>");
  if (ar) html += F("</div>");
  html += htmlFooter();
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.send(200, "text/html", html);
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
#define STORAGE_FS_META_PATH   "/eeprom_image_v1784.meta"
#define STORAGE_FS_META_MAGIC  0x53424645UL  // EBFS - EEPROM Backup File SafeBoot
#define STORAGE_FS_META_VERSION 1

struct StorageFsImageMeta {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t eepromSize;
  uint32_t mapVersion;
  uint32_t checksum;
};


// V1.6.7C: Dedicated rescue store for the whole User Management page.
// This keeps user/password/permissions/Telegram IDs/subscription options/login intro persistent
// even when EEPROM/NVS commit paths are unreliable after flash/partition changes.
#define USER_PAGE_STORE_PATH    "/user_page_v167.bin"
#define USER_PAGE_STORE_MAGIC   0x55504731UL  // UPG1
#define USER_PAGE_STORE_VERSION 4

// V1 format is kept for safe migration from older user-store files.
struct UserPagePersistentRecordV1 {
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

// V2 adds Smart Input Card user permissions to the User Management rescue store.
// This is kept for migration from the previous rescue-store format.
struct UserPagePersistentRecordV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t checksum;
  WebUserAccount users[WEB_USER_COUNT];
  uint16_t visibleMask[WEB_USER_COUNT];
  uint16_t controlMask[WEB_USER_COUNT];
  char telegramChatId[WEB_USER_COUNT][USER_TELEGRAM_CHAT_ID_MAX_LEN + 1];
  byte telegramNotify[WEB_USER_COUNT];
  uint8_t inputCardVisibleMask[WEB_USER_COUNT];
  uint8_t inputCardNotifyMask[WEB_USER_COUNT];
  uint32_t lastDisableYm;
  byte monthlyAuto;
  byte codeExpiry;
  byte telegramSmart;
  byte introEnabled;
  uint16_t introLen;
  char introText[LOGIN_INTRO_MAX_LEN + 1];
};

// V3 adds per-user Adhan Notify to the same rescue store so the Users page
// checkbox survives reboot/EEPROM rescue even if NVS is unreliable.
struct UserPagePersistentRecordV3 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t checksum;
  WebUserAccount users[WEB_USER_COUNT];
  uint16_t visibleMask[WEB_USER_COUNT];
  uint16_t controlMask[WEB_USER_COUNT];
  char telegramChatId[WEB_USER_COUNT][USER_TELEGRAM_CHAT_ID_MAX_LEN + 1];
  byte telegramNotify[WEB_USER_COUNT];
  byte adhanNotify[WEB_USER_COUNT];
  uint8_t inputCardVisibleMask[WEB_USER_COUNT];
  uint8_t inputCardNotifyMask[WEB_USER_COUNT];
  uint32_t lastDisableYm;
  byte monthlyAuto;
  byte codeExpiry;
  byte telegramSmart;
  byte introEnabled;
  uint16_t introLen;
  char introText[LOGIN_INTRO_MAX_LEN + 1];
};

// V4 adds per-user Startup Telegram Report selection.
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
  byte adhanNotify[WEB_USER_COUNT];
  byte startupNotify[WEB_USER_COUNT];
  uint8_t inputCardVisibleMask[WEB_USER_COUNT];
  uint8_t inputCardNotifyMask[WEB_USER_COUNT];
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
#define AUTO_BACKUP_TMP_LOG     "/restore_log_tmp.bin"
#define AUTO_BACKUP_UPLOAD_TMP  "/restore_auto_upload.bin"
#define AUTO_BACKUP_FORMAT_VER  3
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

uint32_t storageSafeFnvUpdate(uint32_t hash, uint8_t b) {
  hash ^= b;
  hash *= 16777619UL;
  return hash;
}

uint32_t storageSafeFileChecksum(File& f) {
  uint32_t h = 2166136261UL;
  uint8_t buf[128];
  f.seek(0);
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; i++) h = storageSafeFnvUpdate(h, buf[i]);
    yield();
  }
  f.seek(0);
  return h;
}

bool storageSafeReadMetaBytes(uint8_t* metaBytes, size_t metaLen) {
  if (!metaBytes || metaLen == 0) return false;
  memset(metaBytes, 0, metaLen);
  if (!storageFsBeginOk || !SPIFFS.exists(STORAGE_FS_META_PATH)) return false;
  File mf = SPIFFS.open(STORAGE_FS_META_PATH, FILE_READ);
  if (!mf) return false;
  if (mf.size() != (size_t)metaLen) {
    mf.close();
    return false;
  }
  int rd = mf.read(metaBytes, metaLen);
  mf.close();
  return rd == (int)metaLen;
}

bool storageSafeMetaValidBytes(const uint8_t* metaBytes, size_t metaLen, uint32_t checksum) {
  if (!metaBytes || metaLen != sizeof(StorageFsImageMeta)) return false;
  const StorageFsImageMeta* meta = (const StorageFsImageMeta*)metaBytes;
  if (meta->magic != STORAGE_FS_META_MAGIC) return false;
  if (meta->version != STORAGE_FS_META_VERSION) return false;
  if (meta->size != sizeof(StorageFsImageMeta)) return false;
  if (meta->eepromSize != EEPROM_SIZE) return false;
  if (meta->mapVersion != EEPROM_BACKUP_MAP_VERSION) return false;
  if (meta->checksum != checksum) return false;
  return true;
}

byte storageSafeLegacyMagicScore(File& f) {
  byte score = 0;
  int checks[][2] = {
    {EEPROM_WIFI_MAGIC_ADDR, EEPROM_WIFI_MAGIC},
    {EEPROM_DEV_MAGIC_ADDR, EEPROM_DEV_MAGIC},
    {EEPROM_USER_MAGIC_ADDR, EEPROM_USER_MAGIC},
    {EEPROM_TS_MAGIC_ADDR, EEPROM_TS_MAGIC},
    {EEPROM_STATIC_MAGIC_ADDR, EEPROM_STATIC_MAGIC},
    {EEPROM_DUCK_MAGIC_ADDR, EEPROM_DUCK_MAGIC},
    {EEPROM_SMART_MAGIC_ADDR, EEPROM_SMART_MAGIC},
    {EEPROM_FIELD_LABEL_MAGIC_ADDR, EEPROM_FIELD_LABEL_MAGIC},
    {EEPROM_USERPERM_MAGIC_ADDR, EEPROM_USERPERM_MAGIC},
    {EEPROM_OUTPUT_AUTO_MAGIC_ADDR, EEPROM_OUTPUT_AUTO_MAGIC},
    {EEPROM_MANUAL_AUTO_MAGIC_ADDR, EEPROM_MANUAL_AUTO_MAGIC},
    {EEPROM_SMART_INPUT_MAGIC_ADDR, EEPROM_SMART_INPUT_MAGIC},
    {EEPROM_INPUT_CARD_PERM_MAGIC_ADDR, EEPROM_INPUT_CARD_PERM_MAGIC}
  };
  for (byte i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
    f.seek(checks[i][0]);
    int c = f.read();
    if (c == checks[i][1]) score++;
  }
  f.seek(0);
  return score;
}

bool storageSafeImageLooksBlank(File& f) {
  bool all00 = true;
  bool allFF = true;
  uint8_t buf[128];
  f.seek(0);
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      if (buf[i] != 0x00) all00 = false;
      if (buf[i] != 0xFF) allFF = false;
      if (!all00 && !allFF) { f.seek(0); return false; }
    }
    yield();
  }
  f.seek(0);
  return all00 || allFF;
}

bool storageSafeBackupFileAllowed(File& f, uint32_t checksum) {
  StorageFsImageMeta meta;
  bool hasMeta = storageSafeReadMetaBytes((uint8_t*)&meta, sizeof(meta));
  if (hasMeta) {
    if (storageSafeMetaValidBytes((const uint8_t*)&meta, sizeof(meta), checksum)) return true;
    setStorageFsError("safe boot: bad meta/checksum");
    return false;
  }

  // Legacy rescue image without meta. Load only if it is clearly a real project image.
  // This prevents a corrupted/random 4096-byte file from overriding EEPROM RAM and blocking AP boot.
  if (storageSafeImageLooksBlank(f)) {
    setStorageFsError("safe boot: blank legacy image");
    return false;
  }
  byte score = storageSafeLegacyMagicScore(f);
  if (score == 0) {
    setStorageFsError("safe boot: legacy image rejected");
    return false;
  }
  setStorageFsError("legacy image accepted");
  return true;
}

bool saveStorageFsMeta(uint32_t checksum) {
  if (!storageFsBeginOk) return false;
  StorageFsImageMeta meta;
  memset(&meta, 0, sizeof(meta));
  meta.magic = STORAGE_FS_META_MAGIC;
  meta.version = STORAGE_FS_META_VERSION;
  meta.size = sizeof(StorageFsImageMeta);
  meta.eepromSize = EEPROM_SIZE;
  meta.mapVersion = EEPROM_BACKUP_MAP_VERSION;
  meta.checksum = checksum;
  File mf = SPIFFS.open(STORAGE_FS_META_PATH, FILE_WRITE);
  if (!mf) return false;
  size_t w = mf.write((const uint8_t*)&meta, sizeof(meta));
  mf.flush();
  mf.close();
  return w == sizeof(meta);
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

  uint32_t checksum = 2166136261UL;
  uint8_t buf[128];
  for (int off = 0; off < EEPROM_SIZE; off += (int)sizeof(buf)) {
    int n = EEPROM_SIZE - off;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    for (int i = 0; i < n; i++) {
      buf[i] = EEPROM.read(off + i);
      checksum = storageSafeFnvUpdate(checksum, buf[i]);
    }
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
  if (!saveStorageFsMeta(checksum)) {
    setStorageFsError("meta save failed");
    return false;
  }
  storageFsLastSaveOk = true;
  storageFsSaveCount++;
  setStorageFsError("none");
  DBG_PRINTLN("SPIFFS EEPROM image save: OK + meta");
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

  uint32_t checksum = storageSafeFileChecksum(f);
  if (!storageSafeBackupFileAllowed(f, checksum)) {
    f.close();
    storageFsLoadedAtBoot = false;
    DBG_PRINT("SPIFFS EEPROM image ignored: "); DBG_PRINTLN(storageFsLastError);
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
  DBG_PRINTLN("SPIFFS EEPROM image load at boot: YES safe");
  return true;
}

void removeStorageFsBackup() {
  if (storageFsBeginOk && SPIFFS.exists(STORAGE_FS_BACKUP_PATH)) {
    SPIFFS.remove(STORAGE_FS_BACKUP_PATH);
  }
  if (storageFsBeginOk && SPIFFS.exists(STORAGE_FS_META_PATH)) {
    SPIFFS.remove(STORAGE_FS_META_PATH);
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
  for (uint16_t i = 0; i < EEPROM_SIZE; i++) {
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
  setRelay(OUTPUT7_PIN, currentOutput7State);
  setRelay(OUTPUT8_PIN, currentOutput8State);

  DBG_PRINTLN("==== APPLY OUTPUTS ====");
  DBG_PRINT("Output1 GPIO"); DBG_PRINT(OUTPUT1_PIN); DBG_PRINT(" = "); DBG_PRINTLN(currentOutput1State ? "ON" : "OFF");
  DBG_PRINT("Output2 GPIO"); DBG_PRINT(OUTPUT2_PIN); DBG_PRINT(" = "); DBG_PRINTLN(currentOutput2State ? "ON" : "OFF");
  DBG_PRINT("Output5 GPIO"); DBG_PRINT(OUTPUT5_PIN); DBG_PRINT(" = "); DBG_PRINTLN(currentOutput5State ? "ON" : "OFF");
  DBG_PRINT("Output6 GPIO"); DBG_PRINT(OUTPUT6_PIN); DBG_PRINT(" = "); DBG_PRINTLN(currentOutput6State ? "ON" : "OFF");
  DBG_PRINT("Output7 GPIO"); DBG_PRINT(OUTPUT7_PIN); DBG_PRINT(" = "); DBG_PRINTLN(currentOutput7State ? "ON" : "OFF");
  DBG_PRINT("Output8 GPIO"); DBG_PRINT(OUTPUT8_PIN); DBG_PRINT(" = "); DBG_PRINTLN(currentOutput8State ? "ON" : "OFF");
  DBG_PRINTLN("=======================");
}

void saveOutputState(byte outputNumber, byte state) {
  int addr = EEPROM_OUTPUT1_ADDR;
  if (outputNumber == 1) addr = EEPROM_OUTPUT1_ADDR;
  else if (outputNumber == 2) addr = EEPROM_OUTPUT2_ADDR;
  else if (outputNumber == 5) addr = EEPROM_OUTPUT5_ADDR;
  else if (outputNumber == 6) addr = EEPROM_OUTPUT6_ADDR;
  else if (outputNumber == 7) addr = EEPROM_OUTPUT7_ADDR;
  else if (outputNumber == 8) addr = EEPROM_OUTPUT8_ADDR;
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
  byte rawOutput7 = EEPROM.read(EEPROM_OUTPUT7_ADDR);
  byte rawOutput8 = EEPROM.read(EEPROM_OUTPUT8_ADDR);

  DBG_PRINT("Raw EEPROM Output1 @addr "); DBG_PRINT(EEPROM_OUTPUT1_ADDR); DBG_PRINT(" = "); DBG_PRINTLN(rawOutput1);
  DBG_PRINT("Raw EEPROM Output2 @addr "); DBG_PRINT(EEPROM_OUTPUT2_ADDR); DBG_PRINT(" = "); DBG_PRINTLN(rawOutput2);
  DBG_PRINT("Raw EEPROM Output5 @addr "); DBG_PRINT(EEPROM_OUTPUT5_ADDR); DBG_PRINT(" = "); DBG_PRINTLN(rawOutput5);
  DBG_PRINT("Raw EEPROM Output6 @addr "); DBG_PRINT(EEPROM_OUTPUT6_ADDR); DBG_PRINT(" = "); DBG_PRINTLN(rawOutput6);
  DBG_PRINT("Raw EEPROM Output7 @addr "); DBG_PRINT(EEPROM_OUTPUT7_ADDR); DBG_PRINT(" = "); DBG_PRINTLN(rawOutput7);
  DBG_PRINT("Raw EEPROM Output8 @addr "); DBG_PRINT(EEPROM_OUTPUT8_ADDR); DBG_PRINT(" = "); DBG_PRINTLN(rawOutput8);

  currentOutput1State = sanitizeState(rawOutput1, 0);
  currentOutput2State = sanitizeState(rawOutput2, 0);
  currentOutput5State = sanitizeState(rawOutput5, 0);
  currentOutput6State = sanitizeState(rawOutput6, 0);
  currentOutput7State = sanitizeState(rawOutput7, 0);
  currentOutput8State = sanitizeState(rawOutput8, 0);

  if (rawOutput1 != currentOutput1State) DBG_PRINTLN("Output1 EEPROM value invalid -> repaired to 0");
  if (rawOutput2 != currentOutput2State) DBG_PRINTLN("Output2 EEPROM value invalid -> repaired to 0");
  if (rawOutput5 != currentOutput5State) DBG_PRINTLN("Output5 EEPROM value invalid -> repaired to 0");
  if (rawOutput6 != currentOutput6State) DBG_PRINTLN("Output6 EEPROM value invalid -> repaired to 0");
  if (rawOutput7 != currentOutput7State) DBG_PRINTLN("Output7 EEPROM value invalid -> repaired to 0");
  if (rawOutput8 != currentOutput8State) DBG_PRINTLN("Output8 EEPROM value invalid -> repaired to 0");

  DBG_PRINT("Loaded Output1 state = "); DBG_PRINTLN(currentOutput1State ? "ON" : "OFF");
  DBG_PRINT("Loaded Output2 state = "); DBG_PRINTLN(currentOutput2State ? "ON" : "OFF");
  DBG_PRINT("Loaded Output5 state = "); DBG_PRINTLN(currentOutput5State ? "ON" : "OFF");
  DBG_PRINT("Loaded Output6 state = "); DBG_PRINTLN(currentOutput6State ? "ON" : "OFF");
  DBG_PRINT("Loaded Output7 state = "); DBG_PRINTLN(currentOutput7State ? "ON" : "OFF");
  DBG_PRINT("Loaded Output8 state = "); DBG_PRINTLN(currentOutput8State ? "ON" : "OFF");

  saveOutputState(1, currentOutput1State);
  saveOutputState(2, currentOutput2State);
  saveOutputState(5, currentOutput5State);
  saveOutputState(6, currentOutput6State);
  saveOutputState(7, currentOutput7State);
  saveOutputState(8, currentOutput8State);

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

    if (mqttBaseTopic[0] == '\0') strncpy(mqttBaseTopic, "ahmed/device", sizeof(mqttBaseTopic) - 1);
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
    strncpy(mqttBaseTopic, "ahmed/device", sizeof(mqttBaseTopic) - 1);
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
  if (mqttBaseTopic[0] == '\0') strncpy(mqttBaseTopic, "ahmed/device", sizeof(mqttBaseTopic) - 1);
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
  strncpy(mqttBaseTopic, "ahmed/device", sizeof(mqttBaseTopic) - 1);
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

String outputDisplayLabel(byte outputNumber) {
  if (outputNumber == 7) return String("Aux Output 7");
  if (outputNumber == 8) return String("Aux Output 8");
  return fieldLabel(outputNumber);
}

String outputDisplayLabelHtml(byte outputNumber) {
  return htmlEscapeText(outputDisplayLabel(outputNumber));
}

bool isThingSpeakCommandOutput(byte outputNumber) {
  return (outputNumber == 1 || outputNumber == 2 || outputNumber == 5 || outputNumber == 6);
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
  return canCurrentUserSeeField(1) || canCurrentUserSeeField(2) || canCurrentUserSeeField(5) || canCurrentUserSeeField(6) || canCurrentUserSeeField(7) || canCurrentUserSeeField(8) || canCurrentUserSeeField(9) || canCurrentUserSeeField(10) || roleAtLeast(WEB_ROLE_ENGINEER) || userHasAnyVisibleSmartInputCard();
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
  memset(webUserAdhanNotify, 0, sizeof(webUserAdhanNotify));
  memset(webUserStartupNotify, 0, sizeof(webUserStartupNotify));
  // Keep old single-admin notification behavior on first upgrade if a global Chat ID already exists.
  if (telegramChatId[0]) {
    String id = String(telegramChatId);
    if (isSafeTelegramChatIdText(id)) {
      setUserTelegramChatId(0, id);
      webUserTelegramNotify[0] = 1;
      webUserStartupNotify[0] = 1;
    }
  }
}

void saveUserTelegramNotifications() {
  if (!userTelegramPrefs.begin("user_tg", false)) return;
  userTelegramPrefs.putUChar("magic", USER_TELEGRAM_NVS_MAGIC);
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    char keyId[8];
    char keyEn[8];
    char keyAdh[8];
    char keySt[8];
    snprintf(keyId, sizeof(keyId), "id%u", i);
    snprintf(keyEn, sizeof(keyEn), "en%u", i);
    snprintf(keyAdh, sizeof(keyAdh), "adh%u", i);
    snprintf(keySt, sizeof(keySt), "st%u", i);
    userTelegramPrefs.putString(keyId, String(webUserTelegramChatId[i]));
    userTelegramPrefs.putUChar(keyEn, webUserTelegramNotify[i] ? 1 : 0);
    userTelegramPrefs.putUChar(keyAdh, webUserAdhanNotify[i] ? 1 : 0);
    userTelegramPrefs.putUChar(keySt, webUserStartupNotify[i] ? 1 : 0);
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
    char keyAdh[8];
    char keySt[8];
    snprintf(keyId, sizeof(keyId), "id%u", i);
    snprintf(keyEn, sizeof(keyEn), "en%u", i);
    snprintf(keyAdh, sizeof(keyAdh), "adh%u", i);
    snprintf(keySt, sizeof(keySt), "st%u", i);
    webUserTelegramNotify[i] = userTelegramPrefs.getUChar(keyEn, 0) == 1 ? 1 : 0;
    webUserAdhanNotify[i] = userTelegramPrefs.getUChar(keyAdh, 0) == 1 ? 1 : 0;
    webUserStartupNotify[i] = userTelegramPrefs.getUChar(keySt, 0) == 1 ? 1 : 0;
    String id = userTelegramPrefs.getString(keyId, "");
    if (isSafeTelegramChatIdText(id)) setUserTelegramChatId(i, id);
    else webUserTelegramChatId[i][0] = 0;
  }
  userTelegramPrefs.end();
}


void setDefaultUserInputCardPermissions() {
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    webUserInputCardVisibleMask[i] = SMART_INPUT_ALL_MASK;
    webUserInputCardNotifyMask[i] = SMART_INPUT_ALL_MASK;
  }
  webUserInputCardVisibleMask[0] = SMART_INPUT_ALL_MASK;
  webUserInputCardNotifyMask[0] = SMART_INPUT_ALL_MASK;
}

uint8_t sanitizeInputCardMask(uint8_t m) {
  return (uint8_t)(m & SMART_INPUT_ALL_MASK);
}

void loadUserInputCardPermissions() {
  uint16_t baseAddr = EEPROM_INPUT_CARD_PERM_DATA_ADDR;
  bool loaded = false;

  if (EEPROM.read(EEPROM_INPUT_CARD_PERM_MAGIC_ADDR) == EEPROM_INPUT_CARD_PERM_MAGIC) {
    baseAddr = EEPROM_INPUT_CARD_PERM_DATA_ADDR;
    loaded = true;
  } else if (EEPROM.read(EEPROM_INPUT_CARD_PERM_OLD_MAGIC_ADDR) == EEPROM_INPUT_CARD_PERM_MAGIC) {
    // One-time migration from old overlapping location. Values are sanitized before use.
    baseAddr = EEPROM_INPUT_CARD_PERM_OLD_DATA_ADDR;
    loaded = true;
  }

  if (!loaded) {
    setDefaultUserInputCardPermissions();
    saveUserInputCardPermissions();
    return;
  }

  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    webUserInputCardVisibleMask[i] = sanitizeInputCardMask(EEPROM.read(baseAddr + i));
    webUserInputCardNotifyMask[i] = sanitizeInputCardMask(EEPROM.read(baseAddr + WEB_USER_COUNT + i));
  }
  webUserInputCardVisibleMask[0] = SMART_INPUT_ALL_MASK;
  webUserInputCardNotifyMask[0] = SMART_INPUT_ALL_MASK;

  // If loaded from old overlapping location, immediately write to the new safe block.
  if (baseAddr == EEPROM_INPUT_CARD_PERM_OLD_DATA_ADDR) saveUserInputCardPermissions();
}

void saveUserInputCardPermissions() {
  EEPROM.write(EEPROM_INPUT_CARD_PERM_MAGIC_ADDR, EEPROM_INPUT_CARD_PERM_MAGIC);
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    EEPROM.write(EEPROM_INPUT_CARD_PERM_DATA_ADDR + i, sanitizeInputCardMask(webUserInputCardVisibleMask[i]));
    EEPROM.write(EEPROM_INPUT_CARD_PERM_DATA_ADDR + WEB_USER_COUNT + i, sanitizeInputCardMask(webUserInputCardNotifyMask[i]));
  }
  commitEeprom("input-card-permissions");
}

bool canUserSeeSmartInputCard(byte userIndex, byte cardIndex) {
  if (userIndex >= WEB_USER_COUNT || cardIndex >= SMART_INPUT_COUNT) return false;
  if (WEB_USERS[userIndex].role >= WEB_ROLE_ADMIN) return true;
  return (webUserInputCardVisibleMask[userIndex] & (1U << cardIndex)) != 0;
}

bool canUserReceiveSmartInputNotify(byte userIndex, byte cardIndex) {
  if (!canUserSeeSmartInputCard(userIndex, cardIndex)) return false;
  if (WEB_USERS[userIndex].role >= WEB_ROLE_ADMIN) return true;
  return (webUserInputCardNotifyMask[userIndex] & (1U << cardIndex)) != 0;
}

bool canCurrentUserSeeSmartInputCard(byte cardIndex) {
  if (currentWebRole >= WEB_ROLE_ADMIN) return true;
  return canUserSeeSmartInputCard(currentWebUserIndex, cardIndex);
}

bool userHasAnyVisibleSmartInputCard() {
  if (currentWebRole >= WEB_ROLE_ENGINEER) return true;
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) {
    if (canCurrentUserSeeSmartInputCard(i)) return true;
  }
  return false;
}

String inputCardPermissionCheck(byte userIndex, byte cardIndex, bool notify) {
  String name = notify ? String("icn") : String("icv");
  name += String(userIndex); name += F("_"); name += String(cardIndex);
  bool checked = notify ? canUserReceiveSmartInputNotify(userIndex, cardIndex) : canUserSeeSmartInputCard(userIndex, cardIndex);
  String html = F("<label class='small'><input type='checkbox' name='");
  html += name;
  html += F("' ");
  if (checked) html += F("checked ");
  if (userIndex == 0) html += F("disabled ");
  html += F("> Input Card "); html += String(cardIndex + 1); html += F("</label>");
  return html;
}

String userInputCardPermissionsHtml(byte userIndex) {
  String html;
  html.reserve(900);
  html += F("<div class='smsec'><b>Input Card Permissions</b><div class='sub'>Visible = user can see the card. Notify = user can receive Telegram alerts for that card.</div>");
  html += F("<div class='small'>Visible input cards</div><div class='grid2'>");
  for (byte k = 0; k < SMART_INPUT_COUNT; k++) html += inputCardPermissionCheck(userIndex, k, false);
  html += F("</div><div class='small'>Telegram notifications</div><div class='grid2'>");
  for (byte k = 0; k < SMART_INPUT_COUNT; k++) html += inputCardPermissionCheck(userIndex, k, true);
  html += F("</div></div>");
  return html;
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
  html += userInputCardPermissionsHtml(userIndex);
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
    html += F("<label class='small'><input type='checkbox' name='tgn"); html += String(i); html += F("' "); if (webUserTelegramNotify[i]) html += F("checked"); html += F("> Enable Telegram Notifications</label>");
    html += F("<label class='small'><input type='checkbox' name='adhanN"); html += String(i); html += F("' "); if (webUserAdhanNotify[i]) html += F("checked"); html += F("> 🕌 Adhan Notify</label>");
    html += F("<label class='small'><input type='checkbox' name='startN"); html += String(i); html += F("' "); if (webUserStartupNotify[i]) html += F("checked"); html += F("> ✅ Startup Notify</label></div>");
    html += F("<div class='smsec'><b>🔗 Custom Quick Links</b><div class='sub'>Allow this user to open each configurable quick link button.</div>");
    for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) {
      html += F("<label class='small'><input type='checkbox' name='clacc"); html += String(i); html += F("_"); html += String(k); html += F("' ");
      if (i == 0 || webUserCustomLinkAccess[i][k]) html += F("checked ");
      if (i == 0) html += F("disabled ");
      html += F("> Allow "); html += customLinkLabelDisplayText(k); html += F("</label>");
    }
    html += F("</div>");
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
    row += F("> Enable Telegram Notifications</label>");
    row += F("<label class='small'><input type='checkbox' name='adhanN"); row += String(i); row += F("' ");
    if (webUserAdhanNotify[i]) row += F("checked");
    row += F("> 🕌 Adhan Notify</label>");
    row += F("<label class='small'><input type='checkbox' name='startN"); row += String(i); row += F("' ");
    if (webUserStartupNotify[i]) row += F("checked");
    row += F("> ✅ Startup Notify</label></div>");

    row += F("<div class='smsec'><b>🔗 Custom Quick Links</b><div class='sub'>Allow this user to open each configurable quick link button.</div>");
    for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) {
      row += F("<label class='small'><input type='checkbox' name='clacc"); row += String(i); row += F("_"); row += String(k); row += F("' ");
      if (i == 0 || webUserCustomLinkAccess[i][k]) row += F("checked ");
      if (i == 0) row += F("disabled ");
      row += F("> Allow "); row += customLinkLabelDisplayText(k); row += F("</label>");
    }
    row += F("</div>");

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


void clearDeletedUserPermissions(byte userIndex) {
  if (userIndex == 0 || userIndex >= WEB_USER_COUNT) return;

  // When a user slot is emptied, clear every permission tied to that slot so a new
  // user added later in the same slot does not inherit old access.
  webUserVisibleMask[userIndex] = 0;
  webUserControlMask[userIndex] = 0;
  webUserInputCardVisibleMask[userIndex] = 0;
  webUserInputCardNotifyMask[userIndex] = 0;
  webUserTelegramChatId[userIndex][0] = 0;
  webUserTelegramNotify[userIndex] = 0;
  webUserAdhanNotify[userIndex] = 0;
  webUserStartupNotify[userIndex] = 0;
  for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) webUserCustomLinkAccess[userIndex][k] = 0;

  // Close any live Web session that belongs to this deleted slot.
  for (byte s = 0; s < MAX_WEB_SESSIONS; s++) {
    if (webSessions[s].active && webSessions[s].userIndex == userIndex) {
      webSessions[s].active = false;
      webSessions[s].token = "";
      webSessions[s].userIndex = 255;
    }
  }

  // Close any live Telegram session that belongs to this deleted slot.
  for (byte s = 0; s < TELEGRAM_SESSION_COUNT; s++) {
    if (telegramSessions[s].loggedIn && telegramSessions[s].userIndex == userIndex) {
      telegramSessions[s].loggedIn = false;
      telegramSessions[s].stage = TG_STAGE_IDLE;
      telegramSessions[s].userIndex = 255;
      telegramSessions[s].pendingUser[0] = '\0';
    }
  }
}

void handleSaveUsers() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  bool simpleUsersMode = webServer.hasArg("simpleusers");
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    String name = webServer.arg(String("un") + i); name.trim();
    String pass = webServer.arg(String("pw") + i); pass.trim();
    String tgId = webServer.arg(String("tgid") + i); tgId.trim();
    bool tgNotify = webServer.hasArg(String("tgn") + i);
    bool adhanNotify = webServer.hasArg(String("adhanN") + i);
    bool startupNotify = webServer.hasArg(String("startN") + i);
    bool customAccess[CUSTOM_LINK_COUNT];
    for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) customAccess[k] = (i == 0) || webServer.hasArg(String("clacc") + i + "_" + k);
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
      webUserAdhanNotify[i] = adhanNotify ? 1 : 0;
      webUserStartupNotify[i] = startupNotify ? 1 : 0;
      for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) webUserCustomLinkAccess[i][k] = 1;
      continue;
    }

    if (name.length() == 0) {
      WEB_USERS[i].enabled = 0;
      WEB_USERS[i].user[0] = 0;
      WEB_USERS[i].pass[0] = 0;
      WEB_USERS[i].role = WEB_ROLE_VIEWER;
      clearDeletedUserPermissions(i);
      continue;
    }

    if (isSafeUsername(name) && !usernameExists(name, i)) setWebUserName(i, name);
    if (pass.length() > 0) setWebUserPassword(i, pass);
    if (WEB_USERS[i].pass[0] == 0) strcpy(WEB_USERS[i].pass, "1234");
    WEB_USERS[i].role = role;
    WEB_USERS[i].enabled = webServer.hasArg(String("en") + i) ? 1 : 0;
    if (isSafeTelegramChatIdText(tgId)) setUserTelegramChatId(i, tgId);
    webUserTelegramNotify[i] = tgNotify ? 1 : 0;
    webUserAdhanNotify[i] = adhanNotify ? 1 : 0;
    webUserStartupNotify[i] = startupNotify ? 1 : 0;
    for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) webUserCustomLinkAccess[i][k] = customAccess[k] ? 1 : 0;

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

    uint8_t icv = 0;
    uint8_t icn = 0;
    for (byte k = 0; k < SMART_INPUT_COUNT; k++) {
      if (webServer.hasArg(String("icv") + i + "_" + k)) icv |= (1U << k);
      if (webServer.hasArg(String("icn") + i + "_" + k)) icn |= (1U << k);
    }
    webUserInputCardVisibleMask[i] = sanitizeInputCardMask(icv);
    webUserInputCardNotifyMask[i] = sanitizeInputCardMask(icn);
  }
  webUserVisibleMask[0] = USER_FIELD_VISIBLE_DEFAULT_MASK;
  webUserControlMask[0] = USER_FIELD_CONTROL_DEFAULT_MASK;
  webUserInputCardVisibleMask[0] = SMART_INPUT_ALL_MASK;
  webUserInputCardNotifyMask[0] = SMART_INPUT_ALL_MASK;
  for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) webUserCustomLinkAccess[0][k] = 1;
  saveWebUsersConfig();
  saveUserFieldPermissions();
  saveUserInputCardPermissions();
  saveUserTelegramNotifications();
  saveTelegramUiConfig();
  saveUserPagePersistentConfig("save-users-page");
  autoBackupAfterSave("Users page saved");
  String html = htmlHeader("Users Saved");
  html += F("<div class='card'><h1>Users Saved</h1><div class='sub'>Users, roles, passwords, field permissions, Telegram notifications, Adhan Notify, Startup Notify and Custom Quick Links access saved. If your current password changed, login again.</div>");
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

  // 1. نقرأ اسم المستخدم أولاً من الطلب
  String u = webServer.arg("u"); u.trim();
  String p = webServer.arg("p"); p.trim();
  String ip = webServer.client().remoteIP().toString();
  
  // 2. مفتاح الحظر أصبح عبارة عن (اسم المستخدم + رقم الـ IP)
  String attemptKey = activationAttemptKey("L", u + "_" + ip);
  bool ar = (currentWebLanguage() == TG_LANG_AR);
  String lockMsg;

  if (activationAttemptIsLocked(attemptKey, lockMsg, ar)) {
    String html = htmlHeader(ar ? String("محاولات الدخول متوقفة") : String("Login Locked"));
    html += F("<div class='card'><h1>🛡️ "); html += (ar ? F("حظر مؤقت") : F("Locked Out")); html += F("</h1><div class='msg bad'>");
    html += htmlEscapeText(lockMsg);
    html += F("</div><a class='btn btn2' href='/login'>"); html += (ar ? F("رجوع") : F("Back")); html += F("</a></div>");
    html += htmlFooter();
    webServer.send(429, "text/html", html);
    return;
  }

  byte idx = 255;
  if (findWebUser(u, p, idx)) {
    activationAttemptClear(attemptKey); // مسح المحاولات الفاشلة فوراً عند النجاح
    setLoggedInUser(idx);
    notifyAdminOnUserLogin(idx); 
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

  // --- تسجيل المحاولة الفاشلة ---
  activationAttemptFailureMessage(attemptKey, ar);
  addEsp32Log(String("Web login failed for user: ") + u + String(" from IP: ") + ip);

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
  h.reserve(2600);
  bool arPage = (currentWebLanguage() == TG_LANG_AR);
  h += F("<!doctype html><html lang='");
  if (arPage) h += F("ar' dir='rtl'");
  else h += F("en' dir='ltr'");
  h += F("><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>"); h += title; h += F("</title>");
  h += F("<style>");
  h += F("*{box-sizing:border-box}body{margin:0;font-family:Arial,Tahoma,sans-serif;background:#0f172a;color:#e5e7eb}.wrap{max-width:520px;margin:auto;padding:16px}.card{background:#111c33;border:1px solid #334155;border-radius:18px;padding:16px;margin-top:14px}h1{font-size:24px;margin:4px 0}.sub,.k,.small,.msg{color:#94a3b8}.row{display:flex;justify-content:space-between;border-bottom:1px solid #26364f;padding:9px 0;font-size:14px}.v{font-weight:700;text-align:right}input,select,textarea{width:100%;padding:12px;border-radius:12px;border:1px solid #475569;background:#0b1220;color:#e5e7eb;margin:8px 0}textarea{min-height:140px;resize:vertical;line-height:1.5}.btn{width:100%;border:0;border-radius:14px;padding:13px;font-size:16px;font-weight:700;background:#22c55e;color:#001018;margin-top:8px}.btn2{display:block;text-align:center;text-decoration:none;background:#1e293b;color:#e5e7eb;border:1px solid #475569}.bar{height:12px;background:#0b1220;border-radius:99px;overflow:hidden;border:1px solid #334155;margin-top:12px}.fill{height:100%;width:0;background:#22c55e}.warn{color:#fbbf24}.ok,.on{color:#22c55e}.bad,.off{color:#ef4444}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}.pill{display:inline-block;padding:4px 10px;border-radius:99px;background:#0b1220;border:1px solid #334155;font-weight:700}.oc{display:grid;gap:12px;margin-top:12px}.ocbox{padding:14px;border-radius:18px;background:#0b1220;border:1px solid #334155}.octop{display:flex;align-items:center;justify-content:space-between;gap:10px}.onbtn{background:#22c55e!important;color:#03140a!important}.offbtn{background:#ef4444!important;color:#fff!important}.dot{width:18px;height:18px;border-radius:50%;display:inline-block;margin-left:8px;box-shadow:0 0 10px currentColor}.dot.on{background:#22c55e}.dot.off{background:#ef4444}.bigstate{font-size:22px;font-weight:900}.small{font-size:12px}input[type=checkbox],input[type=radio]{width:auto;margin:6px}.modepick{display:grid;gap:8px;margin:10px 0}.modepick label{padding:10px;border:1px solid #334155;border-radius:12px;background:#0b1220}.smsec{border:1px solid #26364f;border-radius:14px;padding:10px;margin:10px 0}.smsec.disabled{opacity:.38}.dayrow{display:grid;grid-template-columns:74px 74px 1fr 1fr;gap:7px;align-items:center}.dayrow .daylbl{grid-row:span 2}.dayhdr{display:grid;grid-template-columns:74px 74px 1fr 1fr;gap:7px;align-items:center}.timergrid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}.topnav{margin-bottom:8px}.homebtn{margin-top:0;background:#334155;color:#e5e7eb}.navgrid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}.navcard{display:block;text-decoration:none;color:#e5e7eb;padding:14px;border:1px solid #334155;border-radius:16px;background:#0b1220}.navcard b{display:block;font-size:15px;margin-bottom:4px}.navcard span{display:block;color:#94a3b8;font-size:12px}.statgrid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.stat{background:#0b1220;border:1px solid #334155;border-radius:16px;padding:12px}.stat .n{font-size:20px;font-weight:900}.sectiontitle{margin-top:18px;color:#cbd5e1;font-weight:900}.syncbox{border:1px solid #334155;border-radius:14px;padding:12px;background:#0b1220;margin:10px 0}@media(min-width:800px){.wrap{max-width:860px}.oc{grid-template-columns:1fr 1fr}.navgrid{grid-template-columns:repeat(3,1fr)}}");
  h += F(".sensorcard{background:linear-gradient(135deg,#111c33,#0b1f3a);border-color:#2563eb}.sensegrid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:12px}.senseitem{background:#0b1220;border:1px solid #334155;border-radius:18px;padding:16px;text-align:center}.senseicon{font-size:30px;line-height:1}.senselabel{color:#94a3b8;font-size:13px;margin-top:6px}.senseval{font-size:34px;font-weight:900;margin-top:6px;letter-spacing:-1px}.senseval small{font-size:17px;color:#cbd5e1}.tempc{color:#fb923c}.humc{color:#38bdf8}.statuslist{display:grid;gap:8px;margin-top:12px}.statusrow2{display:grid;grid-template-columns:minmax(110px,1.1fr) minmax(130px,2fr) auto;align-items:center;gap:10px;padding:11px 12px;border:1px solid #26364f;border-radius:14px;background:#0b1220}.stname{font-weight:800}.stdetail{font-size:12px;color:#94a3b8;line-height:1.35;overflow-wrap:anywhere}.statepill{display:inline-block;min-width:76px;text-align:center;padding:6px 11px;border-radius:99px;font-weight:900;border:1px solid #334155}.onpill{background:rgba(34,197,94,.16);color:#22c55e;border-color:#15803d}.offpill{background:rgba(239,68,68,.16);color:#ef4444;border-color:#b91c1c}.switchgrid{display:grid;gap:10px;margin-top:12px}.switchcard{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:13px;border-radius:16px;background:#0b1220;border:1px solid #334155}.sw{position:relative;display:inline-block;width:74px;height:40px;flex:0 0 auto}.sw input{opacity:0;width:0;height:0}.sld{position:absolute;cursor:pointer;inset:0;background:#475569;border-radius:999px;transition:.2s;box-shadow:inset 0 0 0 1px #64748b}.sld:before{position:absolute;content:'';height:32px;width:32px;left:4px;top:4px;background:#e5e7eb;border-radius:50%;transition:.2s;box-shadow:0 2px 8px rgba(0,0,0,.35)}.sw input:checked+.sld{background:#22c55e}.sw input:checked+.sld:before{transform:translateX(34px)}.techcard{opacity:.95}@media(max-width:420px){.sensegrid{grid-template-columns:1fr}.senseval{font-size:30px}.statusrow2{grid-template-columns:1fr auto}.stdetail{grid-column:1 / -1}.switchcard{align-items:flex-start}.sw{margin-top:2px}}");
  h += F(".sysbar{display:grid;grid-template-columns:repeat(2,1fr);gap:8px;margin-top:12px}.sysitem{background:#0b1220;border:1px solid #334155;border-radius:14px;padding:10px;min-height:58px}.sysitem b{display:block;font-size:12px;color:#94a3b8;margin-bottom:4px}.sysitem span{font-weight:900;font-size:13px;overflow-wrap:anywhere}.statusbarcard h1{font-size:20px}.statusgroup{border:1px solid #26364f;border-radius:16px;padding:10px;margin-top:12px;background:rgba(15,23,42,.42)}.statusgroup-title{font-weight:900;color:#e2e8f0;margin:0 0 8px;font-size:15px}.statusicon{display:inline-block;width:26px;text-align:center;margin-right:6px}.statusrow2:hover,.switchcard:hover,.sysitem:hover{border-color:#64748b}.activepill{background:rgba(251,146,60,.16);color:#fb923c;border-color:#ea580c}.disabledpill{background:rgba(148,163,184,.14);color:#94a3b8;border-color:#475569}.timerpill{background:rgba(59,130,246,.16);color:#60a5fa;border-color:#2563eb}.schedulepill{background:rgba(168,85,247,.16);color:#c084fc;border-color:#7e22ce}.timerdetail{color:#60a5fa}.scheduledetail{color:#c084fc}.smartcardrow{background:linear-gradient(135deg,#0b1220,#111827)}@media(max-width:520px){.wrap{padding:12px}.btn{padding:15px;font-size:17px}.card{border-radius:16px;padding:14px}.sysbar{grid-template-columns:1fr 1fr}.statusrow2{padding:14px;gap:8px}.stname{font-size:15px}.statepill{min-width:82px}.senseval{font-size:32px}}@media(max-width:360px){.sysbar{grid-template-columns:1fr}.statusrow2{grid-template-columns:1fr}.statepill{width:100%}}");
  h += F("body{background:radial-gradient(circle at top left,#1e3a8a33,transparent 34%),radial-gradient(circle at 90% 5%,#7c3aed33,transparent 30%),linear-gradient(145deg,#06111f 0%,#081426 42%,#020617 100%);min-height:100vh;color:#eef6ff}.wrap{position:relative;max-width:980px}.card{position:relative;overflow:hidden;background:linear-gradient(145deg,rgba(15,23,42,.92),rgba(8,16,30,.94));border:1px solid rgba(148,163,184,.22);box-shadow:0 18px 48px rgba(0,0,0,.36),inset 0 1px 0 rgba(255,255,255,.05)}.card:before{content:'';position:absolute;inset:0;pointer-events:none;background:linear-gradient(120deg,rgba(255,255,255,.06),transparent 36%,transparent 70%,rgba(56,189,248,.05));opacity:.78}.card>*{position:relative}.heroCard{padding:22px;border-color:rgba(56,189,248,.35);background:radial-gradient(circle at 100% 0%,rgba(34,211,238,.23),transparent 32%),radial-gradient(circle at 0% 0%,rgba(168,85,247,.22),transparent 36%),linear-gradient(145deg,rgba(15,23,42,.96),rgba(2,6,23,.96))}.heroTop{display:flex;align-items:flex-start;justify-content:space-between;gap:14px}.heroKicker{font-size:12px;letter-spacing:1.8px;color:#67e8f9;font-weight:900}.heroCard h1{font-size:30px;letter-spacing:.5px;margin:7px 0 4px;color:#f8fafc;text-shadow:0 0 22px rgba(56,189,248,.35)}.heroSub{font-size:13px;max-width:680px}.heroBadge{display:inline-flex;align-items:center;justify-content:center;min-height:44px;min-width:68px;max-width:260px;padding:10px 16px;border-radius:20px;background:linear-gradient(135deg,#22d3ee,#8b5cf6);color:#020617;font-weight:1000;box-shadow:0 0 28px rgba(34,211,238,.33);white-space:normal;overflow-wrap:anywhere;text-align:center;line-height:1.15}.heroChips{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:16px}.miniChip{padding:11px 12px;border-radius:16px;background:rgba(2,6,23,.55);border:1px solid rgba(148,163,184,.23)}.miniChip b{display:block;font-size:12px;color:#93c5fd;margin-bottom:3px}.miniChip span{display:block;font-weight:900;color:#f8fafc;overflow-wrap:anywhere}.heroActions{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;margin-top:12px}.heroAction{margin-top:0;background:linear-gradient(135deg,rgba(30,41,59,.98),rgba(15,23,42,.98));border-color:rgba(96,165,250,.42)}");
  h += F(".homeUiEdit{margin-top:14px;padding:13px;border:1px solid rgba(56,189,248,.22);border-radius:18px;background:rgba(2,6,23,.42)}.homeUiEdit h2{font-size:15px;margin:0 0 8px;color:#bae6fd}.homeUiGrid{display:grid;grid-template-columns:1fr 1fr auto;gap:9px;align-items:end}.homeUiEdit label{font-size:12px;color:#93c5fd;font-weight:900}.homeUiEdit input{margin-top:5px}.homeUiSave{white-space:nowrap;margin:0}.homeUiNote{margin-top:8px;font-size:12px;color:#94a3b8}@media(max-width:760px){.homeUiGrid{grid-template-columns:1fr}.homeUiSave{width:100%}}.homeLinksCard{border-color:rgba(56,189,248,.34);background:radial-gradient(circle at 100% 0%,rgba(34,211,238,.12),transparent 34%),linear-gradient(145deg,rgba(15,23,42,.94),rgba(2,6,23,.96))}.homeLinkGrid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;margin-top:12px}.homeLinkTile{margin:0;min-height:86px;border-color:rgba(96,165,250,.28)}.homeLinkTile b{font-size:16px}.homeLinkTile span{font-size:12px;line-height:1.35}.homeLinkTile.tideTile{border-color:rgba(56,189,248,.36)}.homeLinkTile.tgTile{border-color:rgba(34,197,94,.36)}@media(max-width:560px){.homeLinkGrid{grid-template-columns:1fr}.homeLinkTile{min-height:auto}}");
  h += F(".outStateBox{display:flex;align-items:center;justify-content:flex-end;gap:10px;min-width:170px}.outStateBox .statepill{flex:0 0 auto}.statusrow2 .miniSw{width:76px;height:40px;margin:0}.statusrow2 .miniSw .sld:before{width:32px;height:32px;top:4px;left:4px}.statusrow2 .miniSw input:checked+.sld:before{transform:translateX(36px)}.outCtlHint{font-size:11px;color:#67e8f9;margin-top:3px;text-align:center}.statusrow2.outrowCtl{grid-template-columns:minmax(120px,1.05fr) minmax(150px,2fr) minmax(170px,auto)}@media(max-width:760px){.statusrow2.outrowCtl{grid-template-columns:1fr auto}.statusrow2.outrowCtl .stdetail{grid-column:1/-1}.outStateBox{grid-column:1/-1;justify-content:space-between;min-width:0;width:100%;padding-top:4px}}@media(max-width:430px){.outStateBox{gap:12px}.statusrow2 .miniSw{width:86px;height:46px}.statusrow2 .miniSw .sld:before{width:34px;height:34px;top:6px;left:6px}.statusrow2 .miniSw input:checked+.sld:before{transform:translateX(40px)}}");
  h += F(".sicard{display:block;border:1px solid rgba(56,189,248,.18);border-radius:18px;background:linear-gradient(145deg,rgba(2,6,23,.72),rgba(15,23,42,.68));overflow:hidden}.sicard+ .sicard{margin-top:10px}.sicard[open]{border-color:rgba(56,189,248,.45);box-shadow:0 0 26px rgba(56,189,248,.12)}.sicHomePanel{border-color:rgba(56,189,248,.34);background:radial-gradient(circle at 100% 0%,rgba(56,189,248,.18),transparent 36%),radial-gradient(circle at 0% 0%,rgba(168,85,247,.14),transparent 36%),linear-gradient(145deg,rgba(15,23,42,.95),rgba(2,6,23,.96))}.sicHomeList{display:grid;gap:12px;margin-top:12px}.icToggleCard{border:1px solid rgba(56,189,248,.22);border-radius:19px;background:linear-gradient(145deg,rgba(2,6,23,.78),rgba(15,23,42,.72));overflow:hidden;transition:border-color .2s,box-shadow .2s,transform .2s}.icToggleCard:hover{border-color:rgba(56,189,248,.50);transform:translateY(-1px);box-shadow:0 16px 34px rgba(0,0,0,.22)}.icToggleCard.sicOpen{border-color:rgba(56,189,248,.58);box-shadow:0 0 30px rgba(56,189,248,.13)}.sicHead{cursor:pointer;display:grid;grid-template-columns:minmax(0,1fr) auto auto;align-items:center;gap:10px;padding:13px 14px}.sicChevron{width:32px;height:32px;border-radius:999px;display:inline-flex;align-items:center;justify-content:center;background:rgba(56,189,248,.13);border:1px solid rgba(56,189,248,.22);color:#bae6fd;font-weight:900}.icToggleCard .sicBody{display:none}.icToggleCard.sicOpen .sicBody{display:block}.sicExample{font-size:12px;color:#bae6fd;margin-top:3px}.sicMetaWide{grid-column:1/-1}.sicSum{list-style:none;cursor:pointer;display:grid;grid-template-columns:minmax(0,1fr) auto;align-items:center;gap:10px;padding:12px}.sicSum::-webkit-details-marker{display:none}.sicTitle{display:flex;align-items:center;gap:8px;font-weight:900;min-width:0}.sicTitle b{overflow-wrap:anywhere}.sicSub{display:block;color:#94a3b8;font-size:12px;margin-top:5px;line-height:1.35;overflow-wrap:anywhere}.sicBody{border-top:1px solid rgba(148,163,184,.16);padding:12px;background:rgba(2,6,23,.35)}.sicGrid{display:grid;grid-template-columns:repeat(2,1fr);gap:9px}.sicMeta{border:1px solid rgba(148,163,184,.16);border-radius:14px;padding:9px;background:rgba(15,23,42,.55)}.sicMeta b{display:block;color:#93c5fd;font-size:11px;margin-bottom:4px}.sicMeta span{font-weight:800;color:#f8fafc;overflow-wrap:anywhere}.sicHint{margin-top:10px;color:#94a3b8;font-size:12px}@media(max-width:560px){.heroTop{flex-direction:column}.heroBadge{align-self:flex-start}.heroChips{grid-template-columns:1fr}.heroActions{grid-template-columns:1fr}.sicSum{grid-template-columns:1fr}.sicSum .statepill{width:100%}.sicGrid{grid-template-columns:1fr}} ");

  h += F("@media(max-width:640px){.sicHomePanel{padding:12px!important}.sicHomeList{gap:10px!important}.icToggleCard{width:100%!important;max-width:100%!important;overflow:hidden!important}.sicHead{display:grid!important;grid-template-columns:minmax(0,1fr) 34px!important;align-items:center!important;gap:8px!important;padding:11px!important}.sicHead>span:first-child{grid-column:1/2!important;grid-row:1!important;display:block!important;min-width:0!important;width:100%!important}.sicHead>.sicChevron{grid-column:2!important;grid-row:1!important;width:30px!important;height:30px!important}.sicHead>.statepill{grid-column:1/-1!important;grid-row:2!important;width:100%!important;max-width:100%!important;min-width:0!important;text-align:center!important;white-space:normal!important;line-height:1.25!important;padding:7px 10px!important;font-size:12px!important}.sicTitle{display:flex!important;flex-direction:row!important;align-items:center!important;gap:6px!important;min-width:0!important;width:100%!important;max-width:100%!important;white-space:normal!important}.sicTitle .statusicon{flex:0 0 auto!important}.sicTitle b{display:block!important;min-width:0!important;max-width:100%!important;white-space:normal!important;word-break:normal!important;overflow-wrap:break-word!important;line-height:1.3!important}.sicSub,.sicExample{display:block!important;max-width:100%!important;white-space:normal!important;word-break:normal!important;overflow-wrap:break-word!important;line-height:1.35!important}.sicBody{padding:10px!important}.sicGrid{grid-template-columns:1fr!important}.sicMeta span{word-break:normal!important;overflow-wrap:break-word!important}} ");
  h += F(".statusbarcard{border-color:rgba(45,212,191,.28)}.statusbarcard h1,.statusPanel h1,.quickPanel h1,.sensorcard h1{display:flex;align-items:center;gap:8px}.sysbar{grid-template-columns:repeat(3,1fr)}.sysitem{background:linear-gradient(145deg,rgba(2,6,23,.72),rgba(15,23,42,.62));border-color:rgba(148,163,184,.22);box-shadow:inset 0 0 0 1px rgba(255,255,255,.02)}.sysitem b{color:#bae6fd}.sysitem span{font-size:14px}.ok,.on{color:#35f29a;text-shadow:0 0 12px rgba(53,242,154,.3)}.bad,.off{color:#ff5f75}.warn{color:#fbbf24}.sensorcard{border-color:rgba(56,189,248,.34);background:radial-gradient(circle at 10% 0%,rgba(251,146,60,.18),transparent 30%),radial-gradient(circle at 100% 0%,rgba(56,189,248,.24),transparent 32%),linear-gradient(145deg,rgba(15,23,42,.94),rgba(2,6,23,.96))}.senseitem{background:linear-gradient(145deg,rgba(2,6,23,.72),rgba(15,23,42,.72));border-color:rgba(148,163,184,.25);box-shadow:inset 0 0 28px rgba(56,189,248,.04)}.senseicon{font-size:34px}.senseval{font-size:40px;text-shadow:0 0 24px currentColor}.tempc{color:#fb923c}.humc{color:#38bdf8}.statusPanel{border-color:rgba(167,139,250,.32)}.statusgroup{background:linear-gradient(145deg,rgba(2,6,23,.62),rgba(15,23,42,.50));border-color:rgba(148,163,184,.18);padding:12px}.statusgroup-title{font-size:16px;color:#f8fafc;display:flex;align-items:center;gap:6px}.statusrow2{background:linear-gradient(145deg,rgba(15,23,42,.88),rgba(2,6,23,.86));border-color:rgba(148,163,184,.18);box-shadow:inset 0 0 0 1px rgba(255,255,255,.025);transition:border-color .2s,transform .2s,box-shadow .2s}.statusrow2:hover{transform:translateY(-1px);box-shadow:0 12px 30px rgba(0,0,0,.24),inset 0 0 0 1px rgba(255,255,255,.04)}");
  h += F(".rowOn{border-color:rgba(32,227,122,.42);box-shadow:0 0 0 1px rgba(32,227,122,.08),0 0 22px rgba(32,227,122,.08)}.rowOff{border-color:rgba(255,77,94,.23)}.rowActive{border-color:rgba(245,158,11,.42);box-shadow:0 0 22px rgba(245,158,11,.08)}.stname{color:#f8fafc;letter-spacing:.1px}.stdetail{color:#aebdd0}.statepill{letter-spacing:.4px;box-shadow:inset 0 0 14px rgba(255,255,255,.03)}.onpill{background:linear-gradient(135deg,rgba(32,227,122,.22),rgba(6,78,59,.48));color:#40ff9f;border-color:#16a34a;text-shadow:0 0 12px rgba(64,255,159,.35)}.offpill{background:linear-gradient(135deg,rgba(255,77,94,.20),rgba(127,29,29,.38));color:#ff7b8b;border-color:#dc2626}.activepill{background:linear-gradient(135deg,rgba(245,158,11,.24),rgba(124,45,18,.45));color:#fbbf24;border-color:#f59e0b}.timerdetail{color:#7dd3fc}.scheduledetail{color:#c4b5fd}.smartcardrow{background:radial-gradient(circle at 100% 0%,rgba(56,189,248,.14),transparent 35%),linear-gradient(145deg,rgba(15,23,42,.92),rgba(2,6,23,.92))}.quickPanel{border-color:rgba(34,197,94,.30)}.switchgrid{grid-template-columns:repeat(2,1fr)}.switchcard{background:linear-gradient(145deg,rgba(2,6,23,.72),rgba(15,23,42,.74));border-color:rgba(148,163,184,.2);box-shadow:inset 0 0 0 1px rgba(255,255,255,.02)}.sw{width:82px;height:44px}.sld{background:linear-gradient(135deg,#334155,#111827)}.sld:before{height:34px;width:34px;top:5px;left:5px}.sw input:checked+.sld{background:linear-gradient(135deg,#16a34a,#22c55e);box-shadow:0 0 18px rgba(34,197,94,.30)}.sw input:checked+.sld:before{transform:translateX(38px)}.navcard{background:linear-gradient(145deg,rgba(2,6,23,.66),rgba(15,23,42,.75));border-color:rgba(148,163,184,.22)}.btn{box-shadow:0 10px 22px rgba(0,0,0,.20)}");
  h += F("@media(max-width:760px){.wrap{max-width:560px}.heroTop{display:block}.heroBadge{display:inline-block;margin-top:12px}.heroChips,.heroActions,.switchgrid{grid-template-columns:1fr}.sysbar{grid-template-columns:1fr 1fr}.statusrow2{grid-template-columns:1fr auto}.stdetail{grid-column:1/-1}.sensegrid{grid-template-columns:1fr 1fr}}@media(max-width:430px){.wrap{padding:11px}.heroCard h1{font-size:24px}.heroChips,.sysbar,.sensegrid{grid-template-columns:1fr}.statusrow2{grid-template-columns:1fr}.statepill{width:100%;padding:9px}.sw{width:86px;height:46px}.btn{font-size:17px;padding:15px}.card{margin-top:12px}}");
  h += F(".weatherSplitWrap{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-top:14px}.weatherSplitWrap>.sensorcard{margin-top:0}.splitHead{display:flex;align-items:flex-start;justify-content:space-between;gap:10px;margin-bottom:6px}.splitHead h1{margin:0;font-size:22px}.sourceBadge{display:inline-block;padding:6px 10px;border-radius:999px;font-size:11px;font-weight:1000;letter-spacing:.7px;white-space:nowrap;border:1px solid rgba(148,163,184,.25)}.liveBadge{color:#35f29a;background:rgba(34,197,94,.14);border-color:rgba(34,197,94,.38);box-shadow:0 0 18px rgba(34,197,94,.12)}.onlineBadge{color:#7dd3fc;background:rgba(56,189,248,.14);border-color:rgba(56,189,248,.38);box-shadow:0 0 18px rgba(56,189,248,.12)}.sensorLiveCard{border-color:rgba(34,197,94,.40)!important;background:radial-gradient(circle at 0% 0%,rgba(34,197,94,.16),transparent 33%),linear-gradient(145deg,rgba(15,23,42,.94),rgba(2,6,23,.96))!important}.internetWeatherCard{border-color:rgba(56,189,248,.42)!important;background:radial-gradient(circle at 100% 0%,rgba(56,189,248,.20),transparent 34%),linear-gradient(145deg,rgba(15,23,42,.94),rgba(2,6,23,.96))!important}.sourceGrid{grid-template-columns:1fr 1fr}.weatherNote{margin-top:10px;color:#b6c4d6;font-size:12px;line-height:1.45;border-top:1px solid rgba(148,163,184,.16);padding-top:10px}.weatherOnlineLine{overflow-wrap:anywhere}.sensorLiveCard .senselabel,.internetWeatherCard .senselabel{font-weight:800}.sensorLiveCard .senseitem{border-color:rgba(34,197,94,.22)}.internetWeatherCard .senseitem{border-color:rgba(56,189,248,.24)}@media(max-width:760px){.weatherSplitWrap{grid-template-columns:1fr}.sourceGrid{grid-template-columns:1fr 1fr}}@media(max-width:430px){.sourceGrid{grid-template-columns:1fr}.splitHead{display:block}.sourceBadge{margin-top:8px}} ");
  h += F("html,body{max-width:100%;overflow-x:hidden}.wrap{overflow-x:hidden}.card,.statusgroup,.statusrow2{min-width:0}.statusgroup,.statusrow2.outrowCtl{overflow:hidden}.outStateBox{min-width:0}.outStateBox .statepill{min-width:72px;max-width:100%}@media(max-width:760px){.statusrow2.outrowCtl{grid-template-columns:1fr!important}.statusrow2.outrowCtl .stname,.statusrow2.outrowCtl .stdetail,.statusrow2.outrowCtl .outStateBox{grid-column:1/-1}.outStateBox{display:grid;grid-template-columns:minmax(0,1fr) 86px;align-items:center;gap:10px;justify-content:stretch;width:100%;padding-top:8px}.outStateBox .statepill{width:auto!important;min-width:0}.statusrow2 .miniSw{justify-self:end;max-width:86px}}@media(max-width:380px){.outStateBox{grid-template-columns:minmax(0,1fr) 78px;gap:8px}.statusrow2 .miniSw{width:78px!important;height:42px!important}.statusrow2 .miniSw .sld:before{width:32px!important;height:32px!important;top:5px!important;left:5px!important}.statusrow2 .miniSw input:checked+.sld:before{transform:translateX(36px)!important}.outStateBox .statepill{font-size:14px;padding-left:6px!important;padding-right:6px!important}}");
  h += F(".brandbar{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 16px;margin-bottom:12px;border:1px solid rgba(148,163,184,.24);border-radius:22px;background:linear-gradient(135deg,rgba(15,23,42,.86),rgba(2,6,23,.74));box-shadow:0 18px 44px rgba(0,0,0,.34),inset 0 1px 0 rgba(255,255,255,.07);backdrop-filter:blur(14px)}.brandtitle{font-weight:950;letter-spacing:.4px;color:#f8fafc}.brandmeta{font-size:12px;color:#94a3b8;margin-top:3px}.branddot{width:11px;height:11px;border-radius:50%;display:inline-block;background:#22c55e;box-shadow:0 0 18px #22c55e;margin-inline-end:7px}.topnav{position:sticky;top:8px;z-index:9}.homebtn{border-radius:18px;background:linear-gradient(135deg,rgba(51,65,85,.82),rgba(15,23,42,.88));border:1px solid rgba(148,163,184,.28);box-shadow:0 10px 28px rgba(0,0,0,.28);backdrop-filter:blur(12px)}.btn{cursor:pointer;transition:transform .16s ease,box-shadow .16s ease,border-color .16s ease,filter .16s ease}.btn:hover,.navcard:hover,.card:hover{transform:translateY(-1px)}.btn:active,.navcard:active{transform:translateY(1px) scale(.99)}.btn:not(.btn2){background:linear-gradient(135deg,#22c55e,#14b8a6);box-shadow:0 12px 24px rgba(20,184,166,.18)}.btn2{background:linear-gradient(135deg,rgba(30,41,59,.9),rgba(15,23,42,.92));border-color:rgba(148,163,184,.28)}.navcard,.stat,.sysitem,.senseitem,.switchcard,.statusrow2,.ocbox,.smsec,.syncbox,.modepick label{background:linear-gradient(145deg,rgba(15,23,42,.72),rgba(2,6,23,.62));border-color:rgba(148,163,184,.18);box-shadow:inset 0 1px 0 rgba(255,255,255,.04);backdrop-filter:blur(10px)}input,select,textarea{background:rgba(2,6,23,.72);border-color:rgba(148,163,184,.28);outline:none;transition:border-color .16s ease,box-shadow .16s ease}input:focus,select:focus,textarea:focus{border-color:#38bdf8;box-shadow:0 0 0 3px rgba(56,189,248,.16)}table{width:100%;border-collapse:separate;border-spacing:0 8px}td,th{padding:10px;border-bottom:1px solid rgba(148,163,184,.12)}th{color:#cbd5e1;text-align:inherit}.row{border-bottom:1px solid rgba(148,163,184,.14)}.pill,.statepill{box-shadow:inset 0 1px 0 rgba(255,255,255,.05)}::-webkit-scrollbar{width:10px;height:10px}::-webkit-scrollbar-thumb{background:#334155;border-radius:999px}::-webkit-scrollbar-track{background:#020617}@media(max-width:520px){.brandbar{border-radius:18px;padding:12px}.brandtitle{font-size:14px}.brandmeta{font-size:11px}.topnav{top:4px}} ");
  h += F("/* Phase2 Premium UI */.wrap:before{content:'';position:fixed;z-index:-1;inset:0;background:radial-gradient(circle at 15% 18%,rgba(34,211,238,.10),transparent 25%),radial-gradient(circle at 88% 28%,rgba(168,85,247,.12),transparent 28%),radial-gradient(circle at 50% 100%,rgba(34,197,94,.08),transparent 30%);pointer-events:none}.brandbar{position:sticky;top:8px;z-index:20;border-color:rgba(56,189,248,.32);background:linear-gradient(135deg,rgba(15,23,42,.82),rgba(2,6,23,.70));box-shadow:0 18px 48px rgba(0,0,0,.40),0 0 0 1px rgba(56,189,248,.08),inset 0 1px 0 rgba(255,255,255,.09)}.brandtitle:before{content:'⚡ ';filter:drop-shadow(0 0 8px rgba(56,189,248,.55))}.brandmeta{letter-spacing:.3px}.brandbar .pill{background:rgba(34,197,94,.12);border-color:rgba(34,197,94,.35);color:#bbf7d0}.topnav{top:78px}.homebtn:before{content:'🏠 ';}.navgrid{grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:12px}.navcard,.homeLinkTile{min-height:92px;display:flex;flex-direction:column;justify-content:center;border-radius:22px;padding:16px 15px;background:radial-gradient(circle at 100% 0%,rgba(56,189,248,.14),transparent 38%),linear-gradient(145deg,rgba(15,23,42,.82),rgba(2,6,23,.70));border-color:rgba(56,189,248,.24);box-shadow:0 14px 30px rgba(0,0,0,.24),inset 0 1px 0 rgba(255,255,255,.06)}.navcard:hover,.homeLinkTile:hover{border-color:rgba(34,211,238,.62);box-shadow:0 20px 44px rgba(0,0,0,.34),0 0 32px rgba(56,189,248,.10);transform:translateY(-3px)}.navcard b{font-size:16px;color:#f8fafc}.navcard span{margin-top:4px;line-height:1.35}.tideTile{border-color:rgba(59,130,246,.34);background:radial-gradient(circle at 100% 0%,rgba(59,130,246,.18),transparent 40%),linear-gradient(145deg,rgba(15,23,42,.84),rgba(2,6,23,.72))}.prayerTile{border-color:rgba(168,85,247,.34);background:radial-gradient(circle at 100% 0%,rgba(168,85,247,.20),transparent 40%),linear-gradient(145deg,rgba(15,23,42,.84),rgba(2,6,23,.72))}.dataTile{border-color:rgba(34,197,94,.34);background:radial-gradient(circle at 100% 0%,rgba(34,197,94,.16),transparent 40%),linear-gradient(145deg,rgba(15,23,42,.84),rgba(2,6,23,.72))}.tgTile{border-color:rgba(14,165,233,.34);background:radial-gradient(circle at 100% 0%,rgba(14,165,233,.18),transparent 40%),linear-gradient(145deg,rgba(15,23,42,.84),rgba(2,6,23,.72))}.card h1{display:flex;align-items:center;gap:8px}.card h1:after{content:'';height:2px;flex:1;background:linear-gradient(90deg,rgba(56,189,248,.45),transparent);border-radius:99px}.statgrid,.sysbar,.sensegrid{gap:12px}.stat,.sysitem,.senseitem{border-radius:20px;box-shadow:0 12px 28px rgba(0,0,0,.20),inset 0 1px 0 rgba(255,255,255,.05)}.sysitem b,.stat b,.senseitem .senselabel{text-transform:uppercase;letter-spacing:.8px}.sysitem span,.stat .n{font-size:18px}.statusrow2,.switchcard,.ocbox{border-radius:19px}.statusrow2:hover,.switchcard:hover,.ocbox:hover,.sysitem:hover,.stat:hover{transform:translateY(-1px);border-color:rgba(56,189,248,.48);box-shadow:0 16px 34px rgba(0,0,0,.24)}.statusrow2,.switchcard,.ocbox,.sysitem,.stat{transition:transform .16s ease,border-color .16s ease,box-shadow .16s ease}.statepill{border-width:1px;letter-spacing:.7px}.onpill{box-shadow:0 0 24px rgba(34,197,94,.10),inset 0 1px 0 rgba(255,255,255,.08)}.offpill{box-shadow:0 0 24px rgba(239,68,68,.08),inset 0 1px 0 rgba(255,255,255,.06)}table{overflow:hidden}th{position:sticky;top:0;background:rgba(15,23,42,.92);z-index:1}tr:nth-child(even) td{background:rgba(148,163,184,.035)}tr:hover td{background:rgba(56,189,248,.06)}td:first-child,th:first-child{border-top-left-radius:12px;border-bottom-left-radius:12px}td:last-child,th:last-child{border-top-right-radius:12px;border-bottom-right-radius:12px}.smsec{background:linear-gradient(145deg,rgba(15,23,42,.66),rgba(2,6,23,.54));border-color:rgba(148,163,184,.18)}.heroCard{box-shadow:0 24px 70px rgba(0,0,0,.42),0 0 42px rgba(56,189,248,.08)}.heroBadge{animation:pulseGlow 3.5s ease-in-out infinite}@keyframes pulseGlow{0%,100%{box-shadow:0 0 25px rgba(34,211,238,.28)}50%{box-shadow:0 0 42px rgba(139,92,246,.38)}}@media(max-width:760px){.brandbar{position:relative;top:auto}.topnav{top:4px}.navgrid{grid-template-columns:1fr 1fr}.homeLinkTile{min-height:84px}}@media(max-width:430px){.navgrid{grid-template-columns:1fr}.navcard,.homeLinkTile{min-height:78px}.card h1:after{display:none}.brandbar{display:block}.brandbar .pill{margin-top:10px;display:inline-block}} ");
  if (arPage) h += F("html,body{direction:rtl;text-align:right}.v{text-align:left}.row{direction:rtl}.topnav,.btn,.navcard{text-align:center}.switchcard,.octop,.statusrow2,.sysitem{direction:rtl}.dot{margin-left:0;margin-right:8px}.statusicon{margin-right:0;margin-left:6px}.statusgroup-title{text-align:right}");
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
  h += F("<div class='brandbar'><div><div class='brandtitle'>");
  h += htmlEscapeText(String(deviceName[0] ? deviceName : FW_NAME));
  h += F("</div><div class='brandmeta'>V");
  h += FW_VERSION;
  h += F(" · Build ");
  h += String(FW_BUILD);
  h += F("</div></div><div class='pill'><span class='branddot'></span>");
  h += (WiFi.status() == WL_CONNECTED ? F("Online") : F("Local"));
  h += F("</div></div>");
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
    
    // التعديل الجديد: استرجاع حالة الإشعارات عند تشغيل القطعة أو الريستارت
    adminLoginNotificationEnabled = p.getBool("admNotify", true); // true هي الحالة الافتراضية
    
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
    
    // التعديل الجديد: حفظ حالة إشعارات الأدمن في الذاكرة الدائمة
    p.putBool("admNotify", adminLoginNotificationEnabled); 
    
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

bool verifyUserPageRecordBytes(uint8_t* data, size_t len, uint32_t expectedVersion) {
  if (!data || len < 12) return false;
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t size = 0;
  uint32_t expected = 0;
  memcpy(&magic, data + 0, sizeof(magic));
  memcpy(&version, data + 4, sizeof(version));
  memcpy(&size, data + 6, sizeof(size));
  memcpy(&expected, data + 8, sizeof(expected));
  if (magic != USER_PAGE_STORE_MAGIC || version != expectedVersion || size != len) return false;
  uint32_t zero = 0;
  memcpy(data + 8, &zero, sizeof(zero));
  uint32_t actual = checksumUserPageBytes(data, len);
  memcpy(data + 8, &expected, sizeof(expected));
  return actual == expected;
}

void applyUserPageRecordV1Bytes(const uint8_t* data) {
  const UserPagePersistentRecordV1& r = *((const UserPagePersistentRecordV1*)data);
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
  char introBuf[LOGIN_INTRO_MAX_LEN + 1];
  memcpy(introBuf, r.introText, sizeof(introBuf));
  introBuf[LOGIN_INTRO_MAX_LEN] = 0;
  loginIntroText = String(introBuf);
  if (loginIntroText.length() > LOGIN_INTRO_MAX_LEN) loginIntroText = loginIntroText.substring(0, LOGIN_INTRO_MAX_LEN);
  // Input-card permissions did not exist in the V1 user-page store.
  // Keep the values already loaded from the EEPROM image/rescue map.
}

void applyUserPageRecordV2Bytes(const uint8_t* data) {
  const UserPagePersistentRecordV2& r = *((const UserPagePersistentRecordV2*)data);
  memcpy(WEB_USERS, r.users, sizeof(WEB_USERS));
  memcpy(webUserVisibleMask, r.visibleMask, sizeof(webUserVisibleMask));
  memcpy(webUserControlMask, r.controlMask, sizeof(webUserControlMask));
  memcpy(webUserTelegramChatId, r.telegramChatId, sizeof(webUserTelegramChatId));
  memcpy(webUserTelegramNotify, r.telegramNotify, sizeof(webUserTelegramNotify));
  memcpy(webUserInputCardVisibleMask, r.inputCardVisibleMask, sizeof(webUserInputCardVisibleMask));
  memcpy(webUserInputCardNotifyMask, r.inputCardNotifyMask, sizeof(webUserInputCardNotifyMask));
  lastSubscriptionDisableYm = r.lastDisableYm;
  monthlyUserAutoDisableEnabled = r.monthlyAuto ? true : false;
  activationCodesExpireEndMonth = r.codeExpiry ? true : false;
  telegramSmartPollingEnabled = r.telegramSmart ? true : false;
  loginIntroEnabled = r.introEnabled ? true : false;
  char introBuf[LOGIN_INTRO_MAX_LEN + 1];
  memcpy(introBuf, r.introText, sizeof(introBuf));
  introBuf[LOGIN_INTRO_MAX_LEN] = 0;
  loginIntroText = String(introBuf);
  if (loginIntroText.length() > LOGIN_INTRO_MAX_LEN) loginIntroText = loginIntroText.substring(0, LOGIN_INTRO_MAX_LEN);
}

void applyUserPageRecordV3Bytes(const uint8_t* data) {
  const UserPagePersistentRecordV3& r = *((const UserPagePersistentRecordV3*)data);
  memcpy(WEB_USERS, r.users, sizeof(WEB_USERS));
  memcpy(webUserVisibleMask, r.visibleMask, sizeof(webUserVisibleMask));
  memcpy(webUserControlMask, r.controlMask, sizeof(webUserControlMask));
  memcpy(webUserTelegramChatId, r.telegramChatId, sizeof(webUserTelegramChatId));
  memcpy(webUserTelegramNotify, r.telegramNotify, sizeof(webUserTelegramNotify));
  memcpy(webUserAdhanNotify, r.adhanNotify, sizeof(webUserAdhanNotify));
  memcpy(webUserInputCardVisibleMask, r.inputCardVisibleMask, sizeof(webUserInputCardVisibleMask));
  memcpy(webUserInputCardNotifyMask, r.inputCardNotifyMask, sizeof(webUserInputCardNotifyMask));
  lastSubscriptionDisableYm = r.lastDisableYm;
  monthlyUserAutoDisableEnabled = r.monthlyAuto ? true : false;
  activationCodesExpireEndMonth = r.codeExpiry ? true : false;
  telegramSmartPollingEnabled = r.telegramSmart ? true : false;
  loginIntroEnabled = r.introEnabled ? true : false;
  char introBuf[LOGIN_INTRO_MAX_LEN + 1];
  memcpy(introBuf, r.introText, sizeof(introBuf));
  introBuf[LOGIN_INTRO_MAX_LEN] = 0;
  loginIntroText = String(introBuf);
  if (loginIntroText.length() > LOGIN_INTRO_MAX_LEN) loginIntroText = loginIntroText.substring(0, LOGIN_INTRO_MAX_LEN);
}

void applyUserPageRecordV4Bytes(const uint8_t* data) {
  const UserPagePersistentRecord& r = *((const UserPagePersistentRecord*)data);
  memcpy(WEB_USERS, r.users, sizeof(WEB_USERS));
  memcpy(webUserVisibleMask, r.visibleMask, sizeof(webUserVisibleMask));
  memcpy(webUserControlMask, r.controlMask, sizeof(webUserControlMask));
  memcpy(webUserTelegramChatId, r.telegramChatId, sizeof(webUserTelegramChatId));
  memcpy(webUserTelegramNotify, r.telegramNotify, sizeof(webUserTelegramNotify));
  memcpy(webUserAdhanNotify, r.adhanNotify, sizeof(webUserAdhanNotify));
  memcpy(webUserStartupNotify, r.startupNotify, sizeof(webUserStartupNotify));
  memcpy(webUserInputCardVisibleMask, r.inputCardVisibleMask, sizeof(webUserInputCardVisibleMask));
  memcpy(webUserInputCardNotifyMask, r.inputCardNotifyMask, sizeof(webUserInputCardNotifyMask));
  lastSubscriptionDisableYm = r.lastDisableYm;
  monthlyUserAutoDisableEnabled = r.monthlyAuto ? true : false;
  activationCodesExpireEndMonth = r.codeExpiry ? true : false;
  telegramSmartPollingEnabled = r.telegramSmart ? true : false;
  loginIntroEnabled = r.introEnabled ? true : false;
  char introBuf[LOGIN_INTRO_MAX_LEN + 1];
  memcpy(introBuf, r.introText, sizeof(introBuf));
  introBuf[LOGIN_INTRO_MAX_LEN] = 0;
  loginIntroText = String(introBuf);
  if (loginIntroText.length() > LOGIN_INTRO_MAX_LEN) loginIntroText = loginIntroText.substring(0, LOGIN_INTRO_MAX_LEN);
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
    webUserAdhanNotify[i] = webUserAdhanNotify[i] ? 1 : 0;
    webUserStartupNotify[i] = webUserStartupNotify[i] ? 1 : 0;
    webUserInputCardVisibleMask[i] = sanitizeInputCardMask(webUserInputCardVisibleMask[i]);
    webUserInputCardNotifyMask[i] = sanitizeInputCardMask(webUserInputCardNotifyMask[i]);
  }
  webUserInputCardVisibleMask[0] = SMART_INPUT_ALL_MASK;
  webUserInputCardNotifyMask[0] = SMART_INPUT_ALL_MASK;
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
  memcpy(r.adhanNotify, webUserAdhanNotify, sizeof(webUserAdhanNotify));
  memcpy(r.startupNotify, webUserStartupNotify, sizeof(webUserStartupNotify));
  memcpy(r.inputCardVisibleMask, webUserInputCardVisibleMask, sizeof(webUserInputCardVisibleMask));
  memcpy(r.inputCardNotifyMask, webUserInputCardNotifyMask, sizeof(webUserInputCardNotifyMask));
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
  size_t sz = f.size();
  bool migratedFromV1 = false;

  if (sz == sizeof(UserPagePersistentRecord)) {
    UserPagePersistentRecord r;
    int rd = f.read((uint8_t*)&r, sizeof(r));
    f.close();
    if (rd != (int)sizeof(r)) {
      setUserPageStoreError("read short v4");
      return false;
    }
    if (!verifyUserPageRecordBytes((uint8_t*)&r, sizeof(r), USER_PAGE_STORE_VERSION)) {
      setUserPageStoreError("bad checksum v4");
      return false;
    }
    applyUserPageRecordV4Bytes((const uint8_t*)&r);
  } else if (sz == sizeof(UserPagePersistentRecordV3)) {
    UserPagePersistentRecordV3 r3;
    int rd = f.read((uint8_t*)&r3, sizeof(r3));
    f.close();
    if (rd != (int)sizeof(r3)) {
      setUserPageStoreError("read short v3");
      return false;
    }
    if (!verifyUserPageRecordBytes((uint8_t*)&r3, sizeof(r3), 3)) {
      setUserPageStoreError("bad checksum v3");
      return false;
    }
    applyUserPageRecordV3Bytes((const uint8_t*)&r3);
    migratedFromV1 = true;
  } else if (sz == sizeof(UserPagePersistentRecordV2)) {
    UserPagePersistentRecordV2 r2;
    int rd = f.read((uint8_t*)&r2, sizeof(r2));
    f.close();
    if (rd != (int)sizeof(r2)) {
      setUserPageStoreError("read short v2");
      return false;
    }
    if (!verifyUserPageRecordBytes((uint8_t*)&r2, sizeof(r2), 2)) {
      setUserPageStoreError("bad checksum v2");
      return false;
    }
    applyUserPageRecordV2Bytes((const uint8_t*)&r2);
    migratedFromV1 = true;
  } else if (sz == sizeof(UserPagePersistentRecordV1)) {
    UserPagePersistentRecordV1 r1;
    int rd = f.read((uint8_t*)&r1, sizeof(r1));
    f.close();
    if (rd != (int)sizeof(r1)) {
      setUserPageStoreError("read short v1");
      return false;
    }
    if (!verifyUserPageRecordBytes((uint8_t*)&r1, sizeof(r1), 1)) {
      setUserPageStoreError("bad checksum v1");
      return false;
    }
    applyUserPageRecordV1Bytes((const uint8_t*)&r1);
    migratedFromV1 = true;
  } else {
    f.close();
    setUserPageStoreError("bad store size");
    return false;
  }

  sanitizeLoadedUserPageData();
  userPageStoreLoadedAtBoot = true;
  userPageStoreLoadCount++;
  setUserPageStoreError(migratedFromV1 ? "migrated old user store" : "none");
  if (migratedFromV1) saveUserPagePersistentConfig("migrate-user-store-v4");
  return true;
}

String userPageStorageStatusText() {
  String s;
  s.reserve(140);
  s += F("Load="); s += userPageStoreLoadedAtBoot ? F("YES") : F("NO");
  s += F(" / LastSave="); s += userPageStoreLastSaveOk ? F("OK") : F("FAILED/none");
  s += F(" / Saves="); s += String(userPageStoreSaveCount);
  s += F(" / Store=V4+Adhan+StartupNotify");
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

void setActivationLogStoreError(const char* msg) {
  if (!msg || !msg[0]) msg = "none";
  strncpy(activationLogStoreLastError, msg, sizeof(activationLogStoreLastError) - 1);
  activationLogStoreLastError[sizeof(activationLogStoreLastError) - 1] = '\0';
}

uint32_t checksumActivationLogBytes(const uint8_t* p, size_t len) {
  uint32_t sum = 2166136261UL;
  for (size_t i = 0; i < len; i++) {
    sum ^= p[i];
    sum *= 16777619UL;
  }
  return sum;
}

byte activationLogCountInRam() {
  byte n = 0;
  for (byte i = 0; i < ACTIVATION_LOG_COUNT; i++) if (activationLogCode[i][0]) n++;
  return n;
}

bool saveActivationLogToFs() {
  activationLogStoreLastSaveOk = false;
  if (!storageFsBeginOk && !beginStorageFs()) {
    setActivationLogStoreError("FS mount failed");
    return false;
  }
  ActivationLogStoreRecord r;
  memset(&r, 0, sizeof(r));
  r.magic = ACTIVATION_LOG_STORE_MAGIC;
  r.version = ACTIVATION_LOG_STORE_VERSION;
  r.size = sizeof(ActivationLogStoreRecord);
  memcpy(r.code, activationLogCode, sizeof(activationLogCode));
  memcpy(r.user, activationLogUser, sizeof(activationLogUser));
  memcpy(r.at, activationLogAt, sizeof(activationLogAt));
  memcpy(r.method, activationLogMethod, sizeof(activationLogMethod));
  r.checksum = 0;
  r.checksum = checksumActivationLogBytes((const uint8_t*)&r, sizeof(r));

  File f = SPIFFS.open(ACTIVATION_LOG_STORE_PATH, FILE_WRITE);
  if (!f) {
    setActivationLogStoreError("open write failed");
    return false;
  }
  size_t w = f.write((const uint8_t*)&r, sizeof(r));
  f.flush();
  f.close();
  if (w != sizeof(r)) {
    setActivationLogStoreError("write short");
    return false;
  }
  activationLogStoreLastSaveOk = true;
  activationLogStoreSaveCount++;
  setActivationLogStoreError("none");
  return true;
}

bool loadActivationLogFromFs() {
  activationLogStoreLoadedAtBoot = false;
  if (!storageFsBeginOk && !beginStorageFs()) {
    setActivationLogStoreError("FS mount failed");
    return false;
  }
  if (!SPIFFS.exists(ACTIVATION_LOG_STORE_PATH)) {
    setActivationLogStoreError("no log store");
    return false;
  }
  File f = SPIFFS.open(ACTIVATION_LOG_STORE_PATH, FILE_READ);
  if (!f) {
    setActivationLogStoreError("open read failed");
    return false;
  }
  if (f.size() != sizeof(ActivationLogStoreRecord)) {
    f.close();
    setActivationLogStoreError("bad size");
    return false;
  }
  ActivationLogStoreRecord r;
  int rd = f.read((uint8_t*)&r, sizeof(r));
  f.close();
  if (rd != (int)sizeof(r)) {
    setActivationLogStoreError("read short");
    return false;
  }
  uint32_t expected = r.checksum;
  r.checksum = 0;
  uint32_t actual = checksumActivationLogBytes((const uint8_t*)&r, sizeof(r));
  r.checksum = expected;
  if (r.magic != ACTIVATION_LOG_STORE_MAGIC || r.version != ACTIVATION_LOG_STORE_VERSION || r.size != sizeof(ActivationLogStoreRecord) || actual != expected) {
    setActivationLogStoreError("bad checksum");
    return false;
  }

  memcpy(activationLogCode, r.code, sizeof(activationLogCode));
  memcpy(activationLogUser, r.user, sizeof(activationLogUser));
  memcpy(activationLogAt, r.at, sizeof(activationLogAt));
  memcpy(activationLogMethod, r.method, sizeof(activationLogMethod));

  for (byte i = 0; i < ACTIVATION_LOG_COUNT; i++) {
    activationLogCode[i][ACTIVATION_CODE_LEN] = '\0';
    activationLogUser[i][WEB_USER_MAX_LEN] = '\0';
    activationLogAt[i][ACTIVATION_USED_AT_MAX_LEN] = '\0';
    activationLogMethod[i][ACTIVATION_METHOD_MAX_LEN] = '\0';
    if (activationLogCode[i][0] && !activationCodeTextValid(String(activationLogCode[i]))) {
      activationLogCode[i][0] = '\0';
      activationLogUser[i][0] = '\0';
      activationLogAt[i][0] = '\0';
      activationLogMethod[i][0] = '\0';
    }
  }
  activationLogStoreLoadedAtBoot = true;
  setActivationLogStoreError("none");
  return true;
}

void loadActivationLog() {
  memset(activationLogCode, 0, sizeof(activationLogCode));
  memset(activationLogUser, 0, sizeof(activationLogUser));
  memset(activationLogAt, 0, sizeof(activationLogAt));
  memset(activationLogMethod, 0, sizeof(activationLogMethod));

  if (activationLogPrefs.begin("actlog", true)) {
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

  byte nvsCount = activationLogCountInRam();
  bool fsLoaded = loadActivationLogFromFs();
  if (!fsLoaded && nvsCount > 0) saveActivationLogToFs();
}

void saveActivationLog() {
  bool nvsOk = false;
  if (activationLogPrefs.begin("actlog", false)) {
    nvsOk = true;
    for (byte i = 0; i < ACTIVATION_LOG_COUNT; i++) {
      String idx = String(i);
      activationLogPrefs.putString((String("c") + idx).c_str(), String(activationLogCode[i]));
      activationLogPrefs.putString((String("u") + idx).c_str(), String(activationLogUser[i]));
      activationLogPrefs.putString((String("at") + idx).c_str(), String(activationLogAt[i]));
      activationLogPrefs.putString((String("m") + idx).c_str(), String(activationLogMethod[i]));
    }
    activationLogPrefs.end();
  }
  bool fsOk = saveActivationLogToFs();
  if (!nvsOk && !fsOk) addEsp32Log("Activation log save failed");
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

bool activationCodeAlreadyReservedForUser(const String& codeIn, const String& usedBy) {
  String c = codeIn;
  String u = usedBy;
  c.trim();
  u.trim();
  if (!activationCodeTextValid(c) || !u.length()) return false;
  for (byte i = 0; i < ACTIVATION_CODE_COUNT; i++) {
    if (!activationCodes[i][0]) continue;
    if (!activationCodeUsed[i]) continue;
    if (!c.equals(String(activationCodes[i]))) continue;
    String by = String(activationCodeUsedBy[i]);
    by.trim();
    if (by.equalsIgnoreCase(u)) return true;
  }
  return false;
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
  html += F("<h1>Admin Notifications</h1>");
  html += F("<label class='small'><input type='checkbox' name='adminNotify' value='1' "); 
  if (adminLoginNotificationEnabled) html += F("checked"); 
  html += F("> Enable Telegram login notifications</label>");
  html += F("<div class='sub'>إرسال رسالة للأدمن على تيليجرام عند تسجيل دخول أي مستخدم.</div>");
  html += F("<button class='btn btn2' type='submit'>Save Subscription Settings</button></form>");
  html += F("<div class='row'><span class='k'>Monthly Auto-disable</span><span class='v'>"); html += (monthlyUserAutoDisableEnabled ? "ON" : "OFF"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Code Expiry</span><span class='v'>"); html += (activationCodesExpireEndMonth ? "End of month" : "No expiry"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Telegram Polling</span><span class='v'>"); html += (telegramSmartPollingEnabled ? "Smart" : "Fast 10s"); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>User Page Storage</span><span class='v'>"); html += htmlEscapeText(userPageStorageStatusText()); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Activation Codes Storage</span><span class='v'>Load="); html += activationCodeStoreLoadedAtBoot ? F("YES") : F("NO"); html += F(" / LastSave="); html += activationCodeStoreLastSaveOk ? F("OK") : F("FAILED/none"); html += F(" / Saves="); html += String(activationCodeStoreSaveCount); html += F(" / Err="); html += htmlEscapeText(String(activationCodeStoreLastError)); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Activation Log Storage</span><span class='v'>Load="); html += activationLogStoreLoadedAtBoot ? F("YES") : F("NO"); html += F(" / LastSave="); html += activationLogStoreLastSaveOk ? F("OK") : F("FAILED/none"); html += F(" / Saves="); html += String(activationLogStoreSaveCount); html += F(" / Err="); html += htmlEscapeText(String(activationLogStoreLastError)); html += F("</span></div>");
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
  adminLoginNotificationEnabled = webServer.hasArg("adminNotify");
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
void notifyAdminOnUserLogin(byte idx) {
  // لو الخاصية مقفولة من الإعدادات، اخرج ومتبعتش حاجة
  if (!adminLoginNotificationEnabled) return; 

  // التأكد من أن اليوزر صالح
  if (idx >= WEB_USER_COUNT || !WEB_USERS[idx].user[0]) return;
  
  // الحصول على رقم تيليجرام الخاص بالأدمن (اليوزر رقم 0)
  String adminChatId = String(webUserTelegramChatId[0]);
  adminChatId.trim();
  
  // التأكد من أن الأدمن رابط حسابه بالتيليجرام
  if (adminChatId.length() > 0 && adminChatId != "0" && adminChatId != "255") {
    String msg = "🔔 *تنبيه: تسجيل دخول جديد*\n\n";
    msg += "👤 *المستخدم:* " + String(WEB_USERS[idx].user) + "\n";
    // استخدام دالة الوقت المدمجة في نظامك
    msg += "🕒 *الوقت:* " + safeActivationDateTimeText(); 
    
    telegramSendToChat(adminChatId, msg);
  }
}
void notifyAdminOnUnknownUser(const String& unkChatId, const String& action) {
  // لو الإشعارات مقفولة من الأدمن، متبعتش حاجة
  if (!adminLoginNotificationEnabled) return; 

  String adminChatId = String(webUserTelegramChatId[0]);
  adminChatId.trim();
  
  // التأكد إن الأدمن مربوط، وإن الغريب ده مش هو الأدمن نفسه
  if (adminChatId.length() > 0 && adminChatId != "0" && adminChatId != "255" && adminChatId != unkChatId) {
    String msg = "⚠️ *تنبيه: محاولة من شخص غريب*\n\n";
    msg += "👤 *شخص غير مسجل في النظام يحاول استخدام البوت.*\n";
    msg += "🆔 *رقم الـ ID الخاص به:* `" + unkChatId + "`\n";
    msg += "💬 *الحدث:* " + action + "\n";
    msg += "🕒 *الوقت:* " + safeActivationDateTimeText();
    
    telegramSendToChat(adminChatId, msg);
  }
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

  bool activationAccepted = false;
  if (consumeActivationCode(c, String(WEB_USERS[idx].user), String("Web"))) {
    activationAccepted = true;
  }

  if (!activationAccepted) {
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
    saveUserPagePersistentConfig("monthly-disable-users");
    autoBackupAfterSave("Monthly users disabled");
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

  if (canEngineer || userHasAnyVisibleSmartInputCard()) {
    html += F("<a class='navcard' href='"); html += sessionUrl("/inputcards"); html += F("'><b>");
    html += ar ? F("كروت المداخل") : F("Input Cards");
    html += F("</b><span>");
    html += ar ? F("6 مداخل ديجيتال/أنالوج مع تنبيه وتشغيل مخارج") : F("6 digital/analog inputs with notify and output action");
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
    html += F("<a class='navcard' href='"); html += sessionUrl("/uisettings"); html += F("'><b>");
    html += ar ? F("إعدادات شكل الهوم") : F("UI Branding");
    html += F("</b><span>");
    html += ar ? F("عنوان الهوم، الوصف، الشارة، ولينك بوت تيليجرام") : F("Home title, subtitle, badge and Telegram bot link");
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


String homeExternalLinksCardHtml() {
  byte lang = currentWebLanguage();
  bool ar = (lang == TG_LANG_AR);
  String tgLink = normalizedTelegramBotLink();
  String html;
  html.reserve(2400);
  html += F("<div class='card homeLinksCard'><h1>🔗 ");
  html += ar ? F("روابط سريعة") : F("Quick Links");
  html += F("</h1><div class='sub'>");
  html += ar ? F("روابط سريعة + 3 أزرار مخصصة حسب صلاحيات كل مستخدم.") : F("Quick links + 3 configurable buttons according to each user's permissions.");
  html += F("</div><div class='homeLinkGrid'>");
  html += F("<a class='navcard homeLinkTile tideTile' target='_blank' rel='noopener' href='");
  html += RAS_SEDR_TIDE_URL;
  html += F("'><b>🌊 ");
  html += ar ? F("مد وجزر رأس سدر") : F("Ras Sedr Tide");
  html += F("</b><span>");
  html += ar ? F("يفتح جدول المد والجزر في صفحة خارجية جديدة.") : F("Open the tide forecast in a new external page.");
  html += F("</span></a>");
  html += F("<a class='navcard homeLinkTile prayerTile' href='");
  html += sessionUrl("/prayertimes");
  html += F("'><b>🕌 ");
  html += ar ? F("مواقيت الصلاة") : F("Prayer Times");
  html += F("</b><span>");
  html += ar ? F("يعرض مواعيد الأذان والصلاة القادمة والوقت المتبقي.") : F("Show adhan times, next prayer and remaining time.");
  html += F("</span></a>");
  for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) {
    html += F("<a class='navcard homeLinkTile dataTile' href='");
    html += sessionUrl(String("/ql") + String(k + 1));
    html += F("'><b>");
    html += customLinkTypeIcon(customLinkType[k]);
    html += F(" ");
    html += customLinkLabelDisplayText(k);
    html += F("</b><span>");
    html += ar ? F("زر مخصص يعرض الرابط المحفوظ حسب النوع والصلاحية.") : F("Configurable button that displays the saved link by type and permission.");
    html += F(" ");
    html += customLinkTypeName(customLinkType[k], ar);
    html += F("</span></a>");
    yield();
  }
  if (tgLink.length()) {
    html += F("<a class='navcard homeLinkTile tgTile' target='_blank' rel='noopener' href='");
    html += htmlEscapeText(tgLink);
    html += F("'><b>🤖 ");
    html += ar ? F("بوت تيليجرام") : F("Telegram Bot");
    html += F("</b><span>");
    html += ar ? F("افتح البوت للتحكم والتنبيهات حسب صلاحيات المستخدم.") : F("Open the bot for control and alerts according to user permissions.");
    html += F("</span></a>");
  } else if (webIsAdmin()) {
    html += F("<a class='navcard homeLinkTile tgTile' href='");
    html += sessionUrl("/uisettings");
    html += F("'><b>🤖 ");
    html += ar ? F("بوت تيليجرام") : F("Telegram Bot");
    html += F("</b><span>");
    html += ar ? F("لم يتم حفظ رابط البوت بعد. اضغط لإضافته من إعدادات الواجهة.") : F("No bot link saved yet. Tap to add it from UI Branding Settings.");
    html += F("</span></a>");
  }
  html += F("</div></div>");
  return html;
}

byte sanitizeCustomLinkType(int value) {
  if (value < 0 || value > CUSTOM_LINK_TYPE_PDF) return CUSTOM_LINK_TYPE_CSV;
  return (byte)value;
}

String customLinkTypeName(byte type, bool ar) {
  type = sanitizeCustomLinkType(type);
  if (type == CUSTOM_LINK_TYPE_IMAGE) return ar ? String("صورة") : String("Image");
  if (type == CUSTOM_LINK_TYPE_TEXT) return ar ? String("نص") : String("Text");
  if (type == CUSTOM_LINK_TYPE_PDF) return String("PDF");
  return ar ? String("CSV / Excel") : String("CSV / Excel");
}

String customLinkTypeIcon(byte type) {
  type = sanitizeCustomLinkType(type);
  if (type == CUSTOM_LINK_TYPE_IMAGE) return String("🖼️");
  if (type == CUSTOM_LINK_TYPE_TEXT) return String("📝");
  if (type == CUSTOM_LINK_TYPE_PDF) return String("📕");
  return String("📊");
}

bool currentUserCanOpenCustomLink(byte linkIndex) {
  if (linkIndex >= CUSTOM_LINK_COUNT) return false;
  if (currentWebUserIndex == 0 || currentWebRole >= WEB_ROLE_ADMIN) return true;
  if (currentWebUserIndex >= WEB_USER_COUNT) return false;
  return webUserCustomLinkAccess[currentWebUserIndex][linkIndex] != 0;
}

String customLinkLabelDisplayText(byte linkIndex) {
  if (linkIndex >= CUSTOM_LINK_COUNT) return String("Link");
  String t = String(customLinkLabel[linkIndex]);
  t.trim();
  if (!t.length()) t = (linkIndex == 0) ? "data" : String("Link ") + String(linkIndex + 1);
  return htmlEscapeText(t);
}

String normalizedCustomLinkUrl(byte linkIndex) {
  if (linkIndex >= CUSTOM_LINK_COUNT) return String("");
  String u = String(customLinkUrl[linkIndex]);
  u.trim();
  return u;
}

void sendCustomLinkPage(byte linkIndex) {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  byte lang = currentWebLanguage();
  bool ar = (lang == TG_LANG_AR);
  if (linkIndex >= CUSTOM_LINK_COUNT) {
    webServer.send(404, "text/plain", "Not found");
    return;
  }
  String title = customLinkLabelDisplayText(linkIndex);
  byte type = sanitizeCustomLinkType(customLinkType[linkIndex]);
  String html = htmlHeader(title);
  html.reserve(7200);
  if (ar) html += F("<div dir='rtl' style='text-align:right'>");
  html += F("<div class='card'><h1>");
  html += customLinkTypeIcon(type);
  html += F(" ");
  html += title;
  html += F("</h1><div class='sub'>");
  html += customLinkTypeName(type, ar);
  html += F("</div>");
  if (!currentUserCanOpenCustomLink(linkIndex)) {
    html += F("<div class='msg bad'>");
    html += ar ? F("غير مصرح لك باستخدام هذا الزرار. تواصل مع الأدمن لتفعيل الصلاحية.") : F("You are not authorized to use this button. Contact the admin to enable access.");
    html += F("</div><a class='btn btn2' href='"); html += sessionUrl("/"); html += F("'>"); html += ar ? F("رجوع للهوم") : F("Back to Home"); html += F("</a></div>");
    if (ar) html += F("</div>");
    html += htmlFooter();
    webServer.send(403, "text/html", html);
    return;
  }
  String url = normalizedCustomLinkUrl(linkIndex);
  if (!url.length()) {
    html += F("<div class='msg warn'>");
    html += ar ? F("لم يتم حفظ رابط لهذا الزر بعد. الأدمن يضيف الرابط من UI Branding Settings.") : F("URL is not configured yet. Admin can add it from UI Branding Settings.");
    html += F("</div>");
  } else if (type == CUSTOM_LINK_TYPE_IMAGE) {
    html += F("<div class='sub'>"); html += ar ? F("يتم عرض الصورة من الرابط المحفوظ.") : F("Image is displayed from the saved URL."); html += F("</div>");
    html += F("<div style='overflow:auto;text-align:center'><img src='"); html += htmlEscapeText(url); html += F("' style='max-width:100%;height:auto;border-radius:14px;border:1px solid rgba(148,163,184,.25)' onerror=\"document.getElementById('linkMsg').className='msg bad';document.getElementById('linkMsg').textContent='Image load failed. Use a direct GitHub Raw image link.'\"></div>");
    html += F("<div id='linkMsg' class='msg ok'>"); html += ar ? F("تم تحميل الصورة أو جاري التحميل.") : F("Image loading/displayed."); html += F("</div>");
  } else if (type == CUSTOM_LINK_TYPE_TEXT) {
    html += F("<div id='linkMsg' class='msg warn'>Loading text...</div><pre id='txtBox' style='white-space:pre-wrap;overflow:auto;background:rgba(2,6,23,.75);border:1px solid rgba(148,163,184,.25);padding:12px;border-radius:12px'></pre>");
    html += F("<script>const U="); html += jsQuote(url); html += F(";function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}fetch(U,{cache:'no-store'}).then(x=>{if(!x.ok)throw new Error('HTTP '+x.status);return x.text();}).then(t=>{document.getElementById('txtBox').innerHTML=esc(t);document.getElementById('linkMsg').className='msg ok';document.getElementById('linkMsg').textContent='Text loaded';}).catch(e=>{document.getElementById('linkMsg').className='msg bad';document.getElementById('linkMsg').textContent='Text load failed: '+e.message+' - use a direct GitHub Raw text link.';});</script>");
  } else if (type == CUSTOM_LINK_TYPE_PDF) {
    html += F("<div class='sub'>"); html += ar ? F("سيتم عرض ملف PDF داخل الصفحة إن كان المتصفح يدعم ذلك.") : F("PDF will be shown inside this page if the browser supports it."); html += F("</div>");
    html += F("<iframe src='"); html += htmlEscapeText(url); html += F("' style='width:100%;height:75vh;border:1px solid rgba(148,163,184,.25);border-radius:12px'></iframe>");
    html += F("<a class='btn btn2' target='_blank' rel='noopener' href='"); html += htmlEscapeText(url); html += F("'>"); html += ar ? F("فتح في صفحة خارجية") : F("Open external"); html += F("</a>");
  } else {
    html += F("<div class='sub'>"); html += ar ? F("يتم تحميل ملف CSV من الرابط المحفوظ وعرضه كجدول داخل الصفحة.") : F("The saved CSV link is loaded and displayed as a table inside this page."); html += F("</div>");
    html += F("<div id='csvMsg' class='msg warn'>Loading CSV...</div><div style='overflow:auto'><table id='csvTable' style='width:100%;border-collapse:collapse;margin-top:10px'></table></div>");
    html += F("<script>const CSV_URL="); html += jsQuote(url); html += F(";function esc(s){return String(s).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));}function parseCSV(t){let r=[],row=[],v='',q=false;for(let i=0;i<t.length;i++){let c=t[i],n=t[i+1];if(q){if(c=='\"'&&n=='\"'){v+='\"';i++;}else if(c=='\"')q=false;else v+=c;}else{if(c=='\"')q=true;else if(c==','){row.push(v);v='';}else if(c=='\\n'){row.push(v);r.push(row);row=[];v='';}else if(c!='\\r')v+=c;}}row.push(v);r.push(row);return r.filter(x=>x.some(y=>String(y).trim().length));}fetch(CSV_URL,{cache:'no-store'}).then(x=>{if(!x.ok)throw new Error('HTTP '+x.status);return x.text();}).then(t=>{let rows=parseCSV(t);let h='';rows.forEach((row,i)=>{h+='<tr>';row.forEach(c=>h+=(i==0?'<th':'<td')+\" style='border:1px solid #334155;padding:8px;text-align:left'\"+'>'+esc(c)+(i==0?'</th>':'</td>'));h+='</tr>';});document.getElementById('csvTable').innerHTML=h;document.getElementById('csvMsg').className='msg ok';document.getElementById('csvMsg').textContent='CSV loaded: '+rows.length+' rows';}).catch(e=>{document.getElementById('csvMsg').className='msg bad';document.getElementById('csvMsg').textContent='CSV load failed: '+e.message+' - use a direct GitHub Raw CSV link if Google Drive is blocked.';});</script>");
  }
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/"); html += F("'>"); html += ar ? F("رجوع للهوم") : F("Back to Home"); html += F("</a></div>");
  if (ar) html += F("</div>");
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void sendCustomLink1Page() { sendCustomLinkPage(0); }
void sendCustomLink2Page() { sendCustomLinkPage(1); }
void sendCustomLink3Page() { sendCustomLinkPage(2); }

String uiSingleLineClean(String value, const String& fallback) {
  value.replace("\r", " ");
  value.replace("\n", " ");
  value.trim();
  if (!value.length()) value = fallback;
  return value;
}

void uiCopySingleLineToChar(String value, char* dest, size_t destLen, const char* fallback) {
  if (!dest || destLen == 0) return;
  String fb = fallback ? String(fallback) : String("");
  value = uiSingleLineClean(value, fb);
  value.substring(0, destLen - 1).toCharArray(dest, destLen);
  dest[destLen - 1] = 0;
}

String normalizedTelegramBotLink() {
  String link = String(telegramBotLink);
  link.trim();
  if (!link.length()) return String("");
  if (link.startsWith("http://") || link.startsWith("https://")) return link;
  if (link.startsWith("@")) link = link.substring(1);
  return String("https://t.me/") + link;
}

String homeHeroKickerDisplayText() {
  String t = String(homeHeroKicker);
  t.trim();
  if (!t.length()) t = HOME_DEFAULT_KICKER;
  return htmlEscapeText(t);
}

String homeHeroTitleDisplayText() {
  String t = String(homeHeroTitle);
  t.trim();
  if (!t.length()) t = HOME_DEFAULT_TITLE;
  return htmlEscapeText(t);
}

String homeHeroSubtitleDisplayText() {
  String t = String(homeHeroSubtitle);
  t.trim();
  if (!t.length()) t = HOME_DEFAULT_SUBTITLE;
  return htmlEscapeText(t);
}

String homeBadgeDisplayText() {
  String t = String(homeBadgeText);
  t.trim();
  if (!t.length() || t.equalsIgnoreCase("ESP32")) t = HOME_DEFAULT_BADGE;
  t.replace("ESP32", "Device");
  t.replace("esp32", "device");
  return htmlEscapeText(t);
}

void applyDefaultHomeHeaderTexts() {
  uiCopySingleLineToChar(HOME_DEFAULT_KICKER, homeHeroKicker, sizeof(homeHeroKicker), HOME_DEFAULT_KICKER);
  uiCopySingleLineToChar(HOME_DEFAULT_TITLE, homeHeroTitle, sizeof(homeHeroTitle), HOME_DEFAULT_TITLE);
  uiCopySingleLineToChar(HOME_DEFAULT_SUBTITLE, homeHeroSubtitle, sizeof(homeHeroSubtitle), HOME_DEFAULT_SUBTITLE);
  uiCopySingleLineToChar(HOME_DEFAULT_BADGE, homeBadgeText, sizeof(homeBadgeText), HOME_DEFAULT_BADGE);
}

String homeUiInlineFormHtml(bool ar) {
  (void)ar;
  return String("");
}

void loadHomeUiConfigFromFs() {
  if (!storageFsBeginOk && !beginStorageFs()) return;
  const char* cfgPath = HOME_UI_CONFIG_PATH;
  if (!SPIFFS.exists(cfgPath)) {
    if (SPIFFS.exists(HOME_UI_CONFIG_OLD_PATH)) cfgPath = HOME_UI_CONFIG_OLD_PATH;
    else if (SPIFFS.exists(HOME_UI_CONFIG_OLDER_PATH)) cfgPath = HOME_UI_CONFIG_OLDER_PATH;
    else return;
  }
  File f = SPIFFS.open(cfgPath, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();
    if (key == "botlink") uiCopySingleLineToChar(val, telegramBotLink, sizeof(telegramBotLink), "");
    else if (key == "dataLabel") uiCopySingleLineToChar(val, customLinkLabel[0], sizeof(customLinkLabel[0]), "data");
    else if (key == "dataUrl") uiCopySingleLineToChar(val, customLinkUrl[0], sizeof(customLinkUrl[0]), "");
    else if (key == "dataPerm") {
      for (byte i = 0; i < WEB_USER_COUNT; i++) webUserCustomLinkAccess[i][0] = (i == 0) ? 1 : 0;
      for (byte i = 0; i < WEB_USER_COUNT && i < val.length(); i++) webUserCustomLinkAccess[i][0] = (val.charAt(i) == '1') ? 1 : 0;
      for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) webUserCustomLinkAccess[0][k] = 1;
    }
    else if (key.startsWith("clabel")) {
      byte k = (byte)key.substring(6).toInt();
      if (k < CUSTOM_LINK_COUNT) uiCopySingleLineToChar(val, customLinkLabel[k], sizeof(customLinkLabel[k]), (k == 0) ? "data" : "Link");
    }
    else if (key.startsWith("curl")) {
      byte k = (byte)key.substring(4).toInt();
      if (k < CUSTOM_LINK_COUNT) uiCopySingleLineToChar(val, customLinkUrl[k], sizeof(customLinkUrl[k]), "");
    }
    else if (key.startsWith("ctype")) {
      byte k = (byte)key.substring(5).toInt();
      if (k < CUSTOM_LINK_COUNT) customLinkType[k] = sanitizeCustomLinkType(val.toInt());
    }
    else if (key.startsWith("cperm")) {
      byte k = (byte)key.substring(5).toInt();
      if (k < CUSTOM_LINK_COUNT) {
        for (byte i = 0; i < WEB_USER_COUNT; i++) webUserCustomLinkAccess[i][k] = (i == 0) ? 1 : 0;
        for (byte i = 0; i < WEB_USER_COUNT && i < val.length(); i++) webUserCustomLinkAccess[i][k] = (val.charAt(i) == '1') ? 1 : 0;
        webUserCustomLinkAccess[0][k] = 1;
      }
    }
    else if (key == "badge") uiCopySingleLineToChar(val, homeBadgeText, sizeof(homeBadgeText), HOME_DEFAULT_BADGE);
    else if (key == "kicker") uiCopySingleLineToChar(val, homeHeroKicker, sizeof(homeHeroKicker), HOME_DEFAULT_KICKER);
    else if (key == "title") uiCopySingleLineToChar(val, homeHeroTitle, sizeof(homeHeroTitle), HOME_DEFAULT_TITLE);
    else if (key == "subtitle") uiCopySingleLineToChar(val, homeHeroSubtitle, sizeof(homeHeroSubtitle), HOME_DEFAULT_SUBTITLE);
    else if (key == "prayerGov") setPrayerGovernorateFromId(val);
    else if (key == "showWeather") homeShowWeatherSections = val.toInt() ? 1 : 0;
    else if (key == "showLinks") homeShowQuickLinksSection = val.toInt() ? 1 : 0;
    else if (key == "showAllowed") homeShowAllowedStatusSection = val.toInt() ? 1 : 0;
    else if (key == "showInputCards") homeShowInputCardsSection = val.toInt() ? 1 : 0;
    else if (key == "showTech") homeShowTechnicalSection = val.toInt() ? 1 : 0;
    yield();
  }
  f.close();
}

void saveHomeUiConfigToFs() {
  if (!storageFsBeginOk && !beginStorageFs()) return;
  File f = SPIFFS.open(HOME_UI_CONFIG_PATH, FILE_WRITE);
  if (!f) return;
  f.print("botlink="); f.println(String(telegramBotLink));
  for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) {
    f.print("clabel"); f.print(k); f.print("="); f.println(String(customLinkLabel[k]));
    f.print("curl"); f.print(k); f.print("="); f.println(String(customLinkUrl[k]));
    f.print("ctype"); f.print(k); f.print("="); f.println(String(customLinkType[k]));
    f.print("cperm"); f.print(k); f.print("=");
    for (byte i = 0; i < WEB_USER_COUNT; i++) f.print(webUserCustomLinkAccess[i][k] ? "1" : "0");
    f.println();
  }
  f.print("badge="); f.println(String(homeBadgeText));
  f.print("kicker="); f.println(String(homeHeroKicker));
  f.print("title="); f.println(String(homeHeroTitle));
  f.print("subtitle="); f.println(String(homeHeroSubtitle));
  f.print("prayerGov="); f.println(String(prayerGovernorateId));
  f.print("showWeather="); f.println(homeShowWeatherSections ? "1" : "0");
  f.print("showLinks="); f.println(homeShowQuickLinksSection ? "1" : "0");
  f.print("showAllowed="); f.println(homeShowAllowedStatusSection ? "1" : "0");
  f.print("showInputCards="); f.println(homeShowInputCardsSection ? "1" : "0");
  f.print("showTech="); f.println(homeShowTechnicalSection ? "1" : "0");
  f.close();
}

void loadTelegramUiConfig() {
  telegramBotLink[0] = '\0';
  applyDefaultHomeHeaderTexts();
  Preferences p;
  if (p.begin("tg_ui", true)) {
    String link = p.getString("botlink", "");
    String badge = p.getString("hbadge", HOME_DEFAULT_BADGE);
    String kicker = p.getString("hkicker", HOME_DEFAULT_KICKER);
    String title = p.getString("htitle", HOME_DEFAULT_TITLE);
    String subtitle = p.getString("hsub", HOME_DEFAULT_SUBTITLE);
    String pgov = p.getString("pgov", "south_sinai");
    setPrayerGovernorateFromId(pgov);
    for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) {
      String defLabel = (k == 0) ? String("data") : String("Link ") + String(k + 1);
      String oldLabel = (k == 0) ? p.getString("dlabel", defLabel) : defLabel;
      String oldUrl = (k == 0) ? p.getString("durl", "") : String("");
      String oldPerm = (k == 0) ? p.getString("dperm", "1") : String("1");
      String keyLabel = String("clabel") + String(k);
      String keyUrl = String("curl") + String(k);
      String keyType = String("ctype") + String(k);
      String keyPerm = String("cperm") + String(k);
      uiCopySingleLineToChar(p.getString(keyLabel.c_str(), oldLabel), customLinkLabel[k], sizeof(customLinkLabel[k]), defLabel.c_str());
      uiCopySingleLineToChar(p.getString(keyUrl.c_str(), oldUrl), customLinkUrl[k], sizeof(customLinkUrl[k]), "");
      customLinkType[k] = sanitizeCustomLinkType(p.getUChar(keyType.c_str(), (k == 0) ? CUSTOM_LINK_TYPE_CSV : CUSTOM_LINK_TYPE_IMAGE));
      String perm = p.getString(keyPerm.c_str(), oldPerm);
      for (byte i = 0; i < WEB_USER_COUNT; i++) webUserCustomLinkAccess[i][k] = (i == 0) ? 1 : 0;
      for (byte i = 0; i < WEB_USER_COUNT && i < perm.length(); i++) webUserCustomLinkAccess[i][k] = (perm.charAt(i) == '1') ? 1 : 0;
      webUserCustomLinkAccess[0][k] = 1;
    }
    homeShowWeatherSections = p.getUChar("showWeather", 1) ? 1 : 0;
    homeShowQuickLinksSection = p.getUChar("showLinks", 1) ? 1 : 0;
    homeShowAllowedStatusSection = p.getUChar("showAllowed", 1) ? 1 : 0;
    homeShowInputCardsSection = p.getUChar("showCards", 1) ? 1 : 0;
    homeShowTechnicalSection = p.getUChar("showTech", 1) ? 1 : 0;
    uiCopySingleLineToChar(link, telegramBotLink, sizeof(telegramBotLink), "");
    uiCopySingleLineToChar(badge, homeBadgeText, sizeof(homeBadgeText), HOME_DEFAULT_BADGE);
    uiCopySingleLineToChar(kicker, homeHeroKicker, sizeof(homeHeroKicker), HOME_DEFAULT_KICKER);
    uiCopySingleLineToChar(title, homeHeroTitle, sizeof(homeHeroTitle), HOME_DEFAULT_TITLE);
    uiCopySingleLineToChar(subtitle, homeHeroSubtitle, sizeof(homeHeroSubtitle), HOME_DEFAULT_SUBTITLE);
    p.end();
  }
  loadHomeUiConfigFromFs();
}


void saveTelegramUiConfig() {
  uiCopySingleLineToChar(String(telegramBotLink), telegramBotLink, sizeof(telegramBotLink), "");
  uiCopySingleLineToChar(String(homeBadgeText), homeBadgeText, sizeof(homeBadgeText), HOME_DEFAULT_BADGE);
  uiCopySingleLineToChar(String(homeHeroKicker), homeHeroKicker, sizeof(homeHeroKicker), HOME_DEFAULT_KICKER);
  uiCopySingleLineToChar(String(homeHeroTitle), homeHeroTitle, sizeof(homeHeroTitle), HOME_DEFAULT_TITLE);
  uiCopySingleLineToChar(String(homeHeroSubtitle), homeHeroSubtitle, sizeof(homeHeroSubtitle), HOME_DEFAULT_SUBTITLE);
  for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) {
    String defLabel = (k == 0) ? String("data") : String("Link ") + String(k + 1);
    uiCopySingleLineToChar(String(customLinkLabel[k]), customLinkLabel[k], sizeof(customLinkLabel[k]), defLabel.c_str());
    uiCopySingleLineToChar(String(customLinkUrl[k]), customLinkUrl[k], sizeof(customLinkUrl[k]), "");
    customLinkType[k] = sanitizeCustomLinkType(customLinkType[k]);
    webUserCustomLinkAccess[0][k] = 1;
  }
  setPrayerGovernorateFromId(String(prayerGovernorateId));
  Preferences p;
  if (p.begin("tg_ui", false)) {
    p.putString("botlink", String(telegramBotLink));
    p.putString("hbadge", String(homeBadgeText));
    p.putString("hkicker", String(homeHeroKicker));
    p.putString("htitle", String(homeHeroTitle));
    p.putString("hsub", String(homeHeroSubtitle));
    p.putString("pgov", String(prayerGovernorateId));
    for (byte k = 0; k < CUSTOM_LINK_COUNT; k++) {
      String keyLabel = String("clabel") + String(k);
      String keyUrl = String("curl") + String(k);
      String keyType = String("ctype") + String(k);
      String keyPerm = String("cperm") + String(k);
      p.putString(keyLabel.c_str(), String(customLinkLabel[k]));
      p.putString(keyUrl.c_str(), String(customLinkUrl[k]));
      p.putUChar(keyType.c_str(), customLinkType[k]);
      String perm = "";
      for (byte i = 0; i < WEB_USER_COUNT; i++) perm += (webUserCustomLinkAccess[i][k] ? '1' : '0');
      p.putString(keyPerm.c_str(), perm);
    }
    p.putUChar("showWeather", homeShowWeatherSections ? 1 : 0);
    p.putUChar("showLinks", homeShowQuickLinksSection ? 1 : 0);
    p.putUChar("showAllowed", homeShowAllowedStatusSection ? 1 : 0);
    p.putUChar("showCards", homeShowInputCardsSection ? 1 : 0);
    p.putUChar("showTech", homeShowTechnicalSection ? 1 : 0);
    p.end();
  }
  saveHomeUiConfigToFs();
}

byte prayerGovernorateIndexById(const String& idIn) {
  String id = idIn;
  id.trim();
  id.toLowerCase();
  // Backward-compatible aliases from older builds.
  if (id == "sharqia") id = "zagazig";
  else if (id == "north_coast") id = "north_coast_marina";
  else if (id == "south_sinai") id = "ras_sedr";
  for (byte i = 0; i < PRAYER_GOV_COUNT; i++) {
    if (id == String(PRAYER_GOVS[i].id)) return i;
  }
  return 0; // Ras Sedr default.
}

void setPrayerGovernorateFromId(String id) {
  byte idx = prayerGovernorateIndexById(id);
  strncpy(prayerGovernorateId, PRAYER_GOVS[idx].id, sizeof(prayerGovernorateId) - 1);
  prayerGovernorateId[sizeof(prayerGovernorateId) - 1] = '\0';
}

byte selectedPrayerGovernorateIndex() {
  return prayerGovernorateIndexById(String(prayerGovernorateId));
}

String prayerGovernorateName(bool ar) {
  byte idx = selectedPrayerGovernorateIndex();
  return ar ? String(PRAYER_GOVS[idx].ar) : String(PRAYER_GOVS[idx].en);
}

String prayerGovernorateCity() {
  return String(PRAYER_GOVS[selectedPrayerGovernorateIndex()].city);
}

String prayerGovernorateLat() {
  return String(PRAYER_GOVS[selectedPrayerGovernorateIndex()].lat);
}

String prayerGovernorateLon() {
  return String(PRAYER_GOVS[selectedPrayerGovernorateIndex()].lon);
}

String prayerWeatherAreaName(bool ar) {
  return prayerGovernorateName(ar);
}

String prayerTodayDateKey() {
  if (!isTimeValidNow()) return String("");
  time_t now = (time_t)getDeviceEpoch();
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday);
  return String(buf);
}

String prayerApiDatePath() {
  if (!isTimeValidNow()) return String("");
  time_t now = (time_t)getDeviceEpoch();
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d-%02d-%04d", tmNow.tm_mday, tmNow.tm_mon + 1, tmNow.tm_year + 1900);
  return String(buf);
}

String prayerNormalizeTime(String t) {
  t.trim();
  if (t.length() >= 5 && t.charAt(2) == ':') return t.substring(0, 5);
  int c = t.indexOf(':');
  if (c > 0 && c + 2 < (int)t.length()) {
    int start = c - 2;
    if (start < 0) start = 0;
    String x = t.substring(start, c + 3);
    x.trim();
    if (x.length() >= 4) return x;
  }
  return String("--:--");
}

String prayerExtractJsonValue(const String& payload, const char* key) {
  String pat = String("\"") + key + "\":\"";
  int p = payload.indexOf(pat);
  if (p < 0) return String("");
  p += pat.length();
  int e = payload.indexOf('"', p);
  if (e < 0) return String("");
  return payload.substring(p, e);
}

void prayerCopyTime(char* dest, size_t len, const String& val) {
  String t = prayerNormalizeTime(val);
  t.toCharArray(dest, len);
}

int prayerTimeToMinutes(const char* t) {
  if (!t || strlen(t) < 5 || t[2] != ':') return -1;
  int h = (t[0] - '0') * 10 + (t[1] - '0');
  int m = (t[3] - '0') * 10 + (t[4] - '0');
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

void prayerSetLastError(const String& err, int code = 0) {
  String e = err;
  e.trim();
  if (!e.length()) e = "none";
  e.substring(0, sizeof(prayerTimesCache.lastError) - 1).toCharArray(prayerTimesCache.lastError, sizeof(prayerTimesCache.lastError));
  prayerTimesCache.lastHttpCode = code;
}

void loadPrayerTimesCacheFromFs() {
  memset(&prayerTimesCache, 0, sizeof(prayerTimesCache));
  prayerTimesCache.valid = false;
  prayerSetLastError("not loaded", 0);
  if (!storageFsBeginOk && !beginStorageFs()) return;
  if (!SPIFFS.exists(PRAYER_CACHE_PATH)) return;
  File f = SPIFFS.open(PRAYER_CACHE_PATH, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim(); val.trim();
    if (key == "date") val.toCharArray(prayerTimesCache.dateKey, sizeof(prayerTimesCache.dateKey));
    else if (key == "gov") val.toCharArray(prayerTimesCache.govId, sizeof(prayerTimesCache.govId));
    else if (key == "city") val.toCharArray(prayerTimesCache.city, sizeof(prayerTimesCache.city));
    else if (key == "fajr") prayerCopyTime(prayerTimesCache.fajr, sizeof(prayerTimesCache.fajr), val);
    else if (key == "sunrise") prayerCopyTime(prayerTimesCache.sunrise, sizeof(prayerTimesCache.sunrise), val);
    else if (key == "dhuhr") prayerCopyTime(prayerTimesCache.dhuhr, sizeof(prayerTimesCache.dhuhr), val);
    else if (key == "asr") prayerCopyTime(prayerTimesCache.asr, sizeof(prayerTimesCache.asr), val);
    else if (key == "maghrib") prayerCopyTime(prayerTimesCache.maghrib, sizeof(prayerTimesCache.maghrib), val);
    else if (key == "isha") prayerCopyTime(prayerTimesCache.isha, sizeof(prayerTimesCache.isha), val);
    else if (key == "updated") val.toCharArray(prayerTimesCache.updatedAt, sizeof(prayerTimesCache.updatedAt));
    yield();
  }
  f.close();
  prayerTimesCache.valid = prayerTimeToMinutes(prayerTimesCache.fajr) >= 0 && prayerTimeToMinutes(prayerTimesCache.dhuhr) >= 0 && prayerTimeToMinutes(prayerTimesCache.isha) >= 0;
  prayerTimesCache.fromCache = prayerTimesCache.valid;
  if (prayerTimesCache.valid) prayerSetLastError("cached", 200);
}

void savePrayerTimesCacheToFs() {
  if (!prayerTimesCache.valid) return;
  if (!storageFsBeginOk && !beginStorageFs()) return;
  File f = SPIFFS.open(PRAYER_CACHE_PATH, FILE_WRITE);
  if (!f) return;
  f.print("date="); f.println(prayerTimesCache.dateKey);
  f.print("gov="); f.println(prayerTimesCache.govId);
  f.print("city="); f.println(prayerTimesCache.city);
  f.print("fajr="); f.println(prayerTimesCache.fajr);
  f.print("sunrise="); f.println(prayerTimesCache.sunrise);
  f.print("dhuhr="); f.println(prayerTimesCache.dhuhr);
  f.print("asr="); f.println(prayerTimesCache.asr);
  f.print("maghrib="); f.println(prayerTimesCache.maghrib);
  f.print("isha="); f.println(prayerTimesCache.isha);
  f.print("updated="); f.println(prayerTimesCache.updatedAt);
  f.close();
}

bool prayerCacheMatchesToday() {
  if (!prayerTimesCache.valid) return false;
  if (String(prayerTimesCache.govId) != String(prayerGovernorateId)) return false;
  String today = prayerTodayDateKey();
  if (today.length() && today != String(prayerTimesCache.dateKey)) return false;
  return true;
}

bool fetchPrayerTimesFromApi(bool forceRefresh) {
  if (!forceRefresh && prayerCacheMatchesToday() && lastPrayerFetchMs != 0 && millis() - lastPrayerFetchMs < PRAYER_CACHE_REFRESH_MS) return true;
  if (WiFi.status() != WL_CONNECTED) {
    prayerSetLastError("WiFi offline", 0);
    return false;
  }

  String city = prayerGovernorateCity();
  String apiDate = prayerApiDatePath();
  String url;
  if (apiDate.length()) {
    // V1.7.8.24: coordinates endpoint avoids city-name differences and keeps Prayer/Weather on the same selected area.
    url = String("https://api.aladhan.com/v1/timings/") + apiDate + "?latitude=" + prayerGovernorateLat() + "&longitude=" + prayerGovernorateLon() + "&method=" + String(PRAYER_API_METHOD);
  } else {
    url = String("https://api.aladhan.com/v1/timings?latitude=") + prayerGovernorateLat() + "&longitude=" + prayerGovernorateLon() + "&method=" + String(PRAYER_API_METHOD);
  }
  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  if (!http.begin(secure, url)) {
    prayerSetLastError("API begin failed", -1);
    return false;
  }
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  int code = http.GET();
  prayerTimesCache.lastHttpCode = code;
  if (code != HTTP_CODE_OK) {
    http.end();
    prayerSetLastError(String("HTTP ") + String(code), code);
    return false;
  }
  String payload = http.getString();
  http.end();

  String fajr = prayerExtractJsonValue(payload, "Fajr");
  String sunrise = prayerExtractJsonValue(payload, "Sunrise");
  String dhuhr = prayerExtractJsonValue(payload, "Dhuhr");
  String asr = prayerExtractJsonValue(payload, "Asr");
  String maghrib = prayerExtractJsonValue(payload, "Maghrib");
  String isha = prayerExtractJsonValue(payload, "Isha");
  if (!fajr.length() || !dhuhr.length() || !asr.length() || !maghrib.length() || !isha.length()) {
    prayerSetLastError("API parse failed", code);
    return false;
  }

  memset(&prayerTimesCache, 0, sizeof(prayerTimesCache));
  prayerTimesCache.valid = true;
  prayerTimesCache.fromCache = false;
  prayerTimesCache.lastHttpCode = code;
  prayerTodayDateKey().toCharArray(prayerTimesCache.dateKey, sizeof(prayerTimesCache.dateKey));
  if (!prayerTimesCache.dateKey[0]) strncpy(prayerTimesCache.dateKey, "today", sizeof(prayerTimesCache.dateKey) - 1);
  String(prayerGovernorateId).toCharArray(prayerTimesCache.govId, sizeof(prayerTimesCache.govId));
  prayerWeatherAreaName(false).toCharArray(prayerTimesCache.city, sizeof(prayerTimesCache.city));
  prayerCopyTime(prayerTimesCache.fajr, sizeof(prayerTimesCache.fajr), fajr);
  prayerCopyTime(prayerTimesCache.sunrise, sizeof(prayerTimesCache.sunrise), sunrise);
  prayerCopyTime(prayerTimesCache.dhuhr, sizeof(prayerTimesCache.dhuhr), dhuhr);
  prayerCopyTime(prayerTimesCache.asr, sizeof(prayerTimesCache.asr), asr);
  prayerCopyTime(prayerTimesCache.maghrib, sizeof(prayerTimesCache.maghrib), maghrib);
  prayerCopyTime(prayerTimesCache.isha, sizeof(prayerTimesCache.isha), isha);
  currentDeviceDateTimeText().toCharArray(prayerTimesCache.updatedAt, sizeof(prayerTimesCache.updatedAt));
  prayerSetLastError("OK", code);
  lastPrayerFetchMs = millis();
  savePrayerTimesCacheToFs();
  addEsp32Log(String("Prayer times updated: ") + prayerWeatherAreaName(false));
  return true;
}

bool ensurePrayerTimesFresh(bool forceRefresh) {
  if (!prayerTimesCache.valid) loadPrayerTimesCacheFromFs();
  if (!forceRefresh && prayerCacheMatchesToday()) return true;
  bool ok = fetchPrayerTimesFromApi(forceRefresh);
  if (!ok && prayerTimesCache.valid) {
    prayerTimesCache.fromCache = true;
    return true;
  }
  return ok;
}

void prayerComputeNext(bool ar, String& nextName, uint32_t& secondsLeft) {
  nextName = ar ? String("غير معروف") : String("Unknown");
  secondsLeft = 0;
  if (!prayerTimesCache.valid || !isTimeValidNow()) return;
  time_t now = (time_t)getDeviceEpoch();
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  int nowSec = tmNow.tm_hour * 3600 + tmNow.tm_min * 60 + tmNow.tm_sec;
  const char* namesEn[5] = {"Fajr", "Dhuhr", "Asr", "Maghrib", "Isha"};
  const char* namesAr[5] = {"الفجر", "الظهر", "العصر", "المغرب", "العشاء"};
  int mins[5] = {
    prayerTimeToMinutes(prayerTimesCache.fajr),
    prayerTimeToMinutes(prayerTimesCache.dhuhr),
    prayerTimeToMinutes(prayerTimesCache.asr),
    prayerTimeToMinutes(prayerTimesCache.maghrib),
    prayerTimeToMinutes(prayerTimesCache.isha)
  };
  for (byte i = 0; i < 5; i++) {
    if (mins[i] < 0) continue;
    int targetSec = mins[i] * 60;
    if (targetSec > nowSec) {
      nextName = ar ? String(namesAr[i]) : String(namesEn[i]);
      secondsLeft = (uint32_t)(targetSec - nowSec);
      return;
    }
  }
  if (mins[0] >= 0) {
    nextName = ar ? String("فجر الغد") : String("Tomorrow Fajr");
    secondsLeft = (uint32_t)(86400 - nowSec + mins[0] * 60);
  }
}

String prayerFormatDuration(uint32_t sec, bool ar) {
  uint32_t h = sec / 3600UL;
  uint32_t m = (sec % 3600UL) / 60UL;
  uint32_t s = sec % 60UL;
  char buf[32];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
  return String(buf);
}


String prayerDisplayClock12(const char* rawTime, bool ar) {
  int mins = prayerTimeToMinutes(rawTime);
  if (mins < 0) return String(rawTime ? rawTime : "--");
  byte h24 = (byte)(mins / 60);
  byte mm = (byte)(mins % 60);
  bool pm = h24 >= 12;
  byte h12 = h24 % 12;
  if (h12 == 0) h12 = 12;
  char buf[10];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)h12, (unsigned)mm);
  String x = String(buf);
  x += " ";
  x += ar ? (pm ? String("م") : String("ص")) : (pm ? String("PM") : String("AM"));
  return x;
}

String prayerTimesTelegramText(const String& chatId, bool forceRefresh) {
  bool ar = telegramChatIsArabic(chatId);
  bool ok = ensurePrayerTimesFresh(forceRefresh);
  String gov = prayerWeatherAreaName(ar);
  String msg;
  msg += ar ? "🕌 مواقيت الصلاة - " : "🕌 Prayer Times - ";
  msg += gov;
  if (!ok && !prayerTimesCache.valid) {
    msg += ar ? "\n\nتعذر جلب مواقيت الصلاة الآن. تأكد من الإنترنت ثم حاول مرة أخرى." : "\n\nCould not fetch prayer times now. Check internet and try again.";
    msg += "\n";
    msg += prayerTimesCache.lastError;
    return msg;
  }
  String nextName;
  uint32_t left = 0;
  prayerComputeNext(ar, nextName, left);
  msg += "\n";
  msg += ar ? "الفجر: " : "Fajr: "; msg += prayerDisplayClock12(prayerTimesCache.fajr, ar);
  msg += ar ? "\nالشروق: " : "\nSunrise: "; msg += prayerDisplayClock12(prayerTimesCache.sunrise, ar);
  msg += ar ? "\nالظهر: " : "\nDhuhr: "; msg += prayerDisplayClock12(prayerTimesCache.dhuhr, ar);
  msg += ar ? "\nالعصر: " : "\nAsr: "; msg += prayerDisplayClock12(prayerTimesCache.asr, ar);
  msg += ar ? "\nالمغرب: " : "\nMaghrib: "; msg += prayerDisplayClock12(prayerTimesCache.maghrib, ar);
  msg += ar ? "\nالعشاء: " : "\nIsha: "; msg += prayerDisplayClock12(prayerTimesCache.isha, ar);
  msg += "\n\n⏳ ";
  msg += ar ? "الصلاة القادمة: " : "Next prayer: "; msg += nextName;
  msg += "\n";
  msg += ar ? "باقي على " : "Remaining to "; msg += nextName; msg += ar ? ": " : ": "; msg += prayerFormatDuration(left, ar);
  msg += "\n";
  msg += ar ? "آخر تحديث: " : "Last update: "; msg += prayerTimesCache.updatedAt[0] ? String(prayerTimesCache.updatedAt) : String("--");
  if (prayerTimesCache.fromCache) msg += ar ? "\n⚠️ يتم عرض آخر بيانات محفوظة." : "\n⚠️ Showing cached data.";
  return msg;
}

String prayerGovernorateSelectHtml(bool ar) {
  String html;
  html.reserve(5200);
  html += F("<select name='pgov'>");
  const char* lastGroup = nullptr;
  for (byte i = 0; i < PRAYER_GOV_COUNT; i++) {
    const char* g = ar ? PRAYER_GOVS[i].groupAr : PRAYER_GOVS[i].groupEn;
    if (lastGroup == nullptr || strcmp(lastGroup, g) != 0) {
      if (lastGroup != nullptr) html += F("</optgroup>");
      html += F("<optgroup label='"); html += htmlEscapeText(String(g)); html += F("'>");
      lastGroup = g;
    }
    html += F("<option value='"); html += PRAYER_GOVS[i].id; html += F("'");
    if (String(PRAYER_GOVS[i].id) == String(prayerGovernorateId)) html += F(" selected");
    html += F(">");
    html += ar ? PRAYER_GOVS[i].ar : PRAYER_GOVS[i].en;
    html += F("</option>");
  }
  if (lastGroup != nullptr) html += F("</optgroup>");
  html += F("</select>");
  return html;
}

void sendPrayerTimesPage() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  byte lang = currentWebLanguage();
  bool ar = (lang == TG_LANG_AR);
  bool ok = ensurePrayerTimesFresh(false);
  String nextName;
  uint32_t left = 0;
  prayerComputeNext(ar, nextName, left);
  String html = htmlHeader(ar ? String("مواقيت الصلاة") : String("Prayer Times"));
  html.reserve(7600);
  if (ar) html += F("<div dir='rtl' style='text-align:right'>");
  html += F("<div class='card heroCard'><div class='heroTop'><div><div class='heroKicker'>PRAYER TIMES</div><h1>🕌 ");
  html += ar ? F("مواقيت الصلاة") : F("Prayer Times");
  html += F(" - "); html += prayerGovernorateName(ar);
  html += F("</h1><div class='sub heroSub'>");
  html += ar ? F("الأدمن فقط يختار المحافظة. الصفحة تعرض الصلاة القادمة والوقت المتبقي.") : F("Admin selects the governorate. This page shows the next prayer and remaining time.");
  html += F("</div></div><div class='heroBadge'>"); html += prayerTimesCache.city[0] ? htmlEscapeText(String(prayerTimesCache.city)) : htmlEscapeText(prayerGovernorateCity()); html += F("</div></div></div>");

  if (!ok && !prayerTimesCache.valid) {
    html += F("<div class='card'><h1>⚠️ "); html += ar ? F("تعذر تحميل المواقيت") : F("Could not load times"); html += F("</h1><div class='sub'>");
    html += htmlEscapeText(String(prayerTimesCache.lastError));
    html += F("</div><a class='btn btn2' href='"); html += sessionUrl("/prayerrefresh"); html += F("'>"); html += ar ? F("حاول التحديث") : F("Try Refresh"); html += F("</a></div>");
  } else {
    html += F("<div class='card'><h1>⏳ "); html += ar ? F("الصلاة القادمة") : F("Next Prayer"); html += F("</h1>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("الصلاة") : F("Prayer"); html += F("</span><span class='v'>"); html += htmlEscapeText(nextName); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("الوقت المتبقي") : F("Remaining"); html += F("</span><span class='v' id='prayLeft' data-left='"); html += String(left); html += F("'>"); html += prayerFormatDuration(left, ar); html += F("</span></div>");
    html += F("<div class='sub'>");
    if (prayerTimesCache.fromCache) html += ar ? F("⚠️ يتم عرض آخر بيانات محفوظة عند تعذر التحديث.") : F("⚠️ Cached data is shown when online update is not available.");
    else html += ar ? F("تم جلب المواقيت من الإنترنت وحفظها كنسخة احتياطية.") : F("Times were fetched online and cached locally.");
    html += F("</div></div>");

    html += F("<div class='card'><h1>🔔 "); html += ar ? F("خرج الأذان") : F("Adhan Output"); html += F("</h1>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("التفعيل") : F("Enabled"); html += F("</span><span class='v'>"); html += adhanOutputEnabled ? (ar ? F("مفعل") : F("Enabled")) : (ar ? F("مغلق") : F("Disabled")); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>GPIO</span><span class='v'>"); html += String(adhanOutputPin); html += F(" / "); html += adhanOutputState ? F("ON") : F("OFF"); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("المدة") : F("Duration"); html += F("</span><span class='v'>"); html += String(adhanOutputDurationSec); html += ar ? F(" ثانية") : F(" sec"); html += F("</span></div>");
    html += F("</div>");

    html += F("<div class='card'><h1>🕌 "); html += ar ? F("مواقيت اليوم") : F("Today Times"); html += F("</h1>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("الفجر") : F("Fajr"); html += F("</span><span class='v'>"); html += prayerDisplayClock12(prayerTimesCache.fajr, ar); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("الشروق") : F("Sunrise"); html += F("</span><span class='v'>"); html += prayerDisplayClock12(prayerTimesCache.sunrise, ar); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("الظهر") : F("Dhuhr"); html += F("</span><span class='v'>"); html += prayerDisplayClock12(prayerTimesCache.dhuhr, ar); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("العصر") : F("Asr"); html += F("</span><span class='v'>"); html += prayerDisplayClock12(prayerTimesCache.asr, ar); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("المغرب") : F("Maghrib"); html += F("</span><span class='v'>"); html += prayerDisplayClock12(prayerTimesCache.maghrib, ar); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("العشاء") : F("Isha"); html += F("</span><span class='v'>"); html += prayerDisplayClock12(prayerTimesCache.isha, ar); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("آخر تحديث") : F("Last Update"); html += F("</span><span class='v'>"); html += htmlEscapeText(String(prayerTimesCache.updatedAt)); html += F("</span></div>");
    html += F("</div>");
  }
  html += F("<div class='card'><a class='btn btn2' href='"); html += sessionUrl("/prayerrefresh"); html += F("'>🔄 "); html += ar ? F("تحديث الآن") : F("Refresh Now"); html += F("</a><a class='btn btn2' href='"); html += sessionUrl("/"); html += F("'>"); html += ar ? F("رجوع للهوم") : F("Back to Home"); html += F("</a></div>");
  html += F("<script>function fmtPrayerLeft(x){x=Math.max(0,x|0);let h=Math.floor(x/3600),m=Math.floor((x%3600)/60),s=x%60;return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(s).padStart(2,'0')}let pe=document.getElementById('prayLeft');if(pe){let left=parseInt(pe.getAttribute('data-left')||'0');setInterval(()=>{left--;if(left<=0){location.reload();return;}pe.textContent=fmtPrayerLeft(left);},1000);}</script>");
  if (ar) html += F("</div>");
  html += htmlFooter();
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.send(200, "text/html", html);
}

void handlePrayerTimesRefresh() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  ensurePrayerTimesFresh(true);
  webServer.sendHeader("Location", sessionUrl("/prayertimes"), true);
  webServer.send(302, "text/plain", "");
}

bool isSafeAdhanOutputGpio(byte pin) {
  if (pin > 33) return false;
  if (pin == 1 || pin == 3) return false;           // UART0
  if (pin >= 6 && pin <= 11) return false;          // flash pins
  if (pin == OUTPUT1_PIN || pin == OUTPUT2_PIN || pin == OUTPUT5_PIN || pin == OUTPUT6_PIN || pin == OUTPUT7_PIN || pin == OUTPUT8_PIN) return false;
  if (pin == INPUT7_PIN || pin == INPUT8_PIN || pin == INPUT9_PIN || pin == INPUT10_PIN) return false;
  if (pin == INTERNET_LED_PIN || pin == STATUS_LED_PIN || pin == DHTPIN) return false;
  if (pin == 21 || pin == 22) return false;         // RTC I2C default
  return true;
}

void adhanWriteOutput(byte state) {
  adhanOutputState = state ? 1 : 0;
  byte level;
  if (adhanOutputActiveHigh) level = adhanOutputState ? HIGH : LOW;
  else level = adhanOutputState ? LOW : HIGH;
  digitalWrite(adhanOutputPin, level);
}

void setupAdhanOutputPin() {
  if (!isSafeAdhanOutputGpio(adhanOutputPin)) adhanOutputPin = ADHAN_DEFAULT_GPIO;
  pinMode(adhanOutputPin, OUTPUT);
  adhanOutputPulseActive = false;
  adhanOutputOffDueMs = 0;
  adhanWriteOutput(0);
}

uint32_t adhanTodayKey() {
  int y = 0; byte m = 0; byte d = 0;
  if (!getDeviceDateParts(y, m, d)) return 0;
  return (uint32_t)y * 10000UL + (uint32_t)m * 100UL + (uint32_t)d;
}

void saveAdhanOutputConfigToFs() {
  if (!storageFsBeginOk && !beginStorageFs()) return;
  File f = SPIFFS.open(ADHAN_SETTINGS_PATH, FILE_WRITE);
  if (!f) return;
  f.print("enabled="); f.println(adhanOutputEnabled ? 1 : 0);
  f.print("gpio="); f.println(adhanOutputPin);
  f.print("activeHigh="); f.println(adhanOutputActiveHigh ? 1 : 0);
  f.print("duration="); f.println(adhanOutputDurationSec);
  f.print("mask="); f.println(adhanPrayerMask);
  f.print("tg="); f.println(adhanTelegramNotifyEnabled ? 1 : 0);
  f.print("lastKey="); f.println(adhanLastExecutionKey);
  f.close();
}

void loadAdhanOutputConfigFromFs() {
  if (!storageFsBeginOk && !beginStorageFs()) return;
  if (!SPIFFS.exists(ADHAN_SETTINGS_PATH)) return;
  File f = SPIFFS.open(ADHAN_SETTINGS_PATH, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim(); val.trim();
    if (key == "enabled") adhanOutputEnabled = val.toInt() == 1;
    else if (key == "gpio") adhanOutputPin = (byte)val.toInt();
    else if (key == "activeHigh") adhanOutputActiveHigh = val.toInt() ? 1 : 0;
    else if (key == "duration") { long d = val.toInt(); if (d >= 1 && d <= 3600) adhanOutputDurationSec = (uint16_t)d; }
    else if (key == "mask") { int m = val.toInt(); if (m > 0 && m <= ADHAN_PRAYER_ALL) adhanPrayerMask = (byte)m; }
    else if (key == "tg") adhanTelegramNotifyEnabled = val.toInt() == 1;
    else if (key == "lastKey") adhanLastExecutionKey = (uint32_t)val.toInt();
    yield();
  }
  f.close();
}

void saveAdhanOutputConfig() {
  if (!isSafeAdhanOutputGpio(adhanOutputPin)) adhanOutputPin = ADHAN_DEFAULT_GPIO;
  if (adhanOutputDurationSec < 1) adhanOutputDurationSec = 1;
  if (adhanOutputDurationSec > 3600) adhanOutputDurationSec = 3600;
  if ((adhanPrayerMask & ADHAN_PRAYER_ALL) == 0) adhanPrayerMask = ADHAN_PRAYER_ALL;
  Preferences p;
  if (p.begin("adhan_out", false)) {
    p.putBool("en", adhanOutputEnabled);
    p.putUChar("gpio", adhanOutputPin);
    p.putUChar("ah", adhanOutputActiveHigh ? 1 : 0);
    p.putUShort("dur", adhanOutputDurationSec);
    p.putUChar("mask", adhanPrayerMask);
    p.putBool("tg", adhanTelegramNotifyEnabled);
    p.putUInt("last", adhanLastExecutionKey);
    p.end();
  }
  saveAdhanOutputConfigToFs();
}

void loadAdhanOutputConfig() {
  adhanOutputEnabled = false;
  adhanOutputPin = ADHAN_DEFAULT_GPIO;
  adhanOutputActiveHigh = 1;
  adhanOutputDurationSec = 60;
  adhanPrayerMask = ADHAN_PRAYER_ALL;
  adhanTelegramNotifyEnabled = true;
  adhanLastExecutionKey = 0;
  Preferences p;
  if (p.begin("adhan_out", true)) {
    adhanOutputEnabled = p.getBool("en", adhanOutputEnabled);
    adhanOutputPin = p.getUChar("gpio", adhanOutputPin);
    adhanOutputActiveHigh = p.getUChar("ah", adhanOutputActiveHigh) ? 1 : 0;
    adhanOutputDurationSec = p.getUShort("dur", adhanOutputDurationSec);
    adhanPrayerMask = p.getUChar("mask", adhanPrayerMask);
    adhanTelegramNotifyEnabled = p.getBool("tg", adhanTelegramNotifyEnabled);
    adhanLastExecutionKey = p.getUInt("last", 0);
    p.end();
  }
  loadAdhanOutputConfigFromFs();
  if (!isSafeAdhanOutputGpio(adhanOutputPin)) adhanOutputPin = ADHAN_DEFAULT_GPIO;
  if (adhanOutputDurationSec < 1 || adhanOutputDurationSec > 3600) adhanOutputDurationSec = 60;
  if ((adhanPrayerMask & ADHAN_PRAYER_ALL) == 0) adhanPrayerMask = ADHAN_PRAYER_ALL;
}

String adhanPrayerNameByIndex(byte idx, bool ar) {
  const char* en[5] = {"Fajr", "Dhuhr", "Asr", "Maghrib", "Isha"};
  const char* aa[5] = {"الفجر", "الظهر", "العصر", "المغرب", "العشاء"};
  if (idx > 4) return ar ? String("الأذان") : String("Adhan");
  return ar ? String(aa[idx]) : String(en[idx]);
}

int adhanPrayerMinuteByIndex(byte idx) {
  switch (idx) {
    case 0: return prayerTimeToMinutes(prayerTimesCache.fajr);
    case 1: return prayerTimeToMinutes(prayerTimesCache.dhuhr);
    case 2: return prayerTimeToMinutes(prayerTimesCache.asr);
    case 3: return prayerTimeToMinutes(prayerTimesCache.maghrib);
    case 4: return prayerTimeToMinutes(prayerTimesCache.isha);
  }
  return -1;
}

byte adhanPrayerBitByIndex(byte idx) {
  switch (idx) {
    case 0: return ADHAN_PRAYER_FAJR;
    case 1: return ADHAN_PRAYER_DHUHR;
    case 2: return ADHAN_PRAYER_ASR;
    case 3: return ADHAN_PRAYER_MAGHRIB;
    case 4: return ADHAN_PRAYER_ISHA;
  }
  return 0;
}

int telegramNotifyAdhanUsers(const String& text) {
  if (!adhanTelegramNotifyEnabled || !telegramEnabled || !telegramBotToken[0] || WiFi.status() != WL_CONNECTED) return 0;
  int sent = 0;
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    if (!WEB_USERS[i].enabled || !WEB_USERS[i].user[0]) continue;
    if (!webUserAdhanNotify[i] || !webUserTelegramChatId[i][0]) continue;
    bool dup = false;
    for (byte j = 0; j < i; j++) {
      if (webUserAdhanNotify[j] && webUserTelegramChatId[j][0] && strcmp(webUserTelegramChatId[j], webUserTelegramChatId[i]) == 0) { dup = true; break; }
    }
    if (dup) continue;
    telegramSendToChat(String(webUserTelegramChatId[i]), text);
    sent++;
    yield();
  }
  return sent;
}

String startupTelegramReportText(const String& chatId) {
  bool ar = telegramChatIsArabic(chatId);
  String msg;
  msg.reserve(1700);
  msg += ar ? String("✅ تم تشغيل الجهاز") : String("✅ Device started");
  msg += "\n\n📌 ";
  String title = String(homeHeroTitle); title.trim();
  msg += title.length() ? title : String(FW_DEVICE_NAME);
  msg += "\n🔢 V"; msg += String(FW_VERSION); msg += " / Build "; msg += String(FW_BUILD);
  msg += "\n📶 WiFi: "; msg += (WiFi.status() == WL_CONNECTED ? "Connected" : "Offline");
  msg += "\n🌐 IP: "; msg += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("--"));
  msg += "\n🕒 "; msg += ar ? String("مصدر الوقت: ") : String("Time source: "); msg += clockStateText();
  msg += "\n🧭 RTC: "; msg += rtcPresent ? (rtcTimeValid ? "OK" : "Present / No valid time") : "No RTC";
  msg += "\n💾 Free Heap: "; msg += String(ESP.getFreeHeap()); msg += " bytes";
  msg += "\n📁 Storage: "; msg += storageFsBeginOk ? "OK" : "Check";

  msg += ar ? String("\n\n🌡️ قراءة الحساس الحية:") : String("\n\n🌡️ Live device sensor:");
  msg += "\nTemp: "; msg += (sensorReadOk && !isnan(currentTemperature)) ? String(currentTemperature, 1) : String("--"); msg += " °C";
  msg += "\nHumidity: "; msg += (sensorReadOk && !isnan(currentHumidity)) ? String(currentHumidity, 0) : String("--"); msg += " %";

  updateCityWeather(false);
  msg += ar ? String("\n\n🌐 طقس الإنترنت - ") : String("\n\n🌐 Internet weather - ");
  msg += prayerWeatherAreaName(ar);
  msg += "\nTemp: "; msg += (cityWeatherOk && !isnan(cityWeatherTemp)) ? String(cityWeatherTemp, 1) : String("--"); msg += " °C";
  msg += "\nHumidity: "; msg += (cityWeatherOk && !isnan(cityWeatherHum)) ? String(cityWeatherHum, 0) : String("--"); msg += " %";

  ensurePrayerTimesFresh(false);
  String nextName; uint32_t left = 0;
  prayerComputeNext(ar, nextName, left);
  msg += ar ? String("\n\n🕌 الصلاة القادمة: ") : String("\n\n🕌 Next prayer: "); msg += nextName;
  msg += ar ? String("\n⏳ فاضل: ") : String("\n⏳ Remaining: "); msg += prayerFormatDuration(left, ar);

  msg += ar ? String("\n\n🔌 حالة المخارج:") : String("\n\n🔌 Outputs:");
  msg += "\n"; msg += fieldLabel(1); msg += ": "; msg += currentOutput1State ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(2); msg += ": "; msg += currentOutput2State ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(3); msg += ": "; msg += smartOutputState ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(5); msg += ": "; msg += currentOutput5State ? "ON" : "OFF";
  msg += "\n"; msg += fieldLabel(6); msg += ": "; msg += currentOutput6State ? "ON" : "OFF";
  msg += "\n"; msg += outputDisplayLabel(7); msg += ": "; msg += currentOutput7State ? "ON" : "OFF";
  msg += "\n"; msg += outputDisplayLabel(8); msg += ": "; msg += currentOutput8State ? "ON" : "OFF";

  msg += "\n\n"; msg += currentDeviceDateTimeText();
  return msg;
}

int telegramNotifyStartupUsers() {
  if (!telegramEnabled || !telegramBotToken[0] || WiFi.status() != WL_CONNECTED) return 0;
  int sent = 0;
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    if (!WEB_USERS[i].enabled || !WEB_USERS[i].user[0]) continue;
    if (!webUserStartupNotify[i] || !webUserTelegramChatId[i][0]) continue;
    bool dup = false;
    for (byte j = 0; j < i; j++) {
      if (webUserStartupNotify[j] && webUserTelegramChatId[j][0] && strcmp(webUserTelegramChatId[j], webUserTelegramChatId[i]) == 0) { dup = true; break; }
    }
    if (dup) continue;
    String chatId = String(webUserTelegramChatId[i]);
    telegramSendToChat(chatId, startupTelegramReportText(chatId));
    sent++;
    yield();
  }
  return sent;
}

void maintainStartupTelegramReport() {
  if (startupTelegramReportSent) return;
  unsigned long now = millis();
  if (now < STARTUP_TELEGRAM_REPORT_DELAY_MS) return;
  if (lastStartupTelegramReportTryMs != 0 && now - lastStartupTelegramReportTryMs < STARTUP_TELEGRAM_REPORT_RETRY_MS) return;
  lastStartupTelegramReportTryMs = now;
  if (!telegramEnabled || !telegramBotToken[0] || WiFi.status() != WL_CONNECTED) return;
  int sent = telegramNotifyStartupUsers();
  if (sent > 0) {
    startupTelegramReportSent = true;
    addEsp32Log(String("Startup Telegram report sent to ") + String(sent) + " user(s)");
  } else if (now > 300000UL) {
    startupTelegramReportSent = true;
    addEsp32Log("Startup Telegram report skipped: no selected users");
  }
}

void startAdhanOutputPulse(const String& prayerName, bool notifyUsers) {
  if (!isSafeAdhanOutputGpio(adhanOutputPin)) adhanOutputPin = ADHAN_DEFAULT_GPIO;
  pinMode(adhanOutputPin, OUTPUT);
  adhanOutputPulseActive = true;
  adhanOutputOffDueMs = millis() + ((unsigned long)adhanOutputDurationSec * 1000UL);
  adhanWriteOutput(1);
  String logMsg = String("Adhan output ON: ") + prayerName + " GPIO" + String(adhanOutputPin) + " for " + String(adhanOutputDurationSec) + " sec";
  addEsp32Log(logMsg);
  if (notifyUsers) {
    String msg = String("🕌 حان الآن أذان ") + prayerName + "\n";
    msg += String("📍 ") + prayerWeatherAreaName(true) + "\n";
    msg += String("🔔 تنبيه الأذان لمدة ") + String(adhanOutputDurationSec) + String(" ثانية") + "\n";
    msg += currentDeviceDateTimeText();
    telegramNotifyAdhanUsers(msg);
  }
}

void maintainAdhanOutputAutomation() {
  if (adhanOutputPulseActive && (long)(millis() - adhanOutputOffDueMs) >= 0) {
    adhanOutputPulseActive = false;
    adhanOutputOffDueMs = 0;
    adhanWriteOutput(0);
    addEsp32Log("Adhan output OFF");
  }

  if (!adhanOutputEnabled) return;
  if ((unsigned long)(millis() - lastAdhanCheckMs) < ADHAN_CHECK_INTERVAL_MS) return;
  lastAdhanCheckMs = millis();
  if (!isTimeValidNow()) return;
  if (!prayerCacheMatchesToday()) {
    if (!ensurePrayerTimesFresh(false)) return;
  }
  if (!prayerCacheMatchesToday()) return;
  time_t now = (time_t)getDeviceEpoch();
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  int nowSec = tmNow.tm_hour * 3600 + tmNow.tm_min * 60 + tmNow.tm_sec;
  uint32_t dayKey = adhanTodayKey();
  if (dayKey == 0) return;
  for (byte i = 0; i < 5; i++) {
    byte bit = adhanPrayerBitByIndex(i);
    if ((adhanPrayerMask & bit) == 0) continue;
    int mins = adhanPrayerMinuteByIndex(i);
    if (mins < 0) continue;
    int targetSec = mins * 60;
    if (nowSec >= targetSec && nowSec < targetSec + 60) {
      uint32_t execKey = dayKey * 10UL + (uint32_t)(i + 1);
      if (adhanLastExecutionKey == execKey) return;
      adhanLastExecutionKey = execKey;
      saveAdhanOutputConfig();
      startAdhanOutputPulse(adhanPrayerNameByIndex(i, true), true);
      return;
    }
  }
}

String adhanOutputSettingsCardHtml(bool ar) {
  String html;
  html.reserve(1800);
  html += F("<div class='card'><h1>🕌 "); html += ar ? F("خرج الأذان المستقل") : F("Dedicated Adhan Output"); html += F("</h1><div class='sub'>");
  html += ar ? F("خرج جديد مستقل لا يستخدم مخارج التحكم الحالية. الإشعارات تذهب فقط للمستخدمين المحددين من صفحة Users بخانة Adhan Notify.") : F("A dedicated GPIO output, separate from the normal outputs. Notifications are sent only to users selected on the Users page with Adhan Notify.");
  html += F("</div><form method='POST' action='/savehomeui'>");
  html += sessionHiddenInput();
  html += F("<input type='hidden' name='adhcfg' value='1'>");
  html += F("<label class='small'><input type='checkbox' name='adhen' "); if (adhanOutputEnabled) html += F("checked"); html += F("> "); html += ar ? F("تفعيل خرج الأذان") : F("Enable Adhan Output"); html += F("</label>");
  html += F("<label>"); html += ar ? F("GPIO الخرج الجديد") : F("New output GPIO"); html += F("</label><input name='adhgpio' type='number' min='0' max='33' value='"); html += String(adhanOutputPin); html += F("'>");
  html += F("<div class='sub'>"); html += ar ? F("الافتراضي GPIO5. تجنب الأرجل المستخدمة بالفعل مثل 18/21/22/25/26/27/32/33.") : F("Default GPIO5. Avoid pins already used such as 18/21/22/25/26/27/32/33."); html += F("</div>");
  html += F("<label>"); html += ar ? F("مستوى التشغيل") : F("Active Level"); html += F("</label><select name='adhlevel'><option value='1'"); if (adhanOutputActiveHigh) html += F(" selected"); html += F(">Active HIGH</option><option value='0'"); if (!adhanOutputActiveHigh) html += F(" selected"); html += F(">Active LOW</option></select>");
  html += F("<label>"); html += ar ? F("مدة التشغيل بالثواني") : F("ON duration in seconds"); html += F("</label><input name='adhdur' type='number' min='1' max='3600' value='"); html += String(adhanOutputDurationSec); html += F("'>");
  html += F("<div class='smsec'><b>"); html += ar ? F("الصلوات") : F("Prayers"); html += F("</b><div class='grid2'>");
  const char* nms[5] = {"adhfajr", "adhdhuhr", "adhasr", "adhmaghrib", "adhisha"};
  for (byte i = 0; i < 5; i++) { html += F("<label class='small'><input type='checkbox' name='"); html += nms[i]; html += F("' "); if (adhanPrayerMask & adhanPrayerBitByIndex(i)) html += F("checked"); html += F("> "); html += adhanPrayerNameByIndex(i, ar); html += F("</label>"); }
  html += F("</div></div>");
  html += F("<label class='small'><input type='checkbox' name='adhtg' "); if (adhanTelegramNotifyEnabled) html += F("checked"); html += F("> "); html += ar ? F("إرسال إشعار تيليجرام للمستخدمين المحددين") : F("Send Telegram notification to selected users"); html += F("</label>");
  html += F("<div class='row'><span class='k'>"); html += ar ? F("الحالة") : F("Status"); html += F("</span><span class='v'>"); html += adhanOutputState ? F("ON") : F("OFF"); html += F(" / GPIO"); html += String(adhanOutputPin); html += F("</span></div>");
  html += F("<button class='btn' type='submit'>"); html += ar ? F("حفظ إعدادات خرج الأذان") : F("Save Adhan Output Settings"); html += F("</button>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/testadhan"); html += F("'>"); html += ar ? F("اختبار 3 ثواني") : F("Test 3 seconds"); html += F("</a></form></div>");
  return html;
}

void handleTestAdhanOutput() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  uint16_t oldDur = adhanOutputDurationSec;
  adhanOutputDurationSec = 3;
  startAdhanOutputPulse(String("اختبار خرج الأذان"), false);
  adhanOutputDurationSec = oldDur;
  webServer.sendHeader("Location", sessionUrl("/uisettings"), true);
  webServer.send(302, "text/plain", "");
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



byte smartInputDefaultPin(byte idx) {
  const byte pins[SMART_INPUT_COUNT] = {32, 33, 16, 17, 34, 35};
  return idx < SMART_INPUT_COUNT ? pins[idx] : 32;
}

bool smartInputPinAnalogCapable(byte pin) {
  return (pin == 32 || pin == 33 || pin == 34 || pin == 35 || pin == 36 || pin == 39);
}

bool smartInputOutputValid(byte out) {
  return (out == 1 || out == 2 || out == 5 || out == 6 || out == 7 || out == 8);
}

bool smartInputPinReserved(byte pin) {
  if (pin == OUTPUT1_PIN || pin == OUTPUT2_PIN || pin == OUTPUT5_PIN || pin == OUTPUT6_PIN || pin == OUTPUT7_PIN || pin == OUTPUT8_PIN) return true;
  if (pin == STATUS_LED_PIN || pin == INTERNET_LED_PIN) return true;
#if ENABLE_DS3231_RTC
  if (pin == RTC_SDA_PIN || pin == RTC_SCL_PIN) return true;
#endif
  if (pin == DHTPIN) return true;
  return false;
}

byte smartInputSafePin(byte requested, byte idx, byte mode) {
  if (requested > 39) return smartInputDefaultPin(idx);
  if (smartInputPinReserved(requested)) return smartInputDefaultPin(idx);
  if (mode == SMART_INPUT_MODE_ANALOG && !smartInputPinAnalogCapable(requested)) return smartInputDefaultPin(idx);
  return requested;
}

void smartInputResetRuntime(byte i) {
  if (i >= SMART_INPUT_COUNT) return;
  smartInputRaw[i] = 0;
  smartInputValue[i] = 0;
  smartInputFilteredValue[i] = 0;
  smartInputFilterReady[i] = 0;
  smartInputActive[i] = 0;
  smartInputLastActive[i] = 255;
  smartInputStableActive[i] = 0;
  smartInputRawActive[i] = 0;
  smartInputLastRawActive[i] = 255;
  smartInputRawChangeMs[i] = 0;
  smartInputActiveStartMs[i] = 0;
  smartInputLastOffMs[i] = 0;
  smartInputPulseOffMs[i] = 0;
  smartInputPulseOutput[i] = 0;
  if (i < SMART_INPUT_COUNT) {
    strncpy(smartInputLastCardAction[i], "none", sizeof(smartInputLastCardAction[i]) - 1);
    smartInputLastCardAction[i][sizeof(smartInputLastCardAction[i]) - 1] = 0;
  }
}

void smartInputSetDefaultCard(byte i) {
  if (i >= SMART_INPUT_COUNT) return;
  memset(&smartInputCfg[i], 0, sizeof(SmartInputCardConfig));
  smartInputCfg[i].version = EEPROM_SMART_INPUT_VERSION;
  smartInputCfg[i].debounceMs = SMART_INPUT_DEFAULT_DEBOUNCE_MS;
  smartInputCfg[i].delaySec = 0;
  smartInputCfg[i].minOffSec = 0;
  smartInputCfg[i].actionDurationSec = 5;
  smartInputCfg[i].notifyCooldownSec = SMART_INPUT_DEFAULT_COOLDOWN_SEC;
  smartInputCfg[i].factor = 1.0f;
  smartInputCfg[i].threshold = 1000.0f;
  smartInputCfg[i].offset = 0.0f;
  smartInputCfg[i].hysteresis = SMART_INPUT_DEFAULT_HYSTERESIS;
  smartInputCfg[i].enabled = (i < 4) ? 1 : 0;
  smartInputCfg[i].mode = SMART_INPUT_MODE_DIGITAL;
  smartInputCfg[i].pin = smartInputDefaultPin(i);
  smartInputCfg[i].activeHigh = 0;
  smartInputCfg[i].telegramNotify = 0;
  smartInputCfg[i].outputAction = 0;
  smartInputCfg[i].actionOutput = 1;
  smartInputCfg[i].offOnClear = 0;
  smartInputCfg[i].triggerMode = SMART_INPUT_TRIGGER_CHANGE;
  smartInputCfg[i].actionMode = SMART_INPUT_ACTION_ON;
  smartInputCfg[i].notifyOnActive = 1;
  smartInputCfg[i].notifyOnClear = 1;
  smartInputCfg[i].smoothing = 1;
  smartInputCfg[i].sensorType = SMART_INPUT_SENSOR_DHT22;
  smartInputCfg[i].sensorMetric = SMART_INPUT_SENSOR_METRIC_TEMP;
  smartInputCfg[i].sensorTempThreshold = 35.0f;
  smartInputCfg[i].sensorHumThreshold = 70.0f;
  String n = String("Input Card ") + String(i + 1);
  n.toCharArray(smartInputCfg[i].name, sizeof(smartInputCfg[i].name));
  String("value").toCharArray(smartInputCfg[i].unit, sizeof(smartInputCfg[i].unit));
  smartInputCfg[i].customMsg[0] = 0;
  smartInputResetRuntime(i);
}

void setSmartInputDefaults() {
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) smartInputSetDefaultCard(i);
}

void sanitizeSmartInputCard(byte i) {
  if (i >= SMART_INPUT_COUNT) return;
  smartInputCfg[i].version = EEPROM_SMART_INPUT_VERSION;
  smartInputCfg[i].enabled = smartInputCfg[i].enabled ? 1 : 0;
  if (smartInputCfg[i].mode != SMART_INPUT_MODE_ANALOG && smartInputCfg[i].mode != SMART_INPUT_MODE_SENSOR) smartInputCfg[i].mode = SMART_INPUT_MODE_DIGITAL;
  smartInputCfg[i].pin = smartInputSafePin(smartInputCfg[i].pin, i, smartInputCfg[i].mode);
  smartInputCfg[i].activeHigh = smartInputCfg[i].activeHigh ? 1 : 0;
  smartInputCfg[i].telegramNotify = smartInputCfg[i].telegramNotify ? 1 : 0;
  smartInputCfg[i].outputAction = smartInputCfg[i].outputAction ? 1 : 0;
  if (!smartInputOutputValid(smartInputCfg[i].actionOutput)) smartInputCfg[i].actionOutput = 1;
  smartInputCfg[i].offOnClear = smartInputCfg[i].offOnClear ? 1 : 0;
  if (smartInputCfg[i].triggerMode > SMART_INPUT_TRIGGER_WHILE_ON) smartInputCfg[i].triggerMode = SMART_INPUT_TRIGGER_CHANGE;
  if (smartInputCfg[i].actionMode > SMART_INPUT_ACTION_FOLLOW) smartInputCfg[i].actionMode = SMART_INPUT_ACTION_ON;
  smartInputCfg[i].notifyOnActive = smartInputCfg[i].notifyOnActive ? 1 : 0;
  smartInputCfg[i].notifyOnClear = smartInputCfg[i].notifyOnClear ? 1 : 0;
  if (smartInputCfg[i].debounceMs < 20 || smartInputCfg[i].debounceMs > 5000) smartInputCfg[i].debounceMs = SMART_INPUT_DEFAULT_DEBOUNCE_MS;
  if (smartInputCfg[i].delaySec > 3600) smartInputCfg[i].delaySec = 0;
  if (smartInputCfg[i].minOffSec > 3600) smartInputCfg[i].minOffSec = 0;
  if (smartInputCfg[i].actionDurationSec < 1 || smartInputCfg[i].actionDurationSec > 3600) smartInputCfg[i].actionDurationSec = 5;
  if (smartInputCfg[i].notifyCooldownSec < 1 || smartInputCfg[i].notifyCooldownSec > 3600) smartInputCfg[i].notifyCooldownSec = SMART_INPUT_DEFAULT_COOLDOWN_SEC;
  if (smartInputCfg[i].smoothing < 1 || smartInputCfg[i].smoothing > 20) smartInputCfg[i].smoothing = 1;
  if (isnan(smartInputCfg[i].factor) || smartInputCfg[i].factor < -100000.0f || smartInputCfg[i].factor > 100000.0f) smartInputCfg[i].factor = 1.0f;
  if (isnan(smartInputCfg[i].offset) || smartInputCfg[i].offset < -1000000.0f || smartInputCfg[i].offset > 1000000.0f) smartInputCfg[i].offset = 0.0f;
  if (isnan(smartInputCfg[i].threshold) || smartInputCfg[i].threshold < -1000000.0f || smartInputCfg[i].threshold > 1000000.0f) smartInputCfg[i].threshold = 1000.0f;
  if (isnan(smartInputCfg[i].hysteresis) || smartInputCfg[i].hysteresis < 0.0f || smartInputCfg[i].hysteresis > 1000000.0f) smartInputCfg[i].hysteresis = SMART_INPUT_DEFAULT_HYSTERESIS;
  if (smartInputCfg[i].sensorType > SMART_INPUT_SENSOR_DS18B20) smartInputCfg[i].sensorType = SMART_INPUT_SENSOR_DHT22;
  if (smartInputCfg[i].sensorMetric > SMART_INPUT_SENSOR_METRIC_HUM) smartInputCfg[i].sensorMetric = SMART_INPUT_SENSOR_METRIC_TEMP;
  if (smartInputCfg[i].sensorType == SMART_INPUT_SENSOR_DS18B20) smartInputCfg[i].sensorMetric = SMART_INPUT_SENSOR_METRIC_TEMP;
  if (isnan(smartInputCfg[i].sensorTempThreshold) || smartInputCfg[i].sensorTempThreshold < -80.0f || smartInputCfg[i].sensorTempThreshold > 150.0f) smartInputCfg[i].sensorTempThreshold = 35.0f;
  if (isnan(smartInputCfg[i].sensorHumThreshold) || smartInputCfg[i].sensorHumThreshold < 0.0f || smartInputCfg[i].sensorHumThreshold > 100.0f) smartInputCfg[i].sensorHumThreshold = 70.0f;
  smartInputCfg[i].name[SMART_INPUT_NAME_MAX_LEN] = 0;
  smartInputCfg[i].unit[SMART_INPUT_UNIT_MAX_LEN] = 0;
  smartInputCfg[i].customMsg[SMART_INPUT_MSG_MAX_LEN] = 0;
  if (String(smartInputCfg[i].name).length() == 0) {
    String n = String("Input Card ") + String(i + 1);
    n.toCharArray(smartInputCfg[i].name, sizeof(smartInputCfg[i].name));
  }
  if (String(smartInputCfg[i].unit).length() == 0) {
    if (smartInputCfg[i].mode == SMART_INPUT_MODE_SENSOR) String("C").toCharArray(smartInputCfg[i].unit, sizeof(smartInputCfg[i].unit));
    else String("value").toCharArray(smartInputCfg[i].unit, sizeof(smartInputCfg[i].unit));
  }
}

bool smartInputCfgLooksOk() {
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) {
    if (smartInputCfg[i].version != EEPROM_SMART_INPUT_VERSION) return false;
    sanitizeSmartInputCard(i);
  }
  return true;
}

bool migrateSmartInputCardsV1() {
  SmartInputCardConfigV1 oldCfg[SMART_INPUT_COUNT];
  EEPROM.get(EEPROM_SMART_INPUT_OLD_DATA_ADDR, oldCfg);
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) {
    if (oldCfg[i].version != 1) return false;
    smartInputSetDefaultCard(i);
    smartInputCfg[i].enabled = oldCfg[i].enabled;
    smartInputCfg[i].mode = oldCfg[i].mode;
    smartInputCfg[i].pin = oldCfg[i].pin;
    smartInputCfg[i].activeHigh = oldCfg[i].activeHigh;
    smartInputCfg[i].telegramNotify = oldCfg[i].telegramNotify;
    smartInputCfg[i].outputAction = oldCfg[i].outputAction;
    smartInputCfg[i].actionOutput = oldCfg[i].actionOutput;
    smartInputCfg[i].offOnClear = oldCfg[i].offOnClear;
    strncpy(smartInputCfg[i].name, oldCfg[i].name, SMART_INPUT_NAME_MAX_LEN);
    smartInputCfg[i].factor = oldCfg[i].factor;
    smartInputCfg[i].threshold = oldCfg[i].threshold;
    smartInputCfg[i].actionMode = oldCfg[i].offOnClear ? SMART_INPUT_ACTION_FOLLOW : SMART_INPUT_ACTION_ON;
    sanitizeSmartInputCard(i);
  }
  return true;
}

bool migrateSmartInputCardsV2() {
  SmartInputCardConfigV2 oldCfg[SMART_INPUT_COUNT];
  EEPROM.get(EEPROM_SMART_INPUT_OLD_DATA_ADDR, oldCfg);
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) {
    if (oldCfg[i].version != 2) return false;
    smartInputSetDefaultCard(i);
    smartInputCfg[i].debounceMs = oldCfg[i].debounceMs;
    smartInputCfg[i].delaySec = oldCfg[i].delaySec;
    smartInputCfg[i].minOffSec = oldCfg[i].minOffSec;
    smartInputCfg[i].actionDurationSec = oldCfg[i].actionDurationSec;
    smartInputCfg[i].notifyCooldownSec = oldCfg[i].notifyCooldownSec;
    smartInputCfg[i].factor = oldCfg[i].factor;
    smartInputCfg[i].threshold = oldCfg[i].threshold;
    smartInputCfg[i].offset = oldCfg[i].offset;
    smartInputCfg[i].hysteresis = oldCfg[i].hysteresis;
    smartInputCfg[i].enabled = oldCfg[i].enabled;
    smartInputCfg[i].mode = oldCfg[i].mode;
    smartInputCfg[i].pin = oldCfg[i].pin;
    smartInputCfg[i].activeHigh = oldCfg[i].activeHigh;
    smartInputCfg[i].telegramNotify = oldCfg[i].telegramNotify;
    smartInputCfg[i].outputAction = oldCfg[i].outputAction;
    smartInputCfg[i].actionOutput = oldCfg[i].actionOutput;
    smartInputCfg[i].offOnClear = oldCfg[i].offOnClear;
    smartInputCfg[i].triggerMode = oldCfg[i].triggerMode;
    smartInputCfg[i].actionMode = oldCfg[i].actionMode;
    smartInputCfg[i].notifyOnActive = oldCfg[i].notifyOnActive;
    smartInputCfg[i].notifyOnClear = oldCfg[i].notifyOnClear;
    smartInputCfg[i].smoothing = oldCfg[i].smoothing;
    strncpy(smartInputCfg[i].name, oldCfg[i].name, SMART_INPUT_NAME_MAX_LEN);
    strncpy(smartInputCfg[i].unit, oldCfg[i].unit, SMART_INPUT_UNIT_MAX_LEN);
    strncpy(smartInputCfg[i].customMsg, oldCfg[i].customMsg, SMART_INPUT_MSG_MAX_LEN);
    smartInputCfg[i].sensorType = SMART_INPUT_SENSOR_DHT22;
    smartInputCfg[i].sensorMetric = SMART_INPUT_SENSOR_METRIC_TEMP;
    smartInputCfg[i].sensorTempThreshold = 35.0f;
    smartInputCfg[i].sensorHumThreshold = 70.0f;
    sanitizeSmartInputCard(i);
  }
  return true;
}

void loadSmartInputCardsConfig() {
  smartInputConfigLoaded = false;

  // New safe map (V1.7.8.32): magic at 3399, data at 3400..4095.
  if (EEPROM.read(EEPROM_SMART_INPUT_MAGIC_ADDR) == EEPROM_SMART_INPUT_MAGIC) {
    uint16_t v = 0;
    EEPROM.get(EEPROM_SMART_INPUT_DATA_ADDR, v);
    if (v == EEPROM_SMART_INPUT_VERSION) {
      EEPROM.get(EEPROM_SMART_INPUT_DATA_ADDR, smartInputCfg);
      if (smartInputCfgLooksOk()) smartInputConfigLoaded = true;
    }
  }

  // Old V1/V2 map migration: magic at 3400, data at 3404.
  // V3 at the old address was too large for the 4096-byte EEPROM map, so it is not trusted.
  if (!smartInputConfigLoaded && EEPROM.read(EEPROM_SMART_INPUT_OLD_MAGIC_ADDR) == EEPROM_SMART_INPUT_MAGIC) {
    uint16_t oldV = 0;
    EEPROM.get(EEPROM_SMART_INPUT_OLD_DATA_ADDR, oldV);
    if (oldV == 2 && migrateSmartInputCardsV2()) {
      smartInputConfigLoaded = true;
      saveSmartInputCardsConfig();
    } else if (oldV == 1 && migrateSmartInputCardsV1()) {
      smartInputConfigLoaded = true;
      saveSmartInputCardsConfig();
    }
  }

  if (!smartInputConfigLoaded) {
    setSmartInputDefaults();
    saveSmartInputCardsConfig();
  }
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) smartInputResetRuntime(i);
}

void saveSmartInputCardsConfig() {
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) sanitizeSmartInputCard(i);
  EEPROM.write(EEPROM_SMART_INPUT_MAGIC_ADDR, EEPROM_SMART_INPUT_MAGIC);
  EEPROM.put(EEPROM_SMART_INPUT_DATA_ADDR, smartInputCfg);
  smartInputLastSaveOk = commitEeprom("smart-input-cards-save");
  strncpy(smartInputLastAction, smartInputLastSaveOk ? "saved" : "save failed", sizeof(smartInputLastAction) - 1);
  smartInputLastAction[sizeof(smartInputLastAction) - 1] = 0;
}

void applySmartInputPinModes() {
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) {
    byte p = smartInputCfg[i].pin;
    if (p > 39) continue;
    if (smartInputCfg[i].mode == SMART_INPUT_MODE_DIGITAL) {
      if (p == 34 || p == 35 || p == 36 || p == 39) pinMode(p, INPUT);
      else pinMode(p, smartInputCfg[i].activeHigh ? INPUT : INPUT_PULLUP);
    } else {
      pinMode(p, INPUT);
    }
    yield();
  }
}

String smartInputName(byte idx) {
  if (idx >= SMART_INPUT_COUNT) return String("Input");
  String n = String(smartInputCfg[idx].name);
  n.trim();
  if (!n.length()) n = String("Input Card ") + String(idx + 1);
  return n;
}

String smartInputUnit(byte idx) {
  if (idx >= SMART_INPUT_COUNT) return String("");
  String u = String(smartInputCfg[idx].unit);
  u.trim();
  return u;
}

bool smartInputShouldNotify(byte idx, byte active) {
  if (idx >= SMART_INPUT_COUNT) return false;
  if (!smartInputCfg[idx].telegramNotify) return false;
  if (active && !smartInputCfg[idx].notifyOnActive) return false;
  if (!active && !smartInputCfg[idx].notifyOnClear) return false;
  unsigned long gap = (unsigned long)smartInputCfg[idx].notifyCooldownSec * 1000UL;
  if (gap < 1000UL) gap = 1000UL;
  return smartInputLastNotifyMs[idx] == 0 || (unsigned long)(millis() - smartInputLastNotifyMs[idx]) >= gap;
}

void notifySmartInputUsers(byte idx, byte active, float value, int raw) {
  if (idx >= SMART_INPUT_COUNT) return;
  String msg;
  msg.reserve(220);
  String custom = String(smartInputCfg[idx].customMsg);
  custom.trim();
  if (custom.length()) { msg += custom; msg += "\n"; }
  msg += "Smart Input Changed";
  msg += "\nInput: "; msg += smartInputName(idx);
  msg += "\nMode: "; msg += smartInputCfg[idx].mode == SMART_INPUT_MODE_SENSOR ? "Sensor" : (smartInputCfg[idx].mode == SMART_INPUT_MODE_ANALOG ? "Analog" : "Digital");
  msg += "\nState: "; msg += active ? "ACTIVE" : "OFF";
  msg += "\nValue: "; msg += String(value, smartInputCfg[idx].mode == SMART_INPUT_MODE_ANALOG ? 2 : 0);
  String u = smartInputUnit(idx); if (u.length()) { msg += " "; msg += u; }
  msg += "\nRaw: "; msg += String(raw);
  msg += "\nTime: "; msg += currentDeviceDateTimeText();

  int sent = 0;
  for (byte i = 0; i < WEB_USER_COUNT; i++) {
    if (!WEB_USERS[i].enabled || !WEB_USERS[i].user[0]) continue;
    if (!webUserTelegramNotify[i] || !webUserTelegramChatId[i][0]) continue;
    if (!canUserReceiveSmartInputNotify(i, idx)) continue;
    bool dup = false;
    for (byte j = 0; j < i; j++) {
      if (webUserTelegramNotify[j] && webUserTelegramChatId[j][0] && strcmp(webUserTelegramChatId[j], webUserTelegramChatId[i]) == 0) { dup = true; break; }
    }
    if (dup) continue;
    telegramSendToChat(String(webUserTelegramChatId[i]), msg);
    sent++;
    yield();
  }
  if (sent == 0) telegramSend(msg);
}

bool smartInputTriggerMatches(byte triggerMode, byte oldActive, byte newActive) {
  if (triggerMode == SMART_INPUT_TRIGGER_ON) return oldActive == 0 && newActive == 1;
  if (triggerMode == SMART_INPUT_TRIGGER_OFF) return oldActive == 1 && newActive == 0;
  if (triggerMode == SMART_INPUT_TRIGGER_WHILE_ON) return newActive == 1;
  return oldActive != newActive;
}

void setSmartInputLastExecution(byte idx, const String& text) {
  if (idx >= SMART_INPUT_COUNT) return;
  String t = text;
  t.trim();
  if (!t.length()) t = "none";
  t.substring(0, sizeof(smartInputLastCardAction[idx]) - 1).toCharArray(smartInputLastCardAction[idx], sizeof(smartInputLastCardAction[idx]));
  smartInputLastCardAction[idx][sizeof(smartInputLastCardAction[idx]) - 1] = 0;
}

void smartInputExecuteOutput(byte idx, byte out, byte mode, byte active) {
  if (idx >= SMART_INPUT_COUNT || !smartInputOutputValid(out)) return;
  if (mode == SMART_INPUT_ACTION_NONE) return;
  String actionMsg = String("Output ") + String(out) + " ";
  if (mode == SMART_INPUT_ACTION_ON) {
    setOutputFromAutomation(out, 1, "Smart Input ON");
    actionMsg += "ON";
  } else if (mode == SMART_INPUT_ACTION_OFF) {
    setOutputFromAutomation(out, 0, "Smart Input OFF");
    actionMsg += "OFF";
  } else if (mode == SMART_INPUT_ACTION_TOGGLE) {
    byte nextState = telegramOutputState(out) ? 0 : 1;
    setOutputFromAutomation(out, nextState, "Smart Input Toggle");
    actionMsg += String("TOGGLE -> ") + (nextState ? "ON" : "OFF");
  } else if (mode == SMART_INPUT_ACTION_PULSE) {
    setOutputFromAutomation(out, 1, "Smart Input Pulse");
    smartInputPulseOutput[idx] = out;
    smartInputPulseOffMs[idx] = millis() + ((unsigned long)smartInputCfg[idx].actionDurationSec * 1000UL);
    actionMsg += String("PULSE ") + String(smartInputCfg[idx].actionDurationSec) + " sec";
  } else if (mode == SMART_INPUT_ACTION_FOLLOW) {
    setOutputFromAutomation(out, active ? 1 : 0, active ? "Smart Input Follow ON" : "Smart Input Follow OFF");
    actionMsg += String("FOLLOW -> ") + (active ? "ON" : "OFF");
  }
  actionMsg += " / ";
  actionMsg += currentDeviceDateTimeText();
  setSmartInputLastExecution(idx, actionMsg);
}
void smartInputApplyOutputAction(byte idx, byte oldActive, byte newActive) {
  if (idx >= SMART_INPUT_COUNT) return;
  if (!smartInputCfg[idx].outputAction) return;
  byte out = smartInputCfg[idx].actionOutput;
  if (!smartInputOutputValid(out)) return;
  byte mode = smartInputCfg[idx].actionMode;
  if (mode == SMART_INPUT_ACTION_FOLLOW) {
    if (oldActive != newActive) smartInputExecuteOutput(idx, out, mode, newActive);
    return;
  }
  if (smartInputTriggerMatches(smartInputCfg[idx].triggerMode, oldActive, newActive)) smartInputExecuteOutput(idx, out, mode, newActive);
  if (!newActive && smartInputCfg[idx].offOnClear && mode != SMART_INPUT_ACTION_OFF) {
    setOutputFromAutomation(out, 0, "Smart Input Clear");
    String msg = String("Output ") + String(out) + " CLEAR -> OFF / " + currentDeviceDateTimeText();
    setSmartInputLastExecution(idx, msg);
  }
}

void maintainSmartInputPulses() {
  unsigned long now = millis();
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) {
    if (smartInputPulseOffMs[i] && (long)(now - smartInputPulseOffMs[i]) >= 0) {
      byte out = smartInputPulseOutput[i];
      smartInputPulseOffMs[i] = 0;
      smartInputPulseOutput[i] = 0;
      if (smartInputOutputValid(out)) {
        setOutputFromAutomation(out, 0, "Smart Input Pulse End");
        String msg = String("Output ") + String(out) + " PULSE END -> OFF / " + currentDeviceDateTimeText();
        setSmartInputLastExecution(i, msg);
      }
    }
  }
}

byte smartInputApplyDigitalDebounce(byte idx, byte rawActive, unsigned long now) {
  if (idx >= SMART_INPUT_COUNT) return rawActive;
  if (smartInputLastRawActive[idx] == 255) {
    smartInputLastRawActive[idx] = rawActive;
    smartInputStableActive[idx] = rawActive;
    smartInputRawChangeMs[idx] = now;
    return rawActive;
  }
  if (rawActive != smartInputLastRawActive[idx]) {
    smartInputLastRawActive[idx] = rawActive;
    smartInputRawChangeMs[idx] = now;
  }
  if (rawActive != smartInputStableActive[idx] && (unsigned long)(now - smartInputRawChangeMs[idx]) >= smartInputCfg[idx].debounceMs) {
    smartInputStableActive[idx] = rawActive;
  }
  return smartInputStableActive[idx];
}

byte smartInputApplyDelayAndMinOff(byte idx, byte logicActive, unsigned long now) {
  if (idx >= SMART_INPUT_COUNT) return logicActive;
  if (!logicActive) {
    smartInputActiveStartMs[idx] = 0;
    if (smartInputActive[idx]) smartInputLastOffMs[idx] = now;
    return 0;
  }
  if (smartInputCfg[idx].minOffSec > 0 && smartInputLastOffMs[idx] != 0) {
    if ((unsigned long)(now - smartInputLastOffMs[idx]) < (unsigned long)smartInputCfg[idx].minOffSec * 1000UL) return 0;
  }
  if (smartInputActiveStartMs[idx] == 0) smartInputActiveStartMs[idx] = now;
  if (smartInputCfg[idx].delaySec > 0 && (unsigned long)(now - smartInputActiveStartMs[idx]) < (unsigned long)smartInputCfg[idx].delaySec * 1000UL) return 0;
  return 1;
}

void maintainSmartInputCards() {
  maintainSmartInputPulses();
  unsigned long now = millis();
  if (smartInputLastReadMs != 0 && (unsigned long)(now - smartInputLastReadMs) < SMART_INPUT_READ_INTERVAL_MS) return;
  smartInputLastReadMs = now;
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) {
    if (!smartInputCfg[i].enabled) continue;
    byte p = smartInputCfg[i].pin;
    if (p > 39) continue;
    int raw = 0;
    float val = 0;
    byte active = 0;
    if (smartInputCfg[i].mode == SMART_INPUT_MODE_SENSOR) {
      smartInputRaw[i] = 0;
      smartInputValue[i] = 0;
      if (smartInputLastActive[i] == 255) smartInputLastActive[i] = 0;
      yield();
      continue;
    }
    if (smartInputCfg[i].mode == SMART_INPUT_MODE_ANALOG) {
      if (smartInputPinAnalogCapable(p)) raw = analogRead(p);
      else raw = 0;
      val = ((float)raw * smartInputCfg[i].factor) + smartInputCfg[i].offset;
      byte smooth = smartInputCfg[i].smoothing;
      if (smooth < 1) smooth = 1;
      if (!smartInputFilterReady[i] || smooth <= 1) {
        smartInputFilteredValue[i] = val;
        smartInputFilterReady[i] = 1;
      } else {
        smartInputFilteredValue[i] = ((smartInputFilteredValue[i] * (float)(smooth - 1)) + val) / (float)smooth;
      }
      val = smartInputFilteredValue[i];
      if (smartInputActive[i]) active = (val >= (smartInputCfg[i].threshold - smartInputCfg[i].hysteresis)) ? 1 : 0;
      else active = (val >= smartInputCfg[i].threshold) ? 1 : 0;
    } else {
      raw = (digitalRead(p) == HIGH) ? 1 : 0;
      byte rawActive = smartInputCfg[i].activeHigh ? (raw ? 1 : 0) : (raw ? 0 : 1);
      byte debounced = smartInputApplyDigitalDebounce(i, rawActive, now);
      active = smartInputApplyDelayAndMinOff(i, debounced, now);
      val = (float)active;
    }
    smartInputRaw[i] = raw;
    smartInputValue[i] = val;
    byte oldActive = (smartInputLastActive[i] == 255) ? active : smartInputLastActive[i];
    smartInputActive[i] = active;
    if (smartInputLastActive[i] == 255) {
      smartInputLastActive[i] = active;
    } else {
      bool stateChanged = (smartInputLastActive[i] != active);
      bool periodicWhileOn = (smartInputCfg[i].triggerMode == SMART_INPUT_TRIGGER_WHILE_ON && active == 1);
      if (stateChanged || periodicWhileOn) {
        if (stateChanged) {
          smartInputLastActive[i] = active;
          addEsp32Log(String("Smart input ") + smartInputName(i) + (active ? " ACTIVE" : " OFF"));
        }
        if ((stateChanged || periodicWhileOn) && smartInputShouldNotify(i, active)) {
          smartInputLastNotifyMs[i] = now;
          notifySmartInputUsers(i, active, val, raw);
        }
        smartInputApplyOutputAction(i, oldActive, active);
        wsBroadcastStatus();
      }
    }
    yield();
  }
}

String smartInputStorageStatusText() {
  String s;
  s.reserve(160);
  s += F("Loaded="); s += smartInputConfigLoaded ? F("YES") : F("DEFAULTS");
  s += F(" / LastSave="); s += smartInputLastSaveOk ? F("OK") : F("FAILED/none");
  s += F(" / ConfigStore=EEPROM");
  s += F(" / EEPROM+FS="); s += lastStorageSaveOk ? F("SAVED") : F("--");
  s += F(" / Last="); s += smartInputLastAction;
  return s;
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
  if (outputNumber == 7) return currentOutput7State;
  if (outputNumber == 8) return currentOutput8State;
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

String openMeteoUrlForPrayerArea() {
  String url = F("https://api.open-meteo.com/v1/forecast?latitude=");
  url += prayerGovernorateLat();
  url += F("&longitude=");
  url += prayerGovernorateLon();
  url += F("&current=temperature_2m,relative_humidity_2m&timezone=Africa%2FCairo");
  return url;
}

String openMeteoUrlForCity(byte idx) {
  // V1.7.8.24: kept for compatibility; weather now follows the unified Prayer/Adhan area.
  return openMeteoUrlForPrayerArea();
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
  http.setTimeout(5000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  String url = openMeteoUrlForPrayerArea();
  if (!http.begin(wc, url)) {
    cityWeatherOk = false;
    lastCityWeatherMessage = "Internet weather begin failed";
    lastCityWeatherCheckMs = now;
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    cityWeatherOk = false;
    lastCityWeatherMessage = String("Internet weather HTTP ") + code;
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
    lastCityWeatherMessage = "Internet weather parse failed";
    lastCityWeatherCheckMs = now;
    return false;
  }

  cityWeatherTemp = t;
  cityWeatherHum = h;
  cityWeatherOk = true;
  lastCityWeatherMessage = "Internet / OK";
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
  bool ar = (currentWebLanguage() == TG_LANG_AR);
  String html;
  html.reserve(1500);
  html += F("<div class='card'><h1>🌦️ "); html += ar ? F("طقس الإنترنت") : F("Internet Weather"); html += F("</h1><div class='sub'>");
  html += ar ? F("المنطقة يتم اختيارها من صفحة UI Branding Settings، وهي نفس منطقة مواقيت الصلاة والأذان.") : F("Area is selected from UI Branding Settings and shared with Prayer Times and Adhan.");
  html += F("</div>");
  html += F("<div class='row'><span class='k'>"); html += ar ? F("المدينة") : F("Area"); html += F("</span><span class='v' id='owcity'>");
  html += htmlEscapeText(prayerWeatherAreaName(ar));
  html += F("</span></div>");
  html += F("<div class='row'><span class='k'>"); html += ar ? F("حرارة الإنترنت") : F("Internet Temp"); html += F("</span><span class='v'><span id='owtemp'>");
  html += isnan(cityWeatherTemp) ? String("--") : String(cityWeatherTemp, 1);
  html += F("</span> &deg;C</span></div>");
  html += F("<div class='row'><span class='k'>"); html += ar ? F("رطوبة الإنترنت") : F("Internet Humidity"); html += F("</span><span class='v'><span id='owhum'>");
  html += isnan(cityWeatherHum) ? String("--") : String(cityWeatherHum, 0);
  html += F("</span> %</span></div>");
  html += F("<div class='row'><span class='k'>"); html += ar ? F("حالة الطقس") : F("Weather Status"); html += F("</span><span class='v' id='owstatus'>");
  html += htmlEscapeText(lastCityWeatherMessage);
  html += F("</span></div>");
  html += F("<div class='row'><span class='k'>"); html += ar ? F("آخر تحديث") : F("Last Update"); html += F("</span><span class='v' id='owtime'>");
  html += htmlEscapeText(lastCityWeatherTime);
  html += F("</span></div>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/weatherrefresh"); html += F("'>"); html += ar ? F("تحديث الطقس الآن") : F("Refresh Weather"); html += F("</a>");
  html += F("</div>");
  return html;
}

void handleSaveWeatherCity() {
  // V1.7.8.24: separate weather-city selection is disabled.
  // Weather follows the unified area saved from UI Branding Settings.
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  cityWeatherOk = false;
  lastCityWeatherCheckMs = 0;
  lastCityWeatherMessage = "Use UI Branding Settings area";
  lastCityWeatherTime = "--";
  webServer.sendHeader("Location", sessionUrl("/uisettings"));
  webServer.send(303, "text/plain", "Use unified area settings");
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

String format12HourClock(byte hour24, byte minute, byte second, bool ar, bool withSeconds = true) {
  bool pm = hour24 >= 12;
  byte h12 = hour24 % 12;
  if (h12 == 0) h12 = 12;
  char buf[20];
  if (withSeconds) snprintf(buf, sizeof(buf), "%02u:%02u:%02u", (unsigned)h12, (unsigned)minute, (unsigned)second);
  else snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)h12, (unsigned)minute);
  String x = String(buf);
  x += " ";
  x += ar ? (pm ? String("م") : String("ص")) : (pm ? String("PM") : String("AM"));
  return x;
}

String formatDateTime12FromTm(const struct tm& tmv, bool ar) {
  char d[16];
  snprintf(d, sizeof(d), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  String x = String(dayName((byte)tmv.tm_wday));
  x += " ";
  x += d;
  x += " ";
  x += format12HourClock((byte)tmv.tm_hour, (byte)tmv.tm_min, (byte)tmv.tm_sec, ar, true);
  return x;
}

String currentDeviceDateTimeTextForLang(byte lang) {
  bool ar = (lang == TG_LANG_AR);
  if (!isTimeValidNow()) return ar ? String("في انتظار ضبط الوقت") : String("Waiting for Time");
  time_t now = (time_t)getDeviceEpoch();
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  return formatDateTime12FromTm(tmNow, ar);
}

String currentDeviceDateTimeText() {
  return currentDeviceDateTimeTextForLang(currentWebLanguage());
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
  html += F("<div class='sub'>Range: 00:00:01 to 23:59:59. Timer is runtime-only: after device restart, output stays OFF and remaining time resets to 00:00:00. The timer duration is not saved to EEPROM.</div></div>");

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

  if (roleAtLeast(WEB_ROLE_ENGINEER)) {
    html += F("<div class='ocbox'><div class='octop'><div><b>"); html += outputDisplayLabelHtml(7); html += F("</b><div class='small'>Aux Output 7 / GPIO19</div></div><div><span id='o7' class='bigstate "); html += outputClass(currentOutput7State); html += F("'>"); html += outputStateText(currentOutput7State); html += F("</span><span id='d7' class='dot "); html += outputClass(currentOutput7State); html += F("'></span></div></div><button id='b7' class='btn' onclick='toggleOut(7)'>...</button></div>");
    html += F("<div class='ocbox'><div class='octop'><div><b>"); html += outputDisplayLabelHtml(8); html += F("</b><div class='small'>Aux Output 8 / GPIO23</div></div><div><span id='o8' class='bigstate "); html += outputClass(currentOutput8State); html += F("'>"); html += outputStateText(currentOutput8State); html += F("</span><span id='d8' class='dot "); html += outputClass(currentOutput8State); html += F("'></span></div></div><button id='b8' class='btn' onclick='toggleOut(8)'>...</button></div>");
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
  js += F("var outState={1:0,2:0,5:0,6:0,7:0,8:0};");
  js += F("var L_ON='"); js += ar ? F("يعمل") : F("ON"); js += F("',L_OFF='"); js += ar ? F("متوقف") : F("OFF"); js += F("',L_VIEW='"); js += ar ? F("عرض فقط") : F("VIEW ONLY"); js += F("',L_PENDING='"); js += ar ? F("قيد المزامنة") : F("Pending"); js += F("',L_SYNCED='"); js += ar ? F("تمت المزامنة") : F("Synced"); js += F("';");
  js += F("var L_TON='"); js += ar ? F("تشغيل ") : F("TURN ON "); js += F("',L_TOFF='"); js += ar ? F("إيقاف ") : F("TURN OFF "); js += F("',L_SEND='"); js += ar ? F("جاري الإرسال...") : F("Sending..."); js += F("',L_APPLIED='"); js += ar ? F("تم التنفيذ. حالة ThingSpeak قيد المزامنة/تمت.") : F("Applied. Full ThingSpeak state pending/synced."); js += F("',L_NOCHANGE='"); js += ar ? F("لا يوجد تغيير.") : F("No change."); js += F("',L_FAIL='"); js += ar ? F("فشل أمر المخرج/الجلسة") : F("Output command failed/session"); js += F("';");
  js += F("var outLabels={1:\""); js += jsonEscape(fieldLabel(1)); js += F("\",2:\""); js += jsonEscape(fieldLabel(2)); js += F("\",5:\""); js += jsonEscape(fieldLabel(5)); js += F("\",6:\""); js += jsonEscape(fieldLabel(6)); js += F("\",7:\""); js += jsonEscape(outputDisplayLabel(7)); js += F("\",8:\""); js += jsonEscape(outputDisplayLabel(8)); js += F("\"};");
  js += F("function outName(i){return outLabels[i]||('Output '+i)}function paintOne(i,v){outState[i]=v?1:0;let e=document.getElementById('o'+i),d=document.getElementById('d'+i),b=document.getElementById('b'+i);if(e){e.innerHTML=v?L_ON:L_OFF;e.className='bigstate '+(v?'on':'off')}if(d){d.className='dot '+(v?'on':'off')}if(b){b.innerHTML=v?(L_TOFF+outName(i)):(L_TON+outName(i));b.className='btn '+(v?'offbtn':'onbtn')}}");
  js += F("function paintOut(d){if(d.l1)outLabels[1]=d.l1;if(d.l2)outLabels[2]=d.l2;if(d.l5)outLabels[5]=d.l5;if(d.l6)outLabels[6]=d.l6;if(d.lo7)outLabels[7]=d.lo7;if(d.lo8)outLabels[8]=d.lo8;paintOne(1,d.o1);paintOne(2,d.o2);paintOne(5,d.o5);paintOne(6,d.o6);paintOne(7,d.o7);paintOne(8,d.o8);[1,2,5,6].forEach(function(x){let b=document.getElementById('b'+x);if(b&&d['c'+x]===false){b.disabled=true;b.innerHTML=L_VIEW;b.className='btn btn2';}});let i7=document.getElementById('i7');if(i7)i7.innerHTML=d.i7?L_ON:L_OFF;let i8=document.getElementById('i8');if(i8)i8.innerHTML=d.i8?L_ON:L_OFF;let i9=document.getElementById('i9');if(i9)i9.innerHTML=d.i9?L_ON:L_OFF;let i10=document.getElementById('i10');if(i10)i10.innerHTML=d.i10?L_ON:L_OFF;let p=document.getElementById('pend');if(p){p.innerHTML=d.pending?L_PENDING:L_SYNCED;p.className='pill '+(d.pending?'warn':'ok')}let t=document.getElementById('temp');if(t)t.innerHTML=(d.temp===null||d.temp===undefined)?'--':d.temp;let h=document.getElementById('hum');if(h)h.innerHTML=(d.hum===null||d.hum===undefined)?'--':d.hum;let dt=document.getElementById('dhttype');if(dt)dt.innerHTML=d.dht||'--';let ns=document.getElementById('ntpstate');if(ns)ns.innerHTML=d.clockState||'--';let rs=document.getElementById('rtcstate');if(rs)rs.innerHTML=d.rtcStatus||'--';let ds=document.getElementById('devtime');if(ds)ds.innerHTML=d.deviceTime||'--';let dd=document.getElementById('devday');if(dd)dd.innerHTML=d.deviceDay||'--';let tr=document.getElementById('tremain');if(tr)tr.innerHTML=d.timerRemain||'00:00:00';let oc=document.getElementById('owcity');if(oc)oc.innerHTML=d.city||'--';let ot=document.getElementById('owtemp');if(ot)ot.innerHTML=(d.owTemp===null||d.owTemp===undefined)?'--':d.owTemp;let oh=document.getElementById('owhum');if(oh)oh.innerHTML=(d.owHum===null||d.owHum===undefined)?'--':d.owHum;let os=document.getElementById('owstatus');if(os)os.innerHTML=d.owStatus||'--';let ou=document.getElementById('owtime');if(ou)ou.innerHTML=d.owTime||'--'}");
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
  json += F(",\"o7\":");
  json += String(currentOutput7State);
  json += F(",\"o8\":");
  json += String(currentOutput8State);
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
  json += F(",\"lo7\":\""); json += jsonEscape(outputDisplayLabel(7)); json += F("\"");
  json += F(",\"lo8\":\""); json += jsonEscape(outputDisplayLabel(8)); json += F("\"");
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
  json += jsonEscape(prayerWeatherAreaName(false));
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
  json += F(",\"o1Detail\":\""); if (show1) json += jsonEscape(homeOutputDetailText(1, currentWebLanguage() == TG_LANG_AR)); json += F("\"");
  json += F(",\"o2Detail\":\""); if (show2) json += jsonEscape(homeOutputDetailText(2, currentWebLanguage() == TG_LANG_AR)); json += F("\"");
  json += F(",\"o5Detail\":\""); if (show5) json += jsonEscape(homeOutputDetailText(5, currentWebLanguage() == TG_LANG_AR)); json += F("\"");
  json += F(",\"o6Detail\":\""); if (show6) json += jsonEscape(homeOutputDetailText(6, currentWebLanguage() == TG_LANG_AR)); json += F("\"");
  json += F(",\"o7Detail\":\""); json += jsonEscape(homeOutputDetailText(7, currentWebLanguage() == TG_LANG_AR)); json += F("\"");
  json += F(",\"o8Detail\":\""); json += jsonEscape(homeOutputDetailText(8, currentWebLanguage() == TG_LANG_AR)); json += F("\"");
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
  } else if (outputNumber == 7) {
    if (currentOutput7State != newState) {
      currentOutput7State = newState;
      setRelay(OUTPUT7_PIN, currentOutput7State);
      if (saveToEeprom) saveOutputState(7, currentOutput7State);
      changed = true;
    }
  } else if (outputNumber == 8) {
    if (currentOutput8State != newState) {
      currentOutput8State = newState;
      setRelay(OUTPUT8_PIN, currentOutput8State);
      if (saveToEeprom) saveOutputState(8, currentOutput8State);
      changed = true;
    }
  } else {
    return false;
  }

  if (saveToEeprom) recordOutputManualControl(outputNumber, newState);
  rememberOutputCommand(source, outputNumber, newState, changed);
  if (changed && syncThingSpeak && isThingSpeakCommandOutput(outputNumber)) {
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
  bool auxOut = (out == 7 || out == 8);
  if ((out != 1 && out != 2 && out != 5 && out != 6 && !auxOut) || !isValidState(state)) {
    webServer.send(400, "application/json", F("{\"error\":\"bad request\"}"));
    return;
  }
  if (auxOut) {
    if (!roleAtLeast(WEB_ROLE_ENGINEER)) {
      webServer.send(403, "application/json", F("{\"error\":\"aux output denied\"}"));
      return;
    }
  } else if (!canCurrentUserControlField(out)) {
    webServer.send(403, "application/json", F("{\"error\":\"field denied\"}"));
    return;
  }
  String src = String("Web(") + currentWebUserName() + ")";
  bool changed = setOutputFromWeb(out, state, src.c_str());
  webServer.send(200, "application/json", outputStatusJson(changed));
}




String smartInputModeText(byte m, bool ar) {
  if (m == SMART_INPUT_MODE_ANALOG) return ar ? String("أنالوج") : String("Analog");
  if (m == SMART_INPUT_MODE_SENSOR) return ar ? String("حساس") : String("Sensor");
  return ar ? String("ديجيتال") : String("Digital");
}

String smartInputSensorTypeText(byte t, bool ar) {
  if (t == SMART_INPUT_SENSOR_DHT11) return String("DHT11");
  if (t == SMART_INPUT_SENSOR_DS18B20) return String("DS18B20");
  return String("DHT22");
}

String smartInputSensorMetricText(byte t, bool ar) {
  if (t == SMART_INPUT_SENSOR_METRIC_HUM) return ar ? String("الرطوبة") : String("Humidity");
  return ar ? String("الحرارة") : String("Temperature");
}

String smartInputTriggerText(byte m, bool ar) {
  if (m == SMART_INPUT_TRIGGER_ON) return ar ? String("عند ON فقط") : String("ON only");
  if (m == SMART_INPUT_TRIGGER_OFF) return ar ? String("عند OFF فقط") : String("OFF only");
  if (m == SMART_INPUT_TRIGGER_WHILE_ON) return ar ? String("طالما ON") : String("While ON");
  return ar ? String("عند أي تغيير") : String("Any change");
}

String smartInputActionText(byte m, bool ar) {
  if (m == SMART_INPUT_ACTION_ON) return ar ? String("شغل مخرج") : String("Output ON");
  if (m == SMART_INPUT_ACTION_OFF) return ar ? String("اقفل مخرج") : String("Output OFF");
  if (m == SMART_INPUT_ACTION_TOGGLE) return ar ? String("تبديل حالة المخرج") : String("Toggle output");
  if (m == SMART_INPUT_ACTION_PULSE) return ar ? String("تشغيل لمدة") : String("Pulse output");
  if (m == SMART_INPUT_ACTION_FOLLOW) return ar ? String("تابع حالة الدخل") : String("Follow input");
  return ar ? String("لا تفعل شيء") : String("No action");
}

String smartInputStateText(byte idx, bool ar) {
  if (idx >= SMART_INPUT_COUNT) return String("--");
  if (!smartInputCfg[idx].enabled) return ar ? String("معطل") : String("Disabled");
  if (smartInputCfg[idx].mode == SMART_INPUT_MODE_SENSOR) {
    String t = smartInputSensorTypeText(smartInputCfg[idx].sensorType, ar);
    t += ar ? String(" / وضع حساس محفوظ") : String(" / sensor mode saved");
    return t;
  }
  if (smartInputCfg[idx].mode == SMART_INPUT_MODE_ANALOG) {
    String t = String(smartInputValue[idx], 2);
    String u = smartInputUnit(idx);
    if (u.length()) { t += " "; t += u; }
    t += String(" / raw ") + String(smartInputRaw[idx]);
    t += smartInputActive[idx] ? (ar ? String(" / نشط") : String(" / ACTIVE")) : (ar ? String(" / متوقف") : String(" / OFF"));
    return t;
  }
  return smartInputActive[idx] ? (ar ? String("نشط") : String("ACTIVE")) : (ar ? String("متوقف") : String("OFF"));
}

String outputSelectOptions(byte selected) {
  String h;
  const byte outs[6] = {1, 2, 5, 6, 7, 8};
  const byte pins[6] = {OUTPUT1_PIN, OUTPUT2_PIN, OUTPUT5_PIN, OUTPUT6_PIN, OUTPUT7_PIN, OUTPUT8_PIN};
  for (byte i = 0; i < 6; i++) {
    byte o = outs[i];
    h += F("<option value='"); h += String(o); h += F("'");
    if (selected == o) h += F(" selected");
    h += F(">"); h += outputDisplayLabelHtml(o); h += F(" / Output "); h += String(o); h += F(" / GPIO"); h += String(pins[i]); h += F("</option>");
  }
  return h;
}

String smartInputTriggerOptions(byte selected, bool ar) {
  String h;
  for (byte m = 0; m <= SMART_INPUT_TRIGGER_WHILE_ON; m++) {
    h += F("<option value='"); h += String(m); h += F("'");
    if (selected == m) h += F(" selected");
    h += F(">"); h += smartInputTriggerText(m, ar); h += F("</option>");
  }
  return h;
}

String smartInputActionOptions(byte selected, bool ar) {
  String h;
  for (byte m = 0; m <= SMART_INPUT_ACTION_FOLLOW; m++) {
    h += F("<option value='"); h += String(m); h += F("'");
    if (selected == m) h += F(" selected");
    h += F(">"); h += smartInputActionText(m, ar); h += F("</option>");
  }
  return h;
}

String smartInputSensorTypeOptions(byte selected, bool ar) {
  String h;
  const byte vals[3] = {SMART_INPUT_SENSOR_DHT11, SMART_INPUT_SENSOR_DHT22, SMART_INPUT_SENSOR_DS18B20};
  for (byte k = 0; k < 3; k++) {
    byte v = vals[k];
    h += F("<option value='"); h += String(v); h += F("'");
    if (selected == v) h += F(" selected");
    h += F(">"); h += smartInputSensorTypeText(v, ar); h += F("</option>");
  }
  return h;
}

String smartInputSensorMetricOptions(byte selected, bool ar) {
  String h;
  for (byte v = 0; v <= SMART_INPUT_SENSOR_METRIC_HUM; v++) {
    h += F("<option value='"); h += String(v); h += F("'");
    if (selected == v) h += F(" selected");
    h += F(">"); h += smartInputSensorMetricText(v, ar); h += F("</option>");
  }
  return h;
}

String smartInputPinWarning(byte pin, byte mode, bool ar) {
  if (smartInputPinReserved(pin)) return ar ? String("تحذير: هذا GPIO محجوز لمخرج/RTC/DHT/LED ولن يتم حفظه") : String("Warning: GPIO reserved for output/RTC/DHT/LED and will not be saved");
  if (mode == SMART_INPUT_MODE_ANALOG && !smartInputPinAnalogCapable(pin)) return ar ? String("تحذير: هذا GPIO لا يدعم الأنالوج") : String("Warning: GPIO does not support analog");
  if (pin == 34 || pin == 35 || pin == 36 || pin == 39) return ar ? String("ملاحظة: هذا GPIO input-only ولا يوجد به pull-up داخلي") : String("Note: input-only GPIO, no internal pull-up");
  return String("");
}

void sendInputCardsPage() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  bool canEdit = roleAtLeast(WEB_ROLE_ENGINEER);
  if (!canEdit && !userHasAnyVisibleSmartInputCard()) { webServer.send(403, "text/plain", "Access denied"); return; }
  bool ar = (currentWebLanguage() == TG_LANG_AR);
  String html = htmlHeader(ar ? String("كروت المداخل الستة") : String("Six Input Cards"));
  html.reserve(24500);
  html += F("<div class='card'><h1>"); html += ar ? F("كروت المداخل الستة") : F("Six Input Cards"); html += F("</h1><div class='sub'>");
  html += ar ? F("مداخل ديجيتال/أنالوج/حساس مع إخفاء الإعدادات غير المستخدمة، والحفظ في EEPROM.") : F("Digital/Analog/Sensor input cards with irrelevant settings hidden and EEPROM storage.");
  html += F("</div><div class='row'><span class='k'>Smart Input Storage</span><span class='v'>"); html += smartInputStorageStatusText(); html += F("</span></div>");
  html += F("<div class='msg warn'>");
  html += ar ? F("الحماية تمنع حفظ GPIO المستخدم كمخرج أو RTC أو DHT الأساسي أو LED الإنترنت. GPIO34/35/36/39 مداخل فقط. إعدادات Sensor Mode محفوظة في EEPROM، والقراءة الفعلية للحساسات الإضافية مرحلة تالية.") : F("GPIO protection rejects pins used by outputs, RTC, main DHT or Internet LED. GPIO34/35/36/39 are input-only. Sensor Mode settings are saved in EEPROM; actual extra sensor reading engine is a next stage.");
  html += F("</div></div>");

  if (canEdit) { html += F("<form method='POST' action='/saveinputcards'>"); html += sessionHiddenInput(); }
  html += F("<div class='oc'>");
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) {
    if (!canEdit && !canCurrentUserSeeSmartInputCard(i)) continue;
    html += F("<div class='ocbox'><h1>"); html += smartInputName(i); html += F("</h1>");
    html += F("<div class='row'><span class='k'>GPIO</span><span class='v'>"); html += String(smartInputCfg[i].pin); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("النوع") : F("Mode"); html += F("</span><span class='v'>"); html += smartInputModeText(smartInputCfg[i].mode, ar); html += F("</span></div>");
    html += F("<div class='row'><span class='k'>"); html += ar ? F("القراءة الحالية") : F("Current Reading"); html += F("</span><span class='v' id='sicv"); html += String(i); html += F("'>"); html += smartInputStateText(i, ar); html += F("</span></div>");

    if (!canEdit) { html += F("</div>"); continue; }

    String warn = smartInputPinWarning(smartInputCfg[i].pin, smartInputCfg[i].mode, ar);
    if (warn.length()) { html += F("<div class='msg warn'>"); html += htmlEscapeText(warn); html += F("</div>"); }
    html += F("<label><input type='checkbox' name='en"); html += String(i); html += F("' "); if (smartInputCfg[i].enabled) html += F("checked"); html += F("> "); html += ar ? F("تفعيل هذا الدخل") : F("Enable this input"); html += F("</label>");
    html += F("<label>"); html += ar ? F("اسم الدخل") : F("Input name"); html += F("</label><input maxlength='"); html += String(SMART_INPUT_NAME_MAX_LEN); html += F("' name='nm"); html += String(i); html += F("' value='"); html += htmlEscapeText(smartInputName(i)); html += F("'>");
    html += F("<div class='grid2'><div><label>GPIO Pin</label><input type='number' min='0' max='39' name='pin"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].pin); html += F("'></div><div><label>"); html += ar ? F("النوع") : F("Mode"); html += F("</label><select class='simode' id='simode"); html += String(i); html += F("' data-i='"); html += String(i); html += F("' name='mode"); html += String(i); html += F("' onchange='siModeChanged("); html += String(i); html += F(")'>");
    html += F("<option value='0'"); if (smartInputCfg[i].mode == SMART_INPUT_MODE_DIGITAL) html += F(" selected"); html += F(">"); html += ar ? F("ديجيتال") : F("Digital"); html += F("</option>");
    html += F("<option value='1'"); if (smartInputCfg[i].mode == SMART_INPUT_MODE_ANALOG) html += F(" selected"); html += F(">"); html += ar ? F("أنالوج") : F("Analog"); html += F("</option>");
    html += F("<option value='2'"); if (smartInputCfg[i].mode == SMART_INPUT_MODE_SENSOR) html += F(" selected"); html += F(">"); html += ar ? F("حساس") : F("Sensor"); html += F("</option></select></div></div>");

    html += F("<div class='sidig' id='sidig"); html += String(i); html += F("'><div class='sub'>"); html += ar ? F("إعدادات الديجيتال") : F("Digital settings"); html += F("</div>");
    html += F("<label><input type='checkbox' name='ah"); html += String(i); html += F("' "); if (smartInputCfg[i].activeHigh) html += F("checked"); html += F("> "); html += ar ? F("الديجيتال نشط عند HIGH بدل LOW") : F("Digital active on HIGH instead of LOW"); html += F("</label>");
    html += F("<div class='grid2'><div><label>Debounce ms</label><input type='number' min='20' max='5000' name='db"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].debounceMs); html += F("'></div><div><label>"); html += ar ? F("تأخير قبل التنفيذ بالثواني") : F("Delay before action sec"); html += F("</label><input type='number' min='0' max='3600' name='dly"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].delaySec); html += F("'></div></div>");
    html += F("<div class='grid2'><div><label>"); html += ar ? F("أقل وقت فصل بالثواني") : F("Minimum OFF sec"); html += F("</label><input type='number' min='0' max='3600' name='moff"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].minOffSec); html += F("'></div><div><label>"); html += ar ? F("Trigger Mode") : F("Trigger Mode"); html += F("</label><select name='trg"); html += String(i); html += F("'>"); html += smartInputTriggerOptions(smartInputCfg[i].triggerMode, ar); html += F("</select></div></div></div>");

    html += F("<div class='siana' id='siana"); html += String(i); html += F("'><div class='sub'>"); html += ar ? F("إعدادات الأنالوج") : F("Analog settings"); html += F("</div>");
    html += F("<div class='grid2'><div><label>"); html += ar ? F("معامل الضرب") : F("Multiplier"); html += F("</label><input name='fac"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].factor, 4); html += F("'></div><div><label>Offset</label><input name='ofs"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].offset, 2); html += F("'></div></div>");
    html += F("<div class='grid2'><div><label>"); html += ar ? F("حد التشغيل") : F("Threshold"); html += F("</label><input name='thr"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].threshold, 2); html += F("'></div><div><label>Hysteresis</label><input name='hys"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].hysteresis, 2); html += F("'></div></div>");
    html += F("<div class='grid2'><div><label>Smoothing samples</label><input type='number' min='1' max='20' name='smo"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].smoothing); html += F("'></div><div><label>Unit</label><input maxlength='"); html += String(SMART_INPUT_UNIT_MAX_LEN); html += F("' name='unit"); html += String(i); html += F("' value='"); html += htmlEscapeText(smartInputUnit(i)); html += F("'></div></div></div>");

    html += F("<div class='sisensor' id='sisensor"); html += String(i); html += F("'><div class='sub'>"); html += ar ? F("إعدادات الحساس") : F("Sensor settings"); html += F("</div>");
    html += F("<div class='msg warn'>"); html += ar ? F("هذه المرحلة تحفظ نوع الحساس وإعداداته في EEPROM وتنظم الواجهة. تشغيل قراءة حساسات إضافية يتم في نسخة لاحقة بعد اختبار الحفظ.") : F("This stage saves sensor type/settings in EEPROM and organizes the UI. Actual extra sensor reading is added in a later version after storage is verified."); html += F("</div>");
    html += F("<div class='grid2'><div><label>"); html += ar ? F("نوع الحساس") : F("Sensor Type"); html += F("</label><select name='stype"); html += String(i); html += F("' id='stype"); html += String(i); html += F("' onchange='siSensorTypeChanged("); html += String(i); html += F(")'>"); html += smartInputSensorTypeOptions(smartInputCfg[i].sensorType, ar); html += F("</select></div><div><label>"); html += ar ? F("القراءة المستخدمة") : F("Reading Used"); html += F("</label><select name='smet"); html += String(i); html += F("' id='smet"); html += String(i); html += F("'>"); html += smartInputSensorMetricOptions(smartInputCfg[i].sensorMetric, ar); html += F("</select></div></div>");
    html += F("<div class='grid2'><div><label>"); html += ar ? F("حد الحرارة °C") : F("Temperature Threshold °C"); html += F("</label><input name='sthr"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].sensorTempThreshold, 1); html += F("'></div><div><label>"); html += ar ? F("حد الرطوبة %") : F("Humidity Threshold %"); html += F("</label><input name='shthr"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].sensorHumThreshold, 1); html += F("'></div></div>");
    html += F("<div class='grid2'><div><label>Hysteresis</label><input name='shys"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].hysteresis, 2); html += F("'></div><div><label>Unit</label><input maxlength='"); html += String(SMART_INPUT_UNIT_MAX_LEN); html += F("' name='sunit"); html += String(i); html += F("' value='"); html += htmlEscapeText(smartInputUnit(i)); html += F("'></div></div></div>");

    html += F("<label><input type='checkbox' name='tg"); html += String(i); html += F("' "); if (smartInputCfg[i].telegramNotify) html += F("checked"); html += F("> "); html += ar ? F("تفعيل إشعارات تيليجرام") : F("Enable Telegram notifications"); html += F("</label>");
    html += F("<label><input type='checkbox' name='tgon"); html += String(i); html += F("' "); if (smartInputCfg[i].notifyOnActive) html += F("checked"); html += F("> "); html += ar ? F("إشعار عند ON") : F("Notify on ON"); html += F("</label>");
    html += F("<label><input type='checkbox' name='tgoff"); html += String(i); html += F("' "); if (smartInputCfg[i].notifyOnClear) html += F("checked"); html += F("> "); html += ar ? F("إشعار عند OFF") : F("Notify on OFF"); html += F("</label>");
    html += F("<label>"); html += ar ? F("مدة منع تكرار الإشعار بالثواني") : F("Notification cooldown sec"); html += F("</label><input type='number' min='1' max='3600' name='cool"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].notifyCooldownSec); html += F("'>");
    html += F("<label>"); html += ar ? F("رسالة مخصصة للإشعار") : F("Custom notification message"); html += F("</label><input maxlength='"); html += String(SMART_INPUT_MSG_MAX_LEN); html += F("' name='msg"); html += String(i); html += F("' value='"); html += htmlEscapeText(String(smartInputCfg[i].customMsg)); html += F("'>");

    html += F("<label><input type='checkbox' name='act"); html += String(i); html += F("' "); if (smartInputCfg[i].outputAction) html += F("checked"); html += F("> "); html += ar ? F("تنفيذ أمر على مخرج") : F("Run output action"); html += F("</label>");
    html += F("<label>"); html += ar ? F("نوع الأمر") : F("Action type"); html += F("</label><select name='am"); html += String(i); html += F("'>"); html += smartInputActionOptions(smartInputCfg[i].actionMode, ar); html += F("</select>");
    html += F("<label>"); html += ar ? F("المخرج المطلوب") : F("Output to control"); html += F("</label><select name='out"); html += String(i); html += F("'>"); html += outputSelectOptions(smartInputCfg[i].actionOutput); html += F("</select>");
    html += F("<div class='grid2'><div><label>"); html += ar ? F("مدة التشغيل المؤقت بالثواني") : F("Pulse duration sec"); html += F("</label><input type='number' min='1' max='3600' name='dur"); html += String(i); html += F("' value='"); html += String(smartInputCfg[i].actionDurationSec); html += F("'></div><div>");
    html += F("<label><input type='checkbox' name='clr"); html += String(i); html += F("' "); if (smartInputCfg[i].offOnClear) html += F("checked"); html += F("> "); html += ar ? F("إيقاف المخرج عند رجوع الدخل طبيعي") : F("Turn output OFF when input returns normal"); html += F("</label></div></div>");
    html += F("</div>");
  }
  html += F("</div>");
  if (canEdit) { html += F("<button class='btn'>"); html += ar ? F("حفظ كروت المداخل") : F("Save Input Cards"); html += F("</button></form>"); }

  html += F("<script>function siModeChanged(i){let s=document.getElementById('simode'+i),d=document.getElementById('sidig'+i),a=document.getElementById('siana'+i),sn=document.getElementById('sisensor'+i);if(!s)return;let v=s.value;if(d)d.style.display=(v=='0')?'block':'none';if(a)a.style.display=(v=='1')?'block':'none';if(sn)sn.style.display=(v=='2')?'block':'none';siSensorTypeChanged(i)}function siSensorTypeChanged(i){let st=document.getElementById('stype'+i),sm=document.getElementById('smet'+i);if(st&&sm&&st.value=='2'){sm.value='0';sm.disabled=true}else if(sm){sm.disabled=false}}function siInitModes(){for(let i=0;i<6;i++)siModeChanged(i)}function updSI(d){for(let i=0;i<6;i++){let e=document.getElementById('sicv'+i);if(!e)continue;let x=d['i'+i];if(!x||!x.visible){e.innerHTML='--';continue;}e.innerHTML=x.text;e.className='v '+(x.active?'on':'off')}}function refSI(){fetch('/inputstatus?sid="); html += webSessionToken; html += F("').then(r=>r.json()).then(updSI).catch(e=>{})}siInitModes();setInterval(refSI,2000);refSI();</script>");
  html += mainMenuCardsHtml();
  html += htmlFooter();
  webServer.send(200, "text/html", html);
}

void handleSaveInputCards() {
  if (!requireRole(WEB_ROLE_ENGINEER)) return;
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) {
    smartInputCfg[i].version = EEPROM_SMART_INPUT_VERSION;
    smartInputCfg[i].enabled = webServer.hasArg(String("en") + i) ? 1 : 0;
    {
      int mv = webServer.arg(String("mode") + i).toInt();
      smartInputCfg[i].mode = (mv == 2) ? SMART_INPUT_MODE_SENSOR : ((mv == 1) ? SMART_INPUT_MODE_ANALOG : SMART_INPUT_MODE_DIGITAL);
    }
    int pin = webServer.arg(String("pin") + i).toInt();
    if (pin < 0) pin = 0; if (pin > 39) pin = smartInputDefaultPin(i);
    smartInputCfg[i].pin = smartInputSafePin((byte)pin, i, smartInputCfg[i].mode);
    smartInputCfg[i].activeHigh = webServer.hasArg(String("ah") + i) ? 1 : 0;
    smartInputCfg[i].telegramNotify = webServer.hasArg(String("tg") + i) ? 1 : 0;
    smartInputCfg[i].notifyOnActive = webServer.hasArg(String("tgon") + i) ? 1 : 0;
    smartInputCfg[i].notifyOnClear = webServer.hasArg(String("tgoff") + i) ? 1 : 0;
    smartInputCfg[i].outputAction = webServer.hasArg(String("act") + i) ? 1 : 0;
    byte out = (byte)webServer.arg(String("out") + i).toInt();
    if (!smartInputOutputValid(out)) out = 1;
    smartInputCfg[i].actionOutput = out;
    smartInputCfg[i].offOnClear = webServer.hasArg(String("clr") + i) ? 1 : 0;
    smartInputCfg[i].triggerMode = (byte)webServer.arg(String("trg") + i).toInt();
    smartInputCfg[i].actionMode = (byte)webServer.arg(String("am") + i).toInt();
    smartInputCfg[i].debounceMs = constrain(webServer.arg(String("db") + i).toInt(), 20, 5000);
    smartInputCfg[i].delaySec = constrain(webServer.arg(String("dly") + i).toInt(), 0, 3600);
    smartInputCfg[i].minOffSec = constrain(webServer.arg(String("moff") + i).toInt(), 0, 3600);
    smartInputCfg[i].actionDurationSec = constrain(webServer.arg(String("dur") + i).toInt(), 1, 3600);
    smartInputCfg[i].notifyCooldownSec = constrain(webServer.arg(String("cool") + i).toInt(), 1, 3600);
    smartInputCfg[i].smoothing = constrain(webServer.arg(String("smo") + i).toInt(), 1, 20);
    smartInputCfg[i].sensorType = (byte)constrain(webServer.arg(String("stype") + i).toInt(), 0, 2);
    smartInputCfg[i].sensorMetric = (byte)constrain(webServer.arg(String("smet") + i).toInt(), 0, 1);
    String nm = webServer.arg(String("nm") + i); nm.trim();
    if (!nm.length()) nm = String("Input Card ") + String(i + 1);
    nm.replace("\r", " "); nm.replace("\n", " ");
    nm.substring(0, SMART_INPUT_NAME_MAX_LEN).toCharArray(smartInputCfg[i].name, sizeof(smartInputCfg[i].name));
    String unit = (smartInputCfg[i].mode == SMART_INPUT_MODE_SENSOR) ? webServer.arg(String("sunit") + i) : webServer.arg(String("unit") + i);
    unit.trim(); unit.replace("\r", " "); unit.replace("\n", " ");
    unit.substring(0, SMART_INPUT_UNIT_MAX_LEN).toCharArray(smartInputCfg[i].unit, sizeof(smartInputCfg[i].unit));
    String cm = webServer.arg(String("msg") + i); cm.trim(); cm.replace("\r", " "); cm.replace("\n", " ");
    cm.substring(0, SMART_INPUT_MSG_MAX_LEN).toCharArray(smartInputCfg[i].customMsg, sizeof(smartInputCfg[i].customMsg));
    float fac = webServer.arg(String("fac") + i).toFloat();
    if (isnan(fac) || fac < -100000.0f || fac > 100000.0f) fac = 1.0f;
    smartInputCfg[i].factor = fac;
    float ofs = webServer.arg(String("ofs") + i).toFloat();
    if (isnan(ofs) || ofs < -1000000.0f || ofs > 1000000.0f) ofs = 0.0f;
    smartInputCfg[i].offset = ofs;
    float thr = webServer.arg(String("thr") + i).toFloat();
    if (isnan(thr) || thr < -1000000.0f || thr > 1000000.0f) thr = 1000.0f;
    smartInputCfg[i].threshold = thr;
    float hys = (smartInputCfg[i].mode == SMART_INPUT_MODE_SENSOR) ? webServer.arg(String("shys") + i).toFloat() : webServer.arg(String("hys") + i).toFloat();
    if (isnan(hys) || hys < 0.0f || hys > 1000000.0f) hys = SMART_INPUT_DEFAULT_HYSTERESIS;
    smartInputCfg[i].hysteresis = hys;
    float sth = webServer.arg(String("sthr") + i).toFloat();
    if (isnan(sth) || sth < -80.0f || sth > 150.0f) sth = 35.0f;
    smartInputCfg[i].sensorTempThreshold = sth;
    float shth = webServer.arg(String("shthr") + i).toFloat();
    if (isnan(shth) || shth < 0.0f || shth > 100.0f) shth = 70.0f;
    smartInputCfg[i].sensorHumThreshold = shth;
    sanitizeSmartInputCard(i);
    smartInputResetRuntime(i);
  }
  saveSmartInputCardsConfig();
  applySmartInputPinModes();
  autoBackupAfterSave("Smart input cards saved");
  webServer.sendHeader("Location", sessionUrl("/inputcards"), true);
  webServer.send(303, "text/plain", "Saved");
}

void sendSmartInputStatusJson() {
  if (!requireRole(WEB_ROLE_VIEWER)) return;
  bool ar = (currentWebLanguage() == TG_LANG_AR);
  String json = F("{");
  for (byte i = 0; i < SMART_INPUT_COUNT; i++) {
    if (i) json += F(",");
    bool visible = canCurrentUserSeeSmartInputCard(i);
    json += F("\"i"); json += String(i); json += F("\":{");
    json += F("\"visible\":"); json += visible ? F("true") : F("false"); json += F(",");
    if (visible) {
      json += F("\"name\":\""); json += jsonEscape(smartInputName(i)); json += F("\",");
      json += F("\"enabled\":"); json += smartInputCfg[i].enabled ? F("true") : F("false"); json += F(",");
      json += F("\"mode\":\""); json += smartInputCfg[i].mode == SMART_INPUT_MODE_SENSOR ? F("sensor") : (smartInputCfg[i].mode == SMART_INPUT_MODE_ANALOG ? F("analog") : F("digital")); json += F("\",");
      json += F("\"modeLabel\":\""); json += jsonEscape(smartInputModeText(smartInputCfg[i].mode, ar)); json += F("\",");
      json += F("\"read\":\""); json += jsonEscape(smartInputHomeReadingText(i, ar)); json += F("\",");
      json += F("\"extraLabel\":\""); json += jsonEscape(smartInputHomeExtraLabelText(i, ar)); json += F("\",");
      json += F("\"extra\":\""); json += jsonEscape(smartInputHomeExtraValueText(i, ar)); json += F("\",");
      json += F("\"active\":"); json += smartInputActive[i] ? F("true") : F("false"); json += F(",");
      json += F("\"raw\":"); json += String(smartInputRaw[i]); json += F(",");
      json += F("\"value\":"); json += String(smartInputValue[i], 2); json += F(",");
      json += F("\"unit\":\""); json += jsonEscape(smartInputUnit(i)); json += F("\",");
      json += F("\"text\":\""); json += jsonEscape(smartInputStateText(i, ar)); json += F("\",");
      json += F("\"detail\":\""); json += jsonEscape(homeSmartInputDetailText(i, ar)); json += F("\",");
      json += F("\"last\":\""); json += jsonEscape(String(smartInputLastCardAction[i])); json += F("\"");
    } else {
      json += F("\"active\":false,\"text\":\"--\"");
    }
    json += F("}");
  }
  json += F(",\"storage\":\""); json += jsonEscape(smartInputStorageStatusText()); json += F("\"}");
  webServer.send(200, "application/json", json);
}


// ========================= HOME STATUS DETAIL HELPERS V1.7.8.7 =========================

String homeDateTimeFromLocalEpoch(uint32_t ep, bool ar) {
  if (ep <= 1700000000UL) return String("--");
  time_t t = (time_t)ep;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char d[16];
  snprintf(d, sizeof(d), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  String x = String(dayName((byte)tmv.tm_wday));
  x += " ";
  x += d;
  x += " ";
  x += format12HourClock((byte)tmv.tm_hour, (byte)tmv.tm_min, 0, ar, false);
  return x;
}

String homeScheduleNextEventText(byte outputNumber, bool ar) {
  byte idx = outputAutoIndex(outputNumber);
  if (idx >= OUTPUT_AUTO_COUNT) return ar ? String("مخرج مساعد / بدون جدول") : String("Aux output / no schedule");
  OutputAutoConfig &cfg = outputAutoCfg[idx];
  if (!cfg.enabled || !cfg.scheduleEnable) return String("");
  if (!isTimeValidNow()) return ar ? String("الجدول ينتظر ضبط الوقت") : String("Schedule waiting for time");

  uint32_t nowEp = getDeviceEpoch();
  time_t nowT = (time_t)nowEp;
  struct tm tmNow;
  gmtime_r(&nowT, &tmNow);
  byte today = (byte)tmNow.tm_wday;
  uint32_t secToday = (uint32_t)tmNow.tm_hour * 3600UL + (uint32_t)tmNow.tm_min * 60UL + (uint32_t)tmNow.tm_sec;
  uint32_t baseToday = nowEp - secToday;

  uint32_t bestEp = 0;
  byte bestState = 0;

  // Candidate events over the coming week. For normal schedules the next ON comes before OFF.
  for (byte off = 0; off <= 7; off++) {
    byte d = (today + off) % 7;
    if (!cfg.dayEnabled[d]) continue;
    uint32_t base = baseToday + (uint32_t)off * 86400UL;
    for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
      if (!cfg.slotEnabled[d][sl]) continue;
      uint16_t st = cfg.dayStartMin[d][sl];
      uint16_t en = cfg.dayEndMin[d][sl];
      if (st == en || st > 1439 || en > 1439) continue;
      uint32_t onEp = base + (uint32_t)st * 60UL;
      uint32_t offEp = base + (uint32_t)en * 60UL;
      if (st > en) offEp += 86400UL; // Overnight schedule
      if (onEp > nowEp && (bestEp == 0 || onEp < bestEp)) { bestEp = onEp; bestState = 1; }
      if (offEp > nowEp && (bestEp == 0 || offEp < bestEp)) { bestEp = offEp; bestState = 0; }
    }
  }

  // If we are in an overnight period that started yesterday, the next event is today's OFF.
  byte prevDay = (today == 0) ? 6 : (today - 1);
  if (cfg.dayEnabled[prevDay]) {
    for (byte sl = 0; sl < SCHEDULE_SLOTS_PER_DAY; sl++) {
      if (!cfg.slotEnabled[prevDay][sl]) continue;
      uint16_t st = cfg.dayStartMin[prevDay][sl];
      uint16_t en = cfg.dayEndMin[prevDay][sl];
      if (st > en) {
        uint16_t nowMin = (uint16_t)(tmNow.tm_hour * 60 + tmNow.tm_min);
        if (nowMin < en) {
          uint32_t offEp = baseToday + (uint32_t)en * 60UL;
          if (offEp > nowEp && (bestEp == 0 || offEp < bestEp)) { bestEp = offEp; bestState = 0; }
        }
      }
    }
  }

  if (bestEp == 0) return ar ? String("لا يوجد حدث جدول قادم") : String("No upcoming schedule event");
  String r = ar ? String("الجدول القادم: ") : String("Next schedule: ");
  r += bestState ? (ar ? String("فتح ") : String("ON ")) : (ar ? String("قفل ") : String("OFF "));
  r += ar ? String("في ") : String("at ");
  r += homeDateTimeFromLocalEpoch(bestEp, ar);
  return r;
}

String homeManualDailyOffText(byte outputNumber, bool ar) {
  byte idx = outputAutoIndex(outputNumber);
  if (idx >= OUTPUT_AUTO_COUNT) return String("");
  if (!manualAutoOff9Enabled[idx] || !outputStateByNumber(outputNumber) || !outputLastManualOn[idx]) return String("");
  if (!isTimeValidNow()) {
    return (ar ? String("إغلاق يومي يدوي عند ") : String("Daily manual OFF at ")) + manualAutoOff9TimeText();
  }
  uint32_t nowEp = getDeviceEpoch();
  time_t nowT = (time_t)nowEp;
  struct tm tmNow;
  gmtime_r(&nowT, &tmNow);
  uint32_t secToday = (uint32_t)tmNow.tm_hour * 3600UL + (uint32_t)tmNow.tm_min * 60UL + (uint32_t)tmNow.tm_sec;
  uint32_t baseToday = nowEp - secToday;
  uint32_t target = baseToday + (uint32_t)manualAutoOff9Minute * 60UL;
  uint32_t todayYmd = currentYmdKey();
  if (target <= nowEp || manualAutoOff9LastYmd == todayYmd) target += 86400UL;
  String r = ar ? String("إغلاق يدوي يومي في ") : String("Daily manual OFF at ");
  r += homeDateTimeFromLocalEpoch(target, ar);
  return r;
}

String homeOutputDetailText(byte outputNumber, bool ar) {
  byte idx = outputAutoIndex(outputNumber);
  if (idx < OUTPUT_AUTO_COUNT) {
    uint32_t rem = manualAutoOffDurationRemainingSec(idx);
    if (rem > 0) {
      String r = ar ? String("تايمر: قفل بعد ") : String("Timer: OFF in ");
      r += formatHMS(rem);
      if (manualAutoOffDurDeadlineEpoch[idx] > 0 && isTimeValidNow()) {
        r += ar ? String(" / الساعة ") : String(" / at ");
        r += homeDateTimeFromLocalEpoch(manualAutoOffDurDeadlineEpoch[idx], ar);
      }
      return r;
    }
    String daily = homeManualDailyOffText(outputNumber, ar);
    if (daily.length()) return daily;
    String sched = homeScheduleNextEventText(outputNumber, ar);
    if (sched.length()) return sched;
    return ar ? String("تحكم يدوي / لا يوجد حدث قريب") : String("Manual / no upcoming event");
  }
  if (outputNumber == 7 || outputNumber == 8) return ar ? String("مخرج مساعد لكروت المداخل") : String("Aux output for input cards");
  return String("");
}

String homeOldInputDetailText(byte inputNumber, bool ar) {
  byte pin = 0;
  if (inputNumber == 7) pin = INPUT7_PIN;
  else if (inputNumber == 8) pin = INPUT8_PIN;
  else if (inputNumber == 9) pin = INPUT9_PIN;
  else if (inputNumber == 10) pin = INPUT10_PIN;
  else return String("");
  String r = ar ? String("دخل ديجيتال / GPIO") : String("Digital input / GPIO");
  r += String(pin);
  return r;
}

String homeSmartInputDetailText(byte idx, bool ar) {
  if (idx >= SMART_INPUT_COUNT) return String("");
  String r;
  if (!smartInputCfg[idx].enabled) {
    r = ar ? String("معطل / GPIO") : String("Disabled / GPIO");
    r += String(smartInputCfg[idx].pin);
    return r;
  }
  String actionText;
  if (smartInputCfg[idx].outputAction) {
    actionText = ar ? String(" / الأمر التالي: ") : String(" / Next Action: ");
    actionText += outputDisplayLabel(smartInputCfg[idx].actionOutput);
    actionText += String(" ");
    actionText += smartInputActionText(smartInputCfg[idx].actionMode, ar);
    if (smartInputCfg[idx].actionMode == SMART_INPUT_ACTION_PULSE) {
      actionText += ar ? String(" لمدة ") : String(" for ");
      actionText += String(smartInputCfg[idx].actionDurationSec);
      actionText += ar ? String(" ثانية") : String(" sec");
    }
  }
  if (smartInputCfg[idx].mode == SMART_INPUT_MODE_SENSOR) {
    r = ar ? String("حساس / ") : String("Sensor / ");
    r += smartInputSensorTypeText(smartInputCfg[idx].sensorType, ar);
    r += String(" / GPIO");
    r += String(smartInputCfg[idx].pin);
    r += ar ? String(" / القراءة: ") : String(" / reading: ");
    r += smartInputSensorMetricText(smartInputCfg[idx].sensorMetric, ar);
    r += ar ? String(" / محفوظ في EEPROM") : String(" / saved in EEPROM");
    r += actionText;
    return r;
  }
  if (smartInputCfg[idx].mode == SMART_INPUT_MODE_ANALOG) {
    r = ar ? String("أنالوج / GPIO") : String("Analog / GPIO");
    r += String(smartInputCfg[idx].pin);
    r += ar ? String(" / خام ") : String(" / raw ");
    r += String(smartInputRaw[idx]);
    r += ar ? String(" / حد ") : String(" / threshold ");
    r += String(smartInputCfg[idx].threshold, 2);
    String u = smartInputUnit(idx);
    if (u.length()) { r += String(" "); r += u; }
    r += actionText;
    return r;
  }
  r = ar ? String("ديجيتال / GPIO") : String("Digital / GPIO");
  r += String(smartInputCfg[idx].pin);
  r += smartInputCfg[idx].activeHigh ? (ar ? String(" / نشط HIGH") : String(" / active HIGH")) : (ar ? String(" / نشط LOW") : String(" / active LOW"));
  r += ar ? String(" / Debounce ") : String(" / debounce ");
  r += String(smartInputCfg[idx].debounceMs);
  r += String("ms");
  r += actionText;
  return r;
}

String smartInputHomeReadingText(byte idx, bool ar) {
  if (idx >= SMART_INPUT_COUNT) return String("--");
  if (!smartInputCfg[idx].enabled) return ar ? String("معطل") : String("Disabled");
  if (smartInputCfg[idx].mode == SMART_INPUT_MODE_SENSOR) {
    String r = smartInputSensorMetricText(smartInputCfg[idx].sensorMetric, ar);
    r += ar ? String(" / حد ") : String(" / threshold ");
    if (smartInputCfg[idx].sensorMetric == SMART_INPUT_SENSOR_METRIC_HUM) {
      r += String(smartInputCfg[idx].sensorHumThreshold, 1);
      r += String("%");
    } else {
      r += String(smartInputCfg[idx].sensorTempThreshold, 1);
      r += String(" °C");
    }
    r += ar ? String(" / قراءة الحساس الفعلية مرحلة تالية") : String(" / sensor reading engine pending");
    return r;
  }
  if (smartInputCfg[idx].mode == SMART_INPUT_MODE_ANALOG) {
    String r = ar ? String("خام ") : String("Raw ");
    r += String(smartInputRaw[idx]);
    r += String(" / ");
    r += String(smartInputValue[idx], 2);
    String u = smartInputUnit(idx);
    if (u.length()) { r += String(" "); r += u; }
    return r;
  }
  return smartInputActive[idx] ? (ar ? String("نشط") : String("ACTIVE")) : (ar ? String("متوقف") : String("OFF"));
}

String smartInputHomeExtraLabelText(byte idx, bool ar) {
  if (idx >= SMART_INPUT_COUNT) return String("--");
  if (smartInputCfg[idx].mode == SMART_INPUT_MODE_SENSOR) return ar ? String("نوع الحساس") : String("Sensor Type");
  if (smartInputCfg[idx].mode == SMART_INPUT_MODE_ANALOG) return ar ? String("المعامل") : String("Factor");
  return ar ? String("التريجر") : String("Trigger");
}

String smartInputHomeExtraValueText(byte idx, bool ar) {
  if (idx >= SMART_INPUT_COUNT) return String("--");
  if (smartInputCfg[idx].mode == SMART_INPUT_MODE_SENSOR) {
    String r = smartInputSensorTypeText(smartInputCfg[idx].sensorType, ar);
    r += String(" / ");
    r += smartInputSensorMetricText(smartInputCfg[idx].sensorMetric, ar);
    return r;
  }
  if (smartInputCfg[idx].mode == SMART_INPUT_MODE_ANALOG) return String(smartInputCfg[idx].factor, 3);
  return smartInputTriggerText(smartInputCfg[idx].triggerMode, ar);
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
  html.reserve(15500);

  if (ar) html += F("<div dir='rtl' style='text-align:right'>");

  // --- التعديل الصحيح هنا جوه دالة الـ HomePage ---
  html += F("<div class='card heroCard'><div class='heroTop'>");
  html += F("<img src='https://raw.githubusercontent.com/ahmedaaa8-bot/esp01-ota/main/lido_icon_256.png' style='width: 55px; height: 55px; border-radius: 8px; margin-inline-end: 15px;'>");
  html += F("<div><div class='heroKicker'>");
  // ------------------------------------------------
  html += homeHeroKickerDisplayText();
  html += F("</div><h1>");
  html += homeHeroTitleDisplayText();
  html += F("</h1><div class='sub heroSub'>");
  html += homeHeroSubtitleDisplayText();
  html += F("</div></div><div class='heroBadge'>"); html += homeBadgeDisplayText(); html += F("</div></div>");
  html += F("<div class='heroChips'>");
  html += F("<div class='miniChip'><b>👤 "); html += ar ? F("المستخدم") : F("User"); html += F("</b><span>"); html += currentWebUserName(); html += F("</span></div>");
  html += F("<div class='miniChip'><b>🛡️ "); html += ar ? F("الصلاحية") : F("Role"); html += F("</b><span>"); html += roleNameForLang(currentWebRole, ar); html += F("</span></div>");
  html += F("<div class='miniChip'><b>🌐 "); html += ar ? F("اللغة") : F("Language"); html += F("</b><span>"); html += ar ? F("العربية") : F("English"); html += F("</span></div>");
  html += F("</div>");
  if (admin || canEngineer) html += F("<div class='heroActions'>");
  if (admin) { html += F("<a class='btn btn2 heroAction' href='"); html += sessionUrl("/esp32"); html += F("'>⚙️ Device Realtime / MQTT / Telegram</a>"); }
  if (admin) { html += F("<a class='btn btn2 heroAction' href='"); html += sessionUrl("/uisettings"); html += F("'>🎨 "); html += ar ? F("إعدادات عنوان الهوم") : F("UI Branding Settings"); html += F("</a>"); }
  if (canEngineer) { html += F("<a class='btn btn2 heroAction' href='"); html += sessionUrl("/outputauto"); html += F("'>📅 "); html += ar ? F("إعدادات المخارج والجداول") : F("Output Schedules / Manual Options"); html += F("</a>"); }
  if (admin || canEngineer) html += F("</div>");
  if (mustChangeAdminPassword()) html += ar ? F("<div class='card bad'><b>تنبيه أمان:</b> غيّر كلمة مرور admin/admin الافتراضية من إدارة المستخدمين.</div>") : F("<div class='card bad'><b>Security:</b> change default admin/admin from User Management.</div>");
  html += F("</div>");

  bool homeWifiOk = (WiFi.status() == WL_CONNECTED);
  bool homeStorageOk = (eepromBeginOk && storageFsBeginOk);
  html += F("<div class='card statusbarcard cockpitCard'><h1>🛰️ "); html += ar ? F("شريط حالة الجهاز") : F("Device Status Bar"); html += F("</h1><div class='sysbar'>");
  html += F("<div class='sysitem'><b>📶 "); html += ar ? F("واي فاي") : F("WiFi"); html += F("</b><span class='"); html += homeWifiOk ? F("ok") : F("bad"); html += F("'>"); html += homeWifiOk ? (ar ? F("متصل") : F("Connected")) : (ar ? F("غير متصل") : F("Offline")); html += F("</span></div>");
  html += F("<div class='sysitem'><b>🌐 IP</b><span>"); html += homeWifiOk ? WiFi.localIP().toString() : WiFi.softAPIP().toString(); html += F("</span></div>");
  html += F("<div class='sysitem'><b>🕒 "); html += ar ? F("مصدر الوقت") : F("Time Source"); html += F("</b><span>"); html += clockStateText(); html += F("</span></div>");
  html += F("<div class='sysitem'><b>🔋 RTC</b><span>"); html += rtcStatusText(); html += F("</span></div>");
  html += F("<div class='sysitem'><b>🧠 Free Heap</b><span>"); html += String(ESP.getFreeHeap()); html += F(" bytes</span></div>");
  html += F("<div class='sysitem'><b>💾 "); html += ar ? F("التخزين") : F("Storage"); html += F("</b><span class='"); html += homeStorageOk ? F("ok") : F("warn"); html += F("'>"); html += homeStorageOk ? F("OK") : F("Check"); html += F("</span></div>");
  html += F("</div></div>");

  html += webLanguageCardHtml();
  if (homeShowQuickLinksSection) html += homeExternalLinksCardHtml();

  if (homeShowWeatherSections) {
  updateCityWeather(false);
  html += F("<div class='weatherSplitWrap'>");

  html += F("<div class='card sensorcard sensorLiveCard'><div class='splitHead'><h1>🟢 ");
  html += ar ? F("قراءة الحساس الحية") : F("Live Sensor Readings");
  html += F("</h1><span class='sourceBadge liveBadge'>");
  html += ar ? F("قراءة حية") : F("LIVE SENSOR");
  html += F("</span></div><div class='sub'>");
  html += ar ? F("القيم التالية تأتي مباشرة من الحساس المتصل بالجهاز الآن.") : F("Direct live readings from the physical sensor connected to this device.");
  html += F("</div><div class='sensegrid sourceGrid'>");
  html += F("<div class='senseitem'><div class='senseicon'>🌡️</div><div class='senselabel'>"); html += ar ? F("حرارة الحساس الحية") : F("Live Sensor Temperature"); html += F("</div><div class='senseval tempc'><span id='temp'>--</span><small> °C</small></div></div>");
  html += F("<div class='senseitem'><div class='senseicon'>💧</div><div class='senselabel'>"); html += ar ? F("رطوبة الحساس الحية") : F("Live Sensor Humidity"); html += F("</div><div class='senseval humc'><span id='hum'>--</span><small> %</small></div></div>");
  html += F("</div><div class='weatherNote'>");
  html += ar ? F("📡 المصدر: حساس الجهاز الداخلي - قراءة حية من الجهاز") : F("📡 Source: internal device sensor - live reading from device");
  html += F("</div></div>");

  html += F("<div class='card sensorcard internetWeatherCard'><div class='splitHead'><h1>🔵 ");
  html += ar ? F("طقس الإنترنت") : F("Internet Weather");
  html += F("</h1><span class='sourceBadge onlineBadge'>");
  html += ar ? F("من الإنترنت") : F("ONLINE WEATHER");
  html += F("</span></div><div class='sub'>");
  html += ar ? F("القيم التالية من الإنترنت للمدينة المختارة: ") : F("Weather service reading for selected city: ");
  html += htmlEscapeText(prayerWeatherAreaName(ar));
  html += F("</div><div class='sensegrid sourceGrid'>");
  html += F("<div class='senseitem'><div class='senseicon'>☁️</div><div class='senselabel'>"); html += ar ? F("حرارة الإنترنت") : F("Internet Temperature"); html += F("</div><div class='senseval tempc'><span id='owtempHome'>");
  html += isnan(cityWeatherTemp) ? String("--") : String(cityWeatherTemp, 1);
  html += F("</span><small> °C</small></div></div>");
  html += F("<div class='senseitem'><div class='senseicon'>🌦️</div><div class='senselabel'>"); html += ar ? F("رطوبة الإنترنت") : F("Internet Humidity"); html += F("</div><div class='senseval humc'><span id='owhumHome'>");
  html += isnan(cityWeatherHum) ? String("--") : String(cityWeatherHum, 0);
  html += F("</span><small> %</small></div></div>");
  html += F("</div><div class='weatherNote weatherOnlineLine'>🌐 ");
  html += ar ? F("المصدر: الإنترنت") : F("Source: Internet");
  html += F(" | 📍 "); html += htmlEscapeText(prayerWeatherAreaName(ar));
  html += F(" | "); html += ar ? F("الحالة: ") : F("Status: ");
  html += F("<span id='owstatusHome'>"); html += htmlEscapeText(lastCityWeatherMessage); html += F("</span>");
  html += F(" | "); html += ar ? F("آخر تحديث: ") : F("Last update: ");
  html += F("<span id='owtimeHome'>"); html += htmlEscapeText(lastCityWeatherTime); html += F("</span>");
  html += F("</div></div>");

  html += F("</div>");
  }

  bool homeShowOutputs = canCurrentUserSeeField(1) || canCurrentUserSeeField(2) || canCurrentUserSeeField(5) || canCurrentUserSeeField(6) || roleAtLeast(WEB_ROLE_ENGINEER);
  bool homeShowOldInputs = canCurrentUserSeeField(7) || canCurrentUserSeeField(8) || canCurrentUserSeeField(9) || canCurrentUserSeeField(10);
  bool homeShowSmartInputs = false;
  for (byte ic = 0; ic < SMART_INPUT_COUNT; ic++) {
    if (canCurrentUserSeeSmartInputCard(ic)) { homeShowSmartInputs = true; break; }
  }


  if (homeShowAllowedStatusSection && userHasAnyVisibleIoField()) {
    html += F("<div class='card statusPanel'><h1>🧭 "); html += ar ? F("الحالات المصرح بها") : F("Allowed Status"); html += F("</h1><div class='sub'>"); html += ar ? F("المخارج والمداخل المسموح بها فقط، منظمة في مجموعات واضحة.") : F("Only your allowed outputs and inputs, organized in clear groups."); html += F("</div><div class='statuslist'>");

    if (homeShowOutputs) {
      html += F("<div class='statusgroup'><div class='statusgroup-title'>"); html += ar ? F("🔌 المخارج") : F("🔌 Outputs"); html += F("</div>");
      if (canCurrentUserSeeField(1)) { html += F("<div class='statusrow2 outrowCtl'><span class='stname'><span class='statusicon'>🔌</span>"); html += fieldLabelHtml(1); html += F("</span><span id='sd1' class='stdetail'>"); html += htmlEscapeText(homeOutputDetailText(1, ar)); html += F("</span><span class='outStateBox'><span id='s1' class='statepill offpill'>--</span>"); if (canCurrentUserControlField(1)) html += F("<label class='sw miniSw'><input id='sws1' type='checkbox' onchange='setOut(1,this.checked?1:0)'><span class='sld'></span></label>"); html += F("</span></div>"); }
      if (canCurrentUserSeeField(2)) { html += F("<div class='statusrow2 outrowCtl'><span class='stname'><span class='statusicon'>💡</span>"); html += fieldLabelHtml(2); html += F("</span><span id='sd2' class='stdetail'>"); html += htmlEscapeText(homeOutputDetailText(2, ar)); html += F("</span><span class='outStateBox'><span id='s2' class='statepill offpill'>--</span>"); if (canCurrentUserControlField(2)) html += F("<label class='sw miniSw'><input id='sws2' type='checkbox' onchange='setOut(2,this.checked?1:0)'><span class='sld'></span></label>"); html += F("</span></div>"); }
      if (canCurrentUserSeeField(5)) { html += F("<div class='statusrow2 outrowCtl'><span class='stname'><span class='statusicon'>🔌</span>"); html += fieldLabelHtml(5); html += F("</span><span id='sd5' class='stdetail'>"); html += htmlEscapeText(homeOutputDetailText(5, ar)); html += F("</span><span class='outStateBox'><span id='s5' class='statepill offpill'>--</span>"); if (canCurrentUserControlField(5)) html += F("<label class='sw miniSw'><input id='sws5' type='checkbox' onchange='setOut(5,this.checked?1:0)'><span class='sld'></span></label>"); html += F("</span></div>"); }
      if (canCurrentUserSeeField(6)) { html += F("<div class='statusrow2 outrowCtl'><span class='stname'><span class='statusicon'>🔌</span>"); html += fieldLabelHtml(6); html += F("</span><span id='sd6' class='stdetail'>"); html += htmlEscapeText(homeOutputDetailText(6, ar)); html += F("</span><span class='outStateBox'><span id='s6' class='statepill offpill'>--</span>"); if (canCurrentUserControlField(6)) html += F("<label class='sw miniSw'><input id='sws6' type='checkbox' onchange='setOut(6,this.checked?1:0)'><span class='sld'></span></label>"); html += F("</span></div>"); }
      if (roleAtLeast(WEB_ROLE_ENGINEER)) { html += F("<div class='statusrow2 outrowCtl'><span class='stname'><span class='statusicon'>⚙️</span>"); html += outputDisplayLabelHtml(7); html += F("</span><span id='sd7' class='stdetail'>"); html += htmlEscapeText(homeOutputDetailText(7, ar)); html += F("</span><span class='outStateBox'><span id='s7' class='statepill offpill'>--</span><label class='sw miniSw'><input id='sws7' type='checkbox' onchange='setOut(7,this.checked?1:0)'><span class='sld'></span></label></span></div>"); }
      if (roleAtLeast(WEB_ROLE_ENGINEER)) { html += F("<div class='statusrow2 outrowCtl'><span class='stname'><span class='statusicon'>⚙️</span>"); html += outputDisplayLabelHtml(8); html += F("</span><span id='sd8' class='stdetail'>"); html += htmlEscapeText(homeOutputDetailText(8, ar)); html += F("</span><span class='outStateBox'><span id='s8' class='statepill offpill'>--</span><label class='sw miniSw'><input id='sws8' type='checkbox' onchange='setOut(8,this.checked?1:0)'><span class='sld'></span></label></span></div>"); }
      html += F("</div>");
    }

    if (homeShowOldInputs) {
      html += F("<div class='statusgroup'><div class='statusgroup-title'>"); html += ar ? F("🔘 المداخل") : F("🔘 Inputs"); html += F("</div>");
      if (canCurrentUserSeeField(7)) { html += F("<div class='statusrow2'><span class='stname'><span class='statusicon'>🔘</span>"); html += fieldLabelHtml(7); html += F("</span><span id='sid7' class='stdetail'>"); html += htmlEscapeText(homeOldInputDetailText(7, ar)); html += F("</span><span id='si7' class='statepill offpill'>--</span></div>"); }
      if (canCurrentUserSeeField(8)) { html += F("<div class='statusrow2'><span class='stname'><span class='statusicon'>🔘</span>"); html += fieldLabelHtml(8); html += F("</span><span id='sid8' class='stdetail'>"); html += htmlEscapeText(homeOldInputDetailText(8, ar)); html += F("</span><span id='si8' class='statepill offpill'>--</span></div>"); }
      if (canCurrentUserSeeField(9)) { html += F("<div class='statusrow2'><span class='stname'><span class='statusicon'>🔘</span>"); html += fieldLabelHtml(9); html += F("</span><span id='sid9' class='stdetail'>"); html += htmlEscapeText(homeOldInputDetailText(9, ar)); html += F("</span><span id='si9' class='statepill offpill'>--</span></div>"); }
      if (canCurrentUserSeeField(10)) { html += F("<div class='statusrow2'><span class='stname'><span class='statusicon'>🔘</span>"); html += fieldLabelHtml(10); html += F("</span><span id='sid10' class='stdetail'>"); html += htmlEscapeText(homeOldInputDetailText(10, ar)); html += F("</span><span id='si10' class='statepill offpill'>--</span></div>"); }
      html += F("</div>");
    }


    html += F("</div><div class='msg' id='outmsg'>"); html += ar ? F("جاهز") : F("Ready"); html += F("</div></div>");
  } else if (homeShowAllowedStatusSection) {
    html += F("<div class='card statusPanel'><h1>🧭 "); html += ar ? F("الحالات المصرح بها") : F("Allowed Status"); html += F("</h1><div class='msg warn'>"); html += ar ? F("لا توجد مخارج أو مداخل ظاهرة لهذا المستخدم. اطلب من الأدمن تفعيل الصلاحيات.") : F("No outputs or inputs are visible for this user. Ask Admin to enable field permissions."); html += F("</div></div>");
  }

  if (homeShowInputCardsSection && homeShowSmartInputs) {
    html += F("<div class='card sicHomePanel'><h1>📦 "); html += ar ? F("كروت المداخل") : F("Input Cards"); html += F("</h1><div class='sub'>");
    html += ar ? F("كل كارت مقفول يعرض الملخص فقط. اضغط على الكارت لفتح التفاصيل أو قفله.") : F("Each card is collapsed by default and shows a summary. Tap the card to open or close details.");
    html += F("</div><div class='sicHomeList'>");
    for (byte ic = 0; ic < SMART_INPUT_COUNT; ic++) {
      if (canCurrentUserSeeSmartInputCard(ic)) {
        String cardMode = smartInputModeText(smartInputCfg[ic].mode, ar);
        String cardReading = smartInputHomeReadingText(ic, ar);
        String cardExtraLabel = smartInputHomeExtraLabelText(ic, ar);
        String cardExtraValue = smartInputHomeExtraValueText(ic, ar);
        String cardAction = smartInputCfg[ic].outputAction ? (outputDisplayLabel(smartInputCfg[ic].actionOutput) + String(" ") + smartInputActionText(smartInputCfg[ic].actionMode, ar)) : (ar ? String("لا يوجد أمر") : String("No action"));
        String lastExec = String(smartInputLastCardAction[ic]);
        if (!lastExec.length()) lastExec = "none";
        html += F("<div class='icToggleCard rowOff' id='sicCardHome"); html += String(ic); html += F("'>");
        html += F("<div class='sicHead' onclick='toggleSicCard(this)'><span><span class='sicTitle'><span class='statusicon'>📦</span><b>");
        html += String("Input Card ") + String(ic + 1) + F(" - ") + htmlEscapeText(smartInputName(ic));
        html += F("</b></span><span id='sicDHome"); html += String(ic); html += F("' class='sicSub'>"); html += htmlEscapeText(homeSmartInputDetailText(ic, ar)); html += F("</span><span class='sicExample'>");
        html += ar ? F("اضغط للفتح والقفل") : F("Tap to open / collapse");
        html += F("</span></span><span id='sicHome"); html += String(ic); html += F("' class='statepill offpill'>--</span><span class='sicChevron'>⌄</span></div>");
        html += F("<div class='sicBody'><div class='sicGrid'>");
        html += F("<div class='sicMeta'><b>"); html += ar ? F("النوع") : F("Type"); html += F("</b><span id='sicModeHome"); html += String(ic); html += F("'>"); html += htmlEscapeText(cardMode); html += F("</span></div>");
        html += F("<div class='sicMeta'><b>GPIO</b><span>"); html += String(smartInputCfg[ic].pin); html += F("</span></div>");
        html += F("<div class='sicMeta'><b>"); html += ar ? F("آخر قراءة") : F("Last Reading"); html += F("</b><span id='sicReadHome"); html += String(ic); html += F("'>"); html += htmlEscapeText(cardReading); html += F("</span></div>");
        html += F("<div class='sicMeta'><b id='sicExtraLabelHome"); html += String(ic); html += F("'>"); html += htmlEscapeText(cardExtraLabel); html += F("</b><span id='sicExtraHome"); html += String(ic); html += F("'>"); html += htmlEscapeText(cardExtraValue); html += F("</span></div>");
        html += F("<div class='sicMeta'><b>Telegram</b><span>"); html += smartInputCfg[ic].telegramNotify ? (ar ? F("يرسل") : F("Notify")) : (ar ? F("لا يرسل") : F("No notify")); html += F("</span></div>");
        html += F("<div class='sicMeta'><b>"); html += ar ? F("يتحكم في") : F("Controls"); html += F("</b><span>"); html += htmlEscapeText(cardAction); html += F("</span></div>");
        html += F("<div class='sicMeta'><b>"); html += ar ? F("آخر تنفيذ") : F("Last Execution"); html += F("</b><span id='sicLastHome"); html += String(ic); html += F("'>"); html += htmlEscapeText(lastExec); html += F("</span></div>");
        html += F("<div class='sicMeta'><b>"); html += ar ? F("الحالة") : F("Enabled"); html += F("</b><span>"); html += smartInputCfg[ic].enabled ? (ar ? F("مفعل") : F("Enabled")) : (ar ? F("معطل") : F("Disabled")); html += F("</span></div>");
        html += F("<div class='sicMeta sicMetaWide'><b>"); html += ar ? F("الإجراء القادم") : F("Next Action"); html += F("</b><span>"); html += htmlEscapeText(cardAction); html += F("</span></div>");
        html += F("</div></div></div>");
      }
    }
    html += F("</div></div>");
  }

  // V1.7.8.17: mobile CSS keeps output switches inside the Allowed Status card.
  // V1.7.8.16: Quick Control removed from Home; use switches inside Allowed Status rows.

  if (admin && homeShowTechnicalSection) {
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
  html += F("var outState={1:0,2:0,5:0,6:0,7:0,8:0};");
  html += F("var TXT_ON='"); html += ar ? F("يعمل") : F("ON"); html += F("',TXT_OFF='"); html += ar ? F("متوقف") : F("OFF"); html += F("',TXT_ACTIVE='"); html += ar ? F("نشط") : F("ACTIVE"); html += F("',TXT_CUR_ON='"); html += ar ? F("يعمل الآن") : F("Currently ON"); html += F("',TXT_CUR_OFF='"); html += ar ? F("متوقف الآن") : F("Currently OFF"); html += F("';");
  html += F("function setPill(id,on,onText,offText,activeClass){let e=document.getElementById(id);if(!e)return;e.innerHTML=on?onText:offText;let ac=activeClass||'onpill';e.className='statepill '+(on?ac:'offpill');let r=e.closest?e.closest('.statusrow2,.icToggleCard'):null;if(r){r.classList.remove('rowOn','rowOff','rowActive');r.classList.add(on?(ac=='activepill'?'rowActive':'rowOn'):'rowOff')}}function syncOutSwitches(i,v){let a=document.getElementById('sw'+i),b=document.getElementById('sws'+i);if(a)a.checked=!!v;if(b)b.checked=!!v}function paintOutOne(i,v){if(v===null||v===undefined)return;outState[i]=v?1:0;setPill('s'+i,v,TXT_ON,TXT_OFF,'onpill');syncOutSwitches(i,v);let c=document.getElementById('ctl'+i);if(c){c.innerHTML=v?TXT_CUR_ON:TXT_CUR_OFF;c.className='small '+(v?'ok':'bad')}}function paintOutDetail(i,t){let e=document.getElementById('sd'+i);if(e&&t!==undefined){e.innerHTML=t||'';let x=(t||'').toLowerCase();e.className='stdetail '+((x.indexOf('timer')>=0||x.indexOf('تايمر')>=0)?'timerdetail':((x.indexOf('schedule')>=0||x.indexOf('جدول')>=0||x.indexOf('daily')>=0||x.indexOf('يومي')>=0)?'scheduledetail':''))}}function paintInput(i,v){if(v===null||v===undefined)return;setPill('si'+i,v,TXT_ACTIVE,TXT_OFF,'activepill')}");
  html += F("function paint(d){let t=document.getElementById('temp'),h=document.getElementById('hum');if(t)t.innerHTML=(d.temp==null?'--':d.temp);if(h)h.innerHTML=(d.hum==null?'--':d.hum);let ot=document.getElementById('owtempHome'),oh=document.getElementById('owhumHome'),os=document.getElementById('owstatusHome'),ou=document.getElementById('owtimeHome');if(ot)ot.innerHTML=(d.owTemp==null?'--':d.owTemp);if(oh)oh.innerHTML=(d.owHum==null?'--':d.owHum);if(os)os.innerHTML=d.owStatus||'--';if(ou)ou.innerHTML=d.owTime||'--';paintOutOne(1,d.o1);paintOutOne(2,d.o2);paintOutOne(5,d.o5);paintOutOne(6,d.o6);paintOutOne(7,d.o7);paintOutOne(8,d.o8);paintOutDetail(1,d.o1Detail);paintOutDetail(2,d.o2Detail);paintOutDetail(5,d.o5Detail);paintOutDetail(6,d.o6Detail);paintOutDetail(7,d.o7Detail);paintOutDetail(8,d.o8Detail);paintInput(7,d.i7);paintInput(8,d.i8);paintInput(9,d.i9);paintInput(10,d.i10)}");
  html += F("function toggleSicCard(head){let c=head&&head.closest?head.closest('.icToggleCard'):null;if(!c)return;c.classList.toggle('sicOpen');let ch=c.querySelector('.sicChevron');if(ch)ch.innerHTML=c.classList.contains('sicOpen')?'⌃':'⌄'}function paintSIC(d){for(let i=0;i<6;i++){let e=document.getElementById('sicHome'+i);if(!e)continue;let card=e.closest?e.closest('.icToggleCard'):null;let x=d['i'+i];if(!x||!x.visible){e.innerHTML='--';e.className='statepill offpill';if(card){card.classList.remove('rowActive','rowOn');card.classList.add('rowOff')}continue;}e.innerHTML=x.text||'--';e.className='statepill '+(x.active?'activepill':'offpill');if(card){card.classList.remove('rowActive','rowOn','rowOff');card.classList.add(x.active?'rowActive':'rowOff')}let de=document.getElementById('sicDHome'+i);if(de)de.innerHTML=x.detail||'';let mt=document.getElementById('sicModeHome'+i);if(mt)mt.innerHTML=x.modeLabel||x.mode||'--';let rd=document.getElementById('sicReadHome'+i);if(rd)rd.innerHTML=x.read||('Raw '+(x.raw!==undefined?x.raw:'--')+' / '+(x.value!==undefined?x.value:'--')+(x.unit?' '+x.unit:''));let el=document.getElementById('sicExtraLabelHome'+i);if(el)el.innerHTML=x.extraLabel||'';let ev=document.getElementById('sicExtraHome'+i);if(ev)ev.innerHTML=x.extra||'';let le=document.getElementById('sicLastHome'+i);if(le)le.innerHTML=x.last||'none';}}function refreshSIC(){fetch('/inputstatus?sid="); html += webSessionToken; html += F("').then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(paintSIC).catch(e=>{})}");
  html += F("function refresh(){fetch('/status?sid="); html += webSessionToken; html += F("').then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(paint).catch(e=>{});refreshSIC()}function setOut(o,s){let m=document.getElementById('outmsg');if(m)m.innerHTML='"); html += ar ? F("جاري الإرسال...") : F("Sending..."); html += F("';fetch('/setoutput',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'out='+o+'&state='+s+'&sid="); html += webSessionToken; html += F("'}).then(r=>{if(!r.ok)throw new Error('auth');return r.json()}).then(d=>{paint(d);if(m)m.innerHTML=d.changed?'"); html += ar ? F("تم التنفيذ") : F("Applied"); html += F("':'"); html += ar ? F("لا يوجد تغيير") : F("No change"); html += F("';}).catch(e=>{syncOutSwitches(o,outState[o]);if(m)m.innerHTML='<span class=bad>"); html += ar ? F("فشل/الجلسة") : F("Failed/session"); html += F("</span>';})}setInterval(refresh,7000);refresh();");
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

  // Make sure the user page/intro, activation codes and activation-log rescue files are current before packing the full backup.
  saveUserPagePersistentConfig("auto-backup-pack");
  saveActivationCodesToFs();
  saveActivationLogToFs();

  uint16_t userSize = 0;
  uint32_t userChecksum = fsFileChecksum32(USER_PAGE_STORE_PATH, userSize);
  uint16_t actSize = 0;
  uint32_t actChecksum = fsFileChecksum32(ACTIVATION_CODE_STORE_PATH, actSize);
  uint16_t logSize = 0;
  uint32_t logChecksum = fsFileChecksum32(ACTIVATION_LOG_STORE_PATH, logSize);
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
  fileWriteU16(f, logSize);
  fileWriteU32(f, logChecksum);

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
  if (!copyFileBytesTo(f, ACTIVATION_LOG_STORE_PATH)) { f.close(); autoBackupLastMessage = "Activation log copy failed"; saveAutoBackupConfig(); return false; }
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
  uint16_t logSize = 0;
  uint32_t logChecksum = 0;
  if (fmt >= 3) {
    logSize = fileReadU16(f);
    logChecksum = fileReadU32(f);
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

  if (fmt >= 3 && logSize > 0) {
    if (SPIFFS.exists(AUTO_BACKUP_TMP_LOG)) SPIFFS.remove(AUTO_BACKUP_TMP_LOG);
    File lf = SPIFFS.open(AUTO_BACKUP_TMP_LOG, FILE_WRITE);
    if (!lf) { f.close(); msg = "Activation log temp open failed"; return false; }
    uint32_t lsum = 2166136261UL;
    for (uint16_t i = 0; i < logSize; i++) {
      int b = f.read();
      if (b < 0) { lf.close(); f.close(); msg = "Incomplete activation log data"; return false; }
      byte by = (byte)b;
      lsum ^= by;
      lsum *= 16777619UL;
      lf.write(&by, 1);
      yield();
    }
    lf.flush();
    lf.close();
    if (lsum != logChecksum) { f.close(); SPIFFS.remove(AUTO_BACKUP_TMP_LOG); msg = "Activation log checksum mismatch"; return false; }
    if (SPIFFS.exists(ACTIVATION_LOG_STORE_PATH)) SPIFFS.remove(ACTIVATION_LOG_STORE_PATH);
    SPIFFS.rename(AUTO_BACKUP_TMP_LOG, ACTIVATION_LOG_STORE_PATH);
  }
  f.close();
  commitEeprom("restore-auto-backup");
  loadActivationLogFromFs();
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

// Download all stored auto backups in one TAR file.
// This streams directly to the browser and does not create a temporary large file in SPIFFS.
void tarWriteOctalField(char* dst, byte len, uint32_t value) {
  for (byte i = 0; i < len; i++) dst[i] = '0';
  dst[len - 1] = '\0';
  byte pos = len - 2;
  while (value > 0 && pos < len) {
    dst[pos] = char('0' + (value & 7));
    value >>= 3;
    if (pos == 0) break;
    pos--;
  }
}

void tarWriteHeader(WiFiClient& client, const String& fileName, uint32_t fileSize) {
  char h[512];
  memset(h, 0, sizeof(h));
  String n = fileName;
  if (n.startsWith("/")) n.remove(0, 1);
  if (n.length() > 99) n = n.substring(n.length() - 99);
  strncpy(h, n.c_str(), 100);
  tarWriteOctalField(h + 100, 8, 0644);
  tarWriteOctalField(h + 108, 8, 0);
  tarWriteOctalField(h + 116, 8, 0);
  tarWriteOctalField(h + 124, 12, fileSize);
  tarWriteOctalField(h + 136, 12, (uint32_t)(millis() / 1000UL));
  memset(h + 148, ' ', 8);
  h[156] = '0';
  memcpy(h + 257, "ustar", 5);
  memcpy(h + 263, "00", 2);
  uint32_t sum = 0;
  for (uint16_t i = 0; i < 512; i++) sum += (uint8_t)h[i];
  tarWriteOctalField(h + 148, 8, sum);
  h[154] = '\0';
  h[155] = ' ';
  client.write((const uint8_t*)h, 512);
}

void handleDownloadAllAutoBackups() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  if (!storageFsBeginOk && !beginStorageFs()) { webServer.send(500, "text/plain", "FS not mounted"); return; }
  String tarName = String("all_backups_") + backupTimestampForFile() + String("_v") + firmwareVersionForFile() + String("_b") + String(FW_BUILD) + String(".tar");
  webServer.sendHeader("Content-Disposition", String("attachment; filename=\"") + tarName + "\"");
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "application/x-tar", "");
  WiFiClient client = webServer.client();

  File root = SPIFFS.open("/");
  if (root) {
    File f = root.openNextFile();
    uint8_t buf[256];
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    while (f && client.connected()) {
      String n = String(f.name());
      size_t sz = f.size();
      if (storageMaintNameIsAutoBackup(n)) {
        if (!n.startsWith("/")) n = String("/") + n;
        File in = SPIFFS.open(n, FILE_READ);
        if (in) {
          tarWriteHeader(client, n, (uint32_t)in.size());
          uint32_t sent = 0;
          while (in.available() && client.connected()) {
            int r = in.read(buf, sizeof(buf));
            if (r <= 0) break;
            client.write(buf, r);
            sent += (uint32_t)r;
            yield();
          }
          uint16_t pad = (uint16_t)((512UL - (sent % 512UL)) % 512UL);
          if (pad) client.write(zero, pad);
          in.close();
        }
      }
      f.close();
      f = root.openNextFile();
      yield();
    }
    root.close();
    uint8_t endBlocks[1024];
    memset(endBlocks, 0, sizeof(endBlocks));
    client.write(endBlocks, sizeof(endBlocks));
  }
}

File autoBackupRestoreUploadFile;
String autoBackupRestoreUploadMessage = "No upload";

void handleRestoreUploadedAutoBackupUpload() {
  HTTPUpload& up = webServer.upload();
  if (up.status == UPLOAD_FILE_START) {
    autoBackupRestoreUploadMessage = "Upload started";
    if (!storageFsBeginOk && !beginStorageFs()) { autoBackupRestoreUploadMessage = "FS mount failed"; return; }
    if (SPIFFS.exists(AUTO_BACKUP_UPLOAD_TMP)) SPIFFS.remove(AUTO_BACKUP_UPLOAD_TMP);
    autoBackupRestoreUploadFile = SPIFFS.open(AUTO_BACKUP_UPLOAD_TMP, FILE_WRITE);
    if (!autoBackupRestoreUploadFile) autoBackupRestoreUploadMessage = "Temp file open failed";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (autoBackupRestoreUploadFile) {
      size_t w = autoBackupRestoreUploadFile.write(up.buf, up.currentSize);
      if (w != up.currentSize) autoBackupRestoreUploadMessage = "Temp write failed";
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (autoBackupRestoreUploadFile) {
      autoBackupRestoreUploadFile.flush();
      autoBackupRestoreUploadFile.close();
      autoBackupRestoreUploadMessage = "Upload OK";
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (autoBackupRestoreUploadFile) autoBackupRestoreUploadFile.close();
    if (SPIFFS.exists(AUTO_BACKUP_UPLOAD_TMP)) SPIFFS.remove(AUTO_BACKUP_UPLOAD_TMP);
    autoBackupRestoreUploadMessage = "Upload aborted";
  }
}

void handleRestoreUploadedAutoBackupDone() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  String msg = autoBackupRestoreUploadMessage;
  bool ok = false;
  if (msg == "Upload OK" && SPIFFS.exists(AUTO_BACKUP_UPLOAD_TMP)) {
    ok = restoreAutoBackupFile(AUTO_BACKUP_UPLOAD_TMP, msg);
    SPIFFS.remove(AUTO_BACKUP_UPLOAD_TMP);
  } else if (msg.length() == 0) {
    msg = "No uploaded backup file";
  }
  String html = htmlHeader("Restore Uploaded Auto Backup");
  html += F("<div class='card'><h1>Restore Uploaded Auto Backup</h1><div class='sub'>");
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

// ========================= STORAGE MAINTENANCE =========================

String storageMaintSizeText(const char* path) {
  if (!path || !path[0]) return String("--");
  if (!storageFsBeginOk && !beginStorageFs()) return String("FS not mounted");
  if (!SPIFFS.exists(path)) return String("not found");
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return String("open failed");
  size_t sz = f.size();
  f.close();
  return String("found / ") + String((uint32_t)sz) + String(" bytes");
}

bool storageMaintRemovePath(const char* path) {
  if (!path || !path[0]) return false;
  if (!storageFsBeginOk && !beginStorageFs()) return false;
  if (!SPIFFS.exists(path)) return true;
  return SPIFFS.remove(path);
}

bool storageMaintNameIsAutoBackup(const String& name) {
  return name.startsWith(String(AUTO_BACKUP_PREFIX)) || name.startsWith(String("backup_"));
}

String storageMaintNormalizeFsName(String name) {
  name.trim();
  name.replace("\\", "");
  if (name.indexOf("..") >= 0) return String("");
  if (name.startsWith("/")) return name;
  return String("/") + name;
}

uint16_t storageMaintDeleteAutoBackups() {
  if (!storageFsBeginOk && !beginStorageFs()) return 0;
  uint16_t removed = 0;
  File root = SPIFFS.open("/");
  if (!root) return 0;
  File f = root.openNextFile();
  while (f) {
    String n = String(f.name());
    f.close();
    if (storageMaintNameIsAutoBackup(n)) {
      if (!n.startsWith("/")) n = String("/") + n;
      if (SPIFFS.remove(n)) removed++;
      yield();
    }
    f = root.openNextFile();
  }
  root.close();
  return removed;
}

String storageMaintBackupListHtml() {
  String html;
  if (!storageFsBeginOk && !beginStorageFs()) return String("<div class='sub bad'>SPIFFS not mounted</div>");
  File root = SPIFFS.open("/");
  if (!root) return String("<div class='sub bad'>Cannot open SPIFFS root</div>");
  byte shown = 0;
  File f = root.openNextFile();
  while (f) {
    String n = String(f.name());
    size_t sz = f.size();
    f.close();
    if (storageMaintNameIsAutoBackup(n)) {
      if (!n.startsWith("/")) n = String("/") + n;
      String label = n;
      if (label.startsWith("/")) label.remove(0, 1);
      html += F("<div class='row'><span class='k'>");
      html += htmlEscapeText(label);
      html += F("</span><span class='v'>");
      html += String((uint32_t)sz);
      html += F(" bytes<br><a href='");
      html += sessionUrl(String("/storagefile?file=") + n);
      html += F("'>Download</a></span></div>");
      shown++;
      yield();
    }
    f = root.openNextFile();
  }
  root.close();
  if (shown == 0) html += F("<div class='sub'>No auto backup files found.</div>");
  return html;
}

void storageMaintActionResult(const String& msg, bool ok) {
  webServer.sendHeader("Location", sessionUrl(String("/storage?msg=") + (ok ? "ok" : "err") + "&txt=" + msg), true);
  webServer.send(302, "text/plain", "");
}

void handleStorageMaintenanceAction() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  if (!storageFsBeginOk && !beginStorageFs()) { storageMaintActionResult("FS_mount_failed", false); return; }
  String action = webServer.arg("action");
  bool ok = true;
  String msg = "done";

  if (action == "del_eeprom_rescue") {
    bool a = storageMaintRemovePath(STORAGE_FS_BACKUP_PATH);
    bool b = storageMaintRemovePath(STORAGE_FS_META_PATH);
    storageFsLoadedAtBoot = false;
    storageFsLastSaveOk = false;
    setStorageFsError("eeprom rescue deleted by admin");
    ok = a && b;
    msg = ok ? "eeprom_rescue_deleted" : "eeprom_rescue_delete_failed";
  } else if (action == "del_user_store") {
    ok = storageMaintRemovePath(USER_PAGE_STORE_PATH);
    userPageStoreLoadedAtBoot = false;
    userPageStoreLastSaveOk = false;
    strncpy(userPageStoreLastError, "deleted by admin", sizeof(userPageStoreLastError) - 1);
    msg = ok ? "user_store_deleted" : "user_store_delete_failed";
  } else if (action == "del_act_store") {
    ok = storageMaintRemovePath(ACTIVATION_CODE_STORE_PATH);
    activationCodeStoreLoadedAtBoot = false;
    activationCodeStoreLastSaveOk = false;
    strncpy(activationCodeStoreLastError, "deleted by admin", sizeof(activationCodeStoreLastError) - 1);
    msg = ok ? "activation_codes_store_deleted" : "activation_codes_store_delete_failed";
  } else if (action == "del_act_log") {
    ok = storageMaintRemovePath(ACTIVATION_LOG_STORE_PATH);
    activationLogStoreLoadedAtBoot = false;
    activationLogStoreLastSaveOk = false;
    strncpy(activationLogStoreLastError, "deleted by admin", sizeof(activationLogStoreLastError) - 1);
    msg = ok ? "activation_log_store_deleted" : "activation_log_store_delete_failed";
  } else if (action == "del_backups") {
    uint16_t n = storageMaintDeleteAutoBackups();
    autoBackupLatestFile = "";
    autoBackupLastOk = false;
    autoBackupLastMessage = String("Auto backup files deleted: ") + String(n);
    saveAutoBackupConfig();
    msg = String("auto_backups_deleted_") + String(n);
  } else if (action == "del_backup_cfg") {
    ok = storageMaintRemovePath(AUTO_BACKUP_CFG_PATH);
    loadAutoBackupConfig();
    msg = ok ? "auto_backup_config_deleted" : "auto_backup_config_delete_failed";
  } else if (action == "format_spiffs") {
    ok = SPIFFS.format();
    storageFsBeginOk = false;
    beginStorageFs();
    userPageStoreLoadedAtBoot = false;
    activationCodeStoreLoadedAtBoot = false;
    activationLogStoreLoadedAtBoot = false;
    autoBackupLatestFile = "";
    autoBackupLastOk = false;
    msg = ok ? "spiffs_formatted" : "spiffs_format_failed";
  } else {
    ok = false;
    msg = "unknown_action";
  }
  storageMaintActionResult(msg, ok);
}

void handleDownloadStorageFile() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  if (!storageFsBeginOk && !beginStorageFs()) { webServer.send(500, "text/plain", "FS not mounted"); return; }
  String p = storageMaintNormalizeFsName(webServer.arg("file"));
  if (p.length() == 0 || !storageMaintNameIsAutoBackup(p)) { webServer.send(403, "text/plain", "Not allowed"); return; }
  if (!SPIFFS.exists(p)) { webServer.send(404, "text/plain", "File not found"); return; }
  File f = SPIFFS.open(p, FILE_READ);
  if (!f) { webServer.send(500, "text/plain", "Open failed"); return; }
  String label = p;
  if (label.startsWith("/")) label.remove(0, 1);
  webServer.sendHeader("Content-Disposition", String("attachment; filename=\"") + label + "\"");
  webServer.streamFile(f, "application/octet-stream");
  f.close();
}

void sendStorageMaintenancePage() {
  if (!requireRole(WEB_ROLE_ADMIN)) return;
  bool ar = (currentWebLanguage() == TG_LANG_AR);
  if (!storageFsBeginOk) beginStorageFs();
  String html = htmlHeader(ar ? String("صيانة التخزين") : String("Storage Maintenance"));
  html += F("<div class='card'><h1>"); html += ar ? F("صيانة التخزين") : F("Storage Maintenance"); html += F("</h1>");
  html += F("<div class='sub'>");
  html += ar ? F("صفحة إنقاذ وصيانة لملفات التخزين الداخلية. استخدمها بحذر ومن حساب الأدمن فقط.") : F("Admin-only rescue page for internal storage files. Use carefully.");
  html += F("</div>");
  if (webServer.hasArg("txt")) { html += F("<div class='msg "); html += (webServer.arg("msg") == "ok") ? F("ok") : F("bad"); html += F("'>"); html += htmlEscapeText(webServer.arg("txt")); html += F("</div>"); }
  html += F("<div class='row'><span class='k'>SPIFFS</span><span class='v'>"); html += storageFsBeginOk ? F("OK") : F("FAILED"); html += F("</span></div>");
#ifdef ESP32
  if (storageFsBeginOk) {
    html += F("<div class='row'><span class='k'>SPIFFS Used / Total</span><span class='v'>");
    html += String((uint32_t)SPIFFS.usedBytes()); html += F(" / "); html += String((uint32_t)SPIFFS.totalBytes()); html += F(" bytes</span></div>");
  }
#endif
  html += F("<div class='row'><span class='k'>EEPROM Rescue Image</span><span class='v'>"); html += storageMaintSizeText(STORAGE_FS_BACKUP_PATH); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>EEPROM Rescue Meta</span><span class='v'>"); html += storageMaintSizeText(STORAGE_FS_META_PATH); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>User Page Store</span><span class='v'>"); html += storageMaintSizeText(USER_PAGE_STORE_PATH); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Activation Codes Store</span><span class='v'>"); html += storageMaintSizeText(ACTIVATION_CODE_STORE_PATH); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Activation Log Store</span><span class='v'>"); html += storageMaintSizeText(ACTIVATION_LOG_STORE_PATH); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Auto Backup Config</span><span class='v'>"); html += storageMaintSizeText(AUTO_BACKUP_CFG_PATH); html += F("</span></div>");
  html += F("<div class='row'><span class='k'>Storage Health</span><span class='v'>"); html += htmlEscapeText(eepromStorageStatusText()); html += F("</span></div>");
  html += F("</div>");

  html += F("<div class='card'><h1>"); html += ar ? F("أزرار صيانة آمنة") : F("Safe Maintenance Actions"); html += F("</h1>");
  html += F("<div class='sub warn'>"); html += ar ? F("امسح ملف واحد فقط عند الحاجة، ثم أعد تشغيل الجهاز واختبره.") : F("Delete only the file you need, then restart and test."); html += F("</div>");
  const char* actions[][3] = {
    {"del_eeprom_rescue", "Delete EEPROM Rescue Image + Meta", "Delete EEPROM rescue image? Current EEPROM RAM settings are not erased."},
    {"del_user_store", "Delete User Page Store", "Delete user page rescue store? Users in current EEPROM may still remain."},
    {"del_act_store", "Delete Activation Codes Store", "Delete activation codes rescue store?"},
    {"del_act_log", "Delete Activation Log Store", "Delete activation log rescue store?"},
    {"del_backups", "Delete Auto Backup Files", "Delete all auto backup files saved in SPIFFS?"},
    {"del_backup_cfg", "Delete Auto Backup Config", "Reset auto backup options file?"}
  };
  for (byte i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
    html += F("<form method='POST' action='/storageaction' onsubmit=\"return confirm('"); html += actions[i][2]; html += F("');\">");
    html += sessionHiddenInput();
    html += F("<input type='hidden' name='action' value='"); html += actions[i][0]; html += F("'>");
    html += F("<button class='btn btn2' type='submit'>"); html += actions[i][1]; html += F("</button></form>");
  }
  html += F("</div>");

  html += F("<div class='card'><h1>"); html += ar ? F("النسخ الاحتياطية الموجودة") : F("Stored Auto Backups"); html += F("</h1>");
  html += storageMaintBackupListHtml();
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/downloadlatestbackup"); html += F("'>Download Latest Auto Backup</a>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/downloadallbackups"); html += F("'>Download All Backups TAR</a>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/backup"); html += F("'>Download Current EEPROM Backup</a>");
  html += F("<form method='POST' action='/restoreautobackup' enctype='multipart/form-data' onsubmit=\"return confirm('Restore uploaded auto backup and restart?');\">");
  html += sessionHiddenInput();
  html += F("<input type='file' name='autobackup' accept='.bin' required>");
  html += F("<button class='btn' type='submit'>Upload / Restore Auto Backup File</button></form>");
  html += F("</div>");

  html += F("<div class='card'><h1>"); html += ar ? F("إجراء خطر") : F("Danger Zone"); html += F("</h1>");
  html += F("<div class='sub bad'>"); html += ar ? F("Format SPIFFS يمسح ملفات الإنقاذ والنسخ الداخلية، ولا يمسح البرنامج نفسه.") : F("Format SPIFFS deletes internal rescue files and backups, not the firmware sketch."); html += F("</div>");
  html += F("<form method='POST' action='/storageaction' onsubmit=\"return confirm('FORMAT SPIFFS? This deletes internal backups and rescue stores. Continue?');\">");
  html += sessionHiddenInput();
  html += F("<input type='hidden' name='action' value='format_spiffs'><button class='btn' type='submit' style='background:#ef4444;color:#fff'>Format SPIFFS</button></form>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl(String(WEB_OTA_PATH)); html += F("'>Back to Update / Backup</a>");
  html += F("</div>");
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
  html += F("<div class='card'><h1>Firmware Update</h1><div class='sub'>Upload firmware .bin file only.</div>");
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
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/storage"); html += F("'>Storage Maintenance</a>");
  html += F("<form method='POST' action='/savebackupopts'>"); html += sessionHiddenInput();
  html += F("<label class='small'><input type='checkbox' name='abdaily' value='1'"); if (autoBackupDailyEnabled) html += F(" checked"); html += F("> Auto daily backup</label>");
  html += F("<label class='small'>Daily backup time</label><input type='time' name='abtime' value='"); html += autoBackupTimeText(); html += F("'>");
  html += F("<label class='small'>Keep last backups</label><input type='number' name='abkeep' min='1' max='14' value='"); html += String(autoBackupKeepCount); html += F("'>");
  html += F("<label class='small'><input type='checkbox' name='abonsave' value='1'"); if (autoBackupOnSaveEnabled) html += F(" checked"); html += F("> Backup after important Save events</label>");
  html += F("<label class='small'><input type='checkbox' name='abtg' value='1'"); if (autoBackupTelegramNotify) html += F(" checked"); html += F("> Telegram notify admin when backup is created</label>");
  html += F("<button class='btn' type='submit'>Save Backup Options</button></form>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/autobackupnow"); html += F("'>Create Backup Now</a>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/downloadlatestbackup"); html += F("'>Download Latest Auto Backup</a>");
  html += F("<a class='btn btn2' href='"); html += sessionUrl("/downloadallbackups"); html += F("'>Download All Backups TAR</a>");
  html += F("<form method='POST' action='/restorelatestbackup' onsubmit=\"return confirm('Restore latest auto backup and restart?');\">"); html += sessionHiddenInput();
  html += F("<button class='btn btn2' type='submit'>Restore Latest Auto Backup</button></form>");
  html += F("<hr><div class='sub'>Restore a full Auto Backup .bin downloaded from this device.</div>");
  html += F("<form method='POST' action='/restoreautobackup' enctype='multipart/form-data' onsubmit=\"return confirm('Restore uploaded auto backup and restart?');\">");
  html += sessionHiddenInput();
  html += F("<input type='file' name='autobackup' accept='.bin' required>");
  html += F("<button class='btn' type='submit'>Upload / Restore Auto Backup File</button></form>");
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
  html += F("<div class='card'><h1>WiFi Saved</h1><div class='sub'>Settings saved. Device will reconnect using strict Static IP when enabled, otherwise DHCP. AP/web remains active.</div>");
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
  webServer.on("/uisettings", HTTP_GET, sendUiSettingsPage);
  webServer.on("/ql1", HTTP_GET, sendCustomLink1Page);
  webServer.on("/ql2", HTTP_GET, sendCustomLink2Page);
  webServer.on("/ql3", HTTP_GET, sendCustomLink3Page);
  webServer.on("/prayertimes", HTTP_GET, sendPrayerTimesPage);
  webServer.on("/prayerrefresh", HTTP_GET, handlePrayerTimesRefresh);
  webServer.on("/testadhan", HTTP_GET, handleTestAdhanOutput);
  webServer.on("/outputauto", HTTP_GET, sendOutputAutoPage);
  webServer.on("/inputcards", HTTP_GET, sendInputCardsPage);
  webServer.on("/saveinputcards", HTTP_POST, handleSaveInputCards);
  webServer.on("/inputstatus", HTTP_GET, sendSmartInputStatusJson);
  webServer.on("/saveoutauto", HTTP_POST, handleSaveOutputAuto);
  webServer.on("/savesp32", HTTP_POST, handleSaveEsp32Settings);
  webServer.on("/savehomeui", HTTP_POST, handleSaveHomeUiSettings);
  webServer.on("/savelabels", HTTP_POST, handleSaveFieldLabels);
  webServer.on("/override", HTTP_POST, handleTimedOverride);
  webServer.on("/restart", HTTP_GET, sendRestartPage);
  webServer.on("/storage", HTTP_GET, sendStorageMaintenancePage);
  webServer.on("/storageaction", HTTP_POST, handleStorageMaintenanceAction);
  webServer.on("/storagefile", HTTP_GET, handleDownloadStorageFile);
  webServer.on("/backup", HTTP_GET, sendEepromBackup);
  webServer.on("/restore", HTTP_POST, handleRestoreBackupDone, handleRestoreBackupUpload);
  webServer.on("/autobackupnow", HTTP_GET, handleAutoBackupNow);
  webServer.on("/savebackupopts", HTTP_POST, handleSaveAutoBackupOptions);
  webServer.on("/downloadlatestbackup", HTTP_GET, handleDownloadLatestAutoBackup);
  webServer.on("/downloadallbackups", HTTP_GET, handleDownloadAllAutoBackups);
  webServer.on("/restorelatestbackup", HTTP_POST, handleRestoreLatestAutoBackup);
  webServer.on("/restoreautobackup", HTTP_POST, handleRestoreUploadedAutoBackupDone, handleRestoreUploadedAutoBackupUpload);
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
    unsigned long now = millis();
    bool isNewMax = (elapsed > persistentDiag.lastLongTaskMs);
    if (lastSlowTaskLogMs == 0 || isNewMax || now - lastSlowTaskLogMs >= LOOP_GAP_LOG_MIN_MS) {
      lastSlowTaskLogMs = now;
      addEsp32Log(String("Slow task: ") + name + " " + String(elapsed) + " ms");
      savePersistentDiagnostics(String("Slow task: ") + name + " " + String(elapsed) + " ms", isNewMax);
    }
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
  client.setTimeout(THINGSPEAK_CLIENT_TIMEOUT_MS);

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

unsigned long thingSpeakCommandSliceIntervalMs() {
  unsigned long slice = commandCheckIntervalMs / 4UL;
  if (slice < 5000UL) slice = 5000UL;
  return slice;
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

  const byte fields[4] = { FIELD1_OUTPUT1, FIELD2_OUTPUT2, FIELD5_OUTPUT5, FIELD6_OUTPUT6 };
  const byte outputs[4] = { 1, 2, 5, 6 };
  byte step = thingSpeakRemoteCommandStep;
  if (step >= 4) step = 0;

  byte field = fields[step];
  byte outputNumber = outputs[step];
  DBG_PRINT("Checking ThingSpeak command slice field ");
  DBG_PRINT(field);
  DBG_PRINT(" for output ");
  DBG_PRINTLN(outputNumber);

  long state = readThingSpeakField(field);
  DBG_PRINT("ThingSpeak field ");
  DBG_PRINT(field);
  DBG_PRINT(" read = ");
  DBG_PRINTLN(state);

  if (!isValidState(state)) {
    DBG_PRINT("Invalid or failed Output");
    DBG_PRINT(outputNumber);
    DBG_PRINTLN(" command. Keeping last state.");
  } else {
    setOutputState(outputNumber, (byte)state);
  }

  thingSpeakRemoteCommandStep = (byte)((step + 1) % 4);
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

  client.setTimeout(THINGSPEAK_CLIENT_TIMEOUT_MS);
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
  html += F("<div class='row'><span class='k'>TS Command Interval</span><span class='v'>"); html += String(commandCheckIntervalMs / 1000UL); html += F(" sec full cycle</span></div>");
  html += F("<div class='row'><span class='k'>TS Read Slice</span><span class='v'>1 field every "); html += String(thingSpeakCommandSliceIntervalMs() / 1000UL); html += F(" sec</span></div>");
  html += F("<div class='row'><span class='k'>Smart Input Cards</span><span class='v'>"); html += smartInputStorageStatusText(); html += F("</span></div>");
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
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  webServer.sendHeader("Pragma", "no-cache");
  webServer.sendHeader("Expires", "0");
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
  loadUserInputCardPermissions();
  loadOutputStates();
  loadPendingSyncFlag();
  loadWiFiManagerConfig();
  loadStaticIpConfig();
  loadThingSpeakConfig();
  loadWebPerformanceSettings();
  loadEsp32IntegrationConfig();
  loadTelegramUiConfig();
  loadAdhanOutputConfig();
  loadPrayerTimesCacheFromFs();
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
  loadSmartInputCardsConfig();
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
  pinMode(OUTPUT7_PIN, OUTPUT);
  pinMode(OUTPUT8_PIN, OUTPUT);
  pinMode(INPUT7_PIN, INPUT_PULLUP);
  pinMode(INPUT8_PIN, INPUT_PULLUP);
  pinMode(INPUT9_PIN, INPUT_PULLUP);
  pinMode(INPUT10_PIN, INPUT_PULLUP);
  applySmartInputPinModes();
  setupAdhanOutputPin();
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
  client.setTimeout(THINGSPEAK_CLIENT_TIMEOUT_MS);
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

  // Run the first ThingSpeak command slice shortly after boot, then spread the 4 fields across the configured cycle.
  lastCommandCheckTime = millis() - thingSpeakCommandSliceIntervalMs() + 3000UL;
  lastSensorUpdateTime = millis() - SENSOR_UPDATE_INTERVAL + 5000UL;

  DBG_PRINTLN("Setup complete. EEPROM debug is active.");
}

void loop() {
 

  unsigned long loopStartMs = millis();
  if (lastLoopTickMs != 0) {
    unsigned long gap = loopStartMs - lastLoopTickMs;
    if (gap > maxLoopGapMs) maxLoopGapMs = gap;
    if (gap >= LOOP_GAP_WARN_MS) {
      unsigned long nowGapLog = millis();
      bool isNewMaxGap = (gap > persistentDiag.maxLoopGapMs);
      if (lastLoopGapLogMs == 0 || isNewMaxGap || nowGapLog - lastLoopGapLogMs >= LOOP_GAP_LOG_MIN_MS) {
        lastLoopGapLogMs = nowGapLog;
        addEsp32Log(String("Loop gap warning: ") + String(gap) + " ms");
        savePersistentDiagnostics(String("Loop gap warning ") + String(gap) + " ms", isNewMaxGap);
      }
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

  t = millis();
  maintainStartupTelegramReport();
  noteTaskDuration("Startup Telegram report", t);
  serviceWebCore();

  processPendingWsBroadcast();
  handleManualOverrideExpiry();
  maintainManualAutoOff9();
  maintainManualAutoOffDuration();
  maintainOutputAutoControl();
  maintainDigitalInputs();
  maintainSmartInputCards();
  maintainAdhanOutputAutomation();
  handleAutoRestart24h();

  unsigned long now = millis();

  if (now - lastCommandCheckTime >= thingSpeakCommandSliceIntervalMs()) {
    lastCommandCheckTime = now;
    t = millis();
    checkRemoteCommands();
    noteTaskDuration("ThingSpeak read slice", t);
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



