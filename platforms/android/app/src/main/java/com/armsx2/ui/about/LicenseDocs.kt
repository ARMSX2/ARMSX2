package com.armsx2.ui.about

import android.content.Context

/** One open-source component and its license, as the About screen lists them. [preformatted] is a
 *  license kept as laid out (a `<pre>`), shown in a fixed-width font. */
internal data class LicenseEntry(
    val id: String,
    val name: String,
    val url: String?,
    val text: String,
    val preformatted: Boolean,
)

/**
 * The texts behind the About screen's License and Open-source licenses cards.
 *
 * GPL.html and ThirdPartyLicenses.html are the files PCSX2's desktop About dialog opens. They are
 * copied from bin/docs into the APK at build time (generateLicenseDocs in app/build.gradle.kts),
 * so there is one copy to keep current. android.html lists what only the Android app ships, in
 * the same layout: one `<div id>` per component, its name and link in an `<h3>`, its license in
 * a `<pre>`.
 */
internal object LicenseDocs {
    private const val Dir = "licenses"

    fun gpl(context: Context): String = htmlToText(read(context, "GPL.html"))

    /** The Android app's own components first, then PCSX2's. */
    fun components(context: Context): Pair<List<LicenseEntry>, List<LicenseEntry>> =
        parse(read(context, "android.html")) to parse(read(context, "ThirdPartyLicenses.html"))

    private fun read(context: Context, name: String): String =
        context.assets.open("$Dir/$name").bufferedReader().use { it.readText() }

    private val Block = Regex("""<div id="([^"]+)">(.*?)</div>""", RegexOption.DOT_MATCHES_ALL)
    private val Heading = Regex("""<h3>(.*?)</h3>""", RegexOption.DOT_MATCHES_ALL)
    private val Link = Regex("""<a href="([^"]+)"""")
    private val Pre = Regex("""<pre>(.*?)</pre>""", RegexOption.DOT_MATCHES_ALL)
    private val Tag = Regex("""<[^>]+>""")
    private val Entity = Regex("""&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);""")
    private val Named = mapOf(
        "amp" to "&", "lt" to "<", "gt" to ">", "quot" to "\"", "apos" to "'",
        "copy" to "©", "reg" to "®", "nbsp" to " ",
    )

    /** Every component in a license file laid out like ThirdPartyLicenses.html. */
    fun parse(html: String): List<LicenseEntry> = Block.findAll(html).mapNotNull { block ->
        val (id, body) = block.destructured
        val heading = Heading.find(body)?.groupValues?.get(1) ?: return@mapNotNull null
        // "zlib - <a href=...>...</a>": the name is what comes before the link.
        val name = unescape(stripTags(heading.substringBefore("<a"))).trim().removeSuffix("-").trim()
        val url = Link.find(heading)?.groupValues?.get(1)
        // A license is a <pre>; PCSX2's own entry is paragraphs instead.
        val pre = Pre.find(body)
        val text = pre?.let { unescape(stripTags(it.groupValues[1])).trim('\n').trimEnd() }
            ?: htmlToText(body.replace(Heading, ""))
        LicenseEntry(id, name, url, text, preformatted = pre != null)
    }.toList()

    private const val Break = '\u0002'
    private const val PreMark = '\u0001'

    /** Readable plain text from an HTML page: paragraphs and list items on their own lines,
     *  `<pre>` blocks as they are. */
    fun htmlToText(html: String): String {
        var s = html.substringAfter("<body>", html).substringBefore("</body>")
        val pres = mutableListOf<String>()
        s = Pre.replace(s) { m ->
            pres += m.groupValues[1]
            "$Break$PreMark${pres.size - 1}$PreMark$Break"
        }
        s = s.replace(Regex("""<li[^>]*>"""), "$Break• ")
            .replace(Regex("""<br\s*/?>|</(p|h[1-6]|li|ul|ol|div)>"""), Break.toString())
            .replace(Tag, "")
            .replace(Regex("""\s+"""), " ")
            .replace(Regex(""" ?$Break[ $Break]*"""), "\n\n")
        s = unescape(s)
        s = Regex("""$PreMark(\d+)$PreMark""").replace(s) { m ->
            unescape(stripTags(pres[m.groupValues[1].toInt()])).trim('\n').trimEnd()
        }
        return s.trim()
    }

    private fun stripTags(s: String) = s.replace(Tag, "")

    private fun unescape(s: String): String = Entity.replace(s) { m ->
        val e = m.groupValues[1]
        when {
            e.startsWith("#x") -> e.substring(2).toIntOrNull(16)?.let { String(Character.toChars(it)) }
            e.startsWith("#") -> e.substring(1).toIntOrNull()?.let { String(Character.toChars(it)) }
            else -> Named[e]
        } ?: m.value
    }
}
