// === LineGuard (Configurazione Avanzata NVS, Web, Email, OTA - v7.0 Identify Removed / URL OTA Mod) ===

#include <WiFi.h>
#include <ETH.h>
#include <WebServer.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <Preferences.h>
#include <vector>
#include <sstream>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <ESP_Mail_Client.h>
// #include <Adafruit_NeoPixel.h> // Rimossa per eliminazione funzione Identifica

// --- INCLUSIONI PER OTA ---
#include <HTTPClient.h> // Richiesta da HTTPUpdate
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
// #include <ArduinoJson.h> // Non più necessaria per OTA da URL diretto

// --- VERSIONE FIRMWARE CORRENTE ---
#define FIRMWARE_VERSION "0.9.8-URL_OTA" // Aggiornato per nuova logica OTA

// --- RIMOZIONE CONFIGURAZIONE OTA GITHUB ---
// Le costanti relative a GitHub sono state rimosse.

// --- Oggetto Preferences ---
Preferences preferences;

// --- Variabili di Configurazione ---
String nomeDispositivo;
String conf_ssid;
String conf_password;
bool conf_use_static_ip;
IPAddress conf_static_ip;
IPAddress conf_gateway;
IPAddress conf_subnet;
IPAddress conf_dns1;
String conf_monitored_pins_csv;
int conf_speed_sensor_pin;
float conf_meters_per_pulse;
String conf_email_sender;
String conf_email_sender_password;
String conf_email_smtp_server;
int conf_email_smtp_port;
String conf_email_recipient_to;
String conf_email_recipient_cc_csv;
String conf_ota_firmware_url; // NUOVA: URL per OTA diretto

// --- Valori di Default ---
const char* DEFAULT_SSID = "Francesco-Hotspot";
const char* DEFAULT_PASSWORD = "14011994otupactoby14011994otupac";
const bool DEFAULT_USE_STATIC_IP = true;
const char* DEFAULT_STATIC_IP_STR = "192.168.0.150";
const char* DEFAULT_GATEWAY_STR = "192.168.0.1";
const char* DEFAULT_SUBNET_STR = "255.255.255.0";
const char* DEFAULT_DNS1_STR = "8.8.8.8";
const char* DEFAULT_MONITORED_PINS_CSV = "39,40,41,42";
const int DEFAULT_SPEED_SENSOR_PIN = 33;
const float DEFAULT_METERS_PER_PULSE = 1.30381f;
const char* DEFAULT_EMAIL_SENDER = "info@gtispa.it";
const char* DEFAULT_EMAIL_SENDER_PASSWORD = "Cinghiale2025";
const char* DEFAULT_EMAIL_SMTP_SERVER = "82.134.248.117";
const int DEFAULT_EMAIL_SMTP_PORT = 25;
const char* DEFAULT_EMAIL_RECIPIENT_TO = "francesco.caputo@live.it";
const char* DEFAULT_EMAIL_RECIPIENT_CC_CSV = "info@francescocaputo.it";
const char* DEFAULT_OTA_FIRMWARE_URL = ""; // NUOVA: URL OTA di default (vuoto)

WebServer server(80);
#define SD_CS 4
SPIClass spiSD(HSPI);

// --- Variabili di Stato Globali ---
bool ethernetConnected = false;
bool wifiConnected = false;
bool serverStarted = false;
bool timeSynced = false;
bool sdCardInitialized = false;
volatile unsigned long logCount = 0;
bool emailAlertSentForSDFailure = false;
String ota_status_message = ""; // Messaggio per lo stato dell'OTA
std::vector<int> monitoredPinsVec;
std::vector<int> lastPinStatesVec;
std::vector<unsigned long> lastDebounceTimeVec;
const unsigned long debounceDelay = 50;
bool attemptingWiFiConnection = false;
unsigned long lastWiFiAttemptTime = 0;
const unsigned long wifiRetryDelay = 30000;

// Variabili per il calcolo della velocità
int active_speed_sensor_pin;
float active_meters_per_pulse;
float currentSpeedMetersPerMinute = 0.0;
float lastLoggedSpeed = -1.0f; 
const float SPEED_CHANGE_THRESHOLD = 0.1f; 

volatile unsigned long lastPulseTimeMillisISR = 0; 
volatile bool newPulseFlagISR = false;             
unsigned long previousPulseTimeForCalc = 0;   
const unsigned long SPEED_TIMEOUT_DURATION_MS = 10000; 

SMTPSession smtp;
int sdRetryCount = 0;
const int MAX_SD_RETRIES = 5;
unsigned long nextSDRetryTime = 0;
const unsigned long SD_RETRY_INTERVAL = 1000;
bool sdRecoveryAttemptInProgress = false;
String lastSDFailureContext = "";

// --- DICHIARAZIONI ANTICIPATE FUNZIONI ---
void loadConfiguration();
void parseMonitoredPins(const String& csv);
void generateRandomDeviceName(char* nameBuffer, size_t bufferSize);
void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
void printStatusToSerial();
void sendSDFailureEmail(const String& errorMessage);
void startWiFi();
void checkNetworkStatus();
void syncTime();
void performUpdate(String firmware_url, String new_version_name); // Modificato parametro
void handleFirmwareUpdate();
void handleOTAStatusPage();
void handleConfigPage();
void handleSaveConfig();
void startServer();
void pollInputsAndLog();
void handleSDRetryLogic();
String getFormattedTimestamp(unsigned long rawMillis = 0);
void triggerSDRecovery(const String& context);
void calculateSpeedAndLog(); 


// ISR per sensore velocità
void IRAM_ATTR handleSpeedPulse() {
  lastPulseTimeMillisISR = millis(); 
  newPulseFlagISR = true;            
}

// === Funzioni Utilità ===
void generateRandomDeviceName(char* nameBuffer, size_t bufferSize) {
  long randomNumber = random(10000, 100000);
  snprintf(nameBuffer, bufferSize, "LineGuard%ld", randomNumber);
}

void triggerSDRecovery(const String& context) {
    if (emailAlertSentForSDFailure && !sdRecoveryAttemptInProgress) {
        Serial.println("[SD Error] Problema SD: " + context + ". Fallimento definitivo precedentemente segnalato via email. Nessun nuovo tentativo automatico immediato.");
        sdCardInitialized = false;
        return;
    }
    if (!sdRecoveryAttemptInProgress) {
        Serial.println("[SD Error] Rilevato problema SD: " + context + ". Avvio tentativi di recupero.");
        sdCardInitialized = false;
        emailAlertSentForSDFailure = false; 
        sdRecoveryAttemptInProgress = true;
        sdRetryCount = 0;
        nextSDRetryTime = millis(); 
        lastSDFailureContext = context;
    } else {
        Serial.println("[SD Error] Rilevato problema SD: " + context + ", ma recupero già in corso (contesto precedente: " + lastSDFailureContext + ").");
    }
}

void parseMonitoredPins(const String& csv) {
  monitoredPinsVec.clear(); lastPinStatesVec.clear(); lastDebounceTimeVec.clear();
  std::string s_std = csv.c_str(); std::stringstream ss(s_std); std::string item;
  while (std::getline(ss, item, ',')) {
    size_t first = item.find_first_not_of(" \t\n\r\f\v"); if (std::string::npos == first) continue;
    size_t last = item.find_last_not_of(" \t\n\r\f\v"); item = item.substr(first, (last - first + 1));
    if (!item.empty()) {
      char* endptr; long pinNum = strtol(item.c_str(), &endptr, 10);
      if (*endptr == '\0' && endptr != item.c_str()) { monitoredPinsVec.push_back(static_cast<int>(pinNum));}
      else { Serial.printf("[Config] Errore parsing pin: '%s' non è valido.\n", item.c_str());}
    }
  }
  lastPinStatesVec.assign(monitoredPinsVec.size(), 0);
  lastDebounceTimeVec.assign(monitoredPinsVec.size(), 0);
  Serial.print("[Config] Pin monitorati: ");
  if (monitoredPinsVec.empty()) { Serial.print("Nessuno"); }
  else { for (size_t i=0; i<monitoredPinsVec.size(); ++i) { Serial.print(monitoredPinsVec[i]); if (i<monitoredPinsVec.size()-1) Serial.print(", ");}}
  Serial.println();
}

String getFormattedTimestamp(unsigned long rawMillis /*= 0*/) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) { 
    char buffer[30]; strftime(buffer, sizeof(buffer), "%Y-%m-%d, %H:%M:%S", &timeinfo); return String(buffer);
  } else { String fallback = ""; if (!timeSynced) fallback = "1970-01-01, 00:00:00 (No NTP Sync)"; else fallback = "0000-00-00, 00:00:00 (Time Error)"; return fallback; }
}

