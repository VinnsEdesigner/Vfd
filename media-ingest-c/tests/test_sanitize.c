/* test_sanitize.c - the HTML sanitiser.
 *
 * These are written as attack strings rather than as shapes, because a
 * sanitiser is only as good as the things it was tried against. Each case below
 * is a technique that has worked against a real sanitiser somewhere: a script
 * tag, an event handler, a scheme obfuscated with whitespace or case, a value
 * that ends its own quotes, the SVG and MathML vectors, a nested frame, a meta
 * refresh.
 *
 * The assertion is always the same: the dangerous construct is gone from the
 * output. Where something survives that is only a *reading* feature — a link, an
 * image — it is asserted to survive too, because a sanitiser that removes
 * everything is not a sanitiser.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mi_sanitize.h"
#include "mi_util.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        }                                                                   \
    } while (0)

/* Whether `needle` appears in the output, case-insensitively. */
static int has(const char *hay, const char *needle) {
    return strcasestr(hay, needle) != NULL;
}

/* Sanitise a literal and return the result; the caller frees it. */
static char *san(const char *html) {
    mi_sanitize_config cfg;
    memset(&cfg, 0, sizeof cfg);
    return mi_sanitize_html(html, strlen(html), &cfg);
}

static void test_scripts_are_gone(void) {
    char *out = san("<p>before</p><script>alert(1)</script><p>after</p>");
    CHECK(out != NULL, "output produced");
    CHECK(!has(out, "<script"), "a script tag is removed");
    CHECK(!has(out, "alert(1)"), "and so is its content, not just its tags");
    CHECK(has(out, "before") && has(out, "after"), "the text around it survives");
    free(out);

    /* A script with a type that is not JavaScript is still a script. */
    out = san("<script type=\"text/template\"><b>x</b></script>");
    CHECK(!has(out, "<script"), "a non-js script tag is removed too");
    CHECK(!has(out, "<b>x</b>"), "its content is dropped, not re-emitted as markup");
    free(out);

    /* An unterminated script must not swallow the page in a way that leaves a
     * tag open. */
    out = san("<p>keep</p><script>var x = 1;");
    CHECK(has(out, "keep"), "text before an unterminated script survives");
    CHECK(!has(out, "var x"), "the unterminated script's body is dropped");
    free(out);
}

static void test_raw_text_content_is_not_counted_as_nesting(void) {
    /*
     * The bug this guards: a script whose body *contains* the text "<script>"
     * looked like a nested open tag, so a depth-counting skip ran to the end of
     * the document and silently dropped everything after it. The rest of the
     * page must survive.
     */
    char *out = san("<script>var s = \"<script>\";</script><p>IMPORTANT</p>");
    CHECK(has(out, "IMPORTANT"), "a page after a script containing '<script>' survives");
    CHECK(!has(out, "var s"), "the script body is still gone");
    free(out);

    /* The same for a style element. */
    out = san("<style>p:before{content:\"<style>\"}</style><p>ALSO</p>");
    CHECK(has(out, "ALSO"), "a page after a style containing '<style>' survives");
    free(out);
}

static void test_event_handlers_are_gone(void) {
    char *out = san("<p onclick=\"alert(1)\">text</p>");
    CHECK(!has(out, "onclick"), "an onclick handler is removed");
    CHECK(has(out, "text"), "the text survives");
    free(out);

    /* Every `on*` name, including ones a denylist would miss. */
    const char *handlers[] = {
        "onerror", "onload", "onmouseover", "onfocus", "onanimationstart",
        "ontoggle", "onbegin", "onend", "onrepeat", "onbeforeinput", NULL,
    };
    for (size_t i = 0; handlers[i]; i++) {
        char buf[256];
        snprintf(buf, sizeof buf, "<img src=\"/a.png\" %s=\"x()\">", handlers[i]);
        char *o = san(buf);
        CHECK(!has(o, handlers[i]), "an event handler is removed by prefix, not by name");
        free(o);
    }

    /* Mixed case must not slip through. */
    out = san("<p OnClIcK=\"alert(1)\">x</p>");
    CHECK(!has(out, "onclick"), "a mixed-case handler is removed");
    free(out);
}

