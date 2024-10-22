#include "common.h"
#include "resources.h"

// ESP8266
#include "esp8266/spi_struct.h"
#include "esp8266/gpio_struct.h"
#include "esp8266/timer_struct.h"
#include "driver/gpio.h"

// ESP SDK
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

// C
#include <sys/socket.h>
#include <stdint.h>
#include <string.h>

#define MAX_COMMAND_LENGTH 512 // Maximum number of commands to read

extern uint16_t tracking_balance;
extern uint16_t tracking_gain;

static const char* module_id = "wifi";

static void postCommand(uint16_t command) {
  // Enable the command phase
  SPI1.user.usr_command         = 1;

  // Set the command and the length
  SPI1.user2.usr_command_value  = command;
  SPI1.user2.usr_command_bitlen = command <= 0xFF
                                ? 7
                                : command <= 0xFFF
                                ? 11
                                : 15;

  // Start the operation
  SPI1.cmd.usr                  = 1;

  while (SPI1.cmd.usr == 1);

  // Trigger the latch signal - At this point the minimum time required for
  // enabling the latch signal has lapsed

  SET_LO(XLT_CONTROLLER);
  DELAY (40);

  SET_HI(XLT_CONTROLLER);
}

static esp_err_t handle_get_resource(httpd_req_t* request) {
  const char* start;
  const char* end;

  if (
    strcmp(request->uri, "/") == 0)
  {
    start = index_html_start;
    end   = index_html_end;
  } else {
    return httpd_resp_send_404(request);
  }

  return httpd_resp_send(request, start, end - start - 1);
}

static esp_err_t handle_get_tracking_values(httpd_req_t* request) {
  char       buffer[256 + 1];
  nvs_handle handle;
  uint16_t   tracking_balance = 0;
  uint16_t   tracking_gain    = 0;

  if (nvs_open("tracking", NVS_READONLY, &handle) == ESP_OK) {
    nvs_get_u16(handle, "balance", &tracking_balance);
    nvs_get_u16(handle, "gain"   , &tracking_gain   );
    nvs_close  (handle);
  }

  sprintf(buffer,
    "{\"balance\":%d,\"gain\":%d}",
    tracking_balance,
    tracking_gain
  );

  httpd_resp_set_type(request, HTTPD_TYPE_JSON);

  return httpd_resp_send(request, buffer, -1);
}

static esp_err_t handle_post_tracking_values(httpd_req_t* request) {
  uint16_t   size = 2 * sizeof(uint16_t);
  char       buffer[size];
  nvs_handle handle;

  for (int m = 0, s; m < size; m += s) {
    s = httpd_req_recv(request, &buffer[m], size - m);

    if (s <= 0) {
      httpd_resp_set_status(request, HTTPD_500);

      return httpd_resp_send(request, NULL, 0);
    }
  }

  printf("tracking balance = 0x%03x\n", ((uint16_t*) buffer)[0]);
  printf("tracking gain    = 0x%03x\n", ((uint16_t*) buffer)[1]);

  if (nvs_open("tracking", NVS_READWRITE, &handle) == ESP_OK) {
    if (
      nvs_set_u16(handle, "balance", ((uint16_t*) buffer)[0]) != ESP_OK ||
      nvs_set_u16(handle, "gain"   , ((uint16_t*) buffer)[1]) != ESP_OK ||
      nvs_commit (handle)                                     != ESP_OK
    ) {
      httpd_resp_set_status(request, HTTPD_500);
    } else {
      if (tracking_balance != 0) {
        tracking_balance = ((uint16_t*) buffer)[0];

        postCommand(tracking_balance);
      }

      if (tracking_gain != 0) {
        tracking_gain    = ((uint16_t*) buffer)[1];

        postCommand(tracking_gain);
      }
    }

    nvs_close(handle);
  } else {
    httpd_resp_set_status(request, HTTPD_500);
  }

  return httpd_resp_send(request, NULL, 0);
}

