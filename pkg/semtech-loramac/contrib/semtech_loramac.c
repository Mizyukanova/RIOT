/*
 * Copyright (C) 2017 Inria
 *               2017 Inria Chile
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @{
 * @ingroup     pkg_semtech-loramac
 *
 * @file
 * @brief       Implementation of public API for Semtech LoRaMAC
 *
 * This implementation is an adaption of the applications provided on the
 * Semtech Lora-net repository.
 *
 * The LoRaMAC stack and the SX127x driver run in their own thread and simple
 * IPC messages are exchanged to control the MAC.
 *
 * @author      Alexandre Abadie <alexandre.abadie@inria.fr>
 * @author      Jose Alamos <jose.alamos@inria.cl>
 * @}
 */

#include <string.h>

#include "msg.h"
#include "mutex.h"

#include "net/netdev.h"
#include "net/loramac.h"

#include "sx127x.h"
#include "sx127x_params.h"
#include "sx127x_netdev.h"

#include "semtech_loramac.h"
#include "LoRaMac.h"
#include "region/Region.h"

#define ENABLE_DEBUG (0)
#include "debug.h"

#define LORAWAN_MAX_JOIN_RETRIES                    (3U)

#if defined(REGION_EU868)
#define LORAWAN_DUTYCYCLE_ON                        (true)
#define USE_SEMTECH_DEFAULT_CHANNEL_LINEUP          (1)

#if (USE_SEMTECH_DEFAULT_CHANNEL_LINEUP)
#define LC4                { 867100000, 0, { ( ( DR_5 << 4 ) | DR_0 ) }, 0 }
#define LC5                { 867300000, 0, { ( ( DR_5 << 4 ) | DR_0 ) }, 0 }
#define LC6                { 867500000, 0, { ( ( DR_5 << 4 ) | DR_0 ) }, 0 }
#define LC7                { 867700000, 0, { ( ( DR_5 << 4 ) | DR_0 ) }, 0 }
#define LC8                { 867900000, 0, { ( ( DR_5 << 4 ) | DR_0 ) }, 0 }
#define LC9                { 868800000, 0, { ( ( DR_7 << 4 ) | DR_7 ) }, 2 }
#define LC10               { 868300000, 0, { ( ( DR_6 << 4 ) | DR_6 ) }, 1 }
#endif /* USE_SEMTECH_DEFAULT_CHANNEL_LINEUP */
#endif /* REGION_EU868 */

#define SEMTECH_LORAMAC_MSG_QUEUE                   (16U)
#define SEMTECH_LORAMAC_LORAMAC_STACKSIZE           (THREAD_STACKSIZE_DEFAULT)
static msg_t _semtech_loramac_msg_queue[SEMTECH_LORAMAC_MSG_QUEUE];
static char _semtech_loramac_stack[SEMTECH_LORAMAC_LORAMAC_STACKSIZE];
kernel_pid_t semtech_loramac_pid;

sx127x_t sx127x;
RadioEvents_t semtech_loramac_radio_events;
LoRaMacPrimitives_t semtech_loramac_primitives;
LoRaMacCallback_t semtech_loramac_callbacks;

typedef struct {
    uint8_t *payload;
    uint8_t len;
} loramac_send_params_t;

typedef void (*semtech_loramac_func_t)(semtech_loramac_t *, void *);

/**
 * @brief   Struct containing a semtech loramac function call
 *
 * This function is called inside the semtech loramac thread context.
 */
typedef struct {
    semtech_loramac_func_t func;            /**< the function to call. */
    void *arg;                              /**< argument of the function **/
} semtech_loramac_call_t;

