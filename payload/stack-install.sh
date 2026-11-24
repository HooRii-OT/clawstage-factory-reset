#!/usr/bin/env bash
set -euo pipefail

# ==========================================================
# HomeAI Stage Online Installer
# ==========================================================
# This script:
#  - installs Docker (and Compose plugin) if missing (Ubuntu 22.04, Raspberry Pi OS)
#  - removes retired Stage Integration from HA configuration
#  - runs docker compose with compose.release.yaml
#
# Flags:
#  -f            force overwrite if already installed
#  -u            uninstall (does not remove Docker)
#  --no-pull     do not pull images (error if missing locally)
#  --pull        pull images before up (default)
#  --no-start    deploy files/data only; do not start compose services
#  --pull-retries N
#                retry docker compose pull N times (default: 3)
#  --dry-run     print actions only
#
# ==========================================================

# ---- Template variables ----
BUNDLE_NAME="stage-stack-bundle-online"             # e.g. stage-online-bundle
BUNDLE_VERSION="2026.1.3-cc1.1.1"       # e.g. 2026.1.2-cc1.0.0
GIT_SHA="abf16758d9836399b47f32b9558696fc87b60bf2"

# File names within bundle
COMPOSE_FILE="compose.release.yaml"           # e.g. compose.release.yaml
LOCK_FILE="images.lock"                 # e.g. images.lock (optional)
INSTALL_MARK=".installed.json"

# Installation directories
# SYSTEM_DIR: system files (scripts, compose config), e.g. /opt/stage-stack
# DATA_DIR: user data (ha/config, matter/data, otbr/data), e.g. ~/.hoorii/stage-stack
SYSTEM_DIR="/opt/stage-stack"               # e.g. /opt/stage-stack
DATA_DIR="~/.hoorii/stage-stack"                   # e.g. ~/.hoorii/stage-stack

# Retired integration target (relative to DATA_DIR)
INTEGRATION_TARGET_DIR="ha/config/custom_components/stage_integration"

# Docker install behavior
DOCKER_INSTALL_MODE="ubuntu-apt"          # reserved

# Defaults
FORCE=0
UNINSTALL=0
PURGE_SCOPE=""
PULL=1
PULL_RETRIES="${STAGE_STACK_PULL_RETRIES:-3}"
PULL_RETRY_DELAY="${STAGE_STACK_PULL_RETRY_DELAY:-10}"
START_SERVICES=1
DRY_RUN=0
SYSTEM_DIR_SET=0
DATA_DIR_SET=0

# Initialize from template defaults (will be overridden by args)
# These are already set from template variables above

print_usage() {
  cat <<EOF
Usage:
  ./install.sh [options]

Options:
  -f, --force            Force overwrite if already installed
  -u, --uninstall        Uninstall (does not remove Docker)
  --purge SCOPE          Purge data: all|ha|matter|otbr (requires confirmation or --force)
  --system-dir PATH      System files directory (default: release/homeassistant/stage-stack when run from a release; otherwise /opt/stage-stack)
  --data-dir PATH        User data directory (default: release/user/.hoorii/stage-stack when run from a release; otherwise ~/.hoorii/stage-stack)
  --pull                 Pull images before starting (default)
  --no-pull              Do not pull images
  --no-start             Deploy files/data only; do not start compose services
  --pull-retries N       Retry docker compose pull N times (default: ${PULL_RETRIES})
  --pull-retry-delay N   Seconds between pull retries (default: ${PULL_RETRY_DELAY})
  --dry-run              Print actions only
  -h, --help             Show help

Bundle:
  Name    : ${BUNDLE_NAME}
  Version : ${BUNDLE_VERSION}
  Commit  : ${GIT_SHA}

EOF
}

log() { echo "[install] $*"; }
warn() { echo "[install][WARN] $*" >&2; }
die() { echo "[install][ERROR] $*" >&2; exit 1; }

run() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[dry-run] $*"
  else
    eval "$@"
  fi
}

need_root_or_sudo() {
  if [[ "$EUID" -eq 0 ]]; then
    echo ""
    return 0
  fi
  if command -v sudo >/dev/null 2>&1; then
    echo "sudo"
    return 0
  fi
  die "Need root privileges (run as root or install sudo)."
}

