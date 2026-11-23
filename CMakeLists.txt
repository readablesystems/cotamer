cmake_minimum_required(VERSION 3.16)
project(cotamer CXX)

option(SAN "Enable AddressSanitizer+UBSanitizer" OFF)
option(ASAN "Enable AddressSanitizer" OFF)
option(UBSAN "Enable UBSanitizer" OFF)
option(TSAN "Enable ThreadSanitizer" OFF)
# Build tests and examples by default only as the top-level project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(COTAMER_TOP_LEVEL ON)
else()
    set(COTAMER_TOP_LEVEL OFF)
endif()
option(COTAMER_BUILD_TESTS "Build Cotamer tests and examples" ${COTAMER_TOP_LEVEL})

# Optional integrations. AUTO enables if the dependency is found, ON requires
# it, OFF skips it.
set(COTAMER_CURL "AUTO" CACHE STRING "libcurl integration (AUTO/ON/OFF)")
set_property(CACHE COTAMER_CURL PROPERTY STRINGS AUTO ON OFF)
# llhttp: AUTO uses an installed llhttp if found; ON also downloads it if
# not found; FETCH always downloads it.
set(COTAMER_LLHTTP "AUTO" CACHE STRING "llhttp integration (AUTO/ON/FETCH/OFF)")
set_property(CACHE COTAMER_LLHTTP PROPERTY STRINGS AUTO ON FETCH OFF)
set(COTAMER_LLHTTP_VERSION "latest" CACHE STRING
    "llhttp version to download (`latest` for the latest release)")
