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

    for( ;; )
    {
        ms_before = g_ms_ticks;
        tick_before = (uint32_t)xTaskGetTickCount( );

        vTaskDelay( DEF_RTOS_TEST_DELAY_TICKS );

        ms_now = g_ms_ticks;
        tick_delta = (uint32_t)xTaskGetTickCount( ) - tick_before;

        printf( "[RTOS] ticks=%lu realMs=%lu tickHz=%lu heapFree=%lu stackFree=%lu\r\n",
                (unsigned long)tick_delta,
                (unsigned long)( ms_now - ms_before ),
                (unsigned long)configTICK_RATE_HZ,
                (unsigned long)xPortGetFreeHeapSize( ),
                (unsigned long)uxTaskGetStackHighWaterMark( NULL ) );
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

    s = xTaskCreate( vRtosTestTask, "rtos_test", DEF_RTOS_TEST_STACK_WORDS, NULL,
                     DEF_RTOS_TEST_PRIO, NULL );

    printf( "[RTOS] kernel=%s, test task: %s\r\n",
            tskKERNEL_VERSION_NUMBER, ( s == pdPASS ) ? "created" : "FAILED" );
}
