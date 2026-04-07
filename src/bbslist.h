/*
 * bbslist.h -- BBSList commands for cnet-cli
 *
 * BBS directory listing: read-only list
 */

#ifndef CNET_CLI_BBSLIST_H
#define CNET_CLI_BBSLIST_H

struct MainPort;

int cmd_bbslist_list(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_BBSLIST_H */
