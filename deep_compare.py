import sys
import struct
import zlib

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# --- Paths ---
REAL_KD26 = r"C:\Users\rinak\OneDrive\ドキュメント\Yayoi\弥生会計26データフォルダ\宮本　宏美　Cafe nest(令和06年度～令和08年度).KD26"
CONVERTED_KB26 = r"C:\Users\rinak\Documents\cafe_nest_v3.KB26"

PAGE_SIZE = 4096

def load_real():
    with open(REAL_KD26, 'rb') as f:
        return f.read()

def load_converted():
    with open(CONVERTED_KB26, 'rb') as f:
        data = f.read()
    # Parse LFH fields
    comp_size = struct.unpack_from('<Q', data, 18)[0]
    name_len = struct.unpack_from('<H', data, 34)[0]
    extra_len = struct.unpack_from('<H', data, 36)[0]
    offset = 38 + name_len + extra_len
    compressed = data[offset:offset + comp_size]
    return zlib.decompress(compressed, -15)

print("Loading files...")
real = load_real()
conv = load_converted()
print(f"  REAL size: {len(real)} bytes ({len(real)//PAGE_SIZE} pages)")
print(f"  CONV size: {len(conv)} bytes ({len(conv)//PAGE_SIZE} pages)")

real_pages = len(real) // PAGE_SIZE
conv_pages = len(conv) // PAGE_SIZE

# ===== A) Page type distribution =====
print("\n" + "="*70)
print("A) PAGE TYPE DISTRIBUTION")
print("="*70)

PAGE_TYPES = {0: "Free", 1: "Data", 2: "TDEF", 3: "IntermIndex", 4: "LVAL", 5: "LeafIndex"}

def count_page_types(data, num_pages):
    counts = {}
    for i in range(num_pages):
        if i == 0:
            continue  # skip header page
        page_off = i * PAGE_SIZE
        ptype = data[page_off]
        counts[ptype] = counts.get(ptype, 0) + 1
    return counts

real_types = count_page_types(real, real_pages)
conv_types = count_page_types(conv, conv_pages)

all_types = sorted(set(list(real_types.keys()) + list(conv_types.keys())))
print(f"{'Type':>6} {'Name':>15} {'REAL':>8} {'CONV':>8} {'Diff':>8}")
print("-" * 50)
for t in all_types:
    name = PAGE_TYPES.get(t, f"Unknown(0x{t:02X})")
    r = real_types.get(t, 0)
    c = conv_types.get(t, 0)
    diff = c - r
    marker = " <-- DIFF" if diff != 0 else ""
    print(f"  0x{t:02X} {name:>15} {r:>8} {c:>8} {diff:>+8}{marker}")

# ===== B) Page 0 header comparison =====
print("\n" + "="*70)
print("B) PAGE 0 (DATABASE HEADER) COMPARISON - first 256 bytes")
print("="*70)

real_hdr = real[:256]
conv_hdr = conv[:256]

diffs = []
for i in range(256):
    if real_hdr[i] != conv_hdr[i]:
        diffs.append(i)

if not diffs:
    print("  Headers are IDENTICAL in first 256 bytes.")
else:
    print(f"  Found {len(diffs)} differing bytes:")
    for off in diffs:
        print(f"    Offset 0x{off:04X} ({off:3d}): REAL=0x{real_hdr[off]:02X}  CONV=0x{conv_hdr[off]:02X}")

# Key fields
print("\n  Key header fields:")
key_offsets = [
    (0x04, 4, "Jet version"),
    (0x2C, 4, "Field at 0x2C"),
    (0x3C, 4, "Field at 0x3C (page count?)"),
    (0x40, 4, "Field at 0x40"),
    (0x58, 4, "Field at 0x58"),
    (0x5C, 4, "Field at 0x5C"),
    (0x6C, 4, "Field at 0x6C"),
    (0x08, 16, "Jet4 key/password region (0x08-0x17)"),
]
for off, size, desc in key_offsets:
    rv = real[off:off+size]
    cv = conv[off:off+size]
    match = "MATCH" if rv == cv else "DIFFER"
    if size <= 4:
        rv_int = int.from_bytes(rv, 'little')
        cv_int = int.from_bytes(cv, 'little')
        print(f"    {desc} (0x{off:02X}): REAL={rv_int} (0x{rv_int:08X})  CONV={cv_int} (0x{cv_int:08X})  [{match}]")
    else:
        print(f"    {desc} (0x{off:02X}): REAL={rv.hex()}  CONV={cv.hex()}  [{match}]")