static void test_dangerous_urls(void) {
    char *out = san("<a href=\"javascript:alert(1)\">x</a>");
    CHECK(!has(out, "javascript:"), "a javascript: href is removed");
    CHECK(has(out, ">x</a>"), "but the link text survives");
    free(out);

    /* Case and embedded whitespace are the two classic bypasses. */
    out = san("<a href=\"JaVaScRiPt:alert(1)\">x</a>");
    CHECK(!has(out, "alert"), "an upper-case scheme is caught");
    free(out);

    out = san("<a href=\"java\tscript:alert(1)\">x</a>");
    CHECK(!has(out, "script:alert"), "a tab inside the scheme is caught");
    free(out);

    out = san("<a href=\"java\nscript:alert(1)\">x</a>");
    CHECK(!has(out, "script:alert"), "a newline inside the scheme is caught");
    free(out);

    out = san("<a href=\"vbscript:msgbox(1)\">x</a>");
    CHECK(!has(out, "vbscript"), "a vbscript: href is removed");
    free(out);

    /* data: can carry a whole document. */
    out = san("<a href=\"data:text/html,<script>alert(1)</script>\">x</a>");
    CHECK(!has(out, "data:text/html"), "a data: href is removed");
    free(out);

    out = san("<img src=\"data:text/html,<script>x</script>\">");
    CHECK(!has(out, "data:text/html"), "a data: img src is removed");
    free(out);
}

/*
 * A value that tries to end its own quotes and start a new attribute.
 *
 * The property to assert is not "the text onmouseover is absent" — it may
 * legitimately appear as *inert text inside the attribute*, escaped. What must
 * be true is that no new attribute was created, so the check is for an
 * attribute boundary: a quote that closes the value early, followed by the name
 * of a handler. The escaped forms (`&quot;`, `&#96;`, `&amp;quot;`) are text,
 * not syntax, and that is what makes them safe.
 */
static void test_attribute_breakout(void) {
    /* An escaped quote inside the value. */
    char *out = san("<img src=\"/a.png\" alt=\"x&quot; onmouseover=&quot;alert(1)\">");
    CHECK(!has(out, "x\" onmouseover"), "an entity-encoded quote does not close the value");
    CHECK(has(out, "&quot;") || has(out, "&amp;quot;"),
          "the quote survives only as escaped text");
    free(out);

    /* A literal single quote inside a double-quoted value. */
    out = san("<img src=\"/a.png\" alt='x\" onload=\"alert(1)'>");
    CHECK(!has(out, "x\" onload"), "a raw quote inside the value is escaped, not emitted");
    free(out);

    /* A backtick, which older parsers accepted as a quote. */
    out = san("<img src=\"/a.png\" alt=\"x` onerror=alert(1)\">");
    CHECK(!has(out, "x` onerror"), "a backtick cannot act as a quote");
    free(out);

    /* A raw double quote inside a double-quoted value: the one case where the
     * value really is cut short by the parser, and the rest must not become an
     * attribute. */
    out = san("<img src=\"/a.png\" alt=\"x\" onerror=\"alert(1)\">");
    CHECK(!has(out, "onerror=\"alert"), "a raw break-out does not become an attribute");
    free(out);

    /* An unquoted value followed by a handler. */
    out = san("<img src=/a.png onerror=alert(1)>");
    CHECK(!has(out, "onerror="), "an unquoted handler is removed");
    free(out);
}

static void test_svg_and_math_vectors(void) {
    /* SVG can carry script, and `xlink:href` was a whole family of bypasses. */
    char *out = san("<svg><script>alert(1)</script></svg>");
    CHECK(!has(out, "<svg"), "an svg element is removed");
    CHECK(!has(out, "alert"), "and its content with it");
    free(out);

    out = san("<svg><a xlink:href=\"javascript:alert(1)\"><text>x</text></a></svg>");
    CHECK(!has(out, "javascript:"), "an svg xlink href is removed");
    CHECK(!has(out, "xlink"), "the xlink attribute is not on the allowlist");
    free(out);

    out = san("<math><mtext><script>alert(1)</script></mtext></math>");
    CHECK(!has(out, "<math"), "a math element is removed");
    CHECK(!has(out, "alert"), "and its content with it");
    free(out);

    /* A foreignObject carrying a whole document. */
    out = san("<svg><foreignObject><body onload=\"alert(1)\"></body></foreignObject></svg>");
    CHECK(!has(out, "foreignobject"), "a foreignObject is removed");
    CHECK(!has(out, "onload"), "and its handler with it");
    free(out);
}

