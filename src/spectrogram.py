#!/usr/bin/env python3
"""
Spectrogram Visualizer for Daisy Seed FFT Output

Reads serial output in format:
    SPEC,<mag0>,<mag1>,<mag2>,...   (decimated magnitude bins)

Creates a real-time scrolling spectrogram.

Usage:
    python3 spectrogram_spec.py /dev/tty.usbmodemXXXX

Requirements:
    pip install pyserial matplotlib numpy
"""

import sys
import numpy as np
import matplotlib.pyplot as plt

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Error: pyserial not installed. Run: pip install pyserial")
    sys.exit(1)

# Configuration - must match firmware
SAMPLE_RATE = 48000
FFT_SIZE = 1024
NUM_BINS = FFT_SIZE // 2  # 512
BIN_DECIMATION = 2        # Firmware outputs every 2nd bin
NUM_OUTPUT_BINS = NUM_BINS // BIN_DECIMATION  # 256
HISTORY_FRAMES = 200
MAX_FREQ_DISPLAY = 20000

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 spectrogram_spec.py <serial_port>")
        print("\nExamples:")
        print("  python3 spectrogram_spec.py /dev/tty.usbmodem14101")
        print("  python3 spectrogram_spec.py COM3")
        print("\nAvailable ports:")
        for port in serial.tools.list_ports.comports():
            print(f"  {port.device} - {port.description}")
        sys.exit(1)

    port = sys.argv[1]
    print(f"Connecting to {port}...")
    
    try:
        ser = serial.Serial(port, 115200, timeout=0.1)
    except Exception as e:
        print(f"Error opening serial port: {e}")
        sys.exit(1)

    print("Connected! Waiting for data...")
    print("Press Ctrl+C to exit")

    # Setup plot
    plt.ion()
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 8))
    
    # Frequency axis for decimated bins
    # Each output bin represents: bin_index * BIN_DECIMATION * SAMPLE_RATE / FFT_SIZE
    freqs = np.arange(NUM_OUTPUT_BINS) * BIN_DECIMATION * SAMPLE_RATE / FFT_SIZE
    max_bin_display = min(NUM_OUTPUT_BINS, int(MAX_FREQ_DISPLAY * FFT_SIZE / SAMPLE_RATE / BIN_DECIMATION) + 1)
    
    # Current spectrum (bar plot)
    bar_width = BIN_DECIMATION * SAMPLE_RATE / FFT_SIZE * 0.8
    bars = ax1.bar(freqs[:max_bin_display], np.zeros(max_bin_display), width=bar_width, color='steelblue')
    ax1.set_xlabel('Frequency (Hz)')
    ax1.set_ylabel('Magnitude')
    ax1.set_title('Current Spectrum (Decimated)')
    ax1.set_xlim(0, MAX_FREQ_DISPLAY)
    ax1.set_ylim(0, 10000)
    
    # Peak annotation
    peak_text = ax1.text(0.98, 0.95, '', transform=ax1.transAxes, 
                         fontsize=11, verticalalignment='top', horizontalalignment='right',
                         bbox=dict(boxstyle='round', facecolor='yellow', alpha=0.9))
    
    # Spectrogram
    spectrogram = np.zeros((max_bin_display, HISTORY_FRAMES))
    img = ax2.imshow(spectrogram, aspect='auto', origin='lower',
                     extent=[0, HISTORY_FRAMES, 0, MAX_FREQ_DISPLAY],
                     cmap='magma', vmin=0, vmax=10000)
    ax2.set_xlabel('Frame')
    ax2.set_ylabel('Frequency (Hz)')
    ax2.set_title('Spectrogram (Time History)')
    cbar = plt.colorbar(img, ax=ax2, label='Magnitude')
    
    plt.tight_layout()

    frame_count = 0
    
    try:
        while True:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            
            if not line:
                continue
            
            # Handle SPEC format: SPEC,mag0,mag1,mag2,...
            if line.startswith("SPEC,"):
                parts = line.split(",")
                try:
                    # Parse magnitudes (skip "SPEC" prefix)
                    mags = [int(x) for x in parts[1:] if x.strip()]
                    
                    # Ensure we have enough data
                    if len(mags) < max_bin_display:
                        mags.extend([0] * (max_bin_display - len(mags)))
                    mags = mags[:max_bin_display]
                    
                    # Update bar plot
                    for bar, mag in zip(bars, mags):
                        bar.set_height(mag)
                    
                    # Auto-scale Y axis
                    max_mag = max(mags) if mags else 1
                    if max_mag > 0:
                        ax1.set_ylim(0, max_mag * 1.3)
                    
                    # Find and display peak
                    peak_idx = np.argmax(mags[1:]) + 1  # Skip DC
                    peak_freq = peak_idx * BIN_DECIMATION * SAMPLE_RATE / FFT_SIZE
                    peak_mag = mags[peak_idx]
                    peak_text.set_text(f'Peak: {peak_freq:.0f} Hz\nMag: {peak_mag}')
                    
                    # Update spectrogram
                    spectrogram = np.roll(spectrogram, -1, axis=1)
                    spectrogram[:, -1] = mags
                    img.set_data(spectrogram)
                    
                    # Auto-scale spectrogram color
                    vmax = np.percentile(spectrogram, 99)
                    if vmax > 0:
                        img.set_clim(0, vmax)
                    
                    frame_count += 1
                    if frame_count % 10 == 0:
                        print(f"Frame {frame_count}: Peak at {peak_freq:.0f} Hz, mag={peak_mag}")
                    
                except ValueError as e:
                    print(f"Parse error: {e}")
                    pass
                
                plt.pause(0.001)
            
            # Handle old FREQ format for compatibility
            elif line.startswith("FREQ,"):
                print(f"[FREQ] {line[:60]}...")
            
            # Handle PEAK format
            elif line.startswith("PEAK,"):
                parts = line.split(",")
                try:
                    freq = int(parts[1])
                    mag = int(parts[2])
                    print(f"PEAK: {freq} Hz, mag={mag}")
                except:
                    pass
            
            # Other info
            elif line and not line.startswith("="):
                # Don't spam with corrupted lines
                if len(line) < 100:
                    print(f"[INFO] {line}")
            
    except KeyboardInterrupt:
        print("\n\nExiting...")
    finally:
        ser.close()
        plt.close()
        print("Done.")

if __name__ == "__main__":
    main()