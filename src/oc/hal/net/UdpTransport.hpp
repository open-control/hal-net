#pragma once

/**
 * @file UdpTransport.hpp
 * @brief UDP-based frame transport for desktop platforms
 *
 * Provides frame transport over UDP for communication with oc-bridge
 * in "virtual controller" mode. Each UDP datagram = one complete frame.
 *
 * ## Architecture
 *
 * ```
 * Desktop App ──UDP:9001──► oc-bridge ──UDP:9000──► Bitwig Extension
 * ```
 *
 * ## Usage
 *
 * ```cpp
 * UdpConfig config;
 * config.host = "127.0.0.1";
 * config.port = 9001;  // oc-bridge virtual_port
 *
 * UdpTransport transport(config);
 * transport.init();
 *
 * // Set receive callback
 * transport.setOnReceive([](const uint8_t* data, size_t len) {
 *     // Handle incoming frame
 * });
 *
 * // In main loop
 * transport.update();  // Poll for incoming frames
 *
 * // Send a frame
 * transport.send(frameData, frameLen);
 * ```
 *
 * ## Platform Notes
 *
 * - Windows: Uses Winsock2 (ws2_32.lib required)
 * - Linux/macOS: Uses POSIX sockets
 * - No COBS encoding (oc-bridge Virtual mode uses RawCodec)
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <oc/type/Result.hpp>
#include <oc/interface/ITransport.hpp>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

namespace oc::hal::net {

/**
 * @brief Configuration for UdpTransport
 */
struct UdpConfig {
    /// Host address to connect to (default: localhost)
    std::string host = "127.0.0.1";
    
    /// Port to send/receive on (default: oc-bridge virtual_port)
    uint16_t port = 9001;
    
    /// Receive buffer size in bytes
    size_t recvBufferSize = 4096;
};

/**
 * @brief UDP-based frame transport for oc-bridge communication
 *
 * Implements IFrameTransport using UDP sockets. Designed for use with
 * oc-bridge in Virtual mode where each datagram is a complete frame.
 *
 * Features:
 * - Non-blocking socket for use in game loops
 * - No framing overhead (UDP datagrams are naturally delimited)
 * - Cross-platform (Windows/Linux/macOS)
 */
class UdpTransport : public interface::ITransport {
public:
    UdpTransport();
    explicit UdpTransport(const UdpConfig& config);
    ~UdpTransport() override;

    // Non-copyable
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // Moveable
    UdpTransport(UdpTransport&& other) noexcept;
    UdpTransport& operator=(UdpTransport&& other) noexcept;

    /**
     * @brief Initialize the UDP socket
     *
     * Creates a non-blocking UDP socket and binds it for receiving.
     * On Windows, initializes Winsock if needed.
     *
     * @return Result<void> - ok() on success, err() on failure
     */
    oc::type::Result<void> init() override;

    /**
     * @brief Poll for incoming frames
     *
     * Checks for available data on the socket and dispatches
     * complete frames via the receive callback.
     * Non-blocking - returns immediately if no data available.
     */
    void update() override;

    /**
     * @brief Send a frame over UDP
     *
     * Sends the data as a single UDP datagram to the configured
     * host:port. No framing is added (raw send).
     *
     * @param data Pointer to frame data
     * @param length Number of bytes to send
     */
    void send(const uint8_t* data, size_t length) override;

    /**
     * @brief Set callback for received frames
     *
     * @param cb Callback invoked when a complete frame is received
     */
    void setOnReceive(ReceiveCallback cb) override;

    /**
     * @brief Check if transport is initialized and ready
     */
    bool isReady() const override { return initialized_; }

private:
    void cleanup();

    UdpConfig config_;
    ReceiveCallback onReceive_;
    bool initialized_ = false;

#ifdef _WIN32
    SOCKET socket_ = INVALID_SOCKET;
    static bool winsockInitialized_;
    static int winsockRefCount_;
#else
    int socket_ = -1;
#endif

    struct sockaddr_in destAddr_;
    std::vector<uint8_t> recvBuffer_;
};

}  // namespace oc::hal::net
