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

#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <driver/i2s.h>
#include <math.h>

using namespace websockets;

// ==========================================================================
// USER CONFIGURATION
// ==========================================================================

#define WIFI_SSID "Fiber"
#define WIFI_PASSWORD "12345678"

#define FALLBACK_SSID "Fiber"
#define FALLBACK_PASSWORD "12345678"

// Backend Host & Port -- UPDATE THIS every time your PC changes network!
// Current IP: 10.63.92.168
#define WS_SERVER_HOST "10.63.92.168"
#define WS_SERVER_PORT 8000
#define WS_SERVER_PATH "/ws/device"
#define WS_SERVER_URL "ws://10.63.92.168:8000/ws/device"
#define DEVICE_ID "gitmusic-01"
#define RECONNECT_DELAY_MS 3000
// 3s ping -- keeps TCP session alive, prevents uvicorn idle-close at 60s
#define PING_INTERVAL_MS 3000

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

#define PANEL_WIDTH 64
#define PANEL_HEIGHT 32
#define PANELS_NUMBER 1

#define R1_PIN 25
#define G1_PIN 26
#define B1_PIN 27
#define R2_PIN 14
#define G2_PIN 12
#define B2_PIN 13
#define A_PIN 23
#define B_PIN 19
#define C_PIN 5
#define D_PIN 17
#define E_PIN -1 // 1/16 scan -- E unused
#define CLK_PIN 16
#define LAT_PIN 4
#define OE_PIN 15

// ==========================================================================
// MAX98357A I2S PIN CONFIGURATION
// ==========================================================================

#define I2S_BCK_PIN 18
#define I2S_WS_PIN 33
#define I2S_DATA_PIN 22
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 44100

// ==========================================================================
// DISPLAY LAYOUT (64x32 HUB75 RGB LED Matrix)
// ==========================================================================

#define USERNAME_ROW_START 0
#define USERNAME_ROW_END 7

// 7x52 Contribution Grid: 52 weeks horizontally (X), 7 days vertically (Y)
// Coordinate mapping: x = gridX + week, y = gridY + day
const int gridX = 6;  // Centered horizontally: (64 - 52) / 2 = 6 (X: 6..57)
const int gridY = 12; // Centered vertically in available area (Y: 12..18)
#define GRAPH_COLS                                                             \
  52 // 52 weeks horizontally (week 0=oldest on left .. week 51=current on
     // right)
#define GRAPH_ROWS 7 // 7 days vertically (day 0=Sunday .. day 6=Saturday)

// ==========================================================================
// AUDIO - Pentatonic scale (from niyamax/gitmusic useAudioEngine.js)
// 2.5-octave scale spanning C4 to D6 for dynamic streak-based pitch ascension
// ==========================================================================

static const float PENTATONIC_NOTES[] = {
    261.63f,  // 0: C4
    293.66f,  // 1: D4
    329.63f,  // 2: E4
    392.00f,  // 3: G4
    440.00f,  // 4: A4
    523.25f,  // 5: C5
    587.33f,  // 6: D5
    659.25f,  // 7: E5
    783.99f,  // 8: G5
    880.00f,  // 9: A5
    1046.50f, // 10: C6
    1174.66f, // 11: D6
};

#define NOTE_DURATION_MS 35

// ==========================================================================
// COLOUR PALETTE (GitHub contribution graph levels 0-4)
// ==========================================================================

struct RGB {
  uint8_t r, g, b;
};

static const RGB LEVEL_COLORS[5] = {
    {8, 14, 10},      // level 0 -- clearly visible crisp matrix background dot
    {0, 240, 60},     // level 1 -- maximum vibrant electric green
    {20, 255, 80},    // level 2 -- intensely bright emerald green
    {80, 255, 120},   // level 3 -- brilliant luminous neon green
    {200, 255, 230},  // level 4 -- maximum ultra-brilliant diamond mint
};

static const RGB COLOR_USERNAME = {255, 255, 255}; // Pure bright white username per spec
static const RGB COLOR_BG = {0, 0, 0};

// ==========================================================================
// GLOBALS
// ==========================================================================

MatrixPanel_I2S_DMA *dma_display = nullptr;
WebsocketsClient wsClient;

bool wsConnected = false;
unsigned long lastPing = 0;
unsigned long lastReconnectAttempt = 0;

struct GitMusicData {
  char username[64] = "";
  int currentStreak = 0;
  int longestStreak = 0;
  int today = 0;
  int todayRow = 6; // day row in week 51 for today (0=Sun..6=Sat)
  int totalContribs = 0;
  int weeklyContribs = 0;
  int monthlyContribs = 0;
  uint8_t levels[52][7]; // contribution levels grid
  bool musicEnabled = false;
  int musicPattern = 0;
  float musicIntensity = 0.5f;
  bool sessionActive = false;
} gmData;

// Scrolling text state
struct ScrollState {
  int textW = 0;
  int scrollX = 0;
  bool active = false;
  unsigned long lastTick = 0;
  const int tickMs = 55;
} scroll;

// Display mode
enum DisplayMode { MODE_IDLE, MODE_SWEEP, MODE_STATS, MODE_SESSION_END };
DisplayMode displayMode = MODE_IDLE;

// Sweep animation state
int sweepCol = 0;
int sweepRunningStreak = 0;
unsigned long sweepLastMs = 0;
#define SWEEP_COL_DELAY_MS 45 // ms between column reveals

// Idle animation state
unsigned long idleLastMs = 0;
int idleHue = 0;

// ==========================================================================
// SYNCHRONIZED PIANO & LED TIMELINE STATE
// ==========================================================================

enum MusicPlaybackState {
  STATE_IDLE,
  STATE_PREPARING,
  STATE_READY,
  STATE_PLAYING,
  STATE_STOPPING
};
volatile MusicPlaybackState playbackState = STATE_IDLE;

struct MusicalEvent {
  uint32_t timeMs;
  uint8_t week;
  uint8_t day;
  uint8_t dayMask; // 7-bit mask of selected contribution days highlighted
                   // simultaneously
  uint8_t velocity;
};

#define MAX_TIMELINE_EVENTS 256
MusicalEvent timelineEvents[MAX_TIMELINE_EVENTS];
int timelineEventCount = 0;
uint32_t compositionDurationMs = 29538;
unsigned long playbackT0 = 0;
int currentTimelineIdx = 0;
int activeHighlightWeek = -1;
int activeHighlightDay = -1;
uint8_t activeHighlightMask = 0;
unsigned long activeHighlightEndMs = 0;
String currentAudioUrl = "";
TaskHandle_t audioTaskHandle = NULL;

// ==========================================================================
// FORWARD DECLARATIONS
// ==========================================================================

void connectWifi();
void connectWebSocket();
void audioPlayerTask(void *pvParameters);
void tickLEDTimeline();
void stopSynchronizedMusic();
void handleWebSocketMessage(WebsocketsMessage msg);
void handleWebSocketEvent(WebsocketsEvent event, String data);
#if ARDUINOJSON_VERSION_MAJOR >= 7
void parseGitHubUpdate(JsonDocument &doc);
#else
void parseGitHubUpdate(DynamicJsonDocument &doc);
#endif
void printStats();
void sendRegistration();
void sendPing();

