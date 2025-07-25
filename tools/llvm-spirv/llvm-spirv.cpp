//===-- llvm-spirv.cpp - The LLVM/SPIR-V translator utility -----*- C++ -*-===//
//
//
//                     The LLVM/SPIRV Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
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
///  Common Usage:
///  llvm-spirv          - Read LLVM bitcode from stdin, write SPIR-V to stdout
///  llvm-spirv x.bc     - Read LLVM bitcode from the x.bc file, write SPIR-V
///                        to x.bil file
///  llvm-spirv -r       - Read SPIR-V from stdin, write LLVM bitcode to stdout
///  llvm-spirv -r x.bil - Read SPIR-V from the x.bil file, write SPIR-V to
///                        the x.bc file
///
///  Options:
///      --help   - Output command line options
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#ifdef LLVM_SPIRV_HAVE_SPIRV_TOOLS
#include "spirv-tools/libspirv.hpp"
#endif

#ifndef _SPIRV_SUPPORT_TEXT_FMT
#define _SPIRV_SUPPORT_TEXT_FMT
#endif

#include "LLVMSPIRVLib.h"

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#define DEBUG_TYPE "spirv"

namespace kExt {
const char SpirvBinary[] = ".spv";
const char SpirvText[] = ".spt";
const char LLVMBinary[] = ".bc";
} // namespace kExt

using namespace llvm;

static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<input file>"),
                                      cl::init("-"));

static cl::opt<std::string> OutputFile("o",
                                       cl::desc("Override output filename"),
                                       cl::value_desc("filename"));

static cl::opt<bool>
    IsReverse("r", cl::desc("Reverse translation (SPIR-V to LLVM)"));

static cl::opt<bool>
    IsRegularization("s",
                     cl::desc("Regularize LLVM to be representable by SPIR-V"));

using SPIRV::VersionNumber;

static cl::opt<VersionNumber> MaxSPIRVVersion(
    "spirv-max-version",
    cl::desc("Choose maximum SPIR-V version which can be emitted"),
    cl::values(clEnumValN(VersionNumber::SPIRV_1_0, "1.0", "SPIR-V 1.0"),
               clEnumValN(VersionNumber::SPIRV_1_1, "1.1", "SPIR-V 1.1"),
               clEnumValN(VersionNumber::SPIRV_1_2, "1.2", "SPIR-V 1.2"),
               clEnumValN(VersionNumber::SPIRV_1_3, "1.3", "SPIR-V 1.3"),
               clEnumValN(VersionNumber::SPIRV_1_4, "1.4", "SPIR-V 1.4"),
               clEnumValN(VersionNumber::SPIRV_1_5, "1.5", "SPIR-V 1.5"),
               clEnumValN(VersionNumber::SPIRV_1_6, "1.6", "SPIR-V 1.6")),
    cl::init(VersionNumber::MaximumVersion));

static cl::list<std::string> SPIRVAllowUnknownIntrinsics(
    "spirv-allow-unknown-intrinsics", cl::CommaSeparated,
    cl::desc("Unknown intrinsics that begin with any prefix from the "
             "comma-separated input list will be translated as external "
             "function calls in SPIR-V.\nLeaving any prefix unspecified "
             "(default) would naturally allow all unknown intrinsics"),
    cl::value_desc("intrinsic_prefix_1,intrinsic_prefix_2"), cl::ValueOptional);

static cl::opt<bool> SPIRVGenKernelArgNameMD(
    "spirv-gen-kernel-arg-name-md", cl::init(false),
    cl::desc("Enable generating OpenCL kernel argument name "
             "metadata"));

static cl::opt<SPIRV::ExtInst> ExtInst(
    "spirv-ext-inst",
    cl::desc("Specify the extended instruction set to use when "
             "translating from a LLVM intrinsic function to SPIR-V.  "
             "If none, some LLVM intrinsic functions will be emulated."),
    cl::values(clEnumValN(SPIRV::ExtInst::None, "none",
                          "No extended instructions"),
               clEnumValN(SPIRV::ExtInst::OpenCL, "OpenCL.std",
                          "OpenCL.std extended instruction set")),
    cl::init(SPIRV::ExtInst::None));

static cl::opt<SPIRV::BIsRepresentation> BIsRepresentation(
    "spirv-target-env",
    cl::desc("Specify a representation of different SPIR-V Instructions which "
             "is used when translating from SPIR-V to LLVM IR"),
    cl::values(
        clEnumValN(SPIRV::BIsRepresentation::OpenCL12, "CL1.2", "OpenCL C 1.2"),
        clEnumValN(SPIRV::BIsRepresentation::OpenCL20, "CL2.0", "OpenCL C 2.0"),
        clEnumValN(SPIRV::BIsRepresentation::SPIRVFriendlyIR, "SPV-IR",
                   "SPIR-V Friendly IR")),
    cl::init(SPIRV::BIsRepresentation::OpenCL12));

static cl::opt<bool> PreserveOCLKernelArgTypeMetadataThroughString(
    "preserve-ocl-kernel-arg-type-metadata-through-string", cl::init(false),
    cl::desc("Preserve OpenCL kernel_arg_type and kernel_arg_type_qual "
             "metadata through OpString"));

