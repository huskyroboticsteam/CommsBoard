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
#include "comp.h"
#include "app_fatfs.h"
#include "fdcan.h"
#include "i2c.h"
#include "rtc.h"
#include "spi.h"
#include "usb.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct {
  uint32_t total_us;       /* all writes + syncs + close */
  uint32_t kb_per_s;
  uint32_t avg_sector_us;  /* average 512-byte f_write */
  uint32_t max_sector_us;  /* slowest f_write: card busy pauses show up here */
  uint32_t max_sync_us;    /* slowest f_sync: FAT + directory update */
} SdSpeed;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Status LED. If PB0 gets the CubeMX label "LED_STATUS", main.h defines these. */
#ifndef LED_STATUS_Pin
#define LED_STATUS_Pin       GPIO_PIN_0
#define LED_STATUS_GPIO_Port GPIOB
#endif

#define SPEED_TEST_KB     256   /* how much the speed test writes */
#define SPEED_CHUNK_BYTES 8192
#define SPEED_SYNC_BYTES 32768

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
volatile FRESULT sd_test_result;  /* last FatFs result of the SD test; watch it in the debugger */
volatile SdSpeed sd_speed;        /* speed test results; also saved to SPEED.CSV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* SD card test: append one fake CAN26 Heartbeat line to CANTEST.CSV.
 * Returns 0 on success, or which step failed: 1 mount, 2 open, 3 write, 4 close. */
static uint8_t sd_test(void)
{
  char line[80];
  int n = 0;
  UINT written;

  sd_test_result = f_mount(&USERFatFs, USERPath, 1);
  if (sd_test_result != FR_OK) return 1;

  sd_test_result = f_open(&USERFile, "CANTEST.CSV", FA_OPEN_APPEND | FA_WRITE);
  if (sd_test_result != FR_OK) return 2;

  if (f_size(&USERFile) == 0)   /* new file: write the column names first */
    n = snprintf(line, sizeof line, "ms,id,dlc,data\r\n");
  /* Heartbeat from UUID 0x61 to the Jetson (0x01): error 0, state 1 */
  n += snprintf(line + n, sizeof line - n, "%lu,0x408,7,02 61 00 00 00 00 01\r\n", (unsigned long)HAL_GetTick());

  sd_test_result = f_write(&USERFile, line, n, &written);
  if (sd_test_result != FR_OK || written != (UINT)n) {
    f_close(&USERFile);
    return 3;
  }

  sd_test_result = f_close(&USERFile);
  if (sd_test_result != FR_OK) return 4;
  return 0;
}

/* Microseconds since start, using the CPU cycle counter (no RTC needed).
 * The 32-bit counter wraps after ~25 s at 170 MHz, so only time shorter things. */
static uint32_t us_since(uint32_t start)
{
  return (DWT->CYCCNT - start) / (SystemCoreClock / 1000000U);
}

/* Write-speed test: writes SPEED_TEST_KB to SPEED.BIN in SPEED_CHUNK_BYTES pieces (the way
 * the logger will), with an f_sync every SPEED_SYNC_BYTES bytes. Results go to
 * sd_speed and one line is appended to SPEED.CSV. Returns 0 on success, 5 on failure. */
static uint8_t sd_speed_test(void)
{
  static uint8_t buf[SPEED_CHUNK_BYTES] __attribute__((aligned(4)));
  char line[128];
  uint32_t t_all, t, us, sum_write_us = 0, i;
  UINT written;
  const uint32_t chunks = SPEED_TEST_KB * 1024U / SPEED_CHUNK_BYTES;
  int n = 0;

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* turn on the cycle counter */
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)i;

  sd_test_result = f_open(&USERFile, "SPEED.BIN", FA_CREATE_ALWAYS | FA_WRITE);
  if (sd_test_result != FR_OK) return 5;

  t_all = DWT->CYCCNT;
  for (i = 1; i <= chunks; i++) {
    t = DWT->CYCCNT;
    sd_test_result = f_write(&USERFile, buf, sizeof buf, &written);
    us = us_since(t);
    if (sd_test_result != FR_OK || written != sizeof buf) {
      f_close(&USERFile);
      return 5;
    }
    sum_write_us += us;
    if (us > sd_speed.max_sector_us) sd_speed.max_sector_us = us;

    if ((i * SPEED_CHUNK_BYTES) % SPEED_SYNC_BYTES == 0) {
      t = DWT->CYCCNT;
      sd_test_result = f_sync(&USERFile);
      us = us_since(t);
      if (sd_test_result != FR_OK) {
        f_close(&USERFile);
        return 5;
      }
      if (us > sd_speed.max_sync_us) sd_speed.max_sync_us = us;
    }
  }
  sd_test_result = f_close(&USERFile);
  if (sd_test_result != FR_OK) return 5;

  sd_speed.total_us = us_since(t_all);
  sd_speed.kb_per_s = (uint32_t)(SPEED_TEST_KB * 1000000ULL / sd_speed.total_us);
  sd_speed.avg_sector_us = sum_write_us / (SPEED_TEST_KB * 2);

  sd_test_result = f_open(&USERFile, "SPEED.CSV", FA_OPEN_APPEND | FA_WRITE);
  if (sd_test_result != FR_OK) return 5;
  if (f_size(&USERFile) == 0)
    n = snprintf(line, sizeof line, "kb,total_ms,kb_per_s,avg_sector_us,max_sector_us,max_sync_us\r\n");
  n += snprintf(line + n, sizeof line - n, "%d,%lu,%lu,%lu,%lu,%lu\r\n", SPEED_TEST_KB,
                (unsigned long)(sd_speed.total_us / 1000), (unsigned long)sd_speed.kb_per_s,
                (unsigned long)sd_speed.avg_sector_us, (unsigned long)sd_speed.max_sector_us,
                (unsigned long)sd_speed.max_sync_us);
  sd_test_result = f_write(&USERFile, line, n, &written);
  if (sd_test_result != FR_OK || written != (UINT)n) {
    f_close(&USERFile);
    return 5;
  }
  sd_test_result = f_close(&USERFile);
  return (sd_test_result == FR_OK) ? 0 : 5;
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

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_COMP1_Init();
  MX_FDCAN2_Init();
  MX_I2C1_Init();
  MX_RTC_Init();
  MX_SPI1_Init();
  MX_USB_PCD_Init();
  if (MX_FATFS_Init() != APP_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN 2 */
  uint8_t sd_fail = sd_test();
  if (sd_fail == 0) sd_fail = sd_speed_test();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* LED_STATUS: solid on = SD tests passed; N blinks then a pause = step N failed
     * (1 mount, 2 open, 3 write, 4 close, 5 speed test) */
    if (sd_fail == 0) {
      HAL_GPIO_WritePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin, GPIO_PIN_SET);
    } else {
      for (uint8_t i = 0; i < sd_fail; i++) {
        HAL_GPIO_WritePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin, GPIO_PIN_SET);
        HAL_Delay(200);
        HAL_GPIO_WritePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin, GPIO_PIN_RESET);
        HAL_Delay(300);
      }
      HAL_Delay(1500);
    }
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

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_LSI
                              |RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV6;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV4;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
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
