/*
 * group.c -- Access group commands for cnet-cli
 *
 * Access group operations: list, show, edit, transpose
 *
 * AGC[] is static configuration loaded at boot -- no semaphores needed
 * for read or single-writer mutation.
 *
 * Edit writes to disk via temp-file + DeleteFile + Rename pattern.
 * Transpose copies DefPrivs to user accounts via LockAccount/UnLockAccount.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>

#include "group.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;

/* ---- constants ---- */

#define BBS_ADATA_PATH     "SysData:bbs.adata"
#define BBS_ADATA_TMP_PATH "SysData:bbs.adata.tmp"
#define ACCESS_GROUP_COUNT 32

/* ---- internal helpers ---- */

/*
 * Parse a hexadecimal value string ("0xNNNNNNNN" or "NNNNNNNN").
 * Returns 1 on success (value stored in *out), 0 on failure.
 */
static int parse_hex_ulong(const char *s, unsigned long *out)
{
    char *endp;
    unsigned long val;

    if (!s || !*s)
        return 0;

    val = strtoul(s, &endp, 16);
    if (*endp != '\0')
        return 0;

    *out = val;
    return 1;
}

/*
 * Write the full 32-group AGC[] array to SysData:bbs.adata.
 *
 * Uses temp-file + DeleteFile + Rename for safe persistence.
 * Returns 0 on success, -1 on recoverable failure (original intact),
 * -2 on critical failure (original deleted, temp file is only copy).
 */
static int write_access_data_disk(struct AccessGroup *agc)
{
    BPTR fh;
    long expected = (long)(sizeof(struct AccessGroup) * ACCESS_GROUP_COUNT);
    long written;

    /* Write to temp file first. If the write fails or is interrupted,
     * the original bbs.adata remains intact on disk. */
    fh = Open((CONST_STRPTR)BBS_ADATA_TMP_PATH, MODE_NEWFILE);
    if (!fh)
        return -1;

    written = Write(fh, (APTR)agc, expected);
    Close(fh);

    if (written != expected) {
        DeleteFile((CONST_STRPTR)BBS_ADATA_TMP_PATH);
        return -1;
    }

    /* AmigaOS Rename() does NOT replace an existing target -- it fails
     * with ERROR_OBJECT_EXISTS.  Must DeleteFile() the original first,
     * then Rename() the temp file into place. */
    if (!DeleteFile((CONST_STRPTR)BBS_ADATA_PATH)) {
        /* DeleteFile failed -- original is still intact, temp is extra.
         * Clean up the temp file and report failure. */
        DeleteFile((CONST_STRPTR)BBS_ADATA_TMP_PATH);
        return -1;
    }

    if (!Rename((CONST_STRPTR)BBS_ADATA_TMP_PATH,
                (CONST_STRPTR)BBS_ADATA_PATH)) {
        /* Critical: DeleteFile succeeded but Rename failed. The original
         * bbs.adata is gone and the only copy of the data is in the temp
         * file. Do NOT delete it -- the sysop must manually rename
         * SysData:bbs.adata.tmp to SysData:bbs.adata to recover. */
        return -2;
    }

    return 0;
}

/*
 * Decode ABits and ABits2 into named boolean flags.
 * Emits a "decoded_flags" JSON object within the current object.
 */