static cl::opt<bool>
    SPIRVToolsDis("spirv-tools-dis", cl::init(false),
                  cl::desc("Emit textual assembly using SPIRV-Tools"));

static cl::opt<bool> SPIRVEmitFunctionPtrAddrSpace(
    "spirv-emit-function-ptr-addr-space", cl::init(false),
    cl::desc("Emit and consume CodeSectionINTEL for function pointers"));

using SPIRV::ExtensionID;

#ifdef _SPIRV_SUPPORT_TEXT_FMT
namespace SPIRV {
// Use textual format for SPIRV.
extern bool SPIRVUseTextFormat;
} // namespace SPIRV

static cl::opt<bool>
    ToText("to-text",
           cl::desc("Convert input SPIR-V binary to internal textual format"));

static cl::opt<bool> ToBinary(
    "to-binary",
    cl::desc("Convert input SPIR-V in internal textual format to binary"));
#endif

static cl::opt<std::string> SpecConst(
    "spec-const",
    cl::desc("Translate SPIR-V to LLVM with constant specialization\n"
             "All ids must be valid specialization constant ids for the input "
             "SPIR-V module.\n"
             "The list of valid ids is available via -spec-const-info option.\n"
             "For duplicate ids the later one takes precedence.\n"
             "Float values may be represented in decimal or hexadecimal, hex "
             "values must be preceded by 0x.\n"
             "Supported types are: i1, i8, i16, i32, i64, f16, f32, f64.\n"),
    cl::value_desc("id1:type1:value1 id2:type2:value2 ..."));

static cl::opt<bool>
    SPIRVMemToReg("spirv-mem2reg", cl::init(false),
                  cl::desc("LLVM/SPIR-V translation enable mem2reg"));

static cl::opt<bool>
    SPIRVPreserveAuxData("spirv-preserve-auxdata", cl::init(false),
                         cl::desc("Preserve all auxiliary data, such as "
                                  "function attributes and metadata"));

static cl::opt<bool> SpecConstInfo(
    "spec-const-info",
    cl::desc("Display id of constants available for specializaion and their "
             "size in bytes"));

static cl::opt<bool>
    SPIRVPrintReport("spirv-print-report", cl::init(false),
                     cl::desc("Display general information about the module "
                              "(capabilities, extensions, version, memory model"
                              " and addressing model)"));

static cl::opt<SPIRV::FPContractMode> FPCMode(
    "spirv-fp-contract", cl::desc("Set FP Contraction mode:"),
    cl::init(SPIRV::FPContractMode::On),
    cl::values(
        clEnumValN(SPIRV::FPContractMode::On, "on",
                   "choose a mode according to presence of llvm.fmuladd "
                   "intrinsic or `contract' flag on fp operations"),
        clEnumValN(SPIRV::FPContractMode::Off, "off",
                   "disable FP contraction for all entry points"),
        clEnumValN(
            SPIRV::FPContractMode::Fast, "fast",
            "allow all operations to be contracted for all entry points")));

static cl::opt<bool> SPIRVAllowExtraDIExpressions(
    "spirv-allow-extra-diexpressions", cl::init(false),
    cl::desc("Allow DWARF operations not listed in the OpenCL.DebugInfo.100 "
             "specification (experimental, may produce incompatible SPIR-V "
             "module)"));

static cl::opt<SPIRV::DebugInfoEIS> DebugEIS(
    "spirv-debug-info-version", cl::desc("Set SPIR-V debug info version:"),
    cl::init(SPIRV::DebugInfoEIS::OpenCL_DebugInfo_100),
    cl::values(
        clEnumValN(SPIRV::DebugInfoEIS::SPIRV_Debug, "legacy",
                   "Emit debug info compliant with the SPIRV.debug extended "
                   "instruction set. This option is used for compatibility "
                   "with older versions of the translator"),
        clEnumValN(SPIRV::DebugInfoEIS::OpenCL_DebugInfo_100, "ocl-100",
                   "Emit debug info compliant with the OpenCL.DebugInfo.100 "
                   "extended instruction set. This version of SPIR-V debug "
                   "info format is compatible with the SPIRV-Tools"),
        clEnumValN(
            SPIRV::DebugInfoEIS::NonSemantic_Shader_DebugInfo_100,
            "nonsemantic-shader-100",
            "Emit debug info compliant with the "
            "NonSemantic.Shader.DebugInfo.100 extended instruction set. This "
            "version of SPIR-V debug info format is compatible with the rules "
            "regarding non-semantic instruction sets."),
        clEnumValN(
            SPIRV::DebugInfoEIS::NonSemantic_Shader_DebugInfo_200,
            "nonsemantic-shader-200",
            "Emit debug info compliant with the "
            "NonSemantic.Shader.DebugInfo.200 extended instruction set. This "
            "version of SPIR-V debug info format is compatible with the rules "
            "regarding non-semantic instruction sets.")));

static cl::opt<bool> SPIRVReplaceLLVMFmulAddWithOpenCLMad(
    "spirv-replace-fmuladd-with-ocl-mad",
    cl::desc("Allow replacement of llvm.fmuladd.* intrinsic with OpenCL mad "
             "instruction from OpenCL extended instruction set (deprecated)"),
    cl::init(true));

