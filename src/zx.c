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
#include <string.h>                             /* memcpy( ), memcmp( ) for the keyboard matrix */
#include "app_km.h"                             /* g_usbHidMouseReady (the presence of the mouse) */

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
static volatile uint8_t ZxResetPending = 0; /* a reset of the Z80 (SPI_RST_REG) was requested     */
static volatile uint32_t ZxIntCount = 0;    /* serviced requests (diagnostic)                 */
static uint32_t ZxSpuriousCount = 0;        /* interrupts without a wait port index           */
static uint32_t ZxReadCount = 0;            /* ... of ZxIntCount: the Z80 was reading         */
static uint32_t ZxWriteCount = 0;           /* ... of ZxIntCount: the Z80 was writing         */
static uint8_t  ZxAddrCount[16] = { 0 };    /* the Gluk registers 0x00..0x0F of this period   */
static uint32_t ZxExtCount = 0;             /* the other wait ports / addresses of the period */
static uint32_t ZxStatNextMs = 0;           /* when the next summary line is due (g_ms_ticks) */
#if DEF_ZX_KBD_EN
static uint32_t ZxKbdTransfers = 0;         /* transferred keyboard matrices (diagnostic)     */
#endif
#if DEF_ZX_MOUSE_EN
static uint32_t ZxMouseTransfers = 0;       /* transferred mouse sets (diagnostic)            */
/* The absolute movements of the period, as they arrive in the reports: a phantom jump is one big
 * value here, while the normal movement of the mouse accumulates in many small ones (see the
 * summary line of zx_stats( )). */
static uint32_t ZxMouseAbsX = 0;
static uint32_t ZxMouseAbsY = 0;
/** When the mouse registers were transferred for the last time (the rate limit, see spi.h). */
static uint32_t ZxMouseLastMs = 0;
/** The buttons byte which the FPGA holds (see zx_mouse_task( ): it is written only when it
 *  differs), and the flag which forces the whole triple out (after zx_mouse_reset( )). */
static uint8_t  ZxMouseBtnSent = 0xFF;
static uint8_t  ZxMouseForceAll = 1;
/* The last suspicious report, kept for the ZX task to print (see zx_mouse_dump( )). */
static uint8_t  ZxMsDump[ 8 ];
static uint8_t  ZxMsDumpLen = 0;
static int16_t  ZxMsDumpDx = 0;
static int16_t  ZxMsDumpDy = 0;
static volatile uint8_t ZxMsDumpNew = 0;
/* The ZX mouse registers (the AVR zx_mouse_x / zx_mouse_y / zx_mouse_button, see the mouse block
 * below for the layout): they are declared here as well, because zx_stats( ) reports them. The
 * power-on state is the "no mouse" signature of the AVR (X = Y = 0xFF), because the presence is
 * only known once the USB enumeration reports it - zx_mouse_check( ) then sets the "mouse present"
 * values (X = 0, Y = 1). */
static uint8_t  zx_mouse_x = 0xFF;
static uint8_t  zx_mouse_y = 0xFF;
static uint8_t  zx_mouse_wheel = ZX_MOUSE_WHEEL_INIT;
static uint8_t  zx_mouse_button = 0xFF;
#endif
#if ( DEF_ZXSPI_TRACE >= 1 )
static uint8_t  ZxTracePrev = 0xFF;         /* the { rd/wr, address } of the last trace line  */
#endif

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

#if DEF_ZX_KBD_EN
    /* The AVR calls zx_task( ZX_TASK_INIT ) here, and that clears the keyboard matrix. The empty
     * matrix has to be sent to the FPGA as well: a (re)configuration leaves its kbd_reg at 0, but
     * the strobe is what tells the port engine about the new state. The transfer itself is done by
     * zx_service( ). */
    zx_clr_kb( );
#endif

#if DEF_ZX_MOUSE_EN
    /* The same for the mouse: the mouse registers of the FPGA start from 0 after a (re)configuration,
     * so the presence signature has to go out again even though the mouse itself did not change. The
     * force flag makes zx_mouse_task( ) send the whole triple instead of only the changes. */
    ZxMouseForceAll = 1;
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
 * @fn      Zx_CsSet / Zx_CsReset
 *
 * @brief   The two levels of the chip select of the FPGA SPI slave, each held for
 *          ZX_CS_EDGE_DELAY_US (see spi.h). The FPGA synchronises that line with its own fclk, so
 *          the edges must not be shorter than a few of its cycles; every CS transition of the
 *          protocol goes through these helpers.
 *
 * @return  none
 */
