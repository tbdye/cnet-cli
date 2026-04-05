/*
 * file.c -- File area commands for cnet-cli
 *
 * File area operations: list, show, add, edit, remove, validate, find
 *
 * All commands follow the OneMoreUser / OneLessUser lifecycle:
 * OneMoreUser loads subboard item/header data files (_Items3, _Headers3)
 * into memory; OneLessUser decrements the user count and may unload.
 * Every code path between them must be protected via goto cleanup.
 *
 * File areas use MRK_FILE_TXFER (1) instead of MRK_MSG_BASE (0).
 * ihead.Size holds the actual file size (non-zero for file entries).
 * ItemType3 fields FileName (Title), Downloads, Validated, Finished,
 * Described, InfoX, InfoLen are file-specific.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>

#include "file.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;

/*
 * Maximum text buffer for reading message/description text from _text.
 * Same value as message.c.
 */
#define TEXT_READ_BUF 16384

/* Magic value for HeaderType records in _text. */
#define HEADERTYPE_MAGIC 0xBB25B8C4UL

/*
 * Short description filename.
 * Verify from test upload; may be _Short2 in some CNet versions.
 * All code references use this constant, so only one line changes.
 */
#define SHORT_DESC_FILE "_Short"

/* ---- Helpers copied from message.c (static, not shared) ---- */

/*
 * Build a path to a file under a subboard's data/ directory.
 * Handles AmigaOS path joining: volume: needs no separator,
 * directory/ needs no extra separator.
 */
static void build_data_file_path(char *buf, int bufsz,
    const char *data_path, const char *filename)
{
    int len = (int)strlen(data_path);

    if (len > 0 && (data_path[len - 1] == ':' ||
                     data_path[len - 1] == '/')) {
        snprintf(buf, bufsz, "%sdata/%s", data_path, filename);
    } else {
        snprintf(buf, bufsz, "%s/data/%s", data_path, filename);
    }
}

/*
 * Build the path to the _text file for a subboard.
 */
static void build_text_path(char *buf, int bufsz,
    const char *data_path)
{
    build_data_file_path(buf, bufsz, data_path, "_text");
}

/*
 * Look up a handle from the Key array by account number.
 * Acquires SEM[1] shared for safe concurrent access.
 * Returns a pointer to a static buffer with the handle, or ""
 * if the account is out of range.
 */
static const char *lookup_handle(struct MainPort *myp,
    short account)
{
    static char handle_buf[22];

    handle_buf[0] = '\0';

    ObtainSemaphoreShared(&myp->SEM[1]);
    if (account > 0 && account <= (short)myp->Nums[0]) {
        strncpy(handle_buf, myp->Key[account - 1].Handle,
            sizeof(handle_buf) - 1);
        handle_buf[sizeof(handle_buf) - 1] = '\0';
    }
    ReleaseSemaphore(&myp->SEM[1]);

    return handle_buf;
}

/*
 * Set an IsDate to the current date/time using AmigaOS DateStamp.
 *
 * DateStamp provides days since 1978-01-01 and ticks within the day.
 * We convert to the IsDate format (year offset from 1900).
 */