// === Funzione Caricamento Configurazione ===
void loadConfiguration() {
  preferences.begin("device-cfg", false);
  if (preferences.isKey("devName")) { nomeDispositivo = preferences.getString("devName"); }
  else { char tempName[32]; generateRandomDeviceName(tempName, sizeof(tempName)); nomeDispositivo = String(tempName); preferences.putString("devName", nomeDispositivo); Serial.println("[Config] Nome disp. generato: " + nomeDispositivo);}
  conf_ssid = preferences.getString("wifiSsid", DEFAULT_SSID);
  conf_password = preferences.getString("wifiPass", DEFAULT_PASSWORD);
  bool netCfgDone = preferences.getBool("netCfgDone", false);
  if (!netCfgDone) {
    conf_use_static_ip = DEFAULT_USE_STATIC_IP; preferences.putBool("useStaticIP", conf_use_static_ip);
    if (DEFAULT_USE_STATIC_IP) { preferences.putString("staticIP",DEFAULT_STATIC_IP_STR); preferences.putString("gatewayIP",DEFAULT_GATEWAY_STR); preferences.putString("subnetMask",DEFAULT_SUBNET_STR); preferences.putString("dns1IP",DEFAULT_DNS1_STR);}
    preferences.putBool("netCfgDone", true);
  } else { conf_use_static_ip = preferences.getBool("useStaticIP", DEFAULT_USE_STATIC_IP); }
  if (conf_use_static_ip) {
    conf_static_ip.fromString(preferences.getString("staticIP",DEFAULT_STATIC_IP_STR).c_str()); conf_gateway.fromString(preferences.getString("gatewayIP",DEFAULT_GATEWAY_STR).c_str());
    conf_subnet.fromString(preferences.getString("subnetMask",DEFAULT_SUBNET_STR).c_str()); conf_dns1.fromString(preferences.getString("dns1IP",DEFAULT_DNS1_STR).c_str());
  }
  conf_monitored_pins_csv = preferences.getString("pinsCSV", DEFAULT_MONITORED_PINS_CSV);
  conf_speed_sensor_pin = preferences.getInt("speedPin", DEFAULT_SPEED_SENSOR_PIN);
  conf_meters_per_pulse = preferences.getFloat("metersPulse", DEFAULT_METERS_PER_PULSE);
  conf_email_sender = preferences.getString("emailSender", DEFAULT_EMAIL_SENDER);
  conf_email_sender_password = preferences.getString("emailPass", DEFAULT_EMAIL_SENDER_PASSWORD);
  conf_email_smtp_server = preferences.getString("emailSmtp", DEFAULT_EMAIL_SMTP_SERVER);
  conf_email_smtp_port = preferences.getInt("emailPort", DEFAULT_EMAIL_SMTP_PORT);
  conf_email_recipient_to = preferences.getString("emailTo", DEFAULT_EMAIL_RECIPIENT_TO);
  conf_email_recipient_cc_csv = preferences.getString("emailCcCsv", DEFAULT_EMAIL_RECIPIENT_CC_CSV);
  
  // NUOVA SEZIONE PER URL OTA
  conf_ota_firmware_url = preferences.getString("otaFwUrl", DEFAULT_OTA_FIRMWARE_URL);

  // Salva i valori di default se le chiavi non esistono
  if (!preferences.isKey("wifiSsid")) preferences.putString("wifiSsid",conf_ssid); 
  if (!preferences.isKey("wifiPass")) preferences.putString("wifiPass",conf_password);
  if (!preferences.isKey("pinsCSV")) preferences.putString("pinsCSV",conf_monitored_pins_csv); 
  if (!preferences.isKey("speedPin")) preferences.putInt("speedPin",conf_speed_sensor_pin);
  if (!preferences.isKey("metersPulse")) preferences.putFloat("metersPulse",conf_meters_per_pulse); 
  if (!preferences.isKey("emailSender")) preferences.putString("emailSender",conf_email_sender);
  if (!preferences.isKey("emailPass")) preferences.putString("emailPass",conf_email_sender_password); 
  if (!preferences.isKey("emailSmtp")) preferences.putString("emailSmtp",conf_email_smtp_server);
  if (!preferences.isKey("emailPort")) preferences.putInt("emailPort",conf_email_smtp_port); 
  if (!preferences.isKey("emailTo")) preferences.putString("emailTo",conf_email_recipient_to);
  if (!preferences.isKey("emailCcCsv")) preferences.putString("emailCcCsv",conf_email_recipient_cc_csv);
  if (!preferences.isKey("otaFwUrl")) preferences.putString("otaFwUrl", conf_ota_firmware_url); // Salva il default (o il valore letto se la chiave non c'era)

  preferences.end();
  active_speed_sensor_pin = conf_speed_sensor_pin; active_meters_per_pulse = conf_meters_per_pulse;
  Serial.println("[Config] Nome Dispositivo: " + nomeDispositivo);
  Serial.println("[Config] SSID: " + conf_ssid);
  Serial.println("[Config] Uso IP Statico: " + String(conf_use_static_ip ? "Si" : "No"));
  if (conf_use_static_ip) { Serial.println("[Config] IP Statico: " + conf_static_ip.toString()); }
  Serial.println("[Config] Pin Sensore Velocità: " + String(active_speed_sensor_pin));
  Serial.print("[Config] Metri per Impulso: "); Serial.println(active_meters_per_pulse, 5);
  Serial.println("[Config] Email Mittente: " + conf_email_sender);
  Serial.println("[Config] Email Destinatario TO: " + conf_email_recipient_to);
  Serial.println("[Config] Email Destinatari CC: " + conf_email_recipient_cc_csv);
  Serial.println("[Config] URL Firmware OTA: " + (conf_ota_firmware_url.isEmpty() ? "Non impostato" : conf_ota_firmware_url)); 
}

// === Setup ===
void setup() {
  Serial.begin(9600);
  while (!Serial) delay(10);
  delay(1000);  Serial.println(F("\n\n=== LineGuard Booting ==="));
  Serial.println("[Setup] Firmware Version: " + String(FIRMWARE_VERSION));

  const int analogSeedPin = 2;
  randomSeed(analogRead(analogSeedPin));
  Serial.println("[Setup] Random seed initialized using analogRead on GPIO" + String(analogSeedPin));

  loadConfiguration();
  parseMonitoredPins(conf_monitored_pins_csv);

  Serial.println("[Setup] Init network interfaces (WiFi events, ETH)...");
  WiFi.onEvent(WiFiEvent);
  ETH.begin();

  Serial.println(F("[Setup] Config speed sensor..."));
  if (active_speed_sensor_pin >= 0 && active_speed_sensor_pin <= 48 && digitalPinToInterrupt(active_speed_sensor_pin) != -1) {
    pinMode(active_speed_sensor_pin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(active_speed_sensor_pin), handleSpeedPulse, RISING);
    Serial.printf("[Setup] Speed sensor OK: GPIO%d.\n", active_speed_sensor_pin);
  } else {
    Serial.printf("[Setup] Err speed sensor pin (GPIO%d) invalid or no interrupt.\n", active_speed_sensor_pin);
  }

  Serial.print("[Setup] Init SPI for SD (HSPI)... ");
  spiSD.begin(7, 5, 6, SD_CS); // CLK, MISO, MOSI, CS
  Serial.println("SPI init.");

  Serial.print("[Setup] Init SD Card (SPI)... ");
  if (SD.begin(SD_CS, spiSD, 8000000)) {
    Serial.println("SD Card OK.");
    sdCardInitialized = true;
    emailAlertSentForSDFailure = false;
    if (!SD.exists("/log.csv")) {
        Serial.print("[Setup] /log.csv not found. Creating... ");
        File f = SD.open("/log.csv", FILE_WRITE);
        if (f) {
            if (f.println("Dispositivo,Timestamp,Tipo,Descrizione,Valore")) {
                 Serial.println("File /log.csv created.");
            } else {
                 Serial.println("Err scrittura header /log.csv.");
                 triggerSDRecovery("Creazione header /log.csv fallita @setup");
            }
            f.close();
        } else {
            Serial.println("Err create /log.csv.");
            triggerSDRecovery("Creazione /log.csv fallita @setup");
        }
    } else {
        Serial.println("[Setup] /log.csv exists.");
        File f_test = SD.open("/log.csv", FILE_APPEND);
        if (!f_test) {
            Serial.println("[Setup] Errore apertura /log.csv esistente in append.");
            triggerSDRecovery("Errore test append /log.csv @setup");
        } else {
            f_test.close();
            Serial.println("[Setup] /log.csv esistente è scrivibile.");
        }
    }
  } else {
    Serial.println("SD Card init FAILED! Avvio tentativi di recupero in background.");
    triggerSDRecovery("SD Card init failed @setup");
  }

  Serial.printf("[Setup] Heap disponibile: %u B\n", ESP.getFreeHeap());
  Serial.println("[Setup] Config monitored pins (NVS/default)...");
  if (!monitoredPinsVec.empty()) {
    for (size_t i = 0; i < monitoredPinsVec.size(); ++i) {
      int pin = monitoredPinsVec[i];
      if (pin >= 0 && pin <= 48 && digitalPinToInterrupt(pin) != -1) { 
        pinMode(pin, INPUT_PULLDOWN);
        if (i < lastPinStatesVec.size()) lastPinStatesVec[i] = digitalRead(pin);
        if (i < lastDebounceTimeVec.size()) lastDebounceTimeVec[i] = 0;
        Serial.printf("[Setup] Pin GPIO%d (Idx %u) PULLDOWN. State: %d\n", pin, (unsigned int)i, (i < lastPinStatesVec.size() ? lastPinStatesVec[i] : -1));
      } else {
        Serial.printf("[Setup] Err: Pin GPIO%d (idx %u) invalid or no interrupt. Skipping.\n", pin, (unsigned int)i);
      }
    }
  } else {
    Serial.println("[Setup] Warn: No monitored pins!");
  }

  Serial.println("=== Setup Complete. System Running. ===");
}


