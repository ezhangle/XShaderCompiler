/*
 * GLSLKeywords.h
 * 
 * This file is part of the XShaderCompiler project (Copyright (c) 2014-2016 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#ifndef XSC_GLSL_KEYWORDS_H
#define XSC_GLSL_KEYWORDS_H


#include "Token.h"
#include "ASTEnums.h"
#include <string>
#include <memory>


namespace Xsc
{


// Returns true if the specified identifier is a reserved GLSL keyword.
bool IsGLSLKeyword(const std::string& ident);

// Returns the GLSL keyword for the specified data type or null on failure.
const std::string* DataTypeToGLSLKeyword(const DataType t);

// Returns the GLSL keyword for the specified storage class or null on failure.
const std::string* StorageClassToGLSLKeyword(const StorageClass t);

// Returns the GLSL keyword for the specified buffer type or null on failure.
const std::string* BufferTypeToGLSLKeyword(const BufferType t);

// Returns the GLSL keyword for the specified semantic.
std::unique_ptr<std::string> SemanticToGLSLKeyword(const IndexedSemantic& semantic);


} // /namespace Xsc


#endif



// ================================================================================