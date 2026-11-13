// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "common/ModuleMap.h"

#if defined(__linux__)

#include <atomic>
#include <cstring>
#include <elf.h>
#include <link.h>
#include <mutex>
#include <unistd.h>

#ifndef NT_GNU_BUILD_ID
#define NT_GNU_BUILD_ID 3
#endif

namespace ModuleMap
{
	namespace
	{
		struct Entry
		{
			uptr start; // the lowest loaded segment
			uptr end; // the end of the highest
			uptr bias;
			char name[kNameSize];
			u8 build_id[kMaxBuildIdSize];
			u32 build_id_size;
		};

		struct Table
		{
			std::atomic<u32> seq{0}; // odd while being written
			u32 count = 0;
			u32 dropped = 0;
			Entry entries[kMaxModules];
		};

		// With more modules than entries, the first kKeepFirst (the executable,
		// the dynamic linker, libc and the like) and the latest are kept, and
		// the ones loaded in between are dropped: a driver loads late, and is
		// the one a fault is likeliest to be in.
		static constexpr u32 kKeepFirst = 8;

		struct Fill
		{
			Table* table;
			u32 index = 0; // of the module dl_iterate_phdr is on
			u32 skip_from = 0; // modules [skip_from, skip_to) are dropped
			u32 skip_to = 0;
		};

		int CountModule(dl_phdr_info*, size_t, void* data)
		{
			(*static_cast<u32*>(data))++;
			return 0;
		}

		Table s_tables[2];
		std::atomic<u32> s_active{0};
		std::mutex s_refresh_lock;
		char s_exe_name[kNameSize];

		void CopyBaseName(char (&out)[kNameSize], const char* path)
		{
			const char* base = std::strrchr(path, '/');
			base = base ? base + 1 : path;
			std::strncpy(out, base, kNameSize - 1);
			out[kNameSize - 1] = 0;
		}

		// The executable has no name in dl_iterate_phdr on glibc.
		void ReadExeName()
		{
			if (s_exe_name[0])
				return;
			char path[512];
			const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
			if (n <= 0)
			{
				std::strcpy(s_exe_name, "(executable)");
				return;
			}
			path[n] = 0;
			CopyBaseName(s_exe_name, path);
		}

		// The NT_GNU_BUILD_ID note in a PT_NOTE segment, if there is one.
		void ReadBuildId(const dl_phdr_info* info, const ElfW(Phdr)& note, Entry* e)
		{
			const u8* p = reinterpret_cast<const u8*>(info->dlpi_addr + note.p_vaddr);
			size_t left = note.p_filesz;
			while (left >= sizeof(ElfW(Nhdr)))
			{
				ElfW(Nhdr) nh;
				std::memcpy(&nh, p, sizeof(nh));
				const size_t name_size = (static_cast<size_t>(nh.n_namesz) + 3) & ~static_cast<size_t>(3);
				const size_t desc_size = (static_cast<size_t>(nh.n_descsz) + 3) & ~static_cast<size_t>(3);
				const size_t size = sizeof(nh) + name_size + desc_size;
				if (size > left)
					return;
				const u8* name = p + sizeof(nh);
				if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 && std::memcmp(name, "GNU", 4) == 0 && nh.n_descsz > 0)
				{
					e->build_id_size = nh.n_descsz < kMaxBuildIdSize ? nh.n_descsz : kMaxBuildIdSize;
					std::memcpy(e->build_id, name + name_size, e->build_id_size);
					return;
				}
				p += size;
				left -= size;
			}
		}

		// Only a note inside a loaded segment is mapped.
		bool IsLoaded(const dl_phdr_info* info, const ElfW(Phdr)& note)
		{
			for (int i = 0; i < info->dlpi_phnum; i++)
			{
				const ElfW(Phdr)& ph = info->dlpi_phdr[i];
				if (ph.p_type == PT_LOAD && note.p_vaddr >= ph.p_vaddr &&
					note.p_vaddr + note.p_filesz <= ph.p_vaddr + ph.p_filesz)
				{
					return true;
				}
			}
			return false;
		}