# ===== C) MSysObjects TDEF (page 2) comparison =====
print("\n" + "="*70)
print("C) MSysObjects TDEF (page 2) COMPARISON")
print("="*70)

real_p2 = real[2*PAGE_SIZE : 2*PAGE_SIZE + 128]
conv_p2 = conv[2*PAGE_SIZE : 2*PAGE_SIZE + 128]

print("  First 128 bytes of page 2:")
diffs_p2 = []
for i in range(128):
    if real_p2[i] != conv_p2[i]:
        diffs_p2.append(i)

if not diffs_p2:
    print("    IDENTICAL")
else:
    print(f"    {len(diffs_p2)} differing bytes:")
    for off in diffs_p2:
        print(f"      Offset +0x{off:02X} ({off:3d}): REAL=0x{real_p2[off]:02X}  CONV=0x{conv_p2[off]:02X}")

# TDEF page header: byte 0 = page type, bytes 1-2 = unknown, bytes 4-7 = next page of same type
# For Jet4 TDEF: offset 0x24 = next TDEF page? offset 0x08 = table type?
print("\n  TDEF page 2 structure:")
for name, off, size in [("PageType", 0, 1), ("UnkFlag", 1, 1), ("FreeSpace", 2, 2),
                          ("TdefOwner", 4, 4), ("NextPage", 8, 4)]:
    rv = int.from_bytes(real_p2[off:off+size], 'little')
    cv = int.from_bytes(conv_p2[off:off+size], 'little')
    match = "MATCH" if rv == cv else "DIFFER"
    print(f"    {name} (+0x{off:02X}): REAL={rv}  CONV={cv}  [{match}]")

# Find data pages owned by TDEF page 2
def find_data_pages_for_tdef(data, num_pages, tdef_page):
    """In Jet4, data pages have owner TDEF at offset 4 (4 bytes LE)."""
    pages = []
    for i in range(num_pages):
        if i == 0:
            continue
        page_off = i * PAGE_SIZE
        ptype = data[page_off]
        if ptype == 0x01:  # Data page
            owner = struct.unpack_from('<I', data, page_off + 4)[0]
            if owner == tdef_page:
                pages.append(i)
    return pages

real_dp2 = find_data_pages_for_tdef(real, real_pages, 2)
conv_dp2 = find_data_pages_for_tdef(conv, conv_pages, 2)
print(f"\n  Data pages owned by TDEF page 2 (MSysObjects):")
print(f"    REAL: {len(real_dp2)} pages: {real_dp2}")
print(f"    CONV: {len(conv_dp2)} pages: {conv_dp2}")

# ===== D) System tables (MSys*) =====
print("\n" + "="*70)
print("D) SYSTEM TABLES (MSys*) IN TDEF PAGES")
print("="*70)

msys_pattern = b'\x4D\x00\x53\x00\x79\x00\x73\x00'  # "MSys" in UTF-16LE

def find_msys_in_tdefs(data, num_pages, label):
    results = []
    for i in range(num_pages):
        if i == 0:
            continue
        page_off = i * PAGE_SIZE
        ptype = data[page_off]
        if ptype != 0x02:
            continue
        page_data = data[page_off:page_off + PAGE_SIZE]
        pos = 0
        while True:
            idx = page_data.find(msys_pattern, pos)
            if idx == -1:
                break
            # Try to extract the full name (read until null or non-printable)
            name_bytes = bytearray()
            j = idx
            while j + 1 < PAGE_SIZE:
                ch = struct.unpack_from('<H', page_data, j)[0]
                if ch == 0 or ch > 0x7F:
                    if len(name_bytes) > 0:
                        break
                    else:
                        j += 2
                        continue
                name_bytes.append(ch & 0xFF)
                j += 2
            name = name_bytes.decode('ascii', errors='replace')
            if name.startswith('MSys'):
                results.append((i, idx, name))
            pos = idx + 2
    return results

