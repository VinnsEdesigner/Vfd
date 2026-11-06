/* mi_css.h - a CSS selector engine over the HTML DOM.
 *
 * "Give me the price inside `.product .price`" needs a selector, and a selector
 * needs the DOM from mi_html. This is the engine: it parses a selector once and
 * matches nodes against it.
 *
 * The subset is the one people actually write for extraction — a page's structure
 * is rarely more exotic than this:
 *
 *   div            tag
 *   .price         class
 *   #main          id
 *   *              any element
 *   a b            descendant
 *   a > b          direct child
 *   a + b          next sibling
 *   a ~ b          following sibling
 *   [href]         has attribute
 *   [href="x"]     attribute equals  (also ^= $= *= ~= for prefix/suffix/contains/word)
 *   a, b           a group (any of)
 *   :first-child   first element among its siblings
 *   :last-child    last element among its siblings
 *   :nth-child(n)  the nth element among its siblings (1-based; odd/even too)
 *   :not(sel)      excludes a match
 *
 * A selector that cannot be parsed is refused rather than guessed at: a caller
 * that wrote a broken rule wants to know, not to silently extract nothing.
 */
#ifndef MI_CSS_H
#define MI_CSS_H

#include <stddef.h>
#include "mi_html.h"

typedef struct mi_css_selector mi_css_selector;

/*
 * Compile `selector`. Returns NULL if it is malformed, with `err` (optional)
 * describing the problem. A compiled selector is immutable and may be reused
 * across nodes and threads.
 */
mi_css_selector *mi_css_compile(const char *selector, char *err, size_t errcap);

void mi_css_free(mi_css_selector *sel);

/* Whether `node` matches. A non-element node never matches. */
int mi_css_matches(const mi_css_selector *sel, const mi_html_node *node);

/*
 * Collect every node under `root` (inclusive) that matches, in document order.
 * `*out` receives a `mi_malloc`ed array the caller frees; `*count` its length.
 * Returns 0 on success.
 */
int mi_css_select(const mi_css_selector *sel, const mi_html_node *root,
                  mi_html_node ***out, size_t *count);

/* The last compile error, for a caller that did not pass its own buffer. */
const char *mi_css_last_error(void);

#endif