static void set_current_date(struct IsDate *d)
{
    struct DateStamp ds;
    long days, year, month, day;
    long hour, minute, second;
    long m;
    static const int mdays[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    DateStamp(&ds);

    /* Time from ticks: ds_Minute is minutes since midnight,
     * ds_Tick is ticks (1/50th sec) within the current minute. */
    hour   = ds.ds_Minute / 60;
    minute = ds.ds_Minute % 60;
    second = ds.ds_Tick / 50;

    /* Convert days since 1978-01-01 to year/month/day. */
    days = ds.ds_Days;
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
        int md = mdays[m];
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

    d->Year   = (UBYTE)(year - ISDATE_BASE_YEAR);
    d->Month  = (UBYTE)(month + 1);
    d->Date   = (UBYTE)day;
    d->Hour   = (UBYTE)hour;
    d->Minute = (UBYTE)minute;
    d->Second = (UBYTE)second;
}

/*
 * Read null-terminated text from the _text file at the given offset.
 * Allocates a buffer via malloc; caller must free.
 * Returns NULL on failure (sets *err_msg to an error string).
 *
 * Reads up to TEXT_READ_BUF bytes looking for the null terminator.
 */
static char *read_text(const char *text_path, long offset,
    const char **err_msg)
{
    BPTR fh;
    char *buf;
    long nread;
    int i;

    fh = Open((CONST_STRPTR)text_path, MODE_OLDFILE);
    if (!fh) {
        *err_msg = "Cannot open _text file";
        return NULL;
    }

    if (Seek(fh, offset, OFFSET_BEGINNING) == -1) {
        Close(fh);
        *err_msg = "Seek failed in _text file";
        return NULL;
    }

    buf = (char *)malloc(TEXT_READ_BUF);
    if (!buf) {
        Close(fh);
        *err_msg = "Out of memory";
        return NULL;
    }

    nread = Read(fh, buf, TEXT_READ_BUF - 1);
    Close(fh);

    if (nread < 0) {
        free(buf);
        *err_msg = "Read failed from _text file";
        return NULL;
    }

    /* Ensure null termination -- find the first \0 in the data. */
    for (i = 0; i < nread; i++) {
        if (buf[i] == '\0')
            break;
    }
    /* If no null found in the read data, terminate at end. */
    if (i >= nread) {
        buf[nread] = '\0';
    }

    *err_msg = NULL;
    return buf;
}

/*
 * Read a HeaderType and body text from the _text file.
 *
 * Attempts to read a 288-byte HeaderType at seek_pos. If the magic
 * value validates, follows HeaderType.Text to read the body text.
 * If magic does not match (old-format message), falls back to
 * reading raw text at seek_pos via read_text().
 *
 * Returns:
 *   1 = HeaderType format (out_hdr filled, out_text is body)
 *   0 = Legacy format (out_hdr zeroed, out_text is raw text)
 *  -1 = Error (out_text is NULL)
 */
static int read_header_and_text(const char *text_path, long seek_pos,
    struct HeaderType *out_hdr, char **out_text,
    const char **err_msg)
{
    BPTR fh;
    struct HeaderType hdr_buf;
    long nread;
    char *body_text;

    fh = Open((CONST_STRPTR)text_path, MODE_OLDFILE);
    if (!fh) {
        *err_msg = "Cannot open _text file";
        return -1;
    }

    if (Seek(fh, seek_pos, OFFSET_BEGINNING) == -1) {
        Close(fh);
        *err_msg = "Seek failed in _text file";
        return -1;
    }

    nread = Read(fh, (APTR)&hdr_buf, (long)sizeof(struct HeaderType));
    Close(fh);

    if (nread < (long)sizeof(struct HeaderType))
        goto legacy_fallback;

    if (hdr_buf.Magic != HEADERTYPE_MAGIC)
        goto legacy_fallback;

    /* HeaderType is valid. Read body text from hdr_buf.Text. */
    if (hdr_buf.Text < 0 || hdr_buf.TextLen <= 0) {
        *err_msg = "HeaderType has invalid Text/TextLen";
        return -1;
    }

    body_text = read_text(text_path, hdr_buf.Text, err_msg);
    if (!body_text)
        return -1;

    if (out_hdr)
        *out_hdr = hdr_buf;
    *out_text = body_text;
    *err_msg = NULL;
    return 1;

legacy_fallback:
    if (out_hdr)
        memset(out_hdr, 0, sizeof(struct HeaderType));
    body_text = read_text(text_path, seek_pos, err_msg);
    if (!body_text)
        return -1;
    *out_text = body_text;
    *err_msg = NULL;
    return 0;
}

/*
 * Load the _Message3 file from disk.
 * Returns a malloc'd array of MessageType3 records.
 * Sets *out_count to the number of records loaded.
 * Returns NULL on failure or if no messages exist.
 */
static struct MessageType3 *load_messages(const char *data_path,
    long *out_count)
{
    char msg_path[256];
    BPTR fh;
    long file_size;
    long rec_count;
    struct MessageType3 *msgs;
    long nread;

    build_data_file_path(msg_path, sizeof(msg_path),
        data_path, "_Message3");

    fh = Open((CONST_STRPTR)msg_path, MODE_OLDFILE);
    if (!fh) {
        *out_count = 0;
        return NULL;
    }

    /* Get file size by seeking to end. */
    Seek(fh, 0, OFFSET_END);
    file_size = Seek(fh, 0, OFFSET_BEGINNING);
    if (file_size <= 0) {
        Close(fh);
        *out_count = 0;
        return NULL;
    }

    rec_count = file_size / (long)sizeof(struct MessageType3);
    if (rec_count <= 0) {
        Close(fh);
        *out_count = 0;
        return NULL;
    }

    msgs = (struct MessageType3 *)malloc(
        (unsigned long)rec_count * sizeof(struct MessageType3));
    if (!msgs) {
        Close(fh);
        *out_count = 0;
        return NULL;
    }

    nread = Read(fh, (APTR)msgs,
        rec_count * (long)sizeof(struct MessageType3));
    Close(fh);

    if (nread != rec_count * (long)sizeof(struct MessageType3)) {
        free(msgs);
        *out_count = 0;
        return NULL;
    }

    *out_count = rec_count;
    return msgs;
}

/*
 * Read sub->count, increment it, return the pre-increment value.
 * The returned value is the unique ID for a new item.
 *
 * sub->count is a unique ID sequence (NOT an item count). It is
 * incremented for both new items AND responses. The actual item
 * count is sub->rn.
 *
 * Caller MUST hold sub->sem (ObtainSemaphore) to prevent races.
 */
static ULONG get_next_id(struct SubboardType4 *sub)
{
    ULONG id = sub->count;
    sub->count++;
    return id;
}

/* ---- New helpers for file operations ---- */

/*
 * Read a short description from the _Short file.
 * Returns malloc'd buffer; caller must free. NULL on failure.
 */
static char *read_short_desc(const char *data_path,
    long info_x, long info_len)
{
    char short_path[256];
    BPTR fh;
    char *buf;
    long nread;

    if (info_len <= 0)
        return NULL;

    build_data_file_path(short_path, sizeof(short_path),
        data_path, SHORT_DESC_FILE);

    fh = Open((CONST_STRPTR)short_path, MODE_OLDFILE);
    if (!fh)
        return NULL;

    if (Seek(fh, info_x, OFFSET_BEGINNING) == -1) {
        Close(fh);
        return NULL;
    }

    buf = (char *)malloc((unsigned long)info_len + 1);
    if (!buf) {
        Close(fh);
        return NULL;
    }

    nread = Read(fh, buf, info_len);
    Close(fh);

    if (nread != info_len) {
        free(buf);
        return NULL;
    }

    buf[info_len] = '\0';
    return buf;
}

/*
 * Build the physical file path for a file entry.
 * Uses UDBase{Part}: or sub->ZeroPath for partition 0.
 */
static void build_file_path(char *buf, int bufsz,
    struct SubboardType4 *sub, const char *filename, short part)
{
    if (part == 0 && sub->ZeroPath[0] != '\0') {
        int len = (int)strlen(sub->ZeroPath);
        if (len > 0 && (sub->ZeroPath[len - 1] == ':' ||
                         sub->ZeroPath[len - 1] == '/'))
            snprintf(buf, bufsz, "%s%s", sub->ZeroPath, filename);
        else
            snprintf(buf, bufsz, "%s/%s", sub->ZeroPath, filename);
    } else {
        snprintf(buf, bufsz, "UDBase%d:%s/%s",
            (int)part, sub->SubDirName, filename);
    }
}

/*
 * Verify a physical file exists and get its size.
 * Returns file size (>0) on success, 0 if not found, -1 on error.
 *
 * Uses Lock/Examine instead of CNet FileSize() because FileSize()
 * returns 0 for non-existent files (indistinguishable from empty files).
 *
 * FileInfoBlock is heap-allocated via AllocVec() to guarantee the
 * longword alignment that Examine() requires. Stack allocation is
 * unsafe when packed-struct pragmas are active.
 */
static long verify_file_exists(const char *path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    long size;

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!lock)
        return 0;  /* file not found */

    fib = (struct FileInfoBlock *)AllocVec(
        sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        UnLock(lock);
        return -1;  /* allocation failed */
    }

    if (!Examine(lock, fib)) {
        FreeVec(fib);
        UnLock(lock);
        return -1;  /* error */
    }

    size = fib->fib_Size;
    FreeVec(fib);
    UnLock(lock);
    return size;
}

