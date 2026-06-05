#include "esp8266_at.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern DMA_HandleTypeDef hdma_usart1_rx;

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

static volatile int g_wifi_connected = 0;
static volatile int g_mqtt_connected = 0;
static volatile int g_wifi_state = ESP_WIFI_STATE_IDLE;

enum { ESP_DMA_RX_SIZE = 1024 };
static uint8_t esp_dma_rx[ESP_DMA_RX_SIZE];
static volatile uint16_t esp_dma_last_pos = 0;
// stored connection parameters for reconnect attempts
static char stored_accessToken[64] = {0};
static char stored_projectKey[64] = {0};
static char stored_host[128] = {0};
static char stored_port[16] = {0};
static char stored_topic[64] = {0};

/* ---- helper: print a debug line to USART2 ---- */
static void debug_print(const char *msg)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)msg, (uint16_t)strlen(msg), 1000);
}

/* ---- helper: print AT command being sent ---- */
static void debug_cmd(const char *cmd)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n[CMD] ", 8, 1000);
    if (cmd) HAL_UART_Transmit(&huart2, (uint8_t *)cmd, (uint16_t)strlen(cmd), 1000);
    HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 2, 1000);
}

int esp_get_wifi_state(void)
{
    return g_wifi_state;
}

int esp_send_cmd_wait(const char *cmd, char *resp, int resp_len, uint32_t timeout_ms)
{
    char sendbuf[256];
    uint32_t start = HAL_GetTick();
    int idx = 0;

    if (cmd != NULL)
    {
        debug_cmd(cmd);
        snprintf(sendbuf, sizeof(sendbuf), "%s\r\n", cmd);
        HAL_UART_Transmit(&huart1, (uint8_t *)sendbuf, (uint16_t)strlen(sendbuf), 1000);
    }

    if (resp == NULL || resp_len <= 0)
    {
        return 0;
    }

    // If DMA not started, try to start on the fly
    if (hdma_usart1_rx.State == HAL_DMA_STATE_RESET)
    {
        HAL_UART_Receive_DMA(&huart1, esp_dma_rx, ESP_DMA_RX_SIZE);
    }

    while ((HAL_GetTick() - start) < timeout_ms && idx < resp_len - 1)
    {
        uint16_t pos = (uint16_t)(ESP_DMA_RX_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx));
        while (esp_dma_last_pos != pos && idx < resp_len - 1)
        {
            uint8_t c = esp_dma_rx[esp_dma_last_pos];
            resp[idx++] = (char)c;
            // echo to debug uart
            HAL_UART_Transmit(&huart2, &c, 1, 100);
            esp_dma_last_pos++;
            if (esp_dma_last_pos >= ESP_DMA_RX_SIZE) esp_dma_last_pos = 0;
        }
        if (idx > 0)
        {
            resp[idx] = '\0';
            if (strstr(resp, "WIFI CONNECTED") != NULL ||
                strstr(resp, "WIFI_CONNECTED") != NULL ||
                strstr(resp, "WIFI GOT IP") != NULL)
            {
                g_wifi_connected = 1;
                g_wifi_state = ESP_WIFI_STATE_CONNECTED;
                debug_print("\r\n[DEBUG] WiFi connected URC detected\r\n");
            }
            // Some ESP8266 firmware sends SmartConfig-specific URCs
            // that indicate success even before "WIFI CONNECTED"
            if (strstr(resp, "CWSTARTSMART:SUCCESS") != NULL ||
                strstr(resp, "CWSTARTSMART:CONNECTED") != NULL ||
                strstr(resp, "smartconfig connected") != NULL ||
                strstr(resp, "SmartConfig connected") != NULL)
            {
                g_wifi_connected = 1;
                g_wifi_state = ESP_WIFI_STATE_CONNECTED;
                debug_print("\r\n[DEBUG] SmartConfig success URC detected\r\n");
            }
            if (strstr(resp, "WIFI DISCONNECT") != NULL ||
                strstr(resp, "WIFI DISCONNECT\r\n") != NULL ||
                strstr(resp, "WIFI DISCONNECTED") != NULL)
            {
                g_wifi_connected = 0;
                debug_print("\r\n[DEBUG] WiFi disconnect URC detected\r\n");
            }
            if (strstr(resp, "+MQTTCONNECTED") != NULL ||
                strstr(resp, "+MQTTCONNECTED\r\n") != NULL ||
                strstr(resp, "MQTT CONNECTED") != NULL)
            {
                g_mqtt_connected = 1;
                debug_print("\r\n[DEBUG] MQTT connected URC detected\r\n");
            }
            if (strstr(resp, "+MQTTDISCONNECTED") != NULL ||
                strstr(resp, "MQTT DISCONNECT") != NULL)
            {
                g_mqtt_connected = 0;
                debug_print("\r\n[DEBUG] MQTT disconnect URC detected\r\n");
            }
            // fast exit if OK or ERROR or prompt seen
            if (strstr(resp, "OK") != NULL || strstr(resp, "ERROR") != NULL || strstr(resp, ">") != NULL)
            {
                break;
            }
        }
        else
        {
            HAL_Delay(5);
        }
    }
    resp[idx] = '\0';
    return idx;
}

