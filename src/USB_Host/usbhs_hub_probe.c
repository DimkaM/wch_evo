/********************************** (C) COPYRIGHT  *******************************
* File Name          : usbhs_hub_probe.c
* Version            : V1.0.0
* Date               : 2026/09/23
* Description        : Experimental probe of the USBHD (USBHS) SPLIT transaction mechanism.
*                      The 12 valid bits of R16_UH_SPLIT_DATA are swept in order to find the
*                      encoding which makes a HUB accept a SPLIT packet while the high-speed
*                      link is kept. The candidate found is verified by reading the device
*                      descriptor of the device connected behind the HUB.
*                      Used only when DEF_USBHS_HUB_SPLIT_PROBE is not zero.
*******************************************************************************/


/********************************************************************************/
/* Header File */
#include "usb_host_config.h"

#if DEF_USBHS_HUB_SPLIT_PROBE

/********************************************************************************/
/* Probe Configuration */
#define PROBE_SPLIT_MAX         0x1000          /* number of the 12-bit SPLIT packet values to try */
#define PROBE_NO_TRANSFER       0xFF            /* no transfer at all (see USBHSH_HubSplitTransact) */
#define PROBE_TIMEOUT           150             /* transaction timeout, in polling loop units */
#define PROBE_USE_MF_GAP        0               /* 1: wait for a microframe between the split phases */

#if PROBE_USE_MF_GAP
#define PROBE_GAP()             USBHSH_SplitWaitMicroframe( )
#else
#define PROBE_GAP()             do { } while( 0 )
#endif

/* Phase variants: { start split, complete split } value of RB_UH_*_DATA_NO */
static const uint8_t ProbePhase[ 4 ][ 2 ] =
{
    { 0, 1 },
    { 1, 0 },
    { 0, 0 },
    { 1, 1 }
};

#define PROBE_SPLITCAN_WAIT     100             /* max wait (us) for the "SPLIT can be transmitted" state */

/* Statistics of R8_USB_MIS_ST.RB_UMS_SPLIT_CAN */
static uint16_t ProbeCanReady;
static uint16_t ProbeCanBusy;

/* Struct Definition */
typedef struct
{
    uint8_t  res_ss;                            /* result of the start split phase */
    uint8_t  res_cs;                            /* result of the complete split phase */
    uint8_t  res_in;                            /* result of the start split data phase */
    uint8_t  res_cin;                           /* result of the complete split data phase */
    uint16_t rx_len;                            /* data length of the complete split data phase */
    uint8_t  desc_ok;                           /* device descriptor of the target device was read */
} PROBE_RESULT, *PPROBE_RESULT;

/* Variable Definition */
static uint8_t ProbeDescBuf[ sizeof( USB_DEV_DESCR ) ];

/*********************************************************************
 * @fn      ProbeWaitSplitCan
 *
 * @brief   Wait until the hardware reports that a SPLIT packet can be transmitted
 *          (R8_USB_MIS_ST.RB_UMS_SPLIT_CAN). Statistics are collected in ProbeCanReady and
 *          ProbeCanBusy.
 *
 * @return  1: a SPLIT can be transmitted, 0: timeout
 */
static uint8_t ProbeWaitSplitCan( void )
{
    uint16_t i;

    for( i = 0; i < PROBE_SPLITCAN_WAIT; i++ )
    {
        if( USBHSH->MIS_ST & USBHS_UMS_SPLIT_CAN )
        {
            ProbeCanReady++;
            return 1;
        }
        Delay_Us( 1 );
    }

    ProbeCanBusy++;
    return 0;
}

/*********************************************************************
 * @fn      ProbeTrySplit
 *
 * @brief   Try one SPLIT control transfer (GetDescriptor DEVICE) to the device which is
 *          connected behind the HUB: start split + complete split of the SETUP stage, then
 *          start split + complete split of the data stage.
 *
 * @para    split_data: content of the SPLIT packet
 *          ph_ss     : RB_UH_*_DATA_NO value used for the start split phases
 *          pr        : probe result
 *
 * @return  none
 */
