// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Timer.h"

#include "imgui.h"

namespace FullscreenUI
{
	/// Tells a settings page that has stayed current for a moment from one the D-pad is passing, so a page
	/// that goes to the network when it opens does not send a request for every page scrolled past (GitHub's
	/// API allows 60 an hour without a token). A page keeps one of these, calls Settled() once per frame it
	/// is drawn, and starts its requests only when that returns true.
	class PageDwell
	{
	public:
		static constexpr float SECONDS = 1.0f;

		/// True once the page has been drawn every frame for SECONDS, and on each frame after that. A
		/// frame in which the page was not drawn starts the count again.
		bool Settled()
		{
			const int frame = ImGui::GetFrameCount();
			if (frame - m_last_frame > 1)
				m_timer.Reset();
			m_last_frame = frame;
			return m_timer.GetTimeSeconds() >= SECONDS;
		}

		/// For the page's shutdown: the next ImGui context counts frames from 0 again.
		void Reset() { m_last_frame = -100; }

	private:
		int m_last_frame = -100;
		Common::Timer m_timer;
	};
} // namespace FullscreenUI
