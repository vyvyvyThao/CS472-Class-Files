/**
 * =============================================================================
 * STUDENT ASSIGNMENT: CRYPTO-SERVER.C
 * =============================================================================
 * 
 * ASSIGNMENT OBJECTIVE:
 * Implement a TCP server that accepts client connections and processes
 * encrypted/plaintext messages. Your focus is on socket programming, connection
 * handling, and the server-side protocol implementation.
 * 
 * =============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdint.h>
#include "crypto-server.h"
#include "crypto-lib.h"
#include "protocol.h"

uint8_t compute_hash(const char *message, size_t len);  // Defined in crypto-client.c
int server_loop(int server_socket, const char* addr, int port);
int service_client_loop(int client_socket);
int build_response(crypto_msg_t *request, crypto_msg_t *response, crypto_key_t *client_key, crypto_key_t *server_key);


/* =============================================================================
 * STUDENT TODO: IMPLEMENT THIS FUNCTION
 * =============================================================================
 * This is the main server initialization function. You need to:
 * 1. Create a TCP socket
 * 2. Set socket options (SO_REUSEADDR)
 * 3. Bind to the specified address and port
 * 4. Start listening for connections
 * 5. Call your server loop function
 * 6. Clean up when done
 * 
 * Parameters:
 *   addr - Server bind address (e.g., "0.0.0.0" for all interfaces)
 *   port - Server port number (e.g., 1234)
 * 
 * NOTE: If addr is "0.0.0.0", use INADDR_ANY instead of inet_pton()
 */

// External references to global sockets (defined in crypto-echo.c)
extern int server_sockfd;
extern int client_sockfd;

void start_server(const char* addr, int port) {
    int sockfd;
    struct sockaddr_in server_addr;
    int reuse = 1;

    // 1. Create TCP socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Error creating socket");
        exit(EXIT_FAILURE);
    }

    server_sockfd = sockfd; // For signal handler

    // 2. Set SO_REUSEADDR option (for development)
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("Error setting socket options");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // 3. Configure server address (sockaddr_in)
    //    - Handle "0.0.0.0" specially (use INADDR_ANY)
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (strcmp(addr, "0.0.0.0") == 0) {
        server_addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, addr, &server_addr.sin_addr) <= 0) {
            fprintf(stderr, "Error: Invalid address %s\n", addr);
            close(sockfd);
            exit(EXIT_FAILURE);
        }
    }

    // 4. Bind socket to address
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error binding socket");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // 5. Start listening (use BACKLOG constant)
    if (listen(sockfd, BACKLOG) < 0) {
        perror("Error listening on socket");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    printf("Server listening on %s:%d\n", addr, port);
    
    // 6. Call your server loop function
    server_loop(sockfd, addr, port);
    
    // 7. Close socket
    close(sockfd);
    server_sockfd = -1;
    printf("Server shutdown complete.\n");
}


int server_loop(int server_socket, const char* addr, int port) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char client_ip[INET_ADDRSTRLEN];
    int client_socket;
    int result;
    
    // 1. Print "Server listening..." message
    printf("Waiting for client connection...\n");
    
    // 2. Infinite loop:
    while (1) {
        //    a) Accept connection (creates new client socket)
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("Error accepting connection");
            continue;
        }
        
        client_sockfd = client_socket; // For signal handler
        
        //    b) Get client IP using inet_ntop()
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        
        //    c) Print "Client connected..." message
        printf("Client connected from %s:%d\n", client_ip, ntohs(client_addr.sin_port));
        
        //    d) Call service_client_loop(client_socket)
        result = service_client_loop(client_socket);
        
        //    e) Check return code:
        //    f) Close client socket
        close(client_socket);
        client_sockfd = -1; // Reset for signal handler
        printf("Client connection closed.\n");
        
        //       - RC_CLIENT_EXITED: close socket, accept next client
        if (result == RC_CLIENT_EXITED) {
            printf("Ready for next client connection.\n\n");
            continue;
        }
        //       - RC_CLIENT_REQ_SERVER_EXIT: close sockets, return
        else if (result == RC_CLIENT_REQ_SERVER_EXIT) {
            printf("Server shutdown requested by client.\n");
            break;
        }
        //       - Error: close socket, continue
        else {
            printf("Error in client service, ready for next client.\n\n");
            continue;
        }
    }
    
    // 3. Return when server shutdown requested
    return 0;
}

