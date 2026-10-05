// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#include "common/Console.h"
#include "ACMACROS.h"
#include "ACJV.h"
#include "ACUART.h"
#include "Config.h"
#include "Host.h"
#include "Input/InputManager.h"
#include "ImGui/ImGuiManager.h"
#include "GS/GS.h"
#include "common/SettingsInterface.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <string>

enum ACJVCMD {
	UNKNOWN = -2, // unknown CMD, should fire up a warning for developer
	NONE = -1,  // Neutral state
	JVS_INIT0, // starts with 26 A3
	JVS_INIT1, // starts with 98 59
	JVS_JVS,   // starts with 6F 3E. this holds an actual JVS packet inside
};

bool ACJV::enabled = false;
void do_acjv_packet();

// R/W arrays are u8 because ACJV is processing (u8) arrays, but the MMIO is performed over (volatile u16*). leaving the higher byte empty
std::array<u8, ACJV_PACKETSIZE> rdbuf; // NAMCO_PCB ---> IOP
std::array<u8, ACJV_PACKETSIZE> wrbuf; // IOP --> NAMCO_PCB
inline u16* rdbuf_getu16() { // NAMCO_PCB ---> IOP
    return reinterpret_cast<u16*>(rdbuf.data());
}
inline const u16* wrbuf_getu16() { // IOP --> NAMCO_PCB
    return reinterpret_cast<const u16*>(wrbuf.data());
}

std::string BOARDS[] = {
	"namco ltd.;RAYS PCB;",
	"namco ltd.;FCA-1;Ver1.01;JPN,Multipurpose",
	"namco ltd.;FCB;Ver1.02;JPN,TouchPanel&Multipurpose",
	"namco ltd.;TSS-I/O;Ver2.11;GUN-EXTENTION",
	"namco ltd.;MIU-I/O;Ver2.05;JPN,GUN-EXTENTION",
	"TAITO CORP.;I/O PCB-24/24/8/2SE;ver1.2forBG3;IN24/OUT24/AD8/DA2/SERIAL/EEPROM", // Battle Gear 3 / Tuned — real Taito board ID from its TMP95C063 firmware dump
};
enum BOARDID ACJV::CurrentBoardID = RAYS_PCB;

static constexpr const char* BOARD_DISPLAY_NAMES[] = {
	"RAYS PCB",
	"FCA-1 (Multipurpose)",
	"FCB (Touch Panel)",
	"TSS-I/O (Gun Extension)",
	"MIU-I/O (Gun Extension)",
	"Taito I/O PCB-24/24/8/2SE (Battle Gear 3)",
};

static constexpr u16 DEFAULT_DIP_SWITCH_STATE =
    (DIPS::VIDEO_VOLTAGE | DIPS::MONITOR_SYNCFREQ | DIPS::VIDEO_SYNC_SPLIT);

static constexpr const std::array<u16, ACJV::NUM_DIP_SWITCHES> s_dip_switch_masks = {{
	DIPS::TESTMODE,
	DIPS::VIDEO_VOLTAGE,
	DIPS::MONITOR_SYNCFREQ,
	DIPS::VIDEO_SYNC_SPLIT,
}};

#include "ACJV_Inputs.h"

static u16 s_dip_switch_state = DEFAULT_DIP_SWITCH_STATE;
static bool s_suppress_daemon = true;
static std::atomic<bool> s_sinden_border_enabled{false};
static std::atomic<int> s_sinden_border_mode{0};
static std::atomic<int> s_sinden_border_thickness{10};
static std::atomic<bool> s_lightgun_link_2p{false};
static std::string s_gameid;

std::span<const ACJV::DIPSwitchInfo> ACJV::GetDIPSwitches()
{
	return s_dip_switch_info;
}

const ACJV::DIPSwitchInfo& ACJV::GetTestModeDIPSwitch()
{
	return s_dip_switch_info[0];
}

const ACJV::DIPSwitchInfo& ACJV::GetVideoVoltageDIPSwitch()
{
	return s_dip_switch_info[1];
}

const ACJV::DIPSwitchInfo& ACJV::GetMonitorSyncFrequencyDIPSwitch()
{
	return s_dip_switch_info[2];
}

const ACJV::DIPSwitchInfo& ACJV::GetVideoSyncSplitDIPSwitch()
{
	return s_dip_switch_info[3];
}

bool ACJV::IsSuppressDaemonEnabled()
{
	return s_suppress_daemon;
}

const char* ACJV::GetBoardDisplayName(BOARDID id)
{
	if (id >= RAYS_PCB && id <= TAITO_BG3_IO_PCB)
		return BOARD_DISPLAY_NAMES[id];
	return "Unknown";
}

BOARDID ACJV::GetCurrentBoardID()
{
	return CurrentBoardID;
}

std::span<const InputBindingInfo> ACJV::GetDIPSwitchBindings()
{
	return s_dip_switch_bindings;
}

static JVS_MODE m_jvsMode = JVS_MODE::DEFAULT;

std::span<const InputBindingInfo> ACJV::GetButtonBindings()
{
	if (m_jvsMode == JVS_MODE::TWINSTICK)
		return s_twinstick_p1_button_bindings;
	return s_jvs_p1_button_bindings;
}

std::span<const InputBindingInfo> ACJV::GetP2ButtonBindings()
{
	// Twin-stick (Zoids) is 1 player per cabinet (versus is networked), so no P2 layout.
	if (m_jvsMode == JVS_MODE::TWINSTICK)
		return {};
	return s_jvs_p2_button_bindings;
}

std::span<const InputBindingInfo> ACJV::GetCoinBindings()
{
	return s_jvs_coin_bindings;
}

std::span<const InputBindingInfo> ACJV::GetWheelBindings()
{
	return s_jvs_wheel_bindings;
}

std::span<const InputBindingInfo> ACJV::GetDrumBindings()
{
	return s_jvs_drum_bindings;
}

std::span<const InputBindingInfo> ACJV::GetTwinstickBindings()
{
	return s_twinstick_p1_button_bindings;
}

bool ACJV::GetDIPSwitchState(u32 index)
{
	return (index < s_dip_switch_masks.size()) && ((s_dip_switch_state & s_dip_switch_masks[index]) != 0);
}

void ACJV::SetDIPSwitchState(u32 index, bool enabled)
{
	if (index >= s_dip_switch_masks.size())
		return;

	const u16 mask = s_dip_switch_masks[index];
	if (enabled)
		s_dip_switch_state |= mask;
	else
		s_dip_switch_state &= ~mask;
}

void ACJV::ToggleDIPSwitchState(u32 index)
{
	if (index >= s_dip_switch_masks.size())
		return;

	const u16 mask = s_dip_switch_masks[index];
	s_dip_switch_state ^= mask;
}

// Fixed JVS switches a macro can fire (configurable without a running game).
static constexpr ACJV::JvsMacroSwitch s_jvs_macro_switches[] = {
	{"Button1", "Button 1", JVS_BTN_1},
	{"Button2", "Button 2", JVS_BTN_2},
	{"Button3", "Button 3", JVS_BTN_3},
	{"Button4", "Button 4", JVS_BTN_4},
	{"Button5", "Button 5", JVS_BTN_5},
	{"Button6", "Button 6", JVS_BTN_6},
};
std::span<const ACJV::JvsMacroSwitch> ACJV::GetMacroSwitches() { return s_jvs_macro_switches; }

// Per-layout macros: config keys carry the layout key (button-name prefix).
std::string ACJV::LayoutKey(std::span<const InputBindingInfo> buttons)
{
	if (buttons.empty())
		return {};
	std::string n(buttons[0].name);
	const size_t pos = n.find('_');
	return (pos == std::string::npos) ? n : n.substr(0, pos);
}
std::string ACJV::GetCurrentLayoutKey()
{
	std::string k = LayoutKey(GetFightingButtons());
	if (k.empty()) k = LayoutKey(GetRacingButtons());
	if (k.empty()) k = LayoutKey(GetStandardButtons());
	return k;
}
std::string ACJV::MacroConfigKey(const std::string& layoutKey, u32 player, u32 index, const char* suffix)
{
	return "Macro_" + layoutKey + "_P" + std::to_string(player + 1) + "_" + std::to_string(index + 1) + suffix;
}

// JVS macros (combo): fire switch bits for one player; masks pushed via SetMacroMask.
struct JvsMacro { u16 mask = 0; bool active = false; };
static JvsMacro s_jvs_macros[JVS_PLAYER_COUNT][ACJV::NUM_JVS_MACROS];
static u16 m_jvsMacroButtonState[JVS_PLAYER_COUNT] = {};
static void RecomputeMacroState(u32 player)
{
	u16 state = 0;
	for (u32 i = 0; i < ACJV::NUM_JVS_MACROS; i++)
		if (s_jvs_macros[player][i].active)
			state |= s_jvs_macros[player][i].mask;
	m_jvsMacroButtonState[player] = state;
}
void ACJV::SetMacroMask(u32 player, u32 index, u16 mask)
{
	if (player >= JVS_PLAYER_COUNT || index >= NUM_JVS_MACROS)
		return;
	s_jvs_macros[player][index] = {mask, false};
	RecomputeMacroState(player);
}

