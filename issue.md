# Known Issues / Tech Debt

## Status

Issues 1 and 2 below are **fixed upstream**: the submodule is bumped
to `solvcon/solvcon` `b7b934f5`, whose rewritten value scanner handles
empty arrays/objects and tracks string state. The local patch
(`patches/solvcon-serializer-edge-cases.patch`) has been deleted;
`scripts/apply_solvcon_patches.sh` is kept as infrastructure (it
no-ops with an empty `patches/` dir) in case a future workaround is
needed. Their write-ups are kept below for historical context.

Issue 3 (`escape_string` UTF-8 corruption) is still present upstream
at `b7b934f5`; the bot continues to sidestep it with its own escaper.

## 1. ~~solvcon JSON parser rejects empty arrays and empty objects~~ (fixed upstream)

**Where:** `third_party/solvcon/cpp/solvcon/serialization/SerializableItem.cpp`
**Functions:** `JsonNode::parse_array`, `JsonNode::parse_object`

**Bug.** Round-tripping any JSON that contains `[]` or `{}` throws
`Invalid JSON format: invalid value expression: …` or
`Invalid JSON format: missing opening quote for key.` The state machine
never models the "immediately-closing-bracket-after-opening" transition:

- In `parse_array`, after consuming `[`, the state moves to `ObjectValue`.
  `]` then falls through the value-dispatch and trips the
  "not alpha / not number / not `"`" guard.
- In `parse_object`, after consuming `{`, the state moves to `ObjectKey`.
  `}` then trips the "missing opening quote for key" branch.

**Why it matters for solvcon-bot.** The GitHub REST API returns `[]` for
*every* empty list — empty reviews, empty comments, empty PR list,
etc. Any list endpoint with zero items would crash the parser in M3.
The state file also serializes `reviewed_prs`, `handled_comments` as
arrays that are empty on first run, so first-tick save → second-tick
load round-trip also crashes (caught during M1 verification).

**Local workaround (already applied, not committed).** Two small
patches in this checkout's `third_party/solvcon` working tree:

```diff
@@ parse_array, JsonState::ObjectValue
+            if (c == ']')
+            {
+                // Empty array (or trailing-comma terminator).
+                state = JsonState::End;
+                break;
+            }
             if (c == '{') { …existing… }
```

```diff
@@ parse_object, JsonState::ObjectKey
+            if (c == '}')
+            {
+                // Empty object (or trailing-comma terminator).
+                state = JsonState::End;
+                break;
+            }
             if (c == '"') { …existing… }
```

**Resolution.** Fixed upstream; the submodule is now pinned at
`b7b934f5`, which includes the fix, and the local patch has been
deleted.

**Caveat (over-permissive).** The current patch also accepts
trailing-comma inputs like `[1,2,]` and `{"k":1,}` because the
empty-container early-out (`if (c == ']') state = End`) fires both at
"just opened the bracket" and at "just consumed a comma". A
strict-JSON variant would need an extra flag to distinguish those two
states. We accept the looser behavior for now because GitHub's API
never emits trailing commas, so the laxness is unreachable in
practice.

---

## 2. ~~solvcon JSON parser does not track string state while scanning values~~ (fixed upstream)

**Where:** `third_party/solvcon/cpp/solvcon/serialization/SerializableItem.cpp`
**Functions:** `JsonNode::parse_object`, `JsonNode::parse_array`

**Bug.** In both `parse_object` and `parse_array`, when the current
value is a scalar/string (not a nested `{`/`[`), the parser scans
forward until the *next* character is `,` or the close bracket:

```cpp
while (index < json.size() - 1) {
    value_expression.push_back(json[index]);
    const char c_next = json[index + 1];
    if (c_next == ',' || c_next == '}') break;  // (or ']' for arrays)
    ...
    index += 1;
}
```

This does not track whether the scan is currently inside a quoted
string, so any JSON string value that contains `,` or `}` (or `]`)
terminates the value early. The leftover quoted suffix then fails the
quote-pair check and the parser throws `Invalid JSON format: invalid
value expression: …`. Escaped quotes (`\"`) inside strings are also
not tracked, so a string like `"He said \"hi\""` is split at the
first inner quote.

