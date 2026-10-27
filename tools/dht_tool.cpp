#include "core/hash_utils.h"
#include "network/dht/kademlia_node.h"
#include "network/dht/node_id.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

void printHelp() {
    std::cout << "\n=== DHT Kademlia Tool (BEP 5) ===\n"
              << "Commands:\n"
              << "  help                                  Show this help message\n"
              << "  info                                  Display self Node ID and stats\n"
              << "  nodes                                 List all contacts in Routing Table\n"
              << "  ping <ip> <port>                      Send ping to remote DHT node\n"
              << "  bootstrap <ip> <port>                 Bootstrap into DHT network\n"
              << "  get_peers <info_hash_hex>             Query peers for an info_hash\n"
              << "  announce <info_hash_hex> <tcp_port>   Announce peer for info_hash\n"
              << "  find_node <target_hex>                Find closest nodes to target ID\n"
              << "  quit / exit                           Shutdown DHT node and quit\n"
              << "=================================\n" << std::endl;
}

std::vector<std::string> splitTokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string hexToRaw(const std::string& hex) {
    if (hex.size() != 40) return "";
    std::string raw;
    raw.reserve(20);
    for (std::size_t i = 0; i < 40; i += 2) {
        unsigned int byteVal = 0;
        std::stringstream ss;
        ss << std::hex << hex.substr(i, 2);
        ss >> byteVal;
        raw.push_back(static_cast<char>(byteVal));
    }
    return raw;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 6881;
    std::string bootstrapHost;
    std::uint16_t bootstrapPort = 0;

    if (argc > 1) {
        try {
            port = static_cast<std::uint16_t>(std::stoi(argv[1]));
        } catch (...) {
            std::cerr << "Invalid port, using default 6881\n";
        }
    }

    if (argc > 2) {
        std::string bp = argv[2];
        const auto colon = bp.find(':');
        if (colon != std::string::npos) {
            bootstrapHost = bp.substr(0, colon);
            try {
                bootstrapPort = static_cast<std::uint16_t>(std::stoi(bp.substr(colon + 1)));
            } catch (...) {}
        }
    }

    p2p::NodeId selfId = p2p::NodeId::random();
    p2p::KademliaNode node(selfId, port);

    if (!node.start()) {
        std::cerr << "Failed to bind DHT node to UDP port " << port << std::endl;
        return 1;
    }

    std::cout << "[DHT] Node started successfully!\n"
              << "  UDP Port: " << port << "\n"
              << "  Node ID:  " << selfId.toHex() << std::endl;

    if (!bootstrapHost.empty() && bootstrapPort != 0) {
        std::cout << "[DHT] Bootstrapping to " << bootstrapHost << ":" << bootstrapPort << "..." << std::endl;
        if (node.bootstrap(bootstrapHost, bootstrapPort)) {
            std::cout << "[DHT] Bootstrap SUCCESS! Routing table size: " << node.routingTable().size() << std::endl;
        } else {
            std::cout << "[DHT] Bootstrap FAILED (node did not respond to ping)" << std::endl;
        }
    }

    printHelp();

    std::string line;
    while (std::cout << "dht> " && std::getline(std::cin, line)) {
        auto tokens = splitTokens(line);
        if (tokens.empty()) continue;

        const std::string& cmd = tokens[0];
        if (cmd == "quit" || cmd == "exit") {
            break;
        } else if (cmd == "help") {
            printHelp();
        } else if (cmd == "info") {
            std::cout << "Node ID:  " << node.selfId().toHex() << "\n"
                      << "UDP Port: " << node.port() << "\n"
                      << "Contacts: " << node.routingTable().size() << " nodes in routing table" << std::endl;
        } else if (cmd == "nodes") {
            const auto closest = node.routingTable().closestTo(node.selfId(), 128);
            std::cout << "--- Routing Table (" << closest.size() << " nodes) ---\n";
            for (std::size_t i = 0; i < closest.size(); ++i) {
                std::cout << "  [" << i + 1 << "] " << closest[i].ip << ":" << closest[i].port
                          << " | ID: " << closest[i].id.toHex() << "\n";
            }
            std::cout << "------------------------------------\n";
        } else if (cmd == "ping") {
            if (tokens.size() < 3) {
                std::cout << "Usage: ping <ip> <port>\n";
                continue;
            }
            std::string ip = tokens[1];
            std::uint16_t targetPort = static_cast<std::uint16_t>(std::stoi(tokens[2]));
            std::cout << "Pinging " << ip << ":" << targetPort << "...\n";
            if (node.ping(ip, targetPort)) {
                std::cout << "PONG received! Node added to routing table.\n";
            } else {
                std::cout << "Ping timed out (no response).\n";
            }
        } else if (cmd == "bootstrap") {
            if (tokens.size() < 3) {
                std::cout << "Usage: bootstrap <ip> <port>\n";
                continue;
            }
            std::string ip = tokens[1];
            std::uint16_t targetPort = static_cast<std::uint16_t>(std::stoi(tokens[2]));
            std::cout << "Bootstrapping via " << ip << ":" << targetPort << "...\n";
            if (node.bootstrap(ip, targetPort)) {
                std::cout << "Bootstrap OK! Routing table size: " << node.routingTable().size() << "\n";
            } else {
                std::cout << "Bootstrap failed.\n";
            }
        } else if (cmd == "get_peers") {
            if (tokens.size() < 2) {
                std::cout << "Usage: get_peers <info_hash_hex>\n";
                continue;
            }
            std::string rawHash = hexToRaw(tokens[1]);
            if (rawHash.size() != 20) {
                std::cout << "Error: info_hash must be exactly 40 hex characters.\n";
                continue;
            }
            std::cout << "Querying DHT for peers of " << tokens[1] << "...\n";
            auto peers = node.getPeers(rawHash);
            std::cout << "Found " << peers.size() << " peer(s):\n";
            for (const auto& p : peers) {
                std::cout << "  -> " << p.ip << ":" << p.port << "\n";
            }
        } else if (cmd == "announce") {
            if (tokens.size() < 3) {
                std::cout << "Usage: announce <info_hash_hex> <tcp_port>\n";
                continue;
            }
            std::string rawHash = hexToRaw(tokens[1]);
            if (rawHash.size() != 20) {
                std::cout << "Error: info_hash must be exactly 40 hex characters.\n";
                continue;
            }
            std::uint16_t tcpPort = static_cast<std::uint16_t>(std::stoi(tokens[2]));
            std::cout << "Announcing peer for " << tokens[1] << " on TCP port " << tcpPort << "...\n";
            node.announcePeer(rawHash, tcpPort);
            std::cout << "Announce complete!\n";
        } else if (cmd == "find_node") {
            if (tokens.size() < 2) {
                std::cout << "Usage: find_node <target_hex>\n";
                continue;
            }
            std::string rawTarget = hexToRaw(tokens[1]);
            if (rawTarget.size() != 20) {
                std::cout << "Error: target ID must be 40 hex characters.\n";
                continue;
            }
            auto contacts = node.findNode(p2p::NodeId::fromString(rawTarget));
            std::cout << "Closest " << contacts.size() << " node(s):\n";
            for (const auto& c : contacts) {
                std::cout << "  -> " << c.ip << ":" << c.port << " (" << c.id.toHex() << ")\n";
            }
        } else {
            std::cout << "Unknown command: " << cmd << ". Type 'help' for commands.\n";
        }
    }

    std::cout << "Stopping DHT node..." << std::endl;
    node.stop();
    return 0;
}

