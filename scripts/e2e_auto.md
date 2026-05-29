# Auto-path e2e: manual checklist

The auto path triggers when a PR receives its first `APPROVED` review.
GitHub disallows self-approval, so this path needs a separate
reviewer account — it's not fully automatable from a single shell.

Below is a 5-minute manual checklist that exercises the path end-to-end
against a real PR.

## Prereqs (one-time)

1. Fill in `scripts/e2e.env` (copy from `scripts/e2e.env.example`).
2. Make sure the bot account is a collaborator on `$GITHUB_REPO`.
3. Have an open PR in `$GITHUB_REPO` you can use. Author of the PR
   must be **different** from the bot account — otherwise the bot
   approving its own PR is blocked by GitHub anyway, but the auto
   path stalls on "no reviews".
4. Have a third account (or your own second identity) that has not
   yet approved the PR — that's the reviewer.

## Run

In one terminal:

```bash
source scripts/e2e.env
rm -f "${STATE_FILE:-./modmesh-bot.state}".*  "${STATE_FILE:-./modmesh-bot.state}"

GITHUB_TOKEN="$BOT_TOKEN" \
GITHUB_REPO="$GITHUB_REPO" \
BOT_HANDLE="$BOT_HANDLE" \
REVIEWER_ARGV="$REVIEWER_ARGV" \
POLL_INTERVAL_SEC=10 \
STATE_FILE="${STATE_FILE:-/tmp/modmesh-bot-e2e-auto.state}" \
MODMESH_BOT_LOG_LEVEL=info \
./build/modmesh-bot 2>&1 | tee /tmp/auto-e2e.log
```

In a second terminal (or from the web UI), as the **reviewer
account** (not the PR author, not the bot):

1. Open the PR.
2. Click **Files changed** → **Review changes** → **Approve** →
   **Submit review**.

You should see, within `POLL_INTERVAL_SEC` of the approval landing,
one log line like:

```
… INFO watcher running reviewer for PR #<n> (diff <…> bytes)
… INFO watcher posted auto review for PR #<n>
```

And exactly one new comment on the PR authored by `$BOT_HANDLE`, with
a body that starts with:

```
<!-- modmesh-bot/<ver> source=auto pr=<n> trigger=first-approval -->
```

## Idempotency check

Kill the bot (`Ctrl-C`), delete the state file
(`rm "$STATE_FILE"*`), and start it again. The bot must:

- list the existing PR comments,
- find its own previous post (matched by the version-agnostic marker
  key),
- log `auto: marker already present for PR #<n> — skipping dispatch`,
- mark the PR as reviewed in the new state file,
- NOT post a duplicate.

If a second comment appears, the marker-dedupe logic regressed.

## Cleanup

Delete the bot's review comment from the PR (web UI is fine) and the
state file.
