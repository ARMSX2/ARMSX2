// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/LocalLinkSettings.h"

#include "common/SettingsInterface.h"

#include <cstring>
#include <random>

namespace Catalog
{
	namespace LocalLink
	{
		namespace
		{
			bool IsSpace(char c)
			{
				return c == ' ' || c == '\t' || c == '\r' || c == '\n';
			}

			std::string_view Trim(std::string_view text)
			{
				while (!text.empty() && IsSpace(text.front()))
					text.remove_prefix(1);
				while (!text.empty() && IsSpace(text.back()))
					text.remove_suffix(1);
				return text;
			}

			enum class Parsed
			{
				Ok,
				NotANumber,
				OutOfRange,
			};

			// Plain decimal digits only: no sign, no "0x", no decimals. Ten or more digits cannot be in
			// either range, so the value saturates instead of overflowing.
			Parsed ParseNumber(std::string_view text, u32 min, u32 max, u32* out)
			{
				text = Trim(text);
				if (text.empty())
					return Parsed::NotANumber;

				u64 value = 0;
				for (const char c : text)
				{
					if (c < '0' || c > '9')
						return Parsed::NotANumber;
					value = value * 10 + static_cast<u64>(c - '0');
					if (value > 0xFFFFFFFFull)
						value = 0xFFFFFFFFull;
				}

				if (value < min || value > max)
					return Parsed::OutOfRange;
				*out = static_cast<u32>(value);
				return Parsed::Ok;
			}

			u32 RandomBelow(u32 count)
			{
				static std::random_device s_random;
				return std::uniform_int_distribution<u32>(0, count - 1)(s_random);
			}
		} // namespace

		std::string CleanRoomCode(std::string_view text)
		{
			std::string code;
			for (const char c : text)
			{
				if (c >= 'a' && c <= 'z')
					code.push_back(static_cast<char>(c - 'a' + 'A'));
				else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
					code.push_back(c);
			}
			return IsRoomCodeUsable(code) ? code : std::string();
		}

		bool IsRoomCodeUsable(std::string_view code)
		{
			return code.size() >= ROOM_CODE_MIN_LENGTH && code.size() <= ROOM_CODE_MAX_LENGTH;
		}

		std::string GenerateRoomCode()
		{
			const u32 alphabet_size = static_cast<u32>(std::strlen(ROOM_CODE_ALPHABET));
			std::string code;
			for (u32 i = 0; i < GENERATED_ROOM_CODE_LENGTH; i++)
				code.push_back(ROOM_CODE_ALPHABET[RandomBelow(alphabet_size)]);
			return code;
		}

		u32 GenerateGuestPeerId()
		{
			return GUEST_PEER_ID_MIN + RandomBelow(GUEST_PEER_ID_MAX - GUEST_PEER_ID_MIN + 1);
		}

		Problem Apply(SettingsInterface& si, Field field, std::string_view text)
		{
			switch (field)
			{
				case Field::Address:
				{
					const std::string_view address = Trim(text);
					if (address.size() > ADDRESS_MAX_LENGTH)
						return Problem::AddressTooLong;
					si.SetStringValue(SECTION, KEY_ADDRESS, std::string(address).c_str());
					return Problem::None;
				}

				case Field::Port:
				{
					u32 port;
					switch (ParseNumber(text, PORT_MIN, PORT_MAX, &port))
					{
						case Parsed::NotANumber:
							return Problem::PortNotANumber;
						case Parsed::OutOfRange:
							return Problem::PortOutOfRange;
						case Parsed::Ok:
							break;
					}
					si.SetUIntValue(SECTION, KEY_PORT, port);
					return Problem::None;
				}

				case Field::PeerId:
				{
					u32 peer_id;
					switch (ParseNumber(text, GUEST_PEER_ID_MIN, GUEST_PEER_ID_MAX, &peer_id))
					{
						case Parsed::NotANumber:
							return Problem::PeerIdNotANumber;
						case Parsed::OutOfRange:
							return Problem::PeerIdOutOfRange;
						case Parsed::Ok:
							break;
					}
					si.SetUIntValue(SECTION, KEY_PEER_ID, peer_id);
					return Problem::None;
				}

				case Field::RoomCode:
				{
					const std::string code = CleanRoomCode(text);
					if (code.empty())
						return Problem::RoomCodeLength;
					si.SetStringValue(SECTION, KEY_ROOM_CODE, code.c_str());
					return Problem::None;
				}
			}
			return Problem::None;
		}

		bool SeedDefaults(SettingsInterface& si)
		{
			bool wrote = false;

			if (!IsRoomCodeUsable(si.GetStringValue(SECTION, KEY_ROOM_CODE, "")))
			{
				si.SetStringValue(SECTION, KEY_ROOM_CODE, GenerateRoomCode().c_str());
				wrote = true;
			}

			if (!si.GetBoolValue(SECTION, KEY_HOST, false))
			{
				const u32 peer_id = si.GetUIntValue(SECTION, KEY_PEER_ID, HOST_PEER_ID);
				if (peer_id < GUEST_PEER_ID_MIN || peer_id > GUEST_PEER_ID_MAX)
				{
					si.SetUIntValue(SECTION, KEY_PEER_ID, GenerateGuestPeerId());
					wrote = true;
				}
			}

			return wrote;
		}
	} // namespace LocalLink
} // namespace Catalog
