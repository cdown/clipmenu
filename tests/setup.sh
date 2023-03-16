#!/usr/bin/env bash

msg() {
    printf '>>> %s\n' "$@" >&2
}

toplevel() {
    git rev-parse --show-toplevel 2>/dev/null && return
    readlink -f "${BASH_SOURCE[0]}/../.." 2>/dev/null && return
    pwd -P
}

cleanup() {
    local rc="$?"

    if (( rc != 0 )); then
        msg "ERROR: '${BASH_COMMAND}' exited with status '$rc'"
    fi

    if command -v cleanup_pre &>/dev/null; then
        cleanup_pre
    fi

    if [[ -n "${tempdir:-}" ]] && [[ -d "$tempdir" ]]; then
        # Try to use coreutils-specific args for safety; fall back to
        # busybox-compatible args if necessary.
        rm --one-file-system --preserve-root -rf "$tempdir" || rm -rf "$tempdir"
    fi
}

# Show contextual data in execution tracing (`set -x`) output
PS4='+ ${BASH_SOURCE[0]##*/}@${LINENO}${FUNCNAME[0]:+${FUNCNAME[0]}()}: '
export PS4

toplevel="$(toplevel)"

export PATH="${toplevel}/tests/fixtures/bin:${toplevel}${PATH:+:${PATH}}"

tempdir="$(mktemp -d)"

trap cleanup EXIT

export CM_DIR="$tempdir"

dir=$(clipctl cache-dir)

# shellcheck disable=SC2034
cache_file=$dir/line_cache
