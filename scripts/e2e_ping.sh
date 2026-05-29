#!/usr/bin/env bash
#
# End-to-end ping-path smoke test for modmesh-bot against a real GitHub
# repo. Requires two GitHub identities: one for the mentioner ($USER_*)
# and one for the bot ($BOT_*). See scripts/e2e.env.example for the
# expected env-var matrix.
#
# Flow:
#   1. Sanity-check env + that the binary exists.
#   2. Start fresh: remove any prior state file + lock.
#   3. As USER, leave a unique @BOT_HANDLE comment on TEST_PR_NUMBER.
#   4. Start modmesh-bot in the background with BOT_TOKEN.
#   5. Poll the PR's comments for a bot post that references our test
#      comment id, with a timeout.
#   6. SIGTERM the bot, capture its log.
#   7. Verify: bot comment exists, contains the expected marker key.
#   8. Cleanup: delete both the test mention and the bot's reply.
#
# Re-runnable. Each invocation uses a fresh nonce so previous runs do
# not interfere.

set -euo pipefail

ENV_FILE="${1:-scripts/e2e.env}"
if [[ ! -f "$ENV_FILE" ]]; then
    echo "fatal: env file '$ENV_FILE' not found." >&2
    echo "  copy scripts/e2e.env.example to scripts/e2e.env and fill in." >&2
    exit 2
fi
# shellcheck disable=SC1090
source "$ENV_FILE"

# --- sanity ---------------------------------------------------------------

require() {
    local n="$1"
    if [[ -z "${!n:-}" ]]; then
        echo "fatal: $n is unset in $ENV_FILE" >&2
        exit 2
    fi
}
require GITHUB_REPO
require BOT_HANDLE
require BOT_TOKEN
require USER_LOGIN
require USER_TOKEN
require TEST_PR_NUMBER
require REVIEWER_ARGV
: "${STATE_FILE:=/tmp/modmesh-bot-e2e.state}"

BIN="${BIN:-./build/modmesh-bot}"
if [[ ! -x "$BIN" ]]; then
    echo "fatal: $BIN not found or not executable" >&2
    echo "  build first: cmake --build build" >&2
    exit 2
fi

if ! command -v jq >/dev/null; then
    echo "fatal: jq is required for response parsing" >&2
    exit 2
fi

NONCE=$(date +%s)-$$
echo "==> e2e nonce: $NONCE"

# --- helpers -------------------------------------------------------------

# Args: TOKEN, METHOD, PATH (path starts with /)
gh_api() {
    local token="$1"; local method="$2"; local path="$3"; shift 3
    curl -sS -X "$method" \
        -H "Authorization: Bearer $token" \
        -H "Accept: application/vnd.github+json" \
        -H "X-GitHub-Api-Version: 2022-11-28" \
        -H "User-Agent: modmesh-bot-e2e" \
        "https://api.github.com${path}" \
        "$@"
}

abort() {
    echo "==> ABORT: $*" >&2
    exit 1
}

# --- 0. preflight: confirm bot is a collaborator -------------------------

echo "==> verifying bot ($BOT_HANDLE) is a collaborator on $GITHUB_REPO"
collab_status=$(gh_api "$USER_TOKEN" GET \
    "/repos/$GITHUB_REPO/collaborators/$BOT_HANDLE" \
    -o /dev/null -w '%{http_code}')
if [[ "$collab_status" != "204" ]]; then
    abort "bot collaborator check returned HTTP $collab_status (expected 204)"
fi

echo "==> verifying PR #$TEST_PR_NUMBER is open"
pr_state=$(gh_api "$USER_TOKEN" GET \
    "/repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER" \
    | jq -r '.state // "missing"')
if [[ "$pr_state" != "open" ]]; then
    abort "PR #$TEST_PR_NUMBER is in state '$pr_state' (need open)"
fi

# --- 1. leave the @-mention comment as USER ------------------------------

mention_body="modmesh-bot e2e ping $NONCE — @${BOT_HANDLE} please review"
echo "==> posting mention as $USER_LOGIN: $mention_body"
mention_response=$(gh_api "$USER_TOKEN" POST \
    "/repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER/comments" \
    -d "$(jq -n --arg b "$mention_body" '{body:$b}')")
mention_id=$(echo "$mention_response" | jq -r '.id // empty')
if [[ -z "$mention_id" ]]; then
    abort "could not post mention: $mention_response"
fi
echo "==> mention id: $mention_id"

