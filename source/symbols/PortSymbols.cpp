//------------------------------------------------------------------------------
// PortSymbols.cpp
// Contains port-related symbol definitions
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/symbols/PortSymbols.h"

#include "slang/binding/MiscExpressions.h"
#include "slang/compilation/Compilation.h"
#include "slang/compilation/Definition.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/symbols/ASTSerializer.h"
#include "slang/symbols/AttributeSymbol.h"
#include "slang/symbols/InstanceSymbols.h"
#include "slang/symbols/MemberSymbols.h"
#include "slang/symbols/VariableSymbols.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxFacts.h"
#include "slang/types/AllTypes.h"
#include "slang/types/NetType.h"
#include "slang/util/StackContainer.h"

namespace slang {

namespace {

const NetType& getDefaultNetType(const Scope& scope, SourceLocation location) {
    auto& netType = scope.getDefaultNetType();
    if (!netType.isError())
        return netType;

    scope.addDiag(diag::ImplicitNetPortNoDefault, location);
    return scope.getCompilation().getWireNetType();
}

std::tuple<const Definition*, string_view> getInterfacePortInfo(
    const Scope& scope, const InterfacePortHeaderSyntax& header) {

    auto& comp = scope.getCompilation();
    auto token = header.nameOrKeyword;
    auto def = comp.getDefinition(token.valueText(), scope);
    string_view modport;

    if (!def) {
        scope.addDiag(diag::UnknownInterface, token.range()) << token.valueText();
    }
    else if (def->definitionKind != DefinitionKind::Interface) {
        auto& diag = scope.addDiag(diag::PortTypeNotInterfaceOrData, header.nameOrKeyword.range());
        diag << def->name;
        diag.addNote(diag::NoteDeclarationHere, def->location);
        def = nullptr;
    }
    else if (header.modport) {
        auto member = header.modport->member;
        modport = member.valueText();
        if (auto it = def->modports.find(modport); it == def->modports.end() && !modport.empty()) {
            auto& diag = scope.addDiag(diag::NotAModport, member.range());
            diag << modport;
            diag << def->name;
            modport = {};
        }
    }

    return { def, modport };
}

// Helper class to build up lists of port symbols.
class AnsiPortListBuilder {
public:
    AnsiPortListBuilder(const Scope& scope,
                        SmallVector<std::pair<Symbol*, const Symbol*>>& implicitMembers) :
        compilation(scope.getCompilation()),
        scope(scope), implicitMembers(implicitMembers) {}

    Symbol* createPort(const ImplicitAnsiPortSyntax& syntax) {
        // Helper function to check if an implicit type syntax is totally empty.
        auto isEmpty = [](const DataTypeSyntax& typeSyntax) {
            if (typeSyntax.kind != SyntaxKind::ImplicitType)
                return false;

            const auto& implicitType = typeSyntax.as<ImplicitTypeSyntax>();
            return !implicitType.signing && implicitType.dimensions.empty();
        };

        auto& decl = *syntax.declarator;
        switch (syntax.header->kind) {
            case SyntaxKind::VariablePortHeader: {
                // A VariablePortHeader is parsed as a catch-all when we aren't sure what kind of
                // port this is. There are three components to a port that matter: kind, type,
                // direction. If all three are omitted, inherit them all from the previous port.
                // We'll never even get into this code path if the very first port omitted all three
                // because then it would be a non-ansi port list.
                auto& header = syntax.header->as<VariablePortHeaderSyntax>();
                if (!header.direction && !header.varKeyword && isEmpty(*header.dataType))
                    return addInherited(decl, syntax.attributes);

                // It's possible that this is actually an interface port if the data type is just an
                // identifier. The only way to know is to do a lookup and see what comes back.
                string_view simpleName = SyntaxFacts::getSimpleTypeName(*header.dataType);
                if (!simpleName.empty()) {
                    auto found = Lookup::unqualified(scope, simpleName, LookupFlags::Type);
                    if (found && found->kind == SymbolKind::NetType) {
                        return add(decl, getDirection(header.direction), nullptr,
                                   &found->as<NetType>(), syntax.attributes);
                    }

                    // If we didn't find a valid type, try to find a definition.
                    if (!found || !found->isType()) {
                        if (auto definition = compilation.getDefinition(simpleName, scope)) {
                            if (definition->definitionKind != DefinitionKind::Interface) {
                                auto& diag = scope.addDiag(diag::PortTypeNotInterfaceOrData,
                                                           header.dataType->sourceRange());
                                diag << definition->name;
                                diag.addNote(diag::NoteDeclarationHere, definition->location);
                                definition = nullptr;
                            }
                            else {
                                if (header.varKeyword) {
                                    scope.addDiag(diag::VarWithInterfacePort,
                                                  header.varKeyword.location());
                                }

                                if (header.direction) {
                                    scope.addDiag(diag::DirectionWithInterfacePort,
                                                  header.direction.location());
                                }
                            }

                            return add(decl, definition, ""sv, syntax.attributes);
                        }
                    }
                }

                // Rules from [23.2.2.3]:
                // - If we have a var keyword, it's a var
                // - For input and inout, default to a net
                // - For output, if we have a data type it's a var, otherwise net
                // - For ref it's always a var
                //
                // Unfortunately, all major simulators ignore the rule for input ports,
                // and treat them the same as output ports (i.e. it's not a net if there
                // is a data type specified). This is pretty noticeable as otherwise a
                // port like this:
                //    input int i
                // will throw an error because int is not a valid type for a net. Actually
                // noticing the other fact, that it's a net port vs a variable port, is very
                // hard to do, so we go along with everyone else and use the same rule.

                ArgumentDirection direction = getDirection(header.direction);
                const NetType* netType = nullptr;
                if (!header.varKeyword && (direction == ArgumentDirection::InOut ||
                                           (direction != ArgumentDirection::Ref &&
                                            header.dataType->kind == SyntaxKind::ImplicitType))) {
                    netType = &getDefaultNetType(scope, decl.name.location());
                }

                return add(decl, direction, header.dataType, netType, syntax.attributes);
            }
            case SyntaxKind::NetPortHeader: {
                auto& header = syntax.header->as<NetPortHeaderSyntax>();
                return add(decl, getDirection(header.direction), header.dataType,
                           &compilation.getNetType(header.netType.kind), syntax.attributes);
            }
            case SyntaxKind::InterfacePortHeader: {
                // TODO: handle generic interface header
                auto& header = syntax.header->as<InterfacePortHeaderSyntax>();
                auto [definition, modport] = getInterfacePortInfo(scope, header);
                return add(decl, definition, modport, syntax.attributes);
            }
            default:
                THROW_UNREACHABLE;
        }
    }

