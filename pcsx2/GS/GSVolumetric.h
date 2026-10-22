// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Volumetric: depth that lines up with the presented image, and multiview stereo, for frontends that
// show the game in 3D (the Steam Frame VR frontend reads it through VKLibretro::Frame). Built with
// ENABLE_VOLUMETRIC (PCSX2_VOLUMETRIC); without it, the GS's hooks below are empty inline functions.
//
// The GS has no notion of "the depth of the displayed frame": the display circuits read colour
// targets, and a depth buffer is only tied to a colour target while they are drawn together. So
// the hardware renderer remembers which depth target each colour target's 3D geometry was drawn
// with, snapshots that depth (and the colour) when the frame is done - when drawing moves to
// another colour target, or before the next frame clears a shared Z buffer - and at vsync copies
// the displayed target's depth (the snapshot, or the live target if it is still current) into
// float canvases through the same rectangles the display merge uses. Depth values stay raw: GS Z /
// 2^32, larger is nearer. A HUD drawn with Z writes sits at the format's maximum.

#pragma once

#include "GS/GSVector.h"

#include <span>
#include <string>

class GSTexture;

struct GSVolumetricFrame
{
	// R32F (GSTexture::Format::DepthColor) canvas in the merged frame's space; 0 where nothing
	// with depth was displayed. Null when this vsync has no usable depth.
	GSTexture* depth = nullptr;
	// Multiplying a sample by this gives 0..1 of the Z buffer format's range (2^32 / Zmax).
	float depth_scale = 0.0f;
	// RGBA8 canvas like depth: the colour as it was when the depth was snapshotted. Pixels where the
	// presented frame differs were drawn after the 3D (HUD, overlays). Null when not available.
	GSTexture* prehud = nullptr;
	// Geometric stereo prototype (GSVolumetric::SetStereo): which eye the presented frame was drawn
	// for, 0 = left, 1 = right; -1 without stereo.
	int stereo_eye = -1;
	// Multiview stereo: the presented frame is a 2-layer texture, layer 0 for the left eye and 1 for
	// the right (stereo_eye is then -1).
	bool stereo_layers = false;
	// R32F canvas like depth: the depth target as it is at vsync, when depth comes from a snapshot
	// taken during this frame. A HUD drawn after it with Z writes shows here at the Z format's
	// maximum, which tells it apart from lights and effects drawn after it without Z.
	GSTexture* live_depth = nullptr;
	// The part of the merged frame (and so of the depth canvas) that is presented, as UVs ...
	GSVector4 src_uv = GSVector4::zero();
	// ... and where that part lands in the presented canvas, in pixels.
	GSVector4 draw_rect = GSVector4::zero();
};

#ifdef PCSX2_VOLUMETRIC

namespace GSVolumetric
{
	// Off by default; a host that consumes depth turns it on before the GS opens, or while it runs
	// (depth then starts with the next frames drawn).
	void SetEnabled(bool enabled);
	bool IsEnabled();
	// The pre-HUD colour and live depth canvases besides the depth, at the upscaled resolution. Off by
	// default: the light guns only sample the depth, which is then kept at the PS2's own resolution
	// (a few hundred pixels across is plenty for aiming, and copies at 3x cost real GPU time).
	void SetDepthExtras(bool enabled);
	bool DepthExtras();

	// Logs, every 300 vsyncs, which colour/depth targets the Z-writing draws used.
	void SetTrace(bool enabled);
	bool IsTraceEnabled();

	// Written on the GS thread during VSync, read by the present path of the same vsync.
	GSVolumetricFrame& CurrentFrame();

