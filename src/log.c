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
#include <ctype.h>

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

/* ---- log callers-parsed ---- */

/*
 * Strip CNet MCI escape sequences (0x19 + command) in-place.
 * Returns new length. Null-terminates the result.
 */
static long strip_mci_raw(char *buf, long len)
{
    long r = 0, w = 0;

    while (r < len) {
        if ((unsigned char)buf[r] == 0x19 && r + 1 < len) {
            char cmd = buf[r + 1];
            if ((cmd == 'c' || cmd == ':') && r + 2 < len)
                r += 3;   /* 3-byte color/attribute sequence */
            else
                r += 2;   /* 2-byte unknown escape */
        } else {
            buf[w++] = buf[r++];
        }
    }
    buf[w] = '\0';
    return w;
}

/*
 * Check if content looks like a SAM summary line.
 * SAM format: two alpha chars, ':', then digit(s).
 * Example: "ms:2    m1:1    mu:1"
 */
static int looks_like_sam(const char *content)
{
    if (strlen(content) < 4)
        return 0;
    if (!isalpha((unsigned char)content[0]) ||
        !isalpha((unsigned char)content[1]))
        return 0;
    if (content[2] != ':')
        return 0;
    if (!isdigit((unsigned char)content[3]))
        return 0;
    return 1;
}

/* Parsed record structures (stack-local, reused per record). */

#define MAX_EVENTS_PER_RECORD 32
#define MAX_SAM_PAIRS 15

struct parsed_event {
    char time[6];       /* "HH:MM" */
    char label[16];     /* event label, trimmed */
    char detail[256];   /* event detail, trimmed */
};

struct sam_pair {
    char key[4];        /* "ms", "mu", etc. */
    long value;
};

struct parsed_record {
    /* Header */
    char date[8];         /* "DD-Mon" */
    char time[6];         /* "HH:MM" */
    int port;             /* port number, -1 if unknown */
    char connect[64];     /* connect string */

    /* Events */
    struct parsed_event events[MAX_EVENTS_PER_RECORD];
    int num_events;

    /* User detail (from post-SIGNON detail line) */
    char user_handle[32];
    char user_realname[64];
    char user_phone[16];
    char user_verification[16];
    char user_country[16];
    int has_user_detail;

    /* SIGNON specifics */
    int signon_account;   /* -1 if no SIGNON */
    int signon_caller;    /* -1 if no SIGNON */

    /* SIGNOFF */
    char signoff_reason[32];

    /* SAM counters */
    struct sam_pair sam[MAX_SAM_PAIRS];
    int num_sam;
};

/*
 * Parse the header line of a record.
 * Expected format: "DD-Mon HH:MM Port N    connect_string"
 * Columns: 1-6 date, 7 space, 8-12 time, 13 space, 14-23 label, 24+ detail.
 * Returns 1 on success, 0 if the line is too short or not a header.
 */
static int parse_header_line(const char *line, struct parsed_record *rec)
{
    int len = (int)strlen(line);
    const char *p;
    int i;

    /* Header lines have non-space content in the date column (col 1). */
    if (len < 14 || line[0] == ' ')
        return 0;

    /* Date: columns 1-6 (index 0-5). */
    snprintf(rec->date, sizeof(rec->date), "%.*s", 6, line);
    /* Trim trailing spaces from date. */
    for (i = (int)strlen(rec->date) - 1; i >= 0 && rec->date[i] == ' '; i--)
        rec->date[i] = '\0';

    /* Time: columns 8-12 (index 7-11). */
    if (len >= 12) {
        snprintf(rec->time, sizeof(rec->time), "%.*s", 5, line + 7);
        /* Trim leading/trailing spaces from time. */
        p = rec->time;
        while (*p == ' ') p++;
        if (p != rec->time)
            memmove(rec->time, p, strlen(p) + 1);
    }

    /* Label column (14-23, index 13-22): expect "Port N". */
    rec->port = -1;
    if (len >= 18 && strncmp(line + 13, "Port ", 5) == 0) {
        rec->port = atoi(line + 18);
    }

    /* Connect string: everything after the label column (index 23+). */
    rec->connect[0] = '\0';
    if (len > 23) {
        const char *cs = line + 23;
        /* Trim leading spaces. */
        while (*cs == ' ') cs++;
        snprintf(rec->connect, sizeof(rec->connect), "%s", cs);
        /* Trim trailing spaces. */
        for (i = (int)strlen(rec->connect) - 1;
             i >= 0 && rec->connect[i] == ' '; i--)
            rec->connect[i] = '\0';
    }

    return 1;
}

