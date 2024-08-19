//===--- SubstitutionMap.cpp - Type substitution map ----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the SubstitutionMap class. A SubstitutionMap packages
// together a set of replacement types and protocol conformances for
// specializing generic types.
//
// SubstitutionMaps either have type parameters or archetypes as keys,
// based on whether they were built from a GenericSignature or a
// GenericEnvironment.
//
// To specialize a type, call Type::subst() with the right SubstitutionMap.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/SubstitutionMap.h"
#include "SubstitutionMapStorage.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ConformanceLookup.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/InFlightSubstitution.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "llvm/Support/Debug.h"

using namespace swift;

SubstitutionMap::Storage::Storage(
                              GenericSignature genericSig,
                              ArrayRef<Type> replacementTypes,
                              ArrayRef<ProtocolConformanceRef> conformances)
  : genericSig(genericSig),
    numConformanceRequirements(genericSig->getNumConformanceRequirements())
{
  assert(replacementTypes.size() == getNumReplacementTypes());
  assert(conformances.size() == numConformanceRequirements);

  std::copy(replacementTypes.begin(), replacementTypes.end(),
            getReplacementTypes().data());
  std::copy(conformances.begin(), conformances.end(),
            getConformances().data());
}

SubstitutionMap::SubstitutionMap(
                                GenericSignature genericSig,
                                ArrayRef<Type> replacementTypes,
                                ArrayRef<ProtocolConformanceRef> conformances)
  : storage(Storage::get(genericSig, replacementTypes, conformances)) {
#ifndef NDEBUG
  if (genericSig->getASTContext().LangOpts.VerifyAllSubstitutionMaps)
    verify();
#endif
}

ArrayRef<ProtocolConformanceRef> SubstitutionMap::getConformances() const {
  return storage ? storage->getConformances()
                 : ArrayRef<ProtocolConformanceRef>();
}

ArrayRef<Type> SubstitutionMap::getReplacementTypes() const {
  if (empty()) return { };

  return storage->getReplacementTypes();
}

ArrayRef<Type> SubstitutionMap::getInnermostReplacementTypes() const {
  if (empty()) return { };

  return getReplacementTypes().take_back(
      getGenericSignature().getInnermostGenericParams().size());
}

GenericSignature SubstitutionMap::getGenericSignature() const {
  return storage ? storage->getGenericSignature() : nullptr;
}

bool SubstitutionMap::empty() const {
  return getGenericSignature().isNull();
}

bool SubstitutionMap::hasAnySubstitutableParams() const {
  auto genericSig = getGenericSignature();
  if (!genericSig) return false;

  return !genericSig->areAllParamsConcrete();
}

RecursiveTypeProperties SubstitutionMap::getRecursiveProperties() const {
  RecursiveTypeProperties properties;
  for (auto replacementTy : getReplacementTypes())
    properties |= replacementTy->getRecursiveProperties();
  return properties;
}

bool SubstitutionMap::isCanonical() const {
  if (empty()) return true;

  if (!getGenericSignature()->isCanonical()) return false;

  for (Type replacementTy : getReplacementTypes()) {
    if (!replacementTy->isCanonical())
      return false;
  }

  for (auto conf : getConformances()) {
    if (!conf.isCanonical())
      return false;
  }

  return true;
}

SubstitutionMap SubstitutionMap::getCanonical(bool canonicalizeSignature) const {
  if (empty()) return *this;

  auto sig = getGenericSignature();
  if (canonicalizeSignature) sig = sig.getCanonicalSignature();

  SmallVector<Type, 4> replacementTypes;
  for (Type replacementType : getReplacementTypes()) {
    replacementTypes.push_back(replacementType->getCanonicalType());
  }

  SmallVector<ProtocolConformanceRef, 4> conformances;
  for (auto conf : getConformances()) {
    conformances.push_back(conf.getCanonicalConformanceRef());
  }

  return SubstitutionMap::get(sig,
                              ArrayRef<Type>(replacementTypes),
                              ArrayRef<ProtocolConformanceRef>(conformances));
}

SubstitutionMap SubstitutionMap::get(GenericSignature genericSig,
                                     SubstitutionMap substitutions) {
  if (!genericSig)
    return SubstitutionMap();

  return SubstitutionMap::get(genericSig,
           [&](SubstitutableType *type) -> Type {
             return substitutions.lookupSubstitution(
                cast<SubstitutableType>(type->getCanonicalType()));
           },
           LookUpConformanceInSubstitutionMap(substitutions));
}

