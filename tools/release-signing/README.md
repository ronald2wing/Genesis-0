# Release signing

Tooling and process for the two **Ed25519** trust domains in this project:

1. the **update manifest** the self-updater verifies against `kReleasePublicKey`
   (`src/workspace/update/Signature.h`), and
2. the **catalog entry** signature verified against `kCatalogPublicKey`
   (`src/extensions/Catalog.h`).

This directory holds **no keys**. It documents how to mint one on a release
machine and how to sign the exact bytes the in-tree verifier rebuilds.

`keygen.sh` mints a keypair. `sign.sh` signs one canonical payload file and
emits the signature in the encoding the parser expects.

> **The private key never enters the repository or CI.** Generate it on a
> release workstation (or an offline machine), keep it out of git, out of the
> build sandbox, and out of any CI job that checks out this repo. Only the raw
> 32-byte public half is embedded in source. See *Private key rule* below.

## 1. What exactly gets signed (canonical form)

The verifier does not sign the JSON. It rebuilds a fixed byte string from
parsed fields and verifies the detached signature over those bytes. Sign the
same bytes, byte for byte.

### Update manifest

Source of truth: `signed_payload` in `src/workspace/update/Manifest.cpp:16-19`.

```
version + "\n" + url + "\n" + sha256 + "\n"
```

That is:

```
<version>\n<url>\n<sha256>\n
```

- `version` — the release version string, e.g. `1.2.3`.
- `url` — the package download URL.
- `sha256` — the package digest, exactly 64 lowercase hex digits
  (`Manifest.cpp:86-88`).
- **The trailing `\n` after `sha256` is part of the payload.** Do not omit it.
- `notes` is **not** signed (`Manifest.h:28-32`); a compromised feed can only
  misinform through it, never install.
- The `signature` field itself is not part of the payload.

The signature is stored in the manifest's `"signature"` JSON string.

### Catalog entry

Source of truth: `signed_entry_payload` in `src/extensions/Catalog.cpp:192-195`.

```
id + "\n" + sha256
```

That is:

```
<id>\n<sha256>
```

- `id` — the namespaced extension id, e.g. `acme.my-effect`.
- `sha256` — the content digest the source must match (`CatalogEntry::sha256`).
- **There is no trailing newline here**, unlike the manifest payload. This
  asymmetry is the easiest signing mistake; the two payloads are deliberately
  not identical.
- `sha256` is mandatory for a signed entry: `verify_entry_signature`
  (`Catalog.cpp:197-212`) returns `Rejected` when the entry has a signature but
  no `sha256`, because there would be nothing to bind the identity to.
- Not signed: `name`, `version`, `description`, `source` (including
  `source.url`). The signature binds **id to content digest**; the digest is
  then enforced against the materialized source before install
  (`install_from_catalog`, `Catalog.cpp:296-325`). See *Ambiguities* below.

The signature is stored in the entry's `"signature"` JSON string.

### Signature wire format

Both fields are a raw **64-byte** Ed25519 signature encoded as **128 lowercase
hex digits**, `[0-9a-f]`.

The parsers are strict:

- `src/workspace/update/Signature.cpp:40-62` — requires length 128 and only
  lowercase `a-f`; anything else decodes to empty and verifies as `Rejected`.
- `src/extensions/Catalog.cpp:158-180` — same rules.

Uppercase hex, base64, or a different length is **rejected** (not normalized).

## 2. Verification built into the app

- `verify_ed25519` calls `EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, ...)`
  with the 32 raw public bytes (`Signature.cpp:73`), then
  `EVP_DigestVerify` over the canonical payload.
- The crypto path is compiled only when `GENESIS_UPDATER_ENABLED` is defined
  (`Signature.cpp:10-12,69`), which CMake sets when `GENESIS_UPDATER=ON` and
  OpenSSL 3.x is found. Otherwise `verify_ed25519` returns `Unavailable`.
- **Catalog signature verification has the same gate.** `Catalog.cpp` routes
  through `workspace::update::verify_ed25519`, so a signed catalog entry needs a
  `GENESIS_UPDATER=ON` + OpenSSL build too; without it the install is refused
  as `Unavailable` (`Catalog.cpp:309-313`).

