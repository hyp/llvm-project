//===----------- CoreAPIsTest.cpp - Unit tests for Core ORC APIs ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OrcTestCommon.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/OrcError.h"
#include "llvm/Testing/Support/Error.h"

#include <set>
#include <thread>

using namespace llvm;
using namespace llvm::orc;

class CoreAPIsStandardTest : public CoreAPIsBasedStandardTest {};

namespace {

TEST_F(CoreAPIsStandardTest, BasicSuccessfulLookup) {
  bool OnCompletionRun = false;

  auto OnCompletion = [&](Expected<SymbolMap> Result) {
    EXPECT_TRUE(!!Result) << "Resolution unexpectedly returned error";
    auto &Resolved = *Result;
    auto I = Resolved.find(Foo);
    EXPECT_NE(I, Resolved.end()) << "Could not find symbol definition";
    EXPECT_EQ(I->second.getAddress(), FooAddr)
        << "Resolution returned incorrect result";
    OnCompletionRun = true;
  };

  std::shared_ptr<MaterializationResponsibility> FooMR;

  cantFail(JD.define(std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}}),
      [&](MaterializationResponsibility R) {
        FooMR = std::make_shared<MaterializationResponsibility>(std::move(R));
      })));

  ES.lookup(JITDylibSearchList({{&JD, false}}), {Foo}, SymbolState::Ready,
            OnCompletion, NoDependenciesToRegister);

  EXPECT_FALSE(OnCompletionRun) << "Should not have been resolved yet";

  FooMR->notifyResolved({{Foo, FooSym}});

  EXPECT_FALSE(OnCompletionRun) << "Should not be ready yet";

  FooMR->notifyEmitted();

  EXPECT_TRUE(OnCompletionRun) << "Should have been marked ready";
}

TEST_F(CoreAPIsStandardTest, ExecutionSessionFailQuery) {
  bool OnCompletionRun = false;

  auto OnCompletion = [&](Expected<SymbolMap> Result) {
    EXPECT_FALSE(!!Result) << "Resolution unexpectedly returned success";
    auto Msg = toString(Result.takeError());
    EXPECT_EQ(Msg, "xyz") << "Resolution returned incorrect result";
    OnCompletionRun = true;
  };

  AsynchronousSymbolQuery Q(SymbolNameSet({Foo}), SymbolState::Ready,
                            OnCompletion);

  ES.legacyFailQuery(Q,
                     make_error<StringError>("xyz", inconvertibleErrorCode()));

  EXPECT_TRUE(OnCompletionRun) << "OnCompletionCallback was not run";
}

TEST_F(CoreAPIsStandardTest, EmptyLookup) {
  bool OnCompletionRun = false;

  auto OnCompletion = [&](Expected<SymbolMap> Result) {
    cantFail(std::move(Result));
    OnCompletionRun = true;
  };

  ES.lookup(JITDylibSearchList({{&JD, false}}), {}, SymbolState::Ready,
            OnCompletion, NoDependenciesToRegister);

  EXPECT_TRUE(OnCompletionRun) << "OnCompletion was not run for empty query";
}

TEST_F(CoreAPIsStandardTest, RemoveSymbolsTest) {
  // Test that:
  // (1) Missing symbols generate a SymbolsNotFound error.
  // (2) Materializing symbols generate a SymbolCouldNotBeRemoved error.
  // (3) Removal of unmaterialized symbols triggers discard on the
  //     materialization unit.
  // (4) Removal of symbols destroys empty materialization units.
  // (5) Removal of materialized symbols works.

  // Foo will be fully materialized.
  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}})));

  // Bar will be unmaterialized.
  bool BarDiscarded = false;
  bool BarMaterializerDestructed = false;
  cantFail(JD.define(std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Bar, BarSym.getFlags()}}),
      [this](MaterializationResponsibility R) {
        ADD_FAILURE() << "Unexpected materialization of \"Bar\"";
        R.notifyResolved({{Bar, BarSym}});
        R.notifyEmitted();
      },
      [&](const JITDylib &JD, const SymbolStringPtr &Name) {
        EXPECT_EQ(Name, Bar) << "Expected \"Bar\" to be discarded";
        if (Name == Bar)
          BarDiscarded = true;
      },
      [&]() { BarMaterializerDestructed = true; })));

  // Baz will be in the materializing state initially, then
  // materialized for the final removal attempt.
  Optional<MaterializationResponsibility> BazR;
  cantFail(JD.define(std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Baz, BazSym.getFlags()}}),
      [&](MaterializationResponsibility R) { BazR.emplace(std::move(R)); },
      [](const JITDylib &JD, const SymbolStringPtr &Name) {
        ADD_FAILURE() << "\"Baz\" discarded unexpectedly";
      })));

  bool OnCompletionRun = false;
  ES.lookup(
      JITDylibSearchList({{&JD, false}}), {Foo, Baz}, SymbolState::Ready,
      [&](Expected<SymbolMap> Result) {
        cantFail(Result.takeError());
        OnCompletionRun = true;
      },
      NoDependenciesToRegister);

  {
    // Attempt 1: Search for a missing symbol, Qux.
    auto Err = JD.remove({Foo, Bar, Baz, Qux});
    EXPECT_TRUE(!!Err) << "Expected failure";
    EXPECT_TRUE(Err.isA<SymbolsNotFound>())
        << "Expected a SymbolsNotFound error";
    consumeError(std::move(Err));
  }

  {
    // Attempt 2: Search for a symbol that is still materializing, Baz.
    auto Err = JD.remove({Foo, Bar, Baz});
    EXPECT_TRUE(!!Err) << "Expected failure";
    EXPECT_TRUE(Err.isA<SymbolsCouldNotBeRemoved>())
        << "Expected a SymbolsNotFound error";
    consumeError(std::move(Err));
  }

  BazR->notifyResolved({{Baz, BazSym}});
  BazR->notifyEmitted();
  {
    // Attempt 3: Search now that all symbols are fully materialized
    // (Foo, Baz), or not yet materialized (Bar).
    auto Err = JD.remove({Foo, Bar, Baz});
    EXPECT_FALSE(!!Err) << "Expected failure";
  }

  EXPECT_TRUE(BarDiscarded) << "\"Bar\" should have been discarded";
  EXPECT_TRUE(BarMaterializerDestructed)
      << "\"Bar\"'s materializer should have been destructed";
  EXPECT_TRUE(OnCompletionRun) << "OnCompletion should have been run";
}

