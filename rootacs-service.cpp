#include <Windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rootacs {

inline constexpr wchar_t kServiceName[] = L"RootACS-Service";
#ifdef ROOTACS_DEBUG_BUILD
inline constexpr wchar_t kPipeName[] = LR"(\\.\pipe\RootAccessPipe-DebugLocal)";
#else
inline constexpr wchar_t kPipeName[] = LR"(\\.\pipe\RootAccessPipe)";
#endif
inline constexpr std::uint32_t kProtocolMagic = 0x53434152;
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxSessionIdLength = 64;
inline constexpr std::uint32_t kMaxPayloadBytes = 64 * 1024;

enum class MessageType : std::uint16_t {
    AuthInit = 1,
    AuthMetadata = 2,
    InitShell = 3,
    SessionStarted = 4,
    StdinData = 5,
    StdoutData = 6,
    ExitSession = 7,
    ExitService = 8,
    Status = 9,
    Error = 10,
};

#pragma pack(push, 1)
struct MessageHeader {
    std::uint32_t magic{kProtocolMagic};
    std::uint16_t version{kProtocolVersion};
    MessageType type{MessageType::Status};
    std::uint32_t payloadSize{0};
    char sessionId[kMaxSessionIdLength]{};
};
#pragma pack(pop)

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedHandle() {
        reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    HANDLE get() const noexcept {
        return handle_;
    }

    HANDLE release() noexcept {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{nullptr};
};

bool IsValidHeader(const MessageHeader& header) {
    return header.magic == kProtocolMagic &&
           header.version == kProtocolVersion &&
           header.payloadSize <= kMaxPayloadBytes;
}

std::string ExtractSessionId(const MessageHeader& header) {
    return std::string(header.sessionId, strnlen_s(header.sessionId, kMaxSessionIdLength));
}

MessageHeader MakeHeader(
    MessageType type,
    const std::string& sessionId,
    std::uint32_t payloadSize) {
    MessageHeader header{};
    header.type = type;
    header.payloadSize = payloadSize;

    const auto copyLength = std::min(sessionId.size(), kMaxSessionIdLength - 1);
    if (copyLength != 0U) {
        std::memcpy(header.sessionId, sessionId.data(), copyLength);
    }
    return header;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int requiredChars = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (requiredChars <= 0) {
        return {};
    }

    std::wstring converted(requiredChars, L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            converted.data(),
            requiredChars) <= 0) {
        return {};
    }

    return converted;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int requiredChars = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (requiredChars <= 0) {
        return {};
    }

    std::string converted(requiredChars, '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            converted.data(),
            requiredChars,
            nullptr,
            nullptr) <= 0) {
        return {};
    }

    return converted;
}

std::string NarrowFromBuffer(const std::vector<std::uint8_t>& buffer) {
    if (buffer.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(buffer.data()), buffer.size());
}

std::wstring GetLastErrorMessage(DWORD error) {
    wchar_t* messageBuffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;

    const DWORD chars = FormatMessageW(
        flags,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&messageBuffer),
        0,
        nullptr);

    std::wstring message;
    if (chars != 0 && messageBuffer != nullptr) {
        message.assign(messageBuffer, chars);
        LocalFree(messageBuffer);
    }

    return message;
}

std::wstring DescribeWin32Error(DWORD error) {
    std::wstring message = GetLastErrorMessage(error);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ' || message.back() == L'\t')) {
        message.pop_back();
    }

    std::wstring description = L"error=" + std::to_wstring(error);
    if (!message.empty()) {
        description += L": ";
        description += message;
    }
    return description;
}

bool ReadExact(HANDLE handle, void* buffer, std::size_t bytesToRead) {
    auto* current = static_cast<std::uint8_t*>(buffer);
    std::size_t remaining = bytesToRead;

    while (remaining > 0U) {
        DWORD chunkRead = 0;
        if (!ReadFile(handle, current, static_cast<DWORD>(remaining), &chunkRead, nullptr)) {
            return false;
        }
        if (chunkRead == 0U) {
            return false;
        }
        current += chunkRead;
        remaining -= chunkRead;
    }

    return true;
}

