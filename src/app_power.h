/********************************** (C) COPYRIGHT *******************************
* File Name          : app_power.h
* Version            : V1.0.0
* Description        : Application sequence of the ATX power supply, the FPGA configuration and the
*                      push button (see app_power.c and PINS.md).
*******************************************************************************/
#ifndef __APP_POWER_H
#define __APP_POWER_H

/* The switches of the ATX driver (DEF_POWER_EN, DEF_POWER_*_MS) and of the button driver
 * (DEF_BUTTON_EN, DEF_BTN_*) are owned by these headers: including them here guarantees that a
 * file which only includes app_power.h still sees the macros (e.g. for #if guards). */
#include "power.h"
#include "button.h"

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************/
/* Configuration switches */
/* 1 - the PSU is switched on automatically DEF_FPGA_CONFIG_DELAY_MS after the start (by that time
 *     the USB host stack is initialised), 0 - the board waits for a button press. */
#define DEF_POWER_AUTO_ON           1

/* After a power off the button is ignored until PWR_OK falls (the output rails discharge), this is
 * the maximum wait for that. */
#define DEF_PWR_OFF_LOCKOUT_MS      1000

/* Bring-up aid: print the raw levels of the power/button pins (PC0/PC1/PC2) once per
 * DEF_PINS_DEBUG_MS. Comparing them with a multimeter shows a broken or swapped wire
 * ("drv" is the level the MCU drives on PC0, "pin" is what the pin really reads back).
 * Set to 0 for a quiet log, 1 when the wiring has to be checked. */
#define DEF_PINS_DEBUG              0
#define DEF_PINS_DEBUG_MS           1000

/*******************************************************************************/
/* Function Declaration */
extern void AppPower_Init( void );                  /* ATX pins + button, once at startup */
extern void AppPower_Startup( void );               /* switch the PSU on and configure the FPGA */
extern void AppPower_Step( void );                  /* button service, call every DEF_BTN_POLL_MS */
extern void AppPowerTask( void *pvParameters );     /* FreeRTOS task, never returns */

#ifdef __cplusplus
}
#endif

#endif /* __APP_POWER_H */
