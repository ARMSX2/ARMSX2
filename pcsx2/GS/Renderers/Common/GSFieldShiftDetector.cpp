// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSFieldShiftDetector.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"

#include "common/Console.h"
#include "common/Timer.h"

#include <algorithm>

GSFieldShiftDetector::GSFieldShiftDetector() = default;

GSFieldShiftDetector::~GSFieldShiftDetector()
{
	ReleaseResources();
}

void GSFieldShiftDetector::ReleaseReadbacks()
{
	// Safe with copies still queued: every backend defers a download buffer's destruction until
	// the command buffer that writes it has completed.
	for (ProbeSlot& slot : m_slots)
	{
		slot.download.reset();
		slot.in_flight = false;
	}

	m_prev = std::vector<u8>();
	m_cur = std::vector<u8>();
	m_have_prev = false;
}

void GSFieldShiftDetector::ReleaseResources()
{
	ReleaseReadbacks();

	// The probe target goes back to the device pool only here, never on the decision. A decision
	// can land with copies out of it still queued, and the pool will hand a texture recycled this
	// frame straight back to the next caller -- which would leave those copies reading a target
	// somebody else is now drawing into, with the layout transitions they recorded no longer
	// describing it. A renderer reset or teardown has no queued work to contradict.
	if (m_probe)
	{
		if (g_gs_device)
			g_gs_device->Recycle(m_probe);
		m_probe = nullptr;
	}
}

void GSFieldShiftDetector::Reset()
{
	ReleaseResources();
	m_decision = Decision::Pending;
	m_probing = true;
	m_fields_since_decision = 0;
	m_size = GSVector2i(0, 0);
	m_scale = 0;
	m_rows = 0;
	m_cols = 0;
	m_next_index = 0;
	m_retire_index = 0;
	m_prev_index = 0;
	m_prev_applied = 0;
	m_prev_parity = 0;
	m_fields_probed = 0;
	m_waited_frames = 0;
	m_unusable_pairs = 0;
	m_tally = GSFieldShiftTally();
	m_cpu_ticks = 0;
}

void GSFieldShiftDetector::StartRound(const GSVector2i& size, int scale)
{
	// The probe target only has to go when its size does; a recheck of the same picture keeps it.
	if (!(size == m_size) || scale != m_scale)
		ReleaseResources();
	else
		ReleaseReadbacks();
	m_size = size;
	m_scale = scale;
	m_rows = size.y;
	m_cols = std::min(PROBE_COLUMNS, size.x);
	m_next_index = 0;
	m_retire_index = 0;
	m_fields_probed = 0;
	m_waited_frames = 0;
	m_unusable_pairs = 0;
	m_tally = GSFieldShiftTally();
	m_cpu_ticks = 0;
}

void GSFieldShiftDetector::Decide(bool no_shift, const char* why)
{
	m_decision = no_shift ? Decision::NoShift : Decision::Shift;
	m_probing = false;
	m_fields_since_decision = 0;

	DevCon.WriteLn("GS: field shift detector: %s after %d fields (%d shift / %d no-shift votes, "
				   "%d unusable pairs), %s, %.3f ms of GS thread total",
		no_shift ? "NO SHIFT" : "SHIFT", m_fields_probed, m_tally.shift, m_tally.noshift,
		m_unusable_pairs, why, Common::Timer::ConvertValueToMilliseconds(m_cpu_ticks));

	ReleaseReadbacks();
}

void GSFieldShiftDetector::Update(
	GSTexture* merge, const GSVector2i& size, int scale, int applied_offset_rows, int field_parity)
{
	if (!merge || !g_gs_device || scale < 2 || size.x <= 0 || size.y <= 0)
		return;

	const bool picture_changed = !(size == m_size) || scale != m_scale;
	if (!m_probing)
	{
		// Resting on a decision. Measure again when the picture it was taken on is gone, or when it
		// has stood long enough that the game may have moved on from what it was showing.
		if (!picture_changed && ++m_fields_since_decision < GS_FIELD_SHIFT_RECHECK_FIELDS)
			return;

		m_probing = true;
		StartRound(size, scale);
	}
	else if (picture_changed)
	{
		// A video-mode change. Everything measured so far was measured on a different picture.
		StartRound(size, scale);
	}

	const u64 start = Common::Timer::GetCurrentValue();

	// Collect finished readbacks, oldest first, so consecutive signatures really are consecutive
	// fields. Poll() neither submits nor waits.
	bool waiting = false;
	for (;;)
	{
		ProbeSlot* oldest = nullptr;
		for (ProbeSlot& slot : m_slots)
		{
			if (slot.in_flight && slot.index == m_retire_index)
				oldest = &slot;
		}

		if (!oldest)
			break;

		if (!oldest->download->Poll())
		{
			waiting = true;
			break;
		}

		const bool ok = Retire(*oldest);
		m_retire_index++;
		if (!ok || !m_probing)
		{
			m_cpu_ticks += Common::Timer::GetCurrentValue() - start;
			return;
		}
	}

	if (waiting)
	{
		if (++m_waited_frames > MAX_WAIT_FRAMES)
		{
			// Either the backend cannot tell us a copy is done without flushing (D3D, Metal), or
			// the GPU is that far behind. Flushing would block the GS thread on the GPU, which is
			// the one thing this must never do, so stop here and go with the votes already in --
			// which is the default when there are none.
			m_cpu_ticks += Common::Timer::GetCurrentValue() - start;
			Decide(GSFieldShiftTallySaysNoShift(m_tally), "readback did not complete without a flush");
			return;
		}
	}
	else
	{
		m_waited_frames = 0;
	}

	if (m_fields_probed >= MAX_PROBE_FIELDS)
	{
		if (m_retire_index >= m_next_index)
		{
			m_cpu_ticks += Common::Timer::GetCurrentValue() - start;
			Decide(GSFieldShiftTallySaysNoShift(m_tally), "probe budget spent");
			return;
		}
	}
	else if (!Probe(merge, applied_offset_rows, field_parity))
	{
		m_cpu_ticks += Common::Timer::GetCurrentValue() - start;
		return;
	}

	m_cpu_ticks += Common::Timer::GetCurrentValue() - start;
}

