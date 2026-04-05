/*
 * news.c -- News/GFile/PFile (text/door) commands for cnet-cli
 *
 * News operations: list, read, post, edit, delete
 *
 * All commands follow the OneMoreUser / OneLessUser lifecycle:
 * OneMoreUser loads subboard item/header data files (_Items3, _Headers3)
 * into memory; OneLessUser decrements the user count and may unload.
 * Every code path between them must be protected via goto cleanup.
 *
 * News/GFile/PFile areas use MRK_TEXT_DOOR (3) instead of MRK_MSG_BASE (0).
 * No _Message3 / _Short file; no responses.
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

#include "news.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;

/*
 * Maximum text buffer for reading text from _text file.
 * Same value as message.c and file.c.
 */
#define TEXT_READ_BUF 16384

/* Magic value for HeaderType records in _text. */
#define HEADERTYPE_MAGIC 0xBB25B8C4UL

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
 * Build a path to an item text file in the subboard's DataPath directory.
 * Format: "DataPath/item_N.txt" (NOT under data/).
 * Handles AmigaOS path joining: volume: or trailing / need no separator.
 */
static void build_item_file_path(char *buf, int bufsz,
    const char *data_path, ULONG item_id)
{
    int len = (int)strlen(data_path);

    if (len > 0 && (data_path[len - 1] == ':' ||
                     data_path[len - 1] == '/')) {
        snprintf(buf, bufsz, "%sitem_%lu.txt", data_path,
            (unsigned long)item_id);
    } else {
        snprintf(buf, bufsz, "%s/item_%lu.txt", data_path,
            (unsigned long)item_id);
    }
}

/*
 * Read an entire AmigaOS file into a malloc'd buffer.
 * Returns NULL on failure (sets *err_msg to an error string).
 * Caller must free the returned buffer.
 */
static char *read_dos_file(const char *path, const char **err_msg)
{
    BPTR fh;
    long size;
    long nread;
    char *buf;

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        *err_msg = "Cannot open text file";
        return NULL;
    }

    /* Seek to end to get file size. */
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size < 0) {
        Close(fh);
        *err_msg = "Cannot determine file size";
        return NULL;
    }

    buf = (char *)malloc(size + 1);
    if (!buf) {
        Close(fh);
        *err_msg = "Out of memory";
        return NULL;
    }

    nread = Read(fh, buf, size);
    Close(fh);

    if (nread < 0) {
        free(buf);
        *err_msg = "Read failed from text file";
        return NULL;
    }

    buf[nread] = '\0';

    *err_msg = NULL;
    return buf;
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
 * Attempts to read a HeaderType-sized block at seek_pos. If the magic
 * value validates, follows HeaderType.Text to read the body text.
 * If magic does not match (plain text item), falls back to
 * reading raw text at seek_pos via read_text().
 *
 * Returns:
 *   1 = HeaderType format (out_hdr filled, out_text is body)
 *   0 = Legacy/plain format (out_hdr zeroed, out_text is raw text)
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
 * Check if _text at the given offset contains a HeaderType record.
 * Reads sizeof(struct HeaderType) raw bytes and checks the Magic field.
 * Returns 1 if HeaderType magic matches, 0 otherwise.
 */
