// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Stand-in transport for platforms where Catalog has no HTTP stack yet (Android, which has a
// Java HTTP bridge, and iOS, which has no libcurl). Every request fails with a clear message,
// so Catalog code builds and links everywhere and the services simply report "unreachable".

#include "Catalog/HttpFetcher.h"

namespace Catalog
{
	namespace
	{
		class UnavailableTransport final : public HttpTransport
		{
		public:
			std::string Perform(const HttpExchange&, HttpSink&) override
			{
				return "Online services are not available on this platform.";
			}
		};
	} // namespace

	std::unique_ptr<HttpTransport> CreateDefaultHttpTransport()
	{
		return std::make_unique<UnavailableTransport>();
	}
} // namespace Catalog
