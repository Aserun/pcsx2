/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */


#include "PrecompiledHeader.h"

#include "Common.h"
#include "vtlb.h"

#include "iCore.h"
#include "iR5900.h"

using namespace vtlb_private;
using namespace x86Emitter;

//////////////////////////////////////////////////////////////////////////////////////////
// iAllocRegSSE -- allocates an xmm register.  If no xmm register is available, xmm0 is
// saved into g_globalXMMData and returned as a free register.
//
class iAllocRegSSE
{
protected:
	xRegisterSSE m_reg;
	bool m_free;

public:
	iAllocRegSSE() :
		m_reg( xmm0 ),
		m_free( !!_hasFreeXMMreg() )
	{
		if( m_free )
			m_reg = xRegisterSSE( _allocTempXMMreg( XMMT_INT, -1 ) );
		else
			xStoreReg( m_reg );
	}

	~iAllocRegSSE()
	{
		if( m_free )
			_freeXMMreg( m_reg.Id );
		else
			xRestoreReg( m_reg );
	}

	operator xRegisterSSE() const { return m_reg; }
};

// Moves 128 bits from point B to point A, using SSE's MOVAPS (or MOVDQA).
// This instruction always uses an SSE register, even if all registers are allocated!  It
// saves an SSE register to memory first, performs the copy, and restores the register.
//
static void iMOV128_SSE( const xIndirectVoid& destRm, const xIndirectVoid& srcRm )
{
	iAllocRegSSE reg;
	xMOVDQA( reg, srcRm );
	xMOVDQA( destRm, reg );
}

// Moves 64 bits of data from point B to point A, using either MMX, SSE, or x86 registers
// if neither MMX nor SSE is available to the task.
//
// Optimizations: This method uses MMX is the cpu is in MMX mode, or SSE if it's in FPU
// mode (saving on potential xEMMS uses).
//
static void iMOV64_Smart( const xIndirectVoid& destRm, const xIndirectVoid& srcRm )
{
	if( (x86FpuState == FPU_STATE) && _hasFreeXMMreg() )
	{
		// Move things using MOVLPS:
		xRegisterSSE reg( _allocTempXMMreg( XMMT_INT, -1 ) );
		xMOVL.PS( reg, srcRm );
		xMOVL.PS( destRm, reg );
		_freeXMMreg( reg.Id );
		return;
	}

	if( _hasFreeMMXreg() )
	{
		xRegisterMMX reg( _allocMMXreg(-1, MMX_TEMP, 0) );
		xMOVQ( reg, srcRm );
		xMOVQ( destRm, reg );
		_freeMMXreg( reg.Id );
	}
	else
	{
		xMOV( eax, srcRm );
		xMOV( destRm, eax );
		xMOV( eax, srcRm+4 );
		xMOV( destRm+4, eax );
	}
}

/*
	// Pseudo-Code For the following Dynarec Implementations -->

	u32 vmv=vmap[addr>>VTLB_PAGE_BITS];
	s32 ppf=addr+vmv;
	if (!(ppf<0))
	{
		data[0]=*reinterpret_cast<DataType*>(ppf);
		if (DataSize==128)
			data[1]=*reinterpret_cast<DataType*>(ppf+8);
		return 0;
	}
	else
	{
		//has to: translate, find function, call function
		u32 hand=(u8)vmv;
		u32 paddr=ppf-hand+0x80000000;
		//Console.WriteLn("Translated 0x%08X to 0x%08X",params addr,paddr);
		return reinterpret_cast<TemplateHelper<DataSize,false>::HandlerType*>(RWFT[TemplateHelper<DataSize,false>::sidx][0][hand])(paddr,data);
	}

	// And in ASM it looks something like this -->

	mov eax,ecx;
	shr eax,VTLB_PAGE_BITS;
	mov eax,[eax*4+vmap];
	add ecx,eax;
	js _fullread;

	//these are wrong order, just an example ...
	mov [eax],ecx;
	mov ecx,[edx];
	mov [eax+4],ecx;
	mov ecx,[edx+4];
	mov [eax+4+4],ecx;
	mov ecx,[edx+4+4];
	mov [eax+4+4+4+4],ecx;
	mov ecx,[edx+4+4+4+4];
	///....

	jmp cont;
	_fullread:
	movzx eax,al;
	sub   ecx,eax;
	sub   ecx,0x80000000;
	call [eax+stuff];
	cont:
	........

*/

