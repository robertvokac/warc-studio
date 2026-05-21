cmake_minimum_required(VERSION 3.26)
project(warc-studio
    VERSION 0.1.0
    DESCRIPTION "Small C++23 archive studio for Browsertrix, WACZ files, and ReplayWeb.page"
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

# ---------------------------------------------------------------------------
# Application target
# ---------------------------------------------------------------------------

add_executable(warc-studio
    src/main.cpp
    src/BrowsertrixService.cpp
    src/CaptureDepthPolicy.cpp
    src/Database.cpp
    src/FileService.cpp
    src/Html.cpp
    src/HttpUtil.cpp
)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------

add_executable(warc-studio-tests
    src/CaptureDepthPolicy.cpp
    tests/test_capture_depth_policy.cpp
)

target_include_directories(warc-studio-tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(warc-studio-tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(warc-studio-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_include_directories(warc-studio
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # Asio headers are in asio/asio/include inside the fetched source tree
        ${asio_SOURCE_DIR}/asio/include
)

target_compile_definitions(warc-studio
    PRIVATE
        ASIO_STANDALONE   # Use standalone Asio (not Boost.Asio)
        # Do NOT define CROW_USE_BOOST — Crow uses standalone Asio when the macro is absent
)

target_link_libraries(warc-studio
    PRIVATE
        Crow::Crow
        SQLite::SQLite3
        Threads::Threads
        OpenSSL::Crypto
)

if(MSVC)
    target_compile_options(warc-studio PRIVATE /W4 /permissive-)
else()
    target_compile_options(warc-studio PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Copy the static/ directory to the build output so the app can serve CSS and favicon.
add_custom_command(TARGET warc-studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/static"
        "${CMAKE_CURRENT_BINARY_DIR}/static"
    COMMENT "Copying static assets to build directory"
)

install(TARGETS warc-studio RUNTIME DESTINATION bin)
install(DIRECTORY static/ DESTINATION share/warc-studio/static)
