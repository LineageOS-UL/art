/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

namespace art {

/*
 * This source files contains "gen" codegen routines that should
 * be applicable to most targets.  Only mid-level support utilities
 * and "op" calls may be used here.
 */


typedef int (*NextCallInsn)(CompilationUnit*, MIR*, int, uint32_t dexIdx,
                            uint32_t methodIdx, uintptr_t directCode,
                            uintptr_t directMethod);
/*
 * If there are any ins passed in registers that have not been promoted
 * to a callee-save register, flush them to the frame.  Perform intial
 * assignment of promoted arguments.
 */
void flushIns(CompilationUnit* cUnit)
{
    if (cUnit->numIns == 0)
        return;
    int firstArgReg = rARG1;
#if !defined(TARGET_X86)
    int lastArgReg = rARG3;
#else
    int lastArgReg = rARG2;
#endif
    int startVReg = cUnit->numDalvikRegisters - cUnit->numIns;
    /*
     * Copy incoming arguments to their proper home locations.
     * NOTE: an older version of dx had an issue in which
     * it would reuse static method argument registers.
     * This could result in the same Dalvik virtual register
     * being promoted to both core and fp regs. To account for this,
     * we only copy to the corresponding promoted physical register
     * if it matches the type of the SSA name for the incoming
     * argument.  It is also possible that long and double arguments
     * end up half-promoted.  In those cases, we must flush the promoted
     * half to memory as well.
     */
    for (int i = 0; i < cUnit->numIns; i++) {
        PromotionMap* vMap = &cUnit->promotionMap[startVReg + i];
        if (i <= (lastArgReg - firstArgReg)) {
            // If arriving in register
            bool needFlush = true;
            RegLocation* tLoc = &cUnit->regLocation[startVReg + i];
            if ((vMap->coreLocation == kLocPhysReg) && !tLoc->fp) {
                opRegCopy(cUnit, vMap->coreReg, firstArgReg + i);
                needFlush = false;
            } else if ((vMap->fpLocation == kLocPhysReg) && tLoc->fp) {
                opRegCopy(cUnit, vMap->fpReg, firstArgReg + i);
                needFlush = false;
            } else {
                needFlush = true;
            }

            // For wide args, force flush if only half is promoted
            if (tLoc->wide) {
                PromotionMap* pMap = vMap + (tLoc->highWord ? -1 : +1);
                needFlush |= (pMap->coreLocation != vMap->coreLocation) ||
                             (pMap->fpLocation != vMap->fpLocation);
            }
            if (needFlush) {
                storeBaseDisp(cUnit, rSP, oatSRegOffset(cUnit, startVReg + i),
                              firstArgReg + i, kWord);
            }
        } else {
            // If arriving in frame & promoted
            if (vMap->coreLocation == kLocPhysReg) {
                loadWordDisp(cUnit, rSP, oatSRegOffset(cUnit, startVReg + i),
                             vMap->coreReg);
            }
            if (vMap->fpLocation == kLocPhysReg) {
                loadWordDisp(cUnit, rSP, oatSRegOffset(cUnit, startVReg + i),
                             vMap->fpReg);
            }
        }
    }
}

void scanMethodLiteralPool(CompilationUnit* cUnit, LIR** methodTarget, LIR** codeTarget, const DexFile* dexFile, uint32_t dexMethodIdx)
{
    LIR* curTarget = cUnit->methodLiteralList;
    LIR* nextTarget = curTarget != NULL ? curTarget->next : NULL;
    while (curTarget != NULL && nextTarget != NULL) {
      if (curTarget->operands[0] == (int)dexFile &&
          nextTarget->operands[0] == (int)dexMethodIdx) {
        *codeTarget = curTarget;
        *methodTarget = nextTarget;
        DCHECK((*codeTarget)->next == *methodTarget);
        DCHECK_EQ((*codeTarget)->operands[0], (int)dexFile);
        DCHECK_EQ((*methodTarget)->operands[0], (int)dexMethodIdx);
        break;
      }
      curTarget = nextTarget->next;
      nextTarget = curTarget != NULL ? curTarget->next : NULL;
    }
}

/*
 * Bit of a hack here - in leiu of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
int nextSDCallInsn(CompilationUnit* cUnit, MIR* mir,
                   int state, uint32_t dexIdx, uint32_t unused,
                   uintptr_t directCode, uintptr_t directMethod)
{
    if (directCode != 0 && directMethod != 0) {
        switch(state) {
        case 0:  // Get the current Method* [sets rARG0]
            if (directCode != (uintptr_t)-1) {
                loadConstant(cUnit, rINVOKE_TGT, directCode);
            } else {
                LIR* dataTarget = scanLiteralPool(cUnit->codeLiteralList, dexIdx, 0);
                if (dataTarget == NULL) {
                    dataTarget = addWordData(cUnit, &cUnit->codeLiteralList, dexIdx);
                }
#if defined(TARGET_ARM)
                LIR* loadPcRel = rawLIR(cUnit, cUnit->currentDalvikOffset,
                                        kThumb2LdrPcRel12, rINVOKE_TGT, 0, 0, 0, 0, dataTarget);
                oatAppendLIR(cUnit, loadPcRel);
#else
                UNIMPLEMENTED(FATAL) << (void*)dataTarget;
#endif
            }
            if (directMethod != (uintptr_t)-1) {
                loadConstant(cUnit, rARG0, directMethod);
            } else {
                LIR* dataTarget = scanLiteralPool(cUnit->methodLiteralList, dexIdx, 0);
                if (dataTarget == NULL) {
                    dataTarget = addWordData(cUnit, &cUnit->methodLiteralList, dexIdx);
                }
#if defined(TARGET_ARM)
                LIR* loadPcRel = rawLIR(cUnit, cUnit->currentDalvikOffset,
                                        kThumb2LdrPcRel12, rARG0, 0, 0, 0, 0, dataTarget);
                oatAppendLIR(cUnit, loadPcRel);
#else
                UNIMPLEMENTED(FATAL) << (void*)dataTarget;
#endif
            }
            break;
        default:
            return -1;
      }
    } else {
        switch(state) {
        case 0:  // Get the current Method* [sets rARG0]
            loadCurrMethodDirect(cUnit, rARG0);
            break;
        case 1:  // Get method->dex_cache_resolved_methods_
            loadWordDisp(cUnit, rARG0,
                Method::DexCacheResolvedMethodsOffset().Int32Value(),
                rARG0);
            // Set up direct code if known.
            if (directCode != 0) {
                if (directCode != (uintptr_t)-1) {
                    loadConstant(cUnit, rINVOKE_TGT, directCode);
                } else {
                    LIR* dataTarget = scanLiteralPool(cUnit->codeLiteralList, dexIdx, 0);
                    if (dataTarget == NULL) {
                        dataTarget = addWordData(cUnit, &cUnit->codeLiteralList, dexIdx);
                    }
#if defined(TARGET_ARM)
                    LIR* loadPcRel = rawLIR(cUnit, cUnit->currentDalvikOffset,
                                            kThumb2LdrPcRel12, rINVOKE_TGT, 0, 0, 0, 0, dataTarget);
                    oatAppendLIR(cUnit, loadPcRel);
#else
                    UNIMPLEMENTED(FATAL) << (void*)dataTarget;
#endif
                }
            }
            break;
        case 2:  // Grab target method*
            loadWordDisp(cUnit, rARG0,
                Array::DataOffset(sizeof(Object*)).Int32Value() + dexIdx * 4,
                rARG0);
            break;
#if !defined(TARGET_X86)
        case 3:  // Grab the code from the method*
            if (directCode == 0) {
                loadWordDisp(cUnit, rARG0, Method::GetCodeOffset().Int32Value(),
                             rINVOKE_TGT);
            }
            break;
#endif
        default:
            return -1;
        }
    }
    return state + 1;
}

/*
 * Bit of a hack here - in leiu of a real scheduling pass,
 * emit the next instruction in a virtual invoke sequence.
 * We can use rLR as a temp prior to target address loading
 * Note also that we'll load the first argument ("this") into
 * rARG1 here rather than the standard loadArgRegs.
 */
int nextVCallInsn(CompilationUnit* cUnit, MIR* mir,
                  int state, uint32_t dexIdx, uint32_t methodIdx,
                  uintptr_t unused, uintptr_t unused2)
{
    RegLocation rlArg;
    /*
     * This is the fast path in which the target virtual method is
     * fully resolved at compile time.
     */
    switch(state) {
        case 0:  // Get "this" [set rARG1]
            rlArg = oatGetSrc(cUnit, mir, 0);
            loadValueDirectFixed(cUnit, rlArg, rARG1);
            break;
        case 1: // Is "this" null? [use rARG1]
            genNullCheck(cUnit, oatSSASrc(mir,0), rARG1, mir);
            // get this->klass_ [use rARG1, set rINVOKE_TGT]
            loadWordDisp(cUnit, rARG1, Object::ClassOffset().Int32Value(),
                         rINVOKE_TGT);
            break;
        case 2: // Get this->klass_->vtable [usr rINVOKE_TGT, set rINVOKE_TGT]
            loadWordDisp(cUnit, rINVOKE_TGT, Class::VTableOffset().Int32Value(),
                         rINVOKE_TGT);
            break;
        case 3: // Get target method [use rINVOKE_TGT, set rARG0]
            loadWordDisp(cUnit, rINVOKE_TGT, (methodIdx * 4) +
                         Array::DataOffset(sizeof(Object*)).Int32Value(),
                         rARG0);
            break;
#if !defined(TARGET_X86)
        case 4: // Get the compiled code address [uses rARG0, sets rINVOKE_TGT]
            loadWordDisp(cUnit, rARG0, Method::GetCodeOffset().Int32Value(),
                         rINVOKE_TGT);
            break;
#endif
        default:
            return -1;
    }
    return state + 1;
}

int nextInvokeInsnSP(CompilationUnit* cUnit, MIR* mir, int trampoline,
                     int state, uint32_t dexIdx, uint32_t methodIdx)
{
    /*
     * This handles the case in which the base method is not fully
     * resolved at compile time, we bail to a runtime helper.
     */
#if !defined(TARGET_X86)
    if (state == 0) {
        // Load trampoline target
        loadWordDisp(cUnit, rSELF, trampoline, rINVOKE_TGT);
        // Load rARG0 with method index
        loadConstant(cUnit, rARG0, dexIdx);
        return 1;
    }
#endif
    return -1;
}

int nextStaticCallInsnSP(CompilationUnit* cUnit, MIR* mir,
                         int state, uint32_t dexIdx, uint32_t methodIdx,
                         uintptr_t unused, uintptr_t unused2)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeStaticTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

int nextDirectCallInsnSP(CompilationUnit* cUnit, MIR* mir, int state,
                         uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                         uintptr_t unused2)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeDirectTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

int nextSuperCallInsnSP(CompilationUnit* cUnit, MIR* mir, int state,
                        uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                        uintptr_t unused2)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeSuperTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

int nextVCallInsnSP(CompilationUnit* cUnit, MIR* mir, int state,
                    uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                    uintptr_t unused2)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeVirtualTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

/*
 * All invoke-interface calls bounce off of art_invoke_interface_trampoline,
 * which will locate the target and continue on via a tail call.
 */
int nextInterfaceCallInsn(CompilationUnit* cUnit, MIR* mir, int state,
                          uint32_t dexIdx, uint32_t unused, uintptr_t unused2,
                          uintptr_t unused3)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeInterfaceTrampoline);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

