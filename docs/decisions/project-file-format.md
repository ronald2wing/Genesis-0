# Genesis-0 — Project File: Current Format Only (Decision Record)

Status: Accepted (2026-10-04).

Deciders: project owner.

Scope: how `concat.json` project documents are read and written, and where the `f64`-seconds /
`Rational` time boundary falls.

## Decision

The host project file is a host-native format. The reader opens only the current shape:

- No migrations, no flat-timeline lifting, no shared-frame inheritance.
- No `f64`-seconds edit points: edit-point times are `Rational`, serialized as `"num/den"` strings,
  and the reader accepts only a string.
- No named key eases: a key's ease is its four control points; an absent ease is linear.
- A document written by a newer build (a `version` above `DOCUMENT_VERSION`) is refused whole, never
  half-loaded.

## Current shape

- `version` is `DOCUMENT_VERSION` (= 1). The reader refuses `version > 1`; an absent or non-integer
  `version` is treated as current.
- Top-level keys are `version`, `name`, `video`, `media`, `fonts`, `timelines`, `activeTimelineId`.
  Any other top-level key is reported through `DocumentLoadReport` and dropped on the next save.
- The reader stays tolerant of hand-edited files the way a command would be: entries that do not
  parse are dropped individually, out-of-range values are clamped, and a document that is not an
  object or has no surviving timeline returns `nullopt` rather than panicking.

## What was removed

- `src/project/document/Migrate.cpp` and its two migrations (flat-timeline lift, shared-frame
  inheritance).
- The flat `tracks`/`clips`/`video` top-level mirror on write.
- The `f64`-seconds read branch (`decode_time` accepted a number; it now accepts only a string).
- The named key-ease spelling (`read_ease` accepted a `"inOut"` string; control points only).

## Consequences

- A document produced by a build that shipped any removed shape is no longer read: it degrades to a
  dropped entry, or — for the version bump — a whole-file refusal. There is no migration path.
- The on-disk format evolves by bumping `DOCUMENT_VERSION`; an older build refuses a newer file
  whole, so a newer file is never partially loaded and re-saved over.