/* ---- Passive wait for "WIFI CONNECTED" URC ---- */
int esp_wait_for_wifi_connected(uint32_t timeout_ms)
{
    char buf[256];
    uint32_t start = HAL_GetTick();

    debug_print("[DEBUG] Waiting for WIFI CONNECTED...\r\n");

    // Ensure DMA is running
    if (hdma_usart1_rx.State == HAL_DMA_STATE_RESET)
    {
        HAL_UART_Receive_DMA(&huart1, esp_dma_rx, ESP_DMA_RX_SIZE);
        esp_dma_last_pos = 0;
    }

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        // Read whatever is available (passive, no command sent)
        esp_send_cmd_wait(NULL, buf, sizeof(buf), 500);

        if (g_wifi_connected)
        {
            debug_print("[DEBUG] WIFI CONNECTED detected!\r\n");
            return 0;
        }
    }

    debug_print("[DEBUG] Timeout waiting for WIFI CONNECTED\r\n");
    return -1;
}

/* ---- Try to connect using saved WiFi credentials ---- */
int esp_try_connect_saved_wifi(uint32_t timeout_ms)
{
    char resp[256];

    debug_print("[DEBUG] Trying to connect with saved WiFi...\r\n");
    g_wifi_state = ESP_WIFI_STATE_TRYING_SAVED;

    // Set station mode
    esp_send_cmd_wait("AT+CWMODE=1", resp, sizeof(resp), 2000);

    // Enable auto-connect (ESP8266 will try saved AP)
    esp_send_cmd_wait("AT+CWAUTOCONN=1", resp, sizeof(resp), 2000);

    // Wait for the module to auto-connect
    return esp_wait_for_wifi_connected(timeout_ms);
}

