/* mi_xpath.h - a small XPath engine over the HTML DOM.
 *
 * CSS covers "find this element"; XPath covers "walk to it by structure" and
 * "read this attribute" in one expression, which is what a lot of extraction
 * rules are written in. This is the subset those rules use, not all of XPath:
 *
 *   /html/body/div          an absolute path
 *   //div                   any descendant
 *   //div/p                 a child step
 *   //a/@href               an attribute value
 *   //div/text()            the text of the node
 *   //div[@class]           has an attribute
 *   //div[@class='x']       attribute equals
 *   //div[1]                the first such (1-based)
 *   //div[last()]           the last
 *   //div[contains(@class,'x')]
 *   //div[starts-with(@id,'p-')]
 *
 * Anything outside this subset is refused rather than guessed at, for the same
 * reason the CSS engine refuses: a rule the engine cannot honour should say so.
 */
#ifndef MI_XPATH_H
#define MI_XPATH_H

#include <stddef.h>
#include "mi_html.h"

typedef struct mi_xpath mi_xpath;

mi_xpath *mi_xpath_compile(const char *expr, char *err, size_t errcap);
void mi_xpath_free(mi_xpath *x);

/*
 * Evaluate against `root`, collecting the result nodes in document order.
 * Returns 0 and fills `*out`/`*count` (a `mi_malloc`ed array the caller frees).
 *
 * An expression ending in `/@attr` or `/text()` yields the *containing node* in
 * `*out`, and the caller reads the attribute or text from it via
 * `mi_xpath_is_attribute` / `mi_xpath_result_attribute`. This keeps the result
 * type uniform (nodes) while still supporting the two value selectors.
 */
int mi_xpath_select(const mi_xpath *x, const mi_html_node *root,
                    mi_html_node ***out, size_t *count);

/* 1 when the expression ends in `/@name`: the result is that attribute. */
int mi_xpath_is_attribute(const mi_xpath *x);
/* The attribute name for `/@name`, or NULL. */
const char *mi_xpath_result_attribute(const mi_xpath *x);
/* 1 when the expression ends in `/text()`. */
int mi_xpath_is_text(const mi_xpath *x);

/* The last compile error. */
const char *mi_xpath_last_error(void);

#endif