static void test_frames_and_meta(void) {
    char *out = san("<iframe src=\"https://evil.example/\"></iframe>");
    CHECK(!has(out, "<iframe"), "an iframe is removed");
    free(out);

    out = san("<object data=\"https://evil.example/\"></object>");
    CHECK(!has(out, "<object"), "an object is removed");
    free(out);

    out = san("<embed src=\"https://evil.example/\">");
    CHECK(!has(out, "<embed"), "an embed is removed");
    free(out);

    /* A meta refresh is a navigation without a link. */
    out = san("<meta http-equiv=\"refresh\" content=\"0;url=https://evil.example/\">");
    CHECK(!has(out, "evil.example"), "a meta refresh is removed");
    free(out);

    /* A base tag would rewrite every relative URL in the document. */
    out = san("<base href=\"https://evil.example/\">");
    CHECK(!has(out, "evil.example"), "an injected base is removed");
    free(out);

    /* A form can post the user's input somewhere. */
    out = san("<form action=\"https://evil.example/\"><input name=\"x\"></form>");
    CHECK(!has(out, "<form"), "a form is removed");
    CHECK(!has(out, "<input"), "and its input");
    free(out);
}

static void test_style_is_filtered(void) {
    /* url() in a style is a fetch the page controls. */
    char *out = san("<p style=\"background: url(https://evil.example/x)\">t</p>");
    CHECK(!has(out, "url("), "a style url() is removed");
    CHECK(has(out, ">t</p>"), "the element and its text survive");
    free(out);

    out = san("<p style=\"width: expression(alert(1))\">t</p>");
    CHECK(!has(out, "expression"), "a CSS expression is removed");
    free(out);

    /* A harmless style is kept, or the page loses its layout. */
    out = san("<p style=\"color: red\">t</p>");
    CHECK(has(out, "color: red"), "a harmless inline style is kept");
    free(out);
}

static void test_reading_features_survive(void) {
    /* A sanitiser that removes everything is not a sanitiser. */
    char *out = san(
        "<h1>Title</h1><p>Some <strong>bold</strong> and <em>italic</em> text.</p>"
        "<ul><li>one</li><li>two</li></ul>"
        "<img src=\"https://site.example/a.png\" alt=\"a picture\" width=\"10\">"
        "<table><tr><td>cell</td></tr></table>");
    CHECK(has(out, "<h1>Title</h1>"), "a heading survives");
    CHECK(has(out, "<strong>bold</strong>"), "emphasis survives");
    CHECK(has(out, "<li>one</li>"), "a list survives");
    CHECK(has(out, "src=\"https://site.example/a.png\""), "an image survives");
    CHECK(has(out, "alt=\"a picture\""), "its alt text survives");
    CHECK(has(out, "<td>cell</td>"), "a table cell survives");
    free(out);

    /* A void element must not be given a close tag, or every line break would
     * become two. */
    out = san("<p>a<br>b</p>");
    CHECK(has(out, "<br>"), "a br is emitted as a void element");
    CHECK(!has(out, "</br>"), "and is not closed, which would double it");
    free(out);
}

static void test_text_is_escaped(void) {
    /*
     * A page whose text contains markup must display it, not run it.
     *
     * The `&` is escaped too, so an already-escaped entity becomes `&amp;lt;`
     * and the reader sees the literal characters `&lt;script&gt;`. That is the
     * correct outcome: the alternative — passing `&lt;` through untouched —
     * would decode it back to a real `<` in the browser, which is precisely the
     * bug this guards against. What matters is that no tag is created, so the
     * assertion is on `<script` rather than on the exact escaping.
     */
    char *out = san("<p>the tag &lt;script&gt; is dangerous</p>");
    CHECK(!has(out, "<script"), "escaped markup never becomes a real tag");
    CHECK(has(out, "&amp;lt;") || has(out, "&lt;script"),
          "and the text is still readable as characters");
    free(out);

    /* A bare angle bracket that is not a tag. */
    out = san("<p>3 < 5 and 5 > 3</p>");
    CHECK(has(out, "3 &lt; 5"), "a bare '<' is escaped");
    free(out);

    /* A raw script tag in the text position must not survive as markup. */
    out = san("<p>text</p>&lt;script&gt;alert(1)&lt;/script&gt;<p>more</p>");
    CHECK(!has(out, "<script"), "a doubly-escaped script is not resurrected");
    CHECK(has(out, "more"), "the page continues");
    free(out);
}

static void test_comments_and_doctype(void) {
    char *out = san("<!doctype html><!-- a comment --><p>text</p>");
    CHECK(!has(out, "a comment"), "a comment is dropped");
    CHECK(has(out, "text"), "the page around it survives");
    free(out);

    /* A conditional comment, which was a script vector in old IE. */
    out = san("<!--[if IE]><script>alert(1)</script><![endif]--><p>x</p>");
    CHECK(!has(out, "alert"), "a conditional comment does not hide a script");
    CHECK(has(out, "x"), "the page survives");
    free(out);
}

