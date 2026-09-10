#include "network.h"
#include <string.h>
#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG="network";static bool ap_mode;
static void events(void*a,esp_event_base_t b,int32_t id,void*d){if(b==WIFI_EVENT&&id==WIFI_EVENT_STA_START)esp_wifi_connect();else if(b==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED)esp_wifi_connect();}
esp_err_t network_init(void){app_config_t c;app_config_get(&c);ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());wifi_init_config_t w=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&w));ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,events,NULL));
 if(!c.ssid[0]){ap_mode=true;esp_netif_create_default_wifi_ap();wifi_config_t x={.ap={.channel=1,.max_connection=4,.authmode=WIFI_AUTH_WPA2_PSK}};strcpy((char*)x.ap.ssid,APP_AP_SSID);x.ap.ssid_len=strlen(APP_AP_SSID);strcpy((char*)x.ap.password,APP_AP_PASSWORD);ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&x));ESP_ERROR_CHECK(esp_wifi_start());ESP_LOGI(TAG,"setup AP %s at 192.168.4.1",APP_AP_SSID);return ESP_OK;}
 esp_netif_t*n=esp_netif_create_default_wifi_sta();if(!c.dhcp){esp_netif_dhcpc_stop(n);esp_netif_ip_info_t ip={0};ESP_ERROR_CHECK(esp_netif_str_to_ip4(c.ip,&ip.ip));ESP_ERROR_CHECK(esp_netif_str_to_ip4(c.gateway,&ip.gw));ESP_ERROR_CHECK(esp_netif_str_to_ip4(c.netmask,&ip.netmask));ESP_ERROR_CHECK(esp_netif_set_ip_info(n,&ip));esp_netif_dns_info_t dns={0};dns.ip.type=ESP_IPADDR_TYPE_V4;ESP_ERROR_CHECK(esp_netif_str_to_ip4(c.dns,&dns.ip.u_addr.ip4));ESP_ERROR_CHECK(esp_netif_set_dns_info(n,ESP_NETIF_DNS_MAIN,&dns));}wifi_config_t x={0};strncpy((char*)x.sta.ssid,c.ssid,sizeof(x.sta.ssid));strncpy((char*)x.sta.password,c.password,sizeof(x.sta.password));x.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&x));return esp_wifi_start();}
int network_rssi(void){wifi_ap_record_t a;return !ap_mode&&esp_wifi_sta_get_ap_info(&a)==ESP_OK?a.rssi:0;}
const char *network_mode(void){return ap_mode?"setup-ap":"station";}
