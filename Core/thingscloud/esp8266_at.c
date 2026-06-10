#include "esp8266_at.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern DMA_HandleTypeDef hdma_usart1_rx;

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

volatile int g_wifi_connected = 0;
static volatile int g_mqtt_connected = 0;
volatile int g_wifi_state = ESP_WIFI_STATE_IDLE;

/* SmartConfig凭证相关：当SmartConfig通过手机配网成功接收到WiFi凭证时，
   同时保存SSID和密码，以便在自动连接失败时进行显式AT+CWJAP重连 */
static volatile int g_smartconfig_got_credentials = 0;   // SmartConfig成功接收到WiFi凭证标志
static char g_smartconfig_ssid[33] = {0};                // 保存SmartConfig收到的SSID（最多32字符）
static char g_smartconfig_password[65] = {0};            // 保存SmartConfig收到的密码（最多64字符）

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

/* 获取SmartConfig接收到的WiFi凭证（SSID和密码）
   返回0表示有有效凭证，返回-1表示尚无凭证
   调用者可以用这些凭证进行显式AT+CWJAP连接 */
int esp_get_smartconfig_credentials(char *ssid_out, int ssid_len,
                                    char *pwd_out, int pwd_len)
{
    if (!g_smartconfig_got_credentials || g_smartconfig_ssid[0] == '\0')
    {
        return -1;  // 没有有效的SmartConfig凭证
    }

    if (ssid_out && ssid_len > 0)
    {
        strncpy(ssid_out, g_smartconfig_ssid, ssid_len - 1);
        ssid_out[ssid_len - 1] = '\0';
    }

    if (pwd_out && pwd_len > 0)
    {
        strncpy(pwd_out, g_smartconfig_password, pwd_len - 1);
        pwd_out[pwd_len - 1] = '\0';
    }

    return 0;
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

            /* 检测SmartConfig成功接收到WiFi凭证（但WiFi可能尚未连接）
               部分ESP8266固件输出"Smart get wifi info"表示已通过手机配网获取到SSID和密码
               此时标记凭证已收到，后续流程会等待WiFi连接或进行显式重连 */
            if (strstr(resp, "Smart get wifi info") != NULL ||
                strstr(resp, "smart get wifi info") != NULL)
            {
                g_smartconfig_got_credentials = 1;
                g_wifi_state = ESP_WIFI_STATE_GOT_CRED;  // 更新状态为"已获取凭证"
                debug_print("\r\n[DEBUG] SmartConfig credentials received! (Smart get wifi info)\r\n");
            }

            /* 部分ESP8266固件还会输出"smartconfig type:"作为凭证接收的URC，也作为备选检测 */
            if (strstr(resp, "smartconfig type:") != NULL)
            {
                g_smartconfig_got_credentials = 1;
                if (g_wifi_state != ESP_WIFI_STATE_CONNECTED)
                {
                    g_wifi_state = ESP_WIFI_STATE_GOT_CRED;
                }
                debug_print("\r\n[DEBUG] SmartConfig type URC detected\r\n");
            }

            /* 解析SmartConfig接收到的SSID：从"ssid:XXX"格式的URC中提取WiFi名称
               例如URC输出 "ssid:CCB\r\n" → 解析出 "CCB" */
            {
                const char *ssid_marker = strstr(resp, "ssid:");
                if (ssid_marker != NULL)
                {
                    ssid_marker += 5; // 跳过"ssid:"这5个字符
                    int i = 0;
                    while (*ssid_marker != '\0' && *ssid_marker != '\r' && *ssid_marker != '\n'
                           && *ssid_marker != ' ' && i < (int)sizeof(g_smartconfig_ssid)-1)
                    {
                        g_smartconfig_ssid[i++] = *ssid_marker++;
                    }
                    g_smartconfig_ssid[i] = '\0';
                    debug_print("[DEBUG] SmartConfig SSID parsed: ");
                    debug_print(g_smartconfig_ssid);
                    debug_print("\r\n");
                }
            }

            /* 解析SmartConfig接收到的密码：从"password:XXX"格式的URC中提取WiFi密码
               例如URC输出 "password:yilou101\r\n" → 解析出 "yilou101" */
            {
                const char *pwd_marker = strstr(resp, "password:");
                if (pwd_marker != NULL)
                {
                    pwd_marker += 9; // 跳过"password:"这9个字符
                    int i = 0;
                    while (*pwd_marker != '\0' && *pwd_marker != '\r' && *pwd_marker != '\n'
                           && *pwd_marker != ' ' && i < (int)sizeof(g_smartconfig_password)-1)
                    {
                        g_smartconfig_password[i++] = *pwd_marker++;
                    }
                    g_smartconfig_password[i] = '\0';
                    debug_print("[DEBUG] SmartConfig password parsed (len=");
                    char lenbuf[8];
                    snprintf(lenbuf, sizeof(lenbuf), "%d", i);
                    debug_print(lenbuf);
                    debug_print(")\r\n");
                }
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

/* ---- Passive wait for "WIFI CONNECTED" URC ----
   不发送任何AT命令，纯被动监听DMA缓冲区
   调用前会自动同步DMA读指针到当前时刻，丢弃旧的启动数据
   只等待从此刻开始新到达的WIFI CONNECTED URC */
int esp_wait_for_wifi_connected(uint32_t timeout_ms)
{
    char buf[256];
    uint32_t start;

    debug_print("[DEBUG] Waiting for WIFI CONNECTED...\r\n");

    // 确保DMA在运行
    if (hdma_usart1_rx.State == HAL_DMA_STATE_RESET)
    {
        HAL_UART_Receive_DMA(&huart1, esp_dma_rx, ESP_DMA_RX_SIZE);
        esp_dma_last_pos = 0;
    }

    // 先检查：WiFi是否已经在上一次URC检测中连接了？
    if (g_wifi_connected)
    {
        debug_print("[DEBUG] WiFi already connected (flag set)\r\n");
        return 0;
    }

    // 同步DMA读指针到当前写入位置，丢弃启动阶段的旧数据
    // 避免读取早已被环形缓冲区覆盖的过时数据
    // 从此刻开始只监听新到达的URC
    esp_dma_last_pos = (uint16_t)(ESP_DMA_RX_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx));
    debug_print("[DEBUG] DMA read pointer synced, listening for new URC...\r\n");

    start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        // 被动读取URC（不发送任何AT命令）
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

/* ---- Start smartconfig (ESP-TOUCH + AirKiss), timeout_ms wait ----
   改进后的SmartConfig流程：
   1. 启动SmartConfig配网
   2. 等待WiFi连接或凭证接收
      - 如果收到"WIFI CONNECTED" → 立即成功
      - 如果收到"Smart get wifi info" → 凭证已获取，继续等待WiFi自然连接（不设硬限制）
   3. 超时后停止SmartConfig
   4. 如果仍未连接但有凭证 → 等待30秒让ESP8266自动连接
      - 成功 → 返回0
      - 失败 → 返回-1（凭证保留在全局变量，由forever循环用AT+CWJAP重试）
   5. 如果无凭证 → 返回-1 */
int esp_start_smartconfig_ex(uint32_t timeout_ms)
{
    char resp[512];
    int had_credentials = 0;        // 本次配网是否已收到过凭证

    debug_print("[DEBUG] Starting SmartConfig (ESP-TOUCH + AirKiss)...\r\n");
    g_wifi_state = ESP_WIFI_STATE_SMARTCONFIG;
    g_wifi_connected = 0;               // 重置WiFi连接标志
    g_smartconfig_got_credentials = 0;  // 重置SmartConfig凭证接收标志
    memset(g_smartconfig_ssid, 0, sizeof(g_smartconfig_ssid));          // 清空之前保存的SSID
    memset(g_smartconfig_password, 0, sizeof(g_smartconfig_password)); // 清空之前保存的密码

    // Ensure DMA rx is running to capture URCs
    if (hdma_usart1_rx.State == HAL_DMA_STATE_RESET)
    {
        HAL_UART_Receive_DMA(&huart1, esp_dma_rx, ESP_DMA_RX_SIZE);
        esp_dma_last_pos = 0;
    }

    // Set station mode
    esp_send_cmd_wait("AT+CWMODE=1", resp, sizeof(resp), 2000);

    // Enable auto-connect (for future boots)
    // ESP8266会保存SmartConfig收到的WiFi凭证，下次上电自动连接
    esp_send_cmd_wait("AT+CWAUTOCONN=1", resp, sizeof(resp), 2000);

    // Start smartconfig (ESP-TOUCH + AirKiss)
    esp_send_cmd_wait("AT+CWSTARTSMART=3", resp, sizeof(resp), 3000);

    debug_print("[DEBUG] SmartConfig started. Use phone App to configure WiFi.\r\n");

    /* ===== 等待WiFi连接 =====
       被动监听URC，等待ESP8266自己完成WiFi连接
       - WiFi连接成功 → 立即跳出
       - SmartConfig收到凭证 → 记录日志，继续等待（不设硬限制，让ESP8266自然完成连接）
       - 总超时 → 退出等待
       不再提前调用CWSTOPSMART打断ESP8266的连接过程 */
    {
        char buf[256];

        // 确保DMA在运行
        if (hdma_usart1_rx.State == HAL_DMA_STATE_RESET)
        {
            HAL_UART_Receive_DMA(&huart1, esp_dma_rx, ESP_DMA_RX_SIZE);
            esp_dma_last_pos = 0;
        }

        uint32_t start = HAL_GetTick();
        while ((HAL_GetTick() - start) < timeout_ms)
        {
            // 被动读取URC（每次最多等500ms）
            esp_send_cmd_wait(NULL, buf, sizeof(buf), 500);

            // 条件1：WiFi已连接 → 直接跳出等待
            if (g_wifi_connected)
            {
                debug_print("[DEBUG] WIFI CONNECTED detected!\r\n");
                break;
            }

            // 条件2：SmartConfig首次收到WiFi凭证，记录日志
            // 不设硬限制，让ESP8266自然完成WiFi连接过程
            if (g_smartconfig_got_credentials && !had_credentials)
            {
                had_credentials = 1;
                debug_print("[DEBUG] SmartConfig got credentials! Waiting for WiFi connection...\r\n");
            }
        }
    }

    // 无论成功还是超时，都要停止SmartConfig退出配网模式
    debug_print("[DEBUG] Stopping SmartConfig...\r\n");
    esp_send_cmd_wait("AT+CWSTOPSMART", resp, sizeof(resp), 3000);

    /* ===== 根据等待结果处理 ===== */

    if (g_wifi_connected)
    {
        // 情况1：WiFi已成功连接
        debug_print("[DEBUG] SmartConfig SUCCESS - WiFi connected!\r\n");

        // AT+CWSTOPSMART可能导致短暂断开，检查一下
        if (!g_wifi_connected)
        {
            debug_print("[DEBUG] WiFi disconnected after CWSTOPSMART, waiting reconnect...\r\n");
            // ESP8266已通过SmartConfig获得凭证并保存，会自动重连
            if (esp_wait_for_wifi_connected(30000) == 0)
            {
                debug_print("[DEBUG] WiFi reconnected OK after CWSTOPSMART\r\n");
            }
            else
            {
                debug_print("[DEBUG] WiFi reconnection timeout after CWSTOPSMART\r\n");
            }
        }

        if (g_wifi_connected)
        {
            g_wifi_state = ESP_WIFI_STATE_CONNECTED;
            // 查询当前连接的AP信息（SSID已保存在ESP8266 Flash中）
            esp_send_cmd_wait("AT+CWJAP?", resp, sizeof(resp), 3000);
            debug_print("[DEBUG] Current AP info: ");
            debug_print(resp);
        }
    }
    else if (g_smartconfig_got_credentials && g_smartconfig_ssid[0] != '\0')
    {
        // 情况2：SmartConfig成功收到了WiFi凭证，但WiFi未能自动连接
        // 不发送AT+CWJAP主动连接，而是等待ESP8266使用新凭证自动连接
        // CWSTOPSMART后SmartConfig模式退出，ESP8266会用刚收到的凭证尝试连接
        debug_print("[DEBUG] SmartConfig got credentials, waiting for auto-connect after CWSTOPSMART...\r\n");
        debug_print("[DEBUG] SSID: ");
        debug_print(g_smartconfig_ssid);
        debug_print("\r\n");

        // 等待CWSTOPSMART完全生效，然后ESP8266自动连接
        // 给30秒让ESP8266用SmartConfig收到的凭证连接
        if (esp_wait_for_wifi_connected(30000) == 0)
        {
            g_wifi_state = ESP_WIFI_STATE_CONNECTED;
            debug_print("[DEBUG] SmartConfig auto-connect SUCCEEDED!\r\n");
        }
        else
        {
            // 自动连接失败（密码错误或信号问题），返回失败让forever循环重试SmartConfig
            debug_print("[DEBUG] SmartConfig auto-connect FAILED, will retry SmartConfig\r\n");
            g_wifi_state = ESP_WIFI_STATE_FAILED;
        }
    }
    else
    {
        // 情况3：SmartConfig完全超时，未收到任何凭证
        debug_print("[DEBUG] SmartConfig FAILED - no credentials received within timeout\r\n");
        g_wifi_state = ESP_WIFI_STATE_FAILED;
    }

    return (g_wifi_connected ? 0 : -1);
}

/* Backward-compatible wrapper */
int esp_start_smartconfig(void)
{
    return esp_start_smartconfig_ex(60000);
}

/* SmartConfig无限循环版：反复尝试直到WiFi连接成功，永不返回失败
   适用于"不配网不罢休"的场景（Phase 2）

   智能重试策略（每次循环）：
   1. 优先检查：如果之前SmartConfig已成功收到SSID和密码但WiFi未连上
      → 直接用AT+CWJAP显式连接，无需重新走SmartConfig
      → 成功 → 返回0
      → 失败（密码错误等）→ 清零凭证，回退到SmartConfig
   2. 走SmartConfig配网（50秒超时）
      → 收到凭证后不打断，让ESP8266自然完成连接
      → 成功 → 返回0，ESP8266自动保存凭证到Flash
      → 失败但收到凭证 → 凭证保留，下次循环走步骤1
      → 失败且无凭证 → 下次循环继续步骤2

   防死锁措施：每5次失败后发送AT+RST重置ESP8266模块 */
int esp_start_smartconfig_forever(void)
{
    int result;
    int attempt = 0;
    char buf[16];
    char resp[256];

    debug_print("[DEBUG] SmartConfig FOREVER mode — will retry until success\r\n");

    do {
        attempt++;
        snprintf(buf, sizeof(buf), "%d", attempt);
        debug_print("[DEBUG] === SmartConfig attempt #");
        debug_print(buf);
        debug_print(" ===\r\n");

        /* ===== 优先使用已有凭证：如果之前SmartConfig已成功收到SSID和密码，
           但WiFi未能自动连接，则直接用AT+CWJAP显式连接，无需重新走SmartConfig
           避免反复CWSTARTSMART/CWSTOPSMART增加不确定性 */
        if (g_smartconfig_got_credentials && g_smartconfig_ssid[0] != '\0')
        {
            debug_print("[DEBUG] Using saved SmartConfig credentials for AT+CWJAP...\r\n");
            debug_print("[DEBUG] SSID: ");
            debug_print(g_smartconfig_ssid);
            debug_print("\r\n");

            g_wifi_state = ESP_WIFI_STATE_TRYING_SAVED;

            // 确保DMA在运行
            if (hdma_usart1_rx.State == HAL_DMA_STATE_RESET)
            {
                HAL_UART_Receive_DMA(&huart1, esp_dma_rx, ESP_DMA_RX_SIZE);
                esp_dma_last_pos = 0;
            }

            // 设置Station模式 + 显式连接
            esp_send_cmd_wait("AT+CWMODE=1", resp, sizeof(resp), 2000);
            esp_send_cmd_wait("AT+CWAUTOCONN=1", resp, sizeof(resp), 2000);

            char cwjap_cmd[256];
            snprintf(cwjap_cmd, sizeof(cwjap_cmd), "AT+CWJAP=\"%s\",\"%s\"",
                     g_smartconfig_ssid, g_smartconfig_password);
            esp_send_cmd_wait(cwjap_cmd, resp, sizeof(resp), 15000);

            // 等待WiFi连接URC
            if (esp_wait_for_wifi_connected(15000) == 0)
            {
                debug_print("[DEBUG] AT+CWJAP with SmartConfig credentials SUCCESS!\r\n");
                g_wifi_state = ESP_WIFI_STATE_CONNECTED;
                return 0;
            }

            // AT+CWJAP也失败了 → 凭证可能无效（密码错误/信号问题）
            // 清零凭证，回退到SmartConfig重新配网
            debug_print("[DEBUG] AT+CWJAP failed, clearing credentials and restarting SmartConfig...\r\n");
            g_smartconfig_got_credentials = 0;
            memset(g_smartconfig_ssid, 0, sizeof(g_smartconfig_ssid));
            memset(g_smartconfig_password, 0, sizeof(g_smartconfig_password));
        }

        /* 防死锁：每5次失败尝试后，通过AT+RST硬重置ESP8266
           确保模块处于干净状态，避免固件状态机卡死 */
        if ((attempt % 5) == 0)  // 第5、10、15...次尝试前重置ESP8266
        {
            debug_print("[DEBUG] === Periodic ESP8266 reset (anti-deadlock) ===\r\n");
            esp_send_cmd_wait("AT+RST", resp, sizeof(resp), 2000);
            // 等待ESP8266完全重启（含bootloader时间）
            HAL_Delay(5000);
            // 重启DMA接收
            if (hdma_usart1_rx.State == HAL_DMA_STATE_RESET)
            {
                HAL_UART_Receive_DMA(&huart1, esp_dma_rx, ESP_DMA_RX_SIZE);
                esp_dma_last_pos = 0;
            }
            debug_print("[DEBUG] ESP8266 reset complete, continuing SmartConfig...\r\n");
        }

        // 每次尝试50秒
        result = esp_start_smartconfig_ex(50000);

        if (result == 0)
        {
            // WiFi已连接
            debug_print("[DEBUG] SmartConfig FOREVER: SUCCESS on attempt #");
            debug_print(buf);
            debug_print("\r\n");
            return 0;
        }

        // 每次尝试之间短暂等待，让ESP8266复位配网状态
        debug_print("[DEBUG] SmartConfig attempt failed, retrying in 3s...\r\n");
        HAL_Delay(3000);

    } while (1);

    // 永远不会到达这里
    return 0;
}

/* ---- High-level WiFi auto-connect orchestrator ----
   三阶段配网策略（优先级从高到低）：
   Phase 1: 尝试ESP8266内部Flash已保存的WiFi（上次配网成功的）— 15秒超时
   Phase 2: 尝试代码中硬编码的默认WiFi（ssid/password参数）— 30秒超时
   Phase 3: SmartConfig智能配网 — 无限等待直到用户手机配网成功
   调用此函数后要么WiFi已连接（返回0），要么永不返回（Phase 3） */
int esp_wifi_auto_connect(const char *default_ssid, const char *default_password)
{
    int result;

    debug_print("\r\n[DEBUG] ===== WiFi Auto-Connect Start =====\r\n");

    // ===== Phase 1: 尝试已保存的WiFi（15秒）=====
    // ESP8266内部Flash存着上次SmartConfig或AT+CWJAP成功后的凭证
    debug_print("[DEBUG] Phase 1: Trying saved WiFi (15s)...\r\n");
    result = esp_try_connect_saved_wifi(15000);
    if (result == 0)
    {
        debug_print("[DEBUG] Phase 1 OK - connected with saved WiFi\r\n");
        g_wifi_state = ESP_WIFI_STATE_CONNECTED;
        return 0;
    }
    debug_print("[DEBUG] Phase 1 failed - no saved WiFi or out of range\r\n");

    // ===== Phase 2: 尝试硬编码默认WiFi（30秒）=====
    if (default_ssid && default_ssid[0] != '\0')
    {
        debug_print("[DEBUG] Phase 2: Trying default WiFi (30s): ");
        debug_print(default_ssid);
        debug_print("\r\n");
        result = esp_init_and_connect_wifi(default_ssid, default_password);
        if (result == 0)
        {
            debug_print("[DEBUG] Phase 2 OK - connected with default WiFi\r\n");
            g_wifi_state = ESP_WIFI_STATE_CONNECTED;
            return 0;
        }
        debug_print("[DEBUG] Phase 2 failed - default WiFi not available\r\n");
    }
    else
    {
        debug_print("[DEBUG] Phase 2 skipped - no default SSID configured\r\n");
    }

    // ===== Phase 3: SmartConfig无限等待 =====
    // 用户用手机ESP-TOUCH/AirKiss配网，不成功不罢休
    debug_print("[DEBUG] Phase 3: Starting SmartConfig (wait forever)...\r\n");
    result = esp_start_smartconfig_forever();
    // esp_start_smartconfig_forever 永不返回失败，到这里的result一定是0

    debug_print("[DEBUG] Phase 3 OK - SmartConfig WiFi connected!\r\n");
    g_wifi_state = ESP_WIFI_STATE_CONNECTED;
    return 0;
}

/* ---- 显式SSID/密码连接（用于Phase 2：硬编码默认WiFi） ----
   发送AT+CWJAP进行显式连接，连接成功后ESP8266会自动保存凭证到Flash
   下次上电Phase 1（已保存WiFi）可以直接连接 */
int esp_init_and_connect_wifi(const char *ssid, const char *password)
{
    char resp[512];
    char cmd[256];

    if (ssid == NULL || ssid[0] == '\0' || password == NULL)
    {
        debug_print("[DEBUG] init_and_connect: invalid SSID/password, skip\r\n");
        return -1;
    }

    debug_print("[DEBUG] Connecting with explicit SSID: ");//explicit：明确的，显式的
    debug_print(ssid);
    debug_print("\r\n");
    g_wifi_state = ESP_WIFI_STATE_TRYING_DEFAULT;

    // 确保DMA在运行
    if (hdma_usart1_rx.State == HAL_DMA_STATE_RESET)
    {
        HAL_UART_Receive_DMA(&huart1, esp_dma_rx, ESP_DMA_RX_SIZE);
        esp_dma_last_pos = 0;
    }

    // 设置Station模式
    esp_send_cmd_wait("AT+CWMODE=1", resp, sizeof(resp), 2000);

    // 开启自动连接（连接成功后ESP8266会保存凭证到Flash）
    esp_send_cmd_wait("AT+CWAUTOCONN=1", resp, sizeof(resp), 2000);

    // 显式连接指定的WiFi（15秒超时，AP不可达时AT+CWJAP约10~15秒返回ERROR）
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);
    esp_send_cmd_wait(cmd, resp, sizeof(resp), 15000);

    // 等待WIFI CONNECTED URC（再给3秒，连接成功的话URC很快就会到）URC: Unsolicited Result Code，非命令响应的异步消息
    if (esp_wait_for_wifi_connected(3000) == 0)
    {
        g_wifi_state = ESP_WIFI_STATE_CONNECTED;
        debug_print("[DEBUG] Explicit WiFi connect OK\r\n");
        return 0;
    }

    debug_print("[DEBUG] Explicit WiFi connect FAILED\r\n");
    return -1;
}

void esp_start_dma_rx(void)
{
    // start circular DMA reception into esp_dma_rx 中文：启动DMA接收，将数据接收缓冲区设置为esp_dma_rx大小
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

/* ---- Reconnect logic: uses saved WiFi (no need for explicit ssid/password) ----
   重连策略：
   1. 优先使用ESP8266已保存的WiFi凭证自动连接
   2. 如果保存的WiFi连接失败，尝试使用函数参数传入的SSID/密码
   3. 如果仍未连接，尝试使用SmartConfig之前获取的凭证 */
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
            // 保存的WiFi连接失败，尝试使用传入的SSID/密码
            if (ssid && ssid[0] && password)
            {
                debug_print("[DEBUG] Reconnect: falling back to explicit SSID: ");
                debug_print(ssid);
                debug_print("\r\n");
                // 显式连接WiFi
                char cmd[256];
                char resp[256];
                snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);
                esp_send_cmd_wait(cmd, resp, sizeof(resp), 30000);
                // 等待连接
                esp_wait_for_wifi_connected(15000);
            }

            // 如果仍然未连接，尝试使用SmartConfig之前获取的凭证
            if (!g_wifi_connected && g_smartconfig_got_credentials && g_smartconfig_ssid[0] != '\0')
            {
                debug_print("[DEBUG] Reconnect: trying SmartConfig credentials: ");
                debug_print(g_smartconfig_ssid);
                debug_print("\r\n");
                char cmd[256];
                char resp[256];
                snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"",
                         g_smartconfig_ssid, g_smartconfig_password);
                esp_send_cmd_wait(cmd, resp, sizeof(resp), 30000);
                esp_wait_for_wifi_connected(15000);
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
