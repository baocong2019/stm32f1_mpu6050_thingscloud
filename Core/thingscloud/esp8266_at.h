/* Minimal ESP8266 AT helper for MQTT via UART1 */
#ifndef __ESP8266_AT_H
#define __ESP8266_AT_H

#include "main.h"

int esp_init_and_connect_wifi(const char *ssid, const char *password);
int esp_mqtt_usercfg_and_connect(const char *accessToken, const char *projectKey, const char *host, const char *port, const char *topic);
int esp_mqtt_publish_temperature(const char *topic, float temperature);
int esp_send_cmd_wait(const char *cmd, char *resp, int resp_len, uint32_t timeout_ms);
int esp_is_wifi_connected(void);
int esp_is_mqtt_connected(void);
void esp_check_and_reconnect(const char *ssid, const char *password, const char *accessToken, const char *projectKey, const char *host, const char *port, const char *topic);
void esp_start_dma_rx(void);

#endif
