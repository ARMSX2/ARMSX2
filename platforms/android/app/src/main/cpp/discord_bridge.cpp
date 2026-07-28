// SPDX-FileCopyrightText: 2026 ARMSX2 contributors
// SPDX-License-Identifier: MIT
//
// Licensed MIT, deliberately, and NOT GPL like the rest of ARMSX2.
//
// This file is on the helper side of the Discord process boundary: it links, or
// belongs to the process that links, Discord's proprietary Social SDK. A GPL-3.0+
// file cannot be combined with a proprietary library whose corresponding source we
// cannot supply, so licensing this GPL would recreate exactly the defect the
// separate process exists to avoid -- the boundary has to hold in the licence
// headers as well as in the linker.
//
// MIT rather than Apache-2.0 because it is GPL-compatible in the other direction
// too: the emulator side (DiscordPresence.kt and the Friends UI, which stay GPL)
// consumes the shared IPC definitions here, and that only works if this side is
// permissive.
//
// Do not "fix" this back to the PCSX2 GPL header. It is not PCSX2 code, and the
// licence is load-bearing.
//
// Copyright (c) 2026 ARMSX2 contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// Discord Social SDK bridge: rich presence, and which friends are in ARMSX2 right now.
//
// DELIBERATELY FREE-STANDING. This file includes no PCSX2 header and links against no PCSX2 code:
// only JNI and the Discord SDK. That is a licensing constraint, not a style preference — ARMSX2 is
// GPL-3.0+ and the Social SDK is proprietary, so the two must not become one linked program. This
// builds into its own libarmsx2_discord.so, loaded only in the :discord process; emucore has no
// DT_NEEDED on the SDK and never loads it.
//
// If you are tempted to call Host::AddOSDMessage or anything else from the emulator here: don't.
// Send a message to the app process and let it do that.
//
// Shape of this file, and why:
//
// Kotlin POLLS this, rather than the bridge calling up into Java. The SDK fires its callbacks on
// its own threads, so pushing would mean AttachCurrentThread plus a global ref whose lifetime has
// to outlive both the Activity and the SDK — for a feature the SDK itself only runs while the app
// is foregrounded. Polling a snapshot behind a mutex has none of that, and "once a second while a
// screen is open" is not a budget worth optimising.
//
// Everything is behind ARMSX2_HAS_DISCORD. A tree without the SDK staged still builds; every entry
// point below has a stub that reports "unavailable", so the Kotlin side needs no build-flavour
// awareness at all.

#include <jni.h>

#include <android/log.h>

#define DTAG "ARMSX2Discord"
#define DLOGI(...) __android_log_print(ANDROID_LOG_INFO, DTAG, __VA_ARGS__)
#define DLOGW(...) __android_log_print(ANDROID_LOG_WARN, DTAG, __VA_ARGS__)

#ifdef ARMSX2_HAS_DISCORD

// discordpp.h is a single-header library: everything below its DISCORDPP_IMPLEMENTATION guard is
// the C++ wrapper's bodies, and the shipped .so exports ONLY the C API (Discord_*) — checked with
// llvm-nm, zero `discordpp::` symbols in it. Without this define the file compiles perfectly and
// then fails at link with an undefined symbol for every single call, which reads like a missing
// library rather than a missing macro. This must stay the one and only translation unit that
// defines it; a second would be duplicate-symbol errors instead.
#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

#include <dlfcn.h>

