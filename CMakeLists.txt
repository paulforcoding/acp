cmake_minimum_required(VERSION 3.20)
project(acp
    VERSION 0.4.0
    DESCRIPTION "AIO File Copy Tool"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------
option(BUILD_STATIC "Build with static linking" OFF)
option(ENABLE_LIBURING "Enable io_uring support (Linux only)" OFF)
option(BUILD_TESTS "Build tests" ON)

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLATFORM_LINUX TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PLATFORM_MACOS TRUE)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

# ---------------------------------------------------------------------------
# Static linking setup
# ---------------------------------------------------------------------------
if(BUILD_STATIC)
    if(PLATFORM_LINUX)
        # Static-link libgcc/libstdc++ but keep glibc dynamic (system ABI guarantee).
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
    elseif(PLATFORM_MACOS)
        # macOS does not support -static for executables.
        # System frameworks remain dynamic.
    endif()
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# Required: spdlog (header-only usage)
find_path(SPDLOG_INCLUDE_DIR spdlog/spdlog.h REQUIRED)

# Required: fmt (header-only usage via FMT_HEADER_ONLY in source)
find_path(FMT_INCLUDE_DIR fmt/format.h REQUIRED)

# Required: OpenSSL (keep dynamic: OL9/RHEL9 do not ship static libs)
find_package(OpenSSL REQUIRED)

# Note: OL9/RHEL9 do not ship static archives for many libs (openssl, libaio).
# We rely on the linker flags (-static-libgcc -static-libstdc++) to static-link
# the C++ runtime only; third-party libs are resolved normally (shared preferred).

# Required: xxhash (prefer pkg-config, fallback to find_library)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(XXHASH libxxhash)
endif()

if(NOT XXHASH_FOUND)
    find_library(XXHASH_LIBRARY xxhash REQUIRED)
    find_path(XXHASH_INCLUDE_DIR xxhash.h REQUIRED)
    set(XXHASH_LIBRARIES ${XXHASH_LIBRARY})
    set(XXHASH_INCLUDE_DIRS ${XXHASH_INCLUDE_DIR})
    set(XXHASH_CFLAGS_OTHER "")
endif()

# Platform-specific: libaio (Linux required)
if(PLATFORM_LINUX)
    find_library(LIBAIO_LIBRARY aio REQUIRED)
    find_path(LIBAIO_INCLUDE_DIR libaio.h REQUIRED)

    # Optional: libacl for ACL preservation
    find_library(ACL_LIBRARY acl)
    if(ACL_LIBRARY)
        find_path(ACL_INCLUDE_DIR sys/acl.h)
        if(ACL_INCLUDE_DIR)
            message(STATUS "libacl found: ${ACL_LIBRARY}")
        endif()
    else()
        message(WARNING "libacl not found, ACL preservation will be skipped")
    endif()
endif()

