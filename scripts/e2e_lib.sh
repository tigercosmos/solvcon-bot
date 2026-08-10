#!/usr/bin/env bash
#
# Shared helpers for the e2e_*.sh scripts. Sourced, not executed.
#
# Contract:
#   e2e_setup [env_file]         -- load .env, validate, check bot is
#                                   collaborator, check PR is open.
#                                   Sets globals: GITHUB_REPO,
#                                   BOT_HANDLE, BOT_PAT, TEST_PR_NUMBER,
#                                   STATE_FILE, REVIEWER_KIND,
#                                   REVIEWER_MODEL, REVIEWER_EFFORT,
#                                   REVIEWER_PROMPT, REVIEWER_PROMPT_FILE,
#                                   REVIEWER_MOCK_EXIT_CODE,
#                                   REVIEWER_MOCK_OUTPUT,
#                                   REVIEWER_ENV_PASSTHROUGH,
#                                   USER_LOGIN, NONCE, BIN, BOT_LOG.
#
#   e2e_post_mention BODY        -- post BODY as a comment from the
#                                   gh-authed user. Sets MENTION_ID.
#
#   e2e_start_bot                -- launch ./build/solvcon-bot in the
#                                   background with the env from
#                                   e2e_setup. Sets BOT_PID.
#
#   e2e_stop_bot                 -- SIGTERM the bot, wait, clear state.
#
#   e2e_find_bot_reply KEY [TIMEOUT_SEC]
#                                -- poll PR comments for one authored by
#                                   $BOT_HANDLE whose body contains KEY,
#                                   returns 0 on found, sets BOT_REPLY_ID.
#                                   Returns 1 on timeout.
#
#   e2e_assert_no_bot_reply KEY DURATION_SEC
#                                -- ensure no bot reply appears for
#                                   DURATION_SEC. Returns 0 if absent.
#
#   e2e_delete_comment AUTH_TOKEN ID
#                                -- DELETE comment via gh or curl.
#
#   e2e_lc S                     -- echo S lowercased (bash-3.2 safe).
#
# Globals set by e2e_setup are used by the cleanup trap each scenario
# script registers.

set -euo pipefail

e2e_lc() { echo "$1" | tr '[:upper:]' '[:lower:]'; }

e2e_require() {
    local n="$1"
    if [[ -z "${!n:-}" ]]; then
        echo "fatal: $n is unset in $ENV_FILE" >&2
        exit 2
    fi
}

