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


#include "xsecctl.hxx"
#include <algorithm>
#include <initializer_list>
#include <tools/debug.hxx>

#include <com/sun/star/xml/crypto/sax/ElementMarkPriority.hpp>
#include <com/sun/star/xml/crypto/sax/XReferenceResolvedBroadcaster.hpp>
#include <com/sun/star/xml/crypto/sax/XMissionTaker.hpp>
#include <com/sun/star/xml/crypto/sax/XReferenceCollector.hpp>
#include <com/sun/star/xml/crypto/sax/XSAXEventKeeperStatusChangeBroadcaster.hpp>
#include <com/sun/star/xml/crypto/SecurityOperationStatus.hpp>
#include <com/sun/star/embed/XHierarchicalStorageAccess.hpp>
#include <com/sun/star/embed/ElementModes.hpp>
#include <com/sun/star/beans/StringPair.hpp>

#include <xmloff/attrlist.hxx>
#include <rtl/math.hxx>
#include <rtl/ref.hxx>
#include <unotools/datetime.hxx>
#include <comphelper/ofopxmlhelper.hxx>
#include <sax/tools/converter.hxx>

namespace cssu = com::sun::star::uno;
namespace cssl = com::sun::star::lang;
namespace cssxc = com::sun::star::xml::crypto;
namespace cssxs = com::sun::star::xml::sax;
namespace cssxw = com::sun::star::xml::wrapper;
using namespace com::sun::star;

/* bridge component names */
#define XMLSIGNATURE_COMPONENT "com.sun.star.xml.crypto.XMLSignature"
#define XMLDOCUMENTWRAPPER_COMPONENT "com.sun.star.xml.wrapper.XMLDocumentWrapper"

/* xml security framework components */
#define SAXEVENTKEEPER_COMPONENT "com.sun.star.xml.crypto.sax.SAXEventKeeper"

XSecController::XSecController( const cssu::Reference<cssu::XComponentContext>& rxCtx )
    : mxCtx(rxCtx)
    , m_nNextSecurityId(1)
    , m_bIsPreviousNodeInitializable(false)
    , m_bIsSAXEventKeeperConnected(false)
    , m_bIsCollectingElement(false)
    , m_bIsBlocking(false)
    , m_nStatusOfSecurityComponents(UNINITIALIZED)
    , m_bIsSAXEventKeeperSticky(false)
    , m_pErrorMessage(nullptr)
    , m_nReservedSignatureId(0)
    , m_bVerifyCurrentSignature(false)
{
}

XSecController::~XSecController()
{
}


/*
 * private methods
 */
int XSecController::findSignatureInfor( sal_Int32 nSecurityId) const
/****** XSecController/findSignatureInfor *************************************
 *
 *   NAME
 *  findSignatureInfor -- find SignatureInformation struct for a particular
 *                        signature
 *
 *   SYNOPSIS
 *  index = findSignatureInfor( nSecurityId );
 *
 *   FUNCTION
 *  see NAME.
 *
 *   INPUTS
 *  nSecurityId - the signature's id
 *
 *   RESULT
 *  index - the index of the signature, or -1 when no such signature
 *          existing
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    int i;
    int size = m_vInternalSignatureInformations.size();

    for (i=0; i<size; ++i)
    {
        if (m_vInternalSignatureInformations[i].signatureInfor.nSecurityId == nSecurityId)
        {
            return i;
        }
    }

    return -1;
}

void XSecController::createXSecComponent( )
/****** XSecController/createXSecComponent ************************************
 *
 *   NAME
 *  bResult = createXSecComponent -- creates xml security components
 *
 *   SYNOPSIS
 *  createXSecComponent( );
 *
 *   FUNCTION
 *  Creates xml security components, including:
 *  1. an xml signature bridge component ( Java based or C based)
 *  2. an XMLDocumentWrapper component ( Java based or C based)
 *  3. a SAXEventKeeper component
 *
 *   INPUTS
 *  empty
 *
 *   RESULT
 *  empty
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    OUString sSAXEventKeeper( SAXEVENTKEEPER_COMPONENT );
    OUString sXMLSignature( XMLSIGNATURE_COMPONENT );
    OUString sXMLDocument( XMLDOCUMENTWRAPPER_COMPONENT );

    /*
     * marks all security components are not available.
     */
    m_nStatusOfSecurityComponents = FAILTOINITIALIZED;
    m_xXMLSignature = nullptr;
    m_xXMLDocumentWrapper = nullptr;
    m_xSAXEventKeeper = nullptr;

    cssu::Reference< cssl::XMultiComponentFactory > xMCF( mxCtx->getServiceManager() );

    m_xXMLSignature.set(
        xMCF->createInstanceWithContext( sXMLSignature, mxCtx ),
        cssu::UNO_QUERY );

    bool bSuccess = m_xXMLSignature.is();
    if ( bSuccess )
    /*
     * XMLSignature created successfully.
     */
    {
        m_xXMLDocumentWrapper.set(
            xMCF->createInstanceWithContext( sXMLDocument, mxCtx ),
            cssu::UNO_QUERY );
    }

    bSuccess &= m_xXMLDocumentWrapper.is();
    if ( bSuccess )
    /*
     * XMLDocumentWrapper created successfully.
     */
    {
        m_xSAXEventKeeper.set(
            xMCF->createInstanceWithContext( sSAXEventKeeper, mxCtx ),
            cssu::UNO_QUERY );
    }

    bSuccess &= m_xSAXEventKeeper.is();

    if (bSuccess)
    /*
     * SAXEventKeeper created successfully.
     */
    {
        cssu::Reference< cssl::XInitialization > xInitialization(m_xSAXEventKeeper,  cssu::UNO_QUERY);

        cssu::Sequence <cssu::Any> arg(1);
        arg[0] = cssu::makeAny(m_xXMLDocumentWrapper);
        xInitialization->initialize(arg);

        cssu::Reference<cssxc::sax::XSAXEventKeeperStatusChangeBroadcaster>
            xSAXEventKeeperStatusChangeBroadcaster(m_xSAXEventKeeper, cssu::UNO_QUERY);
        cssu::Reference< cssxc::sax::XSAXEventKeeperStatusChangeListener >
            xStatusChangeListener = this;

        xSAXEventKeeperStatusChangeBroadcaster
            ->addSAXEventKeeperStatusChangeListener( xStatusChangeListener );

        m_nStatusOfSecurityComponents = INITIALIZED;
    }
}

