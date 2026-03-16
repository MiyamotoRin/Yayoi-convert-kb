import sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

import struct
import zlib
import hashlib

PAGE_SIZE = 4096

def read_real_mdb(path):
    with open(path, 'rb') as f:
        return f.read()

def read_converted_mdb(path):
    with open(path, 'rb') as f:
        raw = f.read()
    # Parse local file header to find compressed data
    comp_size = struct.unpack_from('<Q', raw, 18)[0]
    name_len = struct.unpack_from('<H', raw, 34)[0]  # this is for KB format, not standard zip
    extra_len = struct.unpack_from('<H', raw, 36)[0]
    data_offset = 38 + name_len + extra_len
    compressed = raw[data_offset:data_offset + comp_size]
    return zlib.decompress(compressed, -15)

def get_tdef_pages(data):
    """Return dict of page_number -> page_data for all TDEF pages."""
    num_pages = len(data) // PAGE_SIZE
    tdefs = {}
    for i in range(num_pages):
        page = data[i * PAGE_SIZE:(i + 1) * PAGE_SIZE]
        if len(page) >= 1 and page[0] == 0x02:
            tdefs[i] = page
    return tdefs

def fingerprint(page_data):
    """Hash first 128 bytes of TDEF page for comparison."""
    return hashlib.md5(page_data[:128]).hexdigest()

def extract_names(page_data):
    """Extract all length-prefixed UTF-16LE strings from a TDEF page."""
    names = []
    # Scan from offset 0x30 onwards
    offset = 0x30
    end = len(page_data) - 2
    while offset < end:
        # Try reading a uint16 LE as length
        L = struct.unpack_from('<H', page_data, offset)[0]
        if 4 <= L <= 128 and L % 2 == 0 and offset + 2 + L <= len(page_data):
            try:
                s = page_data[offset + 2:offset + 2 + L].decode('utf-16-le')
                # Check if all chars are alphanumeric or underscore
                if len(s) >= 2 and all(c.isalnum() or c == '_' for c in s):
                    names.append(s)
                    offset += 2 + L
                    continue
            except:
                pass
        offset += 1
    return names

def main():
    real_path = r"C:\Users\rinak\OneDrive\ドキュメント\Yayoi\弥生会計26データフォルダ\宮本　宏美　Cafe nest(令和06年度～令和08年度).KD26"
    conv_path = r"C:\Users\rinak\Documents\cafe_nest_v3.KB26"

    print("Reading REAL KD26...")
    real_data = read_real_mdb(real_path)
    print(f"  Size: {len(real_data)} bytes, {len(real_data)//PAGE_SIZE} pages")

    print("Reading CONVERTED KB26...")
    conv_data = read_converted_mdb(conv_path)
    print(f"  Size: {len(conv_data)} bytes, {len(conv_data)//PAGE_SIZE} pages")

    print("\nFinding TDEF pages...")
    real_tdefs = get_tdef_pages(real_data)
    conv_tdefs = get_tdef_pages(conv_data)
    print(f"  REAL: {len(real_tdefs)} TDEF pages")
    print(f"  CONV: {len(conv_tdefs)} TDEF pages")
    print(f"  Difference: {len(real_tdefs) - len(conv_tdefs)} pages")

    # Fingerprint all TDEF pages
    real_fps = {}
    for pg, data in real_tdefs.items():
        fp = fingerprint(data)
        real_fps[pg] = fp

    conv_fps = {}
    for pg, data in conv_tdefs.items():
        fp = fingerprint(data)
        conv_fps[pg] = fp

    conv_fp_set = set(conv_fps.values())

    # Find REAL TDEF pages with no matching fingerprint in CONV
    unmatched = []
    for pg, fp in sorted(real_fps.items()):
        if fp not in conv_fp_set:
            unmatched.append(pg)

    print(f"\nUnmatched TDEF pages in REAL (no fingerprint match in CONV): {len(unmatched)}")

    # Also try matching by extracting names and comparing
    # First, collect all names per TDEF page for both
    print("\n" + "="*70)
    print("ALL TDEF pages in REAL with their extracted names:")
    print("="*70)
    real_page_names = {}
    for pg in sorted(real_tdefs.keys()):
        names = extract_names(real_tdefs[pg])
        real_page_names[pg] = names

    conv_page_names = {}
    for pg in sorted(conv_tdefs.keys()):
        names = extract_names(conv_tdefs[pg])
        conv_page_names[pg] = names

    # Collect all "first names" (likely table names) from each
    # The table name is often the LAST name in the TDEF page
    real_table_candidates = {}
    for pg, names in sorted(real_page_names.items()):
        if names:
            real_table_candidates[pg] = names[-1]  # last name as candidate

    conv_table_candidates = {}
    for pg, names in sorted(conv_page_names.items()):
        if names:
            conv_table_candidates[pg] = names[-1]

    real_tables = set(real_table_candidates.values())
    conv_tables = set(conv_table_candidates.values())

    missing = real_tables - conv_tables
    extra = conv_tables - real_tables

    print(f"\nTable name candidates in REAL: {len(real_tables)}")
    print(f"Table name candidates in CONV: {len(conv_tables)}")
    print(f"Missing from CONV: {len(missing)}")
    print(f"Extra in CONV: {len(extra)}")

    print("\n" + "="*70)
    print("MISSING TABLES (in REAL but not in CONV):")
    print("="*70)
    for name in sorted(missing):
        # Find which page(s) this came from
        pages = [pg for pg, n in real_table_candidates.items() if n == name]
        print(f"  {name}  (TDEF page(s): {pages})")

    if extra:
        print("\n" + "="*70)
        print("EXTRA TABLES (in CONV but not in REAL):")
        print("="*70)
        for name in sorted(extra):
            pages = [pg for pg, n in conv_table_candidates.items() if n == name]
            print(f"  {name}  (TDEF page(s): {pages})")

    # For unmatched pages, show all names found
    print("\n" + "="*70)
    print("DETAILS: Unmatched TDEF pages from REAL (by fingerprint):")
    print("="*70)
    for pg in unmatched:
        names = real_page_names.get(pg, [])
        candidate = real_table_candidates.get(pg, "???")
        in_conv = "YES" if candidate in conv_tables else "NO"
        print(f"  Page {pg}: names={names}, table_candidate='{candidate}', in_conv={in_conv}")

    # Also try: for each TDEF page, use ALL names (not just last) to match
    # Collect name sets
    print("\n" + "="*70)
    print("ALTERNATIVE: Match by ALL names in TDEF page")
    print("="*70)
    # Build a set of all name-lists (as frozensets) for conv
    conv_name_sets = {}
    for pg, names in conv_page_names.items():
        key = frozenset(names)
        conv_name_sets[key] = pg

    unmatched_by_names = []
    for pg in sorted(real_tdefs.keys()):
        names = real_page_names.get(pg, [])
        key = frozenset(names)
        if key not in conv_name_sets:
            unmatched_by_names.append(pg)

    print(f"Unmatched by name-set: {len(unmatched_by_names)}")
    for pg in unmatched_by_names:
        names = real_page_names.get(pg, [])
        print(f"  Page {pg}: {names}")

if __name__ == '__main__':
    main()
