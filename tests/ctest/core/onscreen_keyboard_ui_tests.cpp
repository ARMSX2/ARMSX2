// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Big Picture's text entry dialog with its on-screen keyboard (ImGui/ImGuiFullscreen_Keyboard.h) in a real
// ImGui context with no renderer: controller and keyboard input go in as ImGui key events, a frame is run
// for each, and what the dialog did shows in its entry. The rules of the keys themselves are in
// onscreen_keyboard_tests.cpp.

#include <gtest/gtest.h>

#include "ImGui/ImGuiFullscreen_Keyboard.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <string>

using namespace ImGuiFullscreen;
using OnScreenKeyboard::Direction;
using OnScreenKeyboard::KeyPos;
using OnScreenKeyboard::Page;
using OnScreenKeyboard::ShiftState;

namespace
{
	class KeyboardDialogTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_context = ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.BackendFlags |= ImGuiBackendFlags_HasGamepad | ImGuiBackendFlags_RendererHasTextures;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;

			ImFont* font = io.Fonts->AddFontDefault();
			g_standard_font = {font, 14.0f};
			g_medium_font = {font, 14.0f};
			g_large_font = {font, 22.0f};
			SetTheme("Dark");
			UpdateLayoutScale();

			m_entry.Begin("", InputFilterType::None);
		}

		void TearDown() override
		{
			ImGui::DestroyContext(m_context);
			g_standard_font = {};
			g_medium_font = {};
			g_large_font = {};
		}

		// One frame of the dialog, drawn as DrawInputDialog draws it. Returns what the dialog decided.
		KeyboardAction Frame()
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowSize(ImVec2(GetKeyboardEntryWidth(m_entry), 0.0f));
			ImGui::SetNextWindowPos(ImVec2(640.0f, 360.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::OpenPopup("Test Entry");

			KeyboardAction action = KeyboardAction::None;
			if (ImGui::BeginPopupModal("Test Entry", nullptr,
					ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
						ImGuiWindowFlags_NoNavInputs))
			{
				action = DrawKeyboardEntry(m_entry, "Enter something.", "", "OK");
				ImGui::EndPopup();
			}
			ImGui::EndFrame();
			ResetCloseMenuIfNeeded();

			// There is no renderer to upload the font to; say it has been.
			for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
			{
				if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
				{
					texture->SetTexID(static_cast<ImTextureID>(1));
					texture->SetStatus(ImTextureStatus_OK);
				}
			}
			return action;
		}

		// Runs frames until the dialog window has been drawn and can take input (it ignores the one it opens in).
		void OpenDialog()
		{
			for (int i = 0; i < 4; i++)
				Frame();
		}

		// A press and release of `key` with a frame for each. Returns what the dialog decided over the two.
		KeyboardAction Press(ImGuiKey key)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddKeyAnalogEvent(key, true, 1.0f);
			KeyboardAction action = Frame();
			io.AddKeyAnalogEvent(key, false, 0.0f);
			const KeyboardAction after = Frame();
			return (action != KeyboardAction::None) ? action : after;
		}

		static ImGuiKey Confirm()
		{
			return ImGui::GetIO().ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
		}

		static ImGuiKey Cancel()
		{
			return ImGui::GetIO().ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight;
		}

		ImGuiContext* m_context = nullptr;
		KeyboardEntry m_entry;
	};
} // namespace

TEST_F(KeyboardDialogTest, TheButtonPressThatOpensTheDialogDoesNotTypeAKey)
{
	// The row that opens the dialog was activated by this very press, which is still held in the dialog's first frame.
	ImGui::GetIO().AddKeyAnalogEvent(Confirm(), true, 1.0f);
	Frame();
	Frame();
	ImGui::GetIO().AddKeyAnalogEvent(Confirm(), false, 0.0f);
	Frame();
	EXPECT_EQ(m_entry.GetText(), "");

	// But the next press is a key.
	OpenDialog();
	Press(Confirm());
	EXPECT_EQ(m_entry.GetText(), "1");
}

TEST_F(KeyboardDialogTest, DPadMovesTheHighlightAndConfirmTypesTheKey)
{
	OpenDialog();
	EXPECT_EQ(m_entry.model.Highlight(), (KeyPos{0, 0}));

	Press(ImGuiKey_GamepadDpadRight);
	EXPECT_EQ(m_entry.model.Highlight(), (KeyPos{0, 1}));
	Press(ImGuiKey_GamepadDpadDown);
	EXPECT_EQ(m_entry.model.Highlight(), (KeyPos{1, 1}));
	Press(Confirm());
	EXPECT_EQ(m_entry.GetText(), "w");

	// Left wraps around the row.
	Press(ImGuiKey_GamepadDpadLeft);
	Press(ImGuiKey_GamepadDpadLeft);
	EXPECT_EQ(m_entry.model.Highlight(), (KeyPos{1, 9}));
}

TEST_F(KeyboardDialogTest, DPadAndConfirmNeverLeaveTheKeyboard)
{
	// ImGui's own navigation is off in the dialog's window: nothing takes focus, so Down cannot move it off the
	// keys and Confirm cannot press anything but the selected key.
	OpenDialog();
	const ImGuiID nav_before = ImGui::GetCurrentContext()->NavId;
	for (int i = 0; i < 12; i++)
		Press(ImGuiKey_GamepadDpadDown);
	EXPECT_EQ(ImGui::GetCurrentContext()->NavId, nav_before);
	EXPECT_EQ(m_entry.model.Highlight().row, 2u); // twelve downs on five rows
}