void ACJV::LoadConfig(const SettingsInterface& si)
{
	u16 state = 0;
	for (u32 i = 0; i < s_dip_switch_info.size(); i++)
	{
		const DIPSwitchInfo& dip_switch = s_dip_switch_info[i];
		if (si.GetBoolValue(CONFIG_SECTION, dip_switch.name, dip_switch.default_value))
			state |= s_dip_switch_masks[i];
	}
	s_dip_switch_state = state;
	s_suppress_daemon = si.GetBoolValue("Arcade", "SuppressDaemon", true);
	s_sinden_border_enabled = si.GetBoolValue(CONFIG_SECTION, "SindenBorderEnabled", false);
	s_sinden_border_mode = si.GetIntValue(CONFIG_SECTION, "SindenBorderMode", 0);
	s_sinden_border_thickness = si.GetIntValue(CONFIG_SECTION, "SindenBorderThickness", 10);
	s_lightgun_link_2p = si.GetBoolValue(CONFIG_SECTION, "LightgunLinkAs2P", false);
}

void ACJV::CopyConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si, bool copy_settings, bool copy_bindings)
{
	if (copy_settings)
	{
		for (const DIPSwitchInfo& dip_switch : s_dip_switch_info)
			dest_si->CopyBoolValue(src_si, CONFIG_SECTION, dip_switch.name);
		dest_si->CopyBoolValue(src_si, "Arcade", "SuppressDaemon");
		dest_si->CopyBoolValue(src_si, CONFIG_SECTION, "SindenBorderEnabled");
		dest_si->CopyIntValue(src_si, CONFIG_SECTION, "SindenBorderMode");
		dest_si->CopyIntValue(src_si, CONFIG_SECTION, "SindenBorderThickness");
		dest_si->CopyFloatValue(src_si, CONFIG_SECTION, "AnalogDeadzone");
		dest_si->CopyFloatValue(src_si, CONFIG_SECTION, "AnalogSensitivity");
		dest_si->CopyFloatValue(src_si, CONFIG_SECTION, "TriggerDeadzone");
		dest_si->CopyBoolValue(src_si, CONFIG_SECTION, "InvertSteering");
		dest_si->CopyBoolValue(src_si, CONFIG_SECTION, "LightgunLinkAs2P");
	}

	if (copy_bindings)
	{
		for (const DIPSwitchInfo& dip_switch : s_dip_switch_info)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, dip_switch.toggle_bind_name);
		for (const InputBindingInfo& bi : s_jvs_p1_button_bindings)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, bi.name);
		for (const InputBindingInfo& bi : s_jvs_p2_button_bindings)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, bi.name);
		for (const InputBindingInfo& bi : s_jvs_coin_bindings)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, bi.name);
		// Peripheral binds (drum/wheel/twinstick) have no pad-mirror fallback, so they must travel with a profile too.
		for (const InputBindingInfo& bi : s_jvs_drum_bindings)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, bi.name);
		for (const InputBindingInfo& bi : s_jvs_wheel_bindings)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, bi.name);
		for (const InputBindingInfo& bi : s_twinstick_p1_button_bindings)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, bi.name);
		// Per-layout hub binds use {base}_P1/_P2 keys (racing: _P1 only, 1 player).
		for (const LayoutInfo& fl : s_fighting_layout_ui)
			for (const InputBindingInfo& bi : fl.buttons)
			{
				dest_si->CopyStringListValue(src_si, CONFIG_SECTION, (std::string(bi.name) + "_P1").c_str());
				dest_si->CopyStringListValue(src_si, CONFIG_SECTION, (std::string(bi.name) + "_P2").c_str());
			}
		for (const LayoutInfo& sl : s_standard_layout_ui)
			for (const InputBindingInfo& bi : sl.buttons)
			{
				dest_si->CopyStringListValue(src_si, CONFIG_SECTION, (std::string(bi.name) + "_P1").c_str());
				dest_si->CopyStringListValue(src_si, CONFIG_SECTION, (std::string(bi.name) + "_P2").c_str());
			}
		for (const RacingLayoutInfo& rl : s_racing_layout_ui)
			for (const InputBindingInfo& bi : rl.buttons)
				dest_si->CopyStringListValue(src_si, CONFIG_SECTION, (std::string(bi.name) + "_P1").c_str());
		const auto copy_macros = [&](std::span<const InputBindingInfo> buttons) {
			const std::string lk = LayoutKey(buttons);
			if (lk.empty())
				return;
			for (u32 p = 0; p < JVS_PLAYER_COUNT; p++)
				for (u32 i = 0; i < NUM_JVS_MACROS; i++)
				{
					dest_si->CopyStringListValue(src_si, CONFIG_SECTION, MacroConfigKey(lk, p, i, "").c_str());
					dest_si->CopyStringListValue(src_si, CONFIG_SECTION, MacroConfigKey(lk, p, i, "Binds").c_str());
				}
		};
		for (const LayoutInfo& fl : s_fighting_layout_ui) copy_macros(fl.buttons);
		for (const RacingLayoutInfo& rl : s_racing_layout_ui)     copy_macros(rl.buttons);
		for (const LayoutInfo& sl : s_standard_layout_ui) copy_macros(sl.buttons);
	}
}

void ACJV::SetDefaultConfiguration(SettingsInterface& si)
{
	si.ClearSection(CONFIG_SECTION);
	for (const DIPSwitchInfo& dip_switch : s_dip_switch_info)
		si.SetBoolValue(CONFIG_SECTION, dip_switch.name, dip_switch.default_value);
	si.SetBoolValue("Arcade", "SuppressDaemon", true);
	si.SetBoolValue(CONFIG_SECTION, "SindenBorderEnabled", false);
	si.SetIntValue(CONFIG_SECTION, "SindenBorderMode", 0);
	si.SetIntValue(CONFIG_SECTION, "SindenBorderThickness", 10);
}

// The game reading the JVS board: return the requested word from its read buffer (rdbuf).
u16 ACJV::Read16(u32 addr) {
    if (addr >= ACJV_RDBASE && addr < 0x124045FE) {
        int x = (addr - ACJV_RDBASE)/2;
        // El_isra's initial-polling scaffold, disabled (tested OK without it):
        // if (x == 2 || x == 3 || x == 4) return rdbuf.at(x)|1;// initial polling expects these addrs to not be zero
        return (u16)rdbuf.at(x);
    } else if ((addr == 0x124045FE)) {
		return (u16)rdbuf.at((addr - ACJV_RDBASE)/2);
	}
	return 0;
}

void ACJV::Write16(u32 addr, u16 val) {
    if (addr >= ACJV_WRBASE && addr < 0x12404BFE) { //0x124048FE
        u32 x = (addr - ACJV_WRBASE)/2;
        wrbuf[x] = val;
    } else if (addr == 0x12404BFE) {
       wrbuf[(addr -  ACJV_WRBASE)/2] = val;
		do_acjv_packet();
	}
}

#define JVS_ASSERT(x) if (!(x)) Console.WriteLn("## ASSERT ## %s:%s:%d %s", __FILE__, __FUNCTION__, __LINE__, #x);

// JVS bus state — volatile runtime values, reset on game switch (see SetGameId)

u16 ACJV::coin[2] = {0, 0};
static u16 m_jvsSystemButtonState = 0;
// ARMSX2: atomic, because the Android pad bridge sets these from the input thread while the CPU thread
// sets the gun sensor bit in the same word (UpdateLightgunFromMouse); a plain |= / &= from both could
// drop either change.
static std::atomic<u16> m_jvsButtonState[JVS_PLAYER_COUNT] = {};
static u8 m_testButtonState = 0;
static u16 m_jvsScreenPosX[JVS_PLAYER_COUNT] = {};
static u16 m_jvsScreenPosY[JVS_PLAYER_COUNT] = {};
static float m_jvsLightgunDX[JVS_PLAYER_COUNT] = {-1.0f, -1.0f};  // per-player normalized display X (-1 = off-screen)
static float m_jvsLightgunDY[JVS_PLAYER_COUNT] = {-1.0f, -1.0f};  // per-player normalized display Y (-1 = off-screen)
static u16 m_jvsWheelChannels[JVS_WHEEL_CHANNEL_MAX] = {};
static u16 m_jvsDrumChannels[JVS_DRUM_CHANNEL_MAX] = {};

static float m_wheelSteerR = 0.0f; // stick right  -> steering positive
static float m_wheelSteerL = 0.0f; // stick left   -> steering negative
static float m_wheelGas    = 0.0f; // right trigger (R2)
static float m_wheelBrake  = 0.0f; // left trigger  (L2)

