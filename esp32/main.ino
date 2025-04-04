#include <mutex>  
#include <HTTPClient.h>
#include "HomeSpan.h"  
#include "LED.h"
#include "DEV_RELAY.h"
#include <ESP_Mail_Client.h>
#include <driver/ledc.h>
#include <esp32-hal-ledc.h> 
#include "DacESP32.h"

#define HISTORY_SIZE 10  // Number past data for prediction
#define SMTP_PORT esp_mail_smtp_port_587 //used for sending to gmail.com
#define SMTP_HOST "smtp.gmail.com"
#define AUTHOR_EMAIL "johnnymeng27@gmail.com"
#define AUTHOR_PASSWORD "xvueceliwzdmixmp" 
#define RECIPIENT_EMAIL "johnnymeng27@gmail.com"
#define FAN_PIN 25
#define FAN_PIN_2 13
#define LEDC_CHANNEL 0
#define LEDC_RESOLUTION 8  
#define LEDC_FREQ 25000    

// Add these near your other global variables
unsigned long cpuActiveTime = 0;        // Total CPU active time (ms)
unsigned long wifiActiveTime = 0;       // Total Wi-Fi active time (ms)
unsigned long lastLogTime = 0;          // Last log timestamp
SemaphoreHandle_t timeMutex;            // Mutex for time variables

DacESP32 dac1(DAC_CHAN_0);

/*------------GLOBAL VARIABLES------------*/
float pm25_history[HISTORY_SIZE] = {0};  
int history_index = 0;
bool history_full = false;
bool wifi_connected = false;
int wifi_delay = 100000;

const char* ssid = "张懿鸣的iPhone";
const char* password = "22222222";
// const char* serverAddress = "http://172.20.10.10:10000/data";

const char* serverAddress = "http://54.86.88.38:10000/updateaqi";
const char* emailServerAddress = "http://172.20.10.10:10000/send-email";
const char* dataAddress = "http://54.86.88.38:10000/getonedata";

unsigned long randomSeedValue = 0;
float predicted_AQI = 0.0;  

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
    uint16_t aqiLo;  
    uint16_t aqiHi;  
};

struct HomeKitCharacteristics {
  SpanCharacteristic *airQuality = nullptr;
  SpanCharacteristic *pm25 = nullptr;
  SpanCharacteristic *pm10 = nullptr;
  SpanCharacteristic *temperature = nullptr;
  SpanCharacteristic *humidity = nullptr;
  SpanCharacteristic *co2 = nullptr;

   SpanCharacteristic *co2_sens = nullptr;
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
SemaphoreHandle_t dataMutex;  
TaskHandle_t httpTaskHandle;
SemaphoreHandle_t wifiMutex; 
TaskHandle_t predictionTaskHandle;
TaskHandle_t emailTaskHandle;

/*------------FUNCTION PROTOTYPES------------*/
void homekitTask(void *pvParameters);
void sendWebsiteData(void *pvParameters);
uint8_t getAirQualityIndex(float pm25);
void recAQData(void *pvParameters);
uint8_t calculateAQI(float conc, AQIBreakpoint breakpoints[], size_t size);
void emailTask(void *pvParameters);
void fanControlTask(void *pvParameters);

void logUsage() {
  if (millis() - lastLogTime >= 60000) {  // Log every 1 minute
    xSemaphoreTake(timeMutex, portMAX_DELAY);
    Serial.printf("[POWER] CPU Active: %.1f%%, Wi-Fi Active: %.1f%%\n",
      (cpuActiveTime / 60000.0) * 100,  // % of 60s
      (wifiActiveTime / 60000.0) * 100
    );
    cpuActiveTime = 0;  // Reset counters
    wifiActiveTime = 0;
    lastLogTime = millis();
    xSemaphoreGive(timeMutex);
  }
}

void setup() {
  Serial.begin(115200);

  randomSeed(analogRead(0));
  // ledc_timer_config_t timer_conf = {
  //     .speed_mode = LEDC_LOW_SPEED_MODE,
  //     .duty_resolution = LEDC_TIMER_8_BIT,
  //     .timer_num = LEDC_TIMER_0,
  //     .freq_hz = 25000,
  //     .clk_cfg = LEDC_AUTO_CLK
  // };
  // ledc_timer_config(&timer_conf);

  // ledc_channel_config_t channel_conf = {
  //     .gpio_num = FAN_PIN,
  //     .speed_mode = LEDC_LOW_SPEED_MODE,
  //     .channel = LEDC_CHANNEL_0,
  //     .timer_sel = LEDC_TIMER_0,
  //     .duty = 0,
  //     .hpoint = 0
  // };
  // ledc_channel_config(&channel_conf);
  
  //set up wifi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
  WiFi.setSleep(false);

  pinMode(FAN_PIN, OUTPUT);
  pinMode(FAN_PIN_2, OUTPUT);
  digitalWrite(FAN_PIN, LOW);
  digitalWrite(FAN_PIN_2, HIGH);

  timeMutex = xSemaphoreCreateMutex();
  
  //set up Homespan
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
      new Characteristic::Name("TemperatureTest");
    new Service::TemperatureSensor();
      hkChars.temperature_sens = new Characteristic::CurrentTemperature(51);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name("Carbon");
    new Service::CarbonDioxideSensor();
      hkChars.co2 = new Characteristic::CarbonDioxideLevel(400);

    new SpanAccessory();                        
  new Service::AccessoryInformation();     
    new Characteristic::Name("carbondata");
  new Service::CarbonDioxideSensor();      
    hkChars.co2_sens = new Characteristic::CarbonDioxideLevel(400); 


    new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name("Carbontest");
    new Service::CarbonDioxideSensor();
      hkChars.co2_sens = new Characteristic::CarbonDioxideLevel(100);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name("Humiditytest");
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
    2,
    &httpTaskHandle,
    0
  );