detect_ubuntu_2204() {
  if [[ -f /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    if [[ "${ID:-}" == "ubuntu" && "${VERSION_ID:-}" == "22.04" ]]; then
      return 0
    fi
  fi
  return 1
}

detect_raspbian() {
  if [[ -f /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    if [[ "${ID:-}" == "raspbian" ]] || [[ "${ID_LIKE:-}" == *"debian"* && "${PRETTY_NAME:-}" == *"Raspberry Pi"* ]]; then
      return 0
    fi
    # Alternative check for Raspberry Pi OS (newer versions)
    if [[ "${ID:-}" == "debian" && -f /proc/device-tree/model && "$(cat /proc/device-tree/model 2>/dev/null)" == *"Raspberry Pi"* ]]; then
      return 0
    fi
  fi
  return 1
}

has_docker() {
  command -v docker >/dev/null 2>&1
}

has_compose() {
  if docker compose version >/dev/null 2>&1; then
    return 0
  fi
  if command -v docker-compose >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

compose_cmd() {
  if docker compose version >/dev/null 2>&1; then
    echo "docker compose"
  else
    echo "docker-compose"
  fi
}

render_compose_data_dir() {
  local compose="$SYSTEM_ROOT/$COMPOSE_FILE"
  [[ -f "$compose" ]] || die "Compose file not found for rendering: $compose"

  local SUDO=""
  if [[ ! -w "$compose" ]]; then
    SUDO="$(need_root_or_sudo)"
  fi

  local escaped_data_root
  escaped_data_root="$(printf '%s' "$DATA_ROOT" | sed 's/[\/&]/\\&/g')"

  log "Rendering compose data directory: $DATA_ROOT"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[dry-run] render /home/pi/.hoorii/stage-stack -> $DATA_ROOT in $compose"
  else
    $SUDO sed -i "s|/home/pi/.hoorii/stage-stack|$escaped_data_root|g" "$compose"
  fi
}

install_docker_ubuntu_apt() {
  local SUDO
  SUDO="$(need_root_or_sudo)"

  if ! detect_ubuntu_2204; then
    die "This installer currently supports Ubuntu 22.04 only for auto Docker installation."
  fi

  log "Installing Docker (Ubuntu apt)..."
  # Official Docker Engine install steps (apt repository)
  run "$SUDO apt-get update"
  run "$SUDO apt-get install -y ca-certificates curl gnupg lsb-release"

  # Keyring
  run "$SUDO install -m 0755 -d /etc/apt/keyrings"
  run "$SUDO rm -f /etc/apt/keyrings/docker.gpg"
  run "$SUDO bash -c 'curl -fsSL https://download.docker.com/linux/ubuntu/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg'"
  run "$SUDO chmod a+r /etc/apt/keyrings/docker.gpg"

  # Repo
  run "$SUDO bash -c 'echo \"deb [arch=\$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu \$(. /etc/os-release && echo \$VERSION_CODENAME) stable\" > /etc/apt/sources.list.d/docker.list'"

  run "$SUDO apt-get update"
  run "$SUDO apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin"

  # Enable + start
  run "$SUDO systemctl enable --now docker"

  log "Docker installed."
}

install_docker_raspbian_apt() {
  local SUDO
  SUDO="$(need_root_or_sudo)"

  if ! detect_raspbian; then
    die "This function should only be called on Raspberry Pi OS."
  fi

  log "Installing Docker (Raspberry Pi OS apt)..."
  # Official Docker Engine install steps for Raspberry Pi (apt repository)
  run "$SUDO apt-get update"
  run "$SUDO apt-get install -y ca-certificates curl gnupg lsb-release"

  # Keyring
  run "$SUDO install -m 0755 -d /etc/apt/keyrings"
  run "$SUDO rm -f /etc/apt/keyrings/docker.gpg"
  run "$SUDO bash -c 'curl -fsSL https://download.docker.com/linux/debian/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg'"
  run "$SUDO chmod a+r /etc/apt/keyrings/docker.gpg"

  # Repo (using Debian repository for ARM support)
  run "$SUDO bash -c 'echo \"deb [arch=\$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/debian \$(. /etc/os-release && echo \$VERSION_CODENAME) stable\" > /etc/apt/sources.list.d/docker.list'"

  run "$SUDO apt-get update"
  run "$SUDO apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin"

  # Enable + start
  run "$SUDO systemctl enable --now docker"

  log "Docker installed."
}

ensure_docker_ready() {
  if ! has_docker; then
    log "Docker not found. Will install Docker."
    if detect_ubuntu_2204; then
      install_docker_ubuntu_apt
    elif detect_raspbian; then
      install_docker_raspbian_apt
    else
      die "Unsupported OS for auto Docker installation. Supported: Ubuntu 22.04, Raspberry Pi OS. Please install Docker manually."
    fi
  fi

  if ! has_compose; then
    die "Docker Compose not found after Docker install. Please check your Docker installation."
  fi

  # Ensure daemon reachable
  if ! docker info >/dev/null 2>&1; then
    warn "docker info failed. You may need to run as root, or add your user to the docker group:"
    warn "  sudo usermod -aG docker \$USER && newgrp docker"
    die "Docker daemon not reachable."
  fi
}

bundle_root() {
  # Return the system installation directory (where scripts and config are)
  echo "$SYSTEM_ROOT"
}

data_root() {
  # Return the data directory (where user data is stored)
  echo "$DATA_ROOT"
}

script_root() {
  # Return the script/bundle directory (pre-saved in main)
  echo "$SCRIPT_DIR"
}

installed_marker_path() {
  echo "$(data_root)/${INSTALL_MARK}"
}

is_installed() {
  [[ -f "$(installed_marker_path)" ]]
}

write_installed_marker() {
  local root
  root="$(data_root)"
  local ts
  ts="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

  # Optional: include lock content if present
  local lock_content=""
  local sys_root
  sys_root="$(bundle_root)"
  if [[ -f "$sys_root/$LOCK_FILE" ]]; then
    lock_content="$(sed 's/"/\\"/g' "$sys_root/$LOCK_FILE" | awk '{print "    \""$0"\","}')"
  fi

  if [[ "$DRY_RUN" -eq 1 ]]; then
    log "Would write install marker: $root/$INSTALL_MARK"
    return 0
  fi

  cat > "$root/$INSTALL_MARK" <<EOF
{
  "bundle_name": "${BUNDLE_NAME}",
  "bundle_version": "${BUNDLE_VERSION}",
  "git_sha": "${GIT_SHA}",
  "installed_at_utc": "${ts}",
  "compose_file": "${COMPOSE_FILE}",
  "system_dir": "${SYSTEM_ROOT}",
  "data_dir": "${DATA_ROOT}"
}
EOF
}

remove_installed_marker() {
  local root
  root="$(data_root)"
  run "rm -f \"$root/$INSTALL_MARK\""
}

reserve_bluetooth_for_shadowlink() {
  local address SUDO=""
  address="$(busctl get-property org.bluez /org/bluez/hci0 org.bluez.Adapter1 Address 2>/dev/null | awk -F '"' '{print $2}')" || return 0
  [[ "$address" =~ ^([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}$ ]] || return 0
  [[ "$DRY_RUN" -eq 0 ]] || { log "Would disable HA discovery of Shadowlink adapter $address"; return 0; }
  # HA owns this storage while running. Stop it before preserving/updating entries.
  if [[ "${1:-}" != "already-stopped" ]]; then
    compose_stop_service "homeassistant"
  fi
  [[ -w "$(data_root)/ha/config" ]] || SUDO="$(need_root_or_sudo)"
  $SUDO python3 - "$(data_root)/ha/config/.storage/core.config_entries" "$address" <<'PYBLUETOOTH'
import datetime
import json
import os
from pathlib import Path
import sys
import tempfile
import uuid
path = Path(sys.argv[1])
address = sys.argv[2].upper()
# Schema matches the pinned HA image's config_entries storage (1.5).
state = json.loads(path.read_text()) if path.exists() else {
    "version": 1, "minor_version": 5, "key": "core.config_entries", "data": {"entries": []}}
entries = state["data"]["entries"]
now = datetime.datetime.now(datetime.timezone.utc).isoformat()
matched = False
for entry in entries:
    if entry["domain"] == "bluetooth" and str(entry.get("unique_id", "")).upper() == address:
        entry["disabled_by"] = "user"
        entry["modified_at"] = now
        matched = True
if not matched:
    entries.append({"domain": "bluetooth", "unique_id": address,
        "entry_id": uuid.uuid4().hex, "version": 1, "minor_version": 1,
        "source": "integration_discovery", "title": "Reserved for Shadowlink (" + address + ")",
        "data": {}, "options": {}, "disabled_by": "user", "discovery_keys": {},
        "subentries": [], "pref_disable_new_entities": False, "pref_disable_polling": False,
        "created_at": now, "modified_at": now})
path.parent.mkdir(parents=True, exist_ok=True)
metadata = path.stat() if path.exists() else None
fd, temporary = tempfile.mkstemp(dir=path.parent, prefix=".clawstage-bluetooth-")
try:
    with os.fdopen(fd, "w") as stream:
        json.dump(state, stream, ensure_ascii=False)
        stream.flush()
        os.fsync(stream.fileno())
    if metadata:
        os.chmod(temporary, metadata.st_mode & 0o777)
        os.chown(temporary, metadata.st_uid, metadata.st_gid)
    os.replace(temporary, path)
finally:
    Path(temporary).unlink(missing_ok=True)
PYBLUETOOTH
}

remove_stage_integration() {
  # The legacy directory is also the pending-migration marker. Fresh installs
  # and later upgrades must not inspect HA storage for an already retired plugin.
  local target="$(data_root)/$INTEGRATION_TARGET_DIR"
  [[ -e "$target" || -L "$target" ]] || return 0
  # Stop HA even without a Bluetooth adapter: its registries must be offline.
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log "Would stop HA and remove retired Stage Integration from $(data_root)/ha/config"
    return 0
  fi
  compose_stop_service "homeassistant"
  local SUDO=""
  [[ -w "$(data_root)/ha/config" ]] || SUDO="$(need_root_or_sudo)"
  $SUDO python3 "$(bundle_root)/remove-stage-integration.py" "$(data_root)/ha/config" \
    || die "Failed to remove retired Stage Integration"
}

locked_images() {
  local root
  root="$(bundle_root)"
  local lock="$root/$LOCK_FILE"

  [[ -f "$lock" ]] || return 0
  sed -n 's/^[A-Za-z0-9_]*_DIGEST=//p' "$lock"
}

locked_images_ready() {
  local image
  local count=0

  while IFS= read -r image; do
    [[ -n "$image" ]] || continue
    count=$((count + 1))
    if ! docker image inspect "$image" >/dev/null 2>&1; then
      return 1
    fi
  done < <(locked_images)

  [[ "$count" -gt 0 ]]
}

compose_pull_with_retry() {
  local compose="$1"
  local C="$2"
  local attempt=1
  local retries="$PULL_RETRIES"

  [[ "$retries" =~ ^[0-9]+$ ]] || retries=3
  [[ "$retries" -ge 1 ]] || retries=1

  if locked_images_ready; then
    log "All locked Docker images already exist locally; skip pull."
    return 0
  fi

  while [[ "$attempt" -le "$retries" ]]; do
    log "Pulling Docker images (attempt $attempt/$retries)..."
    if run "$C -f \"$compose\" pull" 2>&1; then
      if locked_images_ready; then
        log "Locked Docker images are ready."
      else
        warn "Compose pull finished, but locked image inspection did not fully pass."
      fi
      return 0
    fi

    if [[ "$attempt" -lt "$retries" ]]; then
      warn "Docker image pull failed; retrying in ${PULL_RETRY_DELAY}s..."
      sleep "$PULL_RETRY_DELAY"
    fi
    attempt=$((attempt + 1))
  done

  die "Docker image pull failed after $retries attempt(s)."
}

compose_pull_only() {
  local root
  root="$(bundle_root)"

  local compose="$root/$COMPOSE_FILE"
  [[ -f "$compose" ]] || die "Compose file not found: $compose"

  local C
  C="$(compose_cmd)"

  if [[ "$PULL" -eq 1 ]]; then
    compose_pull_with_retry "$compose" "$C"
  else
    log "Skip pulling images (--no-pull)."
    locked_images_ready || die "Required locked Docker images are missing locally."
  fi
}

compose_pull_up() {
  local root
  root="$(bundle_root)"
  
  local compose="$root/$COMPOSE_FILE"
  [[ -f "$compose" ]] || die "Compose file not found: $compose"

  local C
  C="$(compose_cmd)"

  if [[ "$PULL" -eq 1 ]]; then
    compose_pull_with_retry "$compose" "$C"
  else
    log "Skip pulling images (--no-pull)."
    locked_images_ready || die "Required locked Docker images are missing locally."
  fi

  if [[ "$START_SERVICES" -eq 0 ]]; then
    log "Services not started (--no-start)."
    return 0
  fi

  log "Starting services (system_dir=$SYSTEM_ROOT, data_dir=$DATA_ROOT)..."
  run "$C -f \"$compose\" up -d --no-build --pull never"
  if [[ "$DRY_RUN" -eq 0 ]]; then
    log "Services started."
    log "HA: http://<device-ip>:8123"
  else
    log "Services not started (--dry-run)."
  fi
}

compose_down() {
  local root
  root="$(bundle_root)"
  
  local compose="$root/$COMPOSE_FILE"
  [[ -f "$compose" ]] || die "Compose file not found: $compose"

  local C
  C="$(compose_cmd)"

  log "Stopping services..."
  run "$C -f \"$compose\" down"
}

compose_stop_service() {
  local service="$1"
  local root
  root="$(bundle_root)"
  
  local compose="$root/$COMPOSE_FILE"
  [[ -f "$compose" ]] || die "Compose file not found: $compose"

  local C
  C="$(compose_cmd)"

  log "Stopping service: $service"
  run "$C -f \"$compose\" stop \"$service\"" || die "Cannot stop $service; refusing to purge its data."
}

purge_step() {
  local step="$1" started=$SECONDS
  shift
  log "Purge step=$step started."
  "$@"
  log "Purge step=$step elapsed=$((SECONDS - started))s."
}

uninstall_all() {
  local root
  root="$(data_root)"
  ensure_docker_ready

  compose_down

  # Remove integration directory (keep ha/config as a whole)
  local target="$root/$INTEGRATION_TARGET_DIR"
  if [[ -d "$target" ]]; then
    log "Removing integration dir: $target"
    
    # Check if we need sudo
    local SUDO=""
    if [[ ! -w "$(dirname "$target")" ]]; then
      SUDO="$(need_root_or_sudo)"
    fi
    
    run "$SUDO rm -rf \"$target\""
  fi

  remove_installed_marker
  log "Uninstalled (Docker kept)."
}

purge_data() {
  local scope="$1"
  local root
  root="$DATA_ROOT"
  
  local SUDO=""
  if [[ ! -w "$root" ]]; then
    SUDO="$(need_root_or_sudo)"
  fi

  case "$scope" in
    all)
      log "Purging all data (ha, matter, otbr)..."
      
      # Confirm deletion
      if [[ "$FORCE" -ne 1 ]]; then
        read -p "This will delete all data (ha/config, matter/data, otbr/data). Continue? (y/N): " confirm
        if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
          log "Purge cancelled."
          return 0
        fi
      fi
      
      local started=$SECONDS
      # `down` stops independent services concurrently and removes their writable
      # layers. Finish successfully before deleting any shared persistent state.
      purge_step stop-remove compose_down
      purge_step ha-data purge_ha_data "$root" "$SUDO"
      purge_step matter-data purge_matter_data "$root" "$SUDO"
      purge_step otbr-data purge_otbr_data "$root" "$SUDO"
      log "All data purged."
      # Always recreate containers: stopping/starting alone could retain user
      # state outside the bind mounts in the old containers' writable layers.
      purge_step start compose_pull_up
      log "Purge scope=all elapsed=$((SECONDS - started))s."
      ;;
    ha)
      log "Purging HA data..."
      if [[ "$FORCE" -ne 1 ]]; then
        read -p "This will delete ha/config. Continue? (y/N): " confirm
        if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
          log "Purge cancelled."
          return 0
        fi
      fi
      compose_stop_service "homeassistant"
      purge_ha_data "$root" "$SUDO"
      log "HA data purged."
      log "Stopping all services..."
      compose_down
      log "Restarting services..."
      compose_pull_up
      ;;
    matter)
      log "Purging Matter Server data..."
      if [[ "$FORCE" -ne 1 ]]; then
        read -p "This will delete matter/data. Continue? (y/N): " confirm
        if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
          log "Purge cancelled."
          return 0
        fi
      fi
      compose_stop_service "matter-server"
      purge_matter_data "$root" "$SUDO"
      log "Matter Server data purged."
      log "Stopping all services..."
      compose_down
      log "Restarting services..."
      compose_pull_up
      ;;
    otbr)
      log "Purging OTBR data..."
      if [[ "$FORCE" -ne 1 ]]; then
        read -p "This will delete otbr/data. Continue? (y/N): " confirm
        if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
          log "Purge cancelled."
          return 0
        fi
      fi
      compose_stop_service "otbr"
      purge_otbr_data "$root" "$SUDO"
      log "OTBR data purged."
      log "Stopping all services..."
      compose_down
      log "Restarting services..."
      compose_pull_up
      ;;
    *)
      die "Unknown purge scope: $scope. Supported: all, ha, matter, otbr"
      ;;
  esac
}