/* Prepares the payload of the frame */
static bool _semtech_loramac_send(semtech_loramac_t *mac,
                                  uint8_t *payload, uint8_t len)
{
    DEBUG("[semtech-loramac] send frame %s\n", (char *)payload);
    McpsReq_t mcpsReq;
    LoRaMacTxInfo_t txInfo;
    uint8_t dr = semtech_loramac_get_dr(mac);

    if (LoRaMacQueryTxPossible(len, &txInfo) != LORAMAC_STATUS_OK) {
        DEBUG("[semtech-loramac] empty frame in order to flush MAC commands\n");
        /* Send empty frame in order to flush MAC commands */
        mcpsReq.Type = MCPS_UNCONFIRMED;
        mcpsReq.Req.Unconfirmed.fBuffer = NULL;
        mcpsReq.Req.Unconfirmed.fBufferSize = 0;
        mcpsReq.Req.Unconfirmed.Datarate = (int8_t)dr;
    }
    else {
        if (mac->cnf == LORAMAC_TX_UNCNF) {
            DEBUG("[semtech-loramac] MCPS_UNCONFIRMED\n");
            mcpsReq.Type = MCPS_UNCONFIRMED;
            mcpsReq.Req.Unconfirmed.fPort = mac->port;
            mcpsReq.Req.Unconfirmed.fBuffer = payload;
            mcpsReq.Req.Unconfirmed.fBufferSize = len;
            mcpsReq.Req.Unconfirmed.Datarate = (int8_t)dr;
        }
        else {
            DEBUG("[semtech-loramac] MCPS_CONFIRMED\n");
            mcpsReq.Type = MCPS_CONFIRMED;
            mcpsReq.Req.Confirmed.fPort = mac->port;
            mcpsReq.Req.Confirmed.fBuffer = payload;
            mcpsReq.Req.Confirmed.fBufferSize = len;
            mcpsReq.Req.Confirmed.NbTrials = mac->trials;
            mcpsReq.Req.Confirmed.Datarate = (int8_t)dr;
        }
    }

    int ret = LoRaMacMcpsRequest(&mcpsReq);
    
    switch (ret) {
        case LORAMAC_STATUS_OK:
            DEBUG("[semtech-loramac] MCPS request OK\n");
            return false;
        case LORAMAC_STATUS_BUSY:
            DEBUG("[semtech-loramac] MCPS status BUSY\n");
            break;
        case LORAMAC_STATUS_DUTYCYCLE_RESTRICTED:
            DEBUG("[semtech-loramac] MCPS duty cycle restriction\n");
            break;
        default:
            DEBUG("[semtech-loramac] MCPS request error %d\n", ret);
            break;
    }
    
    mac->state = SEMTECH_LORAMAC_STATE_IDLE;
    
    return true;
}

/* MCPS-Confirm event function */
static void mcps_confirm(McpsConfirm_t *confirm)
{
    DEBUG("[semtech-loramac] MCPS confirm event\n");
    if (confirm->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
        DEBUG("[semtech-loramac] MCPS confirm event OK\n");

        switch (confirm->McpsRequest) {
            case MCPS_UNCONFIRMED:
            {
                /* Check Datarate
                   Check TxPower */
                DEBUG("[semtech-loramac] MCPS confirm event UNCONFIRMED\n");
                msg_t msg;
                msg.type = MSG_TYPE_LORAMAC_TX_DONE;
                msg_send(&msg, semtech_loramac_pid);
                break;
            }

            case MCPS_CONFIRMED:
                /* Check Datarate
                   Check TxPower
                   Check AckReceived */
                DEBUG("[semtech-loramac] MCPS confirm event CONFIRMED\n");
                break;

            case MCPS_PROPRIETARY:
                DEBUG("[semtech-loramac] MCPS confirm event PROPRIETARY\n");
                break;

            default:
                DEBUG("[semtech-loramac] MCPS confirm event UNKNOWN\n");
                break;
        }
    }
    else {
        msg_t msg;
        msg.type = MSG_TYPE_LORAMAC_TX_CNF_FAILED;
        msg_send(&msg, semtech_loramac_pid);
    }
}

