#!/usr/bin/env bash
# Build the updater's crypto dependency for the declared macOS 14 baseline.
set -euo pipefail
version=3.6.5
digest=a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98
work=/tmp/magnite-openssl
mkdir -p "$work"
curl -fL --retry 3 "https://github.com/openssl/openssl/releases/download/openssl-$version/openssl-$version.tar.gz" -o "$work/source.tar.gz"
printf '%s  %s\n' "$digest" "$work/source.tar.gz" | shasum -a 256 -c -
tar -xzf "$work/source.tar.gz" -C "$work"
cd "$work/openssl-$version"
# Use logical install paths in compiled strings, then stage with DESTDIR.
# No dynamic providers are needed for Ed25519 verification.
export MACOSX_DEPLOYMENT_TARGET=14.0
./Configure darwin64-arm64-cc no-shared no-tests no-module no-dso no-engine \
  --prefix=/usr/local --openssldir=/etc/ssl -mmacosx-version-min=14.0 \
  "-ffile-prefix-map=$work=."
make -j2
make DESTDIR="$work/stage" install_sw
mkdir -p "$work/stage/usr/local/share/magnite-openssl"
cp LICENSE.txt "$work/stage/usr/local/share/magnite-openssl/"
printf 'OPENSSL_ROOT_DIR=%s\n' "$work/stage/usr/local" >> "$GITHUB_ENV"
