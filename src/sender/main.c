#include "sender.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "freertos/task.h"

void app_main() {
  run_sender();

  vTaskDelete(NULL);
}