// Per-game JVS button mapping for lightgun games, keyed by NM game ID (see issue #9).
// Field order: pedal, sensor, sensor_active_high, p1_start, p2_start, p1_trigger, p2_trigger, board, link_2p
// Each button value is a JVS bit from JVSButton enum. 0 = not used for this game.
// link_2p: "LINK AS" cabinet side switch bit the game polls (TC3 reads Push3 as MIU-I/O, TC4 reads Push4).
static const GunMapping s_default_gun_mapping = {JVS_BTN_3, JVS_BTN_RIGHT, false, 0, 0, JVS_BTN_2, 0, GunBoardModel::Classic};
static const std::map<std::string, GunMapping> s_gun_mappings = {
	{"NM00003", {0,            0x200,         true,  JVS_BTN_3,  JVS_BTN_6, JVS_BTN_2,    JVS_BTN_5, GunBoardModel::CameraVN}},      // Vampire Night
	{"NM00012", {JVS_BTN_6,    0,             false, 0,          0,          JVS_BTN_2,    0,         GunBoardModel::TwoTierTC3, JVS_BTN_3}},    // Time Crisis 3
	{"NM00021", {JVS_BTN_3,    JVS_BTN_RIGHT, false, 0,          0,          JVS_BTN_LEFT, 0,         GunBoardModel::Classic}},       // Cobra The Arcade
	{"NM00032", {JVS_BTN_3,    JVS_BTN_RIGHT, false, 0,          0,          JVS_BTN_LEFT, 0,         GunBoardModel::SideSwitchTC4, JVS_BTN_4}}, // Time Crisis 4
};
static const GunMapping* m_gunMapping = &s_default_gun_mapping;

static const std::map<std::string, FightingLayout> s_fighting_layouts = {
	{"NM00004", FightingLayout::TEKKEN},     // Tekken 4
	{"NM00019", FightingLayout::TEKKEN},     // Tekken 5 / 5.1
	{"NM00026", FightingLayout::TEKKEN},     // Tekken 5 DR
	{"NM00007", FightingLayout::SOULCAL},    // Soul Calibur II
	{"NM00031", FightingLayout::SOULCAL},    // Soul Calibur III
	{"NM00002", FightingLayout::BLOODYROAR}, // Bloody Roar 3
	{"NM00048", FightingLayout::FATE},       // Fate Unlimited Codes
	{"NM00027", FightingLayout::SDBZ},       // Super Dragon Ball Z
	{"NM00029", FightingLayout::KINNIKUMAN}, // Kinnikuman MGP 1
	{"NM00035", FightingLayout::YUYU},       // YuYu Hakusho: Deathmatch
	{"NM00040", FightingLayout::KINNIKUMAN}, // Kinnikuman MGP 2
	{"NM00011", FightingLayout::PRIDEGP},    // Pride GP 2003
	{"NM00018", FightingLayout::SIX_BUTTON}, // Capcom Fighting Jam
	{"NM00042", FightingLayout::BASARA},     // Sengoku Basara X
	{"NM00013", FightingLayout::GUNDAM},     // Z-Gundam: A.E.U.G. vs Titans
	{"NM00017", FightingLayout::GUNDAM},     // Z-Gundam: A.E.U.G. vs Titans DX
	{"NM00024", FightingLayout::GUNDAM},     // Gundam SEED: Federation vs Z.A.F.T.
	{"NM00034", FightingLayout::GUNDAM},     // Gundam SEED Destiny: Federation vs Z.A.F.T. II
	{"NM00043", FightingLayout::GUNDAM},     // Gundam vs Gundam
	{"NM00052", FightingLayout::GUNDAM},     // Gundam vs Gundam NEXT
};

static const std::map<std::string, RacingLayout> s_racing_layouts = {
	{"NM00047", RacingLayout::UNIVERSAL},   // Ace Driver 3: Final Turn
	{"NM00010", RacingLayout::BG3},         // Battle Gear 3
	{"NM00015", RacingLayout::BG3},         // Battle Gear 3 Tuned
	{"NM00008", RacingLayout::UNIVERSAL},   // Wangan Midnight
	{"NM00005", RacingLayout::UNIVERSAL},   // Wangan Midnight R
	{"NM00001", RacingLayout::UNIVERSAL},   // Ridge Racer V
	{"NM00039", RacingLayout::UNIVERSAL},   // MotoGP
};

static const std::map<std::string, StandardLayout> s_standard_layouts = {
	{"NM00009", StandardLayout::BASEBALL},    // Netchu Pro Baseball 2002
	{"NM00006", StandardLayout::SMASHCOURT},  // Smash Court Pro Tournament
	{"NM10003", StandardLayout::TECHNICBEAT}, // Technic Beat (unique unofficial gameid; NM00003 = Vampire Night, GameIndex PR #92)
	{"NM00030", StandardLayout::GUNDAMQUIZ},  // Gundam Quiz Warrior (moved from Fighting: a quiz, not a fighter)
	{"NM00037", StandardLayout::INUFUKU},     // Quiz Suku Suku Inufuku 2
};

// Drum (Taiko) and twin-stick (Zoids) gameids for ResolveModeFromGameId (no per-button table).
static constexpr const char* s_drum_games[] = {
	"NM00023", "NM00033", "NM00038", "NM00041", "NM00044", "NM00045",
	"NM00046", "NM00051", "NM00053", "NM00054", "NM00056", "NM00057",
};
static constexpr const char* s_twinstick_games[] = {"NM00016", "NM00025"};
static constexpr const char* s_touch_games[] = { // V290 FCB touch panel games
	"NM00014", // Dragon Chronicle
	"NM00020", // Dragon Chronicle Online
	"NM00022", // THE IDOLM@STER
	"NM00028", // Druaga Online
	"NM00036", // Zenno Training (Whole Brain)
};

// Derive the JVS device mode from the gameid alone (jvsmode= is an optional override, see VMManager).
JVS_MODE ACJV::ResolveModeFromGameId(const std::string& gameid)
{
	if (s_racing_layouts.count(gameid))   return JVS_MODE::DRIVE;
	if (s_fighting_layouts.count(gameid)) return JVS_MODE::FIGHTING;
	if (s_standard_layouts.count(gameid)) return JVS_MODE::STANDARD;
	if (s_gun_mappings.count(gameid))     return JVS_MODE::LIGHTGUN;
	if (std::ranges::find(s_drum_games, gameid) != std::ranges::end(s_drum_games))
		return JVS_MODE::DRUM;
	if (std::ranges::find(s_twinstick_games, gameid) != std::ranges::end(s_twinstick_games))
		return JVS_MODE::TWINSTICK;
	if (std::ranges::find(s_touch_games, gameid) != std::ranges::end(s_touch_games))
		return JVS_MODE::TOUCH;
	return JVS_MODE::DEFAULT;
}

std::span<const InputBindingInfo> ACJV::GetFightingButtons()
{
	return GetFightingButtons(s_gameid);
}

std::span<const InputBindingInfo> ACJV::GetFightingButtons(const std::string& gameid)
{
	auto it = s_fighting_layouts.find(gameid);
	if (it == s_fighting_layouts.end())
		return {};
	switch (it->second)
	{
		case FightingLayout::TEKKEN:     return s_fight_tekken;
		case FightingLayout::SOULCAL:    return s_fight_soulcal;
		case FightingLayout::SIX_BUTTON: return s_fight_sixbutton;
		case FightingLayout::GUNDAM:     return s_fight_gundam;
		case FightingLayout::BLOODYROAR: return s_fight_bloodyroar;
		case FightingLayout::FATE:       return s_fight_fate;
		case FightingLayout::KINNIKUMAN: return s_fight_kinnikuman;
		case FightingLayout::PRIDEGP:    return s_fight_pridegp;
		case FightingLayout::BASARA:     return s_fight_basara;
		case FightingLayout::SDBZ:       return s_fight_sdbz;
		case FightingLayout::YUYU:       return s_fight_yuyu;
	}
	return {};
}

std::span<const ACJV::LayoutInfo> ACJV::GetFightingLayouts()
{
	return s_fighting_layout_ui;
}

std::span<const InputBindingInfo> ACJV::GetStandardButtons()
{
	return GetStandardButtons(s_gameid);
}

std::span<const InputBindingInfo> ACJV::GetStandardButtons(const std::string& gameid)
{
	auto it = s_standard_layouts.find(gameid);
	if (it == s_standard_layouts.end())
		return {};
	switch (it->second)
	{
		case StandardLayout::BASEBALL:    return s_standard_baseball;
		case StandardLayout::SMASHCOURT:  return s_standard_smashcourt;
		case StandardLayout::TECHNICBEAT: return s_standard_technicbeat;
		case StandardLayout::GUNDAMQUIZ:  return s_standard_gundamquiz;
		case StandardLayout::INUFUKU:     return s_standard_inufuku;
	}
	return {};
}

std::span<const ACJV::LayoutInfo> ACJV::GetStandardLayouts()
{
	return s_standard_layout_ui;
}

