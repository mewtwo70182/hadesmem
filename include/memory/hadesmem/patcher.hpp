// Copyright (C) 2010-2013 Joshua Boyce.
// See the file COPYING for copying permission.

#pragma once

#include <locale>
#include <memory>
#include <vector>
#include <climits>
#include <cstdint>
#include <sstream>

#include <hadesmem/detail/warning_disable_prefix.hpp>
#include <asmjit/asmjit.h>
#include <hadesmem/detail/warning_disable_suffix.hpp>

#include <hadesmem/detail/warning_disable_prefix.hpp>
#include <udis86.h>
#include <hadesmem/detail/warning_disable_suffix.hpp>

#include <windows.h>

#include <hadesmem/read.hpp>
#include <hadesmem/alloc.hpp>
#include <hadesmem/error.hpp>
#include <hadesmem/flush.hpp>
#include <hadesmem/write.hpp>
#include <hadesmem/process.hpp>

// TODO: Fix exception safety.

// TODO: EAT hooking.

// TODO: IAT hooking.

// TODO: VEH hooking. (INT 3, DR, invalid instr, etc.)

// TODO: Make hooking a transactional operation.

// TODO: Freeze target when hooking (except calling thread if applicable � 
// e.g. in injected code). Support doing this via both manual thread 
// enumeration (will require new Thread, ThreadList, etc APIs) and also 
// NtSuspendProcess. NtSuspendProcess method cannot be used on self? Or does 
// it handle this case and exclude the current thread?

// TODO: VMT hooking.

// TODO: Explicitly support (and test) hook chains.

// TODO: Support 'safe' unloading by incrementing/decrementing a counter for 
// each detour so it can be detect when your code is currently executing 
// before unloading? What other options are there?

// TODO: Support passing a hook context. (This is needed to support 
// multi-module support properly in base hook. i.e. Two concurrent D3D 
// instances.) Need to be sure not to dirty registers though.

// TODO: Add a 'thumbprint' to all memory allocations so the blocks can be 
// easily identified in a debugger.

namespace hadesmem
{

class PatchRaw
{
public:
  PatchRaw(Process const& process, PVOID target, 
    std::vector<BYTE> const& data)
    : process_(&process), 
    applied_(false), 
    target_(target), 
    data_(data), 
    orig_()
  { }
    
  PatchRaw(PatchRaw&& other) HADESMEM_NOEXCEPT
    : process_(other.process_), 
    applied_(other.applied_), 
    target_(other.target_), 
    data_(std::move(other.data_)), 
    orig_(std::move(other.orig_))
  {
    other.process_ = nullptr;
    other.applied_ = false;
    other.target_ = nullptr;
  }
    
  PatchRaw& operator=(PatchRaw&& other) HADESMEM_NOEXCEPT
  {
    RemoveUnchecked();
    
    process_ = other.process_;
    other.process_ = nullptr;
    applied_ = other.applied_;
    other.applied_ = false;
    target_ = other.target_;
    other.target_ = nullptr;
    data_ = std::move(other.data_);
    orig_ = std::move(other.orig_);
    return *this;
  }

  ~PatchRaw() HADESMEM_NOEXCEPT
  {
    RemoveUnchecked();
  }
  
  bool IsApplied() const HADESMEM_NOEXCEPT
  {
    return applied_;
  }
  
  void Apply()
  {
    if (applied_)
    {
      return;
    }

    orig_ = ReadVector<BYTE>(*process_, target_, data_.size());
      
    WriteVector(*process_, target_, data_);

    FlushInstructionCache(*process_, target_, data_.size());

    applied_ = true;
  }

  void Remove()
  {
    if (!applied_)
    {
      return;
    }

    WriteVector(*process_, target_, orig_);

    FlushInstructionCache(*process_, target_, orig_.size());

    applied_ = false;
  }
  
  // TODO: Code smell... This feels like code duplication between derived classes.
  void RemoveUnchecked() HADESMEM_NOEXCEPT
  {
    try
    {
      Remove();
    }
    catch (std::exception const& e)
    {
      (void)e;

      // WARNING: Patch may not be removed if Remove fails.
      // TODO: Add debug logging to other destructors.
      HADESMEM_TRACE_A(boost::diagnostic_information(e).c_str());
      HADESMEM_TRACE_A("\n");
      
      // TODO: Code smell... Should this be handled by the base class somehow?
      process_ = nullptr;
      applied_ = false;

      target_ = nullptr;
      data_.clear();
      orig_.clear();
    }
  }

private:
  PatchRaw(PatchRaw const& other) HADESMEM_DELETED_FUNCTION;
  PatchRaw& operator=(PatchRaw const& other) HADESMEM_DELETED_FUNCTION;
  
