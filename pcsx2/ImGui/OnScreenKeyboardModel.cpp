// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ImGui/OnScreenKeyboardModel.h"

#include "common/StringUtil.h"

#include <algorithm>
#include <iterator>

namespace OnScreenKeyboard
{
	namespace
	{
		constexpr Key Ch(const char* normal, const char* shifted, u8 span = 1)
		{
			return Key{KeyKind::Character, normal, shifted, span};
		}

		constexpr Key Fn(KeyKind kind, u8 span = 1)
		{
			return Key{kind, "", "", span};
		}

		// Letters page. Shift capitalizes the letters, and turns '-' into '_' and '.' into ':', which
		// is what a host name or an address with a port needs.
		constexpr Key kLettersRow0[] = {Ch("1", "1"), Ch("2", "2"), Ch("3", "3"), Ch("4", "4"), Ch("5", "5"), Ch("6", "6"), Ch("7", "7"),
			Ch("8", "8"), Ch("9", "9"), Ch("0", "0")};
		constexpr Key kLettersRow1[] = {Ch("q", "Q"), Ch("w", "W"), Ch("e", "E"), Ch("r", "R"), Ch("t", "T"), Ch("y", "Y"), Ch("u", "U"),
			Ch("i", "I"), Ch("o", "O"), Ch("p", "P")};
		constexpr Key kLettersRow2[] = {Ch("a", "A"), Ch("s", "S"), Ch("d", "D"), Ch("f", "F"), Ch("g", "G"), Ch("h", "H"), Ch("j", "J"),
			Ch("k", "K"), Ch("l", "L"), Ch("-", "_")};
		constexpr Key kLettersRow3[] = {Fn(KeyKind::Shift), Ch("z", "Z"), Ch("x", "X"), Ch("c", "C"), Ch("v", "V"), Ch("b", "B"),
			Ch("n", "N"), Ch("m", "M"), Ch(".", ":"), Fn(KeyKind::Backspace)};

		// Symbols page. The digits stay so that "192.168.0.1:7777" does not mean a page switch for every
		// number.
		constexpr Key kSymbolsRow0[] = {Ch("1", "1"), Ch("2", "2"), Ch("3", "3"), Ch("4", "4"), Ch("5", "5"), Ch("6", "6"), Ch("7", "7"),
			Ch("8", "8"), Ch("9", "9"), Ch("0", "0")};
		constexpr Key kSymbolsRow1[] = {Ch("!", "!"), Ch("@", "@"), Ch("#", "#"), Ch("$", "$"), Ch("%", "%"), Ch("^", "^"), Ch("&", "&"),
			Ch("*", "*"), Ch("(", "("), Ch(")", ")")};
		constexpr Key kSymbolsRow2[] = {Ch("-", "-"), Ch("_", "_"), Ch("=", "="), Ch("+", "+"), Ch("[", "["), Ch("]", "]"), Ch("{", "{"),
			Ch("}", "}"), Ch(":", ":"), Ch(";", ";")};
		constexpr Key kSymbolsRow3[] = {Ch("/", "/"), Ch("?", "?"), Ch(".", "."), Ch(",", ","), Ch("'", "'"), Ch("\"", "\""), Ch("\\", "\\"),
			Ch("|", "|"), Ch("~", "~"), Fn(KeyKind::Backspace)};

		// Under both pages. The Symbols key is labelled by the page it leads to.
		constexpr Key kBottomRow[] = {Fn(KeyKind::Symbols, 2), Fn(KeyKind::Space, 4), Fn(KeyKind::Ok, 2), Fn(KeyKind::Cancel, 2)};

