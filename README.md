# modmesh-bot

A lightweight C++23 daemon that watches one GitHub repository's pull
requests and runs an AI code review when either:

1. **Auto path** — the PR receives its first `APPROVED` review (once
   per PR).
2. **Ping path** — a repo collaborator posts a comment that
   `@`-mentions the bot's handle.

The review is produced by an AI CLI (`claude`, `codex`, …) run as a
subprocess on the PR diff. The output is posted back as a PR comment
with a hidden HTML marker for idempotency.

See `plan.md` for the full design; `issue.md` for known limitations.

## Build

Prerequisites:

- C++23 compiler (Clang ≥ 16 or GCC ≥ 13). On macOS, AppleClang from
  current Xcode CLT works.
- CMake ≥ 3.20.
- OpenSSL 3. On macOS: `brew install openssl@3` and pass
  `OPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` to CMake. On Linux:
  the distro's `libssl-dev` is fine.

Submodules:

```bash
git submodule update --init --recursive
```

Configure and build:

```bash
# macOS
OPENSSL_ROOT_DIR=$(brew --prefix openssl@3) cmake -S . -B build
cmake --build build

# Linux
cmake -S . -B build
cmake --build build
```

Run the tests:

```bash
ctest --test-dir build --output-on-failure
```

## Configuration

All configuration comes from environment variables. Required:

| Var | Notes |
|---|---|
| `GITHUB_TOKEN` | Personal access token. Classic needs `repo` (or `public_repo`) plus `read:org` for org repos. Fine-grained needs `pull-requests: write`, `contents: read`, `metadata: read`, and `members: read` for org repos. |
| `GITHUB_REPO` | `owner/name`, e.g. `solvcon/modmesh`. |
| `BOT_HANDLE` | The bot's GitHub username, without the `@`. |
| `REVIEWER_ARGV` | JSON array, e.g. `["claude","-p"]` or `["codex","exec"]`. The first element is the executable; the rest are passed to `execvp`. No shell parsing — quote characters survive verbatim. |

Optional:

| Var | Default | Notes |
|---|---|---|
| `POLL_INTERVAL_SEC` | `30` | Polling cadence in seconds. |
| `STATE_FILE` | `./modmesh-bot.state` | Persistence + lock-file base path (`.lock` and `.tmp` suffixes used during save). |
| `MAX_DIFF_BYTES` | `200000` | Abort diff download once accumulated bytes exceed this; the bot then posts a "diff too big" notice instead of running the reviewer. |
| `MAX_OUTPUT_BYTES` | `60000` | Cap on captured stdout / stderr of the reviewer CLI; further bytes are dropped with a `[truncated]` footer. |
| `SUBPROCESS_TIMEOUT_SEC` | `300` | Hard timeout on the reviewer CLI. On expiry the bot SIGTERMs the child's process group, then SIGKILLs. |
| `HTTP_CONNECT_TIMEOUT_SEC` | `10` | cpp-httplib `set_connection_timeout`. |
| `HTTP_READ_TIMEOUT_SEC` | `30` | cpp-httplib `set_read_timeout`. |
| `HTTP_WRITE_TIMEOUT_SEC` | `30` | cpp-httplib `set_write_timeout`. |
| `MODMESH_BOT_LOG_LEVEL` | `info` | One of `debug`, `info`, `warn`, `error`. |

## Run

```bash
GITHUB_TOKEN=ghp_xxx \
GITHUB_REPO=tigercosmos/modmesh \
BOT_HANDLE=modmesh-bot \
REVIEWER_ARGV='["claude","-p"]' \
./build/modmesh-bot
```

The daemon prints one structured log line per event:

```
2026-05-29T01:06:48.798Z INFO main starting modmesh-bot 0.0.1 repo=… poll=30s
2026-05-29T01:06:48.798Z INFO main state file locked: ./modmesh-bot.state
2026-05-29T01:06:50.011Z INFO watcher posted auto review for PR #42
```

`SIGINT` / `SIGTERM` triggers a clean shutdown. The signal interrupts
the inter-tick sleep within ~1s; if a tick is in flight, the daemon
finishes whatever HTTP request or reviewer subprocess is currently
running before saving state and exiting 0. Worst-case stop latency is
therefore bounded by `SUBPROCESS_TIMEOUT_SEC` (reviewer in flight) and
`HTTP_READ_TIMEOUT_SEC` (a single GitHub request waiting for body
bytes).

