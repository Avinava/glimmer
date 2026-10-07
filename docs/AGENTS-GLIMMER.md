# glimmer for agents

glimmer is a small desk display on the user's LAN (`http://glimmer.local`,
or its IP). Coding agents put **cards** on it so the user notices things
without watching the terminal:

- an agent is **blocked on an approval or a question** — shown automatically
  by the hooks in [`tools/agents/`](../tools/agents/); agents do nothing;
- a long task's **progress**;
- a task **finished** or **failed**.

The display has no buttons. It only *shows* — the user always answers in the
terminal.

| Approval card | Agents list |
|---|---|
| ![Approval card](images/screen-approval.png) | ![Agents list](images/screen-agents.png) |

---

## Instructions to paste into a project's `CLAUDE.md` / `AGENTS.md`

```markdown
## glimmer desk display
MCP server `glimmer` (push_card, clear_card, list_cards) shows cards on the
user's desk display. Use it only for things worth a glance:
- Long task (> ~2 min: test suite, build, deploy, migration): push
  kind=progress, a stable id (e.g. "tests"), agent, project, progress 0-100.
  Re-push the same id to update, at most every 30 s.
- When it ends: push the SAME id with kind=success or kind=error, a short
  title ("TESTS PASSED"), optional value ("142/142"), ttl_s=120.
- Blocked on a decision outside a permission prompt: push kind=input,
  title = the question in <= 27 chars; clear_card it once the user answers.
- Limits: title 27, value 15, body 39, project 15, id 23 chars.
- Never send secrets, tokens, file contents or personal data. No cards for
  routine steps.
```

---

## Card reference

`push_card` (MCP) and `POST /push` (HTTP JSON) take the same fields. Longer
strings are cut to the limit; on screen, lines that don't fit end in `...`.

| field | type | meaning |
|---|---|---|
| `title` | string ≤ 27 | headline (required by MCP) |
| `value` | string ≤ 15 | optional big text, e.g. `142/142`, `PASSED` |
| `body` | string ≤ 39 | optional detail line |
| `kind` | `approval` · `error` · `input` · `progress` · `warning` · `success` · `info` | colour and priority, highest first. Default `info` |
| `id` | string ≤ 23 | stable id; re-sending it **updates the card in place** (no new interruption unless the kind gets more urgent). Default: a new `push:<n>` card each time |
| `agent` | `claude` · `codex` · `other` | coloured square + name on the card. Default `other` |
| `project` | string ≤ 15 | e.g. the repo name |
| `progress` | 0–100 | progress bar; also the value (`64%`) when no `value` |
| `ttl_s` | seconds | lifetime; `0` = until cleared; max 86400. Default by kind: approval/input = the user's "Give up after" (30 min by default), error 300, warning 60, progress 3600, success/info 30 |
| `display` | `interrupt` · `queue` | `interrupt` takes the screen for up to 30 s (or `ttl_s`, if shorter), then stays on the **Agents** list; `queue` is only listed. Default `interrupt`, except `progress` → `queue` |
| `urgent` | bool | may show on (and wake) the night screen — use sparingly |

Legacy fields still accepted: `subtitle` (= `body`), `color` (mapped to a
kind), `duration_s` (≤ 300; holds the screen for its whole lifetime).

Behaviour:

- **Approvals and questions** (`approval`, `input`) hold the screen until
  cleared or expired (user setting "Hold the screen until answered", on by
  default). While any waits, the strip at the bottom of every screen is
  solid amber (approval) or sky (question).
- The queue holds **5** cards, most important first. When full, the least
  important, oldest card goes; approvals/questions are never pushed out by
  anything less important — the push fails instead (HTTP 409 / MCP
  `isError`).
- Several cards holding the screen cycle every 6 s.
- In night mode "clock" or "dark", only `urgent` cards, `error` cards with no
  `agent`, and — if the user enabled "Show approvals at night too" —
  approvals/questions appear; others wait (the strip still lights).

### How cards look

Centred, one focal element, no chrome:

- **Approval / question**: `■ CLAUDE · project` (muted) · the tool or your
  `title` as the big word (`Your turn` for a hook question) · one muted detail
  line (the command) · status in the kind colour: `NEEDS YOU · 2M` /
  `WAITING · 2M`.