real_msys = find_msys_in_tdefs(real, real_pages, "REAL")
conv_msys = find_msys_in_tdefs(conv, conv_pages, "CONV")

print("  REAL MSys tables found in TDEFs:")
for pg, off, name in real_msys:
    print(f"    Page {pg}, offset +0x{off:03X}: {name}")

print("  CONV MSys tables found in TDEFs:")
for pg, off, name in conv_msys:
    print(f"    Page {pg}, offset +0x{off:03X}: {name}")

# Compare
real_names = set(n for _, _, n in real_msys)
conv_names = set(n for _, _, n in conv_msys)
only_real = real_names - conv_names
only_conv = conv_names - real_names
if only_real:
    print(f"  ONLY in REAL: {only_real}")
if only_conv:
    print(f"  ONLY in CONV: {only_conv}")
if not only_real and not only_conv:
    print(f"  Same set of MSys tables: {sorted(real_names)}")

# ===== E) Version/Config tables =====
print("\n" + "="*70)
print("E) TABLES WITH 'Version' OR 'Config' IN NAME")
print("="*70)

version_pattern = b'\x56\x00\x65\x00\x72\x00\x73\x00\x69\x00\x6F\x00\x6E\x00'
config_pattern = b'\x43\x00\x6F\x00\x6E\x00\x66\x00\x69\x00\x67\x00'

def find_pattern_in_tdefs(data, num_pages, pattern, pattern_name):
    results = []
    for i in range(num_pages):
        if i == 0:
            continue
        page_off = i * PAGE_SIZE
        ptype = data[page_off]
        if ptype != 0x02:
            continue
        page_data = data[page_off:page_off + PAGE_SIZE]
        pos = 0
        while True:
            idx = page_data.find(pattern, pos)
            if idx == -1:
                break
            # Try to read surrounding UTF-16LE string
            # Go backward to find start
            start = idx
            while start >= 2:
                ch = struct.unpack_from('<H', page_data, start - 2)[0]
                if ch == 0 or ch > 0x7E or ch < 0x20:
                    break
                start -= 2
            # Go forward to find end
            end = idx
            while end + 1 < PAGE_SIZE:
                ch = struct.unpack_from('<H', page_data, end)[0]
                if ch == 0 or ch > 0x7E or ch < 0x20:
                    break
                end += 2
            name = page_data[start:end].decode('utf-16-le', errors='replace')
            results.append((i, idx, name))
            pos = idx + 2
    return results

for pattern, pname in [(version_pattern, "Version"), (config_pattern, "Config")]:
    print(f"\n  Searching for '{pname}':")
    real_res = find_pattern_in_tdefs(real, real_pages, pattern, pname)
    conv_res = find_pattern_in_tdefs(conv, conv_pages, pattern, pname)
    print(f"    REAL:")
    for pg, off, name in real_res:
        print(f"      Page {pg}, offset +0x{off:03X}: '{name}'")
    if not real_res:
        print(f"      (none)")
    print(f"    CONV:")
    for pg, off, name in conv_res:
        print(f"      Page {pg}, offset +0x{off:03X}: '{name}'")
    if not conv_res:
        print(f"      (none)")

# ===== BONUS: Compare ALL TDEF pages =====
print("\n" + "="*70)
print("BONUS: TDEF PAGE-BY-PAGE COMPARISON")
print("="*70)

def get_tdef_pages(data, num_pages):
    pages = []
    for i in range(num_pages):
        if i == 0:
            continue
        if data[i * PAGE_SIZE] == 0x02:
            pages.append(i)
    return pages

real_tdefs = get_tdef_pages(real, real_pages)
conv_tdefs = get_tdef_pages(conv, conv_pages)

