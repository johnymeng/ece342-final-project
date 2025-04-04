#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SHTC3.h>
#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

#define DEBUG 1

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define I2C_SDA D2
#define I2C_SCL D1
#define PMS_RX D5
#define PMS_TX D6
#define MHZ19B_RX D7
#define MHZ19B_TX D8

// wifi
const char* ssid = ""; // FIXME
const char* password = ""; // FIXME
String serverIP = ""; // FIXME
String endpoint = ""; // FIXME

// pms sensers require a warm-up time
unsigned long sensorStartTime = 0;
const unsigned long WIFI_CON_TIME = 10 * 1000;
const unsigned long WARM_UP_TIME = 30 * 1000;
bool pmsReadingsReady = false;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire); // display
Adafruit_SHTC3 shtc3 = Adafruit_SHTC3(); // temp humidity sensor
SoftwareSerial pmsSerial(PMS_RX, PMS_TX); // pm2.5
SoftwareSerial mhzSerial(MHZ19B_RX, MHZ19B_TX); // co2

// data storage
struct PMSData {
  int PM1_0;
  int PM2_5;
  int PM10;
};
PMSData pmsData;
int co2ppm = 0;

bool readPMSData(PMSData& data) {
  if (pmsSerial.available() >= 32) {
    uint8_t buffer[32];
    if (pmsSerial.readBytes(buffer, 32) == 32) {
      if (buffer[0] == 0x42 && buffer[1] == 0x4D) {
        data.PM1_0 = buffer[4] * 256 + buffer[5];
        data.PM2_5 = buffer[6] * 256 + buffer[7];
        data.PM10 = buffer[8] * 256 + buffer[9];
        return true;
      }
    }
    // Clear any remaining data
    while(pmsSerial.available()) {
      pmsSerial.read();
    }
  }
  return false;
}

int readCO2() {
  // Command to read CO2 value
  byte cmd[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
  byte response[9];
  mhzSerial.write(cmd, 9);
  
  delay(100);
  
  if (mhzSerial.available() >= 9) {
    mhzSerial.readBytes(response, 9);

    if (response[0] == 0xFF && response[1] == 0x86) {
      // calculate CO2 value
      int ppm = (256 * response[2]) + response[3];
      return ppm;
    }

    // remove stale data
    while(mhzSerial.available()) {
      mhzSerial.read();
    }
  } 
  return co2ppm;
}

// convert sensor data to string format for http
String formatSensorData(float temp, float humidity, int pm25, int pm10, int co2) {
  String dataString = String(temp, 2);
  dataString += ",";
  dataString += String(pm25);
  dataString += ",";
  dataString += String(pm10);
  dataString += ",";
  dataString += String(humidity, 2);
  dataString += ",";
  dataString += String(co2);
  
  return dataString;
}

void sendDataToServer(String data) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    
    String url = "http://" + serverIP + "/" + endpoint;
    
    if (DEBUG) {
      Serial.printf("Sending data to: %s\n", url.c_str());
      Serial.printf("Data: %s\n", data.c_str());
    }
    
    http.begin(client, url);
    http.addHeader("Content-Type", "text/plain");
    
    int httpCode = http.POST(data);
    
    if (DEBUG) {
      Serial.printf("HTTP Response code: %d\n", httpCode);
    }
    
    http.end();
  } else {
    if (DEBUG) {
      Serial.printf("WiFi not connected, can't send data\n");
    }
  }
}

void setup() {
  sensorStartTime = millis();  

  // initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);

  // Initialize OLED display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.printf("SSD1306 allocation failed\n");
  }

  // Initialize SHTC3 sensor
  Serial.printf("SHTC3 test\n");
  if (!shtc3.begin()) {
    Serial.printf("Couldn't find SHTC3\n");
  } else {
    Serial.printf("Found SHTC3 sensor\n");
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Starting Air Monitor");
  display.println("");
  display.println("Connecting to WiFi...");
  display.println("Warming up sensors...");
  display.display();

  Serial.begin(9600);
  pmsSerial.begin(9600);
  mhzSerial.begin(9600);
  
  WiFi.begin(ssid, password);
  
  delay(WIFI_CON_TIME);
}

void loop() {
  unsigned long currentTime = millis();
  
  // Check if sensor is in warm-up period
  if (!pmsReadingsReady && (currentTime - sensorStartTime >= WARM_UP_TIME)) {
    pmsReadingsReady = true;
    if (DEBUG) {
      Serial.printf("PMS sensor ready for readings\n");
    }
  } else {
    delay(1000);
  }
  
  // Read MH-Z19B CO2 sensor data
  co2ppm = readCO2();
  
  if (DEBUG) {
    Serial.printf("CO2: %d ppm\n", co2ppm);
  }
  
  // Get data from sensors
  sensors_event_t humidity, temp;
  shtc3.getEvent(&humidity, &temp);
  bool pmDataValid = false;
  if (pmsReadingsReady) {
    pmDataValid = readPMSData(pmsData);
  }
  
  // Print debug info if enabled
  if (DEBUG) {
    Serial.printf("Tem: %.2f C Hum: %.2f%% rH\n", temp.temperature, humidity.relative_humidity);
    
    if (pmDataValid) {
      Serial.printf("PM1.0: %d, PM2.5: %d ug/m3, PM10: %d ug/m3\n", pmsData.PM1_0, pmsData.PM2_5, pmsData.PM10);
    }
    else {
      Serial.printf("PMS data not valid\n");
    }
  }
  
  String formattedData = formatSensorData(
    temp.temperature,
    humidity.relative_humidity,
    pmsData.PM2_5,
    pmsData.PM10,
    co2ppm
  );
    
  sendDataToServer(formattedData);
  
  // Display different screens based on sensor state
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  
  display.setTextSize(2);
  display.print("AirMonitor\n");
  
  display.setTextSize(1);
  display.printf("\nTemp: %.2f C\n", temp.temperature);
  display.printf("Humidity: %.2f %%\n", humidity.relative_humidity);
  display.printf("PM2.5: %d ug/m3\n", pmsData.PM2_5);
  display.printf("PM10: %d ug/m3\n", pmsData.PM10);
  display.printf("CO2: %d ppm\n", co2ppm);

  display.display();
  delay(1000);
}