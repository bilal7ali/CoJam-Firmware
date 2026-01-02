#!/usr/bin/env python3
"""
Step 3: Spectral Flux Visualizer

Reads serial output:
    FLUX,<flux>,<threshold>,<onset>
    ONSET,<frame>,<flux>,<count>

Displays real-time plot of flux vs threshold with onset markers.

Usage:
    python3 flux_visualize.py /dev/tty.usbmodemXXXX

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
HISTORY_SIZE = 500  # Number of frames to display
SAMPLE_RATE = 48000
HOP_SIZE = 512
FRAME_RATE = SAMPLE_RATE / HOP_SIZE  # ~93.75 Hz

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 flux_visualize.py <serial_port>")
        print("\nAvailable ports:")
        for port in serial.tools.list_ports.comports():
            print(f"  {port.device} - {port.description}")
        sys.exit(1)

    port = sys.argv[1]
    print(f"Connecting to {port}...")
    
    try:
        ser = serial.Serial(port, 115200, timeout=0.1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

    print("Connected! Waiting for data...")
    print("Press Ctrl+C to exit")

    # Data buffers
    flux_history = deque(maxlen=HISTORY_SIZE)
    threshold_history = deque(maxlen=HISTORY_SIZE)
    onset_frames = deque(maxlen=100)  # Store frame indices where onsets occurred
    
    # Initialize with zeros
    for _ in range(HISTORY_SIZE):
        flux_history.append(0)
        threshold_history.append(0)

    # Setup plot
    plt.ion()
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 8))
    
    # Time axis in seconds
    time_axis = np.linspace(-HISTORY_SIZE / FRAME_RATE, 0, HISTORY_SIZE)
    
    # Flux and threshold plot
    flux_line, = ax1.plot(time_axis, list(flux_history), 'b-', label='Spectral Flux', linewidth=1)
    thresh_line, = ax1.plot(time_axis, list(threshold_history), 'r--', label='Threshold', linewidth=1)
    onset_scatter = ax1.scatter([], [], c='green', s=100, marker='v', label='Onset', zorder=5)
    
    ax1.set_xlabel('Time (seconds)')
    ax1.set_ylabel('Flux')
    ax1.set_title('Spectral Flux and Onset Detection')
    ax1.legend(loc='upper left')
    ax1.grid(True, alpha=0.3)
    ax1.set_xlim(-HISTORY_SIZE / FRAME_RATE, 0)
    ax1.set_ylim(0, 100)
    
    # Stats text
    stats_text = ax1.text(0.98, 0.95, '', transform=ax1.transAxes,
                          fontsize=10, verticalalignment='top', horizontalalignment='right',
                          bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.9))
    
    # Onset interval histogram
    ax2.set_xlabel('Inter-Onset Interval (ms)')
    ax2.set_ylabel('Count')
    ax2.set_title('Onset Interval Distribution (for BPM estimation)')
    ax2.set_xlim(200, 2000)  # 30-300 BPM range
    
    plt.tight_layout()

    frame_count = 0
    onset_count = 0
    onset_times = []  # Store onset times for interval calculation
    
    try:
        while True:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            
            if not line:
                continue
            
            # Parse FLUX line
            if line.startswith("FLUX,"):
                parts = line.split(",")
                try:
                    flux = int(parts[1])
                    threshold = int(parts[2])
                    is_onset = int(parts[3])
                    
                    flux_history.append(flux)
                    threshold_history.append(threshold)
                    frame_count += 1
                    
                    # Update plots every 4 frames
                    if frame_count % 4 == 0:
                        flux_line.set_ydata(list(flux_history))
                        thresh_line.set_ydata(list(threshold_history))
                        
                        # Auto-scale Y
                        max_val = max(max(flux_history), max(threshold_history))
                        if max_val > 0:
                            ax1.set_ylim(0, max_val * 1.2)
                        
                        # Update onset markers
                        recent_onsets = [t for t in onset_times if t > frame_count - HISTORY_SIZE]
                        if recent_onsets:
                            onset_x = [(t - frame_count) / FRAME_RATE for t in recent_onsets]
                            onset_y = [max(flux_history) * 0.9] * len(onset_x)
                            onset_scatter.set_offsets(np.column_stack([onset_x, onset_y]))
                        
                        plt.pause(0.001)
                        
                except (ValueError, IndexError):
                    pass
            
            # Parse ONSET line
            elif line.startswith("ONSET,"):
                parts = line.split(",")
                try:
                    frame = int(parts[1])
                    flux = int(parts[2])
                    count = int(parts[3])
                    
                    onset_times.append(frame)
                    onset_count = count
                    
                    # Calculate intervals
                    if len(onset_times) >= 2:
                        intervals_frames = np.diff(onset_times[-20:])  # Last 20 intervals
                        intervals_ms = intervals_frames * 1000 / FRAME_RATE
                        
                        # Estimate BPM from median interval
                        median_interval = np.median(intervals_ms)
                        estimated_bpm = 60000 / median_interval if median_interval > 0 else 0
                        
                        # Update histogram
                        ax2.clear()
                        ax2.hist(intervals_ms, bins=30, range=(200, 2000), color='steelblue', alpha=0.7)
                        ax2.axvline(x=median_interval, color='red', linestyle='--', label=f'Median: {median_interval:.0f}ms')
                        ax2.set_xlabel('Inter-Onset Interval (ms)')
                        ax2.set_ylabel('Count')
                        ax2.set_title(f'Onset Interval Distribution - Est. BPM: {estimated_bpm:.1f}')
                        ax2.legend()
                        ax2.set_xlim(200, 2000)
                        
                        # Update stats
                        stats_text.set_text(f'Onsets: {onset_count}\nEst. BPM: {estimated_bpm:.1f}')
                        
                        print(f"ONSET #{count}: frame={frame}, flux={flux}, interval={intervals_ms[-1]:.0f}ms, BPM≈{estimated_bpm:.1f}")
                    else:
                        print(f"ONSET #{count}: frame={frame}, flux={flux}")
                        
                except (ValueError, IndexError):
                    pass
            
            # Other messages
            elif line and not line.startswith("=") and not line.startswith("Step"):
                print(f"[INFO] {line}")
            
    except KeyboardInterrupt:
        print("\n\nExiting...")
        
        # Final stats
        if len(onset_times) >= 2:
            intervals = np.diff(onset_times) * 1000 / FRAME_RATE
            print(f"\nFinal Statistics:")
            print(f"  Total onsets: {onset_count}")
            print(f"  Mean interval: {np.mean(intervals):.1f} ms")
            print(f"  Median interval: {np.median(intervals):.1f} ms")
            print(f"  Estimated BPM: {60000 / np.median(intervals):.1f}")
            
    finally:
        ser.close()
        plt.close()

if __name__ == "__main__":
    main()
