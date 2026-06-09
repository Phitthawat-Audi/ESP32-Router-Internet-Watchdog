#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_now.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =====================================================
// WIFI CONFIG
// =====================================================
const char* ssid     = "dlink_DWR-961_F1FF";
const char* password = "FAqst37625";

// =====================================================
// INTERNET TEST
// =====================================================
const char* testHost = "httpbin.org";
const int testPort = 80;

WiFiClient client;

// =====================================================
// OLED CONFIG
// =====================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  -1
);

// =====================================================
// RELAY CONFIG
// =====================================================
#define RELAY_PIN 25

// LOW = ON
// HIGH = OFF

// =====================================================
// TIMER
// =====================================================
const unsigned long HEARTBEAT_INTERVAL      = 15000;
const unsigned long INTERNET_TIMEOUT        = 120000;
const unsigned long RELAY_OFF_TIME          = 10000;
const unsigned long ROUTER_BOOT_WAIT        = 120000;
const unsigned long INTERNET_CHECK_INTERVAL = 15000;
const unsigned long WIFI_CONNECT_TIMEOUT    = 30000;

// =====================================================
// RETRY
// =====================================================
const int MAX_INTERNET_RETRY = 3;

// =====================================================
// VARIABLES
// =====================================================
unsigned long lastHeartbeatTime     = 0;
unsigned long lastInternetCheckTime = 0;

bool remotePaused = false;

String wifiStatusText     = "UNKNOWN";
String internetStatusText = "UNKNOWN";

float speedMbps = 0.0;

// =====================================================
// ESP-NOW RECEIVE CALLBACK
// =====================================================
void onReceive(const esp_now_recv_info *info,
               const uint8_t *data,
               int len) {

  String msg = "";

  for (int i = 0; i < len; i++) {
    msg += (char)data[i];
  }

  Serial.print("\nESP-NOW MSG: ");
  Serial.println(msg);

  if (msg == "PAUSE") {

    remotePaused = true;

    Serial.println("REMOTE PAUSE ENABLED");
  }

  else if (msg == "RESUME") {

    remotePaused = false;

    Serial.println("REMOTE RESUME");
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println("\n=================================");
  Serial.println("ESP32 INTERNET WATCHDOG START");
  Serial.println("=================================");

  // Relay
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {

    Serial.println("OLED NOT FOUND");

    while (true);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  // WiFi
  WiFi.setSleep(false);

  WiFi.mode(WIFI_STA);

  // =====================================================
  // ESP-NOW INIT
  // =====================================================
  if (esp_now_init() != ESP_OK) {

    Serial.println("ESP-NOW INIT FAILED");

    while (true);
  }

  esp_now_register_recv_cb(onReceive);

  Serial.println("ESP-NOW READY");

  connectWiFi();

  updateOLED();
}

// =====================================================
// LOOP
// =====================================================
void loop() {

  // =========================================
  // REMOTE PAUSE
  // =========================================
  if (remotePaused) {

    Serial.println("REMOTE PAUSED");

    display.clearDisplay();

    display.setCursor(0, 20);
    display.println("REMOTE PAUSED");

    display.display();

    delay(3000);

    return;
  }

  // =========================================
  // HEARTBEAT
  // =========================================
  if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {

    lastHeartbeatTime = millis();

    heartbeat();
  }

  // =========================================
  // INTERNET CHECK
  // =========================================
  if (millis() - lastInternetCheckTime >= INTERNET_CHECK_INTERVAL) {

    lastInternetCheckTime = millis();

    checkWiFi();

    checkInternetStatus();

    speedTest();

    updateOLED();
  }
}

// =====================================================
// OLED UPDATE
// =====================================================
void updateOLED() {

  display.clearDisplay();

  display.setCursor(0, 0);
  display.print("IP:");
  display.println(WiFi.localIP());

  display.setCursor(0, 16);
  display.print("Wifi:");
  display.print(wifiStatusText);

  display.print(" Net:");
  display.println(internetStatusText);

  display.setCursor(0, 32);
  display.print("RSSI:");
  display.print(WiFi.RSSI());
  display.println(" dBm");

  display.setCursor(0, 48);
  display.print("Mbps:");
  display.println(speedMbps, 2);

  display.display();
}

// =====================================================
// WIFI CONNECT
// =====================================================
bool connectWiFi() {

  Serial.println("\nConnecting WiFi...");

  display.clearDisplay();
  display.setCursor(0, 20);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.disconnect();

  delay(1000);

  WiFi.mode(WIFI_STA);

  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttemptTime < WIFI_CONNECT_TIMEOUT) {

    Serial.print(".");

    delay(500);
  }

  Serial.println("");

  if (WiFi.status() == WL_CONNECTED) {

    wifiStatusText = "OK";

    Serial.println("WiFi Connected");

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());

    // IMPORTANT FOR ESP-NOW
    Serial.print("WiFi Channel: ");
    Serial.println(WiFi.channel());

    return true;
  }

  wifiStatusText = "LOST";

  Serial.println("WiFi Connect Failed");

  return false;
}

