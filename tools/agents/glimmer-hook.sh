#!/bin/sh
# glimmer-hook.sh <claude|codex>
#
# Forward an agent hook event (JSON on stdin) to a glimmer desk display, so it
# can show "APPROVAL NEEDED" while Claude Code / Codex waits on you and clear
# it once you've answered.
#
# Sends only a few small fields — never the raw event: a PostToolUse event
# carries the tool's whole output, far more than the device's RAM.
#
# Environment:
#   GLIMMER_URL    default http://glimmer.local   (or http://<device-ip>)
#   GLIMMER_TOKEN  the device's API token, if you set one
#
# Requires curl and jq. Always exits 0 and gives up after 2 s, so a missing or
# offline display never blocks or slows the agent.

agent=${1:-claude}
url=${GLIMMER_URL:-http://glimmer.local}

command -v jq >/dev/null 2>&1 || exit 0

payload=$(jq -c '{
  event:   (.hook_event_name   // ""),
  type:    (.notification_type // ""),
  session: (.session_id        // ""),
  cwd:     (.cwd               // ""),
  tool:    (.tool_name         // ""),
  detail:  ((.tool_input.command // .tool_input.file_path // .tool_input.path // "")
            | tostring | .[0:60]),
  message: ((.message // .last_assistant_message // "") | tostring
            | gsub("\\s+"; " ") | .[0:80])
}' 2>/dev/null) || exit 0
[ -n "$payload" ] || exit 0

if [ -n "$GLIMMER_TOKEN" ]; then
  curl -s --max-time 2 -X POST -H 'Content-Type: application/json' \
       -H "Authorization: Bearer $GLIMMER_TOKEN" \
       --data "$payload" "$url/hook?agent=$agent" >/dev/null 2>&1
else
  curl -s --max-time 2 -X POST -H 'Content-Type: application/json' \
       --data "$payload" "$url/hook?agent=$agent" >/dev/null 2>&1
fi
exit 0
