
/*我想使用ESP01，已提前烧录了AT固件，使用MQTT连接到thingscloud，网络端thingscloud已经设置好相应设置。其参数我已经在thingscloud.c文件中提前按数组设置好了
请帮我使用图1中的步骤，正确发送指令，将MPU6050的温度数据按tim2的定时1s进行一传输到thingscloud，其打印信息使用串口2打印，其中esp01我已经连接到了STM32F103C8T6的
串口1-PA9\PA10上，波特率115200.你看参考ESP8266-AT文档，正确响应相应的指令和回复信息，确保后续判断问题出在了哪一步，最终实现效果就是让温度信息上传到thingscloud*/
// AccessToken:rq79c7pxv0kb6lzh
// ProjectKey:58Cr4nuaDL
// ssid:bbc
// password:36914700
// port:8883
// web:bj-2-mqtt.iot-api.com

// push_topic:attributes
// data_name:temperature


char buf_AccessToken[]="rq79c7pxv0kb6lzh";
char buf_ProjectKey[]="58Cr4nuaDL";
char ssid[]="bbc";
char password[]="36914700";
char port[]="1883";
char web[]="bj-2-mqtt.iot-api.com";
char push_topic[]="attributes";
char data_name[]="temperature";
char topic[]="topic";
char Subscribe_topic[]="attributes/push";
char Subscribe_command[]="command/send/+";
char command_name[]="LED_switch";


#include "esp8266_at.h"
#include "../oled/driver_ssd1306_basic.h"
#include <string.h>

void thingscloud_init()
{
	static char buf[50];
	static int wifi_result;
	static int mqtt_result;

	snprintf(buf, sizeof(buf), "connected to WiFi: %s", ssid);
    ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);

	wifi_result = esp_init_and_connect_wifi(ssid, password);
	if(wifi_result == 0)
	{
		ssd1306_basic_clear();
		snprintf(buf, sizeof(buf), "wifi is ok");
		ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
		HAL_Delay(400);
		ssd1306_basic_clear();
		snprintf(buf, sizeof(buf), "connecting to thingscloud");
		ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
		
		mqtt_result=esp_mqtt_usercfg_and_connect(buf_AccessToken, buf_ProjectKey, web, port, topic);
		if(mqtt_result == 0)
		{
			ssd1306_basic_clear();
			snprintf(buf, sizeof(buf), "mqtt is ok");
			ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
			// start DMA rx to capture incoming URCs and subscribe to command topic
			esp_start_dma_rx();
			esp_mqtt_subscribe(Subscribe_topic);
			esp_mqtt_subscribe(Subscribe_command);
			
			ssd1306_basic_clear();
			snprintf(buf, sizeof(buf), "subscribed topic");
			ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
		}
		else
		{
			ssd1306_basic_clear();
			snprintf(buf, sizeof(buf), "mqtt connect failed");
			ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Fanxian, SSD1306_FONT_12);
		}
	}
}

void thingscloud_publish_temp(float t)
{
	// ensure connection is alive; reconnect if necessary
	esp_check_and_reconnect(ssid, password, buf_AccessToken, buf_ProjectKey, web, port, push_topic);
	esp_mqtt_publish_temperature(push_topic, t);
}

/* Poll incoming MQTT messages and handle LED command
   Call this regularly from main loop (e.g., in while(1)). */
void thingscloud_poll_and_handle(void)
{
	char topic[128];
	char payload[256];
	if (esp_mqtt_poll(topic, sizeof(topic), payload, sizeof(payload)))
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
				if (turn_on)
				{
					HAL_GPIO_WritePin(BOARD_LED_GPIO_Port, BOARD_LED_Pin, GPIO_PIN_RESET);
					// ssd1306_basic_clear();
					ssd1306_basic_string(0, 0, "LED: ON", 6, Oled_dis_Zhengxian, SSD1306_FONT_12);
				}
				else
				{
					HAL_GPIO_WritePin(BOARD_LED_GPIO_Port, BOARD_LED_Pin, GPIO_PIN_SET);
					//ssd1306_basic_clear();
					ssd1306_basic_string(0, 0, "LED: OFF", 7, Oled_dis_Zhengxian, SSD1306_FONT_12);
				}
			}
		}
	}
}