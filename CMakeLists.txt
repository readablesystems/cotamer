cmake_minimum_required(VERSION 3.16)
project(cotamer CXX)

option(SAN "Enable AddressSanitizer+UBSanitizer" OFF)
option(ASAN "Enable AddressSanitizer" OFF)
option(UBSAN "Enable UBSanitizer" OFF)
option(TSAN "Enable ThreadSanitizer" OFF)

# Optional integrations. AUTO enables if the dependency is found, ON requires
# it, OFF skips it.
set(COTAMER_LLHTTP "AUTO" CACHE STRING "llhttp integration (AUTO/ON/OFF)")
set_property(CACHE COTAMER_LLHTTP PROPERTY STRINGS AUTO ON OFF)
set(COTAMER_CURL "AUTO" CACHE STRING "libcurl integration (AUTO/ON/OFF)")
set_property(CACHE COTAMER_CURL PROPERTY STRINGS AUTO ON OFF)
set(COTAMER_MBEDTLS "AUTO" CACHE STRING "mbedtls integration (AUTO/ON/OFF)")
set_property(CACHE COTAMER_MBEDTLS PROPERTY STRINGS AUTO ON OFF)
set(COTAMER_NLOHMANN_JSON "AUTO" CACHE STRING "nlohmann::json integration (AUTO/ON/OFF)")
set_property(CACHE COTAMER_NLOHMANN_JSON PROPERTY STRINGS AUTO ON OFF)
set(COTAMER_WSLAY "AUTO" CACHE STRING "wslay integration (WebSockets) (AUTO/ON/OFF)")
set_property(CACHE COTAMER_WSLAY PROPERTY STRINGS AUTO ON OFF)
set(COTAMER_ZLIB "AUTO" CACHE STRING "zlib integration (AUTO/ON/OFF)")
set_property(CACHE COTAMER_ZLIB PROPERTY STRINGS AUTO ON OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

# Find xxhash
find_path(XXHASH_INCLUDE_DIR xxhash.h HINTS /opt/homebrew/include)
find_library(XXHASH_LIBRARY xxhash HINTS /opt/homebrew/lib)

if(NOT XXHASH_INCLUDE_DIR OR NOT XXHASH_LIBRARY)
    message(FATAL_ERROR "xxhash not found. Install with: brew install xxhash")
endif()

# Find llhttp
set(COTAMER_HAVE_LLHTTP 0)
if(NOT COTAMER_LLHTTP STREQUAL "OFF")
    find_path(LLHTTP_INCLUDE_DIR llhttp.h HINTS /opt/homebrew/include)
    find_library(LLHTTP_LIBRARY llhttp HINTS /opt/homebrew/lib)
    if(LLHTTP_INCLUDE_DIR AND LLHTTP_LIBRARY)
        set(COTAMER_HAVE_LLHTTP 1)
        message(STATUS "llhttp integration: enabled")
    elseif(COTAMER_LLHTTP STREQUAL "ON")
        message(FATAL_ERROR "COTAMER_LLHTTP=ON but llhttp was not found")
    else()
        message(STATUS "llhttp integration: disabled (llhttp not found)")
    endif()
else()
    message(STATUS "llhttp integration: disabled (COTAMER_LLHTTP=OFF)")
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

# Find mbedtls (mbedtls, mbedx509, mbedcrypto)
set(COTAMER_HAVE_MBEDTLS 0)
if(NOT COTAMER_MBEDTLS STREQUAL "OFF")
    find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h HINTS /opt/homebrew/include /usr/local/include)
    find_library(MBEDTLS_LIBRARY mbedtls HINTS /opt/homebrew/lib /usr/local/lib)
    find_library(MBEDX509_LIBRARY mbedx509 HINTS /opt/homebrew/lib /usr/local/lib)
    find_library(MBEDCRYPTO_LIBRARY mbedcrypto HINTS /opt/homebrew/lib /usr/local/lib)
    if(MBEDTLS_INCLUDE_DIR AND MBEDTLS_LIBRARY AND MBEDX509_LIBRARY AND MBEDCRYPTO_LIBRARY)
        set(COTAMER_HAVE_MBEDTLS 1)
        set(MBEDTLS_LIBRARIES ${MBEDTLS_LIBRARY} ${MBEDX509_LIBRARY} ${MBEDCRYPTO_LIBRARY})
        message(STATUS "mbedtls integration: enabled")
    elseif(COTAMER_MBEDTLS STREQUAL "ON")
        message(FATAL_ERROR "COTAMER_MBEDTLS=ON but mbedtls was not found")
    else()
        message(STATUS "mbedtls integration: disabled (mbedtls not found)")
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