bool GSFieldShiftDetector::Probe(GSTexture* merge, int applied_offset_rows, int field_parity)
{
	ProbeSlot* free_slot = nullptr;
	for (ProbeSlot& slot : m_slots)
	{
		if (!slot.in_flight)
		{
			free_slot = &slot;
			break;
		}
	}

	if (!free_slot)
	{
		// Every slot is still in flight, so this field goes unprobed and the pair spanning it is
		// not two consecutive fields. Retiring in index order makes that visible later.
		m_next_index++;
		return true;
	}

	if (!m_probe)
	{
		m_probe = g_gs_device->CreateRenderTarget(m_cols, m_rows, GSTexture::Format::Color, false);
		if (!m_probe)
		{
			Decide(false, "no probe target");
			return false;
		}
	}

	if (!free_slot->download)
	{
		free_slot->download = g_gs_device->CreateDownloadTexture(m_cols, m_rows, GSTexture::Format::Color);
		if (!free_slot->download)
		{
			Decide(false, "backend has no download texture");
			return false;
		}
	}

	// A plain point-sampled column subsample: every row survives at full device resolution, which
	// is where the whole signal is, and the columns are thinned to keep the transfer trivial.
	g_gs_device->StretchRect(merge, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), m_probe,
		GSVector4(0.0f, 0.0f, static_cast<float>(m_cols), static_cast<float>(m_rows)), ShaderConvert::COPY,
		Nearest);

	const GSVector4i rc(0, 0, m_cols, m_rows);
	free_slot->download->CopyFromTexture(rc, m_probe, rc, 0, true);
	free_slot->index = m_next_index++;
	free_slot->applied = applied_offset_rows;
	free_slot->parity = field_parity;
	free_slot->in_flight = true;
	m_fields_probed++;
	return true;
}

bool GSFieldShiftDetector::Retire(ProbeSlot& slot)
{
	const GSVector4i rc(0, 0, m_cols, m_rows);
	if (!slot.download->Map(rc))
	{
		Decide(false, "readback could not be mapped");
		return false;
	}

	const u8* bits = slot.download->GetMapPointer();
	const u32 pitch = slot.download->GetMapPitch();
	m_cur.resize(static_cast<size_t>(m_cols) * static_cast<size_t>(m_rows));
	for (int r = 0; r < m_rows; r++)
	{
		const u8* src = bits + static_cast<size_t>(r) * pitch;
		u8* dst = m_cur.data() + static_cast<size_t>(r) * m_cols;
		for (int c = 0; c < m_cols; c++)
		{
			// Cheap luma. The test only needs a monotone stand-in for the pixel; exact weights buy
			// nothing and a shift or two is faster than a multiply per channel.
			const u32 sum = static_cast<u32>(src[c * 4 + 0]) + static_cast<u32>(src[c * 4 + 1]) * 2u +
			                static_cast<u32>(src[c * 4 + 2]);
			dst[c] = static_cast<u8>(sum >> 2);
		}
	}

	slot.download->Unmap();
	slot.in_flight = false;

	Compare(slot);
	if (!m_probing)
		return true;

	m_prev.swap(m_cur);
	m_prev_index = slot.index;
	m_prev_applied = slot.applied;
	m_prev_parity = slot.parity;
	m_have_prev = true;
	return true;
}

void GSFieldShiftDetector::Compare(const ProbeSlot& slot)
{
	if (!m_have_prev)
		return;

	if (slot.index != m_prev_index + 1 || !GSFieldShiftPairIsComparable(m_prev_parity, slot.parity))
	{
		m_unusable_pairs++;
		return;
	}

	const GSFieldShiftSample sample = GSMeasureFieldShift(
		m_cur.data(), m_prev.data(), m_cols, m_rows, m_scale, slot.applied - m_prev_applied);
	const GSFieldShiftVote vote = GSClassifyFieldShiftPair(sample);

	DevCon.WriteLn("GS: field shift pair %u: applied %+d, aligned %.3f up %.3f down %.3f -> %s", slot.index,
		slot.applied - m_prev_applied, sample.mad_aligned, sample.mad_up, sample.mad_down,
		(vote == GSFieldShiftVote::NoShift) ? "no shift" :
											  ((vote == GSFieldShiftVote::Shift) ? "shift" : "nothing"));

	if (vote == GSFieldShiftVote::NoShift)
	{
		m_tally.noshift++;
		// The two fields are the same picture and nothing else comes close. One pair this lopsided
		// settles it, which is what lets a no-shift game correct itself on the earliest field it
		// possibly can.
		if (GSFieldShiftMargin(sample) >= GS_FIELD_SHIFT_DECISIVE_MARGIN)
			Decide(true, "both fields draw the same picture");
	}
	else if (vote == GSFieldShiftVote::Shift)
	{
		m_tally.shift++;
	}
}