e2e_load_env() {
    local env_file="${1:-.env}"
    ENV_FILE="$env_file"
    if [[ ! -f "$env_file" ]]; then
        echo "fatal: env file '$env_file' not found." >&2
        echo "  copy .env.example to .env and fill in." >&2
        exit 2
    fi

    # Allow caller-set vars to win over .env. Same set as the
    # bot-side env, plus a few script-level knobs.
    __pre_REVIEWER_KIND="${REVIEWER_KIND:-}"
    __pre_REVIEWER_MODEL="${REVIEWER_MODEL:-}"
    __pre_REVIEWER_EFFORT="${REVIEWER_EFFORT:-}"
    __pre_REVIEWER_PROMPT="${REVIEWER_PROMPT:-}"
    __pre_REVIEWER_PROMPT_FILE="${REVIEWER_PROMPT_FILE:-}"
    __pre_REVIEWER_MOCK_EXIT_CODE="${REVIEWER_MOCK_EXIT_CODE:-}"
    __pre_REVIEWER_MOCK_OUTPUT="${REVIEWER_MOCK_OUTPUT:-}"
    __pre_REVIEWER_ENV_PASSTHROUGH="${REVIEWER_ENV_PASSTHROUGH:-}"
    __pre_E2E_TIMEOUT_SEC="${E2E_TIMEOUT_SEC:-}"
    __pre_POLL_INTERVAL_SEC="${POLL_INTERVAL_SEC:-}"
    __pre_SUBPROCESS_TIMEOUT_SEC="${SUBPROCESS_TIMEOUT_SEC:-}"
    __pre_MAX_DIFF_BYTES="${MAX_DIFF_BYTES:-}"
    __pre_TEST_PR_NUMBER="${TEST_PR_NUMBER:-}"

    # shellcheck disable=SC1090
    set -a; source "$env_file"; set +a

    [[ -n "$__pre_REVIEWER_KIND" ]] && REVIEWER_KIND="$__pre_REVIEWER_KIND"
    [[ -n "$__pre_REVIEWER_MODEL" ]] && REVIEWER_MODEL="$__pre_REVIEWER_MODEL"
    [[ -n "$__pre_REVIEWER_EFFORT" ]] && REVIEWER_EFFORT="$__pre_REVIEWER_EFFORT"
    [[ -n "$__pre_REVIEWER_PROMPT" ]] && REVIEWER_PROMPT="$__pre_REVIEWER_PROMPT"
    [[ -n "$__pre_REVIEWER_PROMPT_FILE" ]] && REVIEWER_PROMPT_FILE="$__pre_REVIEWER_PROMPT_FILE"
    [[ -n "$__pre_REVIEWER_MOCK_EXIT_CODE" ]] && REVIEWER_MOCK_EXIT_CODE="$__pre_REVIEWER_MOCK_EXIT_CODE"
    [[ -n "$__pre_REVIEWER_MOCK_OUTPUT" ]] && REVIEWER_MOCK_OUTPUT="$__pre_REVIEWER_MOCK_OUTPUT"
    [[ -n "$__pre_REVIEWER_ENV_PASSTHROUGH" ]] && REVIEWER_ENV_PASSTHROUGH="$__pre_REVIEWER_ENV_PASSTHROUGH"
    [[ -n "$__pre_E2E_TIMEOUT_SEC" ]] && E2E_TIMEOUT_SEC="$__pre_E2E_TIMEOUT_SEC"
    [[ -n "$__pre_POLL_INTERVAL_SEC" ]] && POLL_INTERVAL_SEC="$__pre_POLL_INTERVAL_SEC"
    [[ -n "$__pre_SUBPROCESS_TIMEOUT_SEC" ]] && SUBPROCESS_TIMEOUT_SEC="$__pre_SUBPROCESS_TIMEOUT_SEC"
    [[ -n "$__pre_MAX_DIFF_BYTES" ]] && MAX_DIFF_BYTES="$__pre_MAX_DIFF_BYTES"
    [[ -n "$__pre_TEST_PR_NUMBER" ]] && TEST_PR_NUMBER="$__pre_TEST_PR_NUMBER"
    # Final-statement matters: if the last `[[ ... ]] &&` chain evaluated
    # false, the function would return 1, which `set -e` then turns
    # into a script exit. Ensure success explicitly.
    return 0
}

e2e_setup() {
    e2e_load_env "${1:-.env}"

    e2e_require GITHUB_REPO
    e2e_require BOT_HANDLE
    e2e_require BOT_PAT
    e2e_require TEST_PR_NUMBER
    : "${REVIEWER_KIND:=mock}"
    : "${STATE_FILE:=/tmp/solvcon-bot-e2e.state}"

    BIN="${BIN:-./build/solvcon-bot}"
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

    if [[ "$(e2e_lc "$USER_LOGIN")" == "$(e2e_lc "$BOT_HANDLE")" ]]; then
        echo "fatal: gh is authed as the bot ($BOT_HANDLE)." >&2
        echo "  The mentioner must be a DIFFERENT collaborator." >&2
        exit 2
    fi

    NONCE="$(date +%s)-$$"
    echo "==> nonce: $NONCE"

    echo "==> verifying bot ($BOT_HANDLE) is a collaborator on $GITHUB_REPO"
    if ! gh api "repos/$GITHUB_REPO/collaborators/$BOT_HANDLE" --silent 2>/dev/null; then
        echo "fatal: $BOT_HANDLE is NOT a collaborator on $GITHUB_REPO." >&2
        exit 1
    fi

    echo "==> verifying PR #$TEST_PR_NUMBER is open"
    local pr_state
    pr_state=$(gh api "repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER" \
        --jq '.state // "missing"' 2>/dev/null || echo missing)
    if [[ "$pr_state" != "open" ]]; then
        echo "fatal: PR #$TEST_PR_NUMBER is in state '$pr_state' (need open)" >&2
        exit 1
    fi

    BOT_LOG="${BOT_LOG:-/tmp/solvcon-bot-e2e.log}"
    rm -f "$BOT_LOG"
}

