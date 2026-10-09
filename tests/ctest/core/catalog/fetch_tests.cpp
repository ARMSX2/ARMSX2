// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "catalog_test_support.h"

#include "FakeFetcher.h"

#include "Catalog/HttpFetcher.h"

#include "common/SHA256.h"

#include "fmt/format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace Catalog;
using CatalogTest::FakeFetcher;
using CatalogTest::FakeReply;
using CatalogTest::ScopedTempDir;

namespace
{
	const std::string URL = "https://example.com/catalog.json";

	std::string Pattern(size_t size)
	{
		std::string data(size, '\0');
		for (size_t i = 0; i < size; i++)
			data[i] = static_cast<char>('a' + (i * 7 + i / 251) % 26);
		return data;
	}

	// A chain of redirects https://example.com/hop0 -> hop1 -> ... -> hop<redirects>, which
	// answers 200 "end".
	void ScriptRedirectChain(FakeFetcher& http, int redirects)
	{
		for (int i = 0; i < redirects; i++)
			http.Redirect(fmt::format("https://example.com/hop{}", i), fmt::format("https://example.com/hop{}", i + 1));
		http.Ok(fmt::format("https://example.com/hop{}", redirects), "end", "text/plain");
	}
} // namespace

// ---- URL helpers ------------------------------------------------------------------------

TEST(CatalogUrl, OnlyHttpsWithAHostIsAccepted)
{
	EXPECT_TRUE(IsHttpsUrl("https://example.com"));
	EXPECT_TRUE(IsHttpsUrl("HTTPS://Example.com/x?y=1#z"));
	EXPECT_TRUE(IsHttpsUrl("https://127.0.0.1:8443/x"));

	EXPECT_FALSE(IsHttpsUrl(""));
	EXPECT_FALSE(IsHttpsUrl("http://example.com"));
	EXPECT_FALSE(IsHttpsUrl("ftp://example.com"));
	EXPECT_FALSE(IsHttpsUrl("file:///etc/passwd"));
	EXPECT_FALSE(IsHttpsUrl("https://"));
	EXPECT_FALSE(IsHttpsUrl("https:///path"));
	EXPECT_FALSE(IsHttpsUrl("https:example.com"));
	EXPECT_FALSE(IsHttpsUrl("https://exa mple.com"));
	EXPECT_FALSE(IsHttpsUrl("https://example.com/\nHost: evil"));
}

TEST(CatalogUrl, RedirectLocationsResolveAgainstTheirBase)
{
	const std::string base = "https://a.example/dir/file.json?x=1#frag";
	EXPECT_EQ(ResolveRedirectLocation(base, "https://b.example/z"), "https://b.example/z");
	EXPECT_EQ(ResolveRedirectLocation(base, "//b.example/z"), "https://b.example/z");
	EXPECT_EQ(ResolveRedirectLocation(base, "/z"), "https://a.example/z");
	EXPECT_EQ(ResolveRedirectLocation(base, "other.json"), "https://a.example/dir/other.json");
	EXPECT_EQ(ResolveRedirectLocation(base, "?y=2"), "https://a.example/dir/file.json?y=2");
	EXPECT_EQ(ResolveRedirectLocation(base, "#top"), "https://a.example/dir/file.json?x=1#top");
	EXPECT_EQ(ResolveRedirectLocation("https://a.example", "x"), "https://a.example/x");
	EXPECT_EQ(ResolveRedirectLocation("https://a.example:8443/p", "/q"), "https://a.example:8443/q");

	// Policy is not the resolver's job: other schemes come back unchanged for the caller to refuse.
	EXPECT_EQ(ResolveRedirectLocation(base, "http://evil.example/x"), "http://evil.example/x");
	EXPECT_EQ(ResolveRedirectLocation(base, "javascript:alert(1)"), "javascript:alert(1)");

	EXPECT_EQ(ResolveRedirectLocation(base, ""), "");
	EXPECT_EQ(ResolveRedirectLocation("ftp://a.example/x", "y"), "");
	EXPECT_EQ(ResolveRedirectLocation("not a url", "y"), "");
}