namespace
{
	/// Drain the Social SDK's callback queue.
	///
	/// NOT discordpp::RunCallbacks(). That calls Discord_RunCallbacks(), and upstream PCSX2
	/// statically links 3rdparty/discord-rpc — a completely different Discord library — which
	/// exports a function of exactly that name. Out of the Social SDK's 513 exported symbols it is
	/// the ONLY collision, and it is the one that matters.
	///
	/// A static archive member beats a shared library, and VMManager.cpp pulls that member in for
	/// its own Discord_Initialize/Discord_UpdatePresence, so the linker resolved our pump to
	/// discord-rpc's. With no RPC connection that is a silent no-op: the client initialised, the
	/// browser opened, Discord approved, the code came back — and not one callback was ever
	/// delivered. No log callback, no status callback, no authorization result. Nothing to see in
	/// any log, because the code that would have logged never ran.
	///
	/// Link order cannot fix it (the member is pulled in regardless), so resolve the SDK's own
	/// symbol straight out of its .so. RTLD_NOLOAD because emucore already has a DT_NEEDED on it —
	/// this only wants a handle, never a second copy.
	void PumpSdkCallbacks()
	{
		using RunCallbacksFn = void (*)();
		static RunCallbacksFn fn = [] () -> RunCallbacksFn {
			void* handle = dlopen("libdiscord_partner_sdk.so", RTLD_NOW | RTLD_NOLOAD);
			if (!handle)
			{
				DLOGW("dlopen(libdiscord_partner_sdk.so) failed: %s", dlerror());
				return nullptr;
			}
			auto* sym = reinterpret_cast<RunCallbacksFn>(dlsym(handle, "Discord_RunCallbacks"));
			if (!sym)
				DLOGW("dlsym(Discord_RunCallbacks) failed: %s", dlerror());
			else
				DLOGI("SDK pump resolved at %p (discord-rpc's same-named symbol bypassed)", (void*)sym);
			return sym;
		}();
		if (fn)
			fn();
	}
} // namespace

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	// Connection state, mirrored to Kotlin as an int so the UI can render without a second call.
	// Kept deliberately coarse: the UI only distinguishes "can I press Connect", "wait", and "done".
	enum class BridgeStatus : int
	{
		Disabled = 0,
		Disconnected = 1,
		Authorizing = 2,
		Connecting = 3,
		Connected = 4,
		Failed = 5,
	};

	struct Friend
	{
		std::string name;
		std::string game;   // title they are playing; empty means sitting in the library
		std::string serial; // for cover art on our side; empty when they are in the library
		std::string avatar; // their Discord avatar
	};

	// Record/field separators for the friends list handed to Kotlin. Deliberately control
	// characters: a display name or a game title can legitimately contain almost any printable
	// character, and a friend called "a|b" must not turn into two rows.
	constexpr char kFieldSep = '\x1f';
	constexpr char kRecordSep = '\x1e';

	struct State
	{
		std::mutex mutex;
		std::shared_ptr<discordpp::Client> client;

		std::thread pump;
		std::atomic<bool> pump_quit{false};

		std::atomic<int> status{static_cast<int>(BridgeStatus::Disconnected)};

		// Handed to Kotlin once so it can persist it and skip the browser next launch. Cleared on
		// read: it is a credential, and there is no reason for it to sit in native memory after the
		// one consumer has it.
		std::string fresh_token;
		std::string error;
		std::vector<Friend> friends;
		// The signed-in account, so the UI can show whose Discord this is.
		std::string self_name;
		std::string self_avatar;

		// Last presence asked for, replayed after a (re)connect. Presence set before Connect is
		// cleared by the connect itself, which the header calls out explicitly, so the only safe
		// way to keep it is to remember it and set it again once connected.
		std::string want_serial;
		std::string want_title;
		std::string want_cover;
	};

	State& S()
	{
		static State s;
		return s;
	}

	constexpr uint64_t kApplicationId = 1531447040435814411ull;
	// Must match the intent filter in AndroidManifest.xml, and the redirect registered in the
	// Discord developer portal. The SDK does not derive it for us.
	const char* kRedirectUri = "discord-1531447040435814411:/authorize/callback";
	// Presence and the friends list. Deliberately no voice scopes: we do not ship voice, and the
	// consent screen should not ask for anything we cannot use.
	const char* kScopes = "sdk.social_layer_presence sdk.social_layer";
	// LargeImage takes "an identifier OR URL" (discordpp.h), and we use both kinds.
	//
	// Our own logo is an Art Asset uploaded to the application in the developer portal, referenced
	// by its key. Pointing at the logo in the repo instead looked equivalent and was not: that file
	// is the OLD mark, so the presence quietly showed outdated branding. The portal upload is the
	// one that gets refreshed when the logo changes, so the key is the correct reference and the
	// artwork can be updated without shipping a build.
	const char* kIdleImageKey = "armsx2";

	const char* StatusName(BridgeStatus s)
	{
		switch (s)
		{
			case BridgeStatus::Disabled: return "Disabled";
			case BridgeStatus::Disconnected: return "Disconnected";
			case BridgeStatus::Authorizing: return "Authorizing";
			case BridgeStatus::Connecting: return "Connecting";
			case BridgeStatus::Connected: return "Connected";
			case BridgeStatus::Failed: return "Failed";
		}
		return "?";
	}

	void SetStatus(BridgeStatus s)
	{
		S().status.store(static_cast<int>(s), std::memory_order_release);
		DLOGI("status -> %s", StatusName(s));
	}

	// Push the remembered game to Discord. Caller must NOT hold the mutex: UpdateRichPresence can
	// invoke its callback inline.
	void PushPresence()
	{
		std::shared_ptr<discordpp::Client> client;
		std::string serial, title, cover;
		{
			std::lock_guard<std::mutex> lock(S().mutex);
			client = S().client;
			serial = S().want_serial;
			title = S().want_title;
			cover = S().want_cover;
		}
		if (!client)
			return;

		discordpp::Activity activity;
		// Name and applicationId are overwritten by the SDK — the header says so, and it is why
		// Discord will read "Playing ARMSX2" with the game underneath rather than the other way
		// round. Details is therefore where the game has to go.
		activity.SetType(discordpp::ActivityTypes::Playing);
		if (!title.empty())
		{
			activity.SetDetails(title);
			// State is our in-a-game marker, not just the serial: a friend's client decides
			// "library or game" purely from whether this field is set, so a game that has no
			// serial (a raw ELF) still has to set something or it reads as being in the library.
			activity.SetState(serial.empty() ? std::string("-") : serial);
		}
		else
		{
			// No State at all. That absence is what tells a friend's client we are in the library,
			// which beats matching on this string — it is English, and it is a display string.
			activity.SetDetails(std::string("In the library"));
		}

		// The game's own cover while playing, our logo when idle — never upstream PCSX2's artwork.
		// The cover URL is whatever the library is already showing (xlenore's cover repos, keyed by
		// serial), so a game with no cover published simply falls back to the logo rather than
		// showing a broken image. Mixing the two forms is fine: Discord resolves each asset field
		// independently, so a URL in the large slot and an asset key in the small one both render.
		discordpp::ActivityAssets assets;
		assets.SetLargeImage(cover.empty() ? std::string(kIdleImageKey) : cover);
		assets.SetLargeText(title.empty() ? std::string("ARMSX2") : title);
		if (!cover.empty())
		{
			// Small badge in the corner keeps ARMSX2 identifiable when the big image is a cover.
			assets.SetSmallImage(std::string(kIdleImageKey));
			assets.SetSmallText(std::string("ARMSX2"));
		}
		activity.SetAssets(std::move(assets));

		client->UpdateRichPresence(std::move(activity), [](discordpp::ClientResult result) {
			if (!result.Successful())
				DLOGW("presence update failed: %s", result.Error().c_str());
		});
	}

	void RefreshFriends()
	{
		std::shared_ptr<discordpp::Client> client;
		{
			std::lock_guard<std::mutex> lock(S().mutex);
			client = S().client;
		}
		if (!client)
			return;

		std::vector<Friend> found;
		// OnlinePlayingGame is the SDK's own grouping for "friend, online, in THIS application" —
		// exactly the set worth surfacing, and it means we never enumerate or display the rest of
		// someone's friends list.
		for (const auto& rel : client->GetRelationshipsByGroup(discordpp::RelationshipGroupType::OnlinePlayingGame))
		{
			const auto user = rel.User();
			if (!user.has_value())
				continue;

			Friend f;
			f.name = user->DisplayName();
			if (f.name.empty())
				continue;
			f.avatar = user->AvatarUrl(discordpp::UserHandle::AvatarType::Webp,
				discordpp::UserHandle::AvatarType::Png);

			// We author the activity these friends are publishing, so this reads back exactly what
			// PushPresence wrote: Details is the game title, State is the serial (or "-"), and no
			// State at all means they are sitting in the library.
			if (const auto activity = user->GameActivity(); activity.has_value())
			{
				const auto state = activity->State();
				if (state.has_value() && !state->empty())
				{
					f.game = activity->Details().value_or(std::string());
					if (*state != "-")
						f.serial = *state;
				}
			}
			found.push_back(std::move(f));
		}

		std::lock_guard<std::mutex> lock(S().mutex);
		S().friends = std::move(found);
	}

	// Who we are signed in as. Refreshed alongside the friends list rather than once at connect:
	// GetCurrentUserV2 returns nullopt until the SDK has the account, and that is not guaranteed to
	// have happened the instant Ready fires.
	void RefreshSelf()
	{
		std::shared_ptr<discordpp::Client> client;
		{
			std::lock_guard<std::mutex> lock(S().mutex);
			client = S().client;
		}
		if (!client)
			return;

		const auto user = client->GetCurrentUserV2();
		if (!user.has_value())
			return;

		std::string name = user->DisplayName();
		std::string avatar = user->AvatarUrl(discordpp::UserHandle::AvatarType::Webp,
			discordpp::UserHandle::AvatarType::Png);

		std::lock_guard<std::mutex> lock(S().mutex);
		S().self_name = std::move(name);
		S().self_avatar = std::move(avatar);
	}

	void StartPump()
	{
		if (S().pump.joinable())
			return;
		S().pump_quit.store(false, std::memory_order_release);
		S().pump = std::thread([] {
			// The SDK dispatches every callback from whoever calls RunCallbacks, so this thread is
			// the one all our callbacks above land on. 20 ms is the SDK sample's cadence; presence
			// is not latency-sensitive and this must never contend with the emu threads.
			DLOGI("pump thread started");
			uint64_t ticks = 0;
			while (!S().pump_quit.load(std::memory_order_acquire))
			{
				discordpp::RunCallbacks();
				// Every 5s. Silence here means callbacks can never be delivered, which makes every
				// other symptom downstream of it.
				if ((++ticks % 250) == 0)
					DLOGI("pump alive (%llu ticks)", (unsigned long long)ticks);
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			DLOGI("pump thread exiting");
		});
	}

	void StopPump()
	{
		S().pump_quit.store(true, std::memory_order_release);
		if (S().pump.joinable())
			S().pump.join();
	}

	void WireCallbacks(const std::shared_ptr<discordpp::Client>& client)
	{
		client->SetStatusChangedCallback([](discordpp::Client::Status status,
											 discordpp::Client::Error error, int32_t code) {
			DLOGI("sdk status=%d error=%d code=%d", static_cast<int>(status), static_cast<int>(error), code);
			if (status == discordpp::Client::Status::Ready)
			{
				SetStatus(BridgeStatus::Connected);
				// Both of these are only meaningful once Ready, and the presence in particular is
				// wiped by the connect, so this is the earliest correct moment for either.
				PushPresence();
				RefreshFriends();
				RefreshSelf();
			}
			else if (status == discordpp::Client::Status::Disconnected)
			{
				SetStatus(BridgeStatus::Disconnected);
				if (error != discordpp::Client::Error::None)
				{
					std::lock_guard<std::mutex> lock(S().mutex);
					S().error = discordpp::Client::ErrorToString(error) + " (" + std::to_string(code) + ")";
				}
			}
			else
			{
				SetStatus(BridgeStatus::Connecting);
			}
		});

		// Any change to who is online or what they are playing re-derives the list. The SDK gives
		// no finer-grained "this friend changed" for the group query, and the list is tiny.
		client->SetRelationshipCreatedCallback([](uint64_t, bool) { RefreshFriends(); });
		client->SetRelationshipDeletedCallback([](uint64_t, bool) { RefreshFriends(); });
			client->SetRelationshipGroupsUpdatedCallback([](uint64_t) { RefreshFriends(); });
	}

	void ConnectWithToken(const std::string& token)
	{
		std::shared_ptr<discordpp::Client> client;
		{
			std::lock_guard<std::mutex> lock(S().mutex);
			client = S().client;
		}
		if (!client || token.empty())
			return;

		SetStatus(BridgeStatus::Connecting);
		client->UpdateToken(discordpp::AuthorizationTokenType::Bearer, token,
			[client](discordpp::ClientResult result) {
				if (!result.Successful())
				{
					DLOGW("UpdateToken failed: %s", result.Error().c_str());
					{
						std::lock_guard<std::mutex> lock(S().mutex);
						S().error = result.Error();
					}
					SetStatus(BridgeStatus::Failed);
					return;
				}
				client->Connect();
			});
	}

	std::string JStr(JNIEnv* env, jstring s)
	{
		if (!s)
			return {};
		const char* c = env->GetStringUTFChars(s, nullptr);
		std::string out = c ? c : "";
		if (c)
			env->ReleaseStringUTFChars(s, c);
		return out;
	}
} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_armsx2_discord_DiscordNative_available(JNIEnv*, jclass)
{
	return JNI_TRUE;
}

