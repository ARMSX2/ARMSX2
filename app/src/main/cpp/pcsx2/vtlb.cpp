// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

/*
	EE physical map :
	[0000 0000,1000 0000) -> Ram (mirrored ?)
	[1000 0000,1400 0000) -> Registers
	[1400 0000,1fc0 0000) -> Reserved (ingored writes, 'random' reads)
	[1fc0 0000,2000 0000) -> Boot ROM

	[2000 0000,4000 0000) -> Unmapped (BUS ERROR)
	[4000 0000,8000 0000) -> "Extended memory", probably unmapped (BUS ERROR) on retail ps2's :)
	[8000 0000,FFFF FFFF] -> Unmapped (BUS ERROR)

	vtlb/phy only supports the [0000 0000,2000 0000) region, with 4k pages.
	vtlb/vmap supports mapping to either of these locations, or some other (externaly) specified address.
*/

#include "Common.h"
#include "vtlb.h"
#include <unistd.h> // [iter73] STDERR_FILENO for signal-safe write
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "COP0.h"
#include "Cache.h"
#include "IopMem.h"
#include "Host.h"
#include "VMManager.h"

#include "common/BitUtils.h"
#include "common/Error.h"

#include "fmt/format.h"

#include "GS/GSVector.h"

#include <bit>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <unordered_set>
#include <unordered_map>

#define FASTMEM_LOG(...)
//#define FASTMEM_LOG(...) Console.WriteLn(__VA_ARGS__)

#ifndef ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
#define ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS 0
#endif

using namespace R5900;
using namespace vtlb_private;

#define verify pxAssert

namespace R5900::Interpreter::OpcodeImpl::COP0
{
	void TLBWI();
	void TLBWR();
}

extern "C" void LogUnified(const char* fmt, ...)
{
	if (Log::ShouldSuppressLegacyProbeMessage(fmt))
		return;

	va_list args;
	va_start(args, fmt);
	std::vfprintf(stderr, fmt, args);
	va_end(args);
	std::fflush(stderr);
}

extern "C" void TraceJitStore(u32 /*pc*/, u32 /*addr*/, int /*mode*/)
{
}

extern "C" void vtlb_LogSIFAccess(u32 /*addr*/, u32 /*pc*/, bool /*is_write*/)
{
}

namespace vtlb_private
{
	alignas(64) MapData vtlbdata;
} // namespace vtlb_private

static vtlbHandler vtlbHandlerCount = 0;

static vtlbHandler DefaultPhyHandler;
static vtlbHandler UnmappedVirtHandler;
static vtlbHandler UnmappedPhyHandler;
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
static int s_tlb_vmap_probe_cfg = -1;
static bool s_tlb_vmap_probe_map_done = false;
static bool s_tlb_vmap_probe_mapbuf_done = false;
static bool s_tlb_vmap_probe_unmap_done = false;
#endif

struct FastmemVirtualMapping
{
	u32 offset;
	u32 size;
};

struct LoadstoreBackpatchInfo
{
	u32 guest_pc;
	u32 gpr_bitmask;
	u32 fpr_bitmask;
	u16 code_size;
	u8 address_register;
	u8 data_register;
	u8 size_in_bits;
	bool is_signed;
	bool is_load;
	bool is_fpr;
};

static constexpr size_t FASTMEM_AREA_SIZE = 0x100000000ULL;
static constexpr u32 FASTMEM_PAGE_COUNT = FASTMEM_AREA_SIZE / VTLB_PAGE_SIZE;
static constexpr u32 NO_FASTMEM_MAPPING = 0xFFFFFFFFu;

static std::unique_ptr<SharedMemoryMappingArea> s_fastmem_area;
static std::vector<u32> s_fastmem_virtual_mapping; // maps vaddr -> mainmem offset
static std::unordered_multimap<u32, u32> s_fastmem_physical_mapping; // maps mainmem offset -> vaddr
static std::unordered_map<uptr, LoadstoreBackpatchInfo> s_fastmem_backpatch_info;
static std::unordered_set<u32> s_fastmem_faulting_pcs;

vtlb_private::VTLBPhysical vtlb_private::VTLBPhysical::fromPointer(sptr ptr)
{
	pxAssertMsg(ptr >= 0, "Address too high");
	return VTLBPhysical(ptr);
}

vtlb_private::VTLBPhysical vtlb_private::VTLBPhysical::fromHandler(vtlbHandler handler)
{
	return VTLBPhysical(handler | POINTER_SIGN_BIT);
}

vtlb_private::VTLBVirtual::VTLBVirtual(VTLBPhysical phys, u32 paddr, u32 vaddr)
{
	pxAssertMsg(0 == (paddr & VTLB_PAGE_MASK), "Should be page aligned");
	pxAssertMsg(0 == (vaddr & VTLB_PAGE_MASK), "Should be page aligned");
	pxAssertMsg((uptr)paddr < POINTER_SIGN_BIT, "Address too high");
	if (phys.isHandler())
	{
		value = phys.raw() + paddr - vaddr;
	}
	else
	{
		value = phys.raw() - vaddr;
	}
}

__inline int CheckCache(u32 addr)
{
	// Check if the cache is enabled
	if (((cpuRegs.CP0.n.Config >> 16) & 0x1) == 0)
	{
		return false;
	}

	size_t i = 0;
	const size_t size = cachedTlbs.count;

	const int stride = 4;

	const GSVector4i addr_vec = GSVector4i::load(addr);

	for (; i + stride <= size; i += stride)
	{
		const GSVector4i pfn1_vec = GSVector4i::load<true>(&cachedTlbs.PFN1s[i]);
		const GSVector4i pfn0_vec = GSVector4i::load<true>(&cachedTlbs.PFN0s[i]);
		const GSVector4i mask_vec = GSVector4i::load<true>(&cachedTlbs.PageMasks[i]);

		const GSVector4i cached1_enable_vec = GSVector4i::load<true>(&cachedTlbs.CacheEnabled1[i]);
		const GSVector4i cached0_enable_vec = GSVector4i::load<true>(&cachedTlbs.CacheEnabled0[i]);

		const GSVector4i cmp1 = addr_vec.ge32(pfn1_vec) & addr_vec.le32(pfn1_vec + mask_vec);
		const GSVector4i cmp0 = addr_vec.ge32(pfn0_vec) & addr_vec.le32(pfn1_vec + mask_vec);

		const GSVector4i lanes_enabled = (cmp1 & cached1_enable_vec) | (cmp0 & cached0_enable_vec);

		if (!lanes_enabled.allfalse())
			return true;
	}

	for (; i < size; i++)
	{
		const u32 mask = cachedTlbs.PageMasks[i];
		if ((cachedTlbs.CacheEnabled1[i] && addr >= cachedTlbs.PFN1s[i] && addr <= cachedTlbs.PFN1s[i] + mask) ||
			(cachedTlbs.CacheEnabled0[i] && addr >= cachedTlbs.PFN0s[i] && addr <= cachedTlbs.PFN0s[i] + mask))
		{
			return true;
		}
	}

	return false;
}
// --------------------------------------------------------------------------------------
// Interpreter Implementations of VTLB Memory Operations.
// --------------------------------------------------------------------------------------
// See recVTLB.cpp for the dynarec versions.

template <typename DataType>
DataType vtlb_memRead(u32 addr)
{
	static const uint DataSize = sizeof(DataType) * 8;
	auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(addr))
	{
		if (!CHECK_EEREC)
		{
			if (CHECK_CACHE && CheckCache(addr))
			{
				switch (DataSize)
				{
					case 8:
						return readCache8(addr);
						break;
					case 16:
						return readCache16(addr);
						break;
					case 32:
						return readCache32(addr);
						break;
					case 64:
						return readCache64(addr);
						break;

						jNO_DEFAULT;
				}
			}
		}

#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
			if (sizeof(DataType) == 8 && addr >= 0x70003D80u && addr < 0x70003DA0u) {
				static int s_spad64r_cfg = -1;
				static int s_spad64r_cnt = 0;
			if (s_spad64r_cfg < 0)
				s_spad64r_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_SPAD64_PROBE", false) ? 1 : 0;
			if (s_spad64r_cfg == 1 && s_spad64r_cnt < 50) {
				u64 curval = *reinterpret_cast<u64*>(vmv.assumePtr(addr));
				Console.WriteLn("@@SPAD64_READ@@ #%d addr=%08x val=%016llx",
						s_spad64r_cnt++, addr, (unsigned long long)curval);
				}
			}
#endif
			return *reinterpret_cast<DataType*>(vmv.assumePtr(addr));
		}

	//has to: translate, find function, call function
	u32 paddr = vmv.assumeHandlerGetPAddr(addr);
	//Console.WriteLn("Translated 0x%08X to 0x%08X", addr,paddr);
	//return reinterpret_cast<TemplateHelper<DataSize,false>::HandlerType*>(vtlbdata.RWFT[TemplateHelper<DataSize,false>::sidx][0][hand])(paddr,data);

	switch (DataSize)
	{
		case 8:
			return vmv.assumeHandler<8, false>()(paddr);
		case 16:
			return vmv.assumeHandler<16, false>()(paddr);
		case 32:
			return vmv.assumeHandler<32, false>()(paddr);
		case 64:
			return vmv.assumeHandler<64, false>()(paddr);

			jNO_DEFAULT;
	}

	return 0; // technically unreachable, but suppresses warnings.
}

RETURNS_R128 vtlb_memRead128(u32 mem)
{
	auto vmv = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(mem))
	{
		if (!CHECK_EEREC)
		{
			if (CHECK_CACHE && CheckCache(mem))
			{
				return readCache128(mem);
			}
		}

		return r128_load(reinterpret_cast<const void*>(vmv.assumePtr(mem)));
	}
	else
	{
		//has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(mem);
		//Console.WriteLn("Translated 0x%08X to 0x%08X", addr,paddr);
		return vmv.assumeHandler<128, false>()(paddr);
	}
}

template <typename DataType>
void vtlb_memWrite(u32 addr, DataType data)
{
	static const uint DataSize = sizeof(DataType) * 8;

#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
		if (sizeof(DataType) == 8 && addr == 0x70003F80u) {
			static int s_w64_cnt = 0;
			if (s_w64_cnt < 50) {
			Console.WriteLn("[TEMP_DIAG] @@SCRATCH_3F80@@ #%d pc=%08x val=%016llx",
				s_w64_cnt++, cpuRegs.pc, (unsigned long long)(u64)data);
		}
	}

		{
			const u32 phys_ef = addr & 0x1FFFFFFFu;
			if (phys_ef >= 0x0000ef20u && phys_ef <= 0x0000ef44u) {
			static int s_wef_n = 0;
			if (s_wef_n < 10)
				Console.WriteLn("@@WRITE_EF30@@ n=%d pc=%08x addr=%08x phys=%08x sz=%u data=%08x",
					s_wef_n++, cpuRegs.pc, addr, phys_ef, DataSize, (u32)(u64)data);
		}
	}

		{
			const u32 phys154 = addr & 0x1FFFFFFFu;
			if (phys154 == 0x000154a4u) {
			static int s_w154_n = 0;
			if (s_w154_n < 50)
				Console.WriteLn("@@WRITE_154A4@@ n=%d pc=%08x data=%08x ra=%08x",
					s_w154_n++, cpuRegs.pc, (u32)(u64)data,
					cpuRegs.GPR.n.ra.UL[0]);
		}
	}

		{
			u32 phys_sif = addr & 0x1FFFFFFFu;
			if (phys_sif >= 0x00021380u && phys_sif < 0x000213C0u) {
			static int s_wsif_n = 0;
			if (s_wsif_n < 50) {
				const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
				Console.WriteLn("@@WRITE_SIF_TAG@@ [%s] n=%d pc=%08x addr=%08x sz=%u data=%016llx ra=%08x sp=%08x "
					"a3=%08x t0=%08x t1=%08x s1=%08x a0=%08x a1=%08x a2=%08x v0=%08x",
					mode, s_wsif_n++, cpuRegs.pc, addr, DataSize, (unsigned long long)(u64)data,
					cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.sp.UL[0],
					cpuRegs.GPR.n.a3.UL[0], cpuRegs.GPR.n.t0.UL[0],
					cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.s1.UL[0],
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.v0.UL[0]);
			}
		}
	}

		{
			u32 phys_ev = addr & 0x1FFFFFFFu;
			if (phys_ev == 0x00000080u || phys_ev == 0x00000180u ||
		    phys_ev == 0x00000084u || phys_ev == 0x00000184u) {
			static int s_wev_n = 0;
			if (s_wev_n < 30) {
				Console.WriteLn("@@WRITE_EXCVEC@@ n=%d pc=%08x addr=%08x phys=%08x sz=%u data=%08x cyc=%u ra=%08x",
					s_wev_n++, cpuRegs.pc, addr, phys_ev, DataSize, (u32)(u64)data,
					cpuRegs.cycle, cpuRegs.GPR.n.ra.UL[0]);
			}
		}
	}

		{
			u32 phys82 = addr & 0x1FFFFFFFu;
			if (phys82 >= 0x00082000u && phys82 <= 0x00082010u) {
			static int s_w82_n = 0;
			if (s_w82_n < 10) {
				const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
				Console.WriteLn("@@WRITE_82000@@ [%s] n=%d pc=%08x addr=%08x sz=%u data=%08x a0=%08x a1=%08x a2=%08x a3=%08x v0=%08x v1=%08x",
					mode, s_w82_n++, cpuRegs.pc, addr, DataSize, (u32)(u64)data,
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.a3.UL[0],
					cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0]);
			}
		}
	}

		{
			u32 phys99 = addr & 0x1FFFFFFFu;
			if (phys99 >= 0x000991F0u && phys99 < 0x00099200u) {
			static int s_w99_n = 0;
			if (s_w99_n < 20) {
				const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
				Console.WriteLn("@@WRITE_991F0@@ [%s] n=%d pc=%08x addr=%08x phys=%08x sz=%u data=%08x ra=%08x",
					s_w99_n++, mode, cpuRegs.pc, addr, phys99, DataSize, (u32)(u64)data,
					cpuRegs.GPR.n.ra.UL[0]);
			}
		}
	}

		{
			u32 phys = addr & 0x1FFFFFFFu;
			if (phys >= 0x00078000u && phys < 0x00079000u) {
			static u32 s_kseg3w_n = 0;
			if (s_kseg3w_n < 30) {
				Console.WriteLn("@@KSEG3_WRITE_WATCH@@ n=%u pc=%08x addr=%08x phys=%08x sz=%u data=%08x",
					s_kseg3w_n, cpuRegs.pc, addr, phys, DataSize,
					(u32)(u64)data);
				s_kseg3w_n++;
			}
		}
		// @@WATCH_19DC@@ 物理 0x19D8-0x19DF (3c03aaaa 保護対象range) への書き込み監視
		// Removal condition: eeRam19dc=3c03aaaa が JIT でverifyされた後
			if (phys >= 0x19D8u && phys <= 0x19DFu) {
				static int s_w19dc_n = 0;
				if (s_w19dc_n < 20)
				Console.WriteLn("@@WATCH_19DC@@ n=%d pc=%08x addr=%08x phys=%08x sz=%u data=%08x",
						s_w19dc_n++, cpuRegs.pc, addr, phys, DataSize, (u32)(u64)data);
			}
		}
