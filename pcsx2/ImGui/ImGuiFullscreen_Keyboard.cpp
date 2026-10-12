// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ImGui/ImGuiFullscreen_Keyboard.h"

// FullscreenUI_Internal.h is only good for the FSUI_* translation macros here, but it expects these before it.
#include "Achievements.h"
#include "Config.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GameList.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "VMManager.h"
#include "ImGui/FullscreenUI_Internal.h"

#include "ImGui/ImGuiManager.h"
#include "Input/InputManager.h"

#include "common/StringUtil.h"

#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace OSK = OnScreenKeyboard;

namespace ImGuiFullscreen
{
	namespace
	{
		// Sizes are in layout units (LayoutScale turns them into pixels). The letters keyboard is ten
		// columns wide; the number pads have bigger keys because they have fewer.
		constexpr float KEY_GAP = 6.0f;
		constexpr float LETTERS_KEY_UNIT = 84.0f;
		constexpr float LETTERS_KEY_HEIGHT = 54.0f;
		constexpr float PAD_KEY_UNIT = 128.0f;
		constexpr float PAD_KEY_HEIGHT = 62.0f;
		constexpr float TEXT_LINE_HEIGHT = 54.0f;
		constexpr float TEXT_LINE_PADDING = 14.0f;
		constexpr float SECTION_GAP = 12.0f;
		constexpr float HINT_GAP = 22.0f;
		/// DrawInputDialog's WindowPadding. The window is exactly as wide as the keyboard plus this on both sides.
		constexpr float DIALOG_PADDING = 20.0f;

		bool IsPad(OSK::Page page)
		{
			return page == OSK::Page::Numpad || page == OSK::Page::NumpadDot;
		}

		float KeyUnit(OSK::Page page)
		{
			return LayoutScale(IsPad(page) ? PAD_KEY_UNIT : LETTERS_KEY_UNIT);
		}

		float KeyHeight(OSK::Page page)
		{
			return LayoutScale(IsPad(page) ? PAD_KEY_HEIGHT : LETTERS_KEY_HEIGHT);
		}

		float KeyboardWidth(const OSK::Layout& layout, OSK::Page page)
		{
			return static_cast<float>(layout.columns) * KeyUnit(page) + static_cast<float>(layout.columns - 1) * LayoutScale(KEY_GAP);
		}

		OSK::CharSet ToCharSet(InputFilterType filter)
		{
			switch (filter)
			{
				case InputFilterType::Numeric:
					return OSK::CharSet::Digits;
				case InputFilterType::IPAddress:
					return OSK::CharSet::IPv4;
				case InputFilterType::Password:
				case InputFilterType::None:
				default:
					return OSK::CharSet::Any;
			}
		}

		KeyboardAction Merge(KeyboardAction a, OSK::Outcome b)
		{
			switch (b)
			{
				case OSK::Outcome::Submit:
					return KeyboardAction::Submit;
				case OSK::Outcome::Cancel:
					return KeyboardAction::Cancel;
				case OSK::Outcome::None:
				default:
					return a;
			}
		}

		//////////////////////////////////////////////////////////////////////////
		// Input
		//////////////////////////////////////////////////////////////////////////

		// The D-pad repeats at the pace of menu navigation; the left stick arrives as the same keys.
		constexpr ImGuiInputFlags MOVE_REPEAT = ImGuiInputFlags_Repeat | ImGuiInputFlags_RepeatRateNavMove;

		// Backspace is ImGui's west face button (Square on a PlayStation pad) and Space its north one (Triangle). A user
		// who swapped North and West (ImGuiManager::IsGamepadNorthWestSwapped) has already had the swap applied
		// by the time ImGui sees the buttons, so only the glyphs in the hints have to know about it.
		constexpr ImGuiKey BACKSPACE_BUTTON = ImGuiKey_GamepadFaceLeft;
		constexpr ImGuiKey SPACE_BUTTON = ImGuiKey_GamepadFaceUp;

		ImGuiKey ConfirmButton()
		{
			return ImGui::GetIO().ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
		}

