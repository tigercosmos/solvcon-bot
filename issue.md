# Known Issues / Tech Debt

## 1. modmesh JSON parser rejects empty arrays and empty objects

**Where:** `third_party/modmesh/cpp/modmesh/serialization/SerializableItem.cpp`
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

**Why it matters for modmesh-bot.** The GitHub REST API returns `[]` for
*every* empty list — empty reviews, empty comments, empty PR list,
etc. Any list endpoint with zero items would crash the parser in M3.
The state file also serializes `reviewed_prs`, `handled_comments` as
arrays that are empty on first run, so first-tick save → second-tick
load round-trip also crashes (caught during M1 verification).

**Local workaround (already applied, not committed).** Two small
patches in this checkout's `third_party/modmesh` working tree:

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

The patch is sitting uncommitted in the submodule's working tree so M1
verifies locally. The pinned submodule SHA (`e05d29f`) does **not**
include the fix, so a fresh `git submodule update --init` will not
have it.

**Resolution plan.**
1. Upstream the fix to `solvcon/modmesh` as its own PR with unit tests
   covering `[]`, `{}`, nested `{"a":[]}`, `{"a":{}}`, and trailing
   commas (the patch currently also tolerates `[1,2,]` / `{"k":1,}`;
   confirm whether that's desired or whether trailing commas should
   stay rejected).
2. Once merged, bump `third_party/modmesh` to the new SHA and drop the
   note about the workaround from `plan.md`.

**Tracking.** Replace this entry with a link to the upstream PR/issue
once filed.

---

## 2. modmesh JSON parser does not track string state while scanning values

**Where:** `third_party/modmesh/cpp/modmesh/serialization/SerializableItem.cpp`
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

**Why it matters for modmesh-bot.** `IssueComment.body` is exactly the
kind of free-form string that contains commas, braces, and quoted
text in everyday usage. Any comment with `,` in its body will crash
`list_issue_comments`, which is on the ping path's hot path. M3 ships
*through* this bug for now (see below) — production usage will trip
it immediately.

**Required fix in modmesh.** Track string-quote and backslash-escape
state in both scanning loops, and only treat `,`/`}`/`]` as a value
terminator when not currently inside a quoted string.

**Resolution plan.** Same as bug #1: upstream a single PR to
`solvcon/modmesh` that fixes both (string-state scanning here, empty
arrays/objects in #1), then bump our submodule SHA. Until then,
modmesh-bot's parsing of any list endpoint will choke on punctuation
in string fields.

---

## 3. modmesh `escape_string` corrupts non-ASCII bytes (UTF-8)

**Where:** `third_party/modmesh/cpp/modmesh/serialization/SerializableItem.cpp`
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

**Why it matters for modmesh-bot.** Any AI-CLI output containing
non-ASCII (Unicode punctuation, emoji, non-Latin scripts) would be
corrupted when posted back through modmesh's `to_json_string`. We
sidestep this in `GithubClient::post_comment` by emitting the comment
JSON ourselves with a local UTF-8-safe escaper, so M3 ships without
relying on modmesh's emitter for outgoing comment bodies.

**Required fix in modmesh.** Cast to `unsigned char` before the
range comparison; emit raw bytes 0x20–0xFF since JSON strings accept
any UTF-8 byte; only `\u00XX`-escape control bytes 0x00–0x1F (plus
the JSON-mandatory `"` and `\\`).

---

## 4. M3 transport tests not yet present

**Where:** `tests/test_github_client.cpp`

GithubClient's HTTP behaviors — pagination via `Link: rel="next"`,
collaborator 404 vs 403 dispatch, 5xx/429 retry policy, POST
non-retry, diff truncation under MAX_DIFF_BYTES — are exercised only
in M7/M8 end-to-end smoke. A local in-process fake `httplib::Server`
test would catch regressions earlier. Track as M3 follow-up; not a
correctness defect today.