		int AddModule(dl_phdr_info* info, size_t, void* data)
		{
			Fill* f = static_cast<Fill*>(data);
			Table* t = f->table;
			const u32 index = f->index++;
			if ((index >= f->skip_from && index < f->skip_to) || t->count == kMaxModules)
			{
				t->dropped++;
				return 0;
			}

			Entry& e = t->entries[t->count];
			std::memset(&e, 0, sizeof(e));
			e.bias = info->dlpi_addr;
			bool loaded = false;
			for (int i = 0; i < info->dlpi_phnum; i++)
			{
				const ElfW(Phdr)& ph = info->dlpi_phdr[i];
				if (ph.p_type != PT_LOAD || ph.p_memsz == 0)
					continue;
				const uptr start = info->dlpi_addr + ph.p_vaddr;
				const uptr end = start + ph.p_memsz;
				if (!loaded || start < e.start)
					e.start = start;
				if (!loaded || end > e.end)
					e.end = end;
				loaded = true;
			}
			if (!loaded)
				return 0;

			for (int i = 0; i < info->dlpi_phnum && !e.build_id_size; i++)
			{
				if (info->dlpi_phdr[i].p_type == PT_NOTE && IsLoaded(info, info->dlpi_phdr[i]))
					ReadBuildId(info, info->dlpi_phdr[i], &e);
			}

			if (info->dlpi_name && info->dlpi_name[0])
				CopyBaseName(e.name, info->dlpi_name);
			else
				std::memcpy(e.name, s_exe_name, sizeof(e.name));
			t->count++;
			return 0;
		}
	} // namespace

	void Refresh()
	{
		std::lock_guard lock(s_refresh_lock);
		ReadExeName();

		const u32 next = s_active.load(std::memory_order_relaxed) ^ 1;
		Table& t = s_tables[next];
		t.seq.store(t.seq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		t.count = 0;
		t.dropped = 0;
		u32 total = 0;
		dl_iterate_phdr(CountModule, &total);
		Fill fill{&t};
		if (total > kMaxModules)
		{
			fill.skip_from = kKeepFirst;
			fill.skip_to = kKeepFirst + (total - kMaxModules);
		}
		dl_iterate_phdr(AddModule, &fill);
		t.seq.store(t.seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
		s_active.store(next, std::memory_order_release);
	}

	bool Find(uptr address, Hit* hit)
	{
		for (int attempt = 0; attempt < 4; attempt++)
		{
			const Table& t = s_tables[s_active.load(std::memory_order_acquire)];
			const u32 seq = t.seq.load(std::memory_order_acquire);
			if (seq & 1)
				continue;

			bool found = false;
			const u32 count = t.count < kMaxModules ? t.count : kMaxModules;
			for (u32 i = 0; i < count; i++)
			{
				const Entry& e = t.entries[i];
				if (address >= e.start && address < e.end)
				{
					std::memcpy(hit->name, e.name, sizeof(hit->name));
					hit->name[kNameSize - 1] = 0;
					hit->offset = address - e.bias;
					hit->build_id_size = e.build_id_size <= kMaxBuildIdSize ? e.build_id_size : 0;
					std::memcpy(hit->build_id, e.build_id, sizeof(hit->build_id));
					found = true;
					break;
				}
			}

			std::atomic_thread_fence(std::memory_order_acquire);
			if (t.seq.load(std::memory_order_relaxed) == seq)
				return found;
		}
		return false;
	}

	u32 Count()
	{
		return s_tables[s_active.load(std::memory_order_acquire)].count;
	}

	u32 Dropped()
	{
		return s_tables[s_active.load(std::memory_order_acquire)].dropped;
	}
} // namespace ModuleMap

#else

namespace ModuleMap
{
	void Refresh() {}
	bool Find(uptr, Hit*) { return false; }
	u32 Count() { return 0; }
	u32 Dropped() { return 0; }
} // namespace ModuleMap

#endif
