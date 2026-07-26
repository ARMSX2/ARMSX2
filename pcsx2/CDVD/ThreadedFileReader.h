// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

class Error;
class ProgressCallback;

/// A file reader for use with compressed formats
/// Calls decompression code on a separate thread to make a synchronous decompression API async
class ThreadedFileReader
{
	ThreadedFileReader(ThreadedFileReader&&) = delete;
protected:
	std::string m_filename;

	u32 m_dataoffset = 0;
	u32 m_blocksize = 2048;

	struct Chunk
	{
		/// Negative block IDs indicate invalid blocks
		s64 chunkID;
		u64 offset;
		u32 length;
	};

	/// Set nonzero to separate block size of read blocks from m_blocksize
	/// Requires that chunk size is a multiple of internal block size
	/// Use to avoid overrunning stack because PCSX2 likes to allocate 2448-byte buffers
	int m_internalBlockSize = 0;

	/// Get the block containing the given offset
	virtual Chunk ChunkForOffset(u64 offset) = 0;
	/// Synchronously read the given block into `dst`
	virtual int ReadChunk(void* dst, s64 chunkID) = 0;
	/// AsyncFileReader open but ThreadedFileReader needs prep work first
	virtual bool Open2(std::string filename, Error* error) = 0;
	/// AsyncFileReader precache but ThreadedFileReader needs prep work first
	virtual bool Precache2(ProgressCallback* progress, Error* error);
	/// AsyncFileReader close but ThreadedFileReader needs prep work first
	virtual void Close2() = 0;
	/// Checks system memory, to ensure that precaching would not exceed a reasonable amount.
	bool CheckAvailableMemoryForPrecaching(u64 required_size, Error* error);

	ThreadedFileReader();

private:
	int m_amtRead;
	/// Pointer to read into
	/// If null when m_requestSize > 0, indicates a request for readahead only
	std::atomic<void*> m_requestPtr{nullptr};
	/// Request offset in (internal block) bytes from the beginning of the file
	u64 m_requestOffset = 0;
	/// Request size in (internal block) bytes
	/// In addition to marking the request size, the loop thread uses this variable to decide whether there's work to do (size of 0 means no work)
	u32 m_requestSize = 0;
	/// Used to cancel requests early
	/// Note: It might take a while for the cancellation request to be noticed, wait until `m_requestPtr` is cleared to ensure it's not being written to
	std::atomic<bool> m_requestCancelled{false};
	struct Buffer
	{
		void* ptr = nullptr;
		u64 offset = 0;
		std::atomic<u32> size{0};
		u32 cap = 0;
	};
	/// Decompressed-chunk cache. For a CHD one entry is one HUNK, so this is the entire cache of
	/// decompressed disc data — at the original 2 entries (current + next) any seek pattern that
	/// revisits data even slightly out of order re-decompresses from scratch, off whatever storage
	/// the image lives on.
	///
	/// ★ Raised from 2 after measuring on a Retroid Pocket 6: Ultimate Spider-Man (a .chd on an SD
	/// card) streams its open city continuously, and after fast-forward — which moves the player
	/// through the world ~4x faster than the streaming budget allows — the GAME stalls for seconds
	/// waiting on disc data. The emulator is provably healthy through it (surface presenting at
	/// 120 Hz, VSync producing new frames, EE running); the guest simply draws nothing and submits
	/// no audio while it waits, which is exactly how it presents to the user: frozen picture,
	/// silence, working UI, and internal FPS reading N/A. Enabling fastCDVD shortened the stall but
	/// did not remove it, which isolates the remaining cost to decompression + storage latency.
	///
	/// Every access below is written against std::size(m_buffer) (search loops and the round-robin
	/// eviction index alike), so this is safe to tune. Entries allocate lazily — an unused slot is
	/// 24 bytes, and a used one is one chunk — so the real cost is bounded by chunks actually
	/// touched, which matters on memory-tight handhelds.
	Buffer m_buffer[8];
	u32 m_nextBuffer = 0;

	std::thread m_readThread;
	std::mutex m_mtx;
	std::condition_variable m_condition;
	/// True to tell the thread to exit
	bool m_quit = false;
	/// True if the thread is currently doing something other than waiting
	/// View while holding `m_mtx`.  If false, you may touch decompression functions from other threads
	bool m_running = false;

	/// Get the internal block size
	u32 InternalBlockSize() const { return m_internalBlockSize ? m_internalBlockSize : m_blocksize; }
	/// memcpy from internal to external blocks
	/// `size` is in internal block bytes
	/// Returns the number of external block bytes copied
	size_t CopyBlocks(void* dst, const void* src, size_t size) const;

	/// Main loop of read thread
	void Loop();

	/// Load the given block into one of the `m_buffer` buffers if necessary and return a pointer to its contents if successful
	Buffer* GetBlockPtr(const Chunk& block);
	/// Decompress from offset to size into
	bool Decompress(void* ptr, u64 offset, u32 size);
	/// Cancel any inflight read and wait until the thread is no longer doing anything
	void CancelAndWaitUntilStopped(void);
	/// Attempt to read from the cache
	/// Adjusts pointer, offset, and size if successful
	/// Returns true if no additional reads are necessary
	bool TryCachedRead(void*& buffer, u64& offset, u32& size, const std::lock_guard<std::mutex>&);

public:
	virtual ~ThreadedFileReader();

	const std::string& GetFilename() const { return m_filename; }
	u32 GetBlockSize() const { return m_blocksize; }

	virtual u32 GetBlockCount() const = 0;


	bool Open(std::string filename, Error* error);
	bool Precache(ProgressCallback* progress, Error* error);
	int ReadSync(void* pBuffer, u32 sector, u32 count);
	void BeginRead(void* pBuffer, u32 sector, u32 count);
	int FinishRead();
	void CancelRead();
	void Close();
	void SetBlockSize(u32 bytes);
	void SetDataOffset(u32 bytes);
};
