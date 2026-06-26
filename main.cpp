// USBIPD Manager 1.0 - usbipd-win 5.x GUI controller
// Bind and unbind are launched with Windows elevation (UAC). Attach targets WSL
// and does not require elevation. Command output is shown in the log textbox.

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"UsbipdManagerWindow";
constexpr wchar_t kApplicationTitle[] = L"USBIPD Manager 1.0";
constexpr size_t kMaximumLogCharacters = 256 * 1024;
constexpr UINT_PTR kRefreshTimerId = 1;
constexpr UINT kRefreshPeriodMs = 3000;

enum ControlId : int {
    ID_DEVICE_COMBO = 1001,
    ID_BIND_BUTTON,
    ID_UNBIND_BUTTON,
    ID_ATTACH_BUTTON,
    ID_REFRESH_BUTTON,
    ID_FORCE_CHECK,
    ID_STATUS_TEXT,
    ID_LOG_EDIT,
    ID_CLEAR_LOG_BUTTON,
};

struct Device {
    std::wstring busId;
    std::wstring description;
    bool bound = false;
    bool attached = false;
};

HWND g_mainWindow = nullptr;
HWND g_deviceCombo = nullptr;
HWND g_bindButton = nullptr;
HWND g_unbindButton = nullptr;
HWND g_attachButton = nullptr;
HWND g_refreshButton = nullptr;
HWND g_forceCheck = nullptr;
HWND g_statusText = nullptr;
HWND g_logEdit = nullptr;
HWND g_clearLogButton = nullptr;
std::vector<Device> g_devices;
std::wstring g_usbipdPath;
bool g_busy = false;

std::wstring Win32ErrorMessage(DWORD error)
{
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, error, 0,
                                        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = length && buffer ? std::wstring(buffer, length)
                                           : L"Win32 error " + std::to_wstring(error);
    if (buffer) {
        LocalFree(buffer);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        // usbipd state is UTF-8. This fallback keeps diagnostics readable if
        // a third-party wrapper emits bytes in the active Windows code page.
        const int fallbackSize = MultiByteToWideChar(CP_ACP, 0,
                                                      text.data(), static_cast<int>(text.size()),
                                                      nullptr, 0);
        if (fallbackSize <= 0) {
            return L"<unable to decode process output>";
        }
        std::wstring result(static_cast<size_t>(fallbackSize), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()),
                            result.data(), fallbackSize);
        return result;
    }

    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        text.data(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return "<unable to encode text>";
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring QuoteCommandLineArgument(const std::wstring& argument)
{
    if (argument.empty()) {
        return L"\"\"";
    }

    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(ch);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring FindUsbipdExecutable()
{
    std::vector<wchar_t> searchBuffer(32768);
    const DWORD written = SearchPathW(nullptr, L"usbipd.exe", nullptr,
                                      static_cast<DWORD>(searchBuffer.size()),
                                      searchBuffer.data(), nullptr);
    if (written > 0 && written < searchBuffer.size()) {
        return std::wstring(searchBuffer.data(), written);
    }

    wchar_t programFiles[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"ProgramW6432", programFiles, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        length = GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
    }
    if (length > 0 && length < MAX_PATH) {
        std::wstring candidate = std::wstring(programFiles) + L"\\usbipd-win\\usbipd.exe";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }

    return {};
}

bool RunProcessCapture(const std::wstring& executable,
                       const std::wstring& arguments,
                       std::string& output,
                       DWORD& exitCode,
                       std::wstring& error)
{
    output.clear();
    exitCode = static_cast<DWORD>(-1);
    error.clear();

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        error = L"CreatePipe failed: " + Win32ErrorMessage(GetLastError());
        return false;
    }

    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        error = L"SetHandleInformation failed: " + Win32ErrorMessage(GetLastError());
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }

    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        error = L"Unable to open NUL for child-process input: " +
                Win32ErrorMessage(GetLastError());
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = nullInput;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};
    std::wstring commandLine = QuoteCommandLineArgument(executable);
    if (!arguments.empty()) {
        commandLine += L" ";
        commandLine += arguments;
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    const BOOL created = CreateProcessW(
        executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);

    CloseHandle(writePipe);
    writePipe = nullptr;
    CloseHandle(nullInput);
    nullInput = nullptr;

    if (!created) {
        error = L"CreateProcess failed: " + Win32ErrorMessage(GetLastError());
        CloseHandle(readPipe);
        return false;
    }

    char buffer[4096];
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr)) {
            const DWORD readError = GetLastError();
            if (readError != ERROR_BROKEN_PIPE) {
                error = L"ReadFile failed: " + Win32ErrorMessage(readError);
            }
            break;
        }
        if (bytesRead == 0) {
            break;
        }
        output.append(buffer, buffer + bytesRead);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
        error = L"GetExitCodeProcess failed: " + Win32ErrorMessage(GetLastError());
    }

    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return error.empty();
}

