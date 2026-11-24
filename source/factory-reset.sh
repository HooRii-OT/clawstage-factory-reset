#!/usr/bin/env bash
set -Eeuo pipefail

# Clear account state and restore HomeAssistant. Like BLE unbind, leave
# OpenClaw cleared; provisioning rebuilds it through apply-user-config.sh.
# The durable phase prevents retries from clearing prepared data again.
REAL_USER="${CLAWSTAGE_REAL_USER:-pi}"
REAL_HOME="${CLAWSTAGE_REAL_HOME:-/home/$REAL_USER}"
STATE_DIR="${CLAWSTAGE_STATE_DIR:-$REAL_HOME/clawstage}"
RESET_MARKER="${CLAWSTAGE_RESET_MARKER:-$STATE_DIR/.factory-reset-in-progress}"
PAA_CACHE="$STATE_DIR/.factory-reset-paa"
LOCK_PATH="${CLAWSTAGE_MAINTENANCE_LOCK:-/run/lock/clawstage-maintenance.lock}"
RELEASE_DIR=""
SUPPORT_DIR=""
EXPECTED_RELEASE=""
MODE=reset
FROM_SHADOWLINK=false
STEP=preflight
STEP_STARTED=$SECONDS
RESET_STARTED=$SECONDS
FAILURE_REASON=""
RESTART_UPDATE=false
RESTART_FACTORY=false
OTA_STATE="${SOFTWARE_UPDATE_STATE_FILE:-/var/lib/clawstage-software-update/state.json}"
OTA_WANTS="${SOFTWARE_UPDATE_BOOT_VERIFY_WANTS:-/etc/systemd/system/multi-user.target.wants/clawstage-boot-verify.service}"
FACTORY_RUNTIME="${CLAWSTAGE_FACTORY_RUNTIME:-/run/clawstage-factory}"
SUPPORT_MESSAGE='If you have any binding-related issues, please copy all logs produced by this tool and send them to support@hoorii.io. We will help you.'

info() { printf '[%(%Y-%m-%dT%H:%M:%S%z)T] [FACTORY-RESET] %s\n' -1 "$*"; }
error() {
    local code=$?
    [ "$code" -ne 0 ] || code=1
    FAILURE_REASON="$*"
    info "ERROR step=$STEP exit_code=$code reason=$FAILURE_REASON" >&2
    exit "$code"
}
step_done() { info "STEP OK name=$STEP elapsed_seconds=$((SECONDS - STEP_STARTED))"; }
step() {
    step_done
    STEP="$1"
    STEP_STARTED=$SECONDS
    info "STEP START name=$STEP description=$2"
}

