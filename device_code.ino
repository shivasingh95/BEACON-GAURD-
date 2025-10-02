#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h> // For BLE2902, needed for notifications

// Include TinyGPSPlus library for GPS parsing
#include <TinyGPSPlus.h>
#include <HardwareSerial.h> // For using Serial1 and Serial2 on ESP32

// =======================================================
//                    BLE CONFIGURATION
// =======================================================
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b" // Custom Service UUID
#define ALERT_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8" // Custom Alert Characteristic UUID
#define GPS_CHARACTERISTIC_UUID   "00002a23-0000-1000-8000-00805f9b34fb" // Standard GPS Location Characteristic (or custom)

BLEServer* pServer = NULL;
BLECharacteristic* pAlertCharacteristic = NULL;
BLECharacteristic* pGPSCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// =======================================================
//                    PIN DEFINITIONS
// =======================================================
// Push Button
const int BUTTON_PIN = 25; // GPIO25 for push button

// Voice Recognition Module (VR3) - Using Serial2
const int VR_TX_PIN = 27; // ESP32 TX to VR3 RX
const int VR_RX_PIN = 26; // ESP32 RX from VR3 TX (via voltage divider)
HardwareSerial SerialVR(2); // Use Serial2 for VR module

// GPS Module (NEO-6M) - Using Serial1
const int GPS_TX_PIN = 13; // ESP32 TX to GPS RX
const int GPS_RX_PIN = 12; // ESP32 RX from GPS TX
HardwareSerial SerialGPS(1); // Use Serial1 for GPS module

// =======================================================
//                  VOICE RECOGNITION CONFIG
// =======================================================
// Commands and their IDs for Elechouse VR3
// Based on 'sigtrain 2 HELP': If "HELP" was the FIRST command trained in Group 2,
// its command ID will be 0. If it was the second, it would be 1, and so on.
const int VR_COMMAND_ID_HELP = 0; // ASSUMPTION: "HELP" was the first command (ID 0) in Group 2.
                                  // *** ADJUST THIS VALUE IF YOUR TRAINING ASSIGNED A DIFFERENT ID ***
unsigned long lastVoiceReadTime = 0;
const long VOICE_READ_INTERVAL = 50; // Read VR module every 50ms

// =======================================================
//                      GPS CONFIG
// =======================================================
TinyGPSPlus gps; // TinyGPSPlus object
// No periodic GPS sending, so no lastGPSSendTime or GPS_SEND_INTERVAL needed

// =======================================================
//                      FUNCTION PROTOTYPES
// =======================================================
void sendAlert(String alertType);
void sendGPSDataOnAlert(); // Modified function for GPS on alert
void readVRModule();
void processGPSData();
void handleButtonPress();

// =======================================================
//                     BLE CALLBACKS
// =======================================================
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("BLE Client Connected!");
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("BLE Client Disconnected!");
      // Restart advertising to allow reconnection
      BLEDevice::startAdvertising();
    }
};

// =======================================================
//                         SETUP
// =======================================================
void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32 Emergency Beacon (Old Version Logic Firmware)...");

  // Initialize Push Button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // Attach interrupt for immediate button press detection
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);
  Serial.println("Button interrupt attached to GPIO" + String(BUTTON_PIN));

  // Initialize Voice Recognition Serial (Serial2)
  SerialVR.begin(9600, SERIAL_8N1, VR_RX_PIN, VR_TX_PIN);
  Serial.println("Voice Recognition Serial (Serial2) initialized on RX:" + String(VR_RX_PIN) + ", TX:" + String(VR_TX_PIN));

  // Initialize GPS Serial (Serial1)
  SerialGPS.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("GPS Serial (Serial1) initialized on RX:" + String(GPS_RX_PIN) + ", TX:" + String(GPS_TX_PIN));

  // Initialize BLE
  BLEDevice::init("EmergencySwitch"); // Name of your BLE device
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create Alert Characteristic
  pAlertCharacteristic = pService->createCharacteristic(
                      ALERT_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pAlertCharacteristic->addDescriptor(new BLE2902()); // For notifications

  // Create GPS Characteristic
  pGPSCharacteristic = pService->createCharacteristic(
                      GPS_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pGPSCharacteristic->addDescriptor(new BLE2902()); // For notifications

  pService->start(); // Start the service

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // helps with iPhone connection issues
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE Advertising started. Device name: EmergencySwitch");
}

// =======================================================
//                          LOOP
// =======================================================
void loop() {
  // Read and process GPS data continuously
  // This updates the 'gps' object, so the latest data is available when needed.
  processGPSData();

  // Read voice recognition module
  readVRModule();

  // Handle BLE state changes (disconnection management)
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); // Give the BLE stack a moment to recover
    // Restart advertising already handled in onDisconnect
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    // Device connected
    oldDeviceConnected = deviceConnected;
  }
  
  // Removed: Periodic GPS data sending from loop()
  // Now, GPS data is only sent when an alert is triggered.

  // Small delay to prevent watchdog timer from triggering too quickly in empty loops
  delay(10);
}