std::wstring CurrentExecutablePath()
{
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

bool ReadBinaryFile(const std::wstring& path,
                    std::string& data,
                    std::wstring& error)
{
    data.clear();
    error.clear();

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Unable to read helper output: " + Win32ErrorMessage(GetLastError());
        return false;
    }

    char buffer[4096];
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr)) {
            error = L"Unable to read helper output: " + Win32ErrorMessage(GetLastError());
            CloseHandle(file);
            return false;
        }
        if (bytesRead == 0) {
            break;
        }
        data.append(buffer, buffer + bytesRead);
    }

    CloseHandle(file);
    return true;
}

bool WriteBinaryFile(const std::wstring& path,
                     const std::string& data,
                     std::wstring& error)
{
    error.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Unable to create helper output: " + Win32ErrorMessage(GetLastError());
        return false;
    }

    size_t offset = 0;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(data.size() - offset, 64 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, chunk, &written, nullptr)) {
            error = L"Unable to write helper output: " + Win32ErrorMessage(GetLastError());
            CloseHandle(file);
            return false;
        }
        if (written == 0) {
            error = L"Unable to write helper output: zero bytes written.";
            CloseHandle(file);
            return false;
        }
        offset += written;
    }

    CloseHandle(file);
    return true;
}

bool CreateTemporaryOutputFile(std::wstring& path, std::wstring& error)
{
    error.clear();
    wchar_t tempDirectory[MAX_PATH]{};
    const DWORD directoryLength = GetTempPathW(MAX_PATH, tempDirectory);
    if (directoryLength == 0 || directoryLength >= MAX_PATH) {
        error = L"GetTempPath failed: " + Win32ErrorMessage(GetLastError());
        return false;
    }

    wchar_t tempFile[MAX_PATH]{};
    if (GetTempFileNameW(tempDirectory, L"uim", 0, tempFile) == 0) {
        error = L"GetTempFileName failed: " + Win32ErrorMessage(GetLastError());
        return false;
    }

    path = tempFile;
    return true;
}

bool RunElevatedCapture(const std::wstring& executable,
                        const std::wstring& arguments,
                        std::string& output,
                        DWORD& exitCode,
                        std::wstring& error)
{
    output.clear();
    exitCode = static_cast<DWORD>(-1);
    error.clear();

    const std::wstring managerPath = CurrentExecutablePath();
    if (managerPath.empty()) {
        error = L"Unable to determine the USBIPD Manager executable path.";
        return false;
    }

    std::wstring outputPath;
    if (!CreateTemporaryOutputFile(outputPath, error)) {
        return false;
    }

    const std::wstring helperArguments =
        L"--elevated-helper " + QuoteCommandLineArgument(outputPath) + L" " +
        QuoteCommandLineArgument(executable) + L" " +
        QuoteCommandLineArgument(arguments);

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    executeInfo.hwnd = g_mainWindow;
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = managerPath.c_str();
    executeInfo.lpParameters = helperArguments.c_str();
    executeInfo.nShow = SW_HIDE;

    if (!ShellExecuteExW(&executeInfo)) {
        const DWORD shellError = GetLastError();
        DeleteFileW(outputPath.c_str());
        if (shellError == ERROR_CANCELLED) {
            error = L"Administrator elevation was cancelled.";
        } else {
            error = L"ShellExecuteEx failed: " + Win32ErrorMessage(shellError);
        }
        return false;
    }

    if (!executeInfo.hProcess) {
        DeleteFileW(outputPath.c_str());
        error = L"The elevated helper did not return a process handle.";
        return false;
    }

    WaitForSingleObject(executeInfo.hProcess, INFINITE);
    if (!GetExitCodeProcess(executeInfo.hProcess, &exitCode)) {
        error = L"GetExitCodeProcess failed: " + Win32ErrorMessage(GetLastError());
        CloseHandle(executeInfo.hProcess);
        DeleteFileW(outputPath.c_str());
        return false;
    }
    CloseHandle(executeInfo.hProcess);

    std::wstring readError;
    const bool readSucceeded = ReadBinaryFile(outputPath, output, readError);
    DeleteFileW(outputPath.c_str());
    if (!readSucceeded) {
        error = readError;
        return false;
    }

    return true;
}

