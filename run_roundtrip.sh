#!/usr/bin/env bash
set -euo pipefail

if (( $# > 3 )); then
    printf 'Usage: bash %s [build-directory] [configuration] [output-directory]\n' "$0" >&2
    exit 64
fi

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${1:-build}"
configuration="${2:-Release}"

if [[ "$build_dir" != /* ]]; then
    build_dir="$project_dir/$build_dir"
fi

output_dir="${3:-$build_dir/roundtrip-tests}"
if [[ "$output_dir" != /* ]]; then
    output_dir="$project_dir/$output_dir"
fi

exchange_tool="$build_dir/$configuration/survey_exchange"
if [[ ! -x "$exchange_tool" ]]; then
    exchange_tool="$build_dir/survey_exchange"
fi
if [[ ! -x "$exchange_tool" ]]; then
    printf 'survey_exchange was not found. Configure and build the Linux project first.\n' >&2
    exit 1
fi

exec cmake \
    "-DEXCHANGE_TOOL=$exchange_tool" \
    "-DWORK_DIR=$output_dir" \
    -P "$project_dir/cmake/roundtrip_test.cmake"