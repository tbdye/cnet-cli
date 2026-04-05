/*
 * log.c -- Log file commands for cnet-cli
 *
 * System log operations: list, read, callers
 *
 * log list   -- enumerate files in sysdata:log/
 * log read   -- read a named log file with optional --tail/--lines
 * log callers -- shortcut for reading the "calls" log
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>
#include <exec/memory.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>

#include "log.h"
#include "json.h"
#include "util.h"

/* Maximum bytes to read from a log file. */
#define MAX_READ_SIZE (512L * 1024L)

/* Default line cap when neither --tail nor --lines is specified. */
#define DEFAULT_LINE_CAP 1000

/* Maximum number of line pointers we track. */
#define MAX_LINE_PTRS 10000

/* ---- helpers ---- */

/*
 * Format an AmigaOS DateStamp as ISO 8601 "YYYY-MM-DDTHH:MM:SS".
 * Returns buf.
 */
static char *format_datestamp(char *buf, int bufsz, struct DateStamp *ds)
{
    long days, year, month, day;
    long hour, minute, second;
    long m;
    static const int mdays[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    /* Time from ticks: ds_Minute is minutes since midnight,
     * ds_Tick is ticks (1/50th sec) within the current minute. */
    hour   = ds->ds_Minute / 60;
    minute = ds->ds_Minute % 60;
    second = ds->ds_Tick / 50;

    /* Convert days since 1978-01-01 to year/month/day. */
    days = ds->ds_Days;
    year = 1978;

    for (;;) {
        int leap = (year % 4 == 0 &&
                    (year % 100 != 0 || year % 400 == 0));
        int yd = leap ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        year++;
    }

    /* days is now day-of-year (0-based) */
    month = 0;
    for (m = 0; m < 12; m++) {
        int md = mdays[(int)m];
        if (m == 1) {
            int leap = (year % 4 == 0 &&
                        (year % 100 != 0 || year % 400 == 0));
            if (leap) md = 29;
        }
        if (days < md) break;
        days -= md;
        month++;
    }
    day = days + 1; /* 1-based */

    snprintf(buf, bufsz, "%04ld-%02ld-%02ldT%02ld:%02ld:%02ld",
        year, month + 1, day, hour, minute, second);
    return buf;
}

/*
 * Validate a log file name: reject path separators and dot-names.
 * Returns 1 if valid, 0 if rejected.
 */
static int validate_logname(const char *name)
{
    if (!name || !name[0]) return 0;
    if (strchr(name, '/') || strchr(name, ':') || strchr(name, '\\'))
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    return 1;
}

/*
 * Shared implementation for cmd_log_read and cmd_log_callers.
 *
 * Reads the named log file from sysdata:log/, applies --tail or --lines
 * filtering, and emits JSON output.
 */
static int log_read_internal(struct MainPort *myp, const char *logname,
    int tail, int lines)
{
    struct json_state js;
    char path[256];
    BPTR fh;
    long fsize, readsize, offset;
    char *buf;
    char **lineptrs;
    int nlines, i, start, count;
    int truncated = 0;
    int cap;

    snprintf(path, sizeof(path), "sysdata:log/%s", logname);

    ObtainSemaphoreShared(&myp->SEM[12]);

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        ReleaseSemaphore(&myp->SEM[12]);
        json_error("Cannot open log file");
        return 1;
    }

    /* Get file size: after Open, position is 0.
     * Seek(fh, 0, OFFSET_END) moves to end, returns old pos (0).
     * Seek(fh, 0, OFFSET_BEGINNING) moves to start, returns old pos (size). */
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);

    if (fsize <= 0) {
        Close(fh);
        ReleaseSemaphore(&myp->SEM[12]);

        /* Empty file: emit minimal result. */
        json_init(&js, stdout);
        json_obj_open(&js);
        json_kv_str(&js, "log", logname);
        json_key(&js, "lines");
        json_arr_open(&js);
        json_arr_close(&js);
        json_kv_int(&js, "count", 0);
        json_kv_bool(&js, "truncated", 0);
        json_obj_close(&js);
        json_finish(&js);
        return 0;
    }

    /* Cap read size. If file exceeds limit, read only the tail portion. */
    if (fsize > MAX_READ_SIZE) {
        offset = fsize - MAX_READ_SIZE;
        readsize = MAX_READ_SIZE;
        truncated = 1;
        Seek(fh, offset, OFFSET_BEGINNING);
    } else {
        readsize = fsize;
    }

    buf = (char *)malloc(readsize + 1);
    if (!buf) {
        Close(fh);
        ReleaseSemaphore(&myp->SEM[12]);
        json_error("Out of memory");
        return 1;
    }

    if (Read(fh, buf, readsize) != readsize) {
        free(buf);
        Close(fh);
        ReleaseSemaphore(&myp->SEM[12]);
        json_error("Read failed");
        return 1;
    }

    Close(fh);
    ReleaseSemaphore(&myp->SEM[12]);

    buf[readsize] = '\0';

    /* If we truncated, skip the first partial line. */
    if (truncated) {
        char *nl = strchr(buf, '\n');
        if (nl) {
            /* Shift our view past the partial line. */
            long skip = (long)(nl - buf) + 1;
            memmove(buf, nl + 1, readsize - skip + 1);
            readsize -= skip;
        }
    }

    /* Build line pointer array. */
    lineptrs = (char **)malloc(MAX_LINE_PTRS * sizeof(char *));
    if (!lineptrs) {
        free(buf);
        json_error("Out of memory");
        return 1;
    }

    nlines = 0;
    if (readsize > 0) {
        char *p = buf;
        while (*p && nlines < MAX_LINE_PTRS) {
            char *nl;
            lineptrs[nlines++] = p;
            nl = strchr(p, '\n');
            if (!nl) break;
            *nl = '\0';
            p = nl + 1;
        }
        /* Drop trailing empty line from final newline. */
        if (nlines > 0 && lineptrs[nlines - 1][0] == '\0')
            nlines--;
    }

    /* Determine which lines to output. */
    if (tail > 0) {
        /* --tail N: last N lines */
        if (tail >= nlines) {
            start = 0;
            count = nlines;
        } else {
            start = nlines - tail;
            count = tail;
        }
    } else if (lines > 0) {
        /* --lines N: first N lines */
        start = 0;
        count = (lines < nlines) ? lines : nlines;
    } else {
        /* Default cap */
        cap = DEFAULT_LINE_CAP;
        if (nlines > cap) {
            start = nlines - cap;
            count = cap;
            truncated = 1;
        } else {
            start = 0;
            count = nlines;
        }
    }

    /* Emit JSON. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "log", logname);
    json_key(&js, "lines");
    json_arr_open(&js);
    for (i = start; i < start + count; i++)
        json_str(&js, lineptrs[i]);
    json_arr_close(&js);
    json_kv_int(&js, "count", (long)count);
    json_kv_bool(&js, "truncated", truncated);
    json_obj_close(&js);
    json_finish(&js);

    free(lineptrs);
    free(buf);
    return 0;
}

/* ---- log list ---- */