print(f"  REAL TDEF pages: {len(real_tdefs)} -> {real_tdefs}")
print(f"  CONV TDEF pages: {len(conv_tdefs)} -> {conv_tdefs}")

# For shared TDEF pages, compare first 64 bytes
shared = sorted(set(real_tdefs) & set(conv_tdefs))
print(f"\n  Shared TDEF pages: {shared}")
for pg in shared:
    off = pg * PAGE_SIZE
    r = real[off:off+64]
    c = conv[off:off+64]
    if r == c:
        status = "IDENTICAL (first 64 bytes)"
    else:
        diff_positions = [i for i in range(64) if r[i] != c[i]]
        status = f"DIFFER at offsets: {[f'+0x{x:02X}' for x in diff_positions]}"
    # Also check full page
    r_full = real[off:off+PAGE_SIZE]
    c_full = conv[off:off+PAGE_SIZE]
    full_diffs = sum(1 for i in range(PAGE_SIZE) if r_full[i] != c_full[i])
    print(f"  Page {pg}: {status} | Full page: {full_diffs} differing bytes")

# Only in one
only_real_tdef = sorted(set(real_tdefs) - set(conv_tdefs))
only_conv_tdef = sorted(set(conv_tdefs) - set(real_tdefs))
if only_real_tdef:
    print(f"\n  TDEF pages ONLY in REAL: {only_real_tdef}")
if only_conv_tdef:
    print(f"\n  TDEF pages ONLY in CONV: {only_conv_tdef}")

# ===== BONUS 2: Data page owner distribution =====
print("\n" + "="*70)
print("BONUS: DATA PAGE OWNER DISTRIBUTION (which TDEF owns how many data pages)")
print("="*70)

def data_page_owners(data, num_pages):
    owners = {}
    for i in range(num_pages):
        if i == 0:
            continue
        page_off = i * PAGE_SIZE
        if data[page_off] == 0x01:
            owner = struct.unpack_from('<I', data, page_off + 4)[0]
            owners[owner] = owners.get(owner, 0) + 1
    return owners

real_owners = data_page_owners(real, real_pages)
conv_owners = data_page_owners(conv, conv_pages)

all_owners = sorted(set(list(real_owners.keys()) + list(conv_owners.keys())))
print(f"{'Owner':>8} {'REAL':>8} {'CONV':>8} {'Diff':>8}")
print("-" * 40)
for o in all_owners:
    r = real_owners.get(o, 0)
    c = conv_owners.get(o, 0)
    d = c - r
    marker = " <--" if d != 0 else ""
    print(f"  {o:>6} {r:>8} {c:>8} {d:>+8}{marker}")

# ===== BONUS 3: Check for different content in data pages with same owner =====
print("\n" + "="*70)
print("BONUS: COMPARE CONTENT OF FIRST FEW DATA PAGES FOR KEY TABLES")
print("="*70)

# Compare data pages for MSysObjects (TDEF=2)
for tdef_pg in [2]:
    real_dps = find_data_pages_for_tdef(real, real_pages, tdef_pg)
    conv_dps = find_data_pages_for_tdef(conv, conv_pages, tdef_pg)
    min_len = min(len(real_dps), len(conv_dps))
    print(f"\n  TDEF {tdef_pg} data pages: REAL has {len(real_dps)}, CONV has {len(conv_dps)}")
    for idx in range(min_len):
        rp = real_dps[idx]
        cp = conv_dps[idx]
        r_data = real[rp*PAGE_SIZE:(rp+1)*PAGE_SIZE]
        c_data = conv[cp*PAGE_SIZE:(cp+1)*PAGE_SIZE]
        ndiffs = sum(1 for i in range(PAGE_SIZE) if r_data[i] != c_data[i])
        # Check record count (offset 1, 2 bytes LE in Jet4 data page)
        r_recs = struct.unpack_from('<H', r_data, 1)[0]
        c_recs = struct.unpack_from('<H', c_data, 1)[0]
        print(f"    Data page pair [{idx}]: REAL pg {rp} ({r_recs} recs) vs CONV pg {cp} ({c_recs} recs) -> {ndiffs} byte diffs")

print("\nDone.")
