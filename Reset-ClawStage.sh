#!/bin/sh
# Linux/macOS launcher; resolve files from this script, not Terminal's cwd.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Finder/Terminal may have a smaller PATH than an interactive shell.
PATH="${PATH:-/usr/bin:/bin}:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin"
export PATH
supports_python() {
    [ -n "$1" ] && [ -x "$1" ] &&
        "$1" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' >/dev/null 2>&1
}
python_bin=$(command -v python3 || :)
if ! supports_python "$python_bin"; then
    python_bin=''
    for candidate in /opt/homebrew/bin/python3 /usr/local/bin/python3 /Library/Frameworks/Python.framework/Versions/Current/bin/python3 /usr/bin/python3; do
        if supports_python "$candidate"; then
            python_bin=$candidate
            break
        fi
    done
fi
if [ -z "$python_bin" ]; then
    echo 'Python 3.9 or newer is required. Install Python, then run this launcher again.' >&2
    echo "No reset started. See: $root/README.md" >&2
    exit 1
fi
exec "$python_bin" "$root/reset-client.py" "$@"
