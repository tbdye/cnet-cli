/*
 * port.c -- Port management commands for cnet-cli
 *
 * Phase 11: port load, port unload, port dump
 *
 * All three commands are thin wrappers around send_arexx_command().
 * port load and port unload target CONTROLREXX.1; port dump targets
 * CNETREXX{N}.
 *
 * CONTROLREXX.1 has a known issue on CNet v5.36b where RUNPORT and
 * CLOSEPORT return RC=0 but have no observable effect. The warning
 * is included in the JSON output.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>

#include "port.h"
#include "arexx.h"
#include "json.h"
#include "util.h"

/* From main.c */
extern struct Library *CNetBase;
extern struct RxsLib *RexxSysBase;

/* ---- port load ---- */

int cmd_port_load(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char cmdbuf[32];
    char result[256];
    LONG rc;
    int port_num;
    int ret;

    if (argc < 2) {
        json_error("Usage: cnet-cli port load <port-number>");
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

    /* Warn if port number exceeds configured range */
    if (port_num > (int)myp->HiPort) {
        char warnbuf[80];
        snprintf(warnbuf, sizeof(warnbuf),
                 "Port %d exceeds configured HiPort (%d)",
                 port_num, (int)myp->HiPort);
        warn_add(warnbuf);
    }

    /* Send RUNPORT via CONTROLREXX.1 */
    snprintf(cmdbuf, sizeof(cmdbuf), "RUNPORT %d", port_num);
    ret = send_arexx_command("CONTROLREXX.1", cmdbuf, &rc, result,
                             (int)sizeof(result));
    if (ret < 0) {
        json_error("ARexx port CONTROLREXX.1 not found"
                   " (CNet Control not running?)");
        return 1;
    }

    /* Emit JSON result */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_int(&js, "port", (long)port_num);
    json_kv_str(&js, "action", "load");
    json_kv_int(&js, "rc", (long)rc);
    json_kv_str(&js, "warning",
        "CONTROLREXX.1 commands may not take effect on CNet v5.36b");
    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- port unload ---- */

int cmd_port_unload(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char cmdbuf[32];
    char result[256];
    LONG rc;
    int port_num;
    int ret;

    if (argc < 2) {
        json_error("Usage: cnet-cli port unload <port-number>");
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

    /* Warn if port number exceeds configured range */
    if (port_num > (int)myp->HiPort) {
        char warnbuf[80];
        snprintf(warnbuf, sizeof(warnbuf),
                 "Port %d exceeds configured HiPort (%d)",
                 port_num, (int)myp->HiPort);
        warn_add(warnbuf);
    }

    /* Warn if a user is online on this port */
    if (port_num <= (int)myp->HiPort) {
        struct PortData *z = myp->PortZ[port_num];
        if (z && z != myp->z0 && z->OnLine) {
            char buf[64];
            strip_mci(buf, sizeof(buf), z->user1.Handle);
            {
                char wbuf[128];
                snprintf(wbuf, sizeof(wbuf),
                    "User \"%s\" is online on port %d", buf, port_num);
                warn_add(wbuf);
            }
        }
    }

    /* Send CLOSEPORT via CONTROLREXX.1 */
    snprintf(cmdbuf, sizeof(cmdbuf), "CLOSEPORT %d", port_num);
    ret = send_arexx_command("CONTROLREXX.1", cmdbuf, &rc, result,
                             (int)sizeof(result));
    if (ret < 0) {
        json_error("ARexx port CONTROLREXX.1 not found"
                   " (CNet Control not running?)");
        return 1;
    }

    /* Emit JSON result */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_int(&js, "port", (long)port_num);
    json_kv_str(&js, "action", "unload");
    json_kv_int(&js, "rc", (long)rc);
    json_kv_str(&js, "warning",
        "CONTROLREXX.1 commands may not take effect on CNet v5.36b");
    warn_emit(&js);
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}

/* ---- port dump ---- */

int cmd_port_dump(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    char portname[16];
    char result[256];
    char userbuf[64];
    LONG rc;
    int port_num;
    int ret;
    int has_user = 0;

    if (argc < 2) {
        json_error("Usage: cnet-cli port dump <port-number>");
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

    /* Pre-check: who is on this port? */
    userbuf[0] = '\0';
    if (port_num <= (int)myp->HiPort) {
        struct PortData *z = myp->PortZ[port_num];
        if (z && z != myp->z0 && z->OnLine) {
            strip_mci(userbuf, sizeof(userbuf), z->user1.Handle);
            has_user = 1;
        }
    }

    /* Send DROPCARRIER to CNETREXX{N} */
    snprintf(portname, sizeof(portname), "CNETREXX%d", port_num);
    ret = send_arexx_command(portname, "DROPCARRIER", &rc, result,
                             (int)sizeof(result));
    if (ret < 0) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf),
                 "ARexx port %s not found (port not loaded?)",
                 portname);
        json_error(errbuf);
        return 1;
    }

    /* Emit JSON result */
    json_init(&js, stdout);
    json_obj_open(&js);
    json_kv_int(&js, "port", (long)port_num);
    json_kv_str(&js, "action", "dump");
    if (has_user)
        json_kv_str(&js, "user", userbuf);
    json_kv_int(&js, "rc", (long)rc);
    if (!has_user)
        json_kv_str(&js, "warning",
            "no user online on this port");
    json_obj_close(&js);
    json_finish(&js);

    return 0;
}