TEST_F(CoreAPIsStandardTest, ChainedJITDylibLookup) {
  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}})));

  auto &JD2 = ES.createJITDylib("JD2");

  bool OnCompletionRun = false;

  auto Q = std::make_shared<AsynchronousSymbolQuery>(
      SymbolNameSet({Foo}), SymbolState::Ready,
      [&](Expected<SymbolMap> Result) {
        cantFail(std::move(Result));
        OnCompletionRun = true;
      });

  cantFail(JD2.legacyLookup(Q, cantFail(JD.legacyLookup(Q, {Foo}))));

  EXPECT_TRUE(OnCompletionRun) << "OnCompletion was not run for empty query";
}

TEST_F(CoreAPIsStandardTest, LookupWithHiddenSymbols) {
  auto BarHiddenFlags = BarSym.getFlags() & ~JITSymbolFlags::Exported;
  auto BarHiddenSym = JITEvaluatedSymbol(BarSym.getAddress(), BarHiddenFlags);

  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}, {Bar, BarHiddenSym}})));

  auto &JD2 = ES.createJITDylib("JD2");
  cantFail(JD2.define(absoluteSymbols({{Bar, QuxSym}})));

  /// Try a blocking lookup.
  auto Result = cantFail(
      ES.lookup(JITDylibSearchList({{&JD, false}, {&JD2, false}}), {Foo, Bar}));

  EXPECT_EQ(Result.size(), 2U) << "Unexpected number of results";
  EXPECT_EQ(Result.count(Foo), 1U) << "Missing result for \"Foo\"";
  EXPECT_EQ(Result.count(Bar), 1U) << "Missing result for \"Bar\"";
  EXPECT_EQ(Result[Bar].getAddress(), QuxSym.getAddress())
      << "Wrong result for \"Bar\"";
}

TEST_F(CoreAPIsStandardTest, LookupFlagsTest) {
  // Test that lookupFlags works on a predefined symbol, and does not trigger
  // materialization of a lazy symbol. Make the lazy symbol weak to test that
  // the weak flag is propagated correctly.

  BarSym.setFlags(static_cast<JITSymbolFlags::FlagNames>(
      JITSymbolFlags::Exported | JITSymbolFlags::Weak));
  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Bar, BarSym.getFlags()}}),
      [](MaterializationResponsibility R) {
        llvm_unreachable("Symbol materialized on flags lookup");
      });

  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}})));
  cantFail(JD.define(std::move(MU)));

  SymbolNameSet Names({Foo, Bar, Baz});

  auto SymbolFlags = cantFail(JD.lookupFlags(Names));

  EXPECT_EQ(SymbolFlags.size(), 2U)
      << "Returned symbol flags contains unexpected results";
  EXPECT_EQ(SymbolFlags.count(Foo), 1U) << "Missing lookupFlags result for Foo";
  EXPECT_EQ(SymbolFlags[Foo], FooSym.getFlags())
      << "Incorrect flags returned for Foo";
  EXPECT_EQ(SymbolFlags.count(Bar), 1U)
      << "Missing  lookupFlags result for Bar";
  EXPECT_EQ(SymbolFlags[Bar], BarSym.getFlags())
      << "Incorrect flags returned for Bar";
}

