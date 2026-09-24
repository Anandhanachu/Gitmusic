/*
 * gitmusic.ino
 * ──────────────────────────────────────────────────────────────────────────
 * GitMusic – ESP32 Dev Module Firmware  (v2.0)
 *
 * HARDWARE:
 *   - ESP32 Dev Module  (240 MHz required for DMA matrix driver)
 *   - P3 64x32 HUB75 RGB LED Matrix  (1/16 scan, tested GPIO map below)
 *   - MAX98357A I2S Mono Amplifier + Speaker
 *
 * DISPLAY LAYOUT (64 x 32 pixels):
 *   Rows  0-7  : GitHub username  (scrolls if > ~10 chars)
 *   Rows  8-31 : 7 x 52 contribution streak graph (GitHub-style green cells)
 *                Each column = 1 week (52 weeks), each row = 1 day (7 days)
 *                Cell brightness = contribution level 0-4
 *
 * AUDIO (adapted from niyamax/gitmusic Tone.js architecture):
 *   - Pentatonic scale: C4 D4 E4 G4 A4 C5 D5
 *   - contribution level 0 -> silence
 *   - contribution level 1 -> C4  (note 0)
 *   - contribution level 2 -> D4  (note 1)
 *   - contribution level 3 -> E4  (note 2)
 *   - contribution level 4 -> G4  (note 3)
 *   - Sweep animation plays column-by-column, lighting + sounding each week
 *
 * HUB75 -> ESP32 GPIO MAP (physically tested):
 *   R1=25  G1=26  B1=27
 *   R2=14  G2=12  B2=13
 *   A=23   B=19   C=5    D=17   E=-1 (1/16 scan, unused)
 *   CLK=16 LAT=4  OE=15
 *   clkphase=false, latch_blanking=2
 *
 * MAX98357A I2S -> ESP32 GPIO MAP:
 *   BCLK=18   LRCK=33   DIN=22
 *
 * LIBRARIES REQUIRED (Arduino Library Manager):
 *   - ESP32-HUB75-MatrixPanel-I2S-DMA  (mrfaptastic)
 *   - ArduinoWebsockets  (Gil Maimon)
 *   - ArduinoJson        (Benoit Blanchon)
 *
 * ARDUINO BOARD SETTINGS:
 *   Board:           ESP32 Dev Module
 *   CPU Frequency:   240 MHz  (REQUIRED for DMA driver)
 *   Flash Frequency: 80 MHz
 *   Partition:       Default 4MB with spiffs
 * ──────────────────────────────────────────────────────────────────────────
 */

#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <driver/i2s.h>
#include <math.h>

using namespace websockets;

// ==========================================================================
// USER CONFIGURATION
// ==========================================================================

#define WIFI_SSID           "Fiber"
#define WIFI_PASSWORD       "12345678"

// Backend Host & Port (Your computer's current IP is 10.63.92.168)
#define WS_SERVER_HOST      "10.63.92.168"
#define WS_SERVER_PORT      8000
#define WS_SERVER_PATH      "/ws/device"
#define WS_SERVER_URL       "ws://10.63.92.168:8000/ws/device"
#define DEVICE_ID           "gitmusic-01"
#define RECONNECT_DELAY_MS  5000
#define PING_INTERVAL_MS    20000

// Compatibility macro for ArduinoJson v6 vs v7
#if ARDUINOJSON_VERSION_MAJOR >= 7
  #define ALLOC_JSON_DOC(doc, size) JsonDocument doc
#else
  #define ALLOC_JSON_DOC(doc, size) DynamicJsonDocument doc(size)
#endif

// Compatibility macro for ESP-IDF / Arduino Core I2S communication format
#if defined(I2S_COMM_FORMAT_STAND_I2S)
  #define I2S_FORMAT_DEFAULT I2S_COMM_FORMAT_STAND_I2S
#elif defined(I2S_COMM_FORMAT_I2S)
  #define I2S_FORMAT_DEFAULT I2S_COMM_FORMAT_I2S
#else
  #define I2S_FORMAT_DEFAULT (i2s_comm_format_t)(0x01)
#endif

// ==========================================================================
// HUB75 MATRIX PIN CONFIGURATION  (physically tested)
// ==========================================================================

#define PANEL_WIDTH    64
#define PANEL_HEIGHT   32
#define PANELS_NUMBER   1

#define R1_PIN   25
#define G1_PIN   26
#define B1_PIN   27
#define R2_PIN   14
#define G2_PIN   12
#define B2_PIN   13
#define A_PIN    23
#define B_PIN    19
#define C_PIN     5
#define D_PIN    17
#define E_PIN    -1    // 1/16 scan -- E unused
#define CLK_PIN  16
#define LAT_PIN   4
#define OE_PIN   15

// ==========================================================================
// MAX98357A I2S PIN CONFIGURATION
// ==========================================================================

#define I2S_BCK_PIN   18
#define I2S_WS_PIN    33
#define I2S_DATA_PIN  22
#define I2S_PORT      I2S_NUM_0
#define SAMPLE_RATE   44100

// ==========================================================================
// DISPLAY LAYOUT (64x32 HUB75 RGB LED Matrix)
// ==========================================================================

#define USERNAME_ROW_START   0
#define USERNAME_ROW_END     7

