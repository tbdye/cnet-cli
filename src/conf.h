/*
 * conf.h -- Conference room commands for cnet-cli
 *
 * Conference room operations: list
 */

#ifndef CNET_CLI_CONF_H
#define CNET_CLI_CONF_H

struct MainPort;

int cmd_conf_list(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_CONF_H */
