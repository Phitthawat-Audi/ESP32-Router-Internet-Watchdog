#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_task_wdt.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define MODE_WIFI 1
#define MODE_LAN  2
#define MODE_BOTH 3
#define CONNECTION_MODE MODE_BOTH

#if CONNECTION_MODE == MODE_WIFI || CONNECTION_MODE == MODE_BOTH
const char* ssid     = "WIFI";
const char* password = "Password";
#endif

#if CONNECTION_MODE == MODE_LAN || CONNECTION_MODE == MODE_BOTH
#define ETH_CS_PIN  5
#define ETH_RST_PIN 26
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01 };
#endif

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool oledOK = false;

#define RELAY_PIN 25

const uint32_t WDT_TIMEOUT_SEC          = 30;
const unsigned long HEARTBEAT_INTERVAL      = 15000UL;
const unsigned long INTERNET_CHECK_INTERVAL = 15000UL;
const unsigned long INTERNET_TIMEOUT        = 120000UL;
const unsigned long RELAY_OFF_TIME          = 10000UL;
const unsigned long ROUTER_BOOT_WAIT        = 120000UL;
const unsigned long SPEED_TEST_INTERVAL     = 300000UL;
const unsigned long WIFI_CONNECT_TIMEOUT    = 20000UL;
const unsigned long INTERNET_STABLE_TIME    = 300000UL;
const unsigned long WIFI_LOST_TIMEOUT       = 300000UL;
const unsigned long ETH_RECOVER_INTERVAL    = 30000UL;
const unsigned long PAUSE_10_MIN = 600000UL;
const unsigned long PAUSE_30_MIN = 1800000UL;
const unsigned long PAUSE_1_HOUR = 3600000UL;
const unsigned long PAUSE_5_HOUR = 18000000UL;
const int MAX_INTERNET_RETRY = 3;

unsigned long lastHeartbeatTime      = 0;
unsigned long lastInternetCheckTime  = 0;
unsigned long lastSpeedTestTime      = 0;
unsigned long internetLostStartTime  = 0;
unsigned long internetStableStart    = 0;
unsigned long pauseStartTime         = 0;
unsigned long wifiLostStartTime      = 0;
unsigned long lastEthRecoverAttempt  = 0;
unsigned long routerBootStartTime    = 0;

bool internetWasLost   = false;
bool systemPaused      = false;
bool systemStopped     = false;
bool ethOK             = false;
bool wifiOK            = false;
bool waitingRouterBoot = false;

String activeInterface = "NONE";
int rebootCount = 0;
int pauseLevel  = 0;

const char* ethStatusText      = "N/A";
const char* wifiStatusText     = "N/A";
const char* internetStatusText = "UNKNOWN";
float speedMbps = 0.0;

void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    delay(50);
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(500);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms     = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_reconfigure(&wdt_cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);

  Serial.println("\n=================================");
  Serial.println(" ESP32 INTERNET WATCHDOG v2.0");
  Serial.print  (" Mode: ");
  if      (CONNECTION_MODE == MODE_WIFI) Serial.println("WiFi Only");
  else if (CONNECTION_MODE == MODE_LAN)  Serial.println("LAN Only");
  else                                   Serial.println("LAN + WiFi");
  Serial.println("=================================");
  Serial.print("Reset Reason: ");
  Serial.println(esp_reset_reason());

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[WARN] OLED NOT FOUND");
    oledOK = false;
  } else {
    oledOK = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    oledStatus("STARTING...", "", "", "");
  }

#if CONNECTION_MODE == MODE_LAN || CONNECTION_MODE == MODE_BOTH
  initEthernet();
  esp_task_wdt_reset();
#endif

#if CONNECTION_MODE == MODE_WIFI || CONNECTION_MODE == MODE_BOTH
  Serial.println("[WiFi] Starting...");
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  esp_task_wdt_reset();

  unsigned long wStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wStart < WIFI_CONNECT_TIMEOUT) {
    esp_task_wdt_reset();
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiOK = true; wifiStatusText = "OK";
    Serial.print("[WiFi] OK  IP:"); Serial.print(WiFi.localIP());
    Serial.print("  RSSI:");       Serial.println(WiFi.RSSI());
  } else {
    wifiOK = false; wifiStatusText = "WAIT";
    Serial.println("[WiFi] Not connected yet – will retry in loop");
  }
#endif

  selectActiveInterface();
  updateOLED();
}

