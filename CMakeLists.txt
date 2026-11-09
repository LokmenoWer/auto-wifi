cmake_minimum_required(VERSION 3.16)
project(AutoWiFi VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/main.cpp
    src/gui.cpp
    src/config.cpp
    src/wifi_manager.cpp
    src/portal_auth.cpp
    src/utils.cpp
)

if(WIN32)
    enable_language(RC)
    list(APPEND SOURCES res/app.rc)
    add_compile_definitions(UNICODE _UNICODE WINVER=0x0601 _WIN32_WINNT=0x0601)
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
else()
    if(APPLE)
        enable_language(OBJCXX)
        set(CMAKE_OBJCXX_STANDARD 17)
        set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
        list(APPEND SOURCES src/wifi_macos.mm src/gui_macos.mm)
        set_source_files_properties(src/wifi_macos.mm src/gui_macos.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()

    add_executable(auto-wifi ${SOURCES})
    target_include_directories(auto-wifi PRIVATE src res)
    target_compile_options(auto-wifi PRIVATE -Wall -Wextra -O2)

    find_package(Threads REQUIRED)
    target_link_libraries(auto-wifi PRIVATE Threads::Threads)

    if(APPLE)
        target_link_libraries(auto-wifi PRIVATE
            "-framework Foundation"
            "-framework CoreWLAN"
            "-framework CoreLocation"
            "-framework AppKit"
        )
        target_link_options(auto-wifi PRIVATE
            "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_SOURCE_DIR}/res/Info.plist"
        )
    endif()

    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_compile_definitions(auto-wifi PRIVATE USE_LIBCURL=1)
        target_link_libraries(auto-wifi PRIVATE CURL::libcurl)
    endif()
endif()