# Read only an allowlist of diagnostic fields. Never dump config.json, the
# environment, pairing codes, private keys, tokens, or account identifiers.
snapshot_state() {
    local label="$1" config="$RELEASE_DIR/user/.hoorii/config.json" summary path state
    [ -n "$RELEASE_DIR" ] || return 0
    info "STATE snapshot=$label config_path=$config"
    if [ -f "$config" ]; then
        stat -Lc 'config_file uid=%u gid=%g mode=%a bytes=%s' "$config" || true
        if summary="$(jq -ce '
            def field_state($key):
                if type != "object" then "invalid-section"
                elif (has($key) | not) then "missing"
                elif .[$key] == "" then "empty"
                elif (.[$key] | type) == "string" then "present"
                else "invalid-type" end;
            if type != "object" then error("config root must be an object") else . end |
            (if (.commissioning | type) == "object" then .commissioning else . end) as $c |
            {commissioning_source: (if (.commissioning | type) == "object" then "commissioning" else "root" end),
             commissioning_flag: (if ($c | has("commissioning_flag") | not) then "missing"
                 elif ($c.commissioning_flag | type) == "number" then $c.commissioning_flag
                 elif $c.commissioning_flag == "0" or $c.commissioning_flag == "1" then $c.commissioning_flag
                 else "invalid-type-or-value" end),
             kpersistent: ($c | field_state("kpersistent")),
             device_identity: {qrcode: ($c | field_state("qrcode")),
                 long_term_private_key: ($c | field_state("long_term_private_key")),
                 passcode_present: ($c | has("passcode")), discriminator_present: ($c | has("discriminator"))},
             userinfo_type: (.userinfo | type),
             userinfo: {hub_id: (.userinfo | field_state("hub_id")),
                 hub_token: (.userinfo | field_state("hub_token")),
                 union_id: (.userinfo | field_state("union_id")),
                 ha_token: (.userinfo | field_state("ha_token"))},
             userinfo_cleared: (.userinfo | if type == "object" then
                 has("hub_id") and has("hub_token") and has("union_id") and has("ha_token")
                 and all(.[]; . == "") else false end),
             userinfo_extra_field_count: (.userinfo | if type == "object" then
                 (keys - ["hub_id", "hub_token", "union_id", "ha_token"] | length) else null end)}
        ' "$config" 2>/dev/null)"; then
            info "STATE snapshot=$label binding=$summary"
        else
            info "WARNING snapshot=$label config unreadable or invalid JSON/schema; binding state is UNKNOWN (jq exit_code=$?)"
        fi
    else
        info "WARNING snapshot=$label config missing or not a regular file; binding state is UNKNOWN"
    fi
    if [ -f "$RESET_MARKER" ] && [ ! -L "$RESET_MARKER" ]; then
        if summary="$(jq -ce '{version,release,phase,original_ready}' "$RESET_MARKER" 2>/dev/null)"; then
            info "STATE snapshot=$label reset_marker=$RESET_MARKER recovery=$summary"
        else
            info "WARNING snapshot=$label reset_marker=$RESET_MARKER unreadable or invalid"
        fi
    else
        state=absent
        [ ! -e "$RESET_MARKER" ] && [ ! -L "$RESET_MARKER" ] || state=unsafe
        info "STATE snapshot=$label reset_marker=$RESET_MARKER state=$state"
    fi
    path="$RELEASE_DIR/.device-unbind.json"
    if [ -f "$path" ] && [ ! -L "$path" ]; then
        if summary="$(jq -ce '{requestId,action,confirmed,cloud_request_attempted,local_only,
            cleanup_complete,config,clear,purge}' "$path" 2>/dev/null)"; then
            info "STATE snapshot=$label unbind_journal=$summary (confirmed records local intent, not cloud success)"
        else
            info "WARNING snapshot=$label unbind_journal unreadable or invalid"
        fi
    else
        state=absent
        [ ! -e "$path" ] && [ ! -L "$path" ] || state=unsafe
        info "STATE snapshot=$label unbind_journal=$state"
    fi
    for path in "$RELEASE_DIR/.clawstage-release-ready" "$RELEASE_DIR/user/.openclaw" \
        "$RELEASE_DIR/user/.clawstage-openclaw-user-settings.json" "$PAA_CACHE"; do
        state=absent
        [ ! -e "$path" ] && [ ! -L "$path" ] || state=present
        info "STATE snapshot=$label path=$path state=$state"
    done
}

finish_logging() {
    local code="$1" result=FAILED
    trap - ERR EXIT HUP INT TERM
    set +e
    # Restart only services that were running before this explicit invocation.
    # On failure the durable reset marker continues to inhibit new OTA/binding.
    if $RESTART_UPDATE; then
        systemctl start software-update.service || info "WARNING Software Update restart failed"
    fi
    if $RESTART_FACTORY; then
        systemctl start clawstage-factory-console.service || info "WARNING Factory console restart failed"
    fi
    snapshot_state final
    if [ "$code" -eq 0 ]; then
        step_done
        result=SUCCESS
        [ "$MODE" != resume ] || result=PREPARED_PENDING_VERIFICATION
    fi
    info "RESULT=$result step=$STEP exit_code=$code elapsed_seconds=$((SECONDS - RESET_STARTED)) phase=${phase:-not-started} reason=${FAILURE_REASON:-none}"
    [ "${CLAWSTAGE_RESET_SUPPORT_FROM_WRAPPER:-0}" = 1 ] || printf '%s\n' "$SUPPORT_MESSAGE"
    exit "$code"
}
unexpected_failure() {
    local code="$1" line="$2" command="$3"
    FAILURE_REASON="command failed at factory-reset.sh:$line"
    printf '[FACTORY-RESET-ERROR] step=%s exit_code=%s line=%s command=%q\n' "$STEP" "$code" "$line" "$command" >&2
    exit "$code"
}
trap 'unexpected_failure "$?" "$LINENO" "$BASH_COMMAND"' ERR
trap 'finish_logging "$?"' EXIT
trap 'FAILURE_REASON="received SIGHUP"; exit 129' HUP
trap 'FAILURE_REASON="received SIGINT"; exit 130' INT
trap 'FAILURE_REASON="received SIGTERM"; exit 143' TERM
export SHADOWLINK_DEBUG=1