    Symbol* createPort(const ExplicitAnsiPortSyntax& syntax) {
        auto port = compilation.emplace<PortSymbol>(syntax.name.valueText(), syntax.name.location(),
                                                    DeclaredTypeFlags::InferImplicit);
        port->direction = getDirection(syntax.direction);
        port->setSyntax(syntax);
        port->setDeclaredType(compilation.createEmptyTypeSyntax(syntax.name.location()));
        port->setAttributes(scope, syntax.attributes);

        if (syntax.expr)
            port->setInitializerSyntax(*syntax.expr, syntax.expr->getFirstToken().location());

        lastDirection = port->direction;
        lastType = nullptr;
        lastNetType = nullptr;
        lastInterface = nullptr;
        lastModport = ""sv;

        return port;
    }

private:
    ArgumentDirection getDirection(Token token) const {
        return token ? SemanticFacts::getDirection(token.kind) : lastDirection;
    }

    Symbol* addInherited(const DeclaratorSyntax& decl,
                         span<const AttributeInstanceSyntax* const> attrs) {
        if (lastInterface)
            return add(decl, lastInterface, lastModport, attrs);

        if (!lastType && !lastNetType)
            lastType = &compilation.createEmptyTypeSyntax(decl.getFirstToken().location());

        return add(decl, lastDirection, lastType, lastNetType, attrs);
    }

    Symbol* add(const DeclaratorSyntax& decl, ArgumentDirection direction,
                const DataTypeSyntax* type, const NetType* netType,
                span<const AttributeInstanceSyntax* const> attrs) {

        auto port = compilation.emplace<PortSymbol>(decl.name.valueText(), decl.name.location());
        port->direction = direction;
        port->setSyntax(decl);
        port->setAttributes(scope, attrs);

        if (!port->name.empty()) {
            if (port->direction == ArgumentDirection::InOut && !netType)
                scope.addDiag(diag::InOutPortCannotBeVariable, port->location) << port->name;
            else if (port->direction == ArgumentDirection::Ref && netType)
                scope.addDiag(diag::RefPortMustBeVariable, port->location) << port->name;
        }

        // Create a new symbol to represent this port internally to the instance.
        ValueSymbol* symbol;
        if (netType) {
            symbol = compilation.emplace<NetSymbol>(port->name, port->location, *netType);
        }
        else {
            symbol = compilation.emplace<VariableSymbol>(port->name, port->location,
                                                         VariableLifetime::Static);
        }

        if (type) {
            // Symbol and port can't link their types here, they need to be independent.
            // This is due to the way we resolve connections - see the comment in
            // InstanceSymbol::resolvePortConnections for an example of a scenario that
            // would otherwise cause reentrant type resolution for the port symbol.
            symbol->setDeclaredType(*type, decl.dimensions);
            port->setDeclaredType(*type, decl.dimensions);
        }
        else {
            ASSERT(netType);
            if (!decl.dimensions.empty())
                symbol->getDeclaredType()->setDimensionSyntax(decl.dimensions);

            port->getDeclaredType()->copyTypeFrom(*symbol->getDeclaredType());
        }

        // Initializers here are evaluated in the context of the port list and
        // must always be a constant value.
        // TODO: handle initializers
        symbol->setSyntax(decl);
        symbol->setAttributes(scope, attrs);
        port->internalSymbol = symbol;
        implicitMembers.emplace(symbol, port);

        // Remember the properties of this port in case the next port wants to inherit from it.
        lastDirection = direction;
        lastType = type;
        lastNetType = netType;
        lastInterface = nullptr;
        lastModport = ""sv;

        return port;
    }

    Symbol* add(const DeclaratorSyntax& decl, const Definition* iface, string_view modport,
                span<const AttributeInstanceSyntax* const> attrs) {
        auto port =
            compilation.emplace<InterfacePortSymbol>(decl.name.valueText(), decl.name.location());

        if (iface)
            compilation.noteInterfacePort(*iface);

        port->interfaceDef = iface;
        port->modport = modport;
        port->setSyntax(decl);
        port->setAttributes(scope, attrs);

        lastDirection = ArgumentDirection::InOut;
        lastType = nullptr;
        lastNetType = nullptr;
        lastInterface = iface;
        lastModport = modport;

        return port;
    }

    Compilation& compilation;
    const Scope& scope;
    SmallVector<std::pair<Symbol*, const Symbol*>>& implicitMembers;

    ArgumentDirection lastDirection = ArgumentDirection::InOut;
    const DataTypeSyntax* lastType = nullptr;
    const NetType* lastNetType = nullptr;
    const Definition* lastInterface = nullptr;
    string_view lastModport;
};

class NonAnsiPortListBuilder {
public:
    NonAnsiPortListBuilder(
        const Scope& scope,
        span<std::pair<const PortDeclarationSyntax*, const Symbol*> const> portDeclarations,
        SmallVector<std::pair<Symbol*, const Symbol*>>& implicitMembers) :
        comp(scope.getCompilation()),
        scope(scope), implicitMembers(implicitMembers) {

        // All port declarations in the scope have been collected; index them for easy lookup.
        for (auto [port, insertionPoint] : portDeclarations) {
            for (auto decl : port->declarators) {
                if (auto name = decl->name; !name.isMissing()) {
                    auto [it, inserted] =
                        portInfos.emplace(name.valueText(), PortInfo{ *decl, port->attributes });

                    if (inserted) {
                        handleIODecl(*port->header, it->second, insertionPoint);
                    }
                    else {
                        auto& diag = scope.addDiag(diag::Redefinition, name.location());
                        diag << name.valueText();
                        diag.addNote(diag::NotePreviousDefinition,
                                     it->second.syntax->name.location());
                    }
                }
            }
        }
    }

    Symbol* createPort(const ImplicitNonAnsiPortSyntax& syntax) {
        auto loc = syntax.expr->getFirstToken().location();
        switch (syntax.expr->kind) {
            case SyntaxKind::PortReference:
                return &createPort(""sv, loc, syntax.expr->as<PortReferenceSyntax>());
            case SyntaxKind::PortConcatenation:
                return &createPort(""sv, loc, syntax.expr->as<PortConcatenationSyntax>());
            default:
                THROW_UNREACHABLE;
        }
    }

    Symbol* createPort(const ExplicitNonAnsiPortSyntax& syntax) {
        auto name = syntax.name.valueText();
        auto loc = syntax.name.location();

        if (!syntax.expr) {
            auto port = comp.emplace<PortSymbol>(name, loc);
            port->direction = ArgumentDirection::In;
            port->setSyntax(syntax);
            port->setType(comp.getVoidType()); // indicator that this is an empty port
            return port;
        }

        switch (syntax.expr->kind) {
            case SyntaxKind::PortReference:
                return &createPort(name, loc, syntax.expr->as<PortReferenceSyntax>());
            case SyntaxKind::PortConcatenation:
                return &createPort(name, loc, syntax.expr->as<PortConcatenationSyntax>());
            default:
                THROW_UNREACHABLE;
        }
    }