purge_ha_data() {
  local root="$1"
  local SUDO="$2"
  local ha_dir="$root/ha/config"
  
  if [[ ! -d "$ha_dir" ]]; then
    warn "HA config directory not found: $ha_dir"
    return 0
  fi
  
  run "$SUDO rm -rf \"$ha_dir\""
  run "$SUDO mkdir -p \"$ha_dir\""
  log "HA config directory reset."
  
  reserve_bluetooth_for_shadowlink already-stopped
}

purge_matter_data() {
  local root="$1"
  local SUDO="$2"
  local matter_dir="$root/matter/data"
  
  if [[ ! -d "$matter_dir" ]]; then
    warn "Matter data directory not found: $matter_dir"
    return 0
  fi
  
  # Keep public PAA roots in place: a /tmp backup can disappear on a power loss
  # after data removal, leaving an empty cache considered fresh for 24 hours.
  run "$SUDO find \"$matter_dir\" -mindepth 1 -maxdepth 1 ! -name credentials -exec rm -rf -- {} +"

  log "Matter data directory reset (credentials preserved)."
}

purge_otbr_data() {
  local root="$1"
  local SUDO="$2"
  local otbr_dir="$root/otbr/data"
  
  if [[ ! -d "$otbr_dir" ]]; then
    warn "OTBR data directory not found: $otbr_dir"
    return 0
  fi
  
  run "$SUDO rm -rf \"$otbr_dir\""
  run "$SUDO mkdir -p \"$otbr_dir\""
  log "OTBR data directory reset."
}