static cl::opt<SPIRV::BuiltinFormat> SPIRVBuiltinFormat(
    "spirv-builtin-format",
    cl::desc("Set LLVM-IR representation of SPIR-V builtin variables:"),
    cl::init(SPIRV::BuiltinFormat::Function),
    cl::values(
        clEnumValN(SPIRV::BuiltinFormat::Function, "function",
                   "Use functions to represent SPIR-V builtin variables"),
        clEnumValN(SPIRV::BuiltinFormat::Global, "global",
                   "Use globals to represent SPIR-V builtin variables")));

static cl::opt<bool> SPIRVUseLLVMSPIRVBackendTarget(
    "spirv-use-llvm-backend-target",
    cl::desc("Convert LLVM to SPIR-V using the LLVM SPIR-V Backend target if "
             "it's available. Otherwise has no effect. Default behavior is to "
             "don't use the LLVM SPIR-V Backend target."),
    cl::init(false));

static cl::opt<uint32_t> FnVarCategory(
    "fnvar-category",
    cl::desc("Specify architecture category of the target device (omitting "
             "this flag denotes that the target device can be of any "
             "category). Used only with -r and --fnvar-spec-enable."),
    cl::value_desc("category"), cl::ValueRequired);

static cl::opt<uint32_t> FnVarFamily(
    "fnvar-family",
    cl::desc("Specify architecture family of the target device (omitting this "
             "flag denotes that the target device can be of any family). Used "
             "only with -r and --fnvar-spec-enable."),
    cl::value_desc("family"), cl::ValueRequired);

static cl::opt<uint32_t> FnVarArch(
    "fnvar-arch",
    cl::desc("Specify architecture of the target device (omitting this flag "
             "denotes that the target device can be of any architecture). Used "
             "only with -r and --fnvar-spec-enable."),
    cl::value_desc("architecture"), cl::ValueRequired);

static cl::opt<uint32_t>
    FnVarTarget("fnvar-target",
                cl::desc("Specify target of the target device (omitting this "
                         "flag denotes that the target device can be any "
                         "target). Used only with -r and --fnvar-spec-enable."),
                cl::value_desc("target"), cl::ValueRequired);

static cl::list<uint32_t> FnVarFeatures(
    "fnvar-features", cl::CommaSeparated,
    cl::desc("Specify features of the target device (omitting this flag "
             "denotes that the target device supports all features). Used only "
             "with -r and --fnvar-spec-enable."),
    cl::value_desc("feature0,feature1,..."), cl::ValueRequired);

static cl::list<uint32_t> FnVarCapabilities(
    "fnvar-capabilities", cl::CommaSeparated,
    cl::desc("Specify capabilities of the target device (omitting this flag "
             "denotes that the target device supports all features). Used only "
             "with -r and --fnvar-spec-enable."),
    cl::value_desc("capability0,capability1,..."), cl::ValueRequired);

static cl::opt<std::string> FnVarSpvOut(
    "fnvar-spv-out",
    cl::desc("Save the specialized target-specific SPIR-V module to this file. "
             "Used only with -r and --fnvar-spec-enable."),
    cl::value_desc("file"), cl::ValueRequired);

static cl::opt<bool> FnVarSpecEnable(
    "fnvar-spec-enable", cl::init(false),
    cl::desc("Enable specialization of function variants according to "
             "SPV_INTEL_function_variants. Requires -r flag."));

static std::string removeExt(const std::string &FileName) {
  size_t Pos = FileName.find_last_of(".");
  if (Pos != std::string::npos)
    return FileName.substr(0, Pos);
  return FileName;
}

static ExitOnError ExitOnErr;

#ifdef LLVM_SPIRV_HAVE_SPIRV_TOOLS
/// Stream buffer that captures written data into a vector and allows reading
/// the data back as an array of uint32_t's.
class StreambufToArray : public std::streambuf {
public:
  const uint32_t *data() const {
    return reinterpret_cast<const uint32_t *>(Buffer.data());
  }

  size_t size() const { return Buffer.size() / sizeof(uint32_t); }

protected:
  std::streamsize xsputn(const char *s, std::streamsize count) override {
    for (std::streamsize I = 0; I < count; I++) {
      Buffer.push_back(s[I]);
    }
    return count;
  }

private:
  std::vector<char> Buffer;
};
#endif // LLVM_SPIRV_HAVE_SPIRV_TOOLS

