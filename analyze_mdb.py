"""
analyze_mdb.py - Decompress the MDB from a KB26 file and analyze SystemInfo table
"""
import sys, struct, zlib

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

def read_le16(b, off): return struct.unpack_from('<H', b, off)[0]
def read_le32(b, off): return struct.unpack_from('<I', b, off)[0]
def read_le64(b, off): return struct.unpack_from('<Q', b, off)[0]

def decompress_kb(path):
    data = open(path, 'rb').read()
    # LFH at offset 0, Yayoi 38-byte fixed
    # comp_size  at +18 (uint64)
    # uncomp_size at +26 (uint64)
    # name_len   at +34 (uint16)
    # extra_len  at +36 (uint16)
    comp_size   = read_le64(data, 18)
    uncomp_size = read_le64(data, 26)
    name_len    = read_le16(data, 34)
    extra_len   = read_le16(data, 36)
    data_off = 38 + name_len + extra_len
    print(f"comp_size={comp_size} uncomp_size={uncomp_size} data_off={data_off}")
    compressed = data[data_off:data_off+comp_size]
    mdb = zlib.decompress(compressed, -15)
    print(f"decompressed: {len(mdb)} bytes, {len(mdb)//4096} pages")
    return bytearray(mdb)

PAGE = 4096
DATA_VERSION_UTF16 = bytes([
    0x44,0x00,0x61,0x00,0x74,0x00,0x61,0x00,
    0x56,0x00,0x65,0x00,0x72,0x00,0x73,0x00,
    0x69,0x00,0x6f,0x00,0x6e,0x00
])

def find_sysinfo_tdef(mdb):
    """Find TDEF page containing 'DataVersion' UTF-16LE"""
    n = len(mdb) // PAGE
    for pg in range(n):
        base = pg * PAGE
        if mdb[base] != 0x02: continue
        idx = mdb.find(DATA_VERSION_UTF16, base + 0x50, base + PAGE)
        if idx >= 0:
            print(f"  Found DataVersion in TDEF page {pg} at offset {idx} (page-relative {idx - base})")
            return pg
    return None

def hex_dump(data, off, length, label=""):
    if label: print(f"  {label}:")
    for i in range(0, length, 16):
        chunk = data[off+i:off+i+min(16,length-i)]
        hex_part = ' '.join(f'{b:02X}' for b in chunk)
        asc_part = ''.join(chr(b) if 0x20<=b<0x7F else '.' for b in chunk)
        print(f"    {off+i:08X}: {hex_part:<48} |{asc_part}|")

def analyze_sysinfo_data(mdb, tdef_pg):
    """Find and display all rows in the SystemInfo data page"""
    n = len(mdb) // PAGE
    for pg in range(n):
        base = pg * PAGE
        if mdb[base] != 0x01: continue
        owner = read_le32(mdb, base + 4)
        if owner != tdef_pg: continue

        row_count = read_le16(mdb, base + 12)
        print(f"\nSystemInfo data page {pg} (owner={tdef_pg}, rows={row_count}):")

        for r in range(min(row_count, 50)):
            row_off = read_le16(mdb, base + 14 + r * 2)
            if row_off < 10 or row_off >= PAGE: continue

            row_abs = base + row_off
            if row_abs + 27 > len(mdb): continue

            # Row layout: [0-1] flags, [2-5] Id, [6-9] DataVersion, [10-13] DataStatus
            # [14-17] ShoushinStatus, [18-25] RecTimeStamp, [26] null bitmap
            flags = read_le16(mdb, row_abs)
            id_val = read_le32(mdb, row_abs + 2)
            dv = struct.unpack_from('<i', mdb, row_abs + 6)[0]
            ds = read_le32(mdb, row_abs + 10)

            print(f"  row[{r}] offset={row_off}: flags=0x{flags:04X} Id={id_val} DataVersion={dv} DataStatus={ds}")
            hex_dump(mdb, row_abs, 28, f"row[{r}] raw")

def list_all_tdef_tables(mdb):
    """List all TDEF pages with table names (looking for table name in TDEF)"""
    n = len(mdb) // PAGE
    print(f"\n=== All TDEF pages ({n} pages total) ===")
    found = []
    for pg in range(n):
        base = pg * PAGE
        if mdb[base] != 0x02: continue
        # Table name in TDEF: try to find it as UTF-16LE string around offset 0x28
        # Actually in Jet 4.0 the table name is at a fixed location in the TDEF header
        # Let's dump the beginning of the TDEF page to see structure
        # In Jet 4.0: TDEF page starts with type(1) + unknown(1) + free_space(2) + tdef_pg(4) + ...
        # The table name might be stored as a length-prefixed string
        # Let's do a simple UTF-16LE scan for printable ASCII names
        # Search for length-prefixed UTF-16LE strings in first 512 bytes
        found.append(pg)

    print(f"Total TDEF pages: {len(found)}: {found[:50]}")

    # For each TDEF, search for a "table name" pattern
    # In Jet 4 TDEF, the table name is at byte 40 preceded by a uint16 length
    for pg in found[:80]:
        base = pg * PAGE
        # Try to read name at offset 0x28 (40) as length-prefixed UTF-16LE
        for name_off in [0x28, 0x2A, 0x30, 0x40]:
            try:
                nlen = read_le16(mdb, base + name_off)
                if 2 <= nlen <= 64 and (nlen % 2) == 0:
                    name_bytes = mdb[base + name_off + 2 : base + name_off + 2 + nlen]
                    try:
                        name_str = name_bytes.decode('utf-16-le')
                        if all(c.isprintable() or c == '_' for c in name_str):
                            print(f"  TDEF pg={pg} name@+0x{name_off:02X}: '{name_str}'")
                            break
                    except:
                        pass
            except:
                pass

def check_missing_rows(mdb, tdef_pg):
    """Check which Id values are present in SystemInfo"""
    n = len(mdb) // PAGE
    present_ids = set()
    for pg in range(n):
        base = pg * PAGE
        if mdb[base] != 0x01: continue
        owner = read_le32(mdb, base + 4)
        if owner != tdef_pg: continue
        row_count = read_le16(mdb, base + 12)
        for r in range(min(row_count, 50)):
            row_off = read_le16(mdb, base + 14 + r * 2)
            if row_off < 10 or row_off >= PAGE: continue
            row_abs = base + row_off
            if row_abs + 10 > len(mdb): continue
            id_val = read_le32(mdb, row_abs + 2)
            present_ids.add(id_val)

    ref_ids = {1, 2, 5, 6, 9, 10}  # IDs expected in REF (KB26)
    print(f"\nPresent SystemInfo IDs: {sorted(present_ids)}")
    print(f"Missing IDs (vs REF): {sorted(ref_ids - present_ids)}")
    return present_ids

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else \
        r'C:\Users\rinak\Documents\宮本　宏美　Cafe nest(平成34年度～平成36年度)_繰越処理前.KB26'

    print(f"Analyzing: {path}")
    mdb = decompress_kb(path)

    print("\n=== SystemInfo TDEF ===")
    tdef_pg = find_sysinfo_tdef(mdb)
    if tdef_pg is None:
        print("ERROR: SystemInfo TDEF not found!")
        sys.exit(1)

    analyze_sysinfo_data(mdb, tdef_pg)
    check_missing_rows(mdb, tdef_pg)
    list_all_tdef_tables(mdb)