info "STEP START name=$STEP description=Validate invocation and active release"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --release-dir)
            [ -n "${2:-}" ] || error "--release-dir requires a value"
            RELEASE_DIR="$2"; shift 2 ;;
        --support-dir)
            [ -n "${2:-}" ] || error "--support-dir requires a value"
            SUPPORT_DIR="$(readlink -f "$2")"; shift 2 ;;
        --expected-release)
            [ -n "${2:-}" ] || error "--expected-release requires a value"
            EXPECTED_RELEASE="$2"; shift 2 ;;
        --from-shadowlink) FROM_SHADOWLINK=true; shift ;;
        --resume|--verify-resume)
            [ "$MODE" = reset ] || error "Specify only one resume mode"
            MODE="${1#--}"; shift ;;
        *) error "Usage: factory-reset.sh --release-dir DIR [--from-shadowlink | --resume | --verify-resume] [--support-dir DIR --expected-release PATH]" ;;
    esac
done
[ "${EUID:-$(id -u)}" -eq 0 ] || error "This script must run as root"
[ -n "$RELEASE_DIR" ] || error "--release-dir is required"
[ "$FROM_SHADOWLINK" = false ] || [ "$MODE" = reset ] || error "BLE cannot request boot recovery"
if [ -n "$SUPPORT_DIR" ]; then
    [ "$FROM_SHADOWLINK" = false ] && [ "$MODE" = reset ] && [ -n "$EXPECTED_RELEASE" ] \
        || error "Support reset requires explicit mode and --expected-release"
    for helper in shadowlink/runtime/shadowlink-server health-check.sh stack-install.sh; do
        [ -f "$SUPPORT_DIR/$helper" ] || error "Missing support helper: $helper"
    done
fi
RELEASE_DIR="$(readlink -f "$RELEASE_DIR")"
info "Context device_name=$(hostname) release=$RELEASE_DIR mode=$MODE from_shadowlink=$FROM_SHADOWLINK uid=${EUID:-unknown} pid=$$"
if [ -r /sys/class/net/wlan0/address ]; then
    mac="$(tr -d ':\n' < /sys/class/net/wlan0/address)"
    if [[ "$mac" =~ ^[[:xdigit:]]{12}$ ]]; then
        mac="${mac^^}"
        info "Device name derived from WLAN MAC: ClawStage-${mac: -4}"
    fi
fi
if [ -r "$RELEASE_DIR/VERSION" ]; then
    info "Release version=$(head -n 1 "$RELEASE_DIR/VERSION")"
fi
snapshot_state before
[ -d "$RELEASE_DIR" ] && [ "$(readlink -f "$STATE_DIR/current")" = "$RELEASE_DIR" ] \
    || error "Reset requires the active release"
READY="$RELEASE_DIR/.clawstage-release-ready"
NATIVE="$RELEASE_DIR/shadowlink/runtime/shadowlink-server"

# Explicit reset supersedes stale local transactions. Stop their writers before
# acquiring the shared host lock, rather than asking persisted OTA state for
# permission. A second reset waits here and cannot restart another reset's workers.
if [ "$MODE" = reset ]; then
    step quiesce-maintenance "Stop update/recovery/factory workers before taking over local state"
    reset_lock="${LOCK_PATH}.factory-reset"
    [ ! -L "$reset_lock" ] || error "Unsafe reset serialization lock"
    umask 077
    exec 8<>"$reset_lock"
    [ -f "$reset_lock" ] && [ -O "$reset_lock" ] \
        && [ "$(stat -Lc '%a' /proc/$$/fd/8)" = 600 ] \
        && [ "$(stat -Lc '%d:%i' /proc/$$/fd/8)" = "$(stat -Lc '%d:%i' "$reset_lock")" ] \
        || error "Unsafe reset serialization lock"
    flock -x 8
    systemctl is-active --quiet software-update.service && RESTART_UPDATE=true
    systemctl is-active --quiet clawstage-factory-console.service && RESTART_FACTORY=true
    # Stop factory first: its ExecStopPost can restore the OTA service.
    for unit in clawstage-factory-console.service clawstage-shadowlink-reset.service \
        software-update.service clawstage-boot-verify.service; do
        if ! systemctl stop "$unit"; then
            # Removed/old units and failing ExecStopPost are not state inhibitors.
            # A process that is actually still running must stop before data changes.
            activity="$(systemctl show "$unit" --property=ActiveState --value 2>/dev/null || true)"
            case "$activity" in active|activating|deactivating|reloading)
                error "Cannot stop maintenance worker: $unit ($activity)" ;;
                *) info "WARNING stop unit=$unit state=${activity:-not-loaded}; continuing" ;;
            esac
        fi
    done
