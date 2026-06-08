#ifndef PG_RESULT_H
#define PG_RESULT_H

/*
 * pg_result.h — per-thread result storage for the PostgreSQL extension.
 *
 * Shared by postgres.c, pg_lua_helpers.c, and pg_quickjs_helpers.c.
 * No scripting engine headers; no wrkx core headers.
 *
 * ADR 0005, Phase 6 (P6-3).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "pg_message.h"   /* pg_col_desc_t, pg_data_row_t, PG_RESULT_MAX_COLS */

#define PG_RESULT_MAX_ROWS   256
#define PG_RESULT_HEAP_SIZE  (256 * 1024)   /* 256 KB per thread */

typedef struct {
    pg_col_desc_t  cols[PG_RESULT_MAX_COLS];
    int16_t        ncols;
    struct {
        const char *value;  /* NULL for SQL NULL; points into heap */
        size_t      len;
    } fields[PG_RESULT_MAX_ROWS][PG_RESULT_MAX_COLS];
    int32_t        nrows;
    char           cmd_tag[64];
    uint8_t        pg_status;   /* 'I', 'T', or 'E' from final ReadyForQuery */
    bool           valid;
    char           heap[PG_RESULT_HEAP_SIZE];
    size_t         heap_used;
} pg_result_t;

/* Defined in pg_lua_helpers.c; one instance per OS thread. */
extern __thread pg_result_t tls_result;

void pg_result_reset(void);
void pg_result_set_columns(const pg_col_desc_t *cols, int16_t ncols);
void pg_result_append_row(const pg_col_desc_t *cols, int16_t ncols,
                          const pg_data_row_t *row, size_t consumed);
void pg_result_set_cmd_tag(const char *tag);

#endif /* PG_RESULT_H */
