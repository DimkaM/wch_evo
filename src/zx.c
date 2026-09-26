/********************************** (C) COPYRIGHT *******************************
 * File Name          : zx.c
 * Version            : V1.0.0
 * Description        : Interchange with the FPGA registers (ZX ports) and the wait port service.
 *                      Ported from D:\src\pentevo\avr\baseconf\trunk\src:
 *                        zx.c        - zx_spi_send( ), zx_init( ), zx_wait_task( );
 *                        main.c      - the "event from SPI" block of the super loop;
 *                        interrupts.c- ISR(INT6_vect) -> flags_register |= FLAG_SPI_INT.
 *                      The AVR serves the event in its main loop; here the interrupt only sets the
 *                      flag and notifies the ZX task, which runs the very same sequence. SPI1 is
 *                      shared with the FPGA configuration task, so the service takes
 *                      spi_lock( ) around the exchange.
 *******************************************************************************/
#include "usb_host_config.h"
#include "zx.h"
#include "spi.h"
#include "rtc.h"

#if DEF_FREERTOS_EN
#include "FreeRTOS.h"
#include "task.h"
#endif

/*******************************************************************************/
/* Variable Definition */
volatile uint8_t flags_register;            /* the common flag register of the AVR main.h      */
volatile uint8_t flags_ex_register;         /* the extension flag register (the AVR main.h)    */
volatile uint8_t modes_register;            /* the ZX modes (the AVR main.h); the low three bits are also
                                             * the LED byte of the keyboard (VGA/tapeout/Caps) */

static uint8_t  ZxReady = 0;                /* 1 - zx_init( ) has run (the FPGA is configured) */
static volatile uint8_t ZxConfigPending = 0;/* the modes changed and have to be sent to the FPGA */
static volatile uint32_t ZxIntCount = 0;    /* serviced requests (diagnostic)                 */
static uint32_t ZxSpuriousCount = 0;        /* interrupts without a wait port index           */

/*********************************************************************
 * @fn      zx_init
 *
 * @brief   Initialises the ZX part after the FPGA has been configured, mirroring zx_init( ) of the
 *          AVR: the SPI link is initialised (spi_init( )), the Z80 is reset through the FPGA and
 *          the Gluk clock registers are prepared. The AVR also calls zx_task(ZX_TASK_INIT) here,
 *          which belongs to the PS/2 keyboard layer and is not ported yet.
 *
 * @return  none
 */
void zx_init( void )
{
    ZxReady = 0;

#if DEF_ZX_SPI_EN
    spi_lock( );

    spi_init( );

    /* the reset register (0x30) pulses the Z80 reset line in the FPGA */
    zx_spi_send( SPI_RST_REG, 0, 0 );

#if DEF_ZX_GLUK_EN
    gluk_init( );
#endif

    flags_register &= (uint8_t)~( FLAG_SPI_INT );
    ZxReady = 1;

    spi_unlock( );

    /* The initial mode goes to the FPGA as well (the AVR sends it on every mode change, and the
     * video/tv mode has to be known to the FPGA from the start). */
    zx_set_config( 0 );

#if DEF_ZX_SPI_DEBUG
    printf( "ZX: init done, reset pulse sent, gluk regs %02x %02x %02x %02x\r\n",
            (unsigned int)gluk_get_reg( GLUK_REG_SEC ), (unsigned int)gluk_get_reg( GLUK_REG_MIN ),
            (unsigned int)gluk_get_reg( GLUK_REG_HOUR ), (unsigned int)gluk_get_reg( GLUK_REG_DAY_MONTH ) );
#endif
#endif
}

/*********************************************************************
 * @fn      zx_spi_send
 *
 * @brief   Exchanges one FPGA register, byte for byte like zx_spi_send( ) of the AVR project.
 *          The transaction (see slave/slavespi.v and FPGA_SPI.md):
 *
 *            CS low / CS high - the FPGA copies the status byte into its output shift register;
 *            spi_send( addr ) - CS is high: the status byte is shifted out (bit 7 = the Z80 was
 *                               reading, bits 6..0 = the index of the wait port), the address is
 *                               shifted into the register number of the FPGA;
 *            CS low           - the FPGA copies the addressed register into the shift register;
 *            spi_send( data ) - CS is low: the register value is received, the new value is
 *                               shifted in;
 *            CS high          - the FPGA performs the write (register strobes, wait_end, ...).
 *
 *          The SPI bus lock is NOT taken here on purpose: zx_wait_task( ) calls this function
 *          again, exactly as in the AVR; the lock belongs to the service entry points.
 *
 * @param   addr - register number (SPI_*).
 *          data - the byte to write (the read path uses 0xFF as a dummy).
 *          mask - bit mask applied to the status byte; when any bit is set the wait port of the
 *                 status is served at once (the AVR: "if CPU waited").
 *
 * @return  the byte received in the data phase (the previous register content).
 */
