//===-- PPCISelLowering.cpp - PPC DAG Lowering Implementation -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the PPCISelLowering class.
//
//===----------------------------------------------------------------------===//

#include "PPCISelLowering.h"
#include "PPCMachineFunctionInfo.h"
#include "PPCPredicates.h"
#include "PPCTargetMachine.h"
#include "PPCPerfectShuffle.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/VectorExtras.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CallingConv.h"
#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Intrinsics.h"
#include "llvm/ParameterAttributes.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/CommandLine.h"
using namespace llvm;

static cl::opt<bool> EnablePPCPreinc("enable-ppc-preinc", 
cl::desc("enable preincrement load/store generation on PPC (experimental)"),
                                     cl::Hidden);

PPCTargetLowering::PPCTargetLowering(PPCTargetMachine &TM)
  : TargetLowering(TM), PPCSubTarget(*TM.getSubtargetImpl()),
    PPCAtomicLabelIndex(0) {
    
  setPow2DivIsCheap();
  
  // Use _setjmp/_longjmp instead of setjmp/longjmp.
  setUseUnderscoreSetJmp(true);
  setUseUnderscoreLongJmp(true);
    
  // Set up the register classes.
  addRegisterClass(MVT::i32, PPC::GPRCRegisterClass);
  addRegisterClass(MVT::f32, PPC::F4RCRegisterClass);
  addRegisterClass(MVT::f64, PPC::F8RCRegisterClass);
  
  // PowerPC has an i16 but no i8 (or i1) SEXTLOAD
  setLoadXAction(ISD::SEXTLOAD, MVT::i1, Promote);
  setLoadXAction(ISD::SEXTLOAD, MVT::i8, Expand);

  setTruncStoreAction(MVT::f64, MVT::f32, Expand);
    
  // PowerPC has pre-inc load and store's.
  setIndexedLoadAction(ISD::PRE_INC, MVT::i1, Legal);
  setIndexedLoadAction(ISD::PRE_INC, MVT::i8, Legal);
  setIndexedLoadAction(ISD::PRE_INC, MVT::i16, Legal);
  setIndexedLoadAction(ISD::PRE_INC, MVT::i32, Legal);
  setIndexedLoadAction(ISD::PRE_INC, MVT::i64, Legal);
  setIndexedStoreAction(ISD::PRE_INC, MVT::i1, Legal);
  setIndexedStoreAction(ISD::PRE_INC, MVT::i8, Legal);
  setIndexedStoreAction(ISD::PRE_INC, MVT::i16, Legal);
  setIndexedStoreAction(ISD::PRE_INC, MVT::i32, Legal);
  setIndexedStoreAction(ISD::PRE_INC, MVT::i64, Legal);

  // Shortening conversions involving ppcf128 get expanded (2 regs -> 1 reg)
  setConvertAction(MVT::ppcf128, MVT::f64, Expand);
  setConvertAction(MVT::ppcf128, MVT::f32, Expand);
  // This is used in the ppcf128->int sequence.  Note it has different semantics
  // from FP_ROUND:  that rounds to nearest, this rounds to zero.
  setOperationAction(ISD::FP_ROUND_INREG, MVT::ppcf128, Custom);

  // PowerPC has no intrinsics for these particular operations
  setOperationAction(ISD::MEMBARRIER, MVT::Other, Expand);

  // PowerPC has no SREM/UREM instructions
  setOperationAction(ISD::SREM, MVT::i32, Expand);
  setOperationAction(ISD::UREM, MVT::i32, Expand);
  setOperationAction(ISD::SREM, MVT::i64, Expand);
  setOperationAction(ISD::UREM, MVT::i64, Expand);

  // Don't use SMUL_LOHI/UMUL_LOHI or SDIVREM/UDIVREM to lower SREM/UREM.
  setOperationAction(ISD::UMUL_LOHI, MVT::i32, Expand);
  setOperationAction(ISD::SMUL_LOHI, MVT::i32, Expand);
  setOperationAction(ISD::UMUL_LOHI, MVT::i64, Expand);
  setOperationAction(ISD::SMUL_LOHI, MVT::i64, Expand);
  setOperationAction(ISD::UDIVREM, MVT::i32, Expand);
  setOperationAction(ISD::SDIVREM, MVT::i32, Expand);
  setOperationAction(ISD::UDIVREM, MVT::i64, Expand);
  setOperationAction(ISD::SDIVREM, MVT::i64, Expand);
  
  // We don't support sin/cos/sqrt/fmod/pow
  setOperationAction(ISD::FSIN , MVT::f64, Expand);
  setOperationAction(ISD::FCOS , MVT::f64, Expand);
  setOperationAction(ISD::FREM , MVT::f64, Expand);
  setOperationAction(ISD::FPOW , MVT::f64, Expand);
  setOperationAction(ISD::FSIN , MVT::f32, Expand);
  setOperationAction(ISD::FCOS , MVT::f32, Expand);
  setOperationAction(ISD::FREM , MVT::f32, Expand);
  setOperationAction(ISD::FPOW , MVT::f32, Expand);

  setOperationAction(ISD::FLT_ROUNDS_, MVT::i32, Custom);
  
  // If we're enabling GP optimizations, use hardware square root
  if (!TM.getSubtarget<PPCSubtarget>().hasFSQRT()) {
    setOperationAction(ISD::FSQRT, MVT::f64, Expand);
    setOperationAction(ISD::FSQRT, MVT::f32, Expand);
  }
  
  setOperationAction(ISD::FCOPYSIGN, MVT::f64, Expand);
  setOperationAction(ISD::FCOPYSIGN, MVT::f32, Expand);
  
  // PowerPC does not have BSWAP, CTPOP or CTTZ
  setOperationAction(ISD::BSWAP, MVT::i32  , Expand);
  setOperationAction(ISD::CTPOP, MVT::i32  , Expand);
  setOperationAction(ISD::CTTZ , MVT::i32  , Expand);
  setOperationAction(ISD::BSWAP, MVT::i64  , Expand);
  setOperationAction(ISD::CTPOP, MVT::i64  , Expand);
  setOperationAction(ISD::CTTZ , MVT::i64  , Expand);
  
  // PowerPC does not have ROTR
  setOperationAction(ISD::ROTR, MVT::i32   , Expand);
  
  // PowerPC does not have Select
  setOperationAction(ISD::SELECT, MVT::i32, Expand);
  setOperationAction(ISD::SELECT, MVT::i64, Expand);
  setOperationAction(ISD::SELECT, MVT::f32, Expand);
  setOperationAction(ISD::SELECT, MVT::f64, Expand);
  
  // PowerPC wants to turn select_cc of FP into fsel when possible.
  setOperationAction(ISD::SELECT_CC, MVT::f32, Custom);
  setOperationAction(ISD::SELECT_CC, MVT::f64, Custom);

  // PowerPC wants to optimize integer setcc a bit
  setOperationAction(ISD::SETCC, MVT::i32, Custom);
  
  // PowerPC does not have BRCOND which requires SetCC
  setOperationAction(ISD::BRCOND, MVT::Other, Expand);

  setOperationAction(ISD::BR_JT,  MVT::Other, Expand);
  
  // PowerPC turns FP_TO_SINT into FCTIWZ and some load/stores.
  setOperationAction(ISD::FP_TO_SINT, MVT::i32, Custom);

  // PowerPC does not have [U|S]INT_TO_FP
  setOperationAction(ISD::SINT_TO_FP, MVT::i32, Expand);
  setOperationAction(ISD::UINT_TO_FP, MVT::i32, Expand);

  setOperationAction(ISD::BIT_CONVERT, MVT::f32, Expand);
  setOperationAction(ISD::BIT_CONVERT, MVT::i32, Expand);
  setOperationAction(ISD::BIT_CONVERT, MVT::i64, Expand);
  setOperationAction(ISD::BIT_CONVERT, MVT::f64, Expand);

  // We cannot sextinreg(i1).  Expand to shifts.
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1, Expand);

  // Support label based line numbers.
  setOperationAction(ISD::LOCATION, MVT::Other, Expand);
  setOperationAction(ISD::DEBUG_LOC, MVT::Other, Expand);
  
  setOperationAction(ISD::EXCEPTIONADDR, MVT::i64, Expand);
  setOperationAction(ISD::EHSELECTION,   MVT::i64, Expand);
  setOperationAction(ISD::EXCEPTIONADDR, MVT::i32, Expand);
  setOperationAction(ISD::EHSELECTION,   MVT::i32, Expand);
  
  
  // We want to legalize GlobalAddress and ConstantPool nodes into the 
  // appropriate instructions to materialize the address.
  setOperationAction(ISD::GlobalAddress, MVT::i32, Custom);
  setOperationAction(ISD::GlobalTLSAddress, MVT::i32, Custom);
  setOperationAction(ISD::ConstantPool,  MVT::i32, Custom);
  setOperationAction(ISD::JumpTable,     MVT::i32, Custom);
  setOperationAction(ISD::GlobalAddress, MVT::i64, Custom);
  setOperationAction(ISD::GlobalTLSAddress, MVT::i64, Custom);
  setOperationAction(ISD::ConstantPool,  MVT::i64, Custom);
  setOperationAction(ISD::JumpTable,     MVT::i64, Custom);
  
  // RET must be custom lowered, to meet ABI requirements
  setOperationAction(ISD::RET               , MVT::Other, Custom);

  // VASTART needs to be custom lowered to use the VarArgsFrameIndex
  setOperationAction(ISD::VASTART           , MVT::Other, Custom);
  
  // VAARG is custom lowered with ELF 32 ABI
  if (TM.getSubtarget<PPCSubtarget>().isELF32_ABI())
    setOperationAction(ISD::VAARG, MVT::Other, Custom);
  else
    setOperationAction(ISD::VAARG, MVT::Other, Expand);
  
  // Use the default implementation.
  setOperationAction(ISD::VACOPY            , MVT::Other, Expand);
  setOperationAction(ISD::VAEND             , MVT::Other, Expand);
  setOperationAction(ISD::STACKSAVE         , MVT::Other, Expand); 
  setOperationAction(ISD::STACKRESTORE      , MVT::Other, Custom);
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32  , Custom);
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i64  , Custom);

  setOperationAction(ISD::ATOMIC_LAS        , MVT::i32  , Custom);
  setOperationAction(ISD::ATOMIC_LCS        , MVT::i32  , Custom);
  setOperationAction(ISD::ATOMIC_SWAP       , MVT::i32  , Custom);
  if (TM.getSubtarget<PPCSubtarget>().has64BitSupport()) {
    setOperationAction(ISD::ATOMIC_LAS      , MVT::i64  , Custom);
    setOperationAction(ISD::ATOMIC_LCS      , MVT::i64  , Custom);
    setOperationAction(ISD::ATOMIC_SWAP     , MVT::i64  , Custom);
  }

  // We want to custom lower some of our intrinsics.
  setOperationAction(ISD::INTRINSIC_WO_CHAIN, MVT::Other, Custom);
  
  if (TM.getSubtarget<PPCSubtarget>().has64BitSupport()) {
    // They also have instructions for converting between i64 and fp.
    setOperationAction(ISD::FP_TO_SINT, MVT::i64, Custom);
    setOperationAction(ISD::FP_TO_UINT, MVT::i64, Expand);
    setOperationAction(ISD::SINT_TO_FP, MVT::i64, Custom);
    setOperationAction(ISD::UINT_TO_FP, MVT::i64, Expand);
    setOperationAction(ISD::FP_TO_UINT, MVT::i32, Expand);
 
    // FIXME: disable this lowered code.  This generates 64-bit register values,
    // and we don't model the fact that the top part is clobbered by calls.  We
    // need to flag these together so that the value isn't live across a call.
    //setOperationAction(ISD::SINT_TO_FP, MVT::i32, Custom);
    
    // To take advantage of the above i64 FP_TO_SINT, promote i32 FP_TO_UINT
    setOperationAction(ISD::FP_TO_UINT, MVT::i32, Promote);
  } else {
    // PowerPC does not have FP_TO_UINT on 32-bit implementations.
    setOperationAction(ISD::FP_TO_UINT, MVT::i32, Expand);
  }

  if (TM.getSubtarget<PPCSubtarget>().use64BitRegs()) {
    // 64-bit PowerPC implementations can support i64 types directly
    addRegisterClass(MVT::i64, PPC::G8RCRegisterClass);
    // BUILD_PAIR can't be handled natively, and should be expanded to shl/or
    setOperationAction(ISD::BUILD_PAIR, MVT::i64, Expand);
    // 64-bit PowerPC wants to expand i128 shifts itself.
    setOperationAction(ISD::SHL_PARTS, MVT::i64, Custom);
    setOperationAction(ISD::SRA_PARTS, MVT::i64, Custom);
    setOperationAction(ISD::SRL_PARTS, MVT::i64, Custom);
  } else {
    // 32-bit PowerPC wants to expand i64 shifts itself.
    setOperationAction(ISD::SHL_PARTS, MVT::i32, Custom);
    setOperationAction(ISD::SRA_PARTS, MVT::i32, Custom);
    setOperationAction(ISD::SRL_PARTS, MVT::i32, Custom);
  }

  if (TM.getSubtarget<PPCSubtarget>().hasAltivec()) {
    // First set operation action for all vector types to expand. Then we
    // will selectively turn on ones that can be effectively codegen'd.
    for (unsigned VT = (unsigned)MVT::FIRST_VECTOR_VALUETYPE;
         VT <= (unsigned)MVT::LAST_VECTOR_VALUETYPE; ++VT) {
      // add/sub are legal for all supported vector VT's.
      setOperationAction(ISD::ADD , (MVT::ValueType)VT, Legal);
      setOperationAction(ISD::SUB , (MVT::ValueType)VT, Legal);
      
      // We promote all shuffles to v16i8.
      setOperationAction(ISD::VECTOR_SHUFFLE, (MVT::ValueType)VT, Promote);
      AddPromotedToType (ISD::VECTOR_SHUFFLE, (MVT::ValueType)VT, MVT::v16i8);

      // We promote all non-typed operations to v4i32.
      setOperationAction(ISD::AND   , (MVT::ValueType)VT, Promote);
      AddPromotedToType (ISD::AND   , (MVT::ValueType)VT, MVT::v4i32);
      setOperationAction(ISD::OR    , (MVT::ValueType)VT, Promote);
      AddPromotedToType (ISD::OR    , (MVT::ValueType)VT, MVT::v4i32);
      setOperationAction(ISD::XOR   , (MVT::ValueType)VT, Promote);
      AddPromotedToType (ISD::XOR   , (MVT::ValueType)VT, MVT::v4i32);
      setOperationAction(ISD::LOAD  , (MVT::ValueType)VT, Promote);
      AddPromotedToType (ISD::LOAD  , (MVT::ValueType)VT, MVT::v4i32);
      setOperationAction(ISD::SELECT, (MVT::ValueType)VT, Promote);
      AddPromotedToType (ISD::SELECT, (MVT::ValueType)VT, MVT::v4i32);
      setOperationAction(ISD::STORE, (MVT::ValueType)VT, Promote);
      AddPromotedToType (ISD::STORE, (MVT::ValueType)VT, MVT::v4i32);
      
      // No other operations are legal.
      setOperationAction(ISD::MUL , (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::SDIV, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::SREM, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::UDIV, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::UREM, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::FDIV, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::FNEG, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::EXTRACT_VECTOR_ELT, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::INSERT_VECTOR_ELT, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::BUILD_VECTOR, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::UMUL_LOHI, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::SMUL_LOHI, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::UDIVREM, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::SDIVREM, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::SCALAR_TO_VECTOR, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::FPOW, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::CTPOP, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::CTLZ, (MVT::ValueType)VT, Expand);
      setOperationAction(ISD::CTTZ, (MVT::ValueType)VT, Expand);
    }

    // We can custom expand all VECTOR_SHUFFLEs to VPERM, others we can handle
    // with merges, splats, etc.
    setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v16i8, Custom);

    setOperationAction(ISD::AND   , MVT::v4i32, Legal);
    setOperationAction(ISD::OR    , MVT::v4i32, Legal);
    setOperationAction(ISD::XOR   , MVT::v4i32, Legal);
    setOperationAction(ISD::LOAD  , MVT::v4i32, Legal);
    setOperationAction(ISD::SELECT, MVT::v4i32, Expand);
    setOperationAction(ISD::STORE , MVT::v4i32, Legal);
    
    addRegisterClass(MVT::v4f32, PPC::VRRCRegisterClass);
    addRegisterClass(MVT::v4i32, PPC::VRRCRegisterClass);
    addRegisterClass(MVT::v8i16, PPC::VRRCRegisterClass);
    addRegisterClass(MVT::v16i8, PPC::VRRCRegisterClass);
    
    setOperationAction(ISD::MUL, MVT::v4f32, Legal);
    setOperationAction(ISD::MUL, MVT::v4i32, Custom);
    setOperationAction(ISD::MUL, MVT::v8i16, Custom);
    setOperationAction(ISD::MUL, MVT::v16i8, Custom);

    setOperationAction(ISD::SCALAR_TO_VECTOR, MVT::v4f32, Custom);
    setOperationAction(ISD::SCALAR_TO_VECTOR, MVT::v4i32, Custom);
    
    setOperationAction(ISD::BUILD_VECTOR, MVT::v16i8, Custom);
    setOperationAction(ISD::BUILD_VECTOR, MVT::v8i16, Custom);
    setOperationAction(ISD::BUILD_VECTOR, MVT::v4i32, Custom);
    setOperationAction(ISD::BUILD_VECTOR, MVT::v4f32, Custom);
  }
  
  setShiftAmountType(MVT::i32);
  setSetCCResultContents(ZeroOrOneSetCCResult);
  
  if (TM.getSubtarget<PPCSubtarget>().isPPC64()) {
    setStackPointerRegisterToSaveRestore(PPC::X1);
    setExceptionPointerRegister(PPC::X3);
    setExceptionSelectorRegister(PPC::X4);
  } else {
    setStackPointerRegisterToSaveRestore(PPC::R1);
    setExceptionPointerRegister(PPC::R3);
    setExceptionSelectorRegister(PPC::R4);
  }
  
  // We have target-specific dag combine patterns for the following nodes:
  setTargetDAGCombine(ISD::SINT_TO_FP);
  setTargetDAGCombine(ISD::STORE);
  setTargetDAGCombine(ISD::BR_CC);
  setTargetDAGCombine(ISD::BSWAP);
  
  // Darwin long double math library functions have $LDBL128 appended.
  if (TM.getSubtarget<PPCSubtarget>().isDarwin()) {
    setLibcallName(RTLIB::COS_PPCF128, "cosl$LDBL128");
    setLibcallName(RTLIB::POW_PPCF128, "powl$LDBL128");
    setLibcallName(RTLIB::REM_PPCF128, "fmodl$LDBL128");
    setLibcallName(RTLIB::SIN_PPCF128, "sinl$LDBL128");
    setLibcallName(RTLIB::SQRT_PPCF128, "sqrtl$LDBL128");
  }

  computeRegisterProperties();
}

/// getByValTypeAlignment - Return the desired alignment for ByVal aggregate
/// function arguments in the caller parameter area.
unsigned PPCTargetLowering::getByValTypeAlignment(const Type *Ty) const {
  TargetMachine &TM = getTargetMachine();
  // Darwin passes everything on 4 byte boundary.
  if (TM.getSubtarget<PPCSubtarget>().isDarwin())
    return 4;
  // FIXME Elf TBD
  return 4;
}

const char *PPCTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  default: return 0;
  case PPCISD::FSEL:          return "PPCISD::FSEL";
  case PPCISD::FCFID:         return "PPCISD::FCFID";
  case PPCISD::FCTIDZ:        return "PPCISD::FCTIDZ";
  case PPCISD::FCTIWZ:        return "PPCISD::FCTIWZ";
  case PPCISD::STFIWX:        return "PPCISD::STFIWX";
  case PPCISD::VMADDFP:       return "PPCISD::VMADDFP";
  case PPCISD::VNMSUBFP:      return "PPCISD::VNMSUBFP";
  case PPCISD::VPERM:         return "PPCISD::VPERM";
  case PPCISD::Hi:            return "PPCISD::Hi";
  case PPCISD::Lo:            return "PPCISD::Lo";
  case PPCISD::DYNALLOC:      return "PPCISD::DYNALLOC";
  case PPCISD::GlobalBaseReg: return "PPCISD::GlobalBaseReg";
  case PPCISD::SRL:           return "PPCISD::SRL";
  case PPCISD::SRA:           return "PPCISD::SRA";
  case PPCISD::SHL:           return "PPCISD::SHL";
  case PPCISD::EXTSW_32:      return "PPCISD::EXTSW_32";
  case PPCISD::STD_32:        return "PPCISD::STD_32";
  case PPCISD::CALL_ELF:      return "PPCISD::CALL_ELF";
  case PPCISD::CALL_Macho:    return "PPCISD::CALL_Macho";
  case PPCISD::MTCTR:         return "PPCISD::MTCTR";
  case PPCISD::BCTRL_Macho:   return "PPCISD::BCTRL_Macho";
  case PPCISD::BCTRL_ELF:     return "PPCISD::BCTRL_ELF";
  case PPCISD::RET_FLAG:      return "PPCISD::RET_FLAG";
  case PPCISD::MFCR:          return "PPCISD::MFCR";
  case PPCISD::VCMP:          return "PPCISD::VCMP";
  case PPCISD::VCMPo:         return "PPCISD::VCMPo";
  case PPCISD::LBRX:          return "PPCISD::LBRX";
  case PPCISD::STBRX:         return "PPCISD::STBRX";
  case PPCISD::LARX:          return "PPCISD::LARX";
  case PPCISD::STCX:          return "PPCISD::STCX";
  case PPCISD::CMP_UNRESERVE: return "PPCISD::CMP_UNRESERVE";
  case PPCISD::COND_BRANCH:   return "PPCISD::COND_BRANCH";
  case PPCISD::MFFS:          return "PPCISD::MFFS";
  case PPCISD::MTFSB0:        return "PPCISD::MTFSB0";
  case PPCISD::MTFSB1:        return "PPCISD::MTFSB1";
  case PPCISD::FADDRTZ:       return "PPCISD::FADDRTZ";
  case PPCISD::MTFSF:         return "PPCISD::MTFSF";
  case PPCISD::TAILCALL:      return "PPCISD::TAILCALL";
  case PPCISD::TC_RETURN:     return "PPCISD::TC_RETURN";
  }
}


MVT::ValueType
PPCTargetLowering::getSetCCResultType(const SDOperand &) const {
  return MVT::i32;
}


//===----------------------------------------------------------------------===//
// Node matching predicates, for use by the tblgen matching code.
//===----------------------------------------------------------------------===//

/// isFloatingPointZero - Return true if this is 0.0 or -0.0.
static bool isFloatingPointZero(SDOperand Op) {
  if (ConstantFPSDNode *CFP = dyn_cast<ConstantFPSDNode>(Op))
    return CFP->getValueAPF().isZero();
  else if (ISD::isEXTLoad(Op.Val) || ISD::isNON_EXTLoad(Op.Val)) {
    // Maybe this has already been legalized into the constant pool?
    if (ConstantPoolSDNode *CP = dyn_cast<ConstantPoolSDNode>(Op.getOperand(1)))
      if (ConstantFP *CFP = dyn_cast<ConstantFP>(CP->getConstVal()))
        return CFP->getValueAPF().isZero();
  }
  return false;
}

/// isConstantOrUndef - Op is either an undef node or a ConstantSDNode.  Return
/// true if Op is undef or if it matches the specified value.
static bool isConstantOrUndef(SDOperand Op, unsigned Val) {
  return Op.getOpcode() == ISD::UNDEF || 
         cast<ConstantSDNode>(Op)->getValue() == Val;
}

/// isVPKUHUMShuffleMask - Return true if this is the shuffle mask for a
/// VPKUHUM instruction.
bool PPC::isVPKUHUMShuffleMask(SDNode *N, bool isUnary) {
  if (!isUnary) {
    for (unsigned i = 0; i != 16; ++i)
      if (!isConstantOrUndef(N->getOperand(i),  i*2+1))
        return false;
  } else {
    for (unsigned i = 0; i != 8; ++i)
      if (!isConstantOrUndef(N->getOperand(i),  i*2+1) ||
          !isConstantOrUndef(N->getOperand(i+8),  i*2+1))
        return false;
  }
  return true;
}

/// isVPKUWUMShuffleMask - Return true if this is the shuffle mask for a
/// VPKUWUM instruction.
bool PPC::isVPKUWUMShuffleMask(SDNode *N, bool isUnary) {
  if (!isUnary) {
    for (unsigned i = 0; i != 16; i += 2)
      if (!isConstantOrUndef(N->getOperand(i  ),  i*2+2) ||
          !isConstantOrUndef(N->getOperand(i+1),  i*2+3))
        return false;
  } else {
    for (unsigned i = 0; i != 8; i += 2)
      if (!isConstantOrUndef(N->getOperand(i  ),  i*2+2) ||
          !isConstantOrUndef(N->getOperand(i+1),  i*2+3) ||
          !isConstantOrUndef(N->getOperand(i+8),  i*2+2) ||
          !isConstantOrUndef(N->getOperand(i+9),  i*2+3))
        return false;
  }
  return true;
}

/// isVMerge - Common function, used to match vmrg* shuffles.
///
static bool isVMerge(SDNode *N, unsigned UnitSize, 
                     unsigned LHSStart, unsigned RHSStart) {
  assert(N->getOpcode() == ISD::BUILD_VECTOR &&
         N->getNumOperands() == 16 && "PPC only supports shuffles by bytes!");
  assert((UnitSize == 1 || UnitSize == 2 || UnitSize == 4) &&
         "Unsupported merge size!");
  
  for (unsigned i = 0; i != 8/UnitSize; ++i)     // Step over units
    for (unsigned j = 0; j != UnitSize; ++j) {   // Step over bytes within unit
      if (!isConstantOrUndef(N->getOperand(i*UnitSize*2+j),
                             LHSStart+j+i*UnitSize) ||
          !isConstantOrUndef(N->getOperand(i*UnitSize*2+UnitSize+j),
                             RHSStart+j+i*UnitSize))
        return false;
    }
      return true;
}

/// isVMRGLShuffleMask - Return true if this is a shuffle mask suitable for
/// a VRGL* instruction with the specified unit size (1,2 or 4 bytes).
bool PPC::isVMRGLShuffleMask(SDNode *N, unsigned UnitSize, bool isUnary) {
  if (!isUnary)
    return isVMerge(N, UnitSize, 8, 24);
  return isVMerge(N, UnitSize, 8, 8);
}

/// isVMRGHShuffleMask - Return true if this is a shuffle mask suitable for
/// a VRGH* instruction with the specified unit size (1,2 or 4 bytes).
bool PPC::isVMRGHShuffleMask(SDNode *N, unsigned UnitSize, bool isUnary) {
  if (!isUnary)
    return isVMerge(N, UnitSize, 0, 16);
  return isVMerge(N, UnitSize, 0, 0);
}