/* MCPS-Indication event function */
static void mcps_indication(McpsIndication_t *indication)
{
    DEBUG("[semtech-loramac] MCPS indication event\n");
    if (indication->Status != LORAMAC_EVENT_INFO_STATUS_OK) {
        DEBUG("[semtech-loramac] MCPS indication no OK\n");
        return;
    }

    if (ENABLE_DEBUG) {
        switch (indication->McpsIndication) {
            case MCPS_UNCONFIRMED:
                DEBUG("[semtech-loramac] MCPS indication Unconfirmed\n");
                break;

            case MCPS_CONFIRMED:
                DEBUG("[semtech-loramac] MCPS indication Confirmed\n");
                break;

            case MCPS_PROPRIETARY:
                DEBUG("[semtech-loramac] MCPS indication Proprietary\n");
                break;

            case MCPS_MULTICAST:
                DEBUG("[semtech-loramac] MCPS indication Multicast\n");
                break;

            default:
                break;
        }
    }
    
    /* Check Multicast
       Check Port
       Check Datarate
       Check FramePending */
    if (indication->FramePending == true) {
        /* The server signals that it has pending data to be sent.
           We schedule an uplink as soon as possible to flush the server. */
        DEBUG("[semtech-loramac] MCPS indication: pending data, schedule an "
              "uplink\n");
        msg_t msg;
        msg.type = MSG_TYPE_LORAMAC_TX_SCHEDULE;
        msg_send(&msg, semtech_loramac_pid);
    }

    msg_t msg;
    if (indication->RxData) {
        DEBUG("[semtech-loramac] MCPS indication: data received\n");
        msg.type = MSG_TYPE_LORAMAC_RX;
        msg.content.ptr = indication;
    }
    else {
        msg.type = MSG_TYPE_LORAMAC_TX_DONE;
    }
    msg_send(&msg, semtech_loramac_pid);
}

/*MLME-Indication event function */
static void mlme_indication( MlmeIndication_t *mlmeIndication )
{
    switch (mlmeIndication->MlmeIndication) {
        case MLME_SCHEDULE_UPLINK:
            /* The MAC signals that we shall provide an uplink
               as soon as possible */
            DEBUG("[semtech-loramac] MLME indication: schedule an uplink\n");
            msg_t msg;
            msg.type = MSG_TYPE_LORAMAC_TX_SCHEDULE;
            msg_send(&msg, semtech_loramac_pid);
            break;
        default:
            break;
    }
}

/*MLME-Confirm event function */
static void mlme_confirm(MlmeConfirm_t *confirm)
{
    DEBUG("[semtech-loramac] MLME confirm event\n");
    switch (confirm->MlmeRequest) {
        case MLME_JOIN:
            if (confirm->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
                /* Status is OK, node has joined the network */
                DEBUG("[semtech-loramac] join succeeded\n");
                msg_t msg;
                msg.type = MSG_TYPE_LORAMAC_JOIN;
                msg.content.value = SEMTECH_LORAMAC_JOIN_SUCCEEDED;
                msg_send(&msg, semtech_loramac_pid);
            }
            else {
                DEBUG("[semtech-loramac] join not successful\n");
                /* Join was not successful. */
                msg_t msg;
                msg.type = MSG_TYPE_LORAMAC_JOIN;
                msg.content.value = SEMTECH_LORAMAC_JOIN_FAILED;
                msg_send(&msg, semtech_loramac_pid);
            }
            break;

        case MLME_LINK_CHECK:
            if (confirm->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
                DEBUG("[semtech-loramac] link check received\n");
                msg_t msg;
                msg.type = MSG_TYPE_LORAMAC_LINK_CHECK;
                msg.content.ptr = confirm;
                msg_send(&msg, semtech_loramac_pid);
            }

        default:
            break;
    }
}

void _init_loramac(semtech_loramac_t *mac,
                   LoRaMacPrimitives_t * primitives, LoRaMacCallback_t *callbacks)
{
    mutex_lock(&mac->lock);
    DEBUG("[semtech-loramac] initializing loramac\n");
    primitives->MacMcpsConfirm = mcps_confirm;
    primitives->MacMcpsIndication = mcps_indication;
    primitives->MacMlmeConfirm = mlme_confirm;
    primitives->MacMlmeIndication = mlme_indication;
    
    int result = LoRaMacInitialization(&semtech_loramac_radio_events,
                                        primitives, callbacks, LORAMAC_ACTIVE_REGION);
    
    if (result != LORAMAC_STATUS_OK) {
        DEBUG("[semtech-loramac] initialization failed with code %d\n", result);
    }

    mutex_unlock(&mac->lock);

    semtech_loramac_set_dr(mac, LORAMAC_DEFAULT_DR);
    semtech_loramac_set_adr(mac, LORAMAC_DEFAULT_ADR);
    semtech_loramac_set_public_network(mac, LORAMAC_DEFAULT_PUBLIC_NETWORK);
    semtech_loramac_set_class(mac, LORAMAC_DEFAULT_DEVICE_CLASS);
    semtech_loramac_set_tx_port(mac, LORAMAC_DEFAULT_TX_PORT);
    semtech_loramac_set_tx_mode(mac, LORAMAC_DEFAULT_TX_MODE);
    mac->link_chk.available = false;
}

