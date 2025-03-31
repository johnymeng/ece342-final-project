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


#include <WiFi.h>
#include <HTTPClient.h>
#include <HomeSpan.h>
#include "LED.h"
#include "DEV_RELAY.h"

/*------------GLOBAL VARIABLES------------*/
struct AirQualityData {
  float temperature = 0.0;
  float pm25 = 0.0;
  float pm10 = 0.0;
  float humidity = 0.0;
  bool newDataAvailable = false;
};

struct HomeKitCharacteristics {
  SpanCharacteristic *airQuality = nullptr;
  SpanCharacteristic *pm25 = nullptr;
  SpanCharacteristic *pm10 = nullptr;
  SpanCharacteristic *temperature = nullptr;
  SpanCharacteristic *humidity = nullptr;

  SpanCharacteristic *temperature_sens = nullptr;
  SpanCharacteristic *humidity_sens = nullptr;
};

HomeKitCharacteristics hkChars;

AirQualityData aqData;
SemaphoreHandle_t dataMutex;  // New mutex for sensor data

// Task handles
TaskHandle_t uartTaskHandle;
TaskHandle_t httpTaskHandle;

// WiFi Configuration
const char* ssid = "meshAirsonics_R7F8_2.4G";
const char* password = "rprmwk9899";
const char* serverAddress = "http://10.88.111.4:10000/data";

// const char* ssid = "张懿鸣的iPhone";
// const char* password = "22222222";
// const char* serverAddress = "http://172.20.10.10:10000/data";

// Sensor Configuration
const int sensorPin = 34;
const TickType_t sensorInterval = 5000 / portTICK_PERIOD_MS;

// Mutex for shared resources (WiFi/Serial)
SemaphoreHandle_t wifiMutex;


/*------------FUNCTION PROTOTYPES------------*/
void homekitTask(void *pvParameters);
void sensorTask(void *pvParameters);
uint8_t getAirQualityIndex(float pm25);

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 16, 17); // RX: 16, TX: 17 for UART communication
  
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

    // //air quality sensor
    // new SpanAccessory();
    // new Service::AccessoryInformation();
    //   new Characteristic::Name("Air Quality Sensor");
    //   new Characteristic::Manufacturer("ECE342");
    //   new Characteristic::SerialNumber("AQ-001");
    // new Service::AirQualitySensor();
    //   hkChars.airQuality = new Characteristic::AirQuality(1); // Initial value: UNKNOWN
    //   hkChars.pm25 = new Characteristic::PM25Density(10);           // PM2.5
    //   hkChars.pm10 = new Characteristic::PM10Density(11);            // PM10
    //   hkChars.temperature = new Characteristic::CurrentTemperature(51);
    //   hkChars.humidity = new Characteristic::CurrentRelativeHumidity(50);

    // //temperature sensor
    //   new SpanAccessory();
    // new Service::AccessoryInformation();
    //   new Characteristic::Name("Temperature Sensor");
    // new Service::TemperatureSensor();
    //   hkChars.temperature_sens = new Characteristic::CurrentTemperature(51);

    // //humidity sensor
    //   new SpanAccessory();
    // new Service::AccessoryInformation();
    //   new Characteristic::Name("Humidity Sensor");
    // new Service::HumiditySensor();
    //   hkChars.humidity_sens = new Characteristic::CurrentRelativeHumidity(50);

  // Create mutex for shared resources
  wifiMutex = xSemaphoreCreateMutex();
  dataMutex = xSemaphoreCreateMutex();

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
    uartTask, 
    "UART_Task", 
    4096, 
    NULL, 
    1,  // Low priority
    &uartTaskHandle,
    1   // Core 1
  );

  xTaskCreatePinnedToCore(
    sensorTask,
    "HTTP_Task",
    8192,
    NULL,
    1,  // Same priority
    &httpTaskHandle,
    1   // Core 1
  );

  vTaskDelete(NULL);
}

void loop() {} // Not used

// High priority HomeKit control on Core 0
void homekitTask(void *pvParameters) {
  for(;;) {
    homeSpan.poll();

    // xSemaphoreTake(dataMutex, portMAX_DELAY);
    // if (aqData.newDataAvailable) {
    //   hkChars.airQuality->setVal(getAirQualityIndex(aqData.pm25));
    //   hkChars.pm25->setVal(aqData.pm25);
    //   hkChars.pm10->setVal(aqData.pm10);
    //   hkChars.temperature->setVal(aqData.temperature);
    //   hkChars.humidity->setVal(aqData.humidity);

    //   hkChars.temperature_sens = hkChars.temperature;
    //   hkChars.humidity_sens = hkChars.humidity;
    //   // hkChars.statusActive->setVal(1); // FUNCTIONING
    //   aqData.newDataAvailable = false;
    // }
    // xSemaphoreGive(dataMutex);

    vTaskDelay(1);  // Yield to other tasks
  }
}

