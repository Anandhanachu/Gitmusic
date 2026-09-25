"""
piano_synth.py – Realistic acoustic piano synthesizer for GitMusic.

Renders an authoritative musical event timeline into high-fidelity
16-bit PCM WAV (44.1 kHz, mono) audio.

Features of the physical acoustic piano model:
 - Multi-string inharmonic partials: f_k = k * f_0 * sqrt(1 + B * k^2)
 - Dual unison detuning (gentle singing beating characteristic of grand pianos)
 - Non-linear hammer velocity response: softer notes are darker and warmer;
   louder notes have sparkling higher harmonics.
 - Percussive felt hammer strike transient at t=0
 - Soundboard acoustic resonance and smooth ambient tail
 - Seamless looping crossfade at end of composition
"""

import os
import wave
import numpy as np
from typing import Dict, Any

SAMPLE_RATE = 44100  # 44.1 kHz 16-bit PCM Mono


def _synthesize_piano_note(
    freq: float,
    duration_s: float,
    velocity: int,
    sample_rate: int = SAMPLE_RATE
) -> np.ndarray:
    """Synthesizes a single acoustic piano note using physical acoustic modeling."""
    if freq <= 0.0:
        return np.zeros(int(sample_rate * duration_s), dtype=np.float32)

    total_samples = int(sample_rate * duration_s)
    t = np.linspace(0, duration_s, total_samples, endpoint=False, dtype=np.float32)

    # Dynamics: velocity 0..127 normalized to 0.15..1.0
    vel_ratio = max(0.15, min(1.0, velocity / 127.0))
    
    # String inharmonicity coefficient (stiffness)
    B = 0.00018
    
    # Brighter notes get more audible harmonics at higher velocity
    num_harmonics = min(14, max(4, int(6 + 8 * vel_ratio)))
    
    # Unison detuning creates acoustic piano chorus/warmth
    detunes = [-0.35, 0.35] if freq > 160.0 else [0.0]
    
    note_wave = np.zeros(total_samples, dtype=np.float32)
    decay_base = 2.0 + 1.2 * vel_ratio

    for d_hz in detunes:
        f_actual = freq + d_hz
        for k in range(1, num_harmonics + 1):
            f_k = k * f_actual * np.sqrt(1.0 + B * (k ** 2))
            if f_k >= sample_rate / 2.0:
                break
            
            # Harmonic amplitude rolls off with frequency and velocity
            amp_k = (1.0 / (k ** 1.25)) * (vel_ratio ** (0.35 + 0.04 * k))
            
            # Higher harmonics decay faster
            decay_rate = decay_base / (k ** 0.8)
            decay_env = np.exp(-t * (1.6 / decay_rate), dtype=np.float32)
            
            # Smooth hammer attack (~4ms)
            attack_samples = min(int(0.004 * sample_rate), total_samples)
            if attack_samples > 0:
                decay_env[:attack_samples] *= np.linspace(0.0, 1.0, attack_samples, dtype=np.float32)
                
            note_wave += amp_k * decay_env * np.sin(2.0 * np.pi * f_k * t)

    # Tactile hammer-on-felt knock transient
    knock_len = min(int(0.012 * sample_rate), total_samples)
    if knock_len > 0:
        knock = (np.random.randn(knock_len) * np.exp(-np.linspace(0, 6, knock_len)) * 0.12 * vel_ratio).astype(np.float32)
        note_wave[:knock_len] += knock

    return note_wave


def render_composition_to_wav(
    timeline: Dict[str, Any],
    output_wav_path: str,
    sample_rate: int = SAMPLE_RATE
) -> str:
    """
    Renders an event timeline into a 16-bit PCM WAV file.
    Includes ambient acoustic reverb tail and seamless loop crossfade.
    """
    total_duration_ms = timeline.get("duration_ms", 29538)
    total_samples = int((total_duration_ms / 1000.0) * sample_rate)
    
    # Extra 1.5s tail for acoustic reverb ring-out
    tail_samples = int(1.5 * sample_rate)
    buffer_len = total_samples + tail_samples
    audio_buffer = np.zeros(buffer_len, dtype=np.float32)

    events = timeline.get("events", [])
    
    for event in events:
        start_ms = event.get("time", 0)
        freq = event.get("freq", 261.63)
        duration_ms = event.get("duration", 800)
        velocity = event.get("velocity", 70)
        
        start_sample = int((start_ms / 1000.0) * sample_rate)
        if start_sample >= total_samples:
            continue
            
        # Give piano notes a natural ring decay of up to 2.8s
        note_duration_s = min(3.0, max(0.4, (duration_ms / 1000.0) * 1.5))
        note_samples = _synthesize_piano_note(freq, note_duration_s, velocity, sample_rate)
        
        end_sample = min(buffer_len, start_sample + len(note_samples))
        chunk_len = end_sample - start_sample
        audio_buffer[start_sample:end_sample] += note_samples[:chunk_len]

    # Simple ambient soundboard reverb (exponentially decaying early reflections)
    reverb_delays = [int(sample_rate * d) for d in [0.031, 0.057, 0.089, 0.127]]
    reverb_gains = [0.22, 0.16, 0.11, 0.07]
    for delay_samps, gain in zip(reverb_delays, reverb_gains):
        audio_buffer[delay_samps:] += gain * audio_buffer[:-delay_samps]

    # Fold tail back into beginning for a completely seamless, natural loop!
    # The acoustic resonance of the final bar smoothly blooms into the first bar.
    crossfade_len = min(tail_samples, total_samples)
    audio_buffer[:crossfade_len] += audio_buffer[total_samples:total_samples + crossfade_len] * 0.75
    
    # Truncate buffer to exact total_samples for strict timeline synchronization
    final_audio = audio_buffer[:total_samples]

    # Normalize to peak -1.0 dBFS
    max_val = np.max(np.abs(final_audio))
    if max_val > 1e-5:
        final_audio = (final_audio / max_val) * 0.92

    # Convert to 16-bit signed PCM integers (-32768 to 32767)
    pcm_data = (final_audio * 32767.0).astype(np.int16)

    # Ensure target directory exists
    os.makedirs(os.path.dirname(os.path.abspath(output_wav_path)), exist_ok=True)

    # Write standard 16-bit mono WAV
    with wave.open(output_wav_path, "wb") as wf:
        wf.setnchannels(1)      # Mono
        wf.setsampwidth(2)      # 16-bit (2 bytes)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_data.tobytes())

    return output_wav_path
