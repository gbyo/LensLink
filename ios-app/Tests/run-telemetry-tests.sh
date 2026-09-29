#!/bin/bash
# Compiles Sources/Telemetry.swift together with TelemetryTests.swift for
# the iOS Simulator SDK and runs the result in a simulator (macOS with
# Xcode only). Opt-in, like obs-plugin/tests; CI does not run it. Boots and
# then removes a throwaway simulator device unless one is already booted.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
udid=""
created=""
cleanup() {
    if [ -n "$created" ]; then
        xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true
        xcrun simctl delete "$udid" >/dev/null 2>&1 || true
    fi
    rm -rf "$out"
}
trap cleanup EXIT

# Top-level code is only allowed in a file named main.swift.
cp "$here/TelemetryTests.swift" "$out/main.swift"
xcrun --sdk iphonesimulator swiftc \
    -target arm64-apple-ios15.0-simulator \
    "$here/../Sources/Telemetry.swift" "$out/main.swift" \
    -o "$out/telemetry-tests"

udid="$(xcrun simctl list devices booted | grep -Eo '[0-9A-F-]{36}' | head -1 || true)"
if [ -z "$udid" ]; then
    # Any runtime that supports an iPhone device type (a runtime can be
    # installed for a single exotic device only).
    read -r runtime devicetype < <(xcrun simctl list runtimes -j | python3 -c '
import json, sys
for r in json.load(sys.stdin)["runtimes"]:
    if not r.get("isAvailable"):
        continue
    for t in r["supportedDeviceTypes"]:
        if t["identifier"].startswith("com.apple.CoreSimulator.SimDeviceType.iPhone-1"):
            print(r["identifier"], t["identifier"])
            sys.exit(0)')
    udid="$(xcrun simctl create lenslink-telemetry-tests "$devicetype" "$runtime")"
    created=1
    xcrun simctl boot "$udid"
fi
xcrun simctl spawn "$udid" "$out/telemetry-tests"