uint8_t zx_spi_send( uint8_t addr, uint8_t data, uint8_t mask )
{
    uint8_t status;
    uint8_t ret;

    GPIO_ResetBits( nSPICS_PORT, nSPICS );      /* fix for status locking (AVR comment) */
    GPIO_SetBits( nSPICS_PORT, nSPICS );
    status = spi_send( addr );                  /* set address of the SPI register */

    GPIO_ResetBits( nSPICS_PORT, nSPICS );      /* send data for that register */
    ret = spi_send( data );
    GPIO_SetBits( nSPICS_PORT, nSPICS );

    if( ( status & mask ) != 0 )
    {
        zx_wait_task( status );                 /* if CPU waited */
    }

    return ret;
}

/*********************************************************************
 * @fn      zx_wait_task
 *
 * @brief   Serves one wait port access reported by the FPGA (zx_wait_task( ) of the AVR project).
 *          The status byte is { wr_n, waits[6:0] } (top.v: .status_in( { wr_n, waits[6:0] } )):
 *          bit 7 = 1 - the Z80 is reading the port, 0 - it is writing; bits 6..0 - the index of
 *          the wait port (ZXW_GLUK_CLOCK, ZXW_KONDR_RS232).
 *          For a read the MCU writes the byte into SPI_WAIT_DATA - the FPGA then puts it onto the
 *          Z80 bus and releases the wait state - for a write it takes the byte the Z80 wrote. The
 *          access to 0x40 is also what completes the wait in the FPGA
 *          (assign wait_end = sel_waitreg && scs_n_01).
 *
 * @param   status - the status byte read in the address phase.
 *
 * @return  none
 */
void zx_wait_task( uint8_t status )
{
    uint8_t addr = 0;
    uint8_t data = 0xFF;

    flags_register &= (uint8_t)~( FLAG_SPI_INT );   /* reset flag */

    if( ( status & 0x7F ) == 0 )
    {
        /* No wait port index: a spurious interrupt, for example the trailing edge of a request
         * which has just been served. Nothing is written back, so the wait logic of the FPGA is
         * not disturbed (the AVR would have written 0xFF into the wait register here). */
        ZxSpuriousCount++;
        return;
    }

    /* prepare data */
    switch( status & 0x7F )
    {
        case ZXW_GLUK_CLOCK:
#if DEF_ZX_GLUK_EN
            /* the address which the Z80 put on the Gluk clock port */
            addr = zx_spi_send( SPI_GLUK_ADDR, data, 0 );
            if( ( status & 0x80 ) != 0 )
            {
                data = gluk_get_reg( addr );        /* the Z80 reads: the value comes from the model */
            }
#endif
            break;

        case ZXW_KONDR_RS232:
            /* the Kondratiev RS232 port emulation is not ported (that branch is not used) */
            break;

        default:
            break;
    }

    if( ( status & 0x80 ) != 0 )
    {
        zx_spi_send( SPI_WAIT_DATA, data, 0 );              /* the Z80 reads: give it the byte */
    }
    else
    {
        data = zx_spi_send( SPI_WAIT_DATA, data, 0 );       /* the Z80 writes: take the byte */
    }

    if( ( status & 0x80 ) == 0 )
    {
        /* save data */
        switch( status & 0x7F )
        {
            case ZXW_GLUK_CLOCK:
#if DEF_ZX_GLUK_EN
                gluk_set_reg( addr, data );
#endif
                break;

            default:
                break;
        }
    }

    ZxIntCount++;

#if DEF_ZX_SPI_DEBUG
    printf( "[ZXSPI] st=%02x port=%u %s addr=%02x data=%02x\r\n",
            (unsigned int)status, (unsigned int)( status & 0x7F ),
            ( ( status & 0x80 ) != 0 ) ? "rd" : "wr",
            (unsigned int)addr, (unsigned int)data );
#endif
}