SubstitutionMap SubstitutionMap::get(GenericSignature genericSig,
                                     TypeSubstitutionFn subs,
                                     LookupConformanceFn lookupConformance) {
  InFlightSubstitution IFS(subs, lookupConformance, std::nullopt);
  return get(genericSig, IFS);
}

SubstitutionMap SubstitutionMap::get(GenericSignature genericSig,
                                     ArrayRef<Type> types,
                                     LookupConformanceFn lookupConformance) {
  return get(genericSig,
             QueryReplacementTypeArray{genericSig, types},
             lookupConformance);
}

SubstitutionMap SubstitutionMap::get(GenericSignature genericSig,
                                     InFlightSubstitution &IFS) {
  if (!genericSig) {
    return SubstitutionMap();
  }

  // Form the replacement types.
  SmallVector<Type, 4> replacementTypes;
  replacementTypes.reserve(genericSig.getGenericParams().size());

  for (auto *gp : genericSig.getGenericParams()) {
    // Record the replacement.
    Type replacement = Type(gp).subst(IFS);

    assert((!replacement || replacement->hasError() ||
            gp->isParameterPack() == replacement->is<PackType>()) &&
           "replacement for pack parameter must be a pack type");

    replacementTypes.push_back(replacement);
  }

  // Form the stored conformances.
  SmallVector<ProtocolConformanceRef, 4> conformances;
  for (const auto &req : genericSig.getRequirements()) {
    if (req.getKind() != RequirementKind::Conformance) continue;

    CanType depTy = req.getFirstType()->getCanonicalType();
    auto replacement = depTy.subst(IFS);
    auto *proto = req.getProtocolDecl();
    auto conformance = IFS.lookupConformance(depTy, replacement, proto,
                                             /*level=*/0);
    conformances.push_back(conformance);
  }

  return SubstitutionMap(genericSig, replacementTypes, conformances);
}

Type SubstitutionMap::lookupSubstitution(CanSubstitutableType type) const {
  if (empty())
    return Type();

  // If we have an archetype, map out of the context so we can compute a
  // conformance access path.
  if (auto archetype = dyn_cast<ArchetypeType>(type)) {
    // Only consider root archetypes.
    if (!archetype->isRoot())
      return Type();

    if (!isa<PrimaryArchetypeType>(archetype) &&
        !isa<PackArchetypeType>(archetype))
      return Type();

    type = cast<GenericTypeParamType>(
      archetype->getInterfaceType()->getCanonicalType());
  }

  // Find the index of the replacement type based on the generic parameter we
  // have.
  GenericSignature genericSig = getGenericSignature();
  auto genericParam = cast<GenericTypeParamType>(type);
  auto genericParams = genericSig.getGenericParams();
  auto replacementIndex =
    GenericParamKey(genericParam).findIndexIn(genericParams);

  // If this generic parameter isn't represented, we don't have a replacement
  // type for it.
  if (replacementIndex == genericParams.size())
    return Type();

  return getReplacementTypes()[replacementIndex];
}

