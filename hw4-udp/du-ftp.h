#pragma once

#include <stdint.h>

#define PROG_MD_CLI     0
#define PROG_MD_SVR     1
#define DEF_PORT_NO     2080
#define FNAME_SZ        150
#define PROG_DEF_FNAME  "test.c"
#define PROG_DEF_SVR_ADDR   "127.0.0.1"

#define FTP_STATUS_MSG_MAX   256

typedef enum {
    FTP_MSG_FILE_INFO = 1,
    FTP_MSG_DATA      = 2,
    FTP_MSG_STATUS    = 3,
    FTP_MSG_CLOSE     = 4
} ftp_msg_type;

typedef enum {
    FTP_STATUS_OK = 0,
    FTP_STATUS_ERR_OPEN_FILE,
    FTP_STATUS_ERR_WRITE,
    FTP_STATUS_ERR_PROTOCOL,
    FTP_STATUS_ERR_NOT_FOUND
} ftp_status_code;

typedef struct ftp_pdu {
    uint32_t    msg_type;
    uint32_t    status;
    uint32_t    payload_size;
    uint32_t    total_size;
    char        file_name[FNAME_SZ];
} ftp_pdu;

typedef struct prog_config{
    int     prog_mode;
    int     port_number;
    char    svr_ip_addr[16];
    char    file_name[128];
} prog_config;