/*********************************************************************
 * @fn      zx_set_config
 *
 * @brief   Sends the current modes to the configuration register (SPI_CONFIG_REG, 0x50) of the
 *          FPGA - exactly zx_set_config( ) of the AVR project:
 *
 *            zx_spi_send( SPI_CONFIG_REG,
 *                         ( modes_register & MODE_VIDEO_MASK ) |
 *                         ( ( modes_register & MODE_TAPEOUT ) ? SPI_TAPEOUT_MODE_FLAG : 0 ) |
 *                         ( ( flags_ex_register & FLAG_EX_NMI ) ? SPI_CONFIG_NMI_FLAG : 0 ) |
 *                         ( flags & ~( MODE_VIDEO_MASK | SPI_TAPEOUT_MODE_FLAG |
 *                                      SPI_CONFIG_NMI_FLAG ) ), 0x7F );
 *
 * @param   flags - the extra bits (in the AVR it is SPI_TAPE_FLAG when the tape input is high).
 *
 * @return  none
 */
void zx_set_config( uint8_t flags )
{
#if DEF_ZX_SPI_EN
    uint8_t data;

    if( ( ZxReady == 0 ) || ( spi_ready( ) == 0 ) )
    {
        return;                                     /* the FPGA is not configured yet */
    }

    data = (uint8_t)( ( modes_register & MODE_VIDEO_MASK ) |
                      ( ( modes_register & MODE_TAPEOUT ) ? SPI_TAPEOUT_MODE_FLAG : 0 ) |
                      ( ( flags_ex_register & FLAG_EX_NMI ) ? SPI_CONFIG_NMI_FLAG : 0 ) |
                      ( flags & (uint8_t)~( MODE_VIDEO_MASK | SPI_TAPEOUT_MODE_FLAG | SPI_CONFIG_NMI_FLAG ) ) );

    spi_lock( );
    zx_spi_send( SPI_CONFIG_REG, data, 0x7F );
    spi_unlock( );

#if DEF_ZX_SPI_DEBUG
    printf( "[ZX] config=%02x (modes=%02x)\r\n", (unsigned int)data, (unsigned int)modes_register );
#endif
#endif
}

/*********************************************************************
 * @fn      zx_mode_switcher
 *
 * @brief   zx_mode_switcher( ) of the AVR project: inverts the given mode bits, saves the mode
 *          register to the NVRAM and asks for the configuration to be sent to the FPGA.
 *
 *          IMPORTANT: this function is called from the keyboard handler, which runs inside
 *          USBH_MainDeal( ) with the scheduler suspended (vTaskSuspendAll, see src/app_usb.c).
 *          Blocking on the SPI bus mutex is not allowed there - FreeRTOS asserts
 *          "Cannot block if the scheduler is suspended" (queue.c) - so the SPI write is only
 *          requested here and performed by the "zx" task (zx_service( ), which also refreshes the
 *          LEDs through the USB path). The same applies to every other call which can be reached
 *          from the USB report path.
 *
 * @param   mode - the bits to invert: MODE_TAPEOUT for "Num Lock", a mask of MODE_VIDEO_MASK
 *          (walking through the video modes) for "Scroll Lock".
 *
 * @return  none
 */
void zx_mode_switcher( uint8_t mode )
{
    /* invert the mode */
    modes_register ^= mode;

    /* save the mode register to the NVRAM (the AVR writes its RTC_COMMON_MODE_REG cell, mapped
     * to a BKP register here, see src/rtc.c). Only register access, no blocking. */
    rtc_write( RTC_COMMON_MODE_REG, modes_register );

    /* the configuration is sent by the "zx" task, see the note above */
    ZxConfigPending = 1;

#if DEF_ZX_SPI_DEBUG
    printf( "[ZX] mode switch %02x -> modes=%02x (leds=%02x)\r\n", (unsigned int)mode,
            (unsigned int)modes_register, (unsigned int)( modes_register & MODE_LED_MASK ) );
#endif
}

/*********************************************************************
 * @fn      zx_service
 *
 * @brief   Polls FLAG_SPI_INT and serves the pending request: the "event from SPI" block of the
 *          AVR main loop, unchanged. Called from the ZX task in the FreeRTOS mode and from the
 *          super loop of main( ) in the bare-metal mode.
 *
 * @return  none
 */