static void Zx_CsSet( void )
{
    GPIO_SetBits( nSPICS_PORT, nSPICS );
    Delay_Us( ZX_CS_EDGE_DELAY_US );
}

static void Zx_CsReset( void )
{
    GPIO_ResetBits( nSPICS_PORT, nSPICS );
    Delay_Us( ZX_CS_EDGE_DELAY_US );
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

    Zx_CsReset( );                              /* fix for status locking (AVR comment) */
    Zx_CsSet( );
    status = spi_send( addr );                  /* set address of the SPI register */

    Zx_CsReset( );                              /* send data for that register */
    ret = spi_send( data );
    Zx_CsSet( );

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

    if( ( status & 0x80 ) != 0 )
    {
        ZxReadCount++;
    }
    else
    {
        ZxWriteCount++;
    }

    if( ( status & 0x7F ) == ZXW_GLUK_CLOCK )
    {
        if( addr < 16 )
        {
            ZxAddrCount[ addr ]++;
        }
        else
        {
            ZxExtCount++;
        }
    }
    else
    {
        ZxExtCount++;                           /* another wait port (the RS232 is not ported) */
    }

#if ( DEF_ZXSPI_TRACE >= 1 )
    {
        uint8_t key = (uint8_t)( ( status & 0x80 ) | ( addr & 0x7F ) );

        /* The raw trace with DEF_ZXSPI_TRACE == 2, and only the changed port with the level 1: a ZX
         * program which polls one register then adds no line here at all (see spi.h for the price
         * of a line - the Z80 waits for its transmission). */
        if( ( DEF_ZXSPI_TRACE >= 2 ) || ( key != ZxTracePrev ) )
        {
            printf( "[ZXSPI] st=%02x port=%u %s addr=%02x data=%02x\r\n",
                    (unsigned int)status, (unsigned int)( status & 0x7F ),
                    ( ( status & 0x80 ) != 0 ) ? "rd" : "wr",
                    (unsigned int)addr, (unsigned int)data );
        }

        ZxTracePrev = key;
    }
#endif
}

#if DEF_ZX_SPI_DEBUG
/*********************************************************************
 * @fn      zx_stats
 *
 * @brief   Prints one compact line about the served traffic every DEF_ZX_STAT_MS ms (see spi.h) and
 *          resets the counters of the period. It is the default instead of DEF_ZXSPI_TRACE:
 *          printing every access is both a log flood and a real slowdown, because each line holds
 *          the Z80 in its wait state while it is transmitted at 115200 (about 5 ms).
 *
 *          The line carries the requests of the period (serviced, spurious, read / written), the
 *          keyboard and the mouse transfers of the period, the other wait ports, a histogram of the
 *          Gluk registers 0x00..0x0F (only the non-zero ones are listed) and the number of the SPI
 *          timeouts, when there were any.
 *
 * @return  none
 */
