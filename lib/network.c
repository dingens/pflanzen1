#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "checksum/crc16_ccitt.h"
#include "net/gnrc/rpl.h"
#include "net/sock/udp.h"
#include "net/ipv6/addr.h"

#include "global.c"
#include "util.c"
#include "pump.c"
#include "network.h"

typedef void (*h2op_receive_handler) (uint8_t* buf, size_t buflen);
typedef void (*h2op_receive_hook) (H2OP_MSGTYPE type, nodeid_t source,
                                   uint8_t* data, size_t len);
h2op_receive_hook H2OP_RECEIVE_HOOKS[H2OP_RECEIVE_HOOKS_NUMOF];

#define SERVER_MSG_QUEUE_SIZE (8)
#define SERVER_BUFFER_SIZE (H2OP_MAX_LENGTH)

int rv; // generic declaration for return value (use locally only)

// section: address handling

void h2op_nodeid_to_addr ( nodeid_t nodeid, ipv6_addr_t *addr) {
    switch(nodeid) {
        case 0: // reserved
            ipv6_addr_set_unspecified(addr);
            error(0, 0, "Invalid value for nodeid: 0000");
            break;
        case 0xffff: // special address meaning broadcast (ff02::1)
            ipv6_addr_set_all_nodes_multicast(addr, 2);
            break;
        default:
            addr->u64[0] = byteorder_htonll((uint64_t) H2O_NETWORK_PREFIX);
            addr->u64[1] = byteorder_htonll((uint64_t) nodeid);
            break;
    }
}

void add_public_address ( const gnrc_netif_t *netif ) {
    /* add the node's IPV6 address (deduced from NODE_ID).
     * @param netif: interface id. may be null, then the first interface is used.
     */
    gnrc_netif_t ifid; // netif is const, don't change it
    if ( netif == NULL ) {
        ifid = *gnrc_netif_iter(NULL);
    } else {
        ifid = *netif;
    }

    ipv6_addr_t addr;
    h2op_nodeid_to_addr(NODE_ID, &addr);
    rv = gnrc_netif_ipv6_addr_add(&ifid, &addr, IPV6_PREFIX_LENGTH,
                                  GNRC_NETIF_IPV6_ADDRS_FLAGS_STATE_VALID);
    if ( rv != sizeof(ipv6_addr_t) ) {
        error(0,-rv, "Cannot set address");
        printf("Tried adding address: "); fflush(stdout);
        ipv6_addr_print(&addr); putchar('\n');
    } else {
        printf("My public address is: "); fflush(stdout);
        ipv6_addr_print(&addr); putchar('\n');
    }
}

void network_init ( bool rpl_root ){
    rv = gnrc_rpl_init((*gnrc_netif_iter(NULL)).pid); // just use the first if
    if ( rv < 0 ) {
        error(-rv, 0, "Error while initializing RPL");
    } else if (PFLANZEN_DEBUG) {
        puts("RPL initialized.");
    }

    add_public_address(NULL);

    if ( !rpl_root )
        return;

    ipv6_addr_t myaddr;
    h2op_nodeid_to_addr(NODE_ID, &myaddr);

    rv = (int) gnrc_rpl_root_init(1, &myaddr, false, false);
    if ( rv == 0 ) {
        puts("Error while setting RPL root");
    } else if (PFLANZEN_DEBUG) {
        puts("RPL root set.");
    }
}


// section: h2op client

void h2op_header_hton ( H2OP_HEADER *header ) {
    header->node = htons(header->node);
}

ssize_t udp_send ( const ipv6_addr_t *recipient, uint16_t port,
                   const uint8_t *data, size_t data_len ) {
    /* send an udp datagram. */
    sock_udp_ep_t remote = {
        .family = AF_INET6,
        .port = port,
    };
    memcpy(&remote.addr, recipient, sizeof(*recipient));

    if (ipv6_addr_is_link_local((ipv6_addr_t *)&remote.addr)) {
        /* choose first interface when address is link local */
        gnrc_netif_t *netif = gnrc_netif_iter(NULL);
        remote.netif = (uint16_t)netif->pid;
    }

    ssize_t res = sock_udp_send(NULL, data, data_len, &remote);
    // TODO error handling
    return res;
}