int nextInterfaceCallInsnWithAccessCheck(CompilationUnit* cUnit, MIR* mir,
                                         int state, uint32_t dexIdx,
                                         uint32_t unused, uintptr_t unused2,
                                         uintptr_t unused3)
{
  int trampoline = OFFSETOF_MEMBER(Thread, pInvokeInterfaceTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, mir, trampoline, state, dexIdx, 0);
}

int loadArgRegs(CompilationUnit* cUnit, MIR* mir, DecodedInstruction* dInsn,
                int callState, NextCallInsn nextCallInsn, uint32_t dexIdx,
                uint32_t methodIdx, uintptr_t directCode,
                uintptr_t directMethod, bool skipThis)
{
#if !defined(TARGET_X86)
    int lastArgReg = rARG3;
#else
    int lastArgReg = rARG2;
#endif
    int nextReg = rARG1;
    int nextArg = 0;
    if (skipThis) {
        nextReg++;
        nextArg++;
    }
    for (; (nextReg <= lastArgReg) && (nextArg < mir->ssaRep->numUses); nextReg++) {
        RegLocation rlArg = oatGetRawSrc(cUnit, mir, nextArg++);
        rlArg = oatUpdateRawLoc(cUnit, rlArg);
        if (rlArg.wide && (nextReg <= rARG2)) {
            loadValueDirectWideFixed(cUnit, rlArg, nextReg, nextReg + 1);
            nextReg++;
            nextArg++;
        } else {
            rlArg.wide = false;
            loadValueDirectFixed(cUnit, rlArg, nextReg);
        }
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                 directCode, directMethod);
    }
    return callState;
}