static void zx_stats( void )
{
#if ( DEF_ZX_STAT_MS > 0 )
    static uint32_t prev_int = 0;
    static uint32_t prev_spur = 0;
    static uint32_t prev_rd = 0;
    static uint32_t prev_wr = 0;
    static uint32_t prev_kbd = 0;
    static uint32_t prev_mouse = 0;
    uint32_t kbd_now = 0;
    uint32_t mouse_now = 0;
    uint8_t  i;

#if DEF_ZX_KBD_EN
    kbd_now = ZxKbdTransfers;
#endif
#if DEF_ZX_MOUSE_EN
    mouse_now = ZxMouseTransfers;
#endif

    if( (uint32_t)( g_ms_ticks - ZxStatNextMs ) < (uint32_t)DEF_ZX_STAT_MS )
    {
        return;                                 /* the period has not elapsed yet */
    }

    ZxStatNextMs = g_ms_ticks;

    printf( "[ZX] %us: req=%u spur=%u rd=%u wrt=%u kbd=%u ms=%u oth=%u",
            (unsigned int)( DEF_ZX_STAT_MS / 1000u ),
            (unsigned int)( ZxIntCount - prev_int ), (unsigned int)( ZxSpuriousCount - prev_spur ),
            (unsigned int)( ZxReadCount - prev_rd ), (unsigned int)( ZxWriteCount - prev_wr ),
            (unsigned int)( kbd_now - prev_kbd ), (unsigned int)( mouse_now - prev_mouse ),
            (unsigned int)ZxExtCount );

#if DEF_ZX_MOUSE_EN
    /* The mouse registers as they are now, i.e. what the FPGA has (modulo the transfer which may be
     * in flight): the X and the Y counters change with every movement, so two consecutive summary
     * lines with different values prove that the reports reach the ZX side. |dX| and |dY| are the
     * movements which arrived in the period: a phantom jump shows up as a big single value. */
    printf( " btn=%02x x=%02x y=%02x |dX|=%u |dY|=%u", (unsigned int)zx_mouse_button,
            (unsigned int)zx_mouse_x, (unsigned int)zx_mouse_y,
            (unsigned int)ZxMouseAbsX, (unsigned int)ZxMouseAbsY );

    ZxMouseAbsX = 0;
    ZxMouseAbsY = 0;
#endif

    prev_int = ZxIntCount;
    prev_spur = ZxSpuriousCount;
    prev_rd = ZxReadCount;
    prev_wr = ZxWriteCount;
    prev_kbd = kbd_now;
    prev_mouse = mouse_now;

    for( i = 0; i < 16; i++ )
    {
        if( ZxAddrCount[ i ] != 0 )
        {
            printf( " %02x=%u", (unsigned int)i, (unsigned int)ZxAddrCount[ i ] );

            ZxAddrCount[ i ] = 0;
        }
    }

    ZxExtCount = 0;

    if( spi_timeout_count( ) != 0 )
    {
        printf( " spi_to=%u", (unsigned int)spi_timeout_count( ) );
    }

    printf( "\r\n" );
#endif
}
#endif

/*******************************************************************************/
/* The ZX keyboard matrix (kbmap.c / zx.c of the AVR project, z80/zkbdmus.v of the FPGA) */

#if DEF_ZX_KBD_EN
/** The matrix which is sent to the FPGA (zx_map[5] of the AVR): zx_map[m] bit r is the key of the
 *  half-row r and the Z80 data bit ( 4 - m ), so the byte 4 goes to the register first. */
static uint8_t          zx_map[5];
/** The snapshot of the matrix which is being transferred. */
static uint8_t          ZxKbdSend[5];
/** One counter per key (zx_counters[40] of the AVR): a ZX key stays pressed while at least one
 *  keyboard holds it. */
static uint8_t          zx_counters[40];
/** The matrix changed since the last transfer. Set by zx_kbd_key( )/zx_clr_kb( ) from the USB
 *  report path and by zx_kbd_task( ) itself when a change happened during a transfer. */
static volatile uint8_t ZxKbdDirty = 0;
#if ( DEF_ZX_KBD_REFRESH_MS > 0 )
/** When the matrix was sent for the last time (the periodic refresh, see zx_kbd_task( )). */
static uint32_t         ZxKbdLastMs = 0;
#endif
#endif

/*********************************************************************
 * @fn      zx_clr_kb
 *
 * @brief   Clears the whole keyboard matrix - zx_clr_kb( ) of the AVR project, which is called on
 *          its initialisation and when the ESC key arrives (the CLRKYS event): the ZX software uses
 *          it to release all the keys (a stuck key cannot be released otherwise).
 *
 * @return  none
 */
void zx_clr_kb( void )
{
#if DEF_ZX_KBD_EN
    uint8_t i;

    for( i = 0; i < 40; i++ )
    {
        zx_counters[ i ] = 0;
    }

    for( i = 0; i < 5; i++ )
    {
        zx_map[ i ] = 0;
    }

    ZxKbdDirty = 1;                         /* the cleared matrix has to be sent as well */
#endif
}

/*********************************************************************
 * @fn      zx_kbd_key
 *
 * @brief   One key event of the USB keyboard, i.e. update_keys( ) of the AVR project: the code is
 *          converted into the bit of the matrix register (ZX_KBD_BIT( ), see zx.h), and a per-key
 *          counter keeps the key pressed while at least one keyboard holds it.
 *
 *          This function must not touch the SPI bus: it is called from KB_AnalyzeKeyValue( )
 *          (src/USB_Host/app_km.c), i.e. from the USB report path, which runs with the scheduler
 *          suspended and must not block. The transfer is done by the ZX task (zx_kbd_task( )).
 *
 *          CLRKYS is not a key: it drops the whole matrix (zx_clr_kb( )) and only the press edge
 *          matters, exactly as in the AVR (its update_keys( ) drops the release of CLRKYS).
 *
 * @param   zxcode  - a KEY_* code of zx.h, or CLRKYS.
 *          pressed - 0: the key was released, any other value: it was pressed.
 *
 * @return  none
 */
