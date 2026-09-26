/********************************** (C) COPYRIGHT *******************************
* File Name          : rtc.c
* Version            : V1.0.0
* Description        : Dallas DS12887 (IBM PC compatible clock) register file emulation.
*                      The time base is the CH32V317 RTC counter (32.768 kHz crystal on LSE),
*                      the NVRAM and the settings live in the BKP registers (see RTC.md).
*                      The alarm registers are not emulated, register A/B/C/D are implemented
*                      as far as the hardware allows.
*******************************************************************************/
#include "usb_host_config.h"
#include "rtc.h"

/*******************************************************************************/
/* Constants */
#define RTC_MAGIC                   0x5A    /* stored in BKP together with the settings */
#define RTC_VERSION                 1       /* format version of the emulation */
#define RTC_YEAR_BASE               2000    /* the DS12887 keeps 2 digits, 20xx is assumed */
#define RTC_SEC_PER_DAY             86400u

/*******************************************************************************/
/* Variable Definition */
static uint8_t  RtcCtrlA = 0x00;            /* register A shadow (RS3..0)            */
static uint8_t  RtcCtrlB = DS_B_24_12;      /* register B shadow (BCD, 24 hours)     */
static uint8_t  RtcValid = 0;               /* register D VRT                        */
static uint8_t  RtcSetMode = 0;             /* 1 while B.SET is set                  */
static uint8_t  RtcShadow[ 10 ];            /* the registers 0x00..0x09 while SET = 1 */
static uint8_t  RtcUpdateFlag = 0;          /* register C UF (not battery backed)    */
static uint8_t  RtcLastSecond = 0xFF;
static uint32_t RtcPrescalerMax = 32767;    /* divider value of the selected clock   */

/* Calendar used for the conversion between the counter and the registers */
typedef struct
{
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  dow;               /* 1 = Monday .. 7 = Sunday */
} RTC_CAL;

static const uint8_t RtcMonthDays[ 13 ] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/*******************************************************************************/
/* BKP access helpers. The data registers DR1..DR10 are consecutive, DR11..DR42 follow in a
 * second block, so the offset of DR n (n = 1..42) is computed here. */
static uint16_t Rtc_BkpReg( uint8_t n )
{
    if( n <= 10 )
    {
        return (uint16_t)( BKP_DR1 + 4 * ( n - 1 ) );
    }
    if( n <= 42 )
    {
        return (uint16_t)( BKP_DR11 + 4 * ( n - 11 ) );
    }
    return (uint16_t)BKP_DR1;
}

/* Byte access into a BKP backed block: idx 0,1 -> register first, 2,3 -> register first+1 ... */
static uint8_t Rtc_BkpByteRead( uint8_t first, uint8_t idx )
{
    uint16_t v = BKP_ReadBackupRegister( Rtc_BkpReg( (uint8_t)( first + ( idx >> 1 ) ) ) );

    return ( idx & 1 ) ? (uint8_t)( v >> 8 ) : (uint8_t)( v & 0xFF );
}

static void Rtc_BkpByteWrite( uint8_t first, uint8_t idx, uint8_t data )
{
    uint16_t reg = Rtc_BkpReg( (uint8_t)( first + ( idx >> 1 ) ) );
    uint16_t v = BKP_ReadBackupRegister( reg );

    if( idx & 1 )
    {
        v = (uint16_t)( ( v & 0x00FF ) | ( (uint16_t)data << 8 ) );
    }
    else
    {
        v = (uint16_t)( ( v & 0xFF00 ) | data );
    }
    BKP_WriteBackupRegister( reg, v );
}

static void Rtc_SaveCtrl( void )
{
    BKP_WriteBackupRegister( Rtc_BkpReg( RTC_BKP_CTRL ), (uint16_t)( ( (uint16_t)RtcCtrlB << 8 ) | RtcCtrlA ) );
}

static void Rtc_SaveStat( void )
{
    uint16_t hi = (uint16_t)( ( RTC_VERSION & 0x0F ) | ( RtcValid ? 0x80 : 0x00 ) );

    BKP_WriteBackupRegister( Rtc_BkpReg( RTC_BKP_STAT ), (uint16_t)( ( hi << 8 ) | RTC_MAGIC ) );
}