# -------------------------
# Parse args
# -------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    -f|--force) FORCE=1; shift 1 ;;
    -u|--uninstall) UNINSTALL=1; shift 1 ;;
    --purge) PURGE_SCOPE="${2:-}"; shift 2 ;;
    --system-dir) SYSTEM_DIR="${2:-}"; SYSTEM_DIR_SET=1; shift 2 ;;
    --data-dir) DATA_DIR="${2:-}"; DATA_DIR_SET=1; shift 2 ;;
    --pull) PULL=1; shift 1 ;;
    --no-pull) PULL=0; shift 1 ;;
    --no-start) START_SERVICES=0; shift 1 ;;
    --pull-retries) PULL_RETRIES="${2:-}"; shift 2 ;;
    --pull-retry-delay) PULL_RETRY_DELAY="${2:-}"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift 1 ;;
    -h|--help) print_usage; exit 0 ;;
    *)
      die "Unknown argument: $1 (use --help)"
      ;;
  esac
done

# -------------------------
# Main
# -------------------------
# Save script directory before changing to installation directory
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

infer_release_root_from_script_dir() {
  case "$SCRIPT_DIR" in
    */homeassistant/stage-stack|*/homeassistant/stage-stack-bundle-online)
      (cd "$SCRIPT_DIR/../.." && pwd)
      ;;
    *)
      return 1
      ;;
  esac
}