int service_client_loop(int client_socket) {
    // 1. Allocate send/receive buffers
    char *send_buffer = malloc(BUFFER_SIZE);
    char *recv_buffer = malloc(BUFFER_SIZE);
    
    // 2. Initialize keys to NULL_CRYPTO_KEY
    crypto_key_t server_key = NULL_CRYPTO_KEY;
    crypto_key_t client_key = NULL_CRYPTO_KEY;
    ssize_t received;
    int response_size;
    
    if (!send_buffer || !recv_buffer) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        if (send_buffer) free(send_buffer);
        if (recv_buffer) free(recv_buffer);
        return RC_CLIENT_EXITED;
    }
    
    // 3. Loop:
    while (1) {
        //    a) Receive PDU from client
        received = recv(client_socket, recv_buffer, BUFFER_SIZE, 0);
        
        //    b) Check recv() return:
        if (received <= 0) {
            if (received == 0) {
                //       - 0: client closed, return RC_CLIENT_EXITED
                printf("Client disconnected gracefully.\n");
            } else {
                //       - <0: error, return RC_CLIENT_EXITED
                perror("Error receiving data from client");
            }
            // 4. Free buffers before returning
            free(send_buffer);
            free(recv_buffer);
            return RC_CLIENT_EXITED;
        }
        
        //    c) Cast buffer to crypto_msg_t*
        crypto_msg_t *request = (crypto_msg_t *)recv_buffer;
        
        printf("<<<<<<<<<<<<<<< REQUEST <<<<<<<<<<<<<<<\n");
        printf("-------------------------\n");
        print_msg_info(request, server_key, SERVER_MODE);
        printf("<<<<<<<<<<<<< END REQUEST <<<<<<<<<<<<<\n\n");
        
        //    d) Check for MSG_CMD_SERVER_STOP -> return RC_CLIENT_REQ_SERVER_EXIT
        if (request->header.msg_type == MSG_CMD_SERVER_STOP) {
            free(send_buffer);
            free(recv_buffer);
            return RC_CLIENT_REQ_SERVER_EXIT;
        }
        
        // Check for client exit command
        if (request->header.msg_type == MSG_CMD_CLIENT_STOP) {
            free(send_buffer);
            free(recv_buffer);
            return RC_CLIENT_EXITED;
        }
        
        //    e) Build response PDU (use helper function)
        crypto_msg_t *response = (crypto_msg_t *)send_buffer;
        response_size = build_response(request, response, &client_key, &server_key);
        
        if (response_size < 0) {
            printf("[ERROR] Failed to build response\n");
            continue; // Continue to next message
        }
        
        // Print response info
        printf(">>>>>>>>>>>>>>> RESPONSE >>>>>>>>>>>>>>>\n");
        printf("-------------------------\n");
        print_msg_info(response, server_key, SERVER_MODE);
        printf(">>>>>>>>>>>>> END RESPONSE >>>>>>>>>>>>>\n\n");
        
        //    f) Send response
        if (send(client_socket, send_buffer, response_size, 0) < 0) {
            perror("Error sending response to client");
            break;
        }
        
        //    g) Loop back
    }
    
    // 4. Free buffers before returning
    free(send_buffer);
    free(recv_buffer);
    return RC_CLIENT_EXITED;
}