std::span<const InputBindingInfo> ACJV::GetRacingButtons()
{
	return GetRacingButtons(s_gameid);
}

std::span<const InputBindingInfo> ACJV::GetRacingButtons(const std::string& gameid)
{
	auto it = s_racing_layouts.find(gameid);
	if (it == s_racing_layouts.end())
		return {};
	switch (it->second)
	{
		case RacingLayout::UNIVERSAL: return s_race_universal;
		case RacingLayout::BG3:       return s_race_bg3;
	}
	return {};
}

std::span<const ACJV::RacingLayoutInfo> ACJV::GetRacingLayouts()
{
	return s_racing_layout_ui;
}

// Gamepad input -> JVS button state: set or clear a button bit for a player
void ACJV::SetButtonState(u32 player, u16 mask, bool pressed)
{
	if (player >= JVS_PLAYER_COUNT)
		return;
	if (pressed)
		m_jvsButtonState[player].fetch_or(mask, std::memory_order_relaxed);
	else
		m_jvsButtonState[player].fetch_and(static_cast<u16>(~mask), std::memory_order_relaxed);
}

void ACJV::SetMacroState(u32 player, u32 index, bool active)
{
	if (player >= JVS_PLAYER_COUNT || index >= NUM_JVS_MACROS)
		return;
	s_jvs_macros[player][index].active = active;
	RecomputeMacroState(player);
}

// Gamepad coin button -> increment JVS coin counter for P1 (slot 0) or P2 (slot 1)
void ACJV::InsertCoin(u32 slot)
{
	if (slot == 0)
		ACJV::coin[0]++;
	else if (slot == 1)
		ACJV::coin[1]++;
}

void ACJV::SetMode(JVS_MODE mode)
{
	m_jvsMode = mode;
}

void ACJV::SetWheelAxis(u32 axis, float value)
{
	switch (axis)
	{
		case 0: m_wheelSteerR = value; break;
		case 1: m_wheelSteerL = value; break;
		case 2: m_wheelGas    = value; break;
		case 3: m_wheelBrake  = value; break;
		default: break;
	}
}

void ACJV::SetDrumHit(u32 channel, bool pressed)
{
	if (channel < JVS_DRUM_CHANNEL_MAX)
		m_jvsDrumChannels[channel] = pressed ? 0xFFFF : 0; // max -> above IN/DAI; big notes (大) emerge from hitting both sides
}

JVS_MODE ACJV::GetMode()
{
	return m_jvsMode;
}

// Host steering, -1 (full left)...+1 (full right). GetGas/GetBrake: 0...1.
float ACJV::GetSteer()
{
	return std::clamp(m_wheelSteerR - m_wheelSteerL, -1.0f, 1.0f);
}

float ACJV::GetGas()   { return std::clamp(m_wheelGas,   0.0f, 1.0f); }
float ACJV::GetBrake() { return std::clamp(m_wheelBrake, 0.0f, 1.0f); }

bool ACJV::IsSindenBorderEnabled()
{
	return s_sinden_border_enabled;
}

int ACJV::GetSindenBorderMode()
{
	return s_sinden_border_mode;
}

int ACJV::GetSindenBorderThickness()
{
	return s_sinden_border_thickness;
}

void ACJV::SetScreenPos(u16 x, u16 y)
{
	m_jvsScreenPosX[0] = x;
	m_jvsScreenPosY[0] = y;
}

// Called from VMManager on game boot. Resets all JVS state and selects per-game I/O config.
const std::string& ACJV::GetGameId() { return s_gameid; }

void ACJV::SetGameId(const std::string& gameid)
{
	s_gameid = gameid;
	// Clean slate: zero all input state on game switch within the emulator
	ACUART::SetupGameHandler(gameid);
	ACJV::coin[0] = 0;
	ACJV::coin[1] = 0;
	m_jvsButtonState[0].store(0, std::memory_order_relaxed);
	m_jvsButtonState[1].store(0, std::memory_order_relaxed);
	m_jvsSystemButtonState = 0;
	m_testButtonState = 0;
	for (u32 p = 0; p < JVS_PLAYER_COUNT; p++)
	{
		m_jvsScreenPosX[p] = 0;
		m_jvsScreenPosY[p] = 0;
		m_jvsLightgunDX[p] = -1.0f;
		m_jvsLightgunDY[p] = -1.0f;
	}
	std::memset(m_jvsWheelChannels, 0, sizeof(m_jvsWheelChannels));
	std::memset(m_jvsDrumChannels, 0, sizeof(m_jvsDrumChannels));
	for (u32 p = 0; p < JVS_PLAYER_COUNT; p++) // clear macro state; InputManager repushes masks on the input reload
	{
		m_jvsMacroButtonState[p] = 0;
		for (u32 i = 0; i < NUM_JVS_MACROS; i++)
			s_jvs_macros[p][i].active = false;
	}

	// Select per-game gun mapping, or fall back to default
	auto it = s_gun_mappings.find(gameid);
	if (it != s_gun_mappings.end())
	{
		m_gunMapping = &it->second;
		Console.WriteLn("ACJV: gun mapping for %s: p1_trigger=0x%04X pedal=0x%04X sensor=0x%04X", gameid.c_str(), it->second.p1_trigger, it->second.pedal, it->second.sensor);
	}
	else
		m_gunMapping = &s_default_gun_mapping;

	// Log the running game's layout (buttons come from GetFighting/Racing/StandardButtons).
	if (const std::string lk = GetCurrentLayoutKey(); !lk.empty())
		Console.WriteLn("ACJV: layout for %s: %s", gameid.c_str(), lk.c_str());

	// TC3 has 3 I/O boards: TSS-I/O (white flash), MIU-I/O (640x224), RAYS PCB (0xFFFF).
	// MIU-I/O chosen: no flash artifact, calibration uses JVS trigger debounce directly.
	// RAYS PCB calibration uses DMA protocol (cmd 0x70) that bypasses our JVS handler.
	if (gameid == "NM00012")
		CurrentBoardID = MIU_IO_JPN_GUN_EXTENTI;
	else if (gameid == "NM00010" || gameid == "NM00015")
		CurrentBoardID = TAITO_BG3_IO_PCB; // BG3/BG3T: real Taito K91X0951A board ID (from the F22 EPROM dump)
	else if (std::ranges::find(s_touch_games, gameid) != std::ranges::end(s_touch_games))
		CurrentBoardID = FCB_JPN_TOUCHPANEL; // touch games use the V290 FCB PCB
	else
		CurrentBoardID = RAYS_PCB;
}

const GunMapping& ACJV::GetGunMapping()
{
	return *m_gunMapping;
}

// Per-player lightgun aim source: shared mouse by default, or the player's own controller stick when its
// Aim Device is set to the pad (GunCon2 has_relative_binds pushes that stick's screen pos here).
static bool s_gunAimJoystick[JVS_PLAYER_COUNT] = {};
static u32 s_gunPointerIndex[JVS_PLAYER_COUNT] = {};
static float s_gunRelativeDX[JVS_PLAYER_COUNT] = {-1.0f, -1.0f};
static float s_gunRelativeDY[JVS_PLAYER_COUNT] = {-1.0f, -1.0f};
void ACJV::SetGunAimSource(u32 player, bool joystick) { if (player < JVS_PLAYER_COUNT) s_gunAimJoystick[player] = joystick; }
void ACJV::SetGunPointerIndex(u32 player, u32 index)
{
	if (player < JVS_PLAYER_COUNT && index < InputManager::MAX_POINTER_DEVICES)
		s_gunPointerIndex[player] = index;
}
void ACJV::SetGunRelativeAim(u32 player, float dx, float dy)
{
	if (player < JVS_PLAYER_COUNT) { s_gunRelativeDX[player] = dx; s_gunRelativeDY[player] = dy; }
}
bool ACJV::GetGunStickAim(u32 player, float* dx, float* dy)
{
	if (player >= JVS_PLAYER_COUNT || !s_gunAimJoystick[player] || s_gunRelativeDX[player] < 0.0f || s_gunRelativeDY[player] < 0.0f)
		return false;
	*dx = s_gunRelativeDX[player];
	*dy = s_gunRelativeDY[player];
	return true;
}

// Namco camera-gun geometry, measured in the VPNGAME binary (GunMgrClass::adjustVal_cz):
// the visible picture spans +-230x+-140 of a +-320x+-224 acceptance window, so the picture
// maps to the middle 71.875%/62.5% of the reported range. The ring around it is the
// aim-beside-the-screen area, and the board reports 0xFFFF/0xFFFF when the camera loses
// the screen entirely.
static constexpr float GUN_CAM_VISIBLE_X = 230.0f / 320.0f;
static constexpr float GUN_CAM_VISIBLE_Y = 140.0f / 224.0f;
static u16 s_gunRawX[JVS_PLAYER_COUNT] = {0xFFFF, 0xFFFF};
static u16 s_gunRawY[JVS_PLAYER_COUNT] = {0xFFFF, 0xFFFF};
static bool s_gunForceOff = false;
void ACJV::SetGunForceOffscreen(bool held) { s_gunForceOff = held; }
static float s_gunContour = 0.01f;
void ACJV::SetGunOffscreenContour(float fraction) { s_gunContour = std::clamp(fraction, 0.0f, 0.05f); }

