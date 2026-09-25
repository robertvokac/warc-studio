cmake_minimum_required(VERSION 3.26)
project(warc-studio
    VERSION 0.1.0
    DESCRIPTION "Personal web archive: built-in crawler writing timestamped WARC captures, tags and ReplayWeb.page replay"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

include(FetchContent)

# Asio (standalone, no Boost) — Crow needs it
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-30-2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(asio)

# Crow calls find_package(asio REQUIRED) and expects an asio::asio INTERFACE target.
# FetchContent does not create it automatically, so we create it here.
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include" CACHE PATH "" FORCE)
if(NOT TARGET asio::asio)
    add_library(asio::asio INTERFACE IMPORTED)
    set_target_properties(asio::asio PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${asio_SOURCE_DIR}/asio/include"
    )
    target_compile_definitions(asio::asio INTERFACE ASIO_STANDALONE)
endif()

# Crow — lightweight C++ HTTP server
FetchContent_Declare(
    crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.2.0
    GIT_SHALLOW    TRUE
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DCROW_SOURCE_DIR=<SOURCE_DIR>
        -DCROW_PATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/crow-local-fixes.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_crow_patch.cmake
)
# Do not build Crow's own tests/examples
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(crow)

# SQLite3 — use system package (apt install libsqlite3-dev)
find_package(SQLite3 REQUIRED)

# POSIX threads
find_package(Threads REQUIRED)

# OpenSSL — used for SHA-256 checksum computation on archive files
find_package(OpenSSL REQUIRED)

# zlib — gzip for WARC records, decoding compressed HTTP responses, reading .warc.gz uploads
find_package(ZLIB REQUIRED)

# libcurl — HTTP client of the built-in crawler (apt install libcurl4-openssl-dev)
find_package(CURL REQUIRED)

# ---------------------------------------------------------------------------
# Application target
# ---------------------------------------------------------------------------

add_executable(warc-studio
    src/main.cpp
    src/CrawlScope.cpp
    src/CaptureUtil.cpp
    src/Crawler.cpp
    src/CrawlService.cpp
    src/Database.cpp
    src/FileService.cpp
    src/Html.cpp
    src/HtmlLinks.cpp
    src/HttpUtil.cpp
    src/WarcWriter.cpp
)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------

add_executable(warc-studio-tests
    src/CaptureUtil.cpp
    src/CrawlScope.cpp
    tests/test_crawl_scope.cpp
)

add_executable(warc-studio-util-tests
    src/CaptureUtil.cpp
    src/HtmlLinks.cpp
    tests/test_capture_util.cpp
)

add_executable(warc-studio-regression-tests
    src/CaptureUtil.cpp
    src/Database.cpp
    src/Html.cpp
    src/HttpUtil.cpp
    tests/test_regressions.cpp
)
target_link_libraries(warc-studio-regression-tests PRIVATE SQLite::SQLite3)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME crawl-scope COMMAND warc-studio-tests)
    add_test(NAME capture-util COMMAND warc-studio-util-tests)
    add_test(NAME regressions COMMAND warc-studio-regression-tests)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_integration.py
                    $<TARGET_FILE:warc-studio>)
        set_tests_properties(integration PROPERTIES TIMEOUT 90)
    endif()
endif()

foreach(test_target warc-studio-tests warc-studio-util-tests warc-studio-regression-tests)
    target_include_directories(${test_target}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    if(MSVC)
        target_compile_options(${test_target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

target_include_directories(warc-studio
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # Asio headers are in asio/asio/include inside the fetched source tree
        ${asio_SOURCE_DIR}/asio/include
)

target_compile_definitions(warc-studio
    PRIVATE
        ASIO_STANDALONE   # Use standalone Asio (not Boost.Asio)
        CROW_DISABLE_STATIC_DIR  # Only explicitly registered paths are served on each origin
        # Do NOT define CROW_USE_BOOST — Crow uses standalone Asio when the macro is absent
)

target_link_libraries(warc-studio
    PRIVATE
        Crow::Crow
        SQLite::SQLite3
        Threads::Threads
        OpenSSL::Crypto
        ZLIB::ZLIB
        CURL::libcurl
)

if(MSVC)
    target_compile_options(warc-studio PRIVATE /W4 /permissive-)
else()
    target_compile_options(warc-studio PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Copy the static/ directory to the build output so the app can serve CSS and favicon.
# A separate target runs on every build, so CSS/JS edits are picked up even when
# the executable does not need to be relinked.
add_custom_target(warc-studio-static ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/static"
        "${CMAKE_CURRENT_BINARY_DIR}/static"
    COMMENT "Copying static assets to build directory"
)
add_dependencies(warc-studio warc-studio-static)

install(TARGETS warc-studio RUNTIME DESTINATION bin)
install(DIRECTORY static/ DESTINATION share/warc-studio/static)
