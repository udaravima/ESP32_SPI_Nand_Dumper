"""
ESP32 SPI NAND Dumper — ECC / OOB Stripper.

Post-processes a raw NAND dump (2112 bytes/page) into a clean
firmware image (2048 bytes/page) by stripping the 64-byte
OOB/spare area from each page.

Bad blocks are detected by checking the first byte of the spare
area in each block's first page. Bad block pages are replaced
with 0xFF padding to preserve filesystem alignment.

Usage:
    python3 ecc_stripper.py

Input:  ds35_raw_dump.bin       (raw dump from dump.py)
Output: ds35_clean_firmware.bin (clean, mountable image)
"""
import os

INPUT_FILE = 'target/5th_comp.bin'
OUTPUT_FILE = 'target/5th_comp_clean.bin'

PAGE_SIZE_RAW = 2112
PAGE_SIZE_CLEAN = 2048
PAGES_PER_BLOCK = 64

def process_dump():
    file_size = os.path.getsize(INPUT_FILE)
    total_pages = file_size // PAGE_SIZE_RAW
    
    print(f"[*] Analyzing {INPUT_FILE} ({total_pages} pages found)")
    
    bad_blocks = []
    
    with open(INPUT_FILE, 'rb') as raw_file, open(OUTPUT_FILE, 'wb') as clean_file:
        for page_idx in range(total_pages):
            # Read the full 2112-byte raw page
            raw_page = raw_file.read(PAGE_SIZE_RAW)
            
            # The first page of every block dictates the block's health
            if page_idx % PAGES_PER_BLOCK == 0:
                block_num = page_idx // PAGES_PER_BLOCK
                
                # Byte 2048 is the 1st byte of the spare area (Index 2048 in 0-indexed array)
                bad_block_marker = raw_page[2048]
                
                if bad_block_marker != 0xFF:
                    bad_blocks.append(block_num)
                    print(f"[!] WARNING: Bad Block detected at Block {block_num} (Marker: {hex(bad_block_marker)})")
            
            # Slice off the 64-byte OOB/Spare area and save the pure 2048 bytes
            clean_payload = raw_page[:PAGE_SIZE_CLEAN]
            
            # If the block is bad, it usually contains garbage. 
            # Padding it with 0xFF ensures filesystem offsets remain aligned.
            if (page_idx // PAGES_PER_BLOCK) in bad_blocks:
                clean_file.write(b'\xFF' * PAGE_SIZE_CLEAN)
            else:
                clean_file.write(clean_payload)

    print(f"\n[*] Processing complete!")
    print(f"[*] Found {len(bad_blocks)} Bad Blocks out of 1024 total blocks.")
    print(f"[*] Clean, mountable firmware saved to: {OUTPUT_FILE}")

if __name__ == '__main__':
    process_dump()