// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The rules a UI follows when it edits the Local Link settings, checked against the INI the core
// actually reads: values are written through Catalog::LocalLink, saved to a real INI file, loaded
// again and handed to Pcsx2Config::DEV9Options::LoadSave.

#include "Catalog/LocalLinkSettings.h"

#include "catalog_test_support.h"

#include "Config.h"
#include "INISettingsInterface.h"

#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

#include <gtest/gtest.h>

#include <set>

using namespace Catalog::LocalLink;

namespace
{
	// What the core makes of the INI after the file went to disk and came back.
	Pcsx2Config::DEV9Options LoadThroughTheCore(const std::string& ini_path)
	{
		INISettingsInterface reloaded(ini_path);
		EXPECT_TRUE(reloaded.Load());
		Pcsx2Config::DEV9Options options;
		SettingsLoadWrapper wrap(reloaded);
		options.LoadSave(wrap);
		return options;
	}
} // namespace

// ---- The rules ------------------------------------------------------------------------------

TEST(LocalLinkSettingsTest, ApiNameIsTheOneTheCoreWritesAndReads)
{
	EXPECT_STREQ(API_NAME, Pcsx2Config::DEV9Options::NetApiNames[static_cast<int>(Pcsx2Config::DEV9Options::NetApi::LocalLink)]);
}

TEST(LocalLinkSettingsTest, RoomCodesAreUpperCasedAndKeepOnlyLettersAndDigits)
{
	EXPECT_EQ(CleanRoomCode("abcd1234"), "ABCD1234");
	EXPECT_EQ(CleanRoomCode("AbCd"), "ABCD");
	EXPECT_EQ(CleanRoomCode("  my-room 42 "), "MYROOM42");
	EXPECT_EQ(CleanRoomCode("ABCD1234"), "ABCD1234");

	// Anything that is not ASCII is dropped, like the spaces: the core hashes bytes, and the other
	// devices type the same ASCII code.
	EXPECT_EQ(CleanRoomCode("caf\xc3\xa9" "12"), "CAF12");
}

TEST(LocalLinkSettingsTest, RoomCodeLengthLimitsAreFourToTwelveAfterCleaning)
{
	EXPECT_TRUE(CleanRoomCode("abc").empty());
	EXPECT_EQ(CleanRoomCode("abcd"), "ABCD");
	EXPECT_EQ(CleanRoomCode("abcdefghijkl"), "ABCDEFGHIJKL");
	EXPECT_TRUE(CleanRoomCode("abcdefghijklm").empty());

	// The length that counts is the stored one: eight characters with four symbols between is four.
	EXPECT_EQ(CleanRoomCode("a-b-c-d-"), "ABCD");
	EXPECT_TRUE(CleanRoomCode("a-b-c-").empty());
	EXPECT_TRUE(CleanRoomCode("").empty());
	EXPECT_TRUE(CleanRoomCode("----").empty());

	EXPECT_FALSE(IsRoomCodeUsable("ABC"));
	EXPECT_TRUE(IsRoomCodeUsable("ABCD"));
	EXPECT_TRUE(IsRoomCodeUsable("ABCDEFGHIJKL"));
	EXPECT_FALSE(IsRoomCodeUsable("ABCDEFGHIJKLM"));
}

TEST(LocalLinkSettingsTest, GeneratedRoomCodesAreEightCharactersWithoutTheAmbiguousOnes)
{
	std::set<std::string> seen;
	for (int i = 0; i < 2000; i++)
	{
		const std::string code = GenerateRoomCode();
		ASSERT_EQ(code.size(), 8u) << code;
		for (const char c : code)
		{
			const bool letter = (c >= 'A' && c <= 'Z');
			const bool digit = (c >= '2' && c <= '9');
			EXPECT_TRUE(letter || digit) << code;
			EXPECT_NE(c, 'I') << code;
			EXPECT_NE(c, 'O') << code;
		}
		EXPECT_EQ(CleanRoomCode(code), code) << "a generated code must survive cleaning unchanged";
		seen.insert(code);
	}
	// 32^8 possible codes; 2000 draws colliding more than a handful of times would mean the
	// generator is not random.
	EXPECT_GT(seen.size(), 1990u);
}