cleanup() {
    set +e
    if [[ -n "${BOT_PID:-}" ]] && kill -0 "$BOT_PID" 2>/dev/null; then
        echo "==> stopping bot (pid $BOT_PID)"
        kill -TERM "$BOT_PID" 2>/dev/null
        wait "$BOT_PID" 2>/dev/null
    fi
    if [[ -n "${mention_id:-}" ]]; then
        echo "==> deleting mention id $mention_id"
        gh_api "$USER_TOKEN" DELETE \
            "/repos/$GITHUB_REPO/issues/comments/$mention_id" \
            -o /dev/null
    fi
    if [[ -n "${bot_reply_id:-}" ]]; then
        echo "==> deleting bot reply id $bot_reply_id"
        gh_api "$BOT_TOKEN" DELETE \
            "/repos/$GITHUB_REPO/issues/comments/$bot_reply_id" \
            -o /dev/null
    fi
    rm -f "$STATE_FILE" "$STATE_FILE.tmp" "$STATE_FILE.lock"
    set -e
}
trap cleanup EXIT

# --- 2. clean state -------------------------------------------------------

rm -f "$STATE_FILE" "$STATE_FILE.tmp" "$STATE_FILE.lock"

# --- 3. start the bot -----------------------------------------------------

BOT_LOG=/tmp/modmesh-bot-e2e.log
rm -f "$BOT_LOG"

echo "==> starting modmesh-bot ($BIN)"
GITHUB_TOKEN="$BOT_TOKEN" \
GITHUB_REPO="$GITHUB_REPO" \
BOT_HANDLE="$BOT_HANDLE" \
REVIEWER_ARGV="$REVIEWER_ARGV" \
POLL_INTERVAL_SEC=5 \
STATE_FILE="$STATE_FILE" \
MODMESH_BOT_LOG_LEVEL=info \
"$BIN" >"$BOT_LOG" 2>&1 &
BOT_PID=$!
echo "==> bot pid: $BOT_PID"

# --- 4. poll for bot reply -----------------------------------------------

TIMEOUT_SEC="${E2E_TIMEOUT_SEC:-90}"
echo "==> waiting up to ${TIMEOUT_SEC}s for bot to post a reply"

expected_key="source=ping pr=${TEST_PR_NUMBER} trigger=${mention_id} -->"
deadline=$(( $(date +%s) + TIMEOUT_SEC ))
bot_reply_id=""

while (( $(date +%s) < deadline )); do
    sleep 3
    # Refresh comments. Each loop, scan for one authored by the bot
    # that contains our expected marker key.
    if ! kill -0 "$BOT_PID" 2>/dev/null; then
        abort "bot process exited unexpectedly. Log:
$(cat "$BOT_LOG")"
    fi
    page=$(gh_api "$USER_TOKEN" GET \
        "/repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER/comments?per_page=100")
    bot_reply_id=$(echo "$page" \
        | jq -r --arg login "$BOT_HANDLE" --arg key "$expected_key" '
            [ .[]
              | select(.user.login | ascii_downcase == ($login | ascii_downcase))
              | select(.body | contains($key))
              | .id
            ] | first // empty')
    if [[ -n "$bot_reply_id" && "$bot_reply_id" != "null" ]]; then
        echo "==> bot reply id: $bot_reply_id"
        break
    fi
done

if [[ -z "$bot_reply_id" || "$bot_reply_id" == "null" ]]; then
    echo "==> last 30 lines of bot log:"
    tail -30 "$BOT_LOG" >&2 || true
    abort "timed out waiting for bot to post a reply with marker '$expected_key'"
fi

# --- 5. verify -----------------------------------------------------------

reply=$(gh_api "$USER_TOKEN" GET \
    "/repos/$GITHUB_REPO/issues/comments/$bot_reply_id")
reply_body=$(echo "$reply" | jq -r '.body')
reply_login=$(echo "$reply" | jq -r '.user.login')

if [[ "${reply_login,,}" != "${BOT_HANDLE,,}" ]]; then
    abort "reply author '$reply_login' != expected '$BOT_HANDLE'"
fi
if [[ "$reply_body" != *"$expected_key"* ]]; then
    abort "reply body does not contain marker key '$expected_key'"
fi

echo "==> PASS: bot replied to mention $mention_id with marker '$expected_key'"
echo "    reply id: $bot_reply_id"
echo "    reply head: $(echo "$reply_body" | head -1)"
exit 0
