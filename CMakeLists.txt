cmake_minimum_required(VERSION 3.16)
project(AutoWiFi VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE WINVER=0x0601 _WIN32_WINNT=0x0601)

set(SOURCES
    src/main.cpp
    src/gui.cpp
    src/config.cpp
    src/wifi_manager.cpp
    src/portal_auth.cpp
    src/utils.cpp
    res/app.rc
)

add_executable(auto-wifi WIN32 ${SOURCES})

target_include_directories(auto-wifi PRIVATE src res)

if(MINGW)
    target_compile_options(auto-wifi PRIVATE -Wall -Wextra -O2 -municode)
    target_link_options(auto-wifi PRIVATE -municode -mwindows -static -static-libgcc -static-libstdc++ -s)
endif()

target_link_libraries(auto-wifi PRIVATE
    wlanapi
    winhttp
    crypt32
    comctl32
    shell32
    ole32
    advapi32
    gdi32
    user32
)
