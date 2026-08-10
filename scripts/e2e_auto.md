# Auto-path e2e: manual checklist

The auto path triggers when a PR receives its first `APPROVED` review.
GitHub disallows self-approval, so this needs a reviewer account
that is **not** the PR author — it's not fully automatable from a
single shell.

## Prereqs (one-time)

1. `.env` at the repo root, filled in from `.env.example` (just the
   bot side: `BOT_HANDLE`, `BOT_PAT`, `GITHUB_REPO`, `REVIEWER_KIND`,
   `STATE_FILE`). No user-side token is needed — gh handles your auth.
2. `gh auth login --hostname github.com` once on your machine, signed
   in as the user who will leave the approving review.
3. The bot account (`$BOT_HANDLE`) must be a collaborator on
   `$GITHUB_REPO`.
4. An open PR in `$GITHUB_REPO` you can approve, whose author is
   NOT the account currently signed in to gh.

## Run

In one terminal, start the bot:

```bash
set -a; source .env; set +a
state="${STATE_FILE:-/tmp/solvcon-bot-e2e-auto.state}"
rm -f "$state" "$state.tmp" "$state.lock"

GITHUB_TOKEN="$BOT_PAT" \
GITHUB_REPO="$GITHUB_REPO" \
BOT_HANDLE="$BOT_HANDLE" \
REVIEWER_KIND="${REVIEWER_KIND:-mock}" \
REVIEWER_MODEL="${REVIEWER_MODEL:-}" \
REVIEWER_EFFORT="${REVIEWER_EFFORT:-}" \
REVIEWER_PROMPT="${REVIEWER_PROMPT:-}" \
REVIEWER_PROMPT_FILE="${REVIEWER_PROMPT_FILE:-}" \
REVIEWER_MOCK_EXIT_CODE="${REVIEWER_MOCK_EXIT_CODE:-}" \
REVIEWER_MOCK_OUTPUT="${REVIEWER_MOCK_OUTPUT:-}" \
REVIEWER_ENV_PASSTHROUGH="${REVIEWER_ENV_PASSTHROUGH:-}" \
POLL_INTERVAL_SEC=10 \
STATE_FILE="$state" \
SOLVCON_BOT_LOG_LEVEL=info \
./build/solvcon-bot 2>&1 | tee /tmp/auto-e2e.log
```

In a second terminal (you must be signed in to gh as a non-author
collaborator):

```bash
set -a; source .env; set +a
gh pr review "$TEST_PR_NUMBER" --approve \
    --repo "$GITHUB_REPO" \
    --body "automated approve for solvcon-bot e2e auto path"
```

Within `POLL_INTERVAL_SEC` of the approval landing the bot's log
should show:

```
… INFO watcher running reviewer for PR #<n> (diff <…> bytes)
… INFO watcher posted auto review for PR #<n>
```

…and the PR should have exactly one new comment authored by
`$BOT_HANDLE`, body starting with:

```
<!-- solvcon-bot/<ver> source=auto pr=<n> trigger=first-approval -->
```

## Idempotency check

Kill the bot (`Ctrl-C`), delete the state file
(`rm "$STATE_FILE"*`), restart it. The bot must:

- list the existing PR comments,
- find its own previous post via the version-agnostic marker key,
- log `auto: marker already present for PR #<n> — skipping dispatch`,
- mark the PR as reviewed in the new state file,
- NOT post a duplicate.

If a second comment appears, the marker-dedupe logic regressed.

## Cleanup

Delete the bot's review comment (via web UI or `gh api -X DELETE
repos/$GITHUB_REPO/issues/comments/<id>`) and the local state file.
