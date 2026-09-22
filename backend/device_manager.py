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
DEFAULT_SESSION_TIMEOUT = 300  # 5 minutes


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
        logger.info("ESP32 device registered: %s", self.DEVICE_ID)

    async def unregister_device(self) -> None:
        """Called when the ESP32 disconnects."""
        async with self._lock:
            self._device_ws = None
            self._device_connected = False
            # If a session was active, cancel its timeout but keep the session
            # so the website gets notified of the device drop.
        logger.warning("ESP32 device disconnected: %s", self.DEVICE_ID)

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

    async def send_github_update(self, stats: dict) -> bool:
        """
        Send processed GitHub stats to the ESP32.

        Includes an optional music structure placeholder for future use.
        """
        import math

        # Intensity derived from current streak (capped at 1.0)
        streak = stats.get("current_streak", 0)
        intensity = min(1.0, round(streak / 100.0, 3))
        pattern = streak % 7  # maps to 7 musical notes C-B

        message = {
            "type": "github_update",
            "username": stats.get("username", ""),
            "current_streak": stats.get("current_streak", 0),
            "longest_streak": stats.get("longest_streak", 0),
            "today": stats.get("today_contributions", 0),
            "total_contributions": stats.get("total_contributions", 0),
            "weekly_contributions": stats.get("weekly_contributions", 0),
            "monthly_contributions": stats.get("monthly_contributions", 0),
            # Future audio/LED fields – ESP32 will use these later
            "music": {
                "enabled": True,
                "pattern": pattern,   # 0-6 → C D E F G A B
                "intensity": intensity,
            },
        }
        return await self._send_to_device(message)

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
