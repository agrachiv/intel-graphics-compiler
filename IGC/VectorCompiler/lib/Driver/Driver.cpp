/*========================== begin_copyright_notice ============================

Copyright (C) 2020-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "SPIRVWrapper.h"
#include "vc/Support/PassManager.h"

#include "vc/Driver/Driver.h"

#include "igc/Options/Options.h"
#include "vc/GenXCodeGen/GenXOCLRuntimeInfo.h"
#include "vc/GenXCodeGen/GenXTarget.h"
#include "vc/GenXOpts/GenXOpts.h"
#include "vc/GenXOpts/Utils/KernelInfo.h"
#include "vc/Support/BackendConfig.h"
#include "vc/Support/Status.h"
#include "llvm/GenXIntrinsics/GenXIntrOpts.h"
#include "llvm/GenXIntrinsics/GenXIntrinsics.h"
#include "llvm/GenXIntrinsics/GenXSPIRVReaderAdaptor.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"

#include "llvmWrapper/Option/OptTable.h"
#include "llvmWrapper/Target/TargetMachine.h"

#include "Probe/Assertion.h"

#include <cctype>
#include <memory>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;

static Expected<std::unique_ptr<llvm::Module>>
getModuleFromLLVMText(ArrayRef<char> Input, LLVMContext &C) {
  SMDiagnostic Err;
  llvm::MemoryBufferRef BufferRef(llvm::StringRef(Input.data(), Input.size()),
                                  "LLVM IR Module");
  Expected<std::unique_ptr<llvm::Module>> ExpModule =
      llvm::parseIR(BufferRef, Err, C);

  if (!ExpModule)
    Err.print("getModuleLL", errs());

  if (verifyModule(*ExpModule.get()))
    return make_error<vc::InvalidModuleError>();

  return ExpModule;
}

static Expected<std::unique_ptr<llvm::Module>>
getModuleFromLLVMBinary(ArrayRef<char> Input, LLVMContext& C) {

  llvm::MemoryBufferRef BufferRef(llvm::StringRef(Input.data(), Input.size()),
    "Deserialized LLVM Module");
  auto ExpModule = llvm::parseBitcodeFile(BufferRef, C);

  if (!ExpModule)
    return llvm::handleExpected(
        std::move(ExpModule),
        []() -> llvm::Error {
          IGC_ASSERT_EXIT_MESSAGE(0, "Should create new error");
        },
        [](const llvm::ErrorInfoBase& E) {
          return make_error<vc::BadBitcodeError>(E.message());
        });

  if (verifyModule(*ExpModule.get()))
    return make_error<vc::InvalidModuleError>();

  return ExpModule;
}

static Expected<std::unique_ptr<llvm::Module>>
getModuleFromSPIRV(ArrayRef<char> Input, ArrayRef<uint32_t> SpecConstIds,
                   ArrayRef<uint64_t> SpecConstValues, LLVMContext &Ctx) {
  auto ExpIR = vc::translateSPIRVToIR(Input, SpecConstIds, SpecConstValues);
  if (!ExpIR)
    return ExpIR.takeError();

  return getModuleFromLLVMBinary(ExpIR.get(), Ctx);
}

static Expected<std::unique_ptr<llvm::Module>>
getModule(ArrayRef<char> Input, vc::FileType FType,
          ArrayRef<uint32_t> SpecConstIds, ArrayRef<uint64_t> SpecConstValues,
          LLVMContext &Ctx) {
  switch (FType) {
  case vc::FileType::SPIRV:
    return getModuleFromSPIRV(Input, SpecConstIds, SpecConstValues, Ctx);
  case vc::FileType::LLVM_TEXT:
    return getModuleFromLLVMText(Input, Ctx);
  case vc::FileType::LLVM_BINARY:
    return getModuleFromLLVMBinary(Input, Ctx);
  }
  IGC_ASSERT_EXIT_MESSAGE(0, "Unknown input kind");
}

static Triple overrideTripleWithVC(StringRef TripleStr) {
  Triple T{TripleStr};
  // Normalize triple.
  bool Is32Bit = T.isArch32Bit();
  if (TripleStr.startswith("genx32"))
    Is32Bit = true;
  return Triple{Is32Bit ? "genx32-unknown-unknown" : "genx64-unknown-unknown"};
}

static std::string getSubtargetFeatureString(const vc::CompileOptions &Opts) {

  SubtargetFeatures Features;

  if (!Opts.FeaturesString.empty()) {
    SmallVector<StringRef, 8> AuxFeatures;
    StringRef(Opts.FeaturesString).split(AuxFeatures, ",", -1, false);
    for (const auto& F: AuxFeatures) {
      auto Feature = F.trim();
      bool Enabled = Feature.consume_front("+");
      if (!Enabled) {
        bool Disabled = Feature.consume_front("-");
        IGC_ASSERT_MESSAGE(Disabled, "unexpected feature format");
      }
      Features.AddFeature(Feature.str(), Enabled);
    }
  }
  if (Opts.HasL1ReadOnlyCache)
    Features.AddFeature("has_l1_read_only_cache");
  if (Opts.HasLocalMemFenceSupress)
    Features.AddFeature("supress_local_mem_fence");
  if (Opts.NoVecDecomp)
    Features.AddFeature("disable_vec_decomp");
  if (Opts.NoJumpTables)
    Features.AddFeature("disable_jump_tables");
  if (Opts.TranslateLegacyMemoryIntrinsics)
    Features.AddFeature("translate_legacy_message");
  if (Opts.Binary == vc::BinaryKind::OpenCL ||
      Opts.Binary == vc::BinaryKind::ZE)
    Features.AddFeature("ocl_runtime");

  return Features.getString();
}

static CodeGenOpt::Level getCodeGenOptLevel(const vc::CompileOptions &Opts) {
  if (Opts.OptLevel == vc::OptimizerLevel::None)
    return CodeGenOpt::None;
  return CodeGenOpt::Default;
}

static TargetOptions getTargetOptions(const vc::CompileOptions &Opts) {
  TargetOptions Options;
  Options.AllowFPOpFusion = Opts.AllowFPOpFusion;
  return Options;
}

static Expected<std::unique_ptr<TargetMachine>>
createTargetMachine(const vc::CompileOptions &Opts, Triple &TheTriple) {
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget(
      TheTriple.getArchName().str(), TheTriple, Error);
  IGC_ASSERT_MESSAGE(TheTarget, "vc target was not registered");

  const std::string FeaturesStr = getSubtargetFeatureString(Opts);

  const TargetOptions Options = getTargetOptions(Opts);

  CodeGenOpt::Level OptLevel = getCodeGenOptLevel(Opts);
  std::unique_ptr<TargetMachine> TM{
      TheTarget->createTargetMachine(TheTriple.getTriple(), Opts.CPUStr,
                                     FeaturesStr, Options, /*RelocModel=*/None,
                                     /*CodeModel=*/None, OptLevel)};
  if (!TM)
    return make_error<vc::TargetMachineError>();
  return {std::move(TM)};
}