# Determine the actual user (handles both direct execution and sudo)
# This is important for setting correct ownership on created directories
ACTUAL_USER="${SUDO_USER:-$(whoami)}"
ACTUAL_UID="$(id -u "$ACTUAL_USER" 2>/dev/null || echo "")"
ACTUAL_GID="$(id -g "$ACTUAL_USER" 2>/dev/null || echo "")"
ACTUAL_HOME="$(eval echo ~"$ACTUAL_USER")"

log "Running as: $(whoami), actual owner will be: $ACTUAL_USER (UID:$ACTUAL_UID, HOME:$ACTUAL_HOME)"

if RELEASE_ROOT="$(infer_release_root_from_script_dir 2>/dev/null)"; then
  if [[ "$SYSTEM_DIR_SET" -eq 0 ]]; then
    SYSTEM_DIR="$RELEASE_ROOT/homeassistant/stage-stack"
  fi
  if [[ "$DATA_DIR_SET" -eq 0 ]]; then
    DATA_DIR="$RELEASE_ROOT/user/.hoorii/stage-stack"
  fi
fi

# Expand home directory in SYSTEM_DIR and DATA_DIR
# Important: expand ~ using actual user's home, not root's
SYSTEM_DIR="${SYSTEM_DIR//\~/$ACTUAL_HOME}"
DATA_DIR="${DATA_DIR//\~/$ACTUAL_HOME}"
SYSTEM_DIR="$(eval echo "$SYSTEM_DIR")"
DATA_DIR="$(eval echo "$DATA_DIR")"