  Process const* process_;
  bool applied_;
  PVOID target_;
  std::vector<BYTE> data_;
  std::vector<BYTE> orig_;
};

// TODO: Consolidate memory allocations where possible. Taking a page for 
// every trampoline (including two trampolines per patch on x64 -- fix this 
// too) is extremely wasteful.
// TODO: Support calling convention differences for target function and 
// detour function (calling convention only though, not args or return type). 
// Templates can be used to detect information if appropriate.
class PatchDetour
{
public:
  PatchDetour(Process const& process, PVOID target, PVOID detour)
    : process_(&process), 
    applied_(false), 
    target_(target), 
    detour_(detour), 
    trampoline_(), 
    orig_(), 
    trampolines_()
  { }
    
  PatchDetour(PatchDetour&& other) HADESMEM_NOEXCEPT
    : process_(other.process_), 
    applied_(other.applied_), 
    target_(other.target_), 
    detour_(other.detour_), 
    trampoline_(std::move(other.trampoline_)), 
    orig_(std::move(other.orig_)), 
    trampolines_(std::move(other.trampolines_))
  {
    other.process_ = nullptr;
    other.applied_ = false;
    other.target_ = nullptr;
    other.detour_ = nullptr;
  }
   
  PatchDetour& operator=(PatchDetour&& other) HADESMEM_NOEXCEPT
  {
    RemoveUnchecked();
    
    process_ = other.process_;
    other.process_ = nullptr;
    applied_ = other.applied_;
    other.applied_ = false;
    target_ = other.target_;
    other.target_ = nullptr;
    detour_ = other.detour_;
    other.detour_ = nullptr;
    trampoline_ = std::move(other.trampoline_);
    orig_ = std::move(other.orig_);
    trampolines_ = std::move(other.trampolines_);
    return *this;
  }

  ~PatchDetour() HADESMEM_NOEXCEPT
  {
    RemoveUnchecked();
  }
  
  // TODO: Detect when applying or removing patch and code is currently being 
  // executed. (Redirect EIP to trampoline.)
  void Apply()
  {
    if (applied_)
    {
      return;
    }

    ULONG const kMaxInstructionLen = 15;
    ULONG const tramp_size = kMaxInstructionLen * 3;

    trampoline_.reset(new Allocator(*process_, tramp_size));
    PBYTE tramp_cur = static_cast<PBYTE>(trampoline_->GetBase());
    
    std::vector<BYTE> buffer(ReadVector<BYTE>(*process_, target_, 
      tramp_size));

    ud_t ud_obj;
    ud_init(&ud_obj);
    ud_set_input_buffer(&ud_obj, buffer.data(), buffer.size());
    ud_set_syntax(&ud_obj, UD_SYN_INTEL);
    ud_set_pc(&ud_obj, reinterpret_cast<std::uint64_t>(target_));
#if defined(_M_AMD64) 
    ud_set_mode(&ud_obj, 64);
#elif defined(_M_IX86) 
    ud_set_mode(&ud_obj, 32);
#else 
#error "[HadesMem] Unsupported architecture."
#endif
    
    // TODO: Detect cases where hooking may overflow past the end of a 
    // function, and fail. (Provide policy or flag to allow overriding this 
    // behaviour.) Examples may be instructions such as int 3, ret, jmp, etc.
    unsigned int instr_size = 0;
    do
    {
      unsigned int const len = ud_disassemble(&ud_obj);
      if (len == 0)
      {
        BOOST_THROW_EXCEPTION(Error() << 
          ErrorString("Disassembly failed."));
      }
        
#ifndef NDEBUG
      char const* const asm_str = ud_insn_asm(&ud_obj);
      std::string asm_str_full = "PatchDetour::Apply: ";
      asm_str_full += (asm_str ? asm_str : "Disassembly printing failed.");
      asm_str_full += ' ';
      asm_str_full += '[';
      char const* const asm_bytes_str = ud_insn_hex(&ud_obj);
      asm_str_full += (asm_bytes_str ? asm_bytes_str : 
        "Disassembly bytes printing failed.");
      asm_str_full += ']';
      asm_str_full += '\n';
      OutputDebugStringA(asm_str_full.c_str());
#endif
      
      // TODO: Improved relative instruction rebuilding (including conditionals). 
      // x64 has far more IP relative instructions than x86.
      // TODO: Support more operand sizes for existing relative instruction support.
      // TODO: Improve instruction rebuilding for cases such as jumps 
      // backwards into the detour and fail safely (or whatever is appropriate).
      ud_operand_t const* op = ud_insn_opr(&ud_obj, 0);
      std::size_t const sdword_size_bits = sizeof(std::int32_t) * CHAR_BIT;
      if ((ud_obj.mnemonic == UD_Ijmp || ud_obj.mnemonic == UD_Icall) && 
        op && 
        op->type == UD_OP_JIMM && 
        op->size == sdword_size_bits)
      {
        std::uint64_t const insn_base = ud_insn_off(&ud_obj);
        std::int32_t const insn_target = op->lval.sdword;
        unsigned int const insn_len = ud_insn_len(&ud_obj);
        PVOID jump_target = reinterpret_cast<PBYTE>(
          static_cast<DWORD_PTR>(insn_base)) + insn_target + insn_len;
#ifndef NDEBUG
        std::stringstream jmp_ss;
        jmp_ss.imbue(std::locale::classic());
        jmp_ss << std::hex << jump_target;
        std::string const jump_str = "Jump target is " + jmp_ss.str() + ".\n";
        OutputDebugStringA(jump_str.c_str());
#endif
        if (ud_obj.mnemonic == UD_Ijmp)
        {
          WriteJump(tramp_cur, jump_target);
          tramp_cur += GetJumpSize();
        }
        else
        {
          HADESMEM_ASSERT(ud_obj.mnemonic == UD_Icall);
          WriteCall(tramp_cur, jump_target);
          tramp_cur += GetCallSize();
        }
      }
      else
      {
        uint8_t const* raw = ud_insn_ptr(&ud_obj);
        Write(*process_, tramp_cur, raw, raw + len);
        tramp_cur += len;
      }

      instr_size += len;
    } while (instr_size < GetJumpSize());

    WriteJump(tramp_cur, static_cast<PBYTE>(target_) + instr_size);
    tramp_cur += GetJumpSize();

    FlushInstructionCache(*process_, trampoline_->GetBase(), 
      instr_size + GetJumpSize());

    orig_ = ReadVector<BYTE>(*process_, target_, GetJumpSize());

    WriteJump(target_, detour_);

    FlushInstructionCache(*process_, target_, orig_.size());

    applied_ = true;
  }
    
