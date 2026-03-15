/**
 * kb_converter.cpp
 * 弥生会計バックアップファイル KB12→KB26 コンバータ 実装
 *
 * 変換フロー:
 *   1. 入力ファイル (.KB12) をバイト列として読み込む
 *   2. マジックバイト・バージョンを検証
 *   3. ヘッダチェックサムを確認
 *   4. バージョンフィールドを 12→26 に更新
 *      - KbFileHeader::file_version
 *      - KbFileHeader::app_version
 *      - KbMetadata::compat_version
 *   5. ヘッダチェックサムを再計算
 *   6. 出力ファイル (.KB26) に書き出す
 *
 * 注意:
 *   データブロック (圧縮済みデータ) は変換前後で内容を変更しない。
 *   これはバックアップデータ自体の互換性はYayoi会計アプリ側が担保するためであり、
 *   本ツールはファイル識別子 (バージョンヘッダ) のみを更新する。
 */

#include "kb_converter.h"
#include "kb_format.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <stdexcept>

// ─────────────────────────────────────────────
// CRC32 実装 (外部ライブラリ不要)
// ─────────────────────────────────────────────

static uint32_t crc32_table[256];
static bool     crc32_table_initialized = false;

static void init_crc32_table() {
    if (crc32_table_initialized) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_initialized = true;
}

static uint32_t calc_crc32(const uint8_t* data, size_t length) {
    init_crc32_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ─────────────────────────────────────────────
// ユーティリティ
// ─────────────────────────────────────────────

/** リトルエンディアン uint16_t 読み取り */
static inline uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

/** リトルエンディアン uint32_t 読み取り */
static inline uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

/** リトルエンディアン uint16_t 書き込み */
static inline void write_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

/** リトルエンディアン uint32_t 書き込み */
static inline void write_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

/** ファイル全体をバイト列で読み込む */
static bool read_file(const std::string& path, std::vector<uint8_t>& buf) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    std::streamsize size = ifs.tellg();
    if (size <= 0) return false;
    ifs.seekg(0, std::ios::beg);
    buf.resize(static_cast<size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), size)) return false;
    return true;
}

