// Pure-function tests for src/github_client.* helpers. The HTTP transport
// itself is exercised in the M7/M8 end-to-end smoke runs.

#include "github_client.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <string>

namespace
{

int g_failed = 0;
int g_passed = 0;

#define EXPECT(expr)                                                         \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << ": " << #expr << "\n";                              \
            ++g_failed;                                                      \
        }                                                                    \
        else                                                                 \
        {                                                                    \
            ++g_passed;                                                      \
        }                                                                    \
    } while (0)

#define EXPECT_EQ(a, b)                                                      \
    do                                                                       \
    {                                                                        \
        auto _a = (a);                                                       \
        auto _b = (b);                                                       \
        if (!(_a == _b))                                                     \
        {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << ": " << #a << " == " << #b << "\n";                 \
            ++g_failed;                                                      \
        }                                                                    \
        else                                                                 \
        {                                                                    \
            ++g_passed;                                                      \
        }                                                                    \
    } while (0)

using solvcon_bot::github_detail::json_escape_utf8;
using solvcon_bot::github_detail::parse_link_next;
using solvcon_bot::github_detail::parse_retry_after;
using solvcon_bot::github_detail::url_path_segment_encode;

// --- Link header rel="next" ----------------------------------------------

void test_link_next_present()
{
    const std::string header =
        "<https://api.github.com/repos/o/r/pulls?page=2>; rel=\"next\", "
        "<https://api.github.com/repos/o/r/pulls?page=5>; rel=\"last\"";
    auto next = parse_link_next(header);
    EXPECT(next.has_value());
    EXPECT_EQ(*next, std::string("https://api.github.com/repos/o/r/pulls?page=2"));
}

void test_link_next_absent()
{
    // Only prev+first; no next.
    const std::string header =
        "<https://api.github.com/x?page=4>; rel=\"prev\", "
        "<https://api.github.com/x?page=1>; rel=\"first\"";
    EXPECT(!parse_link_next(header).has_value());
}

void test_link_next_unquoted()
{
    const std::string header = "<https://api.github.com/x?p=2>; rel=next";
    auto next = parse_link_next(header);
    EXPECT(next.has_value());
    EXPECT_EQ(*next, std::string("https://api.github.com/x?p=2"));
}

void test_link_empty()
{
    EXPECT(!parse_link_next("").has_value());
}

// --- Retry-After ---------------------------------------------------------

void test_retry_after_int()
{
    auto now = std::chrono::system_clock::now();
    auto v = parse_retry_after("30", now);
    EXPECT(v.has_value());
    EXPECT_EQ(*v, 30);
}

void test_retry_after_int_with_whitespace()
{
    auto now = std::chrono::system_clock::now();
    auto v = parse_retry_after("   42  ", now);
    EXPECT(v.has_value());
    EXPECT_EQ(*v, 42);
}

void test_retry_after_http_date_future()
{
    // 2016-02-02 13:48:00 UTC.
    auto now_tp = std::chrono::system_clock::from_time_t(1454420880);
    // Date 60 seconds later.
    auto v = parse_retry_after("Tue, 02 Feb 2016 13:49:00 GMT", now_tp);
    EXPECT(v.has_value());
    EXPECT_EQ(*v, 60);
}

void test_retry_after_http_date_past_clamps_to_zero()
{
    auto now_tp = std::chrono::system_clock::from_time_t(1454420880);
    auto v = parse_retry_after("Tue, 02 Feb 2016 13:47:00 GMT", now_tp);
    EXPECT(v.has_value());
    EXPECT_EQ(*v, 0);
}

void test_retry_after_garbage()
{
    auto now = std::chrono::system_clock::now();
    EXPECT(!parse_retry_after("not a date", now).has_value());
    EXPECT(!parse_retry_after("", now).has_value());
}

// --- url_path_segment_encode ---------------------------------------------

void test_url_segment_accepts_valid_logins()
{
    EXPECT_EQ(url_path_segment_encode("tigercosmos"), std::string("tigercosmos"));
    EXPECT_EQ(url_path_segment_encode("abc-123-XYZ"), std::string("abc-123-XYZ"));
    EXPECT_EQ(url_path_segment_encode("a"), std::string("a"));
    // 39-char max
    EXPECT_EQ(url_path_segment_encode(std::string(39, 'a')), std::string(39, 'a'));
}

