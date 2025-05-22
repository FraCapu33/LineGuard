// === LineGuard (Configurazione Avanzata NVS, Web, Email, NeoPixel, OTA - v6 SD Retry) ===

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
#include <Adafruit_NeoPixel.h>

// --- INCLUSIONI PER OTA ---
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// --- VERSIONE FIRMWARE CORRENTE ---
#define FIRMWARE_VERSION "0.9.1" 

// --- CONFIGURAZIONE OTA GITHUB ---
const char* GITHUB_REPO_OWNER = "FraCapu33";
const char* GITHUB_REPO_NAME = "LineGuard";
const char* FIRMWARE_ASSET_NAME = "LineGuard.bin";
String GITHUB_API_LATEST_RELEASE_URL = "https://api.github.com/repos/" + String(GITHUB_REPO_OWNER) + "/" + String(GITHUB_REPO_NAME) + "/releases/latest";

// --- Definizione LED RGB NeoPixel ---
#define NEOPIXEL_PIN 48
#define NUM_NEOPIXELS 1
Adafruit_NeoPixel rgbLed(NUM_NEOPIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

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
String ota_status_message = "";

std::vector<int> monitoredPinsVec;
std::vector<int> lastPinStatesVec;
std::vector<unsigned long> lastDebounceTimeVec;
const unsigned long debounceDelay = 50;

bool attemptingWiFiConnection = false;
unsigned long lastWiFiAttemptTime = 0;
const unsigned long wifiRetryDelay = 30000;

int active_speed_sensor_pin;
float active_meters_per_pulse;
volatile unsigned long speedPulseCount = 0;
unsigned long lastSpeedCalcTimeMillis = 0;
const unsigned long speedCalcIntervalMillis = 1000;
float currentSpeedMetersPerMinute = 0.0;
float lastLoggedSpeed = -1.0f;
const float SPEED_CHANGE_THRESHOLD = 0.1f;

SMTPSession smtp;

// --- Variabili per Retry SD ---
int sdRetryCount = 0;
const int MAX_SD_RETRIES = 5;
unsigned long nextSDRetryTime = 0;
const unsigned long SD_RETRY_INTERVAL = 1000; // 1 secondo
bool sdRecoveryAttemptInProgress = false;
String lastSDFailureContext = "";


// ISR per sensore velocità
void IRAM_ATTR handleSpeedPulse() { speedPulseCount++; }

// Funzione per generare nome dispositivo
void generateRandomDeviceName(char* nameBuffer, size_t bufferSize) {
  long randomNumber = random(10000, 100000);
  snprintf(nameBuffer, bufferSize, "LineGuard%ld", randomNumber);
}

void triggerSDRecovery(const String& context) {
    if (!sdRecoveryAttemptInProgress) { // Avvia solo se non già in corso
        Serial.println("[SD Error] Rilevato problema SD: " + context + ". Avvio tentativi di recupero.");
        sdCardInitialized = false; // Marcala come non inizializzata
        emailAlertSentForSDFailure = false; // Permetti un nuovo invio email se il recupero fallisce
        sdRecoveryAttemptInProgress = true;
        sdRetryCount = 0;
        nextSDRetryTime = millis(); // Inizia il primo tentativo nel prossimo ciclo di loop idoneo
        lastSDFailureContext = context;
    } else {
        Serial.println("[SD Error] Rilevato problema SD: " + context + ", ma recupero già in corso.");
    }
}


// Funzione per parsare CSV dei pin
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

// Funzione per caricare configurazione da NVS
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
  conf_monitored_pins_csv = preferences.getString("pinsCSV", DEFAULT_MONITORED_PINS_CSV); parseMonitoredPins(conf_monitored_pins_csv);
  conf_speed_sensor_pin = preferences.getInt("speedPin", DEFAULT_SPEED_SENSOR_PIN);
  conf_meters_per_pulse = preferences.getFloat("metersPulse", DEFAULT_METERS_PER_PULSE);
  conf_email_sender = preferences.getString("emailSender", DEFAULT_EMAIL_SENDER);
  conf_email_sender_password = preferences.getString("emailPass", DEFAULT_EMAIL_SENDER_PASSWORD);
  conf_email_smtp_server = preferences.getString("emailSmtp", DEFAULT_EMAIL_SMTP_SERVER);
  conf_email_smtp_port = preferences.getInt("emailPort", DEFAULT_EMAIL_SMTP_PORT);
  conf_email_recipient_to = preferences.getString("emailTo", DEFAULT_EMAIL_RECIPIENT_TO);
  conf_email_recipient_cc_csv = preferences.getString("emailCcCsv", DEFAULT_EMAIL_RECIPIENT_CC_CSV);
  if (!preferences.isKey("wifiSsid")) preferences.putString("wifiSsid",conf_ssid); if (!preferences.isKey("wifiPass")) preferences.putString("wifiPass",conf_password);
  if (!preferences.isKey("pinsCSV")) preferences.putString("pinsCSV",conf_monitored_pins_csv); if (!preferences.isKey("speedPin")) preferences.putInt("speedPin",conf_speed_sensor_pin);
  if (!preferences.isKey("metersPulse")) preferences.putFloat("metersPulse",conf_meters_per_pulse); if (!preferences.isKey("emailSender")) preferences.putString("emailSender",conf_email_sender);
  if (!preferences.isKey("emailPass")) preferences.putString("emailPass",conf_email_sender_password); if (!preferences.isKey("emailSmtp")) preferences.putString("emailSmtp",conf_email_smtp_server);
  if (!preferences.isKey("emailPort")) preferences.putInt("emailPort",conf_email_smtp_port); if (!preferences.isKey("emailTo")) preferences.putString("emailTo",conf_email_recipient_to);
  if (!preferences.isKey("emailCcCsv")) preferences.putString("emailCcCsv",conf_email_recipient_cc_csv);
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
}