  void Remove()
  {
    if (!applied_)
    {
      return;
    }

    WriteVector(*process_, target_, orig_);

    trampoline_.reset();

    trampolines_.clear();

    applied_ = false;
  }

  PVOID GetTrampoline() const HADESMEM_NOEXCEPT
  {
    return trampoline_->GetBase();
  }

  template <typename FuncT>
  FuncT GetTrampoline() const HADESMEM_NOEXCEPT
  {
    return reinterpret_cast<FuncT>(reinterpret_cast<DWORD_PTR>(
      trampoline_->GetBase()));
  }
  
  // TODO: Code smell... This feels like code duplication between derived classes.
  void RemoveUnchecked() HADESMEM_NOEXCEPT
  {
    try
    {
      Remove();
    }
    catch (std::exception const& e)
    {
      (void)e;

      // WARNING: Patch may not be removed if Remove fails.
      // TODO: Add debug logging to other destructors.
      HADESMEM_TRACE_A(boost::diagnostic_information(e).c_str());
      HADESMEM_TRACE_A("\n");
      
      // TODO: Code smell... Should this be handled by the base class somehow?
      process_ = nullptr;
      applied_ = false;

      target_ = nullptr;
      detour_ = nullptr;
      trampoline_.reset();
      orig_.clear();
      trampolines_.clear();
    }
  }

private:
  PatchDetour(PatchDetour const& other) HADESMEM_DELETED_FUNCTION;
  PatchDetour& operator=(PatchDetour const& other) HADESMEM_DELETED_FUNCTION;

#if defined(_M_AMD64)
  // Inspired by EasyHook.
  std::unique_ptr<Allocator> AllocTrampolineNear(PVOID address)
  {
    SYSTEM_INFO sys_info;
    ZeroMemory(&sys_info, sizeof(sys_info));
    GetSystemInfo(&sys_info);
    DWORD page_size = sys_info.dwPageSize;

    LONG_PTR const search_beg = 
      (std::max)(reinterpret_cast<LONG_PTR>(address) - 0x7FFFFF00LL, 
      reinterpret_cast<LONG_PTR>(sys_info.lpMinimumApplicationAddress));
    LONG_PTR const search_end = 
      (std::min)(reinterpret_cast<LONG_PTR>(address) + 0x7FFFFF00LL, 
      reinterpret_cast<LONG_PTR>(sys_info.lpMaximumApplicationAddress));

    std::unique_ptr<Allocator> trampoline;

    for (LONG_PTR base = reinterpret_cast<LONG_PTR>(address), index = 0;
      base + index < search_end || base - index > search_beg;
      index += page_size)
    {
      LONG_PTR const higher = base + index;
      if (higher < search_end)
      {
        try
        {
          trampoline.reset(new Allocator(*process_, reinterpret_cast<PVOID>(higher), page_size));
          break;
        }
        catch (std::exception const& /*e*/)
        { }
      }

      LONG_PTR const lower = base - index;
      if (lower > search_beg)
      {
        try
        {
          trampoline.reset(new Allocator(*process_, reinterpret_cast<PVOID>(lower), page_size));
          break;
        }
        catch (std::exception const& /*e*/)
        { }
      }
    }

    if (!trampoline)
    {
      BOOST_THROW_EXCEPTION(Error() << 
        ErrorString("Failed to find trampoline memory block."));
    }

    return trampoline;
  }
#elif defined(_M_IX86) 
#else 
#error "[HadesMem] Unsupported architecture."
#endif