/// Create the client and, when a saved token is passed, go straight to connecting. Idempotent.
JNIEXPORT void JNICALL
Java_com_armsx2_discord_DiscordNative_start(JNIEnv* env, jclass, jstring saved_token)
{
	const std::string token = JStr(env, saved_token);
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		if (!S().client)
		{
			S().client = std::make_shared<discordpp::Client>();
			S().client->SetApplicationId(kApplicationId);
			WireCallbacks(S().client);
			// The SDK's own diagnostics. Without this the only evidence of a failed handshake is
			// the UI sitting on "Connecting" forever, which is exactly where this landed.
			S().client->AddLogCallback([](std::string message, discordpp::LoggingSeverity severity) {
				DLOGI("[sdk sev=%d] %s", static_cast<int>(severity), message.c_str());
			}, discordpp::LoggingSeverity::Verbose);
			DLOGI("client created for application %llu", (unsigned long long)kApplicationId);
		}
	}
	if (!token.empty())
		ConnectWithToken(token);
	else
		SetStatus(BridgeStatus::Disconnected);
}

/// Full browser authorization. The SDK drives the handoff through its own AuthenticationActivity,
/// which is why that activity has to be declared in our manifest with the discord-<id> scheme.
JNIEXPORT void JNICALL
Java_com_armsx2_discord_DiscordNative_authorize(JNIEnv*, jclass)
{
	std::shared_ptr<discordpp::Client> client;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		client = S().client;
		S().error.clear();
	}
	if (!client)
		return;

	// Clear any authorize the SDK still thinks is running. One whose callback never arrived stays
	// in flight forever, and the SDK then ignores every subsequent Authorize() silently — no launch,
	// no error, the UI just sits on "Connecting". That is exactly what a retry looked like here:
	// the first attempt logged an activity launch, every one after it logged nothing at all.
	client->AbortAuthorize();

	SetStatus(BridgeStatus::Authorizing);
	DLOGI("Authorize() scopes='%s' redirect='%s'", kScopes, kRedirectUri);

	// PKCE. The verifier never leaves the device and the challenge is what goes to Discord, which
	// is what lets a public client finish the exchange with no client secret — there is no secret
	// in this app, and there must never be one.
	auto verifier = client->CreateAuthorizationCodeVerifier();

	discordpp::AuthorizationArgs args;
	args.SetClientId(kApplicationId);
	args.SetScopes(kScopes);
	args.SetCodeChallenge(verifier.Challenge());

	client->Authorize(args, [client, verifier = verifier.Verifier()](
							 discordpp::ClientResult result, std::string code, std::string /*redirect*/) {
		DLOGI("authorize callback: ok=%d code_len=%zu", result.Successful() ? 1 : 0, code.size());
		if (!result.Successful() || code.empty())
		{
			DLOGW("authorize failed: %s", result.Error().c_str());
			{
				std::lock_guard<std::mutex> lock(S().mutex);
				S().error = result.Successful() ? "No authorization code returned" : result.Error();
			}
			SetStatus(BridgeStatus::Failed);
			return;
		}

		client->GetToken(kApplicationId, code, verifier, kRedirectUri,
			[client](discordpp::ClientResult token_result, std::string token, std::string /*refresh*/,
				discordpp::AuthorizationTokenType, int32_t, std::string) {
				DLOGI("token callback: ok=%d token_len=%zu", token_result.Successful() ? 1 : 0, token.size());
				if (!token_result.Successful() || token.empty())
				{
					DLOGW("token exchange failed: %s", token_result.Error().c_str());
					{
						std::lock_guard<std::mutex> lock(S().mutex);
						S().error = token_result.Error();
					}
					SetStatus(BridgeStatus::Failed);
					return;
				}
				{
					std::lock_guard<std::mutex> lock(S().mutex);
					S().fresh_token = token;
				}
				ConnectWithToken(token);
			});
	});
}

