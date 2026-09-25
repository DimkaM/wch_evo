/********************************** (C) COPYRIGHT *******************************
* File Name          : app_usb.c
* Version            : V1.0.0
* Description        : USB host stack policy with respect to the ATX power state (see app_usb.h).
*                      The USB part may be supplied from the ATX main rails, therefore the stack
*                      can be stopped while the PSU is off and re-initialised when PWR_OK appears
*                      (DEF_USB_OFF_WHEN_PSU_OFF / DEF_USB_RESTART_ON_POWER_ON), and it can be
*                      restarted after an FPGA reconfiguration (DEF_USB_RESTART_ON_FPGA_CONFIG).
*                      Both the FreeRTOS task and the bare-metal super loop call AppUsb_Step().
*******************************************************************************/
#include "usb_host_config.h"
#include "app_usb.h"
#include "power.h"

#if DEF_FREERTOS_EN
#include "FreeRTOS.h"
#include "task.h"
#endif

#if DEF_USB_RESTART_DELAY_MS
#define USB_DelayMs( ms )   vTaskDelay( pdMS_TO_TICKS( ms ) )
#endif

static uint8_t s_stack_up = 0;              /* 1 - the host controller is initialised */
#if DEF_USB_RESTART_ON_POWER_ON
static uint8_t s_psu_prev = 0;              /* previous PWR_OK state, for the 0 -> 1 edge */
#endif
static volatile uint8_t s_restart_req = 0;  /* set by the power logic (src/app_power.c) */

/*********************************************************************
 * @fn      AppUsb_Init
 *
 * @brief   Initialises the host stack at startup. When DEF_USB_OFF_WHEN_PSU_OFF is enabled and the
 *          PSU is still off, the stack is left down until PWR_OK appears.
 *
 * @return  none
 */
void AppUsb_Init( void )
{
    uint8_t psu_on = ( POWER_IsGood( ) != 0 ) ? 1 : 0;

    s_restart_req = 0;

#if DEF_USB_OFF_WHEN_PSU_OFF
    if( psu_on == 0 )
    {
        printf( "USB: stack stopped (PSU off)\r\n" );   /* stays down until PWR_OK appears */
        return;
    }
#endif

    USBH_StackInit( );
    s_stack_up = 1;

#if DEF_USB_RESTART_ON_POWER_ON
    s_psu_prev = psu_on;
#endif

    printf( "USB: stack initialised (PSU %s)\r\n", ( psu_on != 0 ) ? "on" : "off" );
}

/*********************************************************************
 * @fn      AppUsb_RequestRestart
 *
 * @brief   Requests a stack restart, it is executed by AppUsb_Step() (single owner of the stack).
 *
 * @return  none
 */
void AppUsb_RequestRestart( void )
{
    s_restart_req = 1;
}

/*********************************************************************
 * @fn      AppUsb_Step
 *
 * @brief   One iteration of the USB service: holds the stack down while the PSU is off, starts it
 *          (or restarts on the PWR_OK 0 -> 1 edge) when the PSU is on, executes a pending restart
 *          request and finally polls the host stack. In the FreeRTOS mode the poll pass runs with
 *          the scheduler suspended, so a transaction is not interrupted by other tasks.
 *
 * @return  none
 */
void AppUsb_Step( void )
{
    uint8_t psu_on = ( POWER_IsGood( ) != 0 ) ? 1 : 0;
#if DEF_USB_OFF_WHEN_PSU_OFF
    uint8_t run = psu_on;                   /* the stack follows the PSU state */
#else
    uint8_t run = 1;                        /* the stack runs independently of the PSU */
    (void)psu_on;
#endif

    if( run == 0 )
    {
        if( s_stack_up != 0 )
        {
            USBH_StackDown( );
            s_stack_up = 0;
            s_restart_req = 0;
            printf( "USB: stack stopped (PSU off)\r\n" );
        }
        return;                             /* no polling while the USB part may be unpowered */
    }

    if( s_stack_up == 0 )
    {
        USBH_StackInit( );
        s_stack_up = 1;
        printf( "USB: stack started (PSU on)\r\n" );
#if DEF_USB_RESTART_ON_POWER_ON
        s_psu_prev = psu_on;                /* this initialisation is the restart itself */
#endif
    }

#if DEF_USB_RESTART_ON_POWER_ON
    if( ( psu_on != 0 ) && ( s_psu_prev == 0 ) )
    {
        printf( "USB: restart on power on\r\n" );
        USBH_StackInit( );
    }
    s_psu_prev = psu_on;
#endif

    if( s_restart_req != 0 )
    {
        s_restart_req = 0;
#if DEF_USB_RESTART_DELAY_MS
        USB_DelayMs( DEF_USB_RESTART_DELAY_MS );
#endif
        printf( "USB: restarted on request\r\n" );
        USBH_StackInit( );
    }

#if DEF_FREERTOS_EN
    vTaskSuspendAll( );
    USBH_MainDeal( );
    xTaskResumeAll( );
#else
    USBH_MainDeal( );
#endif
}