set(COTAMER_LLHTTP_SHA256 "" CACHE STRING "SHA-256 of the llhttp download")
set(COTAMER_NLOHMANN_JSON "AUTO" CACHE STRING "nlohmann::json integration (AUTO/ON/OFF)")
set_property(CACHE COTAMER_NLOHMANN_JSON PROPERTY STRINGS AUTO ON OFF)
set(COTAMER_ZLIB "AUTO" CACHE STRING "zlib integration (AUTO/ON/OFF)")
set_property(CACHE COTAMER_ZLIB PROPERTY STRINGS AUTO ON OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Check for a C++23 standard library early, rather than failing mid-build
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
#include <expected>
int main() { std::expected<int, int> e(1); return *e - 1; }" COTAMER_HAVE_STD_EXPECTED)
if(NOT COTAMER_HAVE_STD_EXPECTED)
    message(FATAL_ERROR "Cotamer requires C++23, including <expected> (GCC 13, "
        "Clang 17, or later). On Amazon Linux 2023, `dnf install gcc14 gcc14-c++`, "
        "then configure with -DCMAKE_CXX_COMPILER=gcc14-g++ -DCMAKE_C_COMPILER=gcc14-gcc")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_compile_options(-g -O3 -W -Wall)
add_link_options(-g -O3 -W -Wall)

if((SAN AND NOT TSAN) OR UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()
if((SAN AND NOT TSAN) OR ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()
if(TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# Find xxhash (used by examples)
if(COTAMER_BUILD_TESTS)
    find_path(XXHASH_INCLUDE_DIR xxhash.h HINTS /opt/homebrew/include)
    find_library(XXHASH_LIBRARY xxhash HINTS /opt/homebrew/lib)

    if(NOT XXHASH_INCLUDE_DIR OR NOT XXHASH_LIBRARY)
        message(FATAL_ERROR "xxhash not found. Install with: brew install xxhash")
    endif()
endif()

# Find or download llhttp
set(COTAMER_HAVE_LLHTTP 0)
if(COTAMER_LLHTTP STREQUAL "AUTO" OR COTAMER_LLHTTP STREQUAL "ON")
    find_path(LLHTTP_INCLUDE_DIR llhttp.h HINTS /opt/homebrew/include)
    find_library(LLHTTP_LIBRARY llhttp HINTS /opt/homebrew/lib)
    if(LLHTTP_INCLUDE_DIR AND LLHTTP_LIBRARY)
        set(COTAMER_HAVE_LLHTTP 1)
        message(STATUS "llhttp integration: enabled")
    elseif(COTAMER_LLHTTP STREQUAL "AUTO")
        message(STATUS "llhttp integration: disabled (llhttp not found)")
    endif()
elseif(COTAMER_LLHTTP STREQUAL "OFF")
    message(STATUS "llhttp integration: disabled (COTAMER_LLHTTP=OFF)")
elseif(NOT COTAMER_LLHTTP STREQUAL "FETCH")
    message(FATAL_ERROR "COTAMER_LLHTTP must be AUTO, ON, FETCH, or OFF")
endif()
if(NOT COTAMER_HAVE_LLHTTP
   AND (COTAMER_LLHTTP STREQUAL "ON" OR COTAMER_LLHTTP STREQUAL "FETCH"))
    # Download a release and build its generated C sources directly (its
    # CMakeLists.txt would change our build type and add install rules).
    if(CMAKE_VERSION VERSION_LESS 3.19)
        message(FATAL_ERROR "Downloading llhttp requires CMake 3.19 or later")
    endif()
    # This version's checksum is known; it's the fallback for `latest`.
    set(_llhttp_pinned 9.4.4)
    set(_llhttp_pinned_sha256 5600adb1498f82a9c719b8eb19310021737388e0d92362a6d47be532710d3c96)
    set(_llhttp_version ${COTAMER_LLHTTP_VERSION})
    if(_llhttp_version STREQUAL "latest")
        if(COTAMER_LLHTTP_LATEST)
            set(_llhttp_version ${COTAMER_LLHTTP_LATEST})
        else()
            set(_json "${CMAKE_CURRENT_BINARY_DIR}/llhttp-latest.json")
            file(DOWNLOAD "https://api.github.com/repos/nodejs/llhttp/releases/latest"
                "${_json}" STATUS _status TIMEOUT 30)
            list(GET _status 0 _status_code)
            if(_status_code EQUAL 0)
                file(READ "${_json}" _json)
                string(JSON _tag ERROR_VARIABLE _json_error GET "${_json}" tag_name)
                if(NOT _json_error AND _tag MATCHES "^release/v([0-9]+[.][0-9]+[.][0-9]+)$")
                    set(_llhttp_version ${CMAKE_MATCH_1})
                    set(COTAMER_LLHTTP_LATEST ${_llhttp_version} CACHE INTERNAL
                        "latest llhttp release, as of the first configure")
                endif()
            endif()
            if(_llhttp_version STREQUAL "latest")
                message(WARNING "Couldn't find the latest llhttp release; "
                    "using llhttp ${_llhttp_pinned}")
                set(_llhttp_version ${_llhttp_pinned})
            endif()
        endif()
    endif()
    if(COTAMER_LLHTTP_SHA256)
        set(_llhttp_hash URL_HASH SHA256=${COTAMER_LLHTTP_SHA256})
    elseif(_llhttp_version STREQUAL _llhttp_pinned)
        set(_llhttp_hash URL_HASH SHA256=${_llhttp_pinned_sha256})
    else()
        set(_llhttp_hash)
    endif()
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)   # extracted files get the current time
    endif()
    include(FetchContent)
    FetchContent_Declare(cotamer_llhttp
        URL "https://github.com/nodejs/llhttp/archive/refs/tags/release/v${_llhttp_version}.tar.gz"
        ${_llhttp_hash}
        SOURCE_SUBDIR cotamer-no-cmake)  # don't add llhttp's CMakeLists.txt
    FetchContent_MakeAvailable(cotamer_llhttp)
    enable_language(C)
    add_library(cotamer_llhttp STATIC
        ${cotamer_llhttp_SOURCE_DIR}/src/api.c
        ${cotamer_llhttp_SOURCE_DIR}/src/http.c
        ${cotamer_llhttp_SOURCE_DIR}/src/llhttp.c
    )
    target_include_directories(cotamer_llhttp PUBLIC ${cotamer_llhttp_SOURCE_DIR}/include)
    target_compile_options(cotamer_llhttp PRIVATE -w)
    set_target_properties(cotamer_llhttp PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(LLHTTP_INCLUDE_DIR ${cotamer_llhttp_SOURCE_DIR}/include)
    set(LLHTTP_LIBRARY cotamer_llhttp)
    set(COTAMER_HAVE_LLHTTP 1)
    message(STATUS "llhttp integration: enabled (downloaded llhttp ${_llhttp_version})")
endif()

# Find libcurl
set(COTAMER_HAVE_CURL 0)
if(NOT COTAMER_CURL STREQUAL "OFF")
    find_package(CURL QUIET)
    if(CURL_FOUND)
        set(COTAMER_HAVE_CURL 1)
        message(STATUS "libcurl integration: enabled (${CURL_VERSION_STRING})")
    elseif(COTAMER_CURL STREQUAL "ON")
        message(FATAL_ERROR "COTAMER_CURL=ON but libcurl was not found")
    else()
        message(STATUS "libcurl integration: disabled (libcurl not found)")
    endif()
endif()

# Find nlohmann::json (header-only)
set(COTAMER_HAVE_NLOHMANN_JSON 0)
if(NOT COTAMER_NLOHMANN_JSON STREQUAL "OFF")
    find_package(nlohmann_json QUIET)
    if(nlohmann_json_FOUND)
        set(COTAMER_HAVE_NLOHMANN_JSON 1)
        message(STATUS "nlohmann::json integration: enabled (${nlohmann_json_VERSION})")
        # Propagate the include path globally so cotamer/http.hh's forward-
        # declaration of nlohmann::json compiles for every consumer.
        get_target_property(_njson_inc nlohmann_json::nlohmann_json
                            INTERFACE_INCLUDE_DIRECTORIES)
        if(_njson_inc)
            include_directories(${_njson_inc})
        endif()
    elseif(COTAMER_NLOHMANN_JSON STREQUAL "ON")
        message(FATAL_ERROR "COTAMER_NLOHMANN_JSON=ON but nlohmann_json was not found")
    else()
        message(STATUS "nlohmann::json integration: disabled (nlohmann_json not found)")
    endif()
endif()

# Find zlib
set(COTAMER_HAVE_ZLIB 0)
if(NOT COTAMER_ZLIB STREQUAL "OFF")
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        set(COTAMER_HAVE_ZLIB 1)
        message(STATUS "zlib integration: enabled (${ZLIB_VERSION_STRING})")
    elseif(COTAMER_ZLIB STREQUAL "ON")
        message(FATAL_ERROR "COTAMER_ZLIB=ON but zlib was not found")
    else()
        message(STATUS "zlib integration: disabled (zlib not found)")
    endif()
endif()

# Detect epoll_pwait2 (epoll wait with a nanosecond timeout)
include(CheckCXXSymbolExists)
check_cxx_symbol_exists(epoll_pwait2 "sys/epoll.h" COTAMER_EPOLL_PWAIT2_DECLARED)
if(COTAMER_EPOLL_PWAIT2_DECLARED)
    set(COTAMER_HAVE_EPOLL_PWAIT2 1)
else()
    set(COTAMER_HAVE_EPOLL_PWAIT2 0)
endif()

# Generate config header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cotamer/config.hh.in
    ${CMAKE_CURRENT_BINARY_DIR}/cotamer/config.hh
    @ONLY
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_library(Cotamer OBJECT
    cotamer/cotamer.cc
    cotamer/http_fields.cc
    cotamer/io.cc
)
target_include_directories(Cotamer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

if(COTAMER_HAVE_LLHTTP)
    add_library(CotamerHttp OBJECT
        cotamer/http.cc
    )
    # (BEFORE, so a downloaded llhttp.h wins over one in other include dirs)
    target_include_directories(CotamerHttp BEFORE PUBLIC ${LLHTTP_INCLUDE_DIR})
    target_link_libraries(CotamerHttp PUBLIC Cotamer ${LLHTTP_LIBRARY})
endif()

# Optional libcurl integration
if(COTAMER_HAVE_CURL)
    add_library(CotamerCurl OBJECT
        cotamer/curl.cc
    )
    target_link_libraries(CotamerCurl PUBLIC Cotamer CURL::libcurl)
endif()

if(NOT COTAMER_BUILD_TESTS)
    return()
endif()

add_library(CotamerExamples OBJECT
    examples/utils.cc
)

add_executable(cot-test
    test/cot-test.cc
    $<TARGET_OBJECTS:Cotamer>
)

add_executable(cot-test-threads
    test/cot-test-threads.cc
    $<TARGET_OBJECTS:Cotamer>
)

add_executable(cot-test-io
    test/cot-test-io.cc
    $<TARGET_OBJECTS:Cotamer>
)

if(COTAMER_HAVE_LLHTTP)
    add_executable(cot-test-http
        test/cot-test-http.cc
        $<TARGET_OBJECTS:Cotamer>
        $<TARGET_OBJECTS:CotamerHttp>
    )
    target_include_directories(cot-test-http BEFORE PRIVATE ${LLHTTP_INCLUDE_DIR})
    target_link_libraries(cot-test-http PRIVATE ${LLHTTP_LIBRARY})
endif()

add_executable(cot-manual
    test/cot-manual.cc
    $<TARGET_OBJECTS:Cotamer>
)
if(COTAMER_HAVE_LLHTTP)
    target_sources(cot-manual PRIVATE $<TARGET_OBJECTS:CotamerHttp>)
    target_include_directories(cot-manual BEFORE PRIVATE ${LLHTTP_INCLUDE_DIR})
    target_link_libraries(cot-manual PRIVATE ${LLHTTP_LIBRARY})
endif()

add_executable(cot-benchmark
    test/cot-benchmark.cc
    $<TARGET_OBJECTS:Cotamer>
)

add_executable(ctconsensus
    examples/ctconsensus.cc
    $<TARGET_OBJECTS:Cotamer>
    $<TARGET_OBJECTS:CotamerExamples>
)

add_executable(rpcg-server
    examples/rpcg-server.cc
    examples/serverstub.cc
    $<TARGET_OBJECTS:Cotamer>
    $<TARGET_OBJECTS:CotamerExamples>
)
target_include_directories(rpcg-server PRIVATE
    ${XXHASH_INCLUDE_DIR}
)
target_link_libraries(rpcg-server PRIVATE
    ${XXHASH_LIBRARY}
)

add_executable(rpcg-client
    examples/rpcg-client.cc
    examples/clientstub.cc
    $<TARGET_OBJECTS:Cotamer>
    $<TARGET_OBJECTS:CotamerExamples>
)
target_include_directories(rpcg-client PRIVATE
    ${XXHASH_INCLUDE_DIR}
)
target_link_libraries(rpcg-client PRIVATE
    ${XXHASH_LIBRARY}
)

if(COTAMER_HAVE_LLHTTP)
    add_executable(httpd
        examples/httpd.cc
        $<TARGET_OBJECTS:Cotamer>
        $<TARGET_OBJECTS:CotamerHttp>
    )
    target_include_directories(httpd BEFORE PRIVATE
        ${LLHTTP_INCLUDE_DIR}
    )
    target_link_libraries(httpd PRIVATE
        ${LLHTTP_LIBRARY}
    )
endif()

if(COTAMER_HAVE_LLHTTP AND COTAMER_HAVE_NLOHMANN_JSON)
    add_executable(jsond
        examples/jsond.cc
        $<TARGET_OBJECTS:Cotamer>
        $<TARGET_OBJECTS:CotamerHttp>
    )
    target_include_directories(jsond BEFORE PRIVATE
        ${LLHTTP_INCLUDE_DIR}
    )
    target_link_libraries(jsond PRIVATE
        ${LLHTTP_LIBRARY}
        nlohmann_json::nlohmann_json
    )

    add_executable(jsond-client
        examples/jsond-client.cc
        $<TARGET_OBJECTS:Cotamer>
        $<TARGET_OBJECTS:CotamerHttp>
    )
    target_include_directories(jsond-client BEFORE PRIVATE
        ${LLHTTP_INCLUDE_DIR}
    )
    target_link_libraries(jsond-client PRIVATE
        ${LLHTTP_LIBRARY}
        nlohmann_json::nlohmann_json
    )

    if(COTAMER_HAVE_CURL)
        add_executable(jsond-curl-client
            examples/jsond-curl-client.cc
            $<TARGET_OBJECTS:Cotamer>
            $<TARGET_OBJECTS:CotamerCurl>
        )
        target_include_directories(jsond-curl-client BEFORE PRIVATE
            ${LLHTTP_INCLUDE_DIR}
        )
        target_link_libraries(jsond-curl-client PRIVATE
            CURL::libcurl
            ${LLHTTP_LIBRARY}
            nlohmann_json::nlohmann_json
        )
    endif()
endif()

if(COTAMER_HAVE_NLOHMANN_JSON)
    add_executable(udp-jsond
        examples/udp-jsond.cc
        $<TARGET_OBJECTS:Cotamer>
    )
    target_link_libraries(udp-jsond PRIVATE
        nlohmann_json::nlohmann_json
    )

    add_executable(udp-jsond-client
        examples/udp-jsond-client.cc
        $<TARGET_OBJECTS:Cotamer>
    )
    target_link_libraries(udp-jsond-client PRIVATE
        nlohmann_json::nlohmann_json
    )
endif()

if(COTAMER_HAVE_CURL)
    # cot-test-curl uses cot::http_parser for its in-process test server.
    if(COTAMER_HAVE_LLHTTP)
        add_executable(cot-test-curl
            test/cot-test-curl.cc
            $<TARGET_OBJECTS:Cotamer>
            $<TARGET_OBJECTS:CotamerCurl>
            $<TARGET_OBJECTS:CotamerHttp>
        )
        target_include_directories(cot-test-curl BEFORE PRIVATE
            ${LLHTTP_INCLUDE_DIR}
        )
        target_link_libraries(cot-test-curl PRIVATE
            CURL::libcurl
            ${LLHTTP_LIBRARY}
        )
    endif()
endif()
