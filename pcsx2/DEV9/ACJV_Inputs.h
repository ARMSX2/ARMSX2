// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#pragma once
static constexpr const std::array<ACJV::DIPSwitchInfo, ACJV::NUM_DIP_SWITCHES> s_dip_switch_info = {{
	{"TestMode", TRANSLATE_NOOP("JVS", "Test Mode"), "ToggleTestMode", false},
	{"VideoVoltage", TRANSLATE_NOOP("JVS", "Video Voltage"), "ToggleVideoVoltage", true},
	{"MonitorSyncFrequency", TRANSLATE_NOOP("JVS", "Monitor Sync Frequency"), "ToggleMonitorSyncFrequency", true},
	{"VideoSyncSplit", TRANSLATE_NOOP("JVS", "Video Sync Split"), "ToggleVideoSyncSplit", true},
}};

static constexpr const std::array<InputBindingInfo, ACJV::NUM_DIP_SWITCHES> s_dip_switch_bindings = {{
	{s_dip_switch_info[0].toggle_bind_name, TRANSLATE_NOOP("JVS", "Toggle Test Mode"), nullptr, InputBindingInfo::Type::Button, 0, GenericInputBinding::Unknown},
	{s_dip_switch_info[1].toggle_bind_name, TRANSLATE_NOOP("JVS", "Toggle Video Voltage"), nullptr, InputBindingInfo::Type::Button, 1, GenericInputBinding::Unknown},
	{s_dip_switch_info[2].toggle_bind_name, TRANSLATE_NOOP("JVS", "Toggle Monitor Sync Frequency"), nullptr, InputBindingInfo::Type::Button, 2, GenericInputBinding::Unknown},
	{s_dip_switch_info[3].toggle_bind_name, TRANSLATE_NOOP("JVS", "Toggle Video Sync Split"), nullptr, InputBindingInfo::Type::Button, 3, GenericInputBinding::Unknown},
}};