/// isVSLDOIShuffleMask - If this is a vsldoi shuffle mask, return the shift
/// amount, otherwise return -1.
int PPC::isVSLDOIShuffleMask(SDNode *N, bool isUnary) {
  assert(N->getOpcode() == ISD::BUILD_VECTOR &&
         N->getNumOperands() == 16 && "PPC only supports shuffles by bytes!");
  // Find the first non-undef value in the shuffle mask.
  unsigned i;
  for (i = 0; i != 16 && N->getOperand(i).getOpcode() == ISD::UNDEF; ++i)
    /*search*/;
  
  if (i == 16) return -1;  // all undef.
  
  // Otherwise, check to see if the rest of the elements are consequtively
  // numbered from this value.
  unsigned ShiftAmt = cast<ConstantSDNode>(N->getOperand(i))->getValue();
  if (ShiftAmt < i) return -1;
  ShiftAmt -= i;

  if (!isUnary) {
    // Check the rest of the elements to see if they are consequtive.
    for (++i; i != 16; ++i)
      if (!isConstantOrUndef(N->getOperand(i), ShiftAmt+i))
        return -1;
  } else {
    // Check the rest of the elements to see if they are consequtive.
    for (++i; i != 16; ++i)
      if (!isConstantOrUndef(N->getOperand(i), (ShiftAmt+i) & 15))
        return -1;
  }
  
  return ShiftAmt;
}

/// isSplatShuffleMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a splat of a single element that is suitable for input to
/// VSPLTB/VSPLTH/VSPLTW.
bool PPC::isSplatShuffleMask(SDNode *N, unsigned EltSize) {
  assert(N->getOpcode() == ISD::BUILD_VECTOR &&
         N->getNumOperands() == 16 &&
         (EltSize == 1 || EltSize == 2 || EltSize == 4));
  
  // This is a splat operation if each element of the permute is the same, and
  // if the value doesn't reference the second vector.
  unsigned ElementBase = 0;
  SDOperand Elt = N->getOperand(0);
  if (ConstantSDNode *EltV = dyn_cast<ConstantSDNode>(Elt))
    ElementBase = EltV->getValue();
  else
    return false;   // FIXME: Handle UNDEF elements too!

  if (cast<ConstantSDNode>(Elt)->getValue() >= 16)
    return false;
  
  // Check that they are consequtive.
  for (unsigned i = 1; i != EltSize; ++i) {
    if (!isa<ConstantSDNode>(N->getOperand(i)) ||
        cast<ConstantSDNode>(N->getOperand(i))->getValue() != i+ElementBase)
      return false;
  }
  
  assert(isa<ConstantSDNode>(Elt) && "Invalid VECTOR_SHUFFLE mask!");
  for (unsigned i = EltSize, e = 16; i != e; i += EltSize) {
    if (N->getOperand(i).getOpcode() == ISD::UNDEF) continue;
    assert(isa<ConstantSDNode>(N->getOperand(i)) &&
           "Invalid VECTOR_SHUFFLE mask!");
    for (unsigned j = 0; j != EltSize; ++j)
      if (N->getOperand(i+j) != N->getOperand(j))
        return false;
  }

  return true;
}

/// isAllNegativeZeroVector - Returns true if all elements of build_vector
/// are -0.0.
bool PPC::isAllNegativeZeroVector(SDNode *N) {
  assert(N->getOpcode() == ISD::BUILD_VECTOR);
  if (PPC::isSplatShuffleMask(N, N->getNumOperands()))
    if (ConstantFPSDNode *CFP = dyn_cast<ConstantFPSDNode>(N))
      return CFP->getValueAPF().isNegZero();
  return false;
}

/// getVSPLTImmediate - Return the appropriate VSPLT* immediate to splat the
/// specified isSplatShuffleMask VECTOR_SHUFFLE mask.
unsigned PPC::getVSPLTImmediate(SDNode *N, unsigned EltSize) {
  assert(isSplatShuffleMask(N, EltSize));
  return cast<ConstantSDNode>(N->getOperand(0))->getValue() / EltSize;
}

/// get_VSPLTI_elt - If this is a build_vector of constants which can be formed
/// by using a vspltis[bhw] instruction of the specified element size, return
/// the constant being splatted.  The ByteSize field indicates the number of
/// bytes of each element [124] -> [bhw].
SDOperand PPC::get_VSPLTI_elt(SDNode *N, unsigned ByteSize, SelectionDAG &DAG) {
  SDOperand OpVal(0, 0);

  // If ByteSize of the splat is bigger than the element size of the
  // build_vector, then we have a case where we are checking for a splat where
  // multiple elements of the buildvector are folded together into a single
  // logical element of the splat (e.g. "vsplish 1" to splat {0,1}*8).
  unsigned EltSize = 16/N->getNumOperands();
  if (EltSize < ByteSize) {
    unsigned Multiple = ByteSize/EltSize;   // Number of BV entries per spltval.
    SDOperand UniquedVals[4];
    assert(Multiple > 1 && Multiple <= 4 && "How can this happen?");
    
    // See if all of the elements in the buildvector agree across.
    for (unsigned i = 0, e = N->getNumOperands(); i != e; ++i) {
      if (N->getOperand(i).getOpcode() == ISD::UNDEF) continue;
      // If the element isn't a constant, bail fully out.
      if (!isa<ConstantSDNode>(N->getOperand(i))) return SDOperand();

          
      if (UniquedVals[i&(Multiple-1)].Val == 0)
        UniquedVals[i&(Multiple-1)] = N->getOperand(i);
      else if (UniquedVals[i&(Multiple-1)] != N->getOperand(i))
        return SDOperand();  // no match.
    }
    
    // Okay, if we reached this point, UniquedVals[0..Multiple-1] contains
    // either constant or undef values that are identical for each chunk.  See
    // if these chunks can form into a larger vspltis*.
    
    // Check to see if all of the leading entries are either 0 or -1.  If
    // neither, then this won't fit into the immediate field.
    bool LeadingZero = true;
    bool LeadingOnes = true;
    for (unsigned i = 0; i != Multiple-1; ++i) {
      if (UniquedVals[i].Val == 0) continue;  // Must have been undefs.
      
      LeadingZero &= cast<ConstantSDNode>(UniquedVals[i])->isNullValue();
      LeadingOnes &= cast<ConstantSDNode>(UniquedVals[i])->isAllOnesValue();
    }
    // Finally, check the least significant entry.
    if (LeadingZero) {
      if (UniquedVals[Multiple-1].Val == 0)
        return DAG.getTargetConstant(0, MVT::i32);  // 0,0,0,undef
      int Val = cast<ConstantSDNode>(UniquedVals[Multiple-1])->getValue();
      if (Val < 16)
        return DAG.getTargetConstant(Val, MVT::i32);  // 0,0,0,4 -> vspltisw(4)
    }
    if (LeadingOnes) {
      if (UniquedVals[Multiple-1].Val == 0)
        return DAG.getTargetConstant(~0U, MVT::i32);  // -1,-1,-1,undef
      int Val =cast<ConstantSDNode>(UniquedVals[Multiple-1])->getSignExtended();
      if (Val >= -16)                            // -1,-1,-1,-2 -> vspltisw(-2)
        return DAG.getTargetConstant(Val, MVT::i32);
    }
    
    return SDOperand();
  }
  
  // Check to see if this buildvec has a single non-undef value in its elements.
  for (unsigned i = 0, e = N->getNumOperands(); i != e; ++i) {
    if (N->getOperand(i).getOpcode() == ISD::UNDEF) continue;
    if (OpVal.Val == 0)
      OpVal = N->getOperand(i);
    else if (OpVal != N->getOperand(i))
      return SDOperand();
  }
  
  if (OpVal.Val == 0) return SDOperand();  // All UNDEF: use implicit def.
  
  unsigned ValSizeInBytes = 0;
  uint64_t Value = 0;
  if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(OpVal)) {
    Value = CN->getValue();
    ValSizeInBytes = MVT::getSizeInBits(CN->getValueType(0))/8;
  } else if (ConstantFPSDNode *CN = dyn_cast<ConstantFPSDNode>(OpVal)) {
    assert(CN->getValueType(0) == MVT::f32 && "Only one legal FP vector type!");
    Value = FloatToBits(CN->getValueAPF().convertToFloat());
    ValSizeInBytes = 4;
  }

  // If the splat value is larger than the element value, then we can never do
  // this splat.  The only case that we could fit the replicated bits into our
  // immediate field for would be zero, and we prefer to use vxor for it.
  if (ValSizeInBytes < ByteSize) return SDOperand();
  
  // If the element value is larger than the splat value, cut it in half and
  // check to see if the two halves are equal.  Continue doing this until we
  // get to ByteSize.  This allows us to handle 0x01010101 as 0x01.
  while (ValSizeInBytes > ByteSize) {
    ValSizeInBytes >>= 1;
    
    // If the top half equals the bottom half, we're still ok.
    if (((Value >> (ValSizeInBytes*8)) & ((1 << (8*ValSizeInBytes))-1)) !=
         (Value                        & ((1 << (8*ValSizeInBytes))-1)))
      return SDOperand();
  }

  // Properly sign extend the value.
  int ShAmt = (4-ByteSize)*8;
  int MaskVal = ((int)Value << ShAmt) >> ShAmt;
  
  // If this is zero, don't match, zero matches ISD::isBuildVectorAllZeros.
  if (MaskVal == 0) return SDOperand();

  // Finally, if this value fits in a 5 bit sext field, return it
  if (((MaskVal << (32-5)) >> (32-5)) == MaskVal)
    return DAG.getTargetConstant(MaskVal, MVT::i32);
  return SDOperand();
}

//===----------------------------------------------------------------------===//
//  Addressing Mode Selection
//===----------------------------------------------------------------------===//

/// isIntS16Immediate - This method tests to see if the node is either a 32-bit
/// or 64-bit immediate, and if the value can be accurately represented as a
/// sign extension from a 16-bit value.  If so, this returns true and the
/// immediate.
static bool isIntS16Immediate(SDNode *N, short &Imm) {
  if (N->getOpcode() != ISD::Constant)
    return false;
  
  Imm = (short)cast<ConstantSDNode>(N)->getValue();
  if (N->getValueType(0) == MVT::i32)
    return Imm == (int32_t)cast<ConstantSDNode>(N)->getValue();
  else
    return Imm == (int64_t)cast<ConstantSDNode>(N)->getValue();
}
static bool isIntS16Immediate(SDOperand Op, short &Imm) {
  return isIntS16Immediate(Op.Val, Imm);
}


/// SelectAddressRegReg - Given the specified addressed, check to see if it
/// can be represented as an indexed [r+r] operation.  Returns false if it
/// can be more efficiently represented with [r+imm].
bool PPCTargetLowering::SelectAddressRegReg(SDOperand N, SDOperand &Base,
                                            SDOperand &Index,
                                            SelectionDAG &DAG) {
  short imm = 0;
  if (N.getOpcode() == ISD::ADD) {
    if (isIntS16Immediate(N.getOperand(1), imm))
      return false;    // r+i
    if (N.getOperand(1).getOpcode() == PPCISD::Lo)
      return false;    // r+i
    
    Base = N.getOperand(0);
    Index = N.getOperand(1);
    return true;
  } else if (N.getOpcode() == ISD::OR) {
    if (isIntS16Immediate(N.getOperand(1), imm))
      return false;    // r+i can fold it if we can.
    
    // If this is an or of disjoint bitfields, we can codegen this as an add
    // (for better address arithmetic) if the LHS and RHS of the OR are provably
    // disjoint.
    APInt LHSKnownZero, LHSKnownOne;
    APInt RHSKnownZero, RHSKnownOne;
    DAG.ComputeMaskedBits(N.getOperand(0),
                          APInt::getAllOnesValue(N.getOperand(0)
                            .getValueSizeInBits()),
                          LHSKnownZero, LHSKnownOne);
    
    if (LHSKnownZero.getBoolValue()) {
      DAG.ComputeMaskedBits(N.getOperand(1),
                            APInt::getAllOnesValue(N.getOperand(1)
                              .getValueSizeInBits()),
                            RHSKnownZero, RHSKnownOne);
      // If all of the bits are known zero on the LHS or RHS, the add won't
      // carry.
      if (~(LHSKnownZero | RHSKnownZero) == 0) {
        Base = N.getOperand(0);
        Index = N.getOperand(1);
        return true;
      }
    }
  }
  
  return false;
}

/// Returns true if the address N can be represented by a base register plus
/// a signed 16-bit displacement [r+imm], and if it is not better
/// represented as reg+reg.
bool PPCTargetLowering::SelectAddressRegImm(SDOperand N, SDOperand &Disp,
                                            SDOperand &Base, SelectionDAG &DAG){
  // If this can be more profitably realized as r+r, fail.
  if (SelectAddressRegReg(N, Disp, Base, DAG))
    return false;
  
  if (N.getOpcode() == ISD::ADD) {
    short imm = 0;
    if (isIntS16Immediate(N.getOperand(1), imm)) {
      Disp = DAG.getTargetConstant((int)imm & 0xFFFF, MVT::i32);
      if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(N.getOperand(0))) {
        Base = DAG.getTargetFrameIndex(FI->getIndex(), N.getValueType());
      } else {
        Base = N.getOperand(0);
      }
      return true; // [r+i]
    } else if (N.getOperand(1).getOpcode() == PPCISD::Lo) {
      // Match LOAD (ADD (X, Lo(G))).
      assert(!cast<ConstantSDNode>(N.getOperand(1).getOperand(1))->getValue()
             && "Cannot handle constant offsets yet!");
      Disp = N.getOperand(1).getOperand(0);  // The global address.
      assert(Disp.getOpcode() == ISD::TargetGlobalAddress ||
             Disp.getOpcode() == ISD::TargetConstantPool ||
             Disp.getOpcode() == ISD::TargetJumpTable);
      Base = N.getOperand(0);
      return true;  // [&g+r]
    }
  } else if (N.getOpcode() == ISD::OR) {
    short imm = 0;
    if (isIntS16Immediate(N.getOperand(1), imm)) {
      // If this is an or of disjoint bitfields, we can codegen this as an add
      // (for better address arithmetic) if the LHS and RHS of the OR are
      // provably disjoint.
      APInt LHSKnownZero, LHSKnownOne;
      DAG.ComputeMaskedBits(N.getOperand(0),
                            APInt::getAllOnesValue(N.getOperand(0)
                                                   .getValueSizeInBits()),
                            LHSKnownZero, LHSKnownOne);

      if ((LHSKnownZero.getZExtValue()|~(uint64_t)imm) == ~0ULL) {
        // If all of the bits are known zero on the LHS or RHS, the add won't
        // carry.
        Base = N.getOperand(0);
        Disp = DAG.getTargetConstant((int)imm & 0xFFFF, MVT::i32);
        return true;
      }
    }
  } else if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N)) {
    // Loading from a constant address.
    
    // If this address fits entirely in a 16-bit sext immediate field, codegen
    // this as "d, 0"
    short Imm;
    if (isIntS16Immediate(CN, Imm)) {
      Disp = DAG.getTargetConstant(Imm, CN->getValueType(0));
      Base = DAG.getRegister(PPC::R0, CN->getValueType(0));
      return true;
    }

    // Handle 32-bit sext immediates with LIS + addr mode.
    if (CN->getValueType(0) == MVT::i32 ||
        (int64_t)CN->getValue() == (int)CN->getValue()) {
      int Addr = (int)CN->getValue();
      
      // Otherwise, break this down into an LIS + disp.
      Disp = DAG.getTargetConstant((short)Addr, MVT::i32);
      
      Base = DAG.getTargetConstant((Addr - (signed short)Addr) >> 16, MVT::i32);
      unsigned Opc = CN->getValueType(0) == MVT::i32 ? PPC::LIS : PPC::LIS8;
      Base = SDOperand(DAG.getTargetNode(Opc, CN->getValueType(0), Base), 0);
      return true;
    }
  }
  
  Disp = DAG.getTargetConstant(0, getPointerTy());
  if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(N))
    Base = DAG.getTargetFrameIndex(FI->getIndex(), N.getValueType());
  else
    Base = N;
  return true;      // [r+0]
}

/// SelectAddressRegRegOnly - Given the specified addressed, force it to be
/// represented as an indexed [r+r] operation.
bool PPCTargetLowering::SelectAddressRegRegOnly(SDOperand N, SDOperand &Base,
                                                SDOperand &Index,
                                                SelectionDAG &DAG) {
  // Check to see if we can easily represent this as an [r+r] address.  This
  // will fail if it thinks that the address is more profitably represented as
  // reg+imm, e.g. where imm = 0.
  if (SelectAddressRegReg(N, Base, Index, DAG))
    return true;
  
  // If the operand is an addition, always emit this as [r+r], since this is
  // better (for code size, and execution, as the memop does the add for free)
  // than emitting an explicit add.
  if (N.getOpcode() == ISD::ADD) {
    Base = N.getOperand(0);
    Index = N.getOperand(1);
    return true;
  }
  
  // Otherwise, do it the hard way, using R0 as the base register.
  Base = DAG.getRegister(PPC::R0, N.getValueType());
  Index = N;
  return true;
}

/// SelectAddressRegImmShift - Returns true if the address N can be
/// represented by a base register plus a signed 14-bit displacement
/// [r+imm*4].  Suitable for use by STD and friends.
bool PPCTargetLowering::SelectAddressRegImmShift(SDOperand N, SDOperand &Disp,
                                                 SDOperand &Base,
                                                 SelectionDAG &DAG) {
  // If this can be more profitably realized as r+r, fail.
  if (SelectAddressRegReg(N, Disp, Base, DAG))
    return false;
  
  if (N.getOpcode() == ISD::ADD) {
    short imm = 0;
    if (isIntS16Immediate(N.getOperand(1), imm) && (imm & 3) == 0) {
      Disp =  DAG.getTargetConstant(((int)imm & 0xFFFF) >> 2, MVT::i32);
      if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(N.getOperand(0))) {
        Base = DAG.getTargetFrameIndex(FI->getIndex(), N.getValueType());
      } else {
        Base = N.getOperand(0);
      }
      return true; // [r+i]
    } else if (N.getOperand(1).getOpcode() == PPCISD::Lo) {
      // Match LOAD (ADD (X, Lo(G))).
      assert(!cast<ConstantSDNode>(N.getOperand(1).getOperand(1))->getValue()
             && "Cannot handle constant offsets yet!");
      Disp = N.getOperand(1).getOperand(0);  // The global address.
      assert(Disp.getOpcode() == ISD::TargetGlobalAddress ||
             Disp.getOpcode() == ISD::TargetConstantPool ||
             Disp.getOpcode() == ISD::TargetJumpTable);
      Base = N.getOperand(0);
      return true;  // [&g+r]
    }
  } else if (N.getOpcode() == ISD::OR) {
    short imm = 0;
    if (isIntS16Immediate(N.getOperand(1), imm) && (imm & 3) == 0) {
      // If this is an or of disjoint bitfields, we can codegen this as an add
      // (for better address arithmetic) if the LHS and RHS of the OR are
      // provably disjoint.
      APInt LHSKnownZero, LHSKnownOne;
      DAG.ComputeMaskedBits(N.getOperand(0),
                            APInt::getAllOnesValue(N.getOperand(0)
                                                   .getValueSizeInBits()),
                            LHSKnownZero, LHSKnownOne);
      if ((LHSKnownZero.getZExtValue()|~(uint64_t)imm) == ~0ULL) {
        // If all of the bits are known zero on the LHS or RHS, the add won't
        // carry.
        Base = N.getOperand(0);
        Disp = DAG.getTargetConstant(((int)imm & 0xFFFF) >> 2, MVT::i32);
        return true;
      }
    }
  } else if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N)) {
    // Loading from a constant address.  Verify low two bits are clear.
    if ((CN->getValue() & 3) == 0) {
      // If this address fits entirely in a 14-bit sext immediate field, codegen
      // this as "d, 0"
      short Imm;
      if (isIntS16Immediate(CN, Imm)) {
        Disp = DAG.getTargetConstant((unsigned short)Imm >> 2, getPointerTy());
        Base = DAG.getRegister(PPC::R0, CN->getValueType(0));
        return true;
      }
    
      // Fold the low-part of 32-bit absolute addresses into addr mode.
      if (CN->getValueType(0) == MVT::i32 ||
          (int64_t)CN->getValue() == (int)CN->getValue()) {
        int Addr = (int)CN->getValue();
      
        // Otherwise, break this down into an LIS + disp.
        Disp = DAG.getTargetConstant((short)Addr >> 2, MVT::i32);
        
        Base = DAG.getTargetConstant((Addr-(signed short)Addr) >> 16, MVT::i32);
        unsigned Opc = CN->getValueType(0) == MVT::i32 ? PPC::LIS : PPC::LIS8;
        Base = SDOperand(DAG.getTargetNode(Opc, CN->getValueType(0), Base), 0);
        return true;
      }
    }
  }
  
  Disp = DAG.getTargetConstant(0, getPointerTy());
  if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(N))
    Base = DAG.getTargetFrameIndex(FI->getIndex(), N.getValueType());
  else
    Base = N;
  return true;      // [r+0]
}


/// getPreIndexedAddressParts - returns true by value, base pointer and
/// offset pointer and addressing mode by reference if the node's address
/// can be legally represented as pre-indexed load / store address.
bool PPCTargetLowering::getPreIndexedAddressParts(SDNode *N, SDOperand &Base,
                                                  SDOperand &Offset,
                                                  ISD::MemIndexedMode &AM,
                                                  SelectionDAG &DAG) {
  // Disabled by default for now.
  if (!EnablePPCPreinc) return false;
  
  SDOperand Ptr;
  MVT::ValueType VT;
  if (LoadSDNode *LD = dyn_cast<LoadSDNode>(N)) {
    Ptr = LD->getBasePtr();
    VT = LD->getMemoryVT();
    
  } else if (StoreSDNode *ST = dyn_cast<StoreSDNode>(N)) {
    ST = ST;
    Ptr = ST->getBasePtr();
    VT  = ST->getMemoryVT();
  } else
    return false;

  // PowerPC doesn't have preinc load/store instructions for vectors.
  if (MVT::isVector(VT))
    return false;
  
  // TODO: Check reg+reg first.
  
  // LDU/STU use reg+imm*4, others use reg+imm.
  if (VT != MVT::i64) {
    // reg + imm
    if (!SelectAddressRegImm(Ptr, Offset, Base, DAG))
      return false;
  } else {
    // reg + imm * 4.
    if (!SelectAddressRegImmShift(Ptr, Offset, Base, DAG))
      return false;
  }

  if (LoadSDNode *LD = dyn_cast<LoadSDNode>(N)) {
    // PPC64 doesn't have lwau, but it does have lwaux.  Reject preinc load of
    // sext i32 to i64 when addr mode is r+i.
    if (LD->getValueType(0) == MVT::i64 && LD->getMemoryVT() == MVT::i32 &&
        LD->getExtensionType() == ISD::SEXTLOAD &&
        isa<ConstantSDNode>(Offset))
      return false;
  }    
  
  AM = ISD::PRE_INC;
  return true;
}

//===----------------------------------------------------------------------===//
//  LowerOperation implementation
//===----------------------------------------------------------------------===//

SDOperand PPCTargetLowering::LowerConstantPool(SDOperand Op, 
                                             SelectionDAG &DAG) {
  MVT::ValueType PtrVT = Op.getValueType();
  ConstantPoolSDNode *CP = cast<ConstantPoolSDNode>(Op);
  Constant *C = CP->getConstVal();
  SDOperand CPI = DAG.getTargetConstantPool(C, PtrVT, CP->getAlignment());
  SDOperand Zero = DAG.getConstant(0, PtrVT);

  const TargetMachine &TM = DAG.getTarget();
  
  SDOperand Hi = DAG.getNode(PPCISD::Hi, PtrVT, CPI, Zero);
  SDOperand Lo = DAG.getNode(PPCISD::Lo, PtrVT, CPI, Zero);

  // If this is a non-darwin platform, we don't support non-static relo models
  // yet.
  if (TM.getRelocationModel() == Reloc::Static ||
      !TM.getSubtarget<PPCSubtarget>().isDarwin()) {
    // Generate non-pic code that has direct accesses to the constant pool.
    // The address of the global is just (hi(&g)+lo(&g)).
    return DAG.getNode(ISD::ADD, PtrVT, Hi, Lo);
  }
  
  if (TM.getRelocationModel() == Reloc::PIC_) {
    // With PIC, the first instruction is actually "GR+hi(&G)".
    Hi = DAG.getNode(ISD::ADD, PtrVT,
                     DAG.getNode(PPCISD::GlobalBaseReg, PtrVT), Hi);
  }
  
  Lo = DAG.getNode(ISD::ADD, PtrVT, Hi, Lo);
  return Lo;
}

SDOperand PPCTargetLowering::LowerJumpTable(SDOperand Op, SelectionDAG &DAG) {
  MVT::ValueType PtrVT = Op.getValueType();
  JumpTableSDNode *JT = cast<JumpTableSDNode>(Op);
  SDOperand JTI = DAG.getTargetJumpTable(JT->getIndex(), PtrVT);
  SDOperand Zero = DAG.getConstant(0, PtrVT);
  
  const TargetMachine &TM = DAG.getTarget();

  SDOperand Hi = DAG.getNode(PPCISD::Hi, PtrVT, JTI, Zero);
  SDOperand Lo = DAG.getNode(PPCISD::Lo, PtrVT, JTI, Zero);

  // If this is a non-darwin platform, we don't support non-static relo models
  // yet.
  if (TM.getRelocationModel() == Reloc::Static ||
      !TM.getSubtarget<PPCSubtarget>().isDarwin()) {
    // Generate non-pic code that has direct accesses to the constant pool.
    // The address of the global is just (hi(&g)+lo(&g)).
    return DAG.getNode(ISD::ADD, PtrVT, Hi, Lo);
  }
  
  if (TM.getRelocationModel() == Reloc::PIC_) {
    // With PIC, the first instruction is actually "GR+hi(&G)".
    Hi = DAG.getNode(ISD::ADD, PtrVT,
                     DAG.getNode(PPCISD::GlobalBaseReg, PtrVT), Hi);
  }
  
  Lo = DAG.getNode(ISD::ADD, PtrVT, Hi, Lo);
  return Lo;
}

SDOperand PPCTargetLowering::LowerGlobalTLSAddress(SDOperand Op, 
                                                   SelectionDAG &DAG) {
  assert(0 && "TLS not implemented for PPC.");
  return SDOperand(); // Not reached
}

