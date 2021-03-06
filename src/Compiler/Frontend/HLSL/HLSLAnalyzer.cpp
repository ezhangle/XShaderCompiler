/*
 * HLSLAnalyzer.cpp
 * 
 * This file is part of the XShaderCompiler project (Copyright (c) 2014-2016 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#include "HLSLAnalyzer.h"
#include "HLSLIntrinsics.h"
#include "ConstExprEvaluator.h"
#include "EndOfScopeAnalyzer.h"
#include "Exception.h"
#include "Helper.h"


namespace Xsc
{


static ShaderVersion GetShaderModel(const InputShaderVersion v)
{
    switch (v)
    {
        case InputShaderVersion::HLSL3: return { 3, 0 };
        case InputShaderVersion::HLSL4: return { 4, 0 };
        case InputShaderVersion::HLSL5: return { 5, 0 };
    }
    return { 1, 0 };
}

HLSLAnalyzer::HLSLAnalyzer(Log* log) :
    Analyzer{ log }
{
}

void HLSLAnalyzer::DecorateASTPrimary(
    Program& program, const ShaderInput& inputDesc, const ShaderOutput& outputDesc)
{
    /* Store parameters */
    entryPoint_     = inputDesc.entryPoint;
    shaderTarget_   = inputDesc.shaderTarget;
    versionIn_      = inputDesc.shaderVersion;
    shaderModel_    = GetShaderModel(inputDesc.shaderVersion);
    preferWrappers_ = outputDesc.options.preferWrappers;
    statistics_     = outputDesc.statistics;

    /* Decorate program AST */
    program_ = &program;

    Visit(&program);
}


/*
 * ======= Private: =======
 */

Variant HLSLAnalyzer::EvaluateConstExpr(Expr& expr)
{
    try
    {
        /* Evaluate expression and throw error on var-access */
        ConstExprEvaluator exprEvaluator;
        return exprEvaluator.EvaluateExpr(expr, [](VarAccessExpr* ast) -> Variant { throw ast; });
    }
    catch (const std::exception&)
    {
        return Variant();
    }
    catch (const VarAccessExpr*)
    {
        return Variant();
    }
    return Variant();
}

float HLSLAnalyzer::EvaluateConstExprFloat(Expr& expr)
{
    return static_cast<float>(EvaluateConstExpr(expr).ToReal());
}

/* ------- Visit functions ------- */

#define IMPLEMENT_VISIT_PROC(AST_NAME) \
    void HLSLAnalyzer::Visit##AST_NAME(AST_NAME* ast, void* args)

IMPLEMENT_VISIT_PROC(Program)
{
    /* Analyze context of the entire program */
    Visit(ast->globalStmnts);
}