static constexpr const std::array<InputBindingInfo, 12> s_jvs_p1_button_bindings = {{
	{"P1_Up",      TRANSLATE_NOOP("JVS", "P1 Up"),       nullptr, InputBindingInfo::Type::Button, JVS_BTN_UP,      GenericInputBinding::DPadUp},
	{"P1_Down",    TRANSLATE_NOOP("JVS", "P1 Down"),     nullptr, InputBindingInfo::Type::Button, JVS_BTN_DOWN,    GenericInputBinding::DPadDown},
	{"P1_Left",    TRANSLATE_NOOP("JVS", "P1 Left"),     nullptr, InputBindingInfo::Type::Button, JVS_BTN_LEFT,    GenericInputBinding::DPadLeft},
	{"P1_Right",   TRANSLATE_NOOP("JVS", "P1 Right"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_RIGHT,   GenericInputBinding::DPadRight},
	{"P1_Button1", TRANSLATE_NOOP("JVS", "P1 Button 1"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_1,       GenericInputBinding::Square},
	{"P1_Button2", TRANSLATE_NOOP("JVS", "P1 Button 2"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2,       GenericInputBinding::Triangle},
	{"P1_Button3", TRANSLATE_NOOP("JVS", "P1 Button 3"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_3,       GenericInputBinding::Unknown},
	{"P1_Button4", TRANSLATE_NOOP("JVS", "P1 Button 4"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_4,       GenericInputBinding::Cross},
	{"P1_Button5", TRANSLATE_NOOP("JVS", "P1 Button 5"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_5,       GenericInputBinding::Circle},
	{"P1_Button6", TRANSLATE_NOOP("JVS", "P1 Button 6"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_6,       GenericInputBinding::Unknown},
	{"P1_Start",   TRANSLATE_NOOP("JVS", "P1 Start"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_START,   GenericInputBinding::Start},
	{"P1_Service", TRANSLATE_NOOP("JVS", "P1 Service"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_SERVICE, GenericInputBinding::Select},
}};

static constexpr const std::array<InputBindingInfo, 12> s_jvs_p2_button_bindings = {{
	{"P2_Up",      TRANSLATE_NOOP("JVS", "P2 Up"),       nullptr, InputBindingInfo::Type::Button, JVS_BTN_UP,      GenericInputBinding::DPadUp},
	{"P2_Down",    TRANSLATE_NOOP("JVS", "P2 Down"),     nullptr, InputBindingInfo::Type::Button, JVS_BTN_DOWN,    GenericInputBinding::DPadDown},
	{"P2_Left",    TRANSLATE_NOOP("JVS", "P2 Left"),     nullptr, InputBindingInfo::Type::Button, JVS_BTN_LEFT,    GenericInputBinding::DPadLeft},
	{"P2_Right",   TRANSLATE_NOOP("JVS", "P2 Right"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_RIGHT,   GenericInputBinding::DPadRight},
	{"P2_Button1", TRANSLATE_NOOP("JVS", "P2 Button 1"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_1,       GenericInputBinding::Square},
	{"P2_Button2", TRANSLATE_NOOP("JVS", "P2 Button 2"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2,       GenericInputBinding::Triangle},
	{"P2_Button3", TRANSLATE_NOOP("JVS", "P2 Button 3"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_3,       GenericInputBinding::Unknown},
	{"P2_Button4", TRANSLATE_NOOP("JVS", "P2 Button 4"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_4,       GenericInputBinding::Cross},
	{"P2_Button5", TRANSLATE_NOOP("JVS", "P2 Button 5"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_5,       GenericInputBinding::Circle},
	{"P2_Button6", TRANSLATE_NOOP("JVS", "P2 Button 6"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_6,       GenericInputBinding::Unknown},
	{"P2_Start",   TRANSLATE_NOOP("JVS", "P2 Start"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_START,   GenericInputBinding::Start},
	{"P2_Service", TRANSLATE_NOOP("JVS", "P2 Service"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_SERVICE, GenericInputBinding::Select},
}};

static constexpr const std::array<InputBindingInfo, 4> s_jvs_wheel_bindings = {{ // driving analog bindings, auto-mirrored from Pad0
	{"SteerRight", TRANSLATE_NOOP("JVS", "Steering Right"), nullptr, InputBindingInfo::Type::HalfAxis, 0, GenericInputBinding::LeftStickRight},
	{"SteerLeft",  TRANSLATE_NOOP("JVS", "Steering Left"),  nullptr, InputBindingInfo::Type::HalfAxis, 1, GenericInputBinding::LeftStickLeft},
	{"Gas",        TRANSLATE_NOOP("JVS", "Accelerator"),    nullptr, InputBindingInfo::Type::HalfAxis, 2, GenericInputBinding::R2},
	{"Brake",      TRANSLATE_NOOP("JVS", "Brake"),          nullptr, InputBindingInfo::Type::HalfAxis, 3, GenericInputBinding::L2},
}};

// Taiko drum: 8 piezo sensors on JVS analog channels. bind_index = MEASURED channel (in-game TAIKO TEST,
// scrambled vs the on-screen order): 1P DL=0 DR=3 KL=5 KR=4 | 2P DL=2 DR=7 KL=1 KR=6
static constexpr const std::array<InputBindingInfo, JVS_DRUM_CHANNEL_MAX> s_jvs_drum_bindings = {{
	{"P1_DonLeft",  TRANSLATE_NOOP("JVS", "P1 Don Left (inner, red)"),   nullptr, InputBindingInfo::Type::Button, 0, GenericInputBinding::Unknown},
	{"P1_DonRight", TRANSLATE_NOOP("JVS", "P1 Don Right (inner, red)"),  nullptr, InputBindingInfo::Type::Button, 3, GenericInputBinding::Unknown},
	{"P1_KaLeft",   TRANSLATE_NOOP("JVS", "P1 Ka Left (outer, blue)"),   nullptr, InputBindingInfo::Type::Button, 5, GenericInputBinding::Unknown},
	{"P1_KaRight",  TRANSLATE_NOOP("JVS", "P1 Ka Right (outer, blue)"),  nullptr, InputBindingInfo::Type::Button, 4, GenericInputBinding::Unknown},
	{"P2_DonLeft",  TRANSLATE_NOOP("JVS", "P2 Don Left (inner, red)"),   nullptr, InputBindingInfo::Type::Button, 2, GenericInputBinding::Unknown},
	{"P2_DonRight", TRANSLATE_NOOP("JVS", "P2 Don Right (inner, red)"),  nullptr, InputBindingInfo::Type::Button, 7, GenericInputBinding::Unknown},
	{"P2_KaLeft",   TRANSLATE_NOOP("JVS", "P2 Ka Left (outer, blue)"),   nullptr, InputBindingInfo::Type::Button, 1, GenericInputBinding::Unknown},
	{"P2_KaRight",  TRANSLATE_NOOP("JVS", "P2 Ka Right (outer, blue)"),  nullptr, InputBindingInfo::Type::Button, 6, GenericInputBinding::Unknown},
}};

// Zoids twin-stick (NM00016/25): custom JVS switch-word wiring, mapped live via the I/O-TEST SWITCH TEST.
// Left lever -> left stick, right lever -> right stick; Start/Service on the standard JVS bits.
static constexpr const std::array<InputBindingInfo, 14> s_twinstick_p1_button_bindings = {{
	{"P1_LLeverUp",    TRANSLATE_NOOP("JVS", "P1 Left Lever Up"),     nullptr, InputBindingInfo::Type::Button,   0x0001, GenericInputBinding::LeftStickUp},
	{"P1_LLeverDown",  TRANSLATE_NOOP("JVS", "P1 Left Lever Down"),   nullptr, InputBindingInfo::Type::Button,   0x8000, GenericInputBinding::LeftStickDown},
	{"P1_LLeverLeft",  TRANSLATE_NOOP("JVS", "P1 Left Lever Left"),   nullptr, InputBindingInfo::Type::Button,   0x4000, GenericInputBinding::LeftStickLeft},
	{"P1_LLeverRight", TRANSLATE_NOOP("JVS", "P1 Left Lever Right"),  nullptr, InputBindingInfo::Type::Button,   0x2000, GenericInputBinding::LeftStickRight},
	{"P1_RLeverUp",    TRANSLATE_NOOP("JVS", "P1 Right Lever Up"),    nullptr, InputBindingInfo::Type::Button,   0x0010, GenericInputBinding::RightStickUp},
	{"P1_RLeverDown",  TRANSLATE_NOOP("JVS", "P1 Right Lever Down"),  nullptr, InputBindingInfo::Type::Button,   0x0008, GenericInputBinding::RightStickDown},
	{"P1_RLeverLeft",  TRANSLATE_NOOP("JVS", "P1 Right Lever Left"),  nullptr, InputBindingInfo::Type::Button,   0x0004, GenericInputBinding::RightStickLeft},
	{"P1_RLeverRight", TRANSLATE_NOOP("JVS", "P1 Right Lever Right"), nullptr, InputBindingInfo::Type::Button,   0x0002, GenericInputBinding::RightStickRight},
	{"P1_LTrigger",    TRANSLATE_NOOP("JVS", "P1 Left Trigger"),      nullptr, InputBindingInfo::Type::HalfAxis, 0x0400, GenericInputBinding::L2},
	{"P1_RTrigger",    TRANSLATE_NOOP("JVS", "P1 Right Trigger"),     nullptr, InputBindingInfo::Type::HalfAxis, 0x1000, GenericInputBinding::R2},
	{"P1_LButton",     TRANSLATE_NOOP("JVS", "P1 Left Button"),       nullptr, InputBindingInfo::Type::Button,   0x0200, GenericInputBinding::L1},
	{"P1_RButton",     TRANSLATE_NOOP("JVS", "P1 Right Button"),      nullptr, InputBindingInfo::Type::Button,   0x0800, GenericInputBinding::R1},
	{"P1_Start",       TRANSLATE_NOOP("JVS", "P1 Start"),            nullptr, InputBindingInfo::Type::Button,   JVS_BTN_START,   GenericInputBinding::Start},
	{"P1_Service",     TRANSLATE_NOOP("JVS", "P1 Service"),          nullptr, InputBindingInfo::Type::Button,   JVS_BTN_SERVICE, GenericInputBinding::Select},
}};

// Per-layout fighting action buttons (real labels + per-layout config keys), selected by gameid via
// GetFightingButtons. D-pad/Start stay in the global P1/P2 tables; generic_mapping = the PS2-port default.
static constexpr InputBindingInfo s_fight_tekken[] = {
	{"Tekken_LeftPunch",  TRANSLATE_NOOP("JVS", "Left Punch"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"Tekken_RightPunch", TRANSLATE_NOOP("JVS", "Right Punch"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"Tekken_LeftKick",   TRANSLATE_NOOP("JVS", "Left Kick"),   nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Cross},
	{"Tekken_RightKick",  TRANSLATE_NOOP("JVS", "Right Kick"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_5, GenericInputBinding::Circle},
};
static constexpr InputBindingInfo s_fight_soulcal[] = {
	{"SoulCal_Horizontal", TRANSLATE_NOOP("JVS", "Horizontal Attack"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"SoulCal_Vertical",   TRANSLATE_NOOP("JVS", "Vertical Attack"),   nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"SoulCal_Kick",       TRANSLATE_NOOP("JVS", "Kick"),              nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::Circle},
	{"SoulCal_Guard",      TRANSLATE_NOOP("JVS", "Guard"),             nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Cross},
};
static constexpr InputBindingInfo s_fight_sixbutton[] = {
	{"SixButton_LightPunch",  TRANSLATE_NOOP("JVS", "Light Punch"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"SixButton_MediumPunch", TRANSLATE_NOOP("JVS", "Medium Punch"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"SixButton_HeavyPunch",  TRANSLATE_NOOP("JVS", "Heavy Punch"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::L1},
	{"SixButton_LightKick",   TRANSLATE_NOOP("JVS", "Light Kick"),   nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Cross},
	{"SixButton_MediumKick",  TRANSLATE_NOOP("JVS", "Medium Kick"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_5, GenericInputBinding::Circle},
	{"SixButton_HeavyKick",   TRANSLATE_NOOP("JVS", "Heavy Kick"),   nullptr, InputBindingInfo::Type::Button, JVS_BTN_6, GenericInputBinding::R1},
};
static constexpr InputBindingInfo s_fight_gundam[] = {
	{"Gundam_Shoot",  TRANSLATE_NOOP("JVS", "Shoot"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"Gundam_Melee",  TRANSLATE_NOOP("JVS", "Melee"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"Gundam_Jump",   TRANSLATE_NOOP("JVS", "Jump"),   nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::Cross},
	{"Gundam_Target", TRANSLATE_NOOP("JVS", "Target"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Circle},
};
static constexpr InputBindingInfo s_fight_bloodyroar[] = {
	{"BloodyRoar_Punch", TRANSLATE_NOOP("JVS", "Punch"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"BloodyRoar_Kick",  TRANSLATE_NOOP("JVS", "Kick"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Cross},
	{"BloodyRoar_Beast", TRANSLATE_NOOP("JVS", "Beast"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::Circle},
	{"BloodyRoar_Block", TRANSLATE_NOOP("JVS", "Block"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Triangle},
};

// Per-game fighting layouts: identical button scheme to the parent franchise (same JVS bits + host
// defaults), but their own config keys so each game can be remapped independently of the others.
static constexpr InputBindingInfo s_fight_fate[] = { // Fate: Unlimited Codes (PS2 manual: Weak/Medium/Strong + Reflect Guard)
	{"Fate_Weak",   TRANSLATE_NOOP("JVS", "Weak Attack"),   nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"Fate_Medium", TRANSLATE_NOOP("JVS", "Medium Attack"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"Fate_Strong", TRANSLATE_NOOP("JVS", "Strong Attack"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::Circle},
	{"Fate_Guard",  TRANSLATE_NOOP("JVS", "Reflect Guard"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Cross},
};
static constexpr InputBindingInfo s_fight_kinnikuman[] = { // Kinnikuman MGP 1 & 2: Attack, Throw/Grab, Special, Guard
	{"Kinnikuman_Attack",    TRANSLATE_NOOP("JVS", "Attack"),     nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"Kinnikuman_ThrowGrab", TRANSLATE_NOOP("JVS", "Throw/Grab"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"Kinnikuman_Special",   TRANSLATE_NOOP("JVS", "Special"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::Circle},
	{"Kinnikuman_Guard",     TRANSLATE_NOOP("JVS", "Guard"),      nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Cross},
};
static constexpr InputBindingInfo s_fight_pridegp[] = { // Pride GP 2003 (limb-based: Left/Right Punch + Kick)
	{"PrideGP_LeftPunch",  TRANSLATE_NOOP("JVS", "Left Punch"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"PrideGP_RightPunch", TRANSLATE_NOOP("JVS", "Right Punch"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"PrideGP_LeftKick",   TRANSLATE_NOOP("JVS", "Left Kick"),   nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Cross},
	{"PrideGP_RightKick",  TRANSLATE_NOOP("JVS", "Right Kick"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_5, GenericInputBinding::Circle},
};
static constexpr InputBindingInfo s_fight_basara[] = { // Sengoku Basara X (US manual: Weak/Medium/Strong + Striker)
	{"Basara_Weak",    TRANSLATE_NOOP("JVS", "Weak Attack"),   nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"Basara_Medium",  TRANSLATE_NOOP("JVS", "Medium Attack"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"Basara_Strong",  TRANSLATE_NOOP("JVS", "Strong Attack"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::Circle},
	{"Basara_Striker", TRANSLATE_NOOP("JVS", "Striker"),       nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Cross},
};
static constexpr InputBindingInfo s_fight_sdbz[] = { // Super Dragon Ball Z (PS2 manual: Light/Heavy Attack, Jump, Guard)
	{"DragonBallZ_Light", TRANSLATE_NOOP("JVS", "Light Attack"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"DragonBallZ_Heavy", TRANSLATE_NOOP("JVS", "Heavy Attack"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"DragonBallZ_Guard", TRANSLATE_NOOP("JVS", "Guard"),        nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Cross},
	{"DragonBallZ_Jump",  TRANSLATE_NOOP("JVS", "Jump"),         nullptr, InputBindingInfo::Type::Button, JVS_BTN_5, GenericInputBinding::Circle},
};
// YuYu Hakusho: Deathmatch (versus, 3 buttons: Punch/Kick/Guard).
static constexpr InputBindingInfo s_fight_yuyu[] = {
	{"YuYu_Punch", TRANSLATE_NOOP("JVS", "Punch"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"YuYu_Kick",  TRANSLATE_NOOP("JVS", "Kick"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"YuYu_Guard", TRANSLATE_NOOP("JVS", "Guard"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::Cross},
};

// Fighting page: section title + action buttons per layout.
static constexpr ACJV::LayoutInfo s_fighting_layout_ui[] = {
	{"Tekken (TK4 / TK5 / TK5DR)", s_fight_tekken,     false},
	{"Soul Calibur (SC2 / SC3)",   s_fight_soulcal,    false},
	{"Gundam VS",                  s_fight_gundam,     true},  // 1 player per cabinet (versus is networked)
	{"Bloody Roar 3",              s_fight_bloodyroar, false},
	{"Fate: Unlimited Codes",      s_fight_fate,       false},
	{"Kinnikuman MGP 1 / 2",       s_fight_kinnikuman, false},
	{"Pride GP 2003",              s_fight_pridegp,    false},
	{"Sengoku Basara X",           s_fight_basara,     false},
	{"Super Dragon Ball Z",        s_fight_sdbz,       false},
	{"YuYu Hakusho: Deathmatch",   s_fight_yuyu,       false},
	{"Capcom Fighting Jam",        s_fight_sixbutton,  false}, // 6 buttons -> last so the 4-button sections pair evenly
};

// Per-layout racing buttons (real labels + per-game keys); 1 player per cabinet. Analog steering/pedals
// live in s_jvs_wheel_bindings (shared); menu nav = D-pad Up/Down (System).
static constexpr InputBindingInfo s_race_universal[] = { // Wangan/RRV/MotoGP/Ace Driver 3 (View = Sw2|Push9)
	{"Racing_ShiftUp",   TRANSLATE_NOOP("JVS", "Shift Up"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::R1},
	{"Racing_ShiftDown", TRANSLATE_NOOP("JVS", "Shift Down"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::L1},
	{"Racing_View",      TRANSLATE_NOOP("JVS", "View Change"), nullptr, InputBindingInfo::Type::Button, static_cast<u16>(JVS_BTN_2 | JVS_BTN_9), GenericInputBinding::Triangle},
};
static constexpr InputBindingInfo s_race_bg3[] = { // Battle Gear 3 (switch chain @0x1e3f90)
	{"BG3_ShiftUp",   TRANSLATE_NOOP("JVS", "Shift Up"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_RIGHT, GenericInputBinding::R1},
	{"BG3_ShiftDown", TRANSLATE_NOOP("JVS", "Shift Down"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_1,     GenericInputBinding::L1},
	{"BG3_View",      TRANSLATE_NOOP("JVS", "View Change"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_DOWN,  GenericInputBinding::Triangle},
	{"BG3_Sidebrake", TRANSLATE_NOOP("JVS", "Sidebrake"),   nullptr, InputBindingInfo::Type::Button, JVS_BTN_LEFT,  GenericInputBinding::Square},
	{"BG3_Hazard",    TRANSLATE_NOOP("JVS", "Hazard"),      nullptr, InputBindingInfo::Type::Button, JVS_BTN_UP,    GenericInputBinding::Circle},
};

static constexpr ACJV::RacingLayoutInfo s_racing_layout_ui[] = {
	{"Racing (Shift / View)", s_race_universal},
	{"Battle Gear 3 / Tuned", s_race_bg3},
};

// Per-layout standard action buttons
static constexpr InputBindingInfo s_standard_smashcourt[] = {
	{"Smash_TopSpin", TRANSLATE_NOOP("JVS", "Top Spin"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Cross},
	{"Smash_Slice",   TRANSLATE_NOOP("JVS", "Slice"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Square},
};
static constexpr InputBindingInfo s_standard_technicbeat[] = {
	{"Technic_Activate", TRANSLATE_NOOP("JVS", "Activate"),     nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"Technic_Action",   TRANSLATE_NOOP("JVS", "Action"),       nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Cross},
	{"Technic_Super",    TRANSLATE_NOOP("JVS", "Super Action"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::Circle},
};
static constexpr InputBindingInfo s_standard_baseball[] = {
	{"Baseball_A", TRANSLATE_NOOP("JVS", "Swing / Throw"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Cross},
	{"Baseball_B", TRANSLATE_NOOP("JVS", "Full Swing / Run"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Square},
	{"Baseball_C", TRANSLATE_NOOP("JVS", "Relay Throw"),      nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::Triangle},
};
// Quiz Inufuku 2 (NM00037): answer buttons 1-4, wired on the Up/Down/Left/Right switch bits.
static constexpr InputBindingInfo s_standard_inufuku[] = {
	{"Inufuku_1", TRANSLATE_NOOP("JVS", "Button 1"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_UP,    GenericInputBinding::Triangle},
	{"Inufuku_2", TRANSLATE_NOOP("JVS", "Button 2"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_DOWN,  GenericInputBinding::Cross},
	{"Inufuku_3", TRANSLATE_NOOP("JVS", "Button 3"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_LEFT,  GenericInputBinding::Square},
	{"Inufuku_4", TRANSLATE_NOOP("JVS", "Button 4"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_RIGHT, GenericInputBinding::Circle},
};
// Gundam Quiz Warrior (NM00030): a quiz, but on the same 4-button wiring as the Gundam VS games (own keys).
static constexpr InputBindingInfo s_standard_gundamquiz[] = {
	{"GundamQuiz_Target", TRANSLATE_NOOP("JVS", "Answer A"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_4, GenericInputBinding::Circle},
	{"GundamQuiz_Shoot",  TRANSLATE_NOOP("JVS", "Answer B"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_1, GenericInputBinding::Square},
	{"GundamQuiz_Melee",  TRANSLATE_NOOP("JVS", "Answer C"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2, GenericInputBinding::Triangle},
	{"GundamQuiz_Jump",   TRANSLATE_NOOP("JVS", "Answer D"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_3, GenericInputBinding::Cross},
};

// Standard page: section title + action buttons per layout.
static constexpr ACJV::LayoutInfo s_standard_layout_ui[] = {
	{"Smash Court Pro Tournament", s_standard_smashcourt,  false, TRANSLATE_NOOP("JVS", "Top Spin + Slice together = Lob / Drop")},
	{"Technic Beat",               s_standard_technicbeat, false},
	{"Necchuu! Pro Yakyuu 2002",   s_standard_baseball,    false},
	{"Gundam Quiz Warrior",        s_standard_gundamquiz,  false},
	{"Quiz Suku Suku Inufuku 2",   s_standard_inufuku,     false, TRANSLATE_NOOP("JVS", "Answer buttons 1-4 sit on the Up/Down/Left/Right switches")},
};

static constexpr const std::array<InputBindingInfo, 2> s_jvs_coin_bindings = {{
	{"Coin1", TRANSLATE_NOOP("JVS", "Insert Coin P1"), nullptr, InputBindingInfo::Type::Button, 0, GenericInputBinding::Unknown},
	{"Coin2", TRANSLATE_NOOP("JVS", "Insert Coin P2"), nullptr, InputBindingInfo::Type::Button, 1, GenericInputBinding::Unknown},
}};