template <typename T> bool getDefaultOverridableFlag(T OptFlag, bool Default) {
  switch (OptFlag) {
  default:
    return Default;
  case T::Enable:
    return true;
  case T::Disable:
    return false;
  }
}

// Create backend options for immutable config pass. Override default
// values with provided ones.
static GenXBackendOptions createBackendOptions(const vc::CompileOptions &Opts) {
  GenXBackendOptions BackendOpts;
  if (Opts.StackMemSize) {
    BackendOpts.StackSurfaceMaxSize = Opts.StackMemSize.getValue();
    BackendOpts.StatelessPrivateMemSize = Opts.StackMemSize.getValue();
  }

  BackendOpts.DebuggabilityEmitDebuggableKernels = Opts.EmitDebuggableKernels;
  BackendOpts.DebuggabilityForLegacyPath =
      (Opts.Binary != vc::BinaryKind::CM) && Opts.EmitDebuggableKernels;
  BackendOpts.DebuggabilityZeBinCompatibleDWARF =
      (Opts.Binary == vc::BinaryKind::ZE);
  BackendOpts.DebuggabilityEmitBreakpoints = Opts.EmitExtendedDebug;
  bool IsOptLevel_O0 =
      Opts.OptLevel == vc::OptimizerLevel::None && Opts.EmitExtendedDebug;
  BackendOpts.DebuggabilityExtendedDebug =
      getDefaultOverridableFlag(Opts.NoOptFinalizerMode, IsOptLevel_O0);

  BackendOpts.DebuggabilityValidateDWARF = Opts.ForceDebugInfoValidation;

  BackendOpts.DisableFinalizerMsg = Opts.DisableFinalizerMsg;
  BackendOpts.EnableAsmDumps = Opts.DumpAsm;
  BackendOpts.EnableDebugInfoDumps = Opts.DumpDebugInfo;
  BackendOpts.Dumper = Opts.Dumper.get();
  BackendOpts.ShaderOverrider = Opts.ShaderOverrider.get();
  BackendOpts.DisableStructSplitting = Opts.DisableStructSplitting;
  BackendOpts.ForceArrayPromotion = (Opts.Binary == vc::BinaryKind::CM);
  if (Opts.ForceLiveRangesLocalizationForAccUsage)
    BackendOpts.LocalizeLRsForAccUsage = true;
  if (Opts.ForceDisableNonOverlappingRegionOpt)
    BackendOpts.DisableNonOverlappingRegionOpt = true;
  BackendOpts.FCtrl = Opts.FCtrl;
  BackendOpts.WATable = Opts.WATable;
  BackendOpts.IsLargeGRFMode = Opts.IsLargeGRFMode;
  BackendOpts.UseBindlessBuffers = Opts.UseBindlessBuffers;
  if (Opts.SaveStackCallLinkage)
    BackendOpts.SaveStackCallLinkage = true;
  BackendOpts.UsePlain2DImages = Opts.UsePlain2DImages;
  BackendOpts.EnablePreemption = Opts.EnablePreemption;

  BackendOpts.DisableLiveRangesCoalescing =
      getDefaultOverridableFlag(Opts.DisableLRCoalescingMode, false);
  return BackendOpts;
}

