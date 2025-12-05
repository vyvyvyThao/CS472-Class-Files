#include <stdio.h> 
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#include "du-proto.h"

static char _dpBuffer[DP_MAX_DGRAM_SZ];
static int  _debugMode = 1;
static int  _dpRetryMax = DP_MAX_RETRY_ATTEMPTS;
static int  _dpRetryDelayUsec = DP_RETRY_DELAY_USEC;
static int  _dpDropThreshold = 0;
static bool _dpRuntimeConfigLoaded = false;

static void dp_load_runtime_config(void);
static int dp_wait_for_ack(dp_connp dp, int expected_mtype);
static void dp_retry_backoff(int attempt);
static bool dp_simulate_drop(void);

static dp_connp dpinit(){
    dp_connp dpsession = malloc(sizeof(dp_connection));
    bzero(dpsession, sizeof(dp_connection));
    dpsession->outSockAddr.isAddrInit = false;
    dpsession->inSockAddr.isAddrInit = false;
    dpsession->outSockAddr.len = sizeof(struct sockaddr_in);
    dpsession->inSockAddr.len = sizeof(struct sockaddr_in);
    dpsession->seqNum = 0;
    dpsession->isConnected = false;
    dpsession->dbgMode = true;
    dp_load_runtime_config();
    return dpsession;
}

void dpclose(dp_connp dpsession) {
    free(dpsession);
}

int  dpmaxdgram(){
    return DP_MAX_BUFF_SZ;
}


