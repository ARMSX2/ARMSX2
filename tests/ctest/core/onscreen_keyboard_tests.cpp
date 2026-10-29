// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The rules of Big Picture's on-screen keyboard (ImGui/OnScreenKeyboardModel.h): the key layouts, how the
// selected key moves, what pressing a key does to the text, and the editing of the text itself.
// Nothing here draws; ImGuiFullscreen_Keyboard.cpp turns controller and mouse input into these calls.

#include <gtest/gtest.h>

#include "ImGui/OnScreenKeyboardModel.h"

#include <deque>
#include <set>
#include <string>
#include <string_view>
#include <utility>

using namespace OnScreenKeyboard;

namespace
{
	Model Start(std::string text = {}, CharSet set = CharSet::Any, size_t max_chars = Model::kDefaultMaxChars)
	{
		Model m;
		m.Begin(std::move(text), set, max_chars);
		return m;
	}

	// Moves the selection to the key that types `text` (or, with an empty `text`, the first key of `kind`) by
	// pressing the model's own directions the way a controller does, taking the shortest way. Finding the
	// key therefore proves a controller can reach it. Returns false and leaves `m` alone if no sequence of
	// moves gets there.
	bool GoTo(Model& m, KeyKind kind, std::string_view text = {})
	{
		std::deque<Model> queue{m};
		std::set<std::pair<u32, u32>> seen{{m.Highlight().row, m.Highlight().col}};
		while (!queue.empty())
		{
			Model at = queue.front();
			queue.pop_front();

			const Key& k = at.HighlightedKey();
			if (k.kind == kind && (kind != KeyKind::Character || text == at.KeyText(k)))
			{
				m = at;
				return true;
			}

			for (const Direction dir : {Direction::Left, Direction::Right, Direction::Up, Direction::Down})
			{
				Model next = at;
				next.MoveHighlight(dir);
				if (seen.insert({next.Highlight().row, next.Highlight().col}).second)
					queue.push_back(next);
			}
		}
		return false;
	}

	void Type(Model& m, KeyKind kind, std::string_view text = {})
	{
		ASSERT_TRUE(GoTo(m, kind, text)) << "no key " << text;
		m.PressHighlighted();
	}

	std::set<std::string> TypedBy(Page page)
	{
		std::set<std::string> typed;
		const Layout& layout = GetLayout(page);
		for (u32 r = 0; r < layout.row_count; r++)
		{
			for (u32 c = 0; c < layout.rows[r].count; c++)
			{
				const Key& k = layout.rows[r].keys[c];
				if (k.kind == KeyKind::Character)
				{
					typed.insert(k.normal);
					typed.insert(k.shifted);
				}
			}
		}
		return typed;
	}
} // namespace

TEST(OnScreenKeyboardLayout, EveryRowFillsTheLayoutWidth)
{
	for (const Page page : {Page::Letters, Page::Symbols, Page::Numpad, Page::NumpadDot})
	{
		const Layout& layout = GetLayout(page);
		ASSERT_GT(layout.row_count, 0u);
		for (u32 r = 0; r < layout.row_count; r++)
		{
			u32 width = 0;
			for (u32 c = 0; c < layout.rows[r].count; c++)
			{
				EXPECT_GE(layout.rows[r].keys[c].span, 1);
				width += layout.rows[r].keys[c].span;
			}
			EXPECT_EQ(width, layout.columns) << "page " << static_cast<int>(page) << " row " << r;
		}
	}
}

TEST(OnScreenKeyboardLayout, LettersPageHasTheControlKeysAndAllTheLetters)
{
	Model m = Start();
	for (const KeyKind kind : {KeyKind::Shift, KeyKind::Symbols, KeyKind::Space, KeyKind::Backspace, KeyKind::Ok, KeyKind::Cancel})
		EXPECT_TRUE(GoTo(m, kind)) << "kind " << static_cast<int>(kind);

	const std::set<std::string> typed = TypedBy(Page::Letters);
	for (char c = 'a'; c <= 'z'; c++)
	{
		EXPECT_TRUE(typed.count(std::string(1, c))) << c;
		EXPECT_TRUE(typed.count(std::string(1, static_cast<char>(c - 'a' + 'A')))) << c;
	}
	for (char c = '0'; c <= '9'; c++)
		EXPECT_TRUE(typed.count(std::string(1, c))) << c;
}

