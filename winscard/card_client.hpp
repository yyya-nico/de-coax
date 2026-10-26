// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <windows.h>
#include "card_command.hpp"
namespace px4 {
// The local adapter shares the exact USB owner already used by BonDriver.
class CardClient final {
    HMODULE module_ = nullptr;
    void *connection_ = nullptr;
    using CloseFn = void (*)(void *);
    using CallFn = BOOL (*)(void *, void *, DWORD);
    CloseFn close_ = nullptr;
    CallFn call_ = nullptr;

  public:
    CardClient() noexcept = default;
    CardClient(const CardClient &) = delete;
    CardClient &operator=(const CardClient &) = delete;
    ~CardClient();
    bool Connect() noexcept;
    bool Call(card_command::Command &) noexcept;
};
} // namespace px4