int RunElevatedHelper(int argc, wchar_t** argv)
{
    if (argc != 5 || std::wstring(argv[1]) != L"--elevated-helper") {
        return 254;
    }

    const std::wstring outputPath = argv[2];
    const std::wstring executable = argv[3];
    const std::wstring arguments = argv[4];

    std::string output;
    DWORD exitCode = 255;
    std::wstring processError;
    if (!RunProcessCapture(executable, arguments, output, exitCode, processError)) {
        output += WideToUtf8(processError);
        output += "\r\n";
        exitCode = 255;
    }

    std::wstring writeError;
    if (!WriteBinaryFile(outputPath, output, writeError)) {
        return 253;
    }

    return static_cast<int>(exitCode);
}

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    std::vector<Device> ReadDevices()
    {
        SkipWhitespace();
        if (text_.compare(position_, 3, "\xEF\xBB\xBF") == 0) {
            position_ += 3;
            SkipWhitespace();
        }

        Expect('{');
        std::vector<Device> devices;
        SkipWhitespace();
        if (Consume('}')) {
            return devices;
        }

        for (;;) {
            const std::string key = ReadString();
            Expect(':');
            if (key == "Devices") {
                ReadDeviceArray(devices);
            } else {
                SkipValue();
            }

            SkipWhitespace();
            if (Consume('}')) {
                break;
            }
            Expect(',');
        }
        return devices;
    }