void loop() {
  esp_task_wdt_reset();

  if (systemStopped) {
    oledStatus("!! SYSTEM !!", "!! STOPPED !!", "Manual reset", "required");
    safeDelay(10000);
    return;
  }

  if (systemPaused) {
    handlePauseMode();
    return;
  }

  if (waitingRouterBoot) {
    unsigned long elapsed = millis() - routerBootStartTime;
    if (elapsed < ROUTER_BOOT_WAIT) {
      unsigned long remain = (ROUTER_BOOT_WAIT - elapsed) / 1000;
      Serial.print("[RELAY] Waiting router boot: ");
      Serial.print(remain); Serial.println(" sec");
      if (oledOK) {
        display.clearDisplay();
        display.setCursor(0, 0);  display.println("ROUTER REBOOTING");
        display.setCursor(0, 20); display.print("Wait: "); display.print(remain); display.println("s");
        display.setCursor(0, 40); display.print("Reboots: "); display.println(rebootCount);
        display.display();
      }
      safeDelay(2000);
      return;
    }
    waitingRouterBoot = false;
    internetLostStartTime = millis();
    Serial.println("[RELAY] Router boot wait done – resuming checks");
  }

  if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    lastHeartbeatTime = millis();
    heartbeat();
  }

  if (millis() - lastInternetCheckTime >= INTERNET_CHECK_INTERVAL) {
    lastInternetCheckTime = millis();

#if CONNECTION_MODE == MODE_LAN || CONNECTION_MODE == MODE_BOTH
    checkEthernet();
#endif
#if CONNECTION_MODE == MODE_WIFI || CONNECTION_MODE == MODE_BOTH
    checkWiFi();
#endif

    selectActiveInterface();

    if (activeInterface == "NONE") {
      Serial.println("[CHECK] No interface available");
      internetStatusText = "NO IF";
      updateOLED();
      return;
    }

    checkInternetStatus();
    updateOLED();
  }

  if (millis() - lastSpeedTestTime >= SPEED_TEST_INTERVAL) {
    lastSpeedTestTime = millis();
    if (activeInterface != "NONE") {
      speedTest();
      updateOLED();
    }
  }
}

#if CONNECTION_MODE == MODE_LAN || CONNECTION_MODE == MODE_BOTH

void initEthernet() {
  Serial.println("[ETH] Initializing...");
  if (ETH_RST_PIN >= 0) {
    pinMode(ETH_RST_PIN, OUTPUT);
    digitalWrite(ETH_RST_PIN, LOW);
    safeDelay(150);
    digitalWrite(ETH_RST_PIN, HIGH);
    safeDelay(300);
  }
  Ethernet.init(ETH_CS_PIN);
  esp_task_wdt_reset();
  int r = Ethernet.begin(mac, 10000, 4000);
  esp_task_wdt_reset();
  if (r == 0) {
    if      (Ethernet.hardwareStatus() == EthernetNoHardware) ethStatusText = "NO HW";
    else if (Ethernet.linkStatus()     == LinkOFF)            ethStatusText = "NO LINK";
    else                                                       ethStatusText = "NO DHCP";
    ethOK = false;
    Serial.print("[ETH] FAILED: "); Serial.println(ethStatusText);
  } else {
    ethOK = true; ethStatusText = "OK";
    Serial.print("[ETH] OK  IP: "); Serial.println(Ethernet.localIP());
  }
}

void checkEthernet() {
  Ethernet.maintain();
  esp_task_wdt_reset();

  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    if (ethOK) Serial.println("[ETH] Hardware not found!");
    ethOK = false; ethStatusText = "NO HW";
    return;
  }
  if (Ethernet.linkStatus() == LinkOFF) {
    delay(300); esp_task_wdt_reset();
    if (Ethernet.linkStatus() == LinkOFF) {
      if (ethOK) Serial.println("[ETH] Link DOWN");
      ethOK = false; ethStatusText = "NO LINK";
      lastEthRecoverAttempt = 0;
      return;
    }
  }
  if (Ethernet.localIP() == IPAddress(0, 0, 0, 0)) {
    if (ethOK) Serial.println("[ETH] Lost IP");
    ethOK = false; ethStatusText = "NO IP";
    if (millis() - lastEthRecoverAttempt >= ETH_RECOVER_INTERVAL) {
      lastEthRecoverAttempt = millis();
      Serial.println("[ETH] Re-initializing...");
      initEthernet();
    }
    return;
  }
  if (!ethOK) {
    Serial.print("[ETH] Recovered  IP: "); Serial.println(Ethernet.localIP());
  }
  ethOK = true; ethStatusText = "OK";
}