// 7x52 Contribution Grid: 52 weeks horizontally (X), 7 days vertically (Y)
// Coordinate mapping: x = gridX + week, y = gridY + day
const int gridX            = 6;   // Centered horizontally: (64 - 52) / 2 = 6 (X: 6..57)
const int gridY            = 12;  // Centered vertically in available area (Y: 12..18)
#define GRAPH_COLS           52   // 52 weeks horizontally (week 0=oldest on left .. week 51=current on right)
#define GRAPH_ROWS            7   // 7 days vertically (day 0=Sunday .. day 6=Saturday)

// ==========================================================================
// AUDIO - Pentatonic scale (from niyamax/gitmusic useAudioEngine.js)
// Contribution level 0=silence, 1=C4, 2=D4, 3=E4, 4=G4
// ==========================================================================

static const float PENTATONIC_NOTES[] = {
  261.63f,  // C4 -- level 1
  293.66f,  // D4 -- level 2
  329.63f,  // E4 -- level 3
  392.00f,  // G4 -- level 4
  440.00f,  // A4 -- streak bonus
  523.25f,  // C5 -- today highlight
  587.33f,  // D5 -- longest streak
};

#define NOTE_DURATION_MS  35

// ==========================================================================
// COLOUR PALETTE (GitHub contribution graph levels 0-4)
// ==========================================================================

struct RGB { uint8_t r, g, b; };

static const RGB LEVEL_COLORS[5] = {
  {   2,   2,   2 },   // level 0 -- no activity (almost off)
  {   0, 140,  45 },   // level 1 -- clear noticeable green
  {   0, 200,  65 },   // level 2 -- bright green
  {  40, 240,  90 },   // level 3 -- vivid electric green
  { 100, 255, 140 },   // level 4 -- brilliant mint green
};

static const RGB COLOR_USERNAME = { 255, 255, 255 };  // White username per spec
static const RGB COLOR_BG       = {   0,   0,   0 };

// ==========================================================================
// GLOBALS
// ==========================================================================

MatrixPanel_I2S_DMA *dma_display = nullptr;
WebsocketsClient wsClient;

bool wsConnected = false;
unsigned long lastPing = 0;
unsigned long lastReconnectAttempt = 0;

struct GitMusicData {
  char    username[64]    = "";
  int     currentStreak   = 0;
  int     longestStreak   = 0;
  int     today           = 0;
  int     totalContribs   = 0;
  int     weeklyContribs  = 0;
  int     monthlyContribs = 0;
  uint8_t levels[52][7];          // contribution levels grid
  bool    musicEnabled    = false;
  int     musicPattern    = 0;
  float   musicIntensity  = 0.5f;
  bool    sessionActive   = false;
} gmData;

// Scrolling text state
struct ScrollState {
  int           textW    = 0;
  int           scrollX  = 0;
  bool          active   = false;
  unsigned long lastTick = 0;
  const int     tickMs   = 55;
} scroll;

// Display mode
enum DisplayMode { MODE_IDLE, MODE_SWEEP, MODE_STATS, MODE_SESSION_END };
DisplayMode displayMode = MODE_IDLE;

// Sweep animation state
int           sweepCol     = 0;
unsigned long sweepLastMs  = 0;
#define SWEEP_COL_DELAY_MS  45  // ms between column reveals

// Idle animation state
unsigned long idleLastMs = 0;
int           idleHue    = 0;

// ==========================================================================
// FORWARD DECLARATIONS
// ==========================================================================

void connectWifi();
void connectWebSocket();
void handleWebSocketMessage(WebsocketsMessage msg);
void handleWebSocketEvent(WebsocketsEvent event, String data);
#if ARDUINOJSON_VERSION_MAJOR >= 7
void parseGitHubUpdate(JsonDocument& doc);
#else
void parseGitHubUpdate(DynamicJsonDocument& doc);
#endif
void printStats();
void sendRegistration();
void sendPing();

void initMatrix();
void initI2S();
void clearDisplay();
void clearUsernameBanner();
void drawUsername(const char* name, bool resetScroll);
void drawStreakGraph(bool fullReveal);
void drawStreakCell(int weekCol, int dayRow, uint8_t level, bool flash);
void startSweepAnimation();
void tickSweepAnimation();
void celebrateStreakReveal();
void tickStatsAnimation();
void tickIdleAnimation();
void tickScrollText();
void fadeOutDisplay();

void playNote(float freq, int durationMs);
void playStreakChime(int streak);
void playSessionEndTone();
void playIdlePulse();

float levelToFreq(uint8_t level);
uint16_t rgb888to565(RGB c);
RGB  hsv2rgb(float h, float s, float v);
int  getTextPixelWidth(const char* str);
void drawChar3x5(int x, int y, char c, RGB color);
void drawText3x5(int x, int y, const char* str, RGB color);
void fillLevelsFromStreak();

