/// server_iocp.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

#define MAX_DOCS 100
#define MAX_SECTIONS 10
#define MAX_TITLE 64
#define MAX_LINE 256
#define MAX_LINES 10
#define BUF_SIZE 2048
#define MAX_WORKERS 8

// IO Operation types
typedef enum {
    OP_ACCEPT,
    OP_RECV,
    OP_SEND,
    OP_WRITE_WAIT
} IO_OPERATION;

// Forward declarations
typedef struct ClientContext ClientContext;

// Per-IO data structure
typedef struct {
    OVERLAPPED overlapped;
    WSABUF wsaBuf;
    char buffer[BUF_SIZE];
    IO_OPERATION operation;
    SOCKET socket;
    ClientContext* client;
} PER_IO_DATA;

// Client context structure
struct ClientContext {
    SOCKET socket;
    CRITICAL_SECTION cs;
    char recvBuffer[BUF_SIZE];
    int recvPos;
    char* args[64];
    int argc;

    // Write operation state
    char tempLines[MAX_LINES][MAX_LINE];
    int lineCount;
    int docIdx;
    int sectionIdx;
    LONG64 writeTicket;

    BOOL isWriteMode;
};

// Document structure
typedef struct {
    char title[MAX_TITLE];
    char section_titles[MAX_SECTIONS][MAX_TITLE];
    char section_contents[MAX_SECTIONS][MAX_LINES][MAX_LINE];
    int section_line_count[MAX_SECTIONS];
    int section_count;
} Document;

// Lock-free queue node for write requests
typedef struct WriteNode {
    struct WriteNode* volatile next;
    LONG64 ticket;
    ClientContext* client;
    int estimatedLines;
} WriteNode;

// Lock-free queue for each section
typedef struct {
    WriteNode* volatile head;
    WriteNode* volatile tail;
    volatile LONG64 currentTicket;
    volatile LONG64 nextTicket;
} LockFreeQueue;

// Global variables
Document docs[MAX_DOCS];
volatile LONG doc_count = 0;
HANDLE g_hIOCP = NULL;
SOCKET g_listenSocket = INVALID_SOCKET;
LockFreeQueue sectionQueues[MAX_DOCS][MAX_SECTIONS];
SRWLOCK docsLock;
LPFN_ACCEPTEX lpfnAcceptEx = NULL;

// Function prototypes
void InitializeLockFreeQueue(LockFreeQueue* queue);
void EnqueueWrite(LockFreeQueue* queue, ClientContext* client, int estimatedLines);
WriteNode* DequeueWrite(LockFreeQueue* queue);
Document* FindDoc(const char* title);
void ParseCommand(const char* input, char* args[], int* argc);
BOOL SendData(ClientContext* client, const char* data, int len);
void ProcessCommand(ClientContext* client);
void ProcessWriteLine(ClientContext* client, const char* line);
unsigned __stdcall WorkerThread(void* param);

void InitializeLockFreeQueue(LockFreeQueue* queue) {
    queue->head = queue->tail = NULL;
    queue->currentTicket = 0;
    queue->nextTicket = 0;
}

void EnqueueWrite(LockFreeQueue* queue, ClientContext* client, int estimatedLines) {
    WriteNode* node = (WriteNode*)malloc(sizeof(WriteNode));
    node->client = client;
    node->estimatedLines = estimatedLines;
    node->next = NULL;

    // Get ticket atomically
    node->ticket = InterlockedIncrement64(&queue->nextTicket) - 1;
    client->writeTicket = node->ticket;

    // Lock-free enqueue with priority (shorter writes first)
    WriteNode* prev = NULL;
    WriteNode* curr = NULL;

    while (1) {
        prev = NULL;
        curr = queue->head;

        // Find insertion point based on estimated lines
        while (curr && curr->estimatedLines <= estimatedLines) {
            prev = curr;
            curr = curr->next;
        }

        node->next = curr;

        if (prev == NULL) {
            // Insert at head
            if (InterlockedCompareExchangePointer((PVOID*)&queue->head, node, curr) == curr) {
                if (curr == NULL) {
                    InterlockedCompareExchangePointer((PVOID*)&queue->tail, node, NULL);
                }
                break;
            }
        }
        else {
            // Insert after prev
            if (InterlockedCompareExchangePointer((PVOID*)&prev->next, node, curr) == curr) {
                if (curr == NULL) {
                    InterlockedCompareExchangePointer((PVOID*)&queue->tail, node, prev);
                }
                break;
            }
        }
    }
}

