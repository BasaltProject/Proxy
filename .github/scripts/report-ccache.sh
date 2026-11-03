#!/bin/sh
# Text statistics work with older ccache releases as well as current ones.
set -eu
if ! command -v ccache >/dev/null 2>&1; then
    echo '::notice::ccache is unavailable; its setup may not have completed.'
    exit 0
fi
stats="$(ccache --show-stats)"
report() {
    printf '## CCache Statistics\n\n```text\n'
    ccache --version | sed -n '1p'
    printf '%s\n```\n' "$stats"
}
summary="${1:-${GITHUB_STEP_SUMMARY:-}}"
if [ -n "$summary" ]; then
    report | tee -a "$summary"
else
    report
fi
