#!/usr/bin/env bash
# Adds butler-hook.sh to Claude Code's user settings (~/.claude/settings.json), for every session on this machine.
# Keeps a backup next to it, and leaves every other setting and hook alone. Run it again after moving this folder,
# or after changing butler-bridge.py: it stops the running bridge, and the next hook starts the new one.
#   ./install.sh              install (or update the path)
#   ./install.sh --uninstall  remove the butler's hooks
set -euo pipefail

hook="$(cd "$(dirname "$0")" && pwd)/butler-hook.sh"
settings="${CLAUDE_SETTINGS:-$HOME/.claude/settings.json}"
events='["SessionStart","UserPromptSubmit","PreToolUse","PostToolUse","PermissionRequest","Notification","Stop","SessionEnd"]'

for tool in jq python3 perl; do command -v $tool >/dev/null || { echo "install.sh: needs $tool" >&2; exit 1; }; done
chmod +x "$hook"
[ -f "$settings" ] || echo '{}' > "$settings"
cp "$settings" "$settings.bak-butler"

# Drop any earlier butler entry (whatever its path), then add ours unless uninstalling
jq --arg hook "$hook" --argjson events "$events" --arg mode "${1:-install}" '
    .hooks //= {}
    | reduce $events[] as $e (.;
        .hooks[$e] = ((.hooks[$e] // [])
            | map(.hooks |= map(select(.command | tostring | endswith("butler-hook.sh") | not)))
            | map(select(.hooks | length > 0))
            | if $mode == "--uninstall" then . else . + [{hooks: [{type: "command", command: $hook, timeout: 5}]}] end))
    | .hooks |= with_entries(select(.value | length > 0))
' "$settings.bak-butler" > "$settings"

python3 "$(dirname "$hook")/butler-bridge.py" stop >/dev/null

if [ "${1:-}" = --uninstall ]; then echo "removed the butler hooks from $settings"
else echo "installed $hook for $(echo "$events" | jq -r 'length') events in $settings (backup: $settings.bak-butler)"; fi
