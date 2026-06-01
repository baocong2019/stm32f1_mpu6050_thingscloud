/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "../oled/driver_ssd1306_display_test.h"
#include "../oled/driver_ssd1306_basic.h"
#include <string.h>
#include <stdio.h>
/* MPU6050 basic example header */
#include "../mpu6050/driver_mpu6050_basic.h"
#include <stdlib.h>
#include "../thingscloud/thingscloud.h"
#include "../thingscloud/esp8266_at.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */



/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;

TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
DMA_HandleTypeDef hdma_usart1_rx;

/* USER CODE BEGIN PV */
char buf[80];
int value = 123;
float mpu_g[3]; /* acceleration, g */
float mpu_dps[3]; /* gyroscope, degrees/s */
uint32_t led_cnt=0;
int16_t accel_raw[3] = {0};
int16_t gyro_raw[3] = {0};
float temp_deg = 0;
int temp_tenth = 0;
volatile uint8_t publish_flag = 0;
volatile uint8_t push_data_thingscloud_interval_time = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM2_Init(void);

extern void esp_start_dma_rx(void);


/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void Get_MPU6050_Data()
{
    uint8_t raw14[14];
    HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&hi2c1, MPU6050_ADDRESS_AD0_LOW, 0x3B, I2C_MEMADD_SIZE_8BIT, raw14, 14, 500);
    if (st == HAL_OK)
    {
        accel_raw[0] = (int16_t)((uint16_t)raw14[0] << 8) | raw14[1];
        accel_raw[1] = (int16_t)((uint16_t)raw14[2] << 8) | raw14[3];
        accel_raw[2] = (int16_t)((uint16_t)raw14[4] << 8) | raw14[5];
        int16_t temp_raw = (int16_t)((uint16_t)raw14[6] << 8) | raw14[7];
        gyro_raw[0] = (int16_t)((uint16_t)raw14[8] << 8) | raw14[9];
        gyro_raw[1] = (int16_t)((uint16_t)raw14[10] << 8) | raw14[11];
        gyro_raw[2] = (int16_t)((uint16_t)raw14[12] << 8) | raw14[13];

        temp_deg = (float)temp_raw / 340.0f + 36.53f;
    }
    else
    {
        HAL_UART_Transmit(&huart2, (uint8_t *)"direct read 0x3B fail\r\n", 24, 1000);
    }

    //mpu6050_basic_read_temperature(&temp_deg);
}

void Dis_MPU6050_Data_On_oled()
{
    // snprintf(buf, sizeof(buf), "Ax:%6d Ay:%6d", accel_raw[0], accel_raw[1]);
    // ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);

    // snprintf(buf, sizeof(buf), "Az:%6d Gx:%6d", accel_raw[2], gyro_raw[0]);
    // ssd1306_basic_string(0, 16, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);

    // snprintf(buf, sizeof(buf), "Gy:%6d Gz:%6d", gyro_raw[1], gyro_raw[2]);
    // ssd1306_basic_string(0, 32, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);

    temp_tenth = (int)(temp_deg * 10.0f + (temp_deg >= 0 ? 0.5f : -0.5f));
    snprintf(buf, sizeof(buf), "T:%d.%dC", temp_tenth / 10, abs(temp_tenth % 10));
    ssd1306_basic_string(0, 48, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
}


void Dis_wifi_status_on_oled()
{
    // show WiFi status to the right of temperature (clear area then write)
    char wbuf[32];
    if (esp_is_wifi_connected())
    {
      snprintf(wbuf, sizeof(wbuf), "W:OK");
    }
    else
    {
      snprintf(wbuf, sizeof(wbuf), "W:--");
    }
    ssd1306_basic_string(70, 48, wbuf, (uint16_t)strlen(wbuf), Oled_dis_Zhengxian, SSD1306_FONT_12);
}

void Uart_Send_MPU6050_Data()
{
    HAL_UART_Transmit(&huart2, (uint8_t *)"RAW: ", 5, 1000);
    snprintf(buf, sizeof(buf), "Ax=%d,Ay=%d,Az=%d,Gx=%d,Gy=%d,Gz=%d,T=%d.%d\r\n",
    accel_raw[0], accel_raw[1], accel_raw[2], gyro_raw[0], gyro_raw[1], gyro_raw[2], temp_tenth / 10, abs(temp_tenth % 10));
    HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)strlen(buf), 1000);
}

void Blink_Board_LED()
{
    led_cnt++;
    if(led_cnt%20000==0)
    {
      HAL_GPIO_TogglePin(BOARD_LED_GPIO_Port, BOARD_LED_Pin);
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if(htim->Instance == TIM2)
    {
        led_cnt++;
        if(led_cnt >= 100) // 10ms × 100 = 1000ms = 1s
        {
            led_cnt = 0;
            //HAL_GPIO_TogglePin(BOARD_LED_GPIO_Port, BOARD_LED_Pin);
            push_data_thingscloud_interval_time++;
            if (push_data_thingscloud_interval_time >= 5) // 5s interval
            {
                push_data_thingscloud_interval_time = 0;
                publish_flag = 1; // signal main loop to publish temperature
            }
        }
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */
  HAL_Init();
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */
  SystemClock_Config();
  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  esp_start_dma_rx();
  MX_I2C1_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  HAL_TIM_Base_Start_IT(&htim2); // 启动TIM2并开启更新中断
  ssd1306_basic_init(SSD1306_INTERFACE_IIC, SSD1306_ADDR_SA0_0);
  ssd1306_basic_display_on();
  thingscloud_init();
  if (mpu6050_basic_init(MPU6050_ADDRESS_AD0_LOW) != 0)
  {
    HAL_UART_Transmit(&huart2, (uint8_t *)"MPU6050 init failed\r\n", 21, 1000);
    snprintf(buf, sizeof(buf), "MPU6050 init failed");
    ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
  }
  else
  {
    snprintf(buf, sizeof(buf), "MPU6050 init ok");
    ssd1306_basic_string(0, 0, buf, (uint16_t)strlen(buf), Oled_dis_Zhengxian, SSD1306_FONT_12);
    HAL_Delay(400);
    ssd1306_basic_clear();
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    Get_MPU6050_Data();
    Dis_MPU6050_Data_On_oled();
    Dis_wifi_status_on_oled();
    //Uart_Send_MPU6050_Data();
    if (publish_flag)
    {
        publish_flag = 0;
        thingscloud_publish_temp(temp_deg);
    }
    thingscloud_poll_and_handle();
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 7199;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 99;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(BOARD_LED_GPIO_Port, BOARD_LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, OLED_SCL_Pin|OLED_SDA_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : BOARD_LED_Pin */
  GPIO_InitStruct.Pin = BOARD_LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(BOARD_LED_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : OLED_SCL_Pin OLED_SDA_Pin */
  GPIO_InitStruct.Pin = OLED_SCL_Pin|OLED_SDA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
