// Derived from px4_drv WinSCard_PX4; see docs/design.md.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <windows.h>

namespace px4::winscard {

/**
 * System32 にある Windows 標準 WinSCard.dll の関数を取得する
 * @param name 取得するエクスポート名
 * @returns 関数のアドレス (DLL を読み込めない場合や未実装の場合は nullptr)
 */
FARPROC GetNativeFunction(const char *name) noexcept;

template <typename Function>
Function GetNativeFunction(const char *name) noexcept
{
	return reinterpret_cast<Function>(GetNativeFunction(name));
}

} // namespace px4::winscard
