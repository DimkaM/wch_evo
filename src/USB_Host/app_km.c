/********************************** (C) COPYRIGHT  *******************************
 * File Name          : app_km.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2022/09/01
 * Description        : The USB host operates the keyboard and mouse.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/


/********************************************************************************/
/* Header File */
#include "usb_host_config.h"
#include "app_usb.h"                            /* AppUsb_RequestRestart( ) - HUB recovery */

/*******************************************************************************/
/* Variable Definition */
uint8_t  DevDesc_Buf[ 18 ];                                                     // Device Descriptor Buffer
uint8_t  Com_Buf[ DEF_COM_BUF_LEN ];                                            // General Buffer
struct   _ROOT_HUB_DEVICE RootHubDev[ DEF_TOTAL_ROOT_HUB ];
struct   __HOST_CTL HostCtl[ DEF_TOTAL_ROOT_HUB * DEF_ONE_USB_SUP_DEV_TOTAL ];
volatile uint32_t g_ms_ticks = 0;                                                // 1 ms time base for the application tasks (TIM3 update interrupt)
volatile uint8_t  g_usbRootReady = 0;                                            // USB readiness flags, see app_km.h
volatile uint8_t  g_usbHidKbReady = 0;


#if DEF_USBFS_PORT_EN
/* One-shot request to check every port of a HUB right after it has been enumerated: a device
 * plugged in before the enumeration may not generate a new port change event. */
static uint8_t USBH_HubScanAll[ DEF_TOTAL_ROOT_HUB ];
#endif

/* State of the HUB port change bit handling, see HUB_Port_ClearChanges( ):
 * USBH_HubPortRetry - number of consecutive enumeration retries of an un-enumerated device;
 * USBH_HubPortStuck - number of consecutive scans in which the change bits of a port could not
 *                     be cleared (diagnostic only);
 * USBH_HubPortEmpty - 1 when the port had no device during the previous scan (a connection change
 *                     reported for an already empty port means that the HUB did not detect the
 *                     device which has just been plugged in, see the port power cycle);
 * USBH_HubRecoverCount / USBH_HubRecoverTick - automatic stack restarts (see DEF_HUB_RECOVER_*). */
static uint8_t USBH_HubPortRetry[ DEF_TOTAL_ROOT_HUB ][ DEF_NEXT_HUB_PORT_NUM_MAX ];
static uint8_t USBH_HubPortStuck[ DEF_TOTAL_ROOT_HUB ][ DEF_NEXT_HUB_PORT_NUM_MAX ];
static uint8_t USBH_HubPortEmpty[ DEF_TOTAL_ROOT_HUB ][ DEF_NEXT_HUB_PORT_NUM_MAX ];
static uint8_t USBH_HubRecoverCount;                    /* consecutive automatic restarts */
static uint32_t USBH_HubRecoverTick;                    /* g_ms_ticks of the last restart */

/*******************************************************************************/
/* Interrupt Function Declaration */
void TIM3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/*********************************************************************
 * @fn      TIM3_Init
 *
 * @brief   Initialize timer3 for getting keyboard and mouse data.
 *
 * @param   arr - The specific period value.
 *          psc - The specifies prescaler value.
 *
 * @return  none
 */
void TIM3_Init( uint16_t arr, uint16_t psc )
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = { 0 };
    NVIC_InitTypeDef NVIC_InitStructure = { 0 };

    /* Enable timer3 clock */
    RCC_APB1PeriphClockCmd( RCC_APB1Periph_TIM3, ENABLE );

    /* Initialize timer3 */
    TIM_TimeBaseStructure.TIM_Period = arr;
    TIM_TimeBaseStructure.TIM_Prescaler = psc;
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit( TIM3, &TIM_TimeBaseStructure );

    /* Enable updating timer3 interrupt */
    TIM_ITConfig( TIM3, TIM_IT_Update, ENABLE );

    /* Configure timer3 interrupt */
    NVIC_InitStructure.NVIC_IRQChannel = TIM3_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init( &NVIC_InitStructure );

    /* Enable timer3 */
    TIM_Cmd( TIM3, ENABLE );

    /* Enable timer3 interrupt */
    NVIC_EnableIRQ( TIM3_IRQn );
}

/*********************************************************************
 * @fn      TIM3_IRQHandler
 *
 * @brief   This function handles TIM3 global interrupt request.
 *
 * @return  none
 */
void TIM3_IRQHandler( void )
{
    uint8_t usb_port;
    uint8_t hub_port;
    uint8_t index;
    uint8_t intf_num, in_num;

    if( TIM_GetITStatus( TIM3, TIM_IT_Update ) != RESET )
    {
        /* 1 ms time base for the application tasks (see src/app_tasks.c) */
        g_ms_ticks++;

        /* Clear interrupt flag */
        TIM_ClearITPendingBit( TIM3, TIM_IT_Update );

        /* USB HID Device Input Endpoint Timing */
        for( usb_port = 0; usb_port < DEF_TOTAL_ROOT_HUB; usb_port++ )
        {
            if( RootHubDev[ usb_port ].bStatus >= ROOT_DEV_SUCCESS )
            {
                index = RootHubDev[ usb_port ].DeviceIndex;
                if( RootHubDev[ usb_port ].bType == USB_DEV_CLASS_HID )
                {
                    for( intf_num = 0; intf_num < HostCtl[ index ].InterfaceNum; intf_num++ )
                    {
                        for( in_num = 0; in_num < HostCtl[ index ].Interface[ intf_num ].InEndpNum; in_num++ )
                        {
                            HostCtl[ index ].Interface[ intf_num ].InEndpTimeCount[ in_num ]++;
                        }
                    }
                }
                else if( RootHubDev[ usb_port ].bType == USB_DEV_CLASS_HUB )
                {
                    HostCtl[ index ].Interface[ 0 ].InEndpTimeCount[ 0 ]++;
                    for( hub_port = 0; hub_port < RootHubDev[ usb_port ].bPortNum; hub_port++ )
                    {
                        if( RootHubDev[ usb_port ].Device[ hub_port ].bStatus >= ROOT_DEV_SUCCESS )
                        {
                            index = RootHubDev[ usb_port ].Device[ hub_port ].DeviceIndex;

                            if( RootHubDev[ usb_port ].Device[ hub_port ].bType == USB_DEV_CLASS_HID )
                            {
                                for( intf_num = 0; intf_num < HostCtl[ index ].InterfaceNum; intf_num++ )
                                {
                                    for( in_num = 0; in_num < HostCtl[ index ].Interface[ intf_num ].InEndpNum; in_num++ )
                                    {
                                        HostCtl[ index ].Interface[ intf_num ].InEndpTimeCount[ in_num ]++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/*********************************************************************
 * @fn      USBH_CheckRootHubPortStatus
 *
 * @brief   Check status of USB port.
 *
 * @para    index: USB host port
 *
 * @return  The current status of the port.
 */
uint8_t USBH_CheckRootHubPortStatus( uint8_t usb_port )
{
    uint8_t s = ERR_USB_UNSUPPORT;
    
    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_CheckRootHubPortStatus( RootHubDev[ usb_port ].bStatus );
#endif            
    }
    
    return s;
}

/*********************************************************************
 * @fn      USBH_ResetRootHubPort
 *
 * @brief   Reset USB port.
 *
 * @para    index: USB host port
 *          mod: Reset host port operating mode.
 *               0 -> reset and wait end
 *               1 -> begin reset
 *               2 -> end reset
 *
 * @return  none
 */
void USBH_ResetRootHubPort( uint8_t usb_port, uint8_t mode )
{
    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        USBFSH_ResetRootHubPort( mode );
#endif
    }
}

/*********************************************************************
 * @fn      USBH_EnableRootHubPort
 *
 * @brief   Enable USB host port.
 *
 * @para    index: USB host port
 *
 * @return  none
 */
uint8_t USBH_EnableRootHubPort( uint8_t usb_port )
{
    uint8_t s = ERR_USB_UNSUPPORT;
    
    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_EnableRootHubPort( &RootHubDev[ usb_port ].bSpeed );
#endif            
    }
   
    return s;
}

/*********************************************************************
 * @fn      USBH_GetDeviceDescr
 *
 * @brief   Get the device descriptor of the USB device.
 *
 * @para    index: USB host port
 *
 * @return  none
 */
uint8_t USBH_GetDeviceDescr( uint8_t usb_port )
{
    uint8_t s = ERR_USB_UNSUPPORT;
    
    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_GetDeviceDescr( &RootHubDev[ usb_port ].bEp0MaxPks, DevDesc_Buf );
#endif            
    }
    
    return s;
}

/*********************************************************************
 * @fn      USBH_SetUsbAddress
 *
 * @brief   Set USB device address.
 *
 * @para    index: USB host port
 *
 * @return  none
 */
uint8_t USBH_SetUsbAddress( uint8_t usb_port )
{
    uint8_t s = ERR_USB_UNSUPPORT;
    
    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        RootHubDev[ usb_port ].bAddress = (uint8_t)( DEF_USBFS_PORT_INDEX + USB_DEVICE_ADDR );
        s = USBFSH_SetUsbAddress( RootHubDev[ usb_port ].bEp0MaxPks, RootHubDev[ usb_port ].bAddress );
#endif            
    }
    
    return s;
}

/*********************************************************************
 * @fn      USBH_GetConfigDescr
 *
 * @brief   Get the configuration descriptor of the USB device. 
 *
 * @para    index: USB host port
 *
 * @return  none
 */
uint8_t USBH_GetConfigDescr( uint8_t usb_port, uint16_t *pcfg_len )
{
    uint8_t s = ERR_USB_UNSUPPORT;

    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_GetConfigDescr( RootHubDev[ usb_port ].bEp0MaxPks, Com_Buf, DEF_COM_BUF_LEN, pcfg_len );
#endif            
    }
    
    return s;
}

/*********************************************************************
 * @fn      USBH_AnalyseType
 *
 * @brief   Simply analyze USB device type.
 *
* @para     pdev_buf: Device descriptor buffer
 *          pcfg_buf: Configuration descriptor buffer
 *          ptype: Device type.
 *
 * @return  none
 */
void USBH_AnalyseType( uint8_t *pdev_buf, uint8_t *pcfg_buf, uint8_t *ptype )
{
    uint8_t  dv_cls, if_cls;

    dv_cls = ( (PUSB_DEV_DESCR)pdev_buf )->bDeviceClass;
    if_cls = ( (PUSB_CFG_DESCR_LONG)pcfg_buf )->itf_descr.bInterfaceClass;
    if( ( dv_cls == USB_DEV_CLASS_STORAGE ) || ( if_cls == USB_DEV_CLASS_STORAGE ) )
    {
        *ptype = USB_DEV_CLASS_STORAGE;
    }
    else if( ( dv_cls == USB_DEV_CLASS_PRINTER ) || ( if_cls == USB_DEV_CLASS_PRINTER ) )
    {
        *ptype = USB_DEV_CLASS_PRINTER;
    }
    else if( ( dv_cls == USB_DEV_CLASS_HID ) || ( if_cls == USB_DEV_CLASS_HID ) )
    {
        *ptype = USB_DEV_CLASS_HID;
    }
    else if( ( dv_cls == USB_DEV_CLASS_HUB ) || ( if_cls == USB_DEV_CLASS_HUB ) )
    {
        *ptype = USB_DEV_CLASS_HUB;
    }
    else
    {
        *ptype = DEF_DEV_TYPE_UNKNOWN;
    }
}

/*********************************************************************
 * @fn      USBFSH_SetUsbConfig
 *
 * @brief   Set USB configuration.
 *
 * @para    index: USB host port
 *
 * @return  none
 */
uint8_t USBH_SetUsbConfig( uint8_t usb_port, uint8_t cfg_val )
{
    uint8_t s = ERR_USB_UNSUPPORT;
    
    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_SetUsbConfig( RootHubDev[ usb_port ].bEp0MaxPks, cfg_val );
#endif            
    }
    
    return s;
}

/*********************************************************************
 * @fn      USBH_GetStrDescr
 *
 * @brief   Get the string descriptor of the USB device.
 *
 * @para    index: USB host port
 *
 * @return  The result of getting the string descriptor.
 */
uint8_t USBH_GetStrDescr( uint8_t usb_port, uint8_t ep0_size, uint8_t str_num )
{
    uint8_t s = ERR_USB_UNSUPPORT;

    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_GetStrDescr( ep0_size, str_num, Com_Buf );
#endif
    }

    return s;
}

/*********************************************************************
 * @fn      USBH_GetHidData
 *
 * @brief
 *
 * @para    index - Corresponding host port.
 *
 * @return  none
 */
uint8_t USBH_GetHidData( uint8_t usb_port, uint8_t index, uint8_t intf_num, uint8_t endp_num, uint8_t *pbuf, uint16_t *plen )
{
    uint8_t s = ERR_USB_UNSUPPORT;

    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_GetEndpData( HostCtl[ index ].Interface[ intf_num ].InEndpAddr[ endp_num ],
                                &HostCtl[ index ].Interface[ intf_num ].InEndpTog[ endp_num ], pbuf, plen );
#endif
    }

    return s;
}

/*********************************************************************
 * @fn      USBH_SendHidData
 *
 * @brief   Send data to the USB device output endpoint.
 *
 * @para    index: USB host port
 *
 * @return  The result of sending data.
 */
uint8_t USBH_SendHidData( uint8_t usb_port, uint8_t index, uint8_t intf_num, uint8_t endp_num, uint8_t *pbuf, uint16_t len )
{
    uint8_t s = ERR_USB_UNSUPPORT;

    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_SendEndpData( HostCtl[ index ].Interface[ intf_num ].OutEndpAddr[ endp_num ],
                                 &HostCtl[ index ].Interface[ intf_num ].OutEndpTog[ endp_num ], pbuf, len );
#endif
    }

    return s;
}

