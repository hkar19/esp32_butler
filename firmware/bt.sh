#!/usr/bin/env bash
# Build, provision and flash the butler using the settings in butler.env (see README.md).
#
#   ./bt.sh init                     create butler.env (first time)
#   ./bt.sh build                    compile the app (for IDF_TARGET in butler.env, default esp32c6)
#   ./bt.sh menuconfig               change build options, e.g. the LED's GPIO (Butler status LED)
#   ./bt.sh test-build               build from scratch in a temporary folder, as a fresh clone would (also run by CI)
#       [--target CHIP]...           for these chips (default esp32c6); needs no board and no butler.env
#   ./bt.sh flash                    flash bootloader + partition table + app (leaves the settings alone)
#   ./bt.sh provision                butler.env section 2 -> the settings image provision-out/nvs.bin
#   ./bt.sh flash-nvs                flash that settings image (leaves the app alone)
#   ./bt.sh monitor                  serial log through idf.py, resets the board (Ctrl+] to quit)
#   ./bt.sh log                      the board's log as the bridge sees it, without a reset (Ctrl+C to quit)
#   ./bt.sh state                    what the board shows, and every agent it knows
#   ./bt.sh clear                    forget every agent
#   ./bt.sh led-test                 show every LED state in turn, to check the colours by eye
#       [--seconds N] [--state NAME] N seconds per state (default 5); only that state
#   ./bt.sh help                     show this list (also -h, --help, or no arguments)
#
# Commands run in order, e.g. ./bt.sh provision build flash flash-nvs monitor
# flash, flash-nvs and monitor have the bridge (../hooks/butler-bridge.py) let go of the serial port while they run.
set -euo pipefail

cd "$(dirname "$0")"
IDF_REQUIRED=v6.1
LED_STATES="booting no_host question working idle no_agents"
BRIDGE=../hooks/butler-bridge.py
PAUSE_FILE="$HOME/.butler/paused"

die() { echo "bt.sh: $*" >&2; exit 1; }
usage() { awk 'NR > 1 && !/^#/ { exit } NR > 1' "${0##*/}" | sed 's/^# \{0,1\}//'; }   # we're in its folder now
bad_usage() { echo "bt.sh: $1" >&2; echo >&2; usage >&2; exit 2; }
need() { [ -n "${!1:-}" ] || die "$1 is not set in butler.env"; }

load_env() {
    [ -f butler.env ] || die "no butler.env: run ./bt.sh init, then edit it"
    [ "${ENV_LOADED:-}" = 1 ] && return
    ENV_LOADED=1
    set -a; . ./butler.env; set +a
}

activate_idf() {
    # EIM's activate script refuses to be sourced from a script, so read its environment with -e instead
    if [ -z "${IDF_PATH:-}" ] || [ ! -x "${IDF_PYTHON_ENV_PATH:-}/bin/python" ]; then
        need IDF_ACTIVATE
        [ -f "$IDF_ACTIVATE" ] || die "IDF_ACTIVATE not found: $IDF_ACTIVATE"
        local line
        while IFS= read -r line; do
            case "$line" in
                PATH=*) PATH="${line#PATH=}:$PATH" ;;
                SYSTEM_PATH=*) ;;
                [A-Z_]*=*) export "$line" ;;
            esac
        done < <(sh "$IDF_ACTIVATE" -e)
        export PATH
    fi
    PY="$IDF_PYTHON_ENV_PATH/bin/python"
    local v; v=$(idf --version 2>/dev/null) || true
    case "$v" in *"$IDF_REQUIRED"*) ;; *) die "needs ESP-IDF $IDF_REQUIRED, found: ${v:-none}";; esac
}

idf() { "$PY" "$IDF_PATH/tools/idf.py" "$@"; }

nvs_field() { awk -F, -v col="$1" '$1 ~ /^nvs[ \t]*$/ { gsub(/[ \t]/, "", $col); print $col }' partitions.csv; }

cmd_init() {
    if [ -f butler.env ]; then echo "butler.env already exists, left as is"
    else cp butler.env.example butler.env; echo "created butler.env: check ESPPORT in it now"; fi
}

# sdkconfig remembers the chip it was made for; making it again for another chip drops menuconfig changes
check_target() {
    IDF_TARGET=${IDF_TARGET:-esp32c6}; export IDF_TARGET
    local had; had=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"$/\1/p' sdkconfig 2>/dev/null) || true
    if [ -n "$had" ] && [ "$had" != "$IDF_TARGET" ]; then
        echo "bt.sh: sdkconfig is for $had, butler.env says $IDF_TARGET: starting over for $IDF_TARGET"
        idf set-target "$IDF_TARGET"
    fi
}

cmd_build() { activate_idf; check_target; idf build; }

cmd_menuconfig() { activate_idf; check_target; idf menuconfig; }

# The bridge holds the serial port; it lets go while $PAUSE_FILE names a running process, and takes it back after
release_port() {
    [ -z "${PORT_RELEASED:-}" ] || return 0
    PORT_RELEASED=1
    mkdir -p "$(dirname "$PAUSE_FILE")"
    echo $$ > "$PAUSE_FILE"
    trap 'rm -f "$PAUSE_FILE"' EXIT
    trap 'exit 130' INT TERM
    # Only wait if a bridge is running: it checks every 0.2 s, but may be waiting up to 1 s for the board
    if (exec 3<>"/dev/tcp/127.0.0.1/${BUTLER_BRIDGE_PORT:-47474}") 2>/dev/null; then sleep 1.5; fi
}

