/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#ifndef INCLUDED_XMLSECURITY_INC_DOCUMENTSIGNATUREMANAGER_HXX
#define INCLUDED_XMLSECURITY_INC_DOCUMENTSIGNATUREMANAGER_HXX

#include "xmlsecuritydllapi.h"
#include <xmlsecurity/sigstruct.hxx>
#include <xmlsecurity/xmlsignaturehelper.hxx>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <com/sun/star/embed/XStorage.hpp>
#include <xmlsecurity/documentsignaturehelper.hxx>
#include <com/sun/star/beans/PropertyValue.hpp>

/// Manages signatures (addition, removal), used by DigitalSignaturesDialog.
class XMLSECURITY_DLLPUBLIC DocumentSignatureManager
{
public:
    css::uno::Reference<css::uno::XComponentContext> mxContext;
    css::uno::Reference<css::embed::XStorage> mxStore;
    XMLSignatureHelper maSignatureHelper;
    SignatureInformations maCurrentSignatureInformations;
    DocumentSignatureMode meSignatureMode;
    css::uno::Sequence< css::uno::Sequence<css::beans::PropertyValue> > m_manifest;
    css::uno::Reference<css::io::XStream> mxSignatureStream;
    css::uno::Reference<css::io::XStream> mxTempSignatureStream;
    /// Storage containing all OOXML signatures, unused for ODF.
    css::uno::Reference<css::embed::XStorage> mxTempSignatureStorage;

    DocumentSignatureManager(const css::uno::Reference<css::uno::XComponentContext>& xContext, DocumentSignatureMode eMode);
    ~DocumentSignatureManager();
    /**
     * Checks if a particular stream is a valid xml stream. Those are treated
     * differently when they are signed (c14n transformation)
     */
    bool isXML(const OUString& rURI);
    SignatureStreamHelper ImplOpenSignatureStream(sal_Int32 eStreamMode, bool bTempStream);
    /// Add a new signature, using xCert as a signing certificate, and rDescription as description.
    bool add(const css::uno::Reference<css::security::XCertificate>& xCert, const OUString& rDescription, sal_Int32& nSecurityId);
    /// Remove signature at nPosition.
    void remove(sal_uInt16 nPosition);
    /// Read signatures from either a temp stream or the real storage.
    void read(bool bUseTempStream, bool bCacheLastSignature = true);
};

#endif // INCLUDED_XMLSECURITY_INC_XMLSECURITY_DOCUMENTSIGNATUREMANAGER_HXX

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
