#!/bin/bash
# KB26 ファイルから圧縮データを展開して内部バージョン情報を確認する
KB26="/c/Users/rinak/Documents/宮本　宏美　Cafe nest(平成34年度～平成36年度)_繰越処理前.KB26"

if [ ! -f "$KB26" ]; then
    echo "KB26 ファイルが見つかりません: $KB26"
    exit 1
fi

TMP=$(mktemp /tmp/kb_XXXXXX.zip)
cp "$KB26" "$TMP"

# YZ→PK シグネチャ変換 (Python で全置換)
python3 - "$TMP" <<'EOF'
import sys
data = bytearray(open(sys.argv[1],'rb').read())
for i in range(len(data)-3):
    if data[i]==0x59 and data[i+1]==0x5A:
        b2, b3 = data[i+2], data[i+3]
        if (b2==0x03 and b3==0x04) or (b2==0x01 and b3==0x02) or (b2==0x05 and b3==0x06):
            data[i]=0x50; data[i+1]=0x4B
open(sys.argv[1],'wb').write(data)
print("シグネチャ変換完了")
EOF

# ただし弥生は int64 サイズフィールドで標準 ZIP と互換性がないため、
# Python の zipfile では直接読めない可能性がある。
# 代わりに: offset 85 (LFH 38 + name 47) から deflate データを抽出して展開

OUTDIR=$(mktemp -d /tmp/kd_XXXXXX)
python3 - "$KB26" "$OUTDIR" <<'EOF'
import sys, zlib, struct, os

kb_path = sys.argv[1]
out_dir = sys.argv[2]

with open(kb_path, 'rb') as f:
    data = f.read()

# LFH 解析: 固定部38バイト + name_len(at +34) バイト + extra_len(at+36) バイト
name_len  = struct.unpack_from('<H', data, 34)[0]
extra_len = struct.unpack_from('<H', data, 36)[0]
comp_size = struct.unpack_from('<Q', data, 18)[0]  # int64
data_off  = 38 + name_len + extra_len

filename = data[38:38+name_len].decode('shift-jis', errors='replace')
print(f"エントリ名: {filename}")
print(f"data_offset={data_off}, comp_size={comp_size}")

compressed = data[data_off:data_off+comp_size]
try:
    decompressed = zlib.decompress(compressed, -15)  # raw deflate
    out_path = os.path.join(out_dir, filename.replace('/', '_'))
    with open(out_path, 'wb') as f:
        f.write(decompressed)
    print(f"展開完了: {len(decompressed)} バイト → {out_path}")
except Exception as e:
    print(f"展開エラー: {e}")
EOF

# KD ファイルを確認
echo "=== 展開されたファイル ==="
ls -la "$OUTDIR/"

KD_FILE=$(ls "$OUTDIR"/*.KD* 2>/dev/null | head -1)
if [ -n "$KD_FILE" ]; then
    echo "=== 先頭 32 バイト (フォーマット確認) ==="
    hexdump -C "$KD_FILE" | head -2
    echo "=== バージョン文字列検索 ==="
    strings "$KD_FILE" | grep -iE "^[Vv]ersion|KB[0-9]{2}|KD[0-9]{2}|ver=[0-9]|DataVersion|FileVersion|AppVersion|DataFormatVersion" | head -20
    echo "=== 'KB12' / 'KB26' 文字列 ==="
    strings "$KD_FILE" | grep -E "KB1[0-9]|KB2[0-9]" | head -20
fi

rm -rf "$TMP" "$OUTDIR"
