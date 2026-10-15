#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Packages the iOS module SDK: what build-modules.ps1 needs to compile the
# generated game modules for iOS on a PC without Xcode.
#
#   include/   Nebula runtime headers
#   lib/       the runtime's static libraries the modules link
#              (galaxy_ppc_float, galaxy_softfloat, galaxy_dsp_alu), iOS arm64
#   sysroot/   from the iOS SDK: the C and libc++ headers those runtime
#              headers include, and the text stubs (.tbd) of libSystem and
#              libc++ to link against. Apple's SDK terms apply to these
#              files; the kit is for building your own copy.
#
# Usage: tools/ios/package-module-sdk.sh <iOS build dir> <output dir>
# Runs on macOS with Xcode. Set PROBE_CLANG to also record the headers a
# second clang (e.g. Homebrew LLVM, like the PC's) pulls in.
set -euo pipefail

build=$(cd "$1" && pwd)
out=$2
root=$(cd "$(dirname "$0")/../.." && pwd)
target=arm64-apple-ios16.0
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
toolchain=$(dirname "$(dirname "$(xcrun --sdk iphoneos --find clang++)")")

rm -rf "$out"
mkdir -p "$out/include" "$out/lib" "$out/sysroot/usr/include/c++" "$out/sysroot/usr/lib/system"

cp -R "$root/runtime/include/." "$out/include/"
for lib in galaxy_ppc_float galaxy_softfloat galaxy_dsp_alu; do
    cp "$build/lib$lib.a" "$out/lib/"
done

# libc++: all of it (a different clang can take other branches inside it).
libcxx="$sdk/usr/include/c++/v1"
[ -d "$libcxx" ] || libcxx="$toolchain/usr/include/c++/v1"
cp -R "$libcxx" "$out/sysroot/usr/include/c++/v1"

# C headers: the ones the probe includes, with each compiler.
deps=$(mktemp)
probe() {
    "$@" -target "$target" -isysroot "$sdk" -std=c++20 -DGALAXY_BUILDING_GAME_MODULE=1 -DNDEBUG \
        -I "$root/runtime/include" -E -o /dev/null -MD -MF "$deps.d" "$root/tools/ios/module_sdk_probe.cpp"
    tr ' \\' '\n\n' < "$deps.d" | grep -v ':$' | sed '/^$/d' >> "$deps"
}
probe xcrun --sdk iphoneos clang++
if [ -n "${PROBE_CLANG:-}" ]; then
    probe "$PROBE_CLANG" -nostdinc++ -isystem "$libcxx"
fi
# Another clang version takes other branches in these headers too, so copy
# every top-level C header and each whole directory the probe touched.
mkdir -p "$out/sysroot/usr/include"
find "$sdk/usr/include" -maxdepth 1 -type f -name '*.h' -exec cp {} "$out/sysroot/usr/include/" \;
sort -u "$deps" | while read -r file; do
    case "$file" in
    "$sdk"/usr/include/c++/*) ;;
    "$sdk"/usr/include/*/*)
        rel=${file#"$sdk"/usr/include/}
        top=${rel%%/*}
        [ -d "$out/sysroot/usr/include/$top" ] || cp -R "$sdk/usr/include/$top" "$out/sysroot/usr/include/$top"
        ;;
    "$sdk"/*)
        rel=${file#"$sdk"/}
        [ -e "$out/sysroot/$rel" ] && continue
        mkdir -p "$out/sysroot/$(dirname "$rel")"
        cp "$file" "$out/sysroot/$rel"
        ;;
    esac
done
# The SDK's files are read-only; the kit's need not be.
chmod -R u+w "$out"
rm -f "$deps" "$deps.d"

# Link stubs (text). libSystem re-exports usr/lib/system/*.
cp -L "$sdk/usr/lib/libSystem.tbd" "$sdk/usr/lib/libc++.tbd" "$out/sysroot/usr/lib/"
[ -f "$sdk/usr/lib/libc++abi.tbd" ] && cp -L "$sdk/usr/lib/libc++abi.tbd" "$out/sysroot/usr/lib/"
# (Older SDKs keep libSystem's parts in separate stubs; newer ones inline them.)
for stub in "$sdk"/usr/lib/system/*.tbd; do
    [ -e "$stub" ] && cp -L "$stub" "$out/sysroot/usr/lib/system/"
done
cp "$sdk/SDKSettings.json" "$out/sysroot/" 2>/dev/null || true

cat > "$out/README.txt" <<EOF
Nebula iOS module SDK, for tools/ios/build-modules.ps1.
Target: $target
iOS SDK: $(xcrun --sdk iphoneos --show-sdk-version)
Runtime revision: $(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)
Files under sysroot/ come from Apple's iOS SDK (Xcode); Apple's terms apply.
EOF
echo "Module SDK: $out ($(du -sh "$out" | cut -f1))"