## 3. Where the public half is embedded

Both constants are `std::array<std::uint8_t, 32>` and default to all zeros,
meaning "not configured" (`release_key_configured` in `Signature.cpp:25-33`,
`catalog_key_configured` in `Catalog.cpp:182-190`). An all-zero key verifies
nothing; every install/entry is refused.

| Trust domain | File:line | Constant |
|---|---|---|
| Update manifest | `src/workspace/update/Signature.h:23` | `kReleasePublicKey` |
| Catalog entry | `src/extensions/Catalog.h:64` | `kCatalogPublicKey` |

They are **separate trust domains** (see the comment at `Catalog.h:58-63`). Do
not sign both with one keypair.

## 4. Generate a keypair

`keygen.sh` mints an Ed25519 keypair, refuses to overwrite an existing private
key, writes the PEM halves plus the raw public key as hex, and prints a ready
C++ initializer.

```sh
tools/release-signing/keygen.sh /secure/offline/release-keys release
```

Writes under `/secure/offline/release-keys/`:

- `release-ed25519-private.pem` — secret seed (mode `600`). **Never commit.**
- `release-ed25519-public.pem` — public half, for `openssl pkeyutl -verify`.
- `release-ed25519-public.hex` — the raw 32-byte public key as 64 lowercase hex.

To mint the catalog key, run it again with a distinct name and directory:

```sh
tools/release-signing/keygen.sh /secure/offline/catalog-keys catalog
```

The underlying OpenSSL commands (what `keygen.sh` runs):

```sh
openssl genpkey -algorithm ED25519 -out release-ed25519-private.pem
chmod 600 release-ed25519-private.pem
openssl pkey -in release-ed25519-private.pem -pubout -out release-ed25519-public.pem
openssl pkey -in release-ed25519-private.pem -pubout -outform DER -out release-ed25519-public.der

# SPKI DER is 44 bytes: 12-byte prefix 302a300506032b6570032100 then the raw 32.
tail -c 32 release-ed25519-public.der | od -An -v -tx1 | tr -d ' \n'
```

### Embed the public key

Take the C++ initializer printed by `keygen.sh` and replace the empty `{ }` at
the matching line, then commit only that source change:

- release key -> `src/workspace/update/Signature.h:23`
- catalog key -> `src/extensions/Catalog.h:64`

Example shape (values come from `keygen.sh`; shown 8 bytes per line):

```cpp
inline constexpr PublicKey kReleasePublicKey{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* ... 24 more bytes ... */
};
```

Keep the key bytes in one place; do not reformat into a hex string at runtime.

## 5. Sign an update manifest

Build the exact payload bytes, sign, and paste the hex into the manifest JSON.

```sh
version=1.2.3
url=https://example.invalid/genesis-0/1.2.3/genesis-0-1.2.3.tar.zst
sha256=$(sha256sum genesis-0-1.2.3.tar.zst | cut -d' ' -f1)   # 64 lowercase hex

# Payload: version \n url \n sha256 \n  (trailing newline required)
printf '%s\n%s\n%s\n' "$version" "$url" "$sha256" > /tmp/manifest-payload.bin

tools/release-signing/sign.sh /secure/offline/release-keys/release-ed25519-private.pem \
    /tmp/manifest-payload.bin /tmp/manifest
# -> /tmp/manifest.hex  (128 hex chars: the "signature" field)
# -> /tmp/manifest.sig  (64 raw bytes: for openssl verify)
```

Put the contents of `/tmp/manifest.hex` into the manifest:

```json
{
  "version": "1.2.3",
  "url": "https://example.invalid/genesis-0/1.2.3/genesis-0-1.2.3.tar.zst",
  "sha256": "<64 lowercase hex>",
  "signature": "<contents of /tmp/manifest.hex>",
  "notes": "optional, unsigned"
}
```

## 6. Sign a catalog entry

```sh
id=acme.my-effect
sha256=<64 lowercase hex of the materialized source tree>

# Payload: id \n sha256  (NO trailing newline)
printf '%s\n%s' "$id" "$sha256" > /tmp/entry-payload.bin

tools/release-signing/sign.sh /secure/offline/catalog-keys/catalog-ed25519-private.pem \
    /tmp/entry-payload.bin /tmp/entry
```