static void emit_decoded_flags(struct json_state *js, struct Privs *p)
{
    ULONG a = p->ABits;
    ULONG a2 = p->ABits2;

    json_key(js, "decoded_flags");
    json_obj_open(js);

    /* ABits flags */
    json_kv_bool(js, "email",        (a & EMAIL_FLAG) ? 1 : 0);
    json_kv_bool(js, "pfile",        (a & PFILE_FLAG) ? 1 : 0);
    json_kv_bool(js, "gfile",        (a & GFILE_FLAG) ? 1 : 0);
    json_kv_bool(js, "ulist",        (a & ULIST_FLAG) ? 1 : 0);
    json_kv_bool(js, "sysop",        (a & SYSOP_FLAG) ? 1 : 0);
    json_kv_bool(js, "rewards",      (a & REWARDS_FLAG) ? 1 : 0);
    json_kv_bool(js, "autovalid",    (a & AUTOVALID_FLAG) ? 1 : 0);
    json_kv_bool(js, "suspended",    (a & SUSPENDACCT_FLAG) ? 1 : 0);
    json_kv_bool(js, "conference",   (a & CONF_FLAG) ? 1 : 0);
    json_kv_bool(js, "mci1",         (a & MCI1_FLAG) ? 1 : 0);
    json_kv_bool(js, "mci2",         (a & MCI2_FLAG) ? 1 : 0);
    json_kv_bool(js, "relogon",      (a & RELOGON_FLAG) ? 1 : 0);
    json_kv_bool(js, "receive_mail", (a & RECEIVEMAIL_FLAG) ? 1 : 0);
    json_kv_bool(js, "bulk_mail",    (a & BULKMAIL_FLAG) ? 1 : 0);
    json_kv_bool(js, "urgent_mail",  (a & URGENTMAIL_FLAG) ? 1 : 0);
    json_kv_bool(js, "read_any",     (a & READANY_FLAG) ? 1 : 0);
    json_kv_bool(js, "delete_any",   (a & DELETEANY_FLAG) ? 1 : 0);
    json_kv_bool(js, "file_add",     (a & FILEADD_FLAG) ? 1 : 0);
    json_kv_bool(js, "see_anon",     (a & SEEANON_FLAG) ? 1 : 0);
    json_kv_bool(js, "nolocks",      (a & NOLOCKS_FLAG) ? 1 : 0);
    json_kv_bool(js, "vote_topic",   (a & VOTETOPIC_FLAG) ? 1 : 0);
    json_kv_bool(js, "vote_choice",  (a & VOTECHOICE_FLAG) ? 1 : 0);

    /* ABits2 flags */
    json_kv_bool(js, "superuser",    (a2 & SUPERUSER_FLAG) ? 1 : 0);
    json_kv_bool(js, "port_monitor", (a2 & PORTMONITOR_FLAG) ? 1 : 0);
    json_kv_bool(js, "broadcast",    (a2 & BROADCAST_FLAG) ? 1 : 0);
    json_kv_bool(js, "edit_handle",  (a2 & EDHANDLE_FLAG) ? 1 : 0);
    json_kv_bool(js, "edit_realname",(a2 & EDREALNAME_FLAG) ? 1 : 0);
    json_kv_bool(js, "net_mail",     (a2 & NETMAIL_FLAG) ? 1 : 0);
    json_kv_bool(js, "open_screen",  (a2 & OPENSCREEN_FLAG) ? 1 : 0);
    json_kv_bool(js, "open_capture", (a2 & OPENCAPTURE_FLAG) ? 1 : 0);

    json_obj_close(js);
}

/* ---- group list ---- */

int cmd_group_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int i;
    char buf[128];
    char hbuf[16];

    (void)argc;
    (void)argv;

    json_init(&js, stdout);
    json_obj_open(&js);

    json_key(&js, "groups");
    json_arr_open(&js);

    for (i = 0; i < 32; i++) {
        struct AccessGroup *ag = &myp->AGC[i];
        struct Privs *p = &ag->DefPrivs;

        json_obj_open(&js);
        json_kv_int(&js, "id", (long)i);
        json_kv_str(&js, "name",
            strip_mci(buf, sizeof(buf), ag->Name));
        json_kv_bool(&js, "defined",
            ag->Name[0] != '\0' ? 1 : 0);
        json_kv_int(&js, "expire_days", (long)ag->ExpireDays);
        json_kv_int(&js, "expire_access",
            (long)ag->ExpireAccess);
        json_kv_int(&js, "daily_minutes",
            (long)p->DailyMinutes);
        json_kv_int(&js, "calls_per_day", (long)p->Calls);
        json_kv_int(&js, "idle_limit", (long)p->Idle);
        json_kv_int(&js, "editor_lines",
            (long)p->EditorLines);

        snprintf(hbuf, sizeof(hbuf), "0x%08lx",
            (unsigned long)p->ABits);
        json_kv_str(&js, "abits", hbuf);
        snprintf(hbuf, sizeof(hbuf), "0x%08lx",
            (unsigned long)p->ABits2);
        json_kv_str(&js, "abits2", hbuf);

        json_obj_close(&js);
    }

    json_arr_close(&js);
    json_kv_int(&js, "total", 32L);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- group detail emission (forward declaration) ---- */

static void emit_group_detail(struct json_state *js,
    struct AccessGroup *ag, int group_num);

/* ---- group show ---- */

int cmd_group_show(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int group;

    if (argc < 2) {
        json_error("Usage: cnet-cli group show <group-number>");
        return 1;
    }

    if (!all_digits(argv[1])) {
        json_error("Group number must be 0-31");
        return 1;
    }

    group = atoi(argv[1]);
    if (group < 0 || group > 31) {
        json_error("Group number must be 0-31");
        return 1;
    }

    json_init(&js, stdout);
    json_obj_open(&js);

    emit_group_detail(&js, &myp->AGC[group], group);

    json_obj_close(&js);  /* top-level */
    json_finish(&js);

    return 0;
}

/* ---- group edit ---- */

/*
 * Emit group detail fields into an already-open JSON object.
 * Shared by cmd_group_show and cmd_group_edit success output.
 */
static void emit_group_detail(struct json_state *js,
    struct AccessGroup *ag, int group_num)
{
    struct Privs *p = &ag->DefPrivs;
    char buf[128];
    char hbuf[16];

    json_kv_int(js, "id", (long)group_num);
    json_kv_str(js, "name",
        strip_mci(buf, sizeof(buf), ag->Name));
    json_kv_bool(js, "defined",
        ag->Name[0] != '\0' ? 1 : 0);
    json_kv_int(js, "expire_days", (long)ag->ExpireDays);
    json_kv_int(js, "expire_access", (long)ag->ExpireAccess);