// === NUOVA Funzione per Calcolo Velocità e Log ===
void calculateSpeedAndLog() {
    unsigned long currentMillisLoop = millis(); 
    unsigned long localLastPulseTime;
    bool localNewPulseReceived;

    noInterrupts();
    localNewPulseReceived = newPulseFlagISR;
    localLastPulseTime = lastPulseTimeMillisISR; 
    if (localNewPulseReceived) {
        newPulseFlagISR = false; 
    }
    interrupts();

    if (localNewPulseReceived) {
        if (previousPulseTimeForCalc != 0 && localLastPulseTime > previousPulseTimeForCalc) {
            unsigned long deltaTime = localLastPulseTime - previousPulseTimeForCalc;
            if (deltaTime > 0 && deltaTime < (SPEED_TIMEOUT_DURATION_MS + 2000) ) { 
                if (active_meters_per_pulse > 0.00001f && deltaTime > 0) { 
                    currentSpeedMetersPerMinute = (active_meters_per_pulse * 60000.0) / (float)deltaTime;
                    Serial.printf("[SPEED_CALC] DeltaT: %lu ms, Speed: %.2f m/min\n", deltaTime, currentSpeedMetersPerMinute);
                } else {
                    Serial.printf("[SPEED_CALC] DeltaT: %lu ms, ma active_meters_per_pulse non valido (%.5f) o deltaTime zero.\n", deltaTime, active_meters_per_pulse);
                }
            } else {
                Serial.printf("[SPEED_CALC] DeltaT non valido (%lu ms) o troppo grande. Nessun calcolo velocità.\n", deltaTime);
            }
        } else {
             Serial.println("[SPEED_CALC] Primo impulso ricevuto (o dopo reset). In attesa del secondo per calcolare DeltaT.");
        }
        previousPulseTimeForCalc = localLastPulseTime; 
    }

    unsigned long timeSinceLastPulseISR = currentMillisLoop - localLastPulseTime;

    if (previousPulseTimeForCalc == 0) { 
        if (localLastPulseTime == 0 && currentMillisLoop > SPEED_TIMEOUT_DURATION_MS) { 
             if (currentSpeedMetersPerMinute != 0.0) {
                currentSpeedMetersPerMinute = 0.0;
                Serial.println("[SPEED_CALC] Timeout iniziale. Nessun impulso mai ricevuto. Velocità impostata a 0.");
             }
        } else if (localLastPulseTime != 0 && timeSinceLastPulseISR > SPEED_TIMEOUT_DURATION_MS) {
             if (currentSpeedMetersPerMinute != 0.0) {
                currentSpeedMetersPerMinute = 0.0;
                Serial.printf("[SPEED_CALC] Timeout dopo il primo impulso (senza un secondo). Velocità impostata a 0. Time since last: %lu\n", timeSinceLastPulseISR);
             }
        }
    } else { 
        if (timeSinceLastPulseISR > SPEED_TIMEOUT_DURATION_MS) {
            if (currentSpeedMetersPerMinute != 0.0) {
                currentSpeedMetersPerMinute = 0.0;
                previousPulseTimeForCalc = 0; 
                Serial.printf("[SPEED_CALC] Timeout! Nessun impulso da oltre %lu ms. Velocità impostata a 0.\n", SPEED_TIMEOUT_DURATION_MS);
            }
        }
    }

    bool shouldLogSpeed = false;
    if (active_meters_per_pulse > 0.00001f) { 
        if (abs(currentSpeedMetersPerMinute - lastLoggedSpeed) > SPEED_CHANGE_THRESHOLD) {
            shouldLogSpeed = true;
        }
        if (!shouldLogSpeed && ((currentSpeedMetersPerMinute == 0.0 && lastLoggedSpeed != 0.0) ||
                                 (currentSpeedMetersPerMinute != 0.0 && lastLoggedSpeed == 0.0))) {
            if (currentSpeedMetersPerMinute == 0.0 || (currentSpeedMetersPerMinute != 0.0 && previousPulseTimeForCalc != 0)){
                 shouldLogSpeed = true;
            }
        }
        if (lastLoggedSpeed < 0.0f && currentSpeedMetersPerMinute >= 0.0f) {
            if (currentSpeedMetersPerMinute == 0.0 || (currentSpeedMetersPerMinute != 0.0 && previousPulseTimeForCalc != 0)) {
                 shouldLogSpeed = true;
            }
        }
    }

    if (shouldLogSpeed) {
        if (sdCardInitialized) {
            String timestamp = getFormattedTimestamp();
            File f = SD.open("/log.csv", FILE_APPEND);
            if (f) {
                if (!f.printf("%s,%s,V,Velocita_mpm,%.2f\n", nomeDispositivo.c_str(), timestamp.c_str(), currentSpeedMetersPerMinute)){
                    Serial.printf("[Loop] Errore scrittura velocità su SD (printf). Log: %s,%s,V,Velocita_mpm,%.2f\n", nomeDispositivo.c_str(),timestamp.c_str(),currentSpeedMetersPerMinute);
                    triggerSDRecovery("Err scrittura CSV (velocità)");
                }
                f.close();
                logCount++;
                lastLoggedSpeed = currentSpeedMetersPerMinute;
            } else {
                Serial.printf("[Loop] Errore apertura SD per scrittura velocità. Log: %s,%s,V,Velocita_mpm,%.2f\n", nomeDispositivo.c_str(),timestamp.c_str(),currentSpeedMetersPerMinute);
                triggerSDRecovery("Err apertura CSV (velocità)");
            }
        } else {
            String timestamp = getFormattedTimestamp();
            Serial.printf("LOG TO SERIAL (No SD): %s,%s,V,Velocita_mpm,%.2f\n", nomeDispositivo.c_str(), timestamp.c_str(), currentSpeedMetersPerMinute);
            lastLoggedSpeed = currentSpeedMetersPerMinute;
            if (!sdRecoveryAttemptInProgress) {
                 triggerSDRecovery("SD non inizializzata durante log velocità");
            }
        }
    }
}

// === Loop Principale ===
void loop() {
  if (Serial.available()>0){ String cmd=Serial.readStringUntil('\n'); cmd.trim();
    if(cmd.equalsIgnoreCase("stato")){ printStatusToSerial(); }
    else if(cmd.equalsIgnoreCase("erase_nvs_config")){ Serial.println("ERASE NVS? Type 'CONFIRM_ERASE'"); unsigned long st=millis(); String cCmd="";
      while(millis()-st<10000){ if(Serial.available()){cCmd=Serial.readStringUntil('\n');cCmd.trim();break;}delay(100);}
      if(cCmd.equalsIgnoreCase("CONFIRM_ERASE")){ preferences.begin("device-cfg",false); if(preferences.clear())Serial.println("NVS erased.");else Serial.println("Err erasing NVS.");
        preferences.end();Serial.println("Rebooting...");delay(1000);ESP.restart();}else Serial.println("NVS erase cancelled.");}
    else if(cmd.equalsIgnoreCase("ota_update_test")) { // Questo comando ora userà l'URL configurato
        Serial.println("Avvio test OTA da comando seriale (userà URL da NVS)...");
        handleFirmwareUpdate(); 
    } else if (cmd.equalsIgnoreCase("test_sd_fail")) {
        Serial.println("Simulazione fallimento scrittura SD per test recovery...");
        triggerSDRecovery("Test fallimento SD da seriale");
    }
    else if(cmd.length()>0){Serial.print(F("Unknown cmd: "));Serial.println(cmd);}}

  if(serverStarted) server.handleClient();
  checkNetworkStatus();

  static unsigned long lastNtp=0;
  if(millis()-lastNtp > 3600000UL || (!timeSynced && (wifiConnected||ethernetConnected))){
      syncTime();
      lastNtp=millis();
  }

  handleSDRetryLogic();

  if (!sdRecoveryAttemptInProgress) {
    pollInputsAndLog();
  }

  calculateSpeedAndLog();

  delay(10); 
}