static int convertLLVMToSPIRV(const SPIRV::TranslatorOpts &Opts) {
  LLVMContext Context;

  std::unique_ptr<MemoryBuffer> MB =
      ExitOnErr(errorOrToExpected(MemoryBuffer::getFileOrSTDIN(InputFile)));
  std::unique_ptr<Module> M =
      ExitOnErr(getOwningLazyBitcodeModule(std::move(MB), Context,
                                           /*ShouldLazyLoadMetadata=*/true));
  ExitOnErr(M->materializeAll());

  if (OutputFile.empty()) {
    if (InputFile == "-")
      OutputFile = "-";
    else
      OutputFile =
          removeExt(InputFile) +
          (SPIRV::SPIRVUseTextFormat ? kExt::SpirvText : kExt::SpirvBinary);
  }

  if (SPIRVToolsDis) {
#ifdef LLVM_SPIRV_HAVE_SPIRV_TOOLS
    auto DisMessagePrinter = [](spv_message_level_t Level, const char *source,
                                const spv_position_t &position,
                                const char *message) -> void {
      errs() << source << ": " << message << "\n";
    };
    spvtools::SpirvTools SpvTool(SPV_ENV_OPENCL_2_0);
    SpvTool.SetMessageConsumer(DisMessagePrinter);
    std::string DisOutput;

    // Serialize the SPIR-V module to a uint32_t array and then invoke the
    // SPIR-V tools disassembler to obtain textual assembly.
    std::string Err;
    StreambufToArray OutStreamBuf;
    std::ostream OutStream(&OutStreamBuf);
    bool Success = writeSpirv(M.get(), Opts, OutStream, Err);
    if (!Success) {
      errs() << "Failed to translate SPIR-V: " << Err << '\n';
      return -1;
    }

    if (!SpvTool.Disassemble(OutStreamBuf.data(), OutStreamBuf.size(),
                             &DisOutput)) {
      errs() << "Failed to generate textual assembly\n";
      return -1;
    }

    if (OutputFile != "-") {
      std::ofstream OutFile(OutputFile, std::ios::binary);
      OutFile << DisOutput;
    } else {
      std::cout << DisOutput;
    }

    return 0;
#else
    errs() << "llvm-spirv was built without --spirv-tools-dis support\n";
    return -1;
#endif // LLVM_SPIRV_HAVE_SPIRV_TOOLS
  }

  std::string Err;
  bool Success = false;
  if (OutputFile != "-") {
    std::ofstream OutFile(OutputFile, std::ios::binary);
    Success = writeSpirv(M.get(), Opts, OutFile, Err);
  } else {
    Success = writeSpirv(M.get(), Opts, std::cout, Err);
  }

  if (!Success) {
    errs() << "Fails to save LLVM as SPIR-V: " << Err << '\n';
    return -1;
  }
  return 0;
}

static bool isFileEmpty(const std::string &FileName) {
  std::ifstream File(FileName);
  return File && File.peek() == EOF;
}

static int convertSPIRVToLLVM(const SPIRV::TranslatorOpts &Opts) {
  LLVMContext Context;
  
  std::ifstream IFS(InputFile, std::ios::binary);
  Module *M;
  std::string Err;

  if (!readSpirv(Context, Opts, IFS, M, Err)) {
    errs() << "Fails to load SPIR-V as LLVM Module: " << Err << '\n';
    return -1;
  }

  LLVM_DEBUG(dbgs() << "Converted LLVM module:\n" << *M);

  raw_string_ostream ErrorOS(Err);
  if (verifyModule(*M, &ErrorOS)) {
    errs() << "Fails to verify module: " << ErrorOS.str();
    return -1;
  }

  if (OutputFile.empty()) {
    if (InputFile == "-")
      OutputFile = "-";
    else
      OutputFile = removeExt(InputFile) + kExt::LLVMBinary;
  }

  std::error_code EC;
  ToolOutputFile Out(OutputFile.c_str(), EC, sys::fs::OF_None);
  if (EC) {
    errs() << "Fails to open output file: " << EC.message();
    return -1;
  }

  WriteBitcodeToFile(*M, Out.os());
  Out.keep();
  delete M;
  return 0;
}

#ifdef _SPIRV_SUPPORT_TEXT_FMT
static int convertSPIRV() {
  if (ToBinary == ToText) {
    errs() << "Invalid arguments\n";
    return -1;
  }
  std::ifstream IFS(InputFile, std::ios::binary);

  if (OutputFile.empty()) {
    if (InputFile == "-")
      OutputFile = "-";
    else {
      OutputFile = removeExt(InputFile) +
                   (ToBinary ? kExt::SpirvBinary : kExt::SpirvText);
    }
  }

  auto Action = [&](std::ostream &OFS) {
    std::string Err;
    if (!SPIRV::convertSpirv(IFS, OFS, Err, ToBinary, ToText)) {
      errs() << "Fails to convert SPIR-V : " << Err << '\n';
      return -1;
    }
    return 0;
  };

  if (OutputFile == "-")
    return Action(std::cout);

  // Open the output file in binary mode in case we convert text to SPIRV binary
  if (ToBinary) {
    std::ofstream OFS(OutputFile, std::ios::binary);
    return Action(OFS);
  }

  // Convert SPIRV binary to text
  std::ofstream OFS(OutputFile);
  return Action(OFS);
}
#endif

