// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#pragma once
#include <span>
#include "MemoryTypes.h"
#include "common/Pcsx2Types.h"
#include "common/Pcsx2Defs.h"
#include "common/ARCADE.h"
// InputBindingInfo, complete: the layouts below keep spans of it, and NDK 28's libc++ (the Play build's)
// rejects a span of an incomplete type there.
#include "Config.h"

class SettingsInterface;

#define ACJV_BASE_ADDDR 0x12400000
#define ACJV_RANGE      0x1240
#define ACJV_ADDR_CAP   0x8000 // ACJV.IRX r/w fun will do `(2 * (addr & 0x3FFF)`, meaning available range is `0x12400000` - `0x12407FFE`
#define ACJV_PACKETSIZE 0x300 // seems to be the amount of bytes that's always read/written by the game

#define ACJV_RDBASE 0x12404000 // acmeme access addr 0x2300
#define ACJV_WRBASE 0x12404600 // acmeme access addr 0x2000

#define ACJV_CTR_START 0x12416002 // set to 0 when ACJV.IRX runs
#define ACJV_CTR_STOP  0x12416000 // set to 0 during: JVFIRM upload begins, ACCORE starts, `acJvModuleStop()` is called

#define ACJV_RDWR_SIZELIMIT 0x4000 // ACJV.IRX read/write functions only consider 14 bits from addr

#define JVS_CMD_SUCCESS 0x1
#define JVS_REVISION 0x30 //Revision 3.0
#define JVS_VERSION 0x10 //Version 1.0
#define JVS_PLAYER_COUNT 2 


enum BOARDID {
	RAYS_PCB = 0,
	FCA_1_JPN_MULTIPURPOSE,
	FCB_JPN_TOUCHPANEL,
	TSS_GUN_EXTENTION,
	MIU_IO_JPN_GUN_EXTENTI,
	TAITO_BG3_IO_PCB
};

enum class JVS_MODE {
	DEFAULT,
	LIGHTGUN,
	FIGHTING,
	DRIVE,
	DRUM,
	TOUCH,
	STANDARD, // generic full 6-button group for games that fit no specific category
	TWINSTICK, // dual-lever cabinets (Zoids): custom JVS bit wiring, 2 sticks + 2 triggers + 2 buttons
};

// Sw -> pad order per layout, mirroring each game's official PS2 port.
enum class FightingLayout {
	TEKKEN,     // Sw1,2,4,5: Square, Triangle, Cross, Circle         — TK4, TK5, TK5DR
	GUNDAM,     // Sw1,2,3,4: Square, Triangle, Cross, Circle         — Gundam VS (zgundm/dx, SEED/Dest, GvG/NEXT)
	SIX_BUTTON, // Sw1-6:     Square, Triangle, L1, Cross, Circle, R1 — JAM
	SOULCAL,    // Sw1,2,3,4: Square, Triangle, Circle, Cross         — SC2, SC3
	BLOODYROAR, // Sw1,2,3,4: Square, Cross, Circle, Triangle         — BR3
	// Per-game splits: same scheme as the parent franchise, own config keys so each maps independently.
	FATE,       // NM00048: Fate Unlimited Codes (Weak/Medium/Strong + Reflect Guard)
	KINNIKUMAN, // NM00029/40: Kinnikuman MGP 1 & 2 (Left/Right Fist, Special, Guard)
	PRIDEGP,    // NM00011: Pride GP 2003 (Left/Right Punch + Kick, limb-based)
	BASARA,     // NM00042: Sengoku Basara X (Weak/Medium/Strong + Striker)
	SDBZ,       // NM00027: Super Dragon Ball Z (Light/Heavy Attack, Jump, Block)
	YUYU,       // NM00035: YuYu Hakusho Deathmatch (versus, 3 buttons: Punch/Kick/Guard)
};