/* ---- Start smartconfig (ESP-TOUCH + AirKiss), timeout_ms wait ---- */
int esp_start_smartconfig_ex(uint32_t timeout_ms)
{
    char resp[512];

    debug_print("[DEBUG] Starting SmartConfig (ESP-TOUCH + AirKiss)...\r\n");
    g_wifi_state = ESP_WIFI_STATE_SMARTCONFIG;
    g_wifi_connected = 0;  // reset flag before starting

    // Ensure DMA rx is running to capture URCs
    if (hdma_usart1_rx.State == HAL_DMA_STATE_RESET)
    {
        HAL_UART_Receive_DMA(&huart1, esp_dma_rx, ESP_DMA_RX_SIZE);
        esp_dma_last_pos = 0;
    }

    // Set station mode
    esp_send_cmd_wait("AT+CWMODE=1", resp, sizeof(resp), 2000);

    // Enable auto-connect (for future boots)
    esp_send_cmd_wait("AT+CWAUTOCONN=1", resp, sizeof(resp), 2000);

    // Start smartconfig (ESP-TOUCH + AirKiss)
    esp_send_cmd_wait("AT+CWSTARTSMART=3", resp, sizeof(resp), 3000);

    debug_print("[DEBUG] SmartConfig started. Use phone App to configure WiFi.\r\n");

    // Wait for "WIFI CONNECTED" or smartconfig success URC
    int result = esp_wait_for_wifi_connected(timeout_ms);

    // Always stop smartconfig after wait (success or timeout)
    debug_print("[DEBUG] Stopping SmartConfig...\r\n");
    esp_send_cmd_wait("AT+CWSTOPSMART", resp, sizeof(resp), 3000);

    if (result == 0)
    {
        // SmartConfig succeeded in connecting WiFi, but AT+CWSTOPSMART
        // may have caused a brief disconnect. Wait for reconnection.
        debug_print("[DEBUG] SmartConfig SUCCESS - WiFi was connected!\r\n");

        if (!g_wifi_connected)
        {
            debug_print("[DEBUG] WiFi disconnected after CWSTOPSMART, waiting reconnect...\r\n");
            // ESP8266 will auto-reconnect using SmartConfig credentials
            // because AT+CWAUTOCONN=1 was set. Wait up to 30s.
            if (esp_wait_for_wifi_connected(30000) == 0)
            {
                debug_print("[DEBUG] WiFi reconnected OK after CWSTOPSMART\r\n");
            }
            else
            {
                debug_print("[DEBUG] WiFi reconnection timeout, may need explicit connect\r\n");
                result = -1;
            }
        }

        if (g_wifi_connected)
        {
            g_wifi_state = ESP_WIFI_STATE_CONNECTED;
            // Query and log the connected AP info (SSID is saved in ESP8266 flash)
            esp_send_cmd_wait("AT+CWJAP?", resp, sizeof(resp), 3000);
            debug_print("[DEBUG] Current AP info: ");
            debug_print(resp);
        }
    }
    else
    {
        debug_print("[DEBUG] SmartConfig FAILED - timeout\r\n");
        g_wifi_state = ESP_WIFI_STATE_FAILED;
    }

    return (g_wifi_connected ? 0 : -1);
}

/* Backward-compatible wrapper */
int esp_start_smartconfig(void)
{
    return esp_start_smartconfig_ex(60000);
}

/* ---- High-level WiFi auto-connect orchestrator ---- */
int esp_wifi_auto_connect(void)
{
    int result;

    debug_print("\r\n[DEBUG] ===== WiFi Auto-Connect Start =====\r\n");

    // Phase 1: try saved WiFi (20 s timeout)
    result = esp_try_connect_saved_wifi(20000);
    if (result == 0)
    {
        debug_print("[DEBUG] Phase 1 OK - connected with saved WiFi\r\n");
        return 0;
    }

    debug_print("[DEBUG] Phase 1 failed - starting SmartConfig fallback...\r\n");

    // Phase 2: smartconfig (120 s timeout)
    result = esp_start_smartconfig_ex(120000);
    if (result == 0)
    {
        debug_print("[DEBUG] Phase 2 OK - SmartConfig succeeded\r\n");
        return 0;
    }

    debug_print("[DEBUG] ===== WiFi Auto-Connect FAILED =====\r\n");
    g_wifi_state = ESP_WIFI_STATE_FAILED;
    return -1;
}

/* ---- Legacy: explicit SSID/password connect (kept for backward compatibility) ---- */
int esp_init_and_connect_wifi(const char *ssid, const char *password)
{
    char resp[512];
    char cmd[256];

    debug_print("[DEBUG] Connecting with explicit SSID/password...\r\n");
    g_wifi_state = ESP_WIFI_STATE_TRYING_SAVED;

    esp_send_cmd_wait("ATE0", resp, sizeof(resp), 1000);
    esp_send_cmd_wait("AT", resp, sizeof(resp), 2000);
    esp_send_cmd_wait("AT+CWMODE=1", resp, sizeof(resp), 2000);
    // snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);
    // esp_send_cmd_wait(cmd, resp, sizeof(resp), 30000);

    // Wait for connection
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick()-t0) < 10000)
    {
        if (g_wifi_connected)
        {
            g_wifi_state = ESP_WIFI_STATE_CONNECTED;
            break;
        }
        HAL_Delay(200);
    }
    return g_wifi_connected ? 0 : -1;
}