/*
 * Parse an event line and append to rec->events[].
 * Expected format: "       HH:MM LABEL     detail..."
 * Also extracts SIGNON account/caller and SIGNOFF reason.
 */
static void parse_event_line(const char *line, struct parsed_record *rec)
{
    int len = (int)strlen(line);
    struct parsed_event *ev;
    const char *p;
    int i;
    char label_buf[16];

    if (rec->num_events >= MAX_EVENTS_PER_RECORD)
        return;

    ev = &rec->events[rec->num_events];
    memset(ev, 0, sizeof(*ev));

    /* Time: columns 8-12 (index 7-11). */
    if (len >= 12) {
        snprintf(ev->time, sizeof(ev->time), "%.*s", 5, line + 7);
        p = ev->time;
        while (*p == ' ') p++;
        if (p != ev->time)
            memmove(ev->time, p, strlen(p) + 1);
    }

    /* Label: columns 14-23 (index 13-22). */
    if (len >= 14) {
        int label_end = (len < 23) ? len : 23;
        snprintf(label_buf, sizeof(label_buf), "%.*s",
            label_end - 13, line + 13);
        /* Trim trailing spaces. */
        for (i = (int)strlen(label_buf) - 1;
             i >= 0 && label_buf[i] == ' '; i--)
            label_buf[i] = '\0';
        snprintf(ev->label, sizeof(ev->label), "%s", label_buf);
    }

    /* Detail: everything after column 23 (index 23+). */
    if (len > 23) {
        const char *d = line + 23;
        while (*d == ' ') d++;
        snprintf(ev->detail, sizeof(ev->detail), "%s", d);
        /* Trim trailing spaces. */
        for (i = (int)strlen(ev->detail) - 1;
             i >= 0 && ev->detail[i] == ' '; i--)
            ev->detail[i] = '\0';
    }

    rec->num_events++;

    /* Extract SIGNON account/caller from detail. */
    if (strcmp(ev->label, "SIGNON") == 0) {
        if (sscanf(ev->detail, "account %d, caller %d",
                   &rec->signon_account, &rec->signon_caller) != 2) {
            rec->signon_account = -1;
            rec->signon_caller = -1;
        }
    }

    /* Extract SIGNOFF reason. */
    if (strcmp(ev->label, "SIGNOFF") == 0) {
        snprintf(rec->signoff_reason, sizeof(rec->signoff_reason),
            "%s", ev->detail);
    }
}

/*
 * Parse user detail line: "Handle, RealName Phone (Status) Country"
 * Content is the trimmed text from the line (after column 13).
 */