void zx_kbd_key( uint8_t zxcode, uint8_t pressed )
{
#if DEF_ZX_KBD_EN
    uint8_t bit;

    if( zxcode == CLRKYS )
    {
        if( pressed != 0 )
        {
            zx_clr_kb( );
        }

        return;
    }

    if( zxcode >= 40 )
    {
        return;                             /* not a ZX key (the AVR checks zxcode < 40 as well) */
    }

    if( pressed != 0 )
    {
        if( zx_counters[ zxcode ]++ != 0 )
        {
            return;                         /* another keyboard already holds the key */
        }

        bit = (uint8_t)ZX_KBD_BIT( zxcode );
        zx_map[ bit >> 3 ] |= (uint8_t)( 1u << ( bit & 0x07 ) );
    }
    else
    {
        if( ( zx_counters[ zxcode ] == 0 ) || ( --zx_counters[ zxcode ] != 0 ) )
        {
            return;                         /* another keyboard still holds the key */
        }

        bit = (uint8_t)ZX_KBD_BIT( zxcode );
        zx_map[ bit >> 3 ] &= (uint8_t)~( 1u << ( bit & 0x07 ) );
    }

    ZxKbdDirty = 1;
#else
    (void)zxcode;
    (void)pressed;
#endif
}

/*********************************************************************
 * @fn      zx_kbd_task
 *
 * @brief   Transfers the keyboard matrix to the FPGA when it changed - the keyboard part of
 *          zx_task( ) of the AVR project. The sequence is the one of the AVR:
 *
 *            five times: zx_spi_send( SPI_KBD_DAT, byte, 0x7F ) - the 40 bit register is filled
 *                        LSB first, and the AVR sends its zx_map[4] first ("send order: LSbit
 *                        first, from [4] to [0]"); the 0x7F mask serves a pending wait port on
 *                        the way (it is the mask of the AVR as well);
 *            then:       the address of SPI_KBD_STB, which is the strobe - the FPGA latches
 *                        kbd_reg into the port engine when CS goes high again
 *                        (assign kbd_stb = sel_kbdstb && scs_n_01 in slave/slavespi.v). The
 *                        address phase also answers with the status byte.
 *
 *          Unlike the AVR, which does one byte per call of its main loop, the whole sequence is done
 *          here, so the bus lock is held for one bounded time.
 *
 *          The rare false keys which the hardware tests showed ("8" came out as "m" now and then)
 *          are NOT cured here: they come from the SPI transport (a glitch counted as a shift) and
 *          are a subject of src/spi.h and FPGA_SPI.md. The defences which were tried - a single
 *          40 bit burst, a periodic re-send, longer chip select edges - either did not help or made
 *          it worse, and they are reverted or off.
 *
 * @return  none
 */
