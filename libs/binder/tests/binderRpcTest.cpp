/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <BinderRpcTestClientInfo.h>
#include <BinderRpcTestServerInfo.h>
#include <BnBinderRpcCallback.h>
#include <BnBinderRpcSession.h>
#include <BnBinderRpcTest.h>
#include <aidl/IBinderRpcTest.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android/binder_auto_utils.h>
#include <android/binder_libbinder.h>
#include <binder/Binder.h>
#include <binder/BpBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <binder/RpcServer.h>
#include <binder/RpcSession.h>
#include <binder/RpcTlsTestUtils.h>
#include <binder/RpcTlsUtils.h>
#include <binder/RpcTransport.h>
#include <binder/RpcTransportRaw.h>
#include <binder/RpcTransportTls.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>

#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../FdTrigger.h"
#include "../RpcSocketAddress.h" // for testing preconnected clients
#include "../RpcState.h"         // for debugging
#include "../vm_sockets.h"       // for VMADDR_*
#include "utils/Errors.h"

using namespace std::chrono_literals;
using namespace std::placeholders;
using testing::AssertionFailure;
using testing::AssertionResult;
using testing::AssertionSuccess;

namespace android {

static_assert(RPC_WIRE_PROTOCOL_VERSION + 1 == RPC_WIRE_PROTOCOL_VERSION_NEXT ||
              RPC_WIRE_PROTOCOL_VERSION == RPC_WIRE_PROTOCOL_VERSION_EXPERIMENTAL);
const char* kLocalInetAddress = "127.0.0.1";

enum class RpcSecurity { RAW, TLS };

static inline std::vector<RpcSecurity> RpcSecurityValues() {
    return {RpcSecurity::RAW, RpcSecurity::TLS};
}

static inline std::unique_ptr<RpcTransportCtxFactory> newFactory(
        RpcSecurity rpcSecurity, std::shared_ptr<RpcCertificateVerifier> verifier = nullptr,
        std::unique_ptr<RpcAuth> auth = nullptr) {
    switch (rpcSecurity) {
        case RpcSecurity::RAW:
            return RpcTransportCtxFactoryRaw::make();
        case RpcSecurity::TLS: {
            if (verifier == nullptr) {
                verifier = std::make_shared<RpcCertificateVerifierSimple>();
            }
            if (auth == nullptr) {
                auth = std::make_unique<RpcAuthSelfSigned>();
            }
            return RpcTransportCtxFactoryTls::make(std::move(verifier), std::move(auth));
        }
        default:
            LOG_ALWAYS_FATAL("Unknown RpcSecurity %d", rpcSecurity);
    }
}

// Create an FD that returns `contents` when read.
static base::unique_fd mockFileDescriptor(std::string contents) {
    android::base::unique_fd readFd, writeFd;
    CHECK(android::base::Pipe(&readFd, &writeFd)) << strerror(errno);
    std::thread([writeFd = std::move(writeFd), contents = std::move(contents)]() {
        signal(SIGPIPE, SIG_IGN); // ignore possible SIGPIPE from the write
        if (!WriteStringToFd(contents, writeFd)) {
            int savedErrno = errno;
            EXPECT_EQ(EPIPE, savedErrno)
                    << "mockFileDescriptor write failed: " << strerror(savedErrno);
        }
    }).detach();
    return readFd;
}

TEST(BinderRpcParcel, EntireParcelFormatted) {
    Parcel p;
    p.writeInt32(3);

    EXPECT_DEATH(p.markForBinder(sp<BBinder>::make()), "");
}

class BinderRpcServerOnly : public ::testing::TestWithParam<std::tuple<RpcSecurity, uint32_t>> {
public:
    static std::string PrintTestParam(const ::testing::TestParamInfo<ParamType>& info) {
        return std::string(newFactory(std::get<0>(info.param))->toCString()) + "_serverV" +
                std::to_string(std::get<1>(info.param));
    }
};

TEST_P(BinderRpcServerOnly, SetExternalServerTest) {
    base::unique_fd sink(TEMP_FAILURE_RETRY(open("/dev/null", O_RDWR)));
    int sinkFd = sink.get();
    auto server = RpcServer::make(newFactory(std::get<0>(GetParam())));
    server->setProtocolVersion(std::get<1>(GetParam()));
    ASSERT_FALSE(server->hasServer());
    ASSERT_EQ(OK, server->setupExternalServer(std::move(sink)));
    ASSERT_TRUE(server->hasServer());
    base::unique_fd retrieved = server->releaseServer();
    ASSERT_FALSE(server->hasServer());
    ASSERT_EQ(sinkFd, retrieved.get());
}

TEST(BinderRpc, CannotUseNextWireVersion) {
    auto session = RpcSession::make();
    EXPECT_FALSE(session->setProtocolVersion(RPC_WIRE_PROTOCOL_VERSION_NEXT));
    EXPECT_FALSE(session->setProtocolVersion(RPC_WIRE_PROTOCOL_VERSION_NEXT + 1));
    EXPECT_FALSE(session->setProtocolVersion(RPC_WIRE_PROTOCOL_VERSION_NEXT + 2));
    EXPECT_FALSE(session->setProtocolVersion(RPC_WIRE_PROTOCOL_VERSION_NEXT + 15));
}

TEST(BinderRpc, CanUseExperimentalWireVersion) {
    auto session = RpcSession::make();
    EXPECT_TRUE(session->setProtocolVersion(RPC_WIRE_PROTOCOL_VERSION_EXPERIMENTAL));
}

using android::binder::Status;

#define EXPECT_OK(status)                 \
    do {                                  \
        Status stat = (status);           \
        EXPECT_TRUE(stat.isOk()) << stat; \
    } while (false)

class MyBinderRpcSession : public BnBinderRpcSession {
public:
    static std::atomic<int32_t> gNum;

    MyBinderRpcSession(const std::string& name) : mName(name) { gNum++; }
    Status getName(std::string* name) override {
        *name = mName;
        return Status::ok();
    }
    ~MyBinderRpcSession() { gNum--; }

private:
    std::string mName;
};
std::atomic<int32_t> MyBinderRpcSession::gNum;

class MyBinderRpcCallback : public BnBinderRpcCallback {
    Status sendCallback(const std::string& value) {
        std::unique_lock _l(mMutex);
        mValues.push_back(value);
        _l.unlock();
        mCv.notify_one();
        return Status::ok();
    }
    Status sendOnewayCallback(const std::string& value) { return sendCallback(value); }

public:
    std::mutex mMutex;
    std::condition_variable mCv;
    std::vector<std::string> mValues;
};

class MyBinderRpcTest : public BnBinderRpcTest {
public:
    wp<RpcServer> server;
    int port = 0;

    Status sendString(const std::string& str) override {
        (void)str;
        return Status::ok();
    }
    Status doubleString(const std::string& str, std::string* strstr) override {
        *strstr = str + str;
        return Status::ok();
    }
    Status getClientPort(int* out) override {
        *out = port;
        return Status::ok();
    }
    Status countBinders(std::vector<int32_t>* out) override {
        sp<RpcServer> spServer = server.promote();
        if (spServer == nullptr) {
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }
        out->clear();
        for (auto session : spServer->listSessions()) {
            size_t count = session->state()->countBinders();
            out->push_back(count);
        }
        return Status::ok();
    }
    Status getNullBinder(sp<IBinder>* out) override {
        out->clear();
        return Status::ok();
    }
    Status pingMe(const sp<IBinder>& binder, int32_t* out) override {
        if (binder == nullptr) {
            std::cout << "Received null binder!" << std::endl;
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }
        *out = binder->pingBinder();
        return Status::ok();
    }
    Status repeatBinder(const sp<IBinder>& binder, sp<IBinder>* out) override {
        *out = binder;
        return Status::ok();
    }
    static sp<IBinder> mHeldBinder;
    Status holdBinder(const sp<IBinder>& binder) override {
        mHeldBinder = binder;
        return Status::ok();
    }
    Status getHeldBinder(sp<IBinder>* held) override {
        *held = mHeldBinder;
        return Status::ok();
    }
    Status nestMe(const sp<IBinderRpcTest>& binder, int count) override {
        if (count <= 0) return Status::ok();
        return binder->nestMe(this, count - 1);
    }
    Status alwaysGiveMeTheSameBinder(sp<IBinder>* out) override {
        static sp<IBinder> binder = new BBinder;
        *out = binder;
        return Status::ok();
    }
    Status openSession(const std::string& name, sp<IBinderRpcSession>* out) override {
        *out = new MyBinderRpcSession(name);
        return Status::ok();
    }
    Status getNumOpenSessions(int32_t* out) override {
        *out = MyBinderRpcSession::gNum;
        return Status::ok();
    }

    std::mutex blockMutex;
    Status lock() override {
        blockMutex.lock();
        return Status::ok();
    }
    Status unlockInMsAsync(int32_t ms) override {
        usleep(ms * 1000);
        blockMutex.unlock();
        return Status::ok();
    }
    Status lockUnlock() override {
        std::lock_guard<std::mutex> _l(blockMutex);
        return Status::ok();
    }

    Status sleepMs(int32_t ms) override {
        usleep(ms * 1000);
        return Status::ok();
    }

    Status sleepMsAsync(int32_t ms) override {
        // In-process binder calls are asynchronous, but the call to this method
        // is synchronous wrt its client. This in/out-process threading model
        // diffentiation is a classic binder leaky abstraction (for better or
        // worse) and is preserved here the way binder sockets plugs itself
        // into BpBinder, as nothing is changed at the higher levels
        // (IInterface) which result in this behavior.
        return sleepMs(ms);
    }

    Status doCallback(const sp<IBinderRpcCallback>& callback, bool oneway, bool delayed,
                      const std::string& value) override {
        if (callback == nullptr) {
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }

        if (delayed) {
            std::thread([=]() {
                ALOGE("Executing delayed callback: '%s'", value.c_str());
                Status status = doCallback(callback, oneway, false, value);
                ALOGE("Delayed callback status: '%s'", status.toString8().c_str());
            }).detach();
            return Status::ok();
        }

        if (oneway) {
            return callback->sendOnewayCallback(value);
        }

        return callback->sendCallback(value);
    }

    Status doCallbackAsync(const sp<IBinderRpcCallback>& callback, bool oneway, bool delayed,
                           const std::string& value) override {
        return doCallback(callback, oneway, delayed, value);
    }

    Status die(bool cleanup) override {
        if (cleanup) {
            exit(1);
        } else {
            _exit(1);
        }
    }

    Status scheduleShutdown() override {
        sp<RpcServer> strongServer = server.promote();
        if (strongServer == nullptr) {
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }
        std::thread([=] {
            LOG_ALWAYS_FATAL_IF(!strongServer->shutdown(), "Could not shutdown");
        }).detach();
        return Status::ok();
    }

    Status useKernelBinderCallingId() override {
        // this is WRONG! It does not make sense when using RPC binder, and
        // because it is SO wrong, and so much code calls this, it should abort!

        (void)IPCThreadState::self()->getCallingPid();
        return Status::ok();
    }

    Status echoAsFile(const std::string& content, android::os::ParcelFileDescriptor* out) override {
        out->reset(mockFileDescriptor(content));
        return Status::ok();
    }

