// Include Libraries
#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "time.h"
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <math.h> // For sqrt()

// Define ADC pins directly
#define VOLTAGE_SENSOR_PIN 34 // ADC1_CH6 for ZMPT101B
#define CURRENT_SENSOR_PIN 32 // ADC1_CH0 for ACS712 

// --- Relay Pin Definition ---
#define RELAY_PIN 2 // ESP32 GPIO2 (D2 on many boards). Connect to relay module IN pin.
                    // LOGIC INVERTED: Now assumes an ACTIVE-HIGH relay module (HIGH turns relay ON, LOW turns relay OFF).
                    // Or, if using an ACTIVE-LOW relay, this inverted logic might be needed due to wiring or specific module behavior.

// ADC & Manual Calibration Parameters (MUST ADJUST THESE)
const float ADC_REFERENCE_VOLTAGE = 3.3; // ESP32 ADC reference voltage (typically 3.3V, measure for accuracy if possible)
const int ADC_RESOLUTION = 4095;      // ESP32 ADC resolution (12-bit: 0-4095)

float ZMPT101B_ADC_OFFSET = 1868.62 ;   // Ideal offset if output is perfectly centered at VCC/2. CALIBRATE THIS!
float ZMPT101B_VOLTAGE_DIVIDER_RATIO = 673; // Placeholder: e.g. for 230V AC mapping to 1V RMS at ADC. ADJUST THIS BASED ON CALIBRATION
float ACS712_ADC_OFFSET = 2251.74 ;  // Ideal offset if output is VCC/2 at 0A. CALIBRATE THIS!
const float ACS712_SENSITIVITY_MV_PER_AMP = 100; // Example for a 20A module.

// RMS Calculation Parameters
const int RMS_NUM_SAMPLES = 250; // Number of samples for RMS calculation.

// WiFi Credentials
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD" 

// Firebase Project Configuration
#define API_KEY "YOUR_API_KEY" 
#define USER_EMAIL "YOUR_USER_EMAIL" 
#define USER_PASSWORD "YOUR_USER_PASSWORD" 
#define DATABASE_URL "YOUR_DATABASE_URL" 

// Firebase Objects
FirebaseData fbdo; // Used for general Firebase operations (sending sensor data, initial relay state set)
FirebaseData fbdo_stream; // Dedicated for the relay control stream
FirebaseAuth auth;
FirebaseConfig config;
String uid;

// Database Paths
String databasePath; // Main path for sensor readings, e.g., /UsersData/<uid>/sensor_readings
String parentPath;   // Path for each new sensor reading, e.g., /UsersData/<uid>/sensor_readings/<timestamp>
String relayControlPath; // Path for relay control, e.g., /UsersData/<uid>/control/relayState

// Child node paths for sensor data within each reading
String voltagePath = "/voltage_V";
String currentPath = "/current_A";
String powerPath = "/power_W";
String energyAccumulatedPath = "/energy_Wh";
String timestampPath = "/timestamp_epoch";
String rmsPinVoltagePath = "/rms_pin_voltage_calib";

// Sensor Readings and Calculations
float U = 0.0; 
float I = 0.0; 
float P = 0.0; 
float accumulatedEnergyWh = 0.0; 
float rms_pin_voltage_V_global = 0.0; 

unsigned long lastSampleMicros = 0; 

// NTP Server for Time Sync
const char* ntpServer = "pool.ntp.org";

// Timer for Sending Data to Firebase
unsigned long sendDataPrevMillis = 0;
unsigned long timerDelay = 1000; // Send data every 1 second

// Firebase JSON object for sensor data
FirebaseJson json;

// --- Function Declarations ---
void initWiFi();
unsigned long getCurrentEpochTime();
void setupSensorsAndRelay(); 
void readSensorsAndCalculateEnergy();
void sendDataToFirebase();
void streamCallback(FirebaseStream data);
void streamTimeoutCallback(bool timeout);

