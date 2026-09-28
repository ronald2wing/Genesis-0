#!/usr/bin/env bash
# keygen.sh -- mint an Ed25519 release/catalog signing keypair.
#
# Usage:
#   keygen.sh <out-dir> [name]
#
#   name defaults to "release". Run it twice with distinct dirs/names to mint
#   the two independent trust domains (kReleasePublicKey, kCatalogPublicKey).
#
# Writes into <out-dir>:
#   <name>-ed25519-private.pem   secret seed, mode 600 -- NEVER COMMIT
#   <name>-ed25519-public.pem    public half for `openssl pkeyutl -verify`
#   <name>-ed25519-public.hex    raw 32-byte public key, 64 lowercase hex chars
# and prints a C++ initializer for the embedded key constant.
#
# The private key is refused if it already exists, so a stray re-run cannot
# silently replace the key that signed already-shipped builds.
set -euo pipefail

out_dir=${1:?usage: keygen.sh <out-dir> [name]}
name=${2:-release}

command -v openssl >/dev/null 2>&1 || { echo "keygen.sh: openssl not found" >&2; exit 1; }

mkdir -p "$out_dir"
priv="$out_dir/$name-ed25519-private.pem"
pub="$out_dir/$name-ed25519-public.pem"
pub_der="$out_dir/$name-ed25519-public.der"

if [ -e "$priv" ]; then
    echo "keygen.sh: refusing to overwrite existing private key: $priv" >&2
    exit 1
fi

# Ed25519 private key as a PKCS#8 PEM.
openssl genpkey -algorithm ED25519 -out "$priv"
chmod 600 "$priv"

# Public half, PEM and DER. The ED25519 SPKI DER is 44 bytes: the fixed 12-byte
# prefix 302a300506032b6570032100 followed by the raw 32-byte key.
openssl pkey -in "$priv" -pubout -out "$pub"
openssl pkey -in "$priv" -pubout -outform DER -out "$pub_der"

# Raw 32 bytes -> lowercase hex, matching PublicKey in Signature.h.
hex=$(tail -c 32 "$pub_der" | od -An -v -tx1 | tr -d ' \n')
if [ "${#hex}" -ne 64 ]; then
    echo "keygen.sh: unexpected public key length (${#hex} hex chars)" >&2
    exit 1
fi
printf '%s\n' "$hex" > "$out_dir/$name-ed25519-public.hex"
rm -f "$pub_der"

echo "private key: $priv (mode 600 -- keep offline, never commit)"
echo "public key:  $pub"
echo "raw public key (hex): $hex"
echo
echo "C++ initializer (embed the public half, e.g. Signature.h:23 or Catalog.h:64):"
printf 'inline constexpr PublicKey kPublicKey{\n'
i=0
line="    "
while [ "$i" -lt 32 ]; do
    line="${line}0x${hex:$((i * 2)):2}, "
    i=$((i + 1))
    if [ $((i % 8)) -eq 0 ]; then
        printf '%s\n' "${line% }"
        line="    "
    fi
done
printf '};\n'