# Optional: liburing (Linux only)
if(ENABLE_LIBURING)
    if(NOT PLATFORM_LINUX)
        message(FATAL_ERROR "liburing is only supported on Linux")
    endif()
    find_library(LIBURING_LIBRARY uring REQUIRED)
    find_path(LIBURING_INCLUDE_DIR liburing.h REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Source file selection (platform-dependent)
# ---------------------------------------------------------------------------
set(COMMON_SOURCES
    main.cpp
    lib/mainlib.cpp
    base/base.cpp
    base/chan.cpp
    lib/combined/combined.cpp
)

if(PLATFORM_LINUX)
    list(APPEND COMMON_SOURCES lib/acp/acp.cpp)
    if(ENABLE_LIBURING)
        list(APPEND COMMON_SOURCES lib/ucp/ucp.cpp)
    endif()
elseif(PLATFORM_MACOS)
    list(APPEND COMMON_SOURCES lib/gcd/gcd.cpp)
endif()

# ---------------------------------------------------------------------------
# Helper function to apply static link flags per-target
# ---------------------------------------------------------------------------
function(apply_static_link_flags target)
    if(BUILD_STATIC AND PLATFORM_LINUX)
        target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(acp ${COMMON_SOURCES})

target_include_directories(acp
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/thirdparty"
        ${SPDLOG_INCLUDE_DIR}
        ${FMT_INCLUDE_DIR}
)

target_link_libraries(acp
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)

# OL9 spdlog package lacks bundled fmt; force external fmt
target_compile_definitions(acp PRIVATE SPDLOG_FMT_EXTERNAL)

# xxhash
target_include_directories(acp PRIVATE ${XXHASH_INCLUDE_DIRS})
target_link_directories(acp PRIVATE ${XXHASH_LIBRARY_DIRS})
target_link_libraries(acp PRIVATE ${XXHASH_LIBRARIES})
if(XXHASH_CFLAGS_OTHER)
    target_compile_options(acp PRIVATE ${XXHASH_CFLAGS_OTHER})
endif()

if(PLATFORM_LINUX)
    target_include_directories(acp PRIVATE ${LIBAIO_INCLUDE_DIR})
    target_link_libraries(acp PRIVATE ${LIBAIO_LIBRARY})
    if(ENABLE_LIBURING)
        target_include_directories(acp PRIVATE ${LIBURING_INCLUDE_DIR})
        target_link_libraries(acp PRIVATE ${LIBURING_LIBRARY})
        target_compile_definitions(acp PRIVATE ENABLE_LIBURING)
    endif()
    if(ACL_LIBRARY AND ACL_INCLUDE_DIR)
        target_include_directories(acp PRIVATE ${ACL_INCLUDE_DIR})
        target_link_libraries(acp PRIVATE ${ACL_LIBRARY})
        target_compile_definitions(acp PRIVATE HAS_LIBACL)
    endif()
elseif(PLATFORM_MACOS)
    target_link_libraries(acp PRIVATE "-framework CoreServices")
endif()

apply_static_link_flags(acp)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTS)
    enable_testing()

    file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")
    list(FILTER TEST_SOURCES EXCLUDE REGEX "test_main\\.cpp$")

    # Exclude Linux-only tests on macOS
    if(PLATFORM_MACOS)
        list(FILTER TEST_SOURCES EXCLUDE REGEX "test_base\\.cpp$")
        list(FILTER TEST_SOURCES EXCLUDE REGEX "test_inotify")
        list(FILTER TEST_SOURCES EXCLUDE REGEX "test_ioslot\\.cpp$")
        list(FILTER TEST_SOURCES EXCLUDE REGEX "test_integration_liburing")
        list(FILTER TEST_SOURCES EXCLUDE REGEX "test_integration_inotify")
        list(FILTER TEST_SOURCES EXCLUDE REGEX "test_integration_direct_io")
    endif()

    # Test executable needs implementation objects (same as acp minus main.cpp)
    set(TEST_IMPL_SOURCES ${COMMON_SOURCES})
    list(REMOVE_ITEM TEST_IMPL_SOURCES main.cpp)

    add_executable(test_acp
        tests/test_main.cpp
        lib/thirdparty/catch2/catch_amalgamated.cpp
        ${TEST_IMPL_SOURCES}
        ${TEST_SOURCES}
    )

    target_include_directories(test_acp
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/lib/thirdparty"
            "${CMAKE_CURRENT_SOURCE_DIR}/lib/thirdparty/catch2"
            ${SPDLOG_INCLUDE_DIR}
            ${FMT_INCLUDE_DIR}
    )

    target_link_libraries(test_acp
        PRIVATE
            OpenSSL::SSL
            OpenSSL::Crypto
            Threads::Threads
    )

    target_compile_definitions(test_acp PRIVATE SPDLOG_FMT_EXTERNAL)

    target_include_directories(test_acp PRIVATE ${XXHASH_INCLUDE_DIRS})
    target_link_directories(test_acp PRIVATE ${XXHASH_LIBRARY_DIRS})
    target_link_libraries(test_acp PRIVATE ${XXHASH_LIBRARIES})
    if(XXHASH_CFLAGS_OTHER)
        target_compile_options(test_acp PRIVATE ${XXHASH_CFLAGS_OTHER})
    endif()

    if(PLATFORM_LINUX)
        target_include_directories(test_acp PRIVATE ${LIBAIO_INCLUDE_DIR})
        target_link_libraries(test_acp PRIVATE ${LIBAIO_LIBRARY})
        if(ENABLE_LIBURING)
            target_include_directories(test_acp PRIVATE ${LIBURING_INCLUDE_DIR})
            target_link_libraries(test_acp PRIVATE ${LIBURING_LIBRARY})
            target_compile_definitions(test_acp PRIVATE ENABLE_LIBURING)
        endif()
        if(ACL_LIBRARY AND ACL_INCLUDE_DIR)
            target_include_directories(test_acp PRIVATE ${ACL_INCLUDE_DIR})
            target_link_libraries(test_acp PRIVATE ${ACL_LIBRARY})
            target_compile_definitions(test_acp PRIVATE HAS_LIBACL)
        endif()
    elseif(PLATFORM_MACOS)
        target_link_libraries(test_acp PRIVATE "-framework CoreServices")
    endif()

    apply_static_link_flags(test_acp)

    add_test(NAME acp_tests COMMAND test_acp WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ---------------------------------------------------------------------------
# compile_commands.json for clangd / IDE
# ---------------------------------------------------------------------------
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")
