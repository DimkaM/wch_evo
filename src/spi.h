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

/* Debug output of the ZX port service (one line per serviced port) */
#define DEF_ZX_SPI_DEBUG            1

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
 * the low level functions do not lock, because zx_wait_task( ) calls zx_spi_send( ) again. */
extern void    spi_lock_init( void );
extern void    spi_lock( void );
extern void    spi_unlock( void );

#ifdef __cplusplus
}
#endif

#endif /* __SPI_H */
