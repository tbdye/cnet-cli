/*
 * mail.c -- Mail commands for cnet-cli
 *
 * Mail operations: send, list, read, reply, delete, folders, count, feedback, verify
 *
 * All mail operations use direct file I/O against _mhead4 (810-byte
 * records) and _mtext4 (body text). Per-account semaphores from
 * GetMailSems() protect concurrent file access.
 *
 * The on-disk mail header format (810 bytes) does NOT match the SDK's
 * MailHeader4 struct. We use our own MailHeaderDisk struct verified
 * against the live BBS.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>

#include "mail.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;
extern struct Library *CNetMailBase;

/* ---- On-disk mail header layout (810 bytes) ---- */

/*
 * WARNING: This does NOT match the SDK struct MailHeader4 (528 bytes).
 * Verified empirically against live BBS mail files.
 */
#define MHEAD_RECORD_SIZE 810

struct MailHeaderDisk {
    UBYTE  unknown_0[4];      /*   0: Record's own byte offset (or 0) */
    UBYTE  send_date[4];      /*   4: Unix timestamp, big-endian ULONG */
    UBYTE  read_date[6];      /*   8: IsDate format, all zeros = unread */
    char   subject[80];       /*  14: Null-terminated, padded to 80 bytes */
    UBYTE  _gap94;            /*  94: Always zero */
    char   from_name[27];     /*  95: Null-terminated sender name */
    UBYTE  send_date2[4];     /* 122: Copy of send_date */
    UBYTE  from_account[2];   /* 126: short, big-endian */
    UBYTE  original_date[4];  /* 128: Unix timestamp, big-endian ULONG */
    char   folder_name[32];   /* 132: Destination folder, null-terminated */
    UBYTE  _unknown164[168];  /* 164: Zeros (ToName, UUCP_From, UUCP_To area) */
    UBYTE  flags_or_date[4];  /* 332: Multi-purpose (0, timestamp, or -2) */
    UBYTE  _unknown336[8];    /* 336: Always zero */
    UBYTE  filed_date[4];     /* 344: Unix timestamp, big-endian ULONG */
    UBYTE  _unknown348[126];  /* 348: Always zero */
    UBYTE  length[2];         /* 474: USHORT, body text length, big-endian */
    UBYTE  _unknown476[308];  /* 476: Always zero */
    UBYTE  unknown_784;       /* 784: Purpose unclear (0, 1, or 25 observed) */
    UBYTE  unknown_785;       /* 785: Always 0 */
    char   reg_key[14];       /* 786: Registration key string */
    UBYTE  seek[4];           /* 800: ULONG, offset into _mtext4, big-endian */
    UBYTE  _padding804[6];    /* 804: Always zero, pad to 810 */
};

_Static_assert(sizeof(struct MailHeaderDisk) == 810,
    "MailHeaderDisk must be 810 bytes (on-disk format)");

/* ---- Byte-order helpers ---- */

