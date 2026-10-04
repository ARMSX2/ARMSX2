// TexturePackZstd.h — decodes a .tar.zst pack as the tar reader asks for bytes
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

// Holds one decoder buffer at a time, so neither the tar nor the pack is ever whole in memory.
class TexturePackZstd
{
public:
	explicit TexturePackZstd(std::FILE* fp)
		: m_fp(fp)
		, m_in(ZSTD_DStreamInSize())
		, m_out(ZSTD_DStreamOutSize())
	{
		m_input.src = m_in.data();
	}

	bool Valid() const { return m_fp && m_dctx; }

	bool Read(void* buffer, size_t n)
	{
		char* dst = static_cast<char*>(buffer);
		while (n > 0)
		{
			if (m_out_pos == m_out_len)
			{
				// A full output buffer can leave decoded bytes behind, so drain before reading more.
				if (m_drained && m_input.pos == m_input.size)
				{
					m_input.size = std::fread(m_in.data(), 1, m_in.size(), m_fp);
					m_input.pos = 0;
					if (m_input.size == 0)
						return false;
				}
				ZSTD_outBuffer output = {m_out.data(), m_out.size(), 0};
				if (ZSTD_isError(ZSTD_decompressStream(m_dctx.get(), &output, &m_input)))
					return false;
				m_out_pos = 0;
				m_out_len = output.pos;
				m_drained = output.pos < output.size;
				continue;
			}
			const size_t take = std::min(n, m_out_len - m_out_pos);
			std::memcpy(dst, m_out.data() + m_out_pos, take);
			m_out_pos += take;
			dst += take;
			n -= take;
		}
		return true;
	}

private:
	std::FILE* m_fp;
	std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> m_dctx{ZSTD_createDCtx(), ZSTD_freeDCtx};
	std::vector<char> m_in, m_out;
	ZSTD_inBuffer m_input = {nullptr, 0, 0};
	size_t m_out_pos = 0, m_out_len = 0;
	bool m_drained = true;
};
