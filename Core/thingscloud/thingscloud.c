
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


#include "esp8266_at.h"

void thingscloud_init()
{
	// connect WiFi
	esp_init_and_connect_wifi(ssid, password);
	// configure MQTT and connect
	esp_mqtt_usercfg_and_connect(buf_AccessToken, buf_ProjectKey, web, port, topic);
}

void thingscloud_publish_temp(float t)
{
	// ensure connection is alive; reconnect if necessary
	esp_check_and_reconnect(ssid, password, buf_AccessToken, buf_ProjectKey, web, port, push_topic);
	esp_mqtt_publish_temperature(push_topic, t);
}