- **Result / progress**: header · the `value` (or `64%` + bar) as the big
  text · `title` in the kind colour, upper-cased · `body` muted. No `value`:
  the `title` itself is the big text. A hairline shortens as `ttl_s` runs out.
- **Agents list**: up to 3 rows (`■ Needs you · Bash`, then
  `claude · project · 2m`); more become `+ N more`.

Fonts are ASCII plus `°` and `·` — other characters are dropped.

### Other calls

| | HTTP | MCP |
|---|---|---|
| clear one / all | `POST /push/clear` `{"id":"…"}` / `{"all":true}` → `{"cleared":n}` | `clear_card` |
| list | `GET /push` → `{"cards":[…]}` (with `age_s`, `expires_in_s`) | `list_cards` |
| device state | `GET /api/state` | `get_state` |
| switch screen | `POST /api/channel` `{"name":"Claude"}` / `{"action":"next"}` | `show_channel` / `next_channel` |

If the user set an API token on the device, every call above except
`/api/state` needs `Authorization: Bearer <token>` (401 otherwise);
[`tools/agents/mcp.json`](../tools/agents/mcp.json) carries it as a header
from `$GLIMMER_TOKEN`. Bad JSON → 400.

### Examples

```bash
# progress, updated in place
curl -X POST http://glimmer.local/push -H 'Content-Type: application/json' \
  -d '{"id":"tests","kind":"progress","title":"TESTS","progress":40,"agent":"claude","project":"api"}'

# then the result, same id
curl -X POST http://glimmer.local/push -H 'Content-Type: application/json' \
  -d '{"id":"tests","kind":"success","title":"TESTS PASSED","value":"142/142","ttl_s":120}'

# done with it early
curl -X POST http://glimmer.local/push/clear -H 'Content-Type: application/json' -d '{"id":"tests"}'
```

---

## Approvals via hooks (how it works)

[`tools/agents/glimmer-hook.sh`](../tools/agents/glimmer-hook.sh) runs as a
Claude Code / Codex hook (configs:
[`claude-settings.json`](../tools/agents/claude-settings.json),
[`codex-hooks.json`](../tools/agents/codex-hooks.json)). It reads the event
on stdin and sends a small summary — event, notification type, session, cwd,
tool, a 60-char command/path, an 80-char message — to
`POST /hook?agent=claude|codex`. **Never post the raw event**: a PostToolUse
carries the whole tool output, far beyond the device's RAM (bodies > 2 KB are
ignored). The script needs `curl` + `jq`, reads `GLIMMER_URL` /
`GLIMMER_TOKEN`, gives up after 2 s and always exits 0; `/hook` always
answers 200, so the display can never block or decide anything.

Because the command and message end up on a desk screen, the script masks
anything credential-shaped before sending: `key=…`, `token: …`,
`--password=…`, `Authorization: Bearer …` and bare `sk-…`, `eyJ…` (JWT),
`ghp_…`, `xox…-`, `AKIA…` values all become `***`.

| event | card |
|---|---|
| Claude `Notification` `permission_prompt` / Codex `PermissionRequest` | **approval** — tool as the big word, command below, `NEEDS YOU · 2M` |
| Claude `Notification` `idle_prompt` / `agent_needs_input` / `elicitation_dialog` | **input** — `Your turn`, `WAITING · 2M` |
| Codex `Stop` (turn finished, Codex waits for you) | **input** — `Your turn` with the start of Codex's last message, shown only if you haven't replied within 60 s (Codex has no idle event, so this mirrors Claude's `idle_prompt`) |
| `PostToolUse`, `PostToolUseFailure`, `UserPromptSubmit`, `SessionEnd` | clears that session's card |
| Claude `Stop` (or `Notification` `agent_completed`) | clears it — or, with the "DONE card" setting on, a 20 s **DONE** card |

One card per agent session (id `claude:<first 8 chars of session id>`), so
two sessions waiting at once cycle with queue dots. Neither agent fires an
event at the moment you approve: the card clears when the approved tool
finishes (or at your next prompt), and otherwise expires after the "Give up
after" time.
