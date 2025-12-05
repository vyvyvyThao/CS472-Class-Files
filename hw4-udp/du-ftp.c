#include <stdlib.h>
#include <unistd.h> 
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <getopt.h>

#include "du-ftp.h"
#include "du-proto.h"


#define FTP_DATA_CHUNK   (DP_MAX_BUFF_SZ * 4)
#define FTP_MAX_MESSAGE  (sizeof(ftp_pdu) + FTP_DATA_CHUNK)
#define PATH_BUFF_SZ     512
#define CLIENT_DIR       "./outfile"
#define SERVER_DIR       "./infile"

static char sbuffer[FTP_MAX_MESSAGE];
static char rbuffer[FTP_MAX_MESSAGE];
static char full_file_path[PATH_BUFF_SZ];

static void build_full_path(char *dest, size_t dest_sz, const char *base_dir, const char *fname);
static long get_file_size(FILE *f);
static int send_status_pdu(dp_connp dpc, const char *file_name, ftp_status_code status,
    const char *message, size_t progress_bytes);
static int recv_status_pdu(dp_connp dpc, char *msgBuff, size_t msgBuffSz, size_t *progress_bytes);

/*
 *  Helper function that processes the command line arguements.  Highlights
 *  how to use a very useful utility called getopt, where you pass it a
 *  format string and it does all of the hard work for you.  The arg
 *  string basically states this program accepts a -p or -c flag, the
 *  -p flag is for a "pong message", in other words the server echos
 *  back what the client sends, and a -c message, the -c option takes
 *  a course id, and the server looks up the course id and responds
 *  with an appropriate message. 
 */
static int initParams(int argc, char *argv[], prog_config *cfg){
    int option;
    //setup defaults if no arguements are passed
    static char cmdBuffer[64] = {0};

    //setup defaults if no arguements are passed
    cfg->prog_mode = PROG_MD_CLI;
    cfg->port_number = DEF_PORT_NO;
    strcpy(cfg->file_name, PROG_DEF_FNAME);
    strcpy(cfg->svr_ip_addr, PROG_DEF_SVR_ADDR);
    
    while ((option = getopt(argc, argv, ":p:f:a:csh")) != -1){
        switch(option) {
            case 'p':
                strncpy(cmdBuffer, optarg, sizeof(cmdBuffer));
                cfg->port_number = atoi(cmdBuffer);
                break;
            case 'f':
                strncpy(cfg->file_name, optarg, sizeof(cfg->file_name));
                break;
            case 'a':
                strncpy(cfg->svr_ip_addr, optarg, sizeof(cfg->svr_ip_addr));
                break;
            case 'c':
                cfg->prog_mode = PROG_MD_CLI;
                break;
            case 's':
                cfg->prog_mode = PROG_MD_SVR;
                break;
            case 'h':
                printf("USAGE: %s [-p port] [-f fname] [-a svr_addr] [-s] [-c] [-h]\n", argv[0]);
                printf("WHERE:\n\t[-c] runs in client mode, [-s] runs in server mode; DEFAULT= client_mode\n");
                printf("\t[-a svr_addr] specifies the servers IP address as a string; DEFAULT = %s\n", cfg->svr_ip_addr);
                printf("\t[-p portnum] specifies the port number; DEFAULT = %d\n", cfg->port_number);
                printf("\t[-f fname] specifies the filename to send or recv; DEFAULT = %s\n", cfg->file_name);
                printf("\t[-p] displays what you are looking at now - the help\n\n");
                exit(0);
            case ':':
                perror ("Option missing value");
                exit(-1);
            default:
            case '?':
                perror ("Unknown option");
                exit(-1);
        }
    }
    return cfg->prog_mode;
}

static void build_full_path(char *dest, size_t dest_sz, const char *base_dir, const char *fname){
    if (dest == NULL || dest_sz == 0)
        return;

    const char *name = (fname != NULL && fname[0] != '\0') ? fname : PROG_DEF_FNAME;
    const char *dir = (base_dir != NULL) ? base_dir : ".";
    snprintf(dest, dest_sz, "%s/%s", dir, name);
}