bool XSecController::chainOn( bool bRetrievingLastEvent )
/****** XSecController/chainOn ************************************************
 *
 *   NAME
 *  chainOn -- tries to connect the SAXEventKeeper with the SAX chain.
 *
 *   SYNOPSIS
 *  bJustChainingOn = chainOn( bRetrievingLastEvent );
 *
 *   FUNCTION
 *  First, checks whether the SAXEventKeeper is on the SAX chain. If not,
 *  creates xml security components, and chains the SAXEventKeeper into
 *  the SAX chain.
 *  Before being chained in, the SAXEventKeeper needs to receive all
 *  missed key SAX events, which can promise the DOM tree bufferred by the
 *  SAXEventKeeper has the same structure with the original document.
 *
 *   INPUTS
 *  bRetrievingLastEvent - whether to retrieve the last key SAX event from
 *                         the ElementStackKeeper.
 *
 *   RESULT
 *  bJustChainingOn - whether the SAXEventKeeper is just chained into the
 *                    SAX chain.
 *
 *   NOTES
 *  Sometimes, the last key SAX event can't be transferred to the
 *  SAXEventKeeper together.
 *  For instance, at the time an referenced element is detected, the
 *  startElement event has already been reserved by the ElementStackKeeper.
 *  Meanwhile, an ElementCollector needs to be created before the
 *  SAXEventKeeper receives that startElement event.
 *  So for the SAXEventKeeper, it needs to receive all missed key SAX
 *  events except that startElement event, then adds a new
 *  ElementCollector, then receives that startElement event.
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    bool rc = false;

    if (!m_bIsSAXEventKeeperSticky && !m_bIsSAXEventKeeperConnected)
    {
        if ( m_nStatusOfSecurityComponents == UNINITIALIZED )
        {
            createXSecComponent();
        }

        if ( m_nStatusOfSecurityComponents == INITIALIZED )
        /*
         * if all security components are ready, chains on the SAXEventKeeper
         */
        {
            /*
             * disconnect the SAXEventKeeper with its current output handler,
             * to make sure no SAX event is forwarded during the connecting
             * phase.
             */
            m_xSAXEventKeeper->setNextHandler( nullptr );

            cssu::Reference< cssxs::XDocumentHandler > xSEKHandler(m_xSAXEventKeeper, cssu::UNO_QUERY);

            /*
             * connects the previous document handler on the SAX chain
             */
            if ( m_xPreviousNodeOnSAXChain.is() )
            {
                if ( m_bIsPreviousNodeInitializable )
                {
                    cssu::Reference< cssl::XInitialization > xInitialization
                        (m_xPreviousNodeOnSAXChain, cssu::UNO_QUERY);

                    cssu::Sequence<cssu::Any> aArgs( 1 );
                    aArgs[0] <<= xSEKHandler;
                    xInitialization->initialize(aArgs);
                }
                else
                {
                    cssu::Reference< cssxs::XParser > xParser
                        (m_xPreviousNodeOnSAXChain, cssu::UNO_QUERY);
                    xParser->setDocumentHandler( xSEKHandler );
                }
            }

            /*
             * get missed key SAX events
             */
            if (m_xElementStackKeeper.is())
            {
                m_xElementStackKeeper->retrieve(xSEKHandler, bRetrievingLastEvent);

                /*
                 * now the ElementStackKeeper can stop its work, because the
                 * SAXEventKeeper is on the SAX chain, no SAX events will be
                 * missed.
                 */
                m_xElementStackKeeper->stop();
            }

            /*
             * connects the next document handler on the SAX chain
             */
            m_xSAXEventKeeper->setNextHandler( m_xNextNodeOnSAXChain );

            m_bIsSAXEventKeeperConnected = true;

            rc = true;
        }
    }

    return rc;
}

void XSecController::chainOff()
/****** XSecController/chainOff ***********************************************
 *
 *   NAME
 *  chainOff -- disconnects the SAXEventKeeper from the SAX chain.
 *
 *   SYNOPSIS
 *  chainOff( );
 *
 *   FUNCTION
 *  See NAME.
 *
 *   INPUTS
 *  empty
 *
 *   RESULT
 *  empty
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    if (!m_bIsSAXEventKeeperSticky )
    {
        if (m_bIsSAXEventKeeperConnected)
        {
            m_xSAXEventKeeper->setNextHandler( nullptr );

            if ( m_xPreviousNodeOnSAXChain.is() )
            {
                if ( m_bIsPreviousNodeInitializable )
                {
                    cssu::Reference< cssl::XInitialization > xInitialization
                        (m_xPreviousNodeOnSAXChain, cssu::UNO_QUERY);

                    cssu::Sequence<cssu::Any> aArgs( 1 );
                    aArgs[0] <<= m_xNextNodeOnSAXChain;
                    xInitialization->initialize(aArgs);
                }
                else
                {
                    cssu::Reference< cssxs::XParser > xParser(m_xPreviousNodeOnSAXChain, cssu::UNO_QUERY);
                    xParser->setDocumentHandler( m_xNextNodeOnSAXChain );
                }
            }

            if (m_xElementStackKeeper.is())
            {
                /*
                 * start the ElementStackKeeper to reserve any possible
                 * missed key SAX events
                 */
                m_xElementStackKeeper->start();
            }

            m_bIsSAXEventKeeperConnected = false;
        }
    }
}

void XSecController::checkChainingStatus()
/****** XSecController/checkChainingStatus ************************************
 *
 *   NAME
 *  checkChainingStatus -- connects or disconnects the SAXEventKeeper
 *  according to the current situation.
 *
 *   SYNOPSIS
 *  checkChainingStatus( );
 *
 *   FUNCTION
 *  The SAXEventKeeper is chained into the SAX chain, when:
 *  1. some element is being collected, or
 *  2. the SAX event stream is blocking.
 *  Otherwise, chain off the SAXEventKeeper.
 *
 *   INPUTS
 *  empty
 *
 *   RESULT
 *  empty
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    if ( m_bIsCollectingElement || m_bIsBlocking )
    {
        chainOn(true);
    }
    else
    {
        chainOff();
    }
}

void XSecController::initializeSAXChain()
/****** XSecController/initializeSAXChain *************************************
 *
 *   NAME
 *  initializeSAXChain -- initializes the SAX chain according to the
 *  current setting.
 *
 *   SYNOPSIS
 *  initializeSAXChain( );
 *
 *   FUNCTION
 *  Initializes the SAX chain, if the SAXEventKeeper is asked to be always
 *  on the SAX chain, chains it on. Otherwise, starts the
 *  ElementStackKeeper to reserve key SAX events.
 *
 *   INPUTS
 *  empty
 *
 *   RESULT
 *  empty
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    m_bIsSAXEventKeeperConnected = false;
    m_bIsCollectingElement = false;
    m_bIsBlocking = false;

    if (m_xElementStackKeeper.is())
    {
        /*
         * starts the ElementStackKeeper
         */
        m_xElementStackKeeper->start();
    }

    chainOff();
}

