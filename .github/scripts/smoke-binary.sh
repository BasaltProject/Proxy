#!/bin/sh
# Exercise real configuration, listener startup and a clean console shutdown.
set -eu
binary="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
"$binary" --version | grep -q 'Basalt Proxy '
"$binary" --help | grep -q 'Usage: basalt-proxy'
printf 'stop\n' | "$binary" >startup.log 2>&1 || {
    cat startup.log
    exit 1
}
cat startup.log
grep -q 'Listening on port ' startup.log
grep -q 'Proxy stopped.' startup.log
for config in proxy servers plugins; do
    test -f "config/$config.toml"
done