/// Non-null exactly once per successful authorization, so Kotlin can persist it. Cleared on read.
JNIEXPORT jstring JNICALL
Java_com_armsx2_discord_DiscordNative_takeToken(JNIEnv* env, jclass)
{
	std::string token;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		token.swap(S().fresh_token);
	}
	return token.empty() ? nullptr : env->NewStringUTF(token.c_str());
}

JNIEXPORT jint JNICALL
Java_com_armsx2_discord_DiscordNative_status(JNIEnv*, jclass)
{
	return S().status.load(std::memory_order_acquire);
}

JNIEXPORT jstring JNICALL
Java_com_armsx2_discord_DiscordNative_error(JNIEnv* env, jclass)
{
	std::lock_guard<std::mutex> lock(S().mutex);
	return S().error.empty() ? nullptr : env->NewStringUTF(S().error.c_str());
}

/// Remember and publish what is being played. Empty title = back in the library.
JNIEXPORT void JNICALL
Java_com_armsx2_discord_DiscordNative_setPlaying(
	JNIEnv* env, jclass, jstring serial, jstring title, jstring cover)
{
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		S().want_serial = JStr(env, serial);
		S().want_title = JStr(env, title);
		S().want_cover = JStr(env, cover);
	}
	if (S().status.load(std::memory_order_acquire) == static_cast<int>(BridgeStatus::Connected))
		PushPresence();
}

