//===--- ClangImporter.cpp - Import Clang Modules -------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements support for loading Clang modules into Swift.
//
//===----------------------------------------------------------------------===//
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "ImporterImpl.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Component.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Types.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "clang/AST/ASTContext.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/Version.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include <memory>
#include <cstdlib>
#include <dlfcn.h>
#include <sys/param.h>

using namespace swift;

// Commonly-used Clang classes.
using clang::CompilerInstance;
using clang::CompilerInvocation;

// Dummy function used with dladdr.
void swift_clang_importer() { }

#pragma mark Internal data structures
namespace {
  class SwiftModuleLoaderAction : public clang::SyntaxOnlyAction {
  protected:
    /// BeginSourceFileAction - Callback at the start of processing a single
    /// input.
    ///
    /// \return True on success; on failure \see ExecutionAction() and
    /// EndSourceFileAction() will not be called.
    virtual bool BeginSourceFileAction(CompilerInstance &ci,
                                       StringRef filename) {
      // Enable incremental processing, so we can load modules after we've
      // finished parsing our fake translation unit.
      ci.getPreprocessor().enableIncrementalProcessing();

      return clang::SyntaxOnlyAction::BeginSourceFileAction(ci, filename);
    }
  };
}


ClangImporter::ClangImporter(ASTContext &ctx)
  : Impl(*new Implementation(ctx))
{
}

ClangImporter::~ClangImporter() {
  delete &Impl;
}

#pragma mark Module loading