e2e_post_mention() {
    local body="$1"
    echo "==> posting mention as $USER_LOGIN: $body"
    MENTION_ID=$(gh api "repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER/comments" \
        --method POST \
        -f body="$body" \
        --jq '.id')
    if [[ -z "$MENTION_ID" ]]; then
        echo "fatal: could not post mention" >&2
        exit 1
    fi
    echo "==> mention id: $MENTION_ID"
}

e2e_start_bot() {
    rm -f "$STATE_FILE" "$STATE_FILE.tmp" "$STATE_FILE.lock"
    echo "==> starting solvcon-bot ($BIN)"
    echo "    REVIEWER_KIND: $REVIEWER_KIND"
    [[ -n "${REVIEWER_MODEL:-}" ]] && echo "    REVIEWER_MODEL: $REVIEWER_MODEL"
    [[ -n "${REVIEWER_EFFORT:-}" ]] && echo "    REVIEWER_EFFORT: $REVIEWER_EFFORT"
    GITHUB_TOKEN="$BOT_PAT" \
    GITHUB_REPO="$GITHUB_REPO" \
    BOT_HANDLE="$BOT_HANDLE" \
    REVIEWER_KIND="$REVIEWER_KIND" \
    REVIEWER_MODEL="${REVIEWER_MODEL:-}" \
    REVIEWER_EFFORT="${REVIEWER_EFFORT:-}" \
    REVIEWER_PROMPT="${REVIEWER_PROMPT:-}" \
    REVIEWER_PROMPT_FILE="${REVIEWER_PROMPT_FILE:-}" \
    REVIEWER_MOCK_EXIT_CODE="${REVIEWER_MOCK_EXIT_CODE:-}" \
    REVIEWER_MOCK_OUTPUT="${REVIEWER_MOCK_OUTPUT:-}" \
    REVIEWER_ENV_PASSTHROUGH="${REVIEWER_ENV_PASSTHROUGH:-}" \
    POLL_INTERVAL_SEC="${POLL_INTERVAL_SEC:-5}" \
    SUBPROCESS_TIMEOUT_SEC="${SUBPROCESS_TIMEOUT_SEC:-300}" \
    MAX_DIFF_BYTES="${MAX_DIFF_BYTES:-200000}" \
    STATE_FILE="$STATE_FILE" \
    SOLVCON_BOT_LOG_LEVEL=info \
    "$BIN" >"$BOT_LOG" 2>&1 &
    BOT_PID=$!
    echo "==> bot pid: $BOT_PID  log: $BOT_LOG"
}

e2e_stop_bot() {
    if [[ -n "${BOT_PID:-}" ]] && kill -0 "$BOT_PID" 2>/dev/null; then
        echo "==> stopping bot (pid $BOT_PID)"
        kill -TERM "$BOT_PID" 2>/dev/null
        wait "$BOT_PID" 2>/dev/null || true
    fi
    rm -f "$STATE_FILE" "$STATE_FILE.tmp" "$STATE_FILE.lock"
}