TEST(CatalogCancelToken, CopiesShareOneFlag)
{
	CancelToken token;
	const CancelToken copy = token;
	EXPECT_FALSE(copy.IsCancelled());
	token.Cancel();
	EXPECT_TRUE(copy.IsCancelled());
	EXPECT_TRUE(*copy.Flag());

	EXPECT_FALSE(CancelToken().IsCancelled()); // a new token has its own flag
}

// ---- Get --------------------------------------------------------------------------------

TEST(CatalogFetchGet, ReturnsBodyAndMetadata)
{
	FakeFetcher http;
	http.Ok(URL, R"({"schemaVersion":2})");

	const FetchResponse r = http.Get(URL, 1024);
	EXPECT_EQ(r.result, FetchResult::Ok);
	EXPECT_TRUE(r.IsSuccess());
	EXPECT_EQ(r.status, 200);
	EXPECT_EQ(r.final_url, URL);
	EXPECT_EQ(r.content_type, "application/json");
	EXPECT_EQ(r.body, R"({"schemaVersion":2})");
	EXPECT_EQ(r.bytes_received, r.body.size());
	EXPECT_TRUE(r.error.empty());
}

TEST(CatalogFetchGet, SendsAnIdentifyingUserAgentAndTheCallersHeaders)
{
	FakeFetcher http;
	http.Ok(URL, "{}");

	FetchOptions options;
	options.headers.push_back({"Accept", "application/json"});
	http.Get(URL, 1024, options);
	options.user_agent = "custom/1.0";
	http.Get(URL, 1024, options);

	const std::vector<CatalogTest::FakeRequest> requests = http.Requests();
	ASSERT_EQ(requests.size(), 2u);
	EXPECT_EQ(requests[0].method, "GET");
	EXPECT_EQ(requests[0].user_agent.rfind("ARMSX2/", 0), 0u) << requests[0].user_agent;
	EXPECT_EQ(requests[0].user_agent, DefaultUserAgent());
	ASSERT_EQ(requests[0].headers.size(), 1u);
	EXPECT_EQ(requests[0].headers[0].first, "Accept");
	EXPECT_EQ(requests[1].user_agent, "custom/1.0");
}

TEST(CatalogFetchGet, ErrorStatusesAreResponsesNotFailures)
{
	FakeFetcher http;
	http.Status("https://example.com/404", 404);
	http.Status("https://example.com/403", 403, R"({"message":"API rate limit exceeded"})");
	http.Status("https://example.com/429", 429);

	for (const s32 status : {404, 403, 429})
	{
		const FetchResponse r = http.Get(fmt::format("https://example.com/{}", status), 1024);
		EXPECT_EQ(r.result, FetchResult::Ok) << status;
		EXPECT_EQ(r.status, status);
		EXPECT_FALSE(r.IsSuccess());
	}
	EXPECT_EQ(http.Get("https://example.com/403", 1024).body, R"({"message":"API rate limit exceeded"})");
}

TEST(CatalogFetchGet, RepliesForOneUrlAreServedInOrderThenRepeat)
{
	FakeFetcher http;
	http.Status(URL, 429).Ok(URL, "fine");
	EXPECT_EQ(http.Get(URL, 1024).status, 429);
	EXPECT_EQ(http.Get(URL, 1024).body, "fine");
	EXPECT_EQ(http.Get(URL, 1024).body, "fine");
}

TEST(CatalogFetchGet, AnUnscriptedUrlFailsLoudly)
{
	FakeFetcher http;
	const FetchResponse r = http.Get("https://example.com/nobody-scripted-this", 1024);
	EXPECT_EQ(r.result, FetchResult::Transport);
	EXPECT_NE(r.error.find("no reply scripted"), std::string::npos);
}

