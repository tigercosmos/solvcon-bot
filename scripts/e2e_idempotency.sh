#!/usr/bin/env bash
#
# E2E: marker-based idempotency. Steps:
#   1. Run the bot once with a fresh mention; expect one bot reply.
#   2. Stop the bot, WIPE the state file but LEAVE the bot's reply on
#      the PR.
#   3. Restart the bot.
#   4. Expect the bot to find its own previous reply via the
#      version-agnostic marker key, log "marker already present", and
#      NOT post a second reply.
#   5. Assert that there is still exactly one bot reply matching this
#      run's marker key.
#
# This is the recovery path: state file lost, but the PR's comment
# history is the source of truth via the marker.

# shellcheck source=scripts/e2e_lib.sh
source "$(dirname "$0")/e2e_lib.sh"

e2e_setup "${1:-.env}"

MENTION_BODY="modmesh-bot e2e idempotency $NONCE — @${BOT_HANDLE} please review"
e2e_post_mention "$MENTION_BODY"

cleanup() {
    set +e
    e2e_stop_bot
    e2e_user_delete_comment "${MENTION_ID:-}"
    # Delete every bot reply matching our marker key — even duplicates,
    # in case the idempotency assertion failed.
    if [[ -n "${EXPECTED_KEY:-}" ]]; then
        while read -r rid; do
            e2e_bot_delete_comment "$rid"
        done < <(e2e_list_bot_replies "$EXPECTED_KEY")
    fi
    set -e
}
trap cleanup EXIT

EXPECTED_KEY="source=ping pr=${TEST_PR_NUMBER} trigger=${MENTION_ID} -->"

# --- Round 1: bot processes the mention -----------------------------------

echo "==> ROUND 1: fresh state, expect bot to post once"
e2e_start_bot
if ! e2e_find_bot_reply "$EXPECTED_KEY"; then
    echo "fatal: round 1 — bot never posted" >&2
    exit 1
fi
echo "==> round 1 OK: bot reply id $BOT_REPLY_ID"

# --- Round 2: wipe state file, restart, expect dedupe ---------------------

echo "==> ROUND 2: wiping state file, restart, expect dedupe"
e2e_stop_bot
# e2e_stop_bot already removes STATE_FILE*.

e2e_start_bot

# Wait long enough for several ticks to have processed the mention.
# The bot must log "marker already present" and NOT post a duplicate.
ASSERT_WINDOW_SEC="${IDEMPOTENCY_WINDOW_SEC:-25}"
echo "==> watching for ${ASSERT_WINDOW_SEC}s; bot should NOT post a second reply"
sleep "$ASSERT_WINDOW_SEC"

if ! grep -q "marker already present" "$BOT_LOG"; then
    echo "fatal: bot did not log 'marker already present' on round 2" >&2
    echo "  bot log tail:" >&2
    tail -20 "$BOT_LOG" >&2
    exit 1
fi

count=$(e2e_count_bot_replies "$EXPECTED_KEY")
if [[ "$count" != "1" ]]; then
    echo "fatal: expected exactly 1 bot reply with marker key after dedupe; got $count" >&2
    exit 1
fi

echo "==> PASS: marker dedupe worked. One bot reply ($BOT_REPLY_ID) survives."
exit 0
