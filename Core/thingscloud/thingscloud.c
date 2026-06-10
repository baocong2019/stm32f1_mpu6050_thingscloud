#include "esp8266_at.h"
#include "../oled/driver_ssd1306_basic.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define TARGET_PUBLISH_INTERVAL_MS 30000 /* 30 seconds default */

char buf_AccessToken[]="rq79c7pxv0kb6lzh";
char buf_ProjectKey[]="58Cr4nuaDL";
char ssid[32]={0};       // 初始为空，SmartConfig成功后才填入实际SSID
char password[32]={0};   // 初始为空，SmartConfig成功后才填入实际密码
char port[]="1883";
char web[]="bj-2-mqtt.iot-api.com";
char push_topic[]="attributes";
char data_name[]="temperature";
char topic[]="topic";
char Subscribe_topic[]="attributes/push";
char Subscribe_command[]="command/send/+";
char command_name[]="LED_switch";

// target temperature synchronized with cloud
static int target_temp = 21;
static uint32_t last_target_pub = 0;

// set target temp locally and sync to cloud + update OLED
/* 获取云平台下发的目标温度，供main.c的OLED显示使用 */
int thingscloud_get_target_temp(void)
{
    return target_temp;
}

void thingscloud_set_target_temp(int t)
{
    char buf[32];
    target_temp = t;
    // 更新OLED第一行（y=0），不清屏，只擦除目标温度行
    ssd1306_basic_rect(0, 0, 127, 15, 0);  // 清除第1行
    snprintf(buf, sizeof(buf), "Target:%dC", target_temp);
    ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
    // 同步到云平台
    esp_mqtt_publish_attribute(push_topic, "target_temp", target_temp);
}

