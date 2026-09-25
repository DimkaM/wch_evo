/********************************** (C) COPYRIGHT *******************************
* File Name          : button.h
* Version            : V1.0.0
* Description        : Push button input with a software debounce filter. The button closes the
*                      pin to GND, an external pull-up to VDD keeps it at 1 when released.
*                      Behaviour and wiring are described in PINS.md.
*******************************************************************************/
#ifndef __BUTTON_H
#define __BUTTON_H

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************/
/* Configuration switches */
#define DEF_BUTTON_EN               1       /* 1 - the button feature is enabled */
#define DEF_BTN_DEBOUNCE_MS         20      /* a new level is accepted after it stays stable so long */
#define DEF_BTN_LONG_MS             3000    /* a longer hold counts as a long press */
#define DEF_BTN_POLL_MS             2       /* recommended period of BTN_Update() calls */

/*******************************************************************************/
/* Pin: the button closes PC2 to GND, the external pull-up to VDD keeps it at 1 */
#define DEF_BTN_PORT_CTRL           GPIOC
#define DEF_BTN_PIN                 GPIO_Pin_2
#define DEF_BTN_LEVEL_DOWN          0       /* pressed (closed to GND) */
#define DEF_BTN_LEVEL_UP            1       /* released (pull-up) */

/*******************************************************************************/
/* Event type */
typedef enum
{
    BTN_EV_NONE = 0,        /* no confirmed level change */
    BTN_EV_DOWN,            /* confirmed press   */
    BTN_EV_UP               /* confirmed release */
} btn_event_t;

/*******************************************************************************/
/* Function Declaration */
extern void        BTN_Init( void );
extern btn_event_t BTN_Update( void );          /* poll every DEF_BTN_POLL_MS (or faster) */
extern uint8_t     BTN_IsReady( void );         /* 1 - at least one level was confirmed */
extern uint8_t     BTN_IsDown( void );          /* debounced state: 1 - pressed */
extern uint32_t    BTN_HoldMs( void );          /* how long the button is held right now */
extern uint32_t    BTN_LastHoldMs( void );      /* duration of the last confirmed press */

#ifdef __cplusplus
}
#endif

#endif /* __BUTTON_H */