ClangImporter *ClangImporter::create(ASTContext &ctx, StringRef sdkroot,
                                     StringRef targetTriple,
                                     StringRef moduleCachePath,
                                     ArrayRef<std::string> importSearchPaths,
                                     ArrayRef<std::string> frameworkSearchPaths,
                                     StringRef overrideResourceDir) {
  std::unique_ptr<ClangImporter> importer(new ClangImporter(ctx));

  // Create a Clang diagnostics engine.
  // FIXME: Route these diagnostics back to Swift's diagnostics engine,
  // somehow. We'll lose macro expansions, but so what.
  auto clangDiags(CompilerInstance::createDiagnostics(
                    new clang::DiagnosticOptions, 0, nullptr));

  // Don't stop emitting messages if we ever can't find a file.
  // FIXME: This is actually a general problem: any "fatal" error could mess up
  // the CompilerInvocation.
  clangDiags->setDiagnosticErrorAsFatal(clang::diag::err_module_not_found,
                                        false);

  // Construct the invocation arguments for Objective-C ARC with the current
  // target.
  //
  // FIXME: Figure out an appropriate OS deployment version to pass along.
  std::vector<std::string> invocationArgStrs = {
    "-x", "objective-c", "-fobjc-arc", "-fmodules", "-fblocks",
    "-fsyntax-only", "-w",
    "-isysroot", sdkroot.str(), "-triple", targetTriple.str(),
    "swift.m"
  };

  for (auto path : importSearchPaths) {
    invocationArgStrs.push_back("-I");
    invocationArgStrs.push_back(path);
  }

  for (auto path : frameworkSearchPaths) {
    invocationArgStrs.push_back("-F");
    invocationArgStrs.push_back(path);
  }

  // Set the module cache path.
  if (moduleCachePath.empty()) {
    llvm::SmallString<128> DefaultModuleCache;
    llvm::sys::path::system_temp_directory(/*erasedOnReboot=*/false,
                                           DefaultModuleCache);
    llvm::sys::path::append(DefaultModuleCache, "org.llvm.clang");
    llvm::sys::path::append(DefaultModuleCache, "ModuleCache");
    invocationArgStrs.push_back("-fmodules-cache-path=");
    invocationArgStrs.back().append(DefaultModuleCache.str());
  } else {
    invocationArgStrs.push_back("-fmodules-cache-path=");
    invocationArgStrs.back().append(moduleCachePath.str());
  }

  // Figure out where Swift lives.
  // This silly cast below avoids a C++ warning.
  Dl_info info;
  if (dladdr((void *)(uintptr_t)swift_clang_importer, &info) == 0)
    llvm_unreachable("Call to dladdr() failed");

  // Resolve symlinks.
  char swiftPath[MAXPATHLEN];
  if (!realpath(info.dli_fname, swiftPath)) {
    // FIXME: Diagnose this.
    return nullptr;
  }

  if (overrideResourceDir.empty()) {
    llvm::SmallString<128> resourceDir(swiftPath);
  
    // We now have the swift executable/library path. Adjust it to refer to our
    // copy of the Clang headers under lib/swift/clang.
  
    llvm::sys::path::remove_filename(resourceDir);
    llvm::sys::path::remove_filename(resourceDir);
    llvm::sys::path::append(resourceDir, "lib", "swift",
                            "clang", CLANG_VERSION_STRING);
  
    // Set the Clang resource directory to the path we computed.
    invocationArgStrs.push_back("-resource-dir");
    invocationArgStrs.push_back(resourceDir.str());
  } else {
    invocationArgStrs.push_back("-resource-dir");
    invocationArgStrs.push_back(overrideResourceDir);
  }

  std::vector<const char *> invocationArgs;
  for (auto &argStr : invocationArgStrs)
    invocationArgs.push_back(argStr.c_str());

  // Create a new Clang compiler invocation.
  llvm::IntrusiveRefCntPtr<CompilerInvocation> invocation
    = new CompilerInvocation;
  if (!CompilerInvocation::CreateFromArgs(*invocation,
                                         &invocationArgs.front(),
                                         (&invocationArgs.front() +
                                          invocationArgs.size()),
                                         *clangDiags))
    return nullptr;

  // Create an almost-empty memory buffer corresponding to the file "swift.m"
  auto sourceBuffer = llvm::MemoryBuffer::getMemBuffer("extern int __swift;");
  invocation->getPreprocessorOpts().addRemappedFile("swift.m", sourceBuffer);

  // Create a compiler instance.
  importer->Impl.Instance.reset(new CompilerInstance);
  auto &instance = *importer->Impl.Instance;
  instance.setDiagnostics(&*clangDiags);
  instance.setInvocation(&*invocation);

  // Create the associated action.
  importer->Impl.Action.reset(new SwiftModuleLoaderAction);

  // Execute the action. We effectively inline most of
  // CompilerInstance::ExecuteAction here, because we need to leave the AST
  // open for future module loading.
  // FIXME: This has to be cleaned up on the Clang side before we can improve
  // things here.

  // Create the target instance.
  instance.setTarget(
    clang::TargetInfo::CreateTargetInfo(*clangDiags,&instance.getTargetOpts()));
  if (!instance.hasTarget())
    return nullptr;

  // Inform the target of the language options.
  //
  // FIXME: We shouldn't need to do this, the target should be immutable once
  // created. This complexity should be lifted elsewhere.
  instance.getTarget().setForcedLangOptions(instance.getLangOpts());

  // Run the action.
  auto &action = *importer->Impl.Action;
  if (action.BeginSourceFile(instance, instance.getFrontendOpts().Inputs[0])) {
    action.Execute();
    // Note: don't call EndSourceFile here!
  }
  // FIXME: This is necessary because Clang doesn't really support what we're
  // doing, and TUScope has gone stale.
  instance.getSema().TUScope = nullptr;

  // Create the selectors we'll be looking for.
  auto &clangContext = importer->Impl.Instance->getASTContext();
  importer->Impl.objectAtIndexedSubscript
    = clangContext.Selectors.getUnarySelector(
        &clangContext.Idents.get("objectAtIndexedSubscript"));
  clang::IdentifierInfo *setObjectAtIndexedSubscriptIdents[2] = {
    &clangContext.Idents.get("setObject"),
    &clangContext.Idents.get("atIndexedSubscript")
  };
  importer->Impl.setObjectAtIndexedSubscript
    = clangContext.Selectors.getSelector(2, setObjectAtIndexedSubscriptIdents);
  importer->Impl.objectForKeyedSubscript
    = clangContext.Selectors.getUnarySelector(
        &clangContext.Idents.get("objectForKeyedSubscript"));
  clang::IdentifierInfo *setObjectForKeyedSubscriptIdents[2] = {
    &clangContext.Idents.get("setObject"),
    &clangContext.Idents.get("forKeyedSubscript")
  };
  importer->Impl.setObjectForKeyedSubscript
    = clangContext.Selectors.getSelector(2, setObjectForKeyedSubscriptIdents);

  return importer.release();
}

