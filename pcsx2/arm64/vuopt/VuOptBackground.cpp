// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuOptBackground.h"

#include "arm64/AsmHelpers.h"
#include "common/Assertions.h"
#include "common/Threading.h"

#include <cstring>

namespace vuopt::bg
{
	namespace a64 = vixl::aarch64;

	//------------------------------------------------------------------
	// CodeSink
	//------------------------------------------------------------------

	class CodeSink::Recorder final : public ArmAddressRecorder
	{
	public:
		explicit Recorder(CodeSink& s)
			: s_(s)
		{
		}

		// Every address goes in the fixed 16-byte form: an address in the
		// outside world then means the same wherever the code lands, and one
		// inside the buffer is patched by the copy.
		MoveForm ClassifyMove(const void*) override { return MoveForm::CanonicalAbs; }

		void OnCanonicalAbsMove(u8* at, const void* addr) override
		{
			if (const s32 id = s_.PlaceholderId(addr); id >= 0)
				s_.fixups.push_back({Off(at), Fixup::SiteAbs, static_cast<u32>(id), 0});
			else if (s_.InCode(addr))
				s_.fixups.push_back({Off(at), Fixup::IntraAbs, 0, static_cast<u64>(static_cast<const u8*>(addr) - s_.base_)});
		}

		void OnAdrp(u8*, const void*) override { Fail("ADRP emitted"); }

		void OnDirectBranch(u8* at, const void* target, bool) override
		{
			const s32 id = s_.PlaceholderId(target);
			if (id >= 0)
				s_.fixups.push_back({Off(at), Fixup::Site26, static_cast<u32>(id), 0});
			else if (!s_.InCode(target))
				s_.fixups.push_back({Off(at), Fixup::External26, 0, reinterpret_cast<u64>(target)});
			// (a branch to a place in the buffer moves with it)
		}

		bool WantsLongCondBranch(const void* target) override { return !s_.InCode(target); }

		void OnAbsoluteTarget(const void* target) override
		{
			if (s_.PlaceholderId(target) >= 0 || s_.InCode(target))
				Fail("buffer address in an absolute branch");
		}

	private:
		CodeSink& s_;
		u32 Off(const u8* at) const { return static_cast<u32>(at - s_.base_); }
		void Fail(const char* why)
		{
			if (s_.failed.empty())
				s_.failed = why;
		}
	};

	CodeSink::CodeSink()
		: base_(new u8[kCapacity])
		, rec_(std::make_unique<Recorder>(*this))
	{
	}

	CodeSink::~CodeSink()
	{
		pxAssert(!masm_);
		delete[] base_;
	}

	s32 CodeSink::PlaceholderId(const void* p) const
	{
		const u8* b = static_cast<const u8*>(p);
		const u8* z = base_ + kCapacity - kPlaceholderZone;
		if (b < z || b >= base_ + kCapacity)
			return -1;
		return static_cast<s32>((b - z) / 4);
	}

	bool CodeSink::InCode(const void* p) const
	{
		const u8* b = static_cast<const u8*>(p);
		return b >= base_ && b < base_ + kCapacity - kPlaceholderZone;
	}

	void CodeSink::Begin()
	{
		pxAssertRel(!armAsm && !masm_, "vuopt worker: assembler already bound");
		fixups.clear();
		failed.clear();
		// The same assembler setup as microVU's (mVUreset's persistent one):
		// v31 and x17 are not vixl's to borrow.
		auto* m = new a64::MacroAssembler(base_, kCapacity - kPlaceholderZone);
		m->GetScratchVRegisterList()->Remove(31);
		m->GetScratchRegisterList()->Remove(RSCRATCHADDR.GetCode());
		masm_ = m;
		armAsm = m;
		armAsmPtr = base_;
		armAsmCapacity = kCapacity - kPlaceholderZone;
		armConstantPool = nullptr;
		armAddressRecorder = rec_.get();
	}

	u32 CodeSink::End()
	{
		auto* m = static_cast<a64::MacroAssembler*>(masm_);
		pxAssertRel(m && armAsm == m, "vuopt worker: assembler not bound");
		m->FinalizeCode(a64::MacroAssembler::kFallThrough);
		const u32 size = static_cast<u32>(m->GetSizeOfCodeGenerated());
		delete m;
		masm_ = nullptr;
		armAsm = nullptr;
		armAsmPtr = nullptr;
		armAsmCapacity = 0;
		armAddressRecorder = nullptr;
		return size;
	}

