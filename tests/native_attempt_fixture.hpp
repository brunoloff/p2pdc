#pragma once

#include "../src/discovery/incoming_offer_resolver.hpp"
#include "../src/transport/native_attempt.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

using namespace p2pdc;
using namespace p2pdc::detail;
using namespace std::chrono_literals;

namespace {
class Loop {
  public:
    Executor executor() {
        return [this](std::function<void()> task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        };
    }
    bool until(const std::function<bool()> &done) {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (done())
                return true;
            std::this_thread::sleep_for(2ms);
        }
        drain();
        return done();
    }

  private:
    void drain() {
        for (int count = 0; count < 1000; ++count) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex_);
                if (tasks_.empty())
                    return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

struct Peer {
    std::shared_ptr<Identity> identity;
    std::shared_ptr<RecipientKeyPair> box;
    SignalingRecipient recipient;
    std::array<std::byte, 20> address;
    std::shared_ptr<Bytes> capability = std::make_shared<Bytes>(32, std::byte{8});
    std::unique_ptr<NativeAttempt> attempt;
    std::vector<Bytes> messages;
    std::vector<RouteInfo> routes;
    int ready = 0;
    int failed = 0;
    int writable = 0;
    explicit Peer(std::byte seed)
        : identity(std::make_shared<Identity>(
              std::get<Identity>(Identity::import_seed(Bytes(32, seed))))),
          box(std::make_shared<RecipientKeyPair>(
              std::get<RecipientKeyPair>(RecipientKeyPair::import_secret_key(Bytes(32, seed))))),
          recipient{identity->public_key(), {seed}}, address{seed} {}
    NativeAttemptCallbacks callbacks() {
        return {[this] { ++ready; }, [this](Bytes value) { messages.push_back(std::move(value)); },
                [this] { ++writable; }, [this](RouteInfo route) { routes.push_back(route); },
                [this] { ++failed; }};
    }
    NativeAttemptConfig config(Peer &remote, std::shared_ptr<dht::DhtRunner> runner,
                               HandshakeRole role) {
        MemoryBudget budget(4194304);
        return {NativeStore(std::move(runner), {32768, 128, 1048576}),
                address,
                remote.address,
                {recipient, remote.recipient, box->public_key(), remote.box->public_key(),
                 AttemptId{std::byte{7}}, role},
                identity,
                box,
                capability,
                {},
                budget,
                {{524288, 128, 32768}, {524288, 64, 32768}},
                {{4096, 1024}, 4096, 8, 4096, 4},
                {budget, 8192, 64, 1024},
                {4096, 256, 4361},
                65536,
                64,
                1024,
                4096,
                {100, 60, 5, 32768},
                50,
                [] { return std::uint64_t{100}; }};
    }
};

class NativeAttemptTest : public ::testing::Test {
  protected:
    Loop loop;
    std::shared_ptr<dht::DhtRunner> first = std::make_shared<dht::DhtRunner>();
    std::shared_ptr<dht::DhtRunner> second = std::make_shared<dht::DhtRunner>();
    void SetUp() override {
        dht::DhtRunner::Config config;
        config.dht_config.node_config.network = 0x12345683;
        config.peer_discovery = false;
        config.peer_publish = false;
        first->run("127.0.0.1", "::1", "0", config);
        second->run("127.0.0.1", "::1", "0", config);
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        second->bootstrap(first->getBound(), [done](bool success) { done->set_value(success); });
        ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
        ASSERT_TRUE(future.get());
    }
    void TearDown() override {
        first->join();
        second->join();
    }
    Advertisement verified_advertisement(const Peer &peer) {
        AdvertisementFields fields{
            peer.box->public_key(), peer.recipient.instance, peer.address, 1, 0, 100, 150};
        auto encoded = encode_advertisement(*peer.identity, fields, *peer.capability, {100, 60, 5});
        return std::get<Advertisement>(
            decode_advertisement(std::get<Bytes>(encoded), *peer.capability, {100, 60, 5}));
    }
    std::unique_ptr<NativeMailbox> listen(Peer &peer, std::vector<Bytes> &records, bool &failed) {
        MailboxCallbacks callbacks{[&](Bytes value) { records.push_back(std::move(value)); },
                                   [&](MailboxError, std::optional<StoreError>) { failed = true; },
                                   [&](bool ok) { failed = failed || !ok; }};
        auto result = NativeMailbox::create(NativeStore(second, {32768, 128, 1048576}),
                                            peer.address, {524288, 128, 32768},
                                            MemoryBudget(524288), loop.executor(), callbacks);
        return std::move(std::get<std::unique_ptr<NativeMailbox>>(result));
    }
    std::optional<IncomingOffer> wait_for_offer(Peer &sender, Peer &receiver,
                                                IncomingOfferResolver &resolver,
                                                std::span<const Advertisement> peers,
                                                const std::vector<Bytes> &records,
                                                const bool &mailbox_failed) {
        std::optional<IncomingOffer> offer;
        std::size_t consumed = 0;
        const bool found = loop.until([&] {
            (void)sender.attempt->poll_start();
            while (consumed < records.size() && !offer) {
                auto result =
                    resolver.resolve(records[consumed++], *receiver.box, receiver.recipient,
                                     *receiver.capability, peers, 100);
                if (auto *value = std::get_if<IncomingOffer>(&result)) {
                    offer = std::move(*value);
                } else if (auto *error = std::get_if<IncomingOfferError>(&result);
                           !error || *error != IncomingOfferError::NotOffer) {
                    ADD_FAILURE() << "unexpected incoming signaling error";
                }
            }
            return offer.has_value() || sender.failed || mailbox_failed;
        });
        EXPECT_TRUE(found);
        return offer;
    }
    NativeAttemptConfig responder_config(Peer &local, const IncomingOffer &offer) {
        MemoryBudget budget(4194304);
        const auto &peer = offer.peer;
        return {NativeStore(second, {32768, 128, 1048576}),
                local.address,
                peer.fields.signaling_inbox,
                {local.recipient,
                 {peer.signing_public_key, peer.fields.instance_id},
                 local.box->public_key(),
                 peer.fields.recipient_public_key,
                 offer.envelope.fields.attempt_id,
                 HandshakeRole::Answerer},
                local.identity,
                local.box,
                local.capability,
                {},
                budget,
                {{524288, 128, 32768}, {524288, 64, 32768}},
                {{4096, 1024}, 4096, 8, 4096, 4},
                {budget, 8192, 64, 1024},
                {4096, 256, 4361},
                65536,
                64,
                1024,
                4096,
                {100, 60, 5, 32768},
                50,
                [] { return std::uint64_t{100}; }};
    }
    bool publish_conflicting_offer(const Peer &sender, const Peer &recipient,
                                   const IncomingOffer &offer) {
        auto fields = offer.envelope.fields;
        ++fields.sequence;
        fields.payload = Bytes{std::byte{1}}; // Deliberately invalid SDP.
        auto encoded = encode_signaling(*sender.identity, fields, recipient.box->public_key(),
                                        *sender.capability, {100, 60, 5, 32768});
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        NativeStore store(first, {32768, 128, 1048576});
        if (store.put(recipient.address, std::get<Bytes>(encoded),
                      [done](bool ok) { done->set_value(ok); }))
            return false;
        return future.wait_for(10s) == std::future_status::ready && future.get();
    }
    Bytes signed_seed(const Peer &sender, const Peer &recipient, SignalingKind kind,
                      AttemptId attempt = {std::byte{7}}) {
        SignalingFields fields{
            kind, sender.recipient.instance, recipient.recipient, attempt, 0, 100,
            150,  Bytes{std::byte{1}}};
        return std::get<Bytes>(encode_signaling(*sender.identity, fields,
                                                recipient.box->public_key(), *sender.capability,
                                                {100, 60, 5, 32768}));
    }
};
} // namespace
