#include "github_client.hpp"

#include <httplib.h>
#include <solvcon/serialization/SerializableItem.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace solvcon_bot
{

namespace mm_detail = solvcon::detail;

namespace github_detail
{

std::optional<std::string> parse_link_next(const std::string & link_header)
{
    // Each link section is <URL>; rel="name". Sections are comma-separated;
    // commas inside the URL are not allowed by the GitHub Link header.
    std::size_t pos = 0;
    while (pos < link_header.size())
    {
        const std::size_t lt = link_header.find('<', pos);
        if (lt == std::string::npos) break;
        const std::size_t gt = link_header.find('>', lt + 1);
        if (gt == std::string::npos) break;

        // Look for the end of this section (comma at top level) and inspect
        // the parameters in between for rel="next".
        const std::size_t comma = link_header.find(',', gt);
        const std::size_t section_end = (comma == std::string::npos)
            ? link_header.size() : comma;

        const std::string params = link_header.substr(gt + 1, section_end - gt - 1);
        if (params.find("rel=\"next\"") != std::string::npos
            || params.find("rel=next") != std::string::npos)
        {
            return link_header.substr(lt + 1, gt - lt - 1);
        }

        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return std::nullopt;
}

std::optional<int> parse_retry_after(const std::string & header_value,
                                     std::chrono::system_clock::time_point now)
{
    if (header_value.empty()) return std::nullopt;

    // Trim leading and trailing whitespace.
    std::size_t s = 0;
    std::size_t e = header_value.size();
    while (s < e && std::isspace(static_cast<unsigned char>(header_value[s]))) ++s;
    while (e > s && std::isspace(static_cast<unsigned char>(header_value[e - 1]))) --e;
    const std::string v = header_value.substr(s, e - s);
    if (v.empty()) return std::nullopt;

    // Integer seconds variant.
    bool all_digits = true;
    for (char c : v)
    {
        if (!std::isdigit(static_cast<unsigned char>(c))) { all_digits = false; break; }
    }
    if (all_digits)
    {
        try { return std::stoi(v); }
        catch (...) { return std::nullopt; }
    }

    // HTTP-date variant: RFC 7231 IMF-fixdate, e.g.
    // "Wed, 21 Oct 2015 07:28:00 GMT".
    std::tm tm{};
    std::istringstream iss(v);
    iss.imbue(std::locale::classic());
    iss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    if (iss.fail()) return std::nullopt;

    // timegm() interprets tm as UTC; portable equivalent is mktime + tz offset.
    // We do not assume UTC TZ on the host, so use a portable conversion.
#if defined(__APPLE__) || defined(__linux__)
    std::time_t target = timegm(&tm);
#else
    std::time_t target = std::mktime(&tm);
#endif
    if (target == static_cast<std::time_t>(-1)) return std::nullopt;

    auto target_tp = std::chrono::system_clock::from_time_t(target);
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(target_tp - now).count();
    if (diff < 0) diff = 0;
    if (diff > std::numeric_limits<int>::max()) diff = std::numeric_limits<int>::max();
    return static_cast<int>(diff);
}

std::string json_escape_utf8(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s)
    {
        const unsigned char b = static_cast<unsigned char>(ch);
        switch (b)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (b < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", b);
                out.append(buf);
            }
            else
            {
                // 0x20-0xFF: emit verbatim. JSON permits any UTF-8 byte
                // here, and DEL (0x7F) does not require escaping.
                out.push_back(ch);
            }
        }
    }
    return out;
}

std::string url_path_segment_encode(const std::string & user)
{
    // GitHub usernames are [A-Za-z0-9-], 1-39 chars. We refuse to encode
    // anything outside that set so a caller can't smuggle slashes / dots /
    // queries past us.
    if (user.empty() || user.size() > 39) return "";
    for (char c : user)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '-')) return "";
    }
    return user;
}

} // namespace github_detail

