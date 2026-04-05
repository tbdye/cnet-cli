/*
 * group.h -- Access group commands for cnet-cli
 *
 * Access group operations: list, show, edit, transpose
 */

#ifndef CNET_CLI_GROUP_H
#define CNET_CLI_GROUP_H

struct MainPort;

int cmd_group_list(struct MainPort *myp, int argc, char **argv);
int cmd_group_show(struct MainPort *myp, int argc, char **argv);
int cmd_group_edit(struct MainPort *myp, int argc, char **argv);
int cmd_group_transpose(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_GROUP_H */