static void parse_user_detail(const char *content, struct parsed_record *rec)
{
    const char *comma, *paren_open, *paren_close;
    const char *p;
    int i;

    rec->has_user_detail = 1;

    /* Handle: text before first comma. */
    comma = strchr(content, ',');
    if (comma) {
        int hlen = (int)(comma - content);
        if (hlen >= (int)sizeof(rec->user_handle))
            hlen = (int)sizeof(rec->user_handle) - 1;
        memcpy(rec->user_handle, content, hlen);
        rec->user_handle[hlen] = '\0';
    } else {
        /* No comma -- store entire content as handle. */
        snprintf(rec->user_handle, sizeof(rec->user_handle), "%s", content);
        return;
    }

    /* Verification status: text in parentheses. */
    paren_open = strchr(content, '(');
    paren_close = paren_open ? strchr(paren_open, ')') : NULL;
    if (paren_open && paren_close && paren_close > paren_open + 1) {
        int vlen = (int)(paren_close - paren_open - 1);
        if (vlen >= (int)sizeof(rec->user_verification))
            vlen = (int)sizeof(rec->user_verification) - 1;
        memcpy(rec->user_verification, paren_open + 1, vlen);
        rec->user_verification[vlen] = '\0';
    }

    /* Country: text after closing parenthesis, trimmed. */
    if (paren_close) {
        p = paren_close + 1;
        while (*p == ' ') p++;
        snprintf(rec->user_country, sizeof(rec->user_country), "%s", p);
        for (i = (int)strlen(rec->user_country) - 1;
             i >= 0 && rec->user_country[i] == ' '; i--)
            rec->user_country[i] = '\0';
    }

    /*
     * Between comma and parenthesis: "RealName Phone"
     * Phone is the last token that looks like a phone number (digits and dashes).
     * Scan backwards from the parenthesis to find it.
     */
    if (comma && paren_open && paren_open > comma + 2) {
        const char *segment_start = comma + 1;
        const char *segment_end = paren_open;

        /* Trim leading/trailing spaces from the segment. */
        while (*segment_start == ' ' && segment_start < segment_end)
            segment_start++;
        while (segment_end > segment_start && *(segment_end - 1) == ' ')
            segment_end--;

        /* Find the last space-delimited token as phone number. */
        p = segment_end;
        while (p > segment_start && *(p - 1) != ' ')
            p--;

        if (p > segment_start) {
            /* Phone is from p to segment_end. */
            int plen = (int)(segment_end - p);
            if (plen >= (int)sizeof(rec->user_phone))
                plen = (int)sizeof(rec->user_phone) - 1;
            memcpy(rec->user_phone, p, plen);
            rec->user_phone[plen] = '\0';

            /* Real name is from segment_start to p (trimmed). */
            while (p > segment_start && *(p - 1) == ' ')
                p--;
            {
                int nlen = (int)(p - segment_start);
                if (nlen >= (int)sizeof(rec->user_realname))
                    nlen = (int)sizeof(rec->user_realname) - 1;
                memcpy(rec->user_realname, segment_start, nlen);
                rec->user_realname[nlen] = '\0';
            }
        } else {
            /* Single token -- treat as real name, no phone. */
            int nlen = (int)(segment_end - segment_start);
            if (nlen >= (int)sizeof(rec->user_realname))
                nlen = (int)sizeof(rec->user_realname) - 1;
            memcpy(rec->user_realname, segment_start, nlen);
            rec->user_realname[nlen] = '\0';
        }
    }
}

/*
 * Parse SAM summary line: space-separated "xx:NNN" pairs.
 * Content is the trimmed text from the line (after column 13).
 */
static void parse_sam_line(const char *content, struct parsed_record *rec)
{
    const char *p = content;

    while (*p && rec->num_sam < MAX_SAM_PAIRS) {
        /* Skip whitespace. */
        while (*p == ' ') p++;
        if (!*p) break;

        /* Expect "XX:NNN". */
        if (isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1]) &&
            p[2] == ':' && isdigit((unsigned char)p[3])) {
            struct sam_pair *sp = &rec->sam[rec->num_sam];
            sp->key[0] = p[0];
            sp->key[1] = p[1];
            sp->key[2] = '\0';
            sp->value = atol(p + 3);
            rec->num_sam++;
            /* Advance past this pair. */
            p += 3;
            while (*p && *p != ' ') p++;
        } else {
            /* Not a SAM pair -- skip this token. */
            while (*p && *p != ' ') p++;
        }
    }
}

/*
 * Parse a single record's text into a parsed_record struct.
 * rec_text is null-terminated text of one record (lines separated by \n).
 */
