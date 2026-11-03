#!/usr/bin/env bash
set -euo pipefail
build="${1:?build directory}"
remote=/data/local/tmp/basalt-ci
connected=0
for attempt in {1..30}; do
    state="$(adb shell dumpsys connectivity 2>/dev/null || true)"
    if [[ "$state" == *VALIDATED* ]]; then
        connected=1
        break
    fi
    sleep 2
done
if [ "$connected" != 1 ]; then
    printf '%s\n' "$state"
    echo 'Android booted, but its network did not become ready'
    exit 1
fi
adb shell "rm -rf $remote && mkdir -p $remote"
adb push .github/scripts/smoke-binary.sh "$remote/smoke-binary.sh"
for binary in "$build"/bin/* \
    "$build/wire/wire-common/wire-common-test" \
    "$build/wire/wire-io/wire-io-test" \
    "$build/wire/wire-auth/wire-auth-test" \
    "$build/proxy-config/proxy-config-test" \
    "$build/proxy-plugins/proxy-plugins-test" \
    "$build/proxy-runtime/"proxy-runtime-*-test; do
    adb push "$binary" "$remote/"
done
adb shell "chmod 755 $remote/*"
adb shell "cd $remote && TMPDIR=$remote sh ./smoke-binary.sh ./BasaltProxy-*"
for test in wire-common-test wire-io-test wire-auth-test proxy-config-test proxy-plugins-test proxy-runtime-cli-test proxy-runtime-network-test proxy-runtime-admission-test proxy-runtime-session-test proxy-runtime-backend-selection-test proxy-runtime-backend-login-test; do
    adb shell "cd $remote && TMPDIR=$remote BASALT_TEST_ANDROID_DNS_HOST=piston-meta.mojang.com BASALT_TEST_SYSTEM_HTTPS_URL=https://piston-meta.mojang.com/mc/game/version_manifest_v2.json ./$test"
done