TEST(LocalLinkSettingsTest, GeneratedGuestPeerIdsStayInTheGuestRange)
{
	u32 lowest = ~0u, highest = 0;
	for (int i = 0; i < 5000; i++)
	{
		const u32 id = GenerateGuestPeerId();
		lowest = std::min(lowest, id);
		highest = std::max(highest, id);
		ASSERT_GE(id, GUEST_PEER_ID_MIN);
		ASSERT_LE(id, GUEST_PEER_ID_MAX);
	}
	EXPECT_LT(lowest, 1000u);
	EXPECT_GT(highest, 64000u);
}

// ---- Apply: what is stored, and what is refused -----------------------------------------------

TEST(LocalLinkSettingsTest, ApplyStoresTheCleanedRoomCode)
{
	MemorySettingsInterface si;
	EXPECT_EQ(Apply(si, Field::RoomCode, "lan party"), Problem::None);
	EXPECT_EQ(si.GetStringValue(SECTION, KEY_ROOM_CODE, ""), "LANPARTY");
}

TEST(LocalLinkSettingsTest, ApplyRefusesABadRoomCodeAndLeavesTheOldOneAlone)
{
	MemorySettingsInterface si;
	ASSERT_EQ(Apply(si, Field::RoomCode, "GOODCODE"), Problem::None);

	EXPECT_EQ(Apply(si, Field::RoomCode, "abc"), Problem::RoomCodeLength);
	EXPECT_EQ(Apply(si, Field::RoomCode, "abcdefghijklm"), Problem::RoomCodeLength);
	EXPECT_EQ(Apply(si, Field::RoomCode, "!!!!!!"), Problem::RoomCodeLength);
	EXPECT_EQ(si.GetStringValue(SECTION, KEY_ROOM_CODE, ""), "GOODCODE");
}

TEST(LocalLinkSettingsTest, ApplyChecksThePortRange)
{
	MemorySettingsInterface si;
	EXPECT_EQ(Apply(si, Field::Port, "1024"), Problem::None);
	EXPECT_EQ(si.GetUIntValue(SECTION, KEY_PORT, 0u), 1024u);
	EXPECT_EQ(Apply(si, Field::Port, " 65535 "), Problem::None);
	EXPECT_EQ(si.GetUIntValue(SECTION, KEY_PORT, 0u), 65535u);

	EXPECT_EQ(Apply(si, Field::Port, "1023"), Problem::PortOutOfRange);
	EXPECT_EQ(Apply(si, Field::Port, "0"), Problem::PortOutOfRange);
	EXPECT_EQ(Apply(si, Field::Port, "65536"), Problem::PortOutOfRange);
	EXPECT_EQ(Apply(si, Field::Port, "99999999999999999999"), Problem::PortOutOfRange);
	EXPECT_EQ(Apply(si, Field::Port, "abc"), Problem::PortNotANumber);
	EXPECT_EQ(Apply(si, Field::Port, "-5"), Problem::PortNotANumber);
	EXPECT_EQ(Apply(si, Field::Port, "19072.5"), Problem::PortNotANumber);
	EXPECT_EQ(Apply(si, Field::Port, ""), Problem::PortNotANumber);
	EXPECT_EQ(si.GetUIntValue(SECTION, KEY_PORT, 0u), 65535u) << "a refused value must not overwrite the stored one";
}

TEST(LocalLinkSettingsTest, ApplyChecksTheGuestPeerIdRange)
{
	MemorySettingsInterface si;
	EXPECT_EQ(Apply(si, Field::PeerId, "2"), Problem::None);
	EXPECT_EQ(si.GetUIntValue(SECTION, KEY_PEER_ID, 0u), 2u);
	EXPECT_EQ(Apply(si, Field::PeerId, "65533"), Problem::None);
	EXPECT_EQ(si.GetUIntValue(SECTION, KEY_PEER_ID, 0u), 65533u);

	EXPECT_EQ(Apply(si, Field::PeerId, "1"), Problem::PeerIdOutOfRange) << "1 belongs to the host";
	EXPECT_EQ(Apply(si, Field::PeerId, "0"), Problem::PeerIdOutOfRange);
	EXPECT_EQ(Apply(si, Field::PeerId, "65534"), Problem::PeerIdOutOfRange);
	EXPECT_EQ(Apply(si, Field::PeerId, "x"), Problem::PeerIdNotANumber);
	EXPECT_EQ(si.GetUIntValue(SECTION, KEY_PEER_ID, 0u), 65533u);
}

