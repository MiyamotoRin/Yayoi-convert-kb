# Yayoi-convert-kb

弥生会計のバックアップファイル (`.KB12`) を、弥生会計26で読み込める形式 (`.KB26`) に変換するツールです。

## 必要環境

| ツール | バージョン | インストール |
|---|---|---|
| C++ コンパイラ | C++17 以上 (GCC 8+, Clang 7+, MSVC 2017+) | OS 付属またはパッケージマネージャ経由 |
| CMake | 3.15 以上 | https://cmake.org/download/ |
| Make | 任意 (CMake の代替) | OS 付属 |

## ビルド方法

### CMake を使う場合 (推奨)

#### Linux / macOS

```bash
cmake -B build
cmake --build build
```

#### Windows — Visual Studio を使う場合

Visual Studio 2017 以降と「C++ によるデスクトップ開発」ワークロードがインストールされている場合:

```powershell
cmake -B build
cmake --build build --config Release
```

#### Windows — MinGW (MSYS2) を使う場合

[MSYS2](https://www.msys2.org/) をインストールし、MSYS2 MinGW 64-bit シェルで必要なツールを導入してください:

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-cmake
```

その後、同じシェルで:

```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

> **Visual Studio も MinGW も入っていない場合**
> CMake は既定のジェネレータとして NMake を試みますが、Visual Studio Build Tools がないと
> `'nmake' '-?' failed with: no such file or directory` というエラーになります。
> 上記いずれかの C++ コンパイラをインストールしてから再実行してください。

生成された実行ファイルは `build/kb_converter` (Windows では `build/Release/kb_converter.exe` または `build/kb_converter.exe`) です。

### make を使う場合

```bash
make
```

生成された実行ファイルは `./kb_converter` です。

## 使い方

```bash
# 出力ファイル名を自動決定 (入力と同じディレクトリに .KB26 で生成)
./kb_converter 会社データ.KB12

# 出力ファイル名を明示指定
./kb_converter 会社データ.KB12 会社データ.KB26
```

## CMake のインストール方法

### Windows

[cmake.org/download/](https://cmake.org/download/) から `.msi` インストーラをダウンロードして実行してください。

CMake だけでなく **C++ コンパイラ** も必要です。以下のいずれかをインストールしてください:

- **Visual Studio 2017 以降**: インストーラで「C++ によるデスクトップ開発」ワークロードを選択
  - Build Tools のみでよい場合は [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/) (無償) をインストール
- **MinGW-w64**: [MSYS2](https://www.msys2.org/) 経由でインストール
  ```bash
  pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-cmake
  ```

### macOS

```bash
brew install cmake
```

[Homebrew](https://brew.sh/) がインストールされていない場合は、上記リンクから先にインストールしてください。

### Linux (Debian / Ubuntu)

```bash
sudo apt install cmake
```

### Linux (Fedora / RHEL)

```bash
sudo dnf install cmake
```

最新版が必要な場合は [cmake.org/download/](https://cmake.org/download/) からバイナリを取得するか、[pip](https://pypi.org/project/cmake/) 経由でインストールできます。

```bash
pip install cmake
```
