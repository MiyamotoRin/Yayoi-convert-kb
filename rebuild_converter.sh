#!/bin/bash
CXX="/mingw64/bin/c++"
ZINC="/mingw64/include"
ZLIB="/mingw64/lib"
SRC_DIR="C:/Users/rinak/Workspace/Yayoi-convert-kb/src"
BUILD_DIR="C:/Users/rinak/Workspace/Yayoi-convert-kb/build"

echo "Building kb_converter.exe..."
"$CXX" -std=c++17 -O2 \
    -o "$BUILD_DIR/kb_converter_new.exe" \
    "$SRC_DIR/main.cpp" \
    "$SRC_DIR/kb_converter.cpp" \
    2>&1
echo "exit: $?"
ls -la "$BUILD_DIR/kb_converter_new.exe" 2>/dev/null && \
    cp "$BUILD_DIR/kb_converter_new.exe" "$BUILD_DIR/kb_converter.exe" && \
    echo "kb_converter.exe updated: $(ls -la $BUILD_DIR/kb_converter.exe)"
