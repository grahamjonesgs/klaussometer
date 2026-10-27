#ifndef WEB_SERVER_H
#define WEB_SERVER_H

// Local web interface: board info, log viewer, reboot and firmware upload.

// Registers all HTTP endpoints and starts the server. Call once from setup().
void setup_web_server();

// Serves pending requests. Call from loop().
void web_server_handle();

#endif // WEB_SERVER_H