TEST_F(KeyboardDialogTest, TheOtherButtonsEditTheText)
{
	OpenDialog();
	m_entry.model.Insert("ab");

	Press(ImGuiKey_GamepadFaceUp); // north: space
	EXPECT_EQ(m_entry.GetText(), "ab ");
	Press(ImGuiKey_GamepadFaceLeft); // west: backspace
	EXPECT_EQ(m_entry.GetText(), "ab");
	Press(ImGuiKey_GamepadL1);
	EXPECT_EQ(m_entry.model.Cursor(), 1u);
	Press(ImGuiKey_GamepadR1);
	Press(ImGuiKey_GamepadR1);
	EXPECT_EQ(m_entry.model.Cursor(), 2u);
}

TEST_F(KeyboardDialogTest, ShiftAndSymbolsShortcuts)
{
	OpenDialog();
	Press(ImGuiKey_GamepadL2);
	EXPECT_EQ(m_entry.model.Shift(), ShiftState::Once);
	Press(ImGuiKey_GamepadDpadDown); // the q row
	Press(Confirm());
	EXPECT_EQ(m_entry.GetText(), "Q");
	EXPECT_EQ(m_entry.model.Shift(), ShiftState::Off);

	Press(ImGuiKey_GamepadBack); // select
	EXPECT_EQ(m_entry.model.CurrentPage(), Page::Symbols);
	Press(Confirm());
	EXPECT_EQ(m_entry.GetText(), "Q!");
	Press(ImGuiKey_GamepadBack);
	EXPECT_EQ(m_entry.model.CurrentPage(), Page::Letters);
}

TEST_F(KeyboardDialogTest, StartSubmitsOnlyWhenThereIsText)
{
	OpenDialog();
	EXPECT_EQ(Press(ImGuiKey_GamepadStart), KeyboardAction::None);
	m_entry.model.Insert("x");
	EXPECT_EQ(Press(ImGuiKey_GamepadStart), KeyboardAction::Submit);
}

TEST_F(KeyboardDialogTest, TheOkAndCancelKeysAreReachableByTheDPad)
{
	OpenDialog();
	m_entry.model.Insert("x");
	// Down four rows from the top-left key is Symbols; right twice along the bottom row is OK.
	for (int i = 0; i < 4; i++)
		Press(ImGuiKey_GamepadDpadDown);
	Press(ImGuiKey_GamepadDpadRight);
	Press(ImGuiKey_GamepadDpadRight);
	EXPECT_EQ(Press(Confirm()), KeyboardAction::Submit);

	Press(ImGuiKey_GamepadDpadRight);
	EXPECT_EQ(Press(Confirm()), KeyboardAction::Cancel);
}

TEST_F(KeyboardDialogTest, TheCancelButtonCancelsWhateverIsTyped)
{
	OpenDialog();
	m_entry.model.Insert("keep?");
	EXPECT_EQ(Press(Cancel()), KeyboardAction::Cancel);
}

TEST_F(KeyboardDialogTest, OkOrCancelFollowTheSwappedConfirmButton)
{
	ImGui::GetIO().ConfigNavSwapGamepadButtons = true; // Circle confirms
	OpenDialog();
	Press(Confirm());
	EXPECT_EQ(m_entry.GetText(), "1");
	m_entry.model.Insert("x");
	EXPECT_EQ(Press(Cancel()), KeyboardAction::Cancel); // Cross cancels
}

TEST_F(KeyboardDialogTest, TypingOnAKeyboardGoesToTheText)
{
	OpenDialog();
	ImGui::GetIO().AddInputCharactersUTF8("hello");
	Frame();
	EXPECT_EQ(m_entry.GetText(), "hello");

	Press(ImGuiKey_Backspace);
	Press(ImGuiKey_LeftArrow);
	Press(ImGuiKey_LeftArrow);
	ImGui::GetIO().AddInputCharactersUTF8("!");
	Frame();
	EXPECT_EQ(m_entry.GetText(), "he!ll");

	Press(ImGuiKey_Delete);
	EXPECT_EQ(m_entry.GetText(), "he!l");
	EXPECT_EQ(Press(ImGuiKey_Enter), KeyboardAction::Submit);
}

TEST_F(KeyboardDialogTest, EscapeCancels)
{
	OpenDialog();
	EXPECT_EQ(Press(ImGuiKey_Escape), KeyboardAction::Cancel);
}

TEST_F(KeyboardDialogTest, TheNumberPadKeepsLettersOut)
{
	m_entry.Begin("", InputFilterType::Numeric);
	OpenDialog();
	ImGui::GetIO().AddInputCharactersUTF8("1a2b3");
	Frame();
	EXPECT_EQ(m_entry.GetText(), "123");
	EXPECT_EQ(m_entry.model.CurrentPage(), Page::Numpad);
}

TEST_F(KeyboardDialogTest, TheDialogAsksTheHostForTextInput)
{
	// The Qt host only forwards typed characters to ImGui while ImGui says a text box has the focus.
	OpenDialog();
	EXPECT_TRUE(ImGui::GetIO().WantTextInput);
}

TEST_F(KeyboardDialogTest, WindowWidthFollowsTheKeyboardPage)
{
	const float letters = GetKeyboardEntryWidth(m_entry);
	KeyboardEntry pad;
	pad.Begin("", InputFilterType::IPAddress);
	EXPECT_GT(letters, GetKeyboardEntryWidth(pad));
	EXPECT_GT(GetKeyboardEntryWidth(pad), 0.0f);
}
