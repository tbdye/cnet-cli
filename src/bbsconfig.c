/*
 * bbsconfig.c -- BBS configuration commands for cnet-cli
 *
 * BBS configuration display and control:
 *   config show        -- global config as JSON
 *   config flags       -- control panel toggle flags (read/write)
 *   config reload-text -- trigger BBSTEXT/BBSMENU reload
 *   config port <N>    -- per-port configuration (loaded or disk)
 *
 * SECURITY: MyLinkPass, SysPassword, and ppass are never emitted.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>
#include <proto/dos.h>

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
        json_kv_str(&js, "smtpd_temp_dir", gc2->smtpdtemp);
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

/* ------------------------------------------------------------------ */
/* config reload-text                                                  */
/* ------------------------------------------------------------------ */

int cmd_config_reload_text(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;

    (void)argc;
    (void)argv;

    if (!myp->MPE) {
        json_error("MainPortExtension not available");
        return 1;
    }

    ObtainSemaphore(&myp->MPE->sem[0]);
    myp->MPE->reload_text = 1;
    ReleaseSemaphore(&myp->MPE->sem[0]);

    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "action", "reload-text");
    json_kv_str(&js, "status", "triggered");
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ------------------------------------------------------------------ */
/* config flags                                                        */
/* ------------------------------------------------------------------ */

static const struct flag_info {
    const char *name;
    int bit;
} flag_table[] = {
    { "doors_closed", CHECKBIT_DOORSCLOSED },
    { "files_closed", CHECKBIT_FILESCLOSED },
    { "msgs_closed",  CHECKBIT_MSGSCLOSED  },
    { "no_new_users", CHECKBIT_NONUSER     },
    { "sysop_in",     CHECKBIT_SYSOPIN     },
    { NULL, 0 }
};

/*
 * Emit the flags object from a check byte.
 */
static void emit_flags(struct json_state *js, UBYTE check)
{
    const struct flag_info *fi;

    json_key(js, "flags");
    json_obj_open(js);
    for (fi = flag_table; fi->name; fi++)
        json_kv_bool(js, fi->name, (check >> fi->bit) & 1);
    json_obj_close(js);
}

int cmd_config_flags(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    UBYTE check;
    int i;
    int have_set = 0;

    /* Scan for --set arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--set") == 0) {
            have_set = 1;
            break;
        }
    }

    if (!have_set) {
        /* Read-only path */
        check = myp->pc[0].check;

        json_init(&js, stdout);
        json_obj_open(&js);
        emit_flags(&js, check);
        json_obj_close(&js);
        json_finish(&js);
        return 0;
    }

    /* Write path: parse --set flag=value pairs */
    check = myp->pc[0].check;

    for (i = 1; i < argc; i++) {
        const struct flag_info *fi;
        char *eq;
        char name_buf[32];
        const char *val_str;
        int found = 0;
        int val;

        if (strcmp(argv[i], "--set") != 0)
            continue;

        i++;
        if (i >= argc) {
            json_error("--set requires a flag=value argument");
            return 1;
        }

        /* Split on '=' */
        eq = strchr(argv[i], '=');
        if (!eq) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "Invalid flag assignment (expected flag=value): %s",
                argv[i]);
            json_error(buf);
            return 1;
        }

        /* Copy flag name */
        {
            int namelen = (int)(eq - argv[i]);
            if (namelen <= 0 || namelen >= (int)sizeof(name_buf)) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Unknown flag: %s", argv[i]);
                json_error(buf);
                return 1;
            }
            memcpy(name_buf, argv[i], (size_t)namelen);
            name_buf[namelen] = '\0';
        }

        val_str = eq + 1;

        /* Parse value */
        if (strcmp(val_str, "true") == 0 || strcmp(val_str, "1") == 0) {
            val = 1;
        } else if (strcmp(val_str, "false") == 0 ||
                   strcmp(val_str, "0") == 0) {
            val = 0;
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "Invalid flag value (expected true/false/1/0): %s",
                val_str);
            json_error(buf);
            return 1;
        }

        /* Look up flag name */
        for (fi = flag_table; fi->name; fi++) {
            if (strcmp(name_buf, fi->name) == 0) {
                found = 1;
                if (val)
                    check |= (UBYTE)(1 << fi->bit);
                else
                    check &= (UBYTE)~(1 << fi->bit);
                break;
            }
        }

        if (!found) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Unknown flag: %s", name_buf);
            json_error(buf);
            return 1;
        }
    }

    /* Apply to memory */
    myp->pc[0].check = check;
    myp->check_latch = 1;

    /* Persist to disk: in-place write to cnet:bbscontrol3 */
    {
        BPTR fh;

        fh = Open((CONST_STRPTR)"cnet:bbscontrol3", MODE_OLDFILE);
        if (fh) {
            long seek_rc;
            long written;

            seek_rc = Seek(fh, 0L, OFFSET_BEGINNING);
            if (seek_rc == -1L) {
                warn_add("Disk write to cnet:bbscontrol3 failed"
                    " (seek); change is in-memory only"
                    " and will not persist across reboot.");
            } else {
                written = Write(fh, (APTR)&myp->pc[0],
                    (long)sizeof(struct PortConfig));
                if (written != (long)sizeof(struct PortConfig)) {
                    warn_add("Disk write to cnet:bbscontrol3 failed"
                        " (write); change is in-memory only"
                        " and will not persist across reboot.");
                }
            }
            Close(fh);
        } else {
            warn_add("Disk write to cnet:bbscontrol3 failed"
                " (open); change is in-memory only"
                " and will not persist across reboot.");
        }
    }

    /* Emit response */
    json_init(&js, stdout);
    json_obj_open(&js);
    emit_flags(&js, check);
    json_kv_bool(&js, "updated", 1);
    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ------------------------------------------------------------------ */
