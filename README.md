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

## Manual smoke against a real PR (M7/M8)

1. Create a throwaway repo under your account; the bot account must be
   a collaborator. Set `GITHUB_REPO=<you>/<throwaway>`.
2. Get a token (classic, `repo` + `read:org`; or fine-grained with
   the scopes listed above).
3. Set `REVIEWER_ARGV='["/bin/cat"]'` for a no-op reviewer that echoes
   the diff back — this exercises the full pipeline without spending
   AI tokens.
4. Open a PR. Approve it from a separate user → expect the bot to
   post a review on the next poll (auto path).
5. From any collaborator account, leave a comment that
   `@`-mentions the bot's handle → expect a second post (ping path).
6. From a non-collaborator account, leave the same `@`-mention →
   expect the bot to silently mark the comment handled and not post.

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
│   ├── test_config_*.cpp         (covered via main smoke today)
│   ├── test_state_store.cpp      flock, atomic save, cursor semantics
│   ├── test_github_types.cpp     JSON round-trip per type
│   ├── test_github_client.cpp    Link pagination, Retry-After, UTF-8 JSON escape, login encode
│   ├── test_subprocess.cpp       cat echo, timeout, output cap, sanitized env, large stdin
│   ├── test_reviewer.cpp         success, non-zero exit, timeout, empty/missing argv
│   ├── test_mention.cpp          word-boundary matching + login eq
│   └── test_watcher.cpp          fake-driven auto + ping control flow
└── third_party/
    ├── cpp-httplib/httplib.h     vendored, v0.18.5
    └── modmesh/                  submodule, pinned SHA; see issue.md for the local patch
```
