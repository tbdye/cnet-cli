/*
 * json.h -- JSON output emitter for cnet-cli
 *
 * Output-only JSON writer with automatic comma tracking.
 * All output goes to a FILE* (typically stdout or stderr).
 */

#ifndef CNET_CLI_JSON_H
#define CNET_CLI_JSON_H

#include <stdio.h>

/*
 * Maximum nesting depth for objects/arrays.
 * 16 levels is generous for our use case.
 */
#define JSON_MAX_DEPTH 16

struct json_state {
    FILE *fp;
    int depth;
    /*
     * need_comma[d] is nonzero if the next value at nesting depth d
     * must be preceded by a comma (i.e., it is not the first element).
     */
    char need_comma[JSON_MAX_DEPTH];
};

/* Initialize a json_state to write to the given FILE*. */
void json_init(struct json_state *js, FILE *fp);

/* Structural */
void json_obj_open(struct json_state *js);
void json_obj_close(struct json_state *js);
void json_arr_open(struct json_state *js);
void json_arr_close(struct json_state *js);

/* Key (for use inside objects, before a value call) */
void json_key(struct json_state *js, const char *name);

/* Values -- each emits a complete JSON value */
void json_str(struct json_state *js, const char *s);
void json_int(struct json_state *js, long n);
void json_uint(struct json_state *js, unsigned long n);
void json_bool(struct json_state *js, int b);
void json_null(struct json_state *js);

/* Convenience: emit key + value in one call */
void json_kv_str(struct json_state *js, const char *key, const char *val);
void json_kv_int(struct json_state *js, const char *key, long val);
void json_kv_uint(struct json_state *js, const char *key, unsigned long val);
void json_kv_bool(struct json_state *js, const char *key, int val);
void json_kv_null(struct json_state *js, const char *key);

/* Finish: writes a newline after the top-level value. */
void json_finish(struct json_state *js);

#endif /* CNET_CLI_JSON_H */
