#include <WebServer.h>
#include <WiFiManager.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// --- Hardware & Display Settings ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Configuration Storage variables ---
char steam_id[32] = ""; 
char api_token[64] = ""; // Acts as your Steam Web API Key

// Page Layout Arrays (Values correspond to statLabels indices)
int page_big[3]     = {1, 7, 3}; // Defaults -> P1: Level, P2: Status, P3: Games
int page_small_1[3] = {3, 4, 4}; 
int page_small_2[3] = {6, 8, 6}; 

int rotationInterval = 10; 

// --- Runtime Variables & Cache ---
unsigned long lastFetch = 0;
const unsigned long FETCH_INTERVAL_MS = 900000; // 15 minutes cache loop

// Expanded label and tracking arrays to include new real-time presence metrics
String statLabels[]   = {"User", "Level", "Playtime", "Games", "Badges", "XP Total", "Acct Age", "Status", "Bans"};
String statValues[]   = {"Loading...", "-", "-", "-", "-", "-", "-", "Offline", "Good"};

bool needDisplayUpdate = true;
int lastWifiStatus     = -1;
int currentPage        = 0;
unsigned long lastPageSwitch = 0;

// --- Services ---
WebServer server(80);

// ==========================================
// LITTLEFS FILE I/O IMPLEMENTATION
// ==========================================

void loadConfigFile() {
  if (!LittleFS.begin(true)) {
    Serial.println("Failed to mount LittleFS");
    return;
  }
  
  if (LittleFS.exists("/config_pages.txt")) {
    File configFile = LittleFS.open("/config_pages.txt", "r");
    if (configFile) {
      String idStr = configFile.readStringUntil('\n');
      idStr.trim();
      idStr.toCharArray(steam_id, 32);

      String tokStr = configFile.readStringUntil('\n');
      tokStr.trim();
      tokStr.toCharArray(api_token, 64);
      
      for (int i = 0; i < 3; i++) {
        if(configFile.available()) page_big[i] = configFile.readStringUntil('\n').toInt();
        if(configFile.available()) page_small_1[i] = configFile.readStringUntil('\n').toInt();
        if(configFile.available()) page_small_2[i] = configFile.readStringUntil('\n').toInt();
      }
      
      if(configFile.available()) rotationInterval = configFile.readStringUntil('\n').toInt();
      if(rotationInterval < 5) rotationInterval = 5;
      
      configFile.close();
    }
  }
}

void saveConfigFile() {
  File configFile = LittleFS.open("/config_pages.txt", "w");
  if (!configFile) return;
  configFile.println(steam_id);
  configFile.println(api_token);
  
  for (int i = 0; i < 3; i++) {
    configFile.println(page_big[i]);
    configFile.println(page_small_1[i]);
    configFile.println(page_small_2[i]);
  }
  configFile.println(rotationInterval);
  configFile.close();
  needDisplayUpdate = true;
}

// ==========================================
// LOCAL WEB SERVER UI
// ==========================================

