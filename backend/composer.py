"""
composer.py – Transforms GitHub contribution calendar data into an
authoritative, week-based, dynamic-tempo piano composition and event timeline.

Design principles (per Master Specification):
 1. Week-based music:
    - Calendar consists of 52 weeks x 7 days.
    - Each active week generates its own coherent musical section / phrase.
    - INACTIVE WEEKS (week contribution total == 0) produce NO music, NO notes, NO highlights.
 2. Dynamic BPM scaling:
    - activeWeekCount controls tempo:
      bpm = MIN_BPM (55) + (activeWeekCount - 1) * 3, capped at MAX_BPM (100).
      More active weeks = faster music; fewer active weeks = slower music.
 3. Random selection ONLY from contribution days:
    - For each active week:
      activeDays = [d for d in week if d.contribution > 0]
      highlightCount = random(1, len(activeDays))
      selectedDays = unique random sample from activeDays (Fisher-Yates / sample without replacement).
      Never selects zero-contribution days!
 4. Deterministic Randomness:
    - Seeded by username + contribution data so the same profile produces a consistent,
      recognizable musical piece every time.
 5. 7 Diatonic Natural Notes & Melodic Structure:
    - Based on C, D, E, F, G, A, B across octaves 3 to 5.
    - Contribution count influences pitch, octave, velocity, and duration.
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

    # 4. Generate week-by-week musical phrases
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

        # Musical theme for this active week
        theme = CHORD_THEMES[active_idx % len(CHORD_THEMES)]
        palette = theme["palette"]

        # Note duration based on number of events in this week's phrase
        num_selected = len(selected_days)
        if num_selected == 1:
            note_dur_ms = int(beat_ms * 1.8)
            step_gap_ms = int(beat_ms * 1.5)
        elif num_selected == 2:
            note_dur_ms = int(beat_ms * 1.3)
            step_gap_ms = int(beat_ms * 1.1)
        elif num_selected == 3:
            note_dur_ms = int(beat_ms * 1.0)
            step_gap_ms = int(beat_ms * 0.9)
        else:
            note_dur_ms = int(beat_ms * 0.85)
            step_gap_ms = int(beat_ms * 0.75)

        phrase_start_ms = current_time_ms

        for note_idx, d in enumerate(selected_days):
            cnt = col_counts[d] if d < len(col_counts) else 0
            lvl = col_levels[d] if d < len(col_levels) else 1
            if lvl <= 0:
                lvl = 1

            # Contribution count influences pitch, octave, and dynamics
            # Map level (1..4) and count to palette selection with voice leading
            target_palette_idx = min(len(palette) - 1, (lvl - 1) + (note_idx % 2))
            candidate_note = palette[target_palette_idx]

            # Voice leading: ensure smooth transitions (prefer notes within a 5th of last_note_pitch)
            last_idx = PITCH_TO_INDEX.get(last_note_pitch, 7)
            # Find palette note closest to last_idx while honoring contour
            best_note = candidate_note
            best_dist = 999
            for p_note in palette:
                p_idx = PITCH_TO_INDEX.get(p_note, 7)
                dist = abs(p_idx - last_idx)
                # Bias toward candidate_note while minimizing excessive leap
                score = dist + (2 if p_note != candidate_note else 0)
                if score < best_dist:
                    best_dist = score
                    best_note = p_note

            selected_note = best_note
            last_note_pitch = selected_note

            # Dynamics: velocity 60..115 based on level and contribution count
            vel = 58 + min(lvl * 10, 38) + min(int(cnt * 1.5), 18)
            vel = max(55, min(118, vel))

            event_time = phrase_start_ms + (note_idx * step_gap_ms)

            events.append({
                "time": event_time,
                "week": w,
                "day": d,
                "contribution": cnt,
                "level": lvl,
                "note": selected_note,
                "freq": NOTE_FREQS.get(selected_note, 261.63),
                "duration": note_dur_ms,
                "velocity": vel,
                "led": w * 7 + d,
            })

        # Advance timeline for the week's phrase plus a natural breathing rest
        phrase_len_ms = (num_selected * step_gap_ms) + int(beat_ms * 0.45)
        current_time_ms = phrase_start_ms + phrase_len_ms

    # Final gentle tail for acoustic piano ring-out before seamless loop
    total_duration_ms = current_time_ms + int(beat_ms * 1.2)

    # Sort events strictly by timestamp for sequencer predictability
    events.sort(key=lambda e: e["time"])

    return {
        "bpm": bpm,
        "tempo": bpm,
        "duration_ms": total_duration_ms,
        "active_week_count": active_week_count,
        "events": events
    }
