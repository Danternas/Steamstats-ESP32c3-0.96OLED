/*
 * ============================================================================
 *  STEAM PROFILE MONITOR — ESP32 + 0.96" I2C OLED (SSD1306)
 * ============================================================================
 *
 *  Description:
 *  ------------
 *    A compact firmware that polls the Steam Web API for a user's profile
 *    statistics and cycles through 3 customizable display pages on a
 *    128×64 SSD1306 OLED. Configuration (Steam ID, API key, layout) is
 *    persisted to LittleFS and can be updated via a captive-portal web UI.
 *
 *  Features:
 *  ----------
 *    • WiFiManager auto-connect / captive-portal fallback
 *    • NTP time sync for accurate account-age calculation
 *    • LittleFS-persisted settings (survives reboot)
 *    • On-device web UI at the ESP's local IP
 *    • 3-page rotating display with configurable stat slots
 *    • Incremental achievement fetching (batched over time to avoid
 *      memory / timeout issues on large libraries)
 *    • 15-minute main-data cache, 1-second achievement batch cadence
 *
 *  Hardware:
 *  ----------
 *    ESP32 (any variant with WiFi)
 *    0.96" I2C OLED — SSD1306 driver (128×64)
 *    I2C pins: SDA=GPIO21, SCL=GPIO22 (Arduino default)
 *    OLED I2C address: 0x3C
 *
 *  Libraries Required (PlatformIO / Arduino Library Manager):
 *  ----------------------------------------------------------
 *    adafruit/Adafruit SSD1306     ^2.5.7
 *    adafruit/Adafruit GFX Library ^1.11.5
 *    tzapu/WiFiManager             ^2.0.17
 *    bblanchon/ArduinoJson         ^7.0.4
 *    (WebServer is built into ESP32 Arduino core)
 *
 *  Author:  [Your Name]
 *  Created: [Date]
 *  License: MIT
 * ============================================================================
 */

/* ================================================================
 * SECTION 1:  LIBRARY IMPORTS
 * ================================================================
 *  Core networking, display, filesystem, JSON parsing, and time
 *  synchronization libraries.
 */
#include <WebServer.h>          // Built-in ESP32 HTTP server
#include <WiFiManager.h>        // Auto WiFi provisioning & captive portal
#include <Adafruit_GFX.h>       // Graphics primitives base library
#include <Adafruit_SSD1306.h>   // SSD1306 OLED driver
#include <LittleFS.h>           // Lightweight SPIFFS-like filesystem
#include <HTTPClient.h>         // HTTP client for Steam API calls
#include <ArduinoJson.h>        // JSON serialization / deserialization
#include <time.h>               // Standard C time functions
#include <esp_sntp.h>           // ESP32 NTP client for time sync

/* ================================================================
 * SECTION 2:  HARDWARE & DISPLAY CONFIGURATION
 * ================================================================
 *  SSD1306 display object and hardware-level defines.
 */

// --- OLED Display Constants ---
#define SCREEN_WIDTH  128       // Pixel width of the 0.96" OLED
#define SCREEN_HEIGHT  64       // Pixel height of the 0.96" OLED
#define OLED_RESET    -1        // Use software reset (-1) instead of hardware pin

// Instantiate the display object on the default I2C bus (Wire)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

/* ================================================================
 * SECTION 3:  CONFIGURATION STORAGE VARIABLES
 * ================================================================
 *  Persistent user credentials loaded from / saved to LittleFS.
 */
char steam_id[32]  = "";       // Steam ID64 (e.g., "76561198012345678")
char api_token[64] = "";       // Steam Web API Key (32-char hex string)

/* ================================================================
 * SECTION 4:  PAGE LAYOUT ARRAYS
 * ================================================================
 *  Each of the 3 display pages has one "big" (large text) slot and
 *  two "small" (normal text) slots. Array indices map to positions
 *  in the statLabels[] / statValues[] arrays defined below.
 *
 *  Layout indices → statLabels index:
 *    0 = User      1 = Level       2 = Playtime
 *    3 = Games     4 = Badges      5 = XP Total
 *    6 = Acct Age  7 = Status      8 = Bans      9 = Achvs
 *
 *  Default page layout:
 *    Page 1 → Big: Level(1),   Small1: Games(3),   Small2: Acct Age(6)
 *    Page 2 → Big: Status(7),  Small1: Playtime(4), Small2: Bans(8)   ← typo in original: uses 4→Badges & 8→Bans
 *    Page 3 → Big: Games(3),   Small1: XP(4),      Small2: Acct Age(6)
 *
 *  NOTE: The original defaults use {1,7,3} / {3,4,4} / {6,8,6}.
 *        Adjust these or use the web UI to customize.
 */
int page_big[3]     = {1, 7, 3};  // Big-stat index for each of the 3 pages
int page_small_1[3] = {3, 4, 4};  // Top small-stat index for each page
int page_small_2[3] = {6, 8, 6};  // Bottom small-stat index for each page

// How many seconds each page stays on-screen before rotating
int rotationInterval = 10;

/* ================================================================
 * SECTION 5:  RUNTIME VARIABLES & CACHING
 * ================================================================
 *  State tracking for display updates, network status, and API
 *  polling intervals.
 */
