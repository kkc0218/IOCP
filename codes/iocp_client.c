// client_iocp.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define BUF_SIZE 2048

void ReadConfig(const char* filename, char* ip, int* port) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        printf("[ERROR] Cannot open config file: %s\n", filename);
        strcpy(ip, "127.0.0.1");
        *port = 8080;
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "docs_server", 11) == 0) {
            char* p = strchr(line, '=');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                sscanf(p, "%s %d", ip, port);
                break;
            }
        }
    }
    fclose(fp);
}

int main(int argc, char* argv[]) {
    char server_ip[64];
    int server_port;

    // stdout 버퍼링 비활성화
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("[Client] Starting IOCP client...\n");

    ReadConfig("config.txt", server_ip, &server_port);
    printf("[Client] Server config: %s:%d\n", server_ip, server_port);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[ERROR] WSAStartup failed\n");
        return 1;
    }

    // Create socket
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        printf("[ERROR] Socket creation failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    printf("[Client] Socket created\n");

    // Set socket options
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

    // Server address
    SOCKADDR_IN serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &serverAddr.sin_addr) <= 0) {
        printf("[ERROR] Invalid address: %s\n", server_ip);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("[Client] Connecting to %s:%d...\n", server_ip, server_port);

    // Connect
    if (connect(sock, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printf("[ERROR] Connect failed: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("[Client] Connected to server successfully!\n");
    printf("[Client] Available commands:\n");
    printf("  - create <doc_name> <section_count> <section1> <section2> ...\n");
    printf("  - write <doc_name> <section_name>\n");
    printf("  - read [doc_name section_name]\n");
    printf("  - bye\n\n");

    // Main loop
    char input[BUF_SIZE];
    char recvBuf[BUF_SIZE];
    fd_set readfds;
    struct timeval tv;

    while (1) {
        printf("> ");
        fflush(stdout);

        // Get user input
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }

        // Remove newline
        size_t len = strlen(input);
        if (len > 0 && input[len - 1] == '\n') {
            input[len - 1] = '\0';
            len--;
        }

        // Skip empty lines
        if (len == 0) continue;

        // Check for exit
        if (strcmp(input, "bye") == 0) {
            strcat(input, "\n");
            send(sock, input, (int)strlen(input), 0);

            // Wait for disconnection message
            int recvLen = recv(sock, recvBuf, sizeof(recvBuf) - 1, 0);
            if (recvLen > 0) {
                recvBuf[recvLen] = '\0';
                printf("%s", recvBuf);
            }
            break;
        }

        // Add newline for protocol
        strcat(input, "\n");

        // Send command
        printf("[Client] Sending command: '%s' (length: %d)\n", input, (int)strlen(input));
        // Print as hex for debugging
        printf("[Client] Hex data: ");
        for (int i = 0; i < strlen(input); i++) {
            printf("%02X ", (unsigned char)input[i]);
        }
        printf("\n");
        fflush(stdout);

        int sentBytes = send(sock, input, (int)strlen(input), 0);
        if (sentBytes == SOCKET_ERROR) {
            printf("[ERROR] Send failed: %d\n", WSAGetLastError());
            break;
        }

        printf("[Client] Successfully sent %d bytes\n", sentBytes);
        fflush(stdout);

        // Handle different command types
        if (strncmp(input, "write", 5) == 0) {
            // Write mode - handle multiple inputs
            printf("[Client] Entering write mode...\n");
            fflush(stdout);

            while (1) {
                // Wait for response with timeout
                FD_ZERO(&readfds);
                FD_SET(sock, &readfds);
                tv.tv_sec = 5;
                tv.tv_usec = 0;

                if (select(0, &readfds, NULL, NULL, &tv) > 0) {
                    int recvLen = recv(sock, recvBuf, sizeof(recvBuf) - 1, 0);
                    if (recvLen > 0) {
                        recvBuf[recvLen] = '\0';
                        printf("%s", recvBuf);
                        fflush(stdout);

                        if (strstr(recvBuf, "[Error]")) {
                            break;  // Exit write mode on error
                        }

                        if (strstr(recvBuf, "[Write_Completed]")) {
                            break;  // Exit write mode on completion
                        }

                        if (strstr(recvBuf, ">> ")) {
                            // Get input for write mode
                            if (!fgets(input, sizeof(input), stdin)) {
                                break;
                            }

                            // Send the line (keep newline for protocol)
                            send(sock, input, (int)strlen(input), 0);

                            // Check if it's <END>
                            if (strncmp(input, "<END>", 5) == 0) {
                                printf("[Client] Sending END signal, waiting for completion...\n");
                                fflush(stdout);
                            }
                        }
                    }
                    else if (recvLen == 0) {
                        printf("[Client] Server disconnected\n");
                        goto cleanup;
                    }
                }
                else {
                    printf("[ERROR] Timeout waiting for server response\n");
                    break;
                }
            }
        }
        else if (strncmp(input, "read", 4) == 0) {
            // Read mode - accumulate until __END__
            printf("[Client] Reading document...\n");
            fflush(stdout);
            char fullResponse[BUF_SIZE * 10] = { 0 };
            int totalLen = 0;

            while (1) {
                FD_ZERO(&readfds);
                FD_SET(sock, &readfds);
                tv.tv_sec = 5;
                tv.tv_usec = 0;

                if (select(0, &readfds, NULL, NULL, &tv) > 0) {
                    int recvLen = recv(sock, recvBuf, sizeof(recvBuf) - 1, 0);
                    if (recvLen > 0) {
                        recvBuf[recvLen] = '\0';

                        // Accumulate response
                        if (totalLen + recvLen < sizeof(fullResponse) - 1) {
                            strcat(fullResponse, recvBuf);
                            totalLen += recvLen;
                        }

                        // Check for end marker
                        if (strstr(fullResponse, "__END__")) {
                            // Remove __END__ and print
                            char* endPos = strstr(fullResponse, "__END__");
                            *endPos = '\0';
                            printf("%s", fullResponse);
                            break;
                        }
                    }
                    else if (recvLen == 0) {
                        printf("[Client] Server disconnected\n");
                        goto cleanup;
                    }
                }
                else {
                    printf("[ERROR] Timeout waiting for response\n");
                    break;
                }
            }
        }
        else {
            // Normal command - wait for single response
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);
            tv.tv_sec = 5;
            tv.tv_usec = 0;

            if (select(0, &readfds, NULL, NULL, &tv) > 0) {
                int recvLen = recv(sock, recvBuf, sizeof(recvBuf) - 1, 0);
                if (recvLen > 0) {
                    recvBuf[recvLen] = '\0';
                    printf("%s", recvBuf);
                }
                else if (recvLen == 0) {
                    printf("[Client] Server disconnected\n");
                    break;
                }
            }
            else {
                printf("[ERROR] Timeout waiting for response\n");
            }
        }
    }

cleanup:
    printf("[Client] Closing connection...\n");
    closesocket(sock);
    WSACleanup();

    return 0;
}