static void UpdateLightgunFromMouse()
{
	constexpr float edge_margin = 0.01f;
	const auto& gm = ACJV::GetGunMapping();
	const bool camera = (gm.board == GunBoardModel::CameraVN);
	const bool side_switch = (gm.board == GunBoardModel::SideSwitchTC4);
	const bool two_tier = (gm.board == GunBoardModel::TwoTierTC3);
	const bool unclamped = camera || side_switch || two_tier;
	const bool raw_input = false; // ARMSX2: no per-device raw mice; every gun aims through pointer 0 (touch)
	for (u32 p = 0; p < JVS_PLAYER_COUNT; p++)
	{
		float mdx = -1.0f, mdy = -1.0f, udx = -1.0f, udy = -1.0f;
		const auto& [mx, my] = InputManager::GetPointerAbsolutePosition(raw_input ? s_gunPointerIndex[p] : 0);
		GSTranslateWindowToDisplayCoordinates(mx, my, &mdx, &mdy);
		if (unclamped)
			GSTranslateWindowToDisplayCoordinatesUnclamped(mx, my, &udx, &udy);
		const float dx = s_gunAimJoystick[p] ? s_gunRelativeDX[p] : (unclamped ? udx : mdx);
		const float dy = s_gunAimJoystick[p] ? s_gunRelativeDY[p] : (unclamped ? udy : mdy);
		if (camera)
		{
			const float fx = 0.5f + (dx - 0.5f) * GUN_CAM_VISIBLE_X;
			const float fy = 0.5f + (0.5f - dy) * GUN_CAM_VISIBLE_Y; //reported Y is bottom-up
			// Workaround for the PCSX2 pointer limitation: the outer band of the picture counts as
			// aiming beside the screen, so the native reload (invalid position -> reloadExe) stays reachable.
			const float contour = s_gunContour;
			const bool in_aim_area = (dx >= contour && dx <= 1.0f - contour && dy >= contour && dy <= 1.0f - contour);
			const bool lost = s_gunForceOff || !in_aim_area || (fx < 0.0f || fx > 1.0f || fy < 0.0f || fy > 1.0f);
			s_gunRawX[p] = lost ? 0xFFFF : static_cast<u16>(std::clamp(fx * 65535.0f, 1.0f, 65534.0f));
			s_gunRawY[p] = lost ? 0xFFFF : static_cast<u16>(std::clamp(fy * 65535.0f, 1.0f, 65534.0f));
			if (s_gunRawX[p] == 0x7FFF) s_gunRawX[p] = 0x7FFE; //0x7FFF is skipped by the game's adjust sampler
			if (s_gunRawY[p] == 0x7FFF) s_gunRawY[p] = 0x7FFE;
			if (gm.sensor) //the camera still sees the screen from the overscan ring
				ACJV::SetButtonState(p, gm.sensor, gm.sensor_active_high ? !lost : lost);
			continue;
		}
		if (two_tier)
		{
			// TC3's two offscreen tiers (TC3LOAD FUN_0019c800): a coord past [0,640]x[0,448] = yellow
			// reload, the 0xFFFF/0xFFFF sentinel = red gun-lost.
			const float overshoot = std::max({0.0f, -dx, dx - 1.0f, -dy, dy - 1.0f});
			constexpr float LOST = 0.35f; // empirical guess, not real value
			if (overshoot > LOST)
			{
				s_gunRawX[p] = 0xFFFF;
				s_gunRawY[p] = 0xFFFF;
			}
			else
			{
				const int px = static_cast<int>(std::lround(dx * 640.0f));
				const int py = static_cast<int>(std::lround((1.0f - dy) * 448.0f)); //reported Y is bottom-up, native 0..448
				s_gunRawX[p] = static_cast<u16>(static_cast<s16>(std::clamp(px, -32767, 32767)));
				s_gunRawY[p] = static_cast<u16>(static_cast<s16>(std::clamp(py, -32767, 32767)));
			}
			continue;
		}
		if (side_switch)
		{
			// TSS-I/O reports the gun position clamped to its field edge plus a separate offscreen
			// flag; it never zeroes the coordinate (verified in TC4LOAD's JVS parse), so aiming past
			// a side still tells the game which way to switch screens.
			const float cx = std::clamp(dx, 0.0f, 1.0f);
			const float cy = std::clamp(dy, 0.0f, 1.0f);
			const bool inside = (dx >= 0.0f && dy >= 0.0f && dx <= 1.0f && dy <= 1.0f);
			m_jvsScreenPosX[p] = static_cast<u16>((1.0f - cx) * 0xFFFF);
			m_jvsScreenPosY[p] = static_cast<u16>(cy * 0xFFFF);
			m_jvsLightgunDX[p] = inside ? dx : -1.0f;
			m_jvsLightgunDY[p] = inside ? dy : -1.0f;
			if (gm.sensor)
				ACJV::SetButtonState(p, gm.sensor, gm.sensor_active_high ? inside : !inside);
			continue;
		}
		const bool on_screen = (dx >= 0.0f && dy >= 0.0f && dx < (1.0f - edge_margin) && dy < (1.0f - edge_margin));
		if (on_screen)
		{
			m_jvsLightgunDX[p] = dx;
			m_jvsLightgunDY[p] = dy;
			m_jvsScreenPosX[p] = static_cast<u16>((1.0f - dx) * 0xFFFF);
			m_jvsScreenPosY[p] = static_cast<u16>(dy * 0xFFFF);
		}
		else
		{
			m_jvsLightgunDX[p] = -1.0f;
			m_jvsLightgunDY[p] = -1.0f;
			m_jvsScreenPosX[p] = 0;
			m_jvsScreenPosY[p] = 0;
		}
		if (gm.sensor)
			ACJV::SetButtonState(p, gm.sensor, gm.sensor_active_high ? on_screen : !on_screen);
	}
}

static bool m_touchPressed = false;
static bool m_touchPressBound = false;
static bool m_touchRelActive = false;
static float m_touchRel[4] = {}; // Left/Right/Up/Down stick deflection
static std::string m_touchCursorPath;

void ACJV::SetTouchPressed(bool pressed) { m_touchPressed = pressed; }
void ACJV::SetTouchPressBound(bool bound) { m_touchPressBound = bound; }
void ACJV::SetTouchRelativeAxis(u32 axis, float value) { if (axis < std::size(m_touchRel)) m_touchRel[axis] = value; }
void ACJV::SetTouchRelativeActive(bool active) { m_touchRelActive = active; }

// Relative aim draws on its own software-cursor slot (past the guns' MAX+port slots); the mouse shares slot 0.
static u32 TouchPointerIndex() { return m_touchRelActive ? (InputManager::MAX_POINTER_DEVICES + 2) : 0; }

void ACJV::SetTouchCursor(std::string path, float scale, u32 color)
{
	static bool s_shown = false;
	static u32 s_prev_index = 0;
	const bool want = (ACJV::GetMode() == JVS_MODE::TOUCH) && !path.empty();
	const u32 index = TouchPointerIndex();
	if (s_shown && (!want || s_prev_index != index))
		ImGuiManager::ClearSoftwareCursor(s_prev_index);
	m_touchCursorPath = want ? path : std::string();
	if (want)
		ImGuiManager::SetSoftwareCursor(index, std::move(path), scale, color);
	s_shown = want;
	s_prev_index = index;
}

// Touch panel pointer: the mouse, or a controller stick via the relative-aim binds (stick deflection maps to
// the whole screen, like the lightgun relative aim). Touching = the TouchPress bind, or left click when unbound.
// FCB reports X=Y=0xFFFF when the panel isn't touched.
static void UpdateTouchFromPointer()
{
	float wx, wy;
	if (m_touchRelActive)
	{
		wx = (((m_touchRel[1] > 0.0f) ? m_touchRel[1] : -m_touchRel[0]) + 1.0f) * 0.5f * ImGuiManager::GetWindowWidth();
		wy = (((m_touchRel[3] > 0.0f) ? m_touchRel[3] : -m_touchRel[2]) + 1.0f) * 0.5f * ImGuiManager::GetWindowHeight();
	}
	else
	{
		const auto& [mx, my] = InputManager::GetPointerAbsolutePosition(0);
		wx = mx;
		wy = my;
	}
	if (!m_touchCursorPath.empty())
		ImGuiManager::SetSoftwareCursorPosition(TouchPointerIndex(), wx, wy);

	const bool pressed = m_touchPressBound ? m_touchPressed : false /* ARMSX2: a touch panel's press always arrives through SetTouchPressed (JNI) */;
	if (!pressed)
	{
		m_jvsScreenPosX[0] = 0xFFFF;
		m_jvsScreenPosY[0] = 0xFFFF;
		return;
	}
	float mdx, mdy;
	GSTranslateWindowToDisplayCoordinates(wx, wy, &mdx, &mdy);
	const float cx = (mdx < 0.0f) ? 0.0f : (mdx > 1.0f ? 1.0f : mdx);
	const float cy = (mdy < 0.0f) ? 0.0f : (mdy > 1.0f ? 1.0f : mdy);
	m_jvsScreenPosX[0] = std::min<u16>(static_cast<u16>(cx * 0xFFFF), 0xFFFE);
	m_jvsScreenPosY[0] = std::min<u16>(static_cast<u16>((1.0f - cy) * 0xFFFF), 0xFFFE); // FCB Y axis is bottom-up
}