		OSK::Outcome ReadController(OSK::Model& m)
		{
			OSK::Outcome outcome = OSK::Outcome::None;

			if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, MOVE_REPEAT))
				m.MoveHighlight(OSK::Direction::Left);
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, MOVE_REPEAT))
				m.MoveHighlight(OSK::Direction::Right);
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, MOVE_REPEAT))
				m.MoveHighlight(OSK::Direction::Up);
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, MOVE_REPEAT))
				m.MoveHighlight(OSK::Direction::Down);

			if (ImGui::IsKeyPressed(ConfirmButton(), false))
			{
				const OSK::Outcome pressed = m.PressHighlighted();
				if (pressed != OSK::Outcome::None)
					outcome = pressed;
			}

			if (ImGui::IsKeyPressed(BACKSPACE_BUTTON, MOVE_REPEAT))
				m.Backspace();
			if (ImGui::IsKeyPressed(SPACE_BUTTON, false))
				m.Insert(" ");

			if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, MOVE_REPEAT))
				m.MoveCursor(-1);
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, MOVE_REPEAT))
				m.MoveCursor(1);

			if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false))
				m.ToggleShift();
			if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false))
				m.ToggleSymbols();

			if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) && m.CanSubmit())
				outcome = OSK::Outcome::Submit;

			return outcome;
		}

		OSK::Outcome ReadKeyboard(OSK::Model& m)
		{
			ImGuiIO& io = ImGui::GetIO();

			// Typing goes to the text as in any text box, whichever keys are on the screen.
			std::string typed;
			for (const ImWchar c : io.InputQueueCharacters)
				StringUtil::EncodeAndAppendUTF8(typed, c);
			if (!typed.empty())
				m.Insert(typed);

			if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true))
				m.Backspace();
			if (ImGui::IsKeyPressed(ImGuiKey_Delete, true))
				m.Delete();
			if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
				m.MoveCursor(-1);
			if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
				m.MoveCursor(1);
			if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
				m.MoveCursorToStart();
			if (ImGui::IsKeyPressed(ImGuiKey_End, false))
				m.MoveCursorToEnd();
			if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
			{
				if (const char* clip = ImGui::GetClipboardText())
					m.Insert(clip);
			}

			if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && m.CanSubmit())
				return OSK::Outcome::Submit;
			return OSK::Outcome::None;
		}

		//////////////////////////////////////////////////////////////////////////
		// Drawing
		//////////////////////////////////////////////////////////////////////////

		// Everything is drawn in the dialog's text colour at different strengths, so it reads on every theme.
		ImU32 InkColor(float alpha)
		{
			return ImGui::GetColorU32(ModAlpha(UIBackgroundTextColor, alpha));
		}

		float Luminance(const ImVec4& c)
		{
			const auto linear = [](float v) { return (v <= 0.03928f) ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
			return 0.2126f * linear(c.x) + 0.7152f * linear(c.y) + 0.0722f * linear(c.z);
		}

		// Black or white, whichever reads better on `fill`. The selected key's colour is the theme's secondary
		// colour, which is dark blue on some themes and light grey on others.
		ImVec4 InkOn(const ImVec4& fill)
		{
			const float l = Luminance(fill);
			return ((1.05f / (l + 0.05f)) >= ((l + 0.05f) / 0.05f)) ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
		}

		// What a key shows. The characters show what they type (so shift is visible), the rest a symbol or a word.
		std::string LabelFor(const OSK::Model& m, const OSK::Key& key, const char* ok_text)
		{
			switch (key.kind)
			{
				case OSK::KeyKind::Character:
					return m.KeyText(key);
				case OSK::KeyKind::Space:
					return FSUI_STR("Space");
				case OSK::KeyKind::Backspace:
					return ICON_FA_DELETE_LEFT;
				case OSK::KeyKind::Shift:
					return ICON_FA_ARROW_UP;
				case OSK::KeyKind::Symbols:
					return (m.CurrentPage() == OSK::Page::Symbols) ? "abc" : "#+=";
				case OSK::KeyKind::Ok:
					return ok_text;
				case OSK::KeyKind::Cancel:
					return std::string(FSUI_ICONSTR(ICON_FA_XMARK, "Cancel"));
			}
			return {};
		}

		// The text up to the first space: for "<icon> OK" that is the icon, which is what a key too narrow for the wording shows.
		std::string FirstWord(const std::string& s)
		{
			const size_t space = s.find(' ');
			return (space == std::string::npos) ? s : s.substr(0, space);
		}

		void DrawKey(ImDrawList* dl, const ImVec2& min, const ImVec2& max, const OSK::Model& m, const OSK::Key& key, bool highlighted,
			bool held, const char* ok_text)
		{
			const float rounding = LayoutScale(LAYOUT_FRAME_ROUNDING);
			const bool is_function = (key.kind != OSK::KeyKind::Character);
			const bool shift_on = (key.kind == OSK::KeyKind::Shift && m.Shift() != OSK::ShiftState::Off);
			const bool disabled = (key.kind == OSK::KeyKind::Ok && !m.CanSubmit());

			// The selected key and an active shift are filled with the theme's secondary colour (lighter while pressed).
			const ImVec4 accent = held ? UISecondaryStrongColor : UISecondaryColor;
			const ImVec4 accent_ink = InkOn(accent);
			ImU32 fill;
			if (highlighted)
				fill = ImGui::GetColorU32(accent);
			else if (shift_on)
				fill = ImGui::GetColorU32(ModAlpha(accent, 0.85f));
			else
				fill = InkColor(is_function ? 0.20f : 0.11f);
			dl->AddRectFilled(min, max, fill, rounding);

			if (highlighted)
			{
				// The same slow pulse the menu rows' selection border has.
				const float t = std::min<float>(std::abs(std::sin(ImGui::GetTime() * 0.75) * 1.1), 1.0f);
				dl->AddRect(min, max, ImGui::GetColorU32(ModAlpha(accent_ink, 0.35f + 0.65f * t)), rounding, LayoutScale(2.0f));
			}
			else
			{
				dl->AddRect(min, max, InkColor(0.18f), rounding, LayoutScale(1.0f));
			}

			std::string label = LabelFor(m, key, ok_text);
			const std::pair<ImFont*, float> font = g_large_font;
			const float pad = LayoutScale(6.0f);
			float text_w = font.first->CalcTextSizeA(font.second, FLT_MAX, 0.0f, label.c_str()).x;
			if (text_w > (max.x - min.x) - pad * 2.0f)
			{
				label = FirstWord(label);
				text_w = font.first->CalcTextSizeA(font.second, FLT_MAX, 0.0f, label.c_str()).x;
			}

			ImU32 ink = highlighted || shift_on ? ImGui::GetColorU32(accent_ink) : InkColor(1.0f);
			if (disabled)
				ink = InkColor(0.35f);
			const ImVec2 pos(min.x + ((max.x - min.x) - text_w) * 0.5f, min.y + ((max.y - min.y) - font.second) * 0.5f);
			dl->PushClipRect(min, max, true);
			dl->AddText(font.first, font.second, pos, ink, label.c_str());
			if (key.kind == OSK::KeyKind::Shift && m.Shift() == OSK::ShiftState::Locked)
			{
				// Caps lock: a bar under the arrow.
				const float bar_y = pos.y + font.second + LayoutScale(1.0f);
				dl->AddRectFilled(ImVec2(pos.x, bar_y), ImVec2(pos.x + text_w, bar_y + LayoutScale(3.0f)), ink);
			}
			dl->PopClipRect();
		}

		// Index into `text` of the character boundary nearest to the horizontal pixel offset `x` from the start of the text.
		size_t CursorForOffset(const std::string& text, float x)
		{
			const std::pair<ImFont*, float> font = g_large_font;
			size_t best = 0;
			float best_dist = std::abs(x);
			for (size_t pos = 0; pos < text.size();)
			{
				char32_t unused;
				pos += StringUtil::DecodeUTF8(text, pos, &unused);
				const float w = font.first->CalcTextSizeA(font.second, FLT_MAX, 0.0f, text.c_str(), text.c_str() + pos).x;
				const float dist = std::abs(x - w);
				if (dist < best_dist)
				{
					best = pos;
					best_dist = dist;
				}
			}
			return best;
		}

		void DrawTextLine(KeyboardEntry& entry, float width)
		{
			OSK::Model& m = entry.model;
			const std::pair<ImFont*, float> font = g_large_font;
			const float height = LayoutScale(TEXT_LINE_HEIGHT);
			const float pad = LayoutScale(TEXT_LINE_PADDING);
			const float rounding = LayoutScale(LAYOUT_FRAME_ROUNDING);

			const ImVec2 min = ImGui::GetCursorScreenPos();
			const ImVec2 max(min.x + width, min.y + height);
			ImGui::InvisibleButton("##text", ImVec2(width, height), ImGuiButtonFlags_PressedOnClick);
			const bool clicked = ImGui::IsItemActivated();

			// A password is drawn as stars, one for each character, so the cursor's place in it is a count of characters.
			const std::string shown = entry.mask ? std::string(m.CharCount(), '*') : std::string();
			const std::string& text = entry.mask ? shown : m.Text();
			const size_t cursor = entry.mask ? m.CursorChars() : m.Cursor();
			const float inner = width - pad * 2.0f;
			const float caret_w = std::max(1.0f, LayoutScale(2.0f));
			const float text_w = font.first->CalcTextSizeA(font.second, FLT_MAX, 0.0f, text.c_str()).x;
			const float cursor_x = font.first->CalcTextSizeA(font.second, FLT_MAX, 0.0f, text.c_str(), text.c_str() + cursor).x;

			// Keep the cursor in view: scroll just far enough, and not past the end of the text.
			float& scroll = entry.text_scroll;
			scroll = std::clamp(scroll, 0.0f, std::max(0.0f, text_w + caret_w - inner));
			if (cursor_x - scroll > inner - caret_w)
				scroll = cursor_x + caret_w - inner;
			if (cursor_x < scroll)
				scroll = cursor_x;

			if (clicked)
			{
				const float x = ImGui::GetIO().MousePos.x - (min.x + pad) + scroll;
				if (entry.mask)
					m.SetCursorChars(CursorForOffset(text, x)); // a star is one byte, so the offset is the character count
				else
					m.SetCursor(CursorForOffset(text, x));
			}

			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(min, max, InkColor(0.10f), rounding);
			dl->AddRect(min, max, InkColor(0.35f), rounding, LayoutScale(1.0f));

			dl->PushClipRect(ImVec2(min.x + pad, min.y), ImVec2(max.x - pad, max.y), true);
			const float text_y = min.y + (height - font.second) * 0.5f;
			dl->AddText(font.first, font.second, ImVec2(min.x + pad - scroll, text_y), InkColor(1.0f), text.c_str());
			const float caret_x = min.x + pad - scroll + cursor_x;
			dl->AddRectFilled(ImVec2(caret_x, text_y), ImVec2(caret_x + caret_w, text_y + font.second), InkColor(1.0f));
			dl->PopClipRect();
		}

		OSK::Outcome DrawKeys(KeyboardEntry& entry, float width, float height, const char* ok_text)
		{
			OSK::Model& m = entry.model;
			const OSK::Layout& layout = m.CurrentLayout();
			const OSK::Page page = m.CurrentPage();
			const float unit = KeyUnit(page);
			const float gap = LayoutScale(KEY_GAP);
			const bool mouse_moved = (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f);

			OSK::Outcome outcome = OSK::Outcome::None;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const float left = origin.x + (width - KeyboardWidth(layout, page)) * 0.5f;
			const OSK::KeyPos highlight = m.Highlight();

			float y = origin.y;
			for (u32 row = 0; row < layout.row_count; row++)
			{
				float x = left;
				for (u32 col = 0; col < layout.rows[row].count; col++)
				{
					const OSK::Key& key = layout.rows[row].keys[col];
					const float w = static_cast<float>(key.span) * unit + static_cast<float>(key.span - 1) * gap;
					const ImVec2 min(x, y);
					const ImVec2 max(x + w, y + height);

					ImGui::SetCursorScreenPos(min);
					ImGui::PushID(static_cast<int>(row * 16 + col));
					// Holding Backspace repeats it.
					ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, key.kind == OSK::KeyKind::Backspace);
					const bool pressed = ImGui::InvisibleButton("##key", ImVec2(w, height), ImGuiButtonFlags_PressedOnClick);
					ImGui::PopItemFlag();
					const bool hovered = ImGui::IsItemHovered();
					const bool held = ImGui::IsItemActive();
					ImGui::PopID();

					// The mouse moves the selection only when it moves, so it does not undo what the D-pad just did.
					if (hovered && mouse_moved)
						m.SetHighlight({row, col});
					if (pressed)
					{
						const OSK::Outcome result = m.Press({row, col});
						if (result != OSK::Outcome::None)
							outcome = result;
					}

					DrawKey(dl, min, max, m, key, highlight == OSK::KeyPos{row, col}, held, ok_text);
					x += w + gap;
				}
				y += height + gap;
			}

			// Moving the cursor around with SetCursorScreenPos needs an item at the end to grow the window.
			ImGui::SetCursorScreenPos(ImVec2(origin.x, y - gap));
			ImGui::Dummy(ImVec2(width, 0.0f));
			return outcome;
		}

		//////////////////////////////////////////////////////////////////////////
		// Button hints, in the style of the footer: glyph, then what it does.
		//////////////////////////////////////////////////////////////////////////

		struct Shoulders
		{
			const char* l1;
			const char* r1;
			const char* l2;
		};

		Shoulders GetShoulderGlyphs()
		{
			InputLayout layout = InputManager::GetGamepadIconPreference();
			if (layout == InputLayout::Unknown)
				layout = GetGamepadLayout();

			switch (layout)
			{
				case InputLayout::Xbox:
					return {ICON_PF_LEFT_SHOULDER_LB, ICON_PF_RIGHT_SHOULDER_RB, ICON_PF_LEFT_TRIGGER_LT};
				case InputLayout::Nintendo:
					return {ICON_PF_LEFT_SHOULDER_L, ICON_PF_RIGHT_SHOULDER_R, ICON_PF_LEFT_TRIGGER_ZL};
				default:
					return {ICON_PF_LEFT_SHOULDER_L1, ICON_PF_RIGHT_SHOULDER_R1, ICON_PF_LEFT_TRIGGER_L2};
			}
		}

		using Hint = std::pair<std::string, std::string>; ///< glyphs, then the words.

		std::vector<Hint> GetHints(const OSK::Model& m)
		{
			std::vector<Hint> hints;
			if (IsGamepadInputSource())
			{
				const bool circle_ok = ImGui::GetIO().ConfigNavSwapGamepadButtons;
				const GamepadGlyphs glyphs = GetGamepadGlyphs();
				const Shoulders shoulders = GetShoulderGlyphs();
				const bool swapped = ImGuiManager::IsGamepadNorthWestSwapped();
				const bool letters = !IsPad(m.CurrentPage());

				hints.emplace_back(glyphs.dpad, FSUI_STR("Move"));
				hints.emplace_back(glyphs.confirm(circle_ok), FSUI_STR("Type"));
				// Backspace is ImGui's west button and Space its north button, which are the other way round on the device when swapped.
				hints.emplace_back(swapped ? glyphs.north : glyphs.west, FSUI_STR("Backspace"));
				if (letters)
					hints.emplace_back(swapped ? glyphs.west : glyphs.north, FSUI_STR("Space"));
				hints.emplace_back(std::string(shoulders.l1) + shoulders.r1, FSUI_STR("Cursor"));
				const bool on_symbols = (m.CurrentPage() == OSK::Page::Symbols);
				if (letters && !on_symbols)
					hints.emplace_back(shoulders.l2, FSUI_STR("Shift"));
				if (letters)
					hints.emplace_back(glyphs.select, on_symbols ? FSUI_STR("Letters") : FSUI_STR("Symbols"));
				hints.emplace_back(glyphs.start, FSUI_STR("OK"));
				hints.emplace_back(glyphs.cancel(circle_ok), FSUI_STR("Cancel"));
			}
			else
			{
				hints.emplace_back(ICON_PF_ENTER, FSUI_STR("OK"));
				hints.emplace_back(ICON_PF_ESC, FSUI_STR("Cancel"));
			}
			return hints;
		}

		struct PlacedHint
		{
			std::string text;
			float width;
		};

		/// The hints broken into lines no wider than the keyboard, and how tall that is.
		struct HintLayout
		{
			std::vector<std::vector<PlacedHint>> lines;
			float line_height = 0.0f;
			float Height() const { return static_cast<float>(lines.size()) * line_height; }
		};

		HintLayout LayoutHints(const std::vector<Hint>& hints, float width)
		{
			const std::pair<ImFont*, float> font = g_medium_font;
			const float gap = LayoutScale(HINT_GAP);

			HintLayout layout;
			layout.line_height = font.second + LayoutScale(8.0f);
			layout.lines.emplace_back();
			float line_width = 0.0f;
			for (const Hint& hint : hints)
			{
				PlacedHint item{hint.first + " " + hint.second, 0.0f};
				item.width = font.first->CalcTextSizeA(font.second, FLT_MAX, 0.0f, item.text.c_str()).x;
				if (!layout.lines.back().empty() && line_width + gap + item.width > width)
				{
					layout.lines.emplace_back();
					line_width = 0.0f;
				}
				line_width += (layout.lines.back().empty() ? 0.0f : gap) + item.width;
				layout.lines.back().push_back(std::move(item));
			}
			return layout;
		}

		void DrawHints(const HintLayout& layout, float width)
		{
			const std::pair<ImFont*, float> font = g_medium_font;
			const float gap = LayoutScale(HINT_GAP);

			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			float y = origin.y;
			for (const std::vector<PlacedHint>& line : layout.lines)
			{
				float total = 0.0f;
				for (const PlacedHint& item : line)
					total += item.width + gap;
				total -= gap;

				float x = origin.x + (width - total) * 0.5f;
				for (const PlacedHint& item : line)
				{
					AddTextWithShadow(dl, font, ImVec2(x, y), InkColor(0.85f), item.text.c_str());
					x += item.width + gap;
				}
				y += layout.line_height;
			}
			ImGui::Dummy(ImVec2(width, y - origin.y));
		}
	} // namespace

	void KeyboardEntry::Begin(std::string text, InputFilterType filter)
	{
		model.Begin(std::move(text), ToCharSet(filter));
		text_scroll = 0.0f;
		mask = (filter == InputFilterType::Password);
	}

	float GetKeyboardEntryWidth(const KeyboardEntry& entry)
	{
		const OSK::Page page = entry.model.CurrentPage();
		return KeyboardWidth(OSK::GetLayout(page), page) + LayoutScale(DIALOG_PADDING) * 2.0f;
	}

	KeyboardAction DrawKeyboardEntry(KeyboardEntry& entry, const char* message, const char* caption, const char* ok_text)
	{
		OSK::Model& m = entry.model;
		KeyboardAction action = KeyboardAction::None;

		// A host that only forwards typed characters while a text box has the focus (Qt does) needs to be told
		// that this is one, since the text line is drawn here and not by ImGui::InputText.
		ImGui::GetCurrentContext()->PlatformImeData.WantTextInput = true;

		// The button press or click that opened the dialog is still in this frame; it must not type a key.
		if (!ImGui::IsWindowAppearing())
		{
			action = Merge(action, ReadController(m));
			action = Merge(action, ReadKeyboard(m));
			if (WantsToCloseMenu())
				action = KeyboardAction::Cancel;
		}

		const float width = ImGui::GetContentRegionAvail().x;
		const bool has_caption = (caption && caption[0] != '\0');
		const float section_gap = LayoutScale(SECTION_GAP);

		// A long message (and translations run long) must not push the keyboard off a small screen: shorten the keys
		// first and, if that is not enough, leave out the button hints.
		const ImGuiStyle& style = ImGui::GetStyle();
		const OSK::Layout& layout = m.CurrentLayout();
		const float rows = static_cast<float>(layout.row_count);
		const float full_key_height = KeyHeight(m.CurrentPage());
		HintLayout hints = LayoutHints(GetHints(m), width);
		const float title_bar = ImGui::GetFontSize() + style.FramePadding.y * 2.0f;
		const float other = style.WindowPadding.y * 2.0f + title_bar + ImGui::CalcTextSize(message, nullptr, false, width).y +
		                    (has_caption ? ImGui::GetTextLineHeightWithSpacing() : 0.0f) + section_gap * 3.0f +
		                    LayoutScale(TEXT_LINE_HEIGHT) + (rows - 1.0f) * LayoutScale(KEY_GAP) + style.ItemSpacing.y * 8.0f;
		const float available = ImGui::GetIO().DisplaySize.y - LayoutScale(LAYOUT_FOOTER_HEIGHT) - LayoutScale(12.0f);
		float key_height = full_key_height;
		float room_for_keys = available - other - hints.Height();
		if (rows * key_height > room_for_keys)
		{
			key_height = std::max(room_for_keys / rows, full_key_height * 0.7f);
			if (rows * key_height > room_for_keys)
			{
				hints.lines.clear();
				key_height = std::clamp((available - other) / rows, full_key_height * 0.7f, full_key_height);
			}
		}

		ImGui::PushStyleColor(ImGuiCol_Text, UIBackgroundTextColor);
		ImGui::TextWrapped("%s", message);
		ImGui::PopStyleColor();

		ImGui::Dummy(ImVec2(0.0f, section_gap));
		if (has_caption)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, UIBackgroundTextColor);
			ImGui::TextUnformatted(caption);
			ImGui::PopStyleColor();
		}

		DrawTextLine(entry, width);
		ImGui::Dummy(ImVec2(0.0f, section_gap));
		action = Merge(action, DrawKeys(entry, width, key_height, ok_text));
		if (!hints.lines.empty())
		{
			ImGui::Dummy(ImVec2(0.0f, section_gap));
			DrawHints(hints, width);
		}
		return action;
	}

	bool InputTextWithKeyboard(const char* label, const char* hint, char* buf, size_t buf_size, bool password, const char* message)
	{
		const bool edited = ImGui::InputTextWithHint(label, hint, buf, buf_size, password ? ImGuiInputTextFlags_Password : ImGuiInputTextFlags_None);

		// Activated by the keyboard (Enter or Space on a field reached with the arrow keys): the field takes the typing
		// itself. A click or a tap counts as the mouse here, which is also what a touch screen sends.
		if (!ImGui::IsItemActivated() || ImGui::GetCurrentContext()->ActiveIdSource == ImGuiInputSource_Keyboard)
			return edited;

		ImGui::ClearActiveID();
		const auto store_answer = [buf, buf_size](std::string text) { ImStrncpy(buf, text.c_str(), buf_size); };
		OpenInputStringDialog(hint, message, std::string(), FSUI_ICONSTR(ICON_FA_CHECK, "OK"), store_answer, std::string(buf),
			password ? InputFilterType::Password : InputFilterType::None);
		return false;
	}
} // namespace ImGuiFullscreen

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// Same scheme as FullscreenUI_Packs.cpp: tools/generate_fullscreen_ui_translation_strings.py regenerates the
// block below from the FSUI_* macros above.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", "Space");
TRANSLATE_NOOP("FullscreenUI", "Move");
TRANSLATE_NOOP("FullscreenUI", "Type");
TRANSLATE_NOOP("FullscreenUI", "Backspace");
TRANSLATE_NOOP("FullscreenUI", "Cursor");
TRANSLATE_NOOP("FullscreenUI", "Shift");
TRANSLATE_NOOP("FullscreenUI", "Letters");
TRANSLATE_NOOP("FullscreenUI", "Symbols");
TRANSLATE_NOOP("FullscreenUI", "OK");
TRANSLATE_NOOP("FullscreenUI", "Cancel");
// TRANSLATION-STRING-AREA-END
#endif