TEST(CatalogFetchGet, TransportFailureIsReported)
{
	FakeFetcher http;
	http.Fail(URL, "Could not resolve host");
	const FetchResponse r = http.Get(URL, 1024);
	EXPECT_EQ(r.result, FetchResult::Transport);
	EXPECT_EQ(r.error, "Could not resolve host");
	EXPECT_EQ(r.status, 0);
}

TEST(CatalogFetchGet, ABodyShorterThanAnnouncedIsAFailure)
{
	FakeFetcher http;
	http.Truncated(URL, std::string(1000, 'x'), 400);
	const FetchResponse r = http.Get(URL, 4096);
	EXPECT_EQ(r.result, FetchResult::Transport);
	EXPECT_FALSE(r.IsSuccess());
}

// ---- Redirects --------------------------------------------------------------------------

TEST(CatalogFetchRedirect, FollowsToTheFinalUrl)
{
	FakeFetcher http;
	ScriptRedirectChain(http, 3);

	const FetchResponse r = http.Get("https://example.com/hop0", 1024);
	EXPECT_TRUE(r.IsSuccess());
	EXPECT_EQ(r.final_url, "https://example.com/hop3");
	EXPECT_EQ(r.body, "end");
	EXPECT_EQ(http.Requests().size(), 4u);
}

TEST(CatalogFetchRedirect, FiveRedirectsAreFollowedASixthIsRefused)
{
	{
		FakeFetcher http;
		ScriptRedirectChain(http, MAX_REDIRECTS);
		const FetchResponse r = http.Get("https://example.com/hop0", 1024);
		EXPECT_TRUE(r.IsSuccess());
		EXPECT_EQ(r.final_url, fmt::format("https://example.com/hop{}", MAX_REDIRECTS));
	}
	{
		FakeFetcher http;
		ScriptRedirectChain(http, MAX_REDIRECTS + 1);
		const FetchResponse r = http.Get("https://example.com/hop0", 1024);
		EXPECT_EQ(r.result, FetchResult::BadRedirect);
		EXPECT_FALSE(r.IsSuccess());
		EXPECT_EQ(http.Requests().size(), static_cast<size_t>(MAX_REDIRECTS + 1)); // the last hop is never requested
	}
}

TEST(CatalogFetchRedirect, ALoopEndsAtTheHopLimit)
{
	FakeFetcher http;
	http.Redirect("https://example.com/a", "https://example.com/b");
	http.Redirect("https://example.com/b", "https://example.com/a");
	const FetchResponse r = http.Get("https://example.com/a", 1024);
	EXPECT_EQ(r.result, FetchResult::BadRedirect);
	EXPECT_EQ(http.Requests().size(), static_cast<size_t>(MAX_REDIRECTS + 1));
}

TEST(CatalogFetchRedirect, ARedirectToPlainHttpIsRefusedWithoutRequestingIt)
{
	FakeFetcher http;
	http.Redirect(URL, "http://example.com/catalog.json");
	http.Ok("http://example.com/catalog.json", "{}");

	const FetchResponse r = http.Get(URL, 1024);
	EXPECT_EQ(r.result, FetchResult::BadRedirect);
	EXPECT_NE(r.error.find("non-https"), std::string::npos);
	EXPECT_EQ(http.RequestCount("http://example.com/catalog.json"), 0u);
}

TEST(CatalogFetchRedirect, OtherSchemesAreRefusedToo)
{
	FakeFetcher http;
	http.Redirect(URL, "file:///etc/passwd");
	EXPECT_EQ(http.Get(URL, 1024).result, FetchResult::BadRedirect);
}

TEST(CatalogFetchRedirect, RelativeLocationsAreResolved)
{
	FakeFetcher http;
	http.Redirect("https://example.com/b/start", "/c/next");
	http.Ok("https://example.com/c/next", "landed", "text/plain");

	const FetchResponse r = http.Get("https://example.com/b/start", 1024);
	EXPECT_TRUE(r.IsSuccess());
	EXPECT_EQ(r.final_url, "https://example.com/c/next");
}

