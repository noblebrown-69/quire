#!/usr/bin/env python3
"""Interleave odd pages from recto PDF with even pages from verso PDF."""
import sys
from pypdf import PdfReader, PdfWriter

def main():
    if len(sys.argv) != 4:
        print('usage: mirror_merge_pdf.py RECTO VERSO OUT', file=sys.stderr)
        return 2
    recto, verso, out = sys.argv[1:]
    r = PdfReader(recto)
    v = PdfReader(verso)
    if len(r.pages) != len(v.pages) or len(r.pages) < 1:
        print(f'page count mismatch {len(r.pages)} vs {len(v.pages)}', file=sys.stderr)
        return 1
    w = PdfWriter()
    for i in range(len(r.pages)):
        w.add_page(r.pages[i] if (i % 2 == 0) else v.pages[i])  # 0-based: even idx = odd page
    w.write(out)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
