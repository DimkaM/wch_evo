/********************************** (C) COPYRIGHT *******************************
* File Name          : app_power.c
* Version            : V1.0.0
* Description        : ATX power, FPGA configuration and push button sequence (see app_power.h).
*                      This module is the single owner of POWER_*() and FPGA_Config():
*                        startup : wait for the USB stack -> POWER_On() -> FPGA_Config()
*                        PSU off : a button press switches the PSU on and configures the FPGA
*                        PSU on  : a short press re-configures the FPGA, a long press (> 3 s)
*                                  switches the PSU off
*                      The button itself (pin, pull-up, debounce) is in src/button.c.
*******************************************************************************/
#include "usb_host_config.h"
#include "power.h"
#include "fpga.h"
#include "button.h"
#include "app_usb.h"
#include "app_power.h"

#if DEF_FREERTOS_EN
#include "FreeRTOS.h"
#include "task.h"
#endif

/* 1 ms time base, incremented by TIM3_IRQHandler (src/USB_Host/app_km.c) */
extern volatile uint32_t g_ms_ticks;

/* State of the button service */
static uint8_t  s_armed = 0;            /* 1 - the button was confirmed released at least once */
static uint8_t  s_suppress = 0;         /* the running press has already been handled */
static uint32_t s_lockout_until = 0;    /* button actions are on hold until this time */

/* the two execution modes of the project, same helper as in src/fpga.c */
#if DEF_FREERTOS_EN
#define APPPWR_DelayMs( ms )    vTaskDelay( pdMS_TO_TICKS( ms ) )
#else
#define APPPWR_DelayMs( ms )    Delay_Ms( ms )
#endif

/*********************************************************************
 * @fn      AppPower_ConfigFpga
 *
 * @brief   Runs the FPGA configuration. A failed attempt is repeated (DEF_FPGA_CONFIG_RETRY),
 *          because the first one may happen while the local regulators of the FPGA board are still
 *          ramping. When DEF_USB_RESTART_ON_FPGA_CONFIG is enabled, a USB stack restart is asked
 *          for after a successful configuration (the reconfiguration may disturb the USB part).
 *
 * @param   reason - short text for the log ("startup", "button").
 *
 * @return  none
 */
static void AppPower_ConfigFpga( const char *reason )
{
    uint8_t attempt;

    for( attempt = 0; ; attempt++ )
    {
        if( FPGA_Config( ) != 0 )
        {
#if DEF_USB_RESTART_ON_FPGA_CONFIG
            AppUsb_RequestRestart( );
#endif
            return;
        }

        printf( "FPGA: configuration (%s) FAILED (attempt %u of %u)\r\n",
                reason, (unsigned int)( attempt + 1 ), (unsigned int)( DEF_FPGA_CONFIG_RETRY + 1 ) );

        if( attempt >= DEF_FPGA_CONFIG_RETRY )
        {
            return;
        }

        APPPWR_DelayMs( DEF_FPGA_CONFIG_RETRY_MS );
    }
}

/*********************************************************************
 * @fn      AppPower_Init
 *
 * @brief   Configures the ATX control pins and the button, the PSU stays off afterwards.
 *
 * @return  none
 */
void AppPower_Init( void )
{
#if DEF_POWER_EN
    POWER_Init( );
#endif
#if DEF_BUTTON_EN
    BTN_Init( );
#endif
}

/*********************************************************************
 * @fn      AppPower_Startup
 *
 * @brief   Switches the PSU on and configures the FPGA. The caller takes care of the delay after
 *          the start (the USB host stack has to be up first).
 *
 * @return  none
 */
void AppPower_Startup( void )
{
#if DEF_POWER_EN
    /* the FPGA and its configuration pins are supplied by the PSU */
    if( POWER_On( ) == 0 )
    {
        printf( "PWR: no POWER_GOOD, startup aborted\r\n" );
        return;
    }
#endif

#if DEF_FPGA_CONFIG_EN
    printf( "FPGA: start (usbRoot=%u usbHidKb=%u)\r\n",
            (unsigned int)g_usbRootReady, (unsigned int)g_usbHidKbReady );
    AppPower_ConfigFpga( "startup" );
#endif
}

/*********************************************************************
 * @fn      AppPower_Step
 *
 * @brief   One iteration of the button service. Call it every DEF_BTN_POLL_MS (or faster) from the
 *          FreeRTOS power task or from the bare-metal super loop.
 *
 * @return  none
 */
