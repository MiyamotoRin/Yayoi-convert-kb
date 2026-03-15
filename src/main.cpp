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
#ifdef _WIN32
#  include <windows.h>
#endif

#ifdef _WIN32
/** UTF-16 wstring を UTF-8 std::string に変換する */
static std::string wstring_to_utf8(const wchar_t* ws) {
    if (!ws || ws[0] == L'\0') return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &result[0], len, nullptr, nullptr);
    return result;
}
#endif

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
 * 入力パスの拡張子を .KB26 に変えた出力パスを返す。
 * std::filesystem を一切使わず純粋な文字列操作で行う。
 * (MinGW の std::filesystem はロケール変換時に日本語パスで例外を投げるため)
 */
static std::string make_output_path(const std::string& input_path) {
    size_t sep = input_path.find_last_of("/\\");
    size_t dot = input_path.rfind('.');
    if (dot != std::string::npos &&
        (sep == std::string::npos || dot > sep)) {
        return input_path.substr(0, dot) + ".KB26";
    }
    return input_path + ".KB26";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);

    // main(char* argv[]) は CP932 (ANSI) で引数を受け取るため、
    // GetCommandLineW + CommandLineToArgvW で UTF-16 引数を直接取得する。
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc < 2) {
        if (wargv) LocalFree(wargv);
        print_usage(argv[0]);
        return 1;
    }
    std::string input_path  = wstring_to_utf8(wargv[1]);
    std::string output_path = (wargc >= 3) ? wstring_to_utf8(wargv[2])
                                           : make_output_path(input_path);
    LocalFree(wargv);
#else
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    std::string input_path  = argv[1];
    std::string output_path = (argc >= 3) ? argv[2] : make_output_path(input_path);
#endif

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