TEST_F(CoreAPIsStandardTest, LookupWithGeneratorFailure) {

  class BadGenerator : public JITDylib::DefinitionGenerator {
  public:
    Expected<SymbolNameSet> tryToGenerate(JITDylib &,
                                          const SymbolNameSet &) override {
      return make_error<StringError>("BadGenerator", inconvertibleErrorCode());
    }
  };

  JD.addGenerator(std::make_unique<BadGenerator>());

  EXPECT_THAT_ERROR(JD.lookupFlags({Foo}).takeError(), Failed<StringError>())
      << "Generator failure did not propagate through lookupFlags";

  EXPECT_THAT_ERROR(
      ES.lookup(JITDylibSearchList({{&JD, false}}), SymbolNameSet({Foo}))
          .takeError(),
      Failed<StringError>())
      << "Generator failure did not propagate through lookup";
}

TEST_F(CoreAPIsStandardTest, TestBasicAliases) {
  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}, {Bar, BarSym}})));
  cantFail(JD.define(symbolAliases({{Baz, {Foo, JITSymbolFlags::Exported}},
                                    {Qux, {Bar, JITSymbolFlags::Weak}}})));
  cantFail(JD.define(absoluteSymbols({{Qux, QuxSym}})));

  auto Result = ES.lookup(JITDylibSearchList({{&JD, false}}), {Baz, Qux});
  EXPECT_TRUE(!!Result) << "Unexpected lookup failure";
  EXPECT_EQ(Result->count(Baz), 1U) << "No result for \"baz\"";
  EXPECT_EQ(Result->count(Qux), 1U) << "No result for \"qux\"";
  EXPECT_EQ((*Result)[Baz].getAddress(), FooSym.getAddress())
      << "\"Baz\"'s address should match \"Foo\"'s";
  EXPECT_EQ((*Result)[Qux].getAddress(), QuxSym.getAddress())
      << "The \"Qux\" alias should have been overriden";
}

TEST_F(CoreAPIsStandardTest, TestChainedAliases) {
  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}})));
  cantFail(JD.define(symbolAliases(
      {{Baz, {Bar, BazSym.getFlags()}}, {Bar, {Foo, BarSym.getFlags()}}})));

  auto Result = ES.lookup(JITDylibSearchList({{&JD, false}}), {Bar, Baz});
  EXPECT_TRUE(!!Result) << "Unexpected lookup failure";
  EXPECT_EQ(Result->count(Bar), 1U) << "No result for \"bar\"";
  EXPECT_EQ(Result->count(Baz), 1U) << "No result for \"baz\"";
  EXPECT_EQ((*Result)[Bar].getAddress(), FooSym.getAddress())
      << "\"Bar\"'s address should match \"Foo\"'s";
  EXPECT_EQ((*Result)[Baz].getAddress(), FooSym.getAddress())
      << "\"Baz\"'s address should match \"Foo\"'s";
}

TEST_F(CoreAPIsStandardTest, TestBasicReExports) {
  // Test that the basic use case of re-exporting a single symbol from another
  // JITDylib works.
  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}})));

  auto &JD2 = ES.createJITDylib("JD2");

  cantFail(JD2.define(reexports(JD, {{Bar, {Foo, BarSym.getFlags()}}})));

  auto Result = cantFail(ES.lookup(JITDylibSearchList({{&JD2, false}}), Bar));
  EXPECT_EQ(Result.getAddress(), FooSym.getAddress())
      << "Re-export Bar for symbol Foo should match FooSym's address";
}

TEST_F(CoreAPIsStandardTest, TestThatReExportsDontUnnecessarilyMaterialize) {
  // Test that re-exports do not materialize symbols that have not been queried
  // for.
  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}})));

  bool BarMaterialized = false;
  auto BarMU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Bar, BarSym.getFlags()}}),
      [&](MaterializationResponsibility R) {
        BarMaterialized = true;
        R.notifyResolved({{Bar, BarSym}});
        R.notifyEmitted();
      });

  cantFail(JD.define(BarMU));

  auto &JD2 = ES.createJITDylib("JD2");

  cantFail(JD2.define(reexports(
      JD, {{Baz, {Foo, BazSym.getFlags()}}, {Qux, {Bar, QuxSym.getFlags()}}})));

  auto Result = cantFail(ES.lookup(JITDylibSearchList({{&JD2, false}}), Baz));
  EXPECT_EQ(Result.getAddress(), FooSym.getAddress())
      << "Re-export Baz for symbol Foo should match FooSym's address";

  EXPECT_FALSE(BarMaterialized) << "Bar should not have been materialized";
}