TEST(CatalogFetchRedirect, ARedirectWithoutALocationIsRefused)
{
	FakeFetcher http;
	http.Redirect(URL, "");
	EXPECT_EQ(http.Get(URL, 1024).result, FetchResult::BadRedirect);
}

TEST(CatalogFetchRedirect, ARedirectBodyIsNeverRead)
{
	FakeFetcher http;
	ScriptRedirectChain(http, 1);
	http.Get("https://example.com/hop0", 1024);
	EXPECT_EQ(http.Requests()[0].body_bytes_delivered, 0u);
}

TEST(CatalogFetchGet, APlainHttpUrlIsRefusedBeforeAnyRequest)
{
	FakeFetcher http;
	http.Ok("http://example.com/x", "{}");
	const FetchResponse r = http.Get("http://example.com/x", 1024);
	EXPECT_EQ(r.result, FetchResult::BadUrl);
	EXPECT_TRUE(http.Requests().empty());
}

// ---- Byte caps --------------------------------------------------------------------------

TEST(CatalogFetchCap, ABodyExactlyAtTheCapIsAccepted)
{
	FakeFetcher http;
	http.Ok(URL, std::string(1000, 'x'));
	const FetchResponse r = http.Get(URL, 1000);
	EXPECT_TRUE(r.IsSuccess());
	EXPECT_EQ(r.body.size(), 1000u);
}

TEST(CatalogFetchCap, AnAnnouncedLengthOverTheCapIsRefusedWithoutReadingTheBody)
{
	FakeFetcher http;
	http.Ok(URL, std::string(1001, 'x'));
	const FetchResponse r = http.Get(URL, 1000);
	EXPECT_EQ(r.result, FetchResult::TooLarge);
	EXPECT_TRUE(r.body.empty());
	EXPECT_EQ(http.Requests()[0].body_bytes_delivered, 0u);
}

TEST(CatalogFetchCap, AStreamOfUnannouncedLengthIsCutOffAtTheCap)
{
	FakeFetcher http;
	FakeReply reply;
	reply.body = std::string(10000, 'x');
	reply.content_length = -1;
	reply.chunk_size = 100;
	http.Reply(URL, reply);

	const FetchResponse r = http.Get(URL, 1000);
	EXPECT_EQ(r.result, FetchResult::TooLarge);
	EXPECT_LE(http.Requests()[0].body_bytes_delivered, 1000u);
}

TEST(CatalogFetchCap, ALyingContentLengthDoesNotBeatTheCap)
{
	FakeFetcher http;
	FakeReply reply;
	reply.body = std::string(5000, 'x');
	reply.content_length = 10; // announces little, sends a lot
	reply.chunk_size = 100;
	http.Reply(URL, reply);
	EXPECT_EQ(http.Get(URL, 1000).result, FetchResult::TooLarge);
}

// ---- Cancellation -----------------------------------------------------------------------

TEST(CatalogFetchCancel, ACancelledTokenStopsBeforeAnyRequest)
{
	FakeFetcher http;
	http.Ok(URL, "{}");
	FetchOptions options;
	options.cancel.Cancel();
	const FetchResponse r = http.Get(URL, 1024, options);
	EXPECT_EQ(r.result, FetchResult::Cancelled);
	EXPECT_TRUE(http.Requests().empty());
}

TEST(CatalogFetchCancel, CancellingFromTheProgressCallbackStopsTheDownload)
{
	FakeFetcher http;
	FakeReply reply;
	reply.body = std::string(10000, 'x');
	reply.chunk_size = 100;
	http.Reply(URL, reply);

	FetchOptions options;
	options.on_progress = [&](u64, u64) { options.cancel.Cancel(); };
	const FetchResponse r = http.Get(URL, 1 << 20, options);
	EXPECT_EQ(r.result, FetchResult::Cancelled);
	EXPECT_LT(http.Requests()[0].body_bytes_delivered, 10000u);
}

// ---- GetToFile --------------------------------------------------------------------------

