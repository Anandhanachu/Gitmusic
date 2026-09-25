"""
composer.py – Transforms GitHub contribution calendar data into an
authoritative, slow, ambient piano composition and event timeline.

Design principles:
 - Tempo: 65 BPM (~923 ms per beat). Calm, gentle, reflective ambient piano.
 - Pitch system: 7 natural diatonic notes (C, D, E, F, G, A, B) across octaves 3 to 5.
 - Musical structure: 8 bars of 4/4 (~29.5 seconds) in 4 coherent phrases:
     * Phrase 1 (Bars 1-2): Opening motif (C Major -> A minor)
     * Phrase 2 (Bars 3-4): Answering phrase (F Major -> G Major)
     * Phrase 3 (Bars 5-6): Development & peak (A minor -> F Major)
     * Phrase 4 (Bars 7-8): Cadence & gentle resolution (C/G -> C)
 - Deterministic: Same GitHub data always produces the exact same piece.
 - Maps contribution levels (0-4) to musical velocity, duration, and octave selection.
 - Authoritative timeline: Shared identically by Backend, Frontend visualizer, and ESP32.
"""

from typing import List, Dict, Any
import math

# 7-Note Natural Pitch Frequencies (12-TET, A4 = 440 Hz)
NOTE_FREQS = {
    "C2": 65.41,  "D2": 73.42,  "E2": 82.41,  "F2": 87.31,  "G2": 98.00,  "A2": 110.00, "B2": 123.47,
    "C3": 130.81, "D3": 146.83, "E3": 164.81, "F3": 174.61, "G3": 196.00, "A3": 220.00, "B3": 246.94,
    "C4": 261.63, "D4": 293.66, "E4": 329.63, "F4": 349.23, "G4": 392.00, "A4": 440.00, "B4": 493.88,
    "C5": 523.25, "D5": 587.33, "E5": 659.25, "F5": 698.46, "G5": 783.99, "A5": 880.00, "B5": 987.77,
    "C6": 1046.50
}

# Diatonic degrees in C Major / A Minor: 0=C, 1=D, 2=E, 3=F, 4=G, 5=A, 6=B
SCALE_NOTES = ["C", "D", "E", "F", "G", "A", "B"]

TEMPO_BPM = 65
BEAT_MS = int(60000 / TEMPO_BPM)      # ~923 ms
BAR_MS = BEAT_MS * 4                   # ~3692 ms
NUM_BARS = 8
TOTAL_DURATION_MS = BAR_MS * NUM_BARS   # ~29538 ms (~29.5s)


