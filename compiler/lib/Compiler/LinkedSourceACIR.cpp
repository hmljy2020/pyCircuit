#include "LinkedSourceACIR.h"
#include "Dialect/ACIR/ACIRSourceContracts.h"
#include "SourceHeaderHelpers.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/AttrTypeSubElements.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "pycircuit/Dialect/ACIR/SourceMethodAnalysis.h"
#include "pycircuit/Dialect/ACIR/SourceTransactionStateAnalysis.h"
#include "pycircuit/Dialect/ACIR/SourceUnitValidation.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;

namespace acir::compiler {

FailureOr<OwningOpRef<ModuleOp>>
mergeSourceLinkDefinitions(ArrayRef<SourceLinkUnit> units) {
  SmallVector<SourceLinkUnit> orderedUnits(units);
  llvm::sort(orderedUnits,
             [](const SourceLinkUnit &left, const SourceLinkUnit &right) {
               return ac::detail::compareClosedSourceStructure(
                          left.header->getAttr("ac.source_owner"),
                          right.header->getAttr("ac.source_owner")) < 0;
             });
  units = orderedUnits;
  MLIRContext *context = units.front().body->getContext();
  OwningOpRef<ModuleOp> linked(ModuleOp::create(UnknownLoc::get(context)));
  Builder builder(context);
  SmallVector<Attribute> owners;
  for (SourceLinkUnit unit : units)
    owners.push_back(unit.header->getAttr("ac.source_owner"));
  (*linked)->setAttr("ac.source_units", builder.getArrayAttr(owners));
  llvm::StringMap<Operation *> symbols;
  for (SourceLinkUnit unit : units) {
    for (Operation &operation : unit.body.getBody()->getOperations()) {
      auto name = SymbolTable::getSymbolName(&operation);
      if (!name)
        return operation.emitOpError(
            "source-unit top-level operation must be a declaration");
      auto prior = symbols.find(name.getValue());
      if (prior != symbols.end()) {
        bool oldDefinition = isa<ac::ModuleOp>(prior->second);
        bool newDefinition = isa<ac::ModuleOp>(operation);
        if (oldDefinition && isa<ac::ModuleImportOp>(operation))
          continue;
        if (newDefinition && isa<ac::ModuleImportOp>(prior->second)) {
          prior->second->erase();
          symbols.erase(prior);
        } else if (detail::sameDeclaration(prior->second, &operation) &&
                   !newDefinition) {
          auto role =
              operation.getAttrOfType<StringAttr>("ac.declaration_role");
          auto priorRole =
              prior->second->getAttrOfType<StringAttr>("ac.declaration_role");
          // Every pair already passed owning-body authority. Prefer that real
          // nominal definition over an equivalent consumer snapshot, preserving
          // provider metadata and location independently of link input order.
          if (isa<ac::StructOp, ac::EnumOp, ac::ConstantOp>(operation) &&
              role.getValue() == "definition" &&
              priorRole.getValue() == "import_snapshot") {
            prior->second->erase();
            symbols.erase(prior);
          } else {
            continue;
          }
        } else {
          return operation.emitOpError("conflicting linked declaration: ")
                 << name;
        }
      }
      Operation *copy = operation.clone();
      (*linked).getBody()->push_back(copy);
      symbols[name.getValue()] = copy;
    }
  }
  return linked;
}

ac::ModuleOp findSourceLinkEntry(ModuleOp package, StringRef top,
                                 ac::detail::EmitError emitError) {
  auto root =
      dyn_cast_or_null<ac::ModuleOp>(SymbolTable::lookupSymbolIn(package, top));
  if (!root)
    emitError() << "link top is not a supplied module definition: @" << top;
  return root;
}

void addSourceLinkEntry(ModuleOp package, ac::ModuleOp root,
                        ArrayRef<Attribute> arguments,
                        StringRef resetSemantics) {
  auto *context = package.getContext();
  Builder builder(context);
  OperationState state(root.getLoc(), ac::SystemOp::getOperationName());
  state.addAttribute(
      "entry",
      builder.getDictionaryAttr(
          {builder.getNamedAttr(
               "callee", FlatSymbolRefAttr::get(context, root.getSymName())),
           builder.getNamedAttr("parameters", builder.getArrayAttr(arguments)),
           builder.getNamedAttr("type_arguments", builder.getArrayAttr({}))}));
  state.addAttribute("domain", builder.getStringAttr("default"));
  if (!resetSemantics.empty())
    state.addAttribute("ac.execution_policy",
                       builder.getDictionaryAttr({builder.getNamedAttr(
                           "reset", builder.getStringAttr(resetSemantics))}));
  package.getBody()->push_back(Operation::create(state));
}

namespace {

FailureOr<SmallVector<Attribute>> entryDefaults(ac::ModuleOp root) {
  if (root->hasAttr("ac.reference_parameters") ||
      !root.getTypeParameters().empty())
    return root.emitOpError() << "linked source top requires a closed Module "
                                 "reference and type signature";
  SmallVector<Attribute> defaults;
  for (Attribute raw : root.getParameters()) {
    auto parameter = cast<DictionaryAttr>(raw);
    auto value = parameter.getAs<ac::StaticExprAttr>("default");
    if (!value)
      return root.emitOpError() << "linked source top integer parameter needs "
                                   "a default: "
                                << parameter.get("name");
    defaults.push_back(value);
  }
  return defaults;
}

LogicalResult verifySymbolClosure(ModuleOp package,
                                  const SourceHeaderRegistry &catalog) {
  SymbolTable symbols(package);
  auto owners = package->getAttrOfType<ArrayAttr>("ac.source_units");
  if (!owners || owners.empty())
    return package.emitOpError() << "linked source requires source-unit owners";
  llvm::DenseSet<Attribute> uniqueOwners;
  DictionaryAttr previous;
  for (Attribute raw : owners) {
    auto owner = dyn_cast<DictionaryAttr>(raw);
    if (failed(ac::detail::verifySourceOwner(
            owner, [&] { return package.emitOpError(); })) ||
        (previous &&
         ac::detail::compareClosedSourceStructure(previous, owner) >= 0))
      return package.emitOpError() << "linked source owners must be unique "
                                      "and structurally ordered";
    uniqueOwners.insert(owner);
    previous = owner;
  }

  for (Operation &operation : package.getBody()->getOperations()) {
    if (isa<ac::SystemOp>(operation))
      continue;
    auto role = operation.getAttrOfType<StringAttr>("ac.declaration_role");
    if (auto imported = dyn_cast<ac::ModuleImportOp>(operation)) {
      if (!role || role.getValue() != "import_snapshot" ||
          !catalog.isTrustedBuiltin(imported))
        return imported.emitOpError() << "linked source imports require "
                                         "canonical builtin declarations";
      continue;
    }
    if (!isa<ac::ModuleOp, ac::TypeAliasOp, ac::ConstantOp, ac::StructOp,
             ac::EnumOp, func::FuncOp>(operation))
      return operation.emitOpError() << "unexpected linked source declaration";
    auto name = SymbolTable::getSymbolName(&operation);
    if (name && catalog.lookupDeclaration(FlatSymbolRefAttr::get(name)))
      return operation.emitOpError()
             << "linked declaration conflicts with a builtin";
    auto owner = ac::detail::declarationSourceOwner(&operation);
    if (!uniqueOwners.contains(owner) ||
        !ac::detail::hasQualifiedDeclarationIdentityForOwner(&operation, owner))
      return operation.emitOpError()
             << "linked declaration has no matching source-unit owner";
  }

  Operation *site = package;
  AttrTypeWalker walker;
  auto lookup = [&](StringRef name) { return symbols.lookup(name); };
  auto missing = [&](StringRef name) {
    site->emitOpError() << "linked source has no declaration for @" << name;
    return WalkResult::interrupt();
  };
  // Source locations and occurrence histories can identify expanded source
  // declarations that were never hardware symbols.
  auto isProvenance = [](StringRef name) {
    return name == "origin" || name == "ac.origin" || name == "occurrence" ||
           name == "declaration" || name == "location" ||
           name == "ac.location" || name == "ac.guard_location";
  };
  auto checkAttribute = [&](Attribute attr) {
    return walker.walk<WalkOrder::PreOrder>(attr);
  };
  walker.addWalk([&](DictionaryAttr fields) {
    for (NamedAttribute field : fields)
      if (!isProvenance(field.getName().getValue()) &&
          checkAttribute(field.getValue()).wasInterrupted())
        return WalkResult::interrupt();
    return WalkResult::skip();
  });
  walker.addWalk([&](SymbolRefAttr symbol) {
    auto name = symbol.getRootReference().getValue();
    if (!lookup(name))
      return missing(name);
    // Method and instance paths are checked against their bound providers.
    return WalkResult::skip();
  });
  walker.addWalk([&](ac::StructType type) {
    auto name = type.getName().getValue();
    return isa_and_nonnull<ac::StructOp>(lookup(name)) ? WalkResult::advance()
                                                       : missing(name);
  });
  walker.addWalk([&](ac::EnumType type) {
    auto name = type.getName().getValue();
    return isa_and_nonnull<ac::EnumOp>(lookup(name)) ? WalkResult::advance()
                                                     : missing(name);
  });

  auto result = package.walk([&](Operation *operation) {
    site = operation;
    for (NamedAttribute attribute : operation->getAttrs())
      if (!isProvenance(attribute.getName().getValue()) &&
          checkAttribute(attribute.getValue()).wasInterrupted())
        return WalkResult::interrupt();
    for (Type type : operation->getResultTypes())
      if (walker.walk<WalkOrder::PreOrder>(type).wasInterrupted())
        return WalkResult::interrupt();
    for (Region &region : operation->getRegions())
      for (Block &block : region)
        for (Type type : block.getArgumentTypes())
          if (walker.walk<WalkOrder::PreOrder>(type).wasInterrupted())
            return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return success(!result.wasInterrupted());
}

LogicalResult verifyRegistrationOccurrences(ModuleOp package) {
  for (auto module : package.getOps<ac::ModuleOp>()) {
    auto &body = module.getBody().front();
    if (body.getOps<ac::SourceMethodCallOp>().empty())
      continue;
    SmallVector<std::pair<DictionaryAttr, Operation *>> registrations;
    for (Operation &operation : body) {
      if (isa<ac::SourceMethodCallOp, ac::SourceTransactionOp>(operation))
        registrations.emplace_back(
            operation.getAttrOfType<DictionaryAttr>("occurrence"), &operation);
    }
    auto compare = [](const auto &left, const auto &right) {
      return ac::detail::compareClosedSourceStructure(left.first, right.first);
    };
    llvm::stable_sort(registrations, [&](const auto &left, const auto &right) {
      return compare(left, right) < 0;
    });
    for (auto [left, right] :
         llvm::zip(registrations, ArrayRef(registrations).drop_front()))
      if (compare(left, right) == 0)
        return right.second->emitOpError() << "registration occurrence must be unique";
  }
  return success();
}

struct SimplifyLinkedRegistrationsPass
    : PassWrapper<SimplifyLinkedRegistrationsPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SimplifyLinkedRegistrationsPass)

  StringRef getArgument() const final {
    return "ac-simplify-linked-registrations";
  }
  StringRef getDescription() const final {
    return "Replace transparent linked source Rule registrations with Module calls";
  }
  void runOnOperation() final {
    ModuleOp package = getOperation();
    OwningOpRef<ModuleOp> candidate(cast<ModuleOp>(package->clone()));
    if (failed(verifyLinkedSourceACIR(*candidate)))
      return signalPassFailure();

    for (auto module : candidate->getOps<ac::ModuleOp>()) {
      auto transactions = module.getBody().front().getOps<ac::SourceTransactionOp>();
      for (auto transaction : llvm::make_early_inc_range(transactions)) {
        auto entry = ac::getSourceRegistrationEntry(transaction);
        if (failed(entry))
          return signalPassFailure();
        if (!*entry)
          continue;

        auto &registration = **entry;
        OpBuilder builder(transaction);
        IRMapping mapping;
        Value path = registration.call.getPath();
        builder.clone(*path.getDefiningOp(), mapping);
        for (auto [argument, ordinal] : llvm::zip_equal(
                 registration.call.getArguments(),
                 registration.argumentCaptureOrdinals))
          mapping.map(argument, transaction.getCaptures()[ordinal]);
        auto call = cast<ac::SourceMethodCallOp>(
            builder.clone(*registration.call, mapping));
        call->setAttr("ac.registration_name", transaction.getNameAttr());
        for (auto [result, replacement] :
             llvm::zip(transaction.getResults(), call.getResults())) {
          // The wrapper can carry distinct type provenance on its public SSA.
          replacement.setType(result.getType());
          result.replaceAllUsesWith(replacement);
        }
        transaction.erase();
      }
    }
    if (failed(verifyLinkedSourceACIR(*candidate)))
      return signalPassFailure();
    package.getBodyRegion().takeBody(candidate->getBodyRegion());
  }
};

struct ElideLinkedTrueGuardsPass
    : PassWrapper<ElideLinkedTrueGuardsPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ElideLinkedTrueGuardsPass)