// === Funzione Stampa Stato Seriale ===
void printStatusToSerial() {
  Serial.println(F("\n\n--- LineGuard Status (via Seriale) ---"));
  Serial.println(F("Informazioni Dispositivo:"));
  Serial.print(F("  Nome Dispositivo: ")); Serial.println(nomeDispositivo);
  Serial.print(F("  Versione Firmware: ")); Serial.println(FIRMWARE_VERSION);
  Serial.print(F("  Hostname (mDNS): ")); Serial.print(nomeDispositivo); Serial.println(F(".local"));
  Serial.println(F("\nStato della Rete:"));
  Serial.print(F("  Ethernet: ")); Serial.println(ethernetConnected ? F("Connesso") : F("Disconnesso"));
  if (ethernetConnected) { Serial.print(F("  Ethernet IP: ")); Serial.println(ETH.localIP().toString()); }
  String wifiStatusString = F("Disconnesso");
  if (attemptingWiFiConnection) { wifiStatusString = String(F("Connessione a ")) + conf_ssid + F("...");  }
  else if (wifiConnected) { wifiStatusString = String(F("Connesso (")) + WiFi.SSID() + F(")"); }
  Serial.print(F("  Wi-Fi: ")); Serial.println(wifiStatusString);
  if (wifiConnected) { Serial.print(F("  Wi-Fi IP: ")); Serial.println(WiFi.localIP().toString()); }
  Serial.print(F("  IP Attivo: ")); Serial.println(ethernetConnected ? ETH.localIP().toString() : (wifiConnected ? WiFi.localIP().toString() : F("N/A")));
  Serial.println(F("\nConfigurazione di Rete (NVS):"));
  Serial.print(F("  SSID: ")); Serial.println(conf_ssid);
  if(conf_use_static_ip) { Serial.println(F("  Modalità IP: Statico")); Serial.print(F("  IP Statico: ")); Serial.println(conf_static_ip.toString()); Serial.print(F("  Gateway: ")); Serial.println(conf_gateway.toString()); Serial.print(F("  Subnet: ")); Serial.println(conf_subnet.toString()); Serial.print(F("  DNS: ")); Serial.println(conf_dns1.toString()); }
  else { Serial.println(F("  Modalità IP: DHCP")); }
  Serial.println(F("\nConfigurazione Email (NVS):"));
  Serial.print(F("  Mittente: ")); Serial.println(conf_email_sender); Serial.print(F("  Server: ")); Serial.print(conf_email_smtp_server); Serial.print(F(":")); Serial.println(conf_email_smtp_port);
  Serial.print(F("  Dest. TO: ")); Serial.println(conf_email_recipient_to); Serial.print(F("  Dest. CC: ")); Serial.println(conf_email_recipient_cc_csv);
  Serial.println(F("\nConfigurazione OTA (NVS):")); // NUOVA SEZIONE
  Serial.print(F("  URL Firmware: ")); Serial.println(conf_ota_firmware_url.isEmpty() ? "Non impostato" : conf_ota_firmware_url);
  Serial.println(F("\nOra di Sistema:"));
  Serial.print(F("  NTP Sync: ")); Serial.println(timeSynced ? F("Si") : F("No")); Serial.print(F("  Ora Attuale: ")); Serial.println(getFormattedTimestamp());
  Serial.print(F("\nPin Monitorati (CSV): ")); Serial.println(conf_monitored_pins_csv);
  Serial.print(F("Pin Sensore Velocità: ")); Serial.println(active_speed_sensor_pin); Serial.print(F("Metri/Impulso: ")); Serial.println(active_meters_per_pulse, 5);
  uint32_t heapFree = ESP.getFreeHeap(); uint32_t heapTotal = ESP.getHeapSize();
  Serial.println(F("\nMemoria:")); Serial.print(F("  Heap Libera: ")); Serial.print(heapFree / 1024); Serial.println(F(" KB")); Serial.print(F("  Heap Totale: ")); Serial.print(heapTotal / 1024); Serial.println(F(" KB"));
  Serial.println(F("\nScheda SD:")); Serial.print(F("  Inizializzata: ")); Serial.println(sdCardInitialized ? F("Si") : (sdRecoveryAttemptInProgress ? F("Recupero in corso...") : F("No")));
  if (sdCardInitialized) { uint64_t csMB=SD.cardSize()/(1024*1024); uint64_t tbS=SD.totalBytes(); uint64_t ubS=SD.usedBytes(); uint64_t cuMB=ubS/(1024*1024); uint64_t cfMB=(tbS-ubS)/(1024*1024);
    Serial.print(F("  Dimensione: ")); Serial.print(csMB); Serial.println(F(" MB")); Serial.print(F("  Usata: ")); Serial.print(cuMB); Serial.println(F(" MB")); Serial.print(F("  Libera: ")); Serial.print(cfMB); Serial.println(F(" MB")); Serial.print(F("  Log scritti: ")); Serial.println(logCount); }
  else if (!sdRecoveryAttemptInProgress) { Serial.println(F("  Scheda SD non disponibile.")); }
  Serial.println(F("\nVelocità di Produzione:")); Serial.print(F("  Metri/minuto: ")); Serial.println(currentSpeedMetersPerMinute, 2);
  Serial.println(F("--- Fine Stato LineGuard ---"));
}

// === Funzione Invio Email per Errore SD ===
void sendSDFailureEmail(const String& errorMessage) {
  if (!ethernetConnected && !wifiConnected) {
    Serial.println("[Email] Impossibile inviare email: nessuna connessione di rete.");
    return;
  }
  if (emailAlertSentForSDFailure) { 
    Serial.println("[Email] Email di errore SD già inviata per questo evento di fallimento.");
    return;
  }
  if (conf_email_sender.length() == 0 || conf_email_recipient_to.length() == 0 || conf_email_smtp_server.length() == 0) {
    Serial.println("[Email] Configurazione email mittente, destinatario TO o server SMTP mancante. Impossibile inviare.");
    return;
  }

  Serial.println("[Email] Tentativo di invio email per errore SD: " + errorMessage);

  ESP_Mail_Session session_obj; 
  session_obj.server.host_name = conf_email_smtp_server.c_str();
  session_obj.server.port = conf_email_smtp_port;
  session_obj.login.email = conf_email_sender.c_str();
  session_obj.login.password = conf_email_sender_password.c_str();
  session_obj.login.user_domain = ""; 
  session_obj.time.gmt_offset = 1; 
  session_obj.time.day_light_offset = 1; 
  
  SMTP_Message message;
  message.sender.name = nomeDispositivo;
  message.sender.email = conf_email_sender.c_str();
  message.subject = "AVVISO: Problema Scheda SD su Dispositivo " + nomeDispositivo;
  
  String htmlMsg = "<h2>Allarme Problema Scheda SD</h2>";
  htmlMsg += "<p>Il dispositivo <b>" + nomeDispositivo + "</b> ha riscontrato un problema con la scheda SD.</p>";
  htmlMsg += "<p><b>Dettaglio Problema:</b> " + errorMessage + "</p>";
  htmlMsg += "<p><b>Ora Rilevamento:</b> " + getFormattedTimestamp() + "</p>";
  htmlMsg += "<p>Dopo " + String(MAX_SD_RETRIES) + " tentativi di recupero, il problema persiste.</p>";
  htmlMsg += "<p>Si prega di verificare lo stato del dispositivo e della scheda SD.</p>";
  message.html.content = htmlMsg;
  message.html.charSet = "utf-8";
  message.html.transfer_encoding = Content_Transfer_Encoding::enc_qp;
  message.addRecipient(conf_email_recipient_to, conf_email_recipient_to.c_str());

  if (conf_email_recipient_cc_csv.length() > 0) {
    std::string s_std = conf_email_recipient_cc_csv.c_str();
    std::stringstream ss(s_std);
    std::string email_item;
    while (std::getline(ss, email_item, ',')) {
      size_t first = email_item.find_first_not_of(" \t");
      if (std::string::npos == first) continue;
      size_t last = email_item.find_last_not_of(" \t");
      email_item = email_item.substr(first, (last - first + 1));
      if (email_item.length() > 0) {
        message.addCc(email_item.c_str());
        Serial.println("[Email] Aggiunto CC: " + String(email_item.c_str()));
      }
    }
  }
  
  smtp.debug(0);  

  Serial.println("[Email] Connessione al server SMTP...");
  if (!smtp.connect(&session_obj)) {  
    Serial.printf("[Email] Connessione SMTP fallita: %s\n", smtp.errorReason().c_str());
    return;  
  }

  Serial.println("[Email] Invio del messaggio...");
  if (!MailClient.sendMail(&smtp, &message)) {
    Serial.printf("[Email] Invio email fallito: %s\n", smtp.errorReason().c_str());
  } else {
    Serial.println("[Email] Email inviata con successo!");
    emailAlertSentForSDFailure = true;  
  }
  if(smtp.connected()) smtp.closeSession();
}

// === Gestione Eventi Wi-Fi ed Ethernet ===
void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.printf("[WiFiEvent] event: %d\n", event);
  switch (event) {
    case ARDUINO_EVENT_ETH_START: ETH.setHostname(nomeDispositivo.c_str()); Serial.println("[WiFiEvent] Ethernet Started. Hostname: " + nomeDispositivo); break;
    case ARDUINO_EVENT_ETH_CONNECTED: Serial.println("[WiFiEvent] Ethernet Connected"); break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.println("[WiFiEvent] Ethernet IP: " + ETH.localIP().toString());
      ethernetConnected = true;
      if (wifiConnected) { Serial.println("[WiFiEvent] Ethernet connesso, disconnessione da WiFi..."); WiFi.disconnect(true); wifiConnected = false; attemptingWiFiConnection = false; }
      if (!serverStarted) startServer();
      if (!timeSynced) syncTime();
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED: Serial.println("[WiFiEvent] Ethernet Disconnected."); ethernetConnected = false; break;
    case ARDUINO_EVENT_WIFI_STA_START: WiFi.setHostname(nomeDispositivo.c_str()); Serial.println("[WiFiEvent] WiFi Station interface started. Hostname:" + nomeDispositivo); break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED: Serial.println("[WiFiEvent] WiFi MAC Connected to AP: " + String(reinterpret_cast<char*>(info.wifi_sta_connected.ssid))); break; 
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.println("[WiFiEvent] WiFi IP: " + WiFi.localIP().toString());
      wifiConnected = true; attemptingWiFiConnection = false;
      if (ethernetConnected) { Serial.println("[WiFiEvent] WiFi connesso, ma Ethernet è preferito. Disconnessione WiFi..."); WiFi.disconnect(true); wifiConnected = false; }
      else { if (!serverStarted) startServer(); if (!timeSynced) syncTime(); }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: Serial.println("[WiFiEvent] Disconnected from WiFi AP."); wifiConnected = false; attemptingWiFiConnection = false; timeSynced = false; break; 
    default: break;
  }
}

