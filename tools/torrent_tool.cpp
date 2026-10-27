#include "core/bencode.h"
#include "core/hash_utils.h"
#include "core/path_sanitizer.h"
#include "core/piece_manager.h"
#include "core/torrent_builder.h"
#include "core/torrent_file.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void printHelp() {
    std::cout << "P2P BitTorrent CLI Tool\n\n"
              << "Usage:\n"
              << "  p2p create <path> [options]         Create a .torrent file from a file or folder\n"
              << "  p2p info <torrent_file>             Display metadata and info_hash of a .torrent file\n"
              << "  p2p verify <torrent_file> <data>    Verify data integrity against pieces in .torrent\n"
              << "  p2p help                            Show this help message\n\n"
              << "Options for 'create':\n"
              << "  -t, --tracker <url>                 Tracker announce URL (default: http://127.0.0.1:6969/announce)\n"
              << "      --no-tracker                   Disable tracker announce in generated torrent metadata\n"
              << "      --dht-bootstrap-host <ip>       DHT bootstrap host (e.g. 10.10.59.157)\n"
              << "      --dht-bootstrap-port <port>     DHT bootstrap port (e.g. 6881)\n"
              << "      --dht-enabled                   Enable DHT in downstream session/client usage\n"
              << "      --tracker-enabled               Enable tracker announce in downstream session/client usage\n"
              << "  -l, --piece-length <bytes>          Piece length in bytes (default: 262144 = 256 KiB)\n"
              << "  -o, --output <file>                 Output .torrent file path (default: <name>.torrent)\n"
              << "  -n, --name <name>                   Override display name in torrent info\n"
              << std::endl;
}

std::string formatSize(std::uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KiB";
    if (bytes < 1024 * 1024 * 1024) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MiB";
        return ss.str();
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GiB";
    return ss.str();
}

int handleCreate(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Error: missing path to source file or directory.\n"
                  << "Run 'p2p create <path> [options]' or 'p2p help'\n";
        return 1;
    }

    std::filesystem::path sourcePath = argv[2];
    std::string announce = "http://127.0.0.1:6969/announce";
    std::size_t pieceLength = 262144;
    std::string outputPath;
    std::string nameOverride;
    std::string dhtBootstrapHost;
    std::uint16_t dhtBootstrapPort = 0;
    bool enableTracker = true;
    bool enableDht = true;

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-t" || arg == "--tracker") && i + 1 < argc) {
            announce = argv[++i];
        } else if ((arg == "-l" || arg == "--piece-length") && i + 1 < argc) {
            try {
                pieceLength = std::stoull(argv[++i]);
            } catch (...) {
                std::cerr << "Error: invalid piece length\n";
                return 1;
            }
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = argv[++i];
        } else if ((arg == "-n" || arg == "--name") && i + 1 < argc) {
            nameOverride = argv[++i];
        } else if (arg == "--no-tracker") {
            announce.clear();
            enableTracker = false;
        } else if (arg == "--dht-bootstrap-host" && i + 1 < argc) {
            dhtBootstrapHost = argv[++i];
        } else if (arg == "--dht-bootstrap-port" && i + 1 < argc) {
            try {
                dhtBootstrapPort = static_cast<std::uint16_t>(std::stoul(argv[++i]));
            } catch (...) {
                std::cerr << "Error: invalid DHT bootstrap port\n";
                return 1;
            }
        } else if (arg == "--dht-enabled") {
            enableDht = true;
        } else if (arg == "--tracker-enabled") {
            enableTracker = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }

    if (!std::filesystem::exists(sourcePath)) {
        std::cerr << "Error: path does not exist: " << sourcePath << "\n";
        return 1;
    }

    if (outputPath.empty()) {
        std::string base = nameOverride.empty() ? sourcePath.filename().string() : nameOverride;
        if (base.empty()) base = "output";
        outputPath = base + ".torrent";
    }

    std::cout << "Creating .torrent for: " << sourcePath << " ...\n";
    try {
        std::string torrentBytes = p2p::TorrentBuilder::buildTorrent(
            sourcePath, announce, pieceLength, nameOverride);

        std::ofstream out(outputPath, std::ios::binary);
        if (!out) {
            std::cerr << "Error: cannot open output file for writing: " << outputPath << "\n";
            return 1;
        }
        out.write(torrentBytes.data(), static_cast<std::streamsize>(torrentBytes.size()));
        out.close();

        auto meta = p2p::TorrentFile::parse(torrentBytes);
        std::string infoHashHex = p2p::sha1Hex(meta.infoRaw);

        std::cout << "\n Torrent created successfully!\n"
                  << "  Output File:        " << outputPath << "\n"
                  << "  Torrent Name:       " << meta.name << "\n"
                  << "  Info Hash:          " << infoHashHex << "\n"
                  << "  Total Size:         " << formatSize(meta.length) << " (" << meta.length << " bytes)\n"
                  << "  Piece Length:       " << formatSize(meta.pieceLength) << "\n"
                  << "  Piece Count:        " << meta.pieces.size() << "\n"
                  << "  Tracker URL:        " << (announce.empty() ? "(none / trackerless)" : announce) << "\n"
                  << "  Tracker Enabled:    " << (enableTracker ? "yes" : "no") << "\n"
                  << "  DHT Enabled:        " << (enableDht ? "yes" : "no") << "\n"
                  << "  DHT Bootstrap:      " << (dhtBootstrapHost.empty() ? "(none)" : dhtBootstrapHost + ":" + std::to_string(dhtBootstrapPort)) << "\n"
                  << "  Magnet Link:        magnet:?xt=urn:btih:" << infoHashHex << "&dn=" << meta.name << "\n"
                  << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error creating torrent: " << e.what() << "\n";
        return 1;
    }
}

