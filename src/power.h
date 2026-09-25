/********************************** (C) COPYRIGHT *******************************
* File Name          : power.h
* Version            : V1.0.0
* Description        : ATX power supply control. POWER_ON (PC0) drives the base of an
*                      NPN key whose collector pulls PS_ON# low, POWER_GOOD (PC1) reads
*                      the PWR_OK feedback. The PSU is switched on at startup after the
*                      USB host stack is up, the FPGA is configured only when PWR_OK is
*                      present. Wiring and levels are described in PINS.md.
*******************************************************************************/
#ifndef __POWER_H
#define __POWER_H

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************/
/* Configuration switches */
#define DEF_POWER_EN                1       /* 1 - ATX control enabled, 0 - PSU untouched */
#define DEF_POWER_GOOD_TIMEOUT_MS   500     /* ATX: PWR_OK comes within 100...500 ms */

/* Margin after PWR_OK before the FPGA configuration: the local regulators of the FPGA board and
 * the power-on reset of the device need some time after the ATX rails are reported good. With a
 * too short margin the FPGA may answer with a nSTATUS timeout (observed on hardware). */
#define DEF_POWER_SETTLE_MS         300

/* Before switching the PSU on again, wait until PWR_OK falls: the rails have to discharge, so the
 * FPGA gets a clean power cycle instead of a marginal one (also observed on hardware). */
#define DEF_POWER_OFF_CONFIRM_MS    2000

#define DEF_POWER_RETRY             1       /* 1 - one PSU power cycle retry after a timeout */

/*******************************************************************************/
/* Pins */
/* POWER_ON -> 1 kOhm -> base of an NPN (emitter to GND, collector to PS_ON#):
 * 1 = the key is open and pulls PS_ON# low (PSU on), 0 = the key is closed (PSU off).
 * POWER_GOOD <- PWR_OK (+5 V, divided by 12k/20k if the divider is fitted), active high
 * input with the internal pull-down, so a disconnected line reads as 0. */
#define PWR_PORT_CTRL               GPIOC
#define PWR_PIN_ON                  GPIO_Pin_0
#define PWR_PIN_GOOD                GPIO_Pin_1
#define PWR_PIN_ON_ASSERT()         GPIO_SetBits( PWR_PORT_CTRL, PWR_PIN_ON )
#define PWR_PIN_ON_RELEASE()        GPIO_ResetBits( PWR_PORT_CTRL, PWR_PIN_ON )

/*******************************************************************************/
/* Function Declaration */
extern void    POWER_Init( void );      /* keeps the PSU off, call once at startup */
extern uint8_t POWER_On( void );        /* 1 = PWR_OK received (settle delay included) */
extern void    POWER_Off( void );       /* releases PS_ON#, the PSU turns off */
extern uint8_t POWER_IsGood( void );    /* 1 = PWR_OK is high right now */

#ifdef __cplusplus
}
#endif

#endif /* __POWER_H */