// Combine host axes into the 3 JVS analog channels (steer/gas/brake). Steering encoding is per-game.
// (Ridge Racer V uses UpdateFcaFrame instead.)
static void UpdateWheelChannels()
{
	const float steer = std::clamp(m_wheelSteerR - m_wheelSteerL, -1.0f, 1.0f); // -1 full left .. +1 full right
	const bool isBG3 = (s_gameid == "NM00010" || s_gameid == "NM00015");
	const bool isWangan = (s_gameid == "NM00008" || s_gameid == "NM00005");
	if (isBG3)
		m_jvsWheelChannels[0] = static_cast<u16>(512.0f - steer * 496.0f);                    // BG3: 10-bit, center 512, inverted
	else if (isWangan)
		m_jvsWheelChannels[0] = static_cast<u16>(0x8000 + static_cast<int>(steer * 0x7E00));  // Wangan: center 0x8000, +-0x7E00
	else
		m_jvsWheelChannels[0] = static_cast<u16>((steer * 0.5f + 0.5f) * 0xFFFF);             // standard JVS: unsigned 16-bit, center 0x8000
	const float pedalMax = isWangan ? 32767.0f : static_cast<float>(0xFFFF); // Wangan pedals use the 0..0x7FFF half (else they wrap)
	m_jvsWheelChannels[1] = static_cast<u16>(std::clamp(m_wheelGas,   0.0f, 1.0f) * pedalMax); // accelerator
	m_jvsWheelChannels[2] = static_cast<u16>(std::clamp(m_wheelBrake, 0.0f, 1.0f) * pedalMax); // brake
}

