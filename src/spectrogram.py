#!/usr/bin/env python3
"""
Spectrogram Visualizer for Daisy Seed FFT Output

Reads serial output in format:
    FREQ,<frequency_hz>,<magnitude>
    FREQ,<frequency_hz>,<magnitude>
    ...

Creates a real-time scrolling spectrogram.

Usage:
    python3 spectrogram.py /dev/tty.usbmodemXXXX

Requirements:
    pip install pyserial matplotlib numpy
"""

import sys
import numpy as np
import matplotlib.pyplot as plt
from collections import deque

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Error: pyserial not installed. Run: pip install pyserial")
    sys.exit(1)

# Configuration
SAMPLE_RATE = 48000
FFT_SIZE = 1024
NUM_BINS = FFT_SIZE // 2
HISTORY_FRAMES = 200  # Number of frames to show in spectrogram
MAX_FREQ_DISPLAY = 20000  # Max frequency to display (Hz)

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 spectrogram.py <serial_port>")
        print("\nExamples:")
        print("  python3 spectrogram.py /dev/tty.usbmodem14101")
        print("  python3 spectrogram.py COM3")
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
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(14, 10))
    
    # Frequency axis
    freqs = np.arange(NUM_BINS) * SAMPLE_RATE / FFT_SIZE
    max_bin = int(MAX_FREQ_DISPLAY * FFT_SIZE / SAMPLE_RATE)
    
    # Current spectrum (bar plot)
    bars = ax1.bar(freqs[:max_bin], np.zeros(max_bin), width=SAMPLE_RATE/FFT_SIZE*0.8, color='steelblue')
    ax1.set_xlabel('Frequency (Hz)')
    ax1.set_ylabel('Magnitude')
    ax1.set_title('Current Spectrum')
    ax1.set_xlim(0, MAX_FREQ_DISPLAY)
    ax1.set_ylim(0, 0.1)
    
    # Peak annotation
    peak_text = ax1.text(0.98, 0.95, '', transform=ax1.transAxes, 
                         fontsize=11, verticalalignment='top', horizontalalignment='right',
                         bbox=dict(boxstyle='round', facecolor='yellow', alpha=0.9))
    
    # Current spectrum (line plot for detail)
    spectrum_line, = ax2.plot(freqs[:max_bin], np.zeros(max_bin), 'b-', linewidth=0.5)
    ax2.set_xlabel('Frequency (Hz)')
    ax2.set_ylabel('Magnitude')
    ax2.set_title('Spectrum (Line Plot)')
    ax2.set_xlim(0, MAX_FREQ_DISPLAY)
    ax2.set_ylim(0, 0.1)
    ax2.grid(True, alpha=0.3)
    
    # Spectrogram (rolling history)
    spectrogram = np.zeros((max_bin, HISTORY_FRAMES))
    img = ax2.imshow(spectrogram, aspect='auto', origin='lower',
                     extent=[0, HISTORY_FRAMES, 0, MAX_FREQ_DISPLAY],
                     cmap='magma', vmin=0, vmax=0.01)
    ax3.set_xlabel('Frame')
    ax3.set_ylabel('Frequency (Hz)')
    ax3.set_title('Spectrogram (Time History)')
    
    # Move spectrogram to ax3
    ax3.clear()
    img = ax3.imshow(spectrogram, aspect='auto', origin='lower',
                     extent=[0, HISTORY_FRAMES, 0, MAX_FREQ_DISPLAY],
                     cmap='magma', vmin=0, vmax=0.01)
    ax3.set_xlabel('Frame')
    ax3.set_ylabel('Frequency (Hz)')
    ax3.set_title('Spectrogram (Time History)')
    cbar = plt.colorbar(img, ax=ax3, label='Magnitude')
    
    plt.tight_layout()

    # Data collection for current frame
    current_frame = {}
    frame_count = 0
    last_freq = -1
    
    try:
        while True:
            line_data = ser.readline().decode('utf-8', errors='ignore').strip()
            
            if not line_data:
                continue
                
            if line_data.startswith("FREQ,"):
                parts = line_data.split(",")
                try:
                    freq = float(parts[1])
                    mag = float(parts[2])
                    
                    # Detect new frame (frequency wrapped around)
                    if freq < last_freq and len(current_frame) > 10:
                        # Process completed frame
                        mags = np.zeros(NUM_BINS)
                        for f, m in current_frame.items():
                            bin_idx = int(round(f * FFT_SIZE / SAMPLE_RATE))
                            if 0 <= bin_idx < NUM_BINS:
                                mags[bin_idx] = m
                        
                        # Update bar plot
                        for bar, mag_val in zip(bars, mags[:max_bin]):
                            bar.set_height(mag_val)
                        
                        # Update line plot
                        spectrum_line.set_ydata(mags[:max_bin])
                        
                        # Auto-scale Y axis
                        max_mag = np.max(mags[:max_bin])
                        if max_mag > 0:
                            ax1.set_ylim(0, max_mag * 1.2)
                            ax2.set_ylim(0, max_mag * 1.2)
                        
                        # Find and display peak
                        peak_idx = np.argmax(mags[1:max_bin]) + 1  # Skip DC
                        peak_freq = peak_idx * SAMPLE_RATE / FFT_SIZE
                        peak_mag = mags[peak_idx]
                        peak_text.set_text(f'Peak: {peak_freq:.1f} Hz\nMag: {peak_mag:.6f}')
                        
                        # Update spectrogram
                        spectrogram = np.roll(spectrogram, -1, axis=1)
                        spectrogram[:, -1] = mags[:max_bin]
                        img.set_data(spectrogram)
                        
                        # Auto-scale spectrogram color
                        vmax = np.percentile(spectrogram, 99)
                        if vmax > 0:
                            img.set_clim(0, vmax)
                        
                        frame_count += 1
                        if frame_count % 10 == 0:
                            print(f"Frame {frame_count}: Peak at {peak_freq:.1f} Hz, mag={peak_mag:.6f}")
                        
                        # Clear for next frame
                        current_frame = {}
                        plt.pause(0.001)
                    
                    current_frame[freq] = mag
                    last_freq = freq
                    
                except (ValueError, IndexError) as e:
                    pass
            
            elif line_data.startswith("PEAK,"):
                # Handle PEAK format if present
                parts = line_data.split(",")
                try:
                    freq = float(parts[1])
                    mag = float(parts[2])
                    print(f"PEAK: {freq:.1f} Hz, mag={mag:.6f}")
                except:
                    pass
            
            else:
                # Print other messages
                if line_data and not line_data.startswith("="):
                    print(f"[INFO] {line_data}")
            
    except KeyboardInterrupt:
        print("\n\nExiting...")
    finally:
        ser.close()
        plt.close()
        print("Done.")

if __name__ == "__main__":
    main()