# Find wslay (WebSockets)
set(COTAMER_HAVE_WSLAY 0)
if(NOT COTAMER_WSLAY STREQUAL "OFF" AND COTAMER_HAVE_LLHTTP)
    find_path(WSLAY_INCLUDE_DIR wslay/wslay.h HINTS /opt/homebrew/include /usr/local/include)
    find_library(WSLAY_LIBRARY wslay HINTS /opt/homebrew/lib /usr/local/lib)
    if(WSLAY_INCLUDE_DIR AND WSLAY_LIBRARY)
        set(COTAMER_HAVE_WSLAY 1)
        message(STATUS "wslay integration: enabled")
    elseif(COTAMER_WSLAY STREQUAL "ON")
        message(FATAL_ERROR "COTAMER_WSLAY=ON but wslay was not found")
    else()
        message(STATUS "wslay integration: disabled (wslay not found)")
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

# Generate config header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cotamer/config.hh.in
    ${CMAKE_CURRENT_BINARY_DIR}/cotamer/config.hh
    @ONLY
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_library(Cotamer OBJECT
    cotamer/cotamer.cc
    cotamer/io.cc
)

if(COTAMER_HAVE_LLHTTP)
    add_library(CotamerHttp OBJECT
        cotamer/http.cc
    )
    target_include_directories(CotamerHttp PUBLIC ${LLHTTP_INCLUDE_DIR})
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

add_executable(cot-manual
    test/cot-manual.cc
    $<TARGET_OBJECTS:Cotamer>
)
if(COTAMER_HAVE_LLHTTP)
    target_sources(cot-manual PRIVATE $<TARGET_OBJECTS:CotamerHttp>)
    target_include_directories(cot-manual PRIVATE ${LLHTTP_INCLUDE_DIR})
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

