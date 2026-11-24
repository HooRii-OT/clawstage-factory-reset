#!/bin/sh
# OpenSSH invokes this helper for authentication. Never log its stdout.
printf '%s\n' "$CLAWSTAGE_SSH_PASSWORD"