    Symbol* createPort(const EmptyNonAnsiPortSyntax& syntax) {
        auto port = comp.emplace<PortSymbol>("", syntax.placeholder.location());
        port->direction = ArgumentDirection::In;
        port->setSyntax(syntax);
        port->setType(comp.getVoidType()); // indicator that this is an empty port
        return port;
    }

    void finalize() {
        // Error if any port declarations are unused.
        for (auto& [name, info] : portInfos) {
            if (!info.used)
                scope.addDiag(diag::UnusedPortDecl, info.syntax->sourceRange()) << name;
        }
    }

private:
    Compilation& comp;
    const Scope& scope;
    SmallVector<std::pair<Symbol*, const Symbol*>>& implicitMembers;

    struct PortInfo {
        not_null<const DeclaratorSyntax*> syntax;
        span<const AttributeInstanceSyntax* const> attrs;
        const Symbol* internalSymbol = nullptr;
        const Definition* ifaceDef = nullptr;
        string_view modport;
        ArgumentDirection direction = ArgumentDirection::In;
        bool used = false;
        bool isIface = false;

        PortInfo(const DeclaratorSyntax& syntax, span<const AttributeInstanceSyntax* const> attrs) :
            syntax(&syntax), attrs(attrs) {}
    };
    SmallMap<string_view, PortInfo, 8> portInfos;

    const PortInfo* getInfo(string_view name) {
        auto it = portInfos.find(name);
        if (it == portInfos.end())
            return nullptr;

        it->second.used = true;
        return &it->second;
    }

    void handleIODecl(const PortHeaderSyntax& header, PortInfo& info,
                      const Symbol* insertionPoint) {
        auto& decl = *info.syntax;
        auto name = decl.name.valueText();
        auto declLoc = decl.name.location();

        ASSERT(!name.empty());

        switch (header.kind) {
            case SyntaxKind::VariablePortHeader: {
                auto& varHeader = header.as<VariablePortHeaderSyntax>();
                info.direction = SemanticFacts::getDirection(varHeader.direction.kind);

                if (varHeader.constKeyword)
                    scope.addDiag(diag::ConstPortNotAllowed, varHeader.constKeyword.range());

                // If the port has any kind of type declared, this constitutes a full symbol
                // definition. Otherwise we need to see if there's an existing symbol to match with.
                if (varHeader.varKeyword || varHeader.dataType->kind != SyntaxKind::ImplicitType) {
                    if (!varHeader.varKeyword) {
                        auto typeName = SyntaxFacts::getSimpleTypeName(*varHeader.dataType);
                        auto result = Lookup::unqualified(scope, typeName, LookupFlags::Type);
                        if (result && result->kind == SymbolKind::NetType) {
                            auto net =
                                comp.emplace<NetSymbol>(name, declLoc, result->as<NetType>());
                            setInternalSymbol(*net, decl, nullptr, info, insertionPoint);
                            break;
                        }
                    }

                    auto variable =
                        comp.emplace<VariableSymbol>(name, declLoc, VariableLifetime::Static);
                    setInternalSymbol(*variable, decl, varHeader.dataType, info, insertionPoint);
                }
                else if (auto symbol = scope.find(name);
                         symbol && (symbol->kind == SymbolKind::Variable ||
                                    symbol->kind == SymbolKind::Net)) {
                    // Port kind and type come from the matching symbol. Unfortunately
                    // that means we need to merge our own type info with whatever is
                    // declared for that symbol, so we need this ugly const_cast here.
                    info.internalSymbol = symbol;
                    ValueSymbol& val = const_cast<ValueSymbol&>(symbol->as<ValueSymbol>());

                    // If the I/O declaration is located prior to the symbol, we should update
                    // its index so that lookups in between will resolve correctly.
                    uint32_t ioIndex =
                        insertionPoint ? uint32_t(insertionPoint->getIndex()) + 1 : 1;
                    if (uint32_t(symbol->getIndex()) > ioIndex) {
                        val.getDeclaredType()->setOverrideIndex(symbol->getIndex());
                        val.setIndex(SymbolIndex(ioIndex));
                    }

                    val.getDeclaredType()->mergeImplicitPort(
                        varHeader.dataType->as<ImplicitTypeSyntax>(), declLoc, decl.dimensions);
                }
                else {
                    // No symbol and no data type defaults to a basic net.
                    auto net =
                        comp.emplace<NetSymbol>(name, declLoc, getDefaultNetType(scope, declLoc));
                    setInternalSymbol(*net, decl, varHeader.dataType, info, insertionPoint);
                }

                if (info.direction == ArgumentDirection::InOut &&
                    info.internalSymbol->kind != SymbolKind::Net) {
                    scope.addDiag(diag::InOutPortCannotBeVariable, declLoc) << name;
                }
                break;
            }
            case SyntaxKind::NetPortHeader: {
                auto& netHeader = header.as<NetPortHeaderSyntax>();
                info.direction = SemanticFacts::getDirection(netHeader.direction.kind);

                // Create a new symbol to represent this port internally to the instance.
                auto net =
                    comp.emplace<NetSymbol>(name, declLoc, comp.getNetType(netHeader.netType.kind));
                setInternalSymbol(*net, decl, netHeader.dataType, info, insertionPoint);
                break;
            }
            case SyntaxKind::InterfacePortHeader: {
                auto& ifaceHeader = header.as<InterfacePortHeaderSyntax>();
                auto [definition, modport] = getInterfacePortInfo(scope, ifaceHeader);
                ASSERT(ifaceHeader.nameOrKeyword.kind == TokenKind::Identifier);
                info.isIface = true;
                info.ifaceDef = definition;
                info.modport = modport;
                break;
            }
            default:
                THROW_UNREACHABLE;
        }

        const bool isNet = info.internalSymbol && info.internalSymbol->kind == SymbolKind::Net;
        if (info.direction == ArgumentDirection::Ref && isNet)
            scope.addDiag(diag::RefPortMustBeVariable, declLoc) << name;

        if ((info.direction != ArgumentDirection::Out || isNet) && decl.initializer)
            scope.addDiag(diag::DisallowedPortDefault, decl.initializer->sourceRange());
    }

