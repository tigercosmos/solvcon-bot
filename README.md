# modmesh-bot

![CI](https://github.com/tigercosmos/modmesh-bot/actions/workflows/ci.yml/badge.svg)

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

Submodules + local modmesh patches:

```bash
git submodule update --init --recursive
./scripts/apply_modmesh_patches.sh   # applies patches/*.patch into third_party/modmesh
```

`patches/modmesh-serializer-edge-cases.patch` carries two parser fixes
that are not yet upstream in modmesh: empty arrays/objects and
string-state tracking in value scans. The script is idempotent; see
`issue.md` for the bug details and the upstream-PR plan.

Configure and build:

```bash
# macOS
OPENSSL_ROOT_DIR=$(brew --prefix openssl@3) make build
# Linux
make build
```

Run the tests:

```bash
make test
```

The Makefile is the one entry point for both local development and
CI. Everything described below maps to a `make` target.

## Test workflow

```
make                 # build + unit tests (no network, ~10s)
make build           # configure + compile only
make test            # unit tests only
make clean           # rm -rf build/

make e2e             # ALL e2e scenarios against real GitHub (reads .env)
make e2e-ping        # one scenario: ping
make e2e-truncated   # one scenario: MAX_DIFF_BYTES=1 -> "diff skipped" notice
make e2e-failure     # one scenario: reviewer exits non-zero, bot must not post
make e2e-idempotency # one scenario: marker dedupe after state wipe
make e2e-auto        # one scenario: auto path via gh pr review --approve

make all             # build + unit + every e2e
```

| Layer | What runs | Network | Cost | When to run |
|---|---|---|---|---|
| `make` / `make test` | 10 ctest binaries, including an in-process `httplib::Server` for transport tests | none | free | every change, every CI push |
| `make e2e` | scripts/e2e_*.sh — posts comments + approvals on a real GitHub PR, runs the configured reviewer CLI | GitHub API + reviewer CLI | small (free with `/bin/cat`; cents with claude/codex) | before opening a PR, before a release, after touching `src/watcher.cpp` or `src/github_client.cpp` |

### Pre-commit, pre-PR

The Makefile assumes `.env` is filled in (copy from `.env.example`)
before any `make e2e*` target. Useful overrides on the command line:

```bash
# Run only one scenario with a real Claude reviewer:
REVIEWER_KIND=claude REVIEWER_EFFORT=high make e2e-ping

# Keep mentions, replies, and approvals on the PR for manual inspection:
E2E_KEEP_ARTIFACTS=1 make e2e

# Increase the per-scenario wait for a slow AI reviewer:
E2E_TIMEOUT_SEC=300 SUBPROCESS_TIMEOUT_SEC=240 make e2e
```

### CI

Two GitHub Actions workflows:

- **`.github/workflows/ci.yml`** runs `make build && make test` on
  every push to `main` and on every pull request, on Ubuntu and macOS.
  Free, fast, no secrets required.
- **`.github/workflows/e2e.yml`** is `workflow_dispatch` only — a
  manual button in the Actions tab. It needs five repository
  secrets (`E2E_GITHUB_REPO`, `E2E_BOT_HANDLE`, `E2E_BOT_PAT`,
  `E2E_USER_PAT`, `E2E_PR_NUMBER`); see the workflow file's header
  for what each is. The form lets you pick which scenarios to run
  and whether to keep artifacts on the PR.

The CI's build/test path is `make build` + `make test` — identical
to what you ran locally. If `make` works on your machine, CI works.

## Configuration

All configuration comes from environment variables. Required:

| Var | Notes |
|---|---|
| `GITHUB_TOKEN` | Personal access token. Classic needs `repo` (or `public_repo`) plus `read:org` for org repos. Fine-grained needs `pull-requests: write`, `contents: read`, `metadata: read`, and `members: read` for org repos. |
| `GITHUB_REPO` | `owner/name`, e.g. `solvcon/modmesh`. |
| `BOT_HANDLE` | The bot's GitHub username, without the `@`. |
| `REVIEWER_KIND` | One of `mock`, `claude`, `codex` (default `mock`). Selects the reviewer class — see "Reviewer kinds" below. Each kind spawns its own CLI; the bot prepends a built-in review prompt and pipes the diff to its stdin. |
| `REVIEWER_MODEL` | Optional. Passed as `--model NAME` to `claude` / `codex`. Empty falls back to a per-kind default: **`claude-opus-4-8`** for `claude`, **`gpt-5.5`** for `codex`. |
| `REVIEWER_EFFORT` | Optional. For claude, exported as `CLAUDE_EFFORT` in the child env. For codex, passed as `-c reasoning.effort=$EFFORT`. Empty falls back to **`high`** for both kinds. Other values: `minimal`/`low`/`medium`/`xhigh`. |
| `REVIEWER_PROMPT` | Optional literal prompt that replaces the built-in preamble. Mutually exclusive with `REVIEWER_PROMPT_FILE`. |
| `REVIEWER_PROMPT_FILE` | Optional path whose contents replace the built-in preamble. |
| `REVIEWER_MOCK_EXIT_CODE` | Mock-only. Non-zero forces the mock to write to stderr and exit with this code. Used by e2e-failure. |
| `REVIEWER_MOCK_OUTPUT` | Mock-only. If set, the mock prints this instead of echoing the diff. |

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
| `REVIEWER_ENV_PASSTHROUGH` | empty | Comma-separated list of env-var names to pass through to the reviewer subprocess on top of the defaults (`PATH`/`HOME`/`LANG`/`TERM`/`USER`/`LOGNAME`). Use for AI credentials such as `ANTHROPIC_API_KEY` or `OPENAI_API_KEY`. Not needed when the CLI authenticates via files in `$HOME` (codex's `~/.codex`, or claude after `claude /login` writes to the macOS keychain — keychain access already works with the default `USER` passthrough). |
| `MODMESH_BOT_LOG_LEVEL` | `info` | One of `debug`, `info`, `warn`, `error`. |

## Testing the reviewer directly

`make build` also produces `build/run-reviewer`, a standalone driver
that reads a diff from stdin (or a file path) and prints whatever the
configured `IReviewer` would post as a PR comment. Same env vars as
the bot — `REVIEWER_KIND`, `REVIEWER_MODEL`, `REVIEWER_EFFORT`,
`REVIEWER_PROMPT`, `REVIEWER_PROMPT_FILE`, `REVIEWER_MOCK_*`,
`REVIEWER_ENV_PASSTHROUGH`. None of `GITHUB_TOKEN` / `GITHUB_REPO` /
`BOT_HANDLE` is required.

```bash
make build

# Mock — echoes the diff back (free, no AI).
git diff HEAD~3 HEAD | ./build/run-reviewer

# Claude with the defaults (claude-opus-4-8, high effort).
git diff HEAD~3 HEAD | REVIEWER_KIND=claude ./build/run-reviewer

# Try a different model + a custom prompt without running the bot.
git diff main...HEAD \
  | REVIEWER_KIND=claude \
    REVIEWER_MODEL=claude-sonnet-4-6 \
    REVIEWER_PROMPT="Review just for security bugs. One bullet per issue." \
    ./build/run-reviewer

# Codex on a file.
./build/run-reviewer my-experiment.diff   # set REVIEWER_KIND=codex first

# --help shows the env-var matrix.
./build/run-reviewer --help
```

Exit codes: `0` on success (reviewer printed a body to stdout), `1` on
reviewer error (timeout, non-zero exit, etc.), `2` on setup error
(bad env, missing input, etc.). All diagnostics go to stderr so you
can pipe `stdout` straight to a file or another tool. The tool does
*not* enforce `MAX_DIFF_BYTES` — it's a debug driver.

## Reviewer kinds

`REVIEWER_KIND` selects which subclass of `IReviewer` the factory
builds. The selection is fixed at startup. All three kinds share the
same subprocess plumbing — sanitized env, output cap, timeout — and
differ only in which CLI they spawn and how they shape the prompt.

| Kind | What it spawns | Default model / effort | Used for |
|---|---|---|---|
| `mock` | `/bin/cat` (echoes the diff, no AI) or `/bin/sh -c 'exit N'` when `REVIEWER_MOCK_EXIT_CODE` is non-zero | — | Default. Deterministic and free; perfect for e2e of the bot pipeline without spending AI tokens. |
| `claude` | `claude -p --model $REVIEWER_MODEL` with `CLAUDE_EFFORT=$REVIEWER_EFFORT` in the child env | `claude-opus-4-8` / `high` | Real reviews via Anthropic Claude. The bot prepends a built-in review prompt to the diff and pipes the combined buffer to claude's stdin. |
| `codex` | `codex exec --model $REVIEWER_MODEL -c reasoning.effort=$REVIEWER_EFFORT` | `gpt-5.5` / `high` | Real reviews via OpenAI Codex. Same prompt-then-diff stdin shape. |

The built-in review prompt is in `default_review_prompt()` (`src/reviewer.cpp`).
Override per-deployment with `REVIEWER_PROMPT` (literal) or
`REVIEWER_PROMPT_FILE` (path; mutually exclusive with the literal).

Auth lives in the CLI's home directory by default — codex uses
`~/.codex`, claude uses the macOS keychain (which is why `USER` is in
the sanitized env). If your reviewer needs an API key as an env var
instead (e.g. `ANTHROPIC_API_KEY`, `OPENAI_API_KEY`), add the name to
`REVIEWER_ENV_PASSTHROUGH`.

## Run

```bash
GITHUB_TOKEN=ghp_xxx \
GITHUB_REPO=tigercosmos/modmesh \
BOT_HANDLE=modmesh-bot \
REVIEWER_KIND=claude REVIEWER_EFFORT=high \
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

### Identities

Two GitHub identities are exercised:

| Role | Who | Auth used by the test |
|---|---|---|
| **Bot account** | Dedicated GitHub user (e.g. `solvcon-bot`), distinct from yours, collaborator on the target repo. | A PAT stored in `.env` as `BOT_PAT`. The bot binary reads it as `GITHUB_TOKEN`. |
| **You (mentioner / reviewer)** | Your own GitHub user, also a collaborator on the target repo. | `gh auth login --hostname github.com` once — no PAT needs to enter the env file. |

Auto path additionally requires a reviewer that is not the PR's
author (GitHub blocks self-approval). That can be you or any other
collaborator who isn't the PR author.

### One-time setup

1. **Bot account.** Make sure the bot user (e.g. `solvcon-bot`) is a
   **collaborator** on `$GITHUB_REPO`. For
   `tigercosmos/modmesh`: *Settings → Collaborators → Add people*,
   then accept the invite while signed in as the bot.

2. **Bot PAT.** Sign in as the bot, generate a PAT:
   - Classic: scopes `repo` (and `read:org` if the repo is in an org)
   - Fine-grained: `pull-requests: write`, `contents: read`,
     `metadata: read`, plus `members: read` for org repos.

3. **gh CLI for your side.** Once:
   ```bash
   brew install gh                 # macOS; or apt install gh on Debian
   gh auth login --hostname github.com
   ```
   Confirm with `gh auth status` that you're signed in as your own
   account, not the bot.

4. **Local config.** Copy and fill in:
   ```bash
   cp .env.example .env
   $EDITOR .env       # set GITHUB_REPO, BOT_HANDLE, BOT_PAT, TEST_PR_NUMBER
   ```
   `.env` is gitignored.

5. **Test PR.** Open or reuse an open PR in `$GITHUB_REPO`. A
   one-line README tweak on a side branch is fine. Set
   `TEST_PR_NUMBER` in `.env` to its number.

### Ping path (automated)

```bash
cmake --build build
./scripts/e2e_ping.sh
```

Other automated scenarios use the same `.env` and the same shared
helpers in `scripts/e2e_lib.sh`:

```bash
./scripts/e2e_truncated_diff.sh    # MAX_DIFF_BYTES=1, expect skip-notice
./scripts/e2e_reviewer_failure.sh  # reviewer exits non-zero, expect no post
./scripts/e2e_idempotency.sh       # wipe state, restart, expect marker dedupe
./scripts/e2e_auto.sh              # auto-path via `gh pr review --approve`
```

`e2e_auto.sh` requires the test PR to be authored by someone OTHER
than the gh-authed user — GitHub blocks self-approval. The script
fails fast with a clear message if that's not the case.

Set `E2E_KEEP_ARTIFACTS=1` to skip the post-test deletion/dismissal
so the mention, the bot's reply, and any APPROVED review stay on the
PR for manual inspection. The setup-time stale-review dismissal still
runs in `e2e_auto.sh` (otherwise a prior run's APPROVED would pre-empt
the new approval the script submits); dismissals are themselves
preserved as DISMISSED entries in the PR's review history.

The script:

1. Reads `.env`, verifies `gh` is authed as someone other than the bot.
2. Confirms the bot is a collaborator and the PR is open.
3. As you (via `gh api`), posts a uniquely-nonced
   `@<bot> please review` comment on the PR.
4. Starts `./build/modmesh-bot` in the background with `BOT_PAT` as
   `GITHUB_TOKEN`.
5. Polls the PR's comments (via `gh api`) for a reply authored by
   the bot whose body contains the marker key
   `source=ping pr=<n> trigger=<comment-id> -->` (90s timeout —
   override with `E2E_TIMEOUT_SEC`).
6. Verifies the marker key + author.
7. Cleans up via the EXIT trap: SIGTERMs the bot, deletes the test
   comment and the bot's reply, removes the state file.

Exit code 0 means the ping path is healthy end-to-end. Non-zero
prints the last 30 lines of the bot's log so you can see what
went wrong.

### Auto path (manual)

`scripts/e2e_auto.md` walks through the five-minute manual check:
start the bot in one terminal, run

```bash
set -a; source .env; set +a       # load TEST_PR_NUMBER + GITHUB_REPO
gh pr review "$TEST_PR_NUMBER" --approve \
    --repo "$GITHUB_REPO" \
    --body "automated approve for modmesh-bot e2e auto path"
```

in another (you must be signed in to gh as a non-author collaborator),
and within one poll interval the bot's log should print
`INFO watcher posted auto review for PR #<n>`. The PR should have a
new comment from the bot containing
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
│   ├── config.{hpp,cpp}          env-var driven config (incl. REVIEWER_KIND enum)
│   ├── state_store.{hpp,cpp}     flock'd, atomically rewritten JSON state
│   ├── github_types.{hpp,cpp}    SerializableItem wrappers + small URL helpers
│   ├── github_client.{hpp,cpp}   httplib-backed REST client (pagination, retries, streaming diff)
│   ├── subprocess.{hpp,cpp}      fork+execvp with sanitized env, poll-driven IO, killpg on timeout
│   ├── reviewer.{hpp,cpp}        IReviewer abstract interface, factory, MockReviewer
│   ├── reviewer_claude.cpp       ClaudeReviewer: spawns `claude -p`, injects CLAUDE_EFFORT
│   ├── reviewer_codex.cpp        CodexReviewer: spawns `codex exec`, sets reasoning.effort
│   ├── mention.{hpp,cpp}         @-mention matcher + case-insensitive login eq
│   └── watcher.{hpp,cpp}         tick() running auto + ping paths against WatcherIo
├── tests/
│   ├── test_config.cpp           env-var matrix, range validation, REVIEWER_KIND parse
│   ├── test_log.cpp              line shape, level, control-char sanitization
│   ├── test_state_store.cpp      flock, atomic save, cursor semantics
│   ├── test_github_types.cpp     JSON round-trip per type
│   ├── test_github_client.cpp    Link pagination, Retry-After, UTF-8 JSON escape, login encode
│   ├── test_subprocess.cpp       cat echo, timeout, output cap, sanitized env, large stdin
│   ├── test_reviewer.cpp         success, non-zero exit, timeout, empty/missing argv
│   ├── test_mention.cpp          word-boundary matching + login eq
│   └── test_watcher.cpp          fake-driven auto + ping control flow
├── .env.example                  template for the .env at repo root (BOT_PAT, GITHUB_REPO, TEST_PR_NUMBER, …)
├── patches/
│   └── modmesh-serializer-edge-cases.patch  modmesh JSON parser fixes (apply via scripts/apply_modmesh_patches.sh; see issue.md)
├── scripts/
│   ├── apply_modmesh_patches.sh  bootstrap helper: applies patches/*.patch into third_party/modmesh (idempotent)
│   ├── e2e_lib.sh                shared helpers sourced by the e2e_*.sh scripts
│   ├── e2e_ping.sh               ping path: posts an @-mention, waits for the bot's marker-tagged reply
│   ├── e2e_truncated_diff.sh     MAX_DIFF_BYTES=1: bot must post the "(diff exceeds … skipped)" notice, NOT a review
│   ├── e2e_reviewer_failure.sh   reviewer exits non-zero: bot must NOT post and must NOT mark the comment handled
│   ├── e2e_idempotency.sh        wipe state file between two runs; bot must dedupe via marker key and NOT post twice
│   ├── e2e_auto.sh               auto path: `gh pr review --approve` triggers a single bot review post
│   └── e2e_auto.md               manual write-up of the auto-path setup
└── third_party/
    ├── cpp-httplib/httplib.h     vendored, v0.18.5
    └── modmesh/                  submodule, pinned SHA; see issue.md for the local patch
```