    Status concatFiles(const std::vector<android::os::ParcelFileDescriptor>& files,
                       android::os::ParcelFileDescriptor* out) override {
        std::string acc;
        for (const auto& file : files) {
            std::string result;
            CHECK(android::base::ReadFdToString(file.get(), &result));
            acc.append(result);
        }
        out->reset(mockFileDescriptor(acc));
        return Status::ok();
    }
};
sp<IBinder> MyBinderRpcTest::mHeldBinder;

static std::string WaitStatusToString(int wstatus) {
    if (WIFEXITED(wstatus)) {
        return base::StringPrintf("exit status %d", WEXITSTATUS(wstatus));
    }
    if (WIFSIGNALED(wstatus)) {
        return base::StringPrintf("term signal %d", WTERMSIG(wstatus));
    }
    return base::StringPrintf("unexpected state %d", wstatus);
}

class Process {
public:
    Process(Process&&) = default;
    Process(const std::function<void(android::base::borrowed_fd /* writeEnd */,
                                     android::base::borrowed_fd /* readEnd */)>& f) {
        android::base::unique_fd childWriteEnd;
        android::base::unique_fd childReadEnd;
        CHECK(android::base::Pipe(&mReadEnd, &childWriteEnd)) << strerror(errno);
        CHECK(android::base::Pipe(&childReadEnd, &mWriteEnd)) << strerror(errno);
        if (0 == (mPid = fork())) {
            // racey: assume parent doesn't crash before this is set
            prctl(PR_SET_PDEATHSIG, SIGHUP);

            f(childWriteEnd, childReadEnd);

            exit(0);
        }
    }
    ~Process() {
        if (mPid != 0) {
            int wstatus;
            waitpid(mPid, &wstatus, 0);
            if (mCustomExitStatusCheck) {
                mCustomExitStatusCheck(wstatus);
            } else {
                EXPECT_TRUE(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
                        << "server process failed: " << WaitStatusToString(wstatus);
            }
        }
    }
    android::base::borrowed_fd readEnd() { return mReadEnd; }
    android::base::borrowed_fd writeEnd() { return mWriteEnd; }

    void setCustomExitStatusCheck(std::function<void(int wstatus)> f) {
        mCustomExitStatusCheck = std::move(f);
    }

    // Kill the process. Avoid if possible. Shutdown gracefully via an RPC instead.
    void terminate() { kill(mPid, SIGTERM); }

private:
    std::function<void(int wstatus)> mCustomExitStatusCheck;
    pid_t mPid = 0;
    android::base::unique_fd mReadEnd;
    android::base::unique_fd mWriteEnd;
};

static std::string allocateSocketAddress() {
    static size_t id = 0;
    std::string temp = getenv("TMPDIR") ?: "/tmp";
    auto ret = temp + "/binderRpcTest_" + std::to_string(id++);
    unlink(ret.c_str());
    return ret;
};

static unsigned int allocateVsockPort() {
    static unsigned int vsockPort = 3456;
    return vsockPort++;
}

struct ProcessSession {
    // reference to process hosting a socket server
    Process host;

    struct SessionInfo {
        sp<RpcSession> session;
        sp<IBinder> root;
    };

    // client session objects associated with other process
    // each one represents a separate session
    std::vector<SessionInfo> sessions;

    ProcessSession(ProcessSession&&) = default;
    ~ProcessSession() {
        for (auto& session : sessions) {
            session.root = nullptr;
        }

        for (auto& info : sessions) {
            sp<RpcSession>& session = info.session;

            EXPECT_NE(nullptr, session);
            EXPECT_NE(nullptr, session->state());
            EXPECT_EQ(0, session->state()->countBinders()) << (session->state()->dump(), "dump:");

            wp<RpcSession> weakSession = session;
            session = nullptr;
            EXPECT_EQ(nullptr, weakSession.promote()) << "Leaked session";
        }
    }
};

// Process session where the process hosts IBinderRpcTest, the server used
// for most testing here
struct BinderRpcTestProcessSession {
    ProcessSession proc;

    // pre-fetched root object (for first session)
    sp<IBinder> rootBinder;

    // pre-casted root object (for first session)
    sp<IBinderRpcTest> rootIface;

    // whether session should be invalidated by end of run
    bool expectAlreadyShutdown = false;