int handleInfo(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: p2p info <file.torrent>\n";
        return 1;
    }

    std::filesystem::path torrentPath = argv[2];
    std::ifstream file(torrentPath, std::ios::binary);
    if (!file) {
        std::cerr << "Error: cannot open file: " << torrentPath << "\n";
        return 1;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    try {
        auto meta = p2p::TorrentFile::parse(content);
        std::string infoHashHex = p2p::sha1Hex(meta.infoRaw);

        std::cout << "=== Torrent Metadata ===\n"
                  << "  File Path:    " << torrentPath << "\n"
                  << "  Name:         " << meta.name << "\n"
                  << "  Info Hash:    " << infoHashHex << "\n"
                  << "  Total Length: " << formatSize(meta.length) << " (" << meta.length << " bytes)\n"
                  << "  Piece Length: " << formatSize(meta.pieceLength) << "\n"
                  << "  Piece Count:  " << meta.pieces.size() << "\n"
                  << "  Announce URL: " << (meta.announce.empty() ? "(none)" : meta.announce) << "\n";

        if (!meta.filePaths.empty()) {
            std::cout << "  Multi-file mode (" << meta.filePaths.size() << " files):\n";
            for (std::size_t i = 0; i < meta.filePaths.size() && i < 15; ++i) {
                std::cout << "    - " << meta.filePaths[i] << "\n";
            }
            if (meta.filePaths.size() > 15) {
                std::cout << "    ... and " << (meta.filePaths.size() - 15) << " more files\n";
            }
        }
        std::cout << "========================\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error parsing torrent: " << e.what() << "\n";
        return 1;
    }
}

int handleVerify(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: p2p verify <file.torrent> <data_path>\n";
        return 1;
    }

    std::filesystem::path torrentPath = argv[2];
    std::filesystem::path dataPath = argv[3];

    std::ifstream tf(torrentPath, std::ios::binary);
    if (!tf) {
        std::cerr << "Error opening torrent file: " << torrentPath << "\n";
        return 1;
    }
    std::string torrentBytes((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());

    try {
        auto meta = p2p::TorrentFile::parse(torrentBytes);
        std::cout << "Verifying data at " << dataPath << " against torrent " << meta.name << "...\n";

        p2p::PieceManager pm(meta.pieces.size(), static_cast<std::size_t>(meta.pieceLength), meta.length, meta.pieces);
        if (!pm.loadFromFile(dataPath)) {
            std::cerr << "Could not load data file at " << dataPath << "\n";
            return 1;
        }

        std::size_t verified = 0;
        for (std::size_t i = 0; i < meta.pieces.size(); ++i) {
            if (pm.verifyPiece(i)) {
                verified++;
            }
        }

        std::cout << "Verification result: " << verified << "/" << meta.pieces.size() << " pieces valid ("
                  << (verified * 100 / meta.pieces.size()) << "%)\n";
        return verified == meta.pieces.size() ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Error during verification: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printHelp();
        return 0;
    }

    std::string cmd = argv[1];
    if (cmd == "create") {
        return handleCreate(argc, argv);
    } else if (cmd == "info") {
        return handleInfo(argc, argv);
    } else if (cmd == "verify" || cmd == "check") {
        return handleVerify(argc, argv);
    } else if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        printHelp();
        return 0;
    }

    std::cerr << "Unknown command: " << cmd << "\n\n";
    printHelp();
    return 1;
}
