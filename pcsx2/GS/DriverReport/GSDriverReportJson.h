// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// A small streaming JSON writer and the per-check step log used by the driver report.
// Deliberately free of the rest of the emulator so the command-line driverdiag tool and the unit
// tests can use it on their own.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GSDriverReport
{
	/// Escapes `s` as the inside of a JSON string. Bytes that are not valid UTF-8 become U+FFFD, so
	/// a driver string with stray bytes in it still yields a file every JSON parser accepts.
	std::string EscapeJsonString(std::string_view s);

	/// Validates `text` as one JSON object and returns its top-level scalar members: strings
	/// unescaped, numbers, booleans and null as their literal text. Nested values are validated and
	/// skipped. For reading a driver pack's meta.json, which is embedded verbatim only if valid.
	bool ParseJsonObjectScalars(std::string_view text, std::vector<std::pair<std::string, std::string>>* scalars);

	/// Writes one JSON document. Commas and indentation are handled here; the caller only has to
	/// nest Begin/End calls correctly and put a Key before every value inside an object.
	class JsonWriter
	{
	public:
		JsonWriter();

		void BeginObject();
		void EndObject();
		void BeginArray();
		void EndArray();

		void Key(std::string_view key);

		void String(std::string_view value);
		void Bool(bool value);
		void Int(int64_t value);
		void UInt(uint64_t value);
		/// NaN and infinities have no JSON spelling and are written as null.
		void Double(double value);
		void Null();
		/// Hex string "0x..." for IDs and bit masks, which read better that way than as decimals.
		void Hex(uint64_t value);
		/// A value that is already JSON, inserted verbatim. The caller vouches for its validity.
		void Raw(std::string_view json);

		void KeyString(std::string_view key, std::string_view value)
		{
			Key(key);
			String(value);
		}
		void KeyBool(std::string_view key, bool value)
		{
			Key(key);
			Bool(value);
		}
		void KeyInt(std::string_view key, int64_t value)
		{
			Key(key);
			Int(value);
		}
		void KeyUInt(std::string_view key, uint64_t value)
		{
			Key(key);
			UInt(value);
		}
		void KeyDouble(std::string_view key, double value)
		{
			Key(key);
			Double(value);
		}
		void KeyHex(std::string_view key, uint64_t value)
		{
			Key(key);
			Hex(value);
		}
		void KeyNull(std::string_view key)
		{
			Key(key);
			Null();
		}
		void KeyRaw(std::string_view key, std::string_view json)
		{
			Key(key);
			Raw(json);
		}

		/// True once every Begin has been matched by an End.
		bool IsComplete() const { return m_stack.empty() && m_wrote_root; }

		const std::string& GetString() const { return m_out; }
		std::string TakeString() { return std::move(m_out); }

	private:
		struct Level
		{
			bool is_object;
			bool empty;
		};

		void BeforeValue();
		void Newline();

		std::string m_out;
		std::vector<Level> m_stack;
		bool m_after_key = false;
		bool m_wrote_root = false;
	};

	/// One entry of the report's "steps" array: a check that can fail, whether it did, and how
	/// long it took. A failed step never stops the ones after it.
	struct Step
	{
		std::string name;
		bool ok = false;
		std::string error;
		double ms = 0.0;
	};

	class StepLog
	{
	public:
		/// Runs `fn(error)`, which returns true on success and may fill `error` on failure, and
		/// records the outcome and the time taken. (The emulator builds without exceptions, so a
		/// check reports failure by returning false.)
		template <typename F>
		bool Run(std::string_view name, F&& fn)
		{
			const double start = NowMs();
			std::string error;
			const bool ok = fn(error);
			Add(name, ok, std::move(error), NowMs() - start);
			return ok;
		}

		void Add(std::string_view name, bool ok, std::string error, double ms);
		void Append(const StepLog& other);
		const std::vector<Step>& GetSteps() const { return m_steps; }
		void Write(JsonWriter& w) const;

		static double NowMs();

	private:
		std::vector<Step> m_steps;
	};
} // namespace GSDriverReport
