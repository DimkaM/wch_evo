/********************************** (C) COPYRIGHT *******************************
 * File Name          : spi.c
 * Version            : V1.0.0
 * Description        : SPI link to the FPGA (the register / wait port interface of the
 *                      ZX-Evolution base configuration). See spi.h and FPGA_SPI.md.
 *                      Ported from D:\src\pentevo\avr\baseconf\trunk\src\spi.c - the AVR code
 *                      (SPCR = mode 0, LSB first, master; spi_send( ) waits for SPIF) maps to
 *                      SPI1 in full remap with the same settings, see spi_init( ).
 *******************************************************************************/
#include "usb_host_config.h"
#include "spi.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_misc.h"

#if DEF_FREERTOS_EN
#include "FreeRTOS.h"
#include "semphr.h"
#endif

/*******************************************************************************/
/* Variable Definition */
static uint8_t  SpiReady = 0;               /* 1 - the link is initialised (spi_init done)   */
static uint32_t SpiTimeoutCount = 0;        /* number of timeouts (diagnostic)               */

#if DEF_FREERTOS_EN
static SemaphoreHandle_t SpiLock = NULL;    /* bus lock, see spi_lock( )                     */
#endif

/*********************************************************************
 * @fn      spi_send
 *
 * @brief   Exchanges one byte with the FPGA, exactly like spi_send( ) of the AVR project:
 *          write SPDR, wait for SPIF, read SPDR. Here it is SPI1: wait for TXE, write DATAR,
 *          wait for RXNE, read DATAR. The wait is bounded, so a missing peripheral (e.g. an
 *          unconfigured FPGA, whose outputs are tri-state, holds MISO at a fixed level) cannot
 *          hang the caller.
 *
 * @param   byte - the byte to send.
 *
 * @return  the received byte, 0xFF when the peripheral did not answer (timeout).
 */
uint8_t spi_send( uint8_t byte )
{
    uint32_t guard = 100000;

    while( ( SPI_I2S_GetFlagStatus( SPI1, SPI_I2S_FLAG_TXE ) == RESET ) && ( --guard != 0 ) )
    {
    }
    if( guard == 0 )
    {
        SpiTimeoutCount++;
        return 0xFF;
    }

    SPI_I2S_SendData( SPI1, (uint16_t)byte );

    guard = 100000;
    while( ( SPI_I2S_GetFlagStatus( SPI1, SPI_I2S_FLAG_RXNE ) == RESET ) && ( --guard != 0 ) )
    {
    }
    if( guard == 0 )
    {
        SpiTimeoutCount++;
        return 0xFF;
    }

    return (uint8_t)SPI_I2S_ReceiveData( SPI1 );
}

/*********************************************************************
 * @fn      Spi_GpioInit
 *
 * @brief   Configures the SPI1 pins after the full remap (SCK = PB3, MISO = PB4, MOSI = PB5),
 *          the nSPICS output and the spiint_n interrupt input. The FPGA configuration uses the
 *          same SCK/MOSI pins (the dedicated DCLK/DATA0 of the FPGA), the other two lines belong
 *          to the register protocol only.
 *
 * @return  none
 */
static void Spi_GpioInit( void )
{
    GPIO_InitTypeDef GPIO_InitStructure = { 0 };
    EXTI_InitTypeDef EXTI_InitStructure = { 0 };

    RCC_APB2PeriphClockCmd( RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO | RCC_APB2Periph_SPI1, ENABLE );

    /* full remap of SPI1, the same as the FPGA configuration does it (src/fpga.c) */
    GPIO_PinRemapConfig( GPIO_Remap_SPI1, ENABLE );

    /* SCK and MOSI as alternate function push-pull */
    GPIO_InitStructure.GPIO_Pin = SPI_SCK_PIN | SPI_MOSI_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init( SPI_PORT, &GPIO_InitStructure );

    /* MISO: the FPGA drives it (spidi), so the MCU pin is an input */
    GPIO_InitStructure.GPIO_Pin = SPI_MISO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init( SPI_PORT, &GPIO_InitStructure );

    /* nSPICS: idle high - the FPGA copies the status byte into its output shift register on
     * every edge of this line (see slave/slavespi.v) */
    GPIO_SetBits( nSPICS_PORT, nSPICS );
    GPIO_InitStructure.GPIO_Pin = nSPICS;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init( nSPICS_PORT, &GPIO_InitStructure );
    GPIO_SetBits( nSPICS_PORT, nSPICS );

    /* spiint_n: pulled up (the FPGA is tri-state before its configuration, then it pulls the
     * line low when the Z80 accesses a wait port), EXTI line 7, falling edge - the AVR used
     * INT6 with the same trigger */
    GPIO_InitStructure.GPIO_Pin = SPI_IRQ_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init( SPI_IRQ_PORT, &GPIO_InitStructure );

    GPIO_EXTILineConfig( GPIO_PortSourceGPIOB, SPI_IRQ_PIN_SOURCE );

    EXTI_InitStructure.EXTI_Line = SPI_IRQ_EXTI_LINE;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init( &EXTI_InitStructure );

    EXTI_ClearITPendingBit( SPI_IRQ_EXTI_LINE );
    NVIC_EnableIRQ( SPI_IRQ_IRQn );
}

