// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#include "Core/HW/HW.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/HSP/HSP.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/MemoryInterface.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WII_IPC.h"
#include "Core/IOS/IOS.h"
#include "Core/State.h"
#include "Core/System.h"

namespace HW
{
void Init(const Sram* override_sram, const std::string current_file_name)
{
  auto& system = Core::System::GetInstance();
  system.GetCoreTiming().Init();
  SystemTimers::PreInit();

  State::Init();

  // Init the whole Hardware
  system.GetAudioInterface().Init();
  system.GetVideoInterface().Init();
  SerialInterface::Init();
  system.GetProcessorInterface().Init();
  system.GetExpansionInterface().Init(override_sram, current_file_name);  // Needs to be initialized before Memory
  system.GetHSP().Init();
  system.GetMemory().Init();  // Needs to be initialized before AddressSpace
  AddressSpace::Init();
  system.GetMemoryInterface().Init();
  system.GetDSP().Init(Config::Get(Config::MAIN_DSP_HLE));
  system.GetDVDInterface().Init();
  system.GetGPFifo().Init();
  system.GetCPU().Init(Config::Get(Config::MAIN_CPU_CORE));
  SystemTimers::Init();

  if (SConfig::GetInstance().bWii)
  {
    IOS::Init();
    IOS::HLE::Init();  // Depends on Memory
  }
}

void Shutdown()
{
  auto& system = Core::System::GetInstance();

  // IOS should always be shut down regardless of bWii because it can be running in GC mode (MIOS).
  IOS::HLE::Shutdown();  // Depends on Memory
  IOS::Shutdown();

  SystemTimers::Shutdown();
  system.GetCPU().Shutdown();
  system.GetDVDInterface().Shutdown();
  system.GetDSP().Shutdown();
  system.GetExpansionInterface().Shutdown();
  system.GetMemoryInterface().Shutdown();
  AddressSpace::Shutdown();
  system.GetMemory().Shutdown();
  system.GetHSP().Shutdown();
  SerialInterface::Shutdown();
  system.GetAudioInterface().Shutdown();

  State::Shutdown();
  system.GetCoreTiming().Shutdown();
}

void DoState(PointerWrap& p)
{
  auto& system = Core::System::GetInstance();
  system.GetMemory().DoState(p);
  p.DoMarker("Memory");
  system.GetMemoryInterface().DoState(p);
  p.DoMarker("MemoryInterface");
  system.GetVideoInterface().DoState(p);
  p.DoMarker("VideoInterface");
  SerialInterface::DoState(p);
  p.DoMarker("SerialInterface");
  system.GetProcessorInterface().DoState(p);
  p.DoMarker("ProcessorInterface");
  system.GetDSP().DoState(p);
  p.DoMarker("DSP");
  system.GetDVDInterface().DoState(p);
  p.DoMarker("DVDInterface");
  system.GetGPFifo().DoState(p);
  p.DoMarker("GPFifo");
  system.GetExpansionInterface().DoState(p);
  p.DoMarker("ExpansionInterface");
  system.GetAudioInterface().DoState(p);
  p.DoMarker("AudioInterface");
  system.GetHSP().DoState(p);
  p.DoMarker("HSP");

  if (SConfig::GetInstance().bWii)
  {
    IOS::DoState(p);
    p.DoMarker("IOS");
    IOS::HLE::GetIOS()->DoState(p);
    p.DoMarker("IOS::HLE");
  }

  p.DoMarker("WIIHW");
}
}  // namespace HW