void initMatrix();
void initI2S();
void clearDisplay();
void clearUsernameBanner();
void drawUsername(const char *name, bool resetScroll);
void drawStreakGraph(bool fullReveal);
void drawStreakCell(int weekCol, int dayRow, uint8_t level, bool flash,
                    float streakIntensity = 0.0f);
void startSweepAnimation();
void tickSweepAnimation();
void celebrateStreakReveal();
void tickStatsAnimation();
void tickIdleAnimation();
void tickScrollText();
void fadeOutDisplay();

void playNote(float freq, int durationMs, float intensity = 1.0f);
void playStreakChime(int streak);
void playSessionEndTone();
void playIdlePulse();

void startMusicSequencer();
void stopMusicSequencer();
void tickMusicSequencer();
void prepareMusicStep(int step);

float levelToFreq(uint8_t level);
float streakToFreq(uint8_t level, int currentStreak);
uint16_t rgb888to565(RGB c);
RGB hsv2rgb(float h, float s, float v);
int getTextPixelWidth(const char *str);
void drawChar3x5(int x, int y, char c, RGB color);
void drawText3x5(int x, int y, const char *str, RGB color);
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
  Serial.printf("[4/4] BACKEND : Connecting to WebSocket at ws://%s:%d%s ...\n",
                WS_SERVER_HOST, WS_SERVER_PORT, WS_SERVER_PATH);
  wsClient.onMessage(handleWebSocketMessage);
  wsClient.onEvent(handleWebSocketEvent);
  connectWebSocket();

  // [5/5] AUDIO STREAMING TASK (Core 0 background worker)
  xTaskCreatePinnedToCore(audioPlayerTask, "AudioTask", 8192, NULL, 5,
                          &audioTaskHandle, 0);
  Serial.println(
      "[5/5] AUDIO TASK: Spawned background stream player on Core 0");

  Serial.println("======================================================");
  Serial.println("  STATUS: SYSTEM INITIALIZED & READY");
  Serial.println("======================================================");
}

// ==========================================================================
// LOOP
// ==========================================================================

