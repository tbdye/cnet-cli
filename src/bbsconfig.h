/*
 * bbsconfig.h -- BBS configuration commands for cnet-cli
 *
 * BBS configuration display
 *
 * Named bbsconfig.h (not config.h) to avoid collision with
 * cnet-sdk's cnet/config.h which is included transitively.
 */

#ifndef CNET_CLI_BBSCONFIG_H
#define CNET_CLI_BBSCONFIG_H

struct MainPort;

int cmd_config_show(struct MainPort *myp, int argc, char **argv);
int cmd_config_flags(struct MainPort *myp, int argc, char **argv);
int cmd_config_reload_text(struct MainPort *myp, int argc, char **argv);
int cmd_config_port(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_BBSCONFIG_H */