static GenXBackendData createBackendData(const vc::ExternalData &Data,
                                         int PointerSizeInBits) {
  IGC_ASSERT_MESSAGE(PointerSizeInBits == 32 || PointerSizeInBits == 64,
      "only 32 and 64 bit pointers are expected");
  GenXBackendData BackendData;
  BackendData.BiFModule[BiFKind::OCLGeneric] =
      IGCLLVM::makeMemoryBufferRef(*Data.OCLGenericBIFModule);
  BackendData.BiFModule[BiFKind::VCEmulation] =
      IGCLLVM::makeMemoryBufferRef(*Data.VCEmulationBIFModule);
  BackendData.BiFModule[BiFKind::VCSPIRVBuiltins] =
      IGCLLVM::makeMemoryBufferRef(*Data.VCSPIRVBuiltinsBIFModule);
  if (PointerSizeInBits == 64)
    BackendData.BiFModule[BiFKind::VCPrintf] =
        IGCLLVM::makeMemoryBufferRef(*Data.VCPrintf64BIFModule);
  else
    BackendData.BiFModule[BiFKind::VCPrintf] =
        IGCLLVM::makeMemoryBufferRef(*Data.VCPrintf32BIFModule);
  return std::move(BackendData);
}

static void optimizeIR(const vc::CompileOptions &Opts,
                       const vc::ExternalData &ExtData, TargetMachine &TM,
                       Module &M) {
  vc::PassManager PerModulePasses;
  legacy::FunctionPassManager PerFunctionPasses(&M);

  PerModulePasses.add(
      createTargetTransformInfoWrapperPass(TM.getTargetIRAnalysis()));
  PerModulePasses.add(new GenXBackendConfig{createBackendOptions(Opts),
                                            createBackendData(ExtData,
                                                              TM.getPointerSizeInBits(0))});
  PerFunctionPasses.add(
      createTargetTransformInfoWrapperPass(TM.getTargetIRAnalysis()));

  unsigned OptLevel;
  if (Opts.OptLevel == vc::OptimizerLevel::None)
    OptLevel = 0;
  else
    OptLevel = 2;

  PassManagerBuilder PMBuilder;
  PMBuilder.Inliner = createFunctionInliningPass(2, 2, false);
  PMBuilder.OptLevel = OptLevel;
  PMBuilder.SizeLevel = OptLevel;
  PMBuilder.SLPVectorize = false;
  PMBuilder.LoopVectorize = false;
  PMBuilder.DisableUnrollLoops = false;
  PMBuilder.MergeFunctions = false;
  PMBuilder.PrepareForThinLTO = false;
  PMBuilder.PrepareForLTO = false;
  PMBuilder.RerollLoops = true;

  TM.adjustPassManager(PMBuilder);

  PMBuilder.populateFunctionPassManager(PerFunctionPasses);
  PMBuilder.populateModulePassManager(PerModulePasses);

  // Do we need per function passes at all?
  PerFunctionPasses.doInitialization();
  for (Function &F : M) {
    if (!F.isDeclaration())
      PerFunctionPasses.run(F);
  }
  PerFunctionPasses.doFinalization();

  PerModulePasses.run(M);
}

