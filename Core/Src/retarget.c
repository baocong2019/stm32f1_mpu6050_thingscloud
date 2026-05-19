/* retarget.c
 * Redirect printf output to USART2 (huart2)
 */

#include "main.h"

extern UART_HandleTypeDef huart2;

int __io_putchar(int ch)
{
    uint8_t c = (uint8_t)ch;
    if (HAL_UART_Transmit(&huart2, &c, 1, HAL_MAX_DELAY) == HAL_OK)
    {
        return ch;
    }
    return -1;
}

int __io_getchar(void)
{
    uint8_t c = 0;
    if (HAL_UART_Receive(&huart2, &c, 1, HAL_MAX_DELAY) == HAL_OK)
    {
        return c;
    }
    return -1;
}