namespace vtlb_private
{
	// ------------------------------------------------------------------------
	// Prepares eax, ecx, and, ebx for Direct or Indirect operations.
	// Returns the writeback pointer for ebx (return address from indirect handling)
	//
	static uptr* DynGen_PrepRegs()
	{
		// Warning dirty ebx (in case someone got the very bad idea to move this code)
		EE::Profiler.EmitMem();

		xMOV( eax, ecx );
		xSHR( eax, VTLB_PAGE_BITS );
		xMOV( eax, ptr[(eax*4) + vtlbdata.vmap] );
		xMOV( ebx, 0xcdcdcdcd );
		uptr* writeback = ((uptr*)xGetPtr()) - 1;
		xADD( ecx, eax );

		return writeback;
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectRead( u32 bits, bool sign )
	{
		switch( bits )
		{
			case 0:
			case 8:
				if( sign )
					xMOVSX( eax, ptr8[ecx] );
				else
					xMOVZX( eax, ptr8[ecx] );
			break;

			case 1:
			case 16:
				if( sign )
					xMOVSX( eax, ptr16[ecx] );
				else
					xMOVZX( eax, ptr16[ecx] );
			break;

			case 2:
			case 32:
				xMOV( eax, ptr[ecx] );
			break;

			case 3:
			case 64:
				iMOV64_Smart( ptr[edx], ptr[ecx] );
			break;

			case 4:
			case 128:
				iMOV128_SSE( ptr[edx], ptr[ecx] );
			break;

			jNO_DEFAULT
		}
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectWrite( u32 bits )
	{
		switch(bits)
		{
			//8 , 16, 32 : data on EDX
			case 0:
			case 8:
				xMOV( ptr[ecx], dl );
			break;

			case 1:
			case 16:
				xMOV( ptr[ecx], dx );
			break;

			case 2:
			case 32:
				xMOV( ptr[ecx], edx );
			break;

			case 3:
			case 64:
				iMOV64_Smart( ptr[ecx], ptr[edx] );
			break;

			case 4:
			case 128:
				iMOV128_SSE( ptr[ecx], ptr[edx] );
			break;
		}
	}
}

// ------------------------------------------------------------------------
// allocate one page for our naked indirect dispatcher function.
// this *must* be a full page, since we'll give it execution permission later.
// If it were smaller than a page we'd end up allowing execution rights on some
// other vars additionally (bad!).
//
static __pagealigned u8 m_IndirectDispatchers[__pagesize];

// ------------------------------------------------------------------------
// mode        - 0 for read, 1 for write!
// operandsize - 0 thru 4 represents 8, 16, 32, 64, and 128 bits.
//
static u8* GetIndirectDispatcherPtr( int mode, int operandsize, int sign = 0 )
{
	assert(mode || operandsize >= 2 ? !sign : true);

	// Each dispatcher is aligned to 64 bytes.  The actual dispatchers are only like
	// 20-some bytes each, but 64 byte alignment on functions that are called
	// more frequently than a hot sex hotline at 1:15am is probably a good thing.

	// 7*64? 5 widths with two sign extension modes for 8 and 16 bit reads

	// Gregory: a 32 bytes alignment is likely enough and more cache friendly
	const int A = 32;

	return &m_IndirectDispatchers[(mode*(7*A)) + (sign*5*A) + (operandsize*A)];
}

static u8* GetFullTlbDispatcherPtr( int mode, int operandsize, int sign = 0 )
{
	if (operandsize > 6) {
		switch(operandsize)
		{
			case 8:		operandsize=0;	break;
			case 16:	operandsize=1;	break;
			case 32:	operandsize=2;	break;
			case 64:	operandsize=3;	break;
			case 128:	operandsize=4;	break;
						jNO_DEFAULT;
		}
	}
	assert(mode || operandsize >= 2 ? !sign : true);

	// Full TLB dispatcher are bigger than standard dispatcher.
	const int A = 64;

	return &m_IndirectDispatchers[512 + (mode*(7*A)) + (sign*5*A) + (operandsize*A)];
}

// ------------------------------------------------------------------------
// Generates a JS instruction that targets the appropriate templated instance of
// the vtlb Indirect Dispatcher.
//
static void DynGen_IndirectDispatch( int mode, int bits, bool sign = false )
{
	int szidx = 0;
	switch( bits )
	{
		case 8:		szidx=0;	break;
		case 16:	szidx=1;	break;
		case 32:	szidx=2;	break;
		case 64:	szidx=3;	break;
		case 128:	szidx=4;	break;
		jNO_DEFAULT;
	}
	xJS( GetIndirectDispatcherPtr( mode, szidx, sign ) );
}

// ------------------------------------------------------------------------
// Generates a JAE instruction that targets the appropriate templated instance of
// the vtlb full tlb Dispatcher.
//
static void DynGen_FullTlbDispatch( int mode, int bits, bool sign = false )
{
	int szidx = 0;
	switch( bits )
	{
		case 8:		szidx=0;	break;
		case 16:	szidx=1;	break;
		case 32:	szidx=2;	break;
		case 64:	szidx=3;	break;
		case 128:	szidx=4;	break;
		jNO_DEFAULT;
	}
	xJAE( GetFullTlbDispatcherPtr( mode, szidx, sign ) );
}

// ------------------------------------------------------------------------
// Generates the various instances of the indirect dispatchers
static void DynGen_IndirectTlbDispatcher( int mode, int bits, bool sign )
{
	xMOVZX( eax, al );
	xSUB( ecx, 0x80000000 );
	xSUB( ecx, eax );

	// jump to the indirect handler, which is a __fastcall C++ function.
	// [ecx is address, edx is data]
	xFastCall(ptr32[(eax*4) + vtlbdata.RWFT[bits][mode]], ecx, edx);

	if (!mode)
	{
		if (bits == 0)
		{
			if (sign)
				xMOVSX(eax, al);
			else
				xMOVZX(eax, al);
		}
		else if (bits == 1)
		{
			if (sign)
				xMOVSX(eax, ax);
			else
				xMOVZX(eax, ax);
		}
	}

	xJMP( ebx );
}

static void DynGen_FullTlbDispatcher( int mode, int bits, bool sign)
{
	// code is a concatenation of DynGen_PrepRegs / DynGen_IndirectDispatch / DynGen_Direct Read|Write

	// In the future, this code will only be called when a direct access is failling (due to SIGSEGV)
	// It would allow to reduce the code complexity

	// WARNING: only 64 bytes are reserved by handler. Profiler overhead is 14 bytes for EmitSlowMem
	// + 17 bytes for EmitMem
	EE::Profiler.EmitSlowMem();

	// Equivalent to DynGen_PrepRegs (without ebx)
	xMOV( eax, ecx );
	xSHR( eax, VTLB_PAGE_BITS );
	xMOV( eax, ptr[(eax*4) + vtlbdata.vmap] );
	xADD( ecx, eax );

	xJS( GetIndirectDispatcherPtr( mode, bits, sign ) );

	if (mode == 1)
		DynGen_DirectWrite( bits );
	else
		DynGen_DirectRead( bits, sign );

	// quit dispatcher in case of direct read/write
	xJMP(ebx);
}

// One-time initialization procedure.  Multiple subsequent calls during the lifespan of the
// process will be ignored.
//
void vtlb_dynarec_init()
{
	static bool hasBeenCalled = false;
	if (hasBeenCalled) return;
	hasBeenCalled = true;

	// In case init gets called multiple times:
	HostSys::MemProtectStatic( m_IndirectDispatchers, PageAccess_ReadWrite() );

	// clear the buffer to 0xcc (easier debugging).
	memset_8<0xcc,0x1000>( m_IndirectDispatchers );

	for( int mode=0; mode<2; ++mode )
	{
		for( int bits=0; bits<5; ++bits )
		{
			for (int sign = 0; sign < (!mode && bits < 2 ? 2 : 1); sign++)
			{
				xSetPtr( GetIndirectDispatcherPtr( mode, bits, !!sign ) );

				DynGen_IndirectTlbDispatcher( mode, bits, sign );

				xSetPtr( GetFullTlbDispatcherPtr( mode, bits, !!sign ) );

				DynGen_FullTlbDispatcher( mode, bits, sign );
			}
		}
	}

	HostSys::MemProtectStatic( m_IndirectDispatchers, PageAccess_ExecOnly() );
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Load Implementations
void vtlb_DynGenRead64(u32 bits)
{
	pxAssume( bits == 64 || bits == 128 );

	uptr* writeback = DynGen_PrepRegs();

	DynGen_IndirectDispatch( 0, bits );
	DynGen_DirectRead( bits, false );

	*writeback = (uptr)xGetPtr();		// return target for indirect's call/ret
}

// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
void vtlb_DynGenRead32(u32 bits, bool sign)
{
	pxAssume( bits <= 32 );

	uptr* writeback = DynGen_PrepRegs();

	DynGen_IndirectDispatch( 0, bits, sign && bits < 32 );
	DynGen_DirectRead( bits, sign );

	*writeback = (uptr)xGetPtr();
}

// ------------------------------------------------------------------------
// Wrapper to the different load implementation
void vtlb_DynGenRead(u32 likely_address, u32 bits, bool sign)
{
	if (bits < 64)
		vtlb_DynGenRead32(bits, sign);
	else
		vtlb_DynGenRead64(bits);
}


// ------------------------------------------------------------------------
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
void vtlb_DynGenRead64_Const( u32 bits, u32 addr_const )
{
	EE::Profiler.EmitConstMem(addr_const);

	u32 vmv_ptr = vtlbdata.vmap[addr_const>>VTLB_PAGE_BITS];
	s32 ppf = addr_const + vmv_ptr;
	if( ppf >= 0 )
	{
		switch( bits )
		{
			case 64:
				iMOV64_Smart( ptr[edx], ptr[(void*)ppf] );
			break;

			case 128:
				iMOV128_SSE( ptr[edx], ptr[(void*)ppf] );
			break;

			jNO_DEFAULT
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 handler = (u8)vmv_ptr;
		u32 paddr = ppf - handler + 0x80000000;

		int szidx = 0;
		switch( bits )
		{
			case 64:	szidx=3;	break;
			case 128:	szidx=4;	break;
		}

		iFlushCall(FLUSH_FULLVTLB);
		xFastCall( vtlbdata.RWFT[szidx][0][handler], paddr );
	}
}

// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
//
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
//
void vtlb_DynGenRead32_Const( u32 bits, bool sign, u32 addr_const )
{
	EE::Profiler.EmitConstMem(addr_const);

	u32 vmv_ptr = vtlbdata.vmap[addr_const>>VTLB_PAGE_BITS];
	s32 ppf = addr_const + vmv_ptr;
	if( ppf >= 0 )
	{
		switch( bits )
		{
			case 8:
				if( sign )
					xMOVSX( eax, ptr8[(u8*)ppf] );
				else
					xMOVZX( eax, ptr8[(u8*)ppf] );
			break;

			case 16:
				if( sign )
					xMOVSX( eax, ptr16[(u16*)ppf] );
				else
					xMOVZX( eax, ptr16[(u16*)ppf] );
			break;

			case 32:
				xMOV( eax, ptr[(void*)ppf] );
			break;
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 handler = (u8)vmv_ptr;
		u32 paddr = ppf - handler + 0x80000000;

		int szidx = 0;
		switch( bits )
		{
			case 8:		szidx=0;	break;
			case 16:	szidx=1;	break;
			case 32:	szidx=2;	break;
		}

		// Shortcut for the INTC_STAT register, which many games like to spin on heavily.
		if( (bits == 32) && !EmuConfig.Speedhacks.IntcStat && (paddr == INTC_STAT) )
		{
			xMOV( eax, ptr[&psHu32( INTC_STAT )] );
		}
		else
		{
			iFlushCall(FLUSH_FULLVTLB);
			xFastCall( vtlbdata.RWFT[szidx][0][handler], paddr );

			// perform sign extension on the result:

			if( bits==8 )
			{
				if( sign )
					xMOVSX( eax, al );
				else
					xMOVZX( eax, al );
			}
			else if( bits==16 )
			{
				if( sign )
					xMOVSX( eax, ax );
				else
					xMOVZX( eax, ax );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Store Implementations

void vtlb_DynGenWrite(u32 sz)
{
	uptr* writeback = DynGen_PrepRegs();

	DynGen_IndirectDispatch( 1, sz );
	DynGen_DirectWrite( sz );

	*writeback = (uptr)xGetPtr();
}

// ------------------------------------------------------------------------
// Wrapper to the different load implementation
void vtlb_DynGenWrite(u32 likely_address, u32 bits)
{
	vtlb_DynGenWrite(bits);
}

// ------------------------------------------------------------------------
// Generates code for a store instruction, where the address is a known constant.
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
void vtlb_DynGenWrite_Const( u32 bits, u32 addr_const )
{
	EE::Profiler.EmitConstMem(addr_const);

	u32 vmv_ptr = vtlbdata.vmap[addr_const>>VTLB_PAGE_BITS];
	s32 ppf = addr_const + vmv_ptr;
	if( ppf >= 0 )
	{
		switch(bits)
		{
			//8 , 16, 32 : data on EDX
			case 8:
				xMOV( ptr[(void*)ppf], dl );
			break;

			case 16:
				xMOV( ptr[(void*)ppf], dx );
			break;

			case 32:
				xMOV( ptr[(void*)ppf], edx );
			break;

			case 64:
				iMOV64_Smart( ptr[(void*)ppf], ptr[edx] );
			break;

			case 128:
				iMOV128_SSE( ptr[(void*)ppf], ptr[edx] );
			break;
		}

	}
	else
	{
		// has to: translate, find function, call function
		u32 handler = (u8)vmv_ptr;
		u32 paddr = ppf - handler + 0x80000000;

		int szidx = 0;
		switch( bits )
		{
			case 8:  szidx=0;	break;
			case 16:   szidx=1;	break;
			case 32:   szidx=2;	break;
			case 64:   szidx=3;	break;
			case 128:   szidx=4; break;
		}

		iFlushCall(FLUSH_FULLVTLB);
		xFastCall( vtlbdata.RWFT[szidx][1][handler], paddr, edx );
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
//							Extra Implementations

//   ecx - virtual address
//   Returns physical address in eax.
void vtlb_DynV2P()
{
	xMOV(eax, ecx);
	xAND(ecx, VTLB_PAGE_MASK); // vaddr & VTLB_PAGE_MASK

	xSHR(eax, VTLB_PAGE_BITS);
	xMOV(eax, ptr[(eax*4) + vtlbdata.ppmap]); //vtlbdata.ppmap[vaddr>>VTLB_PAGE_BITS];

	xOR(eax, ecx);
}
