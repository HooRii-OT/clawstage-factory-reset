#!/bin/sh
# Terminal opens this file from Finder. Gatekeeper approval, when requested,
# happens before this script runs; see README.md for the first-open steps.
set -u
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
cd "$root" || exit 1
printf 'ClawStage tool folder: %s\n' "$root"
/bin/sh "$root/Reset-ClawStage.sh" --reveal-log "$@"
result=$?
printf '\nTool finished (exit=%s). Logs are saved beside this launcher.\n' "$result"
# Keep Finder-launched Terminal windows readable even when configured to close
# on process exit. Do not block pipes, automated checks or redirected input.
if [ -t 0 ] && [ -t 1 ]; then
    printf 'Press Enter to close this launcher... '
    read -r answer || :
fi
exit "$result"
