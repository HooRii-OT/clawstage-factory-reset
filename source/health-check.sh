#!/usr/bin/env bash
set -euo pipefail

# Verify that every ClawStage component is running and functionally ready.
# The check intentionally waits before its first attempt, retries transient
# failures, and performs a second stable pass before accepting a release.

REAL_USER="${SUDO_USER:-${USER:-pi}}"
if [[ "$REAL_USER" == "root" ]]; then
    REAL_USER="pi"
fi
REAL_HOME="$(eval echo "~$REAL_USER" 2>/dev/null || echo "/home/$REAL_USER")"
DEFAULT_VERSION="$(cat "$(dirname "$0")/VERSION" 2>/dev/null || true)"
VERSION="${DEFAULT_VERSION:-}"
RELEASE_DIR=""
CHECK_HOMEASSISTANT=1
CHECK_STAGE_UI=1
STAGE_UI_ONLY=0
CHECK_TRACKER=1
CHECK_HARDWARE=1
LAUNCHER_START_POST=0
RESET_RUNTIME_ONLY=0
STARTUP_GRACE_SECONDS="${HEALTH_CHECK_STARTUP_GRACE_SECONDS:-15}"
TIMEOUT_SECONDS="${HEALTH_CHECK_TIMEOUT_SECONDS:-}"
RETRY_INTERVAL_SECONDS="${HEALTH_CHECK_RETRY_INTERVAL_SECONDS:-5}"
STABILITY_SECONDS="${HEALTH_CHECK_STABILITY_SECONDS:-5}"
CURL_TIMEOUT_SECONDS="${HEALTH_CHECK_CURL_TIMEOUT_SECONDS:-3}"
OPENCLAW_TASKS_TIMEOUT_SECONDS="${HEALTH_CHECK_OPENCLAW_TASKS_TIMEOUT_SECONDS:-10}"
FAILED_CHECKS=()

usage() {
    cat <<EOF
Usage: sudo ./health-check.sh [options]

Options:
  --version VERSION         Use ~/clawstage/releases/VERSION
  --release-dir PATH        Use an explicit release directory
  --startup-grace SECONDS   Wait before the first check (default: 15)
  --timeout SECONDS         Retry window (default: 480 on first boot, 120 for OTA)
  --retry-interval SECONDS  Delay between attempts (default: 5)
  --stability-window SEC    Stable-running confirmation window (default: 5)
  --no-homeassistant        Skip HomeAssistant stack check
  --no-stage-ui             Skip Stage UI checks
  --stage-ui-only           Check only Stage UI, including its stability window
  --no-hardware             Skip peripheral service checks and their restart stability checks
  --no-tracker              Skip face tracker service check
  --launcher-start-post     Expect launcher ExecStartPost during clone verification
  --reset-runtime           Verify HomeAssistant stack after account reset (Gateway awaits provisioning)
  -h, --help                Show this help

The timing defaults can also be set with HEALTH_CHECK_STARTUP_GRACE_SECONDS,
HEALTH_CHECK_TIMEOUT_SECONDS, HEALTH_CHECK_RETRY_INTERVAL_SECONDS,
HEALTH_CHECK_STABILITY_SECONDS, HEALTH_CHECK_CURL_TIMEOUT_SECONDS and
HEALTH_CHECK_OPENCLAW_TASKS_TIMEOUT_SECONDS.
EOF
}

require_option_value() {
    local option="$1"
    local value="${2:-}"
    if [[ -z "$value" ]]; then
        echo "[ERROR] $option requires a value" >&2
        exit 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            require_option_value "$1" "${2:-}"
            VERSION="${2:-}"
            shift 2
            ;;
        --release-dir)
            require_option_value "$1" "${2:-}"
            RELEASE_DIR="${2:-}"
            shift 2
            ;;
        --startup-grace)
            require_option_value "$1" "${2:-}"
            STARTUP_GRACE_SECONDS="${2:-}"
            shift 2
            ;;
        --timeout)
            require_option_value "$1" "${2:-}"
            TIMEOUT_SECONDS="${2:-}"
            shift 2
            ;;
        --retry-interval)
            require_option_value "$1" "${2:-}"
            RETRY_INTERVAL_SECONDS="${2:-}"
            shift 2
            ;;
        --stability-window)
            require_option_value "$1" "${2:-}"
            STABILITY_SECONDS="${2:-}"
            shift 2
            ;;
        --no-homeassistant)
            CHECK_HOMEASSISTANT=0
            shift
            ;;
        --no-stage-ui)
            CHECK_STAGE_UI=0
            shift
            ;;
        --stage-ui-only)
            STAGE_UI_ONLY=1
            shift
            ;;
        --no-hardware)
            CHECK_HARDWARE=0
            shift
            ;;
        --no-tracker)
            CHECK_TRACKER=0
            shift
            ;;
        --launcher-start-post)
            LAUNCHER_START_POST=1
            shift
            ;;
        --reset-runtime)
            RESET_RUNTIME_ONLY=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -z "$RELEASE_DIR" ]]; then
    if [[ -z "$VERSION" ]]; then
        echo "[ERROR] VERSION is empty. Pass --version or create VERSION next to health-check.sh." >&2
        exit 1
    fi
    RELEASE_DIR="$REAL_HOME/clawstage/releases/$VERSION"
