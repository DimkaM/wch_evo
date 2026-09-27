/********************************** (C) COPYRIGHT *******************************
* File Name          : app_tasks.c
* Version            : V1.0.0
* Description        : FreeRTOS application tasks.
*                      Bring-up task of the gate G1/G2: it checks that the scheduler runs,
*                      that the RTOS tick advances with the expected rate and that a
*                      hardware interrupt (TIM3) is still served while the scheduler is
*                      running. The measuring reference is the 1 ms counter g_ms_ticks
*                      which is incremented by TIM3_IRQHandler (see app_km.c).
*******************************************************************************/
#include "usb_host_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_tasks.h"
#include "fpga.h"
#include "app_usb.h"
#include "app_power.h"
#include "rtc.h"
#include "zx.h"

/*******************************************************************************/
/* Variable Declaration */
extern volatile uint32_t g_ms_ticks;

static TaskHandle_t xUsbHostTaskHandle = NULL;

#if DEF_RTOS_TEST_TASK

/*********************************************************************
 * @fn      vRtosTestTask
 *
 * @brief   Bring-up task: prints the real duration of DEF_RTOS_TEST_DELAY_TICKS RTOS
 *          ticks, the free heap size and the unused stack of the task itself.
 *
 * @param   pvParameters - not used.
 *
 * @return  none
 */
static void vRtosTestTask( void *pvParameters )
{
    uint32_t ms_before, ms_now;
    uint32_t tick_before, tick_delta;
    uint32_t ms_delay_10ms, ms_delay_1000us;
    uint32_t rtc_cnt = 0;
    uint32_t heap_now, heap_prev = 0;
    uint32_t heapmin_now, heapmin_prev = 0;
    uint32_t stack_now, stack_prev = 0;
    uint32_t usbstack_now, usbstack_prev = 0;

    for( ;; )
    {
        ms_before = g_ms_ticks;
        tick_before = (uint32_t)xTaskGetTickCount( );

        vTaskDelay( DEF_RTOS_TEST_DELAY_TICKS );

        ms_now = g_ms_ticks;
        tick_delta = (uint32_t)xTaskGetTickCount( ) - tick_before;

        /* Gate G3: the blocking delays (TIM4 based) must be accurate while the scheduler
         * is running, therefore they are measured against the 1 ms counter as well. */
        ms_delay_10ms = g_ms_ticks;
        Delay_Ms( 10 );
        ms_delay_10ms = g_ms_ticks - ms_delay_10ms;

        ms_delay_1000us = g_ms_ticks;
        Delay_Us( 1000 );
        ms_delay_1000us = g_ms_ticks - ms_delay_1000us;

        heap_now = (uint32_t)xPortGetFreeHeapSize( );
        heapmin_now = (uint32_t)xPortGetMinimumEverFreeHeapSize( );
        stack_now = (uint32_t)uxTaskGetStackHighWaterMark( NULL );
        usbstack_now = (uint32_t)uxTaskGetStackHighWaterMark( xUsbHostTaskHandle );

        /* The line is printed only when something deserves attention: a change of the heap or of a
         * stack high water mark (a leak, or a stack which grows) or a blocking delay which did not
         * come out right - i.e. a scheduling hiccup. While everything is stable the task is
         * silent, so the log stays clean (the previous always-on line is what DEF_RTOS_TEST_TASK
         * was for during the bring-up). */
        if( ( heap_now != heap_prev ) || ( heapmin_now != heapmin_prev ) ||
            ( stack_now != stack_prev ) || ( usbstack_now != usbstack_prev ) ||
            ( tick_delta != (uint32_t)DEF_RTOS_TEST_DELAY_TICKS ) ||
            ( ms_delay_10ms < 10u ) || ( ms_delay_10ms > 11u ) || ( ms_delay_1000us > 2u ) )
        {
            printf( "[RTOS] ticks=%lu realMs=%lu tickHz=%lu heapFree=%lu heapMin=%lu stackFree=%lu usbStackFree=%lu delay10ms=%lu delay1000us=%lu\r\n",
                    (unsigned long)tick_delta,
                    (unsigned long)( ms_now - ms_before ),
                    (unsigned long)configTICK_RATE_HZ,
                    (unsigned long)heap_now,
                    (unsigned long)heapmin_now,
                    (unsigned long)stack_now,
                    (unsigned long)usbstack_now,
                    (unsigned long)ms_delay_10ms,
                    (unsigned long)ms_delay_1000us );

            heap_prev = heap_now;
            heapmin_prev = heapmin_now;
            stack_prev = stack_now;
            usbstack_prev = usbstack_now;
        }

        /* The emulated DS12887 clock is verified every 10 intervals (~20 s). The values are the
         * raw register bytes (BCD by default), the read path of src/rtc.c is exercised as well. */
        if( ( ++rtc_cnt % 10u ) == 0 )
        {
            printf( "[RTC] %02x.%02x.%02x %02x:%02x:%02x dow=%02x A=%02x B=%02x C=%02x D=%02x\r\n",
                    (unsigned)rtc_read( DS_REG_DAY_MONTH ), (unsigned)rtc_read( DS_REG_MONTH ),
                    (unsigned)rtc_read( DS_REG_YEAR ), (unsigned)rtc_read( DS_REG_HOUR ),
                    (unsigned)rtc_read( DS_REG_MIN ), (unsigned)rtc_read( DS_REG_SEC ),
                    (unsigned)rtc_read( DS_REG_DAY_WEEK ), (unsigned)rtc_read( DS_REG_A ),
                    (unsigned)rtc_read( DS_REG_B ), (unsigned)rtc_read( DS_REG_C ),
                    (unsigned)rtc_read( DS_REG_D ) );
        }
    }
}