/*******************************************************************************/
/* Calendar conversion (no libc, no dynamic memory) */
static uint8_t Rtc_IsLeap( uint16_t y )
{
    return ( ( ( y % 4 ) == 0 ) && ( ( y % 100 ) != 0 ) ) || ( ( y % 400 ) == 0 ) ? 1 : 0;
}

static uint8_t Rtc_MonthLen( uint16_t y, uint8_t m )
{
    if( ( m == 2 ) && Rtc_IsLeap( y ) )
    {
        return 29;
    }
    return RtcMonthDays[ m ];
}

static void Rtc_UnixToCal( uint32_t t, RTC_CAL *c )
{
    uint32_t days = t / RTC_SEC_PER_DAY;
    uint32_t rem = t % RTC_SEC_PER_DAY;
    uint16_t y = 1970;
    uint8_t  m = 1;

    c->hour = (uint8_t)( rem / 3600u );
    c->minute = (uint8_t)( ( rem % 3600u ) / 60u );
    c->second = (uint8_t)( rem % 60u );

    /* 01.01.1970 was a Thursday, 1 = Monday .. 7 = Sunday */
    c->dow = (uint8_t)( ( ( days + 3u ) % 7u ) + 1u );

    while( days >= (uint32_t)( Rtc_IsLeap( y ) ? 366 : 365 ) )
    {
        days -= (uint32_t)( Rtc_IsLeap( y ) ? 366 : 365 );
        y++;
    }
    while( days >= (uint32_t)Rtc_MonthLen( y, m ) )
    {
        days -= (uint32_t)Rtc_MonthLen( y, m );
        m++;
    }

    c->year = y;
    c->month = m;
    c->day = (uint8_t)( days + 1u );
}

static uint32_t Rtc_CalToUnix( const RTC_CAL *c )
{
    uint32_t days = 0;
    uint16_t y;
    uint8_t  m;

    for( y = 1970; y < c->year; y++ )
    {
        days += (uint32_t)( Rtc_IsLeap( y ) ? 366 : 365 );
    }
    for( m = 1; m < c->month; m++ )
    {
        days += (uint32_t)Rtc_MonthLen( c->year, m );
    }
    days += (uint32_t)( c->day - 1 );

    return days * RTC_SEC_PER_DAY + (uint32_t)c->hour * 3600u + (uint32_t)c->minute * 60u + c->second;
}

/*******************************************************************************/
/* BCD helpers and register encoding */
static uint8_t Rtc_Bin2Bcd( uint8_t v )
{
    return (uint8_t)( ( ( v / 10u ) << 4 ) | ( v % 10u ) );
}

static uint8_t Rtc_Bcd2Bin( uint8_t v )
{
    return (uint8_t)( ( ( v >> 4 ) * 10u ) + ( v & 0x0Fu ) );
}

static uint8_t Rtc_Encode( uint8_t v )
{
    return ( RtcCtrlB & DS_B_DM ) ? v : Rtc_Bin2Bcd( v );
}

static uint8_t Rtc_Decode( uint8_t v )
{
    return ( RtcCtrlB & DS_B_DM ) ? v : Rtc_Bcd2Bin( v );
}

static uint8_t Rtc_EncodeHour( uint8_t h )
{
    uint8_t h12;

    if( RtcCtrlB & DS_B_24_12 )
    {
        return Rtc_Encode( h );
    }

    h12 = (uint8_t)( h % 12u );
    if( h12 == 0 )
    {
        h12 = 12;
    }
    return (uint8_t)( Rtc_Encode( h12 ) | ( ( h >= 12 ) ? 0x80 : 0x00 ) );   /* bit 7 = PM */
}

static uint8_t Rtc_DecodeHour( uint8_t v )
{
    uint8_t h;

    if( RtcCtrlB & DS_B_24_12 )
    {
        h = Rtc_Decode( v );
        return ( h > 23 ) ? 0 : h;
    }

    h = Rtc_Decode( (uint8_t)( v & 0x7F ) );
    if( ( h == 0 ) || ( h > 12 ) )
    {
        h = 12;
    }
    return (uint8_t)( ( h % 12u ) + ( ( v & 0x80 ) ? 12 : 0 ) );
}

