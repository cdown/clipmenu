#!/usr/bin/env bash

toplevel() {
    git rev-parse --show-toplevel 2>/dev/null && return
    readlink -f "${BASH_SOURCE[0]}/../.." 2>/dev/null && return
    pwd -P
}

cleanup() {
    if command -v cleanup_pre &>/dev/null; then
        cleanup_pre
    fi

    if [[ -n "${tempdir:-}" ]] && [[ -d "$tempdir" ]]; then
        # Try to use coreutils-specific args for safety; fall back to
        # busybox-compatible args if necessary.
        rm --one-file-system --preserve-root -rf "$tempdir" || rm -rf "$tempdir"
    fi
}

toplevel="$(toplevel)"

export PATH="${toplevel}${PATH:+:${PATH}}"

tempdir="$(mktemp -d)"

trap cleanup EXIT

export CM_DIR="$tempdir"

dir=$(clipctl cache-dir)

# shellcheck disable=SC2034
cache_file=$dir/line_cache
