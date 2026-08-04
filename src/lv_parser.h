/* lv_parser.h — Recursive-descent parser for Lua 5.4 syntax.
 *
 * Produces an AST (lv_node_t *) rooted at a LV_NODE_CHUNK.
 *
 * The parser implements the full Lua 5.4 grammar with these exceptions:
 *   - goto/label are parsed but not yet wired into the codegen
 *   - The "in" generic-for uses an iterator protocol implemented in
 *     the runtime (pairs/ipairs registered as host functions).
 */

#ifndef LV_PARSER_H
#define LV_PARSER_H

#include "lv.h"
#include "lv_lexer.h"
#include "lv_ast.h"

typedef struct {
    lv_lexer_t  lx;
    const char *source_name;  /* file name for error messages (may be NULL) */
    int         error_count;
    char       *last_error;   /* owned; most recent parse error */
} lv_parser_t;

/* Initialize a parser over the given source text. source_name is used
 * for error messages and may be NULL (defaults to "<input>"). */
void lv_parser_init(lv_parser_t *p, const char *src, size_t src_len,
                    const char *source_name);

void lv_parser_fini(lv_parser_t *p);

/* Parse the input as a chunk (the top-level grammar production).
 * Returns the AST root on success, or NULL on error (the error message
 * is in p->last_error). */
lv_node_t *lv_parser_parse(lv_parser_t *p);

/* Return the last error message (NULL if no error). */
const char *lv_parser_last_error(const lv_parser_t *p);

#endif /* LV_PARSER_H */