void thingscloud_init()
{
    char buf[50];
    char resp[256];
    int wifi_result;
    int mqtt_result;

    // DMA接收已经在main()中通过esp_start_dma_rx()启动
    // ESP8266上电后如果有已保存的WiFi凭证会自动连接并发送WIFI CONNECTED URC

    // ========================================================
    //  Phase 1: 被动等待ESP8266自动连接（不发送加入AP的AT命令）
    //  超时30秒，等待ESP8266自己连接之前保存过的WiFi
    //  如果Flash中没有凭证或WiFi不可用，则URC不会来
    // ========================================================
    ssd1306_basic_clear();
    snprintf(buf, sizeof(buf), "Ph1: Wait WiFi...");
    ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf),
                         Oled_dis_Zhengxian, SSD1306_FONT_12);
    snprintf(buf, sizeof(buf), "Auto-connecting..");
    ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf),
                         Oled_dis_Fanxian, SSD1306_FONT_12);

    // 纯被动等待：不发送AT+CWMODE、AT+CWAUTOCONN、AT+CWJAP
    // esp_wait_for_wifi_connected()内部已同步DMA指针，只监听新到达的URC
    esp_send_cmd_wait("AT+RST", resp, sizeof(resp), 2000);
    wifi_result = esp_wait_for_wifi_connected(30000);

    // 兜底检测：即使被动等待超时，ESP8266可能在DMA同步前就已经连接了
    // 用AT+CWJAP?查询一下（这只是查询命令，不发起连接）
    if (wifi_result != 0)
    {
        esp_send_cmd_wait("AT+CWJAP?", resp, sizeof(resp), 2000);
        if (strstr(resp, "+CWJAP:") != NULL)
        {
            // ESP8266实际已经连接到WiFi！只是WIFI CONNECTED URC在DMA同步前发出被丢弃了
          //  debug_print("[DEBUG] WiFi was already connected (detected by CWJAP? query)\r\n");
            esp_is_wifi_connected();
            g_wifi_connected = 1;
            esp_get_wifi_state();
            g_wifi_state = ESP_WIFI_STATE_CONNECTED;
            wifi_result = 0;
        }
    }

    if (wifi_result == 0)
    {
        // WiFi已连接成功（URC检测到 或 兜底查询到）
        // 查询当前连接的SSID并显示
        char ssid_buf[33] = {0};
        // 如果兜底查询已经发过了，resp中已有+CWJAP响应，直接用
        // 否则重新查询
        if (strstr(resp, "+CWJAP:") == NULL)
        {
            esp_send_cmd_wait("AT+CWJAP?", resp, sizeof(resp), 3000);
        }
        // 解析 +CWJAP:"SSID",... 中的SSID
        // 响应格式: +CWJAP:"MyWiFi","aa:bb:cc:dd:ee:ff",1,-40
        const char *p1 = strchr(resp, '"');
        if (p1 != NULL)
        {
            const char *p2 = strchr(p1 + 1, '"');
            if (p2 != NULL && (p2 - p1 - 1) < (int)sizeof(ssid_buf))
            {
                int len = (int)(p2 - p1 - 1);
                memcpy(ssid_buf, p1 + 1, len);
                ssid_buf[len] = '\0';
            }
        }

        ssd1306_basic_clear();
        snprintf(buf, sizeof(buf), "WiFi: OK(saved)");
        ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf),
                             Oled_dis_Zhengxian, SSD1306_FONT_12);
        if (ssid_buf[0] != '\0')
        {
            snprintf(buf, sizeof(buf), "SSID:%s", ssid_buf);
            ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf),
                                 Oled_dis_Fanxian, SSD1306_FONT_12);
            // 保存到全局变量，供后续重连回退使用
            strncpy(ssid, ssid_buf, sizeof(ssid) - 1);
        }
        HAL_Delay(800);
        goto mqtt_phase;
    }

    // Phase 1 确实超时：无已保存WiFi或WiFi不可达
    ssd1306_basic_clear();
    snprintf(buf, sizeof(buf), "No saved WiFi");
    ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf),
                         Oled_dis_Fanxian, SSD1306_FONT_12);
    snprintf(buf, sizeof(buf), "Go SmartConfig..");
    ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf),
                         Oled_dis_Fanxian, SSD1306_FONT_12);
    HAL_Delay(800);

    // ========================================================
    //  Phase 2: SmartConfig智能配网（无限等待直到成功）
    //  用户使用手机ESP-TOUCH/AirKiss App配网
    //  内置防死锁：每5次失败自动AT+RST重置ESP8266
    //  配网成功后ESP8266自动保存凭证到Flash → 下次上电Phase 1自动连接
    // ========================================================
    ssd1306_basic_clear();
    snprintf(buf, sizeof(buf), "Ph2: SmartConfig");
    ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf),
                         Oled_dis_Zhengxian, SSD1306_FONT_12);
    snprintf(buf, sizeof(buf), "Use ESP-TOUCH App");
    ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf),
                         Oled_dis_Fanxian, SSD1306_FONT_12);

    // 无限等待直到手机配网成功（内置ESP8266防死锁重置）
    wifi_result = esp_start_smartconfig_forever();

    ssd1306_basic_clear();
    snprintf(buf, sizeof(buf), "WiFi: OK(smart)");
    ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf),
                         Oled_dis_Zhengxian, SSD1306_FONT_12);
    HAL_Delay(600);

    // 将SmartConfig获取的新WiFi凭证保存到全局变量
    // 下次上电Phase 1即可自动连接此WiFi
    esp_get_smartconfig_credentials(ssid, sizeof(ssid),
                                    password, sizeof(password));

mqtt_phase:
    ssd1306_basic_clear();

    // ========================================================
    //  MQTT连接与订阅
    // ========================================================
    snprintf(buf, sizeof(buf), "MQTT connecting...");
    ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf),
                         Oled_dis_Zhengxian, SSD1306_FONT_12);

    mqtt_result = esp_mqtt_usercfg_and_connect(buf_AccessToken, buf_ProjectKey,
                                                web, port, topic);
    if (esp_is_mqtt_connected())
    {
        ssd1306_basic_clear();
        snprintf(buf, sizeof(buf), "MQTT: OK");
        ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf),
                             Oled_dis_Zhengxian, SSD1306_FONT_12);

        // 显示目标温度
        snprintf(buf, sizeof(buf), "Target:%dC", target_temp);
        ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf),
                             Oled_dis_Zhengxian, SSD1306_FONT_12);

        // 订阅命令下发主题
        esp_mqtt_subscribe(Subscribe_topic);

        HAL_Delay(600);
        ssd1306_basic_clear();
        snprintf(buf, sizeof(buf), "Sub: OK");
        ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf),
                             Oled_dis_Zhengxian, SSD1306_FONT_12);
    }
    else
    {
        ssd1306_basic_clear();
        snprintf(buf, sizeof(buf), "MQTT: FAILED");
        ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf),
                             Oled_dis_Fanxian, SSD1306_FONT_12);
        snprintf(buf, sizeof(buf), "Retry in loop..");
        ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf),
                             Oled_dis_Fanxian, SSD1306_FONT_12);
    }
}