# Returns 0 if a matching reply is found before TIMEOUT_SEC; sets
# BOT_REPLY_ID. Returns 1 (after printing the bot log tail) on timeout.
# A third optional arg BASELINE_IDS (space-separated) filters out reply
# IDs that were already present before the test started — used by
# scenarios where the marker key is fixed across runs (auto path:
# trigger=first-approval is the same forever). Without this filter,
# E2E_KEEP_ARTIFACTS would cause a stale reply to be picked up and
# the test would report PASS without ever exercising the bot.
e2e_find_bot_reply() {
    local key="$1"
    local timeout="${2:-${E2E_TIMEOUT_SEC:-90}}"
    local baseline="${3:-}"
    echo "==> waiting up to ${timeout}s for bot to post a reply containing: $key"
    [[ -n "$baseline" ]] && echo "    excluding baseline ids: $baseline"
    local deadline=$(( $(date +%s) + timeout ))
    BOT_REPLY_ID=""
    while (( $(date +%s) < deadline )); do
        sleep 3
        if ! kill -0 "$BOT_PID" 2>/dev/null; then
            echo "==> bot process exited unexpectedly. Log:" >&2
            cat "$BOT_LOG" >&2
            return 1
        fi
        local page
        page=$(gh api "repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER/comments?per_page=100")
        local candidates
        candidates=$(echo "$page" \
            | jq -r --arg login "$BOT_HANDLE" --arg key "$key" '
                [ .[]
                  | select(.user.login | ascii_downcase == ($login | ascii_downcase))
                  | select(.body | contains($key))
                  | .id
                ] | .[]')
        for cand in $candidates; do
            # Skip baseline IDs (present before the run started).
            if [[ -n "$baseline" ]] && [[ " $baseline " == *" $cand "* ]]; then
                continue
            fi
            BOT_REPLY_ID="$cand"
            echo "==> bot reply id: $BOT_REPLY_ID"
            return 0
        done
    done
    echo "==> last 30 lines of bot log:" >&2
    tail -30 "$BOT_LOG" >&2 || true
    return 1
}

# Asserts that NO matching reply appears within `duration` seconds.
# Useful for "the bot should ignore this".
e2e_assert_no_bot_reply() {
    local key="$1"
    local duration="$2"
    echo "==> asserting no bot reply containing '$key' for ${duration}s"
    local deadline=$(( $(date +%s) + duration ))
    while (( $(date +%s) < deadline )); do
        sleep 3
        local page
        page=$(gh api "repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER/comments?per_page=100")
        local found
        found=$(echo "$page" \
            | jq -r --arg login "$BOT_HANDLE" --arg key "$key" '
                [ .[]
                  | select(.user.login | ascii_downcase == ($login | ascii_downcase))
                  | select(.body | contains($key))
                  | .id
                ] | first // empty')
        if [[ -n "$found" && "$found" != "null" ]]; then
            echo "==> unexpected bot reply id $found containing key '$key'" >&2
            return 1
        fi
    done
    return 0
}

# Count how many bot replies match KEY right now.
e2e_count_bot_replies() {
    local key="$1"
    local page
    page=$(gh api "repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER/comments?per_page=100")
    echo "$page" | jq -r --arg login "$BOT_HANDLE" --arg key "$key" '
        [ .[]
          | select(.user.login | ascii_downcase == ($login | ascii_downcase))
          | select(.body | contains($key))
        ] | length'
}

# List all bot reply IDs matching KEY (one per line).
e2e_list_bot_replies() {
    local key="$1"
    local page
    page=$(gh api "repos/$GITHUB_REPO/issues/$TEST_PR_NUMBER/comments?per_page=100")
    echo "$page" | jq -r --arg login "$BOT_HANDLE" --arg key "$key" '
        .[]
          | select(.user.login | ascii_downcase == ($login | ascii_downcase))
          | select(.body | contains($key))
          | .id'
}

# When E2E_KEEP_ARTIFACTS is truthy (1/true/yes/on), all of the
# *delete*/*dismiss* helpers below short-circuit so the bot reply,
# the mention, and any approvals stay on the PR for inspection.
# Falsy values (0/false/no/off/empty) leave cleanup enabled.
e2e_keep_artifacts() {
    case "${E2E_KEEP_ARTIFACTS:-}" in
        1|true|TRUE|True|yes|YES|Yes|on|ON|On) return 0 ;;
        *) return 1 ;;
    esac
}

# Delete a comment authored by the bot, using BOT_PAT.
e2e_bot_delete_comment() {
    local id="$1"
    [[ -z "$id" || "$id" == "null" ]] && return 0
    if e2e_keep_artifacts; then
        echo "==> keeping bot comment id $id (E2E_KEEP_ARTIFACTS)"
        return 0
    fi
    echo "==> deleting bot comment id $id (BOT_PAT)"
    curl -sS -o /dev/null -w 'bot-delete: HTTP %{http_code}\n' \
        -X DELETE \
        -H "Authorization: Bearer $BOT_PAT" \
        -H "Accept: application/vnd.github+json" \
        -H "X-GitHub-Api-Version: 2022-11-28" \
        -H "User-Agent: solvcon-bot-e2e" \
        "https://api.github.com/repos/$GITHUB_REPO/issues/comments/$id" \
        || true
}

# Delete a comment authored by the mentioner, using gh.
e2e_user_delete_comment() {
    local id="$1"
    [[ -z "$id" || "$id" == "null" ]] && return 0
    if e2e_keep_artifacts; then
        echo "==> keeping user comment id $id (E2E_KEEP_ARTIFACTS)"
        return 0
    fi
    echo "==> deleting user comment id $id"
    gh api "repos/$GITHUB_REPO/issues/comments/$id" \
        --method DELETE --silent >/dev/null 2>&1 || true
}