static int is_headertype_at(const char *text_path, long offset)
{
    BPTR fh;
    struct HeaderType hdr;
    long nread;

    fh = Open((CONST_STRPTR)text_path, MODE_OLDFILE);
    if (!fh)
        return 0;

    if (Seek(fh, offset, OFFSET_BEGINNING) == -1) {
        Close(fh);
        return 0;
    }

    nread = Read(fh, (APTR)&hdr, (long)sizeof(struct HeaderType));
    Close(fh);

    if (nread < (long)sizeof(struct HeaderType))
        return 0;

    return (hdr.Magic == HEADERTYPE_MAGIC) ? 1 : 0;
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

/*
 * Fill a HeaderType struct from parameters.
 *
 * The caller passes body_pos=0 and body_len=0 as placeholders;
 * write_message_text() fills in hdr->Text and hdr->TextLen after
 * AllocText returns the actual body position.
 */
static void build_header_type(struct HeaderType *hdr,
    const char *author_name, const char *author_handle,
    long author_id, short author_acct,
    const char *to_name, const char *to_handle,
    long to_id, short to_acct,
    long body_pos, long body_len,
    long prev_pos, ULONG number)
{
    memset(hdr, 0, sizeof(struct HeaderType));

    set_current_date(&hdr->ShowDate);
    set_current_date(&hdr->EditDate);
    set_current_date(&hdr->PostDate);

    hdr->ByAccount = author_acct;

    strncpy(hdr->By, author_name, 35);
    hdr->By[35] = '\0';

    strncpy(hdr->ByUser, author_handle, 23);
    hdr->ByUser[23] = '\0';

    hdr->ByID = author_id;

    if (to_name) {
        strncpy(hdr->To, to_name, 35);
        hdr->To[35] = '\0';
    }

    if (to_handle) {
        strncpy(hdr->ToUser, to_handle, 23);
        hdr->ToUser[23] = '\0';
    }

    hdr->ToID = to_id;
    hdr->ToAccount = to_acct;

    hdr->Magic = HEADERTYPE_MAGIC;
    hdr->Number = number;

    hdr->Text = body_pos;
    hdr->TextLen = body_len;

    hdr->Next = -1;
    hdr->Previous = prev_pos;
}

/*
 * Write a HeaderType and body text to the _text file using dual AllocText.
 *
 * Performs two AllocText calls (body text first, then HeaderType), writes
 * both to _text, and calls SaveFree. On error, all allocations are freed
 * and SaveFree is called to keep the free-list consistent.
 *
 * hdr is mutable: hdr->Text and hdr->TextLen are filled in after the
 * first AllocText returns the body position.
 *
 * Returns 0 on success, -1 on failure.
 */
static int write_message_text(struct SubboardType4 *sub,
    const char *text, struct HeaderType *hdr,
    long *out_header_pos,
    const char **err_msg)
{
    long body_len;
    long body_pos;
    long header_pos;
    char text_path[256];
    BPTR fh;
    long written;

    body_len = (long)strlen(text) + 1; /* +1 for null terminator */

    /* First AllocText: space for body text. */
    body_pos = AllocText(sub, body_len);
    if (body_pos < 0) {
        *err_msg = "AllocText failed for body text (text pool full?)";
        return -1;
    }

    /* Fill in the body offset and length in the HeaderType. */
    hdr->Text = body_pos;
    hdr->TextLen = body_len;

    /* Second AllocText: space for HeaderType. */
    header_pos = AllocText(sub, (long)sizeof(struct HeaderType));
    if (header_pos < 0) {
        FreeText(sub, body_pos, body_len);
        SaveFree(sub);
        *err_msg = "AllocText failed for HeaderType (text pool full?)";
        return -1;
    }

    build_text_path(text_path, sizeof(text_path), sub->DataPath);

    fh = Open((CONST_STRPTR)text_path, MODE_READWRITE);
    if (!fh) {
        FreeText(sub, body_pos, body_len);
        FreeText(sub, header_pos, (long)sizeof(struct HeaderType));
        SaveFree(sub);
        *err_msg = "Cannot open _text file for writing";
        return -1;
    }

    /* Write HeaderType at header_pos. */
    Seek(fh, header_pos, OFFSET_BEGINNING);
    written = Write(fh, (APTR)hdr, (long)sizeof(struct HeaderType));
    if (written != (long)sizeof(struct HeaderType)) {
        Close(fh);
        FreeText(sub, body_pos, body_len);
        FreeText(sub, header_pos, (long)sizeof(struct HeaderType));
        SaveFree(sub);
        *err_msg = "Failed to write HeaderType to _text";
        return -1;
    }

    /* Write body text at body_pos. */
    Seek(fh, body_pos, OFFSET_BEGINNING);
    written = Write(fh, (APTR)text, body_len);
    if (written != body_len) {
        Close(fh);
        FreeText(sub, body_pos, body_len);
        FreeText(sub, header_pos, (long)sizeof(struct HeaderType));
        SaveFree(sub);
        *err_msg = "Failed to write body text to _text";
        return -1;
    }

    Close(fh);
    SaveFree(sub);

    *out_header_pos = header_pos;
    *err_msg = NULL;
    return 0;
}

/* ---- news list ---- */

int cmd_news_list(struct MainPort *myp, int argc, char **argv)
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
        json_error("Usage: cnet-cli news list <sub-id|gokey> "
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

    /* Verify subboard is valid and is a text/door area. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_TEXT_DOOR) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a text/door area");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Load subboard data files. */
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
        json_kv_bool(&js, "frozen", (int)item->Frozen);

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

/* ---- news read ---- */

int cmd_news_read(struct MainPort *myp, int argc, char **argv)
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
    char *raw_str = NULL;
    char *text = NULL;
    const char *err_msg;
    const char *text_format = "none";
    int rc = 0;
    int loaded = 0;

    if (argc < 3) {
        json_error("Usage: cnet-cli news read <sub-id|gokey> "
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

    /* Verify subboard is valid and is a text/door area. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_TEXT_DOOR) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a text/door area");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Load subboard data files. */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        return 1;
    }
    loaded = 1;

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

    /* Determine text format and read content. */
    memset(&hdr, 0, sizeof(hdr));
    if (item.First >= 0) {
        build_text_path(text_path, sizeof(text_path),
            sub->DataPath);

        if (is_headertype_at(text_path, item.First)) {
            /* Old HeaderType format -- use compat reader. */
            int hdr_result;

            hdr_result = read_header_and_text(text_path,
                item.First, &hdr, &text, &err_msg);
            if (hdr_result == 1) {
                text_format = "header";
            } else if (hdr_result == 0) {
                text_format = "plain";
            } else {
                text_format = "none";
                rc = 1;
            }
            goto emit_json;
        }

        /* DOS file path format: read path string, then file. */
        raw_str = read_text(text_path, item.First, &err_msg);
        if (!raw_str) {
            /* Build JSON with error. */
            json_init(&js, stdout);
            json_obj_open(&js);
            json_key(&js, "item");
            json_obj_open(&js);
            json_kv_int(&js, "number", (long)ihead.Number);
            json_kv_int(&js, "index", item_index);
            json_kv_str(&js, "title",
                strip_mci(buf, sizeof(buf), item.Title));
            json_kv_null(&js, "text");
            json_kv_str(&js, "text_error", err_msg);
            json_kv_str(&js, "text_format", "none");
            json_obj_close(&js);
            json_obj_close(&js);
            json_finish(&js);
            rc = 1;
            goto cleanup;
        }

        text = read_dos_file(raw_str, &err_msg);
        if (text) {
            text_format = "file";
        } else {
            text_format = "file";
            rc = 1;
        }
    }

emit_json:
    /* Build JSON output. */
    json_init(&js, stdout);
    json_obj_open(&js);

    json_key(&js, "item");
    json_obj_open(&js);

    json_kv_int(&js, "number", (long)ihead.Number);
    json_kv_int(&js, "index", item_index);
    json_kv_str(&js, "title",
        strip_mci(buf, sizeof(buf), item.Title));
    json_kv_int(&js, "by_account", (long)item.ByAccount);
    json_kv_str(&js, "by_handle",
        strip_mci(buf, sizeof(buf),
            lookup_handle(myp, item.ByAccount)));

    if (is_null_date(&ihead.PostDate))
        json_kv_null(&js, "post_date");
    else
        json_kv_str(&js, "post_date",
            format_date(datebuf, sizeof(datebuf), &ihead.PostDate));

    /* HeaderType metadata only for old "header" format. */
    if (strcmp(text_format, "header") == 0) {
        if (hdr.By[0])
            json_kv_str(&js, "by_name",
                strip_mci(buf, sizeof(buf), hdr.By));
        if (hdr.ByUser[0])
            json_kv_str(&js, "by_user",
                strip_mci(buf, sizeof(buf), hdr.ByUser));
        if (!is_null_date(&hdr.PostDate))
            json_kv_str(&js, "post_date_header",
                format_date(datebuf, sizeof(datebuf),
                    &hdr.PostDate));
        if (hdr.Organ[0])
            json_kv_str(&js, "org",
                strip_mci(buf, sizeof(buf), hdr.Organ));
    }

    json_kv_bool(&js, "killed", (int)ihead.Killed);
    json_kv_bool(&js, "frozen", (int)item.Frozen);
    json_kv_bool(&js, "auto_grab", (int)item.AutoGrab);

    /* Item text */
    if (text)
        json_kv_str(&js, "text", text);
    else
        json_kv_null(&js, "text");

    /* Report I/O errors. */
    if (rc == 1 && err_msg)
        json_kv_str(&js, "text_error", err_msg);

    /* Text file path for "file" format. */
    if (strcmp(text_format, "file") == 0 && raw_str)
        json_kv_str(&js, "text_file", raw_str);

    json_kv_str(&js, "text_format", text_format);

    json_obj_close(&js); /* close "item" */
    json_obj_close(&js); /* close root */
    json_finish(&js);

cleanup:
    if (raw_str)
        free(raw_str);
    if (text)
        free(text);
    if (loaded)
        OneLessUser(sub);
    return rc;
}

/* ---- news post ---- */

int cmd_news_post(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short physnum;
    struct SubboardType4 *sub;
    struct ItemType3 item;
    struct ItemHeader ihead;
    int marker_base;
    const char *title = NULL;
    const char *author_str = NULL;
    const char *text_arg = NULL;
    short author_acct;
    long author_id = 0;
    ULONG new_id;
    long text_offset = 0;
    long path_len;
    char file_path[256];
    char text_path[256];
    BPTR fh;
    long written;
    int rc = 0;
    int i;
    int loaded = 0;
    int sem_held = 0;
    int file_created = 0;

    if (argc < 2) {
        json_error("Usage: cnet-cli news post <sub-id|gokey> "
            "--title \"...\" --author <account> --text \"...\"");
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
        } else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            i++;
            text_arg = argv[i];
        }
    }

    if (!title || !author_str || !text_arg) {
        json_error("Required: --title, --author, --text");
        return 1;
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

    /* Verify text/door type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_TEXT_DOOR) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a text/door area");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Validate that the author account exists. */
    {
        long check_id = 0;

        ObtainSemaphoreShared(&myp->SEM[1]);
        if (author_acct > 0 &&
                author_acct <= (short)myp->Nums[0]) {
            check_id = myp->Key[author_acct - 1].IDNumber;
        }
        ReleaseSemaphore(&myp->SEM[1]);

        if (check_id == 0) {
            json_error("Author account not found");
            return 1;
        }
        author_id = check_id;
    }

    /* Load subboard data. */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        return 1;
    }
    loaded = 1;

    /* Protect text allocation and count increment. */
    ObtainSemaphore(sub->sem);
    sem_held = 1;

    new_id = get_next_id(sub);

    /* Build path for the physical text file in DataPath dir. */
    build_item_file_path(file_path, sizeof(file_path),
        sub->DataPath, new_id);

    /* Create the physical text file. */
    fh = Open((CONST_STRPTR)file_path, MODE_NEWFILE);
    if (!fh) {
        json_error("Cannot create text file");
        rc = 1;
        goto sem_release;
    }
    written = Write(fh, (APTR)text_arg, (long)strlen(text_arg));
    Close(fh);
    file_created = 1;
    if (written != (long)strlen(text_arg)) {
        json_error("Failed to write text file");
        rc = 1;
        goto sem_release;
    }

    /* AllocText for the DOS path string in _text. */
    path_len = (long)strlen(file_path) + 1;
    text_offset = AllocText(sub, path_len);
    if (text_offset < 0) {
        json_error("AllocText failed for path (text pool full?)");
        rc = 1;
        goto sem_release;
    }

    /* Write the DOS path to _text at the allocated position. */
    build_text_path(text_path, sizeof(text_path), sub->DataPath);
    fh = Open((CONST_STRPTR)text_path, MODE_READWRITE);
    if (!fh) {
        FreeText(sub, text_offset, path_len);
        SaveFree(sub);
        json_error("Cannot open _text file for writing");
        rc = 1;
        goto sem_release;
    }
    Seek(fh, text_offset, OFFSET_BEGINNING);
    written = Write(fh, (APTR)file_path, path_len);
    Close(fh);
    if (written != path_len) {
        FreeText(sub, text_offset, path_len);
        SaveFree(sub);
        json_error("Failed to write path to _text");
        rc = 1;
        goto sem_release;
    }

    SaveFree(sub);

    /* Prepare ItemType3. */
    memset(&item, 0, sizeof(item));
    strncpy(item.Title, title, sizeof(item.Title) - 1);
    item.Title[sizeof(item.Title) - 1] = '\0';
    item.ByAccount = author_acct;
    item.ByID = author_id;
    item.ToID = 0;
    item.First = text_offset;
    item.Last = text_offset;
    item.Validated = 1;
    item.Finished = 1;
    item.AutoGrab = 1;
    item.PurgeKill = 1;
    item.PurgeStatus = sub->PurgeStatus;
    item.DLnotifyULer = sub->DLnotifyULer;

    /* Prepare ItemHeader. */
    memset(&ihead, 0, sizeof(ihead));
    ihead.Number = new_id;
    ihead.Size = 0;
    ihead.Responses = 0;
    set_current_date(&ihead.PostDate);
    set_current_date(&ihead.RespDate);

    /* Build TitleSort: uppercase first 8 chars of title. */
    {
        int ti;
        for (ti = 0; ti < 8 && title[ti]; ti++)
            ihead.TitleSort[ti] = (UBYTE)toupper(
                (unsigned char)title[ti]);
        ihead.TitleSort[ti] = '\0';
    }

    /* Add item to subboard. */
    if (!ZAddItem(&item, &ihead, sub)) {
        FreeText(sub, text_offset, path_len);
        SaveFree(sub);
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
    json_kv_str(&js, "status", "posted");
    json_kv_int(&js, "physnum", (long)physnum);
    json_kv_int(&js, "item_count", (long)sub->rn);
    json_kv_int(&js, "next_id", (long)sub->count);
    json_kv_str(&js, "title", title);
    json_kv_int(&js, "by_account", (long)author_acct);
    json_kv_int(&js, "text_offset", text_offset);
    json_kv_str(&js, "text_file", file_path);
    json_obj_close(&js);
    json_finish(&js);

sem_release:
    if (sem_held) {
        ReleaseSemaphore(sub->sem);
        sem_held = 0;
    }

    if (rc && file_created)
        DeleteFile((CONST_STRPTR)file_path);

    if (loaded)
        OneLessUser(sub);
    return rc;
}

/* ---- news edit ---- */

int cmd_news_edit(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short physnum;
    struct SubboardType4 *sub;
    struct ItemType3 item;
    struct ItemHeader ihead;
    struct HeaderType old_hdr;
    struct HeaderType new_hdr;
    int marker_base;
    const char *text_arg = NULL;
    const char *title_arg = NULL;
    const char *err_msg;
    char *raw_str = NULL;
    char *old_text = NULL;
    long item_index;
    long count;
    long header_pos = 0;
    int is_header_fmt = 0;
    int item_changed = 0;
    int rc = 0;
    int i;
    int loaded = 0;
    int sem_held = 0;
    char text_path[256];

    if (argc < 3) {
        json_error("Usage: cnet-cli news edit <sub-id|gokey> "
            "<item-number> [--text \"...\"] [--title \"...\"]");
        return 1;
    }

    item_index = atol(argv[2]);
    if (item_index < 1) {
        json_error("Item number must be >= 1");
        return 1;
    }

    /* Parse arguments */
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            i++;
            text_arg = argv[i];
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            i++;
            title_arg = argv[i];
        }
    }

    if (!text_arg && !title_arg) {
        json_error("Required: --text and/or --title");
        return 1;
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        return 1;
    }

    /* Verify text/door type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_TEXT_DOOR) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a text/door area");
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

    /* Handle --title update (independent of text format). */
    if (title_arg) {
        strncpy(item.Title, title_arg, sizeof(item.Title) - 1);
        item.Title[sizeof(item.Title) - 1] = '\0';

        /* Update TitleSort. */
        {
            int ti;
            for (ti = 0; ti < 8 && title_arg[ti]; ti++)
                ihead.TitleSort[ti] = (UBYTE)toupper(
                    (unsigned char)title_arg[ti]);
            ihead.TitleSort[ti] = '\0';
        }
        item_changed = 1;
    }

    /* If no --text, just save the title change and finish. */
    if (!text_arg) {
        if (item_changed)
            ZPutItem(&item, &ihead, sub,
                (short)(item_index - 1));
        goto emit_json;
    }

    /* Verify item has text content. */
    if (item.First < 0) {
        json_error("Item has no text content");
        rc = 1;
        goto cleanup;
    }

    /* Determine format by checking for HeaderType magic in _text. */
    build_text_path(text_path, sizeof(text_path), sub->DataPath);
    is_header_fmt = is_headertype_at(text_path, item.First);

    if (!is_header_fmt) {
        /*
         * DOS file path format (new): read path, then overwrite
         * the physical file with new --text content.
         */
        BPTR fh;
        long written;

        raw_str = read_text(text_path, item.First, &err_msg);
        if (!raw_str) {
            json_error(err_msg);
            rc = 1;
            goto cleanup;
        }

        fh = Open((CONST_STRPTR)raw_str, MODE_NEWFILE);
        if (!fh) {
            json_error("Cannot open text file for writing");
            rc = 1;
            goto cleanup;
        }
        written = Write(fh, (APTR)text_arg,
            (long)strlen(text_arg));
        Close(fh);
        if (written != (long)strlen(text_arg)) {
            json_error("Failed to write text file");
            rc = 1;
            goto cleanup;
        }

        if (item_changed)
            ZPutItem(&item, &ihead, sub,
                (short)(item_index - 1));
    } else {
        /*
         * HeaderType format (old): free old body and old header,
         * then write new content via dual AllocText.
         */
        int hdr_result;

        /* Re-read via compat reader for full HeaderType. */
        memset(&old_hdr, 0, sizeof(old_hdr));
        hdr_result = read_header_and_text(text_path, item.First,
            &old_hdr, &old_text, &err_msg);
        if (hdr_result == -1) {
            json_error(err_msg);
            rc = 1;
            goto cleanup;
        }

        /* Protect text pool modifications. */
        ObtainSemaphore(sub->sem);
        sem_held = 1;

        if (hdr_result == 1) {
            FreeText(sub, old_hdr.Text, old_hdr.TextLen);
            FreeText(sub, item.First,
                (long)sizeof(struct HeaderType));
            SaveFree(sub);

            new_hdr = old_hdr;
            set_current_date(&new_hdr.EditDate);
            new_hdr.Text = 0;
            new_hdr.TextLen = 0;

            if (write_message_text(sub, text_arg, &new_hdr,
                    &header_pos, &err_msg) != 0) {
                json_error(err_msg);
                rc = 1;
                goto sem_release;
            }

            item.First = header_pos;
            item.Last = header_pos;
        } else {
            /*
             * Plain text: orphan old allocation and upgrade
             * to HeaderType format.
             */
            char author_name[36];
            char author_handle[24];
            long author_id = 0;

            author_name[0] = '\0';
            author_handle[0] = '\0';

            ObtainSemaphoreShared(&myp->SEM[1]);
            if (item.ByAccount > 0 &&
                    item.ByAccount <= (short)myp->Nums[0]) {
                strncpy(author_name,
                    myp->Key[item.ByAccount - 1].RealName,
                    sizeof(author_name) - 1);
                author_name[sizeof(author_name) - 1] = '\0';
                strncpy(author_handle,
                    myp->Key[item.ByAccount - 1].Handle,
                    sizeof(author_handle) - 1);
                author_handle[sizeof(author_handle) - 1] = '\0';
                author_id =
                    myp->Key[item.ByAccount - 1].IDNumber;
            }
            ReleaseSemaphore(&myp->SEM[1]);

            build_header_type(&new_hdr, author_name,
                author_handle, author_id, item.ByAccount,
                NULL, NULL, 0, 0,
                0, 0, -1, ihead.Number);

            new_hdr.PostDate = ihead.PostDate;
            new_hdr.ShowDate = ihead.PostDate;

            if (write_message_text(sub, text_arg, &new_hdr,
                    &header_pos, &err_msg) != 0) {
                json_error(err_msg);
                rc = 1;
                goto sem_release;
            }

            item.First = header_pos;
            item.Last = header_pos;
        }

        ZPutItem(&item, &ihead, sub, (short)(item_index - 1));
    }

