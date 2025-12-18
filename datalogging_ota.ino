#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_SGP30.h>
#include <Adafruit_BME680.h>
#include <time.h>

/* ================= CONFIG ================= */

// WiFi credentials
#define WIFI_SSID "The King 2.5 GHz"
#define WIFI_PASSWORD "1234567891"

// Firebase configuration (using the sensor data database)
#define FIREBASE_HOST "https://ota-check-ab802-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "qHVez2jS7dwaxRH2lgCyi8HnyqCaL39i3qvoGakO"

// Firmware version of THIS build
#define FW_VERSION "1.0"

// Timezone settings
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 19800      // 5 hrs 30 mins = 19800 sec
#define DAYLIGHT_OFFSET_SEC 0

// OTA check interval (in milliseconds) - check every 1 hour
#define OTA_CHECK_INTERVAL 60000

/* ========================================== */

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Preferences (Flash storage)
Preferences prefs;

// Sensors
Adafruit_SGP30 sgp;
Adafruit_BME680 bme;

// Timing variables
unsigned long lastOTACheck = 0;
unsigned long lastSensorRead = 0;
#define SENSOR_READ_INTERVAL 5000  // Read sensors every 5 seconds

// Function declarations
String getTimeStamp();
void checkForFirmwareUpdate();
void performOTA(String firmwareURL, String latestVersion);
void readAndUploadSensorData();

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  // Init preferences
  prefs.begin("ota", false);

  // Read stored version
  String installedVersion = prefs.getString("version", FW_VERSION);
  Serial.println("Booting firmware v" + installedVersion);

  // WiFi connect
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");

  // Configure time
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.println("Waiting for time...");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nTime synchronized");

  // Firebase init
  config.database_url = FIREBASE_HOST;
  if (strlen(FIREBASE_AUTH) > 0) {
    config.signer.tokens.legacy_token = FIREBASE_AUTH;
  }
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Initialize SGP30
  if (!sgp.begin()) {
    Serial.println("SGP30 not found");
    while (1) delay(10);
  }
  Serial.println("SGP30 initialized");

  // Initialize BME680
  if (!bme.begin(0x76)) {
    Serial.println("BME680 not found");
    while (1) delay(10);
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setGasHeater(320, 150);
  Serial.println("BME680 initialized");

  Serial.println("All sensors ready");

  // Check for firmware update at startup
  checkForFirmwareUpdate();
}

void loop() {
  unsigned long currentMillis = millis();

  // Check for OTA update periodically
  if (currentMillis - lastOTACheck >= OTA_CHECK_INTERVAL) {
    lastOTACheck = currentMillis;
    checkForFirmwareUpdate();
  }

  // Read and upload sensor data
  if (currentMillis - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = currentMillis;
    readAndUploadSensorData();
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

/* ================= SENSOR FUNCTIONS ================= */

void readAndUploadSensorData() {
  // Read sensors
  if (!sgp.IAQmeasure()) {
    Serial.println("SGP30 measurement failed");
    return;
  }

  if (!bme.performReading()) {
    Serial.println("BME680 reading failed");
    return;
  }

  // Get timestamp
  String timestamp = getTimeStamp();
  if (timestamp == "time_error") {
    Serial.println("Time not available");
    return;
  }

  // Replace colons with underscores for Firebase path
  timestamp.replace(":", "_");

  // Create JSON object using Firebase JSON
  FirebaseJson json;
  json.set("TVOC", sgp.TVOC);
  json.set("eCO2", sgp.eCO2);
  json.set("Temperature", bme.temperature);
  json.set("Humidity", bme.humidity);
  json.set("Pressure", bme.pressure / 100.0);
  json.set("Gas", bme.gas_resistance / 1000.0);

  // Upload to Firebase
  String path = "/History/" + timestamp;
  
  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
    Serial.println("Sensor data uploaded: " + timestamp);
    Serial.printf("TVOC: %d ppb, eCO2: %d ppm, Temp: %.2f°C, Hum: %.2f%%\n", 
                  sgp.TVOC, sgp.eCO2, bme.temperature, bme.humidity);
  } else {
    Serial.println("Failed to upload sensor data");
    Serial.println("Reason: " + fbdo.errorReason());
  }
}

/* ================= OTA FUNCTIONS ================= */

void checkForFirmwareUpdate() {
  Serial.println("Checking Firebase for latest firmware...");

  if (!Firebase.RTDB.getString(&fbdo, "/ota/version")) {
    Serial.println("Failed to read OTA version");
    Serial.println("Reason: " + fbdo.errorReason());
    return;
  }

  String latestVersion = fbdo.stringData();
  String installedVersion = prefs.getString("version", FW_VERSION);

  Serial.println("Installed Version: " + installedVersion);
  Serial.println("Latest Version: " + latestVersion);

  if (installedVersion == latestVersion) {
    Serial.println("Firmware is already up to date.");
    return;
  }

  Serial.println("New firmware available!");

  if (!Firebase.RTDB.getString(&fbdo, "/ota/url")) {
    Serial.println("Failed to read firmware URL");
    Serial.println("Reason: " + fbdo.errorReason());
    return;
  }

  String firmwareURL = fbdo.stringData();
  Serial.println("Firmware URL: " + firmwareURL);

  performOTA(firmwareURL, latestVersion);
}

void performOTA(String firmwareURL, String latestVersion) {
  Serial.println("Starting OTA update...");
  Serial.println("This may take a few minutes. Do not power off the device!");

  HTTPClient http;
  http.begin(firmwareURL);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP Error: %d\n", httpCode);
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
    Serial.printf("Available: %d bytes\n", ESP.getFreeSketchSpace());
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written == contentLength) {
    Serial.println("Written : " + String(written) + " successfully");
  } else {
    Serial.println("Written only : " + String(written) + "/" + String(contentLength));
  }

  if (Update.end()) {
    Serial.println("OTA done!");
    if (Update.isFinished()) {
      Serial.println("Update successfully completed!");

      // Save new version in flash
      prefs.putString("version", latestVersion);
      Serial.println("Stored new version: " + latestVersion);

      // Optional: Update Firebase to confirm successful OTA
      FirebaseJson json;
      json.set("last_update", latestVersion);
      json.set("status", "success");
      Firebase.RTDB.setJSON(&fbdo, "/ota/last_update_status", &json);

      Serial.println("Rebooting in 3 seconds...");
      delay(3000);
      ESP.restart();
    } else {
      Serial.println("Update not finished? Something went wrong!");
    }
  } else {
    Serial.println("OTA Error Occurred. Error #: " + String(Update.getError()));
    Update.abort();
  }

  http.end();
}