static esp_err_t handle_post_commands(httpd_req_t* request) {
  uint16_t length;
  uint16_t size;
  char*    buffer;

  // The first two bytes indicate the number of commands to read
  if (httpd_req_recv(request, (char*) &length, sizeof(uint16_t)) != sizeof(uint16_t)) {
    httpd_resp_set_status (request, HTTPD_500);

    return httpd_resp_send(request, NULL, 0);
  }

  if (length > MAX_COMMAND_LENGTH) {
    httpd_resp_set_status (request, HTTPD_400);

    return httpd_resp_send(request, NULL, 0);
  }

  // Read the commands
  size   = length * sizeof(int16_t);
  buffer = (char*) malloc(size);

  if (buffer == NULL) {
    httpd_resp_set_status (request, HTTPD_500);

    return httpd_resp_send(request, NULL, 0);
  }

  for (int m = 0, s; m < size; m += s) {
    s = httpd_req_recv(request, &buffer[m], size - m);

    if (s <= 0) {
      httpd_resp_set_status (request, HTTPD_500);

      return httpd_resp_send(request, NULL, 0);
    }
  }

  // Run the commands - The buffer will be freed by the controller API
  for (size_t i = 0; i < length; i++) {
    postCommand(((uint16_t*) buffer)[i]);

    vTaskDelay(100 / portTICK_RATE_MS);
  }

  free(buffer);

  return httpd_resp_send(request, NULL, 0);
}

static esp_err_t handle_post_restart(httpd_req_t* request) {
  httpd_resp_send(request, NULL, 0);
  esp_restart();

  return ESP_OK;
}

static esp_err_t set_up_wifi() {
  esp_err_t               status;

  wifi_init_config_t      wifi_configuration    = WIFI_INIT_CONFIG_DEFAULT();
  tcpip_adapter_ip_info_t ap_dhcp_configuration = {
    .ip       = { .addr = PP_HTONL(0xAC100101) }, // 172. 16.  1.  1
    .gw       = { .addr = PP_HTONL(0xAC100101) }, // 172. 16.  1.  1
    .netmask  = { .addr = PP_HTONL(0xFFFFFFFC) }  // 255.255.255.252
  };

  tcpip_adapter_init();

  if (
    // Create the default event loop required for WiFi service
    (status = esp_event_loop_create_default())                != ESP_OK ||

    // Initialize the storage system, so it's available to other services
    (status = nvs_flash_init())                               != ESP_OK ||

    // Update the DHCP configuration for the AP interface
    (status = tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP))  != ESP_OK ||
    (status = tcpip_adapter_set_ip_info(
                TCPIP_ADAPTER_IF_AP,
                &ap_dhcp_configuration))                      != ESP_OK ||
    (status = tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP)) != ESP_OK ||

    // Start the WiFi service
    (status = esp_wifi_init(&wifi_configuration))             != ESP_OK ||
    (status = esp_wifi_set_mode(WIFI_MODE_AP))                != ESP_OK ||
    (status = esp_wifi_start())                               != ESP_OK
  ) {
    ESP_LOGE(module_id,
      "Failed to set up WiFi service with error code: %d", status
    );
  }

  return status;
}

static esp_err_t set_up_http() {
  esp_err_t      status;

  httpd_config_t httpd_configuration = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t http_server         = NULL;

  if ((status = httpd_start(&http_server, &httpd_configuration)) != ESP_OK) {
    ESP_LOGE(module_id,
      "Failed to start the HTTP server with error code: %d", status
    );
  } else { // Register URI handlers
    const httpd_uri_t handlers[] = {
      { .method = HTTP_GET , .uri = "/"               , .handler = handle_get_resource         },
      { .method = HTTP_GET , .uri = "/tracking-values", .handler = handle_get_tracking_values  },
      { .method = HTTP_POST, .uri = "/tracking-values", .handler = handle_post_tracking_values },
      { .method = HTTP_POST, .uri = "/commands"       , .handler = handle_post_commands        },
      { .method = HTTP_POST, .uri = "/restart"        , .handler = handle_post_restart         },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(httpd_uri_t); i++) {
      status = httpd_register_uri_handler(http_server, &handlers[i]);

      if (status != ESP_OK) {
        ESP_LOGE(module_id,
          "Failed to register URI handler with error code: %d", status
        );

        break;
      }
    }

    if (status != ESP_OK) {
      httpd_stop(http_server);
    }
  }

  if (status != ESP_OK) {
    ESP_LOGE(module_id,
      "Failed to set up HTTP service with error code: %d", status
    );
  }

  return status;
}

void wifi_stop() {
  esp_wifi_stop                ();
  tcpip_adapter_stop           (TCPIP_ADAPTER_IF_AP);
  nvs_flash_deinit             ();
  esp_event_loop_delete_default();
}

int32_t wifi_start() {
  if (set_up_wifi() == ESP_OK) {
    if (set_up_http() != ESP_OK) {
      wifi_stop();

      return -1;
    }
  }

  return 0;
}
