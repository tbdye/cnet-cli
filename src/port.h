/*
 * port.h -- Port management commands for cnet-cli
 *
 * Phase 11: port load, port unload, port dump
 */

#ifndef CNET_CLI_PORT_H
#define CNET_CLI_PORT_H

struct MainPort;

int cmd_port_load(struct MainPort *myp, int argc, char **argv);
int cmd_port_unload(struct MainPort *myp, int argc, char **argv);
int cmd_port_dump(struct MainPort *myp, int argc, char **argv);

#endif /* CNET_CLI_PORT_H */