TEST_F(CoreAPIsStandardTest, TestReexportsGenerator) {
  // Test that a re-exports generator can dynamically generate reexports.

  auto &JD2 = ES.createJITDylib("JD2");
  cantFail(JD2.define(absoluteSymbols({{Foo, FooSym}, {Bar, BarSym}})));

  auto Filter = [this](SymbolStringPtr Name) { return Name != Bar; };

  JD.addGenerator(std::make_unique<ReexportsGenerator>(JD2, false, Filter));

  auto Flags = cantFail(JD.lookupFlags({Foo, Bar, Baz}));
  EXPECT_EQ(Flags.size(), 1U) << "Unexpected number of results";
  EXPECT_EQ(Flags[Foo], FooSym.getFlags()) << "Unexpected flags for Foo";

  auto Result = cantFail(ES.lookup(JITDylibSearchList({{&JD, false}}), Foo));

  EXPECT_EQ(Result.getAddress(), FooSym.getAddress())
      << "Incorrect reexported symbol address";
}

TEST_F(CoreAPIsStandardTest, TestTrivialCircularDependency) {
  Optional<MaterializationResponsibility> FooR;
  auto FooMU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}}),
      [&](MaterializationResponsibility R) { FooR.emplace(std::move(R)); });

  cantFail(JD.define(FooMU));

  bool FooReady = false;
  auto OnCompletion = [&](Expected<SymbolMap> Result) {
    cantFail(std::move(Result));
    FooReady = true;
  };

  ES.lookup(JITDylibSearchList({{&JD, false}}), {Foo}, SymbolState::Ready,
            OnCompletion, NoDependenciesToRegister);

  FooR->notifyResolved({{Foo, FooSym}});
  FooR->notifyEmitted();

  EXPECT_TRUE(FooReady)
    << "Self-dependency prevented symbol from being marked ready";
}

TEST_F(CoreAPIsStandardTest, TestCircularDependenceInOneJITDylib) {
  // Test that a circular symbol dependency between three symbols in a JITDylib
  // does not prevent any symbol from becoming 'ready' once all symbols are
  // emitted.

  // Create three MaterializationResponsibility objects: one for each of Foo,
  // Bar and Baz. These are optional because MaterializationResponsibility
  // does not have a default constructor).
  Optional<MaterializationResponsibility> FooR;
  Optional<MaterializationResponsibility> BarR;
  Optional<MaterializationResponsibility> BazR;

  // Create a MaterializationUnit for each symbol that moves the
  // MaterializationResponsibility into one of the locals above.
  auto FooMU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}}),
      [&](MaterializationResponsibility R) { FooR.emplace(std::move(R)); });

  auto BarMU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Bar, BarSym.getFlags()}}),
      [&](MaterializationResponsibility R) { BarR.emplace(std::move(R)); });

  auto BazMU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Baz, BazSym.getFlags()}}),
      [&](MaterializationResponsibility R) { BazR.emplace(std::move(R)); });

  // Define the symbols.
  cantFail(JD.define(FooMU));
  cantFail(JD.define(BarMU));
  cantFail(JD.define(BazMU));

  // Query each of the symbols to trigger materialization.
  bool FooResolved = false;
  bool FooReady = false;

  auto OnFooResolution = [&](Expected<SymbolMap> Result) {
    cantFail(std::move(Result));
    FooResolved = true;
  };

  auto OnFooReady = [&](Expected<SymbolMap> Result) {
    cantFail(std::move(Result));
    FooReady = true;
  };

  // Issue lookups for Foo. Use NoDependenciesToRegister: We're going to add
  // the dependencies manually below.
  ES.lookup(JITDylibSearchList({{&JD, false}}), {Foo}, SymbolState::Resolved,
            std::move(OnFooResolution), NoDependenciesToRegister);

  ES.lookup(JITDylibSearchList({{&JD, false}}), {Foo}, SymbolState::Ready,
            std::move(OnFooReady), NoDependenciesToRegister);

  bool BarResolved = false;
  bool BarReady = false;
  auto OnBarResolution = [&](Expected<SymbolMap> Result) {
    cantFail(std::move(Result));
    BarResolved = true;
  };

  auto OnBarReady = [&](Expected<SymbolMap> Result) {
    cantFail(std::move(Result));
    BarReady = true;
  };

  ES.lookup(JITDylibSearchList({{&JD, false}}), {Bar}, SymbolState::Resolved,
            std::move(OnBarResolution), NoDependenciesToRegister);

  ES.lookup(JITDylibSearchList({{&JD, false}}), {Bar}, SymbolState::Ready,
            std::move(OnBarReady), NoDependenciesToRegister);

  bool BazResolved = false;
  bool BazReady = false;

  auto OnBazResolution = [&](Expected<SymbolMap> Result) {
    cantFail(std::move(Result));
    BazResolved = true;
  };

  auto OnBazReady = [&](Expected<SymbolMap> Result) {
    cantFail(std::move(Result));
    BazReady = true;
  };

  ES.lookup(JITDylibSearchList({{&JD, false}}), {Baz}, SymbolState::Resolved,
            std::move(OnBazResolution), NoDependenciesToRegister);

  ES.lookup(JITDylibSearchList({{&JD, false}}), {Baz}, SymbolState::Ready,
            std::move(OnBazReady), NoDependenciesToRegister);

  // Add a circular dependency: Foo -> Bar, Bar -> Baz, Baz -> Foo.
  FooR->addDependenciesForAll({{&JD, SymbolNameSet({Bar})}});
  BarR->addDependenciesForAll({{&JD, SymbolNameSet({Baz})}});
  BazR->addDependenciesForAll({{&JD, SymbolNameSet({Foo})}});

  // Add self-dependencies for good measure. This tests that the implementation
  // of addDependencies filters these out.
  FooR->addDependenciesForAll({{&JD, SymbolNameSet({Foo})}});
  BarR->addDependenciesForAll({{&JD, SymbolNameSet({Bar})}});
  BazR->addDependenciesForAll({{&JD, SymbolNameSet({Baz})}});

  // Check that nothing has been resolved yet.
  EXPECT_FALSE(FooResolved) << "\"Foo\" should not be resolved yet";
  EXPECT_FALSE(BarResolved) << "\"Bar\" should not be resolved yet";
  EXPECT_FALSE(BazResolved) << "\"Baz\" should not be resolved yet";

  // Resolve the symbols (but do not emit them).
  FooR->notifyResolved({{Foo, FooSym}});
  BarR->notifyResolved({{Bar, BarSym}});
  BazR->notifyResolved({{Baz, BazSym}});

  // Verify that the symbols have been resolved, but are not ready yet.
  EXPECT_TRUE(FooResolved) << "\"Foo\" should be resolved now";
  EXPECT_TRUE(BarResolved) << "\"Bar\" should be resolved now";
  EXPECT_TRUE(BazResolved) << "\"Baz\" should be resolved now";

  EXPECT_FALSE(FooReady) << "\"Foo\" should not be ready yet";
  EXPECT_FALSE(BarReady) << "\"Bar\" should not be ready yet";
  EXPECT_FALSE(BazReady) << "\"Baz\" should not be ready yet";

  // Emit two of the symbols.
  FooR->notifyEmitted();
  BarR->notifyEmitted();

  // Verify that nothing is ready until the circular dependence is resolved.
  EXPECT_FALSE(FooReady) << "\"Foo\" still should not be ready";
  EXPECT_FALSE(BarReady) << "\"Bar\" still should not be ready";
  EXPECT_FALSE(BazReady) << "\"Baz\" still should not be ready";

  // Emit the last symbol.
  BazR->notifyEmitted();

  // Verify that everything becomes ready once the circular dependence resolved.
  EXPECT_TRUE(FooReady) << "\"Foo\" should be ready now";
  EXPECT_TRUE(BarReady) << "\"Bar\" should be ready now";
  EXPECT_TRUE(BazReady) << "\"Baz\" should be ready now";
}

