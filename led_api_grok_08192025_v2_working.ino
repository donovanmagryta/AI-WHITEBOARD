#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// LED Strip Configuration
#define LED_PIN D2
#define NUM_LEDS 70
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// WiFi Settings
const char *apSSID = "Smart Board";
const byte DNS_PORT = 53;
DNSServer dnsServer;
ESP8266WebServer server(80);
bool isConnectedToWiFi = false;

// API Settings
const char *apiUrl = "https://api.x.ai/v1/chat/completions";
String apiKey; // Dynamic API key loaded from EEPROM
String portalUsername; // Portal username
String portalPassword; // Portal password
String lastApiResponse; // Store the last API response

// EEPROM Configuration
#define EEPROM_SIZE 512
#define MAGIC_ADDR 0        // Magic number (1 byte, 0)
#define SSID_ADDR 1         // WiFi SSID (32 bytes, 1–32)
#define PASS_ADDR 33        // WiFi Password (32 bytes, 33–64)
#define API_KEY_ADDR 65     // API Key (128 bytes, 65–192)
#define PORTAL_USER_ADDR 193 // Portal Username (32 bytes, 193–224)
#define PORTAL_PASS_ADDR 225 // Portal Password (32 bytes, 225–256)
#define INTERVAL_ADDR 257   // Query Interval (4 bytes, 257–260)
#define MAX_REQUESTS_ADDR 261 // Max API Requests (2 bytes, 261–262)
#define MAX_QUESTIONS 50
#define MAX_QUESTION_LEN 2000

String questions[MAX_QUESTIONS];
int sentiments[MAX_QUESTIONS];
int queryCounts[MAX_QUESTIONS];
int questionCount = 0;
unsigned long queryInterval = 30 * 60 * 1000;
unsigned long lastQueryTime = 0;
int maxApiRequests = 10;