		constexpr Row kLettersRows[] = {
			{kLettersRow0, static_cast<u32>(std::size(kLettersRow0))},
			{kLettersRow1, static_cast<u32>(std::size(kLettersRow1))},
			{kLettersRow2, static_cast<u32>(std::size(kLettersRow2))},
			{kLettersRow3, static_cast<u32>(std::size(kLettersRow3))},
			{kBottomRow, static_cast<u32>(std::size(kBottomRow))},
		};
		constexpr Row kSymbolsRows[] = {
			{kSymbolsRow0, static_cast<u32>(std::size(kSymbolsRow0))},
			{kSymbolsRow1, static_cast<u32>(std::size(kSymbolsRow1))},
			{kSymbolsRow2, static_cast<u32>(std::size(kSymbolsRow2))},
			{kSymbolsRow3, static_cast<u32>(std::size(kSymbolsRow3))},
			{kBottomRow, static_cast<u32>(std::size(kBottomRow))},
		};

		constexpr Key kPadRow0[] = {Ch("7", "7"), Ch("8", "8"), Ch("9", "9"), Fn(KeyKind::Backspace)};
		constexpr Key kPadRow1[] = {Ch("4", "4"), Ch("5", "5"), Ch("6", "6"), Fn(KeyKind::Ok)};
		constexpr Key kPadRow2[] = {Ch("1", "1"), Ch("2", "2"), Ch("3", "3"), Fn(KeyKind::Cancel)};
		constexpr Key kPadZero[] = {Ch("0", "0", 4)};
		constexpr Key kPadZeroDot[] = {Ch("0", "0", 3), Ch(".", ".")};

		constexpr Row kNumpadRows[] = {
			{kPadRow0, static_cast<u32>(std::size(kPadRow0))},
			{kPadRow1, static_cast<u32>(std::size(kPadRow1))},
			{kPadRow2, static_cast<u32>(std::size(kPadRow2))},
			{kPadZero, static_cast<u32>(std::size(kPadZero))},
		};
		constexpr Row kNumpadDotRows[] = {
			{kPadRow0, static_cast<u32>(std::size(kPadRow0))},
			{kPadRow1, static_cast<u32>(std::size(kPadRow1))},
			{kPadRow2, static_cast<u32>(std::size(kPadRow2))},
			{kPadZeroDot, static_cast<u32>(std::size(kPadZeroDot))},
		};

		constexpr Layout kLettersLayout{kLettersRows, static_cast<u32>(std::size(kLettersRows)), 10};
		constexpr Layout kSymbolsLayout{kSymbolsRows, static_cast<u32>(std::size(kSymbolsRows)), 10};
		constexpr Layout kNumpadLayout{kNumpadRows, static_cast<u32>(std::size(kNumpadRows)), 4};
		constexpr Layout kNumpadDotLayout{kNumpadDotRows, static_cast<u32>(std::size(kNumpadDotRows)), 4};

		bool IsContinuationByte(char c)
		{
			return (static_cast<u8>(c) & 0xC0) == 0x80;
		}
	} // namespace

	bool Accepts(CharSet set, char32_t codepoint)
	{
		const bool digit = (codepoint >= U'0' && codepoint <= U'9');
		switch (set)
		{
			case CharSet::Digits:
				return digit;
			case CharSet::IPv4:
				return digit || codepoint == U'.';
			case CharSet::Any:
			default:
				// Not control characters (Tab, Enter and the DEL some hosts send for Delete), and
				// nothing past Unicode.
				return codepoint >= 0x20 && codepoint != 0x7F && !(codepoint >= 0x80 && codepoint < 0xA0) && codepoint <= 0x10FFFF;
		}
	}

	const Layout& GetLayout(Page page)
	{
		switch (page)
		{
			case Page::Symbols:
				return kSymbolsLayout;
			case Page::Numpad:
				return kNumpadLayout;
			case Page::NumpadDot:
				return kNumpadDotLayout;
			case Page::Letters:
			default:
				return kLettersLayout;
		}
	}

