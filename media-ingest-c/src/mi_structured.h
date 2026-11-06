/* mi_structured.h - read the data a page publishes about itself.
 *
 * A large slice of the web already states its data in a machine-readable form,
 * put there by the site for search engines:
 *
 *   JSON-LD      <script type="application/ld+json">{"@type":"Product",…}
 *   microdata    itemprop="name" inside itemscope/itemtype
 *   RDFa         property="name" (the same shape, older)
 *   OpenGraph    <meta property="og:title" content="…">
 *   Twitter card <meta name="twitter:title" content="…">
 *
 * Reading these is often *easier and more reliable* than writing a CSS selector:
 * the site has already decided what the price is called, and the answer survives
 * a redesign. For an extractor this is a first-class source, not a fallback.
 *
 * Everything is flattened to a list of (name, value) pairs with dotted names, so
 * one shape serves all four formats:
 *
 *   {"@type":"Product","name":"Widget","offers":{"price":"9.99"}}
 *     ->  @type=Product, name=Widget, offers.price=9.99
 *   <div itemscope itemtype="…/Product"><span itemprop="name">Widget</span>
 *     ->  name=Widget, @type=…/Product
 *   <meta property="og:title" content="Widget">
 *     ->  og:title=Widget
 *
 * A repeated name (a product with several offers) yields several pairs with the
 * same name, in document order, which is what a caller needs to see the
 * repetition at all.
 */
#ifndef MI_STRUCTURED_H
#define MI_STRUCTURED_H

#include <stddef.h>
#include "mi_html.h"

typedef struct {
    char **names;
    char **values;
    size_t count;
} mi_props;

void mi_props_free(mi_props *p);

/*
 * Extract every structured property from a parsed document.
 *
 * Reads JSON-LD, microdata, RDFa, OpenGraph and Twitter-card meta, in one pass.
 * Returns 0 and fills `out`. A page with none yields an empty list, not an error.
 */
int mi_structured_extract(const mi_html_node *doc, mi_props *out);

/* The first value for a name, or NULL. Names are matched case-insensitively. */
const char *mi_props_get(const mi_props *p, const char *name);

/* How many pairs carry this name (a repeated field, e.g. several offers). */
size_t mi_props_count(const mi_props *p, const char *name);

/* The nth value for a name, or NULL. */
const char *mi_props_get_n(const mi_props *p, const char *name, size_t n);

/*
 * Render the properties as a JSON object, for the API. A repeated name becomes
 * an array; a single one stays a string. Owned by the caller.
 */
char *mi_props_to_json(const mi_props *p);

#endif