    void setInternalSymbol(ValueSymbol& symbol, const DeclaratorSyntax& decl,
                           const DataTypeSyntax* dataType, PortInfo& info,
                           const Symbol* insertionPoint) {
        symbol.setSyntax(decl);
        symbol.setAttributes(scope, info.attrs);
        implicitMembers.emplace(&symbol, insertionPoint);
        info.internalSymbol = &symbol;

        if (dataType)
            symbol.setDeclaredType(*dataType, decl.dimensions);
        else if (!decl.dimensions.empty())
            symbol.getDeclaredType()->setDimensionSyntax(decl.dimensions);

        if (insertionPoint)
            symbol.getDeclaredType()->setOverrideIndex(insertionPoint->getIndex());
    }

    Symbol& createPort(string_view externalName, SourceLocation externalLoc,
                       const PortReferenceSyntax& syntax) {
        auto name = syntax.name.valueText();
        if (externalName.empty())
            externalName = name;

        auto info = getInfo(name);
        if (!info) {
            // Treat all unknown ports as an interface port. If that
            // turns out not to be true later we will issue an error then.
            auto port = comp.emplace<InterfacePortSymbol>(externalName, externalLoc);
            port->isMissingIO = true;
            return *port;
        }

        auto loc = info->syntax->name.location();
        if (info->isIface) {
            auto port = comp.emplace<InterfacePortSymbol>(externalName, loc);
            port->setSyntax(*info->syntax);
            port->setAttributes(scope, info->attrs);
            port->interfaceDef = info->ifaceDef;
            port->modport = info->modport;
            return *port;
        }

        // TODO: explicit connection expression

        auto port = comp.emplace<PortSymbol>(externalName, loc);
        port->setSyntax(syntax);
        port->externalLoc = externalLoc;

        ASSERT(info->internalSymbol);
        port->direction = info->direction;
        port->internalSymbol = info->internalSymbol;
        port->getDeclaredType()->copyTypeFrom(*info->internalSymbol->getDeclaredType());
        port->setAttributes(scope, info->attrs);

        if (auto init = info->syntax->initializer)
            port->setInitializerSyntax(*init->expr, init->equals.location());

        return *port;
    }

    Symbol& createPort(string_view name, SourceLocation externalLoc,
                       const PortConcatenationSyntax& syntax) {
        ArgumentDirection dir = ArgumentDirection::In;
        SmallVectorSized<const PortSymbol*, 4> buffer;
        bool allNets = true;
        bool allVars = true;
        bool hadError = false;

        auto reportDirError = [&](DiagCode code) {
            if (!hadError) {
                scope.addDiag(code, syntax.sourceRange());
                hadError = true;
            }
        };

        for (auto item : syntax.references) {
            auto& port = createPort(""sv, item->getFirstToken().location(), *item);
            if (port.kind == SymbolKind::Port) {
                auto& ps = port.as<PortSymbol>();
                buffer.append(&ps);
                ps.setParent(scope);

                // We need to merge the port direction with all of the other component port
                // directions to come up with our "effective" direction, which is what we use
                // to bind connection expressions. The rules here are not spelled out in the
                // LRM, but here's what I think makes sense based on other language rules:
                // - If all the directions are the same, that's the effective direction.
                // - inputs and outputs can be freely mixed; output direction dominates.
                // - if any port is ref, all ports must be variables. Effective direction is ref.
                // - if any port is inout, all ports must be nets. Effective direction is inout.
                // - ref and inout can never mix (implied by above two points).
                if (ps.direction == ArgumentDirection::InOut) {
                    dir = ArgumentDirection::InOut;
                    if (!allNets)
                        reportDirError(diag::PortConcatInOut);
                }
                else if (ps.direction == ArgumentDirection::Ref) {
                    dir = ArgumentDirection::Ref;
                    if (!allVars)
                        reportDirError(diag::PortConcatRef);
                }
                else if (ps.direction == ArgumentDirection::Out && dir == ArgumentDirection::In) {
                    dir = ArgumentDirection::Out;
                }

                auto sym = ps.internalSymbol;
                ASSERT(sym);
                if (sym->kind == SymbolKind::Net) {
                    allVars = false;
                    if (dir == ArgumentDirection::Ref)
                        reportDirError(diag::PortConcatRef);
                }
                else {
                    allNets = false;
                    if (dir == ArgumentDirection::InOut)
                        reportDirError(diag::PortConcatInOut);
                }
            }
            else {
                auto& ip = port.as<InterfacePortSymbol>();
                if (ip.isMissingIO) {
                    // This port gets added to the implicit members list because we
                    // need it to be findable via lookup, so that later declarations
                    // can properly issue an error if this is a real interface port.
                    ip.multiPortLoc = item->getFirstToken().location();
                    implicitMembers.emplace(&port, nullptr);
                }
                else {
                    auto& diag = scope.addDiag(diag::IfacePortInConcat, item->sourceRange());
                    diag << ip.name;
                }
            }
        }

        auto result = comp.emplace<MultiPortSymbol>(name, externalLoc, buffer.copy(comp), dir);
        result->setSyntax(syntax);
        return *result;
    }
};

class PortConnectionBuilder {
public:
    PortConnectionBuilder(const InstanceSymbol& instance,
                          const SeparatedSyntaxList<PortConnectionSyntax>& portConnections) :
        scope(*instance.getParentScope()),
        instance(instance), comp(scope.getCompilation()),
        lookupLocation(LookupLocation::after(instance)) {

        bool hasConnections = false;
        for (auto conn : portConnections) {
            bool isOrdered = conn->kind == SyntaxKind::OrderedPortConnection ||
                             conn->kind == SyntaxKind::EmptyPortConnection;
            if (!hasConnections) {
                hasConnections = true;
                usingOrdered = isOrdered;
            }
            else if (isOrdered != usingOrdered) {
                scope.addDiag(diag::MixingOrderedAndNamedPorts, conn->getFirstToken().location());
                break;
            }

            if (isOrdered) {
                orderedConns.append(conn);
            }
            else if (conn->kind == SyntaxKind::WildcardPortConnection) {
                if (!std::exchange(hasWildcard, true)) {
                    wildcardRange = conn->sourceRange();
                    wildcardAttrs =
                        AttributeSymbol::fromSyntax(conn->attributes, scope, lookupLocation);
                }
                else {
                    auto& diag =
                        scope.addDiag(diag::DuplicateWildcardPortConnection, conn->sourceRange());
                    diag.addNote(diag::NotePreviousUsage, wildcardRange.start());
                }
            }
            else {
                auto& npc = conn->as<NamedPortConnectionSyntax>();
                auto name = npc.name.valueText();
                if (!name.empty()) {
                    auto pair = namedConns.emplace(name, std::make_pair(&npc, false));
                    if (!pair.second) {
                        auto& diag =
                            scope.addDiag(diag::DuplicatePortConnection, npc.name.location());
                        diag << name;
                        diag.addNote(diag::NotePreviousUsage,
                                     pair.first->second.first->name.location());
                    }
                }
            }
        }

        // Build up the set of dimensions for the instantiating instance's array parent, if any.
        // This builds up the dimensions in reverse order, so we have to reverse them back.
        auto parent = &scope;
        while (parent && parent->asSymbol().kind == SymbolKind::InstanceArray) {
            auto& sym = parent->asSymbol().as<InstanceArraySymbol>();
            instanceDims.append(sym.range);
            parent = sym.getParentScope();
        }
        std::reverse(instanceDims.begin(), instanceDims.end());
    }

