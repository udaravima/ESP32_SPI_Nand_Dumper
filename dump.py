"""ESP32 SPI NAND Dumper - PC-side TCP receiver.

Connects to the ESP32 over WiFi TCP, sends a 'GO' trigger, reads a 32-byte
geometry header, then streams the raw NAND dump to a timestamped file in
target/ and writes a <dump>.meta.json sidecar describing the geometry.

Usage:
    python3 dump.py

Set ESP32_IP to the address printed in the ESP32 serial monitor.
"""
import argparse
import socket
import struct
import zlib
import json
import time
import sys
import os
import datetime
from collections import namedtuple

# ============ CONFIGURATION ============
# Precedence at runtime: CLI flag > dump.config.json > these defaults.
# Pass --ip/--port once and they are remembered in dump.config.json (local,
# gitignored) so later runs need no flags.
DEFAULT_IP = '10.65.224.57'   # first-run fallback; override with --ip
DEFAULT_PORT = 3333
DEFAULT_OUT_DIR = 'target'
CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'dump.config.json')
PROGRESS_INTERVAL = 1024      # print progress every N pages
# =======================================


def format_duration(seconds):
    """Format a duration as H:MM:SS, or MM:SS when under an hour."""
    seconds = int(seconds)
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m:02d}:{s:02d}"


def load_config(path=CONFIG_PATH):
    """Return the saved config dict, or {} if missing/unreadable/corrupt."""
    try:
        with open(path) as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def save_config(path, cfg):
    """Persist the resolved connection settings for next time."""
    with open(path, 'w') as f:
        json.dump(cfg, f, indent=2)


def resolve_config(cli, saved):
    """Merge with precedence CLI > saved > default. A None in `cli` (argparse
    leaves unspecified flags None) is ignored so it can't clobber a saved value."""
    def pick(key, default):
        if cli.get(key) is not None:
            return cli[key]
        if saved.get(key) is not None:
            return saved[key]
        return default
    return {"ip": pick("ip", DEFAULT_IP),
            "port": pick("port", DEFAULT_PORT),
            "out_dir": pick("out_dir", DEFAULT_OUT_DIR)}

HEADER_FMT = "<6sBBHHHHIBBBBII"
HEADER_SIZE = 32

# Header flag bits (must match src/dump_header.h)
FLAG_ECC_ON = 0x01
FLAG_QUAD = 0x02
FLAG_VERIFY = 0x04
FLAG_PAGECRC = 0x08   # proto v2: each page followed by a 4-byte CRC32 seal

# Result of streaming a dump off the wire.
ReceiveResult = namedtuple(
    "ReceiveResult", "bytes_received pages_received bad_pages truncated")


def parse_header(buf):
    """Parse and CRC-check the 32-byte geometry header. Returns a dict."""
    assert len(buf) == HEADER_SIZE, "short header"
    (magic, ver, flags, page_size, spare_size, ppb, total_blocks,
     total_pages, mfr, dev, pab, _r, total_bytes, crc) = struct.unpack(HEADER_FMT, buf)
    assert magic == b"NANDMP", "bad magic"
    assert ver in (1, 2), "bad proto version"
    assert zlib.crc32(buf[:28]) & 0xFFFFFFFF == crc, "crc mismatch"
    return dict(page_size=page_size, spare_size=spare_size, pages_per_block=ppb,
                total_blocks=total_blocks, total_pages=total_pages, mfr_id=mfr,
                dev_id=dev, page_addr_bits=pab, flags=flags, total_bytes=total_bytes,
                proto_version=ver)


