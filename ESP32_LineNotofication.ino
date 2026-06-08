#include <WiFi.h>
#include <esp_now.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

const char* ssid = "WIFI";
const char* password = "Password";

String CHANNEL_ACCESS_TOKEN = "dTskFkN/RQPKfvbYlrGCDhrwlgShBuLEDEOgihRSwrvlMKaZ707VLSz5/9dBld2Q2He74lJhNlzg58zYfc9s8bOUc1ue7q8qTZ+DSihu2ltoSbzwzgd6us0vFppEI6lZhYO2B9DQBEyKhvpm8NxMywdB04t89/1O/w1cDnyilFU=";
String USER_ID = "U5c4c19c155eb27bdad410604cc0da381"; // Line Notifiction (ใส่IDของผู้ใช้งาน)

typedef struct {
  int resetCount;
  char message[100];
} AlertData;

AlertData incomingData;

String pendingMessage = "";
bool newMessageReceived = false;

void sendLineMessage(String message);

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  memcpy(&incomingData, data, sizeof(incomingData));

  Serial.println("\nReceived from ESP1");
  Serial.println(incomingData.message);
  Serial.print("Reset Count: ");
  Serial.println(incomingData.resetCount);

  pendingMessage = "แจ้งเตือนจาก ESP1\n";
  pendingMessage += String(incomingData.message);
  pendingMessage += "\nReset Count: ";
  pendingMessage += String(incomingData.resetCount);

  newMessageReceived = true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected");
  Serial.print("ESP2 IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("ESP2 MAC Address: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  delay(3000);
  sendLineMessage("ESP2 พร้อมรับแจ้งเตือนจาก ESP1 แล้ว");
}

void loop() {
  if (newMessageReceived) {
    Serial.println("Sending LINE outside ESP-NOW callback");

    newMessageReceived = false;

    delay(1000);
    sendLineMessage(pendingMessage);
  }

  delay(100);
}

void sendLineMessage(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting...");
    WiFi.reconnect();
    delay(5000);
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(20000);

    bool ok = http.begin(client, "https://api.line.me/v2/bot/message/push");

    if (!ok) {
      Serial.println("HTTP begin failed");
      return;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + CHANNEL_ACCESS_TOKEN);

    StaticJsonDocument<512> doc;
    doc["to"] = USER_ID;

    JsonArray messages = doc.createNestedArray("messages");
    JsonObject textMessage = messages.createNestedObject();
    textMessage["type"] = "text";
    textMessage["text"] = message;

    String payload;
    serializeJson(doc, payload);

    Serial.println("\nPayload:");
    Serial.println(payload);

    int httpCode = http.POST(payload);

    Serial.print("LINE Response code: ");
    Serial.println(httpCode);
    Serial.println(http.getString());

    http.end();
  } else {
    Serial.println("Cannot send LINE: WiFi not connected");
  }
}