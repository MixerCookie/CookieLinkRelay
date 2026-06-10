// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026

#include <juce_core/juce_core.h>
#include "RelayProtocol.h"
#include "aoo/aoo_net.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstring>
#if defined(__APPLE__)
#include <sys/socket.h>
#elif JUCE_WINDOWS
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif
#include <deque>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(COOKIELINK_RELAY_USE_OPENSSL)
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

using namespace cookielink::relay;

namespace {

static void disableSigPipe(juce::StreamingSocket& sock)
{
#if defined(__APPLE__)
    const int fd = sock.getRawSocketHandle();
    if (fd >= 0) {
        int set = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
    }
#else
    ignoreUnused(sock);
#endif
}

static void tuneRelaySocket(juce::StreamingSocket& sock)
{
    const int fd = sock.getRawSocketHandle();
    if (fd < 0) {
        return;
    }

    int set = 1;

#if defined(__APPLE__)
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
#endif

#if defined(TCP_NODELAY)
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&set), sizeof(set));
#endif
}

static int relaySocketWaitMs(int configuredMs)
{
    return configuredMs < 0 ? 200 : juce::jmax(1, configuredMs);
}

static void relaySocketWait(juce::StreamingSocket& socket, bool forRead, int waitMs)
{
    socket.waitUntilReady(forRead, relaySocketWaitMs(waitMs));
}

static juce::String formatErrno(int errnum)
{
    if (errnum == 0) {
        return {};
    }
    return " errno=" + juce::String(errnum) + " (" + juce::String(std::strerror(errnum)) + ")";
}

static bool isTransientSocketErr(int errnum)
{
    return errnum == EINTR || errnum == EAGAIN || errnum == EWOULDBLOCK || errnum == EINPROGRESS;
}

static juce::String relayTimestamp()
{
    return juce::Time::getCurrentTime().toString(true, true, true, true);
}

static juce::String relayMessageName(MessageType type)
{
    switch (type) {
        case MsgHello: return "hello";
        case MsgWelcome: return "welcome";
        case MsgJoinGroup: return "join-group";
        case MsgJoinResult: return "join-result";
        case MsgPeerJoin: return "peer-join";
        case MsgPeerLeave: return "peer-leave";
        case MsgData: return "data";
        case MsgError: return "error";
        case MsgLeaveGroup: return "leave-group";
        case MsgPublicGroupsRequest: return "public-groups-request";
        case MsgPublicGroupsUpdate: return "public-groups-update";
        default: break;
    }
    return "unknown";
}

static void writeU16(juce::MemoryOutputStream& out, uint16_t value)
{
    const uint8_t bytes[2] = {
        static_cast<uint8_t>((value >> 8) & 0xff),
        static_cast<uint8_t>(value & 0xff)
    };
    out.write(bytes, 2);
}

static void writeU32(juce::MemoryOutputStream& out, uint32_t value)
{
    const uint8_t bytes[4] = {
        static_cast<uint8_t>((value >> 24) & 0xff),
        static_cast<uint8_t>((value >> 16) & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff),
        static_cast<uint8_t>(value & 0xff)
    };
    out.write(bytes, 4);
}

static bool readU16(const uint8_t* data, int size, int& offset, uint16_t& value)
{
    if (offset + 2 > size) {
        return false;
    }
    value = (static_cast<uint16_t>(data[offset]) << 8)
        | static_cast<uint16_t>(data[offset + 1]);
    offset += 2;
    return true;
}

static bool readU32(const uint8_t* data, int size, int& offset, uint32_t& value)
{
    if (offset + 4 > size) {
        return false;
    }
    value = (static_cast<uint32_t>(data[offset]) << 24)
        | (static_cast<uint32_t>(data[offset + 1]) << 16)
        | (static_cast<uint32_t>(data[offset + 2]) << 8)
        | static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    return true;
}

static void writeString(juce::MemoryOutputStream& out, const juce::String& text)
{
    auto bytes = text.toRawUTF8();
    auto len = static_cast<uint16_t>(std::min<size_t>(std::strlen(bytes), 0xffff));
    writeU16(out, len);
    out.write(bytes, len);
}

static bool readString(const uint8_t* data, int size, int& offset, juce::String& text)
{
    uint16_t len = 0;
    if (!readU16(data, size, offset, len)) {
        return false;
    }
    if (offset + len > size) {
        return false;
    }
    text = juce::String::fromUTF8(reinterpret_cast<const char*>(data + offset), len);
    offset += len;
    return true;
}

struct Client;

using FramePtr = std::shared_ptr<juce::MemoryBlock>;

struct OutgoingFrame {
    FramePtr frame;
    size_t size = 0;
    bool droppable = false;
    double enqueuedMs = 0.0;
};

struct GroupState {
    juce::String password;
    bool isPublic = false;
    std::unordered_set<uint32_t> members;
};

struct Client {
    explicit Client(std::unique_ptr<juce::StreamingSocket> sock)
        : socket(std::move(sock))
    {}

    std::unique_ptr<juce::StreamingSocket> socket;
    juce::CriticalSection writeLock;
    std::thread thread;
    std::thread sendThread;

    uint32_t id = 0;
    juce::String user;
    juce::String group;
    bool watchPublicGroups = false;

    std::mutex sendMutex;
    std::condition_variable sendCv;
    std::deque<OutgoingFrame> sendQueue;
    size_t sendQueueBytes = 0;
    std::atomic<bool> running { true };
    std::atomic<uint64_t> sentBytes { 0 };
    std::atomic<uint64_t> droppedFrames { 0 };
    std::atomic<uint32_t> lastDropLogMs { 0 };
    double lastWriteBlockMs = 0.0;
    double avgWriteBlockMs = 0.0;
    double maxWriteBlockMs = 0.0;
    int writeBlockSamples = 0;

#if defined(COOKIELINK_RELAY_USE_OPENSSL)
    SSL* ssl = nullptr;
#endif
};


class AooUdpServer {
public:
    struct Options {
        int port = 10998;
        bool enabled = true;
        juce::String logDir;
    };

    explicit AooUdpServer(Options opts)
        : options(std::move(opts))
    {
    }

    ~AooUdpServer()
    {
        stop();
    }

    bool start()
    {
        if (!options.enabled) {
            return true;
        }

        if (options.logDir.isNotEmpty()) {
            openLogger();
        }
        int32_t err = 0;
        {
            std::lock_guard<std::mutex> lock(serverMutex);
            server.reset(aoo::net::iserver::create(options.port, &err));
            if (err != 0 || !server) {
                std::cerr << "AOO UDP server failed to listen on port " << options.port
                          << " (error " << err << ")\n";
                server.reset();
                return false;
            }
        }

        running.store(true);
        serverThread = std::thread([this]() { runServer(); });
        eventThread = std::thread([this]() { eventLoop(); });

        juce::String msg;
        msg << "ServerStart," << options.port;
        logEvent(msg);
        std::cout << "CookieLink AOO UDP server listening on " << options.port << "\n";
        return true;
    }

