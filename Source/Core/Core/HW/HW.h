// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class PointerWrap;
struct Sram;

namespace HW
{
void Init(const Sram* override_sram, const std::string current_file_name);
void Shutdown();
void DoState(PointerWrap& p);
}  // namespace HW
