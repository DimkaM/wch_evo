/********************************** (C) COPYRIGHT *******************************
* File Name          : main.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2022/09/01
* Description        : Main program body.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

/*
 * @Note
 * This example demonstrates the process of enumerating the keyboard and mouse 
 * by a USB host and obtaining data based on the polling time of the input endpoints 
 * of the keyboard and mouse. 
 * The USBFS port also supports enumeration of keyboard and mouse attached at tier
 * level 2(Hub 1).
*/

/*
 * @Note
 * Please select the corresponding macro definition (CH32V30x_D8C/CH32V30x_D8)
 * and startup_xxx.s file according to the chip model, otherwise the example may be abnormal.
 * In addition, when the system clock is selected as the USBFS clock source, only 144MHz/96MHz/48MHz
 * are supported.
 */

/*******************************************************************************/
/* Header Files */
#include "usb_host_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_tasks.h"
#include "fpga.h"
#include "app_usb.h"
#include "app_power.h"

/*********************************************************************
 * @fn      main
 *
 * @brief   Main program.
 *
 * @return  none
 */

#if DEF_FPGA_CONFIG_EN && DEF_POWER_AUTO_ON && !DEF_FREERTOS_EN
/* one-shot flag of the automatic startup in the bare-metal mode (see the super loop) */
static uint8_t StartupDone = 0;
#endif

int main( void )
{
    /* Initialize system configuration */
    SystemCoreClockUpdate( );
    Delay_Init( );
    USART_Printf_Init( 115200 );
    
    printf( "SystemClk:%d\r\n", (int)SystemCoreClock );
    printf( "ChipID:%08x\r\n", (unsigned int)DBGMCU_GetCHIPID() );
    printf( "USB HOST KM Test\r\n" );

    /* Initialize TIM3 */
    TIM3_Init( 9, SystemCoreClock / 10000 - 1 );
    printf( "TIM3 Init OK!\r\n" );

#if DEF_FREERTOS_EN
    /* FreeRTOS mode: the global interrupt (MIE) is enabled by the scheduler when the
     * first task is started, from then on the 1 ms counter g_ms_ticks is incremented by
     * the TIM3 interrupt handler. */
    printf( "FreeRTOS %s\r\n", tskKERNEL_VERSION_NUMBER );

    AppTasks_Start( );
    vTaskStartScheduler( );

    /* The scheduler returns only when there is not enough heap for the idle task. */
    printf( "!!! scheduler returned !!!\r\n" );
    while( 1 )
    {
    }
#else
    /* Original bare-metal mode: one super loop polls the USBFS host port. */

    /* The startup code configures the privileged mode but does not enable the global
     * interrupt (MIE), so the timer interrupt used to poll the HID/HUB endpoints would
     * never be taken. */
    __enable_irq();
    printf( "Global IRQ Enabled\r\n" );

    uint32_t t_start;

    /* ATX control pins and the button, the PSU stays off until the startup below switches it on
     * (see src/power.c, src/button.c, src/app_power.c); g_ms_ticks is already running. */
    AppPower_Init( );

    /* The USB host stack is initialised according to the PSU state (see src/app_usb.c): with
     * DEF_USB_OFF_WHEN_PSU_OFF it stays down until PWR_OK appears. */
    AppUsb_Init( );

    t_start = g_ms_ticks;

    while( 1 )
    {
        AppUsb_Step( );

#if DEF_FPGA_CONFIG_EN && DEF_POWER_AUTO_ON
        /* The automatic startup happens after a fixed delay, exactly as in the RTOS mode, so the
         * USB host stack is initialised by then. Waiting for g_usbRootReady is not possible here:
         * with DEF_USB_OFF_WHEN_PSU_OFF the stack is down while the PSU is off, the flag would
         * never be set and the PSU would never be switched on. */
        if( ( StartupDone == 0 ) && ( ( g_ms_ticks - t_start ) >= DEF_FPGA_CONFIG_DELAY_MS ) )
        {
            StartupDone = 1;
            AppPower_Startup( );
        }
#endif

        AppPower_Step( );
    }
#endif
}