static void parse_record(const char *rec_text, struct parsed_record *rec)
{
    char linebuf[512];
    const char *p = rec_text;
    int first_line = 1;
    int seen_signoff = 0;

    /* Initialize record. */
    memset(rec, 0, sizeof(*rec));
    rec->port = -1;
    rec->signon_account = -1;
    rec->signon_caller = -1;

    while (*p) {
        const char *nl;
        int llen;

        /* Extract one line. */
        nl = strchr(p, '\n');
        llen = nl ? (int)(nl - p) : (int)strlen(p);
        if (llen >= (int)sizeof(linebuf))
            llen = (int)sizeof(linebuf) - 1;
        memcpy(linebuf, p, llen);
        linebuf[llen] = '\0';

        /* Skip empty lines. */
        if (llen == 0) {
            p = nl ? nl + 1 : p + llen;
            continue;
        }

        if (first_line) {
            /* First line should be the header. */
            if (parse_header_line(linebuf, rec))
                first_line = 0;
            /* If it doesn't parse as header, skip. */
            p = nl ? nl + 1 : p + llen;
            continue;
        }

        /* Classify subsequent lines by column layout. */
        if (linebuf[0] != ' ') {
            /* Unexpected non-space in date column after header.
             * Include as generic event with raw text. */
            if (rec->num_events < MAX_EVENTS_PER_RECORD) {
                struct parsed_event *ev =
                    &rec->events[rec->num_events];
                memset(ev, 0, sizeof(*ev));
                snprintf(ev->detail, sizeof(ev->detail), "%s", linebuf);
                rec->num_events++;
            }
        } else if (llen >= 13 && linebuf[7] != ' ') {
            /* Event line: time column has content. */
            parse_event_line(linebuf, rec);
            /* Check if we just saw SIGNOFF. */
            if (rec->num_events > 0 &&
                strcmp(rec->events[rec->num_events - 1].label,
                       "SIGNOFF") == 0) {
                seen_signoff = 1;
            }
        } else {
            /* Detail or SAM line: both date and time columns are blank.
             * Content starts at column 14 (index 13). */
            const char *content = linebuf + 13;
            if (llen > 13) {
                /* Trim leading spaces in content. */
                while (*content == ' ') content++;

                if (seen_signoff && looks_like_sam(content))
                    parse_sam_line(content, rec);
                else if (!rec->has_user_detail && *content)
                    parse_user_detail(content, rec);
                /* else: ignore unexpected extra detail lines */
            }
        }

        p = nl ? nl + 1 : p + llen;
    }
}

/*
 * Emit one parsed record as a JSON object.
 */
static void emit_parsed_record(struct json_state *js,
    const struct parsed_record *rec)
{
    int i;

    json_obj_open(js);

    json_kv_str(js, "date", rec->date);
    json_kv_str(js, "time", rec->time);
    json_kv_int(js, "port", (long)rec->port);
    json_kv_str(js, "connect", rec->connect);

    /* Events array. */
    json_key(js, "events");
    json_arr_open(js);
    for (i = 0; i < rec->num_events; i++) {
        const struct parsed_event *ev = &rec->events[i];
        json_obj_open(js);
        json_kv_str(js, "time", ev->time);
        json_kv_str(js, "event", ev->label);
        json_kv_str(js, "detail", ev->detail);
        json_obj_close(js);
    }
    json_arr_close(js);

    /* User detail. */
    json_key(js, "user");
    if (rec->has_user_detail) {
        json_obj_open(js);
        json_kv_str(js, "handle", rec->user_handle);
        json_kv_str(js, "real_name", rec->user_realname);
        json_kv_str(js, "phone", rec->user_phone);
        json_kv_str(js, "verification", rec->user_verification);
        json_kv_str(js, "country", rec->user_country);
        json_obj_close(js);
    } else {
        json_null(js);
    }

    /* SIGNON account/caller. */
    json_key(js, "signon");
    if (rec->signon_account >= 0) {
        json_obj_open(js);
        json_kv_int(js, "account", (long)rec->signon_account);
        json_kv_int(js, "caller", (long)rec->signon_caller);
        json_obj_close(js);
    } else {
        json_null(js);
    }

    /* Signoff reason. */
    if (rec->signoff_reason[0])
        json_kv_str(js, "signoff_reason", rec->signoff_reason);
    else
        json_kv_null(js, "signoff_reason");

    /* SAM counters. */
    json_key(js, "sam");
    if (rec->num_sam > 0) {
        json_obj_open(js);
        for (i = 0; i < rec->num_sam; i++)
            json_kv_int(js, rec->sam[i].key, rec->sam[i].value);
        json_obj_close(js);
    } else {
        json_null(js);
    }

    json_obj_close(js);
}

/* Maximum number of records to track. */
#define MAX_RECORDS 4000