/* The counter is read twice so the value is consistent. The emulation of register A reports UIP
 * before the second changes, but a caller which ignores UIP must not see a mixed value either. */
static uint32_t Rtc_GetCounterStable( void )
{
    uint32_t c1, c2;

    do
    {
        c1 = RTC_GetCounter( );
        c2 = RTC_GetCounter( );
    } while( c1 != c2 );

    return c1;
}

/*******************************************************************************/
/* Time shadow: while B.SET is set the clock is stopped and the registers 0x00..0x09 hold the
 * values written by the host, exactly as the real DS12887 does. */
static void Rtc_TimeToShadow( void )
{
    RTC_CAL cal;
    uint8_t i;

    Rtc_UnixToCal( Rtc_GetCounterStable( ), &cal );

    RtcShadow[ DS_REG_SEC ] = Rtc_Encode( cal.second );
    RtcShadow[ DS_REG_MIN ] = Rtc_Encode( cal.minute );
    RtcShadow[ DS_REG_HOUR ] = Rtc_EncodeHour( cal.hour );
    RtcShadow[ DS_REG_DAY_WEEK ] = cal.dow;
    RtcShadow[ DS_REG_DAY_MONTH ] = Rtc_Encode( cal.day );
    RtcShadow[ DS_REG_MONTH ] = Rtc_Encode( cal.month );
    RtcShadow[ DS_REG_YEAR ] = Rtc_Encode( (uint8_t)( cal.year % 100 ) );

    /* the alarm slots are not emulated */
    RtcShadow[ DS_REG_SEC_ALARM ] = 0x00;
    RtcShadow[ DS_REG_MIN_ALARM ] = 0x00;
    RtcShadow[ DS_REG_HOUR_ALARM ] = 0x00;

    for( i = 0; i < 10; i++ )
    {
        Rtc_BkpByteWrite( RTC_BKP_SHADOW_FIRST, i, RtcShadow[ i ] );
    }
}

static void Rtc_ShadowToCounter( void )
{
    RTC_CAL cal;
    uint32_t t;

    cal.second = Rtc_Decode( RtcShadow[ DS_REG_SEC ] );
    cal.minute = Rtc_Decode( RtcShadow[ DS_REG_MIN ] );
    cal.hour = Rtc_DecodeHour( RtcShadow[ DS_REG_HOUR ] );
    cal.day = Rtc_Decode( RtcShadow[ DS_REG_DAY_MONTH ] );
    cal.month = Rtc_Decode( RtcShadow[ DS_REG_MONTH ] );
    cal.year = (uint16_t)( RTC_YEAR_BASE + Rtc_Decode( RtcShadow[ DS_REG_YEAR ] ) );

    /* the register values are stored as written, but the clock is loaded with sane values */
    if( cal.second > 59 )
    {
        cal.second = 0;
    }
    if( cal.minute > 59 )
    {
        cal.minute = 0;
    }
    if( ( cal.month < 1 ) || ( cal.month > 12 ) )
    {
        cal.month = 1;
    }
    if( ( cal.day < 1 ) || ( cal.day > Rtc_MonthLen( cal.year, cal.month ) ) )
    {
        cal.day = 1;
    }

    t = Rtc_CalToUnix( &cal );

    RTC_WaitForLastTask( );
    RTC_SetCounter( t );
    RTC_WaitForLastTask( );
    RTC_WaitForSynchro( );

    RtcLastSecond = 0xFF;
}

/*********************************************************************
 * @fn      rtc_read
 *
 * @brief   Reads one register of the emulated DS12887 (see RTC.md for the map).
 *
 * @param   addr - register number (0x00..0x3F).
 *
 * @return  the register value (0x00 for the registers which are not implemented).
 */