void esp_start_dma_rx(void)
{
    // start circular DMA reception into esp_dma_rx
    HAL_UART_Receive_DMA(&huart1, esp_dma_rx, ESP_DMA_RX_SIZE);
    esp_dma_last_pos = 0;
}

int esp_mqtt_usercfg_and_connect(const char *accessToken, const char *projectKey, const char *host, const char *port, const char *topic)
{
    char cmd[512];
    char resp[1024];

    debug_print("[DEBUG] Configuring MQTT and connecting...\r\n");

    // store parameters for future reconnect attempts
    if (accessToken) strncpy(stored_accessToken, accessToken, sizeof(stored_accessToken)-1);
    if (projectKey) strncpy(stored_projectKey, projectKey, sizeof(stored_projectKey)-1);
    if (host) strncpy(stored_host, host, sizeof(stored_host)-1);
    if (port) strncpy(stored_port, port, sizeof(stored_port)-1);
    if (topic) strncpy(stored_topic, topic, sizeof(stored_topic)-1);

    // Configure MQTT user (index 0)
    // Format: AT+MQTTUSERCFG=0,1,"topic","AccessToken","ProjectKey",0,0,""
    snprintf(cmd, sizeof(cmd), "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"", topic, accessToken, projectKey);
    esp_send_cmd_wait(cmd, resp, sizeof(resp), 2000);

    // Connect to MQTT broker
    snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=0,\"%s\",%s,1", host, port);
    esp_send_cmd_wait(cmd, resp, sizeof(resp), 20000);

    // give more time for mqtt connected token (up to 20s)
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick()-t0) < 20000)
    {
        if (g_mqtt_connected) break;
        HAL_Delay(200);
    }

    if (g_mqtt_connected)
        debug_print("[DEBUG] MQTT connected OK\r\n");
    else
        debug_print("[DEBUG] MQTT connect may have failed\r\n");

    return 0;
}

int esp_mqtt_publish_temperature(const char *topic, float temperature)
{
    char payload[128];
    char cmd[256];
    char resp[512];
    // JSON payload without using %f to avoid missing float support in minimal printf
    int t10 = (int)(temperature * 10.0f + (temperature >= 0 ? 0.5f : -0.5f));
    int t_int = t10 / 10;
    int t_frac = abs(t10 % 10);
    // payload JSON must escape inner double-quotes for AT command
    // example: AT+MQTTPUB=0,"attributes","{\"temperature\":29}",0,0
    snprintf(payload, sizeof(payload), "{\\\"%s\\\":%d.%d}", "temperature", t_int, t_frac);
    snprintf(cmd, sizeof(cmd), "AT+MQTTPUB=0,\"%s\",\"%s\",0,0", topic, payload);

    // Ensure MQTT is connected before publishing. If not, attempt reconnect.
    if (!g_mqtt_connected)
    {
        debug_print("[DEBUG] MQTT not connected, attempting reconnect...\r\n");
        // try reconnect using stored parameters
        esp_mqtt_usercfg_and_connect(stored_accessToken, stored_projectKey, stored_host, stored_port, stored_topic);
        // wait briefly for URC
        uint32_t t0 = HAL_GetTick();
        while ((HAL_GetTick()-t0) < 15000)
        {
            if (g_mqtt_connected) break;
            HAL_Delay(200);
        }
    }

    // Try publish with a couple retries if transient ERROR occurs
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        int len = esp_send_cmd_wait(cmd, resp, sizeof(resp), 5000);
        if (len > 0 && strstr(resp, "ERROR") != NULL)
        {
            // publish failed
            HAL_Delay(200);
            continue;
        }
        break;
    }

    return 0;
}