ProtocolConformanceRef
SubstitutionMap::lookupConformance(CanType type, ProtocolDecl *proto) const {
  if (empty())
    return ProtocolConformanceRef::forInvalid();

  // If we have an archetype, map out of the context so we can compute a
  // conformance access path.
  if (auto archetype = dyn_cast<ArchetypeType>(type)) {
    if (!isa<OpaqueTypeArchetypeType>(archetype)) {
      type = archetype->getInterfaceType()->getCanonicalType();
    }
  }

  // Error path: if we don't have a type parameter, there is no conformance.
  // FIXME: Query concrete conformances in the generic signature?
  if (!type->isTypeParameter())
    return ProtocolConformanceRef::forInvalid();

  auto genericSig = getGenericSignature();

  auto getSignatureConformance =
      [&](Type type,
          ProtocolDecl *proto) -> std::optional<ProtocolConformanceRef> {
    unsigned index = 0;
    for (auto reqt : genericSig.getRequirements()) {
      if (reqt.getKind() == RequirementKind::Conformance) {
        if (reqt.getFirstType()->isEqual(type) &&
            reqt.getProtocolDecl() == proto)
          return getConformances()[index];

        ++index;
      }
    }

    return std::nullopt;
  };

  // Fast path -- check if the generic signature directly states the
  // conformance.
  if (auto directConformance = getSignatureConformance(type, proto))
    return *directConformance;

  // If the type doesn't conform to this protocol, the result isn't formed
  // from these requirements.
  if (!genericSig->requiresProtocol(type, proto)) {
    Type substType = type.subst(*this);
    return ProtocolConformanceRef::forMissingOrInvalid(substType, proto);
  }

  // If the protocol is invertible, fall back to a global lookup instead of
  // evaluating a conformance path, to avoid an infinite substitution issue.
  if (proto->getInvertibleProtocolKind()) {
    auto substType = type.subst(*this);
    if (!substType->isTypeParameter())
      return swift::lookupConformance(substType, proto);
    return ProtocolConformanceRef(proto);
  }

  auto path = genericSig->getConformancePath(type, proto);

  ProtocolConformanceRef conformance;
  for (const auto &step : path) {
    // For the first step, grab the initial conformance.
    if (conformance.isInvalid()) {
      if (auto initialConformance = getSignatureConformance(
            step.first, step.second)) {
        conformance = *initialConformance;
        continue;
      }

      // We couldn't find the initial conformance, fail.
      return ProtocolConformanceRef::forInvalid();
    }

    // If we've hit an abstract conformance, everything from here on out is
    // abstract.
    // FIXME: This may not always be true, but it holds for now.
    if (conformance.isAbstract()) {
      // FIXME: Rip this out once we can get a concrete conformance from
      // an archetype.
      auto substType = type.subst(*this);
      if (substType->hasError())
        return ProtocolConformanceRef(proto);

      if ((!substType->is<ArchetypeType>() ||
           substType->castTo<ArchetypeType>()->getSuperclass()) &&
          !substType->isTypeParameter() &&
          !substType->isExistentialType()) {
        return swift::lookupConformance(substType, proto);
      }

      return ProtocolConformanceRef(proto);
    }

    // For the second step, we're looking into the requirement signature for
    // this protocol.
    if (conformance.isPack()) {
      auto pack = conformance.getPack();
      conformance = ProtocolConformanceRef(
          pack->getAssociatedConformance(step.first, step.second));
      if (conformance.isInvalid())
        return conformance;

      continue;
    }

    auto concrete = conformance.getConcrete();
    auto normal = concrete->getRootNormalConformance();

    // If we haven't set the signature conformances yet, force the issue now.
    if (!normal->hasComputedAssociatedConformances()) {
      // If we're in the process of checking the type witnesses, fail
      // gracefully.
      //
      // FIXME: This is unsound, because we may not have diagnosed anything but
      // still end up with an ErrorType in the AST.
      if (proto->getASTContext().evaluator.hasActiveRequest(
            ResolveTypeWitnessesRequest{normal})) {
        return ProtocolConformanceRef::forInvalid();
      }
    }

    // Get the associated conformance.
    conformance = concrete->getAssociatedConformance(step.first, step.second);
    if (conformance.isInvalid())
      return conformance;
  }

  return conformance;
}

SubstitutionMap SubstitutionMap::mapReplacementTypesOutOfContext() const {
  return subst(MapTypeOutOfContext(),
               MakeAbstractConformanceForGenericType(),
               SubstFlags::PreservePackExpansionLevel |
               SubstFlags::SubstitutePrimaryArchetypes);
}

SubstitutionMap SubstitutionMap::subst(SubstitutionMap subMap,
                                       SubstOptions options) const {
  InFlightSubstitutionViaSubMap IFS(subMap, options);
  return subst(IFS);
}

SubstitutionMap SubstitutionMap::subst(TypeSubstitutionFn subs,
                                       LookupConformanceFn conformances,
                                       SubstOptions options) const {
  InFlightSubstitution IFS(subs, conformances, options);
  return subst(IFS);
}

