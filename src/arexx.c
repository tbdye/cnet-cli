/*
 * arexx.c -- ARexx IPC core and command handlers for cnet-cli
 *
 * ARexx IPC operations: send, control
 *
 * Provides the reusable ARexx IPC function send_arexx_command() and
 * two command handlers that expose it via the CLI.
 *
 * All ARexx communication uses rexxsyslib.library. The library base
 * (RexxSysBase) is opened in main.c's init_cnet() and closed in
 * cleanup_cnet(). It is non-fatal if missing -- handlers check
 * before use.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>
#include <proto/rexxsyslib.h>

#include <rexx/storage.h>

#include "arexx.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;
extern struct RxsLib *RexxSysBase;

/* ---- ARexx IPC core ---- */

int send_arexx_command(const char *portname, const char *command,
                       LONG *out_rc, char *result_buf, int buf_size)
{
    struct MsgPort *replyport;
    struct MsgPort *target;
    struct RexxMsg *rmsg;

    if (!RexxSysBase)
        return -1;

    /* Create a temporary reply port */
    replyport = CreateMsgPort();
    if (!replyport)
        return -1;

    /* Create the ARexx message */
    rmsg = CreateRexxMsg(replyport, NULL, NULL);
    if (!rmsg) {
        DeleteMsgPort(replyport);
        return -1;
    }

    /* Clear argument slots (NDK best practice) */
    ClearRexxMsg(rmsg, 1);

    /* Create the command argstring and set rm_Args[0] */
    rmsg->rm_Args[0] = (STRPTR)CreateArgstring(
        (CONST_STRPTR)command, strlen(command));
    if (!rmsg->rm_Args[0]) {
        DeleteRexxMsg(rmsg);
        DeleteMsgPort(replyport);
        return -1;
    }

    /* Request command execution with result string */
    rmsg->rm_Action = RXCOMM | RXFF_RESULT;

    /*
     * Find the target port and send the message.
     * Forbid/Permit bracket prevents the port from vanishing
     * between FindPort and PutMsg.
     */
    Forbid();
    target = FindPort((CONST_STRPTR)portname);
    if (target)
        PutMsg(target, &rmsg->rm_Node);
    Permit();

    if (!target) {
        /* Target port not found -- clean up and return failure */
        DeleteArgstring((UBYTE *)rmsg->rm_Args[0]);
        DeleteRexxMsg(rmsg);
        DeleteMsgPort(replyport);
        return -1;
    }

    /* Wait for the reply (blocks until the receiver responds) */
    WaitPort(replyport);
    GetMsg(replyport);

    /* Extract results */
    *out_rc = rmsg->rm_Result1;
    if (rmsg->rm_Result1 == 0 && rmsg->rm_Result2) {
        /*
         * rm_Result2 is an argstring created by the receiver.
         * Validate with LengthArgstring before using -- if the
         * length is wildly unexpected, skip DeleteArgstring to
         * avoid corruption (accept a small leak).
         */
        ULONG arglen = LengthArgstring(
            (CONST UBYTE *)(APTR)rmsg->rm_Result2);

        if (arglen <= 4096) {
            if (result_buf) {
                int copylen = (int)arglen;
                if (copylen >= buf_size)
                    copylen = buf_size - 1;
                memcpy(result_buf,
                       (char *)(APTR)rmsg->rm_Result2, copylen);
                result_buf[copylen] = '\0';
            }
            DeleteArgstring((UBYTE *)(APTR)rmsg->rm_Result2);
        } else {
            /* Unexpected length -- skip free, accept leak */
            if (result_buf)
                result_buf[0] = '\0';
        }
    } else if (result_buf) {
        result_buf[0] = '\0';
    }

    /* Cleanup: argstring, message, port */
    DeleteArgstring((UBYTE *)rmsg->rm_Args[0]);
    DeleteRexxMsg(rmsg);
    DeleteMsgPort(replyport);

    return 0;
}

/* ---- arexx send ---- */

int cmd_arexx_send(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char portname[16];
    char cmdbuf[512];
    char result[1024];
    LONG rc;
    int port_num;
    int offset;
    int i;
    int ret;

    (void)myp;

    if (argc < 3) {
        json_error("Usage: cnet-cli arexx send <port-number>"
                   " <command...>");
        return 1;
    }

    if (!RexxSysBase) {
        json_error("Cannot open rexxsyslib.library");
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

    /* Build target port name: CNETREXX{N} (no dot, no padding) */
    snprintf(portname, sizeof(portname), "CNETREXX%d", port_num);

    /* Concatenate argv[2..] into a single command string */
    offset = 0;
    for (i = 2; i < argc; i++) {
        int len;
        if (i > 2) {
            if (offset < (int)sizeof(cmdbuf) - 1)
                cmdbuf[offset++] = ' ';
        }
        len = (int)strlen(argv[i]);
        if (offset + len >= (int)sizeof(cmdbuf) - 1) {
            /* Truncate to fit */
            len = (int)sizeof(cmdbuf) - 1 - offset;
            if (len < 0) len = 0;
        }
        memcpy(cmdbuf + offset, argv[i], len);
        offset += len;
    }
    cmdbuf[offset] = '\0';

    /* Send the ARexx command */
    ret = send_arexx_command(portname, cmdbuf, &rc, result,
                             (int)sizeof(result));
    if (ret < 0) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf),
                 "ARexx port %s not found", portname);
        json_error(errbuf);
        return 1;
    }

    /* Emit JSON result */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "port", portname);
    json_kv_str(&js, "command", cmdbuf);
    json_kv_int(&js, "rc", (long)rc);
    if (result[0])
        json_kv_str(&js, "result", result);
    else
        json_kv_null(&js, "result");
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- arexx control ---- */

int cmd_arexx_control(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char cmdbuf[512];
    char result[1024];
    LONG rc;
    int offset;
    int i;
    int ret;

    (void)myp;

    if (argc < 2) {
        json_error("Usage: cnet-cli arexx control <command...>");
        return 1;
    }

    if (!RexxSysBase) {
        json_error("Cannot open rexxsyslib.library");
        return 1;
    }

    /* Concatenate argv[1..] into a single command string */
    offset = 0;
    for (i = 1; i < argc; i++) {
        int len;
        if (i > 1) {
            if (offset < (int)sizeof(cmdbuf) - 1)
                cmdbuf[offset++] = ' ';
        }
        len = (int)strlen(argv[i]);
        if (offset + len >= (int)sizeof(cmdbuf) - 1) {
            len = (int)sizeof(cmdbuf) - 1 - offset;
            if (len < 0) len = 0;
        }
        memcpy(cmdbuf + offset, argv[i], len);
        offset += len;
    }
    cmdbuf[offset] = '\0';

    /* Send the ARexx command to CONTROLREXX.1 */
    ret = send_arexx_command("CONTROLREXX.1", cmdbuf, &rc, result,
                             (int)sizeof(result));
    if (ret < 0) {
        json_error("ARexx port CONTROLREXX.1 not found");
        return 1;
    }

    /* Emit JSON result */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_str(&js, "port", "CONTROLREXX.1");
    json_kv_str(&js, "command", cmdbuf);
    json_kv_int(&js, "rc", (long)rc);
    if (result[0])
        json_kv_str(&js, "result", result);
    else
        json_kv_null(&js, "result");
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}