TEST(OnScreenKeyboardLayout, SymbolsAndLettersTogetherReachTheCharactersAddressesNeed)
{
	// . : - _ @ / ! are the address characters; # and ? are what a URL or a password adds.
	std::set<std::string> typed = TypedBy(Page::Letters);
	const std::set<std::string> symbols = TypedBy(Page::Symbols);
	typed.insert(symbols.begin(), symbols.end());
	for (const char* s : {".", ":", "-", "_", "@", "/", "!", "#", "?"})
		EXPECT_TRUE(typed.count(s)) << s;

	// The symbols page alone must not need the letters page for a number.
	for (char c = '0'; c <= '9'; c++)
		EXPECT_TRUE(symbols.count(std::string(1, c))) << c;
}

TEST(OnScreenKeyboardLayout, NumberPadsHaveDigitsAndActionsAndOnlyTheIPOneHasADot)
{
	for (const CharSet set : {CharSet::Digits, CharSet::IPv4})
	{
		Model m = Start({}, set);
		EXPECT_EQ(m.CurrentPage(), set == CharSet::Digits ? Page::Numpad : Page::NumpadDot);
		for (const KeyKind kind : {KeyKind::Backspace, KeyKind::Ok, KeyKind::Cancel})
			EXPECT_TRUE(GoTo(m, kind));
		for (char c = '0'; c <= '9'; c++)
			EXPECT_TRUE(GoTo(m, KeyKind::Character, std::string(1, c))) << c;
		EXPECT_EQ(GoTo(m, KeyKind::Character, "."), set == CharSet::IPv4);
	}
}

TEST(OnScreenKeyboardLayout, EveryCharacterAPadOffersIsOneItsFilterAccepts)
{
	for (const auto& [page, set] : {std::pair{Page::Numpad, CharSet::Digits}, std::pair{Page::NumpadDot, CharSet::IPv4}})
	{
		for (const std::string& s : TypedBy(page))
			EXPECT_TRUE(Accepts(set, static_cast<char32_t>(static_cast<unsigned char>(s[0])))) << s;
	}
}

TEST(OnScreenKeyboardNavigation, LeftAndRightWrapWithinTheRow)
{
	Model m = Start();
	EXPECT_EQ(m.Highlight(), (KeyPos{0, 0}));
	m.MoveHighlight(Direction::Left);
	EXPECT_EQ(m.Highlight(), (KeyPos{0, 9}));
	m.MoveHighlight(Direction::Right);
	EXPECT_EQ(m.Highlight(), (KeyPos{0, 0}));
	m.MoveHighlight(Direction::Right);
	EXPECT_EQ(m.Highlight(), (KeyPos{0, 1}));
}

TEST(OnScreenKeyboardNavigation, UpAndDownWrapFromFirstRowToLast)
{
	Model m = Start();
	const u32 last = m.CurrentLayout().row_count - 1;
	m.MoveHighlight(Direction::Up);
	EXPECT_EQ(m.Highlight().row, last);
	m.MoveHighlight(Direction::Down);
	EXPECT_EQ(m.Highlight().row, 0u);
	m.MoveHighlight(Direction::Down);
	EXPECT_EQ(m.Highlight().row, 1u);
}

