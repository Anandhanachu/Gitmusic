"""
github.py - GitHub API integration module.

Fetches contribution calendar data from GitHub's GraphQL API.
Supports a MOCK mode (MOCK_GITHUB=true) for development without credentials.
GitHub tokens are NEVER sent to the frontend or ESP32.
"""

import os
import logging
from datetime import date, timedelta
from typing import Dict, List, Optional

import httpx

from backend.streak import calculate_all_stats

logger = logging.getLogger(__name__)

GITHUB_GRAPHQL_URL = "https://api.github.com/graphql"

# ---------------------------------------------------------------------------
# GraphQL query – fetches contribution calendar for the current year
# ---------------------------------------------------------------------------
CONTRIBUTION_QUERY = """
query($login: String!, $from: DateTime!, $to: DateTime!) {
  user(login: $login) {
    name
    login
    contributionsCollection(from: $from, to: $to) {
      contributionCalendar {
        totalContributions
        weeks {
          contributionDays {
            date
            contributionCount
          }
        }
      }
    }
  }
}
"""


def _get_token() -> Optional[str]:
    """Read GitHub token from environment. Returns None if not set."""
    return os.getenv("GITHUB_TOKEN") or None


def _flatten_weeks(weeks: List[Dict]) -> List[Dict]:
    """Flatten GraphQL weeks → list of contributionDays dicts."""
    days: List[Dict] = []
    for week in weeks:
        days.extend(week.get("contributionDays", []))
    return days


async def fetch_github_stats(username: str) -> Dict:
    """
    Fetch contribution stats for *username* from GitHub GraphQL API.

    Returns a dict compatible with GitHubStats model.
    Raises ValueError for invalid/not-found users.
    Raises RuntimeError for API/network errors.
    """
    token = _get_token()
    if not token:
        raise RuntimeError(
            "GITHUB_TOKEN environment variable is not set. "
            "Set it in your .env file."
        )

    today = date.today()
    # Fetch from Jan 1 of the current year to today
    from_dt = f"{today.year}-01-01T00:00:00Z"
    to_dt = f"{today.isoformat()}T23:59:59Z"

    headers = {
        "Authorization": f"bearer {token}",
        "Content-Type": "application/json",
    }
    payload = {
        "query": CONTRIBUTION_QUERY,
        "variables": {
            "login": username,
            "from": from_dt,
            "to": to_dt,
        },
    }

    async with httpx.AsyncClient(timeout=15.0) as client:
        try:
            resp = await client.post(
                GITHUB_GRAPHQL_URL,
                json=payload,
                headers=headers,
            )
        except httpx.RequestError as exc:
            logger.error("GitHub API request error: %s", exc)
            raise RuntimeError(
                "Unable to reach GitHub API. Check your network connection."
            ) from exc

    if resp.status_code == 401:
        raise RuntimeError(
            "GitHub token is invalid or expired. Update GITHUB_TOKEN."
        )

    if resp.status_code == 403:
        raise RuntimeError(
            "GitHub API rate limit exceeded or access forbidden."
        )

    if resp.status_code != 200:
        raise RuntimeError(
            f"GitHub API returned HTTP {resp.status_code}."
        )

    data = resp.json()

    # Handle GraphQL-level errors
    if "errors" in data:
        errors = data["errors"]
        logger.error("GitHub GraphQL errors: %s", errors)
        msg = errors[0].get("message", "Unknown GraphQL error") if errors else "Unknown error"
        if "Could not resolve to a User" in msg or "user" in msg.lower():
            raise ValueError(f"GitHub user '{username}' not found.")
        raise RuntimeError(f"GitHub API error: {msg}")

    user = data.get("data", {}).get("user")
    if user is None:
        raise ValueError(f"GitHub user '{username}' not found.")

    calendar = (
        user.get("contributionsCollection", {})
            .get("contributionCalendar", {})
    )
    weeks = calendar.get("weeks", [])
    contribution_days = _flatten_weeks(weeks)

    stats = calculate_all_stats(contribution_days, today=today)
    stats["username"] = username
    return stats


# ---------------------------------------------------------------------------
# Mock data for development / testing (MOCK_GITHUB=true)
# ---------------------------------------------------------------------------

MOCK_USERS: Dict[str, Dict] = {
    "demo": {
        "username": "demo",
        "current_streak": 24,
        "longest_streak": 42,
        "today_contributions": 6,
        "total_contributions": 1248,
        "weekly_contributions": 23,
        "monthly_contributions": 87,
    },
    "test": {
        "username": "test",
        "current_streak": 3,
        "longest_streak": 15,
        "today_contributions": 1,
        "total_contributions": 312,
        "weekly_contributions": 7,
        "monthly_contributions": 31,
    },
}


async def fetch_github_stats_mock(username: str) -> Dict:
    """
    Return mock GitHub stats for development mode.
    Any username not in MOCK_USERS gets generic generated data.
    """
    import hashlib
    import asyncio

    # Simulate network latency
    await asyncio.sleep(0.3)

    if username.lower() in MOCK_USERS:
        return dict(MOCK_USERS[username.lower()])

    # Generate deterministic mock data based on username hash
    h = int(hashlib.md5(username.encode()).hexdigest(), 16)
    return {
        "username": username,
        "current_streak": h % 50,
        "longest_streak": (h % 50) + (h % 30),
        "today_contributions": h % 10,
        "total_contributions": h % 2000,
        "weekly_contributions": h % 40,
        "monthly_contributions": h % 150,
    }


async def get_github_stats(username: str) -> Dict:
    """
    Main entry point. Uses mock or real GitHub API based on env var.

    Args:
        username: GitHub username to look up.

    Returns:
        Stats dict compatible with GitHubStats model.
    """
    mock_mode = os.getenv("MOCK_GITHUB", "false").lower() in ("true", "1", "yes")

    if mock_mode:
        logger.info("MOCK mode: returning fake GitHub stats for '%s'", username)
        return await fetch_github_stats_mock(username)

    return await fetch_github_stats(username)
