#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// =====================================
// MAC ADDRESS ของ ESP32 ตัวรับ
// =====================================
uint8_t receiverMAC[] = {
  0xA0, 0xB7, 0x65,
  0x05, 0x67, 0x7C
};



// =====================================
// SETUP
// =====================================
void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println("================================");
  Serial.println("ESP32-S2 ESP-NOW SENDER");
  Serial.println("================================");

  // WIFI STA MODE
  WiFi.mode(WIFI_STA);

  // IMPORTANT
  // CHANNEL ต้องตรงกับฝั่งรับ
  // ตอนนี้ router = channel 3
  esp_wifi_set_channel(3, WIFI_SECOND_CHAN_NONE);

  // =====================================
  // ESP-NOW INIT
  // =====================================
  if (esp_now_init() != ESP_OK) {

    Serial.println("ESP-NOW INIT FAILED");
    return;
  }

  Serial.println("ESP-NOW READY");



  // =====================================
  // PEER CONFIG
  // =====================================
  esp_now_peer_info_t peerInfo = {};

  memcpy(peerInfo.peer_addr,
         receiverMAC,
         6);

  peerInfo.channel = 3;
  peerInfo.encrypt = false;

  // ADD PEER
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {

    Serial.println("PAIR FAILED");
    return;
  }

  Serial.println("PAIR SUCCESS");

  Serial.println("");
  Serial.println("P = PAUSE");
  Serial.println("R = RESUME");
}

// =====================================
// LOOP
// =====================================
void loop() {

  if (Serial.available()) {

    char c = Serial.read();

    // =================================
    // PAUSE
    // =================================
    if (c == 'P') {

      char msg[] = "PAUSE";

      esp_err_t result =
        esp_now_send(receiverMAC,
                     (uint8_t *)msg,
                     strlen(msg));

      if (result == ESP_OK) {

        Serial.println("SEND PAUSE");
      }
      else {

        Serial.println("SEND FAILED");
      }
    }

    // =================================
    // RESUME
    // =================================
    if (c == 'R') {

      char msg[] = "RESUME";

      esp_err_t result =
        esp_now_send(receiverMAC,
                     (uint8_t *)msg,
                     strlen(msg));

      if (result == ESP_OK) {

        Serial.println("SEND RESUME");
      }
      else {

        Serial.println("SEND FAILED");
      }
    }
  }
}