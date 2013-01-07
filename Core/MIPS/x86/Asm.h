// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#ifndef _JIT64ASM_H
#define _JIT64ASM_H

#include "x64Emitter.h"
#include "../MIPS.h"
// In Dolphin, we don't use inline assembly. Instead, we generate all machine-near
// code at runtime. In the case of fixed code like this, after writing it, we write
// protect the memory, essentially making it work just like precompiled code.

// There are some advantages to this approach:
//	 1) No need to setup an external assembler in the build.
//	 2) Cross platform, as long as it's x86/x64.
//	 3) Can optimize code at runtime for the specific CPU model.
// There aren't really any disadvantages other than having to maintain a x86 emitter,
// which we have to do anyway :)
// 
// To add a new asm routine, just add another const here, and add the code to Generate.
// Also, possibly increase the size of the code buffer.

namespace MIPSComp
{
	class Jit;
}

class AsmRoutineManager : public Gen::XCodeBlock
{
private:
	void Generate(MIPSState *mips, MIPSComp::Jit *jit);
	void GenerateCommon();

public:
	AsmRoutineManager()
	{
	}

	void Init(MIPSState *mips, MIPSComp::Jit *jit)
	{
		AllocCodeSpace(8192);
		Generate(mips, jit);
		WriteProtect();
	}
	~AsmRoutineManager()
	{
		FreeCodeSpace();
	}

	const u8 *enterCode;

	const u8 *outerLoop;
	const u8 *dispatcher;
	const u8 *dispatcherCheckCoreState;
	const u8 *dispatcherNoCheck;

	const u8 *fpException;

	const u8 *breakpointBailout;
};

#endif	// _JIT64ASM_H