cssu::Reference< com::sun::star::io::XInputStream >
    XSecController::getObjectInputStream( const OUString& objectURL )
/****** XSecController/getObjectInputStream ************************************
 *
 *   NAME
 *  getObjectInputStream -- get a XInputStream interface from a SotStorage
 *
 *   SYNOPSIS
 *  xInputStream = getObjectInputStream( objectURL );
 *
 *   FUNCTION
 *  See NAME.
 *
 *   INPUTS
 *  objectURL - the object uri
 *
 *   RESULT
 *  xInputStream - the XInputStream interface
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
        cssu::Reference< com::sun::star::io::XInputStream > xObjectInputStream;

    DBG_ASSERT( m_xUriBinding.is(), "Need XUriBinding!" );

    xObjectInputStream = m_xUriBinding->getUriBinding(objectURL);

    return xObjectInputStream;
}

/*
 * public methods
 */

sal_Int32 XSecController::getNewSecurityId(  )
{
    sal_Int32 nId = m_nNextSecurityId;
    m_nNextSecurityId++;
    return nId;
}

void XSecController::startMission(
    const cssu::Reference< cssxc::XUriBinding >& xUriBinding,
    const cssu::Reference< cssxc::XXMLSecurityContext >& xSecurityContext )
/****** XSecController/startMission *******************************************
 *
 *   NAME
 *  startMission -- starts a new security mission.
 *
 *   SYNOPSIS
 *  startMission( xUriBinding, xSecurityContect );
 *
 *   FUNCTION
 *  get ready for a new mission.
 *
 *   INPUTS
 *  xUriBinding       - the Uri binding that provide maps between uris and
 *                          XInputStreams
 *  xSecurityContext  - the security context component which can provide
 *                      cryptoken
 *
 *   RESULT
 *  empty
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    m_xUriBinding = xUriBinding;

    m_nStatusOfSecurityComponents = UNINITIALIZED;
    m_xSecurityContext = xSecurityContext;
    m_pErrorMessage = nullptr;

    m_vInternalSignatureInformations.clear();

    m_bVerifyCurrentSignature = false;
}

void XSecController::setSAXChainConnector(
    const cssu::Reference< cssl::XInitialization >& xInitialization,
    const cssu::Reference< cssxs::XDocumentHandler >& xDocumentHandler,
    const cssu::Reference< cssxc::sax::XElementStackKeeper >& xElementStackKeeper)
/****** XSecController/setSAXChainConnector ***********************************
 *
 *   NAME
 *  setSAXChainConnector -- configures the components which will
 *  collaborate with the SAXEventKeeper on the SAX chain.
 *
 *   SYNOPSIS
 *  setSAXChainConnector( xInitialization,
 *                        xDocumentHandler,
 *                        xElementStackKeeper );
 *
 *   FUNCTION
 *  See NAME.
 *
 *   INPUTS
 *  xInitialization     - the previous node on the SAX chain
 *  xDocumentHandler    - the next node on the SAX chain
 *  xElementStackKeeper - the ElementStackKeeper component which reserves
 *                        missed key SAX events for the SAXEventKeeper
 *
 *   RESULT
 *  empty
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    m_bIsPreviousNodeInitializable = true;
    m_xPreviousNodeOnSAXChain = xInitialization;
    m_xNextNodeOnSAXChain = xDocumentHandler;
    m_xElementStackKeeper = xElementStackKeeper;

    initializeSAXChain( );
}

void XSecController::clearSAXChainConnector()
/****** XSecController/clearSAXChainConnector *********************************
 *
 *   NAME
 *  clearSAXChainConnector -- resets the collaborating components.
 *
 *   SYNOPSIS
 *  clearSAXChainConnector( );
 *
 *   FUNCTION
 *  See NAME.
 *
 *   INPUTS
 *  empty
 *
 *   RESULT
 *  empty
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    /*
     * before reseting, if the ElementStackKeeper has kept something, then
     * those kept key SAX events must be transferred to the SAXEventKeeper
     * first. This is to promise the next node to the SAXEventKeeper on the
     * SAX chain always receives a complete document.
     */
    if (m_xElementStackKeeper.is() && m_xSAXEventKeeper.is())
    {
        cssu::Reference< cssxs::XDocumentHandler > xSEKHandler(m_xSAXEventKeeper, cssu::UNO_QUERY);
        m_xElementStackKeeper->retrieve(xSEKHandler, sal_True);
    }

    chainOff();

    m_xPreviousNodeOnSAXChain = nullptr;
    m_xNextNodeOnSAXChain = nullptr;
    m_xElementStackKeeper = nullptr;
}

void XSecController::endMission()
/****** XSecController/endMission *********************************************
 *
 *   NAME
 *  endMission -- forces to end all missions
 *
 *   SYNOPSIS
 *  endMission( );
 *
 *   FUNCTION
 *  Deletes all signature information and forces all missions to an end.
 *
 *   INPUTS
 *  empty
 *
 *   RESULT
 *  empty
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    sal_Int32 size = m_vInternalSignatureInformations.size();

    for (int i=0; i<size; ++i)
    {
        if ( m_nStatusOfSecurityComponents == INITIALIZED )
        /*
         * ResolvedListener only exist when the security components are created.
         */
        {
            cssu::Reference< cssxc::sax::XMissionTaker > xMissionTaker
                ( m_vInternalSignatureInformations[i].xReferenceResolvedListener, cssu::UNO_QUERY );

            /*
             * asks the SignatureCreator/SignatureVerifier to release
             * all resources it uses.
             */
            xMissionTaker->endMission();
        }
    }

    m_xUriBinding = nullptr;
    m_xSecurityContext = nullptr;

    /*
     * free the status change listener reference to this object
     */
    if (m_xSAXEventKeeper.is())
    {
        cssu::Reference<cssxc::sax::XSAXEventKeeperStatusChangeBroadcaster>
            xSAXEventKeeperStatusChangeBroadcaster(m_xSAXEventKeeper, cssu::UNO_QUERY);
        xSAXEventKeeperStatusChangeBroadcaster
            ->addSAXEventKeeperStatusChangeListener( nullptr );
    }
}

