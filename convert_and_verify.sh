#!/bin/bash
INPUT="/c/Users/rinak/Documents/cafe_nest.KB12"
OUTPUT="/c/Users/rinak/Documents/cafe_nest_converted.KB26"
EXE="/c/Users/rinak/Workspace/Yayoi-convert-kb/build/kb_converter.exe"

echo "=== 変換実行 ==="
"$EXE" "$INPUT" "$OUTPUT" 2>&1
echo ""

if [ ! -f "$OUTPUT" ]; then
    echo "出力ファイルが見つかりません"
    exit 1
fi

echo "=== 末尾メタデータ検証 (.KD12 が残っていないか確認) ==="
FILESIZE=$(stat -c%s "$OUTPUT")
echo "出力サイズ: $FILESIZE"
DUMP_START=$(( FILESIZE - 800 ))
dd if="$OUTPUT" bs=1 skip=$DUMP_START count=800 2>/dev/null | hexdump -v -C | grep -E "KD1|KD2"

echo ""
echo "=== 末尾300バイト hex dump ==="
DUMP_START2=$(( FILESIZE - 300 ))
dd if="$OUTPUT" bs=1 skip=$DUMP_START2 count=300 2>/dev/null | hexdump -C
