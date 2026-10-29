// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// libcurl transport for HttpFetcher (Linux, macOS). One easy handle per request, nothing shared
// between calls, so concurrent Perform() calls from different threads are independent. Redirects
// are left to HttpFetcher: this only performs a single exchange.

#include "Catalog/HttpFetcher.h"

#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <pthread.h>

#include <curl/curl.h>

namespace Catalog
{
	namespace
	{
		bool EnsureCurlInitialised()
		{
			// HTTPDownloaderCurl initialises libcurl too. curl_global_init() is reference counted
			// and thread safe, and this one is simply never balanced: the process ends first.
			static std::once_flag once;
			static bool ok = false;
			std::call_once(once, []() { ok = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK); });
			return ok;
		}

		// OpenSSL can raise SIGPIPE on a connection the server dropped; keep it off this thread
		// for the duration of the transfer, as HTTPDownloaderCurl does.
		class ScopedSigpipeBlock
		{
		public:
			ScopedSigpipeBlock()
			{
				sigset_t block;
				sigemptyset(&block);
				sigaddset(&block, SIGPIPE);
				m_blocked = (pthread_sigmask(SIG_BLOCK, &block, &m_old) == 0);
			}
			~ScopedSigpipeBlock()
			{
				if (m_blocked)
					pthread_sigmask(SIG_SETMASK, &m_old, nullptr);
			}

		private:
			sigset_t m_old;
			bool m_blocked = false;
		};

		struct CurlContext
		{
			HttpSink* sink = nullptr;
			HttpHead head;
			bool head_sent = false;
			bool sink_stopped = false; // the sink said stop; the transfer's error is expected
		};

		bool HeaderNameIs(std::string_view line, std::string_view name)
		{
			return line.size() > name.size() && line[name.size()] == ':' &&
			       std::equal(name.begin(), name.end(), line.begin(), [](char a, char b) {
					   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
				   });
		}

		std::string_view HeaderValue(std::string_view line)
		{
			line.remove_prefix(line.find(':') + 1);
			while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
				line.remove_prefix(1);
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
				line.remove_suffix(1);
			return line;
		}