fi

RESOLVED_RELEASE_DIR="$(readlink -f "$RELEASE_DIR" 2>/dev/null || true)"
if [[ -n "$RESOLVED_RELEASE_DIR" ]]; then
    RELEASE_DIR="$RESOLVED_RELEASE_DIR"
fi
STAGE_STACK_DIR="$RELEASE_DIR/homeassistant/stage-stack"
STAGE_STACK_COMPOSE="$STAGE_STACK_DIR/compose.release.yaml"

info() { echo "[INFO] $*"; }
success() { echo "[SUCCESS] $*"; }
warn() { echo "[WARN] $*" >&2; }
err() { echo "[ERROR] $*" >&2; }

validate_non_negative_integer() {
    local name="$1"
    local value="$2"
    if [[ ! "$value" =~ ^[0-9]+$ ]]; then
        err "$name must be a non-negative integer, got: $value"
        exit 1
    fi
}

validate_positive_integer() {
    local name="$1"
    local value="$2"
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        err "$name must be a positive integer, got: $value"
        exit 1
    fi
}

ensure_release() {
    if [[ -z "$RELEASE_DIR" || ! -d "$RELEASE_DIR" ]]; then
        err "Release directory not found: ${RELEASE_DIR:-unset}"
        exit 1
    fi

    local current_target
    current_target="$(readlink -f "$REAL_HOME/clawstage/current" 2>/dev/null || true)"
    if [[ "$current_target" != "$RELEASE_DIR" ]]; then
        err "Release is not active: current=${current_target:-unset} expected=$RELEASE_DIR"
        exit 1
    fi
}

run_as_real_user() {
    local uid
    uid="$(id -u "$REAL_USER")"
    if [[ "$EUID" -eq 0 ]]; then
        sudo -u "$REAL_USER" env \
            HOME="$REAL_HOME" \
            USER="$REAL_USER" \
            LOGNAME="$REAL_USER" \
            HOORII_GATEWAY_REAL_USER="$REAL_USER" \
            HOORII_GATEWAY_REAL_HOME="$REAL_HOME" \
            XDG_RUNTIME_DIR="/run/user/$uid" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" \
            "$@"
    else
        env \
            HOME="$REAL_HOME" \
            USER="$REAL_USER" \
            LOGNAME="$REAL_USER" \
            HOORII_GATEWAY_REAL_USER="$REAL_USER" \
            HOORII_GATEWAY_REAL_HOME="$REAL_HOME" \
            XDG_RUNTIME_DIR="/run/user/$uid" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" \
            "$@"
    fi
}

systemctl_property() {
    local unit="$1"
    local property="$2"
    systemctl show "$unit" --property="$property" --value 2>/dev/null || true
}

user_systemctl_property() {
    local unit="$1"
    local property="$2"
    run_as_real_user systemctl --user show "$unit" --property="$property" --value 2>/dev/null || true
}

check_system_service() {
    local unit="$1"
    local expected_substate="${2:-running}"
    local expected_active="${3:-active}"
    local load active sub result status pid

    if ! command -v systemctl >/dev/null 2>&1; then
        err "systemctl is unavailable; cannot verify $unit"
        return 1
    fi

    load="$(systemctl_property "$unit" LoadState)"
    active="$(systemctl_property "$unit" ActiveState)"
    sub="$(systemctl_property "$unit" SubState)"
    result="$(systemctl_property "$unit" Result)"
    status="$(systemctl_property "$unit" ExecMainStatus)"
    pid="$(systemctl_property "$unit" MainPID)"

    if [[ "$load" != "loaded" || "$active" != "$expected_active" || "$sub" != "$expected_substate" ]]; then
        err "$unit is not healthy (load=${load:-unknown}, active=${active:-unknown}, sub=${sub:-unknown})"
        return 1
    fi
    if [[ -n "$result" && "$result" != "success" ]]; then
        err "$unit result is ${result:-unknown}"
        return 1
    fi
    if [[ -n "$status" && "$status" != "0" ]]; then
        err "$unit main process exited with status $status"
        return 1
    fi
    if [[ "$expected_substate" == "running" && (! "$pid" =~ ^[1-9][0-9]*$) ]]; then
        err "$unit has no live MainPID (MainPID=${pid:-unset})"
        return 1
    fi

    info "$unit is $active/$sub (MainPID=${pid:-none}, restarts=$(systemctl_property "$unit" NRestarts))"
}