#endif

		auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(addr))
	{
		if (!CHECK_EEREC)
		{
			if (CHECK_CACHE && CheckCache(addr))
			{
				switch (DataSize)
				{
					case 8:
						writeCache8(addr, data);
						return;
					case 16:
						writeCache16(addr, data);
						return;
					case 32:
						writeCache32(addr, data);
						return;
					case 64:
						writeCache64(addr, data);
						return;
				}
			}
		}

#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
			if ((addr >= 0x00001000u && addr < 0x02000000u) ||
			    (addr >= 0x80001000u && addr < 0x82000000u) ||
			    (addr >= 0xA0001000u && addr < 0xA2000000u)) {
			static bool s_eeramW_seen = false;
			if (!s_eeramW_seen) {
				s_eeramW_seen = true;
				Console.WriteLn("@@EERAM_FIRST_WRITE@@ direct addr=%08x ee_pc=%08x ra=%08x sp=%08x sz=%u",
						addr, cpuRegs.pc, cpuRegs.GPR.r[31].UL[0], cpuRegs.GPR.r[29].UL[0],
						(u32)sizeof(DataType));
				}
			}

			{
				u32 waddr = addr & 0x1FFFFFFFu;
				if (waddr >= 0x081fe0u && waddr < 0x081ff4u) {
				static int s_sw_n = 0;
				if (s_sw_n++ < 30) {
					u64 dval = 0;
					memcpy(&dval, &data, sizeof(DataType) < 8 ? sizeof(DataType) : 8);
					Console.WriteLn("@@STUB_WRITE_WATCH@@ n=%d addr=%08x paddr=%08x pc=%08x ra=%08x data=0x%llx sz=%u cycle=%u",
						s_sw_n, addr, waddr, cpuRegs.pc, cpuRegs.GPR.r[31].UL[0],
						(unsigned long long)dval, (u32)sizeof(DataType), cpuRegs.cycle);
					}
				}
			}
#endif
			*reinterpret_cast<DataType*>(vmv.assumePtr(addr)) = data;
		}
		else
		{
			//has to: translate, find function, call function
			u32 paddr = vmv.assumeHandlerGetPAddr(addr);
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
			{
				if (paddr >= 0x081fe0u && paddr < 0x081ff4u) {
					static int s_swh_n = 0;
				if (s_swh_n++ < 30) {
					Console.WriteLn("@@STUB_WRITE_WATCH_H@@ n=%d addr=%08x paddr=%08x pc=%08x ra=%08x sz=%u cycle=%u",
						s_swh_n, addr, paddr, cpuRegs.pc, cpuRegs.GPR.r[31].UL[0],
						(u32)sizeof(DataType), cpuRegs.cycle);
					}
				}
			}

			if (paddr >= 0x00001000u && paddr < 0x02000000u) {
				static bool s_eeramH_seen = false;
				if (!s_eeramH_seen) {
				s_eeramH_seen = true;
				Console.WriteLn("@@EERAM_FIRST_WRITE@@ handler addr=%08x paddr=%08x ee_pc=%08x ra=%08x sz=%u",
						addr, paddr, cpuRegs.pc, cpuRegs.GPR.r[31].UL[0], (u32)sizeof(DataType));
				}
			}
#endif
			return vmv.assumeHandler<sizeof(DataType) * 8, true>()(paddr, data);
		}
	}

void TAKES_R128 vtlb_memWrite128(u32 mem, r128 value)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
		{
			u32 phys128 = mem & 0x1FFFFFFFu;
			if (phys128 >= 0x00021380u && phys128 < 0x000213C0u) {
			static int s_w128sif_n = 0;
			if (s_w128sif_n < 30) {
				u32 v128[4]; memcpy(v128, &value, 16);
				const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
				Console.WriteLn("@@WRITE128_SIF_TAG@@ [%s] n=%d pc=%08x addr=%08x val=%08x_%08x_%08x_%08x ra=%08x",
					mode, s_w128sif_n++, cpuRegs.pc, mem, v128[0], v128[1], v128[2], v128[3],
					cpuRegs.GPR.n.ra.UL[0]);
				}
			}
		}

		{
			u32 phys = mem & 0x1FFFFFFFu;
			if (phys >= 0x19D0u && phys <= 0x19FFu) {
			static int s_sq19_n = 0;
			if (s_sq19_n < 20) {
				u32 vals[4];
				memcpy(vals, &value, 16);
				Console.WriteLn("@@SQ_WRITE128_19D0@@ n=%d pc=%08x addr=%08x phys=%08x val[0]=%08x[1]=%08x[2]=%08x[3]=%08x",
					s_sq19_n++, cpuRegs.pc, mem, phys, vals[0], vals[1], vals[2], vals[3]);
				}
			}
		}

		{
			u32 phys128 = mem & 0x1FFFFFFFu;
			if (phys128 < 0x00000200u) {
			static int s_w128ev_n = 0;
			if (s_w128ev_n < 20) {
				u32 v128[4]; memcpy(v128, &value, 16);
				Console.WriteLn("@@WRITE128_EXCVEC@@ n=%d pc=%08x addr=%08x phys=%08x val=%08x_%08x_%08x_%08x cyc=%u",
					s_w128ev_n++, cpuRegs.pc, mem, phys128, v128[0], v128[1], v128[2], v128[3], cpuRegs.cycle);
				}
			}
		}
#endif


	auto vmv = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(mem))
	{
		if (!CHECK_EEREC)
		{
			if (CHECK_CACHE && CheckCache(mem))
			{
				alignas(16) const u128 r = r128_to_u128(value);
				writeCache128(mem, &r);
				return;
			}
		}

		r128_store_unaligned((void*)vmv.assumePtr(mem), value);
	}
	else
	{
		//has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(mem);
		//Console.WriteLn("Translated 0x%08X to 0x%08X", addr,paddr);

		vmv.assumeHandler<128, true>()(paddr, value);
	}
}

template mem8_t vtlb_memRead<mem8_t>(u32 mem);
template mem16_t vtlb_memRead<mem16_t>(u32 mem);
template mem32_t vtlb_memRead<mem32_t>(u32 mem);
template mem64_t vtlb_memRead<mem64_t>(u32 mem);
template void vtlb_memWrite<mem8_t>(u32 mem, mem8_t data);
template void vtlb_memWrite<mem16_t>(u32 mem, mem16_t data);
template void vtlb_memWrite<mem32_t>(u32 mem, mem32_t data);
template void vtlb_memWrite<mem64_t>(u32 mem, mem64_t data);

template <typename DataType>
bool vtlb_ramRead(u32 addr, DataType* value)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
	{
		std::memset(value, 0, sizeof(DataType));
		return false;
	}

	std::memcpy(value, reinterpret_cast<DataType*>(vmv.assumePtr(addr)), sizeof(DataType));
	return true;
}

template <typename DataType>
bool vtlb_ramWrite(u32 addr, const DataType& data)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
		return false;

	std::memcpy(reinterpret_cast<DataType*>(vmv.assumePtr(addr)), &data, sizeof(DataType));
	return true;
}


template bool vtlb_ramRead<mem8_t>(u32 mem, mem8_t* value);
template bool vtlb_ramRead<mem16_t>(u32 mem, mem16_t* value);
template bool vtlb_ramRead<mem32_t>(u32 mem, mem32_t* value);
template bool vtlb_ramRead<mem64_t>(u32 mem, mem64_t* value);
template bool vtlb_ramRead<mem128_t>(u32 mem, mem128_t* value);
template bool vtlb_ramWrite<mem8_t>(u32 mem, const mem8_t& data);
template bool vtlb_ramWrite<mem16_t>(u32 mem, const mem16_t& data);
template bool vtlb_ramWrite<mem32_t>(u32 mem, const mem32_t& data);
template bool vtlb_ramWrite<mem64_t>(u32 mem, const mem64_t& data);
template bool vtlb_ramWrite<mem128_t>(u32 mem, const mem128_t& data);

int vtlb_memSafeCmpBytes(u32 mem, const void* src, u32 size)
{
	// can memcpy so long as pages aren't crossed
	const u8* sptr = static_cast<const u8*>(src);
	const u8* const sptr_end = sptr + size;
	while (sptr != sptr_end)
	{
		auto vmv = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];
		if (vmv.isHandler(mem))
			return -1;

		const size_t remaining_in_page =
			std::min(VTLB_PAGE_SIZE - (mem & VTLB_PAGE_MASK), static_cast<u32>(sptr_end - sptr));
		const int res = std::memcmp(sptr, reinterpret_cast<void*>(vmv.assumePtr(mem)), remaining_in_page);
		if (res != 0)
			return res;

		sptr += remaining_in_page;
		mem += remaining_in_page;
	}

	return 0;
}

bool vtlb_memSafeReadBytes(u32 mem, void* dst, u32 size)
{
	// can memcpy so long as pages aren't crossed
	u8* dptr = static_cast<u8*>(dst);
	u8* const dptr_end = dptr + size;
	while (dptr != dptr_end)
	{
		auto vmv = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];
		if (vmv.isHandler(mem))
			return false;

		const u32 remaining_in_page =
			std::min(VTLB_PAGE_SIZE - (mem & VTLB_PAGE_MASK), static_cast<u32>(dptr_end - dptr));
		std::memcpy(dptr, reinterpret_cast<void*>(vmv.assumePtr(mem)), remaining_in_page);
		dptr += remaining_in_page;
		mem += remaining_in_page;
	}

	return true;
}

bool vtlb_memSafeWriteBytes(u32 mem, const void* src, u32 size)
{
	// can memcpy so long as pages aren't crossed
	const u8* sptr = static_cast<const u8*>(src);
	const u8* const sptr_end = sptr + size;
	while (sptr != sptr_end)
	{
		auto vmv = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];
		if (vmv.isHandler(mem))
			return false;

		const size_t remaining_in_page =
			std::min(VTLB_PAGE_SIZE - (mem & VTLB_PAGE_MASK), static_cast<u32>(sptr_end - sptr));
		std::memcpy(reinterpret_cast<void*>(vmv.assumePtr(mem)), sptr, remaining_in_page);
		sptr += remaining_in_page;
		mem += remaining_in_page;
	}

	return true;
}

// --------------------------------------------------------------------------------------
//  TLB Miss / BusError Handlers
// --------------------------------------------------------------------------------------
// These are valid VM memory errors that should typically be handled by the VM itself via
// its own cpu exception system.
//
// [TODO]  Add first-chance debugging hooks to these exceptions!
//
// Important recompiler note: Mid-block Exception handling isn't reliable *yet* because
// memory ops don't flush the PC prior to invoking the indirect handlers.


static void GoemonTlbMissDebug()
{
	// 0x3d5580 is the address of the TLB cache
	GoemonTlb* tlb = (GoemonTlb*)&eeMem->Main[0x3d5580];

	for (u32 i = 0; i < 150; i++)
	{
		if (tlb[i].valid == 0x1 && tlb[i].low_add != tlb[i].high_add)
			DevCon.WriteLn("GoemonTlbMissDebug: Entry %d is valid. Key %x. From V:0x%8.8x to V:0x%8.8x (P:0x%8.8x)", i, tlb[i].key, tlb[i].low_add, tlb[i].high_add, tlb[i].physical_add);
		else if (tlb[i].low_add != tlb[i].high_add)
			DevCon.WriteLn("GoemonTlbMissDebug: Entry %d is invalid. Key %x. From V:0x%8.8x to V:0x%8.8x (P:0x%8.8x)", i, tlb[i].key, tlb[i].low_add, tlb[i].high_add, tlb[i].physical_add);
	}
}