    BinderRpcTestProcessSession(BinderRpcTestProcessSession&&) = default;
    ~BinderRpcTestProcessSession() {
        if (!expectAlreadyShutdown) {
            EXPECT_NE(nullptr, rootIface);
            if (rootIface == nullptr) return;

            std::vector<int32_t> remoteCounts;
            // calling over any sessions counts across all sessions
            EXPECT_OK(rootIface->countBinders(&remoteCounts));
            EXPECT_EQ(remoteCounts.size(), proc.sessions.size());
            for (auto remoteCount : remoteCounts) {
                EXPECT_EQ(remoteCount, 1);
            }

            // even though it is on another thread, shutdown races with
            // the transaction reply being written
            if (auto status = rootIface->scheduleShutdown(); !status.isOk()) {
                EXPECT_EQ(DEAD_OBJECT, status.transactionError()) << status;
            }
        }

        rootIface = nullptr;
        rootBinder = nullptr;
    }
};

enum class SocketType {
    PRECONNECTED,
    UNIX,
    VSOCK,
    INET,
};
static inline std::string PrintToString(SocketType socketType) {
    switch (socketType) {
        case SocketType::PRECONNECTED:
            return "preconnected_uds";
        case SocketType::UNIX:
            return "unix_domain_socket";
        case SocketType::VSOCK:
            return "vm_socket";
        case SocketType::INET:
            return "inet_socket";
        default:
            LOG_ALWAYS_FATAL("Unknown socket type");
            return "";
    }
}

static base::unique_fd connectTo(const RpcSocketAddress& addr) {
    base::unique_fd serverFd(
            TEMP_FAILURE_RETRY(socket(addr.addr()->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0)));
    int savedErrno = errno;
    CHECK(serverFd.ok()) << "Could not create socket " << addr.toString() << ": "
                         << strerror(savedErrno);

    if (0 != TEMP_FAILURE_RETRY(connect(serverFd.get(), addr.addr(), addr.addrSize()))) {
        int savedErrno = errno;
        LOG(FATAL) << "Could not connect to socket " << addr.toString() << ": "
                   << strerror(savedErrno);
    }
    return serverFd;
}

class BinderRpc
      : public ::testing::TestWithParam<std::tuple<SocketType, RpcSecurity, uint32_t, uint32_t>> {
public:
    struct Options {
        size_t numThreads = 1;
        size_t numSessions = 1;
        size_t numIncomingConnections = 0;
        size_t numOutgoingConnections = SIZE_MAX;
        RpcSession::FileDescriptorTransportMode clientFileDescriptorTransportMode =
                RpcSession::FileDescriptorTransportMode::NONE;
        std::vector<RpcSession::FileDescriptorTransportMode>
                serverSupportedFileDescriptorTransportModes = {
                        RpcSession::FileDescriptorTransportMode::NONE};

        // If true, connection failures will result in `ProcessSession::sessions` being empty
        // instead of a fatal error.
        bool allowConnectFailure = false;
    };

    SocketType socketType() const { return std::get<0>(GetParam()); }
    RpcSecurity rpcSecurity() const { return std::get<1>(GetParam()); }
    uint32_t clientVersion() const { return std::get<2>(GetParam()); }
    uint32_t serverVersion() const { return std::get<3>(GetParam()); }

    // Whether the test params support sending FDs in parcels.
    bool supportsFdTransport() const {
        return clientVersion() >= 1 && serverVersion() >= 1 && rpcSecurity() != RpcSecurity::TLS &&
                (socketType() == SocketType::PRECONNECTED || socketType() == SocketType::UNIX);
    }

    static inline std::string PrintParamInfo(const testing::TestParamInfo<ParamType>& info) {
        auto [type, security, clientVersion, serverVersion] = info.param;
        return PrintToString(type) + "_" + newFactory(security)->toCString() + "_clientV" +
                std::to_string(clientVersion) + "_serverV" + std::to_string(serverVersion);
    }

    static inline void writeString(android::base::borrowed_fd fd, std::string_view str) {
        uint64_t length = str.length();
        CHECK(android::base::WriteFully(fd, &length, sizeof(length)));
        CHECK(android::base::WriteFully(fd, str.data(), str.length()));
    }

    static inline std::string readString(android::base::borrowed_fd fd) {
        uint64_t length;
        CHECK(android::base::ReadFully(fd, &length, sizeof(length)));
        std::string ret(length, '\0');
        CHECK(android::base::ReadFully(fd, ret.data(), length));
        return ret;
    }

    static inline void writeToFd(android::base::borrowed_fd fd, const Parcelable& parcelable) {
        Parcel parcel;
        CHECK_EQ(OK, parcelable.writeToParcel(&parcel));
        writeString(fd,
                    std::string(reinterpret_cast<const char*>(parcel.data()), parcel.dataSize()));
    }

    template <typename T>
    static inline T readFromFd(android::base::borrowed_fd fd) {
        std::string data = readString(fd);
        Parcel parcel;
        CHECK_EQ(OK, parcel.setData(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
        T object;
        CHECK_EQ(OK, object.readFromParcel(&parcel));
        return object;
    }

    // This creates a new process serving an interface on a certain number of
    // threads.
    ProcessSession createRpcTestSocketServerProcess(
            const Options& options, const std::function<void(const sp<RpcServer>&)>& configure) {
        CHECK_GE(options.numSessions, 1) << "Must have at least one session to a server";

        SocketType socketType = std::get<0>(GetParam());
        RpcSecurity rpcSecurity = std::get<1>(GetParam());
        uint32_t clientVersion = std::get<2>(GetParam());
        uint32_t serverVersion = std::get<3>(GetParam());

        unsigned int vsockPort = allocateVsockPort();
        std::string addr = allocateSocketAddress();

        auto ret = ProcessSession{
                .host = Process([=](android::base::borrowed_fd writeEnd,
                                    android::base::borrowed_fd readEnd) {
                    auto certVerifier = std::make_shared<RpcCertificateVerifierSimple>();
                    sp<RpcServer> server = RpcServer::make(newFactory(rpcSecurity, certVerifier));

                    server->setProtocolVersion(serverVersion);
                    server->setMaxThreads(options.numThreads);
                    server->setSupportedFileDescriptorTransportModes(
                            options.serverSupportedFileDescriptorTransportModes);

                    unsigned int outPort = 0;

                    switch (socketType) {
                        case SocketType::PRECONNECTED:
                            [[fallthrough]];
                        case SocketType::UNIX:
                            CHECK_EQ(OK, server->setupUnixDomainServer(addr.c_str())) << addr;
                            break;
                        case SocketType::VSOCK:
                            CHECK_EQ(OK, server->setupVsockServer(vsockPort));
                            break;
                        case SocketType::INET: {
                            CHECK_EQ(OK, server->setupInetServer(kLocalInetAddress, 0, &outPort));
                            CHECK_NE(0, outPort);
                            break;
                        }
                        default:
                            LOG_ALWAYS_FATAL("Unknown socket type");
                    }

                    BinderRpcTestServerInfo serverInfo;
                    serverInfo.port = static_cast<int64_t>(outPort);
                    serverInfo.cert.data = server->getCertificate(RpcCertificateFormat::PEM);
                    writeToFd(writeEnd, serverInfo);
                    auto clientInfo = readFromFd<BinderRpcTestClientInfo>(readEnd);

                    if (rpcSecurity == RpcSecurity::TLS) {
                        for (const auto& clientCert : clientInfo.certs) {
                            CHECK_EQ(OK,
                                     certVerifier
                                             ->addTrustedPeerCertificate(RpcCertificateFormat::PEM,
                                                                         clientCert.data));
                        }
                    }

                    configure(server);

                    server->join();

                    // Another thread calls shutdown. Wait for it to complete.
                    (void)server->shutdown();
                }),
        };

        std::vector<sp<RpcSession>> sessions;
        auto certVerifier = std::make_shared<RpcCertificateVerifierSimple>();
        for (size_t i = 0; i < options.numSessions; i++) {
            sessions.emplace_back(RpcSession::make(newFactory(rpcSecurity, certVerifier)));
        }

        auto serverInfo = readFromFd<BinderRpcTestServerInfo>(ret.host.readEnd());
        BinderRpcTestClientInfo clientInfo;
        for (const auto& session : sessions) {
            auto& parcelableCert = clientInfo.certs.emplace_back();
            parcelableCert.data = session->getCertificate(RpcCertificateFormat::PEM);
        }
        writeToFd(ret.host.writeEnd(), clientInfo);

        CHECK_LE(serverInfo.port, std::numeric_limits<unsigned int>::max());
        if (socketType == SocketType::INET) {
            CHECK_NE(0, serverInfo.port);
        }

        if (rpcSecurity == RpcSecurity::TLS) {
            const auto& serverCert = serverInfo.cert.data;
            CHECK_EQ(OK,
                     certVerifier->addTrustedPeerCertificate(RpcCertificateFormat::PEM,
                                                             serverCert));
        }

        status_t status;

        for (const auto& session : sessions) {
            CHECK(session->setProtocolVersion(clientVersion));
            session->setMaxIncomingThreads(options.numIncomingConnections);
            session->setMaxOutgoingThreads(options.numOutgoingConnections);
            session->setFileDescriptorTransportMode(options.clientFileDescriptorTransportMode);

            switch (socketType) {
                case SocketType::PRECONNECTED:
                    status = session->setupPreconnectedClient({}, [=]() {
                        return connectTo(UnixSocketAddress(addr.c_str()));
                    });
                    break;
                case SocketType::UNIX:
                    status = session->setupUnixDomainClient(addr.c_str());
                    break;
                case SocketType::VSOCK:
                    status = session->setupVsockClient(VMADDR_CID_LOCAL, vsockPort);
                    break;
                case SocketType::INET:
                    status = session->setupInetClient("127.0.0.1", serverInfo.port);
                    break;
                default:
                    LOG_ALWAYS_FATAL("Unknown socket type");
            }
            if (options.allowConnectFailure && status != OK) {
                ret.sessions.clear();
                break;
            }
            CHECK_EQ(status, OK) << "Could not connect: " << statusToString(status);
            ret.sessions.push_back({session, session->getRootObject()});
        }
        return ret;
    }

    BinderRpcTestProcessSession createRpcTestSocketServerProcess(const Options& options) {
        BinderRpcTestProcessSession ret{
                .proc = createRpcTestSocketServerProcess(
                        options,
                        [&](const sp<RpcServer>& server) {
                            server->setPerSessionRootObject([&](const void* addrPtr, size_t len) {
                                // UNIX sockets with abstract addresses return
                                // sizeof(sa_family_t)==2 in addrlen
                                CHECK_GE(len, sizeof(sa_family_t));
                                const sockaddr* addr = reinterpret_cast<const sockaddr*>(addrPtr);
                                sp<MyBinderRpcTest> service = sp<MyBinderRpcTest>::make();
                                switch (addr->sa_family) {
                                    case AF_UNIX:
                                        // nothing to save
                                        break;
                                    case AF_VSOCK:
                                        CHECK_EQ(len, sizeof(sockaddr_vm));
                                        service->port = reinterpret_cast<const sockaddr_vm*>(addr)
                                                                ->svm_port;
                                        break;
                                    case AF_INET:
                                        CHECK_EQ(len, sizeof(sockaddr_in));
                                        service->port =
                                                ntohs(reinterpret_cast<const sockaddr_in*>(addr)
                                                              ->sin_port);
                                        break;
                                    case AF_INET6:
                                        CHECK_EQ(len, sizeof(sockaddr_in));
                                        service->port =
                                                ntohs(reinterpret_cast<const sockaddr_in6*>(addr)
                                                              ->sin6_port);
                                        break;
                                    default:
                                        LOG_ALWAYS_FATAL("Unrecognized address family %d",
                                                         addr->sa_family);
                                }
                                service->server = server;
                                return service;
                            });
                        }),
        };

        ret.rootBinder = ret.proc.sessions.empty() ? nullptr : ret.proc.sessions.at(0).root;
        ret.rootIface = interface_cast<IBinderRpcTest>(ret.rootBinder);

        return ret;
    }

    void testThreadPoolOverSaturated(sp<IBinderRpcTest> iface, size_t numCalls,
                                     size_t sleepMs = 500);
};

TEST_P(BinderRpc, Ping) {
    auto proc = createRpcTestSocketServerProcess({});
    ASSERT_NE(proc.rootBinder, nullptr);
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());
}

TEST_P(BinderRpc, GetInterfaceDescriptor) {
    auto proc = createRpcTestSocketServerProcess({});
    ASSERT_NE(proc.rootBinder, nullptr);
    EXPECT_EQ(IBinderRpcTest::descriptor, proc.rootBinder->getInterfaceDescriptor());
}

TEST_P(BinderRpc, MultipleSessions) {
    auto proc = createRpcTestSocketServerProcess({.numThreads = 1, .numSessions = 5});
    for (auto session : proc.proc.sessions) {
        ASSERT_NE(nullptr, session.root);
        EXPECT_EQ(OK, session.root->pingBinder());
    }
}

TEST_P(BinderRpc, SeparateRootObject) {
    SocketType type = std::get<0>(GetParam());
    if (type == SocketType::PRECONNECTED || type == SocketType::UNIX) {
        // we can't get port numbers for unix sockets
        return;
    }

    auto proc = createRpcTestSocketServerProcess({.numSessions = 2});

    int port1 = 0;
    EXPECT_OK(proc.rootIface->getClientPort(&port1));

    sp<IBinderRpcTest> rootIface2 = interface_cast<IBinderRpcTest>(proc.proc.sessions.at(1).root);
    int port2;
    EXPECT_OK(rootIface2->getClientPort(&port2));

    // we should have a different IBinderRpcTest object created for each
    // session, because we use setPerSessionRootObject
    EXPECT_NE(port1, port2);
}

TEST_P(BinderRpc, TransactionsMustBeMarkedRpc) {
    auto proc = createRpcTestSocketServerProcess({});
    Parcel data;
    Parcel reply;
    EXPECT_EQ(BAD_TYPE, proc.rootBinder->transact(IBinder::PING_TRANSACTION, data, &reply, 0));
}

TEST_P(BinderRpc, AppendSeparateFormats) {
    auto proc1 = createRpcTestSocketServerProcess({});
    auto proc2 = createRpcTestSocketServerProcess({});

    Parcel pRaw;

    Parcel p1;
    p1.markForBinder(proc1.rootBinder);
    p1.writeInt32(3);

    EXPECT_EQ(BAD_TYPE, p1.appendFrom(&pRaw, 0, pRaw.dataSize()));
    EXPECT_EQ(BAD_TYPE, pRaw.appendFrom(&p1, 0, p1.dataSize()));

    Parcel p2;
    p2.markForBinder(proc2.rootBinder);
    p2.writeInt32(7);

    EXPECT_EQ(BAD_TYPE, p1.appendFrom(&p2, 0, p2.dataSize()));
    EXPECT_EQ(BAD_TYPE, p2.appendFrom(&p1, 0, p1.dataSize()));
}

TEST_P(BinderRpc, UnknownTransaction) {
    auto proc = createRpcTestSocketServerProcess({});
    Parcel data;
    data.markForBinder(proc.rootBinder);
    Parcel reply;
    EXPECT_EQ(UNKNOWN_TRANSACTION, proc.rootBinder->transact(1337, data, &reply, 0));
}

TEST_P(BinderRpc, SendSomethingOneway) {
    auto proc = createRpcTestSocketServerProcess({});
    EXPECT_OK(proc.rootIface->sendString("asdf"));
}

TEST_P(BinderRpc, SendAndGetResultBack) {
    auto proc = createRpcTestSocketServerProcess({});
    std::string doubled;
    EXPECT_OK(proc.rootIface->doubleString("cool ", &doubled));
    EXPECT_EQ("cool cool ", doubled);
}

TEST_P(BinderRpc, SendAndGetResultBackBig) {
    auto proc = createRpcTestSocketServerProcess({});
    std::string single = std::string(1024, 'a');
    std::string doubled;
    EXPECT_OK(proc.rootIface->doubleString(single, &doubled));
    EXPECT_EQ(single + single, doubled);
}

TEST_P(BinderRpc, InvalidNullBinderReturn) {
    auto proc = createRpcTestSocketServerProcess({});

    sp<IBinder> outBinder;
    EXPECT_EQ(proc.rootIface->getNullBinder(&outBinder).transactionError(), UNEXPECTED_NULL);
}

TEST_P(BinderRpc, CallMeBack) {
    auto proc = createRpcTestSocketServerProcess({});

    int32_t pingResult;
    EXPECT_OK(proc.rootIface->pingMe(new MyBinderRpcSession("foo"), &pingResult));
    EXPECT_EQ(OK, pingResult);

    EXPECT_EQ(0, MyBinderRpcSession::gNum);
}

TEST_P(BinderRpc, RepeatBinder) {
    auto proc = createRpcTestSocketServerProcess({});

    sp<IBinder> inBinder = new MyBinderRpcSession("foo");
    sp<IBinder> outBinder;
    EXPECT_OK(proc.rootIface->repeatBinder(inBinder, &outBinder));
    EXPECT_EQ(inBinder, outBinder);

    wp<IBinder> weak = inBinder;
    inBinder = nullptr;
    outBinder = nullptr;

    // Force reading a reply, to process any pending dec refs from the other
    // process (the other process will process dec refs there before processing
    // the ping here).
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());

    EXPECT_EQ(nullptr, weak.promote());

    EXPECT_EQ(0, MyBinderRpcSession::gNum);
}

TEST_P(BinderRpc, RepeatTheirBinder) {
    auto proc = createRpcTestSocketServerProcess({});

    sp<IBinderRpcSession> session;
    EXPECT_OK(proc.rootIface->openSession("aoeu", &session));

    sp<IBinder> inBinder = IInterface::asBinder(session);
    sp<IBinder> outBinder;
    EXPECT_OK(proc.rootIface->repeatBinder(inBinder, &outBinder));
    EXPECT_EQ(inBinder, outBinder);

    wp<IBinder> weak = inBinder;
    session = nullptr;
    inBinder = nullptr;
    outBinder = nullptr;

    // Force reading a reply, to process any pending dec refs from the other
    // process (the other process will process dec refs there before processing
    // the ping here).
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());

    EXPECT_EQ(nullptr, weak.promote());
}

TEST_P(BinderRpc, RepeatBinderNull) {
    auto proc = createRpcTestSocketServerProcess({});

    sp<IBinder> outBinder;
    EXPECT_OK(proc.rootIface->repeatBinder(nullptr, &outBinder));
    EXPECT_EQ(nullptr, outBinder);
}

TEST_P(BinderRpc, HoldBinder) {
    auto proc = createRpcTestSocketServerProcess({});

    IBinder* ptr = nullptr;
    {
        sp<IBinder> binder = new BBinder();
        ptr = binder.get();
        EXPECT_OK(proc.rootIface->holdBinder(binder));
    }

    sp<IBinder> held;
    EXPECT_OK(proc.rootIface->getHeldBinder(&held));

    EXPECT_EQ(held.get(), ptr);

    // stop holding binder, because we test to make sure references are cleaned
    // up
    EXPECT_OK(proc.rootIface->holdBinder(nullptr));
    // and flush ref counts
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());
}

// START TESTS FOR LIMITATIONS OF SOCKET BINDER
// These are behavioral differences form regular binder, where certain usecases
// aren't supported.

TEST_P(BinderRpc, CannotMixBindersBetweenUnrelatedSocketSessions) {
    auto proc1 = createRpcTestSocketServerProcess({});
    auto proc2 = createRpcTestSocketServerProcess({});

    sp<IBinder> outBinder;
    EXPECT_EQ(INVALID_OPERATION,
              proc1.rootIface->repeatBinder(proc2.rootBinder, &outBinder).transactionError());
}

