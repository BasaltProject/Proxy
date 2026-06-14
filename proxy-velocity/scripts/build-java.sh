#!/usr/bin/env bash
# Builds the Velocity facade into <out>/velocity (default .basalt): the verified libraries in lib/, basalt-velocity.jar, and the test plugin.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-.basalt}"
"$here/fetch-deps.sh" "$out/velocity"
work="$(mktemp -d)"
cp="$(ls "$out"/velocity/lib/*.jar | tr '\n' ':')"
mkdir -p "$work/classes" "$work/test-classes" "$out/test"
javac --release 21 -proc:none -cp "$cp" -d "$work/classes" $(find "$here/../java" -name '*.java')
jar --create --file "$out/velocity/basalt-velocity.jar" -C "$work/classes" .
javac --release 21 -proc:none -cp "$cp" -d "$work/test-classes" $(find "$here/../test-plugin/src" -name '*.java')
cp "$here/../test-plugin/"*.yml "$here/../test-plugin/"*.json "$work/test-classes/" 2>/dev/null || true
jar --create --file "$out/test/BasaltVelocityTest.jar" -C "$work/test-classes" .
