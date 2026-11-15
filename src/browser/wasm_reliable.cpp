#include "../protocol/reliable_channel.hpp"

#include <emscripten.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

using namespace p2pdc;
using namespace p2pdc::detail;

EM_JS(int, bridge_send, (unsigned id, const std::byte *data, unsigned size), {
    try {
        return Module.p2pdcReliableBridge.send(id, HEAPU8.slice(data, data + size));
    } catch (error) {
        console.error("p2pdc: browser frame submission failed");
        return 2;
    }
});
EM_JS(unsigned, bridge_buffered, (unsigned id), {
    try {
        return Module.p2pdcReliableBridge.buffered(id);
    } catch (error) {
        console.error("p2pdc: browser buffer query failed");
        return 0xffffffff;
    }
});
EM_JS(int, bridge_message, (unsigned id, const std::byte *data, unsigned size), {
    try {
        return Module.p2pdcReliableBridge.message(id, HEAPU8.slice(data, data + size));
    } catch (error) {
        console.error("p2pdc: browser message delivery failed");
        return 0;
    }
});
EM_JS(void, bridge_failure, (unsigned id, unsigned error), {
    try {
        Module.p2pdcReliableBridge.failure(id, error);
    } catch (exception) {
        console.error("p2pdc: browser failure delivery failed");
    }
});
EM_JS(void, bridge_writable, (unsigned id), {
    try {
        Module.p2pdcReliableBridge.writable(id);
    } catch (error) {
        console.error("p2pdc: browser writable delivery failed");
    }
});

namespace {
class WasmLink final : public ReliableSink, public std::enable_shared_from_this<WasmLink> {
  public:
    explicit WasmLink(unsigned value) : id(value) {}
    void initialize();
    [[nodiscard]] std::size_t buffered_amount() const override { return bridge_buffered(id); }
    [[nodiscard]] SinkResult send(ByteView frame) override;
    void close();
    void fail(ChannelFailure error);
    unsigned id;
    bool active = true;
    std::unique_ptr<ReliableChannel> channel;
};

void post(std::function<void()> callback) {
    auto task = std::make_unique<std::function<void()>>(std::move(callback));
    emscripten_async_call(
        [](void *data) {
            std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()> *>(data));
            (*task)();
        },
        task.release(), 0);
}

void WasmLink::initialize() {
    std::weak_ptr<WasmLink> weak = shared_from_this();
    ReliableChannelCallbacks callbacks{
        [weak](Bytes value) {
            if (auto self = weak.lock();
                self && self->active && !bridge_message(self->id, value.data(), value.size()))
                self->fail(ChannelFailure::Allocation);
        },
        [weak](ChannelFailure error) {
            if (auto self = weak.lock())
                self->fail(error);
        },
        [weak] {
            if (auto self = weak.lock(); self && self->active)
                bridge_writable(self->id);
        }};
    channel = std::make_unique<ReliableChannel>(
        ReliableChannelLimits{{262145, 1024}, 2097152, 32, 524288, 4}, MemoryBudget(4194304), *this,
        [weak](std::function<void()> task) {
            try {
                post([weak, task = std::move(task)] {
                    try {
                        task();
                    } catch (...) {
                        if (auto self = weak.lock())
                            self->fail(ChannelFailure::Allocation);
                    }
                });
            } catch (...) {
                if (auto self = weak.lock())
                    self->fail(ChannelFailure::Allocation);
            }
        },
        std::move(callbacks));
}

SinkResult WasmLink::send(ByteView frame) {
    if (!active)
        return SinkResult::Failed;
    switch (bridge_send(id, frame.data(), frame.size())) {
    case 0:
        return SinkResult::Accepted;
    case 1:
        return SinkResult::NotReady;
    default:
        return SinkResult::Failed;
    }
}
void WasmLink::fail(ChannelFailure error) {
    if (!active)
        return;
    active = false;
    if (channel)
        channel->close();
    bridge_failure(id, static_cast<unsigned>(error));
}
void WasmLink::close() {
    active = false;
    if (channel)
        channel->close();
}
std::unordered_map<unsigned, std::shared_ptr<WasmLink>> links;
unsigned next_id = 1;
std::shared_ptr<WasmLink> find(unsigned id) {
    auto entry = links.find(id);
    return entry == links.end() ? nullptr : entry->second;
}
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE unsigned p2pdc_reliable_create() {
    if (links.size() >= 16 || next_id == 0)
        return 0;
    try {
        auto link = std::make_shared<WasmLink>(next_id++);
        link->initialize();
        links.emplace(link->id, link);
        return link->id;
    } catch (...) {
        return 0;
    }
}
EMSCRIPTEN_KEEPALIVE void p2pdc_reliable_destroy(unsigned id) {
    auto link = find(id);
    if (link) {
        link->close();
        links.erase(id);
    }
}
EMSCRIPTEN_KEEPALIVE void p2pdc_reliable_ready(unsigned id, int ready) {
    if (auto link = find(id); link && link->active)
        try {
            link->channel->set_ready(ready != 0);
        } catch (...) {
            link->fail(ChannelFailure::Allocation);
        }
}
EMSCRIPTEN_KEEPALIVE void p2pdc_reliable_wake(unsigned id) {
    if (auto link = find(id); link && link->active)
        try {
            link->channel->wake();
        } catch (...) {
            link->fail(ChannelFailure::Allocation);
        }
}
EMSCRIPTEN_KEEPALIVE int p2pdc_reliable_send(unsigned id, const std::byte *data, unsigned size) {
    auto link = find(id);
    if (!link || !link->active)
        return static_cast<int>(ChannelSend::Closed);
    if (!data && size)
        return static_cast<int>(ChannelSend::Failed);
    try {
        const auto result = link->channel->send(ByteView(data, size));
        return static_cast<int>(link->active ? result : ChannelSend::Failed);
    } catch (...) {
        link->fail(ChannelFailure::Allocation);
        return static_cast<int>(ChannelSend::Failed);
    }
}
EMSCRIPTEN_KEEPALIVE void p2pdc_reliable_frame(unsigned id, const std::byte *data, unsigned size) {
    auto link = find(id);
    if (link && link->active && (data || !size))
        try {
            link->channel->on_frame(ByteView(data, size));
        } catch (...) {
            link->fail(ChannelFailure::Allocation);
        }
}
}
