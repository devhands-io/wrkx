/* src/scripting/session.c
 *
 * Request Layer session store (ADR 0001, Phase 1, P1-4).
 *
 * A per-connection, script-visible key-value store. The orchestrator hangs one
 * session off each connection (reachable from scripts via
 * connection.script_state); the Request Layer owns its lifetime. Keys and
 * values are NUL-terminated strings, copied on insert. Setting an existing key
 * overwrites its value. The store is engine-agnostic: it contains no Lua,
 * QuickJS or protocol knowledge, so any scripting engine can lean on it.
 *
 * Implemented as a small singly-linked list. A session holds the handful of
 * keys a script tracks per connection, so linear scan is the right complexity
 * trade here; this can become a hash table later without touching the
 * frozen contract in script_api.h.
 */

#include <stdlib.h>
#include <string.h>

#include "scripting/script_api.h"

typedef struct session_entry {
    char                 *key;
    char                 *value;
    struct session_entry *next;
} session_entry;

struct session {
    session_entry *head;
};

session *session_create(void) {
    session *s = calloc(1, sizeof(*s));
    return s;
}

static session_entry *session_find(session *s, const char *key) {
    for (session_entry *e = s->head; e != NULL; e = e->next) {
        if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

void session_set(session *s, const char *key, const char *value) {
    if (s == NULL || key == NULL || value == NULL) return;

    session_entry *e = session_find(s, key);
    if (e != NULL) {
        char *dup = strdup(value);
        if (dup == NULL) return;          /* keep old value on OOM */
        free(e->value);
        e->value = dup;
        return;
    }

    e = calloc(1, sizeof(*e));
    if (e == NULL) return;
    e->key   = strdup(key);
    e->value = strdup(value);
    if (e->key == NULL || e->value == NULL) {
        free(e->key);
        free(e->value);
        free(e);
        return;
    }
    e->next = s->head;
    s->head = e;
}

const char *session_get(session *s, const char *key) {
    if (s == NULL || key == NULL) return NULL;
    session_entry *e = session_find(s, key);
    return e != NULL ? e->value : NULL;
}

void session_destroy(session *s) {
    if (s == NULL) return;
    session_entry *e = s->head;
    while (e != NULL) {
        session_entry *next = e->next;
        free(e->key);
        free(e->value);
        free(e);
        e = next;
    }
    free(s);
}
