// SPDX-License-Identifier: GPL-2.0-only
// Reuse the actual-DLL TS validator while issuing read-only B-CAS initialization.
#define wmain bon_only_main
#include "bon_smoke.cpp"
#undef wmain
#include <winscard.h>
#include <atomic>
#include <thread>

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        std::printf("card_smoke <distribution folder> [seconds=30]\n");
        return 2;
    }
    int seconds = argc > 2 ? _wtoi(argv[2]) : 30;
    if (seconds < 1)
        return 2;
    std::wstring directory = argv[1];
    HMODULE scard = LoadLibraryW((directory + L"/WinSCard.dll").c_str());
    HMODULE bon = LoadLibraryW((directory + L"/BonDriver_CECH.dll").c_str());
    if (!scard || !bon) {
        std::printf("load error=%lu\n", GetLastError());
        return 1;
    }
#define API(name)                                                                                  \
    auto name = reinterpret_cast<decltype(&::name)>(GetProcAddress(scard, #name));                 \
    if (!name)                                                                                     \
    return 1
    API(SCardEstablishContext);
    API(SCardListReadersW);
    API(SCardFreeMemory);
    API(SCardConnectW);
    API(SCardTransmit);
    API(SCardDisconnect);
    API(SCardReleaseContext);
    API(SCardBeginTransaction);
    API(SCardEndTransaction);
    API(SCardStatusW);
#undef API
    SCARDCONTEXT context = 0;
    LONG code = SCardEstablishContext(SCARD_SCOPE_USER, nullptr, nullptr, &context);
    if (code) {
        std::printf("context=%08lx\n", code);
        return 1;
    }
    wchar_t *readers = nullptr;
    DWORD length = SCARD_AUTOALLOCATE;
    code = SCardListReadersW(context, nullptr, reinterpret_cast<wchar_t *>(&readers), &length);
    if (code || !readers) {
        std::printf("list=%08lx\n", code);
        return 1;
    }
    std::wstring selected;
    for (const wchar_t *p = readers; *p; p += wcslen(p) + 1)
        if (wcsstr(p, L"Sony CECH-ZD1 J"))
            selected = p;
    SCardFreeMemory(context, readers);
    if (selected.empty()) {
        std::printf("CECH reader absent\n");
        return 1;
    }
    SCARDHANDLE card = 0;
    DWORD protocol = 0;
    code = SCardConnectW(context, selected.c_str(), SCARD_SHARE_SHARED, SCARD_PROTOCOL_T1, &card,
                         &protocol);
    std::printf("connect=%08lx protocol=%lu\n", code, protocol);
    std::fflush(stdout);
    if (code || protocol != SCARD_PROTOCOL_T1)
        return 1;
    SCARD_IO_REQUEST pci{SCARD_PROTOCOL_T1, sizeof(SCARD_IO_REQUEST)};
    const BYTE apdu[] = {0x90, 0x30, 0, 0, 0};
    auto transmit = [&]() {
        BYTE reply[512];
        DWORD n = sizeof reply;
        LONG e = SCardTransmit(card, &pci, apdu, sizeof apdu, nullptr, reply, &n);
        return e == SCARD_S_SUCCESS && n >= 2 && reply[n - 2] == 0x90 && reply[n - 1] == 0;
    };
    if (!transmit()) {
        std::printf("card-only APDU failed\n");
        return 1;
    }
    auto create = reinterpret_cast<IBonDriver *(*)()>(GetProcAddress(bon, "CreateBonDriver"));
    auto *b = create ? static_cast<IBonDriver2 *>(create()) : nullptr;
    if (!b || !b->OpenTuner() || !b->SetChannel(0, 7)) {
        std::printf("tuner open/tune failed\n");
        return 1;
    }
    SCARDHANDLE other = 0;
    DWORD otherProtocol = 0;
    code = SCardConnectW(context, selected.c_str(), SCARD_SHARE_EXCLUSIVE, SCARD_PROTOCOL_T1,
                         &other, &otherProtocol);
    bool sharing = code == SCARD_E_SHARING_VIOLATION;
    if (code == SCARD_S_SUCCESS)
        SCardDisconnect(other, SCARD_LEAVE_CARD);
    std::atomic_bool stop{false}, cardOK{true};
    std::atomic_uint calls{0};
    std::thread worker([&] {
        while (!stop) {
            LONG begin = SCardBeginTransaction(card);
            bool ok = begin == SCARD_S_SUCCESS && transmit();
            LONG end =
                begin == SCARD_S_SUCCESS ? SCardEndTransaction(card, SCARD_LEAVE_CARD) : begin;
            if (!ok || end != SCARD_S_SUCCESS) {
                cardOK = false;
                break;
            }
            ++calls;
            Sleep(50);
        }
    });
    bool tsOK = capture(b, seconds * 1000);
    stop = true;
    worker.join();
    b->PurgeTsStream();
    bool afterPurge = transmit() && capture(b, 2000);
    b->CloseTuner();
    bool afterClose = transmit();
    code = SCardDisconnect(card, SCARD_LEAVE_CARD);
    LONG release = SCardReleaseContext(context);
    bool ok = tsOK && cardOK && calls > 0 && sharing && afterPurge && afterClose &&
              code == SCARD_S_SUCCESS && release == SCARD_S_SUCCESS;
    b->Release();
    std::printf("APDU_calls=%u card_ok=%d sharing=%d purge=%d after_tuner_close=%d result=%s\n",
                calls.load(), cardOK.load(), sharing, afterPurge, afterClose, ok ? "PASS" : "FAIL");
    FreeLibrary(bon);
    FreeLibrary(scard);
    return ok ? 0 : 1;
}