/* ---- file list ---- */

int cmd_file_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char buf[128];
    char datebuf[24];
    short physnum;
    struct SubboardType4 *sub;
    int marker_base;
    long limit = 0; /* 0 = no limit */
    long offset_arg = 0;
    long i;
    long count;
    long emitted = 0;
    int rc = 0;
    int loaded = 0;

    if (argc < 2) {
        json_error("Usage: cnet-cli file list <sub-id|gokey> "
            "[--limit N] [--offset N]");
        return 1;
    }

    /* Parse optional flags */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            i++;
            limit = atol(argv[i]);
        } else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            i++;
            offset_arg = atol(argv[i]);
        }
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    /* Verify subboard is valid and is a file area. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_FILE_TXFER) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a file area");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /*
     * Load subboard data files.
     *
     * TOCTOU note: same window as message.c -- harmless in practice.
     * See message.c cmd_msg_list for full rationale.
     */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        return 1;
    }
    loaded = 1;

    count = (long)sub->rn;

    json_init(&js, stdout);
    json_obj_open(&js);

    json_kv_str(&js, "subboard",
        strip_mci(buf, sizeof(buf), sub->SubDirName));
    json_kv_int(&js, "physnum", (long)physnum);

    json_key(&js, "items");
    json_arr_open(&js);

    for (i = 0; i < count; i++) {
        struct ItemType3 *item;
        struct ItemHeader *ihead;
        const char *handle;

        /* Apply offset */
        if (i < offset_arg)
            continue;

        /* Apply limit */
        if (limit > 0 && emitted >= limit)
            break;

        /*
         * ZGetItemPtr and ZGetIHeadPtr return direct pointers
         * into the loaded arrays -- read-only, no copy overhead.
         * The item number parameter is 0-based (array index).
         */
        item = ZGetItemPtr(sub, (short)i);
        ihead = ZGetIHeadPtr(sub, (short)i);

        if (!item || !ihead)
            continue;

        handle = lookup_handle(myp, item->ByAccount);

        json_obj_open(&js);

        json_kv_int(&js, "number", (long)ihead->Number);
        json_kv_int(&js, "index", i + 1);
        json_kv_str(&js, "title",
            strip_mci(buf, sizeof(buf), item->Title));
        json_kv_int(&js, "size", ihead->Size);
        json_kv_int(&js, "downloads", item->Downloads);
        json_kv_bool(&js, "validated", (int)item->Validated);
        json_kv_bool(&js, "finished", (int)item->Finished);
        json_kv_bool(&js, "described", (int)item->Described);
        json_kv_bool(&js, "missing_file", (int)item->MissingFile);
        json_kv_int(&js, "by_account", (long)item->ByAccount);
        json_kv_str(&js, "by_handle",
            strip_mci(buf, sizeof(buf), handle));
        if (is_null_date(&ihead->PostDate))
            json_kv_null(&js, "post_date");
        else
            json_kv_str(&js, "post_date",
                format_date(datebuf, sizeof(datebuf),
                    &ihead->PostDate));
        json_kv_bool(&js, "killed", (int)ihead->Killed);
        json_kv_int(&js, "responses", ihead->Responses);

        json_obj_close(&js);
        emitted++;
    }

    json_arr_close(&js);

    json_kv_int(&js, "total", count);

    json_obj_close(&js);
    json_finish(&js);

    goto cleanup; /* normal exit -- keep label used for -Werror */

cleanup:
    if (loaded)
        OneLessUser(sub);
    return rc;
}

/* ---- file show ---- */

