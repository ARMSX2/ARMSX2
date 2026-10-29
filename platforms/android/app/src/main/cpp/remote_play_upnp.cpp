// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Remote play, online hosting: a port opened on the host's router (UPnP) for the session.
//
// Online, the friend reaches the host directly, with no relay. When the friend's network lets nothing
// in (a phone on mobile data) and the host's router only admits what the host sent out first, the two
// never connect. A router that speaks UPnP can be asked to forward one port to the host, which gives
// the host a fixed public address the friend's side can always reach. The connection stays direct: the
// router only forwards to the host.
//
// The app (RemotePlayUpnp.kt) asks for the UDP port WebRTC already listens on, adds the router's
// public address for it to the offer, renews the lease while the session lasts and closes the port
// when it ends. Only leased mappings are used: the lease lets a port left open by a crash close
// itself, whereas a permanent forward would never be closed.

#include <jni.h>
#include <android/log.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#define RP_UPNP_LOG(...) __android_log_print(ANDROID_LOG_INFO, "RemotePlay", __VA_ARGS__)

namespace
{
	std::mutex s_mutex;
	bool s_found = false;
	UPNPUrls s_urls{};
	IGDdatas s_data{};
	char s_lan_addr[64]{};
	char s_wan_addr[64]{};

	// UPnP error codes from the WANIPConnection spec.
	constexpr int CONFLICT_IN_MAPPING_ENTRY = 718;
	constexpr int ONLY_PERMANENT_LEASES_SUPPORTED = 725;

	// Under s_mutex.
	void ForgetRouter()
	{
		if (s_found)
			FreeUPNPUrls(&s_urls);
		s_found = false;
		s_urls = {};
		s_data = {};
	}

	// Under s_mutex. The router found before when it still answers (the device may have moved to
	// another network since), otherwise a new search, which takes about two seconds.
	bool FindRouter()
	{
		if (s_found)
		{
			char wan[64]{};
			if (UPNP_GetExternalIPAddress(s_urls.controlURL, s_data.first.servicetype, wan) == UPNPCOMMAND_SUCCESS && wan[0])
			{
				std::snprintf(s_wan_addr, sizeof(s_wan_addr), "%s", wan);
				return true;
			}
			ForgetRouter();
		}

		int error = 0;
		UPNPDev* const devices = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &error);
		if (!devices)
		{
			RP_UPNP_LOG("remote play: no UPnP router answered (%d)", error);
			return false;
		}

		const int igd = UPNP_GetValidIGD(devices, &s_urls, &s_data, s_lan_addr, sizeof(s_lan_addr), s_wan_addr, sizeof(s_wan_addr));
		freeUPNPDevlist(devices);

		if (igd == UPNP_CONNECTED_IGD)
		{
			s_found = true;
			RP_UPNP_LOG("remote play: UPnP router found, this device %s, public %s", s_lan_addr, s_wan_addr);
			return true;
		}

		if (igd == UPNP_PRIVATEIP_IGD)
		{
			// Behind another NAT (the provider's, say): a port opened here leads nowhere.
			RP_UPNP_LOG("remote play: the UPnP router's own address %s is private, not using it", s_wan_addr);
		}
		else
		{
			RP_UPNP_LOG("remote play: no usable UPnP router (%d)", igd);
		}

		if (igd != 0)
			FreeUPNPUrls(&s_urls);
		s_urls = {};
		s_data = {};
		return false;
	}

	// Under s_mutex.
	int AddMapping(const std::string& external, const std::string& internal, const std::string& lease)
	{
		return UPNP_AddPortMapping(s_urls.controlURL, s_data.first.servicetype, external.c_str(), internal.c_str(),
			s_lan_addr, "ARMSX2 remote play", "UDP", nullptr, lease.c_str());
	}
} // namespace

// This device's address on the router's network, or null when there is no router to open a port on
// (or it has no public address of its own). Blocks for about two seconds the first time: call it off
// the main thread.
extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayUpnpPrepare(JNIEnv* env, jclass)
{
	std::lock_guard lock(s_mutex);
	if (!FindRouter() || !s_lan_addr[0])
		return nullptr;
	return env->NewStringUTF(s_lan_addr);
}

// Opens UDP internalPort of this device on the router: the same port, or when another device holds it
// the next free one up to 32 above, for leaseSeconds. Asked again for the same ports it renews the
// lease. Returns "public address:router port", or null when the router refused. externalHint: the
// router port this one had before, to renew; 0 the first time.
extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayUpnpMap(JNIEnv* env, jclass, jint internalPort, jint externalHint, jint leaseSeconds)
{
	std::lock_guard lock(s_mutex);
	if (!s_found || internalPort <= 0 || internalPort > 0xffff)
		return nullptr;

	const std::string internal = std::to_string(internalPort);
	const std::string lease = std::to_string(leaseSeconds);
	const int first = externalHint > 0 ? externalHint : internalPort;
	// A renewal asks for the port it had and no other. That port is the one in the friend's offer,
	// and a mapping moved anywhere else would only leave the old one open behind it.
	const int last = externalHint > 0 ? first : std::min(first + 32, 0xffff);

	for (int external_port = first; external_port <= last; external_port++)
	{
		const std::string external = std::to_string(external_port);
		const int result = AddMapping(external, internal, lease);

		if (result == ONLY_PERMANENT_LEASES_SUPPORTED)
		{
			// Not taken: a permanent forward stays open on the router for good if the app is killed
			// before the session ends. The offer goes out as it was, as with no router at all.
			RP_UPNP_LOG("remote play: the router only opens ports permanently, so it is not used");
			return nullptr;
		}

		if (result == UPNPCOMMAND_SUCCESS)
		{
			RP_UPNP_LOG("remote play: router port %d open to %s:%d (lease %s s)", external_port, s_lan_addr, internalPort, lease.c_str());
			if (!s_wan_addr[0])
				return nullptr;
			const std::string address = std::string(s_wan_addr) + ":" + std::to_string(external_port);
			return env->NewStringUTF(address.c_str());
		}

		if (result != CONFLICT_IN_MAPPING_ENTRY)
		{
			RP_UPNP_LOG("remote play: the router would not open port %d: %d (%s)", external_port, result, strupnperror(result));
			return nullptr;
		}
	}

	RP_UPNP_LOG("remote play: no free router port near %d", first);
	return nullptr;
}

// Closes a router port opened by remotePlayUpnpMap.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayUpnpUnmap(JNIEnv*, jclass, jint externalPort)
{
	std::lock_guard lock(s_mutex);
	if (!s_found || externalPort <= 0)
		return;

	const std::string external = std::to_string(externalPort);
	const int result = UPNP_DeletePortMapping(s_urls.controlURL, s_data.first.servicetype, external.c_str(), "UDP", nullptr);
	RP_UPNP_LOG("remote play: router port %d closed (%d)", externalPort, result);
}