ssize_t h2op_send ( const nodeid_t recipient, H2OP_MSGTYPE type,
                    const uint8_t *data, size_t len, uint16_t source ) {
    /* Send a H2OP message.
     * @param recipient: recipient, node id. 0xffff is broadcast.
     * @param type: message type, as per protocol definition
     * @param data: message data
     * @param len: length of data
     * @param source: node id of data source
     * @return: number of bytes sent, or errors like RIOT's `sock_udp_send`
     */
#pragma pack(push, 1)
    struct {
        H2OP_HEADER header;
        uint8_t data[H2OP_MAX_LENGTH-H2OP_HEADER_LENGTH];
    } buf;
#pragma pack(pop)
    size_t buflen = H2OP_HEADER_LENGTH + len;

    ipv6_addr_t recipient_ip;
    h2op_nodeid_to_addr(recipient, &recipient_ip);

    buf.header = (H2OP_HEADER) {
        .magic = H2OP_MAGIC,
        .version = H2OP_VERSION,
        .len = H2OP_HEADER_LENGTH+len,
        .type = type,
        .node = source,
        .crc = 0,
    };
    memcpy(buf.data, data, len);

    // calculate crc on what is actually sent over the network
    h2op_header_hton(&buf.header);

    buf.header.crc = htons(crc16_ccitt_calc((uint8_t*) &buf, buflen));

    rv = udp_send(&recipient_ip, H2OP_PORT, (uint8_t*) &buf, buflen);
    if (PFLANZEN_DEBUG) {
        printf("%d bytes sent to ", rv); fflush(stdout);
        ipv6_addr_print(&recipient_ip); putchar('\n');
    }
    return rv;
}

// section: h2op server part

void h2op_header_ntoh ( H2OP_HEADER *header ) {
    header->node = ntohs(header->node);
}

void udp_server(uint16_t port, h2op_receive_handler handler) {
    /* Create a UDP server that calls the receive_handler for each incoming message.
     *
     * @param port: the port to listen at
     * @param handler: the function to call. It must accept two parameters:
     * - a pointer to a memory area where the received data is stored.
     * - the length of this data.
     */
    static sock_udp_t sock;
    static uint8_t server_buffer[SERVER_BUFFER_SIZE];
    static msg_t server_msg_queue[SERVER_MSG_QUEUE_SIZE];

    sock_udp_ep_t server = { .port = port, .family = AF_INET6, };
    msg_init_queue(server_msg_queue, SERVER_MSG_QUEUE_SIZE);

    if((rv = sock_udp_create(&sock, &server, NULL, 0)) < 0) {
        error(-rv, 0, "Could not start UDP server");
        return;
    }

    printf("Server running on port %u.\n", port);

    while (true) {
        //TODO: store sender address
        rv = sock_udp_recv(&sock, server_buffer, sizeof(server_buffer),
                           SOCK_NO_TIMEOUT, NULL);
        if ( rv < 0 ) {
            error(0, -rv, "Error while receiving");
            return;
        } else if ( rv == 0 ) {
            puts("no data received");
        } else {
            handler(server_buffer, rv);
        }
    }
}

int h2op_preprocess_packet ( uint8_t *buf, size_t packetlen, uint8_t **data ) {
    /* Preprocess H2OP packet (convert byte order, verify header and checksum)
     * @pre: *buf is unmodified as received from the network
     * @post: on success, headers and checksum are valid,
     *                    *buf header fields are in host byte order, and
     *                    *data points to the start of data
     *        on error, an error message was printed to stderr.
     * @return: length of data on success, -EINVAL on error
     */

    if ( packetlen < H2OP_HEADER_LENGTH ) {
        error(0,0, "invalid packet received: length < %u", H2OP_HEADER_LENGTH);
        return -EINVAL;
    }
    H2OP_HEADER *header = (H2OP_HEADER*) buf;

    if ( header->magic != H2OP_MAGIC ) {
        error(0,0, "invalid magic number (got 0x%02X, want 0xAC)", header->magic);
        return -EINVAL;
    }
    if ( header->version != H2OP_VERSION ) {
        error(0,0, "unknown version (got 0x%02X, want 0x01)", header->version);
        return -EINVAL;
    }

    uint16_t crc_want = ntohs(header->crc);
    header->crc = 0;
    uint16_t crc_have = crc16_ccitt_calc(buf, packetlen);
    if ( crc_want != crc_have ) {
        error(0,0, "crc mismatch (got 0x%04X, calculated 0x%04X)",
              crc_want, crc_have);
        return -EINVAL;
    }
    h2op_header_ntoh(header);

    if ( packetlen < header->len ) {
        error(0,0, "packet length (%u) < length field in header (%u)",
              packetlen, header->len);
        return -EINVAL;
    }
    else if ( packetlen > header->len ) {
        printf("INFO: packet length (%u) > length field in header (%u)\n",
               packetlen, header->len);
    }

    if ( h2op_msgtype_string(header->type) == NULL ) {
        error(0,0, "invalid message type: %u\n", header->type);
        return -EINVAL;
    }

    *data = buf+H2OP_HEADER_LENGTH;
    return header->len;
}