static void dumpFinalOutput(const vc::CompileOptions &Opts, const Module &M,
                            StringRef IsaBinary) {
  if (Opts.DumpIR && Opts.Dumper)
    Opts.Dumper->dumpModule(M, "final.ll");
  if (Opts.DumpIsa && Opts.Dumper)
    Opts.Dumper->dumpBinary({IsaBinary.data(), IsaBinary.size()}, "final.isa");
}

static void populateCodeGenPassManager(const vc::CompileOptions &Opts,
                                       const vc::ExternalData &ExtData,
                                       TargetMachine &TM, raw_pwrite_stream &OS,
                                       legacy::PassManager &PM) {
  TargetLibraryInfoImpl TLII{TM.getTargetTriple()};
  PM.add(new TargetLibraryInfoWrapperPass(TLII));
  PM.add(new GenXBackendConfig{createBackendOptions(Opts),
                               createBackendData(ExtData,
                                   TM.getPointerSizeInBits(0))});

#ifndef NDEBUG
  // Do not enforce IR verification at an arbitrary moments in release builds
  constexpr bool DisableIrVerifier = false;
#else
  constexpr bool DisableIrVerifier = true;
#endif

  auto FileType = IGCLLVM::TargetMachine::CodeGenFileType::CGFT_AssemblyFile;

  bool AddPasses =
      TM.addPassesToEmitFile(PM, OS, nullptr, FileType, DisableIrVerifier);
  IGC_ASSERT_MESSAGE(!AddPasses, "Bad filetype for vc-codegen");
}

static vc::ocl::CompileOutput runOclCodeGen(const vc::CompileOptions &Opts,
                                            const vc::ExternalData &ExtData,
                                            TargetMachine &TM, Module &M) {
  vc::PassManager PM;

  SmallString<32> IsaBinary;
  raw_svector_ostream OS(IsaBinary);
  raw_null_ostream NullOS;
  if (Opts.DumpIsa)
    populateCodeGenPassManager(Opts, ExtData, TM, OS, PM);
  else
    populateCodeGenPassManager(Opts, ExtData, TM, NullOS, PM);

  GenXOCLRuntimeInfo::CompiledModuleT CompiledModule;
  PM.add(createGenXOCLInfoExtractorPass(CompiledModule));

  PM.run(M);
  dumpFinalOutput(Opts, M, IsaBinary);

  return CompiledModule;
}

static vc::cm::CompileOutput runCmCodeGen(const vc::CompileOptions &Opts,
                                          const vc::ExternalData &ExtData,
                                          TargetMachine &TM, Module &M) {
  vc::PassManager PM;

  SmallString<32> IsaBinary;
  raw_svector_ostream OS(IsaBinary);
  populateCodeGenPassManager(Opts, ExtData, TM, OS, PM);
  PM.run(M);
  dumpFinalOutput(Opts, M, IsaBinary);
  vc::cm::CompileOutput Output;
  Output.IsaBinary.assign(IsaBinary.begin(), IsaBinary.end());
  return Output;
}

static vc::CompileOutput runCodeGen(const vc::CompileOptions &Opts,
                                    const vc::ExternalData &ExtData,
                                    TargetMachine &TM, Module &M) {
  switch (Opts.Binary) {
  case vc::BinaryKind::CM:
    return runCmCodeGen(Opts, ExtData, TM, M);
  case vc::BinaryKind::OpenCL:
  case vc::BinaryKind::ZE:
    return runOclCodeGen(Opts, ExtData, TM, M);
  }
  IGC_ASSERT_EXIT_MESSAGE(0, "Unknown runtime kind");
}

// Parse global llvm cl options.
// Parsing of cl options should not fail under any circumstances.
static void parseLLVMOptions(const std::string &Args) {
  BumpPtrAllocator Alloc;
  StringSaver Saver{Alloc};
  SmallVector<const char *, 8> Argv{"vc-codegen"};
  cl::TokenizeGNUCommandLine(Args, Saver, Argv);

  // Reset all options to ensure that scalar part does not affect
  // vector compilation.
  cl::ResetAllOptionOccurrences();
  cl::ParseCommandLineOptions(Argv.size(), Argv.data());
}