SDOperand PPCTargetLowering::LowerGlobalAddress(SDOperand Op, 
                                                SelectionDAG &DAG) {
  MVT::ValueType PtrVT = Op.getValueType();
  GlobalAddressSDNode *GSDN = cast<GlobalAddressSDNode>(Op);
  GlobalValue *GV = GSDN->getGlobal();
  SDOperand GA = DAG.getTargetGlobalAddress(GV, PtrVT, GSDN->getOffset());
  // If it's a debug information descriptor, don't mess with it.
  if (DAG.isVerifiedDebugInfoDesc(Op))
    return GA;
  SDOperand Zero = DAG.getConstant(0, PtrVT);
  
  const TargetMachine &TM = DAG.getTarget();

  SDOperand Hi = DAG.getNode(PPCISD::Hi, PtrVT, GA, Zero);
  SDOperand Lo = DAG.getNode(PPCISD::Lo, PtrVT, GA, Zero);

  // If this is a non-darwin platform, we don't support non-static relo models
  // yet.
  if (TM.getRelocationModel() == Reloc::Static ||
      !TM.getSubtarget<PPCSubtarget>().isDarwin()) {
    // Generate non-pic code that has direct accesses to globals.
    // The address of the global is just (hi(&g)+lo(&g)).
    return DAG.getNode(ISD::ADD, PtrVT, Hi, Lo);
  }
  
  if (TM.getRelocationModel() == Reloc::PIC_) {
    // With PIC, the first instruction is actually "GR+hi(&G)".
    Hi = DAG.getNode(ISD::ADD, PtrVT,
                     DAG.getNode(PPCISD::GlobalBaseReg, PtrVT), Hi);
  }
  
  Lo = DAG.getNode(ISD::ADD, PtrVT, Hi, Lo);
  
  if (!TM.getSubtarget<PPCSubtarget>().hasLazyResolverStub(GV))
    return Lo;
  
  // If the global is weak or external, we have to go through the lazy
  // resolution stub.
  return DAG.getLoad(PtrVT, DAG.getEntryNode(), Lo, NULL, 0);
}

SDOperand PPCTargetLowering::LowerSETCC(SDOperand Op, SelectionDAG &DAG) {
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();
  
  // If we're comparing for equality to zero, expose the fact that this is
  // implented as a ctlz/srl pair on ppc, so that the dag combiner can
  // fold the new nodes.
  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op.getOperand(1))) {
    if (C->isNullValue() && CC == ISD::SETEQ) {
      MVT::ValueType VT = Op.getOperand(0).getValueType();
      SDOperand Zext = Op.getOperand(0);
      if (VT < MVT::i32) {
        VT = MVT::i32;
        Zext = DAG.getNode(ISD::ZERO_EXTEND, VT, Op.getOperand(0));
      } 
      unsigned Log2b = Log2_32(MVT::getSizeInBits(VT));
      SDOperand Clz = DAG.getNode(ISD::CTLZ, VT, Zext);
      SDOperand Scc = DAG.getNode(ISD::SRL, VT, Clz,
                                  DAG.getConstant(Log2b, MVT::i32));
      return DAG.getNode(ISD::TRUNCATE, MVT::i32, Scc);
    }
    // Leave comparisons against 0 and -1 alone for now, since they're usually 
    // optimized.  FIXME: revisit this when we can custom lower all setcc
    // optimizations.
    if (C->isAllOnesValue() || C->isNullValue())
      return SDOperand();
  }
  
  // If we have an integer seteq/setne, turn it into a compare against zero
  // by xor'ing the rhs with the lhs, which is faster than setting a
  // condition register, reading it back out, and masking the correct bit.  The
  // normal approach here uses sub to do this instead of xor.  Using xor exposes
  // the result to other bit-twiddling opportunities.
  MVT::ValueType LHSVT = Op.getOperand(0).getValueType();
  if (MVT::isInteger(LHSVT) && (CC == ISD::SETEQ || CC == ISD::SETNE)) {
    MVT::ValueType VT = Op.getValueType();
    SDOperand Sub = DAG.getNode(ISD::XOR, LHSVT, Op.getOperand(0), 
                                Op.getOperand(1));
    return DAG.getSetCC(VT, Sub, DAG.getConstant(0, LHSVT), CC);
  }
  return SDOperand();
}

SDOperand PPCTargetLowering::LowerVAARG(SDOperand Op, SelectionDAG &DAG,
                              int VarArgsFrameIndex,
                              int VarArgsStackOffset,
                              unsigned VarArgsNumGPR,
                              unsigned VarArgsNumFPR,
                              const PPCSubtarget &Subtarget) {
  
  assert(0 && "VAARG in ELF32 ABI not implemented yet!");
  return SDOperand(); // Not reached
}

SDOperand PPCTargetLowering::LowerVASTART(SDOperand Op, SelectionDAG &DAG,
                              int VarArgsFrameIndex,
                              int VarArgsStackOffset,
                              unsigned VarArgsNumGPR,
                              unsigned VarArgsNumFPR,
                              const PPCSubtarget &Subtarget) {

  if (Subtarget.isMachoABI()) {
    // vastart just stores the address of the VarArgsFrameIndex slot into the
    // memory location argument.
    MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();
    SDOperand FR = DAG.getFrameIndex(VarArgsFrameIndex, PtrVT);
    const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
    return DAG.getStore(Op.getOperand(0), FR, Op.getOperand(1), SV, 0);
  }

  // For ELF 32 ABI we follow the layout of the va_list struct.
  // We suppose the given va_list is already allocated.
  //
  // typedef struct {
  //  char gpr;     /* index into the array of 8 GPRs
  //                 * stored in the register save area
  //                 * gpr=0 corresponds to r3,
  //                 * gpr=1 to r4, etc.
  //                 */
  //  char fpr;     /* index into the array of 8 FPRs
  //                 * stored in the register save area
  //                 * fpr=0 corresponds to f1,
  //                 * fpr=1 to f2, etc.
  //                 */
  //  char *overflow_arg_area;
  //                /* location on stack that holds
  //                 * the next overflow argument
  //                 */
  //  char *reg_save_area;
  //               /* where r3:r10 and f1:f8 (if saved)
  //                * are stored
  //                */
  // } va_list[1];


  SDOperand ArgGPR = DAG.getConstant(VarArgsNumGPR, MVT::i8);
  SDOperand ArgFPR = DAG.getConstant(VarArgsNumFPR, MVT::i8);
  

  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();
  
  SDOperand StackOffsetFI = DAG.getFrameIndex(VarArgsStackOffset, PtrVT);
  SDOperand FR = DAG.getFrameIndex(VarArgsFrameIndex, PtrVT);
  
  uint64_t FrameOffset = MVT::getSizeInBits(PtrVT)/8;
  SDOperand ConstFrameOffset = DAG.getConstant(FrameOffset, PtrVT);

  uint64_t StackOffset = MVT::getSizeInBits(PtrVT)/8 - 1;
  SDOperand ConstStackOffset = DAG.getConstant(StackOffset, PtrVT);

  uint64_t FPROffset = 1;
  SDOperand ConstFPROffset = DAG.getConstant(FPROffset, PtrVT);
  
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  
  // Store first byte : number of int regs
  SDOperand firstStore = DAG.getStore(Op.getOperand(0), ArgGPR,
                                      Op.getOperand(1), SV, 0);
  uint64_t nextOffset = FPROffset;
  SDOperand nextPtr = DAG.getNode(ISD::ADD, PtrVT, Op.getOperand(1),
                                  ConstFPROffset);
  
  // Store second byte : number of float regs
  SDOperand secondStore =
    DAG.getStore(firstStore, ArgFPR, nextPtr, SV, nextOffset);
  nextOffset += StackOffset;
  nextPtr = DAG.getNode(ISD::ADD, PtrVT, nextPtr, ConstStackOffset);
  
  // Store second word : arguments given on stack
  SDOperand thirdStore =
    DAG.getStore(secondStore, StackOffsetFI, nextPtr, SV, nextOffset);
  nextOffset += FrameOffset;
  nextPtr = DAG.getNode(ISD::ADD, PtrVT, nextPtr, ConstFrameOffset);

  // Store third word : arguments given in registers
  return DAG.getStore(thirdStore, FR, nextPtr, SV, nextOffset);

}

#include "PPCGenCallingConv.inc"

/// GetFPR - Get the set of FP registers that should be allocated for arguments,
/// depending on which subtarget is selected.
static const unsigned *GetFPR(const PPCSubtarget &Subtarget) {
  if (Subtarget.isMachoABI()) {
    static const unsigned FPR[] = {
      PPC::F1, PPC::F2, PPC::F3, PPC::F4, PPC::F5, PPC::F6, PPC::F7,
      PPC::F8, PPC::F9, PPC::F10, PPC::F11, PPC::F12, PPC::F13
    };
    return FPR;
  }
  
  
  static const unsigned FPR[] = {
    PPC::F1, PPC::F2, PPC::F3, PPC::F4, PPC::F5, PPC::F6, PPC::F7,
    PPC::F8
  };
  return FPR;
}

/// CalculateStackSlotSize - Calculates the size reserved for this argument on
/// the stack.
static unsigned CalculateStackSlotSize(SDOperand Arg, SDOperand Flag,
                                       bool isVarArg, unsigned PtrByteSize) {
  MVT::ValueType ArgVT = Arg.getValueType();
  ISD::ArgFlagsTy Flags = cast<ARG_FLAGSSDNode>(Flag)->getArgFlags();
  unsigned ArgSize =MVT::getSizeInBits(ArgVT)/8;
  if (Flags.isByVal())
    ArgSize = Flags.getByValSize();
  ArgSize = ((ArgSize + PtrByteSize - 1)/PtrByteSize) * PtrByteSize;

  return ArgSize;
}

SDOperand
PPCTargetLowering::LowerFORMAL_ARGUMENTS(SDOperand Op, 
                                         SelectionDAG &DAG,
                                         int &VarArgsFrameIndex,
                                         int &VarArgsStackOffset,
                                         unsigned &VarArgsNumGPR,
                                         unsigned &VarArgsNumFPR,
                                         const PPCSubtarget &Subtarget) {
  // TODO: add description of PPC stack frame format, or at least some docs.
  //
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();
  SmallVector<SDOperand, 8> ArgValues;
  SDOperand Root = Op.getOperand(0);
  bool isVarArg = cast<ConstantSDNode>(Op.getOperand(2))->getValue() != 0;
  
  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();
  bool isPPC64 = PtrVT == MVT::i64;
  bool isMachoABI = Subtarget.isMachoABI();
  bool isELF32_ABI = Subtarget.isELF32_ABI();
  // Potential tail calls could cause overwriting of argument stack slots.
  unsigned CC = MF.getFunction()->getCallingConv();
  bool isImmutable = !(PerformTailCallOpt && (CC==CallingConv::Fast));
  unsigned PtrByteSize = isPPC64 ? 8 : 4;

  unsigned ArgOffset = PPCFrameInfo::getLinkageSize(isPPC64, isMachoABI);
  // Area that is at least reserved in caller of this function.
  unsigned MinReservedArea = ArgOffset;

  static const unsigned GPR_32[] = {           // 32-bit registers.
    PPC::R3, PPC::R4, PPC::R5, PPC::R6,
    PPC::R7, PPC::R8, PPC::R9, PPC::R10,
  };
  static const unsigned GPR_64[] = {           // 64-bit registers.
    PPC::X3, PPC::X4, PPC::X5, PPC::X6,
    PPC::X7, PPC::X8, PPC::X9, PPC::X10,
  };
  
  static const unsigned *FPR = GetFPR(Subtarget);
  
  static const unsigned VR[] = {
    PPC::V2, PPC::V3, PPC::V4, PPC::V5, PPC::V6, PPC::V7, PPC::V8,
    PPC::V9, PPC::V10, PPC::V11, PPC::V12, PPC::V13
  };

  const unsigned Num_GPR_Regs = array_lengthof(GPR_32);
  const unsigned Num_FPR_Regs = isMachoABI ? 13 : 8;
  const unsigned Num_VR_Regs  = array_lengthof( VR);

  unsigned GPR_idx = 0, FPR_idx = 0, VR_idx = 0;
  
  const unsigned *GPR = isPPC64 ? GPR_64 : GPR_32;
  
  // In 32-bit non-varargs functions, the stack space for vectors is after the
  // stack space for non-vectors.  We do not use this space unless we have
  // too many vectors to fit in registers, something that only occurs in
  // constructed examples:), but we have to walk the arglist to figure 
  // that out...for the pathological case, compute VecArgOffset as the
  // start of the vector parameter area.  Computing VecArgOffset is the
  // entire point of the following loop.
  // Altivec is not mentioned in the ppc32 Elf Supplement, so I'm not trying
  // to handle Elf here.
  unsigned VecArgOffset = ArgOffset;
  if (!isVarArg && !isPPC64) {
    for (unsigned ArgNo = 0, e = Op.Val->getNumValues()-1; ArgNo != e; 
         ++ArgNo) {
      MVT::ValueType ObjectVT = Op.getValue(ArgNo).getValueType();
      unsigned ObjSize = MVT::getSizeInBits(ObjectVT)/8;
      ISD::ArgFlagsTy Flags =
        cast<ARG_FLAGSSDNode>(Op.getOperand(ArgNo+3))->getArgFlags();

      if (Flags.isByVal()) {
        // ObjSize is the true size, ArgSize rounded up to multiple of regs.
        ObjSize = Flags.getByValSize();
        unsigned ArgSize = 
                ((ObjSize + PtrByteSize - 1)/PtrByteSize) * PtrByteSize;
        VecArgOffset += ArgSize;
        continue;
      }

      switch(ObjectVT) {
      default: assert(0 && "Unhandled argument type!");
      case MVT::i32:
      case MVT::f32:
        VecArgOffset += isPPC64 ? 8 : 4;
        break;
      case MVT::i64:  // PPC64
      case MVT::f64:
        VecArgOffset += 8;
        break;
      case MVT::v4f32:
      case MVT::v4i32:
      case MVT::v8i16:
      case MVT::v16i8:
        // Nothing to do, we're only looking at Nonvector args here.
        break;
      }
    }
  }
  // We've found where the vector parameter area in memory is.  Skip the
  // first 12 parameters; these don't use that memory.
  VecArgOffset = ((VecArgOffset+15)/16)*16;
  VecArgOffset += 12*16;

  // Add DAG nodes to load the arguments or copy them out of registers.  On
  // entry to a function on PPC, the arguments start after the linkage area,
  // although the first ones are often in registers.
  // 
  // In the ELF 32 ABI, GPRs and stack are double word align: an argument
  // represented with two words (long long or double) must be copied to an
  // even GPR_idx value or to an even ArgOffset value.

  SmallVector<SDOperand, 8> MemOps;
  unsigned nAltivecParamsAtEnd = 0;
  for (unsigned ArgNo = 0, e = Op.Val->getNumValues()-1; ArgNo != e; ++ArgNo) {
    SDOperand ArgVal;
    bool needsLoad = false;
    MVT::ValueType ObjectVT = Op.getValue(ArgNo).getValueType();
    unsigned ObjSize = MVT::getSizeInBits(ObjectVT)/8;
    unsigned ArgSize = ObjSize;
    ISD::ArgFlagsTy Flags =
      cast<ARG_FLAGSSDNode>(Op.getOperand(ArgNo+3))->getArgFlags();
    // See if next argument requires stack alignment in ELF
    bool Align = Flags.isSplit(); 

    unsigned CurArgOffset = ArgOffset;

    // Varargs or 64 bit Altivec parameters are padded to a 16 byte boundary.
    if (ObjectVT==MVT::v4f32 || ObjectVT==MVT::v4i32 ||
        ObjectVT==MVT::v8i16 || ObjectVT==MVT::v16i8) {
      if (isVarArg || isPPC64) {
        MinReservedArea = ((MinReservedArea+15)/16)*16;
        MinReservedArea += CalculateStackSlotSize(Op.getValue(ArgNo),
                                                  Op.getOperand(ArgNo+3),
                                                  isVarArg,
                                                  PtrByteSize);
      } else  nAltivecParamsAtEnd++;
    } else
      // Calculate min reserved area.
      MinReservedArea += CalculateStackSlotSize(Op.getValue(ArgNo),
                                                Op.getOperand(ArgNo+3),
                                                isVarArg,
                                                PtrByteSize);

    // FIXME alignment for ELF may not be right
    // FIXME the codegen can be much improved in some cases.
    // We do not have to keep everything in memory.
    if (Flags.isByVal()) {
      // ObjSize is the true size, ArgSize rounded up to multiple of registers.
      ObjSize = Flags.getByValSize();
      ArgSize = ((ObjSize + PtrByteSize - 1)/PtrByteSize) * PtrByteSize;
      // Double word align in ELF
      if (Align && isELF32_ABI) GPR_idx += (GPR_idx % 2);
      // Objects of size 1 and 2 are right justified, everything else is
      // left justified.  This means the memory address is adjusted forwards.
      if (ObjSize==1 || ObjSize==2) {
        CurArgOffset = CurArgOffset + (4 - ObjSize);
      }
      // The value of the object is its address.
      int FI = MFI->CreateFixedObject(ObjSize, CurArgOffset);
      SDOperand FIN = DAG.getFrameIndex(FI, PtrVT);
      ArgValues.push_back(FIN);
      if (ObjSize==1 || ObjSize==2) {
        if (GPR_idx != Num_GPR_Regs) {
          unsigned VReg = RegInfo.createVirtualRegister(&PPC::GPRCRegClass);
          RegInfo.addLiveIn(GPR[GPR_idx], VReg);
          SDOperand Val = DAG.getCopyFromReg(Root, VReg, PtrVT);
          SDOperand Store = DAG.getTruncStore(Val.getValue(1), Val, FIN, 
                               NULL, 0, ObjSize==1 ? MVT::i8 : MVT::i16 );
          MemOps.push_back(Store);
          ++GPR_idx;
          if (isMachoABI) ArgOffset += PtrByteSize;
        } else {
          ArgOffset += PtrByteSize;
        }
        continue;
      }
      for (unsigned j = 0; j < ArgSize; j += PtrByteSize) {
        // Store whatever pieces of the object are in registers
        // to memory.  ArgVal will be address of the beginning of
        // the object.
        if (GPR_idx != Num_GPR_Regs) {
          unsigned VReg = RegInfo.createVirtualRegister(&PPC::GPRCRegClass);
          RegInfo.addLiveIn(GPR[GPR_idx], VReg);
          int FI = MFI->CreateFixedObject(PtrByteSize, ArgOffset);
          SDOperand FIN = DAG.getFrameIndex(FI, PtrVT);
          SDOperand Val = DAG.getCopyFromReg(Root, VReg, PtrVT);
          SDOperand Store = DAG.getStore(Val.getValue(1), Val, FIN, NULL, 0);
          MemOps.push_back(Store);
          ++GPR_idx;
          if (isMachoABI) ArgOffset += PtrByteSize;
        } else {
          ArgOffset += ArgSize - (ArgOffset-CurArgOffset);
          break;
        }
      }
      continue;
    }

    switch (ObjectVT) {
    default: assert(0 && "Unhandled argument type!");
    case MVT::i32:
      if (!isPPC64) {
        // Double word align in ELF
        if (Align && isELF32_ABI) GPR_idx += (GPR_idx % 2);

        if (GPR_idx != Num_GPR_Regs) {
          unsigned VReg = RegInfo.createVirtualRegister(&PPC::GPRCRegClass);
          RegInfo.addLiveIn(GPR[GPR_idx], VReg);
          ArgVal = DAG.getCopyFromReg(Root, VReg, MVT::i32);
          ++GPR_idx;
        } else {
          needsLoad = true;
          ArgSize = PtrByteSize;
        }
        // Stack align in ELF
        if (needsLoad && Align && isELF32_ABI) 
          ArgOffset += ((ArgOffset/4) % 2) * PtrByteSize;
        // All int arguments reserve stack space in Macho ABI.
        if (isMachoABI || needsLoad) ArgOffset += PtrByteSize;
        break;
      }
      // FALLTHROUGH
    case MVT::i64:  // PPC64
      if (GPR_idx != Num_GPR_Regs) {
        unsigned VReg = RegInfo.createVirtualRegister(&PPC::G8RCRegClass);
        RegInfo.addLiveIn(GPR[GPR_idx], VReg);
        ArgVal = DAG.getCopyFromReg(Root, VReg, MVT::i64);

        if (ObjectVT == MVT::i32) {
          // PPC64 passes i8, i16, and i32 values in i64 registers. Promote
          // value to MVT::i64 and then truncate to the correct register size.
          if (Flags.isSExt())
            ArgVal = DAG.getNode(ISD::AssertSext, MVT::i64, ArgVal,
                                 DAG.getValueType(ObjectVT));
          else if (Flags.isZExt())
            ArgVal = DAG.getNode(ISD::AssertZext, MVT::i64, ArgVal,
                                 DAG.getValueType(ObjectVT));

          ArgVal = DAG.getNode(ISD::TRUNCATE, MVT::i32, ArgVal);
        }

        ++GPR_idx;
      } else {
        needsLoad = true;
      }
      // All int arguments reserve stack space in Macho ABI.
      if (isMachoABI || needsLoad) ArgOffset += 8;
      break;
      
    case MVT::f32:
    case MVT::f64:
      // Every 4 bytes of argument space consumes one of the GPRs available for
      // argument passing.
      if (GPR_idx != Num_GPR_Regs && isMachoABI) {
        ++GPR_idx;
        if (ObjSize == 8 && GPR_idx != Num_GPR_Regs && !isPPC64)
          ++GPR_idx;
      }
      if (FPR_idx != Num_FPR_Regs) {
        unsigned VReg;
        if (ObjectVT == MVT::f32)
          VReg = RegInfo.createVirtualRegister(&PPC::F4RCRegClass);
        else
          VReg = RegInfo.createVirtualRegister(&PPC::F8RCRegClass);
        RegInfo.addLiveIn(FPR[FPR_idx], VReg);
        ArgVal = DAG.getCopyFromReg(Root, VReg, ObjectVT);
        ++FPR_idx;
      } else {
        needsLoad = true;
      }
      
      // Stack align in ELF
      if (needsLoad && Align && isELF32_ABI)
        ArgOffset += ((ArgOffset/4) % 2) * PtrByteSize;
      // All FP arguments reserve stack space in Macho ABI.
      if (isMachoABI || needsLoad) ArgOffset += isPPC64 ? 8 : ObjSize;
      break;
    case MVT::v4f32:
    case MVT::v4i32:
    case MVT::v8i16:
    case MVT::v16i8:
      // Note that vector arguments in registers don't reserve stack space,
      // except in varargs functions.
      if (VR_idx != Num_VR_Regs) {
        unsigned VReg = RegInfo.createVirtualRegister(&PPC::VRRCRegClass);
        RegInfo.addLiveIn(VR[VR_idx], VReg);
        ArgVal = DAG.getCopyFromReg(Root, VReg, ObjectVT);
        if (isVarArg) {
          while ((ArgOffset % 16) != 0) {
            ArgOffset += PtrByteSize;
            if (GPR_idx != Num_GPR_Regs)
              GPR_idx++;
          }
          ArgOffset += 16;
          GPR_idx = std::min(GPR_idx+4, Num_GPR_Regs);
        }
        ++VR_idx;
      } else {
        if (!isVarArg && !isPPC64) {
          // Vectors go after all the nonvectors.
          CurArgOffset = VecArgOffset;
          VecArgOffset += 16;
        } else {
          // Vectors are aligned.
          ArgOffset = ((ArgOffset+15)/16)*16;
          CurArgOffset = ArgOffset;
          ArgOffset += 16;
        }
        needsLoad = true;
      }
      break;
    }
    
    // We need to load the argument to a virtual register if we determined above
    // that we ran out of physical registers of the appropriate type.
    if (needsLoad) {
      int FI = MFI->CreateFixedObject(ObjSize,
                                      CurArgOffset + (ArgSize - ObjSize),
                                      isImmutable);
      SDOperand FIN = DAG.getFrameIndex(FI, PtrVT);
      ArgVal = DAG.getLoad(ObjectVT, Root, FIN, NULL, 0);
    }
    
    ArgValues.push_back(ArgVal);
  }

  // Set the size that is at least reserved in caller of this function.  Tail
  // call optimized function's reserved stack space needs to be aligned so that
  // taking the difference between two stack areas will result in an aligned
  // stack.
  PPCFunctionInfo *FI = MF.getInfo<PPCFunctionInfo>();
  // Add the Altivec parameters at the end, if needed.
  if (nAltivecParamsAtEnd) {
    MinReservedArea = ((MinReservedArea+15)/16)*16;
    MinReservedArea += 16*nAltivecParamsAtEnd;
  }
  MinReservedArea =
    std::max(MinReservedArea,
             PPCFrameInfo::getMinCallFrameSize(isPPC64, isMachoABI));
  unsigned TargetAlign = DAG.getMachineFunction().getTarget().getFrameInfo()->
    getStackAlignment();
  unsigned AlignMask = TargetAlign-1;
  MinReservedArea = (MinReservedArea + AlignMask) & ~AlignMask;
  FI->setMinReservedArea(MinReservedArea);

  // If the function takes variable number of arguments, make a frame index for
  // the start of the first vararg value... for expansion of llvm.va_start.
  if (isVarArg) {
    
    int depth;
    if (isELF32_ABI) {
      VarArgsNumGPR = GPR_idx;
      VarArgsNumFPR = FPR_idx;
   
      // Make room for Num_GPR_Regs, Num_FPR_Regs and for a possible frame
      // pointer.
      depth = -(Num_GPR_Regs * MVT::getSizeInBits(PtrVT)/8 +
                Num_FPR_Regs * MVT::getSizeInBits(MVT::f64)/8 +
                MVT::getSizeInBits(PtrVT)/8);
      
      VarArgsStackOffset = MFI->CreateFixedObject(MVT::getSizeInBits(PtrVT)/8,
                                                  ArgOffset);

    }
    else
      depth = ArgOffset;
    
    VarArgsFrameIndex = MFI->CreateFixedObject(MVT::getSizeInBits(PtrVT)/8,
                                               depth);
    SDOperand FIN = DAG.getFrameIndex(VarArgsFrameIndex, PtrVT);
    
    // In ELF 32 ABI, the fixed integer arguments of a variadic function are
    // stored to the VarArgsFrameIndex on the stack.
    if (isELF32_ABI) {
      for (GPR_idx = 0; GPR_idx != VarArgsNumGPR; ++GPR_idx) {
        SDOperand Val = DAG.getRegister(GPR[GPR_idx], PtrVT);
        SDOperand Store = DAG.getStore(Root, Val, FIN, NULL, 0);
        MemOps.push_back(Store);
        // Increment the address by four for the next argument to store
        SDOperand PtrOff = DAG.getConstant(MVT::getSizeInBits(PtrVT)/8, PtrVT);
        FIN = DAG.getNode(ISD::ADD, PtrOff.getValueType(), FIN, PtrOff);
      }
    }

    // If this function is vararg, store any remaining integer argument regs
    // to their spots on the stack so that they may be loaded by deferencing the
    // result of va_next.
    for (; GPR_idx != Num_GPR_Regs; ++GPR_idx) {
      unsigned VReg;
      if (isPPC64)
        VReg = RegInfo.createVirtualRegister(&PPC::G8RCRegClass);
      else
        VReg = RegInfo.createVirtualRegister(&PPC::GPRCRegClass);

      RegInfo.addLiveIn(GPR[GPR_idx], VReg);
      SDOperand Val = DAG.getCopyFromReg(Root, VReg, PtrVT);
      SDOperand Store = DAG.getStore(Val.getValue(1), Val, FIN, NULL, 0);
      MemOps.push_back(Store);
      // Increment the address by four for the next argument to store
      SDOperand PtrOff = DAG.getConstant(MVT::getSizeInBits(PtrVT)/8, PtrVT);
      FIN = DAG.getNode(ISD::ADD, PtrOff.getValueType(), FIN, PtrOff);
    }

    // In ELF 32 ABI, the double arguments are stored to the VarArgsFrameIndex
    // on the stack.
    if (isELF32_ABI) {
      for (FPR_idx = 0; FPR_idx != VarArgsNumFPR; ++FPR_idx) {
        SDOperand Val = DAG.getRegister(FPR[FPR_idx], MVT::f64);
        SDOperand Store = DAG.getStore(Root, Val, FIN, NULL, 0);
        MemOps.push_back(Store);
        // Increment the address by eight for the next argument to store
        SDOperand PtrOff = DAG.getConstant(MVT::getSizeInBits(MVT::f64)/8,
                                           PtrVT);
        FIN = DAG.getNode(ISD::ADD, PtrOff.getValueType(), FIN, PtrOff);
      }

      for (; FPR_idx != Num_FPR_Regs; ++FPR_idx) {
        unsigned VReg;
        VReg = RegInfo.createVirtualRegister(&PPC::F8RCRegClass);

        RegInfo.addLiveIn(FPR[FPR_idx], VReg);
        SDOperand Val = DAG.getCopyFromReg(Root, VReg, MVT::f64);
        SDOperand Store = DAG.getStore(Val.getValue(1), Val, FIN, NULL, 0);
        MemOps.push_back(Store);
        // Increment the address by eight for the next argument to store
        SDOperand PtrOff = DAG.getConstant(MVT::getSizeInBits(MVT::f64)/8,
                                           PtrVT);
        FIN = DAG.getNode(ISD::ADD, PtrOff.getValueType(), FIN, PtrOff);
      }
    }
  }
  
  if (!MemOps.empty())
    Root = DAG.getNode(ISD::TokenFactor, MVT::Other,&MemOps[0],MemOps.size());

  ArgValues.push_back(Root);
 
  // Return the new list of results.
  std::vector<MVT::ValueType> RetVT(Op.Val->value_begin(),
                                    Op.Val->value_end());
  return DAG.getNode(ISD::MERGE_VALUES, RetVT, &ArgValues[0], ArgValues.size());
}

