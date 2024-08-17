#include "sniffer.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

void app_main() {
  run_sniffer();

  vTaskDelete(NULL);
}
