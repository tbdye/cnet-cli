/*
 * group.h -- Access group commands for cnet-cli
 *
 * Phase 10: group list, group show
 */

#ifndef CNET_CLI_GROUP_H
#define CNET_CLI_GROUP_H

struct MainPort;

int cmd_group_list(struct MainPort *myp, int argc, char **argv);
int cmd_group_show(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_GROUP_H */
