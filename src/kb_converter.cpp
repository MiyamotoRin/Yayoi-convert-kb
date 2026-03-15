/**
 * kb_converter.cpp
 * 弥生会計バックアップファイル KB12→KB26 コンバータ 実装
 *
 * ファイル形式について:
 *   弥生会計のバックアップファイルは変形ZIPアーカイブ形式を使用する。
 *   標準ZIPの署名 "PK" (0x50 0x4B) がすべて "YZ" (0x59 0x5A) に
 *   置き換えられている。
 *
 * 変換フロー:
 *   1. 入力ファイルを読み込む
 *   2. 先頭マジックバイト "YZ\x03\x04" を確認
 *   3. ZIPのエンドオブセントラルディレクトリ (EOCD) を末尾から探す
 *   4. セントラルディレクトリを解析し、エントリ名を取得する
 *   5. エントリ名・ZIPコメントに含まれる "12" を "26" に書き換える
 *      (ローカルファイルヘッダとセントラルディレクトリの両方を更新)
 *   6. 出力ファイルに書き出す
 *
 * TODO (要調査):
 *   エントリ名やZIPコメントにバージョン文字列が見当たらない場合、
 *   バージョン情報が圧縮データ内に格納されている可能性がある。
 *   その場合は解凍・パッチ・再圧縮が必要になる。
 *   実ファイルのZIPエントリ一覧を確認して格納場所を特定すること。
 *
 *   確認コマンド (Python):
 *     import zipfile, io
 *     data = bytearray(open("file.KB12","rb").read())
 *     data[0], data[1] = 0x50, 0x4B  # YZ -> PK
 *     z = zipfile.ZipFile(io.BytesIO(bytes(data)))
 *     print(z.namelist())
 */

#include "kb_converter.h"
#include "kb_format.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#ifdef _WIN32
#  include <windows.h>
#endif

// ─────────────────────────────────────────────
// ユーティリティ
// ─────────────────────────────────────────────

#ifdef _WIN32
/** UTF-8 文字列を Windows ネイティブの wstring に変換する */
static std::wstring utf8_to_wstring(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                  static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), &result[0], len);
    return result;
}
#endif