    /* Full privileges object */
    json_key(js, "privileges");
    json_obj_open(js);

    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->MBaseFlags);
    json_kv_str(js, "mbase_flags", hbuf);
    {
        char fbuf[128];
        json_kv_str(js, "mbase_flags_groups",
            expand_flags_string(fbuf, sizeof(fbuf), p->MBaseFlags));
    }
    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->FBaseFlags);
    json_kv_str(js, "fbase_flags", hbuf);
    {
        char fbuf[128];
        json_kv_str(js, "fbase_flags_groups",
            expand_flags_string(fbuf, sizeof(fbuf), p->FBaseFlags));
    }
    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->LBaseFlags);
    json_kv_str(js, "lbase_flags", hbuf);
    {
        char fbuf[128];
        json_kv_str(js, "lbase_flags_groups",
            expand_flags_string(fbuf, sizeof(fbuf), p->LBaseFlags));
    }
    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->ABits);
    json_kv_str(js, "abits", hbuf);
    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->ABits2);
    json_kv_str(js, "abits2", hbuf);

    json_kv_int(js, "daily_down_bytes", p->DailyDownBytes);
    json_kv_int(js, "daily_up_bytes", p->DailyUpBytes);
    json_kv_int(js, "calls_per_day", (long)p->Calls);
    json_kv_int(js, "call_minutes", (long)p->CallMinutes);
    json_kv_int(js, "daily_minutes", (long)p->DailyMinutes);
    json_kv_int(js, "daily_downloads",
        (long)p->DailyDownloads);
    json_kv_int(js, "daily_uploads", (long)p->DailyUploads);
    json_kv_int(js, "messages", (long)p->Messages);
    json_kv_int(js, "feedbacks", (long)p->Feedbacks);
    json_kv_int(js, "editor_lines", (long)p->EditorLines);
    json_kv_int(js, "idle_limit", (long)p->Idle);
    json_kv_int(js, "max_mail_kbytes",
        (long)p->MaxMailKBytes);
    json_kv_int(js, "purge_days", (long)p->PurgeDays);
    json_kv_int(js, "file_ratio", (long)p->FileRatio);
    json_kv_int(js, "byte_ratio", (long)p->ByteRatio);
    json_kv_int(js, "sig_lines", (long)p->SigLines);
    json_kv_int(js, "daily_pfile_minutes",
        (long)p->DailyPfileMinutes);
    json_kv_int(js, "allow_aliases", (long)p->AllowAliases);
    json_kv_int(js, "delete_own", (long)p->DeleteOwn);
    json_kv_int(js, "anonymous", (long)p->Anonymous);
    json_kv_int(js, "private_area", (long)p->PrivateArea);
    json_kv_int(js, "callback", (long)p->CallBack);
    json_kv_int(js, "term_link", (long)p->TermLink);
    json_kv_int(js, "caller_id", (long)p->CallerID);
    json_kv_int(js, "page_sysop", (long)p->PageSysop);
    json_kv_int(js, "alias", (long)p->Alias);
    json_kv_int(js, "dictionary", (long)p->Dictionary);

    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->LogFlags);
    json_kv_str(js, "log_flags", hbuf);
    json_kv_int(js, "log_to_mail", (long)p->LogToMail);

    json_obj_close(js);  /* privileges */

    /* Decoded flag names */
    emit_decoded_flags(js, p);
}

