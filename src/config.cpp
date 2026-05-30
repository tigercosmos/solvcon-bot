#include "config.hpp"

#include <modmesh/serialization/SerializableItem.hpp>

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace modmesh_bot
{

namespace
{

std::string require_env(const char * name)
{
    const char * v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        throw std::runtime_error(std::string("required env var not set: ") + name);
    }
    return v;
}

std::string env_or(const char * name, const std::string & fallback)
{
    const char * v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return fallback;
    }
    return v;
}

// Full-string parse with std::from_chars. Returns the parsed value or
// throws if the string is not exactly a non-negative integer in range.
template <typename T>
T parse_nonneg(const char * name, const char * text, T min_value, T max_value)
{
    static_assert(std::is_integral_v<T>);
    const char * first = text;
    const char * last = text;
    while (*last != '\0') ++last;
    if (first == last)
    {
        throw std::runtime_error(std::string("env var is empty: ") + name);
    }
    if (*first == '-' || *first == '+')
    {
        throw std::runtime_error(
            std::string("env var must be a non-negative integer with no sign: ") + name);
    }
    T value{};
    auto res = std::from_chars(first, last, value);
    if (res.ec == std::errc::invalid_argument)
    {
        throw std::runtime_error(std::string("env var is not an integer: ") + name);
    }
    if (res.ec == std::errc::result_out_of_range)
    {
        throw std::runtime_error(std::string("env var is out of range: ") + name);
    }
    if (res.ptr != last)
    {
        throw std::runtime_error(
            std::string("env var has trailing garbage: ") + name);
    }
    if (value < min_value || value > max_value)
    {
        throw std::runtime_error(
            std::string("env var out of allowed range: ") + name);
    }
    return value;
}

int env_int_or(const char * name, int fallback, int min_value, int max_value)
{
    const char * v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    return parse_nonneg<int>(name, v, min_value, max_value);
}

std::size_t env_size_or(const char * name, std::size_t fallback,
                        std::size_t min_value, std::size_t max_value)
{
    const char * v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    return parse_nonneg<std::size_t>(name, v, min_value, max_value);
}

std::vector<std::string> parse_argv_json(const std::string & json)
{
    auto node = std::make_unique<modmesh::detail::JsonNode>(
        modmesh::detail::JsonType::Array, json);
    std::vector<std::string> argv;
    modmesh::detail::JsonHelper::from_json_string(node, argv);
    if (argv.empty())
    {
        throw std::runtime_error("REVIEWER_ARGV must be a non-empty JSON array");
    }
    return argv;
}

// Split a comma-separated, optionally-whitespace-padded list of names
// into a vector. Empty tokens are dropped.
std::vector<std::string> parse_csv_names(const std::string & raw)
{
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= raw.size())
    {
        const std::size_t comma = raw.find(',', pos);
        const std::size_t end = (comma == std::string::npos) ? raw.size() : comma;
        std::size_t s = pos;
        std::size_t e = end;
        while (s < e && std::isspace(static_cast<unsigned char>(raw[s]))) ++s;
        while (e > s && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
        if (e > s) out.emplace_back(raw.substr(s, e - s));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

std::pair<std::string, std::string> split_owner_repo(const std::string & repo)
{
    auto slash = repo.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= repo.size())
    {
        throw std::runtime_error("GITHUB_REPO must look like 'owner/repo'");
    }
    if (repo.find('/', slash + 1) != std::string::npos)
    {
        throw std::runtime_error("GITHUB_REPO must not contain extra slashes");
    }
    return {repo.substr(0, slash), repo.substr(slash + 1)};
}

} // namespace

Config Config::from_env()
{
    Config cfg;
    cfg.github_token = require_env("GITHUB_TOKEN");
    auto [owner, repo] = split_owner_repo(require_env("GITHUB_REPO"));
    cfg.github_owner = std::move(owner);
    cfg.github_repo = std::move(repo);
    cfg.bot_handle = require_env("BOT_HANDLE");
    cfg.reviewer_argv = parse_argv_json(require_env("REVIEWER_ARGV"));

    constexpr int kIntMax = std::numeric_limits<int>::max();
    constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

    cfg.poll_interval_sec = env_int_or("POLL_INTERVAL_SEC", cfg.poll_interval_sec, 1, 86400);
    cfg.state_file = env_or("STATE_FILE", cfg.state_file);
    cfg.github_api_base_url = env_or("GITHUB_API_BASE_URL", cfg.github_api_base_url);
    cfg.max_diff_bytes = env_size_or("MAX_DIFF_BYTES", cfg.max_diff_bytes, 1, kSizeMax);
    cfg.max_output_bytes = env_size_or("MAX_OUTPUT_BYTES", cfg.max_output_bytes, 1, kSizeMax);
    cfg.subprocess_timeout_sec = env_int_or("SUBPROCESS_TIMEOUT_SEC", cfg.subprocess_timeout_sec, 1, kIntMax);
    cfg.http_connect_timeout_sec = env_int_or("HTTP_CONNECT_TIMEOUT_SEC", cfg.http_connect_timeout_sec, 1, kIntMax);
    cfg.http_read_timeout_sec = env_int_or("HTTP_READ_TIMEOUT_SEC", cfg.http_read_timeout_sec, 1, kIntMax);
    cfg.http_write_timeout_sec = env_int_or("HTTP_WRITE_TIMEOUT_SEC", cfg.http_write_timeout_sec, 1, kIntMax);

    if (const char * v = std::getenv("REVIEWER_ENV_PASSTHROUGH");
        v != nullptr && *v != '\0')
    {
        cfg.reviewer_env_passthrough = parse_csv_names(v);
    }

    return cfg;
}

} // namespace modmesh_bot