// --- Setup Function ---
void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Firebase Sensor Data Logger with Relay Control Initializing (INVERTED RELAY LOGIC)...");

  initWiFi();
  configTime(0, 0, ntpServer); 
  setupSensorsAndRelay(); 

  // Firebase Configuration
  config.api_key = API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.database_url = DATABASE_URL;
  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(4096);
  fbdo_stream.setResponseSize(256); 
  config.token_status_callback = tokenStatusCallback; 
  config.max_token_generation_retry = 5;
  Firebase.begin(&config, &auth);

  Serial.println("Getting User UID...");
  while ((auth.token.uid) == "") {
    Serial.print(".");
    delay(1000);
  }
  uid = auth.token.uid.c_str();
  Serial.print("User UID: ");
  Serial.println(uid);

  databasePath = "/UsersData/" + uid + "/sensor_readings"; 
  Serial.print("Sensor Database Path: ");
  Serial.println(databasePath);

  // --- Relay Control Setup ---
  relayControlPath = "/UsersData/" + uid + "/control/relayState";
  Serial.print("Relay Control Path: ");
  Serial.println(relayControlPath);

  Serial.println("Setting initial relay state to OFF (0) in Firebase...");
  if (!Firebase.RTDB.setIntAsync(&fbdo, relayControlPath.c_str(), 0)) {
      Serial.println("FAILED to set initial relay state in Firebase: " + fbdo.errorReason());
  }

  Serial.print("Starting Firebase stream on: "); Serial.println(relayControlPath);
  if (!Firebase.RTDB.beginStream(&fbdo_stream, relayControlPath.c_str())) {
    Serial.println("Stream begin error: " + fbdo_stream.errorReason());
  }
  Firebase.RTDB.setStreamCallback(&fbdo_stream, streamCallback, streamTimeoutCallback);
  // --- End Relay Control Setup ---

  lastSampleMicros = micros(); 
  Serial.println("Setup complete. Starting main loop...");
}

// --- Main Loop ---
void loop() {
  readSensorsAndCalculateEnergy();

  if (Firebase.ready() && (millis() - sendDataPrevMillis >= timerDelay || sendDataPrevMillis == 0)) {
    sendDataPrevMillis = millis();
    sendDataToFirebase();
  }
}

// --- Firebase Stream Callback Functions ---
void streamCallback(FirebaseStream data) {
  Serial.printf("Stream event: path = %s, event type = %s, data type = %s\n", 
                data.streamPath().c_str(), 
                data.eventType().c_str(), 
                data.dataType().c_str());

  if (data.dataTypeEnum() == fb_esp_rtdb_data_type_integer) {
    int relayState = data.intData();
    Serial.print("Relay state received from Firebase: "); Serial.println(relayState);
    // INVERTED LOGIC: Assuming active-HIGH relay or inverted active-LOW setup
    if (relayState == 1) { // Webpage wants ON
      digitalWrite(RELAY_PIN, HIGH); // HIGH = ON for active-HIGH
      Serial.println("Relay turned ON (HIGH signal)");
    } else { // Webpage wants OFF (relayState == 0 or any other value)
      digitalWrite(RELAY_PIN, LOW);  // LOW = OFF for active-HIGH
      Serial.println("Relay turned OFF (LOW signal)");
    }
  } else if (data.dataTypeEnum() == fb_esp_rtdb_data_type_null) {
    Serial.println("Relay control path deleted or data is null. Defaulting relay to OFF.");
    digitalWrite(RELAY_PIN, LOW); // Default to OFF (LOW signal for active-HIGH)
  } else {
    Serial.println("Received unexpected data type for relay state: " + data.dataType());
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("Firebase stream timeout. Library will attempt to resume.");
  }
}

// --- Helper Functions ---

void initWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi ");
  unsigned long wifiConnectStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
    if (millis() - wifiConnectStart > 20000) { 
        Serial.println("\nFailed to connect to WiFi. Restarting...");
        delay(1000);
        ESP.restart();
    }
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

unsigned long getCurrentEpochTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) { 
    Serial.println("Failed to obtain time");
    return(0);
  }
  time(&now);
  return now;
}

void setupSensorsAndRelay() {
  pinMode(VOLTAGE_SENSOR_PIN, INPUT);
  pinMode(CURRENT_SENSOR_PIN, INPUT);
  Serial.println("Sensor pins initialized.");

  pinMode(RELAY_PIN, OUTPUT);
  // INVERTED LOGIC: Initialize relay to OFF state (LOW for active-HIGH relay)
  digitalWrite(RELAY_PIN, LOW); 
  Serial.print("Relay pin D"); Serial.print(RELAY_PIN); Serial.println(" initialized to OFF state (LOW signal).");
  Serial.println("Physical connection for active-HIGH relay (OFF at start with LOW signal):");
  Serial.println("- Relay VCC to ESP32 3.3V/5V (as per relay module spec)");
  Serial.println("- Relay GND to ESP32 GND");
  Serial.println("- Relay IN to ESP32 D2 (GPIO2)");
  Serial.println("Sensor and Relay setup complete.");
}