/*********************************************************************
 * @fn      USBH_ClearEndpStall
 *
 * @brief
 *
 * @para    index - Corresponding host port.
 *
 * @return  none
 */
uint8_t USBH_ClearEndpStall( uint8_t usb_port, uint8_t endp_num )
{
    uint8_t s = ERR_USB_UNSUPPORT;

    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_ClearEndpStall( RootHubDev[ usb_port ].bEp0MaxPks, endp_num );
#endif
    }

    return s;
}

/*********************************************************************
 * @fn      USBH_EnumRootDevice
 *
 * @brief   Generally enumerate a device connected to host port.
 *
 * @para    index: USB host port
 *
 * @return  Enumeration result
 */
uint8_t USBH_EnumRootDevice( uint8_t usb_port )
{
    uint8_t  s;
    uint8_t  enum_cnt;
    uint8_t  cfg_val;
    uint16_t i;
    uint16_t len;

    DUG_PRINTF( "Enum:\r\n" );

    enum_cnt = 0;
ENUM_START:
    /* Delay and wait for the device to stabilize */
    Delay_Ms( 100 );
    enum_cnt++;
    Delay_Ms( 8 << enum_cnt );

    /* Reset the USB device and wait for the USB device to reconnect */
    USBH_ResetRootHubPort( usb_port, 0 );
    for( i = 0, s = 0; i < DEF_RE_ATTACH_TIMEOUT; i++ )
    {
        if( USBH_EnableRootHubPort( usb_port ) == ERR_SUCCESS )
        {
            i = 0;
            s++;
            if( s > 6 )
            {
                break;
            }
        }
        Delay_Ms( 1 );
    }
    if( i )
    {
        /* Determine whether the maximum number of retries has been reached, and retry if not reached */
        if( enum_cnt <= 5 )
        {
            goto ENUM_START;
        }
        return ERR_USB_DISCON;
    }

    /* Get USB device device descriptor */
    DUG_PRINTF("Get DevDesc: ");
    s = USBH_GetDeviceDescr( usb_port );
    if( s == ERR_SUCCESS )
    {
        /* Print USB device device descriptor */
#if DEF_DEBUG_PRINTF
        for( i = 0; i < 18; i++ )
        {
            DUG_PRINTF( "%02x ", DevDesc_Buf[ i ] );
        }
        DUG_PRINTF("\r\n"); 
#endif
    }
    else
    {
        /* Determine whether the maximum number of retries has been reached, and retry if not reached */
        DUG_PRINTF( "Err(%02x)\r\n", s );
        if( enum_cnt <= 5 )
        {
            goto ENUM_START;
        }
        return DEF_DEV_DESCR_GETFAIL;
    }

    /* Set the USB device address */
    DUG_PRINTF("Set DevAddr: ");
    s = USBH_SetUsbAddress( usb_port );
    if( s == ERR_SUCCESS )
    {
        DUG_PRINTF( "OK\r\n" );    
    }
    else
    {
        /* Determine whether the maximum number of retries has been reached, and retry if not reached */
        DUG_PRINTF( "Err(%02x)\r\n", s );
        if( enum_cnt <= 5 )
        {
            goto ENUM_START;
        }
        return DEF_DEV_ADDR_SETFAIL;
    }
    Delay_Ms( 5 );

    /* Get the USB device configuration descriptor */
    DUG_PRINTF("Get CfgDesc: ");
    s = USBH_GetConfigDescr( usb_port, &len );
    if( s == ERR_SUCCESS )
    {
        cfg_val = ( (PUSB_CFG_DESCR)Com_Buf )->bConfigurationValue;
        
        /* Print USB device configuration descriptor  */
#if DEF_DEBUG_PRINTF
        for( i = 0; i < len; i++ )
        {
            DUG_PRINTF( "%02x ", Com_Buf[ i ] );
        }
        DUG_PRINTF("\r\n");
#endif

        /* Simply analyze USB device type  */
        USBH_AnalyseType( DevDesc_Buf, Com_Buf, &RootHubDev[ usb_port ].bType );
        DUG_PRINTF( "DevType: %02x\r\n", RootHubDev[ usb_port ].bType );

    }
    else
    {
        /* Determine whether the maximum number of retries has been reached, and retry if not reached */
        DUG_PRINTF( "Err(%02x)\r\n", s );
        if( enum_cnt <= 5 )
        {
            goto ENUM_START;
        }
        return DEF_CFG_DESCR_GETFAIL;
    }

    /* Set USB device configuration value */
    DUG_PRINTF("Set Cfg: ");
    s = USBH_SetUsbConfig( usb_port, cfg_val );
    if( s == ERR_SUCCESS )
    {
        DUG_PRINTF( "OK\r\n" );
    }
    else
    {
        /* Determine whether the maximum number of retries has been reached, and retry if not reached */
        DUG_PRINTF( "Err(%02x)\r\n", s );
        if( enum_cnt <= 5 )
        {
            goto ENUM_START;
        }
        return ERR_USB_UNSUPPORT;
    }

#if DEF_DEBUG_PRINTF
    DUG_PRINTF( "Root Dev Speed:%x\r\n", RootHubDev[ usb_port ].bSpeed );
#endif

    return ERR_SUCCESS;
}

/*********************************************************************
 * @fn      KM_AnalyzeConfigDesc
 *
 * @brief   Analyze keyboard and mouse configuration descriptor.
 *
 * @para    index: USB host port
 *
 * @return  The result of the analysis.
 */
