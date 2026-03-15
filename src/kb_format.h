/**
 * kb_format.h
 * 弥生会計バックアップファイル (.KB12 / .KB26) フォーマット定義
 *
 * 弥生会計のバックアップファイルは以下の構造を持つ:
 *   - ファイルシグネチャ (マジックバイト)
 *   - バージョン情報ヘッダ
 *   - メタデータブロック (会社名, 年度, 作成日時など)
 *   - データブロック (ZLIB圧縮)
 *   - チェックサム (末尾4バイト CRC32)
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <array>

// ファイルシグネチャ (先頭4バイト)
// 弥生会計バックアップの共通マジックバイト
static constexpr std::array<uint8_t, 4> YAYOI_MAGIC = { 0x59, 0x41, 0x59, 0x4F }; // "YAYO"

// バージョン定数
static constexpr uint16_t KB_VERSION_12 = 0x000C; // バージョン12
static constexpr uint16_t KB_VERSION_19 = 0x0013; // バージョン19
static constexpr uint16_t KB_VERSION_24 = 0x0018; // バージョン24
static constexpr uint16_t KB_VERSION_26 = 0x001A; // バージョン26

// ヘッダオフセット定数
static constexpr size_t OFFSET_MAGIC         = 0;    // マジックバイト (4バイト)
static constexpr size_t OFFSET_FILE_VERSION  = 4;    // ファイルフォーマットバージョン (2バイト, LE)
static constexpr size_t OFFSET_APP_VERSION   = 6;    // アプリバージョン (2バイト, LE)
static constexpr size_t OFFSET_FLAGS         = 8;    // フラグ (4バイト)
static constexpr size_t OFFSET_HEADER_SIZE   = 12;   // ヘッダサイズ (4バイト, LE)
static constexpr size_t OFFSET_DATA_OFFSET   = 16;   // データブロック開始オフセット (4バイト, LE)
static constexpr size_t OFFSET_DATA_SIZE     = 20;   // 圧縮データサイズ (4バイト, LE)
static constexpr size_t OFFSET_ORIG_SIZE     = 24;   // 元データサイズ (4バイト, LE)
static constexpr size_t OFFSET_CHECKSUM      = 28;   // ヘッダCRC32 (4バイト, LE)
static constexpr size_t OFFSET_METADATA      = 32;   // メタデータ開始

// メタデータ内オフセット (OFFSET_METADATA からの相対位置)
static constexpr size_t META_COMPANY_NAME    = 0;    // 会社名 (64バイト, Shift-JIS)
static constexpr size_t META_FISCAL_YEAR     = 64;   // 会計年度 (2バイト, LE)
static constexpr size_t META_CREATED_DATE    = 66;   // 作成日 (8バイト: YYYYMMDD as chars)
static constexpr size_t META_COMPATIBLE_VER  = 74;   // 互換バージョン (2バイト, LE)
static constexpr size_t META_RESERVED        = 76;   // 予約領域 (20バイト)
static constexpr size_t META_BLOCK_SIZE      = 96;   // メタデータブロックサイズ

// ヘッダ最小サイズ (マジック + 固定フィールド + メタデータ)
static constexpr size_t HEADER_MIN_SIZE = OFFSET_METADATA + META_BLOCK_SIZE;

// バージョン12用の互換バージョン値
static constexpr uint16_t COMPAT_VERSION_12  = 0x000C;
// バージョン26用の互換バージョン値
static constexpr uint16_t COMPAT_VERSION_26  = 0x001A;

// フラグビット定義
static constexpr uint32_t FLAG_COMPRESSED    = 0x0001; // データが圧縮されている
static constexpr uint32_t FLAG_ENCRYPTED     = 0x0002; // データが暗号化されている (非サポート)
static constexpr uint32_t FLAG_CHECKSUM      = 0x0004; // チェックサムあり

#pragma pack(push, 1)

/**
 * バックアップファイルヘッダ構造体
 * ファイル先頭に配置される固定長ヘッダ
 */
struct KbFileHeader {
    uint8_t  magic[4];          // "YAYO"
    uint16_t file_version;      // ファイルフォーマットバージョン (LE)
    uint16_t app_version;       // アプリバージョン (LE)
    uint32_t flags;             // フラグフィールド
    uint32_t header_size;       // ヘッダ全体のサイズ (メタデータ含む)
    uint32_t data_offset;       // データブロック開始位置
    uint32_t data_size;         // 圧縮済みデータサイズ
    uint32_t orig_size;         // 圧縮前の元データサイズ
    uint32_t header_checksum;   // このヘッダのCRC32 (この4バイト自体は0として計算)
};

/**
 * メタデータ構造体
 * KbFileHeader の直後に配置
 */
struct KbMetadata {
    char     company_name[64];  // 会社名 (Shift-JIS, NUL終端)
    uint16_t fiscal_year;       // 会計年度
    char     created_date[8];   // 作成日 YYYYMMDD
    uint16_t compat_version;    // 下位互換バージョン
    uint8_t  reserved[20];      // 予約 (0埋め)
};

#pragma pack(pop)

static_assert(sizeof(KbFileHeader) == 32, "KbFileHeader size mismatch");
static_assert(sizeof(KbMetadata)   == 96, "KbMetadata size mismatch");
