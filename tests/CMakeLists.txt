add_executable(test_github_types test_github_types.cpp)
target_link_libraries(test_github_types PRIVATE modmesh-bot-lib)
target_compile_options(test_github_types PRIVATE -Wall -Wextra -Werror)
add_test(NAME github_types COMMAND test_github_types)

add_executable(test_github_client test_github_client.cpp)
target_link_libraries(test_github_client PRIVATE modmesh-bot-lib)
target_compile_options(test_github_client PRIVATE -Wall -Wextra -Werror)
add_test(NAME github_client COMMAND test_github_client)