int cmd_file_show(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char buf[128];
    char datebuf[24];
    char text_path[256];
    short physnum;
    struct SubboardType4 *sub;
    struct ItemType3 item;
    struct ItemHeader ihead;
    struct HeaderType hdr;
    int marker_base;
    long item_index;
    long count;
    char *text = NULL;
    char *short_desc = NULL;
    const char *err_msg;
    int hdr_result;
    int rc = 0;

    if (argc < 3) {
        json_error("Usage: cnet-cli file show <sub-id|gokey> "
            "<item-number>");
        return 1;
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    item_index = atol(argv[2]);
    if (item_index < 1) {
        json_error("Item number must be >= 1");
        return 1;
    }

    /* Verify subboard is valid and is a file area. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_FILE_TXFER) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a file area");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Load subboard data files. */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        return 1;
    }

    count = (long)sub->rn;
    if (item_index > count) {
        rc = 1;
        json_error("Item number out of range");
        goto cleanup;
    }

    /* ZGetItem copies into caller's buffers. Index is 0-based. */
    memset(&item, 0, sizeof(item));
    memset(&ihead, 0, sizeof(ihead));
    ZGetItem(&item, &ihead, sub, (short)(item_index - 1));

    /* Read short description from _Short if available. */
    if (item.InfoLen > 0) {
        short_desc = read_short_desc(sub->DataPath,
            item.InfoX, item.InfoLen);
    }

    /* Build path to _text file. */
    build_text_path(text_path, sizeof(text_path), sub->DataPath);

    /* Read long description from _text via HeaderType-aware reader. */
    hdr_result = -1;
    memset(&hdr, 0, sizeof(hdr));
    if (item.First >= 0) {
        hdr_result = read_header_and_text(text_path, item.First,
            &hdr, &text, &err_msg);
        /* text may be NULL if file doesn't exist yet -- not fatal. */
    }

    /* Build JSON output. */
    json_init(&js, stdout);
    json_obj_open(&js);

    json_key(&js, "item");
    json_obj_open(&js);

    /* Basic fields */
    json_kv_int(&js, "number", (long)ihead.Number);
    json_kv_int(&js, "index", item_index);
    json_kv_str(&js, "title",
        strip_mci(buf, sizeof(buf), item.Title));
    json_kv_int(&js, "size", ihead.Size);
    json_kv_int(&js, "downloads", item.Downloads);
    json_kv_int(&js, "by_account", (long)item.ByAccount);
    json_kv_str(&js, "by_handle",
        strip_mci(buf, sizeof(buf),
            lookup_handle(myp, item.ByAccount)));

    if (item.ToID != 0) {
        short to_acct = IDToAccount(item.ToID);
        if (to_acct > 0)
            json_kv_str(&js, "to_handle",
                strip_mci(buf, sizeof(buf),
                    lookup_handle(myp, to_acct)));
        else
            json_kv_int(&js, "to_id", item.ToID);
    }

    if (is_null_date(&ihead.PostDate))
        json_kv_null(&js, "post_date");
    else
        json_kv_str(&js, "post_date",
            format_date(datebuf, sizeof(datebuf), &ihead.PostDate));

    json_kv_bool(&js, "killed", (int)ihead.Killed);
    json_kv_int(&js, "responses", ihead.Responses);

    /* File-specific flags */
    json_kv_bool(&js, "validated", (int)item.Validated);
    json_kv_bool(&js, "finished", (int)item.Finished);
    json_kv_bool(&js, "described", (int)item.Described);
    json_kv_bool(&js, "missing_file", (int)item.MissingFile);
    json_kv_int(&js, "partition", (long)item.Part);

    /* Credit/accounting fields */
    json_kv_int(&js, "byte_charges", item.ByteCharges);
    json_kv_int(&js, "file_charges", (long)item.FileCharges);
    json_kv_int(&js, "byte_download", item.ByteDownload);
    json_kv_int(&js, "file_download", (long)item.FileDownload);
    json_kv_int(&js, "file_payback", (long)item.FilePayBack);
    json_kv_int(&js, "byte_payback", item.BytePayBack);
    json_kv_int(&js, "byte_rewards", item.ByteRewards);
    json_kv_int(&js, "file_rewards", item.FileRewards);
    json_kv_int(&js, "best_cps", item.BestCPS);

    /* Behavioral flags */
    json_kv_bool(&js, "private", (int)item.Private);
    json_kv_bool(&js, "dl_notify", (int)item.DLnotifyULer);
    json_kv_bool(&js, "frozen", (int)item.Frozen);
    json_kv_bool(&js, "free", (int)item.Free);
    json_kv_bool(&js, "favorite", (int)item.Favorite);
    json_kv_bool(&js, "transformed", (int)item.Transformed);
    json_kv_bool(&js, "purge_kill", (int)item.PurgeKill);
    json_kv_int(&js, "integrity", (long)item.Integrity);
    json_kv_bool(&js, "auto_grab", (int)item.AutoGrab);
    json_kv_int(&js, "purge_status", (long)item.PurgeStatus);
    json_kv_bool(&js, "virus_checked", (int)item.VirusChecked);
    json_kv_bool(&js, "override", (int)item.override);

    /* Dates */
    if (is_null_date(&item.ShowDate))
        json_kv_null(&js, "show_date");
    else
        json_kv_str(&js, "show_date",
            format_date(datebuf, sizeof(datebuf), &item.ShowDate));

    if (is_null_date(&item.UsedDate))
        json_kv_null(&js, "used_date");
    else
        json_kv_str(&js, "used_date",
            format_date(datebuf, sizeof(datebuf), &item.UsedDate));

    /* _Short info */
    json_kv_int(&js, "info_x", item.InfoX);
    json_kv_int(&js, "info_len", item.InfoLen);

    /* Short description from _Short file */
    if (short_desc)
        json_kv_str(&js, "short_desc", short_desc);
    else
        json_kv_null(&js, "short_desc");

    /* HeaderType metadata (if available from _text). */
    if (hdr_result == 1) {
        if (hdr.By[0])
            json_kv_str(&js, "by_name",
                strip_mci(buf, sizeof(buf), hdr.By));
        if (hdr.ByUser[0])
            json_kv_str(&js, "by_user",
                strip_mci(buf, sizeof(buf), hdr.ByUser));
        if (hdr.ToID != 0) {
            if (hdr.To[0])
                json_kv_str(&js, "to_name",
                    strip_mci(buf, sizeof(buf), hdr.To));
            if (hdr.ToUser[0])
                json_kv_str(&js, "to_user",
                    strip_mci(buf, sizeof(buf), hdr.ToUser));
        }
        if (!is_null_date(&hdr.PostDate))
            json_kv_str(&js, "post_date_header",
                format_date(datebuf, sizeof(datebuf),
                    &hdr.PostDate));
        if (hdr.Organ[0])
            json_kv_str(&js, "org",
                strip_mci(buf, sizeof(buf), hdr.Organ));
    }

    /* Long description text */
    if (text)
        json_kv_str(&js, "text", text);
    else
        json_kv_null(&js, "text");

    /*
     * Responses: load _Message3 from disk and iterate records
     * matching this item.
     *
     * OneMoreUser does NOT load the _Message3 file into sub->NewMess
     * (that pointer remains NULL). We must read it directly from disk.
     */
    json_key(&js, "responses_list");
    json_arr_open(&js);

    if (ihead.Responses > 0) {
        struct MessageType3 *msgs;
        long msg_count = 0;

        msgs = load_messages(sub->DataPath, &msg_count);
        if (msgs && msg_count > 0) {
            long mi;
            long resp_found = 0;

            for (mi = 0; mi < msg_count; mi++) {
                struct MessageType3 *msg = &msgs[mi];
                struct HeaderType resp_hdr;
                char *rtext = NULL;
                int resp_result;

                if (msg->ItemNumber != ihead.Number)
                    continue;

                json_obj_open(&js);

                json_kv_int(&js, "number", (long)msg->Number);

                /* Look up response author by ID. */
                if (msg->ByID != 0) {
                    short by_acct = IDToAccount(msg->ByID);
                    if (by_acct > 0)
                        json_kv_str(&js, "by_handle",
                            strip_mci(buf, sizeof(buf),
                                lookup_handle(myp, by_acct)));
                    else
                        json_kv_int(&js, "by_id", msg->ByID);
                } else {
                    json_kv_null(&js, "by_handle");
                }

                /* Response text via HeaderType-aware reader. */
                resp_result = -1;
                memset(&resp_hdr, 0, sizeof(resp_hdr));
                if (msg->Seek >= 0) {
                    resp_result = read_header_and_text(text_path,
                        msg->Seek, &resp_hdr, &rtext, &err_msg);
                }

                if (rtext) {
                    json_kv_str(&js, "text", rtext);
                    free(rtext);
                } else {
                    json_kv_null(&js, "text");
                }

                /* HeaderType metadata for response (if available). */
                if (resp_result == 1) {
                    if (resp_hdr.By[0])
                        json_kv_str(&js, "by_name",
                            strip_mci(buf, sizeof(buf),
                                resp_hdr.By));
                    if (resp_hdr.ByUser[0])
                        json_kv_str(&js, "by_user",
                            strip_mci(buf, sizeof(buf),
                                resp_hdr.ByUser));
                    if (resp_hdr.ToID != 0) {
                        if (resp_hdr.To[0])
                            json_kv_str(&js, "to_name",
                                strip_mci(buf, sizeof(buf),
                                    resp_hdr.To));
                        if (resp_hdr.ToUser[0])
                            json_kv_str(&js, "to_user",
                                strip_mci(buf, sizeof(buf),
                                    resp_hdr.ToUser));
                    }
                    if (resp_hdr.Organ[0])
                        json_kv_str(&js, "org",
                            strip_mci(buf, sizeof(buf),
                                resp_hdr.Organ));
                }

                if (is_null_date(&msg->PostDate))
                    json_kv_null(&js, "post_date");
                else
                    json_kv_str(&js, "post_date",
                        format_date(datebuf, sizeof(datebuf),
                            &msg->PostDate));

                json_obj_close(&js);

                /* Early exit once all expected responses found. */
                resp_found++;
                if (resp_found >= ihead.Responses)
                    break;
            }
            free(msgs);
        }
    }

    json_arr_close(&js);

    json_obj_close(&js); /* close "item" */
    json_obj_close(&js); /* close root */
    json_finish(&js);

cleanup:
    if (text)
        free(text);
    if (short_desc)
        free(short_desc);
    OneLessUser(sub);
    return rc;
}