	void Model::Begin(std::string text, CharSet set, size_t max_chars)
	{
		m_text = std::move(text);
		m_cursor = m_text.size();
		m_max_chars = max_chars;
		m_set = set;
		if (set == CharSet::Digits)
			m_page = Page::Numpad;
		else if (set == CharSet::IPv4)
			m_page = Page::NumpadDot;
		else
			m_page = Page::Letters;
		m_shift = ShiftState::Off;
		m_highlight = KeyPos{};
		m_goal_center = CenterOf(GetLayout(m_page).rows[0], 0);
	}

	size_t Model::CharCount() const
	{
		return static_cast<size_t>(std::count_if(m_text.begin(), m_text.end(), [](char c) { return !IsContinuationByte(c); }));
	}

	size_t Model::CursorChars() const
	{
		return static_cast<size_t>(std::count_if(m_text.begin(), m_text.begin() + m_cursor, [](char c) { return !IsContinuationByte(c); }));
	}

	void Model::SetCursorChars(size_t chars)
	{
		size_t pos = 0;
		for (; chars > 0 && pos < m_text.size(); chars--)
			pos = NextBoundary(pos);
		m_cursor = pos;
	}

	size_t Model::PrevBoundary(size_t pos) const
	{
		if (pos == 0)
			return 0;
		pos--;
		while (pos > 0 && IsContinuationByte(m_text[pos]))
			pos--;
		return pos;
	}

	size_t Model::NextBoundary(size_t pos) const
	{
		if (pos >= m_text.size())
			return m_text.size();
		pos++;
		while (pos < m_text.size() && IsContinuationByte(m_text[pos]))
			pos++;
		return pos;
	}

	bool Model::Insert(std::string_view utf8)
	{
		const size_t have = CharCount();
		size_t added = 0;
		std::string accepted;
		for (size_t offset = 0; offset < utf8.size() && have + added < m_max_chars;)
		{
			char32_t codepoint;
			offset += StringUtil::DecodeUTF8(utf8, offset, &codepoint);
			if (codepoint == 0xFFFFFFFFu || !Accepts(m_set, codepoint))
				continue;

			StringUtil::EncodeAndAppendUTF8(accepted, codepoint);
			added++;
		}

		if (accepted.empty())
			return false;

		m_text.insert(m_cursor, accepted);
		m_cursor += accepted.size();
		return true;
	}

	bool Model::Backspace()
	{
		if (m_cursor == 0)
			return false;

		const size_t start = PrevBoundary(m_cursor);
		m_text.erase(start, m_cursor - start);
		m_cursor = start;
		return true;
	}

	bool Model::Delete()
	{
		if (m_cursor >= m_text.size())
			return false;

		m_text.erase(m_cursor, NextBoundary(m_cursor) - m_cursor);
		return true;
	}

	void Model::MoveCursor(int chars)
	{
		for (; chars < 0 && m_cursor > 0; chars++)
			m_cursor = PrevBoundary(m_cursor);
		for (; chars > 0 && m_cursor < m_text.size(); chars--)
			m_cursor = NextBoundary(m_cursor);
	}

	void Model::SetCursor(size_t byte_offset)
	{
		byte_offset = std::min(byte_offset, m_text.size());
		while (byte_offset > 0 && byte_offset < m_text.size() && IsContinuationByte(m_text[byte_offset]))
			byte_offset--;
		m_cursor = byte_offset;
	}

	const Key& Model::KeyAt(KeyPos pos) const
	{
		const Layout& layout = CurrentLayout();
		if (pos.row >= layout.row_count || pos.col >= layout.rows[pos.row].count)
			return layout.rows[0].keys[0];
		return layout.rows[pos.row].keys[pos.col];
	}

	const char* Model::KeyText(const Key& key) const
	{
		switch (key.kind)
		{
			case KeyKind::Character:
				return (m_page == Page::Letters && m_shift != ShiftState::Off) ? key.shifted : key.normal;
			case KeyKind::Space:
				return " ";
			default:
				return "";
		}
	}