unsigned long lastFetch = 0;                  // millis() of last full API fetch
const unsigned long FETCH_INTERVAL_MS = 900000UL; // 15 minutes between full refreshes

// Human-readable labels (index 0–9) and their current display values
// These are updated by fetchAndParseProfile() and read by drawDisplay()
String statLabels[] = {
  "User",     // 0 — Display name
  "Level",    // 1 — Steam account level
  "Playtime", // 2 — Lifetime playtime across all games
  "Games",    // 3 — Total games owned
  "Badges",   // 4 — Steam badges collected
  "XP Total", // 5 — Total experience points
  "Acct Age", // 6 — Account creation age (years + days)
  "Status",   // 7 — Online / Offline / In-Game / Away / Busy
  "Bans",     // 8 — VAC / Community ban status
  "Achvs"     // 9 — Achievements unlocked / total
};

// Current values for each stat slot; updated during API fetch
String statValues[] = {
  "Loading...", // 0
  "-",          // 1
  "-",          // 2
  "-",          // 3
  "-",          // 4
  "-",          // 5
  "-",          // 6
  "Offline",    // 7
  "Good",       // 8
  "-/-"         // 9
};

// Flag: when true, the OLED buffer is redrawn on the next loop tick
bool needDisplayUpdate = true;

// Track WiFi connection state changes to trigger display refresh
int lastWifiStatus = -1;

// Which page (0, 1, or 2) is currently displayed
int currentPage = 0;
unsigned long lastPageSwitch = 0; // millis() of last page rotation

/* ================================================================
 * SECTION 6:  ACHIEVEMENT BATCHING STATE
 * ================================================================
 *  Achievement data is fetched incrementally in small batches to
 *  avoid overwhelming the ESP32's limited PSRAM / heap. Each loop
 *  iteration processes up to ACHV_BATCH_SIZE games.
 */
uint32_t ownedAppIds[300];   // Buffer for collected AppIDs from game library
int ownedAppIdCount = 0;     // Number of valid entries in ownedAppIds[]

unsigned long totalAchieved = 0;   // Running sum of unlocked achievements
unsigned long totalPossible = 0;   // Running sum of total available achievements

int achvFetchIndex = 0;            // Current position in ownedAppIds[] queue
unsigned long lastAchvFetch = 0;   // millis() of last achievement batch fetch

const unsigned long ACHV_FETCH_INTERVAL_MS = 1000;  // 1s cooldown between batches
const int ACHV_BATCH_SIZE = 5;                       // Max games per batch cycle

/* ================================================================
 * SECTION 7:  WEB SERVER INSTANCE
 * ================================================================
 *  Lightweight HTTP server running on port 80 for configuration UI.
 */
WebServer server(80);

/* ================================================================
 * SECTION 8:  DISPLAY HELPER FUNCTIONS
 * ================================================================
 *  All OLED rendering logic lives here. Each function clears the
 *  buffer, draws its layout, and calls display.display() to push
 *  to the screen via I2C.
 */

/**
 * @brief Renders the captive-portal hotspot active screen.
 * @param ssid  The SSID of the ESP32's access point.
 *
 * Called by WiFiManager's AP callback so the user sees their
 * configuration SSID on the OLED while connecting from a phone.
 */
void drawHotspotInfo(const char* ssid) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // --- Title banner ---
  display.setCursor(0, 0);
  display.println("== HOTSPOT ACTIVE ==");
  display.drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);

  // --- SSID information ---
  display.setCursor(0, 16);
  display.print("SSID: ");
  display.println(ssid);

  // --- Default AP IP address ---
  display.setCursor(0, 28);
  display.print("IP:   192.168.4.1");

  // --- Instructions ---
  display.setCursor(0, 44);
  display.println("Connect to WiFi");
  display.setCursor(0, 54);
  display.println("to setup device.");

  display.display();  // Push buffer to OLED
}

/**
 * @brief WiFiManager AP-mode callback.
 *
 * Invoked automatically when WiFiManager enters configuration
 * portal mode (no saved credentials or connection failed).
 * Prints debug info to Serial and updates the OLED.
 */
void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("[WIFI] Entered Configuration Mode.");
  Serial.print("[WIFI] Config SSID: ");
  Serial.println(myWiFiManager->getConfigPortalSSID());

  // Reflect the hotspot SSID on the OLED display
  drawHotspotInfo(myWiFiManager->getConfigPortalSSID().c_str());
}

/**
 * @brief Renders a "setup required" prompt on the OLED.
 *
 * Shown when the ESP is connected to WiFi but no Steam
 * credentials have been configured yet. Displays the local
 * IP so the user can open the web UI in their browser.
 */
void drawSetupPrompt() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("  == NEED SETUP ==");
  display.drawLine(0, 12, SCREEN_WIDTH, 12, SSD1306_WHITE);

  display.setCursor(0, 24);
  display.println("Open browser to:");
  display.setCursor(0, 36);
  display.print("http://");
  display.println(WiFi.localIP());

  display.display();
}

/**
 * @brief Main display rendering function.
 * @param isConnected  Whether the ESP32 currently has a WiFi link.
 *
 * Draws the full 3-slot stat page: a centered username header,
 * a network-status spinner icon, a large "big" stat, and two
 * smaller sub-stats. Uses pageBig/pageSmall_1/pageSmall_2 arrays
 * to determine which stat indices to show for the current page.
 */