// ==========================================================================
// SETUP
// ==========================================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("======================================================");
  Serial.println("  GitMusic -- ESP32 Dev Module (v2.0)");
  Serial.println("  64x32 HUB75 Matrix + MAX98357A I2S Synthesizer");
  Serial.println("======================================================");

  // [1/4] MATRIX
  Serial.print("[1/4] MATRIX  : Initializing HUB75 (64x32, 1/16 scan)... ");
  initMatrix();
  if (dma_display) {
    Serial.println("OK!");
    drawText3x5(8, 13, "BOOTING", {0, 180, 100});
  } else {
    Serial.println("FAILED!");
  }

  // [2/4] AUDIO
  Serial.print("[2/4] AUDIO   : Initializing MAX98357A I2S (44.1 kHz)... ");
  initI2S();
  Serial.println("OK!");

  displayMode = MODE_IDLE;

  // [3/4] WIFI
  Serial.println("[3/4] WIFI    : Starting connection...");
  connectWifi();

  // [4/4] WEBSOCKET BACKEND
  Serial.printf("[4/4] BACKEND : Connecting to WebSocket at ws://%s:%d%s ...\n", WS_SERVER_HOST, WS_SERVER_PORT, WS_SERVER_PATH);
  wsClient.onMessage(handleWebSocketMessage);
  wsClient.onEvent(handleWebSocketEvent);
  connectWebSocket();

  Serial.println("======================================================");
  Serial.println("  STATUS: SYSTEM INITIALIZED & READY");
  Serial.println("======================================================");
}

// ==========================================================================
// LOOP
// ==========================================================================

void loop() {
  wsClient.poll();

  unsigned long now = millis();

  // Check WiFi status and reconnect if dropped
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWifiCheck = 0;
    if (now - lastWifiCheck >= 5000) {
      lastWifiCheck = now;
      Serial.println("[WiFi] Connection lost, reconnecting...");
      WiFi.reconnect();
    }
  }

  if (wsConnected && (now - lastPing >= PING_INTERVAL_MS)) {
    sendPing();
    lastPing = now;
  }

  if (!wsConnected && (now - lastReconnectAttempt >= RECONNECT_DELAY_MS)) {
    Serial.println("[WS] Attempting reconnect to backend...");
    lastReconnectAttempt = now;
    connectWebSocket();
  }

  switch (displayMode) {
    case MODE_IDLE:
      tickIdleAnimation();
      break;
    case MODE_SWEEP:
      tickSweepAnimation();
      tickScrollText();
      break;
    case MODE_STATS:
      tickStatsAnimation();
      tickScrollText();
      break;
    default:
      break;
  }

  delay(5);
}

// ==========================================================================
// WIFI
// ==========================================================================

void connectWifi() {
  Serial.printf("                Connecting to SSID: \"%s\" ", WIFI_SSID);
  if (dma_display) {
    clearDisplay();
    drawText3x5(8, 13, "WIFI...", {0, 140, 220});
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\n[WiFi] Connection timeout! Restarting ESP32...");
      ESP.restart();
    }
  }
  Serial.println();
  Serial.println("                ✔ WiFi Connected Successfully!");
  Serial.printf("                - IP Address : %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("                - Gateway    : %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("                - Subnet Mask: %s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("                - RSSI Signal: %d dBm\n", WiFi.RSSI());

  if (dma_display) {
    clearDisplay();
    drawText3x5(8, 13, "WIFI OK", {57, 211, 83});
    delay(400);
  }
}

// ==========================================================================
// WEBSOCKET
// ==========================================================================

void connectWebSocket() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("                ✖ WiFi not connected. Cannot reach backend.");
    return;
  }

  // Parse host, port, and path reliably from WS_SERVER_URL or WS_SERVER_HOST
  String host = WS_SERVER_HOST;
  int port = WS_SERVER_PORT;
  String path = WS_SERVER_PATH;

  // Extract from WS_SERVER_URL if provided, stripping any accidental extra slashes
  String raw = WS_SERVER_URL;
  if (raw.startsWith("ws://")) raw = raw.substring(5);
  else if (raw.startsWith("wss://")) raw = raw.substring(6);
  else if (raw.startsWith("http://")) raw = raw.substring(7);
  else if (raw.startsWith("https://")) raw = raw.substring(8);

  while (raw.startsWith("/")) raw = raw.substring(1);

  if (raw.length() > 0) {
    int slashIdx = raw.indexOf('/');
    String hostPort = (slashIdx >= 0) ? raw.substring(0, slashIdx) : raw;
    path = (slashIdx >= 0) ? raw.substring(slashIdx) : WS_SERVER_PATH;

    int colonIdx = hostPort.indexOf(':');
    if (colonIdx >= 0) {
      host = hostPort.substring(0, colonIdx);
      port = hostPort.substring(colonIdx + 1).toInt();
      if (port <= 0) port = 8000;
    } else {
      host = hostPort;
    }
  }

  Serial.printf("                Connecting to Host: %s, Port: %d, Path: %s ...\n",
                host.c_str(), port, path.c_str());

  // Connect directly using host, port, and path (never fails due to URL scheme bugs)
  wsClient.onMessage(handleWebSocketMessage);
  wsClient.onEvent(handleWebSocketEvent);
  bool connected = wsClient.connect(host, port, path);
  if (connected) {
    Serial.println("                ✔ WebSocket Connection Established!");
    wsConnected = true;
    lastPing = millis();
  } else {
    wsConnected = false;
    Serial.println("                ✖ WebSocket Connection Failed!");
    Serial.println("                [Troubleshooting Tip]:");
    Serial.println("                1. Ensure backend is running: uvicorn main:app --host 0.0.0.0 --port 8000");
    Serial.printf ("                2. Ensure host IP is correct (your computer is %s)\n", host.c_str());
    Serial.println("                3. Ensure both ESP32 and PC are on the same Wi-Fi network.");
  }
}

void sendRegistration() {
  ALLOC_JSON_DOC(doc, 128);
  doc["type"]      = "device_register";
  doc["device_id"] = DEVICE_ID;
  char buf[128];
  serializeJson(doc, buf);
  wsClient.send(buf);
  Serial.printf("[Device] Sent registration request (ID: '%s')\n", DEVICE_ID);
}

