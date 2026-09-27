/********************************** (C) COPYRIGHT *******************************
* File Name          : app_power.c
* Version            : V1.0.0
* Description        : ATX power, FPGA configuration and push button sequence (see app_power.h).
*                      This module is the single owner of POWER_*() and FPGA_Config():
*                        startup : wait for the USB stack -> POWER_On() -> FPGA_Config()
*                        PSU off : a button press switches the PSU on and configures the FPGA
*                        PSU on  : a short press resets the Z80 only (SPI_RST_REG - the SOFTRES
*                                  button of the AVR, atx.c:119-123), a long press (> 3 s) switches
*                                  the PSU off
*                      The FPGA reconfiguration (FLAG_HARD_RESET of the AVR) is requested by
*                      Ctrl+Alt+Del through AppPower_RequestConfig( ).
*                      The button itself (pin, pull-up, debounce) is in src/button.c.
*******************************************************************************/
#include "usb_host_config.h"
#include "power.h"
#include "fpga.h"
#include "button.h"
#include "app_usb.h"
#include "app_power.h"
#include "zx.h"                                     /* zx_request_reset( ) - the soft reset of the Z80 */
#include "app_km.h"                                 /* g_usbHidKbReady - F12 comes from the keyboard  */

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

/* F12 - the second "SOFTRES key" of the AVR project (interrupts.c:151-162 feeds it into the same
 * atx_counter as the button). The keyboard layer only reports its level, the hold time and the
 * actions are handled by AppPower_Step( ) together with the button. */
static uint8_t  s_f12_level    = 0;     /* the level last reported by the keyboard layer */
static uint8_t  s_f12_down     = 0;     /* its tracked state */
static uint32_t s_f12_ms       = 0;     /* when the key went down */
static uint8_t  s_f12_suppress = 0;     /* the running press has already been handled (long) */

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
 * @fn      AppPower_KeyF12
 *
 * @brief   Level of the F12 key of the USB keyboard. In the AVR project F12 repeats the SOFTRES
 *          button (interrupts.c:151-162: both feed the same atx_counter), so a short press is the
 *          soft reset of the Z80 and a long press switches the PSU off. The keyboard layer calls
 *          this from the USB report path, which runs with the scheduler suspended, so only the
 *          level is stored here - the actions are taken by AppPower_Step( ).
 *
 * @param   on - 0: the key is released, any other value: it is held.
 *
 * @return  none
 */
void AppPower_KeyF12( uint8_t on )
{
    s_f12_level = ( on != 0 ) ? 1 : 0;
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
        /* A short press is the soft reset of the Z80 (atx.c:119-123 of the AVR project: SOFTRES and
         * F12 feed the same atx_counter there): only the Z80 is reset, the FPGA keeps its
         * configuration and the memory of the ZX survives. The full restart - the reconfiguration of
         * the FPGA, FLAG_HARD_RESET in the AVR - is reachable through Ctrl+Alt+Del now. */
        if( ( locked == 0 ) && ( s_suppress == 0 ) && ( BTN_LastHoldMs( ) < DEF_BTN_LONG_MS ) &&
            ( POWER_IsGood( ) != 0 ) )
        {
            printf( "BTN: short press (%u ms) -> Z80 reset\r\n", (unsigned int)BTN_LastHoldMs( ) );
            zx_request_reset( );
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

#if DEF_BTN_F12_EN
    /* F12 - the very same policy as the button above, because the AVR treats them as one key
     * (atx.c: the SOFTRES pin and the F12 flag both increment atx_counter). A keyboard which
     * disappeared while the key was held would leave the level stuck, so it is dropped with the
     * keyboard. */
    {
        uint8_t level = ( g_usbHidKbReady != 0 ) ? s_f12_level : 0;

        if( level == 0 )
        {
            s_f12_level = 0;
        }

        if( level != s_f12_down )
        {
            if( level != 0 )
            {
                s_f12_down     = 1;
                s_f12_ms       = g_ms_ticks;
                s_f12_suppress = 0;
                printf( "F12: pressed (hold >= %u ms = PSU off)\r\n", (unsigned int)DEF_BTN_LONG_MS );
            }
            else
            {
                uint32_t hold     = g_ms_ticks - s_f12_ms;
                uint8_t  suppress = s_f12_suppress;

                s_f12_down     = 0;
                s_f12_suppress = 0;

                /* released before the threshold: the soft reset of the Z80 (atx.c:119-123) */
                if( ( suppress == 0 ) && ( hold < DEF_BTN_LONG_MS ) && ( POWER_IsGood( ) != 0 ) )
                {
                    printf( "F12: short press (%u ms) -> Z80 reset\r\n", (unsigned int)hold );
                    zx_request_reset( );
                }
            }
        }
#if DEF_POWER_EN
        else if( ( s_f12_down != 0 ) && ( s_f12_suppress == 0 ) && ( POWER_IsGood( ) != 0 ) &&
                 ( ( g_ms_ticks - s_f12_ms ) >= DEF_BTN_LONG_MS ) )
        {
            /* held longer than the threshold: switch the PSU off right away (atx.c:80-118) */
            printf( "F12: long press (%u ms) -> PSU off\r\n", (unsigned int)DEF_BTN_LONG_MS );
            POWER_Off( );
            s_f12_suppress  = 1;
            s_lockout_until = g_ms_ticks + DEF_PWR_OFF_LOCKOUT_MS;
        }
#endif
    }
#endif /* DEF_BTN_F12_EN */
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