    template<typename TPort>
    PortConnection* getConnection(const TPort& port) {
        const bool hasDefault = port.getInitializer() != nullptr;
        if (usingOrdered) {
            if (orderedIndex >= orderedConns.size()) {
                orderedIndex++;

                if (hasDefault)
                    return createConnection(port, port.getInitializer(), {});

                if (port.name.empty()) {
                    if (!warnedAboutUnnamed) {
                        auto& diag = scope.addDiag(diag::UnconnectedUnnamedPort, instance.location);
                        diag.addNote(diag::NoteDeclarationHere, port.location);
                        warnedAboutUnnamed = true;
                    }
                }
                else {
                    scope.addDiag(diag::UnconnectedNamedPort, instance.location) << port.name;
                }

                return emptyConnection(port);
            }

            const PortConnectionSyntax& pc = *orderedConns[orderedIndex++];
            auto attrs = AttributeSymbol::fromSyntax(pc.attributes, scope, lookupLocation);
            if (pc.kind == SyntaxKind::OrderedPortConnection)
                return createConnection(port, *pc.as<OrderedPortConnectionSyntax>().expr, attrs);
            else
                return createConnection(port, port.getInitializer(), attrs);
        }

        if (port.name.empty()) {
            // port is unnamed so can never be connected by name
            if (!warnedAboutUnnamed) {
                auto& diag = scope.addDiag(diag::UnconnectedUnnamedPort, instance.location);
                diag.addNote(diag::NoteDeclarationHere, port.location);
                warnedAboutUnnamed = true;
            }
            return emptyConnection(port);
        }

        auto it = namedConns.find(port.name);
        if (it == namedConns.end()) {
            if (hasWildcard)
                return implicitNamedPort(port, wildcardAttrs, wildcardRange, true);

            if (hasDefault)
                return createConnection(port, port.getInitializer(), {});

            scope.addDiag(diag::UnconnectedNamedPort, instance.location) << port.name;
            return emptyConnection(port);
        }

        // We have a named connection; there are two possibilities here:
        // - An explicit connection (with an optional expression)
        // - An implicit connection, where we have to look up the name ourselves
        const NamedPortConnectionSyntax& conn = *it->second.first;
        it->second.second = true;

        auto attrs = AttributeSymbol::fromSyntax(conn.attributes, scope, lookupLocation);
        if (conn.openParen) {
            // For explicit named port connections, having an empty expression means no connection,
            // so we never take the default value here.
            if (conn.expr)
                return createConnection(port, *conn.expr, attrs);

            return emptyConnection(port);
        }

        return implicitNamedPort(port, attrs, conn.name.range(), false);
    }

    PortConnection* getIfaceConnection(const InterfacePortSymbol& port) {
        // TODO: verify that interface ports must always have a name
        ASSERT(!port.name.empty());

        // If the port definition is empty it means an error already
        // occurred; there's no way to check this connection so early out.
        if (!port.interfaceDef) {
            if (usingOrdered)
                orderedIndex++;
            else {
                auto it = namedConns.find(port.name);
                if (it != namedConns.end())
                    it->second.second = true;
            }
            return emptyConnection(port);
        }

        auto reportUnconnected = [&]() {
            auto& diag = scope.addDiag(diag::InterfacePortNotConnected, instance.location);
            diag << port.name;
            diag.addNote(diag::NoteDeclarationHere, port.location);
            return emptyConnection(port);
        };

        if (usingOrdered) {
            const PropertyExprSyntax* expr = nullptr;
            span<const AttributeSymbol* const> attributes;

            if (orderedIndex < orderedConns.size()) {
                const PortConnectionSyntax& pc = *orderedConns[orderedIndex];
                attributes = AttributeSymbol::fromSyntax(pc.attributes, scope, lookupLocation);
                if (pc.kind == SyntaxKind::OrderedPortConnection)
                    expr = pc.as<OrderedPortConnectionSyntax>().expr;
            }

            orderedIndex++;
            if (!expr)
                return reportUnconnected();

            return getInterfaceExpr(port, *expr, attributes);
        }

        auto it = namedConns.find(port.name);
        if (it == namedConns.end()) {
            if (hasWildcard)
                return getImplicitInterface(port, wildcardRange, wildcardAttrs);

            return reportUnconnected();
        }

        // We have a named connection; there are two possibilities here:
        // - An explicit connection (with an optional expression)
        // - An implicit connection, where we have to look up the name ourselves
        const NamedPortConnectionSyntax& conn = *it->second.first;
        it->second.second = true;

        auto attributes = AttributeSymbol::fromSyntax(conn.attributes, scope, lookupLocation);
        if (conn.openParen) {
            // For explicit named port connections, having an empty expression means no connection.
            if (!conn.expr)
                return reportUnconnected();

            return getInterfaceExpr(port, *conn.expr, attributes);
        }

        return getImplicitInterface(port, conn.name.range(), attributes);
    }

    void finalize() {
        if (usingOrdered) {
            if (orderedIndex < orderedConns.size()) {
                auto loc = orderedConns[orderedIndex]->getFirstToken().location();
                auto& diag = scope.addDiag(diag::TooManyPortConnections, loc);
                diag << instance.body.getDefinition().name;
                diag << orderedConns.size();
                diag << orderedIndex;
            }
        }
        else {
            for (auto& pair : namedConns) {
                // We marked all the connections that we used, so anything left over is a connection
                // for a non-existent port.
                if (!pair.second.second) {
                    auto& diag =
                        scope.addDiag(diag::PortDoesNotExist, pair.second.first->name.location());
                    diag << pair.second.first->name.valueText();
                    diag << instance.body.getDefinition().name;
                }
            }
        }
    }

private:
    PortConnection* emptyConnection(const PortSymbol& port) {
        return comp.emplace<PortConnection>(port, nullptr, span<const AttributeSymbol* const>{});
    }

    PortConnection* emptyConnection(const MultiPortSymbol& port) {
        return comp.emplace<PortConnection>(port, nullptr, span<const AttributeSymbol* const>{});
    }

    PortConnection* emptyConnection(const InterfacePortSymbol& port) {
        return comp.emplace<PortConnection>(port, nullptr, span<const AttributeSymbol* const>{});
    }