void sendPing() {
  ALLOC_JSON_DOC(doc, 32);
  doc["type"] = "ping";
  char buf[32];
  serializeJson(doc, buf);
  wsClient.send(buf);
}

void handleWebSocketEvent(WebsocketsEvent event, String data) {
  switch (event) {
    case WebsocketsEvent::ConnectionOpened:
      Serial.println("[WS] ✔ Connected to backend server!");
      wsConnected = true;
      lastPing = millis();
      sendRegistration();
      if (dma_display) {
        clearDisplay();
        drawText3x5(8, 13, "ONLINE", {57, 211, 83});
      }
      break;
    case WebsocketsEvent::ConnectionClosed:
      Serial.println("[WS] ✖ Disconnected from backend server.");
      wsConnected = false;
      gmData.sessionActive = false;
      displayMode = MODE_IDLE;
      break;
    case WebsocketsEvent::GotPing:
      wsClient.pong();
      break;
    default:
      break;
  }
}

void handleWebSocketMessage(WebsocketsMessage msg) {
  String payload = msg.data();
  Serial.printf("[WS Message] <- %s\n", payload.c_str());

  ALLOC_JSON_DOC(doc, 4096);   // 364 levels (0-4) + metadata
  DeserializationError err = deserializeJson(doc, payload);
  if (err) { Serial.printf("[JSON] Deserialization error: %s\n", err.c_str()); return; }

  const char* type = doc["type"] | "";

  if (strcmp(type, "device_registered") == 0) {
    Serial.println("[Device] ✔ Successfully registered with backend as active display device!");

  } else if (strcmp(type, "github_update") == 0) {
    Serial.printf("[Session] ✔ Received GitHub update for user: '%s'\n", doc["username"] | "");
    parseGitHubUpdate(doc);
    printStats();
    startSweepAnimation();

  } else if (strcmp(type, "session_end") == 0) {
    Serial.println("[Session] Session ended by user/backend.");
    playSessionEndTone();
    fadeOutDisplay();
    memset(&gmData, 0, sizeof(gmData));
    gmData.sessionActive = false;
    displayMode = MODE_IDLE;

  } else if (strcmp(type, "pong") == 0) {
    // Keepalive pong received
  } else {
    Serial.printf("[WS] Unhandled message type: '%s'\n", type);
  }
}

// ==========================================================================
// DATA PARSING
// ==========================================================================

#if ARDUINOJSON_VERSION_MAJOR >= 7
void parseGitHubUpdate(JsonDocument& doc) {
#else
void parseGitHubUpdate(DynamicJsonDocument& doc) {
#endif
  strlcpy(gmData.username,       doc["username"]              | "", sizeof(gmData.username));
  gmData.currentStreak   =       doc["current_streak"]        | 0;
  gmData.longestStreak   =       doc["longest_streak"]        | 0;
  gmData.today           =       doc["today"]                 | 0;
  gmData.totalContribs   =       doc["total_contributions"]   | 0;
  gmData.weeklyContribs  =       doc["weekly_contributions"]  | 0;
  gmData.monthlyContribs =       doc["monthly_contributions"] | 0;
  gmData.sessionActive   = true;

  // ── Parse REAL contribution levels from backend ─────────────────────────
  // Backend sends flat_levels[364]: col-major, 52 cols × 7 rows (0=Sun)
  // levels[col][row], col 0 = oldest week, col 51 = current week
  memset(gmData.levels, 0, sizeof(gmData.levels));
  if (doc.containsKey("levels")) {
    JsonArray arr = doc["levels"].as<JsonArray>();
    int idx = 0;
    for (int col = 0; col < 52 && idx < (int)arr.size(); col++) {
      for (int row = 0; row < 7 && idx < (int)arr.size(); row++, idx++) {
        uint8_t v = arr[idx] | 0;
        gmData.levels[col][row] = (v > 4) ? 4 : v;
      }
    }
    Serial.printf("[Levels] Parsed %d real contribution cells from backend.\n", idx);
  } else {
    // Fallback: all zeros (no data) – clearly visible as empty grid
    Serial.println("[Levels] WARNING: No 'levels' array in payload – grid will be empty.");
  }

  if (doc.containsKey("music")) {
    JsonObject music = doc["music"];
    gmData.musicEnabled   = music["enabled"]   | false;
    gmData.musicPattern   = music["pattern"]   | 0;
    gmData.musicIntensity = music["intensity"] | 0.5f;
  }
}

// fillLevelsFromStreak() is retired.
// Real data is now parsed directly in parseGitHubUpdate() from backend 'levels' array.
void fillLevelsFromStreak() { /* no-op – kept for linker compat only */ }

void printStats() {
  Serial.println();
  Serial.println("+----------------------------------+");
  Serial.printf( "|  User     : %-20s|\n", gmData.username);
  Serial.printf( "|  Streak   : %-4d days            |\n", gmData.currentStreak);
  Serial.printf( "|  Longest  : %-4d days            |\n", gmData.longestStreak);
  Serial.printf( "|  Today    : %-4d commits         |\n", gmData.today);
  Serial.printf( "|  Total    : %-6d              |\n", gmData.totalContribs);
  Serial.printf( "|  Weekly   : %-4d                 |\n", gmData.weeklyContribs);
  Serial.printf( "|  Monthly  : %-4d                 |\n", gmData.monthlyContribs);
  Serial.println("+----------------------------------+");
}

// ==========================================================================
// MATRIX INIT
// ==========================================================================

void initMatrix() {
  HUB75_I2S_CFG mxconfig(PANEL_WIDTH, PANEL_HEIGHT, PANELS_NUMBER);
  mxconfig.gpio.r1  = R1_PIN;
  mxconfig.gpio.g1  = G1_PIN;
  mxconfig.gpio.b1  = B1_PIN;
  mxconfig.gpio.r2  = R2_PIN;
  mxconfig.gpio.g2  = G2_PIN;
  mxconfig.gpio.b2  = B2_PIN;
  mxconfig.gpio.a   = A_PIN;
  mxconfig.gpio.b   = B_PIN;
  mxconfig.gpio.c   = C_PIN;
  mxconfig.gpio.d   = D_PIN;
  mxconfig.gpio.e   = E_PIN;
  mxconfig.gpio.clk = CLK_PIN;
  mxconfig.gpio.lat = LAT_PIN;
  mxconfig.gpio.oe  = OE_PIN;
  mxconfig.clkphase       = false;  // tested value
  mxconfig.latch_blanking = 2;      // tested value

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  if (!dma_display->begin()) {
    Serial.println("[Matrix] ERROR: DMA init failed!");
    return;
  }
  dma_display->setBrightness8(255);
  dma_display->clearScreen();
  Serial.println("[Matrix] HUB75 64x32 initialized.");
}

// ==========================================================================
// I2S AUDIO INIT (MAX98357A)
// ==========================================================================

void initI2S() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_FORMAT_DEFAULT;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 64;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_BCK_PIN;
  pins.ws_io_num    = I2S_WS_PIN;
  pins.data_out_num = I2S_DATA_PIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  if (err != ESP_OK) { Serial.printf("[I2S] Install error: %d\n", err); return; }
  err = i2s_set_pin(I2S_PORT, &pins);
  if (err != ESP_OK) { Serial.printf("[I2S] Pin error: %d\n", err); return; }
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("[I2S] MAX98357A initialized (BCK=18, LRCK=33, DIN=22).");
}