TEST_P(BinderRpc, CannotMixBindersBetweenTwoSessionsToTheSameServer) {
    auto proc = createRpcTestSocketServerProcess({.numThreads = 1, .numSessions = 2});

    sp<IBinder> outBinder;
    EXPECT_EQ(INVALID_OPERATION,
              proc.rootIface->repeatBinder(proc.proc.sessions.at(1).root, &outBinder)
                      .transactionError());
}

TEST_P(BinderRpc, CannotSendRegularBinderOverSocketBinder) {
    auto proc = createRpcTestSocketServerProcess({});

    sp<IBinder> someRealBinder = IInterface::asBinder(defaultServiceManager());
    sp<IBinder> outBinder;
    EXPECT_EQ(INVALID_OPERATION,
              proc.rootIface->repeatBinder(someRealBinder, &outBinder).transactionError());
}

TEST_P(BinderRpc, CannotSendSocketBinderOverRegularBinder) {
    auto proc = createRpcTestSocketServerProcess({});

    // for historical reasons, IServiceManager interface only returns the
    // exception code
    EXPECT_EQ(binder::Status::EX_TRANSACTION_FAILED,
              defaultServiceManager()->addService(String16("not_suspicious"), proc.rootBinder));
}

// END TESTS FOR LIMITATIONS OF SOCKET BINDER

TEST_P(BinderRpc, RepeatRootObject) {
    auto proc = createRpcTestSocketServerProcess({});

    sp<IBinder> outBinder;
    EXPECT_OK(proc.rootIface->repeatBinder(proc.rootBinder, &outBinder));
    EXPECT_EQ(proc.rootBinder, outBinder);
}

TEST_P(BinderRpc, NestedTransactions) {
    auto proc = createRpcTestSocketServerProcess({
            // Enable FD support because it uses more stack space and so represents
            // something closer to a worst case scenario.
            .clientFileDescriptorTransportMode = RpcSession::FileDescriptorTransportMode::UNIX,
            .serverSupportedFileDescriptorTransportModes =
                    {RpcSession::FileDescriptorTransportMode::UNIX},
    });

    auto nastyNester = sp<MyBinderRpcTest>::make();
    EXPECT_OK(proc.rootIface->nestMe(nastyNester, 10));

    wp<IBinder> weak = nastyNester;
    nastyNester = nullptr;
    EXPECT_EQ(nullptr, weak.promote());
}

TEST_P(BinderRpc, SameBinderEquality) {
    auto proc = createRpcTestSocketServerProcess({});

    sp<IBinder> a;
    EXPECT_OK(proc.rootIface->alwaysGiveMeTheSameBinder(&a));

    sp<IBinder> b;
    EXPECT_OK(proc.rootIface->alwaysGiveMeTheSameBinder(&b));

    EXPECT_EQ(a, b);
}

TEST_P(BinderRpc, SameBinderEqualityWeak) {
    auto proc = createRpcTestSocketServerProcess({});

    sp<IBinder> a;
    EXPECT_OK(proc.rootIface->alwaysGiveMeTheSameBinder(&a));
    wp<IBinder> weak = a;
    a = nullptr;

    sp<IBinder> b;
    EXPECT_OK(proc.rootIface->alwaysGiveMeTheSameBinder(&b));

    // this is the wrong behavior, since BpBinder
    // doesn't implement onIncStrongAttempted
    // but make sure there is no crash
    EXPECT_EQ(nullptr, weak.promote());

    GTEST_SKIP() << "Weak binders aren't currently re-promotable for RPC binder.";

    // In order to fix this:
    // - need to have incStrongAttempted reflected across IPC boundary (wait for
    //   response to promote - round trip...)
    // - sendOnLastWeakRef, to delete entries out of RpcState table
    EXPECT_EQ(b, weak.promote());
}

#define expectSessions(expected, iface)                   \
    do {                                                  \
        int session;                                      \
        EXPECT_OK((iface)->getNumOpenSessions(&session)); \
        EXPECT_EQ(expected, session);                     \
    } while (false)

TEST_P(BinderRpc, SingleSession) {
    auto proc = createRpcTestSocketServerProcess({});

    sp<IBinderRpcSession> session;
    EXPECT_OK(proc.rootIface->openSession("aoeu", &session));
    std::string out;
    EXPECT_OK(session->getName(&out));
    EXPECT_EQ("aoeu", out);

    expectSessions(1, proc.rootIface);
    session = nullptr;
    expectSessions(0, proc.rootIface);
}

TEST_P(BinderRpc, ManySessions) {
    auto proc = createRpcTestSocketServerProcess({});

    std::vector<sp<IBinderRpcSession>> sessions;

    for (size_t i = 0; i < 15; i++) {
        expectSessions(i, proc.rootIface);
        sp<IBinderRpcSession> session;
        EXPECT_OK(proc.rootIface->openSession(std::to_string(i), &session));
        sessions.push_back(session);
    }
    expectSessions(sessions.size(), proc.rootIface);
    for (size_t i = 0; i < sessions.size(); i++) {
        std::string out;
        EXPECT_OK(sessions.at(i)->getName(&out));
        EXPECT_EQ(std::to_string(i), out);
    }
    expectSessions(sessions.size(), proc.rootIface);

    while (!sessions.empty()) {
        sessions.pop_back();
        expectSessions(sessions.size(), proc.rootIface);
    }
    expectSessions(0, proc.rootIface);
}

size_t epochMillis() {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    using std::chrono::system_clock;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

TEST_P(BinderRpc, ThreadPoolGreaterThanEqualRequested) {
    constexpr size_t kNumThreads = 10;

    auto proc = createRpcTestSocketServerProcess({.numThreads = kNumThreads});

    EXPECT_OK(proc.rootIface->lock());

    // block all but one thread taking locks
    std::vector<std::thread> ts;
    for (size_t i = 0; i < kNumThreads - 1; i++) {
        ts.push_back(std::thread([&] { proc.rootIface->lockUnlock(); }));
    }

    usleep(100000); // give chance for calls on other threads

    // other calls still work
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());

    constexpr size_t blockTimeMs = 500;
    size_t epochMsBefore = epochMillis();
    // after this, we should never see a response within this time
    EXPECT_OK(proc.rootIface->unlockInMsAsync(blockTimeMs));

    // this call should be blocked for blockTimeMs
    EXPECT_EQ(OK, proc.rootBinder->pingBinder());

    size_t epochMsAfter = epochMillis();
    EXPECT_GE(epochMsAfter, epochMsBefore + blockTimeMs) << epochMsBefore;

    for (auto& t : ts) t.join();
}

void BinderRpc::testThreadPoolOverSaturated(sp<IBinderRpcTest> iface, size_t numCalls,
                                            size_t sleepMs) {
    size_t epochMsBefore = epochMillis();

    std::vector<std::thread> ts;
    for (size_t i = 0; i < numCalls; i++) {
        ts.push_back(std::thread([&] { iface->sleepMs(sleepMs); }));
    }

    for (auto& t : ts) t.join();

    size_t epochMsAfter = epochMillis();

    EXPECT_GE(epochMsAfter, epochMsBefore + 2 * sleepMs);

    // Potential flake, but make sure calls are handled in parallel.
    EXPECT_LE(epochMsAfter, epochMsBefore + 3 * sleepMs);
}

TEST_P(BinderRpc, ThreadPoolOverSaturated) {
    constexpr size_t kNumThreads = 10;
    constexpr size_t kNumCalls = kNumThreads + 3;
    auto proc = createRpcTestSocketServerProcess({.numThreads = kNumThreads});
    testThreadPoolOverSaturated(proc.rootIface, kNumCalls);
}

TEST_P(BinderRpc, ThreadPoolLimitOutgoing) {
    constexpr size_t kNumThreads = 20;
    constexpr size_t kNumOutgoingConnections = 10;
    constexpr size_t kNumCalls = kNumOutgoingConnections + 3;
    auto proc = createRpcTestSocketServerProcess(
            {.numThreads = kNumThreads, .numOutgoingConnections = kNumOutgoingConnections});
    testThreadPoolOverSaturated(proc.rootIface, kNumCalls);
}

TEST_P(BinderRpc, ThreadingStressTest) {
    constexpr size_t kNumClientThreads = 10;
    constexpr size_t kNumServerThreads = 10;
    constexpr size_t kNumCalls = 100;

    auto proc = createRpcTestSocketServerProcess({.numThreads = kNumServerThreads});

    std::vector<std::thread> threads;
    for (size_t i = 0; i < kNumClientThreads; i++) {
        threads.push_back(std::thread([&] {
            for (size_t j = 0; j < kNumCalls; j++) {
                sp<IBinder> out;
                EXPECT_OK(proc.rootIface->repeatBinder(proc.rootBinder, &out));
                EXPECT_EQ(proc.rootBinder, out);
            }
        }));
    }

    for (auto& t : threads) t.join();
}

static void saturateThreadPool(size_t threadCount, const sp<IBinderRpcTest>& iface) {
    std::vector<std::thread> threads;
    for (size_t i = 0; i < threadCount; i++) {
        threads.push_back(std::thread([&] { EXPECT_OK(iface->sleepMs(500)); }));
    }
    for (auto& t : threads) t.join();
}

TEST_P(BinderRpc, OnewayStressTest) {
    constexpr size_t kNumClientThreads = 10;
    constexpr size_t kNumServerThreads = 10;
    constexpr size_t kNumCalls = 1000;

    auto proc = createRpcTestSocketServerProcess({.numThreads = kNumServerThreads});

    std::vector<std::thread> threads;
    for (size_t i = 0; i < kNumClientThreads; i++) {
        threads.push_back(std::thread([&] {
            for (size_t j = 0; j < kNumCalls; j++) {
                EXPECT_OK(proc.rootIface->sendString("a"));
            }
        }));
    }

    for (auto& t : threads) t.join();

    saturateThreadPool(kNumServerThreads, proc.rootIface);
}

TEST_P(BinderRpc, OnewayCallDoesNotWait) {
    constexpr size_t kReallyLongTimeMs = 100;
    constexpr size_t kSleepMs = kReallyLongTimeMs * 5;

    auto proc = createRpcTestSocketServerProcess({});

    size_t epochMsBefore = epochMillis();

    EXPECT_OK(proc.rootIface->sleepMsAsync(kSleepMs));

    size_t epochMsAfter = epochMillis();
    EXPECT_LT(epochMsAfter, epochMsBefore + kReallyLongTimeMs);
}

TEST_P(BinderRpc, OnewayCallQueueing) {
    constexpr size_t kNumSleeps = 10;
    constexpr size_t kNumExtraServerThreads = 4;
    constexpr size_t kSleepMs = 50;

    // make sure calls to the same object happen on the same thread
    auto proc = createRpcTestSocketServerProcess({.numThreads = 1 + kNumExtraServerThreads});

    EXPECT_OK(proc.rootIface->lock());

    size_t epochMsBefore = epochMillis();

    // all these *Async commands should be queued on the server sequentially,
    // even though there are multiple threads.
    for (size_t i = 0; i + 1 < kNumSleeps; i++) {
        proc.rootIface->sleepMsAsync(kSleepMs);
    }
    EXPECT_OK(proc.rootIface->unlockInMsAsync(kSleepMs));

    // this can only return once the final async call has unlocked
    EXPECT_OK(proc.rootIface->lockUnlock());

    size_t epochMsAfter = epochMillis();

    EXPECT_GT(epochMsAfter, epochMsBefore + kSleepMs * kNumSleeps);

    saturateThreadPool(1 + kNumExtraServerThreads, proc.rootIface);
}