String generateSelectOptions(int selectedValue) {
  String out = "";
  for(int i = 0; i < 9; i++) {
    out += "<option value='" + String(i) + "' " + String(selectedValue == i ? "selected" : "") + ">" + statLabels[i] + "</option>";
  }
  return out;
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;background:#1b2838;color:#c7d5e0;padding:20px;}";
  html += ".card{background:#171a21;padding:20px;border-radius:5px;max-width:450px;margin:auto;box-shadow:0 4px 8px rgba(0,0,0,0.3);}";
  html += "h2, h3{color:#66c0f4;margin-top:0;} input[type=text], select{width:100%;padding:8px;box-sizing:border-box;background:#2a475e;color:#fff;border:1px solid #1b2838;border-radius:3px;margin-bottom:5px;}";
  html += ".btn{background:#5c7e10;color:#fff;border:0;padding:10px 15px;cursor:pointer;border-radius:3px;width:100%;font-weight:bold;margin-top:15px;}";
  html += ".btn:hover{background:#7a9c1e;} .btn-warn{background:#a32a2a;margin-top:25px;} .btn-warn:hover{background:#c43b3b;}";
  html += "hr{border:0;border-top:1px solid #2a475e;margin:20px 0;} .row{margin-bottom:12px;} .pg-box{background:#202530;padding:12px;border-radius:4px;margin-bottom:15px;border-left:4px solid #66c0f4;}";
  html += ".hint{font-size:0.8em;color:#66c0f4;text-decoration:none;display:inline-block;margin-top:2px;margin-bottom:8px;} .hint:hover{text-decoration:underline;}</style></head><body>";
  html += "<div class='card'><h2>Steam Monitor Setup</h2><form action='/save' method='POST'>";
  
  html += "<div class='row'><label><b>Steam ID64:</b></label><br>";
  html += "<input type='text' name='steam_id' value='" + String(steam_id) + "' placeholder='e.g., 7656119...'>";
  html += "<a class='hint' href='https://steamid.io/' target='_blank'>Find your SteamID64 number &rarr;</a>";
  html += "</div>";

  html += "<div class='row'><label><b>Steam Web API Key:</b></label><br>";
  html += "<input type='text' name='api_token' value='" + String(api_token) + "' placeholder='Enter your Steam API Key'>";
  html += "<a class='hint' href='https://steamcommunity.com/dev/apikey' target='_blank'>Get Steam API Key &rarr;</a>";
  html += "</div><hr>";

  html += "<div class='row'><label><b>🔄 Page Rotation Speed (Seconds):</b></label><br>";
  html += "<input type='number' name='rot_speed' min='5' max='300' value='" + String(rotationInterval) + "' style='width:100%;padding:8px;background:#2a475e;color:#fff;border:1px solid #1b2838;border-radius:3px;'>";
  html += "</div><hr>";
  
  for (int i = 0; i < 3; i++) {
    html += "<div class='pg-box'><h3>📄 Page " + String(i + 1) + " Layout</h3>";
    html += "<div class='row'><label>⭐ Main Highlight Stat (Big Size):</label><br><select name='p" + String(i) + "_big'>" + generateSelectOptions(page_big[i]) + "</select></div>";
    html += "<div class='row'><label>▫ Sub Stat Slot 1 (Small):</label><br><select name='p" + String(i) + "_s1'>" + generateSelectOptions(page_small_1[i]) + "</select></div>";
    html += "<div class='row'><label>▫ Sub Stat Slot 2 (Small):</label><br><select name='p" + String(i) + "_s2'>" + generateSelectOptions(page_small_2[i]) + "</select></div>";
    html += "</div>";
  }
  
  html += "<input type='submit' value='Save All Settings' class='btn'></form>";
  html += "<hr><form action='/reset_wifi' method='POST' onsubmit=\"return confirm('Reset Wi-Fi?');\">";
  html += "<input type='submit' value='Reset Wi-Fi Settings' class='btn btn-warn'></form>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("steam_id")) server.arg("steam_id").toCharArray(steam_id, 32);
  if (server.hasArg("api_token")) server.arg("api_token").toCharArray(api_token, 64);
  if (server.hasArg("rot_speed")) rotationInterval = server.arg("rot_speed").toInt();
  
  for (int i = 0; i < 3; i++) {
    if (server.hasArg("p" + String(i) + "_big")) page_big[i] = server.arg("p" + String(i) + "_big").toInt();
    if (server.hasArg("p" + String(i) + "_s1")) page_small_1[i] = server.arg("p" + String(i) + "_s1").toInt();
    if (server.hasArg("p" + String(i) + "_s2")) page_small_2[i] = server.arg("p" + String(i) + "_s2").toInt();
  }
  
  saveConfigFile();
  lastFetch = 0; 
  server.send(200, "text/html", "<html><body style='background:#1b2838;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;'><h2>Configuration Saved Successfully!</h2></body><script>setTimeout(function(){window.location.href='/';},1500);</script></html>");
}