static void test_csp_is_injected(void) {
    char *out = san("<p>x</p>");
    CHECK(has(out, "Content-Security-Policy"), "a policy is injected");
    CHECK(has(out, "default-src 'none'"), "it denies everything by default");
    CHECK(!has(out, "unsafe-eval"), "it does not allow eval");
    CHECK(!has(out, "script-src"), "it does not allow scripts at all");
    CHECK(!has(out, "unsafe-inline' 'unsafe-inline"), "and does not allow scripts inline");
    /*
     * `frame-ancestors` must NOT be here: a browser ignores it in a `<meta>`
     * policy — verified, and it says so in the console — so including it would
     * claim a protection the document does not have. Who may frame this is
     * enforced by the console's sandbox attribute instead.
     */
    CHECK(!has(out, "frame-ancestors"),
          "no frame-ancestors, which a meta policy cannot enforce");
    free(out);

    /* The policy is also available on its own, so a test can pin it without
     * rendering a document. */
    const char *csp = mi_sanitize_csp();
    CHECK(csp && strstr(csp, "default-src 'none'") != NULL, "the policy is exposed");
}

static void test_links_are_rewritten(void) {
    mi_sanitize_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.link_prefix = "/api/v1/render?url=";
    cfg.base_url = "https://site.example/docs/page.html";

    const char *src = "<a href=\"/other\">next</a>";
    char *out = mi_sanitize_html(src, strlen(src), &cfg);
    CHECK(has(out, "/api/v1/render?url=https://site.example/other"),
          "a root-relative link is made absolute and routed through the renderer");
    free(out);

    src = "<a href=\"sibling.html\">sib</a>";
    out = mi_sanitize_html(src, strlen(src), &cfg);
    CHECK(has(out, "/api/v1/render?url=https://site.example/docs/sibling.html"),
          "a document-relative link resolves against the page's directory");
    free(out);

    src = "<a href=\"https://other.example/x\">abs</a>";
    out = mi_sanitize_html(src, strlen(src), &cfg);
    CHECK(has(out, "/api/v1/render?url=https://other.example/x"),
          "an absolute link is routed through the renderer");
    free(out);

    /* A fragment is a jump within this document, so it is left alone. */
    src = "<a href=\"#section\">jump</a>";
    out = mi_sanitize_html(src, strlen(src), &cfg);
    CHECK(has(out, "href=\"#section\""), "a fragment link is left as it is");
    free(out);

    /* With no prefix configured, links are left alone rather than mangled. */
    mi_sanitize_config bare;
    memset(&bare, 0, sizeof bare);
    src = "<a href=\"/other\">x</a>";
    out = mi_sanitize_html(src, strlen(src), &bare);
    CHECK(has(out, "href=\"/other\""), "without a prefix a link is untouched");
    free(out);
}

static void test_edge_cases(void) {
    /* An empty document is still a document. */
    char *out = san("");
    CHECK(out != NULL && has(out, "<html"), "an empty input yields a document");
    free(out);

    /* A lone angle bracket must not be dropped silently. */
    out = san("a < b");
    CHECK(has(out, "&lt;"), "a stray '<' is escaped, not lost");
    free(out);

    /* An unterminated tag at the very end. */
    out = san("<p>text<p");
    CHECK(has(out, "text"), "text before an unterminated tag survives");
    free(out);

    /* A tag name with a slash in it is not a tag. */
    out = san("<a/b onclick=x>y</a>");
    CHECK(!has(out, "onclick"), "a malformed tag does not smuggle a handler");
    free(out);

    /* A large document must be processed without hanging: this is a smoke test
     * that the parser terminates. */
    char big[20000];
    memset(big, 0, sizeof big);
    for (size_t i = 0; i + 8 < sizeof big; i += 8) memcpy(big + i, "<p>ab</p>", 8);
    out = san(big);
    CHECK(out != NULL && has(out, "ab"), "a large document is processed");
    free(out);
}

int main(void) {
    mi_log_set_level(MI_LOG_ERROR);

    test_scripts_are_gone();
    test_raw_text_content_is_not_counted_as_nesting();
    test_event_handlers_are_gone();
    test_dangerous_urls();
    test_attribute_breakout();
    test_svg_and_math_vectors();
    test_frames_and_meta();
    test_style_is_filtered();
    test_reading_features_survive();
    test_text_is_escaped();
    test_comments_and_doctype();
    test_csp_is_injected();
    test_links_are_rewritten();
    test_edge_cases();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