void zx_service( void )
{
#if DEF_ZX_SPI_EN
    uint8_t status;

    if( ( ZxReady == 0 ) || ( spi_ready( ) == 0 ) )
    {
        return;                                     /* the FPGA is not configured yet */
    }

    /* A mode change requested from the keyboard path (zx_mode_switcher( )): the SPI write happens
     * here, because that caller runs with the scheduler suspended and must not block on the bus
     * mutex (see the note in zx_mode_switcher( )). */
    if( ZxConfigPending != 0 )
    {
        ZxConfigPending = 0;
        zx_set_config( ( flags_register & FLAG_LAST_TAPE_VALUE ) ? SPI_TAPE_FLAG : 0 );
    }

    if( ( flags_register & FLAG_SPI_INT ) == 0 )
    {
        return;
    }

    spi_lock( );

    /* get status byte */
    GPIO_ResetBits( nSPICS_PORT, nSPICS );
    GPIO_SetBits( nSPICS_PORT, nSPICS );
    status = spi_send( 0 );
    zx_wait_task( status );

    spi_unlock( );
#endif
}

#if DEF_FREERTOS_EN
static TaskHandle_t ZxTaskHandle = NULL;
static void vZxTask( void *pvParameters );
#endif

/*********************************************************************
 * @fn      EXTI9_5_IRQHandler
 *
 * @brief   spiint_n (PB7, EXTI line 7): the FPGA asks for the ZX port service. The exact
 *          counterpart of ISR(INT6_vect) of the AVR project - only the flag is set. In the
 *          FreeRTOS mode the ZX task is notified as well, so the wait state of the Z80 stays
 *          short (the AVR served the request from its main loop).
 *
 * @return  none
 */
void EXTI9_5_IRQHandler( void ) __attribute__( ( interrupt( "WCH-Interrupt-fast" ) ) );
void EXTI9_5_IRQHandler( void )
{
    if( EXTI_GetITStatus( SPI_IRQ_EXTI_LINE ) != RESET )
    {
        EXTI_ClearITPendingBit( SPI_IRQ_EXTI_LINE );

        flags_register |= FLAG_SPI_INT;             /* ISR(INT6_vect) of the AVR */

#if DEF_FREERTOS_EN
        if( ZxTaskHandle != NULL )
        {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;

            vTaskNotifyGiveFromISR( ZxTaskHandle, &xHigherPriorityTaskWoken );
            portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
        }
#endif
    }
}

#if DEF_FREERTOS_EN
/*********************************************************************
 * @fn      vZxTask
 *
 * @brief   The ZX service task: it replaces the "event from SPI" block of the AVR main loop, so
 *          the request of the FPGA is served with a task latency instead of the loop latency.
 *
 * @param   pvParameters - not used.
 *
 * @return  none
 */
static void vZxTask( void *pvParameters )
{
    (void)pvParameters;

    printf( "[RTOS] zx task started (waiting for the FPGA)\r\n" );

    for( ;; )
    {
        if( ZxReady == 0 )
        {
            /* The FPGA is not configured yet: nothing to serve. The task must sleep here - it runs
             * above the power and USB tasks, a spin would starve them (and the PSU would never be
             * switched on). */
            vTaskDelay( pdMS_TO_TICKS( DEF_ZX_TASK_IDLE_MS ) );
            continue;
        }

        zx_service( );

        /* The notification comes from the SPI interrupt. The timeout is only a safety net (a
         * missed notification must not leave the Z80 waiting forever) and has to be at least one
         * RTOS tick: 500 Hz means one tick is 2 ms, pdMS_TO_TICKS( 1 ) would be 0. */
        (void)ulTaskNotifyTake( pdTRUE, DEF_ZX_TASK_POLL_TICKS );
    }
}
#endif

/*********************************************************************
 * @fn      zx_task_start
 *
 * @brief   Creates the SPI bus lock and starts the ZX service task. Called from AppTasks_Start( ),
 *          i.e. before the scheduler runs. In the bare-metal mode nothing has to be started: the
 *          super loop of main( ) calls zx_service( ) directly.
 *
 * @return  none
 */
void zx_task_start( void )
{
#if DEF_FREERTOS_EN
    BaseType_t s;

    spi_lock_init( );

#if DEF_ZX_SPI_EN
    s = xTaskCreate( vZxTask, "zx", DEF_ZX_TASK_STACK_WORDS, NULL, DEF_ZX_TASK_PRIO, &ZxTaskHandle );
    printf( "[RTOS] zx task: %s\r\n", ( s == pdPASS ) ? "created" : "FAILED" );
#else
    printf( "[RTOS] zx task: disabled (DEF_ZX_SPI_EN = 0)\r\n" );
#endif

#endif
}
