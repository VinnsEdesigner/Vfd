/* mi_html5.h - the HTML5 tree-construction algorithm.
 *
 * The tolerant parser in mi_html.c nests most real pages correctly, but it is an
 * approximation: it knows a handful of "implicitly closes" pairs and nothing
 * about the spec's insertion modes, the active-formatting-elements list, or
 * foster parenting. The cases it gets wrong are exactly the ones that matter to
 * a selector — a mis-nested table, a `<p>` reopened after a block, an
 * unclosed `<b>` spanning paragraphs.
 *
 * This is the algorithm from the HTML Standard, not an approximation of it:
 *
 *   tokenizer     the real state machine, including raw-text and RCDATA
 *                 elements, character references, comments and doctypes.
 *   tree builder  insertion modes (initial, before/after head, in body, text,
 *                 in table and its sub-modes, in select, after body, …), the
 *                 stack of open elements, implied end tags, and the
 *                 "reset the insertion mode appropriately" rule.
 *   formatting    the list of active formatting elements and the adoption
 *                 agency algorithm, so `<b>a<i>b</b>c</i>` nests the way a
 *                 browser nests it.
 *   tables        foster parenting, so content that appears inside a table but
 *                 is not table content is moved before it, as a browser does.
 *
 * It produces the same `mi_html_node` tree, so every consumer (the CSS engine,
 * XPath, extraction) is unchanged.
 *
 * Deliberate simplifications, each of which a caller should know about:
 *   - the input is assumed already decoded to UTF-8; there is no encoding
 *     sniffing or `<meta charset>` handling (the fetch layer does not decode
 *     either, so this matches the rest of the engine);
 *   - foreign content (SVG, MathML) is parsed as ordinary elements with their
 *     names and attributes preserved, without the spec's integration points.
 *     Their self-closing syntax is honoured;
 *   - the scripting flag is off by default, so `<noscript>` is parsed as markup
 *     (which is what a non-scripting user agent does).
 */
#ifndef MI_HTML5_H
#define MI_HTML5_H

#include <stddef.h>
#include "mi_html.h"

typedef struct {
    /* 1 when scripting is enabled: `<noscript>` content becomes raw text. */
    int scripting;
    /* A cap on nesting depth, so a hostile page of a million open tags cannot
     * exhaust the stack. The spec has no such limit; this is a safety valve. */
    int max_depth;
} mi_html5_config;

/*
 * Parse `html` per the HTML5 tree-construction algorithm. Returns a document
 * node (children are the top-level nodes) or NULL on allocation failure. Free
 * with `mi_html_free`, the same as the other parser.
 */
mi_html_node *mi_html5_parse(const char *html, size_t len,
                             const mi_html5_config *cfg);

#endif
