#pragma once

#define IPV6_PREFIX_LENGTH (64)
#define H2OP_PORT (44555)
#define H2OP_MAGIC (0xAC)
#define H2OP_VERSION (1)
typedef enum {
    H2OP_DATA_TEMPERATURE = 0x11,
    H2OP_DATA_HUMIDITY = 0x12,
    H2OP_WARN_BUCKET_EMPTY = 0x99,
    H2OP_CONF_MEASUREMENT_INTERVAL = 0x31,
    H2OP_INFO_PUMP_STATUS = 0xA1,
} H2OP_MSGTYPE;
typedef enum {
    H2OP_DATA = 0x10,
    H2OP_CONF = 0x30,
    H2OP_WARN = 0x90,
    H2OP_INFO = 0xA0,
} H2OP_MSGSUPERTYPE;
#define H2OP_MSGSUPERTYPE_MASK (0xF0)
char* h2op_msgtype_string ( H2OP_MSGTYPE t ) {
    switch ( t ) {
        case H2OP_DATA_TEMPERATURE: return "H2OP_DATA_TEMPERATURE";
        case H2OP_DATA_HUMIDITY: return "H2OP_DATA_HUMIDITY";
        case H2OP_WARN_BUCKET_EMPTY: return "H2OP_WARN_BUCKET_EMPTY";
        case H2OP_CONF_MEASUREMENT_INTERVAL: return "H2OP_CONF_MEASUREMENT_INTERVAL";
        case H2OP_INFO_PUMP_STATUS: return "H2OP_INFO_PUMP_STATUS";
        default: return NULL;
    }
}
#pragma pack(push, 1)
typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t len;
    uint8_t type;
    nodeid_t node;
    uint16_t crc;
} H2OP_HEADER;
#pragma pack(pop)
#define H2OP_HEADER_LENGTH (sizeof(H2OP_HEADER))
#define H2OP_MAX_LENGTH (127) // 802.15.4's mtu
kernel_pid_t H2OD_THREAD_ID = KERNEL_PID_UNDEF;

ssize_t h2op_send ( const nodeid_t recipient, H2OP_MSGTYPE type,
                    const uint8_t *data, size_t len, uint16_t source );