TEST(CatalogFetchToFile, StreamsTheBodyToDiskAndFeedsTheTap)
{
	ScopedTempDir dir;
	const std::string data = Pattern(3 * 1024 * 1024 + 123);

	FakeFetcher http;
	FakeReply reply;
	reply.body = data;
	reply.chunk_size = 64 * 1024;
	http.Reply("https://example.com/pack.tar.zst", reply);

	SHA256Digest tap;
	u64 tapped = 0;
	std::vector<u64> progress;
	u64 progress_total = 0;
	FetchOptions options;
	options.on_data = [&](const u8* chunk, size_t size) {
		tap.Update(chunk, size);
		tapped += size;
	};
	options.on_progress = [&](u64 done, u64 total) {
		progress.push_back(done);
		progress_total = total;
	};

	const FetchResponse r = http.GetToFile("https://example.com/pack.tar.zst", dir.File("pack.bin"), options);
	ASSERT_TRUE(r.IsSuccess()) << r.error;
	EXPECT_TRUE(r.body.empty()) << "a file download must not also buffer the body";
	EXPECT_EQ(r.bytes_received, data.size());
	EXPECT_EQ(tapped, data.size());
	EXPECT_EQ(tap.FinalHex(), SHA256Digest::HashHex(data));

	std::string on_disk;
	EXPECT_EQ(SHA256Digest::HashFile(dir.File("pack.bin").c_str(), &on_disk), SHA256Digest::FileResult::Ok);
	EXPECT_EQ(on_disk, SHA256Digest::HashHex(data));

	ASSERT_FALSE(progress.empty());
	EXPECT_TRUE(std::is_sorted(progress.begin(), progress.end()));
	EXPECT_EQ(progress.back(), data.size());
	EXPECT_EQ(progress_total, data.size());
}

TEST(CatalogFetchToFile, AppendBuildsOneFileFromSeveralParts)
{
	ScopedTempDir dir;
	FakeFetcher http;
	http.Ok("https://example.com/part1", "AAAA", "application/octet-stream");
	http.Ok("https://example.com/part2", "BBBB", "application/octet-stream");
	const std::string file = dir.File("pack.download");

	FetchOptions options;
	EXPECT_TRUE(http.GetToFile("https://example.com/part1", file, options).IsSuccess());
	options.append = true;
	EXPECT_TRUE(http.GetToFile("https://example.com/part2", file, options).IsSuccess());
	EXPECT_EQ(FileSystem::ReadFileToString(file.c_str()).value_or(""), "AAAABBBB");

	// Without append the file starts over.
	options.append = false;
	EXPECT_TRUE(http.GetToFile("https://example.com/part2", file, options).IsSuccess());
	EXPECT_EQ(FileSystem::ReadFileToString(file.c_str()).value_or(""), "BBBB");
}

TEST(CatalogFetchToFile, ANotFoundLeavesAnExistingFileAlone)
{
	ScopedTempDir dir;
	const std::string file = dir.File("keep.bin");
	ASSERT_TRUE(FileSystem::WriteStringToFile(file.c_str(), "precious"));

	FakeFetcher http;
	http.Status("https://example.com/gone", 404, "<html>not found</html>");
	const FetchResponse r = http.GetToFile("https://example.com/gone", file);
	EXPECT_EQ(r.result, FetchResult::Ok);
	EXPECT_EQ(r.status, 404);
	EXPECT_FALSE(r.IsSuccess());
	EXPECT_EQ(FileSystem::ReadFileToString(file.c_str()).value_or(""), "precious");

	// And creates nothing when there was nothing.
	const std::string absent = dir.File("absent.bin");
	http.GetToFile("https://example.com/gone", absent);
	EXPECT_FALSE(FileSystem::FileExists(absent.c_str()));
}

TEST(CatalogFetchToFile, FollowsRedirectsAndWritesOnlyTheFinalBody)
{
	ScopedTempDir dir;
	FakeFetcher http;
	ScriptRedirectChain(http, 2);
	const std::string file = dir.File("out.bin");
	const FetchResponse r = http.GetToFile("https://example.com/hop0", file);
	ASSERT_TRUE(r.IsSuccess()) << r.error;
	EXPECT_EQ(FileSystem::ReadFileToString(file.c_str()).value_or(""), "end");
}