check_user_service() {
    local unit="$1"
    local load active sub result status pid

    if ! command -v systemctl >/dev/null 2>&1; then
        err "systemctl is unavailable; cannot verify user service $unit"
        return 1
    fi

    load="$(user_systemctl_property "$unit" LoadState)"
    active="$(user_systemctl_property "$unit" ActiveState)"
    sub="$(user_systemctl_property "$unit" SubState)"
    result="$(user_systemctl_property "$unit" Result)"
    status="$(user_systemctl_property "$unit" ExecMainStatus)"
    pid="$(user_systemctl_property "$unit" MainPID)"

    if [[ "$load" != "loaded" || "$active" != "active" || "$sub" != "running" ]]; then
        err "$unit user service is not healthy (load=${load:-unknown}, active=${active:-unknown}, sub=${sub:-unknown})"
        return 1
    fi
    if [[ -n "$result" && "$result" != "success" ]]; then
        err "$unit user service result is ${result:-unknown}"
        return 1
    fi
    if [[ -n "$status" && "$status" != "0" ]]; then
        err "$unit user service main process exited with status $status"
        return 1
    fi
    if [[ ! "$pid" =~ ^[1-9][0-9]*$ ]]; then
        err "$unit user service has no live MainPID (MainPID=${pid:-unset})"
        return 1
    fi

    info "$unit user service is active/running (MainPID=$pid, restarts=$(user_systemctl_property "$unit" NRestarts))"
}

http_code() {
    local method="$1"
    local url="$2"
    local data="${3:-}"
    local args=(-sS -o /dev/null -w '%{http_code}' --max-time "$CURL_TIMEOUT_SECONDS" -X "$method")
    if [[ -n "$data" ]]; then
        args+=(-H 'Content-Type: application/json' --data "$data")
    fi
    curl "${args[@]}" "$url" 2>/dev/null || true
}

check_http_code() {
    local name="$1"
    local method="$2"
    local url="$3"
    local expected="$4"
    local data="${5:-}"
    local code
    code="$(http_code "$method" "$url" "$data")"
    if [[ "$code" != "$expected" ]]; then
        err "$name endpoint is not ready: ${code:-no response} (expected HTTP $expected, $url)"
        return 1
    fi
    info "$name endpoint is healthy (HTTP $code, $url)"
}

check_matter_websocket() {
    local url="http://127.0.0.1:5580/ws"
    local code

    # The Matter server is a WebSocket API. HTTP 101 proves that the process
    # has completed protocol initialization, rather than only opening its TCP
    # listener. curl remains connected after a successful upgrade and reaches
    # max-time; it still emits the received HTTP status via --write-out.
    code="$(curl --http1.1 -sS -o /dev/null -w '%{http_code}' \
        --max-time "$CURL_TIMEOUT_SECONDS" \
        -H 'Connection: Upgrade' \
        -H 'Upgrade: websocket' \
        -H 'Sec-WebSocket-Version: 13' \
        -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
        "$url" 2>/dev/null || true)"
    if [[ "$code" != "101" ]]; then
        err "Matter server WebSocket is not ready: ${code:-no response} (expected HTTP 101, $url)"
        return 1
    fi
    info "Matter server WebSocket is healthy (HTTP $code, $url)"
}

check_launcher() {
    if [[ "$LAUNCHER_START_POST" -eq 1 ]]; then
        check_system_service "clawstage-startup.service" "start-post" "activating"
    else
        check_system_service "clawstage-startup.service" "exited"
    fi
}