/// CalculateParameterAndLinkageAreaSize - Get the size of the paramter plus
/// linkage area.
static unsigned
CalculateParameterAndLinkageAreaSize(SelectionDAG &DAG,
                                     bool isPPC64,
                                     bool isMachoABI,
                                     bool isVarArg,
                                     unsigned CC,
                                     SDOperand Call,
                                     unsigned &nAltivecParamsAtEnd) {
  // Count how many bytes are to be pushed on the stack, including the linkage
  // area, and parameter passing area.  We start with 24/48 bytes, which is
  // prereserved space for [SP][CR][LR][3 x unused].
  unsigned NumBytes = PPCFrameInfo::getLinkageSize(isPPC64, isMachoABI);
  unsigned NumOps = (Call.getNumOperands() - 5) / 2;
  unsigned PtrByteSize = isPPC64 ? 8 : 4;

  // Add up all the space actually used.
  // In 32-bit non-varargs calls, Altivec parameters all go at the end; usually
  // they all go in registers, but we must reserve stack space for them for
  // possible use by the caller.  In varargs or 64-bit calls, parameters are
  // assigned stack space in order, with padding so Altivec parameters are
  // 16-byte aligned.
  nAltivecParamsAtEnd = 0;
  for (unsigned i = 0; i != NumOps; ++i) {
    SDOperand Arg = Call.getOperand(5+2*i);
    SDOperand Flag = Call.getOperand(5+2*i+1);
    MVT::ValueType ArgVT = Arg.getValueType();
    // Varargs Altivec parameters are padded to a 16 byte boundary.
    if (ArgVT==MVT::v4f32 || ArgVT==MVT::v4i32 ||
        ArgVT==MVT::v8i16 || ArgVT==MVT::v16i8) {
      if (!isVarArg && !isPPC64) {
        // Non-varargs Altivec parameters go after all the non-Altivec
        // parameters; handle those later so we know how much padding we need.
        nAltivecParamsAtEnd++;
        continue;
      }
      // Varargs and 64-bit Altivec parameters are padded to 16 byte boundary.
      NumBytes = ((NumBytes+15)/16)*16;
    }
    NumBytes += CalculateStackSlotSize(Arg, Flag, isVarArg, PtrByteSize);
  }

   // Allow for Altivec parameters at the end, if needed.
  if (nAltivecParamsAtEnd) {
    NumBytes = ((NumBytes+15)/16)*16;
    NumBytes += 16*nAltivecParamsAtEnd;
  }

  // The prolog code of the callee may store up to 8 GPR argument registers to
  // the stack, allowing va_start to index over them in memory if its varargs.
  // Because we cannot tell if this is needed on the caller side, we have to
  // conservatively assume that it is needed.  As such, make sure we have at
  // least enough stack space for the caller to store the 8 GPRs.
  NumBytes = std::max(NumBytes,
                      PPCFrameInfo::getMinCallFrameSize(isPPC64, isMachoABI));

  // Tail call needs the stack to be aligned.
  if (CC==CallingConv::Fast && PerformTailCallOpt) {
    unsigned TargetAlign = DAG.getMachineFunction().getTarget().getFrameInfo()->
      getStackAlignment();
    unsigned AlignMask = TargetAlign-1;
    NumBytes = (NumBytes + AlignMask) & ~AlignMask;
  }

  return NumBytes;
}

/// CalculateTailCallSPDiff - Get the amount the stack pointer has to be
/// adjusted to accomodate the arguments for the tailcall.
static int CalculateTailCallSPDiff(SelectionDAG& DAG, bool IsTailCall,
                                   unsigned ParamSize) {

  if (!IsTailCall) return 0;

  PPCFunctionInfo *FI = DAG.getMachineFunction().getInfo<PPCFunctionInfo>();
  unsigned CallerMinReservedArea = FI->getMinReservedArea();
  int SPDiff = (int)CallerMinReservedArea - (int)ParamSize;
  // Remember only if the new adjustement is bigger.
  if (SPDiff < FI->getTailCallSPDelta())
    FI->setTailCallSPDelta(SPDiff);

  return SPDiff;
}

/// IsEligibleForTailCallElimination - Check to see whether the next instruction
/// following the call is a return. A function is eligible if caller/callee
/// calling conventions match, currently only fastcc supports tail calls, and
/// the function CALL is immediatly followed by a RET.
bool
PPCTargetLowering::IsEligibleForTailCallOptimization(SDOperand Call,
                                                     SDOperand Ret,
                                                     SelectionDAG& DAG) const {
  // Variable argument functions are not supported.
  if (!PerformTailCallOpt ||
      cast<ConstantSDNode>(Call.getOperand(2))->getValue() != 0) return false;

  if (CheckTailCallReturnConstraints(Call, Ret)) {
    MachineFunction &MF = DAG.getMachineFunction();
    unsigned CallerCC = MF.getFunction()->getCallingConv();
    unsigned CalleeCC = cast<ConstantSDNode>(Call.getOperand(1))->getValue();
    if (CalleeCC == CallingConv::Fast && CallerCC == CalleeCC) {
      // Functions containing by val parameters are not supported.
      for (unsigned i = 0; i != ((Call.getNumOperands()-5)/2); i++) {
         ISD::ArgFlagsTy Flags = cast<ARG_FLAGSSDNode>(Call.getOperand(5+2*i+1))
           ->getArgFlags();
         if (Flags.isByVal()) return false;
      }

      SDOperand Callee = Call.getOperand(4);
      // Non PIC/GOT  tail calls are supported.
      if (getTargetMachine().getRelocationModel() != Reloc::PIC_)
        return true;

      // At the moment we can only do local tail calls (in same module, hidden
      // or protected) if we are generating PIC.
      if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee))
        return G->getGlobal()->hasHiddenVisibility()
            || G->getGlobal()->hasProtectedVisibility();
    }
  }

  return false;
}

/// isCallCompatibleAddress - Return the immediate to use if the specified
/// 32-bit value is representable in the immediate field of a BxA instruction.
static SDNode *isBLACompatibleAddress(SDOperand Op, SelectionDAG &DAG) {
  ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op);
  if (!C) return 0;
  
  int Addr = C->getValue();
  if ((Addr & 3) != 0 ||  // Low 2 bits are implicitly zero.
      (Addr << 6 >> 6) != Addr)
    return 0;  // Top 6 bits have to be sext of immediate.
  
  return DAG.getConstant((int)C->getValue() >> 2,
                         DAG.getTargetLoweringInfo().getPointerTy()).Val;
}

struct TailCallArgumentInfo {
  SDOperand Arg;
  SDOperand FrameIdxOp;
  int       FrameIdx;

  TailCallArgumentInfo() : FrameIdx(0) {}
};

/// StoreTailCallArgumentsToStackSlot - Stores arguments to their stack slot.
static void
StoreTailCallArgumentsToStackSlot(SelectionDAG &DAG,
                                           SDOperand Chain,
                   const SmallVector<TailCallArgumentInfo, 8> &TailCallArgs,
                   SmallVector<SDOperand, 8> &MemOpChains) {
  for (unsigned i = 0, e = TailCallArgs.size(); i != e; ++i) {
    SDOperand Arg = TailCallArgs[i].Arg;
    SDOperand FIN = TailCallArgs[i].FrameIdxOp;
    int FI = TailCallArgs[i].FrameIdx;
    // Store relative to framepointer.
    MemOpChains.push_back(DAG.getStore(Chain, Arg, FIN,
                                       PseudoSourceValue::getFixedStack(),
                                       FI));
  }
}

/// EmitTailCallStoreFPAndRetAddr - Move the frame pointer and return address to
/// the appropriate stack slot for the tail call optimized function call.
static SDOperand EmitTailCallStoreFPAndRetAddr(SelectionDAG &DAG,
                                               MachineFunction &MF,
                                               SDOperand Chain,
                                               SDOperand OldRetAddr,
                                               SDOperand OldFP,
                                               int SPDiff,
                                               bool isPPC64,
                                               bool isMachoABI) {
  if (SPDiff) {
    // Calculate the new stack slot for the return address.
    int SlotSize = isPPC64 ? 8 : 4;
    int NewRetAddrLoc = SPDiff + PPCFrameInfo::getReturnSaveOffset(isPPC64,
                                                                   isMachoABI);
    int NewRetAddr = MF.getFrameInfo()->CreateFixedObject(SlotSize,
                                                          NewRetAddrLoc);
    int NewFPLoc = SPDiff + PPCFrameInfo::getFramePointerSaveOffset(isPPC64,
                                                                    isMachoABI);
    int NewFPIdx = MF.getFrameInfo()->CreateFixedObject(SlotSize, NewFPLoc);

    MVT::ValueType VT = isPPC64 ? MVT::i64 : MVT::i32;
    SDOperand NewRetAddrFrIdx = DAG.getFrameIndex(NewRetAddr, VT);
    Chain = DAG.getStore(Chain, OldRetAddr, NewRetAddrFrIdx,
                         PseudoSourceValue::getFixedStack(), NewRetAddr);
    SDOperand NewFramePtrIdx = DAG.getFrameIndex(NewFPIdx, VT);
    Chain = DAG.getStore(Chain, OldFP, NewFramePtrIdx,
                         PseudoSourceValue::getFixedStack(), NewFPIdx);
  }
  return Chain;
}

/// CalculateTailCallArgDest - Remember Argument for later processing. Calculate
/// the position of the argument.
static void
CalculateTailCallArgDest(SelectionDAG &DAG, MachineFunction &MF, bool isPPC64,
                         SDOperand Arg, int SPDiff, unsigned ArgOffset,
                      SmallVector<TailCallArgumentInfo, 8>& TailCallArguments) {
  int Offset = ArgOffset + SPDiff;
  uint32_t OpSize = (MVT::getSizeInBits(Arg.getValueType())+7)/8;
  int FI = MF.getFrameInfo()->CreateFixedObject(OpSize, Offset);
  MVT::ValueType VT = isPPC64 ? MVT::i64 : MVT::i32;
  SDOperand FIN = DAG.getFrameIndex(FI, VT);
  TailCallArgumentInfo Info;
  Info.Arg = Arg;
  Info.FrameIdxOp = FIN;
  Info.FrameIdx = FI;
  TailCallArguments.push_back(Info);
}

/// EmitTCFPAndRetAddrLoad - Emit load from frame pointer and return address
/// stack slot. Returns the chain as result and the loaded frame pointers in
/// LROpOut/FPOpout. Used when tail calling.
SDOperand PPCTargetLowering::EmitTailCallLoadFPAndRetAddr(SelectionDAG & DAG,
                                                          int SPDiff,
                                                          SDOperand Chain,
                                                          SDOperand &LROpOut,
                                                          SDOperand &FPOpOut) {
  if (SPDiff) {
    // Load the LR and FP stack slot for later adjusting.
    MVT::ValueType VT = PPCSubTarget.isPPC64() ? MVT::i64 : MVT::i32;
    LROpOut = getReturnAddrFrameIndex(DAG);
    LROpOut = DAG.getLoad(VT, Chain, LROpOut, NULL, 0);
    Chain = SDOperand(LROpOut.Val, 1);
    FPOpOut = getFramePointerFrameIndex(DAG);
    FPOpOut = DAG.getLoad(VT, Chain, FPOpOut, NULL, 0);
    Chain = SDOperand(FPOpOut.Val, 1);
  }
  return Chain;
}

/// CreateCopyOfByValArgument - Make a copy of an aggregate at address specified
/// by "Src" to address "Dst" of size "Size".  Alignment information is 
/// specified by the specific parameter attribute. The copy will be passed as
/// a byval function parameter.
/// Sometimes what we are copying is the end of a larger object, the part that
/// does not fit in registers.
static SDOperand 
CreateCopyOfByValArgument(SDOperand Src, SDOperand Dst, SDOperand Chain,
                          ISD::ArgFlagsTy Flags, SelectionDAG &DAG,
                          unsigned Size) {
  SDOperand SizeNode = DAG.getConstant(Size, MVT::i32);
  return DAG.getMemcpy(Chain, Dst, Src, SizeNode, Flags.getByValAlign(), false,
                       NULL, 0, NULL, 0);
}

/// LowerMemOpCallTo - Store the argument to the stack or remember it in case of
/// tail calls.
static void
LowerMemOpCallTo(SelectionDAG &DAG, MachineFunction &MF, SDOperand Chain,
                 SDOperand Arg, SDOperand PtrOff, int SPDiff,
                 unsigned ArgOffset, bool isPPC64, bool isTailCall,
                 bool isVector, SmallVector<SDOperand, 8> &MemOpChains,
                 SmallVector<TailCallArgumentInfo, 8>& TailCallArguments) {
  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();
  if (!isTailCall) {
    if (isVector) {
      SDOperand StackPtr;
      if (isPPC64)
        StackPtr = DAG.getRegister(PPC::X1, MVT::i64);
      else
        StackPtr = DAG.getRegister(PPC::R1, MVT::i32);
      PtrOff = DAG.getNode(ISD::ADD, PtrVT, StackPtr,
                           DAG.getConstant(ArgOffset, PtrVT));
    }
    MemOpChains.push_back(DAG.getStore(Chain, Arg, PtrOff, NULL, 0));
  // Calculate and remember argument location.
  } else CalculateTailCallArgDest(DAG, MF, isPPC64, Arg, SPDiff, ArgOffset,
                                  TailCallArguments);
}

