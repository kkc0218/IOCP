# Contributing to IOCP Document Server

Thank you for your interest in contributing to the IOCP Document Server project! This document provides guidelines and information for contributors.

## Getting Started

### Prerequisites
- Windows 10/11 (IOCP is Windows-specific)
- Visual Studio 2019+ with C++ tools OR MinGW-w64
- Git for version control
- Basic knowledge of C programming and Windows API

### Development Setup
1. Fork the repository
2. Clone your fork:
   ```cmd
   git clone https://github.com/yourusername/iocp-document-server.git
   cd iocp-document-server
   ```
3. Build the project:
   ```cmd
   build.bat
   ```
   Or using CMake:
   ```cmd
   mkdir build && cd build
   cmake ..
   cmake --build .
   ```

##  How to Contribute

### Reporting Issues
- Check existing issues before creating a new one
- Use the issue template if available
- Provide detailed information:
  - Windows version
  - Compiler version
  - Steps to reproduce
  - Expected vs actual behavior
  - Relevant logs or error messages

### Submitting Changes
1. Create a feature branch:
   ```cmd
   git checkout -b feature/your-feature-name
   ```
2. Make your changes
3. Test thoroughly
4. Commit with clear messages:
   ```cmd
   git commit -m "Add: Brief description of changes"
   ```
5. Push to your fork:
   ```cmd
   git push origin feature/your-feature-name
   ```
6. Create a Pull Request

## Areas for Contribution

### High Priority
- **Linux Port**: Adapt IOCP concepts to epoll/io_uring
- **Performance Optimizations**: Memory usage, thread efficiency
- **Unit Tests**: Comprehensive test suite
- **Documentation**: API documentation, tutorials

### Medium Priority
- **New Features**: Additional command types, file operations
- **Security**: Input validation, DoS protection
- **Monitoring**: Metrics collection, health checks
- **CLI Tools**: Management utilities

### Low Priority
- **Language Bindings**: Python, C#, etc.
- **GUI Client**: Windows forms/WPF client
- **Configuration**: More flexible config options

## Coding Standards

### C Code Style
```c
// Use K&R style braces
if (condition) {
    // 4-space indentation
    function_call();
}

// Function names: lowercase with underscores
void process_client_data(ClientContext* client);

// Constants: UPPERCASE with underscores
#define MAX_BUFFER_SIZE 2048

// Structures: PascalCase
typedef struct {
    int value;
    char* data;
} DataStructure;
```

### Naming Conventions
- **Functions**: `snake_case`
- **Variables**: `snake_case` or `camelCase` (be consistent)
- **Constants**: `UPPER_SNAKE_CASE`
- **Types**: `PascalCase`
- **Macros**: `UPPER_SNAKE_CASE`

### Comments
```c
/**
 * Brief function description
 * 
 * @param client Client context structure
 * @param data Data buffer to process
 * @return Success/failure status
 */
BOOL ProcessClientData(ClientContext* client, const char* data);

// Single-line comments for brief explanations
int retry_count = 0;  // Maximum retries before giving up
```

### Error Handling
```c
// Always check return values
if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    printf("[ERROR] WSAStartup failed\n");
    return 1;
}

// Use consistent error logging
printf("[ERROR] Operation failed: %d\n", GetLastError());
```

## Testing Guidelines

### Manual Testing
1. Build both debug and release versions
2. Test basic operations:
   - Server startup/shutdown
   - Client connections
   - Document operations (create, write, read)
   - Concurrent access
   - Error conditions

### Performance Testing
```cmd
# Test with multiple clients
for /l %i in (1,1,10) do start client_iocp.exe

# Monitor resource usage
tasklist | findstr iocp
```

### Memory Testing
- Use Application Verifier (Windows SDK)
- Check for memory leaks with debug builds
- Verify proper cleanup on client disconnect

## Debugging

### Debug Builds
```cmd
# Visual Studio
cl /Zi /DEBUG server_iocp.c /link ws2_32.lib mswsock.lib

# MinGW
gcc -g -O0 -DDEBUG server_iocp.c -o server_debug.exe -lws2_32 -lmswsock
```

### Common Debug Scenarios
- **Socket Errors**: Check WSAGetLastError()
- **IOCP Issues**: Verify completion port association
- **Memory Issues**: Use debug heap, Application Verifier
- **Race Conditions**: Add logging, use thread sanitizers

### Debug Output
The server includes extensive debug logging. Enable with:
```c
#define DEBUG_VERBOSE 1  // Add to compiler flags or source
```

## Documentation

### Code Documentation
- Document all public functions with Doxygen-style comments
- Explain complex algorithms and lock-free operations
- Include usage examples for new features

### README Updates
- Update feature lists for new functionality
- Add new compilation instructions if needed
- Update performance benchmarks

## Code Review Process

### Pull Request Requirements
- [ ] Code follows style guidelines
- [ ] All tests pass
- [ ] No memory leaks detected
- [ ] Documentation updated
- [ ] Performance impact assessed
- [ ] Windows compatibility verified

### Review Checklist
- **Functionality**: Does it work as intended?
- **Performance**: Any negative impact on throughput/latency?
- **Security**: Proper input validation, no buffer overflows?
- **Memory Management**: Proper allocation/deallocation?
- **Thread Safety**: Correct use of synchronization primitives?
- **Error Handling**: Comprehensive error checking?

## Architecture Guidelines

### Lock-Free Programming
When modifying lock-free code:
- Understand ABA problems and solutions
- Use atomic operations correctly
- Test thoroughly under high concurrency
- Document memory ordering requirements

### IOCP Best Practices
- Minimize context switches
- Reuse I/O data structures
- Handle completion errors gracefully
- Maintain proper socket lifecycle

### Memory Management
- Prefer stack allocation when possible
- Use memory pools for frequent allocations
- Always pair malloc/free, new/delete
- Check for null pointers

## Getting Help

### Communication Channels
- **Issues**: For bugs and feature requests
- **Discussions**: For questions and general discussion
- **Wiki**: For detailed documentation

### Maintainer Contact
- Create an issue for technical questions
- Tag maintainers in pull requests: @maintainer-username

## License

By contributing to this project, you agree that your contributions will be licensed under the MIT License.

## Recognition

Contributors will be:
- Listed in the project's contributor list
- Mentioned in release notes for significant contributions
- Credited in documentation for major features

Thank you for helping make IOCP Document Server better!
