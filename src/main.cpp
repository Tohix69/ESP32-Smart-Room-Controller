#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <time.h>
#include <Update.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "secrets.h"
#include "project_config.h"

// ==========================================
// LUMINO SMART ROOM CONTROLLER
// VERSION 1.6-B - OLED BOOT + DEVICE SPOTLIGHT + AUTO SLEEP
// Based on the user's working V1.3 Recovery Build
// ==========================================

// OLED
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_SDA 21
#define OLED_SCL 22

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool oledReady = false;

// STORAGE
Preferences preferences;
bool storageReady = false;

// RELAY PINS
const uint8_t relayPins[4] = {16, 17, 18, 19};

// TOUCH PINS
const uint8_t touchPins[4] = {32, 33, 25, 26};

const char* deviceNames[4] = {
    "Light 1",
    "Light 2",
    "Light 3",
    "Fan"
};

// RELAY LOGIC
const uint8_t RELAY_ON = LOW;
const uint8_t RELAY_OFF = HIGH;

// DEVICE STATES
bool deviceStates[4] = {false, false, false, false};

// TOUCH DETECTION
bool rawStates[4] = {false, false, false, false};
bool stableStates[4] = {false, false, false, false};
unsigned long lastChangeTime[4] = {0, 0, 0, 0};
const unsigned long TOUCH_DEBOUNCE = 50;

// WIFI MANAGEMENT
bool fallbackAPActive = false;
unsigned long wifiStartTime = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastDisplayUpdate = 0;
String wifiStatus = "Starting";

// Wi-Fi credentials currently being used
String activeSSID;
String activePassword;
String savedSSID;
String savedPassword;

// Wi-Fi setup transaction: new credentials are only saved after connection succeeds
bool wifiTestInProgress = false;
unsigned long wifiTestStarted = 0;
const unsigned long WIFI_TEST_TIMEOUT = 25000;
String pendingSSID;
String pendingPassword;
String previousSSID;
String previousPassword;

// Captive portal DNS for the fallback access point
DNSServer dnsServer;
const uint16_t DNS_PORT = 53;
bool dnsServerStarted = false;

// HTTP + WEBSOCKET SERVERS
WebServer server(80);
WebSocketsServer webSocket(81);
bool webServerStarted = false;
bool webSocketStarted = false;
String lastBroadcastWifiStatus;

// Wi-Fi scan lifecycle (prevents treating an unstarted scan as an empty result)
bool wifiScanActive = false;

// DAILY FAN SCHEDULE (up to 8 events)
struct FanScheduleEvent {
    uint16_t minuteOfDay; // 0..1439
    bool turnOn;
};
const uint8_t MAX_FAN_EVENTS = 8;
FanScheduleEvent fanEvents[MAX_FAN_EVENTS];
uint8_t fanEventCount = 0;
bool fanScheduleEnabled = true;
bool ntpConfigured = false;
bool clockIsValid = false;
int lastScheduleDate = -1;
int lastScheduleMinute = -1;
unsigned long lastClockCheck = 0;
unsigned long lastNtpAttempt = 0;
const unsigned long NTP_RETRY_INTERVAL = 30000; // Retry every 30 seconds until time is valid

bool getLocalTimeInfo(struct tm &timeInfo);
unsigned long oledAnimationStarted = 0;
uint8_t lastAnimatedDevice = 255;
unsigned long deviceAnimationStarted = 0;
unsigned long lastOledActivity = 0;
bool oledSleeping = false;
const unsigned long OLED_EVENT_SCREEN_MS = 5000;
const unsigned long OLED_IDLE_SLEEP_MS = 15000;
const unsigned long OLED_BOOT_ANIMATION_MS = 2200;

void wakeOLED();

// ==========================================
// RELAY INITIALIZATION
// ==========================================

void initializeRelays() {
    for (int i = 0; i < 4; i++) {
        digitalWrite(relayPins[i], RELAY_OFF);
        pinMode(relayPins[i], OUTPUT);
        deviceStates[i] = false;
    }
    Serial.println("Relays initialized.");
}

// ==========================================
// STORAGE
// ==========================================

void initializeStorage() {
    storageReady = preferences.begin("lumino", false);
    Serial.println(storageReady ? "Storage ready." : "Storage failed!");
}

void restoreDeviceStates() {
    if (!storageReady) return;

    for (int i = 0; i < 4; i++) {
        String key = "device" + String(i);
        deviceStates[i] = preferences.getUChar(key.c_str(), 0) == 1;

        digitalWrite(
            relayPins[i],
            deviceStates[i] ? RELAY_ON : RELAY_OFF
        );

        Serial.printf(
            "%s: %s\n",
            deviceNames[i],
            deviceStates[i] ? "ON" : "OFF"
        );
    }
}

void saveDeviceState(uint8_t index) {
    if (!storageReady || index >= 4) return;

    String key = "device" + String(index);
    preferences.putUChar(
        key.c_str(),
        deviceStates[index] ? 1 : 0
    );
}

// ==========================================
// DEVICE CONTROL
// ==========================================

void broadcastState();

void setDeviceState(uint8_t index, bool state) {
    if (index >= 4) return;
    if (deviceStates[index] == state) return;

    deviceStates[index] = state;

    digitalWrite(
        relayPins[index],
        state ? RELAY_ON : RELAY_OFF
    );

    saveDeviceState(index);
    lastAnimatedDevice = index;
    deviceAnimationStarted = millis();
    lastOledActivity = millis();
    wakeOLED();
    broadcastState();

    Serial.printf(
        "%s changed to %s\n",
        deviceNames[index],
        state ? "ON" : "OFF"
    );
}

void toggleDevice(uint8_t index) {
    if (index >= 4) return;
    setDeviceState(index, !deviceStates[index]);
}

// ==========================================
// OLED INITIALIZATION
// ==========================================

void initializeOLED() {
    Wire.begin(OLED_SDA, OLED_SCL);

    oledReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

    if (oledReady) {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        oledAnimationStarted = millis();
        lastOledActivity = millis();
        Serial.println("OLED initialized.");
    } else {
        Serial.println("OLED initialization failed.");
    }
}

// ==========================================
// TOUCH INITIALIZATION
// ==========================================

void initializeTouchSensors() {
    for (int i = 0; i < 4; i++) {
        pinMode(touchPins[i], INPUT_PULLDOWN);
        rawStates[i] = digitalRead(touchPins[i]);
        stableStates[i] = rawStates[i];
        lastChangeTime[i] = millis();
    }
    Serial.println("Touch sensors initialized.");
}

// ==========================================
// TOUCH MONITORING
// ==========================================

void readTouchSensors() {
    for (int i = 0; i < 4; i++) {
        bool currentRaw = digitalRead(touchPins[i]);

        if (currentRaw != rawStates[i]) {
            rawStates[i] = currentRaw;
            lastChangeTime[i] = millis();
        }

        if (millis() - lastChangeTime[i] >= TOUCH_DEBOUNCE) {
            if (stableStates[i] != rawStates[i]) {
                stableStates[i] = rawStates[i];

                if (stableStates[i]) {
                    toggleDevice(i);
                }
            }
        }
    }
}

// ==========================================
// WIFI CREDENTIAL STORAGE AND IP CONFIGURATION
// ==========================================

void loadWiFiCredentials() {
    if (storageReady) {
        savedSSID = preferences.getString("wifi_ssid", "");
        savedPassword = preferences.getString("wifi_pass", "");
    }

    if (savedSSID.length() > 0) {
        activeSSID = savedSSID;
        activePassword = savedPassword;
        Serial.println("Using saved WiFi credentials.");
    } else {
        activeSSID = WIFI_SSID;
        activePassword = WIFI_PASSWORD;
        Serial.println("Using WiFi credentials from secrets.h.");
    }
}

void saveWiFiCredentials(const String& ssid, const String& password) {
    if (!storageReady) {
        Serial.println("Cannot save WiFi credentials: Preferences unavailable.");
        return;
    }

    preferences.putString("wifi_ssid", ssid);
    preferences.putString("wifi_pass", password);
    savedSSID = ssid;
    savedPassword = password;
}