def compose_from_github(levels_grid: List[List[int]], current_streak: int = 0) -> Dict[str, Any]:
    """
    Transforms 52x7 GitHub contribution level grid into a musical event timeline.
    
    levels_grid: 52 columns (weeks 0..51), each with 7 rows (days 0..6, Sun..Sat).
    current_streak: Running streak count.
    
    Returns:
      {
        "tempo": 65,
        "duration_ms": 29538,
        "events": [
          {
            "time": 0,           # millisecond offset from loop start
            "note": "C3",        # note name
            "freq": 130.81,      # frequency in Hz
            "duration": 1800,    # duration in ms
            "velocity": 75,      # 0..127 dynamics
            "week": 0,           # associated GitHub week (0..51)
            "day": 1,            # associated GitHub day (0..6)
            "led": 1             # cell index for quick LED addressing
          }, ...
        ]
      }
    """
    events = []

    # Calculate weekly summaries to guide harmonic variation
    weekly_max = []
    weekly_sums = []
    for w in range(min(52, len(levels_grid))):
        col = levels_grid[w] if w < len(levels_grid) else [0]*7
        mx = max(col) if col else 0
        sm = sum(col) if col else 0
        weekly_max.append(mx)
        weekly_sums.append(sm)

    # 4 phrases across 8 bars:
    # Bar 0: C Major (pedal C3, motif C4-E4-G4)
    # Bar 1: A minor (pedal A2, motif E4-G4-A4)
    # Bar 2: F Major (pedal F2, motif A4-C5-F4)
    # Bar 3: G Major (pedal G2, motif B4-D5-G4)
    # Bar 4: A minor (pedal A2, motif C5-E5-A4)
    # Bar 5: F Major (pedal F2, motif D5-C5-A4)
    # Bar 6: C/G (pedal G2, motif E5-D5-C5)
    # Bar 7: C Major resolution (pedal C3, motif G4-E4-C4 with gentle rallentando)
    
    PHRASE_DEFS = [
        {"chord": "C",  "root": "C3", "chord_tones": ["C4", "E4", "G4", "C5"], "week_range": (0, 7)},
        {"chord": "Am", "root": "A2", "chord_tones": ["A3", "C4", "E4", "A4"], "week_range": (7, 14)},
        {"chord": "F",  "root": "F2", "chord_tones": ["F3", "A3", "C4", "F4"], "week_range": (14, 21)},
        {"chord": "G",  "root": "G2", "chord_tones": ["G3", "B3", "D4", "G4"], "week_range": (21, 28)},
        {"chord": "Am", "root": "A2", "chord_tones": ["C4", "E4", "A4", "C5"], "week_range": (28, 35)},
        {"chord": "F",  "root": "F2", "chord_tones": ["A4", "C5", "D5", "F5"], "week_range": (35, 42)},
        {"chord": "G",  "root": "G2", "chord_tones": ["G4", "B4", "D5", "E5"], "week_range": (42, 48)},
        {"chord": "C",  "root": "C3", "chord_tones": ["E5", "D5", "C5", "C4"], "week_range": (48, 52)},
    ]

    for bar in range(NUM_BARS):
        bar_start_ms = bar * BAR_MS
        p_def = PHRASE_DEFS[bar]
        w_start, w_end = p_def["week_range"]
        
        # 1. Warm bass accompaniment note on beat 1 of each bar
        root_note = p_def["root"]
        bass_week = min(51, max(0, w_start))
        events.append({
            "time": bar_start_ms,
            "note": root_note,
            "freq": NOTE_FREQS[root_note],
            "duration": int(BAR_MS * 0.9),
            "velocity": 68 + min(15, current_streak // 2),
            "week": bass_week,
            "day": 0,
            "led": bass_week * 7 + 0
        })

        # 2. Harmonically rich chord arpeggio on beat 2 or 3 (mid register)
        mid_note = p_def["chord_tones"][1]
        mid_time = bar_start_ms + BEAT_MS
        events.append({
            "time": mid_time,
            "note": mid_note,
            "freq": NOTE_FREQS[mid_note],
            "duration": int(BEAT_MS * 1.5),
            "velocity": 60,
            "week": bass_week,
            "day": 3,
            "led": bass_week * 7 + 3
        })

        # 3. Melodic line sculpted deterministically from user's GitHub activity in this week range
        weeks_in_range = list(range(w_start, min(52, w_end)))
        if not weeks_in_range:
            weeks_in_range = [bass_week]

        # Select 2 to 3 rhythmic steps within the bar for the melody
        melody_offsets = [0, int(BEAT_MS * 1.5), int(BEAT_MS * 2.5)] if bar < 7 else [0, BEAT_MS * 2]
        
        for idx, offset in enumerate(melody_offsets):
            step_week = weeks_in_range[idx % len(weeks_in_range)]
            col = levels_grid[step_week] if step_week < len(levels_grid) else [0]*7
            
            # Find the most significant day in this week
            best_day = 0
            best_lvl = col[0]
            for d in range(1, 7):
                if col[d] > best_lvl:
                    best_lvl = col[d]
                    best_day = d
            
            event_time = bar_start_ms + offset
            
            # Contribution level controls tone selection, velocity, and duration
            if best_lvl == 0:
                # Rest or gentle passing tone (calm ambient space)
                # Play a very soft, sustained note from the chord palette
                mel_note = p_def["chord_tones"][0]
                vel = 48
                dur = int(BEAT_MS * 0.9)
            elif best_lvl == 1:
                mel_note = p_def["chord_tones"][idx % len(p_def["chord_tones"])]
                vel = 62
                dur = int(BEAT_MS * 0.8)
            elif best_lvl == 2:
                # Expressive chord tone
                mel_note = p_def["chord_tones"][(idx + 1) % len(p_def["chord_tones"])]
                vel = 74
                dur = int(BEAT_MS * 1.0)
            elif best_lvl == 3:
                # Melodic leap (octave 5)
                note_base = p_def["chord_tones"][(idx + 2) % len(p_def["chord_tones"])]
                # Elevate to octave 5 if possible
                note_name = note_base[0]
                mel_note = f"{note_name}5" if f"{note_name}5" in NOTE_FREQS else note_base
                vel = 86
                dur = int(BEAT_MS * 1.2)
            else: # best_lvl == 4
                # Brilliant, singing accent (sparkle)
                note_name = p_def["chord_tones"][-1][0]
                mel_note = f"{note_name}5" if f"{note_name}5" in NOTE_FREQS else "C5"
                vel = 98
                dur = int(BEAT_MS * 1.4)
                
            events.append({
                "time": event_time,
                "note": mel_note,
                "freq": NOTE_FREQS[mel_note],
                "duration": dur,
                "velocity": min(115, vel + min(12, current_streak // 3)),
                "week": step_week,
                "day": best_day,
                "led": step_week * 7 + best_day
            })

    # Sort events strictly by timestamp for sequencer predictability
    events.sort(key=lambda e: e["time"])

    return {
        "tempo": TEMPO_BPM,
        "duration_ms": TOTAL_DURATION_MS,
        "events": events
    }