bool WriteExact(HANDLE handle, const void* buffer, std::size_t bytesToWrite) {
    const auto* current = static_cast<const std::uint8_t*>(buffer);
    std::size_t remaining = bytesToWrite;

    while (remaining > 0U) {
        DWORD chunkWritten = 0;
        if (!WriteFile(handle, current, static_cast<DWORD>(remaining), &chunkWritten, nullptr)) {
            return false;
        }
        if (chunkWritten == 0U) {
            return false;
        }
        current += chunkWritten;
        remaining -= chunkWritten;
    }

    return true;
}

bool ReadFrame(HANDLE pipe, MessageHeader* header, std::vector<std::uint8_t>* payload) {
    if (!ReadExact(pipe, header, sizeof(*header))) {
        return false;
    }

    if (!IsValidHeader(*header)) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }

    payload->assign(header->payloadSize, 0);
    if (header->payloadSize == 0U) {
        return true;
    }

    return ReadExact(pipe, payload->data(), payload->size());
}

namespace {

#ifndef ROOTACS_DEBUG_BUILD
constexpr wchar_t kPipeSecuritySddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
#endif
constexpr DWORD kPipeBufferBytes = 64 * 1024;
constexpr DWORD kPeekSleepMs = 20;
constexpr DWORD kMinFrameHeaderBytes = sizeof(MessageHeader);
constexpr DWORD kSessionOutputStartDelayMs = 100;
#ifdef ROOTACS_DEBUG_BUILD
constexpr char kServiceLogPath[] = "service-debug.log";
#else
constexpr char kServiceLogPath[] = "service.log";
#endif

void AppendServiceLog(const std::string& message) {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    std::ofstream stream(kServiceLogPath, std::ios::app);
    if (!stream.is_open()) {
        return;
    }

    stream << std::setfill('0')
           << std::setw(4) << st.wYear << '-'
           << std::setw(2) << st.wMonth << '-'
           << std::setw(2) << st.wDay << ' '
           << std::setw(2) << st.wHour << ':'
           << std::setw(2) << st.wMinute << ':'
           << std::setw(2) << st.wSecond << '.'
           << std::setw(3) << st.wMilliseconds
           << " | " << message << '\n';
}

} // namespace

struct Session {
    std::string sessionId;
    std::wstring shellPath;
    std::mutex ioMutex;
    ScopedHandle processHandle;
    ScopedHandle threadHandle;
    ScopedHandle stdinWrite;
    ScopedHandle stdoutRead;
    std::thread outputThread;
    std::atomic_bool active{false};
};

class PipeSecurityDescriptor {
public:
    ~PipeSecurityDescriptor() {
        if (securityDescriptor_ != nullptr) {
            LocalFree(securityDescriptor_);
            securityDescriptor_ = nullptr;
        }
    }

    bool Initialize() {
        securityAttributes_ = {};
        securityAttributes_.nLength = sizeof(securityAttributes_);

#ifdef ROOTACS_DEBUG_BUILD
        securityDescriptor_ = static_cast<PSECURITY_DESCRIPTOR>(
            LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
        if (securityDescriptor_ == nullptr) {
            return false;
        }

        if (!InitializeSecurityDescriptor(securityDescriptor_, SECURITY_DESCRIPTOR_REVISION)) {
            LocalFree(securityDescriptor_);
            securityDescriptor_ = nullptr;
            return false;
        }

        if (!SetSecurityDescriptorDacl(securityDescriptor_, TRUE, nullptr, FALSE)) {
            LocalFree(securityDescriptor_);
            securityDescriptor_ = nullptr;
            return false;
        }
#else
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kPipeSecuritySddl,
                SDDL_REVISION_1,
                &securityDescriptor_,
                nullptr)) {
            return false;
        }
#endif

        securityAttributes_.lpSecurityDescriptor = securityDescriptor_;
        securityAttributes_.bInheritHandle = FALSE;
        return true;
    }

    SECURITY_ATTRIBUTES* attributes() noexcept {
        return &securityAttributes_;
    }

private:
    PSECURITY_DESCRIPTOR securityDescriptor_{nullptr};
    SECURITY_ATTRIBUTES securityAttributes_{};
};

class RootAcsService {
public:
    RootAcsService() {
        std::memset(&status_, 0, sizeof(status_));
        status_.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        status_.dwCurrentState = SERVICE_STOPPED;
    }