TEST_P(BinderRpc, OnewayCallExhaustion) {
    constexpr size_t kNumClients = 2;
    constexpr size_t kTooLongMs = 1000;

    auto proc = createRpcTestSocketServerProcess({.numThreads = kNumClients, .numSessions = 2});

    // Build up oneway calls on the second session to make sure it terminates
    // and shuts down. The first session should be unaffected (proc destructor
    // checks the first session).
    auto iface = interface_cast<IBinderRpcTest>(proc.proc.sessions.at(1).root);

    std::vector<std::thread> threads;
    for (size_t i = 0; i < kNumClients; i++) {
        // one of these threads will get stuck queueing a transaction once the
        // socket fills up, the other will be able to fill up transactions on
        // this object
        threads.push_back(std::thread([&] {
            while (iface->sleepMsAsync(kTooLongMs).isOk()) {
            }
        }));
    }
    for (auto& t : threads) t.join();

    Status status = iface->sleepMsAsync(kTooLongMs);
    EXPECT_EQ(DEAD_OBJECT, status.transactionError()) << status;

    // now that it has died, wait for the remote session to shutdown
    std::vector<int32_t> remoteCounts;
    do {
        EXPECT_OK(proc.rootIface->countBinders(&remoteCounts));
    } while (remoteCounts.size() == kNumClients);

    // the second session should be shutdown in the other process by the time we
    // are able to join above (it'll only be hung up once it finishes processing
    // any pending commands). We need to erase this session from the record
    // here, so that the destructor for our session won't check that this
    // session is valid, but we still want it to test the other session.
    proc.proc.sessions.erase(proc.proc.sessions.begin() + 1);
}

TEST_P(BinderRpc, Callbacks) {
    const static std::string kTestString = "good afternoon!";

    for (bool callIsOneway : {true, false}) {
        for (bool callbackIsOneway : {true, false}) {
            for (bool delayed : {true, false}) {
                auto proc = createRpcTestSocketServerProcess(
                        {.numThreads = 1, .numSessions = 1, .numIncomingConnections = 1});
                auto cb = sp<MyBinderRpcCallback>::make();

                if (callIsOneway) {
                    EXPECT_OK(proc.rootIface->doCallbackAsync(cb, callbackIsOneway, delayed,
                                                              kTestString));
                } else {
                    EXPECT_OK(
                            proc.rootIface->doCallback(cb, callbackIsOneway, delayed, kTestString));
                }

                // if both transactions are synchronous and the response is sent back on the
                // same thread, everything should have happened in a nested call. Otherwise,
                // the callback will be processed on another thread.
                if (callIsOneway || callbackIsOneway || delayed) {
                    using std::literals::chrono_literals::operator""s;
                    std::unique_lock<std::mutex> _l(cb->mMutex);
                    cb->mCv.wait_for(_l, 1s, [&] { return !cb->mValues.empty(); });
                }

                EXPECT_EQ(cb->mValues.size(), 1)
                        << "callIsOneway: " << callIsOneway
                        << " callbackIsOneway: " << callbackIsOneway << " delayed: " << delayed;
                if (cb->mValues.empty()) continue;
                EXPECT_EQ(cb->mValues.at(0), kTestString)
                        << "callIsOneway: " << callIsOneway
                        << " callbackIsOneway: " << callbackIsOneway << " delayed: " << delayed;

                // since we are severing the connection, we need to go ahead and
                // tell the server to shutdown and exit so that waitpid won't hang
                if (auto status = proc.rootIface->scheduleShutdown(); !status.isOk()) {
                    EXPECT_EQ(DEAD_OBJECT, status.transactionError()) << status;
                }

                // since this session has an incoming connection w/ a threadpool, we
                // need to manually shut it down
                EXPECT_TRUE(proc.proc.sessions.at(0).session->shutdownAndWait(true));

                proc.proc.host.setCustomExitStatusCheck([](int wstatus) {
                    // Flaky. Sometimes gets SIGABRT.
                    EXPECT_TRUE((WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) ||
                                (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGABRT))
                            << "server process failed: " << WaitStatusToString(wstatus);
                });
                proc.expectAlreadyShutdown = true;
            }
        }
    }
}

TEST_P(BinderRpc, OnewayCallbackWithNoThread) {
    auto proc = createRpcTestSocketServerProcess({});
    auto cb = sp<MyBinderRpcCallback>::make();

    Status status = proc.rootIface->doCallback(cb, true /*oneway*/, false /*delayed*/, "anything");
    EXPECT_EQ(WOULD_BLOCK, status.transactionError());
}

TEST_P(BinderRpc, Die) {
    for (bool doDeathCleanup : {true, false}) {
        auto proc = createRpcTestSocketServerProcess({});

        // make sure there is some state during crash
        // 1. we hold their binder
        sp<IBinderRpcSession> session;
        EXPECT_OK(proc.rootIface->openSession("happy", &session));
        // 2. they hold our binder
        sp<IBinder> binder = new BBinder();
        EXPECT_OK(proc.rootIface->holdBinder(binder));

        EXPECT_EQ(DEAD_OBJECT, proc.rootIface->die(doDeathCleanup).transactionError())
                << "Do death cleanup: " << doDeathCleanup;

        proc.proc.host.setCustomExitStatusCheck([](int wstatus) {
            EXPECT_TRUE(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 1)
                    << "server process failed incorrectly: " << WaitStatusToString(wstatus);
        });
        proc.expectAlreadyShutdown = true;
    }
}

TEST_P(BinderRpc, UseKernelBinderCallingId) {
    bool okToFork = ProcessState::selfOrNull() == nullptr;

    auto proc = createRpcTestSocketServerProcess({});

    // If this process has used ProcessState already, then the forked process
    // cannot use it at all. If this process hasn't used it (depending on the
    // order tests are run), then the forked process can use it, and we'll only
    // catch the invalid usage the second time. Such is the burden of global
    // state!
    if (okToFork) {
        // we can't allocate IPCThreadState so actually the first time should
        // succeed :(
        EXPECT_OK(proc.rootIface->useKernelBinderCallingId());
    }

    // second time! we catch the error :)
    EXPECT_EQ(DEAD_OBJECT, proc.rootIface->useKernelBinderCallingId().transactionError());

    proc.proc.host.setCustomExitStatusCheck([](int wstatus) {
        EXPECT_TRUE(WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGABRT)
                << "server process failed incorrectly: " << WaitStatusToString(wstatus);
    });
    proc.expectAlreadyShutdown = true;
}

TEST_P(BinderRpc, FileDescriptorTransportRejectNone) {
    auto proc = createRpcTestSocketServerProcess({
            .clientFileDescriptorTransportMode = RpcSession::FileDescriptorTransportMode::NONE,
            .serverSupportedFileDescriptorTransportModes =
                    {RpcSession::FileDescriptorTransportMode::UNIX},
            .allowConnectFailure = true,
    });
    EXPECT_TRUE(proc.proc.sessions.empty()) << "session connections should have failed";
    proc.proc.host.terminate();
    proc.proc.host.setCustomExitStatusCheck([](int wstatus) {
        EXPECT_TRUE(WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGTERM)
                << "server process failed incorrectly: " << WaitStatusToString(wstatus);
    });
    proc.expectAlreadyShutdown = true;
}

TEST_P(BinderRpc, FileDescriptorTransportRejectUnix) {
    auto proc = createRpcTestSocketServerProcess({
            .clientFileDescriptorTransportMode = RpcSession::FileDescriptorTransportMode::UNIX,
            .serverSupportedFileDescriptorTransportModes =
                    {RpcSession::FileDescriptorTransportMode::NONE},
            .allowConnectFailure = true,
    });
    EXPECT_TRUE(proc.proc.sessions.empty()) << "session connections should have failed";
    proc.proc.host.terminate();
    proc.proc.host.setCustomExitStatusCheck([](int wstatus) {
        EXPECT_TRUE(WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGTERM)
                << "server process failed incorrectly: " << WaitStatusToString(wstatus);
    });
    proc.expectAlreadyShutdown = true;
}

TEST_P(BinderRpc, FileDescriptorTransportOptionalUnix) {
    auto proc = createRpcTestSocketServerProcess({
            .clientFileDescriptorTransportMode = RpcSession::FileDescriptorTransportMode::NONE,
            .serverSupportedFileDescriptorTransportModes =
                    {RpcSession::FileDescriptorTransportMode::NONE,
                     RpcSession::FileDescriptorTransportMode::UNIX},
    });

    android::os::ParcelFileDescriptor out;
    auto status = proc.rootIface->echoAsFile("hello", &out);
    EXPECT_EQ(status.transactionError(), FDS_NOT_ALLOWED) << status;
}

TEST_P(BinderRpc, ReceiveFile) {
    auto proc = createRpcTestSocketServerProcess({
            .clientFileDescriptorTransportMode = RpcSession::FileDescriptorTransportMode::UNIX,
            .serverSupportedFileDescriptorTransportModes =
                    {RpcSession::FileDescriptorTransportMode::UNIX},
    });

    android::os::ParcelFileDescriptor out;
    auto status = proc.rootIface->echoAsFile("hello", &out);
    if (!supportsFdTransport()) {
        EXPECT_EQ(status.transactionError(), BAD_VALUE) << status;
        return;
    }
    ASSERT_TRUE(status.isOk()) << status;

    std::string result;
    CHECK(android::base::ReadFdToString(out.get(), &result));
    EXPECT_EQ(result, "hello");
}

TEST_P(BinderRpc, SendFiles) {
    auto proc = createRpcTestSocketServerProcess({
            .clientFileDescriptorTransportMode = RpcSession::FileDescriptorTransportMode::UNIX,
            .serverSupportedFileDescriptorTransportModes =
                    {RpcSession::FileDescriptorTransportMode::UNIX},
    });

    std::vector<android::os::ParcelFileDescriptor> files;
    files.emplace_back(android::os::ParcelFileDescriptor(mockFileDescriptor("123")));
    files.emplace_back(android::os::ParcelFileDescriptor(mockFileDescriptor("a")));
    files.emplace_back(android::os::ParcelFileDescriptor(mockFileDescriptor("b")));
    files.emplace_back(android::os::ParcelFileDescriptor(mockFileDescriptor("cd")));

    android::os::ParcelFileDescriptor out;
    auto status = proc.rootIface->concatFiles(files, &out);
    if (!supportsFdTransport()) {
        EXPECT_EQ(status.transactionError(), BAD_VALUE) << status;
        return;
    }
    ASSERT_TRUE(status.isOk()) << status;

    std::string result;
    CHECK(android::base::ReadFdToString(out.get(), &result));
    EXPECT_EQ(result, "123abcd");
}

TEST_P(BinderRpc, SendMaxFiles) {
    if (!supportsFdTransport()) {
        GTEST_SKIP() << "Would fail trivially (which is tested by BinderRpc::SendFiles)";
    }

    auto proc = createRpcTestSocketServerProcess({
            .clientFileDescriptorTransportMode = RpcSession::FileDescriptorTransportMode::UNIX,
            .serverSupportedFileDescriptorTransportModes =
                    {RpcSession::FileDescriptorTransportMode::UNIX},
    });

    std::vector<android::os::ParcelFileDescriptor> files;
    for (int i = 0; i < 253; i++) {
        files.emplace_back(android::os::ParcelFileDescriptor(mockFileDescriptor("a")));
    }

    android::os::ParcelFileDescriptor out;
    auto status = proc.rootIface->concatFiles(files, &out);
    ASSERT_TRUE(status.isOk()) << status;

    std::string result;
    CHECK(android::base::ReadFdToString(out.get(), &result));
    EXPECT_EQ(result, std::string(253, 'a'));
}

