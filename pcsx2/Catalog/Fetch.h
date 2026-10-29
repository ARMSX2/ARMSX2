// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The HTTP layer every Catalog service goes through (texture-pack catalog and installer, covers,
// driver packs, online patches, ...). Services take a `Fetcher&`, so tests hand them a
// FakeFetcher (tests/ctest/core/catalog/FakeFetcher.h) and nothing touches the network.
//
// Why this is not common/HTTPDownloader: that class is poll-based and keeps the whole response
// in memory, while texture packs reach 1.5 GB and must stream to disk. A write sink there would
// mean changing its base class and all three backends (curl, WinHTTP, the Android Java bridge).
// Instead the implementation here (HttpFetcher.h) owns the policy (https-only redirects, byte
// caps, file streaming, progress, cancellation) in portable code and talks to libcurl's easy
// interface (WinHTTP on Windows) through a thin HttpTransport, so no upstream file changes and
// the policy is testable with a scripted transport.
//
// All calls block and are safe to make concurrently from different threads; services run them
// on worker threads. Nothing here takes a lock the UI or emulation threads can contend on.

#include "Catalog/CancelToken.h"

#include "common/Pcsx2Types.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Catalog
{
	/// How many redirects are followed before the request is refused. Only https targets are
	/// followed, including the first URL.
	static constexpr int MAX_REDIRECTS = 5;

	enum class FetchResult
	{
		Ok, ///< A complete HTTP response arrived. Look at FetchResponse::status; 404, 403 and 429 land here.
		Cancelled, ///< The CancelToken fired.
		TooLarge, ///< The body exceeded the byte cap (Get) or max_file_bytes (GetToFile).
		BadUrl, ///< The request URL is not https.
		BadRedirect, ///< Redirect to a non-https URL, an unusable Location, or more than MAX_REDIRECTS.
		FileError, ///< GetToFile could not create or write the destination file.
		Transport, ///< DNS, TLS, connection, timeout, or a body cut short of its Content-Length.
	};

	struct FetchResponse
	{
		FetchResult result = FetchResult::Ok;
		s32 status = 0; ///< HTTP status of the last response, 0 if none arrived.
		std::string final_url; ///< The URL that produced `status`, after redirects.
		std::string content_type; ///< Content-Type header of that response, as sent.
		std::string body; ///< Get/Post only. Present for non-2xx statuses too (e.g. GitHub's rate-limit JSON).
		std::string error; ///< Human-readable cause when `result` is not Ok.
		u64 bytes_received = 0; ///< Body bytes received (Get/Post) or written to the file (GetToFile).

		bool IsSuccess() const { return result == FetchResult::Ok && status >= 200 && status < 300; }
	};

	using Header = std::pair<std::string, std::string>;

	struct FetchOptions
	{
		CancelToken cancel;

		/// Extra request headers, e.g. {"Accept", "application/json"}.
		std::vector<Header> headers;

		/// Empty means "ARMSX2/<version>".
		std::string user_agent;

		u32 connect_timeout_seconds = 15;

		/// The request is abandoned when no body bytes arrive for this long.
		u32 idle_timeout_seconds = 30;

		/// Called from the fetching thread as the body arrives, at most about every 100 ms, and
		/// once more when it ends. `total` is 0 when the server did not say. Counts bytes of this
		/// call only: when appending parts to one file, add the earlier parts yourself.
		std::function<void(u64 done, u64 total)> on_progress;

		// GetToFile only:

		/// Open the file for append instead of truncating it (several parts into one archive).
		bool append = false;

		/// Fail with TooLarge once more than this many bytes were written by this call; 0 = no limit.
		u64 max_file_bytes = 0;

		/// Called with every chunk right after it was written, in order. Feed SHA256Digest here to
		/// hash while downloading instead of re-reading a 1.5 GB file.
		std::function<void(const u8* data, size_t size)> on_data;
	};

	class Fetcher
	{
	public:
		virtual ~Fetcher() = default;

		/// GET into memory. A body larger than `max_body_bytes` (judged from Content-Length up
		/// front, and again as bytes arrive) fails with TooLarge. Catalog JSON uses 8 MiB, the
		/// creator-links JSON 2 MiB.
		virtual FetchResponse Get(const std::string& url, size_t max_body_bytes, const FetchOptions& options = {}) = 0;

		/// GET streamed into `path`; the body is never held in memory. The file is created (or
		/// appended to) only once a 2xx response starts, so a 404 leaves an existing file alone.
		/// On failure or cancellation the bytes written so far stay on disk: the caller owns the
		/// cleanup of its staging directory. The parent directory must already exist.
		virtual FetchResponse GetToFile(const std::string& url, const std::string& path, const FetchOptions& options = {}) = 0;

		/// POST a small body (the popularity ping). Redirects are not followed: a 3xx comes back
		/// as the response. The reply body is kept up to 64 KiB.
		virtual FetchResponse Post(const std::string& url, std::string_view body, std::string_view content_type, const FetchOptions& options = {}) = 0;
	};

	/// The process-wide fetcher over the platform HTTP stack. On platforms without one (Android
	/// and iOS for now) every call fails with FetchResult::Transport.
	Fetcher& GetDefaultFetcher();

	/// "ARMSX2/<version>".
	std::string DefaultUserAgent();

	/// True for "https://<non-empty host>..." (scheme matched case-insensitively), the only kind
	/// of URL this layer will request or follow.
	bool IsHttpsUrl(std::string_view url);
} // namespace Catalog