/* Read big-endian ULONG from raw bytes */
static ULONG read_be_ulong(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

/* Read big-endian USHORT from raw bytes */
static USHORT read_be_ushort(const UBYTE *p)
{
    return (USHORT)(((USHORT)p[0] << 8) | (USHORT)p[1]);
}

/* Write big-endian ULONG to raw bytes */
static void write_be_ulong(UBYTE *p, ULONG val)
{
    p[0] = (UBYTE)(val >> 24);
    p[1] = (UBYTE)(val >> 16);
    p[2] = (UBYTE)(val >> 8);
    p[3] = (UBYTE)val;
}

/* Write big-endian USHORT to raw bytes */
static void write_be_ushort(UBYTE *p, USHORT val)
{
    p[0] = (UBYTE)(val >> 8);
    p[1] = (UBYTE)val;
}

/* Days from Unix epoch (1970-01-01) to Amiga epoch (1978-01-01) */
#define AMIGA_UNIX_EPOCH_OFFSET 252460800UL  /* 2922 days * 86400 */

static ULONG get_unix_time(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return AMIGA_UNIX_EPOCH_OFFSET
         + (ULONG)ds.ds_Days * 86400UL
         + (ULONG)ds.ds_Minute * 60UL
         + (ULONG)ds.ds_Tick / 50UL;
}

/* Check if ReadDate is all zeros (unread) */
static int is_unread(const struct MailHeaderDisk *hdr)
{
    int i;
    for (i = 0; i < 6; i++) {
        if (hdr->read_date[i] != 0) return 0;
    }
    return 1;
}

/* ---- Path construction ---- */

/*
 * Build the full path to _mhead4 or _mtext4 within a mail folder.
 * CreateFolderName does NOT append a trailing '/'.
 * We add the separator between the folder path and filename.
 */
static void build_folder_file_path(char *buf, int bufsz,
    const char *folder_path, const char *filename)
{
    int len = (int)strlen(folder_path);
    if (len > 0 && folder_path[len - 1] == '/')
        snprintf(buf, bufsz, "%s%s", folder_path, filename);
    else
        snprintf(buf, bufsz, "%s/%s", folder_path, filename);
}

/* ---- User resolution for mail ---- */

/*
 * Resolve a user identifier to both account number and UUCP name.
 * Uses resolve_user_full() from util.c.
 *
 * Returns 0 on success, -1 on failure.
 */
static int resolve_user_mail(struct MainPort *myp,
    const char *id_or_handle, short *out_account,
    char *out_uucp, int uucp_bufsz)
{
    short account;

    account = resolve_user_full(myp, id_or_handle,
        out_uucp, uucp_bufsz);
    if (account < 1)
        return -1;
    if (out_uucp[0] == '\0')
        return -1;  /* no UUCP name for this account */

    *out_account = account;
    return 0;
}

/* ---- Direct mail write ---- */

/*
 * Write a mail message directly to a user's mail folder via file I/O.
 * Handles both new mail (to INBOX) and sent copies (to SENTMAIL).
 *
 * Returns 0 on success, -1 on failure.
 * Caller must NOT hold any mail semaphores when calling.
 */
static int write_mail_direct(struct MainPort *myp,
    const char *rcpt_uucp, short rcpt_account,
    short from_account, const char *from_handle,
    const char *subject, const char *body, USHORT body_len,
    ULONG original_date, const char *folder,
    const char *label)
{
    char folder_path[256];
    char mhead_path[300];
    char mtext_path[300];
    char errbuf[80];
    struct SignalSemaphore *sems;
    BPTR lock_test;
    int file_exists;
    long mhead_file_size = 0;
    ULONG new_seek = 0;
    ULONG now;
    int rc = -1;
    BPTR fh;
    long nwritten;

    /* Non-reentrant (C1), safe in single-threaded cnet-cli */
    static struct MailHeaderDisk hdr;

    /* Parameter validation */
    if (from_account < 1 || rcpt_account < 1)
        return -1;
    if (rcpt_account > (short)myp->Nums[0])
        return -1;

    /* Build folder path */
    CreateMailDir((char *)rcpt_uucp);
    CreateFolderName(folder_path, (char *)rcpt_uucp, (char *)folder);
    /* Return value intentionally unchecked (matches codebase pattern;
     * upstream resolve_user_mail() already validates UUCP is non-empty) */
    BuildDir(folder_path);

    build_folder_file_path(mhead_path, (int)sizeof(mhead_path),
        folder_path, "_mhead4");
    build_folder_file_path(mtext_path, (int)sizeof(mtext_path),
        folder_path, "_mtext4");

    /* Acquire exclusive mail semaphore */
    sems = GetMailSems();
    ObtainSemaphore(&sems[rcpt_account - 1]);

    /* Step 1: Check _mhead4 existence and get size */
    lock_test = Lock((CONST_STRPTR)mhead_path, SHARED_LOCK);
    if (lock_test) {
        UnLock(lock_test);
        mhead_file_size = FileSize(mhead_path);
        if (mhead_file_size < 0)
            mhead_file_size = 0;
    } else {
        mhead_file_size = 0;
    }

    /* Step 2: Write body to _mtext4 */
    if (body_len > 0) {
        /*
         * Use Lock to check file existence. FileSize from
         * cnet.library returns 0 (not -1) for non-existent
         * files, making it unreliable for existence checks.
         */
        lock_test = Lock((CONST_STRPTR)mtext_path, SHARED_LOCK);
        file_exists = (lock_test != 0);
        if (lock_test)
            UnLock(lock_test);

        if (!file_exists) {
            fh = Open((CONST_STRPTR)mtext_path, MODE_NEWFILE);
            new_seek = 0;
        } else {
            fh = Open((CONST_STRPTR)mtext_path, MODE_OLDFILE);
        }

        if (!fh) {
            snprintf(errbuf, sizeof(errbuf),
                "Failed to open %s text file", label);
            json_error(errbuf);
            goto cleanup;
        }

        if (file_exists) {
            /* Seek to end, then back to start to get file size,
             * then to end again for append. Matches the pattern
             * used by read_mhead_records(). */
            Seek(fh, 0, OFFSET_END);
            new_seek = (ULONG)Seek(fh, 0, OFFSET_BEGINNING);
            Seek(fh, 0, OFFSET_END);
        }

        nwritten = Write(fh, (APTR)body, (long)body_len);
        Close(fh);

        if (nwritten != (long)body_len) {
            snprintf(errbuf, sizeof(errbuf),
                "Failed to write body to %s mailbox", label);
            json_error(errbuf);
            goto cleanup;
        }
    }
    /* If body_len == 0, new_seek stays 0 */

    /* Step 3: Construct 810-byte header */
    now = get_unix_time();
    memset(&hdr, 0, sizeof(hdr));

    /* +0: Record offset in _mhead4 */
    write_be_ulong(hdr.unknown_0, (ULONG)mhead_file_size);

    /* +4, +122: Send timestamps */
    write_be_ulong(hdr.send_date, now);
    write_be_ulong(hdr.send_date2, now);

    /* +14: Subject */
    strncpy(hdr.subject, subject, 79);

    /* +95: Sender name */
    strncpy(hdr.from_name, from_handle, 26);

    /* +126: Sender account */
    write_be_ushort(hdr.from_account, (USHORT)from_account);

    /* +128: Original date */
    write_be_ulong(hdr.original_date,
        original_date ? original_date : now);

    /* +344: Filed date */
    write_be_ulong(hdr.filed_date, now);

    /* +474: Body length */
    write_be_ushort(hdr.length, (USHORT)body_len);

    /* +786: Registration key (exactly 13 bytes, remaining byte zeroed by memset) */
    memcpy(hdr.reg_key, "1234CNET56789", 13);

    /* +800: Seek offset into _mtext4 */
    write_be_ulong(hdr.seek, new_seek);

    /* Step 4: Append header to _mhead4 */
    lock_test = Lock((CONST_STRPTR)mhead_path, SHARED_LOCK);
    file_exists = (lock_test != 0);
    if (lock_test)
        UnLock(lock_test);

    if (!file_exists) {
        fh = Open((CONST_STRPTR)mhead_path, MODE_NEWFILE);
    } else {
        fh = Open((CONST_STRPTR)mhead_path, MODE_OLDFILE);
    }

    if (!fh) {
        snprintf(errbuf, sizeof(errbuf),
            "Failed to open %s header file", label);
        json_error(errbuf);
        goto cleanup;
    }

    if (file_exists)
        Seek(fh, 0, OFFSET_END);

    nwritten = Write(fh, (APTR)&hdr, MHEAD_RECORD_SIZE);
    Close(fh);

    if (nwritten != MHEAD_RECORD_SIZE) {
        snprintf(errbuf, sizeof(errbuf),
            "Failed to write header to %s mailbox", label);
        json_error(errbuf);
        goto cleanup;
    }

    rc = 0;

cleanup:
    ReleaseSemaphore(&sems[rcpt_account - 1]);
    return rc;
}

/* ---- File I/O helpers ---- */

/*
 * Read all mail header records from a _mhead4 file.
 * Returns a malloc'd array of MailHeaderDisk records.
 * Sets *out_count to the number of records loaded.
 * Returns NULL on failure or if no records exist.
 */
static struct MailHeaderDisk *read_mhead_records(const char *mhead_path,
    long *out_count)
{
    BPTR fh;
    long file_size;
    long rec_count;
    struct MailHeaderDisk *buf;
    long nread;

    fh = Open((CONST_STRPTR)mhead_path, MODE_OLDFILE);
    if (!fh) {
        *out_count = 0;
        return NULL;
    }

    Seek(fh, 0, OFFSET_END);
    file_size = Seek(fh, 0, OFFSET_BEGINNING);
    if (file_size <= 0 || file_size % MHEAD_RECORD_SIZE != 0) {
        Close(fh);
        *out_count = 0;
        return NULL;
    }

    rec_count = file_size / MHEAD_RECORD_SIZE;

    buf = (struct MailHeaderDisk *)malloc(
        (unsigned long)file_size);
    if (!buf) {
        Close(fh);
        *out_count = 0;
        return NULL;
    }

    nread = Read(fh, (APTR)buf, file_size);
    Close(fh);

    if (nread != file_size) {
        free(buf);
        *out_count = 0;
        return NULL;
    }

    *out_count = rec_count;
    return buf;
}

/*
 * Read the body text for a specific mail header from _mtext4.
 * Returns a malloc'd null-terminated string, or NULL on failure.
 */
static char *read_mail_body(const char *mtext_path,
    ULONG seek_offset, USHORT length)
{
    BPTR fh;
    char *buf;
    long nread;

    if (length == 0)
        return strdup("");

    fh = Open((CONST_STRPTR)mtext_path, MODE_OLDFILE);
    if (!fh)
        return NULL;

    Seek(fh, (long)seek_offset, OFFSET_BEGINNING);

    buf = (char *)malloc((unsigned long)length + 1);
    if (!buf) {
        Close(fh);
        return NULL;
    }

    nread = Read(fh, buf, (long)length);
    Close(fh);

    if (nread < 0) {
        free(buf);
        return NULL;
    }

    buf[nread] = '\0';
    return buf;
}

/* ---- JSON emission helpers ---- */

/*
 * Emit a single mail header as a JSON object.
 */
static void emit_mail_header_json(struct json_state *js,
    const struct MailHeaderDisk *hdr, long index)
{
    /*
     * Ensure subject and from_name are safely null-terminated
     * when copied to output buffers.
     */
    char subj_buf[81];
    char from_buf[28];
    USHORT body_length;

    memcpy(subj_buf, hdr->subject, 80);
    subj_buf[80] = '\0';

    memcpy(from_buf, hdr->from_name, 27);
    from_buf[27] = '\0';

    body_length = read_be_ushort(hdr->length);

    json_obj_open(js);
    json_kv_int(js, "index", index);
    json_kv_str(js, "subject", subj_buf);
    json_kv_str(js, "from", from_buf);
    json_kv_int(js, "from_account",
        (long)read_be_ushort(hdr->from_account));
    json_kv_uint(js, "send_date",
        (unsigned long)read_be_ulong(hdr->send_date));
    json_kv_bool(js, "unread", is_unread(hdr));
    json_kv_int(js, "body_length", (long)body_length);

    {
        char folder_buf[33];
        memcpy(folder_buf, hdr->folder_name, 32);
        folder_buf[32] = '\0';
        if (folder_buf[0])
            json_kv_str(js, "original_folder", folder_buf);
        else
            json_kv_null(js, "original_folder");
    }

    json_obj_close(js);
}

/* ---- mail count ---- */

int cmd_mail_count(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    char uucp[12];
    char folder_path[256];
    char mhead_path[300];
    const char *folder = "INBOX";
    struct SignalSemaphore *sems;
    struct MailHeaderDisk *headers;
    long total = 0;
    long unread = 0;
    long i;

    if (argc < 2) {
        json_error("Usage: cnet-cli mail count <account|handle>"
            " [--folder <name>]");
        return 1;
    }

    /* Parse --folder flag */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--folder") == 0 && i + 1 < argc) {
            i++;
            folder = argv[i];
        }
    }

    if (resolve_user_mail(myp, argv[1], &account, uucp,
            (int)sizeof(uucp)) < 0) {
        json_error("User not found");
        return 1;
    }

    if (!CNetMailBase) {
        json_error("cnetmail.library not available"
            " (needed for mail semaphores)");
        return 1;
    }

    CreateFolderName(folder_path, uucp, (char *)folder);
    build_folder_file_path(mhead_path, (int)sizeof(mhead_path),
        folder_path, "_mhead4");

    sems = GetMailSems();
    ObtainSemaphoreShared(&sems[account - 1]);

    headers = read_mhead_records(mhead_path, &total);
    if (headers) {
        for (i = 0; i < total; i++) {
            if (is_unread(&headers[i]))
                unread++;
        }
        free(headers);
    }

    ReleaseSemaphore(&sems[account - 1]);

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "account", uucp);
    json_kv_int(&js, "account_number", (long)account);
    json_kv_str(&js, "folder", folder);
    json_kv_int(&js, "total", total);
    json_kv_int(&js, "unread", unread);
    json_kv_int(&js, "read", total - unread);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- mail list ---- */

