// SPDX-FileCopyrightText: 2026 EmuCoreX contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LocalLinkAdapter.h"

#include "DEV9.h"

#include "common/Console.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <random>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
constexpr u32 LOCAL_LINK_MAGIC = 0x45434c58; // "ECLX" on the wire after htonl().
constexpr u8 LOCAL_LINK_VERSION = 1;
constexpr u8 MESSAGE_HELLO = 1;
constexpr u8 MESSAGE_HELLO_ACK = 2;
constexpr u8 MESSAGE_DATA = 3;

#pragma pack(push, 1)
struct WireHeader
{
	u32 magic;
	u8 version;
	u8 type;
	u16 header_size;
	u32 peer_id;
	u32 sequence;
	u32 frame_id;
	u16 fragment_index;
	u16 fragment_count;
	u16 payload_size;
	u16 reserved;
	u64 session_nonce;
	u64 auth_tag;
};
#pragma pack(pop)

static_assert(sizeof(WireHeader) == 44);

u64 RotateLeft(u64 value, int bits)
{
	return (value << bits) | (value >> (64 - bits));
}

u64 HostToNetwork64(u64 value)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return value;
#else
	return (static_cast<u64>(htonl(static_cast<u32>(value))) << 32) | htonl(static_cast<u32>(value >> 32));
#endif
}

u64 NetworkToHost64(u64 value)
{
	return HostToNetwork64(value);
}

u64 Load64Le(const u8* bytes)
{
	u64 value = 0;
	for (int i = 7; i >= 0; --i)
		value = (value << 8) | bytes[i];
	return value;
}

void SipRound(u64& v0, u64& v1, u64& v2, u64& v3)
{
	v0 += v1;
	v1 = RotateLeft(v1, 13);
	v1 ^= v0;
	v0 = RotateLeft(v0, 32);
	v2 += v3;
	v3 = RotateLeft(v3, 16);
	v3 ^= v2;
	v0 += v3;
	v3 = RotateLeft(v3, 21);
	v3 ^= v0;
	v2 += v1;
	v1 = RotateLeft(v1, 17);
	v1 ^= v2;
	v2 = RotateLeft(v2, 32);
}

u64 SipHash24(const u8* data, std::size_t size, u64 key0, u64 key1)
{
	u64 v0 = 0x736f6d6570736575ULL ^ key0;
	u64 v1 = 0x646f72616e646f6dULL ^ key1;
	u64 v2 = 0x6c7967656e657261ULL ^ key0;
	u64 v3 = 0x7465646279746573ULL ^ key1;

	const u8* cursor = data;
	const u8* end = data + (size & ~static_cast<std::size_t>(7));
	while (cursor != end)
	{
		const u64 word = Load64Le(cursor);
		v3 ^= word;
		SipRound(v0, v1, v2, v3);
		SipRound(v0, v1, v2, v3);
		v0 ^= word;
		cursor += 8;
	}

	u64 tail = static_cast<u64>(size) << 56;
	for (std::size_t i = 0; i < (size & 7); ++i)
		tail |= static_cast<u64>(cursor[i]) << (8 * i);
	v3 ^= tail;
	SipRound(v0, v1, v2, v3);
	SipRound(v0, v1, v2, v3);
	v0 ^= tail;
	v2 ^= 0xff;
	for (int i = 0; i < 4; ++i)
		SipRound(v0, v1, v2, v3);
	return v0 ^ v1 ^ v2 ^ v3;
}
} // namespace

