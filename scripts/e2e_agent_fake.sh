#!/usr/bin/env bash
#
# E2E: the codexmon-backed AgentReviewer path, end to end against a
# real PR, with a FAKE codexmon binary so no AI tokens are spent.
#
# The fake accepts the bot's `doctor` preflight, then for `run` it
# records its argv + stdin, writes a canned review to a result file,
# and prints the status JSON real codexmon would print. The bot must:
#   1. pass preflight,
#   2. dispatch the ping through `codexmon run --agent claude`,
#   3. feed the prompt + PR diff on the fake's stdin,
#   4. read the review body from result_file,
#   5. post it as the PR comment with the ping marker.

# shellcheck source=scripts/e2e_lib.sh
source "$(dirname "$0")/e2e_lib.sh"

REVIEWER_KIND=claude \
e2e_setup "${1:-.env}"
REVIEWER_KIND=claude

# --- fake codexmon ---------------------------------------------------------

FAKE_DIR=$(mktemp -d /tmp/modmesh-bot-e2e-fake-codexmon.XXXXXX)
FAKE_REVIEW="fake-codexmon review $NONCE: the diff looks reviewable."
printf '%s\n' "$FAKE_REVIEW" > "$FAKE_DIR/result.txt"

cat > "$FAKE_DIR/codexmon" <<EOF
#!/bin/sh
# Fake codexmon for e2e. \$1 is the subcommand.
case "\$1" in
    doctor)
        printf '{"agent":"claude","ready":true}\n'
        exit 0
        ;;
esac
# run path: capture argv + stdin, emit the canned status JSON.
printf '%s\n' "\$@" > '$FAKE_DIR/argv.txt'
cat > '$FAKE_DIR/stdin.txt'
printf '{"state":"completed","result_preview":"fake","result_file":"%s"}\n' \
    '$FAKE_DIR/result.txt'
exit 0
EOF
chmod +x "$FAKE_DIR/codexmon"
export CODEXMON_BIN="$FAKE_DIR/codexmon"
echo "==> fake codexmon: $CODEXMON_BIN"

MENTION_BODY="modmesh-bot e2e agent-fake $NONCE — @${BOT_HANDLE} please review"
e2e_post_mention "$MENTION_BODY"

cleanup() {
    set +e
    e2e_stop_bot
    e2e_user_delete_comment "${MENTION_ID:-}"
    e2e_bot_delete_comment "${BOT_REPLY_ID:-}"
    rm -rf "$FAKE_DIR"
    set -e
}
trap cleanup EXIT

e2e_start_bot

EXPECTED_KEY="source=ping pr=${TEST_PR_NUMBER} trigger=${MENTION_ID} -->"

if ! e2e_find_bot_reply "$EXPECTED_KEY"; then
    echo "fatal: bot never posted the agent-path reply" >&2
    exit 1
fi

# The posted body must be the review read from the fake's result_file.
REPLY_BODY=$(gh api "repos/$GITHUB_REPO/issues/comments/$BOT_REPLY_ID" --jq '.body')
if [[ "$REPLY_BODY" != *"$FAKE_REVIEW"* ]]; then
    echo "fatal: bot reply does not contain the fake review body" >&2
    echo "  reply: $REPLY_BODY" >&2
    exit 1
fi

# The fake must have been driven as a monitored codexmon run for the
# claude agent, with the prompt + real PR diff arriving on stdin.
if ! grep -qx -- 'run' "$FAKE_DIR/argv.txt" || \
   ! grep -qx -- '--agent' "$FAKE_DIR/argv.txt" || \
   ! grep -qx -- 'claude' "$FAKE_DIR/argv.txt"; then
    echo "fatal: fake codexmon argv missing run/--agent/claude:" >&2
    cat "$FAKE_DIR/argv.txt" >&2
    exit 1
fi
if ! grep -q 'BEGIN_DIFF' "$FAKE_DIR/stdin.txt" || \
   ! grep -q 'diff --git' "$FAKE_DIR/stdin.txt"; then
    echo "fatal: fake codexmon stdin missing the prompt-wrapped PR diff" >&2
    head -5 "$FAKE_DIR/stdin.txt" >&2
    exit 1
fi

echo "==> PASS: agent path dispatched via codexmon and posted the result_file body"
exit 0
