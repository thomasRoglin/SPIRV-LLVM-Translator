//===- SPIRVOpCode.h - Class to represent SPIR-V Operation Codes -*- C++ -*-==//
//
//                     The LLVM/SPIRV Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file defines Operation Code class for SPIR-V.
///
//===----------------------------------------------------------------------===//

#ifndef SPIRV_LIBSPIRV_SPIRVOPCODE_H
#define SPIRV_LIBSPIRV_SPIRVOPCODE_H

#include "SPIRVUtil.h"
#include "spirv/unified1/spirv.hpp"
#include "spirv_internal.hpp"
#include <string>

using namespace spv;
namespace SPIRV {

template <> inline void SPIRVMap<Op, std::string>::init() {
#define _SPIRV_OP(x, ...) add(Op##x, #x);
#define _SPIRV_OP_INTERNAL(x, ...) add(internal::Op##x, #x);
#include "SPIRVOpCodeEnum.h"
#include "SPIRVOpCodeEnumInternal.h"
#undef _SPIRV_OP_INTERNAL
#undef _SPIRV_OP
}
SPIRV_DEF_NAMEMAP(Op, OpCodeNameMap)

inline bool isFPAtomicOpCode(Op OpCode) {
  return OpCode == OpAtomicFAddEXT || OpCode == OpAtomicFMinEXT ||
         OpCode == OpAtomicFMaxEXT;
}
inline bool isAtomicOpCode(Op OpCode) {
  static_assert(OpAtomicLoad < OpAtomicXor, "");
  return ((unsigned)OpCode >= OpAtomicLoad &&
          (unsigned)OpCode <= OpAtomicXor) ||
         OpCode == OpAtomicFlagTestAndSet || OpCode == OpAtomicFlagClear ||
         isFPAtomicOpCode(OpCode);
}
inline bool isAtomicOpCodeUntypedPtrSupported(Op OpCode) {
  static_assert(OpAtomicLoad < OpAtomicXor, "");
  return ((unsigned)OpCode >= OpAtomicLoad &&
          (unsigned)OpCode <= OpAtomicXor) ||
         isFPAtomicOpCode(OpCode);
}

inline bool isBinaryOpCode(Op OpCode) {
  return ((unsigned)OpCode >= OpIAdd && (unsigned)OpCode <= OpFMod) ||
         OpCode == OpDot || OpCode == OpIAddCarry || OpCode == OpISubBorrow ||
         OpCode == OpUMulExtended || OpCode == OpSMulExtended;
}

inline bool isBinaryPtrOpCode(Op OpCode) {
  return (unsigned)OpCode >= OpPtrEqual && (unsigned)OpCode <= OpPtrDiff;
}

inline bool isShiftOpCode(Op OpCode) {
  return (unsigned)OpCode >= OpShiftRightLogical &&
         (unsigned)OpCode <= OpShiftLeftLogical;
}

inline bool isLogicalOpCode(Op OpCode) {
  return (unsigned)OpCode >= OpLogicalEqual && (unsigned)OpCode <= OpLogicalNot;
}

inline bool isUnaryPredicateOpCode(Op OpCode) {
  return (unsigned)OpCode >= OpAny && (unsigned)OpCode <= OpSignBitSet;
}

inline bool isBitwiseOpCode(Op OpCode) {
  return (unsigned)OpCode >= OpBitwiseOr && (unsigned)OpCode <= OpBitwiseAnd;
}

inline bool isBinaryShiftLogicalBitwiseOpCode(Op OpCode) {
  return (((unsigned)OpCode >= OpShiftRightLogical &&
           (unsigned)OpCode <= OpBitwiseAnd) ||
          isBinaryOpCode(OpCode));
}

inline bool isCmpOpCode(Op OpCode) {
  return ((unsigned)OpCode >= OpIEqual &&
          (unsigned)OpCode <= OpFUnordGreaterThanEqual) ||
         (OpCode >= OpLessOrGreater && OpCode <= OpLogicalNotEqual);
}

inline bool isCvtOpCode(Op OpCode) {
  return ((unsigned)OpCode >= OpConvertFToU && (unsigned)OpCode <= OpBitcast) ||
         OpCode == OpSatConvertSToU || OpCode == OpSatConvertUToS ||
         OpCode == OpPtrCastToCrossWorkgroupINTEL ||
         OpCode == OpCrossWorkgroupCastToPtrINTEL;
}

inline bool isCvtToUnsignedOpCode(Op OpCode) {
  return OpCode == OpConvertFToU || OpCode == OpUConvert ||
         OpCode == OpSatConvertSToU;
}

inline bool isCvtFromUnsignedOpCode(Op OpCode) {
  return OpCode == OpConvertUToF || OpCode == OpUConvert ||
         OpCode == OpSatConvertUToS;
}

inline bool isSatCvtOpCode(Op OpCode) {
  return OpCode == OpSatConvertUToS || OpCode == OpSatConvertSToU;
}

inline bool isOpaqueGenericTypeOpCode(Op OpCode) {
  return ((unsigned)OpCode >= OpTypeEvent && (unsigned)OpCode <= OpTypeQueue) ||
         OpCode == OpTypeSampler;
}

inline bool isGenericNegateOpCode(Op OpCode) {
  return (unsigned)OpCode == OpSNegate || (unsigned)OpCode == OpFNegate ||
         (unsigned)OpCode == OpNot;
}

inline bool isAccessChainOpCode(Op OpCode) {
  return OpCode == OpAccessChain || OpCode == OpInBoundsAccessChain;
}

inline bool isUntypedAccessChainOpCode(Op OpCode) {
  return OpCode == OpUntypedAccessChainKHR ||
         OpCode == OpUntypedInBoundsAccessChainKHR ||
         OpCode == OpUntypedPtrAccessChainKHR ||
         OpCode == OpUntypedInBoundsPtrAccessChainKHR;
}

inline bool hasExecScope(Op OpCode) {
  unsigned OC = OpCode;
  return (OpGroupWaitEvents <= OC && OC <= OpGroupSMax) ||
         (OpGroupReserveReadPipePackets <= OC &&
          OC <= OpGroupCommitWritePipe) ||
         (OC == OpGroupNonUniformRotateKHR);
}

inline bool hasGroupOperation(Op OpCode) {
  unsigned OC = OpCode;
  return (OpGroupIAdd <= OC && OC <= OpGroupSMax) ||
         (OpGroupNonUniformBallotBitCount == OC) ||
         (OpGroupNonUniformIAdd <= OC && OC <= OpGroupNonUniformLogicalXor) ||
         (OpGroupIMulKHR <= OC && OC <= OpGroupLogicalXorKHR);
}

inline bool isUniformArithmeticOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return (OpGroupIAdd <= OC && OC <= OpGroupSMax) ||
         (OpGroupIMulKHR <= OC && OC <= OpGroupLogicalXorKHR);
}