void loop() {
  wsClient.poll();
  tickLEDTimeline();

  unsigned long now = millis();

  // WiFi guard: reconnect gracefully without aborting DHCP handshake.
  // 18-second window is intentional: WPA2 + DHCP can take 5-12s on a
  // congested access point.  Calling WiFi.disconnect() too early was the
  // original root cause of the recurring disconnect bug.
  if (WiFi.status() != WL_CONNECTED) {
    wsConnected = false;
    WiFi.setSleep(false); // Re-assert every loop -- some BSPs reset this flag
    static unsigned long lastWifiRetry = 0;
    if (now - lastWifiRetry >= 18000) { // 18 s grace for DHCP
      lastWifiRetry = now;
      Serial.println("[WiFi] Re-attempting WiFi connection...");
      WiFi.reconnect();
    }
  }

  if (wsConnected && (now - lastPing >= PING_INTERVAL_MS)) {
    sendPing();
    lastPing = now;
  }

  if (!wsConnected && (WiFi.status() == WL_CONNECTED) &&
      (now - lastReconnectAttempt >= RECONNECT_DELAY_MS)) {
    lastReconnectAttempt = now;
    Serial.println("[WS] Connecting to backend server...");
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
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Disable WiFi modem sleep to eliminate latency & disconnects
  WiFi.setAutoReconnect(true); // Let ESP32 auto-reconnect automatically
  WiFi.persistent(true);
  WiFi.setTxPower(WIFI_POWER_17dBm); // Stable transmission power to prevent brownout current spikes

  const char *ssids[] = {WIFI_SSID, FALLBACK_SSID};
  const char *passs[] = {WIFI_PASSWORD, FALLBACK_PASSWORD};

  for (int attempt = 0; attempt < 2 && WiFi.status() != WL_CONNECTED;
       attempt++) {
    const char *curSsid = ssids[attempt];
    const char *curPass = passs[attempt];
    if (!curSsid || strlen(curSsid) == 0)
      continue;

    Serial.printf("                Connecting to SSID: \"%s\" ", curSsid);
    if (dma_display) {
      clearDisplay();
      drawText3x5(8, 13, "WIFI...", {0, 140, 220});
    }
    WiFi.begin(curSsid, curPass);
    unsigned long start = millis();
    // 25s timeout for thorough WPA2 and DHCP negotiation
    while (WiFi.status() != WL_CONNECTED && (millis() - start < 25000)) {
      delay(400);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(" CONNECTED!");
      break;
    } else {
      Serial.println(" Timeout.");
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(
        "\n[WiFi] Could not connect to primary or fallback network.");
    Serial.println("[WiFi] Retrying primary network in background...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    return;
  }

  Serial.println("                ✔ WiFi Connected Successfully!");
  Serial.printf("                - IP Address : %s\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("                - Gateway    : %s\n",
                WiFi.gatewayIP().toString().c_str());
  Serial.printf("                - Subnet Mask: %s\n",
                WiFi.subnetMask().toString().c_str());
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
    return;
  }

  // Only close if previously connected to avoid client socket lockup
  if (wsConnected) {
    wsClient.close();
    wsConnected = false;
  }

  // Parse host, port, and path reliably from WS_SERVER_URL or WS_SERVER_HOST
  String host = WS_SERVER_HOST;
  int port = WS_SERVER_PORT;
  String path = WS_SERVER_PATH;

  // Extract from WS_SERVER_URL if provided, stripping any accidental extra
  // slashes
  String raw = WS_SERVER_URL;
  if (raw.startsWith("ws://"))
    raw = raw.substring(5);
  else if (raw.startsWith("wss://"))
    raw = raw.substring(6);
  else if (raw.startsWith("http://"))
    raw = raw.substring(7);
  else if (raw.startsWith("https://"))
    raw = raw.substring(8);

  while (raw.startsWith("/"))
    raw = raw.substring(1);

  if (raw.length() > 0) {
    int slashIdx = raw.indexOf('/');
    String hostPort = (slashIdx >= 0) ? raw.substring(0, slashIdx) : raw;
    path = (slashIdx >= 0) ? raw.substring(slashIdx) : WS_SERVER_PATH;

    int colonIdx = hostPort.indexOf(':');
    if (colonIdx >= 0) {
      host = hostPort.substring(0, colonIdx);
      port = hostPort.substring(colonIdx + 1).toInt();
      if (port <= 0)
        port = 8000;
    } else {
      host = hostPort;
    }
  }

  Serial.printf(
      "                Connecting to Host: %s, Port: %d, Path: %s ...\n",
      host.c_str(), port, path.c_str());

  // Connect directly using host, port, and path (never fails due to URL scheme
  // bugs)
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
    Serial.println("                1. Ensure backend is running: uvicorn "
                   "main:app --host 0.0.0.0 --port 8000");
    Serial.printf(
        "                2. Ensure host IP is correct (your computer is %s)\n",
        host.c_str());
    Serial.println("                3. Ensure both ESP32 and PC are on the "
                   "same Wi-Fi network.");
  }
}

void sendRegistration() {
  static JsonDocument regDoc;
  regDoc.clear();
  regDoc["type"] = "device_register";
  regDoc["device_id"] = DEVICE_ID;
  char buf[128];
  serializeJson(regDoc, buf);
  wsClient.send(buf);
  Serial.printf("[Device] Sent registration request (ID: '%s')\n", DEVICE_ID);
}

void sendPing() {
  wsClient
      .ping(); // Send RFC 6455 Ping frame to keep connection continuously alive
  static JsonDocument pingDoc;
  pingDoc.clear();
  pingDoc["type"] = "ping";
  char buf[32];
  serializeJson(pingDoc, buf);
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
    stopMusicSequencer();
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
  // Use static JsonDocument on the heap to prevent stack overflow on Core 1
  static JsonDocument doc;
  doc.clear();
  DeserializationError err = deserializeJson(doc, msg.data());
  if (err) {
    Serial.printf("[JSON] Deserialization error: %s\n", err.c_str());
    return;
  }

  const char *type = doc["type"] | "";

  if (strcmp(type, "device_registered") == 0) {
    Serial.println("[Device] ✔ Successfully registered with backend as active "
                   "display device!");

  } else if (strcmp(type, "github_update") == 0) {
    Serial.printf("[Session] ✔ Received GitHub update for user: '%s'\n",
                  doc["username"] | "");
    parseGitHubUpdate(doc);
    printStats();
    startSweepAnimation(); // Runs special column reveal animation, streak
                           // chimes & celebration sparkle!
    Serial.println("[Display] Started column reveal sweep animation before "
                   "showing streak.");

  } else if (strcmp(type, "session_end") == 0) {
    Serial.println("[Session] Session ended by user/backend.");
    stopSynchronizedMusic();
    clearDisplay();
    drawText3x5(4, 13, "GITMUSIC", {70, 70, 70});
    memset(&gmData, 0, sizeof(gmData));
    gmData.sessionActive = false;
    displayMode = MODE_IDLE;

  } else if (strcmp(type, "prepare_music") == 0) {
    Serial.println("[Music] Received prepare_music command from backend");
    playbackState = STATE_PREPARING;

    String audioUrl = doc["audio_url"] | "";
    if (audioUrl.indexOf("localhost") >= 0) {
      audioUrl.replace("localhost", WS_SERVER_HOST);
    }
    if (audioUrl.indexOf("127.0.0.1") >= 0) {
      audioUrl.replace("127.0.0.1", WS_SERVER_HOST);
    }
    currentAudioUrl = audioUrl;
    compositionDurationMs = doc["duration_ms"] | 29538;

    timelineEventCount = 0;
    if (doc.containsKey("timeline")) {
      JsonArray arr = doc["timeline"].as<JsonArray>();
      for (JsonObject ev : arr) {
        if (timelineEventCount < MAX_TIMELINE_EVENTS) {
          uint8_t w = ev["week"] | 0;
          timelineEvents[timelineEventCount].timeMs = ev["time"] | 0;
          timelineEvents[timelineEventCount].week = w;
          uint8_t d = ev["day"] | 0;
          timelineEvents[timelineEventCount].day = d;
          uint8_t mask = ev["day_mask"] | 0;
          if (mask == 0)
            mask = (1 << d);

          // STRICT SAFETY GUARD: Filter out any bits for days without contributions!
          uint8_t cleanMask = 0;
          for (int bit = 0; bit < 7; bit++) {
            if ((mask & (1 << bit)) && gmData.levels[w][bit] > 0) {
              cleanMask |= (1 << bit);
            }
          }
          if (cleanMask == 0 && gmData.levels[w][d] > 0) {
            cleanMask = (1 << d);
          }
          timelineEvents[timelineEventCount].dayMask = cleanMask;
          timelineEvents[timelineEventCount].velocity = ev["velocity"] | 70;
          timelineEventCount++;
        }
      }
    }
    Serial.printf("[Music] Parsed %d timeline events (duration: %u ms)\n",
                  timelineEventCount, compositionDurationMs);
    Serial.printf("[Music] Audio stream URL: %s\n", currentAudioUrl.c_str());

    playbackState = STATE_READY;
    static JsonDocument resp;
    resp.clear();
    resp["type"] = "music_ready";
    char respBuf[64];
    serializeJson(resp, respBuf);
    wsClient.send(respBuf);
    Serial.println("[Music] ✔ Audio primed. Sent music_ready to backend.");

  } else if (strcmp(type, "music_start") == 0 || strcmp(type, "play") == 0) {
    if (doc.containsKey("audio_url")) {
      const char *u = doc["audio_url"] | "";
      if (strlen(u) > 0) {
        String aUrl = u;
        if (aUrl.indexOf("localhost") >= 0)
          aUrl.replace("localhost", WS_SERVER_HOST);
        if (aUrl.indexOf("127.0.0.1") >= 0)
          aUrl.replace("127.0.0.1", WS_SERVER_HOST);
        currentAudioUrl = aUrl;
      }
    }
    Serial.println("[Music] ▶ START command received: launching audio stream & "
                   "LED timeline!");
    playbackT0 = millis();
    currentTimelineIdx = 0;
    activeHighlightWeek = -1;
    activeHighlightDay = -1;
    activeHighlightMask = 0;
    playbackState = STATE_PLAYING;
    displayMode = MODE_STATS; // Display the authentic contribution grid
    drawStreakGraph(true);

  } else if (strcmp(type, "music_stop") == 0 || strcmp(type, "stop") == 0) {
    Serial.println("[Music] ⏹ STOP command received: silencing audio & "
                   "restoring idle LEDs");
    stopSynchronizedMusic();

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
void parseGitHubUpdate(JsonDocument &doc) {
#else
void parseGitHubUpdate(DynamicJsonDocument &doc) {
#endif
  strlcpy(gmData.username, doc["username"] | "", sizeof(gmData.username));
  gmData.currentStreak = doc["current_streak"] | 0;
  gmData.longestStreak = doc["longest_streak"] | 0;
  gmData.today = doc["today"] | 0;
  gmData.todayRow = doc["today_row"] | 6;
  gmData.totalContribs = doc["total_contributions"] | 0;
  gmData.weeklyContribs = doc["weekly_contributions"] | 0;
  gmData.monthlyContribs = doc["monthly_contributions"] | 0;
  gmData.sessionActive = true;

  // ── Parse REAL contribution levels from backend ─────────────────────────
  // Preferred: levels_str (compact 364 chars '0'..'4', zero-allocation, immune
  // to truncation)
  memset(gmData.levels, 0, sizeof(gmData.levels));
  if (doc.containsKey("levels_str")) {
    const char *str = doc["levels_str"].as<const char *>();
    if (str) {
      int len = strlen(str);
      int idx = 0;
      for (int col = 0; col < 52 && idx < len; col++) {
        for (int row = 0; row < 7 && idx < len; row++, idx++) {
          uint8_t v = (uint8_t)(str[idx] - '0');
          gmData.levels[col][row] = (v > 4) ? 4 : v;
        }
      }
      Serial.printf("[Levels] Parsed %d contribution cells from levels_str.\n",
                    idx);
    }
  } else if (doc.containsKey("levels")) {
    JsonArray arr = doc["levels"].as<JsonArray>();
    int idx = 0;
    for (int col = 0; col < 52 && idx < (int)arr.size(); col++) {
      for (int row = 0; row < 7 && idx < (int)arr.size(); row++, idx++) {
        uint8_t v = arr[idx] | 0;
        gmData.levels[col][row] = (v > 4) ? 4 : v;
      }
    }
    Serial.printf("[Levels] Parsed %d real contribution cells from backend.\n",
                  idx);
  } else {
    // Fallback: all zeros (no data) – clearly visible as empty grid
    Serial.println(
        "[Levels] WARNING: No 'levels' array in payload – grid will be empty.");
  }

  if (doc.containsKey("music")) {
    JsonObject music = doc["music"];
    gmData.musicEnabled = music["enabled"] | false;
    gmData.musicPattern = music["pattern"] | 0;
    gmData.musicIntensity = music["intensity"] | 0.5f;
  }
}

// fillLevelsFromStreak() is retired.
// Real data is now parsed directly in parseGitHubUpdate() from backend 'levels'
// array.
void fillLevelsFromStreak() { /* no-op – kept for linker compat only */ }

void printStats() {
  Serial.println();
  Serial.println("+----------------------------------+");
  Serial.printf("|  User     : %-20s|\n", gmData.username);
  Serial.printf("|  Streak   : %-4d days            |\n", gmData.currentStreak);
  Serial.printf("|  Longest  : %-4d days            |\n", gmData.longestStreak);
  Serial.printf("|  Today    : %-4d commits         |\n", gmData.today);
  Serial.printf("|  Total    : %-6d              |\n", gmData.totalContribs);
  Serial.printf("|  Weekly   : %-4d                 |\n",
                gmData.weeklyContribs);
  Serial.printf("|  Monthly  : %-4d                 |\n",
                gmData.monthlyContribs);
  Serial.println("+----------------------------------+");
}

// ==========================================================================
// MATRIX INIT
// ==========================================================================

void initMatrix() {
  HUB75_I2S_CFG mxconfig(PANEL_WIDTH, PANEL_HEIGHT, PANELS_NUMBER);
  mxconfig.gpio.r1 = R1_PIN;
  mxconfig.gpio.g1 = G1_PIN;
  mxconfig.gpio.b1 = B1_PIN;
  mxconfig.gpio.r2 = R2_PIN;
  mxconfig.gpio.g2 = G2_PIN;
  mxconfig.gpio.b2 = B2_PIN;
  mxconfig.gpio.a = A_PIN;
  mxconfig.gpio.b = B_PIN;
  mxconfig.gpio.c = C_PIN;
  mxconfig.gpio.d = D_PIN;
  mxconfig.gpio.e = E_PIN;
  mxconfig.gpio.clk = CLK_PIN;
  mxconfig.gpio.lat = LAT_PIN;
  mxconfig.gpio.oe = OE_PIN;
  mxconfig.clkphase = false;   // tested value
  mxconfig.latch_blanking = 2; // tested value

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  if (!dma_display->begin()) {
    Serial.println("[Matrix] ERROR: DMA init failed!");
    return;
  }
  // High radiant brightness with clean power headroom to prevent WiFi brownouts
  dma_display->setBrightness8(210);
  dma_display->clearScreen();
  Serial.println("[Matrix] HUB75 64x32 initialized.");
}

// ==========================================================================
// I2S AUDIO INIT (MAX98357A)
// ==========================================================================

void initI2S() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_FORMAT_DEFAULT;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 12;
  cfg.dma_buf_len = 512; // 512 samples @ 44.1kHz = 11.6ms per buffer, 139ms total headroom
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_BCK_PIN;
  pins.ws_io_num = I2S_WS_PIN;
  pins.data_out_num = I2S_DATA_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S] Install error: %d\n", err);
    return;
  }
  err = i2s_set_pin(I2S_PORT, &pins);
  if (err != ESP_OK) {
    Serial.printf("[I2S] Pin error: %d\n", err);
    return;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("[I2S] MAX98357A initialized (BCK=18, LRCK=33, DIN=22).");
}

// ==========================================================================
// AUDIO GENERATION
// ==========================================================================

void playNote(float freq, int durationMs, float intensity) {
  // Do NOT interfere with the synchronized WAV stream on Core 0
  if (playbackState == STATE_PLAYING)
    return;
  if (freq <= 0.0f || durationMs <= 0)
    return;

  const int totalSamples = (SAMPLE_RATE * durationMs) / 1000;

  // ===== ABSOLUTE MAXIMUM VOLUME =====
  // Full int16 range = 32767 DAC ceiling, no floor.
  // 4-harmonic piano model: weightings sum to 2.02, so we normalize to keep
  // the total mix peak exactly at 32767 -- maximum loudness, zero distortion.
  const float masterVol = 32767.0f;
  const float mixNorm   = 1.0f / 2.02f;  // normalizer for 4-harmonic mix
  const float amp0 = masterVol * mixNorm;          // fundamental
  const float amp1 = masterVol * mixNorm * 0.60f;  // 2nd harmonic (piano brightness)
  const float amp2 = masterVol * mixNorm * 0.30f;  // 3rd harmonic (body/warmth)
  const float amp3 = masterVol * mixNorm * 0.12f;  // 4th harmonic (attack sparkle)
  const float twoPiF  = 2.0f * PI * freq;
  const float twoPiF2 = twoPiF * 2.0f;
  const float twoPiF3 = twoPiF * 3.0f;
  const float twoPiF4 = twoPiF * 4.0f;

  const int CHUNK = 128;
  int16_t buf[CHUNK];
  int written = 0;
  size_t bytesOut;

  while (written < totalSamples) {
    int chunk = min(CHUNK, totalSamples - written);
    for (int i = 0; i < chunk; i++) {
      float t    = (float)(written + i) / (float)SAMPLE_RATE;
      float frac = (float)(written + i) / (float)totalSamples;
      // ADSR: punchy 6ms attack, slight decay to 0.90 sustain, clean release
      float env;
      if      (frac < 0.06f) env = frac / 0.06f;
      else if (frac < 0.20f) env = 1.0f - 0.10f * ((frac - 0.06f) / 0.14f);
      else if (frac > 0.85f) env = 0.90f * (1.0f - frac) / 0.15f;
      else                   env = 0.90f;
      float sample = env * (
          amp0 * sinf(twoPiF  * t) +
          amp1 * sinf(twoPiF2 * t) +
          amp2 * sinf(twoPiF3 * t) +
          amp3 * sinf(twoPiF4 * t)
      );
      // Hard-clip safety guard (should never trigger with correct normalization)
      if (sample >  32767.0f) sample =  32767.0f;
      if (sample < -32767.0f) sample = -32767.0f;
      buf[i] = (int16_t)sample;
    }
    i2s_write(I2S_PORT, buf, chunk * sizeof(int16_t), &bytesOut, portMAX_DELAY);
    written += chunk;
    wsClient.poll(); // Keep WebSocket alive during note synthesis
  }
}

// Pitch ascends across 2.5 octaves as streak builds up!
float streakToFreq(uint8_t level, int currentStreak) {
  if (level == 0)
    return 0.0f;
  int baseNote = (int)level - 1; // 0..3 (C4, D4, E4, G4)
  // Every 2 streak days/weeks shifts note up the pentatonic scale
  int streakBonus = min(currentStreak / 2, 7);
  int noteIndex = min(baseNote + streakBonus, 11);
  return PENTATONIC_NOTES[noteIndex];
}

float levelToFreq(uint8_t level) { return streakToFreq(level, 0); }

// ==========================================================================
// SYNCHRONIZED PIANO AUDIO & LED TIMELINE ENGINE
//
// 1. Audio Task (Core 0):
//    Streams 16-bit 44.1kHz mono PCM WAV from backend via HTTP chunking
//    Feeds directly into MAX98357A via I2S DMA. Zero lag, non-blocking for
//    matrix.
//
// 2. LED Timeline Engine (Core 1):
//    Executes authoritative musical timeline events locally using millis().
//    Highlights exact (week, day) cell matching the piano note with brilliant
//    bloom. Wi-Fi jitter does not affect LED-to-audio sync because timeline is
//    local!
//
// 3. Looping:
//    Natural smooth loop when song reaches end, restarting audio and timeline
//    without memory leaks or task duplication.
// ==========================================================================

void audioPlayerTask(void *pvParameters) {
  uint8_t audioBuffer[1024];
  HTTPClient httpClient;

  while (true) {
    if (playbackState == STATE_PLAYING && currentAudioUrl.length() > 0) {
      Serial.printf("[AudioTask] Streaming realistic piano audio from: %s\n",
                    currentAudioUrl.c_str());
      httpClient.begin(currentAudioUrl);
      httpClient.setTimeout(5000);
      int code = httpClient.GET();

      if (code == HTTP_CODE_OK) {
        int contentLength = httpClient.getSize();
        int dataToRead =
            (contentLength > 44) ? (contentLength - 44) : contentLength;
        int totalDataRead = 0;

        WiFiClient *stream = httpClient.getStreamPtr();

        // Skip 44-byte standard RIFF WAV header
        uint8_t header[44];
        int headerRead = 0;
        unsigned long headerStart = millis();
        while (headerRead < 44 && (millis() - headerStart < 2000) &&
               stream->connected()) {
          if (stream->available()) {
            header[headerRead++] = stream->read();
          } else {
            vTaskDelay(pdMS_TO_TICKS(1));
          }
        }

        Serial.printf("[AudioTask] ▶ Playing 16-bit 44.1kHz mono piano PCM "
                      "stream (%d bytes)...\n",
                      dataToRead);

        while (playbackState == STATE_PLAYING) {
          int avail = stream->available();
          if (avail > 0) {
            int toRead = min((int)sizeof(audioBuffer), avail);
            if (dataToRead > 0) {
              toRead = min(toRead, dataToRead - totalDataRead);
            }
            int bytesRead = stream->readBytes((char *)audioBuffer, toRead);
            if (bytesRead > 0) {
              totalDataRead += bytesRead;
              size_t bytesWritten = 0;
              i2s_write(I2S_PORT, audioBuffer, bytesRead, &bytesWritten,
                        portMAX_DELAY);
            }
            if (dataToRead > 0 && totalDataRead >= dataToRead) {
              Serial.println(
                  "[AudioTask] Finished playing full WAV audio file");
              break;
            }
          } else {
            if (!stream->connected()) {
              break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
          }
        }

        i2s_zero_dma_buffer(I2S_PORT);
      } else {
        Serial.printf("[AudioTask] HTTP GET failed, code: %d\n", code);
        vTaskDelay(pdMS_TO_TICKS(300));
      }
      httpClient.end();

      // If still in PLAYING state, seamlessly loop playback
      if (playbackState == STATE_PLAYING) {
        playbackT0 = millis();
        currentTimelineIdx = 0;
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

void clearActiveHighlights() {
  if (activeHighlightWeek >= 0 && activeHighlightMask > 0) {
    for (int d = 0; d < 7; d++) {
      if (activeHighlightMask & (1 << d)) {
        drawStreakCell(activeHighlightWeek, d,
                       gmData.levels[activeHighlightWeek][d], false);
      }
    }
    activeHighlightWeek = -1;
    activeHighlightMask = 0;
  }
}

void tickLEDTimeline() {
  if (playbackState != STATE_PLAYING || timelineEventCount == 0)
    return;

  unsigned long now = millis();
  unsigned long elapsedMs = now - playbackT0;

  // Natural smooth loop handling
  if (elapsedMs >= compositionDurationMs) {
    playbackT0 = now;
    elapsedMs = 0;
    currentTimelineIdx = 0;
    clearActiveHighlights();
    Serial.println("[LED Timeline] Looping composition to beginning");

    wsClient.send("{\"type\":\"music_loop\"}");
  }

  // Fade active highlight back to authentic green level color
  if (activeHighlightWeek >= 0 && now >= activeHighlightEndMs) {
    clearActiveHighlights();
  }

  // Process timeline events matching current elapsed timestamp
  while (currentTimelineIdx < timelineEventCount &&
         elapsedMs >= timelineEvents[currentTimelineIdx].timeMs) {
    MusicalEvent &ev = timelineEvents[currentTimelineIdx];

    // Clear previous week highlights
    clearActiveHighlights();

    activeHighlightWeek = ev.week;
    activeHighlightMask = ev.dayMask;
    activeHighlightEndMs = now + 420; // Visible bloom duration

    int x = gridX + ev.week;

    // Draw radiant highlight on ALL selected cells in this week SIMULTANEOUSLY
    // for this week's single tone!
    for (int d = 0; d < 7; d++) {
      if (ev.dayMask & (1 << d)) {
        // STRICT SAFETY GUARD: Never highlight a day that has 0 contributions!
        if (gmData.levels[ev.week][d] == 0)
          continue;

        int y = gridY + d;
        if (x >= 0 && x < PANEL_WIDTH && y >= 0 && y < PANEL_HEIGHT) {
          if (ev.velocity > 85) {
            dma_display->drawPixelRGB888(x, y, 255, 255,
                                         255); // Brilliant white peak
          } else {
            dma_display->drawPixelRGB888(x, y, 160, 255, 230); // Radiant mint
          }
        }
      }
    }

    currentTimelineIdx++;
  }
}

void stopSynchronizedMusic() {
  playbackState = STATE_IDLE;
  i2s_zero_dma_buffer(I2S_PORT);

  clearActiveHighlights();

  Serial.println("[Music] ⏹ Synchronized playback stopped and reset.");
}

// Retain legacy helpers for backwards compatibility
void startMusicSequencer() {
  playbackT0 = millis();
  playbackState = STATE_PLAYING;
}
void stopMusicSequencer() { stopSynchronizedMusic(); }
void tickMusicSequencer() { tickLEDTimeline(); }

// Ascending arpeggio chime on session start, scaling intensity & octave with
// streak
void playStreakChime(int streak) {
  int notes = 3 + min(streak / 5, 5);
  for (int i = 0; i < notes; i++) {
    float intensity = min(1.0f, 0.4f + (float)i * 0.15f);
    int noteIdx = min(i * 2, 11);
    playNote(PENTATONIC_NOTES[noteIdx], 140, intensity);
    delay(55);
  }
}

// Descending farewell when session ends
void playSessionEndTone() {
  for (int i = 3; i >= 0; i--) {
    playNote(PENTATONIC_NOTES[i], 180, 0.7f);
    delay(100);
  }
}

// Soft ambient pulse in idle mode
void playIdlePulse() { playNote(PENTATONIC_NOTES[0] * 0.5f, 120, 0.3f); }

// ==========================================================================
// FONT: Compact 3x5 pixel bitmap font (ASCII 32-90)
// Supports A-Z, 0-9, symbols needed for GitHub usernames.
// Each char = 3 columns, each column = 5 bits (bit0=top, bit4=bottom).
// ==========================================================================

static const uint8_t FONT_DATA[59][3] = {
    {0x00, 0x00, 0x00}, // ' '  32
    {0x00, 0x17, 0x00}, // '!'  33
    {0x03, 0x00, 0x03}, // '"'  34
    {0x1F, 0x0A, 0x1F}, // '#'  35
    {0x16, 0x1F, 0x0D}, // '$'  36
    {0x13, 0x08, 0x19}, // '%'  37
    {0x0E, 0x15, 0x0A}, // '&'  38
    {0x00, 0x03, 0x00}, // '\'' 39
    {0x00, 0x0E, 0x11}, // '('  40
    {0x11, 0x0E, 0x00}, // ')'  41
    {0x0A, 0x04, 0x0A}, // '*'  42
    {0x04, 0x0E, 0x04}, // '+'  43
    {0x10, 0x08, 0x00}, // ','  44
    {0x04, 0x04, 0x04}, // '-'  45
    {0x00, 0x10, 0x00}, // '.'  46
    {0x10, 0x08, 0x04}, // '/'  47
    // 0-9 (48-57)
    {0x0E, 0x11, 0x0E}, // '0'
    {0x12, 0x1F, 0x10}, // '1'
    {0x19, 0x15, 0x12}, // '2'
    {0x11, 0x15, 0x0E}, // '3'
    {0x07, 0x04, 0x1F}, // '4'
    {0x17, 0x15, 0x09}, // '5'
    {0x0E, 0x15, 0x08}, // '6'
    {0x01, 0x19, 0x07}, // '7'
    {0x0A, 0x15, 0x0A}, // '8'
    {0x02, 0x15, 0x0E}, // '9'
    // :;<=>?@  (58-64)
    {0x00, 0x0A, 0x00}, // ':'
    {0x10, 0x0A, 0x00}, // ';'
    {0x04, 0x0A, 0x11}, // '<'
    {0x0A, 0x0A, 0x0A}, // '='
    {0x11, 0x0A, 0x04}, // '>'
    {0x01, 0x15, 0x02}, // '?'
    {0x0E, 0x15, 0x1D}, // '@'
    // A-Z  (65-90)
    {0x1E, 0x05, 0x1E}, // 'A'
    {0x1F, 0x15, 0x0A}, // 'B'
    {0x0E, 0x11, 0x11}, // 'C'
    {0x1F, 0x11, 0x0E}, // 'D'
    {0x1F, 0x15, 0x11}, // 'E'
    {0x1F, 0x05, 0x01}, // 'F'
    {0x0E, 0x15, 0x1C}, // 'G'
    {0x1F, 0x04, 0x1F}, // 'H'
    {0x11, 0x1F, 0x11}, // 'I'
    {0x08, 0x11, 0x0F}, // 'J'
    {0x1F, 0x04, 0x1B}, // 'K'
    {0x1F, 0x10, 0x10}, // 'L'
    {0x1F, 0x02, 0x1F}, // 'M' - two peaks with center dip
    {0x1F, 0x01,
     0x1E}, // 'N' - clear arch with top-right opening, distinct from 'M'
    {0x0E, 0x11, 0x0E}, // 'O'
    {0x1F, 0x05, 0x02}, // 'P'
    {0x0E, 0x19, 0x1E}, // 'Q'
    {0x1F, 0x05, 0x1A}, // 'R'
    {0x12, 0x15, 0x09}, // 'S'
    {0x01, 0x1F, 0x01}, // 'T'
    {0x0F, 0x10, 0x0F}, // 'U'
    {0x07, 0x18, 0x07}, // 'V'
    {0x1F, 0x0C, 0x1F}, // 'W'
    {0x1B, 0x04, 0x1B}, // 'X'
    {0x03, 0x1C, 0x03}, // 'Y'
    {0x19, 0x15, 0x13}, // 'Z'
};

static const uint8_t GLYPH_UNDERSCORE[3] = {0x10, 0x10, 0x10};

const uint8_t *getGlyph(char c) {
  if (c == '_')
    return GLYPH_UNDERSCORE;
  char uc = (c >= 'a' && c <= 'z') ? c - 32 : c;
  if (uc >= 32 && uc <= 90)
    return FONT_DATA[uc - 32];
  return FONT_DATA[0]; // space
}

void drawChar3x5(int x, int y, char c, RGB color) {
  const uint8_t *g = getGlyph(c);
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

void drawText3x5(int x, int y, const char *str, RGB color) {
  int cx = x;
  for (int i = 0; str[i]; i++) {
    drawChar3x5(cx, y, str[i], color);
    cx += 4;
  }
}

// ── Scaled Font Engine for Bold High-Impact Display ───────────────────────
void drawCharScaled(int x, int y, char c, RGB color, int scaleX, int scaleY) {
  const uint8_t *g = getGlyph(c);
  for (int col = 0; col < 3; col++) {
    for (int row = 0; row < 5; row++) {
      if (g[col] & (1 << row)) {
        for (int dx = 0; dx < scaleX; dx++) {
          for (int dy = 0; dy < scaleY; dy++) {
            int px = x + col * scaleX + dx;
            int py = y + row * scaleY + dy;
            if (px >= 0 && px < PANEL_WIDTH && py >= 0 && py < PANEL_HEIGHT) {
              dma_display->drawPixelRGB888(px, py, color.r, color.g, color.b);
            }
          }
        }
      }
    }
  }
}

void drawTextScaled(int x, int y, const char *str, RGB color, int scaleX, int scaleY, int letterSpacing) {
  int cx = x;
  for (int i = 0; str[i]; i++) {
    drawCharScaled(cx, y, str[i], color, scaleX, scaleY);
    cx += 3 * scaleX + letterSpacing;
  }
}

int getTextPixelWidth(const char *str) {
  int len = strlen(str);
  if (len == 0)
    return 0;
  return len * 4 - 1;
}

// ==========================================================================
// COLOUR UTILITIES
// ==========================================================================

uint16_t rgb888to565(RGB c) { return dma_display->color565(c.r, c.g, c.b); }

RGB hsv2rgb(float h, float s, float v) {
  float r, g, b;
  int i = (int)(h * 6.0f);
  float f = h * 6.0f - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - f * s);
  float t = v * (1.0f - (1.0f - f) * s);
  switch (i % 6) {
  case 0:
    r = v;
    g = t;
    b = p;
    break;
  case 1:
    r = q;
    g = v;
    b = p;
    break;
  case 2:
    r = p;
    g = v;
    b = t;
    break;
  case 3:
    r = p;
    g = q;
    b = v;
    break;
  case 4:
    r = t;
    g = p;
    b = v;
    break;
  default:
    r = v;
    g = p;
    b = q;
    break;
  }
  // Full 100% 255-scale brightness for maximum screen illumination
  return {(uint8_t)(r * 255.0f), (uint8_t)(g * 255.0f), (uint8_t)(b * 255.0f)};
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

void drawUsername(const char *name, bool resetScroll) {
  clearUsernameBanner();
  int textW = getTextPixelWidth(name);
  int yPos = 1; // 1px top padding; text is 5px tall -> rows 1-5

  if (textW <= PANEL_WIDTH) {
    int xPos = (PANEL_WIDTH - textW) / 2;
    drawText3x5(xPos, yPos, name, COLOR_USERNAME);
    scroll.active = false;
  } else {
    scroll.textW = textW;
    scroll.active = true;
    if (resetScroll)
      scroll.scrollX = PANEL_WIDTH + 4;
    drawText3x5(scroll.scrollX, yPos, name, COLOR_USERNAME);
  }
}

void tickScrollText() {
  if (!scroll.active || !gmData.sessionActive)
    return;
  unsigned long now = millis();
  if (now - scroll.lastTick < (unsigned long)scroll.tickMs)
    return;
  scroll.lastTick = now;

  scroll.scrollX--;
  if (scroll.scrollX < -(scroll.textW + 4))
    scroll.scrollX = PANEL_WIDTH + 4;

  clearUsernameBanner();
  drawText3x5(scroll.scrollX, 1, gmData.username, COLOR_USERNAME);
}

// ==========================================================================
// STREAK GRAPH (52 weeks horizontally x 7 days vertically)
// Mapping: x = gridX + week (0..51), y = gridY + day (0..6)
// Exactly 1 LED per cell. No transposing, no rotation, no stretching.
// ==========================================================================

void drawStreakCell(int weekCol, int dayRow, uint8_t level, bool flash,
                    float streakIntensity) {
  int x = gridX + weekCol;
  int y = gridY + dayRow;

  if (x < 0 || x >= PANEL_WIDTH || y < 0 || y >= PANEL_HEIGHT)
    return;

  RGB color;
  if (flash) {
    if (level == 0) {
      color = RGB{15, 20, 25}; // Soft dim baseline when no commits in column
    } else {
      // Dynamic visual intensity pointing out streak:
      // Low streak (si ~ 0): Electric emerald green { 60, 230, 110 }
      // Mid streak (si ~ 0.5): Luminous bright cyan { 90, 255, 210 }
      // High streak (si ~ 1.0): Radiant brilliant diamond gold-white { 255,
      // 255, 220 }
      float si = (streakIntensity < 0.0f)
                     ? 0.0f
                     : ((streakIntensity > 1.0f) ? 1.0f : streakIntensity);
      uint8_t r = (uint8_t)(60 + 195 * si);
      uint8_t g = 255;
      uint8_t b = (uint8_t)(110 + 110 * si);
      color = RGB{r, g, b};
    }
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

// ==========================================================================
// IDLE ANIMATION -- Dynamic Living Cover Screen (v3.0)
//
// Full-screen animated plasma wave background (HSV colour-cycling)
// + Large 2x-scaled "GITMUSIC" title with per-letter rainbow colour flow
// + Divider sine-wave bar
// + 16 bouncing equalizer bars with hue-shifted peaks
// ==========================================================================

unsigned long idleFrameCount = 0;

void tickIdleAnimation() {
  unsigned long now = millis();
  if (now - idleLastMs < 35) // ~28 fps -- smooth but not CPU-hogging
    return;
  idleLastMs = now;
  idleFrameCount++;

  idleHue = (idleHue + 2) % 360;

  // -------------------------------------------------------------------
  // 1. FULL-SCREEN MOVING PLASMA WAVE BACKGROUND
  //    Equation: hue = baseHue + sin(x*freq + phase) + sin(y*freq + phase2)
  //    This creates the classic "lava lamp" plasma motion without division.
  // -------------------------------------------------------------------
  float phase1 = (float)now * 0.0011f;  // horizontal ripple speed
  float phase2 = (float)now * 0.00085f; // vertical ripple speed
  float phase3 = (float)now * 0.0007f;  // diagonal drift speed

  for (int y = 0; y < PANEL_HEIGHT; y++) {
    for (int x = 0; x < PANEL_WIDTH; x++) {
      float plasma =
          sinf((float)x * 0.22f + phase1) +
          sinf((float)y * 0.30f + phase2) +
          sinf(((float)x + (float)y) * 0.18f + phase3);
      // plasma range: -3..+3 -- normalise to 0..1
      float h = fmodf((plasma + 3.0f) / 6.0f + (float)idleHue / 360.0f, 1.0f);
      // Keep saturation high, reduce value for a "deep" background feel
      RGB c = hsv2rgb(h, 1.0f, 0.45f);
      dma_display->drawPixelRGB888(x, y, c.r, c.g, c.b);
    }
  }

  // -------------------------------------------------------------------
  // 2. "GITMUSIC" -- 2x-scaled, per-letter rainbow hue cycling
  //    Letters are at y=3, height=10px so they sit clearly above the
  //    divider bar.
  //    Each letter gets its own hue offset (40 deg apart) so they look
  //    like a flowing rainbow stream moving left-to-right.
  // -------------------------------------------------------------------
  const char *title   = "GITMUSIC";
  int titleLen        = 8;           // G-I-T-M-U-S-I-C
  int charWScaled     = 3 * 2 + 1;  // 3px glyph * 2 + 1 spacing = 7px per char
  int totalTitleW     = titleLen * charWScaled - 1;  // 55 px
  int titleX          = (PANEL_WIDTH - totalTitleW) / 2; // centre at ~4
  int titleY          = 2;          // 2px top margin; glyphs are 5*2=10px tall

  for (int ci = 0; ci < titleLen; ci++) {
    // Hue sweeps: each letter offset by 40 degrees, full cycle every ~5s
    float letterHue = fmodf(
        (float)idleHue / 360.0f + (float)ci * (40.0f / 360.0f),
        1.0f
    );
    // Full saturation + full brightness = pure vivid rainbow
    RGB lc = hsv2rgb(letterHue, 1.0f, 1.0f);
    // Add a subtle pulse beat (1 Hz)
    float beat = (sinf((float)now * 0.00628f) + 1.0f) * 0.5f; // 0..1
    lc.r = (uint8_t)(lc.r * (0.80f + 0.20f * beat));
    lc.g = (uint8_t)(lc.g * (0.80f + 0.20f * beat));
    lc.b = (uint8_t)(lc.b * (0.80f + 0.20f * beat));

    int lx = titleX + ci * charWScaled;
    drawCharScaled(lx, titleY, title[ci], lc, 2, 2);
  }

  // -------------------------------------------------------------------
  // 3. SINE-WAVE DIVIDER LINE at y=15 (below title, above EQ bars)
  // -------------------------------------------------------------------
  for (int x = 0; x < PANEL_WIDTH; x++) {
    float wv = sinf((float)x * 0.20f + (float)now * 0.004f);
    float hd = fmodf((float)idleHue / 360.0f + (float)x / 64.0f, 1.0f);
    RGB wd = hsv2rgb(hd, 1.0f, 0.75f + 0.25f * wv);
    dma_display->drawPixelRGB888(x, 15, wd.r, wd.g, wd.b);
  }

  // -------------------------------------------------------------------
  // 4. 16 BOUNCING EQUALIZER BARS (rows 17 to 31)
  //    Hue of each bar = base hue + bar position offset -> flowing rainbow
  // -------------------------------------------------------------------
  for (int bar = 0; bar < 16; bar++) {
    int bx = 2 + bar * 4;
    // Multi-sine for realistic non-uniform bar movement
    float s1 = sinf((float)now * 0.0060f + (float)bar * 0.55f);
    float s2 = cosf((float)now * 0.0095f + (float)bar * 1.10f);
    float s3 = sinf((float)now * 0.0033f - (float)bar * 0.38f);
    int barHeight = (int)(2.0f + 4.5f * s1 + 3.5f * s2 + 2.0f * s3);
    barHeight = max(1, min(13, barHeight));

    float barBaseHue = fmodf((float)idleHue / 360.0f + (float)bar / 16.0f, 1.0f);

    for (int h = 0; h < barHeight; h++) {
      int by = 31 - h;
      // Hue varies from bar base at bottom to +0.25 (cyan/white) at top
      float hv = fmodf(barBaseHue + (float)h * 0.02f, 1.0f);
      float sat = 1.0f;
      float val = (h >= barHeight - 1) ? 1.0f : 0.85f; // top pixel brighter
      RGB barColor = hsv2rgb(hv, sat, val);
      dma_display->drawPixelRGB888(bx,     by, barColor.r, barColor.g, barColor.b);
      dma_display->drawPixelRGB888(bx + 1, by, barColor.r, barColor.g, barColor.b);
      dma_display->drawPixelRGB888(bx + 2, by, barColor.r, barColor.g, barColor.b);
    }
    // Bright white peak pixel floating 1 above bar top
    int peakY = 31 - min(13, barHeight + 1);
    if (peakY >= 17)
      dma_display->drawPixelRGB888(bx + 1, peakY, 255, 255, 255);
  }
}

// ==========================================================================
// SWEEP ANIMATION & CELEBRATION
//
// Column-by-column reveal of the 52-week contribution graph.
// Directly adapted from niyamax/gitmusic GitSequencer:
//   - activeCol sweeps left to right (oldest to newest week)
//   - Each active column briefly flashes bright cyan-green before settling
//   - Plays a pentatonic note proportional to that week's max contribution
//   level
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
          dma_display->drawPixelRGB888(x, y, 255, 255,
                                       220); // Radiant gold-white sparkle
        }
      }
    }
    delay(50);
    for (int w = startCol; w < 52; w++) {
      for (int d = 0; d < GRAPH_ROWS; d++) {
        drawStreakCell(w, d, gmData.levels[w][d], false);
      }
    }
    delay(35);
  }

  // Grand ascending celebratory streak chord progression (maximum sound)
  playNote(PENTATONIC_NOTES[5], 90, 1.0f); // C5
  delay(30);
  playNote(PENTATONIC_NOTES[7], 110, 1.0f); // E5
  delay(30);
  playNote(PENTATONIC_NOTES[9], 180, 1.0f); // A5
  delay(30);
  playNote(PENTATONIC_NOTES[10], 250, 1.0f); // C6 peak!
}

void startSweepAnimation() {
  sweepCol = 0;
  sweepRunningStreak = 0;
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

  Serial.println(
      "[Sweep] Starting column-reveal sequencer with streak modulation...");
  playStreakChime(gmData.currentStreak);
}

void tickSweepAnimation() {
  unsigned long now = millis();
  if (now - sweepLastMs < SWEEP_COL_DELAY_MS)
    return;
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

  // Find max level in current column & compute running streak
  uint8_t maxLvl = 0;
  for (int d = 0; d < GRAPH_ROWS; d++) {
    if (gmData.levels[sweepCol][d] > maxLvl)
      maxLvl = gmData.levels[sweepCol][d];
  }

  if (maxLvl > 0) {
    sweepRunningStreak++;
  } else {
    sweepRunningStreak = 0; // Streak drops
  }

  // Dynamic streak intensity: increases with streak, drops when streak breaks
  float streakIntensity =
      (sweepRunningStreak > 0)
          ? min(1.0f, 0.25f + (float)sweepRunningStreak * 0.12f)
          : 0.0f;

  // Flash current column: brightness & color point out streak intensity!
  for (int d = 0; d < GRAPH_ROWS; d++) {
    drawStreakCell(sweepCol, d, gmData.levels[sweepCol][d], true,
                   streakIntensity);
  }

  // Sound playing based on streak at MAXIMUM volume:
  // Pitch ascends through scale degrees as streak builds up, drops when streak
  // ends
  if (maxLvl > 0) {
    float freq = streakToFreq(maxLvl, sweepRunningStreak);
    int duration = NOTE_DURATION_MS + min(sweepRunningStreak * 4, 35);
    playNote(freq, duration, streakIntensity);
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
  if (!gmData.sessionActive)
    return;
  // CRITICAL FIX: If synchronized music is playing, do NOT touch display or
  // collide with timeline!
  if (playbackState == STATE_PLAYING)
    return;

  unsigned long now = millis();
  if (now - lastStatsTick < 100)
    return; // 10 fps
  lastStatsTick = now;

  statsPulseStep = (statsPulseStep + 1) % 50; // 5.0 second cycle

  // 1. Gently pulse the "Today" cell if user committed today
  if (gmData.today > 0 && gmData.todayRow >= 0 && gmData.todayRow < 7) {
    float pulse = (sinf((float)statsPulseStep * 0.25f) + 1.0f) * 0.5f;
    uint8_t baseLevel = gmData.levels[51][gmData.todayRow];
    RGB baseColor = LEVEL_COLORS[min((int)baseLevel, 4)];
    uint8_t r = (uint8_t)(baseColor.r + (255 - baseColor.r) * 0.55f * pulse);
    uint8_t g = (uint8_t)(baseColor.g + (255 - baseColor.g) * 0.55f * pulse);
    uint8_t b = (uint8_t)(baseColor.b + (220 - baseColor.b) * 0.55f * pulse);

    int x = gridX + 51;
    int y = gridY + gmData.todayRow;
    dma_display->drawPixelRGB888(x, y, r, g, b);
  }
}

// ==========================================================================
// FADE OUT
// ==========================================================================

void fadeOutDisplay() {
  clearDisplay();
  dma_display->setBrightness8(255);
}