dp_connp dpServerInit(int port) {
    struct sockaddr_in *servaddr;
    int *sock;
    int rc;

    dp_connp dpc = dpinit();
    if (dpc == NULL) {
        perror("drexel protocol create failure"); 
        return NULL;
    }

    sock = &(dpc->udp_sock);
    servaddr = &(dpc->inSockAddr.addr);
        

    // Creating socket file descriptor 
    if ( (*sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) { 
        perror("socket creation failed"); 
        return NULL;
    } 

    // Filling server information 
    servaddr->sin_family    = AF_INET; // IPv4 
    servaddr->sin_addr.s_addr = INADDR_ANY; 
    servaddr->sin_port = htons(port); 

    // Set socket options so that we dont have to wait for ports held by OS
    // if (setsockopt(*sock, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int)) < 0){
    //     perror("setsockopt(SO_REUSEADDR) failed");
    //     close(*sock);
    //     return NULL;
    // }
    if (setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0){
        perror("setsockopt(SO_REUSEADDR) failed");
        close(*sock);
        return NULL;
    }
    if ( (rc = bind(*sock, (const struct sockaddr *)servaddr,  
            dpc->inSockAddr.len)) < 0 ) 
    { 
        perror("bind failed"); 
        close (*sock);
        return NULL;
    } 

    dpc->inSockAddr.isAddrInit = true;
    dpc->outSockAddr.len = sizeof(struct sockaddr_in);
    return dpc;
}


dp_connp dpClientInit(char *addr, int port) {
    struct sockaddr_in *servaddr;
    int *sock;

    dp_connp dpc = dpinit();
    if (dpc == NULL) {
        perror("drexel protocol create failure"); 
        return NULL;
    }

    sock = &(dpc->udp_sock);
    servaddr = &(dpc->outSockAddr.addr);

    // Creating socket file descriptor 
    if ( (*sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) { 
        perror("socket creation failed"); 
        return NULL;
    } 

    // Filling server information 
    servaddr->sin_family = AF_INET; 
    servaddr->sin_port = htons(port); 
    servaddr->sin_addr.s_addr = inet_addr(addr);
    dpc->outSockAddr.len = sizeof(struct sockaddr_in); 
    dpc->outSockAddr.isAddrInit = true;

    // The inbound address is the same as the outbound address
    memcpy(&dpc->inSockAddr, &dpc->outSockAddr, sizeof(dpc->outSockAddr));

    return dpc;
}


int dprecv(dp_connp dp, void *buff, int buff_sz){

    if (buff == NULL || buff_sz <= 0)
        return DP_ERROR_GENERAL;

    int totalCopied = 0;
    bool expectingMore = true;

    while (expectingMore) {
        int rcvLen = dprecvdgram(dp, _dpBuffer, sizeof(_dpBuffer));

        if (rcvLen == DP_CONNECTION_CLOSED)
            return DP_CONNECTION_CLOSED;
        if (rcvLen < 0)
            return rcvLen;
        if (rcvLen < (int)sizeof(dp_pdu))
            return DP_ERROR_BAD_DGRAM;

        dp_pdu *inPdu = (dp_pdu *)_dpBuffer;
        int payloadSz = inPdu->dgram_sz;

        if ((int)(sizeof(dp_pdu) + payloadSz) > rcvLen)
            return DP_ERROR_BAD_DGRAM;
        if ((totalCopied + payloadSz) > buff_sz)
            return DP_BUFF_UNDERSIZED;

        if (payloadSz > 0)
            memcpy(((char *)buff) + totalCopied, _dpBuffer + sizeof(dp_pdu), payloadSz);

        totalCopied += payloadSz;
        if ((inPdu->mtype & DP_MT_FRAGMENT) != DP_MT_FRAGMENT)
            expectingMore = false;
    }

    return totalCopied;
}


static int dprecvdgram(dp_connp dp, void *buff, int buff_sz){
    int bytesIn = 0;
    int errCode = DP_NO_ERROR;

    if(buff_sz > DP_MAX_DGRAM_SZ)
        return DP_BUFF_OVERSIZED;

    bytesIn = dprecvraw(dp, buff, buff_sz);
    if (bytesIn < 0)
        return DP_ERROR_GENERAL;

    //check for some sort of error and just return it
    if (bytesIn < sizeof(dp_pdu))
        errCode = DP_ERROR_BAD_DGRAM;

    dp_pdu inPdu = {0};
    if (bytesIn >= (int)sizeof(dp_pdu))
        memcpy(&inPdu, buff, sizeof(dp_pdu));
    if ((int)(inPdu.dgram_sz + sizeof(dp_pdu)) > buff_sz)
        errCode = DP_BUFF_UNDERSIZED;

    //Copy buffer back
    // memcpy(buff, (_dpBuffer+sizeof(dp_pdu)), inPdu.dgram_sz);
    
    
    //UDPATE SEQ NUMBER AND PREPARE ACK
    if (errCode == DP_NO_ERROR){
        if(inPdu.dgram_sz == 0)
            //Update Seq Number to just ack a control message - just got PDU
            dp->seqNum ++;
        else
            //Update Seq Number to increas by the inbound PDU dgram_sz
            dp->seqNum += inPdu.dgram_sz;
    } else {
        //Update seq number to ACK error
        dp->seqNum++;
    }

    dp_pdu outPdu;
    outPdu.proto_ver = DP_PROTO_VER_1;
    outPdu.dgram_sz = 0;
    outPdu.seqnum = dp->seqNum;
    outPdu.err_num = errCode;

    int actSndSz = 0;
    //HANDLE ERROR SITUATION
    if(errCode != DP_NO_ERROR) {
        outPdu.mtype = DP_MT_ERROR;
        actSndSz = dpsendraw(dp, &outPdu, sizeof(dp_pdu));
        if (actSndSz != sizeof(dp_pdu))
            return DP_ERROR_PROTOCOL;
    }


    if (inPdu.mtype & DP_MT_SND){
        outPdu.mtype = DP_MT_SNDACK;
        actSndSz = dpsendraw(dp, &outPdu, sizeof(dp_pdu));
        if (actSndSz != sizeof(dp_pdu))
            return DP_ERROR_PROTOCOL;
    } else if (inPdu.mtype & DP_MT_CLOSE){
        outPdu.mtype = DP_MT_CLOSEACK;
        actSndSz = dpsendraw(dp, &outPdu, sizeof(dp_pdu));
        if (actSndSz != sizeof(dp_pdu))
            return DP_ERROR_PROTOCOL;
        dpclose(dp);
        return DP_CONNECTION_CLOSED;
    } else {
        printf("ERROR: Unexpected or bad mtype in header %d\n", inPdu.mtype);
        return DP_ERROR_PROTOCOL;
    }

    return bytesIn;
}


static int dprecvraw(dp_connp dp, void *buff, int buff_sz){
    int bytes = 0;

    if(!dp->inSockAddr.isAddrInit) {
        perror("dprecv: dp connection not setup properly - cli struct not init");
        return -1;
    }

    bytes = recvfrom(dp->udp_sock, (char *)buff, buff_sz,  
                MSG_WAITALL, ( struct sockaddr *) &(dp->outSockAddr.addr), 
                &(dp->outSockAddr.len)); 

    if (bytes < 0) {
        perror("dprecv: received error from recvfrom()");
        return -1;
    }
    dp->outSockAddr.isAddrInit = true;

    //some helper code if you want to do debugging
    if (bytes > sizeof(dp_pdu)){
        if(false) {                         //just diabling for now
            dp_pdu *inPdu = buff;
            char * payload = (char *)buff + sizeof(dp_pdu);
            printf("DATA : %.*s\n", inPdu->dgram_sz , payload); 
        }
    }

    dp_pdu *inPdu = buff;
    print_in_pdu(inPdu);

    //return the number of bytes received 
    return bytes;
}

int dpsend(dp_connp dp, void *sbuff, int sbuff_sz){

    if (sbuff_sz == 0)
        return dpsenddgram(dp, (sbuff != NULL) ? sbuff : _dpBuffer, 0, DP_MT_SND);
    if (sbuff == NULL || sbuff_sz < 0)
        return DP_ERROR_GENERAL;

    int totalSent = 0;

    while (totalSent < sbuff_sz){
        int chunk = sbuff_sz - totalSent;
        if (chunk > DP_MAX_BUFF_SZ)
            chunk = DP_MAX_BUFF_SZ;

        int mtype = DP_MT_SND;
        if (totalSent + chunk < sbuff_sz)
            mtype |= DP_MT_FRAGMENT;

        int sndSz = dpsenddgram(dp, ((char *)sbuff) + totalSent, chunk, mtype);
        if (sndSz < 0)
            return sndSz;
        totalSent += sndSz;
    }

    return totalSent;
}

static int dpsenddgram(dp_connp dp, void *sbuff, int sbuff_sz, int mtype){
    if(!dp->outSockAddr.isAddrInit) {
        perror("dpsend:dp connection not setup properly");
        return DP_ERROR_GENERAL;
    }

    if(sbuff_sz > DP_MAX_BUFF_SZ)
        return DP_ERROR_GENERAL;

    dp_pdu *outPdu = (dp_pdu *)_dpBuffer;
    int    sndSz = sbuff_sz;
    outPdu->proto_ver = DP_PROTO_VER_1;
    outPdu->mtype = mtype;
    outPdu->dgram_sz = sndSz;
    outPdu->seqnum = dp->seqNum;

    if (sndSz > 0)
        memcpy((_dpBuffer + sizeof(dp_pdu)), sbuff, sndSz);

    int totalSendSz = outPdu->dgram_sz + sizeof(dp_pdu);
    int attempt = 0;
    int lastErr = DP_ERROR_GENERAL;

    while (attempt < _dpRetryMax) {
        attempt++;

        bool simulatedDrop = dp_simulate_drop();
        if (!simulatedDrop) {
            int bytesOut = dpsendraw(dp, _dpBuffer, totalSendSz);
            if (bytesOut != totalSendSz) {
                lastErr = DP_ERROR_GENERAL;
                dp_retry_backoff(attempt);
                continue;
            }
        } else if (_debugMode) {
            printf("Simulating outbound message drop (attempt %d)\n", attempt);
        }

        int ackRc = simulatedDrop ? DP_ERROR_SHOULD_RETRY : dp_wait_for_ack(dp, DP_MT_SNDACK);
        if (ackRc == DP_NO_ERROR) {
            if(outPdu->dgram_sz == 0)
                dp->seqNum++;
            else
                dp->seqNum += outPdu->dgram_sz;
            return sbuff_sz;
        }

        lastErr = ackRc;
        dp_retry_backoff(attempt);
    }

    if (lastErr == DP_ERROR_SHOULD_RETRY)
        return DP_ERROR_RETRY_EXCEEDED;
    return lastErr;
}


static int dpsendraw(dp_connp dp, void *sbuff, int sbuff_sz){
    int bytesOut = 0;
    // dp_pdu *pdu = sbuff;

    if(!dp->outSockAddr.isAddrInit) {
        perror("dpsendraw:dp connection not setup properly");
        return -1;
    }

    dp_pdu *outPdu = sbuff;
    bytesOut = sendto(dp->udp_sock, (const char *)sbuff, sbuff_sz, 
        0, (const struct sockaddr *) &(dp->outSockAddr.addr), 
            dp->outSockAddr.len); 

    
    print_out_pdu(outPdu);

    return bytesOut;
}

static int dp_wait_for_ack(dp_connp dp, int expected_mtype){
    dp_pdu inPdu = {0};
    int bytesIn = dprecvraw(dp, &inPdu, sizeof(dp_pdu));
    if (bytesIn < (int)sizeof(dp_pdu))
        return DP_ERROR_SHOULD_RETRY;

    if (inPdu.mtype == DP_MT_ERROR)
        return DP_ERROR_SHOULD_RETRY;

    if ((inPdu.mtype & expected_mtype) != expected_mtype)
        return DP_ERROR_SHOULD_RETRY;

    return DP_NO_ERROR;
}

static void dp_retry_backoff(int attempt){
    if (_dpRetryDelayUsec <= 0)
        return;

    int delay = _dpRetryDelayUsec * (attempt > 1 ? attempt : 1);
    if (delay > (DP_RETRY_DELAY_USEC * 10))
        delay = DP_RETRY_DELAY_USEC * 10;
    usleep(delay);
}

static bool dp_simulate_drop(void){
    if (_dpDropThreshold <= 0)
        return false;
    return dprand(_dpDropThreshold) == 1;
}


int dplisten(dp_connp dp) {
    int sndSz, rcvSz;

    if(!dp->inSockAddr.isAddrInit) {
        perror("dplisten:dp connection not setup properly - cli struct not init");
        return DP_ERROR_GENERAL;
    }

    dp_pdu pdu = {0};

    printf("Waiting for a connection...\n");
    rcvSz = dprecvraw(dp, &pdu, sizeof(pdu));
    if (rcvSz != sizeof(pdu)) {
        perror("dplisten:The wrong number of bytes were received");
        return DP_ERROR_GENERAL;
    }

    pdu.mtype = DP_MT_CNTACK;
    dp->seqNum = pdu.seqnum + 1;
    pdu.seqnum = dp->seqNum;
    
    sndSz = dpsendraw(dp, &pdu, sizeof(pdu));
    
    if (sndSz != sizeof(pdu)) {
        perror("dplisten:The wrong number of bytes were sent");
        return DP_ERROR_GENERAL;
    }
    dp->isConnected = true; 
    //For non data transmissions, ACK of just control data increase seq # by one
    printf("Connection established OK!\n");

    return true;
}

int dpconnect(dp_connp dp) {

    int sndSz, rcvSz;

    if(!dp->outSockAddr.isAddrInit) {
        perror("dpconnect:dp connection not setup properly - svr struct not init");
        return DP_ERROR_GENERAL;
    }

    dp_pdu pdu = {0};
    pdu.mtype = DP_MT_CONNECT;
    pdu.seqnum = dp->seqNum;
    pdu.dgram_sz = 0;

    sndSz = dpsendraw(dp, &pdu, sizeof(pdu));
    if (sndSz != sizeof(dp_pdu)) {
        perror("dpconnect:Wrong about of connection data sent");
        return -1;
    }
    
    rcvSz = dprecvraw(dp, &pdu, sizeof(pdu));
    if (rcvSz != sizeof(dp_pdu)) {
        perror("dpconnect:Wrong about of connection data received");
        return -1;
    }
    if (pdu.mtype != DP_MT_CNTACK) {
        perror("dpconnect:Expected CNTACT Message but didnt get it");
        return -1;
    }

    //For non data transmissions, ACK of just control data increase seq # by one
    dp->seqNum++;
    dp->isConnected = true;
    printf("Connection established OK!\n");

    return true;
}

int dpdisconnect(dp_connp dp) {

    int sndSz, rcvSz;

    dp_pdu pdu = {0};
    pdu.proto_ver = DP_PROTO_VER_1;
    pdu.mtype = DP_MT_CLOSE;
    pdu.seqnum = dp->seqNum;
    pdu.dgram_sz = 0;

    sndSz = dpsendraw(dp, &pdu, sizeof(pdu));
    if (sndSz != sizeof(dp_pdu)) {
        perror("dpdisconnect:Wrong about of connection data sent");
        return DP_ERROR_GENERAL;
    }
    
    rcvSz = dprecvraw(dp, &pdu, sizeof(pdu));
    if (rcvSz != sizeof(dp_pdu)) {
        perror("dpdisconnect:Wrong about of connection data received");
        return DP_ERROR_GENERAL;
    }
    if (pdu.mtype != DP_MT_CLOSEACK) {
        perror("dpdisconnect:Expected CNTACT Message but didnt get it"); 
        return DP_ERROR_GENERAL;
    }
    //For non data transmissions, ACK of just control data increase seq # by one
    dpclose(dp);

    return DP_CONNECTION_CLOSED;
}

void * dp_prepare_send(dp_pdu *pdu_ptr, void *buff, int buff_sz) {
    if (buff_sz < sizeof(dp_pdu)) {
        perror("Expected CNTACT Message but didnt get it");
        return NULL;
    }
    bzero(buff, buff_sz);
    memcpy(buff, pdu_ptr, sizeof(dp_pdu));

    return buff + sizeof(dp_pdu);
}

static void dp_load_runtime_config(void){
    if (_dpRuntimeConfigLoaded)
        return;

    char *retryEnv = getenv("DP_MAX_RETRIES");
    if (retryEnv != NULL) {
        int val = atoi(retryEnv);
        if (val > 0)
            _dpRetryMax = val;
    }

    char *delayEnv = getenv("DP_RETRY_DELAY_USEC");
    if (delayEnv != NULL) {
        int val = atoi(delayEnv);
        if (val >= 0)
            _dpRetryDelayUsec = val;
    }

    char *dropEnv = getenv("DP_SIM_DROP_PCT");
    if (dropEnv != NULL) {
        int val = atoi(dropEnv);
        if (val >= 0 && val <= 99)
            _dpDropThreshold = val;
    }

    _dpRuntimeConfigLoaded = true;
}


//// MISC HELPERS
void print_out_pdu(dp_pdu *pdu) {
    if (_debugMode != 1)
        return;
    printf("PDU DETAILS ===>  [OUT]\n");
    print_pdu_details(pdu);
}
void print_in_pdu(dp_pdu *pdu) {
    if (_debugMode != 1)
        return;
    printf("===> PDU DETAILS  [IN]\n");
    print_pdu_details(pdu);
}
static void print_pdu_details(dp_pdu *pdu){
    
    printf("\tVersion:  %d\n", pdu->proto_ver);
    printf("\tMsg Type: %s\n", pdu_msg_to_string(pdu));
    printf("\tMsg Size: %d\n", pdu->dgram_sz);
    printf("\tSeq Numb: %d\n", pdu->seqnum);
    printf("\n");
}

static char * pdu_msg_to_string(dp_pdu *pdu) {
    static char desc[64];
    desc[0] = '\0';

    int type = pdu->mtype;
    if (type & DP_MT_ACK)
        strncat(desc, "ACK|", sizeof(desc) - strlen(desc) - 1);
    if (type & DP_MT_SND)
        strncat(desc, "SEND|", sizeof(desc) - strlen(desc) - 1);
    if (type & DP_MT_CONNECT)
        strncat(desc, "CONNECT|", sizeof(desc) - strlen(desc) - 1);
    if (type & DP_MT_CLOSE)
        strncat(desc, "CLOSE|", sizeof(desc) - strlen(desc) - 1);
    if (type & DP_MT_NACK)
        strncat(desc, "NACK|", sizeof(desc) - strlen(desc) - 1);
    if (type & DP_MT_FRAGMENT)
        strncat(desc, "FRAG|", sizeof(desc) - strlen(desc) - 1);
    if (type & DP_MT_ERROR)
        strncat(desc, "ERROR|", sizeof(desc) - strlen(desc) - 1);

    if (desc[0] == '\0'){
        snprintf(desc, sizeof(desc), "UNKNOWN:%d", type);
    } else {
        size_t len = strlen(desc);
        if (len > 0 && desc[len-1] == '|')
            desc[len-1] = '\0';
    }

    return desc;
}

/*
 *  This is a helper for testing if you want to inject random errors from
 *  time to time. It take a threshold number as a paramter and behaves as
 *  follows:
 *      if threshold is < 1 it always returns FALSE or zero
 *      if threshold is > 99 it always returns TRUE or 1
 *      if (1 <= threshold <= 99) it generates a random number between
 *          1..100 and if the random number is less than the threshold
 *          it returns TRUE, else it returns false
 * 
 *  Example: dprand(50) is a coin flip
 *              dprand(25) will return true 25% of the time
 *              dprand(99) will return true 99% of the time
 */
int dprand(int threshold){

    if (threshold < 1)
        return 0;
    if (threshold > 99)
        return 1;
    //initialize randome number seed
    srand(time(0));

    int rndInRange = (rand() % (100-1+1)) + 1;
    if (threshold < rndInRange)
        return 1;
    else
        return 0;
}