check_xmos() {
    info "Checking XMOS wakeword..."
    timeout 30 "$RELEASE_DIR/xmos-oww/runtime/run-wakeword" --check-models >/dev/null || {
        err "XMOS native model inference verification failed"
        return 1
    }
    check_user_service "xmos-oww.service" || return 1

    local main_pid parec_pid parec_state
    main_pid="$(user_systemctl_property "xmos-oww.service" MainPID)"
    parec_pid="$(pgrep -P "$main_pid" -x parec 2>/dev/null | head -n 1 || true)"
    if [[ -z "$parec_pid" ]]; then
        err "XMOS detector is active but its parec capture process is not running"
        return 1
    fi
    parec_state="$(ps -o stat= -p "$parec_pid" 2>/dev/null | tr -d '[:space:]' || true)"
    if [[ -z "$parec_state" || "$parec_state" == Z* ]]; then
        err "XMOS parec capture process is not healthy (PID=$parec_pid, state=${parec_state:-missing})"
        return 1
    fi
    info "XMOS audio capture process is running (PID=$parec_pid, state=$parec_state)"
}

check_audio_routing() {
    local sink source
    sink="$(run_as_real_user pactl get-default-sink 2>/dev/null)" || return 1
    source="$(run_as_real_user pactl get-default-source 2>/dev/null)" || return 1
    if [[ "${sink,,}" != alsa_output.usb-xmos_* || "${source,,}" != alsa_input.usb-xmos_* || "$source" == *.monitor ]]; then
        err "Default audio must use XMOS input/output (sink=$sink source=$source)"
        return 1
    fi
    run_as_real_user pactl list sinks short | awk -v node="$sink" '$2 == node {found=1} END {exit !found}' || return 1
    run_as_real_user pactl list sources short | awk -v node="$source" '$2 == node {found=1} END {exit !found}' || return 1
    info "Default audio input/output use available XMOS nodes"
}

check_fan_control() {
    if [[ ! -p /tmp/fanctl ]] || ! run_as_real_user test -w /tmp/fanctl; then
        err "Fan control FIFO is missing or not writable by $REAL_USER"
        return 1
    fi
}

check_others() {
    info "Checking hardware services..."
    local failed=0
    local hardware_runtime="$RELEASE_DIR/others/runtime"
    local required

    check_system_service "gpio25-power-enable.service" "exited" || failed=1
    if ! pinctrl get 25 2>/dev/null | grep -Eq '^25:[[:space:]]+op[[:space:]]+dh([[:space:]]|$)'; then
        err "GPIO25 screen/peripheral power is not configured output HIGH"
        failed=1
    fi

    check_system_service "oww-servo.service" || failed=1
    check_system_service "light-sensor.service" || failed=1
    check_system_service "desk-shaking.service" || failed=1
    check_system_service "privacy-button-state.service" || failed=1
    check_system_service "factory-reset-button.service" || failed=1
    check_system_service "fan-daemon.service" || failed=1
    check_system_service "stage-boot-config.service" "exited" || failed=1
    check_system_service "clawstage-user-timezone.service" || failed=1
    check_audio_routing || { err "XMOS default audio routing check failed"; failed=1; }
    check_fan_control || failed=1

    for required in robotarm stepper-position-service face-tracker-service; do
        if [[ ! -x "$hardware_runtime/$required" ]] \
            || ! "$hardware_runtime/$required" --version >/dev/null 2>&1; then
            err "Native hardware runtime is missing or cannot run: $required"
            failed=1
        fi
    done
    if [[ ! -x "$hardware_runtime/yunet_tracker" ]]; then
        err "Native face detection runtime is missing"
        failed=1
    fi

    # A malformed request exercises the rotation HTTP parser without moving the
    # hardware. The service's expected response is HTTP 400.
    check_http_code "Display rotation" POST "http://127.0.0.1:5001/move" 400 '{}' || failed=1
    # Status reads the ADC but never energizes the stepper motor.
    check_http_code "Display position sensor" GET "http://127.0.0.1:5001/status" 200 || failed=1

    if [[ "$CHECK_TRACKER" -eq 1 ]]; then
        check_system_service "face-tracker.service" || failed=1
        # GET on this POST-only route proves the control route is registered but
        # does not start the camera/tracker process.
        check_http_code "Face tracker" GET "http://127.0.0.1:5002/tracker/start" 405 || failed=1
    else
        info "Face tracker health check skipped by option"
    fi

    [[ "$failed" -eq 0 ]]
}

has_connected_ble_device() {
    local device_path connected

    while read -r device_path; do
        [[ "$device_path" =~ ^/org/bluez/hci0/dev_[^/]+$ ]] || continue
        connected="$(busctl get-property \
            org.bluez \
            "$device_path" \
            org.bluez.Device1 \
            Connected 2>/dev/null || true)"
        if [[ "$connected" == "b true" ]]; then
            info "BLE device is connected: $device_path"
            return 0
        fi
    done < <(busctl tree --list org.bluez 2>/dev/null || true)

    return 1
}

