#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace modmesh_bot
{

struct Config
{
    std::string github_token;
    std::string github_owner;
    std::string github_repo;
    std::string bot_handle;
    std::vector<std::string> reviewer_argv;

    int poll_interval_sec = 30;
    std::string state_file = "./modmesh-bot.state";

    std::size_t max_diff_bytes = 200000;
    std::size_t max_output_bytes = 60000;
    int subprocess_timeout_sec = 300;

    int http_connect_timeout_sec = 10;
    int http_read_timeout_sec = 30;
    int http_write_timeout_sec = 30;

    static Config from_env();
};

} // namespace modmesh_bot