void do_jvs_packet(const u8* input, u8* output) {
	input++;
	u8 inDest = *input++;
	u8 inSize = *input++;
	u8 outSize = 0;
	u32 inWorkChecksum = inDest + inSize;
	inSize--;

	(*output++) = JVS_SYNC;
	(*output++) = 0x00; //Master ID?
	u8* dstSize = output++;
	(*dstSize) = 1;
	(*output++) = JVS_CMD_SUCCESS;
	while(inSize != 0) {
		u8 cmd = (*input++);
		inSize--;
		inWorkChecksum += cmd;
		switch(cmd) {
		case JVS::RESET: {
			JVS_ASSERT(inSize != 0);
			u8 param = (*input++);
			JVS_ASSERT(param == 0xD9);
			inSize--;
			inWorkChecksum += param;
		}
		break;
		case JVS::READ_ID_DATA: {
			(*output++) = JVS_CMD_SUCCESS;
			(*dstSize)++;
			const char* boardName = BOARDS[ACJV::CurrentBoardID].c_str();
			size_t length = strlen(boardName);

			for(int i = 0; i < length + 1; i++)
			{
				(*output++) = boardName[i];
				(*dstSize)++;
			}
		}
		break;
		case JVS::SET_NODE_ADDRESS: {
			JVS_ASSERT(inSize != 0);
			u8 param = (*input++);
			inSize--;
			inWorkChecksum += param;
			(*output++) = JVS_CMD_SUCCESS;
			(*dstSize)++;
		}
		break;
		case JVS::GET_CMDFORMAT_REV: {
			(*output++) = JVS_CMD_SUCCESS;
			(*output++) = 0x13; //Revision 1.3
			(*dstSize) += 2;
		}
		break;
		case JVS::GET_REVISION: {
			(*output++) = JVS_CMD_SUCCESS;
			(*output++) = JVS_REVISION;
			(*dstSize) += 2;
		}
		break;
		case JVS::GET_SUPP_COMM_VER: {
			(*output++) = JVS_CMD_SUCCESS;
			(*output++) = JVS_VERSION;
			(*dstSize) += 2;
		}
		break;
		case JVS::GET_SLAVE_FEAT: {
			(*output++) = JVS_CMD_SUCCESS;

			(*output++) = 0x02;             //Coin input
			(*output++) = JVS_PLAYER_COUNT; //2 Coin slots
			(*output++) = 0x00;
			(*output++) = 0x00;

			(*output++) = 0x01;             //Switch input
			(*output++) = JVS_PLAYER_COUNT; //2 players
			(*output++) = 0x10;             //16 switches
			(*output++) = 0x00;
			// Driving games (Wangan Midnight, MotoGP, ...): 3 analog channels (steer/gas/brake)
			if(m_jvsMode == JVS_MODE::DRIVE)
			{
				// BG3's Taito K91X0951A is an AD8 board (8 analog ch, 10-bit) per its dumped firmware's
				// JVS feature descriptor @0xfc0fea (03 08 0a 00); other driving boards report 3x 16-bit.
				const bool isBG3 = (ACJV::CurrentBoardID == TAITO_BG3_IO_PCB);
				(*output++) = 0x03;                              //Analog Input
				(*output++) = isBG3 ? 8 : JVS_WHEEL_CHANNEL_MAX; //Channel Count
				(*output++) = isBG3 ? 0x0A : 0x10;               //Bits
				(*output++) = 0x00;
				(*dstSize) += 4;
			}
			else
			if(m_jvsMode == JVS_MODE::LIGHTGUN)
			{
				(*output++) = 0x06; //Screen Pos Input
				(*output++) = 0x10; //X pos bits
				(*output++) = 0x10; //Y pos bits
				(*output++) = m_gunMapping->p2_trigger ? 0x02 : 0x01; // gun count: 2 if P2 trigger defined (Vampire Night), else 1

				//GPIO for recoil
				(*output++) = 0x12; //GPIO output
				(*output++) = 0x10; //slot(?) count
				(*output++) = 0x00;
				(*output++) = 0x00;

				//Time Crisis 4 reads from analog input to determine screen position
				(*output++) = 0x03; //Analog Input
				(*output++) = 0x02; //Channel Count (2 channels)
				(*output++) = 0x10; //Bits (16 bits)
				(*output++) = 0x00;

				(*dstSize) += 12;
			}
			// Taiko drum: 8 analog channels (piezo sensors), 10-bit (measured: game polls READ_INP_ANALOG 8)
			else if(m_jvsMode == JVS_MODE::DRUM)
			{
				(*output++) = 0x03;                 //Analog Input
				(*output++) = JVS_DRUM_CHANNEL_MAX; //Channel Count (8 channels)
				(*output++) = 0x0A;                 //Bits (10 bits)
				(*output++) = 0x00;

				(*dstSize) += 4;
			}
			else if(m_jvsMode == JVS_MODE::TOUCH)
			{
				(*output++) = 0x06; //Screen Pos Input
				(*output++) = 0x10; //X pos bits
				(*output++) = 0x10; //Y pos bits
				(*output++) = 0x01; //channels

				(*dstSize) += 4;
			}
			(*output++) = 0x00; //End of features

			(*dstSize) += 10;
		}
		break;
		case JVS::CONVEY_ID_MAINBOARD:
		{
			while(1)
			{
				u8 value = (*input++);
				JVS_ASSERT(inSize != 0);
				inSize--;
				inWorkChecksum += value;
				if(value == 0) break;
			}
		}
		break;
		case JVS::READ_INP_SWITCH:
		{
			JVS_ASSERT(inSize >= 2);
			u8 playerCount = (*input++);
			u8 byteCount = (*input++);
			JVS_ASSERT(playerCount >= 1);
			JVS_ASSERT(playerCount <= JVS_PLAYER_COUNT);
			JVS_ASSERT(byteCount == 2);
			inWorkChecksum += playerCount;
			inWorkChecksum += byteCount;
			inSize -= 2;

			(*output++) = JVS_CMD_SUCCESS;
			(*output++) = m_testButtonState|(s_dip_switch_state & TESTMODE);
			//(*output++) = (m_jvsSystemButtonState == 0x03) ? 0x80 : 0;  //Test

			u16 p1btn = m_jvsButtonState[0].load(std::memory_order_relaxed) | m_jvsMacroButtonState[0];
			if (s_lightgun_link_2p)
				p1btn |= m_gunMapping->link_2p; // test menu reads it as LINK AS : 2 (RIGHT)
			(*output++) = static_cast<u8>(p1btn);      //Player 1
			(*output++) = static_cast<u8>(p1btn >> 8); //Player 1
			(*dstSize) += 4;

			//if (m_jvsButtonState[0])
			//	Console.WriteLn("JVS P1 buttons: %04X coin:%d", m_jvsButtonState[0].load(), ACJV::coin[0]);

			if(playerCount == 2)
			{
				const u16 p2btn = m_jvsButtonState[1].load(std::memory_order_relaxed) | m_jvsMacroButtonState[1];
				(*output++) = static_cast<u8>(p2btn);      //Player 2
				(*output++) = static_cast<u8>(p2btn >> 8); //Player 2
				(*dstSize) += 2;
			}
		}
		break;
		case JVS::READ_INP_COIN:
		{
			JVS_ASSERT(inSize != 0);
			u8 slotCount = (*input++);
			JVS_ASSERT(slotCount >= 1);
			JVS_ASSERT(slotCount <= 2);
			inWorkChecksum += slotCount;
			inSize--;
			u8 slot1Condition = COIN_NORMAL; // see enum COINCOND
			u8 slot2Condition = COIN_NORMAL; // see enum COINCOND

			(*output++) = JVS_CMD_SUCCESS;

			(*output++) = static_cast<u8>(((ACJV::coin[0] >> 8) & 0x3f) | (slot1Condition << 6)); //Coin 1 MSB + slot1condition
			(*output++) = static_cast<u8>(ACJV::coin[0] & 0x00ff);                                //Coin 1 LSB

			(*dstSize) += 3;

			if(slotCount == 2)
			{
				(*output++) = static_cast<u8>(((ACJV::coin[1] >> 8) & 0x3f) | (slot2Condition << 6)); //Coin 2 MSB + slot2condition
				(*output++) = static_cast<u8>(ACJV::coin[1] & 0x00ff);                                //Coin 2 LSB

				(*dstSize) += 2;
			}
		}
		break;
		case JVS::OUTPUT_COIN_NUM: // actually never received this jvs cmd
		{
			JVS_ASSERT(inSize >= 3);
			u8 slotCount = (*input++);
			u8 amountMSB = (*input++);
			u8 amountLSB = (*input++);

			JVS_ASSERT(slotCount >= 1);
			JVS_ASSERT(slotCount <= 2);
			//inWorkChecksum += slotCount;
			inSize -= 3;

			if(slotCount == 1) ACJV::coin[0] += (amountMSB << 8) + amountLSB;
			if(slotCount == 2) ACJV::coin[1] += (amountMSB << 8) + amountLSB;

			(*output++) = JVS_CMD_SUCCESS;

			(*dstSize) += 1;
		}
		break;
		case JVS::DECREASE_COIN_NUM: // actually never received this jvs cmd
		{
			JVS_ASSERT(inSize >= 3);
			u8 slotCount = (*input++);
			u8 amountMSB = (*input++);
			u8 amountLSB = (*input++);

			JVS_ASSERT(slotCount >= 1);
			JVS_ASSERT(slotCount <= 2);
			//inWorkChecksum += slotCount;
			inSize -= 3;

			if(slotCount == 1) ACJV::coin[0] -= (amountMSB << 8) + amountLSB;
			if(slotCount == 2) ACJV::coin[1] -= (amountMSB << 8) + amountLSB;

			(*output++) = JVS_CMD_SUCCESS;

			(*dstSize) += 1;
		}
		break;
		case JVS::READ_INP_ANALOG:
		{
			JVS_ASSERT(inSize != 0);
			u8 channel = (*input++);
			inWorkChecksum += channel;
			inSize--;

			(*output++) = JVS_CMD_SUCCESS;

			// TC4 reads screen position from analog channels instead of SCREENPOS
			if(m_jvsMode == JVS_MODE::LIGHTGUN)
			{
				JVS_ASSERT(channel == 2);
				UpdateLightgunFromMouse();
				(*output++) = static_cast<u8>(m_jvsScreenPosX[0] >> 8); //Pos X MSB
				(*output++) = static_cast<u8>(m_jvsScreenPosX[0]);      //Pos X LSB
				(*output++) = static_cast<u8>(m_jvsScreenPosY[0] >> 8); //Pos Y MSB
				(*output++) = static_cast<u8>(m_jvsScreenPosY[0]);      //Pos Y LSB
			}
			else if(m_jvsMode == JVS_MODE::DRUM)
			{
				JVS_ASSERT(channel == JVS_DRUM_CHANNEL_MAX);
				for(int i = 0; i < JVS_DRUM_CHANNEL_MAX; i++)
				{
					(*output++) = static_cast<u8>(m_jvsDrumChannels[i] >> 8);
					(*output++) = static_cast<u8>(m_jvsDrumChannels[i]);
				}
			}
			else if(m_jvsMode == JVS_MODE::DRIVE)
			{
				UpdateWheelChannels();
				// Respond with exactly the channel count the game requested (BG3's AD8 board asks for 8,
				// other driving boards 3). ch0-2 = steer/gas/brake; spare channels report mid-scale.
				for(int i = 0; i < channel; i++)
				{
					u16 v = (i < JVS_WHEEL_CHANNEL_MAX) ? m_jvsWheelChannels[i] : 0x8000;
					(*output++) = static_cast<u8>(v >> 8);
					(*output++) = static_cast<u8>(v);
				}
			}

			(*dstSize) += (2 * channel) + 1;
		}
		break;
		case JVS::READ_INP_SCREENPOS:
		{
			JVS_ASSERT(inSize != 0);
			u8 channel = (*input++);
			inWorkChecksum += channel;
			inSize--;

			if(m_jvsMode == JVS_MODE::LIGHTGUN)
				UpdateLightgunFromMouse();
			else if(m_jvsMode == JVS_MODE::TOUCH)
				UpdateTouchFromPointer();

			(*output++) = JVS_CMD_SUCCESS;

			// Screen position scaling depends on I/O board:
			// - MIU-I/O (TC3): native 640x224, Y inverted (bottom-up)
			// - RAYS PCB (TC4, Cobra, VPN): full 16-bit range 0xFFFF, Y inverted (bottom-up)
			// pos=0 means off-screen in JVS, so on-screen values are clamped to minimum 1
			// JVS SCREENPOS: 'channel' is the 1-based gun index (Vampire Night reads channel=1 then =2 each
			// frame), so return that one gun's player position - gun 1 -> P1, gun 2 -> P2.
			const u32 pl = (channel >= 1 && channel <= JVS_PLAYER_COUNT) ? (channel - 1u) : 0u;
			u16 posX = 0, posY = 0;
			const GunBoardModel board = ACJV::GetGunMapping().board;
			if(m_jvsMode == JVS_MODE::TOUCH)
			{
				posX = m_jvsScreenPosX[0];
				posY = m_jvsScreenPosY[0];
			}
			else if(m_jvsMode == JVS_MODE::LIGHTGUN && (board == GunBoardModel::CameraVN || board == GunBoardModel::TwoTierTC3))
			{ //values built straight in UpdateLightgunFromMouse (0xFFFF/0xFFFF = camera lost / fully off)
				posX = s_gunRawX[pl];
				posY = s_gunRawY[pl];
			}
			else if(m_jvsMode == JVS_MODE::LIGHTGUN && m_jvsLightgunDX[pl] >= 0.0f)
			{
				const float scaleX = (ACJV::CurrentBoardID == MIU_IO_JPN_GUN_EXTENTI) ? 640.0f : 0xFFFF;
				const float scaleY = (ACJV::CurrentBoardID == MIU_IO_JPN_GUN_EXTENTI) ? 224.0f : 0xFFFF;
				posX = static_cast<u16>(m_jvsLightgunDX[pl] * scaleX);
				if (ACJV::CurrentBoardID == RAYS_PCB || ACJV::CurrentBoardID == MIU_IO_JPN_GUN_EXTENTI)
					posY = static_cast<u16>((1.0f - m_jvsLightgunDY[pl]) * scaleY);
				else
					posY = static_cast<u16>(m_jvsLightgunDY[pl] * scaleY);
				if (posX == 0) posX = 1;
				if (posY == 0) posY = 1;
			}
			(*output++) = static_cast<u8>(posX >> 8);
			(*output++) = static_cast<u8>(posX);
			(*output++) = static_cast<u8>(posY >> 8);
			(*output++) = static_cast<u8>(posY);

			(*dstSize) += 1 + 4; // status + one position (X, Y) for the requested gun
		}
		break;
		// GPIO output — game sends byte values to control physical outputs (e.g. gun recoil solenoids).
		// Byte 1 = P1 recoil: value >= 0x50 means recoil triggered, value 0xC0 observed during fire.
		// TODO: forward p1Recoil to serial port / USB for real lightgun recoil hardware
		case JVS::OUTPUT_GENERAL:
		{
			JVS_ASSERT(inSize >= 2);

			u8 bytecount = (*input++);
			inWorkChecksum += bytecount;
			inSize--;

			for(int i = 1; i <= bytecount; i++)
			{
				u8 gpvalue = (*input++);
				inWorkChecksum += gpvalue;
				inSize--;

				if(i == 1)
				{
					int p1Recoil = (gpvalue >= 0x50) ? 1 : 0;
					(void)p1Recoil;
				}
			}

			(*output++) = JVS_CMD_SUCCESS;
			(*dstSize) += 1;
		}
		break;
		// Namco vendor command 0x70. Two dialects share it: the FCB touch panel sends a sub-command
		// plus args (0x60 status = 1, 0x62 touch init = 2, 0x18 boot config = 4) and waits on an echo
		// with report 0x01; the Sys246gun camera board (VN) sends adjust-control sub 0x40 and its
		// n246JvioNamcoGun* parsers expect report, 0xFF marker, length, then payload[2] = 1 (complete).
		case JVS::NAMCO_VENDOR:
		{
			JVS_ASSERT(inSize != 0);
			u8 sub = (*input++);
			inWorkChecksum += sub;
			inSize--;

			if (ACJV::GetGunMapping().board == GunBoardModel::CameraVN)
			{
				while (inSize != 0)
				{
					u8 data = (*input++);
					inWorkChecksum += data;
					inSize--;
				}
				(*output++) = JVS_CMD_SUCCESS;
				(*output++) = 0xFF;
				(*output++) = 0x04; //payload length
				(*output++) = sub;
				(*output++) = 0x00;
				(*output++) = 0x01; //payload[2]: adjust complete (the real board reports busy first; ours finishes instantly)
				(*output++) = 0x00;
				(*dstSize) += 7;
			}
			else
			{
				u8 args = (sub == 0x60) ? 1 : (sub == 0x62) ? 2 : (sub == 0x18) ? 4 : inSize;
				for(u8 i = 0; i < args && inSize != 0; i++)
				{
					u8 data = (*input++);
					inWorkChecksum += data;
					inSize--;
				}
				(*output++) = JVS_CMD_SUCCESS;
				(*output++) = 0x02; //payload length
				(*output++) = sub;  //sub-command echo
				(*output++) = 0x01; //status: ready
				(*dstSize) += 4;
			}
		}
		break;
		default:
			//Unknown command
			// Console.Error("ACJV::%s: unknown JVS CMD 0x%X", __FUNCTION__, cmd);
			break;
		}
	}
	u8 inChecksum = (*input);
	// if (inChecksum != (inWorkChecksum & 0xFF))
	//     Console.Warning("ACJV::%s: checksum mismatch: %02X | %02X", __FUNCTION__, inChecksum, inWorkChecksum&0xFF);
}


