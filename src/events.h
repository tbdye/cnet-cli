/*
 * events.h -- Event commands for cnet-cli
 *
 * Scheduled event operations: list, show
 */

#ifndef CNET_CLI_EVENTS_H
#define CNET_CLI_EVENTS_H

struct MainPort;

int cmd_event_list(struct MainPort *myp, int argc, char **argv);
int cmd_event_show(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_EVENTS_H */