TEST_P(BinderRpc, SendTooManyFiles) {
    if (!supportsFdTransport()) {
        GTEST_SKIP() << "Would fail trivially (which is tested by BinderRpc::SendFiles)";
    }

    auto proc = createRpcTestSocketServerProcess({
            .clientFileDescriptorTransportMode = RpcSession::FileDescriptorTransportMode::UNIX,
            .serverSupportedFileDescriptorTransportModes =
                    {RpcSession::FileDescriptorTransportMode::UNIX},
    });

    std::vector<android::os::ParcelFileDescriptor> files;
    for (int i = 0; i < 254; i++) {
        files.emplace_back(android::os::ParcelFileDescriptor(mockFileDescriptor("a")));
    }

    android::os::ParcelFileDescriptor out;
    auto status = proc.rootIface->concatFiles(files, &out);
    EXPECT_EQ(status.transactionError(), BAD_VALUE) << status;
}

TEST_P(BinderRpc, WorksWithLibbinderNdkPing) {
    auto proc = createRpcTestSocketServerProcess({});

    ndk::SpAIBinder binder = ndk::SpAIBinder(AIBinder_fromPlatformBinder(proc.rootBinder));
    ASSERT_NE(binder, nullptr);

    ASSERT_EQ(STATUS_OK, AIBinder_ping(binder.get()));
}

TEST_P(BinderRpc, WorksWithLibbinderNdkUserTransaction) {
    auto proc = createRpcTestSocketServerProcess({});

    ndk::SpAIBinder binder = ndk::SpAIBinder(AIBinder_fromPlatformBinder(proc.rootBinder));
    ASSERT_NE(binder, nullptr);

    auto ndkBinder = aidl::IBinderRpcTest::fromBinder(binder);
    ASSERT_NE(ndkBinder, nullptr);

    std::string out;
    ndk::ScopedAStatus status = ndkBinder->doubleString("aoeu", &out);
    ASSERT_TRUE(status.isOk()) << status.getDescription();
    ASSERT_EQ("aoeuaoeu", out);
}

ssize_t countFds() {
    DIR* dir = opendir("/proc/self/fd/");
    if (dir == nullptr) return -1;
    ssize_t ret = 0;
    dirent* ent;
    while ((ent = readdir(dir)) != nullptr) ret++;
    closedir(dir);
    return ret;
}

TEST_P(BinderRpc, Fds) {
    ssize_t beforeFds = countFds();
    ASSERT_GE(beforeFds, 0);
    {
        auto proc = createRpcTestSocketServerProcess({.numThreads = 10});
        ASSERT_EQ(OK, proc.rootBinder->pingBinder());
    }
    ASSERT_EQ(beforeFds, countFds()) << (system("ls -l /proc/self/fd/"), "fd leak?");
}

TEST_P(BinderRpc, AidlDelegatorTest) {
    auto proc = createRpcTestSocketServerProcess({});
    auto myDelegator = sp<IBinderRpcTestDelegator>::make(proc.rootIface);
    ASSERT_NE(nullptr, myDelegator);

    std::string doubled;
    EXPECT_OK(myDelegator->doubleString("cool ", &doubled));
    EXPECT_EQ("cool cool ", doubled);
}

static bool testSupportVsockLoopback() {
    // We don't need to enable TLS to know if vsock is supported.
    unsigned int vsockPort = allocateVsockPort();

    android::base::unique_fd serverFd(
            TEMP_FAILURE_RETRY(socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)));
    LOG_ALWAYS_FATAL_IF(serverFd == -1, "Could not create socket: %s", strerror(errno));

    sockaddr_vm serverAddr{
            .svm_family = AF_VSOCK,
            .svm_port = vsockPort,
            .svm_cid = VMADDR_CID_ANY,
    };
    int ret = TEMP_FAILURE_RETRY(
            bind(serverFd.get(), reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)));
    LOG_ALWAYS_FATAL_IF(0 != ret, "Could not bind socket to port %u: %s", vsockPort,
                        strerror(errno));

    ret = TEMP_FAILURE_RETRY(listen(serverFd.get(), 1 /*backlog*/));
    LOG_ALWAYS_FATAL_IF(0 != ret, "Could not listen socket on port %u: %s", vsockPort,
                        strerror(errno));

    // Try to connect to the server using the VMADDR_CID_LOCAL cid
    // to see if the kernel supports it. It's safe to use a blocking
    // connect because vsock sockets have a 2 second connection timeout,
    // and they return ETIMEDOUT after that.
    android::base::unique_fd connectFd(
            TEMP_FAILURE_RETRY(socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)));
    LOG_ALWAYS_FATAL_IF(connectFd == -1, "Could not create socket for port %u: %s", vsockPort,
                        strerror(errno));

    bool success = false;
    sockaddr_vm connectAddr{
            .svm_family = AF_VSOCK,
            .svm_port = vsockPort,
            .svm_cid = VMADDR_CID_LOCAL,
    };
    ret = TEMP_FAILURE_RETRY(connect(connectFd.get(), reinterpret_cast<sockaddr*>(&connectAddr),
                                     sizeof(connectAddr)));
    if (ret != 0 && (errno == EAGAIN || errno == EINPROGRESS)) {
        android::base::unique_fd acceptFd;
        while (true) {
            pollfd pfd[]{
                    {.fd = serverFd.get(), .events = POLLIN, .revents = 0},
                    {.fd = connectFd.get(), .events = POLLOUT, .revents = 0},
            };
            ret = TEMP_FAILURE_RETRY(poll(pfd, arraysize(pfd), -1));
            LOG_ALWAYS_FATAL_IF(ret < 0, "Error polling: %s", strerror(errno));

            if (pfd[0].revents & POLLIN) {
                sockaddr_vm acceptAddr;
                socklen_t acceptAddrLen = sizeof(acceptAddr);
                ret = TEMP_FAILURE_RETRY(accept4(serverFd.get(),
                                                 reinterpret_cast<sockaddr*>(&acceptAddr),
                                                 &acceptAddrLen, SOCK_CLOEXEC));
                LOG_ALWAYS_FATAL_IF(ret < 0, "Could not accept4 socket: %s", strerror(errno));
                LOG_ALWAYS_FATAL_IF(acceptAddrLen != static_cast<socklen_t>(sizeof(acceptAddr)),
                                    "Truncated address");

                // Store the fd in acceptFd so we keep the connection alive
                // while polling connectFd
                acceptFd.reset(ret);
            }

            if (pfd[1].revents & POLLOUT) {
                // Connect either succeeded or timed out
                int connectErrno;
                socklen_t connectErrnoLen = sizeof(connectErrno);
                int ret = getsockopt(connectFd.get(), SOL_SOCKET, SO_ERROR, &connectErrno,
                                     &connectErrnoLen);
                LOG_ALWAYS_FATAL_IF(ret == -1,
                                    "Could not getsockopt() after connect() "
                                    "on non-blocking socket: %s.",
                                    strerror(errno));

                // We're done, this is all we wanted
                success = connectErrno == 0;
                break;
            }
        }
    } else {
        success = ret == 0;
    }

    ALOGE("Detected vsock loopback supported: %s", success ? "yes" : "no");

    return success;
}

static std::vector<SocketType> testSocketTypes(bool hasPreconnected = true) {
    std::vector<SocketType> ret = {SocketType::UNIX, SocketType::INET};

    if (hasPreconnected) ret.push_back(SocketType::PRECONNECTED);

    static bool hasVsockLoopback = testSupportVsockLoopback();

    if (hasVsockLoopback) {
        ret.push_back(SocketType::VSOCK);
    }

    return ret;
}

static std::vector<uint32_t> testVersions() {
    std::vector<uint32_t> versions;
    for (size_t i = 0; i < RPC_WIRE_PROTOCOL_VERSION_NEXT; i++) {
        versions.push_back(i);
    }
    versions.push_back(RPC_WIRE_PROTOCOL_VERSION_EXPERIMENTAL);
    return versions;
}

INSTANTIATE_TEST_CASE_P(PerSocket, BinderRpc,
                        ::testing::Combine(::testing::ValuesIn(testSocketTypes()),
                                           ::testing::ValuesIn(RpcSecurityValues()),
                                           ::testing::ValuesIn(testVersions()),
                                           ::testing::ValuesIn(testVersions())),
                        BinderRpc::PrintParamInfo);

class BinderRpcServerRootObject
      : public ::testing::TestWithParam<std::tuple<bool, bool, RpcSecurity>> {};

TEST_P(BinderRpcServerRootObject, WeakRootObject) {
    using SetFn = std::function<void(RpcServer*, sp<IBinder>)>;
    auto setRootObject = [](bool isStrong) -> SetFn {
        return isStrong ? SetFn(&RpcServer::setRootObject) : SetFn(&RpcServer::setRootObjectWeak);
    };

    auto [isStrong1, isStrong2, rpcSecurity] = GetParam();
    auto server = RpcServer::make(newFactory(rpcSecurity));
    auto binder1 = sp<BBinder>::make();
    IBinder* binderRaw1 = binder1.get();
    setRootObject(isStrong1)(server.get(), binder1);
    EXPECT_EQ(binderRaw1, server->getRootObject());
    binder1.clear();
    EXPECT_EQ((isStrong1 ? binderRaw1 : nullptr), server->getRootObject());

    auto binder2 = sp<BBinder>::make();
    IBinder* binderRaw2 = binder2.get();
    setRootObject(isStrong2)(server.get(), binder2);
    EXPECT_EQ(binderRaw2, server->getRootObject());
    binder2.clear();
    EXPECT_EQ((isStrong2 ? binderRaw2 : nullptr), server->getRootObject());
}

INSTANTIATE_TEST_CASE_P(BinderRpc, BinderRpcServerRootObject,
                        ::testing::Combine(::testing::Bool(), ::testing::Bool(),
                                           ::testing::ValuesIn(RpcSecurityValues())));

class OneOffSignal {
public:
    // If notify() was previously called, or is called within |duration|, return true; else false.
    template <typename R, typename P>
    bool wait(std::chrono::duration<R, P> duration) {
        std::unique_lock<std::mutex> lock(mMutex);
        return mCv.wait_for(lock, duration, [this] { return mValue; });
    }
    void notify() {
        std::unique_lock<std::mutex> lock(mMutex);
        mValue = true;
        lock.unlock();
        mCv.notify_all();
    }

private:
    std::mutex mMutex;
    std::condition_variable mCv;
    bool mValue = false;
};

TEST_P(BinderRpcServerOnly, Shutdown) {
    auto addr = allocateSocketAddress();
    auto server = RpcServer::make(newFactory(std::get<0>(GetParam())));
    server->setProtocolVersion(std::get<1>(GetParam()));
    ASSERT_EQ(OK, server->setupUnixDomainServer(addr.c_str()));
    auto joinEnds = std::make_shared<OneOffSignal>();

    // If things are broken and the thread never stops, don't block other tests. Because the thread
    // may run after the test finishes, it must not access the stack memory of the test. Hence,
    // shared pointers are passed.
    std::thread([server, joinEnds] {
        server->join();
        joinEnds->notify();
    }).detach();

    bool shutdown = false;
    for (int i = 0; i < 10 && !shutdown; i++) {
        usleep(300 * 1000); // 300ms; total 3s
        if (server->shutdown()) shutdown = true;
    }
    ASSERT_TRUE(shutdown) << "server->shutdown() never returns true";

    ASSERT_TRUE(joinEnds->wait(2s))
            << "After server->shutdown() returns true, join() did not stop after 2s";
}

