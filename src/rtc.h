/********************************** (C) COPYRIGHT *******************************
* File Name          : rtc.h
* Version            : V1.0.0
* Description        : Dallas DS12887 (IBM PC compatible clock) register file emulation on top of
*                      the CH32V317 RTC counter and the BKP registers.
*                      Alarm registers are not emulated, the control registers A/B/C/D are
*                      implemented as far as the hardware allows (see RTC.md).
*******************************************************************************/
#ifndef __RTC_H
#define __RTC_H

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************/
/* Configuration switches */

/* 1 - rtc_init() sets up the RTC clock, checks the validity of the time and sets the time
 *     below when there is none. 0 - the register interface is used without touching the RTC. */
#define DEF_RTC_INIT_EN             1

/* Time written when no valid time is found: 26.09.2026 08:56:37 (as given, no time zone) */
#define DEF_RTC_INIT_YEAR           2026
#define DEF_RTC_INIT_MONTH          9
#define DEF_RTC_INIT_DAY            26
#define DEF_RTC_INIT_HOUR           8
#define DEF_RTC_INIT_MIN            56
#define DEF_RTC_INIT_SEC            37

/* Maximum wait for the 32.768 kHz oscillator (LSE) to become ready; if it fails the internal
 * LSI is used instead (less accurate, a warning is printed). */
#define DEF_RTC_LSE_WAIT_MS         1000

/* Emulated update-in-progress window in RTC divider ticks (32768 ticks = 1 s): register A
 * reports UIP while the last DEF_RTC_UIP_TICKS ticks of a second are running. */
#define DEF_RTC_UIP_TICKS           700

/*******************************************************************************/
/* DS12887 registers (the PC/AT clock) */
#define DS_REG_SEC                  0x00
#define DS_REG_SEC_ALARM            0x01
#define DS_REG_MIN                  0x02
#define DS_REG_MIN_ALARM            0x03
#define DS_REG_HOUR                 0x04
#define DS_REG_HOUR_ALARM           0x05
#define DS_REG_DAY_WEEK             0x06
#define DS_REG_DAY_MONTH            0x07
#define DS_REG_MONTH                0x08
#define DS_REG_YEAR                 0x09
#define DS_REG_A                    0x0A
#define DS_REG_B                    0x0B
#define DS_REG_C                    0x0C
#define DS_REG_D                    0x0D
#define DS_REG_NVRAM_FIRST          0x0E
#define DS_REG_NVRAM_LAST           0x3F

/* Register A bits */
#define DS_A_UIP                    0x80    /* update in progress (emulated from the divider) */
#define DS_A_DV                     0x20    /* DV2..0 = 010 -> 32.768 kHz time base */
/* Register B bits */
#define DS_B_SET                    0x80    /* 1 = clock stopped, the time registers can be written */
#define DS_B_PIE                    0x40    /* stored, no interrupt is generated */
#define DS_B_AIE                    0x20    /* stored, no interrupt is generated */
#define DS_B_UIE                    0x10    /* stored, no interrupt is generated */
#define DS_B_SQWE                   0x08    /* stored, no square wave is generated */
#define DS_B_DM                     0x04    /* 1 = binary data, 0 = BCD */
#define DS_B_24_12                  0x02    /* 1 = 24 hour mode, 0 = 12 hour mode */
#define DS_B_DSE                    0x01    /* daylight saving enable (stored) */
/* Register C bits */
#define DS_C_IRQF                   0x80
#define DS_C_PF                     0x40
#define DS_C_AF                     0x20
#define DS_C_UF                     0x10    /* update ended flag, set once per second */
/* Register D bits */
#define DS_D_VRT                    0x80    /* valid RAM and time */

/*******************************************************************************/
/* BKP registers used by the emulation (BKP_DR1..BKP_DR42 are available, see RTC.md):
 *   DR1 .. DR25  -> NVRAM, the DS12887 registers 0x0E..0x3F (2 bytes each)
 *   DR26         -> settings:  low byte = register A, high byte = register B
 *   DR27         -> status:    low byte = magic 0x5A, high byte = VRT (bit 7) + format version
 *   DR28 .. DR32 -> shadow of the time registers 0x00..0x09 while SET = 1 (2 bytes each)
 *   DR33 .. DR42 -> free for future use */
#define RTC_BKP_NVRAM_FIRST         1
#define RTC_BKP_NVRAM_REGS          25
#define RTC_BKP_CTRL                26
#define RTC_BKP_STAT                27
#define RTC_BKP_SHADOW_FIRST        28
#define RTC_BKP_SHADOW_REGS         5
/* The AVR project has extra NVRAM cells at the addresses 0xFD..0xFF (its RTC chip has more memory
 * than the DS12887): the PS/2 mouse resolution, the common modes and the extra year data. They are
 * mapped to the free BKP registers DR33..DR35 (byte index 0,1 -> DR33, 2,3 -> DR34, ...). */