static inline uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static inline uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static bool read_file(const std::string& path, std::vector<uint8_t>& buf) {
    // std::ifstream は Windows ANSI パスとして解釈するため _wfopen を使用する
#ifdef _WIN32
    FILE* f = _wfopen(utf8_to_wstring(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    if (size <= 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    buf.resize(static_cast<size_t>(size));
    bool ok = (std::fread(buf.data(), 1, buf.size(), f) == buf.size());
    std::fclose(f);
    return ok;
}

static bool write_file(const std::string& path, const std::vector<uint8_t>& buf) {
#ifdef _WIN32
    FILE* f = _wfopen(utf8_to_wstring(path).c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = (std::fwrite(buf.data(), 1, buf.size(), f) == buf.size());
    std::fclose(f);
    return ok;
}

// ─────────────────────────────────────────────
// ZIPシグネチャ検出 (YZ形式)
// ─────────────────────────────────────────────

/**
 * buf[pos] に YZ + 指定後半2バイトの4バイトシグネチャがあるか確認する。
 * buf の範囲外アクセスを避けるため、pos + 3 < buf.size() を前提とする。
 */
static bool has_yayoi_sig(const std::vector<uint8_t>& buf, size_t pos,
                           uint8_t b2, uint8_t b3)
{
    return (pos + 3 < buf.size())
        && buf[pos + 0] == YAYOI_SIG_0
        && buf[pos + 1] == YAYOI_SIG_1
        && buf[pos + 2] == b2
        && buf[pos + 3] == b3;
}

/**
 * buf[pos] に標準ZIP PK + 指定後半2バイトの4バイトシグネチャがあるか確認する。
 * KB12ファイルによっては先頭 LFH だけ YZ に変換されていて、
 * EOCD やセントラルディレクトリは標準の PK シグネチャのままの場合がある。
 */
static bool has_pk_sig(const std::vector<uint8_t>& buf, size_t pos,
                        uint8_t b2, uint8_t b3)
{
    return (pos + 3 < buf.size())
        && buf[pos + 0] == ZIP_PK_0
        && buf[pos + 1] == ZIP_PK_1
        && buf[pos + 2] == b2
        && buf[pos + 3] == b3;
}

/** YZ または PK どちらかのシグネチャにマッチするか確認する */
static bool has_zip_sig(const std::vector<uint8_t>& buf, size_t pos,
                         uint8_t b2, uint8_t b3)
{
    return has_yayoi_sig(buf, pos, b2, b3) || has_pk_sig(buf, pos, b2, b3);
}

/** 先頭4バイトが弥生バックアップのマジックか確認する */
static bool check_magic(const std::vector<uint8_t>& buf) {
    if (buf.size() < 4) return false;
    return buf[0] == YAYOI_MAGIC[0]
        && buf[1] == YAYOI_MAGIC[1]
        && buf[2] == YAYOI_MAGIC[2]
        && buf[3] == YAYOI_MAGIC[3];
}

// ─────────────────────────────────────────────
// EOCD (エンドオブセントラルディレクトリ) 検索
// ─────────────────────────────────────────────

/**
 * ファイル末尾から EOCD シグネチャ "YZ\x05\x06" を探す。
 * 見つかった場合は eocd_offset に位置を格納して true を返す。
 *
 * EOCDは末尾から最大 EOCD固定部 + ZIPコメント最大長 (65535) の範囲に存在する。
 * 複数の候補がある場合は最も末尾寄りで整合性が取れるものを採用する。
 */
static bool find_eocd(const std::vector<uint8_t>& buf, size_t& eocd_offset) {
    if (buf.size() < ZIP_EOCD_FIXED_SIZE) return false;

    const size_t search_limit = buf.size() - ZIP_EOCD_FIXED_SIZE;
    const size_t search_start = (buf.size() > ZIP_EOCD_FIXED_SIZE + ZIP_MAX_COMMENT_SIZE)
                                ? buf.size() - ZIP_EOCD_FIXED_SIZE - ZIP_MAX_COMMENT_SIZE
                                : 0;

    // 末尾から前方向に走査 (YZ・PK 両形式に対応)
    // 弥生バックアップは EOCD の後ろに独自メタデータを付加することがあるため、
    // 末尾一致 (==) ではなくバッファ内に収まるか (<=) で検証する。
    // 代わりに CDオフセット・CDシグネチャの整合性チェックで誤検知を防ぐ。
    for (size_t i = search_limit; ; --i) {
        if (has_zip_sig(buf, i, ZIP_EOCD_B2, ZIP_EOCD_B3)) {
            uint16_t comment_len = read_le16(buf.data() + i + ZIP_EOCD_COMMENT_LEN);
            uint32_t cd_off  = read_le32(buf.data() + i + ZIP_EOCD_CD_OFFSET);
            uint32_t cd_size = read_le32(buf.data() + i + ZIP_EOCD_CD_SIZE);

            // コメントがバッファに収まるか
            bool comment_ok = (i + ZIP_EOCD_FIXED_SIZE + comment_len <= buf.size());
            // CDの位置がEOCDより前に収まるか
            bool cd_pos_ok  = (cd_off <= i)
                           && (static_cast<uint64_t>(cd_off) + cd_size <= i);
            // CDの先頭シグネチャが正しいか (空CDは除く)
            bool cd_sig_ok  = (cd_size == 0)
                           || has_zip_sig(buf, cd_off, ZIP_CDIR_B2, ZIP_CDIR_B3);

            if (comment_ok && cd_pos_ok && cd_sig_ok) {
                eocd_offset = i;
                return true;
            }
        }
        if (i == search_start) break;
    }
    return false;
}

// ─────────────────────────────────────────────
// ZIPエントリ情報
// ─────────────────────────────────────────────

struct ZipEntry {
    size_t      cd_offset;      // セントラルディレクトリ内の位置
    size_t      local_offset;   // ローカルファイルヘッダの位置
    std::string name;           // エントリ名
    uint16_t    name_len;       // エントリ名バイト長
};

/**
 * セントラルディレクトリを解析して ZipEntry の一覧を返す。
 * 返り値: エントリを解析できた数 (失敗時は -1)
 */
static int parse_central_directory(const std::vector<uint8_t>& buf,
                                    size_t cd_offset, uint32_t cd_size,
                                    uint16_t num_entries,
                                    std::vector<ZipEntry>& entries)
{
    if (cd_offset + cd_size > buf.size()) return -1;

    size_t pos = cd_offset;
    const size_t cd_end = cd_offset + cd_size;
    int count = 0;

    for (uint16_t i = 0; i < num_entries; ++i) {
        // セントラルディレクトリエントリのシグネチャ確認 "YZ\x01\x02"
        if (pos + ZIP_CDE_FIXED_SIZE > cd_end) return -1;
        if (!has_zip_sig(buf, pos, ZIP_CDIR_B2, ZIP_CDIR_B3)) return -1;

        uint16_t name_len    = read_le16(buf.data() + pos + ZIP_CDE_FILENAME_LEN);
        uint16_t extra_len   = read_le16(buf.data() + pos + ZIP_CDE_EXTRA_LEN);
        uint16_t comment_len = read_le16(buf.data() + pos + ZIP_CDE_COMMENT_LEN);
        uint32_t local_off   = read_le32(buf.data() + pos + ZIP_CDE_LOCAL_OFFSET);

        if (pos + ZIP_CDE_FIXED_SIZE + name_len > cd_end) return -1;

        ZipEntry entry;
        entry.cd_offset    = pos;
        entry.local_offset = static_cast<size_t>(local_off);
        entry.name_len     = name_len;
        entry.name.assign(
            reinterpret_cast<const char*>(buf.data() + pos + ZIP_CDE_FIXED_SIZE),
            name_len
        );

        entries.push_back(entry);
        ++count;

        pos += ZIP_CDE_FIXED_SIZE + name_len + extra_len + comment_len;
    }
    return count;
}

// ─────────────────────────────────────────────
// バージョン文字列パッチ
// ─────────────────────────────────────────────

/**
 * バッファ内 offset から name_len バイトのエントリ名を読み取り、
 * "12" を "26" に置換して書き戻す。
 * 置換が1件以上行われた場合 true を返す。
 *
 * 注意: "12" という部分文字列が年月日など無関係な箇所に現れる可能性がある。
 *       現状は単純置換としているが、実ファイルで検証後に絞り込むことを推奨する。
 */
static bool patch_version_in_name(std::vector<uint8_t>& buf,
                                   size_t name_offset, uint16_t name_len)
{
    bool patched = false;
    for (uint16_t i = 0; i + 1 < name_len; ++i) {
        if (buf[name_offset + i] == '1' && buf[name_offset + i + 1] == '2') {
            buf[name_offset + i]     = '2';
            buf[name_offset + i + 1] = '6';
            patched = true;
        }
    }
    return patched;
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
        case ConvertResult::ERR_INVALID_MAGIC:      return "Yayoiバックアップファイルではありません"
                                                           " (先頭バイトが YZ\\x03\\x04 でない)";
        case ConvertResult::ERR_INVALID_VERSION:    return "変換対象のバージョンではありません (KB12 のみ変換可能)";
        case ConvertResult::ERR_INVALID_HEADER:     return "ZIPヘッダが不正または破損しています";
        case ConvertResult::ERR_CHECKSUM_MISMATCH:  return "チェックサムが一致しません (ファイルが破損している可能性があります)";
        case ConvertResult::ERR_UNSUPPORTED_FEATURE:return "バージョン情報がエントリ名・ZIPコメントに見つかりませんでした。"
                                                           " 圧縮データ内に格納されている可能性があります (要調査)";
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
#ifdef _WIN32
        FILE* test = _wfopen(utf8_to_wstring(input_path).c_str(), L"rb");
#else
        FILE* test = std::fopen(input_path.c_str(), "rb");
#endif
        if (test) std::fclose(test);
        return test ? ConvertResult::ERR_INPUT_READ_FAILED
                    : ConvertResult::ERR_INPUT_NOT_FOUND;
    }

    notify("ファイルを読み込みました (" + std::to_string(buf.size()) + " bytes)", 10);

    // ── Step 2: マジックバイト確認 "YZ\x03\x04" ──────────────
    notify("マジックバイトを確認しています...", 15);

    if (!check_magic(buf)) {
        return ConvertResult::ERR_INVALID_MAGIC;
    }

    if (buf.size() < YAYOI_MIN_FILE_SIZE) {
        return ConvertResult::ERR_TRUNCATED_FILE;
    }

    // ── Step 3: EOCD を末尾から探す ──────────────────────────
    notify("ZIPエンドレコードを検索しています...", 25);

    size_t eocd_offset = 0;
    if (!find_eocd(buf, eocd_offset)) {
        return ConvertResult::ERR_INVALID_HEADER;
    }

    uint16_t num_entries = read_le16(buf.data() + eocd_offset + ZIP_EOCD_CD_ENTRIES_TOTAL);
    uint32_t cd_size     = read_le32(buf.data() + eocd_offset + ZIP_EOCD_CD_SIZE);
    uint32_t cd_offset   = read_le32(buf.data() + eocd_offset + ZIP_EOCD_CD_OFFSET);
    uint16_t zip_comment_len = read_le16(buf.data() + eocd_offset + ZIP_EOCD_COMMENT_LEN);

    notify("ZIPエントリ数: " + std::to_string(num_entries), 30);

    if (cd_offset + cd_size > buf.size()) {
        return ConvertResult::ERR_INVALID_HEADER;
    }

    // ── Step 4: セントラルディレクトリを解析 ─────────────────
    notify("セントラルディレクトリを解析しています...", 40);

    std::vector<ZipEntry> entries;
    if (parse_central_directory(buf, cd_offset, cd_size, num_entries, entries) < 0) {
        return ConvertResult::ERR_INVALID_HEADER;
    }

    // エントリ名をログ出力 (デバッグ・フォーマット調査用)
    for (const auto& e : entries) {
        notify("  エントリ: \"" + e.name + "\"", -1);
    }

    // ── Step 5: バージョン文字列 "12" を "26" に書き換える ───
    notify("バージョン情報を更新しています...", 60);

    bool any_patched = false;

    // 5a. セントラルディレクトリのエントリ名を更新
    for (const auto& e : entries) {
        size_t cd_name_offset = e.cd_offset + ZIP_CDE_FIXED_SIZE;
        if (patch_version_in_name(buf, cd_name_offset, e.name_len)) {
            any_patched = true;
            notify("  CD エントリ名を更新: \"" + e.name + "\"", -1);
        }

        // 5b. 対応するローカルファイルヘッダのエントリ名も更新する
        size_t lfh = e.local_offset;
        if (lfh + ZIP_LFH_FIXED_SIZE > buf.size()) continue;
        if (!has_zip_sig(buf, lfh, ZIP_LOCAL_B2, ZIP_LOCAL_B3)) continue;

        uint16_t lfh_name_len = read_le16(buf.data() + lfh + ZIP_LFH_FILENAME_LEN);
        if (lfh + ZIP_LFH_FIXED_SIZE + lfh_name_len > buf.size()) continue;

        size_t lfh_name_offset = lfh + ZIP_LFH_FIXED_SIZE;
        if (patch_version_in_name(buf, lfh_name_offset, lfh_name_len)) {
            any_patched = true;
        }
    }

    // 5c. ZIPコメントを更新
    if (zip_comment_len > 0) {
        size_t zip_comment_offset = eocd_offset + ZIP_EOCD_FIXED_SIZE;
        for (uint16_t i = 0; i + 1 < zip_comment_len; ++i) {
            if (buf[zip_comment_offset + i]     == '1' &&
                buf[zip_comment_offset + i + 1] == '2')
            {
                buf[zip_comment_offset + i]     = '2';
                buf[zip_comment_offset + i + 1] = '6';
                any_patched = true;
            }
        }
    }

    if (!any_patched) {
        // バージョン文字列が見つからなかった場合
        // 圧縮データ内に格納されている可能性があるため調査が必要
        return ConvertResult::ERR_UNSUPPORTED_FEATURE;
    }

    // ── Step 6: 書き出し ─────────────────────────────────────
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
#ifdef _WIN32
        FILE* test = _wfopen(utf8_to_wstring(file_path).c_str(), L"rb");
#else
        FILE* test = std::fopen(file_path.c_str(), "rb");
#endif
        if (test) std::fclose(test);
        return test ? set_err("ファイルの読み込みに失敗しました")
                    : set_err("ファイルが見つかりません: " + file_path);
    }

    if (!check_magic(buf)) {
        return set_err("Yayoiバックアップファイルではありません"
                       " (期待: YZ\\x03\\x04, 実際の先頭バイト: "
                       + (buf.size() >= 1 ? std::to_string(buf[0]) : "?")
                       + " " + (buf.size() >= 2 ? std::to_string(buf[1]) : "?") + ")");
    }

    if (buf.size() < YAYOI_MIN_FILE_SIZE) {
        return set_err("ファイルが短すぎます");
    }

    size_t eocd_offset = 0;
    if (!find_eocd(buf, eocd_offset)) {
        return set_err("ZIPエンドレコードが見つかりません (ファイルが破損している可能性があります)");
    }

    if (error_msg) *error_msg = "";
    return true;
}

uint16_t get_kb_file_version(const std::string& file_path) {
    // TODO: ZIPエントリ名またはコメントからバージョン番号を取得する実装
    //       現在はマジックバイトが正しければ 12 を返す暫定実装
    std::vector<uint8_t> buf;
    if (!read_file(file_path, buf)) return 0;
    if (!check_magic(buf)) return 0;
    return 12;
}

// ─────────────────────────────────────────────
// 診断ユーティリティ
// ─────────────────────────────────────────────

/** バイト列を "XX XX XX ..." 形式のhex文字列に変換する */
static std::string hex_dump(const uint8_t* p, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
        if (i) oss << ' ';
        oss << std::hex << std::uppercase
            << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(p[i]);
    }
    return oss.str();
}

/** バイトを印字可能なASCII文字または '.' に変換する */
static char to_printable(uint8_t b) {
    return (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
}

/** 16バイト単位のhexダンプ行を出力する */
static void print_hex_region(const std::vector<uint8_t>& buf,
                               size_t start, size_t len,
                               const std::string& label)
{
    std::cout << label << " (offset 0x"
              << std::hex << std::uppercase << start << " から "
              << std::dec << len << " バイト):\n";
    for (size_t i = 0; i < len; i += 16) {
        size_t row_len = std::min<size_t>(16, len - i);
        std::cout << "  " << std::hex << std::uppercase
                  << std::setw(6) << std::setfill('0') << (start + i) << "  ";
        for (size_t j = 0; j < row_len; ++j) {
            std::cout << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(buf[start + i + j]) << ' ';
        }
        for (size_t j = row_len; j < 16; ++j) std::cout << "   ";
        std::cout << " |";
        for (size_t j = 0; j < row_len; ++j)
            std::cout << to_printable(buf[start + i + j]);
        std::cout << "|\n";
    }
    std::cout << std::dec;
}

void diagnose_kb_file(const std::string& file_path) {
    std::cout << "=== KB ファイル診断 ===\n";
    std::cout << "ファイル: " << file_path << "\n\n";

    std::vector<uint8_t> buf;
    if (!read_file(file_path, buf)) {
        std::cout << "[ERROR] ファイルを読み込めませんでした\n";
        return;
    }

    // ── 基本情報 ─────────────────────────────────────────────
    std::cout << "ファイルサイズ: " << buf.size() << " バイト (0x"
              << std::hex << std::uppercase << buf.size() << ")\n\n"
              << std::dec;

    // ── 先頭32バイト ─────────────────────────────────────────
    size_t head_len = std::min<size_t>(32, buf.size());
    print_hex_region(buf, 0, head_len, "先頭 32 バイト");
    std::cout << "\n";

    // ── 末尾64バイト ─────────────────────────────────────────
    size_t tail_len = std::min<size_t>(64, buf.size());
    size_t tail_start = buf.size() - tail_len;
    print_hex_region(buf, tail_start, tail_len, "末尾 64 バイト");
    std::cout << "\n";

    // ── マジックバイト確認 ────────────────────────────────────
    std::cout << "--- マジックバイト ---\n";
    if (buf.size() >= 4) {
        std::cout << "先頭 4 バイト: " << hex_dump(buf.data(), 4) << "\n";
        bool is_yayoi = check_magic(buf);
        bool is_pk    = (buf[0] == 'P' && buf[1] == 'K');
        std::cout << "  -> " << (is_yayoi ? "YZ 形式 (Yayoi バックアップ)" :
                                  is_pk    ? "PK 形式 (標準 ZIP)" :
                                             "不明な形式") << "\n";
    }
    std::cout << "\n";

    // ── シグネチャ出現位置スキャン ────────────────────────────
    struct SigInfo { uint8_t b0, b1, b2, b3; const char* name; };
    SigInfo sigs[] = {
        { 'Y','Z',0x03,0x04, "YZ ローカルファイルヘッダ (YZ\\x03\\x04)" },
        { 'Y','Z',0x01,0x02, "YZ セントラルディレクトリ  (YZ\\x01\\x02)" },
        { 'Y','Z',0x05,0x06, "YZ EOCD                   (YZ\\x05\\x06)" },
        { 'P','K',0x03,0x04, "PK ローカルファイルヘッダ (PK\\x03\\x04)" },
        { 'P','K',0x01,0x02, "PK セントラルディレクトリ  (PK\\x01\\x02)" },
        { 'P','K',0x05,0x06, "PK EOCD                   (PK\\x05\\x06)" },
    };

    std::cout << "--- シグネチャ出現位置スキャン ---\n";
    for (const auto& s : sigs) {
        std::vector<size_t> positions;
        for (size_t i = 0; i + 3 < buf.size(); ++i) {
            if (buf[i]==s.b0 && buf[i+1]==s.b1 && buf[i+2]==s.b2 && buf[i+3]==s.b3)
                positions.push_back(i);
        }
        std::cout << s.name << ": ";
        if (positions.empty()) {
            std::cout << "なし\n";
        } else {
            std::cout << positions.size() << " 件\n";
            for (size_t p : positions) {
                std::cout << "  offset 0x" << std::hex << std::uppercase << p
                          << " (" << std::dec << p << ")\n";
            }
        }
    }
    std::cout << "\n";

    // ── EOCD候補の詳細検証 ────────────────────────────────────
    std::cout << "--- EOCD 候補の検証 ---\n";
    struct EocdSig { uint8_t b0, b1; const char* label; };
    EocdSig eocd_sigs[] = { {'Y','Z',"YZ"}, {'P','K',"PK"} };

    bool any_eocd = false;
    for (const auto& es : eocd_sigs) {
        for (size_t i = 0; i + 3 < buf.size(); ++i) {
            if (buf[i]==es.b0 && buf[i+1]==es.b1 && buf[i+2]==0x05 && buf[i+3]==0x06) {
                any_eocd = true;
                uint16_t comment_len = (i + ZIP_EOCD_COMMENT_LEN + 1 < buf.size())
                    ? read_le16(buf.data() + i + ZIP_EOCD_COMMENT_LEN) : 0xFFFF;
                size_t expected_end = i + ZIP_EOCD_FIXED_SIZE + comment_len;

                std::cout << es.label << " EOCD at offset 0x"
                          << std::hex << std::uppercase << i
                          << " (" << std::dec << i << "):\n";
                std::cout << "  comment_len フィールド = " << comment_len << "\n";
                std::cout << "  期待するファイル末尾位置 = "
                          << expected_end << " (実際 = " << buf.size() << ")\n";
                // 弥生形式は EOCD の後ろに独自メタデータを付加するため末尾一致しない場合がある。
                // 代わりに CD整合性 (cd_off + cd_size == EOCD offset) で有効性を判定する。
                bool end_match = (expected_end == buf.size());
                std::cout << "  末尾一致: " << (end_match ? "OK" : "NG (弥生形式の後付けメタデータの可能性あり)") << "\n";
                if (!end_match) {
                    std::cout << "  ずれ = "
                              << static_cast<long long>(expected_end)
                                 - static_cast<long long>(buf.size())
                              << " バイト\n";
                }
                // EOCDフィールドの内容も表示
                if (i + ZIP_EOCD_FIXED_SIZE <= buf.size()) {
                    uint16_t disk_num      = read_le16(buf.data() + i + 4);
                    uint16_t cd_disk       = read_le16(buf.data() + i + 6);
                    uint16_t cd_entries_on_disk = read_le16(buf.data() + i + 8);
                    uint16_t cd_entries_total   = read_le16(buf.data() + i + 10);
                    uint32_t cd_size       = read_le32(buf.data() + i + 12);
                    uint32_t cd_offset     = read_le32(buf.data() + i + 16);
                    std::cout << "  ディスク番号=" << disk_num
                              << " CDディスク=" << cd_disk
                              << " エントリ(このディスク)=" << cd_entries_on_disk
                              << " エントリ(合計)=" << cd_entries_total << "\n";
                    std::cout << "  CDサイズ=0x" << std::hex << cd_size
                              << " CDオフセット=0x" << cd_offset << "\n" << std::dec;
                    // CD整合性: cd_offset + cd_size == EOCD offset なら有効
                    bool cd_ok = (static_cast<uint64_t>(cd_offset) + cd_size == i);
                    std::cout << "  CD整合性 (CDオフセット+CDサイズ==EOCD位置): "
                              << (cd_ok ? "OK (有効)" : "NG") << "\n";

                    // CD 領域のダンプ
                    if (cd_offset < buf.size() && cd_size > 0) {
                        size_t dump_len = std::min<size_t>(cd_size, buf.size() - cd_offset);
                        print_hex_region(buf, cd_offset, dump_len,
                                         "  セントラルディレクトリ 全体");

                        // CDE フィールド解析 (先頭エントリのみ)
                        size_t pos = cd_offset;
                        if (pos + ZIP_CDE_FIXED_SIZE <= cd_offset + cd_size
                         && pos + ZIP_CDE_FIXED_SIZE <= buf.size()) {
                            std::cout << "\n  [CDE フィールド解析 (先頭エントリ)]\n";
                            std::cout << "  シグネチャ : "
                                      << hex_dump(buf.data() + pos, 4) << "\n";
                            uint16_t ver_made   = read_le16(buf.data() + pos + 4);
                            uint16_t ver_need   = read_le16(buf.data() + pos + 6);
                            uint16_t flags      = read_le16(buf.data() + pos + 8);
                            uint16_t method     = read_le16(buf.data() + pos + 10);
                            uint16_t name_len2  = read_le16(buf.data() + pos + ZIP_CDE_FILENAME_LEN);
                            uint16_t extra_len2 = read_le16(buf.data() + pos + ZIP_CDE_EXTRA_LEN);
                            uint16_t cmt_len2   = read_le16(buf.data() + pos + ZIP_CDE_COMMENT_LEN);
                            uint32_t local_off2 = read_le32(buf.data() + pos + ZIP_CDE_LOCAL_OFFSET);
                            std::cout << "  ver_made=" << ver_made
                                      << " ver_need=" << ver_need
                                      << " flags=0x" << std::hex << flags
                                      << " method=" << std::dec << method << "\n";
                            std::cout << "  name_len=" << name_len2
                                      << " extra_len=" << extra_len2
                                      << " comment_len=" << cmt_len2
                                      << " local_offset=0x" << std::hex << local_off2
                                      << std::dec << "\n";
                            std::cout << "  fixed(" << ZIP_CDE_FIXED_SIZE << ") + name_len = " << (ZIP_CDE_FIXED_SIZE + name_len2)
                                      << " / CD全体 = " << cd_size
                                      << " → " << ((ZIP_CDE_FIXED_SIZE + name_len2 <= cd_size) ? "OK" : "NG (name_len が CD サイズを超えている)") << "\n";

                            // ファイル名バイト列
                            if (pos + ZIP_CDE_FIXED_SIZE + name_len2 <= buf.size()) {
                                std::string raw_name(
                                    reinterpret_cast<const char*>(buf.data() + pos + ZIP_CDE_FIXED_SIZE),
                                    name_len2);
                                std::cout << "  ファイル名 (raw hex): "
                                          << hex_dump(buf.data() + pos + ZIP_CDE_FIXED_SIZE,
                                                      name_len2) << "\n";
                                std::cout << "  ファイル名 (ASCII): ";
                                for (char c : raw_name) std::cout << to_printable(static_cast<uint8_t>(c));
                                std::cout << "\n";
                            }

                            // LFH のダンプ
                            if (local_off2 + ZIP_LFH_FIXED_SIZE <= buf.size()) {
                                print_hex_region(buf, local_off2,
                                                 std::min<size_t>(64, buf.size() - local_off2),
                                                 "  ローカルファイルヘッダ (先頭 64 バイト)");
                            }
                        }
                    }
                }
                std::cout << "\n";
            }
        }
    }
    if (!any_eocd) {
        std::cout << "EOCD シグネチャが一切見つかりませんでした。\n"
                  << "ファイルが破損しているか、全く異なるフォーマットの可能性があります。\n\n";
    }

    std::cout << "=== 診断終了 ===\n";
}
