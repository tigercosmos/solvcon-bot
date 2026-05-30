#!/usr/bin/env bash
#
# E2E: auto path. The bot must post once when a PR receives its first
# APPROVED review.
#
# Setup constraints:
# - $TEST_PR_NUMBER must be open and authored by SOMEONE OTHER than the
#   gh-authed user (GitHub blocks self-approval).
# - The bot's state file must NOT already have $TEST_PR_NUMBER marked
#   as reviewed (we wipe at start).
# - There must be NO existing APPROVED review on the PR, otherwise the
#   bot will pick up the OLD approval on its first tick and ignore our
#   new one. Easiest way: dismiss any prior reviews before running.
#
# Flow:
#   1. Sanity: PR exists, you're not the author.
#   2. Start the bot fresh.
#   3. Submit `gh pr review --approve` as the gh user.
#   4. Wait for the bot to post.
#   5. Verify the marker is the auto-path marker (trigger=first-approval).
#   6. Cleanup: stop bot, delete bot's reply, dismiss the approval if
#      possible (not always supported by gh; we leave it as a no-op).

# shellcheck source=scripts/e2e_lib.sh
source "$(dirname "$0")/e2e_lib.sh"

e2e_setup "${1:-.env}"

# Verify gh user is NOT the PR author (else self-approval is blocked).
pr_author=$(gh api "repos/$GITHUB_REPO/pulls/$TEST_PR_NUMBER" --jq '.user.login')
if [[ "$(e2e_lc "$pr_author")" == "$(e2e_lc "$USER_LOGIN")" ]]; then
    echo "fatal: PR #$TEST_PR_NUMBER was authored by $pr_author, which is the gh user." >&2
    echo "  GitHub blocks self-approval. Use a PR by someone else, or" >&2
    echo "  switch gh to a different account." >&2
    exit 1
fi

EXPECTED_KEY="source=auto pr=${TEST_PR_NUMBER} trigger=first-approval -->"
APPROVAL_BODY="modmesh-bot e2e auto path $NONCE"

# A PR's reviews are a long-lived record. If a prior APPROVED review
# is hanging around, the bot would react to it on its first tick and
# pre-empt our scripted approval — the test would pass for the wrong
# reason (the stale approval) or fail when the bot's reply marker
# doesn't include our nonce. Dismiss any existing APPROVED reviews
# before starting.
dismiss_existing_approvals() {
    local stale
    stale=$(gh api "repos/$GITHUB_REPO/pulls/$TEST_PR_NUMBER/reviews" \
        --jq '[.[] | select(.state == "APPROVED") | .id] | .[]?' || true)
    for rid in $stale; do
        echo "==> dismissing stale APPROVED review id=$rid"
        gh api "repos/$GITHUB_REPO/pulls/$TEST_PR_NUMBER/reviews/$rid/dismissals" \
            --method PUT \
            -f message="dismissed by modmesh-bot e2e setup" \
            --silent >/dev/null 2>&1 || true
    done
}
dismiss_existing_approvals

# Track the review id we create so cleanup can dismiss it too.
CREATED_REVIEW_ID=""

cleanup() {
    set +e
    e2e_stop_bot
    if [[ -n "${BOT_REPLY_ID:-}" ]]; then
        e2e_bot_delete_comment "$BOT_REPLY_ID"
    fi
    if [[ -n "${CREATED_REVIEW_ID:-}" ]]; then
        echo "==> dismissing created APPROVED review id=$CREATED_REVIEW_ID"
        gh api "repos/$GITHUB_REPO/pulls/$TEST_PR_NUMBER/reviews/$CREATED_REVIEW_ID/dismissals" \
            --method PUT \
            -f message="dismissed by modmesh-bot e2e cleanup" \
            --silent >/dev/null 2>&1 || true
    fi
    set -e
}
trap cleanup EXIT

echo "==> ROUND 1: starting bot"
e2e_start_bot

# Give the bot one tick to settle on the current PR state before we
# add the approval. This avoids a race where the bot lists reviews
# before the approval lands and concludes "no APPROVED yet".
sleep 7

echo "==> submitting APPROVED review as $USER_LOGIN"
CREATED_REVIEW_ID=$(gh api "repos/$GITHUB_REPO/pulls/$TEST_PR_NUMBER/reviews" \
    --method POST \
    -f event=APPROVE \
    -f body="$APPROVAL_BODY" \
    --jq '.id')

if ! e2e_find_bot_reply "$EXPECTED_KEY"; then
    echo "fatal: bot never posted an auto-path review" >&2
    exit 1
fi

reply=$(gh api "repos/$GITHUB_REPO/issues/comments/$BOT_REPLY_ID")
reply_body=$(echo "$reply" | jq -r '.body')

if [[ "$reply_body" != *"$EXPECTED_KEY"* ]]; then
    echo "fatal: bot reply body does not contain auto-path marker key" >&2
    exit 1
fi

echo "==> PASS: bot posted an auto-path review for PR #$TEST_PR_NUMBER"
echo "    reply id: $BOT_REPLY_ID"
echo "    head:     $(echo "$reply_body" | head -1)"
exit 0
