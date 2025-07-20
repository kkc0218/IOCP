# Makefile for IOCP Document Server
# Compatible with MinGW-w64 and MSYS2

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99
SERVER_LIBS = -lws2_32 -lmswsock
CLIENT_LIBS = -lws2_32

# Debug flags
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c99 -DDEBUG

# Targets
SERVER_TARGET = server_iocp.exe
CLIENT_TARGET = client_iocp.exe
SERVER_SOURCE = server_iocp.c
CLIENT_SOURCE = client_iocp.c

# Default target
all: $(SERVER_TARGET) $(CLIENT_TARGET)

# Server target
$(SERVER_TARGET): $(SERVER_SOURCE)
	$(CC) $(CFLAGS) -o $@ $< $(SERVER_LIBS)

# Client target  
$(CLIENT_TARGET): $(CLIENT_SOURCE)
	$(CC) $(CFLAGS) -o $@ $< $(CLIENT_LIBS)

# Debug builds
debug: server-debug client-debug

server-debug: $(SERVER_SOURCE)
	$(CC) $(DEBUG_CFLAGS) -o server_iocp_debug.exe $< $(SERVER_LIBS)

client-debug: $(CLIENT_SOURCE)
	$(CC) $(DEBUG_CFLAGS) -o client_iocp_debug.exe $< $(CLIENT_LIBS)

# Clean target
clean:
	del /f *.exe *.obj *.pdb 2>nul || rm -f *.exe *.obj *.pdb

# Install target (copy to system PATH)
install: all
	@echo "Copy executables to desired location manually"
	@echo "Server: $(SERVER_TARGET)"
	@echo "Client: $(CLIENT_TARGET)"

# Test target
test: all
	@echo "Starting server in background..."
	@echo "Run 'make test-client' in another terminal to test"

test-client: $(CLIENT_TARGET)
	./$(CLIENT_TARGET)

# Help target
help:
	@echo "Available targets:"
	@echo "  all          - Build both server and client (default)"
	@echo "  server       - Build server only" 
	@echo "  client       - Build client only"
	@echo "  debug        - Build debug versions"
	@echo "  server-debug - Build server debug version"
	@echo "  client-debug - Build client debug version"
	@echo "  clean        - Remove built files"
	@echo "  install      - Show install instructions"
	@echo "  test         - Run basic test"
	@echo "  test-client  - Run client for testing"
	@echo "  help         - Show this help"

# Individual targets
server: $(SERVER_TARGET)
client: $(CLIENT_TARGET)

# Phony targets
.PHONY: all clean install test test-client help debug server-debug client-debug server client
