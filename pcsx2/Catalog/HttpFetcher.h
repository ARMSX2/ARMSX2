// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The real Fetcher (Fetch.h) and the one-request transport underneath it. A service never
// includes this header; it is for GetDefaultFetcher(), the platform transports, and tests that
// want to drive the real redirect/cap/file logic with a scripted transport (FakeFetcher does).

#include "Catalog/Fetch.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Catalog
{
	/// What a transport reports about a response before any of its body is delivered.
	struct HttpHead
	{
		s32 status = 0;
		std::string content_type;
		std::string location; ///< Raw Location header, possibly relative. Empty if absent.
		s64 content_length = -1; ///< -1 when unknown.
	};

	/// One request, exactly as the transport should send it. Redirects are never followed by the
	/// transport: HttpFetcher decides, hop by hop.
	struct HttpExchange
	{
		std::string url;
		std::vector<Header> headers;
		std::string user_agent;
		u32 connect_timeout_seconds = 15;
		u32 idle_timeout_seconds = 30;

		bool is_post = false;
		std::string_view post_body; ///< Valid for the duration of Perform().
		std::string_view post_content_type;
	};

	/// Receives a response as it arrives. The transport calls these on the thread that called
	/// Perform().
	class HttpSink
	{
	public:
		virtual ~HttpSink() = default;

		/// Once, after the status line and headers and before any body bytes (also for bodiless
		/// responses). Return false to skip the body: the transport abandons the transfer.
		virtual bool OnHead(const HttpHead& head) = 0;

		/// Body bytes, in order. Return false to abort the transfer.
		virtual bool OnBody(const u8* data, size_t size) = 0;

		/// Polled at least once a second while a transfer runs, including while it is stalled.
		/// Return true to abort the transfer.
		virtual bool ShouldAbort() = 0;
	};

	class HttpTransport
	{
	public:
		virtual ~HttpTransport() = default;

		/// Runs one request and feeds the response to `sink`. Returns an empty string when the
		/// exchange ended normally, or when the sink stopped it; otherwise a description of what
		/// went wrong (connection failure, TLS error, timeout, body shorter than its
		/// Content-Length). Must be callable from several threads at once.
		virtual std::string Perform(const HttpExchange& exchange, HttpSink& sink) = 0;
	};

	/// The transport for this platform: libcurl on Linux and macOS, WinHTTP on Windows, and on
	/// platforms with neither a transport that fails every request with an explanatory message.
	/// Defined by exactly one of HttpTransportCurl/WinHTTP/None.cpp, chosen in CMake.
	std::unique_ptr<HttpTransport> CreateDefaultHttpTransport();

	class HttpFetcher final : public Fetcher
	{
	public:
		explicit HttpFetcher(std::shared_ptr<HttpTransport> transport);

		FetchResponse Get(const std::string& url, size_t max_body_bytes, const FetchOptions& options = {}) override;
		FetchResponse GetToFile(const std::string& url, const std::string& path, const FetchOptions& options = {}) override;
		FetchResponse Post(const std::string& url, std::string_view body, std::string_view content_type, const FetchOptions& options = {}) override;

	private:
		std::shared_ptr<HttpTransport> m_transport;
	};

	/// Resolves a Location header against the URL that produced it: absolute URLs, "//host/path",
	/// "/path", "?query" and "relative/path" are handled (dot segments are not collapsed).
	/// Returns an empty string if `base` is not an http(s) URL or `location` is empty.
	std::string ResolveRedirectLocation(std::string_view base, std::string_view location);
} // namespace Catalog