Expected<vc::CompileOutput> vc::Compile(ArrayRef<char> Input,
                                        const vc::CompileOptions &Opts,
                                        const vc::ExternalData &ExtData,
                                        ArrayRef<uint32_t> SpecConstIds,
                                        ArrayRef<uint64_t> SpecConstValues) {
  parseLLVMOptions(Opts.LLVMOptions);
  // Reset options when everything is done here. This is needed to not
  // interfere with subsequent translations (including scalar part).
  const auto ClOptGuard =
      llvm::make_scope_exit([]() { cl::ResetAllOptionOccurrences(); });

  LLVMContext Context;
  LLVMInitializeGenXTarget();
  LLVMInitializeGenXTargetInfo();
  llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
  llvm::initializeTarget(Registry);

  Expected<std::unique_ptr<llvm::Module>> ExpModule =
      getModule(Input, Opts.FType, SpecConstIds, SpecConstValues, Context);
  if (!ExpModule)
    return ExpModule.takeError();
  Module &M = *ExpModule.get();

  if (Opts.DumpIR && Opts.Dumper)
    Opts.Dumper->dumpModule(M, "after_spirv_reader.ll");

  vc::PassManager PerModulePasses;
  PerModulePasses.add(createGenXSPIRVReaderAdaptorPass());
  PerModulePasses.add(createGenXRestoreIntrAttrPass());
  PerModulePasses.run(M);

  Triple TheTriple = overrideTripleWithVC(M.getTargetTriple());
  M.setTargetTriple(TheTriple.getTriple());

  auto ExpTargetMachine = createTargetMachine(Opts, TheTriple);
  if (!ExpTargetMachine)
    return ExpTargetMachine.takeError();
  TargetMachine &TM = *ExpTargetMachine.get();
  M.setDataLayout(TM.createDataLayout());

  // Save old value and restore at the end.
  bool TimePassesIsEnabledLocal = TimePassesIsEnabled;
  if (Opts.TimePasses)
    TimePassesIsEnabled = true;

  // Enable LLVM statistics recording if required.
  if (Opts.ShowStats || !Opts.StatsFile.empty())
    llvm::EnableStatistics(false);

  if (Opts.DumpIR && Opts.Dumper)
    Opts.Dumper->dumpModule(M, "after_ir_adaptors.ll");

  optimizeIR(Opts, ExtData, TM, M);

  if (Opts.DumpIR && Opts.Dumper)
    Opts.Dumper->dumpModule(M, "optimized.ll");

  vc::CompileOutput Output = runCodeGen(Opts, ExtData, TM, M);

  // Print timers if any and restore old TimePassesIsEnabled value.
  TimerGroup::printAll(llvm::errs());
  TimePassesIsEnabled = TimePassesIsEnabledLocal;

  // Print LLVM statistics if required.
  if (Opts.ShowStats)
    llvm::PrintStatistics(llvm::errs());
  if (!Opts.StatsFile.empty()) {
    std::error_code EC;
    auto StatS = std::make_unique<llvm::raw_fd_ostream>(
        Opts.StatsFile, EC, llvm::sys::fs::OF_Text);
    if (EC)
      llvm::errs() << Opts.StatsFile << ": " << EC.message();
    else
      llvm::PrintStatisticsJSON(*StatS);
  }
  return Output;
}

template <typename ID, ID... UnknownIDs>
static Expected<opt::InputArgList>
parseOptions(const SmallVectorImpl<const char *> &Argv, unsigned FlagsToInclude,
             const opt::OptTable &Options, bool IsStrictMode) {
  const bool IsInternal = FlagsToInclude & IGC::options::VCInternalOption;

  unsigned MissingArgIndex = 0;
  unsigned MissingArgCount = 0;
  opt::InputArgList InputArgs =
      Options.ParseArgs(Argv, MissingArgIndex, MissingArgCount, FlagsToInclude);
  if (MissingArgCount)
    return make_error<vc::OptionError>(Argv[MissingArgIndex], IsInternal);

  // ocloc uncoditionally passes opencl options to internal options.
  // Skip checking of internal options for now.
  if (IsStrictMode) {
    if (opt::Arg *A = InputArgs.getLastArg(UnknownIDs...)) {
      std::string BadOpt = A->getAsString(InputArgs);
      return make_error<vc::OptionError>(BadOpt, IsInternal);
    }
  }

  return {std::move(InputArgs)};
}