void GoemonPreloadTlb()
{
	// 0x3d5580 is the address of the TLB cache table
	GoemonTlb* tlb = (GoemonTlb*)&eeMem->Main[0x3d5580];

	for (u32 i = 0; i < 150; i++)
	{
		if (tlb[i].valid == 0x1 && tlb[i].low_add != tlb[i].high_add)
		{

			u32 size = tlb[i].high_add - tlb[i].low_add;
			u32 vaddr = tlb[i].low_add;
			u32 paddr = tlb[i].physical_add;

			// TODO: The old code (commented below) seems to check specifically for handler 0.  Is this really correct?
			//if ((uptr)vtlbdata.vmap[vaddr>>VTLB_PAGE_BITS] == POINTER_SIGN_BIT) {
			auto vmv = vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS];
			if (vmv.isHandler(vaddr) && vmv.assumeHandlerGetID() == 0)
			{
				DevCon.WriteLn("GoemonPreloadTlb: Entry %d. Key %x. From V:0x%8.8x to P:0x%8.8x (%d pages)", i, tlb[i].key, vaddr, paddr, size >> VTLB_PAGE_BITS);
				vtlb_VMap(vaddr, paddr, size);
				vtlb_VMap(0x20000000 | vaddr, paddr, size);
			}
		}
	}
}

void GoemonUnloadTlb(u32 key)
{
	// 0x3d5580 is the address of the TLB cache table
	GoemonTlb* tlb = (GoemonTlb*)&eeMem->Main[0x3d5580];
	for (u32 i = 0; i < 150; i++)
	{
		if (tlb[i].key == key)
		{
			if (tlb[i].valid == 0x1)
			{
				u32 size = tlb[i].high_add - tlb[i].low_add;
				u32 vaddr = tlb[i].low_add;
				DevCon.WriteLn("GoemonUnloadTlb: Entry %d. Key %x. From V:0x%8.8x to V:0x%8.8x (%d pages)", i, tlb[i].key, vaddr, vaddr + size, size >> VTLB_PAGE_BITS);

				vtlb_VMapUnmap(vaddr, size);
				vtlb_VMapUnmap(0x20000000 | vaddr, size);

				// Unmap the tlb in game cache table
				// Note: Game copy FEFEFEFE for others data
				tlb[i].valid = 0;
				tlb[i].key = 0xFEFEFEFE;
				tlb[i].low_add = 0xFEFEFEFE;
				tlb[i].high_add = 0xFEFEFEFE;
			}
			else
			{
				DevCon.Error("GoemonUnloadTlb: Entry %d is not valid. Key %x", i, tlb[i].key);
			}
		}
	}
}

// Generates a tlbMiss Exception
static __ri void vtlb_Miss(u32 addr, u32 mode)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	static bool s_tlbmiss_1shot_done = false;
	if (!s_tlbmiss_1shot_done && mode == 0 && cpuRegs.pc == 0x9fc437e8 && (addr & 0xffff0000u) == 0x70000000u)
	{
		s_tlbmiss_1shot_done = true;
		Console.Error("@@TLBMISS_1SHOT@@ pc=%08x addr=%08x isDevBuild=%d cause=%08x sp=%08x ra=%08x a0=%08x a1=%08x t0=%08x t1=%08x entryHi=%08x entryLo0=%08x entryLo1=%08x index=%08x badVAddr=%08x",
			cpuRegs.pc, addr, IsDevBuild ? 1 : 0, cpuRegs.CP0.n.Cause,
			cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
			cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0],
			cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.Index, cpuRegs.CP0.n.BadVAddr);
		u32 code = 0;
		if (const void* p = vtlb_GetPhyPtr(cpuRegs.pc & 0x1fffffff))
		{
			std::memcpy(&code, p, sizeof(code));
			Console.Error("@@TLBMISS_INS@@ pc=%08x code=%08x", cpuRegs.pc, code);
		}
	}
#endif
	[[maybe_unused]] static int s_tlb_after_probe_cfg = -1;
	[[maybe_unused]] static bool s_tlb_after_probe_done = false;
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	if (s_tlb_after_probe_cfg < 0)
	{
		s_tlb_after_probe_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_TLB_AFTER_PROBE", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_TLB_AFTER_PROBE=%d", s_tlb_after_probe_cfg);
	}
	const bool do_tlb_after_probe =
		(s_tlb_after_probe_cfg == 1 && !s_tlb_after_probe_done && mode == 0 && cpuRegs.pc == 0x9fc437e8 && (addr & 0xFFFFF000u) == 0x70004000u);
#else
	const bool do_tlb_after_probe = false;
#endif

	if (EmuConfig.Gamefixes.GoemonTlbHack)
		GoemonTlbMissDebug();

	// Hack to handle expected tlb miss by some games.
	if (Cpu == &intCpu)
	{
		if (do_tlb_after_probe)
		{
			Console.Error("@@TLB_BEFORE@@ pc=%08x addr=%08x entryHi=%08x entryLo0=%08x entryLo1=%08x index=%08x status=%08x cause=%08x badVAddr=%08x",
				cpuRegs.pc, addr, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.Index,
				cpuRegs.CP0.n.Status, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.BadVAddr);
		}
		// [P9 TEMP_DIAG] EE RAM (VA 0-32MB) TLB miss を identity mapping で自動解決。
		// BIOS TLBL handler (0xBFC004B8) は NOP loop のため、ここに到達すると EE が
		// 永久に止まる。real R5900 では BIOS kernel が TLB flush 後に EE RAM を直接
		// アクセスする前に必ず mapping を再configするはずだが、interpreterでは
		// タイミングの差でそれが起きない。vtlb identity mapping を install してリトライ。
			// Removal condition: P9 interpreter参照トレース取得after completed。
			if (addr < Ps2MemSize::MainRam) {
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
				static u32 s_automap_cap = 0;
				if (s_automap_cap < 20) {
					// cpuRegs.code = instruction that caused this miss (set by execI before execution)
					Console.WriteLn("@@INTERP_AUTOMAP@@ pc=%08x addr=%08x mode=%u code=%08x [%u/20]",
						cpuRegs.pc, addr, mode, cpuRegs.code, ++s_automap_cap);
				}
#endif
				const u32 page_va = addr & ~VTLB_PAGE_MASK;
				vtlb_VMap(page_va, page_va, VTLB_PAGE_SIZE);
				Cpu->CancelInstruction();
			return;
		}

		if (mode)
			cpuTlbMissW(addr, cpuRegs.branch);
		else
			cpuTlbMissR(addr, cpuRegs.branch);
		if (do_tlb_after_probe)
		{
			Console.Error("@@TLB_AFTER@@ pc=%08x addr=%08x entryHi=%08x entryLo0=%08x entryLo1=%08x index=%08x status=%08x cause=%08x badVAddr=%08x",
				cpuRegs.pc, addr, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.Index,
				cpuRegs.CP0.n.Status, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.BadVAddr);
			const u32 probe_vaddr = 0x70004000u;
			const auto vmv_after = vtlbdata.vmap[probe_vaddr >> VTLB_PAGE_BITS];
			const bool is_handler_after = vmv_after.isHandler(probe_vaddr);
			const u32 handler_id_after = is_handler_after ? vmv_after.assumeHandlerGetID() : 0xFFFFFFFFu;
			const u32 paddr_after = is_handler_after ? vmv_after.assumeHandlerGetPAddr(probe_vaddr) : 0;
			const u32 ppmap_after = vtlbdata.ppmap ? vtlbdata.ppmap[probe_vaddr >> VTLB_PAGE_BITS] : 0xFFFFFFFFu;
			const bool mapped_after = (!is_handler_after || handler_id_after != static_cast<u32>(UnmappedVirtHandler));
			Console.Error("@@TLB_MAP_AFTER@@ vaddr=%08x mapped=%d is_handler=%d handler=%08x paddr=%08x ppmap=%08x vmraw=%016llx",
				probe_vaddr, mapped_after ? 1 : 0, is_handler_after ? 1 : 0, handler_id_after, paddr_after, ppmap_after,
				static_cast<unsigned long long>(vmv_after.raw()));
			s_tlb_after_probe_done = true;
		}

		// Exception handled. Current instruction need to be stopped
		Cpu->CancelInstruction();
		return;
	}

	static int s_tlbmiss_recomp_exception_cfg = -1;
	static int s_tlb_recomp_refill_probe_cfg = -1;
	static int s_tlb_recomp_synth_tlbwi_cfg = -1;
	static bool s_tlb_recomp_refill_probe_done = false;
	static u32 s_tlb_recomp_synth_tlbwi_count = 0;
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	if (s_tlbmiss_recomp_exception_cfg < 0)
	{
		s_tlbmiss_recomp_exception_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_TLBMISS_RECOMP_EXCEPTION", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_TLBMISS_RECOMP_EXCEPTION=%d", s_tlbmiss_recomp_exception_cfg);
	}
	if (s_tlb_recomp_refill_probe_cfg < 0)
	{
		s_tlb_recomp_refill_probe_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_TLB_RECOMP_REFILL_PROBE", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_TLB_RECOMP_REFILL_PROBE=%d", s_tlb_recomp_refill_probe_cfg);
	}
	if (s_tlb_recomp_synth_tlbwi_cfg < 0)
	{
		s_tlb_recomp_synth_tlbwi_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_TLBMISS_RECOMP_SYNTH_TLBWI", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_TLBMISS_RECOMP_SYNTH_TLBWI=%d", s_tlb_recomp_synth_tlbwi_cfg);
	}
#endif
	if (s_tlbmiss_recomp_exception_cfg == 1)
	{
		const bool do_tlb_recomp_refill_probe =
			(s_tlb_recomp_refill_probe_cfg == 1 && !s_tlb_recomp_refill_probe_done && (addr & 0xFFFFF000u) == 0x70004000u);
		const u32 miss_page = addr >> VTLB_PAGE_BITS;
		const auto vmv_before = vtlbdata.vmap[miss_page];
		if (do_tlb_recomp_refill_probe)
		{
			Console.Error("@@TLB_RECOMP_BEFORE@@ pc=%08x addr=%08x isLoad=%d entryHi=%08x entryLo0=%08x entryLo1=%08x index=%08x status=%08x cause=%08x badVAddr=%08x vmap_raw=%016llx",
				cpuRegs.pc, addr, mode ? 0 : 1, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.Index,
				cpuRegs.CP0.n.Status, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.BadVAddr, static_cast<unsigned long long>(vmv_before.raw()));
		}
		if (do_tlb_after_probe)
		{
			Console.Error("@@TLB_BEFORE@@ pc=%08x addr=%08x entryHi=%08x entryLo0=%08x entryLo1=%08x index=%08x status=%08x cause=%08x badVAddr=%08x",
				cpuRegs.pc, addr, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.Index,
				cpuRegs.CP0.n.Status, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.BadVAddr);
		}
		if (mode)
			cpuTlbMissW(addr, cpuRegs.branch);
		else
			cpuTlbMissR(addr, cpuRegs.branch);
		if (s_tlb_recomp_synth_tlbwi_cfg == 1 && (addr & 0xFFFF0000u) == 0x70000000u)
		{
			const auto vmv_after_miss = vtlbdata.vmap[miss_page];
			const bool is_handler_after_miss = vmv_after_miss.isHandler(addr);
			const u32 handler_id_after_miss = is_handler_after_miss ? vmv_after_miss.assumeHandlerGetID() : static_cast<u32>(UnmappedVirtHandler);
			const bool mapped_after_miss = (!is_handler_after_miss || handler_id_after_miss != static_cast<u32>(UnmappedVirtHandler));
			if (!mapped_after_miss)
			{
				R5900::Interpreter::OpcodeImpl::COP0::TLBWR();
				if (s_tlb_recomp_synth_tlbwi_count < 8)
				{
					Console.Error("@@TLB_RECOMP_SYNTH_TLBWI@@ n=%u pc=%08x addr=%08x random=%08x entryHi=%08x entryLo0=%08x entryLo1=%08x",
						s_tlb_recomp_synth_tlbwi_count, cpuRegs.pc, addr, cpuRegs.CP0.n.Random, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);
				}
				s_tlb_recomp_synth_tlbwi_count++;
			}
		}
		if (do_tlb_recomp_refill_probe)
		{
			const auto vmv_after_recomp = vtlbdata.vmap[miss_page];
			const bool is_handler_after_recomp = vmv_after_recomp.isHandler(addr);
			const u32 handler_id_after_recomp = is_handler_after_recomp ? vmv_after_recomp.assumeHandlerGetID() : static_cast<u32>(UnmappedVirtHandler);
			const bool mapped_now = (!is_handler_after_recomp || handler_id_after_recomp != static_cast<u32>(UnmappedVirtHandler));
			const char* kind = mapped_now ? (is_handler_after_recomp ? "handler" : "direct") : "none";
			Console.Error("@@TLB_RECOMP_AFTER@@ pc=%08x addr=%08x isLoad=%d entryHi=%08x entryLo0=%08x entryLo1=%08x index=%08x status=%08x cause=%08x badVAddr=%08x vmap_raw=%016llx",
				cpuRegs.pc, addr, mode ? 0 : 1, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.Index,
				cpuRegs.CP0.n.Status, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.BadVAddr, static_cast<unsigned long long>(vmv_after_recomp.raw()));
			Console.Error("@@TLB_RECOMP_MAP_AFTER@@ pc=%08x addr=%08x mapped_now=%d kind=%s",
				cpuRegs.pc, addr, mapped_now ? 1 : 0, kind);
			s_tlb_recomp_refill_probe_done = true;
		}
		if (do_tlb_after_probe)
		{
			Console.Error("@@TLB_AFTER@@ pc=%08x addr=%08x entryHi=%08x entryLo0=%08x entryLo1=%08x index=%08x status=%08x cause=%08x badVAddr=%08x",
				cpuRegs.pc, addr, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.Index,
				cpuRegs.CP0.n.Status, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.BadVAddr);
			const u32 probe_vaddr = 0x70004000u;
			const auto vmv_after = vtlbdata.vmap[probe_vaddr >> VTLB_PAGE_BITS];
			const bool is_handler_after = vmv_after.isHandler(probe_vaddr);
			const u32 handler_id_after = is_handler_after ? vmv_after.assumeHandlerGetID() : 0xFFFFFFFFu;
			const u32 paddr_after = is_handler_after ? vmv_after.assumeHandlerGetPAddr(probe_vaddr) : 0;
			const u32 ppmap_after = vtlbdata.ppmap ? vtlbdata.ppmap[probe_vaddr >> VTLB_PAGE_BITS] : 0xFFFFFFFFu;
			const bool mapped_after = (!is_handler_after || handler_id_after != static_cast<u32>(UnmappedVirtHandler));
			Console.Error("@@TLB_MAP_AFTER@@ vaddr=%08x mapped=%d is_handler=%d handler=%08x paddr=%08x ppmap=%08x vmraw=%016llx",
				probe_vaddr, mapped_after ? 1 : 0, is_handler_after ? 1 : 0, handler_id_after, paddr_after, ppmap_after,
				static_cast<unsigned long long>(vmv_after.raw()));
			s_tlb_after_probe_done = true;
		}
		Cpu->ExitExecution();
		return;
	}

	static int s_tlbmiss_mirror_cfg = -1;
		if (s_tlbmiss_mirror_cfg < 0)
		{
			s_tlbmiss_mirror_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_TLBMISS_MIRROR_SPR_70004000", true) ? 1 : 0;
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
			Console.WriteLn("@@CFG@@ iPSX2_TLBMISS_MIRROR_SPR_70004000=%d", s_tlbmiss_mirror_cfg);
#endif
		}
		const u32 miss_page = addr & 0xFFFFF000u;
		if (s_tlbmiss_mirror_cfg == 1 && miss_page >= 0x70000000u && miss_page <= 0x700FFFFFu)
		{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
			static bool s_tlbmiss_mirror_logged = false;
#endif
			static u32 s_tlbmiss_mirror_count = 0;
			if (s_tlbmiss_mirror_count < 1024)
			{
				const u32 spr_off = miss_page & (Ps2MemSize::Scratch - 1u);
				vtlb_VMapBuffer(miss_page, eeMem->Scratch + spr_off, VTLB_PAGE_SIZE);
				s_tlbmiss_mirror_count++;
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
				if (!s_tlbmiss_mirror_logged)
				{
					s_tlbmiss_mirror_logged = true;
					Console.Error("@@TLBMISS_MIRROR@@ pc=%08x addr=%08x mapped=1 mode=page", cpuRegs.pc, addr);
				}
#endif
			}
			return;
		}

	const std::string message(fmt::format("TLB Miss, pc=0x{:x} addr=0x{:x} [{}]", cpuRegs.pc, addr, mode ? "store" : "load"));
	if (EmuConfig.Cpu.Recompiler.PauseOnTLBMiss)
	{
		// Pause, let the user try to figure out what went wrong in the debugger.
		Host::ReportErrorAsync("R5900 Exception", message);
		VMManager::SetPaused(true);
		Cpu->ExitExecution();
		return;
	}

	static int spamStop = 0;
	static int s_tlbmiss_spam = -1;
	if (s_tlbmiss_spam < 0)
		s_tlbmiss_spam = iPSX2_GetRuntimeEnvBool("iPSX2_TLBMISS_SPAM", false) ? 1 : 0;
	if (spamStop++ < 50 || s_tlbmiss_spam == 1)
		Console.Error(message);
}