#define RTC_BKP_EXTRA_FIRST         33
/* DR33 .. DR42 -> free for future use */

/*******************************************************************************/
/* Gluk clock (the ZX-Evolution clock served at the Z80 ports). Ported from the AVR project
 * (D:\src\pentevo\avr\baseconf\trunk\src\rtc.h and rtc.c) with the same names: gluk_regs[14],
 * gluk_init( ), gluk_get_reg( ), gluk_set_reg( ), gluk_inc( ). The indexes 0x00..0x0D are the
 * DS12887 registers above (the GLUK_* names alias the DS_REG_* ones), 0x0E..0x3F is its NVRAM -
 * both are served by rtc_read( ) / rtc_write( ). The register access over SPI is in src/zx.c. */
#define GLUK_REG_SEC                DS_REG_SEC
#define GLUK_REG_SEC_ALARM          DS_REG_SEC_ALARM
#define GLUK_REG_MIN                DS_REG_MIN
#define GLUK_REG_MIN_ALARM          DS_REG_MIN_ALARM
#define GLUK_REG_HOUR               DS_REG_HOUR
#define GLUK_REG_HOUR_ALARM         DS_REG_HOUR_ALARM
#define GLUK_REG_DAY_WEEK           DS_REG_DAY_WEEK
#define GLUK_REG_DAY_MONTH          DS_REG_DAY_MONTH
#define GLUK_REG_MONTH              DS_REG_MONTH
#define GLUK_REG_YEAR               DS_REG_YEAR
#define GLUK_REG_A                  DS_REG_A
#define GLUK_REG_B                  DS_REG_B
#define GLUK_REG_C                  DS_REG_C
#define GLUK_REG_D                  DS_REG_D
#define GLUK_REG_NVRAM_FIRST        DS_REG_NVRAM_FIRST
#define GLUK_REG_NVRAM_LAST         DS_REG_NVRAM_LAST

/* Register B bits (the AVR names) */
#define GLUK_B_DATA_MODE            DS_B_DM         /* 1 = binary data, 0 = BCD data */
#define GLUK_B_24_12_MODE           DS_B_24_12      /* 1 = 24 hour mode, 0 = 12 hour mode */

/* Register C bits: the update flag is emulated, the others are the ZX-Evolution extensions
 * (register C bit 0 is the NUM LED state on read and "clear the PS/2 keyboard log" on write,
 * bit 1 switches the CAPS LED, bit 7 enables the EEPROM mode of the indexes >= 0xF0). They
 * belong to the PS/2 keyboard layer and are not served yet (DEF_ZX_GLUK_EVO_EXT in src/zx.h). */
#define GLUK_C_UPDATE_FLAG          DS_C_UF
#define GLUK_C_NUM_LED_FLAG         0x01
#define GLUK_C_CAPS_LED_FLAG        0x02
#define GLUK_C_EEPROM_FLAG          0x80

/* Initial values of the Gluk registers (the AVR project) */
#define GLUK_A_INIT_VALUE           0x00
#define GLUK_B_INIT_VALUE           0x02
#define GLUK_C_INIT_VALUE           0x00
#define GLUK_D_INIT_VALUE           0x80

/* The AVR keeps these in the extra cells of its RTC chip. They are not mapped yet: they belong to
 * the ZX-Evolution extensions (see DEF_ZX_GLUK_EVO_EXT in src/zx.h and FPGA_SPI.md). */
#define RTC_YEAR_ADD_REG            0xFF
#define RTC_COMMON_MODE_REG         0xFE
#define RTC_PS2MOUSE_RES_REG        0xFD

/*******************************************************************************/
/* Function Declaration */
extern void    rtc_init( void );                        /* RTC clock, validity, automatic time */
extern uint8_t rtc_read( uint8_t addr );                /* DS12887 register read  */
extern void    rtc_write( uint8_t addr, uint8_t data ); /* DS12887 register write */

/* The Gluk clock register file (the AVR rtc.h / rtc.c names). gluk_regs holds the last values of
 * the registers 0x00..0x0D for diagnostics; the model itself is the DS12887 emulation above, so
 * gluk_get_reg( ) / gluk_set_reg( ) work on the whole 0x00..0x3F range. */
extern uint8_t gluk_regs[ 14 ];
extern void    gluk_init( void );
extern void    gluk_inc( void );
extern uint8_t gluk_get_reg( uint8_t index );
extern void    gluk_set_reg( uint8_t index, uint8_t data );

#ifdef __cplusplus
}
#endif

#endif /* __RTC_H */
