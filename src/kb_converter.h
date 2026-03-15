/**
 * kb_converter.h
 * 弥生会計バックアップファイル KB12→KB26 コンバータ インターフェース
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

/**
 * 変換結果コード
 */
enum class ConvertResult {
    SUCCESS = 0,
    ERR_INPUT_NOT_FOUND,       // 入力ファイルが見つからない
    ERR_INPUT_READ_FAILED,     // 入力ファイルの読み込み失敗
    ERR_OUTPUT_WRITE_FAILED,   // 出力ファイルの書き込み失敗
    ERR_INVALID_MAGIC,         // マジックバイトが不正 (Yayoiファイルでない)
    ERR_INVALID_VERSION,       // バージョンが変換対象でない
    ERR_INVALID_HEADER,        // ヘッダが破損している
    ERR_CHECKSUM_MISMATCH,     // チェックサム不一致
    ERR_UNSUPPORTED_FEATURE,   // 非対応機能 (暗号化など)
    ERR_TRUNCATED_FILE,        // ファイルが切り捨てられている
};

/**
 * 変換結果をテキストで返す
 */
const char* convert_result_to_string(ConvertResult result);

/**
 * 進捗コールバック型
 * @param message  状態メッセージ
 * @param percent  進捗 0〜100 (-1 は不定)
 */
using ProgressCallback = std::function<void(const std::string& message, int percent)>;

/**
 * KB12ファイルをKB26ファイルに変換する
 *
 * @param input_path   変換元 .KB12 ファイルパス
 * @param output_path  変換先 .KB26 ファイルパス
 * @param progress     進捗コールバック (nullptr可)
 * @return ConvertResult
 */
ConvertResult convert_kb12_to_kb26(
    const std::string& input_path,
    const std::string& output_path,
    ProgressCallback progress = nullptr
);

/**
 * ファイルが有効なKB12ファイルかを検証する
 *
 * @param file_path  検証対象ファイルパス
 * @param error_msg  エラーメッセージ出力用 (nullptr可)
 * @return true: 有効なKB12ファイル
 */
bool validate_kb12_file(const std::string& file_path, std::string* error_msg = nullptr);

/**
 * KBファイルのバージョンを取得する
 *
 * @param file_path  対象ファイルパス
 * @return バージョン番号 (取得失敗時は0)
 */
uint16_t get_kb_file_version(const std::string& file_path);

/**
 * ファイルの内部構造を診断して標準出力に詳細を表示する。
 * 問題の原因調査用。先頭/末尾のバイト列、シグネチャ出現位置、
 * EOCD候補の検証結果などを出力する。
 *
 * @param file_path  診断対象ファイルパス
 */
void diagnose_kb_file(const std::string& file_path);