static void ProbeTrySplit( uint16_t split_data, uint8_t ph_ss, uint8_t ph_cs, PPROBE_RESULT pr )
{
    uint16_t cnt;

    pr->res_ss  = PROBE_NO_TRANSFER;
    pr->res_cs  = PROBE_NO_TRANSFER;
    pr->res_in  = PROBE_NO_TRANSFER;
    pr->res_cin = PROBE_NO_TRANSFER;
    pr->rx_len  = 0;
    pr->desc_ok = 0;

    /* The address the host operates (R16_USB_DEV_AD) has to be selected by the caller: it is
     * either the address of the target device or the HUB address, see USBHS_HubSplitProbe( ). */

    /* SETUP stage */
    pUSBHS_SetupRequest->bRequestType = USB_REQ_TYP_IN;
    pUSBHS_SetupRequest->bRequest = USB_GET_DESCRIPTOR;
    pUSBHS_SetupRequest->wValue = (uint16_t)( (uint16_t)USB_DESCR_TYP_DEVICE << 8 );
    pUSBHS_SetupRequest->wIndex = 0x0000;
    pUSBHS_SetupRequest->wLength = (uint16_t)sizeof( USB_DEV_DESCR );

    USBHSH->HOST_TX_LEN = sizeof( USB_SETUP_REQ );
    (void)ProbeWaitSplitCan( );
    pr->res_ss = USBHSH_HubSplitTransact( split_data, ( USB_PID_SETUP << 4 ) | 0x00, 0x00, ph_ss, PROBE_TIMEOUT );
    PROBE_GAP( );
    (void)ProbeWaitSplitCan( );
    pr->res_cs = USBHSH_HubSplitTransact( split_data, ( USB_PID_SETUP << 4 ) | 0x00, 0x00, ph_cs, PROBE_TIMEOUT );
    PROBE_GAP( );

    /* DATA stage */
    USBHSH->HOST_TX_LEN = 0;
    (void)ProbeWaitSplitCan( );
    pr->res_in = USBHSH_HubSplitTransact( split_data, ( USB_PID_IN << 4 ) | 0x00, 0x00, ph_ss, PROBE_TIMEOUT );
    PROBE_GAP( );
    (void)ProbeWaitSplitCan( );
    pr->res_cin = USBHSH_HubSplitTransact( split_data, ( USB_PID_IN << 4 ) | 0x00, 0x00, ph_cs, PROBE_TIMEOUT );

    if( pr->res_cin != PROBE_NO_TRANSFER )
    {
        cnt = USBHSH->RX_LEN;
        if( cnt > sizeof( ProbeDescBuf ) )
        {
            cnt = sizeof( ProbeDescBuf );
        }
        if( cnt != 0 )
        {
            memcpy( ProbeDescBuf, USBHS_RX_Buf, cnt );
            pr->rx_len = cnt;
            if( ( ProbeDescBuf[ 0 ] == 0x12 ) && ( ProbeDescBuf[ 1 ] == USB_DESCR_TYP_DEVICE ) )
            {
                pr->desc_ok = 1;
            }
        }
    }

    USBHSH->HOST_SPLIT_DATA = 0;
}

/*********************************************************************
 * @fn      USBHS_HubSplitProbe
 *
 * @brief   Enumerate the HUB on the high-speed port (no SPLIT is needed to talk to the HUB
 *          itself), reset the port with the target device and sweep R16_UH_SPLIT_DATA.
 *
 * @return  none
 */
