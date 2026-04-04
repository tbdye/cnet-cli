/*
 * log.h -- Log file commands for cnet-cli
 *
 * Phase 10: log list, log read, log callers
 */

#ifndef CNET_CLI_LOG_H
#define CNET_CLI_LOG_H

struct MainPort;

int cmd_log_list(struct MainPort *myp, int argc, char **argv);
int cmd_log_read(struct MainPort *myp, int argc, char **argv);
int cmd_log_callers(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_LOG_H */
