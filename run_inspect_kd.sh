#!/bin/bash
TMP="/tmp/test_kb26.KB26"
EXE="/c/Users/rinak/Workspace/Yayoi-convert-kb/build/inspect_kd.exe"

echo "=== 内部エントリ名 (bytes 38..38+name_len) を確認 ==="
python3 - "$TMP" <<'PYEOF'
import sys, struct

data = open(sys.argv[1], 'rb').read()
name_len  = struct.unpack_from('<H', data, 34)[0]
extra_len = struct.unpack_from('<H', data, 36)[0]
comp_size = struct.unpack_from('<Q', data, 18)[0]
uncomp_size = struct.unpack_from('<Q', data, 26)[0]
data_off  = 38 + name_len + extra_len
filename_bytes = data[38:38+name_len]
try:
    filename = filename_bytes.decode('shift-jis')
except:
    filename = filename_bytes.decode('utf-8', errors='replace')
print(f"name_len={name_len}, extra_len={extra_len}, comp_size={comp_size}, uncomp_size={uncomp_size}")
print(f"data_off={data_off}")
print(f"filename hex: {filename_bytes.hex()}")
print(f"filename: {filename}")
PYEOF

echo ""
echo "=== inspect_kd.exe ==="
"$EXE" "$TMP" 2>&1
