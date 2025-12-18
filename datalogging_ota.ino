#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Wire.h>
#include <Adafruit_SGP30.h>
#include <Adafruit_BME680.h>
#include <time.h>

/* ================= CONFIG ================= */

// WiFi credentials
#define WIFI_SSID "The King 2.5 GHz"
#define WIFI_PASSWORD "1234567891"

// Firebase configuration
#define FIREBASE_HOST "https://ota-check-ab802-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_AUTH "qHVez2jS7dwaxRH2lgCyi8HnyqCaL39i3qvoGakO"

// Current firmware version - UPDATE THIS WHEN RELEASING NEW VERSION
#define FW_VERSION "1.3"

// Timezone settings
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 19800      // 5 hrs 30 mins = 19800 sec (IST)
#define DAYLIGHT_OFFSET_SEC 0

// Timing intervals
#define SENSOR_READ_INTERVAL 5000      // 5 seconds
#define OTA_CHECK_INTERVAL 60000     // 1 min (check for updates)

/* ========================================== */

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Sensors
Adafruit_SGP30 sgp;
Adafruit_BME680 bme;

// Timing variables
unsigned long lastSensorRead = 0;
unsigned long lastOTACheck = 0;

// Function declarations
String getTimeStamp();
String getDateStamp();
void readAndUploadSensorData();
void checkForFirmwareUpdate();
void performOTA(String firmwareURL);

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  Serial.println("====================================");
  Serial.println("Booting firmware v" FW_VERSION);
  Serial.println("====================================");

  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Configure NTP time
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.println("Waiting for time...");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nTime synchronized");

  // Initialize Firebase
  config.database_url = FIREBASE_HOST;
  if (strlen(FIREBASE_AUTH) > 0) {
    config.signer.tokens.legacy_token = FIREBASE_AUTH;
  }
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("Firebase initialized");

  // Initialize SGP30 sensor
  if (!sgp.begin()) {
    Serial.println("SGP30 not found");
    while (1);
  }
  Serial.println("SGP30 sensor initialized");

  // Initialize BME680 sensor
  if (!bme.begin(0x76)) {
    Serial.println("BME680 not found");
    while (1);
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setGasHeater(320, 150);
  Serial.println("BME680 sensor initialized");

  Serial.println("====================================");
  Serial.println("All sensors ready!");
  Serial.println("====================================\n");

  // Check for firmware update at startup
  checkForFirmwareUpdate();
}

void loop() {
  unsigned long currentMillis = millis();

  // Read and upload sensor data every SENSOR_READ_INTERVAL
  if (currentMillis - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = currentMillis;
    readAndUploadSensorData();
  }

  // Check for OTA updates every OTA_CHECK_INTERVAL
  if (currentMillis - lastOTACheck >= OTA_CHECK_INTERVAL) {
    lastOTACheck = currentMillis;
    checkForFirmwareUpdate();
  }

  delay(100);  // Small delay to prevent watchdog issues
}

/* ================= TIME FUNCTIONS ================= */

String getTimeStamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "time_error";
  }

  char buffer[20];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo); // 24-hr format
  return String(buffer);
}

String getDateStamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "date_error";
  }

  char buffer[20];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo); // YYYY-MM-DD format
  return String(buffer);
}

/* ================= SENSOR FUNCTIONS ================= */

void readAndUploadSensorData() {
  // Read sensors
  sgp.IAQmeasure();
  bme.performReading();

  // Get date and time
  String date = getDateStamp();
  String time = getTimeStamp();
  
  if (date == "date_error" || time == "time_error") {
    Serial.println("Time not available, skipping upload");
    return;
  }

  // Create JSON payload
  String jsonData = "{";
  jsonData += "\"TVOC\":" + String(sgp.TVOC) + ",";
  jsonData += "\"eCO2\":" + String(sgp.eCO2) + ",";
  jsonData += "\"Temperature\":" + String(bme.temperature) + ",";
  jsonData += "\"Humidity\":" + String(bme.humidity) + ",";
  jsonData += "\"Pressure\":" + String(bme.pressure / 100.0) + ",";
  jsonData += "\"Gas\":" + String(bme.gas_resistance / 1000.0) + ",";
  jsonData += "\"Date\":\"" + date + "\",";
  jsonData += "\"Time\":\"" + time + "\"";
  jsonData += "}";

  // Send to Firebase using HTTP PUT
  HTTPClient http;
  String timestamp = time;
  timestamp.replace(":", "_");
  String url = String(FIREBASE_HOST) + "History/" + date + "/" + timestamp + ".json";

  http.begin(url);
  int httpCode = http.PUT(jsonData);

  Serial.print("Firebase response: ");
  Serial.println(httpCode);
  
  if (httpCode == 200) {
    Serial.printf("[%s %s] TVOC: %d ppb | eCO2: %d ppm | Temp: %.2f°C | Hum: %.2f%%\n",
                  date.c_str(), time.c_str(), sgp.TVOC, sgp.eCO2, bme.temperature, bme.humidity);
  }

  http.end();
}

/* ================= OTA FUNCTIONS ================= */

void checkForFirmwareUpdate() {
  Serial.println("Checking Firebase for latest firmware...");

  // Read latest firmware version from Firebase
  if (!Firebase.RTDB.getString(&fbdo, "/ota/version")) {
    Serial.printf("Failed to get version: %s\n", fbdo.errorReason().c_str());
    return;
  }
  
  String latestVersion = fbdo.stringData();
  Serial.println("Latest version on Firebase: " + latestVersion);

  // Compare with current firmware version
  if (String(FW_VERSION) == latestVersion) {
    Serial.println("Firmware is already up to date.");
    return;
  }

  Serial.println("*** NEW FIRMWARE AVAILABLE ***");
  Serial.println("Current version: " FW_VERSION);
  Serial.println("New version: " + latestVersion);

  // Get firmware URL from Firebase
  if (!Firebase.RTDB.getString(&fbdo, "/ota/url")) {
    Serial.printf("Failed to get URL: %s\n", fbdo.errorReason().c_str());
    return;
  }
  
  String firmwareURL = fbdo.stringData();
  Serial.println("Firmware URL: " + firmwareURL);

  // Perform OTA update
  performOTA(firmwareURL);
}

void performOTA(String firmwareURL) {
  Serial.println("\n========================================");
  Serial.println("STARTING OTA UPDATE");
  Serial.println("DO NOT POWER OFF THE DEVICE!");
  Serial.println("========================================\n");

  HTTPClient http;
  http.begin(firmwareURL);
  
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Failed to download firmware, HTTP code: %d\n", httpCode);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("Invalid content length");
    http.end();
    return;
  }

  Serial.printf("Firmware size: %d bytes\n", contentLength);

  if (!Update.begin(contentLength)) {
    Serial.println("Not enough space for OTA");
    http.end();
    return;
  }

  Serial.println("Downloading and flashing firmware...");
  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written == contentLength && Update.end()) {
    Serial.println("\n========================================");
    Serial.println("OTA SUCCESS!");
    Serial.println("========================================");
    Serial.println("Rebooting in 3 seconds...");
    delay(3000);
    ESP.restart();
  } else {
    Serial.println("OTA Failed");
    Update.printError(Serial);
    Update.abort();
  }

  http.end();
}