#endif

#if CONNECTION_MODE == MODE_WIFI || CONNECTION_MODE == MODE_BOTH

bool connectWiFi() {
  Serial.println("[WiFi] Reconnecting...");
  WiFi.disconnect(false);
  safeDelay(500);
  WiFi.begin(ssid, password);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_CONNECT_TIMEOUT) {
    esp_task_wdt_reset();
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiOK = true; wifiStatusText = "OK"; wifiLostStartTime = 0;
    Serial.print("[WiFi] OK  IP:"); Serial.print(WiFi.localIP());
    Serial.print("  RSSI:");        Serial.println(WiFi.RSSI());
    return true;
  }
  wifiOK = false; wifiStatusText = "WAIT";
  Serial.println("[WiFi] Reconnect failed – retry next cycle");
  return false;
}

void checkWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiOK) Serial.println("[WiFi] Recovered");
    wifiOK = true; wifiStatusText = "OK"; wifiLostStartTime = 0;
    return;
  }
  if (wifiOK) {
    Serial.println("[WiFi] LOST");
    wifiLostStartTime = millis();
  }
  wifiOK = false; wifiStatusText = "LOST";

#if CONNECTION_MODE == MODE_WIFI
  connectWiFi();
  if (wifiLostStartTime > 0) {
    if (millis() - wifiLostStartTime >= WIFI_LOST_TIMEOUT) {
      Serial.println("[WiFi] Lost too long – restarting ESP32");
      safeDelay(500);
      ESP.restart();
    }
  }
#else
  if (!ethOK) connectWiFi();
  else {
    static unsigned long lastWifiRetry = 0;
    if (millis() - lastWifiRetry >= WIFI_CONNECT_TIMEOUT) {
      lastWifiRetry = millis();
      connectWiFi();
    }
  }
#endif
}

#endif

void selectActiveInterface() {
  String prev = activeInterface;
#if CONNECTION_MODE == MODE_BOTH
  if      (ethOK)  activeInterface = "ETH";
  else if (wifiOK) activeInterface = "WIFI";
  else             activeInterface = "NONE";
#elif CONNECTION_MODE == MODE_LAN
  activeInterface = ethOK  ? "ETH"  : "NONE";
#elif CONNECTION_MODE == MODE_WIFI
  activeInterface = wifiOK ? "WIFI" : "NONE";
#endif
  if (activeInterface != prev) {
    Serial.print("[IF] Changed: "); Serial.print(prev);
    Serial.print(" → ");           Serial.println(activeInterface);
  }
}

void checkInternetStatus() {
  bool internetOK  = false;
  long bestLatency = 0;

  for (int i = 1; i <= MAX_INTERNET_RETRY; i++) {
    Serial.print("[NET] Check "); Serial.print(i);
    Serial.print(" via "); Serial.println(activeInterface);
    long rt = 0;
    if (checkInternet(rt)) { internetOK = true; bestLatency = rt; break; }
    esp_task_wdt_reset();
    safeDelay(1500);
  }

#if CONNECTION_MODE == MODE_BOTH
  if (!internetOK && activeInterface == "ETH" && wifiOK) {
    Serial.println("[NET] ETH internet failed – trying WiFi fallback");
    String savedIF = activeInterface;
    activeInterface = "WIFI";
    for (int i = 1; i <= MAX_INTERNET_RETRY; i++) {
      long rt = 0;
      if (checkInternet(rt)) {
        internetOK = true; bestLatency = rt;
        Serial.println("[NET] WiFi fallback SUCCESS");
        break;
      }
      esp_task_wdt_reset();
      safeDelay(1500);
    }
    if (!internetOK) {
      activeInterface = savedIF;
      Serial.println("[NET] WiFi fallback also failed – back to ETH");
    }
  }
#endif

  if (internetOK) {
    internetStatusText = "OK";
    Serial.print("[NET] OK  Latency: "); Serial.print(bestLatency); Serial.println(" ms");
    if (internetStableStart == 0 && internetWasLost) internetStableStart = millis();
    if (internetWasLost && internetStableStart > 0 &&
        millis() - internetStableStart >= INTERNET_STABLE_TIME) {
      Serial.println("[NET] Stable – resetting counters");
      internetWasLost = false; rebootCount = 0; pauseLevel = 0; internetStableStart = 0;
    }
    return;
  }

  internetStatusText  = "LOST";
  internetStableStart = 0;
  Serial.println("[NET] LOST");
  if (!internetWasLost) { internetLostStartTime = millis(); internetWasLost = true; }

  unsigned long lostDuration = millis() - internetLostStartTime;
  Serial.printf("[NET] Lost duration: %lu min %lu sec\n", lostDuration / 60000, (lostDuration / 1000) % 60);

  if (lostDuration >= INTERNET_TIMEOUT) {
    restartRelay();
    rebootCount++;
    Serial.print("[NET] Reboot count: "); Serial.println(rebootCount);
  }
  if (rebootCount >= 3) startPauseMode();
}

