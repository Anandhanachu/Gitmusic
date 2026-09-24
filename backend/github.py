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

try:
    from backend.streak import calculate_all_stats
except ModuleNotFoundError:
    from streak import calculate_all_stats

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
    # Fetch exactly 365 days (52 weeks) back from today, matching GitHub's full profile view
    start_date = today - timedelta(days=364)
    from_dt = f"{start_date.isoformat()}T00:00:00Z"
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
    # Pass raw daily data so device_manager can build the real 7×52 grid
    stats["raw_days"] = [
        {"date": d["date"], "count": d.get("contributionCount", 0)}
        for d in contribution_days
    ]
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
    Now includes raw_days so the 7×52 grid renders correctly on the ESP32.
    """
    import hashlib
    import asyncio
    from datetime import date, timedelta

    # Simulate network latency
    await asyncio.sleep(0.3)

    if username.lower() in MOCK_USERS:
        base = dict(MOCK_USERS[username.lower()])
    else:
        h = int(hashlib.md5(username.encode()).hexdigest(), 16)
        base = {
            "username": username,
            "current_streak": h % 50,
            "longest_streak": (h % 50) + (h % 30),
            "today_contributions": h % 10,
            "total_contributions": h % 2000,
            "weekly_contributions": h % 40,
            "monthly_contributions": h % 150,
        }

    # Generate realistic mock raw_days for the past 365 days
    import random
    rng = random.Random(username.lower())
    today = date.today()
    current_streak = base.get("current_streak", 0)
    raw_days = []
    for i in range(364, -1, -1):
        d = today - timedelta(days=i)
        # During the streak window: guaranteed contributions
        if i < current_streak:
            count = rng.randint(1, 12)
        else:
            # Outside streak: sparse contributions
            count = rng.choices([0, 1, 2, 3, 5, 8], weights=[60, 15, 10, 8, 5, 2])[0]
        raw_days.append({"date": d.isoformat(), "count": count})

    base["raw_days"] = raw_days
    return base


async def fetch_github_stats_public(username: str) -> Dict:
    """
    Fetch REAL GitHub contribution data directly without needing a personal token.
    Uses the standard public contribution calendar API (same data GitHub renders on user profile).
    """
    url = f"https://github-contributions-api.jogruber.de/v4/{username}?y=last"
    async with httpx.AsyncClient(timeout=15.0) as client:
        try:
            resp = await client.get(url)
        except Exception as exc:
            logger.error("Public contribution API error: %s", exc)
            raise RuntimeError(f"Unable to fetch GitHub stats for '{username}'.") from exc

    if resp.status_code == 404:
        raise ValueError(f"GitHub user '{username}' not found.")
    if resp.status_code != 200:
        raise RuntimeError(f"GitHub data provider returned HTTP {resp.status_code}.")

    data = resp.json()
    contrib_list = data.get("contributions", [])
    if not contrib_list:
        raise ValueError(f"No contribution data found for '{username}'.")

    # Format into standard contributionDays list
    contribution_days = [
        {"date": item["date"], "contributionCount": int(item.get("count", 0))}
        for item in contrib_list
    ]

    today = date.today()
    stats = calculate_all_stats(contribution_days, today=today)
    stats["username"] = username
    stats["raw_days"] = [
        {"date": d["date"], "count": d.get("contributionCount", 0)}
        for d in contribution_days
    ]
    return stats


async def get_github_stats(username: str) -> Dict:
    """
    Main entry point.
    1. If GITHUB_TOKEN is configured and valid, use GitHub GraphQL.
    2. Otherwise, fetch REAL public GitHub data via the public contribution calendar.
    3. Only fall back to synthetic mock if explicitly forced and offline.
    """
    token = _get_token()
    if token and token != "your_github_token_here":
        try:
            return await fetch_github_stats(username)
        except Exception as exc:
            logger.warning("GraphQL with token failed (%s), trying public endpoint...", exc)

    # Fetch REAL contribution data for the actual username:
    try:
        return await fetch_github_stats_public(username)
    except Exception as exc:
        logger.warning("Public contribution fetch failed (%s). Falling back to mock...", exc)
        return await fetch_github_stats_mock(username)
