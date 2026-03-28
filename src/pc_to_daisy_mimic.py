#!/usr/bin/env python3
"""
daisy_validation.py — Validation backend for CoJam Daisy USB audio exchange.

Full session flow:
  1. Receive preamble (8 bytes) from Daisy: magic(4) + density(uint8, 1-10) + pad(3)
  2. Receive WAV header (44 bytes) from Daisy
  3. Receive PCM data from Daisy, save as capture.wav
  4. Wait for 0xAA ready byte from Daisy
  5. Load user-supplied drum track WAV, convert/trim/loop to 4 bars at BPM
  6. Send preamble to Daisy: magic(4) + float32 BPM (little-endian)
  7. Send uint16 style_len + style bytes (empty string for validation)
  8. Send WAV (header + PCM) to Daisy

Usage:
    python daisy_validation.py --port /dev/ttyACM0 --drum kick_loop.wav --bpm 120

Arguments:
    --port     Serial port the Daisy is connected to
    --drum     Path to your drum track WAV file (any sample rate, mono or stereo)
    --bpm      BPM to use for 4-bar calculation and to report back to Daisy
    --out      Output filename for the captured guitar WAV (default: capture.wav)
    --timeout  Serial read timeout in seconds (default: 60)

Dependencies:
    pip install pyserial numpy soundfile scipy

Protocol (must match firmware):
  Daisy → PC:
    [0:4]  magic = 0xC0 'J' 'A' 'M'
    [4]    uint8  density (1–10)
    [5:8]  pad zeros
    [8:52] 44-byte WAV header (float32 mono 48 kHz)
    [52:]  PCM bytes (float32 mono)
    [+1]   0xAA ready byte

  PC → Daisy:
    [0:4]  magic = 0xC0 'J' 'A' 'M'
    [4:8]  float32 BPM (little-endian)
    [8:10] uint16 style_len (little-endian)
    [10:]  style_len bytes of UTF-8 style string
    [+44]  WAV header (float32 mono 48 kHz)
    [+N]   PCM bytes (float32 mono, 4 bars at BPM)
"""

import argparse
import struct
import sys
import time

import numpy as np
import serial
import soundfile as sf
from scipy.signal import resample_poly
from math import gcd

# ── Protocol constants (must match firmware) ──────────────────────────────────

PREAMBLE_MAGIC   = bytes([0xC0, 0x4A, 0x41, 0x4D])  # 0xC0 'J' 'A' 'M'
PREAMBLE_BYTES   = 8
WAV_HEADER_BYTES = 44
READY_BYTE       = 0xAA

# ── Audio constants (must match firmware CAPTURE_SECONDS) ─────────────────────
# testing: 2 — production: 20. Must match firmware CAPTURE_SECONDS.

TARGET_SAMPLE_RATE = 48000
CAPTURE_SECONDS    = 20
CAPTURE_SAMPLES    = TARGET_SAMPLE_RATE * CAPTURE_SECONDS
BYTES_PER_SAMPLE   = 4  # float32
CAPTURE_WAV_BYTES  = WAV_HEADER_BYTES + CAPTURE_SAMPLES * BYTES_PER_SAMPLE

# Bars / beats — assumes 4/4 time
BARS          = 4
BEATS_PER_BAR = 4


# ── Serial helpers ────────────────────────────────────────────────────────────

def recv_exact(ser: serial.Serial, n: int, label: str) -> bytes:
    """Block until exactly n bytes have been received."""
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            print(f"\n[ERROR] Timeout waiting for {label}.", file=sys.stderr)
            sys.exit(1)
        buf.extend(chunk)
    return bytes(buf)


def recv_with_progress(ser: serial.Serial, n: int, label: str) -> bytes:
    """Block until n bytes received, printing a progress bar."""
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            print(f"\n[ERROR] Timeout during {label}.", file=sys.stderr)
            sys.exit(1)
        buf.extend(chunk)
        pct = 100.0 * len(buf) / n
        bar = "#" * int(pct / 2)
        print(f"  [{bar:<50}] {pct:5.1f}%  {len(buf):>9,} / {n:,} bytes\r",
              end="", flush=True)
    print()
    return bytes(buf)