SYSTEM_ROOT="$SYSTEM_DIR"
DATA_ROOT="$DATA_DIR"

# Handle purge and uninstall before checking installation status
if [[ -n "$PURGE_SCOPE" ]]; then
  # Ensure directory exists for purge operations
  if [[ ! -d "$DATA_ROOT" ]]; then
    die "Data directory not found: $DATA_ROOT. Nothing to purge."
  fi
  cd "$DATA_ROOT"
  purge_data "$PURGE_SCOPE"
  exit 0
fi

if [[ "$UNINSTALL" -eq 1 ]]; then
  # Ensure directory exists for uninstall
  if [[ ! -d "$DATA_ROOT" ]]; then
    die "Data directory not found: $DATA_ROOT. Nothing to uninstall."
  fi
  cd "$DATA_ROOT"
  uninstall_all
  exit 0
fi

# Check if already installed
if is_installed && [[ "$FORCE" -ne 1 ]]; then
  die "Already installed (system: $SYSTEM_ROOT, data: $DATA_ROOT). Use -f to force overwrite, or -u to uninstall."
fi

# Create directories if they don't exist
if [[ ! -d "$SYSTEM_ROOT" ]]; then
  SUDO=""
  if [[ ! -w "$(dirname "$SYSTEM_ROOT")" ]]; then
    SUDO="$(need_root_or_sudo)"
  fi
  $SUDO mkdir -p "$SYSTEM_ROOT"
  log "Created system directory: $SYSTEM_ROOT"
