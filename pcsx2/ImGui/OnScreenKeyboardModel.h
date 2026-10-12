// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The logic of Big Picture's on-screen keyboard with nothing drawn: the keys on each page, which
// key a controller has selected and where a direction moves it, and the text being edited with its
// cursor. ImGuiFullscreen_Keyboard.cpp draws this and feeds it input; keeping it free of ImGui
// lets tests/ctest/core/onscreen_keyboard_tests.cpp cover the rules without a window.

#include "common/Pcsx2Types.h"

#include <string>
#include <string_view>

namespace OnScreenKeyboard
{
	/// What a text entry accepts. Mirrors ImGuiFullscreen::InputFilterType, which this file does
	/// not include so that it builds without ImGui.
	enum class CharSet : u8
	{
		Any, ///< Anything printable.
		Digits, ///< 0-9 only: the number pad.
		IPv4, ///< 0-9 and '.': the number pad with a dot key.
	};

	/// True if a character typed on a physical keyboard, pasted, or pressed on the keyboard is
	/// allowed by `set`.
	bool Accepts(CharSet set, char32_t codepoint);

	enum class Page : u8
	{
		Letters, ///< Digits, QWERTY, a few punctuation keys.
		Symbols, ///< Digits and the rest of the ASCII punctuation.
		Numpad, ///< 0-9 for CharSet::Digits.
		NumpadDot, ///< 0-9 and '.' for CharSet::IPv4.
	};

	enum class KeyKind : u8
	{
		Character, ///< Types Key::normal, or Key::shifted while shift is on.
		Space,
		Backspace,
		Shift,
		Symbols, ///< Switches between the Letters and Symbols pages.
		Ok, ///< Submits the text (only while there is some).
		Cancel,
	};

	struct Key
	{
		KeyKind kind;
		const char* normal; ///< For Character keys: the text typed, and the label.
		const char* shifted; ///< For Character keys: the text and label while shift is on.
		u8 span; ///< Width in key units; a row's spans add up to Layout::columns.
	};

	struct Row
	{
		const Key* keys;
		u32 count;
	};

	struct Layout
	{
		const Row* rows;
		u32 row_count;
		u32 columns; ///< Width of the widest row in key units.
	};

	const Layout& GetLayout(Page page);

	struct KeyPos
	{
		u32 row = 0;
		u32 col = 0;
		bool operator==(const KeyPos& o) const { return row == o.row && col == o.col; }
	};

	enum class Direction : u8
	{
		Left,
		Right,
		Up,
		Down,
	};

	/// Shift on the Letters page. Once is the next character only; Locked is caps lock.
	enum class ShiftState : u8
	{
		Off,
		Once,
		Locked,
	};

	/// What pressing a key asks of the dialog around the keyboard.
	enum class Outcome : u8
	{
		None,
		Submit,
		Cancel,
	};

	class Model
	{
	public:
		/// Most characters a text entry holds unless the caller says otherwise. Keeps a held
		/// key from growing the string without end.
		static constexpr size_t kDefaultMaxChars = 512;

		/// Starts an entry: `text` is shown with the cursor after it, the page that suits `set`
		/// opens and the first key is selected. `max_chars` limits what can be added, not what
		/// `text` already holds.
		void Begin(std::string text, CharSet set, size_t max_chars = kDefaultMaxChars);

		//////////////////////////////////////////////////////////////////////////
		// The text. Offsets are bytes of UTF-8; every edit moves by whole characters.
		//////////////////////////////////////////////////////////////////////////

		const std::string& Text() const { return m_text; }
		size_t Cursor() const { return m_cursor; }

		/// Inserts at the cursor the characters of `utf8` that `set` accepts, up to the limit.
		/// Returns whether anything was inserted.
		bool Insert(std::string_view utf8);
		/// Deletes the character before the cursor / at the cursor. Return whether one existed.
		bool Backspace();
		bool Delete();
		/// Moves the cursor by `chars` characters (negative is left), stopping at either end.
		void MoveCursor(int chars);
		void MoveCursorToStart() { m_cursor = 0; }
		void MoveCursorToEnd() { m_cursor = m_text.size(); }
		/// Puts the cursor at `byte_offset`, moved back to the start of its character if that
		/// is inside one, and to the end of the text if it is past it.
		void SetCursor(size_t byte_offset);
		/// The cursor as a count of characters from the start, and a way to set it that way. A text
		/// shown as one star per character (a password) is placed by these.
		size_t CursorChars() const;
		void SetCursorChars(size_t chars);
		size_t CharCount() const;

		/// Whether OK is allowed to submit. An empty text cannot be.
		bool CanSubmit() const { return !m_text.empty(); }

		//////////////////////////////////////////////////////////////////////////
		// The keys
		//////////////////////////////////////////////////////////////////////////

		Page CurrentPage() const { return m_page; }
		const Layout& CurrentLayout() const { return GetLayout(m_page); }
		ShiftState Shift() const { return m_shift; }
		KeyPos Highlight() const { return m_highlight; }
		const Key& HighlightedKey() const { return KeyAt(m_highlight); }
		const Key& KeyAt(KeyPos pos) const;

		/// The label of `key` now, which is also what it types: the shifted text on the Letters
		/// page while shift is on, the normal text otherwise. Empty for keys that type nothing.
		const char* KeyText(const Key& key) const;

		/// Moves the selection one key. Left and right wrap within a row; up and down wrap
		/// from the first row to the last and keep to the same horizontal position, so the
		/// wide Space key is reached from any key above it.
		void MoveHighlight(Direction dir);
		/// Selects `pos` (a mouse hovering a key). Out-of-range positions are ignored.
		void SetHighlight(KeyPos pos);

		/// Does what the selected key does, and tells the dialog if that submits or cancels.
		Outcome PressHighlighted();
		/// SetHighlight(pos), then PressHighlighted().
		Outcome Press(KeyPos pos);

		/// The shortcut on the controller (L2). Off to Once and back; Locked goes to Off.
		void ToggleShift();
		/// The Shift key. Off to Once, Once to Locked, Locked to Off.
		void CycleShift();
		/// The shortcut on the controller (Select): the other of Letters and Symbols. Nothing
		/// on a number pad.
		void ToggleSymbols();

	private:
		static u32 CenterOf(const Row& row, u32 col);
		u32 ColumnAtCenter(const Row& row, u32 center) const;
		void SelectColumnNear(u32 row, u32 center);
		void SwitchPage(Page page);
		size_t PrevBoundary(size_t pos) const;
		size_t NextBoundary(size_t pos) const;

		std::string m_text;
		size_t m_cursor = 0;
		size_t m_max_chars = kDefaultMaxChars;
		CharSet m_set = CharSet::Any;
		Page m_page = Page::Letters;
		ShiftState m_shift = ShiftState::Off;
		KeyPos m_highlight;
		u32 m_goal_center = 1; ///< Where up and down aim, in half key units from the left edge.
	};
} // namespace OnScreenKeyboard
