# gl-custom-element spike

Gate: can a **custom** GStreamer element bind N named intermediates (N input
textures into one fragment shader), where stock `glshader` cannot?

`glshader` is a `GstGLFilter` (single input, one texture). A second
`sampler2D` silently aliases texture unit 0, and a later pass cannot read an
earlier pass's output (settled in `spikes/gl-multipass/`). The framework's
multi-input GL base class is `GstGLMixer` (a `GstVideoAggregator`), whose
request sink pads each expose a `current_texture`. This spike subclasses
`GstGLMixer` as `gltexmix` and samples every pad's texture in one shader, each
bound to a distinct texture unit.

## Files

- `gltexmix.c` — the `gltexmix` element (`GstGLMixer` subclass). Overrides
  `process_textures` to run GL work on the GL thread, iterating sink pads and
  binding `GST_GL_MIXER_PAD(pad)->current_texture` to `GL_TEXTURE0+i` with
  `glUniform1i(tex{i}, i)`, then drawing a fullscreen quad.
- `main.c` — headless EGL harness (NEED_CONTEXT answer, appsink residency
  probe, PNG capture branch). Registers `gltexmix`, builds the pipelines.
- `shaders.h` — GLSL bodies: single-input RED/GREEN/INVERT (glshader) and
  two-input YELLOW/TEX1/AVG (gltexmix).
- `run.sh` — runs every case in probe + capture mode.
- `verify.py` — pixel verdicts: "no error" is not success; output equal to one
  input is a failure to combine.

## Cases

| case       | tex0      | tex1   | shader                      | correct output |
|------------|-----------|--------|-----------------------------|----------------|
| `combine`  | RED       | GREEN  | `vec4(tex0.r, tex1.g, 0,1)` | YELLOW         |
| `tex1-only`| RED       | GREEN  | `texture2D(tex1, uv)`       | GREEN          |
| `named`    | invert(src)| src   | `0.5*tex0 + 0.5*tex1`       | const ~128 gray|

`tex1-only` is the anti-aliasing proof: it samples only the second unit, so
GREEN proves `tex1` is independently bound and not aliased to `tex0`.
`named` is the decisive proof of combining two named intermediates: the
average of a frame and its inverse is constant regardless of content.

## Build and run

```sh
cmake -S . -B build
cmake --build build
./run.sh        # writes PNGs + logs to /tmp/opencode/gl-custom-element
python3 verify.py
```

Requires GStreamer 1.28 with `gst-plugins-bad` and GL/EGL headers
(`gstreamer-gl-1.0`, `gstreamer-gl-egl-1.0`).