struct GithubClient::Impl
{
    Config cfg;
    httplib::Client cli;
    httplib::Headers default_headers;
    std::string repo_path; // "/repos/{owner}/{repo}"

    // Conditional-request cache for endpoints we opt in to. Keyed by
    // the canonical first-page URL (path+query). On each call we send
    // `If-None-Match: <etag>`; a 304 returns the cached page bodies
    // (re-parsed into the requested item type) without another fetch.
    //
    // Storing the raw page bodies — not the parsed items — lets a
    // single cache slot work across different T template
    // instantiations and avoids std::any acrobatics. The parser is
    // cheap relative to a real network round-trip.
    //
    // The cache is bounded by (1 entry for /pulls) + (1 entry per
    // ever-seen PR for /reviews). That's ~10 KB/entry × hundreds of
    // PRs in the worst case; deemed acceptable for a single-instance
    // long-running bot. No eviction in v1.
    struct CacheEntry
    {
        std::string etag;
        std::vector<std::string> page_bodies; // one body per page, in order
    };
    std::unordered_map<std::string, CacheEntry> conditional_cache;

    // Collaborator lookups are memoized with a TTL. The auto path
    // re-inspects every APPROVED review on every tick (the reviews
    // list is ETag-cached, this lookup is not), so without a cache a
    // single non-collaborator approval on a public repo would cost one
    // authenticated request per tick forever — enough parked
    // approvals could drain the whole API quota. Both outcomes are
    // cached; a freshly added collaborator is picked up after the TTL.
    struct CollabEntry
    {
        bool is_collab;
        std::chrono::steady_clock::time_point expires;
    };
    static constexpr std::chrono::minutes kCollabCacheTtl{15};
    std::unordered_map<std::string, CollabEntry> collab_cache;

    explicit Impl(const Config & c)
        : cfg(c), cli(c.github_api_base_url.c_str())
    {
        cli.set_connection_timeout(cfg.http_connect_timeout_sec, 0);
        cli.set_read_timeout(cfg.http_read_timeout_sec, 0);
        cli.set_write_timeout(cfg.http_write_timeout_sec, 0);
        cli.enable_server_certificate_verification(true);
        cli.set_follow_location(false);
        cli.set_keep_alive(true);

        default_headers = {
            {"Authorization", std::string("Bearer ") + cfg.github_token},
            {"Accept", "application/vnd.github+json"},
            {"User-Agent", std::string("solvcon-bot/") + SOLVCON_BOT_VERSION},
            {"X-GitHub-Api-Version", "2022-11-28"},
        };

        repo_path = "/repos/" + cfg.github_owner + "/" + cfg.github_repo;
    }

