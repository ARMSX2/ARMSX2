// SPDX-FileCopyrightText: 2026 EmuCoreX contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "net.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

// A small authenticated layer-2 tunnel intended for phones on the same Wi-Fi
// network or local hotspot. The host is a star-topology Ethernet relay; it is
// deliberately separate from the internet-facing DEV9 Sockets backend.
class LocalLinkAdapter final : public NetAdapter
{
public:
	LocalLinkAdapter();
	~LocalLinkAdapter() override;

	bool blocks() override;
	bool isInitialised() override;
	bool recv(NetPacket* pkt) override;
	bool send(NetPacket* pkt) override;
	void reloadSettings() override;
	void close() override;

private:
	static constexpr std::size_t MAX_FRAGMENT_PAYLOAD = 1000;
	static constexpr std::size_t MAX_PEERS = 8;

	struct Peer
	{
		sockaddr_in endpoint{};
		u32 id = 0;
		std::chrono::steady_clock::time_point last_seen{};
		u32 highest_sequence = 0;
		u64 replay_window = 0;
	};

	struct ReassemblyKey
	{
		u32 peer_id;
		u32 frame_id;

		bool operator==(const ReassemblyKey& rhs) const
		{
			return peer_id == rhs.peer_id && frame_id == rhs.frame_id;
		}
	};

	struct ReassemblyKeyHash
	{
		std::size_t operator()(const ReassemblyKey& key) const
		{
			return (static_cast<std::size_t>(key.peer_id) << 32) ^ key.frame_id;
		}
	};

	struct Reassembly
	{
		std::array<u8, 1514> data{};
		std::array<u16, 2> sizes{};
		std::array<bool, 2> received{};
		u16 fragment_count = 0;
		std::chrono::steady_clock::time_point created{};
	};

#ifdef _WIN32
	SOCKET m_socket = INVALID_SOCKET;
	bool m_wsa_started = false;
#else
	int m_socket = -1;
#endif
	std::atomic<bool> m_initialized{false};
	bool m_host = false;
	std::atomic<bool> m_closed{false};
	u16 m_port = 19072;
	u32 m_peer_id = 1;
	std::atomic<u32> m_send_sequence{1};
	std::atomic<u32> m_frame_id{1};
	std::atomic<u64> m_session_nonce{0};
	u64 m_auth_key0 = 0;
	u64 m_auth_key1 = 0;
	sockaddr_in m_host_endpoint{};
	std::chrono::steady_clock::time_point m_last_hello{};

	std::mutex m_peer_mutex;
	std::mutex m_socket_mutex;
	std::vector<Peer> m_peers;
	std::unordered_map<u32, Peer> m_remote_peers;
	std::unordered_map<ReassemblyKey, Reassembly, ReassemblyKeyHash> m_reassembly;

	bool OpenSocket();
	bool ConfigureEndpoint();
	void SendHelloIfNeeded(bool force = false);
	void SendControl(u8 type, const sockaddr_in& endpoint);
	bool SendFrameFragments(const NetPacket& pkt, const sockaddr_in& endpoint);
	bool SendDatagram(u8 type, u32 source_peer, u32 frame_id, u16 fragment_index,
		u16 fragment_count, const void* payload, u16 payload_size, const sockaddr_in& endpoint);
	bool ReceiveDatagram(NetPacket* pkt, bool* had_datagram);
	void RelayDatagram(const void* data, std::size_t size, const sockaddr_in& source);
	Peer* FindPeer(u32 id, const sockaddr_in& endpoint);
	bool RegisterPeer(u32 id, u32 hello_sequence, const sockaddr_in& endpoint);
	bool AcceptSequence(Peer& peer, u32 sequence);
	bool VerifyLocalLinkPacket(NetPacket* pkt, int read_size);
	void PurgeExpiredState();

	static bool SameEndpoint(const sockaddr_in& lhs, const sockaddr_in& rhs);
	static u64 DeriveKey(const std::string& room_code, u64 salt);
	static u64 Authenticate(const void* header, std::size_t header_size,
		const void* payload, std::size_t payload_size, u64 key0, u64 key1);
};
