/*
 * Simple software I2C (bit-bang) driver for STM32 HAL
 * Pins used: OLED_SCL_Pin / OLED_SCL_GPIO_Port
 *            OLED_SDA_Pin / OLED_SDA_GPIO_Port
 */
#ifndef __IIC_H
#define __IIC_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

uint8_t iic_init(void);
uint8_t iic_deinit(void);
uint8_t iic_write(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);

#endif

