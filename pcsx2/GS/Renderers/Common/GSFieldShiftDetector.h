// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSFieldShiftPolicy.h"

#include <memory>
#include <vector>

class GSTexture;
class GSDownloadTexture;

/// Decides, from the frames themselves, whether a field-mode game moves its projection half a
/// display line between fields. Only consulted where the field render is presented directly
/// (integer upscale of 2 or more), where that shift is the only correction left to make.
///
/// Runs for a bounded handful of fields, then rests on its decision. It measures again after a
/// video-mode change and every GS_FIELD_SHIFT_RECHECK_FIELDS fields, keeping the decision it has
/// in force meanwhile, so an answer taken on a boot logo does not outlive the logo. It
/// NEVER waits on the GPU: the readback is issued without a flush and collected on a later frame
/// through Poll(), and a backend that cannot answer Poll() without a flush simply gets the default
/// instead of a stall.
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

	/// Feed one field. `merge` is the merge target as it stands for this field, `size` its size,
	/// `scale` the integer upscale (one native line in device rows), `applied_offset_rows` the
	/// merge offset this field was actually drawn with, so the measurement can take it back out,
	/// and `field_parity` which of the two fields this is.
	void Update(GSTexture* merge, const GSVector2i& size, int scale, int applied_offset_rows, int field_parity);

private:
	enum class Decision
	{
		Pending,
		Shift,
		NoShift,
	};

	/// Columns sampled across the frame. 32 of them over a full frame height is ~29k samples a
	/// field, which swamps the difference the test is looking for by two orders of magnitude, and
	/// keeps a whole session's transfer under two megabytes.
	static constexpr int PROBE_COLUMNS = 32;
	/// Fields probed before the vote is taken, whatever it says.
	static constexpr int MAX_PROBE_FIELDS = 12;
	/// Frames the oldest queued readback may stay unfinished before the detector gives up on the
	/// backend. Never flushed: flushing is the stall this whole design exists to avoid.
	static constexpr int MAX_WAIT_FRAMES = 8;
	/// Readbacks in flight at once. EVERY field has to be probed -- a probe that waits for the
	/// previous readback samples every OTHER field, which is the same field twice and says nothing
	/// (see GSFieldShiftPairIsComparable). One slot per frame the GPU can be behind.
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
	/// Index of the next field to probe, and of the oldest probe not yet collected. Retiring in
	/// index order is what makes "these two signatures are consecutive fields" true by
	/// construction.
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
	/// Pairs thrown away because both fields were the same one, or because a field went unprobed
	/// between them. A run that is all of these is why a title can spend the whole probe budget
	/// with nothing decided.
	int m_unusable_pairs = 0;
	GSFieldShiftTally m_tally;

	/// GS-thread time this detector has cost, start to decision. Reported once, when it decides.
	u64 m_cpu_ticks = 0;
};
