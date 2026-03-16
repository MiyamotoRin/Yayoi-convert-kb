import shutil, sys
src = r"C:\Users\rinak\OneDrive\ドキュメント\Yayoi\SystemLog\YKaikei26\YKaikei26.32.0.4.112Log_20260315.txt"
dst = "/tmp/ylog.txt"
shutil.copy2(src, dst)
with open(dst, "rb") as f:
    for line in f:
        try:
            decoded = line.decode("shift-jis")
            if "15:5" in decoded or "ERROR" in decoded.upper() or "doRestore" in decoded:
                print(decoded, end="")
        except:
            pass