static Expected<opt::InputArgList>
parseApiOptions(StringSaver &Saver, StringRef ApiOptions, bool IsStrictMode) {
  using namespace IGC::options::api;

  SmallVector<const char *, 8> Argv;
  cl::TokenizeGNUCommandLine(ApiOptions, Saver, Argv);

  const opt::OptTable &Options = IGC::getApiOptTable();
  // This can be rewritten to parse options and then check for
  // OPT_vc_codegen, but it would be better to manually check for
  // this option before any real parsing. If it is missing,
  // then no parsing should be done at all.
  auto HasOption = [&Argv](const std::string &Opt) {
    return std::any_of(Argv.begin(), Argv.end(),
                       [&Opt](const char *ArgStr) { return Opt == ArgStr; });
  };
  const std::string VCCodeGenOptName =
      Options.getOption(OPT_vc_codegen).getPrefixedName();
  if (HasOption(VCCodeGenOptName)) {
    const unsigned FlagsToInclude =
        IGC::options::VCApiOption | IGC::options::IGCApiOption;
    return parseOptions<ID, OPT_UNKNOWN, OPT_INPUT>(Argv, FlagsToInclude,
                                                    Options, IsStrictMode);
  }
  // Deprecated -cmc parsing just for compatibility.
  const std::string IgcmcOptName =
      Options.getOption(OPT_igcmc).getPrefixedName();
  if (HasOption(IgcmcOptName)) {
    llvm::errs()
        << "'" << IgcmcOptName
        << "' option is deprecated and will be removed in the future release. "
           "Use -vc-codegen instead for compiling from SPIRV.\n";
    const unsigned FlagsToInclude =
        IGC::options::IgcmcApiOption | IGC::options::IGCApiOption;
    return parseOptions<ID, OPT_UNKNOWN, OPT_INPUT>(Argv, FlagsToInclude,
                                                    Options, IsStrictMode);
  }

  return make_error<vc::NotVCError>();
}

static Expected<opt::InputArgList>
parseInternalOptions(StringSaver &Saver, StringRef InternalOptions) {
  using namespace IGC::options::internal;

  SmallVector<const char *, 8> Argv;
  cl::TokenizeGNUCommandLine(InternalOptions, Saver, Argv);
  // Internal options are always unchecked.
  constexpr bool IsStrictMode = false;
  const opt::OptTable &Options = IGC::getInternalOptTable();
  const unsigned FlagsToInclude =
      IGC::options::VCInternalOption | IGC::options::IGCInternalOption;
  return parseOptions<ID, OPT_UNKNOWN, OPT_INPUT>(Argv, FlagsToInclude, Options,
                                                  IsStrictMode);
}

static Error makeOptionError(const opt::Arg &A, const opt::ArgList &Opts,
                             bool IsInternal) {
  const std::string BadOpt = A.getAsString(Opts);
  return make_error<vc::OptionError>(BadOpt, IsInternal);
}

static Error fillApiOptions(const opt::ArgList &ApiOptions,
                            vc::CompileOptions &Opts) {
  using namespace IGC::options::api;

  if (ApiOptions.hasArg(OPT_no_vector_decomposition))
    Opts.NoVecDecomp = true;
  if (ApiOptions.hasArg(OPT_emit_debug)) {
    Opts.EmitExtendedDebug = true;
    Opts.EmitDebuggableKernels = true; // TODO: we should not depend on "-g"
  }
  if (ApiOptions.hasArg(OPT_vc_fno_struct_splitting))
    Opts.DisableStructSplitting = true;
  if (ApiOptions.hasArg(OPT_vc_fno_jump_tables))
    Opts.NoJumpTables = true;
  if (ApiOptions.hasArg(OPT_vc_ftranslate_legacy_memory_intrinsics))
    Opts.TranslateLegacyMemoryIntrinsics = true;
  if (ApiOptions.hasArg(OPT_vc_disable_finalizer_msg))
    Opts.DisableFinalizerMsg = true;
  if (ApiOptions.hasArg(OPT_large_GRF))
    Opts.IsLargeGRFMode = true;
  if (ApiOptions.hasArg(OPT_vc_use_plain_2d_images))
    Opts.UsePlain2DImages = true;
  if (ApiOptions.hasArg(OPT_vc_enable_preemption))
    Opts.EnablePreemption = true;

  if (opt::Arg *A = ApiOptions.getLastArg(OPT_fp_contract)) {
    StringRef Val = A->getValue();
    auto MayBeAllowFPOPFusion =
        StringSwitch<Optional<FPOpFusion::FPOpFusionMode>>(Val)
            .Case("on", FPOpFusion::Standard)
            .Case("fast", FPOpFusion::Fast)
            .Case("off", FPOpFusion::Strict)
            .Default(None);
    if (!MayBeAllowFPOPFusion)
      return makeOptionError(*A, ApiOptions, /*IsInternal=*/false);
    Opts.AllowFPOpFusion = MayBeAllowFPOPFusion.getValue();
  }

  if (opt::Arg *A =
          ApiOptions.getLastArg(OPT_vc_optimize, OPT_opt_disable_ze)) {
    if (A->getOption().matches(OPT_vc_optimize)) {
      StringRef Val = A->getValue();
      auto MaybeLevel = StringSwitch<Optional<vc::OptimizerLevel>>(Val)
                            .Case("none", vc::OptimizerLevel::None)
                            .Case("full", vc::OptimizerLevel::Full)
                            .Default(None);
      if (!MaybeLevel)
        return makeOptionError(*A, ApiOptions, /*IsInternal=*/false);
      Opts.OptLevel = MaybeLevel.getValue();
    } else {
      IGC_ASSERT(A->getOption().matches(OPT_opt_disable_ze));
      Opts.OptLevel = vc::OptimizerLevel::None;
    }
  }

  if (opt::Arg *A = ApiOptions.getLastArg(OPT_vc_stateless_private_size)) {
    StringRef Val = A->getValue();
    unsigned Result;
    if (Val.getAsInteger(/*Radix=*/0, Result))
      return makeOptionError(*A, ApiOptions, /*IsInternal=*/false);
    Opts.StackMemSize = Result;
  }

  return Error::success();
}