	bool PatchAbsMov(u32* insn, u64 value)
	{
		// movz xN, #imm16 (hw=0) then movk xN, #imm16, lsl 16*i
		constexpr u32 kOpMask = 0xFF800000u, kMovz = 0xD2800000u, kMovk = 0xF2800000u;
		for (int i = 0; i < 4; i++)
		{
			if ((insn[i] & kOpMask) != (i == 0 ? kMovz : kMovk) || (insn[i] & 0x00600000u) != (static_cast<u32>(i) << 21))
				return false;
		}
		for (int i = 0; i < 4; i++)
			insn[i] = (insn[i] & ~(0xFFFFu << 5)) | (static_cast<u32>((value >> (16 * i)) & 0xFFFFu) << 5);
		return true;
	}

	bool PatchRel26(u32* insn, const u8* at, const void* target)
	{
		// B = 0x14000000, BL = 0x94000000
		if ((*insn & 0x7C000000u) != 0x14000000u)
			return false;
		const s64 disp = GetPCDisplacement(at, target);
		if (!vixl::IsInt26(disp))
			return false;
		*insn = (*insn & 0xFC000000u) | (static_cast<u32>(disp) & 0x03FFFFFFu);
		return true;
	}

	//------------------------------------------------------------------
	// Worker
	//------------------------------------------------------------------

	void Worker::Start()
	{
		if (thread_.joinable())
			return;
		{
			std::lock_guard lk(mu_);
			stop_ = false;
		}
		thread_ = std::thread([this] { Loop(); });
	}

	void Worker::Stop()
	{
		if (!thread_.joinable())
			return;
		{
			std::lock_guard lk(mu_);
			stop_ = true;
			queue_.clear();
		}
		cv_.notify_all();
		thread_.join();
		std::lock_guard lk(mu_);
		finished_.clear();
		completed_.store(0, std::memory_order_relaxed);
		running_.reset();
	}

	bool Worker::Submit(std::shared_ptr<Job> job)
	{
		{
			std::lock_guard lk(mu_);
			if (stop_ || !thread_.joinable() || queue_.size() >= kMaxQueued)
				return false;
			queue_.push_back(std::move(job));
		}
		cv_.notify_one();
		return true;
	}

	void Worker::Wait(const std::shared_ptr<Job>& job)
	{
		std::unique_lock lk(mu_);
		doneCv_.wait(lk, [&] { return job->done.load(std::memory_order_acquire) || stop_; });
	}

	void Worker::RemoveQueued(const std::function<bool(const Job&)>& drop)
	{
		std::lock_guard lk(mu_);
		for (auto it = queue_.begin(); it != queue_.end();)
			it = drop(**it) ? queue_.erase(it) : std::next(it);
	}

	void Worker::TakeCompleted(std::vector<std::shared_ptr<Job>>& out)
	{
		std::lock_guard lk(mu_);
		for (auto& j : finished_)
			out.push_back(std::move(j));
		finished_.clear();
		completed_.store(0, std::memory_order_relaxed);
	}

	void Worker::Hold(bool on)
	{
		{
			std::lock_guard lk(mu_);
			hold_ = on;
		}
		cv_.notify_all();
	}

	void Worker::SetGate(std::function<void()> gate)
	{
		std::lock_guard lk(mu_);
		gate_ = std::move(gate);
	}

	void Worker::WaitIdle()
	{
		std::unique_lock lk(mu_);
		doneCv_.wait(lk, [&] { return stop_ || !thread_.joinable() || (queue_.empty() && !running_); });
	}

	size_t Worker::Queued()
	{
		std::lock_guard lk(mu_);
		return queue_.size();
	}

	void Worker::Loop()
	{
		Threading::SetNameOfCurrentThread("VU1 vuopt compile");
		// Below the emulator's threads: a tier-up may wait, the VU must not.
		Threading::ThreadHandle::GetForCallingThread().SetNicePriority(10);
		for (;;)
		{
			std::function<void()> gate;
			{
				std::unique_lock lk(mu_);
				cv_.wait(lk, [&] { return stop_ || (!hold_ && !queue_.empty()); });
				if (stop_)
					break;
				running_ = std::move(queue_.front());
				queue_.pop_front();
				gate = gate_;
			}
			if (gate)
				gate();
			running_->Run();
			{
				std::lock_guard lk(mu_);
				running_->done.store(true, std::memory_order_release);
				if (!stop_)
				{
					finished_.push_back(std::move(running_));
					completed_.store(static_cast<u32>(finished_.size()), std::memory_order_relaxed);
				}
				running_.reset();
			}
			doneCv_.notify_all();
		}
		doneCv_.notify_all();
	}
} // namespace vuopt::bg