static long get_file_size(FILE *f){
    if (f == NULL)
        return -1;

    long size = -1;
    long current = ftell(f);
    if (fseek(f, 0L, SEEK_END) == 0) {
        size = ftell(f);
        if (size < 0)
            size = -1;
    }
    if (fseek(f, (current >= 0) ? current : 0L, SEEK_SET) != 0)
        return -1;
    return size;
}

static int send_status_pdu(dp_connp dpc, const char *file_name, ftp_status_code status,
    const char *message, size_t progress_bytes){

    char status_buff[sizeof(ftp_pdu) + FTP_STATUS_MSG_MAX] = {0};
    ftp_pdu *pdu = (ftp_pdu *)status_buff;
    size_t msgLen = 0;
    if (message != NULL)
        msgLen = strnlen(message, FTP_STATUS_MSG_MAX);

    pdu->msg_type = FTP_MSG_STATUS;
    pdu->status = status;
    pdu->payload_size = msgLen;
    pdu->total_size = progress_bytes;
    if (file_name != NULL)
        strncpy(pdu->file_name, file_name, sizeof(pdu->file_name) - 1);

    if (msgLen > 0)
        memcpy(status_buff + sizeof(ftp_pdu), message, msgLen);

    return dpsend(dpc, status_buff, sizeof(ftp_pdu) + msgLen);
}

static int recv_status_pdu(dp_connp dpc, char *msgBuff, size_t msgBuffSz, size_t *progress_bytes){
    int rcvSz = dprecv(dpc, rbuffer, sizeof(rbuffer));
    if (rcvSz < 0)
        return rcvSz;
    if (rcvSz < (int)sizeof(ftp_pdu))
        return DP_ERROR_PROTOCOL;

    ftp_pdu *pdu = (ftp_pdu *)rbuffer;
    if (pdu->msg_type != FTP_MSG_STATUS)
        return DP_ERROR_PROTOCOL;

    int payloadSz = pdu->payload_size;
    if ((int)(sizeof(ftp_pdu) + payloadSz) > rcvSz)
        return DP_ERROR_PROTOCOL;

    if (progress_bytes != NULL)
        *progress_bytes = pdu->total_size;

    if (msgBuff != NULL && msgBuffSz > 0){
        size_t copySz = payloadSz;
        if (copySz >= msgBuffSz)
            copySz = msgBuffSz - 1;
        if (copySz > 0)
            memcpy(msgBuff, rbuffer + sizeof(ftp_pdu), copySz);
        msgBuff[copySz] = '\0';
    }

    return pdu->status;
}

