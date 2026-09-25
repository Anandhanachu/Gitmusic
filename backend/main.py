"""
main.py - FastAPI application entry point for GitMusic backend.

Endpoints:
  REST:
    GET  /api/health
    GET  /api/device/status
    POST /api/connect
    POST /api/disconnect

  WebSocket:
    /ws/device   ← ESP32 connects here
    /ws/client   ← Website connects here

  Static files:
    /  (serves frontend/)
"""

import logging
import os
import re
from contextlib import asynccontextmanager
from pathlib import Path

from dotenv import load_dotenv

# Load .env from backend directory or project root
_here = Path(__file__).parent
for _candidate in [_here / ".env", _here.parent / ".env"]:
    if _candidate.is_file():
        load_dotenv(_candidate)
        break

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from slowapi import Limiter, _rate_limit_exceeded_handler
from slowapi.util import get_remote_address
from slowapi.errors import RateLimitExceeded

try:
    from backend.device_manager import device_manager
    from backend.github import get_github_stats
    from backend.models import (
        ConnectRequest, ConnectResponse,
        DisconnectRequest, DisconnectResponse,
    )
    from backend.websocket_manager import ws_manager
except ModuleNotFoundError:
    from device_manager import device_manager
    from github import get_github_stats
    from models import (
        ConnectRequest, ConnectResponse,
        DisconnectRequest, DisconnectResponse,
    )
    from websocket_manager import ws_manager

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Rate limiter
# ---------------------------------------------------------------------------
limiter = Limiter(key_func=get_remote_address)

# ---------------------------------------------------------------------------
# Input validation helper
# ---------------------------------------------------------------------------
GITHUB_USERNAME_RE = re.compile(r"^[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,37}[a-zA-Z0-9])?$")


def validate_username(username: str) -> str:
    """Sanitize and validate a GitHub username."""
    username = username.strip()
    if not username:
        raise HTTPException(status_code=400, detail="Username cannot be empty.")
    if len(username) > 39:
        raise HTTPException(status_code=400, detail="Username too long.")
    if not GITHUB_USERNAME_RE.match(username):
        raise HTTPException(
            status_code=400,
            detail="Invalid GitHub username. Only alphanumeric characters and hyphens are allowed.",
        )
    return username


# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------

@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info("GitMusic backend starting up …")
    yield
    logger.info("GitMusic backend shutting down …")


app = FastAPI(
    title="GitMusic API",
    description="Backend for the GitMusic IoT device.",
    version="1.0.0",
    lifespan=lifespan,
)

app.state.limiter = limiter
app.add_exception_handler(RateLimitExceeded, _rate_limit_exceeded_handler)

# Allow all origins in development; tighten in production via env var.
ALLOWED_ORIGINS = os.getenv("ALLOWED_ORIGINS", "*").split(",")