/* config port                                                         */
/* ------------------------------------------------------------------ */

static const char *screen_open_name(UBYTE open)
{
    switch (open) {
    case 0: return "none";
    case SCR_OPEN_PERMANENT: return "permanent";
    case SCR_OPEN_FORCALL:   return "forcall";
    case SCR_OPEN_WORKBENCH: return "workbench";
    default: return "unknown";
    }
}

static const char *interlace_name(UBYTE lace)
{
    switch (lace) {
    case 0: return "none";
    case 1: return "24-line";
    case 2: return "49-line";
    default: return "unknown";
    }
}

static void emit_port_config(struct json_state *js,
    const struct PortConfig *pc)
{
    json_key(js, "port_config");
    json_obj_open(js);
    json_kv_bool(js, "online", pc->online != 0);
    json_kv_str(js, "screen_open", screen_open_name(pc->open));
    json_kv_int(js, "check", (long)pc->check);
    json_kv_int(js, "idle", (long)pc->idle);
    json_kv_bool(js, "offline", pc->offline != 0);
    json_kv_int(js, "bplanes", (long)pc->bplanes);
    json_kv_str(js, "interlace", interlace_name(pc->lace));
    json_obj_close(js);
}

/*
 * Emit serial_config JSON from a SerPort4.
 * ppass is intentionally omitted for security.
 */
static void emit_serial_config(struct json_state *js,
    const struct SerPort4 *sp)
{
    json_key(js, "serial_config");
    json_obj_open(js);
    json_kv_str(js, "device_name", sp->name);
    json_kv_int(js, "unit", sp->unit);
    json_kv_int(js, "flags", sp->flags);
    json_kv_int(js, "idle_baud", (long)sp->idlebaud);
    json_kv_int(js, "escape", (long)sp->escape);
    json_kv_int(js, "answer_pause", (long)sp->answerpause);
    json_kv_int(js, "seconds", (long)sp->seconds);
    json_kv_str(js, "init1", sp->init1);
    json_kv_str(js, "init2", sp->init2);
    json_kv_str(js, "hangup", sp->hangup);
    json_kv_str(js, "dialout", sp->dialout);
    json_kv_str(js, "answer", sp->answer);
    json_kv_str(js, "offhook", sp->offhook);
    json_kv_str(js, "terminal", sp->terminal);
    json_kv_str(js, "caller_id", sp->callerid);
    json_kv_str(js, "ring", sp->ring);
    json_kv_str(js, "connect", sp->connect);
    json_kv_str(js, "termlink", sp->termlink);
    json_kv_bool(js, "null_modem", sp->null != 0);
    json_kv_str(js, "idle_who", sp->IdleWho);

    json_key(js, "port_flags");
    json_obj_open(js);
    json_kv_bool(js, "show_on_who",
        (sp->portflags & SP_PF_SHOWONWHO) != 0);
    json_kv_bool(js, "telnetd",
        (sp->portflags & SP_PF_TELNETD) != 0);
    json_kv_bool(js, "offclose",
        (sp->portflags & SP_PF_OFFCLOSE) != 0);
    json_obj_close(js);

    json_obj_close(js);
}