void h2op_hooks_receive_handler ( uint8_t *buf, size_t packetlen ) {
    /* handler for `udp_server` that calls hooks defined by
     * `h2op_add_receive_hook` for each packet.
     */
    uint8_t *data = NULL;
    rv = h2op_preprocess_packet ( buf, packetlen, &data );
    if ( rv <= 0 ) {
        return;
    }
    H2OP_HEADER *header = (H2OP_HEADER*) buf;

    H2OP_MSGTYPE type = header->type;
    nodeid_t source = header->node;
    size_t datalen = (header->len) - H2OP_HEADER_LENGTH;
    for ( size_t i=0; i < H2OP_RECEIVE_HOOKS_NUMOF; i++ ) {
        if ( H2OP_RECEIVE_HOOKS[i] != NULL ) {
            (H2OP_RECEIVE_HOOKS[i])(type, source, data, datalen);
        }
    }
}

void h2op_del_receive_hook ( h2op_receive_hook func ) {
    /* Remove a receive hook set by `h2op_add_receive_hook`.
     * If the hook is not defined, do nothing.
     */
    for ( size_t i = 0; i < H2OP_RECEIVE_HOOKS_NUMOF; i++ ) {
        if ( H2OP_RECEIVE_HOOKS[i] == func ) {
            H2OP_RECEIVE_HOOKS[i] = NULL;
        }
    }
}

//TODO not thread safe
int h2op_add_receive_hook ( h2op_receive_hook func ) {
    /* Add a H2OP receive hook.
     * For each incoming H2OP packet, func() will be called once.
     *
     * Returns 1 on success, -ENOMEM if no space is available.
     * The number of possible hooks is defined by `H2OP_RECEIVE_HOOKS_NUMOF`.
     */

    // remove possible duplicates
    h2op_del_receive_hook(func);

    for ( size_t i = 0; i < H2OP_RECEIVE_HOOKS_NUMOF; i++ ) {
        if ( H2OP_RECEIVE_HOOKS[i] == NULL || H2OP_RECEIVE_HOOKS[i] == func ) {
            H2OP_RECEIVE_HOOKS[i] = func;
            return 1;
        }
    }
    return -ENOMEM;
}

void* h2od_thread ( void *arg ) {
    (void) arg;
    udp_server(H2OP_PORT, &h2op_hooks_receive_handler);
    return NULL;
}

void h2od_start ( void ) {
    if ( H2OD_THREAD_ID != KERNEL_PID_UNDEF ) {
        puts("Server already running.");
        return;
    }

    static char h2od_thread_stack[THREAD_STACKSIZE_MAIN];
    H2OD_THREAD_ID = thread_create(h2od_thread_stack, sizeof(h2od_thread_stack),
                                   THREAD_PRIORITY_H2OD, THREAD_CREATE_STACKTEST,
                                   h2od_thread, NULL, "h2od_thread");

    if ( H2OD_THREAD_ID < 0 ) {
        error(0, -H2OD_THREAD_ID, "Cannot create h2od thread");
        H2OD_THREAD_ID = KERNEL_PID_UNDEF;
    }
}

// section: interaction with other modules

void h2op_debug_hook (H2OP_MSGTYPE type, nodeid_t source,
                      uint8_t* data, size_t len) {
    if (!PFLANZEN_DEBUG)
        return;

    printf("H2OP packet received.  type: %s(0x%X)  source: %04x\n",
            h2op_msgtype_string(type), type, source);
    switch ( type ) {
        case H2OP_DATA_TEMPERATURE:
            if (len != 2) break;
            int16_t temp = ntohs(* (int16_t*) data);
            printf("Temperature: %hd\n", temp);
            return;
        case H2OP_DATA_HUMIDITY:
            if (len != 2) break;
            int16_t hum = ntohs(* (int16_t*) data);
            printf("Humidity: %hd\n", hum);
            return;
        case H2OP_CONF_MEASUREMENT_INTERVAL:
            if (len != 4) break;
            uint32_t interval = ntohl(* (uint32_t*) data);
            printf("Interval: %"PRIu32"\n", interval);
            return;
        default:
            // avoid "blabla not handled in switch" error
            break;
    }
    hexdump("Could not interpret packet contents", data, len);
}

