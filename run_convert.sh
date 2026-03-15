#!/bin/bash
# 元ファイルを探す (KB12 形式のオリジナル)
# Documents に KB26 拡張子のファイルがあるが、これが変換済みの可能性あり
# 元の KB12 ファイルがある場合はそこから変換
INPUT=""
for f in \
    "/c/Users/rinak/Documents/"*.KB12 \
    "/c/Users/rinak/Desktop/"*.KB12 \
    "/c/Users/rinak/Downloads/"*.KB12; do
    if [ -f "$f" ]; then
        INPUT="$f"
        break
    fi
done

if [ -z "$INPUT" ]; then
    echo "KB12 ファイルが見つかりません。既存 KB26 ファイルで検証します"
    # 末尾メタデータの .KD12 → .KD26 確認 (変換済みファイルのコピー上でテスト)
    SRC="/c/Users/rinak/Documents/宮本　宏美　Cafe nest(平成34年度～平成36年度)_繰越処理前.KB26"
    TMP_IN="/tmp/test_input.KB26"
    OUT="/tmp/test_output.KB26"
    cp "$SRC" "$TMP_IN"
    echo "入力: $TMP_IN"
    /c/Users/rinak/Workspace/Yayoi-convert-kb/build/kb_converter.exe "$TMP_IN" "$OUT" 2>&1
    echo "変換結果: $?"

    # 出力の末尾メタデータを確認
    echo ""
    echo "=== 出力ファイル末尾の .KD12 / .KD26 検索 ==="
    FILESIZE=$(stat -c%s "$OUT" 2>/dev/null)
    if [ -n "$FILESIZE" ]; then
        echo "出力サイズ: $FILESIZE"
        DUMP_START=$(( FILESIZE - 800 ))
        dd if="$OUT" bs=1 skip=$DUMP_START count=800 2>/dev/null | \
            hexdump -v -C | grep -E "KD1|KD2|\.KD"
    fi
else
    OUT="/tmp/converted_output.KB26"
    echo "変換中: $INPUT → $OUT"
    /c/Users/rinak/Workspace/Yayoi-convert-kb/build/kb_converter.exe "$INPUT" "$OUT"
fi