SDOperand PPCTargetLowering::LowerCALL(SDOperand Op, SelectionDAG &DAG,
                                       const PPCSubtarget &Subtarget,
                                       TargetMachine &TM) {
  SDOperand Chain  = Op.getOperand(0);
  bool isVarArg    = cast<ConstantSDNode>(Op.getOperand(2))->getValue() != 0;
  unsigned CC      = cast<ConstantSDNode>(Op.getOperand(1))->getValue();
  bool isTailCall  = cast<ConstantSDNode>(Op.getOperand(3))->getValue() != 0 &&
                     CC == CallingConv::Fast && PerformTailCallOpt;
  SDOperand Callee = Op.getOperand(4);
  unsigned NumOps  = (Op.getNumOperands() - 5) / 2;
  
  bool isMachoABI = Subtarget.isMachoABI();
  bool isELF32_ABI  = Subtarget.isELF32_ABI();

  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();
  bool isPPC64 = PtrVT == MVT::i64;
  unsigned PtrByteSize = isPPC64 ? 8 : 4;
  
  MachineFunction &MF = DAG.getMachineFunction();

  // args_to_use will accumulate outgoing args for the PPCISD::CALL case in
  // SelectExpr to use to put the arguments in the appropriate registers.
  std::vector<SDOperand> args_to_use;
  
  // Mark this function as potentially containing a function that contains a
  // tail call. As a consequence the frame pointer will be used for dynamicalloc
  // and restoring the callers stack pointer in this functions epilog. This is
  // done because by tail calling the called function might overwrite the value
  // in this function's (MF) stack pointer stack slot 0(SP).
  if (PerformTailCallOpt && CC==CallingConv::Fast)
    MF.getInfo<PPCFunctionInfo>()->setHasFastCall();

  unsigned nAltivecParamsAtEnd = 0;

  // Count how many bytes are to be pushed on the stack, including the linkage
  // area, and parameter passing area.  We start with 24/48 bytes, which is
  // prereserved space for [SP][CR][LR][3 x unused].
  unsigned NumBytes =
    CalculateParameterAndLinkageAreaSize(DAG, isPPC64, isMachoABI, isVarArg, CC,
                                         Op, nAltivecParamsAtEnd);

  // Calculate by how many bytes the stack has to be adjusted in case of tail
  // call optimization.
  int SPDiff = CalculateTailCallSPDiff(DAG, isTailCall, NumBytes);
  
  // Adjust the stack pointer for the new arguments...
  // These operations are automatically eliminated by the prolog/epilog pass
  Chain = DAG.getCALLSEQ_START(Chain,
                               DAG.getConstant(NumBytes, PtrVT));
  SDOperand CallSeqStart = Chain;
  
  // Load the return address and frame pointer so it can be move somewhere else
  // later.
  SDOperand LROp, FPOp;
  Chain = EmitTailCallLoadFPAndRetAddr(DAG, SPDiff, Chain, LROp, FPOp);

  // Set up a copy of the stack pointer for use loading and storing any
  // arguments that may not fit in the registers available for argument
  // passing.
  SDOperand StackPtr;
  if (isPPC64)
    StackPtr = DAG.getRegister(PPC::X1, MVT::i64);
  else
    StackPtr = DAG.getRegister(PPC::R1, MVT::i32);
  
  // Figure out which arguments are going to go in registers, and which in
  // memory.  Also, if this is a vararg function, floating point operations
  // must be stored to our stack, and loaded into integer regs as well, if
  // any integer regs are available for argument passing.
  unsigned ArgOffset = PPCFrameInfo::getLinkageSize(isPPC64, isMachoABI);
  unsigned GPR_idx = 0, FPR_idx = 0, VR_idx = 0;
  
  static const unsigned GPR_32[] = {           // 32-bit registers.
    PPC::R3, PPC::R4, PPC::R5, PPC::R6,
    PPC::R7, PPC::R8, PPC::R9, PPC::R10,
  };
  static const unsigned GPR_64[] = {           // 64-bit registers.
    PPC::X3, PPC::X4, PPC::X5, PPC::X6,
    PPC::X7, PPC::X8, PPC::X9, PPC::X10,
  };
  static const unsigned *FPR = GetFPR(Subtarget);
  
  static const unsigned VR[] = {
    PPC::V2, PPC::V3, PPC::V4, PPC::V5, PPC::V6, PPC::V7, PPC::V8,
    PPC::V9, PPC::V10, PPC::V11, PPC::V12, PPC::V13
  };
  const unsigned NumGPRs = array_lengthof(GPR_32);
  const unsigned NumFPRs = isMachoABI ? 13 : 8;
  const unsigned NumVRs  = array_lengthof( VR);
  
  const unsigned *GPR = isPPC64 ? GPR_64 : GPR_32;

  std::vector<std::pair<unsigned, SDOperand> > RegsToPass;
  SmallVector<TailCallArgumentInfo, 8> TailCallArguments;

  SmallVector<SDOperand, 8> MemOpChains;
  for (unsigned i = 0; i != NumOps; ++i) {
    bool inMem = false;
    SDOperand Arg = Op.getOperand(5+2*i);
    ISD::ArgFlagsTy Flags =
      cast<ARG_FLAGSSDNode>(Op.getOperand(5+2*i+1))->getArgFlags();
    // See if next argument requires stack alignment in ELF
    bool Align = Flags.isSplit();

    // PtrOff will be used to store the current argument to the stack if a
    // register cannot be found for it.
    SDOperand PtrOff;
    
    // Stack align in ELF 32
    if (isELF32_ABI && Align)
      PtrOff = DAG.getConstant(ArgOffset + ((ArgOffset/4) % 2) * PtrByteSize,
                               StackPtr.getValueType());
    else
      PtrOff = DAG.getConstant(ArgOffset, StackPtr.getValueType());

    PtrOff = DAG.getNode(ISD::ADD, PtrVT, StackPtr, PtrOff);

    // On PPC64, promote integers to 64-bit values.
    if (isPPC64 && Arg.getValueType() == MVT::i32) {
      // FIXME: Should this use ANY_EXTEND if neither sext nor zext?
      unsigned ExtOp = Flags.isSExt() ? ISD::SIGN_EXTEND : ISD::ZERO_EXTEND;
      Arg = DAG.getNode(ExtOp, MVT::i64, Arg);
    }

    // FIXME Elf untested, what are alignment rules?
    // FIXME memcpy is used way more than necessary.  Correctness first.
    if (Flags.isByVal()) {
      unsigned Size = Flags.getByValSize();
      if (isELF32_ABI && Align) GPR_idx += (GPR_idx % 2);
      if (Size==1 || Size==2) {
        // Very small objects are passed right-justified.
        // Everything else is passed left-justified.
        MVT::ValueType VT = (Size==1) ? MVT::i8 : MVT::i16;
        if (GPR_idx != NumGPRs) {
          SDOperand Load = DAG.getExtLoad(ISD::EXTLOAD, PtrVT, Chain, Arg, 
                                          NULL, 0, VT);
          MemOpChains.push_back(Load.getValue(1));
          RegsToPass.push_back(std::make_pair(GPR[GPR_idx++], Load));
          if (isMachoABI)
            ArgOffset += PtrByteSize;
        } else {
          SDOperand Const = DAG.getConstant(4 - Size, PtrOff.getValueType());
          SDOperand AddPtr = DAG.getNode(ISD::ADD, PtrVT, PtrOff, Const);
          SDOperand MemcpyCall = CreateCopyOfByValArgument(Arg, AddPtr,
                                CallSeqStart.Val->getOperand(0), 
                                Flags, DAG, Size);
          // This must go outside the CALLSEQ_START..END.
          SDOperand NewCallSeqStart = DAG.getCALLSEQ_START(MemcpyCall,
                               CallSeqStart.Val->getOperand(1));
          DAG.ReplaceAllUsesWith(CallSeqStart.Val, NewCallSeqStart.Val);
          Chain = CallSeqStart = NewCallSeqStart;
          ArgOffset += PtrByteSize;
        }
        continue;
      }
      // Copy entire object into memory.  There are cases where gcc-generated
      // code assumes it is there, even if it could be put entirely into
      // registers.  (This is not what the doc says.)
      SDOperand MemcpyCall = CreateCopyOfByValArgument(Arg, PtrOff,
                            CallSeqStart.Val->getOperand(0), 
                            Flags, DAG, Size);
      // This must go outside the CALLSEQ_START..END.
      SDOperand NewCallSeqStart = DAG.getCALLSEQ_START(MemcpyCall,
                           CallSeqStart.Val->getOperand(1));
      DAG.ReplaceAllUsesWith(CallSeqStart.Val, NewCallSeqStart.Val);
      Chain = CallSeqStart = NewCallSeqStart;
      // And copy the pieces of it that fit into registers.
      for (unsigned j=0; j<Size; j+=PtrByteSize) {
        SDOperand Const = DAG.getConstant(j, PtrOff.getValueType());
        SDOperand AddArg = DAG.getNode(ISD::ADD, PtrVT, Arg, Const);
        if (GPR_idx != NumGPRs) {
          SDOperand Load = DAG.getLoad(PtrVT, Chain, AddArg, NULL, 0);
          MemOpChains.push_back(Load.getValue(1));
          RegsToPass.push_back(std::make_pair(GPR[GPR_idx++], Load));
          if (isMachoABI)
            ArgOffset += PtrByteSize;
        } else {
          ArgOffset += ((Size - j + PtrByteSize-1)/PtrByteSize)*PtrByteSize;
          break;
        }
      }
      continue;
    }

    switch (Arg.getValueType()) {
    default: assert(0 && "Unexpected ValueType for argument!");
    case MVT::i32:
    case MVT::i64:
      // Double word align in ELF
      if (isELF32_ABI && Align) GPR_idx += (GPR_idx % 2);
      if (GPR_idx != NumGPRs) {
        RegsToPass.push_back(std::make_pair(GPR[GPR_idx++], Arg));
      } else {
        LowerMemOpCallTo(DAG, MF, Chain, Arg, PtrOff, SPDiff, ArgOffset,
                         isPPC64, isTailCall, false, MemOpChains,
                         TailCallArguments);
        inMem = true;
      }
      if (inMem || isMachoABI) {
        // Stack align in ELF
        if (isELF32_ABI && Align)
          ArgOffset += ((ArgOffset/4) % 2) * PtrByteSize;

        ArgOffset += PtrByteSize;
      }
      break;
    case MVT::f32:
    case MVT::f64:
      if (FPR_idx != NumFPRs) {
        RegsToPass.push_back(std::make_pair(FPR[FPR_idx++], Arg));

        if (isVarArg) {
          SDOperand Store = DAG.getStore(Chain, Arg, PtrOff, NULL, 0);
          MemOpChains.push_back(Store);

          // Float varargs are always shadowed in available integer registers
          if (GPR_idx != NumGPRs) {
            SDOperand Load = DAG.getLoad(PtrVT, Store, PtrOff, NULL, 0);
            MemOpChains.push_back(Load.getValue(1));
            if (isMachoABI) RegsToPass.push_back(std::make_pair(GPR[GPR_idx++],
                                                                Load));
          }
          if (GPR_idx != NumGPRs && Arg.getValueType() == MVT::f64 && !isPPC64){
            SDOperand ConstFour = DAG.getConstant(4, PtrOff.getValueType());
            PtrOff = DAG.getNode(ISD::ADD, PtrVT, PtrOff, ConstFour);
            SDOperand Load = DAG.getLoad(PtrVT, Store, PtrOff, NULL, 0);
            MemOpChains.push_back(Load.getValue(1));
            if (isMachoABI) RegsToPass.push_back(std::make_pair(GPR[GPR_idx++],
                                                                Load));
          }
        } else {
          // If we have any FPRs remaining, we may also have GPRs remaining.
          // Args passed in FPRs consume either 1 (f32) or 2 (f64) available
          // GPRs.
          if (isMachoABI) {
            if (GPR_idx != NumGPRs)
              ++GPR_idx;
            if (GPR_idx != NumGPRs && Arg.getValueType() == MVT::f64 &&
                !isPPC64)  // PPC64 has 64-bit GPR's obviously :)
              ++GPR_idx;
          }
        }
      } else {
        LowerMemOpCallTo(DAG, MF, Chain, Arg, PtrOff, SPDiff, ArgOffset,
                         isPPC64, isTailCall, false, MemOpChains,
                         TailCallArguments);
        inMem = true;
      }
      if (inMem || isMachoABI) {
        // Stack align in ELF
        if (isELF32_ABI && Align)
          ArgOffset += ((ArgOffset/4) % 2) * PtrByteSize;
        if (isPPC64)
          ArgOffset += 8;
        else
          ArgOffset += Arg.getValueType() == MVT::f32 ? 4 : 8;
      }
      break;
    case MVT::v4f32:
    case MVT::v4i32:
    case MVT::v8i16:
    case MVT::v16i8:
      if (isVarArg) {
        // These go aligned on the stack, or in the corresponding R registers
        // when within range.  The Darwin PPC ABI doc claims they also go in 
        // V registers; in fact gcc does this only for arguments that are
        // prototyped, not for those that match the ...  We do it for all
        // arguments, seems to work.
        while (ArgOffset % 16 !=0) {
          ArgOffset += PtrByteSize;
          if (GPR_idx != NumGPRs)
            GPR_idx++;
        }
        // We could elide this store in the case where the object fits
        // entirely in R registers.  Maybe later.
        PtrOff = DAG.getNode(ISD::ADD, PtrVT, StackPtr, 
                            DAG.getConstant(ArgOffset, PtrVT));
        SDOperand Store = DAG.getStore(Chain, Arg, PtrOff, NULL, 0);
        MemOpChains.push_back(Store);
        if (VR_idx != NumVRs) {
          SDOperand Load = DAG.getLoad(MVT::v4f32, Store, PtrOff, NULL, 0);
          MemOpChains.push_back(Load.getValue(1));
          RegsToPass.push_back(std::make_pair(VR[VR_idx++], Load));
        }
        ArgOffset += 16;
        for (unsigned i=0; i<16; i+=PtrByteSize) {
          if (GPR_idx == NumGPRs)
            break;
          SDOperand Ix = DAG.getNode(ISD::ADD, PtrVT, PtrOff,
                                  DAG.getConstant(i, PtrVT));
          SDOperand Load = DAG.getLoad(PtrVT, Store, Ix, NULL, 0);
          MemOpChains.push_back(Load.getValue(1));
          RegsToPass.push_back(std::make_pair(GPR[GPR_idx++], Load));
        }
        break;
      }

      // Non-varargs Altivec params generally go in registers, but have
      // stack space allocated at the end.
      if (VR_idx != NumVRs) {
        // Doesn't have GPR space allocated.
        RegsToPass.push_back(std::make_pair(VR[VR_idx++], Arg));
      } else if (nAltivecParamsAtEnd==0) {
        // We are emitting Altivec params in order.
        LowerMemOpCallTo(DAG, MF, Chain, Arg, PtrOff, SPDiff, ArgOffset,
                         isPPC64, isTailCall, true, MemOpChains,
                         TailCallArguments);
        ArgOffset += 16;
      }
      break;
    }
  }
  // If all Altivec parameters fit in registers, as they usually do,
  // they get stack space following the non-Altivec parameters.  We
  // don't track this here because nobody below needs it.
  // If there are more Altivec parameters than fit in registers emit
  // the stores here.
  if (!isVarArg && nAltivecParamsAtEnd > NumVRs) {
    unsigned j = 0;
    // Offset is aligned; skip 1st 12 params which go in V registers.
    ArgOffset = ((ArgOffset+15)/16)*16;
    ArgOffset += 12*16;
    for (unsigned i = 0; i != NumOps; ++i) {
      SDOperand Arg = Op.getOperand(5+2*i);
      MVT::ValueType ArgType = Arg.getValueType();
      if (ArgType==MVT::v4f32 || ArgType==MVT::v4i32 ||
          ArgType==MVT::v8i16 || ArgType==MVT::v16i8) {
        if (++j > NumVRs) {
          SDOperand PtrOff;
          // We are emitting Altivec params in order.
          LowerMemOpCallTo(DAG, MF, Chain, Arg, PtrOff, SPDiff, ArgOffset,
                           isPPC64, isTailCall, true, MemOpChains,
                           TailCallArguments);
          ArgOffset += 16;
        }
      }
    }
  }

  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, MVT::Other,
                        &MemOpChains[0], MemOpChains.size());
  
  // Build a sequence of copy-to-reg nodes chained together with token chain
  // and flag operands which copy the outgoing args into the appropriate regs.
  SDOperand InFlag;
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
    Chain = DAG.getCopyToReg(Chain, RegsToPass[i].first, RegsToPass[i].second,
                             InFlag);
    InFlag = Chain.getValue(1);
  }
 
  // With the ELF 32 ABI, set CR6 to true if this is a vararg call.
  if (isVarArg && isELF32_ABI) {
    SDOperand SetCR(DAG.getTargetNode(PPC::CRSET, MVT::i32), 0);
    Chain = DAG.getCopyToReg(Chain, PPC::CR1EQ, SetCR, InFlag);
    InFlag = Chain.getValue(1);
  }

  // Emit a sequence of copyto/copyfrom virtual registers for arguments that
  // might overwrite each other in case of tail call optimization.
  if (isTailCall) {
    SmallVector<SDOperand, 8> MemOpChains2;
    // Do not flag preceeding copytoreg stuff together with the following stuff.
    InFlag = SDOperand();
    StoreTailCallArgumentsToStackSlot(DAG, Chain, TailCallArguments,
                                      MemOpChains2);
    if (!MemOpChains2.empty())
      Chain = DAG.getNode(ISD::TokenFactor, MVT::Other,
                          &MemOpChains2[0], MemOpChains2.size());

    // Store the return address to the appropriate stack slot.
    Chain = EmitTailCallStoreFPAndRetAddr(DAG, MF, Chain, LROp, FPOp, SPDiff,
                                          isPPC64, isMachoABI);
  }

  // Emit callseq_end just before tailcall node.
  if (isTailCall) {
    SmallVector<SDOperand, 8> CallSeqOps;
    SDVTList CallSeqNodeTys = DAG.getVTList(MVT::Other, MVT::Flag);
    CallSeqOps.push_back(Chain);
    CallSeqOps.push_back(DAG.getIntPtrConstant(NumBytes));
    CallSeqOps.push_back(DAG.getIntPtrConstant(0));
    if (InFlag.Val)
      CallSeqOps.push_back(InFlag);
    Chain = DAG.getNode(ISD::CALLSEQ_END, CallSeqNodeTys, &CallSeqOps[0],
                        CallSeqOps.size());
    InFlag = Chain.getValue(1);
  }

  std::vector<MVT::ValueType> NodeTys;
  NodeTys.push_back(MVT::Other);   // Returns a chain
  NodeTys.push_back(MVT::Flag);    // Returns a flag for retval copy to use.

  SmallVector<SDOperand, 8> Ops;
  unsigned CallOpc = isMachoABI? PPCISD::CALL_Macho : PPCISD::CALL_ELF;
  
  // If the callee is a GlobalAddress/ExternalSymbol node (quite common, every
  // direct call is) turn it into a TargetGlobalAddress/TargetExternalSymbol
  // node so that legalize doesn't hack it.
  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), Callee.getValueType());
  else if (ExternalSymbolSDNode *S = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(S->getSymbol(), Callee.getValueType());
  else if (SDNode *Dest = isBLACompatibleAddress(Callee, DAG))
    // If this is an absolute destination address, use the munged value.
    Callee = SDOperand(Dest, 0);
  else {
    // Otherwise, this is an indirect call.  We have to use a MTCTR/BCTRL pair
    // to do the call, we can't use PPCISD::CALL.
    SDOperand MTCTROps[] = {Chain, Callee, InFlag};
    Chain = DAG.getNode(PPCISD::MTCTR, NodeTys, MTCTROps, 2+(InFlag.Val!=0));
    InFlag = Chain.getValue(1);
    
    // Copy the callee address into R12/X12 on darwin.
    if (isMachoABI) {
      unsigned Reg = Callee.getValueType() == MVT::i32 ? PPC::R12 : PPC::X12;
      Chain = DAG.getCopyToReg(Chain, Reg, Callee, InFlag);
      InFlag = Chain.getValue(1);
    }

    NodeTys.clear();
    NodeTys.push_back(MVT::Other);
    NodeTys.push_back(MVT::Flag);
    Ops.push_back(Chain);
    CallOpc = isMachoABI ? PPCISD::BCTRL_Macho : PPCISD::BCTRL_ELF;
    Callee.Val = 0;
    // Add CTR register as callee so a bctr can be emitted later.
    if (isTailCall)
      Ops.push_back(DAG.getRegister(PPC::CTR, getPointerTy()));
  }

  // If this is a direct call, pass the chain and the callee.
  if (Callee.Val) {
    Ops.push_back(Chain);
    Ops.push_back(Callee);
  }
  // If this is a tail call add stack pointer delta.
  if (isTailCall)
    Ops.push_back(DAG.getConstant(SPDiff, MVT::i32));

  // Add argument registers to the end of the list so that they are known live
  // into the call.
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i)
    Ops.push_back(DAG.getRegister(RegsToPass[i].first, 
                                  RegsToPass[i].second.getValueType()));

  // When performing tail call optimization the callee pops its arguments off
  // the stack. Account for this here so these bytes can be pushed back on in
  // PPCRegisterInfo::eliminateCallFramePseudoInstr.
  int BytesCalleePops =
    (CC==CallingConv::Fast && PerformTailCallOpt) ? NumBytes : 0;

  if (InFlag.Val)
    Ops.push_back(InFlag);

  // Emit tail call.
  if (isTailCall) {
    assert(InFlag.Val &&
           "Flag must be set. Depend on flag being set in LowerRET");
    Chain = DAG.getNode(PPCISD::TAILCALL,
                        Op.Val->getVTList(), &Ops[0], Ops.size());
    return SDOperand(Chain.Val, Op.ResNo);
  }

  Chain = DAG.getNode(CallOpc, NodeTys, &Ops[0], Ops.size());
  InFlag = Chain.getValue(1);

  Chain = DAG.getCALLSEQ_END(Chain,
                             DAG.getConstant(NumBytes, PtrVT),
                             DAG.getConstant(BytesCalleePops, PtrVT),
                             InFlag);
  if (Op.Val->getValueType(0) != MVT::Other)
    InFlag = Chain.getValue(1);

  SmallVector<SDOperand, 16> ResultVals;
  SmallVector<CCValAssign, 16> RVLocs;
  unsigned CallerCC = DAG.getMachineFunction().getFunction()->getCallingConv();
  CCState CCInfo(CallerCC, isVarArg, TM, RVLocs);
  CCInfo.AnalyzeCallResult(Op.Val, RetCC_PPC);
  
  // Copy all of the result registers out of their specified physreg.
  for (unsigned i = 0, e = RVLocs.size(); i != e; ++i) {
    CCValAssign &VA = RVLocs[i];
    MVT::ValueType VT = VA.getValVT();
    assert(VA.isRegLoc() && "Can only return in registers!");
    Chain = DAG.getCopyFromReg(Chain, VA.getLocReg(), VT, InFlag).getValue(1);
    ResultVals.push_back(Chain.getValue(0));
    InFlag = Chain.getValue(2);
  }

  // If the function returns void, just return the chain.
  if (RVLocs.empty())
    return Chain;
  
  // Otherwise, merge everything together with a MERGE_VALUES node.
  ResultVals.push_back(Chain);
  SDOperand Res = DAG.getNode(ISD::MERGE_VALUES, Op.Val->getVTList(),
                              &ResultVals[0], ResultVals.size());
  return Res.getValue(Op.ResNo);
}

SDOperand PPCTargetLowering::LowerRET(SDOperand Op, SelectionDAG &DAG, 
                                      TargetMachine &TM) {
  SmallVector<CCValAssign, 16> RVLocs;
  unsigned CC = DAG.getMachineFunction().getFunction()->getCallingConv();
  bool isVarArg = DAG.getMachineFunction().getFunction()->isVarArg();
  CCState CCInfo(CC, isVarArg, TM, RVLocs);
  CCInfo.AnalyzeReturn(Op.Val, RetCC_PPC);
  
  // If this is the first return lowered for this function, add the regs to the
  // liveout set for the function.
  if (DAG.getMachineFunction().getRegInfo().liveout_empty()) {
    for (unsigned i = 0; i != RVLocs.size(); ++i)
      DAG.getMachineFunction().getRegInfo().addLiveOut(RVLocs[i].getLocReg());
  }

  SDOperand Chain = Op.getOperand(0);

  Chain = GetPossiblePreceedingTailCall(Chain, PPCISD::TAILCALL);
  if (Chain.getOpcode() == PPCISD::TAILCALL) {
    SDOperand TailCall = Chain;
    SDOperand TargetAddress = TailCall.getOperand(1);
    SDOperand StackAdjustment = TailCall.getOperand(2);

    assert(((TargetAddress.getOpcode() == ISD::Register &&
             cast<RegisterSDNode>(TargetAddress)->getReg() == PPC::CTR) ||
            TargetAddress.getOpcode() == ISD::TargetExternalSymbol ||
            TargetAddress.getOpcode() == ISD::TargetGlobalAddress ||
            isa<ConstantSDNode>(TargetAddress)) &&
    "Expecting an global address, external symbol, absolute value or register");

    assert(StackAdjustment.getOpcode() == ISD::Constant &&
           "Expecting a const value");

    SmallVector<SDOperand,8> Operands;
    Operands.push_back(Chain.getOperand(0));
    Operands.push_back(TargetAddress);
    Operands.push_back(StackAdjustment);
    // Copy registers used by the call. Last operand is a flag so it is not
    // copied.
    for (unsigned i=3; i < TailCall.getNumOperands()-1; i++) {
      Operands.push_back(Chain.getOperand(i));
    }
    return DAG.getNode(PPCISD::TC_RETURN, MVT::Other, &Operands[0],
                       Operands.size());
  }

  SDOperand Flag;
  
  // Copy the result values into the output registers.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];
    assert(VA.isRegLoc() && "Can only return in registers!");
    Chain = DAG.getCopyToReg(Chain, VA.getLocReg(), Op.getOperand(i*2+1), Flag);
    Flag = Chain.getValue(1);
  }

  if (Flag.Val)
    return DAG.getNode(PPCISD::RET_FLAG, MVT::Other, Chain, Flag);
  else
    return DAG.getNode(PPCISD::RET_FLAG, MVT::Other, Chain);
}

SDOperand PPCTargetLowering::LowerSTACKRESTORE(SDOperand Op, SelectionDAG &DAG,
                                   const PPCSubtarget &Subtarget) {
  // When we pop the dynamic allocation we need to restore the SP link.
  
  // Get the corect type for pointers.
  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();

  // Construct the stack pointer operand.
  bool IsPPC64 = Subtarget.isPPC64();
  unsigned SP = IsPPC64 ? PPC::X1 : PPC::R1;
  SDOperand StackPtr = DAG.getRegister(SP, PtrVT);

  // Get the operands for the STACKRESTORE.
  SDOperand Chain = Op.getOperand(0);
  SDOperand SaveSP = Op.getOperand(1);
  
  // Load the old link SP.
  SDOperand LoadLinkSP = DAG.getLoad(PtrVT, Chain, StackPtr, NULL, 0);
  
  // Restore the stack pointer.
  Chain = DAG.getCopyToReg(LoadLinkSP.getValue(1), SP, SaveSP);
  
  // Store the old link SP.
  return DAG.getStore(Chain, LoadLinkSP, StackPtr, NULL, 0);
}



SDOperand
PPCTargetLowering::getReturnAddrFrameIndex(SelectionDAG & DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  bool IsPPC64 = PPCSubTarget.isPPC64();
  bool isMachoABI = PPCSubTarget.isMachoABI();
  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();

  // Get current frame pointer save index.  The users of this index will be
  // primarily DYNALLOC instructions.
  PPCFunctionInfo *FI = MF.getInfo<PPCFunctionInfo>();
  int RASI = FI->getReturnAddrSaveIndex();

  // If the frame pointer save index hasn't been defined yet.
  if (!RASI) {
    // Find out what the fix offset of the frame pointer save area.
    int LROffset = PPCFrameInfo::getReturnSaveOffset(IsPPC64, isMachoABI);
    // Allocate the frame index for frame pointer save area.
    RASI = MF.getFrameInfo()->CreateFixedObject(IsPPC64? 8 : 4, LROffset);
    // Save the result.
    FI->setReturnAddrSaveIndex(RASI);
  }
  return DAG.getFrameIndex(RASI, PtrVT);
}

SDOperand
PPCTargetLowering::getFramePointerFrameIndex(SelectionDAG & DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  bool IsPPC64 = PPCSubTarget.isPPC64();
  bool isMachoABI = PPCSubTarget.isMachoABI();
  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();

  // Get current frame pointer save index.  The users of this index will be
  // primarily DYNALLOC instructions.
  PPCFunctionInfo *FI = MF.getInfo<PPCFunctionInfo>();
  int FPSI = FI->getFramePointerSaveIndex();

  // If the frame pointer save index hasn't been defined yet.
  if (!FPSI) {
    // Find out what the fix offset of the frame pointer save area.
    int FPOffset = PPCFrameInfo::getFramePointerSaveOffset(IsPPC64, isMachoABI);
    
    // Allocate the frame index for frame pointer save area.
    FPSI = MF.getFrameInfo()->CreateFixedObject(IsPPC64? 8 : 4, FPOffset); 
    // Save the result.
    FI->setFramePointerSaveIndex(FPSI);                      
  }
  return DAG.getFrameIndex(FPSI, PtrVT);
}

SDOperand PPCTargetLowering::LowerDYNAMIC_STACKALLOC(SDOperand Op,
                                         SelectionDAG &DAG,
                                         const PPCSubtarget &Subtarget) {
  // Get the inputs.
  SDOperand Chain = Op.getOperand(0);
  SDOperand Size  = Op.getOperand(1);
  
  // Get the corect type for pointers.
  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();
  // Negate the size.
  SDOperand NegSize = DAG.getNode(ISD::SUB, PtrVT,
                                  DAG.getConstant(0, PtrVT), Size);
  // Construct a node for the frame pointer save index.
  SDOperand FPSIdx = getFramePointerFrameIndex(DAG);
  // Build a DYNALLOC node.
  SDOperand Ops[3] = { Chain, NegSize, FPSIdx };
  SDVTList VTs = DAG.getVTList(PtrVT, MVT::Other);
  return DAG.getNode(PPCISD::DYNALLOC, VTs, Ops, 3);
}

SDOperand PPCTargetLowering::LowerAtomicLAS(SDOperand Op, SelectionDAG &DAG) {
  MVT::ValueType VT = Op.Val->getValueType(0);
  SDOperand Chain   = Op.getOperand(0);
  SDOperand Ptr     = Op.getOperand(1);
  SDOperand Incr    = Op.getOperand(2);

  // Issue a "load and reserve".
  std::vector<MVT::ValueType> VTs;
  VTs.push_back(VT);
  VTs.push_back(MVT::Other);

  SDOperand Label  = DAG.getConstant(PPCAtomicLabelIndex++, MVT::i32);
  SDOperand Ops[] = {
    Chain,               // Chain
    Ptr,                 // Ptr
    Label,               // Label
  };
  SDOperand Load = DAG.getNode(PPCISD::LARX, VTs, Ops, 3);
  Chain = Load.getValue(1);

  // Compute new value.
  SDOperand NewVal  = DAG.getNode(ISD::ADD, VT, Load, Incr);

  // Issue a "store and check".
  SDOperand Ops2[] = {
    Chain,               // Chain
    NewVal,              // Value
    Ptr,                 // Ptr
    Label,               // Label
  };
  SDOperand Store = DAG.getNode(PPCISD::STCX, MVT::Other, Ops2, 4);
  SDOperand OutOps[] = { Load, Store };
  return DAG.getNode(ISD::MERGE_VALUES, DAG.getVTList(VT, MVT::Other),
                     OutOps, 2);
}

SDOperand PPCTargetLowering::LowerAtomicLCS(SDOperand Op, SelectionDAG &DAG) {
  MVT::ValueType VT = Op.Val->getValueType(0);
  SDOperand Chain   = Op.getOperand(0);
  SDOperand Ptr     = Op.getOperand(1);
  SDOperand NewVal  = Op.getOperand(2);
  SDOperand OldVal  = Op.getOperand(3);

  // Issue a "load and reserve".
  std::vector<MVT::ValueType> VTs;
  VTs.push_back(VT);
  VTs.push_back(MVT::Other);

  SDOperand Label  = DAG.getConstant(PPCAtomicLabelIndex++, MVT::i32);
  SDOperand Ops[] = {
    Chain,               // Chain
    Ptr,                 // Ptr
    Label,               // Label
  };
  SDOperand Load = DAG.getNode(PPCISD::LARX, VTs, Ops, 3);
  Chain = Load.getValue(1);

  // Compare and unreserve if not equal.
  SDOperand Ops2[] = {
    Chain,               // Chain
    OldVal,              // Old value
    Load,                // Value in memory
    Label,               // Label
  };
  Chain = DAG.getNode(PPCISD::CMP_UNRESERVE, MVT::Other, Ops2, 4);

  // Issue a "store and check".
  SDOperand Ops3[] = {
    Chain,               // Chain
    NewVal,              // Value
    Ptr,                 // Ptr
    Label,               // Label
  };
  SDOperand Store = DAG.getNode(PPCISD::STCX, MVT::Other, Ops3, 4);
  SDOperand OutOps[] = { Load, Store };
  return DAG.getNode(ISD::MERGE_VALUES, DAG.getVTList(VT, MVT::Other),
                     OutOps, 2);
}

SDOperand PPCTargetLowering::LowerAtomicSWAP(SDOperand Op, SelectionDAG &DAG) {
  MVT::ValueType VT = Op.Val->getValueType(0);
  SDOperand Chain   = Op.getOperand(0);
  SDOperand Ptr     = Op.getOperand(1);
  SDOperand NewVal  = Op.getOperand(2);

  // Issue a "load and reserve".
  std::vector<MVT::ValueType> VTs;
  VTs.push_back(VT);
  VTs.push_back(MVT::Other);

  SDOperand Label  = DAG.getConstant(PPCAtomicLabelIndex++, MVT::i32);
  SDOperand Ops[] = {
    Chain,               // Chain
    Ptr,                 // Ptr
    Label,               // Label
  };
  SDOperand Load = DAG.getNode(PPCISD::LARX, VTs, Ops, 3);
  Chain = Load.getValue(1);

  // Issue a "store and check".
  SDOperand Ops2[] = {
    Chain,               // Chain
    NewVal,              // Value
    Ptr,                 // Ptr
    Label,               // Label
  };
  SDOperand Store = DAG.getNode(PPCISD::STCX, MVT::Other, Ops2, 4);
  SDOperand OutOps[] = { Load, Store };
  return DAG.getNode(ISD::MERGE_VALUES, DAG.getVTList(VT, MVT::Other),
                     OutOps, 2);
}

/// LowerSELECT_CC - Lower floating point select_cc's into fsel instruction when
/// possible.
SDOperand PPCTargetLowering::LowerSELECT_CC(SDOperand Op, SelectionDAG &DAG) {
  // Not FP? Not a fsel.
  if (!MVT::isFloatingPoint(Op.getOperand(0).getValueType()) ||
      !MVT::isFloatingPoint(Op.getOperand(2).getValueType()))
    return SDOperand();
  
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(4))->get();
  
  // Cannot handle SETEQ/SETNE.
  if (CC == ISD::SETEQ || CC == ISD::SETNE) return SDOperand();
  
  MVT::ValueType ResVT = Op.getValueType();
  MVT::ValueType CmpVT = Op.getOperand(0).getValueType();
  SDOperand LHS = Op.getOperand(0), RHS = Op.getOperand(1);
  SDOperand TV  = Op.getOperand(2), FV  = Op.getOperand(3);
  
  // If the RHS of the comparison is a 0.0, we don't need to do the
  // subtraction at all.
  if (isFloatingPointZero(RHS))
    switch (CC) {
    default: break;       // SETUO etc aren't handled by fsel.
    case ISD::SETULT:
    case ISD::SETOLT:
    case ISD::SETLT:
      std::swap(TV, FV);  // fsel is natively setge, swap operands for setlt
    case ISD::SETUGE:
    case ISD::SETOGE:
    case ISD::SETGE:
      if (LHS.getValueType() == MVT::f32)   // Comparison is always 64-bits
        LHS = DAG.getNode(ISD::FP_EXTEND, MVT::f64, LHS);
      return DAG.getNode(PPCISD::FSEL, ResVT, LHS, TV, FV);
    case ISD::SETUGT:
    case ISD::SETOGT:
    case ISD::SETGT:
      std::swap(TV, FV);  // fsel is natively setge, swap operands for setlt
    case ISD::SETULE:
    case ISD::SETOLE:
    case ISD::SETLE:
      if (LHS.getValueType() == MVT::f32)   // Comparison is always 64-bits
        LHS = DAG.getNode(ISD::FP_EXTEND, MVT::f64, LHS);
      return DAG.getNode(PPCISD::FSEL, ResVT,
                         DAG.getNode(ISD::FNEG, MVT::f64, LHS), TV, FV);
    }
      
  SDOperand Cmp;
  switch (CC) {
  default: break;       // SETUO etc aren't handled by fsel.
  case ISD::SETULT:
  case ISD::SETOLT:
  case ISD::SETLT:
    Cmp = DAG.getNode(ISD::FSUB, CmpVT, LHS, RHS);
    if (Cmp.getValueType() == MVT::f32)   // Comparison is always 64-bits
      Cmp = DAG.getNode(ISD::FP_EXTEND, MVT::f64, Cmp);
      return DAG.getNode(PPCISD::FSEL, ResVT, Cmp, FV, TV);
  case ISD::SETUGE:
  case ISD::SETOGE:
  case ISD::SETGE:
    Cmp = DAG.getNode(ISD::FSUB, CmpVT, LHS, RHS);
    if (Cmp.getValueType() == MVT::f32)   // Comparison is always 64-bits
      Cmp = DAG.getNode(ISD::FP_EXTEND, MVT::f64, Cmp);
      return DAG.getNode(PPCISD::FSEL, ResVT, Cmp, TV, FV);
  case ISD::SETUGT:
  case ISD::SETOGT:
  case ISD::SETGT:
    Cmp = DAG.getNode(ISD::FSUB, CmpVT, RHS, LHS);
    if (Cmp.getValueType() == MVT::f32)   // Comparison is always 64-bits
      Cmp = DAG.getNode(ISD::FP_EXTEND, MVT::f64, Cmp);
      return DAG.getNode(PPCISD::FSEL, ResVT, Cmp, FV, TV);
  case ISD::SETULE:
  case ISD::SETOLE:
  case ISD::SETLE:
    Cmp = DAG.getNode(ISD::FSUB, CmpVT, RHS, LHS);
    if (Cmp.getValueType() == MVT::f32)   // Comparison is always 64-bits
      Cmp = DAG.getNode(ISD::FP_EXTEND, MVT::f64, Cmp);
      return DAG.getNode(PPCISD::FSEL, ResVT, Cmp, TV, FV);
  }
  return SDOperand();
}