void drawDisplay(bool isConnected) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // --- Header: Centered Username (index 0) ---
  display.setTextSize(1);
  String userHeader = statValues[0];

  // Truncate long names to fit the 128px screen
  if (userHeader.length() > 14)
    userHeader = userHeader.substring(0, 12) + "..";

  // Calculate horizontal centering offset using getTextBounds()
  int16_t hX1, hY1;
  uint16_t hW, hH;
  display.getTextBounds(userHeader, 0, 0, &hX1, &hY1, &hW, &hH);
  int headerX = (SCREEN_WIDTH - hW) / 2;

  display.setCursor(headerX, 0);
  display.print(userHeader);

  // --- Network Status Spinner (top-right corner) ---
  if (!isConnected) {
    // Blinking dot when not connected (toggles every 500ms)
    if ((millis() / 500) % 2 == 0)
      display.fillRect(124, 2, 2, 2, SSD1306_WHITE);
  } else {
    // 4-frame rotating spinner (changes every 10 seconds)
    int spinnerState = (millis() / 10000) % 4;
    switch (spinnerState) {
      case 0: display.drawFastVLine(124, 0, 6, SSD1306_WHITE);  break; // Vertical line
      case 1: display.drawLine(122, 5, 127, 0, SSD1306_WHITE); break; // Diagonal ↘
      case 2: display.drawFastHLine(122, 2, 6, SSD1306_WHITE); break; // Horizontal line
      case 3: display.drawLine(122, 0, 127, 5, SSD1306_WHITE); break; // Diagonal ↙
    }
  }

  // Separator line below header
  display.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_WHITE);

  // --- Determine which stat indices to show for current page ---
  int activeBig    = page_big[currentPage];
  int activeSmall1 = page_small_1[currentPage];
  int activeSmall2 = page_small_2[currentPage];

  // --- Big Stat (large font) ---
  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print(statLabels[activeBig] + ":");

  display.setTextSize(2);  // Doubled size for the value
  display.setCursor(0, 24);

  String bigVal = statValues[activeBig];
  // Truncate very long values to prevent overflow past 128px
  if (bigVal.length() > 10)
    bigVal = bigVal.substring(0, 8) + "..";
  display.print(bigVal);

  // --- Small Stats (normal font, two rows) ---
  display.setTextSize(1);

  // Top small stat (row 1)
  display.setCursor(0, 44);
  display.print(statLabels[activeSmall1] + ": ");
  display.print(statValues[activeSmall1]);

  // Bottom small stat (row 2)
  display.setCursor(0, 54);
  display.print(statLabels[activeSmall2] + ": ");
  display.print(statValues[activeSmall2]);

  display.display();  // Push complete frame to OLED
}

/* ================================================================
 * SECTION 9:  LITTLEFS FILE I/O
 * ================================================================
 *  Persistent storage for all user configuration. File format is
 *  a simple newline-delimited text file at /config_pages.txt:
 *
 *    Line 1:  steam_id
 *    Line 2:  api_token
 *    Line 3–5:  page_big[0..2]
 *    Line 6–8:  page_small_1[0..2]
 *    Line 9–11: page_small_2[0..2]
 *    Line 12: rotationInterval
 */

/**
 * @brief Loads configuration from LittleFS on startup.
 *
 * Opens /config_pages.txt and parses each line into the
 * corresponding global variables. If the file doesn't exist
 * or LittleFS fails to mount, defaults are retained.
 */
void loadConfigFile() {
  // Mount LittleFS (true = format if corrupted)
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Failed to mount LittleFS");
    return;
  }

  // Check if config file exists
  if (LittleFS.exists("/config_pages.txt")) {
    File configFile = LittleFS.open("/config_pages.txt", "r");
    if (configFile) {
      // --- Read Steam credentials ---
      String idStr = configFile.readStringUntil('\n');
      idStr.trim();
      idStr.toCharArray(steam_id, 32);

      String tokStr = configFile.readStringUntil('\n');
      tokStr.trim();
      tokStr.toCharArray(api_token, 64);

      // --- Read page layout arrays (3 values each) ---
      for (int i = 0; i < 3; i++) {
        if (configFile.available())
          page_big[i] = configFile.readStringUntil('\n').toInt();
        if (configFile.available())
          page_small_1[i] = configFile.readStringUntil('\n').toInt();
        if (configFile.available())
          page_small_2[i] = configFile.readStringUntil('\n').toInt();
      }

      // --- Read rotation interval ---
      if (configFile.available())
        rotationInterval = configFile.readStringUntil('\n').toInt();
      if (rotationInterval < 5)
        rotationInterval = 5;  // Enforce minimum 5-second rotation

      configFile.close();
      Serial.println("[FS] Configuration loaded from LittleFS.");
    }
  } else {
    Serial.println("[FS] No config file found; using defaults.");
  }
}

/**
 * @brief Saves current configuration to LittleFS.
 *
 * Called after the user submits the web UI form. Overwrites
 * /config_pages.txt with all current global settings.
 */