enum class RacingLayout {
	UNIVERSAL,  // Wangan/RRV/MotoGP/Ace Driver 3 (View = Sw2|Push9)
	BG3,        // NM00010/15: D-pad-wired + Sidebrake + Hazard (Taito board)
};

enum class StandardLayout {
	BASEBALL,    // NM00009: Netchu Pro Baseball (Sw1-6 superset)
	SMASHCOURT,  // NM00006: Smash Court Pro (2 buttons)
	TECHNICBEAT, // NM10003: Technic Beat (3 buttons)
	GUNDAMQUIZ,  // NM00030: Gundam Quiz Warrior (4-button Gundam wiring)
	INUFUKU,     // NM00037: Quiz Inufuku 2 (answer buttons 1-4 on the directional bits)
};

#define JVS_WHEEL_CHANNEL_MAX 3
#define JVS_DRUM_CHANNEL_MAX 8

enum class GunBoardModel : u8 {
    Classic,        // (0,0) when off-screen, sensor bit only
    CameraVN,       // Sys246gun camera (VN): 0xFFFF/0xFFFF = camera lost, 5-point calibration frame
    SideSwitchTC4,  // TSS-I/O (TC4): coords clamp to the field edge (never zeroed), off-screen flag separate
    TwoTierTC3,     // TSS-I/O (TC3): coord past [0,640]x[0,448] = yellow reload; 0xFFFF/0xFFFF = red fully-lost
};

struct GunMapping {
    u16 pedal;
    u16 sensor;
    bool sensor_active_high;
    u16 p1_start;
    u16 p2_start;
    u16 p1_trigger;
    u16 p2_trigger;
    GunBoardModel board;
    u16 link_2p; // linked-cabinet side switch: held = game runs as 2P/right side
};

namespace ACJV {
    enum : u32
    {
        NUM_DIP_SWITCHES = 4,
        NUM_JVS_MACROS = 8,
    };

    static constexpr const char* CONFIG_SECTION = "JVS";
    static constexpr const char* TRANSLATION_CONTEXT = "JVS";

    struct DIPSwitchInfo
    {
        const char* name;
        const char* display_name;
        const char* toggle_bind_name;
        bool default_value;
    };

    extern bool enabled;
    extern u16 coin[2];

    u16 Read16(u32 addr);
    void Write16(u32 addr, u16 val);
    void UpdateFcaFrame(); // Ridge Racer V: refresh the FCA-1 I/O board's input buffer every frame (steering/pedals/buttons)
    void SetTouchPressed(bool pressed);
    void SetTouchPressBound(bool bound);
    void SetTouchRelativeAxis(u32 axis, float value); // 0..3 = Left/Right/Up/Down stick deflection
    void SetTouchRelativeActive(bool active);
    void SetTouchCursor(std::string path, float scale, u32 color);
    void OnBoardStart();   // set the JVS I/O board firmware version at power-on (Battle Gear 3 Tuned reads it before its first command)
    extern enum BOARDID CurrentBoardID;