/*
 * Load up to 5 arguments, the first three of which will be in
 * rARG1 .. rARG3.  On entry rARG0 contains the current method pointer,
 * and as part of the load sequence, it must be replaced with
 * the target method pointer.  Note, this may also be called
 * for "range" variants if the number of arguments is 5 or fewer.
 */
int genDalvikArgsNoRange(CompilationUnit* cUnit, MIR* mir,
                         DecodedInstruction* dInsn, int callState,
                         LIR** pcrLabel, NextCallInsn nextCallInsn,
                         uint32_t dexIdx, uint32_t methodIdx,
                         uintptr_t directCode, uintptr_t directMethod,
                         bool skipThis)
{
    RegLocation rlArg;

    /* If no arguments, just return */
    if (dInsn->vA == 0)
        return callState;

    callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                             directCode, directMethod);

    DCHECK_LE(dInsn->vA, 5U);
    if (dInsn->vA > 3) {
        uint32_t nextUse = 3;
        //Detect special case of wide arg spanning arg3/arg4
        RegLocation rlUse0 = oatGetRawSrc(cUnit, mir, 0);
        RegLocation rlUse1 = oatGetRawSrc(cUnit, mir, 1);
        RegLocation rlUse2 = oatGetRawSrc(cUnit, mir, 2);
        if (((!rlUse0.wide && !rlUse1.wide) || rlUse0.wide) &&
            rlUse2.wide) {
            int reg = -1;
            // Wide spans, we need the 2nd half of uses[2].
            rlArg = oatUpdateLocWide(cUnit, rlUse2);
            if (rlArg.location == kLocPhysReg) {
                reg = rlArg.highReg;
            } else {
                // rARG2 & rARG3 can safely be used here
#if defined(TARGET_X86)
                UNIMPLEMENTED(FATAL);
#else
                reg = rARG3;
#endif
                loadWordDisp(cUnit, rSP,
                             oatSRegOffset(cUnit, rlArg.sRegLow) + 4, reg);
                callState = nextCallInsn(cUnit, mir, callState, dexIdx,
                                         methodIdx, directCode, directMethod);
            }
            storeBaseDisp(cUnit, rSP, (nextUse + 1) * 4, reg, kWord);
            storeBaseDisp(cUnit, rSP, 16 /* (3+1)*4 */, reg, kWord);
            callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                     directCode, directMethod);
            nextUse++;
        }
        // Loop through the rest
        while (nextUse < dInsn->vA) {
            int lowReg;
            int highReg = -1;
            rlArg = oatGetRawSrc(cUnit, mir, nextUse);
            rlArg = oatUpdateRawLoc(cUnit, rlArg);
            if (rlArg.location == kLocPhysReg) {
                lowReg = rlArg.lowReg;
                highReg = rlArg.highReg;
            } else {
                lowReg = rARG2;
#if defined(TARGET_X86)
                UNIMPLEMENTED(FATAL);
#else
                highReg = rARG3;
#endif
                if (rlArg.wide) {
                    loadValueDirectWideFixed(cUnit, rlArg, lowReg, highReg);
                } else {
                    loadValueDirectFixed(cUnit, rlArg, lowReg);
                }
                callState = nextCallInsn(cUnit, mir, callState, dexIdx,
                                         methodIdx, directCode, directMethod);
            }
            int outsOffset = (nextUse + 1) * 4;
            if (rlArg.wide) {
                storeBaseDispWide(cUnit, rSP, outsOffset, lowReg, highReg);
                nextUse += 2;
            } else {
                storeWordDisp(cUnit, rSP, outsOffset, lowReg);
                nextUse++;
            }
            callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                     directCode, directMethod);
        }
    }

    callState = loadArgRegs(cUnit, mir, dInsn, callState, nextCallInsn,
                            dexIdx, methodIdx, directCode, directMethod,
                            skipThis);

    if (pcrLabel) {
        *pcrLabel = genNullCheck(cUnit, oatSSASrc(mir,0), rARG1, mir);
    }
    return callState;
}