Module *ClangImporter::loadModule(
    SourceLoc importLoc,
    ArrayRef<std::pair<Identifier, SourceLoc>> path) {
  // Convert the Swift import path over to a Clang import path.
  // FIXME: Map source locations over. Fun, fun!
  SmallVector<std::pair<clang::IdentifierInfo *, clang::SourceLocation>, 4>
    clangPath;

  auto &clangContext = Impl.Instance->getASTContext();
  for (auto component : path) {
    clangPath.push_back({&clangContext.Idents.get(component.first.str()),
                         clang::SourceLocation()} );
  }

  // Load the Clang module.
  // FIXME: The source location here is completely bogus. It can't be
  // invalid, and it can't be the same thing twice in a row, so we just use
  // a counter. Having real source locations would be far, far better.
  // FIXME: This should not print a message if we just can't find a Clang
  // module -- that's Swift's responsibility, since there could in theory be a
  // later module loader.
  auto &srcMgr = clangContext.getSourceManager();
  clang::SourceLocation clangImportLoc
    = srcMgr.getLocForStartOfFile(srcMgr.getMainFileID())
            .getLocWithOffset(Impl.ImportCounter++);
  auto clangModule = Impl.Instance->loadModule(clangImportLoc,
                                               clangPath,
                                               clang::Module::AllVisible,
                                               /*IsInclusionDirective=*/false);
  if (!clangModule)
    return nullptr;
  auto &cachedResult = Impl.ModuleWrappers[clangModule];
  if (Module *result = cachedResult.getPointer()) {
    if (!cachedResult.getInt()) {
      // Force load adapter modules for all imported modules.
      // FIXME: This forces the creation of wrapper modules for all imports as
      // well, and may do unnecessary work.
      cachedResult.setInt(true);
      //result->forAllVisibleModules(path, [&](Module::ImportedModule import) {});
    }
    return result;
  }

  // FIXME: Revisit this once components are fleshed out. Clang components
  // are likely born-fragile.
  auto component = new (Impl.SwiftContext.Allocate<Component>(1)) Component();

  // Build the representation of the Clang module in Swift.
  // FIXME: The name of this module could end up as a key in the ASTContext,
  // but that's not correct for submodules.
  auto result = new (Impl.SwiftContext)
    ClangModule(Impl.SwiftContext, (*clangModule).getFullModuleName(),
                *this, component, clangModule);
  cachedResult.setPointer(result);

  // FIXME: Total hack.
  if (!Impl.firstClangModule)
    Impl.firstClangModule = result;

  // Force load adapter modules for all imported modules.
  // FIXME: This forces the creation of wrapper modules for all imports as
  // well, and may do unnecessary work.
  cachedResult.setInt(true);
  result->forAllVisibleModules(path, [](Module::ImportedModule import) {});

  // Bump the generation count.
  ++Impl.Generation;
  Impl.SwiftContext.bumpGeneration();

  return result;
}

ClangModule *
ClangImporter::Implementation::getWrapperModule(ClangImporter &importer,
                                                clang::Module *underlying,
                                                Component *component) {
  auto &cacheEntry = ModuleWrappers[underlying];
  if (ClangModule *cachedModule = cacheEntry.getPointer())
    return cachedModule;

  if (!component)
    component = new (SwiftContext.Allocate<Component>(1)) Component();

  auto result = new (SwiftContext) ClangModule(SwiftContext,
                                               underlying->getFullModuleName(),
                                               importer, component, underlying);
  cacheEntry.setPointer(result);
  return result;
}

