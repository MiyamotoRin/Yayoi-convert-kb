/**
 * kb_format.h
 * 弥生会計バックアップファイル (.KB12 / .KB26) フォーマット定義
 *
 * 実際のファイル構造 (実ファイルのバイナリ解析により判明):
 *   弥生会計のバックアップファイルは変形ZIPアーカイブ形式を使用する。
 *   標準ZIPの署名 "PK" (0x50 0x4B) がすべて "YZ" (0x59 0x5A) に
 *   置き換えられている以外は標準ZIPと同一の構造を持つ。
 *
 *   先頭バイト列の実測値 (2ファイル共通):
 *     59 5A 03 04 14 00 00 00 08 00 ...
 *      Y  Z  [ZIPローカルファイルヘッダ後半]  [ver=20] [flags=0] [deflate]
 *   標準ZIPとの比較:
 *     50 4B 03 04 14 00 00 00 08 00 ...
 *      P  K
 *
 * バージョン情報の格納場所 (要調査):
 *   ZIPエントリのファイル名・ZIPコメントにバージョン文字列が含まれる可能性がある。
 *   圧縮データ内に格納されている場合は解凍・再圧縮が必要になる。
 */
#pragma once

#include <cstdint>
#include <array>

// ─────────────────────────────────────────────
// 弥生バックアップ固有のシグネチャ
// ─────────────────────────────────────────────

// ファイル先頭4バイト: "YZ" + ZIPローカルファイルヘッダサブタイプ
static constexpr std::array<uint8_t, 4> YAYOI_MAGIC = {
    0x59, 0x5A, 0x03, 0x04   // "YZ\x03\x04"
};

// YZ の各バイト (署名比較用)
static constexpr uint8_t YAYOI_SIG_0 = 0x59; // 'Y'
static constexpr uint8_t YAYOI_SIG_1 = 0x5A; // 'Z'

// 標準ZIPの "PK" (インメモリパッチ・逆変換用)
static constexpr uint8_t ZIP_PK_0 = 0x50; // 'P'
static constexpr uint8_t ZIP_PK_1 = 0x4B; // 'K'

// ─────────────────────────────────────────────
// ZIPシグネチャ後半2バイト (標準ZIP・YZ形式共通)
// ─────────────────────────────────────────────

static constexpr uint8_t ZIP_LOCAL_B2   = 0x03; // ローカルファイルヘッダ: \x03\x04
static constexpr uint8_t ZIP_LOCAL_B3   = 0x04;
static constexpr uint8_t ZIP_CDIR_B2    = 0x01; // セントラルディレクトリ: \x01\x02
static constexpr uint8_t ZIP_CDIR_B3    = 0x02;
static constexpr uint8_t ZIP_EOCD_B2    = 0x05; // エンドオブセントラルディレクトリ: \x05\x06
static constexpr uint8_t ZIP_EOCD_B3    = 0x06;

// ─────────────────────────────────────────────
// ZIPローカルファイルヘッダ フィールドオフセット (先頭4バイト込み)
// ─────────────────────────────────────────────
static constexpr size_t ZIP_LFH_FLAGS            = 6;   // 汎用ビットフラグ (2バイト)
static constexpr size_t ZIP_LFH_COMPRESSION      = 8;   // 圧縮方式 (2バイト)
static constexpr size_t ZIP_LFH_CRC32            = 14;  // CRC-32 (4バイト)
static constexpr size_t ZIP_LFH_COMPRESSED_SIZE  = 18;  // 圧縮後サイズ (4バイト)
static constexpr size_t ZIP_LFH_FILENAME_LEN     = 26;  // ファイル名長 (2バイト)
static constexpr size_t ZIP_LFH_EXTRA_LEN        = 28;  // 拡張フィールド長 (2バイト)
static constexpr size_t ZIP_LFH_FIXED_SIZE       = 30;  // ファイル名以前の固定部分

// ─────────────────────────────────────────────
// ZIPセントラルディレクトリエントリ フィールドオフセット (先頭4バイト込み)
// ─────────────────────────────────────────────
static constexpr size_t ZIP_CDE_COMPRESSED_SIZE  = 20;  // 圧縮後サイズ (4バイト)
static constexpr size_t ZIP_CDE_FILENAME_LEN     = 28;  // ファイル名長 (2バイト)
static constexpr size_t ZIP_CDE_EXTRA_LEN        = 30;  // 拡張フィールド長 (2バイト)
static constexpr size_t ZIP_CDE_COMMENT_LEN      = 32;  // コメント長 (2バイト)
static constexpr size_t ZIP_CDE_LOCAL_OFFSET     = 42;  // ローカルヘッダ位置 (4バイト)
static constexpr size_t ZIP_CDE_FIXED_SIZE       = 46;  // ファイル名以前の固定部分

// ─────────────────────────────────────────────
// ZIPエンドオブセントラルディレクトリ フィールドオフセット (先頭4バイト込み)
// ─────────────────────────────────────────────
static constexpr size_t ZIP_EOCD_CD_ENTRIES_TOTAL = 10; // 総エントリ数 (2バイト)
static constexpr size_t ZIP_EOCD_CD_SIZE          = 12; // セントラルディレクトリサイズ (4バイト)
static constexpr size_t ZIP_EOCD_CD_OFFSET        = 16; // セントラルディレクトリ開始位置 (4バイト)
static constexpr size_t ZIP_EOCD_COMMENT_LEN      = 20; // ZIPコメント長 (2バイト)
static constexpr size_t ZIP_EOCD_FIXED_SIZE       = 22; // コメント以前の固定部分

// ─────────────────────────────────────────────
// その他定数
// ─────────────────────────────────────────────

// ZIPコメントの最大長 (65535バイト)
static constexpr size_t ZIP_MAX_COMMENT_SIZE = 65535;

// ファイルの最小有効サイズ
static constexpr size_t YAYOI_MIN_FILE_SIZE = ZIP_LFH_FIXED_SIZE + ZIP_EOCD_FIXED_SIZE;
