#!/usr/bin/env bash
#
# End-to-end ping-path smoke for solvcon-bot against a real GitHub repo.
# Posts a uniquely-nonced @<bot> mention as the gh-authed user, starts
# the bot, polls for the bot's marker-tagged reply, and cleans up both
# comments on exit.

# shellcheck source=scripts/e2e_lib.sh
source "$(dirname "$0")/e2e_lib.sh"

e2e_setup "${1:-.env}"

MENTION_BODY="@${BOT_HANDLE} review"
e2e_post_mention "$MENTION_BODY"

cleanup() {
    set +e
    e2e_stop_bot
    e2e_user_delete_comment "${MENTION_ID:-}"
    e2e_bot_delete_comment "${BOT_REPLY_ID:-}"
    set -e
}
trap cleanup EXIT

e2e_start_bot

EXPECTED_KEY="source=ping pr=${TEST_PR_NUMBER} trigger=${MENTION_ID} -->"
if ! e2e_find_bot_reply "$EXPECTED_KEY"; then
    echo "fatal: timed out waiting for bot to post a reply with marker '$EXPECTED_KEY'" >&2
    exit 1
fi

# Verify author + key on the actual reply body.
reply=$(gh api "repos/$GITHUB_REPO/issues/comments/$BOT_REPLY_ID")
reply_body=$(echo "$reply" | jq -r '.body')
reply_login=$(echo "$reply" | jq -r '.user.login')

if [[ "$(e2e_lc "$reply_login")" != "$(e2e_lc "$BOT_HANDLE")" ]]; then
    echo "fatal: reply author '$reply_login' != expected '$BOT_HANDLE'" >&2
    exit 1
fi
if [[ "$reply_body" != *"$EXPECTED_KEY"* ]]; then
    echo "fatal: reply body does not contain marker key '$EXPECTED_KEY'" >&2
    exit 1
fi

echo "==> PASS: bot replied to mention $MENTION_ID with marker '$EXPECTED_KEY'"
echo "    reply id:  $BOT_REPLY_ID"
echo "    head:      $(echo "$reply_body" | head -1)"
exit 0