TEST_F(CoreAPIsStandardTest, DropMaterializerWhenEmpty) {
  bool DestructorRun = false;

  JITSymbolFlags WeakExported(JITSymbolFlags::Exported);
  WeakExported |= JITSymbolFlags::Weak;

  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, WeakExported}, {Bar, WeakExported}}),
      [](MaterializationResponsibility R) {
        llvm_unreachable("Unexpected call to materialize");
      },
      [&](const JITDylib &JD, SymbolStringPtr Name) {
        EXPECT_TRUE(Name == Foo || Name == Bar)
            << "Discard of unexpected symbol?";
      },
      [&]() { DestructorRun = true; });

  cantFail(JD.define(MU));

  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}})));

  EXPECT_FALSE(DestructorRun)
      << "MaterializationUnit should not have been destroyed yet";

  cantFail(JD.define(absoluteSymbols({{Bar, BarSym}})));

  EXPECT_TRUE(DestructorRun)
      << "MaterializationUnit should have been destroyed";
}

TEST_F(CoreAPIsStandardTest, AddAndMaterializeLazySymbol) {
  bool FooMaterialized = false;
  bool BarDiscarded = false;

  JITSymbolFlags WeakExported(JITSymbolFlags::Exported);
  WeakExported |= JITSymbolFlags::Weak;

  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, JITSymbolFlags::Exported}, {Bar, WeakExported}}),
      [&](MaterializationResponsibility R) {
        assert(BarDiscarded && "Bar should have been discarded by this point");
        R.notifyResolved(SymbolMap({{Foo, FooSym}}));
        R.notifyEmitted();
        FooMaterialized = true;
      },
      [&](const JITDylib &JD, SymbolStringPtr Name) {
        EXPECT_EQ(Name, Bar) << "Expected Name to be Bar";
        BarDiscarded = true;
      });

  cantFail(JD.define(MU));
  cantFail(JD.define(absoluteSymbols({{Bar, BarSym}})));

  SymbolNameSet Names({Foo});

  bool OnCompletionRun = false;

  auto OnCompletion = [&](Expected<SymbolMap> Result) {
    EXPECT_TRUE(!!Result) << "Resolution unexpectedly returned error";
    auto I = Result->find(Foo);
    EXPECT_NE(I, Result->end()) << "Could not find symbol definition";
    EXPECT_EQ(I->second.getAddress(), FooSym.getAddress())
        << "Resolution returned incorrect result";
    OnCompletionRun = true;
  };

  ES.lookup(JITDylibSearchList({{&JD, false}}), Names, SymbolState::Ready,
            std::move(OnCompletion), NoDependenciesToRegister);

  EXPECT_TRUE(FooMaterialized) << "Foo was not materialized";
  EXPECT_TRUE(BarDiscarded) << "Bar was not discarded";
  EXPECT_TRUE(OnCompletionRun) << "OnResolutionCallback was not run";
}

