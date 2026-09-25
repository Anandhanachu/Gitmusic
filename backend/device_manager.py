"""
device_manager.py - Manages the single physical GitMusic ESP32 device.

Key guarantees:
- Only ONE session can own the device at a time.
- asyncio.Lock prevents race conditions when two users connect simultaneously.
- Session timeout releases the device if the website disconnects unexpectedly.
- All state mutations happen inside the lock.
"""

import asyncio
import logging
import os
import uuid
from datetime import datetime, timezone
from typing import Optional

from fastapi import WebSocket

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Configurable timeout (seconds). Override via SESSION_TIMEOUT_SECONDS env var.
# ---------------------------------------------------------------------------
DEFAULT_SESSION_TIMEOUT = 7200  # 2 hours


def _get_timeout() -> int:
    try:
        return int(os.getenv("SESSION_TIMEOUT_SECONDS", DEFAULT_SESSION_TIMEOUT))
    except ValueError:
        return DEFAULT_SESSION_TIMEOUT


class Session:
    """Represents a user's active control session."""

    def __init__(self, session_id: str, username: str, device_id: str):
        self.session_id = session_id
        self.username = username
        self.device_id = device_id
        self.created_at: datetime = datetime.now(timezone.utc)
        self.client_ws: Optional[WebSocket] = None  # owning website WebSocket
        self._timeout_task: Optional[asyncio.Task] = None
        self.stats: dict = {}
        self.levels: list = []
        self.timeline: Optional[dict] = None
        self.audio_path: Optional[str] = None
        self.music_state: str = "IDLE"  # IDLE, PREPARING, READY, PLAYING, STOPPING

    def attach_websocket(self, ws: WebSocket) -> None:
        self.client_ws = ws

    def cancel_timeout(self) -> None:
        if self._timeout_task and not self._timeout_task.done():
            self._timeout_task.cancel()
            self._timeout_task = None


