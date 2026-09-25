"""
composer.py – Transforms GitHub contribution calendar data into an
authoritative, week-by-week piano composition and event timeline.

Design principles (per Master Specification):
 1. Exactly ONE musical tone per active week:
    - Calendar consists of 52 weeks x 7 days.
    - Each active week produces a SINGLE musical tone.
    - Inactive weeks (total contributions == 0) produce NO tone, NO note, NO highlight.
 2. Simultaneous highlighted days:
    - For that single week's tone, randomly selected LEDs from that week's
      contribution days (> 0 commits) are highlighted SIMULTANEOUSLY.
    - highlightCount = random(1, len(activeDays)).
    - Never selects zero-contribution days.
 3. Dynamic BPM scaling based on active weeks:
    - BPM = min(55 + (activeWeekCount - 1) * 3, 100).
    - Few active weeks = slower tempo; more active weeks = faster tempo.
 4. Deterministic Randomness:
    - Seeded by username + contribution data so the profile produces a consistent,
      recognizable musical piece every time.
 5. 7 Diatonic Natural Notes & Melodic Structure:
    - Based on C, D, E, F, G, A, B across octaves 3 to 5.
    - Max contribution count and level in the week drive pitch, octave, velocity, and duration.
    - Smooth voice leading between consecutive active weeks.
 6. Authoritative timeline:
    - Shared identically by Backend audio renderer, Frontend visualizer, and ESP32 LEDs.
"""

import random
from typing import List, Dict, Any, Optional

# 7-Note Natural Pitch Frequencies (12-TET, A4 = 440 Hz)
NOTE_FREQS = {
    "C2": 65.41,  "D2": 73.42,  "E2": 82.41,  "F2": 87.31,  "G2": 98.00,  "A2": 110.00, "B2": 123.47,
    "C3": 130.81, "D3": 146.83, "E3": 164.81, "F3": 174.61, "G3": 196.00, "A3": 220.00, "B3": 246.94,
    "C4": 261.63, "D4": 293.66, "E4": 329.63, "F4": 349.23, "G4": 392.00, "A4": 440.00, "B4": 493.88,
    "C5": 523.25, "D5": 587.33, "E5": 659.25, "F5": 698.46, "G5": 783.99, "A5": 880.00, "B5": 987.77,
    "C6": 1046.50
}

# Diatonic pitch ordering for interval / voice-leading calculations
DIATONIC_PITCHES = [
    "C3", "D3", "E3", "F3", "G3", "A3", "B3",
    "C4", "D4", "E4", "F4", "G4", "A4", "B4",
    "C5", "D5", "E5", "F5", "G5", "A5", "B5",
    "C6"
]
PITCH_TO_INDEX = {p: i for i, p in enumerate(DIATONIC_PITCHES)}

# Harmonic themes cycling through consecutive active weeks (connected musical narrative)
CHORD_THEMES = [
    {"name": "C_maj",  "palette": ["C4", "E4", "G4", "C5", "E5"]},
    {"name": "A_min",  "palette": ["A3", "C4", "E4", "A4", "C5"]},
    {"name": "F_maj",  "palette": ["F3", "A3", "C4", "F4", "A4"]},
    {"name": "G_maj",  "palette": ["G3", "B3", "D4", "G4", "B4"]},
    {"name": "E_min",  "palette": ["E3", "G3", "B3", "E4", "G4"]},
    {"name": "D_min",  "palette": ["D3", "F3", "A3", "D4", "F4"]},
    {"name": "G_cad",  "palette": ["G3", "B3", "D4", "F4", "G4"]},
    {"name": "C_res",  "palette": ["C4", "E4", "G4", "C5", "E5"]},
]


def calculate_bpm(active_week_count: int) -> int:
    """
    Calculates tempo dynamically based on the number of active contribution weeks.
    Few active weeks -> slow piano (55 BPM).
    More active weeks -> faster piano (up to 100 BPM).
    """
    if active_week_count <= 0:
        return 0
    MIN_BPM = 55
    MAX_BPM = 100
    bpm = MIN_BPM + (active_week_count - 1) * 3
    return min(bpm, MAX_BPM)


