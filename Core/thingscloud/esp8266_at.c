#include "esp8266_at.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern DMA_HandleTypeDef hdma_usart1_rx;

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

static volatile int g_wifi_connected = 0;
static volatile int g_mqtt_connected = 0;

enum { ESP_DMA_RX_SIZE = 1024 };
static uint8_t esp_dma_rx[ESP_DMA_RX_SIZE];
static volatile uint16_t esp_dma_last_pos = 0;

int esp_send_cmd_wait(const char *cmd, char *resp, int resp_len, uint32_t timeout_ms)
{
    char sendbuf[256];
    uint32_t start = HAL_GetTick();
    int idx = 0;

    if (cmd != NULL)
    {
        snprintf(sendbuf, sizeof(sendbuf), "%s\r\n", cmd);
        HAL_UART_Transmit(&huart1, (uint8_t *)sendbuf, (uint16_t)strlen(sendbuf), 1000);
        HAL_UART_Transmit(&huart2, (uint8_t *)sendbuf, (uint16_t)strlen(sendbuf), 1000); // echo to debug
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
            // update connection flags based on received tokens
            if (strstr(resp, "WIFI CONNECTED") != NULL || strstr(resp, "WIFI CONNECTED\r\n") != NULL || strstr(resp, "WIFI_CONNECTED") != NULL)
            {
                g_wifi_connected = 1;
            }
            if (strstr(resp, "WIFI DISCONNECT") != NULL || strstr(resp, "WIFI DISCONNECT\r\n") != NULL || strstr(resp, "WIFI DISCONNECTED") != NULL)
            {
                g_wifi_connected = 0;
            }
            if (strstr(resp, "+MQTTCONNECTED") != NULL || strstr(resp, "+MQTTCONNECTED\r\n") != NULL || strstr(resp, "MQTT CONNECTED") != NULL)
            {
                g_mqtt_connected = 1;
            }
            if (strstr(resp, "+MQTTDISCONNECTED") != NULL || strstr(resp, "MQTT DISCONNECT") != NULL || strstr(resp, "ERROR") != NULL)
            {
                g_mqtt_connected = 0;
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

int esp_init_and_connect_wifi(const char *ssid, const char *password)
{
    char resp[512];
    // Disable echo to simplify parsing
    esp_send_cmd_wait("ATE0", resp, sizeof(resp), 1000);
    // Test
    esp_send_cmd_wait("AT", resp, sizeof(resp), 2000);

    // Set station mode
    snprintf(resp, sizeof(resp), "AT+CWMODE=1");
    esp_send_cmd_wait(resp, resp, sizeof(resp), 2000);

    // Join AP
    snprintf(resp, sizeof(resp), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);
    esp_send_cmd_wait(resp, resp, sizeof(resp), 30000);

    // wait up to 10s for WIFI CONNECTED token
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick()-t0) < 10000)
    {
        if (g_wifi_connected) break;
        HAL_Delay(200);
    }

    return 0;
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

    // Configure MQTT user (index 0)
    // Format: AT+MQTTUSERCFG=0,1,"topic","AccessToken","ProjectKey",0,0,""
    snprintf(cmd, sizeof(cmd), "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"", topic, accessToken, projectKey);
    esp_send_cmd_wait(cmd, resp, sizeof(resp), 2000);

    // Connect to MQTT broker
    snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=0,\"%s\",%s,1", host, port);
    esp_send_cmd_wait(cmd, resp, sizeof(resp), 15000);

    // give some time for mqtt connected token
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick()-t0) < 5000)
    {
        if (g_mqtt_connected) break;
        HAL_Delay(100);
    }

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
    snprintf(payload, sizeof(payload), "{\"%s\":%d.%d}", "temperature", t_int, t_frac);
    // AT+MQTTPUB=0,"attributes","{\"temperature\":29}",0,0
    snprintf(cmd, sizeof(cmd), "AT+MQTTPUB=0,\"%s\",\"%s\",0,0", topic, payload);
    esp_send_cmd_wait(cmd, resp, sizeof(resp), 5000);

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

void esp_check_and_reconnect(const char *ssid, const char *password, const char *accessToken, const char *projectKey, const char *host, const char *port, const char *topic)
{
    static uint32_t last_wifi_attempt = 0;
    static uint32_t last_mqtt_attempt = 0;

    uint32_t now = HAL_GetTick();
    if (!g_wifi_connected && (now - last_wifi_attempt) > 5000)
    {
        last_wifi_attempt = now;
        esp_init_and_connect_wifi(ssid, password);
    }
    if (g_wifi_connected && !g_mqtt_connected && (now - last_mqtt_attempt) > 5000)
    {
        last_mqtt_attempt = now;
        esp_mqtt_usercfg_and_connect(accessToken, projectKey, host, port, topic);
    }
}
