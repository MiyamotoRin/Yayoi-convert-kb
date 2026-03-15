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

#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cctype>
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
    // Windows では日本語パスが ANSI として解釈されるため wstring 版を使用する
#ifdef _WIN32
    std::ifstream ifs(utf8_to_wstring(path), std::ios::binary | std::ios::ate);
#else
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
#endif
    if (!ifs) return false;
    std::streamsize size = ifs.tellg();
    if (size <= 0) return false;
    ifs.seekg(0, std::ios::beg);
    buf.resize(static_cast<size_t>(size));
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(buf.data()), size));
}

static bool write_file(const std::string& path, const std::vector<uint8_t>& buf) {
#ifdef _WIN32
    std::ofstream ofs(utf8_to_wstring(path), std::ios::binary | std::ios::trunc);
#else
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
#endif
    if (!ofs) return false;
    return static_cast<bool>(ofs.write(reinterpret_cast<const char*>(buf.data()),
                                       static_cast<std::streamsize>(buf.size())));
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

    // 末尾から前方向に走査
    for (size_t i = search_limit; ; --i) {
        if (has_yayoi_sig(buf, i, ZIP_EOCD_B2, ZIP_EOCD_B3)) {
            // ZIPコメント長フィールドとファイル末尾が一致するか検証する
            uint16_t comment_len = read_le16(buf.data() + i + ZIP_EOCD_COMMENT_LEN);
            if (i + ZIP_EOCD_FIXED_SIZE + comment_len == buf.size()) {
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
        if (!has_yayoi_sig(buf, pos, ZIP_CDIR_B2, ZIP_CDIR_B3)) return -1;

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
        std::ifstream test(utf8_to_wstring(input_path));
#else
        std::ifstream test(input_path);
#endif
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
        if (!has_yayoi_sig(buf, lfh, ZIP_LOCAL_B2, ZIP_LOCAL_B3)) continue;

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
        std::ifstream test(utf8_to_wstring(file_path));
#else
        std::ifstream test(file_path);
#endif
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