// =====================================================
// HEARTBEAT
// =====================================================
void heartbeat() {

  Serial.println("\n===== HEARTBEAT =====");

  Serial.print("Free Heap: ");
  Serial.println(ESP.getFreeHeap());

  if (WiFi.status() == WL_CONNECTED) {

    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());
  }
}

// =====================================================
// CHECK WIFI
// =====================================================
void checkWiFi() {

  if (WiFi.status() == WL_CONNECTED) {

    wifiStatusText = "OK";

    Serial.println("WiFi OK");

    return;
  }

  wifiStatusText = "LOST";

  Serial.println("WiFi LOST");

  connectWiFi();
}

// =====================================================
// INTERNET CHECK
// =====================================================
void checkInternetStatus() {

  bool internetOK = false;

  for (int i = 1; i <= MAX_INTERNET_RETRY; i++) {

    Serial.print("Internet Retry: ");
    Serial.println(i);

    long responseTime = 0;

    if (checkInternet(responseTime)) {

      internetOK = true;

      break;
    }

    delay(2000);
  }

  if (internetOK) {

    internetStatusText = "OK";

    Serial.println("Internet OK");

    return;
  }

  internetStatusText = "LOST";

  Serial.println("Internet LOST");
}

// =====================================================
// INTERNET TEST
// =====================================================
bool checkInternet(long &responseTime) {

  if (WiFi.status() != WL_CONNECTED) {

    return false;
  }

  HTTPClient http;

  unsigned long startTime = millis();

  http.begin("http://clients3.google.com/generate_204");

  http.setConnectTimeout(5000);

  http.setTimeout(5000);

  int httpCode = http.GET();

  responseTime = millis() - startTime;

  http.end();

  return (httpCode == 204);
}

// =====================================================
// SPEED TEST
// =====================================================
void speedTest() {

  Serial.println("\n===== SPEED TEST =====");

  int successRequests = 0;

  int numRequests = 5;

  long startTime = millis();

  for (int i = 0; i < numRequests; i++) {

    if (client.connect(testHost, testPort)) {

      client.print(
        String("GET /get HTTP/1.1\r\n") +
        "Host: " + testHost + "\r\n" +
        "Connection: close\r\n\r\n"
      );

      unsigned long timeout = millis();

      while (!client.available()) {

        if (millis() - timeout > 5000) {

          client.stop();

          break;
        }

        delay(10);
      }

      if (client.available()) {

        while (client.available()) {

          client.read();
        }

        successRequests++;
      }

      client.stop();
    }

    delay(100);
  }

  long elapsedTime = millis() - startTime;

  speedMbps =
    (successRequests * 0.2 * 8) /
    (elapsedTime / 1000.0);

  Serial.print("Approx Speed: ");
  Serial.print(speedMbps);
  Serial.println(" Mbps");
}