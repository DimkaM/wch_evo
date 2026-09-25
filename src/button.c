/********************************** (C) COPYRIGHT *******************************
* File Name          : button.c
* Version            : V1.0.0
* Description        : Push button driver with a time based debounce filter (src/button.h).
*                      The raw level is sampled by the caller (BTN_Update) and a new level is
*                      accepted only when it stays unchanged for DEF_BTN_DEBOUNCE_MS real
*                      milliseconds. The filter is independent of the poll rate, so the same
*                      code works in the FreeRTOS task and in the bare-metal super loop.
*******************************************************************************/
#include "usb_host_config.h"
#include "button.h"

/* 1 ms time base, incremented by TIM3_IRQHandler (src/USB_Host/app_km.c) */
extern volatile uint32_t g_ms_ticks;

/* Debounce filter state */
static uint8_t  s_raw        = DEF_BTN_LEVEL_UP;    /* last raw sample */
static uint32_t s_raw_ms     = 0;                   /* when the raw level changed */
static uint8_t  s_stable     = DEF_BTN_LEVEL_UP;    /* debounced level */
static uint32_t s_stable_ms  = 0;                   /* when the debounced level was confirmed */
static uint8_t  s_ready      = 0;                   /* 1 - at least one level was confirmed */
static uint32_t s_hold_ms    = 0;                   /* duration of the last confirmed press */

/*********************************************************************
 * @fn      BTN_Init
 *
 * @brief   Configures the button pin as an input with the internal pull-up (a backup for the
 *          external one).
 *
 * @return  none
 */
void BTN_Init( void )
{
    GPIO_InitTypeDef GPIO_InitStructure = { 0 };

    RCC_APB2PeriphClockCmd( RCC_APB2Periph_GPIOC, ENABLE );

    GPIO_InitStructure.GPIO_Pin = DEF_BTN_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init( DEF_BTN_PORT_CTRL, &GPIO_InitStructure );

    printf( "BTN: init (idle=%u)\r\n", (unsigned int)( GPIO_ReadInputDataBit( DEF_BTN_PORT_CTRL, DEF_BTN_PIN ) != 0 ) );
}

/*********************************************************************
 * @fn      BTN_IsReady
 *
 * @brief   A level was confirmed at least once, the state can be trusted.
 *
 * @return  1 - confirmed, 0 - not yet.
 */
uint8_t BTN_IsReady( void )
{
    return s_ready;
}

/*********************************************************************
 * @fn      BTN_IsDown
 *
 * @brief   Debounced state of the button.
 *
 * @return  1 - pressed, 0 - released.
 */
uint8_t BTN_IsDown( void )
{
    return ( s_stable == DEF_BTN_LEVEL_DOWN ) ? 1 : 0;
}

/*********************************************************************
 * @fn      BTN_HoldMs
 *
 * @brief   How long the button is held at this moment (measured from the confirmed press).
 *
 * @return  hold time in milliseconds, 0 when the button is released.
 */
uint32_t BTN_HoldMs( void )
{
    if( s_stable != DEF_BTN_LEVEL_DOWN )
    {
        return 0;
    }
    return g_ms_ticks - s_stable_ms;
}

/*********************************************************************
 * @fn      BTN_LastHoldMs
 *
 * @brief   Duration of the last completed press (valid after a BTN_EV_UP event).
 *
 * @return  duration in milliseconds.
 */
uint32_t BTN_LastHoldMs( void )
{
    return s_hold_ms;
}

/*********************************************************************
 * @fn      BTN_Update
 *
 * @brief   Debounce filter: a new level is accepted after it stayed unchanged for
 *          DEF_BTN_DEBOUNCE_MS. Both edges are reported once.
 *
 * @return  BTN_EV_DOWN on a confirmed press, BTN_EV_UP on a confirmed release, BTN_EV_NONE else.
 */
btn_event_t BTN_Update( void )
{
    uint8_t  raw = ( GPIO_ReadInputDataBit( DEF_BTN_PORT_CTRL, DEF_BTN_PIN ) != 0 ) ? DEF_BTN_LEVEL_UP : DEF_BTN_LEVEL_DOWN;
    uint32_t now = g_ms_ticks;

    if( raw != s_raw )                      /* bounce or noise: restart the stability window */
    {
        s_raw = raw;
        s_raw_ms = now;
        return BTN_EV_NONE;
    }

    if( ( now - s_raw_ms ) < DEF_BTN_DEBOUNCE_MS )
    {
        return BTN_EV_NONE;                 /* the current level is not stable yet */
    }

    if( s_ready == 0 )
    {
        /* The very first stable level defines the idle state and is not reported as an event, so a
         * button that is held while the board boots does not trigger anything. The application
         * arms itself on the first confirmed release after that. */
        s_stable = raw;
        s_stable_ms = now;
        s_ready = 1;
        return BTN_EV_NONE;
    }

    if( raw != s_stable )
    {
        if( raw == DEF_BTN_LEVEL_UP )
        {
            s_hold_ms = now - s_stable_ms;  /* length of the press that just ended */
            s_stable = raw;
            s_stable_ms = now;
            return BTN_EV_UP;
        }

        s_stable = raw;
        s_stable_ms = now;
        return BTN_EV_DOWN;
    }

    return BTN_EV_NONE;
}
