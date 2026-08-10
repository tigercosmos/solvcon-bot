#!/usr/bin/env bash
#
# E2E: reviewer CLI exits non-zero. The bot should log the failure,
# NOT post a comment, and NOT mark the comment handled — the mention
# should still be there for the next tick to retry. (We don't actually
# wait for a retry to succeed; we just assert that no bot reply appears
# in the verification window.)

# shellcheck source=scripts/e2e_lib.sh
source "$(dirname "$0")/e2e_lib.sh"

# Force the reviewer to fail by configuring the mock to exit non-zero.
REVIEWER_KIND=mock \
REVIEWER_MOCK_EXIT_CODE=17 \
e2e_setup "${1:-.env}"
REVIEWER_KIND=mock
REVIEWER_MOCK_EXIT_CODE=17

MENTION_BODY="solvcon-bot e2e reviewer-fail $NONCE — @${BOT_HANDLE} please review"
e2e_post_mention "$MENTION_BODY"

cleanup() {
    set +e
    e2e_stop_bot
    e2e_user_delete_comment "${MENTION_ID:-}"
    # We don't expect a bot reply, but clean defensively in case the
    # assertion failed.
    e2e_bot_delete_comment "${BOT_REPLY_ID:-}"
    set -e
}
trap cleanup EXIT

e2e_start_bot

EXPECTED_KEY="source=ping pr=${TEST_PR_NUMBER} trigger=${MENTION_ID} -->"

# Wait 25 seconds (about 5 ticks at 5s poll). The bot should attempt
# the dispatch each tick, the reviewer should fail each time, and no
# comment should ever land on the PR.
if ! e2e_assert_no_bot_reply "$EXPECTED_KEY" 25; then
    echo "fatal: bot DID post a reply when the reviewer was failing" >&2
    exit 1
fi

# Bot log should contain the "mock reviewer exited 17" error at least
# once — MockReviewer wraps subprocess failures with its own prefix.
if ! grep -q "mock reviewer exited 17" "$BOT_LOG"; then
    echo "fatal: expected 'mock reviewer exited 17' in bot log; got:" >&2
    tail -30 "$BOT_LOG" >&2
    exit 1
fi

echo "==> PASS: bot logged reviewer failure and posted no comment"
exit 0