Authentication failures (HTTP 401), insufficient scopes (HTTP 403),
and malformed requests (HTTP 422) cause the daemon to log an ERROR and
exit 1 — those won't recover by polling harder.

## Concurrency

The bot acquires an exclusive `flock` on `${STATE_FILE}.lock` at
startup. A second instance against the same state file exits
immediately with `another modmesh-bot instance holds the state lock`.

State is persisted to `${STATE_FILE}` atomically (write `.tmp`,
`fsync`, `rename`, `fsync` the parent directory). The lock is held on
a separate `.lock` file so that the rename does not orphan the lock.

## What the bot persists

`${STATE_FILE}` is one JSON object:

- `reviewed_prs` — set of PR numbers we've already dispatched an auto
  review for. We only ever dispatch one auto review per PR.
- `handled_comments` — set of issue-comment IDs we've already
  classified (either dispatched or deliberately ignored).
- `cursor_updated_at` + `cursor_id` — `(updated_at, id)` high-water
  mark on the repo-wide issue-comments stream. Each polling tick
  fetches `since=cursor_updated_at` and advances the cursor only
  after a comment is durably handled.

## Idempotency

Every comment the bot posts is prefixed with a hidden HTML marker:

```
<!-- modmesh-bot/<ver> source=auto|ping pr=<n> trigger=<comment_id|first-approval> -->
```

