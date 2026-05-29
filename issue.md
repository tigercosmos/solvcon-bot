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