    std::span<const DIPSwitchInfo> GetDIPSwitches();
    const DIPSwitchInfo& GetTestModeDIPSwitch();
    const DIPSwitchInfo& GetVideoVoltageDIPSwitch();
    const DIPSwitchInfo& GetMonitorSyncFrequencyDIPSwitch();
    const DIPSwitchInfo& GetVideoSyncSplitDIPSwitch();
    std::span<const InputBindingInfo> GetDIPSwitchBindings();
    std::span<const InputBindingInfo> GetButtonBindings();
    std::span<const InputBindingInfo> GetP2ButtonBindings();
    std::span<const InputBindingInfo> GetCoinBindings();
    std::span<const InputBindingInfo> GetWheelBindings();
    std::span<const InputBindingInfo> GetDrumBindings();
    std::span<const InputBindingInfo> GetTwinstickBindings(); // Zoids
    std::span<const InputBindingInfo> GetFightingButtons(); // empty if none
    // ARMSX2: the same for any game, not only the running one (the Arcade controls settings).
    std::span<const InputBindingInfo> GetFightingButtons(const std::string& gameid);
    std::span<const InputBindingInfo> GetStandardButtons(const std::string& gameid);
    std::span<const InputBindingInfo> GetRacingButtons(const std::string& gameid);
    struct LayoutInfo { const char* name; std::span<const InputBindingInfo> buttons; bool one_player; const char* note = nullptr; }; // Fighting/Standard share this; note = optional hint under the buttons
    std::span<const LayoutInfo> GetFightingLayouts();
    std::span<const InputBindingInfo> GetStandardButtons(); // empty if none
    std::span<const LayoutInfo> GetStandardLayouts();
    std::span<const InputBindingInfo> GetRacingButtons(); // empty if none
    struct RacingLayoutInfo { const char* name; std::span<const InputBindingInfo> buttons; };
    std::span<const RacingLayoutInfo> GetRacingLayouts();
    struct JvsMacroSwitch { const char* name; const char* label; u16 bit; };
    std::span<const JvsMacroSwitch> GetMacroSwitches(); // fixed JVS switches a macro can fire (game-independent)
    std::string LayoutKey(std::span<const InputBindingInfo> buttons); // stable per-layout config key (button-name prefix)
    std::string GetCurrentLayoutKey();                  // running game's layout key, or empty
    std::string MacroConfigKey(const std::string& layoutKey, u32 player, u32 index, const char* suffix);
    void SetButtonState(u32 player, u16 mask, bool pressed);
    void SetMacroState(u32 player, u32 index, bool active); // JVS input macro trigger: fire its switch set for the player
    void SetMacroMask(u32 player, u32 index, u16 mask);     // set a macro's combined switch mask (pushed by InputManager)
    void SetWheelAxis(u32 axis, float value);
    void SetDrumHit(u32 channel, bool pressed); // Taiko drum sensor -> JVS analog channel (0=off, else above DAI threshold)
    float GetSteer(); // host steering, -1 (full left)...+1 (full right); used by the BG3 drive-board responder (acuart)
    float GetGas();   // host accelerator, 0...1
    float GetBrake(); // host brake, 0...1
    void InsertCoin(u32 slot);

    bool GetDIPSwitchState(u32 index);
    void SetDIPSwitchState(u32 index, bool enabled);
    void ToggleDIPSwitchState(u32 index);
    void LoadConfig(const SettingsInterface& si);
    void CopyConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si, bool copy_settings = true, bool copy_bindings = true);
    void SetDefaultConfiguration(SettingsInterface& si);

    bool IsSuppressDaemonEnabled();
    const char* GetBoardDisplayName(BOARDID id);
    BOARDID GetCurrentBoardID();
    void SetMode(JVS_MODE mode);
    JVS_MODE GetMode();
    JVS_MODE ResolveModeFromGameId(const std::string& gameid); // device mode from the gameid alone; jvsmode= is an optional override
    void SetScreenPos(u16 x, u16 y);
    void SetGunAimSource(u32 player, bool joystick);        // lightgun aim source: false = shared mouse, true = player's stick
    void SetGunPointerIndex(u32 player, u32 index);         // host pointer slot for the player's mouse aim (per-device with Raw Input)
    void SetGunRelativeAim(u32 player, float dx, float dy); // player's stick-driven screen position from the GunCon2 (display coords)
    bool GetGunStickAim(u32 player, float* dx, float* dy);  // ARMSX2: where a stick-aimed gun points (display coords), for its crosshair
    void SetGunForceOffscreen(bool held);                   // force the camera-lost report (the pedal bind = manual reload on Vampire Night)
    void SetGunOffscreenContour(float fraction);            // width of the aim-beside-the-screen band at the window edge (0..0.05)
    void SetGameId(const std::string& gameid);
    const std::string& GetGameId();
    const GunMapping& GetGunMapping();

    bool IsSindenBorderEnabled();
    int GetSindenBorderMode();
    int GetSindenBorderThickness();
}