IMPLEMENT_VISIT_PROC(CodeBlock)
{
    OpenScope();
    {
        Visit(ast->stmnts);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(FunctionCall)
{
    PushFunctionCall(ast);
    {
        /* Analyze function arguments first */
        Visit(ast->arguments);

        /* Then analyze function name */
        if (ast->varIdent)
        {
            if (ast->varIdent->next)
            {
                /* Analyze variable identifier */
                AnalyzeVarIdent(ast->varIdent.get());

                /* Check if the function call refers to an intrinsic */
                auto intrIt = HLSLIntrinsics().find(ast->varIdent->next->ident);
                if (intrIt != HLSLIntrinsics().end())
                {
                    auto intrinsic = intrIt->second.intrinsic;

                    /* Verify intrinsic for respective object class */
                    switch (ast->varIdent->symbolRef->Type())
                    {
                        case AST::Types::TextureDecl:
                            if (!IsTextureIntrinsic(intrinsic))
                                Error("invalid intrinsic '" + ast->varIdent->next->ident + "' for a texture object", ast);
                            break;

                        default:
                            break;
                    }

                    AnalyzeFunctionCallIntrinsic(ast, intrIt->second);
                }
                else
                    AnalyzeFunctionCallStandard(ast);
            }
            else
            {
                /* Check if the function call refers to an intrinsic */
                auto intrIt = HLSLIntrinsics().find(ast->varIdent->ident);
                if (intrIt != HLSLIntrinsics().end())
                    AnalyzeFunctionCallIntrinsic(ast, intrIt->second);
                else
                    AnalyzeFunctionCallStandard(ast);
            }
        }
    }
    PopFunctionCall();
}

IMPLEMENT_VISIT_PROC(VarType)
{
    Visit(ast->structDecl);

    if (ast->typeDenoter)
    {
        AnalyzeTypeDenoter(ast->typeDenoter, ast);

        //TODO: remove this
        #if 1
        if (!ast->typeDenoter->Ident().empty())
        {
            /* Decorate variable type */
            auto symbol = Fetch(ast->typeDenoter->Ident());
            if (symbol)
                ast->symbolRef = symbol;
        }
        #endif
    }
    else
        Error("missing variable type", ast);
}

/* --- Declarations --- */

IMPLEMENT_VISIT_PROC(VarDecl)
{
    Register(ast->ident, ast);

    Visit(ast->arrayDims);

    AnalyzeSemantic(ast->semantic);

    /* Store references to members with system value semantic (SV_...) in all parent structures */
    if (ast->semantic.IsSystemValue())
    {
        for (auto structDecl : GetStructDeclStack())
            structDecl->systemValuesRef[ast->ident] = ast;
    }

    if (ast->initializer)
    {
        Visit(ast->initializer);

        /* Compare initializer type with var-decl type */
        ValidateTypeCastFrom(ast->initializer.get(), ast);
    }
}

IMPLEMENT_VISIT_PROC(TextureDecl)
{
    /* Register identifier for texture */
    Register(ast->ident, ast);
}

IMPLEMENT_VISIT_PROC(SamplerDecl)
{
    /* Register identifier for sampler */
    Register(ast->ident, ast);

    /* Collect output statistics for sampler states */
    if (statistics_)
    {
        SamplerState samplerState;
        {
            for (auto& value : ast->samplerValues)
                AnalyzeSamplerValue(value.get(), samplerState);
        }
        statistics_->samplerStates[ast->ident] = samplerState;
    }
}

IMPLEMENT_VISIT_PROC(StructDecl)
{
    /* Find base struct-decl */
    if (!ast->baseStructName.empty())
        ast->baseStructRef = FetchStructDeclFromIdent(ast->baseStructName);

    /* Register struct identifier in symbol table */
    Register(ast->ident, ast);

    PushStructDecl(ast);
    {
        if (ast->flags(StructDecl::isNestedStruct) && !ast->IsAnonymous())
            Error("nested structures must be anonymous", ast);

        OpenScope();
        {
            Visit(ast->members);
        }
        CloseScope();
    }
    PopStructDecl();

    /* Report warning if structure is empty */
    if (ast->NumMembers() == 0)
        Warning("'" + ast->SignatureToString() + "' is completely empty", ast);
}

IMPLEMENT_VISIT_PROC(AliasDecl)
{
    AnalyzeTypeDenoter(ast->typeDenoter, ast);

    /* Register type-alias identifier in symbol table */
    Register(ast->ident, ast);
}

/* --- Declaration statements --- */

IMPLEMENT_VISIT_PROC(FunctionDecl)
{
    GetReportHandler().PushContextDesc(ast->SignatureToString(false));

    const auto isEntryPoint = (ast->ident == entryPoint_);

    /* Analyze function return semantic */
    AnalyzeSemantic(ast->semantic);

    /* Register function declaration in symbol table */
    Register(ast->ident, ast);

    /* Visit attributes */
    Visit(ast->attribs);

    /* Visit function header */
    Visit(ast->returnType);

    OpenScope();
    {
        Visit(ast->parameters);

        /* Special case for the main entry point */
        if (isEntryPoint)
            AnalyzeEntryPoint(ast);

        /* Visit function body */
        PushFunctionDeclLevel(isEntryPoint);
        {
            Visit(ast->codeBlock);
        }
        PopFunctionDeclLevel();

        /* Analyze last statement of function body ('isEndOfFunction' flag) */
        AnalyzeEndOfScopes(*ast);
    }
    CloseScope();

    GetReportHandler().PopContextDesc();
}

IMPLEMENT_VISIT_PROC(BufferDeclStmnt)
{
    /* Validate buffer slots */
    if (ast->slotRegisters.size() > 1)
        Error("buffers can only be bound to one slot", ast->slotRegisters[1].get(), HLSLErr::ERR_BIND_INVALID);

    for (const auto& slotRegister : ast->slotRegisters)
    {
        if (slotRegister->shaderTarget != ShaderTarget::Undefined)
            Error("user-defined constant buffer slots can not be target specific", slotRegister.get(), HLSLErr::ERR_TARGET_INVALID);
    }

    for (auto& member : ast->members)
    {
        Visit(member);

        //TODO: move this to the HLSLParser!
        /* Decorate all members with a reference to this buffer declaration */
        for (auto& varDecl : member->varDecls)
            varDecl->bufferDeclRef = ast;
    }
}

IMPLEMENT_VISIT_PROC(StructDeclStmnt)
{
    Visit(ast->structDecl);
}

#if 0
IMPLEMENT_VISIT_PROC(VarDeclStmnt)
{
    Visit(ast->varType);
    Visit(ast->varDecls);

    /* Decorate variable type */
    if (InsideEntryPoint() && ast->varDecls.empty())
    {
        if (auto symbol = ast->varType->symbolRef)
        {
            if (auto structDecl = symbol->As<StructDecl>())
            {
                if (structDecl->flags(StructDecl::isShaderOutput) && structDecl->aliasName.empty())
                {
                    /* Store alias name for shader output interface block */
                    structDecl->aliasName = ast->varDecls.front()->ident;
                }
            }
        }
    }
}
#endif

/* --- Statements --- */

IMPLEMENT_VISIT_PROC(ForLoopStmnt)
{
    WarningOnNullStmnt(ast->bodyStmnt, "for loop");

    Visit(ast->attribs);

    OpenScope();
    {
        Visit(ast->initSmnt);
        Visit(ast->condition);
        Visit(ast->iteration);

        OpenScope();
        {
            Visit(ast->bodyStmnt);
        }
        CloseScope();
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(WhileLoopStmnt)
{
    WarningOnNullStmnt(ast->bodyStmnt, "while loop");

    Visit(ast->attribs);

    OpenScope();
    {
        Visit(ast->condition);
        Visit(ast->bodyStmnt);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(DoWhileLoopStmnt)
{
    WarningOnNullStmnt(ast->bodyStmnt, "do-while loop");

    Visit(ast->attribs);

    OpenScope();
    {
        Visit(ast->bodyStmnt);
        Visit(ast->condition);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(IfStmnt)
{
    WarningOnNullStmnt(ast->bodyStmnt, "if");

    Visit(ast->attribs);

    OpenScope();
    {
        Visit(ast->condition);
        Visit(ast->bodyStmnt);
    }
    CloseScope();

    Visit(ast->elseStmnt);
}

IMPLEMENT_VISIT_PROC(ElseStmnt)
{
    WarningOnNullStmnt(ast->bodyStmnt, "else");

    OpenScope();
    {
        Visit(ast->bodyStmnt);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(SwitchStmnt)
{
    Visit(ast->attribs);

    OpenScope();
    {
        Visit(ast->selector);
        Visit(ast->cases);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(ExprStmnt)
{
    Visit(ast->expr);

    /* Validate expression type by just calling the getter */
    GetTypeDenoterFrom(ast->expr.get());

    /* Analyze wrapper inlining for intrinsic calls */
    if (!preferWrappers_)
    {
        if (auto funcCallExpr = ast->expr->As<FunctionCallExpr>())
            AnalyzeIntrinsicWrapperInlining(funcCallExpr->call.get());
    }
}

IMPLEMENT_VISIT_PROC(ReturnStmnt)
{
    Visit(ast->expr);

    /* Validate expression type by just calling the getter */
    GetTypeDenoterFrom(ast->expr.get());

    //TODO: refactor this
    #if 1
    /* Analyze entry point return statement */
    if (InsideEntryPoint())
    {
        if (auto varAccessExpr = ast->expr->As<VarAccessExpr>())
        {
            if (varAccessExpr->varIdent->symbolRef)
            {
                if (auto varDecl = varAccessExpr->varIdent->symbolRef->As<VarDecl>())
                {
                    if (varDecl->declStmntRef && varDecl->declStmntRef->varType)
                    {
                        /*
                        Variable declaration statement has been found,
                        now find the structure object to add the alias name for the interface block.
                        */
                        auto varType = varDecl->declStmntRef->varType.get();
                        if (varType->symbolRef)
                        {
                            if (auto structDecl = varType->symbolRef->As<StructDecl>())
                            {
                                /* Store alias name for the interface block */
                                structDecl->aliasName = varAccessExpr->varIdent->ident;

                                /*
                                Don't generate code for this variable declaration,
                                because this variable is now already used as interface block.
                                */
                                varDecl->flags << VarDecl::disableCodeGen;
                            }
                        }
                    }
                }
            }
        }
    }
    #endif
}

/* --- Expressions --- */

IMPLEMENT_VISIT_PROC(TypeNameExpr)
{
    AnalyzeTypeDenoter(ast->typeDenoter, ast);
}

IMPLEMENT_VISIT_PROC(SuffixExpr)
{
    Visit(ast->expr);

    /* Left-hand-side of the suffix expression must be either from type structure or base (for vector subscript) */
    auto typeDenoter = ast->expr->GetTypeDenoter()->Get();

    if (auto structTypeDen = typeDenoter->As<StructTypeDenoter>())
    {
        /* Fetch struct member variable declaration from next identifier */
        if (auto memberVarDecl = FetchFromStructDecl(*structTypeDen, ast->varIdent->ident, ast->varIdent.get()))
        {
            /* Analyzer next identifier with fetched symbol */
            AnalyzeVarIdentWithSymbol(ast->varIdent.get(), memberVarDecl);
        }
    }
}

IMPLEMENT_VISIT_PROC(VarAccessExpr)
{
    AnalyzeVarIdent(ast->varIdent.get());

    if (ast->assignExpr)
    {
        Visit(ast->assignExpr);
        ValidateTypeCastFrom(ast->assignExpr.get(), ast->varIdent.get());
    }
}

#undef IMPLEMENT_VISIT_PROC

/* --- Helper functions for context analysis --- */

void HLSLAnalyzer::AnalyzeFunctionCallStandard(FunctionCall* ast)
{
    /* Decorate function identifier (if it's a member function) */
    if (ast->varIdent->next)
    {
        AnalyzeVarIdent(ast->varIdent.get());

        //TODO: refactor member functions!
        #if 0
        if (auto symbol = ast->varIdent->symbolRef)
        {
            if (symbol->Type() == AST::Types::TextureDecl)
                ast->flags << FunctionCall::isTexFunc;
        }
        #endif
    }
    else
    {
        /* Fetch function declaratino by arguments */
        ast->funcDeclRef = FetchFunctionDecl(ast->varIdent->ident, ast->arguments, ast);
    }
}

void HLSLAnalyzer::AnalyzeFunctionCallIntrinsic(FunctionCall* ast, const HLSLIntrinsicEntry& intr)
{
    /* Check shader input version */
    if (shaderModel_ < intr.minShaderModel)
    {
        Warning(
            "intrinsic '" + ast->varIdent->ToString() + "' requires shader model " + intr.minShaderModel.ToString() +
            ", but only " + shaderModel_.ToString() + " is specified", ast
        );
    }

    /* Decorate AST with intrinsic ID */
    ast->intrinsic = intr.intrinsic;

    /* Analyze special intrinsic types */
    using T = Intrinsic;

    struct IntrinsicConversion
    {
        T   standardIntrinsic;
        int numArgs;
        T   overloadedIntrinsic;
    };

    static const std::vector<IntrinsicConversion> intrinsicConversions
    {
        { T::AsUInt_1,              3, T::AsUInt_3              },
        { T::Tex1D_2,               4, T::Tex1D_4               },
        { T::Tex2D_2,               4, T::Tex2D_4               },
        { T::Tex3D_2,               4, T::Tex3D_4               },
        { T::TexCube_2,             4, T::TexCube_4             },
        { T::Texture_Load_1,        2, T::Texture_Load_2        },
        { T::Texture_Load_1,        3, T::Texture_Load_3        },
        { T::Texture_Sample_2,      3, T::Texture_Sample_3      },
        { T::Texture_Sample_2,      4, T::Texture_Sample_4      },
        { T::Texture_Sample_2,      5, T::Texture_Sample_5      },
        { T::Texture_SampleBias_3,  4, T::Texture_SampleBias_4  },
        { T::Texture_SampleBias_3,  5, T::Texture_SampleBias_5  },
        { T::Texture_SampleBias_3,  6, T::Texture_SampleBias_6  },
        { T::Texture_SampleCmp_3,   4, T::Texture_SampleCmp_4   },
        { T::Texture_SampleCmp_3,   5, T::Texture_SampleCmp_5   },
        { T::Texture_SampleCmp_3,   6, T::Texture_SampleCmp_6   },
        { T::Texture_SampleGrad_4,  5, T::Texture_SampleGrad_5  },
        { T::Texture_SampleGrad_4,  6, T::Texture_SampleGrad_6  },
        { T::Texture_SampleGrad_4,  7, T::Texture_SampleGrad_7  },
        { T::Texture_SampleLevel_3, 4, T::Texture_SampleLevel_4 },
        { T::Texture_SampleLevel_3, 5, T::Texture_SampleLevel_5 },
    };

    for (const auto& conversion : intrinsicConversions)
    {
        /* Is another overloaded version of the intrinsic used? */
        if (ast->intrinsic == conversion.standardIntrinsic && ast->arguments.size() == static_cast<std::size_t>(conversion.numArgs))
        {
            /* Convert intrinsic type */
            ast->intrinsic = conversion.overloadedIntrinsic;
            break;
        }
    }
}

void HLSLAnalyzer::AnalyzeIntrinsicWrapperInlining(FunctionCall* ast)
{
    /* Is this a 'clip'-intrinsic call? */
    if (ast->intrinsic == Intrinsic::Clip)
    {
        /* The wrapper function for this intrinsic can be inlined */
        ast->flags << FunctionCall::canInlineIntrinsicWrapper;
    }
}

void HLSLAnalyzer::AnalyzeVarIdent(VarIdent* varIdent)
{
    if (varIdent)
    {
        try
        {
            auto symbol = Fetch(varIdent->ident);
            if (symbol)
                AnalyzeVarIdentWithSymbol(varIdent, symbol);
            else
                ErrorUndeclaredIdent(varIdent->ident, varIdent);
        }
        catch (const ASTRuntimeError& e)
        {
            Error(e.what(), e.GetAST());
        }
        catch (const std::exception& e)
        {
            Error(e.what(), varIdent);
        }
    }
}

void HLSLAnalyzer::AnalyzeVarIdentWithSymbol(VarIdent* varIdent, AST* symbol)
{
    /* Decorate variable identifier with this symbol */
    varIdent->symbolRef = symbol;

    switch (symbol->Type())
    {
        case AST::Types::VarDecl:
            AnalyzeVarIdentWithSymbolVarDecl(varIdent, static_cast<VarDecl*>(symbol));
            break;
        case AST::Types::TextureDecl:
            AnalyzeVarIdentWithSymbolTextureDecl(varIdent, static_cast<TextureDecl*>(symbol));
            break;
        case AST::Types::SamplerDecl:
            AnalyzeVarIdentWithSymbolSamplerDecl(varIdent, static_cast<SamplerDecl*>(symbol));
            break;
        case AST::Types::StructDecl:
            //...
            break;
        case AST::Types::AliasDecl:
            //...
            break;
        default:
            Error("invalid symbol reference to variable identifier '" + varIdent->ToString() + "'", varIdent);
            break;
    }
}

void HLSLAnalyzer::AnalyzeVarIdentWithSymbolVarDecl(VarIdent* varIdent, VarDecl* varDecl)
{
    /* Decorate next identifier */
    if (varIdent->next)
    {
        /* Has variable a struct type denoter? */
        auto varTypeDen = varDecl->GetTypeDenoter()->GetFromArray(varIdent->arrayIndices.size());
        if (auto structTypeDen = varTypeDen->As<StructTypeDenoter>())
        {
            /* Fetch struct member variable declaration from next identifier */
            if (auto memberVarDecl = FetchFromStructDecl(*structTypeDen, varIdent->next->ident, varIdent))
            {
                /* Analyzer next identifier with fetched symbol */
                AnalyzeVarIdentWithSymbol(varIdent->next.get(), memberVarDecl);
            }
        }
    }

    /* Has the variable the fragment coordinate semantic? */
    if (varDecl->semantic == Semantic::Position && shaderTarget_ == ShaderTarget::FragmentShader)
        program_->flags << Program::isFragCoordUsed;
}

void HLSLAnalyzer::AnalyzeVarIdentWithSymbolTextureDecl(VarIdent* varIdent, TextureDecl* textureDecl)
{
    //TODO...
}

void HLSLAnalyzer::AnalyzeVarIdentWithSymbolSamplerDecl(VarIdent* varIdent, SamplerDecl* samplerDecl)
{
    //TODO...
}

void HLSLAnalyzer::AnalyzeEntryPoint(FunctionDecl* funcDecl)
{
    /* Store reference to entry point in root AST node */
    program_->entryPointRef = funcDecl;

    /* Mark this function declaration with the entry point flag */
    funcDecl->flags << FunctionDecl::isEntryPoint;

    /* Analyze all function parameters */
    for (auto& param : funcDecl->parameters)
    {
        if (param->varDecls.size() == 1)
            AnalyzeEntryPointParameter(funcDecl, param.get());
        else
            Error("invalid number of variable declarations in function parameter", param.get());
    }

    /* Analyze return type */
    auto returnTypeDen = funcDecl->returnType->typeDenoter->Get();
    if (auto structTypeDen = returnTypeDen->As<StructTypeDenoter>())
    {
        /* Analyze entry point output structure */
        AnalyzeEntryPointStructInOut(funcDecl, structTypeDen->structDeclRef, "", false);
    }

    /* Check if fragment shader use a slightly different screen space (VPOS vs. SV_Position) */
    if (shaderTarget_ == ShaderTarget::FragmentShader && versionIn_ <= InputShaderVersion::HLSL3)
        program_->flags << Program::hasSM3ScreenSpace;
}

void HLSLAnalyzer::AnalyzeEntryPointParameter(FunctionDecl* funcDecl, VarDeclStmnt* param)
{
    auto varDecl = param->varDecls.front().get();

    /* Analyze input semantic */
    if (param->IsInput())
        AnalyzeEntryPointParameterInOut(funcDecl, varDecl, true);

    /* Analyze output semantic */
    if (param->IsOutput())
        AnalyzeEntryPointParameterInOut(funcDecl, varDecl, false);
}

void HLSLAnalyzer::AnalyzeEntryPointParameterInOut(FunctionDecl* funcDecl, VarDecl* varDecl, bool input)
{
    auto varTypeDen = varDecl->GetTypeDenoter()->Get();
    if (auto structTypeDen = varTypeDen->As<StructTypeDenoter>())
    {
        /* Analyze entry point structure */
        AnalyzeEntryPointStructInOut(funcDecl, structTypeDen->structDeclRef, varDecl->ident, input);
    }
    else
    {
        /* Has the variable a system value semantic? */
        if (varDecl->semantic.IsValid())
        {
            if (varDecl->semantic.IsSystemValue())
                varDecl->flags << VarDecl::isSystemValue;
        }
        else
            Error("missing semantic in parameter '" + varDecl->ident + "' of entry point", varDecl);

        /* Add variable declaration to the global input/output semantics */
        if (input)
        {
            funcDecl->inputSemantics.Add(varDecl);
            varDecl->flags << VarDecl::isShaderInput;
        }
        else
        {
            funcDecl->outputSemantics.Add(varDecl);
            varDecl->flags << VarDecl::isShaderOutput;
        }
    }
}

void HLSLAnalyzer::AnalyzeEntryPointStructInOut(FunctionDecl* funcDecl, StructDecl* structDecl, const std::string& structAliasName, bool input)
{
    /* Set structure alias name */
    structDecl->aliasName = structAliasName;

    /* Analyze all structure members */
    for (auto& member : structDecl->members)
    {
        for (auto& memberVar : member->varDecls)
            AnalyzeEntryPointParameterInOut(funcDecl, memberVar.get(), input);
    }

    /* Mark structure as shader input/output */
    if (input)
        structDecl->flags << StructDecl::isShaderInput;
    else
        structDecl->flags << StructDecl::isShaderOutput;
}

void HLSLAnalyzer::AnalyzeSemantic(IndexedSemantic& semantic)
{
    if (semantic == Semantic::Position && shaderTarget_ == ShaderTarget::VertexShader)
    {
        /* Convert shader semantic to VertexPosition */
        semantic = IndexedSemantic(Semantic::VertexPosition, semantic.Index());
    }
}

void HLSLAnalyzer::AnalyzeEndOfScopes(FunctionDecl& funcDecl)
{
    /* Analyze end of scopes from function body */
    EndOfScopeAnalyzer scopeAnalyzer;
    scopeAnalyzer.MarkEndOfScopesFromFunction(funcDecl);
}

void HLSLAnalyzer::AnalyzeSamplerValue(SamplerValue* ast, SamplerState& samplerState)
{
    const auto& name = ast->name;

    /* Assign value to sampler state */
    if (auto literalExpr = ast->value->As<LiteralExpr>())
    {
        const auto& value = literalExpr->value;

        if (name == "MipLODBias")
            samplerState.mipLODBias = FromString<float>(value);
        else if (name == "MaxAnisotropy")
            samplerState.maxAnisotropy = FromString<unsigned int>(value);
        else if (name == "MinLOD")
            samplerState.minLOD = FromString<float>(value);
        else if (name == "MaxLOD")
            samplerState.maxLOD = FromString<float>(value);
    }
    else if (auto varAccessExpr = ast->value->As<VarAccessExpr>())
    {
        const auto& value = varAccessExpr->varIdent->ident;

        if (name == "Filter")
            AnalyzeSamplerValueFilter(value, samplerState.filter);
        else if (name == "AddressU")
            AnalyzeSamplerValueTextureAddressMode(value, samplerState.addressU);
        else if (name == "AddressV")
            AnalyzeSamplerValueTextureAddressMode(value, samplerState.addressV);
        else if (name == "AddressW")
            AnalyzeSamplerValueTextureAddressMode(value, samplerState.addressW);
        else if (name == "ComparisonFunc")
            AnalyzeSamplerValueComparisonFunc(value, samplerState.comparisonFunc);
    }
    else if (name == "BorderColor")
    {
        try
        {
            if (auto funcCallExpr = ast->value->As<FunctionCallExpr>())
            {
                if (funcCallExpr->call->typeDenoter && funcCallExpr->call->typeDenoter->IsVector() && funcCallExpr->call->arguments.size() == 4)
                {
                    /* Evaluate sub expressions to constant floats */
                    for (std::size_t i = 0; i < 4; ++i)
                        samplerState.borderColor[i] = EvaluateConstExprFloat(*funcCallExpr->call->arguments[i]);
                }
                else
                    throw std::string("invalid type or invalid number of arguments");
            }
            else if (auto castExpr = ast->value->As<CastExpr>())
            {
                /* Evaluate sub expression to constant float and copy into four sub values */
                auto subValueSrc = EvaluateConstExprFloat(*castExpr->expr);
                for (std::size_t i = 0; i < 4; ++i)
                    samplerState.borderColor[i] = subValueSrc;
            }
            else if (auto initExpr = ast->value->As<InitializerExpr>())
            {
                if (initExpr->exprs.size() == 4)
                {
                    /* Evaluate sub expressions to constant floats */
                    for (std::size_t i = 0; i < 4; ++i)
                        samplerState.borderColor[i] = EvaluateConstExprFloat(*initExpr->exprs[i]);
                }
                else
                    throw std::string("invalid number of arguments");
            }
        }
        catch (const std::string& s)
        {
            Warning(s + " to initialize sampler value 'BorderColor'", ast->value.get());
        }
    }
}

void HLSLAnalyzer::AnalyzeSamplerValueFilter(const std::string& value, SamplerState::Filter& filter)
{
    using T = SamplerState::Filter;

    static const std::map<std::string, T> valueMap
    {
        { "MIN_MAG_MIP_POINT",                          T::MinMagMipPoint                       },
        { "MIN_MAG_POINT_MIP_LINEAR",                   T::MinMagPointMipLinear                 },
        { "MIN_POINT_MAG_LINEAR_MIP_POINT",             T::MinPointMagLinearMipPoint            },
        { "MIN_POINT_MAG_MIP_LINEAR",                   T::MinPointMagMipLinear                 },
        { "MIN_LINEAR_MAG_MIP_POINT",                   T::MinLinearMagMipPoint                 },
        { "MIN_LINEAR_MAG_POINT_MIP_LINEAR",            T::MinLinearMagPointMipLinear           },
        { "MIN_MAG_LINEAR_MIP_POINT",                   T::MinMagLinearMipPoint                 },
        { "MIN_MAG_MIP_LINEAR",                         T::MinMagMipLinear                      },
        { "ANISOTROPIC",                                T::Anisotropic                          },
        { "COMPARISON_MIN_MAG_MIP_POINT",               T::ComparisonMinMagMipPoint             },
        { "COMPARISON_MIN_MAG_POINT_MIP_LINEAR",        T::ComparisonMinMagPointMipLinear       },
        { "COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT",  T::ComparisonMinPointMagLinearMipPoint  },
        { "COMPARISON_MIN_POINT_MAG_MIP_LINEAR",        T::ComparisonMinPointMagMipLinear       },
        { "COMPARISON_MIN_LINEAR_MAG_MIP_POINT",        T::ComparisonMinLinearMagMipPoint       },
        { "COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR", T::ComparisonMinLinearMagPointMipLinear },
        { "COMPARISON_MIN_MAG_LINEAR_MIP_POINT",        T::ComparisonMinMagLinearMipPoint       },
        { "COMPARISON_MIN_MAG_MIP_LINEAR",              T::ComparisonMinMagMipLinear            },
        { "COMPARISON_ANISOTROPIC",                     T::ComparisonAnisotropic                },
        { "MINIMUM_MIN_MAG_MIP_POINT",                  T::MinimumMinMagMipPoint                },
        { "MINIMUM_MIN_MAG_POINT_MIP_LINEAR",           T::MinimumMinMagPointMipLinear          },
        { "MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT",     T::MinimumMinPointMagLinearMipPoint     },
        { "MINIMUM_MIN_POINT_MAG_MIP_LINEAR",           T::MinimumMinPointMagMipLinear          },
        { "MINIMUM_MIN_LINEAR_MAG_MIP_POINT",           T::MinimumMinLinearMagMipPoint          },
        { "MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR",    T::MinimumMinLinearMagPointMipLinear    },
        { "MINIMUM_MIN_MAG_LINEAR_MIP_POINT",           T::MinimumMinMagLinearMipPoint          },
        { "MINIMUM_MIN_MAG_MIP_LINEAR",                 T::MinimumMinMagMipLinear               },
        { "MINIMUM_ANISOTROPIC",                        T::MinimumAnisotropic                   },
        { "MAXIMUM_MIN_MAG_MIP_POINT",                  T::MaximumMinMagMipPoint                },
        { "MAXIMUM_MIN_MAG_POINT_MIP_LINEAR",           T::MaximumMinMagPointMipLinear          },
        { "MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT",     T::MaximumMinPointMagLinearMipPoint     },
        { "MAXIMUM_MIN_POINT_MAG_MIP_LINEAR",           T::MaximumMinPointMagMipLinear          },
        { "MAXIMUM_MIN_LINEAR_MAG_MIP_POINT",           T::MaximumMinLinearMagMipPoint          },
        { "MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR",    T::MaximumMinLinearMagPointMipLinear    },
        { "MAXIMUM_MIN_MAG_LINEAR_MIP_POINT",           T::MaximumMinMagLinearMipPoint          },
        { "MAXIMUM_MIN_MAG_MIP_LINEAR",                 T::MaximumMinMagMipLinear               },
        { "MAXIMUM_ANISOTROPIC",                        T::MaximumAnisotropic                   },
    };

    auto it = valueMap.find(value);
    if (it != valueMap.end())
        filter = it->second;
}

void HLSLAnalyzer::AnalyzeSamplerValueTextureAddressMode(const std::string& value, SamplerState::TextureAddressMode& addressMode)
{
    using T = SamplerState::TextureAddressMode;

    static const std::map<std::string, T> valueMap
    {
        { "WRAP",        T::Wrap       },
        { "MIRROR",      T::Mirror     },
        { "CLAMP",       T::Clamp      },
        { "BORDER",      T::Border     },
        { "MIRROR_ONCE", T::MirrorOnce },
    };

    auto it = valueMap.find(value);
    if (it != valueMap.end())
        addressMode = it->second;
}

void HLSLAnalyzer::AnalyzeSamplerValueComparisonFunc(const std::string& value, SamplerState::ComparisonFunc& comparisonFunc)
{
    using T = SamplerState::ComparisonFunc;

    static const std::map<std::string, T> valueMap
    {
        { "COMPARISON_NEVER",         T::Never        },
        { "COMPARISON_LESS",          T::Less         },
        { "COMPARISON_EQUAL",         T::Equal        },
        { "COMPARISON_LESS_EQUAL",    T::LessEqual    },
        { "COMPARISON_GREATER",       T::Greater      },
        { "COMPARISON_NOT_EQUAL",     T::NotEqual     },
        { "COMPARISON_GREATER_EQUAL", T::GreaterEqual },
        { "COMPARISON_ALWAYS",        T::Always       },
    };

    auto it = valueMap.find(value);
    if (it != valueMap.end())
        comparisonFunc = it->second;
}



} // /namespace Xsc



// ================================================================================