ClangModule *ClangImporter::Implementation::getClangModuleForDecl(
    const clang::Decl *D) {
  if (auto OID = dyn_cast<clang::ObjCInterfaceDecl>(D))
    // Put the Objective-C class into the module that contains the @interface
    // definition, not just @class forward declaration.
    D = OID->getDefinition();
  else
    D = D->getCanonicalDecl();

  clang::Module *M = D->getOwningModule();
  if (!M)
    return nullptr;
  // Get the parent module because currently we don't represent submodules with
  // ClangModule.
  // FIXME: this is just a workaround until we can import submodules.
  M = M->getTopLevelModule();

  return getWrapperModule(
      static_cast<ClangImporter &>(*SwiftContext.getClangModuleLoader()), M,
      nullptr);
}

#pragma mark Source locations
clang::SourceLocation
ClangImporter::Implementation::importSourceLoc(SourceLoc loc) {
  // FIXME: Implement!
  return clang::SourceLocation();
}

SourceLoc
ClangImporter::Implementation::importSourceLoc(clang::SourceLocation loc) {
  // FIXME: Implement!
  return SourceLoc();
}

SourceRange
ClangImporter::Implementation::importSourceRange(clang::SourceRange loc) {
  // FIXME: Implement!
  return SourceRange();
}

#pragma mark Importing names

/// \brief Determine whether the given name is reserved for Swift.
static bool isSwiftReservedName(StringRef name) {
  /// FIXME: Check Swift keywords.
  return llvm::StringSwitch<bool>(name)
           .Cases("true", "false", true)
           .Default(false);
}

clang::DeclarationName
ClangImporter::Implementation::importName(Identifier name) {
  // FIXME: When we start dealing with C++, we can map over some operator
  // names.
  if (name.isOperator())
    return clang::DeclarationName();

  if (isSwiftReservedName(name.str()))
    return clang::DeclarationName();

  // Map the identifier. If it's some kind of keyword, it can't be mapped.
  auto ident = &Instance->getASTContext().Idents.get(name.str());
  if (ident->getTokenID() != clang::tok::identifier)
    return clang::DeclarationName();

  return ident;
}

Identifier
ClangImporter::Implementation::importName(clang::DeclarationName name,
                                          StringRef suffix) {
  // FIXME: At some point, we'll be able to import operators as well.
  if (!name || name.getNameKind() != clang::DeclarationName::Identifier)
    return Identifier();

  // Get the Swift identifier.
  if (suffix.empty()) {
    StringRef nameStr = name.getAsIdentifierInfo()->getName();
    if (isSwiftReservedName(nameStr))
      return Identifier();

    return SwiftContext.getIdentifier(nameStr);
  }

  // Append the suffix, and try again.
  llvm::SmallString<64> nameBuf;
  nameBuf += name.getAsIdentifierInfo()->getName();
  nameBuf += suffix;

  if (isSwiftReservedName(nameBuf))
    return Identifier();

  return SwiftContext.getIdentifier(nameBuf);
}