    int Execute() {
        RootAcsService* previousInstance = instance_;
        if (instance_ == nullptr) {
            instance_ = this;
        }

        if (!Initialize()) {
            const DWORD error = GetLastError();
            AppendServiceLog("Initialize failed: " + std::to_string(error));
            ReportServiceStatus(SERVICE_STOPPED, error, 0);
            if (previousInstance == nullptr) {
                instance_ = nullptr;
            }
            return static_cast<int>(error);
        }

        Run();
        AppendServiceLog("Service Execute finished");

        if (previousInstance == nullptr) {
            instance_ = nullptr;
        }

        return 0;
    }

    void RequestStop() {
        const bool alreadyStopping = stopping_.exchange(true);
        if (alreadyStopping) {
            return;
        }

        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);

        if (listenerThread_.joinable()) {
            CancelSynchronousIo(listenerThread_.native_handle());
        }

        if (stopEvent_) {
            SetEvent(stopEvent_.get());
        }

        DisconnectAllClients();
    }

    static BOOL WINAPI ConsoleCtrlHandler(DWORD controlCode) {
        switch (controlCode) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (instance_ != nullptr) {
                instance_->RequestStop();
                return TRUE;
            }
            return FALSE;
        default:
            return FALSE;
        }
    }

    static void WINAPI ServiceMain(DWORD, LPWSTR*) {
        RootAcsService service;
        instance_ = &service;

        service.statusHandle_ =
            RegisterServiceCtrlHandlerW(kServiceName, &RootAcsService::ServiceCtrlHandler);
        if (service.statusHandle_ == nullptr) {
            instance_ = nullptr;
            return;
        }

        service.ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
        service.Execute();
        instance_ = nullptr;
    }

    static void WINAPI ServiceCtrlHandler(DWORD controlCode) {
        if (instance_ == nullptr) {
            return;
        }

        switch (controlCode) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            instance_->RequestStop();
            break;
        default:
            break;
        }
    }