void handleWifiReset() {
  server.send(200, "text/html", "<html><body>Resetting Wi-Fi...</body></html>");
  delay(1000);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// ==========================================
// STEAM DIRECT DATA ACQUISITION
// ==========================================

bool fetchAndParseProfile(const char* targetId, const char* apiKey) {
  if (targetId[0] == '\0' || apiKey[0] == '\0') {
    Serial.println("[ERROR] Steam ID or API Key is blank. Skipping fetch.");
    return false;
  }

  HTTPClient http;
  DynamicJsonDocument doc(3072); 
  bool summarySuccess = false;
  bool badgeSuccess = false;

  Serial.println("\n=== Starting Steam API Data Fetch ===");
  Serial.print("[DEBUG] Target SteamID64: "); Serial.println(targetId);

  // --- 1. Player Summary (Name, Age & Current Status) ---
  String urlSummary = "http://api.steampowered.com/ISteamUser/GetPlayerSummaries/v0002/?key=" + String(apiKey) + "&steamids=" + String(targetId);
  Serial.println("[FETCH] Requesting Player Summary...");
  http.begin(urlSummary);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    DeserializationError error = deserializeJson(doc, payload);
    if (!error && doc["response"]["players"][0]) {
      JsonObject player = doc["response"]["players"][0];
      
      // Index 0: Username
      statValues[0] = player["personaname"].as<String>();
      Serial.print("[DATA] Username Found: "); Serial.println(statValues[0]);
      
      // Index 6: Account Age Calculation (Dynamic to 2026 Baseline)
      if (!player["timecreated"].isNull()) {
        long creationTime = player["timecreated"].as<long>();
        long currentTime = 1783296000; 
        long ageYears = (currentTime - creationTime) / (365L * 24L * 3600L);
        statValues[6] = String(ageYears >= 0 ? ageYears : 0) + " yrs";
      } else {
        statValues[6] = "Private";
      }

      // Index 7: Live Presence Status Check
      if (!player["gameextrainfo"].isNull()) {
        statValues[7] = player["gameextrainfo"].as<String>(); // Shows active game name
      } else {
        int state = player["personastate"].as<int>();
        switch(state) {
          case 0:  statValues[7] = "Offline"; break;
          case 1:  statValues[7] = "Online";  break;
          case 2:  statValues[7] = "Busy";    break;
          case 3:  statValues[7] = "Away";    break;
          default: statValues[7] = "Online";  break;
        }
      }
      Serial.print("[DATA] Status: "); Serial.println(statValues[7]);
      summarySuccess = true;
    }
  }
  http.end();
  doc.clear();

  // --- 2. Badges, XP & Level (Fixed IPlayerService Path) ---
  String urlBadges = "http://api.steampowered.com/IPlayerService/GetBadges/v1/?key=" + String(apiKey) + "&steamid=" + String(targetId);
  Serial.println("[FETCH] Requesting Badges & Levels...");
  http.begin(urlBadges);
  httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    DeserializationError error = deserializeJson(doc, payload);
    if (!error && !doc["response"].isNull()) {
      JsonObject resp = doc["response"];
      
      statValues[1] = resp["player_level"].isNull() ? "Private" : resp["player_level"].as<String>();
      statValues[5] = resp["player_xp"].isNull() ? "Private" : resp["player_xp"].as<String>();
      
      if (!resp["badges"].isNull()) {
        statValues[4] = String(resp["badges"].as<JsonArray>().size());
      } else {
        statValues[4] = "0";
      }
      badgeSuccess = true;
      Serial.println("[SUCCESS] Badges data parsed.");
    }
  } else {
    Serial.print("[WARN] Badges failed with HTTP Code: "); Serial.println(httpCode);
  }
  http.end();
  doc.clear();

  // --- 3. Moderation & Ban Records Check ---
  String urlBans = "http://api.steampowered.com/ISteamUser/GetPlayerBans/v1/?key=" + String(apiKey) + "&steamids=" + String(targetId);
  Serial.println("[FETCH] Requesting Moderation Records...");
  http.begin(urlBans);
  httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    DeserializationError error = deserializeJson(doc, payload);
    if (!error && doc["players"][0]) {
      JsonObject banData = doc["players"][0];
      bool vacBanned = banData["VACBanned"].as<bool>();
      bool communityBanned = banData["CommunityBanned"].as<bool>();
      
      // Index 8: Account Ban Summary Status
      if (vacBanned || communityBanned) {
        statValues[8] = "Flagged / Ban";
      } else {
        statValues[8] = "Clean";
      }
      Serial.print("[DATA] Account Standing: "); Serial.println(statValues[8]);
    }
  }
  http.end();
  doc.clear();

  // --- 4. Library Details (Stream-Optimized to Stop RAM Freezes) ---
  String urlGames = "http://api.steampowered.com/IPlayerService/GetOwnedGames/v1/?key=" + String(apiKey) + "&steamid=" + String(targetId) + "&include_appinfo=0&include_played_free_games=1";
  Serial.println("[FETCH] Requesting Games via Network Stream...");
  http.begin(urlGames);
  httpCode = http.GET();
  
  if (httpCode == 200) {
    DynamicJsonDocument bigDoc(16384); // Large document is safe when skipping massive payload string creation
    WiFiClient* stream = http.getStreamPtr();
    DeserializationError error = deserializeJson(bigDoc, *stream);
    
    if (!error && !bigDoc["response"].isNull()) {
      JsonObject resp = bigDoc["response"];
      
      // Index 3: Games Count
      statValues[3] = resp["game_count"].isNull() ? "0" : resp["game_count"].as<String>();
      
      // Index 2: Calculate Combined Lifetime Playtime Hours
      if (!resp["games"].isNull()) {
        JsonArray gamesArray = resp["games"].as<JsonArray>();
        unsigned long totalMinutes = 0;
        for (JsonObject game : gamesArray) {
          totalMinutes += game["playtime_forever"].as<unsigned long>();
        }
        statValues[2] = String(totalMinutes / 60) + "h";
      } else {
        statValues[2] = "Private";
      }
      Serial.println("[SUCCESS] Games library parsed via direct stream.");
    } else {
      Serial.print("[ERROR] Games stream parse status: "); Serial.println(error.c_str());
    }
  } else {
    Serial.print("[WARN] Games failed with HTTP Code: "); Serial.println(httpCode);
  }
  http.end();

  Serial.println("=== Finished Steam API Data Fetch ===\n");
  return (summarySuccess || badgeSuccess);
}

