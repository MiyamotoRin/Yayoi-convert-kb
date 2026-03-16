#!/usr/bin/env python3
"""Analyze SystemInfo table across REAL KD26, CONVERTED KB26, and second REAL KD26."""
import sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

import struct
import zlib
import os

# ─── MDB parsing helpers ───

def read_file(path):
    with open(path, 'rb') as f:
        return f.read()

def extract_mdb_from_kb(path):
    """Extract the MDB from a Yayoi KB file (modified ZIP with YZ signatures)."""
    data = bytearray(read_file(path))
    # Patch YZ -> PK for all signatures
    # Local file header at offset 0
    if data[0:2] == b'YZ':
        data[0:2] = b'PK'
    # Find and patch all YZ signatures
    i = 0
    while i < len(data) - 1:
        if data[i] == 0x59 and data[i+1] == 0x5A:
            # Check if it looks like a PK signature context (03 04, 01 02, 05 06)
            if i + 3 < len(data) and data[i+2] in (0x01, 0x03, 0x05) and data[i+3] in (0x02, 0x04, 0x06):
                data[i] = 0x50
                data[i+1] = 0x4B
                i += 4
                continue
        i += 1

    # Now parse as ZIP - find local file header and decompress
    # LFH: PK\x03\x04
    if data[0:4] != b'PK\x03\x04':
        print(f"ERROR: Not a valid ZIP/KB file after patching: {data[0:4].hex()}")
        return None

    # Read LFH fields
    comp_method = struct.unpack_from('<H', data, 8)[0]
    comp_size = struct.unpack_from('<Q', data, 18)[0]  # uint64 LE at offset 18
    uncomp_size_field = struct.unpack_from('<Q', data, 26)[0]
    name_len = struct.unpack_from('<H', data, 34)[0]  # uint16 LE at offset 34 (was 26 in standard, but instruction says 34)
    extra_len = struct.unpack_from('<H', data, 36)[0]  # uint16 LE at offset 36

    entry_name = data[38:38+name_len]
    print(f"  Entry name: {entry_name}")
    print(f"  Comp method: {comp_method}, Comp size: {comp_size}, Name len: {name_len}, Extra len: {extra_len}")

    data_offset = 38 + name_len + extra_len
    compressed_data = bytes(data[data_offset:data_offset + comp_size])

    print(f"  Compressed data offset: {data_offset}, length: {len(compressed_data)}")

    # Decompress with raw deflate (wbits=-15)
    try:
        mdb_data = zlib.decompress(compressed_data, -15)
        print(f"  Decompressed MDB size: {len(mdb_data)}")
        return mdb_data
    except Exception as e:
        print(f"  Decompression error: {e}")
        # Try other methods
        for wbits in [15, -15, 31, 47]:
            try:
                mdb_data = zlib.decompress(compressed_data, wbits)
                print(f"  Decompressed with wbits={wbits}, size: {len(mdb_data)}")
                return mdb_data
            except:
                pass
        return None

def find_systeminfo_pages(mdb_data):
    """Find SystemInfo table data in MDB by searching for the table name and data patterns."""
    results = []

    # Strategy 1: Search for "SystemInfo" string occurrences
    name = b'S\x00y\x00s\x00t\x00e\x00m\x00I\x00n\x00f\x00o\x00'  # UTF-16LE "SystemInfo"
    pos = 0
    positions = []
    while True:
        idx = mdb_data.find(name, pos)
        if idx == -1:
            break
        positions.append(idx)
        pos = idx + 1

    print(f"  Found 'SystemInfo' (UTF-16LE) at {len(positions)} positions: {[hex(p) for p in positions]}")

    # Strategy 2: Search for ASCII "SystemInfo"
    name_ascii = b'SystemInfo'
    pos = 0
    ascii_positions = []
    while True:
        idx = mdb_data.find(name_ascii, pos)
        if idx == -1:
            break
        ascii_positions.append(idx)
        pos = idx + 1
    print(f"  Found 'SystemInfo' (ASCII) at {len(ascii_positions)} positions: {[hex(p) for p in ascii_positions]}")

    return positions, ascii_positions

