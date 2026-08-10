// Tests for src/log.* — log line format, control-character sanitization,
// level filter. We capture std::cerr via rdbuf-swap for the duration of
// each assertion.

#include "log.hpp"

#include <algorithm>
#include <iostream>
#include <regex>
#include <sstream>
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
        else { ++g_passed; }                                                 \
    } while (0)

#define EXPECT_EQ(a, b)                                                      \
    do                                                                       \
    {                                                                        \
        auto _a = (a);                                                       \
        auto _b = (b);                                                       \
        if (!(_a == _b))                                                     \
        {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << ": " << #a << " == " << #b                          \
                      << "\n  got: <" << _a << ">"                           \
                      << "\n  want: <" << _b << ">\n";                       \
            ++g_failed;                                                      \
        }                                                                    \
        else { ++g_passed; }                                                 \
    } while (0)

// RAII swap of std::cerr's streambuf into a captured stringstream.
class CerrCapture
{
public:
    CerrCapture() : prev_(std::cerr.rdbuf(ss_.rdbuf())) {}
    ~CerrCapture() { std::cerr.rdbuf(prev_); }
    std::string str() const { return ss_.str(); }
private:
    std::ostringstream ss_;
    std::streambuf * prev_;
};

void test_log_line_shape()
{
    std::string out;
    {
        CerrCapture cap;
        solvcon_bot::log_info("foo", "hello");
        out = cap.str();
    }
    // YYYY-MM-DDTHH:MM:SS.mmmZ INFO foo hello\n
    const std::regex re(
        R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z INFO foo hello\n$)");
    EXPECT(std::regex_match(out, re));
}

void test_log_levels_string()
{
    std::string out;
    {
        CerrCapture cap;
        solvcon_bot::log_warn("c", "warned");
        solvcon_bot::log_error("c", "broken");
        out = cap.str();
    }
    EXPECT(out.find(" WARN c warned") != std::string::npos);
    EXPECT(out.find(" ERROR c broken") != std::string::npos);
}

void test_log_sanitizes_embedded_newline()
{
    std::string out;
    {
        CerrCapture cap;
        solvcon_bot::log_info("c", "line1\nline2");
        out = cap.str();
    }
    // The literal \n should have been escaped to "\\n".
    EXPECT(out.find("line1\\nline2") != std::string::npos);
    // And there should be exactly ONE newline at the end of the record.
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'),
              static_cast<std::ptrdiff_t>(1));
}

void test_log_sanitizes_embedded_cr()
{
    std::string out;
    {
        CerrCapture cap;
        solvcon_bot::log_info("c", "foo\rbar");
        out = cap.str();
    }
    EXPECT(out.find("foo\\rbar") != std::string::npos);
}

void test_log_sanitizes_other_control_chars()
{
    std::string out;
    {
        CerrCapture cap;
        solvcon_bot::log_info("c", std::string("\x01\x02\x1f", 3));
        out = cap.str();
    }
    // Three control bytes → three '?'.
    EXPECT(out.find("???") != std::string::npos);
}

void test_log_keeps_tabs()
{
    std::string out;
    {
        CerrCapture cap;
        solvcon_bot::log_info("c", "a\tb");
        out = cap.str();
    }
    // Tab is treated as printable for legibility.
    EXPECT(out.find("a\tb") != std::string::npos);
}

void test_log_component_also_sanitized()
{
    std::string out;
    {
        CerrCapture cap;
        solvcon_bot::log_info("ev\nil", "msg");
        out = cap.str();
    }
    EXPECT(out.find("ev\\nil") != std::string::npos);
    // Single newline (the line terminator).
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'),
              static_cast<std::ptrdiff_t>(1));
}

} // namespace

int main()
{
    test_log_line_shape();
    test_log_levels_string();
    test_log_sanitizes_embedded_newline();
    test_log_sanitizes_embedded_cr();
    test_log_sanitizes_other_control_chars();
    test_log_keeps_tabs();
    test_log_component_also_sanitized();

    std::cerr << "log tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