void zx_kbd_task( void )
{
#if DEF_ZX_KBD_EN
    uint8_t status;

    /* The matrix is re-sent periodically even when it did not change only when
     * DEF_ZX_KBD_REFRESH_MS is not 0: a transfer which was disturbed by a glitch on the SPI lines
     * then corrects itself within one scan of the ZX. The hardware tests of 27.09.2026 showed the
     * opposite effect and the feature is off by default: every transfer is also a chance for such a
     * glitch, so 50 transfers per second produced far more false keys than the rare event they were
     * meant to repair. */
#if ( DEF_ZX_KBD_REFRESH_MS > 0 )
    if( (uint32_t)( g_ms_ticks - ZxKbdLastMs ) >= (uint32_t)DEF_ZX_KBD_REFRESH_MS )
    {
        ZxKbdLastMs = g_ms_ticks;
        ZxKbdDirty = 1;
    }
#endif

    if( ZxKbdDirty == 0 )
    {
        return;                                 /* nothing changed since the last transfer */
    }

    /* Cleared BEFORE the snapshot on purpose: a key event which arrives while the transfer is
     * running sets the flag again and the matrix is sent once more (the AVR re-checks its FIFO in
     * the same way, and it never re-sends in the middle of a transfer either). */
    ZxKbdDirty = 0;

    memcpy( ZxKbdSend, (const void *)zx_map, sizeof( ZxKbdSend ) );

    spi_lock( );

    /* The five bytes are sent one by one, each in its own chip select phase (the AVR sequence). A
     * single 40 bit burst was tried on 27.09.2026 and made the rare false keys MORE frequent ("8"
     * came out as "space", a much larger shift, and often): a long chip select phase is a long window
     * in which a glitch on the lines can disturb the shifting register, while the eight clocks of one
     * byte expose it five times less. */
    zx_spi_send( SPI_KBD_DAT, ZxKbdSend[ 4 ], 0x7F );
    zx_spi_send( SPI_KBD_DAT, ZxKbdSend[ 3 ], 0x7F );
    zx_spi_send( SPI_KBD_DAT, ZxKbdSend[ 2 ], 0x7F );
    zx_spi_send( SPI_KBD_DAT, ZxKbdSend[ 1 ], 0x7F );
    zx_spi_send( SPI_KBD_DAT, ZxKbdSend[ 0 ], 0x7F );

    /* the strobe (the AVR: status = spi_send( SPI_KBD_STB ); CS low; CS high; then the status) */
    Zx_CsSet( );
    status = spi_send( SPI_KBD_STB );
    Zx_CsReset( );
    Zx_CsSet( );

    if( ( status & 0x7F ) != 0 )
    {
        zx_wait_task( status );                 /* if CPU waited */
    }

    spi_unlock( );

    ZxKbdTransfers++;

    /* a key event which arrived during the transfer has to be sent again */
    if( memcmp( ZxKbdSend, (const void *)zx_map, sizeof( ZxKbdSend ) ) != 0 )
    {
        ZxKbdDirty = 1;
    }

#if DEF_ZX_SPI_DEBUG
    printf( "[ZX] kb %02x %02x %02x %02x %02x n=%u%s\r\n",
            (unsigned int)ZxKbdSend[ 4 ], (unsigned int)ZxKbdSend[ 3 ],
            (unsigned int)ZxKbdSend[ 2 ], (unsigned int)ZxKbdSend[ 1 ],
            (unsigned int)ZxKbdSend[ 0 ], (unsigned int)ZxKbdTransfers,
            ( ZxKbdDirty != 0 ) ? " (again)" : "" );
#endif
#endif
}

/*******************************************************************************/
/* The mouse of the ZX (the AVR zx.c / ps2.c, the FPGA z80/zkbdmus.v) */

#if DEF_ZX_MOUSE_EN
/* The state of the mouse (the AVR zx_mouse_x / zx_mouse_y / zx_mouse_wheel / zx_mouse_button) is
 * declared above, next to the diagnostics of zx_stats( ), which reports it. */
/** The registers changed since the last transfer. Set from the USB report path (it must not block)
 *  and cleared by zx_mouse_task( ) before it takes its snapshot. */
static volatile uint8_t ZxMouseDirty = 1;
/** The values of the transfer in progress: the snapshot keeps the three bytes consistent while the
 *  report path may update them (a newer report is transferred by the next pass). */
static uint8_t          ZxMouseSend[3];
/** The last known state of the mouse (g_usbHidMouseReady of src/USB_Host/app_km.h). */
static uint8_t          ZxMousePresent = 0;
#endif

/*********************************************************************
 * @fn      zx_mouse_reset
 *
 * @brief   zx_mouse_reset( ) of the AVR project: sets the values by which the ZX software finds out
 *          whether a mouse is connected, and asks for the transfer of all three registers.
 *
 *            enable != 0 - a mouse is there: X = 0, Y = 1 (the AVR comment: "ZX autodetecting found
 *                         mouse on this values");
 *            enable == 0 - no mouse: X = Y = 0xFF ("not found mouse on this values").
 *
 *          The buttons byte always starts at 0xFF, i.e. no button and the wheel nibble 0xF ("no
 *          wheel") - the value which a mouse without a wheel keeps.
 *
 * @param   enable - 0: no mouse, any other value: a mouse is connected.
 *
 * @return  none
 */
void zx_mouse_reset( uint8_t enable )
{
#if DEF_ZX_MOUSE_EN
    if( enable != 0 )
    {
        zx_mouse_x = 0;
        zx_mouse_y = 1;
    }
    else
    {
        zx_mouse_x = 0xFF;
        zx_mouse_y = 0xFF;
    }

    zx_mouse_wheel = ZX_MOUSE_WHEEL_INIT;
    zx_mouse_button = 0xFF;

    ZxMouseForceAll = 1;                        /* the whole triple goes out, not only the changes */
    ZxMouseDirty = 1;
#else
    (void)enable;
#endif
}

