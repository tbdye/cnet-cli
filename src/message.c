/*
 * message.c -- Message commands for cnet-cli
 *
 * Message operations: list, read, post, respond, delete, edit, search, move
 *
 * All commands follow the OneMoreUser / OneLessUser lifecycle:
 * OneMoreUser loads subboard item/header data files (_Items3, _Headers3)
 * into memory; OneLessUser decrements the user count and may unload.
 * OneMoreUser does NOT load _Message3 (responses); those are read
 * directly from disk by load_messages() when needed.
 * Every code path between them must be protected via goto cleanup.
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

#include "message.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;

/*
 * Maximum text buffer for reading message text from _text file.
 * Messages are null-terminated in the text pool. We read in chunks
 * until we hit the terminator or exhaust the buffer.
 */
#define TEXT_READ_BUF 16384

/* Magic value for HeaderType records in _text. */
#define HEADERTYPE_MAGIC 0xBB25B8C4UL

/* Maximum responses to walk in the linked list chain.
 * Prevents infinite loops from circular chains or corrupted pointers. */
#define MAX_RESPONSES_WALK 10000

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
 * Look up a user's unique ID from the Key array by account number.
 * Acquires SEM[1] shared.
 * Returns the IDNumber, or 0 if account is out of range.
 */
static long lookup_id(struct MainPort *myp, short account)
{
    long id = 0;

    ObtainSemaphoreShared(&myp->SEM[1]);
    if (account > 0 && account <= (short)myp->Nums[0]) {
        id = myp->Key[account - 1].IDNumber;
    }
    ReleaseSemaphore(&myp->SEM[1]);

    return id;
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

/* ---- New helper functions for HeaderType-based _text format ---- */

/*
 * Read sub->count, increment it, return the pre-increment value.
 * The returned value is the unique ID for a new message or response.
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

    /* Second AllocText: space for HeaderType (288 bytes). */
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

    /*
     * Write body text at body_pos.
     *
     * If this write fails after the HeaderType was already written above,
     * the stale HeaderType bytes remain in _text at header_pos. This is
     * harmless: FreeText reclaims both allocations in the free list, and
     * no ItemType3 or MessageType3 record references header_pos, so the
     * orphaned data is unreachable and will be overwritten by a future
     * AllocText. The BBS handles partial writes the same way.
     */
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
 * Read only the HeaderType from _text at the given seek position.
 * Does NOT read the body text. Validates HEADERTYPE_MAGIC.
 * Returns 0 on success, -1 on failure (I/O error or bad magic).
 */
static int read_header_only(const char *text_path, long seek_pos,
    struct HeaderType *out_hdr)
{
    BPTR fh;
    LONG nread;

    fh = Open((CONST_STRPTR)text_path, MODE_OLDFILE);
    if (!fh)
        return -1;

    if (Seek(fh, seek_pos, OFFSET_BEGINNING) < 0) {
        Close(fh);
        return -1;
    }

    nread = Read(fh, out_hdr, (LONG)sizeof(struct HeaderType));
    Close(fh);

    if (nread != (LONG)sizeof(struct HeaderType))
        return -1;

    /* Validate magic to catch corrupted Next pointers that lead
     * to garbage data. Without this check, the counting pass could
     * follow a long chain of garbage before the reading pass
     * (which does validate magic via read_header_and_text())
     * catches the error. */
    if (out_hdr->Magic != HEADERTYPE_MAGIC)
        return -1;

    return 0;
}

/* ---- msg list ---- */

int cmd_msg_list(struct MainPort *myp, int argc, char **argv)
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
        json_error("Usage: cnet-cli msg list <sub-id|gokey> "
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

    /* Verify subboard is valid and is a MsgBase. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_MSG_BASE) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a message base");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /*
     * Load subboard data files.
     *
     * TOCTOU note: there is a narrow window between the SEM[5] release
     * above and this OneMoreUser call where another process could
     * theoretically kill the subboard.  In practice this is harmless:
     * the Subboard[] array is never reallocated while CNet runs, so the
     * pointer remains valid.  The marker check above is a courtesy
     * validation, not a safety gate.  OneMoreUser handles its own
     * internal locking for the data file load.
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
        json_kv_int(&js, "by_account", (long)item->ByAccount);
        json_kv_str(&js, "by_handle",
            strip_mci(buf, sizeof(buf), handle));
        json_kv_int(&js, "responses", ihead->Responses);
        if (is_null_date(&ihead->PostDate))
            json_kv_null(&js, "post_date");
        else
            json_kv_str(&js, "post_date",
                format_date(datebuf, sizeof(datebuf),
                    &ihead->PostDate));
        json_kv_bool(&js, "killed", (int)ihead->Killed);
        json_kv_int(&js, "size", ihead->Size);

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

/* ---- msg read ---- */

int cmd_msg_read(struct MainPort *myp, int argc, char **argv)
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
    const char *err_msg;
    int hdr_result;
    int rc = 0;

    if (argc < 3) {
        json_error("Usage: cnet-cli msg read <sub-id|gokey> "
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

    /* Verify subboard is valid and is a MsgBase. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_MSG_BASE) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a message base");
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

    /* Build path to _text file. */
    build_text_path(text_path, sizeof(text_path), sub->DataPath);

    /* Read the item's text content via HeaderType-aware reader. */
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

    json_kv_int(&js, "number", (long)ihead.Number);
    json_kv_int(&js, "index", item_index);
    json_kv_str(&js, "title",
        strip_mci(buf, sizeof(buf), item.Title));
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

    /* HeaderType metadata (if available). */
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

    json_kv_int(&js, "responses", ihead.Responses);
    json_kv_bool(&js, "killed", (int)ihead.Killed);
    json_kv_int(&js, "size", ihead.Size);

    /* Message text */
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
    OneLessUser(sub);
    return rc;
}

/* ---- Encapsulated multi-step operations ---- */

/*
 * Legacy: Write raw text content to a subboard's _text file.
 *
 * Retained for backward compatibility with potential future callers.
 * The new cmd_msg_post() and cmd_msg_respond() use write_message_text()
 * instead, which writes both a HeaderType and body text.
 */
static int write_item_text(struct SubboardType4 *sub, const char *text,
    long *out_textpos, const char **err_msg)
{
    long text_len;
    long textpos;
    char text_path[256];
    BPTR fh;
    long written;
    char nul = '\0';

    text_len = (long)strlen(text) + 1; /* +1 for null terminator */

    /* Reserve space in the subboard's text pool. */
    textpos = AllocText(sub, text_len);
    if (textpos < 0) {
        *err_msg = "AllocText failed (text pool full?)";
        return -1;
    }

    build_text_path(text_path, sizeof(text_path), sub->DataPath);

    fh = Open((CONST_STRPTR)text_path, MODE_READWRITE);
    if (!fh) {
        FreeText(sub, textpos, text_len);
        *err_msg = "Cannot open _text file for writing";
        return -1;
    }

    if (Seek(fh, textpos, OFFSET_BEGINNING) == -1) {
        Close(fh);
        FreeText(sub, textpos, text_len);
        *err_msg = "Seek failed in _text file";
        return -1;
    }

    written = Write(fh, (APTR)text, text_len - 1);
    if (written != text_len - 1) {
        Close(fh);
        FreeText(sub, textpos, text_len);
        *err_msg = "Write failed to _text file";
        return -1;
    }

    /* Null terminator marks end-of-text for read_text(). */
    written = Write(fh, &nul, 1);
    if (written != 1) {
        Close(fh);
        FreeText(sub, textpos, text_len);
        *err_msg = "Write null terminator failed";
        return -1;
    }

    Close(fh);

    *out_textpos = textpos;
    *err_msg = NULL;
    return 0;
}

/*
 * Legacy: Add a new item to a subboard and persist the updated count.
 *
 * Retained for backward compatibility with potential future callers.
 * The new cmd_msg_post() calls ZAddItem directly and manages sub->count
 * via get_next_id() to avoid double-incrementing.
 */
static int add_item(struct MainPort *myp, int physnum,
    struct SubboardType4 *sub, struct ItemType3 *item,
    struct ItemHeader *ihead, int *warn_disk_failed)
{
    UBYTE result;

    *warn_disk_failed = 0;

    result = ZAddItem(item, ihead, sub);
    if (!result) {
        return -1;
    }

    /* ZAddItem does not update the in-memory item count. */
    sub->count++;

    /* Persist the updated subboard record to disk. */
    ObtainSemaphore(&myp->SEM[5]);
    if (write_subboard_disk(physnum, sub) != 0) {
        *warn_disk_failed = 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    return 0;
}

/*
 * Legacy: Add a response (message) to an existing item and persist all updates.
 *
 * Retained for backward compatibility with potential future callers.
 * The new cmd_msg_respond() calls ZAddMessage/ZPutItem directly and
 * manages sub->count via get_next_id() to avoid double-incrementing.
 */
static int add_response(struct MainPort *myp, int physnum,
    struct SubboardType4 *sub, struct MessageType3 *msg,
    struct ItemType3 *item, struct ItemHeader *ihead,
    short item_array_index, int *warn_disk_failed)
{
    *warn_disk_failed = 0;

    /* Write the MessageType3 record to _Message3. */
    ZAddMessage(msg, sub);

    /* ZAddMessage does not update the item header's response count. */
    ihead->Responses++;
    ZPutItem(item, ihead, sub, item_array_index);

    /* ZAddMessage does not update the in-memory message count. */
    sub->nNewMess++;

    /* Persist the updated subboard record to disk. */
    ObtainSemaphore(&myp->SEM[5]);
    if (write_subboard_disk(physnum, sub) != 0) {
        *warn_disk_failed = 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    return 0;
}

/* Suppress unused-function warnings for legacy helpers. */
static void dummy_legacy_refs(void) __attribute__((unused));
static void dummy_legacy_refs(void)
{
    (void)write_item_text;
    (void)add_item;
    (void)add_response;
    (void)lookup_id;
    (void)read_text;
}

/*
 * Patch a HeaderType's Next or Previous pointer in the _text file.
 * Reads the HeaderType at target_pos, updates the specified field,
 * and writes it back. Returns 0 on success, -1 on failure.
 */
static int patch_header_link(struct SubboardType4 *sub,
    long target_pos, int is_next, long new_value)
{
    char text_path[256];
    BPTR fh;
    struct HeaderType tmp_hdr;
    long nread, writ;

    build_text_path(text_path, sizeof(text_path), sub->DataPath);

    fh = Open((CONST_STRPTR)text_path, MODE_READWRITE);
    if (!fh)
        return -1;

    Seek(fh, target_pos, OFFSET_BEGINNING);
    nread = Read(fh, (APTR)&tmp_hdr, (long)sizeof(struct HeaderType));
    if (nread != (long)sizeof(struct HeaderType) ||
            tmp_hdr.Magic != HEADERTYPE_MAGIC) {
        Close(fh);
        return -1;
    }

    if (is_next)
        tmp_hdr.Next = new_value;
    else
        tmp_hdr.Previous = new_value;

    Seek(fh, target_pos, OFFSET_BEGINNING);
    writ = Write(fh, (APTR)&tmp_hdr, (long)sizeof(struct HeaderType));
    Close(fh);

    return (writ == (long)sizeof(struct HeaderType)) ? 0 : -1;
}

/*
 * Write a single MessageType3 record back to _Message3 at a given
 * record index. Returns 0 on success, -1 on failure.
 */
static int write_message3_record(const char *data_path,
    long record_index, struct MessageType3 *msg)
{
    char msg_path[256];
    BPTR fh;
    long offset;
    long written;

    build_data_file_path(msg_path, sizeof(msg_path),
        data_path, "_Message3");

    fh = Open((CONST_STRPTR)msg_path, MODE_READWRITE);
    if (!fh)
        return -1;

    offset = record_index * (long)sizeof(struct MessageType3);
    Seek(fh, offset, OFFSET_BEGINNING);
    written = Write(fh, (APTR)msg,
        (long)sizeof(struct MessageType3));
    Close(fh);

    return (written == (long)sizeof(struct MessageType3)) ? 0 : -1;
}

/*
 * Read text content from an AmigaOS file path.
 * Returns a malloc'd buffer on success (caller must free), or NULL on failure.
 * Sets *out_len to the number of bytes read (not including null terminator).
 * Maximum file size: 65536 bytes.
 */
static char *read_text_file(const char *path, long *out_len)
{
    BPTR fh;
    long size;
    char *buf;
    long nread;

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return NULL;

    /* Seek to end to get size. */
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size < 0 || size > 65536) {
        Close(fh);
        return NULL;
    }

    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        Close(fh);
        return NULL;
    }

    nread = Read(fh, buf, size);
    Close(fh);

    if (nread < 0) {
        free(buf);
        return NULL;
    }

    buf[nread] = '\0';
    if (out_len)
        *out_len = nread;
    return buf;
}

/* ---- msg post ---- */

int cmd_msg_post(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short physnum;
    struct SubboardType4 *sub;
    struct ItemType3 item;
    struct ItemHeader ihead;
    struct HeaderType hdr;
    int marker_base;
    const char *title = NULL;
    const char *author_str = NULL;
    const char *to_str = NULL;
    const char *text_arg = NULL;
    const char *err_msg;
    short author_acct;
    short to_acct = 0;
    char author_name[36];
    char author_handle[24];
    long author_id = 0;
    char to_name[36];
    char to_handle[24];
    long to_id = 0;
    ULONG new_id;
    long header_pos = 0;
    int rc = 0;
    int i;
    int loaded = 0;
    int sem_held = 0;
    const char *file_arg = NULL;
    char *file_buf = NULL;

    if (argc < 2) {
        json_error("Usage: cnet-cli msg post <sub-id|gokey> "
            "--title \"...\" --author <account> "
            "--text \"...\" | --file <path> "
            "[--to <account>]");
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
        } else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
            i++;
            to_str = argv[i];
        } else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            i++;
            text_arg = argv[i];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            i++;
            file_arg = argv[i];
        }
    }

    if (text_arg && file_arg) {
        json_error("Cannot use both --text and --file");
        free(file_buf);
        return 1;
    }
    if (file_arg) {
        file_buf = read_text_file(file_arg, NULL);
        if (!file_buf) {
            json_error("Failed to read --file");
            return 1;
        }
        text_arg = file_buf;
    }

    if (!title || !author_str || !text_arg) {
        json_error("Required: --title, --author, --text (or --file)");
        free(file_buf);
        return 1;
    }

    author_acct = (short)atol(author_str);
    if (author_acct < 1) {
        json_error("Invalid --author account number");
        free(file_buf);
        return 1;
    }

    if (to_str) {
        to_acct = (short)atol(to_str);
        if (to_acct < 1) {
            json_error("Invalid --to account number");
            free(file_buf);
            return 1;
        }
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        free(file_buf);
        return 1;
    }

    /* Verify MsgBase type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        free(file_buf);
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_MSG_BASE) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a message base");
        free(file_buf);
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Look up author identity under SEM[1] shared. */
    author_name[0] = '\0';
    author_handle[0] = '\0';
    ObtainSemaphoreShared(&myp->SEM[1]);
    if (author_acct > 0 && author_acct <= (short)myp->Nums[0]) {
        strncpy(author_name,
            myp->Key[author_acct - 1].RealName,
            sizeof(author_name) - 1);
        author_name[sizeof(author_name) - 1] = '\0';
        strncpy(author_handle,
            myp->Key[author_acct - 1].Handle,
            sizeof(author_handle) - 1);
        author_handle[sizeof(author_handle) - 1] = '\0';
        author_id = myp->Key[author_acct - 1].IDNumber;
    }
    ReleaseSemaphore(&myp->SEM[1]);

    /* Validate that the author account was found. */
    if (author_id == 0) {
        json_error("Author account not found");
        free(file_buf);
        return 1;
    }

    /* Look up recipient identity if --to specified. */
    to_name[0] = '\0';
    to_handle[0] = '\0';
    if (to_acct > 0) {
        ObtainSemaphoreShared(&myp->SEM[1]);
        if (to_acct <= (short)myp->Nums[0]) {
            strncpy(to_name,
                myp->Key[to_acct - 1].RealName,
                sizeof(to_name) - 1);
            to_name[sizeof(to_name) - 1] = '\0';
            strncpy(to_handle,
                myp->Key[to_acct - 1].Handle,
                sizeof(to_handle) - 1);
            to_handle[sizeof(to_handle) - 1] = '\0';
            to_id = myp->Key[to_acct - 1].IDNumber;
        }
        ReleaseSemaphore(&myp->SEM[1]);
    }

    /* Load subboard data. */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        free(file_buf);
        return 1;
    }
    loaded = 1;

    /* Protect text allocation and count increment. */
    ObtainSemaphore(sub->sem);
    sem_held = 1;

    new_id = get_next_id(sub);

    /* Build HeaderType (body_pos/body_len are placeholders). */
    build_header_type(&hdr, author_name, author_handle,
        author_id, author_acct,
        to_acct > 0 ? to_name : NULL,
        to_acct > 0 ? to_handle : NULL,
        to_id, to_acct,
        0, 0, -1, new_id);

    /* Write HeaderType and body text to _text. */
    if (write_message_text(sub, text_arg, &hdr,
            &header_pos, &err_msg) != 0) {
        json_error(err_msg);
        rc = 1;
        goto sem_release;
    }

    /* Prepare ItemType3. */
    memset(&item, 0, sizeof(item));
    strncpy(item.Title, title, sizeof(item.Title) - 1);
    item.Title[sizeof(item.Title) - 1] = '\0';
    item.ByAccount = author_acct;
    item.ByID = author_id;
    item.ToID = to_id;
    item.First = header_pos;
    item.Last = header_pos;
    item.Validated = 1;
    item.Finished = 1;
    item.PurgeKill = 1;
    item.PurgeStatus = sub->PurgeStatus;
    item.DLnotifyULer = sub->DLnotifyULer;

    /* Prepare ItemHeader. */
    memset(&ihead, 0, sizeof(ihead));
    ihead.Number = new_id;
    ihead.Size = 0; /* 0 = message post, not file */
    ihead.Responses = 0;
    set_current_date(&ihead.PostDate);
    set_current_date(&ihead.RespDate);

    /* Build TitleSort: uppercase first 8 chars of title. */
    {
        int ti;
        for (ti = 0; ti < 8 && title[ti]; ti++)
            ihead.TitleSort[ti] = (UBYTE)toupper((unsigned char)title[ti]);
        ihead.TitleSort[ti] = '\0';
    }

    /* Add item to subboard. */
    if (!ZAddItem(&item, &ihead, sub)) {
        FreeText(sub, hdr.Text, hdr.TextLen);
        FreeText(sub, header_pos, (long)sizeof(struct HeaderType));
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
    json_kv_int(&js, "header_offset", header_pos);
    json_kv_int(&js, "body_offset", hdr.Text);
    json_obj_close(&js);
    json_finish(&js);

sem_release:
    if (sem_held) {
        ReleaseSemaphore(sub->sem);
        sem_held = 0;
    }

    if (loaded)
        OneLessUser(sub);
    free(file_buf);
    return rc;
}