int cmd_mail_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    char uucp[12];
    char folder_path[256];
    char mhead_path[300];
    const char *folder = "INBOX";
    int limit = 50;
    int offset = 0;
    struct SignalSemaphore *sems;
    struct MailHeaderDisk *all_headers;
    long total = 0;
    long unread = 0;
    int actual_limit;
    long i;

    if (argc < 2) {
        json_error("Usage: cnet-cli mail list <account|handle>"
            " [--folder <name>] [--limit N] [--offset N]");
        return 1;
    }

    /* Parse flags */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--folder") == 0 && i + 1 < argc) {
            i++;
            folder = argv[i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            i++;
            limit = atoi(argv[i]);
        } else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            i++;
            offset = atoi(argv[i]);
        }
    }

    if (resolve_user_mail(myp, argv[1], &account, uucp,
            (int)sizeof(uucp)) < 0) {
        json_error("User not found");
        return 1;
    }

    if (!CNetMailBase) {
        json_error("cnetmail.library not available"
            " (needed for mail semaphores)");
        return 1;
    }

    CreateFolderName(folder_path, uucp, (char *)folder);
    build_folder_file_path(mhead_path, (int)sizeof(mhead_path),
        folder_path, "_mhead4");

    sems = GetMailSems();
    ObtainSemaphoreShared(&sems[account - 1]);

    all_headers = read_mhead_records(mhead_path, &total);
    if (all_headers) {
        for (i = 0; i < total; i++) {
            if (is_unread(&all_headers[i]))
                unread++;
        }
    }

    /* Clamp offset/limit to valid range */
    if (offset >= (int)total)
        offset = (int)total;
    actual_limit = limit;
    if (offset + actual_limit > (int)total)
        actual_limit = (int)total - offset;

    /* Release semaphore before JSON emission */
    ReleaseSemaphore(&sems[account - 1]);

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "account", uucp);
    json_kv_int(&js, "account_number", (long)account);
    json_kv_str(&js, "folder", folder);
    json_kv_int(&js, "total", total);
    json_kv_int(&js, "unread", unread);
    json_kv_int(&js, "offset", (long)offset);
    json_kv_int(&js, "limit", (long)limit);

    json_key(&js, "messages");
    json_arr_open(&js);

    if (all_headers && actual_limit > 0) {
        for (i = 0; i < actual_limit; i++) {
            emit_mail_header_json(&js,
                &all_headers[offset + i],
                (long)(offset + i));
        }
    }

    json_arr_close(&js);
    json_obj_close(&js);
    json_finish(&js);

    if (all_headers)
        free(all_headers);

    return 0;
}

