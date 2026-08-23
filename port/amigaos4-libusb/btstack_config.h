//
// btstack_config.h for AmigaOS 4 libusb port
//

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// Port related features
#define HAVE_ASSERT
#define HAVE_BTSTACK_STDIN
#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

// BTstack features that can be enabled
#define ENABLE_ATT_DELAYED_RESPONSE
#define ENABLE_AVRCP_COVER_ART
#define ENABLE_BLE
#define ENABLE_BTSTACK_STDIN_LOGGING
#define ENABLE_CLASSIC
#define ENABLE_CROSS_TRANSPORT_KEY_DERIVATION
#define ENABLE_GOEP_L2CAP

/*
 * Room for GOEP's ERTM channel to be set up at all.
 *
 * l2cap_ertm_setup_buffers() lays out one MTU for reassembly, then four receive
 * and four transmit buffers of one MPS each, and asserts that they fit. The
 * GOEP server asks for an MTU of half this value and four buffers each way, so
 * with the 2000 it defaults to the layout needs about nine thousand bytes and
 * gets two - the assert fires, or with asserts compiled out the channel is
 * quietly never opened, which is what an accepted connection that never opens
 * turned out to be.
 *
 * That mattered more than a stalled bearer. Single Response Mode is a GOEP 2.0
 * feature and GOEP 2.0 is OBEX over L2CAP: without this channel there is no
 * SRM, and without SRM every OBEX packet costs a round trip - which is what
 * held file transfers to seventeen packets a second.
 *
 * The number comes from the packet log rather than a guess. The negotiated MPS
 * is 1238, and GOEP asks for an MTU of half the buffer with four buffers each
 * way, so the layout needs buffer/2 + 8 * 1238. At 20000 that is 19904 of
 * 20000, leaving 96 bytes for the eight packet state structures - which do not
 * fit, and the channel was closed during configuration. 28000 leaves four
 * thousand spare.
 */
#define GOEP_SERVER_ERTM_BUFFER 28000
#define ENABLE_HFP_WIDE_BAND_SPEECH
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define ENABLE_LE_EXTENDED_ADVERTISING
#define ENABLE_LE_ISOCHRONOUS_STREAMS
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION
#define ENABLE_LE_SECURE_CONNECTIONS
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS
#define ENABLE_MODPLAYER
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_PRINTF_TO_LOG
// NOTE: not enabled - hci_transport_h2_libusb.c does not handle the SCO endpoints
// (isochronous transfers / alternate settings) yet
// #define ENABLE_SCO_OVER_HCI
#define ENABLE_SDP_DES_DUMP
#define ENABLE_SOFTWARE_AES128

// BTstack configuration. buffers, sizes, ...
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14 // sizeof BNEP header, avoid memcpy

#define NVM_NUM_DEVICE_DB_ENTRIES      16
#define NVM_NUM_LINK_KEYS              16

// Mesh Configuration
#define ENABLE_MESH
#define ENABLE_MESH_ADV_BEARER
#define ENABLE_MESH_GATT_BEARER
#define ENABLE_MESH_PB_ADV
#define ENABLE_MESH_PB_GATT
#define ENABLE_MESH_PROVISIONER
#define ENABLE_MESH_PROXY_SERVER

#define MAX_NR_MESH_SUBNETS            2
#define MAX_NR_MESH_TRANSPORT_KEYS    16
#define MAX_NR_MESH_VIRTUAL_ADDRESSES 16

// allow for one NetKey update
#define MAX_NR_MESH_NETWORK_KEYS      (MAX_NR_MESH_SUBNETS+1)

#endif
