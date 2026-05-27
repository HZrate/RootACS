#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
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

bool WriteFrame(
    HANDLE pipe,
    MessageType type,
    const std::string& sessionId,
    const void* payload,
    std::uint32_t payloadSize) {
    const MessageHeader header = MakeHeader(type, sessionId, payloadSize);
    if (!WriteExact(pipe, &header, sizeof(header))) {
        return false;
    }

    if (payloadSize == 0U) {
        return true;
    }

    return WriteExact(pipe, payload, payloadSize);
}

namespace {

constexpr DWORD kConnectTimeoutMs = 5000;
constexpr DWORD kReaderPollDelayMs = 10;
constexpr DWORD kMinFrameHeaderBytes = sizeof(MessageHeader);
constexpr char kClientLogPath[] = "client-debug.log";

void AppendClientLog(const std::string& message) {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    std::ofstream stream(kClientLogPath, std::ios::app);
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

class RootAcsClient {
public:
    RootAcsClient() {
        instance_ = this;
    }

    ~RootAcsClient() {
        Stop();
        instance_ = nullptr;
    }

    int Run() {
        AppendClientLog("Run start");

        if (!SetConsoleCtrlHandler(&RootAcsClient::ConsoleCtrlHandler, TRUE)) {
            const DWORD error = GetLastError();
            std::wcerr << L"SetConsoleCtrlHandler failed: " << DescribeWin32Error(error) << std::endl;
        }

        if (!Connect()) {
            return 1;
        }

        if (!PerformHandshake()) {
            return 1;
        }

        readerThread_ = std::thread([this]() { ReaderLoop(); });

        if (!StartInteractiveSession()) {
            Stop();
            return 1;
        }

        std::string line;
        while (!stopping_) {
            std::cout << "[rootacs] > " << std::flush;
            AppendClientLog("Waiting for stdin line");
            if (!std::getline(std::cin, line)) {
                AppendClientLog("getline failed or EOF");
                break;
            }
            AppendClientLog("Read stdin line: " + line);

            std::string currentSessionId;
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                currentSessionId = sessionId_;
            }

            if (currentSessionId.empty()) {
                std::cerr << "[rootacs] no active session" << std::endl;
                break;
            }

            line += "\r\n";
            AppendClientLog(
                "Sending stdin frame session=" + currentSessionId +
                " bytes=" + std::to_string(line.size()));
            const bool sent = SendFrame(
                MessageType::StdinData,
                currentSessionId,
                line.data(),
                static_cast<std::uint32_t>(line.size()));
            AppendClientLog(
                "SendFrame stdin session=" + currentSessionId +
                " bytes=" + std::to_string(line.size()) +
                " ok=" + (sent ? "1" : "0"));
            if (!sent) {
                const DWORD error = GetLastError();
                AppendClientLog("Stdin write failed error=" + std::to_string(error));
                std::wcerr << L"[rootacs] write failed: " << DescribeWin32Error(error) << std::endl;
                break;
            }
        }

        Stop();
        return 0;
    }

    static BOOL WINAPI ConsoleCtrlHandler(DWORD controlType) {
        if (instance_ == nullptr) {
            return FALSE;
        }

        switch (controlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            instance_->RequestServiceExit();
            instance_->Stop();
            return TRUE;
        default:
            return FALSE;
        }
    }

private:
    bool Connect() {
        if (!WaitNamedPipeW(kPipeName, kConnectTimeoutMs)) {
            const DWORD error = GetLastError();
            AppendClientLog("WaitNamedPipe failed error=" + std::to_string(error));
            std::wcerr << L"WaitNamedPipe failed: " << DescribeWin32Error(error) << std::endl;
            return false;
        }

        pipe_.reset(CreateFileW(
            kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));

        if (!pipe_) {
            const DWORD error = GetLastError();
            AppendClientLog("CreateFile pipe failed error=" + std::to_string(error));
            std::wcerr << L"CreateFile for pipe failed: " << DescribeWin32Error(error) << std::endl;
            return false;
        }

        DWORD mode = PIPE_READMODE_BYTE;
        if (!SetNamedPipeHandleState(pipe_.get(), &mode, nullptr, nullptr)) {
            const DWORD error = GetLastError();
            AppendClientLog("SetNamedPipeHandleState failed error=" + std::to_string(error));
            std::wcerr << L"SetNamedPipeHandleState failed: " << DescribeWin32Error(error) << std::endl;
            return false;
        }

        AppendClientLog("Connect complete");
        return true;
    }

    bool PerformHandshake() {
        if (!SendFrame(MessageType::AuthInit, {}, nullptr, 0)) {
            const DWORD error = GetLastError();
            AppendClientLog("AUTH_INIT send failed error=" + std::to_string(error));
            std::wcerr << L"AUTH_INIT send failed: " << DescribeWin32Error(error) << std::endl;
            return false;
        }

        MessageHeader header{};
        std::vector<std::uint8_t> payload;
        if (!ReceiveFrame(&header, &payload)) {
            const DWORD error = GetLastError();
            AppendClientLog("Handshake read failed error=" + std::to_string(error));
            std::wcerr << L"Handshake read failed: " << DescribeWin32Error(error) << std::endl;
            return false;
        }

        if (header.type != MessageType::AuthMetadata) {
            std::cerr << "[rootacs] unexpected handshake response" << std::endl;
            return false;
        }

        std::cout << "[rootacs] connected: " << NarrowFromBuffer(payload) << std::endl;
        AppendClientLog("Handshake complete");
        return true;
    }

    bool StartInteractiveSession() {
        const std::wstring shellPath = PromptShellPath();
        const std::string shellUtf8 = WideToUtf8(shellPath);
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            sessionId_.clear();
            sessionStartError_.clear();
            sessionStartComplete_ = false;
            sessionStartFailed_ = false;
        }

        if (!SendFrame(
                MessageType::InitShell,
                {},
                shellUtf8.data(),
                static_cast<std::uint32_t>(shellUtf8.size()))) {
            const DWORD error = GetLastError();
            AppendClientLog("INIT_SHELL send failed error=" + std::to_string(error));
            std::wcerr << L"INIT_SHELL send failed: " << DescribeWin32Error(error) << std::endl;
            return false;
        }

        std::unique_lock<std::mutex> lock(sessionMutex_);
        sessionStartedCv_.wait(lock, [this]() {
            return stopping_ || sessionStartComplete_ || sessionStartFailed_;
        });

        if (stopping_) {
            return false;
        }

        if (sessionStartFailed_) {
            if (!sessionStartError_.empty()) {
                std::cerr << "[rootacs] service error: " << sessionStartError_ << std::endl;
            } else {
                std::cerr << "[rootacs] session bootstrap failed" << std::endl;
            }
            return false;
        }

        return !sessionId_.empty();
    }