static void _join_otaa(semtech_loramac_t *mac)
{
    DEBUG("[semtech-loramac] starting OTAA join\n");

    mutex_lock(&mac->lock);
    MibRequestConfirm_t mibReq;
    mibReq.Type = MIB_NETWORK_JOINED;
    mibReq.Param.IsNetworkJoined = false;
    LoRaMacMibSetRequestConfirm(&mibReq);

    MlmeReq_t mlmeReq;
    mlmeReq.Type = MLME_JOIN;
    mlmeReq.Req.Join.DevEui = mac->deveui;
    mlmeReq.Req.Join.AppEui = mac->appeui;
    mlmeReq.Req.Join.AppKey = mac->appkey;
    mlmeReq.Req.Join.Datarate = mac->datarate;
    uint8_t ret = LoRaMacMlmeRequest(&mlmeReq);
    
    switch(ret) {
        case LORAMAC_STATUS_OK:
            mutex_unlock(&mac->lock);
            return;
        case LORAMAC_STATUS_DUTYCYCLE_RESTRICTED:
        {
            mutex_unlock(&mac->lock);
            DEBUG("[semtech-loramac] Duty cycle restricted\n");
            /* Cannot join. */
            msg_t msg;
            msg.type = MSG_TYPE_LORAMAC_JOIN;
            msg.content.value = SEMTECH_LORAMAC_RESTRICTED;
            msg_send(&msg, semtech_loramac_pid);
            return;
        }
        default:
        {
            mutex_unlock(&mac->lock);
            DEBUG("[semtech-loramac] join not successful: %d\n", ret);
            /* Cannot join. */
            msg_t msg;
            msg.type = MSG_TYPE_LORAMAC_JOIN;
            msg.content.value = SEMTECH_LORAMAC_JOIN_FAILED;
            msg_send(&msg, semtech_loramac_pid);
            return;
        }
    }
}

static void _join_abp(semtech_loramac_t *mac)
{
    DEBUG("[semtech-loramac] starting ABP join\n");

    semtech_loramac_set_netid(mac, LORAMAC_DEFAULT_NETID);

    mutex_lock(&mac->lock);
    MibRequestConfirm_t mibReq;
    mibReq.Type = MIB_NETWORK_JOINED;
    mibReq.Param.IsNetworkJoined = false;
    LoRaMacMibSetRequestConfirm(&mibReq);

    mibReq.Type = MIB_DEV_ADDR;
    mibReq.Param.DevAddr = ((uint32_t)mac->devaddr[0] << 24 |
                            (uint32_t)mac->devaddr[1] << 16 |
                            (uint32_t)mac->devaddr[2] << 8 |
                            (uint32_t)mac->devaddr[3]);
    LoRaMacMibSetRequestConfirm(&mibReq);

    mibReq.Type = MIB_NWK_SKEY;
    mibReq.Param.NwkSKey = mac->nwkskey;
    LoRaMacMibSetRequestConfirm(&mibReq);

    mibReq.Type = MIB_APP_SKEY;
    mibReq.Param.AppSKey = mac->appskey;
    LoRaMacMibSetRequestConfirm(&mibReq);

    mibReq.Type = MIB_NETWORK_JOINED;
    mibReq.Param.IsNetworkJoined = true;
    LoRaMacMibSetRequestConfirm(&mibReq);

    /* switch back to idle state now*/
    mac->state = SEMTECH_LORAMAC_STATE_IDLE;
    mutex_unlock(&mac->lock);
}

static void _join(semtech_loramac_t *mac, void *arg)
{
    uint8_t join_type = *(uint8_t *)arg;

    switch (join_type) {
        case LORAMAC_JOIN_OTAA:
            _join_otaa(mac);
            break;

        case LORAMAC_JOIN_ABP:
            _join_abp(mac);
            break;
    }
}

static void _send(semtech_loramac_t *mac, void *arg)
{
    loramac_send_params_t params = *(loramac_send_params_t *)arg;
    _semtech_loramac_send(mac, params.payload, params.len);
}