/*********************************************************************
 * @fn      zx_mouse_report
 *
 * @brief   One HID mouse report, i.e. the mouse part of the PS/2 parser of the AVR (ps2.c): the
 *          movement is accumulated in the X and the Y counters (8 bit, they wrap around - the ZX
 *          software computes the movement from their differences, so no delta is lost even if
 *          several reports arrive between two transfers), the wheel is added to its nibble and the
 *          buttons replace the low nibble of the buttons byte:
 *
 *              button = ( wheel nibble << 4 ) | ( ( ~buttons & 0x07 ) | 0x08 )
 *
 *          (0 = pressed and bit 3 = 1 - exactly the value of the AVR, which gets it from its
 *          "b ^ 0x07").
 *
 *          Like zx_kbd_key( ), only the state is changed here: the function is called from the USB
 *          report path, which runs with the scheduler suspended and must not block on the SPI bus.
 *
 * @param   dx, dy  - the relative movement (signed, as the HID report carries it).
 *          wheel   - the relative wheel movement (0 when the mouse has no wheel).
 *          buttons - the HID button bits (MOUSE_BTN_LEFT/RIGHT/MIDDLE, 1 = pressed).
 *
 * @return  none
 */
void zx_mouse_report( int8_t dx, int8_t dy, int8_t wheel, uint8_t buttons )
{
#if DEF_ZX_MOUSE_EN
    zx_mouse_x = (uint8_t)( zx_mouse_x + (uint8_t)dx );
    zx_mouse_y = (uint8_t)( zx_mouse_y + (uint8_t)dy );

    ZxMouseAbsX += (uint32_t)( ( dx < 0 ) ? -dx : dx );
    ZxMouseAbsY += (uint32_t)( ( dy < 0 ) ? -dy : dy );

    if( wheel != 0 )
    {
        zx_mouse_wheel = (uint8_t)( ( zx_mouse_wheel + (uint8_t)wheel ) & 0x0F );
    }

    zx_mouse_button = (uint8_t)( (uint8_t)( zx_mouse_wheel << 4 ) |
                                 (uint8_t)( (uint8_t)( ~buttons & ZX_MOUSE_BTN_MASK ) |
                                            (uint8_t)ZX_MOUSE_BTN_FLAG ) );

    ZxMouseDirty = 1;
#else
    (void)dx;
    (void)dy;
    (void)wheel;
    (void)buttons;
#endif
}

/*********************************************************************
 * @fn      zx_mouse_dump
 *
 * @brief   Keeps the raw report of a suspicious mouse movement (more than 48 counts in one report)
 *          so that the ZX task can print it with the movement which the parser extracted from it.
 *          Printing it right here would be wrong: this is the USB report path, which holds the
 *          whole USB stack (see the note about the report dump in spi.h).
 *
 * @param   raw    - the report bytes.
 *          len    - their number (at most 8 are kept).
 *          dx, dy - the movement extracted from the report.
 *
 * @return  none
 */
void zx_mouse_dump( const uint8_t *raw, uint8_t len, int16_t dx, int16_t dy )
{
#if DEF_ZX_MOUSE_EN
    uint8_t i;

    if( len > (uint8_t)( sizeof( ZxMsDump ) / sizeof( ZxMsDump[ 0 ] ) ) )
    {
        len = (uint8_t)( sizeof( ZxMsDump ) / sizeof( ZxMsDump[ 0 ] ) );
    }

    for( i = 0; i < len; i++ )
    {
        ZxMsDump[ i ] = raw[ i ];
    }

    ZxMsDumpLen = len;
    ZxMsDumpDx = dx;
    ZxMsDumpDy = dy;
    ZxMsDumpNew = 1;
#else
    (void)raw;
    (void)len;
    (void)dx;
    (void)dy;
#endif
}

/*********************************************************************
 * @fn      zx_mouse_task
 *
 * @brief   Transfers the three mouse registers to the FPGA when they changed - zx_mouse_task( ) of
 *          the AVR project, which sends SPI_MOUSE_BTN, SPI_MOUSE_X and SPI_MOUSE_Y with the same
 *          0x7F mask and then clears its own flag. The strobes of those registers latch the bytes
 *          into the port engine at the end of their transaction (slave/slavespi.v), so the Z80 gets
 *          a consistent X and Y pair as well.
 *
 *          The snapshot keeps the three bytes consistent even if a new report arrives while the
 *          transfer runs: the flag is set again then and the newer values are transferred by the
 *          next pass (the counters accumulate the movement, so nothing is lost).
 *
 * @return  none
 */