class DeviceManager:
    """
    Singleton-style manager for the one physical GitMusic ESP32.

    Thread safety:
        All public mutating methods acquire self._lock (asyncio.Lock).
        This prevents race conditions when two coroutines call
        acquire_device() concurrently.
    """

    DEVICE_ID = "gitmusic-01"

    def __init__(self) -> None:
        self._lock = asyncio.Lock()

        # WebSocket connection to the ESP32
        self._device_ws: Optional[WebSocket] = None
        self._device_connected: bool = False

        # Current owning session
        self._session: Optional[Session] = None

    # ------------------------------------------------------------------
    # Device connection management (called by ESP32 WebSocket handler)
    # ------------------------------------------------------------------

    async def register_device(self, ws: WebSocket) -> None:
        """Called when the ESP32 connects and registers."""
        async with self._lock:
            self._device_ws = ws
            self._device_connected = True
            # When ESP32 freshly boots or reconnects, release any dead orphaned session
            if self._session is not None:
                logger.info(
                    "Device freshly reconnected: resetting previous session '%s' (user '%s')",
                    self._session.session_id,
                    self._session.username,
                )
                if self._session._timeout_task:
                    self._session._timeout_task.cancel()
                self._session = None
        logger.info("ESP32 device registered: %s", self.DEVICE_ID)

    async def unregister_device(self, ws: Optional[WebSocket] = None) -> bool:
        """Called when the ESP32 disconnects."""
        async with self._lock:
            if ws is not None and self._device_ws is not ws:
                logger.info("Ignoring unregister from stale WebSocket connection.")
                return False
            self._device_ws = None
            self._device_connected = False
            # Clean up active session when physical device disconnects (e.g. during reflashing)
            if self._session is not None:
                if self._session._timeout_task:
                    self._session._timeout_task.cancel()
                self._session = None
        logger.warning("ESP32 device disconnected: %s", self.DEVICE_ID)
        return True

    # ------------------------------------------------------------------
    # Session / device acquisition
    # ------------------------------------------------------------------

    async def acquire_device(
        self,
        username: str,
        client_ws: Optional[WebSocket] = None,
    ) -> Optional[Session]:
        """
        Attempt to acquire the device for *username*.

        Returns a Session on success, or None if the device is unavailable.
        This is the critical section – only one coroutine can be here at once.
        """
        async with self._lock:
            if self._session is not None:
                # Device already owned by another user
                logger.info(
                    "Device busy – '%s' tried to acquire while '%s' owns it.",
                    username,
                    self._session.username,
                )
                return None

            session_id = str(uuid.uuid4()).replace("-", "")[:16]
            session = Session(session_id, username, self.DEVICE_ID)
            if client_ws:
                session.attach_websocket(client_ws)

            self._session = session

        logger.info(
            "Device acquired by '%s' (session=%s)", username, session.session_id
        )

        # Start timeout OUTSIDE the lock to avoid deadlocking the lock inside
        # the timeout coroutine.
        timeout_seconds = _get_timeout()
        session._timeout_task = asyncio.create_task(
            self._session_timeout(session.session_id, timeout_seconds)
        )

        return session

    async def release_device(
        self,
        session_id: str,
        reason: str = "user_disconnect",
    ) -> bool:
        """
        Release device ownership.

        Returns True if released, False if the session_id didn't match.
        """
        async with self._lock:
            if self._session is None:
                return False
            if self._session.session_id != session_id:
                logger.warning(
                    "release_device called with wrong session_id '%s' "
                    "(active: '%s'). Ignoring.",
                    session_id,
                    self._session.session_id,
                )
                return False

            old_session = self._session
            self._session = None

        # Cancel timeout outside lock
        old_session.cancel_timeout()

        logger.info(
            "Device released (session=%s, user=%s, reason=%s)",
            old_session.session_id,
            old_session.username,
            reason,
        )

        # Notify ESP32 that the session ended
        await self._send_to_device({"type": "session_end"})

        return True

    # ------------------------------------------------------------------
    # Session timeout
    # ------------------------------------------------------------------

    async def _session_timeout(self, session_id: str, timeout: int) -> None:
        """Coroutine that fires after *timeout* seconds to release the session."""
        try:
            await asyncio.sleep(timeout)
        except asyncio.CancelledError:
            return  # Normal cancellation when user disconnects cleanly

        logger.info(
            "Session %s timed out after %d seconds.", session_id, timeout
        )

        # Notify the owning website WebSocket before releasing
        async with self._lock:
            if self._session and self._session.session_id == session_id:
                ws = self._session.client_ws
            else:
                ws = None

        if ws:
            try:
                await ws.send_json(
                    {
                        "type": "session_timeout",
                        "message": "Your session expired. The device has been released.",
                    }
                )
            except Exception:
                pass

        await self.release_device(session_id, reason="timeout")

    # ------------------------------------------------------------------
    # Sending data to the ESP32
    # ------------------------------------------------------------------

    async def _send_to_device(self, message: dict) -> bool:
        """Send a JSON message to the connected ESP32. Returns True on success."""
        if not self._device_connected or self._device_ws is None:
            logger.warning("Cannot send to device – not connected.")
            return False
        try:
            await self._device_ws.send_json(message)
            return True
        except Exception as exc:
            logger.error("Failed to send message to device: %s", exc)
            return False

    def _get_lan_ip(self) -> str:
        import socket
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "172.20.10.10"

    async def send_github_update(self, stats: dict) -> bool:
        """
        Send processed GitHub stats to the ESP32.

        Includes real 52-week contribution grid (levels[52][7]) so the ESP32
        can display the authentic GitHub contribution graph without guessing.
        """
        from datetime import date, timedelta

        def count_to_level(count: int) -> int:
            if count == 0: return 0
            if count <= 3: return 1
            if count <= 6: return 2
            if count <= 9: return 3
            return 4

        raw_days = stats.get("raw_days", [])
        date_map: dict = {}
        for entry in raw_days:
            try:
                d = date.fromisoformat(entry["date"])
                date_map[d] = int(entry.get("count", 0))
            except Exception:
                pass

        today = date.today()
        days_since_sunday = (today.weekday() + 1) % 7
        current_week_sunday = today - timedelta(days=days_since_sunday)
        grid_start = current_week_sunday - timedelta(weeks=51)

        levels = [[0] * 7 for _ in range(52)]
        counts = [[0] * 7 for _ in range(52)]
        for col in range(52):
            week_sunday = grid_start + timedelta(weeks=col)
            for row in range(7):
                day = week_sunday + timedelta(days=row)
                if day > today:
                    levels[col][row] = 0
                    counts[col][row] = 0
                else:
                    count = date_map.get(day, 0)
                    counts[col][row] = count
                    levels[col][row] = count_to_level(count)

        flat_levels = [levels[c][r] for c in range(52) for r in range(7)]

        streak = stats.get("current_streak", 0)
        intensity = min(1.0, round(streak / 100.0, 3))
        pattern = streak % 7

        message = {
            "type": "github_update",
            "username": stats.get("username", ""),
            "current_streak": stats.get("current_streak", 0),
            "longest_streak": stats.get("longest_streak", 0),
            "today": stats.get("today_contributions", 0),
            "total_contributions": stats.get("total_contributions", 0),
            "weekly_contributions": stats.get("weekly_contributions", 0),
            "monthly_contributions": stats.get("monthly_contributions", 0),
            "levels": flat_levels,
            "levels_str": "".join(str(lvl) for lvl in flat_levels),
            "today_row": days_since_sunday,
            "music": {
                "enabled": True,
                "pattern": pattern,
                "intensity": intensity,
            },
        }

        # Cache levels, counts, and pre-render piano composition in background
        if self._session:
            self._session.stats = stats
            self._session.levels = levels
            self._session.counts = counts
            try:
                from composer import compose_from_github
                from piano_synth import render_composition_to_wav
                timeline = compose_from_github(
                    levels,
                    stats.get("current_streak", 0),
                    counts_grid=counts,
                    username=stats.get("username", "")
                )
                self._session.timeline = timeline
                cache_dir = os.path.join(os.path.dirname(__file__), "audio_cache")
                wav_path = os.path.join(cache_dir, f"{self._session.session_id}.wav")
                render_composition_to_wav(timeline, wav_path)
                self._session.audio_path = wav_path
                logger.info(
                    "Pre-rendered piano audio for session '%s' (active_weeks=%d, bpm=%d, duration=%d ms)",
                    self._session.session_id,
                    timeline.get("active_week_count", 0),
                    timeline.get("bpm", 0),
                    timeline.get("duration_ms", 0),
                )
            except Exception as exc:
                logger.error("Error pre-rendering piano composition: %s", exc)

        ok = await self._send_to_device(message)

        # Proactively prepare music on ESP32 so it is armed immediately
        try:
            await self.prepare_music("http://localhost:8000")
        except Exception as exc:
            logger.warning("Auto-prepare music exception: %s", exc)

        return ok

    async def prepare_music(self, base_url: str) -> dict:
        """
        Initiates the music preparation handshake with ESP32.
        Renders/loads audio & timeline, sends prepare_music command to ESP32.
        """
        if not self._device_connected or self._session is None:
            return {"success": False, "error": "DEVICE_NOT_CONNECTED", "message": "ESP32 not connected"}

        # Ensure timeline and audio exist
        if not self._session.timeline or not self._session.audio_path or not os.path.exists(self._session.audio_path):
            from composer import compose_from_github
            from piano_synth import render_composition_to_wav
            counts = getattr(self._session, "counts", self._session.levels)
            timeline = compose_from_github(
                self._session.levels,
                self._session.stats.get("current_streak", 0),
                counts_grid=counts,
                username=self._session.username
            )
            self._session.timeline = timeline
            cache_dir = os.path.join(os.path.dirname(__file__), "audio_cache")
            wav_path = os.path.join(cache_dir, f"{self._session.session_id}.wav")
            render_composition_to_wav(timeline, wav_path)
            self._session.audio_path = wav_path

        self._session.music_state = "PREPARING"

        # Format compact timeline for ESP32
        esp_timeline = [
            {
                "time": ev["time"],
                "week": ev["week"],
                "day": ev["day"],
                "day_mask": ev.get("day_mask", 1 << ev["day"]),
                "velocity": ev["velocity"]
            }
            for ev in self._session.timeline.get("events", [])
        ]

        lan_ip = self._get_lan_ip()
        audio_url = f"http://{lan_ip}:8000/api/music/{self._session.session_id}/audio.wav"

        message = {
            "type": "prepare_music",
            "audio_url": audio_url,
            "duration_ms": self._session.timeline.get("duration_ms", 29538),
            "timeline": esp_timeline
        }

        ok = await self._send_to_device(message)
        return {
            "success": ok,
            "state": "PREPARING",
            "duration_ms": self._session.timeline.get("duration_ms", 29538),
            "timeline": self._session.timeline,
            "audio_url": f"/api/music/{self._session.session_id}/audio.wav"
        }

    async def on_device_ready(self) -> None:
        """Called when ESP32 reports that it has buffered audio and is READY."""
        if self._session:
            self._session.music_state = "READY"
        logger.info("ESP32 reported music READY")

    async def start_music(self) -> bool:
        """Sends START command to ESP32."""
        if self._session:
            self._session.music_state = "PLAYING"
            lan_ip = self._get_lan_ip()
            audio_url = f"http://{lan_ip}:8000/api/music/{self._session.session_id}/audio.wav"
            return await self._send_to_device({
                "type": "music_start",
                "audio_url": audio_url,
                "duration_ms": self._session.timeline.get("duration_ms", 29538) if self._session.timeline else 29538,
            })
        return await self._send_to_device({"type": "music_start"})

    async def stop_music(self) -> bool:
        """Sends STOP command to ESP32."""
        if self._session:
            self._session.music_state = "IDLE"
        return await self._send_to_device({"type": "music_stop"})

    async def play_tone(self) -> bool:
        """Send a play_tone command to the connected ESP32."""
        return await self._send_to_device({"type": "play_tone"})

    # ------------------------------------------------------------------
    # Status helpers (read-only – no lock needed for simple reads)
    # ------------------------------------------------------------------

    @property
    def device_connected(self) -> bool:
        return self._device_connected

    @property
    def is_available(self) -> bool:
        return self._device_connected and self._session is None

    @property
    def current_session(self) -> Optional[Session]:
        return self._session

    def get_status_dict(self) -> dict:
        """Return a serialisable status dictionary."""
        if not self._device_connected:
            status = "DISCONNECTED"
        elif self._session is not None:
            status = "BUSY"
        else:
            status = "AVAILABLE"

        return {
            "device_id": self.DEVICE_ID,
            "status": status,
            "current_user": self._session.username if self._session else None,
            "session_id": self._session.session_id if self._session else None,
        }


# ---------------------------------------------------------------------------
# Module-level singleton – import and use this everywhere.
# ---------------------------------------------------------------------------
device_manager = DeviceManager()
