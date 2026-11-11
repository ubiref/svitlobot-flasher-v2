#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Preferences.h>

#define LED_PIN  8
#define BOOT_PIN 9

static const char* SVITLOBOT_BASE = "https://api.svitlobot.in.ua/channelPing?channel_key=";
static const char* PORTAL_SSID = "ESP32C3-SvitloBot";
static const char* PORTAL_PASS = "12345678";

const unsigned long PING_INTERVAL          = 60000;
const unsigned long RETRY_PING_INTERVAL    = 5000;
const unsigned long WIFI_RETRY_INTERVAL    = 5000;
const unsigned long BUTTON_PORTAL_HOLD     = 5000;
const unsigned long BUTTON_RESET_HOLD      = 12000;
const unsigned long LED_BLINK_INTERVAL     = 350;

unsigned long lastPingTime = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastBlink = 0;
unsigned long buttonPressedAt = 0;

bool wasWiFiConnected = false;
bool pingPending = true;
bool portalActive = false;
bool ledOn = false;
bool buttonWasDown = false;
bool portalButtonTriggered = false;
bool resetButtonTriggered = false;

WiFiManager wm;
char keyBuff[96] = "";
WiFiManagerParameter p_key("sv_key", "Ключ СвітлоБота", "", 90);

void setLedSolidOn() {
  portalActive = false;
  digitalWrite(LED_PIN, LOW); // LED active-low
  ledOn = true;
}

void setLedOff() {
  digitalWrite(LED_PIN, HIGH);
  ledOn = false;
}

void ledBlinkTick() {
  if (!portalActive) return;
  if (millis() - lastBlink >= LED_BLINK_INTERVAL) {
    lastBlink = millis();
    ledOn = !ledOn;
    digitalWrite(LED_PIN, ledOn ? LOW : HIGH);
  }
}

String loadKey() {
  Preferences prefs;
  prefs.begin("cfg", true);
  String key = prefs.getString("sv_key", "");
  prefs.end();
  return key;
}

void saveKey(const String& key) {
  if (key.length() < 5) return;
  Preferences prefs;
  prefs.begin("cfg", false);
  prefs.putString("sv_key", key);
  prefs.end();
}

void clearKeyOnly() {
  Preferences prefs;
  prefs.begin("cfg", false);
  prefs.remove("sv_key");
  prefs.end();
}

bool svitlobotPing(const String& key) {
  if (WiFi.status() != WL_CONNECTED || key.length() < 5) return false;

  WiFiClientSecure client;
  client.setInsecure(); // Залишено як у попередній робочій версії

  HTTPClient http;
  const String url = String(SVITLOBOT_BASE) + key;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);

  Serial.print("PING... ");
  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed");
    return false;
  }

  const int code = http.GET();
  Serial.printf("HTTP code: %d\n", code);
  http.end();
  return code >= 200 && code < 300;
}

void startManualPortal() {
  if (portalActive) return;

  Serial.println("BOOT held: starting Wi-Fi setup portal manually");
  String saved = loadKey();
  saved.toCharArray(keyBuff, sizeof(keyBuff));
  p_key.setValue(keyBuff, 90);

  portalActive = true;
  lastBlink = millis();
  setLedOff();

  // Точка доступу запускається ТІЛЬКИ після ручного утримання BOOT.
  const bool started = wm.startConfigPortal(PORTAL_SSID, PORTAL_PASS);
  if (!started && WiFi.status() != WL_CONNECTED) {
    Serial.println("Portal start requested; waiting for WiFiManager process");
  }
}

void handleBootButton() {
  const bool down = (digitalRead(BOOT_PIN) == LOW);

  if (down && !buttonWasDown) {
    buttonWasDown = true;
    portalButtonTriggered = false;
    resetButtonTriggered = false;
    buttonPressedAt = millis();
  }

  if (down && buttonWasDown) {
    const unsigned long held = millis() - buttonPressedAt;

    if (held >= BUTTON_RESET_HOLD && !resetButtonTriggered) {
      resetButtonTriggered = true;
      Serial.println("BOOT held 12s: factory reset of Wi-Fi and SvitloBot key");
      wm.resetSettings();
      clearKeyOnly();
      delay(200);
      ESP.restart();
    } else if (held >= BUTTON_PORTAL_HOLD && !portalButtonTriggered) {
      portalButtonTriggered = true;
      startManualPortal();
    }
  }

  if (!down && buttonWasDown) {
    buttonWasDown = false;
    portalButtonTriggered = false;
    resetButtonTriggered = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  setLedOff();
  pinMode(BOOT_PIN, INPUT_PULLUP);

  wm.setConfigPortalBlocking(false);
  wm.setWiFiAutoReconnect(true);
  wm.setAPCallback([](WiFiManager*) {
    portalActive = true;
    Serial.println("Wi-Fi setup AP is active");
  });

  String saved = loadKey();
  saved.toCharArray(keyBuff, sizeof(keyBuff));
  p_key.setValue(keyBuff, 90);
  wm.addParameter(&p_key);

  // Не використовуємо autoConnect(): він запускає портал автоматично,
  // якщо роутер ще не встиг завантажитися.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  Serial.println("Starting STA connection using saved Wi-Fi credentials...");
  WiFi.begin();
  lastReconnectAttempt = millis();
}

void loop() {
  wm.process();
  handleBootButton();
  ledBlinkTick();

  if (WiFi.status() == WL_CONNECTED) {
    if (portalActive) {
      String keyFromPortal = String(p_key.getValue());
      keyFromPortal.trim();
      if (keyFromPortal.length() > 0) saveKey(keyFromPortal);

      wm.stopConfigPortal();
      portalActive = false;
      Serial.println("Wi-Fi connected; setup portal stopped");
    }

    if (!wasWiFiConnected) {
      wasWiFiConnected = true;
      Serial.print("Wi-Fi connected. IP: ");
      Serial.println(WiFi.localIP());
      pingPending = true;
      lastPingTime = 0; // Негайний ping після відновлення Wi-Fi
    }

    const unsigned long pingInterval = pingPending ? RETRY_PING_INTERVAL : PING_INTERVAL;
    if (lastPingTime == 0 || millis() - lastPingTime >= pingInterval) {
      lastPingTime = millis();
      if (svitlobotPing(loadKey())) {
        Serial.println("SvitloBot ping successful");
        pingPending = false;
        setLedSolidOn();
      } else {
        Serial.println("SvitloBot ping failed; retrying in 5 seconds");
        pingPending = true;
        setLedOff();
      }
    }
  } else {
    if (wasWiFiConnected) {
      Serial.println("Wi-Fi connection lost");
      wasWiFiConnected = false;
      pingPending = true;
      lastPingTime = 0;
      if (!portalActive) setLedOff();
    }

    // Портал не запускається автоматично. Плата продовжує пробувати
    // підключитися до збереженої мережі, поки роутер завантажується.
    if (!portalActive && millis() - lastReconnectAttempt >= WIFI_RETRY_INTERVAL) {
      lastReconnectAttempt = millis();
      Serial.println("Wi-Fi unavailable; retrying saved credentials...");
      WiFi.reconnect();
    }
  }

  delay(5);
}
