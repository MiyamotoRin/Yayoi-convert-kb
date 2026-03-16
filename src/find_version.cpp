#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>
#include <string>

static std::vector<uint8_t> read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return {}; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz);
    fread(buf.data(), 1, sz, f);
    fclose(f);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: find_version <file>\n"); return 1; }
    auto buf = read_file(argv[1]);
    if (buf.empty()) return 1;

    // Search for UTF-16LE "DataVersion"
    // D a t a V e r s i o n
    uint8_t pattern[] = {0x44,0x00,0x61,0x00,0x74,0x00,0x61,0x00,
                         0x56,0x00,0x65,0x00,0x72,0x00,0x73,0x00,
                         0x69,0x00,0x6f,0x00,0x6e,0x00};
    size_t plen = sizeof(pattern);

    printf("File: %s (%zu bytes)\n", argv[1], buf.size());
    int found = 0;
    for (size_t i = 0; i + plen <= buf.size(); i++) {
        if (memcmp(buf.data() + i, pattern, plen) == 0) {
            size_t page = i / 4096;
            size_t off_in_page = i % 4096;
            printf("\n[%d] offset=%zu (page=%zu, off_in_page=%zu)\n", ++found, i, page, off_in_page);
            // Print 80 bytes before and 80 bytes after as hex + ASCII
            size_t start = (i > 80) ? i - 80 : 0;
            size_t end = std::min(i + plen + 80, buf.size());
            for (size_t j = start; j < end; j++) {
                if (j == i) printf(" [[[");
                if (j == i + plen) printf("]]]");
                printf("%02X ", buf[j]);
            }
            printf("\n");
            // Also print ASCII interpretation
            printf("ASCII: ");
            for (size_t j = start; j < end; j++) {
                uint8_t c = buf[j];
                printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
            }
            printf("\n");
        }
    }
    printf("\nTotal occurrences: %d\n", found);
    return 0;
}