TEST(LocalLinkSettingsTest, ApplyStoresTheAddressAsTypedMinusSurroundingSpace)
{
	MemorySettingsInterface si;
	EXPECT_EQ(Apply(si, Field::Address, "  192.168.1.42\n"), Problem::None);
	EXPECT_EQ(si.GetStringValue(SECTION, KEY_ADDRESS, ""), "192.168.1.42");
	EXPECT_EQ(Apply(si, Field::Address, "Host.Example.com"), Problem::None);
	EXPECT_EQ(si.GetStringValue(SECTION, KEY_ADDRESS, ""), "Host.Example.com") << "host names keep their case";

	EXPECT_EQ(Apply(si, Field::Address, std::string(254, 'a')), Problem::AddressTooLong);
	EXPECT_EQ(si.GetStringValue(SECTION, KEY_ADDRESS, ""), "Host.Example.com");
}

// ---- SeedDefaults -----------------------------------------------------------------------------

TEST(LocalLinkSettingsTest, SeedDefaultsGivesAnEmptyConfigARoomCodeAndAGuestPeerId)
{
	MemorySettingsInterface si;
	EXPECT_TRUE(SeedDefaults(si));

	const std::string code = si.GetStringValue(SECTION, KEY_ROOM_CODE, "");
	EXPECT_EQ(code.size(), 8u);
	const u32 peer_id = si.GetUIntValue(SECTION, KEY_PEER_ID, 0u);
	EXPECT_GE(peer_id, GUEST_PEER_ID_MIN);
	EXPECT_LE(peer_id, GUEST_PEER_ID_MAX);
}

TEST(LocalLinkSettingsTest, SeedDefaultsChangesNothingThatIsAlreadyValid)
{
	MemorySettingsInterface si;
	si.SetStringValue(SECTION, KEY_ROOM_CODE, "ROOM42");
	si.SetUIntValue(SECTION, KEY_PEER_ID, 77);
	EXPECT_FALSE(SeedDefaults(si));
	EXPECT_EQ(si.GetStringValue(SECTION, KEY_ROOM_CODE, ""), "ROOM42");
	EXPECT_EQ(si.GetUIntValue(SECTION, KEY_PEER_ID, 0u), 77u);
}

TEST(LocalLinkSettingsTest, SeedDefaultsReplacesAnUnusableRoomCode)
{
	for (const char* bad : {"", "ABC", "ABCDEFGHIJKLM"})
	{
		MemorySettingsInterface si;
		si.SetStringValue(SECTION, KEY_ROOM_CODE, bad);
		si.SetUIntValue(SECTION, KEY_PEER_ID, 5);
		EXPECT_TRUE(SeedDefaults(si)) << bad;
		EXPECT_TRUE(IsRoomCodeUsable(si.GetStringValue(SECTION, KEY_ROOM_CODE, ""))) << bad;
	}
}

TEST(LocalLinkSettingsTest, SeedDefaultsLeavesTheHostsPeerIdAlone)
{
	// The core forces peer 1 for a host, so a host needs no id of its own; and flipping back to a
	// guest later seeds one then.
	MemorySettingsInterface si;
	si.SetBoolValue(SECTION, KEY_HOST, true);
	si.SetStringValue(SECTION, KEY_ROOM_CODE, "ROOM42");
	EXPECT_FALSE(SeedDefaults(si));
	EXPECT_FALSE(si.ContainsValue(SECTION, KEY_PEER_ID));

	si.SetBoolValue(SECTION, KEY_HOST, false);
	EXPECT_TRUE(SeedDefaults(si));
	EXPECT_GE(si.GetUIntValue(SECTION, KEY_PEER_ID, 0u), GUEST_PEER_ID_MIN);
}