emit_json:
    /* Persist updated subboard to disk. */
    ObtainSemaphore(&myp->SEM[5]);
    write_subboard_disk((int)physnum, sub);
    ReleaseSemaphore(&myp->SEM[5]);

    /* Output confirmation. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "edited");
    json_kv_int(&js, "physnum", (long)physnum);
    json_kv_int(&js, "item_index", item_index);
    json_kv_int(&js, "item_number", (long)ihead.Number);
    if (text_arg)
        json_kv_str(&js, "old_format",
            is_header_fmt ? "header" : "file");
    if (title_arg)
        json_kv_str(&js, "title", title_arg);
    if (text_arg) {
        if (is_header_fmt) {
            json_kv_int(&js, "header_offset", header_pos);
            json_kv_int(&js, "body_offset", new_hdr.Text);
        } else if (raw_str) {
            json_kv_str(&js, "text_file", raw_str);
        }
    }
    json_obj_close(&js);
    json_finish(&js);

sem_release:
    if (sem_held) {
        ReleaseSemaphore(sub->sem);
        sem_held = 0;
    }

cleanup:
    if (raw_str)
        free(raw_str);
    if (old_text)
        free(old_text);
    if (loaded)
        OneLessUser(sub);
    return rc;
}

/* ---- news delete ---- */

int cmd_news_delete(struct MainPort *myp, int argc, char **argv)
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

    if (argc < 3) {
        json_error("Usage: cnet-cli news delete <sub-id|gokey> "
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

    /* Verify text/door type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_TEXT_DOOR) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a text/door area");
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

    /* Mark as killed. */
    ihead.Killed = 1;

    /* Write back. */
    ZPutItem(&item, &ihead, sub, (short)(item_index - 1));

    /* Output confirmation. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "deleted");
    json_kv_int(&js, "physnum", (long)physnum);
    json_kv_int(&js, "item_index", item_index);
    json_kv_int(&js, "item_number", (long)ihead.Number);
    json_kv_str(&js, "title",
        strip_mci(buf, sizeof(buf), item.Title));
    json_obj_close(&js);
    json_finish(&js);

cleanup:
    if (loaded)
        OneLessUser(sub);
    return rc;
}
