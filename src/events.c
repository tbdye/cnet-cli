/*
 * events.c -- Event commands for cnet-cli
 *
 * Scheduled event listing and display. Reads cnet:configs/events.cfg
 * under MainPort->eventsem shared lock. Parse sequential JobType4 records.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#include <cnet/eventdefs.h>
#include <cnet/dates.h>
#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/cnet.h>

#include "events.h"
#include "json.h"
#include "util.h"

extern struct Library *CNetBase;

/* ---- label arrays ---- */

#define NUM_TYPE_LABELS   23
#define NUM_STATUS_LABELS  4
#define NUM_INVOKE_LABELS  4
#define NUM_PORT_LABELS    3

static const char *type_labels[NUM_TYPE_LABELS] = {
    "RunCNetC",       /*  0 EVENT_RUNCNETC   */
    "RunARexx",       /*  1 EVENT_RUNAREXX   */
    "RunDOS",         /*  2 EVENT_RUNDOS     */
    "ReadFile",       /*  3 EVENT_READFILE   */
    "DOS-CMD",        /*  4 EVENT_DOSCMD     */
    "ClosePort",      /*  5 EVENT_CLOSEPORT  */
    "Charges#",       /*  6 EVENT_CHARGES    */
    "LogonBPS",       /*  7 EVENT_LOGONBPS   */
    "DloadBPS",       /*  8 EVENT_DLOADBPS   */
    "ULoadBPS",       /*  9 EVENT_ULOADBPS   */
    "LogonAccess",    /* 10 EVENT_LOGONACC   */
    "XfersAccess",    /* 11 EVENT_XFERSACC   */
    "DoorsAccess",    /* 12 EVENT_PFILEACC   */
    "Modem#",         /* 13 EVENT_MODEMNUM   */
    "CallBack",       /* 14 EVENT_CALLBACK   */
    "Avalid#",        /* 15 EVENT_AVALIDNUM  */
    "Doors",          /* 16 EVENT_PFILES     */
    "Files",          /* 17 EVENT_UDBASE     */
    "MsgArea",        /* 18 EVENT_BASE       */
    "NewUsers",       /* 19 EVENT_NEWUSERS   */
    "SysopIn",        /* 20 EVENT_SYSOPIN    */
    "JoinLink",       /* 21 EVENT_JOINLINK   */
    "On-Line"         /* 22 EVENT_ONLINE     */
};

static const char *status_labels[NUM_STATUS_LABELS] = {
    "Ready", "Suspended", "Once/Delete", "Cancelled"
};

static const char *invoke_labels[NUM_INVOKE_LABELS] = {
    "Immediate-NoDump",
    "Immediate-UDump",
    "If Port Idle",
    "If User Online"
};

static const char *day_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *port_labels[NUM_PORT_LABELS] = {
    "Ignore", "Run", "Run/Close"
};

/* ---- helpers ---- */

static const char *event_type_label(UBYTE type)
{
    if (type >= NUM_TYPE_LABELS)
        return "Unknown";
    return type_labels[type];
}

static const char *event_status_label(UBYTE status)
{
    if (status >= NUM_STATUS_LABELS)
        return "Unknown";
    return status_labels[status];
}

static const char *invoke_type_label(LONG invoke)
{
    short bit;

    if (invoke == 0)
        return "Unknown";

    bit = BitPosition(invoke);
    if (bit < 0 || bit >= NUM_INVOKE_LABELS)
        return "Unknown";
    return invoke_labels[bit];
}

static int invoke_bit_index(LONG invoke)
{
    short bit;

    if (invoke == 0)
        return -1;

    bit = BitPosition(invoke);
    if (bit < 0 || bit >= NUM_INVOKE_LABELS)
        return -1;
    return (int)bit;
}

/*
 * Convert Amiga epoch seconds (since 1978-01-01) to ISO 8601 string.
 * Returns buf on success, NULL if seconds == 0 (meaning "never").
 */
