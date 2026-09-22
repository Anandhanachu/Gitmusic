/*
 * gitmusic.ino
 * ──────────────────────────────────────────────────────────────────────────
 * GitMusic – XIAO ESP32-S3 firmware
 *
 * PURPOSE:
 *   Maintain a persistent WebSocket connection to the GitMusic backend.
 *   On connect, register as "gitmusic-01".
 *   Receive processed GitHub stats via JSON and print them to Serial.
 *
 * HARDWARE:
 *   - Seeed Studio XIAO ESP32-S3
 *   - (future) SSD1306/SH1106 OLED display
 *   - (future) MAX98357A I2S audio amplifier + speaker
 *   - (future) 364 individually addressable WS2812B / APA106 LEDs
 *
 * LIBRARIES REQUIRED (install via Arduino Library Manager):
 *   - ArduinoWebsockets  (Gil Maimon)  → search "ArduinoWebsockets"
 *   - ArduinoJson        (Benoit B.)   → search "ArduinoJson"
 *   - WiFi               (built-in ESP32 core)
 *
 * CONFIGURATION:
 *   Edit the #define values below before flashing.
 *
 * NOTE:
 *   This firmware does NOT contain any GitHub API credentials.
 *   It only receives already-processed JSON data from the backend.
 * ──────────────────────────────────────────────────────────────────────────
 */

#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>

using namespace websockets;

// ── USER CONFIGURATION ─────────────────────────────────────────────────────

// Your Wi-Fi credentials
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// Backend WebSocket URL
// Local development: ws://192.168.x.x:8000/ws/device
// Production:        wss://your-app.onrender.com/ws/device
#define WS_SERVER_URL   "ws://192.168.1.100:8000/ws/device"

// This must match DeviceManager.DEVICE_ID in backend/device_manager.py
#define DEVICE_ID       "gitmusic-01"

// Reconnect interval in milliseconds
#define RECONNECT_DELAY_MS   5000

// Ping interval in milliseconds (keep-alive)
#define PING_INTERVAL_MS     20000

// ── GLOBALS ────────────────────────────────────────────────────────────────

WebsocketsClient wsClient;

bool wsConnected       = false;
unsigned long lastPing = 0;
unsigned long lastReconnectAttempt = 0;

// Latest received stats (initialised to defaults)
struct GitMusicData {
  char username[64]   = "";
  int  currentStreak  = 0;
  int  longestStreak  = 0;
  int  today          = 0;
  int  totalContribs  = 0;
  int  weeklyContribs = 0;
  int  monthlyContribs= 0;
  bool musicEnabled   = false;
  int  musicPattern   = 0;
  float musicIntensity = 0.0f;
  bool sessionActive  = false;
} gmData;

// ── FORWARD DECLARATIONS ───────────────────────────────────────────────────

void connectWifi();
void connectWebSocket();
void handleWebSocketMessage(WebsocketsMessage msg);
void handleWebSocketEvent(WebsocketsEvent event, String data);
void parseGitHubUpdate(JsonDocument& doc);
void printStats();
void sendRegistration();
void sendPing();

// ── SETUP ──────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("╔════════════════════════════╗");
  Serial.println("║   GitMusic – ESP32 Firmware  ║");
  Serial.println("╚════════════════════════════╝");
  Serial.printf("  Device ID : %s\n", DEVICE_ID);
  Serial.printf("  Backend   : %s\n", WS_SERVER_URL);
  Serial.println();

  connectWifi();

  // Register WebSocket callbacks
  wsClient.onMessage(handleWebSocketMessage);
  wsClient.onEvent(handleWebSocketEvent);

  connectWebSocket();
}

// ── LOOP ───────────────────────────────────────────────────────────────────

void loop() {
  // Let the WebSocket client process incoming data
  wsClient.poll();

  unsigned long now = millis();

  // ── Keep-alive ping ──
  if (wsConnected && (now - lastPing >= PING_INTERVAL_MS)) {
    sendPing();
    lastPing = now;
  }

  // ── Reconnect if disconnected ──
  if (!wsConnected && (now - lastReconnectAttempt >= RECONNECT_DELAY_MS)) {
    Serial.println("[WS] Attempting reconnect…");
    lastReconnectAttempt = now;
    connectWebSocket();
  }

  // ── Future: update OLED display ──
  // updateOLED();

  // ── Future: update LEDs ──
  // updateLEDs();

  // ── Future: update audio ──
  // updateAudio();

  delay(10);
}

// ── Wi-Fi ──────────────────────────────────────────────────────────────────

void connectWifi() {
  Serial.printf("[WiFi] Connecting to \"%s\"", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\n[WiFi] Connection timeout – restarting.");
      ESP.restart();
    }
  }

  Serial.println();
  Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
}

// ── WebSocket ──────────────────────────────────────────────────────────────

void connectWebSocket() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WS] No WiFi – cannot connect.");
    return;
  }

  Serial.printf("[WS] Connecting to %s …\n", WS_SERVER_URL);
  bool ok = wsClient.connect(WS_SERVER_URL);

  if (ok) {
    Serial.println("[WS] TCP connection established.");
    // Registration is sent in the onEvent CONNECTED callback
  } else {
    Serial.println("[WS] Connection failed – will retry.");
  }
}

void sendRegistration() {
  StaticJsonDocument<128> doc;
  doc["type"]      = "device_register";
  doc["device_id"] = DEVICE_ID;

  char buf[128];
  serializeJson(doc, buf);
  wsClient.send(buf);
  Serial.printf("[WS] → Sent registration for device '%s'\n", DEVICE_ID);
}