private:
    struct ParsedDevice {
        std::optional<std::string> busId;
        std::string description;
        std::optional<std::string> persistedGuid;
        std::optional<std::string> clientIpAddress;
        bool hasExplicitBound = false;
        bool explicitBound = false;
        bool hasExplicitAttached = false;
        bool explicitAttached = false;
    };

    void SkipWhitespace()
    {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
    }

    char Peek()
    {
        SkipWhitespace();
        if (position_ >= text_.size()) {
            throw std::runtime_error("unexpected end of JSON");
        }
        return text_[position_];
    }

    bool Consume(char expected)
    {
        SkipWhitespace();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void Expect(char expected)
    {
        if (!Consume(expected)) {
            throw std::runtime_error(std::string("expected '") + expected + "'");
        }
    }

    void ExpectLiteral(const char* literal)
    {
        SkipWhitespace();
        for (const char* p = literal; *p; ++p) {
            if (position_ >= text_.size() || text_[position_] != *p) {
                throw std::runtime_error(std::string("expected ") + literal);
            }
            ++position_;
        }
    }

    static int HexValue(char ch)
    {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }

    uint16_t ReadHex4()
    {
        if (position_ + 4 > text_.size()) {
            throw std::runtime_error("incomplete Unicode escape");
        }
        uint16_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = HexValue(text_[position_++]);
            if (digit < 0) {
                throw std::runtime_error("invalid Unicode escape");
            }
            value = static_cast<uint16_t>((value << 4) | digit);
        }
        return value;
    }

    static void AppendUtf8(std::string& output, uint32_t codePoint)
    {
        if (codePoint <= 0x7F) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint <= 0x10FFFF) {
            output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            throw std::runtime_error("invalid Unicode code point");
        }
    }

    std::string ReadString()
    {
        SkipWhitespace();
        Expect('"');
        std::string result;

        while (position_ < text_.size()) {
            const unsigned char ch = static_cast<unsigned char>(text_[position_++]);
            if (ch == '"') {
                return result;
            }
            if (ch < 0x20) {
                throw std::runtime_error("control character in JSON string");
            }
            if (ch != '\\') {
                result.push_back(static_cast<char>(ch));
                continue;
            }

            if (position_ >= text_.size()) {
                throw std::runtime_error("incomplete JSON escape");
            }
            const char escape = text_[position_++];
            switch (escape) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                uint32_t codePoint = ReadHex4();
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (position_ + 2 > text_.size() ||
                        text_[position_] != '\\' || text_[position_ + 1] != 'u') {
                        throw std::runtime_error("missing low surrogate");
                    }
                    position_ += 2;
                    const uint16_t low = ReadHex4();
                    if (low < 0xDC00 || low > 0xDFFF) {
                        throw std::runtime_error("invalid low surrogate");
                    }
                    codePoint = 0x10000u +
                                ((codePoint - 0xD800u) << 10) +
                                (low - 0xDC00u);
                } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                    throw std::runtime_error("unexpected low surrogate");
                }
                AppendUtf8(result, codePoint);
                break;
            }
            default:
                throw std::runtime_error("invalid JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    std::optional<std::string> ReadNullableString()
    {
        SkipWhitespace();
        if (Peek() == 'n') {
            ExpectLiteral("null");
            return std::nullopt;
        }
        return ReadString();
    }

    bool ReadBoolean()
    {
        SkipWhitespace();
        if (Peek() == 't') {
            ExpectLiteral("true");
            return true;
        }
        if (Peek() == 'f') {
            ExpectLiteral("false");
            return false;
        }
        throw std::runtime_error("expected Boolean");
    }

    void SkipNumber()
    {
        SkipWhitespace();
        const size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
                ++position_;
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
                ++position_;
            }
        }
        if (position_ == start) {
            throw std::runtime_error("expected number");
        }
    }

    void SkipValue()
    {
        SkipWhitespace();
        switch (Peek()) {
        case '"':
            static_cast<void>(ReadString());
            return;
        case '{':
            Expect('{');
            if (Consume('}')) return;
            for (;;) {
                static_cast<void>(ReadString());
                Expect(':');
                SkipValue();
                if (Consume('}')) return;
                Expect(',');
            }
        case '[':
            Expect('[');
            if (Consume(']')) return;
            for (;;) {
                SkipValue();
                if (Consume(']')) return;
                Expect(',');
            }
        case 't': ExpectLiteral("true"); return;
        case 'f': ExpectLiteral("false"); return;
        case 'n': ExpectLiteral("null"); return;
        default: SkipNumber(); return;
        }
    }

    ParsedDevice ReadDeviceObject()
    {
        ParsedDevice parsed;
        Expect('{');
        if (Consume('}')) {
            return parsed;
        }

        for (;;) {
            const std::string key = ReadString();
            Expect(':');

            if (key == "BusId") {
                parsed.busId = ReadNullableString();
            } else if (key == "Description") {
                const auto description = ReadNullableString();
                parsed.description = description.value_or("");
            } else if (key == "PersistedGuid") {
                parsed.persistedGuid = ReadNullableString();
            } else if (key == "ClientIPAddress") {
                parsed.clientIpAddress = ReadNullableString();
            } else if (key == "IsBound") {
                parsed.explicitBound = ReadBoolean();
                parsed.hasExplicitBound = true;
            } else if (key == "IsAttached") {
                parsed.explicitAttached = ReadBoolean();
                parsed.hasExplicitAttached = true;
            } else {
                SkipValue();
            }

            if (Consume('}')) {
                break;
            }
            Expect(',');
        }
        return parsed;
    }

    void ReadDeviceArray(std::vector<Device>& devices)
    {
        Expect('[');
        if (Consume(']')) {
            return;
        }

        for (;;) {
            if (Peek() == '{') {
                ParsedDevice parsed = ReadDeviceObject();
                if (parsed.busId && !parsed.busId->empty()) {
                    Device device;
                    device.busId = Utf8ToWide(*parsed.busId);
                    device.description = parsed.description.empty()
                                             ? L"Unknown USB device"
                                             : Utf8ToWide(parsed.description);
                    device.bound = parsed.hasExplicitBound
                                       ? parsed.explicitBound
                                       : parsed.persistedGuid.has_value();
                    device.attached = parsed.hasExplicitAttached
                                          ? parsed.explicitAttached
                                          : parsed.clientIpAddress.has_value();
                    if (device.attached) {
                        device.bound = true;
                    }
                    devices.push_back(std::move(device));
                }
            } else {
                SkipValue();
            }

            if (Consume(']')) {
                break;
            }
            Expect(',');
        }
    }

    const std::string& text_;
    size_t position_ = 0;
};

void SetStatus(const std::wstring& text)
{
    SetWindowTextW(g_statusText, text.c_str());
}