def recv_exact(sock, n):
    """Receive exactly n bytes or raise."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise IOError("connection closed mid-header")
        buf += chunk
    return buf


def receive_pages(sock, geom, fout, progress=None):
    """Stream a dump off `sock` into file object `fout`.

    Proto v2 (FLAG_PAGECRC set): each page arrives as [page_size data bytes]
    [4-byte CRC32 of that data]. We verify the seal, record any page whose CRC
    mismatches in `bad_pages`, and ALWAYS write the data (even when bad) so a
    later majority vote across dumps still has the bytes. Proto v1: a raw byte
    stream with no per-page seal.

    `progress`, if given, is called as progress(pages_received, bytes_received).
    Returns a ReceiveResult. Sets `truncated` if the stream ended early.
    """
    page_size = geom["page_size"]
    total_pages = geom["total_pages"]
    total_bytes = geom["total_bytes"]

    if geom["flags"] & FLAG_PAGECRC:
        bad_pages, pages_received, bytes_received, truncated = [], 0, 0, False
        for i in range(total_pages):
            try:
                frame = recv_exact(sock, page_size + 4)
            except (socket.timeout, OSError):
                truncated = True
                break
            data = frame[:page_size]
            (crc_rx,) = struct.unpack("<I", frame[page_size:])
            if zlib.crc32(data) & 0xFFFFFFFF != crc_rx:
                bad_pages.append(i)
            fout.write(data)
            pages_received += 1
            bytes_received += page_size
            if progress:
                progress(pages_received, bytes_received)
        return ReceiveResult(bytes_received, pages_received, bad_pages, truncated)

    # Proto v1: unframed byte stream.
    bytes_received, truncated = 0, False
    while bytes_received < total_bytes:
        try:
            chunk = sock.recv(min(65536, total_bytes - bytes_received))
        except socket.timeout:
            truncated = True
            break
        if not chunk:
            truncated = True
            break
        fout.write(chunk)
        bytes_received += len(chunk)
        if progress:
            progress(bytes_received // page_size, bytes_received)
    return ReceiveResult(bytes_received, bytes_received // page_size, [], truncated)


def write_badpages(out_path, bad_pages, total_pages):
    """Write the <out_path>.badpages.json sidecar listing pages that failed
    their CRC seal. Only called when there is at least one bad page."""
    with open(out_path + ".badpages.json", "w") as f:
        json.dump({"total_pages": total_pages,
                   "bad_page_count": len(bad_pages),
                   "bad_pages": bad_pages}, f)


def write_metadata(out_path, geom, byte_count, result=None):
    """Write the <out_path>.meta.json sidecar next to the dump."""
    meta = {
        "geometry": geom,
        "ecc_on": bool(geom["flags"] & FLAG_ECC_ON),
        "quad": bool(geom["flags"] & FLAG_QUAD),
        "verify": bool(geom["flags"] & FLAG_VERIFY),
        "page_crc": bool(geom["flags"] & FLAG_PAGECRC),
        "proto_version": geom.get("proto_version"),
        "bytes_received": byte_count,
        "timestamp": datetime.datetime.now().isoformat(),
    }
    if result is not None:
        meta["bad_page_count"] = len(result.bad_pages)
        meta["truncated"] = result.truncated
    with open(out_path + ".meta.json", "w") as f:
        json.dump(meta, f, indent=2)


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="ESP32 SPI NAND dumper — PC-side TCP receiver.")
    ap.add_argument("--ip", help="ESP32 IP address (shown in the serial monitor).")
    ap.add_argument("--port", type=int, help="TCP port (default 3333).")
    ap.add_argument("--out-dir", dest="out_dir",
                    help="Directory for dumps (default target/).")
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    cli = {"ip": args.ip, "port": args.port, "out_dir": args.out_dir}
    cfg = resolve_config(cli, load_config())
    # Any flag the user passed is remembered, so next run needs no flags.
    if any(v is not None for v in cli.values()):
        save_config(CONFIG_PATH, cfg)
        print(f"[*] Saved connection settings to {os.path.basename(CONFIG_PATH)}")
    ip, port, out_dir = cfg["ip"], cfg["port"], cfg["out_dir"]

    out_file = os.path.join(
        out_dir, 'nand_raw_dump_'
        + datetime.datetime.now().strftime("%Y%m%d_%H%M%S") + '.bin')
    os.makedirs(os.path.dirname(out_file) or '.', exist_ok=True)

    print(f"[*] Connecting to ESP32 at {ip}:{port}...")
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30)
        sock.connect((ip, port))
    except Exception as e:
        print(f"[!] Could not connect: {e}")
        sys.exit(1)

    print("[+] Connected! Sending 'GO' trigger...")
    sock.sendall(b'G')

    geom = parse_header(recv_exact(sock, HEADER_SIZE))
    total_bytes = geom["total_bytes"]
    total_mb = total_bytes / (1024 * 1024)
    page_size = geom["page_size"]
    pagecrc = bool(geom["flags"] & FLAG_PAGECRC)
    print(f"[*] Chip 0x{geom['mfr_id']:02X} 0x{geom['dev_id']:02X} | "
          f"{geom['total_blocks']} blocks x {geom['pages_per_block']} x {page_size} B "
          f"| ECC {'on' if geom['flags'] & FLAG_ECC_ON else 'off'} "
          f"| {'quad' if geom['flags'] & FLAG_QUAD else 'single'}")
    print(f"[*] Proto v{geom['proto_version']} | "
          f"per-page CRC {'on' if pagecrc else 'off'} | expecting {total_mb:.2f} MB.")

    start_time = time.time()

    def progress(pages_done, bytes_done):
        if pages_done % PROGRESS_INTERVAL:
            return
        mb = bytes_done / (1024 * 1024)
        el = time.time() - start_time
        rate = bytes_done / el if el > 0 else 0            # bytes/sec
        eta = (total_bytes - bytes_done) / rate if rate > 0 else 0
        print(f"\r[>] {mb:.1f}/{total_mb:.0f} MB "
              f"({bytes_done / total_bytes * 100:.1f}%) {rate / (1024*1024):.2f} MB/s | "
              f"elapsed {format_duration(el)} | ETA {format_duration(eta)}   ",
              end="", flush=True)

    try:
        with open(out_file, 'wb') as f:
            res = receive_pages(sock, geom, f, progress)

        write_metadata(out_file, geom, res.bytes_received, res)
        el = time.time() - start_time
        avg = res.bytes_received / el / (1024 * 1024) if el > 0 else 0
        print(f"\n[*] Dump complete! Saved to {out_file}")
        print(f"[*] Metadata: {out_file}.meta.json")
        print(f"[*] Received {res.bytes_received} bytes "
              f"({res.pages_received}/{geom['total_pages']} pages) in "
              f"{format_duration(el)} ({avg:.2f} MB/s avg)")
        if res.truncated or res.bytes_received < total_bytes:
            print(f"[!] WARNING: truncated — expected {total_bytes} "
                  f"but got {res.bytes_received} bytes.")
        if pagecrc:
            if res.bad_pages:
                write_badpages(out_file, res.bad_pages, geom["total_pages"])
                print(f"[!] {len(res.bad_pages)} page(s) FAILED their CRC seal — "
                      f"see {out_file}.badpages.json")
            else:
                print("[+] All pages passed their CRC seal.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