fi

if [[ ! -d "$DATA_ROOT" ]]; then
  SUDO_DATA=""
  if [[ ! -w "$(dirname "$DATA_ROOT")" ]]; then
    SUDO_DATA="$(need_root_or_sudo)"
  fi
  $SUDO_DATA mkdir -p "$DATA_ROOT"
  log "Created data directory: $DATA_ROOT"
  
  # Fix ownership: ensure data directory belongs to the actual user (not root)
  if [[ -n "$SUDO_DATA" && -n "$ACTUAL_UID" && -n "$ACTUAL_GID" ]]; then
    log "Setting ownership of $DATA_ROOT to $ACTUAL_USER ($ACTUAL_UID:$ACTUAL_GID)..."
    $SUDO_DATA chown "$ACTUAL_UID:$ACTUAL_GID" "$DATA_ROOT"
  fi
fi

# Determine if we need sudo for copying files to system directory
SUDO=""
if [[ ! -w "$SYSTEM_ROOT" ]]; then
  SUDO="$(need_root_or_sudo)"
fi

# Copy bundle files from script directory to system directory
log "Copying bundle files to $SYSTEM_ROOT..."
$SUDO cp "$SCRIPT_DIR/$COMPOSE_FILE" "$SYSTEM_ROOT/" 2>/dev/null || warn "Could not copy $COMPOSE_FILE"
$SUDO cp "$SCRIPT_DIR/$(basename "$0")" "$SYSTEM_ROOT/" 2>/dev/null || warn "Could not copy install.sh"
[[ -f "$SCRIPT_DIR/$LOCK_FILE" ]] && $SUDO cp "$SCRIPT_DIR/$LOCK_FILE" "$SYSTEM_ROOT/" 2>/dev/null || true
[[ -f "$SCRIPT_DIR/matter-startup.py" ]] || die "Matter startup wrapper is missing"
if [[ ! "$SCRIPT_DIR/matter-startup.py" -ef "$SYSTEM_ROOT/matter-startup.py" ]]; then
  $SUDO install -m 0644 "$SCRIPT_DIR/matter-startup.py" "$SYSTEM_ROOT/matter-startup.py" \
    || die "Could not install Matter startup wrapper"
