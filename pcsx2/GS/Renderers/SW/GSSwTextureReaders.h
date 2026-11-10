// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <atomic>
#include <cstdint>

// Orders a cached texture's refill against the queued draws that still read it.
//
// A draw does not sample local memory. The GS thread copies the blocks it needs into
// the texture's buffer (GSTextureCacheSW::Texture::Update) before the draw is queued,
// and the workers read that buffer later. Writing local memory therefore never
// disturbs a queued draw, but refilling the buffer does: Update rewrites every block
// that was invalidated since, in place, whoever is still reading it.
//
// The page counts cannot see this. They tell the next draw that samples the texture
// whether someone is still drawing onto its pages; they do not tell it whether an
// earlier draw is still reading the buffer. The earlier reader and the draw that
// invalidated the texture may sit on different workers, and the invalidating draw
// can finish first.
//
// Rule: a texture invalidated while a queued draw holds it is not refilled until
// the workers are drained. Blocks loaded for the first time need no drain, since
// no queued draw's rectangle covers them.
//
// `queued` is touched by the GS thread and by the workers (a draw's data dies on the
// worker that finishes it last). The flag belongs to the GS thread.
struct GSSwTextureReaders
{
	std::atomic<uint32_t> queued{0};
	bool invalidated_under_reader = false;

	/// A draw that samples the texture is being queued.
	void Acquire() { queued.fetch_add(1, std::memory_order_relaxed); }

	/// That draw's data was destroyed: every worker has finished it.
	void Release() { queued.fetch_sub(1, std::memory_order_release); }

	/// Blocks of the texture that held valid texels were marked for refill.
	void Invalidated()
	{
		if (queued.load(std::memory_order_acquire) != 0)
			invalidated_under_reader = true;
	}

	/// Asked for a draw about to sample the texture, before its own Acquire and before
	/// the refill. True if the workers must be drained first. The caller drains, so
	/// the question is settled either way.
	bool DrainBeforeUpdate()
	{
		if (!invalidated_under_reader)
			return false;

		invalidated_under_reader = false;

		return queued.load(std::memory_order_acquire) != 0;
	}
};