// === Funzioni di Rete Ausiliarie ===
void startWiFi() {
  if (ethernetConnected || wifiConnected || attemptingWiFiConnection) return; 
  if (millis() - lastWiFiAttemptTime < wifiRetryDelay && lastWiFiAttemptTime != 0) return; 
  Serial.println("[WiFi] Attempting to connect to WiFi: " + conf_ssid);
  attemptingWiFiConnection = true; lastWiFiAttemptTime = millis();
  if (conf_use_static_ip) {
    if (!WiFi.config(conf_static_ip, conf_gateway, conf_subnet, conf_dns1)) { Serial.println("[WiFi] STA Failed to configure static IP!"); }
  } else { WiFi.config(IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0)); } 
  WiFi.begin(conf_ssid.c_str(), conf_password.c_str());
  Serial.println("[WiFi] Connection process initiated.");
}
void checkNetworkStatus() {
  if (!ethernetConnected && !wifiConnected && !attemptingWiFiConnection) {  
      startWiFi(); 
  } else if (ethernetConnected && WiFi.status() == WL_CONNECTED) {  
      Serial.println("[NetworkCheck] Ethernet active, ensuring WiFi is disconnected.");  
      WiFi.disconnect(true);  
      wifiConnected = false;  
      attemptingWiFiConnection = false;  
  }
}
void syncTime() {
  if (!timeSynced && (ethernetConnected || wifiConnected)) {  
    Serial.println("[Time] Attempting to sync time with NTP server...");
    configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.nist.gov");  
    struct tm timeinfo; int retries = 0; const int maxRetries = 20;  
    while(!getLocalTime(&timeinfo, 0) && retries < maxRetries) { if(timeinfo.tm_year > (2000-1900)) break; Serial.print("."); delay(50); retries++; }  
    if (timeinfo.tm_year > (2000 - 1900)) { Serial.println("\n[Time] Time synchronized successfully!"); Serial.printf("[Time] Current time: %s\n", getFormattedTimestamp().c_str()); timeSynced = true; }
    else { Serial.println("\n[Time][Error] Failed to synchronize time via NTP after retries."); timeSynced = false; }
  }
}