void zx_mouse_task( void )
{
#if DEF_ZX_MOUSE_EN
    if( ZxMouseDirty == 0 )
    {
        return;                                 /* nothing changed since the last transfer */
    }

    /* The reports of a mouse arrive up to a hundred times per second, while the ZX reads the mouse
     * ports about once per frame: the transfer is limited to DEF_ZX_MOUSE_RATE_MS. The X and the Y
     * counters accumulate the movement, so nothing is lost - only the granularity gets coarser - and
     * every SPI transaction which is not made is a glitch which cannot happen (see spi.h). */
    if( (uint32_t)( g_ms_ticks - ZxMouseLastMs ) < (uint32_t)DEF_ZX_MOUSE_RATE_MS )
    {
        return;
    }

    ZxMouseLastMs = g_ms_ticks;
    ZxMouseDirty = 0;

    ZxMouseSend[ 0 ] = zx_mouse_button;
    ZxMouseSend[ 1 ] = zx_mouse_x;
    ZxMouseSend[ 2 ] = zx_mouse_y;

    spi_lock( );

    /* The buttons byte changes rarely (a click), so it is written only when it differs: one SPI
     * transaction less per transfer, a third of the mouse traffic. */
    if( ( ZxMouseSend[ 0 ] != ZxMouseBtnSent ) || ( ZxMouseForceAll != 0 ) )
    {
        zx_spi_send( SPI_MOUSE_BTN, ZxMouseSend[ 0 ], 0x7F );

        ZxMouseBtnSent = ZxMouseSend[ 0 ];
        ZxMouseForceAll = 0;
    }

    zx_spi_send( SPI_MOUSE_X, ZxMouseSend[ 1 ], 0x7F );
    zx_spi_send( SPI_MOUSE_Y, ZxMouseSend[ 2 ], 0x7F );

    spi_unlock( );

    ZxMouseTransfers++;

#if ( DEF_ZX_SPI_DEBUG && DEF_ZX_MOUSE_EN )
    /* The report of a movement which looked suspicious (zx_mouse_dump( )): printed here, in the task
     * context, because the USB path must not print. */
    if( ZxMsDumpNew != 0 )
    {
        uint8_t i;

        ZxMsDumpNew = 0;

        printf( "[ZX] ms raw dx=%d dy=%d len=%u:", (int)ZxMsDumpDx, (int)ZxMsDumpDy,
                (unsigned int)ZxMsDumpLen );

        for( i = 0; i < ZxMsDumpLen; i++ )
        {
            printf( " %02x", (unsigned int)ZxMsDump[ i ] );
        }

        printf( "\r\n" );
    }
#endif

#if ( DEF_ZXSPI_TRACE >= 2 )
    printf( "[ZX] ms btn=%02x x=%02x y=%02x n=%u\r\n",
            (unsigned int)ZxMouseSend[ 0 ], (unsigned int)ZxMouseSend[ 1 ],
            (unsigned int)ZxMouseSend[ 2 ], (unsigned int)ZxMouseTransfers );
#elif ( DEF_ZXSPI_TRACE >= 1 )
    {
        /* Only the button / wheel changes: the movement itself would be one line per report (up to
         * a thousand per second with a fast mouse), see spi.h. */
        static uint8_t prev_btn = 0xFF;
        uint8_t btn = (uint8_t)( ZxMouseSend[ 0 ] & (uint8_t)~( ZX_MOUSE_BTN_FLAG ) );

        if( btn != prev_btn )
        {
            printf( "[ZX] ms btn=%02x x=%02x y=%02x n=%u\r\n",
                    (unsigned int)ZxMouseSend[ 0 ], (unsigned int)ZxMouseSend[ 1 ],
                    (unsigned int)ZxMouseSend[ 2 ], (unsigned int)ZxMouseTransfers );
        }

        prev_btn = btn;
    }
#endif
#endif
}

/*********************************************************************
 * @fn      zx_mouse_check
 *
 * @brief   Follows g_usbHidMouseReady (src/USB_Host/app_km.c) and re-initialises the mouse registers
 *          on every change of the presence - the way the AVR does it in main( ) and in ps2.c when
 *          the initialisation of its PS/2 mouse succeeds or fails. Many ZX programs detect a mouse
 *          exactly by those values (see zx_mouse_reset( )).
 *
 * @return  none
 */
