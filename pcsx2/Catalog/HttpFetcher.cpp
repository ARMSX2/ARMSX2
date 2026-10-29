// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/HttpFetcher.h"

#include "BuildVersion.h"

#include "common/Error.h"
#include "common/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

#include "fmt/format.h"

namespace Catalog
{
	static bool EqualsNoCase(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() &&
		       std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
				   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			   });
	}

	bool IsHttpsUrl(std::string_view url)
	{
		constexpr std::string_view prefix = "https://";
		if (url.size() <= prefix.size() || !EqualsNoCase(url.substr(0, prefix.size()), prefix))
			return false;

		// A host has to follow the scheme.
		switch (url[prefix.size()])
		{
			case '/':
			case '?':
			case '#':
			case ':':
			case '@':
				return false;
		}

		// Spaces and control characters never belong in a request URL.
		return std::none_of(url.begin(), url.end(), [](char c) {
			return static_cast<unsigned char>(c) <= ' ' || c == 0x7f;
		});
	}

	// "scheme:" at the start of a reference (RFC 3986 section 3.1), as opposed to a path.
	static bool HasScheme(std::string_view ref)
	{
		if (ref.empty() || !std::isalpha(static_cast<unsigned char>(ref[0])))
			return false;
		for (size_t i = 1; i < ref.size(); i++)
		{
			const char c = ref[i];
			if (c == ':')
				return true;
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.')
				return false;
		}
		return false;
	}

	std::string ResolveRedirectLocation(std::string_view base, std::string_view location)
	{
		if (location.empty())
			return {};

		const size_t scheme_end = base.find("://");
		if (scheme_end == std::string_view::npos)
			return {};
		const std::string_view scheme = base.substr(0, scheme_end);
		if (!EqualsNoCase(scheme, "https") && !EqualsNoCase(scheme, "http"))
			return {};

		if (HasScheme(location))
			return std::string(location);
		if (location.starts_with("//"))
			return fmt::format("{}:{}", scheme, location);

		const size_t authority_start = scheme_end + 3;
		size_t authority_end = base.find_first_of("/?#", authority_start);
		if (authority_end == std::string_view::npos)
			authority_end = base.size();
		const std::string_view origin = base.substr(0, authority_end);

		if (location[0] == '/')
			return fmt::format("{}{}", origin, location);

		// Everything left is relative to the base's path, query or whole URL (minus fragment).
		std::string_view rest = base.substr(authority_end);
		rest = rest.substr(0, rest.find('#'));
		if (location[0] == '#')
			return fmt::format("{}{}{}", origin, rest, location);

		const std::string_view path = rest.substr(0, rest.find('?'));
		if (location[0] == '?')
			return fmt::format("{}{}{}", origin, path, location);

		const size_t last_slash = path.rfind('/');
		const std::string_view directory = (last_slash == std::string_view::npos) ? std::string_view("/") : path.substr(0, last_slash + 1);
		return fmt::format("{}{}{}", origin, directory, location);
	}

	std::string DefaultUserAgent()
	{
		return fmt::format("ARMSX2/{}", (BuildVersion::GitTag && BuildVersion::GitTag[0]) ? BuildVersion::GitTag : "unknown");
	}

	namespace
	{
		enum class Kind
		{
			Memory,
			File,
			Post,
		};

		struct Job
		{
			Kind kind = Kind::Memory;
			size_t max_body_bytes = 0; // Memory, Post
			std::string file_path; // File
			std::string_view post_body;
			std::string_view post_content_type;
		};

		// The reply body of a POST is kept only so a service can read an error message.
		constexpr size_t POST_REPLY_LIMIT = 64 * 1024;

		bool IsRedirectStatus(s32 status)
		{
			return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
		}

		// Turns what a transport reports into a FetchResponse: decides redirects, enforces the
		// caps, owns the output file, reports progress. It remembers why it stopped a transfer so
		// the caller can tell its own stops from transport failures.
		class FetchSink final : public HttpSink
		{
		public:
			enum class Stop
			{
				None,
				Redirect,
				Skipped, // non-2xx on a file download: not worth downloading the error page
				Cancelled,
				TooLarge,
				FileError,
			};

			FetchSink(const Job& job, const FetchOptions& options, FetchResponse& response, bool follow_redirects)
				: m_job(job)
				, m_options(options)
				, m_response(response)
				, m_follow_redirects(follow_redirects)
			{
			}

			~FetchSink() override { CloseFile(nullptr); }

			Stop GetStop() const { return m_stop; }
			bool GotHead() const { return m_got_head; }
			const std::string& GetLocation() const { return m_location; }
			const std::string& GetFileError() const { return m_file_error; }
			u64 GetReceived() const { return m_received; }

			bool OnHead(const HttpHead& head) override
			{
				m_got_head = true;
				m_content_length = head.content_length;
				m_response.status = head.status;
				m_response.content_type = head.content_type;

				if (m_follow_redirects && IsRedirectStatus(head.status))
				{
					m_location = head.location;
					m_stop = Stop::Redirect;
					return false;
				}

				if (m_job.kind == Kind::File)
				{
					if (head.status < 200 || head.status >= 300)
					{
						m_stop = Stop::Skipped;
						return false;
					}
					if (m_options.max_file_bytes != 0 && head.content_length > 0 &&
						static_cast<u64>(head.content_length) > m_options.max_file_bytes)
					{
						m_stop = Stop::TooLarge;
						return false;
					}

					Error error;
					m_file = FileSystem::OpenManagedCFile(m_job.file_path.c_str(), m_options.append ? "ab" : "wb", &error);
					if (!m_file)
					{
						m_file_error = error.GetDescription();
						m_stop = Stop::FileError;
						return false;
					}
					return true;
				}

				const size_t limit = (m_job.kind == Kind::Post) ? POST_REPLY_LIMIT : m_job.max_body_bytes;
				if (head.content_length >= 0 && static_cast<u64>(head.content_length) > limit)
				{
					m_stop = Stop::TooLarge;
					return false;
				}
				if (head.content_length > 0)
					m_response.body.reserve(static_cast<size_t>(head.content_length));
				return true;
			}

			bool OnBody(const u8* data, size_t size) override
			{
				if (m_options.cancel.IsCancelled())
				{
					m_stop = Stop::Cancelled;
					return false;
				}

				if (m_job.kind == Kind::File)
				{
					if (m_options.max_file_bytes != 0 && m_received + size > m_options.max_file_bytes)
					{
						m_stop = Stop::TooLarge;
						return false;
					}
					if (std::fwrite(data, 1, size, m_file.get()) != size)
					{
						m_file_error = "Write to the destination file failed (disk full?).";
						m_stop = Stop::FileError;
						return false;
					}
					m_received += size;
					if (m_options.on_data)
						m_options.on_data(data, size);
				}
				else
				{
					const size_t limit = (m_job.kind == Kind::Post) ? POST_REPLY_LIMIT : m_job.max_body_bytes;
					if (m_received + size > limit)
					{
						m_stop = Stop::TooLarge;
						return false;
					}
					m_response.body.append(reinterpret_cast<const char*>(data), size);
					m_received += size;
				}

				ReportProgress(false);
				return true;
			}

			bool ShouldAbort() override { return m_options.cancel.IsCancelled(); }

			void ReportProgress(bool force)
			{
				if (!m_options.on_progress)
					return;

				const auto now = std::chrono::steady_clock::now();
				if (!force && now - m_last_progress < std::chrono::milliseconds(100))
					return;
				m_last_progress = now;
				m_options.on_progress(m_received, m_content_length > 0 ? static_cast<u64>(m_content_length) : 0);
			}

			// Flushes and closes the output file. False if the data did not make it to disk.
			bool CloseFile(std::string* error)
			{
				if (!m_file)
					return true;

				std::FILE* fp = m_file.release();
				const bool ok = (std::fflush(fp) == 0) && (std::ferror(fp) == 0);
				if (std::fclose(fp) != 0 || !ok)
				{
					if (error)
						*error = "Could not finish writing the destination file.";
					return false;
				}
				return true;
			}

		private:
			const Job& m_job;
			const FetchOptions& m_options;
			FetchResponse& m_response;
			const bool m_follow_redirects;

			Stop m_stop = Stop::None;
			bool m_got_head = false;
			s64 m_content_length = -1;
			u64 m_received = 0;
			std::string m_location;
			std::string m_file_error;
			FileSystem::ManagedCFilePtr m_file;
			std::chrono::steady_clock::time_point m_last_progress{};
		};

		FetchResponse Finish(FetchResponse response, FetchResult result, std::string error)
		{
			response.result = result;
			response.error = std::move(error);
			return response;
		}

		FetchResponse RunFetch(HttpTransport& transport, const Job& job, const std::string& url, const FetchOptions& options)
		{
			FetchResponse response;
			response.final_url = url;

			if (!IsHttpsUrl(url))
				return Finish(std::move(response), FetchResult::BadUrl, fmt::format("Only https:// URLs are fetched: {}", url));

			const bool follow_redirects = (job.kind != Kind::Post);
			std::string current = url;
			for (int redirects = 0;; redirects++)
			{
				if (options.cancel.IsCancelled())
					return Finish(std::move(response), FetchResult::Cancelled, "Cancelled.");

				HttpExchange exchange;
				exchange.url = current;
				exchange.headers = options.headers;
				exchange.user_agent = options.user_agent.empty() ? DefaultUserAgent() : options.user_agent;
				exchange.connect_timeout_seconds = options.connect_timeout_seconds;
				exchange.idle_timeout_seconds = options.idle_timeout_seconds;
				exchange.is_post = (job.kind == Kind::Post);
				exchange.post_body = job.post_body;
				exchange.post_content_type = job.post_content_type;

				response.final_url = current;
				response.status = 0;
				response.content_type.clear();
				response.body.clear();

				FetchSink sink(job, options, response, follow_redirects);
				const std::string transport_error = transport.Perform(exchange, sink);
				response.bytes_received = sink.GetReceived();

				switch (sink.GetStop())
				{
					case FetchSink::Stop::Redirect:
					{
						const std::string next = ResolveRedirectLocation(current, sink.GetLocation());
						if (next.empty())
							return Finish(std::move(response), FetchResult::BadRedirect, fmt::format("Redirect ({}) from {} has no usable Location.", response.status, current));
						if (!IsHttpsUrl(next))
							return Finish(std::move(response), FetchResult::BadRedirect, fmt::format("Refusing redirect to a non-https URL: {}", next));
						if (redirects >= MAX_REDIRECTS)
							return Finish(std::move(response), FetchResult::BadRedirect, fmt::format("More than {} redirects.", MAX_REDIRECTS));
						current = next;
						continue;
					}

					case FetchSink::Stop::Skipped:
						return Finish(std::move(response), FetchResult::Ok, {});

					case FetchSink::Stop::Cancelled:
						return Finish(std::move(response), FetchResult::Cancelled, "Cancelled.");

					case FetchSink::Stop::TooLarge:
						return Finish(std::move(response), FetchResult::TooLarge, "The response is larger than the allowed size.");

					case FetchSink::Stop::FileError:
						return Finish(std::move(response), FetchResult::FileError, sink.GetFileError());

					case FetchSink::Stop::None:
						break;
				}

				if (!transport_error.empty())
				{
					if (options.cancel.IsCancelled())
						return Finish(std::move(response), FetchResult::Cancelled, "Cancelled.");
					return Finish(std::move(response), FetchResult::Transport, transport_error);
				}
				if (!sink.GotHead())
					return Finish(std::move(response), FetchResult::Transport, "No response.");

				std::string file_error;
				if (!sink.CloseFile(&file_error))
					return Finish(std::move(response), FetchResult::FileError, std::move(file_error));

				sink.ReportProgress(true);
				return Finish(std::move(response), FetchResult::Ok, {});
			}
		}
	} // namespace

	HttpFetcher::HttpFetcher(std::shared_ptr<HttpTransport> transport)
		: m_transport(std::move(transport))
	{
	}

	FetchResponse HttpFetcher::Get(const std::string& url, size_t max_body_bytes, const FetchOptions& options)
	{
		Job job;
		job.kind = Kind::Memory;
		job.max_body_bytes = max_body_bytes;
		return RunFetch(*m_transport, job, url, options);
	}

	FetchResponse HttpFetcher::GetToFile(const std::string& url, const std::string& path, const FetchOptions& options)
	{
		Job job;
		job.kind = Kind::File;
		job.file_path = path;
		return RunFetch(*m_transport, job, url, options);
	}

	FetchResponse HttpFetcher::Post(const std::string& url, std::string_view body, std::string_view content_type, const FetchOptions& options)
	{
		Job job;
		job.kind = Kind::Post;
		job.post_body = body;
		job.post_content_type = content_type;
		return RunFetch(*m_transport, job, url, options);
	}

	Fetcher& GetDefaultFetcher()
	{
		// Thread-safe initialisation; HttpFetcher keeps no per-call state.
		static HttpFetcher fetcher{std::shared_ptr<HttpTransport>(CreateDefaultHttpTransport())};
		return fetcher;
	}
} // namespace Catalog