# ── Protocol builders ─────────────────────────────────────────────────────────

def build_wav_header(num_samples: int, sample_rate: int) -> bytes:
    """Build a 44-byte WAV header for IEEE float32 mono audio."""
    data_size = num_samples * BYTES_PER_SAMPLE
    riff_size = data_size + 36
    byte_rate = sample_rate * BYTES_PER_SAMPLE

    header = bytearray(WAV_HEADER_BYTES)
    header[0:4]  = b"RIFF"
    struct.pack_into("<I", header,  4, riff_size)
    header[8:12] = b"WAVE"
    header[12:16]= b"fmt "
    struct.pack_into("<I", header, 16, 16)           # fmt chunk size
    struct.pack_into("<H", header, 20, 3)            # IEEE float
    struct.pack_into("<H", header, 22, 1)            # mono
    struct.pack_into("<I", header, 24, sample_rate)
    struct.pack_into("<I", header, 28, byte_rate)
    struct.pack_into("<H", header, 32, BYTES_PER_SAMPLE)
    struct.pack_into("<H", header, 34, 32)           # bits per sample
    header[36:40]= b"data"
    struct.pack_into("<I", header, 40, data_size)
    return bytes(header)


def build_preamble_to_daisy(bpm: float) -> bytes:
    """PC → Daisy preamble: magic(4) + float32 BPM little-endian."""
    buf = bytearray(PREAMBLE_BYTES)
    buf[0:4] = PREAMBLE_MAGIC
    struct.pack_into("<f", buf, 4, bpm)
    return bytes(buf)


def build_style_field(style: str = "", max_bytes: int = 256) -> bytes:
    """uint16_LE style_len + UTF-8 style bytes (empty for validation script)."""
    encoded = style.encode("utf-8", errors="replace")[:max_bytes]
    return struct.pack("<H", len(encoded)) + encoded


# ── Protocol parsers ──────────────────────────────────────────────────────────

def parse_daisy_preamble(data: bytes) -> int:
    """
    Parse the Daisy → PC preamble.
    Layout: magic(4) + uint8 density(1-10) + pad(3).
    Returns density as int.
    """
    if len(data) != PREAMBLE_BYTES:
        raise ValueError(f"Bad preamble length: {len(data)} (expected {PREAMBLE_BYTES})")
    if data[0:4] != PREAMBLE_MAGIC:
        raise ValueError(f"Bad preamble magic: {data[0:4].hex()}")
    density = int(data[4])
    return max(1, min(10, density))


def parse_wav_header(header: bytes) -> dict:
    """Parse a 44-byte WAV header; raise ValueError on unsupported format."""
    if header[0:4] != b"RIFF" or header[8:12] != b"WAVE":
        raise ValueError(f"Bad RIFF/WAVE magic: {header[0:4]} {header[8:12]}")

    fmt_tag     = struct.unpack_from("<H", header, 20)[0]
    channels    = struct.unpack_from("<H", header, 22)[0]
    sample_rate = struct.unpack_from("<I", header, 24)[0]
    data_size   = struct.unpack_from("<I", header, 40)[0]

    if fmt_tag != 3:
        raise ValueError(f"Unexpected WAV format tag {fmt_tag} (expected 3 = IEEE float)")
    if channels != 1:
        raise ValueError(f"Expected mono, got {channels} channels")
    if sample_rate != TARGET_SAMPLE_RATE:
        raise ValueError(f"Expected {TARGET_SAMPLE_RATE} Hz, got {sample_rate} Hz")

    return {
        "sample_rate": sample_rate,
        "data_size":   data_size,
        "num_samples": data_size // BYTES_PER_SAMPLE,
    }


# ── Drum track preparation ────────────────────────────────────────────────────