#endif /* DEF_RTOS_TEST_TASK */

/*********************************************************************
 * @fn      vUsbHostTask
 *
 * @brief   USB host task: initializes the USBFS host port and then polls the USB host stack.
 *          The stop/restart policy with respect to the ATX power state is in src/app_usb.c
 *          (AppUsb_Step also runs one poll pass with the scheduler suspended, because the WCH
 *          host stack is a polling state machine which must not be disturbed in the middle of a
 *          transaction; the hardware interrupts stay enabled during that time).
 *
 * @param   pvParameters - not used.
 *
 * @return  none
 */
static void vUsbHostTask( void *pvParameters )
{
    AppUsb_Init( );

    printf( "[RTOS] USB host task started\r\n" );

    for( ;; )
    {
        AppUsb_Step( );

        vTaskDelay( DEF_RTOS_USB_DELAY_TICKS );
    }
}

/*********************************************************************
 * @fn      AppTasks_Start
 *
 * @brief   Creates the application tasks. It is called from main() before
 *          vTaskStartScheduler().
 *
 * @return  none
 */
void AppTasks_Start( void )
{
    BaseType_t s;

    printf( "[RTOS] kernel=%s\r\n", tskKERNEL_VERSION_NUMBER );

    /* The ATX pins and the button are initialized here, so the PSU is guaranteed to be off until
     * the power task switches it on (see src/power.c, src/button.c, src/app_power.c). The call is
     * not wrapped into an #if on purpose: AppPower_Init() guards DEF_POWER_EN / DEF_BUTTON_EN
     * internally, and a missing macro in this file would silently drop the initialization. */
    AppPower_Init( );

    s = xTaskCreate( vUsbHostTask, "usb_host", DEF_RTOS_USB_TASK_STACK_WORDS, NULL,
                     DEF_RTOS_USB_TASK_PRIO, &xUsbHostTaskHandle );
    printf( "[RTOS] usb host task: %s\r\n", ( s == pdPASS ) ? "created" : "FAILED" );

#if DEF_FPGA_CONFIG_EN || DEF_BUTTON_EN
    /* The power task waits DEF_FPGA_CONFIG_DELAY_MS (the USB host stack is up by then), switches
     * the PSU on, configures the FPGA and after that serves the push button. Unlike the previous
     * one-shot fpga_cfg task it stays alive. */
    s = xTaskCreate( AppPowerTask, "power", DEF_FPGA_CONFIG_STACK_WORDS, NULL,
                     DEF_FPGA_CONFIG_PRIO, NULL );
    printf( "[RTOS] power task: %s\r\n", ( s == pdPASS ) ? "created" : "FAILED" );
#endif

#if DEF_RTOS_TEST_TASK
    s = xTaskCreate( vRtosTestTask, "rtos_test", DEF_RTOS_TEST_STACK_WORDS, NULL,
                     DEF_RTOS_TEST_PRIO, NULL );
    printf( "[RTOS] test task: %s\r\n", ( s == pdPASS ) ? "created" : "FAILED" );
#endif

#if DEF_ZX_SPI_EN
    /* The ZX port service (the SPI link to the FPGA, see src/zx.c): it creates the SPI bus lock
     * and the service task. In the bare-metal mode main( ) calls zx_service( ) instead. */
    zx_task_start( );
#endif
}
