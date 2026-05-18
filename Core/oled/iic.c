#include "iic.h"
#include "main.h"

/* short delay for timing (tuned empirically) */
static void iic_delay(void)
{
	volatile int i = 0;
	for (i = 0; i < 30; i++) {
		__NOP();
	}
}

/* set SDA as output */
static void sda_output(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};
	GPIO_InitStruct.Pin = OLED_SDA_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(OLED_SDA_GPIO_Port, &GPIO_InitStruct);
}

/* set SDA as input */
static void sda_input(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};
	GPIO_InitStruct.Pin = OLED_SDA_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(OLED_SDA_GPIO_Port, &GPIO_InitStruct);
}

uint8_t iic_init(void)
{
	/* ensure GPIO clocks and pins are already initialized by CubeMX
	 * here we set pins to idle high
	 */
	HAL_GPIO_WritePin(OLED_SCL_GPIO_Port, OLED_SCL_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(OLED_SDA_GPIO_Port, OLED_SDA_Pin, GPIO_PIN_SET);
	sda_output();
	return 0;
}

uint8_t iic_deinit(void)
{
	/* set pins to inputs to save power */
	GPIO_InitTypeDef GPIO_InitStruct = {0};
	GPIO_InitStruct.Pin = OLED_SCL_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(OLED_SCL_GPIO_Port, &GPIO_InitStruct);
	GPIO_InitStruct.Pin = OLED_SDA_Pin;
	HAL_GPIO_Init(OLED_SDA_GPIO_Port, &GPIO_InitStruct);
	return 0;
}

/* generate start condition */
static void iic_start(void)
{
	sda_output();
	HAL_GPIO_WritePin(OLED_SDA_GPIO_Port, OLED_SDA_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(OLED_SCL_GPIO_Port, OLED_SCL_Pin, GPIO_PIN_SET);
	iic_delay();
	HAL_GPIO_WritePin(OLED_SDA_GPIO_Port, OLED_SDA_Pin, GPIO_PIN_RESET);
	iic_delay();
	HAL_GPIO_WritePin(OLED_SCL_GPIO_Port, OLED_SCL_Pin, GPIO_PIN_RESET);
}

/* generate stop condition */
static void iic_stop(void)
{
	sda_output();
	HAL_GPIO_WritePin(OLED_SCL_GPIO_Port, OLED_SCL_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(OLED_SDA_GPIO_Port, OLED_SDA_Pin, GPIO_PIN_RESET);
	iic_delay();
	HAL_GPIO_WritePin(OLED_SCL_GPIO_Port, OLED_SCL_Pin, GPIO_PIN_SET);
	iic_delay();
	HAL_GPIO_WritePin(OLED_SDA_GPIO_Port, OLED_SDA_Pin, GPIO_PIN_SET);
	iic_delay();
}

/* write a byte, return 0 if ACK received, 1 if NACK */
static uint8_t iic_write_byte(uint8_t data)
{
	uint8_t i;
	sda_output();
	for (i = 0; i < 8; i++) {
		if (data & 0x80) {
			HAL_GPIO_WritePin(OLED_SDA_GPIO_Port, OLED_SDA_Pin, GPIO_PIN_SET);
		} else {
			HAL_GPIO_WritePin(OLED_SDA_GPIO_Port, OLED_SDA_Pin, GPIO_PIN_RESET);
		}
		data <<= 1;
		iic_delay();
		HAL_GPIO_WritePin(OLED_SCL_GPIO_Port, OLED_SCL_Pin, GPIO_PIN_SET);
		iic_delay();
		HAL_GPIO_WritePin(OLED_SCL_GPIO_Port, OLED_SCL_Pin, GPIO_PIN_RESET);
	}
	/* release SDA for ACK */
	sda_input();
	iic_delay();
	HAL_GPIO_WritePin(OLED_SCL_GPIO_Port, OLED_SCL_Pin, GPIO_PIN_SET);
	iic_delay();
	uint8_t ack = HAL_GPIO_ReadPin(OLED_SDA_GPIO_Port, OLED_SDA_Pin);
	HAL_GPIO_WritePin(OLED_SCL_GPIO_Port, OLED_SCL_Pin, GPIO_PIN_RESET);
	sda_output();
	return ack; /* 0 means ACK */
}

/* write buffer: addr is 7-bit address, reg is first byte to send after addr */
uint8_t iic_write(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
	uint8_t ret;
	iic_start();
	/* send device addr byte (driver uses 8-bit address value like 0x78) */
	ret = iic_write_byte(addr);
	if (ret) {
		iic_stop();
		return 1;
	}
	/* send register/control byte */
	ret = iic_write_byte(reg);
	if (ret) {
		iic_stop();
		return 1;
	}
	for (uint16_t i = 0; i < len; i++) {
		ret = iic_write_byte(buf[i]);
		if (ret) {
			iic_stop();
			return 1;
		}
	}
	iic_stop();
	return 0;
}