void thingscloud_publish_temp(float t)
{
    // ensure connection is alive; reconnect if necessary
    esp_check_and_reconnect(ssid, password, buf_AccessToken, buf_ProjectKey, web, port, push_topic);
    esp_mqtt_publish_temperature(push_topic, t);
    // occasionally publish current target temperature so cloud sees latest value
    uint32_t now = HAL_GetTick();
    if ((now - last_target_pub) > TARGET_PUBLISH_INTERVAL_MS)
    {
        esp_mqtt_publish_attribute(push_topic, "target_temp", target_temp);
        last_target_pub = now;
    }
}

/* Poll incoming MQTT messages and handle LED command
   Call this regularly from main loop (e.g., in while(1)). */
void thingscloud_poll_and_handle(void)
{
    char topic_buf[128];
    char payload[256];
    if (esp_mqtt_poll(topic_buf, sizeof(topic_buf), payload, sizeof(payload)))
    {
        // simple check: look for command key (either command_name or "led")
        const char *key = NULL;
        if (strstr(payload, command_name) != NULL) key = command_name;
        else if (strstr(payload, "led") != NULL) key = "led";

        if (key)
        {
            // find the value after the key: look for ':' following the key
            const char *p = strstr(payload, key);
            const char *colon = NULL;
            if (p) colon = strchr(p, ':');
            if (colon)
            {
                // skip whitespace and possible quotes
                const char *v = colon + 1;
                while (*v == ' ' || *v == '\"' || *v == '\'') v++;
                // read token
                char token[32] = {0};
                int i = 0;
                while (*v != '\0' && *v != '"' && *v != '}' && *v != ',' && i < (int)sizeof(token)-1)
                {
                    token[i++] = *v++;
                }
                token[i] = '\0';

                // determine on/off
                int turn_on = 0;
                if (strcmp(token, "on") == 0 || strcmp(token, "1") == 0 || strcmp(token, "true") == 0)
                    turn_on = 1;

                // Many BluePill/PC13 LEDs are active-low: set RESET to turn on
                // Only update LED area on OLED, don't clear everything
                if (turn_on)
                {
                    HAL_GPIO_WritePin(BOARD_LED_GPIO_Port, BOARD_LED_Pin, GPIO_PIN_SET);
                    ssd1306_basic_rect(0, 0, 127, 15, 0);  // clear line 1 area
                    ssd1306_basic_string(0, 0, "LED:ON", 6, Oled_dis_Zhengxian, SSD1306_FONT_12);
                }
                else
                {
                    HAL_GPIO_WritePin(BOARD_LED_GPIO_Port, BOARD_LED_Pin, GPIO_PIN_RESET);
                    ssd1306_basic_rect(0, 0, 127, 15, 0);  // clear line 1 area
                    ssd1306_basic_string(0, 0, "LED:OFF", 7, Oled_dis_Zhengxian, SSD1306_FONT_12);
                }
            }
        }
        // also check for attribute update: target_temp
        const char *p_tt = strstr(payload, "target_temp");
        if (p_tt)
        {
            const char *colon = strchr(p_tt, ':');
            if (colon)
            {
                int val = atoi(colon+1);
                if (val != 0 || strstr(colon+1, "0") != NULL)
                {
                    thingscloud_set_target_temp(val);
                }
            }
        }
    }
}