TEST(CatalogFetchToFile, AnEmptyBodyStillCreatesTheFile)
{
	ScopedTempDir dir;
	FakeFetcher http;
	http.Ok("https://example.com/empty", "", "application/octet-stream");
	const std::string file = dir.File("empty.bin");
	EXPECT_TRUE(http.GetToFile("https://example.com/empty", file).IsSuccess());
	EXPECT_TRUE(FileSystem::FileExists(file.c_str()));
	EXPECT_EQ(FileSystem::GetPathFileSize(file.c_str()), 0);
}

TEST(CatalogFetchToFile, MaxFileBytesRefusesAnAnnouncedOversizeBeforeWriting)
{
	ScopedTempDir dir;
	FakeFetcher http;
	http.Ok("https://example.com/big", std::string(5000, 'x'), "application/octet-stream");
	FetchOptions options;
	options.max_file_bytes = 4000;
	const FetchResponse r = http.GetToFile("https://example.com/big", dir.File("big.bin"), options);
	EXPECT_EQ(r.result, FetchResult::TooLarge);
	EXPECT_FALSE(FileSystem::FileExists(dir.File("big.bin").c_str()));
}

TEST(CatalogFetchToFile, MaxFileBytesCutsOffAnUnannouncedStream)
{
	ScopedTempDir dir;
	FakeFetcher http;
	FakeReply reply;
	reply.body = std::string(50000, 'x');
	reply.content_length = -1;
	reply.chunk_size = 1000;
	http.Reply("https://example.com/endless", reply);

	FetchOptions options;
	options.max_file_bytes = 4000;
	const FetchResponse r = http.GetToFile("https://example.com/endless", dir.File("big.bin"), options);
	EXPECT_EQ(r.result, FetchResult::TooLarge);
	EXPECT_LE(FileSystem::GetPathFileSize(dir.File("big.bin").c_str()), 4000);
}

TEST(CatalogFetchToFile, AnUnwritableDestinationIsAFileError)
{
	ScopedTempDir dir;
	FakeFetcher http;
	http.Ok("https://example.com/x", "data", "application/octet-stream");
	bool tapped = false;
	FetchOptions options;
	options.on_data = [&](const u8*, size_t) { tapped = true; };
	const FetchResponse r = http.GetToFile("https://example.com/x", dir.File("no-such-dir/out.bin"), options);
	EXPECT_EQ(r.result, FetchResult::FileError);
	EXPECT_FALSE(r.error.empty());
	EXPECT_FALSE(tapped);
}

TEST(CatalogFetchToFile, CancellingMidDownloadKeepsWhatWasWritten)
{
	ScopedTempDir dir;
	FakeFetcher http;
	FakeReply reply;
	reply.body = std::string(10000, 'x');
	reply.chunk_size = 1000;
	http.Reply("https://example.com/x", reply);

	u64 seen = 0;
	FetchOptions options;
	options.on_data = [&](const u8*, size_t size) {
		seen += size;
		if (seen >= 3000)
			options.cancel.Cancel();
	};
	const FetchResponse r = http.GetToFile("https://example.com/x", dir.File("out.bin"), options);
	EXPECT_EQ(r.result, FetchResult::Cancelled);
	EXPECT_EQ(FileSystem::GetPathFileSize(dir.File("out.bin").c_str()), 3000);
}

TEST(CatalogFetchToFile, ATruncatedDownloadIsAFailure)
{
	ScopedTempDir dir;
	FakeFetcher http;
	http.Truncated("https://example.com/x", std::string(5000, 'x'), 2000);
	const FetchResponse r = http.GetToFile("https://example.com/x", dir.File("out.bin"));
	EXPECT_EQ(r.result, FetchResult::Transport);
	EXPECT_EQ(r.bytes_received, 2000u);
}

