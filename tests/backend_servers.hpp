#pragma once

#include <nlohmann/json.hpp>

#include <csignal>
#include <cstdio>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace p2pdc::test {

// The JavaScript test servers (tests/support/serve_backends.mjs) in a node process: a Nostr
// relay and an MQTT 5 broker on localhost, controlled through commands, and an untrusted
// wss:// server.
class BackendServers {
  public:
    BackendServers() {
        int to_child[2], from_child[2];
        if (pipe(to_child) != 0 || pipe(from_child) != 0)
            throw std::runtime_error("cannot create pipes");
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, to_child[0], 0);
        posix_spawn_file_actions_adddup2(&actions, from_child[1], 1);
        posix_spawn_file_actions_addclose(&actions, to_child[1]);
        posix_spawn_file_actions_addclose(&actions, from_child[0]);
        std::string node = P2PDC_TEST_NODE,
                    script = P2PDC_TEST_SOURCE_DIR "/tests/support/serve_backends.mjs";
        char *argv[] = {node.data(), script.data(), nullptr};
        const int spawned = posix_spawn(&pid_, node.c_str(), &actions, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(to_child[0]);
        close(from_child[1]);
        if (spawned != 0)
            throw std::runtime_error("cannot start node");
        input_ = fdopen(to_child[1], "w");
        output_ = fdopen(from_child[0], "r");
        const auto urls = read();
        nostr = urls.at("nostr").get<std::string>();
        mqtt = urls.at("mqtt").get<std::string>();
        untrusted = urls.at("untrusted").get<std::string>();
        peerjs = urls.at("peerjs").get<std::string>();
        silent_peerjs = urls.at("silentPeerjs").get<std::string>();
    }
    ~BackendServers() {
        fclose(input_); // the script exits when its input closes
        fclose(output_);
        int status = 0;
        waitpid(pid_, &status, 0);
    }
    BackendServers(const BackendServers &) = delete;
    BackendServers &operator=(const BackendServers &) = delete;

    nlohmann::json command(const std::string &line) {
        std::fputs((line + "\n").c_str(), input_);
        std::fflush(input_);
        return read();
    }

    std::string nostr;
    std::string mqtt;
    // A wss:// echo server with a self-signed certificate.
    std::string untrusted;
    // A strict PeerJS server, which relays only genuine PeerJS signalling, with the key "peerjs".
    std::string peerjs;
    // The same, but closing connections for a taken ID without telling them so.
    std::string silent_peerjs;

  private:
    nlohmann::json read() {
        std::string line;
        for (int c = std::fgetc(output_); c != EOF && c != '\n'; c = std::fgetc(output_))
            line.push_back(static_cast<char>(c));
        if (line.empty())
            throw std::runtime_error("test servers ended");
        return nlohmann::json::parse(line);
    }
    pid_t pid_ = 0;
    FILE *input_ = nullptr;
    FILE *output_ = nullptr;
};

} // namespace p2pdc::test
