cmake_minimum_required(VERSION 3.28)
project(launcher C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(launcher WIN32
    src/app/win_main.c
    src/core/base.c
    src/render/dx11_renderer.c
    src/search/fuzzy.c
    src/text/font_raster.c
    src/text/kb_text.c
    src/ui/ui.c
    src/ui/ctx_menu_icons.c
    src/platform/catalog.c
    src/platform/catalog_watch.c
    src/platform/catalog_aliases.c
    src/platform/everything_client.c
    src/platform/icon_worker.c
    src/platform/launch.c
    src/platform/start_menu_scan.c
    src/platform/system32_catalog.c
    src/platform/shell_display_name.c
    third_party/fzy/match.c
    third_party/everything_sdk/sdk/src/Everything.c
)

target_include_directories(launcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanosvg
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fzy
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/everything_sdk/sdk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/everything_sdk/sdk/ipc
)

target_compile_definitions(launcher PRIVATE
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

add_custom_command(TARGET launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data"
        "$<TARGET_FILE_DIR:launcher>/data"
    COMMENT "Copying data/ (SVG icons, etc.) next to launcher.exe")

target_link_libraries(launcher PRIVATE
    d3d11
    dxgi
    dxguid
    d3dcompiler
    shell32
    shlwapi
    ole32
    propsys
)