// --- FUNZIONI OTA (MODIFICATE) ---
void performUpdate(String firmware_url, String new_version_name) {
    ota_status_message = "Avvio aggiornamento alla versione '" + new_version_name + "' da: " + firmware_url + "<br>Attendere il riavvio del dispositivo...";
    Serial.println(ota_status_message);
    
    String html = "<html><head><title>Aggiornamento Firmware</title><meta http-equiv='refresh' content='30;url=/status'></head>"; // Aumentato timeout refresh
    html += "<body><h1>Aggiornamento Firmware</h1><p>" + ota_status_message + "</p>";
    html += "<p>Il dispositivo si riavvier&agrave; automaticamente se l'aggiornamento ha successo. Questo pu&ograve; richiedere alcuni minuti.</p>";
    html += "<p>Se il dispositivo non si riavvia entro 1-2 minuti, controlla la console seriale per errori.</p>";
    html += "<p><a href='/status'>Torna allo stato (dopo il riavvio o in caso di problemi)</a></p></body></html>";
    server.send(200, "text/html", html);
    delay(200); 

    WiFiClientSecure clientSecureOTA; 
    clientSecureOTA.setInsecure(); // Permette URL HTTPS con certificati autofirmati/non validati e dovrebbe funzionare anche per HTTP
    
    t_httpUpdate_return ret = httpUpdate.update(clientSecureOTA, firmware_url);

    switch (ret) {
        case HTTP_UPDATE_FAILED:
            ota_status_message = "Errore aggiornamento: " + httpUpdate.getLastErrorString() + " (Codice: " + String(httpUpdate.getLastError()) +")";
            Serial.printf("[OTA] Update failed: Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            break;
        case HTTP_UPDATE_NO_UPDATES: 
            ota_status_message = "Nessun aggiornamento effettuato (il server potrebbe aver indicato che non era necessario o l'URL era identico)."; 
            Serial.println("[OTA] No updates performed by server.");
            break;
        case HTTP_UPDATE_OK:
            ota_status_message = "Aggiornamento completato con successo! Riavvio...";
            Serial.println("[OTA] Update OK! Rebooting...");
            // Il riavvio è gestito da httpUpdate
            break;
        default:
            ota_status_message = "Risultato aggiornamento sconosciuto: " + String(ret);
            Serial.printf("[OTA] Unknown update result: %d\n", ret);
            break;
    }
}

void handleFirmwareUpdate() {
    if (!wifiConnected && !ethernetConnected) {
        ota_status_message = "Errore: Nessuna connessione di rete per eseguire l'aggiornamento.";
        Serial.println("[OTA] " + ota_status_message);
        server.sendHeader("Location", "/otastatus", true);
        server.send(302, "text/plain", "");
        return;
    }

    if (conf_ota_firmware_url.isEmpty()) {
        ota_status_message = "Errore: L'URL del firmware OTA non è configurato. Vai alla <a href='/config'>pagina di configurazione</a> per impostarlo.";
        Serial.println("[OTA] " + ota_status_message);
        server.sendHeader("Location", "/otastatus", true);
        server.send(302, "text/plain", "");
        return;
    }

    if (!conf_ota_firmware_url.startsWith("http://") && !conf_ota_firmware_url.startsWith("https://")) {
        ota_status_message = "Errore: L'URL del firmware OTA fornito non sembra valido (deve iniziare con http:// o https://).<br>URL: " + conf_ota_firmware_url;
        Serial.println("[OTA] " + ota_status_message);
        server.sendHeader("Location", "/otastatus", true); 
        server.send(302, "text/plain", "");
        return;
    }

    String pseudo_version = "dall'URL specificato";
    int lastSlash = conf_ota_firmware_url.lastIndexOf('/');
    if (lastSlash != -1 && lastSlash < conf_ota_firmware_url.length() - 1) {
        pseudo_version = conf_ota_firmware_url.substring(lastSlash + 1);
    }

    ota_status_message = "Tentativo di aggiornamento firmware alla versione '" + pseudo_version + "' dall'URL: " + conf_ota_firmware_url;
    Serial.println("[OTA] " + ota_status_message);
    
    performUpdate(conf_ota_firmware_url, pseudo_version); 
    
    // Se performUpdate ritorna (es. per un errore), ota_status_message sarà già aggiornato.
    // Reindirizza a /otastatus per mostrare il messaggio di errore.
    if (ota_status_message.startsWith("Errore aggiornamento:") || ota_status_message.startsWith("Nessun aggiornamento")) {
        String tempRedirectHtml = "<html><head><title>Stato Aggiornamento</title><meta http-equiv='refresh' content='0;url=/otastatus'></head><body>Reindirizzamento a stato OTA...</body></html>";
        server.send(200, "text/html", tempRedirectHtml);
    }
    // Se performUpdate ha successo, il dispositivo si riavvia prima di questo punto.
}

void handleOTAStatusPage() {
    String html = "<html><head><title>Stato Aggiornamento Firmware</title>";
    // Non fare refresh se l'aggiornamento è stato avviato (attesa riavvio) o se c'è un errore specifico da leggere.
    if (!ota_status_message.startsWith("Avvio aggiornamento") && 
        !ota_status_message.startsWith("Tentativo di aggiornamento") &&
        !ota_status_message.startsWith("Aggiornamento completato") && 
        !ota_status_message.startsWith("Errore aggiornamento:") && 
        !ota_status_message.startsWith("Errore: L'URL del firmware OTA non è configurato") &&
        !ota_status_message.startsWith("Errore: L'URL del firmware OTA fornito non sembra valido")
        ) {
         // html += "<meta http-equiv='refresh' content='10;url=/status'>"; // Potrebbe essere rimosso o modificato
    }
    html += "<style>body {font-family: Arial, sans-serif; margin: 20px;} h1 {color: #333;} p {line-height: 1.6;} a {color: #007bff; text-decoration: none;} a:hover{text-decoration: underline;}</style>";
    html += "</head><body><h1>Stato Aggiornamento Firmware</h1>";
    html += "<p>" + ota_status_message + "</p>";

    if (ota_status_message.startsWith("Avvio aggiornamento") || ota_status_message.startsWith("Tentativo di aggiornamento") || ota_status_message.startsWith("Aggiornamento completato")) {
        html += "<p>Se l'aggiornamento &egrave; stato avviato con successo, il dispositivo si riavvier&agrave; a breve.</p>";
    }
    
    if (ota_status_message.startsWith("Errore: L'URL del firmware OTA non è configurato") || 
        ota_status_message.startsWith("Errore: L'URL del firmware OTA fornito non sembra valido")) {
        html += "<p><a href='/config'>Vai alla Pagina di Configurazione</a> per impostare o correggere l'URL.</p>";
    }

    html += "<p><a href='/status'>Torna alla pagina di Stato Principale</a></p>";
    html += "<p><a href='/doupdate'>Tenta nuovamente l'aggiornamento (se configurato)</a></p>"; // Link per ritentare
    html += "</body></html>";
    server.send(200, "text/html", html);
}


// === Web Server Handlers ===
void handleConfigPage() {
  String html = "<html><head><title>Configurazione Dispositivo</title>";
  html += "<style>body {font-family: Arial, sans-serif; margin: 20px; background-color: #f4f4f4; color: #333;}";
  html += "h1 {color: #0056b3;}";
  html += "form {background-color: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1);}";
  html += "fieldset {border: 1px solid #ccc; border-radius: 5px; margin-bottom: 20px; padding: 15px;}";
  html += "legend {font-weight: bold; font-size: 1.1em; color: #0056b3;}";
  html += "label {display: inline-block; width: 220px; margin-bottom: 8px; vertical-align: top;}";
  html += "input[type='text'], input[type='number'], input[type='password'] {width: calc(100% - 230px); padding: 8px; margin-bottom: 12px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box;}";
  html += "input[type='checkbox'] {margin-right: 5px; vertical-align: middle;}";
  html += "label[for='useStaticIP'] {width: auto;}";  
  html += "input[type='submit'] {background-color: #28a745; color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer; font-size: 1em;}";
  html += "input[type='submit']:hover {background-color: #218838;}";
  html += "small {color: #555; display: block; margin-top: -8px; margin-bottom: 10px;}";
  html += "a {color: #007bff; text-decoration: none;} a:hover {text-decoration: underline;}";
  html += "</style>";
  html += "</head><body>";
  html += "<h1>Configurazione Dispositivo: " + nomeDispositivo + "</h1>";
  html += "<form action='/saveconfig' method='POST'>";
  html += "<fieldset><legend>Generale</legend>";
  html += "<label for='devName'>Nome Dispositivo:</label><input type='text' id='devName' name='devName' value='" + nomeDispositivo + "' required><br>";
  html += "</fieldset>";
  html += "<fieldset><legend>Configurazione WiFi</legend>";
  html += "<label for='wifiSsid'>SSID:</label><input type='text' id='wifiSsid' name='wifiSsid' value='" + conf_ssid + "' required><br>";
  html += "<label for='wifiPass'>Password:</label><input type='password' id='wifiPass' name='wifiPass' value='" + conf_password + "'><br><small>(Lascia vuoto per non cambiare)</small><br>";
  html += "</fieldset>";
  html += "<fieldset><legend>Configurazione Rete IP</legend>";
  html += "<input type='checkbox' id='useStaticIP' name='useStaticIP' value='1'" + String(conf_use_static_ip ? " checked" : "") + "> <label for='useStaticIP'>Usa IP Statico</label><br><br>";
  html += "<label for='staticIP'>IP Statico:</label><input type='text' id='staticIP' name='staticIP' value='" + (conf_use_static_ip ? conf_static_ip.toString() : DEFAULT_STATIC_IP_STR) + "'><br>";
  html += "<label for='gatewayIP'>Gateway:</label><input type='text' id='gatewayIP' name='gatewayIP' value='" + (conf_use_static_ip ? conf_gateway.toString() : DEFAULT_GATEWAY_STR) + "'><br>";
  html += "<label for='subnetMask'>Subnet Mask:</label><input type='text' id='subnetMask' name='subnetMask' value='" + (conf_use_static_ip ? conf_subnet.toString() : DEFAULT_SUBNET_STR) + "'><br>";
  html += "<label for='dns1IP'>DNS1:</label><input type='text' id='dns1IP' name='dns1IP' value='" + (conf_use_static_ip ? conf_dns1.toString() : DEFAULT_DNS1_STR) + "'><br>";
  html += "</fieldset>";
  html += "<fieldset><legend>Configurazione Pin Monitorati</legend>";
  html += "<label for='pinsCSV'>Pin (CSV):</label><input type='text' id='pinsCSV' name='pinsCSV' value='" + conf_monitored_pins_csv + "'><br>";
  html += "<small>Es. 39,40,41,42 (verifica pin validi per ESP32)</small>";
  html += "</fieldset>";
  html += "<fieldset><legend>Configurazione Sensore Velocità</legend>";
  html += "<label for='speedPin'>Pin Sensore Velocità:</label><input type='number' id='speedPin' name='speedPin' value='" + String(conf_speed_sensor_pin) + "' min='-1' max='48'><br><small>(-1 per disabilitare)</small><br>";  
  html += "<label for='metersPulse'>Metri per Impulso:</label><input type='text' id='metersPulse' name='metersPulse' value='" + String(conf_meters_per_pulse, 5) + "'><br><small>(Es. 1.30381, usa il punto come separatore decimale)</small>";
  html += "</fieldset>";
  html += "<fieldset><legend>Configurazione Allarmi Email (Errore SD)</legend>";
  html += "<label for='emailSender'>Email Mittente:</label><input type='text' id='emailSender' name='emailSender' value='" + conf_email_sender + "'><br>";
  html += "<label for='emailPass'>Password Email/App:</label><input type='password' id='emailPass' name='emailPass'><br><small>(Lascia vuoto per non cambiare)</small><br>";
  html += "<label for='emailSmtp'>Server SMTP:</label><input type='text' id='emailSmtp' name='emailSmtp' value='" + conf_email_smtp_server + "'><br>";
  html += "<label for='emailPort'>Porta SMTP:</label><input type='number' id='emailPort' name='emailPort' value='" + String(conf_email_smtp_port) + "' min='1' max='65535'><br>";
  html += "<label for='emailTo'>Email Destinatario (TO):</label><input type='text' id='emailTo' name='emailTo' value='" + conf_email_recipient_to + "' required><br>";
  html += "<label for='emailCcCsv'>Email Dest. (CC, CSV):</label><input type='text' id='emailCcCsv' name='emailCcCsv' value='" + conf_email_recipient_cc_csv + "'><br><small>Es. mail1@ex.com,mail2@ex.com</small>";
  html += "</fieldset>";
  
  // NUOVO FIELDSET PER URL OTA
  html += "<fieldset><legend>Configurazione Aggiornamento Firmware (OTA)</legend>";
  html += "<label for='otaFwUrl'>URL Firmware (.bin):</label><input type='text' id='otaFwUrl' name='otaFwUrl' value='" + conf_ota_firmware_url + "' style='width: calc(100% - 230px);'><br>"; // Aggiunto style per coerenza
  html += "<small>Inserisci l'URL completo del file firmware .bin per l'aggiornamento OTA. Lascia vuoto per disabilitare l'aggiornamento da URL.</small>";
  html += "</fieldset>";

  html += "<br><input type='submit' value='Salva Configurazione e Riavvia'>";
  html += "</form>";
  html += "<hr><p><a href='/status'>Torna allo Stato</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSaveConfig() {
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Method Not Allowed"); return; }
  Serial.println("[Config] Ricevuta richiesta /saveconfig");
  preferences.begin("device-cfg", false);  
  if (server.hasArg("devName")) { String val = server.arg("devName"); if (val.length() > 0) preferences.putString("devName", val); }
  if (server.hasArg("wifiSsid")) { String val = server.arg("wifiSsid"); if (val.length() > 0) preferences.putString("wifiSsid", val); }
  if (server.hasArg("wifiPass")) { String val = server.arg("wifiPass"); if (val.length() > 0 ) preferences.putString("wifiPass", val);  }
  bool newUseStaticIP = server.hasArg("useStaticIP"); preferences.putBool("useStaticIP", newUseStaticIP); preferences.putBool("netCfgDone", true);  
  if (newUseStaticIP) {
    if (server.hasArg("staticIP")) preferences.putString("staticIP", server.arg("staticIP"));
    if (server.hasArg("gatewayIP")) preferences.putString("gatewayIP", server.arg("gatewayIP"));
    if (server.hasArg("subnetMask")) preferences.putString("subnetMask", server.arg("subnetMask"));
    if (server.hasArg("dns1IP")) preferences.putString("dns1IP", server.arg("dns1IP"));
  }
  if (server.hasArg("pinsCSV")) { String val = server.arg("pinsCSV"); preferences.putString("pinsCSV", val); }
  if (server.hasArg("speedPin")) { int val = server.arg("speedPin").toInt(); if (val >= -1 && val <= 48) preferences.putInt("speedPin", val); }  
  if (server.hasArg("metersPulse")) {
    String metersPulseStr = server.arg("metersPulse"); std::string stdStr = metersPulseStr.c_str();
    std::replace(stdStr.begin(), stdStr.end(), ',', '.');  
    float val = atof(stdStr.c_str());
    if (val >= 0) preferences.putFloat("metersPulse", val);
  }
  if (server.hasArg("emailSender")) preferences.putString("emailSender", server.arg("emailSender"));
  if (server.hasArg("emailPass")) {String val = server.arg("emailPass"); if(val.length() > 0) preferences.putString("emailPass", val);}  
  if (server.hasArg("emailSmtp")) preferences.putString("emailSmtp", server.arg("emailSmtp"));
  if (server.hasArg("emailPort")) { int val = server.arg("emailPort").toInt(); if (val > 0 && val <= 65535) preferences.putInt("emailPort", val);}
  if (server.hasArg("emailTo")) preferences.putString("emailTo", server.arg("emailTo"));
  if (server.hasArg("emailCcCsv")) preferences.putString("emailCcCsv", server.arg("emailCcCsv"));

  // NUOVA SEZIONE PER SALVATAGGIO URL OTA
  if (server.hasArg("otaFwUrl")) {
    String val = server.arg("otaFwUrl");
    preferences.putString("otaFwUrl", val); // Salva l'URL fornito
    Serial.println("[Config] Salvato URL OTA: " + val);
  }

  preferences.end();  
  String html = "<html><head><title>Configurazione Salvata</title><meta http-equiv='refresh' content='3;url=/'></head><body><h1>Configurazione Salvata!</h1><p>Il dispositivo si riavvier&agrave; tra poco.</p><p><a href='/'>Attendere o clicca.</a></p></body></html>";
  server.send(200, "text/html", html);
  delay(1000); Serial.println("[Config] Nuova configurazione salvata. Riavvio..."); ESP.restart();
}

void startServer() {
  if (serverStarted) return;
  if (!ethernetConnected && !wifiConnected) { Serial.println("[Server] Cannot start server, no network connection."); return; }
  if (MDNS.begin(nomeDispositivo.c_str())) { Serial.println("[mDNS] mDNS responder started: http://" + nomeDispositivo + ".local"); }
  else { Serial.println("[mDNS][Error] Failed to start mDNS responder."); }
  server.on("/", HTTP_GET, [](){ server.sendHeader("Location", "/status", true); server.send(302, "text/plain", ""); });
  server.on("/config", HTTP_GET, handleConfigPage);
  server.on("/saveconfig", HTTP_POST, handleSaveConfig);
  server.on("/doupdate", HTTP_GET, handleFirmwareUpdate); // Userà la nuova logica
  server.on("/otastatus", HTTP_GET, handleOTAStatusPage);  // Pagina di stato per OTA
  server.on("/log", HTTP_GET, []() {  
    if (!sdCardInitialized && !sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card non disponibile."); return; }
    if (!sdCardInitialized && sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card in recupero, riprovare tra poco."); return; }

    if (SD.exists("/log_temp.csv")) SD.remove("/log_temp.csv");  
    File oF = SD.open("/log.csv", FILE_READ);
    if (!oF) { server.send(500, "text/plain", "Errore apertura /log.csv per lettura"); triggerSDRecovery("Errore apertura /log.csv per download"); return; }
    File tF = SD.open("/log_temp.csv", FILE_WRITE);
    if (!tF) { oF.close(); server.send(500, "text/plain", "Errore apertura /log_temp.csv per scrittura"); triggerSDRecovery("Errore apertura /log_temp.csv per scrittura"); return; }
    uint8_t b[512]; size_t br;
    while ((br=oF.read(b,sizeof(b))) > 0) {
        if (tF.write(b,br) != br) {  
            oF.close(); tF.close(); SD.remove("/log_temp.csv");
            server.send(500, "text/plain", "Errore scrittura su /log_temp.csv");
            triggerSDRecovery("Errore scrittura /log_temp.csv durante copia"); return;
        }
    }
    oF.close(); tF.close();

    if (!SD.remove("/log.csv")) {Serial.println("Errore rimozione /log.csv dopo copia in temp");}  
    File nLF = SD.open("/log.csv", FILE_WRITE);  
    if (nLF) {
        if (!nLF.println("Dispositivo,Timestamp,Tipo,Descrizione,Valore")) {  
             nLF.close(); triggerSDRecovery("Errore scrittura header nuovo /log.csv");
        } else {
            nLF.close();
            logCount = 0;  
            Serial.println("[Log] File /log.csv azzerato e header riscritto.");
        }
    } else {
        triggerSDRecovery("Errore ricreazione /log.csv dopo azzeramento");
    }

    File fTS = SD.open("/log_temp.csv", FILE_READ);  
    if (fTS) {
      server.sendHeader("Content-Disposition", "attachment; filename=\"log_downloaded.csv\"");  
      server.streamFile(fTS,"text/csv");
      fTS.close();
    }
    else {
      server.send(404,"text/plain","log_temp.csv non trovato per il download (dopo copia).");
      triggerSDRecovery("Errore apertura /log_temp.csv per stream dopo copia");
    }
  });
  server.on("/logtemp", HTTP_GET, []() {  
    if (!sdCardInitialized && !sdRecoveryAttemptInProgress) { server.send(503,"text/plain","SD Card non disponibile."); return; }
    if (!sdCardInitialized && sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card in recupero, riprovare tra poco."); return; }
    if (!SD.exists("/log_temp.csv")) { server.send(404,"text/plain","/log_temp.csv non trovato."); return; }
    File f=SD.open("/log_temp.csv",FILE_READ);
    if(!f){server.send(500,"text/plain","Errore apertura /log_temp.csv"); triggerSDRecovery("Errore apertura /log_temp.csv per /logtemp"); return;}
    server.sendHeader("Content-Disposition","attachment; filename=\"log_temp.csv\"");
    server.streamFile(f,"text/csv");
    f.close();
  });
  server.on("/del", HTTP_GET, []() {  
    if (!sdCardInitialized && !sdRecoveryAttemptInProgress) { server.send(503,"text/plain","SD Card non disponibile."); return; }
    if (!sdCardInitialized && sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card in recupero, riprovare tra poco."); return; }
    if(SD.exists("/log_temp.csv")){
        if(SD.remove("/log_temp.csv")) server.send(200,"text/plain","log_temp.csv cancellato.");
        else {
            server.send(500,"text/plain","Errore cancellazione log_temp.csv");
            triggerSDRecovery("Errore cancellazione /log_temp.csv");
        }
    }
    else server.send(404,"text/plain","Nessun log temporaneo da cancellare.");
  });
  server.on("/status", HTTP_GET, []() {
    uint32_t hF=ESP.getFreeHeap(); uint32_t hT=ESP.getHeapSize(); uint64_t csMB=0,cuMB=0,cfMB=0;
    String sdStatusString = "";
    if (sdCardInitialized) {
        csMB=SD.cardSize()/(1024*1024);uint64_t tb=SD.totalBytes();uint64_t ub=SD.usedBytes();cuMB=ub/(1024*1024);cfMB=(tb-ub)/(1024*1024);
        sdStatusString = "Inizializzata | Dim: "+String(csMB)+"MB | Usata: "+String(cuMB)+"MB | Libera: "+String(cfMB)+"MB | Logs: "+String(logCount);
    } else if (sdRecoveryAttemptInProgress) {
        sdStatusString = "Recupero in corso (tentativo " + String(sdRetryCount) + "/" + String(MAX_SD_RETRIES) + ")...";
    } else {
        sdStatusString = "Non Inizializzata / Errore";
    }
    
    Serial.printf("[DEBUG WebStatus] Valore di currentSpeedMetersPerMinute prima dell'invio HTML: %.2f\n", currentSpeedMetersPerMinute);

    String sP="<html><head><title>Status "+nomeDispositivo+"</title><meta http-equiv='refresh' content='10'><style>body{font-family:Arial,sans-serif;margin:15px;}h1{color:#333;}ul{list-style-type:none;padding:0;}li{background-color:#f9f9f9;border:1px solid #ddd;margin-bottom:8px;padding:10px;border-radius:4px;}li strong{color:#555;} .ota-button{padding:8px 12px;background-color:#007bff;color:white;text-decoration:none;border-radius:4px;border:none;cursor:pointer;font-size:0.9em;margin-left:10px;} .ota-button:hover{background-color:#0056b3;}</style></head><body><h1>Status Dispositivo</h1>";
    sP += "<p style='margin-bottom:15px;'><a href='/doupdate' class='ota-button'>Verifica/Installa Aggiornamenti Firmware</a></p>"; // Il testo rimane generico
    sP += "<ul>";
    sP+="<li><strong>Dispositivo:</strong> "+nomeDispositivo+" (<a href='/config'>Configura</a>)</li>";
    sP+="<li><strong>Versione Firmware:</strong> " + String(FIRMWARE_VERSION) + "</li>";
    sP+="<li><strong>Hostname:</strong> "+nomeDispositivo+".local</li>";
    sP+="<li><strong>Ethernet:</strong> "+String(ethernetConnected?"Connesso":"Disconnesso")+(ethernetConnected?(" @ "+ETH.localIP().toString()):"")+"</li>";
    sP+="<li><strong>Wi-Fi:</strong> "+String(wifiConnected?("Connesso ("+WiFi.SSID()+") @ "+WiFi.localIP().toString()):(attemptingWiFiConnection?("Connessione a "+conf_ssid+"..."):"Disconnesso"))+"</li>";
    sP+="<li><strong>IP Attivo:</strong> "+String(ethernetConnected?ETH.localIP().toString():(wifiConnected?WiFi.localIP().toString():"N/A"))+"</li>";
    sP+="<li><strong>Config. Rete (NVS):</strong> SSID: "+conf_ssid+(conf_use_static_ip?(" | Static IP: "+conf_static_ip.toString()+" | Gateway: "+conf_gateway.toString()+" | Subnet: "+conf_subnet.toString()+" | DNS: "+conf_dns1.toString()):" | Modalità IP: DHCP")+"</li>";
    sP+="<li><strong>Config. Email:</strong> Mittente: "+conf_email_sender+" | Dest. TO: "+conf_email_recipient_to+" | Dest. CC: "+conf_email_recipient_cc_csv+"</li>";
    sP+="<li><strong>Ora di Sistema:</strong> "+getFormattedTimestamp()+(timeSynced?" (Sincronizzata)":" (Non Sincronizzata)")+"</li>";
    
    // NUOVA RIGA PER VISUALIZZARE URL OTA CONFIGURATO
    sP+="<li><strong>URL Firmware OTA (NVS):</strong> "+(conf_ota_firmware_url.isEmpty() ? "Non impostato" : ("<span title='" + conf_ota_firmware_url + "'>" + (conf_ota_firmware_url.length() > 60 ? conf_ota_firmware_url.substring(0, 57) + "..." : conf_ota_firmware_url) + "</span>"))+"</li>";

    sP+="<li><strong>Pin Monitorati (CSV):</strong> "+conf_monitored_pins_csv+"</li><li><strong>Pin Sensore Velocità:</strong> "+String(active_speed_sensor_pin)+" | <strong>Metri/Impulso:</strong> "+String(active_meters_per_pulse,5)+"</li>";
    sP+="<li><strong>Velocità Attuale:</strong> "+String(currentSpeedMetersPerMinute,2)+" m/min</li>";
    sP+="<li><strong>Memoria Heap:</strong> Libera "+String(hF/1024)+"KB / Totale "+String(hT/1024)+"KB</li>";
    sP+="<li><strong>Scheda SD:</strong> "+ sdStatusString +"</li>";
    sP+="</ul><hr><p><a href='/file'>Visualizza File su SD</a> | <a href='/log'>Scarica Log & Azzera</a> | <a href='/del'>Cancella Log Temporaneo</a></p></body></html>"; server.send(200,"text/html",sP);
  });
  server.on("/file", HTTP_GET, []() {  
    if (!sdCardInitialized && !sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card non disponibile."); return; }
    if (!sdCardInitialized && sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card in recupero, riprovare tra poco."); return; }
    File root = SD.open("/");
    if (!root || !root.isDirectory()) {  
        server.send(500, "text/plain", "Errore apertura root SD o non è una directory.");
        if(root) root.close();  
        triggerSDRecovery("Errore apertura root SD per /file");
        return;
    }
    String html = "<html><head><title>Files su SD</title><style>body{font-family:Arial,sans-serif;margin:15px;}table{width:100%;border-collapse:collapse;}th,td{border:1px solid #ddd;padding:8px;text-align:left;}th{background-color:#f2f2f2;}</style></head><body><h1>Files su Scheda SD</h1><table><tr><th>Nome File</th><th>Dimensione</th><th>Azioni</th></tr>"; File file = root.openNextFile();
    while(file){
        html += "<tr><td>";
        if(file.isDirectory()){ html += "<b>[D] "+String(file.name())+"</b>"; } else { html += String(file.name());}
        html += "</td><td>";
        if(!file.isDirectory()){ html += String(file.size()/1024.0,2)+" KB";} else {html += "-";}
        html += "</td><td>";
        if(!file.isDirectory() && String(file.name()) != "/" ){html += " (<a href='/download?file="+String(file.name())+"'>Download</a>)";}
        html += "</td></tr>";
        file.close();  
        file=root.openNextFile();
    }
    root.close(); html += "</table><br><a href='/status'>Torna allo Stato</a></body></html>"; server.send(200, "text/html", html);
  });
  server.on("/download", HTTP_GET, []() {  
    if (!sdCardInitialized && !sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card non disponibile."); return; }
    if (!sdCardInitialized && sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card in recupero, riprovare tra poco."); return; }
    if (!server.hasArg("file")) { server.send(400, "text/plain", "Parametro 'file' mancante"); return; }
    String filename=server.arg("file"); if(!filename.startsWith("/")) filename="/"+filename;  
    if(!SD.exists(filename)){ server.send(404,"text/plain","File non trovato: " + filename); return; }
    File f=SD.open(filename,FILE_READ);
    if(f){
        server.sendHeader("Content-Disposition","attachment; filename=\""+filename.substring(filename.lastIndexOf('/')+1)+"\"");  
        server.streamFile(f,"application/octet-stream");  
        f.close();
    }
    else {
        server.send(500,"text/plain","Errore apertura file per download");
        triggerSDRecovery("Errore apertura file per /download: " + filename);
    }
  });
  server.begin(); serverStarted = true; Serial.println("[Server] Web server started.");
}

// === Funzione di Polling e Logging degli Input ===
void pollInputsAndLog() {
  if (sdRecoveryAttemptInProgress) return;  
  if (monitoredPinsVec.empty()) return;  

  for (size_t i = 0; i < monitoredPinsVec.size(); ++i) {
    int pin = monitoredPinsVec[i]; if (pin < 0 || pin >= GPIO_NUM_MAX) continue;  
    bool currentState = digitalRead(pin);
    if (currentState != lastPinStatesVec[i]) {  
      if (millis() - lastDebounceTimeVec[i] > debounceDelay) {  
        lastPinStatesVec[i] = currentState; String timestamp = getFormattedTimestamp();
        char pinDesc[10]; snprintf(pinDesc, sizeof(pinDesc), "IN%d", pin);
        if (sdCardInitialized) {  
          File f = SD.open("/log.csv", FILE_APPEND);
          if (f) {  
            if (!f.printf("%s,%s,IO,%s,%d\n", nomeDispositivo.c_str(), timestamp.c_str(), pinDesc, currentState)) {
                Serial.printf("[Poll] Errore scrittura su SD (printf). Log: %s,%s,IO,%s,%d\n",nomeDispositivo.c_str(),timestamp.c_str(),pinDesc,currentState);
                triggerSDRecovery("Err scrittura CSV (polling IO)");
            }
            f.close();  
            logCount++;
          }
          else {  
            Serial.printf("[Poll] Errore apertura SD per scrittura. Log: %s,%s,IO,%s,%d\n",nomeDispositivo.c_str(),timestamp.c_str(),pinDesc,currentState);
            triggerSDRecovery("Err apertura CSV (polling IO)");
          }
        } else {  
            Serial.printf("LOG SERIAL(NoSD):%s,%s,IO,%s,%d\n",nomeDispositivo.c_str(),timestamp.c_str(),pinDesc,currentState);  
            if(!sdRecoveryAttemptInProgress) triggerSDRecovery("SD non inizializzata durante polling IO");
        }
      }
      lastDebounceTimeVec[i] = millis();  
    }
  }
}

// === Funzione di gestione Retry SD ===
void handleSDRetryLogic() {
    if (sdRecoveryAttemptInProgress && millis() >= nextSDRetryTime) {
        bool currentAttemptSuccess = false;  

        if (sdRetryCount < MAX_SD_RETRIES) {
            sdRetryCount++;  
            Serial.printf("[SD Recovery] Tentativo %d/%d (Contesto: %s)...\n", sdRetryCount, MAX_SD_RETRIES, lastSDFailureContext.c_str());
            
            if (SD.begin(SD_CS, spiSD, 8000000)) {  
                Serial.println("[SD Recovery] SD.begin() OK. Verifico operatività file di log...");
                bool logFileOperational = true;  

                if (!SD.exists("/log.csv")) {
                    Serial.print("[SD Recovery] /log.csv non trovato. Creazione... ");
                    File f = SD.open("/log.csv", FILE_WRITE);
                    if (f) {
                        if (f.println("Dispositivo,Timestamp,Tipo,Descrizione,Valore")) {
                            Serial.println("File /log.csv creato con successo.");
                        } else {
                            Serial.println("[SD Recovery] Errore scrittura header su /log.csv.");
                            logFileOperational = false;
                        }
                        f.close();
                    } else {
                        Serial.println("[SD Recovery] Errore apertura /log.csv per creazione.");
                        logFileOperational = false;
                    }
                } else {  
                    File f_test = SD.open("/log.csv", FILE_APPEND);
                    if (f_test) {
                        Serial.println("[SD Recovery] Apertura /log.csv in append OK.");
                        f_test.close();
                    } else {
                        Serial.println("[SD Recovery] Errore apertura /log.csv in append (file esistente).");
                        logFileOperational = false;
                    }
                }

                if (logFileOperational) {
                    currentAttemptSuccess = true;  
                } else {
                    Serial.println("[SD Recovery] SD.begin() OK, ma problemi con file di log.");
                }
            } else {  
                Serial.println("[SD Recovery] SD.begin() fallito durante il tentativo.");
            }

            if (currentAttemptSuccess) {
                Serial.println("[SD Recovery] Tentativo di recupero SD riuscito!");
                sdCardInitialized = true;        
                sdRecoveryAttemptInProgress = false;  
                sdRetryCount = 0;            
                emailAlertSentForSDFailure = false;  
            } else {  
                Serial.printf("[SD Recovery] Tentativo %d non riuscito.\n", sdRetryCount);
                sdCardInitialized = false;  
                if (sdRetryCount >= MAX_SD_RETRIES) {
                    Serial.println("[SD Recovery] Tutti i " + String(MAX_SD_RETRIES) + " tentativi di recupero SD esauriti. Invio email di errore.");
                    sdRecoveryAttemptInProgress = false;  
                    if (!emailAlertSentForSDFailure) {  
                        sendSDFailureEmail("Recupero SD fallito dopo " + String(MAX_SD_RETRIES) + " tentativi. Ultimo contesto: " + lastSDFailureContext);
                    }
                } else {
                    nextSDRetryTime = millis() + SD_RETRY_INTERVAL;  
                }
            }
        }
    }
}