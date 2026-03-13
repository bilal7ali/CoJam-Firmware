import serial
import struct
import time

# --- Configuration ---
SERIAL_PORT = '/dev/tty.usbmodem3777345E32331'  # Change to /dev/ttyACM0 on Linux/Mac
BAUD_RATE = 115200    # CDC usually ignores this, but 115200 is standard
OUT_FILE = "recorded_audio.csv"

def receive_daisy_data():
    try:
        # Open serial port
        with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=None) as ser:
            print(f"Connected to {SERIAL_PORT}. Waiting for Daisy...")

            # 1. Read the Prefix (Total number of samples as a 32-bit unsigned int)
            # Expecting 4 bytes from: hw.usb_handle.TransmitInternal((uint8_t*)&total_samples, 4);
            raw_size = ser.read(4)
            print("raw size", raw_size);
            if len(raw_size) < 4:
                print("Error: Did not receive size prefix.")
                return

            total_samples = struct.unpack('<I', raw_size)[0]
            print(f"Daisy is sending {total_samples} samples ({total_samples * 4} bytes)...")

            # 2. Read the Payload (The raw float data)
            # We read in chunks to avoid serial buffer overflow on some OSs
            bytes_to_read = total_samples * 4 # float is 4 bytes
            raw_payload = b''
            
            start_time = time.time()
            while len(raw_payload) < bytes_to_read:
                chunk = ser.read(min(bytes_to_read - len(raw_payload), 4096))
                if not chunk:
                    break
                raw_payload += chunk
            
            end_time = time.time()
            print(f"Received {len(raw_payload)} bytes in {end_time - start_time:.2f} seconds.")

            # 3. Unpack bytes into Floats
            # '<' = Little Endian, 'f' = float, '{count}' = number of repeats
            fmt = f"<{total_samples}f"
            audio_data = struct.unpack(fmt, raw_payload)
            
            print(f"Successfully saved to {OUT_FILE}")
            print(f"Samples: {audio_data}")

    except serial.SerialException as e:
        print(f"Serial Error: {e}")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")

if __name__ == "__main__":
    receive_daisy_data()