bool checkInternet(long &responseTime) {
  unsigned long startTime = millis();

#if CONNECTION_MODE == MODE_LAN || CONNECTION_MODE == MODE_BOTH
  if (activeInterface == "ETH") {
    EthernetClient client;
    client.setTimeout(5000);
    if (client.connect("clients3.google.com", 80)) {
      client.println("GET /generate_204 HTTP/1.1");
      client.println("Host: clients3.google.com");
      client.println("Connection: close");
      client.println();
      unsigned long t = millis();
      while (!client.available()) {
        esp_task_wdt_reset(); delay(1);
        if (millis() - t > 5000) break;
      }
      bool ok = false;
      if (client.available()) {
        String line = client.readStringUntil('\n');
        ok = (line.indexOf("204") >= 0);
        while (client.available()) { client.read(); esp_task_wdt_reset(); }
      }
      client.stop();
      responseTime = millis() - startTime;
      return ok;
    }
    responseTime = millis() - startTime;
    return false;
  }
#endif

#if CONNECTION_MODE == MODE_WIFI || CONNECTION_MODE == MODE_BOTH
  if (activeInterface == "WIFI") {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.begin("http://connectivitycheck.gstatic.com/generate_204");
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    int code = http.GET();
    http.end();
    responseTime = millis() - startTime;
    return (code == 204);
  }
#endif

  return false;
}

void speedTest() {
  Serial.println("\n[SPEED] Starting...");
  int  ok = 0;
  long startTime = millis();
  const char* host = "httpbin.org";

  for (int i = 0; i < 5; i++) {
    esp_task_wdt_reset();
    bool reqOK = false;

#if CONNECTION_MODE == MODE_LAN || CONNECTION_MODE == MODE_BOTH
    if (activeInterface == "ETH") {
      EthernetClient c; c.setTimeout(5000);
      if (c.connect(host, 80)) {
        c.print(String("GET /get HTTP/1.1\r\nHost: ") + host + "\r\nConnection: close\r\n\r\n");
        unsigned long t = millis();
        while (!c.available()) { delay(1); esp_task_wdt_reset(); if (millis()-t > 5000) { c.stop(); break; } }
        if (c.available()) { while (c.available()) { c.read(); esp_task_wdt_reset(); } reqOK = true; }
        c.stop();
      }
    }
#endif
#if CONNECTION_MODE == MODE_WIFI || CONNECTION_MODE == MODE_BOTH
    if (activeInterface == "WIFI") {
      WiFiClient c;
      if (c.connect(host, 80)) {
        c.print(String("GET /get HTTP/1.1\r\nHost: ") + host + "\r\nConnection: close\r\n\r\n");
        unsigned long t = millis();
        while (!c.available()) { delay(1); esp_task_wdt_reset(); if (millis()-t > 5000) { c.stop(); break; } }
        if (c.available()) { while (c.available()) { c.read(); esp_task_wdt_reset(); } reqOK = true; }
        c.stop();
      }
    }
#endif
    if (reqOK) ok++;
    safeDelay(100);
  }

  long elapsed = millis() - startTime;
  speedMbps = (ok * 0.2f * 8.0f) / (elapsed / 1000.0f);
  Serial.print("[SPEED] ~"); Serial.print(speedMbps, 2); Serial.println(" Mbps");
}

void heartbeat() {
  int heap = ESP.getFreeHeap();
  Serial.printf("\n[HB] Heap:%d  IF:%s  Reboots:%d\n", heap, activeInterface.c_str(), rebootCount);
#if CONNECTION_MODE == MODE_LAN || CONNECTION_MODE == MODE_BOTH
  if (ethOK)  { Serial.print("[HB] ETH IP: "); Serial.println(Ethernet.localIP()); }
#endif
#if CONNECTION_MODE == MODE_WIFI || CONNECTION_MODE == MODE_BOTH
  if (wifiOK) Serial.printf("[HB] WiFi RSSI: %d\n", WiFi.RSSI());
#endif
  if (heap < 10000) {
    Serial.println("[HB] Low heap – restarting ESP32");
    safeDelay(500);
    ESP.restart();
  }
}