  StringRef getArgument() const final { return "ac-elide-linked-true-guards"; }
  StringRef getDescription() const final {
    return "Omit literal true Method guards and call paths in linked source ACIR";
  }
  void runOnOperation() final {
    ModuleOp package = getOperation();
    OwningOpRef<ModuleOp> candidate(cast<ModuleOp>(package->clone()));
    if (failed(verifyLinkedSourceACIR(*candidate)))
      return signalPassFailure();

    auto registrations = candidate->walk([](ac::SourceTransactionOp transaction) {
      if (!transaction->hasAttr("ac.registration_entry"))
        return WalkResult::advance();
      transaction.emitOpError()
          << "run --ac-simplify-linked-registrations before eliding conditions";
      return WalkResult::interrupt();
    });
    if (registrations.wasInterrupted())
      return signalPassFailure();

    candidate->walk([](ac::SourceMethodOp method) {
      auto &guard = method.getGuard().front();
      auto yield = cast<ac::YieldOp>(guard.getTerminator());
      auto values = yield.getValues();
      if (values.size() == 1 &&
          llvm::hasSingleElement(guard.without_terminator()) &&
          values.front().getDefiningOp() == &guard.front() &&
          ac::isSourceLiteralTrue(values.front()))
        yield->setOperands(ValueRange{});
    });
    auto marker = UnitAttr::get(package.getContext());
    candidate->walk([&](ac::SourceMethodCallOp call) {
      if (ac::isSourceLiteralTrue(call.getPath())) {
        call->eraseOperands(0);
        call->setAttr("ac.implicit_path", marker);
      }
    });
    (*candidate)->setAttr("ac.linked_implicit_guards", marker);
    if (failed(verifyLinkedSourceACIR(*candidate)))
      return signalPassFailure();
    package->setAttr("ac.linked_implicit_guards", marker);
    package.getBodyRegion().takeBody(candidate->getBodyRegion());
  }
};

} // namespace

