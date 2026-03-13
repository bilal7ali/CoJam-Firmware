import serial
import struct
import time

# Open Serial Port
ser = serial.Serial('COM1', 115200) # Use /dev/tty.usbmodem... on Mac/Linux

def send_audio_chunk(float_list):
    # 'f' tells struct to pack each number as a 32-bit float
    # '<' ensures "Little Endian" (which STM32 uses)
    binary_data = struct.pack(f'<{len(float_list)}f', *float_list)
    
    # Send a start byte so Daisy knows a chunk is coming
    ser.write(b'\x01') 
    ser.write(binary_data)

# Example: Send a small sine wave chunk
chunk = [0.5, 0.2, -0.1, -0.5] 
send_audio_chunk(chunk)