def find_data_rows(mdb_data):
    """Try to find SystemInfo data rows by searching for known patterns.
    We know:
    - Id=1 might have DataVersion like 3600 or 2100
    - Id=2 might have DataVersion like 210101307 or 320004112
    """
    results = []

    # Search for known DataVersion values as int32 LE
    known_values = {
        'DataVersion=3600': struct.pack('<i', 3600),
        'DataVersion=2100': struct.pack('<i', 2100),
        'DataVersion=210101307': struct.pack('<i', 210101307),
        'DataVersion=320004112': struct.pack('<i', 320004112),
    }

    for label, needle in known_values.items():
        pos = 0
        while True:
            idx = mdb_data.find(needle, pos)
            if idx == -1:
                break
            # Print surrounding context
            start = max(0, idx - 32)
            end = min(len(mdb_data), idx + 32)
            context = mdb_data[start:end]
            print(f"  {label} found at offset {hex(idx)}")
            print(f"    Context [{hex(start)}..{hex(end)}]: {context.hex(' ')}")
            # Try to interpret as row: look for Id field nearby
            # Check bytes before for small int (Id=1, 2, etc)
            for back in range(1, 33):
                if idx - back >= 0:
                    val = mdb_data[idx-back]
                    if val in (1, 2, 3) and back <= 16:
                        pass  # Could be Id
            results.append((label, idx))
            pos = idx + 1

    return results

def scan_for_table_data(mdb_data, label):
    """Scan MDB pages for SystemInfo table data."""
    page_size = 4096  # Jet4 default
    num_pages = len(mdb_data) // page_size

    print(f"\n  === Scanning {label} ({len(mdb_data)} bytes, {num_pages} pages) ===")

    # Find SystemInfo references
    positions, ascii_positions = find_systeminfo_pages(mdb_data)

    # Find known DataVersion values
    rows = find_data_rows(mdb_data)

    # Also try to find table definition page
    # In Jet4 MDB, table definitions are in system catalog
    # Let's look for patterns that indicate SystemInfo rows

    # Search for DataVersion as column name (UTF-16LE)
    dv_name = b'D\x00a\x00t\x00a\x00V\x00e\x00r\x00s\x00i\x00o\x00n\x00'
    pos = 0
    dv_positions = []
    while True:
        idx = mdb_data.find(dv_name, pos)
        if idx == -1:
            break
        dv_positions.append(idx)
        pos = idx + 1
    print(f"  Found 'DataVersion' (UTF-16LE) at {len(dv_positions)} positions: {[hex(p) for p in dv_positions]}")

    # Search for DataStatus column name
    ds_name = b'D\x00a\x00t\x00a\x00S\x00t\x00a\x00t\x00u\x00s\x00'
    pos = 0
    ds_positions = []
    while True:
        idx = mdb_data.find(ds_name, pos)
        if idx == -1:
            break
        ds_positions.append(idx)
        pos = idx + 1
    print(f"  Found 'DataStatus' (UTF-16LE) at {len(ds_positions)} positions: {[hex(p) for p in ds_positions]}")

    return rows

def dump_page(mdb_data, page_num, page_size=4096):
    """Dump a page in hex."""
    offset = page_num * page_size
    page = mdb_data[offset:offset+page_size]
    print(f"\n  --- Page {page_num} (offset {hex(offset)}) ---")
    for i in range(0, min(len(page), 256), 16):
        hex_str = ' '.join(f'{b:02x}' for b in page[i:i+16])
        ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in page[i:i+16])
        print(f"    {offset+i:08x}: {hex_str:<48s} {ascii_str}")

def try_pyodbc(path, label):
    """Try reading SystemInfo via ODBC if available."""
    try:
        import pyodbc
        conn_str = (
            r'DRIVER={Microsoft Access Driver (*.mdb, *.accdb)};'
            f'DBQ={path};'
        )
        conn = pyodbc.connect(conn_str)
        cursor = conn.cursor()
        cursor.execute("SELECT * FROM SystemInfo")
        columns = [desc[0] for desc in cursor.description]
        print(f"\n  [{label}] SystemInfo columns: {columns}")
        for row in cursor.fetchall():
            print(f"  Row: {list(row)}")
            # Print hex of each int column
            for i, val in enumerate(row):
                if isinstance(val, int):
                    print(f"    {columns[i]} = {val} (hex: {val & 0xFFFFFFFF:08X})")
        conn.close()
        return True
    except Exception as e:
        print(f"  [{label}] pyodbc failed: {e}")
        return False