// ==========================================================================
// AUDIO GENERATION
//
// Synthesizes a sine wave at the given frequency.
// Includes basic ADSR envelope (attack 10%, release 20%).
// Adapted from niyamax/gitmusic's velocity-modulated note triggering.
// ==========================================================================

void playNote(float freq, int durationMs) {
  if (freq <= 0.0f || durationMs <= 0) return;

  const int   totalSamples = (SAMPLE_RATE * durationMs) / 1000;
  const float amplitude    = 10000.0f * max(0.2f, gmData.musicIntensity);
  const float twoPiF       = 2.0f * PI * freq;

  const int CHUNK = 128;
  int16_t   buf[CHUNK];
  int       written = 0;
  size_t    bytesOut;

  while (written < totalSamples) {
    int chunk = min(CHUNK, totalSamples - written);
    for (int i = 0; i < chunk; i++) {
      float t    = (float)(written + i) / (float)SAMPLE_RATE;
      float frac = (float)(written + i) / (float)totalSamples;
      float env  = 1.0f;
      if (frac < 0.10f) env = frac / 0.10f;         // attack
      if (frac > 0.80f) env = (1.0f - frac) / 0.20f; // release
      buf[i] = (int16_t)(amplitude * env * sinf(twoPiF * t));
    }
    i2s_write(I2S_PORT, buf, chunk * sizeof(int16_t), &bytesOut, portMAX_DELAY);
    written += chunk;
  }
}

// Level 0=silence, 1=C4, 2=D4, 3=E4, 4=G4
float levelToFreq(uint8_t level) {
  if (level == 0) return 0.0f;
  int idx = (int)level - 1;
  if (idx > 3) idx = 3;
  return PENTATONIC_NOTES[idx];
}

// Ascending arpeggio chime on session start (like niyamax chord trigger)
void playStreakChime(int streak) {
  int notes = 3 + min(streak / 7, 4);
  for (int i = 0; i < notes; i++) {
    playNote(PENTATONIC_NOTES[i % 7], 160);
    delay(85);
  }
}

// Descending farewell when session ends
void playSessionEndTone() {
  for (int i = 3; i >= 0; i--) {
    playNote(PENTATONIC_NOTES[i], 180);
    delay(100);
  }
}

// Soft ambient pulse in idle mode
void playIdlePulse() {
  playNote(PENTATONIC_NOTES[0] * 0.5f, 120);
}

// ==========================================================================
// FONT: Compact 3x5 pixel bitmap font (ASCII 32-90)
// Supports A-Z, 0-9, symbols needed for GitHub usernames.
// Each char = 3 columns, each column = 5 bits (bit0=top, bit4=bottom).
// ==========================================================================

