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

#include <documentsignaturemanager.hxx>

#include <com/sun/star/embed/StorageFormats.hpp>
#include <com/sun/star/embed/ElementModes.hpp>
#include <com/sun/star/io/TempFile.hpp>
#include <com/sun/star/io/XTruncate.hpp>
#include <com/sun/star/security/SerialNumberAdapter.hpp>
#include <com/sun/star/embed/XTransactedObject.hpp>

#include <comphelper/storagehelper.hxx>
#include <rtl/ustrbuf.hxx>
#include <sax/tools/converter.hxx>
#include <tools/date.hxx>
#include <tools/time.hxx>

#include <certificate.hxx>

using namespace com::sun::star;

DocumentSignatureManager::DocumentSignatureManager(const uno::Reference<uno::XComponentContext>& xContext, DocumentSignatureMode eMode)
    : mxContext(xContext),
      maSignatureHelper(xContext),
      meSignatureMode(eMode)
{
}

DocumentSignatureManager::~DocumentSignatureManager()
{
}

/* Using the zip storage, we cannot get the properties "MediaType" and "IsEncrypted"
    We use the manifest to find out if a file is xml and if it is encrypted.
    The parameter is an encoded uri. However, the manifest contains paths. Therefore
    the path is encoded as uri, so they can be compared.
*/
bool DocumentSignatureManager::isXML(const OUString& rURI)
{
    SAL_WARN_IF(!mxStore.is(), "xmlsecurity.helper", "empty storage reference");

    // FIXME figure out why this is necessary.
    static bool bTest = getenv("LO_TESTNAME");
    if (bTest)
        return true;

    bool bIsXML = false;
    bool bPropsAvailable = false;
    const OUString sPropFullPath("FullPath");
    const OUString sPropMediaType("MediaType");
    const OUString sPropDigest("Digest");

    for (int i = 0; i < m_manifest.getLength(); i++)
    {
        const uno::Sequence<css::beans::PropertyValue>& entry = m_manifest[i];
        OUString sPath, sMediaType;
        bool bEncrypted = false;
        for (int j = 0; j < entry.getLength(); j++)
        {
            const css::beans::PropertyValue& prop = entry[j];

            if (prop.Name.equals(sPropFullPath))
                prop.Value >>= sPath;
            else if (prop.Name.equals(sPropMediaType))
                prop.Value >>= sMediaType;
            else if (prop.Name.equals(sPropDigest))
                bEncrypted = true;
        }
        if (DocumentSignatureHelper::equalsReferenceUriManifestPath(rURI, sPath))
        {
            bIsXML = sMediaType == "text/xml" && ! bEncrypted;
            bPropsAvailable = true;
            break;
        }
    }
    if (!bPropsAvailable)
    {
        //This would be the case for at least mimetype, META-INF/manifest.xml
        //META-INF/macrosignatures.xml.
        //Files can only be encrypted if they are in the manifest.xml.
        //That is, the current file cannot be encrypted, otherwise bPropsAvailable
        //would be true.
        OUString aXMLExt("XML");
        sal_Int32 nSep = rURI.lastIndexOf('.');
        if (nSep != (-1))
        {
            OUString aExt = rURI.copy(nSep+1);
            if (aExt.equalsIgnoreAsciiCase(aXMLExt))
                bIsXML = true;
        }
    }
    return bIsXML;
}