WriteNode* DequeueWrite(LockFreeQueue* queue) {
    WriteNode* head;

    while (1) {
        head = queue->head;
        if (head == NULL) return NULL;

        // Check if it's this writer's turn
        if (head->ticket == queue->currentTicket) {
            if (InterlockedCompareExchangePointer((PVOID*)&queue->head, head->next, head) == head) {
                if (head->next == NULL) {
                    InterlockedCompareExchangePointer((PVOID*)&queue->tail, NULL, head);
                }
                InterlockedIncrement64(&queue->currentTicket);
                return head;
            }
        }
        else {
            return NULL;
        }
    }
}

Document* FindDoc(const char* title) {
    for (int i = 0; i < doc_count; i++) {
        if (strcmp(docs[i].title, title) == 0)
            return &docs[i];
    }
    return NULL;
}

void ParseCommand(const char* input, char* args[], int* argc) {
    *argc = 0;
    const char* p = input;

    // Free previous args
    for (int i = 0; i < 64 && args[i]; i++) {
        free(args[i]);
        args[i] = NULL;
    }

    while (*p && *argc < 64) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '"') {
            p++;
            const char* start = p;
            while (*p && *p != '"') p++;
            int len = (int)(p - start);
            args[*argc] = (char*)malloc(len + 1);
            strncpy(args[*argc], start, len);
            args[*argc][len] = '\0';
            (*argc)++;
            if (*p == '"') p++;
        }
        else {
            const char* start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
            int len = (int)(p - start);
            args[*argc] = (char*)malloc(len + 1);
            strncpy(args[*argc], start, len);
            args[*argc][len] = '\0';
            (*argc)++;
        }
    }
}

BOOL SendData(ClientContext* client, const char* data, int len) {
    PER_IO_DATA* ioData = (PER_IO_DATA*)malloc(sizeof(PER_IO_DATA));
    ZeroMemory(&ioData->overlapped, sizeof(OVERLAPPED));
    ioData->operation = OP_SEND;
    ioData->client = client;

    if (len < 0) len = (int)strlen(data);
    memcpy(ioData->buffer, data, len);
    ioData->wsaBuf.buf = ioData->buffer;
    ioData->wsaBuf.len = len;

    DWORD bytesSent;
    if (WSASend(client->socket, &ioData->wsaBuf, 1, &bytesSent, 0,
        &ioData->overlapped, NULL) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSA_IO_PENDING) {
            printf("[ERROR] WSASend failed: %d\n", WSAGetLastError());
            free(ioData);
            return FALSE;
        }
    }
    return TRUE;
}

void ProcessWriteLine(ClientContext* client, const char* line) {
    printf("[Worker-%d] Processing write line: '%s'\n", GetCurrentThreadId(), line);
    fflush(stdout);

    if (strcmp(line, "<END>") == 0) {
        printf("[Worker-%d] Write mode: END signal received, saving %d lines\n",
            GetCurrentThreadId(), client->lineCount);
        fflush(stdout);

        // Enqueue write request
        LockFreeQueue* queue = &sectionQueues[client->docIdx][client->sectionIdx];
        EnqueueWrite(queue, client, client->lineCount);

        // Wait for turn
        while (1) {
            WriteNode* node = DequeueWrite(queue);
            if (node && node->client == client) {
                // It's our turn, write to document
                AcquireSRWLockExclusive(&docsLock);

                Document* doc = &docs[client->docIdx];
                doc->section_line_count[client->sectionIdx] = 0;

                for (int j = 0; j < client->lineCount && j < MAX_LINES; j++) {
                    strcpy(doc->section_contents[client->sectionIdx][j],
                        client->tempLines[j]);
                }
                doc->section_line_count[client->sectionIdx] = client->lineCount;

                ReleaseSRWLockExclusive(&docsLock);

                free(node);
                SendData(client, "[Write_Completed]\n", -1);

                // Write 모드 종료
                client->isWriteMode = FALSE;
                client->recvPos = 0;  // Reset receive buffer

                // Return to normal recv mode
                PER_IO_DATA* recvData = (PER_IO_DATA*)malloc(sizeof(PER_IO_DATA));
                ZeroMemory(&recvData->overlapped, sizeof(OVERLAPPED));
                recvData->operation = OP_RECV;
                recvData->client = client;
                recvData->wsaBuf.buf = recvData->buffer;
                recvData->wsaBuf.len = BUF_SIZE;

                DWORD flags = 0;
                DWORD bytesRecv = 0;
                WSARecv(client->socket, &recvData->wsaBuf, 1, &bytesRecv,
                    &flags, &recvData->overlapped, NULL);
                break;
            }
            Sleep(1);
        }
    }
    else {
        // Store line
        if (client->lineCount < MAX_LINES) {
            strcpy(client->tempLines[client->lineCount], line);
            client->lineCount++;
            printf("[Worker-%d] Write mode: stored line %d: '%s'\n",
                GetCurrentThreadId(), client->lineCount, line);
            fflush(stdout);
        }
        SendData(client, ">> ", -1);
    }
}

