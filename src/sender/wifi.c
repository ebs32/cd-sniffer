#include "actions.h"
#include "controller.h"
#include "resources.h"

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

#define STATUS_READ_MS      250 // Time period between status reads

// Timeout waiting on the status to change in ticks
// This can be calculated as (1000 / STATUS_READ_MS) * N_SEC
#define STATUS_TIMEOUT_T      8

#define EVENT_BUFFER_SIZE    16 // Size of the event buffer
#define MAX_COMMAND_LENGTH  512 // Maximum number of commands to read

static const char* module_id = "wifi";

static TEvent events[EVENT_BUFFER_SIZE];
static size_t read_idx  = 0;
static size_t write_idx = 0;

static void handle_ctl_update(const TEvent* event) {
  events[write_idx].is_busy     = event->is_busy;
  events[write_idx].is_powered  = event->is_powered;
  events[write_idx].status_text = event->status_text;

  if (++write_idx == EVENT_BUFFER_SIZE) {
    write_idx = 0;
  }
}

static esp_err_t handle_get_resource(httpd_req_t* request) {
  const char *start;
  const char *end;

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

static esp_err_t handle_get_actions(httpd_req_t* request) {
  char   buffer[256 + 1];
  size_t length = sizeof(actions) / sizeof(TAction);

  httpd_resp_set_type(request, HTTPD_TYPE_JSON);

  // To save some memory at the cost of speed send a chunked response containing
  // all the actions available
  httpd_resp_send_chunk(request, "[", 1);

  for (size_t i = 0; i < length; i++) {
    sprintf(buffer,
      "{\"i\":\"%c\",\"d\":\"%s\"}", actions[i].id, actions[i].description
    );

    httpd_resp_send_chunk(request, buffer, -1);

    if (i + 1 < length) {
      httpd_resp_send_chunk(request, ",", 1);
    }
  }

  httpd_resp_send_chunk(request, "]", 1);

  return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t handle_get_status(httpd_req_t* request) {
  char   buffer[256 + 1];
  size_t idx;
  int    socket_fd;

  // Wait for the status to change or the timeout to expire or the client to
  // abort the request, whichever occurs first
  socket_fd = httpd_req_to_sockfd(request);

  for (size_t i = 0; read_idx == write_idx && i < STATUS_TIMEOUT_T; i++) {
    // If this call returns 0 then there is no bytes pending to be to read,
    // which means the client, potentially, closed the socket on its end
    if (recv(socket_fd, NULL, 0, MSG_DONTWAIT) == 0) {
      return ESP_FAIL;
    }

    vTaskDelay(STATUS_READ_MS / portTICK_RATE_MS);
  }

  // Send the controller status to the client, which may have changed or not;
  // but in any case send it as well as the corresponding friendly description
  // and the flag indicating whether the controller is busy or not

  if(read_idx == write_idx) {
    idx = (read_idx == 0 ? EVENT_BUFFER_SIZE : read_idx) - 1;
  } else {
    idx = read_idx;

    if (++read_idx == EVENT_BUFFER_SIZE) {
      read_idx = 0;
    }
  }

  sprintf(
    buffer,
    "[{\"s\":%d,\"t\":\"%s\",\"b\":%d}]",
    events[idx].is_powered,
    events[idx].status_text,
    events[idx].is_busy
  );

  httpd_resp_set_type(request, HTTPD_TYPE_JSON);

  return httpd_resp_send(request, buffer, -1);
}

static esp_err_t handle_post_action(httpd_req_t* request) {
  char   buffer[64 + 1];
  size_t buffer_size = sizeof(buffer) / sizeof(char);

  // If the request is valid then execute the requested action and return
  // the 200/OK response immediately
  if (
    httpd_req_get_url_query_str(request, buffer, buffer_size) == ESP_OK &&
    httpd_query_key_value(buffer, "a", buffer, buffer_size)   == ESP_OK
  ) {
    for (size_t i = 0; i < sizeof(actions) / sizeof(TAction); i++) {
      if (buffer[0] == actions[i].id) {
        actions[i].fn();

        return httpd_resp_send(request, NULL, 0);
      }
    }
  }

  // Otherwise, send a 400/Bad Request response
  httpd_resp_set_status(request, HTTPD_400);

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
  ctl_run_micom_commands(length, (uint16_t*) buffer);

  return httpd_resp_send(request, NULL, 0);
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
      { .method = HTTP_GET , .uri = "/"        , .handler = handle_get_resource  },
      { .method = HTTP_GET , .uri = "/actions" , .handler = handle_get_actions   },
      { .method = HTTP_GET , .uri = "/status"  , .handler = handle_get_status    },
      { .method = HTTP_POST, .uri = "/action"  , .handler = handle_post_action   },
      { .method = HTTP_POST, .uri = "/commands", .handler = handle_post_commands },
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
    if (set_up_http() != ESP_OK || ctl_add_listener(handle_ctl_update) != 0) {
      wifi_stop();

      return -1;
    }
  }

  return 0;
}