check_shadowlink() {
    info "Checking Shadowlink BLE..."
    check_system_service "bluetooth.service" || return 1
    check_system_service "shadowlink-ble-server.service" || return 1

    if ! command -v busctl >/dev/null 2>&1; then
        err "busctl is unavailable; cannot verify the Bluetooth adapter"
        return 1
    fi
    local powered active_instances
    powered="$(busctl get-property org.bluez /org/bluez/hci0 org.bluez.Adapter1 Powered 2>/dev/null || true)"
    if [[ "$powered" != "b true" ]]; then
        err "Bluetooth adapter hci0 is not powered (D-Bus value: ${powered:-unavailable})"
        return 1
    fi
    active_instances="$(busctl get-property org.bluez /org/bluez/hci0 org.bluez.LEAdvertisingManager1 ActiveInstances 2>/dev/null || true)"
    if [[ ! "$active_instances" =~ [[:space:]]([1-9][0-9]*)$ ]]; then
        if has_connected_ble_device; then
            info "No active LE advertisement while a BLE device is connected; GATT session is healthy"
        else
            err "Bluetooth adapter has no active LE advertisement or connected BLE device (D-Bus value: ${active_instances:-unavailable})"
            return 1
        fi
    fi

    local invocation logs
    invocation="$(systemctl_property "shadowlink-ble-server.service" InvocationID)"
    if [[ -n "$invocation" ]]; then
        logs="$(journalctl "_SYSTEMD_INVOCATION_ID=$invocation" --no-pager 2>/dev/null || true)"
    else
        logs="$(journalctl -u shadowlink-ble-server.service -b --no-pager 2>/dev/null || true)"
    fi
    if ! grep -qF "GATT application registered successfully" <<< "$logs"; then
        err "Shadowlink service is running but GATT registration has not succeeded"
        return 1
    fi
    if ! grep -qE "Advertisement (registered successfully|already registered)" <<< "$logs"; then
        err "Shadowlink service is running but BLE advertising registration has not succeeded"
        return 1
    fi
    info "Shadowlink GATT application and BLE advertisement are registered"
}

# Native client for new releases; retain compatibility with installed Python releases.
run_update_ctl() {
    local client="$1"
    shift
    case "$client" in
        *.py) python3 "$client" "$@" ;;
        *) "$client" "$@" ;;
    esac
}

check_software_update() {
    info "Checking Software Update controller..."
    check_system_service "software-update.service" || return 1
    local distro_unit
    for distro_unit in swupdate.service swupdate.socket; do
        if systemctl is-enabled --quiet "$distro_unit" 2>/dev/null; then
            err "Unused distro $distro_unit must be disabled; the product controller invokes SWUpdate directly"
            return 1
        fi
    done
    local ctl="$RELEASE_DIR/software-update/runtime/software-updatectl"
    [ -f "$ctl" ] || ctl="$RELEASE_DIR/software-update/software_update_ctl.py"
    # Compatibility with releases installed with the merged display-shell directory.
    [ -f "$ctl" ] || ctl="${ctl/\/software-update\//\/display-shell\/}"
    [[ -f "$ctl" ]] || {
        warn "Software Update client not found: $ctl"
        return 1
    }
    run_update_ctl "$ctl" status >/dev/null 2>&1 || {
        warn "Software Update controller socket is unavailable"
        return 1
    }
    if [[ -f "$RELEASE_DIR/display-shell/install-state.sh" ]]; then
        check_system_service "clawstage-local-websocket.service" || return 1
        python3 - <<'PY' || return 1
import asyncio
import websockets

async def check():
    async with websockets.connect("ws://127.0.0.1:9003/ws", open_timeout=2):
        pass

asyncio.run(check())
PY
        if run_as_real_user systemctl --user show-environment 2>/dev/null | grep -q '^WAYLAND_DISPLAY='; then
            check_user_service "clawstage-display.service" || return 1
        fi
    fi
}

