# 🎵 GitMusic

> **Turn your GitHub activity into sound and light.**

GitMusic is a physical IoT project built around a **Seeed Studio XIAO ESP32-S3**. A user enters their GitHub username on the website; the backend retrieves their contribution data, calculates streak statistics, and streams the results in real time to the connected ESP32 device. The ESP32 then drives an OLED display, a MAX98357A audio amplifier, and 364 individually addressable LEDs to produce a personalised light-and-sound show.

---

## Table of Contents

1. [Architecture](#architecture)
2. [Technologies](#technologies)
3. [Folder Structure](#folder-structure)
4. [Running Locally](#running-locally)
5. [GitHub API Credentials](#github-api-credentials)
6. [Environment Variables](#environment-variables)
7. [Website ↔ Backend Communication](#website--backend-communication)
8. [Backend ↔ ESP32 Communication](#backend--esp32-communication)
9. [WebSocket Protocol](#websocket-protocol)
10. [JSON Message Formats](#json-message-formats)
11. [Device Locking](#device-locking)
12. [Simultaneous Users](#simultaneous-users)
13. [Deploying to Render](#deploying-to-render)
14. [Connecting the ESP32](#connecting-the-esp32)
15. [Troubleshooting](#troubleshooting)

---

## Architecture

```
Browser (User)
     │
     │  HTTP REST  +  WebSocket (/ws/client)
     ▼
FastAPI Backend  ──► GitHub GraphQL API
     │
     │  Streak / stats calculation (Python)
     │
     │  WebSocket (/ws/device)
     ▼
XIAO ESP32-S3
     ├── SSD1306 OLED display
     ├── MAX98357A I2S audio amplifier
     └── 364 × WS2812B LEDs
```

**Key constraint:** There is only **one physical ESP32 device**. The backend uses an `asyncio.Lock` to guarantee that only one user session can control the device at any given time.

---

## Technologies

| Layer | Technology |
|-------|-----------|
| Frontend | HTML5, CSS3, Vanilla JavaScript |
| Backend | Python 3.11, FastAPI, Uvicorn |
| Real-time | WebSockets (native FastAPI) |
| GitHub data | GitHub GraphQL API |
| ESP32 firmware | Arduino (C++), ArduinoWebsockets, ArduinoJson |
| Deployment | Render (backend), GitHub Pages / Render Static (frontend) |

---

## Folder Structure

```
gitmusic/
├── frontend/
│   ├── index.html          # Single-page UI
│   ├── style.css           # Dark futuristic design
│   └── app.js              # WebSocket + REST client logic
│
├── backend/
│   ├── __init__.py         # Auto-loads .env
│   ├── main.py             # FastAPI app, routes, WS endpoints
│   ├── device_manager.py   # Single-device state + asyncio lock
│   ├── websocket_manager.py# Broadcast to all website clients
│   ├── github.py           # GitHub GraphQL API + mock mode
│   ├── streak.py           # Streak calculation algorithms
│   ├── models.py           # Pydantic models
│   ├── requirements.txt
│   └── .env.example
│
├── esp32/
│   └── gitmusic.ino        # XIAO ESP32-S3 firmware
│
├── render.yaml             # Render deployment config
├── .gitignore
└── README.md
```

---

## Running Locally

### Prerequisites

- Python 3.11+
- pip
- A GitHub Personal Access Token (or set `MOCK_GITHUB=true` to skip)

### 1 – Clone and install

```bash
git clone https://github.com/Anandhanachu/Gitmusic.git
cd Gitmusic
```

### 2 – Configure the backend

```bash
cd backend
cp .env.example .env
# Edit .env and set GITHUB_TOKEN (or set MOCK_GITHUB=true)
```

### 3 – Install Python dependencies

```bash
pip install -r backend/requirements.txt
```

### 4 – Start the backend

```bash
# From the project root (gitmusic/)
uvicorn backend.main:app --reload --port 8000
```

The backend now serves:
- `http://localhost:8000/` → GitMusic website
- `http://localhost:8000/api/health` → health check
- `ws://localhost:8000/ws/client` → website WebSocket
- `ws://localhost:8000/ws/device` → ESP32 WebSocket

### 5 – Open the website

Navigate to **http://localhost:8000** in your browser.

> **No separate frontend server needed.** The FastAPI backend serves the static files directly.

---

## GitHub API Credentials

1. Go to **https://github.com/settings/tokens**
2. Click **"Generate new token (classic)"**
3. Select the scope: **`read:user`** (minimum required)
4. Copy the token and add it to `backend/.env`:

```
GITHUB_TOKEN=ghp_xxxxxxxxxxxxxxxxxxxx
```

### Mock mode (no token needed)

Set `MOCK_GITHUB=true` in `.env`. The following usernames return preset data:

| Username | Current Streak | Total |
|----------|---------------|-------|
| `demo`   | 24 days       | 1,248 |
| `test`   | 3 days        | 312   |
| any other | deterministic random based on name | – |

---

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `GITHUB_TOKEN` | *(required)* | GitHub personal access token |
| `MOCK_GITHUB` | `false` | Use mock data instead of real GitHub API |
| `SESSION_TIMEOUT_SECONDS` | `300` | Auto-release device after this many idle seconds |
| `ALLOWED_ORIGINS` | `*` | CORS allowed origins (comma-separated) |

---

## Website ↔ Backend Communication

### REST

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/health` | Health check |
| GET | `/api/device/status` | Current device state |
| POST | `/api/connect` | Acquire device + fetch GitHub stats |
| POST | `/api/disconnect` | Release device |

### WebSocket (`/ws/client`)

The website opens a WebSocket immediately on page load and receives real-time broadcasts:

```json
// Device status changed
{ "type": "device_status", "status": "BUSY", "username": "torvalds" }

// Session timed out
{ "type": "session_timeout", "message": "Your session expired." }
```

---

## Backend ↔ ESP32 Communication

The ESP32 connects to `ws(s)://YOUR_BACKEND/ws/device` and maintains a persistent connection.

---

## WebSocket Protocol

### ESP32 → Backend

```json
// Initial registration (sent once on connect)
{ "type": "device_register", "device_id": "gitmusic-01" }

// Keep-alive
{ "type": "ping" }
```

### Backend → ESP32

```json
// Registration acknowledgement
{ "type": "device_registered", "device_id": "gitmusic-01", "status": "AVAILABLE" }

// GitHub stats (when a user connects)
{
  "type": "github_update",
  "username": "achukuttan",
  "current_streak": 24,
  "longest_streak": 36,
  "today": 6,
  "total_contributions": 1248,
  "weekly_contributions": 23,
  "monthly_contributions": 87,
  "music": { "enabled": true, "pattern": 3, "intensity": 0.72 }
}

// Session ended (user disconnected / timeout)
{ "type": "session_end" }

// Keep-alive response
{ "type": "pong" }
```

---

## JSON Message Formats

### POST /api/connect

**Request:**
```json
{ "username": "achukuttan" }
```

**Success response:**
```json
{
  "success": true,
  "session_id": "7f3c9e2a1b4d5e6f",
  "username": "achukuttan",
  "stats": {
    "current_streak": 24,
    "longest_streak": 36,
    "today_contributions": 6,
    "total_contributions": 1248,
    "weekly_contributions": 23,
    "monthly_contributions": 87
  }
}
```

**Device busy response:**
```json
{
  "success": false,
  "error": "DEVICE_BUSY",
  "message": "The GitMusic device is currently being used by another user."
}
```

---

## Device Locking

The `DeviceManager` class in [`backend/device_manager.py`](backend/device_manager.py) uses an `asyncio.Lock` to ensure atomic device acquisition:

1. `acquire_device()` acquires the lock before checking/setting state.
2. If the device is already owned, it returns `None` immediately.
3. The REST `POST /api/connect` endpoint checks the return value and rejects the second request.
4. A session timeout coroutine is created per session (configurable via `SESSION_TIMEOUT_SECONDS`).
5. `release_device()` also runs inside the lock and validates the session ID.

---

## Simultaneous Users

| Scenario | Outcome |
|----------|---------|
| User A connects first | ✅ Gets the device, GitHub data sent to ESP32 |
| User B connects while A is active | ❌ Gets `DEVICE_BUSY` error immediately |
| User A's browser closes | Device auto-released after `SESSION_TIMEOUT_SECONDS` |
| User A clicks Disconnect | Device immediately released, User B can now connect |
| ESP32 disconnects | All clients see `DISCONNECTED` status; connect button disabled |

---

## Deploying to Render

### Backend

1. Push the repo to GitHub.
2. In Render → **New Web Service** → connect your repo.
3. Set **Root Directory** to `.` and **Build Command** to `pip install -r backend/requirements.txt`.
4. Set **Start Command** to `uvicorn backend.main:app --host 0.0.0.0 --port $PORT`.
5. Add environment variables in the Render dashboard:
   - `GITHUB_TOKEN` = your token
   - `MOCK_GITHUB` = `false`
   - `SESSION_TIMEOUT_SECONDS` = `300`

### Frontend

The backend serves the frontend as static files, so **no separate deployment is needed**. Just deploy the backend and open its URL.

Alternatively, deploy `frontend/` as a **Render Static Site** and set `BASE_URL` in `app.js` to the backend URL.

---

## Connecting the ESP32

1. Open `esp32/gitmusic.ino` in **Arduino IDE** (2.x recommended).
2. Install the XIAO ESP32-S3 board via **Boards Manager** → search `esp32`.
3. Install libraries via **Library Manager**:
   - `ArduinoWebsockets` (Gil Maimon)
   - `ArduinoJson` (Benoit Blanchon)
4. Edit the `#define` values at the top of the sketch:
   ```cpp
   #define WIFI_SSID      "YourWiFiName"
   #define WIFI_PASSWORD  "YourWiFiPassword"
   #define WS_SERVER_URL  "ws://192.168.1.x:8000/ws/device"
   ```
5. Select board: **XIAO_ESP32S3** and your COM port.
6. Upload the sketch.
7. Open **Serial Monitor** at **115200 baud** to watch the registration handshake.

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `GITHUB_TOKEN is not set` | Add `GITHUB_TOKEN` to `backend/.env` or set `MOCK_GITHUB=true` |
| `GitHub user not found` | Check the username spelling; the user must be public |
| `Device is disconnected` | The ESP32 is not connected to the backend WebSocket |
| ESP32 not reconnecting | Check `WS_SERVER_URL`; ensure backend is reachable from ESP32's network |
| Two users can control device | This is a bug – check `device_manager.py` lock logic |
| Session never releases | Reduce `SESSION_TIMEOUT_SECONDS` or click Disconnect |
| CORS error in browser | Set `ALLOWED_ORIGINS` to your frontend URL |
| Rate limit from GitHub | Wait 60 minutes or use a different token |

---

## API Testing Commands

```bash
# Health check
curl http://localhost:8000/api/health

# Device status
curl http://localhost:8000/api/device/status

# Connect with a username
curl -X POST http://localhost:8000/api/connect \
  -H "Content-Type: application/json" \
  -d '{"username": "demo"}'

# Disconnect (replace SESSION_ID)
curl -X POST http://localhost:8000/api/disconnect \
  -H "Content-Type: application/json" \
  -d '{"session_id": "SESSION_ID_HERE"}'
```

---

*Built with ❤️ for the GitMusic IoT project.*
