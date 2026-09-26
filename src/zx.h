/********************************** (C) COPYRIGHT *******************************
 * File Name          : zx.h
 * Version            : V1.0.0
 * Description        : Interchange with the FPGA registers (the ZX ports) via SPI, and the wait
 *                      port service. Ported from D:\src\pentevo\avr\baseconf\trunk\src (zx.h,
 *                      zx.c and main.h of the AVR project), the names are kept where possible:
 *                      zx_init( ), zx_spi_send( ), zx_wait_task( ), SPI_* register numbers,
 *                      ZXW_GLUK_CLOCK, FLAG_SPI_INT.
 *                      The other half of the contract is the FPGA: fpga/baseconf/trunk/slave/
 *                      slavespi.v (registers), zports.v / zwait.v (wait ports) and top.v
 *                      (.status_in({wr_n, waits[6:0]})). See FPGA_SPI.md for the full
 *                      description of the transaction.
 *******************************************************************************/
#ifndef __ZX_H
#define __ZX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"

/*******************************************************************************/
/* Configuration switches */

/* 1 - the ZX port service is used (the task, the interrupt and the register access) */
#define DEF_ZX_SPI_EN               1

/* 1 - the Gluk clock registers (see src/rtc.c) are served on the SPI interrupt */
#define DEF_ZX_GLUK_EN              1

/* The ZX-Evolution extensions of the Gluk clock (register A = EEPROM address, register C flags,
 * register D = keyboard state, the indexes >= 0xF0 = the version / EEPROM space). Enabled: the ZX
 * software uses them (see the log in FPGA_SPI.md). */
#define DEF_ZX_GLUK_EVO_EXT         1

/* 1 - the USB keyboard feeds the ZX keyboard matrix (SPI_KBD_DAT / SPI_KBD_STB; the AVR zx.c and
 * kbmap.c). The matrix of the 40 keys is built from the HID reports (src/USB_Host/app_km.c) and
 * strobed into the FPGA, which scans it as the port 0xFE of the Z80 (z80/zkbdmus.v, z80/zports.v).
 * The keys of the ZX-Evolution keyboard (its PS/2 port) are mapped to USB HID usages there. */
#define DEF_ZX_KBD_EN               1

/* ZX service task (FreeRTOS mode): priority and stack size in words. The priority is above the
 * USB host task (DEF_RTOS_USB_TASK_PRIO) and the power/FPGA task (DEF_FPGA_CONFIG_PRIO), because
 * the Z80 is held in a wait state until the port is served. Since the task runs above them, it
 * MUST block: DEF_ZX_TASK_POLL_TICKS is the safety timeout of the notification wait in RTOS ticks
 * (1 tick = 2 ms here; note that pdMS_TO_TICKS( 1 ) is 0 at 500 Hz and would turn the loop into a
 * busy spin which starves every lower priority task - observed on hardware), and while the FPGA
 * is not configured yet the task sleeps DEF_ZX_TASK_IDLE_MS per iteration. */
#define DEF_ZX_TASK_PRIO            5
#define DEF_ZX_TASK_STACK_WORDS     192
#define DEF_ZX_TASK_POLL_TICKS      1
#define DEF_ZX_TASK_IDLE_MS         20

/*******************************************************************************/
/* Register numbers of the FPGA SPI slave (zx.h of the AVR project, slave/slavespi.v) */

/** ZX keyboard data (40 bit, five bytes). */
#define SPI_KBD_DAT                 0x10
/** ZX keyboard strobe. */
#define SPI_KBD_STB                 0x11

/** ZX mouse X coordinate register. */
#define SPI_MOUSE_X                 0x20
/** ZX mouse Y coordinate register. */
#define SPI_MOUSE_Y                 0x21
/** ZX mouse buttons register. */
#define SPI_MOUSE_BTN               0x22
/** Kempston joystick register. */
#define SPI_KEMPSTON_JOYSTICK       0x23

/** ZX reset register (positive pulse to the Z80). */
#define SPI_RST_REG                 0x30

/** Data of a wait port: writing completes the Z80 wait, reading gives what the Z80 wrote. */
#define SPI_WAIT_DATA               0x40
/** Address which the Z80 put on the Gluk clock port. */
#define SPI_GLUK_ADDR               0x41
/** Address of the Kondratiev RS232 port. */
#define SPI_RS232_ADDR              0x42

