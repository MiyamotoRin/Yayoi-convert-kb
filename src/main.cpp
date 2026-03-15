/**
 * main.cpp
 * 弥生会計バックアップファイル KB12→KB26 コンバータ エントリポイント
 *
 * 使い方:
 *   kb_converter <input.KB12> [output.KB26]
 *
 *   output を省略した場合は input と同じディレクトリに
 *   拡張子を .KB26 に変えたファイルを生成します。
 */

#include "kb_converter.h"

#include <iostream>
#include <string>
#include <filesystem>
#ifdef _WIN32
#  include <windows.h>
#endif

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    std::cerr
        << "弥生会計バックアップ KB12→KB26 コンバータ\n"
        << "\n"
        << "使い方:\n"
        << "  " << prog << " <input.KB12> [output.KB26]\n"
        << "\n"
        << "引数:\n"
        << "  input.KB12   変換元バックアップファイル\n"
        << "  output.KB26  変換先ファイル (省略時: 入力ファイルと同ディレクトリに .KB26 で生成)\n"
        << "\n"
        << "例:\n"
        << "  " << prog << " 会社データ.KB12\n"
        << "  " << prog << " 会社データ.KB12 会社データ.KB26\n";
}

/**
 * 入力パスの拡張子を .KB26 に変えた出力パスを返す
 */
static std::string make_output_path(const std::string& input_path) {
    fs::path p(input_path);
    // 拡張子を大文字小文字問わず置換
    p.replace_extension(".KB26");
    return p.string();
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string input_path  = argv[1];
    std::string output_path = (argc >= 3) ? argv[2] : make_output_path(input_path);

    std::cout << "入力ファイル : " << input_path  << "\n"
              << "出力ファイル : " << output_path << "\n"
              << "\n";

    // ── 事前検証 ────────────────────────────────────────────
    std::cout << "[1/2] 入力ファイルを検証しています..." << std::flush;
    std::string validate_err;
    if (!validate_kb12_file(input_path, &validate_err)) {
        std::cout << " 失敗\n";
        std::cerr << "エラー: " << validate_err << "\n";
        return 2;
    }
    std::cout << " OK\n";

    // ── 変換実行 ─────────────────────────────────────────────
    std::cout << "[2/2] 変換中...\n";

    auto progress = [](const std::string& msg, int pct) {
        if (pct >= 0) {
            std::cout << "  [" << pct << "%] " << msg << "\n";
        } else {
            std::cout << "  " << msg << "\n";
        }
        std::cout.flush();
    };

    ConvertResult result = convert_kb12_to_kb26(input_path, output_path, progress);

    if (result != ConvertResult::SUCCESS) {
        std::cerr << "\n変換エラー: " << convert_result_to_string(result) << "\n";
        return 3;
    }

    std::cout << "\n変換が完了しました: " << output_path << "\n";
    return 0;
}
