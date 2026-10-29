// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Big Picture's text entry dialog (ImGuiFullscreen::OpenInputStringDialog) with an on-screen keyboard,
// so a controller, a touch screen or a mouse can type. The rules of the keyboard are in
// OnScreenKeyboardModel.h; this draws it and turns input into those calls. ImGuiFullscreen.cpp calls
// in here with a few lines, so the rest of the dialog stays out of the file upstream keeps changing.
//
// The window is drawn without ImGui navigation (ImGuiWindowFlags_NoNavInputs) because the D-pad and
// the face buttons belong to the keyboard here; the dialog's own Cancel path replaces the one ImGui
// would have given a focused window.

#include "common/SmallString.h"

#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/OnScreenKeyboardModel.h"

#include <string>

namespace ImGuiFullscreen
{
	enum class KeyboardAction : u8
	{
		None,
		Submit, ///< OK: take GetText().
		Cancel,
	};

	/// Everything one open text entry keeps between frames.
	struct KeyboardEntry
	{
		OnScreenKeyboard::Model model;
		float text_scroll = 0.0f; ///< How far the text line is scrolled left, in pixels, to keep the cursor in view.
		bool mask = false; ///< The text line shows a star for each character (InputFilterType::Password).

		void Begin(std::string text, InputFilterType filter);

		const std::string& GetText() const { return model.Text(); }
	};

	/// The width of the dialog window for this entry, in pixels. Fixed for the entry's page, so it can be
	/// given to SetNextWindowSize before the window exists.
	float GetKeyboardEntryWidth(const KeyboardEntry& entry);

	/// Draws the inside of the dialog's window: `message`, an optional `caption`, the text line, the
	/// keyboard and the button hints, and reads the controller and keyboard. `ok_text` labels the OK key.
	/// Call between BeginPopupModal (with ImGuiWindowFlags_NoNavInputs) and EndPopup.
	KeyboardAction DrawKeyboardEntry(KeyboardEntry& entry, const char* message, const char* caption, const char* ok_text);

	/// ImGui::InputTextWithHint for a field a controller or touch screen must be able to fill. A keyboard user
	/// types in the field as usual; a gamepad button, click or tap opens the text entry dialog titled `hint`, with
	/// `message` above the text line, and writes the answer to `buf`. `buf` must outlive the dialog, so pass a
	/// static buffer. `password` hides the text. Returns true on the frame the field was edited by typing.
	bool InputTextWithKeyboard(const char* label, const char* hint, char* buf, size_t buf_size, bool password, const char* message);
} // namespace ImGuiFullscreen