static const uint8_t FONT_DATA[59][3] = {
  {0x00,0x00,0x00}, // ' '  32
  {0x00,0x17,0x00}, // '!'  33
  {0x03,0x00,0x03}, // '"'  34
  {0x1F,0x0A,0x1F}, // '#'  35
  {0x16,0x1F,0x0D}, // '$'  36
  {0x13,0x08,0x19}, // '%'  37
  {0x0E,0x15,0x0A}, // '&'  38
  {0x00,0x03,0x00}, // '\'' 39
  {0x00,0x0E,0x11}, // '('  40
  {0x11,0x0E,0x00}, // ')'  41
  {0x0A,0x04,0x0A}, // '*'  42
  {0x04,0x0E,0x04}, // '+'  43
  {0x10,0x08,0x00}, // ','  44
  {0x04,0x04,0x04}, // '-'  45
  {0x00,0x10,0x00}, // '.'  46
  {0x10,0x08,0x04}, // '/'  47
  // 0-9 (48-57)
  {0x0E,0x11,0x0E}, // '0'
  {0x12,0x1F,0x10}, // '1'
  {0x19,0x15,0x12}, // '2'
  {0x11,0x15,0x0E}, // '3'
  {0x07,0x04,0x1F}, // '4'
  {0x17,0x15,0x09}, // '5'
  {0x0E,0x15,0x08}, // '6'
  {0x01,0x19,0x07}, // '7'
  {0x0A,0x15,0x0A}, // '8'
  {0x02,0x15,0x0E}, // '9'
  // :;<=>?@  (58-64)
  {0x00,0x0A,0x00}, // ':'
  {0x10,0x0A,0x00}, // ';'
  {0x04,0x0A,0x11}, // '<'
  {0x0A,0x0A,0x0A}, // '='
  {0x11,0x0A,0x04}, // '>'
  {0x01,0x15,0x02}, // '?'
  {0x0E,0x15,0x1D}, // '@'
  // A-Z  (65-90)
  {0x1E,0x05,0x1E}, // 'A'
  {0x1F,0x15,0x0A}, // 'B'
  {0x0E,0x11,0x11}, // 'C'
  {0x1F,0x11,0x0E}, // 'D'
  {0x1F,0x15,0x11}, // 'E'
  {0x1F,0x05,0x01}, // 'F'
  {0x0E,0x15,0x1C}, // 'G'
  {0x1F,0x04,0x1F}, // 'H'
  {0x11,0x1F,0x11}, // 'I'
  {0x08,0x11,0x0F}, // 'J'
  {0x1F,0x04,0x1B}, // 'K'
  {0x1F,0x10,0x10}, // 'L'
  {0x1F,0x02,0x1F}, // 'M'
  {0x1F,0x06,0x1F}, // 'N'
  {0x0E,0x11,0x0E}, // 'O'
  {0x1F,0x05,0x02}, // 'P'
  {0x0E,0x19,0x1E}, // 'Q'
  {0x1F,0x05,0x1A}, // 'R'
  {0x12,0x15,0x09}, // 'S'
  {0x01,0x1F,0x01}, // 'T'
  {0x0F,0x10,0x0F}, // 'U'
  {0x07,0x18,0x07}, // 'V'
  {0x1F,0x0C,0x1F}, // 'W'
  {0x1B,0x04,0x1B}, // 'X'
  {0x03,0x1C,0x03}, // 'Y'
  {0x19,0x15,0x13}, // 'Z'
};

static const uint8_t GLYPH_UNDERSCORE[3] = { 0x10, 0x10, 0x10 };

const uint8_t* getGlyph(char c) {
  if (c == '_') return GLYPH_UNDERSCORE;
  char uc = (c >= 'a' && c <= 'z') ? c - 32 : c;
  if (uc >= 32 && uc <= 90) return FONT_DATA[uc - 32];
  return FONT_DATA[0]; // space
}

void drawChar3x5(int x, int y, char c, RGB color) {
  const uint8_t* g = getGlyph(c);
  for (int col = 0; col < 3; col++) {
    for (int row = 0; row < 5; row++) {
      if (g[col] & (1 << row)) {
        int px = x + col, py = y + row;
        if (px >= 0 && px < PANEL_WIDTH && py >= 0 && py < PANEL_HEIGHT)
          dma_display->drawPixelRGB888(px, py, color.r, color.g, color.b);
      }
    }
  }
}

void drawText3x5(int x, int y, const char* str, RGB color) {
  int cx = x;
  for (int i = 0; str[i]; i++) {
    drawChar3x5(cx, y, str[i], color);
    cx += 4;
  }
}

int getTextPixelWidth(const char* str) {
  int len = strlen(str);
  if (len == 0) return 0;
  return len * 4 - 1;
}

// ==========================================================================
// COLOUR UTILITIES
// ==========================================================================

uint16_t rgb888to565(RGB c) { return dma_display->color565(c.r, c.g, c.b); }

RGB hsv2rgb(float h, float s, float v) {
  float r, g, b;
  int   i = (int)(h * 6.0f);
  float f = h * 6.0f - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - f * s);
  float t = v * (1.0f - (1.0f - f) * s);
  switch (i % 6) {
    case 0: r=v; g=t; b=p; break;
    case 1: r=q; g=v; b=p; break;
    case 2: r=p; g=v; b=t; break;
    case 3: r=p; g=q; b=v; break;
    case 4: r=t; g=p; b=v; break;
    default:r=v; g=p; b=q; break;
  }
  return { (uint8_t)(r * 60), (uint8_t)(g * 60), (uint8_t)(b * 60) };
}

// ==========================================================================
// DISPLAY HELPERS
// ==========================================================================

void clearDisplay() { dma_display->clearScreen(); }

void clearUsernameBanner() {
  for (int y = USERNAME_ROW_START; y <= USERNAME_ROW_END; y++)
    for (int x = 0; x < PANEL_WIDTH; x++)
      dma_display->drawPixelRGB888(x, y, 0, 0, 0);
}