int cmd_log_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char datebuf[24];
    BPTR lock;
    struct FileInfoBlock *fib;
    int total;

    (void)myp;
    (void)argc;
    (void)argv;

    lock = Lock((CONST_STRPTR)"sysdata:log", ACCESS_READ);
    if (!lock) {
        json_error("Cannot lock sysdata:log");
        return 1;
    }

    fib = (struct FileInfoBlock *)AllocVec(
        sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        UnLock(lock);
        json_error("Out of memory");
        return 1;
    }

    if (!Examine(lock, fib)) {
        FreeVec(fib);
        UnLock(lock);
        json_error("Cannot examine sysdata:log");
        return 1;
    }

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "directory", "sysdata:log");
    json_key(&js, "files");
    json_arr_open(&js);

    total = 0;
    while (ExNext(lock, fib)) {
        /* Only list files, not subdirectories. */
        if (fib->fib_DirEntryType >= 0)
            continue;

        json_obj_open(&js);
        json_kv_str(&js, "name", (const char *)fib->fib_FileName);
        json_kv_int(&js, "size", fib->fib_Size);
        json_kv_str(&js, "date",
            format_datestamp(datebuf, sizeof(datebuf), &fib->fib_Date));
        json_obj_close(&js);
        total++;
    }

    json_arr_close(&js);
    json_kv_int(&js, "total", (long)total);
    json_obj_close(&js);
    json_finish(&js);

    FreeVec(fib);
    UnLock(lock);
    return 0;
}

/* ---- log read ---- */

int cmd_log_read(struct MainPort *myp, int argc, char **argv)
{
    const char *logname = NULL;
    int tail = 0;
    int lines = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            i++;
            if (!all_digits(argv[i])) {
                json_error("--tail requires a numeric argument");
                return 1;
            }
            tail = atoi(argv[i]);
        } else if (strcmp(argv[i], "--lines") == 0 && i + 1 < argc) {
            i++;
            if (!all_digits(argv[i])) {
                json_error("--lines requires a numeric argument");
                return 1;
            }
            lines = atoi(argv[i]);
        } else if (!logname) {
            logname = argv[i];
        }
    }

    if (!logname) {
        json_error("Usage: log read <logname> [--tail N] [--lines N]");
        return 1;
    }

    if (!validate_logname(logname)) {
        json_error("Invalid log name");
        return 1;
    }

    if (tail > 0 && lines > 0) {
        json_error("Cannot use both --tail and --lines");
        return 1;
    }

    return log_read_internal(myp, logname, tail, lines);
}

/* ---- log callers ---- */

int cmd_log_callers(struct MainPort *myp, int argc, char **argv)
{
    int tail = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            i++;
            if (!all_digits(argv[i])) {
                json_error("--tail requires a numeric argument");
                return 1;
            }
            tail = atoi(argv[i]);
        }
    }

    return log_read_internal(myp, "calls", tail, 0);
}