// =======================================================
//                       FUNCTIONS
// =======================================================

// Function to send alert via BLE
void sendAlert(String alertType) {
  if (deviceConnected) {
    pAlertCharacteristic->setValue(alertType.c_str());
    pAlertCharacteristic->notify();
    Serial.println("BLE Alert Sent: " + alertType);
    
    // In old version logic, GPS data was often sent alongside the alert
    // to provide the module's "last known" as a fallback.
    sendGPSDataOnAlert(); // Trigger sending GPS data immediately after alert
  } else {
    Serial.println("BLE Alert FAILED: No device connected.");
  }
}

// Function to send GPS data via BLE specifically when an alert is triggered
void sendGPSDataOnAlert() {
  if (deviceConnected) {
    String gps_payload = "0.0,0.0"; // Default invalid location

    if (gps.location.isValid()) {
      gps_payload = String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
      Serial.println("Current GPS: " + gps_payload);
    } else {
      Serial.println("GPS not valid (for alert).");
    }

    pGPSCharacteristic->setValue(gps_payload.c_str());
    pGPSCharacteristic->notify();
    Serial.println("BLE GPS Sent (on Alert): " + gps_payload);
  } else {
    Serial.println("BLE GPS FAILED (on Alert): No device connected.");
  }
}

// Function to read data from VR Module
void readVRModule() {
  if (millis() - lastVoiceReadTime > VOICE_READ_INTERVAL) {
    if (SerialVR.available()) {
      uint8_t voiceCommand = SerialVR.read(); // Read the byte from VR module

      // Debug: Print raw command ID
      Serial.print("VR Raw Command ID: ");
      Serial.println(voiceCommand);

      // Check if the command ID matches our trained "HELP" command (ID 0 in Group 2)
      if (voiceCommand == VR_COMMAND_ID_HELP) { // This is 0 based on our assumption
        Serial.println("Voice Command 'HELP' recognized!");
        sendAlert("VoiceAlert"); // Send a "VoiceAlert" via BLE
      } else if (voiceCommand >= 0x01 && voiceCommand <= 0x14) { // Other trained commands (1-20)
        Serial.print("Other voice command recognized: ID ");
        Serial.println(voiceCommand);
      } else if (voiceCommand == 0x21) { // 0x21 indicates command ID out of scope
        Serial.println("Voice: Command ID out of scope.");
      } else if (voiceCommand == 0x22) { // 0x22 indicates unknown command
        Serial.println("Voice: Unknown command.");
      } else {
        Serial.print("Voice: Unhandled VR response: 0x");
        Serial.println(voiceCommand, HEX);
      }
    }
    lastVoiceReadTime = millis();
  }
}

// Function to process incoming GPS NMEA data
void processGPSData() {
  while (SerialGPS.available() > 0) {
    if (gps.encode(SerialGPS.read())) {
      // Data was successfully encoded, means new valid data might be available
      // The 'gps' object is continuously updated so its data is fresh when sendGPSDataOnAlert() is called.
    }
  }
}

// Interrupt Service Routine (ISR) for button press
// MUST be fast and not use Serial.print or delay
void IRAM_ATTR handleButtonPress() {
  static unsigned long lastInterruptTime = 0;
  unsigned long interruptTime = millis();
  // Debounce: ignore presses that are too close together (e.g., within 200ms)
  if (interruptTime - lastInterruptTime > 200) {
    sendAlert("Alert"); // Send a "Alert" via BLE
    lastInterruptTime = interruptTime;
  }
}