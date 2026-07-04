#!/usr/bin/env bash
#
# E2E: AgentReviewer preflight. With an AI reviewer kind configured
# but codexmon missing, the bot must exit non-zero AT STARTUP with an
# actionable install hint — and must never touch the PR.

# shellcheck source=scripts/e2e_lib.sh
source "$(dirname "$0")/e2e_lib.sh"

REVIEWER_KIND=claude \
e2e_setup "${1:-.env}"
REVIEWER_KIND=claude
export CODEXMON_BIN="/nonexistent/codexmon-e2e-preflight"

cleanup() {
    set +e
    e2e_stop_bot
    set -e
}
trap cleanup EXIT

e2e_start_bot

# The bot should die within a few seconds (preflight runs before the
# first poll tick). Wait up to 15s for the process to exit.
echo "==> waiting for bot to exit from failed preflight"
DEADLINE=$(( $(date +%s) + 15 ))
while kill -0 "$BOT_PID" 2>/dev/null; do
    if (( $(date +%s) >= DEADLINE )); then
        echo "fatal: bot still running 15s after startup with a missing codexmon" >&2
        tail -20 "$BOT_LOG" >&2
        exit 1
    fi
    sleep 1
done

set +e
wait "$BOT_PID"
BOT_RC=$?
set -e
if [[ "$BOT_RC" -eq 0 ]]; then
    echo "fatal: bot exited 0 despite failed preflight" >&2
    tail -20 "$BOT_LOG" >&2
    exit 1
fi
echo "==> bot exited non-zero ($BOT_RC) as expected"

if ! grep -q "preflight failed" "$BOT_LOG" || \
   ! grep -q "install_codexmon" "$BOT_LOG"; then
    echo "fatal: bot log lacks the preflight failure + install hint:" >&2
    tail -20 "$BOT_LOG" >&2
    exit 1
fi

echo "==> PASS: missing codexmon fails startup with an install hint"
exit 0