/* ---- msg respond ---- */

int cmd_msg_respond(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short physnum;
    struct SubboardType4 *sub;
    struct ItemType3 item;
    struct ItemHeader ihead;
    struct MessageType3 msg;
    struct HeaderType hdr;
    int marker_base;
    const char *author_str = NULL;
    const char *to_str = NULL;
    const char *text_arg = NULL;
    const char *err_msg;
    short author_acct;
    short to_acct = 0;
    char author_name[36];
    char author_handle[24];
    long author_id = 0;
    char to_name[36];
    char to_handle[24];
    long to_id = 0;
    long item_index;
    long count;
    long old_last;
    ULONG new_id;
    long header_pos = 0;
    int rc = 0;
    int i;
    int loaded = 0;
    int sem_held = 0;
    const char *file_arg = NULL;
    char *file_buf = NULL;

    if (argc < 3) {
        json_error("Usage: cnet-cli msg respond <sub-id|gokey> "
            "<item-number> --author <account> "
            "--text \"...\" | --file <path> "
            "[--to <account>]");
        return 1;
    }

    item_index = atol(argv[2]);
    if (item_index < 1) {
        json_error("Item number must be >= 1");
        return 1;
    }

    /* Parse arguments */
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--author") == 0 && i + 1 < argc) {
            i++;
            author_str = argv[i];
        } else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
            i++;
            to_str = argv[i];
        } else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            i++;
            text_arg = argv[i];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            i++;
            file_arg = argv[i];
        }
    }

    if (text_arg && file_arg) {
        json_error("Cannot use both --text and --file");
        free(file_buf);
        return 1;
    }
    if (file_arg) {
        file_buf = read_text_file(file_arg, NULL);
        if (!file_buf) {
            json_error("Failed to read --file");
            return 1;
        }
        text_arg = file_buf;
    }

    if (!author_str || !text_arg) {
        json_error("Required: --author, --text (or --file)");
        free(file_buf);
        return 1;
    }

    author_acct = (short)atol(author_str);
    if (author_acct < 1) {
        json_error("Invalid --author account number");
        free(file_buf);
        return 1;
    }

    if (to_str) {
        to_acct = (short)atol(to_str);
        if (to_acct < 1) {
            json_error("Invalid --to account number");
            free(file_buf);
            return 1;
        }
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        free(file_buf);
        return 1;
    }

    /* Verify MsgBase type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        free(file_buf);
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_MSG_BASE) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a message base");
        free(file_buf);
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Look up author identity under SEM[1] shared. */
    author_name[0] = '\0';
    author_handle[0] = '\0';
    ObtainSemaphoreShared(&myp->SEM[1]);
    if (author_acct > 0 && author_acct <= (short)myp->Nums[0]) {
        strncpy(author_name,
            myp->Key[author_acct - 1].RealName,
            sizeof(author_name) - 1);
        author_name[sizeof(author_name) - 1] = '\0';
        strncpy(author_handle,
            myp->Key[author_acct - 1].Handle,
            sizeof(author_handle) - 1);
        author_handle[sizeof(author_handle) - 1] = '\0';
        author_id = myp->Key[author_acct - 1].IDNumber;
    }
    ReleaseSemaphore(&myp->SEM[1]);

    /* Validate that the author account was found. */
    if (author_id == 0) {
        json_error("Author account not found");
        free(file_buf);
        return 1;
    }

    /* Look up recipient identity if --to specified. */
    to_name[0] = '\0';
    to_handle[0] = '\0';
    if (to_acct > 0) {
        ObtainSemaphoreShared(&myp->SEM[1]);
        if (to_acct <= (short)myp->Nums[0]) {
            strncpy(to_name,
                myp->Key[to_acct - 1].RealName,
                sizeof(to_name) - 1);
            to_name[sizeof(to_name) - 1] = '\0';
            strncpy(to_handle,
                myp->Key[to_acct - 1].Handle,
                sizeof(to_handle) - 1);
            to_handle[sizeof(to_handle) - 1] = '\0';
            to_id = myp->Key[to_acct - 1].IDNumber;
        }
        ReleaseSemaphore(&myp->SEM[1]);
    }

    /* Load subboard data. */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        free(file_buf);
        return 1;
    }
    loaded = 1;

    /* Verify item exists using actual item count. */
    count = (long)sub->rn;
    if (item_index > count) {
        json_error("Item number out of range");
        rc = 1;
        goto cleanup;
    }

    /* Protect text allocation, linked list, and count increment. */
    ObtainSemaphore(sub->sem);
    sem_held = 1;

    /* Get a copy of the item under lock. */
    memset(&item, 0, sizeof(item));
    memset(&ihead, 0, sizeof(ihead));
    ZGetItem(&item, &ihead, sub, (short)(item_index - 1));

    old_last = item.Last;

    new_id = get_next_id(sub);

    /* Build HeaderType (body_pos/body_len are placeholders). */
    build_header_type(&hdr, author_name, author_handle,
        author_id, author_acct,
        to_acct > 0 ? to_name : NULL,
        to_acct > 0 ? to_handle : NULL,
        to_id, to_acct,
        0, 0, old_last, new_id);

    /* Write HeaderType and body text to _text. */
    if (write_message_text(sub, text_arg, &hdr,
            &header_pos, &err_msg) != 0) {
        json_error(err_msg);
        rc = 1;
        goto sem_release;
    }

    /*
     * Linked list update: patch old last HeaderType's Next field
     * to point to the new response's HeaderType.
     *
     * AmigaOS dos.library guarantees read-after-write coherence
     * across Close/Open boundaries from the same process.
     */
    if (old_last >= 0) {
        char text_path[256];
        BPTR fh;
        struct HeaderType old_hdr;
        long nread;

        build_text_path(text_path, sizeof(text_path),
            sub->DataPath);
        fh = Open((CONST_STRPTR)text_path, MODE_READWRITE);
        if (fh) {
            Seek(fh, old_last, OFFSET_BEGINNING);
            nread = Read(fh, (APTR)&old_hdr,
                (long)sizeof(struct HeaderType));
            if (nread == (long)sizeof(struct HeaderType) &&
                    old_hdr.Magic == HEADERTYPE_MAGIC) {
                long writ;
                old_hdr.Next = header_pos;
                Seek(fh, old_last, OFFSET_BEGINNING);
                writ = Write(fh, (APTR)&old_hdr,
                    (long)sizeof(struct HeaderType));
                if (writ != (long)sizeof(struct HeaderType)) {
                    {
                        char wbuf[128];
                        snprintf(wbuf, sizeof(wbuf),
                            "Linked-list patch write failed "
                            "(old_last=%ld, wrote=%ld)",
                            old_last, writ);
                        warn_add(wbuf);
                    }
                }
            }
            Close(fh);
        }
    }

    /* Update item to point to new last response. */
    item.Last = header_pos;
    ihead.Responses++;
    set_current_date(&ihead.RespDate);
    ZPutItem(&item, &ihead, sub, (short)(item_index - 1));

    /* Prepare MessageType3. */
    memset(&msg, 0, sizeof(msg));
    msg.ItemNumber = ihead.Number;
    msg.Seek = header_pos;
    msg.ByID = author_id;
    msg.ToID = to_id;
    msg.Number = new_id;
    set_current_date(&msg.PostDate);

    ZAddMessage(&msg, sub);

    sub->nNewMess++;

    /* Persist updated subboard to disk. */
    ObtainSemaphore(&myp->SEM[5]);
    write_subboard_disk((int)physnum, sub);
    ReleaseSemaphore(&myp->SEM[5]);

    /* Output confirmation. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "responded");
    json_kv_int(&js, "physnum", (long)physnum);
    json_kv_int(&js, "item_index", item_index);
    json_kv_int(&js, "item_number", (long)ihead.Number);
    json_kv_int(&js, "response_number", (long)new_id);
    json_kv_int(&js, "responses", ihead.Responses);
    json_kv_int(&js, "by_account", (long)author_acct);
    json_kv_int(&js, "header_offset", header_pos);
    json_kv_int(&js, "body_offset", hdr.Text);
    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

sem_release:
    if (sem_held) {
        ReleaseSemaphore(sub->sem);
        sem_held = 0;
    }

cleanup:
    if (loaded)
        OneLessUser(sub);
    free(file_buf);
    return rc;
}

/* ---- msg delete ---- */