static Error fillInternalOptions(const opt::ArgList &InternalOptions,
                                 vc::CompileOptions &Opts) {
  using namespace IGC::options::internal;

  if (InternalOptions.hasArg(OPT_dump_isa_binary))
    Opts.DumpIsa = true;
  if (InternalOptions.hasArg(OPT_dump_llvm_ir))
    Opts.DumpIR = true;
  if (InternalOptions.hasArg(OPT_dump_asm))
    Opts.DumpAsm = true;
  if (InternalOptions.hasArg(OPT_ftime_report))
    Opts.TimePasses = true;
  if (InternalOptions.hasArg(OPT_print_stats))
    Opts.ShowStats = true;
  Opts.StatsFile = InternalOptions.getLastArgValue(OPT_stats_file).str();
  if (InternalOptions.hasArg(OPT_intel_use_bindless_buffers_ze))
    Opts.UseBindlessBuffers = true;

  if (opt::Arg *A = InternalOptions.getLastArg(OPT_binary_format)) {
    StringRef Val = A->getValue();
    auto MaybeBinary = StringSwitch<Optional<vc::BinaryKind>>(Val)
                           .Case("cm", vc::BinaryKind::CM)
                           .Case("ocl", vc::BinaryKind::OpenCL)
                           .Case("ze", vc::BinaryKind::ZE)
                           .Default(None);
    if (!MaybeBinary)
      return makeOptionError(*A, InternalOptions, /*IsInternal=*/true);
    Opts.Binary = MaybeBinary.getValue();
  }

  Opts.FeaturesString =
      llvm::join(InternalOptions.getAllArgValues(OPT_target_features), ",");

  if (InternalOptions.hasArg(OPT_help)) {
    constexpr const char *Usage = "-options \"-vc-codegen [options]\"";
    constexpr const char *Title = "Vector compiler options";
    constexpr unsigned FlagsToInclude = IGC::options::VCApiOption;
    constexpr unsigned FlagsToExclude = 0;
    constexpr bool ShowAllAliases = false;
    IGCLLVM::printHelp(IGC::getApiOptTable(), llvm::errs(), Usage, Title,
                       FlagsToInclude, FlagsToExclude, ShowAllAliases);
  }
  if (InternalOptions.hasArg(OPT_help_internal)) {
    constexpr const char *Usage =
        "-options \"-vc-codegen\" -internal_options \"[options]\"";
    constexpr const char *Title = "Vector compiler internal options";
    constexpr unsigned FlagsToInclude = IGC::options::VCInternalOption;
    constexpr unsigned FlagsToExclude = 0;
    constexpr bool ShowAllAliases = false;
    IGCLLVM::printHelp(IGC::getInternalOptTable(), llvm::errs(), Usage, Title,
                       FlagsToInclude, FlagsToExclude, ShowAllAliases);
  }

  return Error::success();
}

