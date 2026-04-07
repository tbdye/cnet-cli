/*
 * log.h -- Log file commands for cnet-cli
 *
 * System log operations: list, read, callers
 */

#ifndef CNET_CLI_LOG_H
#define CNET_CLI_LOG_H

struct MainPort;

int cmd_log_list(struct MainPort *myp, int argc, char **argv);
int cmd_log_read(struct MainPort *myp, int argc, char **argv);
int cmd_log_callers(struct MainPort *myp, int argc, char **argv);
int cmd_log_callers_parsed(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_LOG_H */
