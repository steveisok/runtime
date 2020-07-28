// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.CodeDom;
using System.CodeDom.Compiler;
using System.Collections;
using System.ComponentModel.Design;
using System.Xml;
using System.Xml.Schema;
using System.Xml.Serialization;
using System.Xml.Serialization.Advanced;

namespace System.Data.Design
{
    public class TypedDataSetSchemaImporterExtension : SchemaImporterExtension
    {
        public TypedDataSetSchemaImporterExtension ()
            : this (TypedDataSetGenerator.GenerateOption.None)
        {
        }

        protected TypedDataSetSchemaImporterExtension (TypedDataSetGenerator.GenerateOption dataSetGenerateOptions)
        {
        }

        public override string ImportSchemaType (XmlSchemaType type, XmlSchemaObject context, XmlSchemas schemas, XmlSchemaImporter importer, CodeCompileUnit compileUnit, CodeNamespace mainNamespace, CodeGenerationOptions options, CodeDomProvider codeProvider)
        {
            if (type == null)
                return null;

            var xe = context as XmlSchemaElement;
            if (xe == null)
                return null;

            return null;
        }

        public override string ImportSchemaType (string name, string namespaceName, XmlSchemaObject context, XmlSchemas schemas, XmlSchemaImporter importer, CodeCompileUnit compileUnit, CodeNamespace mainNamespace, CodeGenerationOptions options, CodeDomProvider codeProvider)
        {
            return null;
        }
    }
}