/* ---- mail read ---- */

int cmd_mail_read(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    char uucp[12];
    char folder_path[256];
    char mhead_path[300];
    char mtext_path[300];
    const char *folder = "INBOX";
    int mail_num;
    struct SignalSemaphore *sems;
    BPTR fh;
    long file_size, rec_count, nread;
    ULONG seek_offset;
    USHORT body_length;
    char *body;
    char subj_buf[81];
    char from_buf[28];

    /*
     * Static struct for single-record read. Non-reentrant (C1), safe
     * in single-threaded cnet-cli.
     */
    static struct MailHeaderDisk hdr;

    if (argc < 3) {
        json_error("Usage: cnet-cli mail read <account|handle>"
            " <num> [--folder <name>]");
        return 1;
    }

    mail_num = atoi(argv[2]);

    /* Parse --folder flag */
    {
        long i;
        for (i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--folder") == 0 &&
                    i + 1 < argc) {
                i++;
                folder = argv[i];
            }
        }
    }

    if (resolve_user_mail(myp, argv[1], &account, uucp,
            (int)sizeof(uucp)) < 0) {
        json_error("User not found");
        return 1;
    }

    if (!CNetMailBase) {
        json_error("cnetmail.library not available"
            " (needed for mail semaphores)");
        return 1;
    }

    CreateFolderName(folder_path, uucp, (char *)folder);
    build_folder_file_path(mhead_path, (int)sizeof(mhead_path),
        folder_path, "_mhead4");
    build_folder_file_path(mtext_path, (int)sizeof(mtext_path),
        folder_path, "_mtext4");

    sems = GetMailSems();
    ObtainSemaphoreShared(&sems[account - 1]);

    fh = Open((CONST_STRPTR)mhead_path, MODE_OLDFILE);
    if (!fh) {
        ReleaseSemaphore(&sems[account - 1]);
        json_error("Cannot open mail header file");
        return 1;
    }

    /* Verify mail_num is in range */
    Seek(fh, 0, OFFSET_END);
    file_size = Seek(fh, 0, OFFSET_BEGINNING);
    rec_count = file_size / MHEAD_RECORD_SIZE;

    if (mail_num < 0 || mail_num >= (int)rec_count) {
        char errbuf[80];
        Close(fh);
        ReleaseSemaphore(&sems[account - 1]);
        snprintf(errbuf, sizeof(errbuf),
            "Mail number out of range (0-%ld)",
            rec_count - 1);
        json_error(errbuf);
        return 1;
    }

    Seek(fh, (long)mail_num * MHEAD_RECORD_SIZE,
        OFFSET_BEGINNING);
    nread = Read(fh, (APTR)&hdr, MHEAD_RECORD_SIZE);
    Close(fh);

    if (nread != MHEAD_RECORD_SIZE) {
        ReleaseSemaphore(&sems[account - 1]);
        json_error("Failed to read mail header record");
        return 1;
    }

    /* Read body text */
    seek_offset = read_be_ulong(hdr.seek);
    body_length = read_be_ushort(hdr.length);
    body = read_mail_body(mtext_path, seek_offset, body_length);

    /* Release semaphore before JSON emission */
    ReleaseSemaphore(&sems[account - 1]);

    /* Safe null-terminated copies */
    memcpy(subj_buf, hdr.subject, 80);
    subj_buf[80] = '\0';
    memcpy(from_buf, hdr.from_name, 27);
    from_buf[27] = '\0';

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_int(&js, "index", (long)mail_num);
    json_kv_str(&js, "subject", subj_buf);
    json_kv_str(&js, "from", from_buf);
    json_kv_int(&js, "from_account",
        (long)read_be_ushort(hdr.from_account));
    json_kv_uint(&js, "send_date",
        (unsigned long)read_be_ulong(hdr.send_date));
    json_kv_bool(&js, "unread", is_unread(&hdr));
    json_kv_str(&js, "folder", folder);

    {
        char folder_buf[33];
        memcpy(folder_buf, hdr.folder_name, 32);
        folder_buf[32] = '\0';
        if (folder_buf[0])
            json_kv_str(&js, "original_folder", folder_buf);
        else
            json_kv_null(&js, "original_folder");
    }

    if (body) {
        json_kv_str(&js, "body", body);
        free(body);
    } else {
        json_kv_null(&js, "body");
    }

    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- mail folders ---- */

/*
 * Maximum number of mail folders to enumerate.
 * 64 is generous for typical BBS usage.
 */
#define MAX_FOLDERS 64

struct folder_info {
    char name[256];
    int total;
    int unread;
};

