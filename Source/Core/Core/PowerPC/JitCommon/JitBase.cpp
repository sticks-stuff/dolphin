// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitCommon/JitBase.h"

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <unistd.h>
#endif

// The BLR optimization is nice, but it means that JITted code can overflow the
// native stack by repeatedly running BL.  (The chance of this happening in any
// retail game is close to 0, but correctness is correctness...) Also, the
// overflow might not happen directly in the JITted code but in a C++ function
// called from it, so we can't just adjust RSP in the case of a fault.
// Instead, we have to have extra stack space preallocated under the fault
// point which allows the code to continue, after wiping the JIT cache so we
// can reset things at a safe point.  Once this condition trips, the
// optimization is permanently disabled, under the assumption this will never
// happen in practice.

// On Unix, we just mark an appropriate region of the stack as PROT_NONE and
// handle it the same way as fastmem faults.  It's safe to take a fault with a
// bad RSP, because on Linux we can use sigaltstack and on OS X we're already
// on a separate thread.

// Windows is... under-documented.
// It already puts guard pages so it can automatically grow the stack and it
// doesn't look like there is a way to hook into a guard page fault and implement
// our own logic.
// But when windows reaches the last guard page, it raises a "Stack Overflow"
// exception which we can hook into, however by default it leaves you with less
// than 4kb of stack. So we use SetThreadStackGuarantee to trigger the Stack
// Overflow early while we still have 256kb of stack remaining.
// After resetting the stack to the top, we call _resetstkoflw() to restore
// the guard page at the 256kb mark.

const u8* JitBase::Dispatch(JitBase& jit)
{
  return jit.GetBlockCache()->Dispatch();
}

void JitTrampoline(JitBase& jit, u32 em_address)
{
  jit.Jit(em_address);
}

JitBase::JitBase() : m_code_buffer(code_buffer_size)
{
  m_registered_config_callback_id = Config::AddConfigChangedCallback(
      [this] { Core::RunAsCPUThread([this] { RefreshConfig(); }); });
  RefreshConfig();
}

JitBase::~JitBase()
{
  Config::RemoveConfigChangedCallback(m_registered_config_callback_id);
}

void JitBase::RefreshConfig()
{
  bJITOff = Config::Get(Config::MAIN_DEBUG_JIT_OFF);
  bJITLoadStoreOff = Config::Get(Config::MAIN_DEBUG_JIT_LOAD_STORE_OFF);
  bJITLoadStorelXzOff = Config::Get(Config::MAIN_DEBUG_JIT_LOAD_STORE_LXZ_OFF);
  bJITLoadStorelwzOff = Config::Get(Config::MAIN_DEBUG_JIT_LOAD_STORE_LWZ_OFF);
  bJITLoadStorelbzxOff = Config::Get(Config::MAIN_DEBUG_JIT_LOAD_STORE_LBZX_OFF);
  bJITLoadStoreFloatingOff = Config::Get(Config::MAIN_DEBUG_JIT_LOAD_STORE_FLOATING_OFF);
  bJITLoadStorePairedOff = Config::Get(Config::MAIN_DEBUG_JIT_LOAD_STORE_PAIRED_OFF);
  bJITFloatingPointOff = Config::Get(Config::MAIN_DEBUG_JIT_FLOATING_POINT_OFF);
  bJITIntegerOff = Config::Get(Config::MAIN_DEBUG_JIT_INTEGER_OFF);
  bJITPairedOff = Config::Get(Config::MAIN_DEBUG_JIT_PAIRED_OFF);
  bJITSystemRegistersOff = Config::Get(Config::MAIN_DEBUG_JIT_SYSTEM_REGISTERS_OFF);
  bJITBranchOff = Config::Get(Config::MAIN_DEBUG_JIT_BRANCH_OFF);
  bJITRegisterCacheOff = Config::Get(Config::MAIN_DEBUG_JIT_REGISTER_CACHE_OFF);
  m_enable_debugging = Config::Get(Config::MAIN_ENABLE_DEBUGGING);
  m_enable_float_exceptions = Config::Get(Config::MAIN_FLOAT_EXCEPTIONS);
  m_enable_div_by_zero_exceptions = Config::Get(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS);
  m_low_dcbz_hack = Config::Get(Config::MAIN_LOW_DCBZ_HACK);
  m_fprf = Config::Get(Config::MAIN_FPRF);
  m_accurate_nans = Config::Get(Config::MAIN_ACCURATE_NANS);
  m_fastmem_enabled = Config::Get(Config::MAIN_FASTMEM);
  m_mmu_enabled = Core::System::GetInstance().IsMMUMode();
  m_pause_on_panic_enabled = Core::System::GetInstance().IsPauseOnPanicMode();
  m_accurate_cpu_cache_enabled = Config::Get(Config::MAIN_ACCURATE_CPU_CACHE);
  if (m_accurate_cpu_cache_enabled)
  {
    m_fastmem_enabled = false;
    // This hack is unneeded if the data cache is being emulated.
    m_low_dcbz_hack = false;
  }

  analyzer.SetDebuggingEnabled(m_enable_debugging);
  analyzer.SetBranchFollowingEnabled(Config::Get(Config::MAIN_JIT_FOLLOW_BRANCH));
  analyzer.SetFloatExceptionsEnabled(m_enable_float_exceptions);
  analyzer.SetDivByZeroExceptionsEnabled(m_enable_div_by_zero_exceptions);
}

