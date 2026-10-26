// SPDX-License-Identifier: GPL-2.0-only
// Included after Bon, inside its anonymous namespace. State is per USB owner.
namespace cc = px4::card_command;
constexpr wchar_t cardReaderName[] = L"Sony CECH-ZD1 J Smart Card Reader";
struct CardConnection;
struct CardService {
    std::shared_ptr<Bon> device;
    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_set<CardConnection *> connected;
    CardConnection *transaction = nullptr;
    unsigned depth = 0;
    uint64_t epoch = 0;
    explicit CardService(std::shared_ptr<Bon> b) : device(std::move(b)) {}
};
std::mutex serviceMutex;
std::weak_ptr<CardService> cardService;
struct CardConnection {
    std::shared_ptr<CardService> service;
    bool connected = false;
    bool removed = false;
    cc::ShareMode mode = cc::ShareMode::UNDEFINED;
    explicit CardConnection(std::shared_ptr<CardService> s) : service(std::move(s)) {}
    ~CardConnection() {
        {
            std::lock_guard<std::mutex> lock(service->mutex);
            disconnect();
        }
        service->device->ReleaseCard();
    }
    void disconnect() {
        service->connected.erase(this);
        connected = false;
        if (service->transaction == this) {
            service->transaction = nullptr;
            service->depth = 0;
            service->changed.notify_all();
        }
    }
    bool compatible(cc::ShareMode requested) {
        if (requested != cc::ShareMode::SHARED && requested != cc::ShareMode::EXCLUSIVE)
            return false;
        for (auto *c : service->connected)
            if (c != this &&
                (requested == cc::ShareMode::EXCLUSIVE || c->mode == cc::ShareMode::EXCLUSIVE))
                return false;
        return true;
    }
    cc::Error transfer(uint32_t operation, const BYTE *data, size_t size,
                       std::vector<BYTE> &result) {
        if (!service->device->CardTransfer(operation, data, size, result))
            return cc::Error::NO_READER;
        uint32_t code = 0;
        std::memcpy(&code, result.data(), 4);
        result.erase(result.begin(), result.begin() + 4);
        switch (code) {
        case 0:
            return cc::Error::NONE;
        case 1:
            return connected ? cc::Error::REMOVED : cc::Error::NO_CARD;
        case 2:
            return cc::Error::TIMEOUT;
        case 3:
            return cc::Error::NO_READER;
        default:
            return cc::Error::PROTOCOL;
        }
    }
    cc::Error status(cc::Command &c) {
        std::vector<BYTE> p;
        auto e = transfer(0, nullptr, 0, p);
        if (e != cc::Error::NONE)
            return e;
        if (p.size() < 2 || p.size() > 2 + cc::MAX_ATR_SIZE)
            return cc::Error::PROTOCOL;
        c.card_present = p[0] != 0;
        c.card_initialized = p[1] != 0;
        c.atr_length = static_cast<uint32_t>(p.size() - 2);
        std::memcpy(c.atr, p.data() + 2, c.atr_length);
        return cc::Error::NONE;
    }
    cc::Error reset(cc::Command &c) {
        std::vector<BYTE> p;
        auto e = transfer(1, nullptr, 0, p);
        if (e != cc::Error::NONE)
            return e;
        if (p.empty() || p.size() > cc::MAX_ATR_SIZE)
            return cc::Error::PROTOCOL;
        c.card_present = c.card_initialized = true;
        c.atr_length = static_cast<uint32_t>(p.size());
        std::memcpy(c.atr, p.data(), p.size());
        return cc::Error::NONE;
    }
    bool call(cc::Command &c) {
        std::unique_lock<std::mutex> lock(service->mutex);
        if (service->epoch != service->device->Epoch()) {
            for (auto *other : service->connected) {
                other->connected = false;
                other->removed = true;
            }
            service->connected.clear();
            service->transaction = nullptr;
            service->depth = 0;
            service->epoch = service->device->Epoch();
            service->changed.notify_all();
        }
        c.status = cc::Status::FAILED;
        c.error = cc::Error::NONE;
        auto fail = [&](cc::Error e) {
            c.error = e;
            return true;
        };
        auto finish = [&](cc::Error e) {
            c.error = e;
            c.status = e == cc::Error::NONE ? cc::Status::SUCCEEDED : cc::Status::FAILED;
            return true;
        };
        switch (c.code) {
        case cc::Code::GET_VERSION:
            c.version = cc::VERSION;
            break;
        case cc::Code::LIST_READERS:
            c.reader_count = service->device->IsTunerOpening() ? 1 : 0;
            c.reader_generation = c.reader_count ? 1 : 2;
            if (c.reader_count)
                wcscpy_s(c.readers[0], cardReaderName);
            break;
        case cc::Code::STATUS:
            if (removed)
                return fail(cc::Error::REMOVED);
            if (!wmemchr(c.reader_name, 0, cc::MAX_READER_NAME) ||
                wcscmp(c.reader_name, cardReaderName))
                return fail(cc::Error::NO_READER);
            return finish(status(c));
        case cc::Code::CONNECT: {
            if (connected)
                return fail(cc::Error::INVALID_PARAMETER);
            if (!wmemchr(c.reader_name, 0, cc::MAX_READER_NAME) ||
                wcscmp(c.reader_name, cardReaderName))
                return fail(cc::Error::NO_READER);
            auto requested = static_cast<cc::ShareMode>(c.share_mode);
            if (!compatible(requested))
                return fail(cc::Error::BUSY);
            auto e = status(c);
            if (e != cc::Error::NONE)
                return fail(e);
            if (!c.card_present)
                return fail(cc::Error::NO_CARD);
            if (!c.card_initialized) {
                e = reset(c);
                if (e != cc::Error::NONE)
                    return fail(e);
            }
            connected = true;
            removed = false;
            mode = requested;
            service->connected.insert(this);
            break;
        }
        case cc::Code::RECONNECT: {
            if (!connected)
                return fail(cc::Error::INVALID_PARAMETER);
            auto requested = static_cast<cc::ShareMode>(c.share_mode);
            if (!compatible(requested))
                return fail(cc::Error::BUSY);
            mode = requested;
            break;
        }
        case cc::Code::DISCONNECT:
            disconnect();
            break;
        case cc::Code::BEGIN_TRANSACTION:
        case cc::Code::END_TRANSACTION:
        case cc::Code::RESET:
        case cc::Code::TRANSMIT: {
            if (!connected)
                return fail(cc::Error::INVALID_PARAMETER);
            if (!service->changed.wait_for(lock, std::chrono::seconds(3), [&] {
                    return !service->transaction || service->transaction == this;
                }))
                return fail(cc::Error::BUSY);
            if (c.code == cc::Code::BEGIN_TRANSACTION) {
                service->transaction = this;
                ++service->depth;
                break;
            }
            if (c.code == cc::Code::END_TRANSACTION) {
                if (service->transaction != this)
                    return fail(cc::Error::INVALID_PARAMETER);
                auto e = cc::Error::NONE;
                if (c.disposition == static_cast<uint32_t>(cc::Disposition::RESET))
                    e = reset(c);
                else
                    e = status(c);
                if (--service->depth == 0) {
                    service->transaction = nullptr;
                    service->changed.notify_all();
                }
                return finish(e);
            }
            if (c.code == cc::Code::RESET)
                return finish(reset(c));
            if (!c.data_length || c.data_length > cc::MAX_DATA_SIZE ||
                c.data_capacity > cc::MAX_DATA_SIZE)
                return fail(cc::Error::INVALID_PARAMETER);
            std::vector<BYTE> p;
            auto e = transfer(2, c.data, c.data_length, p);
            if (e != cc::Error::NONE)
                return fail(e);
            c.data_length = static_cast<uint32_t>(p.size());
            if (p.size() > c.data_capacity)
                return fail(cc::Error::INSUFFICIENT_BUFFER);
            std::memcpy(c.data, p.data(), p.size());
            break;
        }
        default:
            return fail(cc::Error::INVALID_PARAMETER);
        }
        c.status = cc::Status::SUCCEEDED;
        return true;
    }
};
void *openCardConnection() {
    std::shared_ptr<CardService> s;
    {
        std::lock_guard<std::mutex> lock(serviceMutex);
        s = cardService.lock();
        if (!s) {
            s = std::make_shared<CardService>(getBackend());
            cardService = s;
        }
    }
    if (!s->device->AcquireCard())
        return nullptr;
    try {
        return new CardConnection(s);
    } catch (...) {
        s->device->ReleaseCard();
        throw;
    }
}