static void _semtech_loramac_call(semtech_loramac_func_t func, void *arg)
{
    semtech_loramac_call_t call;
    call.func = func;
    call.arg = arg;

    msg_t msg, msg_resp;
    msg.type = MSG_TYPE_LORAMAC_CMD;
    msg.content.ptr = &call;
    msg_send_receive(&msg, &msg_resp, semtech_loramac_pid);
}

static void _semtech_loramac_event_cb(netdev_t *dev, netdev_event_t event, void *arg)
{
    (void) arg;
    netdev_sx127x_lora_packet_info_t packet_info;

    msg_t msg;
    msg.content.ptr = dev;

    switch (event) {
        case NETDEV_EVENT_ISR:
            msg.type = MSG_TYPE_ISR;
            if (msg_send(&msg, semtech_loramac_pid) <= 0) {
                DEBUG("[semtech-loramac] possibly lost interrupt.\n");
            }
            break;

        case NETDEV_EVENT_TX_COMPLETE:
            sx127x_set_sleep((sx127x_t *)dev);
            semtech_loramac_radio_events.TxDone();
            DEBUG("[semtech-loramac] Transmission completed\n");
            break;

        case NETDEV_EVENT_TX_TIMEOUT:
            msg.type = MSG_TYPE_TX_TIMEOUT;
            if (msg_send(&msg, semtech_loramac_pid) <= 0) {
                DEBUG("[semtech-loramac] TX timeout, possibly lost interrupt.\n");
            }
            break;

        case NETDEV_EVENT_RX_COMPLETE:
        {
            size_t len;
            uint8_t radio_payload[SX127X_RX_BUFFER_SIZE];
            len = dev->driver->recv(dev, NULL, 0, 0);
            dev->driver->recv(dev, radio_payload, len, &packet_info);
            semtech_loramac_radio_events.RxDone(radio_payload,
                                                len, packet_info.rssi,
                                                packet_info.snr);
            break;
        }
        case NETDEV_EVENT_RX_TIMEOUT:
            msg.type = MSG_TYPE_RX_TIMEOUT;
            if (msg_send(&msg, semtech_loramac_pid) <= 0) {
                DEBUG("[semtech-loramac] RX timeout, possibly lost interrupt.\n");
            }
            break;

        case NETDEV_EVENT_CRC_ERROR:
            DEBUG("[semtech-loramac] RX CRC error\n");
            semtech_loramac_radio_events.RxError();
            break;

        case NETDEV_EVENT_FHSS_CHANGE_CHANNEL:
            DEBUG("[semtech-loramac] FHSS channel change\n");
            semtech_loramac_radio_events.FhssChangeChannel(((sx127x_t *)dev)->_internal.last_channel);
            break;

        case NETDEV_EVENT_CAD_DONE:
            DEBUG("[semtech-loramac] test: CAD done\n");
            semtech_loramac_radio_events.CadDone(((sx127x_t *)dev)->_internal.is_last_cad_success);
            break;
            
        case NETDEV_EVENT_CAD_DETECTED:
            DEBUG("[semtech-loramac] CAD detected\n");
            break;
            
        case NETDEV_EVENT_VALID_HEADER:
            DEBUG("[semtech-loramac] valid header received");
            break;

        default:
            DEBUG("[semtech-loramac] unexpected netdev event received: %d\n",
                  event);
    }
}

