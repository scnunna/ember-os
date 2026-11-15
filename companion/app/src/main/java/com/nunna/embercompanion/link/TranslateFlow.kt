package com.nunna.embercompanion.link

import com.nunna.embercompanion.protocol.Message

/** What happened to one push-to-talk. [error] is a short code + plain sentence when something went wrong. */
data class TranslateResult(val side: String, val heard: String, val translated: String, val error: String?)

/**
 * The travel-translation pipeline: speech -> text -> translated text -> spoken + sent to the watch.
 * The three steps are passed in as functions so this can be tested without a phone.
 * Side "me": I spoke my language, the other person should hear/see their language. Side "them": the reverse.
 */
class TranslateFlow(
    private val transcribe: suspend (samples: FloatArray, lang: String) -> String,
    private val translate: suspend (text: String, from: String, to: String) -> String,
    private val speak: suspend (text: String, lang: String) -> Unit,
    private val send: (Message) -> Unit,
) {
    suspend fun run(side: String, samples16k: FloatArray, myLang: String, theirLang: String): TranslateResult {
        val from = if (side == "me") myLang else theirLang
        val to = if (side == "me") theirLang else myLang
        if (samples16k.size < 16000 / 5) return TranslateResult(side, "", "", "TR-01: That was too short. Hold the button and speak a little longer.")
        val heard = try { transcribe(samples16k, from).trim() } catch (e: Exception) {
            return TranslateResult(side, "", "", "TR-02: Could not turn the speech into text. ${e.message ?: "Check that the speech model is downloaded."}")
        }
        if (heard.isBlank()) return TranslateResult(side, "", "", "TR-03: I did not hear any words. Try again, a bit closer to the microphone.")
        val out = try { translate(heard, from, to).trim() } catch (e: Exception) {
            return TranslateResult(side, heard, "", "TR-04: Could not translate. Open Travel translate and download both language packs.")
        }
        send(Message.Tr(side, heard, out))
        val speakErr = try { speak(out, to); null } catch (e: Exception) {
            "TR-05: Translated, but could not speak it. ${e.message ?: "Install a voice for that language in Android settings."}"
        }
        return TranslateResult(side, heard, out, speakErr)
    }
}