// ==========================================
// RENDER MODULES
// ==========================================

void drawDisplay(bool isConnected) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Header Centering Logics
  display.setTextSize(1);
  String userHeader = statValues[0];
  if(userHeader.length() > 14) userHeader = userHeader.substring(0, 12) + "..";
  
  int16_t hX1, hY1;
  uint16_t hW, hH;
  display.getTextBounds(userHeader, 0, 0, &hX1, &hY1, &hW, &hH);
  
  int headerX = (SCREEN_WIDTH - hW) / 2;
  display.setCursor(headerX, 0);
  display.print(userHeader);
  
  // Dynamic Network Sync Status Spinner
  if (!isConnected) {
    if ((millis() / 500) % 2 == 0) display.fillRect(124, 2, 2, 2, SSD1306_WHITE);
  } else {
    int spinnerState = (millis() / 10000) % 4;
    switch (spinnerState) {
      case 0: display.drawFastVLine(124, 0, 6, SSD1306_WHITE); break;
      case 1: display.drawLine(122, 5, 127, 0, SSD1306_WHITE); break;
      case 2: display.drawFastHLine(122, 2, 6, SSD1306_WHITE); break;
      case 3: display.drawLine(122, 0, 127, 5, SSD1306_WHITE); break;
    }
  }
  
  display.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_WHITE);
  
  int activeBig    = page_big[currentPage];
  int activeSmall1 = page_small_1[currentPage];
  int activeSmall2 = page_small_2[currentPage];

  // Render Highlight Block
  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print(statLabels[activeBig] + ":");
  
  display.setTextSize(2);
  display.setCursor(0, 24); 
  
  // Safety check to truncate extremely long strings (like custom game names) on the big layout field
  String bigVal = statValues[activeBig];
  if (bigVal.length() > 10) bigVal = bigVal.substring(0, 8) + "..";
  display.print(bigVal);

  // Render Sub Rows Info
  display.setTextSize(1);
  
  // Slot 1
  display.setCursor(0, 44);
  display.print(statLabels[activeSmall1] + ": ");
  display.print(statValues[activeSmall1]);

  // Slot 2
  display.setCursor(0, 54);
  display.print(statLabels[activeSmall2] + ": ");
  display.print(statValues[activeSmall2]);
  
  display.display();
}

void drawSetupPrompt() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("  == NEED SETUP ==");
  display.drawLine(0, 12, SCREEN_WIDTH, 12, SSD1306_WHITE);
  display.setCursor(0, 24);
  display.print("http://");
  display.println(WiFi.localIP());
  display.display();
}

// ==========================================
// CORE SYSTEM SETUP & LOOP RUNTIMES
// ==========================================

void setup() {
  Serial.begin(115200);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);
  
  loadConfigFile();
  
  WiFiManager wm;
  wm.setConfigPortalTimeout(180); 
  wm.autoConnect("SteamSetup");

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset_wifi", HTTP_POST, handleWifiReset);
  server.begin();
  
  lastPageSwitch = millis();
}

void loop() {
  server.handleClient();

  int currentWifiStatus = WiFi.status();
  bool isConnected = (currentWifiStatus == WL_CONNECTED);
  bool hasValidCredentials = (steam_id[0] != '\0' && api_token[0] != '\0');

  if (currentWifiStatus != lastWifiStatus) {
    lastWifiStatus = currentWifiStatus;
    needDisplayUpdate = true;
  }

  if (hasValidCredentials) {
    unsigned long now = millis();
    unsigned long targetIntervalMs = (unsigned long)rotationInterval * 1000;
    
    if (now - lastPageSwitch >= targetIntervalMs) {
      currentPage = (currentPage + 1) % 3; 
      lastPageSwitch = now;
      needDisplayUpdate = true;
    }
  }

  if (isConnected && hasValidCredentials) {
    unsigned long now = millis();
    if (now - lastFetch >= FETCH_INTERVAL_MS || lastFetch == 0) {
      if (fetchAndParseProfile(steam_id, api_token)) {
        lastFetch = now;
      } else {
        lastFetch = now - (FETCH_INTERVAL_MS - 30000); // Retry sooner on network blips
      }
      needDisplayUpdate = true;
    }
  }

  static unsigned long lastSpinnerTick = 0;
  if (millis() - lastSpinnerTick >= 10000) {
    lastSpinnerTick = millis();
    needDisplayUpdate = true;
  }

  if (needDisplayUpdate) {
    needDisplayUpdate = false; 
    if (!hasValidCredentials && isConnected) {
      drawSetupPrompt();
    } else {
      drawDisplay(isConnected);
    }
  }
  delay(100); 
}