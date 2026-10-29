// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// A Catalog::Fetcher for tests: URL -> canned response, with every request recorded.
//
// It is the real Catalog::HttpFetcher on top of a scripted transport, not a reimplementation, so
// a service under test goes through the production redirect policy, byte caps, file streaming,
// progress and cancellation. Only the network is replaced.
//
//   FakeFetcher http;
//   http.Ok("https://example.com/catalog.json", R"({"schema":2})")
//       .Redirect("https://old.example.com/x", "https://example.com/x")
//       .Status("https://example.com/missing", 404)
//       .Status("https://api.github.com/rate", 429);
//   auto service = MakeService(http);              // takes a Catalog::Fetcher&
//   ASSERT_EQ(http.RequestCount("https://example.com/catalog.json"), 1u);
//
// Several replies for one URL are served in order, the last one repeating, so "429 first, 200
// on the retry" is two Reply() calls. A URL nobody scripted fails as a transport error ("no reply
// scripted") rather than returning a default, so an unexpected request is visible.

#include "Catalog/HttpFetcher.h"

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace CatalogTest
{
	struct FakeReply
	{
		s32 status = 200;
		std::string body;
		std::string content_type = "application/octet-stream";

		std::string location;

		/// Content-Length announced: -2 = the body's size, -1 = not announced, otherwise as given
		/// (a value larger than the body, with cut_after, gives a truncated download).
		s64 content_length = -2;

		/// Deliver the body in pieces of this size; 0 = in one piece. Small pieces let a test cancel
		/// or hit a cap part-way through.
		size_t chunk_size = 0;

		/// Deliver only this many body bytes, then fail like a dropped connection.
		size_t cut_after = std::string::npos;

		/// Non-empty: fail like a connection or TLS error before any response arrives.
		std::string transport_error;
	};

	struct FakeRequest
	{
		std::string method; // "GET" or "POST"
		std::string url;
		std::vector<Catalog::Header> headers;
		std::string user_agent;
		std::string post_body;
		std::string post_content_type;
		size_t body_bytes_delivered = 0; // reply body bytes the fetcher accepted
	};

	class FakeTransport final : public Catalog::HttpTransport
	{
	public:
		void AddReply(const std::string& url, FakeReply reply)
		{
			std::lock_guard lock(m_mutex);
			m_replies[url].push_back(std::move(reply));
		}

		std::vector<FakeRequest> Requests() const
		{
			std::lock_guard lock(m_mutex);
			return m_requests;
		}

		std::string Perform(const Catalog::HttpExchange& exchange, Catalog::HttpSink& sink) override
		{
			FakeReply reply;
			size_t index = 0;
			{
				std::lock_guard lock(m_mutex);

				FakeRequest request;
				request.method = exchange.is_post ? "POST" : "GET";
				request.url = exchange.url;
				request.headers = exchange.headers;
				request.user_agent = exchange.user_agent;
				request.post_body = std::string(exchange.post_body);
				request.post_content_type = std::string(exchange.post_content_type);
				m_requests.push_back(std::move(request));
				index = m_requests.size() - 1;

				auto it = m_replies.find(exchange.url);
				if (it == m_replies.end() || it->second.empty())
					return "FakeFetcher: no reply scripted for " + exchange.url;

				reply = it->second.front();
				if (it->second.size() > 1)
					it->second.pop_front();
			}

			if (!reply.transport_error.empty())
				return reply.transport_error;

			Catalog::HttpHead head;
			head.status = reply.status;
			head.content_type = reply.content_type;
			head.location = reply.location;
			head.content_length = (reply.content_length == -2) ? static_cast<s64>(reply.body.size()) : reply.content_length;
			if (!sink.OnHead(head))
				return {};

			const size_t deliverable = std::min(reply.cut_after, reply.body.size());
			const size_t chunk = reply.chunk_size ? reply.chunk_size : std::max<size_t>(deliverable, 1);
			size_t delivered = 0;
			std::string result;
			while (delivered < deliverable)
			{
				if (sink.ShouldAbort())
				{
					result = "Aborted.";
					break;
				}

				const size_t n = std::min(chunk, deliverable - delivered);
				if (!sink.OnBody(reinterpret_cast<const u8*>(reply.body.data()) + delivered, n))
					break; // the sink stopped it; no transport error to report
				delivered += n;
			}

			if (result.empty() && delivered == deliverable && deliverable < reply.body.size())
				result = "FakeFetcher: connection closed before the whole body arrived";

			{
				std::lock_guard lock(m_mutex);
				m_requests[index].body_bytes_delivered = delivered;
			}
			return result;
		}

	private:
		mutable std::mutex m_mutex;
		std::map<std::string, std::deque<FakeReply>> m_replies;
		std::vector<FakeRequest> m_requests;
	};

	class FakeFetcher final : public Catalog::Fetcher
	{
	public:
		FakeFetcher()
			: m_transport(std::make_shared<FakeTransport>())
			, m_fetcher(m_transport)
		{
		}

		FakeFetcher& Reply(const std::string& url, FakeReply reply)
		{
			m_transport->AddReply(url, std::move(reply));
			return *this;
		}

		FakeFetcher& Ok(const std::string& url, std::string body, std::string content_type = "application/json")
		{
			FakeReply reply;
			reply.body = std::move(body);
			reply.content_type = std::move(content_type);
			return Reply(url, std::move(reply));
		}

		/// A redirect from `url` to `to` (which may be relative).
		FakeFetcher& Redirect(const std::string& url, const std::string& to, s32 status = 302)
		{
			FakeReply reply;
			reply.status = status;
			reply.location = to;
			reply.body = "redirecting";
			return Reply(url, std::move(reply));
		}

		FakeFetcher& Status(const std::string& url, s32 status, std::string body = {})
		{
			FakeReply reply;
			reply.status = status;
			reply.body = std::move(body);
			reply.content_type = "text/plain";
			return Reply(url, std::move(reply));
		}

		/// 200 whose connection drops after `delivered` bytes of the announced body.
		FakeFetcher& Truncated(const std::string& url, std::string body, size_t delivered)
		{
			FakeReply reply;
			reply.body = std::move(body);
			reply.cut_after = delivered;
			return Reply(url, std::move(reply));
		}

		/// A connection-level failure (DNS, TLS, timeout).
		FakeFetcher& Fail(const std::string& url, std::string error)
		{
			FakeReply reply;
			reply.transport_error = std::move(error);
			return Reply(url, std::move(reply));
		}

		std::vector<FakeRequest> Requests() const { return m_transport->Requests(); }

		size_t RequestCount(const std::string& url) const
		{
			const std::vector<FakeRequest> requests = m_transport->Requests();
			return static_cast<size_t>(std::count_if(requests.begin(), requests.end(),
				[&](const FakeRequest& r) { return r.url == url; }));
		}

		Catalog::FetchResponse Get(const std::string& url, size_t max_body_bytes, const Catalog::FetchOptions& options = {}) override
		{
			return m_fetcher.Get(url, max_body_bytes, options);
		}

		Catalog::FetchResponse GetToFile(const std::string& url, const std::string& path, const Catalog::FetchOptions& options = {}) override
		{
			return m_fetcher.GetToFile(url, path, options);
		}

		Catalog::FetchResponse Post(const std::string& url, std::string_view body, std::string_view content_type, const Catalog::FetchOptions& options = {}) override
		{
			return m_fetcher.Post(url, body, content_type, options);
		}

	private:
		std::shared_ptr<FakeTransport> m_transport;
		Catalog::HttpFetcher m_fetcher;
	};
} // namespace CatalogTest