int cmd_msg_delete(struct MainPort *myp, int argc, char **argv)
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
        json_error("Usage: cnet-cli msg delete <sub-id|gokey> "
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

    /* Verify MsgBase type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_MSG_BASE) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a message base");
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

/* ---- msg edit ---- */

int cmd_msg_edit(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char datebuf[24];
    short physnum;
    struct SubboardType4 *sub;
    struct ItemType3 item;
    struct ItemHeader ihead;
    struct HeaderType old_hdr;
    struct HeaderType new_hdr;
    int marker_base;
    const char *text_arg = NULL;
    const char *title_arg = NULL;
    const char *response_str = NULL;
    const char *err_msg;
    char *old_text = NULL;
    long item_index;
    long count;
    long old_first;
    long header_pos = 0;
    int hdr_result;
    int rc = 0;
    int i;
    int loaded = 0;
    int sem_held = 0;
    int title_changed = 0;
    int text_changed = 0;
    long response_n = 0;
    char text_path[256];
    const char *file_arg = NULL;
    char *file_buf = NULL;

    if (argc < 3) {
        json_error("Usage: cnet-cli msg edit <sub-id|gokey> "
            "<item-number> [--text \"...\" | --file <path>] "
            "[--title \"...\"] [--response N]");
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
        } else if (strcmp(argv[i], "--response") == 0 &&
                i + 1 < argc) {
            i++;
            response_str = argv[i];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            i++;
            file_arg = argv[i];
        }
    }

    if (text_arg && file_arg) {
        json_error("Cannot use both --text and --file");
        free(file_buf);
        return 1;
    }
    if (file_arg) {
        file_buf = read_text_file(file_arg, NULL);
        if (!file_buf) {
            json_error("Failed to read --file");
            return 1;
        }
        text_arg = file_buf;
    }

    if (!text_arg && !title_arg) {
        json_error("Required: --text (or --file) and/or --title");
        free(file_buf);
        return 1;
    }

    if (response_str) {
        response_n = atol(response_str);
        if (response_n < 1) {
            json_error("--response must be >= 1");
            free(file_buf);
            return 1;
        }
        if (!text_arg) {
            json_error("Required: --text (or --file) for response edit");
            free(file_buf);
            return 1;
        }
        /* Title is silently ignored for response edits. */
        title_arg = NULL;
    }

    physnum = resolve_subboard(myp, argv[1]);
    if (physnum < 0) {
        json_error("Subboard not found");
        free(file_buf);
        return 1;
    }

    /* Verify MsgBase type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        free(file_buf);
        return 1;
    }
    sub = &myp->Subboard[physnum];
    marker_base = sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_MSG_BASE) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard is not a message base");
        free(file_buf);
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /* Load subboard data. */
    if (!OneMoreUser(sub, (UBYTE)0)) {
        json_error("OneMoreUser failed (cannot load subboard data)");
        free(file_buf);
        return 1;
    }
    loaded = 1;

    count = (long)sub->rn;
    if (item_index > count) {
        json_error("Item number out of range");
        rc = 1;
        goto edit_cleanup;
    }

    /* Get a copy of the item. */
    memset(&item, 0, sizeof(item));
    memset(&ihead, 0, sizeof(ihead));
    ZGetItem(&item, &ihead, sub, (short)(item_index - 1));

    if (response_n > 0) {
        /*
         * Response edit path.
         * Find the N-th response matching this item's ihead.Number
         * in _Message3, then edit its body text.
         */
        struct MessageType3 *msgs;
        long msg_count = 0;
        long mi;
        long resp_found = 0;
        long target_record = -1;
        struct MessageType3 target_msg;
        long old_seek;

        msgs = load_messages(sub->DataPath, &msg_count);
        if (!msgs || msg_count == 0) {
            json_error("No responses found");
            rc = 1;
            if (msgs) free(msgs);
            goto edit_cleanup;
        }

        /* Find the N-th response for this item. */
        memset(&target_msg, 0, sizeof(target_msg));
        for (mi = 0; mi < msg_count; mi++) {
            if (msgs[mi].ItemNumber != ihead.Number)
                continue;
            resp_found++;
            if (resp_found == response_n) {
                target_msg = msgs[mi];
                target_record = mi;
                break;
            }
        }
        free(msgs);

        if (target_record < 0) {
            json_error("Response not found");
            rc = 1;
            goto edit_cleanup;
        }

        old_seek = target_msg.Seek;

        /* Read old HeaderType from _text at msg.Seek. */
        build_text_path(text_path, sizeof(text_path),
            sub->DataPath);
        memset(&old_hdr, 0, sizeof(old_hdr));
        hdr_result = read_header_and_text(text_path, old_seek,
            &old_hdr, &old_text, &err_msg);
        if (hdr_result == -1) {
            json_error(err_msg);
            rc = 1;
            goto edit_cleanup;
        }
        if (hdr_result == 0) {
            json_error("Cannot edit legacy-format message "
                "(no HeaderType)");
            rc = 1;
            goto edit_cleanup;
        }

        /* Protect text pool modifications. */
        ObtainSemaphore(sub->sem);
        sem_held = 1;

        /* Free old allocations. */
        FreeText(sub, old_hdr.Text, old_hdr.TextLen);
        FreeText(sub, old_seek,
            (long)sizeof(struct HeaderType));
        SaveFree(sub);

        /* Prepare new HeaderType preserving all fields. */
        new_hdr = old_hdr;
        set_current_date(&new_hdr.EditDate);
        new_hdr.Text = 0;
        new_hdr.TextLen = 0;

        /* Write new HeaderType + body to _text. */
        if (write_message_text(sub, text_arg, &new_hdr,
                &header_pos, &err_msg) != 0) {
            json_error(err_msg);
            rc = 1;
            goto edit_sem_release;
        }

        /* Patch linked list neighbors. */
        if (old_hdr.Previous >= 0)
            patch_header_link(sub, old_hdr.Previous,
                1, header_pos);
        if (old_hdr.Next >= 0)
            patch_header_link(sub, old_hdr.Next,
                0, header_pos);

        /* Update item.First/Last if they pointed to old_seek. */
        {
            int item_updated = 0;
            if (item.First == old_seek) {
                item.First = header_pos;
                item_updated = 1;
            }
            if (item.Last == old_seek) {
                item.Last = header_pos;
                item_updated = 1;
            }
            if (item_updated)
                ZPutItem(&item, &ihead, sub,
                    (short)(item_index - 1));
        }

        /* Update _Message3 record. */
        target_msg.Seek = header_pos;
        write_message3_record(sub->DataPath, target_record,
            &target_msg);

        text_changed = 1;

        /* Persist subboard. */
        ObtainSemaphore(&myp->SEM[5]);
        write_subboard_disk((int)physnum, sub);
        ReleaseSemaphore(&myp->SEM[5]);

        /* Emit JSON for response edit. */
        json_init(&js, stdout);
        json_obj_open(&js);
        json_kv_str(&js, "status", "edited");
        json_kv_int(&js, "physnum", (long)physnum);
        json_kv_int(&js, "item_index", item_index);
        json_kv_int(&js, "item_number", (long)ihead.Number);
        json_kv_int(&js, "response", response_n);
        json_kv_int(&js, "response_number",
            (long)target_msg.Number);
        json_kv_int(&js, "header_offset", header_pos);
        json_kv_int(&js, "body_offset", new_hdr.Text);
        json_kv_str(&js, "edit_date",
            format_date(datebuf, sizeof(datebuf),
                &new_hdr.EditDate));
        json_kv_bool(&js, "text_changed", text_changed);
        json_obj_close(&js);
        json_finish(&js);

        goto edit_sem_release;
    }

    /*
     * Item post edit path (no --response).
     */

    /* Handle --title update. */
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
        title_changed = 1;
    }

    /* If no --text, just save the title change and finish. */
    if (!text_arg) {
        if (title_changed)
            ZPutItem(&item, &ihead, sub,
                (short)(item_index - 1));
        goto emit_edit_json;
    }

    /* Verify item has text content. */
    if (item.First < 0) {
        json_error("Item has no text content");
        rc = 1;
        goto edit_cleanup;
    }

    old_first = item.First;

    /* Read old HeaderType via compat reader. */
    build_text_path(text_path, sizeof(text_path), sub->DataPath);
    memset(&old_hdr, 0, sizeof(old_hdr));
    hdr_result = read_header_and_text(text_path, item.First,
        &old_hdr, &old_text, &err_msg);
    if (hdr_result == -1) {
        json_error(err_msg);
        rc = 1;
        goto edit_cleanup;
    }
    if (hdr_result == 0) {
        json_error("Cannot edit legacy-format message "
            "(no HeaderType)");
        rc = 1;
        goto edit_cleanup;
    }

    /* Protect text pool modifications. */
    ObtainSemaphore(sub->sem);
    sem_held = 1;

    /* Free old allocations. */
    FreeText(sub, old_hdr.Text, old_hdr.TextLen);
    FreeText(sub, item.First, (long)sizeof(struct HeaderType));
    SaveFree(sub);

    /* Prepare new HeaderType preserving all fields. */
    new_hdr = old_hdr;
    set_current_date(&new_hdr.EditDate);
    new_hdr.Text = 0;
    new_hdr.TextLen = 0;

    /* Write new HeaderType + body to _text. */
    if (write_message_text(sub, text_arg, &new_hdr,
            &header_pos, &err_msg) != 0) {
        json_error(err_msg);
        rc = 1;
        goto edit_sem_release;
    }

    /* Update item pointers. */
    item.First = header_pos;
    if (item.Last == old_first)
        item.Last = header_pos;
    ZPutItem(&item, &ihead, sub, (short)(item_index - 1));

    /* Patch linked list: if old_hdr.Next exists, update
     * its Previous to point to the new header position. */
    if (old_hdr.Next >= 0)
        patch_header_link(sub, old_hdr.Next, 0, header_pos);

    text_changed = 1;