int cmd_mail_folders(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    char uucp[12];
    char folders_path[256];
    struct SignalSemaphore *sems;
    struct CNetFileEntry *dir_list;
    struct CNetFileEntry *entry;
    static struct folder_info folders[MAX_FOLDERS];
    int folder_count = 0;
    int i;

    /*
     * Static struct for scanning headers one at a time.
     * Non-reentrant (C1), safe in single-threaded cnet-cli.
     */
    static struct MailHeaderDisk scan_hdr;

    if (argc < 2) {
        json_error("Usage: cnet-cli mail folders <account|handle>");
        return 1;
    }

    if (resolve_user_mail(myp, argv[1], &account, uucp,
            (int)sizeof(uucp)) < 0) {
        json_error("User not found");
        return 1;
    }

    if (!CNetMailBase) {
        json_error("cnetmail.library not available"
            " (needed for mail semaphores)");
        return 1;
    }

    /* Build FOLDERS path manually since CreateFolderName
     * requires a specific folder name. */
    snprintf(folders_path, sizeof(folders_path),
        "mail:users/%s/FOLDERS", uucp);

    sems = GetMailSems();
    ObtainSemaphoreShared(&sems[account - 1]);

    dir_list = CNetReadDir(folders_path, 0);

    if (dir_list) {
        /*
         * Skip the directory header entry (first entry).
         * CNetReadDir returns the scanned directory itself as
         * the first entry. Actual contents start at nextfile.
         */
        entry = dir_list->nextfile;

        while (entry && folder_count < MAX_FOLDERS) {
            if (entry->ftype == CNFE_TYPE_DIR) {
                char folder_path[256];
                char mhead_path[300];
                long fsize;

                strncpy(folders[folder_count].name,
                    entry->filename, 255);
                folders[folder_count].name[255] = '\0';

                CreateFolderName(folder_path, uucp,
                    entry->filename);
                build_folder_file_path(mhead_path,
                    (int)sizeof(mhead_path),
                    folder_path, "_mhead4");

                fsize = FileSize(mhead_path);
                if (fsize > 0) {
                    BPTR fh;
                    int j;

                    folders[folder_count].total =
                        (int)(fsize / MHEAD_RECORD_SIZE);
                    folders[folder_count].unread = 0;

                    fh = Open((CONST_STRPTR)mhead_path,
                        MODE_OLDFILE);
                    if (fh) {
                        for (j = 0;
                                j < folders[folder_count].total;
                                j++) {
                            long nr = Read(fh, (APTR)&scan_hdr,
                                MHEAD_RECORD_SIZE);
                            if (nr != MHEAD_RECORD_SIZE)
                                break;
                            if (is_unread(&scan_hdr))
                                folders[folder_count].unread++;
                        }
                        Close(fh);
                    }
                } else {
                    folders[folder_count].total = 0;
                    folders[folder_count].unread = 0;
                }

                folder_count++;
            }

            entry = entry->nextfile;
        }

        CNetDisposeDir(&dir_list);
    }

    /* Release semaphore before JSON emission */
    ReleaseSemaphore(&sems[account - 1]);

    if (folder_count == 0) {
        json_error("No mail folders found for this account");
        return 1;
    }

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "account", uucp);
    json_kv_int(&js, "account_number", (long)account);

    json_key(&js, "folders");
    json_arr_open(&js);

    for (i = 0; i < folder_count; i++) {
        json_obj_open(&js);
        json_kv_str(&js, "name", folders[i].name);
        json_kv_int(&js, "total", (long)folders[i].total);
        json_kv_int(&js, "unread", (long)folders[i].unread);
        json_obj_close(&js);
    }

    json_arr_close(&js);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- mail send ---- */

int cmd_mail_send(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    const char *from = NULL;
    const char *to = NULL;
    const char *subject = NULL;
    const char *body = NULL;
    int sentmail = 0;
    short from_account, to_account;
    char from_uucp[12];
    char to_uucp[12];
    char from_handle[22];  /* matches KeyElement4.Handle[21] + null */
    long i;

    /* Parse required flags */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            i++;
            from = argv[i];
        } else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
            i++;
            to = argv[i];
        } else if (strcmp(argv[i], "--subject") == 0 &&
                   i + 1 < argc) {
            i++;
            subject = argv[i];
        } else if (strcmp(argv[i], "--body") == 0 && i + 1 < argc) {
            i++;
            body = argv[i];
        } else if (strcmp(argv[i], "--sentmail") == 0) {
            sentmail = 1;
        }
    }

    if (!from) {
        json_error("Missing required flag: --from");
        return 1;
    }
    if (!to) {
        json_error("Missing required flag: --to");
        return 1;
    }
    if (!subject) {
        json_error("Missing required flag: --subject");
        return 1;
    }
    if (!body) {
        json_error("Missing required flag: --body");
        return 1;
    }

    if (!CNetMailBase) {
        json_error("cnetmail.library not available"
            " (needed for mail semaphores)");
        return 1;
    }

    /* Validate subject length */
    if (strlen(subject) > 79) {
        json_error("Subject too long (max 79 characters)");
        return 1;
    }

    /* Validate body size (USHORT max = 65535) */
    if (strlen(body) > 65535) {
        json_error("Body too long (max 65535 bytes)");
        return 1;
    }

    /* Resolve sender */
    if (resolve_user_mail(myp, from, &from_account, from_uucp,
            (int)sizeof(from_uucp)) < 0) {
        json_error("Sender not found");
        return 1;
    }

    /* Look up sender's Handle under SEM[1] */
    ObtainSemaphoreShared(&myp->SEM[1]);
    if (from_account > 0 &&
            from_account <= (short)myp->Nums[0]) {
        safe_strcpy(from_handle,
            myp->Key[from_account - 1].Handle,
            (int)sizeof(from_handle));
    } else {
        from_handle[0] = '\0';
    }
    ReleaseSemaphore(&myp->SEM[1]);

    if (from_handle[0] == '\0') {
        json_error("Sender account out of range");
        return 1;
    }

    /* Resolve recipient */
    if (resolve_user_mail(myp, to, &to_account, to_uucp,
            (int)sizeof(to_uucp)) < 0) {
        json_error("Recipient not found");
        return 1;
    }

    /* Write mail to recipient's INBOX */
    if (write_mail_direct(myp, to_uucp, to_account,
            from_account, from_handle, subject, body,
            (USHORT)strlen(body), 0, "INBOX",
            "recipient") < 0) {
        return 1;  /* write_mail_direct already emitted error */
    }

    /* Optional SENTMAIL copy */
    if (sentmail) {
        if (write_mail_direct(myp, from_uucp, from_account,
                from_account, from_handle, subject, body,
                (USHORT)strlen(body), 0, "SENTMAIL",
                "SENTMAIL") < 0) {
            /* Non-fatal -- primary delivery succeeded */
            json_init(&js, stdout);
            json_obj_open(&js);
            json_kv_str(&js, "status", "sent");
            json_kv_str(&js, "warning",
                "SENTMAIL copy failed");
            json_kv_str(&js, "from", from_uucp);
            json_kv_str(&js, "to", to_uucp);
            json_kv_str(&js, "subject", subject);
            json_obj_close(&js);
            json_finish(&js);
            return 0;
        }
    }

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "sent");
    json_kv_str(&js, "from", from_uucp);
    json_kv_str(&js, "to", to_uucp);
    json_kv_str(&js, "subject", subject);
    if (sentmail)
        json_kv_bool(&js, "sentmail_copy", 1);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- mail reply ---- */