// BusError exception: more serious than a TLB miss.  If properly emulated the PS2 kernel
// itself would invoke a diagnostic/assertion screen that displays the cpu state at the
// time of the exception.
static __ri void vtlb_BusError(u32 addr, u32 mode)
{
	// [iter222] BIOS writes to unmapped IO ports (e.g. 0xB000F180→phys 0x1000F180).
	// On real PS2 hardware, stores to unmapped addresses are silently ignored.
	// Suppress Bus Error for store mode to prevent BIOS init failure.
	// Removal condition: vtlb handlermappingが実機相当にfixされた後
	if (mode == 1) {
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
		static std::atomic<int> s_be_store_count{0};
		int n = s_be_store_count.fetch_add(1, std::memory_order_relaxed);
		if (n < 5) {
			Console.WriteLn("@@BUS_ERROR_STORE_SKIP@@ n=%d addr=%08x ee_pc=%08x (silently ignored)",
				n, addr, cpuRegs.pc);
		}
#endif
		return;
	}

	const std::string message(fmt::format("Bus Error, addr=0x{:x} [{}]", addr, mode ? "store" : "load"));
	if (EmuConfig.Cpu.Recompiler.PauseOnTLBMiss)
	{
		// Pause, let the user try to figure out what went wrong in the debugger.
		Host::ReportErrorAsync("R5900 Exception", message);
		VMManager::SetPaused(true);
		Cpu->ExitExecution();
		return;
	}

#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	{
		static std::atomic<int> s_be_count{0};
		int be_n = s_be_count.fetch_add(1, std::memory_order_relaxed);
		if (be_n < 50) {
			Console.WriteLn("@@BUS_ERROR_PC@@ n=%d addr=%08x mode=%u ee_pc=%08x cycle=%u",
				be_n, addr, mode, cpuRegs.pc, cpuRegs.cycle);
		}
		if (be_n == 0 && eeMem) {
			// Dump loop-start opcodes (cpuRegs.pc - 0x9c = 0x100b68) and block-start opcodes
			// [iter170] extended to 40 instructions to cover gap at pc-0x1c (0x100be8-0x100c00)
			if (cpuRegs.pc >= 0x9c && (cpuRegs.pc - 0x9c + 160) < Ps2MemSize::MainRam) {
				const u32* lo = reinterpret_cast<const u32*>(eeMem->Main + cpuRegs.pc - 0x9c);
				Console.WriteLn("@@LOOP_OPS0@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x", cpuRegs.pc - 0x9c,
					lo[0], lo[1], lo[2], lo[3], lo[4], lo[5], lo[6], lo[7]);
				Console.WriteLn("@@LOOP_OPS1@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x", cpuRegs.pc - 0x7c,
					lo[8], lo[9], lo[10], lo[11], lo[12], lo[13], lo[14], lo[15]);
				Console.WriteLn("@@LOOP_OPS2@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x", cpuRegs.pc - 0x5c,
					lo[16], lo[17], lo[18], lo[19], lo[20], lo[21], lo[22], lo[23]);
				Console.WriteLn("@@LOOP_OPS3@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x", cpuRegs.pc - 0x3c,
					lo[24], lo[25], lo[26], lo[27], lo[28], lo[29], lo[30], lo[31]);
				// GAP coverage: 8 more instructions (0x100be8-0x100c04)
				Console.WriteLn("@@LOOP_OPS4@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x", cpuRegs.pc - 0x1c,
					lo[32], lo[33], lo[34], lo[35], lo[36], lo[37], lo[38], lo[39]);
			}
			if (cpuRegs.pc + 64 < Ps2MemSize::MainRam) {
				const u32* ops = reinterpret_cast<const u32*>(eeMem->Main + cpuRegs.pc);
				Console.WriteLn("@@BLOCK_OPS@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x", cpuRegs.pc,
					ops[0], ops[1], ops[2], ops[3], ops[4], ops[5], ops[6], ops[7]);
				Console.WriteLn("@@BLOCK_OPS2@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x", cpuRegs.pc+32,
					ops[8], ops[9], ops[10], ops[11], ops[12], ops[13], ops[14], ops[15]);
			}
			// Read actual return address from stack: LD ra, 0x60(sp) at epilogue → [sp+0x60]
			// Also read JAL 0x100c60 subroutine opcodes (the loop bit-buffer refill routine)
			{
				u32 sp_val = cpuRegs.GPR.r[29].UL[0]; // stale sp (should be correct: sp is callee-saved)
				u32 stack_ra_addr = sp_val + 0x60;
				if (stack_ra_addr + 4 <= Ps2MemSize::MainRam) {
					u32 ra_actual = *(const u32*)(eeMem->Main + stack_ra_addr);
					Console.WriteLn("@@STACK_RA@@ sp=%08x [sp+0x60]=%08x (actual return addr)", sp_val, ra_actual);
					// Also read opcodes at the return address to see what caller does after decompressor exits
					if (ra_actual < Ps2MemSize::MainRam - 32) {
						const u32* caller = reinterpret_cast<const u32*>(eeMem->Main + ra_actual);
						Console.WriteLn("@@CALLER_OPS@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
							ra_actual, caller[0], caller[1], caller[2], caller[3],
							caller[4], caller[5], caller[6], caller[7]);
					}
				}
				// JAL 0x100c60: the bit-buffer refill subroutine (called every 30 iterations, advances s3)
				// [iter171] extended to 48 instructions to find JR ra and check if s3 is written
				u32 jal_target = 0x100c60;
				if (jal_target + 192 < Ps2MemSize::MainRam) {
					const u32* jal = reinterpret_cast<const u32*>(eeMem->Main + jal_target);
					Console.WriteLn("@@JAL_OPS@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
						jal_target, jal[0], jal[1], jal[2], jal[3], jal[4], jal[5], jal[6], jal[7]);
					Console.WriteLn("@@JAL_OPS2@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
						jal_target+32, jal[8], jal[9], jal[10], jal[11], jal[12], jal[13], jal[14], jal[15]);
					Console.WriteLn("@@JAL_OPS3@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
						jal_target+64, jal[16], jal[17], jal[18], jal[19], jal[20], jal[21], jal[22], jal[23]);
					Console.WriteLn("@@JAL_OPS4@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
						jal_target+96, jal[24], jal[25], jal[26], jal[27], jal[28], jal[29], jal[30], jal[31]);
					Console.WriteLn("@@JAL_OPS5@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
						jal_target+128, jal[32], jal[33], jal[34], jal[35], jal[36], jal[37], jal[38], jal[39]);
					Console.WriteLn("@@JAL_OPS6@@ pc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
						jal_target+160, jal[40], jal[41], jal[42], jal[43], jal[44], jal[45], jal[46], jal[47]);
				}
			}
		}
		// [iter42] Specific probe for 0x32000000 (unmapped physical load causing hang).
		// Removal condition: 0x32000000 アクセス元 EE 命令 (PC) およびcauseafter determined。
		if (addr == 0x32000000) {
			static bool s_32_seen = false;
			if (!s_32_seen) {
				s_32_seen = true;
				Console.WriteLn("@@BUS_ERROR_32@@ addr=%08x mode=%u ee_pc=%08x ra=%08x t0=%08x a0=%08x a6=%08x",
					addr, mode, cpuRegs.pc, cpuRegs.GPR.r[31].UL[0],
					cpuRegs.GPR.r[8].UL[0], cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[10].UL[0]);
			}
		}
	}
#endif
	Console.Error(message);
}

// clang-format off
template <typename OperandType>
static OperandType vtlbUnmappedVReadSm(u32 addr) {
	vtlb_Miss(addr, 0);
	// If vtlb_Miss installed a buffer mapping (mirror path), retry the read.
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr))
		return *reinterpret_cast<const OperandType*>(vmv.assumePtr(addr));
	return 0;
}
static RETURNS_R128 vtlbUnmappedVReadLg(u32 addr) { vtlb_Miss(addr, 0); return r128_zero(); }

template <typename OperandType>
static void vtlbUnmappedVWriteSm(u32 addr, OperandType data) {
	vtlb_Miss(addr, 1);
	// If vtlb_Miss installed a buffer mapping (mirror path), retry the write.
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr))
		*reinterpret_cast<OperandType*>(vmv.assumePtr(addr)) = data;
}
static void TAKES_R128 vtlbUnmappedVWriteLg(u32 addr, r128 data) { vtlb_Miss(addr, 1); }

template <typename OperandType>
static OperandType vtlbUnmappedPReadSm(u32 addr) {
	// [iter419] PS1DRV HW reg alias: 0x40000000-0x40FFFFFF -> 0x10000000-0x10FFFFFF
	// Use pmap direct dispatch (NOT vtlb_memRead) to avoid vmap-based infinite recursion.
	if (addr >= 0x40000000u && addr < 0x41000000u) {
		const u32 hw = addr - 0x30000000u;
		static u32 s_redir_cnt = 0;
		if (s_redir_cnt < 4) Console.WriteLn("@@PS1DRV_HW_REDIR@@ rd addr=0x%08x->0x%08x sz=%u n=%u", addr, hw, (u32)sizeof(OperandType), s_redir_cnt++);
		const auto& phys = vtlbdata.pmap[hw >> VTLB_PAGE_BITS];
		if (phys.isHandler()) {
			using FP = vtlbMemFP<sizeof(OperandType)*8, false>;
			auto fn = (typename FP::fn*)vtlbdata.RWFT[FP::Index][0][phys.assumeHandler()];
			if (fn) return fn(hw);
		}
		return 0;
	}
	vtlb_BusError(addr, 0);
	if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr)){
		switch (sizeof(OperandType)) {
			case 1: return readCache8(addr, false);
			case 2: return readCache16(addr, false);
			case 4: return readCache32(addr, false);
			case 8: return readCache64(addr, false);
			default: pxFail("Invalid data size for unmapped physical cache load");
		}
	}
	return 0;
}
static RETURNS_R128 vtlbUnmappedPReadLg(u32 addr) { vtlb_BusError(addr, 0); if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr)){ return readCache128(addr, false); } return r128_zero(); }

