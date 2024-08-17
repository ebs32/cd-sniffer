// Project
#include "resources.h"

// ESP SDK
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

// C
#include <stdint.h>
#include <string.h>

static const char* module_id = "wifi";

static esp_err_t handle_post_action(httpd_req_t *request) {
  return httpd_resp_send_404(request);
}

static esp_err_t handle_get_resource(httpd_req_t *request) {
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

  httpd_resp_set_type(request, HTTPD_TYPE_TEXT);

  return httpd_resp_send(request, start, end - start - 1);
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
      { .method = HTTP_GET , .uri = "/"      , .handler = handle_get_resource },
      { .method = HTTP_POST, .uri = "/action", .handler = handle_post_action  },
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

void stop_wifi() {
  esp_wifi_stop                ();
  tcpip_adapter_stop           (TCPIP_ADAPTER_IF_AP);
  nvs_flash_deinit             ();
  esp_event_loop_delete_default();
}

int32_t start_wifi() {
  if (set_up_wifi() == ESP_OK) {
    if (set_up_http() != ESP_OK) {
      stop_wifi();

      return -1;
    }
  }

  return 0;
}