int build_response(crypto_msg_t *request, crypto_msg_t *response,
    crypto_key_t *client_key, crypto_key_t *server_key) {
    // 1. Set response->header.direction = DIR_RESPONSE
    response->header.direction = DIR_RESPONSE;
    response->header.msg_type = request->header.msg_type;
    response->header.payload_len = 0;
    
    switch (request->header.msg_type) {
        case MSG_KEY_EXCHANGE: {
            // Generate key pair
            if (gen_key_pair(server_key, client_key) != RC_OK) {
                printf("[ERROR] Key generation failed\n");
                return -1;
            }
            
            // Send client's key in response
            memcpy(response->payload, client_key, sizeof(crypto_key_t));
            response->header.payload_len = sizeof(crypto_key_t);
            
            printf("Generated keys - Server: 0x%04X, Client: 0x%04X\n", 
                   *server_key, *client_key);
            break;
        }
        
        case MSG_DATA: {
            char echo_msg[MAX_MSG_DATA_SIZE];
            snprintf(echo_msg, sizeof(echo_msg), "echo %.*s", 
                    (int)request->header.payload_len, request->payload);
            
            size_t echo_len = strlen(echo_msg);
            memcpy(response->payload, echo_msg, echo_len);
            response->header.payload_len = echo_len;
            break;
        }
        
        case MSG_ENCRYPTED_DATA: {
            if (*server_key == NULL_CRYPTO_KEY) {
                printf("[ERROR] No server key available for decryption\n");
                return -1;
            }
            
            // Decrypt incoming message
            uint8_t decrypted[MAX_MSG_DATA_SIZE];
            int decrypted_len = decrypt_string(*server_key, decrypted, 
                                             request->payload, request->header.payload_len);
            if (decrypted_len < 0) {
                printf("[ERROR] Decryption failed\n");
                return -1;
            }
            
            decrypted[decrypted_len] = '\0';
            
            // Create echo response
            char echo_msg[MAX_MSG_DATA_SIZE];
            snprintf(echo_msg, sizeof(echo_msg), "echo %s", decrypted);
            
            // Encrypt echo response
            int encrypted_len = encrypt_string(*server_key, response->payload, 
                                             (uint8_t*)echo_msg, strlen(echo_msg));
            if (encrypted_len < 0) {
                printf("[ERROR] Encryption failed\n");
                return -1;
            }
            
            response->header.payload_len = encrypted_len;
            break;
        }
        
        case MSG_DIG_SIGNATURE: {
            if (*server_key == NULL_CRYPTO_KEY) {
                printf("[ERROR] No server key available for signature verification\n");
                return -1;
            }
            
            if (request->header.payload_len < 2) {
                printf("[ERROR] Invalid digital signature message format\n");
                return -1;
            }
            
            // Extract encrypted signature and message
            uint8_t encrypted_sig = request->payload[0];
            const char *message = (const char *)&request->payload[1];
            size_t message_len = request->header.payload_len - 1;
            
            printf("[DEBUG] Encrypted signature received: 0x%02X\n", encrypted_sig);
            printf("[DEBUG] Message: '%.*s' (len=%zu)\n", (int)message_len, message, message_len);
            
            // Decrypt signature
            uint8_t received_hash;
            if (decrypt(*server_key, &received_hash, &encrypted_sig, 1) != RC_OK) {
                printf("[ERROR] Failed to decrypt client signature\n");
                return -1;
            }
            
            // Compute hash of the received message
            uint8_t computed_hash = compute_hash(message, message_len);
            
            printf("[DEBUG] Decrypted hash: 0x%02X\n", received_hash);
            printf("[DEBUG] Computed hash: 0x%02X\n", computed_hash);
            
            // Verify signature
            if (received_hash != computed_hash) {
                printf("[ERROR] Digital signature verification failed!\n");
                printf("Expected: 0x%02X, Got: 0x%02X\n", computed_hash, received_hash);
                return -1;
            }
            
            printf("✓ Client signature verified\n");
            
            // Create echo response
            char echo_msg[MAX_MSG_DATA_SIZE];
            snprintf(echo_msg, sizeof(echo_msg), "echo %.*s", (int)message_len, message);
            
            // Create signed response
            size_t echo_len = strlen(echo_msg);
            uint8_t response_hash = compute_hash(echo_msg, echo_len);
            
            // Encrypt the response hash
            uint8_t encrypted_response_hash;
            if (encrypt(*server_key, &encrypted_response_hash, &response_hash, 1) != RC_OK) {
                printf("[ERROR] Failed to encrypt response signature\n");
                return -1;
            }
            
            // Build signed response payload: [encrypted_hash][response_message]
            response->payload[0] = encrypted_response_hash;
            memcpy(&response->payload[1], echo_msg, echo_len);
            response->header.payload_len = 1 + echo_len;
            
            printf("Response signed with hash: 0x%02X\n", response_hash);
            break;
        }
        
        case MSG_CMD_CLIENT_STOP:
        case MSG_CMD_SERVER_STOP:
            // No response needed for commands
            response->header.payload_len = 0;
            break;
            
        default:
            printf("[ERROR] Unknown message type: %d\n", request->header.msg_type);
            return -1;
    }
    
    return sizeof(crypto_pdu_t) + response->header.payload_len;
}
