/*
 * Exception.cpp
 * 
 * This file is part of the XShaderCompiler project (Copyright (c) 2014-2016 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#include "Exception.h"


namespace Xsc
{


ASTRuntimeError::ASTRuntimeError(const char* msg, const AST* ast) :
    std::runtime_error  { msg },
    ast_                { ast }
{
}

ASTRuntimeError::ASTRuntimeError(const std::string& msg, const AST* ast) :
    std::runtime_error  { msg },
    ast_                { ast }
{
}

[[noreturn]]
void RuntimeErr(const char* msg)
{
    throw std::runtime_error(msg);
}

[[noreturn]]
void RuntimeErr(const std::string& msg)
{
    throw std::runtime_error(msg);
}

[[noreturn]]
void RuntimeErr(const char* msg, const AST* ast)
{
    throw ASTRuntimeError(msg, ast);
}

[[noreturn]]
void RuntimeErr(const std::string& msg, const AST* ast)
{
    throw ASTRuntimeError(msg, ast);
}

[[noreturn]]
void InvalidArg(const char* msg)
{
    throw std::invalid_argument(msg);
}

[[noreturn]]
void InvalidArg(const std::string& msg)
{
    throw std::invalid_argument(msg);
}


} // /namespace Xsc



// ================================================================================
