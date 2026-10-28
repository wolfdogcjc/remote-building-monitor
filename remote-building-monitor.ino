/**
 * ============================================================================
 * PRODUCTION REMOTE BUILDING MONITORING SYSTEM 
 * (Raspberry Pi Pico 2 W + SIM7080G + DHT22 + PIR -> Blynk)
 * ============================================================================
 */

#define TINY_GSM_MODEM_SIM7080
#include <TinyGsmClient.h>
#include <DHT.h>
#include "hardware/watchdog.h" // RP2040 Hardware Watchdog

// --- HOLOGRAM APN ---
const char apn[]  = "hologram";
const char user[] = "";
const char pass[] = "";

// --- BLYNK CONFIG ---
const char BLYNK_AUTH[] = "YOUR_BLYNK_AUTH_TOKEN"; // Replace with your public/placeholder token
const char BLYNK_HOST[] = "blynk.cloud";
const int  BLYNK_PORT   = 80;

#define SerialAT Serial1
TinyGsm modem(SerialAT);
TinyGsmClient client(modem);

#define MODEM_PWRKEY 14
#define DHT_PIN      3
#define PIR_PIN      2
#define DHTTYPE      DHT22

DHT dht(DHT_PIN, DHTTYPE);

// Timing Intervals (in milliseconds)
const unsigned long SENSOR_CHECK_INTERVAL  = 300000;    // 5 minutes for DHT22 temp/hum
const unsigned long NORMAL_REPORT_INTERVAL = 10800000;  // 3 hours heartbeat (10800000 ms)

const unsigned long MOTION_COOLDOWN      = 10000;     // 10 seconds motion cooldown
const unsigned long MOTION_CLEAR_DELAY   = 10000;     // 10 seconds before sending motion off (0)
const unsigned long WATCHDOG_TIMEOUT     = 7200000;   // 2 Hours of total inactivity forces a reboot

unsigned long lastSensorCheckTime  = 0;
unsigned long lastReportTime       = 0;
unsigned long lastMotionTrigger    = 0;
unsigned long bootTime             = 0;
unsigned long lastActivityTime     = 0; // Tracks last healthy loop/action

// Motion clear tracking variables
bool pendingMotionClear            = false;
unsigned long motionClearTargetTime = 0;

const unsigned long PIR_WARMUP_TIME  = 45000;         // 45 seconds PIR stabilization on boot

// Tracking thresholds
bool alerted_45 = false;
bool alerted_35 = false;
bool alerted_30 = false;

void powerOnModem();
void powerOffModem();
bool sendToBlynk(bool fullTelemetry, float temp, float hum, int motion);

// Helper to read DHT with quick resampling up to 3 times, falling back to 99°F if NaN
bool readDHT(float &tempF, float &hum) {
  for (int i = 0; i < 3; i++) {
    tempF = dht.readTemperature(true);
    hum   = dht.readHumidity();
    if (!isnan(tempF) && !isnan(hum)) return true;
    delay(200); // Quick retry delay
  }
  tempF = 99.0;
  hum   = 0.0;
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
  delay(1000);

  Serial.println("\n========================================");
  Serial.println("[INIT] Smart Remote Building Monitor Starting...");
  Serial.println("========================================");

  dht.begin();
  pinMode(PIR_PIN, INPUT);
  pinMode(MODEM_PWRKEY, OUTPUT);

  bootTime = millis();
  lastActivityTime = millis(); // Initialize watchdog timer

  Serial.println("[STATUS] Performing initial boot sync with Blynk...");
  float tempF, hum;
  readDHT(tempF, hum);
  
  sendToBlynk(true, tempF, hum, 0); 
  lastReportTime = millis();
  lastActivityTime = millis(); // Reset watchdog on success
  
  Serial.println("[STATUS] PIR stabilizing... motion reporting enabled in 45 seconds.");
  Serial.println("[STATUS] Entering continuous monitoring loop...\n");
}