// This can only be used when UPSTREAM_NODE is set.
#ifdef UPSTREAM_NODE
void h2op_forward_data_hook (H2OP_MSGTYPE type, nodeid_t source,
                             uint8_t* data, size_t len) {
    /* forward all data packets to the upstream node, towards the collector */

    if ( (type & H2OP_MSGSUPERTYPE_MASK) != H2OP_DATA ) {
        return;
    }

    if ( PFLANZEN_DEBUG ) {
        printf("Forwarding packet... ");
    }
    rv = h2op_send(UPSTREAM_NODE, type, data, len, source);
    if ( rv <= 0 ) {
        error(0,-rv,"Could not forward packet");
    }
}
#endif

void h2op_pump_set_data_hook (H2OP_MSGTYPE type, nodeid_t source,
                             uint8_t* data, size_t len) {
    if ( type != H2OP_DATA_HUMIDITY ) return;
    if ( len != 2 ) return;

    int16_t hum = ntohs(* (int16_t*) data);

    pump_set_data(source, hum);
}

void h2op_measurement_interval_hook (H2OP_MSGTYPE type, nodeid_t source,
                                     uint8_t* data, size_t len) {
    (void) source;

    if ( type != H2OP_CONF_MEASUREMENT_INTERVAL ) return;
    if ( len != 4 ) return;

    uint32_t interval = ntohl(* (uint32_t*) data);

    MEASUREMENT_INTERVAL = interval;
    if ( PFLANZEN_DEBUG ) {
        printf("Measurement interval set to %"PRIu32"\n", interval);
    }
}

// section: shell handlers

int h2o_send_data_shell ( int argc, char *argv[]) {
    if ( argc < 3 ) {
        printf("Usage: %s (temperature|humidity) VALUE [SOURCE|-] [TO]\n", argv[0]);
        printf("       SOURCE defaults to self, TO defaults to UPSTREAM_NODE\n");
        return 1;
    }

    H2OP_MSGTYPE type = H2OP_DATA_TEMPERATURE;
    if ( argv[1][0] == 't' ) {
        type = H2OP_DATA_TEMPERATURE;
    } else if ( argv[1][0] == 'h' ) {
        type = H2OP_DATA_HUMIDITY;
    } else {
        fprintf(stderr, "Invalid argument: %s (must be one of temperature, "
                "humidity)\n", argv[1]);
        fflush(stderr);
    }

    int16_t value = strtol(argv[2], NULL, 10);

    nodeid_t source;
    if ( argc < 4 || strcmp(argv[3], "-") == 0 ) {
        source = NODE_ID;
    } else {
        source = strtoul(argv[3], NULL, 16);
    }

    nodeid_t to;
    if ( argc < 5 || strcmp(argv[4], "-") == 0 ) {
#ifdef UPSTREAM_NODE
        to = UPSTREAM_NODE;
#else
        fprintf(stderr, "No upstream node set. TO cannot be empty.\n");
        return 1;
#endif
    } else {
        to = strtoul(argv[4], NULL, 16);
    }

    //XXX: why is there no network_int16_t in RIOT?
    int16_t netval = htons(value);

    rv = h2op_send(to, type, (uint8_t*) &netval, sizeof(value), source);
    if ( rv <= 0 ) {
        error(0,-rv, "could not send data");
        return 1;
    } else {
        return 0;
    }
}

int shell_h2od ( int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    h2od_start();
    return 0;
}

int h2o_set_measurement_interval_shell (int argc, char *argv[]) {
    if ( argc < 2 ) {
        printf("Usage: %s usecs [node]\n", argv[0]);
        printf("`node` defaults to current node. Use ffff for all nodes.\n");
        return 1;
    }

    int32_t interval = strtol(argv[1], NULL, 10);
    nodeid_t to = 0;
    if ( argc > 2 ) {
        to = strtoul(argv[2], NULL, 16);
    }
    if ( to == 0 ) {
        to = NODE_ID; // default: only this node
    }

    uint32_t netval = htonl(interval);
    rv = h2op_send(to, H2OP_CONF_MEASUREMENT_INTERVAL,
              (uint8_t*) &netval, sizeof(interval), NODE_ID);
    if ( rv <= 0 ) {
        error(0,-rv, "could not send data");
        return 1;
    } else {
        return 0;
    }
}