cmd_flash() { need ESPPORT; release_port; activate_idf; idf -p "$ESPPORT" ${ESPBAUD:+-b "$ESPBAUD"} flash; }

cmd_monitor() { need ESPPORT; release_port; activate_idf; idf -p "$ESPPORT" monitor; }

cmd_log() { [ -f ~/.butler/bridge.log ] || die "no bridge log yet: ./bt.sh state starts the bridge"; tail -n 40 -f ~/.butler/bridge.log; }

cmd_provision() {
    activate_idf
    local args=(--size "$(nvs_field 5)")
    [ -z "${BT_LED_BRIGHTNESS_PCT:-}" ] || args+=(--led-brightness-pct "$BT_LED_BRIGHTNESS_PCT")
    [ -z "${BT_WORKING_STALE_S:-}" ] || args+=(--working-stale-s "$BT_WORKING_STALE_S")
    [ -z "${BT_SESSION_TTL_S:-}" ] || args+=(--session-ttl-s "$BT_SESSION_TTL_S")
    "$PY" tools/provision.py "${args[@]}"
    echo "Next: ./bt.sh flash-nvs"
}

cmd_flash_nvs() {
    need ESPPORT
    local bin=provision-out/nvs.bin off
    [ -f "$bin" ] || die "$bin not found: run ./bt.sh provision first"
    off=$(nvs_field 4); [ -n "$off" ] || die "no nvs partition in partitions.csv"
    release_port
    activate_idf
    "$PY" -m esptool -p "$ESPPORT" ${ESPBAUD:+-b "$ESPBAUD"} write-flash "$off" "$bin"
}

# One request to the board through the bridge (started if need be); prints the answer
bridge() { python3 "$BRIDGE" send "$1"; }

cmd_state() { bridge '{"op":"get"}'; }

cmd_clear() { bridge '{"op":"clear"}'; }

cmd_led_test() {
    local s states="${LED_TEST_STATE:-$LED_STATES}" secs="${LED_TEST_SECONDS:-5}"
    for s in $states; do
        printf '%-16s ' "$s"
        bridge "{\"op\":\"led_test\",\"state\":\"$s\",\"seconds\":$secs}"
        sleep "$secs"
    done
}

# Uses only the repo's files (not sdkconfig or build/), so it catches a build that works only on this machine.
# Fails on a compile error, a warning (IDF builds with -Werror), or an app too big for its partition.
cmd_test_build() {
    activate_idf
    local t tmp failed=0
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/butler-test-build.XXXXXX")
    cp dependencies.lock "$tmp/dependencies.lock"   # the component manager writes the target into it
    for t in ${TEST_TARGETS:-esp32c6}; do
        printf 'test-build %-10s ' "$t"
        if IDF_TARGET=$t idf -B "$tmp/build-$t" -D SDKCONFIG="$tmp/sdkconfig-$t" build > "$tmp/$t.log" 2>&1 &&
           [ -s "$tmp/build-$t/esp32_butler.bin" ]; then
            echo "ok, app $(wc -c < "$tmp/build-$t/esp32_butler.bin" | tr -d ' ') bytes"
        else
            echo "FAILED:"
            # The compiler's errors if there are any, else the end of the log (a CMake or size problem)
            { grep -E "error:|Error:|Error at" "$tmp/$t.log" || tail -n 30 "$tmp/$t.log"; } | head -n 30 | sed 's/^/    /'
            failed=1
        fi
    done
    cp "$tmp/dependencies.lock" dependencies.lock
    if [ $failed = 0 ]; then rm -rf "$tmp"; else die "test-build failed (logs kept in $tmp)"; fi
}

[ $# -gt 0 ] || { usage; exit 1; }

cmds=()
while [ $# -gt 0 ]; do
    case "$1" in
        help|-h|--help) usage; exit 0 ;;
        --seconds) [ $# -ge 2 ] || bad_usage "--seconds needs a value"; LED_TEST_SECONDS=$2; shift ;;
        --state)   [ $# -ge 2 ] || bad_usage "--state needs a value: $LED_STATES"; LED_TEST_STATE=$2; shift ;;
        --target)  [ $# -ge 2 ] || bad_usage "--target needs a chip, e.g. esp32c6"; TEST_TARGETS="${TEST_TARGETS:-} $2"; shift ;;
        init|build|menuconfig|test-build|flash|provision|flash-nvs|monitor|log|state|clear|led-test) cmds+=("$1") ;;
        -*) bad_usage "unknown option: $1" ;;
        *) bad_usage "unknown command: $1" ;;
    esac
    shift
done
[ ${#cmds[@]} -gt 0 ] || bad_usage "no command given"
for c in "${cmds[@]}"; do
    case "$c" in
        init) ;;
        test-build) [ ! -f butler.env ] || load_env ;;   # CI has no butler.env: ESP-IDF is exported there
        *) load_env; break ;;
    esac
done
for c in "${cmds[@]}"; do
    "cmd_${c//-/_}"
done