/// The signed-in account as "name<FS>avatar". Empty until connected.
JNIEXPORT jstring JNICALL
Java_com_armsx2_discord_DiscordNative_self(JNIEnv* env, jclass)
{
	// Re-read on demand: the account can land after the first Ready, and this is a cheap
	// accessor the UI already polls.
	RefreshSelf();
	std::string joined;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		if (!S().self_name.empty())
		{
			joined = S().self_name;
			joined.push_back(kFieldSep);
			joined += S().self_avatar;
		}
	}
	return env->NewStringUTF(joined.c_str());
}

/// Friends currently in ARMSX2, newline-separated. Empty string is a legitimate answer and means
/// nobody is on — distinct from not-connected, which the caller reads from discordStatus().
JNIEXPORT jstring JNICALL
Java_com_armsx2_discord_DiscordNative_friends(JNIEnv* env, jclass)
{
	std::string joined;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		for (const auto& f : S().friends)
		{
			if (!joined.empty())
				joined.push_back(kRecordSep);
			joined += f.name;
			joined.push_back(kFieldSep);
			joined += f.game;
			joined.push_back(kFieldSep);
			joined += f.serial;
			joined.push_back(kFieldSep);
			joined += f.avatar;
		}
	}
	return env->NewStringUTF(joined.c_str());
}