// based on https://github.com/search?q=repo%3Ajpd002/Play-%20CSys246%3A%3AProcessJvsPacket&type=code by Jean-Philip Desjardins
// JVFIRM version n246Jvio checks against its own (mismatch stalls boot): BG3=0x210, BG3T=0x213, others 0x208.
static u16 JvFirmwareVersion() {
	return (s_gameid == "NM00015") ? 0x213 : (s_gameid == "NM00010") ? 0x210 : 0x208;
}

// Prime the JVS board firmware-version register when ACJV starts (BG3 Tuned polls it before any command).
void ACJV::OnBoardStart() {
	rdbuf_getu16()[1] = JvFirmwareVersion();
}

void do_acjv_packet() {
	const u16* wr16 = wrbuf_getu16();
	u16* rd16 = rdbuf_getu16();
	rd16[0] = wr16[0];
	u16 RootPacketID = wr16[8];
	if(rd16[0] == 0x3E6F) {
		rd16[1]      = JvFirmwareVersion();
		rd16[0x14]   = RootPacketID; // Xored with value at 0x10 in send packet, needs to be the same
		rd16[0x21]   = wr16[0x0D];
		rdbuf[0x30]  = s_dip_switch_state; // here the game polls the dip switch values?
		static u8 s_acFrameSeq = 0;
		rdbuf[0x57] = ++s_acFrameSeq; // game waits on this byte advancing each frame to finalize a coin decrement
		u16 PacketID = wr16[0x0C];
		if(PacketID != 0) {
			if(wrbuf[0x122] == JVS_SYNC) {
				do_jvs_packet(&wrbuf[0x122], &rdbuf[0x15A]);
			} else {
				do_jvs_packet(&wrbuf[0x22],  &rdbuf[0x5A]);
			}
			static u16 s_lastCoinPkt = 0xFFFF; // coin DECREASE rides in a 2nd JVS packet (slot 0x22); service it once per poll
			if(wrbuf[0x122] == JVS_SYNC && wrbuf[0x22] == JVS_SYNC && wrbuf[0x23] != 0x00 && PacketID != s_lastCoinPkt) {
				do_jvs_packet(&wrbuf[0x22], &rdbuf[0x5A]);
				s_lastCoinPkt = PacketID;
			}
			rd16[0x20] = PacketID;
		}
		// ac_jvsif root field (0x2A/0x2C/0x2E): a board value the white-screen loader sums to pace a cosmetic bar.
		// BG3 gets the bar-skip minimum 0x9C40 (instant boot); others keep 0x5210.
		if (s_gameid == "NM00010" || s_gameid == "NM00015") {
			rd16[0x15] = 0x9C40;
			rd16[0x16] = 0x9C40;
			rd16[0x17] = 0x9C40;
		} else {
			rd16[0x15] = 0x5210;
			rd16[0x16] = 0x5210;
			rd16[0x17] = 0x5210;
		}
	}
}

// Free-run the FCA-1 input frame for Ridge Racer V in the rdbuf (do_acjv_packet never runs for it).
// RRV's init waits on the heartbeat @0x0e/0x0f, then reads steering/pedals/buttons. Ticked from DEV9async.
void ACJV::UpdateFcaFrame()
{
	if (s_gameid != "NM00001")
		return;

	static u16 s_fcaCounter = 0;
	s_fcaCounter++;                            // FCA-1 heartbeat — unblocks FUN_00229af0 case 1
	rdbuf[0x0e] = (u8)(s_fcaCounter & 0xff);   // counter: low byte @0x0e, high byte @0x0f
	rdbuf[0x0f] = (u8)(s_fcaCounter >> 8);

	// FCA-1 raw range (FUN_0022ab70): steer center 0x8000 +-0x6400, pedals 0..0x5800.
	const float steerf = std::clamp(m_wheelSteerR - m_wheelSteerL, -1.0f, 1.0f);
	const u16 steerRaw = (u16)(0x8000 + (int)(steerf * 0x6400));
	rdbuf[0x80] = (u8)(steerRaw & 0xff);
	rdbuf[0x81] = (u8)(steerRaw >> 8);
	const u16 gasRaw = (u16)(std::clamp(m_wheelGas, 0.0f, 1.0f) * 0x5800);
	rdbuf[0x82] = (u8)(gasRaw & 0xff);
	rdbuf[0x83] = (u8)(gasRaw >> 8);
	const u16 brakeRaw = (u16)(std::clamp(m_wheelBrake, 0.0f, 1.0f) * 0x5800);
	rdbuf[0x84] = (u8)(brakeRaw & 0xff);
	rdbuf[0x85] = (u8)(brakeRaw >> 8);

	// Buttons (FCA-1 digital inputs, active-high; bit assignments RE'd from I/O TEST FUN_0022bba8).
	const u16 btn = m_jvsButtonState[0].load(std::memory_order_relaxed) | m_jvsMacroButtonState[0];
	u8 b40 = 0, b41 = 0;
	if (btn & JVS_BTN_3)       b40 |= 0x80; // R1       -> UP SHIFT (gear up)
	if (btn & JVS_BTN_4)       b40 |= 0x40; // L1       -> DOWN SHIFT (gear down)
	if (btn & JVS_BTN_2)       b41 |= 0x01; // Triangle -> VIEW CHANGE
	if (btn & (JVS_BTN_START|JVS_BTN_1)) b41 |= 0x02; // Start / Square -> ENTER (confirm/start)
	if (btn & JVS_BTN_UP)      b41 |= 0x20; // DPad Up   -> UP SELECT
	if (btn & JVS_BTN_DOWN)    b41 |= 0x10; // DPad Down -> DOWN SELECT
	if (btn & JVS_BTN_SERVICE) b41 |= 0x40; // Select   -> SERVICE (adds a service credit)
	rdbuf[0x40] = b40;
	rdbuf[0x41] = b41;

	// TEST switch (rdbuf[0xe2] b7): RRV's FCA path bypasses the standard DIP register, so feed Test Mode here.
	rdbuf[0xe2] = (s_dip_switch_state & TESTMODE) ? 0x80 : 0;

	// COIN: FCA-1 coin counter @rdbuf[0xc0]; RRV credits on increase (FUN_0022aa88). Mirror our coin count.
	rdbuf[0xc0] = (u8)ACJV::coin[0];
}