void AppPower_Step( void )
{
#if DEF_PINS_DEBUG
    static uint32_t s_pins_dbg_ms = 0;

    if( ( g_ms_ticks - s_pins_dbg_ms ) >= DEF_PINS_DEBUG_MS )
    {
        s_pins_dbg_ms = g_ms_ticks;

        /* "drv" is what the MCU drives on PC0, "pin" is what the pin really reads back: a
         * difference means the pin is loaded or shorted (wiring problem). The raw registers are
         * printed as well, so the values can be compared with a multimeter on the pins. */
        printf( "PINS: CFGLR=%08x OUTDR=%08x INDR=%08x | PC0 drv=%u pin=%u | PC1(ok)=%u | PC2(btn)=%u | PSU=%u BTN=%u\r\n",
                (unsigned int)GPIOC->CFGLR, (unsigned int)GPIOC->OUTDR, (unsigned int)GPIOC->INDR,
                (unsigned int)( GPIO_ReadOutputDataBit( DEF_BTN_PORT_CTRL, GPIO_Pin_0 ) != 0 ),
                (unsigned int)( GPIO_ReadInputDataBit( DEF_BTN_PORT_CTRL, GPIO_Pin_0 ) != 0 ),
                (unsigned int)( GPIO_ReadInputDataBit( DEF_BTN_PORT_CTRL, GPIO_Pin_1 ) != 0 ),
                (unsigned int)( GPIO_ReadInputDataBit( DEF_BTN_PORT_CTRL, DEF_BTN_PIN ) != 0 ),
                (unsigned int)POWER_IsGood( ),
                (unsigned int)BTN_IsDown( ) );
    }
#endif

#if DEF_BUTTON_EN
    btn_event_t ev = BTN_Update( );
    uint8_t locked;

    /* Do not act before the button was confirmed released once: a button that is held while the
     * board boots must not trigger anything. */
    if( s_armed == 0 )
    {
        if( ( BTN_IsReady( ) != 0 ) && ( BTN_IsDown( ) == 0 ) )
        {
            s_armed = 1;
            printf( "BTN: armed\r\n" );
        }
        return;
    }

    /* After a power off the button is ignored while PWR_OK is still high (rails discharge). */
    locked = ( ( g_ms_ticks < s_lockout_until ) && ( POWER_IsGood( ) != 0 ) ) ? 1 : 0;

    if( ev == BTN_EV_UP )
    {
        if( ( locked == 0 ) && ( s_suppress == 0 ) && ( BTN_LastHoldMs( ) < DEF_BTN_LONG_MS ) &&
            ( POWER_IsGood( ) != 0 ) )
        {
            printf( "BTN: short press (%u ms) -> FPGA reconfiguration\r\n", (unsigned int)BTN_LastHoldMs( ) );
            AppPower_ConfigFpga( "button" );
        }
        s_suppress = 0;
    }
    else if( ( locked == 0 ) && ( ev == BTN_EV_DOWN ) )
    {
        s_suppress = 0;

#if DEF_POWER_EN
        if( POWER_IsGood( ) == 0 )
        {
            /* the PSU is off: switch it on right away, without waiting for the release */
            printf( "BTN: pressed, PSU off -> power on\r\n" );
            if( POWER_On( ) != 0 )
            {
                AppPower_ConfigFpga( "button" );
            }
            s_suppress = 1;
        }
        else
        {
            printf( "BTN: pressed, PSU on (hold >= %u ms = long press)\r\n", (unsigned int)DEF_BTN_LONG_MS );
        }
#endif
    }
#if DEF_POWER_EN
    else if( ( locked == 0 ) && ( s_suppress == 0 ) && ( BTN_IsDown( ) != 0 ) &&
             ( POWER_IsGood( ) != 0 ) && ( BTN_HoldMs( ) >= DEF_BTN_LONG_MS ) )
    {
        /* long press: switch the PSU off right away, without waiting for the release */
        printf( "BTN: long press (%u ms) -> PSU off\r\n", (unsigned int)BTN_HoldMs( ) );
        POWER_Off( );
        s_suppress = 1;
        s_lockout_until = g_ms_ticks + DEF_PWR_OFF_LOCKOUT_MS;
    }
#endif
#endif /* DEF_BUTTON_EN */
}

#if DEF_FREERTOS_EN
/*********************************************************************
 * @fn      AppPowerTask
 *
 * @brief   Startup sequence (optional) and then the button service. Unlike the previous one-shot
 *          FPGA_ConfigTask this task never deletes itself.
 *
 * @param   pvParameters - not used.
 *
 * @return  none
 */
void AppPowerTask( void *pvParameters )
{
#if DEF_POWER_AUTO_ON
    printf( "PWR: startup in %u ms\r\n", (unsigned int)DEF_FPGA_CONFIG_DELAY_MS );
    APPPWR_DelayMs( DEF_FPGA_CONFIG_DELAY_MS );

    AppPower_Startup( );
#endif

    for( ;; )
    {
        AppPower_Step( );
        APPPWR_DelayMs( DEF_BTN_POLL_MS );
    }
}
#endif /* DEF_FREERTOS_EN */