uint8_t rtc_read( uint8_t addr )
{
    RTC_CAL cal;

    if( addr <= DS_REG_YEAR )
    {
        /* the alarm registers are not emulated */
        if( ( addr == DS_REG_SEC_ALARM ) || ( addr == DS_REG_MIN_ALARM ) || ( addr == DS_REG_HOUR_ALARM ) )
        {
            return 0x00;
        }

        if( RtcSetMode )
        {
            return RtcShadow[ addr ];
        }

        Rtc_UnixToCal( Rtc_GetCounterStable( ), &cal );

        switch( addr )
        {
            case DS_REG_SEC:
                return Rtc_Encode( cal.second );

            case DS_REG_MIN:
                return Rtc_Encode( cal.minute );

            case DS_REG_HOUR:
                return Rtc_EncodeHour( cal.hour );

            case DS_REG_DAY_WEEK:
                return cal.dow;

            case DS_REG_DAY_MONTH:
                return Rtc_Encode( cal.day );

            case DS_REG_MONTH:
                return Rtc_Encode( cal.month );

            case DS_REG_YEAR:
                return Rtc_Encode( (uint8_t)( cal.year % 100 ) );

            default:
                break;
        }
        return 0x00;
    }

    switch( addr )
    {
        case DS_REG_A:
        {
            uint8_t uip = ( RTC_GetDivider( ) > ( RtcPrescalerMax - DEF_RTC_UIP_TICKS ) ) ? DS_A_UIP : 0x00;

            return (uint8_t)( uip | DS_A_DV | ( RtcCtrlA & 0x0F ) );
        }

        case DS_REG_B:
            return RtcCtrlB;

        case DS_REG_C:
        {
            /* the update flag rises once per second and is cleared by reading the register */
            uint8_t flags = 0x00;

            if( RtcSetMode == 0 )
            {
                uint8_t sec = (uint8_t)( Rtc_GetCounterStable( ) % 60u );

                if( sec != RtcLastSecond )
                {
                    RtcLastSecond = sec;
                    RtcUpdateFlag = 1;
                }
            }
            if( RtcUpdateFlag )
            {
                flags |= DS_C_UF;
                RtcUpdateFlag = 0;
            }
            return flags;
        }

        case DS_REG_D:
            return RtcValid ? DS_D_VRT : 0x00;

        default:
            break;
    }

    if( ( addr >= DS_REG_NVRAM_FIRST ) && ( addr <= DS_REG_NVRAM_LAST ) )
    {
        return Rtc_BkpByteRead( RTC_BKP_NVRAM_FIRST, (uint8_t)( addr - DS_REG_NVRAM_FIRST ) );
    }

    return 0x00;
}

/*********************************************************************
 * @fn      rtc_write
 *
 * @brief   Writes one register of the emulated DS12887 (see RTC.md for the map).
 *
 * @param   addr - register number (0x00..0x3F).
 *          data - value to write.
 *
 * @return  none
 */
/* The write path is part of the public interface of the emulation: keep the out-of-line copy,
 * GCC otherwise emits only an internal ".part" clone when all callers live in this file. */
__attribute__(( noinline )) void rtc_write( uint8_t addr, uint8_t data )
{
    if( addr <= DS_REG_YEAR )
    {
        /* the alarm registers are not emulated */
        if( ( addr == DS_REG_SEC_ALARM ) || ( addr == DS_REG_MIN_ALARM ) || ( addr == DS_REG_HOUR_ALARM ) )
        {
            return;
        }

        /* as on the chip the time can only be written while the clock is stopped (B.SET = 1) */
        if( RtcSetMode == 0 )
        {
            return;
        }

        RtcShadow[ addr ] = data;
        Rtc_BkpByteWrite( RTC_BKP_SHADOW_FIRST, addr, data );
        return;
    }

    switch( addr )
    {
        case DS_REG_A:
            /* only the rate select bits are writable, DV and UIP are read only */
            RtcCtrlA = (uint8_t)( data & 0x0F );
            Rtc_SaveCtrl( );
            return;

        case DS_REG_B:
        {
            uint8_t old = RtcCtrlB;

            RtcCtrlB = data;

            if( ( RtcCtrlB & DS_B_SET ) && ( ( old & DS_B_SET ) == 0 ) )
            {
                /* the clock is stopped and the current time is frozen in the registers */
                RtcSetMode = 1;
                Rtc_TimeToShadow( );
                printf( "RTC: SET=1, clock stopped\r\n" );
            }
            else if( ( ( RtcCtrlB & DS_B_SET ) == 0 ) && ( old & DS_B_SET ) )
            {
                /* the written time is loaded into the counter, the clock runs again */
                Rtc_ShadowToCounter( );
                RtcSetMode = 0;
                printf( "RTC: SET=0, clock started\r\n" );
            }

            Rtc_SaveCtrl( );
            return;
        }

        case DS_REG_C:
            /* read only, the flags are cleared by reading */
            return;

        case DS_REG_D:
            RtcValid = ( data & DS_D_VRT ) ? 1 : 0;
            Rtc_SaveStat( );
            printf( "RTC: VRT=%u\r\n", (unsigned int)RtcValid );
            return;

        default:
            break;
    }

    if( ( addr >= DS_REG_NVRAM_FIRST ) && ( addr <= DS_REG_NVRAM_LAST ) )
    {
        Rtc_BkpByteWrite( RTC_BKP_NVRAM_FIRST, (uint8_t)( addr - DS_REG_NVRAM_FIRST ), data );
    }
}