static int regularizeLLVM(SPIRV::TranslatorOpts &Opts) {
  LLVMContext Context;

  std::unique_ptr<MemoryBuffer> MB =
      ExitOnErr(errorOrToExpected(MemoryBuffer::getFileOrSTDIN(InputFile)));
  std::unique_ptr<Module> M =
      ExitOnErr(getOwningLazyBitcodeModule(std::move(MB), Context,
                                           /*ShouldLazyLoadMetadata=*/true));
  ExitOnErr(M->materializeAll());

  if (OutputFile.empty()) {
    if (InputFile == "-")
      OutputFile = "-";
    else
      OutputFile = removeExt(InputFile) + ".regularized.bc";
  }

  std::string Err;
  if (!regularizeLlvmForSpirv(M.get(), Err, Opts)) {
    errs() << "Fails to save LLVM as SPIR-V: " << Err << '\n';
    return -1;
  }

  std::error_code EC;
  ToolOutputFile Out(OutputFile.c_str(), EC, sys::fs::OF_None);
  if (EC) {
    errs() << "Fails to open output file: " << EC.message();
    return -1;
  }

  WriteBitcodeToFile(*M.get(), Out.os());
  Out.keep();
  return 0;
}

static int parseSPVExtOption(
    cl::list<std::string> &SPVExtList,
    SPIRV::TranslatorOpts::ExtensionsStatusMap &ExtensionsStatus) {
  // Map name -> id for known extensions
  std::map<std::string, ExtensionID> ExtensionNamesMap;
#define _STRINGIFY(X) #X
#define STRINGIFY(X) _STRINGIFY(X)
#define EXT(X) ExtensionNamesMap[STRINGIFY(X)] = ExtensionID::X;
#include "LLVMSPIRVExtensions.inc"
#undef EXT
#undef STRINGIFY
#undef _STRINGIFY

  // Set the initial state:
  //  - during SPIR-V consumption, assume that any known extension is allowed.
  //  - during SPIR-V generation, assume that any known extension is disallowed.
  //  - during conversion to/from SPIR-V text representation, assume that any
  //    known extension is allowed.
  std::optional<bool> DefaultVal;
  if (IsReverse)
    DefaultVal = true;
  for (const auto &It : ExtensionNamesMap)
    ExtensionsStatus[It.second] = DefaultVal;

  if (SPVExtList.empty())
    return 0; // Nothing to do

  for (unsigned i = 0; i < SPVExtList.size(); ++i) {
    const std::string &ExtString = SPVExtList[i];
    if (ExtString.empty() ||
        ('+' != ExtString.front() && '-' != ExtString.front())) {
      errs() << "Invalid value of --spirv-ext, expected format is:\n"
             << "\t--spirv-ext=+EXT_NAME,-EXT_NAME\n";
      return -1;
    }

    auto ExtName = ExtString.substr(1);

    if (ExtName.empty()) {
      errs() << "Invalid value of --spirv-ext, expected format is:\n"
             << "\t--spirv-ext=+EXT_NAME,-EXT_NAME\n";
      return -1;
    }

    bool ExtStatus = ('+' == ExtString.front());
    if ("all" == ExtName) {
      // Update status for all known extensions
      for (const auto &It : ExtensionNamesMap)
        ExtensionsStatus[It.second] = ExtStatus;
    } else {
      // Reject unknown extensions
      const auto &It = ExtensionNamesMap.find(ExtName);
      if (ExtensionNamesMap.end() == It) {
        errs() << "Unknown extension '" << ExtName << "' was specified via "
               << "--spirv-ext option\n";
        return -1;
      }

      ExtensionsStatus[It->second] = ExtStatus;
    }
  }

  return 0;
}