int cmd_config_port(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int port_num;
    int loaded = 0;
    struct PortConfig pc;
    struct SerPort4 sp;
    int have_sp = 0;

    if (argc < 2) {
        json_error("Usage: cnet-cli config port <port-number>");
        return 1;
    }

    /* Validate port number */
    if (!all_digits(argv[1])) {
        json_error("Port number must be 0-99");
        return 1;
    }
    port_num = atoi(argv[1]);
    if (port_num < 0 || port_num > 99) {
        json_error("Port number must be 0-99");
        return 1;
    }

    /* Determine if port is loaded (two-step check) */
    if (port_num <= (int)myp->HiPort) {
        struct PortData *z = myp->PortZ[port_num];
        if (z && z != myp->z0)
            loaded = 1;
    }

    if (loaded) {
        /* Loaded port: read from memory */
        memcpy(&pc, &myp->pc[port_num], sizeof(pc));

        {
            struct PortData *z = myp->PortZ[port_num];
            if (z->PDE) {
                memcpy(&sp, &z->PDE->sp, sizeof(sp));
                have_sp = 1;
            } else {
                char wbuf[64];
                snprintf(wbuf, sizeof(wbuf),
                    "PDE unavailable for loaded port %d", port_num);
                warn_add(wbuf);
            }
        }
    } else {
        /* Unloaded port: read PortConfig from disk */
        BPTR fh;

        fh = Open((CONST_STRPTR)"cnet:bbscontrol3", MODE_OLDFILE);
        if (!fh) {
            json_error("Cannot open cnet:bbscontrol3");
            return 1;
        }

        {
            long seek_rc;
            long nread;

            seek_rc = Seek(fh,
                (long)port_num * (long)sizeof(struct PortConfig),
                OFFSET_BEGINNING);
            if (seek_rc == -1L) {
                Close(fh);
                json_error("Seek failed on cnet:bbscontrol3");
                return 1;
            }

            nread = Read(fh, (APTR)&pc,
                (long)sizeof(struct PortConfig));
            Close(fh);

            if (nread != (long)sizeof(struct PortConfig)) {
                json_error("Read failed on cnet:bbscontrol3");
                return 1;
            }
        }

        /* Read SerPort4 from disk */
        {
            char path[40];

            snprintf(path, sizeof(path),
                "cnet:configs/bbsport%d", port_num);

            fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
            if (fh) {
                long nread;

                nread = Read(fh, (APTR)&sp,
                    (long)sizeof(struct SerPort4));
                Close(fh);

                if (nread == (long)sizeof(struct SerPort4)) {
                    have_sp = 1;
                } else {
                    char wbuf[80];
                    snprintf(wbuf, sizeof(wbuf),
                        "Incomplete serial config file for port %d",
                        port_num);
                    warn_add(wbuf);
                }
            } else {
                char wbuf[64];
                snprintf(wbuf, sizeof(wbuf),
                    "No serial config file for port %d",
                    port_num);
                warn_add(wbuf);
            }
        }
    }

    /* Emit JSON */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_int(&js, "port", (long)port_num);
    json_kv_bool(&js, "loaded", loaded);

    emit_port_config(&js, &pc);

    if (have_sp)
        emit_serial_config(&js, &sp);
    else
        json_kv_null(&js, "serial_config");

    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}
