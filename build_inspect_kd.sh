#!/bin/bash
CXX="/mingw64/bin/c++"
ZINC="/mingw64/include"
ZLIB="/mingw64/lib"
SRC="C:/Users/rinak/Workspace/Yayoi-convert-kb/src/inspect_kd.cpp"
OUT="C:/Users/rinak/Workspace/Yayoi-convert-kb/build/inspect_kd.exe"

echo "Building inspect_kd.exe..."
"$CXX" -std=c++17 -O2 -I"$ZINC" -o "$OUT" "$SRC" -L"$ZLIB" -lz
echo "exit: $?"
ls -la "$OUT" 2>/dev/null