void loop() {
  unsigned long currentMillis = millis();

  // --- SOFTWARE WATCHDOG CHECK ---
  if (currentMillis - lastActivityTime > WATCHDOG_TIMEOUT) {
    Serial.println("\n[CRITICAL ERROR] Watchdog timeout! No activity detected for 2 hours. Force rebooting...");
    delay(1000);
    watchdog_reboot(0, 0, 0); // Triggers a clean standard system reboot
  }

  // --- CHECK IF WE NEED TO SEND MOTION CLEAR (0) TO BLYNK (DATA SAVER: NO TEMP/HUM) ---
  if (pendingMotionClear && (currentMillis >= motionClearTargetTime)) {
    pendingMotionClear = false;
    lastActivityTime = currentMillis;

    Serial.println("\n----------------------------------------");
    Serial.println("[STATUS] 10s elapsed. Sending motion clear (v3=0) to Blynk...");
    Serial.println("----------------------------------------");

    // Send ONLY motion state to save cellular data limit
    sendToBlynk(false, 0, 0, 0);
    lastActivityTime = millis();
    Serial.println("[STATUS] Motion indicator successfully cleared on Blynk cloud.\n");
  }

  // 1. NON-STOP MOTION MONITORING (Ignored during initial PIR warm-up window)
  if (currentMillis - bootTime > PIR_WARMUP_TIME) {
    int motionState = digitalRead(PIR_PIN);
    if (motionState == HIGH) {
      if (currentMillis - lastMotionTrigger > MOTION_COOLDOWN) {
        lastMotionTrigger = currentMillis;
        lastActivityTime = currentMillis; // Feed watchdog
        
        Serial.println("\n----------------------------------------");
        Serial.println("[ALERT] >>> MOTION DETECTED IN REMOTE BUILDING! <<<");
        Serial.println("----------------------------------------");

        Serial.println("[DATA] Sending lightweight motion alert (v3=1) [No Temp/Hum to save data]");
        // Send ONLY motion state (false = no temp/hum bundle)
        sendToBlynk(false, 0, 0, 1);
        
        // Schedule the motion clear response for 10 seconds from now
        pendingMotionClear = true;
        motionClearTargetTime = millis() + MOTION_CLEAR_DELAY;

        lastActivityTime = millis(); // Feed watchdog after sync completes
        Serial.println("[STATUS] Resumed normal monitoring. Motion clear scheduled in 10s...\n");
      }
    }
  }

  // 2. 5-MINUTE DHT22 TEMPERATURE & HUMIDITY CHECK
  if (currentMillis - lastSensorCheckTime >= SENSOR_CHECK_INTERVAL) {
    lastSensorCheckTime = currentMillis;
    lastActivityTime = currentMillis; // Feed watchdog on scheduled tick
    evaluateScheduledCheck(currentMillis);
    lastActivityTime = currentMillis; // Feed watchdog after check completes
  }
}

void evaluateScheduledCheck(unsigned long currentMillis) {
  Serial.println("\n----------------------------------------");
  Serial.println("[SCHEDULE] Running 5-minute scheduled sensor check...");
  
  float tempF, hum;
  readDHT(tempF, hum);
  int motion = digitalRead(PIR_PIN);

  Serial.printf("[SENSOR] Temperature: %.1f°F | Humidity: %.1f%% | Motion: %d\n", tempF, hum, motion);

  bool shouldReport = false;
  String reason = "";

  if (tempF <= 45.0) {
    shouldReport = true;
    reason = "Low Temp Alert (<= 45°F) - Reporting every 5 mins";
  }
  else if (currentMillis - lastReportTime >= NORMAL_REPORT_INTERVAL) {
    shouldReport = true;
    reason = "Scheduled 3-Hour Heartbeat";
  }

  if (shouldReport) {
    Serial.println("[TRIGGER] Syncing full telemetry to Blynk. Reason: " + reason);
    // Scheduled reports bundle full telemetry (true)
    if (sendToBlynk(true, tempF, hum, motion)) {
      lastReportTime = currentMillis;
    }
  } else {
    Serial.println("[SLEEP] Conditions safe (> 45°F). Skipping cellular sync to save power/data.");
    Serial.println("[STATUS] Next regular heartbeat in ~3 hours (or instant if motion/freeze occurs).");
  }

  if (tempF <= 30.0 && !alerted_30) {
    Serial.println("[CRITICAL WARNING] Temperature has dropped to 30°F or below!");
    alerted_30 = true; alerted_35 = true; alerted_45 = true;
  } 
  else if (tempF <= 35.0 && !alerted_35 && tempF > 30.0) {
    Serial.println("[WARNING] Temperature has dropped to 35°F.");
    alerted_35 = true; alerted_45 = true;
  } 
  else if (tempF <= 45.0 && !alerted_45 && tempF > 35.0) {
    Serial.println("[NOTICE] Temperature has dropped to 45°F.");
    alerted_45 = true;
  }

  if (tempF > 48.0) {
    if (alerted_45) Serial.println("[INFO] Temperature recovered above safe threshold. Resetting alert flags.");
    alerted_45 = false; alerted_35 = false; alerted_30 = false;
  }
  Serial.println("----------------------------------------\n");
}

