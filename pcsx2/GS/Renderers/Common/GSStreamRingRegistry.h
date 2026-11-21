// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

/// The stream rings that exist, so the device can clean every one before a submit without a hand-written list. A
/// ring joins when it is created and leaves when it is destroyed; a move tells the registry where the ring now is.
/// Owned by the thread that records commands (one registry per device). A handful of rings: a vector.
template <typename Ring>
class GSStreamRingRegistry
{
public:
	/// A ring was created. A second Create of the same ring adds nothing.
	void Add(Ring* ring)
	{
		if (std::find(m_rings.begin(), m_rings.end(), ring) == m_rings.end())
			m_rings.push_back(ring);
	}

	/// A ring was destroyed. Nothing if it was not registered.
	void Remove(Ring* ring)
	{
		const auto it = std::find(m_rings.begin(), m_rings.end(), ring);
		if (it != m_rings.end())
			m_rings.erase(it);
	}

	/// A ring was move-constructed from `from` into `to`: `from` no longer holds a buffer.
	void Replace(Ring* from, Ring* to)
	{
		const auto it = std::find(m_rings.begin(), m_rings.end(), from);
		if (it != m_rings.end())
			*it = to;
	}

	/// Two rings exchanged their buffers (move assignment): each registered address now names the other's buffer.
	void Swap(Ring* a, Ring* b)
	{
		const auto ia = std::find(m_rings.begin(), m_rings.end(), a);
		const auto ib = std::find(m_rings.begin(), m_rings.end(), b);
		if (ia != m_rings.end() && ib == m_rings.end())
			*ia = b;
		else if (ib != m_rings.end() && ia == m_rings.end())
			*ib = a;
	}

	template <typename F>
	void ForEach(F&& f) const
	{
		for (Ring* r : m_rings)
			f(*r);
	}

	std::size_t Size() const { return m_rings.size(); }

private:
	std::vector<Ring*> m_rings;
};
