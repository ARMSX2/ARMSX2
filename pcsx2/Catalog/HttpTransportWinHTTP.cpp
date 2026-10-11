// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// WinHTTP transport for HttpFetcher (Windows). Synchronous: one session/connection/request per
// Perform() call, so concurrent calls from different threads are independent. Redirects are
// switched off here and left to HttpFetcher.
//
// Cancellation is checked between reads; a stalled read ends at the idle timeout rather than
// within a second as it does with curl.

#include "Catalog/HttpFetcher.h"

#include "common/RedtapeWindows.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <winhttp.h>

#include <cstdlib>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace Catalog
{
	namespace
	{
		class WinHttpHandle
		{
		public:
			explicit WinHttpHandle(HINTERNET handle = nullptr)
				: m_handle(handle)
			{
			}
			~WinHttpHandle()
			{
				if (m_handle)
					WinHttpCloseHandle(m_handle);
			}
			WinHttpHandle(const WinHttpHandle&) = delete;
			WinHttpHandle& operator=(const WinHttpHandle&) = delete;

			HINTERNET Get() const { return m_handle; }
			explicit operator bool() const { return m_handle != nullptr; }

		private:
			HINTERNET m_handle;
		};

		std::string QueryHeaderString(HINTERNET request, DWORD info_level)
		{
			DWORD size = 0;
			WinHttpQueryHeaders(request, info_level, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
				return {};

			std::wstring value(size / sizeof(wchar_t), L'\0');
			if (!WinHttpQueryHeaders(request, info_level, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX))
				return {};
			value.resize(size / sizeof(wchar_t)); // on success `size` excludes the terminator
			return StringUtil::WideStringToUTF8String(value);
		}

		class WinHttpTransport final : public HttpTransport
		{
		public:
			std::string Perform(const HttpExchange& exchange, HttpSink& sink) override
			{
				const std::wstring wide_url = StringUtil::UTF8StringToWideString(exchange.url);

				URL_COMPONENTS parts = {};
				parts.dwStructSize = sizeof(parts);
				parts.dwHostNameLength = static_cast<DWORD>(-1);
				parts.dwUrlPathLength = static_cast<DWORD>(-1);
				parts.dwExtraInfoLength = static_cast<DWORD>(-1);
				if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &parts))
					return fmt::format("Cannot parse URL (error {}).", GetLastError());

				const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
				std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
				object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
				if (object.empty())
					object = L"/";

				WinHttpHandle session(WinHttpOpen(StringUtil::UTF8StringToWideString(exchange.user_agent).c_str(),
					WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
				if (!session)
					return fmt::format("WinHttpOpen failed (error {}).", GetLastError());

				const int connect_ms = static_cast<int>(exchange.connect_timeout_seconds * 1000);
				const int idle_ms = static_cast<int>(exchange.idle_timeout_seconds * 1000);
				WinHttpSetTimeouts(session.Get(), connect_ms, connect_ms, idle_ms, idle_ms);

				WinHttpHandle connection(WinHttpConnect(session.Get(), host.c_str(), parts.nPort, 0));
				if (!connection)
					return fmt::format("WinHttpConnect failed (error {}).", GetLastError());

				WinHttpHandle request(WinHttpOpenRequest(connection.Get(), exchange.is_post ? L"POST" : L"GET", object.c_str(),
					nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
				if (!request)
					return fmt::format("WinHttpOpenRequest failed (error {}).", GetLastError());

				DWORD disable = WINHTTP_DISABLE_REDIRECTS;
				WinHttpSetOption(request.Get(), WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

				std::string header_block;
				for (const Header& header : exchange.headers)
					header_block += fmt::format("{}: {}\r\n", header.first, header.second);
				if (exchange.is_post && !exchange.post_content_type.empty())
					header_block += fmt::format("Content-Type: {}\r\n", exchange.post_content_type);
				const std::wstring wide_headers = StringUtil::UTF8StringToWideString(header_block);

				const DWORD body_size = static_cast<DWORD>(exchange.post_body.size());
				if (!WinHttpSendRequest(request.Get(),
						wide_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wide_headers.c_str(),
						wide_headers.empty() ? 0 : static_cast<DWORD>(-1),
						body_size ? const_cast<char*>(exchange.post_body.data()) : WINHTTP_NO_REQUEST_DATA,
						body_size, body_size, 0))
					return fmt::format("WinHttpSendRequest failed (error {}).", GetLastError());

				if (!WinHttpReceiveResponse(request.Get(), nullptr))
					return fmt::format("WinHttpReceiveResponse failed (error {}).", GetLastError());

				HttpHead head;
				DWORD status = 0;
				DWORD status_size = sizeof(status);
				if (WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
						WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
					head.status = static_cast<s32>(status);
				head.content_type = QueryHeaderString(request.Get(), WINHTTP_QUERY_CONTENT_TYPE);
				head.location = QueryHeaderString(request.Get(), WINHTTP_QUERY_LOCATION);
				const std::string length = QueryHeaderString(request.Get(), WINHTTP_QUERY_CONTENT_LENGTH);
				if (!length.empty())
					head.content_length = std::strtoll(length.c_str(), nullptr, 10);

				if (!sink.OnHead(head))
					return {};

				std::vector<u8> buffer(128 * 1024);
				u64 total = 0;
				for (;;)
				{
					if (sink.ShouldAbort())
						return "Aborted.";

					DWORD read = 0;
					if (!WinHttpReadData(request.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read))
						return fmt::format("WinHttpReadData failed (error {}).", GetLastError());
					if (read == 0)
						break;

					total += read;
					if (!sink.OnBody(buffer.data(), read))
						return {};
				}

				if (head.content_length >= 0 && total != static_cast<u64>(head.content_length))
					return fmt::format("Transfer ended after {} of {} bytes.", total, head.content_length);
				return {};
			}
		};
	} // namespace

	std::unique_ptr<HttpTransport> CreateDefaultHttpTransport()
	{
		return std::make_unique<WinHttpTransport>();
	}
} // namespace Catalog