    // Returns the value of a response header (case-insensitive match).
    static std::string header(const httplib::Result & res, const char * key)
    {
        for (auto const & [k, v] : res->headers)
        {
            if (k.size() != std::strlen(key)) continue;
            bool eq = true;
            for (std::size_t i = 0; i < k.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(k[i]))
                    != std::tolower(static_cast<unsigned char>(key[i])))
                { eq = false; break; }
            }
            if (eq) return v;
        }
        return "";
    }

    // GitHub returns 403 both for "your token lacks the scope" (fatal —
    // retrying never helps) and for primary/secondary rate limiting
    // (transient). Only the rate-limited variant carries Retry-After or
    // X-RateLimit-Remaining: 0, so that is what we key on. Non-403
    // statuses are never rate-limit 403s, hence the early false.
    static bool is_rate_limited_403(const httplib::Result & res)
    {
        if (res->status != 403) return false;
        return !header(res, "Retry-After").empty()
            || header(res, "X-RateLimit-Remaining") == "0";
    }

    // For api.github.com Link headers we want the path+query only.
    std::string strip_origin(const std::string & url) const
    {
        if (url.compare(0, cfg.github_api_base_url.size(),
                        cfg.github_api_base_url) == 0)
        {
            return url.substr(cfg.github_api_base_url.size());
        }
        // Fall back to the public origin (covers responses that point
        // back at api.github.com even when our base URL differs).
        const std::string pub = "https://api.github.com";
        if (url.compare(0, pub.size(), pub) == 0)
        {
            return url.substr(pub.size());
        }
        return url;
    }

    // Issue a GET with default headers, retry on 5xx/429 honoring
    // Retry-After. Throws GithubError on persistent failure or on a 4xx that
    // does not match the caller-supplied allow_4xx predicate.
    httplib::Result get(const std::string & path,
                        const httplib::Headers & extra = {})
    {
        return request("GET", path, extra, /*body=*/"");
    }

    httplib::Result post(const std::string & path, const std::string & body)
    {
        httplib::Headers extra = {{"Content-Type", "application/json"}};
        return request("POST", path, extra, body);
    }

    httplib::Result request(const char * method,
                            const std::string & path,
                            const httplib::Headers & extra,
                            const std::string & body)
    {
        // GETs are retried on 5xx/429/network failure. POSTs are NOT
        // retried: a 5xx or timeout may have still landed the write, so
        // retrying would risk duplicate comments. Idempotency at the
        // caller layer (marker dedupe before each post attempt) takes
        // care of cross-tick retries.
        const bool is_get = (std::string{method} == "GET");
        const int max_attempts = is_get ? 6 : 1;
        int attempt = 0;
        while (true)
        {
            ++attempt;
            httplib::Headers headers = default_headers;
            for (auto const & h : extra) headers.insert(h);

            httplib::Result res;
            if (is_get)
            {
                res = cli.Get(path.c_str(), headers);
            }
            else if (std::string{method} == "POST")
            {
                res = cli.Post(path.c_str(), headers, body, "application/json");
            }
            else
            {
                throw GithubError(0, std::string("unsupported HTTP method: ") + method);
            }

            if (!res)
            {
                if (attempt >= max_attempts)
                {
                    auto err = res.error();
                    throw GithubError(0, std::string("network error after retries: ")
                                          + httplib::to_string(err));
                }
                sleep_backoff(attempt, /*retry_after=*/"");
                continue;
            }

            const int status = res->status;
            if (status >= 200 && status < 300) return res;

            // Treat a rate-limited 403 like 429; let the truly-fatal
            // (scope-problem) 403 fall through to the caller.
            const bool rate_limited_403 = is_rate_limited_403(res);

            // Retryable: 429 + 5xx + rate-limit 403, GETs only.
            if (is_get && (status == 429 || rate_limited_403
                           || (status >= 500 && status < 600)))
            {
                if (attempt >= max_attempts)
                {
                    // The transient flag matters only for 403: callers
                    // exit the process on a non-transient 403, and a
                    // rate limit must not be allowed to do that.
                    throw GithubError(status,
                        std::string("HTTP ") + std::to_string(status) + " after retries",
                        /*transient=*/rate_limited_403);
                }
                const std::string ra = header(res, "Retry-After");
                sleep_backoff(attempt, ra);
                continue;
            }

            // A rate-limited 403 on a write is just as transient, but
            // writes are never retried here (the request may have
            // landed; a retry risks duplicates). Throw with the
            // transient flag set so callers can't misread it as the
            // fatal scope-403 and exit the process — marker dedupe
            // retries the write on a later tick.
            if (!is_get && rate_limited_403)
            {
                throw GithubError(status,
                    "HTTP 403 (rate limited) on write: " + path,
                    /*transient=*/true);
            }

            // Non-retryable: surface to caller. They can choose to handle
            // 404 specifically (e.g. is_collaborator).
            return res;
        }
    }

    static void sleep_backoff(int attempt, const std::string & retry_after)
    {
        int seconds = 0;
        if (!retry_after.empty())
        {
            auto parsed = github_detail::parse_retry_after(retry_after,
                std::chrono::system_clock::now());
            if (parsed.has_value()) seconds = *parsed;
        }
        if (seconds <= 0)
        {
            seconds = std::min(60, 1 << std::min(attempt, 6)); // 1,2,4,8,16,32,60
        }
        if (seconds > 600) seconds = 600; // cap at 10 min per plan
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
    }

    template <typename T>
    std::vector<T> walk_pages(std::string path)
    {
        std::vector<T> out;
        while (true)
        {
            auto res = get(path);
            if (res->status < 200 || res->status >= 300)
            {
                throw GithubError(res->status,
                    "list endpoint failed: " + path + " HTTP "
                        + std::to_string(res->status));
            }

            std::vector<T> page = parse_array<T>(res->body);
            out.reserve(out.size() + page.size());
            for (auto & item : page) out.emplace_back(std::move(item));

            const std::string link = header(res, "Link");
            auto next = github_detail::parse_link_next(link);
            if (!next.has_value()) break;
            path = strip_origin(*next);
        }
        return out;
    }

    template <typename T>
    static std::vector<T> parse_array(const std::string & body)
    {
        auto node = std::make_unique<mm_detail::JsonNode>(
            mm_detail::JsonType::Array, body);
        std::vector<T> out;
        mm_detail::JsonHelper::from_json_string(node, out);
        return out;
    }

    // Same as walk_pages<T> but uses an ETag-conditional first-page
    // GET so a quiet endpoint costs one round-trip that returns 304.
    //
    // Mechanics:
    //  - On a cache hit, send If-None-Match: <stored etag>.
    //  - 304 -> re-parse the cached page bodies; no further requests.
    //  - 200 -> paginate fresh from this response, replace the cache.
    //  - 2xx other than 200 (rare for list endpoints) -> treat as 200.
    //  - non-2xx, non-304 -> throw GithubError, same as walk_pages.
    //
    // Only the first page is conditioned. Subsequent pages are fetched
    // unconditionally — otherwise a mid-walk 304 could yield a mixed
    // snapshot if the underlying collection changed mid-pagination.
    template <typename T>
    std::vector<T> walk_pages_conditional(std::string path)
    {
        const std::string key = path; // cache key = canonical first-page URL
        httplib::Headers extra;
        bool sent_if_none_match = false;
        if (auto it = conditional_cache.find(key);
            it != conditional_cache.end() && !it->second.etag.empty())
        {
            extra.emplace("If-None-Match", it->second.etag);
            sent_if_none_match = true;
        }

        auto res = request("GET", path, extra, "");
        if (!res)
        {
            throw GithubError(0,
                "list endpoint network error: " + path);
        }

        if (sent_if_none_match && res->status == 304)
        {
            // Cache hit. Re-parse stored page bodies into T.
            const auto & entry = conditional_cache[key];
            std::vector<T> out;
            for (const auto & body : entry.page_bodies)
            {
                auto page = parse_array<T>(body);
                out.reserve(out.size() + page.size());
                for (auto & item : page) out.emplace_back(std::move(item));
            }
            return out;
        }

        if (res->status < 200 || res->status >= 300)
        {
            throw GithubError(res->status,
                "list endpoint failed: " + path + " HTTP "
                    + std::to_string(res->status));
        }

        // 200 (or 2xx). Paginate fresh. We will only cache if the
        // ENTIRE response fit on a single page — see comment at the
        // cache-store step below.
        const std::string first_etag = header(res, "ETag");
        const std::string first_body = res->body;

        std::vector<T> out = parse_array<T>(first_body);
        std::string next_path;
        if (auto link = github_detail::parse_link_next(header(res, "Link"));
            link.has_value())
        {
            next_path = strip_origin(*link);
        }
        const bool multi_page = !next_path.empty();
        while (!next_path.empty())
        {
            // Subsequent pages: unconditional. Cross-page consistency
            // matters when the collection is sorted.
            auto page_res = request("GET", next_path, {}, "");
            if (!page_res || page_res->status < 200 || page_res->status >= 300)
            {
                throw GithubError(
                    page_res ? page_res->status : 0,
                    "list endpoint failed (page " + next_path + ")");
            }
            auto page = parse_array<T>(page_res->body);
            out.reserve(out.size() + page.size());
            for (auto & item : page) out.emplace_back(std::move(item));
            next_path.clear();
            if (auto link = github_detail::parse_link_next(header(page_res, "Link"));
                link.has_value())
            {
                next_path = strip_origin(*link);
            }
        }

        // Cache decision. We cache ONLY when the response fit on a
        // single page. Reason: GitHub's pagination ETags validate one
        // page, not the whole collection. For endpoints that append
        // new items to the LAST page (like /pulls/{n}/reviews, which
        // is sorted by submission order), a new review on page 2
        // doesn't change page 1's ETag — caching a multi-page snapshot
        // would silently miss that new review forever.
        //
        // For single-page responses there is no "page 2 stale" — the
        // first-page ETag fully validates the collection.
        if (!first_etag.empty() && !multi_page)
        {
            CacheEntry fresh;
            fresh.etag = first_etag;
            fresh.page_bodies.push_back(first_body);
            conditional_cache[key] = std::move(fresh);
        }
        else
        {
            // Multi-page response or no ETag: drop any prior cache to
            // avoid a stale single-page snapshot lingering after the
            // collection grew past one page.
            conditional_cache.erase(key);
        }
        return out;
    }
};