// Funzione per ottenere timestamp
String getFormattedTimestamp(unsigned long rawMillis = 0) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
    char buffer[30]; strftime(buffer, sizeof(buffer), "%Y-%m-%d, %H:%M:%S", &timeinfo); return String(buffer);
  } else { String fallback = ""; if (!timeSynced) fallback = "1970-01-01, 00:00:00 (No NTP Sync)"; else fallback = "0000-00-00, 00:00:00 (Time Error)"; return fallback; }
}

// Funzione per stampare lo stato su seriale
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
  if (emailAlertSentForSDFailure) { // Modificato: Non inviare se già fatto per questa sessione di fallimento
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
  
  smtp.debug(0); // Meno verboso di default, impostare a 1 per debug SMTP

  Serial.println("[Email] Connessione al server SMTP...");
  if (!smtp.connect(&session_obj)) { 
    Serial.printf("[Email] Connessione SMTP fallita: %s\n", smtp.errorReason().c_str());
    return; // Non impostare emailAlertSentForSDFailure se la connessione fallisce
  }

  Serial.println("[Email] Invio del messaggio...");
  if (!MailClient.sendMail(&smtp, &message)) {
    Serial.printf("[Email] Invio email fallito: %s\n", smtp.errorReason().c_str());
    // Non impostare emailAlertSentForSDFailure se l'invio fallisce, per permettere ritentativi
  } else {
    Serial.println("[Email] Email inviata con successo!");
    emailAlertSentForSDFailure = true; // Imposta solo se l'email è stata inviata con successo
  }
  if(smtp.connected()) smtp.closeSession();
}

// === Gestione Eventi Wi-Fi ed Ethernet ===
// ... (codice invariato) ...
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
// ... (codice invariato) ...
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
  if (!ethernetConnected && !wifiConnected) startWiFi();
  else if (ethernetConnected && WiFi.status() == WL_CONNECTED) { Serial.println("[NetworkCheck] Ethernet active, ensuring WiFi is disconnected."); WiFi.disconnect(true); wifiConnected = false; attemptingWiFiConnection = false; }
}
void syncTime() {
  if (!timeSynced && (ethernetConnected || wifiConnected)) {
    Serial.println("[Time] Attempting to sync time with NTP server...");
    configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
    struct tm timeinfo; int retries = 0; const int maxRetries = 20;
    while(!getLocalTime(&timeinfo, 0) && retries < maxRetries) { if(timeinfo.tm_year > (2000-1900)) break; Serial.print("."); delay(50); retries++; } // Shortened delay during NTP
    if (timeinfo.tm_year > (2000 - 1900)) { Serial.println("\n[Time] Time synchronized successfully!"); Serial.printf("[Time] Current time: %s\n", getFormattedTimestamp().c_str()); timeSynced = true; }
    else { Serial.println("\n[Time][Error] Failed to synchronize time via NTP after retries."); timeSynced = false; }
  }
}