emit_edit_json:
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
    if (text_changed) {
        json_kv_int(&js, "header_offset", header_pos);
        json_kv_int(&js, "body_offset", new_hdr.Text);
        json_kv_str(&js, "edit_date",
            format_date(datebuf, sizeof(datebuf),
                &new_hdr.EditDate));
    }
    json_kv_bool(&js, "title_changed", title_changed);
    json_kv_bool(&js, "text_changed", text_changed);
    json_obj_close(&js);
    json_finish(&js);

edit_sem_release:
    if (sem_held) {
        ReleaseSemaphore(sub->sem);
        sem_held = 0;
    }

edit_cleanup:
    if (old_text)
        free(old_text);
    if (loaded)
        OneLessUser(sub);
    free(file_buf);
    return rc;
}

/* ---- msg search ---- */

int cmd_msg_search(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char buf[128];
    char datebuf[24];
    const char *query = NULL;
    const char *sub_arg = NULL;
    const char *from_str = NULL;
    const char *field_str = NULL;
    long limit = 100;
    short from_acct = 0;
    int field_text = 0;
    int field_by = 0;
    int total_matches = 0;
    int subs_searched = 0;
    int i;

    /*
     * Physnum list: collect target subboards first, then iterate.
     * Maximum 256 subboards is generous for any CNet BBS.
     */
#define SEARCH_MAX_SUBS 256
    short physnums[SEARCH_MAX_SUBS];
    int nsubs = 0;
    short skipped[SEARCH_MAX_SUBS];
    int nskipped = 0;

    if (argc < 2) {
        json_error("Usage: cnet-cli msg search <query> "
            "[--sub <id|gokey>] [--limit N] "
            "[--field title|text|by] [--from <account>]");
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
        } else if (strcmp(argv[i], "--from") == 0 &&
                i + 1 < argc) {
            i++;
            from_str = argv[i];
        }
    }

    if (limit <= 0)
        limit = 100;

    if (field_str) {
        if (strcmp(field_str, "text") == 0)
            field_text = 1;
        else if (strcmp(field_str, "by") == 0)
            field_by = 1;
        /* default "title" needs no flag */
    }

    if (from_str) {
        from_acct = (short)atol(from_str);
        if (from_acct < 1) {
            json_error("Invalid --from account number");
            return 1;
        }
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
            if (mb != MRK_MSG_BASE) {
                ReleaseSemaphore(&myp->SEM[5]);
                json_error("Subboard is not a message base");
                return 1;
            }
        }
        ReleaseSemaphore(&myp->SEM[5]);

        physnums[0] = pn;
        nsubs = 1;
    } else {
        ObtainSemaphoreShared(&myp->SEM[5]);
        for (i = 0; i < (int)myp->ns && nsubs < SEARCH_MAX_SUBS;
                i++) {
            struct SubboardType4 *s = &myp->Subboard[i];
            int mb = s->Marker & MRK_SUBBOARD_BASE;
            if (s->Marker & MRK_SUBBOARD_KILLED)
                continue;
            if (mb != MRK_MSG_BASE)
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
        field_text ? "text" : (field_by ? "by" : "title"));
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
            if (nskipped < SEARCH_MAX_SUBS)
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

            /* Apply --from filter first (cheap). */
            if (from_acct > 0 && ip->ByAccount != from_acct)
                continue;

            /* Match based on field. */
            if (field_by) {
                /*
                 * --field by: match by handle against query.
                 * If --from is set, we already filtered above
                 * and the query string is supplementary.
                 */
                if (from_acct > 0)
                    match = 1;
                else
                    match = ci_contains(
                        lookup_handle(myp, ip->ByAccount),
                        query);
            } else if (field_text) {
                /* Match against body text. */
                char tpath[256];
                char *body = NULL;
                struct HeaderType th;
                const char *em;

                if (ip->First < 0)
                    continue;

                build_text_path(tpath, sizeof(tpath),
                    s->DataPath);
                memset(&th, 0, sizeof(th));
                if (read_header_and_text(tpath, ip->First,
                        &th, &body, &em) >= 0 && body) {
                    match = ci_contains(body, query);
                    free(body);
                }
            } else {
                /* Default: title search. */
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
            json_kv_int(&js, "responses", ih->Responses);
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

/* ---- msg move ---- */

/*
 * Temporary storage for a single response's data during move.
 */
struct move_response {
    struct HeaderType hdr;
    char *body;
    struct MessageType3 msg;
};

int cmd_msg_move(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char buf[128];
    short src_physnum, dst_physnum;
    struct SubboardType4 *src_sub, *dst_sub;
    struct ItemType3 src_item;
    struct ItemHeader src_ihead;
    struct HeaderType src_hdr;
    char *src_body = NULL;
    int marker_base;
    long item_index;
    long count;
    const char *err_msg;
    int hdr_result;
    int rc = 0;

    /* Response data. */
    struct move_response *resps = NULL;
    int resp_count = 0;
    int resp_alloc = 0;

    /* Destination state. */
    struct ItemType3 new_item;
    struct ItemHeader new_ihead;
    struct HeaderType new_hdr;
    ULONG new_item_id;
    long new_header_pos = 0;
    int dst_loaded = 0;
    int dst_sem_held = 0;
    int phase2_failed = 0;
    int responses_copied = 0;
    int resp_count_mismatch = 0;

    if (argc < 4) {
        json_error("Usage: cnet-cli msg move <src-sub> "
            "<item-number> <dst-sub>");
        return 1;
    }

    item_index = atol(argv[2]);
    if (item_index < 1) {
        json_error("Item number must be >= 1");
        return 1;
    }

    src_physnum = resolve_subboard(myp, argv[1]);
    if (src_physnum < 0) {
        json_error("Source subboard not found");
        return 1;
    }

    dst_physnum = resolve_subboard(myp, argv[3]);
    if (dst_physnum < 0) {
        json_error("Destination subboard not found");
        return 1;
    }

    if (src_physnum == dst_physnum) {
        json_error("Source and destination subboards are "
            "the same");
        return 1;
    }

    /* Verify both subboards are MsgBase type. */
    ObtainSemaphoreShared(&myp->SEM[5]);
    if (src_physnum >= (short)myp->ns ||
            dst_physnum >= (short)myp->ns) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Subboard number out of range");
        return 1;
    }
    src_sub = &myp->Subboard[src_physnum];
    dst_sub = &myp->Subboard[dst_physnum];
    marker_base = src_sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_MSG_BASE) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Source subboard is not a message base");
        return 1;
    }
    marker_base = dst_sub->Marker & MRK_SUBBOARD_BASE;
    if (marker_base != MRK_MSG_BASE) {
        ReleaseSemaphore(&myp->SEM[5]);
        json_error("Destination subboard is not a message base");
        return 1;
    }
    ReleaseSemaphore(&myp->SEM[5]);

    /*
     * Step 1: Read source data.
     */
    if (!OneMoreUser(src_sub, (UBYTE)0)) {
        json_error("OneMoreUser failed for source subboard");
        return 1;
    }

    count = (long)src_sub->rn;
    if (item_index > count) {
        json_error("Item number out of range");
        OneLessUser(src_sub);
        return 1;
    }

    memset(&src_item, 0, sizeof(src_item));
    memset(&src_ihead, 0, sizeof(src_ihead));
    ZGetItem(&src_item, &src_ihead, src_sub,
        (short)(item_index - 1));

    /* Guard: killed item check. */
    if (src_ihead.Killed) {
        json_error("Source item is killed");
        OneLessUser(src_sub);
        return 1;
    }

    /* Read original post HeaderType + body from _text. */
    memset(&src_hdr, 0, sizeof(src_hdr));
    if (src_item.First < 0) {
        json_error("Source item has no text content");
        OneLessUser(src_sub);
        return 1;
    }

    {
        char tp[256];
        build_text_path(tp, sizeof(tp), src_sub->DataPath);
        hdr_result = read_header_and_text(tp,
            src_item.First, &src_hdr, &src_body, &err_msg);
    }
    if (hdr_result == -1) {
        json_error(err_msg);
        OneLessUser(src_sub);
        return 1;
    }
    if (hdr_result == 0) {
        json_error("Cannot move legacy-format message "
            "(no HeaderType)");
        if (src_body) free(src_body);
        OneLessUser(src_sub);
        return 1;
    }

    /* Read all responses. */
    if (src_ihead.Number != 0) {
        /* Strategy A: _Message3 scan (proven path for valid IDs). */
        struct MessageType3 *msgs;
        long msg_count = 0;
        long mi;

        msgs = load_messages(src_sub->DataPath, &msg_count);
        if (msgs && msg_count > 0) {
            /* Count matching responses first. */
            resp_alloc = 0;
            for (mi = 0; mi < msg_count; mi++) {
                if (msgs[mi].ItemNumber == src_ihead.Number)
                    resp_alloc++;
            }

            if (resp_alloc > 0) {
                resps = (struct move_response *)malloc(
                    (unsigned long)resp_alloc *
                    sizeof(struct move_response));
                if (!resps) {
                    free(msgs);
                    if (src_body) free(src_body);
                    OneLessUser(src_sub);
                    json_error("Out of memory");
                    return 1;
                }
                memset(resps, 0,
                    (unsigned long)resp_alloc *
                    sizeof(struct move_response));

                resp_count = 0;
                for (mi = 0; mi < msg_count; mi++) {
                    char tp[256];
                    struct HeaderType rh;
                    char *rb = NULL;
                    int rr;

                    if (msgs[mi].ItemNumber !=
                            src_ihead.Number)
                        continue;

                    build_text_path(tp, sizeof(tp),
                        src_sub->DataPath);
                    memset(&rh, 0, sizeof(rh));
                    rr = read_header_and_text(tp,
                        msgs[mi].Seek, &rh, &rb, &err_msg);
                    if (rr < 0 || !rb) {
                        /* Skip unreadable responses. */
                        if (rb) free(rb);
                        continue;
                    }

                    resps[resp_count].hdr = rh;
                    resps[resp_count].body = rb;
                    resps[resp_count].msg = msgs[mi];
                    resp_count++;
                }
            }
        }
        if (msgs) free(msgs);
    } else {
        /* Strategy B: linked list walk for legacy items (Number==0).
         * Follow HeaderType.Next from the original post through
         * all responses. This avoids the _Message3 ItemNumber
         * ambiguity when multiple items share Number==0. */
        char tp[256];
        long pos;
        int capacity = 0;

        build_text_path(tp, sizeof(tp), src_sub->DataPath);

        /* Start from original post, skip to first response. */
        pos = src_hdr.Next;

        /* First pass: count responses by walking the chain.
         * MAX_RESPONSES_WALK bounds the loop to prevent infinite
         * iteration from circular chains or corrupted pointers. */
        {
            long walk = pos;
            while (walk >= 0 && capacity < MAX_RESPONSES_WALK) {
                struct HeaderType wh;
                memset(&wh, 0, sizeof(wh));
                if (read_header_only(tp, walk, &wh) != 0)
                    break;
                capacity++;
                walk = wh.Next;
            }
        }

        if (capacity > 0) {
            resps = (struct move_response *)malloc(
                (unsigned long)capacity *
                sizeof(struct move_response));
            if (!resps) {
                if (src_body) free(src_body);
                OneLessUser(src_sub);
                json_error("Out of memory");
                return 1;
            }
            memset(resps, 0,
                (unsigned long)capacity *
                sizeof(struct move_response));
            resp_alloc = capacity;

            /* Second pass: read each response. */
            resp_count = 0;
            pos = src_hdr.Next;
            while (pos >= 0 && resp_count < resp_alloc) {
                struct HeaderType rh;
                char *rb = NULL;
                int rr;

                memset(&rh, 0, sizeof(rh));
                rr = read_header_and_text(tp, pos,
                    &rh, &rb, &err_msg);
                if (rr < 0 || !rb) {
                    if (rb) free(rb);
                    pos = -1; /* Cannot continue chain. */
                    break;
                }

                resps[resp_count].hdr = rh;
                resps[resp_count].body = rb;

                /* Synthesize MessageType3 from HeaderType. */
                memset(&resps[resp_count].msg, 0,
                    sizeof(struct MessageType3));
                resps[resp_count].msg.ItemNumber = 0;
                resps[resp_count].msg.Seek = pos;
                resps[resp_count].msg.ByID = rh.ByID;
                resps[resp_count].msg.ToID = rh.ToID;
                resps[resp_count].msg.Number = rh.Number;
                resps[resp_count].msg.PostDate = rh.PostDate;
                resps[resp_count].msg.Imported = rh.Imported;

                resp_count++;
                pos = rh.Next;
            }
        }
    }

    /* Check for response count mismatch. */
    if (resp_count != (int)src_ihead.Responses)
        resp_count_mismatch = 1;

    OneLessUser(src_sub);

    /*
     * Step 2: Write to destination.
     */
    if (!OneMoreUser(dst_sub, (UBYTE)0)) {
        json_error("OneMoreUser failed for destination subboard");
        rc = 1;
        goto move_free_temp;
    }
    dst_loaded = 1;

    ObtainSemaphore(dst_sub->sem);
    dst_sem_held = 1;

    /* Get new unique ID for original post. */
    new_item_id = get_next_id(dst_sub);

    /* Build new HeaderType from source data (struct copy
     * preserves all fields including other[] and unknown). */
    new_hdr = src_hdr;
    new_hdr.Magic = HEADERTYPE_MAGIC;
    new_hdr.Number = new_item_id;
    new_hdr.Text = 0;
    new_hdr.TextLen = 0;
    new_hdr.Next = -1;
    new_hdr.Previous = -1;

    /* Write original post to destination _text. */
    if (write_message_text(dst_sub, src_body, &new_hdr,
            &new_header_pos, &err_msg) != 0) {
        json_error(err_msg);
        rc = 1;
        goto move_dst_release;
    }

    /* Prepare new ItemType3. */
    memset(&new_item, 0, sizeof(new_item));
    strncpy(new_item.Title, src_item.Title,
        sizeof(new_item.Title) - 1);
    new_item.Title[sizeof(new_item.Title) - 1] = '\0';
    new_item.ByAccount = src_item.ByAccount;
    new_item.ByID = src_item.ByID;
    new_item.ToID = src_item.ToID;
    new_item.Validated = src_item.Validated;
    new_item.Finished = src_item.Finished;
    new_item.PurgeKill = src_item.PurgeKill;
    new_item.PurgeStatus = dst_sub->PurgeStatus;
    new_item.DLnotifyULer = dst_sub->DLnotifyULer;
    new_item.First = new_header_pos;
    new_item.Last = new_header_pos;

    /* Prepare new ItemHeader. */
    memset(&new_ihead, 0, sizeof(new_ihead));
    new_ihead.Number = new_item_id;
    new_ihead.Size = 0;
    new_ihead.Responses = 0;
    new_ihead.PostDate = src_ihead.PostDate;
    new_ihead.RespDate = src_ihead.PostDate;

    /* Build TitleSort from Title. */
    {
        int ti;
        for (ti = 0; ti < 8 && new_item.Title[ti]; ti++)
            new_ihead.TitleSort[ti] = (UBYTE)toupper(
                (unsigned char)new_item.Title[ti]);
        new_ihead.TitleSort[ti] = '\0';
    }

    /* Add item to destination. */
    if (!ZAddItem(&new_item, &new_ihead, dst_sub)) {
        FreeText(dst_sub, new_hdr.Text, new_hdr.TextLen);
        FreeText(dst_sub, new_header_pos,
            (long)sizeof(struct HeaderType));
        SaveFree(dst_sub);
        json_error("ZAddItem failed in destination");
        rc = 1;
        goto move_dst_release;
    }

    /* Write responses (in source order). */
    {
        long prev_hpos = new_header_pos;
        int ri;

        for (ri = 0; ri < resp_count; ri++) {
            struct HeaderType resp_new;
            struct MessageType3 resp_msg;
            ULONG resp_id;
            long resp_hpos = 0;

            resp_id = get_next_id(dst_sub);

            /* Build response HeaderType (struct copy
             * preserves all fields). */
            resp_new = resps[ri].hdr;
            resp_new.Magic = HEADERTYPE_MAGIC;
            resp_new.Number = resp_id;
            resp_new.Text = 0;
            resp_new.TextLen = 0;
            resp_new.Previous = prev_hpos;
            resp_new.Next = -1;

            if (write_message_text(dst_sub, resps[ri].body,
                    &resp_new, &resp_hpos, &err_msg) != 0) {
                phase2_failed = 1;
                break;
            }

            /* Patch previous HeaderType's Next. */
            patch_header_link(dst_sub, prev_hpos, 1,
                resp_hpos);

            /* Build MessageType3. */
            memset(&resp_msg, 0, sizeof(resp_msg));
            resp_msg.ItemNumber = new_item_id;
            resp_msg.Seek = resp_hpos;
            resp_msg.ByID = resps[ri].msg.ByID;
            resp_msg.ToID = resps[ri].msg.ToID;
            resp_msg.Number = resp_id;
            resp_msg.PostDate = resps[ri].msg.PostDate;
            resp_msg.Imported = resps[ri].msg.Imported;
            ZAddMessage(&resp_msg, dst_sub);

            new_ihead.Responses++;
            new_ihead.RespDate = resps[ri].msg.PostDate;
            new_item.Last = resp_hpos;
            dst_sub->nNewMess++;
            responses_copied++;
            prev_hpos = resp_hpos;
        }
    }

    /* Write back updated item with correct Last/Responses. */
    ZPutItem(&new_item, &new_ihead, dst_sub,
        (short)(dst_sub->rn - 1));

    /* Persist destination. */
    ObtainSemaphore(&myp->SEM[5]);
    write_subboard_disk((int)dst_physnum, dst_sub);
    ReleaseSemaphore(&myp->SEM[5]);

