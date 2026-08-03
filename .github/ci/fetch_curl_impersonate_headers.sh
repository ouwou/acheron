#!/usr/bin/env bash
# Reconstruct curl-impersonate's patched public headers.
#
# The x86_64-linux-gnu and arm64-macos release tarballs ship only
# libcurl-impersonate.a with no headers (unlike win32, which ships include/ and
# a CMake config). Building against distro or brew curl headers instead leaves
# curl_easy_impersonate undeclared, so IS_CURL_IMPERSONATE stays off and the
# client links the impersonate library but never calls it. Older distro headers
# (Ubuntu 22.04 ships curl 7.81) also predate the curl_ws_* WebSocket API.
#
# Rebuild the real header set from upstream curl plus curl-impersonate's own
# patch. Requires curl, tar, patch, and filterdiff (patchutils).
#
# Usage: fetch_curl_impersonate_headers.sh <curl-version> <impersonate-tag> <out-dir>
# Result: <out-dir>/curl/*.h  -- pass <out-dir> as CURL_INCLUDE_DIR.
set -euo pipefail

if [ $# -ne 3 ]; then
	sed -n '/^# Usage:/,/^# Result:/p' "$0" >&2
	exit 2
fi

curl_version="$1"
impersonate_tag="$2"
out_dir="$3"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -fsSL -o "$work/curl.tar.gz" "https://curl.se/download/curl-${curl_version}.tar.gz"
tar -xzf "$work/curl.tar.gz" -C "$work"

curl -fsSL -o "$work/curl.patch" \
	"https://raw.githubusercontent.com/lexiforest/curl-impersonate/${impersonate_tag}/patches/curl.patch"

# Only the include/ hunks are wanted; the rest patch lib/ sources we don't build.
filterdiff --include='*include/curl/*' "$work/curl.patch" >"$work/headers.patch"
patch -d "$work/curl-${curl_version}" -p1 <"$work/headers.patch"

# Fail loudly rather than handing back headers that would silently disable
# impersonation, e.g. if the patch layout changes in a future release.
grep -q curl_easy_impersonate "$work/curl-${curl_version}/include/curl/easy.h"

mkdir -p "$out_dir"
rm -rf "$out_dir/curl"
cp -r "$work/curl-${curl_version}/include/curl" "$out_dir/curl"

echo "curl-impersonate headers ready: $out_dir/curl"