template <typename OperandType>
static void vtlbUnmappedPWriteSm(u32 addr, OperandType data) {
	// [iter419] PS1DRV HW reg alias: 0x40000000-0x40FFFFFF -> 0x10000000-0x10FFFFFF
	// Use pmap direct dispatch to avoid vmap-based infinite recursion.
	if (addr >= 0x40000000u && addr < 0x41000000u) {
		const u32 hw = addr - 0x30000000u;
		static u32 s_redir_cnt = 0;
		if (s_redir_cnt < 4) Console.WriteLn("@@PS1DRV_HW_REDIR@@ wr addr=0x%08x->0x%08x sz=%u n=%u", addr, hw, (u32)sizeof(OperandType), s_redir_cnt++);
		const auto& phys = vtlbdata.pmap[hw >> VTLB_PAGE_BITS];
		if (phys.isHandler()) {
			using FP = vtlbMemFP<sizeof(OperandType)*8, true>;
			auto fn = (typename FP::fn*)vtlbdata.RWFT[FP::Index][1][phys.assumeHandler()];
			if (fn) fn(hw, data);
		}
		return;
	}
	vtlb_BusError(addr, 1);
	if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr)) {
		switch (sizeof(OperandType)) {
			case 1: writeCache8(addr, data, false); break;
			case 2: writeCache16(addr, data, false); break;
			case 4: writeCache32(addr, data, false); break;
			case 8: writeCache64(addr, data, false); break;
			default: pxFail("Invalid data size for unmapped physical cache store");
		}
	}
}
static void TAKES_R128 vtlbUnmappedPWriteLg(u32 addr, r128 data) { vtlb_BusError(addr, 1); if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr)) { writeCache128(addr, reinterpret_cast<mem128_t*>(&data) /*Safe??*/, false); }}
// clang-format on

// --------------------------------------------------------------------------------------
//  VTLB mapping errors
// --------------------------------------------------------------------------------------
// These errors are assertion/logic errors that should never occur if PCSX2 has been initialized
// properly.  All addressable physical memory should be configured as TLBMiss or Bus Error.
//

// [iter216] All vtlbDefault handlers: pxFail → capped warning.
// Corrupted vtlb mappings may route accesses to unmapped physical space.
// Real PS2 silently ignores these. pxFail causes SIGABRT loop on iOS debug.
static mem8_t vtlbDefaultPhyRead8(u32 addr)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	static u32 s_cnt = 0;
	if (s_cnt < 3) Console.Warning("@@VTLB_UNMAP_R8@@ addr=0x%08x n=%u", addr, s_cnt++);
#endif
	return 0;
}

static mem16_t vtlbDefaultPhyRead16(u32 addr)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	static u32 s_cnt = 0;
	if (s_cnt < 3) Console.Warning("@@VTLB_UNMAP_R16@@ addr=0x%08x n=%u", addr, s_cnt++);
#endif
	return 0;
}

static mem32_t vtlbDefaultPhyRead32(u32 addr)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	static u32 s_cnt = 0;
	if (s_cnt < 3) Console.Warning("@@VTLB_UNMAP_R32@@ addr=0x%08x n=%u", addr, s_cnt++);
#endif
	return 0;
}

static mem64_t vtlbDefaultPhyRead64(u32 addr)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	static u32 s_cnt = 0;
	if (s_cnt < 3) Console.Warning("@@VTLB_UNMAP_R64@@ addr=0x%08x n=%u", addr, s_cnt++);
#endif
	return 0;
}

static RETURNS_R128 vtlbDefaultPhyRead128(u32 addr)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	static u32 s_cnt = 0;
	if (s_cnt < 3) Console.Warning("@@VTLB_UNMAP_R128@@ addr=0x%08x n=%u", addr, s_cnt++);
#endif
	return r128_zero();
}

static void vtlbDefaultPhyWrite8(u32 addr, mem8_t data)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	static u32 s_cnt = 0;
	if (s_cnt < 3) Console.Warning("@@VTLB_UNMAP_W8@@ addr=0x%08x n=%u", addr, s_cnt++);
#endif
}

static void vtlbDefaultPhyWrite16(u32 addr, mem16_t data)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	static u32 s_cnt = 0;
	if (s_cnt < 3) Console.Warning("@@VTLB_UNMAP_W16@@ addr=0x%08x n=%u", addr, s_cnt++);
#endif
}

static void vtlbDefaultPhyWrite32(u32 addr, mem32_t data)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	static u32 s_cnt = 0;
	if (s_cnt < 3) Console.Warning("@@VTLB_UNMAP_W32@@ addr=0x%08x n=%u", addr, s_cnt++);
#endif
}

static void vtlbDefaultPhyWrite64(u32 addr, mem64_t data)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	// [iPSX2] @@UNMAPPED_PHY_WRITE64@@ (iter175): BIOS at EE PC=0x9FC43420 writes to
	// physical 0x12040000 (GS region + 256KB, unmapped). Real PS2 hardware silently absorbs
	// writes to unmapped physical addresses. pxFail() here causes SIGABRT→signal loop on iOS
	// debug builds. Convert to capped warning to allow execution to continue, matching release
	// build behavior. Removal: once BIOS proceeds past this point and output is confirmed.
	static int s_unmapped_wr64_cnt = 0;
	if (s_unmapped_wr64_cnt < 5)
	{
		Console.WriteLn("@@UNMAPPED_PHY_WRITE64@@ addr=0x%08x data=0x%016llx cnt=%d",
			addr, (unsigned long long)data, s_unmapped_wr64_cnt);
		++s_unmapped_wr64_cnt;
	}
#endif
}

static void TAKES_R128 vtlbDefaultPhyWrite128(u32 addr, r128 data)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	static u32 s_cnt = 0;
	if (s_cnt < 3) Console.Warning("@@VTLB_UNMAP_W128@@ addr=0x%08x n=%u", addr, s_cnt++);
#endif
}

// ===========================================================================================
//  VTLB Public API -- Init/Term/RegisterHandler stuff
// ===========================================================================================
//

// Assigns or re-assigns the callbacks for a VTLB memory handler.  The handler defines specific behavior
// for how memory pages bound to the handler are read from / written to.  If any of the handler pointers
// are NULL, the memory operations will be mapped to the BusError handler (thus generating BusError
// exceptions if the emulated app attempts to access them).
//
// Note: All handlers persist across calls to vtlb_Reset(), but are wiped/invalidated by calls to vtlb_Init()
//
__ri void vtlb_ReassignHandler(vtlbHandler rv,
	vtlbMemR8FP* r8, vtlbMemR16FP* r16, vtlbMemR32FP* r32, vtlbMemR64FP* r64, vtlbMemR128FP* r128,
	vtlbMemW8FP* w8, vtlbMemW16FP* w16, vtlbMemW32FP* w32, vtlbMemW64FP* w64, vtlbMemW128FP* w128)
{
	pxAssume(rv < VTLB_HANDLER_ITEMS);

	vtlbdata.RWFT[0][0][rv] = (void*)((r8 != 0) ? r8 : vtlbDefaultPhyRead8);
	vtlbdata.RWFT[1][0][rv] = (void*)((r16 != 0) ? r16 : vtlbDefaultPhyRead16);
	vtlbdata.RWFT[2][0][rv] = (void*)((r32 != 0) ? r32 : vtlbDefaultPhyRead32);
	vtlbdata.RWFT[3][0][rv] = (void*)((r64 != 0) ? r64 : vtlbDefaultPhyRead64);
	vtlbdata.RWFT[4][0][rv] = (void*)((r128 != 0) ? r128 : vtlbDefaultPhyRead128);

	vtlbdata.RWFT[0][1][rv] = (void*)((w8 != 0) ? w8 : vtlbDefaultPhyWrite8);
	vtlbdata.RWFT[1][1][rv] = (void*)((w16 != 0) ? w16 : vtlbDefaultPhyWrite16);
	vtlbdata.RWFT[2][1][rv] = (void*)((w32 != 0) ? w32 : vtlbDefaultPhyWrite32);
	vtlbdata.RWFT[3][1][rv] = (void*)((w64 != 0) ? w64 : vtlbDefaultPhyWrite64);
	vtlbdata.RWFT[4][1][rv] = (void*)((w128 != 0) ? w128 : vtlbDefaultPhyWrite128);
}

vtlbHandler vtlb_NewHandler()
{
	pxAssertMsg(vtlbHandlerCount < VTLB_HANDLER_ITEMS, "VTLB handler count overflow!");
	return vtlbHandlerCount++;
}

// Registers a handler into the VTLB's internal handler array.  The handler defines specific behavior
// for how memory pages bound to the handler are read from / written to.  If any of the handler pointers
// are NULL, the memory operations will be mapped to the BusError handler (thus generating BusError
// exceptions if the emulated app attempts to access them).
//
// Note: All handlers persist across calls to vtlb_Reset(), but are wiped/invalidated by calls to vtlb_Init()
//
// Returns a handle for the newly created handler  See vtlb_MapHandler for use of the return value.
//
__ri vtlbHandler vtlb_RegisterHandler(vtlbMemR8FP* r8, vtlbMemR16FP* r16, vtlbMemR32FP* r32, vtlbMemR64FP* r64, vtlbMemR128FP* r128,
	vtlbMemW8FP* w8, vtlbMemW16FP* w16, vtlbMemW32FP* w32, vtlbMemW64FP* w64, vtlbMemW128FP* w128)
{
	vtlbHandler rv = vtlb_NewHandler();
	vtlb_ReassignHandler(rv, r8, r16, r32, r64, r128, w8, w16, w32, w64, w128);
	return rv;
}


// Maps the given hander (created with vtlb_RegisterHandler) to the specified memory region.
// New mappings always assume priority over previous mappings, so place "generic" mappings for
// large areas of memory first, and then specialize specific small regions of memory afterward.
// A single handler can be mapped to many different regions by using multiple calls to this
// function.
//
// The memory region start and size parameters must be pagesize aligned.
void vtlb_MapHandler(vtlbHandler handler, u32 start, u32 size)
{
	verify(0 == (start & VTLB_PAGE_MASK));
	verify(0 == (size & VTLB_PAGE_MASK) && size > 0);

	u32 end = start + (size - VTLB_PAGE_SIZE);
	pxAssume((end >> VTLB_PAGE_BITS) < (sizeof(vtlbdata.pmap) / sizeof(vtlbdata.pmap[0])));

	while (start <= end)
	{
		vtlbdata.pmap[start >> VTLB_PAGE_BITS] = VTLBPhysical::fromHandler(handler);
		start += VTLB_PAGE_SIZE;
	}
}

void vtlb_MapBlock(void* base, u32 start, u32 size, u32 blocksize)
{
	verify(0 == (start & VTLB_PAGE_MASK));
	verify(0 == (size & VTLB_PAGE_MASK) && size > 0);
	if (!blocksize)
		blocksize = size;
	verify(0 == (blocksize & VTLB_PAGE_MASK) && blocksize > 0);
	verify(0 == (size % blocksize));

	sptr baseint = (sptr)base;
	u32 end = start + (size - VTLB_PAGE_SIZE);
	verify((end >> VTLB_PAGE_BITS) < std::size(vtlbdata.pmap));

	while (start <= end)
	{
		u32 loopsz = blocksize;
		sptr ptr = baseint;

		while (loopsz > 0)
		{
			vtlbdata.pmap[start >> VTLB_PAGE_BITS] = VTLBPhysical::fromPointer(ptr);

			start += VTLB_PAGE_SIZE;
			ptr += VTLB_PAGE_SIZE;
			loopsz -= VTLB_PAGE_SIZE;
		}
	}
}

void vtlb_Mirror(u32 new_region, u32 start, u32 size)
{
	verify(0 == (new_region & VTLB_PAGE_MASK));
	verify(0 == (start & VTLB_PAGE_MASK));
	verify(0 == (size & VTLB_PAGE_MASK) && size > 0);

	u32 end = start + (size - VTLB_PAGE_SIZE);
	verify((end >> VTLB_PAGE_BITS) < std::size(vtlbdata.pmap));

	while (start <= end)
	{
		vtlbdata.pmap[start >> VTLB_PAGE_BITS] = vtlbdata.pmap[new_region >> VTLB_PAGE_BITS];

		start += VTLB_PAGE_SIZE;
		new_region += VTLB_PAGE_SIZE;
	}
}

__fi void* vtlb_GetPhyPtr(u32 paddr)
{
	if (paddr >= VTLB_PMAP_SZ || vtlbdata.pmap[paddr >> VTLB_PAGE_BITS].isHandler())
		return NULL;
	else
		return reinterpret_cast<void*>(vtlbdata.pmap[paddr >> VTLB_PAGE_BITS].assumePtr() + (paddr & VTLB_PAGE_MASK));
}

__fi u32 vtlb_V2P(u32 vaddr)
{
	u32 paddr = vtlbdata.ppmap[vaddr >> VTLB_PAGE_BITS];
	paddr |= vaddr & VTLB_PAGE_MASK;
	return paddr;
}

static constexpr bool vtlb_MismatchedHostPageSize()
{
	return (__pagesize != VTLB_PAGE_SIZE);
}

static bool vtlb_IsHostAligned(u32 paddr)
{
	if constexpr (!vtlb_MismatchedHostPageSize())
		return true;

	return ((paddr & __pagemask) == 0);
}

static u32 vtlb_HostPage(u32 page)
{
	if constexpr (!vtlb_MismatchedHostPageSize())
		return page;

	return page >> (__pageshift - VTLB_PAGE_BITS);
}

static u32 vtlb_HostAlignOffset(u32 offset)
{
	if constexpr (!vtlb_MismatchedHostPageSize())
		return offset;

	return offset & ~__pagemask;
}

static bool vtlb_IsHostCoalesced(u32 page)
{
	if constexpr (__pagesize == VTLB_PAGE_SIZE)
	{
		return true;
	}
	else
	{
		static constexpr u32 shift = __pageshift - VTLB_PAGE_BITS;
		static constexpr u32 count = (1u << shift);
		static constexpr u32 mask = count - 1;

		const u32 base = page & ~mask;
		const u32 base_offset = s_fastmem_virtual_mapping[base];
		if ((base_offset & __pagemask) != 0)
			return false;

		for (u32 i = 0, expected_offset = base_offset; i < count; i++, expected_offset += VTLB_PAGE_SIZE)
		{
			if (s_fastmem_virtual_mapping[base + i] != expected_offset)
				return false;
		}

		return true;
	}
}

