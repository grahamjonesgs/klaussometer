#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "types.h"
#include <WebServer.h>

// Local web interface: board info, log viewer, reboot and firmware upload.
// Served from loop() via webServer.handleClient().
extern WebServer webServer;

// Registers all HTTP endpoints and starts the server. Call once from setup().
void setup_web_server();

#endif // WEB_SERVER_H