    PortConnection* createConnection(const PortSymbol& port, const Expression* expr,
                                     span<const AttributeSymbol* const> attributes) {
        return comp.emplace<PortConnection>(port, expr, attributes);
    }

    PortConnection* createConnection(const MultiPortSymbol& port, const Expression* expr,
                                     span<const AttributeSymbol* const> attributes) {
        return comp.emplace<PortConnection>(port, expr, attributes);
    }

    template<typename TPort>
    PortConnection* createConnection(const TPort& port, const PropertyExprSyntax& syntax,
                                     span<const AttributeSymbol* const> attributes) {
        // If this is an empty port, it's an error to provide an expression.
        if (port.getType().isVoid()) {
            auto& diag = scope.addDiag(diag::NullPortExpression, syntax.sourceRange());
            diag.addNote(diag::NoteDeclarationHere, port.location);
            return emptyConnection(port);
        }

        // TODO: if port is explicit, check that expression as well
        BindContext context(scope, lookupLocation, BindFlags::NonProcedural);
        context.instance = &instance;

        auto exprSyntax = context.requireSimpleExpr(syntax);
        if (!exprSyntax)
            return emptyConnection(port);

        auto& expr = Expression::bindArgument(port.getType(), port.direction, *exprSyntax, context);
        return createConnection(port, &expr, attributes);
    }

    PortConnection* createConnection(const InterfacePortSymbol& port, const Symbol* ifaceInst,
                                     span<const AttributeSymbol* const> attributes) {
        return comp.emplace<PortConnection>(port, ifaceInst, attributes);
    }

    template<typename TPort>
    PortConnection* implicitNamedPort(const TPort& port,
                                      span<const AttributeSymbol* const> attributes,
                                      SourceRange range, bool isWildcard) {
        // An implicit named port connection is semantically equivalent to `.port(port)` except:
        // - Can't create implicit net declarations this way
        // - Port types need to be equivalent, not just assignment compatible
        // - An implicit connection between nets of two dissimilar net types shall issue an
        //   error when it is a warning in an explicit named port connection

        LookupFlags flags = isWildcard ? LookupFlags::DisallowWildcardImport : LookupFlags::None;
        auto symbol = Lookup::unqualified(scope, port.name, flags);
        if (!symbol) {
            // If this is a wildcard connection, we're allowed to use the port's default value,
            // if it has one.
            if (isWildcard && port.getInitializer())
                return createConnection(port, port.getInitializer(), attributes);

            scope.addDiag(diag::ImplicitNamedPortNotFound, range) << port.name;
            return emptyConnection(port);
        }

        if (!symbol->isDeclaredBefore(lookupLocation).value_or(true)) {
            auto& diag = scope.addDiag(diag::UsedBeforeDeclared, range);
            diag << port.name;
            diag.addNote(diag::NoteDeclarationHere, symbol->location);
        }

        auto& portType = port.getType();
        if (portType.isError())
            return emptyConnection(port);

        BindContext context(scope, LookupLocation::max, BindFlags::NonProcedural);
        auto expr = &ValueExpressionBase::fromSymbol(context, *symbol, false, range);
        if (expr->bad())
            return emptyConnection(port);

        if (!expr->type->isEquivalent(portType)) {
            auto& diag = scope.addDiag(diag::ImplicitNamedPortTypeMismatch, range);
            diag << port.name;
            diag << portType;
            diag << *expr->type;
            return emptyConnection(port);
        }

        // TODO: direction of assignment
        auto& assign = Expression::convertAssignment(context, portType, *expr, range.start());
        return createConnection(port, &assign, attributes);
    }

    PortConnection* getInterfaceExpr(const InterfacePortSymbol& port,
                                     const PropertyExprSyntax& syntax,
                                     span<const AttributeSymbol* const> attributes) {
        BindContext context(scope, lookupLocation, BindFlags::NonProcedural);
        auto expr = context.requireSimpleExpr(syntax);
        if (!expr)
            return emptyConnection(port);

        while (expr->kind == SyntaxKind::ParenthesizedExpression)
            expr = expr->as<ParenthesizedExpressionSyntax>().expression;

        if (!NameSyntax::isKind(expr->kind)) {
            scope.addDiag(diag::InterfacePortInvalidExpression, expr->sourceRange()) << port.name;
            return emptyConnection(port);
        }

        LookupResult result;
        Lookup::name(expr->as<NameSyntax>(), context, LookupFlags::None, result);
        result.reportDiags(context);

        // If we found the interface but it's actually a port, unwrap to the target connection.
        const Symbol* symbol = result.found;
        string_view modport;
        if (symbol && symbol->kind == SymbolKind::InterfacePort) {
            auto& ifacePort = symbol->as<InterfacePortSymbol>();
            modport = ifacePort.modport;

            symbol = ifacePort.getConnection();
            if (symbol && !result.selectors.empty()) {
                SmallVectorSized<const ElementSelectSyntax*, 4> selectors;
                for (auto& sel : result.selectors)
                    selectors.append(std::get<0>(sel));

                symbol = Lookup::selectChild(*symbol, selectors, context, result);
            }
        }
        else {
            result.errorIfSelectors(context);
        }

        const Symbol* conn = nullptr;
        if (symbol)
            conn = getInterface(port, symbol, modport, expr->sourceRange());

        return createConnection(port, conn, attributes);
    }

    PortConnection* getImplicitInterface(const InterfacePortSymbol& port, SourceRange range,
                                         span<const AttributeSymbol* const> attributes) {
        auto symbol = Lookup::unqualified(scope, port.name);
        if (!symbol) {
            scope.addDiag(diag::ImplicitNamedPortNotFound, range) << port.name;
            return emptyConnection(port);
        }

        if (!symbol->isDeclaredBefore(lookupLocation).value_or(true)) {
            auto& diag = scope.addDiag(diag::UsedBeforeDeclared, range);
            diag << port.name;
            diag.addNote(diag::NoteDeclarationHere, symbol->location);
        }

        auto conn = getInterface(port, symbol, {}, range);
        return createConnection(port, conn, attributes);
    }

    static bool areDimSizesEqual(span<const ConstantRange> left, span<const ConstantRange> right) {
        if (left.size() != right.size())
            return false;

        for (size_t i = 0; i < left.size(); i++) {
            if (left[i].width() != right[i].width())
                return false;
        }

        return true;
    }