TEST(LocalLinkSettingsTest, SeedDefaultsReplacesAGuestIdOutsideTheGuestRange)
{
	for (const u32 bad : {0u, 1u, 65534u, 70000u})
	{
		MemorySettingsInterface si;
		si.SetStringValue(SECTION, KEY_ROOM_CODE, "ROOM42");
		si.SetUIntValue(SECTION, KEY_PEER_ID, bad);
		EXPECT_TRUE(SeedDefaults(si)) << bad;
		const u32 id = si.GetUIntValue(SECTION, KEY_PEER_ID, 0u);
		EXPECT_GE(id, GUEST_PEER_ID_MIN) << bad;
		EXPECT_LE(id, GUEST_PEER_ID_MAX) << bad;
	}
}

// ---- The core's view --------------------------------------------------------------------------

TEST(LocalLinkSettingsTest, CoreDefaultsMatchTheDefaultsTheUiAssumes)
{
	const Pcsx2Config::DEV9Options defaults;
	EXPECT_EQ(defaults.LocalLinkPort, DEFAULT_PORT);
	EXPECT_EQ(defaults.LocalLinkPeerId, HOST_PEER_ID);
	EXPECT_FALSE(defaults.LocalLinkHost);
	EXPECT_TRUE(defaults.LocalLinkAddress.empty());
	EXPECT_TRUE(defaults.LocalLinkRoomCode.empty());
}

TEST(LocalLinkSettingsTest, AGuestConfiguredThroughTheUiReachesTheCoreAsEntered)
{
	CatalogTest::ScopedTempDir dir;
	const std::string ini_path = dir.File("PCSX2.ini");

	{
		INISettingsInterface si(ini_path);
		si.SetBoolValue(SECTION, "EthEnable", true);
		si.SetStringValue(SECTION, KEY_API, API_NAME);
		SeedDefaults(si);
		si.SetBoolValue(SECTION, KEY_HOST, false);
		ASSERT_EQ(Apply(si, Field::Address, " 192.168.1.42 "), Problem::None);
		ASSERT_EQ(Apply(si, Field::Port, "20000"), Problem::None);
		ASSERT_EQ(Apply(si, Field::PeerId, "300"), Problem::None);
		ASSERT_EQ(Apply(si, Field::RoomCode, "lan-party 7"), Problem::None);
		ASSERT_TRUE(si.Save());
	}

	const Pcsx2Config::DEV9Options options = LoadThroughTheCore(ini_path);
	EXPECT_TRUE(options.EthEnable);
	EXPECT_EQ(options.EthApi, Pcsx2Config::DEV9Options::NetApi::LocalLink);
	EXPECT_FALSE(options.LocalLinkHost);
	EXPECT_EQ(options.LocalLinkAddress, "192.168.1.42");
	EXPECT_EQ(options.LocalLinkPort, 20000u);
	EXPECT_EQ(options.LocalLinkPeerId, 300u);
	EXPECT_EQ(options.LocalLinkRoomCode, "LANPARTY7");
}

TEST(LocalLinkSettingsTest, ASeededConfigIsOneTheAdapterAccepts)
{
	// LocalLinkAdapter's constructor needs a 4-12 character room code and clamps the rest; a
	// freshly selected Local Link, with the user having typed nothing, must already pass that.
	CatalogTest::ScopedTempDir dir;
	const std::string ini_path = dir.File("PCSX2.ini");

	{
		INISettingsInterface si(ini_path);
		si.SetBoolValue(SECTION, "EthEnable", true);
		si.SetStringValue(SECTION, KEY_API, API_NAME);
		SeedDefaults(si);
		ASSERT_TRUE(si.Save());
	}

	const Pcsx2Config::DEV9Options options = LoadThroughTheCore(ini_path);
	EXPECT_EQ(options.EthApi, Pcsx2Config::DEV9Options::NetApi::LocalLink);
	EXPECT_TRUE(IsRoomCodeUsable(options.LocalLinkRoomCode));
	EXPECT_EQ(options.LocalLinkPort, DEFAULT_PORT);
	EXPECT_GE(options.LocalLinkPeerId, GUEST_PEER_ID_MIN);
	EXPECT_LE(options.LocalLinkPeerId, GUEST_PEER_ID_MAX);
}