SubstitutionMap SubstitutionMap::subst(InFlightSubstitution &IFS) const {
  if (empty()) return SubstitutionMap();

  SmallVector<Type, 4> newSubs;
  for (Type type : getReplacementTypes()) {
    newSubs.push_back(type.subst(IFS));
    assert(type->is<PackType>() == newSubs.back()->is<PackType>() &&
           "substitution changed the pack-ness of a replacement type");
  }

  SmallVector<ProtocolConformanceRef, 4> newConformances;
  auto oldConformances = getConformances();

  auto genericSig = getGenericSignature();
  for (const auto &req : genericSig.getRequirements()) {
    if (req.getKind() != RequirementKind::Conformance) continue;

    auto conformance = oldConformances[0];

    // Fast path for concrete case -- we don't need to compute substType
    // at all.
    if (conformance.isConcrete() &&
        !IFS.shouldSubstituteOpaqueArchetypes()) {
      newConformances.push_back(
        ProtocolConformanceRef(conformance.getConcrete()->subst(IFS)));
    } else {
      auto origType = req.getFirstType();
      auto substType = origType.subst(*this, IFS.getOptions());

      newConformances.push_back(conformance.subst(substType, IFS));
    }
    
    oldConformances = oldConformances.slice(1);
  }

  assert(oldConformances.empty());
  return SubstitutionMap(genericSig, newSubs, newConformances);
}

SubstitutionMap
SubstitutionMap::getProtocolSubstitutions(ProtocolDecl *protocol,
                                          Type selfType,
                                          ProtocolConformanceRef conformance) {
  return get(protocol->getGenericSignature(), llvm::ArrayRef<Type>(selfType),
             llvm::ArrayRef<ProtocolConformanceRef>(conformance));
}

SubstitutionMap
SubstitutionMap::getOverrideSubstitutions(
                                      const ValueDecl *baseDecl,
                                      const ValueDecl *derivedDecl) {
  // For overrides within a protocol hierarchy, substitute the Self type.
  if (auto baseProto = baseDecl->getDeclContext()->getSelfProtocolDecl()) {
    auto baseSig = baseDecl->getInnermostDeclContext()
        ->getGenericSignatureOfContext();
    return baseSig->getIdentitySubstitutionMap();
  }

  auto *baseClass = baseDecl->getDeclContext()->getSelfClassDecl();
  auto *derivedClass = derivedDecl->getDeclContext()->getSelfClassDecl();

  auto baseSig = baseDecl->getInnermostDeclContext()
      ->getGenericSignatureOfContext();

  // If more kinds of overridable decls with generic parameter lists appear,
  // add them here.
  GenericParamList *derivedParams = nullptr;
  if (auto *funcDecl = dyn_cast<AbstractFunctionDecl>(derivedDecl))
    derivedParams = funcDecl->getGenericParams();
  else if (auto *subscriptDecl = dyn_cast<SubscriptDecl>(derivedDecl))
    derivedParams = subscriptDecl->getGenericParams();

  return getOverrideSubstitutions(baseClass, derivedClass, baseSig, derivedParams);
}

OverrideSubsInfo::OverrideSubsInfo(const NominalTypeDecl *baseNominal,
                                   const NominalTypeDecl *derivedNominal,
                                   GenericSignature baseSig,
                                   const GenericParamList *derivedParams)
  : Ctx(baseSig->getASTContext()),
    BaseDepth(0),
    OrigDepth(0),
    DerivedParams(derivedParams) {

  if (auto baseNominalSig = baseNominal->getGenericSignature()) {
    BaseDepth = baseNominalSig.getNextDepth();

    auto *genericEnv = derivedNominal->getGenericEnvironment();
    auto derivedNominalTy = derivedNominal->getDeclaredInterfaceType();

    // FIXME: Map in and out of context to get more accurate
    // conformance information. If the base generic signature
    // is <T: P> and the derived generic signature is <T: C>
    // where C is a class that conforms to P, then we want the
    // substitution map to store the concrete conformance C: P
    // and not the abstract conformance T: P.
    if (genericEnv) {
      derivedNominalTy = genericEnv->mapTypeIntoContext(
          derivedNominalTy);
    }

    BaseSubMap = derivedNominalTy->getContextSubstitutionMap(
        baseNominal, genericEnv);

    BaseSubMap = BaseSubMap.mapReplacementTypesOutOfContext();
  }

  if (auto derivedNominalSig = derivedNominal->getGenericSignature())
    OrigDepth = derivedNominalSig.getNextDepth();
}

