#include "sniffer.h"

// ESP SDK
#include "esp_attr.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

void IRAM_ATTR app_main() {
  run_sniffer();

  vTaskDelete(NULL);
}
