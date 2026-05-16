cmake_minimum_required(VERSION 3.26)

project(warc-studio
    VERSION 0.1.0
    DESCRIPTION "Small C++23 archive studio for Browsertrix, WACZ files, and ReplayWeb.page"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Crow CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_executable(warc-studio
    src/main.cpp
    src/BrowsertrixService.cpp
    src/Database.cpp
    src/FileService.cpp
    src/Html.cpp
    src/HttpUtil.cpp
)

target_include_directories(warc-studio
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(warc-studio
    PRIVATE
        Crow::Crow
        SQLite::SQLite3
        Threads::Threads
)

if(MSVC)
    target_compile_options(warc-studio PRIVATE /W4 /permissive-)
else()
    target_compile_options(warc-studio PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS warc-studio RUNTIME DESTINATION bin)