void test_url_segment_rejects_invalid_logins()
{
    EXPECT_EQ(url_path_segment_encode(""), std::string());
    EXPECT_EQ(url_path_segment_encode(std::string(40, 'a')), std::string());
    EXPECT_EQ(url_path_segment_encode("alice/../bob"), std::string());
    EXPECT_EQ(url_path_segment_encode("alice@example"), std::string());
    EXPECT_EQ(url_path_segment_encode("alice.bob"), std::string());
    EXPECT_EQ(url_path_segment_encode("alice bob"), std::string());
    EXPECT_EQ(url_path_segment_encode("alice%2fbob"), std::string());
}

// --- url_encode_path ------------------------------------------------------

void test_url_encode_path_passthrough_and_percent()
{
    namespace gd = solvcon_bot::github_detail;
    EXPECT_EQ(gd::url_encode_path("src/reviewer.cpp"),
              std::string("src/reviewer.cpp"));
    EXPECT_EQ(gd::url_encode_path("a-b_c.~d"), std::string("a-b_c.~d"));
    EXPECT_EQ(gd::url_encode_path("dir/na me.txt"),
              std::string("dir/na%20me.txt"));
    EXPECT_EQ(gd::url_encode_path("q?.txt"), std::string("q%3F.txt"));
    EXPECT_EQ(gd::url_encode_path("100%.md"), std::string("100%25.md"));
}

void test_url_encode_path_rejects_traversal_and_control()
{
    namespace gd = solvcon_bot::github_detail;
    EXPECT_EQ(gd::url_encode_path(""), std::string());
    EXPECT_EQ(gd::url_encode_path("/abs/path"), std::string());
    EXPECT_EQ(gd::url_encode_path("a/../b"), std::string());
    EXPECT_EQ(gd::url_encode_path(".."), std::string());
    EXPECT_EQ(gd::url_encode_path("."), std::string());
    EXPECT_EQ(gd::url_encode_path("a/./b"), std::string());
    EXPECT_EQ(gd::url_encode_path("a//b"), std::string());
    EXPECT_EQ(gd::url_encode_path("trailing/"), std::string());
    EXPECT_EQ(gd::url_encode_path(std::string("a\nb")), std::string());
}

// --- json_escape_utf8 ----------------------------------------------------

void test_json_escape_quote_and_backslash()
{
    EXPECT_EQ(json_escape_utf8("he said \"hi\""),
              std::string("he said \\\"hi\\\""));
    EXPECT_EQ(json_escape_utf8("path\\with\\backslashes"),
              std::string("path\\\\with\\\\backslashes"));
}

void test_json_escape_control_chars()
{
    EXPECT_EQ(json_escape_utf8("\n"), std::string("\\n"));
    EXPECT_EQ(json_escape_utf8("\t"), std::string("\\t"));
    EXPECT_EQ(json_escape_utf8(std::string("\x01\x02\x1f", 3)),
              std::string("\\u0001\\u0002\\u001f"));
}

void test_json_escape_utf8_preserved()
{
    // "héllo 漢字 🙂" — every non-ASCII byte must round-trip verbatim. This
    // is the bug case: solvcon's escape_string mangles 0x80+ bytes on
    // signed-char platforms.
    const std::string input = "h\xc3\xa9llo \xe6\xbc\xa2\xe5\xad\x97 \xf0\x9f\x99\x82";
    EXPECT_EQ(json_escape_utf8(input), input);
}

void test_json_escape_del_not_escaped()
{
    // 0x7F (DEL) is printable per JSON spec — we keep it raw.
    EXPECT_EQ(json_escape_utf8(std::string("\x7f", 1)), std::string("\x7f", 1));
}

} // namespace

int main()
{
    test_link_next_present();
    test_link_next_absent();
    test_link_next_unquoted();
    test_link_empty();
    test_retry_after_int();
    test_retry_after_int_with_whitespace();
    test_retry_after_http_date_future();
    test_retry_after_http_date_past_clamps_to_zero();
    test_retry_after_garbage();
    test_url_segment_accepts_valid_logins();
    test_url_segment_rejects_invalid_logins();
    test_url_encode_path_passthrough_and_percent();
    test_url_encode_path_rejects_traversal_and_control();
    test_json_escape_quote_and_backslash();
    test_json_escape_control_chars();
    test_json_escape_utf8_preserved();
    test_json_escape_del_not_escaped();

    std::cerr << "github_client tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
