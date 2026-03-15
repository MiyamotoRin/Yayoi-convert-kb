#!/bin/bash
# EOCD とその後の独自メタデータをダンプ
TMP="/tmp/test_kb26.KB26"
FILESIZE=$(stat -c%s "$TMP")
echo "ファイルサイズ: $FILESIZE バイト"

# YZ\x05\x06 (EOCD署名) を末尾付近で探す
# 末尾から1000バイトをhex表示
DUMP_START=$(( FILESIZE - 1000 ))
if [ $DUMP_START -lt 0 ]; then DUMP_START=0; fi

echo "=== 末尾 1000 バイトの hex dump ==="
dd if="$TMP" bs=1 skip=$DUMP_START count=1000 2>/dev/null | hexdump -C

echo ""
echo "=== EOCD (YZ 05 06) 位置 ==="
# Pythonなしで: hexdump で YZ05 06 を探す
dd if="$TMP" bs=1 skip=$DUMP_START 2>/dev/null | \
  hexdump -v -e '1/1 "%02X\n"' | \
  awk 'BEGIN{i=0; start='"$DUMP_START"'}
       { b[i%4]=tolower($1);
         if (b[(i+1)%4]=="59" && b[(i+2)%4]=="5a" && b[(i+3)%4]=="05" && b[i%4]=="06")
             print "EOCD at offset", start+i-3;
         i++; }'
