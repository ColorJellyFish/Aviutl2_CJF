cmake_minimum_required(VERSION 3.24)

project(Aviutl2_CJF LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "AviUtl2 plugins are Windows-only.")
endif()

option(CJF_BUILD_PREVIEW_MOUSE_PROBE "Build the Phase 1 preview mouse diagnostic plugin" ON)
option(CJF_BUILD_PREVIEW_RANGE_SELECTOR "Build the Phase 2 preview range selector plugin" ON)
option(CJF_BUILD_PREVIEW_PEN_TOOL "Build the Phase 6 pen tool plugin" ON)

# MouseProbe はデバッグ用プラグインのため、公開リポジトリにはソースを含めない。
# ソースが存在する場合（＝開発リポジトリ）のみビルドする。
if(CJF_BUILD_PREVIEW_MOUSE_PROBE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/preview_range_cjf/PreviewMouseProbe.cpp)
    add_library(CJFPreviewMouseProbe SHARED
        src/preview_range_cjf/PreviewMouseProbe.cpp
    )

    target_include_directories(CJFPreviewMouseProbe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/aviutl2_sdk
    )

    target_compile_features(CJFPreviewMouseProbe PRIVATE cxx_std_17)

    # SDK/plugin sources contain UTF-8 Japanese strings.
    if(MSVC)
        target_compile_options(CJFPreviewMouseProbe PRIVATE /utf-8)
        # The SDK headers themselves are Shift-JIS; suppress the resulting
        # C4828 (charset) warnings so the build log stays clean.
        target_compile_options(CJFPreviewMouseProbe PRIVATE /wd4828)
    endif()

    target_link_libraries(CJFPreviewMouseProbe PRIVATE user32)

    # AviUtl2 loads generic plugins with the .aux2 extension.
    set_target_properties(CJFPreviewMouseProbe PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CJFPreviewMouseProbe"
        SUFFIX ".aux2"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()

if(CJF_BUILD_PREVIEW_RANGE_SELECTOR)
    add_library(CJFPreviewRangeSelector SHARED
        src/preview_range_cjf/PreviewRangeSelector.cpp
    )

    target_include_directories(CJFPreviewRangeSelector PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/aviutl2_sdk
    )

    target_compile_features(CJFPreviewRangeSelector PRIVATE cxx_std_17)

    # SDK/plugin sources contain UTF-8 Japanese strings.
    if(MSVC)
        target_compile_options(CJFPreviewRangeSelector PRIVATE /utf-8)
        # The SDK headers themselves are Shift-JIS; suppress the resulting
        # C4828 (charset) warnings so the build log stays clean.
        target_compile_options(CJFPreviewRangeSelector PRIVATE /wd4828)
    endif()

    target_link_libraries(CJFPreviewRangeSelector PRIVATE user32 gdi32)

    set_target_properties(CJFPreviewRangeSelector PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CJFPreviewRangeSelector"
        SUFFIX ".aux2"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()

if(CJF_BUILD_PREVIEW_PEN_TOOL)
    add_library(CJFPreviewPenTool SHARED
        src/preview_range_cjf/PreviewPenTool.cpp
    )

    target_include_directories(CJFPreviewPenTool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/aviutl2_sdk
    )

    target_compile_features(CJFPreviewPenTool PRIVATE cxx_std_17)

    # SDK/plugin sources contain UTF-8 Japanese strings.
    if(MSVC)
        target_compile_options(CJFPreviewPenTool PRIVATE /utf-8)
        # The SDK headers themselves are Shift-JIS; suppress the resulting
        # C4828 (charset) warnings so the build log stays clean.
        target_compile_options(CJFPreviewPenTool PRIVATE /wd4828)
    endif()

    target_link_libraries(CJFPreviewPenTool PRIVATE user32 gdi32)

    set_target_properties(CJFPreviewPenTool PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CJFPreviewPenTool"
        SUFFIX ".aux2"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/aviutl2/Plugin/CJF"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif()