static void zx_mouse_check( void )
{
#if DEF_ZX_MOUSE_EN
    if( g_usbHidMouseReady != ZxMousePresent )
    {
        ZxMousePresent = g_usbHidMouseReady;

        zx_mouse_reset( ZxMousePresent );
    }
#endif
}

/*********************************************************************
 * @fn      zx_nmi_set
 *
 * @brief   The NMI of the Z80 - the PRINT SCREEN key of the AVR project (see the "E0 0x7C" case of
 *          its to_zx( )): the key asserts the NMI while it is held and releases it on the way up.
 *          As with the mode switching, only the flag and the request are set here, because the
 *          caller runs in the USB report path with the scheduler suspended; the transfer itself is
 *          done by the ZX task (zx_service( ) sees ZxConfigPending).
 *
 * @param   on - 0: release the NMI, any other value: assert it.
 *
 * @return  none
 */
void zx_nmi_set( uint8_t on )
{
    if( on != 0 )
    {
        if( ( flags_ex_register & FLAG_EX_NMI ) == 0 )
        {
            flags_ex_register |= FLAG_EX_NMI;
            ZxConfigPending = 1;
        }
    }
    else
    {
        if( ( flags_ex_register & FLAG_EX_NMI ) != 0 )
        {
            flags_ex_register &= (uint8_t)~( FLAG_EX_NMI );
            ZxConfigPending = 1;
        }
    }
}

/*********************************************************************
 * @fn      zx_request_reset
 *
 * @brief   Requests the reset of the Z80 (the reset register 0x30 of the FPGA) - the soft reset of
 *          the AVR project (atx.c:119-123), which keeps the configuration of the FPGA and the
 *          memory of the ZX. Only the request is set here: the callers are the button service and
 *          the USB report path (the latter runs with the scheduler suspended), so the SPI
 *          transaction is left to zx_service( ), see ZxResetPending.
 *
 * @return  none
 */
void zx_request_reset( void )
{
    ZxResetPending = 1;
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

    /* A reset of the Z80 requested from the button service or from the keyboard path
     * (zx_request_reset( )): the reset register of the FPGA is pulsed here, in the task context, for
     * the same reason as the mode change below. The 0x7F mask is the one of the AVR soft reset
     * (atx.c:122), so a pending wait port of the Z80 is served on the fly. */
    if( ZxResetPending != 0 )
    {
        ZxResetPending = 0;

        spi_lock( );
        zx_spi_send( SPI_RST_REG, 0, 0x7F );
        spi_unlock( );

#if DEF_ZX_SPI_DEBUG
        printf( "[ZX] z80 reset (SPI_RST_REG)\\r\\n" );
#endif
    }

    /* A mode change requested from the keyboard path (zx_mode_switcher( )): the SPI write happens
     * here, because that caller runs with the scheduler suspended and must not block on the bus
     * mutex (see the note in zx_mode_switcher( )). */
    if( ZxConfigPending != 0 )
    {
        ZxConfigPending = 0;
        zx_set_config( ( flags_register & FLAG_LAST_TAPE_VALUE ) ? SPI_TAPE_FLAG : 0 );
    }

    /* The keyboard matrix (zx_kbd_task( )) goes first: it is a short sequence of SPI transactions,
     * and it is done here, in the task context, so that the USB report path never touches the bus.
     * The Z80 is not waiting for it, but the earlier the new matrix is in the FPGA, the fewer key
     * scan frames miss it. */
#if DEF_ZX_KBD_EN
    zx_kbd_task( );
#endif

    /* The mouse registers: first the presence tracking (it re-initialises the registers when the
     * mouse is plugged in or unplugged - zx_mouse_reset( ), the AVR does the same), then the
     * changed values. Both are short SPI sequences done here, in the task context. */
#if DEF_ZX_MOUSE_EN
    zx_mouse_check( );
    zx_mouse_task( );
#endif

#if DEF_ZX_SPI_DEBUG
    /* The periodic summary of the served traffic: one line per DEF_ZX_STAT_MS instead of one line
     * per access (see spi.h and zx_stats( ) - a per access line also slows the Z80 down, because
     * the wait state lasts until the line is transmitted). */
    zx_stats( );
#endif

    if( ( flags_register & FLAG_SPI_INT ) == 0 )
    {
        return;
    }

    spi_lock( );

    /* get status byte */
    Zx_CsReset( );
    Zx_CsSet( );
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
