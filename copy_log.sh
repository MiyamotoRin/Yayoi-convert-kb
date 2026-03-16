#!/bin/bash
# .NETログ (15:50以降のエラー確認)
for f in "/c/Users/rinak/OneDrive/ドキュメント/Yayoi/SystemLog/YKaikei26/"*.log; do
    result=$(iconv -f SHIFT-JIS -t UTF-8 "$f" 2>/dev/null | grep -E "ERROR|WARN|Restore|restore|バックアップ|復元|失敗|error" | head -10)
    if [ -n "$result" ]; then
        echo "=== $f ==="
        echo "$result"
    fi
done

# ShohizeiExportログも確認
for f in "/c/Users/rinak/OneDrive/ドキュメント/Yayoi/SystemLog/Yayoi.Kaikei.ShohizeiExport/"*155*.log; do
    echo "=== $f ==="
    cat "$f" 2>/dev/null | grep -iE "error|restore|backup|fail" | head -10
done
