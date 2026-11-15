# Third-party components

| Component | Version | Licence | How it is used |
|---|---|---|---|
| sherpa-onnx (k2-fsa) Kotlin API | v2.0.1 | Apache-2.0 | `app/src/main/java/com/k2fsa/sherpa/onnx/` (5 files copied unchanged from `sherpa-onnx/kotlin-api/` at tag v2.0.1: OfflineRecognizer, OfflineStream, FeatureConfig, HomophoneReplacerConfig, QnnConfig) |
| sherpa-onnx native libs | v2.0.1 | Apache-2.0 | `app/src/main/jniLibs/arm64-v8a/libsherpa-onnx-jni.so` and `libonnxruntime.so`, taken from `sherpa-onnx-v2.0.1-android.tar.bz2` (GitHub release). arm64 only, which is what a Pixel 8 needs. Rebuild/refresh by re-extracting the same archive |
| ONNX Runtime (inside libonnxruntime.so) | as bundled by sherpa-onnx v2.0.1 | MIT | speech model inference |
| Whisper tiny, multilingual (sherpa-onnx int8 export) | `sherpa-onnx-whisper-tiny.tar.bz2` from the `asr-models` release of k2-fsa/sherpa-onnx | MIT (OpenAI Whisper) | Not bundled. Downloaded once when the user taps Download (116 MB; the app keeps only the int8 encoder/decoder and tokens, about 104 MB) |
| Apache Commons Compress | 1.28.0 | Apache-2.0 | unpacks the downloaded `.tar.bz2` model on the phone |
| ML Kit Translate (`com.google.mlkit:translate`) | 17.0.3 | Google ML Kit terms | on-device translation; language packs are downloaded by ML Kit when the user taps Download |
| MediaPipe Tasks GenAI (`com.google.mediapipe:tasks-genai`) | 0.10.35 | Apache-2.0 | optional LLM summary, only with a model file the user imports |
| AndroidX Health Connect client | 1.1.0 | Apache-2.0 | writes steps |
| Jetpack Compose (BOM 2025.06.01), Navigation, Activity, Core KTX | see `gradle/libs.versions.toml` | Apache-2.0 | UI |
| kotlinx.serialization / kotlinx.coroutines | 1.9.0 / 1.10.2 | Apache-2.0 | JSON and async |
| Robolectric, JUnit | 4.17 / 4.13.2 | MIT / EPL-1.0 | tests only |

No LLM model is bundled or auto-downloaded. Gated models (for example Gemma) must be obtained by the user and imported with the file picker.
