"""
investigate_tdef.py - Compare SystemInfo TDEF and data page headers
between REAL KD26 and CONVERTED KB26 files.
"""
import sys, struct, zlib

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

def read_le16(b, off): return struct.unpack_from('<H', b, off)[0]
def read_le32(b, off): return struct.unpack_from('<I', b, off)[0]
def read_le64(b, off): return struct.unpack_from('<Q', b, off)[0]

PAGE = 4096

DATA_VERSION_UTF16 = bytes([
    0x44,0x00,0x61,0x00,0x74,0x00,0x61,0x00,
    0x56,0x00,0x65,0x00,0x72,0x00,0x73,0x00,
    0x69,0x00,0x6f,0x00,0x6e,0x00
])

def decompress_kb(path):
    data = open(path, 'rb').read()
    comp_size   = read_le64(data, 18)
    name_len    = read_le16(data, 34)
    extra_len   = read_le16(data, 36)
    data_off = 38 + name_len + extra_len
    compressed = data[data_off:data_off+comp_size]
    mdb = zlib.decompress(compressed, -15)
    return bytearray(mdb)

def load_real_kd(path):
    return bytearray(open(path, 'rb').read())

def find_sysinfo_tdef(mdb):
    """Find TDEF page containing 'DataVersion' UTF-16LE"""
    n = len(mdb) // PAGE
    for pg in range(n):
        base = pg * PAGE
        if mdb[base] != 0x02:
            continue
        idx = mdb.find(DATA_VERSION_UTF16, base + 0x50, base + PAGE)
        if idx >= 0:
            return pg
    return None

def find_sysinfo_data_pages(mdb, tdef_page):
    """Find data pages (type 0x01) that belong to the SystemInfo table.
    In Jet 4.0, data page offset 4-7 has the TDEF page number (owner)."""
    n = len(mdb) // PAGE
    pages = []
    for pg in range(n):
        base = pg * PAGE
        if mdb[base] != 0x01:
            continue
        owner = read_le32(mdb, base + 4)
        if owner == tdef_page:
            pages.append(pg)
    return pages

def hex_dump(data, off, length, label=""):
    if label:
        print(f"  {label}:")
    for i in range(0, length, 16):
        hex_part = ' '.join(f'{data[off+i+j]:02x}' for j in range(min(16, length - i)))
        ascii_part = ''.join(chr(data[off+i+j]) if 32 <= data[off+i+j] < 127 else '.'
                             for j in range(min(16, length - i)))
        print(f"    {i:04x}: {hex_part:<48s}  {ascii_part}")

def analyze_tdef(mdb, pg, label):
    base = pg * PAGE
    print(f"\n{'='*60}")
    print(f"  {label} - SystemInfo TDEF page {pg} (offset 0x{base:x})")
    print(f"{'='*60}")
    hex_dump(mdb, base, 64, "First 64 bytes")

    page_type   = mdb[base]
    free_space  = read_le16(mdb, base + 2)
    tdef_len    = read_le32(mdb, base + 4)  # or next pointer
    next_tdef   = read_le32(mdb, base + 8)
    total_rows  = read_le32(mdb, base + 12)
    autonumber  = read_le32(mdb, base + 16)
    auto_inc    = read_le32(mdb, base + 20)

    print(f"\n  TDEF Header Fields:")
    print(f"    [0]      page_type     = 0x{page_type:02x}")
    print(f"    [2:4]    free_space    = {free_space} (0x{free_space:04x})")
    print(f"    [4:8]    tdef_len/next = {tdef_len} (0x{tdef_len:08x})")
    print(f"    [8:12]   next_tdef_pg  = {next_tdef} (0x{next_tdef:08x})")
    print(f"    [12:16]  total_rows    = {total_rows}  <-- CRITICAL")
    print(f"    [16:20]  autonumber    = {autonumber}")
    print(f"    [20:24]  auto_inc      = {auto_inc}")

    return {
        'page_type': page_type,
        'free_space': free_space,
        'tdef_len': tdef_len,
        'next_tdef': next_tdef,
        'total_rows': total_rows,
        'autonumber': autonumber,
        'auto_inc': auto_inc,
    }

def analyze_data_page(mdb, pg, label):
    base = pg * PAGE
    print(f"\n  {label} - Data page {pg} (offset 0x{base:x})")
    hex_dump(mdb, base, 32, "First 32 bytes")

    page_type  = mdb[base]
    free_space = read_le16(mdb, base + 2)
    owner      = read_le32(mdb, base + 4)
    row_count  = read_le16(mdb, base + 12)

    print(f"    [0]      page_type   = 0x{page_type:02x}")
    print(f"    [2:4]    free_space  = {free_space} (0x{free_space:04x})")
    print(f"    [4:8]    owner_tdef  = {owner}")
    print(f"    [12:14]  row_count   = {row_count}")

    return {
        'page_type': page_type,
        'free_space': free_space,
        'owner': owner,
        'row_count': row_count,
    }

