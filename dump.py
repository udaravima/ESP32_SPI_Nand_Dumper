"""
ESP32 SPI NAND Dumper — PC-side TCP receiver.

Connects to the ESP32 over WiFi TCP, sends a 'GO' trigger,
and streams the raw NAND dump (2112 bytes/page including spare)
to a timestamped binary file in target/.

Usage:
    python3 dump.py

Configuration:
    ESP32_IP  — set to the IP shown in the ESP32 serial monitor
    TCP_PORT  — must match the ESP32 firmware (default 3333)
"""
import socket
import time
import sys
import datetime

# ============ CONFIGURATION ============
# Update ESP32_IP after flashing — it will be printed in the Serial Monitor
ESP32_IP = '10.238.136.57'   # <-- UPDATE THIS
TCP_PORT = 3333

PAGE_SIZE = 2176
TOTAL_PAGES = 2048 * 64
TOTAL_BYTES = TOTAL_PAGES * PAGE_SIZE
OUTPUT_FILE = 'target/ds35_raw_dump_' + datetime.datetime.now().strftime("%Y%m%d_%H%M%S") + '.bin'
PROGRESS_INTERVAL = 1024  # Print progress every N pages
# =======================================

print(f"[*] Connecting to ESP32 at {ESP32_IP}:{TCP_PORT}...")
try:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30)
    sock.connect((ESP32_IP, TCP_PORT))
except Exception as e:
    print(f"[!] Could not connect: {e}")
    sys.exit(1)

print("[+] Connected!")
print("[*] Sending 'GO' trigger...")
sock.sendall(b'G')

print(f"[*] Starting extraction. Expecting {TOTAL_BYTES / (1024*1024):.2f} MB.")
start_time = time.time()
bytes_received = 0

try:
    with open(OUTPUT_FILE, 'wb') as f:
        while bytes_received < TOTAL_BYTES:
            remaining = TOTAL_BYTES - bytes_received
            try:
                chunk = sock.recv(min(65536, remaining))
            except socket.timeout:
                print("\n[!] Socket timeout! ESP32 stopped sending.")
                break

            if not chunk:
                print("\n[!] Connection closed by ESP32.")
                break

            f.write(chunk)
            bytes_received += len(chunk)

            # Progress display
            current_page = bytes_received // PAGE_SIZE
            if current_page % PROGRESS_INTERVAL < (bytes_received - len(chunk)) // PAGE_SIZE % PROGRESS_INTERVAL or current_page // PROGRESS_INTERVAL > (bytes_received - len(chunk)) // PAGE_SIZE // PROGRESS_INTERVAL:
                mb_done = bytes_received / (1024 * 1024)
                pct = bytes_received / TOTAL_BYTES * 100
                elapsed = time.time() - start_time
                speed = mb_done / elapsed if elapsed > 0 else 0
                print(f"\r[>] {mb_done:.1f} / {TOTAL_BYTES/(1024*1024):.0f} MB ({pct:.1f}%) - {speed:.2f} MB/s", end="", flush=True)

    elapsed = time.time() - start_time
    mb_received = bytes_received / (1024 * 1024)
    print(f"\n[*] Dump complete! Saved to {OUTPUT_FILE}")
    print(f"[*] Received {bytes_received} bytes ({mb_received:.2f} MB)")
    if elapsed > 0:
        print(f"[*] Time taken: {elapsed:.2f} seconds ({mb_received / elapsed:.2f} MB/s)")

    if bytes_received < TOTAL_BYTES:
        print(f"[!] WARNING: Expected {TOTAL_BYTES} bytes but only received {bytes_received}")

finally:
    sock.close()
