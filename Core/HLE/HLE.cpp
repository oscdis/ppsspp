// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "HLE.h"
#include <map>
#include <vector>
#include "../MemMap.h"

#include "HLETables.h"
#include "../System.h"
#include "sceDisplay.h"
#include "sceIo.h"
#include "sceAudio.h"
#include "sceKernelMemory.h"
#include "sceKernelThread.h"
#include "sceKernelInterrupt.h"
#include "../MIPS/MIPSCodeUtils.h"
#include "../Host.h"

enum
{
	// Do nothing after the syscall.
	HLE_AFTER_NOTHING = 0x00,
	// Reschedule immediately after the syscall.
	HLE_AFTER_RESCHED = 0x01,
	// Call current thread's callbacks after the syscall.
	HLE_AFTER_CURRENT_CALLBACKS = 0x02,
	// Check all threads' callbacks after the syscall.
	HLE_AFTER_ALL_CALLBACKS = 0x04,
	// Reschedule and process current thread's callbacks after the syscall.
	HLE_AFTER_RESCHED_CALLBACKS = 0x08,
	// Run interrupts (and probably reschedule) after the syscall.
	HLE_AFTER_RUN_INTERRUPTS = 0x10,
	// Switch to CORE_STEPPING after the syscall (for debugging.)
	HLE_AFTER_DEBUG_BREAK = 0x20,
};

static std::vector<HLEModule> moduleDB;
static std::vector<Syscall> unresolvedSyscalls;
static int hleAfterSyscall = HLE_AFTER_NOTHING;
static char hleAfterSyscallReschedReason[512];

void HLEInit()
{
	RegisterAllModules();
}

void HLEDoState(PointerWrap &p)
{
	Syscall sc = {0};
	p.Do(unresolvedSyscalls, sc);
	p.DoMarker("HLE");
}

void HLEShutdown()
{
	hleAfterSyscall = HLE_AFTER_NOTHING;
	moduleDB.clear();
	unresolvedSyscalls.clear();
}

void RegisterModule(const char *name, int numFunctions, const HLEFunction *funcTable)
{
	HLEModule module = {name, numFunctions, funcTable};
	moduleDB.push_back(module);
}

int GetModuleIndex(const char *moduleName)
{
	for (size_t i = 0; i < moduleDB.size(); i++)
		if (strcmp(moduleName, moduleDB[i].name) == 0)
			return (int)i;
	return -1;
}

int GetFuncIndex(int moduleIndex, u32 nib)
{
	const HLEModule &module = moduleDB[moduleIndex];
	for (int i = 0; i < module.numFunctions; i++)
	{
		if (module.funcTable[i].ID == nib)
			return i;
	}
	return -1;
}

u32 GetNibByName(const char *moduleName, const char *function)
{
	int moduleIndex = GetModuleIndex(moduleName);
	const HLEModule &module = moduleDB[moduleIndex];
	for (int i = 0; i < module.numFunctions; i++)
	{
		if (!strcmp(module.funcTable[i].name, function))
			return module.funcTable[i].ID;
	}
	return -1;
}

const HLEFunction *GetFunc(const char *moduleName, u32 nib)
{
	int moduleIndex = GetModuleIndex(moduleName);
	if (moduleIndex != -1)
	{
		int idx = GetFuncIndex(moduleIndex, nib);
		if (idx != -1)
			return &(moduleDB[moduleIndex].funcTable[idx]);
	}
	return 0;
}

const char *GetFuncName(const char *moduleName, u32 nib)
{
	const HLEFunction *func = GetFunc(moduleName,nib);
	if (func)
		return func->name;
	else
	{
		static char temp[256];
		sprintf(temp,"[UNK: 0x%08x ]",nib);
		return temp;
	}
}

u32 GetSyscallOp(const char *moduleName, u32 nib)
{
	int modindex = GetModuleIndex(moduleName);
	if (modindex != -1)
	{
		int funcindex = GetFuncIndex(modindex, nib);
		if (funcindex != -1)
		{
			return (0x0000000c | (modindex<<18) | (funcindex<<6));
		}
		else
		{
			return (0x0003FFCC | (modindex<<18));  // invalid syscall
		}
	}
	else
	{
		ERROR_LOG(HLE, "Unknown module %s!", moduleName);
		return (0x0003FFCC);	// invalid syscall
	}
}

void WriteSyscall(const char *moduleName, u32 nib, u32 address)
{
	if (nib == 0)
	{
		Memory::Write_U32(MIPS_MAKE_JR_RA(), address); //patched out?
		Memory::Write_U32(MIPS_MAKE_NOP(), address+4); //patched out?
		return;
	}
	int modindex = GetModuleIndex(moduleName);
	if (modindex != -1)
	{
		Memory::Write_U32(MIPS_MAKE_JR_RA(), address); // jr ra
		Memory::Write_U32(GetSyscallOp(moduleName, nib), address + 4);
	}
	else
	{
		// Module inexistent.. for now; let's store the syscall for it to be resolved later
		INFO_LOG(HLE,"Syscall (%s,%08x) unresolved, storing for later resolving", moduleName, nib);
		Syscall sysc = {"", address, nib};
		strncpy(sysc.moduleName, moduleName, 32);
		sysc.moduleName[31] = '\0';
		unresolvedSyscalls.push_back(sysc);
	}
}

