#!/usr/bin/env bash
# sign.sh -- Ed25519-sign one canonical payload with the release/catalog key.
#
# Usage:
#   sign.sh <private-key.pem> <payload-file> [out-prefix]
#
# The payload file must be the EXACT canonical bytes the verifier rebuilds
# (see README.md section 1):
#   update manifest:  <version>\n<url>\n<sha256>\n   (trailing newline)
#   catalog entry:    <id>\n<sha256>                 (no trailing newline)
#
# When out-prefix is given, writes:
#   <out-prefix>.hex   128 lowercase hex chars -- the signature JSON field
#   <out-prefix>.sig   64 raw signature bytes   -- for `openssl pkeyutl -verify`
# Without out-prefix, the hex is printed to stdout.
#
# Our parsers accept ONLY 128 lowercase hex chars (Signature.cpp:40-62,
# Catalog.cpp:158-180); uppercase or base64 is rejected.
set -euo pipefail

key=${1:?usage: sign.sh <private-key.pem> <payload-file> [out-prefix]}
payload=${2:?usage: sign.sh <private-key.pem> <payload-file> [out-prefix]}
out_prefix=${3:-}

command -v openssl >/dev/null 2>&1 || { echo "sign.sh: openssl not found" >&2; exit 1; }
[ -f "$key" ] || { echo "sign.sh: private key not found: $key" >&2; exit 1; }
[ -f "$payload" ] || { echo "sign.sh: payload not found: $payload" >&2; exit 1; }

tmp_sig=$(mktemp)
trap 'rm -f "$tmp_sig"' EXIT

# -rawin: sign the message bytes directly. Ed25519 is one-shot, so there is no
# digest algorithm argument; the payload is used verbatim.
openssl pkeyutl -sign -inkey "$key" -rawin -in "$payload" -out "$tmp_sig"

size=$(wc -c < "$tmp_sig" | tr -d ' ')
if [ "$size" -ne 64 ]; then
    echo "sign.sh: unexpected signature size: $size bytes (expected 64)" >&2
    exit 1
fi

hex=$(od -An -v -tx1 "$tmp_sig" | tr -d ' \n')
if [ "${#hex}" -ne 128 ]; then
    echo "sign.sh: unexpected signature hex length: ${#hex} (expected 128)" >&2
    exit 1
fi

if [ -n "$out_prefix" ]; then
    printf '%s\n' "$hex" > "$out_prefix.hex"
    cp "$tmp_sig" "$out_prefix.sig"
    echo "wrote $out_prefix.hex (signature field value)"
    echo "wrote $out_prefix.sig (raw, for openssl pkeyutl -verify)"
else
    printf '%s\n' "$hex"
fi