def try_mdbtools_parse(mdb_data, label):
    """Parse MDB structure manually to find SystemInfo table data pages."""
    if len(mdb_data) < 4096:
        print(f"  [{label}] MDB too small: {len(mdb_data)}")
        return

    # Jet4 header - page 0
    # Offset 0x14 (20): Jet version
    jet_ver = struct.unpack_from('<I', mdb_data, 0x14)[0]
    print(f"  [{label}] Jet version at 0x14: {hex(jet_ver)}")

    # Page size is typically at offset 0x3C in Jet4
    # But for Jet4, page size is always 4096
    page_size = 4096

    # System catalog is typically on page 2
    # Let's read the table definition area

    # In Jet4, the MSysObjects table starts at page 2
    # Each data page has: page type (1 byte), unknown (1 byte), free_space (2 bytes), tdef_pg (4 bytes)

    # Let's search all data pages for SystemInfo row data
    # A data page has type byte = 0x01 at offset 0
    print(f"\n  [{label}] Scanning data pages...")

    data_pages = []
    for pg in range(1, len(mdb_data) // page_size):
        offset = pg * page_size
        page_type = mdb_data[offset]
        if page_type == 0x01:  # Data page
            data_pages.append(pg)

    print(f"  Found {len(data_pages)} data pages")

    # For each data page, look for SystemInfo content
    for pg in data_pages:
        offset = pg * page_size
        page = mdb_data[offset:offset+page_size]

        # Check if this page contains any SystemInfo-related data
        # Look for known int32 values
        has_match = False
        for val_name, val in [('3600', 3600), ('2100', 2100), ('210101307', 210101307), ('320004112', 320004112)]:
            packed = struct.pack('<i', val)
            if packed in page:
                if not has_match:
                    print(f"\n  Data page {pg} contains SystemInfo data:")
                    has_match = True
                idx = page.find(packed)
                print(f"    Found DataVersion={val_name} at page offset {hex(idx)}")

        if has_match:
            # Dump the full page
            # Page header: type(1) + unknown(1) + free_space(2) + tdef_page(4) + unknown(4) + num_rows(2)
            tdef_pg = struct.unpack_from('<I', page, 4)[0]
            num_rows = struct.unpack_from('<H', page, 12)[0]
            print(f"    tdef_page={tdef_pg}, num_rows={num_rows}")

            # Row offset table is at end of page
            row_offsets = []
            for r in range(num_rows):
                ro = struct.unpack_from('<H', page, page_size - 2 - 2*r - 2)[0]
                row_offsets.append(ro)
            print(f"    Row offsets: {[hex(r) for r in row_offsets]}")

            # Dump first 512 bytes of page
            for i in range(0, min(len(page), page_size), 16):
                hex_str = ' '.join(f'{b:02x}' for b in page[i:i+16])
                ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in page[i:i+16])
                print(f"    {offset+i:08x}: {hex_str:<48s} {ascii_str}")


def analyze_dataversion(val, label):
    """Analyze a DataVersion value."""
    print(f"\n  {label}: {val}")
    print(f"    Hex: {val & 0xFFFFFFFF:08X}")
    print(f"    Bytes LE: {struct.pack('<I', val & 0xFFFFFFFF).hex(' ')}")
    print(f"    Bytes BE: {struct.pack('>I', val & 0xFFFFFFFF).hex(' ')}")

    # Try various splits
    s = str(val)
    print(f"    As string: '{s}'")
    if len(s) >= 4:
        print(f"    Split 2|rest: {s[:2]} | {s[2:]}")
        print(f"    Split 3|rest: {s[:3]} | {s[3:]}")
        print(f"    Split 4|rest: {s[:4]} | {s[4:]}")

    # Check if it encodes version info
    # 210101307 -> maybe 21.01.01307 or 2101.01307
    # 320004112 -> maybe 32.00.04112 or 3200.04112
    hi16 = (val >> 16) & 0xFFFF
    lo16 = val & 0xFFFF
    print(f"    Hi16={hi16} Lo16={lo16}")

    hi8 = (val >> 24) & 0xFF
    mid16 = (val >> 8) & 0xFFFF
    lo8 = val & 0xFF
    print(f"    Byte3={hi8} Mid16={mid16} Byte0={lo8}")


