#pragma once

#include "config.hpp"
#include "github_types.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace httplib
{
class Client;
struct Result;
} // namespace httplib

namespace solvcon_bot
{

class GithubError : public std::runtime_error
{
public:
    // `transient` marks a failure that is expected to clear on its own —
    // in practice a rate-limited 403. Callers treat 403 as fatal (token
    // scope problem) unless this flag says otherwise, so it must only be
    // set when the response actually carried rate-limit markers.
    GithubError(int status, std::string message, bool transient = false)
        : std::runtime_error(std::move(message))
        , status_(status)
        , transient_(transient) {}
    int status() const { return status_; }
    bool transient() const { return transient_; }
private:
    int status_;
    bool transient_;
};

struct DiffResult
{
    std::string body;
    bool truncated = false;
};

// Result of fetching one repo file at a ref. `found == false` covers
// 404 (path deleted/renamed at that ref) and paths the encoder refuses;
// `truncated` means the file is larger than the configured per-file cap
// and `body` holds only the leading bytes.
struct FileContent
{
    bool found = false;
    bool truncated = false;
    std::string body;
};

// Pure helpers — exposed for unit testing. No HTTP, no I/O.
namespace github_detail
{

// Pull the URL out of the Link header that's tagged rel="next". Returns
// std::nullopt if no such link is present.
std::optional<std::string> parse_link_next(const std::string & link_header);

// Parse a Retry-After header: either an integer number of seconds or a
// HTTP-date. Returns seconds to wait, or std::nullopt if unparseable.
std::optional<int> parse_retry_after(const std::string & header_value,
                                     std::chrono::system_clock::time_point now);

// Decode a percent-encoded GitHub username for the collaborator endpoint.
// Returns "" if input has any disallowed characters; this is a defensive
// check before we put it in a URL.
std::string url_path_segment_encode(const std::string & user);

// Escape a UTF-8 string for embedding inside a JSON string literal.
// Only 0x00-0x1F control bytes and the JSON-mandatory `"` and `\` are
// escaped; bytes 0x20-0xFF are emitted verbatim, which is correct for
// UTF-8 (JSON strings accept any byte 0x20-0xFF as-is). Unlike solvcon's
// escape_string, this does not corrupt non-ASCII UTF-8 sequences on
// platforms where `char` is signed.
std::string json_escape_utf8(std::string_view s);

// Percent-encode a repo-relative file path for use in a URL path. Keeps
// [A-Za-z0-9._~-] and '/' verbatim, encodes everything else. Returns ""
// (refuses) for empty paths, absolute paths, control characters, or any
// "." / ".." segment — those could redirect the request.
std::string url_encode_path(const std::string & path);

} // namespace github_detail

class GithubClient
{
public:
    explicit GithubClient(const Config & cfg);
    ~GithubClient();

    GithubClient(const GithubClient &) = delete;
    GithubClient & operator=(const GithubClient &) = delete;

    // Each method below performs all pagination internally and returns the
    // fully accumulated list. They throw GithubError on non-2xx HTTP responses
    // (with the exception noted on is_collaborator).

    std::vector<PrSummary> list_open_prs();
    std::vector<Review> list_reviews(int pr_number);
    std::vector<IssueComment> list_issue_comments(const std::string & since_iso8601);
    std::vector<IssueComment> list_pr_comments(int pr_number);

    PrDetail get_issue_detail(int issue_number);

    // GET /repos/{o}/{r}/pulls/{n} (JSON) — title, body, head sha for
    // the review payload. Throws GithubError on non-2xx.
    PrInfo get_pr_info(int pr_number);

    // GET /repos/{o}/{r}/contents/{path}?ref={ref} with the raw media
    // type, capped at cfg.max_context_file_bytes. 404 (and refused
    // paths/refs) return found=false instead of throwing; other non-2xx
    // statuses throw GithubError.
    FileContent get_file_at_ref(const std::string & path,
                                const std::string & ref);

    // true iff GET /repos/{o}/{r}/collaborators/{user} returns 204.
    // 404 ⇒ false. Any other non-2xx (including 403) throws GithubError.
    bool is_collaborator(const std::string & user);

    DiffResult stream_diff(int pr_number);

    void post_comment(int issue_number, const std::string & body);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace solvcon_bot