TEST(OnScreenKeyboardNavigation, DownIntoTheWideBottomRowKeepsTheHorizontalPosition)
{
	// Bottom row: Symbols (2 units), Space (4), OK (2), Cancel (2) under ten one-unit keys.
	Model m = Start();
	const u32 bottom = m.CurrentLayout().row_count - 1;
	const u32 expected_key_for_column[10] = {0, 0, 1, 1, 1, 1, 2, 2, 3, 3};
	for (u32 col = 0; col < 10; col++)
	{
		m.SetHighlight({1, col});
		m.MoveHighlight(Direction::Down);
		m.MoveHighlight(Direction::Down);
		m.MoveHighlight(Direction::Down);
		EXPECT_EQ(m.Highlight().row, bottom) << col;
		EXPECT_EQ(m.Highlight().col, expected_key_for_column[col]) << col;
	}
}

TEST(OnScreenKeyboardNavigation, UpFromAWideKeyRemembersWhereYouCameFrom)
{
	// Down from 'o' lands on Cancel; up again must go back to 'o', not to the key above Cancel's left edge.
	Model m = Start();
	m.SetHighlight({1, 8});
	m.MoveHighlight(Direction::Down);
	m.MoveHighlight(Direction::Down);
	m.MoveHighlight(Direction::Down);
	EXPECT_EQ(m.HighlightedKey().kind, KeyKind::Cancel);
	m.MoveHighlight(Direction::Up);
	m.MoveHighlight(Direction::Up);
	m.MoveHighlight(Direction::Up);
	EXPECT_EQ(m.Highlight(), (KeyPos{1, 8}));
}

TEST(OnScreenKeyboardNavigation, MovingSidewaysResetsTheRememberedPosition)
{
	Model m = Start();
	m.SetHighlight({1, 8});
	m.MoveHighlight(Direction::Down);
	m.MoveHighlight(Direction::Down);
	m.MoveHighlight(Direction::Down); // Cancel
	m.MoveHighlight(Direction::Left); // OK
	m.MoveHighlight(Direction::Up); // the row above, over OK
	EXPECT_EQ(m.Highlight(), (KeyPos{3, 7}));
}

TEST(OnScreenKeyboardNavigation, NumberPadWrapsInBothDirections)
{
	Model m = Start({}, CharSet::IPv4);
	m.MoveHighlight(Direction::Left);
	EXPECT_EQ(m.Highlight(), (KeyPos{0, 3}));
	m.MoveHighlight(Direction::Down);
	m.MoveHighlight(Direction::Down);
	m.MoveHighlight(Direction::Down); // 0 and . share the bottom row; column 3 is the dot
	EXPECT_EQ(std::string(m.KeyText(m.HighlightedKey())), ".");
	m.MoveHighlight(Direction::Down);
	EXPECT_EQ(m.Highlight().row, 0u);
}

TEST(OnScreenKeyboardNavigation, SetHighlightIgnoresPositionsOffThePage)
{
	Model m = Start();
	m.SetHighlight({1, 3});
	m.SetHighlight({99, 0});
	m.SetHighlight({1, 99});
	EXPECT_EQ(m.Highlight(), (KeyPos{1, 3}));
}

TEST(OnScreenKeyboardPress, CharacterKeysTypeAtTheCursor)
{
	Model m = Start("ad");
	m.MoveCursor(-1);
	Type(m, KeyKind::Character, "b");
	Type(m, KeyKind::Character, "c");
	EXPECT_EQ(m.Text(), "abcd");
	EXPECT_EQ(m.Cursor(), 3u);
}

TEST(OnScreenKeyboardPress, ShiftOnceCapitalizesOneCharacter)
{
	Model m = Start();
	m.ToggleShift();
	EXPECT_EQ(m.Shift(), ShiftState::Once);
	Type(m, KeyKind::Character, "A");
	EXPECT_EQ(m.Shift(), ShiftState::Off);
	Type(m, KeyKind::Character, "b");
	EXPECT_EQ(m.Text(), "Ab");
}