/** バイト列をファイルに書き出す */
static bool write_file(const std::string& path, const std::vector<uint8_t>& buf) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    if (!ofs.write(reinterpret_cast<const char*>(buf.data()),
                   static_cast<std::streamsize>(buf.size()))) {
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────
// ヘッダ検証
// ─────────────────────────────────────────────

/**
 * バッファがYayoiバックアップのマジックバイトを持つか確認する。
 * 先頭4バイトが "YAYO" であれば true を返す。
 */
static bool check_magic(const std::vector<uint8_t>& buf) {
    if (buf.size() < 4) return false;
    return (buf[0] == YAYOI_MAGIC[0] &&
            buf[1] == YAYOI_MAGIC[1] &&
            buf[2] == YAYOI_MAGIC[2] &&
            buf[3] == YAYOI_MAGIC[3]);
}

/**
 * ヘッダチェックサムを検証する。
 * header_checksum フィールド (offset 28..31) を 0 として CRC32 を計算し、
 * 格納値と比較する。
 *
 * @param buf         ファイルバッファ
 * @param header_size ヘッダサイズ (メタデータを含む)
 * @return true: チェックサム一致
 */
static bool verify_header_checksum(const std::vector<uint8_t>& buf, uint32_t header_size) {
    if (buf.size() < header_size) return false;

    // チェックサムフィールドを一時的に 0 にしてコピーを作成
    std::vector<uint8_t> tmp(buf.begin(), buf.begin() + header_size);
    write_le32(tmp.data() + OFFSET_CHECKSUM, 0);

    uint32_t calc = calc_crc32(tmp.data(), header_size);
    uint32_t stored = read_le32(buf.data() + OFFSET_CHECKSUM);

    return calc == stored;
}

/**
 * ヘッダチェックサムを再計算してバッファに書き込む。
 *
 * @param buf         ファイルバッファ (インプレース更新)
 * @param header_size ヘッダサイズ
 */
static void update_header_checksum(std::vector<uint8_t>& buf, uint32_t header_size) {
    // チェックサムフィールドを 0 にして計算
    write_le32(buf.data() + OFFSET_CHECKSUM, 0);
    uint32_t crc = calc_crc32(buf.data(), header_size);
    write_le32(buf.data() + OFFSET_CHECKSUM, crc);
}

// ─────────────────────────────────────────────
// 公開 API 実装
// ─────────────────────────────────────────────

const char* convert_result_to_string(ConvertResult result) {
    switch (result) {
        case ConvertResult::SUCCESS:                return "成功";
        case ConvertResult::ERR_INPUT_NOT_FOUND:    return "入力ファイルが見つかりません";
        case ConvertResult::ERR_INPUT_READ_FAILED:  return "入力ファイルの読み込みに失敗しました";
        case ConvertResult::ERR_OUTPUT_WRITE_FAILED:return "出力ファイルの書き込みに失敗しました";
        case ConvertResult::ERR_INVALID_MAGIC:      return "Yayoiバックアップファイルではありません (マジックバイト不一致)";
        case ConvertResult::ERR_INVALID_VERSION:    return "変換対象のバージョンではありません (KB12 のみ変換可能)";
        case ConvertResult::ERR_INVALID_HEADER:     return "ヘッダが不正または破損しています";
        case ConvertResult::ERR_CHECKSUM_MISMATCH:  return "チェックサムが一致しません (ファイルが破損している可能性があります)";
        case ConvertResult::ERR_UNSUPPORTED_FEATURE:return "非対応の機能が含まれています (暗号化ファイルは変換できません)";
        case ConvertResult::ERR_TRUNCATED_FILE:     return "ファイルが途中で切れています";
        default:                                    return "不明なエラー";
    }
}

ConvertResult convert_kb12_to_kb26(
    const std::string& input_path,
    const std::string& output_path,
    ProgressCallback   progress)
{
    auto notify = [&](const std::string& msg, int pct) {
        if (progress) progress(msg, pct);
    };

    // ── Step 1: 読み込み ──────────────────────────────────────
    notify("入力ファイルを読み込んでいます...", 0);

    std::vector<uint8_t> buf;
    if (!read_file(input_path, buf)) {
        // ファイルが存在するか区別
        std::ifstream test(input_path);
        return test ? ConvertResult::ERR_INPUT_READ_FAILED
                    : ConvertResult::ERR_INPUT_NOT_FOUND;
    }

    notify("ファイルを読み込みました (" + std::to_string(buf.size()) + " bytes)", 10);

    // ── Step 2: マジックバイト確認 ───────────────────────────
    notify("マジックバイトを確認しています...", 15);

    if (!check_magic(buf)) {
        return ConvertResult::ERR_INVALID_MAGIC;
    }

    // ── Step 3: ヘッダ最小サイズ確認 ────────────────────────
    if (buf.size() < HEADER_MIN_SIZE) {
        return ConvertResult::ERR_TRUNCATED_FILE;
    }

    uint16_t file_ver    = read_le16(buf.data() + OFFSET_FILE_VERSION);
    uint32_t flags       = read_le32(buf.data() + OFFSET_FLAGS);
    uint32_t header_size = read_le32(buf.data() + OFFSET_HEADER_SIZE);
    uint32_t data_offset = read_le32(buf.data() + OFFSET_DATA_OFFSET);
    uint32_t data_size   = read_le32(buf.data() + OFFSET_DATA_SIZE);

    // ── Step 4: バージョン確認 ───────────────────────────────
    notify("バージョンを確認しています...", 20);

    if (file_ver != KB_VERSION_12) {
        return ConvertResult::ERR_INVALID_VERSION;
    }

    // ── Step 5: ヘッダサイズ整合性確認 ──────────────────────
    if (header_size < HEADER_MIN_SIZE) {
        return ConvertResult::ERR_INVALID_HEADER;
    }
    if (buf.size() < header_size) {
        return ConvertResult::ERR_TRUNCATED_FILE;
    }

    // ── Step 6: 暗号化フラグ確認 ────────────────────────────
    if (flags & FLAG_ENCRYPTED) {
        return ConvertResult::ERR_UNSUPPORTED_FEATURE;
    }

    // ── Step 7: ヘッダチェックサム検証 ──────────────────────
    notify("チェックサムを検証しています...", 30);

    if (flags & FLAG_CHECKSUM) {
        if (!verify_header_checksum(buf, header_size)) {
            return ConvertResult::ERR_CHECKSUM_MISMATCH;
        }
    }

    // ── Step 8: データブロックの範囲確認 ────────────────────
    notify("データブロックの整合性を確認しています...", 40);

    if (data_offset < header_size) {
        return ConvertResult::ERR_INVALID_HEADER;
    }
    if (buf.size() < static_cast<size_t>(data_offset) + data_size) {
        return ConvertResult::ERR_TRUNCATED_FILE;
    }

    // ── Step 9: バージョンフィールドを更新 ──────────────────
    notify("バージョン情報を更新しています...", 60);

    // KbFileHeader のバージョンフィールドを更新
    write_le16(buf.data() + OFFSET_FILE_VERSION, KB_VERSION_26);
    write_le16(buf.data() + OFFSET_APP_VERSION,  KB_VERSION_26);

    // KbMetadata の互換バージョンフィールドを更新
    // メタデータは header の固定フィールド (32バイト) の直後から始まる
    const size_t meta_compat_offset = OFFSET_METADATA + META_COMPATIBLE_VER;
    if (header_size >= meta_compat_offset + 2) {
        uint16_t compat = read_le16(buf.data() + meta_compat_offset);
        // 互換バージョンが12を示している場合のみ更新
        if (compat == COMPAT_VERSION_12) {
            write_le16(buf.data() + meta_compat_offset, COMPAT_VERSION_26);
        }
    }

    // ── Step 10: ヘッダチェックサムを再計算 ─────────────────
    notify("チェックサムを再計算しています...", 75);

    if (flags & FLAG_CHECKSUM) {
        update_header_checksum(buf, header_size);
    }

    // ── Step 11: ファイル末尾のデータチェックサムを再計算 ───
    // データブロック末尾に全体チェックサムが存在する場合に更新する
    // (data_offset + data_size が buf.size() - 4 と等しい場合、末尾4バイトがCRC32とみなす)
    const size_t expected_end = static_cast<size_t>(data_offset) + data_size;
    if (expected_end + 4 == buf.size()) {
        notify("ファイル全体チェックサムを再計算しています...", 85);
        // ファイル全体 (チェックサムフィールド除く) の CRC32
        write_le32(buf.data() + expected_end, 0);
        uint32_t file_crc = calc_crc32(buf.data(), expected_end);
        write_le32(buf.data() + expected_end, file_crc);
    }

    // ── Step 12: 書き出し ────────────────────────────────────
    notify("出力ファイルに書き込んでいます...", 90);

    if (!write_file(output_path, buf)) {
        return ConvertResult::ERR_OUTPUT_WRITE_FAILED;
    }

    notify("変換完了: " + output_path, 100);
    return ConvertResult::SUCCESS;
}

bool validate_kb12_file(const std::string& file_path, std::string* error_msg) {
    auto set_err = [&](const std::string& msg) -> bool {
        if (error_msg) *error_msg = msg;
        return false;
    };

    std::vector<uint8_t> buf;
    if (!read_file(file_path, buf)) {
        std::ifstream test(file_path);
        return test ? set_err("ファイルの読み込みに失敗しました")
                    : set_err("ファイルが見つかりません: " + file_path);
    }

    if (!check_magic(buf)) {
        return set_err("Yayoiバックアップファイルではありません");
    }

    if (buf.size() < HEADER_MIN_SIZE) {
        return set_err("ファイルが短すぎます");
    }

    uint16_t ver = read_le16(buf.data() + OFFSET_FILE_VERSION);
    if (ver != KB_VERSION_12) {
        std::ostringstream oss;
        oss << "バージョンが KB12 ではありません (検出されたバージョン: " << ver << ")";
        return set_err(oss.str());
    }

    uint32_t flags       = read_le32(buf.data() + OFFSET_FLAGS);
    uint32_t header_size = read_le32(buf.data() + OFFSET_HEADER_SIZE);

    if (flags & FLAG_ENCRYPTED) {
        return set_err("暗号化ファイルは変換に対応していません");
    }

    if (header_size < HEADER_MIN_SIZE || buf.size() < header_size) {
        return set_err("ヘッダが不正または破損しています");
    }

    if (flags & FLAG_CHECKSUM) {
        if (!verify_header_checksum(buf, header_size)) {
            return set_err("ヘッダチェックサムが一致しません (ファイルが破損している可能性があります)");
        }
    }

    if (error_msg) *error_msg = "";
    return true;
}

uint16_t get_kb_file_version(const std::string& file_path) {
    std::vector<uint8_t> buf;
    if (!read_file(file_path, buf)) return 0;
    if (!check_magic(buf)) return 0;
    if (buf.size() < OFFSET_FILE_VERSION + 2) return 0;
    return read_le16(buf.data() + OFFSET_FILE_VERSION);
}