void uartTask(void *pvParameters) {
  for(;;) {
    // Send READY message
    Serial2.println("READY");
    
    // Wait for response with timeout
    unsigned long startTime = millis();
    String response;
    bool dataReceived = false;
    
    while(millis() - startTime < 1000) {
      if(Serial2.available() > 0) {
        response = Serial2.readStringUntil('\n');
        dataReceived = true;
        break;
      }
    }
    
    if(dataReceived) {
      // Parse data with proper error checking
      int comma1 = response.indexOf(',');
      int comma2 = response.indexOf(',', comma1 + 1);
      int comma3 = response.indexOf(',', comma2 + 1);
      
      if(comma1 != -1 && comma2 != -1 && comma3 != -1) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        aqData.temperature = response.substring(0, comma1).toFloat();
        aqData.pm25 = response.substring(comma1+1, comma2).toFloat();
        aqData.pm10 = response.substring(comma2+1, comma3).toFloat();
        aqData.humidity = response.substring(comma3+1).toFloat();
        aqData.newDataAvailable = true;
        xSemaphoreGive(dataMutex);
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(5000));  // 5-second interval
  }
}

// Low priority sensor data transmission on Core 1
void sensorTask(void *pvParameters) {
  for(;;) {
    if(xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
      if(WiFi.status() == WL_CONNECTED) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        bool hasData = aqData.newDataAvailable;
        float temp = aqData.temperature;
        float pm25 = aqData.pm25;
        float pm10 = aqData.pm10;
        float humidity = aqData.humidity;
        aqData.newDataAvailable = false;
        xSemaphoreGive(dataMutex);
        
        if(true) {
          HTTPClient http;
          WiFiClient client;
          
          if(http.begin(client, serverAddress)) {
            http.addHeader("Content-Type", "application/json");
            String payload = "{\"temperature\":" + String(temp) + 
                            ",\"pm25\":" + String(pm25) + 
                            ",\"pm10\":" + String(pm10) + 
                            ",\"humidity\":" + String(humidity) + "}";
            int httpResponse = http.POST(payload);
            
            // Serial.printf("HTTP Response: %d\n", httpResponse);
            http.end();
          }
        }
      }
      xSemaphoreGive(wifiMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));  // 5-second interval
  }
}

uint8_t getAirQualityIndex(float pm25) {
  if (pm25 <= 12.0) return 1;      // EXCELLENT
  if (pm25 <= 35.4) return 2;      // GOOD
  if (pm25 <= 55.4) return 3;      // FAIR
  if (pm25 <= 150.4) return 4;     // INFERIOR
  return 5;                        // POOR (max)
}

// #include <WiFi.h>
// #include <HTTPClient.h>
// #include <HomeSpan.h>
// #include "LED.h"
// #include "DEV_RELAY.h"

// /*------------GLOBAL VARIABLES------------*/
// struct AirQualityData {
//   float temperature = 0.0;
//   float pm25 = 0.0;
//   float pm10 = 0.0;
//   float humidity = 0.0;
//   bool newDataAvailable = false;
// };

// struct HomeKitCharacteristics {
//   SpanCharacteristic *airQuality = nullptr;
//   SpanCharacteristic *pm25 = nullptr;
//   SpanCharacteristic *pm10 = nullptr;
//   SpanCharacteristic *temperature_sens = nullptr;
//   SpanCharacteristic *humidity_sens = nullptr;
//   SpanCharacteristic *led = nullptr;
//   SpanCharacteristic *relay = nullptr;
// };

// HomeKitCharacteristics hkChars;

// AirQualityData aqData;
// SemaphoreHandle_t dataMutex;
// SemaphoreHandle_t wifiMutex;

// // WiFi Configuration
// const char* ssid = "meshAirsonics_R7F8_2.4G";
// const char* password = "rprmwk9899";
// const char* serverAddress = "http://10.88.111.4:10000/data";

// /*------------FUNCTION PROTOTYPES------------*/
// void homekitTask(void *pvParameters);
// void uartTask(void *pvParameters);
// void httpTask(void *pvParameters);
// uint8_t getAirQualityIndex(float pm25);

// void setup() {
//   Serial.begin(115200);
//   Serial2.begin(115200, SERIAL_8N1, 16, 17);
  
//   // Initialize WiFi
//   WiFi.begin(ssid, password);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }

//   // Initialize HomeSpan
//   homeSpan.setPairingCode("11122333");
//   homeSpan.setQRID("111-22-333");
//   homeSpan.begin(Category::Bridges, "HomeSpan Bridge");

//   // Create HomeKit accessories
//   new SpanAccessory();
//     new Service::AccessoryInformation();
//       new Characteristic::Identify();
//       new Characteristic::Name("Internal LED");
//     new LED(2);

    
//   // Create Relay Accessory
//   // new SpanAccessory();
//   //   new Service::AccessoryInformation();
//   //     new Characteristic::Name("Relay");
//   //     new Characteristic::Manufacturer("ECE342");
//   //   new Service::Switch();
//   //     hkChars.relay = new Characteristic::On(false);
//   //   new DEV_RELAY(17);  // Physical relay control