int cmd_mail_reply(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    char uucp[12];
    char folder_path[256];
    char mhead_path[300];
    const char *folder = "INBOX";
    const char *reply_body = NULL;
    const char *from_override = NULL;
    int sentmail = 0;
    int mail_num;
    struct SignalSemaphore *sems;
    BPTR fh;
    long file_size, rec_count, nread;
    short orig_from_account;
    short reply_to_account;
    char reply_to_uucp[12];
    char re_subject[84];
    short from_account;
    char from_uucp[12];
    char from_handle[22];  /* matches KeyElement4.Handle[21] + null */
    ULONG original_send_date;

    /*
     * Static struct for single-record read. Non-reentrant (C1),
     * safe in single-threaded cnet-cli.
     */
    static struct MailHeaderDisk hdr;

    if (argc < 3) {
        json_error("Usage: cnet-cli mail reply <account|handle>"
            " <num> --body <text> [--folder <name>]"
            " [--from <user>] [--sentmail]");
        return 1;
    }

    mail_num = atoi(argv[2]);

    /* Parse flags */
    {
        long i;
        for (i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--body") == 0 &&
                    i + 1 < argc) {
                i++;
                reply_body = argv[i];
            } else if (strcmp(argv[i], "--folder") == 0 &&
                       i + 1 < argc) {
                i++;
                folder = argv[i];
            } else if (strcmp(argv[i], "--from") == 0 &&
                       i + 1 < argc) {
                i++;
                from_override = argv[i];
            } else if (strcmp(argv[i], "--sentmail") == 0) {
                sentmail = 1;
            }
        }
    }

    if (!reply_body) {
        json_error("--body is required");
        return 1;
    }

    if (!CNetMailBase) {
        json_error("cnetmail.library not available"
            " (needed for mail semaphores)");
        return 1;
    }

    /* Validate body size (USHORT max = 65535) */
    if (strlen(reply_body) > 65535) {
        json_error("Body too long (max 65535 bytes)");
        return 1;
    }

    if (resolve_user_mail(myp, argv[1], &account, uucp,
            (int)sizeof(uucp)) < 0) {
        json_error("User not found");
        return 1;
    }

    CreateFolderName(folder_path, uucp, (char *)folder);
    build_folder_file_path(mhead_path, (int)sizeof(mhead_path),
        folder_path, "_mhead4");

    /* Read original message header under shared semaphore */
    sems = GetMailSems();
    ObtainSemaphoreShared(&sems[account - 1]);

    fh = Open((CONST_STRPTR)mhead_path, MODE_OLDFILE);
    if (!fh) {
        ReleaseSemaphore(&sems[account - 1]);
        json_error("Cannot open mail header file");
        return 1;
    }

    Seek(fh, 0, OFFSET_END);
    file_size = Seek(fh, 0, OFFSET_BEGINNING);
    rec_count = file_size / MHEAD_RECORD_SIZE;

    if (mail_num < 0 || mail_num >= (int)rec_count) {
        char errbuf[80];
        Close(fh);
        ReleaseSemaphore(&sems[account - 1]);
        snprintf(errbuf, sizeof(errbuf),
            "Mail number out of range (0-%ld)",
            rec_count - 1);
        json_error(errbuf);
        return 1;
    }

    Seek(fh, (long)mail_num * MHEAD_RECORD_SIZE,
        OFFSET_BEGINNING);
    nread = Read(fh, (APTR)&hdr, MHEAD_RECORD_SIZE);
    Close(fh);

    ReleaseSemaphore(&sems[account - 1]);

    if (nread != MHEAD_RECORD_SIZE) {
        json_error("Failed to read mail header record");
        return 1;
    }

    /* Extract original sender info */
    orig_from_account = (short)read_be_ushort(hdr.from_account);
    original_send_date = read_be_ulong(hdr.send_date);

    /* Look up original sender's UUCP for reply target */
    reply_to_uucp[0] = '\0';
    ObtainSemaphoreShared(&myp->SEM[1]);
    if (orig_from_account > 0 &&
            orig_from_account <= (short)myp->Nums[0]) {
        strncpy(reply_to_uucp,
            myp->Key[orig_from_account - 1].UUCP,
            sizeof(reply_to_uucp) - 1);
        reply_to_uucp[sizeof(reply_to_uucp) - 1] = '\0';
    }
    ReleaseSemaphore(&myp->SEM[1]);

    if (reply_to_uucp[0] == '\0') {
        char errbuf[120];
        char from_buf[28];
        memcpy(from_buf, hdr.from_name, 27);
        from_buf[27] = '\0';
        snprintf(errbuf, sizeof(errbuf),
            "Cannot reply: original sender (account %d, "
            "\"%s\") no longer exists",
            (int)orig_from_account, from_buf);
        json_error(errbuf);
        return 1;
    }

    /* Verify via CNetAddressToAccount */
    reply_to_account = CNetAddressToAccount(reply_to_uucp);
    if (reply_to_account < 1) {
        char errbuf[120];
        char from_buf[28];
        memcpy(from_buf, hdr.from_name, 27);
        from_buf[27] = '\0';
        snprintf(errbuf, sizeof(errbuf),
            "Cannot reply: original sender \"%s\" "
            "(account %d) no longer has a valid account",
            from_buf, (int)orig_from_account);
        json_error(errbuf);
        return 1;
    }

    /* Build reply subject */
    {
        char subj_safe[81];
        memcpy(subj_safe, hdr.subject, 80);
        subj_safe[80] = '\0';

        if (strncasecmp(subj_safe, "RE: ", 4) != 0) {
            snprintf(re_subject, sizeof(re_subject),
                "RE: %s", subj_safe);
        } else {
            strncpy(re_subject, subj_safe,
                sizeof(re_subject) - 1);
            re_subject[sizeof(re_subject) - 1] = '\0';
        }
    }

    /* Truncate to max 79 chars */
    if (strlen(re_subject) > 79)
        re_subject[79] = '\0';

    /* Determine "from" for the reply */
    if (from_override) {
        if (resolve_user_mail(myp, from_override, &from_account,
                from_uucp, (int)sizeof(from_uucp)) < 0) {
            json_error("--from user not found");
            return 1;
        }
    } else {
        from_account = account;
        safe_strcpy(from_uucp, uucp, (int)sizeof(from_uucp));
    }

    /* Look up from_handle under SEM[1] */
    ObtainSemaphoreShared(&myp->SEM[1]);
    if (from_account > 0 &&
            from_account <= (short)myp->Nums[0]) {
        safe_strcpy(from_handle,
            myp->Key[from_account - 1].Handle,
            (int)sizeof(from_handle));
    } else {
        from_handle[0] = '\0';
    }
    ReleaseSemaphore(&myp->SEM[1]);

    if (from_handle[0] == '\0') {
        json_error("Sender account out of range");
        return 1;
    }

    /* Write reply to original sender's INBOX */
    if (write_mail_direct(myp, reply_to_uucp, reply_to_account,
            from_account, from_handle, re_subject, reply_body,
            (USHORT)strlen(reply_body), original_send_date,
            "INBOX", "recipient") < 0) {
        return 1;  /* write_mail_direct already emitted error */
    }

    /* Optional SENTMAIL copy */
    if (sentmail) {
        if (write_mail_direct(myp, from_uucp, from_account,
                from_account, from_handle, re_subject,
                reply_body, (USHORT)strlen(reply_body),
                original_send_date, "SENTMAIL",
                "SENTMAIL") < 0) {
            /* Non-fatal */
            json_init(&js, stdout);
            json_obj_open(&js);
            json_kv_str(&js, "status", "sent");
            json_kv_str(&js, "warning",
                "SENTMAIL copy failed");
            json_kv_int(&js, "in_reply_to",
                (long)mail_num);
            json_kv_str(&js, "from", from_uucp);
            json_kv_str(&js, "to", reply_to_uucp);
            json_kv_str(&js, "subject", re_subject);
            json_obj_close(&js);
            json_finish(&js);
            return 0;
        }
    }

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "sent");
    json_kv_int(&js, "in_reply_to", (long)mail_num);
    json_kv_str(&js, "from", from_uucp);
    json_kv_str(&js, "to", reply_to_uucp);
    json_kv_str(&js, "subject", re_subject);
    if (sentmail)
        json_kv_bool(&js, "sentmail_copy", 1);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- mail delete ---- */