/*********************************************************************
 * @fn      spi_init
 *
 * @brief   (Re)initialises the SPI link for the register protocol. Called by FPGA_Config( ) when
 *          the bitstream has been accepted: the configuration uses the very same SPI1, but with
 *          its own DMA setup and settings, so the peripheral is reset here.
 *
 * @return  none
 */
void spi_init( void )
{
    SPI_InitTypeDef SPI_InitStructure = { 0 };

    SpiReady = 0;

    Spi_GpioInit( );

    SPI_I2S_DeInit( SPI1 );

    /* Mode 0 (CPOL = 0, CPHA = 0) with the LSB first - exactly the SPCR = 0b01110000 / SPI2X = 1
     * of the AVR project, which is also what slave/slavespi.v expects. */
    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
    SPI_InitStructure.SPI_BaudRatePrescaler = DEF_ZX_SPI_PRESCALER;
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_LSB;
    SPI_InitStructure.SPI_CRCPolynomial = 7;
    SPI_Init( SPI1, &SPI_InitStructure );

    SPI_Cmd( SPI1, ENABLE );

    SpiReady = 1;
    SpiTimeoutCount = 0;

    /* SPI_BaudRatePrescaler_x is CR1.BR: 000 -> Fpclk/2, 001 -> /4, ... 011 -> /16 */
    printf( "SPI: link ready, SCK = %u kHz, CS = %u, IRQ = %u\r\n",
            (unsigned int)( SystemCoreClock / ( 2u << ( ( DEF_ZX_SPI_PRESCALER >> 3 ) & 0x07u ) ) / 1000u ),
            (unsigned int)GPIO_ReadOutputDataBit( nSPICS_PORT, nSPICS ),
            (unsigned int)GPIO_ReadInputDataBit( SPI_IRQ_PORT, SPI_IRQ_PIN ) );
}

/*********************************************************************
 * @fn      spi_ready
 *
 * @brief   Tells whether the link is initialised (the FPGA is configured and spi_init( ) has run).
 *          The ZX port service uses it, so the task of src/zx.c does nothing before the FPGA is
 *          configured.
 *
 * @return  1 - ready, 0 - not initialised.
 */
uint8_t spi_ready( void )
{
    return SpiReady;
}

/*********************************************************************
 * @fn      spi_deinit
 *
 * @brief   Prepares the link for a (re)configuration of the FPGA: the service interrupt is
 *          masked, the pending request is dropped (the counter of src/zx.c is reset there as
 *          well) and nSPICS is held high, so the FPGA sees no transaction while its
 *          configuration pins are in use. The SPI1 settings are then set by
 *          FPGA_Peripheral_Init( ).
 *
 * @return  none
 */
void spi_deinit( void )
{
    EXTI_InitTypeDef EXTI_InitStructure = { 0 };

    SpiReady = 0;

    NVIC_DisableIRQ( SPI_IRQ_IRQn );

    EXTI_InitStructure.EXTI_Line = SPI_IRQ_EXTI_LINE;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = DISABLE;
    EXTI_Init( &EXTI_InitStructure );
    EXTI_ClearITPendingBit( SPI_IRQ_EXTI_LINE );

    GPIO_SetBits( nSPICS_PORT, nSPICS );
}

/*********************************************************************
 * @fn      spi_lock_init
 *
 * @brief   Creates the bus lock. Called once from AppTasks_Start( ), i.e. before the scheduler is
 *          started, so no task can race for the lock. It is needed because the FPGA configuration
 *          task (src/app_power.c) and the ZX port service task (src/zx.c) share SPI1.
 *
 * @return  none
 */
void spi_lock_init( void )
{
#if DEF_FREERTOS_EN
    if( SpiLock == NULL )
    {
        SpiLock = xSemaphoreCreateMutex( );
    }
#endif
}

/*********************************************************************
 * @fn      spi_lock
 *
 * @brief   Takes the SPI1 bus lock. It is deliberately not taken inside zx_spi_send( ), because
 *          zx_wait_task( ) calls zx_spi_send( ) again (the AVR has the same call structure); the
 *          lock belongs to the service entry points.
 *
 * @return  none
 */
void spi_lock( void )
{
#if DEF_FREERTOS_EN
    if( SpiLock != NULL )
    {
        (void)xSemaphoreTake( SpiLock, portMAX_DELAY );
    }
#endif
}

/*********************************************************************
 * @fn      spi_unlock
 *
 * @brief   Releases the SPI1 bus lock (see spi_lock( )).
 *
 * @return  none
 */
void spi_unlock( void )
{
#if DEF_FREERTOS_EN
    if( SpiLock != NULL )
    {
        (void)xSemaphoreGive( SpiLock );
    }
#endif
}
