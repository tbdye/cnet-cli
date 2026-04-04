/*
 * group.c -- Access group commands for cnet-cli
 *
 * Phase 10: group list, group show
 *
 * AGC[] is static configuration loaded at boot -- no semaphores needed.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>

#include "group.h"
#include "json.h"
#include "util.h"

/* ---- internal helpers ---- */

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

/* ---- group show ---- */

int cmd_group_show(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct AccessGroup *ag;
    struct Privs *p;
    int group;
    char buf[128];
    char hbuf[16];

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

    ag = &myp->AGC[group];
    p = &ag->DefPrivs;

    json_init(&js, stdout);
    json_obj_open(&js);

    json_kv_int(&js, "id", (long)group);
    json_kv_str(&js, "name",
        strip_mci(buf, sizeof(buf), ag->Name));
    json_kv_bool(&js, "defined",
        ag->Name[0] != '\0' ? 1 : 0);
    json_kv_int(&js, "expire_days", (long)ag->ExpireDays);
    json_kv_int(&js, "expire_access", (long)ag->ExpireAccess);

    /* Full privileges object */
    json_key(&js, "privileges");
    json_obj_open(&js);

    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->MBaseFlags);
    json_kv_str(&js, "mbase_flags", hbuf);
    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->FBaseFlags);
    json_kv_str(&js, "fbase_flags", hbuf);
    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->LBaseFlags);
    json_kv_str(&js, "lbase_flags", hbuf);
    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->ABits);
    json_kv_str(&js, "abits", hbuf);
    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->ABits2);
    json_kv_str(&js, "abits2", hbuf);

    json_kv_int(&js, "daily_down_bytes", p->DailyDownBytes);
    json_kv_int(&js, "daily_up_bytes", p->DailyUpBytes);
    json_kv_int(&js, "calls_per_day", (long)p->Calls);
    json_kv_int(&js, "call_minutes", (long)p->CallMinutes);
    json_kv_int(&js, "daily_minutes", (long)p->DailyMinutes);
    json_kv_int(&js, "daily_downloads",
        (long)p->DailyDownloads);
    json_kv_int(&js, "daily_uploads", (long)p->DailyUploads);
    json_kv_int(&js, "messages", (long)p->Messages);
    json_kv_int(&js, "feedbacks", (long)p->Feedbacks);
    json_kv_int(&js, "editor_lines", (long)p->EditorLines);
    json_kv_int(&js, "idle_limit", (long)p->Idle);
    json_kv_int(&js, "max_mail_kbytes",
        (long)p->MaxMailKBytes);
    json_kv_int(&js, "purge_days", (long)p->PurgeDays);
    json_kv_int(&js, "file_ratio", (long)p->FileRatio);
    json_kv_int(&js, "byte_ratio", (long)p->ByteRatio);
    json_kv_int(&js, "sig_lines", (long)p->SigLines);
    json_kv_int(&js, "daily_pfile_minutes",
        (long)p->DailyPfileMinutes);
    json_kv_int(&js, "allow_aliases", (long)p->AllowAliases);
    json_kv_int(&js, "delete_own", (long)p->DeleteOwn);
    json_kv_int(&js, "anonymous", (long)p->Anonymous);
    json_kv_int(&js, "private_area", (long)p->PrivateArea);
    json_kv_int(&js, "callback", (long)p->CallBack);
    json_kv_int(&js, "term_link", (long)p->TermLink);
    json_kv_int(&js, "caller_id", (long)p->CallerID);
    json_kv_int(&js, "page_sysop", (long)p->PageSysop);
    json_kv_int(&js, "alias", (long)p->Alias);
    json_kv_int(&js, "dictionary", (long)p->Dictionary);

    snprintf(hbuf, sizeof(hbuf), "0x%08lx",
        (unsigned long)p->LogFlags);
    json_kv_str(&js, "log_flags", hbuf);
    json_kv_int(&js, "log_to_mail", (long)p->LogToMail);

    json_obj_close(&js);  /* privileges */

    /* Decoded flag names */
    emit_decoded_flags(&js, p);

    json_obj_close(&js);  /* top-level */
    json_finish(&js);

    return 0;
}
