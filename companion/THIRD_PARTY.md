# Third-party components

| Component | Version | Licence | How it is used |
|---|---|---|---|
| sherpa-onnx (k2-fsa) Kotlin API | v2.0.1 | Apache-2.0 | `app/src/main/java/com/k2fsa/sherpa/onnx/` (5 files copied unchanged from `sherpa-onnx/kotlin-api/` at tag v2.0.1: OfflineRecognizer, OfflineStream, FeatureConfig, HomophoneReplacerConfig, QnnConfig) |
| sherpa-onnx native libs | v2.0.1 | Apache-2.0 | `app/src/main/jniLibs/arm64-v8a/libsherpa-onnx-jni.so` and `libonnxruntime.so`, taken from `sherpa-onnx-v2.0.1-android.tar.bz2` (GitHub release). arm64 only, which is what a Pixel 8 needs. Rebuild/refresh by re-extracting the same archive |
| ONNX Runtime (inside libonnxruntime.so) | as bundled by sherpa-onnx v2.0.1 | MIT | speech model inference |
| Whisper base, multilingual (sherpa-onnx), optional | `sherpa-onnx-whisper-base.tar.bz2`, 207,557,382 bytes, SHA-256 911b2083efd7c0dca2ac3b358b75222660dc09fb716d64fbfc417ba6c99ff3de | MIT | optional bigger speech model, same download path |
| Whisper tiny, multilingual (sherpa-onnx int8 export) | `sherpa-onnx-whisper-tiny.tar.bz2` from the `asr-models` release of k2-fsa/sherpa-onnx | MIT (OpenAI Whisper) | Not bundled. Downloaded through Ember Gateway when the user taps Download, SHA-256 c46116994e539aa165266d96b325252728429c12535eb9d8b6a2b10f129e66b1 checked (116 MB; the app keeps only the int8 encoder/decoder and tokens, about 104 MB) |
| Apache Commons Compress | 1.28.0 | Apache-2.0 | unpacks the downloaded `.tar.bz2` model on the phone |
| LiteRT-LM Kotlin (`com.google.ai.edge.litertlm:litertlm-android`) | 0.19.1 | Apache-2.0 | on-device LLM runtime (translation, summaries, bench). Needs Kotlin 2.4, so the project now builds with Kotlin 2.4.0 |
| Gemma 4 E2B, LiteRT-LM format (`litert-community/gemma-4-E2B-it-litert-lm`, file `gemma-4-E2B-it.litertlm`, 2,588,147,712 bytes, SHA-256 181938105e0eefd105961417e8da75903eacda102c4fce9ce90f50b97139a63c) | HF revision 2d74ac41 (2026-11-11) | Apache-2.0, not gated (checked on the Hugging Face API: `gated: false`, tag `license:apache-2.0`) | Not bundled. Downloaded once through Ember Gateway after you approve the exact URL |
| AndroidX Health Connect client | 1.1.0 | Apache-2.0 | writes steps |
| Jetpack Compose (BOM 2025.06.01), Navigation, Activity, Core KTX | see `gradle/libs.versions.toml` | Apache-2.0 | UI |
| kotlinx.serialization / kotlinx.coroutines | 1.9.0 / 1.10.2 | Apache-2.0 | JSON and async |
| Robolectric, JUnit | 4.17 / 4.13.2 | MIT / EPL-1.0 | tests only |

No LLM model is bundled or auto-downloaded. Gated models (for example Gemma) must be obtained by the user and imported with the file picker.

Bench test clip `app/src/main/assets/bench_clip_10s.wav`: the first 10 s of `test_wavs/1.wav` from the sherpa-onnx whisper-tiny archive (LibriSpeech-style English, from the sherpa-onnx test data).

The Gateway app (`gateway/`) and `shared/` use only AndroidX, Compose and kotlinx libraries listed above.