int server_loop(dp_connp dpc, const prog_config *cfg){
    if (dpc->isConnected == false){
        perror("Expecting the protocol to be in connect state, but its not");
        return DP_ERROR_GENERAL;
    }
    //Loop until a disconnect is received, or error hapens
    FILE *f = NULL;
    size_t totalReceived = 0;
    size_t expectedBytes = 0;
    char activeFile[FNAME_SZ] = {0};
    while(1) {
        int rcvSz = dprecv(dpc, rbuffer, sizeof(rbuffer));
        if (rcvSz == DP_CONNECTION_CLOSED){
            if (f != NULL)
                fclose(f);
            printf("Client closed connection\n");
            return DP_CONNECTION_CLOSED;
        }
        if (rcvSz < 0) {
            if (f != NULL)
                fclose(f);
            return rcvSz;
        }
        if (rcvSz < (int)sizeof(ftp_pdu)) {
            send_status_pdu(dpc, activeFile, FTP_STATUS_ERR_PROTOCOL, "PDU too small", totalReceived);
            continue;
        }

        ftp_pdu *pdu = (ftp_pdu *)rbuffer;
        int payloadSz = pdu->payload_size;
        if ((int)(sizeof(ftp_pdu) + payloadSz) > rcvSz) {
            send_status_pdu(dpc, activeFile, FTP_STATUS_ERR_PROTOCOL, "Payload length mismatch", totalReceived);
            continue;
        }

        char *payload = rbuffer + sizeof(ftp_pdu);

        switch(pdu->msg_type) {
            case FTP_MSG_FILE_INFO: {
                if (f != NULL) {
                    fclose(f);
                    f = NULL;
                }
                memset(activeFile, 0, sizeof(activeFile));
                if (pdu->file_name[0] != '\0')
                    strncpy(activeFile, pdu->file_name, sizeof(activeFile) - 1);
                else if (cfg != NULL)
                    strncpy(activeFile, cfg->file_name, sizeof(activeFile) - 1);

                build_full_path(full_file_path, sizeof(full_file_path), SERVER_DIR,
                    (activeFile[0] != '\0') ? activeFile : PROG_DEF_FNAME);
                f = fopen(full_file_path, "wb");
                expectedBytes = pdu->total_size;
                totalReceived = 0;
                if (f == NULL) {
                    send_status_pdu(dpc, activeFile, FTP_STATUS_ERR_OPEN_FILE, "Unable to open file", totalReceived);
                } else {
                    send_status_pdu(dpc, activeFile, FTP_STATUS_OK, "Ready to receive", totalReceived);
                }
                break;
            }
            case FTP_MSG_DATA: {
                if (f == NULL) {
                    send_status_pdu(dpc, activeFile, FTP_STATUS_ERR_PROTOCOL, "Data received before FILE_INFO", totalReceived);
                    break;
                }
                size_t written = fwrite(payload, 1, payloadSz, f);
                if (written != (size_t)payloadSz) {
                    send_status_pdu(dpc, activeFile, FTP_STATUS_ERR_WRITE, "Disk write failure", totalReceived);
                    fclose(f);
                    f = NULL;
                    break;
                }
                totalReceived += written;
                char msg[64];
                if (expectedBytes > 0)
                    snprintf(msg, sizeof(msg), "%zu/%zu bytes", totalReceived, expectedBytes);
                else
                    snprintf(msg, sizeof(msg), "%zu bytes received", totalReceived);
                send_status_pdu(dpc, activeFile, FTP_STATUS_OK, msg, totalReceived);
                break;
            }
            case FTP_MSG_CLOSE:
                if (f != NULL) {
                    fclose(f);
                    f = NULL;
                }
                send_status_pdu(dpc, activeFile, FTP_STATUS_OK, "Transfer closed", totalReceived);
                break;
            case FTP_MSG_STATUS:
                // Server ignores status updates from the client
                break;
            default:
                send_status_pdu(dpc, activeFile, FTP_STATUS_ERR_PROTOCOL, "Unknown message type", totalReceived);
                break;
        }
    }
}



