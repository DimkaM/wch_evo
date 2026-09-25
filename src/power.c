/********************************** (C) COPYRIGHT *******************************
* File Name          : power.c
* Version            : V1.0.0
* Description        : ATX power supply control (see power.h and PINS.md).
*                      POWER_Init() puts the pins into the "PSU off" state, POWER_On()
*                      opens the NPN key (PS_ON# low) and waits for PWR_OK on POWER_GOOD.
*                      The FPGA is configured only when POWER_On() reports success.
*******************************************************************************/
#include "usb_host_config.h"
#include "power.h"

#if DEF_FREERTOS_EN
#include "FreeRTOS.h"
#include "task.h"
#endif

/* 1 ms time base, incremented by TIM3_IRQHandler (src/USB_Host/app_km.c) */
extern volatile uint32_t g_ms_ticks;

/* the two execution modes of the project, same helper as in src/fpga.c */
#if DEF_FREERTOS_EN
#define PWR_DelayMs( ms )   vTaskDelay( pdMS_TO_TICKS( ms ) )
#else
#define PWR_DelayMs( ms )   Delay_Ms( ms )
#endif

/*********************************************************************
 * @fn      POWER_Init
 *
 * @brief   Configures the ATX control pins and leaves the PSU off: POWER_ON is released
 *          (the NPN key is closed) and POWER_GOOD is a floating input, both levels are
 *          defined by the external 12k/20k divider.
 *
 * @return  none
 */
void POWER_Init( void )
{
    GPIO_InitTypeDef GPIO_InitStructure = { 0 };

    RCC_APB2PeriphClockCmd( RCC_APB2Periph_GPIOC, ENABLE );

    /* the output data bit is cleared first, so the key stays closed while the pin is
     * switched to the output mode (no PS_ON# glitch) */
    PWR_PIN_ON_RELEASE( );
    GPIO_InitStructure.GPIO_Pin = PWR_PIN_ON;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init( PWR_PORT_CTRL, &GPIO_InitStructure );

    /* POWER_GOOD: input with the internal pull-down. The board divider (12k/20k) gives ~3.1 V
     * when PWR_OK is present, while with the line disconnected (or the PSU absent) the pin is
     * pulled to 0 instead of floating - a floating input could be read as 1. */
    GPIO_InitStructure.GPIO_Pin = PWR_PIN_GOOD;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init( PWR_PORT_CTRL, &GPIO_InitStructure );

    printf( "PWR: init, PS_ON# released (PSU off)\r\n" );
}

/*********************************************************************
 * @fn      POWER_IsGood
 *
 * @brief   Current state of PWR_OK (POWER_GOOD input).
 *
 * @return  1 - the PSU reports power good, 0 - not.
 */
uint8_t POWER_IsGood( void )
{
    return ( GPIO_ReadInputDataBit( PWR_PORT_CTRL, PWR_PIN_GOOD ) != 0 ) ? 1 : 0;
}

/*********************************************************************
 * @fn      POWER_WaitGood
 *
 * @brief   Waits until the PSU reports PWR_OK, the timeout is DEF_POWER_GOOD_TIMEOUT_MS
 *          real milliseconds (measured with the 1 ms counter g_ms_ticks).
 *
 * @return  1 - POWER_GOOD became high, 0 - timeout.
 */
static uint8_t POWER_WaitGood( void )
{
    uint32_t t_start = g_ms_ticks;

    while( POWER_IsGood( ) == 0 )
    {
        if( ( g_ms_ticks - t_start ) > DEF_POWER_GOOD_TIMEOUT_MS )
        {
            printf( "PWR: POWER_GOOD timeout (%u ms)\r\n", (unsigned int)( g_ms_ticks - t_start ) );
            return 0;
        }
        PWR_DelayMs( 1 );
    }

    printf( "PWR: POWER_GOOD=1 (%u ms)\r\n", (unsigned int)( g_ms_ticks - t_start ) );
    return 1;
}

/*********************************************************************
 * @fn      POWER_WaitClear
 *
 * @brief   Waits until PWR_OK falls, i.e. the rails of a previous power on have discharged.
 *          Called after PS_ON# has been released, so the PSU is already switching off.
 *
 * @return  1 - PWR_OK became low, 0 - still high after DEF_POWER_OFF_CONFIRM_MS.
 */
static uint8_t POWER_WaitClear( void )
{
    uint32_t t_start = g_ms_ticks;

    while( POWER_IsGood( ) != 0 )
    {
        if( ( g_ms_ticks - t_start ) > DEF_POWER_OFF_CONFIRM_MS )
        {
            printf( "PWR: PWR_OK did not fall within %u ms\r\n", (unsigned int)DEF_POWER_OFF_CONFIRM_MS );
            return 0;
        }
        PWR_DelayMs( 1 );
    }

    printf( "PWR: rails discharged (PWR_OK=0)\r\n" );
    return 1;
}

/*********************************************************************
 * @fn      POWER_On
 *
 * @brief   Switches the PSU on (the NPN key pulls PS_ON# low) and waits for PWR_OK.
 *          If the PSU is still on from a previous run, it is switched off first and the rails are
 *          allowed to discharge, so the supplied FPGA gets a clean power-on instead of a marginal
 *          one. One power cycle retry is done when DEF_POWER_RETRY is enabled. On success the
 *          function returns after DEF_POWER_SETTLE_MS, so the rails and the FPGA power-on reset
 *          are stable when the configuration starts.
 *
 * @return  1 - POWER_GOOD is present, 0 - the PSU did not report power good.
 */
uint8_t POWER_On( void )
{
    uint8_t attempt;

    if( POWER_IsGood( ) != 0 )
    {
        printf( "PWR: PSU is on, doing a clean power cycle\r\n" );
        PWR_PIN_ON_RELEASE( );              /* switch the PSU off first */
        POWER_WaitClear( );                 /* best effort, a timeout is reported in the log */
    }

    for( attempt = 0; ; attempt++ )
    {
        PWR_PIN_ON_ASSERT( );
        printf( "PWR: POWER_ON asserted (attempt %u)\r\n", (unsigned int)( attempt + 1 ) );

        if( POWER_WaitGood( ) != 0 )
        {
            PWR_DelayMs( DEF_POWER_SETTLE_MS );
            return 1;
        }

#if DEF_POWER_RETRY
        if( attempt == 0 )
        {
            printf( "PWR: retry (PSU power cycle)\r\n" );
            PWR_PIN_ON_RELEASE( );
            PWR_DelayMs( 200 );
            continue;
        }
#endif
        return 0;
    }
}

/*********************************************************************
 * @fn      POWER_Off
 *
 * @brief   Releases PS_ON# (the key is closed), the PSU turns off.
 *
 * @return  none
 */
void POWER_Off( void )
{
    PWR_PIN_ON_RELEASE( );
    printf( "PWR: PS_ON# released (PSU off)\r\n" );
}
