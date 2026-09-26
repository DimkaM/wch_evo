/********************************** (C) COPYRIGHT  *******************************
 * File Name          : usb_host_config.h
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2022/08/29
 * Description        : 
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/


#ifndef __USB_HOST_CONFIG_H
#define __USB_HOST_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/* Header File */
#include "string.h"
#include "debug.h"
#include "ch32v30x_usb.h"
#include "ch32v30x_usbfs_host.h"
#include "usb_host_hid.h"
#include "usb_host_hub.h"
#include "app_km.h"

/******************************************************************************/
/* Debug Macro Definition */
#define DEF_DEBUG_PRINTF            1
#if ( DEF_DEBUG_PRINTF == 1 )
#define DUG_PRINTF( format, arg... )    printf( format, ##arg )
#else
#define DUG_PRINTF( format, arg... )    do{ if( 0 )printf( format, ##arg ); }while( 0 );
#endif

/* Additional debug switches, they require DEF_DEBUG_PRINTF = 1:
 * DEF_DEBUG_HUB_SCAN  : details of the HUB port scan (one line per HUB port, per change event)
 * DEF_DEBUG_HID_REPORT: dump of every HID input report of the keyboard/mouse devices (very
 *                       verbose, one line per report interval) */
#define DEF_DEBUG_HUB_SCAN          1
#define DEF_DEBUG_HID_REPORT        1

/******************************************************************************/
/* USB Host Communication Related Macro Definition */

/* USB Host Port General Control.
 * Note: only the USBFS port is used by this project. The USBHS (high-speed USBHD) port
 * support was removed with the branch "remove_usbhs_support": the high-speed port cannot
 * serve a HUB (no PRE token and no usable SPLIT transactions), while the USBFS port covers
 * full/low/high speed devices behind a HUB (see RESEARCH_CONCLUSION.md). */
#define DEF_TOTAL_ROOT_HUB          1
#define DEF_USBFS_PORT_EN           1
#define DEF_USBFS_PORT_INDEX        0x00
#define DEF_ONE_USB_SUP_DEV_TOTAL   5
#define DEF_NEXT_HUB_PORT_NUM_MAX   4

/* Number of automatic retries of the enumeration of a device behind a HUB port whose port reset
 * or enumeration failed. While retrying, the HUB port change bits are kept pending so that the
 * port is scanned again, but the number of retries is bounded: an unsupported/broken device must
 * not reset the port and flood the log forever (see HUB_Port_ClearChanges( ) in app_km.c). */
#define DEF_HUB_ENUM_RETRY_MAX      3
#define DEF_INTERFACE_NUM_MAX       4

/* FreeRTOS mode (see src/app_tasks.c): 1 - the application runs under FreeRTOS,
 * 0 - the original bare-metal mode, where main() calls USBH_MainDeal() in a super loop.
 * Note: in the FreeRTOS mode the global interrupt is enabled by the scheduler when the
 * first task is started, and the delay functions must not use SysTick any more, because
 * SysTick is the RTOS tick. */
#define DEF_FREERTOS_EN             1

/* USB stack policy with respect to the ATX power state (see src/app_usb.c and PINS.md).
 * DEF_USB_OFF_WHEN_PSU_OFF:       1 - while POWER_GOOD is absent (PSU off) the host stack is
 *                                     stopped (USBFS_Host_Init(DISABLE)) and USBH_MainDeal() is
 *                                     not called at all; when the PSU reports PWR_OK the stack is
 *                                     initialised again (full re-enumeration);
 *                                 0 - the stack runs independently of the PSU state.
 * DEF_USB_RESTART_ON_POWER_ON:    1 - every POWER_GOOD 0 -> 1 transition restarts the stack
 *                                     (re-enumeration), also when OFF_WHEN_PSU_OFF is 0;
 *                                 0 - no restart, the running stack handles reconnections itself.
 * DEF_USB_RESTART_ON_FPGA_CONFIG: 1 - every FPGA_Config() (the startup one and the button
 *                                     re-flash) is followed by a USB stack restart, because the
 *                                     FPGA reconfiguration may disturb the USB part;
 *                                 0 - the FPGA configuration does not touch the USB stack.
 * DEF_USB_RESTART_DELAY_MS:       settle time between the restart request and the restart. */
#define DEF_USB_OFF_WHEN_PSU_OFF        1
#define DEF_USB_RESTART_ON_POWER_ON     1
#define DEF_USB_RESTART_ON_FPGA_CONFIG  0
#define DEF_USB_RESTART_DELAY_MS        0

/* USB host task (gate G4). The task initializes both host ports and then calls
 * USBH_MainDeal() with the scheduler suspended, so an enumeration or a blocking
 * transaction is not interrupted by other tasks (hardware interrupts, TIM3 in particular,
 * keep running). Stack size is in words, the delay is in RTOS ticks. */
#define DEF_RTOS_USB_TASK_STACK_WORDS   1024
#define DEF_RTOS_USB_TASK_PRIO          2
#define DEF_RTOS_USB_DELAY_TICKS        1

/* Bring-up task of the first gates (G1/G2). It measures the real duration of
 * DEF_RTOS_TEST_DELAY_TICKS RTOS ticks against the 1 ms counter of TIM3 (g_ms_ticks),
 * so the effective tick rate can be read directly from the serial log.
 * Stack size is in words, priority is idle + 1. */