GithubClient::GithubClient(const Config & cfg)
    : impl_(std::make_unique<Impl>(cfg)) {}

GithubClient::~GithubClient() = default;

std::vector<PrSummary> GithubClient::list_open_prs()
{
    // sort=updated&direction=desc is a GitHub-documented hint that
    // helps the bot's marker dedupe land on the freshest activity
    // first when the list is fully refreshed. The conditional cache
    // keys on the full path, so changing the URL invalidates any
    // previously cached entry — desired on first deployment.
    return impl_->walk_pages_conditional<PrSummary>(
        impl_->repo_path
        + "/pulls?state=open&sort=updated&direction=desc&per_page=100");
}

std::vector<Review> GithubClient::list_reviews(int pr_number)
{
    return impl_->walk_pages_conditional<Review>(
        impl_->repo_path + "/pulls/" + std::to_string(pr_number)
        + "/reviews?per_page=100");
}

std::vector<IssueComment> GithubClient::list_issue_comments(
    const std::string & since_iso8601)
{
    std::string path = impl_->repo_path
        + "/issues/comments?sort=updated&direction=asc&per_page=100";
    if (!since_iso8601.empty())
    {
        path += "&since=" + since_iso8601;
    }
    return impl_->walk_pages<IssueComment>(path);
}