/** Configuration register 0 (video/tape flags) and 1. */
#define SPI_CONFIG_REG              0x50
#define SPI_CONFIG1_REG             0x51
/** SD card data and control registers (the FPGA spihub). */
#define SPI_SDDATA_REG              0x60
#define SPI_SDCTRL_REG              0x61

/** NMI flag of the configuration register. */
#define SPI_CONFIG_NMI_FLAG         0x02
/** Tape input flag of the configuration register. */
#define SPI_TAPE_FLAG               0x04
/** Tape output mode flag of the configuration register. */
#define SPI_TAPEOUT_MODE_FLAG       0x08

/*******************************************************************************/
/* Wait ports: bits 6..0 of the status byte read in the address phase */

/** Gluk clock ZX port out. */
#define ZXW_GLUK_CLOCK              0x01
/** Kondratiev's modem ZX port out (not ported). */
#define ZXW_KONDR_RS232             0x02

/*******************************************************************************/
/* Flags of the common flag register (main.h of the AVR project) */

extern volatile uint8_t flags_register;
/** Spi interrupt detected (0 - not received / 1 - received). */
#define FLAG_SPI_INT                0x08
/** Last tape input value (used by zx_set_config); the tape support is not ported yet. */
#define FLAG_LAST_TAPE_VALUE        0x40

extern volatile uint8_t flags_ex_register;
/** NMI set (0 - not set / 1 - set); the NMI button is not ported yet. */
#define FLAG_EX_NMI                 0x04

/*******************************************************************************/
/* Modes of the ZX (main.h of the AVR project). The low three bits are also the LED byte of the
 * keyboard - the AVR sends it with PS2KEYBOARD_CMD_SETLED (ps2.c):
 *   bit 0 (MODE_VGA)     = "Scroll Lock" LED = the VGA video mode;
 *   bit 1 (MODE_TAPEOUT) = "Num Lock" LED    = the tapeout mode;
 *   bit 2 (MODE_CAPSLED) = "Caps Lock" LED   (driven from the ZX side through the Gluk register C).
 * "Scroll Lock" cycles the video mode (MODE_VIDEO_MASK), "Num Lock" toggles the tapeout mode -
 * exactly the additional key handling of zx.c of the AVR project. */
extern volatile uint8_t modes_register;
#define MODE_VGA                    0x01
#define MODE_TAPEOUT                0x02
#define MODE_CAPSLED                0x04
#define MODES_RASTER                0x30
#define MODE_VIDEO_MASK             ( MODE_VGA | MODES_RASTER )
#define MODE_LED_MASK               ( MODE_VGA | MODE_TAPEOUT | MODE_CAPSLED )

/*******************************************************************************/
/* The keyboard of the ZX (kbmap.h and zx.h of the AVR project, z80/zkbdmus.v of the FPGA) */

/* The ZX keyboard is a matrix of 8 half-rows (the A8..A15 lines of the port 0xFE) by 5 columns
 * (the Z80 data bits D0..D4). The FPGA keeps the whole matrix in a 40 bit register (kbd_reg,
 * written through SPI_KBD_DAT) and strobes it into the port engine (SPI_KBD_STB); z80/zkbdmus.v
 * reads that register as
 *
 *     keys[row][bit] = kbd[ 8 * ( 4 - bit ) + row ]
 *
 * so the bit index of a key inside the register is ( 8 * ( 4 - column ) + row ), and a set bit
 * means "pressed" (it pulls the Z80 data line low). The data path is confirmed by z80/zports.v
 * (dout = { 1'b1, tape_read, 1'b0, keys_in } - no bit swap) and by the AVR, whose SPI is LSB first
 * (SPCR = 0b01110000 in its spi.c, the same setting as our spi_init( )).
 *
 * The codes below are the AVR ones (kbmap.h): the code of a key is the value that the AVR puts into
 * its keyboard FIFO, i.e. ( 4 - column ) * 8 + ( 7 - row ). zx_kbd_key( ) converts such a code into
 * the bit of the register, and the very same expression converts a bit back into the code. */
