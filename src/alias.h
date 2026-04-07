/*
 * alias.h -- Mail alias commands for cnet-cli
 *
 * Mail alias management: list, add, remove
 */

#ifndef CNET_CLI_ALIAS_H
#define CNET_CLI_ALIAS_H

struct MainPort;

int cmd_mail_alias_list(struct MainPort *myp, int argc, char **argv);
int cmd_mail_alias_add(struct MainPort *myp, int argc, char **argv);
int cmd_mail_alias_remove(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_ALIAS_H */