std::wstring NormalizeLogLineEndings(const std::wstring& text)
{
    std::wstring result;
    result.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            result.push_back(L'\r');
            if (i + 1 >= text.size() || text[i + 1] != L'\n') {
                result.push_back(L'\n');
            }
        } else if (text[i] == L'\n') {
            if (i == 0 || text[i - 1] != L'\r') {
                result.push_back(L'\r');
            }
            result.push_back(L'\n');
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

void TrimLogIfNeeded(size_t incomingCharacters)
{
    if (!g_logEdit) {
        return;
    }

    const LRESULT currentLength = GetWindowTextLengthW(g_logEdit);
    if (currentLength < 0 ||
        static_cast<size_t>(currentLength) + incomingCharacters <= kMaximumLogCharacters) {
        return;
    }

    const size_t targetRemoval =
        static_cast<size_t>(currentLength) + incomingCharacters -
        (kMaximumLogCharacters * 3 / 4);
    SendMessageW(g_logEdit, EM_SETSEL, 0,
                 static_cast<LPARAM>(std::min<size_t>(targetRemoval, currentLength)));
    SendMessageW(g_logEdit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(L""));
}

void AppendLogRaw(const std::wstring& text)
{
    if (!g_logEdit || text.empty()) {
        return;
    }

    const std::wstring normalized = NormalizeLogLineEndings(text);
    TrimLogIfNeeded(normalized.size());
    SendMessageW(g_logEdit, EM_SETSEL, static_cast<WPARAM>(-1),
                 static_cast<LPARAM>(-1));
    SendMessageW(g_logEdit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(normalized.c_str()));
    SendMessageW(g_logEdit, EM_SCROLLCARET, 0, 0);
}

void AppendLog(const std::wstring& text)
{
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);

    wchar_t timestamp[32]{};
    swprintf(timestamp, sizeof(timestamp) / sizeof(timestamp[0]),
             L"[%02u:%02u:%02u] ", localTime.wHour,
             localTime.wMinute, localTime.wSecond);

    AppendLogRaw(std::wstring(timestamp) + text + L"\r\n");
}

void AppendCommandResult(const std::wstring& arguments,
                         const std::string& output,
                         DWORD exitCode)
{
    AppendLog(L"> usbipd " + arguments);
    if (!output.empty()) {
        std::wstring decoded = Utf8ToWide(output);
        if (!decoded.empty()) {
            AppendLogRaw(decoded);
            if (decoded.back() != L'\r' && decoded.back() != L'\n') {
                AppendLogRaw(L"\r\n");
            }
        }
    }
    AppendLog(L"usbipd exit code: " + std::to_wstring(exitCode));
}

int SelectedDeviceIndex()
{
    const LRESULT selection = SendMessageW(g_deviceCombo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR || selection < 0 ||
        static_cast<size_t>(selection) >= g_devices.size()) {
        return -1;
    }
    return static_cast<int>(selection);
}

void UpdateButtonState()
{
    const int index = SelectedDeviceIndex();
    const bool valid = index >= 0 && !g_busy;
    const Device* selected = valid ? &g_devices[static_cast<size_t>(index)] : nullptr;

    EnableWindow(g_bindButton, selected && !selected->bound);
    EnableWindow(g_unbindButton, selected && selected->bound);
    EnableWindow(g_attachButton, selected && selected->bound && !selected->attached);
    EnableWindow(g_refreshButton, !g_busy);
    EnableWindow(g_deviceCombo, !g_busy);
    EnableWindow(g_forceCheck, !g_busy);
    EnableWindow(g_clearLogButton, !g_busy);
}

