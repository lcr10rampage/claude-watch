#!/usr/bin/env python3
"""Transcribe a 16 kHz mono WAV with faster-whisper. Run with ~/stt-env/bin/python.
Reads the samples directly (no PyAV) so it works with any av version."""
import sys, wave, time
import numpy as np
from faster_whisper import WhisperModel

MODEL = sys.argv[2] if len(sys.argv) > 2 else "base.en"
w = wave.open(sys.argv[1]); n = w.getnframes(); rate = w.getframerate()
audio = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float32) / 32768.0
if rate != 16000:
    idx = np.linspace(0, len(audio) - 1, int(len(audio) * 16000 / rate)); audio = np.interp(idx, np.arange(len(audio)), audio).astype(np.float32)
t0 = time.time()
model = WhisperModel(MODEL, device="cpu", compute_type="int8", cpu_threads=4)
t1 = time.time()
segs, info = model.transcribe(audio, beam_size=1, language="en", vad_filter=True, condition_on_previous_text=False)
text = " ".join(s.text.strip() for s in segs).strip()
t2 = time.time()
print(f"[{MODEL}] load {t1-t0:.1f}s, transcribe {t2-t1:.1f}s for {n/rate:.1f}s of audio")
print("TEXT:", text if text else "(nothing)")