check_openclaw() {
    info "Checking OpenClaw gateway..."

    local status_script="$RELEASE_DIR/openclaw/hoorii_claw/script/status.sh"
    if [[ ! -x "$status_script" ]]; then
        err "OpenClaw status script not found or not executable: $status_script"
        return 1
    fi

    # An explicitly cleared device is healthy while waiting for provisioning.
    # Do not require HTTP/Agent readiness from a Gateway that must stay stopped.
    local binding_state
    binding_state="$(python3 "$RELEASE_DIR/openclaw/hoorii_claw/script/internal/provision-runtime.py" binding-state "$RELEASE_DIR")" || return 1
    if [[ "$binding_state" == unbound ]]; then
        [[ -x "$RELEASE_DIR/openclaw/home/node/bin/node" && -f "$RELEASE_DIR/openclaw/home/openclaw/node_modules/openclaw/dist/index.js" ]] || return 1
        success "OpenClaw runtime present; unbound device awaits provisioning"
        return 0
    fi
    if ! run_as_real_user bash "$status_script" --check; then
        err "OpenClaw gateway status check failed"
        return 1
    fi
    # Keep the outer HTTP probe as an independent check of the release health
    # contract even though status.sh also treats the plugin endpoint as fatal.
    check_http_code "OpenClaw plugin" GET "http://127.0.0.1:18800/status" 200 || return 1
    local node_bin="$RELEASE_DIR/openclaw/home/node/bin/node"
    local openclaw_entry="$RELEASE_DIR/openclaw/home/openclaw/node_modules/openclaw/dist/index.js"
    local tasks_json
    if [[ ! -x "$node_bin" || ! -f "$openclaw_entry" ]]; then
        err "OpenClaw release-local task runtime is missing"
        return 1
    fi
    if ! tasks_json="$(curl --silent --show-error --fail --max-time "$OPENCLAW_TASKS_TIMEOUT_SECONDS" \
        http://127.0.0.1:18800/task-runtime 2>/dev/null)"; then
        err "OpenClaw background task runtime check failed or timed out"
        return 1
    fi
    if ! "$node_bin" -e '
const value = JSON.parse(process.argv[1]);
if (value.ready !== true || !["task-registry", "native-subagent"].includes(value.mode)) process.exit(1);
' "$tasks_json" 2>/dev/null; then
        err "OpenClaw background task runtime is not ready"
        return 1
    fi
    info "OpenClaw background task runtime is ready"
    success "OpenClaw gateway and plugin are healthy"
}

compose_service_id() {
    local service="$1"
    if docker compose version >/dev/null 2>&1; then
        docker compose -f "$STAGE_STACK_COMPOSE" ps -q "$service" 2>/dev/null || true
    elif command -v docker-compose >/dev/null 2>&1; then
        docker-compose -f "$STAGE_STACK_COMPOSE" ps -q "$service" 2>/dev/null || true
    fi
}

check_container() {
    local service="$1"
    local container_id state status running restarting health
    container_id="$(compose_service_id "$service")"
    if [[ -z "$container_id" ]]; then
        err "HomeAssistant compose service has no container: $service"
        return 1
    fi

    state="$(docker inspect --format '{{.State.Status}}|{{.State.Running}}|{{.State.Restarting}}|{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "$container_id" 2>/dev/null || true)"
    IFS='|' read -r status running restarting health <<< "$state"
    if [[ "$status" != "running" || "$running" != "true" || "$restarting" != "false" ]]; then
        err "$service container is not running normally (state=${state:-unavailable})"
        return 1
    fi
    if [[ "$health" != "none" && "$health" != "healthy" ]]; then
        err "$service container health is $health"
        return 1
    fi
    info "$service container is running (health=$health, restarts=$(docker inspect --format '{{.RestartCount}}' "$container_id" 2>/dev/null || echo unknown))"
}

check_matter_certificates() {
    local certificates="$RELEASE_DIR/user/.hoorii/stage-stack/matter/data/credentials"
    if ! find "$certificates" -maxdepth 1 -type f \
        \( -name '*.der' -o -name '*.pem' \) -size +0c -print -quit 2>/dev/null \
        | grep . >/dev/null; then
        err "Matter public PAA certificates are missing; commissioning cannot work"
        return 1
    fi
}

check_homeassistant() {
    if [[ "$CHECK_HOMEASSISTANT" -eq 0 ]]; then
        info "HomeAssistant health check skipped by option"
        return 0
    fi

    info "Checking HomeAssistant stack..."
    if [[ ! -f "$STAGE_STACK_COMPOSE" ]]; then
        err "HomeAssistant compose file not found: $STAGE_STACK_COMPOSE"
        return 1
    fi
    if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
        err "Docker daemon is unavailable"
        return 1
    fi

    local failed=0
    # HA HTTP initialization may outlast the OTA verification window. Accept
    # its container state here; HTTP readiness is not a release commit gate.
    check_container "homeassistant" || failed=1
    check_container "matter-server" || failed=1
    check_container "otbr" || failed=1
    check_matter_websocket || failed=1
    check_matter_certificates || failed=1
    check_http_code "OTBR REST API" GET "http://127.0.0.1:8081/node" 200 || failed=1
    [[ "$failed" -eq 0 ]]
}