// ==========================================================================
// USERNAME BANNER (rows 0-7)
// Centered if fits; otherwise auto-scrolls left (marquee effect)
// Pure WHITE text: COLOR_USERNAME = {255, 255, 255}
// ==========================================================================

void drawUsername(const char* name, bool resetScroll) {
  clearUsernameBanner();
  int textW = getTextPixelWidth(name);
  int yPos  = 1;  // 1px top padding; text is 5px tall -> rows 1-5

  if (textW <= PANEL_WIDTH) {
    int xPos = (PANEL_WIDTH - textW) / 2;
    drawText3x5(xPos, yPos, name, COLOR_USERNAME);
    scroll.active = false;
  } else {
    scroll.textW  = textW;
    scroll.active = true;
    if (resetScroll) scroll.scrollX = PANEL_WIDTH + 4;
    drawText3x5(scroll.scrollX, yPos, name, COLOR_USERNAME);
  }
}

void tickScrollText() {
  if (!scroll.active || !gmData.sessionActive) return;
  unsigned long now = millis();
  if (now - scroll.lastTick < (unsigned long)scroll.tickMs) return;
  scroll.lastTick = now;

  scroll.scrollX--;
  if (scroll.scrollX < -(scroll.textW + 4)) scroll.scrollX = PANEL_WIDTH + 4;

  clearUsernameBanner();
  drawText3x5(scroll.scrollX, 1, gmData.username, COLOR_USERNAME);
}

// ==========================================================================
// STREAK GRAPH (52 weeks horizontally x 7 days vertically)
// Mapping: x = gridX + week (0..51), y = gridY + day (0..6)
// Exactly 1 LED per cell. No transposing, no rotation, no stretching.
// ==========================================================================

void drawStreakCell(int weekCol, int dayRow, uint8_t level, bool flash) {
  int x = gridX + weekCol;
  int y = gridY + dayRow;

  if (x < 0 || x >= PANEL_WIDTH || y < 0 || y >= PANEL_HEIGHT) return;

  RGB color;
  if (flash) {
    // Bright cyan-green flash for active-column highlight (sequencer sweep effect)
    color = (level == 0) ? RGB{ 15, 15, 25 } : RGB{ 120, 255, 200 };
  } else {
    color = LEVEL_COLORS[min((int)level, 4)];
  }

  // Exactly 1 LED per cell: x = gridX + week, y = gridY + day
  dma_display->drawPixelRGB888(x, y, color.r, color.g, color.b);
}

void drawStreakGraph(bool fullReveal) {
  // Clear lower display area below username banner
  for (int y = USERNAME_ROW_END + 1; y < PANEL_HEIGHT; y++)
    for (int x = 0; x < PANEL_WIDTH; x++)
      dma_display->drawPixelRGB888(x, y, 0, 0, 0);

  int maxCol = fullReveal ? GRAPH_COLS : sweepCol;
  for (int w = 0; w < maxCol && w < GRAPH_COLS; w++)
    for (int d = 0; d < GRAPH_ROWS; d++)
      drawStreakCell(w, d, gmData.levels[w][d], false);
}

// ==========================================================================
// IDLE ANIMATION
// Slow rainbow wave -- ambient glow while waiting for a user.
// Inspired by niyamax reference app's dark ambient aesthetic.
// ==========================================================================

void tickIdleAnimation() {
  unsigned long now = millis();
  if (now - idleLastMs < 60) return; // ~16 fps
  idleLastMs = now;

  idleHue = (idleHue + 1) % 360;

  for (int x = 0; x < PANEL_WIDTH; x++) {
    for (int y = 0; y < PANEL_HEIGHT; y++) {
      float h = fmodf((idleHue + x * 3 + y * 7) / 360.0f, 1.0f);
      RGB c = hsv2rgb(h, 0.85f, 0.28f);
      dma_display->drawPixelRGB888(x, y, c.r, c.g, c.b);
    }
  }
  // Dim "GITMUSIC" overlay in center
  drawText3x5(4, 13, "GITMUSIC", {70, 70, 70});

  // Ambient idle audio pulse every ~15s
  static unsigned long lastIdleSound = 0;
  if (now - lastIdleSound > 15000) {
    lastIdleSound = now;
    playIdlePulse();
  }
}

// ==========================================================================
// SWEEP ANIMATION & CELEBRATION
//
// Column-by-column reveal of the 52-week contribution graph.
// Directly adapted from niyamax/gitmusic GitSequencer:
//   - activeCol sweeps left to right (oldest to newest week)
//   - Each active column briefly flashes bright cyan-green before settling
//   - Plays a pentatonic note proportional to that week's max contribution level
//   - Zero-flicker incremental drawing: only updates 7 cells per step!
//   - Sparkling grand finale on completion + victory fanfare chime!
// ==========================================================================

