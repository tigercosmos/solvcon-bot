#!/bin/sh
# modmesh-bot reviewer wrapper for `claude -p`. The bot pipes the PR
# diff into us on stdin; we wrap it with a "review this" prompt so
# claude produces an actual review rather than treating the diff as a
# bare prompt.
#
# Used by setting in .env:
#   REVIEWER_ARGV='["./scripts/reviewer-claude.sh"]'
#
# Output goes verbatim to the PR comment, prefixed by the bot's
# marker. Keep the prompt short — claude -p limits and the bot's
# MAX_OUTPUT_BYTES (default 60000) both apply.

set -eu

# `claude -p` reads the prompt from stdin. We concatenate our
# preamble + the diff (from our stdin) + a closing marker.
{
    cat <<'EOF'
You are reviewing a GitHub pull request. The full diff is shown
below between BEGIN_DIFF and END_DIFF lines.

Write a concise code review of the diff. Focus on:
- correctness or security bugs
- design or API issues that matter
- missing tests for new behavior

Skip nits unless they hide a real problem. If the diff is fine as-is,
say so plainly in one sentence. Use Markdown. No emojis.

BEGIN_DIFF
EOF
    cat
    cat <<'EOF'
END_DIFF
EOF
} | claude -p
