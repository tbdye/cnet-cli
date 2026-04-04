/*
 * json.c -- JSON output emitter for cnet-cli
 *
 * Compact JSON writer with automatic comma state tracking.
 * String values are escaped per RFC 8259 (JSON).
 */

#include "json.h"
#include <string.h>

void json_init(struct json_state *js, FILE *fp)
{
    memset(js, 0, sizeof(*js));
    js->fp = fp;
}

/*
 * Emit a comma if needed before the next value/key at the current depth.
 * Then mark that future values at this depth will need a comma.
 */
static void json_comma(struct json_state *js)
{
    if (js->depth > 0 && js->depth < JSON_MAX_DEPTH) {
        if (js->need_comma[js->depth]) {
            fputc(',', js->fp);
        }
        js->need_comma[js->depth] = 1;
    }
}

static void json_push(struct json_state *js)
{
    js->depth++;
    if (js->depth < JSON_MAX_DEPTH) {
        js->need_comma[js->depth] = 0;
    }
}

static void json_pop(struct json_state *js)
{
    if (js->depth > 0) {
        js->depth--;
    }
}

void json_obj_open(struct json_state *js)
{
    json_comma(js);
    fputc('{', js->fp);
    json_push(js);
}

void json_obj_close(struct json_state *js)
{
    json_pop(js);
    fputc('}', js->fp);
}

void json_arr_open(struct json_state *js)
{
    json_comma(js);
    fputc('[', js->fp);
    json_push(js);
}

void json_arr_close(struct json_state *js)
{
    json_pop(js);
    fputc(']', js->fp);
}

void json_key(struct json_state *js, const char *name)
{
    json_comma(js);
    fputc('"', js->fp);
    /* Key names are ASCII identifiers -- no escaping needed. */
    fputs(name, js->fp);
    fputs("\":", js->fp);
    /*
     * After emitting the key, the next call will be the value.
     * We must NOT emit a comma before that value, so clear the flag.
     * The comma was already emitted (if needed) before the key.
     */
    if (js->depth > 0 && js->depth < JSON_MAX_DEPTH) {
        js->need_comma[js->depth] = 0;
    }
}

/*
 * Emit a JSON-escaped string (the content between quotes).
 * Handles: backslash, double-quote, and control characters 0x00-0x1F.
 * CNet MCI escape bytes (0x11 CTRL-Q, 0x19 CTRL-Y) are control chars
 * and will be escaped as \uXXXX.
 */
static void json_emit_escaped(struct json_state *js, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    while (*p) {
        if (*p == '"') {
            fputs("\\\"", js->fp);
        } else if (*p == '\\') {
            fputs("\\\\", js->fp);
        } else if (*p == '\n') {
            fputs("\\n", js->fp);
        } else if (*p == '\r') {
            fputs("\\r", js->fp);
        } else if (*p == '\t') {
            fputs("\\t", js->fp);
        } else if (*p == '\b') {
            fputs("\\b", js->fp);
        } else if (*p == '\f') {
            fputs("\\f", js->fp);
        } else if (*p < 0x20) {
            /* Control characters including CNet MCI bytes (0x11, 0x19) */
            fprintf(js->fp, "\\u%04x", (unsigned)*p);
        } else if (*p >= 0x80) {
            /* Latin-1 high bytes -> \u00XX for valid UTF-8 JSON */
            fprintf(js->fp, "\\u%04x", (unsigned)*p);
        } else {
            fputc(*p, js->fp);
        }
        p++;
    }
}

void json_str(struct json_state *js, const char *s)
{
    json_comma(js);
    if (!s) {
        fputs("null", js->fp);
        return;
    }
    fputc('"', js->fp);
    json_emit_escaped(js, s);
    fputc('"', js->fp);
}

void json_int(struct json_state *js, long n)
{
    json_comma(js);
    fprintf(js->fp, "%ld", n);
}

void json_uint(struct json_state *js, unsigned long n)
{
    json_comma(js);
    fprintf(js->fp, "%lu", n);
}

void json_bool(struct json_state *js, int b)
{
    json_comma(js);
    fputs(b ? "true" : "false", js->fp);
}

void json_null(struct json_state *js)
{
    json_comma(js);
    fputs("null", js->fp);
}

/* Convenience key-value helpers */

void json_kv_str(struct json_state *js, const char *key, const char *val)
{
    json_key(js, key);
    json_str(js, val);
}

void json_kv_int(struct json_state *js, const char *key, long val)
{
    json_key(js, key);
    json_int(js, val);
}

void json_kv_uint(struct json_state *js, const char *key, unsigned long val)
{
    json_key(js, key);
    json_uint(js, val);
}

void json_kv_bool(struct json_state *js, const char *key, int val)
{
    json_key(js, key);
    json_bool(js, val);
}

void json_kv_null(struct json_state *js, const char *key)
{
    json_key(js, key);
    json_null(js);
}

void json_finish(struct json_state *js)
{
    fputc('\n', js->fp);
    fflush(js->fp);
}
