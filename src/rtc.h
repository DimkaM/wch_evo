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

/*******************************************************************************/
/* Function Declaration */
extern void    rtc_init( void );                        /* RTC clock, validity, automatic time */
extern uint8_t rtc_read( uint8_t addr );                /* DS12887 register read  */
extern void    rtc_write( uint8_t addr, uint8_t data ); /* DS12887 register write */

#ifdef __cplusplus
}
#endif

#endif /* __RTC_H */
