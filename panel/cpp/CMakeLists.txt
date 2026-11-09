cmake_minimum_required(VERSION 3.21)
project(KierownicaPanel LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Panel buduje sie tylko jako x64 (vJoy i ViGEm w tej samej architekturze).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Statyczny runtime - pojedynczy .exe bez Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin")

add_library(wheelcore STATIC
    src/util.cpp
    src/json.cpp
    src/protocol.cpp
    src/config.cpp
    src/processing.cpp
    src/ffb.cpp
    src/link.cpp
    src/engine.cpp
    src/winsys.cpp
    src/outputs/vjoy.cpp
    src/outputs/xinput.cpp
)
target_include_directories(wheelcore PUBLIC src)
target_compile_definitions(wheelcore PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(wheelcore PUBLIC /utf-8 /W4 /permissive- /EHsc)
target_link_libraries(wheelcore PUBLIC setupapi advapi32 shell32 ole32 dwmapi user32)

add_executable(Kierownica WIN32
    src/main.cpp
    src/ui/gfx.cpp
    src/ui/widgets.cpp
    src/ui/padview.cpp
    src/ui/app.cpp
)
target_link_libraries(Kierownica PRIVATE wheelcore d2d1 dwrite windowscodecs winmm gdi32)
# Common Controls 6 - okna komunikatow w stylu systemu.
target_link_options(Kierownica PRIVATE "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'")
set_target_properties(Kierownica PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BIN_DIR}")
# ViGEmClient.dll musi lezec obok programu (vigem/x64).
add_custom_command(TARGET Kierownica POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../python/vigem" "${BIN_DIR}/vigem")

add_executable(core_tests tests/test_core.cpp)
target_link_libraries(core_tests PRIVATE wheelcore)
set_target_properties(core_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BIN_DIR}/tests")
