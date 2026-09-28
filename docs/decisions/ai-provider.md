# Genesis-0 — AI Provider Decision

Status: Implemented — host-side model store and provider seam; runtimes gated. Checked: 2026-10-02.

Date: 2026-09-30. Implementation reconciliation: 2026-10-02.

Deciders: project owner (the recommendation §4 was followed as written).

Scope: where AI inference runs, where a provider plugs into the existing job contract, which
dependencies and model licences are implicated, and what can be verified without a chosen provider.
This record does not pick a model and does not design a runtime.

## Terminology (enforced)

- **Job** — one asynchronous unit of work (transcribe, synthesize speech, detect subjects, enhance)
  with a request and a result. The contract is `src/jobs/`.
- **Provider** — the thing that executes a job: a local runtime, a bundled model, a downloaded
  model, or a remote service.
- **Seam** — the process boundary across which a provider is reached. Genesis-0 already owns one:
  the injectable worker the `Scheduler` runs.
- **Model** — a set of weights, with its own licence separate from the code that runs it.

## 1. Context

The AI job/artifact contract is settled and implemented: async submit / status / progress / cancel /
result, with generation adding nondestructive clips, analysis adding masks/tracks/captions, and the
source never overwritten (R10; `docs/architecture.md` §12, `:236-246`). The contract, not the
provider, is the durable part.

What is deliberately **not** settled is where inference runs. No specific AI models are selected or
installed (`docs/architecture.md` §12, `:246`), ONNX Runtime is not selected
(`docs/history/rewrite.md:60`), and the open question is explicit: which C++ providers replace
the reference editor's whisper.cpp/Kokoro/`ort`, and whether transcription/TTS is in the first parity target or
deferred (`docs/history/rewrite.md` §7 Q6, `:566-567`).

The reference editor already demonstrates three provider shapes, so this is a choice about which to re-derive, not
a greenfield design:

- **Local, in-process, model on disk.** The reference `speech` crate (3,094 LOC) runs whisper.cpp through
  `whisper-rs` in-process, and Kokoro / Pocket TTS through `sherpa-onnx`; Chatterbox runs through
  ONNX Runtime directly and is desktop-only (`docs/history/rewrite.md:64`;
  the reference speech crate's `Cargo.toml:29-77`).
- **Local, in-process, ONNX.** The reference `vision` crate (2,760 LOC) runs RVM person matting, IS-Net object
  detection, SlimSAM brush encode/decode, and Real-ESRGAN enhance, all through `ort`
  (`docs/history/rewrite.md:60`; the reference vision crate's `Cargo.toml:24-33`).
- **Host-owned model download.** No weights ship in the bundle; every model row carries a SHA-256
  and a download with nothing to check against is refused; the mirror is tried before upstream;
  Hugging Face upstreams are pinned to a commit
  (the reference host crate's `src/models.rs:12-23,39-56,185-195`).

**What is not claimed.** Genesis-0 has no provider and no runtime. Everything above is the reference editor's
implementation, cited as the precedent being decided about.

## 2. The contract that must not change

The job contract is the fixed point this decision sits on. It is implemented and tested:
`src/jobs/Job.{h,cpp}` and `src/jobs/Scheduler.{h,cpp}`, with `tests/jobs/JobTests.cpp` (nine
tests, `:66-293`). Its load-bearing properties:

- States are `Queued`, `Running`, `Done`, `Failed`, `Cancelled`; done and failed and cancelled are
  terminal, and illegal transitions are refused (`JobTests.cpp:167-191`).
- Cancellation wins over result: a completed result that arrives after cancel is discarded
  (`JobTests.cpp:134-151`).
- Progress is monotonic in ordering and clamped to `1.0`; a finished job ends at full
  (`JobTests.cpp:95-120`).
- Concurrency is bounded and a freed slot starts the next queued job
  (`JobTests.cpp:193-214`).
- **A job never mutates the project.** A result is handed to the caller, who applies it as
  commands; the scheduler never sees the document (`JobTests.cpp:216-244`, R10).
- The scheduler is caller-pumped and owns no thread; the worker is injectable
  (`Scheduler.h:42`; `JobTests.cpp:4-6`).

The injectable worker is the seam. **A provider is a worker implementation, not a change to the
contract.** No provider may add a state, alter cancellation semantics, or gain a path to the
project document.

## 3. Decision(s)

### (a) Cloud API provider

Inference runs at a remote service; the app sends a request and receives a result.

- **Cost.** Lowest install footprint; no model download, no GPU requirement.
- **Benefit.** Best quality per unit of local hardware; no per-platform model build.
- **Decisive costs.** Requires network, credentials, and consent; sends user media (at least
  frames/audio) off-device, which is a privacy decision only the owner can make; adds a recurring
  cost the app does not control; conflicts with an offline promise; and the request carries
  provenance/cost/credential handling the architecture already flags as required but unset
  (`docs/architecture.md` §12, `:245-246`).

### (b) Bundled local model

Weights ship inside the application package.

- **Benefit.** Fully offline; no download step; deterministic.
- **Decisive costs.** Install footprint (the reference editor's models alone are 14 MB to 178 MB per model,
  the reference vision crate's `src/models.rs:68,76,84,92,100`); a per-platform build for any GPU runtime; and
  bundling weights forces the **model licence**, not just the code licence, into the distribution.
  RVM's weights are GPL-3.0 (`models.rs:70`), IS-Net and SlimSAM are Apache-2.0 (`:78,86,94`), and
  Real-ESRGAN is BSD-3-Clause (`:102`) — several regimes in one bundle.

### (c) Host-downloaded local model (recommended first path)

No weights ship in the app. On first use the host downloads the model it needs, verifies a
SHA-256, and runs it locally.

- **Benefit.** Offline after download; small install; the user chooses which model to fetch; the
  digest discipline already exists in the reference editor (the reference host crate's `src/models.rs:12-23,185-195`) and its
  shape can be re-derived as a host-owned model store, exactly as the Extensions install store
  already stages and verifies (`src/extensions/Install.h:16-36`).
- **Cost.** A download UI and a model store; disk usage per model; the licence of each downloaded
  model must be surfaced to the user before download.
- **Why first.** It satisfies offline and privacy without a footprint or a cloud dependency, and it
  exercises the job contract with the smallest new surface. It does not foreclose (a): a cloud
  provider implements the same worker seam later.

### (d) Pack as provider

A Pack supplies the model or the inference.

- **Not possible under the current schema, and should not be forced.** A Pack is a declarative
  manifest plus a host-executed shader; its only granted capability is `execute`, and the `User`
  origin is refused (`src/extensions/Trust.cpp:8-30`). A Pack has no filesystem and no network, so
  it cannot read a model file or reach a service (`docs/architecture.md` §9, `:205-209`). A model
  download is host-owned (`models.rs`), not a Pack. A provider inside a Pack would require the
  executable-Extension path, which is the R8 deliverable of Phase 4 and does not exist yet
  (`docs/architecture.md` §8, `:208`; `docs/history/rewrite.md` §3(b)). This record does not assume
  that path.

## 4. Recommendation

1. **Freeze the job contract as it is.** It is tested and it encodes R10; nothing in this decision
   changes `src/jobs/`.
2. **Make provider selection a host concern behind the injectable worker.** A provider is chosen by
   the host and injected; the job payload never names a runtime, and `JobKind` stays the four task
   kinds it already is (`src/jobs/Job.h:40-57`). Adding a provider must not add a `JobKind`.
3. **Demonstrate one provider path first: (c), local, host-downloaded.** It is the only option that
   is offline, footprint-light, and does not send user media off-device.
4. **Keep (a) as a later, owner-gated addition behind the same seam**, subject to an explicit
   privacy/consent/cost decision. The contract is shaped so a cloud worker and a local worker are
   interchangeable.
5. **(b) is not the default; (d) is out** until the executable-Extension path exists.

This is a policy recommendation, not a model selection. No model is chosen here.

## 5. Dependencies and licences

| Candidate | Role | Where it comes from | Licence to record |
|---|---|---|---|
| ONNX Runtime via `ort` | Vision, and Chatterbox TTS | the reference vision crate's `Cargo.toml:24-33`, the reference speech crate's `Cargo.toml:29,77` | runtime + wrapper crates, separate from model |
| whisper.cpp via `whisper-rs` | Transcription | the reference speech crate's `Cargo.toml:37-39,70` | MIT-class, to be recorded |
| `sherpa-onnx` | Kokoro / Pocket TTS | the reference speech crate's `Cargo.toml:44,67` | Apache-class, to be recorded |
| RVM / IS-Net / SlimSAM / Real-ESRGAN | Vision models | the reference vision crate's `src/models.rs:63-104` | GPL-3.0 / Apache-2.0 / Apache-2.0 / BSD-3-Clause |

Two rules:

- **The runtime licence and the model licence are separate questions.** The code that runs a model
  can be permissive while the weights are copyleft: RVM's weights are GPL-3.0 (`models.rs:70`). A
  provider decision that names the runtime but not the model has decided nothing about distribution.
- **No dependency is added without the owner's explicit justification** (project constraint). This
  table names candidates; it does not select them. ONNX Runtime is explicitly not selected today
  (`docs/history/rewrite.md:60`).

The model store must record each model's licence next to its digest, as the reference editor already does
(`models.rs:58-59`), so the licence travels with the download.

## 6. What is verifiable without a provider

- **Testable now, with a fake worker:** the entire contract — state machine, cancellation beating a
  result, progress clamping, bounded concurrency, and the no-project-mutation rule. Nine tests
  already cover it (`tests/jobs/JobTests.cpp`).
- **Testable without inference:** the model-download shape — a download with no digest is refused,
  a mismatched digest is refused, bytes land in a partial file and are renamed only after the check
  (the reference host crate's `src/models.rs:185-195`). This is the same road as the Extension install store
  (`src/extensions/Install.h:16-36`).
- **Testable with a fixture, no model:** a provider that returns a canned result for a given
  request, so the worker-to-caller hand-off and the command application are exercised end to end.
- **Not testable without a real provider and runtime:** output quality, VRAM admission and
  concurrency under a real model, crash recovery of a real inference process, and any cloud
  latency/cost claim. These stay open until a provider is chosen, and must not be implied by the
  contract tests passing.

The stop/fallback in `docs/history/implementation.md:178-179` is the honest floor: if no provider
satisfies privacy and licence, ship the contract with a mock provider and keep model selection
open.

## 7. Revisit triggers

1. The owner decides user media may leave the device, unblocking (a).
2. A model's licence is found incompatible with GPL-3.0-or-later distribution, removing that model from (b) and
   (c).
3. The executable-Extension path (Phase 4) lands, making (d) technically possible (still not
   necessarily desirable).
4. Mobile is brought into scope and forces the in-process/cloud split the capability matrix already
   anticipates (`docs/architecture.md` §17, `:294`; `docs/history/rewrite.md` §4 Phase 8).
5. A dependency (ONNX Runtime, whisper.cpp, `sherpa-onnx`) cannot be built for a target platform.

## 8. Open questions for the owner

1. **May user media leave the device** for a cloud provider, or is local-only a hard requirement?
   This is a privacy decision and it gates (a) entirely.
2. **Is transcription/TTS in the first parity target, or deferred** (`docs/history/rewrite.md` §7 Q6)?
   The answer decides whether the first provider is vision, speech, or both.
3. **Which single provider path is demonstrated first** for Phase 6 — vision (matting/enhance) or
   speech (transcribe/TTS)?
4. **Which runtime is acceptable** (ONNX Runtime, whisper.cpp, `sherpa-onnx`, or none), given the
   no-dependency-without-justification rule and the explicit "not selected" status of ONNX Runtime?
5. **For model downloads, is a SHA-256 plus a pinned upstream commit sufficient provenance**, or
   must a model also be signature-verified before it runs?
6. **Are GPL-3.0 weights (RVM) acceptable in the shipped model catalogue** at all, given the
   GPL-3.0-or-later product licence, or must the catalogue be permissive-only?
7. **Confirm the GPL-3.0-or-later consequences** in `docs/architecture.md` §16 and
   `docs/decisions/packaging.md` are the register of record, since the model-licence question above
   depends on it.

## 9. Evidence index

| Fact | Location |
|---|---|
| Async contract, generation/analysis rules, no models selected | `docs/architecture.md` §12 (`:236-246`), R10 (`:43`) |
| Job contract implementation | `src/jobs/Job.{h,cpp}`, `src/jobs/Scheduler.{h,cpp}`; `JobKind` at `Job.h:40-57` |
| Contract tests: states, cancel wins, clamp, concurrency, no mutation | `tests/jobs/JobTests.cpp:66-293` |
| Scheduler is caller-pumped, injectable worker, bounded concurrency | `Scheduler.h:42`; `JobTests.cpp:4-6,193-214` |
| The reference editor's vision provider shapes and LOC; ONNX Runtime not selected | `docs/history/rewrite.md:60`; the reference vision crate's `Cargo.toml:24-33` |
| The reference editor's speech provider shapes and LOC | `docs/history/rewrite.md:64`; the reference speech crate's `Cargo.toml:29-77` |
| Model licence per model (RVM GPL-3.0; IS-Net/SlimSAM Apache-2.0; Real-ESRGAN BSD-3-Clause) | the reference vision crate's `src/models.rs:58-104` |
| Host-downloaded model shape: digest mandatory, mirror-first, pinned commit, partial+rename | the reference host crate's `src/models.rs:12-23,39-56,185-195` |
| Pack grants only `execute`; `User` origin refused; no FS/network | `src/extensions/Trust.cpp:8-30`; `Trust.h:44-54` |
| Pack is declarative + shader; executable Extension is the R8 deliverable | `docs/architecture.md` §8-§9 (`:205-220`); `docs/history/rewrite.md` §3(b) |
| Install store: atomic, staging, side-by-side, refuses on failure | `src/extensions/Install.h:16-36` |
| Mobile AI split: desktop worker; mobile in-process or cloud | `docs/architecture.md` §17 (`:294`); `docs/history/rewrite.md` §4 Phase 6/8 |
| Provider open question (which C++ providers; is speech in first target) | `docs/history/rewrite.md` §7 Q6 (`:566-567`) |
| Phase 6 scope and stop/fallback (mock provider) | `docs/history/implementation.md:160-179`; `docs/history/rewrite.md:428-444` |
| Licensing register | `docs/decisions/licensing.md` |

See `docs/decisions/effect-execution.md` for the Pack execution path this record distinguishes a
provider from, `docs/architecture.md` §12 for the contract's requirements, and
`docs/history/rewrite.md` §7 Q6 for the provider question this record proposes an answer to.

## 10. Implementation reconciliation (2026-10-02)

Status is **implemented where noted**: the recommendation in §4 was followed — host-downloaded local
models behind the injectable worker seam — and the providers below are built and tested. The
runtime is gated per-runtime (see `docs/dependencies.md` §"Gated AI runtimes"), so the default build
links none of it and every provider reports `disabled` when its runtime is off. No model weights
ship; each is downloaded after consent and digest-verified.

### 10.1 What is implemented

- **Host-owned model store (§4.1–4.3).** `src/ai/ModelStore.{h,cpp}` implements the §3(c) shape:
  `install()` refuses an entry with no/malformed SHA-256 before creating a file, reuses a verified
  copy, streams bytes into a `.tmp-*` partial beside the destination while hashing, and on digest
  match atomically renames it and writes the licence/digest `.meta` sidecar; on mismatch or an
  interrupted fetch it deletes the partial and fails. `installed()` re-verifies the digest. The
  pinned catalogue is `src/ai/ModelManifest.cpp` (`kCatalogue`, eight entries: whisper, rvm, isnet,
  slimsam-encoder, slimsam-decoder, real-esrgan, kokoro, pocket-tts; SHA-256, licence and size per
  row).
  Consent lives in the controller: `CutoutController` checks `modelPresent()`, raises the `Consent`
  state, and only calls `ai::install` after `confirmDownload()` (`CutoutController.cpp:186-249`).
- **Captions — whisper.cpp (gated, `GENESIS_AI_CAPTIONS`).** `src/ai/captions/WhisperProvider.{h,cpp}`;
  whisper.cpp v1.9.4 is fetched static (MIT). Gating is tested by `tests/ai/WhisperGatingTests.cpp`
  and the provider by `WhisperProviderTests.cpp`.
- **Segmentation / cutout — ONNX Runtime (gated, `GENESIS_AI_VISION`).** `SegmentationProvider`
  runs RVM (person) and IS-Net (object). `MaskDriver` + `MaskStore` turn frames into per-frame alpha
  masks on disk with keying/reuse (`mask_target`/`mask_revision`, `MaskStore.cpp:256-...`); the GES
  mask effect is `src/adapters/engine/ges/effects/MaskEffect.{h,cpp}`. `CutoutController` wires
  decode → segment → store → `SetClipCutout` (`CutoutController.cpp:400-552`).
- **Enhance — Real-ESRGAN.** `src/ai/vision/EnhanceProvider.{h,cpp}`, driven by `EnhanceController`.
- **TTS — sherpa-onnx (gated, `GENESIS_AI_TTS`).** `src/ai/tts/SherpaTtsProvider.{h,cpp}`; the
  pinned commit is built static with a local GCC-16 patch (`cmake/patches/sherpa-onnx-supertonic-u8.patch`,
  `docs/dependencies.md`). Gated by `tests/ai/SherpaTtsGatingTests.cpp`. Two pinned models are
  catalogued: **Kokoro** (`kokoro`, Apache-2.0) does multi-speaker synthesis by **speaker
  selection** from `voices.bin`, and **PocketTTS** (`pocket-tts`, CC-BY-4.0, Kyutai's checkpoint)
  does **zero-shot voice cloning** from a reference embedding through its
  lm_flow/lm_main/encoder/decoder ONNX graphs.
- **Brush — SlimSAM provider proven.** `src/ai/vision/BrushProvider.{h,cpp}` loads the pinned
  Xenova/slimsam-77-uniform export (catalogue ids `slimsam-encoder`/`slimsam-decoder`) and refines a
  base alpha per smart stroke. Strokes are captured on the edit path:
  `EditController::addCutoutStroke` (`EditController.cpp:364-389`) applies an `AddCutoutStroke`
  command, and `CutoutStrokeOverlay.qml` (instantiated in `Main.qml:520`) paints them.

### 10.2 Brush contract decision (pinned)

The brush code, not a runtime guess, fixes the contract: the catalogue pins the SlimSAM
**encoder + decoder as two separate models** (a single combined graph is not assumed); every frame
is resized long-side to a **fixed 1024** and zero-padded square (`brush_model_size`,
`BrushProvider.h:20`) because the patch embedding is a 16×16-stride-16 convolution; and the graph
uses **named tensors** — encoder `pixel_values` → `image_embeddings` /
`image_positional_embeddings`, decoder `input_points`/`input_labels` + both embeddings →
`iou_scores`/`pred_masks` (logits, low-res), blended by `brush_combine` (union for keep, subtract
for drop). This is the contract to preserve if the model pin moves.

### 10.3 Brush path (wired)

The pieces are joined end to end. When a regeneration runs, `CutoutController` reads the custom
cutout's strokes and revision (`vision::smart_strokes` / `vision::mask_revision`,
`CutoutController.cpp:442-443`) and, when there are smart strokes and
`vision::BrushProvider::available()`, installs the SlimSAM encoder/decoder and builds a
`BrushRefineFn` that reaches `BrushProvider::refine` on the worker
(`CutoutController.cpp:645-663`). `MaskDriver::generate_masks` accepts the same seams
(`MaskDriver.h:150-155`), and `BrushProvider::refine` is proven by
`tests/ai/BrushProviderTests.cpp`. An injected `brush_refine` fake (tests) wins over the provider;
a refine failure is non-fatal and keeps the base alpha. The edit-side capture
(`EditController::addCutoutStroke`) and the provider feed the same run, gated by
`GENESIS_AI_VISION`.

### 10.4 Chatterbox Turbo — parked (2026-10-03)

The third offline-TTS candidate, **Chatterbox Turbo** (Resemble AI, 350M, MIT code + model), was
evaluated as a `ChatterboxProvider` and **parked** — not because of licence or availability, but
because its runtime is the wrong shape for this repo. The recommendation is to keep the two pinned
TTS engines (§10.1) and treat Chatterbox as parked until one of the unblock conditions below holds.

**What was found.** A community ONNX export exists and is MIT-licensed:
`ResembleAI/chatterbox-turbo-ONNX` (the turbo variant of `onnx-community/chatterbox-ONNX`). It is a
**four-graph autoregressive + flow-matching pipeline**, not a single deterministic forward pass:

| Graph | Role | fp32 `.onnx` | fp32 `.onnx_data` (SHA-256) |
|---|---|---|---|
| `speech_encoder` | voice embedding of the reference clip | 1,172,072 B | 1,044,712,832 B (`c9915ff6…`) |
| `embed_tokens` | text-token embedding | 2,058 B | 232,812,544 B (`a1c37edc…`) |
| `language_model` | autoregressive LLM → speech tokens | 207,266 B | 1,269,724,812 B (`67db1068…`) |
| `conditional_decoder` | speech tokens → 24 kHz audio | 1,889,468 B | 768,593,792 B (`05f162a5…`) |

Total fp32 footprint ≈ 3.3 GB (`_fp16`/`_q4`/`_quantized` variants exist for each graph). A
GPT-2 byte-level BPE tokenizer (`tokenizer.json`, 3.56 MB, 50,276 vocab) and a
`generation_config.json` sit beside the graphs. `S3GEN_SR = 24000`; the LLM generation loop runs
up to 256 steps with a 30-layer KV-cache and repetition-penalty sampling.

**Why parked.** The runtime is a poor fit for the existing provider pattern, and the gap is large:

1. **Autoregressive, not a forward pass.** Every shipped provider (whisper, RVM, IS-Net,
   SlimSAM, Real-ESRGAN, Kokoro, PocketTTS) is a single or fixed-two-step graph. Chatterbox
   requires a token-by-token generation loop feeding a dynamic KV-cache — a wholly new runtime
   shape, with new cancellation/progress semantics to design and test.
2. **Voice cloning needs the `speech_encoder` plus mel-spectrogram/resampling preprocessing.**
   The only community C++ port (`DDATT/Chatterbox-turbo-cpp`, "not exhaustively tested") explicitly
   avoids this by pre-computing speaker embeddings in Python and shipping them as `.bin` files. A
   host-owned provider cannot clone a reference clip at runtime without reimplementing that encoder
   path; without it the engine collapses to a fixed builtin voice and adds nothing over Kokoro.
3. **The tokenizer is a full byte-level BPE.** No C++ BPE runtime is in-tree; hand-rolling one over
   a 50k vocab is ~500 LOC of easy-to-get-subtly-wrong code.
4. **No published digest.** The model store refuses a download with no SHA-256, and the only digests
   are the HF LFS oids above — the pin would be verification-pending until ~3.3 GB is fetched and
   re-hashed in-session.
5. **Redundant capability.** PocketTTS already provides zero-shot voice cloning and Kokoro provides
   speaker selection, at a fraction of the weight and with a single-graph runtime. Chatterbox's
   unique value — `[laugh]`/`[sigh]` paralinguistic tags, English-only expressiveness — does not
   justify a new runtime, tokenizer, voice encoder and a 3.3 GB pin.

**Unblock conditions** (any one makes the park reversible):

1. A single-forward-pass distillation or a `sherpa-onnx`-style unified graph for Turbo is published,
   removing the autoregressive loop.
2. The `speech_encoder` path is provided as a precomputed fixed-voice embedding (no runtime
   mel/resample), and a pinned digest is obtained by fetching and re-hashing the weights.
3. A maintained C++ runtime (not a 10-star, "not exhaustively tested" port) ships the tokenizer and
   pipeline, and its licence clears the no-dependency rule.

### 10.5 Execution-provider matrix (2026-10-04)

The ONNX Runtime vision providers run behind `src/ai/vision/ExecutionProvider.{h,cpp}`, which
enumerates the execution providers the loaded ORT reports via `Ort::Env::GetEpDevices` and picks one
in a fixed, platform-appropriate priority order. CPU is always available and is the terminal
fallback: the chosen accelerator's append or session load throwing retries a fresh CPU-only session
rather than failing the load, and the status indicator records the fallback as
`<provider> (fallback CPU)`.

| Priority | Provider | Canonical name | ORT device name | Where it wins |
|---|---|---|---|---|
| 1 | TensorRT | `TensorRT` | `TensorrtExecutionProvider` | NVIDIA, preferred over CUDA when both are present |
| 2 | CUDA | `CUDA` | `CUDAExecutionProvider` | NVIDIA without TensorRT |
| 3 | ROCm | `ROCm` | `ROCMExecutionProvider` | AMD |
| 4 | CoreML | `CoreML` | `CoreMLExecutionProvider` | Apple Silicon (macOS) |
| 5 | DirectML | `DirectML` | `DmlExecutionProvider` | Windows fallback (no NVIDIA/AMD path) |
| 6 | OpenVINO | `OpenVINO` | `OpenVINOExecutionProvider` | Intel, last (generic fallback) |
| 7 | CPU | `CPU` | `CPUExecutionProvider` | always available |

The platform split is implicit, not a compile-time `#if` on the target OS: `GetEpDevices` only
reports providers that are compiled into the loaded ORT **and** actually present, so CoreML cannot
appear on Windows and DirectML cannot appear on macOS/Linux. The fixed order therefore lands on the
right provider per platform (TensorRT > CUDA on NVIDIA; ROCm on AMD; CoreML alone on Apple Silicon;
DirectML then OpenVINO on Windows GPU-less/Intel).

Provider options are minimal — the default-constructed option struct per provider (CUDA/TensorRT V2,
ROCm, OpenVINO V2 with an empty option map), and CoreML/DirectML through ORT's generic
name-resolved `AppendExecutionProvider`. The shared session options are unchanged (intra-op threads
auto, full graph optimization).

**Untested-platform caveat.** Only CPU and CUDA are verifiable on this host (Linux, ORT v1.29.0):
TensorRT, ROCm, CoreML, DirectML and OpenVINO are exercised only through the hermetic
selection/priority tests over fake device-name strings (`tests/ai/ExecutionProviderTests.cpp`), never
through a live append or session on real hardware. DirectML in particular has no dedicated append in
this ORT build's C++ API and resolves by name through the generic append, which only succeeds on
Windows builds that ship the DML provider. The append paths are structured so any of these failing
on real hardware degrades to the same CPU fallback, but their real-GPU behaviour is unverified.

