#include "app_config.h"
#include "network.h"
#include "sensor_manager.h"
#include "web_server.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define USER_BUTTON GPIO_NUM_9
static void button_task(void*a){gpio_config_t c={.pin_bit_mask=1ULL<<USER_BUTTON,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};gpio_config(&c);while(1){if(!gpio_get_level(USER_BUTTON)){int n=0;while(!gpio_get_level(USER_BUTTON)&&n<100){vTaskDelay(pdMS_TO_TICKS(100));n++;}if(n>=100){app_config_factory_reset();esp_restart();}}vTaskDelay(pdMS_TO_TICKS(100));}}
void app_main(void){ESP_ERROR_CHECK(app_config_init());ESP_ERROR_CHECK(network_init());ESP_ERROR_CHECK(sensor_manager_init());ESP_ERROR_CHECK(web_server_start());xTaskCreate(button_task,"factory_button",2048,NULL,4,NULL);ESP_LOGI("app","Enviro8 %s ready",APP_VERSION);}
