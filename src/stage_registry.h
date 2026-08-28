#ifndef STAGE_REGISTRY_H
#define STAGE_REGISTRY_H

#include <stddef.h>
#include "stage.h"

/* The compile-time table of every stage type this binary knows how to
 * run - see stage.h's own header comment for why this is static, not a
 * dynamically loaded plugin table. */

/* Looks up a stage type by name (matches a graph JSON node's "type"
 * field, case-sensitive). Returns NULL if no such stage type is
 * compiled into this binary - graph_config.c treats that as a
 * startup-time error, never a runtime one. */
const struct stage *stage_registry_find(const char *name);

/* For a future GET /api/stage-types endpoint (Phase 3): the whole table,
 * indexable, so the web UI's palette and edge-validation rules can be
 * built from what this binary actually supports instead of a
 * client-side hardcoded list that could drift out of sync. */
size_t stage_registry_count(void);
const struct stage *stage_registry_get(size_t idx);

#endif
