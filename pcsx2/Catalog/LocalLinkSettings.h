// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The rules for the Local Link settings a UI edits: which values the core accepts, what a UI must do
// to what the user typed before storing it, and a generator for room codes. The core reads them in
// DEV9/LocalLinkAdapter.cpp and the INI layout is Pcsx2Config::DEV9Options::LoadSave; this header
// only decides what a UI may write. Nothing here touches the network, the host or any global, so
// Big Picture and Qt share it and the tests run it against a plain SettingsInterface.
//
// The INI keys, all in [DEV9/Eth]:
//   EthApi              "Local Link" selects the adapter (the other values are the usual ones)
//   LocalLinkHost       bool, default false     true: this device hosts, false: it joins a host
//   LocalLinkAddress    string, default ""      the host's address (joining only): IPv4 or a host name
//   LocalLinkPort       uint 1024..65535, default 19072, the same on every device
//   LocalLinkPeerId     uint, default 1         1 is the host; a guest needs 2..65533, unique per device
//   LocalLinkRoomCode   string, default ""      4..12 characters, the same on every device
//
// What a UI does:
//
//   * Selecting the adapter: set EthApi to Catalog::LocalLink::API_NAME, then SeedDefaults(si), so a
//     room code and a guest peer ID exist before the user has typed anything. An empty room code
//     makes the adapter refuse to start and the game then reports "network adapter not connected"
//     with nothing pointing at the code.
//   * Toggling the host switch: write LocalLinkHost, then SeedDefaults(si) again (a guest needs a
//     valid peer ID, which the default 1 is not).
//   * Text the user typed: Apply(si, field, text). It stores the cleaned value, or leaves the INI
//     alone and returns why; show the reason to the user in the host's own words.
//   * "Generate": GenerateRoomCode() and store it with Apply(si, Field::RoomCode, code).
//
// The room code is upper-cased here and not in the core: the core hashes the bytes as given, so
// "abcd1234" and "ABCD1234" would not reach each other. Android upper-cases in its UI for the same
// reason, so every UI must, or a session mixing devices silently fails to connect.

#include "common/Pcsx2Types.h"

#include <string>
#include <string_view>

class SettingsInterface;

namespace Catalog
{
	namespace LocalLink
	{
		inline constexpr const char* SECTION = "DEV9/Eth";
		inline constexpr const char* KEY_API = "EthApi";
		inline constexpr const char* KEY_HOST = "LocalLinkHost";
		inline constexpr const char* KEY_ADDRESS = "LocalLinkAddress";
		inline constexpr const char* KEY_PORT = "LocalLinkPort";
		inline constexpr const char* KEY_PEER_ID = "LocalLinkPeerId";
		inline constexpr const char* KEY_ROOM_CODE = "LocalLinkRoomCode";

		/// The EthApi value that selects Local Link; equals Pcsx2Config::DEV9Options::NetApiNames[LocalLink].
		inline constexpr const char* API_NAME = "Local Link";

		inline constexpr u32 ROOM_CODE_MIN_LENGTH = 4;
		inline constexpr u32 ROOM_CODE_MAX_LENGTH = 12;
		inline constexpr u32 GENERATED_ROOM_CODE_LENGTH = 8;
		inline constexpr u32 PORT_MIN = 1024;
		inline constexpr u32 PORT_MAX = 65535;
		inline constexpr u32 DEFAULT_PORT = 19072;
		inline constexpr u32 HOST_PEER_ID = 1;
		inline constexpr u32 GUEST_PEER_ID_MIN = 2;
		inline constexpr u32 GUEST_PEER_ID_MAX = 65533;
		inline constexpr size_t ADDRESS_MAX_LENGTH = 253; ///< The longest DNS name.

		/// The characters GenerateRoomCode() draws from: A-Z and 2-9 without I, O, 0, 1 (the
		/// ones that get misread when a code is read aloud). Typed codes may use any letter or digit.
		inline constexpr const char* ROOM_CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

		enum class Field
		{
			Address,
			Port,
			PeerId,
			RoomCode,
		};

		enum class Problem
		{
			None,
			AddressTooLong,
			PortNotANumber,
			PortOutOfRange,
			PeerIdNotANumber,
			PeerIdOutOfRange,
			RoomCodeLength, ///< After cleaning, fewer than 4 or more than 12 characters.
		};

		/// Validates what the user typed for `field` and stores it in [DEV9/Eth] if acceptable.
		/// On any error nothing is written. What is stored:
		///   Address   the text with surrounding white space removed (an IPv4 address or a host name,
		///             not checked further: the core resolves it when the game starts)
		///   Port      the number, 1024..65535
		///   PeerId    the number, 2..65533 (the guest range; the host's 1 is not typed)
		///   RoomCode  upper-cased with everything but A-Z and 0-9 removed, 4..12 characters
		Problem Apply(SettingsInterface& si, Field field, std::string_view text);

		/// The room code as Apply() would store it, or an empty string if it would be rejected.
		std::string CleanRoomCode(std::string_view text);

		/// True if `code` is something the adapter starts with (4..12 characters).
		bool IsRoomCodeUsable(std::string_view code);

		/// A new random room code of GENERATED_ROOM_CODE_LENGTH characters from ROOM_CODE_ALPHABET.
		std::string GenerateRoomCode();

		/// A random peer ID in the guest range, for a device that has not got one yet.
		u32 GenerateGuestPeerId();

		/// Gives the settings what the adapter needs to start: a usable room code if the stored one
		/// is not, and for a guest (LocalLinkHost false) a peer ID in 2..65533 if the stored one is
		/// not. Changes nothing that is already valid, so calling it again is harmless. Returns true
		/// if it wrote anything.
		bool SeedDefaults(SettingsInterface& si);
	} // namespace LocalLink
} // namespace Catalog