int cmd_log_callers_parsed(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    BPTR fh;
    long fsize, readsize;
    char *buf;
    char *rec_starts[MAX_RECORDS];
    int nrecs = 0;
    int truncated = 0;
    int last_rec_unterminated = 0;
    int tail = 0;
    int i;
    struct parsed_record rec;

    /* Parse arguments. */
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

    /* Open file under SEM[12] shared lock. */
    ObtainSemaphoreShared(&myp->SEM[12]);

    fh = Open((CONST_STRPTR)"sysdata:log/calls", MODE_OLDFILE);
    if (!fh) {
        ReleaseSemaphore(&myp->SEM[12]);
        json_error("Cannot open log file");
        return 1;
    }

    /* Get file size. */
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);

    if (fsize <= 0) {
        Close(fh);
        ReleaseSemaphore(&myp->SEM[12]);

        /* Empty file. */
        json_init(&js, stdout);
        json_obj_open(&js);
        json_key(&js, "records");
        json_arr_open(&js);
        json_arr_close(&js);
        json_kv_int(&js, "count", 0);
        json_kv_bool(&js, "truncated", 0);
        json_obj_close(&js);
        json_finish(&js);
        return 0;
    }

    /* Cap read size; if file exceeds limit, read only the tail portion. */
    if (fsize > MAX_READ_SIZE) {
        long offset = fsize - MAX_READ_SIZE;
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
            long skip = (long)(nl - buf) + 1;
            memmove(buf, nl + 1, readsize - skip + 1);
            readsize -= skip;
        }
    }

    /* Strip MCI escape sequences in-place. */
    readsize = strip_mci_raw(buf, readsize);

    /*
     * Find record boundaries.
     * A delimiter is a '.' character that is either:
     * - at the start of the buffer followed by '\n'
     * - preceded by '\n' and followed by '\n' or '\0'
     *
     * Termination invariant: each record in rec_starts[] is a
     * null-terminated string. Intermediate records are terminated by
     * setting *nl = '\0' on the '\n' before the next delimiter.
     * The last record extends to the buffer's null terminator from
     * strip_mci_raw().
     */
    {
        char *p = buf;
        while (*p) {
            /* Skip delimiter line(s). */
            while (*p == '.' && (p == buf || *(p - 1) == '\0' ||
                                 *(p - 1) == '\n')) {
                char *nl = strchr(p, '\n');
                if (nl) {
                    p = nl + 1;
                } else {
                    p += strlen(p);
                    break;
                }
            }
            if (!*p) break;

            /* This is the start of a record. */
            if (nrecs < MAX_RECORDS)
                rec_starts[nrecs++] = p;

            /* Find the end of this record (next delimiter line). */
            {
                int found_delim = 0;
                while (*p) {
                    char *nl = strchr(p, '\n');
                    if (!nl) {
                        p += strlen(p);
                        break;
                    }
                    if (*(nl + 1) == '.') {
                        /* Check if next line is just '.' or '.\n'. */
                        char c = *(nl + 2);
                        if (c == '\n' || c == '\0') {
                            *nl = '\0';  /* terminate this record */
                            p = nl + 2;  /* skip past '.' */
                            if (*p == '\n') p++;  /* skip delimiter's newline */
                            found_delim = 1;
                            break;
                        }
                    }
                    p = nl + 1;
                }
                if (!found_delim)
                    last_rec_unterminated = 1;
            }
        }
    }

    if (last_rec_unterminated)
        truncated = 1;

    /*
     * Filter to only parseable records (those with a valid header line).
     * This must happen before --tail, because the raw record splitter
     * may produce fragments from delimiter-like lines within records.
     * Without filtering first, --tail N would count from the end of
     * all fragments, not from the end of real records.
     */
    {
        int valid = 0;
        for (i = 0; i < nrecs; i++) {
            parse_record(rec_starts[i], &rec);
            if (rec.date[0] != '\0')
                rec_starts[valid++] = rec_starts[i];
        }
        nrecs = valid;
    }

    /* Apply --tail filtering on validated records. */
    if (tail > 0 && tail < nrecs) {
        int skip = nrecs - tail;
        memmove(rec_starts, rec_starts + skip,
            (size_t)tail * sizeof(char *));
        nrecs = tail;
    }

    /* Emit JSON output. */
    json_init(&js, stdout);
    json_obj_open(&js);

    json_key(&js, "records");
    json_arr_open(&js);

    int emitted = 0;
    for (i = 0; i < nrecs; i++) {
        parse_record(rec_starts[i], &rec);
        emit_parsed_record(&js, &rec);
        emitted++;
    }

    json_arr_close(&js);
    json_kv_int(&js, "count", (long)emitted);
    json_kv_bool(&js, "truncated", truncated);
    json_obj_close(&js);
    json_finish(&js);

    free(buf);
    return 0;
}