Put `/tmp/entry.hex` into the entry's `"signature"` field. The entry must also
carry the same `sha256`, or verification fails closed.

## 7. Verify independently

`sign.sh` is a thin wrapper over one OpenSSL call. Verify the detached signature
against the public PEM (uses the raw `.sig`):

```sh
openssl pkeyutl -verify -pubin -inkey release-ed25519-public.pem -rawin \
    -in /tmp/manifest-payload.bin -sigfile /tmp/manifest.sig
# Signature Verified Successfully
```

`-rawin` tells OpenSSL the input is the message itself (Ed25519 is one-shot;
there is no digest algorithm argument). To confirm the **hex field** decodes to
the same bytes:

```sh
# hex -> raw, then compare (perl is portable; `xxd -r -p` also works)
perl -pe 's/\s//g; $_=pack("H*",$_)' /tmp/manifest.hex > /tmp/manifest-from-hex.sig
cmp /tmp/manifest.sig /tmp/manifest-from-hex.sig && echo "hex round-trips"
```

## 8. Key rotation

`kReleasePublicKey` is a single key; there is no keyring and no dual-key trust
in this design. A build only trusts the exact bytes compiled into it, so
rotation is a **channel-mediated** operation, not a self-update:

1. Mint the new keypair with `keygen.sh`.
2. Sign the release **that carries the new public key** with the **old** private
   key. It must verify on builds still trusting the old key.
3. Ship that build through the platform channel (distro/Flatpak/store or a
   manual download). Do not rely on the self-updater to bootstrap a new key: the
   running app has no way to trust the new key until it is installed.
4. Only after the new-key build is widely deployed, embed the new public key in
   the next release and retire the old private key.
5. Keep the retired key's public PEM archived; old manifests signed by it will
   correctly stop verifying once no shipped build trusts it.

The catalog key rotates independently on the same model. A catalog entry signed
by a retired key is refused once no shipped build embeds that key. Because there
is no anti-downgrade/anti-replay protection (`docs/decisions/auto-update.md`
§10), an old *validly signed* manifest remains verifiable — rotation does not
imply freshness.

## 9. Private key rule

- The private key is generated offline, stored outside the checkout, and **never
  committed, never built into the app, never exposed to CI**.
- Only the raw 32-byte public half is embedded in source (the tables in §3).
- Do not paste a private key into an issue, a build log, or a CI secret that a
  fork/PR job can read. Release signing is a local step on the release machine,
  not a CI step in this repo.
- `keygen.sh` refuses to overwrite an existing private key so a stray re-run
  cannot silently replace the key that signed already-shipped builds.

## 10. Ambiguities and observations in the current payload construction

Recorded while verifying this tooling against the code; none blocks signing.

1. **Manifest has a trailing newline, catalog does not.** `Manifest.cpp:18` ends
   `sha256 + "\n"`; `Catalog.cpp:194` is `id + "\n" + sha256` with nothing after.
   Sign each with its own exact form.
2. **The catalog signature does not cover `source.url`.** It binds `id` to
   `sha256` only. A compromised catalog could point the id at a different URL,
   but the digest is verified against the materialized source before install
   (`Catalog.cpp:325` -> `install_from_source`), so the *content* is still
   pinned. The signed set is intentionally minimal (verify-before-fetch).
3. **Catalog verification is gated on the updater build option.** There is no
   separate `GENESIS_CATALOG` gate; `Catalog.cpp` reuses
   `workspace::update::verify_ed25519`, which returns `Unavailable` unless
   `GENESIS_UPDATER_ENABLED` (`Signature.cpp:69`). A crypto-free build cannot
   verify a signed catalog entry.
4. **No freshness/ordering.** The signed manifest fields are `version`, `url`,
   `sha256` with no timestamp or monotonic counter (`docs/decisions/auto-update.md`
   §10, deferred), so a signature proves origin, not recency.
5. **Strict lowercase hex only.** The in-tree decoder does not accept uppercase;
   always emit lowercase (both scripts do).
