#include <WiFi.h>
#include <HTTPClient.h>
#include <HomeSpan.h>
#include "LED.h"
#include "DEV_RELAY.h"
#include <ESP_Mail_Client.h>

#define HISTORY_SIZE 10  // Number of past data points to use for prediction
#define SMTP_PORT esp_mail_smtp_port_587 //used for sending to outlook.com
#define SMTP_HOST "smtp.gmail.com"
// #define SMTP_PORT 587  // Gmail uses 587 
#define AUTHOR_EMAIL "johnnymeng27@gmail.com"
#define RECIPIENT_EMAIL "johnnymeng27@gmail.com"

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
const char* serverAddress = "http://10.88.111.20:10000/data";
const char* emailServerAddress = "http://172.20.10.10:10000/send-email";

const char* dataAddress = "http://10.88.111.20:8000/data";

unsigned long randomSeedValue = 0;
float predicted_AQI = 0.0;  // Global variable to store the predicted PM2.5 value

/*------------STRUCTS------------*/
struct AirQualityData {
  float temperature = 0.0;
  float pm25 = 0.0;
  float pm10 = 0.0;
  float humidity = 0.0;
  float co2 = 0.0;
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
  SpanCharacteristic *co2 = nullptr;

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

  randomSeed(analogRead(0));
  
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
      new Characteristic::Name("Temperaturetwo");
    new Service::TemperatureSensor();
      hkChars.temperature = new Characteristic::CurrentTemperature(51);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name("CarbdonDioxide");
    new Service::CarbonDioxideSensor();
      hkChars.co2 = new Characteristic::CarbonDioxideLevel(400);

    new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name("CarbonDetector");
    new Service::CarbonDioxideSensor();
      hkChars.co2 = new Characteristic::CarbonDioxideLevel(400);

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
    1, 
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
    2,  // Lowest priority
    &emailTaskHandle,
    0   // Core 1
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

//poll and update homekit widgets
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

