/*
 * bbsconfig.c -- BBS configuration commands for cnet-cli
 *
 * Phase 10: config show
 *
 * Reads the global configuration (NewConfig1) and extended
 * configuration (ConfigExtension) and emits them as JSON.
 *
 * SECURITY: MyLinkPass and SysPassword are never emitted.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>

#include "bbsconfig.h"
#include "json.h"
#include "util.h"

int cmd_config_show(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    struct NewConfig1 *gc;
    char buf[128];

    (void)argc;
    (void)argv;

    gc = &myp->gc;

    json_init(&js, stdout);
    json_obj_open(&js);

    /* ---- identity ---- */

    json_key(&js, "identity");
    json_obj_open(&js);
    json_kv_str(&js, "system_name",
        strip_mci(buf, sizeof(buf), gc->MySystemName));
    json_kv_str(&js, "sysop_name",
        strip_mci(buf, sizeof(buf), gc->MySysopName));
    json_kv_str(&js, "location", gc->MyLocation);
    json_kv_str(&js, "phone_number", gc->MyPhoneNumber);
    json_kv_str(&js, "bbs_id", gc->MyBBSID);
    json_kv_str(&js, "country", gc->MyCountry);
    json_kv_str(&js, "area_code", gc->MyAreaCode);
    json_kv_str(&js, "uucp_name", gc->MyUUCPName);
    json_kv_int(&js, "link_id", (long)gc->MyLinkID);
    json_obj_close(&js);

    /* ---- limits ---- */

    json_key(&js, "limits");
    json_obj_open(&js);
    json_kv_int(&js, "max_user_accounts", gc->maxUserAccounts);
    json_kv_int(&js, "max_open_pfiles", gc->MaxOpenPfiles);
    json_kv_int(&js, "num_rooms", gc->NumRooms);
    json_kv_int(&js, "max_link_ports", gc->nLinkPorts);
    json_kv_int(&js, "max_subboards", gc->nsub);
    json_kv_int(&js, "max_select", gc->nselect);
    json_kv_int(&js, "max_upload", gc->nupload);
    json_kv_int(&js, "max_list", gc->nlist);
    json_kv_int(&js, "max_logon_attempts", gc->maxLogonAttempts);
    json_kv_int(&js, "max_logon_time", gc->maxLogonTime);
    json_kv_int(&js, "max_yank_tasks", gc->maxYankTasks);
    json_kv_int(&js, "max_yank_size", gc->maxYankSize);
    json_kv_int(&js, "max_yank_days", gc->maxYankDays);
    json_kv_int(&js, "max_yanks_per_user", gc->maxYanksPerUser);
    json_kv_int(&js, "max_short_lines", gc->MaxShortLines);
    json_kv_int(&js, "abuffer_size", (long)gc->ABufferSize);
    json_kv_int(&js, "max_file_process", (long)gc->MaxFileProcess);
    json_obj_close(&js);

    /* ---- defaults ---- */

    json_key(&js, "defaults");
    json_obj_open(&js);
    json_kv_int(&js, "balance", gc->DefBalance);
    json_kv_int(&js, "net_credits", gc->DefNetCredits);
    json_kv_int(&js, "byte_credits", gc->DefByteCredits);
    json_kv_int(&js, "file_credits", gc->DefFileCredits);
    json_kv_int(&js, "door_points", gc->DefDoorPoints);
    json_kv_int(&js, "time_form", gc->DefTimeForm);
    json_kv_int(&js, "default_color", gc->DefaultColor);
    json_kv_str(&js, "default_protocol", gc->DefDefProtocol);
    json_kv_int(&js, "mail_sort", (long)gc->DefMailSort);
    json_obj_close(&js);

    /* ---- paths ---- */

    json_key(&js, "paths");
    json_obj_open(&js);
    json_kv_str(&js, "olm", gc->OLMpath);
    json_kv_str(&js, "zip", gc->ZIPpath);
    json_kv_str(&js, "extract", gc->EXTRACTpath);
    json_kv_str(&js, "yank_work", gc->YANKwork);
    json_kv_str(&js, "ram", gc->RAMpath);
    json_kv_str(&js, "terminal", gc->TERMpath);
    json_kv_str(&js, "local_editor", gc->LocalEditor);
    json_kv_str(&js, "cdrom", gc->CDROMpath);
    json_kv_str(&js, "dictionary", gc->DictPath);
    json_kv_str(&js, "outbound", gc->OutboundPath);
    json_kv_str(&js, "inbound", gc->InboundPath);
    json_kv_str(&js, "ram_upload", gc->RamUpload);
    json_kv_str(&js, "disk_upload", gc->DiskUpload);
    json_kv_str(&js, "nodelist", gc->Nodelist);
    json_kv_str(&js, "news", gc->NewsPath);
    json_kv_str(&js, "uumail", gc->UUMailPath);
    json_obj_close(&js);

    /* ---- options ---- */

    json_key(&js, "options");
    json_obj_open(&js);
    json_kv_bool(&js, "logon_feedback", gc->LogonFeedback != 0);
    json_kv_bool(&js, "logon_search", gc->LogonSearch != 0);
    json_kv_bool(&js, "guest_users", gc->GuestUsers != 0);
    json_kv_bool(&js, "hide_status", gc->HideStatus != 0);
    json_kv_bool(&js, "conf_profile", gc->ConfProfile != 0);
    json_kv_bool(&js, "mail_feedback", gc->MailFeedback != 0);
    json_kv_bool(&js, "separate_texts", gc->SeparateTexts != 0);
    json_kv_int(&js, "indent_spaces", (long)gc->IndentSpaces);
    json_kv_bool(&js, "skip_idle_ports", gc->SkipIdlePorts != 0);
    json_kv_int(&js, "blank_ticks", gc->BlankTicks);
    json_kv_int(&js, "blank_bright", gc->BlankBright);
    json_kv_int(&js, "blist_purge_days", gc->BListPurgeDays);
    json_kv_bool(&js, "mid_from_handle", gc->MIDFromHandle != 0);
    json_kv_bool(&js, "file_task_notify", gc->FileTaskNotify != 0);
    json_kv_bool(&js, "monitor_uumail", gc->MonitorUUMail != 0);
    json_kv_bool(&js, "create_web_dir", gc->CreateWebDir != 0);
    json_kv_bool(&js, "news_task_post", gc->NewsTaskPost != 0);
    json_kv_bool(&js, "dynamic_ip", gc->dynamicIP != 0);
    json_kv_bool(&js, "force_empty_trash", gc->ForceEmptyTrash != 0);
    json_obj_close(&js);

    /* ---- resource_counts ---- */

    json_key(&js, "resource_counts");
    json_obj_open(&js);
    json_kv_int(&js, "archivers", gc->narc);
    json_kv_int(&js, "editors", gc->ned);
    json_kv_int(&js, "protocols", gc->nproto);
    json_kv_int(&js, "fido_networks", gc->nfido);
    json_kv_int(&js, "log_types", gc->nlog);
    json_obj_close(&js);

    /* ---- network ---- */

    json_key(&js, "network");
    json_obj_open(&js);
    json_kv_str(&js, "news_server", gc->NewsServer);
    json_kv_int(&js, "nntp_port",
        (long)(int)(unsigned char)gc->NNTPPort);
    json_kv_str(&js, "root_name", gc->RootName);
    json_kv_uint(&js, "ram_upload_size",
        (unsigned long)gc->RamUploadSize);

    if (myp->MPE) {
        struct ConfigExtension *gc2 = &myp->MPE->gc2;

        json_kv_str(&js, "mail_server", gc2->MailServer);
        json_kv_bool(&js, "smtp_mail", gc2->SMTPMail != 0);
        json_kv_uint(&js, "mail_timeout",
            (unsigned long)gc2->mailtimeout);
        json_kv_uint(&js, "news_timeout",
            (unsigned long)gc2->newstimeout);
        json_kv_str(&js, "timezone", gc2->TimeZone);
        json_kv_uint(&js, "smtpd_timeout",
            (unsigned long)gc2->smtpdtimeout);
        json_kv_str(&js, "port_log_dir", gc2->PortLogDir);
        json_kv_int(&js, "user_cache", (long)gc2->cache);
        json_kv_bool(&js, "telnetd_autoload",
            gc2->telnetd_aload != 0);
        json_kv_int(&js, "max_telnetd", (long)gc2->maxtelnetd);
        json_kv_bool(&js, "show_ip_where",
            gc2->ShowIPWhere != 0);
        json_kv_uint(&js, "min_telnetd_mem_kb",
            (unsigned long)gc2->mintelnetdmem);
        json_kv_uint(&js, "next_sub_serial",
            (unsigned long)gc2->nextsubser);
    }

    json_obj_close(&js);

    /* ---- task_buffer_limits ---- */

    json_key(&js, "task_buffer_limits");
    json_obj_open(&js);

    if (myp->MPE) {
        struct ConfigExtension *gc2 = &myp->MPE->gc2;

        json_kv_uint(&js, "mail_task",
            (unsigned long)gc2->MailTaskBufMax);
        json_kv_uint(&js, "news_task",
            (unsigned long)gc2->NewsTaskBufMax);
        json_kv_uint(&js, "file_task",
            (unsigned long)gc2->FileTaskBufMax);
        json_kv_uint(&js, "yank_task",
            (unsigned long)gc2->YankTaskBufMax);
        json_kv_uint(&js, "smtpd",
            (unsigned long)gc2->SMTPBufMax);
        json_kv_uint(&js, "telnetd",
            (unsigned long)gc2->TelnetdBufMax);
        json_kv_uint(&js, "ftpd",
            (unsigned long)gc2->FTPdBufMax);
    }

    json_obj_close(&js);

    json_obj_close(&js);
    json_finish(&js);

    return 0;
}