LocalLinkAdapter::LocalLinkAdapter()
	: NetAdapter()
{
	if (!EmuConfig.DEV9.EthEnable || EmuConfig.DEV9.LocalLinkRoomCode.size() < 4 ||
		EmuConfig.DEV9.LocalLinkRoomCode.size() > 12)
	{
		Console.Error("DEV9: Local Link requires Ethernet and a 4-12 character room code");
		return;
	}

	m_host = EmuConfig.DEV9.LocalLinkHost;
	m_port = static_cast<u16>(std::clamp<u32>(EmuConfig.DEV9.LocalLinkPort, 1024, 65535));
	m_peer_id = m_host ? 1u : std::clamp<u32>(EmuConfig.DEV9.LocalLinkPeerId, 2, 65533);
	if (m_host)
	{
		std::random_device random;
		const u64 nonce = (static_cast<u64>(random()) << 32) | random();
		m_session_nonce.store(nonce != 0 ? nonce : 1, std::memory_order_relaxed);
	}
	m_auth_key0 = DeriveKey(EmuConfig.DEV9.LocalLinkRoomCode, 0x9e3779b97f4a7c15ULL);
	m_auth_key1 = DeriveKey(EmuConfig.DEV9.LocalLinkRoomCode, 0xd1b54a32d192ed03ULL);

	if (!OpenSocket() || !ConfigureEndpoint())
	{
		close();
		return;
	}

	PacketReader::MAC_Address mac = defaultMAC;
	mac.bytes[4] = static_cast<u8>((m_peer_id >> 8) & 0xff);
	mac.bytes[5] = static_cast<u8>(m_peer_id & 0xff);
	SetMACAddress(&mac);

	// Use the full peer id in a /16 so different identities never collapse to
	// the same DHCP lease. Skip 192.0.2.1, which belongs to the internal server.
	u32 host_part = m_peer_id;
	if (host_part >= 513)
		++host_part;
	const PacketReader::IP::IP_Address ps2_ip{{{192, 0,
		static_cast<u8>((host_part >> 8) & 0xff), static_cast<u8>(host_part & 0xff)}}};
	const PacketReader::IP::IP_Address subnet{{{255, 255, 0, 0}}};
	const PacketReader::IP::IP_Address gateway = internalIP;
	InitInternalServer(nullptr, true, ps2_ip, subnet, gateway);

	m_initialized.store(true, std::memory_order_release);
	SendHelloIfNeeded(true);
	Console.WriteLn("DEV9: Local Link %s ready on port %u as peer %u",
		m_host ? "host" : "client", m_port, m_peer_id);
}

LocalLinkAdapter::~LocalLinkAdapter()
{
	close();
}

bool LocalLinkAdapter::blocks()
{
	return false;
}

bool LocalLinkAdapter::isInitialised()
{
	return m_initialized.load(std::memory_order_acquire);
}

bool LocalLinkAdapter::OpenSocket()
{
#ifdef _WIN32
	WSADATA data{};
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return false;
	m_wsa_started = true;
	m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_socket == INVALID_SOCKET)
		return false;
	u_long non_blocking = 1;
	if (ioctlsocket(m_socket, FIONBIO, &non_blocking) != 0)
		return false;
#else
	m_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (m_socket < 0)
		return false;
	const int flags = fcntl(m_socket, F_GETFL, 0);
	if (flags < 0 || fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) < 0)
		return false;
#endif
	const int reuse = 1;
	setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
	return true;
}