void XSecController::exportSignature(
    const cssu::Reference<cssxs::XDocumentHandler>& xDocumentHandler,
    const SignatureInformation& signatureInfo )
/****** XSecController/exportSignature ****************************************
 *
 *   NAME
 *  exportSignature -- export a signature structure to an XDocumentHandler
 *
 *   SYNOPSIS
 *  exportSignature( xDocumentHandler, signatureInfo);
 *
 *   FUNCTION
 *  see NAME.
 *
 *   INPUTS
 *  xDocumentHandler    - the document handler to receive the signature
 *  signatureInfo       - signature to be exported
 *
 *   RESULT
 *  empty
 *
 *   AUTHOR
 *  Michael Mi
 *  Email: michael.mi@sun.com
 ******************************************************************************/
{
    /*
     * defines all element tags in Signature element.
     */
    OUString tag_Signature(TAG_SIGNATURE);
    OUString tag_SignedInfo(TAG_SIGNEDINFO);
    OUString tag_CanonicalizationMethod(TAG_CANONICALIZATIONMETHOD);
    OUString tag_SignatureMethod(TAG_SIGNATUREMETHOD);
    OUString tag_Reference(TAG_REFERENCE);
    OUString tag_Transforms(TAG_TRANSFORMS);
    OUString tag_Transform(TAG_TRANSFORM);
    OUString tag_DigestMethod(TAG_DIGESTMETHOD);
    OUString tag_DigestValue(TAG_DIGESTVALUE);
    OUString tag_SignatureValue(TAG_SIGNATUREVALUE);
    OUString tag_KeyInfo(TAG_KEYINFO);
    OUString tag_X509Data(TAG_X509DATA);
    OUString tag_X509IssuerSerial(TAG_X509ISSUERSERIAL);
    OUString tag_X509IssuerName(TAG_X509ISSUERNAME);
    OUString tag_X509SerialNumber(TAG_X509SERIALNUMBER);
    OUString tag_X509Certificate(TAG_X509CERTIFICATE);
    OUString tag_Object(TAG_OBJECT);
    OUString tag_SignatureProperties(TAG_SIGNATUREPROPERTIES);
    OUString tag_SignatureProperty(TAG_SIGNATUREPROPERTY);
    OUString tag_Date(TAG_DATE);
    OUString tag_Description(TAG_DESCRIPTION);

    const SignatureReferenceInformations& vReferenceInfors = signatureInfo.vSignatureReferenceInfors;
    SvXMLAttributeList *pAttributeList;

    /*
     * Write Signature element
     */
    pAttributeList = new SvXMLAttributeList();
    pAttributeList->AddAttribute(
        ATTR_XMLNS,
        NS_XMLDSIG);

    if (!signatureInfo.ouSignatureId.isEmpty())
    {
        pAttributeList->AddAttribute(
            ATTR_ID,
            OUString(signatureInfo.ouSignatureId));
    }

    xDocumentHandler->startElement( tag_Signature, cssu::Reference< cssxs::XAttributeList > (pAttributeList));
    {
        /* Write SignedInfo element */
        xDocumentHandler->startElement(
            tag_SignedInfo,
            cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
        {
            /* Write CanonicalizationMethod element */
            pAttributeList = new SvXMLAttributeList();
            pAttributeList->AddAttribute(
                ATTR_ALGORITHM,
                ALGO_C14N);
            xDocumentHandler->startElement( tag_CanonicalizationMethod, cssu::Reference< cssxs::XAttributeList > (pAttributeList) );
            xDocumentHandler->endElement( tag_CanonicalizationMethod );

            /* Write SignatureMethod element */
            pAttributeList = new SvXMLAttributeList();
            pAttributeList->AddAttribute(
                ATTR_ALGORITHM,
                ALGO_RSASHA1);
            xDocumentHandler->startElement( tag_SignatureMethod, cssu::Reference< cssxs::XAttributeList > (pAttributeList) );
            xDocumentHandler->endElement( tag_SignatureMethod );

            /* Write Reference element */
            int j;
            int refNum = vReferenceInfors.size();

            for(j=0; j<refNum; ++j)
            {
                const SignatureReferenceInformation& refInfor = vReferenceInfors[j];

                pAttributeList = new SvXMLAttributeList();
                if ( refInfor.nType != SignatureReferenceType::SAMEDOCUMENT )
                /*
                 * stream reference
                 */
                {
                    pAttributeList->AddAttribute(
                        ATTR_URI,
                        refInfor.ouURI);
                }
                else
                /*
                 * same-document reference
                 */
                {
                    pAttributeList->AddAttribute(
                        ATTR_URI,
                        CHAR_FRAGMENT+refInfor.ouURI);
                }

                xDocumentHandler->startElement( tag_Reference, cssu::Reference< cssxs::XAttributeList > (pAttributeList) );
                {
                    /* Write Transforms element */
                    if (refInfor.nType == SignatureReferenceType::XMLSTREAM)
                    /*
                     * xml stream, so c14n transform is needed
                     */
                    {
                        xDocumentHandler->startElement(
                            tag_Transforms,
                            cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
                        {
                            pAttributeList = new SvXMLAttributeList();
                            pAttributeList->AddAttribute(
                                ATTR_ALGORITHM,
                                ALGO_C14N);
                            xDocumentHandler->startElement(
                                tag_Transform,
                                cssu::Reference< cssxs::XAttributeList > (pAttributeList) );
                            xDocumentHandler->endElement( tag_Transform );
                        }
                        xDocumentHandler->endElement( tag_Transforms );
                    }

                    /* Write DigestMethod element */
                    pAttributeList = new SvXMLAttributeList();
                    pAttributeList->AddAttribute(
                        ATTR_ALGORITHM,
                        ALGO_XMLDSIGSHA1);
                    xDocumentHandler->startElement(
                        tag_DigestMethod,
                        cssu::Reference< cssxs::XAttributeList > (pAttributeList) );
                    xDocumentHandler->endElement( tag_DigestMethod );

                    /* Write DigestValue element */
                    xDocumentHandler->startElement(
                        tag_DigestValue,
                        cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
                    xDocumentHandler->characters( refInfor.ouDigestValue );
                    xDocumentHandler->endElement( tag_DigestValue );
                }
                xDocumentHandler->endElement( tag_Reference );
            }
        }
        xDocumentHandler->endElement( tag_SignedInfo );

        /* Write SignatureValue element */
        xDocumentHandler->startElement(
            tag_SignatureValue,
            cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
        xDocumentHandler->characters( signatureInfo.ouSignatureValue );
        xDocumentHandler->endElement( tag_SignatureValue );

        /* Write KeyInfo element */
        xDocumentHandler->startElement(
            tag_KeyInfo,
            cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
        {
            /* Write X509Data element */
            xDocumentHandler->startElement(
                tag_X509Data,
                cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
            {
                /* Write X509IssuerSerial element */
                xDocumentHandler->startElement(
                    tag_X509IssuerSerial,
                    cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
                {
                    /* Write X509IssuerName element */
                    xDocumentHandler->startElement(
                        tag_X509IssuerName,
                        cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
                    xDocumentHandler->characters( signatureInfo.ouX509IssuerName );
                    xDocumentHandler->endElement( tag_X509IssuerName );

                    /* Write X509SerialNumber element */
                    xDocumentHandler->startElement(
                        tag_X509SerialNumber,
                        cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
                    xDocumentHandler->characters( signatureInfo.ouX509SerialNumber );
                    xDocumentHandler->endElement( tag_X509SerialNumber );
                }
                xDocumentHandler->endElement( tag_X509IssuerSerial );

                /* Write X509Certificate element */
                if (!signatureInfo.ouX509Certificate.isEmpty())
                {
                    xDocumentHandler->startElement(
                        tag_X509Certificate,
                        cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
                    xDocumentHandler->characters( signatureInfo.ouX509Certificate );
                    xDocumentHandler->endElement( tag_X509Certificate );
                }
            }
            xDocumentHandler->endElement( tag_X509Data );
        }
        xDocumentHandler->endElement( tag_KeyInfo );

        /* Write Object element */
        xDocumentHandler->startElement(
            tag_Object,
            cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
        {
            /* Write SignatureProperties element */
            xDocumentHandler->startElement(
                tag_SignatureProperties,
                cssu::Reference< cssxs::XAttributeList > (new SvXMLAttributeList()));
            {
                /* Write SignatureProperty element */
                pAttributeList = new SvXMLAttributeList();
                pAttributeList->AddAttribute(
                    ATTR_ID,
                    signatureInfo.ouPropertyId);
                pAttributeList->AddAttribute(
                    ATTR_TARGET,
                    CHAR_FRAGMENT+signatureInfo.ouSignatureId);
                xDocumentHandler->startElement(
                    tag_SignatureProperty,
                    cssu::Reference< cssxs::XAttributeList > (pAttributeList));
                {
                    /* Write timestamp element */

                    pAttributeList = new SvXMLAttributeList();
                    pAttributeList->AddAttribute(
                        ATTR_XMLNS ":" NSTAG_DC,
                        NS_DC);

                    xDocumentHandler->startElement(
                        NSTAG_DC ":" + tag_Date,
                        cssu::Reference< cssxs::XAttributeList > (pAttributeList));

                    OUStringBuffer buffer;
                    //If the xml signature was already contained in the document,
                    //then we use the original date and time string, rather then the
                    //converted one. This avoids writing a different string due to
                    //e.g. rounding issues and thus breaking the signature.
                    if (!signatureInfo.ouDateTime.isEmpty())
                        buffer = signatureInfo.ouDateTime;
                    else
                    {
                        buffer = utl::toISO8601(signatureInfo.stDateTime);
                    }
                    xDocumentHandler->characters( buffer.makeStringAndClear() );

                    xDocumentHandler->endElement(
                        NSTAG_DC ":" + tag_Date);
                }
                xDocumentHandler->endElement( tag_SignatureProperty );
            }

            // Write signature description.
            if (!signatureInfo.ouDescription.isEmpty())
            {
                // SignatureProperty element.
                pAttributeList = new SvXMLAttributeList();
                pAttributeList->AddAttribute(ATTR_ID, signatureInfo.ouDescriptionPropertyId);
                pAttributeList->AddAttribute(ATTR_TARGET, CHAR_FRAGMENT + signatureInfo.ouSignatureId);
                xDocumentHandler->startElement(tag_SignatureProperty, uno::Reference<xml::sax::XAttributeList>(pAttributeList));

                {
                    // Description element.
                    pAttributeList = new SvXMLAttributeList();
                    pAttributeList->AddAttribute(ATTR_XMLNS ":" NSTAG_DC, NS_DC);

                    xDocumentHandler->startElement(NSTAG_DC ":" + tag_Description, uno::Reference<xml::sax::XAttributeList>(pAttributeList));
                    xDocumentHandler->characters(signatureInfo.ouDescription);
                    xDocumentHandler->endElement(NSTAG_DC ":" + tag_Description);
                }

                xDocumentHandler->endElement(tag_SignatureProperty);
            }

            xDocumentHandler->endElement( tag_SignatureProperties );
        }
        xDocumentHandler->endElement( tag_Object );
    }
    xDocumentHandler->endElement( tag_Signature );
}

/// Should we intentionally not sign this stream?
static bool lcl_isOOXMLBlacklist(const OUString& rStreamName)
{
#if !HAVE_BROKEN_STATIC_INITILIZER_LIST
    static
#endif
    const std::initializer_list<OUStringLiteral> vBlacklist =
    {
        OUStringLiteral("/%5BContent_Types%5D.xml"),
        OUStringLiteral("/docProps/app.xml"),
        OUStringLiteral("/docProps/core.xml"),
        // Don't attempt to sign other signatures for now.
        OUStringLiteral("/_xmlsignatures")
    };
    // Just check the prefix, as we don't care about the content type part of the stream name.
    return std::find_if(vBlacklist.begin(), vBlacklist.end(), [&](const OUStringLiteral& rLiteral) { return rStreamName.startsWith(rLiteral); }) != vBlacklist.end();
}

/// Should we intentionally not sign this relation type?
static bool lcl_isOOXMLRelationBlacklist(const OUString& rRelationName)
{
#if !HAVE_BROKEN_STATIC_INITILIZER_LIST
    static
#endif
    const std::initializer_list<OUStringLiteral> vBlacklist =
    {
        OUStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties"),
        OUStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties"),
        OUStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships/digital-signature/origin")
    };
    return std::find(vBlacklist.begin(), vBlacklist.end(), rRelationName) != vBlacklist.end();
}

void XSecController::exportOOXMLSignature(const uno::Reference<embed::XStorage>& xRootStorage, const uno::Reference<xml::sax::XDocumentHandler>& xDocumentHandler, const SignatureInformation& rInformation)
{
    uno::Reference<embed::XHierarchicalStorageAccess> xHierarchicalStorageAccess(xRootStorage, uno::UNO_QUERY);

    {
        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
        pAttributeList->AddAttribute(ATTR_XMLNS, NS_XMLDSIG);
        pAttributeList->AddAttribute(ATTR_ID, "idPackageSignature");
        xDocumentHandler->startElement(TAG_SIGNATURE, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
    }
    xDocumentHandler->startElement(TAG_SIGNEDINFO, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));

    {
        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
        pAttributeList->AddAttribute(ATTR_ALGORITHM, ALGO_C14N);
        xDocumentHandler->startElement(TAG_CANONICALIZATIONMETHOD, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
        xDocumentHandler->endElement(TAG_CANONICALIZATIONMETHOD);
    }

    {
        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
        pAttributeList->AddAttribute(ATTR_ALGORITHM, ALGO_RSASHA256);
        xDocumentHandler->startElement(TAG_SIGNATUREMETHOD, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
        xDocumentHandler->endElement(TAG_SIGNATUREMETHOD);
    }

    const SignatureReferenceInformations& rReferences = rInformation.vSignatureReferenceInfors;
    for (const SignatureReferenceInformation& rReference : rReferences)
    {
        if (rReference.nType == SignatureReferenceType::SAMEDOCUMENT)
        {
            {
                rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
                if (rReference.ouURI != "idSignedProperties")
                    pAttributeList->AddAttribute("Type", "http://www.w3.org/2000/09/xmldsig#Object");
                else
                    pAttributeList->AddAttribute("Type", "http://uri.etsi.org/01903#SignedProperties");
                pAttributeList->AddAttribute(ATTR_URI, CHAR_FRAGMENT + rReference.ouURI);
                xDocumentHandler->startElement(TAG_REFERENCE, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
            }
            if (rReference.ouURI == "idSignedProperties")
            {
                xDocumentHandler->startElement(TAG_TRANSFORMS, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
                rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
                pAttributeList->AddAttribute(ATTR_ALGORITHM, ALGO_C14N);
                xDocumentHandler->startElement(TAG_TRANSFORM, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
                xDocumentHandler->endElement(TAG_TRANSFORM);
                xDocumentHandler->endElement(TAG_TRANSFORMS);
            }

            {
                rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
                pAttributeList->AddAttribute(ATTR_ALGORITHM, ALGO_XMLDSIGSHA256);
                xDocumentHandler->startElement(TAG_DIGESTMETHOD, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
                xDocumentHandler->endElement(TAG_DIGESTMETHOD);
            }
            xDocumentHandler->startElement(TAG_DIGESTVALUE, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
            xDocumentHandler->characters(rReference.ouDigestValue);
            xDocumentHandler->endElement(TAG_DIGESTVALUE);
            xDocumentHandler->endElement(TAG_REFERENCE);
        }
    }

    xDocumentHandler->endElement(TAG_SIGNEDINFO);

    xDocumentHandler->startElement(TAG_SIGNATUREVALUE, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters(rInformation.ouSignatureValue);
    xDocumentHandler->endElement(TAG_SIGNATUREVALUE);

    xDocumentHandler->startElement(TAG_KEYINFO, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->startElement(TAG_X509DATA, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->startElement(TAG_X509CERTIFICATE, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters(rInformation.ouX509Certificate);
    xDocumentHandler->endElement(TAG_X509CERTIFICATE);
    xDocumentHandler->endElement(TAG_X509DATA);
    xDocumentHandler->endElement(TAG_KEYINFO);

    {
        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
        pAttributeList->AddAttribute(ATTR_ID, "idPackageObject");
        xDocumentHandler->startElement(TAG_OBJECT, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
    }
    xDocumentHandler->startElement(TAG_MANIFEST, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    for (const SignatureReferenceInformation& rReference : rReferences)
    {
        if (rReference.nType != SignatureReferenceType::SAMEDOCUMENT)
        {
            if (lcl_isOOXMLBlacklist(rReference.ouURI))
                continue;

            {
                rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
                pAttributeList->AddAttribute(ATTR_URI, rReference.ouURI);
                xDocumentHandler->startElement(TAG_REFERENCE, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
            }

            // Transforms
            if (rReference.ouURI.endsWith("?ContentType=application/vnd.openxmlformats-package.relationships+xml"))
            {
                OUString aURI = rReference.ouURI;
                // Ignore leading slash.
                if (aURI.startsWith("/"))
                    aURI = aURI.copy(1);
                // Ignore query part of the URI.
                sal_Int32 nQueryPos = aURI.indexOf('?');
                if (nQueryPos != -1)
                    aURI = aURI.copy(0, nQueryPos);

                uno::Reference<io::XInputStream> xRelStream(xHierarchicalStorageAccess->openStreamElementByHierarchicalName(aURI, embed::ElementModes::READ), uno::UNO_QUERY);
                xDocumentHandler->startElement(TAG_TRANSFORMS, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
                {
                    rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
                    pAttributeList->AddAttribute(ATTR_ALGORITHM, ALGO_RELATIONSHIP);
                    xDocumentHandler->startElement(TAG_TRANSFORM, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
                }

                uno::Sequence< uno::Sequence<beans::StringPair> > aRelationsInfo = comphelper::OFOPXMLHelper::ReadRelationsInfoSequence(xRelStream, aURI, mxCtx);
                for (const uno::Sequence<beans::StringPair>& rPairs : aRelationsInfo)
                {
                    OUString aId;
                    OUString aType;
                    for (const beans::StringPair& rPair : rPairs)
                    {
                        if (rPair.First == "Id")
                            aId = rPair.Second;
                        else if (rPair.First == "Type")
                            aType = rPair.Second;
                    }

                    if (lcl_isOOXMLRelationBlacklist(aType))
                        continue;

                    {
                        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
                        pAttributeList->AddAttribute(ATTR_XMLNS ":" NSTAG_MDSSI, NS_MDSSI);
                        pAttributeList->AddAttribute(ATTR_SOURCEID, aId);
                        xDocumentHandler->startElement(NSTAG_MDSSI ":" TAG_RELATIONSHIPREFERENCE, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
                    }
                    xDocumentHandler->endElement(NSTAG_MDSSI ":" TAG_RELATIONSHIPREFERENCE);
                }

                xDocumentHandler->endElement(TAG_TRANSFORM);
                {
                    rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
                    pAttributeList->AddAttribute(ATTR_ALGORITHM, ALGO_C14N);
                    xDocumentHandler->startElement(TAG_TRANSFORM, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
                }
                xDocumentHandler->endElement(TAG_TRANSFORM);
                xDocumentHandler->endElement(TAG_TRANSFORMS);
            }

            {
                rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
                pAttributeList->AddAttribute(ATTR_ALGORITHM, ALGO_XMLDSIGSHA256);
                xDocumentHandler->startElement(TAG_DIGESTMETHOD, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
                xDocumentHandler->endElement(TAG_DIGESTMETHOD);
            }
            xDocumentHandler->startElement(TAG_DIGESTVALUE, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
            xDocumentHandler->characters(rReference.ouDigestValue);
            xDocumentHandler->endElement(TAG_DIGESTVALUE);
            xDocumentHandler->endElement(TAG_REFERENCE);
        }
    }
    xDocumentHandler->endElement(TAG_MANIFEST);

    // SignatureProperties
    xDocumentHandler->startElement(TAG_SIGNATUREPROPERTIES, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    {
        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
        pAttributeList->AddAttribute(ATTR_ID, "idSignatureTime");
        pAttributeList->AddAttribute(ATTR_TARGET, "#idPackageSignature");
        xDocumentHandler->startElement(TAG_SIGNATUREPROPERTY, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
    }
    {
        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
        pAttributeList->AddAttribute(ATTR_XMLNS ":" NSTAG_MDSSI, NS_MDSSI);
        xDocumentHandler->startElement(NSTAG_MDSSI ":" TAG_SIGNATURETIME, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
    }
    xDocumentHandler->startElement(NSTAG_MDSSI ":" TAG_FORMAT, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("YYYY-MM-DDThh:mm:ssTZD");
    xDocumentHandler->endElement(NSTAG_MDSSI ":" TAG_FORMAT);

    xDocumentHandler->startElement(NSTAG_MDSSI ":" TAG_VALUE, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    OUString aSignatureTimeValue;
    if (!rInformation.ouDateTime.isEmpty())
        aSignatureTimeValue = rInformation.ouDateTime;
    else
    {
        aSignatureTimeValue = utl::toISO8601(rInformation.stDateTime);
        // Ignore sub-seconds.
        sal_Int32 nCommaPos = aSignatureTimeValue.indexOf(',');
        if (nCommaPos != -1)
        {
            aSignatureTimeValue = aSignatureTimeValue.copy(0, nCommaPos);
            aSignatureTimeValue += "Z";
        }
    }
    xDocumentHandler->characters(aSignatureTimeValue);
    xDocumentHandler->endElement(NSTAG_MDSSI ":" TAG_VALUE);

    xDocumentHandler->endElement(NSTAG_MDSSI ":" TAG_SIGNATURETIME);
    xDocumentHandler->endElement(TAG_SIGNATUREPROPERTY);
    xDocumentHandler->endElement(TAG_SIGNATUREPROPERTIES);

    xDocumentHandler->endElement(TAG_OBJECT);

    // idOfficeObject
    {
        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
        pAttributeList->AddAttribute(ATTR_ID, "idOfficeObject");
        xDocumentHandler->startElement(TAG_OBJECT, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
    }
    xDocumentHandler->startElement(TAG_SIGNATUREPROPERTIES, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    {
        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
        pAttributeList->AddAttribute(ATTR_ID, "idOfficeV1Details");
        pAttributeList->AddAttribute(ATTR_TARGET, "#idPackageSignature");
        xDocumentHandler->startElement(TAG_SIGNATUREPROPERTY, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
    }
    {
        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
        pAttributeList->AddAttribute(ATTR_XMLNS, "http://schemas.microsoft.com/office/2006/digsig");
        xDocumentHandler->startElement("SignatureInfoV1", uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
    }
    xDocumentHandler->startElement("SetupId", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->endElement("SetupId");
    xDocumentHandler->startElement("SignatureText", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->endElement("SignatureText");
    xDocumentHandler->startElement("SignatureImage", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->endElement("SignatureImage");
    xDocumentHandler->startElement("SignatureComments", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters(rInformation.ouDescription);
    xDocumentHandler->endElement("SignatureComments");
    // Just hardcode something valid according to [MS-OFFCRYPTO].
    xDocumentHandler->startElement("WindowsVersion", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("6.1");
    xDocumentHandler->endElement("WindowsVersion");
    xDocumentHandler->startElement("OfficeVersion", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("16.0");
    xDocumentHandler->endElement("OfficeVersion");
    xDocumentHandler->startElement("ApplicationVersion", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("16.0");
    xDocumentHandler->endElement("ApplicationVersion");
    xDocumentHandler->startElement("Monitors", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("1");
    xDocumentHandler->endElement("Monitors");
    xDocumentHandler->startElement("HorizontalResolution", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("1280");
    xDocumentHandler->endElement("HorizontalResolution");
    xDocumentHandler->startElement("VerticalResolution", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("800");
    xDocumentHandler->endElement("VerticalResolution");
    xDocumentHandler->startElement("ColorDepth", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("32");
    xDocumentHandler->endElement("ColorDepth");
    xDocumentHandler->startElement("SignatureProviderId", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("{00000000-0000-0000-0000-000000000000}");
    xDocumentHandler->endElement("SignatureProviderId");
    xDocumentHandler->startElement("SignatureProviderUrl", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->endElement("SignatureProviderUrl");
    xDocumentHandler->startElement("SignatureProviderDetails", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("9"); // This is what MSO 2016 writes, though [MS-OFFCRYPTO] doesn't document what the value means.
    xDocumentHandler->endElement("SignatureProviderDetails");
    xDocumentHandler->startElement("SignatureType", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    xDocumentHandler->characters("1");
    xDocumentHandler->endElement("SignatureType");
    xDocumentHandler->endElement("SignatureInfoV1");
    xDocumentHandler->endElement(TAG_SIGNATUREPROPERTY);
    xDocumentHandler->endElement(TAG_SIGNATUREPROPERTIES);
    xDocumentHandler->endElement(TAG_OBJECT);

    xDocumentHandler->startElement(TAG_OBJECT, uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
    {
        rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
        pAttributeList->AddAttribute(ATTR_XMLNS ":" NSTAG_XD, NS_XD);
        pAttributeList->AddAttribute(ATTR_TARGET, "#idPackageSignature");
        xDocumentHandler->startElement(NSTAG_XD ":" TAG_QUALIFYINGPROPERTIES, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
    }

    // FIXME why does this part crash NSS when MOZILLA_CERTIFICATE_FOLDER is not set?
    static bool bTest = getenv("LO_TESTNAME");
    if (!bTest)
    {
        {
            rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
            pAttributeList->AddAttribute(ATTR_ID, "idSignedProperties");
            xDocumentHandler->startElement(NSTAG_XD ":" TAG_SIGNEDPROPERTIES, uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
        }

        xDocumentHandler->startElement("xd:SignedSignatureProperties", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
        xDocumentHandler->startElement("xd:SigningTime", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
        xDocumentHandler->characters(aSignatureTimeValue);
        xDocumentHandler->endElement("xd:SigningTime");
        xDocumentHandler->startElement("xd:SigningCertificate", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
        xDocumentHandler->startElement("xd:Cert", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
        xDocumentHandler->startElement("xd:CertDigest", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
        {
            rtl::Reference<SvXMLAttributeList> pAttributeList(new SvXMLAttributeList());
            pAttributeList->AddAttribute(ATTR_ALGORITHM, ALGO_XMLDSIGSHA256);
            xDocumentHandler->startElement("DigestMethod", uno::Reference<xml::sax::XAttributeList>(pAttributeList.get()));
        }
        xDocumentHandler->endElement("DigestMethod");
        xDocumentHandler->startElement("DigestValue", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));

        assert(!rInformation.ouCertDigest.isEmpty());
        xDocumentHandler->characters(rInformation.ouCertDigest);

        xDocumentHandler->endElement("DigestValue");
        xDocumentHandler->endElement("xd:CertDigest");
        xDocumentHandler->startElement("xd:IssuerSerial", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
        xDocumentHandler->startElement("X509IssuerName", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
        xDocumentHandler->characters(rInformation.ouX509IssuerName);
        xDocumentHandler->endElement("X509IssuerName");
        xDocumentHandler->startElement("X509SerialNumber", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
        xDocumentHandler->characters(rInformation.ouX509SerialNumber);
        xDocumentHandler->endElement("X509SerialNumber");
        xDocumentHandler->endElement("xd:IssuerSerial");
        xDocumentHandler->endElement("xd:Cert");
        xDocumentHandler->endElement("xd:SigningCertificate");
        xDocumentHandler->startElement("xd:SignaturePolicyIdentifier", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
        xDocumentHandler->startElement("xd:SignaturePolicyImplied", uno::Reference<xml::sax::XAttributeList>(new SvXMLAttributeList()));
        xDocumentHandler->endElement("xd:SignaturePolicyImplied");
        xDocumentHandler->endElement("xd:SignaturePolicyIdentifier");
        xDocumentHandler->endElement("xd:SignedSignatureProperties");

        xDocumentHandler->endElement(NSTAG_XD ":" TAG_SIGNEDPROPERTIES);
    }
    xDocumentHandler->endElement(NSTAG_XD ":" TAG_QUALIFYINGPROPERTIES);
    xDocumentHandler->endElement(TAG_OBJECT);

    xDocumentHandler->endElement(TAG_SIGNATURE);
}

SignatureInformation XSecController::getSignatureInformation( sal_Int32 nSecurityId ) const
{
    SignatureInformation aInf( 0 );
    int nIndex = findSignatureInfor(nSecurityId);
    DBG_ASSERT( nIndex != -1, "getSignatureInformation - SecurityId is invalid!" );
    if ( nIndex != -1)
    {
        aInf = m_vInternalSignatureInformations[nIndex].signatureInfor;
    }
    return aInf;
}

SignatureInformations XSecController::getSignatureInformations() const
{
    SignatureInformations vInfors;
    int sigNum = m_vInternalSignatureInformations.size();

    for (int i=0; i<sigNum; ++i)
    {
        SignatureInformation si = m_vInternalSignatureInformations[i].signatureInfor;
        vInfors.push_back(si);
    }

    return vInfors;
}

/*
 * XSecurityController
 *
 * no methods
 */

/*
 * XFastPropertySet
 */

/*
 * XSAXEventKeeperStatusChangeListener
 */

void SAL_CALL XSecController::blockingStatusChanged( sal_Bool isBlocking )
    throw (cssu::RuntimeException, std::exception)
{
    this->m_bIsBlocking = isBlocking;
    checkChainingStatus();
}

void SAL_CALL XSecController::collectionStatusChanged(
    sal_Bool isInsideCollectedElement )
    throw (cssu::RuntimeException, std::exception)
{
    this->m_bIsCollectingElement = isInsideCollectedElement;
    checkChainingStatus();
}

void SAL_CALL XSecController::bufferStatusChanged( sal_Bool /*isBufferEmpty*/)
    throw (cssu::RuntimeException, std::exception)
{

}

/*
 * XSignatureCreationResultListener
 */
void SAL_CALL XSecController::signatureCreated( sal_Int32 securityId, com::sun::star::xml::crypto::SecurityOperationStatus nResult )
        throw (com::sun::star::uno::RuntimeException, std::exception)
{
    int index = findSignatureInfor(securityId);
    assert(index != -1 && "Signature Not Found!");
    SignatureInformation& signatureInfor = m_vInternalSignatureInformations.at(index).signatureInfor;
    signatureInfor.nStatus = nResult;
}

/*
 * XSignatureVerifyResultListener
 */
void SAL_CALL XSecController::signatureVerified( sal_Int32 securityId, com::sun::star::xml::crypto::SecurityOperationStatus nResult )
        throw (com::sun::star::uno::RuntimeException, std::exception)
{
    int index = findSignatureInfor(securityId);
    assert(index != -1 && "Signature Not Found!");
    SignatureInformation& signatureInfor = m_vInternalSignatureInformations.at(index).signatureInfor;
    signatureInfor.nStatus = nResult;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
