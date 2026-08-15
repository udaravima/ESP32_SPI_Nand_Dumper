"""ESP32 SPI NAND Dumper - PC-side TCP receiver.

Connects to the ESP32 over WiFi TCP, sends a 'GO' trigger, reads a 32-byte
geometry header, then streams the raw NAND dump to a timestamped file in
target/ and writes a <dump>.meta.json sidecar describing the geometry.

Usage:
    python3 dump.py

Set ESP32_IP to the address printed in the ESP32 serial monitor.
"""
import socket
import struct
import zlib
import json
import time
import sys
import os
import datetime

# ============ CONFIGURATION ============
ESP32_IP = '10.238.136.57'   # <-- UPDATE THIS (from the serial monitor)
TCP_PORT = 3333
PROGRESS_INTERVAL = 1024      # print progress every N pages
# =======================================

HEADER_FMT = "<6sBBHHHHIBBBBII"
HEADER_SIZE = 32


def parse_header(buf):
    """Parse and CRC-check the 32-byte geometry header. Returns a dict."""
    assert len(buf) == HEADER_SIZE, "short header"
    (magic, ver, flags, page_size, spare_size, ppb, total_blocks,
     total_pages, mfr, dev, pab, _r, total_bytes, crc) = struct.unpack(HEADER_FMT, buf)
    assert magic == b"NANDMP", "bad magic"
    assert ver == 1, "bad proto version"
    assert zlib.crc32(buf[:28]) & 0xFFFFFFFF == crc, "crc mismatch"
    return dict(page_size=page_size, spare_size=spare_size, pages_per_block=ppb,
                total_blocks=total_blocks, total_pages=total_pages, mfr_id=mfr,
                dev_id=dev, page_addr_bits=pab, flags=flags, total_bytes=total_bytes)


def recv_exact(sock, n):
    """Receive exactly n bytes or raise."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise IOError("connection closed mid-header")
        buf += chunk
    return buf


def write_metadata(out_path, geom, byte_count):
    """Write the <out_path>.meta.json sidecar next to the dump."""
    meta = {
        "geometry": geom,
        "ecc_on": bool(geom["flags"] & 0x01),
        "quad": bool(geom["flags"] & 0x02),
        "verify": bool(geom["flags"] & 0x04),
        "bytes_received": byte_count,
        "timestamp": datetime.datetime.now().isoformat(),
    }
    with open(out_path + ".meta.json", "w") as f:
        json.dump(meta, f, indent=2)


def main():
    out_file = ('target/nand_raw_dump_'
                + datetime.datetime.now().strftime("%Y%m%d_%H%M%S") + '.bin')
    os.makedirs(os.path.dirname(out_file) or '.', exist_ok=True)

    print(f"[*] Connecting to ESP32 at {ESP32_IP}:{TCP_PORT}...")
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30)
        sock.connect((ESP32_IP, TCP_PORT))
    except Exception as e:
        print(f"[!] Could not connect: {e}")
        sys.exit(1)

    print("[+] Connected! Sending 'GO' trigger...")
    sock.sendall(b'G')

    geom = parse_header(recv_exact(sock, HEADER_SIZE))
    total_bytes = geom["total_bytes"]
    page_size = geom["page_size"]
    print(f"[*] Chip 0x{geom['mfr_id']:02X} 0x{geom['dev_id']:02X} | "
          f"{geom['total_blocks']} blocks x {geom['pages_per_block']} x {page_size} B "
          f"| ECC {'on' if geom['flags'] & 1 else 'off'} "
          f"| {'quad' if geom['flags'] & 2 else 'single'}")
    print(f"[*] Expecting {total_bytes / (1024*1024):.2f} MB.")

    start_time = time.time()
    bytes_received = 0
    try:
        with open(out_file, 'wb') as f:
            while bytes_received < total_bytes:
                try:
                    chunk = sock.recv(min(65536, total_bytes - bytes_received))
                except socket.timeout:
                    print("\n[!] Socket timeout! ESP32 stopped sending.")
                    break
                if not chunk:
                    print("\n[!] Connection closed by ESP32.")
                    break
                f.write(chunk)
                prev_page = bytes_received // page_size
                bytes_received += len(chunk)
                cur_page = bytes_received // page_size
                if cur_page // PROGRESS_INTERVAL > prev_page // PROGRESS_INTERVAL:
                    mb = bytes_received / (1024 * 1024)
                    pct = bytes_received / total_bytes * 100
                    el = time.time() - start_time
                    spd = mb / el if el > 0 else 0
                    print(f"\r[>] {mb:.1f} / {total_bytes/(1024*1024):.0f} MB "
                          f"({pct:.1f}%) - {spd:.2f} MB/s", end="", flush=True)

        write_metadata(out_file, geom, bytes_received)
        el = time.time() - start_time
        print(f"\n[*] Dump complete! Saved to {out_file}")
        print(f"[*] Metadata: {out_file}.meta.json")
        print(f"[*] Received {bytes_received} bytes in {el:.1f}s")
        if bytes_received < total_bytes:
            print(f"[!] WARNING: expected {total_bytes} but got {bytes_received}")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