// Returns true on error.
bool parseSpecConstOpt(llvm::StringRef SpecConstStr,
                       SPIRV::TranslatorOpts &Opts) {
  std::ifstream IFS(InputFile, std::ios::binary);
  std::vector<SpecConstInfoTy> SpecConstInfo;
  if (!getSpecConstInfo(IFS, SpecConstInfo))
    return true;

  SmallVector<StringRef, 8> Split;
  SpecConstStr.split(Split, ' ', -1, false);
  for (StringRef Option : Split) {
    SmallVector<StringRef, 4> Params;
    Option.split(Params, ':', 2);
    if (Params.size() < 3) {
      errs() << "Error: Invalid format of -" << SpecConst.ArgStr
             << " option: \"" << Option << "\". Expected format: -"
             << SpecConst.ArgStr << " \"<" << SpecConst.ValueStr << ">\"\n";
      return true;
    }
    uint32_t SpecId;
    if (Params[0].getAsInteger(10, SpecId)) {
      errs() << "Error: Invalid id for '-" << SpecConst.ArgStr
             << "' option! In \"" << Option << "\": \"" << Params[0]
             << "\" must be a 32-bit unsigned integer\n";
      return true;
    }
    auto It =
        std::find_if(SpecConstInfo.begin(), SpecConstInfo.end(),
                     [=](SpecConstInfoTy Info) { return Info.ID == SpecId; });
    if (It == SpecConstInfo.end()) {
      errs() << "Error: CL_INVALID_SPEC_ID. \"" << Option << "\": There is no "
             << "specialization constant with id = " << SpecId
             << " in the SPIR-V module.";
      return true;
    }
    if (Params[1].consume_front("i")) {
      unsigned Width = 0;
      Params[1].getAsInteger(10, Width);
      if (!isPowerOf2_32(Width) || Width > 64) {
        errs() << "Error: Invalid type for '-" << SpecConst.ArgStr
               << "' option! In \"" << Option << "\": \"i" << Params[1]
               << "\" - is not allowed type. "
               << "Allowed types are: i1, i8, i16, i32, i64, f16, f32, f64\n";
        return true;
      }
      size_t Size = Width < 8 ? 1 : Width / 8;
      if (Size != It->Size) {
        errs() << "Error: CL_INVALID_VALUE. In \"" << Option << "\": Size of "
               << "type i" << Width << " (" << Size << " bytes) "
               << "does not match the size of the specialization constant "
               << "in the module (" << It->Size << " bytes)\n";
        return true;
      }
      APInt Value;
      bool Err = Params[2].getAsInteger(10, Value);
      if (Err || Value.getActiveWords() > 1 ||
          (Width < 64 && Value.getZExtValue() >> Width)) {
        errs() << "Error: Invalid value for '-" << SpecConst.ArgStr
               << "' option! In \"" << Option << "\": can't convert \""
               << Params[2] << "\" to " << Width << "-bit integer number\n";
        return true;
      }
      Opts.setSpecConst(SpecId, Value.getZExtValue());
    } else if (Params[1].consume_front("f")) {
      unsigned Width = 0;
      Params[1].getAsInteger(10, Width);
      const llvm::fltSemantics *FS = nullptr;
      switch (Width) {
      case 16:
        FS = &APFloat::IEEEhalf();
        break;
      case 32:
        FS = &APFloat::IEEEsingle();
        break;
      case 64:
        FS = &APFloat::IEEEdouble();
        break;
      default:
        errs() << "Error: Invalid type for '-" << SpecConst.ArgStr
               << "' option! In \"" << Option << "\": \"f" << Params[1]
               << "\" - is not allowed type. "
               << "Allowed types are: i1, i8, i16, i32, i64, f16, f32, f64\n";
        return true;
      }
      APFloat Value(*FS);
      if (Params[2].find("0x") != StringRef::npos) {
        std::stringstream paramStream;
        paramStream << std::hex << Params[2].data();
        uint64_t specVal = 0;
        paramStream >> specVal;
        Opts.setSpecConst(SpecId, specVal);
      } else {
        Expected<APFloat::opStatus> StatusOrErr =
            Value.convertFromString(Params[2], APFloat::rmNearestTiesToEven);
        if (!StatusOrErr) {
          return true;
        }
        // It's ok to have inexact conversion from decimal representation.
        APFloat::opStatus Status = *StatusOrErr;
        if (Status & ~APFloat::opInexact) {
          errs() << "Error: Invalid value for '-" << SpecConst.ArgStr
                 << "' option! In \"" << Option << "\": can't convert \""
                 << Params[2] << "\" to " << Width
                 << "-bit floating point number\n";
          return true;
        }
        Opts.setSpecConst(SpecId, Value.bitcastToAPInt().getZExtValue());
      }
    } else {
      errs() << "Error: Invalid type for '-" << SpecConst.ArgStr
             << "' option! In \"" << Option << "\": \"" << Params[1]
             << "\" - is not allowed type. "
             << "Allowed types are: i1, i8, i16, i32, i64, f16, f32, f64\n";
      return true;
    }
  }
  return false;
}

static void parseAllowUnknownIntrinsicsOpt(SPIRV::TranslatorOpts &Opts) {
  SPIRV::TranslatorOpts::ArgList PrefixList;
  for (const auto &Prefix : SPIRVAllowUnknownIntrinsics) {
    PrefixList.push_back(Prefix);
  }
  Opts.setSPIRVAllowUnknownIntrinsics(PrefixList);
}