void ProcessCommand(ClientContext* client) {
    if (client->argc == 0) return;

    printf("[Server] Processing command: %s\n", client->args[0]);
    fflush(stdout);

    if (strcmp(client->args[0], "create") == 0) {
        AcquireSRWLockExclusive(&docsLock);

        if (client->argc < 3 || doc_count >= MAX_DOCS) {
            ReleaseSRWLockExclusive(&docsLock);
            SendData(client, "[Error] Invalid create command.\n", -1);
            return;
        }

        if (FindDoc(client->args[1])) {
            ReleaseSRWLockExclusive(&docsLock);
            SendData(client, "[Error] Document already exists.\n", -1);
            return;
        }

        int section_count = atoi(client->args[2]);
        if (section_count <= 0 || section_count > MAX_SECTIONS ||
            client->argc != 3 + section_count) {
            ReleaseSRWLockExclusive(&docsLock);
            SendData(client, "[Error] Invalid section count or titles.\n", -1);
            return;
        }

        int idx = (int)doc_count;
        strcpy(docs[idx].title, client->args[1]);
        docs[idx].section_count = section_count;

        for (int i = 0; i < section_count; i++) {
            strcpy(docs[idx].section_titles[i], client->args[3 + i]);
            docs[idx].section_line_count[i] = 0;
        }

        InterlockedIncrement(&doc_count);
        ReleaseSRWLockExclusive(&docsLock);

        SendData(client, "[OK] Document created.\n", -1);
    }
    else if (strcmp(client->args[0], "write") == 0) {
        if (client->argc < 3) {
            SendData(client, "[Error] Invalid write command.\n", -1);
            return;
        }

        AcquireSRWLockShared(&docsLock);
        Document* doc = FindDoc(client->args[1]);
        if (!doc) {
            ReleaseSRWLockShared(&docsLock);
            SendData(client, "[Error] Document not found.\n", -1);
            return;
        }

        int section_idx = -1;
        for (int i = 0; i < doc->section_count; i++) {
            if (strcmp(doc->section_titles[i], client->args[2]) == 0) {
                section_idx = i;
                break;
            }
        }

        if (section_idx == -1) {
            ReleaseSRWLockShared(&docsLock);
            SendData(client, "[Error] Section not found.\n", -1);
            return;
        }

        client->docIdx = (int)(doc - docs);
        client->sectionIdx = section_idx;
        client->lineCount = 0;
        client->recvPos = 0;  // Reset receive buffer
        client->isWriteMode = TRUE;  // Set write mode flag
        ReleaseSRWLockShared(&docsLock);

        printf("[Server] Write mode enabled for client, doc=%d, section=%d\n",
            client->docIdx, client->sectionIdx);
        fflush(stdout);

        SendData(client, "[OK] You can start writing. Send <END> to finish.\n>> ", -1);
        // Don't post new WSARecv here - the existing OP_RECV will handle it
    }
    else if (strcmp(client->args[0], "read") == 0) {
        char response[BUF_SIZE * 10] = { 0 };
        int pos = 0;

        AcquireSRWLockShared(&docsLock);

        if (client->argc == 1) {
            for (int i = 0; i < doc_count; i++) {
                pos += sprintf(response + pos, "%s\n", docs[i].title);
                for (int j = 0; j < docs[i].section_count; j++) {
                    pos += sprintf(response + pos, "    %d. %s\n", j + 1, docs[i].section_titles[j]);
                }
            }
        }
        else if (client->argc >= 3) {
            Document* doc = FindDoc(client->args[1]);
            if (!doc) {
                ReleaseSRWLockShared(&docsLock);
                SendData(client, "[Error] Document not found.\n__END__\n", -1);
                return;
            }

            int found = 0;
            for (int i = 0; i < doc->section_count; i++) {
                if (strcmp(doc->section_titles[i], client->args[2]) == 0) {
                    found = 1;
                    pos += sprintf(response + pos, "%s\n    %d. %s\n",
                        doc->title, i + 1, doc->section_titles[i]);

                    for (int j = 0; j < doc->section_line_count[i]; j++) {
                        pos += sprintf(response + pos, "       %s\n",
                            doc->section_contents[i][j]);
                    }
                    break;
                }
            }

            if (!found) {
                strcpy(response, "[Error] Section not found.\n");
                pos = (int)strlen(response);
            }
        }

        ReleaseSRWLockShared(&docsLock);

        strcpy(response + pos, "__END__\n");
        SendData(client, response, -1);
    }
    else if (strcmp(client->args[0], "bye") == 0) {
        SendData(client, "[Disconnected]\n", -1);
        // 소켓 종료는 SendData 완료 후 처리
    }
    else {
        SendData(client, "[Error] Unknown command.\n", -1);
    }
}