// FIXME: Split this code up when LegalizeDAGTypes lands.
SDOperand PPCTargetLowering::LowerFP_TO_SINT(SDOperand Op, SelectionDAG &DAG) {
  assert(MVT::isFloatingPoint(Op.getOperand(0).getValueType()));
  SDOperand Src = Op.getOperand(0);
  if (Src.getValueType() == MVT::f32)
    Src = DAG.getNode(ISD::FP_EXTEND, MVT::f64, Src);
  
  SDOperand Tmp;
  switch (Op.getValueType()) {
  default: assert(0 && "Unhandled FP_TO_SINT type in custom expander!");
  case MVT::i32:
    Tmp = DAG.getNode(PPCISD::FCTIWZ, MVT::f64, Src);
    break;
  case MVT::i64:
    Tmp = DAG.getNode(PPCISD::FCTIDZ, MVT::f64, Src);
    break;
  }
  
  // Convert the FP value to an int value through memory.
  SDOperand FIPtr = DAG.CreateStackTemporary(MVT::f64);
  
  // Emit a store to the stack slot.
  SDOperand Chain = DAG.getStore(DAG.getEntryNode(), Tmp, FIPtr, NULL, 0);

  // Result is a load from the stack slot.  If loading 4 bytes, make sure to
  // add in a bias.
  if (Op.getValueType() == MVT::i32)
    FIPtr = DAG.getNode(ISD::ADD, FIPtr.getValueType(), FIPtr,
                        DAG.getConstant(4, FIPtr.getValueType()));
  return DAG.getLoad(Op.getValueType(), Chain, FIPtr, NULL, 0);
}

SDOperand PPCTargetLowering::LowerFP_ROUND_INREG(SDOperand Op, 
                                                 SelectionDAG &DAG) {
  assert(Op.getValueType() == MVT::ppcf128);
  SDNode *Node = Op.Val;
  assert(Node->getOperand(0).getValueType() == MVT::ppcf128);
  assert(Node->getOperand(0).Val->getOpcode() == ISD::BUILD_PAIR);
  SDOperand Lo = Node->getOperand(0).Val->getOperand(0);
  SDOperand Hi = Node->getOperand(0).Val->getOperand(1);

  // This sequence changes FPSCR to do round-to-zero, adds the two halves
  // of the long double, and puts FPSCR back the way it was.  We do not
  // actually model FPSCR.
  std::vector<MVT::ValueType> NodeTys;
  SDOperand Ops[4], Result, MFFSreg, InFlag, FPreg;

  NodeTys.push_back(MVT::f64);   // Return register
  NodeTys.push_back(MVT::Flag);    // Returns a flag for later insns
  Result = DAG.getNode(PPCISD::MFFS, NodeTys, &InFlag, 0);
  MFFSreg = Result.getValue(0);
  InFlag = Result.getValue(1);

  NodeTys.clear();
  NodeTys.push_back(MVT::Flag);   // Returns a flag
  Ops[0] = DAG.getConstant(31, MVT::i32);
  Ops[1] = InFlag;
  Result = DAG.getNode(PPCISD::MTFSB1, NodeTys, Ops, 2);
  InFlag = Result.getValue(0);

  NodeTys.clear();
  NodeTys.push_back(MVT::Flag);   // Returns a flag
  Ops[0] = DAG.getConstant(30, MVT::i32);
  Ops[1] = InFlag;
  Result = DAG.getNode(PPCISD::MTFSB0, NodeTys, Ops, 2);
  InFlag = Result.getValue(0);

  NodeTys.clear();
  NodeTys.push_back(MVT::f64);    // result of add
  NodeTys.push_back(MVT::Flag);   // Returns a flag
  Ops[0] = Lo;
  Ops[1] = Hi;
  Ops[2] = InFlag;
  Result = DAG.getNode(PPCISD::FADDRTZ, NodeTys, Ops, 3);
  FPreg = Result.getValue(0);
  InFlag = Result.getValue(1);

  NodeTys.clear();
  NodeTys.push_back(MVT::f64);
  Ops[0] = DAG.getConstant(1, MVT::i32);
  Ops[1] = MFFSreg;
  Ops[2] = FPreg;
  Ops[3] = InFlag;
  Result = DAG.getNode(PPCISD::MTFSF, NodeTys, Ops, 4);
  FPreg = Result.getValue(0);

  // We know the low half is about to be thrown away, so just use something
  // convenient.
  return DAG.getNode(ISD::BUILD_PAIR, Lo.getValueType(), FPreg, FPreg);
}

SDOperand PPCTargetLowering::LowerSINT_TO_FP(SDOperand Op, SelectionDAG &DAG) {
  // Don't handle ppc_fp128 here; let it be lowered to a libcall.
  if (Op.getValueType() != MVT::f32 && Op.getValueType() != MVT::f64)
    return SDOperand();

  if (Op.getOperand(0).getValueType() == MVT::i64) {
    SDOperand Bits = DAG.getNode(ISD::BIT_CONVERT, MVT::f64, Op.getOperand(0));
    SDOperand FP = DAG.getNode(PPCISD::FCFID, MVT::f64, Bits);
    if (Op.getValueType() == MVT::f32)
      FP = DAG.getNode(ISD::FP_ROUND, MVT::f32, FP, DAG.getIntPtrConstant(0));
    return FP;
  }
  
  assert(Op.getOperand(0).getValueType() == MVT::i32 &&
         "Unhandled SINT_TO_FP type in custom expander!");
  // Since we only generate this in 64-bit mode, we can take advantage of
  // 64-bit registers.  In particular, sign extend the input value into the
  // 64-bit register with extsw, store the WHOLE 64-bit value into the stack
  // then lfd it and fcfid it.
  MachineFrameInfo *FrameInfo = DAG.getMachineFunction().getFrameInfo();
  int FrameIdx = FrameInfo->CreateStackObject(8, 8);
  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();
  SDOperand FIdx = DAG.getFrameIndex(FrameIdx, PtrVT);
  
  SDOperand Ext64 = DAG.getNode(PPCISD::EXTSW_32, MVT::i32,
                                Op.getOperand(0));
  
  // STD the extended value into the stack slot.
  MachineMemOperand MO(PseudoSourceValue::getFixedStack(),
                       MachineMemOperand::MOStore, FrameIdx, 8, 8);
  SDOperand Store = DAG.getNode(PPCISD::STD_32, MVT::Other,
                                DAG.getEntryNode(), Ext64, FIdx,
                                DAG.getMemOperand(MO));
  // Load the value as a double.
  SDOperand Ld = DAG.getLoad(MVT::f64, Store, FIdx, NULL, 0);
  
  // FCFID it and return it.
  SDOperand FP = DAG.getNode(PPCISD::FCFID, MVT::f64, Ld);
  if (Op.getValueType() == MVT::f32)
    FP = DAG.getNode(ISD::FP_ROUND, MVT::f32, FP, DAG.getIntPtrConstant(0));
  return FP;
}

SDOperand PPCTargetLowering::LowerFLT_ROUNDS_(SDOperand Op, SelectionDAG &DAG) {
  /*
   The rounding mode is in bits 30:31 of FPSR, and has the following
   settings:
     00 Round to nearest
     01 Round to 0
     10 Round to +inf
     11 Round to -inf

  FLT_ROUNDS, on the other hand, expects the following:
    -1 Undefined
     0 Round to 0
     1 Round to nearest
     2 Round to +inf
     3 Round to -inf

  To perform the conversion, we do:
    ((FPSCR & 0x3) ^ ((~FPSCR & 0x3) >> 1))
  */

  MachineFunction &MF = DAG.getMachineFunction();
  MVT::ValueType VT = Op.getValueType();
  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();
  std::vector<MVT::ValueType> NodeTys;
  SDOperand MFFSreg, InFlag;

  // Save FP Control Word to register
  NodeTys.push_back(MVT::f64);    // return register
  NodeTys.push_back(MVT::Flag);   // unused in this context
  SDOperand Chain = DAG.getNode(PPCISD::MFFS, NodeTys, &InFlag, 0);

  // Save FP register to stack slot
  int SSFI = MF.getFrameInfo()->CreateStackObject(8, 8);
  SDOperand StackSlot = DAG.getFrameIndex(SSFI, PtrVT);
  SDOperand Store = DAG.getStore(DAG.getEntryNode(), Chain,
                                 StackSlot, NULL, 0);

  // Load FP Control Word from low 32 bits of stack slot.
  SDOperand Four = DAG.getConstant(4, PtrVT);
  SDOperand Addr = DAG.getNode(ISD::ADD, PtrVT, StackSlot, Four);
  SDOperand CWD = DAG.getLoad(MVT::i32, Store, Addr, NULL, 0);

  // Transform as necessary
  SDOperand CWD1 =
    DAG.getNode(ISD::AND, MVT::i32,
                CWD, DAG.getConstant(3, MVT::i32));
  SDOperand CWD2 =
    DAG.getNode(ISD::SRL, MVT::i32,
                DAG.getNode(ISD::AND, MVT::i32,
                            DAG.getNode(ISD::XOR, MVT::i32,
                                        CWD, DAG.getConstant(3, MVT::i32)),
                            DAG.getConstant(3, MVT::i32)),
                DAG.getConstant(1, MVT::i8));

  SDOperand RetVal =
    DAG.getNode(ISD::XOR, MVT::i32, CWD1, CWD2);

  return DAG.getNode((MVT::getSizeInBits(VT) < 16 ?
                      ISD::TRUNCATE : ISD::ZERO_EXTEND), VT, RetVal);
}

SDOperand PPCTargetLowering::LowerSHL_PARTS(SDOperand Op, SelectionDAG &DAG) {
  MVT::ValueType VT = Op.getValueType();
  unsigned BitWidth = MVT::getSizeInBits(VT);
  assert(Op.getNumOperands() == 3 &&
         VT == Op.getOperand(1).getValueType() &&
         "Unexpected SHL!");
  
  // Expand into a bunch of logical ops.  Note that these ops
  // depend on the PPC behavior for oversized shift amounts.
  SDOperand Lo = Op.getOperand(0);
  SDOperand Hi = Op.getOperand(1);
  SDOperand Amt = Op.getOperand(2);
  MVT::ValueType AmtVT = Amt.getValueType();
  
  SDOperand Tmp1 = DAG.getNode(ISD::SUB, AmtVT,
                               DAG.getConstant(BitWidth, AmtVT), Amt);
  SDOperand Tmp2 = DAG.getNode(PPCISD::SHL, VT, Hi, Amt);
  SDOperand Tmp3 = DAG.getNode(PPCISD::SRL, VT, Lo, Tmp1);
  SDOperand Tmp4 = DAG.getNode(ISD::OR , VT, Tmp2, Tmp3);
  SDOperand Tmp5 = DAG.getNode(ISD::ADD, AmtVT, Amt,
                               DAG.getConstant(-BitWidth, AmtVT));
  SDOperand Tmp6 = DAG.getNode(PPCISD::SHL, VT, Lo, Tmp5);
  SDOperand OutHi = DAG.getNode(ISD::OR, VT, Tmp4, Tmp6);
  SDOperand OutLo = DAG.getNode(PPCISD::SHL, VT, Lo, Amt);
  SDOperand OutOps[] = { OutLo, OutHi };
  return DAG.getNode(ISD::MERGE_VALUES, DAG.getVTList(VT, VT),
                     OutOps, 2);
}

SDOperand PPCTargetLowering::LowerSRL_PARTS(SDOperand Op, SelectionDAG &DAG) {
  MVT::ValueType VT = Op.getValueType();
  unsigned BitWidth = MVT::getSizeInBits(VT);
  assert(Op.getNumOperands() == 3 &&
         VT == Op.getOperand(1).getValueType() &&
         "Unexpected SRL!");
  
  // Expand into a bunch of logical ops.  Note that these ops
  // depend on the PPC behavior for oversized shift amounts.
  SDOperand Lo = Op.getOperand(0);
  SDOperand Hi = Op.getOperand(1);
  SDOperand Amt = Op.getOperand(2);
  MVT::ValueType AmtVT = Amt.getValueType();
  
  SDOperand Tmp1 = DAG.getNode(ISD::SUB, AmtVT,
                               DAG.getConstant(BitWidth, AmtVT), Amt);
  SDOperand Tmp2 = DAG.getNode(PPCISD::SRL, VT, Lo, Amt);
  SDOperand Tmp3 = DAG.getNode(PPCISD::SHL, VT, Hi, Tmp1);
  SDOperand Tmp4 = DAG.getNode(ISD::OR , VT, Tmp2, Tmp3);
  SDOperand Tmp5 = DAG.getNode(ISD::ADD, AmtVT, Amt,
                               DAG.getConstant(-BitWidth, AmtVT));
  SDOperand Tmp6 = DAG.getNode(PPCISD::SRL, VT, Hi, Tmp5);
  SDOperand OutLo = DAG.getNode(ISD::OR, VT, Tmp4, Tmp6);
  SDOperand OutHi = DAG.getNode(PPCISD::SRL, VT, Hi, Amt);
  SDOperand OutOps[] = { OutLo, OutHi };
  return DAG.getNode(ISD::MERGE_VALUES, DAG.getVTList(VT, VT),
                     OutOps, 2);
}

SDOperand PPCTargetLowering::LowerSRA_PARTS(SDOperand Op, SelectionDAG &DAG) {
  MVT::ValueType VT = Op.getValueType();
  unsigned BitWidth = MVT::getSizeInBits(VT);
  assert(Op.getNumOperands() == 3 &&
         VT == Op.getOperand(1).getValueType() &&
         "Unexpected SRA!");
  
  // Expand into a bunch of logical ops, followed by a select_cc.
  SDOperand Lo = Op.getOperand(0);
  SDOperand Hi = Op.getOperand(1);
  SDOperand Amt = Op.getOperand(2);
  MVT::ValueType AmtVT = Amt.getValueType();
  
  SDOperand Tmp1 = DAG.getNode(ISD::SUB, AmtVT,
                               DAG.getConstant(BitWidth, AmtVT), Amt);
  SDOperand Tmp2 = DAG.getNode(PPCISD::SRL, VT, Lo, Amt);
  SDOperand Tmp3 = DAG.getNode(PPCISD::SHL, VT, Hi, Tmp1);
  SDOperand Tmp4 = DAG.getNode(ISD::OR , VT, Tmp2, Tmp3);
  SDOperand Tmp5 = DAG.getNode(ISD::ADD, AmtVT, Amt,
                               DAG.getConstant(-BitWidth, AmtVT));
  SDOperand Tmp6 = DAG.getNode(PPCISD::SRA, VT, Hi, Tmp5);
  SDOperand OutHi = DAG.getNode(PPCISD::SRA, VT, Hi, Amt);
  SDOperand OutLo = DAG.getSelectCC(Tmp5, DAG.getConstant(0, AmtVT),
                                    Tmp4, Tmp6, ISD::SETLE);
  SDOperand OutOps[] = { OutLo, OutHi };
  return DAG.getNode(ISD::MERGE_VALUES, DAG.getVTList(VT, VT),
                     OutOps, 2);
}

//===----------------------------------------------------------------------===//
// Vector related lowering.
//

// If this is a vector of constants or undefs, get the bits.  A bit in
// UndefBits is set if the corresponding element of the vector is an 
// ISD::UNDEF value.  For undefs, the corresponding VectorBits values are
// zero.   Return true if this is not an array of constants, false if it is.
//
static bool GetConstantBuildVectorBits(SDNode *BV, uint64_t VectorBits[2],
                                       uint64_t UndefBits[2]) {
  // Start with zero'd results.
  VectorBits[0] = VectorBits[1] = UndefBits[0] = UndefBits[1] = 0;
  
  unsigned EltBitSize = MVT::getSizeInBits(BV->getOperand(0).getValueType());
  for (unsigned i = 0, e = BV->getNumOperands(); i != e; ++i) {
    SDOperand OpVal = BV->getOperand(i);
    
    unsigned PartNo = i >= e/2;     // In the upper 128 bits?
    unsigned SlotNo = e/2 - (i & (e/2-1))-1;  // Which subpiece of the uint64_t.

    uint64_t EltBits = 0;
    if (OpVal.getOpcode() == ISD::UNDEF) {
      uint64_t EltUndefBits = ~0U >> (32-EltBitSize);
      UndefBits[PartNo] |= EltUndefBits << (SlotNo*EltBitSize);
      continue;
    } else if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(OpVal)) {
      EltBits = CN->getValue() & (~0U >> (32-EltBitSize));
    } else if (ConstantFPSDNode *CN = dyn_cast<ConstantFPSDNode>(OpVal)) {
      assert(CN->getValueType(0) == MVT::f32 &&
             "Only one legal FP vector type!");
      EltBits = FloatToBits(CN->getValueAPF().convertToFloat());
    } else {
      // Nonconstant element.
      return true;
    }
    
    VectorBits[PartNo] |= EltBits << (SlotNo*EltBitSize);
  }
  
  //printf("%llx %llx  %llx %llx\n", 
  //       VectorBits[0], VectorBits[1], UndefBits[0], UndefBits[1]);
  return false;
}

// If this is a splat (repetition) of a value across the whole vector, return
// the smallest size that splats it.  For example, "0x01010101010101..." is a
// splat of 0x01, 0x0101, and 0x01010101.  We return SplatBits = 0x01 and 
// SplatSize = 1 byte.
static bool isConstantSplat(const uint64_t Bits128[2], 
                            const uint64_t Undef128[2],
                            unsigned &SplatBits, unsigned &SplatUndef,
                            unsigned &SplatSize) {
  
  // Don't let undefs prevent splats from matching.  See if the top 64-bits are
  // the same as the lower 64-bits, ignoring undefs.
  if ((Bits128[0] & ~Undef128[1]) != (Bits128[1] & ~Undef128[0]))
    return false;  // Can't be a splat if two pieces don't match.
  
  uint64_t Bits64  = Bits128[0] | Bits128[1];
  uint64_t Undef64 = Undef128[0] & Undef128[1];
  
  // Check that the top 32-bits are the same as the lower 32-bits, ignoring
  // undefs.
  if ((Bits64 & (~Undef64 >> 32)) != ((Bits64 >> 32) & ~Undef64))
    return false;  // Can't be a splat if two pieces don't match.

  uint32_t Bits32  = uint32_t(Bits64) | uint32_t(Bits64 >> 32);
  uint32_t Undef32 = uint32_t(Undef64) & uint32_t(Undef64 >> 32);

  // If the top 16-bits are different than the lower 16-bits, ignoring
  // undefs, we have an i32 splat.
  if ((Bits32 & (~Undef32 >> 16)) != ((Bits32 >> 16) & ~Undef32)) {
    SplatBits = Bits32;
    SplatUndef = Undef32;
    SplatSize = 4;
    return true;
  }
  
  uint16_t Bits16  = uint16_t(Bits32)  | uint16_t(Bits32 >> 16);
  uint16_t Undef16 = uint16_t(Undef32) & uint16_t(Undef32 >> 16);

  // If the top 8-bits are different than the lower 8-bits, ignoring
  // undefs, we have an i16 splat.
  if ((Bits16 & (uint16_t(~Undef16) >> 8)) != ((Bits16 >> 8) & ~Undef16)) {
    SplatBits = Bits16;
    SplatUndef = Undef16;
    SplatSize = 2;
    return true;
  }
  
  // Otherwise, we have an 8-bit splat.
  SplatBits  = uint8_t(Bits16)  | uint8_t(Bits16 >> 8);
  SplatUndef = uint8_t(Undef16) & uint8_t(Undef16 >> 8);
  SplatSize = 1;
  return true;
}

/// BuildSplatI - Build a canonical splati of Val with an element size of
/// SplatSize.  Cast the result to VT.
static SDOperand BuildSplatI(int Val, unsigned SplatSize, MVT::ValueType VT,
                             SelectionDAG &DAG) {
  assert(Val >= -16 && Val <= 15 && "vsplti is out of range!");

  static const MVT::ValueType VTys[] = { // canonical VT to use for each size.
    MVT::v16i8, MVT::v8i16, MVT::Other, MVT::v4i32
  };

  MVT::ValueType ReqVT = VT != MVT::Other ? VT : VTys[SplatSize-1];
  
  // Force vspltis[hw] -1 to vspltisb -1 to canonicalize.
  if (Val == -1)
    SplatSize = 1;
  
  MVT::ValueType CanonicalVT = VTys[SplatSize-1];
  
  // Build a canonical splat for this value.
  SDOperand Elt = DAG.getConstant(Val, MVT::getVectorElementType(CanonicalVT));
  SmallVector<SDOperand, 8> Ops;
  Ops.assign(MVT::getVectorNumElements(CanonicalVT), Elt);
  SDOperand Res = DAG.getNode(ISD::BUILD_VECTOR, CanonicalVT,
                              &Ops[0], Ops.size());
  return DAG.getNode(ISD::BIT_CONVERT, ReqVT, Res);
}

/// BuildIntrinsicOp - Return a binary operator intrinsic node with the
/// specified intrinsic ID.
static SDOperand BuildIntrinsicOp(unsigned IID, SDOperand LHS, SDOperand RHS,
                                  SelectionDAG &DAG, 
                                  MVT::ValueType DestVT = MVT::Other) {
  if (DestVT == MVT::Other) DestVT = LHS.getValueType();
  return DAG.getNode(ISD::INTRINSIC_WO_CHAIN, DestVT,
                     DAG.getConstant(IID, MVT::i32), LHS, RHS);
}

/// BuildIntrinsicOp - Return a ternary operator intrinsic node with the
/// specified intrinsic ID.
static SDOperand BuildIntrinsicOp(unsigned IID, SDOperand Op0, SDOperand Op1,
                                  SDOperand Op2, SelectionDAG &DAG, 
                                  MVT::ValueType DestVT = MVT::Other) {
  if (DestVT == MVT::Other) DestVT = Op0.getValueType();
  return DAG.getNode(ISD::INTRINSIC_WO_CHAIN, DestVT,
                     DAG.getConstant(IID, MVT::i32), Op0, Op1, Op2);
}


/// BuildVSLDOI - Return a VECTOR_SHUFFLE that is a vsldoi of the specified
/// amount.  The result has the specified value type.
static SDOperand BuildVSLDOI(SDOperand LHS, SDOperand RHS, unsigned Amt,
                             MVT::ValueType VT, SelectionDAG &DAG) {
  // Force LHS/RHS to be the right type.
  LHS = DAG.getNode(ISD::BIT_CONVERT, MVT::v16i8, LHS);
  RHS = DAG.getNode(ISD::BIT_CONVERT, MVT::v16i8, RHS);
  
  SDOperand Ops[16];
  for (unsigned i = 0; i != 16; ++i)
    Ops[i] = DAG.getConstant(i+Amt, MVT::i32);
  SDOperand T = DAG.getNode(ISD::VECTOR_SHUFFLE, MVT::v16i8, LHS, RHS,
                            DAG.getNode(ISD::BUILD_VECTOR, MVT::v16i8, Ops,16));
  return DAG.getNode(ISD::BIT_CONVERT, VT, T);
}

