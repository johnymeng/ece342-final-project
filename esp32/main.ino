#include <WiFi.h>
#include <HTTPClient.h>
#include <HomeSpan.h>
#include "LED.h"
#include "DEV_RELAY.h"

// WiFi Configuration
// const char* ssid = "meshAirsonics_R7F8_2.4G";
// const char* password = "rprmwk9899";
// const char* serverAddress = "http://10.88.111.52:10000/data";

const char* ssid = "张懿鸣的iPhone";
const char* password = "22222222";
const char* serverAddress = "http://172.20.10.10:10000/data";

// Sensor Configuration
const int sensorPin = 34;
const TickType_t sensorInterval = 5000 / portTICK_PERIOD_MS;

// Mutex for shared resources (WiFi/Serial)
SemaphoreHandle_t wifiMutex;

void homekitTask(void *pvParameters);
void sensorTask(void *pvParameters);

void setup() {
  Serial.begin(115200);
  
  // Initialize WiFi connection
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  // Initialize HomeSpan
  homeSpan.setPairingCode("11122333");
  homeSpan.setQRID("111-22-333");
  homeSpan.begin(Category::Bridges, "HomeSpan Bridge");

  // Create HomeKit accessories
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Internal LED");
    new LED(2);

  new SpanAccessory();
    new Service::AccessoryInformation();    
      new Characteristic::Identify();
      new Characteristic::Name("Relay");
    new DEV_RELAY(17);

  // Create mutex for shared resources
  wifiMutex = xSemaphoreCreateMutex();

  // Create tasks pinned to different cores
  xTaskCreatePinnedToCore(
    homekitTask,    // Task function
    "HomeKit",      // Task name
    10000,          // Stack size
    NULL,           // Parameters
    2,              // Priority (higher)
    NULL,           // Task handle
    0);             // Core 0

  xTaskCreatePinnedToCore(
    sensorTask,
    "Sensor",
    10000,
    NULL,
    1,              // Lower priority
    NULL,
    1);             // Core 1

  // Delete default Arduino loop task
  vTaskDelete(NULL);
}

void loop() {} // Not used

// High priority HomeKit control on Core 0
void homekitTask(void *pvParameters) {
  for(;;) {
    homeSpan.poll();
    vTaskDelay(1);  // Yield to other tasks
  }
}

// Low priority sensor data transmission on Core 1
void sensorTask(void *pvParameters) {
  for(;;) {
    if (xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
      if (WiFi.status() == WL_CONNECTED) {
        int sensorValue = analogRead(sensorPin);
        
        HTTPClient http;
        WiFiClient client;
        
        if (http.begin(client, serverAddress)) {
          http.addHeader("Content-Type", "application/json");
          String payload = "{\"air_quality\":" + String(sensorValue) + "}";
          int response = http.POST(payload);
          
          Serial.printf("Sensor: %d | HTTP: %d\n", sensorValue, response);
          http.end();
        }
      }
      xSemaphoreGive(wifiMutex);
    }
    vTaskDelay(sensorInterval);
  }
}