void configureIPForNetwork(const String& ssid) {
    // Keep the known static IP on the original configured network.
    // Use DHCP on other networks so the controller can move to a new router.
    if (ssid == String(WIFI_SSID) && WIFI_USE_STATIC_IP) {
        IPAddress localIP(LOCAL_IP);
        IPAddress gateway(GATEWAY_IP);
        IPAddress subnet(SUBNET_MASK);
        IPAddress dns1(PRIMARY_DNS);
        IPAddress dns2(SECONDARY_DNS);

        if (!WiFi.config(localIP, gateway, subnet, dns1, dns2)) {
            Serial.println("Static IP configuration failed!");
        } else {
            Serial.println("Original network static IP configured.");
        }
    } else {
        WiFi.config(
            IPAddress(0, 0, 0, 0),
            IPAddress(0, 0, 0, 0),
            IPAddress(0, 0, 0, 0)
        );
        Serial.println("Using DHCP for this WiFi network.");
    }
}

// ==========================================
// START WIFI
// ==========================================

void initializeWiFi() {
    Serial.println("Starting WiFi...");

    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);

    loadWiFiCredentials();
    configureIPForNetwork(activeSSID);
    WiFi.begin(activeSSID.c_str(), activePassword.c_str());

    wifiStartTime = millis();
    lastWifiAttempt = millis();
    wifiStatus = "Connecting";

    Serial.print("Connecting to: ");
    Serial.println(activeSSID);
}

// ==========================================
// START FALLBACK ACCESS POINT + CAPTIVE DNS
// ==========================================

void startFallbackAP() {
    if (fallbackAPActive) return;

    Serial.println("Starting fallback Access Point...");
    WiFi.mode(WIFI_AP_STA);

    if (WiFi.softAP(AP_SSID, AP_PASSWORD)) {
        fallbackAPActive = true;

        IPAddress apIP = WiFi.softAPIP();
        dnsServer.start(DNS_PORT, "*", apIP);
        dnsServerStarted = true;

        Serial.println("Fallback AP started.");
        Serial.print("AP Name: ");
        Serial.println(AP_SSID);
        Serial.print("AP IP: ");
        Serial.println(apIP);
        Serial.println("Open http://192.168.4.1 to configure WiFi.");

        wifiStatus = "AP Setup Mode";
    } else {
        Serial.println("AP startup failed!");
    }
}

// ==========================================
// STOP FALLBACK ACCESS POINT
// ==========================================

void stopFallbackAP() {
    if (!fallbackAPActive) return;

    if (dnsServerStarted) {
        dnsServer.stop();
        dnsServerStarted = false;
    }

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    fallbackAPActive = false;

    Serial.println("Fallback AP stopped.");
}

// ==========================================
// WIFI CONNECTION MANAGEMENT
// ==========================================

void manageWiFi() {
    unsigned long now = millis();

    // During a new-credential test, keep the setup AP available.
    if (wifiTestInProgress) {
        if (WiFi.status() == WL_CONNECTED) {
            activeSSID = pendingSSID;
            activePassword = pendingPassword;
            saveWiFiCredentials(activeSSID, activePassword);

            wifiTestInProgress = false;
            wifiStatus = "Connected";

            Serial.println();
            Serial.println("New WiFi credentials verified and saved.");
            Serial.print("SSID: ");
            Serial.println(WiFi.SSID());
            Serial.print("IP: ");
            Serial.println(WiFi.localIP());

            stopFallbackAP();
            return;
        }

        if (now - wifiTestStarted >= WIFI_TEST_TIMEOUT) {
            Serial.println("New WiFi connection failed. Keeping previous credentials.");

            wifiTestInProgress = false;
            activeSSID = previousSSID;
            activePassword = previousPassword;

            WiFi.disconnect(false);
            if (activeSSID.length() > 0) {
                configureIPForNetwork(activeSSID);
                WiFi.begin(activeSSID.c_str(), activePassword.c_str());
            }

            wifiStatus = "AP Setup Mode";
        }
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (wifiStatus != "Connected") {
            wifiStatus = "Connected";

            Serial.println();
            Serial.println("WiFi connected!");
            Serial.print("SSID: ");
            Serial.println(WiFi.SSID());
            Serial.print("IP: ");
            Serial.println(WiFi.localIP());
            Serial.print("Signal: ");
            Serial.print(WiFi.RSSI());
            Serial.println(" dBm");

            if (savedSSID.length() == 0 && activeSSID.length() > 0) {
                saveWiFiCredentials(activeSSID, activePassword);
            }

            stopFallbackAP();
        }
        return;
    }

    wifiStatus = fallbackAPActive ? "AP Setup Mode" : "Disconnected";

    if (!fallbackAPActive &&
        now - wifiStartTime >= AP_FALLBACK_DELAY) {
        startFallbackAP();
    }

    // Do not restart the saved-SSID connection while an AP-mode scan is running.
    // Repeated disconnect()/begin() calls can cancel the asynchronous scan.
    if (wifiScanActive) {
        return;
    }

    if (now - lastWifiAttempt >= WIFI_RETRY_INTERVAL) {
        lastWifiAttempt = now;

        Serial.println("Retrying configured WiFi...");
        WiFi.disconnect(false);
        configureIPForNetwork(activeSSID);
        WiFi.begin(activeSSID.c_str(), activePassword.c_str());
    }
}

// ==========================================
// OLED DISPLAY
// ==========================================

// Small monochrome device glyphs, designed for a 128x64 SSD1306.
void drawDeviceIcon(uint8_t index, int16_t x, int16_t y, bool active) {
    const uint16_t color = SSD1306_WHITE;
    if (index == 0 || index == 1 || index == 2) {
        // Compact light-bulb icon.
        display.drawCircle(x + 4, y + 4, 3, color);
        display.drawLine(x + 2, y + 8, x + 6, y + 8, color);
        display.drawLine(x + 2, y + 10, x + 6, y + 10, color);
        if (active) display.fillCircle(x + 4, y + 4, 1, color);
    } else {
        // Fan icon: hub and four short blades.
        display.drawCircle(x + 4, y + 5, 1, color);
        if (active) {
            display.fillCircle(x + 4, y + 5, 1, color);
            display.drawLine(x + 4, y + 4, x + 4, y, color);
            display.drawLine(x + 5, y + 5, x + 9, y + 5, color);
            display.drawLine(x + 4, y + 6, x + 4, y + 10, color);
            display.drawLine(x + 3, y + 5, x, y + 5, color);
        } else {
            display.drawLine(x + 4, y + 4, x + 4, y + 1, color);
            display.drawLine(x + 5, y + 5, x + 8, y + 5, color);
            display.drawLine(x + 4, y + 6, x + 4, y + 9, color);
            display.drawLine(x + 3, y + 5, x + 1, y + 5, color);
        }
    }
}

void drawWifiIndicator(int16_t x, int16_t y) {
    bool connected = WiFi.status() == WL_CONNECTED;
    display.drawCircle(x + 5, y + 5, 5, SSD1306_WHITE);
    if (connected) {
        display.fillCircle(x + 5, y + 5, 2, SSD1306_WHITE);
    } else {
        display.drawLine(x + 1, y + 9, x + 9, y + 1, SSD1306_WHITE);
    }
}

void wakeOLED() {
    if (!oledReady) return;
    if (oledSleeping) {
        display.ssd1306_command(SSD1306_DISPLAYON);
        oledSleeping = false;
    }
}