    const Symbol* getInterface(const InterfacePortSymbol& port, const Symbol* symbol,
                               string_view providedModport, SourceRange range) {
        if (!port.interfaceDef)
            return nullptr;

        auto portDims = port.getDeclaredRange();
        if (!portDims)
            return nullptr;

        // The user can explicitly connect a modport symbol.
        if (symbol->kind == SymbolKind::Modport) {
            // Interface that owns the modport must match our expected interface.
            auto connDef = symbol->getDeclaringDefinition();
            ASSERT(connDef);
            if (connDef != port.interfaceDef) {
                // TODO: print the potentially nested name path instead of the simple name
                auto& diag = scope.addDiag(diag::InterfacePortTypeMismatch, range);
                diag << connDef->name << port.interfaceDef->name;
                diag.addNote(diag::NoteDeclarationHere, port.location);
                return nullptr;
            }

            // Modport must match the specified requirement, if we have one.
            ASSERT(providedModport.empty());
            if (!port.modport.empty() && symbol->name != port.modport) {
                auto& diag = scope.addDiag(diag::ModportConnMismatch, range);
                diag << connDef->name << symbol->name;
                diag << port.interfaceDef->name << port.modport;
                return nullptr;
            }

            // Make sure the port doesn't require an array.
            if (!portDims->empty()) {
                auto& diag = scope.addDiag(diag::PortConnDimensionsMismatch, range) << port.name;
                diag.addNote(diag::NoteDeclarationHere, port.location);
                return nullptr;
            }

            // Everything checks out. Connect to the modport.
            return symbol;
        }

        // If the symbol is another port, unwrap it now.
        if (symbol->kind == SymbolKind::InterfacePort) {
            // Should be impossible to already have a modport specified here.
            ASSERT(providedModport.empty());

            auto& ifacePort = symbol->as<InterfacePortSymbol>();
            providedModport = ifacePort.modport;
            symbol = ifacePort.getConnection();
            if (!symbol)
                return nullptr;
        }

        // Make sure the thing we're connecting to is an interface or array of interfaces.
        SmallVectorSized<ConstantRange, 4> dims;
        const Symbol* child = symbol;
        while (child->kind == SymbolKind::InstanceArray) {
            auto& array = child->as<InstanceArraySymbol>();
            if (array.elements.empty())
                return nullptr;

            dims.append(array.range);
            child = array.elements[0];
        }

        if (child->kind != SymbolKind::Instance || !child->as<InstanceSymbol>().isInterface()) {
            // If this is a variable with an errored type, an error is already emitted.
            if (child->kind != SymbolKind::Variable ||
                !child->as<VariableSymbol>().getType().isError()) {
                auto& diag = scope.addDiag(diag::NotAnInterface, range) << symbol->name;
                diag.addNote(diag::NoteDeclarationHere, symbol->location);
            }
            return nullptr;
        }

        auto connDef = &child->as<InstanceSymbol>().getDefinition();
        if (connDef != port.interfaceDef) {
            // TODO: print the potentially nested name path instead of the simple name
            auto& diag = scope.addDiag(diag::InterfacePortTypeMismatch, range);
            diag << connDef->name << port.interfaceDef->name;
            diag.addNote(diag::NoteDeclarationHere, port.location);
            return nullptr;
        }

        // If a modport was provided and our port requires a modport, make sure they match.
        if (!providedModport.empty() && !port.modport.empty() && providedModport != port.modport) {
            auto& diag = scope.addDiag(diag::ModportConnMismatch, range);
            diag << connDef->name << providedModport;
            diag << port.interfaceDef->name << port.modport;
            return nullptr;
        }

        // If the dimensions match exactly what the port is expecting make the connection.
        if (areDimSizesEqual(*portDims, dims))
            return symbol;

        // Otherwise, if the instance being instantiated is part of an array of instances *and*
        // the symbol we're connecting to is an array of interfaces, we need to check to see whether
        // to slice up that array among all the instances. We do the slicing operation if:
        // instance array dimensions + port dimensions == connection dimensions
        span<const ConstantRange> dimSpan = dims;
        if (dimSpan.size() >= instanceDims.size() &&
            areDimSizesEqual(dimSpan.subspan(0, instanceDims.size()), instanceDims) &&
            areDimSizesEqual(dimSpan.subspan(instanceDims.size()), *portDims)) {

            // It's ok to do the slicing, so pick the correct slice for the connection
            // based on the actual path of the instance we're elaborating.
            for (size_t i = 0; i < instance.arrayPath.size(); i++) {
                // First translate the path index since it's relative to that particular
                // array's declared range.
                int32_t index = instanceDims[i].translateIndex(instance.arrayPath[i]);

                // Now translate back to be relative to the connecting interface's declared range.
                // Note that we want this to be zero based because we're going to index into
                // the actual span of elements, so we only need to flip the index if the range
                // is not little endian.
                auto& array = symbol->as<InstanceArraySymbol>();
                if (!array.range.isLittleEndian())
                    index = array.range.upper() - index - array.range.lower();

                symbol = array.elements[size_t(index)];
            }

            return symbol;
        }

        auto& diag = scope.addDiag(diag::PortConnDimensionsMismatch, range) << port.name;
        diag.addNote(diag::NoteDeclarationHere, port.location);
        return nullptr;
    }

    const Scope& scope;
    const InstanceSymbol& instance;
    Compilation& comp;
    SmallVectorSized<ConstantRange, 4> instanceDims;
    SmallVectorSized<const PortConnectionSyntax*, 8> orderedConns;
    SmallMap<string_view, std::pair<const NamedPortConnectionSyntax*, bool>, 8> namedConns;
    span<const AttributeSymbol* const> wildcardAttrs;
    LookupLocation lookupLocation;
    SourceRange wildcardRange;
    size_t orderedIndex = 0;
    bool usingOrdered = true;
    bool hasWildcard = false;
    bool warnedAboutUnnamed = false;
};

} // end anonymous namespace

PortSymbol::PortSymbol(string_view name, SourceLocation loc, bitmask<DeclaredTypeFlags> flags) :
    ValueSymbol(SymbolKind::Port, name, loc, flags | DeclaredTypeFlags::Port) {
    externalLoc = loc;
}

void PortSymbol::fromSyntax(
    const PortListSyntax& syntax, const Scope& scope, SmallVector<const Symbol*>& results,
    SmallVector<std::pair<Symbol*, const Symbol*>>& implicitMembers,
    span<std::pair<const PortDeclarationSyntax*, const Symbol*> const> portDeclarations) {

    switch (syntax.kind) {
        case SyntaxKind::AnsiPortList: {
            AnsiPortListBuilder builder{ scope, implicitMembers };
            for (auto port : syntax.as<AnsiPortListSyntax>().ports) {
                switch (port->kind) {
                    case SyntaxKind::ImplicitAnsiPort:
                        results.append(builder.createPort(port->as<ImplicitAnsiPortSyntax>()));
                        break;
                    case SyntaxKind::ExplicitAnsiPort:
                        results.append(builder.createPort(port->as<ExplicitAnsiPortSyntax>()));
                        break;
                    default:
                        THROW_UNREACHABLE;
                }
            }

            if (!portDeclarations.empty()) {
                scope.addDiag(diag::PortDeclInANSIModule,
                              portDeclarations[0].first->getFirstToken().location());
            }
            break;
        }
        case SyntaxKind::NonAnsiPortList: {
            NonAnsiPortListBuilder builder{ scope, portDeclarations, implicitMembers };
            for (auto port : syntax.as<NonAnsiPortListSyntax>().ports) {
                switch (port->kind) {
                    case SyntaxKind::ImplicitNonAnsiPort:
                        results.append(builder.createPort(port->as<ImplicitNonAnsiPortSyntax>()));
                        break;
                    case SyntaxKind::ExplicitNonAnsiPort:
                        results.append(builder.createPort(port->as<ExplicitNonAnsiPortSyntax>()));
                        break;
                    case SyntaxKind::EmptyNonAnsiPort:
                        results.append(builder.createPort(port->as<EmptyNonAnsiPortSyntax>()));
                        break;
                    default:
                        THROW_UNREACHABLE;
                }
            }
            builder.finalize();
            break;
        }
        case SyntaxKind::WildcardPortList:
            scope.addDiag(diag::NotYetSupported, syntax.sourceRange());
            break;
        default:
            THROW_UNREACHABLE;
    }
}

void PortSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.write("direction", toString(direction));
    if (internalSymbol)
        serializer.writeLink("internalSymbol", *internalSymbol);
}

