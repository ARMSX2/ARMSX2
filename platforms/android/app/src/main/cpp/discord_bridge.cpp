// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Discord Social SDK bridge: rich presence, and which friends are in ARMSX2 right now.
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
		std::string game; // what they are playing IN ARMSX2, empty when unknown
	};

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

		// Last presence asked for, replayed after a (re)connect. Presence set before Connect is
		// cleared by the connect itself, which the header calls out explicitly, so the only safe
		// way to keep it is to remember it and set it again once connected.
		std::string want_serial;
		std::string want_title;
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

	void SetStatus(BridgeStatus s) { S().status.store(static_cast<int>(s), std::memory_order_release); }

	// Push the remembered game to Discord. Caller must NOT hold the mutex: UpdateRichPresence can
	// invoke its callback inline.
	void PushPresence()
	{
		std::shared_ptr<discordpp::Client> client;
		std::string serial, title;
		{
			std::lock_guard<std::mutex> lock(S().mutex);
			client = S().client;
			serial = S().want_serial;
			title = S().want_title;
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
			if (!serial.empty())
				activity.SetState(serial);
		}
		else
		{
			activity.SetDetails(std::string("In the library"));
		}

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
			Friend f;
			const auto user = rel.User();
			if (user.has_value())
				f.name = user->DisplayName();
			if (f.name.empty())
				continue;
			found.push_back(std::move(f));
		}

		std::lock_guard<std::mutex> lock(S().mutex);
		S().friends = std::move(found);
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
			while (!S().pump_quit.load(std::memory_order_acquire))
			{
				discordpp::RunCallbacks();
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
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
			if (status == discordpp::Client::Status::Ready)
			{
				SetStatus(BridgeStatus::Connected);
				// Both of these are only meaningful once Ready, and the presence in particular is
				// wiped by the connect, so this is the earliest correct moment for either.
				PushPresence();
				RefreshFriends();
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
Java_kr_co_iefriends_pcsx2_NativeApp_discordAvailable(JNIEnv*, jclass)
{
	return JNI_TRUE;
}

/// Create the client and, when a saved token is passed, go straight to connecting. Idempotent.
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_discordStart(JNIEnv* env, jclass, jstring saved_token)
{
	const std::string token = JStr(env, saved_token);
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		if (!S().client)
		{
			S().client = std::make_shared<discordpp::Client>();
			S().client->SetApplicationId(kApplicationId);
			WireCallbacks(S().client);
			DLOGI("client created for application %llu", (unsigned long long)kApplicationId);
		}
	}
	StartPump();
	if (!token.empty())
		ConnectWithToken(token);
	else
		SetStatus(BridgeStatus::Disconnected);
}

/// Full browser authorization. The SDK drives the handoff through its own AuthenticationActivity,
/// which is why that activity has to be declared in our manifest with the discord-<id> scheme.
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_discordAuthorize(JNIEnv*, jclass)
{
	std::shared_ptr<discordpp::Client> client;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		client = S().client;
		S().error.clear();
	}
	if (!client)
		return;

	SetStatus(BridgeStatus::Authorizing);

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
Java_kr_co_iefriends_pcsx2_NativeApp_discordTakeToken(JNIEnv* env, jclass)
{
	std::string token;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		token.swap(S().fresh_token);
	}
	return token.empty() ? nullptr : env->NewStringUTF(token.c_str());
}

JNIEXPORT jint JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_discordStatus(JNIEnv*, jclass)
{
	return S().status.load(std::memory_order_acquire);
}

JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_discordError(JNIEnv* env, jclass)
{
	std::lock_guard<std::mutex> lock(S().mutex);
	return S().error.empty() ? nullptr : env->NewStringUTF(S().error.c_str());
}

/// Remember and publish what is being played. Empty title = back in the library.
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_discordSetPlaying(JNIEnv* env, jclass, jstring serial, jstring title)
{
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		S().want_serial = JStr(env, serial);
		S().want_title = JStr(env, title);
	}
	if (S().status.load(std::memory_order_acquire) == static_cast<int>(BridgeStatus::Connected))
		PushPresence();
}

/// Friends currently in ARMSX2, newline-separated. Empty string is a legitimate answer and means
/// nobody is on — distinct from not-connected, which the caller reads from discordStatus().
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_discordFriends(JNIEnv* env, jclass)
{
	std::string joined;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		for (const auto& f : S().friends)
		{
			if (!joined.empty())
				joined.push_back('\n');
			joined += f.name;
		}
	}
	return env->NewStringUTF(joined.c_str());
}

/// Sign out. Drops the client entirely so no stale presence survives, and stops the pump thread —
/// a disabled feature should cost nothing, not a thread waking 50 times a second forever.
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_discordStop(JNIEnv*, jclass)
{
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		S().client.reset();
		S().friends.clear();
		S().fresh_token.clear();
		S().error.clear();
	}
	StopPump();
	SetStatus(BridgeStatus::Disconnected);
}

} // extern "C"

#else // !ARMSX2_HAS_DISCORD

// SDK not staged. Every entry point still exists so the Kotlin side is identical either way; it
// simply reports Disabled and does nothing.
extern "C" {
JNIEXPORT jboolean JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_discordAvailable(JNIEnv*, jclass) { return JNI_FALSE; }
JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_discordStart(JNIEnv*, jclass, jstring) {}
JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_discordAuthorize(JNIEnv*, jclass) {}
JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_discordTakeToken(JNIEnv*, jclass) { return nullptr; }
JNIEXPORT jint JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_discordStatus(JNIEnv*, jclass) { return 0; }
JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_discordError(JNIEnv*, jclass) { return nullptr; }
JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_discordSetPlaying(JNIEnv*, jclass, jstring, jstring) {}
JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_discordFriends(JNIEnv* env, jclass) { return env->NewStringUTF(""); }
JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_discordStop(JNIEnv*, jclass) {}
}

#endif // ARMSX2_HAS_DISCORD
