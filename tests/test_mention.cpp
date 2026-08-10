// Tests for src/mention.* — the @-handle matcher and case-insensitive
// login equality.

#include "mention.hpp"

#include <iostream>
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

using solvcon_bot::eq_login;
using solvcon_bot::mention_matches;

// --- mention_matches positive --------------------------------------------

void test_bare_at_handle()
{
    EXPECT(mention_matches("@bot please review", "bot"));
}

void test_handle_with_dash_and_digits()
{
    EXPECT(mention_matches("ping @bot-user-2 here", "bot-user-2"));
}

void test_case_insensitive_handle()
{
    EXPECT(mention_matches("Hey @BOT please help", "bot"));
    EXPECT(mention_matches("Hey @Bot please help", "bot"));
    EXPECT(mention_matches("Hey @bot please help", "BOT"));
}

void test_handle_at_start_and_end_of_string()
{
    EXPECT(mention_matches("@bot", "bot"));
    EXPECT(mention_matches("trailing @bot", "bot"));
}

void test_handle_followed_by_punctuation()
{
    EXPECT(mention_matches("hello, @bot.", "bot"));
    EXPECT(mention_matches("hello, @bot, please review", "bot"));
    EXPECT(mention_matches("@bot's PR is ready", "bot"));
    EXPECT(mention_matches("(@bot)", "bot"));
    EXPECT(mention_matches("[@bot]", "bot"));
}

void test_handle_inside_code_block_still_matches()
{
    // The plan does NOT exclude code blocks; matching is purely on word
    // boundaries.
    const std::string body = "```\n@bot please review\n```";
    EXPECT(mention_matches(body, "bot"));
}

void test_multiple_mentions()
{
    EXPECT(mention_matches("/cc @alice and @bot", "bot"));
}

// --- mention_matches negative --------------------------------------------

void test_no_at_no_match()
{
    EXPECT(!mention_matches("bot please review", "bot"));
}

void test_partial_word_suffix_not_a_match()
{
    EXPECT(!mention_matches("see @botand check", "bot"));
    EXPECT(!mention_matches("see @bot2", "bot"));
    EXPECT(!mention_matches("see @bot-user", "bot"));
}

void test_email_like_left_boundary()
{
    // 'email@bot.com' — left of '@' is 'l', so no word boundary.
    EXPECT(!mention_matches("contact email@bot.com today", "bot"));
}

void test_left_boundary_dash()
{
    // '-' is a username char, so foo-@bot is NOT a match (left is dash).
    EXPECT(!mention_matches("foo-@bot", "bot"));
}

void test_left_boundary_digit()
{
    EXPECT(!mention_matches("9@bot", "bot"));
}

void test_handle_differs_from_body_handle()
{
    EXPECT(!mention_matches("@alice please review", "bot"));
}

void test_empty_handle_never_matches()
{
    EXPECT(!mention_matches("@", ""));
    EXPECT(!mention_matches("@anything", ""));
}

void test_empty_body_never_matches()
{
    EXPECT(!mention_matches("", "bot"));
}

void test_at_with_no_handle_after()
{
    EXPECT(!mention_matches("just a @ symbol", "bot"));
    EXPECT(!mention_matches("@", "bot"));
}

// --- eq_login -----------------------------------------------------------

void test_eq_login_same_case()
{
    EXPECT(eq_login("tigercosmos", "tigercosmos"));
}

void test_eq_login_mixed_case()
{
    EXPECT(eq_login("TigerCosmos", "tigercosmos"));
    EXPECT(eq_login("tigercosmos", "TIGERCOSMOS"));
}

void test_eq_login_length_differs()
{
    EXPECT(!eq_login("alice", "alic"));
    EXPECT(!eq_login("alice", "alices"));
}

void test_eq_login_distinct()
{
    EXPECT(!eq_login("alice", "bob"));
    EXPECT(!eq_login("alice", "ALICEX"));
}

void test_eq_login_empty()
{
    EXPECT(eq_login("", ""));
    EXPECT(!eq_login("", "x"));
    EXPECT(!eq_login("x", ""));
}

} // namespace

int main()
{
    test_bare_at_handle();
    test_handle_with_dash_and_digits();
    test_case_insensitive_handle();
    test_handle_at_start_and_end_of_string();
    test_handle_followed_by_punctuation();
    test_handle_inside_code_block_still_matches();
    test_multiple_mentions();
    test_no_at_no_match();
    test_partial_word_suffix_not_a_match();
    test_email_like_left_boundary();
    test_left_boundary_dash();
    test_left_boundary_digit();
    test_handle_differs_from_body_handle();
    test_empty_handle_never_matches();
    test_empty_body_never_matches();
    test_at_with_no_handle_after();

    test_eq_login_same_case();
    test_eq_login_mixed_case();
    test_eq_login_length_differs();
    test_eq_login_distinct();
    test_eq_login_empty();

    std::cerr << "mention tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
