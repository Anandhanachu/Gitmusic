# 🎵 GitMusic

> **Turn your GitHub activity into sound and light on a 64×32 HUB75 RGB LED Matrix & I2S Synthesizer.**

GitMusic is an interactive IoT hardware project powered by an **ESP32 Dev Module**. When a user enters their GitHub username on the web dashboard, the backend fetches their 52-week contribution history, calculates streaks and activity levels, and streams the data in real time to the ESP32. 

The ESP32 transforms this data into a living audio-visual show:
- **Top rows (0–7)**: GitHub username banner with smooth left-to-right marquee scrolling for longer usernames, framed by an animated cyan-green gradient divider line.
- **Bottom rows (8–31)**: Exact 7×52 contribution streak graph (364 cells) rendered in authentic GitHub green palettes.
- **Tone.js-inspired Music Sequencer**: Plays through the user's year column by column using an 8-bit pentatonic scale (C4–D5), highlighting each week as its chord sounds.
- **Celebration Finale & Living Stats**: Triple-flash sparkle on the user's active streak with a fanfare chime, followed by a gentle "breathing" heartbeat effect on today's commit cell.

---

## Table of Contents

1. [System Architecture](#system-architecture)
2. [Hardware Components](#hardware-components)
3. [Wiring & Pinout Diagrams](#wiring--pinout-diagrams)
   - [HUB75 Matrix to ESP32](#hub75-matrix-to-esp32)
   - [MAX98357A I2S Amplifier to ESP32](#max98357a-i2s-amplifier-to-esp32)
   - [Power Supply & Grounding](#power-supply--grounding)
4. [Project Structure](#project-structure)
5. [Step-by-Step Implementation Guide](#step-by-step-implementation-guide)
   - [Step 1: Backend Setup](#step-1-backend-setup)
   - [Step 2: Frontend Setup](#step-2-frontend-setup)
   - [Step 3: Arduino IDE & ESP32 Configuration](#step-3-arduino-ide--esp32-configuration)
   - [Step 4: Install Required Arduino Libraries](#step-4-install-required-arduino-libraries)
   - [Step 5: Configure & Flash ESP32 Firmware](#step-5-configure--flash-esp32-firmware)
6. [Interactive Flow & Animations](#interactive-flow--animations)
7. [Troubleshooting & Diagnostics](#troubleshooting--diagnostics)

---

## System Architecture

```text
  ┌─────────────────────────────────────────────────────────┐
  │                    Browser Dashboard                    │
  │     (Vanilla HTML5, CSS3, Modern Dark Glassmorphic UI)   │
  └────────────────────────────┬────────────────────────────┘
                               │ HTTP REST & WebSocket (/ws/client)
                               ▼
  ┌─────────────────────────────────────────────────────────┐
  │                 FastAPI Python Backend                  │
  │   - GitHub GraphQL API Fetcher (or Mock Mode)           │
  │   - 52-Week Contribution Level & Streak Engine          │
  │   - Asyncio Single-Device Hardware Lock Session Manager  │
  └────────────────────────────┬────────────────────────────┘
                               │ WebSocket (/ws/device)
                               ▼
  ┌─────────────────────────────────────────────────────────┐
  │              ESP32 Dev Module (240 MHz)                 │
  │                                                         │
  │   ┌─────────────────────────────────────────────────┐   │
  │   │ HUB75 I2S DMA Driver                            │   │
  │   │ ➔ 64×32 RGB LED Matrix (P3 1/16 Scan)          │   │
  │   │   - Rows 0-7:  Scrolling Username Banner        │   │
  │   │   - Rows 8-31: 7×52 Contribution Streak Graph   │   │
  │   └─────────────────────────────────────────────────┘   │
  │                                                         │
  │   ┌─────────────────────────────────────────────────┐   │
  │   │ MAX98357A I2S Audio Synthesizer                 │   │
  │   │ ➔ 4Ω/8Ω Speaker (Pentatonic Sequencer Chimes)   │   │
  │   └─────────────────────────────────────────────────┘   │
  └─────────────────────────────────────────────────────────┘
```

---

## Hardware Components

| Component | Specification | Purpose |
|-----------|--------------|---------|
| **Microcontroller** | ESP32 Dev Module (30-pin or 38-pin, 240 MHz Dual-Core) | Main controller with hardware DMA & I2S |
| **RGB LED Matrix** | HUB75 P3 64×32 RGB LED Panel (P3 6432 2121 16SD1.0) | 2048 LEDs, 1/16 scan rate |
| **Audio Amplifier** | MAX98357A I2S 3.2W Class D Mono Amplifier | Digital-to-analog audio amplification |
| **Speaker** | 4Ω 3W or 8Ω 2W Speaker | Real-time musical feedback & chimes |
| **External PSU** | 5V 3A to 5V 5A DC Power Adapter | Powers the 64×32 LED Matrix (peak draw ~2.5A) |
| **USB Cable** | Micro-USB or USB-C Data Cable | ESP32 programming & power |
| **Jumper Wires** | Female-to-Female Dupont Wires | Direct connections between ESP32 and modules |

---

## Wiring & Pinout Diagrams

### HUB75 Matrix to ESP32

Connect the 16-pin IDC input connector (**JIN**) on the back of the HUB75 matrix to the ESP32 Dev Module according to this verified pin mapping:

```text
           HUB75 IDC INPUT (JIN) PINOUT
                  ┌────┬────┐
             R1   │  1 │  2 │  G1
             B1   │  3 │  4 │  GND
             R2   │  5 │  6 │  G2
             B2   │  7 │  8 │  E (N/C)
              A   │  9 │ 10 │  B
              C   │ 11 │ 12 │  D
            CLK   │ 13 │ 14 │  LAT / STB
             OE   │ 15 │ 16 │  GND
                  └────┴────┘
```

| HUB75 Pin | Pin Name | ESP32 Dev Module GPIO | Note |
|:---------:|:--------:|:---------------------:|:-----|
| 1 | **R1** | **GPIO 25** | Top half Red |
| 2 | **G1** | **GPIO 26** | Top half Green |
| 3 | **B1** | **GPIO 27** | Top half Blue |
| 4 | **GND** | **GND** | Signal Ground (Must tie to ESP32 GND) |
| 5 | **R2** | **GPIO 14** | Bottom half Red |
| 6 | **G2** | **GPIO 12** | Bottom half Green |
| 7 | **B2** | **GPIO 13** | Bottom half Blue |
| 8 | **E** | *NOT CONNECTED* | Unused for 1/16 scan panels (`E_PIN = -1`) |
| 9 | **A** | **GPIO 23** | Row Address bit 0 |
| 10 | **B** | **GPIO 19** | Row Address bit 1 |
| 11 | **C** | **GPIO 5** | Row Address bit 2 |
| 12 | **D** | **GPIO 17** | Row Address bit 3 |
| 13 | **CLK** | **GPIO 16** | Shift Clock |
| 14 | **LAT** | **GPIO 4** | Latch / Strobe |
| 15 | **OE** | **GPIO 15** | Output Enable (Active Low) |
| 16 | **GND** | **GND** | Signal Ground |

> [!IMPORTANT]
> **Panel Configuration Settings in Firmware:**
> - `mxconfig.clkphase = false`
> - `mxconfig.latch_blanking = 2`
> - `mxconfig.gpio.e = -1` (1/16 scan panels address 16 rows using A, B, C, D only).

---

### MAX98357A I2S Amplifier to ESP32

The MAX98357A digital amplifier communicates with the ESP32 over the native I2S bus:

| MAX98357A Pin | ESP32 Dev Module Pin | Description |
|:-------------:|:--------------------:|:------------|
| **LRC** (WS) | **GPIO 33** | Left/Right Word Select Clock |
| **BCLK** | **GPIO 18** | Bit Clock |
| **DIN** | **GPIO 22** | Serial Audio Data |
| **VIN** | **5V** or **3.3V** | Power (5V delivers 3.2W max output) |
| **GND** | **GND** | Power Ground (tie to ESP32 GND) |
| **GAIN** | *Leave Floating* or **GND** | Floating = 9dB gain, GND = 12dB gain |
| **SD** | *Leave Floating* | Mixes Left & Right channel automatically |

Connect your 4Ω or 8Ω speaker to the **+** and **-** screw terminal block of the MAX98357A.

---

### Power Supply & Grounding

```text
   5V 3A+ Power Adapter
   ├── (+) 5V ───────► HUB75 Matrix Power Terminals (+5V)
   │
   └── (-) GND ──────┬─► HUB75 Matrix Power Terminals (GND)
                     ├─► HUB75 JIN IDC GND Pins (Pin 4 & 16)
                     ├─► ESP32 Dev Module GND Pin
                     └─► MAX98357A GND Pin
                     
   PC / Laptop USB
   └── USB Cable ────► ESP32 Micro-USB / USB-C Port
```

> [!WARNING]
> **Common Ground is Mandatory:** You must connect the Ground (`GND`) of the 5V power supply, the HUB75 Matrix, the ESP32, and the MAX98357A amplifier together. Without a common ground reference, the high-speed 20 MHz DMA signals will experience severe noise and corrupted display output.

---

## Project Structure

```text
gitmusic/
├── backend/
│   ├── main.py              # FastAPI server, REST routes, WebSocket endpoints
│   ├── device_manager.py    # Single-device session state & asyncio locking
│   ├── websocket_manager.py # Broadcast updates to web clients
│   ├── github.py            # GitHub GraphQL API client & mock data generator
│   ├── streak.py            # Streak calculation & weekly aggregation logic
│   ├── models.py            # Pydantic schemas
│   ├── requirements.txt     # Python backend dependencies
│   └── .env.example         # Template for environment configuration
│
├── frontend/
│   ├── index.html           # Dark cybernetic glassmorphism web dashboard
│   ├── style.css            # Responsive layout & micro-animations
│   └── app.js               # REST fetch & client WebSocket handling
│
├── esp32/
│   └── gitmusic.ino         # ESP32 Dev Module HUB75 DMA + I2S firmware
│
├── render.yaml              # Render cloud deployment blueprint
└── README.md                # Complete documentation and setup manual
```

---

## Step-by-Step Implementation Guide

### Step 1: Backend Setup

The backend handles GitHub API fetching, calculates streaks, manages single-user hardware locking, and streams WebSocket events.

1. **Open a terminal in the `backend/` folder:**
   ```bash
   cd backend
   ```

2. **Create and activate a Python virtual environment:**
   - **Windows (PowerShell):**
     ```powershell
     python -m venv venv
     .\venv\Scripts\Activate.ps1
     ```
   - **macOS / Linux:**
     ```bash
     python3 -m venv venv
     source venv/bin/activate
     ```

3. **Install the required packages:**
   ```bash
   pip install -r requirements.txt
   ```

4. **Configure the environment file:**
   ```bash
   cp .env.example .env
   ```
   Open `.env` in your text editor:
   ```env
   # Set your GitHub Personal Access Token (classic token with read:user scope)
   GITHUB_TOKEN=ghp_yourActualTokenHere
   
   # Or set to true for offline development with generated mock data:
   MOCK_GITHUB=false
   
   # Server configuration
   PORT=8000
   HOST=0.0.0.0
   ```

5. **Start the backend server:**
   ```bash
   uvicorn main:app --reload --host 0.0.0.0 --port 8000
   ```
   You should see:
   ```text
   INFO:     Uvicorn running on http://0.0.0.0:8000 (Press CTRL+C to quit)
   ```
   Test health in your browser by visiting `http://localhost:8000/docs`.

---

### Step 2: Frontend Setup

The frontend is a lightweight static web interface.

1. Find your computer's local IP address on your Wi-Fi network:
   - **Windows:** Run `ipconfig` in CMD/PowerShell (look for *IPv4 Address*, e.g., `192.168.1.100`).
   - **macOS / Linux:** Run `ifconfig` or `ip a`.
2. Open `frontend/index.html` directly in your browser, or serve it using Python:
   ```bash
   cd ../frontend
   python -m http.server 3000
   ```
3. Open `http://localhost:3000` in your web browser. In the connection settings, point the API host to `http://localhost:8000` (or your local IP).

---

### Step 3: Arduino IDE & ESP32 Configuration

1. **Install Arduino IDE 2.x** from [arduino.cc](https://www.arduino.cc/en/software).
2. **Add ESP32 Board Manager URL:**
   - Go to **File ➔ Preferences**.
   - In **Additional boards manager URLs**, add:
     ```text
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Click **OK**.
3. **Install ESP32 Platform:**
   - Open **Boards Manager** (left sidebar icon).
   - Search for `esp32` by **Espressif Systems** and click **Install**.
4. **Select Board & Performance Settings:**
   - Under **Tools ➔ Board ➔ esp32**, choose: **ESP32 Dev Module**.
   - Under **Tools**, set the following options:
     - **CPU Frequency:** `240MHz (WiFi/BT)` *(MANDATORY for HUB75 DMA driver timing)*
     - **Flash Frequency:** `80MHz`
     - **Flash Mode:** `QIO`
     - **Partition Scheme:** `Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)`
     - **Core Debug Level:** `None`

---

### Step 4: Install Required Arduino Libraries

In Arduino IDE, open **Tools ➔ Manage Libraries...** (or `Ctrl+Shift+I` / `Cmd+Shift+I`) and install:

1. **`ESP32-HUB75-MatrixPanel-I2S-DMA`** by *mrfaptastic*
   - Direct high-speed DMA driver for HUB75 LED panels on ESP32.
2. **`ArduinoWebsockets`** by *Gil Maimon*
   - Lightweight WebSocket client with native frame handling.
3. **`ArduinoJson`** by *Benoit Blanchon*
   - JSON serialization and parsing (firmware supports both v6 and v7 automatically).

---

### Step 5: Configure & Flash ESP32 Firmware

1. Open [`esp32/gitmusic.ino`](file:///c:/Users/anand/OneDrive/Desktop/gitmusic/esp32/gitmusic.ino) in Arduino IDE.
2. Update your Wi-Fi credentials and your backend server's local LAN IP:
   ```cpp
   // ==========================================================================
   // USER CONFIGURATION
   // ==========================================================================
   #define WIFI_SSID           "Your_WiFi_Name"
   #define WIFI_PASSWORD       "Your_WiFi_Password"
   #define WS_SERVER_URL       "ws://192.168.1.100:8000/ws/device"
   #define DEVICE_ID           "gitmusic-01"
   ```
3. Connect your ESP32 Dev Module to your computer via USB.
4. Select the appropriate serial COM port under **Tools ➔ Port**.
5. Click **Upload** (`Ctrl+U` / `Cmd+U`).
6. Once uploaded, open the **Serial Monitor** at **115200 baud**. You will see:
   ```text
   =============================================
     GitMusic -- ESP32 Dev Module (v2.0)
     64x32 HUB75 + MAX98357A I2S Audio
   =============================================
   [Matrix] HUB75 64x32 initialized.
   [I2S] MAX98357A initialized (BCK=18, LRCK=33, DIN=22).
   [WiFi] Connecting to "Your_WiFi_Name"......
   [WiFi] Connected! IP: 192.168.1.150
   [WS] Connecting to ws://192.168.1.100:8000/ws/device ...
   [WS] Connected to backend.
   [WS] Registered as 'gitmusic-01'
   ```

---

## Interactive Flow & Animations

### 1. Ambient Idle Mode
- When no active session is running, the panel displays a gentle, hypnotic 60 FPS rainbow wave across all 2048 LEDs.
- A subtle "GITMUSIC" watermark glows in the center.
- An ambient pentatonic bass note plays once every 15 seconds to signal that the device is online and ready.

### 2. Live Sequencer Sweep (New User Session)
- When a user enters their GitHub handle on the website, the ESP32 locks the session and receives the user's data.
- The username appears on rows 0–7 (centering short names or automatically scrolling long names).
- An animated green-to-cyan gradient divider line frames the top header.
- **Tone.js Column Sweep**: The matrix reveals all 52 weeks column by column from oldest to newest:
  - Each week column briefly flashes in neon mint/cyan.
  - An authentic pentatonic note triggers based on that week's maximum commit density:
    - `Level 0`: Silence
    - `Level 1`: C4 (261.63 Hz)
    - `Level 2`: D4 (293.66 Hz)
    - `Level 3`: E4 (329.63 Hz)
    - `Level 4`: G4 (392.00 Hz)

### 3. Grand Finale Celebration
- When the sweep finishes at week 51:
  - The current week and active streak columns flash 3 times with a sparkling gold-white effect (`RGB 240, 240, 180`).
  - An ascending fanfare chord chimes (`A4 ➔ C5 ➔ D5`).

### 4. Living Stats View
- The 52×7 graph settles into authentic GitHub contribution shades:
  - Level 0: Dim background dot (`#0a0a0a`)
  - Level 1: Dark green (`#0e4429`)
  - Level 2: Mid green (`#006d32`)
  - Level 3: Bright green (`#26a641`)
  - Level 4: Vivid emerald (`#39d353`)
- **Heartbeat Today Cell**: If the user has made commits today, cell `[51, 6]` gently pulses in brightness every 1.5 seconds.
- **Streak Shimmer Wave**: Every 5 seconds, an energetic shimmer wave travels across the active streak weeks.

---

## Troubleshooting & Diagnostics

| Symptom | Probable Cause | Solution |
|---------|---------------|----------|
| **Display flickers, jitters, or displays random snow** | ESP32 CPU frequency is under 240 MHz or clock phase is inverted | Set CPU Frequency to **240 MHz** in Arduino IDE Tools. Ensure `mxconfig.clkphase = false` and `mxconfig.latch_blanking = 2`. |
| **Only the top 16 rows light up or bottom 16 rows duplicate** | Incorrect row scan setting or E pin miswired | Panel is 1/16 scan: ensure `mxconfig.gpio.e = -1` (unconnected). Check ribbon connector orientation. |
| **Colors are wrong (e.g. Red appears Blue)** | R/G/B pin swapping on ribbon cable | Verify GPIO assignments: R1=25, G1=26, B1=27, R2=14, G2=12, B2=13. |
| **LEDs dim or ESP32 brownout restarts when matrix flashes** | Insufficient 5V power supply capacity | Power the matrix with an external **5V 3A+ power supply**. Do not power 2048 LEDs from the ESP32 5V/VIN pin. |
| **No sound from the speaker** | Incorrect I2S pin assignments or missing ground | Verify pins: BCLK=18, LRCK=33, DIN=22. Ensure MAX98357A GND is tied to ESP32 GND. Verify `SAMPLE_RATE = 44100`. |
| **Audio has loud buzz or clicking** | Floating GND or shared noisy 5V rail | Ensure a solid, thick ground wire between the ESP32 and amplifier. Connect MAX98357A GAIN to GND for 12dB quiet mode. |
| **`[WS] Failed - will retry` in Serial Monitor** | Wrong IP address or firewall blocking port 8000 | Verify your computer's LAN IP address. On Windows, allow port 8000 through Windows Defender Firewall. |
| **`[JSON] Error: ...`** | ArduinoJson version discrepancy | The firmware includes backward-compatible macros for both ArduinoJson v6 and v7. Update to the latest ArduinoJson via Library Manager. |

---

## License

MIT License. Designed with ❤️ for the open-source community.