// Push data to Blynk via Port 80 (Supports single-pin or batch update to save data)
bool sendToBlynk(bool fullTelemetry, float temp, float hum, int motion) {
  bool success = false;
  const int maxRetries = 3;

  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    Serial.printf("[MODEM] Waking up SIM7080G cellular module (Attempt %d of %d)...\n", attempt, maxRetries);
    powerOnModem();

    SerialAT.setTX(0);
    SerialAT.setRX(1);
    SerialAT.begin(115200);
    delay(3000);

    SerialAT.println("+++");         delay(1000);
    SerialAT.println("AT&F0");          delay(500);
    SerialAT.println("AT+CGDCONT=1,\"IP\",\"hologram\""); modem.waitResponse();
    SerialAT.println("AT+CNMP=38");                     modem.waitResponse();
    SerialAT.println("AT+CMNB=3");                      modem.waitResponse();
    delay(2000);

    Serial.println("[NETWORK] Searching for cellular tower...");
    unsigned long netTimeout = millis();
    bool towerFound = false;
    while (!modem.waitForNetwork(15000L)) {
      Serial.print(".");
      if (millis() - netTimeout > 45000UL) {
        Serial.println("\n[ERROR] Network timeout during search.");
        break;
      }
    }
    if (modem.isNetworkConnected()) {
      towerFound = true;
      Serial.println("\n[NETWORK] Tower found!");
    }

    if (!towerFound) {
      Serial.println("[WARNING] Failed to find network tower. Shutting down modem and retrying...");
      powerOffModem();
      delay(3000);
      continue; // Retry loop
    }

    Serial.println("[GPRS] Connecting to Hologram network...");
    unsigned long gprsTimeout = millis();
    bool gprsConnected = false;
    while (!modem.gprsConnect(apn, user, pass)) {
      Serial.print(".");
      delay(3000);
      if (millis() - gprsTimeout > 45000UL) {
        Serial.println("\n[ERROR] GPRS connection timeout.");
        break;
      }
    }
    if (modem.isGprsConnected()) {
      gprsConnected = true;
      Serial.println("\n[GPRS] Connected successfully!");
    }

    if (!gprsConnected) {
      Serial.println("[WARNING] Failed GPRS connection. Shutting down modem and retrying...");
      powerOffModem();
      delay(3000);
      continue; // Retry loop
    }

    // Clean up socket state before connecting
    client.stop();
    delay(1000);

    Serial.println("[BLYNK] Connecting to blynk.cloud on Port 80...");
    if (client.connect(BLYNK_HOST, BLYNK_PORT)) {
      String url = "";
      if (fullTelemetry) {
        // Batch update endpoint for Temp (v0), Humidity (v1), and Motion (v3)
        url = String("/external/api/batch/update?token=") + BLYNK_AUTH + 
              "&v0=" + String(temp, 1) + 
              "&v1=" + String(hum, 1) + 
              "&v3=" + String(motion);
      } else {
        // Lightweight single-pin update endpoint for Motion only (v3) to save data limit
        url = String("/external/api/update?token=") + BLYNK_AUTH + 
              "&v3=" + String(motion);
      }

      Serial.println("[BLYNK] Sending HTTP GET Request...");
      client.print(String("GET ") + url + " HTTP/1.1\r\n");
      client.print(String("Host: ") + BLYNK_HOST + "\r\n");
      client.print("Connection: close\r\n\r\n");
      
      unsigned long timeout = millis();
      while (client.connected() && millis() - timeout < 6000UL) {
        if (client.available()) {
          String line = client.readStringUntil('\n');
        }
      }
      client.stop();
      
      Serial.println("[BLYNK] Telemetry successfully synced to cloud!");
      success = true;
      break; // Success! Exit retry loop
    } else {
      Serial.println("[WARNING] Failed to connect to blynk.cloud server.");
      client.stop();
    }

    Serial.println("[MODEM] Shutting down modem before retry...");
    powerOffModem();
    delay(3000);
  }

  if (!success) {
    Serial.println("[ERROR] All modem connection attempts failed after retries.");
  }

  Serial.println("[MODEM] Shutting down modem to conserve power...");
  powerOffModem();
  Serial.println("[MODEM] Modem shutdown complete.\n");
  return success;
}

void powerOnModem() {
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(100);
  digitalWrite(MODEM_PWRKEY, LOW);  
  delay(2000);                      
  digitalWrite(MODEM_PWRKEY, HIGH); 
  delay(6000);
}

void powerOffModem() {
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1200);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(2000);
}