int cmd_mail_delete(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    short account;
    char uucp[12];
    char src_folder_path[256];
    char src_mhead[300];
    char src_mtext[300];
    const char *folder = "INBOX";
    int mail_num;
    int is_trashcan;
    struct SignalSemaphore *sems;
    struct MailHeaderDisk *all_headers = NULL;
    long rec_count = 0;
    struct MailHeaderDisk hdr;
    ULONG body_seek;
    USHORT body_length;
    char *body = NULL;
    long i;

    if (argc < 3) {
        json_error("Usage: cnet-cli mail delete <account|handle>"
            " <num> [--folder <name>]");
        return 1;
    }

    mail_num = atoi(argv[2]);

    /* Parse --folder flag */
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--folder") == 0 && i + 1 < argc) {
            i++;
            folder = argv[i];
        }
    }

    if (resolve_user_mail(myp, argv[1], &account, uucp,
            (int)sizeof(uucp)) < 0) {
        json_error("User not found");
        return 1;
    }

    if (!CNetMailBase) {
        json_error("cnetmail.library not available"
            " (needed for mail semaphores)");
        return 1;
    }

    is_trashcan = (strcasecmp(folder, "TRASHCAN") == 0);

    /* Build source paths */
    CreateFolderName(src_folder_path, uucp, (char *)folder);
    build_folder_file_path(src_mhead, (int)sizeof(src_mhead),
        src_folder_path, "_mhead4");
    build_folder_file_path(src_mtext, (int)sizeof(src_mtext),
        src_folder_path, "_mtext4");

    /* Acquire EXCLUSIVE mail semaphore (write operation) */
    sems = GetMailSems();
    ObtainSemaphore(&sems[account - 1]);

    /* ---- STEP 1: Read ALL source header records ---- */
    all_headers = read_mhead_records(src_mhead, &rec_count);
    if (!all_headers || rec_count == 0) {
        json_error("Cannot open mail header file or file is empty");
        goto cleanup_sem;
    }

    if (mail_num < 0 || mail_num >= (int)rec_count) {
        char errbuf[80];
        snprintf(errbuf, sizeof(errbuf),
            "Mail number out of range (0-%ld)",
            rec_count - 1);
        json_error(errbuf);
        goto cleanup_body;
    }

    /* Copy the doomed header record */
    memcpy(&hdr, &all_headers[mail_num], MHEAD_RECORD_SIZE);

    /* Read body text (needed for TRASHCAN move) */
    body_seek = read_be_ulong(hdr.seek);
    body_length = read_be_ushort(hdr.length);
    if (body_length > 0)
        body = read_mail_body(src_mtext, body_seek, body_length);

    /* M1 fix: abort if body text cannot be read for TRASHCAN move */
    if (body_length > 0 && !body && !is_trashcan) {
        json_error("Cannot read message body for TRASHCAN move");
        goto cleanup_body;
    }

    /* ---- STEP 2: Write to TRASHCAN ---- */
    if (!is_trashcan) {
        char trash_folder_path[256];
        char trash_mtext[300];
        char trash_mhead[300];
        ULONG new_seek = 0;
        int text_ok = 1;
        long trash_tsize, trash_hsize;
        BPTR fh_ttext, fh_thead;
        long nwritten;

        /* Ensure TRASHCAN directory exists */
        CreateMailDir(uucp);
        CreateFolderName(trash_folder_path, uucp,
            (char *)"TRASHCAN");
        BuildDir(trash_folder_path);

        build_folder_file_path(trash_mtext,
            (int)sizeof(trash_mtext),
            trash_folder_path, "_mtext4");
        build_folder_file_path(trash_mhead,
            (int)sizeof(trash_mhead),
            trash_folder_path, "_mhead4");

        /* ---- Append body text to TRASHCAN _mtext4 ---- */
        if (body && body_length > 0) {
            BPTR lock_test;
            int file_exists;

            text_ok = 0;

            /*
             * Use Lock to check file existence. FileSize from
             * cnet.library returns 0 (not -1) for non-existent
             * files, making it unreliable for existence checks.
             */
            lock_test = Lock((CONST_STRPTR)trash_mtext,
                SHARED_LOCK);
            file_exists = (lock_test != 0);
            if (lock_test)
                UnLock(lock_test);

            if (!file_exists) {
                /* File doesn't exist -- create it */
                fh_ttext = Open((CONST_STRPTR)trash_mtext,
                    MODE_NEWFILE);
                trash_tsize = 0;
            } else {
                /* File exists -- open for append */
                fh_ttext = Open((CONST_STRPTR)trash_mtext,
                    MODE_OLDFILE);
                trash_tsize = FileSize(trash_mtext);
                if (trash_tsize < 0)
                    trash_tsize = 0;
            }

            if (!fh_ttext) {
                json_error("Failed to open TRASHCAN text file");
                goto cleanup_body;
            }

            if (file_exists)
                Seek(fh_ttext, 0, OFFSET_END);

            new_seek = (ULONG)trash_tsize;

            nwritten = Write(fh_ttext, (APTR)body,
                (long)body_length);
            Close(fh_ttext);

            if (nwritten == (long)body_length)
                text_ok = 1;
        }

        if (!text_ok) {
            json_error("Failed to write message body to TRASHCAN");
            goto cleanup_body;
        }

        /* Update header's Seek to point to TRASHCAN location */
        write_be_ulong(hdr.seek, new_seek);

        /* Check header file existence using Lock (same FileSize bug) */
        {
            BPTR hlock = Lock((CONST_STRPTR)trash_mhead,
                SHARED_LOCK);
            int hdr_exists = (hlock != 0);
            if (hlock)
                UnLock(hlock);

            /* Update unknown_0 to reflect new position in TRASHCAN */
            if (!hdr_exists) {
                write_be_ulong(hdr.unknown_0, 0);
                trash_hsize = 0;
            } else {
                trash_hsize = FileSize(trash_mhead);
                if (trash_hsize < 0)
                    trash_hsize = 0;
                write_be_ulong(hdr.unknown_0, (ULONG)trash_hsize);
            }

            /* ---- Append header to TRASHCAN _mhead4 ---- */
            if (!hdr_exists) {
                fh_thead = Open((CONST_STRPTR)trash_mhead,
                    MODE_NEWFILE);
            } else {
                fh_thead = Open((CONST_STRPTR)trash_mhead,
                    MODE_OLDFILE);
            }

            if (!fh_thead) {
                json_error("Failed to open TRASHCAN header file");
                goto cleanup_body;
            }

            if (hdr_exists)
                Seek(fh_thead, 0, OFFSET_END);
        }

        nwritten = Write(fh_thead, (APTR)&hdr,
            MHEAD_RECORD_SIZE);
        Close(fh_thead);

        if (nwritten != MHEAD_RECORD_SIZE) {
            json_error("Failed to write header to TRASHCAN");
            goto cleanup_body;
        }
    }

    /* ---- STEP 3: Remove from source ---- */
    if (rec_count == 1) {
        /* Only record -- write empty file */
        BPTR fh_src = Open((CONST_STRPTR)src_mhead,
            MODE_NEWFILE);
        if (fh_src)
            Close(fh_src);

        if (is_trashcan) {
            /* Truncate _mtext4 as well -- no records remain */
            BPTR fh_txt = Open((CONST_STRPTR)src_mtext,
                MODE_NEWFILE);
            if (fh_txt)
                Close(fh_txt);
        }
    } else {
        /* Remove record by shifting remaining down */
        long new_size;
        BPTR fh_src;
        long nwritten;

        if (mail_num < (int)rec_count - 1) {
            memmove(&all_headers[mail_num],
                &all_headers[mail_num + 1],
                (unsigned long)(rec_count - mail_num - 1) *
                    MHEAD_RECORD_SIZE);
        }

        new_size = (rec_count - 1) * MHEAD_RECORD_SIZE;
        fh_src = Open((CONST_STRPTR)src_mhead, MODE_NEWFILE);
        if (fh_src) {
            nwritten = Write(fh_src, (APTR)all_headers,
                new_size);
            Close(fh_src);

            if (nwritten != new_size) {
                /*
                 * Partial write -- file may be corrupted.
                 * TRASHCAN has the backup. Emit warning but
                 * continue to success since data is preserved.
                 */
                warn_add("Source header file may be corrupted"
                    " after partial write");
            }
        }
    }

    free(all_headers);
    all_headers = NULL;
    if (body) {
        free(body);
        body = NULL;
    }

    /* Release semaphore before JSON emission */
    ReleaseSemaphore(&sems[account - 1]);

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status",
        is_trashcan ? "permanently_deleted" : "moved_to_trashcan");
    json_kv_int(&js, "deleted_index", (long)mail_num);
    json_kv_str(&js, "folder", folder);
    json_kv_str(&js, "account", uucp);
    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    return 0;

