// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/Json.h"

#include "Catalog/FileUtil.h"

#include "rapidjson/encodedstream.h"
#include "rapidjson/error/en.h"
#include "rapidjson/memorystream.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "fmt/format.h"

namespace Catalog
{
	bool ParseJson(std::string_view text, rapidjson::Document* document, std::string* error)
	{
		rapidjson::MemoryStream memory(text.data(), text.size());
		rapidjson::EncodedInputStream<rapidjson::UTF8<>, rapidjson::MemoryStream> stream(memory);
		// Iterative: the default parser recurses once per nesting level, so a few megabytes of '[' from a remote
		// host would overflow the stack.
		document->ParseStream<rapidjson::kParseDefaultFlags | rapidjson::kParseIterativeFlag, rapidjson::UTF8<>>(stream);

		if (document->HasParseError())
		{
			if (error)
				*error = fmt::format("offset {}: {}", document->GetErrorOffset(), rapidjson::GetParseError_En(document->GetParseError()));
			return false;
		}
		return true;
	}

	std::string JsonToString(const rapidjson::Value& value, bool pretty)
	{
		rapidjson::StringBuffer buffer;
		if (pretty)
		{
			rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
			writer.SetIndent(' ', 2);
			value.Accept(writer);
		}
		else
		{
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			value.Accept(writer);
		}
		return std::string(buffer.GetString(), buffer.GetSize());
	}

	bool WriteJsonFileAtomic(const std::string& path, const rapidjson::Value& value, Error* error)
	{
		return WriteFileAtomic(path, JsonToString(value, true), error);
	}
} // namespace Catalog