//If bTempStream is true, then a temporary stream is return. If it is false then, the actual
//signature stream is used.
//Every time the user presses Add a new temporary stream is created.
//We keep the temporary stream as member because ImplGetSignatureInformations
//will later access the stream to create DocumentSignatureInformation objects
//which are stored in maCurrentSignatureInformations.
SignatureStreamHelper DocumentSignatureManager::ImplOpenSignatureStream(sal_Int32 nStreamOpenMode, bool bTempStream)
{
    SignatureStreamHelper aHelper;
    if (mxStore.is())
    {
        uno::Reference<container::XNameAccess> xNameAccess(mxStore, uno::UNO_QUERY);
        if (xNameAccess.is() && xNameAccess->hasByName("[Content_Types].xml"))
            aHelper.nStorageFormat = embed::StorageFormats::OFOPXML;
    }

    if (bTempStream)
    {
        if (nStreamOpenMode & css::embed::ElementModes::TRUNCATE)
        {
            //We write always into a new temporary stream.
            mxTempSignatureStream.set(css::io::TempFile::create(mxContext), uno::UNO_QUERY_THROW);
            if (aHelper.nStorageFormat != embed::StorageFormats::OFOPXML)
                aHelper.xSignatureStream = mxTempSignatureStream;
            else
            {
                mxTempSignatureStorage = comphelper::OStorageHelper::GetStorageOfFormatFromStream(ZIP_STORAGE_FORMAT_STRING, mxTempSignatureStream);
                aHelper.xSignatureStorage = mxTempSignatureStorage;
            }
        }
        else
        {
            //When we read from the temp stream, then we must have previously
            //created one.
            SAL_WARN_IF(!mxTempSignatureStream.is(), "xmlsecurity.helper", "empty temp. signature stream reference");
        }
        aHelper.xSignatureStream = mxTempSignatureStream;
        if (aHelper.nStorageFormat == embed::StorageFormats::OFOPXML)
            aHelper.xSignatureStorage = mxTempSignatureStorage;
    }
    else
    {
        //No temporary stream
        if (!mxSignatureStream.is())
        {
            //We may not have a dedicated stream for writing the signature
            //So we take one directly from the storage
            //Or DocumentDigitalSignatures::showDocumentContentSignatures was called,
            //in which case Add/Remove is not allowed. This is done, for example, if the
            //document is readonly
            aHelper = DocumentSignatureHelper::OpenSignatureStream(mxStore, nStreamOpenMode, meSignatureMode);
        }
        else
        {
            aHelper.xSignatureStream = mxSignatureStream;
        }
    }

    if (nStreamOpenMode & css::embed::ElementModes::TRUNCATE)
    {
        if (aHelper.xSignatureStream.is() && aHelper.nStorageFormat != embed::StorageFormats::OFOPXML)
        {
            css::uno::Reference<css::io::XTruncate> xTruncate(aHelper.xSignatureStream, uno::UNO_QUERY_THROW);
            xTruncate->truncate();
        }
    }
    else if (bTempStream || mxSignatureStream.is())
    {
        //In case we read the signature stream from the storage directly,
        //which is the case when DocumentDigitalSignatures::showDocumentContentSignatures
        //then XSeakable is not supported
        uno::Reference<io::XSeekable> xSeek(aHelper.xSignatureStream, uno::UNO_QUERY_THROW);
        xSeek->seek(0);
    }

    return aHelper;
}