def prepare_drum_track(path: str, bpm: float) -> np.ndarray:
    """
    Load a drum track WAV and prepare for transmission:
      - Convert to mono
      - Resample to TARGET_SAMPLE_RATE if needed
      - Trim or loop to exactly 4 bars at the given BPM
      - Normalise to [-1.0, 1.0]
      - Return as float32 numpy array
    """
    print(f"[drum] Loading: {path}")
    data, src_rate = sf.read(path, dtype="float32", always_2d=True)

    if data.shape[1] > 1:
        print(f"[drum] Converting {data.shape[1]}-channel audio to mono")
        data = np.mean(data, axis=1)
    else:
        data = data[:, 0]

    if src_rate != TARGET_SAMPLE_RATE:
        print(f"[drum] Resampling {src_rate} Hz → {TARGET_SAMPLE_RATE} Hz")
        g    = gcd(TARGET_SAMPLE_RATE, src_rate)
        up   = TARGET_SAMPLE_RATE // g
        down = src_rate // g
        data = resample_poly(data, up, down).astype(np.float32)

    duration_sec   = (BARS * BEATS_PER_BAR * 60.0) / bpm
    target_samples = int(round(duration_sec * TARGET_SAMPLE_RATE))

    print(f"[drum] 4 bars @ {bpm:.1f} BPM = {duration_sec:.3f}s = {target_samples:,} samples")
    print(f"[drum] Source length: {len(data):,} samples "
          f"({len(data) / TARGET_SAMPLE_RATE:.3f}s)")

    if len(data) >= target_samples:
        data = data[:target_samples]
        print(f"[drum] Trimmed to {target_samples:,} samples")
    else:
        repeats = (target_samples // len(data)) + 1
        data    = np.tile(data, repeats)[:target_samples]
        print(f"[drum] Looped ×{repeats} and trimmed to {target_samples:,} samples")

    peak = np.max(np.abs(data))
    if peak > 0.0:
        data = data / peak
    return data.astype(np.float32)


# ── Main session ──────────────────────────────────────────────────────────────

def run(port: str, drum_path: str, bpm: float, out_path: str,
        timeout_s: float) -> None:

    print("=" * 60)
    print(f"  CoJam validation backend")
    print(f"  Port:    {port}")
    print(f"  Drum:    {drum_path}")
    print(f"  BPM:     {bpm}")
    print(f"  Output:  {out_path}")
    print(f"  Capture: {CAPTURE_SECONDS}s  ({CAPTURE_SAMPLES:,} samples)")
    print("=" * 60)

    with serial.Serial(port, baudrate=115200, timeout=timeout_s) as ser:

        # ── Step 1: Receive Daisy preamble ────────────────────────────────────
        print("\n[1/7] Waiting for Daisy preamble...")
        raw_preamble = recv_exact(ser, PREAMBLE_BYTES, "Daisy preamble")
        try:
            daisy_density = parse_daisy_preamble(raw_preamble)
        except ValueError as e:
            print(f"[ERROR] Preamble parse failed: {e}", file=sys.stderr)
            sys.exit(1)
        print(f"      Daisy density: {daisy_density}/10")

        # ── Step 2: Receive WAV header ────────────────────────────────────────
        print("\n[2/7] Receiving WAV header...")
        raw_header = recv_exact(ser, WAV_HEADER_BYTES, "WAV header")
        try:
            wav_info = parse_wav_header(raw_header)
        except ValueError as e:
            print(f"[ERROR] WAV header invalid: {e}", file=sys.stderr)
            sys.exit(1)
        pcm_bytes = wav_info["data_size"]
        print(f"      {wav_info['num_samples']:,} samples  "
              f"({wav_info['num_samples'] / TARGET_SAMPLE_RATE:.2f}s)  "
              f"{pcm_bytes:,} PCM bytes")

        expected_pcm = CAPTURE_SAMPLES * BYTES_PER_SAMPLE
        if pcm_bytes != expected_pcm:
            print(f"      [WARN] Expected {expected_pcm:,} bytes "
                  f"({CAPTURE_SECONDS}s) — check CAPTURE_SECONDS in firmware vs script")

        # ── Step 3: Receive PCM data ──────────────────────────────────────────
        print(f"\n[3/7] Receiving {pcm_bytes:,} bytes of PCM data...")
        raw_pcm = recv_with_progress(ser, pcm_bytes, "PCM data")

        # ── Step 4: Save captured WAV ─────────────────────────────────────────
        print(f"\n[4/7] Saving captured audio → {out_path}")
        capture_f32 = np.frombuffer(raw_pcm, dtype=np.float32)
        with open(out_path, "wb") as f:
            f.write(raw_header + raw_pcm)
        peak = float(np.max(np.abs(capture_f32)))
        print(f"      Saved {len(raw_header) + len(raw_pcm):,} bytes  "
              f"(peak amplitude: {peak:.4f})")

        # ── Step 5: Wait for Daisy ready byte ─────────────────────────────────
        print(f"\n[5/7] Waiting for Daisy ready byte (0x{READY_BYTE:02X})...")
        ready = recv_exact(ser, 1, "ready byte")
        if ready[0] != READY_BYTE:
            print(f"[ERROR] Expected 0x{READY_BYTE:02X}, got 0x{ready[0]:02X}",
                  file=sys.stderr)
            sys.exit(1)
        print("      Daisy is ready.")

        # ── Step 6: Prepare drum track ────────────────────────────────────────
        print(f"\n[6/7] Preparing drum track...")
        drum_samples = prepare_drum_track(drum_path, bpm)
        num_samples  = len(drum_samples)
        pcm_out      = drum_samples.tobytes()

        # ── Step 7: Transmit to Daisy ─────────────────────────────────────────
        # PC → Daisy layout:
        #   preamble(8) + style_len(2) + style_bytes(0) + wav_header(44) + pcm
        tx_preamble   = build_preamble_to_daisy(bpm)
        tx_style      = build_style_field("")   # empty — validation script has no style
        tx_wav_header = build_wav_header(num_samples, TARGET_SAMPLE_RATE)
        payload       = tx_preamble + tx_style + tx_wav_header + pcm_out
        total_tx      = len(payload)

        print(f"\n[7/7] Transmitting to Daisy ({total_tx:,} bytes total)...")
        print(f"      Preamble : {len(tx_preamble)} bytes  (BPM={bpm:.2f})")
        print(f"      Style    : {len(tx_style)} bytes  (empty)")
        print(f"      WAV hdr  : {len(tx_wav_header)} bytes")
        print(f"      PCM data : {len(pcm_out):,} bytes  ({num_samples:,} samples)")

        CHUNK   = 2048
        sent    = 0
        t_start = time.monotonic()

        while sent < len(payload):
            chunk  = payload[sent:sent + CHUNK]
            ser.write(chunk)
            sent  += len(chunk)
            pct    = 100.0 * sent / len(payload)
            bar    = "#" * int(pct / 2)
            print(f"  [{bar:<50}] {pct:5.1f}%  {sent:>9,} / {len(payload):,} bytes\r",
                  end="", flush=True)
            time.sleep(0.002)

        ser.flush()
        elapsed = time.monotonic() - t_start
        print(f"\n      Done. {sent:,} bytes in {elapsed:.2f}s "
              f"({sent / elapsed / 1024:.1f} KB/s)")

    print("\n" + "=" * 60)
    print("  Session complete.")
    print(f"  Captured guitar  → {out_path}")
    print(f"  Drum track sent  → {num_samples:,} samples @ {bpm:.1f} BPM")
    print(f"  Daisy density    : {daisy_density}/10")
    print(f"  PC BPM           : {bpm:.2f}")
    print("=" * 60)


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="CoJam Daisy USB validation backend",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--port",    required=True,
                        help="Serial port (e.g. /dev/ttyACM0, COM3)")
    parser.add_argument("--drum",    required=True,
                        help="Path to drum track WAV file")
    parser.add_argument("--bpm",     type=float, required=True,
                        help="BPM for 4-bar length calculation")
    parser.add_argument("--out",     default="capture.wav",
                        help="Output filename for captured guitar audio")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="Serial read timeout in seconds (default 60 — "
                             "covers capture time + transfer)")
    args = parser.parse_args()

    if not (60.0 <= args.bpm <= 180.0):
        print(f"[ERROR] BPM {args.bpm} is outside supported range [60, 180].",
              file=sys.stderr)
        sys.exit(1)

    run(args.port, args.drum, args.bpm, args.out, args.timeout)