#define KEY_SP                      0
#define KEY_EN                      1
#define KEY_P                       2
#define KEY_0                       3
#define KEY_1                       4
#define KEY_Q                       5
#define KEY_A                       6
#define KEY_CS                      7
#define KEY_SS                      8
#define KEY_L                       9
#define KEY_O                       10
#define KEY_9                       11
#define KEY_2                       12
#define KEY_W                       13
#define KEY_S                       14
#define KEY_Z                       15
#define KEY_M                       16
#define KEY_K                       17
#define KEY_I                       18
#define KEY_8                       19
#define KEY_3                       20
#define KEY_E                       21
#define KEY_D                       22
#define KEY_X                       23
#define KEY_N                       24
#define KEY_J                       25
#define KEY_U                       26
#define KEY_7                       27
#define KEY_4                       28
#define KEY_R                       29
#define KEY_F                       30
#define KEY_C                       31
#define KEY_B                       32
#define KEY_H                       33
#define KEY_Y                       34
#define KEY_6                       35
#define KEY_5                       36
#define KEY_T                       37
#define KEY_G                       38
#define KEY_V                       39

/** Not a ZX key (the AVR NO_KEY). */
#define NO_KEY                      0x7F
/** "Drop the whole keyboard matrix" event (the AVR CLRKYS, produced by its ESC key). */
#define CLRKYS                      0x7A

/** Bit 7 of a key event: the key is pressed (the AVR PRESS_MASK). */
#define PRESS_MASK                  0x80
#define KEY_MASK                    0x7F

/** The bit of the 40 bit keyboard register which belongs to a key code (see above). The expression
 *  is its own inverse, so it converts a code into a bit and a bit back into a code. */
#define ZX_KBD_BIT( code )          ( ( (code) & 0xF8 ) | ( 7 - ( (code) & 0x07 ) ) )

/*******************************************************************************/
/* Function Declaration */

/* Initialises the ZX part: the SPI link (spi_init( )) and the Z80 reset through the FPGA, exactly
 * like zx_init( ) of the AVR runs after the configuration. Called when FPGA_Config( ) succeeded. */
extern void    zx_init( void );

/* Exchange with one FPGA register: the AVR zx_spi_send( ) of zx.c. Returns the byte received in
 * the data phase (the previous content of the register). The caller has to hold the SPI bus lock
 * (spi_lock( )), because zx_wait_task( ) calls this function again. */
extern uint8_t zx_spi_send( uint8_t addr, uint8_t data, uint8_t mask );

/* Serves one wait port access reported by the FPGA, i.e. the AVR zx_wait_task( ). */
extern void    zx_wait_task( uint8_t status );

/* Clears the ZX keyboard matrix (zx_clr_kb( ) of the AVR); the cleared matrix is then sent to the
 * FPGA as well. Used on the initialisation and for the CLRKYS event (the ESC key of the AVR). */
extern void    zx_clr_kb( void );

/* Reports one key event of the USB keyboard: zxcode is a KEY_* code, pressed != 0 means "pressed"
 * (the AVR update_keys( ) with its per-key counter, so that several keyboards may hold the same ZX
 * key). Only the matrix state is changed here - the transfer is done by the ZX task, because this
 * function is called from the USB report path, which must not block on the SPI bus. */
extern void    zx_kbd_key( uint8_t zxcode, uint8_t pressed );

/* Transfers the matrix to the FPGA when it changed: the five SPI_KBD_DAT bytes and the SPI_KBD_STB
 * strobe (the keyboard part of zx_task( ) of the AVR: "send order: LSbit first, from [4] to [0]").
 * Called by zx_service( ), i.e. from the ZX task or from the super loop of main( ). */
extern void    zx_kbd_task( void );

/* Sends the current modes to the configuration register of the FPGA (zx_set_config( ) of the AVR).
 * "flags" carries the extra bits (the tape input flag); the video mode, the tapeout mode and the
 * NMI flag are taken from modes_register / flags_ex_register. */
extern void    zx_set_config( uint8_t flags );

/* Inverts the mode bits, sends the configuration to the FPGA, saves the modes into the NVRAM and
 * refreshes the keyboard LEDs (zx_mode_switcher( ) of the AVR). Called from the keyboard handler
 * for "Scroll Lock" (the video mode) and "Num Lock" (the tapeout mode), see app_km.c. */
extern void    zx_mode_switcher( uint8_t mode );

/* Polls FLAG_SPI_INT and serves the request: the corresponding block of the AVR main loop. In the
 * FreeRTOS mode it is called from the ZX task (zx_task_start( )), in the bare-metal mode from the
 * super loop of main( ). */
extern void    zx_service( void );

/* Creates the bus lock and starts the ZX service task (FreeRTOS mode only). */
extern void    zx_task_start( void );

#ifdef __cplusplus
}
#endif

#endif /* __ZX_H */