TEST_F(CoreAPIsStandardTest, TestBasicWeakSymbolMaterialization) {
  // Test that weak symbols are materialized correctly when we look them up.
  BarSym.setFlags(BarSym.getFlags() | JITSymbolFlags::Weak);

  bool BarMaterialized = false;
  auto MU1 = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}, {Bar, BarSym.getFlags()}}),
      [&](MaterializationResponsibility R) {
        R.notifyResolved(SymbolMap({{Foo, FooSym}, {Bar, BarSym}})), R.notifyEmitted();
        BarMaterialized = true;
      });

  bool DuplicateBarDiscarded = false;
  auto MU2 = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Bar, BarSym.getFlags()}}),
      [&](MaterializationResponsibility R) {
        ADD_FAILURE() << "Attempt to materialize Bar from the wrong unit";
        R.failMaterialization();
      },
      [&](const JITDylib &JD, SymbolStringPtr Name) {
        EXPECT_EQ(Name, Bar) << "Expected \"Bar\" to be discarded";
        DuplicateBarDiscarded = true;
      });

  cantFail(JD.define(MU1));
  cantFail(JD.define(MU2));

  bool OnCompletionRun = false;

  auto OnCompletion = [&](Expected<SymbolMap> Result) {
    cantFail(std::move(Result));
    OnCompletionRun = true;
  };

  ES.lookup(JITDylibSearchList({{&JD, false}}), {Bar}, SymbolState::Ready,
            std::move(OnCompletion), NoDependenciesToRegister);

  EXPECT_TRUE(OnCompletionRun) << "OnCompletion not run";
  EXPECT_TRUE(BarMaterialized) << "Bar was not materialized at all";
  EXPECT_TRUE(DuplicateBarDiscarded)
      << "Duplicate bar definition not discarded";
}

TEST_F(CoreAPIsStandardTest, DefineMaterializingSymbol) {
  bool ExpectNoMoreMaterialization = false;
  ES.setDispatchMaterialization(
      [&](JITDylib &JD, std::unique_ptr<MaterializationUnit> MU) {
        if (ExpectNoMoreMaterialization)
          ADD_FAILURE() << "Unexpected materialization";
        MU->doMaterialize(JD);
      });

  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}}),
      [&](MaterializationResponsibility R) {
        cantFail(
            R.defineMaterializing(SymbolFlagsMap({{Bar, BarSym.getFlags()}})));
        R.notifyResolved(SymbolMap({{Foo, FooSym}, {Bar, BarSym}}));
        R.notifyEmitted();
      });

  cantFail(JD.define(MU));
  cantFail(ES.lookup(JITDylibSearchList({{&JD, false}}), Foo));

  // Assert that materialization is complete by now.
  ExpectNoMoreMaterialization = true;

  // Look up bar to verify that no further materialization happens.
  auto BarResult = cantFail(ES.lookup(JITDylibSearchList({{&JD, false}}), Bar));
  EXPECT_EQ(BarResult.getAddress(), BarSym.getAddress())
      << "Expected Bar == BarSym";
}

TEST_F(CoreAPIsStandardTest, GeneratorTest) {
  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}})));

  class TestGenerator : public JITDylib::DefinitionGenerator {
  public:
    TestGenerator(SymbolMap Symbols) : Symbols(std::move(Symbols)) {}
    Expected<SymbolNameSet> tryToGenerate(JITDylib &JD,
                                          const SymbolNameSet &Names) {
      SymbolMap NewDefs;
      SymbolNameSet NewNames;

      for (auto &Name : Names) {
        if (Symbols.count(Name)) {
          NewDefs[Name] = Symbols[Name];
          NewNames.insert(Name);
        }
      }
      cantFail(JD.define(absoluteSymbols(std::move(NewDefs))));
      return NewNames;
    };

  private:
    SymbolMap Symbols;
  };

  JD.addGenerator(std::make_unique<TestGenerator>(SymbolMap({{Bar, BarSym}})));

  auto Result =
      cantFail(ES.lookup(JITDylibSearchList({{&JD, false}}), {Foo, Bar}));

  EXPECT_EQ(Result.count(Bar), 1U) << "Expected to find fallback def for 'bar'";
  EXPECT_EQ(Result[Bar].getAddress(), BarSym.getAddress())
      << "Expected fallback def for Bar to be equal to BarSym";
}