void drawBootAnimation() {
    unsigned long age = millis() - oledAnimationStarted;
    if (age >= OLED_BOOT_ANIMATION_MS) return;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawRoundRect(18, 8, 92, 48, 8, SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(31, 17);
    display.print("LUMINO");
    display.setTextSize(1);
    display.setCursor(38, 38);
    display.print("SMART ROOM");
    int16_t barWidth = (int16_t)((age * 78UL) / OLED_BOOT_ANIMATION_MS);
    display.drawRoundRect(25, 58, 78, 4, 2, SSD1306_WHITE);
    if (barWidth > 0) display.fillRect(26, 59, barWidth, 2, SSD1306_WHITE);
    display.display();
}

void drawDeviceSpotlight() {
    uint8_t i = lastAnimatedDevice;
    bool on = deviceStates[i];
    unsigned long age = millis() - deviceAnimationStarted;
    bool blink = ((age / 350) % 2) == 0;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.drawRoundRect(3, 3, 122, 58, 8, SSD1306_WHITE);

    // Large, centered device icon.
    int16_t cx = 64;
    if (i < 3) {
        display.drawCircle(cx, 19, 8, SSD1306_WHITE);
        display.drawLine(cx - 5, 27, cx + 5, 27, SSD1306_WHITE);
        display.drawLine(cx - 4, 30, cx + 4, 30, SSD1306_WHITE);
        if (on) display.fillCircle(cx, 19, blink ? 4 : 2, SSD1306_WHITE);
    } else {
        display.drawCircle(cx, 20, 3, SSD1306_WHITE);
        display.drawLine(cx, 17, cx, 7, SSD1306_WHITE);
        display.drawLine(cx + 3, 20, cx + 13, 20, SSD1306_WHITE);
        display.drawLine(cx, 23, cx, 33, SSD1306_WHITE);
        display.drawLine(cx - 3, 20, cx - 13, 20, SSD1306_WHITE);
        if (on && blink) display.fillCircle(cx, 20, 3, SSD1306_WHITE);
    }

    const char* names[4] = {"LIGHT 1", "LIGHT 2", "LIGHT 3", "FAN"};
    display.setTextSize(1);
    int16_t nameWidth = (int16_t)strlen(names[i]) * 6;
    display.setCursor((128 - nameWidth) / 2, 36);
    display.print(names[i]);
    display.setTextSize(2);
    const char* state = on ? "ON" : "OFF";
    int16_t stateWidth = (int16_t)strlen(state) * 12;
    display.setCursor((128 - stateWidth) / 2, 45);
    display.print(state);
    display.display();
}

void updateOLED() {
    if (!oledReady) return;

    unsigned long nowMs = millis();
    if (nowMs - lastDisplayUpdate < 100) return;
    lastDisplayUpdate = nowMs;

    // Startup animation is shown once after boot.
    if (nowMs - oledAnimationStarted < OLED_BOOT_ANIMATION_MS) {
        wakeOLED();
        drawBootAnimation();
        return;
    }

    // Show the most recently changed device full-screen for five seconds.
    if (lastAnimatedDevice < 4 && nowMs - deviceAnimationStarted < OLED_EVENT_SCREEN_MS) {
        wakeOLED();
        drawDeviceSpotlight();
        return;
    }

    // Return to the dashboard, then switch the OLED off after 15 seconds
    // without a local touch or a device-state change from the web/schedule.
    if (!oledSleeping && nowMs - lastOledActivity >= OLED_IDLE_SLEEP_MS) {
        display.clearDisplay();
        display.display();
        display.ssd1306_command(SSD1306_DISPLAYOFF);
        oledSleeping = true;
        return;
    }
    if (oledSleeping) return;

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    struct tm now;
    if (getLocalTimeInfo(now)) {
        char clockText[6];
        snprintf(clockText, sizeof(clockText), "%02d:%02d", now.tm_hour, now.tm_min);
        display.setTextSize(2);
        display.setCursor(0, 0);
        display.print(clockText);
        display.setTextSize(1);
        display.setCursor(0, 19);
        const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        display.printf("%s %02d %s", days[now.tm_wday], now.tm_mday, months[now.tm_mon]);
    } else {
        display.setTextSize(2);
        display.setCursor(0, 0);
        display.print("--:--");
        display.setTextSize(1);
        display.setCursor(0, 19);
        display.print("SYNCING CLOCK");
    }

    drawWifiIndicator(115, 2);
    display.drawLine(0, 29, 127, 29, SSD1306_WHITE);

    // Short labels keep all four devices readable on the 128x64 screen.
    const char* shortNames[4] = {"L1", "L2", "L3", "FAN"};
    for (uint8_t i = 0; i < 4; i++) {
        int16_t tileX = (i % 2) * 64;
        int16_t tileY = 32 + (i / 2) * 16;
        bool active = deviceStates[i];
        display.drawRoundRect(tileX, tileY, 62, 14, 3, SSD1306_WHITE);
        drawDeviceIcon(i, tileX + 3, tileY + 1, active);
        display.setTextSize(1);
        display.setCursor(tileX + 16, tileY + 3);
        display.print(shortNames[i]);
        display.setCursor(tileX + 43, tileY + 3);
        display.print(active ? "ON" : "OFF");
    }
    display.display();
}

// ==========================================
// WEB DASHBOARD HTML
// ==========================================

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#101827">
<title>Lumino Smart Room</title>
<style>
:root{color-scheme:dark;--bg:#0b1220;--panel:#151f30;--panel2:#1b2940;--text:#eef4ff;--muted:#91a0b8;--accent:#6ee7c0;--line:#29384f}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;background:radial-gradient(circle at top,#1a2b43 0,#0b1220 48%);font-family:Arial,Helvetica,sans-serif;color:var(--text);padding:24px 16px 40px}
main{width:100%;max-width:760px;margin:0 auto}
header{display:flex;justify-content:space-between;align-items:center;gap:16px;margin:8px 0 26px}
.brand{display:flex;align-items:center;gap:12px}
.logo{width:44px;height:44px;border-radius:14px;background:linear-gradient(135deg,#6ee7c0,#4ca7e8);display:grid;place-items:center;color:#09201c;font-size:23px;font-weight:800}
h1{font-size:21px;margin:0 0 4px;letter-spacing:.2px}
.subtitle{font-size:12px;color:var(--muted)}
.connection{width:12px;height:12px;min-width:12px;border-radius:50%;background:#e05252;border:2px solid rgba(255,255,255,.18);box-shadow:0 0 0 3px rgba(224,82,82,.14);display:inline-block;vertical-align:middle;font-size:0;line-height:0;white-space:nowrap;transition:background .2s,box-shadow .2s}.connection.online{background:#32c878;box-shadow:0 0 0 3px rgba(50,200,120,.16)}
h2{font-size:14px;color:#cbd6e8;font-weight:600;margin:0 0 14px}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}
.card{background:linear-gradient(145deg,var(--panel2),var(--panel));border:1px solid var(--line);border-radius:20px;padding:19px;min-height:168px;transition:border-color .2s,background .2s,transform .12s;cursor:pointer;user-select:none;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
.card:active{transform:scale(.985)}
.card:focus-visible{outline:2px solid var(--accent);outline-offset:3px}
.card.on{border-color:#3c8f7e;background:linear-gradient(145deg,#1b3b3a,#172a38)}
.card-top{display:flex;justify-content:space-between;align-items:flex-start}
.device-icon{width:43px;height:43px;border-radius:14px;background:#263750;display:grid;place-items:center;font-size:22px}
.card.on .device-icon{background:#24564d}
.state{font-size:10px;color:var(--muted);letter-spacing:1px;font-weight:700}
.card.on .state{color:var(--accent)}
.device-name{font-size:16px;font-weight:700;margin:18px 0 13px}
.card-bottom{display:flex;justify-content:space-between;align-items:center}
.state-text{font-size:12px;color:var(--muted)}
.switch{border:0;width:53px;height:30px;border-radius:20px;background:#40506a;padding:3px;cursor:default;pointer-events:none;transition:background .2s}
.switch span{display:block;width:24px;height:24px;background:#fff;border-radius:50%;transition:transform .2s}
.card.on .switch{background:#39b99a}
.card.on .switch span{transform:translateX(23px)}
.switch:disabled{opacity:.55;cursor:wait}
footer{margin-top:22px;padding:15px 17px;border:1px solid var(--line);border-radius:15px;background:#111c2b;color:var(--muted);font-size:12px;display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap}
#message{min-height:16px;margin-top:14px;text-align:center;font-size:12px;color:var(--muted)}
@media(max-width:420px){body{padding:18px 12px 28px}.grid{gap:10px}.card{padding:15px;min-height:158px}.device-name{font-size:15px}}
</style>
</head>
<body>
<main>
<header>
 <div class="brand">
  <div class="logo">L</div>
  <div><h1>Lumino Smart Room</h1><div class="subtitle">Your room, connected.</div></div>
 </div>
 <div style="display:flex;align-items:center;gap:8px">
  <a href="/firmware" aria-label="Firmware update" title="Firmware update" style="color:var(--accent);text-decoration:none;border:1px solid var(--line);height:36px;padding:0 10px;border-radius:12px;display:flex;align-items:center;font-size:12px">Update</a>
  <a href="/schedule" aria-label="Fan schedule" title="Fan schedule" style="color:var(--accent);text-decoration:none;border:1px solid var(--line);height:36px;padding:0 10px;border-radius:12px;display:flex;align-items:center;font-size:12px">Schedule</a>
  <a href="/wifi" aria-label="Wi-Fi settings" title="Wi-Fi settings" style="color:var(--accent);text-decoration:none;border:1px solid var(--line);width:36px;height:36px;border-radius:12px;display:grid;place-items:center;font-size:21px;line-height:1">&#9881;</a>
  <span class="connection" id="connection" role="status" aria-label="Connecting" title="Connecting"></span>
 </div>
</header>
<h2>DEVICE CONTROLS</h2>
<section class="grid">
 <article class="card" id="card0" role="button" tabindex="0" aria-label="Toggle Light 1" aria-pressed="false" onclick="toggleDevice(0)" onkeydown="if(event.key==='Enter'||event.key===' '){event.preventDefault();toggleDevice(0);}"><div class="card-top"><div class="device-icon">💡</div><div class="state" id="state0">OFF</div></div><div class="device-name">Light 1</div><div class="card-bottom"><div class="state-text" id="text0">Switched off</div><button class="switch" id="button0" aria-hidden="true" tabindex="-1"><span></span></button></div></article>
 <article class="card" id="card1" role="button" tabindex="0" aria-label="Toggle Light 2" aria-pressed="false" onclick="toggleDevice(1)" onkeydown="if(event.key==='Enter'||event.key===' '){event.preventDefault();toggleDevice(1);}"><div class="card-top"><div class="device-icon">💡</div><div class="state" id="state1">OFF</div></div><div class="device-name">Light 2</div><div class="card-bottom"><div class="state-text" id="text1">Switched off</div><button class="switch" id="button1" aria-hidden="true" tabindex="-1"><span></span></button></div></article>
 <article class="card" id="card2" role="button" tabindex="0" aria-label="Toggle Light 3" aria-pressed="false" onclick="toggleDevice(2)" onkeydown="if(event.key==='Enter'||event.key===' '){event.preventDefault();toggleDevice(2);}"><div class="card-top"><div class="device-icon">💡</div><div class="state" id="state2">OFF</div></div><div class="device-name">Light 3</div><div class="card-bottom"><div class="state-text" id="text2">Switched off</div><button class="switch" id="button2" aria-hidden="true" tabindex="-1"><span></span></button></div></article>
 <article class="card" id="card3" role="button" tabindex="0" aria-label="Toggle Fan" aria-pressed="false" onclick="toggleDevice(3)" onkeydown="if(event.key==='Enter'||event.key===' '){event.preventDefault();toggleDevice(3);}"><div class="card-top"><div class="device-icon">🌀</div><div class="state" id="state3">OFF</div></div><div class="device-name">Fan</div><div class="card-bottom"><div class="state-text" id="text3">Switched off</div><button class="switch" id="button3" aria-hidden="true" tabindex="-1"><span></span></button></div></article>
</section>
<div id="message"></div>
<footer><span id="wifi">Wi-Fi: checking…</span><span id="ip">Controller: checking…</span></footer>
</main>
<script>
let requestBusy=false;
function setConnectionStatus(online,label){
 const indicator=document.getElementById('connection');
 indicator.classList.toggle('online',!!online);
 indicator.setAttribute('aria-label',label);
 indicator.title=label;
}
function render(data){
 if(!data || !Array.isArray(data.devices)) return;
 data.devices.forEach((device,i)=>{
  const on=!!device.on;
  document.getElementById('card'+i).classList.toggle('on',on);
  document.getElementById('card'+i).setAttribute('aria-pressed',String(on));
  document.getElementById('state'+i).textContent=on?'ON':'OFF';
  document.getElementById('text'+i).textContent=on?'Switched on':'Switched off';
  document.getElementById('button'+i).setAttribute('aria-pressed',String(on));
 });
 document.getElementById('wifi').textContent='Wi-Fi: '+data.wifi;
 document.getElementById('ip').textContent='Controller: '+data.ip;
 setConnectionStatus(true, 'Online');
}
async function refreshState(){
 try{
  const response=await fetch('/api/state',{cache:'no-store'});
  if(!response.ok) throw new Error('HTTP '+response.status);
  render(await response.json());
 }catch(e){
  setConnectionStatus(false, 'Offline');
 }
}
let socket=null;
let reconnectTimer=null;
function connectWebSocket(){
 if(socket && (socket.readyState===WebSocket.OPEN || socket.readyState===WebSocket.CONNECTING)) return;
 const scheme=location.protocol==='https:'?'wss':'ws';
 socket=new WebSocket(scheme+'://'+location.hostname+':81/');
 socket.onopen=()=>{
  setConnectionStatus(true, 'Online');
  if(reconnectTimer){clearTimeout(reconnectTimer);reconnectTimer=null;}
 };
 socket.onmessage=(event)=>{
  try{render(JSON.parse(event.data));}catch(e){}
 };
 socket.onclose=()=>{
  setConnectionStatus(false, 'Offline');
  if(!reconnectTimer) reconnectTimer=setTimeout(()=>{reconnectTimer=null;connectWebSocket();},2000);
 };
 socket.onerror=()=>{try{socket.close();}catch(e){}};
}
async function toggleDevice(index){
 if(requestBusy) return;
 requestBusy=true;
 const card=document.getElementById('card'+index);
 card.setAttribute('aria-busy','true');
 try{
  const response=await fetch('/api/toggle?index='+index,{method:'POST',cache:'no-store'});
  if(!response.ok) throw new Error('Request failed');
  render(await response.json());
  document.getElementById('message').textContent='Device updated';
 }catch(e){
  document.getElementById('message').textContent='Could not reach the controller. Please try again.';
  await refreshState();
 }finally{
  card.removeAttribute('aria-busy');
  requestBusy=false;
  setTimeout(()=>{document.getElementById('message').textContent='';},2200);
 }
}
refreshState();
connectWebSocket();
</script>
</body>
</html>
)rawliteral";

// ==========================================
// WIFI SETUP PORTAL HTML
// ==========================================

const char WIFI_SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#101827">
<title>Lumino Wi-Fi Setup</title>
<style>
:root{color-scheme:dark;--panel:#172438;--text:#eef4ff;--muted:#9aabc2;--accent:#6ee7c0;--line:#30425b}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;padding:24px 15px;background:radial-gradient(circle at top,#1a2b43,#0b1220 55%);font-family:Arial,sans-serif;color:var(--text)}
main{max-width:480px;margin:25px auto}
header{display:flex;align-items:center;gap:12px;margin-bottom:22px}
.logo{width:44px;height:44px;border-radius:14px;background:linear-gradient(135deg,#6ee7c0,#4ca7e8);display:grid;place-items:center;color:#09201c;font-size:23px;font-weight:800}
h1{font-size:21px;margin:0 0 4px}
.sub{font-size:12px;color:var(--muted)}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:18px;padding:20px}
h2{font-size:16px;margin:0 0 8px}
.help{font-size:12px;line-height:1.5;color:var(--muted);margin:0 0 18px}
label{display:block;font-size:12px;color:#cbd6e8;margin:15px 0 7px}
input,select{width:100%;padding:13px;border-radius:10px;border:1px solid var(--line);background:#0d1828;color:var(--text);font-size:14px;outline:none}
input:focus,select:focus{border-color:var(--accent)}
button{display:block;width:100%;border:0;border-radius:11px;padding:13px;background:#32aa8c;color:#06251d;font-size:14px;font-weight:700;text-align:center;cursor:pointer;margin-top:14px}
.secondary{background:#263a55;color:var(--text)}
.status{font-size:12px;color:var(--accent);min-height:18px;margin-top:14px;line-height:1.5}
.note{font-size:11px;color:var(--muted);line-height:1.5;margin-top:17px}
a.back{display:block;color:var(--accent);text-decoration:none;font-size:12px;margin-top:18px;text-align:center}
</style>
</head>
<body>
<main>
<header><div class="logo">L</div><div><h1>Wi-Fi Setup</h1><div class="sub">Lumino Smart Room Controller</div></div></header>
<section class="panel">
<h2>Connect to a Wi-Fi network</h2>
<p class="help">Choose your network and enter its password. Lumino will test the connection before saving the new credentials.</p>
<label for="networks">Available networks</label>
<select id="networks"><option value="">Loading networks…</option></select>
<button type="button" class="secondary" onclick="scanNetworks()">Scan again</button>
<label for="ssid">Wi-Fi name (SSID)</label>
<input id="ssid" type="text" maxlength="32" autocomplete="off" placeholder="Enter Wi-Fi name">
<label for="password">Wi-Fi password</label>
<input id="password" type="password" maxlength="64" autocomplete="new-password" placeholder="Enter Wi-Fi password">
<button type="button" onclick="saveNetwork()">Connect to Wi-Fi</button>
<div class="status" id="status"></div>
<p class="note">Your previous working credentials are kept unless the new network connects successfully. The setup network stays available if the connection fails.</p>
</section>
<a class="back" href="/">← Back to device dashboard</a>
</main>
<script>
const networks=document.getElementById('networks');
const ssidInput=document.getElementById('ssid');
const statusBox=document.getElementById('status');
let scanning=false;
async function scanNetworks(){
 if(scanning)return;
 scanning=true;
 networks.innerHTML='<option value="">Scanning…</option>';
 statusBox.textContent='Searching for nearby Wi-Fi networks…';
 try{
  let data={scanning:true};
  while(data.scanning){
   const r=await fetch('/wifi/scan',{cache:'no-store'});
   data=await r.json();
   if(data.scanning) await new Promise(resolve=>setTimeout(resolve,1200));
  }
  networks.innerHTML='';
  if(!data.networks || data.networks.length===0){
   networks.innerHTML='<option value="">No networks found — enter SSID manually</option>';
  }else{
   networks.innerHTML='<option value="">Select a network…</option>';
   data.networks.forEach(n=>{
    const o=document.createElement('option');
    o.value=n.ssid;
    o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.secure?' 🔒':'');
    networks.appendChild(o);
   });
  }
  statusBox.textContent='Network scan complete.';
 }catch(e){
  networks.innerHTML='<option value="">Scan failed — enter SSID manually</option>';
  statusBox.textContent='Could not scan. You can still enter the network name manually.';
 }finally{scanning=false;}
}
networks.addEventListener('change',()=>{if(networks.value)ssidInput.value=networks.value;});
async function saveNetwork(){
 const ssid=ssidInput.value.trim();
 const password=document.getElementById('password').value;
 if(!ssid){statusBox.textContent='Please enter or select a Wi-Fi name.';return;}
 statusBox.textContent='Testing the new Wi-Fi connection. Please wait…';
 try{
  const body=new URLSearchParams();
  body.set('ssid',ssid);body.set('password',password);
  const r=await fetch('/wifi/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  const data=await r.json();
  if(!r.ok)throw new Error(data.error||'Could not start connection test');
  statusBox.textContent='Connection test started. Keep this page open while Lumino tests the network.';
  pollStatus();
 }catch(e){statusBox.textContent=e.message;}
}
function pollStatus(){
 let attempts=0;
 const timer=setInterval(async()=>{
  attempts++;
  try{
   const r=await fetch('/wifi/status',{cache:'no-store'});
   const d=await r.json();
   if(d.state==='connected'){
    clearInterval(timer);
    statusBox.textContent='Connected! New credentials saved. The setup Wi-Fi will close. Reconnect your phone to the new Wi-Fi and open the controller IP.';
   }else if(d.state==='failed'){
    clearInterval(timer);
    statusBox.textContent='Connection failed. Previous credentials were kept. You can try again.';
   }
  }catch(e){}
  if(attempts>20){clearInterval(timer);statusBox.textContent='Still waiting. If the setup Wi-Fi disconnects, check the new network and controller IP.';}
 },1500);
}
scanNetworks();
</script>
</body>
</html>
)rawliteral";

// ==========================================
// HTTP API AND WIFI PORTAL ROUTES
// ==========================================

void handleDashboard() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", DASHBOARD_HTML);
}

String buildStateJson() {
    String json = "{\"devices\":[";
    for (int i = 0; i < 4; i++) {
        if (i > 0) json += ",";
        json += "{\"name\":\"";
        json += deviceNames[i];
        json += "\",\"on\":";
        json += deviceStates[i] ? "true" : "false";
        json += "}";
    }

    String ipText;
    if (WiFi.status() == WL_CONNECTED) {
        ipText = WiFi.localIP().toString();
    } else if (fallbackAPActive) {
        ipText = WiFi.softAPIP().toString();
    } else {
        ipText = "Not connected";
    }

    json += "],\"wifi\":\"";
    json += wifiStatus;
    json += "\",\"ip\":\"";
    json += ipText;
    json += "\"}";
    return json;
}

void broadcastState() {
    if (!webSocketStarted) return;
    String json = buildStateJson();
    webSocket.broadcastTXT(json);
}

void handleStateRequest() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", buildStateJson());
}

void handleToggleRequest() {
    if (!server.hasArg("index")) {
        server.send(400, "application/json", "{\"error\":\"Missing index\"}");
        return;
    }

    int index = server.arg("index").toInt();
    if (index < 0 || index >= 4) {
        server.send(400, "application/json", "{\"error\":\"Invalid index\"}");
        return;
    }

    toggleDevice((uint8_t)index);
    handleStateRequest();
}

void handleWiFiSetupPage() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", WIFI_SETUP_HTML);
}

void handleWiFiScan() {
    int scanResult = WiFi.scanComplete();

    // Start a scan explicitly if one is not already running.
    if (!wifiScanActive) {
        Serial.println("[WiFi Scan] Starting asynchronous network scan...");
        Serial.printf("[WiFi Scan] Wi-Fi mode before scan: %d\n", (int)WiFi.getMode());

        // In fallback AP+STA mode, the station may still be repeatedly trying
        // the old router credentials. Disconnect only the STA interface first;
        // leave the SoftAP running so the phone stays connected to the portal.
        if (fallbackAPActive && !wifiTestInProgress) {
            Serial.println("[WiFi Scan] Pausing STA connection attempt; keeping setup AP online.");
            WiFi.disconnect(false, false);
            delay(100);
        }

        WiFi.scanDelete();
        scanResult = WiFi.scanNetworks(true, true); // async, include hidden

        if (scanResult == WIFI_SCAN_RUNNING) {
            wifiScanActive = true;
            Serial.println("[WiFi Scan] Scan started successfully.");
            server.send(200, "application/json", "{\"scanning\":true}");
            return;
        }

        if (scanResult >= 0) {
            // Some core versions may complete very quickly.
            wifiScanActive = true;
            Serial.printf("[WiFi Scan] Scan completed immediately: %d networks.\n", scanResult);
        } else {
            Serial.printf("[WiFi Scan] Could not start scan. Result: %d\n", scanResult);
            if (fallbackAPActive && !wifiTestInProgress && activeSSID.length() > 0) {
                Serial.println("[WiFi Scan] Resuming configured router connection after start failure...");
                configureIPForNetwork(activeSSID);
                WiFi.begin(activeSSID.c_str(), activePassword.c_str());
                lastWifiAttempt = millis();
            }
            server.send(200, "application/json", "{\"scanning\":false,\"networks\":[],\"error\":\"Could not start Wi-Fi scan\"}");
            return;
        }
    } else {
        scanResult = WiFi.scanComplete();
    }

    if (scanResult == WIFI_SCAN_RUNNING) {
        server.send(200, "application/json", "{\"scanning\":true}");
        return;
    }

    if (scanResult == WIFI_SCAN_FAILED) {
        Serial.println("[WiFi Scan] Scan failed.");
        wifiScanActive = false;
        WiFi.scanDelete();
        if (fallbackAPActive && !wifiTestInProgress && activeSSID.length() > 0) {
            Serial.println("[WiFi Scan] Resuming configured router connection after scan failure...");
            configureIPForNetwork(activeSSID);
            WiFi.begin(activeSSID.c_str(), activePassword.c_str());
            lastWifiAttempt = millis();
        }
        server.send(200, "application/json", "{\"scanning\":false,\"networks\":[],\"error\":\"Wi-Fi scan failed\"}");
        return;
    }

    Serial.printf("[WiFi Scan] Scan completed. Networks found: %d\n", scanResult);
    Serial.printf("[WiFi Scan] Wi-Fi mode after scan: %d\n", (int)WiFi.getMode());
    wifiScanActive = false;

    // Resume the previously configured router connection after the scan.
    // This does not affect the fallback AP or the unsaved credentials form.
    if (fallbackAPActive && !wifiTestInProgress && activeSSID.length() > 0) {
        Serial.println("[WiFi Scan] Resuming configured router connection...");
        configureIPForNetwork(activeSSID);
        WiFi.begin(activeSSID.c_str(), activePassword.c_str());
        lastWifiAttempt = millis();
    }

    String json = "{\"scanning\":false,\"networks\":[";
    bool first = true;

    for (int i = 0; i < scanResult; i++) {
        String networkName = WiFi.SSID(i);
        if (networkName.length() == 0) continue;

        if (!first) json += ",";
        first = false;

        json += "{\"ssid\":\"";
        for (size_t c = 0; c < networkName.length(); c++) {
            char ch = networkName[c];
            if (ch == '"' || ch == '\\') json += '\\';
            if ((uint8_t)ch >= 32) json += ch;
        }
        json += "\",\"rssi\":";
        json += String(WiFi.RSSI(i));
        json += ",\"secure\":";
        json += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "false" : "true";
        json += "}";
    }

    json += "]}";
    wifiScanActive = false;
    WiFi.scanDelete();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

void handleWiFiSave() {
    if (wifiTestInProgress) {
        server.send(409, "application/json", "{\"error\":\"A WiFi connection test is already running.\"}");
        return;
    }

    if (!server.hasArg("ssid")) {
        server.send(400, "application/json", "{\"error\":\"Missing WiFi name.\"}");
        return;
    }

    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");
    newSSID.trim();

    if (newSSID.length() == 0 || newSSID.length() > 32 || newPassword.length() > 64) {
        server.send(400, "application/json", "{\"error\":\"Invalid WiFi credentials.\"}");
        return;
    }

    previousSSID = activeSSID;
    previousPassword = activePassword;
    pendingSSID = newSSID;
    pendingPassword = newPassword;

    wifiTestInProgress = true;
    wifiTestStarted = millis();
    wifiStatus = "Testing new WiFi";

    // Keep the AP available. Do not save credentials until connection succeeds.
    WiFi.disconnect(false);
    configureIPForNetwork(pendingSSID);
    WiFi.begin(pendingSSID.c_str(), pendingPassword.c_str());

    server.send(202, "application/json", "{\"state\":\"testing\"}");
}

void handleWiFiStatus() {
    String state = "idle";

    if (wifiTestInProgress) {
        state = "testing";
    } else if (WiFi.status() == WL_CONNECTED) {
        state = "connected";
    } else if (fallbackAPActive) {
        state = "failed";
    }

    String json = "{\"state\":\"" + state + "\",\"wifi\":\"" + wifiStatus + "\"}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

// ==========================================
// NTP CLOCK + DAILY FAN SCHEDULER
// ==========================================

void startNtpIfConnected() {
    // This function must never wait for NTP or block the main loop.
    if (WiFi.status() != WL_CONNECTED) {
        ntpConfigured = false;
        clockIsValid = false;
        return;
    }

    time_t nowEpoch = time(nullptr);
    if (nowEpoch >= 1735689600) { // 2025-01-01 00:00:00 UTC
        if (!clockIsValid) {
            struct tm timeInfo;
            localtime_r(&nowEpoch, &timeInfo);
            clockIsValid = true;
            Serial.printf(
                "[NTP] Time synchronized: %04d-%02d-%02d %02d:%02d:%02d\\n",
                timeInfo.tm_year + 1900,
                timeInfo.tm_mon + 1,
                timeInfo.tm_mday,
                timeInfo.tm_hour,
                timeInfo.tm_min,
                timeInfo.tm_sec
            );
        }
        return;
    }

    clockIsValid = false;

    // Start/retry SNTP configuration without waiting for a response.
    if (!ntpConfigured ||
        millis() - lastNtpAttempt >= NTP_RETRY_INTERVAL) {
        Serial.println("[NTP] Starting non-blocking time synchronization...");
        configTzTime(
            TIME_ZONE,
            "pool.ntp.org",
            "time.google.com",
            "time.cloudflare.com"
        );
        ntpConfigured = true;
        lastNtpAttempt = millis();
    }
}

bool getLocalTimeInfo(struct tm &timeInfo) {
    // Returns local time using TIME_ZONE from project_config.h.
    // Read the system clock immediately; never wait inside the control loop.
    time_t nowEpoch = time(nullptr);
    if (nowEpoch < 1735689600) return false; // Time not synchronized yet
    return localtime_r(&nowEpoch, &timeInfo) != nullptr;
}

void loadFanSchedule() {
    if (!storageReady) return;
    fanScheduleEnabled = preferences.getBool("sch_enabled", true);
    uint8_t storedCount = preferences.getUChar("sch_count", 0);
    fanEventCount = min(storedCount, MAX_FAN_EVENTS);
    for (uint8_t i = 0; i < fanEventCount; i++) {
        String minuteKey = "sch_m" + String(i);
        String stateKey = "sch_s" + String(i);
        fanEvents[i].minuteOfDay = min((uint16_t)preferences.getUShort(minuteKey.c_str(), 0), (uint16_t)1439);
        fanEvents[i].turnOn = preferences.getBool(stateKey.c_str(), false);
    }
    Serial.printf("Restored %u daily fan schedule event(s).\n", fanEventCount);
}

void saveFanSchedule() {
    if (!storageReady) return;
    preferences.putBool("sch_enabled", fanScheduleEnabled);
    preferences.putUChar("sch_count", fanEventCount);
    for (uint8_t i = 0; i < fanEventCount; i++) {
        String minuteKey = "sch_m" + String(i);
        String stateKey = "sch_s" + String(i);
        preferences.putUShort(minuteKey.c_str(), fanEvents[i].minuteOfDay);
        preferences.putBool(stateKey.c_str(), fanEvents[i].turnOn);
    }
    // Clear stale entries beyond the current count.
    for (uint8_t i = fanEventCount; i < MAX_FAN_EVENTS; i++) {
        String minuteKey = "sch_m" + String(i);
        String stateKey = "sch_s" + String(i);
        preferences.remove(minuteKey.c_str());
        preferences.remove(stateKey.c_str());
    }
}

void sortFanSchedule() {
    for (uint8_t i = 0; i < fanEventCount; i++) {
        for (uint8_t j = i + 1; j < fanEventCount; j++) {
            if (fanEvents[j].minuteOfDay < fanEvents[i].minuteOfDay) {
                FanScheduleEvent temp = fanEvents[i];
                fanEvents[i] = fanEvents[j];
                fanEvents[j] = temp;
            }
        }
    }
}

String buildScheduleJson() {
    struct tm currentTime;
    clockIsValid = getLocalTimeInfo(currentTime);
    String json = "{\"enabled\":";
    json += fanScheduleEnabled ? "true" : "false";
    json += ",\"clockValid\":";
    json += clockIsValid ? "true" : "false";
    json += ",\"time\":\"";
    struct tm t;
    if (getLocalTimeInfo(t)) {
        char timeText[24];
        strftime(timeText, sizeof(timeText), "%Y-%m-%d %H:%M:%S", &t);
        json += timeText;
    } else {
        json += "Waiting for NTP";
    }
    json += "\",\"events\":[";
    for (uint8_t i = 0; i < fanEventCount; i++) {
        if (i) json += ",";
        char hhmm[6];
        snprintf(hhmm, sizeof(hhmm), "%02u:%02u", fanEvents[i].minuteOfDay / 60, fanEvents[i].minuteOfDay % 60);
        json += "{\"time\":\"" + String(hhmm) + "\",\"on\":";
        json += fanEvents[i].turnOn ? "true" : "false";
        json += "}";
    }
    json += "]}";
    return json;
}

extern const char FAN_SCHEDULE_HTML[] PROGMEM;

void handleSchedulePage() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", FAN_SCHEDULE_HTML);
}

void handleScheduleApi() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", buildScheduleJson());
}

void handleScheduleEnable() {
    if (!server.hasArg("enabled")) {
        server.send(400, "application/json", "{\"error\":\"Missing enabled value\"}");
        return;
    }
    fanScheduleEnabled = server.arg("enabled") == "1";
    saveFanSchedule();
    server.send(200, "application/json", buildScheduleJson());
}

void handleScheduleAdd() {
    if (fanEventCount >= MAX_FAN_EVENTS) {
        server.send(400, "application/json", "{\"error\":\"Maximum of 8 events reached\"}");
        return;
    }
    if (!server.hasArg("time") || !server.hasArg("action")) {
        server.send(400, "application/json", "{\"error\":\"Time and action are required\"}");
        return;
    }
    String timeText = server.arg("time");
    if (timeText.length() != 5 || timeText.charAt(2) != ':') {
        server.send(400, "application/json", "{\"error\":\"Invalid time format\"}");
        return;
    }
    int hour = timeText.substring(0, 2).toInt();
    int minute = timeText.substring(3, 5).toInt();
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        server.send(400, "application/json", "{\"error\":\"Invalid time\"}");
        return;
    }
    uint16_t minuteOfDay = hour * 60 + minute;
    for (uint8_t i = 0; i < fanEventCount; i++) {
        if (fanEvents[i].minuteOfDay == minuteOfDay) {
            server.send(409, "application/json", "{\"error\":\"An event already exists at this time\"}");
            return;
        }
    }
    fanEvents[fanEventCount].minuteOfDay = minuteOfDay;
    fanEvents[fanEventCount].turnOn = server.arg("action") == "on";
    fanEventCount++;
    sortFanSchedule();
    saveFanSchedule();
    server.send(200, "application/json", buildScheduleJson());
}

void handleScheduleDelete() {
    if (!server.hasArg("index")) {
        server.send(400, "application/json", "{\"error\":\"Missing event index\"}");
        return;
    }
    int index = server.arg("index").toInt();
    if (index < 0 || index >= fanEventCount) {
        server.send(400, "application/json", "{\"error\":\"Invalid event index\"}");
        return;
    }
    for (uint8_t i = index; i + 1 < fanEventCount; i++) fanEvents[i] = fanEvents[i + 1];
    fanEventCount--;
    saveFanSchedule();
    server.send(200, "application/json", buildScheduleJson());
}

void runFanSchedule() {
    struct tm now;
    if (!getLocalTimeInfo(now)) {
        clockIsValid = false;
        return;
    }
    clockIsValid = true;
    if (!fanScheduleEnabled || fanEventCount == 0) return;
    int dateKey = (now.tm_year + 1900) * 10000 + (now.tm_mon + 1) * 100 + now.tm_mday;
    int minuteKey = now.tm_hour * 60 + now.tm_min;
    if (dateKey == lastScheduleDate && minuteKey == lastScheduleMinute) return;

    for (uint8_t i = 0; i < fanEventCount; i++) {
        if (fanEvents[i].minuteOfDay == minuteKey) {
            Serial.printf("[Schedule] %02d:%02d -> Fan %s\n", now.tm_hour, now.tm_min, fanEvents[i].turnOn ? "ON" : "OFF");
            setDeviceState(3, fanEvents[i].turnOn);
            lastScheduleDate = dateKey;
            lastScheduleMinute = minuteKey;
            break;
        }
    }
}

const char FAN_SCHEDULE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#101827"><title>Fan Schedule - Lumino</title>
<style>
:root{color-scheme:dark;--bg:#0b1220;--panel:#172438;--text:#eef4ff;--muted:#9aabc2;--accent:#6ee7c0;--line:#30425b}*{box-sizing:border-box}body{margin:0;min-height:100vh;padding:22px 14px;background:radial-gradient(circle at top,#1a2b43,#0b1220 55%);font-family:Arial,sans-serif;color:var(--text)}main{max-width:560px;margin:20px auto}header{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px}h1{font-size:21px;margin:0 0 5px}.sub,.muted{font-size:12px;color:var(--muted)}.panel{background:var(--panel);border:1px solid var(--line);border-radius:17px;padding:18px;margin-bottom:14px}h2{font-size:15px;margin:0 0 12px}label{display:block;font-size:12px;color:#cbd6e8;margin:12px 0 6px}input,select{width:100%;padding:12px;border:1px solid var(--line);border-radius:10px;background:#0d1828;color:var(--text);font-size:14px}button{border:0;border-radius:10px;padding:12px;background:#32aa8c;color:#06251d;font-weight:700;cursor:pointer}.add{width:100%;margin-top:14px}.row{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:12px 0;border-bottom:1px solid var(--line)}.row:last-child{border:0}.eventtime{font-weight:700}.action{font-size:12px;color:var(--accent);margin-top:4px}.delete{background:#3b2935;color:#ffd6df;padding:8px 11px}.topline{display:flex;justify-content:space-between;align-items:center}.toggle{width:auto}.status{font-size:12px;line-height:1.5;color:var(--accent);margin-top:10px;min-height:16px}a{color:var(--accent);text-decoration:none;font-size:12px}.clock{font-size:13px;color:var(--accent);margin-top:6px}
</style></head><body><main><header><div><h1>Fan Schedule</h1><div class="sub">Daily recurring ON/OFF events · local time zone</div></div><a href="/">← Dashboard</a></header>
<section class="panel"><div class="topline"><h2>Schedule control</h2><button class="toggle" id="toggle" onclick="toggleSchedule()">Loading…</button></div><div class="clock" id="clock">Checking controller time…</div><p class="muted">Events repeat every day. Manual fan control remains in effect until the next scheduled event.</p></section>
<section class="panel"><h2>Add an event</h2><label for="time">Time</label><input id="time" type="time" value="22:30"><label for="action">Fan action</label><select id="action"><option value="on">Turn fan ON</option><option value="off">Turn fan OFF</option></select><button class="add" onclick="addEvent()">Add schedule event</button><div class="status" id="status"></div></section>
<section class="panel"><h2>Daily events</h2><div id="events" class="muted">Loading schedule…</div></section></main>
<script>
let schedule={enabled:true,events:[]};
function showStatus(t){document.getElementById('status').textContent=t;}
function render(){document.getElementById('toggle').textContent=schedule.enabled?'Schedule ON':'Schedule OFF';document.getElementById('events').innerHTML='';if(!schedule.events.length){document.getElementById('events').textContent='No events added yet.';}schedule.events.forEach((e,i)=>{const row=document.createElement('div');row.className='row';const left=document.createElement('div');const time=document.createElement('div');time.className='eventtime';time.textContent=e.time;const action=document.createElement('div');action.className='action';action.textContent=e.on?'Turn fan ON':'Turn fan OFF';left.append(time,action);const del=document.createElement('button');del.className='delete';del.textContent='Delete';del.onclick=()=>deleteEvent(i);row.append(left,del);document.getElementById('events').append(row);});document.getElementById('clock').textContent=schedule.clockValid?'Controller time: '+schedule.time:'Clock: waiting for NTP synchronization';}
async function load(){try{const r=await fetch('/api/schedule',{cache:'no-store'});if(!r.ok)throw Error();schedule=await r.json();render();}catch(e){showStatus('Could not load schedule.');}}
async function toggleSchedule(){try{const b=new URLSearchParams({enabled:schedule.enabled?'0':'1'});const r=await fetch('/api/schedule/enable',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});schedule=await r.json();render();}catch(e){showStatus('Could not update schedule.');}}
async function addEvent(){const b=new URLSearchParams({time:document.getElementById('time').value,action:document.getElementById('action').value});try{const r=await fetch('/api/schedule/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});const d=await r.json();if(!r.ok)throw Error(d.error||'Could not add event');schedule=d;render();showStatus('Event saved.');}catch(e){showStatus(e.message);}}
async function deleteEvent(index){const b=new URLSearchParams({index});try{const r=await fetch('/api/schedule/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});const d=await r.json();if(!r.ok)throw Error(d.error||'Could not delete event');schedule=d;render();showStatus('Event deleted.');}catch(e){showStatus(e.message);}}
load();setInterval(load,15000);
</script></body></html>
)rawliteral";

void webSocketEvent(uint8_t clientNumber, WStype_t type, uint8_t * payload, size_t length) {
    (void)payload;
    (void)length;

    if (type == WStype_CONNECTED) {
        IPAddress clientIP = webSocket.remoteIP(clientNumber);
        Serial.printf("[WebSocket] Client %u connected from %s\n",
                      clientNumber, clientIP.toString().c_str());
        String json = buildStateJson();
        webSocket.sendTXT(clientNumber, json);
    } else if (type == WStype_DISCONNECTED) {
        Serial.printf("[WebSocket] Client %u disconnected.\n", clientNumber);
    }
}

// ==========================================
// BROWSER-BASED OTA FIRMWARE UPDATE
// ==========================================

const char OTA_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="en"><head><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#101827"><title>Lumino Firmware Update</title>
<style>body{margin:0;padding:24px 15px;background:#0b1220;color:#eef4ff;font-family:Arial,sans-serif}main{max-width:480px;margin:25px auto}.panel{background:#172438;border:1px solid #30425b;border-radius:18px;padding:22px}h1{font-size:22px}p{color:#9aabc2;line-height:1.5}input,button{width:100%;box-sizing:border-box;padding:14px;border-radius:10px;margin-top:14px}input{background:#0d1828;color:#eef4ff;border:1px solid #30425b}button{border:0;background:#32aa8c;color:#06251d;font-weight:bold}progress{width:100%;height:18px;margin-top:16px}#status{margin-top:14px;color:#6ee7c0}a{color:#6ee7c0}</style></head>
<body><main><section class="panel"><h1>Firmware Update</h1>
<p>Select a compiled ESP32 firmware <b>.bin</b> file. Keep the controller powered and do not close this page during the update.</p>
<form id="f"><input id="file" type="file" accept=".bin" required><button>Upload &amp; Update</button></form>
<progress id="progress" value="0" max="100"></progress><div id="status"></div>
<p><a href="/">Back to dashboard</a></p></section></main>
<script>document.getElementById('f').addEventListener('submit',e=>{e.preventDefault();const file=document.getElementById('file').files[0];if(!file)return;const x=new XMLHttpRequest();const d=new FormData();d.append('firmware',file);x.open('POST','/firmware/upload');x.upload.onprogress=e=>{if(e.lengthComputable)document.getElementById('progress').value=e.loaded/e.total*100};x.onload=()=>{document.getElementById('status').textContent=x.responseText||'Update request completed.'};x.onerror=()=>document.getElementById('status').textContent='Upload failed. Check connection and try again.';document.getElementById('status').textContent='Uploading…';x.send(d)});</script></body></html>
)rawliteral";

void handleFirmwarePage() {
    if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) { server.requestAuthentication(); return; }
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", OTA_PAGE);
}

void handleFirmwareUpload() {
    // Authenticate every multipart upload callback before accepting firmware bytes.
    if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) { return; }
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Update started: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[OTA] Update complete: %u bytes\n", upload.totalSize);
        else Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        Serial.println("[OTA] Upload aborted.");
    }
}

void handleFirmwareUploadResult() {
    if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) { server.requestAuthentication(); return; }
    if (Update.hasError()) {
        server.send(500, "text/plain", "Firmware update failed. The current firmware should remain available.");
    } else {
        server.send(200, "text/plain", "Firmware uploaded successfully. Controller is restarting now.");
        delay(500);
        ESP.restart();
    }
}

void initializeWebSocket() {
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    webSocketStarted = true;
    Serial.println("WebSocket server started on port 81.");
}

void initializeWebServer() {
    server.on("/", HTTP_GET, handleDashboard);
    server.on("/api/state", HTTP_GET, handleStateRequest);
    server.on("/api/toggle", HTTP_POST, handleToggleRequest);
    server.on("/firmware", HTTP_GET, handleFirmwarePage);
    server.on("/firmware/upload", HTTP_POST, handleFirmwareUploadResult, handleFirmwareUpload);

    server.on("/wifi", HTTP_GET, handleWiFiSetupPage);
    server.on("/wifi/scan", HTTP_GET, handleWiFiScan);
    server.on("/wifi/save", HTTP_POST, handleWiFiSave);
    server.on("/wifi/status", HTTP_GET, handleWiFiStatus);

    server.on("/schedule", HTTP_GET, handleSchedulePage);
    server.on("/api/schedule", HTTP_GET, handleScheduleApi);
    server.on("/api/schedule/enable", HTTP_POST, handleScheduleEnable);
    server.on("/api/schedule/add", HTTP_POST, handleScheduleAdd);
    server.on("/api/schedule/delete", HTTP_POST, handleScheduleDelete);

    server.onNotFound([]() {
        if (fallbackAPActive) {
            server.sendHeader(
                "Location",
                String("http://") + WiFi.softAPIP().toString() + "/wifi",
                true
            );
            server.send(302, "text/plain", "");
        } else {
            server.send(404, "text/plain", "Not found");
        }
    });

    server.begin();
    webServerStarted = true;
    Serial.println("HTTP dashboard and WiFi setup portal started on port 80.");
}

// ==========================================
// SETUP
// ==========================================

void setup() {
    Serial.begin(115200);
    delay(300);

    Serial.println();
    Serial.println("================================");
    Serial.println("LUMINO SMART ROOM V1.6-B");
    Serial.println("MODERN OLED + DEVICE SPOTLIGHT + AUTO SLEEP + DAILY FAN SCHEDULE");
    Serial.println("================================");

    // Keep the proven V1.3 hardware initialization order.
    initializeRelays();
    initializeStorage();
    restoreDeviceStates();
    loadFanSchedule();
    initializeOLED();
    initializeTouchSensors();

    // Start Wi-Fi as in V1.3, then start the lightweight HTTP server.
    initializeWiFi();
    startNtpIfConnected();
    initializeWebServer();
    initializeWebSocket();

    Serial.println("Local controller ready.");
}

// ==========================================
// MAIN LOOP
// ==========================================

void loop() {
    // Keep local hardware control first, as in working V1.3.
    readTouchSensors();
    manageWiFi();
    startNtpIfConnected();
    if (millis() - lastClockCheck >= 1000) {
        lastClockCheck = millis();
        runFanSchedule();
    }

    if (wifiStatus != lastBroadcastWifiStatus) {
        lastBroadcastWifiStatus = wifiStatus;
        broadcastState();
    }

    // HTTP handling is added after local control.
    if (dnsServerStarted) {
        dnsServer.processNextRequest();
    }

    if (webServerStarted) {
        server.handleClient();
    }

    if (webSocketStarted) {
        webSocket.loop();
    }

    updateOLED();
}