/*
 * May have 0+ arguments (also used for jumbo).  Note that
 * source virtual registers may be in physical registers, so may
 * need to be flushed to home location before copying.  This
 * applies to arg3 and above (see below).
 *
 * Two general strategies:
 *    If < 20 arguments
 *       Pass args 3-18 using vldm/vstm block copy
 *       Pass arg0, arg1 & arg2 in rARG1-rARG3
 *    If 20+ arguments
 *       Pass args arg19+ using memcpy block copy
 *       Pass arg0, arg1 & arg2 in rARG1-rARG3
 *
 */
int genDalvikArgsRange(CompilationUnit* cUnit, MIR* mir,
                       DecodedInstruction* dInsn, int callState,
                       LIR** pcrLabel, NextCallInsn nextCallInsn,
                       uint32_t dexIdx, uint32_t methodIdx,
                       uintptr_t directCode, uintptr_t directMethod,
                       bool skipThis)
{
    int firstArg = dInsn->vC;
    int numArgs = dInsn->vA;

    // If we can treat it as non-range (Jumbo ops will use range form)
    if (numArgs <= 5)
        return genDalvikArgsNoRange(cUnit, mir, dInsn, callState, pcrLabel,
                                    nextCallInsn, dexIdx, methodIdx,
                                    directCode, directMethod, skipThis);
    /*
     * Make sure range list doesn't span the break between in normal
     * Dalvik vRegs and the ins.
     */
    int highestArg = oatGetSrc(cUnit, mir, numArgs-1).sRegLow;
    int boundaryReg = cUnit->numDalvikRegisters - cUnit->numIns;
    if ((firstArg < boundaryReg) && (highestArg >= boundaryReg)) {
        LOG(FATAL) << "Argument list spanned locals & args";
    }

    /*
     * First load the non-register arguments.  Both forms expect all
     * of the source arguments to be in their home frame location, so
     * scan the sReg names and flush any that have been promoted to
     * frame backing storage.
     */
    // Scan the rest of the args - if in physReg flush to memory
    for (int nextArg = 0; nextArg < numArgs;) {
        RegLocation loc = oatGetRawSrc(cUnit, mir, nextArg);
        if (loc.wide) {
            loc = oatUpdateLocWide(cUnit, loc);
            if ((nextArg >= 2) && (loc.location == kLocPhysReg)) {
                storeBaseDispWide(cUnit, rSP,
                                  oatSRegOffset(cUnit, loc.sRegLow),
                                  loc.lowReg, loc.highReg);
            }
            nextArg += 2;
        } else {
            loc = oatUpdateLoc(cUnit, loc);
            if ((nextArg >= 3) && (loc.location == kLocPhysReg)) {
                storeBaseDisp(cUnit, rSP, oatSRegOffset(cUnit, loc.sRegLow),
                              loc.lowReg, kWord);
            }
            nextArg++;
        }
    }

    int startOffset = oatSRegOffset(cUnit,
        cUnit->regLocation[mir->ssaRep->uses[3]].sRegLow);
    int outsOffset = 4 /* Method* */ + (3 * 4);
#if defined(TARGET_MIPS) || defined(TARGET_X86)
    // Generate memcpy
    opRegRegImm(cUnit, kOpAdd, rARG0, rSP, outsOffset);
    opRegRegImm(cUnit, kOpAdd, rARG1, rSP, startOffset);
    callRuntimeHelperRegRegImm(cUnit, OFFSETOF_MEMBER(Thread, pMemcpy),
                               rARG0, rARG1, (numArgs - 3) * 4);
    // Restore Method*
    loadCurrMethodDirect(cUnit, rARG0);
#else
    if (numArgs >= 20) {
        // Generate memcpy
        opRegRegImm(cUnit, kOpAdd, rARG0, rSP, outsOffset);
        opRegRegImm(cUnit, kOpAdd, rARG1, rSP, startOffset);
        callRuntimeHelperRegRegImm(cUnit, OFFSETOF_MEMBER(Thread, pMemcpy),
                                   rARG0, rARG1, (numArgs - 3) * 4);
        // Restore Method*
        loadCurrMethodDirect(cUnit, rARG0);
    } else {
        // Use vldm/vstm pair using rARG3 as a temp
        int regsLeft = std::min(numArgs - 3, 16);
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                 directCode, directMethod);
        opRegRegImm(cUnit, kOpAdd, rARG3, rSP, startOffset);
        LIR* ld = newLIR3(cUnit, kThumb2Vldms, rARG3, fr0, regsLeft);
        //TUNING: loosen barrier
        ld->defMask = ENCODE_ALL;
        setMemRefType(ld, true /* isLoad */, kDalvikReg);
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                 directCode, directMethod);
        opRegRegImm(cUnit, kOpAdd, rARG3, rSP, 4 /* Method* */ + (3 * 4));
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                 directCode, directMethod);
        LIR* st = newLIR3(cUnit, kThumb2Vstms, rARG3, fr0, regsLeft);
        setMemRefType(st, false /* isLoad */, kDalvikReg);
        st->defMask = ENCODE_ALL;
        callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                                 directCode, directMethod);

    }
#endif

    callState = loadArgRegs(cUnit, mir, dInsn, callState, nextCallInsn,
                            dexIdx, methodIdx, directCode, directMethod,
                            skipThis);

    callState = nextCallInsn(cUnit, mir, callState, dexIdx, methodIdx,
                             directCode, directMethod);
    if (pcrLabel) {
        *pcrLabel = genNullCheck(cUnit, oatSSASrc(mir,0), rARG1, mir);
    }
    return callState;
}

}  // namespace art