static bool vtlb_GetMainMemoryOffsetFromPtr(uptr ptr, u32* mainmem_offset, u32* mainmem_size, PageProtectionMode* prot)
{
	const uptr page_end = ptr + VTLB_PAGE_SIZE;

	// EE memory and ROMs.
	if (ptr >= (uptr)eeMem->Main && page_end <= (uptr)eeMem->ZeroRead)
	{
		const u32 eemem_offset = static_cast<u32>(ptr - (uptr)eeMem->Main);

		// [FIX] Prevent fastmem mappings from extending past EEmemSize into IOP memory.
		// sizeof(EEVM_MemoryAllocMess) > EEmemSize, so ROM2's upper pages would overlap
		// with IOP memory in the shared data file. Fall back to slowmem for those pages.
		if ((eemem_offset + VTLB_PAGE_SIZE) > HostMemoryMap::EEmemSize)
			return false;

		const bool writeable = ((eemem_offset < Ps2MemSize::ExposedRam) ? (mmap_GetRamPageInfo(eemem_offset) != ProtMode_Write) : true);
		*mainmem_offset = (eemem_offset + HostMemoryMap::EEmemOffset);
		*mainmem_size = (offsetof(EEVM_MemoryAllocMess, ZeroRead) - eemem_offset);
		*prot = PageProtectionMode().Read().Write(writeable);
		return true;
	}

	// IOP memory.
	if (ptr >= (uptr)iopMem->Main && page_end <= (uptr)iopMem->P)
	{
		const u32 iopmem_offset = static_cast<u32>(ptr - (uptr)iopMem->Main);
		*mainmem_offset = iopmem_offset + HostMemoryMap::IOPmemOffset;
		*mainmem_size = (offsetof(IopVM_MemoryAllocMess, P) - iopmem_offset);
		*prot = PageProtectionMode().Read().Write();
		return true;
	}

	// VU memory - this includes both data and code for VU0/VU1.
	// Practically speaking, this is only data, because the code goes through a handler.
	if (ptr >= (uptr)SysMemory::GetVUMem() && page_end <= (uptr)SysMemory::GetVUMemEnd())
	{
		const u32 vumem_offset = static_cast<u32>(ptr - (uptr)SysMemory::GetVUMem());
		*mainmem_offset = vumem_offset + HostMemoryMap::VUmemOffset;
		*mainmem_size = HostMemoryMap::VUmemSize - vumem_offset;
		*prot = PageProtectionMode().Read().Write();
		return true;
	}

	// We end up with some unknown mappings here; currently the IOP memory, instead of being physically mapped
	// as 2MB, ends up being mapped as 8MB. But this shouldn't be virtual mapped anyway, so fallback to slowmem
	// in such cases.
	return false;
}

static bool vtlb_GetMainMemoryOffset(u32 paddr, u32* mainmem_offset, u32* mainmem_size, PageProtectionMode* prot)
{
	if (paddr >= VTLB_PMAP_SZ)
		return false;

	// Handlers aren't in our shared memory, obviously.
	const VTLBPhysical& vm = vtlbdata.pmap[paddr >> VTLB_PAGE_BITS];
	if (vm.isHandler())
		return false;

	return vtlb_GetMainMemoryOffsetFromPtr(vm.raw(), mainmem_offset, mainmem_size, prot);
}

static void vtlb_CreateFastmemMapping(u32 vaddr, u32 mainmem_offset, const PageProtectionMode& mode)
{
	if (s_fastmem_virtual_mapping.empty())
		return;

	FASTMEM_LOG("Create fastmem mapping @ vaddr %08X mainmem %08X", vaddr, mainmem_offset);

	const u32 page = vaddr / VTLB_PAGE_SIZE;

	if (s_fastmem_virtual_mapping[page] == mainmem_offset)
	{
		// current mapping is fine
		return;
	}

	if (s_fastmem_virtual_mapping[page] != NO_FASTMEM_MAPPING)
	{
		// current mapping needs to be removed
		const bool was_coalesced = vtlb_IsHostCoalesced(page);

		s_fastmem_virtual_mapping[page] = NO_FASTMEM_MAPPING;
		if (was_coalesced && !s_fastmem_area->Unmap(s_fastmem_area->PagePointer(vtlb_HostPage(page)), __pagesize))
			Console.Error("Failed to unmap vaddr %08X", vaddr);

		// remove reverse mapping
		auto range = s_fastmem_physical_mapping.equal_range(mainmem_offset);
		for (auto it = range.first; it != range.second;)
		{
			auto this_it = it++;
			if (this_it->second == vaddr)
				s_fastmem_physical_mapping.erase(this_it);
		}
	}

	s_fastmem_virtual_mapping[page] = mainmem_offset;
	if (vtlb_IsHostCoalesced(page))
	{
		const u32 host_page = vtlb_HostPage(page);
		const u32 host_offset = vtlb_HostAlignOffset(mainmem_offset);

		if (!s_fastmem_area->Map(SysMemory::GetDataFileHandle(), host_offset,
				s_fastmem_area->PagePointer(host_page), __pagesize, mode))
		{
			Console.Error("Failed to map vaddr %08X to mainmem offset %08X", vtlb_HostAlignOffset(vaddr), host_offset);
			s_fastmem_virtual_mapping[page] = NO_FASTMEM_MAPPING;
			return;
		}

	}

	s_fastmem_physical_mapping.emplace(mainmem_offset, vaddr);
}

static void vtlb_RemoveFastmemMapping(u32 vaddr)
{
	if (s_fastmem_virtual_mapping.empty())
		return;

	const u32 page = vaddr / VTLB_PAGE_SIZE;
	if (s_fastmem_virtual_mapping[page] == NO_FASTMEM_MAPPING)
		return;

	const u32 mainmem_offset = s_fastmem_virtual_mapping[page];
	const bool was_coalesced = vtlb_IsHostCoalesced(page);
	FASTMEM_LOG("Remove fastmem mapping @ vaddr %08X mainmem %08X", vaddr, mainmem_offset);
	s_fastmem_virtual_mapping[page] = NO_FASTMEM_MAPPING;

	if (was_coalesced && !s_fastmem_area->Unmap(s_fastmem_area->PagePointer(vtlb_HostPage(page)), __pagesize))
		Console.Error("Failed to unmap vaddr %08X", vtlb_HostAlignOffset(vaddr));

	// remove from reverse map
	auto range = s_fastmem_physical_mapping.equal_range(mainmem_offset);
	for (auto it = range.first; it != range.second;)
	{
		auto this_it = it++;
		if (this_it->second == vaddr)
			s_fastmem_physical_mapping.erase(this_it);
	}
}

static void vtlb_RemoveFastmemMappings(u32 vaddr, u32 size)
{
	if (s_fastmem_virtual_mapping.empty())
		return;

	pxAssert((vaddr & VTLB_PAGE_MASK) == 0);
	pxAssert(size > 0 && (size & VTLB_PAGE_MASK) == 0);

	const u32 num_pages = size / VTLB_PAGE_SIZE;
	for (u32 i = 0; i < num_pages; i++, vaddr += VTLB_PAGE_SIZE)
		vtlb_RemoveFastmemMapping(vaddr);
}

static void vtlb_RemoveFastmemMappings()
{
	if (s_fastmem_virtual_mapping.empty())
	{
		// not initialized yet
		return;
	}

	for (u32 page = 0; page < FASTMEM_PAGE_COUNT; page++)
	{
		if (s_fastmem_virtual_mapping[page] == NO_FASTMEM_MAPPING)
			continue;

		if (vtlb_IsHostCoalesced(page))
		{
			if (!s_fastmem_area->Unmap(s_fastmem_area->PagePointer(vtlb_HostPage(page)), __pagesize))
				Console.Error("Failed to unmap vaddr %08X", page * __pagesize);
		}

		s_fastmem_virtual_mapping[page] = NO_FASTMEM_MAPPING;
	}

	s_fastmem_physical_mapping.clear();
}

bool vtlb_ResolveFastmemMapping(uptr* addr)
{
	uptr uaddr = *addr;
	// yaps2 2801ebde: subtraction-first, single unsigned bound. `fastmem_start +
	// 0xFFFFFFFF` overflows a 64-bit uptr when the fastmem mapping lands within 4 GB of the
	// top of the address space (exotic kernels / high-mmap allocators), wrapping fastmem_end
	// below fastmem_start so the two-sided compare silently rejected every valid in-range
	// address. `offset >= FASTMEM_AREA_SIZE` is overflow-proof regardless of base.
	const uptr fastmem_start = (uptr)vtlbdata.fastmem_base;
	const uptr offset = uaddr - fastmem_start;
	if (offset >= FASTMEM_AREA_SIZE)
		return false;

	const u32 vaddr = static_cast<u32>(offset);
	FASTMEM_LOG("Trying to resolve %p (vaddr %08X)", (void*)uaddr, vaddr);

	const u32 vpage = vaddr / VTLB_PAGE_SIZE;
	if (s_fastmem_virtual_mapping[vpage] == NO_FASTMEM_MAPPING)
	{
		FASTMEM_LOG("%08X is not virtual mapped", vaddr);
		return false;
	}

	const u32 mainmem_offset = s_fastmem_virtual_mapping[vpage] + (vaddr & VTLB_PAGE_MASK);
	FASTMEM_LOG("Resolved %p (vaddr %08X) to mainmem offset %08X", uaddr, vaddr, mainmem_offset);
	*addr = ((uptr)SysMemory::GetDataPtr(0)) + mainmem_offset;
	return true;
}

bool vtlb_GetGuestAddress(uptr host_addr, u32* guest_addr)
{
	// yaps2 2801ebde: subtraction-first, single unsigned bound. `fastmem_start +
	// 0xFFFFFFFF` overflows a 64-bit uptr when the fastmem mapping lands within 4 GB of the
	// top of the address space (exotic kernels / high-mmap allocators), wrapping fastmem_end
	// below fastmem_start so the two-sided compare silently rejected every valid in-range
	// address. `offset >= FASTMEM_AREA_SIZE` is overflow-proof regardless of base.
	const uptr fastmem_start = (uptr)vtlbdata.fastmem_base;
	const uptr offset = host_addr - fastmem_start;
	if (offset >= FASTMEM_AREA_SIZE)
		return false;

	*guest_addr = static_cast<u32>(offset);
	return true;
}

void vtlb_UpdateFastmemProtection(u32 paddr, u32 size, PageProtectionMode prot)
{
	if (!CHECK_FASTMEM)
		return;

	pxAssert((paddr & VTLB_PAGE_MASK) == 0);
	pxAssert(size > 0 && (size & VTLB_PAGE_MASK) == 0);

	u32 mainmem_start, mainmem_size;
	PageProtectionMode old_prot;
	if (!vtlb_GetMainMemoryOffset(paddr, &mainmem_start, &mainmem_size, &old_prot))
		return;

	FASTMEM_LOG("UpdateFastmemProtection %08X mmoffset %08X %08X", paddr, mainmem_start, size);

	u32 current_mainmem = mainmem_start;
	const u32 num_pages = std::min(size, mainmem_size) / VTLB_PAGE_SIZE;
	for (u32 i = 0; i < num_pages; i++, current_mainmem += VTLB_PAGE_SIZE)
	{
		// update virtual mapping mapping
		auto range = s_fastmem_physical_mapping.equal_range(current_mainmem);
		for (auto it = range.first; it != range.second; ++it)
		{
			FASTMEM_LOG("  valias %08X (size %u)", it->second, VTLB_PAGE_SIZE);

			if (vtlb_IsHostAligned(it->second))
				HostSys::MemProtect(s_fastmem_area->OffsetPointer(it->second), __pagesize, prot);
		}
	}
}

void vtlb_ClearLoadStoreInfo()
{
	s_fastmem_backpatch_info.clear();
	s_fastmem_faulting_pcs.clear();
}

void vtlb_AddLoadStoreInfo(uptr code_address, u32 code_size, u32 guest_pc, u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits, bool is_signed, bool is_load, bool is_fpr)
{
	pxAssert(code_size <= std::numeric_limits<u16>::max());

	auto iter = s_fastmem_backpatch_info.find(code_address);
	if (iter != s_fastmem_backpatch_info.end())
		s_fastmem_backpatch_info.erase(iter);

	LoadstoreBackpatchInfo info{guest_pc, gpr_bitmask, fpr_bitmask, static_cast<u16>(code_size), address_register, data_register, size_in_bits, is_signed, is_load, is_fpr};
	s_fastmem_backpatch_info.emplace(code_address, info);
}

bool vtlb_BackpatchLoadStore(uptr code_address, uptr fault_address)
{
	// yaps2 2801ebde: subtraction-first, single unsigned bound. `fastmem_start +
	// 0xFFFFFFFF` overflows a 64-bit uptr when the fastmem mapping lands within 4 GB of the
	// top of the address space (exotic kernels / high-mmap allocators), wrapping fastmem_end
	// below fastmem_start so the two-sided compare silently rejected every valid in-range
	// address. `offset >= FASTMEM_AREA_SIZE` is overflow-proof regardless of base.
	const uptr fastmem_start = (uptr)vtlbdata.fastmem_base;
	const uptr offset = fault_address - fastmem_start;
	if (offset >= FASTMEM_AREA_SIZE)
		return false;

	auto iter = s_fastmem_backpatch_info.find(code_address);
	if (iter == s_fastmem_backpatch_info.end())
		return false;

	const LoadstoreBackpatchInfo& info = iter->second;
	const u32 guest_addr = static_cast<u32>(offset);
	vtlb_DynBackpatchLoadStore(code_address, info.code_size, info.guest_pc, guest_addr,
		info.gpr_bitmask, info.fpr_bitmask, info.address_register, info.data_register,
		info.size_in_bits, info.is_signed, info.is_load, info.is_fpr);

	// queue block for recompilation later
	Cpu->Clear(info.guest_pc, 1);

	// and store the pc in the faulting list, so that we don't emit another fastmem loadstore
	s_fastmem_faulting_pcs.insert(info.guest_pc);
	s_fastmem_backpatch_info.erase(iter);
	return true;
}