TEST_F(CoreAPIsStandardTest, FailResolution) {
  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, JITSymbolFlags::Exported | JITSymbolFlags::Weak},
                      {Bar, JITSymbolFlags::Exported | JITSymbolFlags::Weak}}),
      [&](MaterializationResponsibility R) {
        R.failMaterialization();
      });

  cantFail(JD.define(MU));

  SymbolNameSet Names({Foo, Bar});
  auto Result = ES.lookup(JITDylibSearchList({{&JD, false}}), Names);

  EXPECT_FALSE(!!Result) << "Expected failure";
  if (!Result) {
    handleAllErrors(Result.takeError(),
                    [&](FailedToMaterialize &F) {
                      EXPECT_EQ(F.getSymbols(), Names)
                          << "Expected to fail on symbols in Names";
                    },
                    [](ErrorInfoBase &EIB) {
                      std::string ErrMsg;
                      {
                        raw_string_ostream ErrOut(ErrMsg);
                        EIB.log(ErrOut);
                      }
                      ADD_FAILURE()
                          << "Expected a FailedToResolve error. Got:\n"
                          << ErrMsg;
                    });
  }
}

TEST_F(CoreAPIsStandardTest, FailEmissionEarly) {

  cantFail(JD.define(absoluteSymbols({{Baz, BazSym}})));

  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}, {Bar, BarSym.getFlags()}}),
      [&](MaterializationResponsibility R) {
        R.notifyResolved(SymbolMap({{Foo, FooSym}, {Bar, BarSym}}));

        ES.lookup(
            JITDylibSearchList({{&JD, false}}), SymbolNameSet({Baz}),
            SymbolState::Resolved,
            [&R](Expected<SymbolMap> Result) {
              // Called when "baz" is resolved. We don't actually depend
              // on or care about baz, but use it to trigger failure of
              // this materialization before Baz has been finalized in
              // order to test that error propagation is correct in this
              // scenario.
              cantFail(std::move(Result));
              R.failMaterialization();
            },
            [&](const SymbolDependenceMap &Deps) {
              R.addDependenciesForAll(Deps);
            });
      });

  cantFail(JD.define(MU));

  SymbolNameSet Names({Foo, Bar});
  auto Result = ES.lookup(JITDylibSearchList({{&JD, false}}), Names);

  EXPECT_THAT_EXPECTED(std::move(Result), Failed())
      << "Unexpected success while trying to test error propagation";
}

TEST_F(CoreAPIsStandardTest, TestLookupWithUnthreadedMaterialization) {
  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, JITSymbolFlags::Exported}}),
      [&](MaterializationResponsibility R) {
        R.notifyResolved({{Foo, FooSym}});
        R.notifyEmitted();
      });

  cantFail(JD.define(MU));

  auto FooLookupResult =
      cantFail(ES.lookup(JITDylibSearchList({{&JD, false}}), Foo));

  EXPECT_EQ(FooLookupResult.getAddress(), FooSym.getAddress())
      << "lookup returned an incorrect address";
  EXPECT_EQ(FooLookupResult.getFlags(), FooSym.getFlags())
      << "lookup returned incorrect flags";
}

TEST_F(CoreAPIsStandardTest, TestLookupWithThreadedMaterialization) {
#if LLVM_ENABLE_THREADS

  std::thread MaterializationThread;
  ES.setDispatchMaterialization(
      [&](JITDylib &JD, std::unique_ptr<MaterializationUnit> MU) {
        auto SharedMU = std::shared_ptr<MaterializationUnit>(std::move(MU));
        MaterializationThread =
            std::thread([SharedMU, &JD]() { SharedMU->doMaterialize(JD); });
      });

  cantFail(JD.define(absoluteSymbols({{Foo, FooSym}})));

  auto FooLookupResult =
      cantFail(ES.lookup(JITDylibSearchList({{&JD, false}}), Foo));

  EXPECT_EQ(FooLookupResult.getAddress(), FooSym.getAddress())
      << "lookup returned an incorrect address";
  EXPECT_EQ(FooLookupResult.getFlags(), FooSym.getFlags())
      << "lookup returned incorrect flags";
  MaterializationThread.join();
#endif
}

