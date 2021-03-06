/*
 * Token.h
 * 
 * This file is part of the XShaderCompiler project (Copyright (c) 2014-2016 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#ifndef XSC_TOKEN_H
#define XSC_TOKEN_H


#include "SourceArea.h"
#include <string>
#include <memory>


namespace Xsc
{


// Token classes used by the scanner and parser.
class Token
{
    
    public:
        
        // Token type enumeration.
        enum class Types
        {
            Undefined,

            /* --- Identifiers --- */
            Ident,              // (letter | '_') (letter | '_' | digit)*

            /* --- Literals --- */
            BoolLiteral,        // true | false
            IntLiteral,         // digit+
            FloatLiteral,       // digit+ '.' digit+
            StringLiteral,      // '"' ANY+ '"'

            /* --- Operators --- */
            AssignOp,           // =, +=, -=, *=, /=, %=, <<=, >>=, |= , &=, ^=
            BinaryOp,           // &&, ||, |, ^, &, <<, >>, +, -, *, /, %, ==, !=, <, >, <=, >=
            UnaryOp,            // !, ~, +, -, ++, --
            TernaryOp,          // ?

            /* --- Punctuation --- */
            Dot,                // .
            Colon,              // :
            Semicolon,          // ;
            Comma,              // ,

            /* --- Brackets --- */
            LBracket,           // (
            RBracket,           // )
            LCurly,             // {
            RCurly,             // }
            LParen,             // [
            RParen,             // ]

            /* --- Type denoters --- */
            StringType,         // string
            ScalarType,         // bool, int, uint, half, float, double
            VectorType,         // ScalarType ('1'-'4')
            MatrixType,         // ScalarType ('1'-'4') 'x' ('1'-'4')

            Sampler,            // sampler, sampler1D, sampler2D, sampler3D, samplerCUBE
            SamplerState,       // sampler_state, SamplerState, SamplerComparisonState

            /*
            texture,
            Texture1D, Texture1DArray, Texture2D, Texture2DArray, Texture3D, TextureCube, TextureCubeArray,
            Texture2DMS, Texture2DMSArray, RWTexture1D, RWTexture1DArray, RWTexture2D, RWTexture2DArray, RWTexture3D
            */
            Texture,

            /*
            AppendStructuredBuffer, Buffer, ByteAddressBuffer, ConsumeStructuredBuffer,
            StructuredBuffer, RWBuffer, RWByteAddressBuffer, RWStructuredBuffer
            */
            StorageBuffer,

            UniformBuffer,      // cbuffer, tbuffer

            Vector,             // vector (e.g. "vector<float, 3>")
            Matrix,             // matrix (e.g. "matrix<int, 4, 4>")

            Void,               // void

            /* --- Keywords --- */
            Reserved,           // reserved keyword (not allowed, but reserved for future use)

            Do,                 // do
            While,              // while
            For,                // for
            
            If,                 // if
            Else,               // else
            
            Switch,             // switch
            Case,               // case
            Default,            // default

            Typedef,            // typedef
            Struct,             // struct
            Register,           // register
            PackOffset,         // packoffset

            CtrlTransfer,       // break, continue, discard
            Return,             // return

            InputModifier,      // in, out, inout, uniform

            /*
            extern, nointerpolation, precise, shared, groupshared, static,
            uniform, volatile, linear, centroid, noperspective, sample
            */
            StorageClass,
            TypeModifier,       // const, row_major, column_major

            Inline,             // inline

            /* --- Technique keywords --- */
            Technique,          // technique
            Pass,               // pass
            Compile,            // compile

            /* --- Preprocessor specific tokens --- */
            Directive,          // Preprocessor directive ('#' IDENT).
            DirectiveConcat,    // Preprocessor directive concatenation ('##').
            Comment,            // Commentary (only a single text line)
            WhiteSpaces,        // White spaces (' ', '\t', '\r')
            NewLines,           // New-line characters ('\n', "\r\n")
            LineBreak,          // Line break for pre-processor directives '\'
            VarArg,             // Variadic argument specifier ('...').
            Misc,               // Miscellaneous

            /* --- Special tokens --- */
            EndOfStream,        // End-of-stream
        };

        Token(Token&& other);

        Token(const SourcePosition& pos, const Types type);
        Token(const SourcePosition& pos, const Types type, const std::string& spell);
        Token(const SourcePosition& pos, const Types type, std::string&& spell);

        // Returns the source area of this token.
        SourceArea Area() const;

        // Returns a descriptive string for the specified token type.
        static std::string TypeToString(const Types type);

        // Returns the token spelling of the content (e.g. only the content of a string literal within the quotes).
        std::string SpellContent() const;

        // Returns the token type.
        inline Types Type() const
        {
            return type_;
        }

        // Returns the token source position.
        inline const SourcePosition& Pos() const
        {
            return pos_;
        }

        // Returns the token spelling.
        inline const std::string& Spell() const
        {
            return spell_;
        }

        #ifdef XSC_ENABLE_MEMORY_POOL

        void* operator new (std::size_t count);
        void operator delete (void* ptr);

        #endif

    private:

        Types           type_;  // Type of this token.
        SourcePosition  pos_;   // Source area of this token.
        std::string     spell_; // Token spelling.

};

using TokenPtr = std::shared_ptr<Token>;


} // /namespace Xsc


#endif



// ================================================================================