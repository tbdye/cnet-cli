/*
 * user.h -- User management commands for cnet-cli
 *
 * User operations: list, show, find, edit, disable, enable, delete, profile; who detail; OLM
 */

#ifndef CNET_CLI_USER_H
#define CNET_CLI_USER_H

struct MainPort;

/* User commands */
int cmd_user_list(struct MainPort *myp, int argc, char **argv);
int cmd_user_show(struct MainPort *myp, int argc, char **argv);
int cmd_user_find(struct MainPort *myp, int argc, char **argv);
int cmd_user_edit(struct MainPort *myp, int argc, char **argv);
int cmd_user_disable(struct MainPort *myp, int argc, char **argv);
int cmd_user_enable(struct MainPort *myp, int argc, char **argv);

/* User profile command */
int cmd_user_profile(struct MainPort *myp, int argc, char **argv);

/* User plan file command */
int cmd_user_plan(struct MainPort *myp, int argc, char **argv);

/* User delete command */
int cmd_user_delete(struct MainPort *myp, int argc, char **argv);

/* Extended who command */
int cmd_who_detail(struct MainPort *myp, int argc, char **argv);

/* OLM command */
int cmd_olm_send(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_USER_H */
