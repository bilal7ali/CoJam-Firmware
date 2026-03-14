import argparse
import struct
import sys
import serial

SAMPLE_RATE    = 48000
CAPTURE_SEC    = 5
NUM_SAMPLES    = SAMPLE_RATE * CAPTURE_SEC
WAV_HEADER     = 44
BYTES_FLOAT32  = 4
PREAMBLE_BYTES = 8
WAV_BYTES      = WAV_HEADER + NUM_SAMPLES * BYTES_FLOAT32
TOTAL_BYTES    = PREAMBLE_BYTES + WAV_BYTES

MAGIC = bytes([0xC0, 0x4A, 0x41, 0x4D])  # 0xC0 'J' 'A' 'M'


def recv_exact(ser, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            print("\nTimeout.", file=sys.stderr)
            sys.exit(1)
        buf.extend(chunk)
    return bytes(buf)


def receive(port, output_path, timeout_s=30.0):
    print(f"Opening {port}, expecting {TOTAL_BYTES:,} bytes...")

    with serial.Serial(port, baudrate=115200, timeout=timeout_s) as ser:
        # Read and validate preamble
        preamble = recv_exact(ser, PREAMBLE_BYTES)

        if preamble[:4] != MAGIC:
            print(f"Bad magic: {preamble[:4].hex()}", file=sys.stderr)
            sys.exit(1)

        bpm = struct.unpack_from("<f", preamble, 4)[0]
        print(f"Detected BPM: {bpm:.2f}")

        # Read WAV
        received = bytearray()
        while len(received) < WAV_BYTES:
            chunk = ser.read(WAV_BYTES - len(received))
            if not chunk:
                print("\nTimeout waiting for WAV data.", file=sys.stderr)
                sys.exit(1)
            received.extend(chunk)
            pct = 100.0 * len(received) / WAV_BYTES
            print(f"  {len(received):>9,} / {WAV_BYTES:,} bytes  ({pct:.1f}%)", end="\r")

    print()
    with open(output_path, "wb") as f:
        f.write(received)

    print(f"Saved: {output_path}  (BPM={bpm:.2f})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--out", default="capture.wav")
    args = parser.parse_args()
    receive(args.port, args.out)