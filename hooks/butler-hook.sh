#!/usr/bin/env bash
# Claude Code hook: tells the butler what this session is doing (see README.md, "Claude Code hooks").
# Registered for SessionStart, UserPromptSubmit, PreToolUse, PostToolUse, PermissionRequest, Notification, Stop
# and SessionEnd. Reads the hook's JSON on stdin and returns at once: the report goes to butler-bridge.py (which
# owns the USB serial port) in the background, so a board that is unplugged never slows Claude down.
# BUTLER_BRIDGE_PORT overrides the bridge's port, 47474.

input=$(cat)
command -v jq >/dev/null || exit 0

# One line, fields split by \x1f: unlike a tab, read does not merge it when a field is empty
IFS=$'\x1f' read -r event session tool notif message cwd < <(
    printf '%s' "$input" | jq -r '[.hook_event_name, .session_id, .tool_name, .notification_type, .message, .cwd]
                                  | map(. // "" | tostring | gsub("[\n\u001f]"; " ")) | join("\u001f")' 2>/dev/null)
[ -n "$session" ] || exit 0

case "$event" in
    SessionStart)       state=idle ;;
    UserPromptSubmit)   state=working ;;
    PreToolUse)         case "$tool" in AskUserQuestion|ExitPlanMode) state=question ;; *) state=working ;; esac ;;
    PostToolUse)        state=working ;;
    PermissionRequest)  state=question ;;
    Notification)
        case "$notif" in
            permission_prompt|elicitation_dialog) state=question ;;
            idle_prompt) state=idle ;;
            "") case "$message" in                  # older versions send no notification_type
                    *permission*) state=question ;;
                    *"waiting for your input"*) state=idle ;;
                    *) exit 0 ;;
                esac ;;
            *) exit 0 ;;
        esac ;;
    Stop)               state=idle ;;
    SessionEnd)         state=end ;;
    *)                  exit 0 ;;
esac

# The sender's clock in ms: the butler drops a report older than the last one it has for this session
t=$(perl -MTime::HiRes=time -e 'printf "%d", time * 1000' 2>/dev/null || echo 0)
body=$(jq -cn --arg s "$session" --arg st "$state" --arg n "${cwd##*/}" --argjson t "$t" \
           '{op: "state", session: $s, state: $st, name: $n, t: $t}')

# Straight to the bridge with bash's /dev/tcp (no new process); if it isn't running, the Python client starts it
{
    { printf '%s\n' "$body" >"/dev/tcp/127.0.0.1/${BUTLER_BRIDGE_PORT:-47474}"; } 2>/dev/null ||
        python3 "$(dirname "$0")/butler-bridge.py" send "$body"
} </dev/null >/dev/null 2>&1 &
disown 2>/dev/null
exit 0