static char *format_cnet_timestamp(char *buf, int bufsz, ULONG seconds)
{
    ULONG days, secs_in_day;
    int year, month, day;
    int hour, minute, second;
    int leap;

    /* Days in each month for non-leap and leap years */
    static const int mdays[2][12] = {
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
        { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
    };

    if (seconds == 0 || bufsz < 20)
        return NULL;

    days = seconds / SECONDS_IN_DAY;
    secs_in_day = seconds % SECONDS_IN_DAY;

    hour = (int)(secs_in_day / SECONDS_IN_HOUR);
    secs_in_day %= SECONDS_IN_HOUR;
    minute = (int)(secs_in_day / SECONDS_IN_MINUTE);
    second = (int)(secs_in_day % SECONDS_IN_MINUTE);

    /* Convert days since 1978-01-01 to year/month/day */
    year = 1978;
    for (;;) {
        leap = ((year % 4 == 0) && (year % 100 != 0)) ||
               (year % 400 == 0);
        if (days < (ULONG)(365 + leap))
            break;
        days -= (ULONG)(365 + leap);
        year++;
    }

    leap = ((year % 4 == 0) && (year % 100 != 0)) ||
           (year % 400 == 0);
    month = 0;
    while (month < 11 && days >= (ULONG)mdays[leap][month]) {
        days -= (ULONG)mdays[leap][month];
        month++;
    }
    day = (int)days + 1;
    month += 1; /* 1-based */

    snprintf(buf, bufsz, "%04d-%02d-%02dT%02d:%02d:%02d",
        year, month, day, hour, minute, second);

    return buf;
}

static void emit_days_array(struct json_state *js, UBYTE days)
{
    int i;

    json_arr_open(js);
    for (i = 0; i < 7; i++) {
        if (days & (1 << i))
            json_str(js, day_names[i]);
    }
    json_arr_close(js);
}

/*
 * Format repeat interval in seconds as human-readable "Nd Nh Nm".
 * Returns "none" if repeat <= 0.
 */
static char *format_repeat(char *buf, int bufsz, LONG repeat)
{
    int d, h, m;
    LONG rem;

    if (repeat <= 0) {
        snprintf(buf, bufsz, "none");
        return buf;
    }

    d = (int)(repeat / SECONDS_IN_DAY);
    rem = repeat % SECONDS_IN_DAY;
    h = (int)(rem / SECONDS_IN_HOUR);
    rem %= SECONDS_IN_HOUR;
    m = (int)(rem / SECONDS_IN_MINUTE);

    snprintf(buf, bufsz, "%dd %dh %dm", d, h, m);
    return buf;
}

static const char *port_action_label(UBYTE runport)
{
    if (runport >= NUM_PORT_LABELS)
        return "Unknown";
    return port_labels[runport];
}

/* ---- commands ---- */

int cmd_event_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct JobType4 job;
    BPTR fh;
    int index = 0;
    int emitted_count = 0;
    int show_all = 0;
    int i;
    char ts_buf[24];

    /* Parse flags */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0)
            show_all = 1;
    }

    ObtainSemaphoreShared(&myp->eventsem);

    fh = Open((CONST_STRPTR)"cnet:configs/events.cfg", MODE_OLDFILE);
    if (!fh) {
        /* No events file -- emit empty result */
        json_init(&js, stdout);
        json_obj_open(&js);
        json_key(&js, "events");
        json_arr_open(&js);
        json_arr_close(&js);
        json_kv_int(&js, "count", 0);
        json_obj_close(&js);
        json_finish(&js);
        ReleaseSemaphore(&myp->eventsem);
        return 0;
    }

    json_init(&js, stdout);
    json_obj_open(&js);
    json_key(&js, "events");
    json_arr_open(&js);

    while (Read(fh, &job, sizeof(struct JobType4))
            == sizeof(struct JobType4)) {
        if (!show_all && job.deleted) {
            index++;
            continue;
        }

        json_obj_open(&js);
        json_kv_int(&js, "index", (long)index);
        json_kv_str(&js, "name", job.Name);
        json_kv_str(&js, "type", event_type_label(job.type));
        json_kv_int(&js, "type_id", (long)job.type);
        json_kv_str(&js, "status", event_status_label(job.status));
        json_kv_int(&js, "status_id", (long)job.status);
        json_kv_str(&js, "invoke", invoke_type_label(job.invoke));
        json_kv_int(&js, "invoke_raw", job.invoke);

        if (format_cnet_timestamp(ts_buf, sizeof(ts_buf),
                job.StartTime))
            json_kv_str(&js, "start_time", ts_buf);
        else
            json_kv_null(&js, "start_time");
        json_kv_uint(&js, "start_time_raw",
            (unsigned long)job.StartTime);

        json_kv_int(&js, "repeat_seconds", job.repeat);
        json_kv_bool(&js, "deleted", (int)job.deleted);
        json_kv_bool(&js, "enabled",
            job.status == 0 && job.deleted == 0);
        json_obj_close(&js);

        emitted_count++;
        index++;
    }

    json_arr_close(&js);
    json_kv_int(&js, "count", (long)emitted_count);
    json_obj_close(&js);
    json_finish(&js);

    Close(fh);
    ReleaseSemaphore(&myp->eventsem);
    return 0;
}

