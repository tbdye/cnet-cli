/*
 * user.h -- User management commands for cnet-cli
 *
 * Phase 6: user list, show, find, edit, disable, enable; who detail; OLM
 */

#ifndef CNET_CLI_USER_H
#define CNET_CLI_USER_H

struct MainPort;

/* User commands (Phase 6) */
int cmd_user_list(struct MainPort *myp, int argc, char **argv);
int cmd_user_show(struct MainPort *myp, int argc, char **argv);
int cmd_user_find(struct MainPort *myp, int argc, char **argv);
int cmd_user_edit(struct MainPort *myp, int argc, char **argv);
int cmd_user_disable(struct MainPort *myp, int argc, char **argv);
int cmd_user_enable(struct MainPort *myp, int argc, char **argv);

/* User profile command (Phase 13) */
int cmd_user_profile(struct MainPort *myp, int argc, char **argv);

/* User delete command (Phase 15) */
int cmd_user_delete(struct MainPort *myp, int argc, char **argv);

/* Extended who command (Phase 6) */
int cmd_who_detail(struct MainPort *myp, int argc, char **argv);

/* OLM command (Phase 6) */
int cmd_olm_send(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_USER_H */