//   // Create Air Quality Sensor Accessory
//   // new SpanAccessory();
//   //   new Service::AccessoryInformation();
//   //     new Characteristic::Name("Air Quality Sensor");
//   //     new Characteristic::Manufacturer("ECE342");
//   //   new Service::AirQualitySensor();
//   //     hkChars.airQuality = new Characteristic::AirQuality(1);
//   //     hkChars.pm25 = new Characteristic::PM25Density(0);
//   //     hkChars.pm10 = new Characteristic::PM10Density(0);

//   // Create Temperature Sensor Accessory
//   // new SpanAccessory();
//   //   new Service::AccessoryInformation();
//   //     new Characteristic::Name("Temperature Sensor");
//   //     new Characteristic::Manufacturer("ECE342");
//   //   new Service::TemperatureSensor();
//   //     hkChars.temperature_sens = new Characteristic::CurrentTemperature(0);

//   // // Create Humidity Sensor Accessory
//   // new SpanAccessory();
//   //   new Service::AccessoryInformation();
//   //     new Characteristic::Name("Humidity Sensor");
//   //     new Characteristic::Manufacturer("ECE342");
//   //   new Service::HumiditySensor();
//   //     hkChars.humidity_sens = new Characteristic::CurrentRelativeHumidity(0);

//   // Create mutexes
//   wifiMutex = xSemaphoreCreateMutex();
//   dataMutex = xSemaphoreCreateMutex();

//   // Create tasks
//   xTaskCreatePinnedToCore(homekitTask, "HomeKit", 10000, NULL, 2, NULL, 0);
//   xTaskCreatePinnedToCore(uartTask, "UART_Task", 4096, NULL, 1, NULL, 1);
//   xTaskCreatePinnedToCore(httpTask, "HTTP_Task", 8192, NULL, 1, NULL, 1);
  
//   vTaskDelete(NULL);
// }

// void loop() {}

// void homekitTask(void *pvParameters) {
//   for(;;) {
//     homeSpan.poll();
    
//     xSemaphoreTake(dataMutex, portMAX_DELAY);
//     if(aqData.newDataAvailable) {
//       // Update Air Quality Sensor
//       hkChars.airQuality->setVal(getAirQualityIndex(aqData.pm25));
//       hkChars.pm25->setVal(aqData.pm25);
//       hkChars.pm10->setVal(aqData.pm10);
      
//       // Update separate temperature/humidity sensors
//       hkChars.temperature_sens->setVal(aqData.temperature);
//       hkChars.humidity_sens->setVal(aqData.humidity);
      
//       aqData.newDataAvailable = false;
//     }
//     xSemaphoreGive(dataMutex);
    
//     vTaskDelay(pdMS_TO_TICKS(1));
//   }
// }

// void uartTask(void *pvParameters) {
//   for(;;) {
//     Serial2.println("READY");
    
//     unsigned long startTime = millis();
//     String response;
//     bool dataReceived = false;
    
//     while(millis() - startTime < 1000) {
//       if(Serial2.available() > 0) {
//         response = Serial2.readStringUntil('\n');
//         dataReceived = true;
//         break;
//       }
//     }
    
//     if(dataReceived) {
//       int comma1 = response.indexOf(',');
//       int comma2 = response.indexOf(',', comma1 + 1);
//       int comma3 = response.indexOf(',', comma2 + 1);
      
//       if(comma1 != -1 && comma2 != -1 && comma3 != -1) {
//         xSemaphoreTake(dataMutex, portMAX_DELAY);
//         aqData.temperature = response.substring(0, comma1).toFloat();
//         aqData.pm25 = response.substring(comma1+1, comma2).toFloat();
//         aqData.pm10 = response.substring(comma2+1, comma3).toFloat();
//         aqData.humidity = response.substring(comma3+1).toFloat();
//         aqData.newDataAvailable = true;
//         xSemaphoreGive(dataMutex);
//       }
//     }
    
//     vTaskDelay(pdMS_TO_TICKS(5000));
//   }
// }

// void httpTask(void *pvParameters) {
//   for(;;) {
//     if(xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
//       if(WiFi.status() == WL_CONNECTED) {
//         xSemaphoreTake(dataMutex, portMAX_DELAY);
//         float temp = aqData.temperature;
//         float pm25 = aqData.pm25;
//         float pm10 = aqData.pm10;
//         float humidity = aqData.humidity;
//         xSemaphoreGive(dataMutex);
        
//         HTTPClient http;
//         WiFiClient client;
        
//         if(http.begin(client, serverAddress)) {
//           String payload = "{\"temperature\":" + String(temp) + 
//                           ",\"pm25\":" + String(pm25) + 
//                           ",\"pm10\":" + String(pm10) + 
//                           ",\"humidity\":" + String(humidity) + "}";
//           http.addHeader("Content-Type", "application/json");
//           http.POST(payload);
//           http.end();
//         }
//       }
//       xSemaphoreGive(wifiMutex);
//     }
//     vTaskDelay(pdMS_TO_TICKS(5000));
//   }
// }

// uint8_t getAirQualityIndex(float pm25) {
//   if(pm25 <= 12.0) return 1;
//   if(pm25 <= 35.4) return 2;
//   if(pm25 <= 55.4) return 3;
//   if(pm25 <= 150.4) return 4;
//   return 5;
// }