// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GSPassScheduler on the deviceless None backend: draws are queued, reordered into one run per
// attachment pair, and emitted through a device that records what it was handed. Nothing here
// needs a graphics API; the textures are RAM-backed stubs and only their identity and State matter.

#include <gtest/gtest.h>

#include "GS/Renderers/Common/GSPassScheduler.h"
#include "GS/Renderers/Null/GSDeviceNone.h"

#include <cstring>
#include <memory>
#include <vector>

namespace
{
	struct Emitted
	{
		GSTexture* rt;
		GSTexture* ds;
		GSTexture::State rt_state;
		u32 first_index;
	};

	class RecordingDevice final : public GSDeviceNone
	{
	public:
		std::vector<Emitted> emitted;

		void DoRenderHW(GSHWDrawConfig& config) override
		{
			emitted.push_back({config.rt, config.ds, config.rt->GetState(), config.indices[0]});
		}
	};

	std::unique_ptr<GSTexture> Target()
	{
		return std::make_unique<GSTextureNone>(GSTexture::RenderTarget, 64, 64, 1, GSTexture::Format::Color);
	}

	std::unique_ptr<GSTexture> Depth()
	{
		return std::make_unique<GSTextureNone>(GSTexture::DepthStencil, 64, 64, 1, GSTexture::Format::DepthStencil);
	}

	// One triangle, tagged through its first index so the emitted order can be read back.
	class Draw
	{
	public:
		Draw(GSTexture* rt, GSTexture* ds, u16 tag, GSTexture* tex = nullptr)
		{
			std::memset(static_cast<void*>(&config), 0, sizeof(config));
			indices = {tag, 1, 2};
			config.rt = rt;
			config.ds = ds;
			config.tex = tex;
			config.verts = verts;
			config.nverts = 3;
			config.indices = indices.data();
			config.nindices = 3;
		}

		GSHWDrawConfig config;
		GSVertex verts[3] = {};
		std::vector<u16> indices;
	};
} // namespace

// A plain write to a target, with no read of any attachment, is the only thing held back.
TEST(GSPassScheduler, OnlyPlainWritesAreDeferrable)
{
	auto rt = Target();
	Draw plain(rt.get(), nullptr, 0);
	EXPECT_TRUE(GSPassScheduler::IsDeferrable(plain.config));

	Draw barrier(rt.get(), nullptr, 0);
	barrier.config.require_one_barrier = true;
	EXPECT_FALSE(GSPassScheduler::IsDeferrable(barrier.config));

	Draw self_read(rt.get(), nullptr, 0, rt.get());
	EXPECT_FALSE(GSPassScheduler::IsDeferrable(self_read.config));

	Draw date(rt.get(), nullptr, 0);
	date.config.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil;
	EXPECT_FALSE(GSPassScheduler::IsDeferrable(date.config));

	Draw no_target(nullptr, nullptr, 0);
	EXPECT_FALSE(GSPassScheduler::IsDeferrable(no_target.config));
}

// The case the scheduler exists for: draws alternating between two independent targets come out
// as one contiguous run per target, each run in its original order.
TEST(GSPassScheduler, PingPongBecomesOneRunPerTarget)
{
	auto a = Target();
	auto b = Target();
	GSPassScheduler sched;
	RecordingDevice dev;

	std::vector<std::unique_ptr<Draw>> draws;
	for (u16 i = 0; i < 6; i++)
	{
		draws.push_back(std::make_unique<Draw>((i & 1) ? b.get() : a.get(), nullptr, static_cast<u16>(10 + i)));
		ASSERT_EQ(sched.TryEnqueue(draws.back()->config), GSPassScheduler::Disposition::Queued) << i;
	}
	EXPECT_EQ(sched.GetCount(), 6u);
	EXPECT_TRUE(sched.References(a.get()));
	EXPECT_TRUE(sched.References(b.get()));

	// Geometry is taken by value: the caller's buffers are reused by the next draw.
	for (auto& d : draws)
		d->indices[0] = 0xFFFF;

	sched.Emit(&dev);
	ASSERT_EQ(dev.emitted.size(), 6u);
	const u16 expected[] = {10, 12, 14, 11, 13, 15};
	for (size_t i = 0; i < 6; i++)
		EXPECT_EQ(dev.emitted[i].first_index, expected[i]) << i;
	EXPECT_EQ(dev.emitted[0].rt, a.get());
	EXPECT_EQ(dev.emitted[3].rt, b.get());
	EXPECT_TRUE(sched.IsEmpty());
	EXPECT_FALSE(sched.References(a.get()));
}