/* ---- file add ---- */

int cmd_file_add(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short physnum;
    struct SubboardType4 *sub;
    struct ItemType3 item;
    struct ItemHeader ihead;
    int marker_base;
    const char *title = NULL;
    const char *author_str = NULL;
    const char *desc = NULL;
    short author_acct;
    long author_id = 0;
    char author_handle[24];
    char file_path[256];
    long file_size;
    ULONG new_id;
    int rc = 0;
    int i;
    int loaded = 0;
    int sem_held = 0;

    if (argc < 2) {
        json_error("Usage: cnet-cli file add <sub-id|gokey> "
            "--title \"filename\" --author <account> "
            "[--desc \"Short description\"]");
        return 1;
    }

    /* Parse arguments */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            i++;
            title = argv[i];
        } else if (strcmp(argv[i], "--author") == 0 && i + 1 < argc) {
            i++;
            author_str = argv[i];
        } else if (strcmp(argv[i], "--desc") == 0 && i + 1 < argc) {
            i++;
            desc = argv[i];
        }
    }

    if (!title || !author_str) {
        json_error("Required: --title, --author");
        return 1;
    }

    /*
     * Warn about --desc: _Short file format is unverified.
     * The basic file add (without short description) works,
     * but writing to _Short is deferred until format is
     * empirically verified via a test upload.
     */
    if (desc) {
        warn_add("--desc ignored; _Short file format is unverified");
        desc = NULL;
    }

    author_acct = (short)atol(author_str);
    if (author_acct < 1) {
        json_error("Invalid --author account number");
        return 1;
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    /* Verify file area type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_FILE_TXFER) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a file area");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Look up author identity under SEM[1] shared. */
    author_handle[0] = '\0';
    ObtainSemaphoreShared(&myp->SEM[1]);
    if (author_acct > 0 && author_acct <= (short)myp->Nums[0]) {
        strncpy(author_handle,
            myp->Key[author_acct - 1].Handle,
            sizeof(author_handle) - 1);
        author_handle[sizeof(author_handle) - 1] = '\0';
        author_id = myp->Key[author_acct - 1].IDNumber;
    }
    ReleaseSemaphore(&myp->SEM[1]);

    if (author_id == 0) {
        json_error("Author account not found");
        return 1;
    }

    /*
     * Verify physical file exists on the Amiga.
     * Build path using partition 0 (default).
     */
    build_file_path(file_path, sizeof(file_path), sub, title, 0);
    file_size = verify_file_exists(file_path);
    if (file_size <= 0) {
        char errbuf[300];
        if (file_size == 0)
            snprintf(errbuf, sizeof(errbuf),
                "File not found: %s", file_path);
        else
            snprintf(errbuf, sizeof(errbuf),
                "Error checking file: %s", file_path);
        json_error(errbuf);
        return 1;
    }

    /* Load subboard data. */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        return 1;
    }
    loaded = 1;

    /* Protect count increment and ZAddItem. */
    ObtainSemaphore(sub->sem);
    sem_held = 1;

    new_id = get_next_id(sub);

    /* Prepare ItemType3 with file-specific fields. */
    memset(&item, 0, sizeof(item));
    strncpy(item.Title, title, sizeof(item.Title) - 1);
    item.Title[sizeof(item.Title) - 1] = '\0';
    item.ByAccount = author_acct;
    item.ByID = author_id;
    item.ToID = 0;
    item.Part = 0;
    item.Downloads = 0;
    item.Validated = 1;     /* sysop-added files are pre-validated */
    item.Finished = 1;      /* file already on disk */
    item.Described = 0;     /* no short desc (deferred) */
    item.PurgeKill = 1;
    item.PurgeStatus = sub->PurgeStatus;
    item.DLnotifyULer = sub->DLnotifyULer;
    item.override = sub->override;
    item.First = -1;        /* no long description */
    item.Last = -1;
    item.InfoX = 0;
    item.InfoLen = 0;
    set_current_date(&item.ShowDate);

    /* Prepare ItemHeader. */
    memset(&ihead, 0, sizeof(ihead));
    ihead.Number = new_id;
    ihead.Size = file_size;  /* must be > 0 for files */
    ihead.Responses = 0;
    set_current_date(&ihead.PostDate);
    set_current_date(&ihead.RespDate);

    /* Build TitleSort: uppercase first 8 chars of filename. */
    {
        int ti;
        for (ti = 0; ti < 8 && title[ti]; ti++)
            ihead.TitleSort[ti] =
                (UBYTE)toupper((unsigned char)title[ti]);
        ihead.TitleSort[ti] = '\0';
    }

    ihead.Killed = 0;

    /* Add item to subboard. */
    if (!ZAddItem(&item, &ihead, sub)) {
        json_error("ZAddItem failed");
        rc = 1;
        goto sem_release;
    }

    /* Persist updated subboard to disk. */
    ObtainSemaphore(&myp->SEM[5]);
    write_subboard_disk((int)physnum, sub);
    ReleaseSemaphore(&myp->SEM[5]);

    /* Output confirmation. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "added");
    json_kv_int(&js, "physnum", (long)physnum);
    json_kv_int(&js, "item_count", (long)sub->rn);
    json_kv_int(&js, "next_id", (long)sub->count);
    json_kv_str(&js, "title", title);
    json_kv_int(&js, "size", file_size);
    json_kv_int(&js, "by_account", (long)author_acct);
    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

sem_release:
    if (sem_held) {
        ReleaseSemaphore(sub->sem);
        sem_held = 0;
    }

    if (loaded)
        OneLessUser(sub);
    return rc;
}

/* ---- file edit ---- */