  //predict AQI using Linear Regression
   xTaskCreatePinnedToCore(
    predictTask, 
    "PredictionTask", 
    8192, 
    NULL, 
    2,  
    &predictionTaskHandle, 
    1   
  );

  //send AQI email summary every 10 mins
  xTaskCreatePinnedToCore(
    emailTask,
    "EmailTask",
    8192,
    NULL,
    0,  
    &emailTaskHandle,
    1   
  );

  //  xTaskCreatePinnedToCore(
  //   fanControlTask,
  //   "FanControl",
  //   4096,
  //   NULL,
  //   1,   
  //   NULL,
  //   1     
  // );

  vTaskDelete(NULL);
}

void fanControlTask(void *pvParameters) {
  uint8_t lastDuty = 0;
  
  for (;;) {
    uint8_t aqiLevel;
    
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    aqiLevel = hkChars.airQuality->getVal();
    xSemaphoreGive(dataMutex);

    uint8_t dac_value = (255 * aqiLevel) / 5;
    digitalWrite(FAN_PIN, HIGH);

    printf("DAC Output set to %d/255 %d\n", dac_value, aqiLevel);

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

//linear regression for AQI prediction
//sums up the last 10 PM2.5 vals, PM2.5*time_stamps, and PM2.5^2
//then calculates slope and intercept, and plots prediction based on prediction = mx + b
//slope = (nΣxy - ΣxΣy) / (nΣx² - (Σx)²), b = (Σy - mΣx)/n
void predictTask(void *pvParameters) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10000));  // Run prediction every 10 seconds

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    pm25_history[history_index] = aqData.pm25;
    history_index = (history_index + 1) % HISTORY_SIZE;
    if (history_index == 0) history_full = true;

    int numPoints = history_full ? HISTORY_SIZE : history_index;
    if (numPoints < 2) {
      xSemaphoreGive(dataMutex);
      continue;
    }

    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    for (int i = 0; i < numPoints; i++) {
      sumX += i;
      sumY += pm25_history[i];
      sumXY += i * pm25_history[i];
      sumX2 += i * i;
    }
    
    float slope = (numPoints * sumXY - sumX * sumY) / (numPoints * sumX2 - sumX * sumX);
    float intercept = (sumY - slope * sumX) / numPoints;
    float prediction = slope * numPoints + intercept;
    Serial.printf("Predicted AQI: %.2f\n", prediction);

    predicted_AQI = calculateAQI(prediction, breakpoints, sizeof(breakpoints) / sizeof(breakpoints[0]));
    xSemaphoreGive(dataMutex);
  }
}

void loop() {
  float voltage = 0;
  dac1.outputVoltage(voltage);
}