TEST_F(CoreAPIsStandardTest, TestGetRequestedSymbolsAndReplace) {
  // Test that GetRequestedSymbols returns the set of symbols that currently
  // have pending queries, and test that MaterializationResponsibility's
  // replace method can be used to return definitions to the JITDylib in a new
  // MaterializationUnit.
  SymbolNameSet Names({Foo, Bar});

  bool FooMaterialized = false;
  bool BarMaterialized = false;

  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}, {Bar, BarSym.getFlags()}}),
      [&](MaterializationResponsibility R) {
        auto Requested = R.getRequestedSymbols();
        EXPECT_EQ(Requested.size(), 1U) << "Expected one symbol requested";
        EXPECT_EQ(*Requested.begin(), Foo) << "Expected \"Foo\" requested";

        auto NewMU = std::make_unique<SimpleMaterializationUnit>(
            SymbolFlagsMap({{Bar, BarSym.getFlags()}}),
            [&](MaterializationResponsibility R2) {
              R2.notifyResolved(SymbolMap({{Bar, BarSym}}));
              R2.notifyEmitted();
              BarMaterialized = true;
            });

        R.replace(std::move(NewMU));

        R.notifyResolved(SymbolMap({{Foo, FooSym}}));
        R.notifyEmitted();

        FooMaterialized = true;
      });

  cantFail(JD.define(MU));

  EXPECT_FALSE(FooMaterialized) << "Foo should not be materialized yet";
  EXPECT_FALSE(BarMaterialized) << "Bar should not be materialized yet";

  auto FooSymResult =
      cantFail(ES.lookup(JITDylibSearchList({{&JD, false}}), Foo));
  EXPECT_EQ(FooSymResult.getAddress(), FooSym.getAddress())
      << "Address mismatch for Foo";

  EXPECT_TRUE(FooMaterialized) << "Foo should be materialized now";
  EXPECT_FALSE(BarMaterialized) << "Bar still should not be materialized";

  auto BarSymResult =
      cantFail(ES.lookup(JITDylibSearchList({{&JD, false}}), Bar));
  EXPECT_EQ(BarSymResult.getAddress(), BarSym.getAddress())
      << "Address mismatch for Bar";
  EXPECT_TRUE(BarMaterialized) << "Bar should be materialized now";
}

TEST_F(CoreAPIsStandardTest, TestMaterializationResponsibilityDelegation) {
  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}, {Bar, BarSym.getFlags()}}),
      [&](MaterializationResponsibility R) {
        auto R2 = R.delegate({Bar});

        R.notifyResolved({{Foo, FooSym}});
        R.notifyEmitted();
        R2.notifyResolved({{Bar, BarSym}});
        R2.notifyEmitted();
      });

  cantFail(JD.define(MU));

  auto Result = ES.lookup(JITDylibSearchList({{&JD, false}}), {Foo, Bar});

  EXPECT_TRUE(!!Result) << "Result should be a success value";
  EXPECT_EQ(Result->count(Foo), 1U) << "\"Foo\" entry missing";
  EXPECT_EQ(Result->count(Bar), 1U) << "\"Bar\" entry missing";
  EXPECT_EQ((*Result)[Foo].getAddress(), FooSym.getAddress())
      << "Address mismatch for \"Foo\"";
  EXPECT_EQ((*Result)[Bar].getAddress(), BarSym.getAddress())
      << "Address mismatch for \"Bar\"";
}

TEST_F(CoreAPIsStandardTest, TestMaterializeWeakSymbol) {
  // Confirm that once a weak definition is selected for materialization it is
  // treated as strong.
  JITSymbolFlags WeakExported = JITSymbolFlags::Exported;
  WeakExported &= JITSymbolFlags::Weak;

  std::unique_ptr<MaterializationResponsibility> FooResponsibility;
  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}}),
      [&](MaterializationResponsibility R) {
        FooResponsibility =
            std::make_unique<MaterializationResponsibility>(std::move(R));
      });

  cantFail(JD.define(MU));
  auto OnCompletion = [](Expected<SymbolMap> Result) {
    cantFail(std::move(Result));
  };

  ES.lookup(JITDylibSearchList({{&JD, false}}), {Foo}, SymbolState::Ready,
            std::move(OnCompletion), NoDependenciesToRegister);

  auto MU2 = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, JITSymbolFlags::Exported}}),
      [](MaterializationResponsibility R) {
        llvm_unreachable("This unit should never be materialized");
      });

  auto Err = JD.define(MU2);
  EXPECT_TRUE(!!Err) << "Expected failure value";
  EXPECT_TRUE(Err.isA<DuplicateDefinition>())
      << "Expected a duplicate definition error";
  consumeError(std::move(Err));

  FooResponsibility->notifyResolved(SymbolMap({{Foo, FooSym}}));
  FooResponsibility->notifyEmitted();
}

} // namespace