int cmd_file_edit(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char buf[128];
    short physnum;
    struct SubboardType4 *sub;
    struct ItemType3 item;
    struct ItemHeader ihead;
    int marker_base;
    long item_index;
    long count;
    int rc = 0;
    int loaded = 0;
    int changed = 0;
    int desc_skipped = 0;
    int i;

    /* Optional field values; -1 = not specified */
    int set_validated = -1;
    int set_frozen = -1;
    int set_free = -1;
    int set_private = -1;
    int set_missing = -1;
    int set_purge_status = -1;
    const char *set_desc = NULL;

    if (argc < 3) {
        json_error("Usage: cnet-cli file edit <sub-id|gokey> "
            "<item-number> [--validated 0|1] [--frozen 0|1] "
            "[--free 0|1] [--private 0|1] [--missing 0|1] "
            "[--purge-status N] [--desc \"...\"]");
        return 1;
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    item_index = atol(argv[2]);
    if (item_index < 1) {
        json_error("Item number must be >= 1");
        return 1;
    }

    /* Parse optional flags */
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--validated") == 0 && i + 1 < argc) {
            i++;
            set_validated = atoi(argv[i]);
        } else if (strcmp(argv[i], "--frozen") == 0 && i + 1 < argc) {
            i++;
            set_frozen = atoi(argv[i]);
        } else if (strcmp(argv[i], "--free") == 0 && i + 1 < argc) {
            i++;
            set_free = atoi(argv[i]);
        } else if (strcmp(argv[i], "--private") == 0 && i + 1 < argc) {
            i++;
            set_private = atoi(argv[i]);
        } else if (strcmp(argv[i], "--missing") == 0 && i + 1 < argc) {
            i++;
            set_missing = atoi(argv[i]);
        } else if (strcmp(argv[i], "--purge-status") == 0 &&
                   i + 1 < argc) {
            i++;
            set_purge_status = atoi(argv[i]);
            if (set_purge_status < 0 || set_purge_status > 4) {
                json_error("--purge-status must be 0-4");
                return 1;
            }
        } else if (strcmp(argv[i], "--desc") == 0 && i + 1 < argc) {
            i++;
            set_desc = argv[i];
        }
    }

    /*
     * Warn about --desc: _Short file format is unverified.
     */
    if (set_desc) {
        warn_add("--desc ignored; _Short file format is unverified");
        set_desc = NULL;
        desc_skipped = 1;
    }

    /* Verify file area type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_FILE_TXFER) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a file area");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Load subboard data. */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        return 1;
    }
    loaded = 1;

    count = (long)sub->rn;
    if (item_index > count) {
        json_error("Item number out of range");
        rc = 1;
        goto cleanup;
    }

    /* Get a copy of the item. */
    memset(&item, 0, sizeof(item));
    memset(&ihead, 0, sizeof(ihead));
    ZGetItem(&item, &ihead, sub, (short)(item_index - 1));

    /* Apply requested field changes. */
    if (set_validated >= 0) {
        item.Validated = (UBYTE)set_validated;
        changed = 1;
    }
    if (set_frozen >= 0) {
        item.Frozen = (UBYTE)set_frozen;
        changed = 1;
    }
    if (set_free >= 0) {
        item.Free = (UBYTE)set_free;
        changed = 1;
    }
    if (set_private >= 0) {
        item.Private = (UBYTE)set_private;
        changed = 1;
    }
    if (set_missing >= 0) {
        item.MissingFile = (UBYTE)set_missing;
        changed = 1;
    }
    if (set_purge_status >= 0) {
        item.PurgeStatus = (UBYTE)set_purge_status;
        changed = 1;
    }

    if (!changed && !desc_skipped) {
        json_error("No fields to change");
        rc = 1;
        goto cleanup;
    }

    if (!changed && desc_skipped) {
        /* Only --desc was passed and it was skipped. Return success
         * with a note rather than a confusing "No fields to change"
         * error on top of the stderr warning. */
        json_init(&js, stdout);
        json_obj_open(&js);
        json_kv_str(&js, "status", "no_change");
        json_kv_str(&js, "note",
            "--desc was skipped (_Short format unverified); "
            "no other fields specified");
        json_obj_close(&js);
        json_finish(&js);
        goto cleanup;
    }

    /* Write back. */
    ZPutItem(&item, &ihead, sub, (short)(item_index - 1));

    /* Output confirmation. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "edited");
    json_kv_int(&js, "physnum", (long)physnum);
    json_kv_int(&js, "item_index", item_index);
    json_kv_int(&js, "item_number", (long)ihead.Number);
    json_kv_str(&js, "title",
        strip_mci(buf, sizeof(buf), item.Title));
    json_kv_bool(&js, "validated", (int)item.Validated);
    json_kv_bool(&js, "frozen", (int)item.Frozen);
    json_kv_bool(&js, "free", (int)item.Free);
    json_kv_bool(&js, "private", (int)item.Private);
    json_kv_bool(&js, "missing_file", (int)item.MissingFile);
    json_kv_int(&js, "purge_status", (long)item.PurgeStatus);
    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

cleanup:
    if (loaded)
        OneLessUser(sub);
    return rc;
}

/* ---- file remove ---- */