TEST(OnScreenKeyboardPress, ShiftKeyTapsGoOnceThenLockedThenOff)
{
	Model m = Start();
	Type(m, KeyKind::Shift);
	EXPECT_EQ(m.Shift(), ShiftState::Once);
	Type(m, KeyKind::Shift);
	EXPECT_EQ(m.Shift(), ShiftState::Locked);
	Type(m, KeyKind::Character, "Q");
	Type(m, KeyKind::Character, "W");
	EXPECT_EQ(m.Text(), "QW");
	EXPECT_EQ(m.Shift(), ShiftState::Locked);
	Type(m, KeyKind::Shift);
	EXPECT_EQ(m.Shift(), ShiftState::Off);
}

TEST(OnScreenKeyboardPress, ControllerShiftShortcutTogglesAndClearsLock)
{
	Model m = Start();
	m.ToggleShift();
	EXPECT_EQ(m.Shift(), ShiftState::Once);
	m.ToggleShift();
	EXPECT_EQ(m.Shift(), ShiftState::Off);
	m.CycleShift();
	m.CycleShift();
	EXPECT_EQ(m.Shift(), ShiftState::Locked);
	m.ToggleShift();
	EXPECT_EQ(m.Shift(), ShiftState::Off);
}

TEST(OnScreenKeyboardPress, ShiftAlsoChangesDashAndDot)
{
	Model m = Start();
	m.CycleShift();
	m.CycleShift(); // locked
	Type(m, KeyKind::Character, "_");
	Type(m, KeyKind::Character, ":");
	EXPECT_EQ(m.Text(), "_:");
}

TEST(OnScreenKeyboardPress, SpaceAndBackspaceKeys)
{
	Model m = Start();
	Type(m, KeyKind::Character, "a");
	Type(m, KeyKind::Space);
	Type(m, KeyKind::Character, "b");
	EXPECT_EQ(m.Text(), "a b");
	Type(m, KeyKind::Backspace);
	Type(m, KeyKind::Backspace);
	EXPECT_EQ(m.Text(), "a");
}

TEST(OnScreenKeyboardPress, SymbolsKeySwitchesPagesAndKeepsTheRowAndPosition)
{
	Model m = Start();
	m.SetHighlight({2, 4});
	m.ToggleSymbols();
	EXPECT_EQ(m.CurrentPage(), Page::Symbols);
	EXPECT_EQ(m.Highlight(), (KeyPos{2, 4}));
	m.ToggleSymbols();
	EXPECT_EQ(m.CurrentPage(), Page::Letters);

	Type(m, KeyKind::Symbols);
	EXPECT_EQ(m.CurrentPage(), Page::Symbols);
	Type(m, KeyKind::Character, "@");
	Type(m, KeyKind::Character, "/");
	EXPECT_EQ(m.Text(), "@/");
}

TEST(OnScreenKeyboardPress, ShiftDoesNothingOnTheSymbolsPage)
{
	Model m = Start();
	m.ToggleSymbols();
	m.ToggleShift();
	m.CycleShift();
	EXPECT_EQ(m.Shift(), ShiftState::Off);
	Type(m, KeyKind::Character, "!");
	EXPECT_EQ(m.Text(), "!");
}

TEST(OnScreenKeyboardPress, NumberPadHasNoSymbolsPage)
{
	Model m = Start({}, CharSet::Digits);
	m.ToggleSymbols();
	m.ToggleShift();
	EXPECT_EQ(m.CurrentPage(), Page::Numpad);
	EXPECT_EQ(m.Shift(), ShiftState::Off);
}

TEST(OnScreenKeyboardPress, OkSubmitsOnlyWhenThereIsText)
{
	Model m = Start();
	ASSERT_TRUE(GoTo(m, KeyKind::Ok));
	EXPECT_EQ(m.PressHighlighted(), Outcome::None);
	EXPECT_FALSE(m.CanSubmit());

	m.Insert("x");
	EXPECT_TRUE(m.CanSubmit());
	EXPECT_EQ(m.PressHighlighted(), Outcome::Submit);
}

TEST(OnScreenKeyboardPress, CancelAlwaysCancels)
{
	Model m = Start("keep");
	ASSERT_TRUE(GoTo(m, KeyKind::Cancel));
	EXPECT_EQ(m.PressHighlighted(), Outcome::Cancel);
	EXPECT_EQ(m.Text(), "keep");
}