// If this is a case we can't handle, return null and let the default
// expansion code take care of it.  If we CAN select this case, and if it
// selects to a single instruction, return Op.  Otherwise, if we can codegen
// this case more efficiently than a constant pool load, lower it to the
// sequence of ops that should be used.
SDOperand PPCTargetLowering::LowerBUILD_VECTOR(SDOperand Op, 
                                               SelectionDAG &DAG) {
  // If this is a vector of constants or undefs, get the bits.  A bit in
  // UndefBits is set if the corresponding element of the vector is an 
  // ISD::UNDEF value.  For undefs, the corresponding VectorBits values are
  // zero. 
  uint64_t VectorBits[2];
  uint64_t UndefBits[2];
  if (GetConstantBuildVectorBits(Op.Val, VectorBits, UndefBits))
    return SDOperand();   // Not a constant vector.
  
  // If this is a splat (repetition) of a value across the whole vector, return
  // the smallest size that splats it.  For example, "0x01010101010101..." is a
  // splat of 0x01, 0x0101, and 0x01010101.  We return SplatBits = 0x01 and 
  // SplatSize = 1 byte.
  unsigned SplatBits, SplatUndef, SplatSize;
  if (isConstantSplat(VectorBits, UndefBits, SplatBits, SplatUndef, SplatSize)){
    bool HasAnyUndefs = (UndefBits[0] | UndefBits[1]) != 0;
    
    // First, handle single instruction cases.
    
    // All zeros?
    if (SplatBits == 0) {
      // Canonicalize all zero vectors to be v4i32.
      if (Op.getValueType() != MVT::v4i32 || HasAnyUndefs) {
        SDOperand Z = DAG.getConstant(0, MVT::i32);
        Z = DAG.getNode(ISD::BUILD_VECTOR, MVT::v4i32, Z, Z, Z, Z);
        Op = DAG.getNode(ISD::BIT_CONVERT, Op.getValueType(), Z);
      }
      return Op;
    }

    // If the sign extended value is in the range [-16,15], use VSPLTI[bhw].
    int32_t SextVal= int32_t(SplatBits << (32-8*SplatSize)) >> (32-8*SplatSize);
    if (SextVal >= -16 && SextVal <= 15)
      return BuildSplatI(SextVal, SplatSize, Op.getValueType(), DAG);
    
    
    // Two instruction sequences.
    
    // If this value is in the range [-32,30] and is even, use:
    //    tmp = VSPLTI[bhw], result = add tmp, tmp
    if (SextVal >= -32 && SextVal <= 30 && (SextVal & 1) == 0) {
      Op = BuildSplatI(SextVal >> 1, SplatSize, Op.getValueType(), DAG);
      return DAG.getNode(ISD::ADD, Op.getValueType(), Op, Op);
    }
    
    // If this is 0x8000_0000 x 4, turn into vspltisw + vslw.  If it is 
    // 0x7FFF_FFFF x 4, turn it into not(0x8000_0000).  This is important
    // for fneg/fabs.
    if (SplatSize == 4 && SplatBits == (0x7FFFFFFF&~SplatUndef)) {
      // Make -1 and vspltisw -1:
      SDOperand OnesV = BuildSplatI(-1, 4, MVT::v4i32, DAG);
      
      // Make the VSLW intrinsic, computing 0x8000_0000.
      SDOperand Res = BuildIntrinsicOp(Intrinsic::ppc_altivec_vslw, OnesV, 
                                       OnesV, DAG);
      
      // xor by OnesV to invert it.
      Res = DAG.getNode(ISD::XOR, MVT::v4i32, Res, OnesV);
      return DAG.getNode(ISD::BIT_CONVERT, Op.getValueType(), Res);
    }

    // Check to see if this is a wide variety of vsplti*, binop self cases.
    unsigned SplatBitSize = SplatSize*8;
    static const signed char SplatCsts[] = {
      -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -6, 6, -7, 7,
      -8, 8, -9, 9, -10, 10, -11, 11, -12, 12, -13, 13, 14, -14, 15, -15, -16
    };
    
    for (unsigned idx = 0; idx < array_lengthof(SplatCsts); ++idx) {
      // Indirect through the SplatCsts array so that we favor 'vsplti -1' for
      // cases which are ambiguous (e.g. formation of 0x8000_0000).  'vsplti -1'
      int i = SplatCsts[idx];
      
      // Figure out what shift amount will be used by altivec if shifted by i in
      // this splat size.
      unsigned TypeShiftAmt = i & (SplatBitSize-1);
      
      // vsplti + shl self.
      if (SextVal == (i << (int)TypeShiftAmt)) {
        SDOperand Res = BuildSplatI(i, SplatSize, MVT::Other, DAG);
        static const unsigned IIDs[] = { // Intrinsic to use for each size.
          Intrinsic::ppc_altivec_vslb, Intrinsic::ppc_altivec_vslh, 0,
          Intrinsic::ppc_altivec_vslw
        };
        Res = BuildIntrinsicOp(IIDs[SplatSize-1], Res, Res, DAG);
        return DAG.getNode(ISD::BIT_CONVERT, Op.getValueType(), Res);
      }
      
      // vsplti + srl self.
      if (SextVal == (int)((unsigned)i >> TypeShiftAmt)) {
        SDOperand Res = BuildSplatI(i, SplatSize, MVT::Other, DAG);
        static const unsigned IIDs[] = { // Intrinsic to use for each size.
          Intrinsic::ppc_altivec_vsrb, Intrinsic::ppc_altivec_vsrh, 0,
          Intrinsic::ppc_altivec_vsrw
        };
        Res = BuildIntrinsicOp(IIDs[SplatSize-1], Res, Res, DAG);
        return DAG.getNode(ISD::BIT_CONVERT, Op.getValueType(), Res);
      }
      
      // vsplti + sra self.
      if (SextVal == (int)((unsigned)i >> TypeShiftAmt)) {
        SDOperand Res = BuildSplatI(i, SplatSize, MVT::Other, DAG);
        static const unsigned IIDs[] = { // Intrinsic to use for each size.
          Intrinsic::ppc_altivec_vsrab, Intrinsic::ppc_altivec_vsrah, 0,
          Intrinsic::ppc_altivec_vsraw
        };
        Res = BuildIntrinsicOp(IIDs[SplatSize-1], Res, Res, DAG);
        return DAG.getNode(ISD::BIT_CONVERT, Op.getValueType(), Res);
      }
      
      // vsplti + rol self.
      if (SextVal == (int)(((unsigned)i << TypeShiftAmt) |
                           ((unsigned)i >> (SplatBitSize-TypeShiftAmt)))) {
        SDOperand Res = BuildSplatI(i, SplatSize, MVT::Other, DAG);
        static const unsigned IIDs[] = { // Intrinsic to use for each size.
          Intrinsic::ppc_altivec_vrlb, Intrinsic::ppc_altivec_vrlh, 0,
          Intrinsic::ppc_altivec_vrlw
        };
        Res = BuildIntrinsicOp(IIDs[SplatSize-1], Res, Res, DAG);
        return DAG.getNode(ISD::BIT_CONVERT, Op.getValueType(), Res);
      }

      // t = vsplti c, result = vsldoi t, t, 1
      if (SextVal == ((i << 8) | (i >> (TypeShiftAmt-8)))) {
        SDOperand T = BuildSplatI(i, SplatSize, MVT::v16i8, DAG);
        return BuildVSLDOI(T, T, 1, Op.getValueType(), DAG);
      }
      // t = vsplti c, result = vsldoi t, t, 2
      if (SextVal == ((i << 16) | (i >> (TypeShiftAmt-16)))) {
        SDOperand T = BuildSplatI(i, SplatSize, MVT::v16i8, DAG);
        return BuildVSLDOI(T, T, 2, Op.getValueType(), DAG);
      }
      // t = vsplti c, result = vsldoi t, t, 3
      if (SextVal == ((i << 24) | (i >> (TypeShiftAmt-24)))) {
        SDOperand T = BuildSplatI(i, SplatSize, MVT::v16i8, DAG);
        return BuildVSLDOI(T, T, 3, Op.getValueType(), DAG);
      }
    }
    
    // Three instruction sequences.
    
    // Odd, in range [17,31]:  (vsplti C)-(vsplti -16).
    if (SextVal >= 0 && SextVal <= 31) {
      SDOperand LHS = BuildSplatI(SextVal-16, SplatSize, MVT::Other, DAG);
      SDOperand RHS = BuildSplatI(-16, SplatSize, MVT::Other, DAG);
      LHS = DAG.getNode(ISD::SUB, LHS.getValueType(), LHS, RHS);
      return DAG.getNode(ISD::BIT_CONVERT, Op.getValueType(), LHS);
    }
    // Odd, in range [-31,-17]:  (vsplti C)+(vsplti -16).
    if (SextVal >= -31 && SextVal <= 0) {
      SDOperand LHS = BuildSplatI(SextVal+16, SplatSize, MVT::Other, DAG);
      SDOperand RHS = BuildSplatI(-16, SplatSize, MVT::Other, DAG);
      LHS = DAG.getNode(ISD::ADD, LHS.getValueType(), LHS, RHS);
      return DAG.getNode(ISD::BIT_CONVERT, Op.getValueType(), LHS);
    }
  }
    
  return SDOperand();
}

/// GeneratePerfectShuffle - Given an entry in the perfect-shuffle table, emit
/// the specified operations to build the shuffle.
static SDOperand GeneratePerfectShuffle(unsigned PFEntry, SDOperand LHS,
                                        SDOperand RHS, SelectionDAG &DAG) {
  unsigned OpNum = (PFEntry >> 26) & 0x0F;
  unsigned LHSID  = (PFEntry >> 13) & ((1 << 13)-1);
  unsigned RHSID = (PFEntry >>  0) & ((1 << 13)-1);
  
  enum {
    OP_COPY = 0,  // Copy, used for things like <u,u,u,3> to say it is <0,1,2,3>
    OP_VMRGHW,
    OP_VMRGLW,
    OP_VSPLTISW0,
    OP_VSPLTISW1,
    OP_VSPLTISW2,
    OP_VSPLTISW3,
    OP_VSLDOI4,
    OP_VSLDOI8,
    OP_VSLDOI12
  };
  
  if (OpNum == OP_COPY) {
    if (LHSID == (1*9+2)*9+3) return LHS;
    assert(LHSID == ((4*9+5)*9+6)*9+7 && "Illegal OP_COPY!");
    return RHS;
  }
  
  SDOperand OpLHS, OpRHS;
  OpLHS = GeneratePerfectShuffle(PerfectShuffleTable[LHSID], LHS, RHS, DAG);
  OpRHS = GeneratePerfectShuffle(PerfectShuffleTable[RHSID], LHS, RHS, DAG);
  
  unsigned ShufIdxs[16];
  switch (OpNum) {
  default: assert(0 && "Unknown i32 permute!");
  case OP_VMRGHW:
    ShufIdxs[ 0] =  0; ShufIdxs[ 1] =  1; ShufIdxs[ 2] =  2; ShufIdxs[ 3] =  3;
    ShufIdxs[ 4] = 16; ShufIdxs[ 5] = 17; ShufIdxs[ 6] = 18; ShufIdxs[ 7] = 19;
    ShufIdxs[ 8] =  4; ShufIdxs[ 9] =  5; ShufIdxs[10] =  6; ShufIdxs[11] =  7;
    ShufIdxs[12] = 20; ShufIdxs[13] = 21; ShufIdxs[14] = 22; ShufIdxs[15] = 23;
    break;
  case OP_VMRGLW:
    ShufIdxs[ 0] =  8; ShufIdxs[ 1] =  9; ShufIdxs[ 2] = 10; ShufIdxs[ 3] = 11;
    ShufIdxs[ 4] = 24; ShufIdxs[ 5] = 25; ShufIdxs[ 6] = 26; ShufIdxs[ 7] = 27;
    ShufIdxs[ 8] = 12; ShufIdxs[ 9] = 13; ShufIdxs[10] = 14; ShufIdxs[11] = 15;
    ShufIdxs[12] = 28; ShufIdxs[13] = 29; ShufIdxs[14] = 30; ShufIdxs[15] = 31;
    break;
  case OP_VSPLTISW0:
    for (unsigned i = 0; i != 16; ++i)
      ShufIdxs[i] = (i&3)+0;
    break;
  case OP_VSPLTISW1:
    for (unsigned i = 0; i != 16; ++i)
      ShufIdxs[i] = (i&3)+4;
    break;
  case OP_VSPLTISW2:
    for (unsigned i = 0; i != 16; ++i)
      ShufIdxs[i] = (i&3)+8;
    break;
  case OP_VSPLTISW3:
    for (unsigned i = 0; i != 16; ++i)
      ShufIdxs[i] = (i&3)+12;
    break;
  case OP_VSLDOI4:
    return BuildVSLDOI(OpLHS, OpRHS, 4, OpLHS.getValueType(), DAG);
  case OP_VSLDOI8:
    return BuildVSLDOI(OpLHS, OpRHS, 8, OpLHS.getValueType(), DAG);
  case OP_VSLDOI12:
    return BuildVSLDOI(OpLHS, OpRHS, 12, OpLHS.getValueType(), DAG);
  }
  SDOperand Ops[16];
  for (unsigned i = 0; i != 16; ++i)
    Ops[i] = DAG.getConstant(ShufIdxs[i], MVT::i32);
  
  return DAG.getNode(ISD::VECTOR_SHUFFLE, OpLHS.getValueType(), OpLHS, OpRHS,
                     DAG.getNode(ISD::BUILD_VECTOR, MVT::v16i8, Ops, 16));
}

/// LowerVECTOR_SHUFFLE - Return the code we lower for VECTOR_SHUFFLE.  If this
/// is a shuffle we can handle in a single instruction, return it.  Otherwise,
/// return the code it can be lowered into.  Worst case, it can always be
/// lowered into a vperm.
SDOperand PPCTargetLowering::LowerVECTOR_SHUFFLE(SDOperand Op, 
                                                 SelectionDAG &DAG) {
  SDOperand V1 = Op.getOperand(0);
  SDOperand V2 = Op.getOperand(1);
  SDOperand PermMask = Op.getOperand(2);
  
  // Cases that are handled by instructions that take permute immediates
  // (such as vsplt*) should be left as VECTOR_SHUFFLE nodes so they can be
  // selected by the instruction selector.
  if (V2.getOpcode() == ISD::UNDEF) {
    if (PPC::isSplatShuffleMask(PermMask.Val, 1) ||
        PPC::isSplatShuffleMask(PermMask.Val, 2) ||
        PPC::isSplatShuffleMask(PermMask.Val, 4) ||
        PPC::isVPKUWUMShuffleMask(PermMask.Val, true) ||
        PPC::isVPKUHUMShuffleMask(PermMask.Val, true) ||
        PPC::isVSLDOIShuffleMask(PermMask.Val, true) != -1 ||
        PPC::isVMRGLShuffleMask(PermMask.Val, 1, true) ||
        PPC::isVMRGLShuffleMask(PermMask.Val, 2, true) ||
        PPC::isVMRGLShuffleMask(PermMask.Val, 4, true) ||
        PPC::isVMRGHShuffleMask(PermMask.Val, 1, true) ||
        PPC::isVMRGHShuffleMask(PermMask.Val, 2, true) ||
        PPC::isVMRGHShuffleMask(PermMask.Val, 4, true)) {
      return Op;
    }
  }
  
  // Altivec has a variety of "shuffle immediates" that take two vector inputs
  // and produce a fixed permutation.  If any of these match, do not lower to
  // VPERM.
  if (PPC::isVPKUWUMShuffleMask(PermMask.Val, false) ||
      PPC::isVPKUHUMShuffleMask(PermMask.Val, false) ||
      PPC::isVSLDOIShuffleMask(PermMask.Val, false) != -1 ||
      PPC::isVMRGLShuffleMask(PermMask.Val, 1, false) ||
      PPC::isVMRGLShuffleMask(PermMask.Val, 2, false) ||
      PPC::isVMRGLShuffleMask(PermMask.Val, 4, false) ||
      PPC::isVMRGHShuffleMask(PermMask.Val, 1, false) ||
      PPC::isVMRGHShuffleMask(PermMask.Val, 2, false) ||
      PPC::isVMRGHShuffleMask(PermMask.Val, 4, false))
    return Op;
  
  // Check to see if this is a shuffle of 4-byte values.  If so, we can use our
  // perfect shuffle table to emit an optimal matching sequence.
  unsigned PFIndexes[4];
  bool isFourElementShuffle = true;
  for (unsigned i = 0; i != 4 && isFourElementShuffle; ++i) { // Element number
    unsigned EltNo = 8;   // Start out undef.
    for (unsigned j = 0; j != 4; ++j) {  // Intra-element byte.
      if (PermMask.getOperand(i*4+j).getOpcode() == ISD::UNDEF)
        continue;   // Undef, ignore it.
      
      unsigned ByteSource = 
        cast<ConstantSDNode>(PermMask.getOperand(i*4+j))->getValue();
      if ((ByteSource & 3) != j) {
        isFourElementShuffle = false;
        break;
      }
      
      if (EltNo == 8) {
        EltNo = ByteSource/4;
      } else if (EltNo != ByteSource/4) {
        isFourElementShuffle = false;
        break;
      }
    }
    PFIndexes[i] = EltNo;
  }
    
  // If this shuffle can be expressed as a shuffle of 4-byte elements, use the 
  // perfect shuffle vector to determine if it is cost effective to do this as
  // discrete instructions, or whether we should use a vperm.
  if (isFourElementShuffle) {
    // Compute the index in the perfect shuffle table.
    unsigned PFTableIndex = 
      PFIndexes[0]*9*9*9+PFIndexes[1]*9*9+PFIndexes[2]*9+PFIndexes[3];
    
    unsigned PFEntry = PerfectShuffleTable[PFTableIndex];
    unsigned Cost  = (PFEntry >> 30);
    
    // Determining when to avoid vperm is tricky.  Many things affect the cost
    // of vperm, particularly how many times the perm mask needs to be computed.
    // For example, if the perm mask can be hoisted out of a loop or is already
    // used (perhaps because there are multiple permutes with the same shuffle
    // mask?) the vperm has a cost of 1.  OTOH, hoisting the permute mask out of
    // the loop requires an extra register.
    //
    // As a compromise, we only emit discrete instructions if the shuffle can be
    // generated in 3 or fewer operations.  When we have loop information 
    // available, if this block is within a loop, we should avoid using vperm
    // for 3-operation perms and use a constant pool load instead.
    if (Cost < 3) 
      return GeneratePerfectShuffle(PFEntry, V1, V2, DAG);
  }
  
  // Lower this to a VPERM(V1, V2, V3) expression, where V3 is a constant
  // vector that will get spilled to the constant pool.
  if (V2.getOpcode() == ISD::UNDEF) V2 = V1;
  
  // The SHUFFLE_VECTOR mask is almost exactly what we want for vperm, except
  // that it is in input element units, not in bytes.  Convert now.
  MVT::ValueType EltVT = MVT::getVectorElementType(V1.getValueType());
  unsigned BytesPerElement = MVT::getSizeInBits(EltVT)/8;
  
  SmallVector<SDOperand, 16> ResultMask;
  for (unsigned i = 0, e = PermMask.getNumOperands(); i != e; ++i) {
    unsigned SrcElt;
    if (PermMask.getOperand(i).getOpcode() == ISD::UNDEF)
      SrcElt = 0;
    else 
      SrcElt = cast<ConstantSDNode>(PermMask.getOperand(i))->getValue();
    
    for (unsigned j = 0; j != BytesPerElement; ++j)
      ResultMask.push_back(DAG.getConstant(SrcElt*BytesPerElement+j,
                                           MVT::i8));
  }
  
  SDOperand VPermMask = DAG.getNode(ISD::BUILD_VECTOR, MVT::v16i8,
                                    &ResultMask[0], ResultMask.size());
  return DAG.getNode(PPCISD::VPERM, V1.getValueType(), V1, V2, VPermMask);
}

/// getAltivecCompareInfo - Given an intrinsic, return false if it is not an
/// altivec comparison.  If it is, return true and fill in Opc/isDot with
/// information about the intrinsic.
static bool getAltivecCompareInfo(SDOperand Intrin, int &CompareOpc,
                                  bool &isDot) {
  unsigned IntrinsicID = cast<ConstantSDNode>(Intrin.getOperand(0))->getValue();
  CompareOpc = -1;
  isDot = false;
  switch (IntrinsicID) {
  default: return false;
    // Comparison predicates.
  case Intrinsic::ppc_altivec_vcmpbfp_p:  CompareOpc = 966; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpeqfp_p: CompareOpc = 198; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpequb_p: CompareOpc =   6; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpequh_p: CompareOpc =  70; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpequw_p: CompareOpc = 134; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpgefp_p: CompareOpc = 454; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpgtfp_p: CompareOpc = 710; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpgtsb_p: CompareOpc = 774; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpgtsh_p: CompareOpc = 838; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpgtsw_p: CompareOpc = 902; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpgtub_p: CompareOpc = 518; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpgtuh_p: CompareOpc = 582; isDot = 1; break;
  case Intrinsic::ppc_altivec_vcmpgtuw_p: CompareOpc = 646; isDot = 1; break;
    
    // Normal Comparisons.
  case Intrinsic::ppc_altivec_vcmpbfp:    CompareOpc = 966; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpeqfp:   CompareOpc = 198; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpequb:   CompareOpc =   6; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpequh:   CompareOpc =  70; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpequw:   CompareOpc = 134; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpgefp:   CompareOpc = 454; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpgtfp:   CompareOpc = 710; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpgtsb:   CompareOpc = 774; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpgtsh:   CompareOpc = 838; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpgtsw:   CompareOpc = 902; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpgtub:   CompareOpc = 518; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpgtuh:   CompareOpc = 582; isDot = 0; break;
  case Intrinsic::ppc_altivec_vcmpgtuw:   CompareOpc = 646; isDot = 0; break;
  }
  return true;
}

/// LowerINTRINSIC_WO_CHAIN - If this is an intrinsic that we want to custom
/// lower, do it, otherwise return null.
SDOperand PPCTargetLowering::LowerINTRINSIC_WO_CHAIN(SDOperand Op, 
                                                     SelectionDAG &DAG) {
  // If this is a lowered altivec predicate compare, CompareOpc is set to the
  // opcode number of the comparison.
  int CompareOpc;
  bool isDot;
  if (!getAltivecCompareInfo(Op, CompareOpc, isDot))
    return SDOperand();    // Don't custom lower most intrinsics.
  
  // If this is a non-dot comparison, make the VCMP node and we are done.
  if (!isDot) {
    SDOperand Tmp = DAG.getNode(PPCISD::VCMP, Op.getOperand(2).getValueType(),
                                Op.getOperand(1), Op.getOperand(2),
                                DAG.getConstant(CompareOpc, MVT::i32));
    return DAG.getNode(ISD::BIT_CONVERT, Op.getValueType(), Tmp);
  }
  
  // Create the PPCISD altivec 'dot' comparison node.
  SDOperand Ops[] = {
    Op.getOperand(2),  // LHS
    Op.getOperand(3),  // RHS
    DAG.getConstant(CompareOpc, MVT::i32)
  };
  std::vector<MVT::ValueType> VTs;
  VTs.push_back(Op.getOperand(2).getValueType());
  VTs.push_back(MVT::Flag);
  SDOperand CompNode = DAG.getNode(PPCISD::VCMPo, VTs, Ops, 3);
  
  // Now that we have the comparison, emit a copy from the CR to a GPR.
  // This is flagged to the above dot comparison.
  SDOperand Flags = DAG.getNode(PPCISD::MFCR, MVT::i32,
                                DAG.getRegister(PPC::CR6, MVT::i32),
                                CompNode.getValue(1)); 
  
  // Unpack the result based on how the target uses it.
  unsigned BitNo;   // Bit # of CR6.
  bool InvertBit;   // Invert result?
  switch (cast<ConstantSDNode>(Op.getOperand(1))->getValue()) {
  default:  // Can't happen, don't crash on invalid number though.
  case 0:   // Return the value of the EQ bit of CR6.
    BitNo = 0; InvertBit = false;
    break;
  case 1:   // Return the inverted value of the EQ bit of CR6.
    BitNo = 0; InvertBit = true;
    break;
  case 2:   // Return the value of the LT bit of CR6.
    BitNo = 2; InvertBit = false;
    break;
  case 3:   // Return the inverted value of the LT bit of CR6.
    BitNo = 2; InvertBit = true;
    break;
  }
  
  // Shift the bit into the low position.
  Flags = DAG.getNode(ISD::SRL, MVT::i32, Flags,
                      DAG.getConstant(8-(3-BitNo), MVT::i32));
  // Isolate the bit.
  Flags = DAG.getNode(ISD::AND, MVT::i32, Flags,
                      DAG.getConstant(1, MVT::i32));
  
  // If we are supposed to, toggle the bit.
  if (InvertBit)
    Flags = DAG.getNode(ISD::XOR, MVT::i32, Flags,
                        DAG.getConstant(1, MVT::i32));
  return Flags;
}

SDOperand PPCTargetLowering::LowerSCALAR_TO_VECTOR(SDOperand Op, 
                                                   SelectionDAG &DAG) {
  // Create a stack slot that is 16-byte aligned.
  MachineFrameInfo *FrameInfo = DAG.getMachineFunction().getFrameInfo();
  int FrameIdx = FrameInfo->CreateStackObject(16, 16);
  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();
  SDOperand FIdx = DAG.getFrameIndex(FrameIdx, PtrVT);
  
  // Store the input value into Value#0 of the stack slot.
  SDOperand Store = DAG.getStore(DAG.getEntryNode(),
                                 Op.getOperand(0), FIdx, NULL, 0);
  // Load it out.
  return DAG.getLoad(Op.getValueType(), Store, FIdx, NULL, 0);
}

SDOperand PPCTargetLowering::LowerMUL(SDOperand Op, SelectionDAG &DAG) {
  if (Op.getValueType() == MVT::v4i32) {
    SDOperand LHS = Op.getOperand(0), RHS = Op.getOperand(1);
    
    SDOperand Zero  = BuildSplatI(  0, 1, MVT::v4i32, DAG);
    SDOperand Neg16 = BuildSplatI(-16, 4, MVT::v4i32, DAG); // +16 as shift amt.
    
    SDOperand RHSSwap =   // = vrlw RHS, 16
      BuildIntrinsicOp(Intrinsic::ppc_altivec_vrlw, RHS, Neg16, DAG);
    
    // Shrinkify inputs to v8i16.
    LHS = DAG.getNode(ISD::BIT_CONVERT, MVT::v8i16, LHS);
    RHS = DAG.getNode(ISD::BIT_CONVERT, MVT::v8i16, RHS);
    RHSSwap = DAG.getNode(ISD::BIT_CONVERT, MVT::v8i16, RHSSwap);
    
    // Low parts multiplied together, generating 32-bit results (we ignore the
    // top parts).
    SDOperand LoProd = BuildIntrinsicOp(Intrinsic::ppc_altivec_vmulouh,
                                        LHS, RHS, DAG, MVT::v4i32);
    
    SDOperand HiProd = BuildIntrinsicOp(Intrinsic::ppc_altivec_vmsumuhm,
                                        LHS, RHSSwap, Zero, DAG, MVT::v4i32);
    // Shift the high parts up 16 bits.
    HiProd = BuildIntrinsicOp(Intrinsic::ppc_altivec_vslw, HiProd, Neg16, DAG);
    return DAG.getNode(ISD::ADD, MVT::v4i32, LoProd, HiProd);
  } else if (Op.getValueType() == MVT::v8i16) {
    SDOperand LHS = Op.getOperand(0), RHS = Op.getOperand(1);
    
    SDOperand Zero = BuildSplatI(0, 1, MVT::v8i16, DAG);

    return BuildIntrinsicOp(Intrinsic::ppc_altivec_vmladduhm,
                            LHS, RHS, Zero, DAG);
  } else if (Op.getValueType() == MVT::v16i8) {
    SDOperand LHS = Op.getOperand(0), RHS = Op.getOperand(1);
    
    // Multiply the even 8-bit parts, producing 16-bit sums.
    SDOperand EvenParts = BuildIntrinsicOp(Intrinsic::ppc_altivec_vmuleub,
                                           LHS, RHS, DAG, MVT::v8i16);
    EvenParts = DAG.getNode(ISD::BIT_CONVERT, MVT::v16i8, EvenParts);
    
    // Multiply the odd 8-bit parts, producing 16-bit sums.
    SDOperand OddParts = BuildIntrinsicOp(Intrinsic::ppc_altivec_vmuloub,
                                          LHS, RHS, DAG, MVT::v8i16);
    OddParts = DAG.getNode(ISD::BIT_CONVERT, MVT::v16i8, OddParts);
    
    // Merge the results together.
    SDOperand Ops[16];
    for (unsigned i = 0; i != 8; ++i) {
      Ops[i*2  ] = DAG.getConstant(2*i+1, MVT::i8);
      Ops[i*2+1] = DAG.getConstant(2*i+1+16, MVT::i8);
    }
    return DAG.getNode(ISD::VECTOR_SHUFFLE, MVT::v16i8, EvenParts, OddParts,
                       DAG.getNode(ISD::BUILD_VECTOR, MVT::v16i8, Ops, 16));
  } else {
    assert(0 && "Unknown mul to lower!");
    abort();
  }
}

