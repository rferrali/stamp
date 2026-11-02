#!/bin/bash
# Run inside the main module's _flatpak_build directory, before Flatpak cleanup.
set -euo pipefail

source_dir="$(cd .. && pwd)"
reports="$PWD/ci-reports"
instrumented="$source_dir/_ci-coverage"
clang_build="$source_dir/_ci-clang"
mkdir -p "$reports/coverage" "$reports/scanbuild"

export PYTHONPATH="$source_dir/.ci-tools${PYTHONPATH:+:$PYTHONPATH}"
export GIO_USE_VFS=local GSETTINGS_BACKEND=memory GTK_A11Y=none
status=0

# Keep coverage instrumentation out of the regular Flatpak executable.
CC=gcc meson setup "$instrumented" "$source_dir" --prefix=/app --buildtype=debug \
    -Db_coverage=true
meson compile -C "$instrumented" -j "${FLATPAK_BUILDER_N_JOBS:-2}"
meson test -C "$instrumented" --print-errorlogs --no-stdsplit --no-rebuild || status=1
if [ -f "$instrumented/meson-logs/testlog.junit.xml" ]; then
    cp "$instrumented/meson-logs/testlog.junit.xml" "$reports/testlog-coverage.junit.xml"
fi
python3 -m gcovr --root "$source_dir" --filter "$source_dir/src/" \
    --xml "$reports/cobertura.xml" --html-details "$reports/coverage/index.html" \
    --print-summary "$instrumented" || status=1

# Configure and compile under scan-build so the same pass checks Clang support.
# Disable ccache here: cached object files would bypass static analysis.
CCACHE_DISABLE=1 bash "$source_dir/.run-scan-build" --use-cc=clang \
    -o "$reports/scanbuild-runs" bash -ec '
        meson setup "$1" "$2" --prefix=/app --buildtype=debug
        meson compile -C "$1" -j "$3"
    ' _ "$clang_build" "$source_dir" "${FLATPAK_BUILDER_N_JOBS:-2}" || status=1
if [ -d "$clang_build/meson-logs" ]; then
    cp -a "$clang_build/meson-logs" "$reports/clang-logs"
fi
cp -a "$instrumented/meson-logs" "$reports/coverage-logs"

printf '<html><body><h1>Scan-Build</h1><p>No findings report generated. Check the CI job log for build errors.</p></body></html>\n' > "$reports/scanbuild/index.html"
for report in "$reports"/scanbuild-runs/*/index.html; do
    if [ -f "$report" ]; then
        cp -a "$(dirname "$report")/." "$reports/scanbuild/"
    fi
done

exit "$status"