def main():
    REAL_PATH = r'C:\Users\rinak\OneDrive\ドキュメント\Yayoi\弥生会計26データフォルダ\宮本　宏美　Cafe nest(令和06年度～令和08年度).KD26'
    CONV_PATH = r'C:\Users\rinak\Documents\cafe_nest_v3.KB26'
    REAL2_PATH = r'C:\Users\rinak\OneDrive\ドキュメント\Yayoi\弥生会計26データフォルダ\宮本　宏美　不動産(平成34年度～平成36年度).KD26'

    print("=" * 80)
    print("STEP 1: Try ODBC access for all three files")
    print("=" * 80)

    # Try ODBC for REAL KD26
    print("\n--- REAL KD26 (Cafe nest) ---")
    try_pyodbc(REAL_PATH, "REAL")

    # Try ODBC for REAL2 KD26
    print("\n--- REAL2 KD26 (不動産) ---")
    try_pyodbc(REAL2_PATH, "REAL2")

    # For CONVERTED, we need to extract MDB first, save to temp, then ODBC
    print("\n--- CONVERTED KB26 ---")
    # Can't ODBC a KB file directly; need to extract

    print("\n" + "=" * 80)
    print("STEP 2: Raw MDB analysis")
    print("=" * 80)

    # Load REAL KD26 (raw MDB)
    print("\n--- Loading REAL KD26 ---")
    real_mdb = read_file(REAL_PATH)
    print(f"  Size: {len(real_mdb)} bytes")
    scan_for_table_data(real_mdb, "REAL")
    try_mdbtools_parse(real_mdb, "REAL")

    # Load REAL2 KD26
    print("\n--- Loading REAL2 KD26 ---")
    real2_mdb = read_file(REAL2_PATH)
    print(f"  Size: {len(real2_mdb)} bytes")
    scan_for_table_data(real2_mdb, "REAL2")
    try_mdbtools_parse(real2_mdb, "REAL2")

    # Load CONVERTED KB26
    print("\n--- Loading CONVERTED KB26 ---")
    conv_mdb = extract_mdb_from_kb(CONV_PATH)
    if conv_mdb:
        scan_for_table_data(conv_mdb, "CONVERTED")
        try_mdbtools_parse(conv_mdb, "CONVERTED")

        # Also try ODBC on extracted MDB
        import tempfile
        tmp_path = os.path.join(tempfile.gettempdir(), 'conv_systeminfo_test.mdb')
        with open(tmp_path, 'wb') as f:
            f.write(conv_mdb)
        print(f"\n  Saved extracted MDB to {tmp_path}")
        try_pyodbc(tmp_path, "CONVERTED")

    print("\n" + "=" * 80)
    print("STEP 3: DataVersion analysis")
    print("=" * 80)

    analyze_dataversion(210101307, "CONVERTED Id=2 DataVersion")
    analyze_dataversion(320004112, "REAL Id=2 DataVersion")
    analyze_dataversion(3600, "REAL Id=1 DataVersion (expected)")
    analyze_dataversion(2100, "CONVERTED Id=1 DataVersion (original KB12)")

    # Check relationship
    print("\n\n--- Relationship analysis ---")
    print(f"  CONV Id=1: 2100  ->  CONV Id=2: 210101307")
    print(f"  REAL Id=1: 3600  ->  REAL Id=2: 320004112")
    print(f"  Observation: Id=2 value starts with digits from Id=1?")
    print(f"    2100 -> 2101... (first 4 digits of 210101307)")
    print(f"    3600 -> but REAL Id=2 starts with 3200, not 3600")
    print(f"    Maybe Id=1 is patched to 3600 but Id=2 encodes original?")

    # Try: maybe Id=2 encodes (major_version * 10000000 + something)
    print(f"\n  CONV: 210101307 / 10000000 = {210101307 / 10000000:.2f}")
    print(f"  REAL: 320004112 / 10000000 = {320004112 / 10000000:.2f}")

    # Try: first 2 digits as version
    print(f"\n  CONV: first 2 digits = 21 (-> ver 21 = 弥生会計21?)")
    print(f"  REAL: first 2 digits = 32 (-> ver 32 = 弥生会計26+6?)")

    # Maybe it's product_version * 10000000 + schema_checksum
    print(f"\n  CONV: 21 * 10000000 + 0101307 = {21*10000000 + 101307}")
    print(f"  REAL: 32 * 10000000 + 0004112 = {32*10000000 + 4112}")
    print(f"  Actual CONV: {210101307}")
    print(f"  Actual REAL: {320004112}")

    # Check if 21 maps to 弥生会計12 era, 32 maps to 弥生会計26 era
    print(f"\n  If version mapping:")
    print(f"    21 -> KB12/KD12 era product version")
    print(f"    32 -> KB26/KD26 era product version")

    # What if we just need to change 21 -> 32 in the value?
    patched = 320101307
    print(f"\n  If we change prefix 21->32: {patched}")
    print(f"  Real value:                  {320004112}")
    print(f"  Difference:                  {patched - 320004112}")

    # What about the suffix part?
    conv_suffix = 210101307 % 10000000  # 0101307
    real_suffix = 320004112 % 10000000  # 0004112
    print(f"\n  CONV suffix (mod 10M): {conv_suffix}")
    print(f"  REAL suffix (mod 10M): {real_suffix}")
    print(f"  These are different -> suffix might be a checksum or build number")


if __name__ == '__main__':
    main()
