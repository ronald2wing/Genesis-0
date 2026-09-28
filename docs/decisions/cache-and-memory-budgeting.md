# Genesis-0 — Cache and Memory Budgeting Decision

Status: Not adopted — policy considered, declined 2026-10-04. Checked: 2026-10-04.

Date: 2026-10-04. Deciders: project owner + orchestrator.

Scope: **budgeting of frame/media memory** — whether Genesis-0 should port the reference editor's RAM probe to size
an in-RAM reader pool. It is **not** the on-disk processed-media cache policy, which is already
implemented (`CacheSweep`); this record only explains why the two are not the same control.

---

## 1. What the reference editor does

the reference host crate's `src/memory.rs` probes physical RAM (`sysconf` on Linux/macOS,
`GlobalMemoryStatusEx` on Windows) at startup and derives a byte budget from it. That budget sizes an
**in-RAM reader/decoder pool** — the process owns the frame buffers, so it must size them against the
machine it runs on. The reference `media` crate separately owns `decode.rs`/`pool.rs`/`prefetch.rs`
(`docs/history/rewrite.md:57`); the probe is the host-level input to that pool's capacity.

## 2. Why we do not port it

1. **Layer mismatch.** Genesis-0 delegates frame buffering to GStreamer/GES. The host keeps no
   reference into engine frame memory it does not own (`docs/decisions/engine-adapter-contract.md:82`); GES
   owns the decode pool, its sizing, and its eviction. A host-side RAM probe has no consumer.
2. **GStreamer-managed pools.** The reference `media` crate is a **replace** verdict
   (`docs/history/rewrite.md:57`): the host does not re-implement codecs or a decoder pool (R5), so the
   pool that would consume the budget is not ours to size.
3. **The one cap Genesis-0 owns is on disk.** `CacheSweep` bounds the processed-media caches
   (waveforms, filmstrips, posters, reverses, proxies, masks, title PNGs) to a configurable cap,
   10 GiB default (`src/workspace/CacheSweep.h:23`). A disk cap is a function of free disk space, not
   physical RAM; tying it to RAM would evict on the wrong axis and misbehave on small-RAM/large-disk
   machines.

## 3. Current posture

- The on-disk cache is bounded by `CacheSweep` (`src/workspace/CacheSweep.{h,cpp}`): three roots, cap
  + oldest-first (mtime) eviction, source assets and extension packs never touched.
- The cap persists in `cache.json` and is driven from Settings → Storage through the `cache` context
  property (`CacheController`); the default is 10 GiB (`docs/building.md` §"Processed-media cache").
- **No RAM probe exists**, by design — there is nothing on the host that would read its result.

## 4. Revisit condition

Reopen this record if Genesis-0 ever grows a **custom frame/reader pool** the host owns (i.e. frame
buffers not delegated to GES). At that point a machine-relative RAM probe becomes meaningful and
should be added **per-platform** (`sysconf`/`sysctlbyname`/`GlobalMemoryStatusEx`) as the pool's
sizing input — the same shape as the reference editor's `memory.rs`. Until then the disk cap is the only budget,
and RAM probing stays unadopted.

## Evidence index

| Fact | Location |
|---|---|
| The reference editor's RAM probe → in-RAM reader pool | the reference host crate's `src/memory.rs` |
| the reference `media` crate (decode/pool/prefetch) is **replace**; GES owns decode | `docs/history/rewrite.md:57` |
| the reference `host` crate is **re-derive**; playback/scheduler re-implemented on GES | `docs/history/rewrite.md:63` |
| Engine keeps no reference into host memory it does not own | `docs/decisions/engine-adapter-contract.md:82` |
| Processed-media cache cap + eviction | `src/workspace/CacheSweep.h:23-61` |
| Cache section (roots, controller, 10 GiB default) | `docs/building.md` §"Processed-media cache" |