int cmd_group_edit(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct AccessGroup *ag;
    struct Privs *p;
    int group;
    int i;
    int disk_rc;

    /* Sentinel values: LONG_MIN = "not specified" for short fields,
     * -1 for UBYTE fields. Hex fields use have_xxx booleans. */
    const char *new_name = NULL;
    long new_expire_days = LONG_MIN;
    int new_expire_access = -1;

    /* Privs short fields */
    long new_daily_minutes = LONG_MIN;
    long new_call_minutes = LONG_MIN;
    long new_calls = LONG_MIN;
    long new_idle = LONG_MIN;
    long new_editor_lines = LONG_MIN;
    long new_messages = LONG_MIN;
    long new_feedbacks = LONG_MIN;
    long new_daily_downloads = LONG_MIN;
    long new_daily_uploads = LONG_MIN;
    long new_max_mail_kbytes = LONG_MIN;
    long new_purge_days = LONG_MIN;
    long new_sig_lines = LONG_MIN;
    long new_daily_pfile_minutes = LONG_MIN;
    long new_log_to_mail = LONG_MIN;
    long new_alias = LONG_MIN;
    long new_dictionary = LONG_MIN;

    /* Privs long fields -- use boolean flags instead of LONG_MIN sentinel
     * because atol() can legitimately return LONG_MIN for these fields. */
    long new_daily_down_bytes = 0;
    int have_daily_down_bytes = 0;
    long new_daily_up_bytes = 0;
    int have_daily_up_bytes = 0;

    /* Privs UBYTE fields */
    int new_file_ratio = -1;
    int new_byte_ratio = -1;
    int new_allow_aliases = -1;
    int new_delete_own = -1;
    int new_anonymous = -1;
    int new_private_area = -1;
    int new_callback = -1;
    int new_term_link = -1;
    int new_caller_id = -1;
    int new_page_sysop = -1;

    /* Hex bitmask fields */
    int have_mbase_flags = 0;
    unsigned long val_mbase_flags = 0;
    int have_fbase_flags = 0;
    unsigned long val_fbase_flags = 0;
    int have_lbase_flags = 0;
    unsigned long val_lbase_flags = 0;
    int have_abits = 0;
    unsigned long val_abits = 0;
    int have_abits2 = 0;
    unsigned long val_abits2 = 0;
    int have_log_flags = 0;
    unsigned long val_log_flags = 0;

    /* Track changed field names for output */
    const char *changed_fields[40];
    int changed_count = 0;

    if (argc < 2) {
        json_error("Usage: cnet-cli group edit <group-number> "
            "[--name <value>] [--expire-days <N>] ...");
        return 1;
    }

    if (!all_digits(argv[1])) {
        json_error("Group number must be 0-31");
        return 1;
    }

    group = atoi(argv[1]);
    if (group < 0 || group > 31) {
        json_error("Group number must be 0-31");
        return 1;
    }

    /* Parse --flag value pairs */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            new_name = argv[++i];
        } else if (strcmp(argv[i], "--expire-days") == 0 &&
                   i + 1 < argc) {
            i++;
            new_expire_days = atol(argv[i]);
            if (new_expire_days < 0 || new_expire_days > 32767) {
                json_error("--expire-days must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--expire-access") == 0 &&
                   i + 1 < argc) {
            i++;
            new_expire_access = atoi(argv[i]);
            if (new_expire_access < 0 || new_expire_access > 31) {
                json_error("--expire-access must be 0-31");
                return 1;
            }
        } else if (strcmp(argv[i], "--daily-minutes") == 0 &&
                   i + 1 < argc) {
            i++;
            new_daily_minutes = atol(argv[i]);
            if (new_daily_minutes < 0 || new_daily_minutes > 32767) {
                json_error("--daily-minutes must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--call-minutes") == 0 &&
                   i + 1 < argc) {
            i++;
            new_call_minutes = atol(argv[i]);
            if (new_call_minutes < 0 || new_call_minutes > 32767) {
                json_error("--call-minutes must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--calls") == 0 &&
                   i + 1 < argc) {
            i++;
            new_calls = atol(argv[i]);
            if (new_calls < 0 || new_calls > 32767) {
                json_error("--calls must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--idle") == 0 &&
                   i + 1 < argc) {
            i++;
            new_idle = atol(argv[i]);
            if (new_idle < 0 || new_idle > 32767) {
                json_error("--idle must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--editor-lines") == 0 &&
                   i + 1 < argc) {
            i++;
            new_editor_lines = atol(argv[i]);
            if (new_editor_lines < 0 || new_editor_lines > 32767) {
                json_error("--editor-lines must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--messages") == 0 &&
                   i + 1 < argc) {
            i++;
            new_messages = atol(argv[i]);
            if (new_messages < 0 || new_messages > 32767) {
                json_error("--messages must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--feedbacks") == 0 &&
                   i + 1 < argc) {
            i++;
            new_feedbacks = atol(argv[i]);
            if (new_feedbacks < 0 || new_feedbacks > 32767) {
                json_error("--feedbacks must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--daily-downloads") == 0 &&
                   i + 1 < argc) {
            i++;
            new_daily_downloads = atol(argv[i]);
            if (new_daily_downloads < 0 ||
                    new_daily_downloads > 32767) {
                json_error("--daily-downloads must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--daily-uploads") == 0 &&
                   i + 1 < argc) {
            i++;
            new_daily_uploads = atol(argv[i]);
            if (new_daily_uploads < 0 ||
                    new_daily_uploads > 32767) {
                json_error("--daily-uploads must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--daily-down-bytes") == 0 &&
                   i + 1 < argc) {
            i++;
            new_daily_down_bytes = atol(argv[i]);
            have_daily_down_bytes = 1;
        } else if (strcmp(argv[i], "--daily-up-bytes") == 0 &&
                   i + 1 < argc) {
            i++;
            new_daily_up_bytes = atol(argv[i]);
            have_daily_up_bytes = 1;
        } else if (strcmp(argv[i], "--max-mail-kbytes") == 0 &&
                   i + 1 < argc) {
            i++;
            new_max_mail_kbytes = atol(argv[i]);
            if (new_max_mail_kbytes < 0 ||
                    new_max_mail_kbytes > 32767) {
                json_error("--max-mail-kbytes must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--purge-days") == 0 &&
                   i + 1 < argc) {
            i++;
            new_purge_days = atol(argv[i]);
            if (new_purge_days < 0 || new_purge_days > 32767) {
                json_error("--purge-days must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--sig-lines") == 0 &&
                   i + 1 < argc) {
            i++;
            new_sig_lines = atol(argv[i]);
            if (new_sig_lines < 0 || new_sig_lines > 32767) {
                json_error("--sig-lines must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--daily-pfile-minutes") == 0 &&
                   i + 1 < argc) {
            i++;
            new_daily_pfile_minutes = atol(argv[i]);
            if (new_daily_pfile_minutes < 0 ||
                    new_daily_pfile_minutes > 32767) {
                json_error("--daily-pfile-minutes must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--file-ratio") == 0 &&
                   i + 1 < argc) {
            i++;
            new_file_ratio = atoi(argv[i]);
            if (new_file_ratio < 0 || new_file_ratio > 255) {
                json_error("--file-ratio must be 0-255");
                return 1;
            }
        } else if (strcmp(argv[i], "--byte-ratio") == 0 &&
                   i + 1 < argc) {
            i++;
            new_byte_ratio = atoi(argv[i]);
            if (new_byte_ratio < 0 || new_byte_ratio > 255) {
                json_error("--byte-ratio must be 0-255");
                return 1;
            }
        } else if (strcmp(argv[i], "--log-to-mail") == 0 &&
                   i + 1 < argc) {
            i++;
            new_log_to_mail = atol(argv[i]);
            if (new_log_to_mail < 0 || new_log_to_mail > 32767) {
                json_error("--log-to-mail must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--allow-aliases") == 0 &&
                   i + 1 < argc) {
            i++;
            new_allow_aliases = atoi(argv[i]);
            if (new_allow_aliases < 0 || new_allow_aliases > 2) {
                json_error("--allow-aliases must be 0-2");
                return 1;
            }
        } else if (strcmp(argv[i], "--delete-own") == 0 &&
                   i + 1 < argc) {
            i++;
            new_delete_own = atoi(argv[i]);
            if (new_delete_own < 0 || new_delete_own > 2) {
                json_error("--delete-own must be 0-2");
                return 1;
            }
        } else if (strcmp(argv[i], "--anonymous") == 0 &&
                   i + 1 < argc) {
            i++;
            new_anonymous = atoi(argv[i]);
            if (new_anonymous < 0 || new_anonymous > 2) {
                json_error("--anonymous must be 0-2");
                return 1;
            }
        } else if (strcmp(argv[i], "--private-area") == 0 &&
                   i + 1 < argc) {
            i++;
            new_private_area = atoi(argv[i]);
            if (new_private_area < 0 || new_private_area > 2) {
                json_error("--private-area must be 0-2");
                return 1;
            }
        } else if (strcmp(argv[i], "--callback") == 0 &&
                   i + 1 < argc) {
            i++;
            new_callback = atoi(argv[i]);
            if (new_callback < 0 || new_callback > 2) {
                json_error("--callback must be 0-2");
                return 1;
            }
        } else if (strcmp(argv[i], "--term-link") == 0 &&
                   i + 1 < argc) {
            i++;
            new_term_link = atoi(argv[i]);
            if (new_term_link < 0 || new_term_link > 1) {
                json_error("--term-link must be 0-1");
                return 1;
            }
        } else if (strcmp(argv[i], "--caller-id") == 0 &&
                   i + 1 < argc) {
            i++;
            new_caller_id = atoi(argv[i]);
            if (new_caller_id < 0 || new_caller_id > 1) {
                json_error("--caller-id must be 0-1");
                return 1;
            }
        } else if (strcmp(argv[i], "--page-sysop") == 0 &&
                   i + 1 < argc) {
            i++;
            new_page_sysop = atoi(argv[i]);
            if (new_page_sysop < 0 || new_page_sysop > 1) {
                json_error("--page-sysop must be 0-1");
                return 1;
            }
        } else if (strcmp(argv[i], "--alias") == 0 &&
                   i + 1 < argc) {
            i++;
            new_alias = atol(argv[i]);
            if (new_alias < 0 || new_alias > 32767) {
                json_error("--alias must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--dictionary") == 0 &&
                   i + 1 < argc) {
            i++;
            new_dictionary = atol(argv[i]);
            if (new_dictionary < 0 || new_dictionary > 32767) {
                json_error("--dictionary must be 0-32767");
                return 1;
            }
        } else if (strcmp(argv[i], "--mbase-flags") == 0 &&
                   i + 1 < argc) {
            i++;
            if (!convert_access_string(argv[i], &val_mbase_flags)) {
                json_error("Invalid --mbase-flags value "
                    "(hex or group string like '1-3,5')");
                return 1;
            }
            have_mbase_flags = 1;
        } else if (strcmp(argv[i], "--fbase-flags") == 0 &&
                   i + 1 < argc) {
            i++;
            if (!convert_access_string(argv[i], &val_fbase_flags)) {
                json_error("Invalid --fbase-flags value "
                    "(hex or group string like '1-3,5')");
                return 1;
            }
            have_fbase_flags = 1;
        } else if (strcmp(argv[i], "--lbase-flags") == 0 &&
                   i + 1 < argc) {
            i++;
            if (!convert_access_string(argv[i], &val_lbase_flags)) {
                json_error("Invalid --lbase-flags value "
                    "(hex or group string like '1-3,5')");
                return 1;
            }
            have_lbase_flags = 1;
        } else if (strcmp(argv[i], "--abits") == 0 &&
                   i + 1 < argc) {
            i++;
            if (!parse_hex_ulong(argv[i], &val_abits)) {
                json_error("Invalid --abits hex value");
                return 1;
            }
            have_abits = 1;
        } else if (strcmp(argv[i], "--abits2") == 0 &&
                   i + 1 < argc) {
            i++;
            if (!parse_hex_ulong(argv[i], &val_abits2)) {
                json_error("Invalid --abits2 hex value");
                return 1;
            }
            have_abits2 = 1;
        } else if (strcmp(argv[i], "--log-flags") == 0 &&
                   i + 1 < argc) {
            i++;
            if (!parse_hex_ulong(argv[i], &val_log_flags)) {
                json_error("Invalid --log-flags hex value");
                return 1;
            }
            have_log_flags = 1;
        } else if (strncmp(argv[i], "--", 2) == 0) {
            /* Unknown flag -- error immediately */
            char ebuf[128];
            snprintf(ebuf, sizeof(ebuf), "Unknown flag: %s",
                argv[i]);
            json_error(ebuf);
            return 1;
        }
    }

    /* Validate at least one field was specified */
    {
        int nchanged =
            (new_name != NULL) +
            (new_expire_days != LONG_MIN) +
            (new_expire_access >= 0) +
            (new_daily_minutes != LONG_MIN) +
            (new_call_minutes != LONG_MIN) +
            (new_calls != LONG_MIN) +
            (new_idle != LONG_MIN) +
            (new_editor_lines != LONG_MIN) +
            (new_messages != LONG_MIN) +
            (new_feedbacks != LONG_MIN) +
            (new_daily_downloads != LONG_MIN) +
            (new_daily_uploads != LONG_MIN) +
            have_daily_down_bytes +
            have_daily_up_bytes +
            (new_max_mail_kbytes != LONG_MIN) +
            (new_purge_days != LONG_MIN) +
            (new_sig_lines != LONG_MIN) +
            (new_daily_pfile_minutes != LONG_MIN) +
            (new_file_ratio >= 0) +
            (new_byte_ratio >= 0) +
            (new_log_to_mail != LONG_MIN) +
            (new_allow_aliases >= 0) +
            (new_delete_own >= 0) +
            (new_anonymous >= 0) +
            (new_private_area >= 0) +
            (new_callback >= 0) +
            (new_term_link >= 0) +
            (new_caller_id >= 0) +
            (new_page_sysop >= 0) +
            (new_alias != LONG_MIN) +
            (new_dictionary != LONG_MIN) +
            have_mbase_flags + have_fbase_flags +
            have_lbase_flags + have_abits +
            have_abits2 + have_log_flags;

        if (nchanged == 0) {
            json_error("No fields to edit");
            return 1;
        }
    }

    /* Apply changes to in-memory AGC[group] */
    ag = &myp->AGC[group];
    p = &ag->DefPrivs;

    if (new_name) {
        safe_strcpy(ag->Name, new_name, (int)sizeof(ag->Name));
        if (changed_count < 40)
            changed_fields[changed_count++] = "name";
    }
    if (new_expire_days != LONG_MIN) {
        ag->ExpireDays = (short)new_expire_days;
        if (changed_count < 40)
            changed_fields[changed_count++] = "expire_days";
    }
    if (new_expire_access >= 0) {
        ag->ExpireAccess = (UBYTE)new_expire_access;
        if (changed_count < 40)
            changed_fields[changed_count++] = "expire_access";
    }

    /* Privs short fields */
    if (new_daily_minutes != LONG_MIN) {
        p->DailyMinutes = (short)new_daily_minutes;
        if (changed_count < 40)
            changed_fields[changed_count++] = "daily_minutes";
    }
    if (new_call_minutes != LONG_MIN) {
        p->CallMinutes = (short)new_call_minutes;
        if (changed_count < 40)
            changed_fields[changed_count++] = "call_minutes";
    }
    if (new_calls != LONG_MIN) {
        p->Calls = (short)new_calls;
        if (changed_count < 40)
            changed_fields[changed_count++] = "calls_per_day";
    }
    if (new_idle != LONG_MIN) {
        p->Idle = (short)new_idle;
        if (changed_count < 40)
            changed_fields[changed_count++] = "idle_limit";
    }
    if (new_editor_lines != LONG_MIN) {
        p->EditorLines = (short)new_editor_lines;
        if (changed_count < 40)
            changed_fields[changed_count++] = "editor_lines";
    }
    if (new_messages != LONG_MIN) {
        p->Messages = (short)new_messages;
        if (changed_count < 40)
            changed_fields[changed_count++] = "messages";
    }
    if (new_feedbacks != LONG_MIN) {
        p->Feedbacks = (short)new_feedbacks;
        if (changed_count < 40)
            changed_fields[changed_count++] = "feedbacks";
    }
    if (new_daily_downloads != LONG_MIN) {
        p->DailyDownloads = (short)new_daily_downloads;
        if (changed_count < 40)
            changed_fields[changed_count++] = "daily_downloads";
    }
    if (new_daily_uploads != LONG_MIN) {
        p->DailyUploads = (short)new_daily_uploads;
        if (changed_count < 40)
            changed_fields[changed_count++] = "daily_uploads";
    }
    if (have_daily_down_bytes) {
        p->DailyDownBytes = new_daily_down_bytes;
        if (changed_count < 40)
            changed_fields[changed_count++] = "daily_down_bytes";
    }
    if (have_daily_up_bytes) {
        p->DailyUpBytes = new_daily_up_bytes;
        if (changed_count < 40)
            changed_fields[changed_count++] = "daily_up_bytes";
    }
    if (new_max_mail_kbytes != LONG_MIN) {
        p->MaxMailKBytes = (short)new_max_mail_kbytes;
        if (changed_count < 40)
            changed_fields[changed_count++] = "max_mail_kbytes";
    }
    if (new_purge_days != LONG_MIN) {
        p->PurgeDays = (short)new_purge_days;
        if (changed_count < 40)
            changed_fields[changed_count++] = "purge_days";
    }
    if (new_sig_lines != LONG_MIN) {
        p->SigLines = (short)new_sig_lines;
        if (changed_count < 40)
            changed_fields[changed_count++] = "sig_lines";
    }
    if (new_daily_pfile_minutes != LONG_MIN) {
        p->DailyPfileMinutes = (short)new_daily_pfile_minutes;
        if (changed_count < 40)
            changed_fields[changed_count++] = "daily_pfile_minutes";
    }
    if (new_log_to_mail != LONG_MIN) {
        p->LogToMail = (short)new_log_to_mail;
        if (changed_count < 40)
            changed_fields[changed_count++] = "log_to_mail";
    }
    if (new_alias != LONG_MIN) {
        p->Alias = (short)new_alias;
        if (changed_count < 40)
            changed_fields[changed_count++] = "alias";
    }
    if (new_dictionary != LONG_MIN) {
        p->Dictionary = (short)new_dictionary;
        if (changed_count < 40)
            changed_fields[changed_count++] = "dictionary";
    }

    /* Privs UBYTE fields */
    if (new_file_ratio >= 0) {
        p->FileRatio = (UBYTE)new_file_ratio;
        if (changed_count < 40)
            changed_fields[changed_count++] = "file_ratio";
    }
    if (new_byte_ratio >= 0) {
        p->ByteRatio = (UBYTE)new_byte_ratio;
        if (changed_count < 40)
            changed_fields[changed_count++] = "byte_ratio";
    }
    if (new_allow_aliases >= 0) {
        p->AllowAliases = (UBYTE)new_allow_aliases;
        if (changed_count < 40)
            changed_fields[changed_count++] = "allow_aliases";
    }
    if (new_delete_own >= 0) {
        p->DeleteOwn = (UBYTE)new_delete_own;
        if (changed_count < 40)
            changed_fields[changed_count++] = "delete_own";
    }
    if (new_anonymous >= 0) {
        p->Anonymous = (UBYTE)new_anonymous;
        if (changed_count < 40)
            changed_fields[changed_count++] = "anonymous";
    }
    if (new_private_area >= 0) {
        p->PrivateArea = (UBYTE)new_private_area;
        if (changed_count < 40)
            changed_fields[changed_count++] = "private_area";
    }
    if (new_callback >= 0) {
        p->CallBack = (UBYTE)new_callback;
        if (changed_count < 40)
            changed_fields[changed_count++] = "callback";
    }
    if (new_term_link >= 0) {
        p->TermLink = (UBYTE)new_term_link;
        if (changed_count < 40)
            changed_fields[changed_count++] = "term_link";
    }
    if (new_caller_id >= 0) {
        p->CallerID = (UBYTE)new_caller_id;
        if (changed_count < 40)
            changed_fields[changed_count++] = "caller_id";
    }
    if (new_page_sysop >= 0) {
        p->PageSysop = (UBYTE)new_page_sysop;
        if (changed_count < 40)
            changed_fields[changed_count++] = "page_sysop";
    }

    /* Hex bitmask fields */
    if (have_mbase_flags) {
        p->MBaseFlags = (long)val_mbase_flags;
        if (changed_count < 40)
            changed_fields[changed_count++] = "mbase_flags";
    }
    if (have_fbase_flags) {
        p->FBaseFlags = (long)val_fbase_flags;
        if (changed_count < 40)
            changed_fields[changed_count++] = "fbase_flags";
    }
    if (have_lbase_flags) {
        p->LBaseFlags = (long)val_lbase_flags;
        if (changed_count < 40)
            changed_fields[changed_count++] = "lbase_flags";
    }
    if (have_abits) {
        p->ABits = (ULONG)val_abits;
        if (changed_count < 40)
            changed_fields[changed_count++] = "abits";
    }
    if (have_abits2) {
        p->ABits2 = (ULONG)val_abits2;
        if (changed_count < 40)
            changed_fields[changed_count++] = "abits2";
    }
    if (have_log_flags) {
        p->LogFlags = (long)val_log_flags;
        if (changed_count < 40)
            changed_fields[changed_count++] = "log_flags";
    }

    /* Write to disk */
    disk_rc = write_access_data_disk(myp->AGC);

    if (disk_rc == -2) {
        /* Critical: original deleted but rename failed.
         * Temp file has the only copy. */
        json_error(
            "CRITICAL: Disk write partially failed. "
            "SysData:bbs.adata was deleted but "
            "SysData:bbs.adata.tmp could not be renamed. "
            "Manually rename bbs.adata.tmp to bbs.adata "
            "before rebooting. In-memory state is current.");
        return 1;
    }
    if (disk_rc == -1) {
        /* Recoverable: original intact on disk, temp cleaned up.
         * In-memory change already applied. */
        json_error(
            "Disk write failed (original bbs.adata intact). "
            "In-memory state is updated. Re-run to retry "
            "disk persistence.");
        return 1;
    }

    /* Emit success JSON */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "updated");

    emit_group_detail(&js, ag, group);

    json_key(&js, "fields_changed");
    json_arr_open(&js);
    for (i = 0; i < changed_count; i++)
        json_str(&js, changed_fields[i]);
    json_arr_close(&js);

    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- group transpose ---- */

/*
 * Maximum number of candidate accounts for transpose.
 * Fixed-size to avoid VLA. 2000 accounts is well beyond any
 * realistic CNet BBS deployment.
 */
#define MAX_TRANSPOSE_CANDIDATES 2000

int cmd_group_transpose(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int group;
    long num_accounts;
    int candidate_count = 0;
    int modified_count = 0;
    int skipped_count = 0;
    int i;
    char buf[128];

    /* Fixed-size candidate array (account numbers, 1-based) */
    static short candidates[MAX_TRANSPOSE_CANDIDATES];

    if (argc < 2) {
        json_error("Usage: cnet-cli group transpose <group-number>");
        return 1;
    }

    if (!all_digits(argv[1])) {
        json_error("Group number must be 0-31");
        return 1;
    }

    group = atoi(argv[1]);
    if (group < 0 || group > 31) {
        json_error("Group number must be 0-31");
        return 1;
    }

    num_accounts = myp->Nums[NUMS_CURRENT_ACCOUNTS];

    /* Phase A: Scan Key[] under SEM[1] shared to find candidates */
    ObtainSemaphoreShared(&myp->SEM[1]);

    for (i = 0; i < num_accounts && i < MAX_TRANSPOSE_CANDIDATES; i++) {
        struct KeyElement4 *key = &myp->Key[i];

        /* Skip empty/deleted slots */
        if (key->Handle[0] == '\0')
            continue;

        /* Skip accounts not in target group */
        if (key->Access != (BYTE)group)
            continue;

        candidates[candidate_count++] = (short)(i + 1); /* 1-based */
    }

    ReleaseSemaphore(&myp->SEM[1]);

    /* Phase B: Lock each candidate, verify group, copy DefPrivs */
    for (i = 0; i < candidate_count; i++) {
        short account = candidates[i];
        struct UserData *user;

        user = LockAccount(account);
        if (!user) {
            skipped_count++;
            continue;
        }

        /* Re-verify group assignment under lock (Key[] may be stale) */
        if (user->Access != (BYTE)group) {
            UnLockAccount(account, 0);  /* discard, no changes */
            continue;
        }

        /* Copy 92-byte Privs struct from group template to user */
        memcpy(&user->MyPrivs, &myp->AGC[group].DefPrivs,
            sizeof(struct Privs));

        UnLockAccount(account, 1);  /* save=1, persist to bbs.udata4 */
        modified_count++;
    }

    /* Emit JSON response */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "status", "transposed");
    json_kv_int(&js, "group", (long)group);
    json_kv_str(&js, "group_name",
        strip_mci(buf, sizeof(buf), myp->AGC[group].Name));
    json_kv_int(&js, "accounts_modified", (long)modified_count);
    json_kv_int(&js, "accounts_skipped", (long)skipped_count);
    json_kv_int(&js, "total_scanned", num_accounts);

    if (myp->AGC[group].Name[0] == '\0')
        warn_add("Group has no name");

    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}
