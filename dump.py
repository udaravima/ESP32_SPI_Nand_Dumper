import serial
import time
import sys
import datetime

# Configuration - Update port as needed
PORT = '/dev/ttyUSB0' 
BAUD = 2000000
PAGE_SIZE = 2112
TOTAL_PAGES = 1024 * 64
TOTAL_BYTES = TOTAL_PAGES * PAGE_SIZE
OUTPUT_FILE = 'target/ds35_raw_dump_' + datetime.datetime.now().strftime("%Y%m%d_%H%M%S") + '.bin'
PROGRESS_INTERVAL = 1024  # Print progress every N pages

print(f"[*] Opening {PORT} at {BAUD} baud...")
try:
    ser = serial.Serial(PORT, BAUD, timeout=5)
except Exception as e:
    print(f"[!] Could not open port: {e}")
    sys.exit(1)

try:
    # Reset the ESP32 (DTR toggle)
    ser.setDTR(False)
    time.sleep(0.1)
    ser.setDTR(True)
    time.sleep(2)  # Wait for ESP32 to boot
    ser.reset_input_buffer()

    print("[*] Sending 'GO' trigger to ESP32...")
    ser.write(b'G')

    print(f"[*] Starting extraction. Expecting {TOTAL_BYTES / (1024*1024):.2f} MB.")
    start_time = time.time()
    bytes_received = 0
    last_progress_page = 0

    with open(OUTPUT_FILE, 'wb') as f:
        while bytes_received < TOTAL_BYTES:
            # Read exactly one full page (loop until we have all PAGE_SIZE bytes)
            page_data = b''
            while len(page_data) < PAGE_SIZE:
                remaining = PAGE_SIZE - len(page_data)
                chunk = ser.read(remaining)
                if not chunk:
                    print("\n[!] Serial timeout! ESP32 stopped sending.")
                    break
                page_data += chunk

            if len(page_data) < PAGE_SIZE:
                # Partial page at timeout — write what we have but don't ACK
                if page_data:
                    f.write(page_data)
                    bytes_received += len(page_data)
                break

            f.write(page_data)
            bytes_received += PAGE_SIZE

            # ACK this page — ESP32 will not send next page until it gets this
            ser.write(b'A')

            # Progress bar based on page count
            current_page = bytes_received // PAGE_SIZE
            if current_page // PROGRESS_INTERVAL > last_progress_page // PROGRESS_INTERVAL:
                last_progress_page = current_page
                mb_done = bytes_received / (1024 * 1024)
                pct = bytes_received / TOTAL_BYTES * 100
                print(f"\r[>] Extracted {mb_done:.1f} MB ({pct:.1f}%)...", end="", flush=True)

    elapsed = time.time() - start_time
    mb_received = bytes_received / (1024 * 1024)
    print(f"\n[*] Dump complete! Saved to {OUTPUT_FILE}")
    print(f"[*] Received {bytes_received} bytes ({mb_received:.2f} MB)")
    if elapsed > 0:
        print(f"[*] Time taken: {elapsed:.2f} seconds ({mb_received / elapsed:.2f} MB/s)")
    else:
        print(f"[*] Time taken: <1 second")

    if bytes_received < TOTAL_BYTES:
        print(f"[!] WARNING: Expected {TOTAL_BYTES} bytes but only received {bytes_received}")

finally:
    ser.close()