// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/ModelManifest.h"

#include <array>

namespace genesis::ai {

namespace {

// The candidate models the AI provider decision names, with the licence each
// one's weights carry (docs/decisions/ai-provider.md §5). A row whose provider
// is not selected yet keeps its provenance (url, digest, size) unset - the
// store refuses to download it. Whisper (Stage 1a), the two vision models
// (Stage 2a) and the two SlimSAM halves (Stage 4 smart brush) are pinned: a
// fixed Hugging Face revision makes the resolve URL immutable, and the SHA-256
// is the digest of the bytes the store checks every download against. None has
// a mirror - the whisper.cpp mirror is dead, and no maintained mirror exists
// for the vision weights.
//
//   whisper base.en     - transcription (whisper.cpp ggml weights), MIT  [pinned]
//   RVM                - person matting,   GPL-3.0                     [pinned]
//   IS-Net             - object cutout,    Apache-2.0                  [pinned]
//   SlimSAM encoder    - brush encode,     Apache-2.0                  [pinned]
//   SlimSAM decoder    - brush decode,     Apache-2.0                  [pinned]
//   Real-ESRGAN        - enhancement,      BSD-3-Clause                [pinned]
//   Kokoro             - text-to-speech,   Apache-2.0                  [pinned]
//   PocketTTS          - voice cloning,    CC-BY-4.0                   [pinned]
//
// RVM's weights are GPL-3.0 (PeterL1n/RobustVideoMatting), recorded truthfully
// even though the re-host names a looser label: the runtime licence and the
// model licence are separate, and GPL-3.0 weights are compatible with this
// GPL-3.0-or-later project. IS-Net's weights are the Apache-2.0
// DIS `isnet-general-use` checkpoint (xuebinqin/DIS); IMG.LY hosts the ONNX
// conversion the provider runs. SlimSAM's weights are the Apache-2.0
// `slimsam-77-uniform` encoder/decoder pair (Xenova hosts the ONNX conversion
// the brush provider runs); the two files travel as two store entries because
// each is a distinct immutable blob the store verifies and the provider loads
// separately. Real-ESRGAN's weights are the BSD-3-Clause
// `realesr-general-x4v3` checkpoint, hosted as the ONNX conversion the enhance
// provider runs (CoderViking/realesr-general-x4v3-onnx). Kokoro's weights are
// the Apache-2.0 `kokoro-int8-multi-lang-v1_0` package sherpa-onnx ships from
// its immutable `tts-models` release tag: one tar.bz2 the store verifies, which
// the TTS provider unpacks into its model.onnx/voices.bin/tokens.txt files.
// PocketTTS's weights are Kyutai's CC-BY-4.0 `pocket-tts` checkpoint, hosted as
// the ONNX conversion sherpa-onnx ships from the same immutable release tag:
// one tar.bz2 the store verifies, which the TTS provider unpacks into its
// lm_flow/lm_main/encoder/decoder/text_conditioner ONNX files plus vocab.json
// and token_scores.json for zero-shot voice cloning.
const std::array<ModelEntry, 8> kCatalogue = { {
        { "whisper", "base.en",
          "https://huggingface.co/ggerganov/whisper.cpp/resolve/"
          "5359861c739e955e79d9a303bcbc70fb988958b1/ggml-base.en.bin",
          "", "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002", "MIT",
          147964211 },
        { "rvm", "1.0",
          "https://huggingface.co/LPDoctor/video_matting/resolve/"
          "9ef0bf424ce229291c43a622d75af667dd1bd9bd/rvm_mobilenetv3_fp32.onnx",
          "", "88d4531297118f595bf2fd60f6f566aec2e559393802d1f436c380f0cbbd2828", "GPL-3.0",
          14975696 },
        { "isnet", "1.0",
          "https://huggingface.co/imgly/isnet-general-onnx/resolve/"
          "440dea96dd4a3b06bbbf5abec3e26569dd7ec49f/onnx/model.onnx",
          "", "cc2c9f5c1751b9737cb81e708ff0c5e9542c2205daed22418a4fd2ab5d4c481a", "Apache-2.0",
          176149806 },
        { "slimsam-encoder", "1.0",
          "https://huggingface.co/Xenova/slimsam-77-uniform/resolve/"
          "5850ab45f587c112167512ffef949107115e26a0/onnx/vision_encoder.onnx",
          "", "9f8433273a6750b587779baa0cf5508111001bf7e7acfcf585d370139fd366d0", "Apache-2.0",
          23276014 },
        { "slimsam-decoder", "1.0",
          "https://huggingface.co/Xenova/slimsam-77-uniform/resolve/"
          "5850ab45f587c112167512ffef949107115e26a0/onnx/"
          "prompt_encoder_mask_decoder.onnx",
          "", "f4514391764fbd56e08e119060d874ecd7d52994bfb1968af159e12d4943b5bb", "Apache-2.0",
          16557892 },
        { "real-esrgan", "1.0",
          "https://huggingface.co/CoderViking/realesr-general-x4v3-onnx/resolve/"
          "c6a971706797c7502945a2b4c4274fce4900d4ab/realesr-general-x4v3.onnx",
          "", "1940a93ee08283a0a7286183186357b1688fe9fa8ede74604b424586aaddf112", "BSD-3-Clause",
          4866417 },
        { "kokoro", "int8-multi-lang-v1_0",
          "https://github.com/k2-fsa/sherpa-onnx/releases/download/"
          "tts-models/kokoro-int8-multi-lang-v1_0.tar.bz2",
          "", "4c3052abaa60943a341f193888cf6abd68787dae6ab8ae5c925a706caa247e4e", "Apache-2.0",
          132303094 },
        { "pocket-tts", "int8-2026-01-26",
          "https://github.com/k2-fsa/sherpa-onnx/releases/download/"
          "tts-models/sherpa-onnx-pocket-tts-int8-2026-01-26.tar.bz2",
          "", "2f3b88823cbbb9bf0b2477ec8ae7b3fec417b3a87b6bb5f256dba66f2ad967cb", "CC-BY-4.0",
          98336520 },
} };

} // namespace

std::span<const ModelEntry> model_catalogue()
{
    return kCatalogue;
}

} // namespace genesis::ai