      hkChars.co2->setVal(aqData.co2);
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
        float cur_AQI = calculateAQI(pm25,breakpoints, sizeof(breakpoints) / sizeof(breakpoints[0]));
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
                            ",\"pm25\":" + String(cur_AQI) + 
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

// void recAQData(void *pvParameters) {
//   for(;;) {
//     // Generate random values instead of HTTP request
//     xSemaphoreTake(dataMutex, portMAX_DELAY);
//     aqData.temperature = random(200, 310) / 10.0;  // 20.0-31.0°C
//     aqData.pm25 = random(0, 300);                   // 0-300 µg/m³
//     aqData.pm10 = random(0, 500);                   // 0-500 µg/m³
//     aqData.humidity = random(300, 710) / 10.0;      // 30.0-70.0%
//     aqData.newDataAvailable = true;
//     xSemaphoreGive(dataMutex);

//     Serial.printf("Generated test values: %.1f°C, PM2.5: %.1f, PM10: %.1f, Humidity: %.1f%%\n",
//                  aqData.temperature, aqData.pm25, aqData.pm10, aqData.humidity);

//     vTaskDelay(pdMS_TO_TICKS(5000));  // Keep 5-second interval
//   }
// }

//receive AQI data from ESP8266
void recAQData(void *pvParameters) {
  for(;;) {
    if(xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
      if(WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        WiFiClient client;
        if(http.begin(client, dataAddress)) {
          int httpResponse = http.GET();
          if(httpResponse == HTTP_CODE_OK) {
            String response = http.getString();
            int comma1 = response.indexOf(',');
            int comma2 = response.indexOf(',', comma1 + 1);
            int comma3 = response.indexOf(',', comma2 + 1);
            int comma4 = response.indexOf(',', comma3 + 1);

            if(comma1 != -1 && comma2 != -1 && comma3 != -1) {
              xSemaphoreTake(dataMutex, portMAX_DELAY);
              aqData.temperature = response.substring(0, comma1).toFloat();
              aqData.pm25 = response.substring(comma1+1, comma2).toFloat();
              aqData.pm10 = response.substring(comma2+1, comma3).toFloat();
              aqData.humidity = response.substring(comma3+1, comma4).toFloat();
              aqData.co2 = response.substring(comma4+1).toFloat();
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

//email user AQI summary every 10 minutes
void emailTask(void *pvParameters) {
  SMTPSession smtp;
  Session_Config config;

  config.server.host_name = SMTP_HOST;
  config.server.port = SMTP_PORT;
  config.login.email = AUTHOR_EMAIL;
  config.login.password = AUTHOR_PASSWORD;
  config.login.user_domain = "127.0.0.1";
  config.secure.mode = esp_mail_secure_mode_ssl_tls; // Use SSL/TLS

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(600000)); // 10-minute delay

    // Collect data safely
    float temp, pm25, pm10, humidity, co2;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    temp = aqData.temperature;
    pm25 = aqData.pm25;
    pm10 = aqData.pm10;
    humidity = aqData.humidity;
    co2 = aqData.co2;
    xSemaphoreGive(dataMutex);

    // Create email message
    SMTP_Message message;
    message.sender.name = "ESP32 Air Quality Monitor";
    message.sender.email = AUTHOR_EMAIL;
    message.subject = "Air Quality Report";
    message.addRecipient("Recipient", RECIPIENT_EMAIL);

    // Format email body
    String body = "Air Quality Data:\n";
    body += "Temperature: " + String(temp) + "°C\n";
    body += "PM2.5: " + String(pm25) + " µg/m³\n";
    body += "PM10: " + String(pm10) + " µg/m³\n";
    body += "Humidity: " + String(humidity) + "%\n";
    body += "CO2: " + String(co2) + " ppm";
    message.text.content = body;

    // Send email
    smtp.debug(1);
    if (!smtp.connect(&config)) {
      Serial.println("SMTP connection failed");
      continue;
    }
    if (!MailClient.sendMail(&smtp, &message)) {
      Serial.println("Email sending failed");
    } else {
      Serial.println("Email sent successfully!");
    }
  }
}

// void emailTask() {
//   // Collect data safely
//   float temp, pm25, pm10, humidity, co2;
//   xSemaphoreTake(dataMutex, portMAX_DELAY);
//   temp = aqData.temperature;
//   pm25 = aqData.pm25;
//   pm10 = aqData.pm10;
//   humidity = aqData.humidity;
//   co2 = aqData.co2;
//   xSemaphoreGive(dataMutex);

//   // Configure SMTP session
//   Session_Config config;
//   config.server.host_name = SMTP_HOST;
//   config.server.port = SMTP_PORT;
//   config.login.email = AUTHOR_EMAIL;
//   config.login.password = AUTHOR_PASSWORD;
//   config.login.user_domain = "127.0.0.1";  // Required but not used
//   config.secure.mode = esp_mail_secure_mode_ssl_tls;  // For Gmail

//   // Create email message
//   SMTP_Message message;
//   message.sender.name = "ESP32 Air Quality Monitor";
//   message.sender.email = AUTHOR_EMAIL;
//   message.subject = "Air Quality Report";
  
//   // Format email body with sensor data
//   String body = "Air Quality Data:\n\n";
//   body += "Temperature: " + String(temp) + "°C\n";
//   body += "PM2.5: " + String(pm25) + " µg/m³\n";
//   body += "PM10: " + String(pm10) + " µg/m³\n";
//   body += "Humidity: " + String(humidity) + "%\n";
//   body += "CO2: " + String(co2) + " ppm";
  
//   message.text.content = body;
//   message.text.charSet = "utf-8";
//   message.addRecipient("Recipient", RECIPIENT_EMAIL);

//   // Send email
//   SMTPSession smtp;
//   smtp.debug(1);  // Enable debug output

//   if (!smtp.connect(&config)) {
//     Serial.println("SMTP connection failed");
//     return;
//   }

//   if (!MailClient.sendMail(&smtp, &message)) {
//     Serial.println("Email sending failed");
//   } else {
//     Serial.println("Email sent successfully!");
//   }
// }
// void emailTask(void *pvParameters) {
//   for (;;) {
//     vTaskDelay(pdMS_TO_TICKS(600000));  // 10-minute interval

//     // Collect data safely
//     float temp, pm25, pm10, humidity, predicted;
//     xSemaphoreTake(dataMutex, portMAX_DELAY);
//     temp = aqData.temperature;
//     pm25 = aqData.pm25;
//     pm10 = aqData.pm10;
//     humidity = aqData.humidity;
//     predicted = predicted_AQI;
//     xSemaphoreGive(dataMutex);

//     // Configure SMTP
//     smtpData.setLogin("smtp.gmail.com", 465, "johnnymeng27@gmail.com", "fluffypandaz1");
//     smtpData.setSender("ESP32", "johnnymeng27@gmail.com");
//     smtpData.setRecipient("john.meng@mail.utoronto.ca");
//     smtpData.setSubject("Air Quality Report");
//     smtpData.setMessage("Temperature: " + String(temp) + "\n" +
//                         "PM2.5: " + String(pm25) + "\n" +
//                         "PM10: " + String(pm10) + "\n" +
//                         "Humidity: " + String(humidity) + "\n" +
//                         "Predicted PM2.5: " + String(predicted));

//     // Send email
//     if (MailClient.sendMail(smtpData)) {
//       Serial.println("Email sent successfully");
//     } else {
//       Serial.println("Email failed");
//     }
//   }
// }

// uint8_t getAirQualityIndex(float pm25) {
//   if (pm25 <= 12.0) return 1;
//   if (pm25 <= 35.4) return 2;
//   if (pm25 <= 55.4) return 3;
//   if (pm25 <= 150.4) return 4;
//   return 5;
// }

//calculate the AQI using standard formula 
uint8_t calculateAQI(float conc, AQIBreakpoint breakpoints[], size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (conc >= breakpoints[i].concLo && conc <= breakpoints[i].concHi) {
            return ((breakpoints[i].aqiHi - breakpoints[i].aqiLo) * (conc - breakpoints[i].concLo) /
                    (breakpoints[i].concHi - breakpoints[i].concLo)) + breakpoints[i].aqiLo;
        }
    }
    return 500;  // Default to max AQI if value is out of range
}

//get the airquality index based on homespan library 
uint8_t getAirQualityIndex(float pm25) {
    uint8_t AQI = calculateAQI(pm25, breakpoints, sizeof(breakpoints) / sizeof(breakpoints[0]));

    if (AQI <= 50) return 1;
    if (AQI <= 100) return 2;
    if (AQI <= 150) return 3;
    if (AQI <= 200) return 4;
    return 5;
}