// ---- Post -------------------------------------------------------------------------------

TEST(CatalogFetchPost, SendsTheBodyAndContentType)
{
	FakeFetcher http;
	http.Status("https://example.com/hit", 204);
	const FetchResponse r = http.Post("https://example.com/hit", "some-pack-id", "text/plain; charset=utf-8");
	EXPECT_EQ(r.result, FetchResult::Ok);
	EXPECT_EQ(r.status, 204);

	const std::vector<CatalogTest::FakeRequest> requests = http.Requests();
	ASSERT_EQ(requests.size(), 1u);
	EXPECT_EQ(requests[0].method, "POST");
	EXPECT_EQ(requests[0].post_body, "some-pack-id");
	EXPECT_EQ(requests[0].post_content_type, "text/plain; charset=utf-8");
}

TEST(CatalogFetchPost, DoesNotFollowRedirects)
{
	FakeFetcher http;
	http.Redirect("https://example.com/hit", "https://example.com/elsewhere");
	http.Ok("https://example.com/elsewhere", "ok");
	const FetchResponse r = http.Post("https://example.com/hit", "x", "text/plain");
	EXPECT_EQ(r.status, 302);
	EXPECT_EQ(http.RequestCount("https://example.com/elsewhere"), 0u);
}

TEST(CatalogFetchPost, RefusesPlainHttp)
{
	FakeFetcher http;
	EXPECT_EQ(http.Post("http://example.com/hit", "x", "text/plain").result, FetchResult::BadUrl);
	EXPECT_TRUE(http.Requests().empty());
}

TEST(CatalogFetchPost, AHugeReplyIsRefused)
{
	FakeFetcher http;
	http.Ok("https://example.com/hit", std::string(200 * 1024, 'x'), "text/plain");
	EXPECT_EQ(http.Post("https://example.com/hit", "x", "text/plain").result, FetchResult::TooLarge);
}

// ---- Concurrency and the real fetcher ---------------------------------------------------

TEST(CatalogFetchConcurrency, ManyThreadsMayFetchAtOnce)
{
	FakeFetcher http;
	for (int i = 0; i < 8; i++)
		http.Ok(fmt::format("https://example.com/{}", i), std::string(1000 + i, static_cast<char>('a' + i)));

	std::vector<std::thread> threads;
	std::atomic<int> failures{0};
	for (int t = 0; t < 8; t++)
	{
		threads.emplace_back([&, t]() {
			for (int n = 0; n < 50; n++)
			{
				const FetchResponse r = http.Get(fmt::format("https://example.com/{}", t), 4096);
				if (!r.IsSuccess() || r.body != std::string(1000 + t, static_cast<char>('a' + t)))
					failures++;
			}
		});
	}
	for (std::thread& thread : threads)
		thread.join();
	EXPECT_EQ(failures.load(), 0);
	EXPECT_EQ(http.Requests().size(), 400u);
}

// These use the real fetcher and its real transport, but only on inputs it rejects before
// any connection is made.
TEST(CatalogRealFetcher, RefusesPlainHttpWithoutTouchingTheNetwork)
{
	Fetcher& fetcher = GetDefaultFetcher();
	EXPECT_EQ(&fetcher, &GetDefaultFetcher());
	EXPECT_EQ(fetcher.Get("http://example.com/", 1024).result, FetchResult::BadUrl);
	EXPECT_EQ(fetcher.Post("http://example.com/", "x", "text/plain").result, FetchResult::BadUrl);
	ScopedTempDir dir;
	EXPECT_EQ(fetcher.GetToFile("http://example.com/", dir.File("x")).result, FetchResult::BadUrl);
	EXPECT_FALSE(FileSystem::FileExists(dir.File("x").c_str()));
}

TEST(CatalogRealFetcher, ACancelledTokenNeverConnects)
{
	FetchOptions options;
	options.cancel.Cancel();
	EXPECT_EQ(GetDefaultFetcher().Get("https://example.com/", 1024, options).result, FetchResult::Cancelled);
}
