#include <WiFi.h>
#include <HTTPClient.h>
#include <HomeSpan.h>
#include "LED.h"
#include "DEV_RELAY.h"

#define HISTORY_SIZE 10  // Number of past data points to use for prediction

/*------------GLOBAL VARIABLES------------*/
float pm25_history[HISTORY_SIZE] = {0};  // Circular buffer for PM2.5
int history_index = 0;
bool history_full = false;
// WiFi Configuration
// const char* ssid = "张懿鸣的iPhone";
// const char* password = "22222222";
// const char* serverAddress = "http://172.20.10.10:10000/data";

const char* ssid = "meshAirsonics_R7F8_2.4G";
const char* password = "rprmwk9899";
const char* serverAddress = "http://10.88.111.23:10000/data";

const char* emailServerAddress = "http://172.20.10.10:10000/send-email";

float predicted_AQI = 0.0;  // Global variable to store the predicted PM2.5 value

/*------------STRUCTS------------*/
struct AirQualityData {
  float temperature = 0.0;
  float pm25 = 0.0;
  float pm10 = 0.0;
  float humidity = 0.0;
  bool newDataAvailable = false;
};

struct AQIBreakpoint {
    float concLo;
    float concHi;
    uint8_t aqiLo;
    uint8_t aqiHi;
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

AQIBreakpoint breakpoints[] = {
        {0.0, 12.0, 0, 50},
        {12.1, 35.4, 51, 100},
        {35.5, 55.4, 101, 150},
        {55.5, 150.4, 151, 200},
        {150.5, 250.4, 201, 300},
        {250.5, 500.4, 301, 500}
    };

HomeKitCharacteristics hkChars;
AirQualityData aqData;

/*------------Semaphore/Tasks------------*/
SemaphoreHandle_t dataMutex;  // Mutex for sensor data
TaskHandle_t httpTaskHandle;
SemaphoreHandle_t wifiMutex; // Mutex for shared resources (WiFi)
TaskHandle_t predictionTaskHandle;
TaskHandle_t emailTaskHandle;

/*------------FUNCTION PROTOTYPES------------*/
void homekitTask(void *pvParameters);
void sendWebsiteData(void *pvParameters);
uint8_t getAirQualityIndex(float pm25);
void recAQData(void *pvParameters);
uint8_t calculateAQI(float conc, AQIBreakpoint breakpoints[], size_t size);
void emailTask(void *pvParameters);

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

  // Create HomeKit accessories (unchanged)
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Internal LED");
    new LED(2);

  new SpanAccessory();
    new Service::AccessoryInformation();    
      new Characteristic::Identify();
      new Characteristic::Name("LED");
    new LED(2);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name("Air Quality Sensor");
      new Characteristic::Manufacturer("ECE342");
      new Characteristic::SerialNumber("AQ-001");
    new Service::AirQualitySensor();
      hkChars.airQuality = new Characteristic::AirQuality(1);
      hkChars.pm25 = new Characteristic::PM25Density(10);
      hkChars.pm10 = new Characteristic::PM10Density(11);
      hkChars.temperature = new Characteristic::CurrentTemperature(51);
      hkChars.humidity = new Characteristic::CurrentRelativeHumidity(50);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name("Temperature");
    new Service::TemperatureSensor();
      hkChars.temperature_sens = new Characteristic::CurrentTemperature(51);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name("Humidity");
    new Service::HumiditySensor();
      hkChars.humidity_sens = new Characteristic::CurrentRelativeHumidity(50);

  // Create mutex for shared resources
  wifiMutex = xSemaphoreCreateMutex();
  dataMutex = xSemaphoreCreateMutex();

  //support apple homekit
  xTaskCreatePinnedToCore(
    homekitTask, 
    "HomeKit", 
    8192, 
    NULL, 
    2, 
    NULL, 
    0
  );

  //send AQI data to webstie
  xTaskCreatePinnedToCore(
    sendWebsiteData,
    "HTTP_Task",
    8192,
    NULL,
    1,
    &httpTaskHandle,
    1
  );

  //receive AQI data from ESP8266
  xTaskCreatePinnedToCore(
    recAQData,
    "HTTP_Task",
    8192,
    NULL,
    1,
    &httpTaskHandle,
    1
  );

  //predict AQI using Linear Regression
   xTaskCreatePinnedToCore(
    predictTask, 
    "PredictionTask", 
    8192, 
    NULL, 
    1,  // Medium priority
    &predictionTaskHandle, 
    1   // Runs on core 1
  );

  //send AQI email summary every 10 mins
  xTaskCreatePinnedToCore(
    emailTask,
    "EmailTask",
    8192,
    NULL,
    0,  // Lowest priority
    &emailTaskHandle,
    1   // Core 1
  );

  vTaskDelete(NULL);
}


