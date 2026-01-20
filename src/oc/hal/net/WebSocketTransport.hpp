#pragma once

/**
 * @file WebSocketTransport.hpp
 * @brief WebSocket-based message transport for Emscripten/WASM builds
 *
 * Provides frame transport over WebSocket for communication with oc-bridge
 * in browser environments. Features automatic reconnection with exponential
 * backoff and message buffering during disconnection.
 *
 * ## Architecture
 *
 * ```
 * Browser App ──WebSocket:9002──► oc-bridge ──► Bitwig Extension
 * ```
 *
 * ## Usage
 *
 * ```cpp
 * WebSocketConfig config;
 * config.url = "ws://127.0.0.1:9002";
 *
 * WebSocketTransport transport(config);
 * transport.init();
 *
 * // Set receive callback
 * transport.setOnReceive([](const uint8_t* data, size_t len) {
 *     // Handle incoming frame
 * });
 *
 * // In main loop
 * transport.update();  // Handles reconnection timing
 *
 * // Send a frame (buffered if not connected)
 * transport.send(frameData, frameLen);
 * ```
 *
 * ## Platform Notes
 *
 * - Only available on Emscripten builds (__EMSCRIPTEN__ defined)
 * - Uses Emscripten WebSocket API (<emscripten/websocket.h>)
 * - Requires linking with -lwebsocket.js
 * - Callbacks are async (triggered by browser event loop)
 */

#ifdef __EMSCRIPTEN__

#include <emscripten/websocket.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <oc/types/Result.hpp>
#include <oc/interface/ITransport.hpp>

namespace oc::hal::net {

/**
 * @brief Configuration for WebSocketTransport
 */
struct WebSocketConfig {
    /// WebSocket server URL (oc-bridge)
    std::string url = "ws://127.0.0.1:9002";

    /// Enable automatic reconnection on disconnect
    bool autoReconnect = true;

    /// Initial delay between reconnection attempts (ms)
    uint32_t reconnectDelayMs = 1000;

    /// Maximum reconnection delay (exponential backoff cap)
    uint32_t reconnectMaxDelayMs = 30000;

    /// Maximum pending messages to buffer (0 = unlimited)
    size_t maxPendingMessages = 100;
};

/**
 * @brief WebSocket-based message transport for oc-bridge communication
 *
 * Implements IFrameTransport using Emscripten WebSocket API.
 * Designed for use with oc-bridge in browser environments.
 *
 * Features:
 * - Automatic reconnection with exponential backoff
 * - Message buffering during disconnection
 * - Async callbacks (event-driven, not polling)
 * - Binary message support
 */
class WebSocketTransport : public interface::ITransport {
public:
    WebSocketTransport();
    explicit WebSocketTransport(const WebSocketConfig& config);
    ~WebSocketTransport() override;

    // Non-copyable, non-movable (due to C callback pointers)
    WebSocketTransport(const WebSocketTransport&) = delete;
    WebSocketTransport& operator=(const WebSocketTransport&) = delete;
    WebSocketTransport(WebSocketTransport&&) = delete;
    WebSocketTransport& operator=(WebSocketTransport&&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // IFrameTransport interface
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Initialize WebSocket connection
     *
     * Checks WebSocket support and initiates connection.
     * Connection is async - use isReady() to check status.
     *
     * @return Result<void> - ok() on success, err() if WebSocket not supported
     */
    oc::Result<void> init() override;

    /**
     * @brief Handle reconnection timing
     *
     * Must be called regularly in the main loop.
     * Note: Message receiving is handled by browser callbacks, not here.
     */
    void update() override;

    /**
     * @brief Send a frame over WebSocket
     *
     * If connected, sends immediately.
     * If disconnected, buffers the message (up to maxPendingMessages).
     *
     * @param data Pointer to frame data
     * @param length Number of bytes to send
     */
    void send(const uint8_t* data, size_t length) override;

    /**
     * @brief Set callback for received frames
     *
     * @param cb Callback invoked when a binary message is received
     */
    void setOnReceive(ReceiveCallback cb) override;

    /**
     * @brief Check if WebSocket is connected and ready
     *
     * @return true if connected, false if connecting/disconnected
     */
    bool isReady() const override;

private:
    /// Connection states
    enum class State {
        Disconnected,  ///< Not connected, may be waiting to reconnect
        Connecting,    ///< Connection in progress
        Connected      ///< Connected and ready
    };

    // Emscripten C-style callbacks (static)
    static EM_BOOL onOpen(int eventType, const EmscriptenWebSocketOpenEvent* event, void* userData);
    static EM_BOOL onMessage(int eventType, const EmscriptenWebSocketMessageEvent* event, void* userData);
    static EM_BOOL onClose(int eventType, const EmscriptenWebSocketCloseEvent* event, void* userData);
    static EM_BOOL onError(int eventType, const EmscriptenWebSocketErrorEvent* event, void* userData);

    void connect();
    void flushPendingMessages();
    void scheduleReconnect();

    WebSocketConfig config_;
    EMSCRIPTEN_WEBSOCKET_T socket_ = 0;
    State state_ = State::Disconnected;

    ReceiveCallback onReceive_;

    // Message buffering during disconnection
    std::vector<std::vector<uint8_t>> pendingMessages_;

    // Reconnection timing (uses oc::time::millis())
    uint32_t lastAttemptMs_ = 0;
    uint32_t currentDelayMs_ = 0;
    uint32_t reconnectAttempts_ = 0;
};

}  // namespace oc::hal::net

#endif  // __EMSCRIPTEN__
