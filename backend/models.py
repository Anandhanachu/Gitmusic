"""
models.py - Pydantic models / data-transfer objects used across the backend.
"""

from enum import Enum
from typing import Optional
from pydantic import BaseModel, Field


# ---------------------------------------------------------------------------
# Device state
# ---------------------------------------------------------------------------

class DeviceStatus(str, Enum):
    AVAILABLE = "AVAILABLE"
    BUSY = "BUSY"
    DISCONNECTED = "DISCONNECTED"


class DeviceState(BaseModel):
    device_id: str
    status: DeviceStatus
    current_user: Optional[str] = None
    session_id: Optional[str] = None


# ---------------------------------------------------------------------------
# REST request / response bodies
# ---------------------------------------------------------------------------

class ConnectRequest(BaseModel):
    username: str = Field(..., min_length=1, max_length=39)


class ConnectResponse(BaseModel):
    success: bool
    session_id: Optional[str] = None
    username: Optional[str] = None
    error: Optional[str] = None
    message: Optional[str] = None
    stats: Optional[dict] = None


class DisconnectRequest(BaseModel):
    session_id: str


class DisconnectResponse(BaseModel):
    success: bool
    message: Optional[str] = None


# ---------------------------------------------------------------------------
# GitHub stats payload
# ---------------------------------------------------------------------------

class GitHubStats(BaseModel):
    username: str
    current_streak: int = 0
    longest_streak: int = 0
    today_contributions: int = 0
    total_contributions: int = 0
    weekly_contributions: int = 0
    monthly_contributions: int = 0


# ---------------------------------------------------------------------------
# WebSocket message types (used as "type" field discriminator)
# ---------------------------------------------------------------------------

class WSMessageType(str, Enum):
    # Device → Backend
    DEVICE_REGISTER = "device_register"

    # Backend → Device
    DEVICE_REGISTERED = "device_registered"
    GITHUB_UPDATE = "github_update"
    SESSION_END = "session_end"
    PING = "ping"

    # Backend → Client (website)
    DEVICE_STATUS = "device_status"
    CONNECT_SUCCESS = "connect_success"
    CONNECT_FAILED = "connect_failed"
    STATS_UPDATE = "stats_update"
    SESSION_TIMEOUT = "session_timeout"
    ERROR = "error"

    # Client → Backend
    CLIENT_CONNECT = "client_connect"
    CLIENT_DISCONNECT = "client_disconnect"
    CLIENT_PING = "client_ping"
