#include "sensor_manager.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define I2C_PORT 0
#define I2C_SDA GPIO_NUM_22
#define I2C_SCL GPIO_NUM_23
#define TCA_ADDR 0x70
static const char *TAG="sensors"; static i2c_master_bus_handle_t bus;static i2c_master_dev_handle_t mux;
static sensor_reading_t readings[8];static SemaphoreHandle_t lock;static volatile bool detect_requested=true;
static esp_err_t tx(i2c_master_dev_handle_t d,const uint8_t *p,size_t n){return i2c_master_transmit(d,p,n,100);}
static esp_err_t txrx(i2c_master_dev_handle_t d,const uint8_t *p,size_t n,uint8_t *r,size_t rn){return i2c_master_transmit_receive(d,p,n,r,rn,100);}
static esp_err_t select_ch(int ch){uint8_t b=1u<<ch;return tx(mux,&b,1);}
static esp_err_t add_dev(uint8_t addr,i2c_master_dev_handle_t *d){i2c_device_config_t c={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=addr,.scl_speed_hz=100000};return i2c_master_bus_add_device(bus,&c,d);}
static uint8_t crc8(const uint8_t *d,int n){uint8_t c=0xff;for(int i=0;i<n;i++){c^=d[i];for(int b=0;b<8;b++)c=(c&0x80)?(c<<1)^0x31:c<<1;}return c;}
static esp_err_t read_sht4x(float *t,float *h,char *serial){i2c_master_dev_handle_t d;esp_err_t e=add_dev(0x44,&d);if(e)return e;uint8_t cmd=0x89,r[6];e=txrx(d,&cmd,1,r,6);if(e==ESP_OK&&crc8(r,2)==r[2]&&crc8(r+3,2)==r[5])snprintf(serial,24,"%02X%02X%02X%02X",r[0],r[1],r[3],r[4]);cmd=0xfd;if(e==ESP_OK)e=tx(d,&cmd,1);if(e==ESP_OK){vTaskDelay(pdMS_TO_TICKS(10));e=i2c_master_receive(d,r,6,100);}i2c_master_bus_rm_device(d);if(e||crc8(r,2)!=r[2]||crc8(r+3,2)!=r[5])return e?e:ESP_ERR_INVALID_CRC;uint16_t rt=(r[0]<<8)|r[1],rh=(r[3]<<8)|r[4];*t=-45+175*(rt/65535.0f);*h=-6+125*(rh/65535.0f);return ESP_OK;}
static esp_err_t read_sht3x(float *t,float *h){i2c_master_dev_handle_t d;esp_err_t e=add_dev(0x44,&d);if(e)return e;uint8_t cmd[2]={0x24,0x00},r[6];e=tx(d,cmd,2);if(e==ESP_OK){vTaskDelay(pdMS_TO_TICKS(16));e=i2c_master_receive(d,r,6,100);}i2c_master_bus_rm_device(d);if(e||crc8(r,2)!=r[2]||crc8(r+3,2)!=r[5])return e?e:ESP_ERR_INVALID_CRC;*t=-45+175*(((r[0]<<8)|r[1])/65535.0f);*h=100*(((r[3]<<8)|r[4])/65535.0f);return ESP_OK;}
static esp_err_t read_sht2x(float *t,float *h){i2c_master_dev_handle_t d;esp_err_t e=add_dev(0x40,&d);if(e)return e;uint8_t c=0xf3,r[3];e=tx(d,&c,1);vTaskDelay(pdMS_TO_TICKS(90));if(e==ESP_OK)e=i2c_master_receive(d,r,3,100);if(e==ESP_OK){uint16_t x=((r[0]<<8)|r[1])&~3;*t=-46.85f+175.72f*x/65536.0f;c=0xf5;e=tx(d,&c,1);vTaskDelay(pdMS_TO_TICKS(30));}if(e==ESP_OK)e=i2c_master_receive(d,r,3,100);i2c_master_bus_rm_device(d);if(e)return e;uint16_t x=((r[0]<<8)|r[1])&~3;*h=-6+125*x/65536.0f;return ESP_OK;}
static sensor_family_t desired(sensor_type_t t){if(t>=SENSOR_SHT45&&t<=SENSOR_SHT40)return FAMILY_SHT4X;if(t>=SENSOR_SHT35&&t<=SENSOR_SHT30)return FAMILY_SHT3X;if(t==SENSOR_SHT20)return FAMILY_SHT2X;if(t==SENSOR_OTHER)return FAMILY_OTHER;return FAMILY_NONE;}
static esp_err_t sample(int ch,sensor_type_t type,sensor_reading_t *o,bool detect){esp_err_t e=select_ch(ch);if(e)return e;sensor_family_t f=desired(type);float t=NAN,h=NAN;char sn[24]={0};if(type==SENSOR_DISABLED)return ESP_ERR_NOT_SUPPORTED;if((detect||type==SENSOR_AUTO)&&read_sht4x(&t,&h,sn)==ESP_OK)f=FAMILY_SHT4X;else if((detect||type==SENSOR_AUTO)&&read_sht3x(&t,&h)==ESP_OK)f=FAMILY_SHT3X;else if((detect||type==SENSOR_AUTO)&&read_sht2x(&t,&h)==ESP_OK)f=FAMILY_SHT2X;else if(f==FAMILY_SHT4X)e=read_sht4x(&t,&h,sn);else if(f==FAMILY_SHT3X)e=read_sht3x(&t,&h);else if(f==FAMILY_SHT2X)e=read_sht2x(&t,&h);else e=ESP_ERR_NOT_FOUND;if(!isnan(t)){e=ESP_OK;o->family=f;o->raw_temperature=t;o->raw_humidity=h;if(sn[0])strcpy(o->serial,sn);}return e;}
static void task(void *arg){while(1){app_config_t cfg;app_config_get(&cfg);bool detect=detect_requested;detect_requested=false;for(int i=0;i<8;i++){sensor_reading_t r; xSemaphoreTake(lock,portMAX_DELAY);r=readings[i];xSemaphoreGive(lock);r.configured=cfg.channels[i].type;esp_err_t e=sample(i,r.configured,&r,detect);r.online=(e==ESP_OK);if(e==ESP_OK){r.temperature=r.raw_temperature+cfg.channels[i].temp_offset;r.humidity=r.raw_humidity+cfg.channels[i].humidity_offset;r.last_read_ms=esp_timer_get_time()/1000;r.sample_count++;}else if(e!=ESP_ERR_NOT_SUPPORTED)r.error_count++;if(r.configured==SENSOR_DISABLED){r.online=false;r.family=FAMILY_NONE;}xSemaphoreTake(lock,portMAX_DELAY);readings[i]=r;xSemaphoreGive(lock);vTaskDelay(pdMS_TO_TICKS(25));}vTaskDelay(pdMS_TO_TICKS(5000));}}
esp_err_t sensor_manager_init(void){lock=xSemaphoreCreateMutex();i2c_master_bus_config_t c={.i2c_port=I2C_PORT,.sda_io_num=I2C_SDA,.scl_io_num=I2C_SCL,.clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};ESP_RETURN_ON_ERROR(i2c_new_master_bus(&c,&bus),TAG,"bus");ESP_RETURN_ON_ERROR(add_dev(TCA_ADDR,&mux),TAG,"mux");xTaskCreate(task,"sensor_poll",6144,NULL,5,NULL);return ESP_OK;}
void sensor_manager_get(int ch,sensor_reading_t *o){xSemaphoreTake(lock,portMAX_DELAY);*o=readings[ch];xSemaphoreGive(lock);}
void sensor_manager_detect(void){detect_requested=true;}
const char *sensor_family_name(sensor_family_t f){static const char*n[]={"None","SHT4x","SHT3x","SHT2x","Other"};return(f>=0&&f<=FAMILY_OTHER)?n[f]:"None";}