#pragma mark Name lookup
void ClangImporter::lookupValue(Module *module,
                                Module::AccessPathTy accessPath,
                                Identifier name,
                                NLKind lookupKind,
                                SmallVectorImpl<ValueDecl*> &results) {
  assert(accessPath.size() <= 1 && "can only refer to top-level decls");
  if (accessPath.size() == 1 && accessPath.front().first != name)
    return;

  auto &pp = Impl.Instance->getPreprocessor();
  auto &sema = Impl.Instance->getSema();

  // If the name ends with 'Proto', strip off the 'Proto' and look for an
  // Objective-C protocol.
  // FIXME: Revisit this notion. We could append 'Proto' only when there is both
  // a class and a protocol with the same name, as with NSObject. However,
  // doing so requires our input modules to be "sane", in the sense that
  // one cannot introduce a class X in one module and a protocol X in a another
  // module that does *not* depend on 
  auto lookupNameKind = clang::Sema::LookupOrdinaryName;
  if (name.str().endswith("Proto")) {
    name = Impl.SwiftContext.getIdentifier(
             name.str().substr(0, name.str().size() - 5));
    lookupNameKind = clang::Sema::LookupObjCProtocolName;
  }

  // Map the name. If we can't represent the Swift name in Clang, bail out now.
  auto clangName = Impl.importName(name);
  if (!clangName)
    return;
  
  // See if there's a preprocessor macro we can import by this name.
  clang::IdentifierInfo *clangID = clangName.getAsIdentifierInfo();
  if (clangID && clangID->hasMacroDefinition()) {
    if (auto clangMacro = pp.getMacroInfo(clangID)) {
      if (auto valueDecl = Impl.importMacro(name, clangMacro)) {
        results.push_back(valueDecl);
      }
    }
  }

  // Perform name lookup into the global scope.
  // FIXME: Map source locations over.
  clang::LookupResult lookupResult(sema, clangName, clang::SourceLocation(),
                                   lookupNameKind);
  bool FoundType = false;
  if (sema.LookupName(lookupResult, /*Scope=*/0)) {
    // FIXME: Filter based on access path? C++ access control?
    for (auto decl : lookupResult) {
      if (auto swiftDecl = Impl.importDecl(decl->getUnderlyingDecl()))
        if (auto valueDecl = dyn_cast<ValueDecl>(swiftDecl)) {
          // If the importer gave us a declaration from the stdlib, make sure
          // it does not show up in the lookup results for the imported module.
          if (valueDecl->getDeclContext() != Impl.getSwiftModule()) {
            results.push_back(valueDecl);
            FoundType = FoundType || isa<TypeDecl>(valueDecl);
          }
        }
    }
  }

  if (lookupNameKind == clang::Sema::LookupOrdinaryName && !FoundType) {
    // Look up a tag name if we did not find a type with this name already.
    // We don't want to introduce multiple types with same name.
    lookupResult.clear(clang::Sema::LookupTagName);
    if (!sema.LookupName(lookupResult, /*Scope=*/0))
      return;

    // FIXME: Filter based on access path? C++ access control?
    for (auto decl : lookupResult) {
      if (auto swiftDecl = Impl.importDecl(decl->getUnderlyingDecl()))
        if (auto valueDecl = dyn_cast<ValueDecl>(swiftDecl))
          results.push_back(valueDecl);
    }
  }
}

void
ClangImporter::lookupVisibleDecls(clang::VisibleDeclConsumer &consumer) const {
  auto &sema = Impl.Instance->getSema();
  sema.LookupVisibleDecls(Impl.getClangASTContext().getTranslationUnitDecl(),
                          clang::Sema::LookupNameKind::LookupAnyName,
                          consumer);
}

namespace {
class ImportingVisibleDeclConsumer : public clang::VisibleDeclConsumer {
  ClangImporter &TheClangImporter;
  ClangImporter::Implementation &Impl;
  swift::VisibleDeclConsumer &NextConsumer;
  const Module *ModuleFilter = nullptr;

public:
  ImportingVisibleDeclConsumer(ClangImporter &TheClangImporter,
                               ClangImporter::Implementation &Impl,
                               swift::VisibleDeclConsumer &NextConsumer)
      : TheClangImporter(TheClangImporter), Impl(Impl),
        NextConsumer(NextConsumer) {}

  void filterByModule(const Module *M) {
    ModuleFilter = M;
  }

  void FoundDecl(clang::NamedDecl *ND, clang::NamedDecl *Hiding,
                 clang::DeclContext *Ctx,
                 bool InBaseClass) override {
    if (ND->getName().empty())
      return;

    if (ND->isModulePrivate())
      return;

    SmallVector<ValueDecl *, 4> Results;
    TheClangImporter.lookupValue(/*module*/ nullptr,
                                 Module::AccessPathTy(),
                                 Impl.SwiftContext.getIdentifier(ND->getName()),
                                 NLKind::UnqualifiedLookup,
                                 Results);
    for (auto *VD : Results)
      if (!ModuleFilter || VD->getModuleContext() == ModuleFilter)
        NextConsumer.foundDecl(VD);
  }
};
} // unnamed namespace

