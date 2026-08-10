#!/usr/bin/env bash
#
# E2E: PR diff exceeds MAX_DIFF_BYTES. The bot should NOT invoke the
# reviewer; instead it should post a "(diff exceeds MAX_DIFF_BYTES …
# — skipped)" notice with the marker. We force this by setting
# MAX_DIFF_BYTES very small (1).

# shellcheck source=scripts/e2e_lib.sh
source "$(dirname "$0")/e2e_lib.sh"

# Override MAX_DIFF_BYTES so even a single-line diff is "truncated".
MAX_DIFF_BYTES=1 e2e_setup "${1:-.env}"
# e2e_setup overrode MAX_DIFF_BYTES from caller; restore (e2e_load_env
# is the saver). We just ensure it's small here:
MAX_DIFF_BYTES=1

MENTION_BODY="solvcon-bot e2e truncated $NONCE — @${BOT_HANDLE} please review"
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
    echo "fatal: timed out waiting for bot to post a (truncated) reply" >&2
    exit 1
fi

reply=$(gh api "repos/$GITHUB_REPO/issues/comments/$BOT_REPLY_ID")
reply_body=$(echo "$reply" | jq -r '.body')

# The notice contains "MAX_DIFF_BYTES=1" and the literal "skipped" word.
if [[ "$reply_body" != *"MAX_DIFF_BYTES"* ]] \
   || [[ "$reply_body" != *"skipped"* ]]; then
    echo "fatal: bot reply does not look like the truncated-diff notice:" >&2
    echo "$reply_body" >&2
    exit 1
fi

# And it should NOT contain the reviewer's output (cat would echo the
# diff, which would have file headers).
if [[ "$reply_body" == *"diff --git"* ]]; then
    echo "fatal: bot reply contains diff content — reviewer should not have run" >&2
    exit 1
fi

echo "==> PASS: bot posted the truncated-diff notice for mention $MENTION_ID"
echo "    body: $reply_body"
exit 0
