#include <nice/agent.h>

#include <cstdlib>
#include <iostream>

namespace {
struct Probe {
    GMainLoop *loop;
    NiceAgent *agent;
    guint stream;
    bool complete = false;
    unsigned relays = 0;
};

void free_candidate(gpointer candidate) {
    nice_candidate_free(static_cast<NiceCandidate *>(candidate));
}

unsigned relay_count(NiceAgent *agent, guint stream) {
    unsigned relays = 0;
    GSList *candidates = nice_agent_get_local_candidates(agent, stream, 1);
    for (auto *item = candidates; item; item = item->next) {
        const auto *candidate = static_cast<const NiceCandidate *>(item->data);
        if (candidate->type == NICE_CANDIDATE_TYPE_RELAYED)
            ++relays;
    }
    g_slist_free_full(candidates, free_candidate);
    return relays;
}

void gathered(NiceAgent *, guint, gpointer user) {
    auto &probe = *static_cast<Probe *>(user);
    probe.complete = true;
    g_main_loop_quit(probe.loop);
}

gboolean timeout(gpointer user) {
    g_main_loop_quit(static_cast<Probe *>(user)->loop);
    return G_SOURCE_REMOVE;
}

void receive(NiceAgent *, guint, guint, guint, gchar *, gpointer) {}
} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: native_turn_tls_probe HOST expect-relay|expect-none\n";
        return 2;
    }
    const std::string expected = argv[2];
    if (expected != "expect-relay" && expected != "expect-none")
        return 2;
    auto *loop = g_main_loop_new(nullptr, FALSE);
    auto *agent = nice_agent_new(g_main_loop_get_context(loop), NICE_COMPATIBILITY_RFC5245);
    g_object_set(agent, "ice-tcp", FALSE, "force-relay", TRUE, nullptr);
    Probe probe{loop, agent, nice_agent_add_stream(agent, 1)};
    NiceAddress address;
    nice_address_init(&address);
    const char *user = std::getenv("P2PDC_TEST_TURN_USER");
    const char *password = std::getenv("P2PDC_TEST_TURN_PASSWORD");
    if (!user || !password || !nice_address_set_from_string(&address, "127.0.0.1") ||
        !nice_agent_add_local_address(agent, &address) ||
        !nice_agent_set_relay_info(agent, probe.stream, 1, argv[1], 53490, user, password,
                                   NICE_RELAY_TYPE_TURN_TLS))
        return 2;
    g_signal_connect(agent, "candidate-gathering-done", G_CALLBACK(gathered), &probe);
    if (!nice_agent_attach_recv(agent, probe.stream, 1, g_main_loop_get_context(loop), receive,
                                nullptr))
        return 2;
    const auto timer = g_timeout_add_seconds(8, timeout, &probe);
    if (!nice_agent_gather_candidates(agent, probe.stream))
        return 2;
    g_main_loop_run(loop);
    if (probe.complete)
        g_source_remove(timer);
    probe.relays = relay_count(agent, probe.stream);
    std::cout << "gathered=" << probe.complete << " relays=" << probe.relays << '\n';
    g_object_unref(agent);
    g_main_loop_unref(loop);
    return (expected == "expect-relay" ? probe.complete && probe.relays > 0 : probe.relays == 0)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
