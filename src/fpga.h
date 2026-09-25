/********************************** (C) COPYRIGHT *******************************
* File Name          : fpga.h
* Version            : V1.0.0
* Description        : Altera FPGA configuration over SPI1 + DMA (passive serial).
*                      The sequence is ported from the working project
*                      D:\src\new_evo\test_altera (src/fpga.c).
*******************************************************************************/
#ifndef __FPGA_H
#define __FPGA_H

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************/
/* Configuration switches */

/* 1 - the FPGA is configured once at startup, 0 - the feature is disabled */
#define DEF_FPGA_CONFIG_EN              1

/* Delay between the start of the application and the configuration. In that time the USB
 * host stack enumerates its devices (a keyboard behind the HUB, needed for the hot key
 * handling planned later), so the FPGA is configured after the USB. */
#define DEF_FPGA_CONFIG_DELAY_MS        3000

/* Power task (FreeRTOS mode, see src/app_power.c): priority and stack size (words). The priority
 * is above DEF_RTOS_USB_TASK_PRIO, so neither the power-on sequence nor the FPGA transfer is
 * interrupted by the USB host task. 384 words give the task enough room for the button handling
 * and the configuration log messages. */
#define DEF_FPGA_CONFIG_PRIO            4
#define DEF_FPGA_CONFIG_STACK_WORDS     384

/* A failed configuration attempt is repeated: the first one may happen while the local regulators
 * of the FPGA board are still ramping (nSTATUS timeout), the retry finds stable rails. */
#define DEF_FPGA_CONFIG_RETRY           1
#define DEF_FPGA_CONFIG_RETRY_MS        500

/* Bytes handed over to the DMA in one go. 512 is what the working project used, the DMA
 * counter allows up to 65535 (a bigger block leaves less room for gaps between blocks). */
#define DEF_FPGA_CONFIG_BLOCK_SIZE      512

/* Timeouts in real milliseconds (they are measured with the 1 ms counter g_ms_ticks) */
#define DEF_FPGA_CONFIG_NSTATUS_MS      1000
#define DEF_FPGA_CONFIG_CONFDONE_MS     100

/*******************************************************************************/
/* FPGA pins: nCONFIG / nSTATUS / CONF_DONE on port A, SPI1 (full remap) on port B */
#define FPGA_PORT_CTRL                  GPIOA
#define FPGA_PORT_SPI                   GPIOB
#define FPGA_PIN_NCONFIG                GPIO_Pin_2
#define FPGA_PIN_NSTATUS                GPIO_Pin_3
#define FPGA_PIN_CONF_DONE              GPIO_Pin_4
#define FPGA_PIN_SPI_SCK                GPIO_Pin_3
#define FPGA_PIN_SPI_MOSI               GPIO_Pin_5

/*******************************************************************************/
/* Function Declaration */
extern uint8_t FPGA_Config( void );                    /* blocking, 1 = configured OK */

#ifdef __cplusplus
}
#endif

#endif /* __FPGA_H */