std::vector<IssueComment> GithubClient::list_pr_comments(int pr_number)
{
    return impl_->walk_pages<IssueComment>(
        impl_->repo_path + "/issues/" + std::to_string(pr_number)
        + "/comments?per_page=100");
}

PrDetail GithubClient::get_issue_detail(int issue_number)
{
    auto res = impl_->get(impl_->repo_path + "/issues/"
                          + std::to_string(issue_number));
    if (res->status < 200 || res->status >= 300)
    {
        throw GithubError(res->status,
            "get_issue_detail failed: HTTP " + std::to_string(res->status));
    }
    PrDetail d;
    d.from_json(res->body);
    return d;
}

bool GithubClient::is_collaborator(const std::string & user)
{
    const std::string seg = github_detail::url_path_segment_encode(user);
    if (seg.empty())
    {
        throw GithubError(0, "rejected collaborator user (invalid chars): " + user);
    }

    const auto now = std::chrono::steady_clock::now();
    if (auto it = impl_->collab_cache.find(seg);
        it != impl_->collab_cache.end() && now < it->second.expires)
    {
        return it->second.is_collab;
    }

    auto res = impl_->get(impl_->repo_path + "/collaborators/" + seg);
    // Only definitive answers are cached; the scope-403 below throws
    // without caching so a fixed token is honored immediately.
    if (res->status == 204 || res->status == 404)
    {
        impl_->collab_cache[seg] = {res->status == 204,
                                    now + Impl::kCollabCacheTtl};
        return res->status == 204;
    }
    // 403 here is a token-scope problem (need read:org / members:read for
    // org repos), not a "not a collaborator" answer. Surface it.
    throw GithubError(res->status,
        "is_collaborator unexpected HTTP " + std::to_string(res->status)
            + " (token may lack scope for org membership lookup)");
}