**Why it matters for solvcon-bot.** `IssueComment.body` is exactly the
kind of free-form string that contains commas, braces, and quoted
text in everyday usage. Any comment with `,` in its body will crash
`list_issue_comments`, which is on the ping path's hot path. M3 ships
*through* this bug for now (see below) — production usage will trip
it immediately.

**Required fix in solvcon.** Track string-quote and backslash-escape
state in both scanning loops, and only treat `,`/`}`/`]` as a value
terminator when not currently inside a quoted string.

**Local workaround (already applied, not committed upstream).** The
scan loops in both `parse_object` and `parse_array` now carry an
`in_string` + `escape` flag pair; `,` / `}` / `]` are only treated
as a value terminator when `in_string` is false. Verified end-to-end:
the bot now parses real `/repos/.../issues/comments` and `/pulls`
responses (which embed `following_url`-style strings containing `{`,
`}`, slashes) without crashing.

**Resolution.** Fixed upstream (together with #1) as of the
`b7b934f5` submodule bump; the local patch has been deleted.

---

## 3. solvcon `escape_string` corrupts non-ASCII bytes (UTF-8)

**Where:** `third_party/solvcon/cpp/solvcon/serialization/SerializableItem.cpp`
**Function:** `escape_string`

**Bug.** The fallback branch is:

```cpp
if (c < 32 || c >= 127) {
    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
        << static_cast<int>(c);
}
```

`c` is `const char`. On signed-char platforms (default on x86_64 and
Apple Silicon), any byte with the high bit set is a negative `char`,
so `c < 32` is true and the byte is "escaped" as `\u` plus the
signed-int representation, which is neither four hex digits nor a
valid Unicode code unit. UTF-8-encoded text is therefore mangled.

**Why it matters for solvcon-bot.** Any AI-CLI output containing
non-ASCII (Unicode punctuation, emoji, non-Latin scripts) would be
corrupted when posted back through solvcon's `to_json_string`. We
sidestep this in `GithubClient::post_comment` by emitting the comment
JSON ourselves with a local UTF-8-safe escaper, so M3 ships without
relying on solvcon's emitter for outgoing comment bodies.

**Required fix in solvcon.** Cast to `unsigned char` before the
range comparison; emit raw bytes 0x20–0xFF since JSON strings accept
any UTF-8 byte; only `\u00XX`-escape control bytes 0x00–0x1F (plus
the JSON-mandatory `"` and `\\`).

---

## 4. ~~BEGIN_DIFF / END_DIFF fences are forgeable from a PR diff~~ (fixed)

**Where:** `src/reviewer.cpp` — `assemble_review_stdin`.

The AI reviewers used to receive their input as
`PROMPT\n\nBEGIN_DIFF\n<diff>\nEND_DIFF\n`. A malicious PR author
could put a line that reads exactly `END_DIFF` in their diff,
followed by adversarial instructions — the AI may then read those
instructions as if they came from the operator.

**Resolution.** `assemble_review_stdin` now generates a 128-bit
random nonce per run (`generate_diff_fence_nonce()`, backed by
`std::random_device`) and emits `BEGIN_DIFF_<32 hex>` /
`END_DIFF_<32 hex>` fences, plus an instruction line naming the
exact fences and telling the model everything between them is
untrusted data. A literal `END_DIFF` line inside a diff can no
longer close the fenced region (covered by tests in
`tests/test_reviewer.cpp`). The planned second layer (rejecting /
encoding `^(BEGIN|END)_DIFF` lines before interpolation) was
dropped: rewriting diff lines would corrupt patch context, and the
nonce alone makes the fence unforgeable without knowing it.

## 5. ~~M3 transport tests not yet present~~ (fixed)

**Where:** `tests/test_github_transport.cpp`

GithubClient's HTTP behaviors — pagination via `Link: rel="next"`,
collaborator 404 vs 403 dispatch, 5xx/429 retry policy (including
transient rate-limited 403s), POST non-retry, diff truncation under
MAX_DIFF_BYTES, and the ETag conditional cache — are now covered by
an in-process fake `httplib::Server` suite in
`tests/test_github_transport.cpp`.