uint8_t KM_AnalyzeConfigDesc( uint8_t usb_port, uint8_t index )
{
    uint8_t  s = 0;
    uint16_t i;
    uint8_t  num, innum, outnum;

    num = 0;
    for( i = 0; i < ( Com_Buf[ 2 ] + ( (uint16_t)Com_Buf[ 3 ] << 8 ) ); )
    {
        if( Com_Buf[ i + 1 ] == DEF_DECR_CONFIG )
        {
            /* Save the number of interface of the USB device, only up to 4 */
            if( ( (PUSB_CFG_DESCR)( &Com_Buf[ i ] ) )->bNumInterfaces > DEF_INTERFACE_NUM_MAX )
            {
                HostCtl[ index ].InterfaceNum = DEF_INTERFACE_NUM_MAX;
            }
            else
            {
                HostCtl[ index ].InterfaceNum = ( (PUSB_CFG_DESCR)( &Com_Buf[ i ] ) )->bNumInterfaces;
            }
            i += Com_Buf[ i ];
        }
        else if( Com_Buf[ i + 1 ] == DEF_DECR_INTERFACE )
        {
            if( num == DEF_INTERFACE_NUM_MAX )
            {
                return s;
            }
            if( ( (PUSB_ITF_DESCR)( &Com_Buf[ i ] ) )->bInterfaceClass == 0x03 )
            {
                /* HID devices (such as USB keyboard and mouse) */
                if( ( (PUSB_ITF_DESCR)( &Com_Buf[ i ] ) )->bInterfaceSubClass <= 0x01 &&
                    ( (PUSB_ITF_DESCR)( &Com_Buf[ i ] ) )->bInterfaceProtocol <= 2 )
                {
                    if( ( (PUSB_ITF_DESCR)( &Com_Buf[ i ] ) )->bInterfaceProtocol == 0x01 ) // Keyboard
                    {
                        HostCtl[ index ].Interface[ num ].Type = DEC_KEY;
                        HID_SetIdle( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, num, 0, 0 );
                    }
                    else if( ( (PUSB_ITF_DESCR)( &Com_Buf[ i ] ) )->bInterfaceProtocol == 0x02 ) // Mouse
                    {
                        HostCtl[ index ].Interface[ num ].Type = DEC_MOUSE;
                        HID_SetIdle( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, num, 0, 0 );
                    }
                    s = ERR_SUCCESS;
                    i += Com_Buf[ i ];
                    innum = 0;
                    outnum = 0;
                    while( 1 )
                    {
                        if( ( Com_Buf[ i + 1 ] == DEF_DECR_INTERFACE ) || ( i >= Com_Buf[ 2 ] ) )
                        {
                            break;
                        }
                        else
                        {
                            /* Analyze each endpoint of the current interface */
                            if( Com_Buf[ i + 1 ] == DEF_DECR_ENDPOINT )
                            {
                                /* Save endpoint related information (endpoint address, attribute, max packet size, polling interval) */
                                if( ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->bEndpointAddress & 0x80 )
                                {
                                    /* IN */
                                    HostCtl[ index ].Interface[ num ].InEndpAddr[ innum ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->bEndpointAddress & 0x0F;
                                    HostCtl[ index ].Interface[ num ].InEndpType[ innum ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->bmAttributes;
                                    HostCtl[ index ].Interface[ num ].InEndpSize[ innum ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->wMaxPacketSizeL +
                                                                              (uint16_t)( ( ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->wMaxPacketSizeH ) << 8 );
                                    HostCtl[ index ].Interface[ num ].InEndpInterval[ innum ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->bInterval;
                                    HostCtl[ index ].Interface[ num ].InEndpNum++;
                                    
                                    innum++;
                                }
                                else
                                {
                                    /* OUT */
                                    HostCtl[ index ].Interface[ num ].OutEndpAddr[ outnum ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->bEndpointAddress & 0x0f;
                                    HostCtl[ index ].Interface[ num ].OutEndpType[ outnum ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->bmAttributes;
                                    HostCtl[ index ].Interface[ num ].OutEndpSize[ outnum ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->wMaxPacketSizeL +
                                                                                (uint16_t)( ( ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->wMaxPacketSizeH ) << 8 );
                                    HostCtl[ index ].Interface[ num ].OutEndpNum++;

                                    outnum++;
                                }

                                i += Com_Buf[ i ];
                            }
                            else if( Com_Buf[ i + 1 ] == DEF_DECR_HID )
                            {
                                /* Save the current interface HID report descriptor length */
                                HostCtl[ index ].Interface[ num ].HidDescLen = ( (PUSB_HID_DESCR)( &Com_Buf[ i ] ) )->wDescriptorLengthL | \
                                                                               ( (uint16_t)( ( (PUSB_HID_DESCR)( &Com_Buf[ i ] ) )->wDescriptorLengthH ) << 8 );
                                i += Com_Buf[ i ];
                            }
                            else
                            {
                                i += Com_Buf[ i ];
                            }
                        }
                    }

                    if( ( outnum == 1 ) && ( HostCtl[ index ].Interface[ num ].Type == DEC_KEY ) )
                    {
                        HostCtl[ index ].Interface[ num ].SetReport_Swi = 0xFF;
                    }
                }
                else
                {
                    HostCtl[ index ].Interface[ num ].Type = DEC_UNKNOW;
                    i += Com_Buf[ i ];
                }
            }
            else
            {
                /* USB device type unknown */
                HostCtl[ index ].Interface[ num ].Type = DEC_UNKNOW;
                i += Com_Buf[ i ];

                break;
            }
                  
            num++;
        }
        else
        {
            i += Com_Buf[ i ];
        }
    }
    
    return s;
}

/*********************************************************************
 * @fn      KM_AnalyzeHidReportDesc
 *
 * @brief   Analyze keyboard and mouse report descriptor.
 *
 * @para    index: USB host port
 *
 * @return  The result of the analysis.
 */
void KM_AnalyzeHidReportDesc( uint8_t index, uint8_t intf_num )
{
    uint8_t  id = 0x00;
    uint8_t  led = 0x00;
    uint8_t  size, type, tag;
    uint8_t  report_size;
    uint8_t  report_cnt;
    uint16_t report_bits;

    uint16_t i = 0;

    /* Usage Page(Generic Desktop), Usage(Kyeboard) */
    if( ( Com_Buf[ i + 0 ] == 0x05 ) && ( Com_Buf[ i + 1 ] == 0x01 ) &&
        ( Com_Buf[ i + 2 ] == 0x09 ) && ( Com_Buf[ i + 3 ] == 0x06 ) )
    {
        i += 4;
        report_size = 0;
        report_cnt = 0;
        report_bits = 0;

        while( i < HostCtl[ index ].Interface[ intf_num ].HidDescLen )
        {
            /* Item Size, Item Type, Item Tag */
            size = Com_Buf[ i ] & 0x03;
            type = Com_Buf[ i ] & 0x0C;
            tag = Com_Buf[ i ] & 0xF0;

            switch( type )
            {
                /* MAIN */
                case 0x00:
                    switch( tag )
                    {
                        /* Output */
                        case 0x90:
                            if( led )
                            {
                                report_bits += report_cnt * report_size;

                                /* Save report ID for output */
                                if( ( id != 0 ) && ( HostCtl[ index ].Interface[ intf_num ].IDFlag == 0 ) )
                                {
                                    HostCtl[ index ].Interface[ intf_num ].IDFlag = 1;
                                    HostCtl[ index ].Interface[ intf_num ].ReportID = id;
                                }
                            }
                            i++;
                            break;

                        default:
                            i++;
                            break;
                    }
                    break;

                /* Global */
                case 0x04:
                    switch( tag )
                    {
                        /* Report ID */
                        case 0x80:
                            i++;
                            id = Com_Buf[ i ];
                            break;

                        /* Report Count */
                        case 0x90:
                            i++;
                            report_cnt = Com_Buf[ i ];
                            break;

                        /* Report Size */
                        case 0x70:
                            i++;
                            report_size = Com_Buf[ i ];
                            break;

                        /* Usage Page */
                        case 0x00:
                            i++;
                            if( Com_Buf[ i ] == 0x08 )      // LED
                            {
                                led = 1;
                            }
                            else
                            {
                                led = 0;
                            }
                            break;

                        default:
                            i++;
                            break;
                    }
                    break;

                /* Local */
                case 0x08:
                    switch( tag )
                    {
                        /* Usage Minimum */
                        case 0x10:
                            i++;
                            if( led )
                            {
                                HostCtl[ index ].Interface[ intf_num ].LED_Usage_Min = Com_Buf[ i ];
                            }
                            break;

                        /* Usage Maximum */
                        case 0x20:
                            i++;
                            if( led )
                            {
                                HostCtl[ index ].Interface[ intf_num ].LED_Usage_Max = Com_Buf[ i ];
                            }
                            break;

                        default:
                            i++;
                            break;
                    }
                    break;

                default:
                    i++;
                    break;
            }
            i += size;
        }

        if( report_bits == 8 )
        {
            if( HostCtl[ index ].Interface[ intf_num ].SetReport_Swi == 0 )
            {
                HostCtl[ index ].Interface[ intf_num ].SetReport_Swi = 1;
            }
        }
        else
        {
            HostCtl[ index ].Interface[ intf_num ].SetReport_Swi = 0;
        }
    }
}

/*********************************************************************
 * @fn      KM_DealHidReportDesc
 *
 * @brief   Get and analyze keyboard and mouse report descriptor.
 *
 * @para    index: USB host port
 *
 * @return  The result of the acquisition and analysis.
 */
uint8_t KM_DealHidReportDesc( uint8_t usb_port, uint8_t index, uint8_t ep0_size )
{
    uint8_t  s;
    uint8_t  num, num_tmp;
    uint8_t  getrep_cnt;
#if DEF_DEBUG_PRINTF
    uint16_t i;
#endif

    getrep_cnt = 0;
    num_tmp = HostCtl[ index ].InterfaceNum;
    while( num_tmp )
    {
        num = HostCtl[ index ].InterfaceNum - num_tmp;
        if( HostCtl[ index ].Interface[ num ].HidDescLen )
        {
GETREP_START:
            getrep_cnt++;
            
            /* Get HID report descriptor */
            DUG_PRINTF("Get Interface%x RepDesc: ", num );
            s = HID_GetHidDesr( usb_port, ep0_size, num, Com_Buf, &HostCtl[ index ].Interface[ num ].HidDescLen );
            if( s == ERR_SUCCESS )
            {
                /* Print HID report descriptor */
#if DEF_DEBUG_PRINTF
                for( i = 0; i < HostCtl[ index ].Interface[ num ].HidDescLen; i++ )
                {
                    DUG_PRINTF( "%02x " , Com_Buf[ i ]);
                }
                DUG_PRINTF("\r\n");
#endif

                /* Analyze Report Descriptor */
                KM_AnalyzeHidReportDesc( index, num );

                num_tmp--;
            }
            else
            {
                DUG_PRINTF( "Err(%02x)\r\n", s );
                if( getrep_cnt <= 5 )
                {
                    goto GETREP_START;
                }

                return DEF_REP_DESCR_GETFAIL;
            }
        }
        else
        {
            num_tmp--;
        }
    }

    return s;
}

/*********************************************************************
 * @fn      USBH_EnumHidDevice
 *
 * @brief   Enumerate HID device.
 *
 * @para    index: USB host port
 *
 * @return  The result of the enumeration.
 */
uint8_t USBH_EnumHidDevice( uint8_t usb_port, uint8_t index, uint8_t ep0_size )
{
    uint8_t  s;
    uint8_t  intf_num;
#if DEF_DEBUG_PRINTF
    uint8_t  i;
#endif

    DUG_PRINTF( "Enum Hid:\r\n" );
    
    /* Analyze HID class device configuration descriptor and save relevant parameters */
    DUG_PRINTF("Analyze CfgDesc: ");
    s = KM_AnalyzeConfigDesc( usb_port, index );
    if( s == ERR_SUCCESS )
    {
        DUG_PRINTF( "OK\r\n" );
    }
    else
    {
        DUG_PRINTF( "Err(%02x)\r\n", s );
        return s;
    }

    /* Get the string descriptor contained in the configuration descriptor if it exists */
    if( Com_Buf[ 6 ] )
    {
        DUG_PRINTF("Get StringDesc4: ");
        s = USBH_GetStrDescr( usb_port, ep0_size, Com_Buf[ 6 ] );
        if( s == ERR_SUCCESS )
        {
            /* Print the string descriptor contained in the configuration descriptor */
#if DEF_DEBUG_PRINTF
            for( i = 0; i < Com_Buf[ 0 ]; i++ )
            {
                DUG_PRINTF( "%02x ", Com_Buf[ i ] );
            }
            DUG_PRINTF("\r\n");
#endif
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
        }
    }

    /* Get HID report descriptor */
    s = KM_DealHidReportDesc( usb_port, index, ep0_size );
    
    /* Get USB vendor string descriptor  */
    if( DevDesc_Buf[ 14 ] )
    {
        DUG_PRINTF("Get StringDesc1: ");
        s = USBH_GetStrDescr( usb_port, ep0_size, DevDesc_Buf[ 14 ] );
        if( s == ERR_SUCCESS )
        {
            /* Print USB vendor string descriptor */
#if DEF_DEBUG_PRINTF
            for( i = 0; i < Com_Buf[ 0 ]; i++ )
            {
                DUG_PRINTF( "%02x ", Com_Buf[ i ]);
            }
            DUG_PRINTF("\r\n");
#endif
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
        }
    }

    /* Get USB product string descriptor */
    if( DevDesc_Buf[ 15 ] )
    {
        DUG_PRINTF("Get StringDesc2: ");
        s = USBH_GetStrDescr( usb_port, ep0_size, DevDesc_Buf[ 15 ] );
        if( s == ERR_SUCCESS )
        {
            /* Print USB product string descriptor */
#if DEF_DEBUG_PRINTF
            for( i = 0; i < Com_Buf[ 0 ]; i++ )
            {
                DUG_PRINTF( "%02x ", Com_Buf[ i ] );
            }
            DUG_PRINTF("\r\n");
#endif
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
        }
    }

    /* Get USB serial number string descriptor */
    if( DevDesc_Buf[ 16 ] )
    {
        DUG_PRINTF("Get StringDesc3: ");
        s = USBH_GetStrDescr( usb_port, ep0_size, DevDesc_Buf[ 16 ] );
        if( s == ERR_SUCCESS )
        {
            /* Print USB serial number string descriptor */
#if DEF_DEBUG_PRINTF
            for( i = 0; i < Com_Buf[ 0 ]; i++ )
            {
                DUG_PRINTF( "%02x ", Com_Buf[ i ] );
            }
            DUG_PRINTF("\r\n");
#endif
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
        }
    }

    /* Get USB serial number string descriptor */
    for( intf_num = 0; intf_num < HostCtl[ index ].InterfaceNum; intf_num++ )
    {
        if( HostCtl[ index ].Interface[ intf_num ].Type == DEC_KEY )
        {
            HostCtl[ index ].Interface[ intf_num ].SetReport_Value = 0x00;
            KB_SetReport( usb_port, index, ep0_size, intf_num );
        }
    }

    return ERR_SUCCESS;
}

/*********************************************************************
 * @fn      HUB_Analyse_ConfigDesc
 *
 * @brief   Analyze HUB configuration descriptor.
 *
 * @para
 *
 * @return  none
 */
uint8_t HUB_AnalyzeConfigDesc( uint8_t index )
{
    uint8_t  s = ERR_SUCCESS;
    uint16_t i;

    for( i = 0; i < ( Com_Buf[ 2 ] + ( (uint16_t)Com_Buf[ 3 ] << 8 ) ); )
    {
        if( Com_Buf[ i + 1 ] == DEF_DECR_CONFIG )
        {
            /* Save the number of interface of the USB device, only up to 4 */
            if( ( (PUSB_CFG_DESCR)( &Com_Buf[ i ] ) )->bNumInterfaces > 1 )
            {
                HostCtl[ index ].InterfaceNum = 1;
            }
            else
            {
                HostCtl[ index ].InterfaceNum = ( (PUSB_CFG_DESCR)( &Com_Buf[ i ] ) )->bNumInterfaces;
            }
            i += Com_Buf[ i ];
        }
        else if( Com_Buf[ i + 1 ] == DEF_DECR_INTERFACE )
        {
            if( ( (PUSB_ITF_DESCR)( &Com_Buf[ i ] ) )->bInterfaceClass == 0x09 )
            {
                i += Com_Buf[ i ];
                while( 1 )
                {
                    if( ( Com_Buf[ i + 1 ] == DEF_DECR_INTERFACE ) || ( i >= ( Com_Buf[ 2 ] + ( (uint16_t)Com_Buf[ 3 ] << 8 ) ) ) )
                    {
                        break;
                    }
                    else
                    {
                        /* Analyze each endpoint of the current interface */
                        if( Com_Buf[ i + 1 ] == DEF_DECR_ENDPOINT )
                        {
                            /* Save endpoint related information (endpoint address, attribute, max packet size, polling interval) */
                            if( ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->bEndpointAddress & 0x80 )
                            {
                                /* IN */
                                HostCtl[ index ].Interface[ 0 ].InEndpAddr[ 0 ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->bEndpointAddress & 0x0F;
                                HostCtl[ index ].Interface[ 0 ].InEndpType[ 0 ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->bmAttributes;
                                HostCtl[ index ].Interface[ 0 ].InEndpSize[ 0 ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->wMaxPacketSizeL + \
                                                                              (uint16_t)( ( ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->wMaxPacketSizeH ) << 8 );
                                HostCtl[ index ].Interface[ 0 ].InEndpInterval[ 0 ] = ( (PUSB_ENDP_DESCR)( &Com_Buf[ i ] ) )->bInterval;
                                HostCtl[ index ].Interface[ 0 ].InEndpNum++;
                            }

                            i += Com_Buf[ i ];
                        }
                        else
                        {
                            i += Com_Buf[ i ];
                        }
                    }
                }
            }
            else
            {
                /* USB device type unknown */
                i += Com_Buf[ i ];
            }
        }
        else
        {
            i += Com_Buf[ i ];
        }
    }
    return s;
}

/*********************************************************************
 * @fn      USBH_EnumHubDevice
 *
 * @brief   Enumerate HUB device.
 *
 * @para    index: USB host port
 *
 * @return  The result of the enumeration.
 */
uint8_t USBH_EnumHubDevice( uint8_t usb_port, uint8_t ep0_size )
{
    uint8_t  s, retry;
    uint16_t len;
    uint16_t  i;

    DUG_PRINTF( "Enum Hub:\r\n" );

    /* Analyze HID class device configuration descriptor and save relevant parameters */
    DUG_PRINTF("Analyze CfgDesc: ");
    s = HUB_AnalyzeConfigDesc( RootHubDev[ usb_port ].DeviceIndex );
    if( s == ERR_SUCCESS )
    {
        DUG_PRINTF( "OK\r\n" );
    }
    else
    {
        DUG_PRINTF( "Err(%02x)\r\n", s );
        return s;
    }

    /* Get the string descriptor contained in the configuration descriptor if it exists */
    if( Com_Buf[ 6 ] )
    {
        DUG_PRINTF("Get StringDesc4: ");
        s = USBH_GetStrDescr( usb_port, ep0_size, Com_Buf[ 6 ] );
        if( s == ERR_SUCCESS )
        {
            /* Print the string descriptor contained in the configuration descriptor */
#if DEF_DEBUG_PRINTF
            for( i = 0; i < Com_Buf[ 0 ]; i++ )
            {
                DUG_PRINTF( "%02x ", Com_Buf[ i ] );
            }
            DUG_PRINTF("\r\n");
#endif
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
        }
    }

    /* Get USB vendor string descriptor  */
    if( DevDesc_Buf[ 14 ] )
    {
        DUG_PRINTF("Get StringDesc1: ");
        s = USBH_GetStrDescr( usb_port, ep0_size, DevDesc_Buf[ 14 ] );
        if( s == ERR_SUCCESS )
        {
            /* Print USB vendor string descriptor */
#if DEF_DEBUG_PRINTF
            for( i = 0; i < Com_Buf[ 0 ]; i++ )
            {
                DUG_PRINTF( "%02x ", Com_Buf[ i ]);
            }
            DUG_PRINTF("\r\n");
#endif
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
        }
    }

    /* Get USB product string descriptor */
    if( DevDesc_Buf[ 15 ] )
    {
        DUG_PRINTF("Get StringDesc2: ");
        s = USBH_GetStrDescr( usb_port, ep0_size, DevDesc_Buf[ 15 ] );
        if( s == ERR_SUCCESS )
        {
            /* Print USB product string descriptor */
#if DEF_DEBUG_PRINTF
            for( i = 0; i < Com_Buf[ 0 ]; i++ )
            {
                DUG_PRINTF( "%02x ", Com_Buf[ i ] );
            }
            DUG_PRINTF("\r\n");
#endif
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
        }
    }

    /* Get USB serial number string descriptor */
    if( DevDesc_Buf[ 16 ] )
    {
        DUG_PRINTF("Get StringDesc3: ");
        s = USBH_GetStrDescr( usb_port, ep0_size, DevDesc_Buf[ 16 ] );
        if( s == ERR_SUCCESS )
        {
            /* Print USB serial number string descriptor */
#if DEF_DEBUG_PRINTF
            for( i = 0; i < Com_Buf[ 0 ]; i++ )
            {
                DUG_PRINTF( "%02x ", Com_Buf[ i ] );
            }
            DUG_PRINTF("\r\n");
#endif
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
        }
    }

    /* Get hub descriptor */
    DUG_PRINTF("Get Hub Desc: ");
    for( retry = 0; retry < 5; retry++ )
    {
        s = HUB_GetClassDevDescr( usb_port, ep0_size, Com_Buf, &len );
        if( s == ERR_SUCCESS )
        {
            /* Print USB device device descriptor */
#if DEF_DEBUG_PRINTF
            for( i = 0; i < len; i++ )
            {
                DUG_PRINTF( "%02x ", Com_Buf[ i ] );
            }
            DUG_PRINTF("\r\n");
#endif

            RootHubDev[ usb_port ].bPortNum = ( (PUSB_HUB_DESCR)Com_Buf)->bNbrPorts;
            if( RootHubDev[ usb_port ].bPortNum > DEF_NEXT_HUB_PORT_NUM_MAX )
            {
                RootHubDev[ usb_port ].bPortNum = DEF_NEXT_HUB_PORT_NUM_MAX;
            }
            DUG_PRINTF( "RootHubDev[ %02x ].bPortNum: %02x\r\n", usb_port, RootHubDev[ usb_port ].bPortNum );
            break;
        }
        else
        {
            /* Determine whether the maximum number of retries has been reached, and retry if not reached */
            DUG_PRINTF( "Err(%02x)\r\n", s );

            if( retry == 4 )
            {
                return ERR_USB_UNKNOWN;
            }
        }
    }

    /* Set the HUB port to power on */
    for( retry = 0, i = 1; i <= RootHubDev[ usb_port ].bPortNum; i++ )
    {
        s = HUB_SetPortFeature( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, i, HUB_PORT_POWER );
        if( s == ERR_SUCCESS )
        {
            continue;
        }
        else
        {
            Delay_Ms( 5 );

            i--;
            retry++;
            if( retry >= 5 )
            {
                return ERR_USB_UNKNOWN;
            }
        }
    }

    /* Wait until the power of the HUB ports is stable: bPwrOn2PwrGood is expressed in 2ms
     * units (the CH334 reports 48, i.e. 96ms), a device plugged into a HUB port is not
     * accessible before that time. */
    if( ( (PUSB_HUB_DESCR)Com_Buf )->bDescriptorType == USB_DESCR_TYP_HUB )
    {
        i = (uint16_t)( ( (PUSB_HUB_DESCR)Com_Buf )->bPwrOn2PwrGood ) * 2;
    }
    else
    {
        i = 0;
    }
    Delay_Ms( ( i > 100 )? i : 100 );

    return ERR_SUCCESS;
}

/*********************************************************************
 * @fn      HUB_Port_PreEnum1
 *
 * @brief
 *
 * @para
 *
 * @return  none
 */
uint8_t HUB_Port_PreEnum1( uint8_t usb_port, uint8_t hub_port, uint8_t *pbuf )
{
    uint8_t  s;
    uint8_t  buf[ 4 ];
    uint8_t  retry;

    /* *pbuf holds the pending wPortChange bits of this port, read by the caller. The HUB interrupt
     * bitmap is not used here any more: the HUB reports a change in it only while the change is
     * new, so a port can stay pending in its port status while the bitmap never reports it again
     * (see section 6.2 of RESEARCH_CONCLUSION.md). Only C_PORT_CONNECTION (bit 0) is handled
     * here. */
    if( ( *pbuf ) & 0x01 )
    {
        s = HUB_GetPortStatus( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, hub_port, &buf[ 0 ] );
        if( s != ERR_SUCCESS )
        {
            DUG_PRINTF( "HUB_PE1_ERR1:%x\r\n", s );
            return s;
        }
        else
        {
            if( buf[ 2 ] & 0x01 )
            {
                s = HUB_ClearPortFeature( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, hub_port, HUB_C_PORT_CONNECTION );
                if( s != ERR_SUCCESS )
                {
                    DUG_PRINTF( "HUB_PE1_ERR2:%x\r\n", s );
                    return s;
                }

                retry = 0;
                do
                {
                    s = HUB_GetPortStatus( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, hub_port, &buf[ 0 ] );
                    if( s != ERR_SUCCESS )
                    {
                        DUG_PRINTF( "HUB_PE1_ERR3:%x\r\n", s );
                        return s;
                    }
                    retry++;
                }while( ( buf[ 2 ] & 0x01 ) && ( retry < 10 ) );

                if( retry != 10 )
                {
                    if( !( buf[ 0 ] & 0x01 ) )
                    {
                        DUG_PRINTF( "Hub Port%x Out\r\n", hub_port );
                        return ERR_USB_DISCON;
                    }
                }
            }
        }
    }

    return ERR_USB_UNKNOWN;
}

/*********************************************************************
 * @fn      HUB_Port_ClearChanges
 *
 * @brief   Clear all pending port change bits of the specified HUB port.
 *
 * @para    usb_port - the index of the USB port the HUB is connected to
 *          hub_port - the HUB port number (1-based, as used by the HUB requests)
 *
 * @return  none
 *
 * Note: a HUB reports a port in its interrupt endpoint bitmap as long as any of the wPortChange
 *       bits of that port is set, and every bit has to be dropped with its own ClearPortFeature
 *       request. Before, only C_PORT_CONNECTION was cleared (by HUB_Port_PreEnum1( )), so e.g.
 *       C_PORT_ENABLE - which the HUB sets when the port is enabled/disabled, i.e. on every
 *       unplug/plug - kept the port in the bitmap forever: the port was re-scanned (and reset)
 *       on every poll after a device had been unplugged and plugged back in.
 */
static void HUB_Port_ClearChanges( uint8_t usb_port, uint8_t hub_port )
{
    uint8_t buf[ 4 ];
    uint8_t ep0_size = RootHubDev[ usb_port ].bEp0MaxPks;

    if( HUB_GetPortStatus( usb_port, ep0_size, hub_port, &buf[ 0 ] ) != ERR_SUCCESS )
    {
        return;
    }

    if( buf[ 2 ] == 0 )
    {
        /* Nothing is pending, no HUB request is needed. */
        return;
    }

    /* Every pending change bit needs its own ClearPortFeature request. */
    if( buf[ 2 ] & 0x01 ) HUB_ClearPortFeature( usb_port, ep0_size, hub_port, HUB_C_PORT_CONNECTION );
    if( buf[ 2 ] & 0x02 ) HUB_ClearPortFeature( usb_port, ep0_size, hub_port, HUB_C_PORT_ENABLE );
    if( buf[ 2 ] & 0x04 ) HUB_ClearPortFeature( usb_port, ep0_size, hub_port, HUB_C_PORT_SUSPEND );
    if( buf[ 2 ] & 0x08 ) HUB_ClearPortFeature( usb_port, ep0_size, hub_port, HUB_C_PORT_OVER_CURRENT );
    if( buf[ 2 ] & 0x10 ) HUB_ClearPortFeature( usb_port, ep0_size, hub_port, HUB_C_PORT_RESET );

    if( HUB_GetPortStatus( usb_port, ep0_size, hub_port, &buf[ 0 ] ) != ERR_SUCCESS )
    {
        return;
    }

    if( buf[ 2 ] != 0 )
    {
        /* The HUB did not accept the clear requests. Report it once instead of flooding the log
         * on every poll (the port keeps being reported by the HUB in this case). */
        if( ++USBH_HubPortStuck[ usb_port ][ hub_port - 1 ] >= 20 )
        {
            USBH_HubPortStuck[ usb_port ][ hub_port - 1 ] = 0;
            DUG_PRINTF( "HUB port%x change bits stuck:%02x\r\n", hub_port, buf[ 2 ] );
        }
    }
    else
    {
        USBH_HubPortStuck[ usb_port ][ hub_port - 1 ] = 0;
#if DEF_DEBUG_HUB_SCAN
        DUG_PRINTF( "HubP%x change bits cleared\r\n", hub_port );
#endif
    }
}

/*********************************************************************
 * @fn      HUB_Port_PreEnum2
 *
 * @brief
 *
 * @para
 *
 * @return  none
 */
uint8_t HUB_Port_PreEnum2( uint8_t usb_port, uint8_t hub_port, uint8_t *pbuf )
{
    uint8_t  s;
    uint8_t  buf[ 4 ];
    uint8_t  retry = 0;

    /* *pbuf holds the pending wPortChange bits of this port (see HUB_Port_PreEnum1( )). The port is
     * reset when it reported C_PORT_CONNECTION / C_PORT_ENABLE / C_PORT_RESET, or when the caller
     * forces a retry (bit 0 set) for a device which is present but not enumerated. */
    if( ( *pbuf ) & 0x13 )
    {
        /* A port change bit does not mean "device present": a disconnect (or a stale change bit)
         * is reported the same way. Resetting an empty port only wastes ~100 ms (the HUB never
         * reports C_PORT_RESET for it) and leaves the port change bits pending. */
        s = HUB_GetPortStatus( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, hub_port, &buf[ 0 ] );
        if( s != ERR_SUCCESS )
        {
            DUG_PRINTF( "HUB_PE2_ERR0:%x\r\n", s );
            return s;
        }

        if( !( buf[ 0 ] & 0x01 ) )
        {
            /* No device on the port - nothing to reset. */
            return ERR_USB_UNKNOWN;
        }

        s = HUB_SetPortFeature( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, hub_port, HUB_PORT_RESET );
        if( s != ERR_SUCCESS )
        {
            DUG_PRINTF( "HUB_PE2_ERR1:%x\r\n", s );
            return s;
        }
        do
        {
            s = HUB_GetPortStatus( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, hub_port, &buf[ 0 ] );
            if( s != ERR_SUCCESS )
            {
                DUG_PRINTF( "HUB_PE2_ERR2:%x\r\n", s );
                return s;
            }
            retry++;
            Delay_Ms(1);
        }while( ( !( buf[ 2 ] & 0x10 ) ) && ( retry <= 100 ) );

        /* The C_PORT_RESET clear and the connection check below have to run in every case: the
         * HUB does not have to report the reset inside the wait window above (an empty or a slow
         * port does not), and its change bits have to be dropped anyway. */
        {
            retry = 0;
            s = HUB_ClearPortFeature( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, hub_port, HUB_C_PORT_RESET  );

            do
            {
                s = HUB_GetPortStatus( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, hub_port, &buf[ 0 ] );
                if( s != ERR_SUCCESS )
                {
                    DUG_PRINTF( "HUB_PE2_ERR3:%x\r\n", s );
                    return s;
                }
                retry++;
            }while( ( buf[ 2 ] & 0x10 ) && ( retry <= 10 ) );

            if( retry != 10 )
            {
                if( buf[ 0 ] & 0x01 )
                {
                    DUG_PRINTF( "Hub Port%x In\r\n", hub_port );
                    return ERR_USB_CONNECT;
                }
            }
        }
    }

    return ERR_USB_UNKNOWN;
}

/*********************************************************************
 * @fn      HUB_CheckPortSpeed
 *
 * @brief
 *
 * @para
 *
 * @return  none
 */
uint8_t HUB_CheckPortSpeed( uint8_t usb_port, uint8_t hub_port, uint8_t *pbuf )
{
    uint8_t  s;

    s = HUB_GetPortStatus( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, hub_port, pbuf );
    if( s )
    {
        return s;
    }

    if( pbuf[ 1 ] & 0x02 )
    {
        return USB_LOW_SPEED;
    }
    else
    {
        if( pbuf[ 1 ] & 0x04 )
        {
            return USB_HIGH_SPEED;
        }
        else
        {
            return USB_FULL_SPEED;
        }
    }
}

#if DEF_USBFS_PORT_EN
/*********************************************************************
 * @fn      USBH_SetSelfAddr
 *
 * @brief   Select the address of the USB device operated by the host.
 *
 * @para    usb_port: USB host port
 *          addr: USB device address
 *
 * @return  none
 */
static void USBH_SetSelfAddr( uint8_t usb_port, uint8_t addr )
{
    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        USBFSH_SetSelfAddr( addr );
#endif
    }
}

/*********************************************************************
 * @fn      USBH_SetSelfSpeed
 *
 * @brief   Set the speed of the USB device operated by the host.
 *
 * @para    usb_port: USB host port
 *          speed: USB device speed
 *
 * @return  none
 */
static void USBH_SetSelfSpeed( uint8_t usb_port, uint8_t speed )
{
    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        USBFSH_SetSelfSpeed( speed );
        /* USBFSH_SetSelfSpeed( ) also enables the PRE token (HOST_SETUP.PRE_PID_EN) which is what
         * makes a low-speed device behind a HUB reachable. The root port itself has to keep the
         * speed of its link: with a HUB in between it must stay in full speed mode, the HUB is
         * the one which converts the transaction to low speed, otherwise the HUB itself cannot
         * be addressed any more. */
        if( ( speed != USB_LOW_SPEED ) || ( RootHubDev[ usb_port ].bSpeed != USB_LOW_SPEED ) )
        {
            USBFSH->HOST_CTRL &= ~USBFS_UH_LOW_SPEED;
        }
#endif
    }
}

#endif

#if DEF_USBFS_PORT_EN
/*********************************************************************
 * @fn      USBH_HubDevGetDeviceDescr
 *
 * @brief   Get the device descriptor of the device connected to a HUB port.
 *
 * @para    usb_port: USB host port
 *          pep0_size: Device endpoint 0 size
 *          pbuf: Data buffer
 *
 * @return  The result of getting the device descriptor.
 */
static uint8_t USBH_HubDevGetDeviceDescr( uint8_t usb_port, uint8_t *pep0_size, uint8_t *pbuf )
{
    uint8_t s = ERR_USB_UNSUPPORT;

    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_GetDeviceDescr( pep0_size, pbuf );
#endif
    }

    return s;
}

/*********************************************************************
 * @fn      USBH_HubDevSetUsbAddress
 *
 * @brief   Set the address of the device connected to a HUB port.
 *
 * @para    usb_port: USB host port
 *          ep0_size: Device endpoint 0 size
 *          addr: Device address
 *
 * @return  The result of setting the device address.
 */
static uint8_t USBH_HubDevSetUsbAddress( uint8_t usb_port, uint8_t ep0_size, uint8_t addr )
{
    uint8_t s = ERR_USB_UNSUPPORT;

    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_SetUsbAddress( ep0_size, addr );
#endif
    }

    return s;
}

/*********************************************************************
 * @fn      USBH_HubDevGetConfigDescr
 *
 * @brief   Get the configuration descriptor of the device connected to a HUB port.
 *
 * @para    usb_port: USB host port
 *          ep0_size: Device endpoint 0 size
 *          pbuf: Data buffer
 *          buf_len: Length of the data buffer
 *          plen: Data length
 *
 * @return  The result of getting the configuration descriptor.
 */
static uint8_t USBH_HubDevGetConfigDescr( uint8_t usb_port, uint8_t ep0_size, uint8_t *pbuf, uint16_t buf_len, uint16_t *plen )
{
    uint8_t s = ERR_USB_UNSUPPORT;

    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_GetConfigDescr( ep0_size, pbuf, buf_len, plen );
#endif
    }

    return s;
}

/*********************************************************************
 * @fn      USBH_HubDevSetUsbConfig
 *
 * @brief   Set the configuration of the device connected to a HUB port.
 *
 * @para    usb_port: USB host port
 *          ep0_size: Device endpoint 0 size
 *          cfg_val: Configuration value
 *
 * @return  The result of setting the device configuration.
 */
static uint8_t USBH_HubDevSetUsbConfig( uint8_t usb_port, uint8_t ep0_size, uint8_t cfg_val )
{
    uint8_t s = ERR_USB_UNSUPPORT;

    if( usb_port == DEF_USBFS_PORT_INDEX )
    {
#if DEF_USBFS_PORT_EN
        s = USBFSH_SetUsbConfig( ep0_size, cfg_val );
#endif
    }

    return s;
}

#endif

/*********************************************************************
 * @fn      USBH_EnumHubPortDevice
 *
 * @brief
 *
 * @para
 *
 * @return  none
 */
uint8_t USBH_EnumHubPortDevice( uint8_t usb_port, uint8_t hub_port, uint8_t *paddr, uint8_t *ptype )
{
    uint8_t  s;
    uint8_t  enum_cnt;
    uint16_t len;
    uint8_t  cfg_val;
#if DEF_DEBUG_PRINTF
    uint16_t i;
#endif

    /* Get USB device descriptor */
    DUG_PRINTF("(S1)Get DevDesc: \r\n");
    enum_cnt = 0;
    do
    {
        enum_cnt++;
        s = USBH_HubDevGetDeviceDescr( usb_port, &RootHubDev[ usb_port ].Device[ hub_port ].bEp0MaxPks, DevDesc_Buf );
        if( s == ERR_SUCCESS )
        {
#if DEF_DEBUG_PRINTF
            for( i = 0; i < 18; i++ )
            {
                DUG_PRINTF( "%02x ", DevDesc_Buf[ i ] );
            }
            DUG_PRINTF( "\r\n" );
#endif
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
            if( enum_cnt >= 10 )
            {
                return DEF_DEV_DESCR_GETFAIL;
            }
        }
    }while( ( s != ERR_SUCCESS ) && ( enum_cnt < 10 ) );

    /* Set the USB device address */
    DUG_PRINTF( "Set DevAddr: \r\n" );
    enum_cnt = 0;
    do
    {
        enum_cnt++;
        s = USBH_HubDevSetUsbAddress( usb_port, RootHubDev[ usb_port ].Device[ hub_port ].bEp0MaxPks, \
                                  RootHubDev[ usb_port ].Device[ hub_port ].DeviceIndex + USB_DEVICE_ADDR );
        if( s == ERR_SUCCESS )
        {
            /* Save address */
            *paddr = RootHubDev[ usb_port ].Device[ hub_port ].DeviceIndex + USB_DEVICE_ADDR;
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
            if( enum_cnt >= 10 )
            {
                return DEF_DEV_ADDR_SETFAIL;
            }
        }
    }while( ( s != ERR_SUCCESS ) && ( enum_cnt < 10 ) );
    Delay_Ms( 5 );

    /* Get USB configuration descriptor */
    DUG_PRINTF( "Get DevCfgDesc: \r\n" );
    enum_cnt = 0;
    do
    {
        enum_cnt++;
        s = USBH_HubDevGetConfigDescr( usb_port, RootHubDev[ usb_port ].Device[ hub_port ].bEp0MaxPks, Com_Buf, DEF_COM_BUF_LEN, &len );
        if( s == ERR_SUCCESS )
        {
#if DEF_DEBUG_PRINTF
            for( i = 0; i < len; i++ )
            {
                DUG_PRINTF( "%02x ", Com_Buf[ i ] );
            }
            DUG_PRINTF( "\r\n" );
#endif

            /* Save configuration value */
            cfg_val = ( (PUSB_CFG_DESCR)Com_Buf )->bConfigurationValue;

            /* Analyze USB device type */
            USBH_AnalyseType( DevDesc_Buf, Com_Buf, ptype );
            DUG_PRINTF( "DevType: %02x\r\n", *ptype );
        }
        else
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
            if( enum_cnt >= 10 )
            {
                return DEF_DEV_DESCR_GETFAIL;
            }
        }
    }while( ( s != ERR_SUCCESS ) && ( enum_cnt < 10 ) );

    /* Set USB device configuration value */
    DUG_PRINTF( "Set CfgValue: \r\n" );
    enum_cnt = 0;
    do
    {
        enum_cnt++;
        s = USBH_HubDevSetUsbConfig( usb_port, RootHubDev[ usb_port ].Device[ hub_port ].bEp0MaxPks, cfg_val );
        if( s != ERR_SUCCESS )
        {
            DUG_PRINTF( "Err(%02x)\r\n", s );
            if( enum_cnt >= 10 )
            {
                return DEF_CFG_VALUE_SETFAIL;
            }
        }
    }while( ( s != ERR_SUCCESS ) && ( enum_cnt < 10 ) );

    return( ERR_SUCCESS );
}

/*********************************************************************
 * @fn      KB_AnalyzeKeyValue
 *
 * @brief   Handle keyboard lighting.
 *
 * @para    index: USB host port
 *          intfnum: Interface number.
 *          pbuf: Data buffer.
 *          len: Data length.
 *
 * @return  The result of the analysis.
 */
void KB_AnalyzeKeyValue( uint8_t index, uint8_t intf_num, uint8_t *pbuf, uint16_t len )
{
    uint8_t  i;
    uint8_t  value;
    uint8_t  bit_pos = 0x00;

    value = HostCtl[ index ].Interface[ intf_num ].SetReport_Value;

    for( i = HostCtl[ index ].Interface[ intf_num ].LED_Usage_Min; i <= HostCtl[ index ].Interface[ intf_num ].LED_Usage_Max; i++ )
    {
        if( i == 0x01 )
        {
            if( memchr( pbuf, DEF_KEY_NUM, len ) )
            {
                HostCtl[ index ].Interface[ intf_num ].SetReport_Value ^= ( 1 << bit_pos );
            }
        }
        else if( i == 0x02 )
        {
            if( memchr( pbuf, DEF_KEY_CAPS, len ) )
            {
                HostCtl[ index ].Interface[ intf_num ].SetReport_Value ^= ( 1 << bit_pos );
            }
        }
        else if( i == 0x03 )
        {
            if( memchr( pbuf, DEF_KEY_SCROLL, len ) )
            {
                HostCtl[ index ].Interface[ intf_num ].SetReport_Value ^= ( 1 << bit_pos );
            }
        }

        bit_pos++;
    }

    if( value != HostCtl[ index ].Interface[ intf_num ].SetReport_Value )
    {
        HostCtl[ index ].Interface[ intf_num ].SetReport_Flag = 1;
    }
    else
    {
        HostCtl[ index ].Interface[ intf_num ].SetReport_Flag = 0;
    }
}

/*********************************************************************
 * @fn      KB_SetReport
 *
 * @brief   Handle keyboard lighting.
 *
 * @para    index: USB device number.
 *          intf_num: Interface number.
 *
 * @return  The result of the handling keyboard lighting.
 */
uint8_t KB_SetReport( uint8_t usb_port, uint8_t index, uint8_t ep0_size, uint8_t intf_num )
{
    uint8_t  dat[ 2 ];
    uint16_t len;

    if( HostCtl[ index ].Interface[ intf_num ].IDFlag )
    {
        dat[ 0 ] = HostCtl[ index ].Interface[ intf_num ].ReportID;
        dat[ 1 ] = HostCtl[ index ].Interface[ intf_num ].SetReport_Value;
        len = 2;
    }
    else
    {
        dat[ 0 ] = HostCtl[ index ].Interface[ intf_num ].SetReport_Value;
        len = 1;
    }

    if( HostCtl[ index ].Interface[ intf_num ].SetReport_Swi == 1 ) // Perform lighting operation through endpoint0
    {
        /* Send set report command */
        return HID_SetReport( usb_port, ep0_size, intf_num, dat, &len );
    }
    else if( HostCtl[ index ].Interface[ intf_num ].SetReport_Swi == 0xFF )  // Perform lighting operation through other endpoint
    {
        return USBH_SendHidData( usb_port, index, intf_num, 0, dat, len );
    }

    return ERR_SUCCESS;
}

/*********************************************************************
 * @fn      USBH_UpdateReadyFlags
 *
 * @brief   Publishes the readiness of the USB host stack (see app_km.h): the root device of
 *          the port and the HID keyboard interfaces of the devices behind a HUB. The FPGA
 *          task logs these flags, the hot key handling planned later will use them.
 *
 * @param   usb_port - USB host port.
 *
 * @return  none
 */
static void USBH_UpdateReadyFlags( uint8_t usb_port )
{
    uint8_t index;
    uint8_t i, j;

    if( RootHubDev[ usb_port ].bStatus < ROOT_DEV_SUCCESS )
    {
        return;
    }

    g_usbRootReady = 1;

    /* the root device itself */
    index = RootHubDev[ usb_port ].DeviceIndex;
    for( i = 0; i < HostCtl[ index ].InterfaceNum; i++ )
    {
        if( HostCtl[ index ].Interface[ i ].Type == DEC_KEY )
        {
            g_usbHidKbReady = 1;
        }
    }

    /* the devices behind the HUB of this port */
    for( j = 0; j < RootHubDev[ usb_port ].bPortNum; j++ )
    {
        if( RootHubDev[ usb_port ].Device[ j ].bStatus < ROOT_DEV_SUCCESS )
        {
            continue;
        }

        index = RootHubDev[ usb_port ].Device[ j ].DeviceIndex;
        for( i = 0; i < HostCtl[ index ].InterfaceNum; i++ )
        {
            if( HostCtl[ index ].Interface[ i ].Type == DEC_KEY )
            {
                g_usbHidKbReady = 1;
            }
        }
    }
}

/*********************************************************************
 * @fn      USBH_MainDeal
 *
 * @brief   Provide a simple enumeration process for USB devices and
 *          obtain keyboard and mouse data at regular intervals.
 *
 * @return  none
 */
void USBH_MainDeal( void )
{
    uint8_t  s;
    uint8_t  usb_port;
#if DEF_USBFS_PORT_EN
    uint8_t  hub_port;
    uint8_t  hub_dat;
#endif
    uint8_t  index;
    uint8_t  intf_num, in_num;
    uint16_t len;
#if ( DEF_DEBUG_PRINTF && DEF_DEBUG_HID_REPORT )
    uint16_t i;
#endif
    
    for( usb_port = 0; usb_port < DEF_TOTAL_ROOT_HUB; usb_port++ )
    {
        s = USBH_CheckRootHubPortStatus( usb_port ); // Check USB device connection or disconnection
        if( s == ROOT_DEV_CONNECTED )
        {
            DUG_PRINTF( "USB Port%x Dev In.\r\n", usb_port );
            
            /* Set root device state parameters */
            RootHubDev[ usb_port ].bStatus = ROOT_DEV_CONNECTED;
            RootHubDev[ usb_port ].DeviceIndex = usb_port * DEF_ONE_USB_SUP_DEV_TOTAL;

            s = USBH_EnumRootDevice( usb_port ); // Simply enumerate root device
            if( s == ERR_SUCCESS )
            {
                if( RootHubDev[ usb_port ].bType == USB_DEV_CLASS_HID ) // Further enumerate it if this device is a HID device
                {
                    DUG_PRINTF("Root Device Is HID. ");

                    s = USBH_EnumHidDevice( usb_port, RootHubDev[ usb_port ].DeviceIndex, RootHubDev[ usb_port ].bEp0MaxPks );
                    DUG_PRINTF( "Further Enum Result: " );
                    if( s == ERR_SUCCESS )
                    {
                        DUG_PRINTF( "OK\r\n" );
                        
                        /* Set the connection status of the device  */
                        RootHubDev[ usb_port ].bStatus = ROOT_DEV_SUCCESS;
                    }
                    else if( s != ERR_USB_DISCON )
                    {
                        DUG_PRINTF( "Err(%02x)\r\n", s );
                        
                        RootHubDev[ usb_port ].bStatus = ROOT_DEV_FAILED;
                    }
                }
                else if( RootHubDev[ usb_port ].bType == USB_DEV_CLASS_HUB )
                {
                    DUG_PRINTF("Root Device Is HUB. ");

                    s = USBH_EnumHubDevice( usb_port, RootHubDev[ usb_port ].bEp0MaxPks );
                    DUG_PRINTF( "Further Enum Result: " );
                    if( s == ERR_SUCCESS )
                    {
                        DUG_PRINTF( "OK\r\n" );

                        /* Set the connection status of the device  */
                        RootHubDev[ usb_port ].bStatus = ROOT_DEV_SUCCESS;

                        /* Check every port of the HUB once, see USBH_HubScanAll[ ] */
                        USBH_HubScanAll[ usb_port ] = 1;
#if DEF_DEBUG_HUB_SCAN
                        DUG_PRINTF( "HUB ports will be scanned\r\n" );
#endif
                    }
                    else if( s != ERR_USB_DISCON )
                    {
                        DUG_PRINTF( "Err(%02x)\r\n", s );

                        RootHubDev[ usb_port ].bStatus = ROOT_DEV_FAILED;
                    }
                }
                else // Detect that this device is a Non-HID device
                {
                    DUG_PRINTF( "Root Device Is " );
                    switch( RootHubDev[ usb_port ].bType )
                    {
                        case USB_DEV_CLASS_STORAGE:
                            DUG_PRINTF("Storage. ");
                            break;
                        case USB_DEV_CLASS_PRINTER:
                            DUG_PRINTF("Printer. ");
                            break;
                        case DEF_DEV_TYPE_UNKNOWN:
                            DUG_PRINTF("Unknown. ");
                            break;
                    }
                    DUG_PRINTF( "End Enum.\r\n" );
                    
                    RootHubDev[ usb_port ].bStatus = ROOT_DEV_SUCCESS;
                }
            }
            else if( s != ERR_USB_DISCON )
            {
                /* Enumeration failed */
                DUG_PRINTF( "Enum Fail with Error Code:%x\r\n",s );
                RootHubDev[ usb_port ].bStatus = ROOT_DEV_FAILED;
            }
        }
        else if( s == ROOT_DEV_DISCONNECT )
        {
            DUG_PRINTF( "USB Port%x Dev Out.\r\n", usb_port );
            
            /* Clear parameters */
            index = RootHubDev[ usb_port ].DeviceIndex;
            memset( &RootHubDev[ usb_port ].bStatus, 0, sizeof( ROOT_HUB_DEVICE ) );
            memset( &HostCtl[ index ].InterfaceNum, 0, sizeof( HOST_CTL ) );

            USBH_HubScanAll[ usb_port ] = 0;

        }
    }

    /* Get the data of the HID device connected to the USB host port */
    for( usb_port = 0; usb_port < DEF_TOTAL_ROOT_HUB; usb_port++ )
    {
        if( RootHubDev[ usb_port ].bStatus >= ROOT_DEV_SUCCESS )
        {
            index = RootHubDev[ usb_port ].DeviceIndex;

            /* publish the readiness of the USB host stack (see app_km.h) */
            USBH_UpdateReadyFlags( usb_port );

            if( RootHubDev[ usb_port ].bType == USB_DEV_CLASS_HID )
            {
                for( intf_num = 0; intf_num < HostCtl[ index ].InterfaceNum; intf_num++ )
                {
                    for( in_num = 0; in_num < HostCtl[ index ].Interface[ intf_num ].InEndpNum; in_num++ )
                    {                   
                        /* Get endpoint data based on the interval time of the device */
                        if( HostCtl[ index ].Interface[ intf_num ].InEndpTimeCount[ in_num ] >= HostCtl[ index ].Interface[ intf_num ].InEndpInterval[ in_num ] )
                        {
                            HostCtl[ index ].Interface[ intf_num ].InEndpTimeCount[ in_num ] %= HostCtl[ index ].Interface[ intf_num ].InEndpInterval[ in_num ];
               
                            /* Get endpoint data */
                            s = USBH_GetHidData( usb_port, index, intf_num, in_num, Com_Buf, &len );
                            if( s == ERR_SUCCESS )
                            {
#if DEF_DEBUG_HID_REPORT
                                for( i = 0; i < len; i++ )
                                {
                                    DUG_PRINTF( "%02x ", Com_Buf[ i ] );
                                }
                                DUG_PRINTF( "\r\n" );
#endif
                                
                                /* Handle keyboard lighting */
                                if( HostCtl[ index ].Interface[ intf_num ].Type == DEC_KEY )
                                {
                                    KB_AnalyzeKeyValue( index, intf_num, Com_Buf, len );

                                    if( HostCtl[ index ].Interface[ intf_num ].SetReport_Flag )
                                    {
                                        KB_SetReport( usb_port, index, RootHubDev[ usb_port ].bEp0MaxPks, intf_num );
                                    }
                                }
                            }
                            else if( s == ERR_USB_DISCON )
                            {
                                break;
                            }
                            else if( s == ( USB_PID_STALL | ERR_USB_TRANSFER ) )
                            {
                                /* USB device abnormal event */
                                DUG_PRINTF("Abnormal\r\n");
                                
                                /* Clear endpoint */
                                USBH_ClearEndpStall( usb_port, HostCtl[ index ].Interface[ intf_num ].InEndpAddr[ in_num ] | 0x80 );
                                HostCtl[ index ].Interface[ intf_num ].InEndpTog[ in_num ] = 0x00;
                                
                                /* Judge the number of error */
                                HostCtl[ index ].ErrorCount++;
                                if( HostCtl[ index ].ErrorCount >= 10 )
                                {
                                    /* Re-enumerate the device and clear the endpoint again */
                                    memset( &RootHubDev[ usb_port ].bStatus, 0, sizeof( ROOT_HUB_DEVICE ) );
                                    s = USBH_EnumRootDevice( usb_port );
                                    if( s == ERR_SUCCESS )
                                    {
                                        USBH_ClearEndpStall( usb_port, HostCtl[ index ].Interface[ intf_num ].InEndpAddr[ in_num ] | 0x80 );
                                        HostCtl[ index ].ErrorCount = 0x00;
                                        
                                        RootHubDev[ usb_port ].bStatus = ROOT_DEV_CONNECTED; 
                                        RootHubDev[ usb_port ].DeviceIndex = usb_port * DEF_ONE_USB_SUP_DEV_TOTAL;
                                        
                                        memset( &HostCtl[ index ].InterfaceNum, 0, sizeof( HOST_CTL ) );
                                        s = USBH_EnumHidDevice( usb_port, index, RootHubDev[ usb_port ].bEp0MaxPks );
                                        if( s == ERR_SUCCESS )
                                        {
                                            RootHubDev[ usb_port ].bStatus = ROOT_DEV_SUCCESS; 
                                        }
                                        else if( s != ERR_USB_DISCON )
                                        {
                                            RootHubDev[ usb_port ].bStatus = ROOT_DEV_FAILED; 
                                        }
                                    }
                                    else if( s != ERR_USB_DISCON )
                                    {
                                        RootHubDev[ usb_port ].bStatus = ROOT_DEV_FAILED;
                                    }
                                }
                            }
                        }
                    }

                    if( s == ERR_USB_DISCON )
                    {
                        break;
                    }
                }
            }
#if DEF_USBFS_PORT_EN
            /* A HUB connected to the root port is processed here. */
            else if( RootHubDev[ usb_port ].bType == USB_DEV_CLASS_HUB )
            {
                /* Query port status change */
                if( HostCtl[ index ].Interface[ 0 ].InEndpTimeCount[ 0 ] >= HostCtl[ index ].Interface[ 0 ].InEndpInterval[ 0 ] )
                {
                    HostCtl[ index ].Interface[ 0 ].InEndpTimeCount[ 0 ] %= HostCtl[ index ].Interface[ 0 ].InEndpInterval[ 0 ];

                    /* Select the HUB device */
                    USBH_SetSelfAddr( usb_port, RootHubDev[ usb_port ].bAddress );
                    USBH_SetSelfSpeed( usb_port, RootHubDev[ usb_port ].bSpeed );

                    /* Get HUB interrupt endpoint data */
                    s = USBH_GetHidData( usb_port, index, 0, 0, Com_Buf, &len );

                    /* Check every port of the HUB once right after the HUB has been enumerated:
                     * a device plugged in before the enumeration does not always generate a new
                     * port change event. */
                    if( USBH_HubScanAll[ usb_port ] )
                    {
                        USBH_HubScanAll[ usb_port ] = 0;
                        s = ERR_SUCCESS;
                        hub_dat = 0xFF;
                    }
                    else if( s == ERR_SUCCESS )
                    {
                        hub_dat = Com_Buf[ 0 ];
                    }

                    if( s == ERR_SUCCESS )
                    {
#if DEF_DEBUG_HUB_SCAN
                        DUG_PRINTF( "Hub Int Data:%02x\r\n", hub_dat );
#endif

                        for( hub_port = 0; hub_port < RootHubDev[ usb_port ].bPortNum; hub_port++ )
                        {
                            uint8_t port_st[ 4 ];       /* wPortStatus (2 bytes) + wPortChange (2) */
                            uint8_t pchg;               /* pending wPortChange bits of the port */

                            /* The HUB has to be addressed again: the enumeration of a device behind
                             * a HUB port changes the address and the speed the host operates (see
                             * USBH_EnumHubPortDevice( )), while all the HUB requests below (port
                             * status, port feature, reset) must be sent to the HUB itself. */
                            USBH_SetSelfAddr( usb_port, RootHubDev[ usb_port ].bAddress );
                            USBH_SetSelfSpeed( usb_port, RootHubDev[ usb_port ].bSpeed );

                            /* The ports are serviced according to their own status, not according
                             * to the interrupt bitmap of the HUB: a change is reported in that
                             * bitmap only while it is new, so a port can keep pending change bits
                             * while the bitmap never reports it again (such a port was not
                             * reset/enumerated at all before). */
                            s = HUB_GetPortStatus( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, ( hub_port + 1 ), &port_st[ 0 ] );
                            if( s != ERR_SUCCESS )
                            {
                                DUG_PRINTF( "HubP%x status err:%x\r\n", hub_port + 1, s );
                                continue;
                            }
                            pchg = port_st[ 2 ];

#if DEF_DEBUG_HUB_SCAN
                            DUG_PRINTF( "HubP%x st:%02x%02x chg:%02x\r\n", hub_port + 1, port_st[ 1 ], port_st[ 0 ], pchg );
#endif

                            if( port_st[ 0 ] & 0x01 )
                            {
                                if( RootHubDev[ usb_port ].Device[ hub_port ].bStatus == ROOT_DEV_SUCCESS )
                                {
                                    /* A device of this port is up: nothing to recover. */
                                    USBH_HubPortRetry[ usb_port ][ hub_port ] = 0;
                                    USBH_HubRecoverCount = 0;
                                }
                                else if( USBH_HubPortRetry[ usb_port ][ hub_port ] < DEF_HUB_ENUM_RETRY_MAX )
                                {
                                    /* The device is present but not enumerated (the port reset or
                                     * the enumeration failed): pretend a connection change so that
                                     * the port is reset and enumerated again. This also covers a
                                     * change which the HUB interrupt bitmap did not report. */
                                    USBH_HubPortRetry[ usb_port ][ hub_port ]++;
                                    DUG_PRINTF( "HubP%x enum retry%x\r\n", hub_port + 1, USBH_HubPortRetry[ usb_port ][ hub_port ] );
                                    pchg |= 0x01;
                                }
                                else if( USBH_HubPortRetry[ usb_port ][ hub_port ] == DEF_HUB_ENUM_RETRY_MAX )
                                {
                                    /* The retries are exhausted, the HUB or the port may be wedged
                                     * (the device does enumerate after a power-on restart): restart
                                     * the host stack, AppUsb_Step( ) performs it. */
                                    USBH_HubPortRetry[ usb_port ][ hub_port ]++;
                                    DUG_PRINTF( "HUB port%x enum failed, giving up\r\n", hub_port + 1 );

#if DEF_HUB_RECOVER_RESTART
                                    if( USBH_HubRecoverCount < DEF_HUB_RECOVER_MAX )
                                    {
                                        if( (uint32_t)( g_ms_ticks - USBH_HubRecoverTick ) >= DEF_HUB_RECOVER_PERIOD_MS )
                                        {
                                            USBH_HubRecoverCount++;
                                            USBH_HubRecoverTick = g_ms_ticks;
                                            DUG_PRINTF( "USB: HUB recovery restart %x/%x\r\n", USBH_HubRecoverCount, DEF_HUB_RECOVER_MAX );
                                            AppUsb_RequestRestart( );
                                        }
                                    }
                                    else
                                    {
                                        DUG_PRINTF( "USB: HUB recovery limit reached\r\n" );
                                    }
#endif
                                }
                            }
                            else
                            {
                                /* No device on the port: nothing to enumerate or to retry. */
                                USBH_HubPortRetry[ usb_port ][ hub_port ] = 0;
                            }

                            /* HUB Port PreEnumate Step 1: C_PORT_CONNECTION */
                            s = HUB_Port_PreEnum1( usb_port, ( hub_port + 1 ), &pchg );
#if DEF_DEBUG_HUB_SCAN
                            DUG_PRINTF( "HubP%x pre1=%02x\r\n", hub_port + 1, s );
#endif
                            if( s == ERR_USB_DISCON )
                            {
                                /* The device is gone: drop the pending change bits of the port
                                 * (C_PORT_ENABLE in particular), otherwise the HUB keeps it in
                                 * its interrupt bitmap and the port is scanned forever. */
                                HUB_Port_ClearChanges( usb_port, ( hub_port + 1 ) );

#if DEF_HUB_PORT_POWER_CYCLE
                                /* A connection change reported for a port which was already empty
                                 * means that the HUB did not detect the device which has just been
                                 * plugged in: cycle the port power, the device then re-establishes
                                 * the D+ pull-up and the port is detected again. */
                                if( ( ( pchg & 0x01 ) != 0 ) && ( USBH_HubPortEmpty[ usb_port ][ hub_port ] != 0 ) )
                                {
                                    DUG_PRINTF( "HubP%x power cycle (device not detected)\r\n", hub_port + 1 );
                                    HUB_ClearPortFeature( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, ( hub_port + 1 ), HUB_PORT_POWER );
                                    Delay_Ms( 100 );
                                    HUB_SetPortFeature( usb_port, RootHubDev[ usb_port ].bEp0MaxPks, ( hub_port + 1 ), HUB_PORT_POWER );
                                    Delay_Ms( 100 );
                                }
#endif
                                USBH_HubPortEmpty[ usb_port ][ hub_port ] = 1;

                                /* Clear parameters */
                                memset( &HostCtl[ RootHubDev[ usb_port ].Device[ hub_port ].DeviceIndex ], 0, sizeof( HOST_CTL ) );
                                memset( &RootHubDev[ usb_port ].Device[ hub_port ].bStatus, 0, sizeof( HUB_DEVICE ) );
                                continue;
                            }

                            /* Remember whether the port has a device: a connection change reported
                             * later for an already empty port means that the HUB missed the device
                             * which has just been plugged in (see the port power cycle above). */
                            USBH_HubPortEmpty[ usb_port ][ hub_port ] = ( ( port_st[ 0 ] & 0x01 ) != 0 ) ? 0 : 1;

                            /* HUB Port PreEnumate Step 2: Set/Clear PORT_RESET. The port is reset
                             * only when it reported a change which needs the reset, and only when
                             * there is a device on it; the settle delay is needed only then. */
                            if( ( ( pchg & 0x13 ) != 0 ) && ( ( port_st[ 0 ] & 0x01 ) != 0 ) )
                            {
                                Delay_Ms( 100 );
                            }
                            s = HUB_Port_PreEnum2( usb_port, ( hub_port + 1 ), &pchg );
#if DEF_DEBUG_HUB_SCAN
                            DUG_PRINTF( "HubP%x pre2=%02x\r\n", hub_port + 1, s );
#endif
                            if( s == ERR_USB_CONNECT )
                            {
                                /* Set parameters */
                                RootHubDev[ usb_port ].Device[ hub_port ].bStatus = ROOT_DEV_CONNECTED;
                                RootHubDev[ usb_port ].Device[ hub_port ].bEp0MaxPks = DEFAULT_ENDP0_SIZE;
                                RootHubDev[ usb_port ].Device[ hub_port ].DeviceIndex = usb_port * DEF_ONE_USB_SUP_DEV_TOTAL + hub_port + 1;
                            }
                            else
                            {
                                /* Nothing to enumerate on this port (or the port has no device):
                                 * drop its pending change bits, otherwise the HUB keeps reporting
                                 * it in its interrupt bitmap. */
                                HUB_Port_ClearChanges( usb_port, ( hub_port + 1 ) );
                            }

                            /* Enumerate HUB Device */
                            if( RootHubDev[ usb_port ].Device[ hub_port ].bStatus == ROOT_DEV_CONNECTED )
                            {
                                /* Check device speed */
                                RootHubDev[ usb_port ].Device[ hub_port ].bSpeed = HUB_CheckPortSpeed( usb_port, ( hub_port + 1 ), Com_Buf );
                                DUG_PRINTF( "Dev Speed:%x\r\n", RootHubDev[ usb_port ].Device[ hub_port ].bSpeed );


                                /* Select the device connected to the specified HUB port. On the
                                 * USBFS port USBH_SetSelfSpeed( ) also updates the low-speed
                                 * related bits of the host port. */
                                USBH_SetSelfAddr( usb_port, RootHubDev[ usb_port ].Device[ hub_port ].bAddress );
                                USBH_SetSelfSpeed( usb_port, RootHubDev[ usb_port ].Device[ hub_port ].bSpeed );

                                /* Enumerate the USB device of the current HUB port */
                                DUG_PRINTF("Enum_HubDevice\r\n");
                                s = USBH_EnumHubPortDevice( usb_port, hub_port, &RootHubDev[ usb_port ].Device[ hub_port ].bAddress, \
                                                            &RootHubDev[ usb_port ].Device[ hub_port ].bType );
                                if( s == ERR_SUCCESS )
                                {
                                    if( RootHubDev[ usb_port ].Device[ hub_port ].bType == USB_DEV_CLASS_HID )
                                    {
                                        DUG_PRINTF( "HUB port%x device is HID! Further Enum:\r\n", hub_port + 1 );

                                        /* Perform HID class enumeration on the current device */
                                        s = USBH_EnumHidDevice( usb_port, RootHubDev[ usb_port ].Device[ hub_port ].DeviceIndex, \
                                                                RootHubDev[ usb_port ].Device[ hub_port ].bEp0MaxPks );
                                        if( s == ERR_SUCCESS )
                                        {
                                            RootHubDev[ usb_port ].Device[ hub_port ].bStatus = ROOT_DEV_SUCCESS;
                                            DUG_PRINTF( "OK!\r\n" );
                                        }
                                    }
                                    else // Detect that this device is a Non-HID device
                                    {
                                        DUG_PRINTF( "HUB port%x device is ", hub_port + 1 );
                                        switch( RootHubDev[ usb_port ].Device[ hub_port ].bType )
                                        {
                                            case USB_DEV_CLASS_STORAGE:
                                                DUG_PRINTF("storage!\r\n");
                                                break;
                                            case USB_DEV_CLASS_PRINTER:
                                                DUG_PRINTF("printer!\r\n");
                                                break;
                                            case USB_DEV_CLASS_HUB:
                                                DUG_PRINTF("printer!\r\n");
                                                break;
                                            case DEF_DEV_TYPE_UNKNOWN:
                                                DUG_PRINTF("unknown!\r\n");
                                                break;
                                        }
                                        RootHubDev[ usb_port ].Device[ hub_port ].bStatus = ROOT_DEV_SUCCESS;
                                    }
                                }
                                else
                                {
                                    RootHubDev[ usb_port ].Device[ hub_port ].bStatus = ROOT_DEV_FAILED;
                                    DUG_PRINTF( "HUB Port%x Enum Err!\r\n", hub_port + 1 );
                                }
                            }

                            /* Drop the remaining change bits of the port, otherwise the HUB keeps
                             * reporting it in its interrupt bitmap. The HUB has to be addressed
                             * again here, because the enumeration above changed the address and the
                             * speed the host operates (see USBH_EnumHubPortDevice( )). */
                            USBH_SetSelfAddr( usb_port, RootHubDev[ usb_port ].bAddress );
                            USBH_SetSelfSpeed( usb_port, RootHubDev[ usb_port ].bSpeed );
                            HUB_Port_ClearChanges( usb_port, ( hub_port + 1 ) );
                        }
                    }
                }

                /* Get HUB port HID device data */
                for( hub_port = 0; hub_port < RootHubDev[ usb_port ].bPortNum; hub_port++ )
                {
                    if( RootHubDev[ usb_port ].Device[ hub_port ].bStatus == ROOT_DEV_SUCCESS )
                    {
                        index = RootHubDev[ usb_port ].Device[ hub_port ].DeviceIndex;

                        if( RootHubDev[ usb_port ].Device[ hub_port ].bType == USB_DEV_CLASS_HID )
                        {
                            for( intf_num = 0; intf_num < HostCtl[ index ].InterfaceNum; intf_num++ )
                            {
                                for( in_num = 0; in_num < HostCtl[ index ].Interface[ intf_num ].InEndpNum; in_num++ )
                                {
                                    /* Get endpoint data based on the interval time of the device */
                                    if( HostCtl[ index ].Interface[ intf_num ].InEndpTimeCount[ in_num ] >= HostCtl[ index ].Interface[ intf_num ].InEndpInterval[ in_num ] )
                                    {
                                        HostCtl[ index ].Interface[ intf_num ].InEndpTimeCount[ in_num ] %= HostCtl[ index ].Interface[ intf_num ].InEndpInterval[ in_num ];

                                        /* Select the device connected to the specified HUB port */
                                        USBH_SetSelfAddr( usb_port, RootHubDev[ usb_port ].Device[ hub_port ].bAddress );
                                        USBH_SetSelfSpeed( usb_port, RootHubDev[ usb_port ].Device[ hub_port ].bSpeed );

                                        /* Get endpoint data */
                                        s = USBH_GetHidData( usb_port, index, intf_num, in_num, Com_Buf, &len );
                                        if( s == ERR_SUCCESS )
                                        {
#if DEF_DEBUG_HID_REPORT
                                            for( i = 0; i < len; i++ )
                                            {
                                                DUG_PRINTF( "%02x ", Com_Buf[ i ] );
                                            }
                                            DUG_PRINTF( "\r\n" );
#endif

                                            if( HostCtl[ index ].Interface[ intf_num ].Type == DEC_KEY )
                                            {
                                                KB_AnalyzeKeyValue( index, intf_num, Com_Buf, len );

                                                if( HostCtl[ index ].Interface[ intf_num ].SetReport_Flag )
                                                {
                                                    KB_SetReport( usb_port, index, RootHubDev[ usb_port ].Device[ hub_port ].bEp0MaxPks, intf_num );
                                                }
                                            }
                                        }
                                        else if( s == ERR_USB_DISCON )
                                        {
                                            break;
                                        }
                                    }
                                }

                                if( s == ERR_USB_DISCON )
                                {
                                    break;
                                }
                            }
                        }
                    }
                }
            }
#endif
        }
    }
}

/*********************************************************************
 * @fn      USBH_ClearHubScanState
 *
 * @brief   Drops the pending one-shot HUB port scans (see USBH_HubScanAll).
 *
 * @return  none
 */
static void USBH_ClearHubScanState( void )
{
#if DEF_USBFS_PORT_EN
    uint8_t i;

    for( i = 0; i < DEF_TOTAL_ROOT_HUB; i++ )
    {
        USBH_HubScanAll[ i ] = 0;
        memset( &USBH_HubPortRetry[ i ][ 0 ], 0, sizeof( USBH_HubPortRetry[ i ] ) );
        memset( &USBH_HubPortStuck[ i ][ 0 ], 0, sizeof( USBH_HubPortStuck[ i ] ) );
        memset( &USBH_HubPortEmpty[ i ][ 0 ], 0, sizeof( USBH_HubPortEmpty[ i ] ) );
    }
    /* USBH_HubRecoverCount is intentionally kept: it limits the automatic restarts. */
#endif
}

/*********************************************************************
 * @fn      USBH_StackInit
 *
 * @brief   (Re)initialises the USBFS host controller and clears the whole enumeration state.
 *          A call on a running stack is a full restart: the driver resets the SIE
 *          (USBFS_Host_Init) and the devices are enumerated again. Used by src/app_usb.c.
 *
 * @return  none
 */
void USBH_StackInit( void )
{
    g_usbRootReady = 0;
    g_usbHidKbReady = 0;

    memset( RootHubDev, 0, sizeof( RootHubDev ) );
    memset( HostCtl, 0, sizeof( HostCtl ) );

    USBH_ClearHubScanState( );

#if DEF_USBFS_PORT_EN
    USBFS_RCC_Init( );
    USBFS_Host_Init( ENABLE );
#endif
}

/*********************************************************************
 * @fn      USBH_StackDown
 *
 * @brief   Stops the USBFS host controller and drops the stack state. Used when the USB part is
 *          supplied from the ATX main rails and the PSU is off (see src/app_usb.c).
 *
 * @return  none
 */
void USBH_StackDown( void )
{
    g_usbRootReady = 0;
    g_usbHidKbReady = 0;

    USBH_ClearHubScanState( );

#if DEF_USBFS_PORT_EN
    USBFS_Host_Init( DISABLE );
#endif
}