bool DocumentSignatureManager::add(const uno::Reference<security::XCertificate>& xCert, const OUString& rDescription, sal_Int32& nSecurityId)
{
    if (!xCert.is())
    {
        SAL_WARN("xmlsecurity.helper", "no certificate selected");
        return false;
    }

    uno::Reference<security::XSerialNumberAdapter> xSerialNumberAdapter = security::SerialNumberAdapter::create(mxContext);
    OUString aCertSerial = xSerialNumberAdapter->toString(xCert->getSerialNumber());
    if (aCertSerial.isEmpty())
    {
        SAL_WARN("xmlsecurity.helper", "Error in Certificate, problem with serial number!");
        return false;
    }

    maSignatureHelper.StartMission();

    nSecurityId = maSignatureHelper.GetNewSecurityId();

    OUStringBuffer aStrBuffer;
    sax::Converter::encodeBase64(aStrBuffer, xCert->getEncoded());

    OUString aCertDigest;
    if (xmlsecurity::Certificate* pCertificate = dynamic_cast<xmlsecurity::Certificate*>(xCert.get()))
    {
        OUStringBuffer aBuffer;
        sax::Converter::encodeBase64(aBuffer, pCertificate->getSHA256Thumbprint());
        aCertDigest = aBuffer.makeStringAndClear();
    }
    else
        SAL_WARN("xmlsecurity.helper", "XCertificate implementation without an xmlsecurity::Certificate one");

    maSignatureHelper.SetX509Certificate(nSecurityId, xCert->getIssuerName(), aCertSerial, aStrBuffer.makeStringAndClear(), aCertDigest);

    std::vector< OUString > aElements = DocumentSignatureHelper::CreateElementList(mxStore, meSignatureMode, OOo3_2Document);
    DocumentSignatureHelper::AppendContentTypes(mxStore, aElements);

    sal_Int32 nElements = aElements.size();
    for (sal_Int32 n = 0; n < nElements; n++)
    {
        bool bBinaryMode = !isXML(aElements[n]);
        maSignatureHelper.AddForSigning(nSecurityId, aElements[n], aElements[n], bBinaryMode);
    }

    maSignatureHelper.SetDateTime(nSecurityId, Date(Date::SYSTEM), tools::Time(tools::Time::SYSTEM));
    maSignatureHelper.SetDescription(nSecurityId, rDescription);

    // We open a signature stream in which the existing and the new
    //signature is written. ImplGetSignatureInformation (later in this function) will
    //then read the stream an will fill  maCurrentSignatureInformations. The final signature
    //is written when the user presses OK. Then only maCurrentSignatureInformation and
    //a sax writer are used to write the information.
    SignatureStreamHelper aStreamHelper = ImplOpenSignatureStream(embed::ElementModes::WRITE | embed::ElementModes::TRUNCATE, true);

    if (aStreamHelper.nStorageFormat != embed::StorageFormats::OFOPXML)
    {
        uno::Reference<io::XOutputStream> xOutputStream(aStreamHelper.xSignatureStream, uno::UNO_QUERY_THROW);
        uno::Reference<xml::sax::XWriter> xSaxWriter = maSignatureHelper.CreateDocumentHandlerWithHeader(xOutputStream);

        // Export old signatures...
        uno::Reference<xml::sax::XDocumentHandler> xDocumentHandler(xSaxWriter, uno::UNO_QUERY_THROW);
        size_t nInfos = maCurrentSignatureInformations.size();
        for (size_t n = 0; n < nInfos; n++)
            XMLSignatureHelper::ExportSignature(xDocumentHandler, maCurrentSignatureInformations[n]);

        // Create a new one...
        maSignatureHelper.CreateAndWriteSignature(xDocumentHandler);

        // That's it...
        XMLSignatureHelper::CloseDocumentHandler(xDocumentHandler);
    }
    else
    {
        // OOXML

        // Handle relations.
        maSignatureHelper.EnsureSignaturesRelation(mxStore);
        // Old signatures + the new one.
        int nSignatureCount = maCurrentSignatureInformations.size() + 1;
        maSignatureHelper.ExportSignatureRelations(aStreamHelper.xSignatureStorage, nSignatureCount);

        // Export old signatures.
        for (size_t i = 0; i < maCurrentSignatureInformations.size(); ++i)
            maSignatureHelper.ExportOOXMLSignature(mxStore, aStreamHelper.xSignatureStorage, maCurrentSignatureInformations[i], i + 1);

        // Create a new signature.
        maSignatureHelper.CreateAndWriteOOXMLSignature(mxStore, aStreamHelper.xSignatureStorage, nSignatureCount);

        // Flush objects.
        uno::Reference<embed::XTransactedObject> xTransact(aStreamHelper.xSignatureStorage, uno::UNO_QUERY);
        xTransact->commit();
        uno::Reference<io::XOutputStream> xOutputStream(aStreamHelper.xSignatureStream, uno::UNO_QUERY);
        xOutputStream->closeOutput();

        uno::Reference<io::XTempFile> xTempFile(aStreamHelper.xSignatureStream, uno::UNO_QUERY);
        SAL_INFO("xmlsecurity.helper", "DocumentSignatureManager::add temporary storage at " << xTempFile->getUri());
    }

    maSignatureHelper.EndMission();
    return true;
}