enum JVS { //https://github.com/TheOnlyJoey/openjvs/wiki/Command-list
    /// broadcast commands (meant for all active nodes, sent to node ID FF)
    RESET = 0xF0, // [REQUIRED]
    SET_NODE_ADDRESS = 0xF1, // [REQUIRED]
    SETUP_COMMS = 0xF2,
    /// initialization commands
    READ_ID_DATA = 0x10, // [REQUIRED]
    GET_CMDFORMAT_REV = 0x11, // [REQUIRED] get command format revision
    GET_REVISION = 0x12, // [REQUIRED] Get JVS revision
    GET_SUPP_COMM_VER= 0x13, // [REQUIRED] get supported communications versions
    GET_SLAVE_FEAT = 0x14, // [REQUIRED] indicates which commands of this enum are supported
    CONVEY_ID_MAINBOARD = 0x15, // convey ID of main board
    /// I/O commands
    READ_INP_SWITCH = 0x20, //read switch inputs
    READ_INP_COIN = 0x21, //read coin inputs
    READ_INP_ANALOG = 0x22, //read analog inputs
    READ_INP_ROTATORY = 0x23, //read rotary inputs
    READ_INP_KEYCODE = 0x24, //read key code input?
    READ_INP_SCREENPOS = 0x25, //read screen position input (duckhunt gun style)
    READ_INP_GENERAL_PURPOSE = 0x26, //read general-purpose (unrelated to player) input
    UNKNOWN_2e = 0x2e, //something with payouts?
    RETRANSIT_DATA_ON_FAIL = 0x2f, // [REQUIRED] request to retransmit data in case of checksum failure
    DECREASE_COIN_NUM = 0x30, //decrease number of coins
    OUTPUT_NUM_PAYOUT = 0x31, //output the number of payouts?
    OUTPUT_GENERAL = 0x32, //general-purpose output
    PUTPUT_ANALOG = 0x33, //analog output
    OUTPUT_CHAR_DATA = 0x34, //output character data, e.g. to LCD
    OUTPUT_COIN_NUM = 0x35, //output the total number of coins?
    SUBSTRACT_PAYOUT = 0x36, //subtract payouts?
    OUTPUT_GENERAL_PURPOSE2 = 0x37, //general-purpose output 2
    OUTPUT_GENERAL_PURPOSE3 = 0x38, //general-purpose output 3
    /// Manufacturer-specific
    //60-7F	 	reserved for manufacturer-specific commands
    NAMCO_VENDOR = 0x70, //Namco extended command: sub-command + args (FCB touch panel init/status)
};

enum COINCOND { // Coin Slot condition
    COIN_NORMAL=0,// normal
    COIN_JAMMED,  // coin jam
    COIN_DISCON,  // counter disconnected
    COIN_BUSY,    // busy
};

enum DIPS {
    TESTMODE = 0x80,
    VIDEO_VOLTAGE = 0x40,
    MONITOR_SYNCFREQ = 0x20,
    VIDEO_SYNC_SPLIT = 0x10,
};

// To improve and correct when adding more JVS controls from games
enum JVSButton : u16 {
    JVS_BTN_START   = 0x80,
    JVS_BTN_SERVICE = 0x40,
    JVS_BTN_UP      = 0x20,
    JVS_BTN_DOWN    = 0x10,
    JVS_BTN_LEFT    = 0x08,
    JVS_BTN_RIGHT   = 0x04,
    JVS_BTN_1       = 0x02,
    JVS_BTN_2       = 0x01,
    JVS_BTN_3       = 0x8000,
    JVS_BTN_4       = 0x4000,
    JVS_BTN_5       = 0x2000,
    JVS_BTN_6       = 0x1000,
    JVS_BTN_9       = 0x0200, // Ace Driver 3 wires VIEW CHANGE to Push9.
};


#define JVS_SYNC 0xE0
