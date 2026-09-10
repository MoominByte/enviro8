#include "app_config.h"
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static app_config_t s_cfg;
static SemaphoreHandle_t s_lock;

static void defaults(app_config_t *c) {
    memset(c, 0, sizeof(*c)); c->dhcp = true;
    strcpy(c->ip,"192.168.1.50"); strcpy(c->gateway,"192.168.1.1");
    strcpy(c->netmask,"255.255.255.0"); strcpy(c->dns,"8.8.8.8");
    for (int i=0;i<APP_CHANNELS;i++) c->channels[i].type = SENSOR_AUTO;
}
esp_err_t app_config_init(void) {
    esp_err_t e=nvs_flash_init();
    if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase());e=nvs_flash_init();}
    if (e != ESP_OK) return e;
    s_lock=xSemaphoreCreateMutex(); defaults(&s_cfg);
    nvs_handle_t h=0; if(nvs_open("app",NVS_READONLY,&h)==ESP_OK){size_t n=sizeof(s_cfg);e=nvs_get_blob(h,"config",&s_cfg,&n);nvs_close(h);if(e!=ESP_OK||n!=sizeof(s_cfg)){defaults(&s_cfg);}}
    return ESP_OK;
}
void app_config_get(app_config_t *out){xSemaphoreTake(s_lock,portMAX_DELAY);*out=s_cfg;xSemaphoreGive(s_lock);}
esp_err_t app_config_save(const app_config_t *cfg){nvs_handle_t h=0;esp_err_t e=nvs_open("app",NVS_READWRITE,&h);if(e==ESP_OK)e=nvs_set_blob(h,"config",cfg,sizeof(*cfg));if(e==ESP_OK)e=nvs_commit(h);if(h)nvs_close(h);if(e==ESP_OK){xSemaphoreTake(s_lock,portMAX_DELAY);s_cfg=*cfg;xSemaphoreGive(s_lock);}return e;}
esp_err_t app_config_factory_reset(void){nvs_handle_t h=0;esp_err_t e=nvs_open("app",NVS_READWRITE,&h);if(e==ESP_OK)e=nvs_erase_all(h);if(e==ESP_OK)e=nvs_commit(h);if(h)nvs_close(h);xSemaphoreTake(s_lock,portMAX_DELAY);defaults(&s_cfg);xSemaphoreGive(s_lock);return e;}
static const char *names[]={"Disabled","Auto","SHT45","SHT41","SHT40","SHT35","SHT31","SHT30","SHT20","Other"};
const char *sensor_type_name(sensor_type_t t){return (t>=0&&t<=SENSOR_OTHER)?names[t]:"Other";}
sensor_type_t sensor_type_parse(const char *s){for(int i=0;i<=SENSOR_OTHER;i++)if(!strcasecmp(s,names[i]))return i;return SENSOR_OTHER;}
