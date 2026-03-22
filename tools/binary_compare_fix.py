#!/usr/bin/env python3
"""
Binary Dump Comparison & Repair Tool
=====================================
Compares multiple binary dump files byte-by-byte and uses majority voting
to identify corrupted bytes and produce a corrected output file.

Usage:
    python3 binary_compare_fix.py <file1> <file2> <file3> [file4 ...] [-o output.bin] [-r report.txt]
"""

import sys
import os
import argparse
import mmap
from collections import Counter
from datetime import datetime

# Process in 1MB chunks for memory efficiency
CHUNK_SIZE = 1024 * 1024


def compare_and_fix(file_paths, output_path, report_path):
    """Compare binary files and generate a corrected file using majority voting."""

    num_files = len(file_paths)
    basenames = [os.path.basename(f) for f in file_paths]

    # Verify all files are the same size
    sizes = [os.path.getsize(f) for f in file_paths]
    if len(set(sizes)) != 1:
        print("ERROR: Files have different sizes!")
        for fp, sz in zip(file_paths, sizes):
            print(f"  {fp}: {sz} bytes")
        sys.exit(1)

    total_size = sizes[0]
    total_chunks = (total_size + CHUNK_SIZE - 1) // CHUNK_SIZE

    print(f"Comparing {num_files} files, each {total_size:,} bytes ({total_size / (1024*1024):.1f} MB)")
    print(f"Files: {', '.join(basenames)}")
    print()

    # Statistics
    total_differences = 0       # byte positions where at least one file disagrees
    majority_fixes = 0          # bytes where majority vote resolved the conflict
    unresolvable = 0            # bytes where no majority exists (tie)
    file_error_counts = [0] * num_files  # per-file disagreement count
    difference_details = []     # list of (offset, values, majority_val, resolved)

    # Open all files
    file_handles = [open(fp, 'rb') for fp in file_paths]

    # Open output file
    out_fh = open(output_path, 'wb')

    try:
        for chunk_idx in range(total_chunks):
            offset_base = chunk_idx * CHUNK_SIZE
            read_size = min(CHUNK_SIZE, total_size - offset_base)

            # Read chunk from each file
            chunks = []
            for fh in file_handles:
                data = fh.read(read_size)
                if len(data) != read_size:
                    raise IOError(f"Unexpected short read from {fh.name}")
                chunks.append(data)

            # Build output chunk via majority vote
            output_chunk = bytearray(read_size)

            for i in range(read_size):
                byte_values = [c[i] for c in chunks]

                if len(set(byte_values)) == 1:
                    # All agree
                    output_chunk[i] = byte_values[0]
                else:
                    # Disagreement found
                    total_differences += 1
                    abs_offset = offset_base + i

                    counter = Counter(byte_values)
                    most_common_val, most_common_count = counter.most_common(1)[0]

                    # Check if there's a clear majority (more than any other single value)
                    if most_common_count > 1 or num_files == 2:
                        # For 2 files we can only flag, not resolve; pick first file's value
                        if num_files == 2:
                            output_chunk[i] = byte_values[0]
                            resolved = False
                            unresolvable += 1
                        else:
                            # Check for tie: if top two have equal count, it's unresolvable
                            top_two = counter.most_common(2)
                            if len(top_two) > 1 and top_two[0][1] == top_two[1][1]:
                                output_chunk[i] = byte_values[0]  # fallback to first file
                                resolved = False
                                unresolvable += 1
                            else:
                                output_chunk[i] = most_common_val
                                resolved = True
                                majority_fixes += 1
                    else:
                        # All different (only possible with 3+ files, all unique)
                        output_chunk[i] = byte_values[0]  # fallback
                        resolved = False
                        unresolvable += 1

                    # Track which files were wrong
                    for fi, bv in enumerate(byte_values):
                        if bv != output_chunk[i]:
                            file_error_counts[fi] += 1

                    difference_details.append((
                        abs_offset,
                        byte_values,
                        output_chunk[i],
                        resolved
                    ))

            out_fh.write(output_chunk)

            # Progress indicator
            pct = (chunk_idx + 1) * 100 // total_chunks
            print(f"\r  Processing: {pct}% ({chunk_idx + 1}/{total_chunks} chunks)", end='', flush=True)

    finally:
        for fh in file_handles:
            fh.close()
        out_fh.close()

    print()
    print()

    # Generate report
    report_lines = []
    report_lines.append("=" * 80)
    report_lines.append("  BINARY DUMP COMPARISON & REPAIR REPORT")
    report_lines.append("=" * 80)
    report_lines.append(f"  Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    report_lines.append(f"  File size:  {total_size:,} bytes ({total_size / (1024*1024):.1f} MB)")
    report_lines.append(f"  Files compared: {num_files}")
    for i, fp in enumerate(file_paths):
        report_lines.append(f"    [{i+1}] {fp}")
    report_lines.append(f"  Output:     {output_path}")
    report_lines.append("")
    report_lines.append("-" * 80)
    report_lines.append("  SUMMARY")
    report_lines.append("-" * 80)
    report_lines.append(f"  Total byte positions with disagreement:  {total_differences:,}")
    report_lines.append(f"  Resolved by majority vote:               {majority_fixes:,}")
    report_lines.append(f"  Unresolvable (tie / no majority):        {unresolvable:,}")
    report_lines.append(f"  Error rate:  {total_differences / total_size * 100:.6f}%")
    report_lines.append("")
    report_lines.append("  Per-file error counts (bytes differing from consensus):")
    for i, (bn, ec) in enumerate(zip(basenames, file_error_counts)):
        report_lines.append(f"    [{i+1}] {bn}: {ec:,} errors ({ec / total_size * 100:.6f}%)")
    report_lines.append("")

    # Difference details (capped at 5000 for readability, full list if smaller)
    report_lines.append("-" * 80)
    report_lines.append("  DIFFERENCE DETAILS")
    report_lines.append("-" * 80)

    max_detail = 5000
    shown = min(len(difference_details), max_detail)
    if len(difference_details) > max_detail:
        report_lines.append(f"  (Showing first {max_detail} of {len(difference_details):,} differences)")
    report_lines.append("")

    # Header
    file_hdr = "  ".join([f"[{i+1}]" for i in range(num_files)])
    report_lines.append(f"  {'Offset':>12}  {file_hdr}   Result  Status")
    report_lines.append(f"  {'------':>12}  {'---  ' * num_files}  ------  ------")

    for idx in range(shown):
        offset, values, result, resolved = difference_details[idx]
        vals_str = "  ".join([f" {v:02X}" for v in values])
        status = "FIXED" if resolved else "UNRESOLVED"
        report_lines.append(f"  0x{offset:08X}  {vals_str}     {result:02X}  {status}")

    if len(difference_details) > max_detail:
        report_lines.append(f"  ... and {len(difference_details) - max_detail:,} more differences")

    report_lines.append("")
    report_lines.append("=" * 80)

    # Also generate a per-page/block summary to identify problematic regions
    if total_differences > 0:
        report_lines.append("")
        report_lines.append("-" * 80)
        report_lines.append("  ERROR DISTRIBUTION BY 2KB PAGE")
        report_lines.append("-" * 80)
        PAGE_SIZE = 2048
        page_errors = {}
        for offset, values, result, resolved in difference_details:
            page = offset // PAGE_SIZE
            page_errors[page] = page_errors.get(page, 0) + 1

        # Show pages sorted by error count (top 100)
        sorted_pages = sorted(page_errors.items(), key=lambda x: x[1], reverse=True)
        report_lines.append(f"  Total pages with errors: {len(sorted_pages):,}")
        report_lines.append(f"  (Showing top 100 most affected pages)")
        report_lines.append("")
        report_lines.append(f"  {'Page #':>8}  {'Offset':>14}  {'Errors':>8}")
        report_lines.append(f"  {'------':>8}  {'------':>14}  {'------':>8}")
        for page_num, count in sorted_pages[:100]:
            page_offset = page_num * PAGE_SIZE
            report_lines.append(f"  {page_num:>8}  0x{page_offset:08X}    {count:>8}")

        report_lines.append("")
        report_lines.append("=" * 80)

    report_text = "\n".join(report_lines)

    # Write report
    with open(report_path, 'w') as rf:
        rf.write(report_text)

    # Print summary to console
    print(report_text)
    print()
    print(f"Report saved to: {report_path}")
    print(f"Corrected binary saved to: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Compare multiple binary dumps and fix errors via majority voting"
    )
    parser.add_argument("files", nargs="+", help="Binary dump files to compare (at least 2)")
    parser.add_argument("-o", "--output", default=None,
                        help="Output corrected binary file (default: target/ds35_corrected.bin)")
    parser.add_argument("-r", "--report", default=None,
                        help="Output report file (default: target/comparison_report.txt)")

    args = parser.parse_args()

    if len(args.files) < 2:
        print("ERROR: Need at least 2 files to compare")
        sys.exit(1)

    # Verify files exist
    for fp in args.files:
        if not os.path.isfile(fp):
            print(f"ERROR: File not found: {fp}")
            sys.exit(1)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_path = args.output or f"target/ds35_corrected_{timestamp}.bin"
    report_path = args.report or f"target/comparison_report_{timestamp}.txt"

    compare_and_fix(args.files, output_path, report_path)


if __name__ == "__main__":
    main()