MultiPortSymbol::MultiPortSymbol(string_view name, SourceLocation loc,
                                 span<const PortSymbol* const> ports, ArgumentDirection direction) :
    Symbol(SymbolKind::MultiPort, name, loc),
    ports(ports), direction(direction) {
}

const Type& MultiPortSymbol::getType() const {
    if (type)
        return *type;

    auto scope = getParentScope();
    auto syntax = getSyntax();
    ASSERT(scope && syntax);

    auto& comp = scope->getCompilation();

    BindContext context(*scope, LookupLocation::before(*this));
    bitwidth_t totalWidth = 0;
    bitmask<IntegralFlags> flags;

    for (auto port : ports) {
        auto& t = port->getType();
        if (t.isError()) {
            type = &comp.getErrorType();
            return *type;
        }

        if (!t.isIntegral()) {
            context.addDiag(diag::BadConcatExpression, port->externalLoc) << t;
            type = &comp.getErrorType();
            return *type;
        }

        totalWidth += t.getBitWidth();

        if (!context.requireValidBitWidth(totalWidth, syntax->sourceRange())) {
            type = &comp.getErrorType();
            return *type;
        }

        if (t.isFourState())
            flags |= IntegralFlags::FourState;
    }

    if (totalWidth == 0) {
        type = &comp.getErrorType();
        return *type;
    }

    type = &comp.getType(totalWidth, flags);
    return *type;
}

void MultiPortSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.startArray("ports");
    for (auto port : ports) {
        serializer.startObject();
        port->serializeTo(serializer);
        serializer.endObject();
    }
    serializer.endArray();
}

optional<span<const ConstantRange>> InterfacePortSymbol::getDeclaredRange() const {
    if (range)
        return *range;

    if (!interfaceDef) {
        range.emplace();
        return *range;
    }

    auto syntax = getSyntax();
    ASSERT(syntax);

    auto scope = getParentScope();
    ASSERT(scope);

    BindContext context(*scope, LookupLocation::before(*this));

    SmallVectorSized<ConstantRange, 4> buffer;
    for (auto dimSyntax : syntax->as<DeclaratorSyntax>().dimensions) {
        auto dim = context.evalDimension(*dimSyntax, /* requireRange */ true, /* isPacked */ false);
        if (!dim.isRange())
            return std::nullopt;

        buffer.append(dim.range);
    }

    range = buffer.copy(scope->getCompilation());
    return *range;
}

const Symbol* InterfacePortSymbol::getConnection() const {
    auto scope = getParentScope();
    ASSERT(scope);

    auto& body = scope->asSymbol().as<InstanceBodySymbol>();
    ASSERT(body.parentInstance);

    auto conn = body.parentInstance->getPortConnection(*this);
    if (!conn)
        return nullptr;

    return conn->ifaceInstance;
}

void InterfacePortSymbol::serializeTo(ASTSerializer& serializer) const {
    if (interfaceDef)
        serializer.write("interfaceDef", interfaceDef->name);
    if (!modport.empty())
        serializer.write("modport", modport);
}

PortConnection::PortConnection(const Symbol& port, const Expression* expr,
                               span<const AttributeSymbol* const> attributes) :
    port(&port),
    expr(expr), isInterfacePort(false), attributes(attributes) {
}

PortConnection::PortConnection(const InterfacePortSymbol& port, const Symbol* instance,
                               span<const AttributeSymbol* const> attributes) :
    ifacePort(&port),
    ifaceInstance(instance), isInterfacePort(true), attributes(attributes) {
}

void PortConnection::makeConnections(
    const InstanceSymbol& instance, span<const Symbol* const> ports,
    const SeparatedSyntaxList<PortConnectionSyntax>& portConnections, PointerMap& results) {

    PortConnectionBuilder builder(instance, portConnections);
    for (auto portBase : ports) {
        if (portBase->kind == SymbolKind::Port) {
            auto& port = portBase->as<PortSymbol>();
            results.emplace(reinterpret_cast<uintptr_t>(&port),
                            reinterpret_cast<uintptr_t>(builder.getConnection(port)));
        }
        else if (portBase->kind == SymbolKind::MultiPort) {
            auto& port = portBase->as<MultiPortSymbol>();
            results.emplace(reinterpret_cast<uintptr_t>(&port),
                            reinterpret_cast<uintptr_t>(builder.getConnection(port)));
        }
        else {
            auto& port = portBase->as<InterfacePortSymbol>();
            results.emplace(reinterpret_cast<uintptr_t>(&port),
                            reinterpret_cast<uintptr_t>(builder.getIfaceConnection(port)));
        }
    }

    builder.finalize();
}

void PortConnection::serializeTo(ASTSerializer& serializer) const {
    serializer.write("isInterfacePort", isInterfacePort);
    if (isInterfacePort) {
        if (ifacePort)
            serializer.writeLink("ifacePort", *ifacePort);
        if (ifaceInstance)
            serializer.writeLink("ifaceInstance", *ifaceInstance);
    }
    else {
        if (port)
            serializer.writeLink("port", *port);
        if (expr)
            serializer.write("expr", *expr);
    }

    if (!attributes.empty()) {
        serializer.startArray("attributes");
        for (auto attr : attributes)
            serializer.serialize(*attr);
        serializer.endArray();
    }
}

} // namespace slang
