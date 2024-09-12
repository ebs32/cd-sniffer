#pragma once

#include <stdint.h>

/**
 * Starts the WiFi service in AP mode and the HTTP server.
 *
 * The default IP for the AP is 172.16.1.1/30 so only one client is allowed to
 * connect to the AP.
 *
 * @returns 0, on success; -1, on error.
 */
int32_t wifi_start();

/**
 * Stops the WiFi service and the HTTP server.
 */
void wifi_stop();