//poll and update homekit widgets
void homekitTask(void *pvParameters) {
  for(;;) {
    unsigned long startTime = millis();
    homeSpan.poll();
    wifi_delay++;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (aqData.newDataAvailable) {
      hkChars.airQuality->setVal(getAirQualityIndex(aqData.pm25));
      hkChars.pm25->setVal(aqData.pm25);
      hkChars.pm10->setVal(aqData.pm10);
      hkChars.temperature->setVal(aqData.temperature);
      hkChars.humidity->setVal(aqData.humidity);
      hkChars.co2->setVal(aqData.co2);


      hkChars.co2_sens->setVal(aqData.co2);
      hkChars.temperature_sens->setVal(aqData.temperature);
      hkChars.humidity_sens->setVal(aqData.humidity);

      // hkChars.co2_sens = hkChars.co2;
      // hkChars.temperature_sens = hkChars.temperature;
      // hkChars.humidity_sens = hkChars.humidity;
      aqData.newDataAvailable = false;
    }
    xSemaphoreGive(dataMutex);

    xSemaphoreTake(timeMutex, portMAX_DELAY);
    wifiActiveTime += millis() - startTime;
    cpuActiveTime += millis() - startTime;
    xSemaphoreGive(timeMutex);
     logUsage();

    vTaskDelay(1);
  }
}

//send air quality data to website
void sendWebsiteData(void *pvParameters) {
  for(;;) {
    unsigned long wifiStart = millis();
    if(xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
      if(WiFi.status() == WL_CONNECTED && wifi_delay > 10000) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        bool hasData = aqData.newDataAvailable;
        float temp = aqData.temperature;
        float pm25 = aqData.pm25;
        float pm10 = aqData.pm10;
        float humidity = aqData.humidity;
        float cur_AQI = calculateAQI(pm25,breakpoints, sizeof(breakpoints) / sizeof(breakpoints[0]));
        float prediction = predicted_AQI;
        aqData.newDataAvailable = false;
        xSemaphoreGive(dataMutex);
        
        if(true) {
          HTTPClient http;
          WiFiClient client;
          
          if(http.begin(client, serverAddress)) {
            http.addHeader("Content-Type", "application/json");
            String payload = String((int)cur_AQI) + "," + String((int)prediction);  
            int httpResponse = http.POST(payload);

            Serial.printf("website data: AQI: %.1f, predicted AQI: %.1f sent, paylod: %s\n",cur_AQI, prediction, payload);
            
            http.end();
          }
        }
      }      
      xSemaphoreGive(wifiMutex);
    }
     logUsage();  // Add this line to periodically log usage
     wifiActiveTime += millis() - wifiStart;
     cpuActiveTime += millis() - wifiStart;
    vTaskDelay(pdMS_TO_TICKS(5000));  // 5-second interval
  }
}

//receive AQI data from ESP8266
void recAQData(void *pvParameters) {
  for(;;) {
    unsigned long wifiStart = millis();
    if(xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
      if(WiFi.status() == WL_CONNECTED && wifi_delay > 10000) {
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

            Serial.printf("received values: %.1f°C, PM2.5: %.1f, PM10: %.1f, Humidity: %.1f%%, CO2: %.1f%%\n",
                 aqData.temperature, aqData.pm25, aqData.pm10, aqData.humidity, aqData.co2);
          }
          http.end();
        }
        else Serial.printf("can't connect to server!\n");
      }
      xSemaphoreGive(wifiMutex);
    }
    wifiActiveTime += millis() - wifiStart;
    cpuActiveTime += millis() - wifiStart;
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

//email user AQI summary every 10 minutes
void emailTask(void *pvParameters) {
  unsigned long wifiStart = millis();
  SMTPSession smtp;
  Session_Config config;

  config.server.host_name = SMTP_HOST;
  config.server.port = SMTP_PORT;
  config.login.email = AUTHOR_EMAIL;
  config.login.password = AUTHOR_PASSWORD;
  config.login.user_domain = "127.0.0.1";
  config.secure.mode = esp_mail_secure_mode_ssl_tls; 

  
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10000)); // 1-minute delay

    // Collect data 
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
  wifiActiveTime += millis() - wifiStart;
  cpuActiveTime += millis() - wifiStart;
   logUsage();
}

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
