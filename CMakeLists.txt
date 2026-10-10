cmake_minimum_required(VERSION 3.5...3.29)
option(DUCKDB_WASM_EXTENSION "Whether compiling for Wasm target" OFF)

###
# Configuration
###
if(NOT DEFINED EXTENSION_NAME)
    message(FATAL_ERROR "DuckDB extension name is required")
endif()
if (DEFINED TARGET_DUCKDB_VERSION_MAJOR)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_MAJOR=${TARGET_DUCKDB_VERSION_MAJOR})
endif()
if (DEFINED TARGET_DUCKDB_VERSION_MINOR)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_MINOR=${TARGET_DUCKDB_VERSION_MINOR})
endif()
if (DEFINED TARGET_DUCKDB_VERSION_PATCH)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_PATCH=${TARGET_DUCKDB_VERSION_PATCH})
endif()

add_definitions(-DDUCKDB_EXTENSION_NAME=${EXTENSION_NAME})

if (DEFINED DUCKDB_EXTENSION_API_VERSION_UNSTABLE)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_UNSTABLE=${DUCKDB_EXTENSION_API_VERSION_UNSTABLE})
endif()

# CI cross-compiles macOS builds (e.g. x86_64 on an arm64 runner) and passes the
# target as OSX_BUILD_ARCH. Must be set before project() to take effect (APPLE is
# not defined yet at this point, so only the variable is checked).
if (NOT "$ENV{OSX_BUILD_ARCH}" STREQUAL "")
    set(CMAKE_OSX_ARCHITECTURES "$ENV{OSX_BUILD_ARCH}")
endif()

###
# Build
###
project(${EXTENSION_NAME} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Hide symbols. Must be set before add_library: these variables only
# initialize properties on targets created after this point.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -s")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -s")

# Create Extension library
set(EXTENSION_SOURCES
        src/docn_extension.cpp
        src/common/config.cpp
        src/common/http.cpp
        src/common/retry.cpp
        src/ai/decide.cpp
        src/ai/decide_json.cpp
        src/ai/models.cpp
        src/ai/models_json.cpp
        src/ai/prompt.cpp
        src/ai/prompt_json.cpp
        src/ai/embed.cpp
        src/ai/embed_json.cpp
        src/common/bind_args.cpp
)

if (DUCKDB_WASM_EXTENSION)
	add_library(${EXTENSION_NAME} STATIC ${EXTENSION_SOURCES})
else()
	add_library(${EXTENSION_NAME} SHARED ${EXTENSION_SOURCES})
endif()

# Include own headers
target_include_directories(${EXTENSION_NAME} PRIVATE src)

# Include DuckDB C API headers
target_include_directories(${EXTENSION_NAME} PRIVATE duckdb_capi)

# Dependencies from vcpkg (see vcpkg.json)
find_package(CURL CONFIG REQUIRED)
find_package(yyjson CONFIG REQUIRED)
target_link_libraries(${EXTENSION_NAME} PRIVATE CURL::libcurl yyjson::yyjson)

# Export only the extension entry point. The static vcpkg libraries (curl, OpenSSL,
# zlib) are built with default visibility, so without this their symbols leak and
# can clash with other extensions that bundle their own OpenSSL.
if (APPLE)
	target_link_options(${EXTENSION_NAME} PRIVATE "-Wl,-exported_symbol,_${EXTENSION_NAME}_init_c_api")
elseif (UNIX AND NOT DUCKDB_WASM_EXTENSION)
	target_link_options(${EXTENSION_NAME} PRIVATE "-Wl,--exclude-libs,ALL")
endif()
