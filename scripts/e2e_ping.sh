#!/usr/bin/env bash
#
# End-to-end ping-path smoke for modmesh-bot against a real GitHub repo.
#
# Two identities are exercised:
#   - the BOT, configured via $BOT_PAT in .env; runs the modmesh-bot
#     binary, posts the AI review back.
#   - YOU, the human running the test, authenticated via `gh auth
#     login`; leaves the @-mention that triggers the bot, and does
#     all read/cleanup against the GitHub API.
#
# Flow:
#   1. Validate env + that the binary exists + that gh is authed.
#   2. Confirm the bot is a collaborator on $GITHUB_REPO and that
#      $TEST_PR_NUMBER is open.
#   3. Start fresh: remove any prior state file + lock.
#   4. As you (gh), leave a uniquely-nonced @$BOT_HANDLE comment on
#      the PR.
#   5. Start modmesh-bot in the background with $BOT_PAT.
#   6. Poll the PR's comments (gh) for a bot post whose body contains
#      the version-agnostic marker key for our trigger comment id.
#   7. SIGTERM the bot, verify the marker + author of the reply.
#   8. Cleanup: delete both the test mention and the bot's reply.
#
# Re-runnable. Each invocation uses a fresh nonce so previous runs
# do not interfere.

set -euo pipefail

ENV_FILE="${1:-.env}"
if [[ ! -f "$ENV_FILE" ]]; then
    echo "fatal: env file '$ENV_FILE' not found." >&2
    echo "  copy .env.example to .env and fill in." >&2
    exit 2
fi
# shellcheck disable=SC1090
set -a; source "$ENV_FILE"; set +a

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
require BOT_PAT
require TEST_PR_NUMBER
require REVIEWER_ARGV
: "${STATE_FILE:=/tmp/modmesh-bot-e2e.state}"

BIN="${BIN:-./build/modmesh-bot}"
if [[ ! -x "$BIN" ]]; then
    echo "fatal: $BIN not found or not executable" >&2
    echo "  build first: cmake --build build" >&2
    exit 2
fi

for cmd in gh jq curl; do
    if ! command -v "$cmd" >/dev/null; then
        echo "fatal: $cmd is required but not on PATH" >&2
        exit 2
    fi
done

if ! gh auth status -h github.com >/dev/null 2>&1; then
    echo "fatal: gh is not authenticated to github.com." >&2
    echo "  run: gh auth login --hostname github.com" >&2
    exit 2
fi

USER_LOGIN=$(gh api user --jq '.login')
echo "==> gh authed as: $USER_LOGIN"

if [[ "${USER_LOGIN,,}" == "${BOT_HANDLE,,}" ]]; then
    echo "fatal: gh is authed as the bot ($BOT_HANDLE)." >&2
    echo "  The mentioner must be a DIFFERENT collaborator." >&2
    exit 2
fi

NONCE="$(date +%s)-$$"
echo "==> e2e nonce: $NONCE"

# --- 0. preflight ---------------------------------------------------------

echo "==> verifying bot ($BOT_HANDLE) is a collaborator on $GITHUB_REPO"
if ! gh api "repos/$GITHUB_REPO/collaborators/$BOT_HANDLE" --silent 2>/dev/null
then
    echo "fatal: $BOT_HANDLE is NOT a collaborator on $GITHUB_REPO." >&2
    echo "  invite the bot account first." >&2
    exit 1
fi

echo "==> verifying PR #$TEST_PR_NUMBER is open"
pr_state=$(gh api "repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER" \
    --jq '.state // "missing"' 2>/dev/null || echo missing)
if [[ "$pr_state" != "open" ]]; then
    echo "fatal: PR #$TEST_PR_NUMBER is in state '$pr_state' (need open)" >&2
    exit 1
fi

# --- 1. leave the @-mention comment as YOU --------------------------------

mention_body="modmesh-bot e2e ping $NONCE — @${BOT_HANDLE} please review"
echo "==> posting mention as $USER_LOGIN: $mention_body"
mention_id=$(gh api "repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER/comments" \
    --method POST \
    -f body="$mention_body" \
    --jq '.id')
if [[ -z "$mention_id" ]]; then
    echo "fatal: could not post mention" >&2
    exit 1
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
        gh api "repos/$GITHUB_REPO/issues/comments/$mention_id" \
            --method DELETE --silent >/dev/null 2>&1
    fi
    if [[ -n "${bot_reply_id:-}" ]]; then
        echo "==> deleting bot reply id $bot_reply_id (using BOT_PAT — the bot owns the comment)"
        # gh CLI doesn't let us override the token per-call without
        # mutating the global login, so use curl + BOT_PAT for this
        # one delete. Users without admin/maintainer on the repo
        # would otherwise leave the bot's comment behind silently.
        curl -sS -o /dev/null -w 'bot-delete: HTTP %{http_code}\n' \
            -X DELETE \
            -H "Authorization: Bearer $BOT_PAT" \
            -H "Accept: application/vnd.github+json" \
            -H "X-GitHub-Api-Version: 2022-11-28" \
            -H "User-Agent: modmesh-bot-e2e" \
            "https://api.github.com/repos/$GITHUB_REPO/issues/comments/$bot_reply_id" \
            || true
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
GITHUB_TOKEN="$BOT_PAT" \
GITHUB_REPO="$GITHUB_REPO" \
BOT_HANDLE="$BOT_HANDLE" \
REVIEWER_ARGV="$REVIEWER_ARGV" \
POLL_INTERVAL_SEC=5 \
STATE_FILE="$STATE_FILE" \
MODMESH_BOT_LOG_LEVEL=info \
"$BIN" >"$BOT_LOG" 2>&1 &
BOT_PID=$!
echo "==> bot pid: $BOT_PID  log: $BOT_LOG"

# --- 4. poll for bot reply -----------------------------------------------

TIMEOUT_SEC="${E2E_TIMEOUT_SEC:-90}"
echo "==> waiting up to ${TIMEOUT_SEC}s for bot to post a reply"

expected_key="source=ping pr=${TEST_PR_NUMBER} trigger=${mention_id} -->"
deadline=$(( $(date +%s) + TIMEOUT_SEC ))
bot_reply_id=""

while (( $(date +%s) < deadline )); do
    sleep 3
    if ! kill -0 "$BOT_PID" 2>/dev/null; then
        echo "==> bot process exited unexpectedly. Log:" >&2
        cat "$BOT_LOG" >&2
        exit 1
    fi
    page=$(gh api "repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER/comments?per_page=100")
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
    echo "==> last 30 lines of bot log:" >&2
    tail -30 "$BOT_LOG" >&2 || true
    echo "fatal: timed out waiting for bot to post a reply with marker '$expected_key'" >&2
    exit 1
fi

# --- 5. verify -----------------------------------------------------------

reply=$(gh api "repos/$GITHUB_REPO/issues/comments/$bot_reply_id")
reply_body=$(echo "$reply" | jq -r '.body')
reply_login=$(echo "$reply" | jq -r '.user.login')

if [[ "${reply_login,,}" != "${BOT_HANDLE,,}" ]]; then
    echo "fatal: reply author '$reply_login' != expected '$BOT_HANDLE'" >&2
    exit 1
fi
if [[ "$reply_body" != *"$expected_key"* ]]; then
    echo "fatal: reply body does not contain marker key '$expected_key'" >&2
    exit 1
fi

echo "==> PASS: bot replied to mention $mention_id with marker '$expected_key'"
echo "    reply id:  $bot_reply_id"
echo "    head:      $(echo "$reply_body" | head -1)"
exit 0
