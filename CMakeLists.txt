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
    target_include_directories(httpd PRIVATE
        ${LLHTTP_INCLUDE_DIR}
    )
    target_link_libraries(httpd PRIVATE
        ${LLHTTP_LIBRARY}
    )
endif()
