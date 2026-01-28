#!/usr/bin/env python3
"""
Step 4: BPM Detection Visualizer

Reads serial output:
    FLUX,<flux>,<threshold>,<onset>
    BPM,<bpm>,<confidence>,<raw_bpm>
    ONSET,<frame>,<flux>,<count>
    ACORR,<corr0>,<corr1>,...

Displays:
- Real-time BPM tracking
- Confidence meter
- Onset detection visualization

Usage:
    python3 bpm_visualize.py /dev/tty.usbmodemXXXX

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
HISTORY_SIZE = 500
FRAME_RATE = 93.75
LAG_MIN = 31
LAG_MAX = 94

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 bpm_visualize.py <serial_port>")
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
    bpm_history = deque(maxlen=HISTORY_SIZE)
    onset_times = []
    
    # Initialize
    for _ in range(HISTORY_SIZE):
        flux_history.append(0)
        threshold_history.append(0)
        bpm_history.append(0)

    # Setup plot
    plt.ion()
    fig = plt.figure(figsize=(14, 10))
    
    # Subplot layout
    ax1 = plt.subplot2grid((3, 2), (0, 0), colspan=2)  # Flux
    ax2 = plt.subplot2grid((3, 2), (1, 0), colspan=2)  # BPM
    ax3 = plt.subplot2grid((3, 2), (2, 0))              # BPM histogram
    ax4 = plt.subplot2grid((3, 2), (2, 1))              # Stats
    
    # Time axis
    time_axis = np.linspace(-HISTORY_SIZE / FRAME_RATE, 0, HISTORY_SIZE)
    
    # Flux plot
    flux_line, = ax1.plot(time_axis, list(flux_history), 'b-', label='Flux', linewidth=1)
    thresh_line, = ax1.plot(time_axis, list(threshold_history), 'r--', label='Threshold', linewidth=1)
    ax1.set_xlabel('Time (seconds)')
    ax1.set_ylabel('Spectral Flux')
    ax1.set_title('Onset Detection')
    ax1.legend(loc='upper left')
    ax1.grid(True, alpha=0.3)
    ax1.set_xlim(-HISTORY_SIZE / FRAME_RATE, 0)
    
    # BPM plot
    bpm_line, = ax2.plot(time_axis, list(bpm_history), 'g-', linewidth=2)
    ax2.set_xlabel('Time (seconds)')
    ax2.set_ylabel('BPM')
    ax2.set_title('Detected BPM')
    ax2.grid(True, alpha=0.3)
    ax2.set_xlim(-HISTORY_SIZE / FRAME_RATE, 0)
    ax2.set_ylim(60, 180)
    ax2.axhline(y=120, color='gray', linestyle=':', alpha=0.5, label='120 BPM')
    
    # Target BPM line (will be updated)
    target_line = ax2.axhline(y=0, color='red', linestyle='--', alpha=0.7, label='Target')
    
    # BPM histogram
    ax3.set_xlabel('BPM')
    ax3.set_ylabel('Count')
    ax3.set_title('BPM Distribution')
    ax3.set_xlim(60, 180)
    
    # Stats panel (text)
    ax4.axis('off')
    stats_text = ax4.text(0.1, 0.9, '', transform=ax4.transAxes,
                          fontsize=14, verticalalignment='top', fontfamily='monospace',
                          bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.9))
    
    plt.tight_layout()

    # State
    current_bpm = 0
    current_conf = 0
    raw_bpm = 0
    frame_count = 0
    all_bpms = []
    
    try:
        while True:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            
            if not line:
                continue
            
            # Parse FLUX
            if line.startswith("FLUX,"):
                parts = line.split(",")
                try:
                    flux = int(parts[1])
                    threshold = int(parts[2])
                    onset = int(parts[3])
                    
                    flux_history.append(flux)
                    threshold_history.append(threshold)
                    frame_count += 1
                    
                    if onset:
                        onset_times.append(frame_count)
                    
                except (ValueError, IndexError):
                    pass
            
            # Parse BPM
            elif line.startswith("BPM,"):
                parts = line.split(",")
                try:
                    # Format: BPM,XX.X,conf,YY.Y
                    bpm_str = parts[1]
                    current_bpm = float(bpm_str)
                    current_conf = int(parts[2])
                    raw_str = parts[3]
                    raw_bpm = float(raw_str)
                    
                    bpm_history.append(current_bpm)
                    
                    if current_bpm > 0:
                        all_bpms.append(current_bpm)
                    
                    # Update plots every BPM message
                    flux_line.set_ydata(list(flux_history))
                    thresh_line.set_ydata(list(threshold_history))
                    bpm_line.set_ydata(list(bpm_history))
                    
                    # Auto-scale flux
                    max_flux = max(max(flux_history), max(threshold_history))
                    if max_flux > 0:
                        ax1.set_ylim(0, max_flux * 1.2)
                    
                    # Auto-scale BPM (with padding)
                    valid_bpms = [b for b in bpm_history if b > 0]
                    if valid_bpms:
                        min_bpm = max(60, min(valid_bpms) - 10)
                        max_bpm = min(180, max(valid_bpms) + 10)
                        ax2.set_ylim(min_bpm, max_bpm)
                    
                    # Update histogram
                    if len(all_bpms) > 10:
                        ax3.clear()
                        ax3.hist(all_bpms[-200:], bins=30, range=(60, 180), 
                                color='steelblue', alpha=0.7)
                        ax3.set_xlabel('BPM')
                        ax3.set_ylabel('Count')
                        ax3.set_title('BPM Distribution')
                        
                        # Add median line
                        median_bpm = np.median(all_bpms[-200:])
                        ax3.axvline(x=median_bpm, color='red', linestyle='--', 
                                   label=f'Median: {median_bpm:.1f}')
                        ax3.legend()
                    
                    # Update stats
                    stats = f"""
Current BPM:  {current_bpm:.1f}
Raw BPM:      {raw_bpm:.1f}
Confidence:   {current_conf}%

Median BPM:   {np.median(all_bpms[-100:]) if all_bpms else 0:.1f}
Std Dev:      {np.std(all_bpms[-100:]) if len(all_bpms) > 1 else 0:.1f}

Onsets:       {len(onset_times)}
Frames:       {frame_count}
"""
                    stats_text.set_text(stats)
                    
                    plt.pause(0.001)
                    
                except (ValueError, IndexError) as e:
                    print(f"Parse error: {e}, line: {line}")
            
            # Parse ONSET
            elif line.startswith("ONSET,"):
                parts = line.split(",")
                try:
                    frame = int(parts[1])
                    flux = int(parts[2])
                    count = int(parts[3])
                    print(f"ONSET #{count}: frame={frame}")
                except:
                    pass
            
            # Other info
            elif line and not line.startswith("="):
                print(f"[INFO] {line}")
            
    except KeyboardInterrupt:
        print("\n\nExiting...")
        
        if all_bpms:
            print(f"\nFinal Statistics:")
            print(f"  Median BPM: {np.median(all_bpms):.1f}")
            print(f"  Mean BPM:   {np.mean(all_bpms):.1f}")
            print(f"  Std Dev:    {np.std(all_bpms):.1f}")
            print(f"  Min BPM:    {min(all_bpms):.1f}")
            print(f"  Max BPM:    {max(all_bpms):.1f}")
        
    finally:
        ser.close()
        plt.close()

if __name__ == "__main__":
    main()