void saveConfigFile() {
  File configFile = LittleFS.open("/config_pages.txt", "w");
  if (!configFile) {
    Serial.println("[FS] Failed to open file for writing.");
    return;
  }

  configFile.println(steam_id);
  configFile.println(api_token);

  // Write all 9 page layout values
  for (int i = 0; i < 3; i++) {
    configFile.println(page_big[i]);
    configFile.println(page_small_1[i]);
    configFile.println(page_small_2[i]);
  }

  configFile.println(rotationInterval);
  configFile.close();

  Serial.println("[FS] Configuration saved to LittleFS.");
  needDisplayUpdate = true;  // Trigger a display refresh
}

/* ================================================================
 * SECTION 10:  LOCAL WEB SERVER — HTML GENERATION & ROUTE HANDLERS
 * ================================================================
 *  A single-page web UI styled with Steam's dark color palette.
 *  Routes:
 *    GET  /          — Main configuration form
 *    POST /save      — Accept form data, save to LittleFS
 *    POST /reset_wifi — Wipe WiFi credentials and restart
 */

/**
 * @brief Generates <option> HTML elements for stat-select dropdowns.
 * @param selectedValue  The currently selected stat index.
 * @return HTML string of all <option> tags with the correct one marked selected.
 */
String generateSelectOptions(int selectedValue) {
  String out = "";
  for (int i = 0; i < 10; i++) {  // 10 stat labels total (indices 0–9)
    out += "<option value='" + String(i) + "' ";
    out += (selectedValue == i ? "selected" : "");
    out += ">" + statLabels[i] + "</option>";
  }
  return out;
}

/**
 * @brief Handles GET / — Renders the full configuration web page.
 *
 * Builds the HTML string inline with embedded CSS styled after
 * Steam's dark theme. Includes inputs for Steam ID, API key,
 * rotation speed, and three page-layout dropdown groups.
 */
