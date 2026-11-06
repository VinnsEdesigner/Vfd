/* mi_extract_rows.h - turn a page into rows of fields, by rule.
 *
 * The crawler finds *files* by name. What a person often wants is the *data*:
 * every product with its name, price and link; every row of a table; every
 * article with its title and date. That is a different question, and this is the
 * answer.
 *
 * A rule set is a small document: a container selector that picks the repeating
 * unit (one product, one table row), and a field per column, each a selector
 * plus how to read it. The same shape covers CSS and XPath, because both are
 * just "select, then read a part of what you selected".
 *
 *   {"container": ".product",
 *    "fields": [
 *      {"name": "title", "selector": "h2", "from": "text"},
 *      {"name": "price", "selector": ".price", "from": "text"},
 *      {"name": "url",   "selector": "a", "from": "attr", "attr": "href"}
 *    ]}
 *
 * `from` is one of:
 *   text        the element's text and its descendants', collapsed
 *   own_text    only the element's direct text (a cell's own label)
 *   html        the element's inner HTML
 *   attr        an attribute's value (`attr` names it)
 *   href/src    shorthand for the common URL attributes, resolved against the page
 *
 * An `xpath` field may be used instead of `selector`. The result is rows of
 * name->value, which the caller renders as CSV or JSON.
 */
#ifndef MI_EXTRACT_ROWS_H
#define MI_EXTRACT_ROWS_H

#include <stddef.h>
#include "mi_html.h"

typedef enum {
    MI_FIELD_TEXT = 0, /* text of the matched element, collapsed */
    MI_FIELD_OWN_TEXT, /* direct text only */
    MI_FIELD_HTML,     /* inner HTML */
    MI_FIELD_ATTR,     /* the attribute named in `attr` */
    MI_FIELD_HREF,     /* href, resolved against the page URL */
    MI_FIELD_SRC,      /* src, resolved against the page URL */
    MI_FIELD_EXISTS,   /* "1" when the selector matched anything, else "" */
    MI_FIELD_COUNT,    /* how many the selector matched, as a number */
} mi_field_from;

typedef struct {
    char *name;
    /* A CSS selector, or an XPath expression when `xpath` is set. */
    char *selector;
    int xpath;
    mi_field_from from;
    char *attr; /* for MI_FIELD_ATTR */
    /* An optional literal when the selector matches nothing — so a column that
     * is sometimes absent still has a value. */
    char *default_value;
} mi_field_rule;

typedef struct {
    /* The repeating unit. NULL means "the whole document is one row", which is
     * what a single-item page wants. */
    char *container;
    int container_xpath;
    mi_field_rule *fields;
    size_t field_count;
} mi_row_rule;

/* One extracted row: values parallel to the rule's fields. */
typedef struct {
    char **values;
    size_t count;
} mi_row;

typedef struct {
    mi_row *rows;
    size_t row_count;
    /* The field names, in order, so a caller can emit a header. */
    char **names;
    size_t name_count;
} mi_rows;

void mi_rows_free(mi_rows *r);

/*
 * Parse a rule set from JSON. Returns NULL with `err` filled on a malformed
 * rule — a rule the engine cannot honour should say so, not silently extract
 * nothing.
 */
mi_row_rule *mi_row_rule_parse(const char *json, char *err, size_t errcap);
void mi_row_rule_free(mi_row_rule *r);

/*
 * Apply `rule` to a page. `page_url` (optional) resolves relative href/src.
 * Returns 0 and fills `out`, or -1 with `err` filled.
 */
int mi_extract_rows(const mi_row_rule *rule, const char *html, size_t len,
                    const char *page_url, mi_rows *out, char *err, size_t errcap);

/* Render rows as CSV (RFC 4180: quotes doubled, fields quoted when needed). */
char *mi_rows_to_csv(const mi_rows *rows);

/* Render rows as a JSON array of objects. */
char *mi_rows_to_json(const mi_rows *rows);

#endif
