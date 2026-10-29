// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The background half of the vu-aot live tier: one low-priority worker thread
// that builds regions while microVU keeps running the program, and the private
// code buffer it emits into.
//
// The worker runs jobs (VuAotJob, in microVU_VuAot-arm64.inl) that hold copies
// of everything they read. The VU1 thread publishes a job's result by copying
// its code into microVU's code cache and patching the places the buffer could
// not know (the fixups below).
//
// The buffer is private because microVU's code cache has one writer, the VU1
// thread, and on Apple platforms it is W^X with the write window owned by that
// thread. The worker writes only its own plain memory; the copy happens inside
// one of microVU's code-cache episodes.

#include "common/Pcsx2Defs.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vuaot::bg
{
	// A place in the job's code the copy has to patch.
	struct Fixup
	{
		enum Kind : u8
		{
			// movz+movk*3 holding an address inside the buffer; `value` is
			// its offset from the buffer's start.
			IntraAbs,
			// B or BL whose target is a placeholder (`id`, see CodeSink); the
			// publisher decides what it stands for.
			Site26,
			// movz+movk*3 holding a placeholder (`id`): an address only the
			// publisher knows.
			SiteAbs,
			// B or BL to an absolute address outside the buffer (`value`): a C
			// helper close enough to the buffer for armEmitCall's short form.
			// The copy re-aims it, through a veneer if need be.
			External26,
			// MOVZ+MOVK run of `id` instructions holding an address outside the
			// buffer (`value`): a C helper, a global, VU state, a graph block.
			// The copy leaves it alone; a saved region re-bases it at install.
			OutsideAbs,
			// An 8-byte data word holding `value` (the region builder's bail
			// records: the program, a node's handle). The copy leaves it alone.
			Data64,
		};
		u32 offset;
		Kind kind;
		u32 id;
		u64 value;
	};

	// The worker's emission buffer. While bound (Begin..End) this thread's
	// armAsm writes into it, and every address-bearing emission goes through
	// the recorder, which logs the ones the copy must patch or re-aim as fixups.
	//
	// Placeholders are addresses in a reserved tail of the buffer that nothing
	// is ever written to. A branch to one is always in B/BL range, and its
	// meaning (an exit block, a jump tail, a microVU stub, a C helper) is the
	// job's business.
	class CodeSink
	{
	public:
		static constexpr size_t kCapacity = 16u << 20;
		static constexpr size_t kPlaceholderZone = 64u << 10;
		static constexpr u32 kMaxPlaceholders = static_cast<u32>(kPlaceholderZone / 4);

		CodeSink();
		~CodeSink();
		CodeSink(const CodeSink&) = delete;
		CodeSink& operator=(const CodeSink&) = delete;

		void Begin();
		// Finalizes and unbinds; returns the number of bytes emitted.
		u32 End();

		u8* Base() const { return base_; }
		const void* Placeholder(u32 id) const { return base_ + kCapacity - kPlaceholderZone + id * 4; }
		// The placeholder id of `p`, or -1.
		s32 PlaceholderId(const void* p) const;
		bool InCode(const void* p) const;
		// A data word the emission wrote at `at` (inside the buffer) holds `value`.
		void NoteData64(const u8* at, u64 value);

		std::vector<Fixup> fixups;
		// Set when an emission took a shape the copy cannot relocate.
		std::string failed;

	private:
		class Recorder;
		u8* base_ = nullptr;
		std::unique_ptr<Recorder> rec_;
		void* masm_ = nullptr; // the bound vixl::aarch64::MacroAssembler
	};

	// Patch helpers (instruction words already in memory). False when the
	// word is not the expected shape or the displacement does not fit.
	bool PatchAbsMov(u32* insn, u64 value);
	bool PatchRel26(u32* insn, const u8* at, const void* target);

	struct Job
	{
		virtual ~Job() = default;
		// Runs on the worker thread.
		virtual void Run() = 0;
		std::atomic<bool> done{false};
	};

	// One thread, a bounded queue, low priority.
	class Worker
	{
	public:
		// A scene that loads new VU code requests a few hundred regions in a burst
		// (God of War II: about 800 jobs over 300 frames).
		static constexpr size_t kMaxQueued = 256;

		~Worker() { Stop(); }

		void Start();
		// Drops every queued job and waits for the one running, if any.
		void Stop();
		bool Running() const { return thread_.joinable(); }

		// False when the queue is full or the worker is not running.
		bool Submit(std::shared_ptr<Job> job);
		// Waits for `job` (deterministic mode).
		void Wait(const std::shared_ptr<Job>& job);
		// Removes queued (not started) jobs for which `drop` returns true.
		void RemoveQueued(const std::function<bool(const Job&)>& drop);
		// Moves the finished jobs out, in completion order.
		void TakeCompleted(std::vector<std::shared_ptr<Job>>& out);
		// Finished jobs not taken yet (read on every VU1 dispatch while the
		// tier is on: one relaxed load).
		u32 CompletedCount() const { return completed_.load(std::memory_order_relaxed); }

		// Test hooks. Hold: no new job starts while held. Gate: called on the
		// worker at the start of each job (a test blocks in it to keep a job
		// in flight). WaitIdle: until the queue is empty and nothing runs.
		void Hold(bool on);
		void SetGate(std::function<void()> gate);
		void WaitIdle();
		size_t Queued();

	private:
		void Loop();

		std::thread thread_;
		std::mutex mu_;
		std::condition_variable cv_;      // the worker waits for work
		std::condition_variable doneCv_;  // waiters wait for a job or for idle
		std::deque<std::shared_ptr<Job>> queue_;
		std::vector<std::shared_ptr<Job>> finished_;
		std::shared_ptr<Job> running_;
		std::function<void()> gate_;
		bool stop_ = false;
		bool hold_ = false;
		std::atomic<u32> completed_{0};
	};
} // namespace vuaot::bg
