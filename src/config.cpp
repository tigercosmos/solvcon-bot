#include "config.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
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

std::string lc(const std::string & s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        out.push_back(
            (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c);
    }
    return out;
}

// Read a text file at `path` into a string, refusing inputs larger
// than `max_bytes` so a misconfigured REVIEWER_PROMPT_FILE pointed at
// /dev/zero or a huge log can't OOM the bot at startup.
std::string read_text_file(const std::string & path, std::size_t max_bytes)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
    {
        throw std::runtime_error(
            "REVIEWER_PROMPT_FILE could not be opened: " + path);
    }
    std::string out;
    out.reserve(4096);
    char buf[4096];
    while (ifs.read(buf, sizeof(buf)) || ifs.gcount() > 0)
    {
        const std::streamsize n = ifs.gcount();
        if (out.size() + static_cast<std::size_t>(n) > max_bytes)
        {
            throw std::runtime_error(
                "REVIEWER_PROMPT_FILE exceeds the " + std::to_string(max_bytes)
                + "-byte cap: " + path);
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

// Reviewer effort flows into argv (`-c reasoning.effort=$EFFORT` for
// codex) and into an env var (`CLAUDE_EFFORT` for claude). Both
// channels reject only what looks suspicious — letters-and-dashes
// only, max 32 chars. Bot operator typo: clearer fail-loud than a
// confusing AI-side error or env-injection.
void validate_reviewer_effort(const std::string & v)
{
    if (v.empty()) return;
    if (v.size() > 32)
    {
        throw std::runtime_error(
            "REVIEWER_EFFORT exceeds 32 chars: " + v);
    }
    for (char c : v)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok)
        {
            throw std::runtime_error(
                std::string("REVIEWER_EFFORT contains an invalid character "
                            "(only [A-Za-z0-9_-] allowed): ") + v);
        }
    }
}

} // namespace

ReviewerKind parse_reviewer_kind(const std::string & s)
{
    const std::string n = lc(s);
    if (n == "mock") return ReviewerKind::Mock;
    if (n == "claude") return ReviewerKind::Claude;
    if (n == "codex") return ReviewerKind::Codex;
    throw std::runtime_error(
        std::string("REVIEWER_KIND must be one of mock|claude|codex; got: ") + s);
}

const char * to_string(ReviewerKind k)
{
    switch (k)
    {
    case ReviewerKind::Mock:   return "mock";
    case ReviewerKind::Claude: return "claude";
    case ReviewerKind::Codex:  return "codex";
    }
    return "?";
}

void apply_reviewer_env(Config & cfg)
{
    // Fail-loud if a stale `REVIEWER_ARGV` is carried over from the
    // pre-redesign config. Silently ignoring it would mean the bot
    // defaults to REVIEWER_KIND=mock and posts diff-echo "reviews"
    // until the operator notices.
    if (const char * v = std::getenv("REVIEWER_ARGV"); v != nullptr && *v != '\0')
    {
        throw std::runtime_error(
            "REVIEWER_ARGV has been removed; set REVIEWER_KIND=mock|claude|codex "
            "(plus REVIEWER_MODEL / REVIEWER_EFFORT / REVIEWER_PROMPT as needed). "
            "See .env.example.");
    }

    cfg.reviewer_kind = parse_reviewer_kind(env_or("REVIEWER_KIND", "mock"));
    cfg.reviewer_model = env_or("REVIEWER_MODEL", "");
    cfg.reviewer_effort = env_or("REVIEWER_EFFORT", "");
    validate_reviewer_effort(cfg.reviewer_effort);

    // Prompt: REVIEWER_PROMPT is a literal string; REVIEWER_PROMPT_FILE
    // points at a path whose contents we read. The two are mutually
    // exclusive; an empty value means "use the built-in default in the
    // reviewer class".
    {
        const std::string p = env_or("REVIEWER_PROMPT", "");
        const std::string pf = env_or("REVIEWER_PROMPT_FILE", "");
        if (!p.empty() && !pf.empty())
        {
            throw std::runtime_error(
                "REVIEWER_PROMPT and REVIEWER_PROMPT_FILE are mutually exclusive");
        }
        // Cap prompt file size at 256 KB — large enough for any
        // realistic operator-authored review prompt, small enough
        // that a misconfigured file (/dev/zero, a multi-GB log)
        // can't OOM startup.
        constexpr std::size_t kPromptFileMax = 256 * 1024;
        if (!p.empty()) cfg.reviewer_prompt = p;
        else if (!pf.empty())
            cfg.reviewer_prompt = read_text_file(pf, kPromptFileMax);
    }

    // Mock knobs. Defaults are inert.
    {
        const std::string v = env_or("REVIEWER_MOCK_EXIT_CODE", "");
        if (!v.empty())
        {
            // Mock failure can legitimately be negative-ish (signaled),
            // but we accept 0..255 (POSIX exit code range).
            cfg.reviewer_mock_exit_code = parse_nonneg<int>(
                "REVIEWER_MOCK_EXIT_CODE", v.c_str(), 0, 255);
        }
    }
    cfg.reviewer_mock_output = env_or("REVIEWER_MOCK_OUTPUT", "");

    // Subprocess plumbing that the reviewer depends on. These match
    // the same env names used by the full bot config.
    constexpr int kIntMax = std::numeric_limits<int>::max();
    constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();
    cfg.max_output_bytes = env_size_or("MAX_OUTPUT_BYTES", cfg.max_output_bytes, 1, kSizeMax);
    cfg.subprocess_timeout_sec = env_int_or(
        "SUBPROCESS_TIMEOUT_SEC", cfg.subprocess_timeout_sec, 1, kIntMax);

    if (const char * v = std::getenv("REVIEWER_ENV_PASSTHROUGH");
        v != nullptr && *v != '\0')
    {
        cfg.reviewer_env_passthrough = parse_csv_names(v);
    }
}

Config Config::from_env()
{
    Config cfg;
    cfg.github_token = require_env("GITHUB_TOKEN");
    auto [owner, repo] = split_owner_repo(require_env("GITHUB_REPO"));
    cfg.github_owner = std::move(owner);
    cfg.github_repo = std::move(repo);
    cfg.bot_handle = require_env("BOT_HANDLE");

    apply_reviewer_env(cfg);

    constexpr int kIntMax = std::numeric_limits<int>::max();
    constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

    cfg.poll_interval_sec = env_int_or("POLL_INTERVAL_SEC", cfg.poll_interval_sec, 1, 86400);
    cfg.state_file = env_or("STATE_FILE", cfg.state_file);
    cfg.github_api_base_url = env_or("GITHUB_API_BASE_URL", cfg.github_api_base_url);
    cfg.max_diff_bytes = env_size_or("MAX_DIFF_BYTES", cfg.max_diff_bytes, 1, kSizeMax);
    cfg.http_connect_timeout_sec = env_int_or("HTTP_CONNECT_TIMEOUT_SEC", cfg.http_connect_timeout_sec, 1, kIntMax);
    cfg.http_read_timeout_sec = env_int_or("HTTP_READ_TIMEOUT_SEC", cfg.http_read_timeout_sec, 1, kIntMax);
    cfg.http_write_timeout_sec = env_int_or("HTTP_WRITE_TIMEOUT_SEC", cfg.http_write_timeout_sec, 1, kIntMax);

    return cfg;
}

} // namespace modmesh_bot