/*********************************************************************
 * @fn      Rtc_SetInitTime
 *
 * @brief   Writes the reference time through the register interface, exactly like the host
 *          software would do it (SET = 1, write 0x00..0x09, SET = 0, mark the time valid).
 *
 * @return  none
 */
static void Rtc_SetInitTime( void )
{
    RTC_CAL cal;

    cal.year = DEF_RTC_INIT_YEAR;
    cal.month = DEF_RTC_INIT_MONTH;
    cal.day = DEF_RTC_INIT_DAY;
    cal.hour = DEF_RTC_INIT_HOUR;
    cal.minute = DEF_RTC_INIT_MIN;
    cal.second = DEF_RTC_INIT_SEC;

    Rtc_UnixToCal( Rtc_CalToUnix( &cal ), &cal );        /* also gives the day of week */

    RtcCtrlB = DS_B_24_12;                               /* BCD data, 24 hour mode */
    Rtc_SaveCtrl( );

    rtc_write( DS_REG_B, (uint8_t)( RtcCtrlB | DS_B_SET ) );

    rtc_write( DS_REG_YEAR, Rtc_Encode( (uint8_t)( cal.year % 100 ) ) );
    rtc_write( DS_REG_MONTH, Rtc_Encode( cal.month ) );
    rtc_write( DS_REG_DAY_MONTH, Rtc_Encode( cal.day ) );
    rtc_write( DS_REG_DAY_WEEK, cal.dow );
    rtc_write( DS_REG_HOUR, Rtc_EncodeHour( cal.hour ) );
    rtc_write( DS_REG_MIN, Rtc_Encode( cal.minute ) );
    rtc_write( DS_REG_SEC, Rtc_Encode( cal.second ) );

    rtc_write( DS_REG_D, DS_D_VRT );                     /* the time is valid now */

    rtc_write( DS_REG_B, (uint8_t)( RtcCtrlB & ~DS_B_SET ) );

    printf( "RTC: time set to %02u.%02u.%04u %02u:%02u:%02u (dow=%u)\r\n",
            (unsigned int)cal.day, (unsigned int)cal.month, (unsigned int)cal.year,
            (unsigned int)cal.hour, (unsigned int)cal.minute, (unsigned int)cal.second,
            (unsigned int)cal.dow );
}

/*********************************************************************
 * @fn      rtc_init
 *
 * @brief   Sets up the RTC clock (32.768 kHz crystal on LSE, LSI as a fallback), restores the
 *          settings from the BKP registers and writes the reference time when no valid time is
 *          found (no RTC clock was enabled, the counter is zero, the backup domain was reset or
 *          the VRT bit is cleared).
 *
 * @return  none
 */