void registerSimplifyLinkedRegistrationsPass() {
  PassRegistration<SimplifyLinkedRegistrationsPass>();
}

void registerElideLinkedTrueGuardsPass() {
  PassRegistration<ElideLinkedTrueGuardsPass>();
}

LogicalResult verifyLinkedSourceACIR(ModuleOp package) {
  if (!ac::detail::isLinkedSourcePackage(package))
    return package.emitOpError() << "expected ac.stage = linked-source";
  if (failed(ac::verifySourceOperations(package)) ||
      failed(verifyRegistrationOccurrences(package)))
    return failure();
  auto systems = package.getOps<ac::SystemOp>();
  if (!llvm::hasSingleElement(systems))
    return package.emitOpError()
           << "linked source requires exactly one ac.system";
  auto system = *systems.begin();
  auto entry = system.getEntry();
  auto emitError = [&] { return system.emitOpError(); };
  auto root = findSourceLinkEntry(
      package, entry.getAs<FlatSymbolRefAttr>("callee").getValue(), emitError);
  if (!root)
    return failure();
  auto defaults = entryDefaults(root);
  if (failed(defaults))
    return failure();
  if (!entry.getAs<ArrayAttr>("type_arguments").empty() ||
      !llvm::equal(entry.getAs<ArrayAttr>("parameters"), *defaults))
    return emitError() << "linked source entry must use the declared defaults";
  auto catalog =
      SourceHeaderRegistry::create(package.getContext(), {}, emitError);
  if (failed(catalog) || failed(verifySymbolClosure(package, *catalog)))
    return failure();
  return verifyLinkedMethodGuards(package);
}

FailureOr<OwningOpRef<ModuleOp>>
linkSourceACIR(ArrayRef<SourceLinkUnit> units, StringRef top,
               ac::detail::EmitError emitError) {
  if (failed(admitSourceLinkUnits(units, emitError)))
    return failure();
  auto linked = mergeSourceLinkDefinitions(units);
  if (failed(linked))
    return failure();
  auto root = findSourceLinkEntry(**linked, top, emitError);
  if (!root)
    return failure();
  auto defaults = entryDefaults(root);
  if (failed(defaults))
    return failure();
  (**linked)->setAttr("ac.stage",
                      StringAttr::get(root.getContext(), "linked-source"));
  addSourceLinkEntry(**linked, root, *defaults, {});
  if (failed(verifyLinkedSourceACIR(**linked)))
    return failure();
  return std::move(*linked);
}

} // namespace acir::compiler
