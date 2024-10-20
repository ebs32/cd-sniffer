#include "patcher.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

void app_main() {
  run_patcher();

  vTaskDelete(NULL);
}