fi

# Keep the same lock inode as OTA/provisioning. A leftover lock file is harmless;
# wait for an actual holder instead of rejecting the reset because it is busy.
step maintenance-lock "Wait for exclusive lock: $LOCK_PATH"
[ ! -L "$LOCK_PATH" ] || error "Unsafe maintenance lock"
umask 077
exec 9<>"$LOCK_PATH"
[ -f "$LOCK_PATH" ] && [ ! -L "$LOCK_PATH" ] && [ -O "$LOCK_PATH" ] \
    && [ "$(stat -Lc '%a' /proc/$$/fd/9)" = 600 ] \
    && [ "$(stat -Lc '%d:%i' /proc/$$/fd/9)" = "$(stat -Lc '%d:%i' "$LOCK_PATH")" ] \
    || error "Unsafe maintenance lock ownership or permissions"
flock -x 9
# A finishing writer may have switched current while we waited. Operate on the
# active installed release, not a path read before the lock was acquired.
RELEASE_DIR="$(readlink -f "$STATE_DIR/current")"
[ -d "$RELEASE_DIR" ] || error "Active release directory is missing"
[ -z "$EXPECTED_RELEASE" ] || [ "$RELEASE_DIR" = "$EXPECTED_RELEASE" ] \
    || error "Active release changed since support preflight; refusing to reset another release"
READY="$RELEASE_DIR/.clawstage-release-ready"
NATIVE="$RELEASE_DIR/shadowlink/runtime/shadowlink-server"
[ -z "$SUPPORT_DIR" ] || NATIVE="$SUPPORT_DIR/shadowlink/runtime/shadowlink-server"
info "Maintenance acquired; active_release=$RELEASE_DIR (persisted state is not an admission check)"

# Remove only named installer-owned records, never paths stored inside records.
# Do not follow a symlink in a parent directory or delete its external target.
remove_state_paths() {
    python3 - "$@" <<'PYREMOVE'
import os, pathlib, shutil, stat, sys
for name in sys.argv[1:]:
    path = pathlib.Path(name)
    if any(parent.is_symlink() for parent in path.parents):
        print(f'[FACTORY-RESET] WARNING skipped state under symlinked parent: {path}', flush=True)
        continue
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        continue
    if stat.S_ISDIR(mode):
        shutil.rmtree(path)
    else:
        path.unlink()
    print(f'[FACTORY-RESET] Removed obsolete state: {path}', flush=True)
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)
PYREMOVE
}

clear_superseded_state() {
    local name
    local paths=("$OTA_STATE" "$OTA_WANTS" "$STATE_DIR/factory-records/mode.json"
        "$FACTORY_RUNTIME/reset-inhibit" "$FACTORY_RUNTIME/reset-event.json"
        "$FACTORY_RUNTIME/boot-factory-flag.json"
        "$REAL_HOME/.hoorii/clawstage_base_installed.flag"
        "$REAL_HOME/.hoorii/stage_drivers_installed.flag")
    for name in .rollback_target .update_target .reboot_required .rollback_health_failed \
        .boot_config_backup .boot_config_restored .driver_rollback_required .driver_rollback_restored \
        .manual-debug-target .factory-image-sealing .factory-image-first-boot \
        .factory-image-needs-verification factory-test.flag; do
        paths+=("$STATE_DIR/$name")
    done
    remove_state_paths "${paths[@]}"
    systemctl daemon-reload || info "WARNING systemd reload failed after retiring boot recovery"
    # Baselines and identity-created are still live contracts, not owner state.
    info "Preserved device identity, driver/base installation baselines, firmware and boot assets"
}