int main(int Ac, char **Av) {
  EnablePrettyStackTrace();
  sys::PrintStackTraceOnErrorSignal(Av[0]);
  PrettyStackTraceProgram X(Ac, Av);

#if defined(LLVM_SPIRV_BACKEND_TARGET_PRESENT)
  // SPIR-V Backend is available, and so we have a clash of command line
  // argument names, because both products use "spirv-ext" name. Let's rename
  // the command line option coming from SPIR-V Backend, as it's not supposed to
  // be used by a user anyway. After that we may safely add the instance of
  // "spirv-ext" required by LLVM/SPIRV Translator from the corresponding auto
  // variable (SPVExt).
  StringMap<llvm::cl::Option *> &RegisteredOptions =
      llvm::cl::getRegisteredOptions();
  if (RegisteredOptions.count("spirv-ext") == 1) {
    llvm::cl::Option *OptToDisable = RegisteredOptions["spirv-ext"];
    OptToDisable->setArgStr("spirv-ext-coming-from-spirv-backend");
    OptToDisable->setHiddenFlag(cl::Hidden);
  }
#endif
  cl::list<std::string> SPVExt(
      "spirv-ext", cl::CommaSeparated,
      cl::desc("Specify list of allowed/disallowed extensions"),
      cl::value_desc("+SPV_extenstion1_name,-SPV_extension2_name"),
      cl::ValueRequired);

  cl::ParseCommandLineOptions(Ac, Av, "LLVM/SPIR-V translator");

  if (InputFile != "-" && isFileEmpty(InputFile)) {
    errs() << "Can't translate, file is empty\n";
    return -1;
  }

  SPIRV::TranslatorOpts::ExtensionsStatusMap ExtensionsStatus;
  // ExtensionsStatus will be properly initialized and update according to
  // values passed via --spirv-ext option in parseSPVExtOption function.
  int Ret = parseSPVExtOption(SPVExt, ExtensionsStatus);
  if (0 != Ret)
    return Ret;

  SPIRV::TranslatorOpts Opts(MaxSPIRVVersion, ExtensionsStatus);
#if defined(LLVM_SPIRV_BACKEND_TARGET_PRESENT)
  Opts.setUseLLVMTarget(SPIRVUseLLVMSPIRVBackendTarget);
#endif

  if (ExtInst.getNumOccurrences() != 0) {
    if (ExtInst.getNumOccurrences() > 1) {
      errs() << "Error: --spirv-ext-inst cannot be used more than once\n";
      return -1;
    } else if (SPIRVReplaceLLVMFmulAddWithOpenCLMad.getNumOccurrences()) {
      errs()
          << "Error: --spirv-ext-inst and --spirv-replace-fmuladd-with-ocl-mad "
             "cannot be used together.  --spirv-replace-fmuladd-with-ocl-mad "
             "is deprecated and --spirv-ext-inst is preferred.\n";
      return -1;
    } else if (IsReverse) {
      errs() << "Note: --spirv-ext-inst option ignored as it only "
                "affects translation from LLVM IR to SPIR-V";
    }
    Opts.setExtInst(ExtInst);
  }

  if (BIsRepresentation.getNumOccurrences() != 0) {
    if (!IsReverse) {
      errs() << "Note: --spirv-target-env option ignored as it only "
                "affects translation from SPIR-V to LLVM IR";
    } else {
      Opts.setDesiredBIsRepresentation(BIsRepresentation);
    }
  }

  Opts.setFPContractMode(FPCMode);

  if (SPIRVBuiltinFormat.getNumOccurrences() != 0) {
    if (!IsReverse) {
      errs() << "Note: --spirv-builtin-format option ignored as it only "
                "affects translation from SPIR-V to LLVM IR";
    } else {
      Opts.setBuiltinFormat(SPIRVBuiltinFormat);
    }
  }

  if (SPIRVMemToReg)
    Opts.setMemToRegEnabled(SPIRVMemToReg);
  if (SPIRVGenKernelArgNameMD)
    Opts.setGenKernelArgNameMDEnabled(SPIRVGenKernelArgNameMD);
  if (IsReverse && !SpecConst.empty()) {
    if (parseSpecConstOpt(SpecConst, Opts))
      return -1;
  }

  if (SPIRVPreserveAuxData) {
    Opts.setPreserveAuxData(SPIRVPreserveAuxData);
    if (!IsReverse)
      Opts.setAllowedToUseExtension(
          SPIRV::ExtensionID::SPV_KHR_non_semantic_info);
  }

  if (SPIRVAllowUnknownIntrinsics.getNumOccurrences() != 0) {
    if (IsReverse) {
      errs()
          << "Note: --spirv-allow-unknown-intrinsics option ignored as it only "
             "affects translation from LLVM IR to SPIR-V";
    } else {
      parseAllowUnknownIntrinsicsOpt(Opts);
    }
  }

  if (SPIRVReplaceLLVMFmulAddWithOpenCLMad.getNumOccurrences() != 0) {
    if (IsReverse) {
      errs() << "Note: --spirv-replace-fmuladd-with-ocl-mad option ignored as "
                "it only affects translation from LLVM IR to SPIR-V";
    } else {
      Opts.setReplaceLLVMFmulAddWithOpenCLMad(
          SPIRVReplaceLLVMFmulAddWithOpenCLMad);
    }
  }

  if (SPIRVAllowExtraDIExpressions.getNumOccurrences() != 0) {
    Opts.setAllowExtraDIExpressionsEnabled(SPIRVAllowExtraDIExpressions);
  }

  if (DebugEIS.getNumOccurrences() != 0) {
    if (IsReverse) {
      errs() << "Note: --spirv-debug-info-version option ignored as it only "
                "affects translation from LLVM IR to SPIR-V";
    } else {
      Opts.setDebugInfoEIS(DebugEIS);
      if (DebugEIS.getValue() ==
          SPIRV::DebugInfoEIS::NonSemantic_Shader_DebugInfo_200)
        Opts.setAllowExtraDIExpressionsEnabled(true);
      if (DebugEIS.getValue() ==
              SPIRV::DebugInfoEIS::NonSemantic_Shader_DebugInfo_100 ||
          DebugEIS.getValue() ==
              SPIRV::DebugInfoEIS::NonSemantic_Shader_DebugInfo_200)
        Opts.setAllowedToUseExtension(
            SPIRV::ExtensionID::SPV_KHR_non_semantic_info);
    }
  }

  if (PreserveOCLKernelArgTypeMetadataThroughString.getNumOccurrences() != 0)
    Opts.setPreserveOCLKernelArgTypeMetadataThroughString(true);

  if (SPIRVEmitFunctionPtrAddrSpace.getNumOccurrences() != 0)
    Opts.setEmitFunctionPtrAddrSpace(true);

  Opts.setFnVarSpecEnable(FnVarSpecEnable);

  if (!IsReverse &&
      (FnVarSpecEnable || FnVarCategory != 0 || FnVarFamily != 0 ||
       FnVarArch != 0 || FnVarTarget != 0 || !FnVarFeatures.empty() ||
       !FnVarCapabilities.empty() || !FnVarSpvOut.empty())) {
    errs() << "--fnvar-xxx flags can be used only with -r\n";
    return -1;
  }

  if (!FnVarSpecEnable &&
      (FnVarCategory != 0 || FnVarFamily != 0 || FnVarArch != 0 ||
       FnVarTarget != 0 || !FnVarFeatures.empty() ||
       !FnVarCapabilities.empty() || !FnVarSpvOut.empty())) {
    errs() << "--fnvar-xxx flags need to be enabled with --fnvar-spec-enable\n";
    return -1;
  }

  if (FnVarCategory.getNumOccurrences() > 0) {
    Opts.setFnVarCategory(FnVarCategory);
  }

  if (FnVarFamily.getNumOccurrences() > 0) {
    Opts.setFnVarFamily(FnVarFamily);
  }

  if (FnVarArch.getNumOccurrences() > 0) {
    Opts.setFnVarArch(FnVarArch);
  }

  if (FnVarTarget.getNumOccurrences() > 0) {
    Opts.setFnVarTarget(FnVarTarget);
  }

  if (!FnVarFeatures.empty()) {
    Opts.setFnVarFeatures(FnVarFeatures);
  }

  if (!FnVarCapabilities.empty()) {
    Opts.setFnVarCapabilities(FnVarCapabilities);
  }

  if (!FnVarSpvOut.empty()) {
    Opts.setFnVarSpvOut(FnVarSpvOut);
  }

  if (!Opts.validateFnVarOpts()) {
    return -1;
  }

#ifdef _SPIRV_SUPPORT_TEXT_FMT
  if (ToText && (ToBinary || IsReverse || IsRegularization)) {
    errs() << "Cannot use -to-text with -to-binary, -r, -s\n";
    return -1;
  }

  if (ToBinary && (ToText || IsReverse || IsRegularization)) {
    errs() << "Cannot use -to-binary with -to-text, -r, -s\n";
    return -1;
  }

  if (ToBinary || ToText)
    return convertSPIRV();
#endif

  if (!IsReverse && !IsRegularization && !SpecConstInfo && !SPIRVPrintReport)
    return convertLLVMToSPIRV(Opts);

  if (IsReverse && IsRegularization) {
    errs() << "Cannot have both -r and -s options\n";
    return -1;
  }
  if (IsReverse)
    return convertSPIRVToLLVM(Opts);

  if (IsRegularization)
    return regularizeLLVM(Opts);

  if (SpecConstInfo) {
    std::ifstream IFS(InputFile, std::ios::binary);
    std::vector<SpecConstInfoTy> SpecConstInfo;
    if (!getSpecConstInfo(IFS, SpecConstInfo)) {
      std::cout << "Invalid SPIR-V binary";
      return -1;
    }
    std::cout << "Number of scalar specialization constants in the module = "
              << SpecConstInfo.size() << "\n";
    for (auto &SpecConst : SpecConstInfo)
      std::cout << "Spec const id = " << SpecConst.ID
                << ", size in bytes = " << SpecConst.Size
                << ", type = " << SpecConst.Type << "\n";
  }

  if (SPIRVPrintReport) {
    std::ifstream IFS(InputFile, std::ios::binary);
    int ErrCode = 0;
    std::optional<SPIRV::SPIRVModuleReport> BinReport =
        SPIRV::getSpirvReport(IFS, ErrCode);
    if (!BinReport) {
      std::cerr << "Invalid SPIR-V binary: \""
                << SPIRV::getErrorMessage(ErrCode) << "\"\n";
      return -1;
    }

    SPIRV::SPIRVModuleTextReport TextReport =
        SPIRV::formatSpirvReport(BinReport.value());

    std::cout << "SPIR-V module report:" << "\n Version: " << TextReport.Version
              << "\n Memory model: " << TextReport.MemoryModel
              << "\n Addressing model: " << TextReport.AddrModel << "\n";

    std::cout << " Number of capabilities: " << TextReport.Capabilities.size()
              << "\n";
    for (auto &Capability : TextReport.Capabilities)
      std::cout << "  Capability: " << Capability << "\n";

    std::cout << " Number of extensions: " << TextReport.Extensions.size()
              << "\n";
    for (auto &Extension : TextReport.Extensions)
      std::cout << "  Extension: " << Extension << "\n";

    std::cout << " Number of extended instruction sets: "
              << TextReport.ExtendedInstructionSets.size() << "\n";
    for (auto &ExtendedInstructionSet : TextReport.ExtendedInstructionSets)
      std::cout << "  Extended Instruction Set: " << ExtendedInstructionSet
                << "\n";
  }
  return 0;
}