fi
[[ -f "$SCRIPT_DIR/remove-stage-integration.py" ]] || die "Stage Integration removal helper is missing"
for helper in ha-config-flow-retry.py ha-retry-init.sh; do
  [[ -f "$SCRIPT_DIR/$helper" ]] || die "HA config flow retry helper is missing: $helper"
  if [[ ! "$SCRIPT_DIR/$helper" -ef "$SYSTEM_ROOT/$helper" ]]; then
    $SUDO install -m 0644 "$SCRIPT_DIR/$helper" "$SYSTEM_ROOT/$helper" \
      || die "Could not install HA config flow retry helper: $helper"
  fi
done
$SUDO chmod 0755 "$SYSTEM_ROOT/ha-retry-init.sh"
if [[ ! "$SCRIPT_DIR/remove-stage-integration.py" -ef "$SYSTEM_ROOT/remove-stage-integration.py" ]]; then
  $SUDO install -m 0644 "$SCRIPT_DIR/remove-stage-integration.py" "$SYSTEM_ROOT/remove-stage-integration.py" \
    || die "Could not install Stage Integration removal helper"
fi
render_compose_data_dir

# Create data directory structure (ha, matter, otbr)
SUDO_DATA=""
if [[ ! -w "$DATA_ROOT" ]]; then
  SUDO_DATA="$(need_root_or_sudo)"
fi
log "Creating data directory structure at $DATA_ROOT..."
$SUDO_DATA mkdir -p "$DATA_ROOT/ha/config"
$SUDO_DATA mkdir -p "$DATA_ROOT/matter/data"
$SUDO_DATA mkdir -p "$DATA_ROOT/otbr/data"

# Fix ownership: ensure data directory belongs to the actual user (not root)
if [[ -n "$SUDO_DATA" && -n "$ACTUAL_UID" && -n "$ACTUAL_GID" ]]; then
  log "Setting ownership of $DATA_ROOT to $ACTUAL_USER ($ACTUAL_UID:$ACTUAL_GID)..."
  $SUDO_DATA chown -R "$ACTUAL_UID:$ACTUAL_GID" "$DATA_ROOT"
fi

# Change to data directory for the rest of the installation
cd "$DATA_ROOT"

ensure_docker_ready

# Retire the legacy integration only in snapshots that still contain it.
remove_stage_integration
reserve_bluetooth_for_shadowlink
if [[ "$START_SERVICES" -eq 1 ]]; then
  compose_pull_up
else
  compose_pull_only
  log "Services not started (--no-start)."
fi

write_installed_marker

log "Done."
log "System files: $SYSTEM_ROOT"
log "Data files: $DATA_ROOT"