void predictTask(void *pvParameters) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10000));  // Run prediction every 10 seconds

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    
    // Store new data point in circular buffer
    pm25_history[history_index] = aqData.pm25;
    history_index = (history_index + 1) % HISTORY_SIZE;
    if (history_index == 0) history_full = true;

    // Ensure we have enough data points
    int numPoints = history_full ? HISTORY_SIZE : history_index;
    if (numPoints < 2) {
      xSemaphoreGive(dataMutex);
      continue;
    }

    // Perform linear regression (least squares)
    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    for (int i = 0; i < numPoints; i++) {
      sumX += i;
      sumY += pm25_history[i];
      sumXY += i * pm25_history[i];
      sumX2 += i * i;
    }
    
    float slope = (numPoints * sumXY - sumX * sumY) / (numPoints * sumX2 - sumX * sumX);
    float intercept = (sumY - slope * sumX) / numPoints;

    // Predict next PM2.5 value
    float prediction = slope * numPoints + intercept;
    Serial.printf("Predicted PM2.5: %.2f\n", prediction);

    // Update global variable with the predicted value
    // predicted_AQI = prediction;
    predicted_AQI = calculateAQI(prediction, breakpoints, sizeof(breakpoints) / sizeof(breakpoints[0]));

    // Update HomeKit with prediction (Optional)
    hkChars.pm25->setVal(prediction);

    xSemaphoreGive(dataMutex);
  }
}

void loop() {}

void homekitTask(void *pvParameters) {
  for(;;) {
    homeSpan.poll();

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (aqData.newDataAvailable) {
      hkChars.airQuality->setVal(getAirQualityIndex(aqData.pm25));
      hkChars.pm25->setVal(aqData.pm25);
      hkChars.pm10->setVal(aqData.pm10);
      hkChars.temperature->setVal(aqData.temperature);
      hkChars.humidity->setVal(aqData.humidity);

      hkChars.temperature_sens = hkChars.temperature;
      hkChars.humidity_sens = hkChars.humidity;
      aqData.newDataAvailable = false;
    }
    xSemaphoreGive(dataMutex);

    vTaskDelay(1);
  }
}

//send air quality data to website
void sendWebsiteData(void *pvParameters) {
  for(;;) {
    if(xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
      if(WiFi.status() == WL_CONNECTED) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        bool hasData = aqData.newDataAvailable;
        float temp = aqData.temperature;
        float pm25 = aqData.pm25;
        float pm10 = aqData.pm10;
        float humidity = aqData.humidity;
        // float prediction = predicted_pm25;  // Get the predicted PM2.5 value
        float prediction = predicted_AQI;
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
                            ",\"humidity\":" + String(humidity) + 
                            ",\"predicted_pm25\":" + String(prediction) + "}";  // Include predicted PM2.5
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
void recAQData(void *pvParameters) {
  for(;;) {
    if(xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
      if(WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        WiFiClient client;
        if(http.begin(client, serverAddress)) {
          int httpResponse = http.GET();
          if(httpResponse == HTTP_CODE_OK) {
            String response = http.getString();
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
          http.end();
        }
      }
      xSemaphoreGive(wifiMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

/*------------EMAIL TASK IMPLEMENTATION------------*/
void emailTask(void *pvParameters) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(600000));  // 10 minute interval

    // Collect data safely
    float temp, pm25, pm10, humidity, predicted;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    temp = aqData.temperature;
    pm25 = aqData.pm25;
    pm10 = aqData.pm10;
    humidity = aqData.humidity;
    predicted = predicted_AQI;
    xSemaphoreGive(dataMutex);

    // Prepare JSON payload
    String payload = "{\"temperature\":" + String(temp) + 
                    ",\"pm25\":" + String(pm25) + 
                    ",\"pm10\":" + String(pm10) + 
                    ",\"humidity\":" + String(humidity) + 
                    ",\"predicted_pm25\":" + String(predicted) + "}";

    // Send HTTP request
    if(xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
      if(WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        WiFiClient client;
        
        if(http.begin(client, emailServerAddress)) {
          http.addHeader("Content-Type", "application/json");
          int httpResponse = http.POST(payload);
          
          if(httpResponse == HTTP_CODE_OK) {
            Serial.println("Email summary request sent successfully");
          } else {
            Serial.printf("Email request failed. Status: %d\n", httpResponse);
          }
          http.end();
        }
      }
      xSemaphoreGive(wifiMutex);
    }
  }
}

// uint8_t getAirQualityIndex(float pm25) {
//   if (pm25 <= 12.0) return 1;
//   if (pm25 <= 35.4) return 2;
//   if (pm25 <= 55.4) return 3;
//   if (pm25 <= 150.4) return 4;
//   return 5;
// }

uint8_t calculateAQI(float conc, AQIBreakpoint breakpoints[], size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (conc >= breakpoints[i].concLo && conc <= breakpoints[i].concHi) {
            return ((breakpoints[i].aqiHi - breakpoints[i].aqiLo) * (conc - breakpoints[i].concLo) /
                    (breakpoints[i].concHi - breakpoints[i].concLo)) + breakpoints[i].aqiLo;
        }
    }
    return 500;  // Default to max AQI if value is out of range
}

uint8_t getAirQualityIndex(float pm25) {
    return calculateAQI(pm25, breakpoints, sizeof(breakpoints) / sizeof(breakpoints[0]));
}