void start_client(dp_connp dpc, const prog_config *cfg){
    if(!dpc->isConnected) {
        printf("Client not connected\n");
        return;
    }

    build_full_path(full_file_path, sizeof(full_file_path), CLIENT_DIR, cfg->file_name);
    FILE *f = fopen(full_file_path, "rb");
    if(f == NULL){
        printf("ERROR:  Cannot open file %s\n", full_file_path);
        return;
    }
    if (dpc->isConnected == false){
        perror("Expecting the protocol to be in connect state, but its not");
        fclose(f);
        return;
    }

    long fileSz = get_file_size(f);
    if (fileSz < 0)
        fileSz = 0;

    ftp_pdu fileInfo = {0};
    fileInfo.msg_type = FTP_MSG_FILE_INFO;
    fileInfo.payload_size = 0;
    fileInfo.total_size = (uint32_t)fileSz;
    strncpy(fileInfo.file_name, cfg->file_name, sizeof(fileInfo.file_name) - 1);

    if (dpsend(dpc, &fileInfo, sizeof(fileInfo)) < 0){
        perror("Failed to send file info header");
        fclose(f);
        return;
    }

    char statusMsg[FTP_STATUS_MSG_MAX] = {0};
    int status = recv_status_pdu(dpc, statusMsg, sizeof(statusMsg), NULL);
    if (status < 0){
        printf("Failed to receive server status (%d)\n", status);
        fclose(f);
        return;
    }
    if (status != FTP_STATUS_OK){
        printf("Server rejected transfer: %s\n", statusMsg);
        fclose(f);
        return;
    }

    char *payload = sbuffer + sizeof(ftp_pdu);
    size_t bytesRead = 0;
    size_t totalSent = 0;
    bool abortTransfer = false;

    while ((bytesRead = fread(payload, 1, FTP_DATA_CHUNK, f)) > 0){
        ftp_pdu *pdu = (ftp_pdu *)sbuffer;
        memset(pdu, 0, sizeof(*pdu));
        pdu->msg_type = FTP_MSG_DATA;
        pdu->status = FTP_STATUS_OK;
        pdu->payload_size = bytesRead;
        pdu->total_size = totalSent + bytesRead;
        strncpy(pdu->file_name, cfg->file_name, sizeof(pdu->file_name) - 1);

        if (dpsend(dpc, sbuffer, sizeof(ftp_pdu) + bytesRead) < 0){
            perror("Failed to send data block");
            abortTransfer = true;
            break;
        }

        size_t serverProgress = 0;
        status = recv_status_pdu(dpc, statusMsg, sizeof(statusMsg), &serverProgress);
        if (status < 0){
            printf("Failed to receive server status (%d)\n", status);
            abortTransfer = true;
            break;
        }
        if (status != FTP_STATUS_OK){
            printf("Server reported error: %s\n", statusMsg);
            abortTransfer = true;
            break;
        }

        totalSent += bytesRead;
    }

    ftp_pdu closePdu = {0};
    closePdu.msg_type = FTP_MSG_CLOSE;
    strncpy(closePdu.file_name, cfg->file_name, sizeof(closePdu.file_name) - 1);
    if (dpsend(dpc, &closePdu, sizeof(closePdu)) < 0)
        perror("Failed to send close message");
    else {
        status = recv_status_pdu(dpc, statusMsg, sizeof(statusMsg), NULL);
        if (status < 0)
            printf("Close confirmation failed (%d)\n", status);
        else if (status != FTP_STATUS_OK)
            printf("Server close status: %s\n", statusMsg);
    }

    fclose(f);
    if (abortTransfer)
        printf("Transfer aborted due to errors\n");
    dpdisconnect(dpc);
}

void start_server(dp_connp dpc, const prog_config *cfg){
    server_loop(dpc, cfg);
}


int main(int argc, char *argv[])
{
    prog_config cfg;
    int cmd;
    dp_connp dpc;
    int rc;


    //Process the parameters and init the header - look at the helpers
    //in the cs472-pproto.c file
    cmd = initParams(argc, argv, &cfg);

    printf("MODE %d\n", cfg.prog_mode);
    printf("PORT %d\n", cfg.port_number);
    printf("FILE NAME: %s\n", cfg.file_name);

    switch(cmd){
        case PROG_MD_CLI:
            //by default client will look for files in the ./outfile directory
            snprintf(full_file_path, sizeof(full_file_path), "./outfile/%s", cfg.file_name);
            dpc = dpClientInit(cfg.svr_ip_addr,cfg.port_number);
            rc = dpconnect(dpc);
            if (rc < 0) {
                perror("Error establishing connection");
                exit(-1);
            }

            start_client(dpc, &cfg);
            exit(0);
            break;

        case PROG_MD_SVR:
            //by default server will look for files in the ./infile directory
            snprintf(full_file_path, sizeof(full_file_path), "./infile/%s", cfg.file_name);
            dpc = dpServerInit(cfg.port_number);
            rc = dplisten(dpc);
            if (rc < 0) {
                perror("Error establishing connection");
                exit(-1);
            }

            start_server(dpc, &cfg);
            break;
        default:
            printf("ERROR: Unknown Program Mode.  Mode set is %d\n", cmd);
            break;
    }
}