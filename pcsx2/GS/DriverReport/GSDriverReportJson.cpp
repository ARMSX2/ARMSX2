// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/DriverReport/GSDriverReportJson.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace GSDriverReport
{
	// Length of the UTF-8 sequence starting at s[i], or 0 if it is not a valid one.
	static size_t Utf8SequenceLength(std::string_view s, size_t i)
	{
		const unsigned char c = static_cast<unsigned char>(s[i]);
		size_t len;
		uint32_t cp;
		if (c < 0x80)
			return 1;
		else if ((c & 0xE0) == 0xC0)
		{
			len = 2;
			cp = c & 0x1F;
		}
		else if ((c & 0xF0) == 0xE0)
		{
			len = 3;
			cp = c & 0x0F;
		}
		else if ((c & 0xF8) == 0xF0)
		{
			len = 4;
			cp = c & 0x07;
		}
		else
			return 0;

		if (i + len > s.size())
			return 0;
		for (size_t k = 1; k < len; k++)
		{
			const unsigned char cc = static_cast<unsigned char>(s[i + k]);
			if ((cc & 0xC0) != 0x80)
				return 0;
			cp = (cp << 6) | (cc & 0x3F);
		}
		// Overlong forms, surrogates and values past U+10FFFF are all invalid.
		static constexpr uint32_t min_for_len[5] = {0, 0, 0x80, 0x800, 0x10000};
		if (cp < min_for_len[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			return 0;
		return len;
	}

	std::string EscapeJsonString(std::string_view s)
	{
		std::string out;
		out.reserve(s.size() + 2);
		for (size_t i = 0; i < s.size();)
		{
			const unsigned char c = static_cast<unsigned char>(s[i]);
			if (c >= 0x80)
			{
				const size_t len = Utf8SequenceLength(s, i);
				if (len == 0)
				{
					out += "\xEF\xBF\xBD";
					i++;
				}
				else
				{
					out.append(s.substr(i, len));
					i += len;
				}
				continue;
			}

			switch (c)
			{
				case '"':
					out += "\\\"";
					break;
				case '\\':
					out += "\\\\";
					break;
				case '\n':
					out += "\\n";
					break;
				case '\r':
					out += "\\r";
					break;
				case '\t':
					out += "\\t";
					break;
				case '\b':
					out += "\\b";
					break;
				case '\f':
					out += "\\f";
					break;
				default:
					if (c < 0x20 || c == 0x7F)
					{
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					}
					else
					{
						out.push_back(static_cast<char>(c));
					}
					break;
			}
			i++;
		}
		return out;
	}

	namespace
	{
		class JsonReader
		{
		public:
			explicit JsonReader(std::string_view text)
				: m_s(text)
			{
			}

			bool ParseTopObject(std::vector<std::pair<std::string, std::string>>* scalars)
			{
				SkipWs();
				if (!ParseObject(scalars, 0))
					return false;
				SkipWs();
				return m_i == m_s.size();
			}

		private:
			static constexpr int MAX_DEPTH = 64;

			void SkipWs()
			{
				while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\n' || m_s[m_i] == '\r'))
					m_i++;
			}

			bool Consume(char c)
			{
				SkipWs();
				if (m_i < m_s.size() && m_s[m_i] == c)
				{
					m_i++;
					return true;
				}
				return false;
			}

			static void AppendUtf8(std::string& out, uint32_t cp)
			{
				if (cp < 0x80)
					out.push_back(static_cast<char>(cp));
				else if (cp < 0x800)
				{
					out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				}
				else if (cp < 0x10000)
				{
					out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				}
				else
				{
					out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				}
			}

			bool ParseHex4(uint32_t* out)
			{
				if (m_i + 4 > m_s.size())
					return false;
				uint32_t v = 0;
				for (int k = 0; k < 4; k++)
				{
					const char c = m_s[m_i++];
					v <<= 4;
					if (c >= '0' && c <= '9')
						v |= static_cast<uint32_t>(c - '0');
					else if (c >= 'a' && c <= 'f')
						v |= static_cast<uint32_t>(c - 'a' + 10);
					else if (c >= 'A' && c <= 'F')
						v |= static_cast<uint32_t>(c - 'A' + 10);
					else
						return false;
				}
				*out = v;
				return true;
			}

			bool ParseString(std::string* out)
			{
				SkipWs();
				if (m_i >= m_s.size() || m_s[m_i] != '"')
					return false;
				m_i++;
				while (m_i < m_s.size())
				{
					const char c = m_s[m_i++];
					if (c == '"')
						return true;
					if (static_cast<unsigned char>(c) < 0x20)
						return false;
					if (c != '\\')
					{
						out->push_back(c);
						continue;
					}
					if (m_i >= m_s.size())
						return false;
					const char e = m_s[m_i++];
					switch (e)
					{
						case '"':
							out->push_back('"');
							break;
						case '\\':
							out->push_back('\\');
							break;
						case '/':
							out->push_back('/');
							break;
						case 'b':
							out->push_back('\b');
							break;
						case 'f':
							out->push_back('\f');
							break;
						case 'n':
							out->push_back('\n');
							break;
						case 'r':
							out->push_back('\r');
							break;
						case 't':
							out->push_back('\t');
							break;
						case 'u':
						{
							uint32_t cp;
							if (!ParseHex4(&cp))
								return false;
							if (cp >= 0xD800 && cp <= 0xDBFF && m_i + 6 <= m_s.size() && m_s[m_i] == '\\' &&
								m_s[m_i + 1] == 'u')
							{
								m_i += 2;
								uint32_t lo;
								if (!ParseHex4(&lo) || lo < 0xDC00 || lo > 0xDFFF)
									return false;
								cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
							}
							AppendUtf8(*out, cp);
							break;
						}
						default:
							return false;
					}
				}
				return false;
			}

			bool ParseLiteral(std::string* out)
			{
				SkipWs();
				const size_t start = m_i;
				for (const char* lit : {"true", "false", "null"})
				{
					const std::string_view l(lit);
					if (m_s.substr(m_i, l.size()) == l)
					{
						m_i += l.size();
						*out = std::string(l);
						return true;
					}
				}
				// Number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
				if (m_i < m_s.size() && m_s[m_i] == '-')
					m_i++;
				const auto digits = [this]() {
					const size_t s = m_i;
					while (m_i < m_s.size() && m_s[m_i] >= '0' && m_s[m_i] <= '9')
						m_i++;
					return m_i > s;
				};
				if (!digits())
					return false;
				if (m_i < m_s.size() && m_s[m_i] == '.')
				{
					m_i++;
					if (!digits())
						return false;
				}
				if (m_i < m_s.size() && (m_s[m_i] == 'e' || m_s[m_i] == 'E'))
				{
					m_i++;
					if (m_i < m_s.size() && (m_s[m_i] == '+' || m_s[m_i] == '-'))
						m_i++;
					if (!digits())
						return false;
				}
				*out = std::string(m_s.substr(start, m_i - start));
				return true;
			}

			bool ParseValue(std::string* scalar, bool* is_scalar, int depth)
			{
				SkipWs();
				if (m_i >= m_s.size())
					return false;
				const char c = m_s[m_i];
				*is_scalar = false;
				if (c == '{')
					return ParseObject(nullptr, depth + 1);
				if (c == '[')
					return ParseArray(depth + 1);
				*is_scalar = true;
				if (c == '"')
					return ParseString(scalar);
				return ParseLiteral(scalar);
			}

			bool ParseArray(int depth)
			{
				if (depth > MAX_DEPTH || !Consume('['))
					return false;
				if (Consume(']'))
					return true;
				for (;;)
				{
					std::string scalar;
					bool is_scalar;
					if (!ParseValue(&scalar, &is_scalar, depth))
						return false;
					if (Consume(','))
						continue;
					return Consume(']');
				}
			}

			bool ParseObject(std::vector<std::pair<std::string, std::string>>* scalars, int depth)
			{
				if (depth > MAX_DEPTH || !Consume('{'))
					return false;
				if (Consume('}'))
					return true;
				for (;;)
				{
					std::string key;
					if (!ParseString(&key) || !Consume(':'))
						return false;
					std::string scalar;
					bool is_scalar;
					if (!ParseValue(&scalar, &is_scalar, depth))
						return false;
					if (scalars && is_scalar)
						scalars->emplace_back(std::move(key), std::move(scalar));
					if (Consume(','))
						continue;
					return Consume('}');
				}
			}

			std::string_view m_s;
			size_t m_i = 0;
		};
	} // namespace

	bool ParseJsonObjectScalars(std::string_view text, std::vector<std::pair<std::string, std::string>>* scalars)
	{
		std::vector<std::pair<std::string, std::string>> tmp;
		JsonReader reader(text);
		if (!reader.ParseTopObject(&tmp))
			return false;
		if (scalars)
			*scalars = std::move(tmp);
		return true;
	}

	JsonWriter::JsonWriter()
	{
		m_out.reserve(64 * 1024);
	}

	void JsonWriter::Newline()
	{
		m_out.push_back('\n');
		m_out.append(m_stack.size() * 2, ' ');
	}

	void JsonWriter::BeforeValue()
	{
		if (m_after_key)
		{
			m_after_key = false;
			return;
		}
		if (m_stack.empty())
		{
			m_wrote_root = true;
			return;
		}
		Level& top = m_stack.back();
		if (!top.empty)
			m_out.push_back(',');
		top.empty = false;
		Newline();
	}

	void JsonWriter::BeginObject()
	{
		BeforeValue();
		m_out.push_back('{');
		m_stack.push_back({true, true});
	}

	void JsonWriter::EndObject()
	{
		if (m_stack.empty())
			return;
		const bool empty = m_stack.back().empty;
		m_stack.pop_back();
		if (!empty)
			Newline();
		m_out.push_back('}');
	}

	void JsonWriter::BeginArray()
	{
		BeforeValue();
		m_out.push_back('[');
		m_stack.push_back({false, true});
	}

	void JsonWriter::EndArray()
	{
		if (m_stack.empty())
			return;
		const bool empty = m_stack.back().empty;
		m_stack.pop_back();
		if (!empty)
			Newline();
		m_out.push_back(']');
	}

	void JsonWriter::Key(std::string_view key)
	{
		if (!m_stack.empty())
		{
			Level& top = m_stack.back();
			if (!top.empty)
				m_out.push_back(',');
			top.empty = false;
			Newline();
		}
		m_out.push_back('"');
		m_out += EscapeJsonString(key);
		m_out += "\": ";
		m_after_key = true;
	}

	void JsonWriter::String(std::string_view value)
	{
		BeforeValue();
		m_out.push_back('"');
		m_out += EscapeJsonString(value);
		m_out.push_back('"');
	}

	void JsonWriter::Bool(bool value)
	{
		BeforeValue();
		m_out += value ? "true" : "false";
	}

	void JsonWriter::Int(int64_t value)
	{
		BeforeValue();
		m_out += std::to_string(value);
	}

	void JsonWriter::UInt(uint64_t value)
	{
		BeforeValue();
		m_out += std::to_string(value);
	}

	void JsonWriter::Double(double value)
	{
		BeforeValue();
		if (!std::isfinite(value))
		{
			m_out += "null";
			return;
		}
		char buf[40];
		std::snprintf(buf, sizeof(buf), "%.9g", value);
		// A locale with a decimal comma would produce invalid JSON; the C locale is the norm here,
		// but make it impossible rather than unlikely.
		for (char* p = buf; *p; p++)
		{
			if (*p == ',')
				*p = '.';
		}
		m_out += buf;
	}

	void JsonWriter::Null()
	{
		BeforeValue();
		m_out += "null";
	}

	void JsonWriter::Hex(uint64_t value)
	{
		char buf[24];
		std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(value));
		String(buf);
	}

	void JsonWriter::Raw(std::string_view json)
	{
		BeforeValue();
		m_out.append(json);
	}

	double StepLog::NowMs()
	{
		using namespace std::chrono;
		return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
	}

	void StepLog::Add(std::string_view name, bool ok, std::string error, double ms)
	{
		m_steps.push_back(Step{std::string(name), ok, std::move(error), ms});
	}

	void StepLog::Append(const StepLog& other)
	{
		m_steps.insert(m_steps.end(), other.m_steps.begin(), other.m_steps.end());
	}

	void StepLog::Write(JsonWriter& w) const
	{
		w.BeginArray();
		for (const Step& s : m_steps)
		{
			w.BeginObject();
			w.KeyString("name", s.name);
			w.KeyBool("ok", s.ok);
			if (s.ok && s.error.empty())
				w.KeyNull("error");
			else
				w.KeyString("error", s.error);
			w.KeyDouble("ms", std::round(s.ms * 1000.0) / 1000.0);
			w.EndObject();
		}
		w.EndArray();
	}
} // namespace GSDriverReport
