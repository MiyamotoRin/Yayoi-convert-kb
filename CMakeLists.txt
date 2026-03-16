cmake_minimum_required(VERSION 3.15)
project(kb_converter
    VERSION 1.0.0
    DESCRIPTION "弥生会計バックアップ KB12→KB26 コンバータ"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ────────────────────────────────────────────
# コンパイルオプション
# ────────────────────────────────────────────
if(MSVC)
    add_compile_options(
        /W4
        /WX
        /utf-8
        /wd4100   # 未使用引数は警告しない
    )
else()
    add_compile_options(
        -Wall
        -Wextra
        -Werror
        -Wpedantic
        -Wno-unused-parameter
    )
endif()

# ────────────────────────────────────────────
# ターゲット
# ────────────────────────────────────────────
add_executable(kb_converter
    src/main.cpp
    src/kb_converter.cpp
)

target_include_directories(kb_converter PRIVATE src)
target_link_libraries(kb_converter PRIVATE z)

add_executable(find_version
    src/find_version.cpp
)

add_executable(parse_sysinfo
    src/parse_sysinfo.cpp
)

# C++17 の std::filesystem リンク (GCC < 9 では明示的に指定が必要)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(kb_converter PRIVATE stdc++fs)
endif()

# ────────────────────────────────────────────
# インストール
# ────────────────────────────────────────────
install(TARGETS kb_converter RUNTIME DESTINATION bin)
