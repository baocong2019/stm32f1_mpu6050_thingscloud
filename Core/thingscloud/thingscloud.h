// ThingsCloud helper prototypes
#ifndef __THINGS_CLOUD_H
#define __THINGS_CLOUD_H

void thingscloud_init(void);
void thingscloud_publish_temp(float t);
void thingscloud_poll_and_handle(void);

/* 获取/设置云平台下发的目标温度，供OLED显示使用 */
int  thingscloud_get_target_temp(void);
void thingscloud_set_target_temp(int t);

#endif