/// Put a line on the in-game OSD. Used for "someone started playing", which has to be visible
/// without leaving the game — the whole point of an in-game notification.
///
JNIEXPORT void JNICALL
Java_com_armsx2_discord_DiscordNative_pump(JNIEnv*, jclass)
{
	PumpSdkCallbacks();
}

/// Sign out. Drops the client entirely so no stale presence survives, and stops the pump thread —
/// a disabled feature should cost nothing, not a thread waking 50 times a second forever.
JNIEXPORT void JNICALL
Java_com_armsx2_discord_DiscordNative_stop(JNIEnv*, jclass)
{
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		S().client.reset();
		S().friends.clear();
		S().fresh_token.clear();
		S().error.clear();
	}
	SetStatus(BridgeStatus::Disconnected);
}

} // extern "C"

#else // !ARMSX2_HAS_DISCORD

// SDK not staged. Every entry point still exists so the Kotlin side is identical either way; it
// simply reports Disabled and does nothing.
extern "C" {
JNIEXPORT jboolean JNICALL Java_com_armsx2_discord_DiscordNative_available(JNIEnv*, jclass) { return JNI_FALSE; }
JNIEXPORT void JNICALL Java_com_armsx2_discord_DiscordNative_start(JNIEnv*, jclass, jstring) {}
JNIEXPORT void JNICALL Java_com_armsx2_discord_DiscordNative_authorize(JNIEnv*, jclass) {}
JNIEXPORT jstring JNICALL Java_com_armsx2_discord_DiscordNative_takeToken(JNIEnv*, jclass) { return nullptr; }
JNIEXPORT jint JNICALL Java_com_armsx2_discord_DiscordNative_status(JNIEnv*, jclass) { return 0; }
JNIEXPORT jstring JNICALL Java_com_armsx2_discord_DiscordNative_error(JNIEnv*, jclass) { return nullptr; }
JNIEXPORT void JNICALL Java_com_armsx2_discord_DiscordNative_setPlaying(JNIEnv*, jclass, jstring, jstring, jstring) {}
JNIEXPORT jstring JNICALL Java_com_armsx2_discord_DiscordNative_friends(JNIEnv* env, jclass) { return env->NewStringUTF(""); }
JNIEXPORT jstring JNICALL Java_com_armsx2_discord_DiscordNative_self(JNIEnv* env, jclass) { return env->NewStringUTF(""); }
JNIEXPORT void JNICALL Java_com_armsx2_discord_DiscordNative_pump(JNIEnv*, jclass) {}
JNIEXPORT void JNICALL Java_com_armsx2_discord_DiscordNative_stop(JNIEnv*, jclass) {}
}

#endif // ARMSX2_HAS_DISCORD
