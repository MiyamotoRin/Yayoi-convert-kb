/**
 * parse_sysinfo.cpp
 * conv_kd.bin (または任意の KD MDB ファイル) から
 * .NET BinaryFormatter でシリアライズされた ZsSystemInfo.Data ブロブを探し、
 * dataVersion フィールドの値を表示する。
 *
 * BinaryFormatter バイナリ形式 (概略):
 *   [0x00] BinaryFormatterVersion (1 byte = 0)
 *   [0x01] major version (4 bytes LE = 1)
 *   ... SerializationHeaderRecord ...
 *   ObjectWithMapTyped / ClassWithMembersAndTypes レコード
 *     → フィールド名リスト → フィールド型リスト → フィールド値リスト (同順)
 *
 * ここでは厳密なパーサーではなく、シグネチャ検索ベースで dataVersion 値を取得する:
 *   1. "<dataVersion>k__BackingField" という文字列を UTF-8 で検索
 *   2. その後ろにある BinaryFormatter の値セクションを読む
 *      (Int32 の場合: RecordTypeEnum=0x08, IntValue=4 bytes LE)
 *      (String の場合: RecordTypeEnum=0x06, LengthPrefixedString)
 */

#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>
#include <string>
#include <algorithm>
#ifdef _WIN32
#  include <windows.h>
static std::wstring utf8_to_wstring(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

static std::vector<uint8_t> read_file(const char* path) {
#ifdef _WIN32
    FILE* f = _wfopen(utf8_to_wstring(path).c_str(), L"rb");
#else
    FILE* f = fopen(path, "rb");
#endif
    if (!f) { perror(path); return {}; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz);
    fread(buf.data(), 1, sz, f);
    fclose(f);
    return buf;
}

// BinaryFormatter の LengthPrefixedString を読む
// 先頭は可変長整数 (7-bit encoded, little-endian)
// オフセットから始まるバイト列を hex + ASCII で表示
static void hex_dump(const uint8_t* p, size_t len, size_t base_offset) {
    for (size_t i = 0; i < len; i += 16) {
        printf("  %08zx: ", base_offset + i);
        for (size_t j = 0; j < 16; j++) {
            if (i+j < len) printf("%02X ", p[i+j]); else printf("   ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i+j < len; j++) {
            uint8_t c = p[i+j];
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: parse_sysinfo <conv_kd.bin> [ref_kd.bin]\n");
        return 1;
    }

    for (int fi = 1; fi < argc; fi++) {
        auto buf = read_file(argv[fi]);
        if (buf.empty()) { fprintf(stderr, "Cannot read: %s\n", argv[fi]); continue; }

        printf("\n===== %s (%zu bytes, %zu pages) =====\n",
               argv[fi], buf.size(), buf.size() / 4096);

        // --- 1. "<dataVersion>k__BackingField" を検索 (UTF-8) ---
        const char* DVER_FIELD = "<dataVersion>k__BackingField";

        // --- 2. "DataVersion" / "dataVersion" / "DataFormatVersion" を検索 (UTF-8) ---
        const char* patterns[] = {
            "<dataVersion>k__BackingField",
            "DataVersion",
            "dataVersion",
            "DataFormatVersion",
            "Version=",
            "(22.", "(32.", "(10.", "(12.", "(26.",
            nullptr
        };

        for (const char** pp = patterns; *pp; pp++) {
            const char* pat = *pp;
            size_t plen = strlen(pat);
            int count = 0;
            for (size_t i = 0; i + plen <= buf.size() && count < 5; i++) {
                if (memcmp(buf.data() + i, pat, plen) == 0) {
                    count++;
                    size_t page = i / 4096;
                    printf("\n[%s] offset=%zu (page=%zu)\n", pat, i, page);

                    // 前後80バイトを hex dump
                    size_t start = (i > 40) ? i - 40 : 0;
                    size_t end   = std::min(i + plen + 80, buf.size());
                    hex_dump(buf.data() + start, end - start, start);

                    // もし "<dataVersion>k__BackingField" なら、値を解析してみる
                    // BinaryFormatter では:
                    //   フィールド名セクションの後ろに型情報・値が並ぶ
                    // 簡易的に: フィールド名の後ろ 200 バイトに注目して整数・文字列を探す
                    if (strcmp(pat, DVER_FIELD) == 0) {
                        printf("  *** dataVersion フィールドを発見 ***\n");
                        printf("  後続 200 バイト:\n");
                        size_t look_start = i + plen;
                        size_t look_end   = std::min(look_start + 200, buf.size());
                        hex_dump(buf.data() + look_start, look_end - look_start, look_start);

                        // BinaryFormatter Int32 レコード: 0x0B = MemberPrimitiveUnTyped
                        // または 0x06 = ObjectNull, 0x09 = MemberReference etc.
                        // フィールド値は型情報に依存; Int32 なら 4バイト LE
                        // まず後続バイトを ASCII + 数値でざっくり表示
                        for (size_t j = look_start; j < look_end - 3; j++) {
                            // Int32 候補 (0〜100 の小さな値)
                            uint32_t v = (uint32_t)buf[j]
                                       | ((uint32_t)buf[j+1] << 8)
                                       | ((uint32_t)buf[j+2] << 16)
                                       | ((uint32_t)buf[j+3] << 24);
                            if (v == 12 || v == 26 || v == 22 || v == 32) {
                                printf("  ** 候補値 Int32=%u at offset=%zu\n", v, j);
                            }
                        }
                    }
                }
            }
            if (count == 0) {
                printf("[%s] → 見つからず\n", pat);
            }
        }

        // --- 3. ZsSystemInfo のデータページを探す ---
        // MDB page owner フィールド (page offset 4) と row count (page offset 12) で識別
        // page type 0x01 = data page
        printf("\n--- MDB データページサマリー (type=0x01, rows>=1) ---\n");
        int data_pages = 0;
        for (size_t pg = 0; (pg + 1) * 4096 <= buf.size() && data_pages < 20; pg++) {
            const uint8_t* p = buf.data() + pg * 4096;
            if (p[0] != 0x01) continue; // data page
            uint16_t row_count = (uint16_t)(p[12] | (p[13] << 8));
            if (row_count == 0 || row_count > 200) continue;
            uint32_t owner = (uint32_t)(p[4] | (p[5]<<8) | (p[6]<<16) | (p[7]<<24));
            if (owner == 0) continue;
            // 行オフセット取得: page+14 以降に row_count 個の uint16_t
            printf("  page=%zu owner=%u rows=%u\n", pg, owner, row_count);
            data_pages++;
        }
    }

    return 0;
}
