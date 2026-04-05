/*
 * gfile.c -- GFile (General Text File) commands for cnet-cli
 *
 * GFile operations: list, read, add, remove
 *
 * GFile areas use the same MRK_TEXT_DOOR (3) marker and identical
 * data format as news. Each wrapper validates arguments with
 * gfile-specific error messages, then delegates to the corresponding
 * news handler.
 */

#include "gfile.h"
#include "news.h"
#include "util.h"

int cmd_gfile_list(struct MainPort *myp, int argc, char **argv)
{
    if (argc < 2) {
        json_error("Usage: cnet-cli gfile list <sub-id|gokey> "
            "[--limit N] [--offset N]");
        return 1;
    }
    return cmd_news_list(myp, argc, argv);
}

int cmd_gfile_read(struct MainPort *myp, int argc, char **argv)
{
    if (argc < 3) {
        json_error("Usage: cnet-cli gfile read <sub-id|gokey> "
            "<item-number>");
        return 1;
    }
    return cmd_news_read(myp, argc, argv);
}

int cmd_gfile_add(struct MainPort *myp, int argc, char **argv)
{
    if (argc < 2) {
        json_error("Usage: cnet-cli gfile add <sub-id|gokey> "
            "--title \"...\" --author <account> --text \"...\"");
        return 1;
    }
    return cmd_news_post(myp, argc, argv);
}

int cmd_gfile_remove(struct MainPort *myp, int argc, char **argv)
{
    if (argc < 3) {
        json_error("Usage: cnet-cli gfile remove <sub-id|gokey> "
            "<item-number>");
        return 1;
    }
    return cmd_news_delete(myp, argc, argv);
}