bool vtlb_IsFaultingPC(u32 guest_pc)
{
	return (s_fastmem_faulting_pcs.find(guest_pc) != s_fastmem_faulting_pcs.end());
}

//virtual mappings
//TODO: Add invalid paddr checks
void vtlb_VMap(u32 vaddr, u32 paddr, u32 size)
{
	verify(0 == (vaddr & VTLB_PAGE_MASK));
	verify(0 == (paddr & VTLB_PAGE_MASK));
	verify(0 == (size & VTLB_PAGE_MASK) && size > 0);
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	if (s_tlb_vmap_probe_cfg < 0)
	{
		s_tlb_vmap_probe_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_TLB_VMAP_PROBE", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_TLB_VMAP_PROBE=%d", s_tlb_vmap_probe_cfg);
	}
#endif

	if (CHECK_FASTMEM)
	{
		const u32 num_pages = size / VTLB_PAGE_SIZE;
		u32 current_vaddr = vaddr;
		u32 current_paddr = paddr;

		for (u32 i = 0; i < num_pages; i++, current_vaddr += VTLB_PAGE_SIZE, current_paddr += VTLB_PAGE_SIZE)
		{
			u32 hoffset, hsize;
			PageProtectionMode mode;
			if (vtlb_GetMainMemoryOffset(current_paddr, &hoffset, &hsize, &mode))
				vtlb_CreateFastmemMapping(current_vaddr, hoffset, mode);
			else
				vtlb_RemoveFastmemMapping(current_vaddr);
		}
	}

	while (size > 0)
	{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
		if (s_tlb_vmap_probe_cfg == 1 && !s_tlb_vmap_probe_map_done && (vaddr & 0xFFFFF000u) == 0x70004000u)
		{
			Console.Error("@@TLB_VMAP@@ op=map vaddr=%08x paddr=%08x size=%08x pc=%08x entryHi=%08x badVAddr=%08x",
				vaddr, paddr, size, cpuRegs.pc, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.BadVAddr);
			s_tlb_vmap_probe_map_done = true;
		}
#endif
		VTLBVirtual vmv;
		if (paddr >= VTLB_PMAP_SZ)
			vmv = VTLBVirtual(VTLBPhysical::fromHandler(UnmappedPhyHandler), paddr, vaddr);
		else
			vmv = VTLBVirtual(vtlbdata.pmap[paddr >> VTLB_PAGE_BITS], paddr, vaddr);

		vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS] = vmv;
		if (vtlbdata.ppmap)
		{
			if (!(vaddr & 0x80000000)) // those address are already physical don't change them
				vtlbdata.ppmap[vaddr >> VTLB_PAGE_BITS] = paddr & ~VTLB_PAGE_MASK;
		}

		vaddr += VTLB_PAGE_SIZE;
		paddr += VTLB_PAGE_SIZE;
		size -= VTLB_PAGE_SIZE;
	}
}

void vtlb_VMapBuffer(u32 vaddr, void* buffer, u32 size)
{
	verify(0 == (vaddr & VTLB_PAGE_MASK));
	verify(0 == (size & VTLB_PAGE_MASK) && size > 0);
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	if (s_tlb_vmap_probe_cfg < 0)
	{
		s_tlb_vmap_probe_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_TLB_VMAP_PROBE", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_TLB_VMAP_PROBE=%d", s_tlb_vmap_probe_cfg);
	}
	if (s_tlb_vmap_probe_cfg == 1 && !s_tlb_vmap_probe_mapbuf_done && (vaddr & 0xFFFFF000u) == 0x70004000u)
	{
		Console.Error("@@TLB_VMAP@@ op=mapbuf vaddr=%08x size=%08x is_scratch=%d pc=%08x entryHi=%08x badVAddr=%08x",
			vaddr, size, (buffer == eeMem->Scratch) ? 1 : 0, cpuRegs.pc, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.BadVAddr);
		s_tlb_vmap_probe_mapbuf_done = true;
	}
#endif

	if (CHECK_FASTMEM)
	{
		if (buffer == eeMem->Scratch && size == Ps2MemSize::Scratch)
		{
			u32 fm_vaddr = vaddr;
			u32 fm_hostoffset = HostMemoryMap::EEmemOffset + offsetof(EEVM_MemoryAllocMess, Scratch);
			PageProtectionMode mode = PageProtectionMode().Read().Write();
			for (u32 i = 0; i < (Ps2MemSize::Scratch / VTLB_PAGE_SIZE); i++, fm_vaddr += VTLB_PAGE_SIZE, fm_hostoffset += VTLB_PAGE_SIZE)
				vtlb_CreateFastmemMapping(fm_vaddr, fm_hostoffset, mode);
		}
		else
		{
			vtlb_RemoveFastmemMappings(vaddr, size);
		}
	}

	uptr bu8 = (uptr)buffer;
	while (size > 0)
	{
		vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS] = VTLBVirtual::fromPointer(bu8, vaddr);
		vaddr += VTLB_PAGE_SIZE;
		bu8 += VTLB_PAGE_SIZE;
		size -= VTLB_PAGE_SIZE;
	}
}

void vtlb_VMapUnmap(u32 vaddr, u32 size)
{
	verify(0 == (vaddr & VTLB_PAGE_MASK));
	verify(0 == (size & VTLB_PAGE_MASK) && size > 0);

	vtlb_RemoveFastmemMappings(vaddr, size);

	while (size > 0)
	{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
		if (s_tlb_vmap_probe_cfg == 1 && !s_tlb_vmap_probe_unmap_done && (vaddr & 0xFFFFF000u) == 0x70004000u)
		{
			Console.Error("@@TLB_VMAP@@ op=unmap vaddr=%08x size=%08x pc=%08x entryHi=%08x badVAddr=%08x",
				vaddr, size, cpuRegs.pc, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.BadVAddr);
			s_tlb_vmap_probe_unmap_done = true;
		}
#endif
		vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS] = VTLBVirtual(VTLBPhysical::fromHandler(UnmappedVirtHandler), vaddr, vaddr);
		vaddr += VTLB_PAGE_SIZE;
		size -= VTLB_PAGE_SIZE;
	}
}