bool LocalLinkAdapter::ConfigureEndpoint()
{
	sockaddr_in local{};
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = htons(m_host ? m_port : 0);
	if (bind(m_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
	{
		Console.Error("DEV9: Local Link failed to bind UDP port %u", m_host ? m_port : 0);
		return false;
	}

	if (m_host)
		return true;

	m_host_endpoint.sin_family = AF_INET;
	m_host_endpoint.sin_port = htons(m_port);

	const std::string& host = EmuConfig.DEV9.LocalLinkAddress;
	if (host.empty())
	{
		Console.Error("DEV9: Local Link join mode needs the host device's address");
		return false;
	}

	// Numeric IPv4 FIRST, and deliberately so: a LAN address or a VPN address (Tailscale's
	// 100.x.y.z, ZeroTier, WireGuard) is the overwhelmingly common case, and this path must never
	// touch DNS. ConfigureEndpoint runs from the adapter's constructor via GetNetAdapter() while
	// the VM is booting on the CPU thread, so a blocking resolve here stalls game start.
	if (inet_pton(AF_INET, host.c_str(), &m_host_endpoint.sin_addr) == 1)
		return true;

	// Not a literal, so treat it as a hostname — a dynamic-DNS name for a port-forwarded host, or
	// a VPN's own DNS name. One blocking lookup, once, at boot. There is no portable timeout for
	// getaddrinfo (DNS_Server.cpp sidesteps that by resolving on its own thread), so a name that
	// does not resolve costs the platform's DNS timeout before we give up — at which point DEV9
	// fails safe: GetNetAdapter deletes the adapter and InitNet clears EthEnable.
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
#ifdef AI_ADDRCONFIG
	hints.ai_flags = AI_ADDRCONFIG;
#endif
	addrinfo* results = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || results == nullptr)
	{
		if (results != nullptr)
			freeaddrinfo(results);
		Console.Error("DEV9: Local Link could not resolve host '%s' (use the numeric IPv4 address "
					  "shown on the host device if this keeps failing)",
			host.c_str());
		return false;
	}
	m_host_endpoint.sin_addr = reinterpret_cast<const sockaddr_in*>(results->ai_addr)->sin_addr;
	freeaddrinfo(results);

	char resolved[INET_ADDRSTRLEN] = {};
	inet_ntop(AF_INET, &m_host_endpoint.sin_addr, resolved, sizeof(resolved));
	Console.WriteLn("DEV9: Local Link resolved host '%s' to %s", host.c_str(), resolved);
	return true;
}

bool LocalLinkAdapter::recv(NetPacket* pkt)
{
	if (NetAdapter::recv(pkt))
		return true;
	if (!m_initialized.load(std::memory_order_acquire) || m_closed.load(std::memory_order_acquire))
		return false;

	SendHelloIfNeeded();
	PurgeExpiredState();
	for (int i = 0; i < 16; ++i)
	{
		bool had_datagram = false;
		if (ReceiveDatagram(pkt, &had_datagram))
			return true;
		if (!had_datagram)
			break;
	}
	return false;
}

bool LocalLinkAdapter::send(NetPacket* pkt)
{
	if (NetAdapter::send(pkt))
		return true;
	if (!m_initialized.load(std::memory_order_acquire) || m_closed.load(std::memory_order_acquire) ||
		pkt == nullptr || pkt->size <= 0 || pkt->size > 1514)
		return false;

	InspectSend(pkt);
	if (!m_host && m_session_nonce.load(std::memory_order_acquire) == 0)
		return true;
	if (!m_host)
		return SendFrameFragments(*pkt, m_host_endpoint);

	bool sent = false;
	std::lock_guard lock(m_peer_mutex);
	for (const Peer& peer : m_peers)
		sent = SendFrameFragments(*pkt, peer.endpoint) || sent;
	return sent || m_peers.empty();
}

void LocalLinkAdapter::reloadSettings()
{
	// Local Link endpoint, identity and room authentication changes require a
	// complete adapter restart. ReconfigureLiveNet() handles that comparison.
}

void LocalLinkAdapter::close()
{
	if (m_closed.exchange(true, std::memory_order_acq_rel))
		return;
	m_initialized.store(false, std::memory_order_release);
	std::lock_guard socket_lock(m_socket_mutex);
#ifdef _WIN32
	if (m_socket != INVALID_SOCKET)
	{
		closesocket(m_socket);
		m_socket = INVALID_SOCKET;
	}
	if (m_wsa_started)
	{
		WSACleanup();
		m_wsa_started = false;
	}
#else
	if (m_socket >= 0)
	{
		::close(m_socket);
		m_socket = -1;
	}
#endif
}

void LocalLinkAdapter::SendHelloIfNeeded(bool force)
{
	if (m_host || !m_initialized.load(std::memory_order_acquire))
		return;
	const auto now = std::chrono::steady_clock::now();
	if (!force && now - m_last_hello < std::chrono::seconds(1))
		return;
	m_last_hello = now;
	SendControl(MESSAGE_HELLO, m_host_endpoint);
}

void LocalLinkAdapter::SendControl(u8 type, const sockaddr_in& endpoint)
{
	SendDatagram(type, m_peer_id, 0, 0, 1, nullptr, 0, endpoint);
}

bool LocalLinkAdapter::SendFrameFragments(const NetPacket& pkt, const sockaddr_in& endpoint)
{
	const u32 frame_id = m_frame_id.fetch_add(1, std::memory_order_relaxed);
	const u16 count = static_cast<u16>((pkt.size + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD);
	bool sent = true;
	for (u16 index = 0; index < count; ++index)
	{
		const std::size_t offset = index * MAX_FRAGMENT_PAYLOAD;
		const u16 size = static_cast<u16>(std::min<std::size_t>(MAX_FRAGMENT_PAYLOAD, pkt.size - offset));
		sent = SendDatagram(MESSAGE_DATA, m_peer_id, frame_id, index, count,
			pkt.buffer + offset, size, endpoint) && sent;
	}
	return sent;
}

bool LocalLinkAdapter::SendDatagram(u8 type, u32 source_peer, u32 frame_id, u16 fragment_index,
	u16 fragment_count, const void* payload, u16 payload_size, const sockaddr_in& endpoint)
{
	std::array<u8, sizeof(WireHeader) + MAX_FRAGMENT_PAYLOAD> datagram{};
	WireHeader header{};
	header.magic = htonl(LOCAL_LINK_MAGIC);
	header.version = LOCAL_LINK_VERSION;
	header.type = type;
	header.header_size = htons(sizeof(WireHeader));
	header.peer_id = htonl(source_peer);
	header.sequence = htonl(m_send_sequence.fetch_add(1, std::memory_order_relaxed));
	header.frame_id = htonl(frame_id);
	header.fragment_index = htons(fragment_index);
	header.fragment_count = htons(fragment_count);
	header.payload_size = htons(payload_size);
	header.session_nonce = HostToNetwork64(m_session_nonce.load(std::memory_order_acquire));
	header.auth_tag = 0;
	std::memcpy(datagram.data(), &header, sizeof(header));
	if (payload_size != 0)
		std::memcpy(datagram.data() + sizeof(header), payload, payload_size);
	header.auth_tag = HostToNetwork64(Authenticate(datagram.data(), sizeof(header),
		datagram.data() + sizeof(header), payload_size, m_auth_key0, m_auth_key1));
	std::memcpy(datagram.data(), &header, sizeof(header));

	std::lock_guard socket_lock(m_socket_mutex);
	if (m_closed.load(std::memory_order_acquire))
		return false;
	const int result = sendto(m_socket, reinterpret_cast<const char*>(datagram.data()),
		static_cast<int>(sizeof(header) + payload_size), 0,
		reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
	return result == static_cast<int>(sizeof(header) + payload_size);
}

bool LocalLinkAdapter::ReceiveDatagram(NetPacket* pkt, bool* had_datagram)
{
	*had_datagram = false;
	std::array<u8, sizeof(WireHeader) + MAX_FRAGMENT_PAYLOAD> datagram{};
	sockaddr_in source{};
#ifdef _WIN32
	int source_size = sizeof(source);
#else
	socklen_t source_size = sizeof(source);
#endif
	int size;
	{
		std::lock_guard socket_lock(m_socket_mutex);
		if (m_closed.load(std::memory_order_acquire))
			return false;
		size = recvfrom(m_socket, reinterpret_cast<char*>(datagram.data()),
			static_cast<int>(datagram.size()), 0, reinterpret_cast<sockaddr*>(&source), &source_size);
	}
	if (size >= 0)
		*had_datagram = true;
	if (size < static_cast<int>(sizeof(WireHeader)))
		return false;

	WireHeader header{};
	std::memcpy(&header, datagram.data(), sizeof(header));
	const u64 received_tag = NetworkToHost64(header.auth_tag);
	header.auth_tag = 0;
	std::memcpy(datagram.data(), &header, sizeof(header));
	const u16 header_size = ntohs(header.header_size);
	const u16 payload_size = ntohs(header.payload_size);
	if (ntohl(header.magic) != LOCAL_LINK_MAGIC || header.version != LOCAL_LINK_VERSION ||
		header_size != sizeof(WireHeader) || payload_size > MAX_FRAGMENT_PAYLOAD ||
		size != static_cast<int>(sizeof(WireHeader) + payload_size) ||
		received_tag != Authenticate(datagram.data(), sizeof(WireHeader),
			datagram.data() + sizeof(WireHeader), payload_size, m_auth_key0, m_auth_key1))
	{
		return false;
	}

	const u32 source_peer = ntohl(header.peer_id);
	const u32 sequence = ntohl(header.sequence);
	const u64 session_nonce = NetworkToHost64(header.session_nonce);
	if (source_peer == 0 || source_peer == m_peer_id)
		return false;
	header.auth_tag = HostToNetwork64(received_tag);
	std::memcpy(datagram.data(), &header, sizeof(header));

	if (header.type == MESSAGE_HELLO)
	{
		if (m_host && RegisterPeer(source_peer, sequence, source))
			SendControl(MESSAGE_HELLO_ACK, source);
		return false;
	}
	if (header.type == MESSAGE_HELLO_ACK)
	{
		if (!m_host && SameEndpoint(source, m_host_endpoint) && session_nonce != 0)
		{
			const u64 previous = m_session_nonce.exchange(session_nonce, std::memory_order_acq_rel);
			if (previous != 0 && previous != session_nonce)
			{
				m_remote_peers.clear();
				m_reassembly.clear();
			}
		}
		return false;
	}
	if (header.type != MESSAGE_DATA)
		return false;
	if (session_nonce == 0 || session_nonce != m_session_nonce.load(std::memory_order_acquire))
		return false;

	if (m_host)
	{
		std::lock_guard lock(m_peer_mutex);
		Peer* peer = FindPeer(source_peer, source);
		if (peer == nullptr || !AcceptSequence(*peer, sequence))
			return false;
		peer->last_seen = std::chrono::steady_clock::now();
		RelayDatagram(datagram.data(), size, source);
	}
	else if (!SameEndpoint(source, m_host_endpoint))
	{
		return false;
	}
	else
	{
		auto remote_it = m_remote_peers.find(source_peer);
		if (remote_it == m_remote_peers.end())
		{
			if (m_remote_peers.size() >= MAX_PEERS + 1)
				return false;
			remote_it = m_remote_peers.emplace(source_peer, Peer{}).first;
		}
		Peer& remote = remote_it->second;
		if (remote.id == 0)
		{
			remote.id = source_peer;
			remote.endpoint = source;
		}
		if (!AcceptSequence(remote, sequence))
			return false;
		remote.last_seen = std::chrono::steady_clock::now();
	}

	const u16 fragment_index = ntohs(header.fragment_index);
	const u16 fragment_count = ntohs(header.fragment_count);
	if (fragment_count == 0 || fragment_count > 2 || fragment_index >= fragment_count)
		return false;

	const ReassemblyKey key{source_peer, ntohl(header.frame_id)};
	auto frame_it = m_reassembly.find(key);
	if (frame_it == m_reassembly.end())
	{
		if (m_reassembly.size() >= 64)
			return false;
		frame_it = m_reassembly.emplace(key, Reassembly{}).first;
	}
	Reassembly& frame = frame_it->second;
	if (frame.fragment_count == 0)
	{
		frame.fragment_count = fragment_count;
		frame.created = std::chrono::steady_clock::now();
	}
	if (frame.fragment_count != fragment_count)
	{
		m_reassembly.erase(key);
		return false;
	}
	const std::size_t offset = fragment_index * MAX_FRAGMENT_PAYLOAD;
	if (offset + payload_size > frame.data.size())
	{
		m_reassembly.erase(key);
		return false;
	}
	std::memcpy(frame.data.data() + offset, datagram.data() + sizeof(WireHeader), payload_size);
	frame.sizes[fragment_index] = payload_size;
	frame.received[fragment_index] = true;
	for (u16 i = 0; i < fragment_count; ++i)
	{
		if (!frame.received[i])
			return false;
	}

	const int frame_size = (fragment_count - 1) * MAX_FRAGMENT_PAYLOAD + frame.sizes[fragment_count - 1];
	std::memcpy(pkt->buffer, frame.data.data(), frame_size);
	m_reassembly.erase(key);
	if (!VerifyLocalLinkPacket(pkt, frame_size))
		return false;
	InspectRecv(pkt);
	return true;
}

void LocalLinkAdapter::RelayDatagram(const void* data, std::size_t size, const sockaddr_in& source)
{
	std::lock_guard socket_lock(m_socket_mutex);
	if (m_closed.load(std::memory_order_acquire))
		return;
	for (const Peer& peer : m_peers)
	{
		if (!SameEndpoint(peer.endpoint, source))
			sendto(m_socket, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
				reinterpret_cast<const sockaddr*>(&peer.endpoint), sizeof(peer.endpoint));
	}
}

LocalLinkAdapter::Peer* LocalLinkAdapter::FindPeer(u32 id, const sockaddr_in& endpoint)
{
	const auto it = std::find_if(m_peers.begin(), m_peers.end(), [&](const Peer& peer) {
		return peer.id == id && SameEndpoint(peer.endpoint, endpoint);
	});
	return it == m_peers.end() ? nullptr : &*it;
}

bool LocalLinkAdapter::RegisterPeer(u32 id, u32 hello_sequence, const sockaddr_in& endpoint)
{
	if (id <= 1)
		return false;
	std::lock_guard lock(m_peer_mutex);
	if (Peer* peer = FindPeer(id, endpoint))
	{
		if (hello_sequence <= peer->highest_sequence)
		{
			peer->highest_sequence = 0;
			peer->replay_window = 0;
		}
		peer->last_seen = std::chrono::steady_clock::now();
		return true;
	}
	const auto same_id = std::find_if(m_peers.begin(), m_peers.end(), [&](const Peer& peer) { return peer.id == id; });
	if (same_id != m_peers.end())
	{
		if (same_id->endpoint.sin_addr.s_addr != endpoint.sin_addr.s_addr)
		{
			Console.Error("DEV9: Local Link rejected duplicate peer id %u", id);
			return false;
		}
		same_id->endpoint = endpoint;
		same_id->highest_sequence = 0;
		same_id->replay_window = 0;
		same_id->last_seen = std::chrono::steady_clock::now();
		return true;
	}
	if (m_peers.size() >= MAX_PEERS)
		return false;
	m_peers.push_back(Peer{endpoint, id, std::chrono::steady_clock::now(), 0, 0});
	Console.WriteLn("DEV9: Local Link peer %u joined", id);
	return true;
}

bool LocalLinkAdapter::VerifyLocalLinkPacket(NetPacket* pkt, int read_size)
{
	const PacketReader::MAC_Address& destination = *reinterpret_cast<PacketReader::MAC_Address*>(&pkt->buffer[0]);
	const PacketReader::MAC_Address& source = *reinterpret_cast<PacketReader::MAC_Address*>(&pkt->buffer[6]);
	if (destination != ps2MAC && destination != broadcastMAC && (destination.bytes[0] & 0x01) == 0)
		return false;
	if (source == ps2MAC)
		return false;
	pkt->size = read_size;
	return true;
}

bool LocalLinkAdapter::AcceptSequence(Peer& peer, u32 sequence)
{
	if (sequence > peer.highest_sequence)
	{
		const u32 shift = sequence - peer.highest_sequence;
		peer.replay_window = shift >= 64 ? 1 : ((peer.replay_window << shift) | 1);
		peer.highest_sequence = sequence;
		return true;
	}
	const u32 delta = peer.highest_sequence - sequence;
	if (delta >= 64 || ((peer.replay_window >> delta) & 1) != 0)
		return false;
	peer.replay_window |= 1ULL << delta;
	return true;
}

void LocalLinkAdapter::PurgeExpiredState()
{
	const auto now = std::chrono::steady_clock::now();
	for (auto it = m_reassembly.begin(); it != m_reassembly.end();)
	{
		if (now - it->second.created > std::chrono::seconds(2))
			it = m_reassembly.erase(it);
		else
			++it;
	}

	if (!m_host)
		return;
	std::lock_guard lock(m_peer_mutex);
	m_peers.erase(std::remove_if(m_peers.begin(), m_peers.end(), [&](const Peer& peer) {
		return now - peer.last_seen > std::chrono::seconds(10);
	}), m_peers.end());
}

bool LocalLinkAdapter::SameEndpoint(const sockaddr_in& lhs, const sockaddr_in& rhs)
{
	return lhs.sin_family == rhs.sin_family && lhs.sin_port == rhs.sin_port &&
		lhs.sin_addr.s_addr == rhs.sin_addr.s_addr;
}

u64 LocalLinkAdapter::DeriveKey(const std::string& room_code, u64 salt)
{
	u64 hash = 1469598103934665603ULL ^ salt;
	for (const unsigned char value : room_code)
	{
		hash ^= value;
		hash *= 1099511628211ULL;
	}
	return hash;
}

u64 LocalLinkAdapter::Authenticate(const void* header, std::size_t header_size,
	const void* payload, std::size_t payload_size, u64 key0, u64 key1)
{
	std::array<u8, sizeof(WireHeader) + MAX_FRAGMENT_PAYLOAD> bytes{};
	std::memcpy(bytes.data(), header, header_size);
	if (payload_size != 0)
		std::memcpy(bytes.data() + header_size, payload, payload_size);
	return SipHash24(bytes.data(), header_size + payload_size, key0, key1);
}
