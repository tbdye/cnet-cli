/*
 * stats.c -- System statistics command for cnet-cli
 *
 * Phase 10: stats
 *
 * Reads system counters, SAM/SAG activity data, and boot time from
 * MainPort. SAM and SAG arrays are copied under SEM[18] shared lock; SAG is
 * included defensively as it shares the same update context.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>

#include "stats.h"
#include "json.h"
#include "util.h"

int cmd_stats_show(struct MainPort *myp)
{
    struct json_state js;
    long sam_copy[5][15];
    ULONG sag_copy[2][72];
    struct IsDate boot_date;
    char datebuf[20];
    int i, j;
    long online_count;

    /* Copy SAM, SAG, and boot date under SEM[18] shared lock.
     * SAG could be read without the lock (read-only data), but we include
     * it here defensively since both arrays share the same update context. */
    ObtainSemaphoreShared(&myp->SEM[18]);
    memcpy(sam_copy, myp->SAM, sizeof(sam_copy));
    memcpy(sag_copy, myp->SAG, sizeof(sag_copy));
    memcpy(&boot_date, &myp->SAMDate[1], sizeof(struct IsDate));
    ReleaseSemaphore(&myp->SEM[18]);

    /* Count online users */
    online_count = 0;
    for (i = 0; i <= (int)myp->HiPort && i < 100; i++) {
        struct PortData *z = myp->PortZ[i];

        if (z && z != myp->z0 && z->OnLine)
            online_count++;
    }

    json_init(&js, stdout);
    json_obj_open(&js);

    /* Counters */
    json_key(&js, "counters");
    json_obj_open(&js);
    json_kv_int(&js, "total_accounts", myp->Nums[NUMS_CURRENT_ACCOUNTS]);
    json_kv_int(&js, "active_accounts", myp->Nums[NUMS_INUSE_ACCOUNTS]);
    json_kv_int(&js, "highest_id", myp->Nums[NUMS_HIGH_ID]);
    json_kv_int(&js, "total_calls", myp->Nums[NUMS_CALLS_TOTAL]);
    json_kv_int(&js, "calls_logged_now", myp->Nums[NUMS_CALLS_LOGGED]);
    json_obj_close(&js);

    /* System info */
    json_key(&js, "system");
    json_obj_open(&js);
    json_kv_int(&js, "subboards", myp->ns);
    json_kv_int(&js, "ports_configured", (long)myp->nPorts);
    json_kv_int(&js, "hi_port", (long)myp->HiPort);
    json_kv_int(&js, "open_pfiles", myp->OpenPfiles);
    json_kv_int(&js, "users_online", online_count);
    json_obj_close(&js);

    /* Boot date */
    if (is_null_date(&boot_date)) {
        json_kv_null(&js, "boot_date");
    } else {
        json_kv_str(&js, "boot_date",
            format_date(datebuf, sizeof(datebuf), &boot_date));
    }

    /* SAM: 5 rows x 15 columns */
    json_key(&js, "sam");
    json_obj_open(&js);
    json_key(&js, "data");
    json_arr_open(&js);
    for (i = 0; i < 5; i++) {
        json_arr_open(&js);
        for (j = 0; j < 15; j++)
            json_int(&js, sam_copy[i][j]);
        json_arr_close(&js);
    }
    json_arr_close(&js);
    json_obj_close(&js);

    /* SAG: 2 rows x 72 columns */
    json_key(&js, "sag");
    json_obj_open(&js);
    json_key(&js, "data");
    json_arr_open(&js);
    for (i = 0; i < 2; i++) {
        json_arr_open(&js);
        for (j = 0; j < 72; j++)
            json_uint(&js, sag_copy[i][j]);
        json_arr_close(&js);
    }
    json_arr_close(&js);
    json_obj_close(&js);

    json_obj_close(&js);
    json_finish(&js);
    return 0;
}