def main():
    real_path = r"C:\Users\rinak\OneDrive\ドキュメント\Yayoi\弥生会計26データフォルダ\宮本　宏美　Cafe nest(令和06年度～令和08年度).KD26"
    conv_path = r"C:\Users\rinak\Documents\cafe_nest_v3.KB26"

    print("Loading REAL KD26...")
    real_mdb = load_real_kd(real_path)
    print(f"  Size: {len(real_mdb)} bytes, {len(real_mdb)//PAGE} pages")

    print("\nLoading CONVERTED KB26 (decompressing)...")
    conv_mdb = decompress_kb(conv_path)
    print(f"  Size: {len(conv_mdb)} bytes, {len(conv_mdb)//PAGE} pages")

    # Find TDEF pages
    print("\n--- Finding SystemInfo TDEF pages ---")
    real_tdef = find_sysinfo_tdef(real_mdb)
    if real_tdef is None:
        print("  ERROR: Could not find SystemInfo TDEF in REAL file!")
        return
    print(f"  REAL: TDEF page = {real_tdef}")

    conv_tdef = find_sysinfo_tdef(conv_mdb)
    if conv_tdef is None:
        print("  ERROR: Could not find SystemInfo TDEF in CONV file!")
        return
    print(f"  CONV: TDEF page = {conv_tdef}")

    # Analyze TDEF headers
    real_fields = analyze_tdef(real_mdb, real_tdef, "REAL")
    conv_fields = analyze_tdef(conv_mdb, conv_tdef, "CONV")

    # Compare
    print(f"\n{'='*60}")
    print(f"  TDEF COMPARISON")
    print(f"{'='*60}")
    for key in real_fields:
        rv = real_fields[key]
        cv = conv_fields[key]
        marker = " <-- MISMATCH!" if rv != cv else ""
        print(f"  {key:20s}: REAL={rv:10}  CONV={cv:10}{marker}")

    # Find data pages
    print(f"\n{'='*60}")
    print(f"  DATA PAGES")
    print(f"{'='*60}")

    real_data_pages = find_sysinfo_data_pages(real_mdb, real_tdef)
    print(f"\n  REAL: {len(real_data_pages)} data page(s): {real_data_pages}")
    real_dp_info = []
    for dp in real_data_pages:
        info = analyze_data_page(real_mdb, dp, "REAL")
        real_dp_info.append(info)

    conv_data_pages = find_sysinfo_data_pages(conv_mdb, conv_tdef)
    print(f"\n  CONV: {len(conv_data_pages)} data page(s): {conv_data_pages}")
    conv_dp_info = []
    for dp in conv_data_pages:
        info = analyze_data_page(conv_mdb, dp, "CONV")
        conv_dp_info.append(info)

    # Summary
    print(f"\n{'='*60}")
    print(f"  SUMMARY")
    print(f"{'='*60}")
    total_real_dp_rows = sum(d['row_count'] for d in real_dp_info)
    total_conv_dp_rows = sum(d['row_count'] for d in conv_dp_info)
    print(f"  REAL: TDEF total_rows={real_fields['total_rows']}, "
          f"sum of data page rows={total_real_dp_rows}")
    print(f"  CONV: TDEF total_rows={conv_fields['total_rows']}, "
          f"sum of data page rows={total_conv_dp_rows}")

    if conv_fields['total_rows'] != total_conv_dp_rows:
        print(f"\n  *** BUG DETECTED: CONV TDEF total_rows ({conv_fields['total_rows']}) "
              f"!= data page row sum ({total_conv_dp_rows}) ***")
        print(f"  *** The TDEF row count needs to be updated to {total_conv_dp_rows} ***")
    else:
        print(f"\n  TDEF row count matches data page rows - OK")

    if real_fields['total_rows'] != conv_fields['total_rows']:
        print(f"\n  *** REAL has {real_fields['total_rows']} rows, "
              f"CONV has {conv_fields['total_rows']} rows in TDEF ***")
        diff = conv_fields['total_rows'] - real_fields['total_rows']
        if diff < 0:
            diff2 = total_conv_dp_rows - real_fields['total_rows']
            print(f"  *** CONV TDEF is SHORT by {-diff} rows vs REAL ***")
            if total_conv_dp_rows > real_fields['total_rows']:
                print(f"  *** But CONV data pages have {diff2} MORE rows than REAL TDEF ***")
                print(f"  *** TDEF row count must be patched to {total_conv_dp_rows} ***")

if __name__ == '__main__':
    main()
