// SPDX-License-Identifier: GPL-2.0-only
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "IBonDriver2.h"
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <unordered_set>
#include "../winscard/card_command.hpp"

namespace {
HMODULE moduleHandle;
constexpr DWORD invalidChannel = 0xffffffff;
constexpr uint32_t ready = 1, tune = 2, quit = 5, purge = 6, ack = 0x100, ts = 0x200, fault = 0x201,
                   signalFrame = 4;
constexpr uint32_t cardRequest = 16, cardReply = 0x110;
constexpr size_t maxPayload = 188 * 1024, maxQueued = 32 * 1024 * 1024;
struct Header {
    uint32_t type, generation, length;
};
bool readAll(HANDLE h, void *p, DWORD n) {
    auto *b = static_cast<BYTE *>(p);
    while (n) {
        DWORD got = 0;
        if (!ReadFile(h, b, n, &got, nullptr) || !got)
            return false;
        b += got;
        n -= got;
    }
    return true;
}
bool writeAll(HANDLE h, const void *p, DWORD n) {
    auto *b = static_cast<const BYTE *>(p);
    while (n) {
        DWORD got = 0;
        if (!WriteFile(h, b, n, &got, nullptr) || !got)
            return false;
        b += got;
        n -= got;
    }
    return true;
}
void closeHandle(HANDLE &h) {
    if (h && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    h = nullptr;
}
void logError(const std::string &s) { OutputDebugStringA(("BonDriver_CECH: " + s + "\n").c_str()); }

class Bon final : public IBonDriver2 {
    std::mutex control, mutex;
    HANDLE input = nullptr, output = nullptr, process = nullptr;
    HANDLE response = CreateEventW(nullptr, TRUE, FALSE, nullptr),
           dataReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread reader;
    bool opening = false, dead = true, success = false, overflow = false;
    uint32_t generation = 0;
    DWORD current = invalidChannel;
    float cnr = 0;
    size_t queued = 0;
    std::deque<std::vector<BYTE>> queue;
    std::vector<BYTE> returned;
    std::vector<BYTE> cardResponse;
    unsigned cardUsers = 0;
    std::atomic_bool tunerActive{false};
    std::atomic_uint64_t hardwareEpoch{0};
    std::wstring names[50];

    bool send(uint32_t type, uint32_t gen, const void *data = nullptr, DWORD length = 0) {
        Header h{type, gen, length};
        return input && writeAll(input, &h, sizeof h) && (!length || writeAll(input, data, length));
    }
    void clearQueue() {
        queue.clear();
        queued = 0;
        ResetEvent(dataReady);
    }
    void receive() {
        Header h{};
        while (readAll(output, &h, sizeof h)) {
            if (h.length > maxPayload)
                break;
            std::vector<BYTE> payload(h.length);
            if (h.length && !readAll(output, payload.data(), h.length))
                break;
            std::lock_guard<std::mutex> lock(mutex);
            if (h.type == ready) {
                success = payload.empty();
                if (!success)
                    logError(std::string(payload.begin(), payload.end()));
                SetEvent(response);
                continue;
            }
            if (h.generation != generation)
                continue;
            if (h.type == cardReply) {
                cardResponse = std::move(payload);
                success = true;
                SetEvent(response);
                continue;
            }
            if (h.type == ack) {
                success = payload.empty();
                if (!success)
                    logError(std::string(payload.begin(), payload.end()));
                SetEvent(response);
            } else if (h.type == signalFrame && payload.size() == sizeof(float)) {
                std::memcpy(&cnr, payload.data(), sizeof(float));
            } else if (h.type == fault) {
                logError(std::string(payload.begin(), payload.end()));
                dead = true;
                cnr = 0;
                clearQueue();
                SetEvent(dataReady);
            } else if (h.type == ts && !dead && !overflow) {
                if (payload.empty() || payload.size() % 188)
                    break;
                if (queued + payload.size() > maxQueued) {
                    overflow = true;
                    cnr = 0;
                    clearQueue();
                    SetEvent(dataReady);
                    logError("TS queue overflow; reception failed");
                } else {
                    queued += payload.size();
                    queue.push_back(std::move(payload));
                    SetEvent(dataReady);
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        dead = true;
        opening = false;
        success = false;
        current = invalidChannel;
        cnr = 0;
        clearQueue();
        SetEvent(response);
        SetEvent(dataReady);
    }
    bool spawn() {
        wchar_t path[32768];
        DWORD n = GetModuleFileNameW(moduleHandle, path, 32768);
        if (!n || n >= 32768)
            return false;
        std::wstring directory(path, n);
        directory.resize(directory.find_last_of(L"\\/") + 1);
        std::wstring exe = directory + L"cech-host.exe", command = L"\"" + exe + L"\"";
        SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
        HANDLE childIn = nullptr, childOut = nullptr, errorFile = nullptr;
        auto cleanup = [&]() {
            closeHandle(childIn);
            closeHandle(childOut);
            closeHandle(errorFile);
        };
        if (!CreatePipe(&childIn, &input, &security, 4096) ||
            !SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0) ||
            !CreatePipe(&output, &childOut, &security, 1024 * 1024) ||
            !SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0)) {
            cleanup();
            return false;
        }
        std::wstring log = directory + L"cech-host-" + std::to_wstring(GetCurrentProcessId()) +
                           L"-" + std::to_wstring(GetTickCount64()) + L".log";
        errorFile = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (errorFile == INVALID_HANDLE_VALUE)
            errorFile = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, 0, nullptr);
        if (errorFile == INVALID_HANDLE_VALUE) {
            cleanup();
            return false;
        }
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<BYTE> attributes(size);
        auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) {
            cleanup();
            return false;
        }
        HANDLE handles[]{childIn, childOut, errorFile};
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = childIn;
        si.StartupInfo.hStdOutput = childOut;
        si.StartupInfo.hStdError = errorFile;
        si.lpAttributeList = list;
        PROCESS_INFORMATION pi{};
        BOOL ok = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                            sizeof handles, nullptr, nullptr) &&
                  CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                 directory.c_str(), &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(list);
        cleanup();
        if (!ok)
            return false;
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        try {
            reader = std::thread([this]() {
                try {
                    receive();
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex);
                    dead = true;
                    opening = false;
                    success = false;
                    cnr = 0;
                    clearQueue();
                    SetEvent(response);
                    SetEvent(dataReady);
                    logError("stream reader failed");
                }
            });
        } catch (...) {
            return false;
        }
        return true;
    }
    void closeUnlocked() {
        if (input) {
            send(quit, generation);
            closeHandle(input);
        }
        if (process) {
            if (WaitForSingleObject(process, 5000) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 5000);
            }
            closeHandle(process);
        }
        // A child inherits only its explicit handles, so process exit closes
        // stdout.
        if (reader.joinable())
            reader.join();
        closeHandle(output);
        std::lock_guard<std::mutex> lock(mutex);
        opening = false;
        dead = true;
        current = invalidChannel;
        cnr = 0;
        clearQueue();
    }
    bool setChannel(DWORD ch) {
        if (ch >= 50)
            return false;
        std::lock_guard<std::mutex> commandLock(control);
        uint32_t gen;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!opening || dead)
                return false;
            if (ch == current && !overflow)
                return true;
            gen = ++generation;
            current = invalidChannel;
            cnr = 0;
            overflow = false;
            success = false;
            clearQueue();
            ResetEvent(response);
        }
        uint32_t frequency = 473142857 + ch * 6000000;
        if (!send(tune, gen, &frequency, sizeof frequency) ||
            WaitForSingleObject(response, 15000) != WAIT_OBJECT_0) {
            closeUnlocked();
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (!success || dead)
            return false;
        current = ch;
        return true;
    }

  public:
    Bon() {
        for (int i = 0; i < 50; ++i)
            names[i] = L"UHF " + std::to_wstring(i + 13) + L"ch";
    }
    ~Bon() {
        closeUnlocked();
        closeHandle(response);
        closeHandle(dataReady);
    }
    bool openHardware() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (opening && !dead)
                return TRUE;
            dead = false;
            success = false;
            ResetEvent(response);
        }
        if (process)
            closeUnlocked();
        {
            std::lock_guard<std::mutex> lock(mutex);
            dead = false;
            success = false;
        }
        if (!response || !dataReady || !spawn() ||
            WaitForSingleObject(response, 15000) != WAIT_OBJECT_0) {
            closeUnlocked();
            return FALSE;
        }
        bool ok;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ok = success && !dead;
            opening = ok;
        }
        if (!ok)
            closeUnlocked();
        else
            ++hardwareEpoch;
        return ok;
    }
    uint64_t Epoch() const { return hardwareEpoch.load(); }
    const BOOL OpenTuner() override {
        std::lock_guard<std::mutex> commandLock(control);
        if (tunerActive)
            return FALSE;
        bool ok = openHardware();
        tunerActive = ok;
        return ok;
    }
    bool AcquireCard() {
        std::lock_guard<std::mutex> commandLock(control);
        if (!openHardware())
            return false;
        ++cardUsers;
        return true;
    }
    void ReleaseCard() {
        std::lock_guard<std::mutex> commandLock(control);
        if (cardUsers)
            --cardUsers;
        if (!cardUsers && !tunerActive)
            closeUnlocked();
    }
    bool CardTransfer(uint32_t op, const BYTE *data, size_t length, std::vector<BYTE> &result) {
        std::lock_guard<std::mutex> commandLock(control);
        uint32_t gen;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (dead || !opening)
                return false;
            gen = generation;
            success = false;
            cardResponse.clear();
            ResetEvent(response);
        }
        std::vector<BYTE> request(4 + length);
        std::memcpy(request.data(), &op, 4);
        if (length)
            std::memcpy(request.data() + 4, data, length);
        if (!send(cardRequest, gen, request.data(), static_cast<DWORD>(request.size())) ||
            WaitForSingleObject(response, 8000) != WAIT_OBJECT_0) {
            closeUnlocked();
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (dead || !success)
            return false;
        result = cardResponse;
        return result.size() >= 4;
    }
    void CloseTuner() override {
        std::lock_guard<std::mutex> commandLock(control);
        tunerActive = false;
        if (!cardUsers) {
            closeUnlocked();
            return;
        }
        uint32_t gen;
        {
            std::lock_guard<std::mutex> lock(mutex);
            gen = ++generation;
            success = false;
            current = invalidChannel;
            cnr = 0;
            clearQueue();
            ResetEvent(response);
        }
        if (!send(3, gen) || WaitForSingleObject(response, 15000) != WAIT_OBJECT_0)
            closeUnlocked();
    }
    const BOOL SetChannel(const BYTE ch) override {
        return ch >= 13 && ch <= 62 ? setChannel(ch - 13) : FALSE;
    }
    const float GetSignalLevel() override {
        std::lock_guard<std::mutex> lock(mutex);
        return cnr;
    }
    const DWORD WaitTsStream(const DWORD timeout = 0) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (dead || overflow)
                return WAIT_FAILED;
            if (!queue.empty())
                return WAIT_OBJECT_0;
        }
        DWORD result = WaitForSingleObject(dataReady, timeout == INFINITE ? 5000 : timeout);
        std::lock_guard<std::mutex> lock(mutex);
        return dead || overflow ? WAIT_FAILED : result;
    }
    const DWORD GetReadyCount() override {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<DWORD>(queue.size());
    }
    const BOOL GetTsStream(BYTE *dst, DWORD *size, DWORD *remain) override {
        if (!dst || !size)
            return FALSE;
        DWORD unused = 0;
        if (!remain)
            remain = &unused;
        BYTE *p = nullptr;
        BOOL ok = GetTsStream(&p, size, remain);
        if (ok && *size)
            std::memcpy(dst, p, *size);
        return ok;
    }
    const BOOL GetTsStream(BYTE **dst, DWORD *size, DWORD *remain) override {
        if (!dst || !size)
            return FALSE;
        DWORD unused = 0;
        if (!remain)
            remain = &unused;
        std::lock_guard<std::mutex> lock(mutex);
        *dst = nullptr;
        *size = 0;
        *remain = 0;
        if (dead || overflow)
            return FALSE;
        if (queue.empty())
            return TRUE;
        returned = std::move(queue.front());
        queue.pop_front();
        queued -= returned.size();
        *dst = returned.data();
        *size = static_cast<DWORD>(returned.size());
        *remain = static_cast<DWORD>(queue.size());
        if (queue.empty())
            ResetEvent(dataReady);
        return TRUE;
    }
    void PurgeTsStream() override {
        std::lock_guard<std::mutex> commandLock(control);
        uint32_t gen;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!opening || dead)
                return;
            gen = ++generation;
            success = false;
            clearQueue();
            ResetEvent(response);
        }
        if (!send(purge, gen) || WaitForSingleObject(response, 15000) != WAIT_OBJECT_0) {
            closeUnlocked();
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (!success) {
            dead = true;
            cnr = 0;
            SetEvent(dataReady);
        }
    }
    void Release() override { delete this; }
    LPCTSTR GetTunerName() override { return TEXT("Sony CECH-ZD1 J (WinUSB)"); }
    const BOOL IsTunerOpening() override {
        std::lock_guard<std::mutex> lock(mutex);
        return opening && !dead;
    }
    LPCTSTR EnumTuningSpace(const DWORD space) override {
        return space == 0 ? TEXT("地上デジタル") : nullptr;
    }
    LPCTSTR EnumChannelName(const DWORD space, const DWORD ch) override {
        return space == 0 && ch < 50 ? names[ch].c_str() : nullptr;
    }
    const BOOL SetChannel(const DWORD space, const DWORD ch) override {
        return space == 0 ? setChannel(ch) : FALSE;
    }
    const DWORD GetCurSpace() override {
        std::lock_guard<std::mutex> lock(mutex);
        return current == invalidChannel ? invalidChannel : 0;
    }
    const DWORD GetCurChannel() override {
        std::lock_guard<std::mutex> lock(mutex);
        return current;
    }
};