int cmd_file_remove(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char buf[128];
    short physnum;
    struct SubboardType4 *sub;
    struct ItemType3 item;
    struct ItemHeader ihead;
    int marker_base;
    long item_index;
    long count;
    int delete_physical = 0;
    int file_deleted = 0;
    int rc = 0;
    int loaded = 0;
    int i;

    if (argc < 3) {
        json_error("Usage: cnet-cli file remove <sub-id|gokey> "
            "<item-number> [--delete-physical]");
        return 1;
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    item_index = atol(argv[2]);
    if (item_index < 1) {
        json_error("Item number must be >= 1");
        return 1;
    }

    /* Parse optional flags */
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--delete-physical") == 0) {
            delete_physical = 1;
        }
    }

    /* Verify file area type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_FILE_TXFER) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a file area");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Load subboard data. */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        return 1;
    }
    loaded = 1;

    count = (long)sub->rn;
    if (item_index > count) {
        json_error("Item number out of range");
        rc = 1;
        goto cleanup;
    }

    /* Get a copy of the item. */
    memset(&item, 0, sizeof(item));
    memset(&ihead, 0, sizeof(ihead));
    ZGetItem(&item, &ihead, sub, (short)(item_index - 1));

    /* Check if already killed. */
    if (ihead.Killed) {
        json_error("Item is already killed");
        rc = 1;
        goto cleanup;
    }

    /* Mark as killed. */
    ihead.Killed = 1;

    /* Write back. */
    ZPutItem(&item, &ihead, sub, (short)(item_index - 1));

    /* Optionally delete the physical file. */
    if (delete_physical) {
        char phys_path[256];
        build_file_path(phys_path, sizeof(phys_path),
            sub, item.Title, item.Part);
        if (DeleteFile((CONST_STRPTR)phys_path))
            file_deleted = 1;
        else
            {
                char wbuf[128];
                snprintf(wbuf, sizeof(wbuf),
                    "Could not delete physical file: %s", phys_path);
                warn_add(wbuf);
            }
    }

    /* Output confirmation. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "removed");
    json_kv_int(&js, "physnum", (long)physnum);
    json_kv_int(&js, "item_index", item_index);
    json_kv_int(&js, "item_number", (long)ihead.Number);
    json_kv_str(&js, "title",
        strip_mci(buf, sizeof(buf), item.Title));
    if (delete_physical)
        json_kv_bool(&js, "file_deleted", file_deleted);
    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

cleanup:
    if (loaded)
        OneLessUser(sub);
    return rc;
}

/* ---- file validate ---- */

