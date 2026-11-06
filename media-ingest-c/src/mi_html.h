/* mi_html.h - a small, tolerant HTML DOM.
 *
 * The crawler already walks tags with a streaming scan, which is the right shape
 * for "find every link" and the wrong one for "give me the price inside
 * `.product .price`": a selector needs a *tree*, with parents and siblings, to
 * answer questions like "the second cell of this row" or "the anchor inside this
 * heading".
 *
 * This is that tree. It is deliberately tolerant, the way a browser is: real
 * pages are malformed, and refusing to parse them would make the extractor
 * useless on half the web. It is not a validator and makes no claim to
 * reproduce the HTML5 tree-construction algorithm exactly — it reproduces it
 * closely enough that the selectors people actually write find the right nodes.
 *
 * What it handles:
 *   - void elements (`<br>`, `<img>`, …) that never have children;
 *   - implicit closes (`<li>` closes the previous `<li>`, `<p>` closes on a
 *     block, table cells close each other) so a page without end tags still
 *     nests correctly;
 *   - comments and doctypes, kept as nodes so nothing is lost;
 *   - `script`/`style`/`textarea` content as raw text, so a `<` inside a script
 *     does not become a tag;
 *   - character references (`&amp;`, `&#233;`, `&nbsp;`).
 */
#ifndef MI_HTML_H
#define MI_HTML_H

#include <stddef.h>

typedef enum {
    MI_HTML_DOCUMENT = 0,
    MI_HTML_ELEMENT,
    MI_HTML_TEXT,
    MI_HTML_COMMENT,
    MI_HTML_DOCTYPE,
} mi_html_type;

typedef struct mi_html_node mi_html_node;

struct mi_html_node {
    mi_html_type type;
    /* The tag name, lowercased (ELEMENT), or the comment/doctype text. */
    char *name;
    /* The text (TEXT), comment body, or doctype. NULL for an element. */
    char *text;
    /* Attributes (ELEMENT). Names lowercased. */
    char **attr_names;
    char **attr_values;
    size_t attr_count;

    mi_html_node *parent;
    mi_html_node *first_child;
    mi_html_node *last_child;
    mi_html_node *next_sibling;
    mi_html_node *prev_sibling;
};

/*
 * Parse `html` into a document node whose children are the top-level nodes.
 *
 * This is the HTML5 tree-construction algorithm (see mi_html5.h), so the tree
 * is nested the way a browser nests it — implied html/head/body, foster
 * parenting around tables, the adoption agency for mis-nested formatting
 * elements. The tolerant hand-written walker this used to be is kept as
 * `mi_html_parse_legacy` for reference, but every consumer uses this.
 *
 * Returns NULL only on allocation failure; empty input yields an empty
 * document. Free with `mi_html_free`.
 */
mi_html_node *mi_html_parse(const char *html, size_t len);

/* The previous tolerant walker, kept only for comparison in tests. */
mi_html_node *mi_html_parse_legacy(const char *html, size_t len);

/* Free a node and everything under it. */
void mi_html_free(mi_html_node *node);

/* An attribute's value, or NULL. Name is matched case-insensitively. */
const char *mi_html_attr(const mi_html_node *el, const char *name);

/* The element's own text and all its descendants', concatenated, with runs of
 * whitespace collapsed and trimmed. Owned by the caller. */
char *mi_html_text(const mi_html_node *el);

/* The element's *direct* text children only, joined — no descendant text. This
 * is the difference between a cell's own label and everything nested in it. */
char *mi_html_own_text(const mi_html_node *el);

/* Whether the node is an element with this (lowercased) tag name. */
int mi_html_is(const mi_html_node *n, const char *tag);

/* Depth-first iteration helper: the next node in document order under `root`,
 * or NULL. Used by the selector engine. */
mi_html_node *mi_html_next(const mi_html_node *root, const mi_html_node *n);

#endif