TEST(BinderRpc, Java) {
#if !defined(__ANDROID__)
    GTEST_SKIP() << "This test is only run on Android. Though it can technically run on host on"
                    "createRpcDelegateServiceManager() with a device attached, such test belongs "
                    "to binderHostDeviceTest. Hence, just disable this test on host.";
#endif // !__ANDROID__
    sp<IServiceManager> sm = defaultServiceManager();
    ASSERT_NE(nullptr, sm);
    // Any Java service with non-empty getInterfaceDescriptor() would do.
    // Let's pick batteryproperties.
    auto binder = sm->checkService(String16("batteryproperties"));
    ASSERT_NE(nullptr, binder);
    auto descriptor = binder->getInterfaceDescriptor();
    ASSERT_GE(descriptor.size(), 0);
    ASSERT_EQ(OK, binder->pingBinder());

    auto rpcServer = RpcServer::make();
    unsigned int port;
    ASSERT_EQ(OK, rpcServer->setupInetServer(kLocalInetAddress, 0, &port));
    auto socket = rpcServer->releaseServer();

    auto keepAlive = sp<BBinder>::make();
    auto setRpcClientDebugStatus = binder->setRpcClientDebug(std::move(socket), keepAlive);

    if (!android::base::GetBoolProperty("ro.debuggable", false) ||
        android::base::GetProperty("ro.build.type", "") == "user") {
        ASSERT_EQ(INVALID_OPERATION, setRpcClientDebugStatus)
                << "setRpcClientDebug should return INVALID_OPERATION on non-debuggable or user "
                   "builds, but get "
                << statusToString(setRpcClientDebugStatus);
        GTEST_SKIP();
    }

    ASSERT_EQ(OK, setRpcClientDebugStatus);

    auto rpcSession = RpcSession::make();
    ASSERT_EQ(OK, rpcSession->setupInetClient("127.0.0.1", port));
    auto rpcBinder = rpcSession->getRootObject();
    ASSERT_NE(nullptr, rpcBinder);

    ASSERT_EQ(OK, rpcBinder->pingBinder());

    ASSERT_EQ(descriptor, rpcBinder->getInterfaceDescriptor())
            << "getInterfaceDescriptor should not crash system_server";
    ASSERT_EQ(OK, rpcBinder->pingBinder());
}

INSTANTIATE_TEST_CASE_P(BinderRpc, BinderRpcServerOnly,
                        ::testing::Combine(::testing::ValuesIn(RpcSecurityValues()),
                                           ::testing::ValuesIn(testVersions())),
                        BinderRpcServerOnly::PrintTestParam);

class RpcTransportTestUtils {
public:
    // Only parameterized only server version because `RpcSession` is bypassed
    // in the client half of the tests.
    using Param =
            std::tuple<SocketType, RpcSecurity, std::optional<RpcCertificateFormat>, uint32_t>;
    using ConnectToServer = std::function<base::unique_fd()>;

    // A server that handles client socket connections.
    class Server {
    public:
        explicit Server() {}
        Server(Server&&) = default;
        ~Server() { shutdownAndWait(); }
        [[nodiscard]] AssertionResult setUp(
                const Param& param,
                std::unique_ptr<RpcAuth> auth = std::make_unique<RpcAuthSelfSigned>()) {
            auto [socketType, rpcSecurity, certificateFormat, serverVersion] = param;
            auto rpcServer = RpcServer::make(newFactory(rpcSecurity));
            rpcServer->setProtocolVersion(serverVersion);
            switch (socketType) {
                case SocketType::PRECONNECTED: {
                    return AssertionFailure() << "Not supported by this test";
                } break;
                case SocketType::UNIX: {
                    auto addr = allocateSocketAddress();
                    auto status = rpcServer->setupUnixDomainServer(addr.c_str());
                    if (status != OK) {
                        return AssertionFailure()
                                << "setupUnixDomainServer: " << statusToString(status);
                    }
                    mConnectToServer = [addr] {
                        return connectTo(UnixSocketAddress(addr.c_str()));
                    };
                } break;
                case SocketType::VSOCK: {
                    auto port = allocateVsockPort();
                    auto status = rpcServer->setupVsockServer(port);
                    if (status != OK) {
                        return AssertionFailure() << "setupVsockServer: " << statusToString(status);
                    }
                    mConnectToServer = [port] {
                        return connectTo(VsockSocketAddress(VMADDR_CID_LOCAL, port));
                    };
                } break;
                case SocketType::INET: {
                    unsigned int port;
                    auto status = rpcServer->setupInetServer(kLocalInetAddress, 0, &port);
                    if (status != OK) {
                        return AssertionFailure() << "setupInetServer: " << statusToString(status);
                    }
                    mConnectToServer = [port] {
                        const char* addr = kLocalInetAddress;
                        auto aiStart = InetSocketAddress::getAddrInfo(addr, port);
                        if (aiStart == nullptr) return base::unique_fd{};
                        for (auto ai = aiStart.get(); ai != nullptr; ai = ai->ai_next) {
                            auto fd = connectTo(
                                    InetSocketAddress(ai->ai_addr, ai->ai_addrlen, addr, port));
                            if (fd.ok()) return fd;
                        }
                        ALOGE("None of the socket address resolved for %s:%u can be connected",
                              addr, port);
                        return base::unique_fd{};
                    };
                }
            }
            mFd = rpcServer->releaseServer();
            if (!mFd.ok()) return AssertionFailure() << "releaseServer returns invalid fd";
            mCtx = newFactory(rpcSecurity, mCertVerifier, std::move(auth))->newServerCtx();
            if (mCtx == nullptr) return AssertionFailure() << "newServerCtx";
            mSetup = true;
            return AssertionSuccess();
        }
        RpcTransportCtx* getCtx() const { return mCtx.get(); }
        std::shared_ptr<RpcCertificateVerifierSimple> getCertVerifier() const {
            return mCertVerifier;
        }
        ConnectToServer getConnectToServerFn() { return mConnectToServer; }
        void start() {
            LOG_ALWAYS_FATAL_IF(!mSetup, "Call Server::setup first!");
            mThread = std::make_unique<std::thread>(&Server::run, this);
        }
        void run() {
            LOG_ALWAYS_FATAL_IF(!mSetup, "Call Server::setup first!");

            std::vector<std::thread> threads;
            while (OK == mFdTrigger->triggerablePoll(mFd, POLLIN)) {
                base::unique_fd acceptedFd(
                        TEMP_FAILURE_RETRY(accept4(mFd.get(), nullptr, nullptr /*length*/,
                                                   SOCK_CLOEXEC | SOCK_NONBLOCK)));
                threads.emplace_back(&Server::handleOne, this, std::move(acceptedFd));
            }

            for (auto& thread : threads) thread.join();
        }
        void handleOne(android::base::unique_fd acceptedFd) {
            ASSERT_TRUE(acceptedFd.ok());
            auto serverTransport = mCtx->newTransport(std::move(acceptedFd), mFdTrigger.get());
            if (serverTransport == nullptr) return; // handshake failed
            ASSERT_TRUE(mPostConnect(serverTransport.get(), mFdTrigger.get()));
        }
        void shutdownAndWait() {
            shutdown();
            join();
        }
        void shutdown() { mFdTrigger->trigger(); }

        void setPostConnect(
                std::function<AssertionResult(RpcTransport*, FdTrigger* fdTrigger)> fn) {
            mPostConnect = std::move(fn);
        }

    private:
        std::unique_ptr<std::thread> mThread;
        ConnectToServer mConnectToServer;
        std::unique_ptr<FdTrigger> mFdTrigger = FdTrigger::make();
        base::unique_fd mFd;
        std::unique_ptr<RpcTransportCtx> mCtx;
        std::shared_ptr<RpcCertificateVerifierSimple> mCertVerifier =
                std::make_shared<RpcCertificateVerifierSimple>();
        bool mSetup = false;
        // The function invoked after connection and handshake. By default, it is
        // |defaultPostConnect| that sends |kMessage| to the client.
        std::function<AssertionResult(RpcTransport*, FdTrigger* fdTrigger)> mPostConnect =
                Server::defaultPostConnect;

        void join() {
            if (mThread != nullptr) {
                mThread->join();
                mThread = nullptr;
            }
        }

        static AssertionResult defaultPostConnect(RpcTransport* serverTransport,
                                                  FdTrigger* fdTrigger) {
            std::string message(kMessage);
            iovec messageIov{message.data(), message.size()};
            auto status = serverTransport->interruptableWriteFully(fdTrigger, &messageIov, 1,
                                                                   std::nullopt, nullptr);
            if (status != OK) return AssertionFailure() << statusToString(status);
            return AssertionSuccess();
        }
    };

    class Client {
    public:
        explicit Client(ConnectToServer connectToServer) : mConnectToServer(connectToServer) {}
        Client(Client&&) = default;
        [[nodiscard]] AssertionResult setUp(const Param& param) {
            auto [socketType, rpcSecurity, certificateFormat, serverVersion] = param;
            (void)serverVersion;
            mFdTrigger = FdTrigger::make();
            mCtx = newFactory(rpcSecurity, mCertVerifier)->newClientCtx();
            if (mCtx == nullptr) return AssertionFailure() << "newClientCtx";
            return AssertionSuccess();
        }
        RpcTransportCtx* getCtx() const { return mCtx.get(); }
        std::shared_ptr<RpcCertificateVerifierSimple> getCertVerifier() const {
            return mCertVerifier;
        }
        // connect() and do handshake
        bool setUpTransport() {
            mFd = mConnectToServer();
            if (!mFd.ok()) return AssertionFailure() << "Cannot connect to server";
            mClientTransport = mCtx->newTransport(std::move(mFd), mFdTrigger.get());
            return mClientTransport != nullptr;
        }
        AssertionResult readMessage(const std::string& expectedMessage = kMessage) {
            LOG_ALWAYS_FATAL_IF(mClientTransport == nullptr, "setUpTransport not called or failed");
            std::string readMessage(expectedMessage.size(), '\0');
            iovec readMessageIov{readMessage.data(), readMessage.size()};
            status_t readStatus =
                    mClientTransport->interruptableReadFully(mFdTrigger.get(), &readMessageIov, 1,
                                                             std::nullopt, nullptr);
            if (readStatus != OK) {
                return AssertionFailure() << statusToString(readStatus);
            }
            if (readMessage != expectedMessage) {
                return AssertionFailure()
                        << "Expected " << expectedMessage << ", actual " << readMessage;
            }
            return AssertionSuccess();
        }
        void run(bool handshakeOk = true, bool readOk = true) {
            if (!setUpTransport()) {
                ASSERT_FALSE(handshakeOk) << "newTransport returns nullptr, but it shouldn't";
                return;
            }
            ASSERT_TRUE(handshakeOk) << "newTransport does not return nullptr, but it should";
            ASSERT_EQ(readOk, readMessage());
        }

    private:
        ConnectToServer mConnectToServer;
        base::unique_fd mFd;
        std::unique_ptr<FdTrigger> mFdTrigger = FdTrigger::make();
        std::unique_ptr<RpcTransportCtx> mCtx;
        std::shared_ptr<RpcCertificateVerifierSimple> mCertVerifier =
                std::make_shared<RpcCertificateVerifierSimple>();
        std::unique_ptr<RpcTransport> mClientTransport;
    };

    // Make A trust B.
    template <typename A, typename B>
    static status_t trust(RpcSecurity rpcSecurity,
                          std::optional<RpcCertificateFormat> certificateFormat, const A& a,
                          const B& b) {
        if (rpcSecurity != RpcSecurity::TLS) return OK;
        LOG_ALWAYS_FATAL_IF(!certificateFormat.has_value());
        auto bCert = b->getCtx()->getCertificate(*certificateFormat);
        return a->getCertVerifier()->addTrustedPeerCertificate(*certificateFormat, bCert);
    }

    static constexpr const char* kMessage = "hello";
};