void celebrateStreakReveal() {
  // Settle week 51 column first
  for (int d = 0; d < GRAPH_ROWS; d++) {
    drawStreakCell(51, d, gmData.levels[51][d], false);
  }

  // 3 quick sparkling flashes on current week and active streak
  int streakWeeks = (gmData.currentStreak + 6) / 7;
  int startCol = max(0, 52 - min(streakWeeks, 8));

  for (int f = 0; f < 3; f++) {
    for (int w = startCol; w < 52; w++) {
      for (int d = 0; d < GRAPH_ROWS; d++) {
        if (gmData.levels[w][d] > 0) {
          int x = gridX + w;
          int y = gridY + d;
          dma_display->drawPixelRGB888(x, y, 240, 240, 180); // Gold-white sparkle
        }
      }
    }
    delay(60);
    for (int w = startCol; w < 52; w++) {
      for (int d = 0; d < GRAPH_ROWS; d++) {
        drawStreakCell(w, d, gmData.levels[w][d], false);
      }
    }
    delay(40);
  }

  // Celebratory ascending fanfare chime (A4 -> C5 -> D5)
  playNote(PENTATONIC_NOTES[4], 90);
  delay(30);
  playNote(PENTATONIC_NOTES[5], 110);
  delay(30);
  playNote(PENTATONIC_NOTES[6], 180);
}

void startSweepAnimation() {
  sweepCol    = 0;
  sweepLastMs = 0;
  displayMode = MODE_SWEEP;

  int textW = getTextPixelWidth(gmData.username);
  scroll.active = (textW > PANEL_WIDTH);
  scroll.scrollX = PANEL_WIDTH + 4;

  clearDisplay();
  drawUsername(gmData.username, true);

  // Clear lower area below username banner once at start
  for (int y = USERNAME_ROW_END + 1; y < PANEL_HEIGHT; y++) {
    for (int x = 0; x < PANEL_WIDTH; x++) {
      dma_display->drawPixelRGB888(x, y, 0, 0, 0);
    }
  }

  Serial.println("[Sweep] Starting column-reveal sequencer...");
  playStreakChime(gmData.currentStreak);
}

void tickSweepAnimation() {
  unsigned long now = millis();
  if (now - sweepLastMs < SWEEP_COL_DELAY_MS) return;
  sweepLastMs = now;

  if (sweepCol >= GRAPH_COLS) {
    // Settle final column and trigger grand finale sparkle
    celebrateStreakReveal();
    displayMode = MODE_STATS;
    drawStreakGraph(true);
    drawUsername(gmData.username, false);
    Serial.println("[Sweep] Done -> MODE_STATS");
    return;
  }

  // Settle previous column to normal level color
  if (sweepCol > 0) {
    int prev = sweepCol - 1;
    for (int d = 0; d < GRAPH_ROWS; d++) {
      drawStreakCell(prev, d, gmData.levels[prev][d], false);
    }
  }

  // Flash current column (bright highlight during active sweep)
  for (int d = 0; d < GRAPH_ROWS; d++) {
    drawStreakCell(sweepCol, d, gmData.levels[sweepCol][d], true);
  }

  // Find max level in this column and play note
  uint8_t maxLvl = 0;
  for (int d = 0; d < GRAPH_ROWS; d++) {
    if (gmData.levels[sweepCol][d] > maxLvl) maxLvl = gmData.levels[sweepCol][d];
  }

  float freq = levelToFreq(maxLvl);
  if (freq > 0.0f) {
    playNote(freq, NOTE_DURATION_MS);
  }

  sweepCol++;
}

// ==========================================================================
// STATS MODE ANIMATION
// Gentle living breathing pulse on Today cell and periodic streak wave shimmer
// ==========================================================================

unsigned long lastStatsTick = 0;
uint8_t statsPulseStep = 0;

void tickStatsAnimation() {
  if (!gmData.sessionActive) return;
  unsigned long now = millis();
  if (now - lastStatsTick < 100) return; // 10 fps
  lastStatsTick = now;

  statsPulseStep = (statsPulseStep + 1) % 50; // 5.0 second cycle

  // 1. Gently pulse the "Today" cell if user committed today
  if (gmData.today > 0) {
    float pulse = (sinf((float)statsPulseStep * 0.25f) + 1.0f) * 0.5f;
    uint8_t r = (uint8_t)(40 + 80 * pulse);
    uint8_t g = (uint8_t)(200 + 55 * pulse);
    uint8_t b = (uint8_t)(60 + 120 * pulse);

    // Week 51 (current week). Find today's day row (latest active day in week 51)
    int todayRow = 6;
    for (int d = 6; d >= 0; d--) {
      if (gmData.levels[51][d] > 0) { todayRow = d; break; }
    }
    int x = gridX + 51;
    int y = gridY + todayRow;
    dma_display->drawPixelRGB888(x, y, r, g, b);
  }

  // 2. Subtle shimmer wave across active streak columns every 5 seconds
  if (statsPulseStep == 0 && gmData.currentStreak > 0) {
    int streakWeeks = (gmData.currentStreak + 6) / 7;
    int startCol = max(0, 52 - min(streakWeeks, 16));
    for (int w = startCol; w < 52; w++) {
      for (int d = 0; d < GRAPH_ROWS; d++) {
        if (gmData.levels[w][d] > 1) {
          drawStreakCell(w, d, gmData.levels[w][d], true);
        }
      }
      delay(20);
      for (int d = 0; d < GRAPH_ROWS; d++) {
        drawStreakCell(w, d, gmData.levels[w][d], false);
      }
    }
  }
}

// ==========================================================================
// FADE OUT
// ==========================================================================

void fadeOutDisplay() {
  for (int br = 100; br >= 0; br -= 8) {
    dma_display->setBrightness8((uint8_t)max(0, br));
    delay(50);
  }
  clearDisplay();
  dma_display->setBrightness8(255);
}