// --- FUNZIONI OTA ---
// ... (codice invariato) ...
void performUpdate(String firmware_url, String new_version) {
    ota_status_message = "Avvio aggiornamento alla versione " + new_version + " da: " + firmware_url + "<br>Attendere il riavvio del dispositivo...";
    Serial.println(ota_status_message);
    
    String html = "<html><head><title>Aggiornamento Firmware</title><meta http-equiv='refresh' content='10;url=/status'></head>"; 
    html += "<body><h1>Aggiornamento Firmware</h1><p>" + ota_status_message + "</p>";
    html += "<p>Il dispositivo si riavvier&agrave; automaticamente se l'aggiornamento ha successo.</p>";
    html += "<p><a href='/status'>Torna allo stato (dopo il riavvio)</a></p></body></html>";
    server.send(200, "text/html", html);
    delay(100); 

    WiFiClientSecure clientSecure;
    clientSecure.setInsecure(); 

    t_httpUpdate_return ret = httpUpdate.update(clientSecure, firmware_url);

    switch (ret) {
        case HTTP_UPDATE_FAILED:
            ota_status_message = "Errore aggiornamento: " + httpUpdate.getLastErrorString();
            Serial.printf("[OTA] Update failed: Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            break;
        case HTTP_UPDATE_NO_UPDATES:
            ota_status_message = "Nessun aggiornamento necessario."; 
            Serial.println("[OTA] No updates needed.");
            break;
        case HTTP_UPDATE_OK:
            ota_status_message = "Aggiornamento completato con successo! Riavvio...";
            Serial.println("[OTA] Update OK! Rebooting...");
            break;
        default:
            ota_status_message = "Risultato aggiornamento sconosciuto.";
            Serial.printf("[OTA] Unknown update result: %d\n", ret);
            break;
    }
}

void handleFirmwareUpdate() {
    if (!wifiConnected && !ethernetConnected) {
        ota_status_message = "Errore: Nessuna connessione di rete per verificare gli aggiornamenti.";
        server.send(200, "text/html", "<h1>Aggiornamento Firmware</h1><p>" + ota_status_message + "</p><p><a href='/status'>Torna allo stato</a></p>");
        return;
    }

    ota_status_message = "Controllo aggiornamenti da GitHub...";
    server.send(200, "text/html", "<html><head><title>Aggiornamento Firmware</title><meta http-equiv='refresh' content='5;url=/otastatus'></head><body><h1>Aggiornamento Firmware</h1><p>" + ota_status_message + "</p><p>Verrai reindirizzato tra poco...</p></body></html>");
    delay(100); 

    HTTPClient http;
    WiFiClientSecure clientSecureCheck; 
    clientSecureCheck.setInsecure(); 

    Serial.println("[OTA] Controllo URL: " + GITHUB_API_LATEST_RELEASE_URL);
    if (http.begin(clientSecureCheck, GITHUB_API_LATEST_RELEASE_URL)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            Serial.println("[OTA] Risposta API GitHub: " + payload.substring(0, min(500, (int)payload.length())) + "..."); // Logga solo una parte

            DynamicJsonDocument doc(2048); 
            DeserializationError error = deserializeJson(doc, payload);

            if (error) {
                ota_status_message = "Errore parsing JSON: " + String(error.c_str());
                Serial.println(ota_status_message);
                http.end();
                return;
            }

            const char* remote_version_tag = doc["tag_name"];
            if (!remote_version_tag) {
                ota_status_message = "Errore: 'tag_name' non trovato nella risposta API.";
                Serial.println(ota_status_message);
                http.end();
                return;
            }
            String remote_version = String(remote_version_tag);
            if (remote_version.startsWith("v")) {
                remote_version.remove(0, 1);
            }

            Serial.println("[OTA] Versione corrente: " + String(FIRMWARE_VERSION));
            Serial.println("[OTA] Versione remota trovata: " + remote_version);

            ota_status_message = "Versione corrente: " + String(FIRMWARE_VERSION) + "<br>";
            ota_status_message += "Ultima versione su GitHub: " + remote_version + "<br>";

            if (remote_version.compareTo(FIRMWARE_VERSION) > 0) {
                Serial.println("[OTA] Nuova versione disponibile.");
                ota_status_message += "Nuova versione disponibile! Ricerca asset firmware...<br>";
                
                String firmware_url = "";
                JsonArray assets = doc["assets"];
                for (JsonObject asset : assets) {
                    const char* asset_name_ptr = asset["name"];
                    if (asset_name_ptr) {
                        String asset_name = String(asset_name_ptr);
                        if (asset_name.equalsIgnoreCase(FIRMWARE_ASSET_NAME) || asset_name.endsWith(".bin")) { 
                            firmware_url = asset["browser_download_url"].as<String>();
                            ota_status_message += "Trovato asset: " + asset_name + " @ " + firmware_url + "<br>";
                            Serial.println("[OTA] Trovato firmware asset: " + asset_name + " URL: " + firmware_url);
                            break; 
                        }
                    }
                }

                if (firmware_url != "") {
                    ota_status_message += "Download e installazione aggiornamento avviati...<br>";
                    performUpdate(firmware_url, remote_version); 
                } else {
                    ota_status_message += "Errore: File firmware (" + String(FIRMWARE_ASSET_NAME) + " o *.bin) non trovato negli asset della release.<br>Assicurati che sia caricato correttamente su GitHub.";
                    Serial.println("[OTA] Firmware asset non trovato.");
                }
            } else {
                ota_status_message += "Il firmware è già aggiornato.";
                Serial.println("[OTA] Firmware già aggiornato.");
            }
        } else {
            ota_status_message = "Errore HTTP API GitHub: " + String(httpCode) + " - " + http.errorToString(httpCode);
            Serial.println(ota_status_message);
        }
        http.end();
    } else {
        ota_status_message = "Impossibile connettersi all'API di GitHub.";
        Serial.println(ota_status_message);
    }
}

void handleOTAStatusPage() {
    String html = "<html><head><title>Stato Aggiornamento Firmware</title>";
    if (!ota_status_message.startsWith("Avvio aggiornamento")) {
         html += "<meta http-equiv='refresh' content='7;url=/status'>";
    }
    html += "</head><body><h1>Stato Aggiornamento Firmware</h1>";
    html += "<p>" + ota_status_message + "</p>";
    html += "<p><a href='/status'>Torna alla pagina di Stato</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
}


// === Web Server ===
// ... (codice handleIdentify, handleConfigPage, handleSaveConfig invariato) ...
void handleIdentify() {
  Serial.println("[WebServer] Ricevuta richiesta /identifica");
  uint32_t identifyColor = rgbLed.Color(0, 0, 255); // Blu per l'identificazione
  uint32_t offColor = rgbLed.Color(0, 0, 0);      // Spento (nero)

  for (int i = 0; i < 5; i++) {
    rgbLed.setPixelColor(0, identifyColor); // Accendi (Blu)
    rgbLed.show();
    delay(250); // Durata accensione
    rgbLed.setPixelColor(0, offColor);      // Spegni
    rgbLed.show();
    delay(250); // Durata spegnimento
  }
  rgbLed.setPixelColor(0, offColor);
  rgbLed.show();

  server.sendHeader("Location", "/status", true);
  server.send(302, "text/plain", "");
}