DiffResult GithubClient::stream_diff(int pr_number)
{
    DiffResult out;
    out.truncated = false;
    const std::size_t cap = impl_->cfg.max_diff_bytes;

    httplib::Headers headers = impl_->default_headers;
    // Replace the Accept header with the diff media type.
    for (auto it = headers.begin(); it != headers.end(); )
    {
        if (it->first == "Accept") it = headers.erase(it);
        else ++it;
    }
    headers.emplace("Accept", "application/vnd.github.diff");

    const std::string path = impl_->repo_path + "/pulls/"
                             + std::to_string(pr_number);

    // Single-shot for now: we don't retry mid-stream. Retries are wrapped
    // around a fresh call.
    int attempt = 0;
    while (true)
    {
        ++attempt;
        out.body.clear();
        out.truncated = false;
        bool we_aborted_at_cap = false;

        auto res = impl_->cli.Get(path.c_str(), headers,
            [&](const char * data, std::size_t len) {
                if (out.body.size() + len > cap)
                {
                    const std::size_t take = (cap > out.body.size())
                        ? (cap - out.body.size()) : 0;
                    out.body.append(data, take);
                    out.truncated = true;
                    we_aborted_at_cap = true;
                    return false;
                }
                out.body.append(data, len);
                return true;
            });

        // cpp-httplib reports our intentional cancel as !res with
        // Error::Canceled. Distinguish that from a real network error by
        // checking our own flag.
        if (we_aborted_at_cap)
        {
            return out;
        }

        if (!res)
        {
            if (attempt >= 6)
            {
                throw GithubError(0, "diff fetch network error: "
                    + httplib::to_string(res.error()));
            }
            Impl::sleep_backoff(attempt, "");
            continue;
        }
        if (res->status >= 200 && res->status < 300) return out;

        // A rate-limited 403 is retryable here exactly as it is in
        // Impl::request(); without this it would fall through to the
        // "diff fetch failed" throw below and read as a fatal 403.
        const bool rate_limited_403 = Impl::is_rate_limited_403(res);
        if (res->status == 429 || rate_limited_403
            || (res->status >= 500 && res->status < 600))
        {
            if (attempt >= 6)
            {
                throw GithubError(res->status,
                    "diff fetch retry exhausted: HTTP "
                        + std::to_string(res->status),
                    /*transient=*/rate_limited_403);
            }
            Impl::sleep_backoff(attempt, Impl::header(res, "Retry-After"));
            continue;
        }
        throw GithubError(res->status,
            "diff fetch failed: HTTP " + std::to_string(res->status));
    }
}

void GithubClient::post_comment(int issue_number, const std::string & body)
{
    // Emit our own JSON for the body so that non-ASCII UTF-8 round-trips
    // intact. See issue.md #3 for why solvcon's escape_string isn't used.
    const std::string payload = "{\"body\":\""
        + github_detail::json_escape_utf8(body) + "\"}";
    auto res = impl_->post(impl_->repo_path + "/issues/"
                           + std::to_string(issue_number) + "/comments",
                           payload);
    if (res->status < 200 || res->status >= 300)
    {
        throw GithubError(res->status,
            "post_comment failed: HTTP " + std::to_string(res->status));
    }
}

} // namespace solvcon_bot