cleanup_body:
    if (all_headers)
        free(all_headers);
    if (body)
        free(body);

cleanup_sem:
    ReleaseSemaphore(&sems[account - 1]);
    return 1;
}

/* ---- mail feedback ---- */

/*
 * Read sysop feedback mail (account #1).
 *
 * Dispatches to cmd_mail_list (no number given) or cmd_mail_read
 * (number given) with account "1" injected into the argv.
 */
int cmd_mail_feedback(struct MainPort *myp, int argc, char **argv)
{
    char *synth[12];
    int synth_argc = 0;
    int i;

    /*
     * argv[0] = "feedback"
     * argv[1] = optional mail number or flag
     * argv[2..] = flags (--folder, --limit, --offset)
     *
     * Detect list vs read: if argv[1] exists and is all digits,
     * it is a mail number -> delegate to cmd_mail_read.
     * Otherwise -> delegate to cmd_mail_list.
     */

    if (argc >= 2 && all_digits(argv[1])) {
        /* Read mode: { "feedback", "1", num, ...flags } */
        synth[synth_argc++] = argv[0];  /* "feedback" */
        synth[synth_argc++] = "1";       /* account */
        synth[synth_argc++] = argv[1];   /* mail number */

        for (i = 2; i < argc && synth_argc < 12; i++)
            synth[synth_argc++] = argv[i];

        return cmd_mail_read(myp, synth_argc, synth);
    }

    /* List mode: { "feedback", "1", ...flags } */
    synth[synth_argc++] = argv[0];  /* "feedback" */
    synth[synth_argc++] = "1";       /* account */

    for (i = 1; i < argc && synth_argc < 12; i++)
        synth[synth_argc++] = argv[i];

    return cmd_mail_list(myp, synth_argc, synth);
}

/* ---- mail verify ---- */

/*
 * View sent mail for an account.
 *
 * Delegates to cmd_mail_list with --folder SentMail injected
 * before any user-provided flags (so user can override with
 * their own --folder if desired, since last --folder wins).
 */
int cmd_mail_verify(struct MainPort *myp, int argc, char **argv)
{
    char *synth[12];
    int synth_argc = 0;
    int i;

    if (argc < 2) {
        json_error("Usage: cnet-cli mail verify <account|handle>"
            " [--limit N] [--offset N]");
        return 1;
    }

    /*
     * Build: { "verify", account, "--folder", "SentMail", ...user_flags }
     *
     * argv[0] = "verify"
     * argv[1] = account identifier
     * argv[2..] = user flags
     */
    synth[synth_argc++] = argv[0];      /* "verify" */
    synth[synth_argc++] = argv[1];      /* account */
    synth[synth_argc++] = "--folder";
    synth[synth_argc++] = "SentMail";

    for (i = 2; i < argc && synth_argc < 12; i++)
        synth[synth_argc++] = argv[i];

    return cmd_mail_list(myp, synth_argc, synth);
}
