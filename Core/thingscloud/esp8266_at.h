/* Minimal ESP8266 AT helper for MQTT via UART1 */
#ifndef __ESP8266_AT_H
#define __ESP8266_AT_H

#include "main.h"

/* ---- WiFi connection states (for OLED display) ---- */
#define ESP_WIFI_STATE_IDLE           0  /* idle / not started */
#define ESP_WIFI_STATE_TRYING_SAVED   1  /* trying to connect with saved WiFi */
#define ESP_WIFI_STATE_TRYING_DEFAULT 2  /* trying to connect with hardcoded default WiFi */ //中文：尝试连接硬编码默认WiFi
#define ESP_WIFI_STATE_SMARTCONFIG    3  /* smartconfig in progress */
#define ESP_WIFI_STATE_CONNECTED      4  /* WiFi connected */
#define ESP_WIFI_STATE_FAILED         5  /* all attempts failed */
#define ESP_WIFI_STATE_GOT_CRED       6  /* smartconfig got credentials, trying to connect */


extern volatile int g_wifi_connected;
extern volatile int g_wifi_state;

int esp_get_wifi_state(void);  /* get current WiFi connection state for OLED */

/* 获取SmartConfig接收到的WiFi凭证（SSID和密码），用于显式重连 */
int esp_get_smartconfig_credentials(char *ssid_out, int ssid_len,
                                    char *pwd_out, int pwd_len);

/* ---- Core WiFi functions ---- */

/* Wait passively for "WIFI CONNECTED" URC without sending any command.
   Returns 0 on success (g_wifi_connected set), -1 on timeout. */
int esp_wait_for_wifi_connected(uint32_t timeout_ms);

/* Try to connect using ESP8266's internally saved WiFi credentials.
   Sends AT+CWMODE=1 + AT+CWAUTOCONN=1, then waits for connection.
   Returns 0 on success, -1 on timeout. */
int esp_try_connect_saved_wifi(uint32_t timeout_ms);

/* High-level WiFi auto-connect orchestrator:
   1. Try saved WiFi for 15 s
   2. If that fails, try default SSID/password for 30 s
   3. If that fails, start smartconfig and wait forever until success
   Updates g_wifi_connected and the WiFi-state flag.
   Returns 0 if WiFi is connected (always returns 0 — Phase 3 never gives up). */
int esp_wifi_auto_connect(const char *default_ssid, const char *default_password);

/* Explicitly connect with SSID/password using AT+CWJAP.
   Also sets AT+CWAUTOCONN=1 so credentials are saved for future boots.
   Returns 0 on success, -1 on timeout. */
int esp_init_and_connect_wifi(const char *ssid, const char *password);

/* Start smartconfig (ESP-TOUCH + AirKiss).  Does NOT call AT+RESTORE,
   so previously-saved credentials are preserved.
   Waits up to timeout_ms for "WIFI CONNECTED" then stops smartconfig.
   Returns 0 on success, -1 on failure. */
int esp_start_smartconfig_ex(uint32_t timeout_ms);

/* SmartConfig无限循环版：反复启动SmartConfig直到WiFi连接成功。
   每次尝试120秒，失败后自动重试，永不返回失败。
   适用于"不配网不罢休"的场景。 */
int esp_start_smartconfig_forever(void);

/* Backward-compatible: calls esp_start_smartconfig_ex(60000). */
int esp_start_smartconfig(void);

/* ---- MQTT functions ---- */
int esp_mqtt_usercfg_and_connect(const char *accessToken, const char *projectKey, const char *host, const char *port, const char *topic);
int esp_mqtt_publish_temperature(const char *topic, float temperature);
int esp_mqtt_publish_attribute(const char *topic, const char *key, int value);
int esp_send_cmd_wait(const char *cmd, char *resp, int resp_len, uint32_t timeout_ms);
int esp_is_wifi_connected(void);
int esp_is_mqtt_connected(void);
void esp_check_and_reconnect(const char *ssid, const char *password, const char *accessToken, const char *projectKey, const char *host, const char *port, const char *topic);
void esp_start_dma_rx(void);

/* Subscribe to a topic (supports MQTT wildcards) */
int esp_mqtt_subscribe(const char *topic);

/* Poll incoming MQTT/AT URC messages; if a publish to any topic is found,
   returns 1 and fills topic_out and payload_out (null-terminated). */
int esp_mqtt_poll(char *topic_out, int topic_out_len, char *payload_out, int payload_out_len);

#endif
