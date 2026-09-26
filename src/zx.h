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
 * register D = keyboard state, the addresses >= 0xF0). Not ported yet: the extensions belong to
 * the PS/2 keyboard layer. 0 - only the plain DS12887 behaviour is served. */
#define DEF_ZX_GLUK_EVO_EXT         0

/* ZX service task (FreeRTOS mode): priority and stack size in words. The priority is above the
 * USB host task (DEF_RTOS_USB_TASK_PRIO) and the power/FPGA task (DEF_FPGA_CONFIG_PRIO), because
 * the Z80 is held in a wait state until the port is served. */
#define DEF_ZX_TASK_PRIO            5
#define DEF_ZX_TASK_STACK_WORDS     192

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
