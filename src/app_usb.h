/********************************** (C) COPYRIGHT *******************************
* File Name          : app_usb.h
* Version            : V1.0.0
* Description        : USB host stack policy with respect to the ATX power state, the switches
*                      live in src/USB_Host/usb_host_config.h (DEF_USB_*). See PINS.md.
*******************************************************************************/
#ifndef __APP_USB_H
#define __APP_USB_H

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************/
/* Function Declaration */
extern void AppUsb_Init( void );            /* initialise the stack according to the PSU state */
extern void AppUsb_RequestRestart( void );  /* ask for a restart, serviced by AppUsb_Step() */
extern void AppUsb_Step( void );            /* stop/start by PWR_OK, restart requests and poll */

#ifdef __cplusplus
}
#endif

#endif /* __APP_USB_H */
