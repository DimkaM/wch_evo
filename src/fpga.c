/********************************** (C) COPYRIGHT *******************************
* File Name          : fpga.c
* Version            : V1.0.0
* Description        : Altera FPGA configuration over SPI1 (full remap) + DMA1_Channel3.
*                      The hardware sequence is ported from the working project
*                      D:\src\new_evo\test_altera (src/fpga.c) without changing it:
*                        nCONFIG low -> nCONFIG high -> wait nSTATUS -> stream the RBF
*                        through the DMA -> wait until SPI is idle -> check CONF_DONE ->
*                        finalisation: SCK/MOSI as GPIO, DATA0 high, 20 DCLK pulses.
*                      The bitstream itself is in src/rbf.h (98023 bytes, const, in flash).
*******************************************************************************/
#include "usb_host_config.h"
#include "fpga.h"
#include "rbf.h"

#if DEF_FREERTOS_EN
#include "FreeRTOS.h"
#include "task.h"
#endif

/*******************************************************************************/
/* Macros */
#define SET_NCONFIG()      GPIO_SetBits( FPGA_PORT_CTRL, FPGA_PIN_NCONFIG )
#define CLR_NCONFIG()      GPIO_ResetBits( FPGA_PORT_CTRL, FPGA_PIN_NCONFIG )
#define GET_NSTATUS()      GPIO_ReadInputDataBit( FPGA_PORT_CTRL, FPGA_PIN_NSTATUS )
#define GET_CONF_DONE()    GPIO_ReadInputDataBit( FPGA_PORT_CTRL, FPGA_PIN_CONF_DONE )

/* The configuration runs from a task in the FreeRTOS mode, in the bare-metal mode from the
 * main super loop - the two helpers hide the difference. */
#if DEF_FREERTOS_EN
#define FPGA_DelayMs( ms )  vTaskDelay( pdMS_TO_TICKS( ms ) )
#define FPGA_Yield()        taskYIELD()
#else
#define FPGA_DelayMs( ms )  Delay_Ms( ms )
#define FPGA_Yield()        do { } while( 0 )
#endif

/*********************************************************************
 * @fn      FPGA_Peripheral_Init
 *
 * @brief   Configures the FPGA control pins (port A), SPI1 in full remap (PB3/PB5) and the
 *          DMA1 channel 3 used for the SPI1 transmit. Taken from the working project.
 *
 * @return  none
 */
static void FPGA_Peripheral_Init( void )
{
    GPIO_InitTypeDef GPIO_InitStructure = { 0 };
    SPI_InitTypeDef  SPI_InitStructure = { 0 };
    DMA_InitTypeDef  DMA_InitStructure = { 0 };

    /* GPIOA / GPIOB / AFIO / SPI1 and DMA1 clocks */
    RCC_APB2PeriphClockCmd( RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
                            RCC_APB2Periph_AFIO | RCC_APB2Periph_SPI1, ENABLE );
    RCC_AHBPeriphClockCmd( RCC_AHBPeriph_DMA1, ENABLE );

    /* Full remap of SPI1: SCK = PB3, MOSI = PB5 (the JTAG pins PA13/PA14 used by the
     * debugger are not touched). */
    GPIO_PinRemapConfig( GPIO_Remap_SPI1, ENABLE );

    /* nCONFIG output */
    GPIO_InitStructure.GPIO_Pin = FPGA_PIN_NCONFIG;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init( FPGA_PORT_CTRL, &GPIO_InitStructure );

    /* nSTATUS and CONF_DONE inputs */
    GPIO_InitStructure.GPIO_Pin = FPGA_PIN_NSTATUS | FPGA_PIN_CONF_DONE;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init( FPGA_PORT_CTRL, &GPIO_InitStructure );

    /* SCK and MOSI as alternate function push-pull */
    GPIO_InitStructure.GPIO_Pin = FPGA_PIN_SPI_SCK | FPGA_PIN_SPI_MOSI;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init( FPGA_PORT_SPI, &GPIO_InitStructure );

    /* SPI1 master: the RBF is transmitted least significant bit first */
    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_16;   /* 96/16 = 6 MHz */
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_LSB;
    SPI_Init( SPI1, &SPI_InitStructure );

    SPI_I2S_DMACmd( SPI1, SPI_I2S_DMAReq_Tx, ENABLE );
    SPI_Cmd( SPI1, ENABLE );

    /* DMA1 channel 3 = SPI1 TX */
    DMA_DeInit( DMA1_Channel3 );
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&( SPI1->DATAR );
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
    DMA_InitStructure.DMA_Priority = DMA_Priority_High;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    DMA_Init( DMA1_Channel3, &DMA_InitStructure );

    SET_NCONFIG();
}

/*********************************************************************
 * @fn      FPGA_DMA_StartBlock
 *
 * @brief   Starts one DMA block transfer to SPI1.
 *
 * @param   buf_ptr - pointer to the block inside the flash (no copy is made).
 *          size - number of bytes of the block.
 *
 * @return  none
 */
static void FPGA_DMA_StartBlock( uint8_t *buf_ptr, uint16_t size )
{
    DMA_Cmd( DMA1_Channel3, DISABLE );
    DMA1_Channel3->MADDR = (uint32_t)buf_ptr;
    DMA1_Channel3->CNTR = size;
    DMA_ClearFlag( DMA1_FLAG_TC3 );
    DMA_Cmd( DMA1_Channel3, ENABLE );
}