void ClangImporter::lookupVisibleDecls(VisibleDeclConsumer &Consumer) const {
  ImportingVisibleDeclConsumer ImportingConsumer(
      const_cast<ClangImporter &>(*this), Impl, Consumer);
  lookupVisibleDecls(ImportingConsumer);
}

void ClangImporter::lookupVisibleDecls(const Module *M,
                                       Module::AccessPathTy AccessPath,
                                       VisibleDeclConsumer &Consumer,
                                       NLKind LookupKind) {
  ImportingVisibleDeclConsumer ImportingConsumer(
      const_cast<ClangImporter &>(*this), Impl, Consumer);
  ImportingConsumer.filterByModule(M);
  lookupVisibleDecls(ImportingConsumer);
}

void ClangImporter::loadExtensions(NominalTypeDecl *nominal,
                                   unsigned previousGeneration) {
  auto objcClass = dyn_cast_or_null<clang::ObjCInterfaceDecl>(
                     nominal->getClangDecl());
  if (!objcClass)
    return;

  // Import all of the visible categories. Simply loading them adds them to
  // the list of extensions.
  for (auto I = objcClass->visible_categories_begin(),
            E = objcClass->visible_categories_end();
       I != E; ++I) {
    Impl.importDecl(*I);
  }
}

void ClangImporter::getReexportedModules(
    const Module *module,
    SmallVectorImpl<Module::ImportedModule> &exports) {

  auto underlying = cast<ClangModule>(module)->clangModule;
  auto topLevelAdapter = cast<ClangModule>(module)->getAdapterModule();

  SmallVector<clang::Module *, 8> exported;
  underlying->getExportedModules(exported);

  for (auto exportMod : exported) {
    auto exportWrapper = Impl.getWrapperModule(*this, exportMod,
                                               module->getComponent());

    auto actualExport = exportWrapper->getAdapterModule();
    if (!actualExport || actualExport == topLevelAdapter)
      actualExport = exportWrapper;

    exports.push_back({Module::AccessPathTy(), actualExport});
  }
}

clang::TargetInfo &ClangImporter::getTargetInfo() const {
  return Impl.Instance->getTarget();
}

//===----------------------------------------------------------------------===//
// ClangModule Implementation
//===----------------------------------------------------------------------===//

ClangModule::ClangModule(ASTContext &ctx, std::string DebugModuleName,
                         ModuleLoader &owner, Component *comp,
                         clang::Module *clangModule)
  : LoadedModule(DeclContextKind::ClangModule,
                 ctx.getIdentifier(clangModule->Name),
                 DebugModuleName,
                 comp, ctx, owner),
    clangModule(clangModule)
{
  // Clang modules are always well-formed.
  ASTStage = TypeChecked;
}

bool ClangModule::isTopLevel() const {
  return !clangModule->isSubModule();
}

StringRef ClangModule::getTopLevelModuleName() const {
  return clangModule->getTopLevelModuleName();
}

Module *ClangModule::getAdapterModule() const {
  if (!isTopLevel()) {
    // FIXME: Is this correct for submodules?
    auto &importer = static_cast<ClangImporter&>(getOwner());
    auto topLevel = clangModule->getTopLevelModule();
    auto wrapper = importer.Impl.getWrapperModule(importer, topLevel,
                                                  getComponent());
    return wrapper->getAdapterModule();
  }

  if (!adapterModule.getInt()) {
    // FIXME: Include proper source location.
    auto adapter = Ctx.getModule(Module::AccessPathTy({Name, SourceLoc()}));
    if (isa<ClangModule>(adapter)) {
      adapter = nullptr;
    } else {
      auto &sharedModuleRef = Ctx.LoadedModules[Name.str()];
      assert(!sharedModuleRef || sharedModuleRef == adapter ||
             sharedModuleRef == this);
      sharedModuleRef = adapter;
    }

    auto mutableThis = const_cast<ClangModule *>(this);
    mutableThis->adapterModule.setPointerAndInt(adapter, true);
  }

  return adapterModule.getPointer();
}