    void ReaderLoop() {
        MessageHeader header{};
        std::vector<std::uint8_t> payload;

        while (!stopping_) {
            DWORD availableBytes = 0;
            if (!PeekNamedPipe(pipe_.get(), nullptr, 0, nullptr, &availableBytes, nullptr)) {
                if (!stopping_) {
                    const DWORD error = GetLastError();
                    AppendClientLog("ReaderLoop peek failed error=" + std::to_string(error));
                    {
                        std::lock_guard<std::mutex> lock(sessionMutex_);
                        if (!sessionStartComplete_ && !sessionStartFailed_) {
                            sessionStartFailed_ = true;
                        }
                    }
                    sessionStartedCv_.notify_all();
                    if (error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED) {
                        std::wcerr << L"[rootacs] read failed: " << DescribeWin32Error(error) << std::endl;
                    }
                }
                break;
            }

            if (availableBytes < kMinFrameHeaderBytes) {
                Sleep(kReaderPollDelayMs);
                continue;
            }

            if (!ReceiveFrame(&header, &payload)) {
                if (!stopping_) {
                    const DWORD error = GetLastError();
                    AppendClientLog("ReaderLoop receive failed error=" + std::to_string(error));
                    {
                        std::lock_guard<std::mutex> lock(sessionMutex_);
                        if (!sessionStartComplete_ && !sessionStartFailed_) {
                            sessionStartFailed_ = true;
                        }
                    }
                    sessionStartedCv_.notify_all();
                    if (error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED) {
                        std::wcerr << L"[rootacs] read failed: " << DescribeWin32Error(error) << std::endl;
                    }
                }
                break;
            }

            AppendClientLog(
                "ReaderLoop frame type=" + std::to_string(static_cast<int>(header.type)) +
                " bytes=" + std::to_string(payload.size()));
            PrintProtocolMessage(header, payload);
        }
    }