void *_semtech_loramac_event_loop(void *arg)
{
    msg_init_queue(_semtech_loramac_msg_queue, SEMTECH_LORAMAC_MSG_QUEUE);
    semtech_loramac_t *mac = (semtech_loramac_t *)arg;

    while (1) {
        msg_t msg;
        msg_receive(&msg);
        if (msg.type == MSG_TYPE_ISR) {
            netdev_t *dev = msg.content.ptr;
            dev->driver->isr(dev);
        }
        else {
            switch (msg.type) {
                case MSG_TYPE_RX_TIMEOUT:
                    DEBUG("[semtech-loramac] RX timer timeout\n");
                    semtech_loramac_radio_events.RxTimeout();
                    break;

                case MSG_TYPE_TX_TIMEOUT:
                    DEBUG("[semtech-loramac] TX timer timeout\n");
                    semtech_loramac_radio_events.TxTimeout();
                    break;

                case MSG_TYPE_MAC_TIMEOUT:
                {
                    DEBUG("%lu - [semtech-loramac] MAC timer timeout\n", rtctimers_millis_now());
                    void (*callback)(void) = msg.content.ptr;
                    callback();
                    break;
                }
                case MSG_TYPE_LORAMAC_CMD:
                {
                    msg_t msg_resp;
                    DEBUG("[semtech-loramac] loramac cmd\n");
                    mac->state = SEMTECH_LORAMAC_STATE_BUSY;
                    semtech_loramac_call_t *call = msg.content.ptr;
                    call->func(mac, call->arg);
                    msg_reply(&msg, &msg_resp);
                    break;
                }
                case MSG_TYPE_LORAMAC_JOIN:
                {
                    DEBUG("[semtech-loramac] loramac join notification\n");
                    msg_t msg_ret;
                    msg_ret.content.value = msg.content.value;
                    msg_send(&msg_ret, mac->caller_pid);
                    /* switch back to idle state now*/
                    mac->state = SEMTECH_LORAMAC_STATE_IDLE;
                    break;
                }
                case MSG_TYPE_LORAMAC_LINK_CHECK:
                {
                    MlmeConfirm_t *confirm = (MlmeConfirm_t *)msg.content.ptr;
                    mac->link_chk.demod_margin = confirm->DemodMargin;
                    mac->link_chk.nb_gateways = confirm->NbGateways;
                    mac->link_chk.available = true;
                    DEBUG("[semtech-loramac] link check info received:\n"
                          "  - Demodulation marging: %d\n"
                          "  - Number of gateways: %d\n",
                          mac->link_chk.demod_margin,
                          mac->link_chk.nb_gateways);
                    break;
                }
                case MSG_TYPE_LORAMAC_TX_DONE:
                {
                    DEBUG("[semtech-loramac] loramac TX done\n");
                    msg_t msg_ret;
                    msg_ret.type = MSG_TYPE_LORAMAC_TX_DONE;
                    msg_send(&msg_ret, mac->caller_pid);
                    /* switch back to idle state now*/
                    mac->state = SEMTECH_LORAMAC_STATE_IDLE;
                    break;
                }
                case MSG_TYPE_LORAMAC_TX_SCHEDULE:
                {
                    DEBUG("[semtech-loramac] schedule immediate TX\n");
                    uint8_t prev_port = mac->port;
                    mac->port = 0;
                    _semtech_loramac_send(mac, NULL, 0);
                    mac->port = prev_port;
                    break;
                }
                case MSG_TYPE_LORAMAC_TX_CNF_FAILED:
                    DEBUG("[semtech-loramac] loramac TX failed\n");
                    msg_t msg_ret;
                    msg_ret.type = MSG_TYPE_LORAMAC_TX_CNF_FAILED;
                    msg_send(&msg_ret, mac->caller_pid);
                    /* switch back to idle state now*/
                    mac->state = SEMTECH_LORAMAC_STATE_IDLE;
                    break;
                case MSG_TYPE_LORAMAC_RX:
                {
                    msg_t msg_ret;
                    msg_ret.type = MSG_TYPE_LORAMAC_RX;
                    McpsIndication_t *indication = (McpsIndication_t *)msg.content.ptr;
                    memcpy(mac->rx_data.payload,
                           indication->Buffer, indication->BufferSize);
                    mac->rx_data.payload_len = indication->BufferSize;
                    mac->rx_data.port = indication->Port;
                    mac->rx_data.ack = indication->AckReceived;
                    mac->rx_data.multicast = indication->Multicast;
                    mac->rx_data.rssi = indication->Rssi;
                    mac->rx_data.datarate = indication->RxDatarate;
                    
                    DEBUG("[semtech-loramac] loramac RX data:\n"
                          "  - Type: %s\n"
                          "  - Size: %d\n"
                          "  - Port: %d\n"
                          "  - RSSI: %d\n"
                          "  - DR:   %d\n",
                          (mac->rx_data.ack)? "ACK" : "Data",
                          mac->rx_data.payload_len,
                          mac->rx_data.port,
                          mac->rx_data.rssi,
                          mac->rx_data.datarate);
                    msg_send(&msg_ret, mac->caller_pid);
                    /* switch back to idle state now*/
                    mac->state = SEMTECH_LORAMAC_STATE_IDLE;
                    break;
                }
                default:
                    DEBUG("[semtech-loramac] Unexpected msg type '%04x'\n",
                          msg.type);
            }
        }
    }
}

