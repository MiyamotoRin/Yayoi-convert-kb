"""
compare_mdb.py - Compare table/TDEF structure between two KB MDB files
"""
import sys, struct, zlib

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

def read_le16(b, off): return struct.unpack_from('<H', b, off)[0]
def read_le32(b, off): return struct.unpack_from('<I', b, off)[0]
def read_le64(b, off): return struct.unpack_from('<Q', b, off)[0]

def decompress_kb(path):
    data = open(path, 'rb').read()
    comp_size   = read_le64(data, 18)
    uncomp_size = read_le64(data, 26)
    name_len    = read_le16(data, 34)
    extra_len   = read_le16(data, 36)
    data_off = 38 + name_len + extra_len
    compressed = data[data_off:data_off+comp_size]
    mdb = zlib.decompress(compressed, -15)
    return bytearray(mdb)

PAGE = 4096

def extract_table_names(mdb):
    """
    Extract table names from TDEF pages.
    In Jet 4.0, TDEF page layout:
      +0:  page type (0x02)
      +1:  unknown
      +2:  free space (2 bytes)
      +4:  next TDEF page (4 bytes) - 0 if no overflow
      +8:  unknown
      +12: table type (0=user, 1=system, 2=linked, etc.)? Actually this is complex.

    The table name is stored differently. Let me search for UTF-16LE readable strings
    in the first 200 bytes of each TDEF page.
    """
    n = len(mdb) // PAGE
    tables = {}
    for pg in range(n):
        base = pg * PAGE
        if mdb[base] != 0x02: continue
        # Search for length-prefixed UTF-16LE strings in first 400 bytes
        for off in range(base + 8, base + 400, 2):
            try:
                nlen = read_le16(mdb, off)
                if 2 <= nlen <= 128 and (nlen % 2) == 0 and off + 2 + nlen < base + 400:
                    name_bytes = mdb[off+2 : off+2+nlen]
                    try:
                        name = name_bytes.decode('utf-16-le')
                        if all(c.isalnum() or c in '_-' for c in name) and len(name) >= 2:
                            if pg not in tables:
                                tables[pg] = name
                    except:
                        pass
            except:
                pass
    return tables

def get_mdb_header_info(mdb):
    """Get info from MDB page 0 (database header) and page 1"""
    # MDB page 0 is the header page
    # Page 0 byte 20 = Jet version (0x04 for Jet 4.0)
    # Encrypted database password at offset 0x42
    print(f"  Page 0 type: {mdb[0]:02X}")
    print(f"  Jet version: {mdb[20]:02X}")
    # Various header bytes
    print(f"  Page 0 bytes 16-32: {' '.join(f'{mdb[i]:02X}' for i in range(16, 32))}")
    print(f"  Page 0 bytes 32-48: {' '.join(f'{mdb[i]:02X}' for i in range(32, 48))}")

def compare_tdef_pages(mdb1, mdb2, label1, label2):
    """Find TDEF pages and extract table names from both MDBs"""
    n1 = len(mdb1) // PAGE
    n2 = len(mdb2) // PAGE

    def get_tdef_names(mdb):
        n = len(mdb) // PAGE
        tdef_pages = []
        for pg in range(n):
            base = pg * PAGE
            if mdb[base] == 0x02:
                tdef_pages.append(pg)
        # For each tdef page, scan for UTF-16LE strings
        names = {}
        for pg in tdef_pages:
            base = pg * PAGE
            # Scan for UTF-16LE strings in the TDEF
            found_names = []
            pos = base + 0x10
            while pos < base + min(1024, PAGE - 2):
                try:
                    nlen = read_le16(mdb, pos)
                    if 2 <= nlen <= 128 and (nlen % 2) == 0:
                        end = pos + 2 + nlen
                        if end <= base + PAGE:
                            name_bytes = mdb[pos+2:end]
                            try:
                                name = name_bytes.decode('utf-16-le')
                                if all(c.isalnum() or c in '_-\\' for c in name) and len(name) >= 2:
                                    found_names.append(name)
                            except:
                                pass
                except:
                    pass
                pos += 2
            if found_names:
                names[pg] = found_names
        return tdef_pages, names

    tdef1, names1 = get_tdef_names(mdb1)
    tdef2, names2 = get_tdef_names(mdb2)

    print(f"\n{label1}: {len(tdef1)} TDEF pages")
    print(f"{label2}: {len(tdef2)} TDEF pages")

    # Collect all table names
    all_names1 = set()
    all_names2 = set()
    for pg, ns in names1.items():
        all_names1.update(ns)
    for pg, ns in names2.items():
        all_names2.update(ns)

    only_in_2 = all_names2 - all_names1
    only_in_1 = all_names1 - all_names2

    print(f"\nTable names only in {label2} (missing from {label1}):")
    for n in sorted(only_in_2):
        print(f"  + {n}")

    print(f"\nTable names only in {label1} (extra vs {label2}):")
    for n in sorted(only_in_1):
        print(f"  - {n}")

    print(f"\nCommon table names ({len(all_names1 & all_names2)}):")
    for n in sorted(all_names1 & all_names2)[:30]:
        print(f"    {n}")

if __name__ == '__main__':
    conv_path = r'C:\Users\rinak\Documents\cafe_nest_v2.KB26'
    ref_path  = r'C:\Users\rinak\Documents\宮本　宏美　不動産(平成34年度～平成36年度)_繰越処理前.KB26'

    print("Loading converted KB26...")
    mdb_conv = decompress_kb(conv_path)
    print("Loading reference KB26...")
    mdb_ref  = decompress_kb(ref_path)

    print(f"\n=== Header comparison ===")
    print(f"CONV ({conv_path}):")
    get_mdb_header_info(mdb_conv)
    print(f"REF ({ref_path}):")
    get_mdb_header_info(mdb_ref)

    compare_tdef_pages(mdb_conv, mdb_ref, "CONV", "REF")