void restartRelay() {
  Serial.println("[RELAY] Router OFF");
  digitalWrite(RELAY_PIN, HIGH);
  safeDelay(RELAY_OFF_TIME);
  Serial.println("[RELAY] Router ON");
  digitalWrite(RELAY_PIN, LOW);
  waitingRouterBoot   = true;
  routerBootStartTime = millis();
  Serial.println("[RELAY] Waiting for router to boot (non-blocking)...");
}

void startPauseMode() {
  systemPaused   = true;
  pauseStartTime = millis();
  rebootCount    = 0;
  pauseLevel++;
  const char* label;
  if      (pauseLevel == 1) label = "10 MIN";
  else if (pauseLevel == 2) label = "30 MIN";
  else if (pauseLevel == 3) label = "1 HOUR";
  else if (pauseLevel == 4) label = "5 HOUR";
  else { Serial.println("[SYS] SYSTEM STOPPED"); systemStopped = true; return; }
  Serial.print("[SYS] PAUSE "); Serial.println(label);
}

void handlePauseMode() {
  esp_task_wdt_reset();
  unsigned long pauseTime;
  if      (pauseLevel == 1) pauseTime = PAUSE_10_MIN;
  else if (pauseLevel == 2) pauseTime = PAUSE_30_MIN;
  else if (pauseLevel == 3) pauseTime = PAUSE_1_HOUR;
  else                      pauseTime = PAUSE_5_HOUR;

  unsigned long elapsed = millis() - pauseStartTime;
  unsigned long remain  = elapsed < pauseTime ? (pauseTime - elapsed) / 1000 : 0;
  Serial.printf("[PAUSE] Level:%d  Remaining:%lu sec\n", pauseLevel, remain);

  if (oledOK) {
    display.clearDisplay();
    display.setCursor(0, 0);  display.println("=== PAUSE MODE ===");
    display.setCursor(0, 16); display.print("Level: "); display.println(pauseLevel);
    display.setCursor(0, 32); display.print("Wait : "); display.print(remain); display.println(" s");
    display.setCursor(0, 48); display.print("Reboots: "); display.println(rebootCount);
    display.display();
  }

  if (elapsed >= pauseTime) {
    Serial.println("[SYS] RESUME");
    systemPaused          = false;
    internetLostStartTime = millis();
    internetWasLost       = false;
  }
  safeDelay(1000);
}

void oledStatus(const char* l0, const char* l1, const char* l2, const char* l3) {
  if (!oledOK) return;
  display.clearDisplay();
  display.setCursor(0,  0); display.println(l0);
  display.setCursor(0, 16); display.println(l1);
  display.setCursor(0, 32); display.println(l2);
  display.setCursor(0, 48); display.println(l3);
  display.display();
}

void updateOLED() {
  if (!oledOK) return;
  display.clearDisplay();

  display.setCursor(0, 0);
  display.print("["); display.print(activeInterface); display.print("] ");
#if CONNECTION_MODE == MODE_LAN || CONNECTION_MODE == MODE_BOTH
  if (activeInterface == "ETH")  { display.println(Ethernet.localIP()); goto line1; }
#endif
#if CONNECTION_MODE == MODE_WIFI || CONNECTION_MODE == MODE_BOTH
  if (activeInterface == "WIFI") { display.println(WiFi.localIP()); goto line1; }
#endif
  display.println("NO IP");

line1:
  display.setCursor(0, 16);
#if CONNECTION_MODE == MODE_BOTH
  display.print("E:"); display.print(ethStatusText);
  display.print(" W:"); display.println(wifiStatusText);
#elif CONNECTION_MODE == MODE_LAN
  display.print("ETH:"); display.println(ethStatusText);
#else
  display.print("WiFi:"); display.println(wifiStatusText);
#endif

  display.setCursor(0, 32);
  display.print("Net:"); display.print(internetStatusText);
#if CONNECTION_MODE == MODE_WIFI || CONNECTION_MODE == MODE_BOTH
  if (activeInterface == "WIFI") {
    display.print(" "); display.print(WiFi.RSSI()); display.print("dB");
  }
#endif

  display.setCursor(0, 48);
  display.print(speedMbps, 2); display.print("M Rb:"); display.println(rebootCount);
  display.display();
}