int cmd_file_validate(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short physnum;
    struct SubboardType4 *sub;
    int marker_base;
    long start = 0;
    long end = 0;
    long i;
    long validated_count = 0;
    long total_in_range = 0;
    long count;
    int rc = 0;
    int loaded = 0;
    const char *range_str;

    if (argc < 3) {
        json_error("Usage: cnet-cli file validate <sub-id|gokey> "
            "<item-range> (number, N-M, or 'all')");
        return 1;
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    range_str = argv[2];

    /* Verify file area type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_FILE_TXFER) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a file area");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Load subboard data. */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        return 1;
    }
    loaded = 1;

    count = (long)sub->rn;

    /* Parse item range. */
    if (strcasecmp(range_str, "all") == 0) {
        start = 1;
        end = count;
    } else {
        const char *dash = strchr(range_str, '-');
        if (dash && dash != range_str && dash[1] != '\0') {
            /* Range: N-M */
            char left_buf[16];
            int left_len = (int)(dash - range_str);
            if (left_len >= (int)sizeof(left_buf))
                left_len = (int)sizeof(left_buf) - 1;
            strncpy(left_buf, range_str, (unsigned)left_len);
            left_buf[left_len] = '\0';

            if (!all_digits(left_buf) || !all_digits(dash + 1)) {
                json_error("Invalid range: expected number, "
                    "range (N-M), or 'all'");
                rc = 1;
                goto cleanup;
            }
            start = atol(left_buf);
            end = atol(dash + 1);
        } else {
            /* Single number */
            if (!all_digits(range_str)) {
                json_error("Invalid range: expected number, "
                    "range (N-M), or 'all'");
                rc = 1;
                goto cleanup;
            }
            start = atol(range_str);
            end = start;
        }
    }

    /* Validate parsed range. */
    if (start < 1 || end < start) {
        json_error("Invalid item range");
        rc = 1;
        goto cleanup;
    }
    if (end > count) {
        json_error("Item range exceeds subboard item count");
        rc = 1;
        goto cleanup;
    }

    /* Process items in range. */
    for (i = start; i <= end; i++) {
        struct ItemType3 vitem;
        struct ItemHeader vihead;

        memset(&vitem, 0, sizeof(vitem));
        memset(&vihead, 0, sizeof(vihead));
        ZGetItem(&vitem, &vihead, sub, (short)(i - 1));

        total_in_range++;

        /* Skip killed items. */
        if (vihead.Killed)
            continue;

        /* Skip already validated items. */
        if (vitem.Validated)
            continue;

        vitem.Validated = 1;
        ZPutItem(&vitem, &vihead, sub, (short)(i - 1));
        validated_count++;
    }

    /* Output confirmation. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "validated");
    json_kv_int(&js, "physnum", (long)physnum);
    json_kv_int(&js, "validated", validated_count);
    json_kv_int(&js, "total_in_range", total_in_range);
    json_kv_int(&js, "range_start", start);
    json_kv_int(&js, "range_end", end);
    json_obj_close(&js);
    json_finish(&js);

cleanup:
    if (loaded)
        OneLessUser(sub);
    return rc;
}

/* ---- file find ---- */

int cmd_file_find(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char buf[128];
    char datebuf[24];
    const char *query = NULL;
    const char *sub_arg = NULL;
    const char *field_str = NULL;
    long limit = 100;
    int field_desc = 0;
    int field_uploader = 0;
    int total_matches = 0;
    int subs_searched = 0;
    int i;

    /*
     * Physnum list: collect target subboards first, then iterate.
     * Maximum 256 subboards is generous for any CNet BBS.
     */
#define FIND_MAX_SUBS 256
    short physnums[FIND_MAX_SUBS];
    int nsubs = 0;
    short skipped[FIND_MAX_SUBS];
    int nskipped = 0;

    if (argc < 2) {
        json_error("Usage: cnet-cli file find <query> "
            "[--sub <id|gokey>] [--limit N] "
            "[--field filename|description|uploader]");
        return 1;
    }

    query = argv[1];

    /* Parse optional flags. */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--sub") == 0 && i + 1 < argc) {
            i++;
            sub_arg = argv[i];
        } else if (strcmp(argv[i], "--limit") == 0 &&
                i + 1 < argc) {
            i++;
            limit = atol(argv[i]);
        } else if (strcmp(argv[i], "--field") == 0 &&
                i + 1 < argc) {
            i++;
            field_str = argv[i];
        }
    }

    if (limit <= 0)
        limit = 100;

    if (field_str) {
        if (strcmp(field_str, "description") == 0)
            field_desc = 1;
        else if (strcmp(field_str, "uploader") == 0)
            field_uploader = 1;
        /* default "filename" needs no flag */
    }

    /* Build list of target subboards. */
    if (sub_arg) {
        short pn = resolve_subboard(myp, sub_arg);
        if (pn < 0) {
            json_error("Subboard not found");
            return 1;
        }

        ObtainSemaphoreShared(&myp->SEM[5]);
        if (pn < (short)myp->ns) {
            struct SubboardType4 *s = &myp->Subboard[pn];
            int mb = s->Marker & MRK_SUBBOARD_BASE;
            if (mb != MRK_FILE_TXFER) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Subboard is not a file area");
                return 1;
            }
        }
        ReleaseSemaphore(&myp->SEM[5]);

        physnums[0] = pn;
        nsubs = 1;
    } else {
        ObtainSemaphoreShared(&myp->SEM[5]);
        for (i = 0; i < (int)myp->ns && nsubs < FIND_MAX_SUBS;
                i++) {
            struct SubboardType4 *s = &myp->Subboard[i];
            int mb = s->Marker & MRK_SUBBOARD_BASE;
            if (s->Marker & MRK_SUBBOARD_KILLED)
                continue;
            if (mb != MRK_FILE_TXFER)
                continue;
            if (s->Subdirectory)
                continue;
            physnums[nsubs++] = (short)i;
        }
        ReleaseSemaphore(&myp->SEM[5]);
    }

    /* Begin JSON output. */
    json_init(&js, stdout);
    json_obj_open(&js);

    json_kv_str(&js, "query", query);
    json_kv_str(&js, "field",
        field_desc ? "description" :
            (field_uploader ? "uploader" : "filename"));
    json_kv_int(&js, "limit", limit);

    json_key(&js, "matches");
    json_arr_open(&js);

    /* Iterate target subboards. */
    for (i = 0; i < nsubs && total_matches < (int)limit; i++) {
        struct SubboardType4 *s;
        long si;
        long sub_count;

        s = &myp->Subboard[physnums[i]];

        if (!OneMoreUser(s, (UBYTE)0)) {
            if (nskipped < FIND_MAX_SUBS)
                skipped[nskipped++] = physnums[i];
            continue;
        }

        subs_searched++;
        sub_count = (long)s->rn;

        for (si = 0; si < sub_count &&
                total_matches < (int)limit; si++) {
            struct ItemType3 *ip;
            struct ItemHeader *ih;
            const char *handle;
            int match = 0;

            ip = ZGetItemPtr(s, (short)si);
            ih = ZGetIHeadPtr(s, (short)si);

            if (!ip || !ih)
                continue;
            if (ih->Killed)
                continue;

            /* Match based on field. */
            if (field_desc) {
                /* Description search via _Short file. */
                if (ip->InfoLen > 0) {
                    char *desc = read_short_desc(
                        s->DataPath, ip->InfoX, ip->InfoLen);
                    if (desc) {
                        match = ci_contains(desc, query);
                        free(desc);
                    }
                }
            } else if (field_uploader) {
                match = ci_contains(
                    lookup_handle(myp, ip->ByAccount), query);
            } else {
                /* Default: filename search. */
                match = ci_contains(ip->Title, query);
            }

            if (!match)
                continue;

            /* Emit match. */
            handle = lookup_handle(myp, ip->ByAccount);

            json_obj_open(&js);
            json_kv_int(&js, "physnum", (long)physnums[i]);
            json_kv_str(&js, "subboard",
                strip_mci(buf, sizeof(buf), s->SubDirName));
            json_kv_int(&js, "item_index", si + 1);
            json_kv_int(&js, "item_number", (long)ih->Number);
            json_kv_str(&js, "title",
                strip_mci(buf, sizeof(buf), ip->Title));
            json_kv_int(&js, "size", ih->Size);
            json_kv_int(&js, "downloads", ip->Downloads);
            json_kv_bool(&js, "validated", (int)ip->Validated);
            json_kv_int(&js, "by_account",
                (long)ip->ByAccount);
            json_kv_str(&js, "by_handle",
                strip_mci(buf, sizeof(buf), handle));
            if (is_null_date(&ih->PostDate))
                json_kv_null(&js, "post_date");
            else
                json_kv_str(&js, "post_date",
                    format_date(datebuf, sizeof(datebuf),
                        &ih->PostDate));
            json_obj_close(&js);

            total_matches++;
        }

        OneLessUser(s);
    }

    json_arr_close(&js);

    json_key(&js, "skipped");
    json_arr_open(&js);
    {
        int si;
        for (si = 0; si < nskipped; si++)
            json_int(&js, (long)skipped[si]);
    }
    json_arr_close(&js);

    json_kv_int(&js, "subboards_searched", (long)subs_searched);
    json_kv_int(&js, "total_matches", (long)total_matches);

    json_obj_close(&js);
    json_finish(&js);

    return 0;
}