check_stage_ui() {
    if [[ "$CHECK_STAGE_UI" -eq 0 ]]; then
        info "Stage UI health check skipped by option"
        return 0
    fi

    info "Checking Stage UI..."
    local failed=0
    check_http_code "Stage UI static server" GET "http://127.0.0.1:3000/" 200 || failed=1
    check_http_code "Stage UI hardware relay" GET "http://127.0.0.1:9001/" 200 || failed=1
    check_http_code "Stage UI software relay" GET "http://127.0.0.1:9002/" 200 || failed=1
    # Listening servers do not prove the kiosk loaded or acknowledged a probe.
    # Chromium may restart inside the unit while systemd NRestarts stays zero.
    local page_health
    if ! page_health="$(curl --silent --show-error --fail --max-time "$CURL_TIMEOUT_SECONDS" \
        http://127.0.0.1:9002/kiosk-health 2>/dev/null)"; then
        err "Stage UI page health endpoint is unavailable"
        failed=1
    elif ! python3 -c '
import json, math, sys
try:
    value = json.load(sys.stdin)
    age = value.get("ackAgeMs")
    valid = (value.get("healthy") is True and value.get("connected") is True
             and type(age) in (int, float) and math.isfinite(age) and 0 <= age < 15000)
except (ValueError, AttributeError, TypeError):
    valid = False
sys.exit(0 if valid else 1)
' <<< "$page_health"; then
        err "Stage UI page has no fresh health acknowledgement"
        failed=1
    else
        info "Stage UI page health acknowledgement is fresh"
    fi
    [[ "$failed" -eq 0 ]]
}

run_named_check() {
    local name="$1"
    shift
    if "$@"; then
        success "$name check passed"
    else
        FAILED_CHECKS+=("$name")
    fi
}

run_all_checks() {
    FAILED_CHECKS=()
    if [[ "${STAGE_UI_ONLY:-0}" -eq 1 ]]; then
        run_named_check "Stage UI" check_stage_ui
        [[ "${#FAILED_CHECKS[@]}" -eq 0 ]]
        return
    fi
    if [[ "${RESET_RUNTIME_ONLY:-0}" -eq 1 ]]; then
        # Account reset leaves Gateway stopped until provisioning rebuilds it.
        run_named_check "HomeAssistant stack" check_homeassistant
        [[ "${#FAILED_CHECKS[@]}" -eq 0 ]]
        return
    fi
    run_named_check "startup launcher" check_launcher
    if [[ "$CHECK_HARDWARE" -eq 1 ]]; then
        run_named_check "hardware services" check_others
    else
        info "Hardware services NOT CHECKED (--no-hardware)"
    fi
    run_named_check "XMOS" check_xmos
    run_named_check "Software Update" check_software_update
    run_named_check "Shadowlink" check_shadowlink
    run_named_check "OpenClaw" check_openclaw
    run_named_check "HomeAssistant" check_homeassistant
    run_named_check "Stage UI" check_stage_ui
    [[ "${#FAILED_CHECKS[@]}" -eq 0 ]]
}

collect_restart_snapshot() {
    local unit value snapshot=""
    if [[ "${STAGE_UI_ONLY:-0}" -eq 1 ]]; then
        user_systemctl_property clawstage-stage-ui.service NRestarts
        return
    fi
    if [[ "${RESET_RUNTIME_ONLY:-0}" -eq 1 ]]; then
        local service container_id
        for service in homeassistant matter-server otbr; do
            container_id="$(compose_service_id "$service")"
            value="missing"
            if [[ -n "$container_id" ]]; then
                value="$(docker inspect --format '{{.RestartCount}}' "$container_id" 2>/dev/null || true)"
            fi
            snapshot+="container:$service=${value:-unknown};"
        done
        printf '%s' "$snapshot"
        return
    fi
    for unit in \
        clawstage-startup.service \
        gpio25-power-enable.service \
        oww-servo.service \
        light-sensor.service \
        desk-shaking.service \
        privacy-button-state.service \
        factory-reset-button.service \
        fan-daemon.service \
        stage-boot-config.service \
        clawstage-user-timezone.service \
        software-update.service \
        shadowlink-ble-server.service; do
        if [[ "$CHECK_HARDWARE" -eq 0 ]]; then
            case "$unit" in
                clawstage-startup.service|software-update.service|shadowlink-ble-server.service) ;;
                *) continue ;;
            esac
        fi
        value="$(systemctl_property "$unit" NRestarts)"
        snapshot+="$unit=${value:-unknown};"
    done
    if [[ "$CHECK_HARDWARE" -eq 1 && "$CHECK_TRACKER" -eq 1 ]]; then
        value="$(systemctl_property "face-tracker.service" NRestarts)"
        snapshot+="face-tracker.service=${value:-unknown};"
    fi
    value="$(user_systemctl_property "xmos-oww.service" NRestarts)"
    snapshot+="user:xmos-oww.service=${value:-unknown};"
    value="$(user_systemctl_property "clawstage-openclaw.service" NRestarts)"
    snapshot+="user:clawstage-openclaw.service=${value:-unknown};"

    local service container_id
    if [[ "$CHECK_HOMEASSISTANT" -eq 1 ]]; then
        for service in homeassistant matter-server otbr; do
            container_id="$(compose_service_id "$service")"
            if [[ -n "$container_id" ]]; then
                value="$(docker inspect --format '{{.RestartCount}}' "$container_id" 2>/dev/null || true)"
            else
                value="missing"
            fi
            snapshot+="container:$service=${value:-unknown};"
        done
    fi
    printf '%s' "$snapshot"
}

