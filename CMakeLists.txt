cmake_minimum_required(VERSION 3.16)
project(p2p_network LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# cpp-httplib & Asio yêu cầu target Windows 10+ (0x0A00) khi compile trên Windows
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

enable_testing()

add_executable(test_piece_picker
    tests/test_piece_picker.cpp
    src/strategy/piece_picker.cpp
)

target_include_directories(test_piece_picker PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_piece_picker COMMAND test_piece_picker)

add_executable(test_choker
    tests/test_choker.cpp
    src/strategy/choker.cpp
)

target_include_directories(test_choker PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_choker COMMAND test_choker)

# DHT (BEP 5): NodeId + RoutingTable + Krpc (logic thuần) + KademliaNode (I/O thật)
add_executable(test_kademlia
    tests/test_kademlia.cpp
    src/network/dht/node_id.cpp
    src/network/dht/routing_table.cpp
    src/network/dht/krpc.cpp
    src/network/dht/kademlia_node.cpp
    src/core/bencode.cpp
    src/core/hash_utils.cpp
)

target_include_directories(test_kademlia PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(test_kademlia PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(test_kademlia PRIVATE ws2_32 crypt32)
endif()
add_test(NAME test_kademlia COMMAND test_kademlia)

# LSD (BEP 14): compile-check (test multicast thật chạy bằng tay qua script riêng)
add_library(lsd_lib STATIC
    src/network/lsd.cpp
)
target_include_directories(lsd_lib PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(lsd_lib PRIVATE ws2_32)
endif()

add_executable(test_bencode
    tests/test_bencode.cpp
    src/core/bencode.cpp
)

target_include_directories(test_bencode PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_bencode COMMAND test_bencode)

add_executable(test_torrent_file
    tests/test_torrent_file.cpp
    src/core/bencode.cpp
    src/core/torrent_file.cpp
)

target_include_directories(test_torrent_file PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_torrent_file COMMAND test_torrent_file)

add_executable(test_path_sanitizer
    tests/test_path_sanitizer.cpp
    src/core/path_sanitizer.cpp
)

target_include_directories(test_path_sanitizer PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_path_sanitizer COMMAND test_path_sanitizer)

add_executable(test_torrent_builder
    tests/test_torrent_builder.cpp
    src/core/bencode.cpp
    src/core/torrent_file.cpp
    src/core/path_sanitizer.cpp
    src/core/hash_utils.cpp
    src/core/torrent_builder.cpp
)

target_include_directories(test_torrent_builder PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(OpenSSL REQUIRED)
target_link_libraries(test_torrent_builder PRIVATE OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME test_torrent_builder COMMAND test_torrent_builder)

add_executable(test_piece_manager
    tests/test_piece_manager.cpp
    src/core/bencode.cpp
    src/core/hash_utils.cpp
    src/core/piece_manager.cpp
)

target_include_directories(test_piece_manager PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(OpenSSL REQUIRED)
target_link_libraries(test_piece_manager PRIVATE OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME test_piece_manager COMMAND test_piece_manager)

add_executable(test_metadata_integration
    tests/test_metadata_integration.cpp
    src/core/bencode.cpp
    src/core/torrent_file.cpp
    src/core/path_sanitizer.cpp
    src/core/hash_utils.cpp
    src/core/torrent_builder.cpp
    src/core/piece_manager.cpp
)

target_include_directories(test_metadata_integration PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(OpenSSL REQUIRED)
target_link_libraries(test_metadata_integration PRIVATE OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME test_metadata_integration COMMAND test_metadata_integration)

find_package(Threads REQUIRED)

add_executable(test_tracker_protocol
    tests/test_tracker_protocol.cpp
    src/core/bencode.cpp
    src/network/tracker_protocol.cpp
    src/network/tracker_client.cpp
    tracker/tracker_server.cpp
    tracker/peer_registry.cpp
)

target_include_directories(test_tracker_protocol PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_include_directories(test_tracker_protocol PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_tracker_protocol PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(test_tracker_protocol PRIVATE ws2_32 crypt32)
endif()
add_test(NAME test_tracker_protocol COMMAND test_tracker_protocol)

# Peer Wire Protocol (BEP 3, BEP 5, BEP 10): encode/decode & defensive checks
add_executable(test_peer_protocol
    tests/test_peer_protocol.cpp
    src/network/peer_protocol.cpp
)
target_include_directories(test_peer_protocol PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_peer_protocol COMMAND test_peer_protocol)

# Peer Connection (TCP socket via Asio, loopback integration test)
add_executable(test_peer_connection
    tests/test_peer_connection.cpp
    src/network/peer_protocol.cpp
    src/network/peer_connection.cpp
)
target_include_directories(test_peer_connection PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/asio
)
target_link_libraries(test_peer_connection PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(test_peer_connection PRIVATE ws2_32 crypt32)
endif()
add_test(NAME test_peer_connection COMMAND test_peer_connection)

# Torrent Session (Orchestrator kết nối Peer, PieceManager, Tracker, Strategy)
add_executable(test_torrent_session
    tests/test_torrent_session.cpp
    src/network/torrent_session.cpp
    src/network/peer_protocol.cpp
    src/network/peer_connection.cpp
    src/core/piece_manager.cpp
    src/core/torrent_file.cpp
    src/core/bencode.cpp
    src/core/hash_utils.cpp
    src/core/path_sanitizer.cpp
    src/network/tracker_client.cpp
    src/network/tracker_protocol.cpp
    src/strategy/piece_picker.cpp
    src/strategy/choker.cpp
    src/network/lsd.cpp
)
target_include_directories(test_torrent_session PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/asio
    ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(test_torrent_session PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(test_torrent_session PRIVATE ws2_32 crypt32)
endif()
add_test(NAME test_torrent_session COMMAND test_torrent_session)

# Công cụ thực chiến Peer Wire: seed, leech, security attack simulation
add_executable(peer_wire_tool
    tools/peer_wire_tool.cpp
    src/network/peer_protocol.cpp
    src/network/peer_connection.cpp
)
target_include_directories(peer_wire_tool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/asio
)
target_link_libraries(peer_wire_tool PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(peer_wire_tool PRIVATE ws2_32 crypt32)
endif()

# Chương trình chính: chạy web UI thật, nối nút "Create Torrent" tới
# core::TorrentBuilder qua POST /api/torrents/create (xem src/webui/).
add_executable(p2p_server
    src/main.cpp
    src/core/bencode.cpp
    src/core/hash_utils.cpp
    src/core/path_sanitizer.cpp
    src/core/torrent_file.cpp
    src/core/torrent_builder.cpp
    src/core/piece_manager.cpp
    src/webui/http_server.cpp
    src/webui/api_routes.cpp
    src/webui/state_bridge.cpp
    src/network/torrent_session.cpp
    src/network/peer_protocol.cpp
    src/network/peer_connection.cpp
    src/network/tracker_client.cpp
    src/network/tracker_protocol.cpp
    src/strategy/piece_picker.cpp
    src/strategy/choker.cpp
    src/network/lsd.cpp
)

target_include_directories(p2p_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/asio
)
target_link_libraries(p2p_server PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

if(WIN32)
    target_link_libraries(p2p_server PRIVATE ws2_32 crypt32)
endif()

# Tracker server thật (BEP 3 + BEP 23): bind 0.0.0.0 để các máy khác trong
# LAN announce tới được (khác p2p_server, chỉ bind 127.0.0.1 cho web UI).
add_executable(p2p_tracker
    tracker/main.cpp
    tracker/tracker_server.cpp
    tracker/peer_registry.cpp
    src/core/bencode.cpp
    src/network/tracker_protocol.cpp
)

target_include_directories(p2p_tracker PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
target_include_directories(p2p_tracker PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(p2p_tracker PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(p2p_tracker PRIVATE ws2_32 crypt32)
endif()