// Prepare llvm options string using different API and internal options.
static std::string composeLLVMArgs(const opt::ArgList &ApiArgs,
                                   const opt::ArgList &InternalArgs) {
  std::string Result;

  // Handle input llvm options.
  if (InternalArgs.hasArg(IGC::options::internal::OPT_llvm_options))
    Result += join(
        InternalArgs.getAllArgValues(IGC::options::internal::OPT_llvm_options),
        " ");

  // Add visaopts if any.
  for (auto OptID : {IGC::options::api::OPT_igcmc_visaopts,
                     IGC::options::api::OPT_Xfinalizer}) {
    if (!ApiArgs.hasArg(OptID))
      continue;
    Result += " -finalizer-opts='";
    Result += join(ApiArgs.getAllArgValues(OptID), " ");
    Result += "'";
  }

  // Add gtpin options if any.
  if (ApiArgs.hasArg(IGC::options::api::OPT_gtpin_rera))
    Result += " -finalizer-opts='-GTPinReRA'";
  if (ApiArgs.hasArg(IGC::options::api::OPT_gtpin_grf_info))
    Result += " -finalizer-opts='-getfreegrfinfo -rerapostschedule'";
  if (opt::Arg *A =
          ApiArgs.getLastArg(IGC::options::api::OPT_gtpin_scratch_area_size)) {
    Result += " -finalizer-opts='-GTPinScratchAreaSize ";
    Result += A->getValue();
    Result += "'";
  }

  return Result;
}

static Expected<vc::CompileOptions>
fillOptions(const opt::ArgList &ApiOptions,
            const opt::ArgList &InternalOptions) {
  vc::CompileOptions Opts;
  Error Status = fillApiOptions(ApiOptions, Opts);
  if (Status)
    return {std::move(Status)};

  Status = fillInternalOptions(InternalOptions, Opts);
  if (Status)
    return {std::move(Status)};

  // Prepare additional llvm options (like finalizer args).
  Opts.LLVMOptions = composeLLVMArgs(ApiOptions, InternalOptions);

  return {std::move(Opts)};
}

// Filter input argument list to derive options that will contribute
// to subsequent translation.
// InputArgs -- argument list to filter, should outlive resulting
// derived option list.
// IncludeFlag -- options with that flag will be included in result.
static opt::DerivedArgList filterUsedOptions(opt::InputArgList &InputArgs,
                                             IGC::options::Flags IncludeFlag) {
  opt::DerivedArgList FilteredArgs(InputArgs);

  // InputArg is not a constant. This is required to pass it to append
  // function of derived argument list. Derived argument list will not
  // own added argument so it will not try to free this memory.
  // Additionally note that InputArgs are used in derived arg list as
  // a constant so added arguments should not be modified through
  // derived list to avoid unexpected results.
  for (opt::Arg *InputArg : InputArgs) {
    const opt::Arg *Arg = InputArg;
    // Get alias as unaliased form can belong to used flags
    // (see cl intel gtpin options).
    if (const opt::Arg *AliasArg = InputArg->getAlias())
      Arg = AliasArg;
    // Ignore options without required flag.
    if (!Arg->getOption().hasFlag(IncludeFlag))
      continue;
    FilteredArgs.append(InputArg);
  }

  return FilteredArgs;
}

opt::DerivedArgList filterApiOptions(opt::InputArgList &InputArgs) {
  if (InputArgs.hasArg(IGC::options::api::OPT_igcmc))
    return filterUsedOptions(InputArgs, IGC::options::IgcmcApiOption);

  return filterUsedOptions(InputArgs, IGC::options::VCApiOption);
}

llvm::Expected<vc::CompileOptions>
vc::ParseOptions(llvm::StringRef ApiOptions, llvm::StringRef InternalOptions,
                 bool IsStrictMode) {
  llvm::BumpPtrAllocator Alloc;
  llvm::StringSaver Saver{Alloc};
  auto ExpApiArgList = parseApiOptions(Saver, ApiOptions, IsStrictMode);
  if (!ExpApiArgList)
    return ExpApiArgList.takeError();
  opt::InputArgList &ApiArgs = ExpApiArgList.get();
  const opt::DerivedArgList VCApiArgs = filterApiOptions(ApiArgs);

  auto ExpInternalArgList = parseInternalOptions(Saver, InternalOptions);
  if (!ExpInternalArgList)
    return ExpInternalArgList.takeError();
  opt::InputArgList &InternalArgs = ExpInternalArgList.get();
  const opt::DerivedArgList VCInternalArgs =
      filterUsedOptions(InternalArgs, IGC::options::VCInternalOption);

  return fillOptions(VCApiArgs, VCInternalArgs);
}