    void Stop() {
        const bool alreadyStopping = stopping_.exchange(true);
        if (!alreadyStopping) {
            AppendClientLog("Stop requested");
            RequestSessionExit();
        }

        sessionStartedCv_.notify_all();
        ClosePipe();

        if (readerThread_.joinable() && readerThread_.get_id() != std::this_thread::get_id()) {
            readerThread_.join();
        }
    }

    bool SendFrame(
        MessageType type,
        const std::string& sessionId,
        const void* payload,
        std::uint32_t payloadSize) {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return WriteFrame(pipe_.get(), type, sessionId, payload, payloadSize);
    }

    bool SendTextFrame(MessageType type, const std::string& sessionId, const std::string& text) {
        return SendFrame(type, sessionId, text.data(), static_cast<std::uint32_t>(text.size()));
    }

    bool ReceiveFrame(MessageHeader* header, std::vector<std::uint8_t>* payload) {
        return ReadFrame(pipe_.get(), header, payload);
    }

    std::wstring PromptShellPath() const {
        while (true) {
            std::cout
                << "[RootACS] Select Shell:\n"
                << "1. CMD (Legacy)\n"
                << "2. PowerShell (5.1)\n"
                << "3. PowerShell Core (pwsh)\n"
                << "4. Custom Path\n"
                << "> " << std::flush;

            std::string choice;
            if (!std::getline(std::cin, choice)) {
                return L"C:\\Windows\\System32\\cmd.exe";
            }

            if (choice == "1" || choice.empty()) {
                return L"C:\\Windows\\System32\\cmd.exe";
            }
            if (choice == "2") {
                return L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
            }
            if (choice == "3") {
                return L"pwsh.exe";
            }
            if (choice == "4") {
                std::cout << "Shell path: " << std::flush;
                std::string customPath;
                if (!std::getline(std::cin, customPath) || customPath.empty()) {
                    return L"C:\\Windows\\System32\\cmd.exe";
                }
                return Utf8ToWide(customPath);
            }
        }
    }

    void PrintProtocolMessage(const MessageHeader& header, const std::vector<std::uint8_t>& payload) {
        const std::string text = NarrowFromBuffer(payload);

        switch (header.type) {
        case MessageType::SessionStarted: {
            const std::string sessionId = ExtractSessionId(header);
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                sessionId_ = sessionId;
                sessionStartComplete_ = true;
                sessionStartFailed_ = false;
            }
            sessionStartedCv_.notify_all();
            std::cout << "[rootacs] session " << sessionId << " started with " << text << std::endl;
            AppendClientLog("Session started: " + sessionId);
            break;
        }
        case MessageType::StdoutData:
            std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
            std::cout.flush();
            break;
        case MessageType::Status:
            std::cerr << "\n[rootacs][status][" << ExtractSessionId(header) << "] " << text << std::endl;
            break;
        case MessageType::Error:
            if (ExtractSessionId(header).empty()) {
                {
                    std::lock_guard<std::mutex> lock(sessionMutex_);
                    if (!sessionStartComplete_) {
                        sessionStartError_ = text;
                        sessionStartFailed_ = true;
                    }
                }
                sessionStartedCv_.notify_all();
            }
            std::cerr << "\n[rootacs][error][" << ExtractSessionId(header) << "] " << text << std::endl;
            break;
        default:
            std::cerr << "\n[rootacs][protocol] unexpected frame type "
                      << static_cast<int>(header.type) << std::endl;
            break;
        }
    }

    void RequestServiceExit() {
        if (!pipe_) {
            return;
        }
        SendTextFrame(MessageType::ExitService, {}, "CTRL_C");
    }

    void RequestSessionExit() {
        if (!pipe_) {
            return;
        }

        std::string currentSessionId;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            currentSessionId = sessionId_;
        }

        if (!currentSessionId.empty()) {
            SendTextFrame(MessageType::ExitSession, currentSessionId, "CLIENT_CLOSE");
        }
    }

    void ClosePipe() {
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (pipe_) {
            pipe_.reset();
        }
    }

    ScopedHandle pipe_;
    std::thread readerThread_;
    std::atomic_bool stopping_{false};
    std::mutex writeMutex_;
    std::mutex sessionMutex_;
    std::condition_variable sessionStartedCv_;
    std::string sessionId_;
    std::string sessionStartError_;
    bool sessionStartComplete_{false};
    bool sessionStartFailed_{false};

    static RootAcsClient* instance_;
};

RootAcsClient* RootAcsClient::instance_ = nullptr;

} // namespace rootacs

int wmain() {
    rootacs::RootAcsClient client;
    return client.Run();
}