if(COTAMER_HAVE_LLHTTP AND COTAMER_HAVE_NLOHMANN_JSON)
    add_executable(jsond
        examples/jsond.cc
        $<TARGET_OBJECTS:Cotamer>
        $<TARGET_OBJECTS:CotamerHttp>
    )
    target_include_directories(jsond PRIVATE
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
    target_include_directories(jsond-client PRIVATE
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
        target_include_directories(jsond-curl-client PRIVATE
            ${LLHTTP_INCLUDE_DIR}
        )
        target_link_libraries(jsond-curl-client PRIVATE
            CURL::libcurl
            ${LLHTTP_LIBRARY}
            nlohmann_json::nlohmann_json
        )
    endif()
endif()

if(COTAMER_HAVE_LLHTTP)
    add_executable(httpd
        examples/httpd.cc
        $<TARGET_OBJECTS:Cotamer>
        $<TARGET_OBJECTS:CotamerHttp>
    )
    target_include_directories(httpd PRIVATE
        ${LLHTTP_INCLUDE_DIR}
    )
    target_link_libraries(httpd PRIVATE
        ${LLHTTP_LIBRARY}
    )

    add_executable(cot-test-http
        test/cot-test-http.cc
        $<TARGET_OBJECTS:Cotamer>
        $<TARGET_OBJECTS:CotamerHttp>
    )
    target_include_directories(cot-test-http PRIVATE
        ${LLHTTP_INCLUDE_DIR}
    )
    target_link_libraries(cot-test-http PRIVATE
        ${LLHTTP_LIBRARY}
    )
endif()

# Optional libcurl integration
if(COTAMER_HAVE_CURL)
    add_library(CotamerCurl OBJECT
        cotamer/curl.cc
    )
    target_link_libraries(CotamerCurl PUBLIC CURL::libcurl)

    # cot-test-curl uses cot::http_parser for its in-process test server.
    if(COTAMER_HAVE_LLHTTP)
        add_executable(cot-test-curl
            test/cot-test-curl.cc
            $<TARGET_OBJECTS:Cotamer>
            $<TARGET_OBJECTS:CotamerCurl>
            $<TARGET_OBJECTS:CotamerHttp>
        )
        target_include_directories(cot-test-curl PRIVATE
            ${LLHTTP_INCLUDE_DIR}
        )
        target_link_libraries(cot-test-curl PRIVATE
            CURL::libcurl
            ${LLHTTP_LIBRARY}
        )
    endif()
endif()

# Optional mbedtls integration
if(COTAMER_HAVE_MBEDTLS)
    add_library(CotamerTls OBJECT
        cotamer/tls.cc
    )
    target_include_directories(CotamerTls PUBLIC ${MBEDTLS_INCLUDE_DIR})

    add_executable(cot-test-tls
        test/cot-test-tls.cc
        $<TARGET_OBJECTS:Cotamer>
        $<TARGET_OBJECTS:CotamerTls>
    )
    target_include_directories(cot-test-tls PRIVATE ${MBEDTLS_INCLUDE_DIR})
    target_link_libraries(cot-test-tls PRIVATE ${MBEDTLS_LIBRARIES})
endif()

# Optional wslay (WebSocket) integration
if(COTAMER_HAVE_LLHTTP AND COTAMER_HAVE_WSLAY)
    add_library(CotamerWebSocket OBJECT
        cotamer/websocket.cc
    )
    target_include_directories(CotamerWebSocket PUBLIC ${WSLAY_INCLUDE_DIR})
    target_include_directories(CotamerWebSocket PRIVATE ${LLHTTP_INCLUDE_DIR})
    if(COTAMER_HAVE_MBEDTLS)
        target_include_directories(CotamerWebSocket PRIVATE ${MBEDTLS_INCLUDE_DIR})
    endif()
    if(COTAMER_HAVE_ZLIB)
        target_link_libraries(CotamerWebSocket PUBLIC ZLIB::ZLIB)
    endif()

    target_sources(httpd PRIVATE $<TARGET_OBJECTS:CotamerWebSocket>)
    target_include_directories(httpd PRIVATE ${WSLAY_INCLUDE_DIR})
    target_link_libraries(httpd PRIVATE ${WSLAY_LIBRARY})
    target_compile_definitions(httpd PRIVATE COTAMER_HTTPD_HAS_WSLAY=1)
    if(COTAMER_HAVE_ZLIB)
        target_link_libraries(httpd PRIVATE ZLIB::ZLIB)
    endif()

    add_executable(cot-test-ws
        test/cot-test-ws.cc
        $<TARGET_OBJECTS:Cotamer>
        $<TARGET_OBJECTS:CotamerHttp>
        $<TARGET_OBJECTS:CotamerWebSocket>
    )
    target_include_directories(cot-test-ws PRIVATE
        ${WSLAY_INCLUDE_DIR}
        ${LLHTTP_INCLUDE_DIR}
    )
    target_link_libraries(cot-test-ws PRIVATE
        ${WSLAY_LIBRARY}
        ${LLHTTP_LIBRARY}
    )
    if(COTAMER_HAVE_ZLIB)
        target_link_libraries(cot-test-ws PRIVATE ZLIB::ZLIB)
    endif()

    add_executable(ws-echo
        examples/ws-echo.cc
        $<TARGET_OBJECTS:Cotamer>
        $<TARGET_OBJECTS:CotamerHttp>
        $<TARGET_OBJECTS:CotamerWebSocket>
    )
    target_include_directories(ws-echo PRIVATE
        ${WSLAY_INCLUDE_DIR}
        ${LLHTTP_INCLUDE_DIR}
    )
    target_link_libraries(ws-echo PRIVATE
        ${WSLAY_LIBRARY}
        ${LLHTTP_LIBRARY}
    )
    if(COTAMER_HAVE_ZLIB)
        target_link_libraries(ws-echo PRIVATE ZLIB::ZLIB)
    endif()
endif()
