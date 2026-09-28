// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <string>
#include <vector>

#include "ai/ModelManifest.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

const genesis::ai::ModelEntry *find(const std::string &id)
{
    for (const genesis::ai::ModelEntry &row : genesis::ai::model_catalogue()) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

// A pinned resolve URL names Hugging Face and a fixed revision: the
// "resolve/<40-hex>" segment is what makes the download immutable. A drift to a
// moving branch (or a missing revision) fails here.
bool is_pinned_resolve(const std::string &url)
{
    const std::string marker = "huggingface.co/";
    const std::string resolve = "/resolve/";
    const std::size_t resolve_at = url.find(resolve);
    if (url.find(marker) == std::string::npos || resolve_at == std::string::npos) {
        return false;
    }
    const std::size_t rev_begin = resolve_at + resolve.size();
    if (rev_begin + 40 > url.size()) {
        return false;
    }
    for (std::size_t i = rev_begin; i < rev_begin + 40; ++i) {
        const char c = url[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return url[rev_begin + 40] == '/';
}

// The pinned vision rows are the download contract the store and provider rely
// on: a fixed revision, the exact SHA-256 the store verifies, the byte size it
// preallocates, and the truthful weights licence. Any drift (re-pin to a new
// revision, a digest typo, a licence relabel) fails the build.
void test_vision_rows_pinned()
{
    const genesis::ai::ModelEntry *rvm = find("rvm");
    check(rvm != nullptr, "the catalogue names rvm");
    if (rvm != nullptr) {
        check(is_pinned_resolve(rvm->url), "RVM url pins a Hugging Face revision");
        check(rvm->mirror.empty(), "RVM has no mirror");
        check(rvm->sha256
                      == "88d4531297118f595bf2fd60f6f566aec2e559393802d1f43"
                         "6c380f0cbbd2828",
              "RVM digest matches the pinned weights");
        check(rvm->size == 14975696, "RVM size matches the pinned weights");
        check(rvm->license == "GPL-3.0", "RVM weights are recorded GPL-3.0");
        check(rvm->version == "1.0", "RVM version is 1.0");
    }

    const genesis::ai::ModelEntry *isnet = find("isnet");
    check(isnet != nullptr, "the catalogue names isnet");
    if (isnet != nullptr) {
        check(is_pinned_resolve(isnet->url), "IS-Net url pins a Hugging Face revision");
        check(isnet->mirror.empty(), "IS-Net has no mirror");
        check(isnet->sha256
                      == "cc2c9f5c1751b9737cb81e708ff0c5e9542c2205daed224"
                         "18a4fd2ab5d4c481a",
              "IS-Net digest matches the pinned weights");
        check(isnet->size == 176149806, "IS-Net size matches the pinned weights");
        check(isnet->license == "Apache-2.0", "IS-Net weights are recorded Apache-2.0");
        check(isnet->version == "1.0", "IS-Net version is 1.0");
    }

    const genesis::ai::ModelEntry *esrgan = find("real-esrgan");
    check(esrgan != nullptr, "the catalogue names real-esrgan");
    if (esrgan != nullptr) {
        check(is_pinned_resolve(esrgan->url), "Real-ESRGAN url pins a Hugging Face revision");
        check(esrgan->mirror.empty(), "Real-ESRGAN has no mirror");
        check(esrgan->sha256
                      == "1940a93ee08283a0a7286183186357b1688fe9fa8ede74"
                         "604b424586aaddf112",
              "Real-ESRGAN digest matches the pinned weights");
        check(esrgan->size == 4866417, "Real-ESRGAN size matches the pinned weights");
        check(esrgan->license == "BSD-3-Clause", "Real-ESRGAN weights are recorded BSD-3-Clause");
        check(esrgan->version == "1.0", "Real-ESRGAN version is 1.0");
    }
}

// A pinned GitHub release URL names the sherpa-onnx `tts-models` release tag:
// release assets are immutable, so the download cannot drift even though there
// is no 40-hex revision segment (unlike a Hugging Face resolve URL).
bool is_pinned_release(const std::string &url)
{
    return url.find("github.com/k2-fsa/sherpa-onnx/releases/download/"
                    "tts-models/")
            != std::string::npos;
}

// The pinned Kokoro row is the download contract the TTS provider relies on: a
// fixed release asset, the exact SHA-256 the store verifies, the byte size it
// preallocates, and the truthful Apache-2.0 weights licence.
void test_kokoro_row_pinned()
{
    const genesis::ai::ModelEntry *kokoro = find("kokoro");
    check(kokoro != nullptr, "the catalogue names kokoro");
    if (kokoro != nullptr) {
        check(is_pinned_release(kokoro->url), "Kokoro url pins the sherpa-onnx tts-models release");
        check(kokoro->mirror.empty(), "Kokoro has no mirror");
        check(kokoro->sha256
                      == "4c3052abaa60943a341f193888cf6abd68787dae6ab8ae"
                         "5c925a706caa247e4e",
              "Kokoro digest matches the pinned weights");
        check(kokoro->size == 132303094, "Kokoro size matches the pinned weights");
        check(kokoro->license == "Apache-2.0", "Kokoro weights are recorded Apache-2.0");
        check(kokoro->version == "int8-multi-lang-v1_0",
              "Kokoro version names the multi-lang int8 variant");
    }
}

// The pinned PocketTTS row is the download contract the voice-cloning path
// relies on: a fixed release asset, the exact SHA-256 the store verifies, the
// byte size it preallocates, and the truthful CC-BY-4.0 weights licence.
void test_pocket_tts_row_pinned()
{
    const genesis::ai::ModelEntry *pocket = find("pocket-tts");
    check(pocket != nullptr, "the catalogue names pocket-tts");
    if (pocket != nullptr) {
        check(is_pinned_release(pocket->url),
              "PocketTTS url pins the sherpa-onnx tts-models release");
        check(pocket->mirror.empty(), "PocketTTS has no mirror");
        check(pocket->sha256
                      == "2f3b88823cbbb9bf0b2477ec8ae7b3fec417b3a87b6bb5"
                         "f256dba66f2ad967cb",
              "PocketTTS digest matches the pinned weights");
        check(pocket->size == 98336520, "PocketTTS size matches the pinned weights");
        check(pocket->license == "CC-BY-4.0", "PocketTTS weights are recorded CC-BY-4.0");
        check(pocket->version == "int8-2026-01-26", "PocketTTS version names the int8 variant");
    }
}

// The SlimSAM encoder/decoder pair is the Stage 4 smart-brush download
// contract: two pinned rows, each a fixed revision, the exact SHA-256 the store
// verifies, the byte size it preallocates, and the truthful Apache-2.0 weights
// licence. Both files come from one immutable Hugging Face revision, so the two
// digests are what keep the pair in step with the brush provider.
void test_slimsam_rows_pinned()
{
    const genesis::ai::ModelEntry *encoder = find("slimsam-encoder");
    check(encoder != nullptr, "the catalogue names slimsam-encoder");
    if (encoder != nullptr) {
        check(is_pinned_resolve(encoder->url), "the encoder url pins a Hugging Face revision");
        check(encoder->mirror.empty(), "the encoder has no mirror");
        check(encoder->sha256
                      == "9f8433273a6750b587779baa0cf5508111001bf7e7acf"
                         "cf585d370139fd366d0",
              "the encoder digest matches the pinned weights");
        check(encoder->size == 23276014, "the encoder size matches the pinned weights");
        check(encoder->license == "Apache-2.0", "the encoder weights are recorded Apache-2.0");
        check(encoder->version == "1.0", "the encoder version is 1.0");
    }

    const genesis::ai::ModelEntry *decoder = find("slimsam-decoder");
    check(decoder != nullptr, "the catalogue names slimsam-decoder");
    if (decoder != nullptr) {
        check(is_pinned_resolve(decoder->url), "the decoder url pins a Hugging Face revision");
        check(decoder->mirror.empty(), "the decoder has no mirror");
        check(decoder->sha256
                      == "f4514391764fbd56e08e119060d874ecd7d52994bfb19"
                         "68af159e12d4943b5bb",
              "the decoder digest matches the pinned weights");
        check(decoder->size == 16557892, "the decoder size matches the pinned weights");
        check(decoder->license == "Apache-2.0", "the decoder weights are recorded Apache-2.0");
        check(decoder->version == "1.0", "the decoder version is 1.0");
    }
}

// The whisper row stays pinned so a vision change never regresses Stage 1a, and
// no unpinned candidate survives (the catalogue is all-pinned now that SlimSAM
// landed).
void test_whisper_row_stable()
{
    const genesis::ai::ModelEntry *whisper = find("whisper");
    check(whisper != nullptr, "the catalogue names whisper");
    if (whisper != nullptr) {
        check(is_pinned_resolve(whisper->url), "whisper url stays a pinned revision");
        check(!whisper->sha256.empty(), "whisper digest stays set");
    }
}

} // namespace

int main()
{
    test_vision_rows_pinned();
    test_slimsam_rows_pinned();
    test_kokoro_row_pinned();
    test_pocket_tts_row_pinned();
    test_whisper_row_stable();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