int cmd_event_show(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct JobType4 job;
    BPTR fh;
    long event_index;
    long offset;
    long fsize;
    long max_events;
    char ts_buf[24];
    char repeat_buf[32];
    int inv_id;

    if (argc < 2 || !all_digits(argv[1])) {
        json_error("Usage: cnet-cli event show <index>");
        return 1;
    }

    event_index = atol(argv[1]);
    if (event_index < 0) {
        json_error("Event index must be >= 0");
        return 1;
    }

    ObtainSemaphoreShared(&myp->eventsem);

    fh = Open((CONST_STRPTR)"cnet:configs/events.cfg", MODE_OLDFILE);
    if (!fh) {
        ReleaseSemaphore(&myp->eventsem);
        json_error("No events configured");
        return 1;
    }

    /* Check file size for bounds validation */
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);
    max_events = fsize / (long)sizeof(struct JobType4);

    if (event_index >= max_events) {
        Close(fh);
        ReleaseSemaphore(&myp->eventsem);
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "Event index %ld out of range (0-%ld)",
                event_index, max_events - 1);
            json_error(buf);
        }
        return 1;
    }

    /* Seek to the requested record */
    offset = event_index * (long)sizeof(struct JobType4);
    Seek(fh, offset, OFFSET_BEGINNING);

    if (Read(fh, &job, sizeof(struct JobType4))
            != sizeof(struct JobType4)) {
        Close(fh);
        ReleaseSemaphore(&myp->eventsem);
        json_error("Event not found");
        return 1;
    }

    Close(fh);
    ReleaseSemaphore(&myp->eventsem);

    /* Emit full detail JSON */
    inv_id = invoke_bit_index(job.invoke);

    json_init(&js, stdout);
    json_obj_open(&js);
    json_key(&js, "event");
    json_obj_open(&js);

    json_kv_int(&js, "index", event_index);
    json_kv_str(&js, "name", job.Name);
    json_kv_str(&js, "command", job.args);
    json_kv_str(&js, "ports", job.ports);

    json_kv_str(&js, "type", event_type_label(job.type));
    json_kv_int(&js, "type_id", (long)job.type);

    json_kv_str(&js, "status", event_status_label(job.status));
    json_kv_int(&js, "status_id", (long)job.status);

    json_kv_str(&js, "invoke", invoke_type_label(job.invoke));
    if (inv_id >= 0)
        json_kv_int(&js, "invoke_id", (long)inv_id);
    else
        json_kv_null(&js, "invoke_id");
    json_kv_int(&js, "invoke_raw", job.invoke);

    if (format_cnet_timestamp(ts_buf, sizeof(ts_buf), job.StartTime))
        json_kv_str(&js, "start_time", ts_buf);
    else
        json_kv_null(&js, "start_time");
    json_kv_uint(&js, "start_time_raw",
        (unsigned long)job.StartTime);

    if (format_cnet_timestamp(ts_buf, sizeof(ts_buf),
            job.DateExecuted))
        json_kv_str(&js, "last_executed", ts_buf);
    else
        json_kv_null(&js, "last_executed");
    json_kv_uint(&js, "last_executed_raw",
        (unsigned long)job.DateExecuted);

    json_kv_int(&js, "valid_seconds", job.valid);

    json_key(&js, "days");
    emit_days_array(&js, job.Days);
    json_kv_int(&js, "days_raw", (long)job.Days);

    json_kv_bool(&js, "deleted", (int)job.deleted);

    json_kv_int(&js, "repeat_seconds", job.repeat);
    json_kv_str(&js, "repeat_human",
        format_repeat(repeat_buf, sizeof(repeat_buf), job.repeat));

    json_kv_str(&js, "port_action", port_action_label(job.runport));
    json_kv_int(&js, "port_action_id", (long)job.runport);

    json_obj_close(&js);
    json_obj_close(&js);
    json_finish(&js);
    return 0;
}
