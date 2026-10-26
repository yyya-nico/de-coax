// SPDX-License-Identifier: GPL-2.0-only
#include "card_client.hpp"
#include <filesystem>
#include <vector>
#include <array>
namespace px4 {
namespace {
void moduleAnchor() {}
} // namespace
CardClient::~CardClient() {
    if (connection_ && close_)
        close_(connection_);
    if (module_)
        FreeLibrary(module_);
}
bool CardClient::Connect() noexcept try {
    if (connection_)
        return true;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleAnchor), &self))
        return false;
    std::vector<wchar_t> path(32768);
    DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size())
        return false;
    auto directory = std::filesystem::path(std::wstring(path.data(), n)).parent_path();
    std::array<std::filesystem::path, 2> candidates{
        directory / L"BonDriver_CECH.dll", directory / L"BonDriver" / L"BonDriver_CECH.dll"};
    for (const auto &p : candidates) {
        module_ = LoadLibraryExW(p.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module_)
            break;
    }
    if (!module_)
        return false;
    auto open = reinterpret_cast<void *(*)()>(GetProcAddress(module_, "CECHCardOpen"));
    close_ = reinterpret_cast<CloseFn>(GetProcAddress(module_, "CECHCardClose"));
    call_ = reinterpret_cast<CallFn>(GetProcAddress(module_, "CECHCardCall"));
    if (!open || !close_ || !call_)
        return false;
    connection_ = open();
    if (!connection_)
        return false;
    card_command::Command c{};
    c.code = card_command::Code::GET_VERSION;
    return Call(c) && c.version == card_command::VERSION;
} catch (...) {
    return false;
}
bool CardClient::Call(card_command::Command &c) noexcept {
    if (!connection_ || !call_)
        return false;
    c.status = card_command::Status::REQUEST;
    c.error = card_command::Error::NONE;
    return call_(connection_, &c, sizeof c) != FALSE;
}
} // namespace px4
