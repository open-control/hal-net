/**
 * @file WebSocketTransport.cpp
 * @brief WebSocket transport implementation for Emscripten
 */

#ifdef __EMSCRIPTEN__

#include "WebSocketTransport.hpp"

#include <algorithm>

#include <oc/log/Log.hpp>
#include <oc/time/Time.hpp>

namespace oc::hal::net {

// ═══════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════

WebSocketTransport::WebSocketTransport()
    : WebSocketTransport(WebSocketConfig{}) {}

WebSocketTransport::WebSocketTransport(const WebSocketConfig& config)
    : config_(config)
    , currentDelayMs_(config.reconnectDelayMs) {}

WebSocketTransport::~WebSocketTransport() {
    if (socket_ > 0) {
        emscripten_websocket_close(socket_, 1000, "destructor");
        emscripten_websocket_delete(socket_);
        socket_ = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// IFrameTransport Implementation
// ═══════════════════════════════════════════════════════════════════════════

core::Result<void> WebSocketTransport::init() {
    if (!emscripten_websocket_is_supported()) {
        OC_LOG_ERROR("[WebSocket] Not supported in this browser");
        return core::Result<void>::err(core::ErrorCode::INVALID_STATE);
    }

    OC_LOG_INFO("[WebSocket] Connecting to {}", config_.url.c_str());
    connect();
    return core::Result<void>::ok();
}

void WebSocketTransport::update() {
    // Reconnection logic (messages are handled by async callbacks)
    if (state_ == State::Disconnected && config_.autoReconnect) {
        uint32_t now = oc::time::millis();
        if (now - lastAttemptMs_ >= currentDelayMs_) {
            OC_LOG_INFO("[WebSocket] Attempting reconnect (attempt {})...", 
                        reconnectAttempts_ + 1);
            connect();
            lastAttemptMs_ = now;
        }
    }
}

void WebSocketTransport::send(const uint8_t* data, size_t length) {
    if (state_ == State::Connected) {
        // Send immediately
        EMSCRIPTEN_RESULT result = emscripten_websocket_send_binary(
            socket_, 
            const_cast<void*>(static_cast<const void*>(data)), 
            static_cast<uint32_t>(length)
        );
        if (result != EMSCRIPTEN_RESULT_SUCCESS) {
            OC_LOG_WARN("[WebSocket] Send failed: {}", result);
        }
    } else {
        // Buffer for later
        if (config_.maxPendingMessages == 0 || 
            pendingMessages_.size() < config_.maxPendingMessages) {
            pendingMessages_.emplace_back(data, data + length);
        } else {
            // Drop oldest message to make room
            pendingMessages_.erase(pendingMessages_.begin());
            pendingMessages_.emplace_back(data, data + length);
            OC_LOG_WARN("[WebSocket] Buffer full, dropped oldest message");
        }
    }
}

void WebSocketTransport::setOnReceive(ReceiveCallback cb) {
    onReceive_ = std::move(cb);
}

bool WebSocketTransport::isReady() const {
    return state_ == State::Connected;
}

// ═══════════════════════════════════════════════════════════════════════════
// Connection Management
// ═══════════════════════════════════════════════════════════════════════════

void WebSocketTransport::connect() {
    // Clean up any existing socket
    if (socket_ > 0) {
        emscripten_websocket_delete(socket_);
        socket_ = 0;
    }

    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = config_.url.c_str();
    attr.protocols = nullptr;  // Binary by default
    attr.createOnMainThread = EM_TRUE;

    socket_ = emscripten_websocket_new(&attr);
    if (socket_ <= 0) {
        OC_LOG_ERROR("[WebSocket] Failed to create socket");
        scheduleReconnect();
        return;
    }

    state_ = State::Connecting;

    // Setup callbacks
    emscripten_websocket_set_onopen_callback(socket_, this, onOpen);
    emscripten_websocket_set_onmessage_callback(socket_, this, onMessage);
    emscripten_websocket_set_onclose_callback(socket_, this, onClose);
    emscripten_websocket_set_onerror_callback(socket_, this, onError);
}

void WebSocketTransport::flushPendingMessages() {
    if (pendingMessages_.empty()) return;

    OC_LOG_INFO("[WebSocket] Flushing {} pending messages", pendingMessages_.size());

    for (const auto& msg : pendingMessages_) {
        emscripten_websocket_send_binary(
            socket_, 
            const_cast<void*>(static_cast<const void*>(msg.data())), 
            static_cast<uint32_t>(msg.size())
        );
    }
    pendingMessages_.clear();
}

void WebSocketTransport::scheduleReconnect() {
    if (!config_.autoReconnect) return;

    // Exponential backoff
    currentDelayMs_ = std::min(currentDelayMs_ * 2, config_.reconnectMaxDelayMs);
    lastAttemptMs_ = oc::time::millis();
    reconnectAttempts_++;

    OC_LOG_INFO("[WebSocket] Reconnect scheduled in {}ms", currentDelayMs_);
}

// ═══════════════════════════════════════════════════════════════════════════
// Static Emscripten Callbacks
// ═══════════════════════════════════════════════════════════════════════════

EM_BOOL WebSocketTransport::onOpen(int /*eventType*/, 
                                    const EmscriptenWebSocketOpenEvent* /*event*/, 
                                    void* userData) {
    auto* self = static_cast<WebSocketTransport*>(userData);

    OC_LOG_INFO("[WebSocket] Connected to {}", self->config_.url.c_str());
    self->state_ = State::Connected;
    
    // Reset backoff on successful connection
    self->currentDelayMs_ = self->config_.reconnectDelayMs;
    self->reconnectAttempts_ = 0;

    // Send any buffered messages
    self->flushPendingMessages();

    return EM_TRUE;
}

EM_BOOL WebSocketTransport::onMessage(int /*eventType*/, 
                                       const EmscriptenWebSocketMessageEvent* event, 
                                       void* userData) {
    auto* self = static_cast<WebSocketTransport*>(userData);

    // Only handle binary messages (not text)
    if (!event->isText && self->onReceive_) {
        self->onReceive_(event->data, static_cast<size_t>(event->numBytes));
    }

    return EM_TRUE;
}

EM_BOOL WebSocketTransport::onClose(int /*eventType*/, 
                                     const EmscriptenWebSocketCloseEvent* event, 
                                     void* userData) {
    auto* self = static_cast<WebSocketTransport*>(userData);

    OC_LOG_WARN("[WebSocket] Closed (code={}, reason={})", 
                event->code, event->reason[0] ? event->reason : "");
    
    self->state_ = State::Disconnected;
    self->scheduleReconnect();

    return EM_TRUE;
}

EM_BOOL WebSocketTransport::onError(int /*eventType*/, 
                                     const EmscriptenWebSocketErrorEvent* /*event*/, 
                                     void* userData) {
    auto* self = static_cast<WebSocketTransport*>(userData);

    OC_LOG_ERROR("[WebSocket] Error occurred");
    // Note: onClose will be called after this by the browser

    return EM_TRUE;
}

}  // namespace oc::hal::net

#endif  // __EMSCRIPTEN__