TEST(OnScreenKeyboardPress, PressSelectsTheKeyFirst)
{
	// A mouse click presses a key without a prior hover.
	Model m = Start();
	EXPECT_EQ(m.Press({1, 0}), Outcome::None);
	EXPECT_EQ(m.Text(), "q");
	EXPECT_EQ(m.Highlight(), (KeyPos{1, 0}));
}

TEST(OnScreenKeyboardText, BeginPutsTheCursorAtTheEnd)
{
	Model m = Start("192.168");
	EXPECT_EQ(m.Text(), "192.168");
	EXPECT_EQ(m.Cursor(), 7u);
}

TEST(OnScreenKeyboardText, InsertGoesAtTheCursor)
{
	Model m = Start("ac");
	m.MoveCursor(-1);
	EXPECT_TRUE(m.Insert("b"));
	EXPECT_EQ(m.Text(), "abc");
	EXPECT_EQ(m.Cursor(), 2u);
}

TEST(OnScreenKeyboardText, BackspaceDeletesBeforeTheCursor)
{
	Model m = Start("abc");
	m.MoveCursor(-1);
	EXPECT_TRUE(m.Backspace());
	EXPECT_EQ(m.Text(), "ac");
	EXPECT_EQ(m.Cursor(), 1u);
}

TEST(OnScreenKeyboardText, BackspaceAtTheStartDoesNothing)
{
	Model m = Start("abc");
	m.MoveCursorToStart();
	EXPECT_FALSE(m.Backspace());
	EXPECT_EQ(m.Text(), "abc");
	EXPECT_EQ(m.Cursor(), 0u);
}

TEST(OnScreenKeyboardText, DeleteRemovesTheCharacterAtTheCursor)
{
	Model m = Start("abc");
	m.MoveCursorToStart();
	EXPECT_TRUE(m.Delete());
	EXPECT_EQ(m.Text(), "bc");
	EXPECT_EQ(m.Cursor(), 0u);
	m.MoveCursorToEnd();
	EXPECT_FALSE(m.Delete());
}

TEST(OnScreenKeyboardText, CursorStopsAtBothEnds)
{
	Model m = Start("ab");
	m.MoveCursor(5);
	EXPECT_EQ(m.Cursor(), 2u);
	m.MoveCursor(-10);
	EXPECT_EQ(m.Cursor(), 0u);
	m.MoveCursor(1);
	EXPECT_EQ(m.Cursor(), 1u);
}

TEST(OnScreenKeyboardText, CursorAndEditsMoveByWholeCharacters)
{
	// "é" is two bytes, "€" three, "😀" four.
	Model m = Start("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z");
	m.MoveCursor(-1); // before z
	EXPECT_EQ(m.Cursor(), 10u);
	m.MoveCursor(-1); // before the emoji
	EXPECT_EQ(m.Cursor(), 6u);
	EXPECT_TRUE(m.Backspace()); // the euro sign
	EXPECT_EQ(m.Text(), "a\xC3\xA9\xF0\x9F\x98\x80z");
	EXPECT_EQ(m.Cursor(), 3u);
	EXPECT_TRUE(m.Delete()); // the emoji
	EXPECT_EQ(m.Text(), "a\xC3\xA9z");
	EXPECT_TRUE(m.Backspace()); // the acute e
	EXPECT_EQ(m.Text(), "az");
}

TEST(OnScreenKeyboardText, SetCursorSnapsToACharacterBoundary)
{
	Model m = Start("a\xC3\xA9z");
	m.SetCursor(2); // the middle of the two-byte e
	EXPECT_EQ(m.Cursor(), 1u);
	m.SetCursor(100);
	EXPECT_EQ(m.Cursor(), m.Text().size());
}

