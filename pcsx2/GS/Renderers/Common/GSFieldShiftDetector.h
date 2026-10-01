// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSFieldShiftPolicy.h"

#include <memory>
#include <vector>

class GSTexture;
class GSDownloadTexture;

/// Decides from the frames whether a field-mode game moves its projection half a display line
/// between fields. Only consulted where the field render is presented directly (integer upscale
/// >= 2).
///
/// Probes a bounded number of fields, then rests. It re-measures after a video-mode change and
/// every GS_FIELD_SHIFT_RECHECK_FIELDS fields, keeping the current decision meanwhile. It NEVER
/// waits on the GPU: readbacks are issued without a flush and collected later via Poll(); a backend
/// that cannot answer Poll() without a flush gets the default instead of a stall.
class GSFieldShiftDetector
{
public:
	GSFieldShiftDetector();
	~GSFieldShiftDetector();

	/// True while nothing has been decided yet -- the default (shift) is in force.
	bool IsPending() const { return m_decision == Decision::Pending; }
	/// True while fields are being measured, for a first decision or a recheck.
	bool IsProbing() const { return m_probing; }
	/// What the merge should do with the FFMD offset on this field.
	bool WantsShift() const { return m_decision != Decision::NoShift; }

	/// Drop everything, including the decision. For a renderer reset or a device teardown.
	void Reset();

	/// Feed one field. `merge` is this field's merge target, `size` its size, `scale` the integer
	/// upscale, `applied_offset_rows` the merge offset the field was drawn with (removed before
	/// comparing), and `field_parity` which field this is.
	void Update(GSTexture* merge, const GSVector2i& size, int scale, int applied_offset_rows, int field_parity);

private:
	enum class Decision
	{
		Pending,
		Shift,
		NoShift,
	};

	/// Columns sampled across the frame: enough samples per field to resolve the difference, with
	/// a small total transfer.
	static constexpr int PROBE_COLUMNS = 32;
	/// Fields probed before the vote is taken, whatever it says.
	static constexpr int MAX_PROBE_FIELDS = 12;
	/// Frames the oldest readback may stay unfinished before the detector gives up on the backend.
	/// Never flushed, to avoid the stall.
	static constexpr int MAX_WAIT_FRAMES = 8;
	/// Readbacks in flight at once. Every field must be probed; waiting on the previous readback
	/// would sample every other field, i.e. the same parity (see GSFieldShiftPairIsComparable).
	/// One slot per frame the GPU can be behind.
	static constexpr int PROBE_SLOTS = 4;

	struct ProbeSlot
	{
		std::unique_ptr<GSDownloadTexture> download;
		u32 index = 0;
		int applied = 0;
		int parity = 0;
		bool in_flight = false;
	};

	bool Probe(GSTexture* merge, int applied_offset_rows, int field_parity);
	bool Retire(ProbeSlot& slot);
	void Compare(const ProbeSlot& slot);
	void Decide(bool no_shift, const char* why);
	void ReleaseReadbacks();
	void StartRound(const GSVector2i& size, int scale);
	void ReleaseResources();

	Decision m_decision = Decision::Pending;
	/// Measuring, as opposed to resting on m_decision.
	bool m_probing = true;
	/// Fields fed since the last decision, while resting.
	int m_fields_since_decision = 0;

	GSVector2i m_size{0, 0};
	int m_scale = 0;
	int m_rows = 0;
	int m_cols = 0;

	GSTexture* m_probe = nullptr;
	ProbeSlot m_slots[PROBE_SLOTS];
	/// Next field to probe, and oldest probe not yet collected. Retiring in index order guarantees
	/// compared signatures are consecutive fields.
	u32 m_next_index = 0;
	u32 m_retire_index = 0;

	std::vector<u8> m_prev;
	std::vector<u8> m_cur;
	bool m_have_prev = false;
	u32 m_prev_index = 0;
	int m_prev_applied = 0;
	int m_prev_parity = 0;

	int m_fields_probed = 0;
	int m_waited_frames = 0;
	/// Pairs discarded for same parity or a gap between fields. Explains a probe round that
	/// ends undecided.
	int m_unusable_pairs = 0;
	GSFieldShiftTally m_tally;

	/// GS-thread time spent up to the decision; reported once.
	u64 m_cpu_ticks = 0;
};
