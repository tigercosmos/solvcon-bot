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

# Our library — main.cpp links against this; tests link against this too.
add_library(modmesh-bot-lib STATIC
    src/config.cpp
    src/state_store.cpp
    src/github_types.cpp
    src/github_client.cpp
)
target_include_directories(modmesh-bot-lib PUBLIC src)
target_include_directories(modmesh-bot-lib SYSTEM PUBLIC third_party/cpp-httplib)
target_compile_options(modmesh-bot-lib PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(modmesh-bot-lib PUBLIC
    CPPHTTPLIB_OPENSSL_SUPPORT
    MODMESH_BOT_VERSION="${MODMESH_BOT_VERSION}"
)
target_link_libraries(modmesh-bot-lib PUBLIC
    modmesh-json
    OpenSSL::SSL
    OpenSSL::Crypto
)
set_target_properties(modmesh-bot-lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(modmesh-bot src/main.cpp)

target_compile_options(modmesh-bot PRIVATE -Wall -Wextra -Werror)
target_link_libraries(modmesh-bot PRIVATE modmesh-bot-lib)

enable_testing()
add_subdirectory(tests)