void JitBase::InitBLROptimization()
{
  m_enable_blr_optimization = jo.enableBlocklink && m_fastmem_enabled && !m_enable_debugging;
  m_cleanup_after_stackfault = false;
}

void JitBase::ProtectStack()
{
  if (!m_enable_blr_optimization)
    return;

#ifdef _WIN32
  ULONG reserveSize = SAFE_STACK_SIZE;
  SetThreadStackGuarantee(&reserveSize);
#else
  auto [stack_addr, stack_size] = Common::GetCurrentThreadStack();

  const uintptr_t stack_base_addr = reinterpret_cast<uintptr_t>(stack_addr);
  const uintptr_t stack_middle_addr = reinterpret_cast<uintptr_t>(&stack_addr);
  if (stack_middle_addr < stack_base_addr || stack_middle_addr >= stack_base_addr + stack_size)
  {
    PanicAlertFmt("Failed to get correct stack base");
    m_enable_blr_optimization = false;
    return;
  }

  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
  {
    PanicAlertFmt("Failed to get page size");
    m_enable_blr_optimization = false;
    return;
  }

  const uintptr_t stack_guard_addr = Common::AlignUp(stack_base_addr + GUARD_OFFSET, page_size);
  if (stack_guard_addr >= stack_middle_addr ||
      stack_middle_addr - stack_guard_addr < GUARD_SIZE + MIN_UNSAFE_STACK_SIZE)
  {
    PanicAlertFmt("Stack is too small for BLR optimization (size {:x}, base {:x}, current stack "
                  "pointer {:x}, alignment {:x})",
                  stack_size, stack_base_addr, stack_middle_addr, page_size);
    m_enable_blr_optimization = false;
    return;
  }

  m_stack_guard = reinterpret_cast<u8*>(stack_guard_addr);
  Common::ReadProtectMemory(m_stack_guard, GUARD_SIZE);
#endif
}

void JitBase::UnprotectStack()
{
#ifndef _WIN32
  if (m_stack_guard)
  {
    Common::UnWriteProtectMemory(m_stack_guard, GUARD_SIZE);
    m_stack_guard = nullptr;
  }
#endif
}

bool JitBase::HandleStackFault()
{
  // It's possible the stack fault might have been caused by something other than
  // the BLR optimization. If the fault was triggered from another thread, or
  // when BLR optimization isn't enabled then there is nothing we can do about the fault.
  // Return false so the regular stack overflow handler can trigger (which crashes)
  if (!m_enable_blr_optimization || !Core::IsCPUThread())
    return false;

  WARN_LOG_FMT(POWERPC, "BLR cache disabled due to excessive BL in the emulated program.");

  UnprotectStack();
  m_enable_blr_optimization = false;

  // We're going to need to clear the whole cache to get rid of the bad
  // CALLs, but we can't yet.  Fake the downcount so we're forced to the
  // dispatcher (no block linking), and clear the cache so we're sent to
  // Jit. In the case of Windows, we will also need to call _resetstkoflw()
  // to reset the guard page.
  // Yeah, it's kind of gross.
  GetBlockCache()->InvalidateICache(0, 0xffffffff, true);
  Core::System::GetInstance().GetCoreTiming().ForceExceptionCheck(0);
  m_cleanup_after_stackfault = true;

  return true;
}

void JitBase::CleanUpAfterStackFault()
{
  if (m_cleanup_after_stackfault)
  {
    ClearCache();
    m_cleanup_after_stackfault = false;
#ifdef _WIN32
    // The stack is in an invalid state with no guard page, reset it.
    _resetstkoflw();
#endif
  }
}

bool JitBase::CanMergeNextInstructions(int count) const
{
  auto& system = Core::System::GetInstance();
  if (system.GetCPU().IsStepping() || js.instructionsLeft < count)
    return false;
  // Be careful: a breakpoint kills flags in between instructions
  for (int i = 1; i <= count; i++)
  {
    if (m_enable_debugging && PowerPC::breakpoints.IsAddressBreakPoint(js.op[i].address))
      return false;
    if (js.op[i].isBranchTarget)
      return false;
  }
  return true;
}

void JitBase::UpdateMemoryAndExceptionOptions()
{
  bool any_watchpoints = PowerPC::memchecks.HasAny();
  jo.fastmem =
      m_fastmem_enabled && jo.fastmem_arena && (PowerPC::ppcState.msr.DR || !any_watchpoints);
  jo.memcheck = m_mmu_enabled || m_pause_on_panic_enabled || any_watchpoints;
  jo.fp_exceptions = m_enable_float_exceptions;
  jo.div_by_zero_exceptions = m_enable_div_by_zero_exceptions;
}

bool JitBase::ShouldHandleFPExceptionForInstruction(const PPCAnalyst::CodeOp* op)
{
  if (jo.fp_exceptions)
    return (op->opinfo->flags & FL_FLOAT_EXCEPTION) != 0;
  else if (jo.div_by_zero_exceptions)
    return (op->opinfo->flags & FL_FLOAT_DIV) != 0;
  else
    return false;
}