void handleConfigPage() {
  String html = "<html><head><title>Configurazione Dispositivo</title>";
  html += "<style>body {font-family: Arial, sans-serif; margin: 20px;} label {display: inline-block; width: 220px; margin-bottom: 8px; vertical-align: top;} input[type='text'], input[type='number'], input[type='password'], input[type='checkbox'] {margin-bottom:12px; width: 300px; padding: 5px; box-sizing: border-box;} fieldset{margin-bottom:20px; padding:15px; border:1px solid #ccc; border-radius:5px;} legend{font-weight:bold; font-size:1.1em;} input[type='submit']{padding:10px 15px; background-color:#4CAF50; color:white; border:none; border-radius:4px; cursor:pointer;} input[type='submit']:hover{background-color:#45a049;}</style>";
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
  html += "<input type='checkbox' id='useStaticIP' name='useStaticIP' value='1'" + String(conf_use_static_ip ? " checked" : "") + "> <label for='useStaticIP' style='width:auto;'>Usa IP Statico</label><br><br>";
  html += "<label for='staticIP'>IP Statico:</label><input type='text' id='staticIP' name='staticIP' value='" + (conf_use_static_ip ? conf_static_ip.toString() : DEFAULT_STATIC_IP_STR) + "'><br>";
  html += "<label for='gatewayIP'>Gateway:</label><input type='text' id='gatewayIP' name='gatewayIP' value='" + (conf_use_static_ip ? conf_gateway.toString() : DEFAULT_GATEWAY_STR) + "'><br>";
  html += "<label for='subnetMask'>Subnet Mask:</label><input type='text' id='subnetMask' name='subnetMask' value='" + (conf_use_static_ip ? conf_subnet.toString() : DEFAULT_SUBNET_STR) + "'><br>";
  html += "<label for='dns1IP'>DNS1:</label><input type='text' id='dns1IP' name='dns1IP' value='" + (conf_use_static_ip ? conf_dns1.toString() : DEFAULT_DNS1_STR) + "'><br>";
  html += "</fieldset>";

  html += "<fieldset><legend>Configurazione Pin Monitorati</legend>";
  html += "<label for='pinsCSV'>Pin (CSV):</label><input type='text' id='pinsCSV' name='pinsCSV' value='" + conf_monitored_pins_csv + "'><br>";
  html += "<small>Es. 39,40,41,42 (verifica pin validi per ESP32-S3)</small>";
  html += "</fieldset>";
  
  html += "<fieldset><legend>Configurazione Sensore Velocità</legend>";
  html += "<label for='speedPin'>Pin Sensore Velocità:</label><input type='number' id='speedPin' name='speedPin' value='" + String(conf_speed_sensor_pin) + "' min='0' max='48'><br>";
  html += "<label for='metersPulse'>Metri per Impulso:</label><input type='text' id='metersPulse' name='metersPulse' value='" + String(conf_meters_per_pulse, 5) + "'><br><small>(Es. 1.30381)</small>";
  html += "</fieldset>";

  html += "<fieldset><legend>Configurazione Allarmi Email (Errore SD)</legend>";
  html += "<label for='emailSender'>Email Mittente:</label><input type='text' id='emailSender' name='emailSender' value='" + conf_email_sender + "'><br>";
  html += "<label for='emailPass'>Password Email/App:</label><input type='password' id='emailPass' name='emailPass' value='" + conf_email_sender_password + "'><br>";
  html += "<label for='emailSmtp'>Server SMTP:</label><input type='text' id='emailSmtp' name='emailSmtp' value='" + conf_email_smtp_server + "'><br>";
  html += "<label for='emailPort'>Porta SMTP:</label><input type='number' id='emailPort' name='emailPort' value='" + String(conf_email_smtp_port) + "' min='1' max='65535'><br>";
  html += "<label for='emailTo'>Email Destinatario (TO):</label><input type='text' id='emailTo' name='emailTo' value='" + conf_email_recipient_to + "' required><br>";
  html += "<label for='emailCcCsv'>Email Dest. (CC, CSV):</label><input type='text' id='emailCcCsv' name='emailCcCsv' value='" + conf_email_recipient_cc_csv + "'><br><small>Es. mail1@ex.com,mail2@ex.com</small>";
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
  if (server.hasArg("wifiPass")) { String val = server.arg("wifiPass"); if (val.length() > 0 || (val.length() == 0 && server.arg("wifiPass") != conf_password) ) preferences.putString("wifiPass", val); }
  
  bool newUseStaticIP = server.hasArg("useStaticIP"); preferences.putBool("useStaticIP", newUseStaticIP); preferences.putBool("netCfgDone", true);
  
  if (newUseStaticIP) {
    if (server.hasArg("staticIP")) preferences.putString("staticIP", server.arg("staticIP"));
    if (server.hasArg("gatewayIP")) preferences.putString("gatewayIP", server.arg("gatewayIP"));
    if (server.hasArg("subnetMask")) preferences.putString("subnetMask", server.arg("subnetMask"));
    if (server.hasArg("dns1IP")) preferences.putString("dns1IP", server.arg("dns1IP"));
  }
  if (server.hasArg("pinsCSV")) { String val = server.arg("pinsCSV"); preferences.putString("pinsCSV", val); }
  if (server.hasArg("speedPin")) { int val = server.arg("speedPin").toInt(); if (val >= 0 && val <= 48) preferences.putInt("speedPin", val); }
  if (server.hasArg("metersPulse")) {
    String metersPulseStr = server.arg("metersPulse"); std::string stdStr = metersPulseStr.c_str();
    std::replace(stdStr.begin(), stdStr.end(), ',', '.'); float val = atof(stdStr.c_str());
    if (val >= 0) preferences.putFloat("metersPulse", val);
  }
  
  if (server.hasArg("emailSender")) preferences.putString("emailSender", server.arg("emailSender"));
  if (server.hasArg("emailPass")) {String val = server.arg("emailPass"); if(val.length() > 0) preferences.putString("emailPass", val);}
  if (server.hasArg("emailSmtp")) preferences.putString("emailSmtp", server.arg("emailSmtp"));
  if (server.hasArg("emailPort")) { int val = server.arg("emailPort").toInt(); if (val > 0 && val <= 65535) preferences.putInt("emailPort", val);}
  if (server.hasArg("emailTo")) preferences.putString("emailTo", server.arg("emailTo"));
  if (server.hasArg("emailCcCsv")) preferences.putString("emailCcCsv", server.arg("emailCcCsv"));
  
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
  server.on("/identifica", HTTP_GET, handleIdentify);
  
  server.on("/doupdate", HTTP_GET, handleFirmwareUpdate);
  server.on("/otastatus", HTTP_GET, handleOTAStatusPage); 

  server.on("/log", HTTP_GET, []() { 
    if (!sdCardInitialized && !sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card non disponibile."); return; }
    if (!sdCardInitialized && sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card in recupero, riprovare tra poco."); return; }
    // ... (resto del codice log)
    if (SD.exists("/log_temp.csv")) SD.remove("/log_temp.csv"); File oF = SD.open("/log.csv", FILE_READ);
    if (!oF) { server.send(500, "text/plain", "Err open /log.csv"); triggerSDRecovery("Errore apertura /log.csv per download"); return; } 
    File tF = SD.open("/log_temp.csv", FILE_WRITE);
    if (!tF) { oF.close(); server.send(500, "text/plain", "Err open /log_temp.csv"); triggerSDRecovery("Errore apertura /log_temp.csv per scrittura"); return; }
    uint8_t b[512]; size_t br; 
    while ((br=oF.read(b,sizeof(b))) > 0) { 
        if (tF.write(b,br) != br) {
            oF.close(); tF.close(); SD.remove("/log_temp.csv"); 
            server.send(500, "text/plain", "Err write /log_temp.csv"); 
            triggerSDRecovery("Errore scrittura /log_temp.csv durante copia"); return;
        }
    } 
    oF.close(); tF.close(); 
    if (!SD.remove("/log.csv")) {Serial.println("Err remove /log.csv"); /* Non critico per il download, ma logga */}
    File nLF = SD.open("/log.csv", FILE_WRITE);
    if (nLF) { 
        if (!nLF.println("Dispositivo,Timestamp,Tipo,Descrizione,Valore")) {
             nLF.close(); triggerSDRecovery("Errore scrittura header nuovo /log.csv");
             server.send(500,"text/plain","Err write new /log.csv header"); return;
        }
        nLF.close(); 
    } else { 
        server.send(500,"text/plain","Err recreate /log.csv"); 
        triggerSDRecovery("Errore ricreazione /log.csv"); return;
    }
    File fTS = SD.open("/log_temp.csv", FILE_READ); 
    if (fTS) { server.sendHeader("Content-Disposition", "attachment; filename=\"log_download.csv\""); server.streamFile(fTS,"text/csv"); fTS.close();}
    else server.send(404,"text/plain","log_temp.csv not found");
  });
  server.on("/logtemp", HTTP_GET, []() {
    if (!sdCardInitialized && !sdRecoveryAttemptInProgress) { server.send(503,"text/plain","SD Card non disponibile."); return; }
    if (!sdCardInitialized && sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card in recupero, riprovare tra poco."); return; }
    if (!SD.exists("/log_temp.csv")) { server.send(404,"text/plain","/log_temp.csv not found."); return; }
    File f=SD.open("/log_temp.csv",FILE_READ); 
    if(!f){server.send(500,"text/plain","Err open /log_temp.csv"); triggerSDRecovery("Errore apertura /log_temp.csv per /logtemp"); return;}
    server.sendHeader("Content-Disposition","attachment; filename=\"log_temp.csv\""); server.streamFile(f,"text/csv"); f.close();
  });
  server.on("/del", HTTP_GET, []() { 
    if (!sdCardInitialized && !sdRecoveryAttemptInProgress) { server.send(503,"text/plain","SD Card non disponibile."); return; }
    if (!sdCardInitialized && sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card in recupero, riprovare tra poco."); return; }
    if(SD.exists("/log_temp.csv")){ 
        if(SD.remove("/log_temp.csv")) server.send(200,"text/plain","log_temp.csv deleted."); 
        else {
            server.send(500,"text/plain","Err deleting log_temp.csv"); 
            triggerSDRecovery("Errore cancellazione /log_temp.csv");
        }
    }
    else server.send(404,"text/plain","No temp log to delete.");
  });
  server.on("/status", HTTP_GET, []() {
    uint32_t hF=ESP.getFreeHeap(); uint32_t hT=ESP.getHeapSize(); uint64_t csMB=0,cuMB=0,cfMB=0;
    String sdStatusString = "";
    if (sdCardInitialized) {
        csMB=SD.cardSize()/(1024*1024);uint64_t tb=SD.totalBytes();uint64_t ub=SD.usedBytes();cuMB=ub/(1024*1024);cfMB=(tb-ub)/(1024*1024);
        sdStatusString = "Inizializzata | Dim: "+String(csMB)+"MB | Usata: "+String(cuMB)+"MB | Libera: "+String(cfMB)+"MB | Logs: "+String(logCount);
    } else if (sdRecoveryAttemptInProgress) {
        sdStatusString = "Recupero in corso (tentativo " + String(sdRetryCount+1) + "/" + String(MAX_SD_RETRIES) + ")...";
    } else {
        sdStatusString = "Non Inizializzata / Errore";
    }

    String sP="<html><head><title>Status "+nomeDispositivo+"</title><meta http-equiv='refresh' content='10'><style>body{font-family:Arial,sans-serif;margin:15px;}h1{color:#333;}ul{list-style-type:none;padding:0;}li{background-color:#f9f9f9;border:1px solid #ddd;margin-bottom:8px;padding:10px;border-radius:4px;}li strong{color:#555;} .ota-button{padding:8px 12px;background-color:#007bff;color:white;text-decoration:none;border-radius:4px;border:none;cursor:pointer;font-size:0.9em;margin-left:10px;} .ota-button:hover{background-color:#0056b3;}</style></head><body><h1>Status Dispositivo</h1>";
    sP += "<p style='margin-bottom:15px;'><a href='/doupdate' class='ota-button'>Verifica/Installa Aggiornamenti Firmware</a></p>"; 
    sP += "<ul>";
    sP+="<li><strong>Dispositivo:</strong> "+nomeDispositivo+" (<a href='/config'>Configura</a>)";
    sP+=" <form action='/identifica' method='get' style='display:inline; margin-left:15px;'><input type='submit' value='Identifica Dispositivo' style='padding: 5px 10px; background-color: #ffc107; color: black; border: 1px solid #d39e00; border-radius: 4px; cursor: pointer; font-size:0.9em;'></form></li>";
    sP+="<li><strong>Versione Firmware:</strong> " + String(FIRMWARE_VERSION) + "</li>"; 
    sP+="<li><strong>Hostname:</strong> "+nomeDispositivo+".local</li>";
    sP+="<li><strong>Ethernet:</strong> "+String(ethernetConnected?"Connesso":"Disconnesso")+(ethernetConnected?(" @ "+ETH.localIP().toString()):"")+"</li>";
    sP+="<li><strong>Wi-Fi:</strong> "+String(wifiConnected?("Connesso ("+WiFi.SSID()+") @ "+WiFi.localIP().toString()):(attemptingWiFiConnection?("Connessione a "+conf_ssid+"..."):"Disconnesso"))+"</li>";
    sP+="<li><strong>IP Attivo:</strong> "+String(ethernetConnected?ETH.localIP().toString():(wifiConnected?WiFi.localIP().toString():"N/A"))+"</li>";
    sP+="<li><strong>Config. Rete (NVS):</strong> SSID: "+conf_ssid+(conf_use_static_ip?(" | Static IP: "+conf_static_ip.toString()+" | Gateway: "+conf_gateway.toString()+" | Subnet: "+conf_subnet.toString()+" | DNS: "+conf_dns1.toString()):" | Modalità IP: DHCP")+"</li>";
    sP+="<li><strong>Config. Email:</strong> Mittente: "+conf_email_sender+" | Dest. TO: "+conf_email_recipient_to+" | Dest. CC: "+conf_email_recipient_cc_csv+"</li>";
    sP+="<li><strong>Ora di Sistema:</strong> "+getFormattedTimestamp()+(timeSynced?" (Sincronizzata)":" (Non Sincronizzata)")+"</li>";
    sP+="<li><strong>Pin Monitorati (CSV):</strong> "+conf_monitored_pins_csv+"</li><li><strong>Pin Sensore Velocità:</strong> "+String(active_speed_sensor_pin)+" | <strong>Metri/Impulso:</strong> "+String(active_meters_per_pulse,5)+"</li>";
    sP+="<li><strong>Velocità Attuale:</strong> "+String(currentSpeedMetersPerMinute,2)+" m/min</li>";
    sP+="<li><strong>Memoria Heap:</strong> Libera "+String(hF/1024)+"KB / Totale "+String(hT/1024)+"KB</li>";
    sP+="<li><strong>Scheda SD:</strong> "+ sdStatusString +"</li>";
    sP+="</ul><hr><p><a href='/file'>Visualizza File su SD</a> | <a href='/log'>Scarica Log & Azzera</a> | <a href='/del'>Cancella Log Temporaneo</a></p></body></html>"; server.send(200,"text/html",sP);
  });
  server.on("/file", HTTP_GET, []() { // ... (codice file esistente, aggiunto trigger recovery) ...
    if (!sdCardInitialized && !sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card non disponibile."); return; }
    if (!sdCardInitialized && sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card in recupero, riprovare tra poco."); return; }
    File root = SD.open("/"); 
    if (!root || !root.isDirectory()) { 
        server.send(500, "text/plain", "Err open root SD"); 
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
        if(!file.isDirectory()){html += " (<a href='/download?file="+String(file.name())+"'>Download</a>)";} 
        html += "</td></tr>"; 
        file.close(); 
        file=root.openNextFile(); 
    }
    root.close(); html += "</table><br><a href='/status'>Torna allo Stato</a></body></html>"; server.send(200, "text/html", html);
  });
  server.on("/download", HTTP_GET, []() { // ... (codice download esistente, aggiunto trigger recovery) ...
    if (!sdCardInitialized && !sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card non disponibile."); return; }
    if (!sdCardInitialized && sdRecoveryAttemptInProgress) { server.send(503, "text/plain", "SD Card in recupero, riprovare tra poco."); return; }
    if (!server.hasArg("file")) { server.send(400, "text/plain", "Missing 'file' param"); return; }
    String filename=server.arg("file"); if(!filename.startsWith("/")) filename="/"+filename;
    if(!SD.exists(filename)){ server.send(404,"text/plain","File not found: " + filename); return; }
    File f=SD.open(filename,FILE_READ); 
    if(f){
        server.sendHeader("Content-Disposition","attachment; filename=\""+filename.substring(filename.lastIndexOf('/')+1)+"\""); 
        server.streamFile(f,"application/octet-stream");
        f.close();
    }
    else {
        server.send(500,"text/plain","Err opening file for download");
        triggerSDRecovery("Errore apertura file per /download: " + filename);
    }
  });
  server.begin(); serverStarted = true; Serial.println("[Server] Web server started.");
}

// === Funzione di Polling e Logging degli Input ===
void pollInputsAndLog() {
  if (sdRecoveryAttemptInProgress) return; // Non fare polling se stiamo tentando di recuperare la SD per evitare ulteriori errori
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
            Serial.printf("LOG SD: %s,%s,IO,%s,%d\n", nomeDispositivo.c_str(), timestamp.c_str(), pinDesc, currentState); 
          }
          else { 
            Serial.printf("[Poll] Errore apertura SD per scrittura. Log: %s,%s,IO,%s,%d\n",nomeDispositivo.c_str(),timestamp.c_str(),pinDesc,currentState);
            triggerSDRecovery("Err apertura CSV (polling IO)");
          }
        } else { 
            Serial.printf("LOG SERIAL(NoSD):%s,%s,IO,%s,%d\n",nomeDispositivo.c_str(),timestamp.c_str(),pinDesc,currentState); 
            if (!sdRecoveryAttemptInProgress) { // Se la SD non è inizializzata e non siamo in recupero, prova a recuperare
                triggerSDRecovery("SD non inizializzata durante polling IO");
            }
        }
      }
      lastDebounceTimeVec[i] = millis();
    }
  }
}

// === Funzione di gestione Retry SD ===
void handleSDRetryLogic() {
    if (sdRecoveryAttemptInProgress && !sdCardInitialized) { // Solo se il recupero è attivo E la SD non è ancora OK
        if (millis() >= nextSDRetryTime) {
            if (sdRetryCount < MAX_SD_RETRIES) {
                sdRetryCount++;
                Serial.printf("[SD Recovery] Tentativo %d/%d di reinizializzare la SD (Contesto: %s)...\n", sdRetryCount, MAX_SD_RETRIES, lastSDFailureContext.c_str());
                spiSD.end(); // Termina SPI prima di reinizializzare
                delay(50);   // Breve pausa
                spiSD.begin(6, 5, 7, SD_CS); // Reinizializza SPI
                
                if (SD.begin(SD_CS, spiSD, 8000000)) {
                    Serial.println("[SD Recovery] SD Card reinizializzata con successo!");
                    sdCardInitialized = true;
                    sdRecoveryAttemptInProgress = false; 
                    sdRetryCount = 0;
                    emailAlertSentForSDFailure = false; // Resetta per permettere futuri alert se necessario
                    
                    // Ricrea il file di log se non esiste, come da logica originale di setup
                    if (!SD.exists("/log.csv")) {
                        Serial.print("[SD Recovery] /log.csv non trovato. Creazione... ");
                        File f = SD.open("/log.csv", FILE_WRITE);
                        if (f) {
                            f.println("Dispositivo,Timestamp,Tipo,Descrizione,Valore");
                            f.close();
                            Serial.println("File /log.csv creato.");
                        } else {
                            Serial.println("[SD Recovery] Errore creazione /log.csv dopo recupero.");
                            // Se fallisce la creazione del file di log, la SD è comunque "up" ma con problemi di file.
                            // Potremmo voler segnalare questo in modo diverso o lasciare che il prossimo accesso fallisca.
                            // Per ora, la consideriamo inizializzata ma il prossimo accesso al file potrebbe fallire.
                        }
                    }
                } else {
                    Serial.printf("[SD Recovery] Tentativo %d fallito.\n", sdRetryCount);
                    if (sdRetryCount >= MAX_SD_RETRIES) {
                        Serial.println("[SD Recovery] Tutti i tentativi di recupero SD falliti. Invio email di errore.");
                        sdRecoveryAttemptInProgress = false; // Termina i tentativi
                        if (!emailAlertSentForSDFailure) {
                           sendSDFailureEmail("Recupero SD fallito dopo " + String(MAX_SD_RETRIES) + " tentativi. Contesto: " + lastSDFailureContext);
                        }
                    } else {
                        nextSDRetryTime = millis() + SD_RETRY_INTERVAL; // Prossimo tentativo
                    }
                }
            }
            // Se sdRetryCount >= MAX_SD_RETRIES, i tentativi sono terminati per questo evento
        }
    }
}


// === Setup ===
void setup() {
  Serial.begin(115200); delay(2000);

  rgbLed.begin();
  rgbLed.setPixelColor(0, rgbLed.Color(0, 0, 0));
  rgbLed.show();
  Serial.println("[Setup] LED RGB (NeoPixel) inizializzato su GPIO" + String(NEOPIXEL_PIN));
  
  const int analogSeedPin = 2;
  randomSeed(analogRead(analogSeedPin));
  Serial.println("[Setup] Random seed initialized using analogRead on GPIO" + String(analogSeedPin));
  
  loadConfiguration();
  Serial.println("\n\n=== LineGuard " + nomeDispositivo + " Init (v6 - SD Retry, OTA, NeoPixel) ===");
  Serial.println("[Setup] Firmware Version: " + String(FIRMWARE_VERSION));
  Serial.println("[Setup] Init network..."); WiFi.onEvent(WiFiEvent); 
  
  ETH.begin();
  
  Serial.println(F("[Setup] Config speed sensor..."));
  if (active_speed_sensor_pin>=0 && active_speed_sensor_pin <= 48 && digitalPinToInterrupt(active_speed_sensor_pin)!=-1) {
    pinMode(active_speed_sensor_pin, INPUT_PULLUP); attachInterrupt(digitalPinToInterrupt(active_speed_sensor_pin), handleSpeedPulse, RISING);
    Serial.printf("[Setup] Speed sensor OK: GPIO%d.\n", active_speed_sensor_pin);
  } else { Serial.printf("[Setup] Err speed sensor pin (GPIO%d) invalid or no interrupt.\n", active_speed_sensor_pin); }
  
  Serial.print("[Setup] Init SPI for SD (HSPI)... ");
  spiSD.begin(6, 5, 7, SD_CS); // CLK, MISO, MOSI, CS
  Serial.println("SPI init.");
  
  Serial.print("[Setup] Init SD Card (SPI)... ");
  if (SD.begin(SD_CS, spiSD, 8000000)) { // Tenta l'inizializzazione
    Serial.println("SD Card OK."); 
    sdCardInitialized=true; 
    emailAlertSentForSDFailure=false;
    if(!SD.exists("/log.csv")){ 
        Serial.print("[Setup] /log.csv not found. Creating... "); 
        File f=SD.open("/log.csv",FILE_WRITE);
        if(f){
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
    }
  } else { 
    Serial.println("SD Card init FAILED! Avvio tentativi di recupero in background."); 
    triggerSDRecovery("SD Card init failed @setup");
    // sdCardInitialized rimane false, gestito da triggerSDRecovery
  }
  
  Serial.printf("[Setup] Heap pre-pin: %u B\n", ESP.getFreeHeap()); Serial.println("[Setup] Config monitored pins (NVS/default)...");
  if(monitoredPinsVec.empty() && conf_monitored_pins_csv.length()>0) parseMonitoredPins(conf_monitored_pins_csv);
  if(!monitoredPinsVec.empty()){
    for(size_t i=0; i<monitoredPinsVec.size(); ++i){ int pin=monitoredPinsVec[i];
      if(pin>=0 && pin <= 48 && digitalPinToInterrupt(pin)!=-1){
        pinMode(pin,INPUT_PULLDOWN);
        if(i<lastPinStatesVec.size())lastPinStatesVec[i]=digitalRead(pin); if(i<lastDebounceTimeVec.size())lastDebounceTimeVec[i]=0;
        Serial.printf("[Setup] Pin GPIO%d (Idx %u) PULLDOWN. State: %d\n",pin,(unsigned int)i,(i<lastPinStatesVec.size()?lastPinStatesVec[i]:-1));
      } else { Serial.printf("[Setup] Err: Pin GPIO%d (idx %u) invalid or no interrupt. Skipping.\n",pin,(unsigned int)i);}}}
  else{ Serial.println("[Setup] Warn: No monitored pins!");}
  Serial.println("=== Setup Complete. System Running. ===");
}

// === Loop Principale ===
void loop() {
  if (Serial.available()>0){ String cmd=Serial.readStringUntil('\n'); cmd.trim();
    if(cmd.equalsIgnoreCase("stato")){ printStatusToSerial(); }
    else if(cmd.equalsIgnoreCase("erase_nvs_config")){ Serial.println("ERASE NVS? Type 'CONFIRM_ERASE'"); unsigned long st=millis(); String cCmd="";
      while(millis()-st<10000){ if(Serial.available()){cCmd=Serial.readStringUntil('\n');cCmd.trim();break;}delay(100);}
      if(cCmd.equalsIgnoreCase("CONFIRM_ERASE")){ preferences.begin("device-cfg",false); if(preferences.clear())Serial.println("NVS erased.");else Serial.println("Err erasing NVS.");
        preferences.end();Serial.println("Rebooting...");delay(1000);ESP.restart();}else Serial.println("NVS erase cancelled.");}
    else if(cmd.equalsIgnoreCase("ota_update_test")) { 
        Serial.println("Avvio test OTA da comando seriale...");
        handleFirmwareUpdate(); 
    } else if (cmd.equalsIgnoreCase("test_sd_fail")) { // Comando di test per fallimento SD
        Serial.println("Simulazione fallimento scrittura SD per test recovery...");
        triggerSDRecovery("Test fallimento SD da seriale");
    }
    else if(cmd.length()>0){Serial.print(F("Unknown cmd: "));Serial.println(cmd);}}
  
  if(serverStarted) server.handleClient(); 
  checkNetworkStatus();
  
  static unsigned long lastNtp=0; 
  if(millis()-lastNtp > 3600000UL || (!timeSynced && (wifiConnected||ethernetConnected))){ // Ogni ora o se non sinc e connesso
      syncTime();
      lastNtp=millis();
  }
  
  handleSDRetryLogic(); // Gestisce i tentativi di recupero SD in modo non bloccante

  if (!sdRecoveryAttemptInProgress) { // Esegui solo se non siamo in fase di recupero SD
    pollInputsAndLog(); 
  }


  unsigned long currMs=millis();
  if(currMs-lastSpeedCalcTimeMillis>=speedCalcIntervalMillis){
    unsigned long pulses; noInterrupts(); pulses=speedPulseCount;speedPulseCount=0;interrupts();
    float pps=(float)pulses/(speedCalcIntervalMillis/1000.0);
    currentSpeedMetersPerMinute=pps*active_meters_per_pulse*60.0;
    lastSpeedCalcTimeMillis=currMs;

    bool shouldLogSpeed = false;
    if (active_meters_per_pulse > 0.00001f) {
      if (lastLoggedSpeed < 0.0f && currentSpeedMetersPerMinute >= 0.0f) {shouldLogSpeed = true;}
      else if (abs(currentSpeedMetersPerMinute - lastLoggedSpeed) > SPEED_CHANGE_THRESHOLD) {shouldLogSpeed = true;}
      if (currentSpeedMetersPerMinute < 0.001f && lastLoggedSpeed > SPEED_CHANGE_THRESHOLD && lastLoggedSpeed >=0.0f ) {shouldLogSpeed = true;}
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
                Serial.printf("LOGGED TO SD: %s,%s,V,Velocita_mpm,%.2f\n", nomeDispositivo.c_str(), timestamp.c_str(), currentSpeedMetersPerMinute);
                lastLoggedSpeed = currentSpeedMetersPerMinute;
            } else {
                Serial.printf("[Loop] Errore apertura SD per scrittura velocità. Log: %s,%s,V,Velocita_mpm,%.2f\n", nomeDispositivo.c_str(),timestamp.c_str(),currentSpeedMetersPerMinute);
                triggerSDRecovery("Err apertura CSV (velocità)");
            }
        } else {
            String timestamp = getFormattedTimestamp();
            Serial.printf("LOG TO SERIAL (No SD): %s,%s,V,Velocita_mpm,%.2f\n", nomeDispositivo.c_str(), timestamp.c_str(), currentSpeedMetersPerMinute);
            lastLoggedSpeed = currentSpeedMetersPerMinute;
            if (!sdRecoveryAttemptInProgress) { // Se la SD non è inizializzata e non siamo in recupero, prova a recuperare
                triggerSDRecovery("SD non inizializzata durante log velocità");
            }
        }
    }
  }
  // Rimosso il check periodico emailAlertSentForSDFailure perché ora gestito da handleSDRetryLogic
  delay(10); 
}