write_phase() {
    local phase="$1" temporary="${RESET_MARKER}.tmp.$$"
    jq -n --arg release "$RELEASE_DIR" --arg phase "$phase" \
        '{version:1,release:$release,phase:$phase,original_ready:true}' > "$temporary"
    chmod 0600 "$temporary"
    sync -f "$temporary"
    mv -f "$temporary" "$RESET_MARKER"
    sync -f "$STATE_DIR"
    info "Recovery phase persisted: $phase marker=$RESET_MARKER"
}

cache_public_paa() {
    local source="$RELEASE_DIR/user/.hoorii/stage-stack/matter/data/credentials"
    local temporary certificate format count=0
    temporary="$(mktemp -d "$STATE_DIR/.factory-reset-paa.XXXXXX")"
    # Old .version/missing cache must never prevent owner cleanup. Use sanitized
    # public certificates from the current data, interrupted reset, or image bundle.
    for source in "$source" "$PAA_CACHE" "$RELEASE_DIR/homeassistant/factory-paa-root-certs"; do
        [ -d "$source" ] && [ ! -L "$source" ] || continue
        for certificate in "$source"/*.der "$source"/*.pem; do
            [ -f "$certificate" ] && [ ! -L "$certificate" ] || continue
            format=PEM
            [[ "$certificate" != *.der ]] || format=DER
            if openssl x509 -in "$certificate" -inform "$format" -outform "$format" \
                -out "$temporary/$(basename "$certificate")" 2>/dev/null \
                && openssl x509 -in "$temporary/$(basename "$certificate")" -inform "$format" -noout -text \
                    | grep -F 'CA:TRUE' >/dev/null; then
                count=$((count + 1))
            else
                rm -f "$temporary/$(basename "$certificate")"
                info "WARNING invalid public Matter certificate skipped: $(basename "$certificate")"
            fi
        done
        [ "$count" -eq 0 ] || break
    done
    if [ "$count" -gt 0 ]; then
        printf '1\n' > "$temporary/.version"
        chmod 0644 "$temporary/.version" "$temporary"/*
    else
        info "WARNING no valid public Matter certificates available; local cleanup will continue; runtime health may require certificate repair"
    fi
    remove_state_paths "$PAA_CACHE"
    mv "$temporary" "$PAA_CACHE"
    sync -f "$STATE_DIR"
    info "Public Matter certificates cached: count=$count cache=$PAA_CACHE"
}

authorize_reset() {
    "$NATIVE" --release-dir "$RELEASE_DIR" --authorize-reset --maintenance-lock-fd 9 \
        || error "Cannot prepare local reset transaction"
}

step recovery-state "Start explicit reset or resume this reset's durable phase"
if [ "$MODE" = reset ]; then
    # An explicit command always starts a new full reset, regardless of corrupt,
    # foreign-release, prepared, completed, or legacy state from an older version.
    step cache-certificates "Preserve available public Matter CA certificates"
    cache_public_paa
    step persist-reset-intent "Replace old reset records with this explicit reset intent"
    remove_state_paths "$RESET_MARKER" "$RELEASE_DIR/.device-unbind.json"
    write_phase clearing
    remove_state_paths "$READY"
    phase=clearing
    step clear-local-state "Retire superseded OTA, recovery and factory-test state"
    clear_superseded_state
else
    [ -f "$RESET_MARKER" ] && [ ! -L "$RESET_MARKER" ] && [ -O "$RESET_MARKER" ] \
        || error "Unsafe reset recovery marker"
    jq -e -s --arg release "$RELEASE_DIR" \
        'length == 1 and (.[0] | .version == 1 and .release == $release and .original_ready == true and
         (.phase == "clearing" or .phase == "prepared"))' "$RESET_MARKER" >/dev/null \
        || error "Invalid reset recovery marker; refusing to change user data"
    phase="$(jq -r .phase "$RESET_MARKER")"
    info "Resuming reset phase=$phase mode=$MODE"
fi
step authorize-reset "Prepare local transaction and attempt cloud unbind once (best effort)"
authorize_reset
rm -f "$READY"
sync -f "$RELEASE_DIR"
printf 'CLAWSTAGE_RESET_ACCEPTED\n'

if [ "$MODE" = verify-resume ]; then
    step verify-prepared "Check that runtime preparation completed before boot verification"
    [ "$phase" = prepared ] || error "Reset runtime preparation is incomplete"
else
    if [ "$FROM_SHADOWLINK" = false ]; then
        step stop-ble "Stop Shadowlink before changing binding configuration"
        systemctl stop shadowlink-ble-server.service \
            || error "Cannot stop Shadowlink before resetting its configuration"
    fi
    if [ "$phase" = clearing ]; then
        step clear-config "Clear commissioning and userinfo through the native reset entry"
        "$NATIVE" --release-dir "$RELEASE_DIR" --reset-config-only --maintenance-lock-fd 9 \
            || error "Cannot clear the account while preserving device identity"
        snapshot_state after-config
        step clear-openclaw "Run the component account cleanup entry"
        runuser -u "$REAL_USER" -- "$RELEASE_DIR/openclaw/hoorii_claw/script/clear.sh" \
            || error "OpenClaw account cleanup failed"
        # Remove the legacy installer guard journal from older releases.
        # The guard is no longer shipped; reset still clears its saved credentials.
        remove_state_paths "$RELEASE_DIR/user/.clawstage-openclaw-user-settings.json"
        sync -f "$RELEASE_DIR/user"
        step clear-homeassistant "Purge HomeAssistant, Matter and OTBR account data using the existing installer"
        stack="$RELEASE_DIR/homeassistant/stage-stack"
        stack_installer="$stack/install.sh"
        [ -z "$SUPPORT_DIR" ] || stack_installer="$SUPPORT_DIR/stack-install.sh"
        env --chdir "$stack" HOME="$REAL_HOME" SUDO_USER="$REAL_USER" \
            "$stack_installer" --purge all -f --no-pull --no-start --system-dir "$stack" \
            --data-dir "$RELEASE_DIR/user/.hoorii/stage-stack" \
            || error "HomeAssistant account cleanup failed"
        step restore-certificates "Restore public Matter certificates for offline setup"
        credentials="$RELEASE_DIR/user/.hoorii/stage-stack/matter/data/credentials"
        rm -rf "$credentials"
        mkdir -p "$credentials"
        if [ -d "$PAA_CACHE" ] && [ ! -L "$PAA_CACHE" ]; then
            cp -a "$PAA_CACHE/." "$credentials/"
        else
            info "WARNING public certificate cache unavailable; account cleanup completed"
        fi
        chmod 0755 "$credentials"
        sync -f "$credentials"
        step start-homeassistant "Start the cleared stack using local container images"
        docker compose -f "$stack/compose.release.yaml" up -d --no-build --pull never \
            || error "HomeAssistant stack did not restart after reset"
        step persist-prepared "Persist completed cleanup so retries do not clear prepared data again"
        write_phase prepared
        phase=prepared
    else
        info "Cleanup skipped: phase=prepared; verifying existing prepared data"
    fi
    # ExecStartPre must return before the launcher starts its other services.
    # ExecStartPost performs the same verification/commit as the live reset.
    if [ "$MODE" = resume ]; then
        info "Reset runtime prepared; startup will verify it"
        exit 0
    fi
    if [ "$FROM_SHADOWLINK" = false ]; then
        step restart-ble "Restart Shadowlink with cleared binding state"
        systemctl restart shadowlink-ble-server.service || error "Shadowlink did not restart after reset"
    fi
fi

step health-check "Verify the reset runtime (OpenClaw awaits provisioning)"
bash "${SUPPORT_DIR:-$RELEASE_DIR}/health-check.sh" --release-dir "$RELEASE_DIR" --reset-runtime \
    || error "Reset software health check failed; recovery will retry at startup"
step notify-reset "Send RESET event via WebSocket"
"$NATIVE" --release-dir "$RELEASE_DIR" --notify-factory-reset --maintenance-lock-fd 9 \
    || error "RESET notification failed; recovery will retry without clearing prepared data"
info "RESET event sent via WebSocket (delivery is not a UI completion acknowledgement)"
step commit-reset "Restore release readiness and remove completed recovery records"
ready_tmp="${READY}.tmp.$$"
printf 'verified_by=factory-reset\nverified_at=%s\n' "$(date -Is)" > "$ready_tmp"
chmod 0600 "$ready_tmp"
sync -f "$ready_tmp"
mv -f "$ready_tmp" "$READY"
sync -f "$RELEASE_DIR"
rm -f "$RESET_MARKER" "$RELEASE_DIR/.device-unbind.json"
sync -f "$STATE_DIR"
rm -rf "$PAA_CACHE"
phase=complete
info "Account state cleared and HomeAssistant verified; OpenClaw awaits provisioning"