void ResolveSyscall(const char *moduleName, u32 nib, u32 address)
{
	for (size_t i = 0; i < unresolvedSyscalls.size(); i++)
	{
		Syscall *sysc = &unresolvedSyscalls[i];
		if (strncmp(sysc->moduleName, moduleName, 32) == 0 && sysc->nid == nib)
		{
			INFO_LOG(HLE,"Resolving %s/%08x",moduleName,nib);
			// Note: doing that, we can't trace external module calls, so maybe something else should be done to debug more efficiently
			// Note that this should be J not JAL, as otherwise control will return to the stub..
			Memory::Write_U32(MIPS_MAKE_J(address), sysc->symAddr);
			Memory::Write_U32(MIPS_MAKE_NOP(), sysc->symAddr + 4);
		}
	}
}

const char *GetFuncName(int moduleIndex, int func)
{
	if (moduleIndex >= 0 && moduleIndex < (int)moduleDB.size())
	{
		const HLEModule &module = moduleDB[moduleIndex];
		if (func>=0 && func <= module.numFunctions)
		{
			return module.funcTable[func].name;
		}
	}
	return "[unknown]";
}

void hleCheckAllCallbacks()
{
	hleAfterSyscall |= HLE_AFTER_ALL_CALLBACKS;
}

void hleCheckCurrentCallbacks()
{
	hleAfterSyscall |= HLE_AFTER_CURRENT_CALLBACKS;
}

void hleReSchedule(const char *reason)
{
	_dbg_assert_msg_(HLE, reason != 0, "hleReSchedule: Expecting a valid reason.");
	_dbg_assert_msg_(HLE, reason != 0 && strlen(reason) < 256, "hleReSchedule: Not too long reason.");

	hleAfterSyscall |= HLE_AFTER_RESCHED;

	if (!reason)
		strcpy(hleAfterSyscallReschedReason, "Invalid reason");
	// You can't seriously need a reason that long, can you?
	else if (strlen(reason) >= sizeof(hleAfterSyscallReschedReason))
	{
		memcpy(hleAfterSyscallReschedReason, reason, sizeof(hleAfterSyscallReschedReason) - 1);
		hleAfterSyscallReschedReason[sizeof(hleAfterSyscallReschedReason) - 1] = 0;
	}
	else
		strcpy(hleAfterSyscallReschedReason, reason);
}

void hleReSchedule(bool callbacks, const char *reason)
{
	hleReSchedule(reason);
	if (callbacks)
		hleAfterSyscall |= HLE_AFTER_RESCHED_CALLBACKS;
}

void hleRunInterrupts()
{
	hleAfterSyscall |= HLE_AFTER_RUN_INTERRUPTS;
}

void hleDebugBreak()
{
	hleAfterSyscall |= HLE_AFTER_DEBUG_BREAK;
}

// Pauses execution after an HLE call.
bool hleExecuteDebugBreak(const HLEFunction &func)
{
	const u32 NID_SUSPEND_INTR = 0x092968F4, NID_RESUME_INTR = 0x5F10D406;

	// Never break on these, they're noise.
	u32 blacklistedNIDs[] = {NID_SUSPEND_INTR, NID_RESUME_INTR, NID_IDLE};
	for (int i = 0; i < ARRAY_SIZE(blacklistedNIDs); ++i)
	{
		if (func.ID == blacklistedNIDs[i])
			return false;
	}

	Core_EnableStepping(true);
	host->SetDebugMode(true);
	return true;
}

inline void hleFinishSyscall(int modulenum, int funcnum)
{
	if ((hleAfterSyscall & HLE_AFTER_CURRENT_CALLBACKS) != 0)
		__KernelForceCallbacks();

	if ((hleAfterSyscall & HLE_AFTER_RUN_INTERRUPTS) != 0)
		__RunOnePendingInterrupt();

	// Rescheduling will also do HLE_AFTER_ALL_CALLBACKS.
	if ((hleAfterSyscall & HLE_AFTER_RESCHED_CALLBACKS) != 0)
		__KernelReSchedule(true, hleAfterSyscallReschedReason);
	else if ((hleAfterSyscall & HLE_AFTER_RESCHED) != 0)
		__KernelReSchedule(hleAfterSyscallReschedReason);
	else if ((hleAfterSyscall & HLE_AFTER_ALL_CALLBACKS) != 0)
		__KernelCheckCallbacks();

	if ((hleAfterSyscall & HLE_AFTER_DEBUG_BREAK) != 0)
	{
		if (!hleExecuteDebugBreak(moduleDB[modulenum].funcTable[funcnum]))
		{
			// We'll do it next syscall.
			hleAfterSyscall = HLE_AFTER_DEBUG_BREAK;
			hleAfterSyscallReschedReason[0] = 0;
			return;
		}
	}

	hleAfterSyscall = HLE_AFTER_NOTHING;
	hleAfterSyscallReschedReason[0] = 0;
}

void CallSyscall(u32 op)
{
	u32 callno = (op >> 6) & 0xFFFFF; //20 bits
	int funcnum = callno & 0xFFF;
	int modulenum = (callno & 0xFF000) >> 12;
	if (funcnum == 0xfff || op == 0xffff)
	{
		_dbg_assert_msg_(HLE,0,"Unknown syscall");
		ERROR_LOG(HLE,"Unknown syscall: Module: %s", moduleDB[modulenum].name); 
		return;
	}
	HLEFunc func = moduleDB[modulenum].funcTable[funcnum].func;
	if (func)
	{
		func();

		if (hleAfterSyscall != HLE_AFTER_NOTHING)
			hleFinishSyscall(modulenum, funcnum);
	}
	else
	{
		ERROR_LOG(HLE,"Unimplemented HLE function %s", moduleDB[modulenum].funcTable[funcnum].name);
	}
}
