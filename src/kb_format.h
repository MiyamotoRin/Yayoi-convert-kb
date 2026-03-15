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
//
// 注意: 弥生形式は標準ZIPと異なり compressed_size・uncompressed_size を
//       int64 (8バイト) で格納する。そのため後続フィールドのオフセットが
//       標準ZIP (4バイト×2=8バイト) より +8 ずれる。
//
//   標準ZIP LFH 固定部 (30バイト):
//     +18: compressed_size   (4バイト)
//     +22: uncompressed_size (4バイト)
//     +26: name_len          (2バイト)
//     +28: extra_len         (2バイト)
//
//   弥生 LFH 固定部 (38バイト):
//     +18: compressed_size   (8バイト / int64)
//     +26: uncompressed_size (8バイト / int64)
//     +34: name_len          (2バイト)
//     +36: extra_len         (2バイト)
// ─────────────────────────────────────────────
static constexpr size_t ZIP_LFH_FLAGS            = 6;   // 汎用ビットフラグ (2バイト)
static constexpr size_t ZIP_LFH_COMPRESSION      = 8;   // 圧縮方式 (2バイト)
static constexpr size_t ZIP_LFH_CRC32            = 14;  // CRC-32 (4バイト)
static constexpr size_t ZIP_LFH_COMPRESSED_SIZE  = 18;  // 圧縮後サイズ (8バイト / int64)
static constexpr size_t ZIP_LFH_UNCOMPRESSED_SIZE= 26;  // 展開後サイズ (8バイト / int64)
static constexpr size_t ZIP_LFH_FILENAME_LEN     = 34;  // ファイル名長 (2バイト)
static constexpr size_t ZIP_LFH_EXTRA_LEN        = 36;  // 拡張フィールド長 (2バイト)
static constexpr size_t ZIP_LFH_FIXED_SIZE       = 38;  // ファイル名以前の固定部分

// ─────────────────────────────────────────────
// ZIPセントラルディレクトリエントリ フィールドオフセット (先頭4バイト込み)
//
// 注意: 弥生形式は compressed_size・uncompressed_size・local_off を
//       int64 (8バイト) で格納する。後続フィールドのオフセットが
//       標準ZIP (4+4+4=12バイト) より +12 ずれる。
//
//   標準ZIP CDE 固定部 (46バイト):
//     +20: compressed_size   (4バイト)
//     +24: uncompressed_size (4バイト)
//     +28: name_len          (2バイト)
//     +30: extra_len         (2バイト)
//     +32: comment_len       (2バイト)
//     +42: local_off         (4バイト)
//
//   弥生 CDE 固定部 (58バイト):
//     +20: compressed_size   (8バイト / int64)
//     +28: uncompressed_size (8バイト / int64)
//     +36: name_len          (2バイト)
//     +38: extra_len         (2バイト)
//     +40: comment_len       (2バイト)
//     +50: local_off         (8バイト / int64, uint32 読み取りで動作)
// ─────────────────────────────────────────────
static constexpr size_t ZIP_CDE_COMPRESSED_SIZE  = 20;  // 圧縮後サイズ (8バイト / int64)
static constexpr size_t ZIP_CDE_UNCOMPRESSED_SIZE= 28;  // 展開後サイズ (8バイト / int64)
static constexpr size_t ZIP_CDE_FILENAME_LEN     = 36;  // ファイル名長 (2バイト)
static constexpr size_t ZIP_CDE_EXTRA_LEN        = 38;  // 拡張フィールド長 (2バイト)
static constexpr size_t ZIP_CDE_COMMENT_LEN      = 40;  // コメント長 (2バイト)
static constexpr size_t ZIP_CDE_LOCAL_OFFSET     = 50;  // ローカルヘッダ位置 (8バイト / int64)
static constexpr size_t ZIP_CDE_FIXED_SIZE       = 58;  // ファイル名以前の固定部分

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
