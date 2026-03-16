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
#include <zlib.h>

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

static inline uint64_t read_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

static inline void write_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

static inline void write_le32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) { p[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
}

static inline void write_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) { p[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
}

// ─────────────────────────────────────────────
// Jet 4.0 MDB 内の SystemInfo テーブル DataVersion パッチ
// ─────────────────────────────────────────────

/**
 * Jet 4.0 MDB バイナリ内の SystemInfo テーブルを探し、
 * Id=1 の行の DataVersion を 3600 に更新し、
 * 弥生26が必要とする追加行 (Id=5,6,9,10,11) を挿入する。
 *
 * SystemInfo テーブルの識別:
 *   TDEF ページ (type=0x02) 内に UTF-16LE の "DataVersion" 列名が含まれる。
 *
 * 行レイアウト (27バイト固定):
 *   [0-1]  : 行フラグ (0x05 0x00)
 *   [2-5]  : Id (Int32 LE)
 *   [6-9]  : DataVersion (Int32 LE)
 *   [10-13]: DataStatus (Int32 LE)
 *   [14-17]: ShoushinStatus (Int32 LE)
 *   [18-25]: RecTimeStamp (Double LE)
 *   [26]   : ヌルビットマップ (0x1F)
 *
 * Jet 4.0 データページ:
 *   行データはページ末尾から下方向に成長し、行オフセットテーブルは
 *   ページ先頭のヘッダ (14バイト) の直後に上方向に成長する。
 */
static bool patch_mdb_sysinfo_version(std::vector<uint8_t>& mdb,
                                       ProgressCallback notify)
{
    static constexpr int32_t  TARGET_DATA_VERSION = 3600; // 0x0E10
    static constexpr size_t   JET_PAGE_SIZE       = 4096;
    static constexpr size_t   ROW_SIZE            = 27;   // 固定長行サイズ

    // 弥生26の実KD26ファイルに存在する追加 SystemInfo 行
    struct RequiredRow { int32_t id; int32_t data_version; };
    static constexpr RequiredRow REQUIRED_ROWS[] = {
        {  5, 3200 },  // サブシステムバージョン
        {  6,   13 },  // カウンター
        {  9, 3400 },  // サブシステムバージョン
        { 10, 3200 },  // サブシステムバージョン
        { 11, 3004 },  // サブシステムバージョン
    };

    if (mdb.size() < JET_PAGE_SIZE || mdb.size() % JET_PAGE_SIZE != 0) return false;
    const size_t num_pages = mdb.size() / JET_PAGE_SIZE;

    // UTF-16LE "DataVersion" (22バイト)
    static const uint8_t DATA_VERSION_UTF16[] = {
        0x44,0x00,0x61,0x00,0x74,0x00,0x61,0x00,
        0x56,0x00,0x65,0x00,0x72,0x00,0x73,0x00,
        0x69,0x00,0x6f,0x00,0x6e,0x00
    };
    static constexpr size_t DV_UTF16_LEN = sizeof(DATA_VERSION_UTF16);

    // ── Step A: "DataVersion" 列を持つ TDEF ページを探す ─────────
    uint32_t sysinfo_tdef = 0;
    for (size_t pg = 0; pg < num_pages && !sysinfo_tdef; ++pg) {
        const uint8_t* base = mdb.data() + pg * JET_PAGE_SIZE;
        if (base[0] != 0x02) continue; // TDEF ページのみ
        for (size_t i = 0x50; i + DV_UTF16_LEN <= JET_PAGE_SIZE; ++i) {
            if (std::memcmp(base + i, DATA_VERSION_UTF16, DV_UTF16_LEN) == 0) {
                sysinfo_tdef = static_cast<uint32_t>(pg);
                break;
            }
        }
    }
    if (!sysinfo_tdef) {
        if (notify) notify("  [警告] SystemInfo TDEF が見つかりません", -1);
        return false;
    }
    if (notify) notify("  SystemInfo TDEF: page=" + std::to_string(sysinfo_tdef), -1);

    // ── Step B: owner=sysinfo_tdef のデータページを探す ──────────
    bool patched = false;
    for (size_t pg = 0; pg < num_pages; ++pg) {
        uint8_t* base = mdb.data() + pg * JET_PAGE_SIZE;
        if (base[0] != 0x01) continue; // データページのみ
        const uint32_t owner = read_le32(base + 4);
        if (owner != sysinfo_tdef) continue;

        uint16_t row_count = read_le16(base + 12);

        // ── Step B-1: 既存行の Id を収集し、Id=1 を更新 ────────
        // RecTimeStamp を Id=1 行からコピーして新規行に流用する
        uint8_t rec_timestamp[8] = {};
        bool has_ids[256] = {};  // 簡易: Id < 256 を想定
        for (uint16_t r = 0; r < row_count && r < 100; ++r) {
            const uint16_t row_off = read_le16(base + 14 + r * 2);
            if (row_off < 10 || row_off >= JET_PAGE_SIZE) continue;

            uint8_t* row = base + row_off;
            if (pg * JET_PAGE_SIZE + row_off + ROW_SIZE > mdb.size()) continue;

            const int32_t id = static_cast<int32_t>(read_le32(row + 2));
            if (id >= 0 && id < 256) has_ids[id] = true;

            if (id == 1) {
                const int32_t dv = static_cast<int32_t>(read_le32(row + 6));
                if (notify) {
                    notify("  SystemInfo Id=1 DataVersion: "
                           + std::to_string(dv) + " -> "
                           + std::to_string(TARGET_DATA_VERSION), -1);
                }
                write_le32(row + 6, static_cast<uint32_t>(TARGET_DATA_VERSION));
                std::memcpy(rec_timestamp, row + 18, 8);
                patched = true;
            }
        }

        // ── Step B-2: 不足行を追加 ───────────────────────────────
        // 行データはページ末尾から下方向に成長する
        // 現在の最下位行オフセットを求める
        uint16_t min_row_off = static_cast<uint16_t>(JET_PAGE_SIZE);
        for (uint16_t r = 0; r < row_count && r < 100; ++r) {
            const uint16_t off = read_le16(base + 14 + r * 2);
            if (off > 0 && off < min_row_off) min_row_off = off;
        }

        uint16_t rows_added = 0;
        for (const auto& req : REQUIRED_ROWS) {
            if (req.id >= 0 && req.id < 256 && has_ids[req.id]) continue;

            // 新規行の配置位置
            const uint16_t new_row_off = min_row_off - static_cast<uint16_t>(ROW_SIZE);

            // オフセットテーブル末尾 = 14 + (row_count + rows_added + 1) * 2
            // 行データはそれより下にある必要がある
            const size_t offset_table_end = 14 + (static_cast<size_t>(row_count)
                                            + rows_added + 1) * 2;
            if (new_row_off < offset_table_end) {
                if (notify) notify("  [警告] ページ空き不足、行追加中断", -1);
                break;
            }

            // 行データを書き込み
            uint8_t* row = base + new_row_off;
            row[0] = 0x05; row[1] = 0x00;                            // flags
            write_le32(row + 2,  static_cast<uint32_t>(req.id));      // Id
            write_le32(row + 6,  static_cast<uint32_t>(req.data_version)); // DataVersion
            write_le32(row + 10, 0);                                  // DataStatus
            write_le32(row + 14, 0);                                  // ShoushinStatus
            std::memcpy(row + 18, rec_timestamp, 8);                  // RecTimeStamp
            row[26] = 0x1F;                                           // null bitmap

            // オフセットテーブルに追加
            const uint16_t slot = row_count + rows_added;
            write_le16(base + 14 + slot * 2, new_row_off);

            min_row_off = new_row_off;
            rows_added++;

            if (notify) {
                notify("  SystemInfo Id=" + std::to_string(req.id)
                       + " 追加 (DataVersion=" + std::to_string(req.data_version) + ")", -1);
            }
        }

        // データページの行数と空き領域を更新
        if (rows_added > 0) {
            const uint16_t new_row_count = row_count + rows_added;
            write_le16(base + 12, new_row_count);

            // 空き領域 = 最下位行オフセット - (ヘッダ14バイト + オフセットテーブル)
            const uint16_t free_space = min_row_off
                - static_cast<uint16_t>(14 + new_row_count * 2);
            write_le16(base + 2, free_space);

            patched = true;
        }
    }

    // ── Step C: TDEF ページの行カウントを更新 ──────────────────
    // Jet 4.0 TDEF ヘッダのオフセット 16-19 にテーブルの総行数が格納される
    if (patched) {
        uint8_t* tdef_base = mdb.data() + sysinfo_tdef * JET_PAGE_SIZE;
        const uint32_t old_tdef_rows = read_le32(tdef_base + 16);
        // データページの行数合計を TDEF に反映
        uint32_t total_rows = 0;
        for (size_t pg = 0; pg < num_pages; ++pg) {
            const uint8_t* dp = mdb.data() + pg * JET_PAGE_SIZE;
            if (dp[0] != 0x01) continue;
            if (read_le32(dp + 4) != sysinfo_tdef) continue;
            total_rows += read_le16(dp + 12);
        }
        write_le32(tdef_base + 16, total_rows);
        if (notify) {
            notify("  TDEF 行カウント更新: "
                   + std::to_string(old_tdef_rows) + " -> "
                   + std::to_string(total_rows), -1);
        }
    }
    return patched;
}

// ─────────────────────────────────────────────
// KD エントリの展開・パッチ・再圧縮
// ─────────────────────────────────────────────

/**
 * KB バッファ内の KD エントリ (Jet 4.0 MDB, deflate 圧縮) を展開し、
 * SystemInfo.DataVersion をパッチして再圧縮する。
 * 圧縮後サイズが変化した場合はバッファを伸縮し、LFH/CDE/EOCD を更新する。
 */
static ConvertResult patch_kd_entry(
    std::vector<uint8_t>& buf,
    size_t lfh_offset,      // ローカルファイルヘッダの先頭位置
    size_t data_offset,     // 圧縮データの先頭位置 (= lfh + 38 + name + extra)
    uint64_t comp_size,     // 圧縮後サイズ
    uint64_t uncomp_size,   // 展開後サイズ
    size_t cde_offset,      // セントラルディレクトリエントリの位置
    size_t eocd_offset,     // EOCD の位置
    ProgressCallback notify)
{
    if (data_offset + comp_size > buf.size()) return ConvertResult::ERR_TRUNCATED_FILE;

    // ── 1. 展開 ─────────────────────────────────────────────────
    notify("  KD データを展開しています...", 62);
    std::vector<uint8_t> mdb(uncomp_size);
    {
        z_stream zs{};
        zs.next_in  = buf.data() + data_offset;
        zs.avail_in = static_cast<uInt>(comp_size);
        zs.next_out = mdb.data();
        zs.avail_out= static_cast<uInt>(uncomp_size);
        if (inflateInit2(&zs, -15) != Z_OK)
            return ConvertResult::ERR_UNSUPPORTED_FEATURE;
        const int ret = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (ret != Z_STREAM_END)
            return ConvertResult::ERR_UNSUPPORTED_FEATURE;
        mdb.resize(zs.total_out);
    }
    notify("  展開完了: " + std::to_string(mdb.size()) + " bytes", 65);

    // ── 2. MDB パッチ ────────────────────────────────────────────
    notify("  SystemInfo DataVersion を更新しています...", 70);
    if (!patch_mdb_sysinfo_version(mdb, notify)) {
        notify("  [情報] DataVersion の更新対象が見つかりませんでした (既に最新の可能性)", -1);
        // 更新不要でもエラーにはしない
    }

    // ── 3. CRC-32 計算 ─────────────────────────────────────────
    const uint32_t new_crc = static_cast<uint32_t>(
        crc32(0, mdb.data(), static_cast<uInt>(mdb.size())));

    // ── 4. 再圧縮 ───────────────────────────────────────────────
    notify("  KD データを再圧縮しています...", 75);
    std::vector<uint8_t> compressed;
    {
        compressed.resize(static_cast<size_t>(compressBound(static_cast<uLong>(mdb.size()))));

        z_stream zs{};
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            return ConvertResult::ERR_UNSUPPORTED_FEATURE;
        zs.next_in  = mdb.data();
        zs.avail_in = static_cast<uInt>(mdb.size());
        zs.next_out = compressed.data();
        zs.avail_out= static_cast<uInt>(compressed.size());
        const int ret = deflate(&zs, Z_FINISH);
        deflateEnd(&zs);
        if (ret != Z_STREAM_END)
            return ConvertResult::ERR_UNSUPPORTED_FEATURE;
        compressed.resize(zs.total_out);
    }
    notify("  再圧縮完了: " + std::to_string(compressed.size()) + " bytes", 80);

    const uint64_t new_comp_size = compressed.size();
    const int64_t  delta = static_cast<int64_t>(new_comp_size)
                         - static_cast<int64_t>(comp_size);

    // ── 5. バッファ内の圧縮データを差し替え ────────────────────
    buf.erase(buf.begin() + static_cast<std::ptrdiff_t>(data_offset),
              buf.begin() + static_cast<std::ptrdiff_t>(data_offset + comp_size));
    buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(data_offset),
               compressed.begin(), compressed.end());

    // ── 6. LFH の CRC-32 と comp_size を更新 ─────────────────
    write_le32(buf.data() + lfh_offset + ZIP_LFH_CRC32,           new_crc);
    write_le64(buf.data() + lfh_offset + ZIP_LFH_COMPRESSED_SIZE, new_comp_size);
    // uncomp_size は変化しないので ZIP_LFH_UNCOMPRESSED_SIZE は更新不要

    // ── 7. CDE の CRC-32 と comp_size を更新 ────────────────
    // delta 分だけ CDE のオフセットが変化する
    const size_t new_cde_offset = static_cast<size_t>(
        static_cast<int64_t>(cde_offset) + delta);
    // CDE: CRC-32 は +16, comp_size は ZIP_CDE_COMPRESSED_SIZE (+20)
    write_le32(buf.data() + new_cde_offset + 16,                    new_crc);
    write_le64(buf.data() + new_cde_offset + ZIP_CDE_COMPRESSED_SIZE, new_comp_size);

    // ── 8. EOCD の CD オフセットを更新 ────────────────────────
    const size_t new_eocd_offset = static_cast<size_t>(
        static_cast<int64_t>(eocd_offset) + delta);
    write_le32(buf.data() + new_eocd_offset + ZIP_EOCD_CD_OFFSET,
               static_cast<uint32_t>(new_cde_offset));

    notify("  KD パッチ完了 (圧縮サイズ変化: "
           + std::string(delta >= 0 ? "+" : "") + std::to_string(delta) + " bytes)", 85);
    return ConvertResult::SUCCESS;
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

    // 5d. EOCD後の弥生独自メタデータ内の ".KD12" を ".KD26" に更新
    //     末尾736バイト領域にはエントリ名が複数箇所コピーされており、
    //     ここに ".KD12" が残っていると復元後の読み込みで失敗する。
    //     "12" を含む他のデータ (日付 "12/31" 等) を誤って書き換えないよう
    //     ".KD12" (2E 4B 44 31 32) のみを対象にする。
    {
        static constexpr uint8_t kd12[] = {0x2E, 0x4B, 0x44, 0x31, 0x32};  // ".KD12"
        static constexpr uint8_t kd26[] = {0x2E, 0x4B, 0x44, 0x32, 0x36};  // ".KD26"
        size_t trailing_start = eocd_offset + ZIP_EOCD_FIXED_SIZE + zip_comment_len;
        for (size_t i = trailing_start; i + 5 <= buf.size(); ++i) {
            if (std::memcmp(buf.data() + i, kd12, 5) == 0) {
                std::memcpy(buf.data() + i, kd26, 5);
                any_patched = true;
                notify("  後付けメタデータ内バージョンを更新 (offset=" + std::to_string(i) + ")", -1);
            }
        }
    }

    if (!any_patched) {
        // バージョン文字列が見つからなかった場合
        // 圧縮データ内に格納されている可能性があるため調査が必要
        return ConvertResult::ERR_UNSUPPORTED_FEATURE;
    }

    // ── Step 5e: KD データ内の MDB (SystemInfo.DataVersion) を更新 ──
    // KB ファイルの唯一のエントリ (KD ファイル) を展開・パッチ・再圧縮する。
    notify("KD データベース内バージョン情報を更新しています...", 60);
    {
        // KD エントリを探す: 名前が ".KD12" または ".KD26" で終わるもの
        for (const auto& e : entries) {
            const std::string& name = e.name;
            // ".KD12" → ".KD26" は既に Step 5a でパッチ済みなので ".KD26" か ".KD12" を確認
            bool is_kd = (name.size() >= 5 &&
                          (name.substr(name.size() - 5) == ".KD26" ||
                           name.substr(name.size() - 5) == ".KD12"));
            if (!is_kd) continue;

            const size_t lfh = e.local_offset;
            if (lfh + ZIP_LFH_FIXED_SIZE > buf.size()) continue;

            const uint64_t entry_comp_size   = read_le64(buf.data() + lfh + ZIP_LFH_COMPRESSED_SIZE);
            const uint64_t entry_uncomp_size = read_le64(buf.data() + lfh + ZIP_LFH_UNCOMPRESSED_SIZE);
            const uint16_t lfh_name_len      = read_le16(buf.data() + lfh + ZIP_LFH_FILENAME_LEN);
            const uint16_t lfh_extra_len     = read_le16(buf.data() + lfh + ZIP_LFH_EXTRA_LEN);
            const size_t   data_off          = lfh + ZIP_LFH_FIXED_SIZE + lfh_name_len + lfh_extra_len;

            // 圧縮サイズの妥当性確認
            if (entry_uncomp_size == 0 || entry_comp_size == 0) continue;
            if (data_off + entry_comp_size > buf.size()) continue;
            // KD ファイルは Jet 4.0 MDB (4096 バイトページ); 展開サイズがその倍数か確認
            if (entry_uncomp_size % 4096 != 0) continue;

            notify("  KD エントリ: \"" + name + "\""
                   " comp=" + std::to_string(entry_comp_size)
                   + " uncomp=" + std::to_string(entry_uncomp_size), -1);

            const ConvertResult kd_result = patch_kd_entry(
                buf,
                lfh, data_off,
                entry_comp_size, entry_uncomp_size,
                e.cd_offset, eocd_offset,
                notify);

            if (kd_result != ConvertResult::SUCCESS) {
                notify("  [警告] KD データのパッチに失敗しました (コード="
                       + std::string(convert_result_to_string(kd_result)) + ")", -1);
                // 失敗してもコンテナ変換は成功とする
            }
            break; // KD エントリは1つのみ
        }
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