unsigned __stdcall WorkerThread(void* param) {
    DWORD bytesTransferred;
    ULONG_PTR completionKey;
    LPOVERLAPPED overlapped;
    PER_IO_DATA* ioData;

    printf("[Worker] Thread %d started\n", GetCurrentThreadId());
    fflush(stdout);  // 즉시 출력

    while (1) {
        printf("[Worker-%d] Waiting for completion status...\n", GetCurrentThreadId());
        fflush(stdout);

        BOOL result = GetQueuedCompletionStatus(g_hIOCP, &bytesTransferred,
            &completionKey, &overlapped, INFINITE);

        printf("[Worker-%d] Got completion status: result=%d, bytes=%d, overlapped=%p\n",
            GetCurrentThreadId(), result, bytesTransferred, overlapped);
        fflush(stdout);

        if (!result) {
            DWORD error = GetLastError();
            if (overlapped == NULL) {
                printf("[Worker-%d] GetQueuedCompletionStatus failed with NULL overlapped: %d\n",
                    GetCurrentThreadId(), error);
                fflush(stdout);
                continue;
            }

            printf("[Worker-%d] GetQueuedCompletionStatus failed: %d\n",
                GetCurrentThreadId(), error);
            fflush(stdout);
            ioData = CONTAINING_RECORD(overlapped, PER_IO_DATA, overlapped);

            // 연결이 끊어진 경우 정리
            if (ioData->client) {
                closesocket(ioData->client->socket);
                DeleteCriticalSection(&ioData->client->cs);
                for (int i = 0; i < 64 && ioData->client->args[i]; i++) {
                    free(ioData->client->args[i]);
                }
                free(ioData->client);
            }
            free(ioData);
            continue;
        }

        // overlapped가 NULL인 경우 체크
        if (overlapped == NULL) {
            printf("[Worker-%d] WARNING: overlapped is NULL but result is success\n", GetCurrentThreadId());
            fflush(stdout);
            continue;
        }

        ioData = CONTAINING_RECORD(overlapped, PER_IO_DATA, overlapped);
        printf("[Worker-%d] Operation type: %d\n", GetCurrentThreadId(), ioData->operation);
        fflush(stdout);

        if (bytesTransferred == 0 && ioData->operation == OP_RECV)
        {
            printf("[Worker-%d] Client disconnected (OP_RECV with 0 bytes)\n", GetCurrentThreadId());
            fflush(stdout);
            if (ioData->client)
            {
                closesocket(ioData->client->socket);
                DeleteCriticalSection(&ioData->client->cs);
                for (int i = 0; i < 64 && ioData->client->args[i]; i++)
                {
                    free(ioData->client->args[i]);
                }
                free(ioData->client);
            }
            free(ioData);
            continue;
        }

        switch (ioData->operation) {
        case OP_ACCEPT: {
            // New client accepted
            printf("[Worker-%d] Processing OP_ACCEPT, socket=%llu, bytesTransferred=%d\n",
                GetCurrentThreadId(), (ULONGLONG)ioData->socket, bytesTransferred);
            fflush(stdout);

            // Check if socket is valid
            if (ioData->socket == INVALID_SOCKET) {
                printf("[ERROR] Accept socket is invalid!\n");
                free(ioData);
                break;
            }

            // If AcceptEx received initial data
            if (bytesTransferred > 0) {
                printf("[Worker-%d] AcceptEx received %d bytes of initial data\n",
                    GetCurrentThreadId(), bytesTransferred);
                // This data will be processed after WSARecv is set up
            }

            // Update accept socket context - IMPORTANT!
            int updateResult = setsockopt(ioData->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                (char*)&g_listenSocket, sizeof(g_listenSocket));

            printf("[Worker-%d] SO_UPDATE_ACCEPT_CONTEXT result: %d (error: %d)\n",
                GetCurrentThreadId(), updateResult, WSAGetLastError());
            fflush(stdout);

            // Set TCP_NODELAY for immediate send
            int flag = 1;
            setsockopt(ioData->socket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

            // Create new client context
            ClientContext* newClient = (ClientContext*)malloc(sizeof(ClientContext));
            ZeroMemory(newClient, sizeof(ClientContext));
            newClient->socket = ioData->socket;
            newClient->isWriteMode = FALSE;  // Initialize write mode flag
            InitializeCriticalSection(&newClient->cs);

            printf("[Worker-%d] Created client context for socket %llu\n",
                GetCurrentThreadId(), (ULONGLONG)newClient->socket);
            fflush(stdout);

            // Get client address info
            SOCKADDR_IN clientAddr;
            int addrLen = sizeof(clientAddr);
            if (getpeername(newClient->socket, (SOCKADDR*)&clientAddr, &addrLen) == 0) {
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
                printf("[Worker-%d] Client connected from %s:%d\n",
                    GetCurrentThreadId(), ipStr, ntohs(clientAddr.sin_port));
                fflush(stdout);
            }
            else {
                printf("[Worker-%d] getpeername failed: %d\n", GetCurrentThreadId(), WSAGetLastError());
                fflush(stdout);
            }

            // Associate client socket with IOCP
            HANDLE hResult = CreateIoCompletionPort((HANDLE)newClient->socket, g_hIOCP,
                (ULONG_PTR)newClient, 0);

            if (hResult == NULL) {
                printf("[ERROR] Failed to associate client socket with IOCP: %d\n", GetLastError());
                closesocket(newClient->socket);
                DeleteCriticalSection(&newClient->cs);
                free(newClient);
                free(ioData);
                break;
            }

            printf("[Worker-%d] Client socket associated with IOCP successfully\n", GetCurrentThreadId());
            fflush(stdout);

            // Start receiving from client
            PER_IO_DATA* recvData = (PER_IO_DATA*)malloc(sizeof(PER_IO_DATA));
            ZeroMemory(&recvData->overlapped, sizeof(OVERLAPPED));
            recvData->operation = OP_RECV;
            recvData->client = newClient;
            recvData->wsaBuf.buf = recvData->buffer;
            recvData->wsaBuf.len = BUF_SIZE;

            printf("[Worker-%d] Starting WSARecv on client socket...\n", GetCurrentThreadId());
            fflush(stdout);

            DWORD flags = 0;
            DWORD bytesRecv = 0;
            int recvResult = WSARecv(newClient->socket, &recvData->wsaBuf, 1, &bytesRecv,
                &flags, &recvData->overlapped, NULL);

            if (recvResult == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    printf("[ERROR] Initial WSARecv failed: %d\n", error);
                    free(recvData);
                    closesocket(newClient->socket);
                    DeleteCriticalSection(&newClient->cs);
                    free(newClient);
                    free(ioData);
                    break;
                }
                else {
                    printf("[Worker-%d] WSARecv pending (normal)\n", GetCurrentThreadId());
                    fflush(stdout);
                }
            }
            else {
                printf("[Worker-%d] WSARecv completed immediately with %d bytes\n",
                    GetCurrentThreadId(), bytesRecv);
                fflush(stdout);
            }

            // Post another AcceptEx
            printf("[Worker-%d] Creating new accept socket...\n", GetCurrentThreadId());
            fflush(stdout);

            SOCKET newAcceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                NULL, 0, WSA_FLAG_OVERLAPPED);

            if (newAcceptSocket == INVALID_SOCKET) {
                printf("[ERROR] Failed to create new accept socket: %d\n", WSAGetLastError());
                free(ioData);
                break;
            }



            // Reuse the IO data for next accept
            ZeroMemory(&ioData->overlapped, sizeof(OVERLAPPED));
            ioData->socket = newAcceptSocket;

            DWORD bytesReceived = 0;
            if (!lpfnAcceptEx(g_listenSocket, newAcceptSocket, ioData->buffer, 0,
                sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
                &bytesReceived, &ioData->overlapped)) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    printf("[ERROR] AcceptEx failed: %d\n", error);
                    closesocket(newAcceptSocket);
                    free(ioData);
                }
                else {
                    printf("[Worker-%d] New AcceptEx pending (normal)\n", GetCurrentThreadId());
                    fflush(stdout);
                }
            }

            printf("[Worker-%d] OP_ACCEPT processing completed\n", GetCurrentThreadId());
            fflush(stdout);
            break;
        }

        case OP_RECV: {
            ClientContext* client = ioData->client;

            printf("[Worker-%d] Processing OP_RECV, bytes=%d, client=%p, isWriteMode=%d\n",
                GetCurrentThreadId(), bytesTransferred, client, client ? client->isWriteMode : -1);
            fflush(stdout);

            if (client == NULL) {
                printf("[ERROR] Client context is NULL in OP_RECV!\n");
                free(ioData);
                break;
            }

            EnterCriticalSection(&client->cs);

            // Check if in write mode
            if (client->isWriteMode) {
                printf("[Worker-%d] OP_RECV in write mode - processing as write data\n", GetCurrentThreadId());
                fflush(stdout);

                // Process as write data
                for (DWORD i = 0; i < bytesTransferred; i++) {
                    char ch = ioData->buffer[i];

                    if (ch == '\n' || ch == '\r') {
                        if (client->recvPos > 0) {
                            client->recvBuffer[client->recvPos] = '\0';
                            printf("[Worker-%d] Write mode line received: '%s'\n",
                                GetCurrentThreadId(), client->recvBuffer);
                            fflush(stdout);

                            ProcessWriteLine(client, client->recvBuffer);
                            client->recvPos = 0;
                        }
                    }
                    else if (client->recvPos < BUF_SIZE - 1) {
                        client->recvBuffer[client->recvPos++] = ch;
                    }
                }

                LeaveCriticalSection(&client->cs);

                // Continue receiving if still in write mode
                if (client->isWriteMode) {
                    ZeroMemory(&ioData->overlapped, sizeof(OVERLAPPED));
                    ioData->wsaBuf.buf = ioData->buffer;
                    ioData->wsaBuf.len = BUF_SIZE;

                    DWORD flags = 0;
                    DWORD bytesRecv = 0;
                    WSARecv(client->socket, &ioData->wsaBuf, 1, &bytesRecv,
                        &flags, &ioData->overlapped, NULL);
                }
                else {
                    free(ioData);
                }
            }
            else {
                // Normal command mode
                // Print received data as hex for debugging
                printf("[Worker-%d] Received data (hex): ", GetCurrentThreadId());
                for (DWORD i = 0; i < bytesTransferred && i < 32; i++) {
                    printf("%02X ", (unsigned char)ioData->buffer[i]);
                }
                printf("\n");

                // Print received data as string
                printf("[Worker-%d] Received data (str): ", GetCurrentThreadId());
                for (DWORD i = 0; i < bytesTransferred; i++) {
                    if (ioData->buffer[i] >= 32 && ioData->buffer[i] <= 126) {
                        printf("%c", ioData->buffer[i]);
                    }
                    else {
                        printf("\\x%02X", (unsigned char)ioData->buffer[i]);
                    }
                }
                printf("\n");
                fflush(stdout);

                // Process received data
                BOOL processedCommand = FALSE;
                for (DWORD i = 0; i < bytesTransferred; i++) {
                    char ch = ioData->buffer[i];

                    if (ch == '\n' || ch == '\r') {
                        if (client->recvPos > 0) {
                            client->recvBuffer[client->recvPos] = '\0';
                            printf("[Worker-%d] Complete command line: '%s'\n",
                                GetCurrentThreadId(), client->recvBuffer);
                            fflush(stdout);

                            ParseCommand(client->recvBuffer, client->args, &client->argc);
                            ProcessCommand(client);
                            processedCommand = TRUE;
                            client->recvPos = 0;
                        }
                    }
                    else if (client->recvPos < BUF_SIZE - 1) {
                        client->recvBuffer[client->recvPos++] = ch;
                    }
                }

                // If no command was processed, send an echo to test connection
                if (!processedCommand && bytesTransferred > 0) {
                    printf("[Worker-%d] No complete command, sending echo test\n", GetCurrentThreadId());
                    char echoMsg[256];
                    sprintf(echoMsg, "[Echo] Received %d bytes\n", bytesTransferred);
                    SendData(client, echoMsg, -1);
                    fflush(stdout);
                }

                LeaveCriticalSection(&client->cs);

                // Continue receiving
                printf("[Worker-%d] Posting next WSARecv...\n", GetCurrentThreadId());
                fflush(stdout);

                ZeroMemory(&ioData->overlapped, sizeof(OVERLAPPED));
                ioData->wsaBuf.buf = ioData->buffer;
                ioData->wsaBuf.len = BUF_SIZE;

                DWORD flags = 0;
                DWORD bytesRecv = 0;
                if (WSARecv(client->socket, &ioData->wsaBuf, 1, &bytesRecv,
                    &flags, &ioData->overlapped, NULL) == SOCKET_ERROR) {
                    int error = WSAGetLastError();
                    if (error != WSA_IO_PENDING) {
                        printf("[ERROR] WSARecv failed: %d\n", error);
                        closesocket(client->socket);
                        DeleteCriticalSection(&client->cs);
                        for (int i = 0; i < 64 && client->args[i]; i++) {
                            free(client->args[i]);
                        }
                        free(client);
                        free(ioData);
                    }
                    else {
                        printf("[Worker-%d] WSARecv pending (normal)\n", GetCurrentThreadId());
                        fflush(stdout);
                    }
                }
            }
            break;
        }

        case OP_WRITE_WAIT: {
            // This case should not be reached anymore
            printf("[Worker-%d] WARNING: OP_WRITE_WAIT reached (deprecated)\n", GetCurrentThreadId());
            free(ioData);
            break;
        }

        case OP_SEND:
            printf("[Worker-%d] Send completed: %d bytes\n", GetCurrentThreadId(), bytesTransferred);
            fflush(stdout);

            if (ioData->client && strstr(ioData->buffer, "[Disconnected]"))
            {
                printf("[Worker-%d] Disconnection message sent, closing socket\n", GetCurrentThreadId());
                fflush(stdout);
                //소켓만 닫고, 클라이언트 리소스는 OP_RECV 0바이트 완료 쪽에서 처리해준다.
                closesocket(ioData->client->socket);
           }

            free(ioData);
            break;
        }
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <IP> <Port>\n", argv[0]);
        return 1;
    }

    // stdout 버퍼링 비활성화
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("[Server] Starting IOCP server...\n");

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[ERROR] WSAStartup failed\n");
        return 1;
    }

    // Initialize SRW lock
    InitializeSRWLock(&docsLock);

    // Initialize lock-free queues
    for (int i = 0; i < MAX_DOCS; i++) {
        for (int j = 0; j < MAX_SECTIONS; j++) {
            InitializeLockFreeQueue(&sectionQueues[i][j]);
        }
    }

    // Create IOCP
    g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (g_hIOCP == NULL) {
        printf("[ERROR] Failed to create IOCP: %d\n", GetLastError());
        return 1;
    }

    // Create listen socket
    g_listenSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,
        NULL, 0, WSA_FLAG_OVERLAPPED);

    if (g_listenSocket == INVALID_SOCKET) {
        printf("[ERROR] Failed to create listen socket: %d\n", WSAGetLastError());
        return 1;
    }

    // Bind and listen
    SOCKADDR_IN serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(atoi(argv[2]));

    if (inet_pton(AF_INET, argv[1], &serverAddr.sin_addr) <= 0) {
        printf("[ERROR] Invalid IP address: %s\n", argv[1]);
        return 1;
    }

    if (bind(g_listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printf("[ERROR] Bind failed: %d\n", WSAGetLastError());
        return 1;
    }

    if (listen(g_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        printf("[ERROR] Listen failed: %d\n", WSAGetLastError());
        return 1;
    }

    printf("[Server] Socket bound and listening on %s:%s\n", argv[1], argv[2]);

    // 실제 바인딩된 주소 확인
    SOCKADDR_IN actualAddr;
    int addrLen = sizeof(actualAddr);
    if (getsockname(g_listenSocket, (SOCKADDR*)&actualAddr, &addrLen) == 0) {
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &actualAddr.sin_addr, ipStr, sizeof(ipStr));
        printf("[Server] Actually listening on %s:%d\n", ipStr, ntohs(actualAddr.sin_port));
    }

    // Associate listen socket with IOCP
    if (CreateIoCompletionPort((HANDLE)g_listenSocket, g_hIOCP, 0, 0) == NULL) {
        printf("[ERROR] Failed to associate listen socket with IOCP: %d\n", GetLastError());
        return 1;
    }

    // Load AcceptEx function
    GUID guidAcceptEx = WSAID_ACCEPTEX;
    DWORD dwBytes;
    if (WSAIoctl(g_listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidAcceptEx, sizeof(guidAcceptEx), &lpfnAcceptEx, sizeof(lpfnAcceptEx),
        &dwBytes, NULL, NULL) == SOCKET_ERROR) {
        printf("[ERROR] Failed to load AcceptEx: %d\n", WSAGetLastError());
        return 1;
    }

    printf("[Server] AcceptEx loaded successfully\n");

    // Create worker threads
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    int numThreads = sysInfo.dwNumberOfProcessors * 2;
    if (numThreads > MAX_WORKERS) numThreads = MAX_WORKERS;

    printf("[Server] Creating %d worker threads...\n", numThreads);
    fflush(stdout);

    for (int i = 0; i < numThreads; i++) {
        unsigned int threadId;
        HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, WorkerThread, NULL, 0, &threadId);
        if (hThread == NULL) {
            printf("[ERROR] Failed to create worker thread %d\n", i);
        }
        else {
            printf("[Server] Worker thread %d created with ID %u\n", i, threadId);
            CloseHandle(hThread);  // 핸들은 닫아도 스레드는 계속 실행됨
        }
        fflush(stdout);
    }

    printf("[Server] All worker threads created\n");
    fflush(stdout);

    // Start accepting connections
    printf("[Server] Starting accept loop...\n");

    for (int i = 0; i < 10; i++) {
        SOCKET acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,
            NULL, 0, WSA_FLAG_OVERLAPPED);

        if (acceptSocket == INVALID_SOCKET) {
            printf("[ERROR] Failed to create accept socket: %d\n", WSAGetLastError());
            continue;
        }

        PER_IO_DATA* ioData = (PER_IO_DATA*)malloc(sizeof(PER_IO_DATA));
        ZeroMemory(&ioData->overlapped, sizeof(OVERLAPPED));
        ioData->operation = OP_ACCEPT;
        ioData->socket = acceptSocket;

        printf("[Server] Calling AcceptEx for socket %d (handle: %llu)...\n",
            i, (ULONGLONG)acceptSocket);
        fflush(stdout);

        DWORD bytesReceived;
        if (!lpfnAcceptEx(g_listenSocket, acceptSocket, ioData->buffer, 0,
            sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
            &bytesReceived, &ioData->overlapped)) {
            int error = WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                printf("[ERROR] AcceptEx failed: %d\n", error);
                closesocket(acceptSocket);
                free(ioData);
            }
            else {
                printf("[Server] AcceptEx pending on socket %d\n", i);
            }
        }
        else {
            printf("[Server] AcceptEx completed immediately on socket %d\n", i);
        }
    }

    printf("[Server] IOCP Server ready. Waiting for connections...\n");
    printf("[Server] Main thread going to sleep. Worker threads are handling connections.\n");

    // Wait forever
    Sleep(INFINITE);

    closesocket(g_listenSocket);
    CloseHandle(g_hIOCP);
    WSACleanup();

    return 0;
}
