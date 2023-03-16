#!/usr/bin/env bash

toplevel() {
    git rev-parse --show-toplevel 2>/dev/null && return
    readlink -f "${BASH_SOURCE[0]}/../.." 2>/dev/null && return
    pwd -P
}

toplevel="$(toplevel)"

export PATH="${toplevel}${PATH:+:${PATH}}"

dir=$(clipctl cache-dir)

# shellcheck disable=SC2034
cache_file=$dir/line_cache