#define DEF_RTOS_TEST_TASK          1
#define DEF_RTOS_TEST_DELAY_TICKS   1000
#define DEF_RTOS_TEST_STACK_WORDS   256
#define DEF_RTOS_TEST_PRIO          1




/* USB Root Device Status */
#define ROOT_DEV_DISCONNECT         0
#define ROOT_DEV_CONNECTED          1
#define ROOT_DEV_FAILED             2
#define ROOT_DEV_SUCCESS            3

/* USB Device Address */
#define USB_DEVICE_ADDR             0x02

/* USB Speed */
#define USB_LOW_SPEED               0x00
#define USB_FULL_SPEED              0x01
#define USB_HIGH_SPEED              0x02
#define USB_SPEED_CHECK_ERR         0xFF

/* Configuration Descriptor Type */
#define DEF_DECR_CONFIG             0x02
#define DEF_DECR_INTERFACE          0x04
#define DEF_DECR_ENDPOINT           0x05
#define DEF_DECR_HID                0x21

/* USB Communication Status Code */
#define ERR_SUCCESS                 0x00
#define ERR_USB_CONNECT             0x15
#define ERR_USB_DISCON              0x16
#define ERR_USB_BUF_OVER            0x17
#define ERR_USB_DISK_ERR            0x1F
#define ERR_USB_TRANSFER            0x20
#define ERR_USB_UNSUPPORT           0xFB
#define ERR_USB_UNAVAILABLE         0xFC
#define ERR_USB_UNKNOWN             0xFE

/* USB Device Enumeration Status Code */
#define DEF_DEV_DESCR_GETFAIL       0x45
#define DEF_DEV_ADDR_SETFAIL        0x46
#define DEF_CFG_DESCR_GETFAIL       0x47
#define DEF_REP_DESCR_GETFAIL       0x48
#define DEF_CFG_VALUE_SETFAIL       0x49
#define DEF_DEV_TYPE_UNKNOWN        0xFF
                       
/* USB Communication Time */
#define DEF_BUS_RESET_TIME          11          // USB bus reset time
#define DEF_RE_ATTACH_TIMEOUT       100         // Wait for the USB device to reconnect after reset, 100mS timeout
#define DEF_WAIT_USB_TRANSFER_CNT   65500        // Wait for the USB transfer to complete
#define DEF_CTRL_TRANS_TIMEOVER_CNT 200000/20   // Control transmission delay timing


/*******************************************************************************/
/* Struct Definition */
/* Note: Please modify it according to your project. */

/* HUB Port Device  */
typedef struct _HUB_DEVICE
{
    uint8_t  bStatus;
    uint8_t  bType;
    uint8_t  bAddress;
    uint8_t  bSpeed;
    uint8_t  bEp0MaxPks;
    uint8_t  DeviceIndex;
}HUB_DEVICE, *PHUB_DEVICE;

/* Root HUB Device Structure */
typedef struct _ROOT_HUB_DEVICE
{
    uint8_t  bStatus;
    uint8_t  bType;
    uint8_t  bAddress;
    uint8_t  bSpeed;
    uint8_t  bEp0MaxPks;
    uint8_t  DeviceIndex;
    uint8_t  bPortNum;
    HUB_DEVICE Device[ DEF_NEXT_HUB_PORT_NUM_MAX ];
} ROOT_HUB_DEVICE, *PROOT_HUB_DEVICE;

/* USB Host Control Structure */
typedef struct __HOST_CTL
{                                   
    uint8_t  InterfaceNum;
    uint8_t  ErrorCount;	
    
    struct interface
    {
        uint8_t  Type;
        uint16_t HidDescLen;
        uint8_t  HidReportID;
        uint8_t  Full_KB_Flag;

        uint8_t  InEndpNum;
        uint8_t  InEndpAddr[ 4 ];
        uint8_t  InEndpType[ 4 ];
        uint16_t InEndpSize[ 4 ];
        uint8_t  InEndpTog[ 4 ];
        uint8_t  InEndpInterval[ 4 ];
        /* 16 bit on purpose: the counter is incremented by the 1 ms TIM3 interrupt and polled
         * by USBH_MainDeal( ), which runs on a fixed 2 ms grid (vTaskDelay( 1 )). With 8 bits
         * the counter wraps at 255 - exactly the interval reported by many HUBs - so the poll
         * could sample only even values and never see the single 255 the comparison looks for,
         * and the HUB ports were never scanned. */
        uint16_t InEndpTimeCount[ 4 ];

        uint8_t  OutEndpNum;
        uint8_t  OutEndpAddr[ 4 ];
        uint8_t  OutEndpType[ 4 ];
        uint16_t OutEndpSize[ 4 ];
        uint8_t  OutEndpTog[ 4 ];

        uint8_t  IDFlag;
        uint8_t  ReportID;

        uint8_t  LED_Usage_Min;
        uint8_t  LED_Usage_Max;

        uint8_t  SetReport_Swi;
        uint8_t  SetReport_Value;
        uint8_t  SetReport_Flag;

    }Interface[ DEF_INTERFACE_NUM_MAX ];
} HOST_CTL, *PHOST_CTL;

/*******************************************************************************/
/* Struct Declaration */
extern struct   _ROOT_HUB_DEVICE RootHubDev[ ];
extern struct   __HOST_CTL HostCtl[ ];


#ifdef __cplusplus
}
#endif

#endif
