add_executable(test_github_types test_github_types.cpp)
target_link_libraries(test_github_types PRIVATE modmesh-bot-lib)
target_compile_options(test_github_types PRIVATE -Wall -Wextra -Werror)
add_test(NAME github_types COMMAND test_github_types)

add_executable(test_github_client test_github_client.cpp)
target_link_libraries(test_github_client PRIVATE modmesh-bot-lib)
target_compile_options(test_github_client PRIVATE -Wall -Wextra -Werror)
add_test(NAME github_client COMMAND test_github_client)

add_executable(test_state_store test_state_store.cpp)
target_link_libraries(test_state_store PRIVATE modmesh-bot-lib)
target_compile_options(test_state_store PRIVATE -Wall -Wextra -Werror)
add_test(NAME state_store COMMAND test_state_store)

add_executable(test_subprocess test_subprocess.cpp)
target_link_libraries(test_subprocess PRIVATE modmesh-bot-lib)
target_compile_options(test_subprocess PRIVATE -Wall -Wextra -Werror)
add_test(NAME subprocess COMMAND test_subprocess)

add_executable(test_reviewer test_reviewer.cpp)
target_link_libraries(test_reviewer PRIVATE modmesh-bot-lib)
target_compile_options(test_reviewer PRIVATE -Wall -Wextra -Werror)
add_test(NAME reviewer COMMAND test_reviewer)

add_executable(test_mention test_mention.cpp)
target_link_libraries(test_mention PRIVATE modmesh-bot-lib)
target_compile_options(test_mention PRIVATE -Wall -Wextra -Werror)
add_test(NAME mention COMMAND test_mention)

add_executable(test_watcher test_watcher.cpp)
target_link_libraries(test_watcher PRIVATE modmesh-bot-lib)
target_compile_options(test_watcher PRIVATE -Wall -Wextra -Werror)
add_test(NAME watcher COMMAND test_watcher)

add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE modmesh-bot-lib)
target_compile_options(test_config PRIVATE -Wall -Wextra -Werror)
add_test(NAME config COMMAND test_config)

add_executable(test_log test_log.cpp)
target_link_libraries(test_log PRIVATE modmesh-bot-lib)
target_compile_options(test_log PRIVATE -Wall -Wextra -Werror)
add_test(NAME log COMMAND test_log)