    void stop()
    {
        running.store(false);
        {
            std::lock_guard<std::mutex> lock(serverMutex);
            if (server) {
                server->quit();
            }
        }

        if (eventThread.joinable()) {
            eventThread.join();
        }
        if (serverThread.joinable()) {
            serverThread.join();
        }

        std::lock_guard<std::mutex> lock(serverMutex);
        if (server) {
            server.reset();
            logEvent("ServerStop");
        }
    }

private:
    void runServer()
    {
        aoo::net::iserver* current = nullptr;
        {
            std::lock_guard<std::mutex> lock(serverMutex);
            current = server.get();
        }
        if (current) {
            current->run();
        }
    }

    void eventLoop()
    {
        while (running.load()) {
            handleEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        handleEvents();
    }

    void handleEvents()
    {
        std::lock_guard<std::mutex> lock(serverMutex);
        if (server) {
            server->handle_events(handleServerEventsCallback, this);
        }
    }

    static int32_t handleServerEventsCallback(void* user, const aoo_event** events, int32_t n)
    {
        return static_cast<AooUdpServer*>(user)->handleServerEvents(events, n);
    }

    int32_t handleServerEvents(const aoo_event** events, int32_t n)
    {
        for (int i = 0; i < n; ++i) {
            switch (events[i]->type) {
                case AOONET_SERVER_USER_JOIN_EVENT:
                {
                    auto* e = (aoonet_server_user_event*) events[i];
                    juce::String msg;
                    msg << "UserJoin," << e->name;
                    logEvent(msg);
                    break;
                }
                case AOONET_SERVER_USER_LEAVE_EVENT:
                {
                    auto* e = (aoonet_server_user_event*) events[i];
                    juce::String msg;
                    msg << "UserLeave," << e->name;
                    logEvent(msg);
                    break;
                }
                case AOONET_SERVER_GROUP_JOIN_EVENT:
                {
                    auto* e = (aoonet_server_group_event*) events[i];
                    juce::String msg;
                    msg << "GroupJoin," << e->group << "," << e->user;
                    logEvent(msg);
                    break;
                }
                case AOONET_SERVER_GROUP_LEAVE_EVENT:
                {
                    auto* e = (aoonet_server_group_event*) events[i];
                    juce::String msg;
                    msg << "GroupLeave," << e->group << "," << e->user;
                    logEvent(msg);
                    break;
                }
                case AOONET_SERVER_ERROR_EVENT:
                {
                    auto* e = (aoonet_server_event*) events[i];
                    juce::String msg;
                    msg << "Error," << e->errormsg;
                    logEvent(msg);
                    break;
                }
                default:
                {
                    juce::String msg;
                    msg << "Unknown," << events[i]->type;
                    logEvent(msg);
                    break;
                }
            }
        }
        return n;
    }

    void openLogger()
    {
        juce::File logdir(options.logDir);
        if (!juce::File::isAbsolutePath(options.logDir)) {
            logdir = juce::File::getCurrentWorkingDirectory().getChildFile(options.logDir);
        }
        logger.reset(new juce::FileLogger(logdir.getChildFile("aooserver_log_" + juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S"))
                                             .withFileExtension(".txt")
                                             .getNonexistentSibling(),
                                         "CookieLink AOO Server",
                                         0));
    }

    void logEvent(const juce::String& evstr)
    {
        juce::String message;
        message << juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S") << ",";
        if (server) {
            message << server->get_group_count() << "," << server->get_user_count() << ",";
        } else {
            message << "0,0,";
        }
        message << evstr;

        if (logger) {
            logger->logMessage(message);
        } else {
            std::cout << message << "\n";
        }
    }

    Options options;
    std::atomic<bool> running { false };
    std::mutex serverMutex;
    aoo::net::iserver::pointer server;
    std::thread serverThread;
    std::thread eventThread;
    std::unique_ptr<juce::FileLogger> logger;
};

class RelayServer {
public:
    struct Options {
        int port = 11000;
        bool requireAuth = false;
        juce::String authToken;
        std::map<juce::String, juce::String> authUsers;
        bool tlsEnabled = false;
        juce::String tlsCertPath;
        juce::String tlsKeyPath;
        juce::String tlsCaPath;
        int sendBufferBytes = 1024 * 1024;
        int recvBufferBytes = 1024 * 1024;
        int writeTimeoutMs = 200;
        int maxQueueBytes = 2 * 1024 * 1024;
        int maxQueueMs = 120;
        int statsIntervalMs = 0;
        bool debug = false;
        bool logFrames = false;
        bool logData = false;
    };

    explicit RelayServer(const Options& opts)
        : options(opts),
          listenPort(opts.port)
    {
#if defined(COOKIELINK_RELAY_USE_OPENSSL)
        if (options.tlsEnabled) {
            SSL_library_init();
            SSL_load_error_strings();
            OpenSSL_add_all_algorithms();

            tlsContext = SSL_CTX_new(TLS_server_method());
            if (!tlsContext) {
                std::cerr << "TLS context init failed\n";
                options.tlsEnabled = false;
            } else {
                SSL_CTX_set_verify(tlsContext, SSL_VERIFY_NONE, nullptr);
                if (SSL_CTX_use_certificate_file(tlsContext, options.tlsCertPath.toRawUTF8(), SSL_FILETYPE_PEM) != 1
                    || SSL_CTX_use_PrivateKey_file(tlsContext, options.tlsKeyPath.toRawUTF8(), SSL_FILETYPE_PEM) != 1) {
                    std::cerr << "TLS certificate/key load failed\n";
                    SSL_CTX_free(tlsContext);
                    tlsContext = nullptr;
                    options.tlsEnabled = false;
                }
                if (options.tlsEnabled && options.tlsCaPath.isNotEmpty()) {
                    SSL_CTX_load_verify_locations(tlsContext, options.tlsCaPath.toRawUTF8(), nullptr);
                }
            }
        }
#endif
    }

    ~RelayServer()
    {
#if defined(COOKIELINK_RELAY_USE_OPENSSL)
        if (tlsContext) {
            SSL_CTX_free(tlsContext);
            tlsContext = nullptr;
        }
#endif
    }

    static juce::String clientLabel(const std::shared_ptr<Client>& client)
    {
        if (!client || !client->socket) {
            return "<unknown>";
        }
        auto addr = client->socket->getHostName();
        if (addr.isEmpty()) {
            addr = "<unknown>";
        }
        return addr;
    }

    int run()
    {
        juce::StreamingSocket listener;
        if (!listener.createListener(listenPort)) {
            std::cerr << "Failed to listen on port " << listenPort << "\n";
            return 1;
        }

        std::cout << "CookieLink TCP relay listening on " << listenPort << "\n";

        if (options.statsIntervalMs > 0) {
            statsThread = std::thread([this]() {
                statsLoop();
            });
            statsThread.detach();
        }

        while (true) {
            std::unique_ptr<juce::StreamingSocket> socket(listener.waitForNextConnection());
            if (!socket) {
                continue;
            }
            disableSigPipe(*socket);
            tuneRelaySocket(*socket);
            auto client = std::make_shared<Client>(std::move(socket));
            if (options.recvBufferBytes > 0) {
                client->socket->setReceiveBufferSize(options.recvBufferBytes);
            }
            if (options.sendBufferBytes > 0) {
                client->socket->setSendBufferSize(options.sendBufferBytes);
            }

            std::cout << "Relay client connected from " << clientLabel(client) << "\n";
            logClientDebug(client, "accepted connection");

            client->sendThread = std::thread([this, client]() {
                sendLoop(client);
            });
            client->sendThread.detach();

            client->thread = std::thread([this, client]() {
                handleClient(client);
            });
            client->thread.detach();
        }
    }

private:
    void logDebug(const juce::String& message) const
    {
        if (!options.debug) {
            return;
        }
        std::cout << relayTimestamp() << " [relay] " << message << "\n";
    }

    void logClientDebug(const std::shared_ptr<Client>& client, const juce::String& message) const
    {
        if (!options.debug) {
            return;
        }
        std::cout << relayTimestamp() << " [relay] ";
        if (client) {
            if (client->id != 0) {
                std::cout << "id=" << client->id << " ";
            }
            if (client->user.isNotEmpty()) {
                std::cout << "user=" << client->user << " ";
            }
            std::cout << "addr=" << clientLabel(client) << " ";
        }
        std::cout << message << "\n";
    }

    void logFrame(const std::shared_ptr<Client>& client, const juce::String& message) const
    {
        if (!options.logFrames) {
            return;
        }
        std::cout << relayTimestamp() << " [relay-frame] ";
        if (client) {
            if (client->id != 0) {
                std::cout << "id=" << client->id << " ";
            }
            if (client->user.isNotEmpty()) {
                std::cout << "user=" << client->user << " ";
            }
        }
        std::cout << message << "\n";
    }

    bool authenticateClient(const juce::String& username, const juce::String& password)
    {
        if (!options.requireAuth) {
            return true;
        }
        if (options.authToken.isNotEmpty()) {
            return password == options.authToken;
        }
        if (!options.authUsers.empty()) {
            auto it = options.authUsers.find(username);
            if (it == options.authUsers.end()) {
                return false;
            }
            return it->second == password;
        }
        return false;
    }

    void handleClient(const std::shared_ptr<Client>& client)
    {
        juce::MemoryBlock frame;
        juce::String readErr;

#if defined(COOKIELINK_RELAY_USE_OPENSSL)
        if (options.tlsEnabled && tlsContext) {
            client->ssl = SSL_new(tlsContext);
            if (!client->ssl) {
                goto cleanup;
            }
            SSL_set_fd(client->ssl, client->socket->getRawSocketHandle());
            if (SSL_accept(client->ssl) != 1) {
                SSL_free(client->ssl);
                client->ssl = nullptr;
                goto cleanup;
            }
        }
#endif

        if (!readFrame(*client, frame, &readErr)) {
            std::cout << "Relay client hello read failed from " << clientLabel(client);
            if (readErr.isNotEmpty()) {
                std::cout << " (" << readErr << ")";
            }
            std::cout << "\n";
            goto cleanup;
        }

        if (!handleHello(client, frame)) {
            goto cleanup;
        }

        client->id = nextClientId.fetch_add(1);
        sendWelcome(client);
        logClientDebug(client, "sent welcome");

        while (true) {
            juce::MemoryBlock msg;
            readErr.clear();
            if (!readFrame(*client, msg, &readErr)) {
                std::cout << "Relay client " << client->id
                          << " read failed (connected=" << (client->socket && client->socket->isConnected() ? "yes" : "no")
                          << " user=" << client->user;
                if (readErr.isNotEmpty()) {
                    std::cout << " err=" << readErr;
                }
                std::cout << ")\n";
                break;
            }
            handleFrame(client, msg);
        }

    cleanup:
        logClientDebug(client, "cleanup connection");
        removeClient(client);

#if defined(COOKIELINK_RELAY_USE_OPENSSL)
        if (client->ssl) {
            SSL_shutdown(client->ssl);
            SSL_free(client->ssl);
            client->ssl = nullptr;
        }
#endif
    }

    bool handleHello(const std::shared_ptr<Client>& client, const juce::MemoryBlock& frame)
    {
        const auto* data = static_cast<const uint8_t*>(frame.getData());
        const int size = static_cast<int>(frame.getSize());
        if (size < 1 || data[0] != MsgHello) {
            std::cout << "Relay client invalid hello from " << clientLabel(client) << "\n";
            sendErrorImmediate(*client, "invalid hello");
            return false;
        }

        int offset = 1;
        uint16_t version = 0;
        if (!readU16(data, size, offset, version)) {
            std::cout << "Relay client invalid hello from " << clientLabel(client) << "\n";
            sendErrorImmediate(*client, "invalid hello");
            return false;
        }
        if (version != kProtocolVersion) {
            std::cout << "Relay client protocol mismatch from " << clientLabel(client)
                      << " version=" << version << "\n";
            sendErrorImmediate(*client, "protocol mismatch");
            return false;
        }

        juce::String username;
        juce::String password;
        if (!readString(data, size, offset, username) || !readString(data, size, offset, password)) {
            std::cout << "Relay client invalid hello from " << clientLabel(client) << "\n";
            sendErrorImmediate(*client, "invalid hello");
            return false;
        }

        if (!authenticateClient(username, password)) {
            std::cout << "Relay client authentication failed from " << clientLabel(client)
                      << " user=" << username << "\n";
            sendErrorImmediate(*client, "authentication failed");
            return false;
        }

        client->user = username;
        std::cout << "Relay client hello ok from " << clientLabel(client)
                  << " user=" << username << "\n";
        logClientDebug(client, "hello ok");
        return true;
    }

    void handleFrame(const std::shared_ptr<Client>& client, const juce::MemoryBlock& frame)
    {
        const auto* data = static_cast<const uint8_t*>(frame.getData());
        const int size = static_cast<int>(frame.getSize());
        if (size < 1) {
            return;
        }

        const auto type = static_cast<MessageType>(data[0]);
        int offset = 1;

        if (options.logFrames) {
            if (type != MsgData || options.logData) {
                juce::String msg = "recv type=" + relayMessageName(type)
                                   + " size=" + juce::String(size);
                logFrame(client, msg);
            }
        }

        switch (type) {
            case MsgJoinGroup:
            {
                juce::String group;
                juce::String password;
                if (!readString(data, size, offset, group) || !readString(data, size, offset, password)) {
                    sendJoinResult(client, false, "invalid join request");
                    break;
                }
                bool isPublic = false;
                if (offset < size) {
                    isPublic = data[offset++] != 0;
                }
                (void)isPublic;
                handleJoinGroup(client, group, password, isPublic);
                break;
            }
            case MsgLeaveGroup:
            {
                juce::String group;
                if (!readString(data, size, offset, group)) {
                    break;
                }
                handleLeaveGroup(client, group);
                break;
            }
            case MsgPublicGroupsRequest:
            {
                bool watch = false;
                if (offset < size) {
                    watch = data[offset++] != 0;
                }
                handlePublicGroupsRequest(client, watch);
                break;
            }
            case MsgData:
            {
                uint32_t srcId = 0;
                uint32_t destId = 0;
                uint32_t payloadLen = 0;
                if (!readU32(data, size, offset, srcId) || !readU32(data, size, offset, destId) || !readU32(data, size, offset, payloadLen)) {
                    break;
                }
                if (payloadLen == 0 || offset + static_cast<int>(payloadLen) > size) {
                    break;
                }
                if (options.logFrames && options.logData) {
                    logFrame(client, "recv type=data src=" + juce::String(srcId)
                             + " dest=" + juce::String(destId)
                             + " payload=" + juce::String(static_cast<int64_t>(payloadLen)));
                }
                handleData(client, destId, data + offset, payloadLen);
                break;
            }
            default:
                break;
        }
    }

    void handleJoinGroup(const std::shared_ptr<Client>& client, const juce::String& group, const juce::String& password, bool isPublic)
    {
        std::vector<std::shared_ptr<Client>> oldPeers;
        std::vector<std::shared_ptr<Client>> peers;
        bool notifyPublic = false;
        bool alreadyInGroup = false;
        bool joinRejected = false;
        size_t memberCount = 0;
        {
            std::lock_guard<std::mutex> lock(stateLock);

            logClientDebug(client, "join request group=" + group + " public=" + juce::String(isPublic ? "yes" : "no"));

            auto& groupState = groups[group];
            if (client->group == group && groupState.members.count(client->id) != 0) {
                alreadyInGroup = true;
            }
            else if (!groupState.members.empty() && groupState.password != password) {
                joinRejected = true;
            }
            else if (groupState.members.empty()) {
                groupState.password = password;
                groupState.isPublic = isPublic;
            }
            else {
                groupState.isPublic = groupState.isPublic || isPublic;
            }

            if (!alreadyInGroup && !joinRejected) {
                if (client->group.isNotEmpty() && client->group != group) {
                    auto oldIt = groups.find(client->group);
                    if (oldIt != groups.end()) {
                        oldIt->second.members.erase(client->id);
                        notifyPublic = notifyPublic || oldIt->second.isPublic;
                        for (auto peerId : oldIt->second.members) {
                            auto it = clients.find(peerId);
                            if (it != clients.end()) {
                                oldPeers.push_back(it->second);
                            }
                        }
                        if (oldIt->second.members.empty()) {
                            groups.erase(oldIt);
                        }
                    }
                    client->group.clear();
                }

                notifyPublic = notifyPublic || groupState.isPublic;
                client->group = group;
                groupState.members.insert(client->id);
                memberCount = groupState.members.size();

                for (auto peerId : groupState.members) {
                    if (peerId == client->id) {
                        continue;
                    }
                    auto it = clients.find(peerId);
                    if (it != clients.end()) {
                        peers.push_back(it->second);
                    }
                }
            }
        }

        if (alreadyInGroup) {
            logClientDebug(client, "join ignored (already in group) group=" + group);
            sendJoinResult(client, true, "");
            return;
        }

        if (joinRejected) {
            logClientDebug(client, "join rejected (password mismatch) group=" + group);
            sendJoinResult(client, false, "group password mismatch");
            return;
        }

        for (const auto& peer : oldPeers) {
            sendPeerLeave(peer, client->id);
        }

        sendJoinResult(client, true, "");
        logClientDebug(client, "joined group=" + group + " members=" + juce::String(static_cast<int>(memberCount)));

        for (const auto& peer : peers) {
            sendPeerJoin(client, peer->id, peer->user);
        }

        for (const auto& peer : peers) {
            sendPeerJoin(peer, client->id, client->user);
        }

        if (notifyPublic) {
            broadcastPublicGroupsUpdate();
        }
    }

    void handleLeaveGroup(const std::shared_ptr<Client>& client, const juce::String& group)
    {
        std::vector<std::shared_ptr<Client>> peers;
        bool wasPublic = false;
        {
            std::lock_guard<std::mutex> lock(stateLock);
            auto it = groups.find(group);
            if (it == groups.end()) {
                return;
            }
            wasPublic = it->second.isPublic;
            it->second.members.erase(client->id);
            client->group.clear();

            for (auto peerId : it->second.members) {
                auto pit = clients.find(peerId);
                if (pit != clients.end()) {
                    peers.push_back(pit->second);
                }
            }

            if (it->second.members.empty()) {
                groups.erase(it);
            }
        }

        for (const auto& peer : peers) {
            sendPeerLeave(peer, client->id);
        }

        if (wasPublic) {
            broadcastPublicGroupsUpdate();
        }
    }

    void handleData(const std::shared_ptr<Client>& client, uint32_t destId, const uint8_t* payload, uint32_t payloadLen)
    {
        std::vector<std::shared_ptr<Client>> targets;
        {
            std::lock_guard<std::mutex> lock(stateLock);
            if (client->group.isEmpty()) {
                return;
            }

            auto it = groups.find(client->group);
            if (it == groups.end()) {
                return;
            }

            if (destId == 0) {
                for (auto peerId : it->second.members) {
                    if (peerId == client->id) {
                        continue;
                    }
                    auto pit = clients.find(peerId);
                    if (pit != clients.end()) {
                        targets.push_back(pit->second);
                    }
                }
            } else {
                if (it->second.members.count(destId) == 0) {
                    return;
                }
                auto pit = clients.find(destId);
                if (pit != clients.end()) {
                    targets.push_back(pit->second);
                }
            }
        }

        if (targets.empty()) {
            return;
        }

        juce::MemoryOutputStream msg;
        msg.writeByte(static_cast<char>(MsgData));
        writeU32(msg, client->id);
        writeU32(msg, destId);
        writeU32(msg, payloadLen);
        msg.write(payload, payloadLen);
        auto frame = buildFrame(msg.getMemoryBlock());

        for (const auto& peer : targets) {
            if (options.logFrames && options.logData) {
                logFrame(peer, "send type=data src=" + juce::String(client->id)
                         + " dest=" + juce::String(destId)
                         + " payload=" + juce::String(static_cast<int64_t>(payloadLen)));
            }
            enqueueFrame(peer, frame, true);
        }
    }

    void removeClient(const std::shared_ptr<Client>& client)
    {
        stopClient(client);
        std::vector<std::shared_ptr<Client>> peers;
        bool wasPublic = false;
        {
            std::lock_guard<std::mutex> lock(stateLock);
            if (!client->group.isEmpty()) {
                auto it = groups.find(client->group);
                if (it != groups.end()) {
                    it->second.members.erase(client->id);
                    wasPublic = it->second.isPublic;
                    for (auto peerId : it->second.members) {
                        auto pit = clients.find(peerId);
                        if (pit != clients.end()) {
                            peers.push_back(pit->second);
                        }
                    }
                    if (it->second.members.empty()) {
                        groups.erase(it);
                    }
                }
            }
            publicGroupWatchers.erase(client->id);
            clients.erase(client->id);
        }

        for (const auto& peer : peers) {
            sendPeerLeave(peer, client->id);
        }

        if (wasPublic) {
            broadcastPublicGroupsUpdate();
        }
    }

    void sendWelcome(const std::shared_ptr<Client>& client)
    {
        juce::MemoryOutputStream payload;
        payload.writeByte(static_cast<char>(MsgWelcome));
        writeU16(payload, kProtocolVersion);
        writeU32(payload, client->id);
        if (!sendFrameImmediate(*client, payload.getMemoryBlock())) {
            std::cout << "Relay failed to send welcome for " << clientLabel(client) << "\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lock(stateLock);
            clients[client->id] = client;
        }
    }

    void sendJoinResult(const std::shared_ptr<Client>& client, bool success, const juce::String& message)
    {
        if (options.logFrames) {
            logFrame(client, "send type=join-result success=" + juce::String(success ? "yes" : "no")
                             + (message.isNotEmpty() ? (" message=" + message) : ""));
        }
        juce::MemoryOutputStream payload;
        payload.writeByte(static_cast<char>(MsgJoinResult));
        payload.writeByte(success ? 1 : 0);
        writeString(payload, message);
        sendFrame(client, payload.getMemoryBlock(), false);
    }

    void sendPeerJoin(const std::shared_ptr<Client>& target, uint32_t peerId, const juce::String& username)
    {
        if (options.logFrames) {
            logFrame(target, "send type=peer-join peerId=" + juce::String(peerId) + " user=" + username);
        }
        juce::MemoryOutputStream payload;
        payload.writeByte(static_cast<char>(MsgPeerJoin));
        writeU32(payload, peerId);
        writeString(payload, username);
        sendFrame(target, payload.getMemoryBlock(), false);
    }

    void sendPeerLeave(const std::shared_ptr<Client>& target, uint32_t peerId)
    {
        if (options.logFrames) {
            logFrame(target, "send type=peer-leave peerId=" + juce::String(peerId));
        }
        juce::MemoryOutputStream payload;
        payload.writeByte(static_cast<char>(MsgPeerLeave));
        writeU32(payload, peerId);
        sendFrame(target, payload.getMemoryBlock(), false);
    }

    void sendError(const std::shared_ptr<Client>& client, const juce::String& message)
    {
        if (options.logFrames) {
            logFrame(client, "send type=error message=" + message);
        }
        juce::MemoryOutputStream payload;
        payload.writeByte(static_cast<char>(MsgError));
        writeString(payload, message);
        sendFrame(client, payload.getMemoryBlock(), false);
    }

    void sendErrorImmediate(Client& client, const juce::String& message)
    {
        juce::MemoryOutputStream payload;
        payload.writeByte(static_cast<char>(MsgError));
        writeString(payload, message);
        sendFrameImmediate(client, payload.getMemoryBlock());
    }

    bool sendFrame(const std::shared_ptr<Client>& client, const juce::MemoryBlock& payload, bool droppable)
    {
        if (!client) {
            return false;
        }
        if (payload.getSize() == 0 || payload.getSize() > kMaxFrameSize) {
            return false;
        }
        auto frame = buildFrame(payload);
        return enqueueFrame(client, frame, droppable);
    }

    bool sendFrameImmediate(Client& client, const juce::MemoryBlock& payload)
    {
        if (payload.getSize() == 0 || payload.getSize() > kMaxFrameSize) {
            return false;
        }
        auto frame = buildFrame(payload);
        return writeAll(client, frame->getData(), static_cast<int>(frame->getSize()));
    }

    bool writeAll(Client& client, const void* data, int bytes)
    {
        const juce::ScopedLock sl(client.writeLock);
        struct WriteTimer {
            RelayServer& owner;
            Client& client;
            double startedMs;
            ~WriteTimer()
            {
                owner.recordClientWriteBlock(client, juce::Time::getMillisecondCounterHiRes() - startedMs);
            }
        } writeTimer { *this, client, juce::Time::getMillisecondCounterHiRes() };

        const auto* out = static_cast<const char*>(data);
        const int waitMs = relaySocketWaitMs(options.writeTimeoutMs);
        int offset = 0;
        while (offset < bytes) {
            if (!client.socket || !client.socket->isConnected()) {
                return false;
            }

            int written = 0;
#if defined(COOKIELINK_RELAY_USE_OPENSSL)
            if (client.ssl) {
                written = SSL_write(client.ssl, out + offset, bytes - offset);
                if (written <= 0) {
                    const int sslErr = SSL_get_error(client.ssl, written);
                    if (sslErr == SSL_ERROR_WANT_READ) {
                        relaySocketWait(*client.socket, true, waitMs);
                        continue;
                    }
                    if (sslErr == SSL_ERROR_WANT_WRITE) {
                        relaySocketWait(*client.socket, false, waitMs);
                        continue;
                    }
                    return false;
                }
            } else
#endif
            {
                errno = 0;
                written = client.socket->write(out + offset, bytes - offset);
                if (written <= 0) {
                    const int errnum = errno;
                    if (written < 0 && isTransientSocketErr(errnum)) {
                        relaySocketWait(*client.socket, false, waitMs);
                        continue;
                    }
                    if (written == 0) {
                        return false;
                    }
                    return false;
                }
            }
            offset += written;
        }
        return true;
    }

    void recordClientWriteBlock(Client& client, double elapsedMs)
    {
        client.lastWriteBlockMs = juce::jmax(0.0, elapsedMs);
        client.maxWriteBlockMs = juce::jmax(client.maxWriteBlockMs, client.lastWriteBlockMs);
        if (client.writeBlockSamples <= 0) {
            client.avgWriteBlockMs = client.lastWriteBlockMs;
            client.writeBlockSamples = 1;
        } else {
            const double weight = client.writeBlockSamples < 64 ? 1.0 / static_cast<double>(client.writeBlockSamples + 1) : 0.04;
            client.avgWriteBlockMs += (client.lastWriteBlockMs - client.avgWriteBlockMs) * weight;
            ++client.writeBlockSamples;
        }
    }

    bool readExact(Client& client, void* dest, int bytes, juce::String* err = nullptr)
    {
        const int waitMs = relaySocketWaitMs(options.writeTimeoutMs);
        int offset = 0;
        auto* out = static_cast<char*>(dest);
        while (offset < bytes) {
            if (!client.socket || !client.socket->isConnected()) {
                if (err) {
                    *err = "relay connection closed";
                }
                return false;
            }

            int got = 0;
#if defined(COOKIELINK_RELAY_USE_OPENSSL)
            if (client.ssl) {
                got = SSL_read(client.ssl, out + offset, bytes - offset);
                if (got <= 0) {
                    const int sslErr = SSL_get_error(client.ssl, got);
                    if (sslErr == SSL_ERROR_WANT_READ) {
                        relaySocketWait(*client.socket, true, waitMs);
                        continue;
                    }
                    if (sslErr == SSL_ERROR_WANT_WRITE) {
                        relaySocketWait(*client.socket, false, waitMs);
                        continue;
                    }
                    if (err) {
                        *err = "relay TLS read failed (ssl_err=" + juce::String(sslErr) + ")";
                    }
                    return false;
                }
            } else
#endif
            {
                errno = 0;
                got = client.socket->read(out + offset, bytes - offset, true);
                if (got > 0) {
                    offset += got;
                    continue;
                }
                if (got == 0) {
                    if (err) {
                        *err = "relay connection closed";
                    }
                    return false;
                }
                const int errnum = errno;
                if (errnum == 0) {
                    if (err) {
                        *err = "relay connection closed";
                    }
                    return false;
                }
                if (isTransientSocketErr(errnum)) {
                    relaySocketWait(*client.socket, true, waitMs);
                    if (!client.socket->isConnected()) {
                        if (err) {
                            *err = "relay connection closed" + formatErrno(errnum);
                        }
                        return false;
                    }
                    continue;
                }
                if (err) {
                    if (!client.socket->isConnected()) {
                        *err = "relay connection closed" + formatErrno(errnum);
                    } else {
                        *err = "relay socket read failed" + formatErrno(errnum);
                    }
                }
                return false;
            }
            offset += got;
        }
        return true;
    }

    bool readFrame(Client& client, juce::MemoryBlock& frame, juce::String* err = nullptr)
    {
        uint8_t lenBytes[4];
        if (!readExact(client, lenBytes, 4, err)) {
            return false;
        }

        const uint32_t length = (static_cast<uint32_t>(lenBytes[0]) << 24)
            | (static_cast<uint32_t>(lenBytes[1]) << 16)
            | (static_cast<uint32_t>(lenBytes[2]) << 8)
            | static_cast<uint32_t>(lenBytes[3]);

        if (length == 0 || length > kMaxFrameSize) {
            if (err) {
                *err = "relay frame length invalid (" + juce::String(static_cast<int64_t>(length)) + ")";
            }
            return false;
        }

        frame.setSize(length);
        return readExact(client, frame.getData(), static_cast<int>(length), err);
    }

    void handlePublicGroupsRequest(const std::shared_ptr<Client>& client, bool watch)
    {
        {
            std::lock_guard<std::mutex> lock(stateLock);
            client->watchPublicGroups = watch;
            if (watch) {
                publicGroupWatchers.insert(client->id);
            } else {
                publicGroupWatchers.erase(client->id);
            }
        }

        sendPublicGroupsUpdate(client);
    }

    void sendPublicGroupsUpdate(const std::shared_ptr<Client>& client)
    {
        if (options.logFrames) {
            logFrame(client, "send type=public-groups-update");
        }
        juce::MemoryOutputStream payload;
        payload.writeByte(static_cast<char>(MsgPublicGroupsUpdate));

        std::vector<std::pair<juce::String, int>> list;
        {
            std::lock_guard<std::mutex> lock(stateLock);
            for (const auto& entry : groups) {
                if (!entry.second.isPublic) {
                    continue;
                }
                list.emplace_back(entry.first, static_cast<int>(entry.second.members.size()));
            }
        }

        writeU16(payload, static_cast<uint16_t>(list.size()));
        for (const auto& entry : list) {
            writeString(payload, entry.first);
            writeU32(payload, static_cast<uint32_t>(entry.second));
        }

        sendFrame(client, payload.getMemoryBlock(), false);
    }

    void broadcastPublicGroupsUpdate()
    {
        std::vector<std::shared_ptr<Client>> watchers;
        {
            std::lock_guard<std::mutex> lock(stateLock);
            for (auto id : publicGroupWatchers) {
                auto it = clients.find(id);
                if (it != clients.end()) {
                    watchers.push_back(it->second);
                }
            }
        }

        for (const auto& client : watchers) {
            sendPublicGroupsUpdate(client);
        }
    }

    void stopClient(const std::shared_ptr<Client>& client)
    {
        if (!client) {
            return;
        }
        client->running.store(false);
        {
            std::lock_guard<std::mutex> lock(client->sendMutex);
            client->sendQueue.clear();
            client->sendQueueBytes = 0;
        }
        client->sendCv.notify_all();
    }

    FramePtr buildFrame(const juce::MemoryBlock& payload)
    {
        auto frame = std::make_shared<juce::MemoryBlock>();
        const auto payloadSize = payload.getSize();
        frame->setSize(payloadSize + 4);
        auto* out = static_cast<uint8_t*>(frame->getData());
        const uint32_t len = static_cast<uint32_t>(payloadSize);
        out[0] = static_cast<uint8_t>((len >> 24) & 0xff);
        out[1] = static_cast<uint8_t>((len >> 16) & 0xff);
        out[2] = static_cast<uint8_t>((len >> 8) & 0xff);
        out[3] = static_cast<uint8_t>(len & 0xff);
        if (payloadSize > 0) {
            std::memcpy(out + 4, payload.getData(), payloadSize);
        }
        return frame;
    }

    int dropExpiredDroppableFramesLocked(const std::shared_ptr<Client>& client, double nowMs)
    {
        if (!client || options.maxQueueMs <= 0) {
            return 0;
        }

        int dropped = 0;
        for (auto it = client->sendQueue.begin(); it != client->sendQueue.end();) {
            if (it->droppable && nowMs - it->enqueuedMs >= static_cast<double>(options.maxQueueMs)) {
                client->sendQueueBytes -= it->size;
                it = client->sendQueue.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
        if (dropped > 0) {
            client->droppedFrames.fetch_add(static_cast<uint64_t>(dropped), std::memory_order_relaxed);
        }
        return dropped;
    }

    int dropDroppableFramesForByteBudgetLocked(const std::shared_ptr<Client>& client, size_t incomingSize)
    {
        if (!client || options.maxQueueBytes <= 0) {
            return 0;
        }

        int dropped = 0;
        const auto maxBytes = static_cast<size_t>(options.maxQueueBytes);
        for (auto it = client->sendQueue.begin();
             it != client->sendQueue.end() && client->sendQueueBytes + incomingSize > maxBytes;) {
            if (it->droppable) {
                client->sendQueueBytes -= it->size;
                it = client->sendQueue.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
        if (dropped > 0) {
            client->droppedFrames.fetch_add(static_cast<uint64_t>(dropped), std::memory_order_relaxed);
        }
        return dropped;
    }

    void logDropIfNeeded(const std::shared_ptr<Client>& client)
    {
        if (!client) {
            return;
        }
        const auto nowMs = juce::Time::getMillisecondCounter();
        auto lastMs = client->lastDropLogMs.load();
        if (nowMs - lastMs >= 1000) {
            if (client->lastDropLogMs.compare_exchange_strong(lastMs, nowMs)) {
                std::cout << "Relay client " << client->id << " dropping frames (total "
                          << client->droppedFrames.load() << ")\n";
            }
        }
    }

    bool enqueueFrame(const std::shared_ptr<Client>& client, const FramePtr& frame, bool droppable)
    {
        if (!client || !frame || frame->getSize() == 0) {
            return false;
        }
        if (!client->running.load()) {
            return false;
        }

        const size_t frameSize = frame->getSize();
        if (options.maxQueueBytes > 0 && frameSize > static_cast<size_t>(options.maxQueueBytes)) {
            return false;
        }

        {
            std::unique_lock<std::mutex> lock(client->sendMutex);
            if (!client->running.load()) {
                return false;
            }
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            const int staleDropped = dropExpiredDroppableFramesLocked(client, nowMs);
            if (staleDropped > 0) {
                lock.unlock();
                logDropIfNeeded(client);
                lock.lock();
                if (!client->running.load()) {
                    return false;
                }
            }

            if (options.maxQueueBytes > 0 && client->sendQueueBytes + frameSize > static_cast<size_t>(options.maxQueueBytes)) {
                const int budgetDropped = dropDroppableFramesForByteBudgetLocked(client, frameSize);
                if (budgetDropped > 0) {
                    lock.unlock();
                    logDropIfNeeded(client);
                    lock.lock();
                    if (!client->running.load()) {
                        return false;
                    }
                }

                if (client->sendQueueBytes + frameSize > static_cast<size_t>(options.maxQueueBytes)) {
                    if (droppable) {
                        client->droppedFrames.fetch_add(1, std::memory_order_relaxed);
                        lock.unlock();
                        logDropIfNeeded(client);
                        return false;
                    }
                    std::cout << "Relay client " << client->id
                              << " queue overflow; closing (queue="
                              << client->sendQueueBytes << "B frame=" << frameSize
                              << "B max=" << options.maxQueueBytes
                              << " user=" << client->user << ")\n";
                    client->running.store(false);
                    client->sendQueue.clear();
                    client->sendQueueBytes = 0;
                    lock.unlock();
                    client->sendCv.notify_all();
                    if (client->socket) {
                        client->socket->close();
                    }
                    return false;
                }
            }

            client->sendQueue.push_back(OutgoingFrame{frame, frameSize, droppable, nowMs});
            client->sendQueueBytes += frameSize;
        }

        client->sendCv.notify_one();
        return true;
    }

    void sendLoop(const std::shared_ptr<Client>& client)
    {
        while (client && client->running.load()) {
            OutgoingFrame item;
            {
                std::unique_lock<std::mutex> lock(client->sendMutex);
                client->sendCv.wait(lock, [&] {
                    return !client->running.load() || !client->sendQueue.empty();
                });
                if (!client->running.load() && client->sendQueue.empty()) {
                    break;
                }
                if (client->sendQueue.empty()) {
                    continue;
                }
                item = std::move(client->sendQueue.front());
                client->sendQueue.pop_front();
                client->sendQueueBytes -= item.size;
            }

            if (!item.frame || item.size == 0) {
                continue;
            }

            if (item.droppable && options.maxQueueMs > 0
                && juce::Time::getMillisecondCounterHiRes() - item.enqueuedMs >= static_cast<double>(options.maxQueueMs)) {
                client->droppedFrames.fetch_add(1, std::memory_order_relaxed);
                logDropIfNeeded(client);
                continue;
            }

            if (!writeAll(*client, item.frame->getData(), static_cast<int>(item.size))) {
                std::cout << "Relay write failed for " << clientLabel(client) << "\n";
                client->running.store(false);
                if (client->socket) {
                    client->socket->close();
                }
                break;
            }
            client->sentBytes.fetch_add(item.size, std::memory_order_relaxed);
        }
    }

    void statsLoop()
    {
        std::unordered_map<uint32_t, uint64_t> lastSent;
        std::unordered_map<uint32_t, uint64_t> lastDropped;
        auto lastTimeMs = juce::Time::getMillisecondCounterHiRes();

        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.statsIntervalMs));

            auto nowMs = juce::Time::getMillisecondCounterHiRes();
            const auto dt = (nowMs - lastTimeMs) / 1000.0;
            if (dt <= 0.0) {
                lastTimeMs = nowMs;
                continue;
            }
            lastTimeMs = nowMs;

            struct Snapshot {
                std::shared_ptr<Client> client;
                uint32_t id = 0;
                juce::String user;
                juce::String group;
            };

            std::vector<Snapshot> snapshot;
            size_t groupCount = 0;
            {
                std::lock_guard<std::mutex> lock(stateLock);
                snapshot.reserve(clients.size());
                for (const auto& entry : clients) {
                    Snapshot item;
                    item.client = entry.second;
                    item.id = entry.second->id;
                    item.user = entry.second->user;
                    item.group = entry.second->group;
                    snapshot.push_back(std::move(item));
                }
                groupCount = groups.size();
            }

            std::unordered_set<uint32_t> activeIds;
            activeIds.reserve(snapshot.size());

            size_t totalQueueBytes = 0;
            size_t totalQueueFrames = 0;
            uint64_t totalRateBytes = 0;
            uint64_t totalDroppedDelta = 0;

            for (auto& item : snapshot) {
                activeIds.insert(item.id);

                size_t qbytes = 0;
                size_t qframes = 0;
                double oldestQueueMs = 0.0;
                double lastWriteMs = 0.0;
                double avgWriteMs = 0.0;
                double maxWriteMs = 0.0;
                if (item.client) {
                    {
                        std::lock_guard<std::mutex> lock(item.client->sendMutex);
                        qbytes = item.client->sendQueueBytes;
                        qframes = item.client->sendQueue.size();
                        if (!item.client->sendQueue.empty()) {
                            oldestQueueMs = juce::jmax(0.0, nowMs - item.client->sendQueue.front().enqueuedMs);
                        }
                    }
                    {
                        const juce::ScopedLock lock(item.client->writeLock);
                        lastWriteMs = item.client->lastWriteBlockMs;
                        avgWriteMs = item.client->avgWriteBlockMs;
                        maxWriteMs = item.client->maxWriteBlockMs;
                    }
                }
                totalQueueBytes += qbytes;
                totalQueueFrames += qframes;

                const auto sent = item.client ? item.client->sentBytes.load() : 0;
                const auto dropped = item.client ? item.client->droppedFrames.load() : 0;

                const auto prevSent = lastSent[item.id];
                const auto prevDropped = lastDropped[item.id];
                lastSent[item.id] = sent;
                lastDropped[item.id] = dropped;

                const auto deltaBytes = sent - prevSent;
                const auto deltaDropped = dropped - prevDropped;
                totalRateBytes += deltaBytes;
                totalDroppedDelta += deltaDropped;

                const uint64_t kbps = static_cast<uint64_t>((deltaBytes * 8.0) / (dt * 1000.0));

                std::cout << "Relay client id=" << item.id
                          << " user=" << item.user
                          << " group=" << item.group
                          << " queue=" << qframes << "/" << qbytes
                          << "B age=" << static_cast<int>(oldestQueueMs)
                          << "ms write=" << static_cast<int>(lastWriteMs)
                          << "/" << static_cast<int>(avgWriteMs)
                          << "/" << static_cast<int>(maxWriteMs)
                          << "ms"
                          << " rate=" << kbps << "kbps"
                          << " dropped=" << deltaDropped << "/" << dropped << "\n";
            }

            for (auto it = lastSent.begin(); it != lastSent.end();) {
                if (activeIds.count(it->first) == 0) {
                    lastDropped.erase(it->first);
                    it = lastSent.erase(it);
                } else {
                    ++it;
                }
            }

            const uint64_t totalKbps = static_cast<uint64_t>((totalRateBytes * 8.0) / (dt * 1000.0));

            std::cout << "Relay totals clients=" << snapshot.size()
                      << " groups=" << groupCount
                      << " queue=" << totalQueueFrames << "/" << totalQueueBytes
                      << "B rate=" << totalKbps << "kbps"
                      << " dropped=" << totalDroppedDelta << "\n";
        }
    }

    Options options;
    int listenPort = 0;
    std::mutex stateLock;
    std::unordered_map<uint32_t, std::shared_ptr<Client>> clients;
    std::map<juce::String, GroupState> groups;
    std::unordered_set<uint32_t> publicGroupWatchers;
    std::atomic<uint32_t> nextClientId { 1 };
    std::thread statsThread;

#if defined(COOKIELINK_RELAY_USE_OPENSSL)
    SSL_CTX* tlsContext = nullptr;
#endif
};

} // namespace

static void printUsage()
{
    std::cout << "Usage: cookielink-relay [--port <tcp_port>] [--udp-port <aoo_port>] [--no-udp]\n"
                 "                     [--udp-logdir <dir>]\n"
                 "                     [--auth-token <token>] [--auth-file <path>]\n"
                 "                     [--tls-cert <path> --tls-key <path> [--tls-ca <path>]]\n"
                 "                     [--send-buf <bytes>] [--recv-buf <bytes>] [--write-timeout <ms>]\n"
                 "                     [--max-queue-ms <ms>] [--max-queue <bytes>] [--stats-interval <ms>]\n"
                 "                     [--debug] [--log-frames] [--log-data]\n";
}


static bool loadAuthFile(const juce::String& path, std::map<juce::String, juce::String>& users)
{
    juce::File file(path);
    if (!file.existsAsFile()) {
        std::cerr << "Auth file not found: " << path << "\n";
        return false;
    }
    juce::String contents = file.loadFileAsString();
    juce::StringArray lines;
    lines.addLines(contents);
    for (auto line : lines) {
        line = line.trim();
        if (line.isEmpty() || line.startsWithChar('#')) {
            continue;
        }
        auto parts = juce::StringArray::fromTokens(line, ":", "");
        if (parts.size() < 2) {
            continue;
        }
        users[parts[0].trim()] = parts[1].trim();
    }
    return true;
}

int main(int argc, char* argv[])
{
   #if !JUCE_WINDOWS
    std::signal(SIGPIPE, SIG_IGN);
   #endif
    RelayServer::Options options;
    AooUdpServer::Options udpOptions;

    for (int i = 1; i < argc; ++i) {
        juce::String arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        if (arg == "--port" && i + 1 < argc) {
            options.port = juce::String(argv[++i]).getIntValue();
        }
        else if (arg == "--udp-port" && i + 1 < argc) {
            udpOptions.port = juce::String(argv[++i]).getIntValue();
        }
        else if (arg == "--no-udp") {
            udpOptions.enabled = false;
        }
        else if (arg == "--udp-logdir" && i + 1 < argc) {
            udpOptions.logDir = argv[++i];
        }
        else if (arg == "--auth-token" && i + 1 < argc) {
            options.authToken = argv[++i];
            options.requireAuth = true;
        }
        else if (arg == "--auth-file" && i + 1 < argc) {
            if (loadAuthFile(argv[++i], options.authUsers)) {
                options.requireAuth = true;
            } else {
                return 1;
            }
        }
        else if (arg == "--tls-cert" && i + 1 < argc) {
            options.tlsCertPath = argv[++i];
            options.tlsEnabled = true;
        }
        else if (arg == "--tls-key" && i + 1 < argc) {
            options.tlsKeyPath = argv[++i];
            options.tlsEnabled = true;
        }
        else if (arg == "--tls-ca" && i + 1 < argc) {
            options.tlsCaPath = argv[++i];
        }
        else if (arg == "--send-buf" && i + 1 < argc) {
            options.sendBufferBytes = juce::String(argv[++i]).getIntValue();
        }
        else if (arg == "--recv-buf" && i + 1 < argc) {
            options.recvBufferBytes = juce::String(argv[++i]).getIntValue();
        }
        else if (arg == "--write-timeout" && i + 1 < argc) {
            options.writeTimeoutMs = juce::String(argv[++i]).getIntValue();
        }
        else if (arg == "--max-queue" && i + 1 < argc) {
            options.maxQueueBytes = juce::String(argv[++i]).getIntValue();
        }
        else if (arg == "--max-queue-ms" && i + 1 < argc) {
            options.maxQueueMs = juce::jlimit(20, 1000, juce::String(argv[++i]).getIntValue());
        }
        else if (arg == "--stats-interval" && i + 1 < argc) {
            options.statsIntervalMs = juce::String(argv[++i]).getIntValue();
        }
        else if (arg == "--debug") {
            options.debug = true;
        }
        else if (arg == "--log-frames") {
            options.logFrames = true;
        }
        else if (arg == "--log-data") {
            options.logFrames = true;
            options.logData = true;
        }
        else if (arg.startsWithChar('-')) {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage();
            return 1;
        }
        else {
            options.port = arg.getIntValue();
        }
    }

#if !defined(COOKIELINK_RELAY_USE_OPENSSL)
    if (options.tlsEnabled) {
        std::cerr << "TLS requested but this build does not include OpenSSL support.\n";
        return 1;
    }
#endif

    if (options.tlsEnabled && (options.tlsCertPath.isEmpty() || options.tlsKeyPath.isEmpty())) {
        std::cerr << "TLS enabled but certificate/key not specified.\n";
        return 1;
    }

    AooUdpServer udpServer(udpOptions);
    if (!udpServer.start()) {
        return 1;
    }

    RelayServer server(options);
    const auto result = server.run();
    udpServer.stop();
    return result;
}
