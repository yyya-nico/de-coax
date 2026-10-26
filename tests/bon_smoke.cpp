// SPDX-License-Identifier: GPL-2.0-only
// Loads the actual DLL and exercises the public ABI, real child host and USB.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../bon/IBonDriver2.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
static bool capture(IBonDriver2 *b, int milliseconds) {
    struct PID {
        bool valid = false, duplicate = false;
        BYTE cc = 0, last[188]{};
    };
    std::vector<PID> pids(8192);
    ULONGLONG end = GetTickCount64() + milliseconds, bytes = 0;
    unsigned bad = 0, tei = 0, ccErrors = 0, tsid = 0xffffffff;
    bool pat = false, mixed = false;
    while (GetTickCount64() < end) {
        DWORD wait = b->WaitTsStream(500);
        if (wait == WAIT_FAILED) {
            std::printf("WAIT_FAILED\n");
            return false;
        }
        if (wait == WAIT_TIMEOUT)
            continue;
        BYTE *data = nullptr;
        DWORD size = 0, remain = 0;
        if (!b->GetTsStream(&data, &size, &remain))
            return false;
        if (size % 188)
            return false;
        for (DWORD i = 0; i < size; i += 188) {
            BYTE *p = data + i;
            if (p[0] != 0x47) {
                ++bad;
                continue;
            }
            unsigned pid = ((p[1] & 31) << 8) | p[2];
            if (pid == 8191)
                continue;
            auto &state = pids[pid];
            if (p[1] & 0x80) {
                ++tei;
                state.valid = false;
                continue;
            }
            BYTE afc = (p[3] >> 4) & 3, cc = p[3] & 15;
            unsigned payloadOffset = 4;
            if (!afc) {
                ++bad;
                state.valid = false;
                continue;
            }
            if (afc & 2) {
                unsigned n = p[4];
                if (n > 183 || ((afc & 1) && n > 182) || (!(afc & 1) && n != 183)) {
                    ++bad;
                    state.valid = false;
                    continue;
                }
                payloadOffset = 5 + n;
                if (n && p[5] & 0x80)
                    state.valid = false;
            }
            if (state.valid) {
                BYTE expected = state.cc;
                if (afc & 1)
                    expected = (expected + 1) & 15;
                if ((afc & 1) && cc == state.cc && !state.duplicate &&
                    !std::memcmp(p, state.last, 188)) {
                    state.duplicate = true;
                    continue;
                }
                if (cc != expected)
                    ++ccErrors;
            }
            state.valid = true;
            state.duplicate = false;
            state.cc = cc;
            std::memcpy(state.last, p, 188);
            if (pid == 0 && (afc & 1)) {
                pat = true;
                if ((p[1] & 0x40) && payloadOffset < 188) {
                    unsigned begin = payloadOffset + 1 + p[payloadOffset];
                    if (begin + 8 <= 188 && p[begin] == 0) {
                        unsigned id = (p[begin + 3] << 8) | p[begin + 4];
                        if (tsid == 0xffffffff)
                            tsid = id;
                        else if (tsid != id)
                            mixed = true;
                    }
                }
            }
        }
        bytes += size;
    }
    std::printf("capture bytes=%llu bad_sync=%u TEI=%u CC=%u PAT=%d TSID=%u "
                "mixed=%d CNR=%.2f\n",
                bytes, bad, tei, ccErrors, pat, tsid, mixed, b->GetSignalLevel());
    std::fflush(stdout);
    return bytes > 0 && bad == 0 && tei == 0 && ccErrors == 0 && pat && !mixed;
}
int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        std::printf("bon_smoke <absolute DLL path> [seconds=10] [physical channel=20]\n");
        return 2;
    }
    int seconds = argc > 2 ? _wtoi(argv[2]) : 10, ch = argc > 3 ? _wtoi(argv[3]) : 20;
    if (seconds < 1 || ch < 13 || ch > 62)
        return 2;
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) {
        std::printf("LoadLibrary error=%lu\n", GetLastError());
        return 1;
    }
    auto create = reinterpret_cast<IBonDriver *(*)()>(GetProcAddress(dll, "CreateBonDriver"));
    if (!create)
        return 1;
    auto *b = static_cast<IBonDriver2 *>(create());
    if (!b)
        return 1;
    bool ok = b->EnumChannelName(0, 0) && b->EnumChannelName(0, 49) && !b->EnumChannelName(0, 50) &&
              !b->EnumTuningSpace(1);
    ULONGLONG start = GetTickCount64();
    ok = ok && b->OpenTuner();
    std::printf("open=%d elapsed_ms=%llu\n", ok, GetTickCount64() - start);
    std::fflush(stdout);
    if (ok) {
        start = GetTickCount64();
        ok = b->SetChannel(0, static_cast<DWORD>(ch - 13));
        std::printf("tune=%d elapsed_ms=%llu\n", ok, GetTickCount64() - start);
        std::fflush(stdout);
    }
    if (ok) {
        BYTE *buffer = nullptr;
        DWORD n = 0;
        ok = b->GetTsStream(&buffer, &n, nullptr) != FALSE;
    }
    if (ok)
        ok = b->GetCurSpace() == 0 && b->GetCurChannel() == static_cast<DWORD>(ch - 13) &&
             capture(b, seconds * 1000);
    if (ok) {
        start = GetTickCount64();
        b->PurgeTsStream();
        std::printf("purge_ms=%llu\n", GetTickCount64() - start);
        ok = capture(b, 2000);
    }
    // An invalid request must leave the last valid channel intact.
    if (ok)
        ok = !b->SetChannel(1, 7) && !b->SetChannel(0, 50) &&
             b->GetCurChannel() == static_cast<DWORD>(ch - 13);
    // This fixture's RF 13 was off air during development. Opt into its test.
    if (ok && argc > 4) {
        start = GetTickCount64();
        ok = !b->SetChannel(0, 0);
        std::printf("off_air_failed=%d elapsed_ms=%llu\n", ok, GetTickCount64() - start);
        ok = ok && b->GetCurChannel() == 0xffffffff && b->GetReadyCount() == 0;
        for (int i = 0; ok && i < 3; ++i) {
            ok = b->SetChannel(0, 5) && capture(b, 1000) &&
                 b->SetChannel(0, static_cast<DWORD>(ch - 13)) && capture(b, 1000);
        }
    }
    b->CloseTuner();
    ok = ok && !b->IsTunerOpening();
    if (ok) {
        ok = b->OpenTuner() && static_cast<IBonDriver *>(b)->SetChannel(static_cast<BYTE>(ch)) &&
             capture(b, 2000);
    }
    b->Release();
    FreeLibrary(dll);
    std::printf("result=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
