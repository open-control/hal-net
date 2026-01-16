#include "UdpTransport.hpp"

#include <oc/log/Log.hpp>

#ifdef _WIN32
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <errno.h>
#endif

namespace oc::hal::net {

// Static members for Winsock reference counting (Windows only)
#ifdef _WIN32
bool UdpTransport::winsockInitialized_ = false;
int UdpTransport::winsockRefCount_ = 0;
#endif

UdpTransport::UdpTransport() : UdpTransport(UdpConfig{}) {}

UdpTransport::UdpTransport(const UdpConfig& config)
    : config_(config) {
    recvBuffer_.resize(config_.recvBufferSize);
}

UdpTransport::~UdpTransport() {
    cleanup();
}

UdpTransport::UdpTransport(UdpTransport&& other) noexcept
    : config_(std::move(other.config_))
    , onReceive_(std::move(other.onReceive_))
    , initialized_(other.initialized_)
    , socket_(other.socket_)
    , destAddr_(other.destAddr_)
    , recvBuffer_(std::move(other.recvBuffer_)) {
#ifdef _WIN32
    other.socket_ = INVALID_SOCKET;
#else
    other.socket_ = -1;
#endif
    other.initialized_ = false;
}

UdpTransport& UdpTransport::operator=(UdpTransport&& other) noexcept {
    if (this != &other) {
        cleanup();
        config_ = std::move(other.config_);
        onReceive_ = std::move(other.onReceive_);
        initialized_ = other.initialized_;
        socket_ = other.socket_;
        destAddr_ = other.destAddr_;
        recvBuffer_ = std::move(other.recvBuffer_);
#ifdef _WIN32
        other.socket_ = INVALID_SOCKET;
#else
        other.socket_ = -1;
#endif
        other.initialized_ = false;
    }
    return *this;
}

core::Result<void> UdpTransport::init() {
    if (initialized_) {
        return core::Result<void>::ok();
    }

#ifdef _WIN32
    // Initialize Winsock (reference counted)
    if (!winsockInitialized_) {
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            OC_LOG_ERROR("UDP: WSAStartup failed: {}", result);
            return core::Result<void>::err(core::ErrorCode::HARDWARE_INIT_FAILED);
        }
        winsockInitialized_ = true;
    }
    winsockRefCount_++;
#endif

    // Create UDP socket
#ifdef _WIN32
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        OC_LOG_ERROR("UDP: Failed to create socket: {}", WSAGetLastError());
        return core::Result<void>::err(core::ErrorCode::HARDWARE_INIT_FAILED);
    }
#else
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) {
        OC_LOG_ERROR("UDP: Failed to create socket: {}", errno);
        return core::Result<void>::err(core::ErrorCode::HARDWARE_INIT_FAILED);
    }
#endif

    // Set non-blocking mode
#ifdef _WIN32
    u_long nonBlocking = 1;
    if (ioctlsocket(socket_, FIONBIO, &nonBlocking) != 0) {
        OC_LOG_ERROR("UDP: Failed to set non-blocking: {}", WSAGetLastError());
        cleanup();
        return core::Result<void>::err(core::ErrorCode::HARDWARE_INIT_FAILED);
    }
#else
    int flags = fcntl(socket_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
        OC_LOG_ERROR("UDP: Failed to set non-blocking: {}", errno);
        cleanup();
        return core::Result<void>::err(core::ErrorCode::HARDWARE_INIT_FAILED);
    }
#endif

    // Bind to receive responses (use any available port)
    struct sockaddr_in localAddr;
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = 0;  // Let OS choose port

    if (bind(socket_, reinterpret_cast<struct sockaddr*>(&localAddr), sizeof(localAddr)) < 0) {
#ifdef _WIN32
        OC_LOG_ERROR("UDP: Bind failed: {}", WSAGetLastError());
#else
        OC_LOG_ERROR("UDP: Bind failed: {}", errno);
#endif
        cleanup();
        return core::Result<void>::err(core::ErrorCode::HARDWARE_INIT_FAILED);
    }

    // Setup destination address
    memset(&destAddr_, 0, sizeof(destAddr_));
    destAddr_.sin_family = AF_INET;
    destAddr_.sin_port = htons(config_.port);
    
#ifdef _WIN32
    inet_pton(AF_INET, config_.host.c_str(), &destAddr_.sin_addr);
#else
    inet_pton(AF_INET, config_.host.c_str(), &destAddr_.sin_addr);
#endif

    initialized_ = true;
    OC_LOG_INFO("UDP: Initialized, target {}:{}", config_.host.c_str(), config_.port);
    return core::Result<void>::ok();
}

void UdpTransport::update() {
    if (!initialized_ || !onReceive_) {
        return;
    }

    // Non-blocking receive
    struct sockaddr_in senderAddr;
#ifdef _WIN32
    int addrLen = sizeof(senderAddr);
    int bytesReceived = recvfrom(
        socket_,
        reinterpret_cast<char*>(recvBuffer_.data()),
        static_cast<int>(recvBuffer_.size()),
        0,
        reinterpret_cast<struct sockaddr*>(&senderAddr),
        &addrLen
    );

    if (bytesReceived > 0) {
        onReceive_(recvBuffer_.data(), static_cast<size_t>(bytesReceived));
    }
    // WSAEWOULDBLOCK is expected for non-blocking sockets with no data
#else
    socklen_t addrLen = sizeof(senderAddr);
    ssize_t bytesReceived = recvfrom(
        socket_,
        recvBuffer_.data(),
        recvBuffer_.size(),
        0,
        reinterpret_cast<struct sockaddr*>(&senderAddr),
        &addrLen
    );

    if (bytesReceived > 0) {
        onReceive_(recvBuffer_.data(), static_cast<size_t>(bytesReceived));
    }
    // EAGAIN/EWOULDBLOCK is expected for non-blocking sockets with no data
#endif
}

void UdpTransport::send(const uint8_t* data, size_t length) {
    if (!initialized_) {
        return;
    }

#ifdef _WIN32
    int bytesSent = sendto(
        socket_,
        reinterpret_cast<const char*>(data),
        static_cast<int>(length),
        0,
        reinterpret_cast<const struct sockaddr*>(&destAddr_),
        sizeof(destAddr_)
    );

    if (bytesSent < 0) {
        OC_LOG_WARN("UDP: Send failed: {}", WSAGetLastError());
    }
#else
    ssize_t bytesSent = sendto(
        socket_,
        data,
        length,
        0,
        reinterpret_cast<const struct sockaddr*>(&destAddr_),
        sizeof(destAddr_)
    );

    if (bytesSent < 0) {
        OC_LOG_WARN("UDP: Send failed: {}", errno);
    }
#endif
}

void UdpTransport::setOnReceive(ReceiveCallback cb) {
    onReceive_ = std::move(cb);
}

void UdpTransport::cleanup() {
#ifdef _WIN32
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    // Cleanup Winsock (reference counted)
    if (winsockInitialized_ && --winsockRefCount_ == 0) {
        WSACleanup();
        winsockInitialized_ = false;
    }
#else
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
#endif
    initialized_ = false;
}

}  // namespace oc::hal::net