std::mutex backendMutex;
std::weak_ptr<Bon> backend;
std::shared_ptr<Bon> getBackend() {
    std::lock_guard<std::mutex> lock(backendMutex);
    auto b = backend.lock();
    if (!b) {
        b = std::make_shared<Bon>();
        backend = b;
    }
    return b;
}

class Facade final : public IBonDriver2 {
    std::shared_ptr<Bon> b;
    std::atomic_bool opened{false};

  public:
    explicit Facade(std::shared_ptr<Bon> p) : b(std::move(p)) {}
    ~Facade() { CloseTuner(); }
    const BOOL OpenTuner() override {
        if (opened)
            return TRUE;
        bool ok = b->OpenTuner();
        opened = ok;
        return ok;
    }
    void CloseTuner() override {
        if (opened.exchange(false))
            b->CloseTuner();
    }
    const BOOL SetChannel(const BYTE ch) override { return opened && b->SetChannel(ch); }
    const float GetSignalLevel() override { return opened ? b->GetSignalLevel() : 0; }
    const DWORD WaitTsStream(const DWORD t = 0) override {
        return opened ? b->WaitTsStream(t) : WAIT_FAILED;
    }
    const DWORD GetReadyCount() override { return opened ? b->GetReadyCount() : 0; }
    const BOOL GetTsStream(BYTE *p, DWORD *s, DWORD *r) override {
        return opened && b->GetTsStream(p, s, r);
    }
    const BOOL GetTsStream(BYTE **p, DWORD *s, DWORD *r) override {
        return opened && b->GetTsStream(p, s, r);
    }
    void PurgeTsStream() override {
        if (opened)
            b->PurgeTsStream();
    }
    void Release() override { delete this; }
    LPCTSTR GetTunerName() override { return b->GetTunerName(); }
    const BOOL IsTunerOpening() override { return opened && b->IsTunerOpening(); }
    LPCTSTR EnumTuningSpace(const DWORD s) override { return b->EnumTuningSpace(s); }
    LPCTSTR EnumChannelName(const DWORD s, const DWORD c) override {
        return b->EnumChannelName(s, c);
    }
    const BOOL SetChannel(const DWORD s, const DWORD c) override {
        return opened && b->SetChannel(s, c);
    }
    const DWORD GetCurSpace() override { return opened ? b->GetCurSpace() : invalidChannel; }
    const DWORD GetCurChannel() override { return opened ? b->GetCurChannel() : invalidChannel; }
};

#include "card_service.inc"
} // namespace
extern "C" __declspec(dllexport) IBonDriver *CreateBonDriver() {
    try {
        return new Facade(getBackend());
    } catch (...) {
        return nullptr;
    }
}
extern "C" __declspec(dllexport) void *CECHCardOpen() {
    try {
        return openCardConnection();
    } catch (...) {
        return nullptr;
    }
}
extern "C" __declspec(dllexport) void CECHCardClose(void *p) {
    try {
        delete static_cast<CardConnection *>(p);
    } catch (...) {
    }
}
extern "C" __declspec(dllexport) BOOL CECHCardCall(void *p, void *command, DWORD size) {
    if (!p || !command || size != sizeof(px4::card_command::Command))
        return FALSE;
    try {
        return static_cast<CardConnection *>(p)->call(
            *static_cast<px4::card_command::Command *>(command));
    } catch (...) {
        return FALSE;
    }
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        moduleHandle = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