def compose_from_github(
    levels_grid: List[List[int]],
    current_streak: int = 0,
    counts_grid: Optional[List[List[int]]] = None,
    username: str = ""
) -> Dict[str, Any]:
    """
    Transforms GitHub contribution data into a week-by-week piano composition.
    Exactly ONE tone plays for each active week.
    For that single tone, randomly selected active days from that week highlight SIMULTANEOUSLY.

    levels_grid: 52 columns (weeks 0..51), each with 7 rows (days 0..6, Sun..Sat).
    counts_grid: Optional actual contribution counts grid (52x7). Defaults to levels_grid.
    current_streak: User's current commit streak.
    username: GitHub username used for deterministic seeded randomness.
    """
    if counts_grid is None:
        counts_grid = levels_grid

    num_weeks = min(52, len(levels_grid), len(counts_grid))

    # 1. Identify active weeks (total contributions > 0)
    # Inactive weeks (total == 0) produce NO music, NO notes, and NO highlights
    active_weeks = []
    for w in range(num_weeks):
        col_counts = counts_grid[w] if w < len(counts_grid) else [0] * 7
        col_levels = levels_grid[w] if w < len(levels_grid) else [0] * 7
        w_total = sum(col_counts) or sum(col_levels)
        if w_total > 0:
            active_weeks.append(w)

    active_week_count = len(active_weeks)

    # Zero contribution profile: no music, no highlight events
    if active_week_count == 0:
        return {
            "bpm": 0,
            "tempo": 0,
            "duration_ms": 0,
            "active_week_count": 0,
            "events": []
        }

    # 2. Dynamic BPM calculation based on number of active weeks
    bpm = calculate_bpm(active_week_count)
    beat_ms = int(60000 / bpm)

    # 3. Deterministic Seed based on username + contribution fingerprint
    total_commits = sum(sum(counts_grid[w]) for w in range(num_weeks))
    seed_str = f"{username}:{active_week_count}:{total_commits}:{current_streak}"
    rng = random.Random(seed_str)

    events = []
    current_time_ms = 0
    last_note_pitch = "C4"

    # Tone duration per week: 90% of beat duration, leaving 10% breathing room
    tone_dur_ms = int(beat_ms * 0.90)

    # 4. Generate exactly ONE musical tone per active week
    for active_idx, w in enumerate(active_weeks):
        col_counts = counts_grid[w] if w < len(counts_grid) else [0] * 7
        col_levels = levels_grid[w] if w < len(levels_grid) else [0] * 7

        # Find all days in this week that have AT LEAST ONE contribution
        active_days = [d for d in range(7) if (col_counts[d] > 0 or col_levels[d] > 0)]

        if not active_days:
            continue

        # Randomly select between 1 and len(active_days) days — ONLY from contribution days
        highlight_count = rng.randint(1, len(active_days))
        # Unique selection without replacement (Fisher-Yates shuffle sample)
        selected_days = sorted(rng.sample(active_days, highlight_count))

        # Bitmask for ESP32 representing all selected days simultaneously
        day_mask = sum(1 << d for d in selected_days)

        # Musical theme for this active week
        theme = CHORD_THEMES[active_idx % len(CHORD_THEMES)]
        palette = theme["palette"]

        # Max contribution count and level among selected days
        max_cnt = max(col_counts[d] for d in selected_days)
        max_lvl = max(col_levels[d] for d in selected_days)
        if max_lvl <= 0:
            max_lvl = 1

        # Contribution intensity influences palette tone selection
        target_palette_idx = min(len(palette) - 1, max_lvl - 1)
        candidate_note = palette[target_palette_idx]

        # Voice leading: ensure smooth transitions from previous active week
        last_idx = PITCH_TO_INDEX.get(last_note_pitch, 7)
        best_note = candidate_note
        best_dist = 999
        for p_note in palette:
            p_idx = PITCH_TO_INDEX.get(p_note, 7)
            dist = abs(p_idx - last_idx)
            score = dist + (2 if p_note != candidate_note else 0)
            if score < best_dist:
                best_dist = score
                best_note = p_note

        selected_note = best_note
        last_note_pitch = selected_note

        # Dynamics: velocity 60..116 based on contribution intensity
        vel = 58 + min(max_lvl * 10, 38) + min(int(max_cnt * 1.5), 18)
        vel = max(55, min(118, vel))

        # Exactly ONE event for this week: all selected days highlight SIMULTANEOUSLY during this tone!
        events.append({
            "time": current_time_ms,
            "week": w,
            "days": selected_days,           # list of days highlighted simultaneously
            "day": selected_days[0],         # primary day for legacy readers
            "day_mask": day_mask,            # 7-bit mask for ESP32
            "note": selected_note,
            "freq": NOTE_FREQS.get(selected_note, 261.63),
            "duration": tone_dur_ms,
            "velocity": vel,
            "contribution": max_cnt,
            "level": max_lvl,
            "led": w * 7 + selected_days[0],
        })

        # Advance timeline by 1 beat for the next active week
        current_time_ms += beat_ms

    # Final gentle tail for acoustic piano ring-out before seamless loop
    total_duration_ms = current_time_ms + int(beat_ms * 0.8)

    # Sort events strictly by timestamp for sequencer predictability
    events.sort(key=lambda e: e["time"])

    return {
        "bpm": bpm,
        "tempo": bpm,
        "duration_ms": total_duration_ms,
        "active_week_count": active_week_count,
        "events": events
    }