void handleRoot() {
  String html = R"(<!DOCTYPE html><html><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>)";

  // --- Embedded CSS (Steam-inspired dark theme) ---
  html += "<style>body{font-family:sans-serif;background:#1b2838;color:#c7d5e0;padding:20px;}";
  html += ".card{background:#171a21;padding:20px;border-radius:5px;max-width:450px;margin:auto;box-shadow:0 4px 8px rgba(0,0,0,0.3);}";
  html += "h2, h3{color:#66c0f4;margin-top:0;}";
  html += "input[type=text], select{width:100%;padding:8px;box-sizing:border-box;background:#2a475e;color:#fff;border:1px solid #1b2838;border-radius:3px;margin-bottom:5px;}";
  html += ".btn{background:#5c7e10;color:#fff;border:0;padding:10px 15px;cursor:pointer;border-radius:3px;width:100%;font-weight:bold;margin-top:15px;}";
  html += ".btn:hover{background:#7a9c1e;}";
  html += ".btn-warn{background:#a32a2a;margin-top:25px;}";
  html += ".btn-warn:hover{background:#c43b3b;}";
  html += "hr{border:0;border-top:1px solid #2a475e;margin:20px 0;}";
  html += ".row{margin-bottom:12px;}";
  html += ".pg-box{background:#202530;padding:12px;border-radius:4px;margin-bottom:15px;border-left:4px solid #66c0f4;}";
  html += ".hint{font-size:0.8em;color:#66c0f4;text-decoration:none;display:inline-block;margin-top:2px;margin-bottom:8px;}";
  html += ".hint:hover{text-decoration:underline;}</style></head><body>";

  // --- Card container and form opening ---
  html += "<div class='card'><h2>Steam Monitor Setup</h2>";
  html += "<form action='/save' method='POST'>";

  // --- Steam ID64 input ---
  html += "<div class='row'><label><b>Steam ID64:</b></label><br>";
  html += "<input type='text' name='steam_id' value='" + String(steam_id) + "' placeholder='e.g., 7656119...'>";
  html += "<a class='hint' href='https://steamid.io/' target='_blank'>Find your SteamID64 number &rarr;</a>";
  html += "</div>";

  // --- Steam Web API Key input ---
  html += "<div class='row'><label><b>Steam Web API Key:</b></label><br>";
  html += "<input type='text' name='api_token' value='" + String(api_token) + "' placeholder='Enter your Steam API Key'>";
  html += "<a class='hint' href='https://steamcommunity.com/dev/apikey' target='_blank'>Get Steam API Key &rarr;</a>";
  html += "</div><hr>";

  // --- Rotation speed input ---
  html += "<div class='row'><label><b>🔄 Page Rotation Speed (Seconds):</b></label><br>";
  html += "<input type='number' name='rot_speed' min='5' max='300' value='" + String(rotationInterval) + "' style='width:100%;padding:8px;background:#2a475e;color:#fff;border:1px solid #1b2838;border-radius:3px;'>";
  html += "</div><hr>";

  // --- Per-page layout dropdowns (3 pages × 3 slots) ---
  for (int i = 0; i < 3; i++) {
    html += "<div class='pg-box'><h3>📄 Page " + String(i + 1) + " Layout</h3>";
    html += "<div class='row'><label>⭐ Main Highlight Stat (Big Size):</label><br>";
    html += "<select name='p" + String(i) + "_big'>" + generateSelectOptions(page_big[i]) + "</select></div>";
    html += "<div class='row'><label>▫ Sub Stat Slot 1 (Small):</label><br>";
    html += "<select name='p" + String(i) + "_s1'>" + generateSelectOptions(page_small_1[i]) + "</select></div>";
    html += "<div class='row'><label>▫ Sub Stat Slot 2 (Small):</label><br>";
    html += "<select name='p" + String(i) + "_s2'>" + generateSelectOptions(page_small_2[i]) + "</select></div>";
    html += "</div>";
  }

  // --- Submit and reset buttons ---
  html += "<input type='submit' value='Save All Settings' class='btn'></form>";
  html += "<hr><form action='/reset_wifi' method='POST' onsubmit=\"return confirm('Reset Wi-Fi?');\">";
  html += "<input type='submit' value='Reset Wi-Fi Settings' class='btn btn-warn'></form>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

/**
 * @brief Handles POST /save — Processes form submission.
 *
 * Extracts all POST parameters, updates global variables,
 * persists to LittleFS, and returns a success page that
 * auto-redirects back to the main form after 1.5 seconds.
 */
void handleSave() {
  // --- Extract credentials ---
  if (server.hasArg("steam_id"))
    server.arg("steam_id").toCharArray(steam_id, 32);
  if (server.hasArg("api_token"))
    server.arg("api_token").toCharArray(api_token, 64);
  if (server.hasArg("rot_speed"))
    rotationInterval = server.arg("rot_speed").toInt();

  // --- Extract page layout selections ---
  for (int i = 0; i < 3; i++) {
    String bigKey = "p" + String(i) + "_big";
    String s1Key  = "p" + String(i) + "_s1";
    String s2Key  = "p" + String(i) + "_s2";

    if (server.hasArg(bigKey)) page_big[i]     = server.arg(bigKey).toInt();
    if (server.hasArg(s1Key))  page_small_1[i] = server.arg(s1Key).toInt();
    if (server.hasArg(s2Key))  page_small_2[i] = server.arg(s2Key).toInt();
  }

  // Persist to filesystem and reset the fetch cache timer
  saveConfigFile();
  lastFetch = 0;

  // Send success confirmation with auto-redirect
  server.send(200, "text/html",
    "<html><body style='background:#1b2838;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;'>"
    "<h2>Configuration Saved Successfully!</h2>"
    "</body><script>setTimeout(function(){window.location.href='/';},1500);</script></html>");
}

/**
 * @brief Handles POST /reset_wifi — Wipes WiFiManager credentials.
 *
 * Sends a brief confirmation page, resets stored WiFi settings,
 * and restarts the ESP32 to re-enter captive-portal mode.
 */
void handleWifiReset() {
  server.send(200, "text/html",
    "<html><body style='background:#1b2838;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;'>"
    "<h2>Resetting Wi-Fi...</h2></body></html>");

  delay(1000);  // Allow the HTTP response to fully transmit

  WiFiManager wm;
  wm.resetSettings();   // Delete stored SSID/password
  ESP.restart();        // Reboot to re-enter setup mode
}

/* ================================================================
 * SECTION 11:  STEAM API DATA FETCHING
 * ================================================================
 *  Calls four separate Steam Web API endpoints to gather:
 *    1. Player Summary     → Username, account age, presence status
 *    2. Player Badges      → Level, XP, badge count
 *    3. Player Bans        → VAC & Community ban status
 *    4. Owned Games        → Game count, playtime, AppID list
 *
 *  Uses streaming JSON deserialization for the large games
 *  response to avoid heap overflow.
 */

/**
 * @brief Fetches and parses all Steam profile data.
 * @param targetId  Steam ID64 string.
 * @param apiKey    Steam Web API key.
 * @return true if at least one API call succeeded.
 *
 * Side effects:
 *   - Populates statValues[] array (indices 0–8)
 *   - Collects AppIDs into ownedAppIds[] for later achievement fetching
 *   - Resets achievement batcher counters
 */
bool fetchAndParseProfile(const char* targetId, const char* apiKey) {
  // --- Guard: skip if credentials are not configured ---
  if (targetId[0] == '\0' || apiKey[0] == '\0') {
    Serial.println("[ERROR] Steam ID or API Key is blank. Skipping fetch.");
    return false;
  }

  HTTPClient http;
  DynamicJsonDocument doc(3072);  // Small doc for summary / badges / bans
  bool summarySuccess = false;
  bool badgeSuccess   = false;

  Serial.println("\n=== Starting Steam API Data Fetch ===");
  Serial.print("[DEBUG] Target SteamID64: ");
  Serial.println(targetId);

  // --- Reset achievement batcher for a fresh library scan ---
  ownedAppIdCount = 0;
  achvFetchIndex  = 0;
  totalAchieved   = 0;
  totalPossible   = 0;
  statValues[9]   = "0/0";

  // ============================================================
  // API CALL 1: Player Summary
  // ============================================================
  // Endpoint: ISteamUser/GetPlayerSummaries
  // Returns:  personaname, timecreated, personastate, gameextrainfo
  // ============================================================
  String urlSummary =
    "http://api.steampowered.com/ISteamUser/GetPlayerSummaries/v0002/"
    "?key=" + String(apiKey)
    + "&steamids=" + String(targetId);

  Serial.println("[FETCH] 1/4 — Player Summary...");
  http.begin(urlSummary);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DeserializationError error = deserializeJson(doc, payload);

    if (!error && doc["response"]["players"][0]) {
      JsonObject player = doc["response"]["players"][0];

      // --- Index 0: Username ---
      statValues[0] = player["personaname"].as<String>();
      Serial.print("[DATA] Username: ");
      Serial.println(statValues[0]);

      // --- Index 6: Account Age (calculated from timecreated) ---
      if (!player["timecreated"].isNull()) {
        long creationTime = player["timecreated"].as<long>();
        long currentTime  = time(nullptr);  // Requires successful NTP sync

        if (currentTime > creationTime && currentTime > 1000000) {
          long ageSeconds = currentTime - creationTime;
          long ageYears   = ageSeconds / (365L * 24L * 3600L);
          long ageDays    = (ageSeconds % (365L * 24L * 3600L)) / (24L * 3600L);
          statValues[6] = String(ageYears) + "y " + String(ageDays) + "d";
        } else {
          statValues[6] = "NTP Err";
        }
      } else {
        statValues[6] = "Private";
      }

      // --- Index 7: Online / Offline / In-Game Status ---
      if (!player["gameextrainfo"].isNull()) {
        // Currently playing — show game name
        statValues[7] = player["gameextrainfo"].as<String>();
      } else {
        // Map personastate enum to human-readable string
        int state = player["personastate"].as<int>();
        switch (state) {
          case 0: statValues[7] = "Offline"; break;
          case 1: statValues[7] = "Online";  break;
          case 2: statValues[7] = "Busy";    break;
          case 3: statValues[7] = "Away";    break;
          default: statValues[7] = "Online"; break;
        }
      }
      Serial.print("[DATA] Status: ");
      Serial.println(statValues[7]);
      summarySuccess = true;
    }
  } else {
    Serial.print("[WARN] Summary call failed — HTTP " + String(httpCode));
  }
  http.end();
  doc.clear();

  // ============================================================
  // API CALL 2: Badges, Level & XP
  // ============================================================
  // Endpoint: IPlayerService/GetBadges
  // Returns:  player_level, player_xp, badges[] array
  // ============================================================
  String urlBadges =
    "http://api.steampowered.com/IPlayerService/GetBadges/v1/"
    "?key=" + String(apiKey)
    + "&steamid=" + String(targetId);

  Serial.println("[FETCH] 2/4 — Badges & Levels...");
  http.begin(urlBadges);
  httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DeserializationError error = deserializeJson(doc, payload);

    if (!error && !doc["response"].isNull()) {
      JsonObject resp = doc["response"];

      statValues[1] = resp["player_level"].isNull()
        ? "Private" : resp["player_level"].as<String>();   // Index 1: Level
      statValues[5] = resp["player_xp"].isNull()
        ? "Private" : resp["player_xp"].as<String>();      // Index 5: XP

      if (!resp["badges"].isNull()) {
        statValues[4] = String(resp["badges"].as<JsonArray>().size());  // Index 4: Badge count
      } else {
        statValues[4] = "0";
      }
      badgeSuccess = true;
      Serial.println("[SUCCESS] Badges data parsed.");
    }
  } else {
    Serial.print("[WARN] Badges call failed — HTTP " + String(httpCode));
  }
  http.end();
  doc.clear();

  // ============================================================
  // API CALL 3: Moderation / Ban Records
  // ============================================================
  // Endpoint: ISteamUser/GetPlayerBans
  // Returns:  VACBanned (bool), CommunityBanned (bool)
  // ============================================================
  String urlBans =
    "http://api.steampowered.com/ISteamUser/GetPlayerBans/v1/"
    "?key=" + String(apiKey)
    + "&steamids=" + String(targetId);

  Serial.println("[FETCH] 3/4 — Moderation Records...");
  http.begin(urlBans);
  httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DeserializationError error = deserializeJson(doc, payload);

    if (!error && doc["players"][0]) {
      JsonObject banData = doc["players"][0];
      bool vacBanned       = banData["VACBanned"].as<bool>();
      bool communityBanned = banData["CommunityBanned"].as<bool>();

      // Index 8: Ban status summary
      if (vacBanned || communityBanned) {
        statValues[8] = "Flagged/Ban";
      } else {
        statValues[8] = "Clean";
      }
      Serial.print("[DATA] Standing: ");
      Serial.println(statValues[8]);
    }
  } else {
    Serial.print("[WARN] Bans call failed — HTTP " + String(httpCode));
  }
  http.end();
  doc.clear();

  // ============================================================
  // API CALL 4: Owned Games (streaming parse)
  // ============================================================
  // Endpoint: IPlayerService/GetOwnedGames
  // Returns:  game_count, games[] with playtime_forever, appid
  // NOTE: Uses getStream() for large responses to avoid heap issues
  // ============================================================
  String urlGames =
    "http://api.steampowered.com/IPlayerService/GetOwnedGames/v1/"
    "?key=" + String(apiKey)
    + "&steamid=" + String(targetId)
    + "&include_appinfo=0&include_played_free_games=1";

  Serial.println("[FETCH] 4/4 — Game Library (streaming)...");
  http.begin(urlGames);
  httpCode = http.GET();

  if (httpCode == 200) {
    // Larger doc for the games array
    DynamicJsonDocument bigDoc(16384);
    WiFiClient& stream = http.getStream();

    DeserializationError error = deserializeJson(bigDoc, stream);

    if (!error && !bigDoc["response"].isNull()) {
      JsonObject resp = bigDoc["response"];

      // --- Index 3: Total games count ---
      statValues[3] = resp["game_count"].isNull()
        ? "0" : resp["game_count"].as<String>();

      // --- Index 2: Total playtime + collect AppIDs for achievements ---
      if (!resp["games"].isNull()) {
        JsonArray gamesArray = resp["games"].as<JsonArray>();
        unsigned long totalMinutes = 0;

        for (JsonObject game : gamesArray) {
          // Accumulate playtime (minutes)
          totalMinutes += game["playtime_forever"].as<unsigned long>();

          // Collect AppIDs (capped at 300 to fit the buffer)
          if (ownedAppIdCount < 300) {
            ownedAppIds[ownedAppIdCount++] = game["appid"].as<uint32_t>();
          }
        }

        // Convert minutes → "X days Y hours" format
        unsigned long playDays  = totalMinutes / (24UL * 60UL);
        unsigned long playHours = (totalMinutes % (24UL * 60UL)) / 60UL;
        statValues[2] = String(playDays) + "d " + String(playHours) + "h";
      } else {
        statValues[2] = "Private";  // Game details are private
      }
      Serial.println("[SUCCESS] Games library parsed via direct stream.");
    } else {
      Serial.print("[ERROR] Games stream parse: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.print("[WARN] Games call failed — HTTP " + String(httpCode));
  }
  http.end();

  Serial.println("=== Finished Steam API Data Fetch ===\n");

  // Return true if at least the summary or badges were fetched
  return (summarySuccess || badgeSuccess);
}

/* ================================================================
 * SECTION 12:  ACHIEVEMENT BATCH FETCHER
 * ================================================================
 *  Achievements are fetched one small batch at a time (5 games per
 *  call) with a 1-second delay between batches. This prevents
 *  request timeouts and heap exhaustion on large libraries.
 *
 *  Called from loop() after each full profile fetch completes and
 *  the AppID list is populated.
 */

/**
 * @brief Fetches achievement data for ACHV_BATCH_SIZE games.
 *
 * Reads the next N AppIDs from ownedAppIds[], calls the Steam
 * achievement API for each, and accumulates totals into
 * totalAchieved / totalPossible. Updates statValues[9] with
 * the running "unlocked / total" count.
 *
 * Safe to call repeatedly — returns immediately when all games
 * have been processed.
 */
void fetchAchievementBatch() {
  // Exit early if no games collected or all games already processed
  if (ownedAppIdCount == 0 || achvFetchIndex >= ownedAppIdCount) {
    if (achvFetchIndex >= ownedAppIdCount && ownedAppIdCount > 0) {
      Serial.println("[ACHV] Achievement scan complete.");
    }
    return;
  }

  HTTPClient http;
  DynamicJsonDocument doc(4096);

  int gamesInThisBatch = 0;

  // Process up to ACHV_BATCH_SIZE games in this cycle
  while (gamesInThisBatch < ACHV_BATCH_SIZE && achvFetchIndex < ownedAppIdCount) {
    uint32_t currentAppId = ownedAppIds[achvFetchIndex++];

    String urlAchv =
      "http://api.steampowered.com/ISteamUserStats/GetPlayerAchievements/v1/"
      "?key=" + String(api_token)
      + "&steamid=" + String(steam_id)
      + "&appid=" + String(currentAppId);

    http.begin(urlAchv);
    int httpCode = http.GET();

    if (httpCode == 200) {
      String payload = http.getString();
      DeserializationError error = deserializeJson(doc, payload);

      if (!error && doc["playerstats"]["achievements"].is<JsonArray>()) {
        JsonArray achvs = doc["playerstats"]["achievements"];
        totalPossible += achvs.size();  // Count all available achievements

        for (JsonObject achv : achvs) {
          if (achv["achieved"].as<bool>()) {
            totalAchieved++;  // Count only unlocked ones
          }
        }
      }
    }
    // 403 / 404 responses are normal (game has no achievements or is hidden)
    // Silently skipped without logging

    http.end();
    doc.clear();
    gamesInThisBatch++;
  }

  // Update the displayed achievement stat with running totals
  statValues[9] = String(totalAchieved) + "/" + String(totalPossible);
  needDisplayUpdate = true;  // Trigger OLED refresh with new value
}

/* ================================================================
 * SECTION 13:  ESP32 SETUP & MAIN LOOP
 * ================================================================
 *  Standard Arduino entry points. setup() initializes hardware,
 *  establishes WiFi, syncs time via NTP, and starts the web
 *  server. loop() handles display rotation, API polling, and
 *  incremental achievement fetching.
 */

/**
 * @brief Arduino setup() — runs once on startup / reboot.
 *
 * Initialization sequence:
 *   1. Start Serial (115200 baud for debug output)
 *   2. Initialize I2C OLED display
 *   3. Load persisted config from LittleFS
 *   4. Connect to WiFi via WiFiManager (captive portal fallback)
 *   5. Synchronize time via NTP (needed for account-age calc)
 *   6. Register and start the HTTP server
 */
void setup() {
  // --- Step 1: Serial for debugging ---
  Serial.begin(115200);

  // --- Step 2: Initialize OLED display ---
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] OLED initialization failed!");
    for (;;);  // Halt — cannot proceed without display
  }

  // --- Step 3: Load persisted configuration ---
  loadConfigFile();

  // --- Step 4: WiFi connection via WiFiManager ---
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.println("  Connecting WiFi...");
  display.display();

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);       // 3-minute timeout for captive portal
  wm.setAPCallback(configModeCallback); // Update OLED when portal starts

  if (!wm.autoConnect("SteamSetup")) {
    Serial.println("[ERROR] Failed to connect or timeout. Restarting...");
    ESP.restart();
  }
  Serial.print("[SYSTEM] WiFi connected. IP: ");
  Serial.println(WiFi.localIP());

  // --- Step 5: NTP time synchronization ---
  Serial.println("[SYSTEM] Synchronizing time with NTP...");
  display.setCursor(0, 36);
  display.println("  Syncing Time...");
  display.display();

  // Configure timezone (UTC) and NTP servers
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait up to 15 seconds for time sync (time() must return > 1e6)
  unsigned long ntpStart = millis();
  while (time(nullptr) <= 1000000 && millis() - ntpStart < 15000) {
    Serial.print(".");
    delay(500);
  }

  if (time(nullptr) > 1000000) {
    Serial.println("\n[SYSTEM] Time synchronized successfully.");
  } else {
    Serial.println("\n[WARN] NTP sync timed out. Check router DNS/firewall.");
  }

  // --- Step 6: Web server route registration ---
  server.on("/",        handleRoot);       // GET  — Configuration form
  server.on("/save",    HTTP_POST, handleSave);      // POST — Save settings
  server.on("/reset_wifi", HTTP_POST, handleWifiReset); // POST — Reset WiFi

  server.begin();
  Serial.println("[SYSTEM] Web server started on port 80.");

  lastPageSwitch = millis();  // Initialize page rotation timer
  Serial.println("[SYSTEM] Setup complete.\n");
}

