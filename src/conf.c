/*
 * conf.c -- Conference room commands for cnet-cli
 *
 * Conference room listing. Reads CRoom[100] under SEM[8] shared lock.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include <cnet/cnet.h>
#undef __asm

#include <proto/exec.h>

#include "conf.h"
#include "json.h"
#include "util.h"

extern struct Library *CNetBase;

int cmd_conf_list(struct MainPort *myp, int argc, char **argv)
{
    struct json_state js;
    int i;
    int show_all = 0;
    char buf[128];

    /* Parse flags */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0)
            show_all = 1;
    }

    ObtainSemaphoreShared(&myp->SEM[8]);

    json_init(&js, stdout);
    json_obj_open(&js);
    json_key(&js, "rooms");
    json_arr_open(&js);

    for (i = 0; i < 100; i++) {
        struct Room *room = myp->CRoom[i];
        if (!room)
            continue;
        /* Skip inactive rooms unless --all */
        if (!show_all && room->Users == 0 && !room->rc.PermaRoom)
            continue;

        json_obj_open(&js);
        json_kv_int(&js, "room_number", (long)i);
        json_kv_str(&js, "name",
            strip_mci(buf, sizeof(buf), room->rc.Name));
        json_kv_str(&js, "topic",
            strip_mci(buf, sizeof(buf), room->rc.Topic));
        json_kv_int(&js, "creator", (long)room->rc.Creator);
        json_kv_int(&js, "users", room->Users);
        json_kv_bool(&js, "public", (int)room->rc.Public);
        json_kv_bool(&js, "quiet", (int)room->rc.Quiet);
        json_kv_bool(&js, "permanent", (int)room->rc.PermaRoom);
        json_kv_int(&js, "max_users", (long)room->rc.MaxUsers);
        json_kv_int(&js, "channel", (long)room->rc.Channel);
        json_obj_close(&js);
    }

    json_arr_close(&js);
    json_obj_close(&js);
    json_finish(&js);

    ReleaseSemaphore(&myp->SEM[8]);
    return 0;
}