/*********************************************************************
 * @fn      FPGA_FinishConfig
 *
 * @brief   Finalisation of the Altera passive serial configuration: SPI is stopped, SCK and
 *          MOSI become plain GPIO outputs, DATA0 (MOSI) is driven high and 20 DCLK pulses
 *          are generated in software. Taken from the working project.
 *
 * @return  none
 */
static void FPGA_FinishConfig( void )
{
    GPIO_InitTypeDef GPIO_InitStructure = { 0 };
    uint8_t i;

    SPI_Cmd( SPI1, DISABLE );

    GPIO_InitStructure.GPIO_Pin = FPGA_PIN_SPI_SCK | FPGA_PIN_SPI_MOSI;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init( FPGA_PORT_SPI, &GPIO_InitStructure );

    GPIO_SetBits( FPGA_PORT_SPI, FPGA_PIN_SPI_MOSI );

    for( i = 0; i < 20; i++ )
    {
        GPIO_SetBits( FPGA_PORT_SPI, FPGA_PIN_SPI_SCK );
        GPIO_ResetBits( FPGA_PORT_SPI, FPGA_PIN_SPI_SCK );
    }
}

/*********************************************************************
 * @fn      FPGA_Config
 *
 * @brief   Configures the FPGA once (blocking). The bitstream is streamed from the flash
 *          through SPI1 + DMA, exactly like in the working project.
 *
 * @return  1 - the FPGA accepted the configuration (CONF_DONE = 1), 0 - failed.
 */
uint8_t FPGA_Config( void )
{
    uint32_t bytes_remaining = sizeof( fpga_rbf_data );
    const uint8_t *data_ptr = fpga_rbf_data;
    uint32_t block_size;
    uint32_t sent = 0;
    uint32_t t_start;

    FPGA_Peripheral_Init( );

    /* Diagnostic: the idle levels tell whether the FPGA is powered and wired at all. With the
     * device unpowered (or nSTATUS not connected) nSTATUS stays low and the configuration can
     * never start. */
    printf( "FPGA: idle levels nSTATUS=%u CONF_DONE=%u\r\n",
            (unsigned int)GET_NSTATUS( ), (unsigned int)GET_CONF_DONE( ) );

    /* 1. Altera reset sequence: nCONFIG low for at least 2 us, then release it */
    CLR_NCONFIG( );
    FPGA_DelayMs( 2 );
    SET_NCONFIG( );

    printf( "FPGA: nCONFIG released, nSTATUS=%u\r\n", (unsigned int)GET_NSTATUS( ) );

    /* 2. The FPGA reports its readiness on nSTATUS */
    t_start = g_ms_ticks;
    while( GET_NSTATUS( ) == 0 )
    {
        if( ( g_ms_ticks - t_start ) > DEF_FPGA_CONFIG_NSTATUS_MS )
        {
            printf( "FPGA: nSTATUS timeout (%u ms)\r\n", (unsigned int)( g_ms_ticks - t_start ) );
            return 0;
        }
        FPGA_DelayMs( 2 );
    }
    printf( "FPGA: nSTATUS OK, sending %u B\r\n", (unsigned int)bytes_remaining );

    /* 3. Stream the bitstream block by block, the DMA feeds SPI1 from the flash */
    t_start = g_ms_ticks;
    while( bytes_remaining > 0 )
    {
        block_size = ( bytes_remaining > DEF_FPGA_CONFIG_BLOCK_SIZE ) ? DEF_FPGA_CONFIG_BLOCK_SIZE : bytes_remaining;

        FPGA_DMA_StartBlock( (uint8_t *)data_ptr, (uint16_t)block_size );

        data_ptr += block_size;
        bytes_remaining -= block_size;
        sent += block_size;

        while( DMA_GetFlagStatus( DMA1_FLAG_TC3 ) == RESET )
        {
            FPGA_Yield( );
        }

        /* the FPGA signals a problem while receiving (nSTATUS is pulled low) */
        if( GET_NSTATUS( ) == 0 )
        {
            printf( "FPGA: nSTATUS lost after %u B\r\n", (unsigned int)sent );
            return 0;
        }
    }

    /* 4. Let the SPI shift register push out the last bits of the final byte */
    while( SPI_I2S_GetFlagStatus( SPI1, SPI_I2S_FLAG_BSY ) == SET )
    {
    }

    printf( "FPGA: sent %u B in %u ms\r\n", (unsigned int)sent, (unsigned int)( g_ms_ticks - t_start ) );

    /* 5. The FPGA drives CONF_DONE high when the configuration was accepted */
    t_start = g_ms_ticks;
    while( GET_CONF_DONE( ) == 0 )
    {
        if( ( g_ms_ticks - t_start ) > DEF_FPGA_CONFIG_CONFDONE_MS )
        {
            printf( "FPGA: CONF_DONE timeout\r\n" );
            return 0;
        }
        FPGA_DelayMs( 2 );
    }

    FPGA_FinishConfig( );
    printf( "FPGA: CONF_DONE=1, FPGA configured\r\n" );

    return 1;
}