Before posting, the bot lists the PR's existing comments and skips if
a matching marker is already present (matched by a
version-agnostic substring, so a bot version bump between "we
posted" and "we checked" still finds the marker). This protects
against duplicate comments if the bot crashes between
`post_comment` and `state.mark_reviewed`.

## Subprocess security

The reviewer is spawned with a sanitized environment containing only
`PATH`, `HOME`, `LANG`, `TERM`. `GITHUB_TOKEN` and every other
inherited variable is explicitly dropped. The PR diff is fed on
stdin; it is never echoed into argv or env. Prompt-injection risk
lives entirely inside the AI CLI's own context.

`SIGPIPE` is ignored at process startup so that a reviewer that
closes its stdin early does not kill the daemon.

## Real-environment e2e smoke

### Identities you need

The bot's design separates two identities:

| Role | Who | What it does |
|---|---|---|
| **Bot account** | Dedicated GitHub user, distinct from your personal one | Holds `GITHUB_TOKEN`, posts review comments. Must be a collaborator on the target repo. |
| **Mentioner / reviewer** | Your own GitHub user (or any other collaborator) | Leaves the `@<bot>` comment that triggers the ping path, or submits the `APPROVED` review that triggers the auto path. |

The auto path additionally needs **a different account from the PR
author** to submit the approving review (GitHub blocks self-approval).
A two-account setup (`tigercosmos` + `modmesh-bot-test`) is the minimum;
three is fully flexible.

### One-time setup

1. **Bot GitHub account.** Sign up a new GitHub user (e.g.
   `modmesh-bot-test`). Add it as a **collaborator** on your fork:
   `https://github.com/tigercosmos/modmesh/settings/access` →
   *Add people*. Accept the invite from the bot account.

2. **Bot token.** Sign in as the bot, generate a PAT:
   - Classic: scopes `repo` (and `read:org` if the repo is in an org)
   - Fine-grained: `pull-requests: write`, `contents: read`,
     `metadata: read`, plus `members: read` for org repos

3. **Mentioner token.** Sign in as yourself, generate a PAT with
   `repo` (or fine-grained with `issues:write` on the target repo).
   This is only used by the e2e script to leave the test comment.

4. **Local config.** Copy and fill in:
   ```bash
   cp scripts/e2e.env.example scripts/e2e.env
   $EDITOR scripts/e2e.env       # fill in tokens, BOT_HANDLE, TEST_PR_NUMBER
   ```
   `scripts/e2e.env` is gitignored.

5. **Test PR.** Either reuse an existing open PR in `$GITHUB_REPO`
   or open a throwaway one (a one-line README tweak on a side branch
   is fine). Set `TEST_PR_NUMBER` in the env file to its number.

### Ping path (automated)

```bash
cmake --build build
./scripts/e2e_ping.sh
```

The script:

1. Checks that the bot is a collaborator and the PR is open.
2. As you, posts a unique `@<bot> please review` comment on the PR.
3. Starts `./build/modmesh-bot` in the background with the bot's
   token + `REVIEWER_ARGV=["/bin/cat"]` (echoes the diff back —
   no AI cost).
4. Polls the PR's comments for a reply authored by the bot whose
   body contains the marker key
   `source=ping pr=<n> trigger=<comment-id> -->` (with a 90s
   timeout — override with `E2E_TIMEOUT_SEC`).
5. Verifies the marker key + author.
6. Cleans up: SIGTERMs the bot, deletes the test comment and the
   bot's reply, removes the state file.

Exit code 0 means the ping path is healthy end-to-end. Exit 1
prints the last 30 lines of the bot's log so you can see what
went wrong.

### Auto path (manual)

`scripts/e2e_auto.md` walks through the five-minute manual check:
run the bot, switch to a second account in the browser, click
**Approve** on the PR, watch the bot's log post a review within one
poll interval. The plan exists as a doc rather than a script
because driving GitHub's review form from CLI requires a third
account (the bot account can't review the bot account's own posts
loop) and gh CLI's `--approve` works fine if you set up `gh auth
login --hostname github.com` as the reviewer account first.

Quick form using `gh`:

```bash
gh auth status                  # confirm you're logged in as REVIEWER
gh pr review "$TEST_PR_NUMBER" --approve \
    --repo "$GITHUB_REPO" \
    --body "automated approve for modmesh-bot e2e auto path"
```

…with the bot running in another terminal. Within one poll interval
the bot's log should print
`INFO watcher posted auto review for PR #<n>` and the PR should
have a new comment from the bot containing
`<!-- modmesh-bot/<ver> source=auto pr=<n> trigger=first-approval -->`.

## Project layout

```
modmesh-bot/
├── CMakeLists.txt
├── README.md
├── plan.md                       full design doc
├── issue.md                      known issues / deferred fixes
├── src/
│   ├── main.cpp                  daemon entry point
│   ├── log.{hpp,cpp}             structured logging (UTC ISO-8601, levels, components)
│   ├── config.{hpp,cpp}          env-var driven config (incl. REVIEWER_ARGV JSON)
│   ├── state_store.{hpp,cpp}     flock'd, atomically rewritten JSON state
│   ├── github_types.{hpp,cpp}    SerializableItem wrappers + small URL helpers
│   ├── github_client.{hpp,cpp}   httplib-backed REST client (pagination, retries, streaming diff)
│   ├── subprocess.{hpp,cpp}      fork+execvp with sanitized env, poll-driven IO, killpg on timeout
│   ├── reviewer.{hpp,cpp}        thin Reviewer that runs cfg.reviewer_argv on a diff
│   ├── mention.{hpp,cpp}         @-mention matcher + case-insensitive login eq
│   └── watcher.{hpp,cpp}         tick() running auto + ping paths against WatcherIo
├── tests/
│   ├── test_config.cpp           env-var matrix, range validation, REVIEWER_ARGV
│   ├── test_log.cpp              line shape, level, control-char sanitization
│   ├── test_state_store.cpp      flock, atomic save, cursor semantics
│   ├── test_github_types.cpp     JSON round-trip per type
│   ├── test_github_client.cpp    Link pagination, Retry-After, UTF-8 JSON escape, login encode
│   ├── test_subprocess.cpp       cat echo, timeout, output cap, sanitized env, large stdin
│   ├── test_reviewer.cpp         success, non-zero exit, timeout, empty/missing argv
│   ├── test_mention.cpp          word-boundary matching + login eq
│   └── test_watcher.cpp          fake-driven auto + ping control flow
├── scripts/
│   ├── e2e.env.example           template for the e2e env file (BOT_TOKEN, USER_TOKEN, …)
│   ├── e2e_ping.sh               automated ping-path e2e against a real PR
│   └── e2e_auto.md               manual checklist for the auto path
└── third_party/
    ├── cpp-httplib/httplib.h     vendored, v0.18.5
    └── modmesh/                  submodule, pinned SHA; see issue.md for the local patch
```