move_dst_release:
    if (dst_sem_held) {
        ReleaseSemaphore(dst_sub->sem);
        dst_sem_held = 0;
    }
    if (dst_loaded) {
        OneLessUser(dst_sub);
        dst_loaded = 0;
    }

    if (rc != 0)
        goto move_free_temp;

    /*
     * Step 3: Delete source (skipped on partial failure).
     */
    if (!phase2_failed) {
        if (OneMoreUser(src_sub, (UBYTE)0)) {
            struct ItemType3 del_item;
            struct ItemHeader del_ihead;

            memset(&del_item, 0, sizeof(del_item));
            memset(&del_ihead, 0, sizeof(del_ihead));
            ZGetItem(&del_item, &del_ihead, src_sub,
                (short)(item_index - 1));
            del_ihead.Killed = 1;
            ZPutItem(&del_item, &del_ihead, src_sub,
                (short)(item_index - 1));
            OneLessUser(src_sub);
        }
    }

    /* Emit JSON output. */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status",
        phase2_failed ? "partial_move" : "moved");
    if (phase2_failed)
        json_kv_str(&js, "warning",
            "Response write failed during copy. "
            "Source item preserved. "
            "Destination has partial copy.");

    json_key(&js, "source");
    json_obj_open(&js);
    json_kv_int(&js, "physnum", (long)src_physnum);
    json_kv_str(&js, "subboard",
        strip_mci(buf, sizeof(buf), src_sub->SubDirName));
    json_kv_int(&js, "item_index", item_index);
    json_kv_int(&js, "item_number", (long)src_ihead.Number);
    json_kv_int(&js, "responses", (long)resp_count);
    if (phase2_failed)
        json_kv_bool(&js, "killed", 0);
    json_obj_close(&js);

    json_key(&js, "destination");
    json_obj_open(&js);
    json_kv_int(&js, "physnum", (long)dst_physnum);
    json_kv_str(&js, "subboard",
        strip_mci(buf, sizeof(buf), dst_sub->SubDirName));
    json_kv_int(&js, "item_index", (long)dst_sub->rn);
    json_kv_int(&js, "new_item_number", (long)new_item_id);
    json_kv_int(&js, "responses_copied",
        (long)responses_copied);
    json_kv_int(&js, "header_offset", new_header_pos);
    json_obj_close(&js);

    if (resp_count_mismatch) {
        json_kv_str(&js, "response_count_warning",
            "Response count from _Message3 does not match "
            "ihead.Responses");
        json_kv_int(&js, "ihead_responses",
            (long)src_ihead.Responses);
        json_kv_int(&js, "actual_responses",
            (long)resp_count);
    }

    json_obj_close(&js);
    json_finish(&js);

move_free_temp:
    if (src_body)
        free(src_body);
    if (resps) {
        int ri;
        for (ri = 0; ri < resp_count; ri++) {
            if (resps[ri].body)
                free(resps[ri].body);
        }
        free(resps);
    }
    return rc;
}