// vtlb_Init -- Clears vtlb handlers and memory mappings.
void vtlb_Init()
{
	vtlbHandlerCount = 0;
	std::memset(vtlbdata.RWFT, 0, sizeof(vtlbdata.RWFT));

#define VTLB_BuildUnmappedHandler(baseName) \
	baseName##ReadSm<mem8_t>, baseName##ReadSm<mem16_t>, baseName##ReadSm<mem32_t>, \
		baseName##ReadSm<mem64_t>, baseName##ReadLg, \
		baseName##WriteSm<mem8_t>, baseName##WriteSm<mem16_t>, baseName##WriteSm<mem32_t>, \
		baseName##WriteSm<mem64_t>, baseName##WriteLg

	//Register default handlers
	//Unmapped Virt handlers _MUST_ be registered first.
	//On address translation the top bit cannot be preserved.This is not normaly a problem since
	//the physical address space can be 'compressed' to just 29 bits.However, to properly handle exceptions
	//there must be a way to get the full address back.Thats why i use these 2 functions and encode the hi bit directly into em :)

	UnmappedVirtHandler = vtlb_RegisterHandler(VTLB_BuildUnmappedHandler(vtlbUnmappedV));
	UnmappedPhyHandler = vtlb_RegisterHandler(VTLB_BuildUnmappedHandler(vtlbUnmappedP));
	DefaultPhyHandler = vtlb_RegisterHandler(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

	// [P15] Fill all unregistered RWFT slots (vtlbHandlerCount..127) with default stubs.
	// DynGen_IndirectTlbDispatcher indexes RWFT by handler ID from the vtlb entry's low byte.
	// If a corrupted or out-of-range handler ID is used, the slot would be NULL (from memset)
	// causing BLR to address 0 → SIGSEGV. Fill with safe stubs to prevent this crash.
	// Removal condition: なし（恒久fix）
	for (u32 h = vtlbHandlerCount; h < VTLB_HANDLER_ITEMS; h++)
	{
		vtlbdata.RWFT[0][0][h] = (void*)vtlbDefaultPhyRead8;
		vtlbdata.RWFT[1][0][h] = (void*)vtlbDefaultPhyRead16;
		vtlbdata.RWFT[2][0][h] = (void*)vtlbDefaultPhyRead32;
		vtlbdata.RWFT[3][0][h] = (void*)vtlbDefaultPhyRead64;
		vtlbdata.RWFT[4][0][h] = (void*)vtlbDefaultPhyRead128;
		vtlbdata.RWFT[0][1][h] = (void*)vtlbDefaultPhyWrite8;
		vtlbdata.RWFT[1][1][h] = (void*)vtlbDefaultPhyWrite16;
		vtlbdata.RWFT[2][1][h] = (void*)vtlbDefaultPhyWrite32;
		vtlbdata.RWFT[3][1][h] = (void*)vtlbDefaultPhyWrite64;
		vtlbdata.RWFT[4][1][h] = (void*)vtlbDefaultPhyWrite128;
	}
	Console.WriteLn("@@VTLB_INIT@@ registered_handlers=%u, filled RWFT slots %u..%u with default stubs",
		vtlbHandlerCount, vtlbHandlerCount, VTLB_HANDLER_ITEMS - 1);

	//done !

	//Setup the initial mappings
	vtlb_MapHandler(DefaultPhyHandler, 0, VTLB_PMAP_SZ);

	//Set the V space as unmapped
	vtlb_VMapUnmap(0, (VTLB_VMAP_ITEMS - 1) * VTLB_PAGE_SIZE);
	//yeah i know, its stupid .. but this code has to be here for now ;p
	vtlb_VMapUnmap((VTLB_VMAP_ITEMS - 1) * VTLB_PAGE_SIZE, VTLB_PAGE_SIZE);

	// The LUT is only used for 1 game so we allocate it only when the gamefix is enabled (save 4MB)
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		vtlb_Alloc_Ppmap();
}

// vtlb_Reset -- Performs a COP0-level reset of the PS2's TLB.
// This function should probably be part of the COP0 rather than here in VTLB.
void vtlb_Reset()
{
	vtlb_RemoveFastmemMappings();
	for (int i = 0; i < 48; i++)
		UnmapTLB(tlb[i], i);
}

void vtlb_Shutdown()
{
	vtlb_RemoveFastmemMappings();
	s_fastmem_backpatch_info.clear();
	s_fastmem_faulting_pcs.clear();
}

void vtlb_ResetFastmem()
{
	DevCon.WriteLn("Resetting fastmem mappings...");

	vtlb_RemoveFastmemMappings();
	s_fastmem_backpatch_info.clear();
	s_fastmem_faulting_pcs.clear();

	if (!CHECK_FASTMEM || !CHECK_EEREC || !vtlbdata.vmap)
		return;

	// we need to go through and look at the vtlb pointers, to remap the host area
	for (size_t i = 0; i < VTLB_VMAP_ITEMS; i++)
	{
		const VTLBVirtual& vm = vtlbdata.vmap[i];
		const u32 vaddr = static_cast<u32>(i) << VTLB_PAGE_BITS;
		if (vm.isHandler(vaddr))
		{
			// Handlers should be unmapped.
			continue;
		}

		// Check if it's a physical mapping to our main memory area.
		u32 mainmem_offset, mainmem_size;
		PageProtectionMode prot;
		if (vtlb_GetMainMemoryOffsetFromPtr(vm.assumePtr(vaddr), &mainmem_offset, &mainmem_size, &prot))
			vtlb_CreateFastmemMapping(vaddr, mainmem_offset, prot);
	}
}

// Reserves the vtlb core allocation used by various emulation components!
// [TODO] basemem - request allocating memory at the specified virtual location, which can allow
//    for easier debugging and/or 3rd party cheat programs.  If 0, the operating system
//    default is used.
bool vtlb_Core_Alloc()
{
	static constexpr size_t VMAP_SIZE = sizeof(VTLBVirtual) * VTLB_VMAP_ITEMS;
	static_assert(HostMemoryMap::VTLBVirtualMapSize == VMAP_SIZE);

	pxAssert(!vtlbdata.vmap && !vtlbdata.fastmem_base && !s_fastmem_area);

	vtlbdata.vmap = reinterpret_cast<VTLBVirtual*>(SysMemory::GetVTLBVirtualMap());

	pxAssert(!s_fastmem_area);
	s_fastmem_area = SharedMemoryMappingArea::Create(FASTMEM_AREA_SIZE);
	if (!s_fastmem_area)
	{
		// 4GB virtual reservation can fail on devices with limited VA space (e.g. iPhone SE 2).
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
		Console.Warning("@@FASTMEM_SKIP@@ 4GB fastmem area allocation failed — continuing without fastmem");
		std::fprintf(stderr, "@@FASTMEM_SKIP@@ area_create_failed size=%zu\n", static_cast<size_t>(FASTMEM_AREA_SIZE));
		std::fflush(stderr);
		vtlbdata.fastmem_base = 0;
		EmuConfig.Cpu.Recompiler.EnableFastmem = false;
		Console.Warning("@@FASTMEM_SKIP@@ EnableFastmem forced OFF (CHECK_FASTMEM will be false)");
#else
		Host::ReportErrorAsync("Error", "Failed to allocate fastmem area");
		return false;
#endif
	}

	// [P49] Force-disable fastmem: env var override OR INI setting (EnableFastmem=false)
	{
		const bool env_force = getenv("iPSX2_FORCE_NO_FASTMEM") && atoi(getenv("iPSX2_FORCE_NO_FASTMEM")) == 1;
		const bool ini_disabled = !EmuConfig.Cpu.Recompiler.EnableFastmem;
		if (s_fastmem_area && (env_force || ini_disabled))
		{
			Console.Warning("@@FASTMEM_SKIP@@ Fastmem disabled (env=%d ini=%d) — releasing fastmem area",
				(int)env_force, (int)ini_disabled);
			s_fastmem_area.reset();
			vtlbdata.fastmem_base = 0;
			EmuConfig.Cpu.Recompiler.EnableFastmem = false;
			Console.Warning("@@FASTMEM_SKIP@@ EnableFastmem forced OFF (CHECK_FASTMEM will be false)");
		}
	}

	if (s_fastmem_area)
	{
		s_fastmem_virtual_mapping.resize(FASTMEM_PAGE_COUNT, NO_FASTMEM_MAPPING);
		vtlbdata.fastmem_base = (uptr)s_fastmem_area->BasePointer();
		DevCon.WriteLn(Color_StrongGreen, "Fastmem area: %p - %p",
			vtlbdata.fastmem_base, vtlbdata.fastmem_base + (FASTMEM_AREA_SIZE - 1));
		std::fprintf(stderr, "@@FASTMEM_ENABLE@@ base=%p size=%zu pages=%zu\n",
			s_fastmem_area->BasePointer(), static_cast<size_t>(FASTMEM_AREA_SIZE),
			static_cast<size_t>(FASTMEM_PAGE_COUNT));
		std::fflush(stderr);
	}

	Error error;
	if (!PageFaultHandler::Install(&error))
	{
		Host::ReportErrorAsync("Failed to install page fault handler.", error.GetDescription());
		return false;
	}

	return true;
}

// The LUT is only used for 1 game so we allocate it only when the gamefix is enabled (save 4MB)
// However automatic gamefix is done after the standard init so a new init function was done.
void vtlb_Alloc_Ppmap()
{
	static constexpr size_t PPMAP_SIZE = sizeof(*vtlbdata.ppmap) * VTLB_VMAP_ITEMS;
	static_assert(HostMemoryMap::VTLBAddressMapSize == PPMAP_SIZE);

	if (vtlbdata.ppmap)
		return;

	vtlbdata.ppmap = reinterpret_cast<u32*>(SysMemory::GetVTLBAddressMap());

	// By default a 1:1 virtual to physical mapping
	for (u32 i = 0; i < VTLB_VMAP_ITEMS; i++)
		vtlbdata.ppmap[i] = i << VTLB_PAGE_BITS;
}

void vtlb_Core_Free()
{
	vtlbdata.vmap = nullptr;
	vtlbdata.ppmap = nullptr;

	vtlb_RemoveFastmemMappings();
	vtlb_ClearLoadStoreInfo();

	vtlbdata.fastmem_base = 0;
	decltype(s_fastmem_physical_mapping)().swap(s_fastmem_physical_mapping);
	decltype(s_fastmem_virtual_mapping)().swap(s_fastmem_virtual_mapping);
	s_fastmem_area.reset();
}

int vtlb_ScanFastmemForOffset(u32 target_offset)
{
	int count = 0;
	const u32 page_offset = target_offset & ~(VTLB_PAGE_SIZE - 1);
	for (u32 i = 0; i < s_fastmem_virtual_mapping.size(); i++) {
		u32 mapping = s_fastmem_virtual_mapping[i];
		if (mapping != NO_FASTMEM_MAPPING) {
			u32 map_page_offset = mapping & ~(VTLB_PAGE_SIZE - 1);
			if (map_page_offset == page_offset) {
				Console.Error("@@FASTMEM_IOP_OVERLAP@@ vpage=%u (vaddr=%08x) -> file_offset=%08x (target=%08x)",
					i, i * VTLB_PAGE_SIZE, mapping, target_offset);
				count++;
			}
		}
	}
	return count;
}

// ===========================================================================================
//  Memory Protection and Block Checking, vtlb Style!
// ===========================================================================================
// For the first time code is recompiled (executed), the PS2 ram page for that code is
// protected using Virtual Memory (mprotect).  If the game modifies its own code then this
// protection causes an *exception* to be raised (signal in Linux), which is handled by
// unprotecting the page and switching the recompiled block to "manual" protection.
//
// Manual protection uses a simple brute-force memcmp of the recompiled code to the code
// currently in RAM for *each time* the block is executed.  Fool-proof, but slow, which
// is why we default to using the exception-based protection scheme described above.
//
// Why manual blocks?  Because many games contain code and data in the same 4k page, so
// we *cannot* automatically recompile and reprotect pages, lest we end up recompiling and
// reprotecting them constantly (Which would be very slow).  As a counter, the R5900 side
// of the block checking code does try to periodically re-protect blocks [going from manual
// back to protected], so that blocks which underwent a single invalidation don't need to
// incur a permanent performance penalty.
//
// Page Granularity:
// Fortunately for us MIPS and x86 use the same page granularity for TLB and memory
// protection, so we can use a 1:1 correspondence when protecting pages.  Page granularity
// is 4096 (4k), which is why you'll see a lot of 0xfff's, >><< 12's, and 0x1000's in the
// code below.
//

struct vtlb_PageProtectionInfo
{
	// Ram De-mapping -- used to convert fully translated/mapped offsets (which reside with
	// in the eeMem->Main block) back into their originating ps2 physical ram address.
	// Values are assigned when pages are marked for protection.  since pages are automatically
	// cleared and reset when TLB-remapped, stale values in this table (due to on-the-fly TLB
	// changes) will be re-assigned the next time the page is accessed.
	u32 ReverseRamMap;

	vtlb_ProtectionMode Mode;
};

alignas(16) static vtlb_PageProtectionInfo m_PageProtectInfo[Ps2MemSize::TotalRam >> __pageshift];


// returns:
//  ProtMode_NotRequired - unchecked block (resides in ROM, thus is integrity is constant)
//  Or the current mode
//
vtlb_ProtectionMode mmap_GetRamPageInfo(u32 paddr)
{
	pxAssert(eeMem);

	paddr &= ~0xfff;

	uptr ptr = (uptr)PSM(paddr);
	uptr rampage = ptr - (uptr)eeMem->Main;

	if (!ptr || rampage >= Ps2MemSize::ExposedRam)
		return ProtMode_NotRequired; //not in ram, no tracking done ...

	rampage >>= __pageshift;

	return m_PageProtectInfo[rampage].Mode;
}

// paddr - physically mapped PS2 address
void mmap_MarkCountedRamPage(u32 paddr)
{
	pxAssert(eeMem);

	paddr &= ~__pagemask;

	uptr ptr = (uptr)PSM(paddr);
	int rampage = (ptr - (uptr)eeMem->Main) >> __pageshift;

	// Important: Update the ReverseRamMap here because TLB changes could alter the paddr
	// mapping into eeMem->Main.

	m_PageProtectInfo[rampage].ReverseRamMap = paddr;

	if (m_PageProtectInfo[rampage].Mode == ProtMode_Write)
		return; // skip town if we're already protected.

	eeRecPerfLog.Write((m_PageProtectInfo[rampage].Mode == ProtMode_Manual) ?
						   "Re-protecting page @ 0x%05x" :
						   "Protected page @ 0x%05x",
		paddr >> __pageshift);

	m_PageProtectInfo[rampage].Mode = ProtMode_Write;
	HostSys::MemProtect(&eeMem->Main[rampage << __pageshift], __pagesize, PageAccess_ReadOnly());
	vtlb_UpdateFastmemProtection(rampage << __pageshift, __pagesize, PageAccess_ReadOnly());
}

// offset - offset of address relative to psM.
// All recompiled blocks belonging to the page are cleared, and any new blocks recompiled
// from code residing in this page will use manual protection.
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
std::atomic<uint32_t> g_mmap_clear_count{0};
std::atomic<uint32_t> g_mmap_clear_page_top{0};      // page cleared most often
std::atomic<uint32_t> g_mmap_clear_page_top_count{0}; // its count
static uint32_t s_mmap_clear_per_page[Ps2MemSize::TotalRam >> __pageshift] = {};
#endif

static __fi void mmap_ClearCpuBlock(uint offset)
{
	pxAssert(eeMem);

	int rampage = offset >> __pageshift;

#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	{
		g_mmap_clear_count.fetch_add(1, std::memory_order_relaxed);
		if (rampage >= 0 && rampage < (int)(Ps2MemSize::TotalRam >> __pageshift)) {
			s_mmap_clear_per_page[rampage]++;
			if (s_mmap_clear_per_page[rampage] > g_mmap_clear_page_top_count.load(std::memory_order_relaxed)) {
				g_mmap_clear_page_top.store((uint32_t)rampage, std::memory_order_relaxed);
				g_mmap_clear_page_top_count.store(s_mmap_clear_per_page[rampage], std::memory_order_relaxed);
			}
		}
	}

	{
		static u32 s_cb_n = 0;
		if (s_cb_n < 4)
		{
			char buf[128];
			int nn = snprintf(buf, sizeof(buf), "@@CLEAR_BLOCK@@ n=%u offset=%u rampage=%d mode=%d\n",
				s_cb_n, offset, rampage, (int)m_PageProtectInfo[rampage].Mode);
			write(STDERR_FILENO, buf, nn);
			s_cb_n++;
		}
	}
#endif

	// Assertion: This function should never be run on a block that's already under
	// manual protection.  Indicates a logic error in the recompiler or protection code.
	pxAssertMsg(m_PageProtectInfo[rampage].Mode != ProtMode_Manual,
		"Attempted to clear a block that is already under manual protection.");

	HostSys::MemProtect(&eeMem->Main[rampage << __pageshift], __pagesize, PageAccess_ReadWrite());
	vtlb_UpdateFastmemProtection(rampage << __pageshift, __pagesize, PageAccess_ReadWrite());
	m_PageProtectInfo[rampage].Mode = ProtMode_Manual;
	Cpu->Clear(m_PageProtectInfo[rampage].ReverseRamMap, __pagesize);
}

#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
std::atomic<uint64_t> g_hpf_cnt{0};
std::atomic<uintptr_t> g_hpf_last_fa{0};
std::atomic<uintptr_t> g_hpf_last_epc{0};
std::atomic<uint32_t> g_hpf_last_path{0}; // 0=unknown, 1=fastmem_write, 2=fastmem_bp, 3=outside, 4=eemem
#endif

PageFaultHandler::HandlerResult PageFaultHandler::HandlePageFault(void* exception_pc, void* fault_address, bool is_write)
{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
	uint64_t cnt = g_hpf_cnt.fetch_add(1, std::memory_order_relaxed);
	// [iter685] store fault info — only overwrite if real fault (non-zero fa) or first time
	uintptr_t fa_val = reinterpret_cast<uintptr_t>(fault_address);
	if (fa_val != 0 || cnt == 0) {
		g_hpf_last_fa.store(fa_val, std::memory_order_relaxed);
		g_hpf_last_epc.store(reinterpret_cast<uintptr_t>(exception_pc), std::memory_order_relaxed);
	}
#endif

	pxAssert(eeMem);

	u32 vaddr;
	if (CHECK_FASTMEM && vtlb_GetGuestAddress(reinterpret_cast<uptr>(fault_address), &vaddr))
	{
		// this was inside the fastmem area. check if it's a code page

		uptr ptr = (uptr)PSM(vaddr);
		uptr offset = (ptr - (uptr)eeMem->Main);
			if (ptr && m_PageProtectInfo[offset >> __pageshift].Mode == ProtMode_Write)
			{
				mmap_ClearCpuBlock(offset);
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
				g_hpf_last_path.store(1, std::memory_order_relaxed); // fastmem_write
#endif
				return HandlerResult::ContinueExecution;
			}
			else
			{
				bool ok = vtlb_BackpatchLoadStore(reinterpret_cast<uptr>(exception_pc),
						   reinterpret_cast<uptr>(fault_address));
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
				g_hpf_last_path.store(2, std::memory_order_relaxed); // fastmem_bp
#endif
				return ok ? HandlerResult::ContinueExecution : HandlerResult::ExecuteNextHandler;
			}
		}
	else
	{
		// get bad virtual address
			uptr offset = reinterpret_cast<uptr>(fault_address) - reinterpret_cast<uptr>(eeMem->Main);
			if (offset >= Ps2MemSize::ExposedRam)
			{
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
				g_hpf_last_path.store(3, std::memory_order_relaxed); // outside
#endif
				return HandlerResult::ExecuteNextHandler;
			}

			mmap_ClearCpuBlock(offset);
#if ARMSX2_ENABLE_EE_HOTPATH_DIAGNOSTICS
			g_hpf_last_path.store(4, std::memory_order_relaxed); // eemem
#endif
			return HandlerResult::ContinueExecution;
		}
	}

// Clears all block tracking statuses, manual protection flags, and write protection.
// This does not clear any recompiler blocks.  It is assumed (and necessary) for the caller
// to ensure the EErec is also reset in conjunction with calling this function.
//  (this function is called by default from the eerecReset).
void mmap_ResetBlockTracking()
{
	//DbgCon.WriteLn( "vtlb/mmap: Block Tracking reset..." );
	std::memset(m_PageProtectInfo, 0, sizeof(m_PageProtectInfo));
	if (eeMem)
		HostSys::MemProtect(eeMem->Main, Ps2MemSize::ExposedRam, PageAccess_ReadWrite());
	vtlb_UpdateFastmemProtection(0, Ps2MemSize::ExposedRam, PageAccess_ReadWrite());
}