inline bool isNonUniformArithmeticOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return (OpGroupNonUniformIAdd <= OC && OC <= OpGroupNonUniformLogicalXor);
}

inline bool isGroupLogicalOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return OC == OpGroupNonUniformLogicalAnd ||
         OC == OpGroupNonUniformLogicalOr ||
         OC == OpGroupNonUniformLogicalXor || OC == OpGroupLogicalAndKHR ||
         OC == OpGroupLogicalOrKHR || OC == OpGroupLogicalXorKHR;
}

inline bool isGroupOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return (OpGroupAll <= OC && OC <= OpGroupSMax) ||
         (OpGroupIMulKHR <= OC && OC <= OpGroupLogicalXorKHR);
}

inline bool isGroupNonUniformOpcode(Op OpCode) {
  unsigned OC = OpCode;
  return (OpGroupNonUniformElect <= OC && OC <= OpGroupNonUniformQuadSwap) ||
         (OC == OpGroupNonUniformRotateKHR);
}

inline bool isMediaBlockINTELOpcode(Op OpCode) {
  return OpCode == OpSubgroupImageMediaBlockReadINTEL ||
         OpCode == OpSubgroupImageMediaBlockWriteINTEL;
}

inline bool isPipeOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return (OpReadPipe <= OC && OC <= OpGroupCommitWritePipe) ||
         (OpReadPipeBlockingINTEL <= OC && OC <= OpWritePipeBlockingINTEL);
}

inline bool isSubgroupAvcINTELTypeOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return OpTypeAvcImePayloadINTEL <= OC && OC <= OpTypeAvcSicResultINTEL;
}

inline bool isSubgroupAvcINTELInstructionOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return OpSubgroupAvcMceGetDefaultInterBaseMultiReferencePenaltyINTEL <= OC &&
         OC <= OpSubgroupAvcSicGetInterRawSadsINTEL;
}

inline bool isSubgroupAvcINTELEvaluateOpcode(Op OpCode) {
  unsigned OC = OpCode;
  return (OpSubgroupAvcImeEvaluateWithSingleReferenceINTEL <= OC &&
          OC <= OpSubgroupAvcImeEvaluateWithDualReferenceStreaminoutINTEL) ||
         (OpSubgroupAvcRefEvaluateWithSingleReferenceINTEL <= OC &&
          OC <= OpSubgroupAvcRefEvaluateWithMultiReferenceInterlacedINTEL) ||
         (OpSubgroupAvcSicEvaluateIpeINTEL <= OC &&
          OC <= OpSubgroupAvcSicEvaluateWithMultiReferenceInterlacedINTEL);
}

inline bool isVCOpCode(Op OpCode) { return OpCode == OpTypeBufferSurfaceINTEL; }

inline bool isTypeOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return (OpTypeVoid <= OC && OC <= OpTypePipe) || OC == OpTypePipeStorage ||
         isSubgroupAvcINTELTypeOpCode(OpCode) || OC == OpTypeVmeImageINTEL ||
         isVCOpCode(OpCode) || OC == internal::OpTypeTokenINTEL ||
         OC == internal::OpTypeJointMatrixINTEL ||
         OC == internal::OpTypeJointMatrixINTELv2 ||
         OC == OpTypeCooperativeMatrixKHR ||
         OC == internal::OpTypeTaskSequenceINTEL ||
         OC == OpTypeUntypedPointerKHR;
}

inline bool isFnVarSpecConstINTEL(Op OpCode) {
  unsigned OC = OpCode;
  return OC == OpSpecConstantArchitectureINTEL ||
         OC == OpSpecConstantTargetINTEL ||
         OC == OpSpecConstantCapabilitiesINTEL;
}

inline bool isSpecConstantOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return (OpSpecConstantTrue <= OC && OC <= OpSpecConstantOp) ||
         isFnVarSpecConstINTEL(OpCode);
}

inline bool isConstantOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return (OpConstantTrue <= OC && OC <= OpSpecConstantOp) || OC == OpUndef ||
         OC == OpConstantPipeStorage || OC == OpConstantFunctionPointerINTEL ||
         isSpecConstantOpCode(OpCode);
}

inline bool isModuleScopeAllowedOpCode(Op OpCode) {
  return OpCode == OpVariable || OpCode == OpExtInst ||
         isConstantOpCode(OpCode);
}

inline bool isIntelSubgroupOpCode(Op OpCode) {
  unsigned OC = OpCode;
  return OpSubgroupShuffleINTEL <= OC && OC <= OpSubgroupImageBlockWriteINTEL;
}

inline bool isEventOpCode(Op OpCode) {
  return OpRetainEvent <= OpCode && OpCode <= OpCaptureEventProfilingInfo;
}

inline bool isSplitBarrierINTELOpCode(Op OpCode) {
  return OpCode == OpControlBarrierArriveINTEL ||
         OpCode == OpControlBarrierWaitINTEL;
}

} // namespace SPIRV

#endif // SPIRV_LIBSPIRV_SPIRVOPCODE_H
