"""
diff_headers.py - Compare Jet MDB headers and key pages between two KB files
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
    return bytearray(zlib.decompress(compressed, -15))

PAGE = 4096

def hex_dump(data, off, length, indent="  "):
    for i in range(0, length, 16):
        chunk = data[off+i:off+i+min(16,length-i)]
        hex_part = ' '.join(f'{b:02X}' for b in chunk)
        asc_part = ''.join(chr(b) if 0x20<=b<0x7F else '.' for b in chunk)
        print(f"{indent}{off+i:08X}: {hex_part:<48} |{asc_part}|")

def compare_pages(mdb1, mdb2, label1, label2, pages_to_check):
    for pg in pages_to_check:
        base = pg * PAGE
        b1 = mdb1[base:base+PAGE]
        b2 = mdb2[base:base+PAGE]
        if b1 == b2:
            print(f"Page {pg}: IDENTICAL")
        else:
            # Find differing bytes
            diffs = [(i, b1[i], b2[i]) for i in range(min(len(b1), len(b2))) if b1[i] != b2[i]]
            print(f"Page {pg}: {len(diffs)} bytes differ")
            # Show first 64 bytes of each
            print(f"  {label1} page {pg} (first 64 bytes):")
            hex_dump(mdb1, base, 64)
            print(f"  {label2} page {pg} (first 64 bytes):")
            hex_dump(mdb2, base, 64)
            if len(diffs) <= 20:
                print(f"  Diff offsets (in-page): {[d[0] for d in diffs]}")

if __name__ == '__main__':
    conv_path = r'C:\Users\rinak\Documents\cafe_nest_v2.KB26'
    ref_path  = r'C:\Users\rinak\Documents\宮本　宏美　不動産(平成34年度～平成36年度)_繰越処理前.KB26'

    print(f"CONV: {conv_path}")
    print(f"REF:  {ref_path}")

    mdb_conv = decompress_kb(conv_path)
    mdb_ref  = decompress_kb(ref_path)

    print(f"\nCONV size: {len(mdb_conv)} bytes ({len(mdb_conv)//PAGE} pages)")
    print(f"REF  size: {len(mdb_ref)} bytes ({len(mdb_ref)//PAGE} pages)")

    # Compare Jet header page (page 0) - critical
    print("\n=== Page 0 (Jet DB Header) ===")
    print("CONV page 0:")
    hex_dump(mdb_conv, 0, 128)
    print("\nREF page 0:")
    hex_dump(mdb_ref, 0, 128)

    # Key Jet 4.0 header fields
    # Jet header page: first 4 bytes = 00 01 00 00 (Jet 4 magic)
    # Bytes 4-23: "Standard Jet DB\0\0\0\0"
    # Byte 20: Jet version (4=Jet4, 3=Jet3)
    # Actually in Jet 4.0, the header is XORed with a key... let me check

    # Page 1 is the system catalog (MSysObjects)
    print("\n=== Page 1 (MSysObjects first data page?) ===")
    compare_pages(mdb_conv, mdb_ref, "CONV", "REF", [1])

    # Dump page 1 of both
    print("\nCONV page 1 (first 64 bytes):")
    hex_dump(mdb_conv, PAGE, 64)
    print("\nREF page 1 (first 64 bytes):")
    hex_dump(mdb_ref, PAGE, 64)

    # Check the Jet header at page 0 unencrypted offset 0x14 (decimal 20)
    # In Jet 4, the DB version is stored here
    print(f"\n=== Jet DB Header Key Fields ===")
    print(f"CONV byte 20 (Jet version): 0x{mdb_conv[20]:02X}")
    print(f"REF  byte 20 (Jet version): 0x{mdb_ref[20]:02X}")

    # Check page count in header
    # In Jet 4 header (page 0), the next-page free list pointer is at offset 0?
    # Actually the free-page list root is at offset 4 in Jet 4... let me not guess
    # Instead, let's look at exact byte-for-byte differences in first page
    p0_diffs = [(i, mdb_conv[i], mdb_ref[i]) for i in range(min(PAGE, len(mdb_conv), len(mdb_ref))) if mdb_conv[i] != mdb_ref[i]]
    print(f"\nPage 0 differences: {len(p0_diffs)} bytes")
    for i, a, b in p0_diffs[:30]:
        print(f"  offset {i} (0x{i:03X}): CONV=0x{a:02X} REF=0x{b:02X}")
