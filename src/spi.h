/********************************** (C) COPYRIGHT *******************************
 * File Name          : spi.h
 * Version            : V1.0.0
 * Description        : SPI link to the FPGA (the register / wait port interface of the
 *                      ZX-Evolution base configuration). The interface and the names are ported
 *                      from the AVR project D:\src\pentevo\avr\baseconf\trunk\src (spi.c, pins.h):
 *                      spi_init( ), spi_send( ), nSPICS. The protocol itself belongs to
 *                      fpga/baseconf/trunk/slave/slavespi.v - see FPGA_SPI.md.
 *                      Unlike the AVR, whose SPI is used for nothing else, the same SPI1 is used
 *                      by the FPGA configuration (src/fpga.c). Therefore the link is
 *                      re-initialised after every configuration (FPGA_Config( ) calls
 *                      spi_deinit( ) / spi_init( )) and the bus is protected by spi_lock( ) /
 *                      spi_unlock( ) while several tasks may use it.
 *******************************************************************************/
#ifndef __SPI_H
#define __SPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ch32v30x.h"
#include "ch32v30x_gpio.h"
#include "ch32v30x_spi.h"
#include "ch32v30x_exti.h"

/*******************************************************************************/
/* Configuration switches */

/* 1 - the FPGA SPI link (ZX ports, see src/zx.c) is used, 0 - SPI1 belongs to the FPGA
 * configuration only (the feature can be switched off completely) */
#define DEF_ZX_SPI_EN               1

/* SPI clock: 96 MHz / 16 = 6 MHz. The AVR ran its hardware SPI at Fosc/2 = 5.53 MHz (Fosc =
 * 11.0592 MHz), and slavespi.v samples SCK with its own fclk through 2 - 3 synchronizer stages,
 * so SCK has to stay well below fclk. Do not raise this without a measurement: 12 MHz (i.e.
 * SPI_BaudRatePrescaler_8) is very likely above the limit of that slave (see FPGA_SPI.md). */
#define DEF_ZX_SPI_PRESCALER        SPI_BaudRatePrescaler_16

/* Debug output of the ZX port service:
 *   0 - no per access / per report lines, only the compact summary which src/zx.c prints every
 *       DEF_ZX_STAT_MS ms (the request counters of the ports, of the keyboard and of the mouse);
 *   1 - one line per changed port (an address or a direction which differs from the previous
 *       access) and per mouse report which changed the buttons or the wheel: a driver stays
 *       visible without the flood of a polling loop;
 *   2 - one line per access / report (the raw trace). Careful: every line holds the Z80 in its
 *       wait state for the time the line takes at 115200 (about 5 ms), and a ZX program which
 *       polls a register in a loop produces thousands of such lines per second. */
#define DEF_ZX_SPI_DEBUG            1
#define DEF_ZXSPI_TRACE             0

/* Period of the summary line of the ZX port service, ms (0 - no summary). It is printed by the ZX
 * task from zx_service( ) and needs g_ms_ticks (TIM3, started by main( )). */
#define DEF_ZX_STAT_MS              5000

/* The FPGA samples nSPICS (the chip select of its SPI slave) with its own fclk of about 21 MHz, so
 * a CS level which lasts only a few CPU cycles - the GPIO registers are written within ~10 ns at
 * 144 MHz - can be missed by the synchroniser of the FPGA or seen as a spike. A spike on the chip
 * select strobes whatever register was addressed with a half shifted value: observed on hardware,
 * pressing "8" repeatedly produced "m" now and then, i.e. the keyboard register was latched three
 * clocks too early. Every CS edge is therefore held for ZX_CS_EDGE_DELAY_US microseconds, which the
 * AVR got for free from its much slower GPIO operations. */
#define ZX_CS_EDGE_DELAY_US         2

/*******************************************************************************/
/* Pins (the names of the AVR pins.h are kept where possible) */

/* nSPICS - the "chip select" of the FPGA SPI slave. It is not a plain chip select: while the
 * line is high the FPGA shifts out the status byte and latches the register number, while it is
 * low the data byte is exchanged (see slave/slavespi.v). The AVR toggles it in the same way in
 * zx_spi_send( ) (avr .../src/zx.c). */
#define nSPICS                      GPIO_Pin_6
#define nSPICS_PORT                 GPIOB
#define nSPICS_PIN                  GPIOB

#define SPI_PORT                    GPIOB
#define SPI_SCK_PIN                 GPIO_Pin_3      /* the same pin as the FPGA DCLK  */
#define SPI_MOSI_PIN                GPIO_Pin_5      /* the same pin as the FPGA DATA0 */
#define SPI_MISO_PIN                GPIO_Pin_4      /* full remap of SPI1: MISO = PB4 */

/* spiint_n - the FPGA asks the MCU to service a wait port (the AVR used INT6) */
#define SPI_IRQ_PIN                 GPIO_Pin_7
#define SPI_IRQ_PORT                GPIOB
#define SPI_IRQ_EXTI_LINE           EXTI_Line7
#define SPI_IRQ_PIN_SOURCE          GPIO_PinSource7
#define SPI_IRQ_IRQn                EXTI9_5_IRQn

/*******************************************************************************/
/* Function Declaration */

/* (Re)initialise the link for the register protocol: PB3/PB5 alternate function, PB4 input,
 * nSPICS = 1, PB7 as a falling edge interrupt, SPI1 in mode 0 with the LSB first (exactly as
 * spi_init( ) / SPCR of the AVR). Called from FPGA_Config( ) when the FPGA is configured. */
extern void    spi_init( void );

/* Opposite of spi_init( ): nSPICS is held high, the interrupt is masked and the service request
 * is dropped. Called from FPGA_Config( ) before the bitstream is sent. */
extern void    spi_deinit( void );

/* 1 - the link is initialised (spi_init( ) has run), 0 - not ready yet */
extern uint8_t spi_ready( void );

/* One byte exchange (the AVR spi_send( )): waits for the transmit register, sends the byte and
 * returns the received one. 0xFF is returned when the peripheral does not answer (timeout). */
extern uint8_t spi_send( uint8_t byte );

/* Bus lock: SPI1 is shared with the FPGA configuration task. spi_lock_init( ) must be called
 * once before the scheduler starts, the lock itself is taken by the callers of zx_spi_send( ) -
 * the low level functions do not lock, because zx_wait_task( ) calls zx_spi_send( ) again.
 * IMPORTANT: spi_lock( ) must never be taken with the scheduler suspended: the USB report path
 * runs inside USBH_MainDeal( ) under vTaskSuspendAll( ), and FreeRTOS then asserts "Cannot block
 * if the scheduler is suspended" (queue.c). Such callers only set a flag (ZxConfigPending) and the
 * "zx" task performs the transfer, see zx_mode_switcher( ) in src/zx.c. */
/* Number of the timeouts of spi_send( ) since the last spi_init( ) (diagnostic: a timeout means
 * that the FPGA did not answer, 0xFF is returned then, see spi_send( ) and the summary line of
 * src/zx.c). */
extern uint32_t spi_timeout_count( void );

extern void    spi_lock_init( void );
extern void    spi_lock( void );
extern void    spi_unlock( void );

#ifdef __cplusplus
}
#endif

#endif /* __SPI_H */