// HTML in PROGMEM
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Smart LED Board Config Page</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <style>
        body {font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; margin: 0; padding: 16px; background: #f5f5f5; color: #333; font-size: 16px; line-height: 1.5;}
        h1, h2 {font-weight: 600; margin: 16px 0 8px; color: #222;}
        h1 {font-size: 24px;}
        h2 {font-size: 18px;}
        form {display: flex; flex-direction: column; gap: 12px;}
        input[type='text'], input[type='password'], input[type='number'], textarea {width: 100%; padding: 8px; border: 1px solid #ccc; border-radius: 4px; font-size: 16px; box-sizing: border-box;}
        textarea {resize: vertical; min-height: 100px;}
        input[type='submit'] {background: #007aff; color: white; padding: 10px; border: none; border-radius: 4px; font-size: 16px; cursor: pointer;}
        input[type='submit']:hover {background: #005bb5;}
        @media (max-width: 600px) {
            input[type='text'], input[type='password'], input[type='number'], textarea {font-size: 14px;}
            input[type='submit'] {font-size: 14px; padding: 8px;}
        }
    </style>
</head>
<body>
    <h1>Smart LED Board Settings</h1>
    <!--WIFI_STATUS-->
    <form action='/save' method='POST' autocomplete='off'>
        <h2>Portal Credentials</h2>
        Username: <input type='text' name='portalUser' maxlength='31' value='<!--PORTAL_USER-->' autocomplete='off'><br>
        Password: <input type='password' name='portalPass' maxlength='31' value='<!--PORTAL_PASS-->' autocomplete='new-password'><br>
        <h2>API Configuration</h2>
        API Key: <input type='password' name='apiKey' maxlength='128' value='<!--API_KEY-->' autocomplete='new-password'><br>
        <h2>WiFi Setup</h2>
        SSID: <input type='text' name='ssid' maxlength='31' value='<!--SSID-->' autocomplete='off'><br>
        Password: <input type='password' name='pass' maxlength='63' value='<!--PASS-->' autocomplete='new-password'><br>
        <h2>Questions</h2>
        Query Interval (minutes): <input type='number' name='interval' value='<!--INTERVAL-->' min='1'><br>
        Max API Requests: <input type='number' name='maxRequests' value='<!--MAX_REQUESTS-->' min='1'><br>
        Questions: <br> Tips: one question per line, max 50, add this preface for normal prompts: "Answer only with 1 for true, 2 for false, or 3 for unsure. Do not give other output. The question is..."<br>
        <textarea name='questions' autocomplete='off' rows='20'><!--QUESTIONS--></textarea><br>
        <input type='submit' value='Save and Apply'>
    </form>
    <div class='response-container'>
        <h2>Last API Response</h2>
        <pre><!--API_RESPONSE--></pre>
    </div>
</body>
</html>
)rawliteral";

// Function prototypes
void handleRoot();
void handleSave();
void queryAPI();
void initializeEEPROM();
void loadConfig();
void saveConfig(String ssid, String pass, String newApiKey, String newQuestions, unsigned long interval, int maxRequests, String portalUser, String portalPass);
void connectToWiFi();
void updateLEDs();
bool checkAuth();

void setup() {
  Serial.begin(115200);
  Serial.println("Starting setup...");

  strip.begin();
  strip.setBrightness(50);
  strip.show();

  EEPROM.begin(EEPROM_SIZE);
  initializeEEPROM();
  loadConfig();

  for (int i = 0; i < MAX_QUESTIONS; i++) {
    sentiments[i] = 0;
    queryCounts[i] = 0;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID);
  Serial.print("AP started: ");
  Serial.println(apSSID);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.setTTL(0);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  connectToWiFi();

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/hotspot-detect.html", []() {
    Serial.println("Android hotspot-detect.html requested");
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/html", "<html><body>Redirecting...</body></html>");
  });
  server.on("/captive.apple.com", []() {
    Serial.println("iOS captive.apple.com requested");
    server.send(200, "text/html", "<html><head><title>Smart LED Board Config</title></head><body>Please login at <a href='http://192.168.4.1'>http://192.168.4.1</a></body></html>");
  });
  server.on("/generate_204", []() {
    Serial.println("Android generate_204 requested");
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/html", "<html><body>Redirecting...</body></html>");
  });
  server.on("/library/test/success.html", []() {
    Serial.println("iOS success.html requested");
    server.send(200, "text/html", "<html><head><title>Smart LED Board Config</title></head><body>Please login at <a href='http://192.168.4.1'>http://192.168.4.1</a></body></html>");
  });
  server.on("/connectivitycheck.android.com/generate_204", []() {
    Serial.println("Android connectivity check requested");
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/html", "<html><body>Redirecting...</body></html>");
  });
  server.onNotFound([]() {
    Serial.println("Unknown request redirected to captive portal: " + server.uri());
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
  Serial.println("HTTP server started.");

  updateLEDs();

  if (isConnectedToWiFi && apiKey.length() > 0) {
    Serial.println("Initial API query...");
    queryAPI();
  }
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  if (isConnectedToWiFi && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected.");
    isConnectedToWiFi = false;
    connectToWiFi();
    updateLEDs();
  }

  if (isConnectedToWiFi && apiKey.length() > 0 && millis() - lastQueryTime >= queryInterval) {
    Serial.println("Periodic API query...");
    queryAPI();
    lastQueryTime = millis();
  }
}

void initializeEEPROM() {
  const byte MAGIC_NUMBER = 0xAB;
  Serial.println("Checking EEPROM initialization...");
  byte magic = EEPROM.read(MAGIC_ADDR);
  Serial.print("Magic number at address 0: 0x");
  Serial.println(magic, HEX);
  if (magic != MAGIC_NUMBER) {
    Serial.println("EEPROM not initialized, setting up...");
    for (int i = 0; i < EEPROM_SIZE; i++) {
      EEPROM.write(i, 0);
    }
    EEPROM.write(MAGIC_ADDR, MAGIC_NUMBER);
    // Set default portal credentials
    String defaultUser = "admin";
    String defaultPass = "password";
    for (int i = 0; i < 32; i++) {
      EEPROM.write(PORTAL_USER_ADDR + i, i < defaultUser.length() ? defaultUser[i] : 0);
      EEPROM.write(PORTAL_PASS_ADDR + i, i < defaultPass.length() ? defaultPass[i] : 0);
    }
    bool commitSuccess = EEPROM.commit();
    Serial.println(commitSuccess ? "EEPROM commit successful." : "EEPROM commit failed!");
    Serial.println("EEPROM initialized with magic number and default credentials.");
  } else {
    Serial.println("EEPROM already initialized, skipping clear.");
  }
}

void loadConfig() {
  Serial.println("Loading configuration from EEPROM...");
  char ssid[32], pass[32], apiKeyBuf[128], portalUser[32], portalPass[32];
  bool validSSID = true, validPass = true, validApiKey = true, validPortalUser = true, validPortalPass = true;

  for (int i = 0; i < 32; i++) {
    ssid[i] = EEPROM.read(SSID_ADDR + i);
    pass[i] = EEPROM.read(PASS_ADDR + i);
    portalUser[i] = EEPROM.read(PORTAL_USER_ADDR + i);
    portalPass[i] = EEPROM.read(PORTAL_PASS_ADDR + i);
    if (ssid[i] != 0 && (ssid[i] < 32 || ssid[i] > 126)) validSSID = false;
    if (pass[i] != 0 && (pass[i] < 32 || pass[i] > 126)) validPass = false;
    if (portalUser[i] != 0 && (portalUser[i] < 32 || portalUser[i] > 126)) validPortalUser = false;
    if (portalPass[i] != 0 && (portalPass[i] < 32 || portalPass[i] > 126)) validPortalPass = false;
  }
  for (int i = 0; i < 128; i++) {
    apiKeyBuf[i] = EEPROM.read(API_KEY_ADDR + i);
    if (apiKeyBuf[i] != 0 && (apiKeyBuf[i] < 32 || apiKeyBuf[i] > 126)) validApiKey = false;
  }

  Serial.print("Loaded SSID: ");
  Serial.println(validSSID ? ssid : "Invalid");
  Serial.print("Loaded Password: ");
  Serial.println(validPass ? pass : "Invalid");
  Serial.print("Loaded API Key: ");
  Serial.println(validApiKey ? apiKeyBuf : "Invalid");
  Serial.print("Loaded Portal Username: ");
  Serial.println(validPortalUser ? portalUser : "Invalid");
  Serial.print("Loaded Portal Password: ");
  Serial.println(validPortalPass ? portalPass : "Invalid");
  Serial.print("Loaded Magic Number: 0x");
  Serial.println(EEPROM.read(MAGIC_ADDR), HEX);

  if (validSSID && validPass && ssid[0] != 0) {
    Serial.println("Attempting WiFi connection with loaded credentials...");
    WiFi.begin(ssid, pass);
  } else {
    Serial.println("Invalid SSID or password, skipping WiFi connection");
  }
  apiKey = validApiKey ? String(apiKeyBuf) : "";
  portalUsername = validPortalUser ? String(portalUser) : "admin";
  portalPassword = validPortalPass ? String(portalPass) : "password";

  queryInterval = 0;
  for (int i = 0; i < 4; i++) {
    queryInterval |= ((unsigned long)EEPROM.read(INTERVAL_ADDR + i) << (i * 8));
  }
  if (queryInterval < 60000) queryInterval = 5 * 60 * 1000;
  maxApiRequests = EEPROM.read(MAX_REQUESTS_ADDR) | (EEPROM.read(MAX_REQUESTS_ADDR + 1) << 8);
  if (maxApiRequests < 1) maxApiRequests = 10;
  Serial.print("Loaded Query Interval (minutes): ");
  Serial.println(queryInterval / 60000);
  Serial.print("Loaded Max API Requests: ");
  Serial.println(maxApiRequests);
}

void saveConfig(String ssid, String pass, String newApiKey, String newQuestions, unsigned long interval, int maxRequests, String portalUser, String portalPass) {
  Serial.println("Saving configuration to EEPROM...");

  // Save SSID
  Serial.print("Writing SSID: ");
  Serial.println(ssid);
  for (int i = 0; i < 32; i++) {
    EEPROM.write(SSID_ADDR + i, i < ssid.length() ? ssid[i] : 0);
  }
  // Save Password
  Serial.print("Writing Password: ");
  Serial.println(pass);
  for (int i = 0; i < 32; i++) {
    EEPROM.write(PASS_ADDR + i, i < pass.length() ? pass[i] : 0);
  }
  // Save API Key
  Serial.print("Writing API Key: ");
  Serial.println(newApiKey);
  for (int i = 0; i < 128; i++) {
    EEPROM.write(API_KEY_ADDR + i, i < newApiKey.length() ? newApiKey[i] : 0);
  }
  // Save Portal Username
  Serial.print("Writing Portal Username: ");
  Serial.println(portalUser);
  for (int i = 0; i < 32; i++) {
    EEPROM.write(PORTAL_USER_ADDR + i, i < portalUser.length() ? portalUser[i] : 0);
  }
  // Save Portal Password
  Serial.print("Writing Portal Password: ");
  Serial.println(portalPass);
  for (int i = 0; i < 32; i++) {
    EEPROM.write(PORTAL_PASS_ADDR + i, i < portalPass.length() ? portalPass[i] : 0);
  }
  // Save Interval
  Serial.print("Writing Interval: ");
  Serial.println(interval / 60000);
  for (int i = 0; i < 4; i++) {
    EEPROM.write(INTERVAL_ADDR + i, (interval >> (i * 8)) & 0xFF);
  }
  // Save Max Requests
  Serial.print("Writing Max Requests: ");
  Serial.println(maxRequests);
  EEPROM.write(MAX_REQUESTS_ADDR, maxRequests & 0xFF);
  EEPROM.write(MAX_REQUESTS_ADDR + 1, (maxRequests >> 8) & 0xFF);

  // Save questions to RAM
  Serial.print("Raw questions input (length=");
  Serial.print(newQuestions.length());
  Serial.print("): [");
  Serial.print(newQuestions);
  Serial.println("]");

  // Replace \r\n with \n and trim leading/trailing whitespace
  newQuestions.replace("\r\n", "\n");
  newQuestions.trim();
  Serial.print("Processed questions input (length=");
  Serial.print(newQuestions.length());
  Serial.print("): [");
  Serial.print(newQuestions);
  Serial.println("]");

  String qArray[MAX_QUESTIONS];
  int newCount = 0;
  while (newQuestions.length() > 0 && newCount < MAX_QUESTIONS) {
    int idx = newQuestions.indexOf('\n');
    String q = (idx == -1) ? newQuestions : newQuestions.substring(0, idx);
    q.trim();
    if (q.length() > 0 && q.length() <= MAX_QUESTION_LEN - 1) {
      qArray[newCount] = q;
      Serial.print("Saving Question ");
      Serial.print(newCount + 1);
      Serial.print(": ");
      Serial.println(q);
      newCount++;
    } else if (q.length() > MAX_QUESTION_LEN - 1) {
      Serial.print("Skipping Question (too long, length=");
      Serial.print(q.length());
      Serial.print("): ");
      Serial.println(q);
    }
    newQuestions = (idx == -1) ? "" : newQuestions.substring(idx + 1);
    newQuestions.trim();
  }
  Serial.print("RAM Question Count: ");
  Serial.println(newCount);

  // Commit changes to EEPROM
  Serial.println("Committing EEPROM changes...");
  bool commitSuccess = EEPROM.commit();
  Serial.println(commitSuccess ? "EEPROM commit successful." : "EEPROM commit failed!");

  // Debug: Read back and verify EEPROM
  char verifySsid[32], verifyPass[32], verifyApiKey[128], verifyPortalUser[32], verifyPortalPass[32];
  for (int i = 0; i < 32; i++) {
    verifySsid[i] = EEPROM.read(SSID_ADDR + i);
    verifyPass[i] = EEPROM.read(PASS_ADDR + i);
    verifyPortalUser[i] = EEPROM.read(PORTAL_USER_ADDR + i);
    verifyPortalPass[i] = EEPROM.read(PORTAL_PASS_ADDR + i);
  }
  for (int i = 0; i < 128; i++) {
    verifyApiKey[i] = EEPROM.read(API_KEY_ADDR + i);
  }
  Serial.print("Saved SSID: ");
  Serial.println(verifySsid);
  Serial.print("Saved Pass: ");
  Serial.println(verifyPass);
  Serial.print("Saved API Key: ");
  Serial.println(verifyApiKey);
  Serial.print("Saved Portal Username: ");
  Serial.println(verifyPortalUser);
  Serial.print("Saved Portal Password: ");
  Serial.println(verifyPortalPass);
  Serial.print("Saved Interval: ");
  unsigned long savedInterval = 0;
  for (int i = 0; i < 4; i++) {
    savedInterval |= ((unsigned long)EEPROM.read(INTERVAL_ADDR + i) << (i * 8));
  }
  Serial.println(savedInterval / 60000);
  Serial.print("Saved Max Requests: ");
  int savedMaxRequests = EEPROM.read(MAX_REQUESTS_ADDR) | (EEPROM.read(MAX_REQUESTS_ADDR + 1) << 8);
  Serial.println(savedMaxRequests);
  Serial.print("Saved Magic Number: 0x");
  Serial.println(EEPROM.read(MAGIC_ADDR), HEX);

  // Update RAM questions
  questionCount = newCount;
  for (int i = 0; i < newCount; i++) {
    questions[i] = qArray[i];
    queryCounts[i] = 0;
  }
  Serial.println("Final RAM questions:");
  for (int i = 0; i < questionCount; i++) {
    Serial.print("Question ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(questions[i]);
  }
  apiKey = newApiKey;
  if (portalUser.length() > 0 && portalPass.length() > 0) {
    portalUsername = portalUser;
    portalPassword = portalPass;
  }
}

void connectToWiFi() {
  char ssid[32], pass[32];
  bool validSSID = true, validPass = true;
  for (int i = 0; i < 32; i++) {
    ssid[i] = EEPROM.read(SSID_ADDR + i);
    pass[i] = EEPROM.read(PASS_ADDR + i);
    if (ssid[i] != 0 && (ssid[i] < 32 || ssid[i] > 126)) validSSID = false;
    if (pass[i] != 0 && (pass[i] < 32 || pass[i] > 126)) validPass = false;
  }
  if (validSSID && validPass && ssid[0] != 0) {
    Serial.println("Connecting to WiFi with credentials...");
    WiFi.begin(ssid, pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      isConnectedToWiFi = true;
      Serial.println("\nWiFi connected.");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\nWiFi connection failed.");
      isConnectedToWiFi = false;
    }
  } else {
    Serial.println("No valid WiFi credentials in EEPROM");
  }
  updateLEDs();
}

bool checkAuth() {
  if (!server.authenticate(portalUsername.c_str(), portalPassword.c_str())) {
    server.requestAuthentication();
    Serial.println("Authentication requested");
    return false;
  }
  Serial.println("Authentication successful");
  return true;
}

void handleRoot() {
  if (!checkAuth()) return;

  Serial.println("Serving captive portal...");
  String html = FPSTR(index_html);
  if (isConnectedToWiFi) {
    html.replace("<!--WIFI_STATUS-->", "<p>Connected to WiFi: " + String(WiFi.SSID()) + "</p>");
  } else {
    html.replace("<!--WIFI_STATUS-->", "<p>Not connected to WiFi</p>");
  }
  char ssid[32], pass[32], apiKeyBuf[128], portalUser[32], portalPass[32];
  bool validSSID = true, validPass = true, validApiKey = true, validPortalUser = true, validPortalPass = true;
  for (int i = 0; i < 32; i++) {
    ssid[i] = EEPROM.read(SSID_ADDR + i);
    pass[i] = EEPROM.read(PASS_ADDR + i);
    portalUser[i] = EEPROM.read(PORTAL_USER_ADDR + i);
    portalPass[i] = EEPROM.read(PORTAL_PASS_ADDR + i);
    if (ssid[i] != 0 && (ssid[i] < 32 || ssid[i] > 126)) validSSID = false;
    if (pass[i] != 0 && (pass[i] < 32 || pass[i] > 126)) validPass = false;
    if (portalUser[i] != 0 && (portalUser[i] < 32 || portalUser[i] > 126)) validPortalUser = false;
    if (portalPass[i] != 0 && (portalPass[i] < 32 || portalPass[i] > 126)) validPortalPass = false;
  }
  for (int i = 0; i < 128; i++) {
    apiKeyBuf[i] = EEPROM.read(API_KEY_ADDR + i);
    if (apiKeyBuf[i] != 0 && (apiKeyBuf[i] < 32 || apiKeyBuf[i] > 126)) validApiKey = false;
  }
  Serial.print("EEPROM SSID: ");
  Serial.println(ssid);
  Serial.print("EEPROM Pass: ");
  Serial.println(pass);
  Serial.print("EEPROM API Key: ");
  Serial.println(apiKeyBuf);
  Serial.print("EEPROM Portal Username: ");
  Serial.println(portalUser);
  Serial.print("EEPROM Portal Password: ");
  Serial.println(portalPass);
  Serial.print("EEPROM Magic Number: 0x");
  Serial.println(EEPROM.read(MAGIC_ADDR), HEX);

  String ssidStr = validSSID ? String(ssid) : "";
  String passStr = validPass ? String(pass) : "";
  String apiKeyStr = validApiKey ? String(apiKeyBuf) : "";
  String portalUserStr = validPortalUser ? String(portalUser) : "";
  String portalPassStr = validPortalPass ? String(portalPass) : "";
  ssidStr.replace("<", "&lt;");
  ssidStr.replace(">", "&gt;");
  passStr.replace("<", "&lt;");
  passStr.replace(">", "&gt;");
  apiKeyStr.replace("<", "&lt;");
  apiKeyStr.replace(">", "&gt;");
  portalUserStr.replace("<", "&lt;");
  portalUserStr.replace(">", "&gt;");
  portalPassStr.replace("<", "&lt;");
  portalPassStr.replace(">", "&gt;");
  html.replace("<!--SSID-->", ssidStr);
  html.replace("<!--PASS-->", passStr);
  html.replace("<!--API_KEY-->", apiKeyStr);
  html.replace("<!--PORTAL_USER-->", portalUserStr);
  html.replace("<!--PORTAL_PASS-->", portalPassStr);
  String questionsText = "";
  for (int i = 0; i < questionCount; i++) {
    String q = questions[i];
    q.replace("<", "&lt;");
    q.replace(">", "&gt;");
    questionsText += q + "\n";
  }
  html.replace("<!--QUESTIONS-->", questionsText);
  html.replace("<!--INTERVAL-->", String(queryInterval / 60000));
  html.replace("<!--MAX_REQUESTS-->", String(maxApiRequests));
  String responseText = lastApiResponse;
  responseText.replace("<", "&lt;");
  responseText.replace(">", "&gt;");
  responseText.replace("\n", "<br>");
  html.replace("<!--API_RESPONSE-->", responseText.length() > 0 ? responseText : "No API response yet");
  server.send(200, "text/html", html);
}

void handleSave() {
  if (!checkAuth()) return;

  Serial.println("Handling form submission...");
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String newApiKey = server.arg("apiKey");
  String newQuestions = server.arg("questions");
  String newPortalUser = server.arg("portalUser");
  String newPortalPass = server.arg("portalPass");
  unsigned long interval = server.arg("interval").toInt() * 60 * 1000;
  if (interval < 60000) interval = 5 * 60 * 1000;
  int maxRequests = server.arg("maxRequests").toInt();
  if (maxRequests < 1) maxRequests = 10;

  Serial.print("Received SSID: ");
  Serial.println(ssid);
  Serial.print("Received Pass: ");
  Serial.println(pass);
  Serial.print("Received API Key: ");
  Serial.println(newApiKey);
  Serial.print("Received Portal Username: ");
  Serial.println(newPortalUser);
  Serial.print("Received Portal Password: ");
  Serial.println(newPortalPass);
  Serial.print("Received Questions: ");
  Serial.println(newQuestions);
  Serial.print("Received Interval (minutes): ");
  Serial.println(interval / 60000);
  Serial.print("Received Max Requests: ");
  Serial.println(maxRequests);

  saveConfig(ssid, pass, newApiKey, newQuestions, interval, maxRequests, newPortalUser, newPortalPass);
  connectToWiFi();
  if (isConnectedToWiFi && apiKey.length() > 0) {
    Serial.println("WiFi connected and API key available, querying API...");
    Serial.println(newQuestions);
    queryAPI();
    lastQueryTime = millis();
  } else {
    Serial.println("No WiFi connection or API key, skipping API query.");
  }

  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void updateLEDs() {
  strip.clear();
  if (isConnectedToWiFi) {
    strip.setPixelColor(0, strip.Color(0, 0, 50));
  }
  strip.show();
}

void queryAPI() {
  if (questionCount == 0 || !isConnectedToWiFi || apiKey.length() == 0) {
    Serial.println("No questions, no WiFi connection, or no API key, skipping API query.");
    return;
  }

  bool anyQueriesLeft = false;
  for (int i = 0; i < questionCount; i++) {
    if (queryCounts[i] < maxApiRequests) {
      anyQueriesLeft = true;
      break;
    }
  }
  if (!anyQueriesLeft) {
    Serial.println("Max API requests reached for all questions.");
    return;
  }

  String query = " ";
  for (int i = 0; i < questionCount; i++) {
    if (queryCounts[i] < maxApiRequests) {
      query += questions[i] + "\n";
    }
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, apiUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + apiKey);
  String contentVar = query;
  contentVar.replace("\"", "\\\"");
  contentVar.replace("\n", "\\n");
  contentVar.replace("\\", "\\\\");
  contentVar.replace("\t", "\\t");
  String payload = "{\"messages\": [{\"role\": \"system\", \"content\": \"You are Grok.\"}, {\"role\": \"user\", \"content\": \"" + contentVar + "\"}], \"model\": \"grok-3\", \"stream\": false, \"temperature\": 0}";
  Serial.println("query:");
  Serial.println(payload);
  int httpCode = http.POST(payload);
  String response = "";
  bool apiError = false;
  if (httpCode == 200) {
    String json = http.getString();
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
      Serial.print("JSON parsing failed: ");
      Serial.println(error.c_str());
      apiError = true;
      http.end();
    } else if (doc["choices"] && doc["choices"][0] && doc["choices"][0]["message"] && doc["choices"][0]["message"]["content"]) {
      response = doc["choices"][0]["message"]["content"].as<String>();
      Serial.println("API Response: ");
      Serial.println(response);
    } else {
      Serial.println("Invalid JSON structure");
      apiError = true;
    }
  } else {
    Serial.print("API Error: ");
    Serial.println(httpCode);
    apiError = true;
  }
  http.end();

  if (apiError) {
    strip.setPixelColor(1, strip.Color(50, 0, 0));
  }

  if (!apiError) {
    int idx = 0;
    for (int i = 0; i < questionCount && (i + 2) < NUM_LEDS; i++) {
      if (queryCounts[i] >= maxApiRequests) continue;
      queryCounts[i]++;
      int answer = response.substring(idx, response.indexOf('\n', idx)).toInt();
      sentiments[i] = answer;
      if (answer == 1) {
        strip.setPixelColor(i + 2, strip.Color(0, 50, 0));
      } else if (answer == 2) {
        strip.setPixelColor(i + 2, strip.Color(50, 0, 0));
      } 
       else if (answer == 0) {
        strip.setPixelColor(i + 2, strip.Color(0, 0, 0));
      } else {
        strip.setPixelColor(i + 2, strip.Color(0, 0, 50));
      }
      idx = response.indexOf('\n', idx) + 1;
    }
  }
  strip.show();
  Serial.println("LEDs updated.");
}