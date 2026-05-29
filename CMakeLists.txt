cmake_minimum_required(VERSION 3.20)
project(modmesh-bot CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(MODMESH_BOT_VERSION "0.0.1")

find_package(OpenSSL 3 REQUIRED)

# Vendored modmesh JSON. Compiled as a separate static library so we can
# scope -Werror to our own sources and treat modmesh's warnings as
# diagnostics rather than build breaks.
add_library(modmesh-json STATIC
    third_party/modmesh/cpp/modmesh/serialization/SerializableItem.cpp
)
target_include_directories(modmesh-json SYSTEM PUBLIC
    third_party/modmesh/cpp
)
set_target_properties(modmesh-json PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(MODMESH_BOT_SOURCES
    src/main.cpp
    src/config.cpp
    src/state_store.cpp
)

add_executable(modmesh-bot ${MODMESH_BOT_SOURCES})

target_compile_options(modmesh-bot PRIVATE
    -Wall -Wextra -Werror
)

target_compile_definitions(modmesh-bot PRIVATE
    CPPHTTPLIB_OPENSSL_SUPPORT
    MODMESH_BOT_VERSION="${MODMESH_BOT_VERSION}"
)

target_include_directories(modmesh-bot PRIVATE
    src
)

target_include_directories(modmesh-bot SYSTEM PRIVATE
    third_party/cpp-httplib
)

target_link_libraries(modmesh-bot PRIVATE
    modmesh-json
    OpenSSL::SSL
    OpenSSL::Crypto
)