		size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata)
		{
			CurlContext& ctx = *static_cast<CurlContext*>(userdata);
			const size_t length = size * nitems;
			const std::string_view line(buffer, length);

			if (line.starts_with("HTTP/"))
			{
				// A new status line: start over. Interim (1xx) responses and the final one each
				// send their own block of headers.
				ctx.head = HttpHead();
				const size_t space = line.find(' ');
				if (space != std::string_view::npos)
					ctx.head.status = std::atoi(std::string(line.substr(space + 1, 3)).c_str());
			}
			else if (HeaderNameIs(line, "content-type"))
			{
				ctx.head.content_type = std::string(HeaderValue(line));
			}
			else if (HeaderNameIs(line, "location"))
			{
				ctx.head.location = std::string(HeaderValue(line));
			}
			else if (HeaderNameIs(line, "content-length"))
			{
				ctx.head.content_length = std::strtoll(std::string(HeaderValue(line)).c_str(), nullptr, 10);
			}
			else if ((line == "\r\n" || line == "\n") && ctx.head.status >= 200 && !ctx.head_sent)
			{
				ctx.head_sent = true;
				if (!ctx.sink->OnHead(ctx.head))
				{
					ctx.sink_stopped = true;
					return 0; // makes curl fail the transfer with CURLE_WRITE_ERROR
				}
			}
			return length;
		}

		size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
		{
			CurlContext& ctx = *static_cast<CurlContext*>(userdata);
			const size_t length = size * nmemb;
			if (!ctx.sink->OnBody(reinterpret_cast<const u8*>(ptr), length))
			{
				ctx.sink_stopped = true;
				return 0;
			}
			return length;
		}

		// Curl calls this at least once a second, even while the connection is stalled, which is
		// what bounds how long a cancel takes to be noticed.
		int XferInfoCallback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
		{
			CurlContext& ctx = *static_cast<CurlContext*>(userdata);
			return ctx.sink->ShouldAbort() ? 1 : 0;
		}

		class CurlTransport final : public HttpTransport
		{
		public:
			std::string Perform(const HttpExchange& exchange, HttpSink& sink) override
			{
				if (!EnsureCurlInitialised())
					return "curl_global_init() failed";

				CURL* const handle = curl_easy_init();
				if (!handle)
					return "curl_easy_init() failed";

				CurlContext ctx;
				ctx.sink = &sink;

				char error_buffer[CURL_ERROR_SIZE] = {};
				curl_slist* headers = nullptr;
				for (const Header& header : exchange.headers)
					headers = curl_slist_append(headers, fmt::format("{}: {}", header.first, header.second).c_str());
				if (exchange.is_post && !exchange.post_content_type.empty())
					headers = curl_slist_append(headers, fmt::format("Content-Type: {}", exchange.post_content_type).c_str());

				curl_easy_setopt(handle, CURLOPT_URL, exchange.url.c_str());
				curl_easy_setopt(handle, CURLOPT_USERAGENT, exchange.user_agent.c_str());
				curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
				curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer);
				curl_easy_setopt(handle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);

				// HttpFetcher follows redirects itself; whatever curl is given, it speaks https only.
				curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500 // 7.85.0
				curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
#else
				curl_easy_setopt(handle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif

				curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(exchange.connect_timeout_seconds));
				// Give up when fewer than 1 byte/s flows for idle_timeout_seconds.
				curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
				curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, static_cast<long>(exchange.idle_timeout_seconds));
				curl_easy_setopt(handle, CURLOPT_BUFFERSIZE, 131072L);

				curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &HeaderCallback);
				curl_easy_setopt(handle, CURLOPT_HEADERDATA, &ctx);
				curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &WriteCallback);
				curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
				curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
				curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &XferInfoCallback);
				curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &ctx);

				// Ask for compression unless the caller chose an encoding (a texture-pack part asks for
				// identity so its bytes match the published hash). curl decodes what comes back, so the
				// sink always sees the original bytes; GitHub's tree lists shrink about tenfold.
				bool caller_chose_encoding = false;
				for (const Header& header : exchange.headers)
					caller_chose_encoding |= StringUtil::compareNoCase(header.first, "Accept-Encoding");
				if (!caller_chose_encoding)
					curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");

				if (headers)
					curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
				if (exchange.is_post)
				{
					// curl turns a POST with a null body pointer back into a GET.
					curl_easy_setopt(handle, CURLOPT_POST, 1L);
					curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(exchange.post_body.size()));
					curl_easy_setopt(handle, CURLOPT_POSTFIELDS, exchange.post_body.empty() ? "" : exchange.post_body.data());
				}

				CURLcode rc;
				{
					ScopedSigpipeBlock block_sigpipe;
					rc = curl_easy_perform(handle);
				}

				// A response with no body never reaches the write callback, and curl may not
				// have delivered the blank line that ends its headers either way: make sure the
				// sink hears about it exactly once.
				if (rc == CURLE_OK && !ctx.head_sent && ctx.head.status >= 200)
				{
					ctx.head_sent = true;
					sink.OnHead(ctx.head);
				}

				std::string error;
				if (rc != CURLE_OK && !ctx.sink_stopped)
				{
					error = error_buffer[0] ? std::string(error_buffer) : std::string(curl_easy_strerror(rc));
					if (rc == CURLE_ABORTED_BY_CALLBACK)
						error = "Aborted.";
				}

				curl_slist_free_all(headers);
				curl_easy_cleanup(handle);
				return error;
			}
		};
	} // namespace

	std::unique_ptr<HttpTransport> CreateDefaultHttpTransport()
	{
		return std::make_unique<CurlTransport>();
	}
} // namespace Catalog