void SetBusy(bool busy)
{
    g_busy = busy;
    UpdateButtonState();
    if (busy) {
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    } else {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
}

std::wstring DeviceDisplayText(const Device& device)
{
    const wchar_t* state = device.attached ? L"Attached"
                           : device.bound   ? L"Shared"
                                            : L"Not shared";
    return device.busId + L"  |  " + device.description + L"  |  " + state;
}

bool RefreshDevices(bool showErrorDialog)
{
    if (g_busy) {
        return false;
    }

    if (g_usbipdPath.empty()) {
        g_usbipdPath = FindUsbipdExecutable();
    }
    if (g_usbipdPath.empty()) {
        const std::wstring message =
            L"usbipd.exe was not found in PATH or in the default installation directory.";
        SetStatus(message);
        if (showErrorDialog) {
            AppendLog(message);
            MessageBoxW(g_mainWindow, message.c_str(), kApplicationTitle, MB_ICONERROR);
        }
        UpdateButtonState();
        return false;
    }

    const int oldIndex = SelectedDeviceIndex();
    const std::wstring oldBusId = oldIndex >= 0
                                      ? g_devices[static_cast<size_t>(oldIndex)].busId
                                      : std::wstring{};

    std::string output;
    DWORD exitCode = 0;
    std::wstring error;
    if (!RunProcessCapture(g_usbipdPath, L"state", output, exitCode, error)) {
        SetStatus(error);
        if (showErrorDialog) {
            AppendLog(L"usbipd state failed: " + error);
            MessageBoxW(g_mainWindow, error.c_str(), L"usbipd state failed", MB_ICONERROR);
        }
        UpdateButtonState();
        return false;
    }

    if (exitCode != 0) {
        std::wstring message = L"usbipd state failed with exit code " +
                               std::to_wstring(exitCode);
        const std::wstring details = Utf8ToWide(output);
        if (!details.empty()) {
            message += L": " + details;
        }
        SetStatus(message);
        if (showErrorDialog) {
            AppendCommandResult(L"state", output, exitCode);
            MessageBoxW(g_mainWindow, message.c_str(), L"usbipd state failed", MB_ICONERROR);
        }
        UpdateButtonState();
        return false;
    }

    std::vector<Device> newDevices;
    try {
        newDevices = JsonReader(output).ReadDevices();
    } catch (const std::exception& exception) {
        const std::wstring message = L"Unable to parse usbipd state JSON: " +
                                     Utf8ToWide(exception.what());
        SetStatus(message);
        if (showErrorDialog) {
            AppendLog(message);
            MessageBoxW(g_mainWindow, message.c_str(), L"usbipd state failed", MB_ICONERROR);
        }
        UpdateButtonState();
        return false;
    }

    g_devices = std::move(newDevices);
    SendMessageW(g_deviceCombo, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_deviceCombo, CB_RESETCONTENT, 0, 0);

    int newSelection = -1;
    for (size_t i = 0; i < g_devices.size(); ++i) {
        const std::wstring display = DeviceDisplayText(g_devices[i]);
        SendMessageW(g_deviceCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(display.c_str()));
        if (!oldBusId.empty() && g_devices[i].busId == oldBusId) {
            newSelection = static_cast<int>(i);
        }
    }

    if (newSelection < 0 && !g_devices.empty()) {
        newSelection = 0;
    }
    SendMessageW(g_deviceCombo, CB_SETCURSEL, newSelection, 0);
    SendMessageW(g_deviceCombo, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_deviceCombo, nullptr, TRUE);

    std::wstring summary;
    if (g_devices.empty()) {
        summary = L"No connected USB devices were reported by usbipd.";
    } else {
        summary = std::to_wstring(g_devices.size()) +
                  (g_devices.size() == 1 ? L" connected USB device." :
                                           L" connected USB devices.");
    }
    SetStatus(summary);
    if (showErrorDialog) {
        AppendLog(summary);
    }
    UpdateButtonState();
    return true;
}

void ExecuteBindingCommand(bool bind)
{
    const int index = SelectedDeviceIndex();
    if (index < 0 || g_busy) {
        return;
    }

    const Device selected = g_devices[static_cast<size_t>(index)];
    SetBusy(true);
    SetStatus((bind ? L"Binding " : L"Unbinding ") + selected.busId + L"...");

    std::wstring arguments = bind ? L"bind " : L"unbind ";
    if (bind && SendMessageW(g_forceCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        arguments += L"--force ";
    }
    arguments += L"--busid ";
    arguments += QuoteCommandLineArgument(selected.busId);

    std::string output;
    DWORD exitCode = 0;
    std::wstring error;
    const bool launched =
        RunElevatedCapture(g_usbipdPath, arguments, output, exitCode, error);

    SetBusy(false);
    if (!launched) {
        SetStatus(error);
        AppendLog(std::wstring(bind ? L"Bind failed: " : L"Unbind failed: ") + error);
        MessageBoxW(g_mainWindow, error.c_str(), kApplicationTitle, MB_ICONERROR);
        return;
    }

    AppendCommandResult(arguments, output, exitCode);
    if (exitCode != 0) {
        const std::wstring message =
            std::wstring(bind ? L"Bind" : L"Unbind") +
            L" failed with exit code " + std::to_wstring(exitCode) + L".";
        SetStatus(message);
        MessageBoxW(g_mainWindow, message.c_str(), kApplicationTitle, MB_ICONERROR);
        RefreshDevices(false);
        return;
    }

    Sleep(150);
    RefreshDevices(true);
}

void ExecuteAttachCommand()
{
    const int index = SelectedDeviceIndex();
    if (index < 0 || g_busy) {
        return;
    }

    const Device selected = g_devices[static_cast<size_t>(index)];
    if (!selected.bound || selected.attached) {
        UpdateButtonState();
        return;
    }

    SetBusy(true);
    SetStatus(L"Attaching " + selected.busId + L" to WSL...");

    const std::wstring arguments =
        L"attach --wsl --busid " + QuoteCommandLineArgument(selected.busId);

    std::string output;
    DWORD exitCode = 0;
    std::wstring error;
    const bool launched =
        RunProcessCapture(g_usbipdPath, arguments, output, exitCode, error);

    SetBusy(false);
    if (!launched) {
        SetStatus(error);
        AppendLog(L"Attach failed: " + error);
        MessageBoxW(g_mainWindow, error.c_str(), kApplicationTitle, MB_ICONERROR);
        return;
    }

    AppendCommandResult(arguments, output, exitCode);
    if (exitCode != 0) {
        const std::wstring message =
            L"Attach failed with exit code " + std::to_wstring(exitCode) + L".";
        SetStatus(message);
        MessageBoxW(g_mainWindow, message.c_str(), kApplicationTitle, MB_ICONERROR);
        RefreshDevices(false);
        return;
    }

    Sleep(150);
    RefreshDevices(true);
}

void ApplyDefaultFont(HWND control)
{
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

void LayoutControls(HWND window)
{
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = 12;
    const int normalButtonWidth = 86;
    const int attachButtonWidth = 122;
    const int buttonHeight = 28;
    const int gap = 8;

    MoveWindow(g_deviceCombo, margin, 32,
               std::max(100, width - margin * 2), 300, TRUE);

    int x = margin;
    MoveWindow(g_bindButton, x, 72, normalButtonWidth, buttonHeight, TRUE);
    x += normalButtonWidth + gap;
    MoveWindow(g_unbindButton, x, 72, normalButtonWidth, buttonHeight, TRUE);
    x += normalButtonWidth + gap;
    MoveWindow(g_attachButton, x, 72, attachButtonWidth, buttonHeight, TRUE);
    x += attachButtonWidth + gap;
    MoveWindow(g_refreshButton, x, 72, normalButtonWidth, buttonHeight, TRUE);
    x += normalButtonWidth + gap;
    MoveWindow(g_forceCheck, x, 76,
               std::max(120, width - x - margin), 22, TRUE);

    MoveWindow(g_statusText, margin, 114,
               std::max(100, width - margin * 2), 36, TRUE);

    const int clearButtonWidth = 92;
    MoveWindow(g_clearLogButton,
               std::max(margin, width - margin - clearButtonWidth),
               154, clearButtonWidth, 26, TRUE);

    const int logTop = 184;
    MoveWindow(g_logEdit, margin, logTop,
               std::max(100, width - margin * 2),
               std::max(80, height - logTop - margin), TRUE);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        g_mainWindow = window;

        HWND label = CreateWindowExW(0, L"STATIC", L"USB device:",
                                     WS_CHILD | WS_VISIBLE,
                                     12, 10, 100, 20,
                                     window, nullptr, nullptr, nullptr);
        g_deviceCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                            CBS_DROPDOWNLIST | WS_VSCROLL,
                                        12, 32, 700, 300,
                                        window,
                                        reinterpret_cast<HMENU>(ID_DEVICE_COMBO),
                                        nullptr, nullptr);
        g_bindButton = CreateWindowExW(0, L"BUTTON", L"Bind",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       12, 72, 86, 28,
                                       window,
                                       reinterpret_cast<HMENU>(ID_BIND_BUTTON),
                                       nullptr, nullptr);
        g_unbindButton = CreateWindowExW(0, L"BUTTON", L"Unbind",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         106, 72, 86, 28,
                                         window,
                                         reinterpret_cast<HMENU>(ID_UNBIND_BUTTON),
                                         nullptr, nullptr);
        g_attachButton = CreateWindowExW(0, L"BUTTON", L"Attach to WSL",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         200, 72, 122, 28,
                                         window,
                                         reinterpret_cast<HMENU>(ID_ATTACH_BUTTON),
                                         nullptr, nullptr);
        g_refreshButton = CreateWindowExW(0, L"BUTTON", L"Refresh",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                          330, 72, 86, 28,
                                          window,
                                          reinterpret_cast<HMENU>(ID_REFRESH_BUTTON),
                                          nullptr, nullptr);
        g_forceCheck = CreateWindowExW(0, L"BUTTON", L"Force bind (USB filter workaround)",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                       424, 74, 300, 24,
                                       window,
                                       reinterpret_cast<HMENU>(ID_FORCE_CHECK),
                                       nullptr, nullptr);
        g_statusText = CreateWindowExW(0, L"STATIC", L"Reading usbipd state...",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       12, 114, 700, 36,
                                       window,
                                       reinterpret_cast<HMENU>(ID_STATUS_TEXT),
                                       nullptr, nullptr);
        HWND logLabel = CreateWindowExW(0, L"STATIC", L"usbipd log:",
                                        WS_CHILD | WS_VISIBLE,
                                        12, 158, 100, 20,
                                        window, nullptr, nullptr, nullptr);
        g_clearLogButton = CreateWindowExW(0, L"BUTTON", L"Clear log",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           700, 154, 92, 26,
                                           window,
                                           reinterpret_cast<HMENU>(ID_CLEAR_LOG_BUTTON),
                                           nullptr, nullptr);
        g_logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                        WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                                        ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL,
                                    12, 184, 780, 300,
                                    window,
                                    reinterpret_cast<HMENU>(ID_LOG_EDIT),
                                    nullptr, nullptr);

        ApplyDefaultFont(label);
        ApplyDefaultFont(g_deviceCombo);
        ApplyDefaultFont(g_bindButton);
        ApplyDefaultFont(g_unbindButton);
        ApplyDefaultFont(g_attachButton);
        ApplyDefaultFont(g_refreshButton);
        ApplyDefaultFont(g_forceCheck);
        ApplyDefaultFont(g_statusText);
        ApplyDefaultFont(logLabel);
        ApplyDefaultFont(g_clearLogButton);
        ApplyDefaultFont(g_logEdit);

        SendMessageW(g_logEdit, EM_SETLIMITTEXT,
                     static_cast<WPARAM>(kMaximumLogCharacters), 0);

        g_usbipdPath = FindUsbipdExecutable();
        SetTimer(window, kRefreshTimerId, kRefreshPeriodMs, nullptr);
        LayoutControls(window);
        AppendLog(L"USBIPD Manager 1.1 started (Attach to WSL enabled).");
        RefreshDevices(true);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 820;
        info->ptMinTrackSize.y = 430;
        return 0;
    }

    case WM_SIZE:
        LayoutControls(window);
        return 0;

    case WM_TIMER:
        if (wParam == kRefreshTimerId && !g_busy &&
            SendMessageW(g_deviceCombo, CB_GETDROPPEDSTATE, 0, 0) == FALSE) {
            RefreshDevices(false);
        }
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        if (id == ID_DEVICE_COMBO && notification == CBN_SELCHANGE) {
            UpdateButtonState();
            return 0;
        }
        if (id == ID_BIND_BUTTON && notification == BN_CLICKED) {
            ExecuteBindingCommand(true);
            return 0;
        }
        if (id == ID_UNBIND_BUTTON && notification == BN_CLICKED) {
            ExecuteBindingCommand(false);
            return 0;
        }
        if (id == ID_ATTACH_BUTTON && notification == BN_CLICKED) {
            ExecuteAttachCommand();
            return 0;
        }
        if (id == ID_REFRESH_BUTTON && notification == BN_CLICKED) {
            AppendLog(L"> usbipd state");
            RefreshDevices(true);
            return 0;
        }
        if (id == ID_CLEAR_LOG_BUTTON && notification == BN_CLICKED) {
            SetWindowTextW(g_logEdit, L"");
            AppendLog(L"Log cleared.");
            return 0;
        }
        break;
    }

    case WM_DESTROY:
        KillTimer(window, kRefreshTimerId);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && std::wstring(argv[1]) == L"--elevated-helper") {
        const int result = RunElevatedHelper(argc, argv);
        LocalFree(argv);
        return result;
    }
    if (argv) {
        LocalFree(argv);
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&windowClass)) {
        MessageBoxW(nullptr, Win32ErrorMessage(GetLastError()).c_str(),
                    L"Unable to register window class", MB_ICONERROR);
        return 1;
    }

    HWND window = CreateWindowExW(
        0, kWindowClass, kApplicationTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 560,
        nullptr, nullptr, instance, nullptr);

    if (!window) {
        MessageBoxW(nullptr, Win32ErrorMessage(GetLastError()).c_str(),
                    L"Unable to create window", MB_ICONERROR);
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