Type QueryOverrideSubs::operator()(SubstitutableType *type) const {
  if (auto gp = type->getAs<GenericTypeParamType>()) {
    if (gp->getDepth() >= info.BaseDepth) {
      assert(gp->getDepth() == info.BaseDepth);
      if (info.DerivedParams != nullptr) {
        return info.DerivedParams->getParams()[gp->getIndex()]
            ->getDeclaredInterfaceType();
      }

      return GenericTypeParamType::get(
          gp->isParameterPack(),
          gp->getDepth() + info.OrigDepth - info.BaseDepth,
          gp->getIndex(), info.Ctx);
    }
  }

  return Type(type).subst(info.BaseSubMap);
}

ProtocolConformanceRef
LookUpConformanceInOverrideSubs::operator()(CanType type,
                                            Type substType,
                                            ProtocolDecl *proto) const {
  if (type->getRootGenericParam()->getDepth() >= info.BaseDepth)
    return ProtocolConformanceRef(proto);

  if (auto conformance = info.BaseSubMap.lookupConformance(type, proto))
    return conformance;

  if (substType->isTypeParameter())
    return ProtocolConformanceRef(proto);

  return lookupConformance(substType, proto);
}

SubstitutionMap
SubstitutionMap::getOverrideSubstitutions(const NominalTypeDecl *baseNominal,
                                          const NominalTypeDecl *derivedNominal,
                                          GenericSignature baseSig,
                                          const GenericParamList *derivedParams) {
  if (baseSig.isNull())
    return SubstitutionMap();

  OverrideSubsInfo info(baseNominal, derivedNominal, baseSig, derivedParams);

  return get(baseSig,
             QueryOverrideSubs(info),
             LookUpConformanceInOverrideSubs(info));
}

SubstitutionMap
SubstitutionMap::combineSubstitutionMaps(SubstitutionMap firstSubMap,
                                         SubstitutionMap secondSubMap,
                                         CombineSubstitutionMaps how,
                                         unsigned firstDepthOrIndex,
                                         unsigned secondDepthOrIndex,
                                         GenericSignature genericSig) {
  auto &ctx = genericSig->getASTContext();

  auto replaceGenericParameter = [&](Type type) -> std::optional<Type> {
    if (auto gp = type->getAs<GenericTypeParamType>()) {
      if (how == CombineSubstitutionMaps::AtDepth) {
        if (gp->getDepth() < firstDepthOrIndex)
          return Type();
        return Type(GenericTypeParamType::get(gp->isParameterPack(),
                                              gp->getDepth() + secondDepthOrIndex -
                                                  firstDepthOrIndex,
                                              gp->getIndex(), ctx));
      }

      assert(how == CombineSubstitutionMaps::AtIndex);
      if (gp->getIndex() < firstDepthOrIndex)
        return Type();
      return Type(GenericTypeParamType::get(
          gp->isParameterPack(), gp->getDepth(),
          gp->getIndex() + secondDepthOrIndex - firstDepthOrIndex, ctx));
    }

    return std::nullopt;
  };

  return get(
    genericSig,
    [&](SubstitutableType *type) {
      if (auto replacement = replaceGenericParameter(type))
        if (*replacement)
          return replacement->subst(secondSubMap);
      return Type(type).subst(firstSubMap);
    },
    [&](CanType type, Type substType, ProtocolDecl *proto) {
      if (auto replacement = type.transformRec(replaceGenericParameter))
        return secondSubMap.lookupConformance(replacement->getCanonicalType(),
                                              proto);
      if (auto conformance = firstSubMap.lookupConformance(type, proto))
        return conformance;

      // We might not have enough information in the substitution maps alone.
      //
      // Eg,
      //
      // class Base<T1> {
      //   func foo<U1>(_: U1) where T1 : P {}
      // }
      //
      // class Derived<T2> : Base<Foo<T2>> {
      //   override func foo<U2>(_: U2) where T2 : Q {}
      // }
      //
      // Suppose we're devirtualizing a call to Base.foo() on a value whose
      // type is known to be Derived<Bar>. We start with substitutions written
      // in terms of Base.foo()'s generic signature:
      //
      // <T1, U1 where T1 : P>
      // T1 := Foo<Bar>
      // T1 : P := Foo<Bar> : P
      //
      // We want to build substitutions in terms of Derived.foo()'s
      // generic signature:
      //
      // <T2, U2 where T2 : Q>
      // T2 := Bar
      // T2 : Q := Bar : Q
      //
      // The conformance Bar : Q is difficult to recover in the general case.
      //
      // Some combination of storing substitution maps in BoundGenericTypes
      // as well as for method overrides would solve this, but for now, just
      // punt to module lookup.
      if (substType->isTypeParameter())
        return ProtocolConformanceRef(proto);

      return swift::lookupConformance(substType, proto);
    });
}

