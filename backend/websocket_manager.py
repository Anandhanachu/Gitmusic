"""
websocket_manager.py - Manages all active website client WebSocket connections.

Allows the backend to broadcast device-status updates to every connected
browser so that ALL visitors see real-time device availability changes.
"""

import asyncio
import logging
from typing import Dict, Set

from fastapi import WebSocket

logger = logging.getLogger(__name__)


class WebSocketManager:
    """
    Keeps track of all connected website client WebSockets.
    Provides broadcast and targeted send utilities.
    """

    def __init__(self) -> None:
        # Maps session_id → WebSocket for identified sessions
        self._sessions: Dict[str, WebSocket] = {}
        # All connected sockets (including pre-session connections)
        self._all: Set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def connect(self, ws: WebSocket, session_id: str = "") -> None:
        """Register a new client WebSocket connection."""
        async with self._lock:
            self._all.add(ws)
            if session_id:
                self._sessions[session_id] = ws
        logger.debug("Client WS connected. Total: %d", len(self._all))

    async def disconnect(self, ws: WebSocket, session_id: str = "") -> None:
        """Remove a client WebSocket connection."""
        async with self._lock:
            self._all.discard(ws)
            if session_id and session_id in self._sessions:
                del self._sessions[session_id]
        logger.debug("Client WS disconnected. Total: %d", len(self._all))

    async def broadcast(self, message: dict) -> None:
        """Send *message* to all connected website clients."""
        dead: Set[WebSocket] = set()
        # Snapshot to avoid mutation during iteration
        async with self._lock:
            snapshot = set(self._all)

        for ws in snapshot:
            try:
                await ws.send_json(message)
            except Exception as exc:
                logger.debug("Broadcast failed for a client ws: %s", exc)
                dead.add(ws)

        if dead:
            async with self._lock:
                self._all -= dead

    async def send_to_session(self, session_id: str, message: dict) -> bool:
        """Send *message* to the WebSocket owning *session_id*."""
        async with self._lock:
            ws = self._sessions.get(session_id)
        if ws is None:
            return False
        try:
            await ws.send_json(message)
            return True
        except Exception as exc:
            logger.warning("Failed to send to session %s: %s", session_id, exc)
            return False

    @property
    def count(self) -> int:
        return len(self._all)


# Module-level singleton
ws_manager = WebSocketManager()
