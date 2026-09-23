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

        printf( "[RTOS] ticks=%lu realMs=%lu tickHz=%lu heapFree=%lu heapMin=%lu stackFree=%lu usbStackFree=%lu delay10ms=%lu delay1000us=%lu\r\n",
                (unsigned long)tick_delta,
                (unsigned long)( ms_now - ms_before ),
                (unsigned long)configTICK_RATE_HZ,
                (unsigned long)xPortGetFreeHeapSize( ),
                (unsigned long)xPortGetMinimumEverFreeHeapSize( ),
                (unsigned long)uxTaskGetStackHighWaterMark( NULL ),
                (unsigned long)uxTaskGetStackHighWaterMark( xUsbHostTaskHandle ),
                (unsigned long)ms_delay_10ms,
                (unsigned long)ms_delay_1000us );
    }
}

#endif /* DEF_RTOS_TEST_TASK */

/*********************************************************************
 * @fn      vUsbHostTask
 *
 * @brief   USB host task: initializes the USBFS host port and then polls the USB host stack.
 *          One poll pass runs with the scheduler suspended, because the WCH host stack is
 *          a polling state machine which must not be disturbed in the middle of a
 *          transaction; the hardware interrupts stay enabled during that time.
 *
 * @param   pvParameters - not used.
 *
 * @return  none
 */
static void vUsbHostTask( void *pvParameters )
{


#if DEF_USBFS_PORT_EN
    printf( "USBFS Host Init\r\n" );
    USBFS_RCC_Init( );
    USBFS_Host_Init( ENABLE );
    memset( &RootHubDev[ DEF_USBFS_PORT_INDEX ].bStatus, 0, sizeof( ROOT_HUB_DEVICE ) );
    memset( &HostCtl[ DEF_USBFS_PORT_INDEX * DEF_ONE_USB_SUP_DEV_TOTAL ].InterfaceNum, 0, DEF_ONE_USB_SUP_DEV_TOTAL * sizeof( HOST_CTL ) );
#endif

    printf( "[RTOS] USB host task started\r\n" );

    for( ;; )
    {
        vTaskSuspendAll( );
        USBH_MainDeal( );
        xTaskResumeAll( );

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

    s = xTaskCreate( vUsbHostTask, "usb_host", DEF_RTOS_USB_TASK_STACK_WORDS, NULL,
                     DEF_RTOS_USB_TASK_PRIO, &xUsbHostTaskHandle );
    printf( "[RTOS] usb host task: %s\r\n", ( s == pdPASS ) ? "created" : "FAILED" );

#if DEF_RTOS_TEST_TASK
    s = xTaskCreate( vRtosTestTask, "rtos_test", DEF_RTOS_TEST_STACK_WORDS, NULL,
                     DEF_RTOS_TEST_PRIO, NULL );
    printf( "[RTOS] test task: %s\r\n", ( s == pdPASS ) ? "created" : "FAILED" );
#endif
}