app.add_middleware(
    CORSMiddleware,
    allow_origins=ALLOWED_ORIGINS,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


# ---------------------------------------------------------------------------
# REST endpoints
# ---------------------------------------------------------------------------

@app.get("/api/health", tags=["Meta"])
async def health():
    """Simple health check."""
    return {"status": "ok", "service": "GitMusic API"}


@app.get("/api/device/status", tags=["Device"])
async def device_status():
    """Return current device state."""
    return device_manager.get_status_dict()


@app.post("/api/connect", response_model=ConnectResponse, tags=["Session"])
@limiter.limit("10/minute")
async def connect(request: Request, body: ConnectRequest):
    """
    Attempt to acquire the GitMusic device for a GitHub username.

    Returns success + stats on acquisition, or an error if device is busy.
    """
    username = validate_username(body.username)

    # Check device availability before hitting GitHub API
    status = device_manager.get_status_dict()
    if status["status"] == "DISCONNECTED":
        return ConnectResponse(
            success=False,
            error="DEVICE_DISCONNECTED",
            message="The GitMusic device is not connected. Please try again later.",
        )
    if status["status"] == "BUSY":
        return ConnectResponse(
            success=False,
            error="DEVICE_BUSY",
            message="The GitMusic device is currently being used by another user.",
        )

    # Fetch GitHub data (may raise ValueError / RuntimeError)
    try:
        stats = await get_github_stats(username)
    except ValueError as exc:
        raise HTTPException(status_code=404, detail=str(exc))
    except RuntimeError as exc:
        raise HTTPException(status_code=502, detail=str(exc))

    # Attempt atomic device acquisition
    session = await device_manager.acquire_device(username)
    if session is None:
        # Lost the race – another request just grabbed the device
        return ConnectResponse(
            success=False,
            error="DEVICE_BUSY",
            message="The GitMusic device is currently being used by another user.",
        )

    # Send data to ESP32
    await device_manager.send_github_update(stats)

    # Broadcast new device status to all connected website clients
    await ws_manager.broadcast({
        "type": "device_status",
        "status": "BUSY",
        "username": username,
    })

    logger.info("Session %s started for user '%s'", session.session_id, username)

    audio_url = f"/api/music/{session.session_id}/audio.wav" if session.audio_path else None

    return ConnectResponse(
        success=True,
        session_id=session.session_id,
        username=username,
        stats=stats,
        levels=session.levels,
        timeline=session.timeline,
        audio_url=audio_url,
    )


@app.post("/api/disconnect", response_model=DisconnectResponse, tags=["Session"])
async def disconnect(body: DisconnectRequest):
    """Release device ownership for a given session."""
    released = await device_manager.release_device(body.session_id)
    if not released:
        return DisconnectResponse(
            success=False,
            message="Session not found or already ended.",
        )

    await ws_manager.broadcast({
        "type": "device_status",
        "status": "AVAILABLE" if device_manager.device_connected else "DISCONNECTED",
        "username": None,
    })

    return DisconnectResponse(success=True, message="Disconnected successfully.")


@app.get("/api/music/{session_id}/audio.wav", tags=["Music"])
async def get_music_audio(session_id: str):
    """Serve the 16-bit 44.1kHz mono PCM WAV for the active session."""
    session = device_manager.current_session
    cache_dir = os.path.join(os.path.dirname(__file__), "audio_cache")
    wav_path = os.path.join(cache_dir, f"{session_id}.wav")
    if not os.path.isfile(wav_path):
        if session and session.session_id == session_id and session.audio_path and os.path.isfile(session.audio_path):
            wav_path = session.audio_path
        else:
            raise HTTPException(status_code=404, detail="Audio file not found for this session.")

    return FileResponse(
        wav_path,
        media_type="audio/wav",
        headers={
            "Accept-Ranges": "bytes",
            "Cache-Control": "public, max-age=3600",
        },
    )


@app.get("/api/music/{session_id}/timeline", tags=["Music"])
async def get_music_timeline(session_id: str):
    """Return the authoritative musical timeline for the session."""
    session = device_manager.current_session
    if not session or session.session_id != session_id or not session.timeline:
        raise HTTPException(status_code=404, detail="Timeline not found for this session.")
    return session.timeline


@app.post("/api/music/prepare", tags=["Music"])
async def prepare_music_endpoint(request: Request):
    """Trigger ESP32 audio buffering and timeline preparation."""
    host = request.headers.get("host", "localhost:8000")
    proto = request.url.scheme
    base_url = f"{proto}://{host}"
    res = await device_manager.prepare_music(base_url)
    return res


@app.post("/api/music/play", tags=["Music"])
@app.post("/api/play", tags=["Music"])
async def play_music():
    """Trigger synchronized playback on ESP32 and website."""
    if not device_manager.device_connected:
        return {"success": False, "message": "ESP32 not connected"}
    ok = await device_manager.start_music()
    await ws_manager.broadcast({
        "type": "music_start",
    })
    return {"success": ok}


@app.post("/api/music/stop", tags=["Music"])
@app.post("/api/stop", tags=["Music"])
async def stop_music():
    """Stop synchronized playback on ESP32 and website."""
    ok = await device_manager.stop_music()
    await ws_manager.broadcast({
        "type": "music_stop",
    })
    return {"success": ok}


# ---------------------------------------------------------------------------
# WebSocket: ESP32 device
# ---------------------------------------------------------------------------

@app.websocket("/ws/device")
async def ws_device(ws: WebSocket):
    """
    Persistent WebSocket endpoint for the physical ESP32.

    Protocol:
        ESP32 → backend:  {"type": "device_register", "device_id": "gitmusic-01"}
        backend → ESP32:  {"type": "device_registered", …}
        backend → ESP32:  {"type": "github_update", …}   (when user connects)
        backend → ESP32:  {"type": "session_end"}         (when user disconnects)
    """
    await ws.accept()
    logger.info("ESP32 WebSocket connection accepted from %s", ws.client)

    registered = False

    try:
        while True:
            try:
                data = await ws.receive_json()
            except WebSocketDisconnect:
                logger.info("ESP32 WebSocket disconnected normally.")
                break
            except Exception as exc:
                logger.warning("ESP32 receive error: %r (%s)", type(exc), exc)
                break

            msg_type = data.get("type", "")

            if msg_type == "device_register":
                device_id = data.get("device_id", "unknown")
                # Only allow the known device ID
                if device_id != device_manager.DEVICE_ID:
                    await ws.send_json({
                        "type": "error",
                        "message": f"Unknown device_id '{device_id}'.",
                    })
                    await ws.close()
                    return

                await device_manager.register_device(ws)
                registered = True

                await ws.send_json({
                    "type": "device_registered",
                    "device_id": device_manager.DEVICE_ID,
                    "status": "AVAILABLE",
                })

                # Notify all website clients that the device came online
                await ws_manager.broadcast({
                    "type": "device_status",
                    "status": "AVAILABLE",
                    "username": None,
                })

                logger.info("Device '%s' registered via WebSocket.", device_id)

            elif msg_type == "ping":
                await ws.send_json({"type": "pong"})

            elif msg_type == "music_ready":
                logger.info("ESP32 reported music_ready")
                await device_manager.on_device_ready()
                await ws_manager.broadcast({
                    "type": "music_ready",
                    "device_id": device_manager.DEVICE_ID,
                })

            elif msg_type == "music_loop":
                logger.info("ESP32 reported music loop")
                await ws_manager.broadcast({
                    "type": "music_loop",
                })

            else:
                logger.debug("Unhandled device message type: '%s'", msg_type)

    except WebSocketDisconnect:
        logger.info("ESP32 WebSocket disconnected.")
    except Exception as exc:
        logger.error("ESP32 WebSocket error: %s", exc)
    finally:
        if registered:
            was_unregistered = await device_manager.unregister_device(ws)
            if was_unregistered:
                await ws_manager.broadcast({
                    "type": "device_status",
                    "status": "DISCONNECTED",
                    "username": None,
                })


# ---------------------------------------------------------------------------
# WebSocket: Website clients
# ---------------------------------------------------------------------------

@app.websocket("/ws/client")
async def ws_client(ws: WebSocket):
    """
    WebSocket endpoint for website browsers.

    Clients receive real-time device-status broadcasts.
    They can also send a ping to keep the connection alive.
    """
    await ws.accept()
    session_id = ""

    await ws_manager.connect(ws)
    logger.debug("Website client connected. Total clients: %d", ws_manager.count)

    # Send current device status immediately on connect
    try:
        await ws.send_json({
            "type": "device_status",
            **device_manager.get_status_dict(),
        })
    except Exception:
        pass

    try:
        while True:
            try:
                data = await ws.receive_json()
            except Exception:
                break

            msg_type = data.get("type", "")

            if msg_type == "client_ping":
                await ws.send_json({"type": "pong"})

            elif msg_type == "register_session":
                # Website tells us its session_id so we can send targeted msgs
                session_id = data.get("session_id", "")
                if session_id:
                    await ws_manager.connect(ws, session_id)

                    # Attach WS to session for timeout notification
                    session = device_manager.current_session
                    if session and session.session_id == session_id:
                        session.attach_websocket(ws)

            elif msg_type == "music_prepare":
                base_url = data.get("base_url", "http://localhost:8000")
                logger.info("Website requested music prepare, base_url: %s", base_url)
                res = await device_manager.prepare_music(base_url)
                await ws.send_json({"type": "music_preparing", "result": res})

            elif msg_type in ("play", "music_play"):
                logger.info("Website client sent 'play' -> starting synchronized playback")
                await device_manager.start_music()
                await ws_manager.broadcast({"type": "music_start"})

            elif msg_type in ("stop", "music_stop"):
                logger.info("Website client sent 'stop' -> stopping synchronized playback")
                await device_manager.stop_music()
                await ws_manager.broadcast({"type": "music_stop"})

            else:
                logger.debug("Unhandled client ws message: '%s'", msg_type)

    except WebSocketDisconnect:
        logger.debug("Website client disconnected.")
    except Exception as exc:
        logger.debug("Website client ws error: %s", exc)
    finally:
        # If this socket owned a session, release the device
        if session_id:
            session = device_manager.current_session
            if session and session.session_id == session_id:
                await device_manager.release_device(session_id, reason="ws_disconnect")
                await ws_manager.broadcast({
                    "type": "device_status",
                    "status": (
                        "AVAILABLE" if device_manager.device_connected else "DISCONNECTED"
                    ),
                    "username": None,
                })

        await ws_manager.disconnect(ws, session_id)
        logger.debug("Website client cleaned up. Total clients: %d", ws_manager.count)


# ---------------------------------------------------------------------------
# Serve frontend static files (index.html, style.css, app.js)
# ---------------------------------------------------------------------------
FRONTEND_DIR = os.path.join(os.path.dirname(__file__), "..", "frontend")

if os.path.isdir(FRONTEND_DIR):
    app.mount(
        "/static",
        StaticFiles(directory=FRONTEND_DIR),
        name="static",
    )

    @app.get("/", include_in_schema=False)
    async def serve_frontend():
        return FileResponse(os.path.join(FRONTEND_DIR, "index.html"))

    @app.get("/{filename}", include_in_schema=False)
    async def serve_static(filename: str):
        path = os.path.join(FRONTEND_DIR, filename)
        if os.path.isfile(path):
            return FileResponse(path)
        raise HTTPException(status_code=404, detail="File not found.")
