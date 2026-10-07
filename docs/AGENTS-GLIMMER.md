# glimmer for agents

glimmer is a small desk display on the user's LAN (`http://glimmer.local`,
or its IP). Coding agents can put **cards** on it so the user notices things
without watching the terminal:

- an agent is **blocked on an approval** (shown automatically by hooks — see
  the README; agents don't need to do anything for this);
- a long task's **progress**;
- a task **finished** or **failed**.

The display has no buttons. It only *shows* — the user always answers in the
terminal.

---

## Instructions to paste into a project's `CLAUDE.md` / `AGENTS.md`

```markdown
## glimmer desk display
A glimmer display is available as the `glimmer` MCP server (tools:
push_card, clear_card, list_cards).
- Long task (> ~2 min: test suites, builds, deploys, migrations): push
  `kind: progress` with a stable `id` (e.g. "tests") and re-send the same
  id to update `progress` (0-100) at most every 30 s. Clear it when done.
- When a long task finishes: push `kind: success` or `kind: error` with the
  same `id`, a short `title` ("TESTS PASSED"), an optional `value`
  ("142/142") and `ttl_s: 120`.
- Blocked and need the user's decision outside a tool-permission prompt:
  push `kind: input`, `title: "NEED YOUR INPUT"`, and a one-line `body`.
  Clear it once the user replies.
- Never push secrets, tokens, file contents or personal data. Keep titles
  <= 27 chars, value <= 15, body <= 39. Don't push for routine steps.
```

---

## Card reference

`push_card` (MCP) and `POST /push` (HTTP) take the same fields:

| field | type | meaning |
|---|---|---|
| `title` | string ≤ 27 | headline (required) |
| `value` | string ≤ 15 | optional big text, e.g. `142/142`, `PASSED` |
| `body` | string ≤ 39 | optional detail line |
| `kind` | `approval` · `input` · `error` · `warning` · `success` · `progress` · `info` | colour and priority, highest first |
| `id` | string ≤ 23 | stable id; re-sending it **updates the card in place** (no new interruption). Default: a new card each time |
| `agent` | `claude` · `codex` · `other` | coloured tag on the card |
| `project` | string ≤ 15 | e.g. the repo name |
| `progress` | 0–100 | draws a progress bar (and shows `64%` when no `value`) |
| `ttl_s` | seconds | how long it lives; `0` = until cleared (max 86400). Defaults by kind: approval/input 30 min, error 5 min, warning 1 min, progress 1 h, others 30 s |
| `display` | `interrupt` · `queue` | `interrupt` (default; `progress` defaults to `queue`) takes the screen for up to 30 s, then stays listed on the **Agents** channel; `queue` is only listed |
| `urgent` | bool | may wake a dark screen at night — use sparingly |

Behaviour:

- **Approvals and questions** (`approval`, `input`) hold the screen until
  cleared (user setting, on by default), and turn the strip at the bottom of
  every screen amber/blue.
- The queue holds 5 cards. When full, the least important, oldest card goes;
  approvals/questions are never pushed out by less important cards (the push
  then fails with HTTP 409 / an MCP error).
- At night only urgent cards interrupt; approvals show as the strip unless
  the user opted in.

Other calls:

| | HTTP | MCP |
|---|---|---|
| clear one / all | `POST /push/clear` `{"id": "..."}` / `{"all": true}` | `clear_card` |
| list | `GET /push` | `list_cards` |
| device state | `GET /api/state` | `get_state` |
| switch screen | `POST /api/channel` `{"name": "Claude"}` / `{"action": "next"}` | `show_channel` / `next_channel` |

If the user set an API token on the device, send
`Authorization: Bearer <token>` (the MCP config carries it as a header).

### Examples

```bash
# progress, updated in place
curl -X POST http://glimmer.local/push -H 'Content-Type: application/json' \
  -d '{"id":"tests","kind":"progress","title":"TESTS","progress":40,"agent":"claude","project":"api"}'

# then the result, same id
curl -X POST http://glimmer.local/push -H 'Content-Type: application/json' \
  -d '{"id":"tests","kind":"success","title":"TESTS PASSED","value":"142/142","ttl_s":120}'
```

---

## Approvals via hooks (how it works)

`tools/agents/glimmer-hook.sh` is installed as a Claude Code / Codex hook. It
reads each hook event on stdin and sends a ~200-byte summary to
`POST /hook?agent=claude|codex` (never the raw event — tool output can be far
larger than the device's RAM). The device maps events to cards:

| event | card |
|---|---|
| Claude `Notification` `permission_prompt` (fires ~6 s after an unanswered prompt) / Codex `PermissionRequest` | **APPROVAL NEEDED** — tool + command, project, "waiting 2m" |
| Claude `Notification` `idle_prompt` / `agent_needs_input` | **WAITING FOR YOU** |
| `PostToolUse`, `PostToolUseFailure`, `UserPromptSubmit`, `SessionEnd` | clears that session's card |
| `Stop` | clears it — or, with "Show a DONE card" on, a 20 s **DONE** card |

One card per agent session (`claude:<session>`), so two sessions waiting at
once show as "1 of 2" and cycle. Neither agent fires an event at the exact
moment you approve; the card clears when the approved tool finishes (or at
your next prompt), and expires after the "give up after" time otherwise.
