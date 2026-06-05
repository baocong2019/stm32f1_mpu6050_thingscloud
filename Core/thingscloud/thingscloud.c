#include "esp8266_at.h"
#include "../oled/driver_ssd1306_basic.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define TARGET_PUBLISH_INTERVAL_MS 30000 /* 30 seconds default */

char buf_AccessToken[]="rq79c7pxv0kb6lzh";
char buf_ProjectKey[]="58Cr4nuaDL";
char ssid[32];
char password[32];
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
void thingscloud_set_target_temp(int t)
{
    char buf[32];
    target_temp = t;
    // update OLED (only the target line, don't clear everything)
    // draw a filled rect to erase the target line area, then redraw
    ssd1306_basic_rect(0, 16, 127, 28, 0);  // clear line 2 area
    snprintf(buf, sizeof(buf), "Target:%dC", target_temp);
    ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
    // publish attribute to cloud
    esp_mqtt_publish_attribute(push_topic, "target_temp", target_temp);
}

void thingscloud_init()
{
    char buf[50];
    int wifi_result;
    int mqtt_result;

    // DMA reception was already started in main() via esp_start_dma_rx().
    // Do NOT restart it here — that would reset esp_dma_last_pos and cause
    // data loss in the circular DMA buffer.

    // ====== Phase 1: WiFi Auto-Connect ======
    ssd1306_basic_clear();
    snprintf(buf, sizeof(buf), "WiFi: auto-connect...");
    ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);

    wifi_result = esp_wifi_auto_connect();

    if (wifi_result == 0)
    {
        int state = esp_get_wifi_state();
        ssd1306_basic_clear();
        snprintf(buf, sizeof(buf), "WiFi: OK (%s)",
                 (state == ESP_WIFI_STATE_CONNECTED) ? "connected" : "ready");
        ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);

        // display current target temperature
        snprintf(buf, sizeof(buf), "Target:%dC", target_temp);
        ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
        HAL_Delay(600);
        ssd1306_basic_clear();

        // ====== Phase 2: MQTT Connect ======
        snprintf(buf, sizeof(buf), "MQTT connecting...");
        ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);

        mqtt_result = esp_mqtt_usercfg_and_connect(buf_AccessToken, buf_ProjectKey, web, port, topic);
        if (esp_is_mqtt_connected())
        {
            ssd1306_basic_clear();
            snprintf(buf, sizeof(buf), "MQTT: OK");
            ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);

            // Subscribe to command topics
            esp_mqtt_subscribe(Subscribe_topic);

            HAL_Delay(600);
            ssd1306_basic_clear();
            snprintf(buf, sizeof(buf), "Subscribed OK");
            ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
        }
        else
        {
            ssd1306_basic_clear();
            snprintf(buf, sizeof(buf), "MQTT: FAILED");
            ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Fanxian, SSD1306_FONT_12);
            snprintf(buf, sizeof(buf), "Will retry in main loop");
            ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf), Oled_dis_Fanxian, SSD1306_FONT_12);
        }
    }
    else
    {
        // WiFi connection failed completely
        ssd1306_basic_clear();
        snprintf(buf, sizeof(buf), "WiFi: FAILED");
        ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Fanxian, SSD1306_FONT_12);
        snprintf(buf, sizeof(buf), "Will retry in loop");
        ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf), Oled_dis_Fanxian, SSD1306_FONT_12);
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