void sendPing() {
  StaticJsonDocument<32> doc;
  doc["type"] = "ping";

  char buf[32];
  serializeJson(doc, buf);
  wsClient.send(buf);
  Serial.println("[WS] → Ping sent.");
}

// ── WebSocket event handler ────────────────────────────────────────────────

void handleWebSocketEvent(WebsocketsEvent event, String data) {
  switch (event) {
    case WebsocketsEvent::ConnectionOpened:
      Serial.println("[WS] ✓ WebSocket connected to backend.");
      wsConnected = true;
      lastPing = millis();
      sendRegistration();
      break;

    case WebsocketsEvent::ConnectionClosed:
      Serial.println("[WS] ✗ WebSocket disconnected.");
      wsConnected = false;
      gmData.sessionActive = false;
      break;

    case WebsocketsEvent::GotPing:
      wsClient.pong();
      break;

    case WebsocketsEvent::GotPong:
      Serial.println("[WS] ← Pong received.");
      break;

    default:
      break;
  }
}

// ── WebSocket message handler ──────────────────────────────────────────────

void handleWebSocketMessage(WebsocketsMessage msg) {
  String payload = msg.data();
  Serial.printf("[WS] ← Received: %s\n", payload.c_str());

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.printf("[JSON] Parse error: %s\n", err.c_str());
    return;
  }

  const char* type = doc["type"] | "";

  if (strcmp(type, "device_registered") == 0) {
    Serial.println("[Device] Registered with backend.");
    Serial.printf("[Device] Status: %s\n", doc["status"] | "unknown");

  } else if (strcmp(type, "github_update") == 0) {
    parseGitHubUpdate(doc);
    printStats();
    // TODO: updateOLED(), updateLEDs(), updateAudio()

  } else if (strcmp(type, "session_end") == 0) {
    Serial.println("[Session] Session ended – clearing display.");
    memset(&gmData, 0, sizeof(gmData));
    gmData.sessionActive = false;
    // TODO: clearOLED(), clearLEDs(), stopAudio()

  } else if (strcmp(type, "pong") == 0) {
    // Server acknowledged our ping
  } else {
    Serial.printf("[WS] Unhandled message type: '%s'\n", type);
  }
}

// ── Data parsing ───────────────────────────────────────────────────────────

void parseGitHubUpdate(JsonDocument& doc) {
  strlcpy(gmData.username,       doc["username"]             | "", sizeof(gmData.username));
  gmData.currentStreak  =        doc["current_streak"]       | 0;
  gmData.longestStreak  =        doc["longest_streak"]       | 0;
  gmData.today          =        doc["today"]                | 0;
  gmData.totalContribs  =        doc["total_contributions"]  | 0;
  gmData.weeklyContribs =        doc["weekly_contributions"] | 0;
  gmData.monthlyContribs=        doc["monthly_contributions"]| 0;
  gmData.sessionActive  = true;

  // Optional music metadata (future use)
  if (doc.containsKey("music")) {
    JsonObject music = doc["music"];
    gmData.musicEnabled   = music["enabled"]   | false;
    gmData.musicPattern   = music["pattern"]   | 0;
    gmData.musicIntensity = music["intensity"] | 0.0f;
  }
}

// ── Debug output ───────────────────────────────────────────────────────────

void printStats() {
  Serial.println();
  Serial.println("┌────────────────────────────────┐");
  Serial.printf ("│  GitHub User   : %-14s│\n", gmData.username);
  Serial.printf ("│  Current Streak: %-4d days      │\n", gmData.currentStreak);
  Serial.printf ("│  Longest Streak: %-4d days      │\n", gmData.longestStreak);
  Serial.printf ("│  Today         : %-4d contrib.  │\n", gmData.today);
  Serial.printf ("│  Total         : %-6d          │\n", gmData.totalContribs);
  Serial.printf ("│  This Week     : %-4d           │\n", gmData.weeklyContribs);
  Serial.printf ("│  This Month    : %-4d           │\n", gmData.monthlyContribs);
  if (gmData.musicEnabled) {
    Serial.printf("│  Music Pattern : %-2d  Intensity: %.2f │\n",
                  gmData.musicPattern, gmData.musicIntensity);
  }
  Serial.println("└────────────────────────────────┘");
  Serial.println();
}

/*
 * ═══════════════════════════════════════════════════════════════
 * FUTURE HARDWARE INTEGRATION STUBS
 * ───────────────────────────────────────────────────────────────
 * Uncomment and implement these functions when the hardware is
 * connected. The data structures are already populated.
 * ═══════════════════════════════════════════════════════════════
 */

/*
void updateOLED() {
  // Use Adafruit SSD1306 or U8g2 library
  // Display: username, current streak, today's contributions
}

void clearOLED() {
  // oled.clearDisplay();
  // oled.display();
}

void updateLEDs() {
  // 364 individually addressable WS2812B LEDs
  // Map gmData.currentStreak → LED pattern
  // Map gmData.musicIntensity → LED brightness
}

void clearLEDs() {
  // FastLED.clear();
  // FastLED.show();
}

void updateAudio() {
  // MAX98357A I2S amplifier
  // Map gmData.musicPattern (0-6) → notes C D E F G A B
  // Map gmData.musicIntensity → volume
}

void stopAudio() {
  // Stop I2S output
}
*/