  void WriteJump(PVOID address, PVOID target)
  {
    AsmJit::X86Assembler jit;

#if defined(_M_AMD64) 
    // TODO: Fall back to PUSH/RET trick (14 bytes) if finding a trampoline 
    // fails.
    // TODO: Use relative jumps where possible (detect delta at 
    // runtime). Saves a byte and a trampoline allocation.
    // TODO: When we allocate a trampoline near an address, we should just 
    // write our real trampoline there, rather than using a second layer 
    // of indirection (which could easily result in virtual address space 
    // exhaustion, given we take a full page for every trampoline).
    std::unique_ptr<Allocator> trampoline(AllocTrampolineNear(address));

    PVOID tramp_addr = trampoline->GetBase();

    Write(*process_, tramp_addr, reinterpret_cast<DWORD_PTR>(target));

    trampolines_.emplace_back(std::move(trampoline));

    AsmJit::Label label(jit.newLabel());
    jit.bind(label);
    // JMP QWORD PTR <Trampoline, Relative>
    LONG_PTR const disp = reinterpret_cast<LONG_PTR>(tramp_addr) - 
      reinterpret_cast<LONG_PTR>(address) - 
      sizeof(LONG);
    jit.jmp(AsmJit::qword_ptr(label, static_cast<sysint_t>(disp)));
#elif defined(_M_IX86) 
    // JMP <Target, Relative>
    jit.jmp(target);
#else 
#error "[HadesMem] Unsupported architecture."
#endif

    DWORD_PTR const stub_size = jit.getCodeSize();
    if (stub_size != GetJumpSize())
    {
      BOOST_THROW_EXCEPTION(Error() << 
        ErrorString("Unexpected stub size."));
    }

    std::vector<BYTE> jump_buf(stub_size);
      
    jit.relocCode(jump_buf.data(), reinterpret_cast<DWORD_PTR>(address));

    WriteVector(*process_, address, jump_buf);
  }
  
  void WriteCall(PVOID address, PVOID target)
  {
    AsmJit::X86Assembler jit;

#if defined(_M_AMD64) 
    std::unique_ptr<Allocator> trampoline(AllocTrampolineNear(address));

    PVOID tramp_addr = trampoline->GetBase();

    Write(*process_, tramp_addr, reinterpret_cast<DWORD_PTR>(target));

    trampolines_.emplace_back(std::move(trampoline));

    AsmJit::Label label(jit.newLabel());
    jit.bind(label);
    // CALL QWORD PTR <Trampoline, Relative>
    LONG_PTR const disp = reinterpret_cast<LONG_PTR>(tramp_addr) - 
      reinterpret_cast<LONG_PTR>(address) - 
      sizeof(LONG);
    jit.call(AsmJit::qword_ptr(label, static_cast<sysint_t>(disp)));
#elif defined(_M_IX86) 
    // CALL <Target, Relative>
    jit.call(target);
#else 
#error "[HadesMem] Unsupported architecture."
#endif

    DWORD_PTR const stub_size = jit.getCodeSize();
    if (stub_size != GetCallSize())
    {
      BOOST_THROW_EXCEPTION(Error() << 
        ErrorString("Unexpected stub size."));
    }

    std::vector<BYTE> jump_buf(stub_size);
      
    jit.relocCode(jump_buf.data(), reinterpret_cast<DWORD_PTR>(address));

    WriteVector(*process_, address, jump_buf);
  }

  unsigned int GetJumpSize() const HADESMEM_NOEXCEPT
  {
#if defined(_M_AMD64) 
    return 6;
#elif defined(_M_IX86) 
    return 5;
#else 
#error "[HadesMem] Unsupported architecture."
#endif
  }

  unsigned int GetCallSize() const HADESMEM_NOEXCEPT
  {
#if defined(_M_AMD64) 
    return 6;
#elif defined(_M_IX86) 
    return 5;
#else 
#error "[HadesMem] Unsupported architecture."
#endif
  }
  
  Process const* process_;
  bool applied_;
  PVOID target_;
  PVOID detour_;
  std::unique_ptr<Allocator> trampoline_;
  std::vector<BYTE> orig_;
  std::vector<std::unique_ptr<Allocator>> trampolines_;
};

}