void USBHS_HubSplitProbe( void )
{
    uint8_t  s, i, ep0_size = DEFAULT_ENDP0_SIZE;
    uint8_t  cfg_val;
    uint8_t  hub_addr = (uint8_t)( DEF_USBHS_PORT_INDEX + USB_DEVICE_ADDR );
    uint8_t  port = DEF_USBHS_HUB_PROBE_PORT;
    uint8_t  status[ 4 ];
    uint8_t  addr_mode, ph_mode, tok_mode, dn;
    uint8_t  found = 0;
    uint16_t len;
    uint16_t split_val;
    uint16_t hits;
    uint16_t hub_desc_hits = 0;
    PROBE_RESULT pr;

    printf( "\r\n=== USBHS HUB SPLIT probe ===\r\n" );

    /* Reset the port and wait for the HUB to be ready */
    USBHSH_ResetRootHubPortSpeed( 0, USB_HIGH_SPEED );
    for( i = 0, s = 0; i < DEF_RE_ATTACH_TIMEOUT; i++ )
    {
        if( USBHSH_EnableRootHubPort( &RootHubDev[ DEF_USBHS_PORT_INDEX ].bSpeed ) == ERR_SUCCESS )
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
    if( i != 0 )
    {
        printf( "probe: no device connected\r\n" );
        return;
    }

    /* Enumerate the HUB */
    s = USBHSH_GetDeviceDescr( &ep0_size, DevDesc_Buf );
    if( s != ERR_SUCCESS )
    {
        printf( "probe: GetDeviceDescr err %02x\r\n", s );
        return;
    }
    s = USBHSH_SetUsbAddress( ep0_size, hub_addr );
    if( s != ERR_SUCCESS )
    {
        printf( "probe: SetUsbAddress err %02x\r\n", s );
        return;
    }
    USBHSH_SetSelfAddr( hub_addr );

    s = USBHSH_GetConfigDescr( ep0_size, Com_Buf, DEF_COM_BUF_LEN, &len );
    if( s != ERR_SUCCESS )
    {
        printf( "probe: GetConfigDescr err %02x\r\n", s );
        return;
    }
    cfg_val = ( (PUSB_CFG_DESCR)Com_Buf )->bConfigurationValue;
    s = USBHSH_SetUsbConfig( ep0_size, cfg_val );
    if( s != ERR_SUCCESS )
    {
        printf( "probe: SetUsbConfig err %02x\r\n", s );
        return;
    }

    s = HUB_GetClassDevDescr( DEF_USBHS_PORT_INDEX, ep0_size, Com_Buf, &len );
    if( s != ERR_SUCCESS )
    {
        printf( "probe: GetHubDescr err %02x\r\n", s );
        return;
    }
    printf( "probe: hub dev at addr %02x, ports %u, MIS_ST %02x (SPLIT_CAN %u), SPEED_TYPE %02x\r\n",
            hub_addr, ( (PUSB_HUB_DESCR)Com_Buf )->bNbrPorts, USBHSH->MIS_ST,
            ( USBHSH->MIS_ST & USBHS_UMS_SPLIT_CAN ) ? 1 : 0, USBHSH->SPEED_TYPE );

    /* Power the HUB ports */
    for( i = 1; i <= ( (PUSB_HUB_DESCR)Com_Buf )->bNbrPorts; i++ )
    {
        HUB_SetPortFeature( DEF_USBHS_PORT_INDEX, ep0_size, i, HUB_PORT_POWER );
    }
    Delay_Ms( 200 );

    /* Reset the port with the target device */
    USBHSH_SetSelfAddr( hub_addr );
    s = HUB_SetPortFeature( DEF_USBHS_PORT_INDEX, ep0_size, port, HUB_PORT_RESET );
    printf( "probe: port %u reset %02x\r\n", port, s );
    for( i = 0; i < 100; i++ )
    {
        HUB_GetPortStatus( DEF_USBHS_PORT_INDEX, ep0_size, port, status );
        if( status[ 2 ] & 0x10 )
        {
            break;
        }
        Delay_Ms( 1 );
    }
    HUB_ClearPortFeature( DEF_USBHS_PORT_INDEX, ep0_size, port, HUB_C_PORT_RESET );
    HUB_GetPortStatus( DEF_USBHS_PORT_INDEX, ep0_size, port, status );
    printf( "probe: port %u status %02x %02x %02x %02x\r\n", port, status[ 0 ], status[ 1 ], status[ 2 ], status[ 3 ] );
    if( ( status[ 0 ] & 0x01 ) == 0 )
    {
        printf( "probe: no device on the HUB port %u\r\n", port );
        return;
    }
    printf( "probe: target device speed %s\r\n",
            ( status[ 1 ] & 0x02 ) ? "low" : ( ( status[ 1 ] & 0x04 ) ? "high" : "full" ) );

    /* Calibration: plain transactions to the HUB (it is reachable without SPLIT) in order to
     * validate the measurement path. H_RES (INT_ST[3:0]) = 0 means "no response or timed out". */
    USBHSH_SetSelfAddr( hub_addr );
    USBHSH->HOST_SPLIT_DATA = 0;
    s = USBHSH_HubSplitTransact( 0x0000, ( USB_PID_IN << 4 ) | 0x01, USBHS_UH_R_TOG_DATA1, 0, PROBE_TIMEOUT );
    printf( "probe: calib IN hub EP1 tog1 INT_ST=%02x\r\n", s );
    s = USBHSH_HubSplitTransact( 0x0000, ( USB_PID_IN << 4 ) | 0x01, 0x00, 0, PROBE_TIMEOUT );
    printf( "probe: calib IN hub EP1 tog0 INT_ST=%02x\r\n", s );
    s = USBHSH_HubSplitTransact( 0x0000, ( USB_PID_OUT << 4 ) | 0x00, 0x00, 0, PROBE_TIMEOUT );
    printf( "probe: calib OUT hub EP0 INT_ST=%02x\r\n", s );

    /* Reference: the full-speed device behind the HUB cannot be reached without a SPLIT */
    USBHSH_SetSelfAddr( 0x00 );
    s = USBHSH_HubSplitTransact( 0x0000, ( USB_PID_IN << 4 ) | 0x00, 0x00, 0, PROBE_TIMEOUT );
    printf( "probe: ref IN device addr 0 (no split) INT_ST=%02x\r\n", s );

    /* Diagnostic: is the "SPLIT packet can be transmitted" status ever reported? */
    {
        uint16_t c1 = 0, c0 = 0, k;

        for( k = 0; k < 2000; k++ )
        {
            if( USBHSH->MIS_ST & USBHS_UMS_SPLIT_CAN )
            {
                c1++;
            }
            else
            {
                c0++;
            }
            Delay_Us( 20 );
        }
        printf( "probe: SPLIT_CAN sampling (~40ms): ready=%u busy=%u MIS_ST=%02x\r\n", c1, c0, USBHSH->MIS_ST );
    }

    /* Sweep the SPLIT packet content. R8_USB_DEV_AD holds either the address of the target device
     * (mode 0) or the HUB address (mode 1), see the note of R16_USB_DEV_AD in the reference
     * manual: "in host mode it is the address or HUB address of the currently operating device". */
    for( addr_mode = 0; addr_mode < 2; addr_mode++ )
    {
        USBHSH_SetSelfAddr( addr_mode ? hub_addr : 0x00 );
        printf( "SPLIT sweep: DEV_AD=%02x\r\n", addr_mode ? hub_addr : 0x00 );

        for( ph_mode = 0; ph_mode < 4; ph_mode++ )
        {
            hits = 0;

            for( split_val = 0; split_val < PROBE_SPLIT_MAX; split_val++ )
            {
                ProbeTrySplit( (uint16_t)split_val, ProbePhase[ ph_mode ][ 0 ], ProbePhase[ ph_mode ][ 1 ], &pr );

                if( ( ( pr.res_ss != PROBE_NO_TRANSFER ) && ( ( pr.res_ss & 0x0F ) != 0 ) ) ||
                    ( ( pr.res_cs != PROBE_NO_TRANSFER ) && ( ( pr.res_cs & 0x0F ) != 0 ) ) ||
                    ( ( pr.res_in != PROBE_NO_TRANSFER ) && ( ( pr.res_in & 0x0F ) != 0 ) ) ||
                    ( ( pr.res_cin != PROBE_NO_TRANSFER ) && ( ( pr.res_cin & 0x0F ) != 0 ) ) )
                {
                    hits++;
                    printf( "SPLITA a=%u v=%03x ph=%u%u ss=%02x cs=%02x in=%02x cin=%02x len=%u\r\n",
                            addr_mode, split_val, ProbePhase[ ph_mode ][ 0 ], ProbePhase[ ph_mode ][ 1 ],
                            pr.res_ss, pr.res_cs, pr.res_in, pr.res_cin, pr.rx_len );
                }

                if( pr.desc_ok )
                {
                    /* The HUB itself also answers with its device descriptor: it has to be
                     * excluded, only the descriptor of the device behind the HUB is a success. */
                    if( memcmp( ProbeDescBuf, DevDesc_Buf, 8 ) == 0 )
                    {
                        hub_desc_hits++;
                    }
                    else
                    {
                        printf( "SPLIT TARGET a=%u v=%03x ph=%u%u DESC:", addr_mode, split_val,
                                ProbePhase[ ph_mode ][ 0 ], ProbePhase[ ph_mode ][ 1 ] );
                        for( i = 0; i < pr.rx_len; i++ )
                        {
                            printf( " %02x", ProbeDescBuf[ i ] );
                        }
                        printf( "\r\n" );
                        found = 1;
                        break;
                    }
                }

                if( ( split_val & 0x03FF ) == 0x03FF )
                {
                    printf( "SPLIT progress a=%u ph=%u%u v=%03x hits=%u\r\n", addr_mode,
                            ProbePhase[ ph_mode ][ 0 ], ProbePhase[ ph_mode ][ 1 ], split_val, hits );
                }
            }

            if( found != 0 )
            {
                break;
            }
            printf( "SPLIT sweep a=%u ph=%u%u done, hits=%u\r\n", addr_mode,
                    ProbePhase[ ph_mode ][ 0 ], ProbePhase[ ph_mode ][ 1 ], hits );
        }

        if( found != 0 )
        {
            break;
        }
    }

    /* Token space: a lone SPLIT (PID 0x8) or PING (PID 0x4) token carrying the SPLIT payload */
    for( tok_mode = 0; tok_mode < 2; tok_mode++ )
    {
        uint8_t tok_pid = (uint8_t)( tok_mode ? 0x04 : 0x08 );

        for( dn = 0; dn < 2; dn++ )
        {
            for( addr_mode = 0; addr_mode < 2; addr_mode++ )
            {
                USBHSH_SetSelfAddr( addr_mode ? hub_addr : 0x00 );
                hits = 0;

                for( split_val = 0; split_val < PROBE_SPLIT_MAX; split_val++ )
                {
                    s = USBHSH_HubSplitTransact( (uint16_t)split_val, (uint8_t)( tok_pid << 4 ), 0x00, dn, PROBE_TIMEOUT );
                    if( ( s != PROBE_NO_TRANSFER ) && ( ( s & 0x0F ) != 0 ) )
                    {
                        hits++;
                        if( hits <= 24 )
                        {
                            printf( "SPLITT tok=%02x dn=%u a=%u v=%03x INT_ST=%02x\r\n", tok_pid, dn, addr_mode, split_val, s );
                        }
                    }
                }

                printf( "SPLITT tok=%02x dn=%u a=%u done hits=%u\r\n", tok_pid, dn, addr_mode, hits );
            }
        }
    }

    USBHSH->HOST_SPLIT_DATA = 0;
    printf( "probe: SPLIT_CAN gate statistics: ready=%u busy(timeout)=%u, HUB descriptor hits=%u\r\n",
            ProbeCanReady, ProbeCanBusy, hub_desc_hits );
    if( found == 0 )
    {
        printf( "SPLIT: no encoding found in this pass\r\n" );
    }
    printf( "=== probe done ===\r\n\r\n" );
}

#endif