void rtc_init( void )
{
    uint32_t i;
    uint8_t  clock_lse = 0;
    uint8_t  magic_ok, rtc_enabled;
    uint16_t ctrl, stat;
    uint8_t  k;

    RCC_APB1PeriphClockCmd( RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE );
    PWR_BackupAccessCmd( ENABLE );

    /* the clock enable bit also tells whether the counter was running before this reset */
    rtc_enabled = ( RCC->BDCTLR & RCC_RTCEN ) ? 1 : 0;

    /* the 32.768 kHz crystal first, the internal LSI as a fallback (less accurate) */
    RCC_LSEConfig( RCC_LSE_ON );
    for( i = 0; i < DEF_RTC_LSE_WAIT_MS; i++ )
    {
        if( RCC_GetFlagStatus( RCC_FLAG_LSERDY ) != RESET )
        {
            break;
        }
        Delay_Ms( 1 );
    }

    if( RCC_GetFlagStatus( RCC_FLAG_LSERDY ) != RESET )
    {
        clock_lse = 1;
        RCC_RTCCLKConfig( RCC_RTCCLKSource_LSE );
        RtcPrescalerMax = 32767;
    }
    else
    {
        RCC_LSICmd( ENABLE );
        for( i = 0; i < DEF_RTC_LSE_WAIT_MS; i++ )
        {
            if( RCC_GetFlagStatus( RCC_FLAG_LSIRDY ) != RESET )
            {
                break;
            }
            Delay_Ms( 1 );
        }
        RCC_RTCCLKConfig( RCC_RTCCLKSource_LSI );
        RtcPrescalerMax = 39999;
    }

    RCC_RTCCLKCmd( ENABLE );
    RTC_WaitForSynchro( );
    RTC_WaitForLastTask( );
    RTC_SetPrescaler( RtcPrescalerMax );
    RTC_WaitForLastTask( );

    /* the settings stored in the BKP registers */
    ctrl = BKP_ReadBackupRegister( Rtc_BkpReg( RTC_BKP_CTRL ) );
    stat = BKP_ReadBackupRegister( Rtc_BkpReg( RTC_BKP_STAT ) );

    RtcCtrlA = (uint8_t)( ctrl & 0x0F );
    RtcCtrlB = (uint8_t)( ctrl >> 8 );
    magic_ok = ( (uint8_t)( stat & 0xFF ) == RTC_MAGIC ) ? 1 : 0;
    RtcValid = ( ( stat >> 15 ) & 1 ) ? 1 : 0;

    if( magic_ok == 0 )
    {
        /* first start or the backup domain was reset: BCD data and 24 hour mode */
        RtcCtrlA = 0x00;
        RtcCtrlB = DS_B_24_12;
        RtcValid = 0;
    }

    for( k = 0; k < 10; k++ )
    {
        RtcShadow[ k ] = Rtc_BkpByteRead( RTC_BKP_SHADOW_FIRST, k );
    }
    RtcSetMode = ( RtcCtrlB & DS_B_SET ) ? 1 : 0;

    printf( "RTC: clock=%s, magic=%u, enabled=%u, cnt=%lu, VRT=%u\r\n",
            clock_lse ? "LSE" : "LSI", (unsigned int)magic_ok, (unsigned int)rtc_enabled,
            (unsigned long)RTC_GetCounter( ), (unsigned int)RtcValid );

    if( ( magic_ok == 0 ) || ( rtc_enabled == 0 ) || ( RTC_GetCounter( ) == 0 ) || ( RtcValid == 0 ) )
    {
        Rtc_SetInitTime( );
    }

    if( clock_lse == 0 )
    {
        printf( "RTC: warning: LSE is not ready, LSI is used (the time base is less accurate)\r\n" );
    }

    /* register dump: it also shows that the read path works */
    printf( "RTC regs: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x | A=%02x B=%02x C=%02x D=%02x\r\n",
            (unsigned int)rtc_read( DS_REG_SEC ),
            (unsigned int)rtc_read( DS_REG_SEC_ALARM ),
            (unsigned int)rtc_read( DS_REG_MIN ),
            (unsigned int)rtc_read( DS_REG_MIN_ALARM ),
            (unsigned int)rtc_read( DS_REG_HOUR ),
            (unsigned int)rtc_read( DS_REG_HOUR_ALARM ),
            (unsigned int)rtc_read( DS_REG_DAY_WEEK ),
            (unsigned int)rtc_read( DS_REG_DAY_MONTH ),
            (unsigned int)rtc_read( DS_REG_MONTH ),
            (unsigned int)rtc_read( DS_REG_YEAR ),
            (unsigned int)rtc_read( DS_REG_A ),
            (unsigned int)rtc_read( DS_REG_B ),
            (unsigned int)rtc_read( DS_REG_C ),
            (unsigned int)rtc_read( DS_REG_D ) );
}