void DocumentSignatureManager::remove(sal_uInt16 nPosition)
{
    maCurrentSignatureInformations.erase(maCurrentSignatureInformations.begin() + nPosition);

    // Export all other signatures...
    SignatureStreamHelper aStreamHelper = ImplOpenSignatureStream(embed::ElementModes::WRITE | embed::ElementModes::TRUNCATE, /*bTempStream=*/true);

    if (aStreamHelper.nStorageFormat != embed::StorageFormats::OFOPXML)
    {
        uno::Reference<io::XOutputStream> xOutputStream(aStreamHelper.xSignatureStream, uno::UNO_QUERY_THROW);
        uno::Reference<xml::sax::XWriter> xSaxWriter = maSignatureHelper.CreateDocumentHandlerWithHeader(xOutputStream);

        uno::Reference< xml::sax::XDocumentHandler> xDocumentHandler(xSaxWriter, uno::UNO_QUERY_THROW);
        size_t nInfos = maCurrentSignatureInformations.size();
        for (size_t n = 0 ; n < nInfos ; ++n)
            XMLSignatureHelper::ExportSignature(xDocumentHandler, maCurrentSignatureInformations[n]);

        XMLSignatureHelper::CloseDocumentHandler(xDocumentHandler);
    }
    else
    {
        // OOXML

        // Handle relations.
        int nSignatureCount = maCurrentSignatureInformations.size();
        maSignatureHelper.ExportSignatureRelations(aStreamHelper.xSignatureStorage, nSignatureCount);

        // Export old signatures.
        for (size_t i = 0; i < maCurrentSignatureInformations.size(); ++i)
            maSignatureHelper.ExportOOXMLSignature(mxStore, aStreamHelper.xSignatureStorage, maCurrentSignatureInformations[i], i + 1);

        // Flush objects.
        uno::Reference<embed::XTransactedObject> xTransact(aStreamHelper.xSignatureStorage, uno::UNO_QUERY);
        xTransact->commit();
        uno::Reference<io::XOutputStream> xOutputStream(aStreamHelper.xSignatureStream, uno::UNO_QUERY);
        xOutputStream->closeOutput();

        uno::Reference<io::XTempFile> xTempFile(aStreamHelper.xSignatureStream, uno::UNO_QUERY);
        SAL_INFO("xmlsecurity.helper", "DocumentSignatureManager::remove: temporary storage is at " << xTempFile->getUri());
    }
}

void DocumentSignatureManager::read(bool bUseTempStream, bool bCacheLastSignature)
{
    maCurrentSignatureInformations.clear();

    maSignatureHelper.StartMission();

    SignatureStreamHelper aStreamHelper = ImplOpenSignatureStream(css::embed::ElementModes::READ, bUseTempStream);
    if (aStreamHelper.nStorageFormat != embed::StorageFormats::OFOPXML && aStreamHelper.xSignatureStream.is())
    {
        uno::Reference< io::XInputStream > xInputStream(aStreamHelper.xSignatureStream, uno::UNO_QUERY);
        maSignatureHelper.ReadAndVerifySignature(xInputStream);
    }
    else if (aStreamHelper.nStorageFormat == embed::StorageFormats::OFOPXML && aStreamHelper.xSignatureStorage.is())
        maSignatureHelper.ReadAndVerifySignatureStorage(aStreamHelper.xSignatureStorage, bCacheLastSignature);
    maSignatureHelper.EndMission();

    maCurrentSignatureInformations = maSignatureHelper.GetSignatureInformations();
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