	// Geometric stereo, frame-alternating prototype: the eye flips with each new frame, and every
	// draw is shifted on screen by the current eye's offset from the screen centre times how far
	// behind the screen its geometry is - the parallax of looking through a window, from that eye:
	//   shift (NDC) = eye_offset (NDC) * depth_factor * f
	//   Q mode: f = max(0, 1 - convergence * Q)          (Q = 1/w of perspective vertices)
	//   Z mode: f = clamp(1 - Znorm / convergence, 0, 1) (Znorm = Z / format maximum)
	// f is 0 for depth at the convergence (on the screen) and 1 for the farthest geometry.
	// Screen-space (FST) draws and HUDs stamped at the maximum depth are not shifted (except the
	// overlays on a shot, see SetShotAnchors).
	//
	// With multiview, both eyes are drawn in every frame instead: every render target gets one layer
	// per eye and every draw is a VK_KHR_multiview draw that applies each eye's shift to its own
	// layer. Needs the Vulkan renderer, with multiview turned on when the GS device is created.
	//
	// Portal (Z mode only, the host's "Volumetric+"): the scene gets a fixed size in the room instead
	// of a fixed depth range. The game's camera is taken to be where the viewer stood at the nominal
	// distance D0, so Znorm at the convergence is on the glass and u = Znorm / convergence (Z ~ 1/w)
	// puts geometry D0 (1/u - 1) behind it, out to infinity at u = 0. Every vertex is then projected
	// onto the glass from the eye's real position, toward and away from the screen too:
	//   r = D / D0, den = 1 + u (r - 1), p' = (p r + eye_offset (1 - u)) / den
	// p is the vertex in NDC. At D = D0 this is the window shift with a depth factor of 1; leaning in
	// shrinks far scenery toward the centre (more of the world shows through the window), near
	// scenery stays put on the glass.
	struct StereoSettings
	{
		bool enabled = false;
		bool z_mode = false;
		float convergence = 1.0f;
		bool multiview = false;
		bool portal = false;
		// Additive 2D overlays that come and go are effects in the scene (the game's profile turns this
		// on, see SetSceneDepth).
		bool fx_overlays = false;
	};
	//
	// Flat (enabled off, multiview on): the device draws one layer, as upstream does, and can switch
	// to two while it runs (GSDevice::SetStereoLayers, done by the hardware renderer at vsync).
	//
	// SetStereo (any thread) takes effect at the next frame boundary, when the GS thread calls
	// ApplyStereo, so that each frame is drawn with one set of settings and a device layer count
	// that matches them. GetStereo is the set in effect (GS thread).
	void SetStereo(const StereoSettings& settings);
	void ApplyStereo();
	const StereoSettings& GetStereo();
	int CurrentEye(); // 0 left, 1 right
	// Per eye, set by the host every frame: its offset from the screen centre in NDC (x right, y
	// down, as the eye would see the screen move) and the depth factor R / (D + R) for geometry R
	// behind a screen D away (1 in portal mode), and for the portal its distance over the nominal one.
	void SetEyeView(int eye, float offset_x, float offset_y, float depth_factor, float distance_ratio = 1.0f);
	// The eye's on-screen shift for the farthest geometry (offset times depth factor), in x and y, and
	// its distance ratio in z.
	GSVector4 EyeShift(int eye);

	// Where the light guns' shots landed lately, for the effects a game stamps over its picture there.
	// Crisis Zone draws its hit bursts as 2D sprites with its HUD's depth and no depth test, so they
	// carry no depth of their own and would sit on the glass. A 2D overlay (an FST or Q = 1 draw into
	// the displayed picture) centred on a shot is drawn at the scene's depth there instead. u, v: 0..1
	// of the game's picture; znorm: the scene's Z / the format's maximum at that point.
	struct ShotAnchor
	{
		float u = 0.0f, v = 0.0f, znorm = 0.0f;
	};
	static constexpr size_t MAX_SHOT_ANCHORS = 16;
	// Any thread; takes effect with the stereo settings, at ApplyStereo. GetShotAnchors: GS thread.
	void SetShotAnchors(const ShotAnchor* anchors, size_t count);
	std::span<const ShotAnchor> GetShotAnchors();

	// The scene's depth over the game's picture, from a recent frame (the host reads it back from the
	// depth canvas), for the effects a game draws as 2D overlays wherever they happen: Crisis Zone's
	// bursts on a dying enemy come a second after the shot, away from it. With fx_overlays, an additive
	// 2D overlay that is not a HUD piece is drawn at the nearest depth under its middle.
	// SCENE_DEPTH_WIDTH x SCENE_DEPTH_HEIGHT Znorm values, row by row over the picture (0..1 u, v);
	// negative where unknown. Null: none. Any thread; takes effect at ApplyStereo.
	static constexpr u32 SCENE_DEPTH_WIDTH = 80, SCENE_DEPTH_HEIGHT = 60;
	void SetSceneDepth(const float* znorm);
	// The nearest (largest) Znorm in the picture's rectangle u0..u1, v0..v1, leaving out HUDs stamped at
	// the maximum; negative if there is none. GS thread.
	float SceneDepth(float u0, float v0, float u1, float v1);

	// Debugging: at the next present, write <prefix>-left.png / -right.png (the images handed to the
	// host) and <prefix>-layer0.png / -layer1.png (the frame's two layers). Any thread.
	void RequestPresentDump(std::string prefix);
	std::string TakePresentDumpRequest(); // the pending prefix, or empty
	// Debugging: log every hardware draw of the next `frames` frames.
	void ArmDrawTrace(int frames);
	bool DrawTraceActive();
	u32 DrawTraceNumber(); // which trace is running (counts from 1)

	// GSRenderer::VSync: before the display merge, and after it with the eye the displayed target
	// was drawn for (-1 if unknown), whether the frame repeats the last one, and whether the device
	// draws both eyes per frame.
	void BeginVSync();
	void EndMerge(int displayed_eye, bool idle_frame, bool multiview);
	// GSRenderer::VSync: the part of the merged frame that is presented, as UVs, and where it lands
	// in the presented canvas, in pixels (GSVolumetricFrame::src_uv, draw_rect).
	void SetPresentedRect(const GSVector4& src_uv, const GSVector4& draw_rect);
} // namespace GSVolumetric

#else

// Built without ENABLE_VOLUMETRIC: the hooks the GS calls do nothing.
namespace GSVolumetric
{
	inline bool IsEnabled() { return false; }
	inline void BeginVSync() {}
	inline void EndMerge(int displayed_eye, bool idle_frame, bool multiview) {}
	inline void SetPresentedRect(const GSVector4& src_uv, const GSVector4& draw_rect) {}
} // namespace GSVolumetric

#endif