/// LowerOperation - Provide custom lowering hooks for some operations.
///
SDOperand PPCTargetLowering::LowerOperation(SDOperand Op, SelectionDAG &DAG) {
  switch (Op.getOpcode()) {
  default: assert(0 && "Wasn't expecting to be able to lower this!"); 
  case ISD::ConstantPool:       return LowerConstantPool(Op, DAG);
  case ISD::GlobalAddress:      return LowerGlobalAddress(Op, DAG);
  case ISD::GlobalTLSAddress:   return LowerGlobalTLSAddress(Op, DAG);
  case ISD::JumpTable:          return LowerJumpTable(Op, DAG);
  case ISD::SETCC:              return LowerSETCC(Op, DAG);
  case ISD::VASTART:            
    return LowerVASTART(Op, DAG, VarArgsFrameIndex, VarArgsStackOffset,
                        VarArgsNumGPR, VarArgsNumFPR, PPCSubTarget);
  
  case ISD::VAARG:            
    return LowerVAARG(Op, DAG, VarArgsFrameIndex, VarArgsStackOffset,
                      VarArgsNumGPR, VarArgsNumFPR, PPCSubTarget);

  case ISD::FORMAL_ARGUMENTS:
    return LowerFORMAL_ARGUMENTS(Op, DAG, VarArgsFrameIndex, 
                                 VarArgsStackOffset, VarArgsNumGPR,
                                 VarArgsNumFPR, PPCSubTarget);

  case ISD::CALL:               return LowerCALL(Op, DAG, PPCSubTarget,
                                                 getTargetMachine());
  case ISD::RET:                return LowerRET(Op, DAG, getTargetMachine());
  case ISD::STACKRESTORE:       return LowerSTACKRESTORE(Op, DAG, PPCSubTarget);
  case ISD::DYNAMIC_STACKALLOC:
    return LowerDYNAMIC_STACKALLOC(Op, DAG, PPCSubTarget);

  case ISD::ATOMIC_LAS:         return LowerAtomicLAS(Op, DAG);
  case ISD::ATOMIC_LCS:         return LowerAtomicLCS(Op, DAG);
  case ISD::ATOMIC_SWAP:        return LowerAtomicSWAP(Op, DAG);
    
  case ISD::SELECT_CC:          return LowerSELECT_CC(Op, DAG);
  case ISD::FP_TO_SINT:         return LowerFP_TO_SINT(Op, DAG);
  case ISD::SINT_TO_FP:         return LowerSINT_TO_FP(Op, DAG);
  case ISD::FP_ROUND_INREG:     return LowerFP_ROUND_INREG(Op, DAG);
  case ISD::FLT_ROUNDS_:        return LowerFLT_ROUNDS_(Op, DAG);

  // Lower 64-bit shifts.
  case ISD::SHL_PARTS:          return LowerSHL_PARTS(Op, DAG);
  case ISD::SRL_PARTS:          return LowerSRL_PARTS(Op, DAG);
  case ISD::SRA_PARTS:          return LowerSRA_PARTS(Op, DAG);

  // Vector-related lowering.
  case ISD::BUILD_VECTOR:       return LowerBUILD_VECTOR(Op, DAG);
  case ISD::VECTOR_SHUFFLE:     return LowerVECTOR_SHUFFLE(Op, DAG);
  case ISD::INTRINSIC_WO_CHAIN: return LowerINTRINSIC_WO_CHAIN(Op, DAG);
  case ISD::SCALAR_TO_VECTOR:   return LowerSCALAR_TO_VECTOR(Op, DAG);
  case ISD::MUL:                return LowerMUL(Op, DAG);
  
  // Frame & Return address.
  case ISD::RETURNADDR:         return LowerRETURNADDR(Op, DAG);
  case ISD::FRAMEADDR:          return LowerFRAMEADDR(Op, DAG);
  }
  return SDOperand();
}

SDNode *PPCTargetLowering::ExpandOperationResult(SDNode *N, SelectionDAG &DAG) {
  switch (N->getOpcode()) {
  default: assert(0 && "Wasn't expecting to be able to lower this!");
  case ISD::FP_TO_SINT: return LowerFP_TO_SINT(SDOperand(N, 0), DAG).Val;
  }
}


//===----------------------------------------------------------------------===//
//  Other Lowering Code
//===----------------------------------------------------------------------===//

MachineBasicBlock *
PPCTargetLowering::EmitInstrWithCustomInserter(MachineInstr *MI,
                                               MachineBasicBlock *BB) {
  const TargetInstrInfo *TII = getTargetMachine().getInstrInfo();
  assert((MI->getOpcode() == PPC::SELECT_CC_I4 ||
          MI->getOpcode() == PPC::SELECT_CC_I8 ||
          MI->getOpcode() == PPC::SELECT_CC_F4 ||
          MI->getOpcode() == PPC::SELECT_CC_F8 ||
          MI->getOpcode() == PPC::SELECT_CC_VRRC) &&
         "Unexpected instr type to insert");
  
  // To "insert" a SELECT_CC instruction, we actually have to insert the diamond
  // control-flow pattern.  The incoming instruction knows the destination vreg
  // to set, the condition code register to branch on, the true/false values to
  // select between, and a branch opcode to use.
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  ilist<MachineBasicBlock>::iterator It = BB;
  ++It;
  
  //  thisMBB:
  //  ...
  //   TrueVal = ...
  //   cmpTY ccX, r1, r2
  //   bCC copy1MBB
  //   fallthrough --> copy0MBB
  MachineBasicBlock *thisMBB = BB;
  MachineBasicBlock *copy0MBB = new MachineBasicBlock(LLVM_BB);
  MachineBasicBlock *sinkMBB = new MachineBasicBlock(LLVM_BB);
  unsigned SelectPred = MI->getOperand(4).getImm();
  BuildMI(BB, TII->get(PPC::BCC))
    .addImm(SelectPred).addReg(MI->getOperand(1).getReg()).addMBB(sinkMBB);
  MachineFunction *F = BB->getParent();
  F->getBasicBlockList().insert(It, copy0MBB);
  F->getBasicBlockList().insert(It, sinkMBB);
  // Update machine-CFG edges by first adding all successors of the current
  // block to the new block which will contain the Phi node for the select.
  for(MachineBasicBlock::succ_iterator i = BB->succ_begin(), 
      e = BB->succ_end(); i != e; ++i)
    sinkMBB->addSuccessor(*i);
  // Next, remove all successors of the current block, and add the true
  // and fallthrough blocks as its successors.
  while(!BB->succ_empty())
    BB->removeSuccessor(BB->succ_begin());
  BB->addSuccessor(copy0MBB);
  BB->addSuccessor(sinkMBB);
  
  //  copy0MBB:
  //   %FalseValue = ...
  //   # fallthrough to sinkMBB
  BB = copy0MBB;
  
  // Update machine-CFG edges
  BB->addSuccessor(sinkMBB);
  
  //  sinkMBB:
  //   %Result = phi [ %FalseValue, copy0MBB ], [ %TrueValue, thisMBB ]
  //  ...
  BB = sinkMBB;
  BuildMI(BB, TII->get(PPC::PHI), MI->getOperand(0).getReg())
    .addReg(MI->getOperand(3).getReg()).addMBB(copy0MBB)
    .addReg(MI->getOperand(2).getReg()).addMBB(thisMBB);

  delete MI;   // The pseudo instruction is gone now.
  return BB;
}

//===----------------------------------------------------------------------===//
// Target Optimization Hooks
//===----------------------------------------------------------------------===//

SDOperand PPCTargetLowering::PerformDAGCombine(SDNode *N, 
                                               DAGCombinerInfo &DCI) const {
  TargetMachine &TM = getTargetMachine();
  SelectionDAG &DAG = DCI.DAG;
  switch (N->getOpcode()) {
  default: break;
  case PPCISD::SHL:
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(N->getOperand(0))) {
      if (C->getValue() == 0)   // 0 << V -> 0.
        return N->getOperand(0);
    }
    break;
  case PPCISD::SRL:
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(N->getOperand(0))) {
      if (C->getValue() == 0)   // 0 >>u V -> 0.
        return N->getOperand(0);
    }
    break;
  case PPCISD::SRA:
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(N->getOperand(0))) {
      if (C->getValue() == 0 ||   //  0 >>s V -> 0.
          C->isAllOnesValue())    // -1 >>s V -> -1.
        return N->getOperand(0);
    }
    break;
    
  case ISD::SINT_TO_FP:
    if (TM.getSubtarget<PPCSubtarget>().has64BitSupport()) {
      if (N->getOperand(0).getOpcode() == ISD::FP_TO_SINT) {
        // Turn (sint_to_fp (fp_to_sint X)) -> fctidz/fcfid without load/stores.
        // We allow the src/dst to be either f32/f64, but the intermediate
        // type must be i64.
        if (N->getOperand(0).getValueType() == MVT::i64 &&
            N->getOperand(0).getOperand(0).getValueType() != MVT::ppcf128) {
          SDOperand Val = N->getOperand(0).getOperand(0);
          if (Val.getValueType() == MVT::f32) {
            Val = DAG.getNode(ISD::FP_EXTEND, MVT::f64, Val);
            DCI.AddToWorklist(Val.Val);
          }
            
          Val = DAG.getNode(PPCISD::FCTIDZ, MVT::f64, Val);
          DCI.AddToWorklist(Val.Val);
          Val = DAG.getNode(PPCISD::FCFID, MVT::f64, Val);
          DCI.AddToWorklist(Val.Val);
          if (N->getValueType(0) == MVT::f32) {
            Val = DAG.getNode(ISD::FP_ROUND, MVT::f32, Val, 
                              DAG.getIntPtrConstant(0));
            DCI.AddToWorklist(Val.Val);
          }
          return Val;
        } else if (N->getOperand(0).getValueType() == MVT::i32) {
          // If the intermediate type is i32, we can avoid the load/store here
          // too.
        }
      }
    }
    break;
  case ISD::STORE:
    // Turn STORE (FP_TO_SINT F) -> STFIWX(FCTIWZ(F)).
    if (TM.getSubtarget<PPCSubtarget>().hasSTFIWX() &&
        !cast<StoreSDNode>(N)->isTruncatingStore() &&
        N->getOperand(1).getOpcode() == ISD::FP_TO_SINT &&
        N->getOperand(1).getValueType() == MVT::i32 &&
        N->getOperand(1).getOperand(0).getValueType() != MVT::ppcf128) {
      SDOperand Val = N->getOperand(1).getOperand(0);
      if (Val.getValueType() == MVT::f32) {
        Val = DAG.getNode(ISD::FP_EXTEND, MVT::f64, Val);
        DCI.AddToWorklist(Val.Val);
      }
      Val = DAG.getNode(PPCISD::FCTIWZ, MVT::f64, Val);
      DCI.AddToWorklist(Val.Val);

      Val = DAG.getNode(PPCISD::STFIWX, MVT::Other, N->getOperand(0), Val,
                        N->getOperand(2), N->getOperand(3));
      DCI.AddToWorklist(Val.Val);
      return Val;
    }
    
    // Turn STORE (BSWAP) -> sthbrx/stwbrx.
    if (N->getOperand(1).getOpcode() == ISD::BSWAP &&
        N->getOperand(1).Val->hasOneUse() &&
        (N->getOperand(1).getValueType() == MVT::i32 ||
         N->getOperand(1).getValueType() == MVT::i16)) {
      SDOperand BSwapOp = N->getOperand(1).getOperand(0);
      // Do an any-extend to 32-bits if this is a half-word input.
      if (BSwapOp.getValueType() == MVT::i16)
        BSwapOp = DAG.getNode(ISD::ANY_EXTEND, MVT::i32, BSwapOp);

      return DAG.getNode(PPCISD::STBRX, MVT::Other, N->getOperand(0), BSwapOp,
                         N->getOperand(2), N->getOperand(3),
                         DAG.getValueType(N->getOperand(1).getValueType()));
    }
    break;
  case ISD::BSWAP:
    // Turn BSWAP (LOAD) -> lhbrx/lwbrx.
    if (ISD::isNON_EXTLoad(N->getOperand(0).Val) &&
        N->getOperand(0).hasOneUse() &&
        (N->getValueType(0) == MVT::i32 || N->getValueType(0) == MVT::i16)) {
      SDOperand Load = N->getOperand(0);
      LoadSDNode *LD = cast<LoadSDNode>(Load);
      // Create the byte-swapping load.
      std::vector<MVT::ValueType> VTs;
      VTs.push_back(MVT::i32);
      VTs.push_back(MVT::Other);
      SDOperand MO = DAG.getMemOperand(LD->getMemOperand());
      SDOperand Ops[] = {
        LD->getChain(),    // Chain
        LD->getBasePtr(),  // Ptr
        MO,                // MemOperand
        DAG.getValueType(N->getValueType(0)) // VT
      };
      SDOperand BSLoad = DAG.getNode(PPCISD::LBRX, VTs, Ops, 4);

      // If this is an i16 load, insert the truncate.  
      SDOperand ResVal = BSLoad;
      if (N->getValueType(0) == MVT::i16)
        ResVal = DAG.getNode(ISD::TRUNCATE, MVT::i16, BSLoad);
      
      // First, combine the bswap away.  This makes the value produced by the
      // load dead.
      DCI.CombineTo(N, ResVal);

      // Next, combine the load away, we give it a bogus result value but a real
      // chain result.  The result value is dead because the bswap is dead.
      DCI.CombineTo(Load.Val, ResVal, BSLoad.getValue(1));
      
      // Return N so it doesn't get rechecked!
      return SDOperand(N, 0);
    }
    
    break;
  case PPCISD::VCMP: {
    // If a VCMPo node already exists with exactly the same operands as this
    // node, use its result instead of this node (VCMPo computes both a CR6 and
    // a normal output).
    //
    if (!N->getOperand(0).hasOneUse() &&
        !N->getOperand(1).hasOneUse() &&
        !N->getOperand(2).hasOneUse()) {
      
      // Scan all of the users of the LHS, looking for VCMPo's that match.
      SDNode *VCMPoNode = 0;
      
      SDNode *LHSN = N->getOperand(0).Val;
      for (SDNode::use_iterator UI = LHSN->use_begin(), E = LHSN->use_end();
           UI != E; ++UI)
        if ((*UI).getUser()->getOpcode() == PPCISD::VCMPo &&
            (*UI).getUser()->getOperand(1) == N->getOperand(1) &&
            (*UI).getUser()->getOperand(2) == N->getOperand(2) &&
            (*UI).getUser()->getOperand(0) == N->getOperand(0)) {
          VCMPoNode = UI->getUser();
          break;
        }
      
      // If there is no VCMPo node, or if the flag value has a single use, don't
      // transform this.
      if (!VCMPoNode || VCMPoNode->hasNUsesOfValue(0, 1))
        break;
        
      // Look at the (necessarily single) use of the flag value.  If it has a 
      // chain, this transformation is more complex.  Note that multiple things
      // could use the value result, which we should ignore.
      SDNode *FlagUser = 0;
      for (SDNode::use_iterator UI = VCMPoNode->use_begin(); 
           FlagUser == 0; ++UI) {
        assert(UI != VCMPoNode->use_end() && "Didn't find user!");
        SDNode *User = UI->getUser();
        for (unsigned i = 0, e = User->getNumOperands(); i != e; ++i) {
          if (User->getOperand(i) == SDOperand(VCMPoNode, 1)) {
            FlagUser = User;
            break;
          }
        }
      }
      
      // If the user is a MFCR instruction, we know this is safe.  Otherwise we
      // give up for right now.
      if (FlagUser->getOpcode() == PPCISD::MFCR)
        return SDOperand(VCMPoNode, 0);
    }
    break;
  }
  case ISD::BR_CC: {
    // If this is a branch on an altivec predicate comparison, lower this so
    // that we don't have to do a MFCR: instead, branch directly on CR6.  This
    // lowering is done pre-legalize, because the legalizer lowers the predicate
    // compare down to code that is difficult to reassemble.
    ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(1))->get();
    SDOperand LHS = N->getOperand(2), RHS = N->getOperand(3);
    int CompareOpc;
    bool isDot;
    
    if (LHS.getOpcode() == ISD::INTRINSIC_WO_CHAIN &&
        isa<ConstantSDNode>(RHS) && (CC == ISD::SETEQ || CC == ISD::SETNE) &&
        getAltivecCompareInfo(LHS, CompareOpc, isDot)) {
      assert(isDot && "Can't compare against a vector result!");
      
      // If this is a comparison against something other than 0/1, then we know
      // that the condition is never/always true.
      unsigned Val = cast<ConstantSDNode>(RHS)->getValue();
      if (Val != 0 && Val != 1) {
        if (CC == ISD::SETEQ)      // Cond never true, remove branch.
          return N->getOperand(0);
        // Always !=, turn it into an unconditional branch.
        return DAG.getNode(ISD::BR, MVT::Other, 
                           N->getOperand(0), N->getOperand(4));
      }
    
      bool BranchOnWhenPredTrue = (CC == ISD::SETEQ) ^ (Val == 0);
      
      // Create the PPCISD altivec 'dot' comparison node.
      std::vector<MVT::ValueType> VTs;
      SDOperand Ops[] = {
        LHS.getOperand(2),  // LHS of compare
        LHS.getOperand(3),  // RHS of compare
        DAG.getConstant(CompareOpc, MVT::i32)
      };
      VTs.push_back(LHS.getOperand(2).getValueType());
      VTs.push_back(MVT::Flag);
      SDOperand CompNode = DAG.getNode(PPCISD::VCMPo, VTs, Ops, 3);
      
      // Unpack the result based on how the target uses it.
      PPC::Predicate CompOpc;
      switch (cast<ConstantSDNode>(LHS.getOperand(1))->getValue()) {
      default:  // Can't happen, don't crash on invalid number though.
      case 0:   // Branch on the value of the EQ bit of CR6.
        CompOpc = BranchOnWhenPredTrue ? PPC::PRED_EQ : PPC::PRED_NE;
        break;
      case 1:   // Branch on the inverted value of the EQ bit of CR6.
        CompOpc = BranchOnWhenPredTrue ? PPC::PRED_NE : PPC::PRED_EQ;
        break;
      case 2:   // Branch on the value of the LT bit of CR6.
        CompOpc = BranchOnWhenPredTrue ? PPC::PRED_LT : PPC::PRED_GE;
        break;
      case 3:   // Branch on the inverted value of the LT bit of CR6.
        CompOpc = BranchOnWhenPredTrue ? PPC::PRED_GE : PPC::PRED_LT;
        break;
      }

      return DAG.getNode(PPCISD::COND_BRANCH, MVT::Other, N->getOperand(0),
                         DAG.getConstant(CompOpc, MVT::i32),
                         DAG.getRegister(PPC::CR6, MVT::i32),
                         N->getOperand(4), CompNode.getValue(1));
    }
    break;
  }
  }
  
  return SDOperand();
}

//===----------------------------------------------------------------------===//
// Inline Assembly Support
//===----------------------------------------------------------------------===//

void PPCTargetLowering::computeMaskedBitsForTargetNode(const SDOperand Op,
                                                       const APInt &Mask,
                                                       APInt &KnownZero, 
                                                       APInt &KnownOne,
                                                       const SelectionDAG &DAG,
                                                       unsigned Depth) const {
  KnownZero = KnownOne = APInt(Mask.getBitWidth(), 0);
  switch (Op.getOpcode()) {
  default: break;
  case PPCISD::LBRX: {
    // lhbrx is known to have the top bits cleared out.
    if (cast<VTSDNode>(Op.getOperand(3))->getVT() == MVT::i16)
      KnownZero = 0xFFFF0000;
    break;
  }
  case ISD::INTRINSIC_WO_CHAIN: {
    switch (cast<ConstantSDNode>(Op.getOperand(0))->getValue()) {
    default: break;
    case Intrinsic::ppc_altivec_vcmpbfp_p:
    case Intrinsic::ppc_altivec_vcmpeqfp_p:
    case Intrinsic::ppc_altivec_vcmpequb_p:
    case Intrinsic::ppc_altivec_vcmpequh_p:
    case Intrinsic::ppc_altivec_vcmpequw_p:
    case Intrinsic::ppc_altivec_vcmpgefp_p:
    case Intrinsic::ppc_altivec_vcmpgtfp_p:
    case Intrinsic::ppc_altivec_vcmpgtsb_p:
    case Intrinsic::ppc_altivec_vcmpgtsh_p:
    case Intrinsic::ppc_altivec_vcmpgtsw_p:
    case Intrinsic::ppc_altivec_vcmpgtub_p:
    case Intrinsic::ppc_altivec_vcmpgtuh_p:
    case Intrinsic::ppc_altivec_vcmpgtuw_p:
      KnownZero = ~1U;  // All bits but the low one are known to be zero.
      break;
    }        
  }
  }
}


/// getConstraintType - Given a constraint, return the type of
/// constraint it is for this target.
PPCTargetLowering::ConstraintType 
PPCTargetLowering::getConstraintType(const std::string &Constraint) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    default: break;
    case 'b':
    case 'r':
    case 'f':
    case 'v':
    case 'y':
      return C_RegisterClass;
    }
  }
  return TargetLowering::getConstraintType(Constraint);
}

std::pair<unsigned, const TargetRegisterClass*> 
PPCTargetLowering::getRegForInlineAsmConstraint(const std::string &Constraint,
                                                MVT::ValueType VT) const {
  if (Constraint.size() == 1) {
    // GCC RS6000 Constraint Letters
    switch (Constraint[0]) {
    case 'b':   // R1-R31
    case 'r':   // R0-R31
      if (VT == MVT::i64 && PPCSubTarget.isPPC64())
        return std::make_pair(0U, PPC::G8RCRegisterClass);
      return std::make_pair(0U, PPC::GPRCRegisterClass);
    case 'f':
      if (VT == MVT::f32)
        return std::make_pair(0U, PPC::F4RCRegisterClass);
      else if (VT == MVT::f64)
        return std::make_pair(0U, PPC::F8RCRegisterClass);
      break;
    case 'v': 
      return std::make_pair(0U, PPC::VRRCRegisterClass);
    case 'y':   // crrc
      return std::make_pair(0U, PPC::CRRCRegisterClass);
    }
  }
  
  return TargetLowering::getRegForInlineAsmConstraint(Constraint, VT);
}


/// LowerAsmOperandForConstraint - Lower the specified operand into the Ops
/// vector.  If it is invalid, don't add anything to Ops.
void PPCTargetLowering::LowerAsmOperandForConstraint(SDOperand Op, char Letter,
                                                     std::vector<SDOperand>&Ops,
                                                     SelectionDAG &DAG) const {
  SDOperand Result(0,0);
  switch (Letter) {
  default: break;
  case 'I':
  case 'J':
  case 'K':
  case 'L':
  case 'M':
  case 'N':
  case 'O':
  case 'P': {
    ConstantSDNode *CST = dyn_cast<ConstantSDNode>(Op);
    if (!CST) return; // Must be an immediate to match.
    unsigned Value = CST->getValue();
    switch (Letter) {
    default: assert(0 && "Unknown constraint letter!");
    case 'I':  // "I" is a signed 16-bit constant.
      if ((short)Value == (int)Value)
        Result = DAG.getTargetConstant(Value, Op.getValueType());
      break;
    case 'J':  // "J" is a constant with only the high-order 16 bits nonzero.
    case 'L':  // "L" is a signed 16-bit constant shifted left 16 bits.
      if ((short)Value == 0)
        Result = DAG.getTargetConstant(Value, Op.getValueType());
      break;
    case 'K':  // "K" is a constant with only the low-order 16 bits nonzero.
      if ((Value >> 16) == 0)
        Result = DAG.getTargetConstant(Value, Op.getValueType());
      break;
    case 'M':  // "M" is a constant that is greater than 31.
      if (Value > 31)
        Result = DAG.getTargetConstant(Value, Op.getValueType());
      break;
    case 'N':  // "N" is a positive constant that is an exact power of two.
      if ((int)Value > 0 && isPowerOf2_32(Value))
        Result = DAG.getTargetConstant(Value, Op.getValueType());
      break;
    case 'O':  // "O" is the constant zero. 
      if (Value == 0)
        Result = DAG.getTargetConstant(Value, Op.getValueType());
      break;
    case 'P':  // "P" is a constant whose negation is a signed 16-bit constant.
      if ((short)-Value == (int)-Value)
        Result = DAG.getTargetConstant(Value, Op.getValueType());
      break;
    }
    break;
  }
  }
  
  if (Result.Val) {
    Ops.push_back(Result);
    return;
  }
  
  // Handle standard constraint letters.
  TargetLowering::LowerAsmOperandForConstraint(Op, Letter, Ops, DAG);
}

// isLegalAddressingMode - Return true if the addressing mode represented
// by AM is legal for this target, for a load/store of the specified type.
bool PPCTargetLowering::isLegalAddressingMode(const AddrMode &AM, 
                                              const Type *Ty) const {
  // FIXME: PPC does not allow r+i addressing modes for vectors!
  
  // PPC allows a sign-extended 16-bit immediate field.
  if (AM.BaseOffs <= -(1LL << 16) || AM.BaseOffs >= (1LL << 16)-1)
    return false;
  
  // No global is ever allowed as a base.
  if (AM.BaseGV)
    return false;
  
  // PPC only support r+r, 
  switch (AM.Scale) {
  case 0:  // "r+i" or just "i", depending on HasBaseReg.
    break;
  case 1:
    if (AM.HasBaseReg && AM.BaseOffs)  // "r+r+i" is not allowed.
      return false;
    // Otherwise we have r+r or r+i.
    break;
  case 2:
    if (AM.HasBaseReg || AM.BaseOffs)  // 2*r+r  or  2*r+i is not allowed.
      return false;
    // Allow 2*r as r+r.
    break;
  default:
    // No other scales are supported.
    return false;
  }
  
  return true;
}

/// isLegalAddressImmediate - Return true if the integer value can be used
/// as the offset of the target addressing mode for load / store of the
/// given type.
bool PPCTargetLowering::isLegalAddressImmediate(int64_t V,const Type *Ty) const{
  // PPC allows a sign-extended 16-bit immediate field.
  return (V > -(1 << 16) && V < (1 << 16)-1);
}

bool PPCTargetLowering::isLegalAddressImmediate(llvm::GlobalValue* GV) const {
  return false; 
}

SDOperand PPCTargetLowering::LowerRETURNADDR(SDOperand Op, SelectionDAG &DAG) {
  // Depths > 0 not supported yet! 
  if (cast<ConstantSDNode>(Op.getOperand(0))->getValue() > 0)
    return SDOperand();

  MachineFunction &MF = DAG.getMachineFunction();
  PPCFunctionInfo *FuncInfo = MF.getInfo<PPCFunctionInfo>();

  // Just load the return address off the stack.
  SDOperand RetAddrFI = getReturnAddrFrameIndex(DAG);

  // Make sure the function really does not optimize away the store of the RA
  // to the stack.
  FuncInfo->setLRStoreRequired();
  return DAG.getLoad(getPointerTy(), DAG.getEntryNode(), RetAddrFI, NULL, 0);
}

SDOperand PPCTargetLowering::LowerFRAMEADDR(SDOperand Op, SelectionDAG &DAG) {
  // Depths > 0 not supported yet! 
  if (cast<ConstantSDNode>(Op.getOperand(0))->getValue() > 0)
    return SDOperand();
  
  MVT::ValueType PtrVT = DAG.getTargetLoweringInfo().getPointerTy();
  bool isPPC64 = PtrVT == MVT::i64;
  
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  bool is31 = (NoFramePointerElim || MFI->hasVarSizedObjects()) 
                  && MFI->getStackSize();

  if (isPPC64)
    return DAG.getCopyFromReg(DAG.getEntryNode(), is31 ? PPC::X31 : PPC::X1,
      MVT::i64);
  else
    return DAG.getCopyFromReg(DAG.getEntryNode(), is31 ? PPC::R31 : PPC::R1,
      MVT::i32);
}