/**
 * @brief Arduino loop() — runs continuously after setup().
 *
 * Each iteration:
 *   1. Handle incoming web server requests
 *   2. Check WiFi status changes → trigger display update
 *   3. Rotate pages at the configured interval
 *   4. Fetch Steam profile data every 15 minutes
 *   5. Fetch achievement batches (1 per second)
 *   6. Redraw the display if needed
 *   7. Short delay to prevent loop from spinning too fast
 */
void loop() {
  // --- Handle web server requests (non-blocking) ---
  server.handleClient();

  // --- Track WiFi connection state changes ---
  int currentWifiStatus = WiFi.status();
  bool isConnected = (currentWifiStatus == WL_CONNECTED);
  bool hasValidCredentials = (steam_id[0] != '\0' && api_token[0] != '\0');

  if (currentWifiStatus != lastWifiStatus) {
    lastWifiStatus    = currentWifiStatus;
    needDisplayUpdate = true;  // Refresh display to show/hide spinner
  }

  // --- Page rotation logic ---
  if (hasValidCredentials) {
    unsigned long now = millis();
    unsigned long targetIntervalMs = (unsigned long)rotationInterval * 1000;

    if (now - lastPageSwitch >= targetIntervalMs) {
      currentPage = (currentPage + 1) % 3;  // Cycle: 0 → 1 → 2 → 0...
      lastPageSwitch = now;
      needDisplayUpdate = true;
    }
  }

  // --- Steam API polling (only when connected + configured) ---
  if (isConnected && hasValidCredentials) {
    unsigned long now = millis();

    // Full profile refresh every 15 minutes
    if (now - lastFetch >= FETCH_INTERVAL_MS || lastFetch == 0) {
      if (fetchAndParseProfile(steam_id, api_token)) {
        lastFetch = now;  // Reset timer on success
      } else {
        // On failure, retry sooner (in 12 minutes instead of 15)
        lastFetch = now - (FETCH_INTERVAL_MS - 30000);
      }
      needDisplayUpdate = true;
    }

    // Incremental achievement batch (1 batch per second)
    if (now - lastAchvFetch >= ACHV_FETCH_INTERVAL_MS) {
      lastAchvFetch = now;
      fetchAchievementBatch();
    }
  }

  // --- Spinner animation refresh (every 10 seconds) ---
  static unsigned long lastSpinnerTick = 0;
  if (millis() - lastSpinnerTick >= 10000) {
    lastSpinnerTick   = millis();
    needDisplayUpdate = true;
  }

  // --- Redraw display if state has changed ---
  if (needDisplayUpdate) {
    needDisplayUpdate = false;

    if (!hasValidCredentials && isConnected) {
      // No Steam credentials yet — show setup prompt
      drawSetupPrompt();
    } else {
      // Normal operation — show the current page
      drawDisplay(isConnected);
    }
  }

  // Small delay to prevent the loop from consuming 100% CPU
  delay(100);
}


