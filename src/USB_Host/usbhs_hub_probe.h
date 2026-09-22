/********************************** (C) COPYRIGHT  *******************************
* File Name          : usbhs_hub_probe.h
* Description        : Header of the experimental USBHS SPLIT transaction probe.
*********************************************************************************/

#ifndef __USBHS_HUB_PROBE_H
#define __USBHS_HUB_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

#if DEF_USBHS_HUB_SPLIT_PROBE
extern void USBHS_HubSplitProbe( void );
#endif

#ifdef __cplusplus
}
#endif

#endif