	u32 Model::CenterOf(const Row& row, u32 col)
	{
		u32 left = 0;
		for (u32 i = 0; i < col; i++)
			left += row.keys[i].span;
		return left * 2 + row.keys[col].span;
	}

	u32 Model::ColumnAtCenter(const Row& row, u32 center) const
	{
		u32 left = 0;
		for (u32 i = 0; i < row.count; i++)
		{
			const u32 right = left + row.keys[i].span;
			if (center >= left * 2 && center < right * 2)
				return i;
			left = right;
		}
		return row.count - 1;
	}

	void Model::SelectColumnNear(u32 row, u32 center)
	{
		const Layout& layout = CurrentLayout();
		m_highlight.row = std::min(row, layout.row_count - 1);
		m_highlight.col = ColumnAtCenter(layout.rows[m_highlight.row], center);
	}

	void Model::MoveHighlight(Direction dir)
	{
		const Layout& layout = CurrentLayout();
		switch (dir)
		{
			case Direction::Left:
			case Direction::Right:
			{
				const Row& row = layout.rows[m_highlight.row];
				m_highlight.col = (dir == Direction::Right) ? (m_highlight.col + 1) % row.count : (m_highlight.col + row.count - 1) % row.count;
				m_goal_center = CenterOf(row, m_highlight.col);
				break;
			}
			case Direction::Up:
				SelectColumnNear((m_highlight.row + layout.row_count - 1) % layout.row_count, m_goal_center);
				break;
			case Direction::Down:
				SelectColumnNear((m_highlight.row + 1) % layout.row_count, m_goal_center);
				break;
		}
	}

	void Model::SetHighlight(KeyPos pos)
	{
		const Layout& layout = CurrentLayout();
		if (pos.row >= layout.row_count || pos.col >= layout.rows[pos.row].count)
			return;

		m_highlight = pos;
		m_goal_center = CenterOf(layout.rows[pos.row], pos.col);
	}

	void Model::SwitchPage(Page page)
	{
		m_page = page;
		SelectColumnNear(m_highlight.row, m_goal_center);
	}

	Outcome Model::PressHighlighted()
	{
		const Key& key = HighlightedKey();
		switch (key.kind)
		{
			case KeyKind::Character:
				// A capital for the next character only: it ends with the character, not with the next key.
				if (Insert(KeyText(key)) && m_shift == ShiftState::Once)
					m_shift = ShiftState::Off;
				break;
			case KeyKind::Space:
				Insert(" ");
				break;
			case KeyKind::Backspace:
				Backspace();
				break;
			case KeyKind::Shift:
				CycleShift();
				break;
			case KeyKind::Symbols:
				ToggleSymbols();
				break;
			case KeyKind::Ok:
				return CanSubmit() ? Outcome::Submit : Outcome::None;
			case KeyKind::Cancel:
				return Outcome::Cancel;
		}
		return Outcome::None;
	}

	Outcome Model::Press(KeyPos pos)
	{
		SetHighlight(pos);
		return PressHighlighted();
	}

	void Model::ToggleShift()
	{
		if (m_page != Page::Letters)
			return;
		m_shift = (m_shift == ShiftState::Off) ? ShiftState::Once : ShiftState::Off;
	}

	void Model::CycleShift()
	{
		if (m_page != Page::Letters)
			return;
		switch (m_shift)
		{
			case ShiftState::Off:
				m_shift = ShiftState::Once;
				break;
			case ShiftState::Once:
				m_shift = ShiftState::Locked;
				break;
			case ShiftState::Locked:
				m_shift = ShiftState::Off;
				break;
		}
	}

	void Model::ToggleSymbols()
	{
		if (m_page == Page::Letters)
			SwitchPage(Page::Symbols);
		else if (m_page == Page::Symbols)
			SwitchPage(Page::Letters);
	}
} // namespace OnScreenKeyboard