private:
    struct ClientConnection {
        explicit ClientConnection(ScopedHandle pipeHandle) : pipe(std::move(pipeHandle)) {}

        ScopedHandle pipe;
        std::mutex writeMutex;
        std::mutex sessionsMutex;
        std::vector<std::string> sessionIds;
        std::atomic_bool connected{true};
    };

    bool Initialize() {
        AppendServiceLog("Initialize start");
        stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stopEvent_) {
            return false;
        }

        if (!pipeSecurity_.Initialize()) {
            AppendServiceLog("Pipe security initialization failed");
            return false;
        }

        AppendServiceLog("Initialize complete");
        return true;
    }

    void Run() {
        ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);

        listenerThread_ = std::thread([this]() { ListenerLoop(); });
        WaitForSingleObject(stopEvent_.get(), INFINITE);

        if (listenerThread_.joinable()) {
            listenerThread_.join();
        }

        StopAllSessions();

        std::vector<std::thread> threadsToJoin;
        {
            std::lock_guard<std::mutex> lock(clientThreadsMutex_);
            threadsToJoin.swap(clientThreads_);
        }

        for (auto& thread : threadsToJoin) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
    }

    void ListenerLoop() {
        AppendServiceLog("ListenerLoop start");
        while (!stopping_) {
            ScopedHandle pipe = CreateServerPipe();
            if (!pipe) {
                const DWORD error = GetLastError();
                AppendServiceLog("CreateServerPipe failed error=" + std::to_string(error));
                RequestStop();
                return;
            }

            const BOOL connected = ConnectNamedPipe(pipe.get(), nullptr) ? TRUE : FALSE;
            if (!connected) {
                const DWORD error = GetLastError();
                if (error != ERROR_PIPE_CONNECTED) {
                    if (error == ERROR_OPERATION_ABORTED || stopping_) {
                        AppendServiceLog("ConnectNamedPipe aborted");
                        return;
                    }
                    AppendServiceLog("ConnectNamedPipe failed: " + std::to_string(error));
                    continue;
                }
            }

            auto client = std::make_shared<ClientConnection>(std::move(pipe));
            AppendServiceLog("Client connected");
            {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                clients_.push_back(client);
            }
            std::thread clientThread([this, client]() { HandleClient(client); });

            std::lock_guard<std::mutex> lock(clientThreadsMutex_);
            clientThreads_.push_back(std::move(clientThread));
        }
    }

    void HandleClient(const std::shared_ptr<ClientConnection>& client) {
        MessageHeader header{};
        std::vector<std::uint8_t> payload;

        while (!stopping_) {
            DWORD availableBytes = 0;
            if (!PeekNamedPipe(client->pipe.get(), nullptr, 0, nullptr, &availableBytes, nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED) {
                    AppendServiceLog("HandleClient peek failed error=" + std::to_string(error));
                }
                break;
            }

            if (availableBytes < kMinFrameHeaderBytes) {
                Sleep(kPeekSleepMs);
                continue;
            }

            if (!ReadFrame(client->pipe.get(), &header, &payload)) {
                break;
            }

            if (!DispatchClientMessage(client, header, payload)) {
                break;
            }
        }

        client->connected = false;
        CleanupSessionsForClient(client);

        if (client->pipe) {
            FlushFileBuffers(client->pipe.get());
            DisconnectNamedPipe(client->pipe.get());
        }

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.erase(
                std::remove(clients_.begin(), clients_.end(), client),
                clients_.end());
        }
    }

    bool DispatchClientMessage(
        const std::shared_ptr<ClientConnection>& client,
        const MessageHeader& header,
        const std::vector<std::uint8_t>& payload) {
        const std::string sessionId = ExtractSessionId(header);
        AppendServiceLog(
            "Dispatch message type=" + std::to_string(static_cast<int>(header.type)) +
            " session=" + sessionId +
            " payload=" + std::to_string(payload.size()));

        switch (header.type) {
        case MessageType::AuthInit: {
            std::string metadata = "HOSTNAME=" + WideToUtf8(GetComputerNameString()) +
                                   ";SERVICE=" + WideToUtf8(std::wstring(kServiceName));
            return SendTextFrame(client, MessageType::AuthMetadata, {}, metadata);
        }
        case MessageType::InitShell: {
            std::wstring shellPath = Utf8ToWide(NarrowFromBuffer(payload));
            if (shellPath.empty()) {
                shellPath = L"C:\\Windows\\System32\\cmd.exe";
            }

            std::string newSessionId;
            std::shared_ptr<Session> session;
            if (!StartSession(client, shellPath, &newSessionId, &session)) {
                AppendServiceLog("StartSession failed for shell=" + WideToUtf8(shellPath));
                return SendTextFrame(client, MessageType::Error, {}, "INIT_SHELL_FAILED");
            }

            const bool sent = SendTextFrame(
                client,
                MessageType::SessionStarted,
                newSessionId,
                WideToUtf8(shellPath));
            if (sent) {
                StartSessionOutputThread(session, client);
                return true;
            }

            StopSession(newSessionId, true);
            return false;
        }
        case MessageType::StdinData:
            if (sessionId.empty()) {
                return SendTextFrame(client, MessageType::Error, {}, "SESSION_ID_REQUIRED");
            }
            if (!WriteToSession(sessionId, payload.data(), payload.size())) {
                return SendTextFrame(client, MessageType::Error, sessionId, "WRITE_TO_SESSION_FAILED");
            }
            return true;
        case MessageType::ExitSession:
            if (!sessionId.empty()) {
                StopSession(sessionId, true);
                return SendTextFrame(client, MessageType::Status, sessionId, "SESSION_TERMINATED");
            }
            return SendTextFrame(client, MessageType::Error, {}, "SESSION_ID_REQUIRED");
        case MessageType::ExitService:
            RequestStop();
            return false;
        default:
            return SendTextFrame(client, MessageType::Error, sessionId, "UNSUPPORTED_MESSAGE");
        }
    }

    bool StartSession(
        const std::shared_ptr<ClientConnection>& client,
        const std::wstring& shellPath,
        std::string* sessionId,
        std::shared_ptr<Session>* sessionOut) {
        auto session = std::make_shared<Session>();
        session->sessionId = GenerateSessionId();
        session->shellPath = shellPath;
        AppendServiceLog(
            "StartSession shell=" + WideToUtf8(shellPath) + " session=" + session->sessionId);

        {
            std::lock_guard<std::mutex> ioLock(session->ioMutex);
            if (!LaunchSessionProcessLocked(session.get())) {
                return false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            sessions_[session->sessionId] = session;
        }

        {
            std::lock_guard<std::mutex> lock(client->sessionsMutex);
            client->sessionIds.push_back(session->sessionId);
        }

        *sessionId = session->sessionId;
        if (sessionOut != nullptr) {
            *sessionOut = session;
        }
        return true;
    }

    void StartSessionOutputThread(
        const std::shared_ptr<Session>& session,
        const std::shared_ptr<ClientConnection>& client) {
        if (session == nullptr) {
            return;
        }

        std::weak_ptr<ClientConnection> weakClient = client;
        session->outputThread = std::thread([this, session, weakClient]() {
            std::vector<std::uint8_t> buffer(4096);
            Sleep(kSessionOutputStartDelayMs);

            while (!stopping_ && session->active) {
                HANDLE stdoutHandle = nullptr;
                HANDLE processHandle = nullptr;
                {
                    std::lock_guard<std::mutex> ioLock(session->ioMutex);
                    stdoutHandle = session->stdoutRead.get();
                    processHandle = session->processHandle.get();
                }

                if (stdoutHandle == nullptr || processHandle == nullptr) {
                    break;
                }

                DWORD availableBytes = 0;
                if (!PeekNamedPipe(stdoutHandle, nullptr, 0, nullptr, &availableBytes, nullptr)) {
                    const DWORD error = GetLastError();
                    if (error == ERROR_BROKEN_PIPE) {
                        availableBytes = 0;
                    } else {
                        Sleep(kPeekSleepMs);
                        continue;
                    }
                }

                if (availableBytes == 0) {
                    if (WaitForSingleObject(processHandle, 0) == WAIT_OBJECT_0) {
                        bool restarted = false;
                        {
                            std::lock_guard<std::mutex> ioLock(session->ioMutex);
                            if (session->active &&
                                WaitForSingleObject(session->processHandle.get(), 0) == WAIT_OBJECT_0) {
                                session->active = false;
                                restarted = LaunchSessionProcessLocked(session.get());
                            }
                        }

                        if (restarted) {
                            AppendServiceLog("Session restarted: " + session->sessionId);
                            if (auto lockedClient = weakClient.lock()) {
                                SendTextFrame(
                                    lockedClient,
                                    MessageType::Status,
                                    session->sessionId,
                                    "PROCESS_RESTARTED");
                            }
                            continue;
                        }
                        break;
                    }
                    Sleep(kPeekSleepMs);
                    continue;
                }

                DWORD bytesRead = 0;
                const DWORD bytesToRead = (std::min)(availableBytes, static_cast<DWORD>(buffer.size()));
                if (!ReadFile(stdoutHandle, buffer.data(), bytesToRead, &bytesRead, nullptr)) {
                    const DWORD error = GetLastError();
                    if (error == ERROR_BROKEN_PIPE) {
                        continue;
                    }
                    Sleep(kPeekSleepMs);
                    continue;
                }

                if (bytesRead == 0) {
                    continue;
                }

                AppendServiceLog(
                    "Stdout bytes session=" + session->sessionId +
                    " bytes=" + std::to_string(bytesRead));

                if (auto lockedClient = weakClient.lock()) {
                    SendFrame(
                        lockedClient,
                        MessageType::StdoutData,
                        session->sessionId,
                        buffer.data(),
                        bytesRead);
                } else {
                    break;
                }
            }

            session->active = false;

            if (auto lockedClient = weakClient.lock()) {
                SendTextFrame(lockedClient, MessageType::Status, session->sessionId, "PROCESS_EXITED");
            }
        });
    }

    bool LaunchSessionProcessLocked(Session* session) {
        SECURITY_ATTRIBUTES inheritAttributes{};
        inheritAttributes.nLength = sizeof(inheritAttributes);
        inheritAttributes.bInheritHandle = TRUE;

        HANDLE childStdoutRead = nullptr;
        HANDLE childStdoutWrite = nullptr;
        HANDLE childStdinRead = nullptr;
        HANDLE childStdinWrite = nullptr;

        if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &inheritAttributes, 0)) {
            const DWORD error = GetLastError();
            AppendServiceLog("CreatePipe stdout failed error=" + std::to_string(error));
            return false;
        }
        if (!SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0)) {
            const DWORD error = GetLastError();
            CloseHandle(childStdoutRead);
            CloseHandle(childStdoutWrite);
            AppendServiceLog("SetHandleInformation stdout failed error=" + std::to_string(error));
            return false;
        }

        if (!CreatePipe(&childStdinRead, &childStdinWrite, &inheritAttributes, 0)) {
            const DWORD error = GetLastError();
            CloseHandle(childStdoutRead);
            CloseHandle(childStdoutWrite);
            AppendServiceLog("CreatePipe stdin failed error=" + std::to_string(error));
            return false;
        }
        if (!SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0)) {
            const DWORD error = GetLastError();
            CloseHandle(childStdoutRead);
            CloseHandle(childStdoutWrite);
            CloseHandle(childStdinRead);
            CloseHandle(childStdinWrite);
            AppendServiceLog("SetHandleInformation stdin failed error=" + std::to_string(error));
            return false;
        }

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;
        startupInfo.hStdInput = childStdinRead;
        startupInfo.hStdOutput = childStdoutWrite;
        startupInfo.hStdError = childStdoutWrite;

        PROCESS_INFORMATION processInfo{};
        std::wstring commandLine = L"\"";
        commandLine += session->shellPath;
        commandLine += L"\"";

        const BOOL created = CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo);

        CloseHandle(childStdinRead);
        CloseHandle(childStdoutWrite);

        if (!created) {
            const DWORD error = GetLastError();
            CloseHandle(childStdoutRead);
            CloseHandle(childStdinWrite);
            AppendServiceLog(
                "CreateProcess failed for shell=" + WideToUtf8(session->shellPath) +
                " error=" + std::to_string(error));
            return false;
        }

        SetPriorityClass(processInfo.hProcess, ABOVE_NORMAL_PRIORITY_CLASS);

        session->processHandle.reset(processInfo.hProcess);
        session->threadHandle.reset(processInfo.hThread);
        session->stdinWrite.reset(childStdinWrite);
        session->stdoutRead.reset(childStdoutRead);
        session->active = true;
        AppendServiceLog("Process launched session=" + session->sessionId);
        return true;
    }

    bool WriteToSession(
        const std::string& sessionId,
        const std::uint8_t* data,
        std::size_t size) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            const auto it = sessions_.find(sessionId);
            if (it == sessions_.end()) {
                return false;
            }
            session = it->second;
        }

        if (size == 0U) {
            return true;
        }

        std::lock_guard<std::mutex> ioLock(session->ioMutex);

        if (!session->active && !LaunchSessionProcessLocked(session.get())) {
            return false;
        }

        const bool ok = WriteExact(session->stdinWrite.get(), data, size);
        AppendServiceLog(
            "WriteToSession session=" + sessionId +
            " bytes=" + std::to_string(size) +
            " ok=" + (ok ? "1" : "0"));
        return ok;
    }

    void StopSession(const std::string& sessionId, bool forceTerminate) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            const auto it = sessions_.find(sessionId);
            if (it == sessions_.end()) {
                return;
            }
            session = it->second;
            sessions_.erase(it);
        }

        {
            std::lock_guard<std::mutex> ioLock(session->ioMutex);
            session->active = false;

            if (forceTerminate && session->processHandle &&
                WaitForSingleObject(session->processHandle.get(), 0) == WAIT_TIMEOUT) {
                TerminateProcess(session->processHandle.get(), 1);
            }
        }

        if (session->outputThread.joinable() &&
            session->outputThread.get_id() != std::this_thread::get_id()) {
            session->outputThread.join();
        }
    }

    void StopAllSessions() {
        std::vector<std::string> sessionIds;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            sessionIds.reserve(sessions_.size());
            for (const auto& entry : sessions_) {
                sessionIds.push_back(entry.first);
            }
        }

        for (const auto& sessionId : sessionIds) {
            StopSession(sessionId, true);
        }
    }

    void CleanupSessionsForClient(const std::shared_ptr<ClientConnection>& client) {
        std::vector<std::string> ownedSessions;
        {
            std::lock_guard<std::mutex> lock(client->sessionsMutex);
            ownedSessions = client->sessionIds;
            client->sessionIds.clear();
        }

        for (const auto& sessionId : ownedSessions) {
            StopSession(sessionId, true);
        }
    }

    void DisconnectAllClients() {
        std::vector<std::shared_ptr<ClientConnection>> clients;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients = clients_;
        }

        for (const auto& client : clients) {
            if (client == nullptr) {
                continue;
            }

            client->connected = false;
            std::lock_guard<std::mutex> writeLock(client->writeMutex);
            if (client->pipe) {
                DisconnectNamedPipe(client->pipe.get());
                client->pipe.reset();
            }
        }
    }

    bool SendFrame(
        const std::shared_ptr<ClientConnection>& client,
        MessageType type,
        const std::string& sessionId,
        const void* payload,
        std::uint32_t payloadSize) {
        if (!client->connected) {
            return false;
        }

        const MessageHeader header = MakeHeader(type, sessionId, payloadSize);

        std::lock_guard<std::mutex> lock(client->writeMutex);
        if (!WriteExact(client->pipe.get(), &header, sizeof(header))) {
            client->connected = false;
            return false;
        }

        if (payloadSize == 0U) {
            return true;
        }

        if (!WriteExact(client->pipe.get(), payload, payloadSize)) {
            client->connected = false;
            return false;
        }

        return true;
    }

    bool SendTextFrame(
        const std::shared_ptr<ClientConnection>& client,
        MessageType type,
        const std::string& sessionId,
        const std::string& text) {
        return SendFrame(
            client,
            type,
            sessionId,
            text.data(),
            static_cast<std::uint32_t>(text.size()));
    }

    std::wstring GetComputerNameString() {
        wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD size = ARRAYSIZE(buffer);
        if (!GetComputerNameW(buffer, &size)) {
            return L"unknown";
        }
        return std::wstring(buffer, size);
    }

    std::string GenerateSessionId() {
        static std::atomic<unsigned long long> counter{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        const auto id = counter.fetch_add(1, std::memory_order_relaxed) + 1;

        return "sess-" + std::to_string(ticks) + "-" + std::to_string(id);
    }

    ScopedHandle CreateServerPipe() {
        HANDLE pipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            kPipeBufferBytes,
            kPipeBufferBytes,
            0,
            pipeSecurity_.attributes());

        return ScopedHandle(pipe);
    }

    void ReportServiceStatus(
        DWORD currentState,
        DWORD win32ExitCode,
        DWORD waitHint) {
        static DWORD checkPoint = 1;

        status_.dwCurrentState = currentState;
        status_.dwWin32ExitCode = win32ExitCode;
        status_.dwWaitHint = waitHint;
        status_.dwControlsAccepted =
            (currentState == SERVICE_START_PENDING) ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
        status_.dwCheckPoint =
            (currentState == SERVICE_RUNNING || currentState == SERVICE_STOPPED) ? 0 : checkPoint++;

        if (statusHandle_ != nullptr) {
            SetServiceStatus(statusHandle_, &status_);
        }
    }

    SERVICE_STATUS_HANDLE statusHandle_{nullptr};
    SERVICE_STATUS status_{};
    std::atomic_bool stopping_{false};
    ScopedHandle stopEvent_;
    PipeSecurityDescriptor pipeSecurity_;
    std::mutex sessionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    std::mutex clientThreadsMutex_;
    std::vector<std::thread> clientThreads_;
    std::mutex clientsMutex_;
    std::vector<std::shared_ptr<ClientConnection>> clients_;
    std::thread listenerThread_;

    static RootAcsService* instance_;
};

RootAcsService* RootAcsService::instance_ = nullptr;

} // namespace rootacs

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && (std::wcscmp(argv[1], L"--console") == 0 || std::wcscmp(argv[1], L"/console") == 0)) {
        SetConsoleCtrlHandler(&rootacs::RootAcsService::ConsoleCtrlHandler, TRUE);
        rootacs::RootAcsService service;
        const int exitCode = service.Execute();
        if (exitCode != 0) {
            std::wcerr << L"RootACS-Service console mode failed: "
                       << rootacs::DescribeWin32Error(static_cast<DWORD>(exitCode)) << std::endl;
        }
        return exitCode;
    }

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        {const_cast<LPWSTR>(rootacs::kServiceName), &rootacs::RootAcsService::ServiceMain},
        {nullptr, nullptr},
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        return static_cast<int>(GetLastError());
    }

    return 0;
}