TEST(OnScreenKeyboardText, MaxCharsLimitsInsertsButNotWhatBeginWasGiven)
{
	Model m = Start("ab", CharSet::Any, 4);
	EXPECT_TRUE(m.Insert("cdef"));
	EXPECT_EQ(m.Text(), "abcd");
	EXPECT_FALSE(m.Insert("g"));
	EXPECT_EQ(m.Text(), "abcd");

	EXPECT_TRUE(m.Backspace());
	EXPECT_TRUE(m.Insert("x"));
	EXPECT_EQ(m.Text(), "abcx");

	// A default value longer than the limit is kept whole; only additions are refused.
	Model longer = Start("abcdef", CharSet::Any, 3);
	EXPECT_EQ(longer.Text(), "abcdef");
	EXPECT_FALSE(longer.Insert("g"));
}

TEST(OnScreenKeyboardText, MaxCharsCountsCharactersNotBytes)
{
	Model m = Start({}, CharSet::Any, 2);
	EXPECT_TRUE(m.Insert("\xC3\xA9\xC3\xA9\xC3\xA9"));
	EXPECT_EQ(m.Text(), "\xC3\xA9\xC3\xA9");
}

TEST(OnScreenKeyboardText, NumericFilterDropsEverythingButDigits)
{
	Model m = Start({}, CharSet::Digits);
	EXPECT_TRUE(m.Insert("12ab3.4"));
	EXPECT_EQ(m.Text(), "1234");
	EXPECT_FALSE(m.Insert("abc"));
	EXPECT_FALSE(m.Insert("."));
	EXPECT_EQ(m.Text(), "1234");
}

TEST(OnScreenKeyboardText, IPv4FilterKeepsDigitsAndDots)
{
	Model m = Start({}, CharSet::IPv4);
	EXPECT_TRUE(m.Insert("192.168.0.1:80 x"));
	EXPECT_EQ(m.Text(), "192.168.0.180");
	EXPECT_FALSE(m.Insert("host"));
}

TEST(OnScreenKeyboardText, AnyFilterTakesPrintableCharactersOnly)
{
	Model m = Start();
	EXPECT_TRUE(m.Insert("a\tb\nc\x7f\x1b"
						 "d"));
	EXPECT_EQ(m.Text(), "abcd");
	EXPECT_TRUE(m.Insert("\xC3\xA9")); // typed on a physical keyboard
	EXPECT_EQ(m.Text(), "abcd\xC3\xA9");
	EXPECT_FALSE(m.Insert("\x01\x02"));
	EXPECT_FALSE(m.Insert("\xFF")); // not UTF-8 at all
}

TEST(OnScreenKeyboardText, CursorCanBeReadAndSetInCharacters)
{
	// Four characters in seven bytes: a, é (two bytes), € (three) and z.
	Model m = Start("a\xC3\xA9\xE2\x82\xACz");
	EXPECT_EQ(m.CharCount(), 4u);
	EXPECT_EQ(m.CursorChars(), 4u);
	m.MoveCursor(-1);
	EXPECT_EQ(m.CursorChars(), 3u);
	m.SetCursorChars(1);
	EXPECT_EQ(m.Cursor(), 1u);
	m.SetCursorChars(2);
	EXPECT_EQ(m.Cursor(), 3u);
	m.SetCursorChars(99);
	EXPECT_EQ(m.Cursor(), m.Text().size());
	m.SetCursorChars(0);
	EXPECT_EQ(m.Cursor(), 0u);
}

TEST(OnScreenKeyboardText, BeginStartsOverCompletely)
{
	Model m = Start("old");
	m.MoveHighlight(Direction::Down);
	m.CycleShift();
	m.ToggleSymbols();
	m.Begin("new", CharSet::Digits);
	EXPECT_EQ(m.Text(), "new");
	EXPECT_EQ(m.Cursor(), 3u);
	EXPECT_EQ(m.Shift(), ShiftState::Off);
	EXPECT_EQ(m.CurrentPage(), Page::Numpad);
	EXPECT_EQ(m.Highlight(), (KeyPos{0, 0}));
}