void SubstitutionMap::verify() const {
#ifndef NDEBUG
  if (empty())
    return;

  unsigned conformanceIndex = 0;

  for (const auto &req : getGenericSignature().getRequirements()) {
    if (req.getKind() != RequirementKind::Conformance)
      continue;

    SWIFT_DEFER { ++conformanceIndex; };
    auto substType = req.getFirstType().subst(*this);
    if (substType->isTypeParameter() ||
        substType->is<ArchetypeType>() ||
        substType->isTypeVariableOrMember() ||
        substType->is<UnresolvedType>() ||
        substType->hasError())
      continue;

    auto conformance = getConformances()[conformanceIndex];

    if (conformance.isInvalid())
      continue;

    // All of the conformances should be concrete.
    if (!conformance.isConcrete()) {
      llvm::dbgs() << "Concrete type cannot have abstract conformance:\n";
      substType->dump(llvm::dbgs());
      llvm::dbgs() << "SubstitutionMap:\n";
      dump(llvm::dbgs());
      llvm::dbgs() << "\n";
      llvm::dbgs() << "Requirement:\n";
      req.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }
    assert(conformance.isConcrete() && "Conformance should be concrete");
    
    if (substType->is<UnboundGenericType>())
      continue;
    
    auto conformanceTy = conformance.getConcrete()->getType();
    if (conformanceTy->hasTypeParameter()
        && !substType->hasTypeParameter()) {
      conformanceTy = conformance.getConcrete()->getDeclContext()
        ->mapTypeIntoContext(conformanceTy);
    }
    
    if (!substType->isEqual(conformanceTy)) {
      llvm::dbgs() << "Conformance must match concrete replacement type:\n";
      substType->dump(llvm::dbgs());
      llvm::dbgs() << "Conformance type:\n";
      conformance.getConcrete()->getType()->dump(llvm::dbgs());
      llvm::dbgs() << "Conformance:\n";
      conformance.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
      llvm::dbgs() << "SubstitutionMap:\n";
      dump(llvm::dbgs());
      llvm::dbgs() << "\n";
      llvm::dbgs() << "Requirement:\n";
      req.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }
    assert(substType->isEqual(conformanceTy)
           && "conformance should match corresponding type");

    if (substType->isExistentialType()) {
      assert(isa<SelfProtocolConformance>(conformance.getConcrete()) &&
              "Existential type cannot have normal conformance");
    }
  }
#endif
}

void SubstitutionMap::profile(llvm::FoldingSetNodeID &id) const {
  id.AddPointer(storage);
}

bool SubstitutionMap::isIdentity() const {
  if (empty())
    return true;

  for (auto conf : getConformances()) {
    if (conf.isAbstract())
      continue;

    if (conf.isPack()) {
      auto patternConfs = conf.getPack()->getPatternConformances();
      if (patternConfs.size() == 1 && patternConfs[0].isAbstract())
        continue;
    }

    return false;
  }

  GenericSignature sig = getGenericSignature();
  bool hasNonIdentityReplacement = false;
  auto replacements = getReplacementTypes();

  sig->forEachParam([&](GenericTypeParamType *paramTy, bool isCanonical) {
    if (isCanonical) {
      Type wrappedParamTy = paramTy;
      if (paramTy->isParameterPack())
        wrappedParamTy = PackType::getSingletonPackExpansion(paramTy);
      if (!wrappedParamTy->isEqual(replacements[0]))
        hasNonIdentityReplacement = true;
    }

    replacements = replacements.slice(1);
  });

  assert(replacements.empty());

  return !hasNonIdentityReplacement;
}

SubstitutionMap SubstitutionMap::mapIntoTypeExpansionContext(
    TypeExpansionContext context) const {
  ReplaceOpaqueTypesWithUnderlyingTypes replacer(
      context.getContext(), context.getResilienceExpansion(),
      context.isWholeModuleContext());
  return this->subst(replacer, replacer,
                     SubstFlags::SubstituteOpaqueArchetypes |
                     SubstFlags::PreservePackExpansionLevel);
}