int semtech_loramac_init(semtech_loramac_t *mac, sx127x_params_t *params)
{
    sx127x_setup(&sx127x, params);
    sx127x.netdev.driver = &sx127x_driver;
    sx127x.netdev.event_callback = _semtech_loramac_event_cb;

    semtech_loramac_pid = thread_create(_semtech_loramac_stack,
                                        sizeof(_semtech_loramac_stack),
                                        THREAD_PRIORITY_MAIN - 1,
                                        THREAD_CREATE_STACKTEST,
                                        _semtech_loramac_event_loop, mac,
                                        "LoRaMAC stack");

    if (semtech_loramac_pid > KERNEL_PID_UNDEF) {
        _init_loramac(mac, &semtech_loramac_primitives, &semtech_loramac_callbacks);
    }
    
    return semtech_loramac_pid;
}

uint8_t semtech_loramac_join(semtech_loramac_t *mac, uint8_t type)
{
    DEBUG("Starting join procedure: %d\n", type);

    if (mac->state != SEMTECH_LORAMAC_STATE_IDLE) {
        DEBUG("[semtech-loramac] internal mac is busy\n");
        return SEMTECH_LORAMAC_BUSY;
    }

    mac->caller_pid = thread_getpid();

    _semtech_loramac_call(_join, &type);

    if (type == LORAMAC_JOIN_OTAA) {
        /* Wait until the OTAA join procedure is complete */
        msg_t msg;
        msg_receive(&msg);
        mac->state = SEMTECH_LORAMAC_STATE_IDLE;
        return (uint8_t)msg.content.value;
    }

    /* ABP join procedure always works */
    return SEMTECH_LORAMAC_JOIN_SUCCEEDED;
}

void semtech_loramac_request_link_check(semtech_loramac_t *mac)
{
    mutex_lock(&mac->lock);
    mac->link_chk.available = false;
    MlmeReq_t mlmeReq;
    mlmeReq.Type = MLME_LINK_CHECK;
    LoRaMacMlmeRequest(&mlmeReq);
    mutex_unlock(&mac->lock);
}

uint8_t semtech_loramac_send(semtech_loramac_t *mac, uint8_t *data, uint8_t len)
{
    mutex_lock(&mac->lock);
    MibRequestConfirm_t mibReq;
    mibReq.Type = MIB_NETWORK_JOINED;
    LoRaMacMibGetRequestConfirm(&mibReq);
    bool is_joined = mibReq.Param.IsNetworkJoined;
    mac->link_chk.available = false;
    mutex_unlock(&mac->lock);

    if (!is_joined) {
        DEBUG("[semtech-loramac] network is not joined\n");
        return SEMTECH_LORAMAC_NOT_JOINED;
    }

    if (mac->state != SEMTECH_LORAMAC_STATE_IDLE) {
        DEBUG("[semtech-loramac] internal mac is busy\n");
        return SEMTECH_LORAMAC_BUSY;
    }

    loramac_send_params_t params;
    params.payload = data;
    params.len = len;

    _semtech_loramac_call(_send, &params);

    return SEMTECH_LORAMAC_TX_SCHEDULED;
}

uint8_t semtech_loramac_recv(semtech_loramac_t *mac)
{
    mac->caller_pid = thread_getpid();

    /* Wait until the mac receive some information */
    msg_t msg;
    msg_receive(&msg);
    uint8_t ret;
    switch (msg.type) {
        case MSG_TYPE_LORAMAC_RX:
            ret = SEMTECH_LORAMAC_DATA_RECEIVED;
            break;
        case MSG_TYPE_LORAMAC_TX_CNF_FAILED:
            ret = SEMTECH_LORAMAC_TX_CNF_FAILED;
            break;
        default:
            ret = SEMTECH_LORAMAC_TX_DONE;
            break;
     }

    DEBUG("[semtech-loramac] MAC reply received: %d\n", ret);

    return ret;
}
