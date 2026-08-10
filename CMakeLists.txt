cmake_minimum_required(VERSION 3.20)
project(solvcon-bot CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(SOLVCON_BOT_VERSION "0.0.1")

find_package(OpenSSL 3 REQUIRED)

# Vendored solvcon JSON. Compiled as a separate static library so we can
# scope -Werror to our own sources and treat solvcon's warnings as
# diagnostics rather than build breaks.
add_library(solvcon-json STATIC
    third_party/solvcon/cpp/solvcon/serialization/SerializableItem.cpp
)
target_include_directories(solvcon-json SYSTEM PUBLIC
    third_party/solvcon/cpp
)
set_target_properties(solvcon-json PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Our library — main.cpp links against this; tests link against this too.
add_library(solvcon-bot-lib STATIC
    src/log.cpp
    src/config.cpp
    src/state_store.cpp
    src/github_types.cpp
    src/github_client.cpp
    src/subprocess.cpp
    src/reviewer.cpp
    src/reviewer_agent.cpp
    src/mention.cpp
    src/watcher.cpp
)
target_include_directories(solvcon-bot-lib PUBLIC src)
target_include_directories(solvcon-bot-lib SYSTEM PUBLIC third_party/cpp-httplib)
target_compile_options(solvcon-bot-lib PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(solvcon-bot-lib PUBLIC
    CPPHTTPLIB_OPENSSL_SUPPORT
    SOLVCON_BOT_VERSION="${SOLVCON_BOT_VERSION}"
)
target_link_libraries(solvcon-bot-lib PUBLIC
    solvcon-json
    OpenSSL::SSL
    OpenSSL::Crypto
)
set_target_properties(solvcon-bot-lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(solvcon-bot src/main.cpp)

target_compile_options(solvcon-bot PRIVATE -Wall -Wextra -Werror)
target_link_libraries(solvcon-bot PRIVATE solvcon-bot-lib)

# Standalone reviewer driver: feed a diff on stdin (or via a file
# arg) and the configured IReviewer prints its review to stdout.
# Useful for prompt iteration without GitHub round-trips.
# Source lives under tools/, not src/, because it's a developer
# helper rather than part of the bot's runtime.
add_executable(run-reviewer tools/run_reviewer.cpp)
target_compile_options(run-reviewer PRIVATE -Wall -Wextra -Werror)
target_link_libraries(run-reviewer PRIVATE solvcon-bot-lib)

enable_testing()
add_subdirectory(tests)