class RpcTransportTest : public testing::TestWithParam<RpcTransportTestUtils::Param> {
public:
    using Server = RpcTransportTestUtils::Server;
    using Client = RpcTransportTestUtils::Client;
    static inline std::string PrintParamInfo(const testing::TestParamInfo<ParamType>& info) {
        auto [socketType, rpcSecurity, certificateFormat, serverVersion] = info.param;
        auto ret = PrintToString(socketType) + "_" + newFactory(rpcSecurity)->toCString();
        if (certificateFormat.has_value()) ret += "_" + PrintToString(*certificateFormat);
        ret += "_serverV" + std::to_string(serverVersion);
        return ret;
    }
    static std::vector<ParamType> getRpcTranportTestParams() {
        std::vector<ParamType> ret;
        for (auto serverVersion : testVersions()) {
            for (auto socketType : testSocketTypes(false /* hasPreconnected */)) {
                for (auto rpcSecurity : RpcSecurityValues()) {
                    switch (rpcSecurity) {
                        case RpcSecurity::RAW: {
                            ret.emplace_back(socketType, rpcSecurity, std::nullopt, serverVersion);
                        } break;
                        case RpcSecurity::TLS: {
                            ret.emplace_back(socketType, rpcSecurity, RpcCertificateFormat::PEM,
                                             serverVersion);
                            ret.emplace_back(socketType, rpcSecurity, RpcCertificateFormat::DER,
                                             serverVersion);
                        } break;
                    }
                }
            }
        }
        return ret;
    }
    template <typename A, typename B>
    status_t trust(const A& a, const B& b) {
        auto [socketType, rpcSecurity, certificateFormat, serverVersion] = GetParam();
        (void)serverVersion;
        return RpcTransportTestUtils::trust(rpcSecurity, certificateFormat, a, b);
    }
};

TEST_P(RpcTransportTest, GoodCertificate) {
    auto server = std::make_unique<Server>();
    ASSERT_TRUE(server->setUp(GetParam()));

    Client client(server->getConnectToServerFn());
    ASSERT_TRUE(client.setUp(GetParam()));

    ASSERT_EQ(OK, trust(&client, server));
    ASSERT_EQ(OK, trust(server, &client));

    server->start();
    client.run();
}

TEST_P(RpcTransportTest, MultipleClients) {
    auto server = std::make_unique<Server>();
    ASSERT_TRUE(server->setUp(GetParam()));

    std::vector<Client> clients;
    for (int i = 0; i < 2; i++) {
        auto& client = clients.emplace_back(server->getConnectToServerFn());
        ASSERT_TRUE(client.setUp(GetParam()));
        ASSERT_EQ(OK, trust(&client, server));
        ASSERT_EQ(OK, trust(server, &client));
    }

    server->start();
    for (auto& client : clients) client.run();
}

TEST_P(RpcTransportTest, UntrustedServer) {
    auto [socketType, rpcSecurity, certificateFormat, serverVersion] = GetParam();
    (void)serverVersion;

    auto untrustedServer = std::make_unique<Server>();
    ASSERT_TRUE(untrustedServer->setUp(GetParam()));

    Client client(untrustedServer->getConnectToServerFn());
    ASSERT_TRUE(client.setUp(GetParam()));

    ASSERT_EQ(OK, trust(untrustedServer, &client));

    untrustedServer->start();

    // For TLS, this should reject the certificate. For RAW sockets, it should pass because
    // the client can't verify the server's identity.
    bool handshakeOk = rpcSecurity != RpcSecurity::TLS;
    client.run(handshakeOk);
}
TEST_P(RpcTransportTest, MaliciousServer) {
    auto [socketType, rpcSecurity, certificateFormat, serverVersion] = GetParam();
    (void)serverVersion;

    auto validServer = std::make_unique<Server>();
    ASSERT_TRUE(validServer->setUp(GetParam()));

    auto maliciousServer = std::make_unique<Server>();
    ASSERT_TRUE(maliciousServer->setUp(GetParam()));

    Client client(maliciousServer->getConnectToServerFn());
    ASSERT_TRUE(client.setUp(GetParam()));

    ASSERT_EQ(OK, trust(&client, validServer));
    ASSERT_EQ(OK, trust(validServer, &client));
    ASSERT_EQ(OK, trust(maliciousServer, &client));

    maliciousServer->start();

    // For TLS, this should reject the certificate. For RAW sockets, it should pass because
    // the client can't verify the server's identity.
    bool handshakeOk = rpcSecurity != RpcSecurity::TLS;
    client.run(handshakeOk);
}

TEST_P(RpcTransportTest, UntrustedClient) {
    auto [socketType, rpcSecurity, certificateFormat, serverVersion] = GetParam();
    (void)serverVersion;

    auto server = std::make_unique<Server>();
    ASSERT_TRUE(server->setUp(GetParam()));

    Client client(server->getConnectToServerFn());
    ASSERT_TRUE(client.setUp(GetParam()));

    ASSERT_EQ(OK, trust(&client, server));

    server->start();

    // For TLS, Client should be able to verify server's identity, so client should see
    // do_handshake() successfully executed. However, server shouldn't be able to verify client's
    // identity and should drop the connection, so client shouldn't be able to read anything.
    bool readOk = rpcSecurity != RpcSecurity::TLS;
    client.run(true, readOk);
}

TEST_P(RpcTransportTest, MaliciousClient) {
    auto [socketType, rpcSecurity, certificateFormat, serverVersion] = GetParam();
    (void)serverVersion;

    auto server = std::make_unique<Server>();
    ASSERT_TRUE(server->setUp(GetParam()));

    Client validClient(server->getConnectToServerFn());
    ASSERT_TRUE(validClient.setUp(GetParam()));
    Client maliciousClient(server->getConnectToServerFn());
    ASSERT_TRUE(maliciousClient.setUp(GetParam()));

    ASSERT_EQ(OK, trust(&validClient, server));
    ASSERT_EQ(OK, trust(&maliciousClient, server));

    server->start();

    // See UntrustedClient.
    bool readOk = rpcSecurity != RpcSecurity::TLS;
    maliciousClient.run(true, readOk);
}

TEST_P(RpcTransportTest, Trigger) {
    std::string msg2 = ", world!";
    std::mutex writeMutex;
    std::condition_variable writeCv;
    bool shouldContinueWriting = false;
    auto serverPostConnect = [&](RpcTransport* serverTransport, FdTrigger* fdTrigger) {
        std::string message(RpcTransportTestUtils::kMessage);
        iovec messageIov{message.data(), message.size()};
        auto status = serverTransport->interruptableWriteFully(fdTrigger, &messageIov, 1,
                                                               std::nullopt, nullptr);
        if (status != OK) return AssertionFailure() << statusToString(status);

        {
            std::unique_lock<std::mutex> lock(writeMutex);
            if (!writeCv.wait_for(lock, 3s, [&] { return shouldContinueWriting; })) {
                return AssertionFailure() << "write barrier not cleared in time!";
            }
        }

        iovec msg2Iov{msg2.data(), msg2.size()};
        status = serverTransport->interruptableWriteFully(fdTrigger, &msg2Iov, 1, std::nullopt,
                                                          nullptr);
        if (status != DEAD_OBJECT)
            return AssertionFailure() << "When FdTrigger is shut down, interruptableWriteFully "
                                         "should return DEAD_OBJECT, but it is "
                                      << statusToString(status);
        return AssertionSuccess();
    };

    auto server = std::make_unique<Server>();
    ASSERT_TRUE(server->setUp(GetParam()));

    // Set up client
    Client client(server->getConnectToServerFn());
    ASSERT_TRUE(client.setUp(GetParam()));

    // Exchange keys
    ASSERT_EQ(OK, trust(&client, server));
    ASSERT_EQ(OK, trust(server, &client));

    server->setPostConnect(serverPostConnect);

    server->start();
    // connect() to server and do handshake
    ASSERT_TRUE(client.setUpTransport());
    // read the first message. This ensures that server has finished handshake and start handling
    // client fd. Server thread should pause at writeCv.wait_for().
    ASSERT_TRUE(client.readMessage(RpcTransportTestUtils::kMessage));
    // Trigger server shutdown after server starts handling client FD. This ensures that the second
    // write is on an FdTrigger that has been shut down.
    server->shutdown();
    // Continues server thread to write the second message.
    {
        std::lock_guard<std::mutex> lock(writeMutex);
        shouldContinueWriting = true;
    }
    writeCv.notify_all();
    // After this line, server thread unblocks and attempts to write the second message, but
    // shutdown is triggered, so write should failed with DEAD_OBJECT. See |serverPostConnect|.
    // On the client side, second read fails with DEAD_OBJECT
    ASSERT_FALSE(client.readMessage(msg2));
}

INSTANTIATE_TEST_CASE_P(BinderRpc, RpcTransportTest,
                        ::testing::ValuesIn(RpcTransportTest::getRpcTranportTestParams()),
                        RpcTransportTest::PrintParamInfo);

class RpcTransportTlsKeyTest
      : public testing::TestWithParam<
                std::tuple<SocketType, RpcCertificateFormat, RpcKeyFormat, uint32_t>> {
public:
    template <typename A, typename B>
    status_t trust(const A& a, const B& b) {
        auto [socketType, certificateFormat, keyFormat, serverVersion] = GetParam();
        (void)serverVersion;
        return RpcTransportTestUtils::trust(RpcSecurity::TLS, certificateFormat, a, b);
    }
    static std::string PrintParamInfo(const testing::TestParamInfo<ParamType>& info) {
        auto [socketType, certificateFormat, keyFormat, serverVersion] = info.param;
        return PrintToString(socketType) + "_certificate_" + PrintToString(certificateFormat) +
                "_key_" + PrintToString(keyFormat) + "_serverV" + std::to_string(serverVersion);
    };
};

TEST_P(RpcTransportTlsKeyTest, PreSignedCertificate) {
    auto [socketType, certificateFormat, keyFormat, serverVersion] = GetParam();

    std::vector<uint8_t> pkeyData, certData;
    {
        auto pkey = makeKeyPairForSelfSignedCert();
        ASSERT_NE(nullptr, pkey);
        auto cert = makeSelfSignedCert(pkey.get(), kCertValidSeconds);
        ASSERT_NE(nullptr, cert);
        pkeyData = serializeUnencryptedPrivatekey(pkey.get(), keyFormat);
        certData = serializeCertificate(cert.get(), certificateFormat);
    }

    auto desPkey = deserializeUnencryptedPrivatekey(pkeyData, keyFormat);
    auto desCert = deserializeCertificate(certData, certificateFormat);
    auto auth = std::make_unique<RpcAuthPreSigned>(std::move(desPkey), std::move(desCert));
    auto utilsParam = std::make_tuple(socketType, RpcSecurity::TLS,
                                      std::make_optional(certificateFormat), serverVersion);

    auto server = std::make_unique<RpcTransportTestUtils::Server>();
    ASSERT_TRUE(server->setUp(utilsParam, std::move(auth)));

    RpcTransportTestUtils::Client client(server->getConnectToServerFn());
    ASSERT_TRUE(client.setUp(utilsParam));

    ASSERT_EQ(OK, trust(&client, server));
    ASSERT_EQ(OK, trust(server, &client));

    server->start();
    client.run();
}

INSTANTIATE_TEST_CASE_P(
        BinderRpc, RpcTransportTlsKeyTest,
        testing::Combine(testing::ValuesIn(testSocketTypes(false /* hasPreconnected*/)),
                         testing::Values(RpcCertificateFormat::PEM, RpcCertificateFormat::DER),
                         testing::Values(RpcKeyFormat::PEM, RpcKeyFormat::DER),
                         testing::ValuesIn(testVersions())),
        RpcTransportTlsKeyTest::PrintParamInfo);

} // namespace android

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    android::base::InitLogging(argv, android::base::StderrLogger, android::base::DefaultAborter);

    return RUN_ALL_TESTS();
}