// Read after write: a draw sampling a target that a queued draw writes cannot be moved.
TEST(GSPassScheduler, SamplingAQueuedTargetNeedsAFlush)
{
	auto a = Target();
	auto b = Target();
	GSPassScheduler sched;

	Draw write_a(a.get(), nullptr, 0);
	ASSERT_EQ(sched.TryEnqueue(write_a.config), GSPassScheduler::Disposition::Queued);

	Draw read_a(b.get(), nullptr, 1, a.get());
	EXPECT_EQ(sched.TryEnqueue(read_a.config), GSPassScheduler::Disposition::NeedsFlush);
}

// Write after read: a draw writing a texture that a queued draw samples cannot be moved either,
// including when it would join the run that already owns the attachment.
TEST(GSPassScheduler, WritingAQueuedSourceNeedsAFlush)
{
	auto a = Target();
	auto b = Target();
	auto c = Target();
	GSPassScheduler sched;

	Draw write_c(c.get(), nullptr, 0);
	ASSERT_EQ(sched.TryEnqueue(write_c.config), GSPassScheduler::Disposition::Queued);
	Draw read_b(a.get(), nullptr, 1, b.get());
	ASSERT_EQ(sched.TryEnqueue(read_b.config), GSPassScheduler::Disposition::Queued);
	EXPECT_TRUE(sched.References(b.get()));

	Draw write_b(b.get(), nullptr, 2);
	EXPECT_EQ(sched.TryEnqueue(write_b.config), GSPassScheduler::Disposition::NeedsFlush);
}

// Runs are reordered against each other, so two runs may not share an attachment.
TEST(GSPassScheduler, RunsMayNotShareADepthBuffer)
{
	auto a = Target();
	auto b = Target();
	auto z = Depth();
	GSPassScheduler sched;

	Draw first(a.get(), z.get(), 0);
	ASSERT_EQ(sched.TryEnqueue(first.config), GSPassScheduler::Disposition::Queued);
	Draw second(b.get(), z.get(), 1);
	EXPECT_EQ(sched.TryEnqueue(second.config), GSPassScheduler::Disposition::NeedsFlush);
}

// The run cap forces a flush rather than growing without bound.
TEST(GSPassScheduler, AFifthTargetNeedsAFlush)
{
	std::vector<std::unique_ptr<GSTexture>> targets;
	std::vector<std::unique_ptr<Draw>> draws;
	GSPassScheduler sched;
	for (u16 i = 0; i < 5; i++)
	{
		targets.push_back(Target());
		draws.push_back(std::make_unique<Draw>(targets.back().get(), nullptr, i));
	}
	for (u16 i = 0; i < 4; i++)
		ASSERT_EQ(sched.TryEnqueue(draws[i]->config), GSPassScheduler::Disposition::Queued) << i;
	EXPECT_EQ(sched.TryEnqueue(draws[4]->config), GSPassScheduler::Disposition::NeedsFlush);
}

// While a draw is queued its target reads as written, as it would have after an immediate draw;
// at emit the backend sees the state the first draw would have found, so it picks the same load op.
TEST(GSPassScheduler, TargetStateIsHiddenWhileQueuedAndRestoredAtEmit)
{
	auto a = Target();
	a->SetState(GSTexture::State::Cleared);
	GSPassScheduler sched;
	RecordingDevice dev;

	Draw d(a.get(), nullptr, 0);
	ASSERT_EQ(sched.TryEnqueue(d.config), GSPassScheduler::Disposition::Queued);
	EXPECT_EQ(a->GetState(), GSTexture::State::Dirty);

	sched.Emit(&dev);
	ASSERT_EQ(dev.emitted.size(), 1u);
	EXPECT_EQ(dev.emitted[0].rt_state, GSTexture::State::Cleared);
}

// Teardown drops the queue without rendering anything.
TEST(GSPassScheduler, ClearDropsWithoutEmitting)
{
	auto a = Target();
	GSPassScheduler sched;
	Draw d(a.get(), nullptr, 0);
	ASSERT_EQ(sched.TryEnqueue(d.config), GSPassScheduler::Disposition::Queued);
	sched.Clear();
	EXPECT_TRUE(sched.IsEmpty());
	EXPECT_FALSE(sched.References(a.get()));
}
