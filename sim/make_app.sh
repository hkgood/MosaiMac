#!/bin/sh
#
# make_app.sh - wrap the simulator binary into a double-clickable .app.
#
# Produces MosaiMac.app next to the repository root. No installer, no codesign,
# no notarisation: this is a developer tool for people who already have the
# source and a compiler.
#
# SPDX-License-Identifier: Apache-2.0

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/MosaiMac.app"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "make_app.sh: the Cocoa backend is macOS-only (uname says $(uname -s))." >&2
    echo "Use 'make sim' for the headless build, which runs the same engine." >&2
    exit 1
fi

mkdir -p "$out/Contents/MacOS" "$out/Contents/Resources"

echo "building MosaiMac.app"
make -C "$root" sim-app

cp "$root/build/sim_cocoa" "$out/Contents/MacOS/MosaiMac"

cat > "$out/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>            <string>MosaiMac</string>
    <key>CFBundleDisplayName</key>     <string>MosaiMac</string>
    <key>CFBundleExecutable</key>      <string>MosaiMac</string>
    <key>CFBundleIdentifier</key>      <string>dev.hkgood.mosaimac</string>
    <key>CFBundlePackageType</key>     <string>APPL</string>
    <key>CFBundleShortVersionString</key> <string>0.1.0</string>
    <key>CFBundleVersion</key>         <string>0.1.0</string>
    <key>NSHighResolutionCapable</key> <true/>
    <key>LSMinimumSystemVersion</key>  <string>10.13</string>
    <key>NSHumanReadableCopyright</key>
    <string>MosaiMac - QuickDraw for ESP32-S31</string>
</dict>
</plist>
PLIST

# Ad-hoc signature. Unsigned bundles are killed on first launch on Apple
# silicon, and this costs nothing.
codesign --force --deep --sign - "$out" 2>/dev/null || \
    echo "note: codesign unavailable, the bundle will still run locally"

echo
echo "built $out"
echo "run it with:  open $out"