wait_for_health() {
    local attempt=1
    local deadline
    local before_snapshot after_snapshot

    if [[ "$STARTUP_GRACE_SECONDS" -gt 0 ]]; then
        info "Waiting ${STARTUP_GRACE_SECONDS}s for services to initialize before health checks..."
        sleep "$STARTUP_GRACE_SECONDS"
    fi
    deadline=$((SECONDS + TIMEOUT_SECONDS))

    while true; do
        info "Health-check attempt $attempt"
        if run_all_checks; then
            before_snapshot="$(collect_restart_snapshot)"
            if [[ "$STABILITY_SECONDS" -gt 0 ]]; then
                info "All checks passed; confirming stability for ${STABILITY_SECONDS}s..."
                sleep "$STABILITY_SECONDS"
            fi
            if run_all_checks; then
                after_snapshot="$(collect_restart_snapshot)"
                if [[ "$before_snapshot" == "$after_snapshot" ]]; then
                    success "All components remained healthy and stable"
                    return 0
                fi
                warn "A service restarted during the stability window; retrying"
            fi
        fi

        if [[ "$SECONDS" -ge "$deadline" ]]; then
            err "Health check timed out after ${TIMEOUT_SECONDS}s; failing checks: ${FAILED_CHECKS[*]:-service stability}"
            return 1
        fi
        warn "Checks not ready: ${FAILED_CHECKS[*]:-service stability}; retrying in ${RETRY_INTERVAL_SECONDS}s"
        sleep "$RETRY_INTERVAL_SECONDS"
        attempt=$((attempt + 1))
    done
}

set_default_health_timeout() {
    [[ -z "$TIMEOUT_SECONDS" ]] || return 0
    # Matter fetches public PAA certificates before opening its API on a
    # clean device. Keep normal OTA bounded without rejecting that first boot.
    if [[ ! -f "$REAL_HOME/clawstage/clawstage_base_installed.flag" \
        || -f "$REAL_HOME/clawstage/.factory-image-needs-verification" ]]; then
        TIMEOUT_SECONDS=480
    else
        TIMEOUT_SECONDS=120
    fi
}

main() {
    if [[ "$STAGE_UI_ONLY" -eq 1 && ( "$CHECK_STAGE_UI" -eq 0 || "$RESET_RUNTIME_ONLY" -eq 1 ) ]]; then
        err "--stage-ui-only cannot be combined with --no-stage-ui or --reset-runtime"
        return 1
    fi
    set_default_health_timeout
    validate_non_negative_integer "startup grace" "$STARTUP_GRACE_SECONDS"
    validate_non_negative_integer "timeout" "$TIMEOUT_SECONDS"
    validate_non_negative_integer "retry interval" "$RETRY_INTERVAL_SECONDS"
    validate_non_negative_integer "stability window" "$STABILITY_SECONDS"
    validate_non_negative_integer "curl timeout" "$CURL_TIMEOUT_SECONDS"
    validate_positive_integer "OpenClaw task timeout" "$OPENCLAW_TASKS_TIMEOUT_SECONDS"
    ensure_release
    if [[ -f "$RELEASE_DIR/.skip-hardware-check" ]]; then
        CHECK_HARDWARE=0
    fi

    info "Using release directory: $RELEASE_DIR"
    info "Health timing: grace=${STARTUP_GRACE_SECONDS}s timeout=${TIMEOUT_SECONDS}s retry=${RETRY_INTERVAL_SECONDS}s stability=${STABILITY_SECONDS}s"
    wait_for_health
    success "Health check finished"
}

main "$@"