void readSensorsAndCalculateEnergy() {
  double sum_sq_voltage_diff = 0;
  for (int i = 0; i < RMS_NUM_SAMPLES; i++) {
    int raw_adc_voltage = analogRead(VOLTAGE_SENSOR_PIN);
    double diff = (double)raw_adc_voltage - ZMPT101B_ADC_OFFSET;
    sum_sq_voltage_diff += diff * diff;
    delayMicroseconds(50); 
  }
  double mean_sq_voltage = sum_sq_voltage_diff / RMS_NUM_SAMPLES;
  double rms_adc_voltage_component = sqrt(mean_sq_voltage);
  rms_pin_voltage_V_global = rms_adc_voltage_component * (ADC_REFERENCE_VOLTAGE / (double)ADC_RESOLUTION);
  U = rms_pin_voltage_V_global * ZMPT101B_VOLTAGE_DIVIDER_RATIO;

  double sum_sq_current_diff = 0;
  for (int i = 0; i < RMS_NUM_SAMPLES; i++) {
    int raw_adc_current = analogRead(CURRENT_SENSOR_PIN);
    double diff = (double)raw_adc_current - ACS712_ADC_OFFSET;
    sum_sq_current_diff += diff * diff;
    delayMicroseconds(50); 
  }
  double mean_sq_current = sum_sq_current_diff / RMS_NUM_SAMPLES;
  double rms_adc_current_component = sqrt(mean_sq_current);
  double rms_pin_current_sense_V = rms_adc_current_component * (ADC_REFERENCE_VOLTAGE / (double)ADC_RESOLUTION);
  double sensitivity_V_per_A = ACS712_SENSITIVITY_MV_PER_AMP / 1000.0;
  if (sensitivity_V_per_A > 0.00001) { 
      I = rms_pin_current_sense_V / sensitivity_V_per_A;
  } else {
      I = 0.0; 
      Serial.println("Error: ACS712 sensitivity is zero or too low. Check ACS712_SENSITIVITY_MV_PER_AMP.");
  }

  unsigned long currentMicros = micros();
  float dt_seconds = (float)(currentMicros - lastSampleMicros) / 1000000.0;
  lastSampleMicros = currentMicros;

  if (U < 10.0) { U = 0.0; }
  if (I < 0.05) { I = 0.0; }
  P = U * I;
  if (P > 0 && dt_seconds > 0) {
      accumulatedEnergyWh += P * (dt_seconds / 3600.0);
  }
}

void sendDataToFirebase() {
  unsigned long client_timestamp_for_path;
  bool use_server_timestamp_field = false;
  unsigned long local_epoch_time = getCurrentEpochTime();

  if (local_epoch_time == 0) {
    Serial.println("Failed to obtain local epoch time. Using millis() for path name and requesting Firebase server timestamp for the data field.");
    client_timestamp_for_path = millis() / 1000; 
    use_server_timestamp_field = true; 
  } else {
    client_timestamp_for_path = local_epoch_time; 
  }

  parentPath = databasePath + "/" + String(client_timestamp_for_path);
  json.clear(); 
  json.set(voltagePath.c_str(), String(U, 2)); 
  json.set(currentPath.c_str(), String(I, 3)); 
  json.set(powerPath.c_str(), String(P, 2));   
  json.set(energyAccumulatedPath.c_str(), String(accumulatedEnergyWh, 4)); 
  json.set(rmsPinVoltagePath.c_str(), String(rms_pin_voltage_V_global, 4));

  if (use_server_timestamp_field) {
    FirebaseJson server_timestamp_json; 
    server_timestamp_json.set(".sv", "timestamp"); 
    json.set(timestampPath.c_str(), server_timestamp_json); 
  } else {
    json.set(timestampPath.c_str(), String(local_epoch_time)); 
  }

  Serial.print("Sending sensor data to Firebase: "); Serial.println(parentPath);
  if (Firebase.RTDB.setJSON(&fbdo, parentPath.c_str(), &json)) {
    // Serial.println("Firebase.RTDB.setJSON: OK"); 
  } else {
    Serial.println("Firebase.RTDB.setJSON: FAILED");
    Serial.print("REASON: "); Serial.println(fbdo.errorReason());
  }
}