int esp_mqtt_publish_attribute(const char *topic, const char *key, int value)
{
    char payload[128];
    char cmd[256];
    char resp[512];

    if (topic == NULL || key == NULL) return -1;

    snprintf(payload, sizeof(payload), "{\\\"%s\\\":%d}", key, value);
    snprintf(cmd, sizeof(cmd), "AT+MQTTPUB=0,\"%s\",\"%s\",0,0", topic, payload);

    // Ensure MQTT connected; try reconnect with stored params if needed
    if (!g_mqtt_connected)
    {
        debug_print("[DEBUG] MQTT not connected, attempting reconnect...\r\n");
        esp_mqtt_usercfg_and_connect(stored_accessToken, stored_projectKey, stored_host, stored_port, stored_topic);
        uint32_t t0 = HAL_GetTick();
        while ((HAL_GetTick()-t0) < 15000)
        {
            if (g_mqtt_connected) break;
            HAL_Delay(200);
        }
    }

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        int len = esp_send_cmd_wait(cmd, resp, sizeof(resp), 5000);
        if (len > 0 && strstr(resp, "ERROR") != NULL)
        {
            HAL_Delay(200);
            continue;
        }
        break;
    }

    return 0;
}

int esp_is_wifi_connected(void)
{
    return g_wifi_connected;
}

int esp_is_mqtt_connected(void)
{
    return g_mqtt_connected;
}

/* ---- Reconnect logic: uses saved WiFi (no need for explicit ssid/password) ---- */
void esp_check_and_reconnect(const char *ssid, const char *password,
                             const char *accessToken, const char *projectKey,
                             const char *host, const char *port, const char *topic)
{
    static uint32_t last_wifi_attempt = 0;
    static uint32_t last_mqtt_attempt = 0;

    uint32_t now = HAL_GetTick();
    if (!g_wifi_connected && (now - last_wifi_attempt) > 5000)
    {
        last_wifi_attempt = now;
        debug_print("[DEBUG] Reconnect: trying saved WiFi...\r\n");
        // Try saved WiFi first with a shorter timeout (15 s)
        if (esp_try_connect_saved_wifi(15000) != 0)
        {
            // If saved WiFi fails after 15s, try with explicit credentials if provided
            if (ssid && ssid[0] && password)
            {
                debug_print("[DEBUG] Reconnect: falling back to explicit SSID...\r\n");
                //esp_init_and_connect_wifi(ssid, password);
            }
        }
    }
    if (g_wifi_connected && !g_mqtt_connected && (now - last_mqtt_attempt) > 5000)
    {
        last_mqtt_attempt = now;
        debug_print("[DEBUG] Reconnect: reconnecting MQTT...\r\n");
        esp_mqtt_usercfg_and_connect(accessToken, projectKey, host, port, topic);
    }
}

int esp_mqtt_subscribe(const char *topic)
{
    char cmd[256];
    char resp[512];
    if (topic == NULL) return -1;
    snprintf(cmd, sizeof(cmd), "AT+MQTTSUB=0,\"%s\",1", topic);
    esp_send_cmd_wait(cmd, resp, sizeof(resp), 5000);
    return 0;
}

int esp_mqtt_poll(char *topic_out, int topic_out_len, char *payload_out, int payload_out_len)
{
    char resp[1024];
    int len = esp_send_cmd_wait(NULL, resp, sizeof(resp), 200);
    if (len <= 0)
    {
        return 0;
    }

    // Try to find a topic like command/send/...
    const char *p_topic = strstr(resp, "command/send/");
    if (p_topic == NULL)
    {
        // fallback: look for any quoted topic pattern after +MQTTSUBRECV or +MQTTPUB
        p_topic = strstr(resp, ",\"");
        if (p_topic) p_topic += 2; // move after ,"
    }

    if (p_topic && topic_out && topic_out_len > 0)
    {
        int i = 0;
        const char *q = p_topic;
        while (*q != '\0' && *q != '"' && *q != '\r' && *q != '\n' && *q != ',' && i < topic_out_len-1)
        {
            topic_out[i++] = *q++;
        }
        topic_out[i] = '\0';
    }

    // find JSON payload between first '{' and the matching '}'
    const char *p = strchr(resp, '{');
    const char *q = NULL;
    if (p) q = strchr(p, '}');
    if (p && q && payload_out && payload_out_len > 0)
    {
        int plen = (int)(q - p + 1);
        if (plen >= payload_out_len) plen = payload_out_len - 1;
        memcpy(payload_out, p, plen);
        payload_out[plen] = '\0';
        return 1;
    }

    return 0;
}
