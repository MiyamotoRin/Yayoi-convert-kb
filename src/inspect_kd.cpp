/**
 * inspect_kd.cpp
 * KB26 ファイルから KD データを展開してバージョン文字列を検索する
 */
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#ifdef _WIN32
#  include <windows.h>
#endif
#include <zlib.h>

#ifdef _WIN32
static std::wstring utf8_to_wstring(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

static std::vector<uint8_t> read_file(const std::string& path) {
#ifdef _WIN32
    FILE* f = _wfopen(utf8_to_wstring(path).c_str(), L"rb");
#else
    FILE* f = fopen(path.c_str(), "rb");
#endif
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz);
    fread(buf.data(), 1, sz, f);
    fclose(f);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: inspect_kd <file.KB26>\n"); return 1; }

    auto buf = read_file(argv[1]);
    if (buf.empty()) { fprintf(stderr, "Cannot read file\n"); return 1; }

    // LFH: 固定38バイト (弥生形式: comp_size/uncomp_size = int64)
    //   +18: comp_size (8バイト)
    //   +34: name_len (2バイト)
    //   +36: extra_len (2バイト)
    auto r16 = [&](size_t off){ return (uint16_t)(buf[off] | buf[off+1]<<8); };
    auto r64 = [&](size_t off){
        uint64_t v = 0;
        for (int i=0;i<8;i++) v |= (uint64_t)buf[off+i] << (8*i);
        return v;
    };

    uint64_t comp_size = r64(18);
    uint16_t name_len  = r16(34);
    uint16_t extra_len = r16(36);
    size_t   data_off  = 38 + name_len + extra_len;

    // エントリ名 (Shift-JIS)
    printf("comp_size=%llu name_len=%u extra_len=%u data_off=%zu\n",
           (unsigned long long)comp_size, name_len, extra_len, data_off);

    // ファイル名をバイト列で表示 (文字コード確認)
    printf("filename hex: ");
    for (uint16_t i = 0; i < name_len && 38+i < buf.size(); i++)
        printf("%02X ", buf[38+i]);
    printf("\n");

    if (data_off + comp_size > buf.size()) {
        fprintf(stderr, "data out of range\n"); return 1;
    }

    // zlib raw deflate 展開
    uLongf uncomp_len = (uLongf)r64(26);  // uncompressed size from LFH
    if (uncomp_len == 0 || uncomp_len > 512*1024*1024UL) {
        uncomp_len = comp_size * 10;  // 推定: 圧縮率 10% 想定の上限
    }
    printf("uncomp_size (from header)=%llu\n", (unsigned long long)r64(26));

    std::vector<uint8_t> out(uncomp_len);
    uLong src_len = (uLong)comp_size;
    int ret = uncompress2(out.data(), &uncomp_len,
                          buf.data() + data_off, &src_len);
    if (ret != Z_OK) {
        // raw deflate で試みる
        z_stream zs{}; zs.next_in = buf.data() + data_off; zs.avail_in = (uInt)comp_size;
        zs.next_out = out.data(); zs.avail_out = (uInt)out.size();
        inflateInit2(&zs, -15);
        ret = inflate(&zs, Z_FINISH);
        uncomp_len = zs.total_out;
        inflateEnd(&zs);
        if (ret != Z_STREAM_END && ret != Z_OK) {
            fprintf(stderr, "inflate error: %d\n", ret); return 1;
        }
    }
    out.resize(uncomp_len);
    printf("展開完了: %llu バイト\n", (unsigned long long)uncomp_len);

    // 展開したデータをファイルに書き出す (argc >= 3 の場合)
    if (argc >= 3) {
#ifdef _WIN32
        FILE* fout = _wfopen(utf8_to_wstring(argv[2]).c_str(), L"wb");
#else
        FILE* fout = fopen(argv[2], "wb");
#endif
        if (fout) {
            fwrite(out.data(), 1, out.size(), fout);
            fclose(fout);
            printf("KD データを書き出しました: %s\n", argv[2]);
        }
    }

    // 先頭16バイトを hex 表示
    printf("先頭 16 バイト: ");
    for (size_t i = 0; i < std::min<size_t>(16, out.size()); i++)
        printf("%02X ", out[i]);
    printf("\n");

    // ASCII 文字列検索: バージョン関連キーワード
    // "KB12", "KB26", "DataVersion", "FileVersion", "KBVER" など
    const char* keywords[] = {
        "KB12", "KB26", "KD12", "KD26",
        "DataVersion", "FileVersion", "AppVersion",
        "DataFormatVersion", "KBVER", "KDVER",
        "Version=12", "Version=26",
        nullptr
    };

    std::string text(out.begin(), out.end());
    printf("\n=== バージョン文字列検索 ===\n");
    for (const char** kw = keywords; *kw; kw++) {
        size_t pos = 0;
        int count = 0;
        while ((pos = text.find(*kw, pos)) != std::string::npos && count < 3) {
            // 周辺コンテキスト表示
            size_t start = (pos > 20) ? pos-20 : 0;
            size_t end   = std::min(pos+40, text.size());
            printf("  [%s] offset=%zu context: ", *kw, pos);
            for (size_t i = start; i < end; i++) {
                char c = text[i];
                printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
            }
            printf("\n");
            pos++;
            count++;
        }
    }

    return 0;
}
