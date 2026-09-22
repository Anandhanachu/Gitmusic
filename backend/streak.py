"""
streak.py - GitHub contribution streak calculation module.

Handles:
- Current streak (consecutive days from latest date)
- Longest streak (all-time best)
- Today's contributions
- Total, weekly, monthly contribution counts
- Timezone-aware date handling
- Year boundaries, leap years, missing dates
"""

from datetime import date, timedelta
from typing import Dict, List, Optional, Tuple
import logging

logger = logging.getLogger(__name__)


def parse_contributions(
    contribution_days: List[Dict],
) -> Dict[date, int]:
    """
    Parse contribution calendar days into a dict of {date: count}.

    Args:
        contribution_days: List of dicts with 'date' (str YYYY-MM-DD) and
                           'contributionCount' (int) from GitHub GraphQL API.

    Returns:
        Dictionary mapping date objects to contribution counts.
    """
    result: Dict[date, int] = {}
    for day in contribution_days:
        try:
            d = date.fromisoformat(day["date"])
            result[d] = int(day.get("contributionCount", 0))
        except (KeyError, ValueError, TypeError) as e:
            logger.warning("Skipping invalid contribution day entry %s: %s", day, e)
    return result


def calculate_current_streak(
    contributions: Dict[date, int],
    today: Optional[date] = None,
) -> int:
    """
    Calculate the current contribution streak.

    The streak is the number of consecutive days (ending today or yesterday)
    where the user had at least one contribution.

    If the user has not contributed today but has an ongoing streak from
    yesterday, the streak is still valid.  If they have not contributed
    today OR yesterday the streak is 0.

    Args:
        contributions: Mapping of date -> contribution count.
        today: Override for "today" (useful in tests). Defaults to date.today().

    Returns:
        Current streak length in days.
    """
    if not contributions:
        return 0

    if today is None:
        today = date.today()

    # Determine the starting point: today if they contributed, else yesterday.
    if contributions.get(today, 0) > 0:
        check_date = today
    else:
        check_date = today - timedelta(days=1)

    # Walk backwards while there are contributions.
    streak = 0
    while contributions.get(check_date, 0) > 0:
        streak += 1
        check_date -= timedelta(days=1)

    return streak


def calculate_longest_streak(contributions: Dict[date, int]) -> int:
    """
    Calculate the all-time longest contribution streak.

    Args:
        contributions: Mapping of date -> contribution count.

    Returns:
        Longest streak length in days.
    """
    if not contributions:
        return 0

    sorted_dates = sorted(contributions.keys())
    longest = 0
    current = 0
    prev_date: Optional[date] = None

    for d in sorted_dates:
        if contributions[d] > 0:
            if prev_date is not None and (d - prev_date).days == 1:
                current += 1
            else:
                current = 1
            longest = max(longest, current)
        else:
            current = 0
        prev_date = d

    return longest


def today_contributions(
    contributions: Dict[date, int],
    today: Optional[date] = None,
) -> int:
    """Return the number of contributions made today."""
    if today is None:
        today = date.today()
    return contributions.get(today, 0)


def total_contributions(contributions: Dict[date, int]) -> int:
    """Return the sum of all contributions across all dates."""
    return sum(contributions.values())


def weekly_contributions(
    contributions: Dict[date, int],
    today: Optional[date] = None,
) -> int:
    """Return the total contributions in the last 7 days (including today)."""
    if today is None:
        today = date.today()
    total = 0
    for i in range(7):
        d = today - timedelta(days=i)
        total += contributions.get(d, 0)
    return total


def monthly_contributions(
    contributions: Dict[date, int],
    today: Optional[date] = None,
) -> int:
    """Return the total contributions in the last 30 days (including today)."""
    if today is None:
        today = date.today()
    total = 0
    for i in range(30):
        d = today - timedelta(days=i)
        total += contributions.get(d, 0)
    return total


def calculate_all_stats(
    contribution_days: List[Dict],
    today: Optional[date] = None,
) -> Dict:
    """
    Master function – parse raw GitHub contribution data and return all stats.

    Args:
        contribution_days: Raw list from GitHub GraphQL
                           (each item has 'date' and 'contributionCount').
        today: Override for "today" (useful in tests).

    Returns:
        Dict with keys:
            current_streak, longest_streak, today_contributions,
            total_contributions, weekly_contributions, monthly_contributions
    """
    if today is None:
        today = date.today()

    contrib_map = parse_contributions(contribution_days)

    return {
        "current_streak": calculate_current_streak(contrib_map, today),
        "longest_streak": calculate_longest_streak(contrib_map),
        "today_contributions": today_contributions(contrib_map, today),
        "total_contributions": total_contributions(contrib_map),
        "weekly_contributions": weekly_contributions(contrib_map, today),
        "monthly_contributions": monthly_contributions(contrib_map, today),
    }
