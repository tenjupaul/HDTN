#include <string.h>
#include <iostream>
#include "app_patterns/BpSourcePattern.h"
#include <boost/lexical_cast.hpp>
#include <memory>
#include <boost/make_unique.hpp>
#include "Uri.h"

#include <time.h>
#include "TimestampUtil.h"
#include "codec/bpv6.h"
#include "TcpclInduct.h"
#include "TcpclV4Induct.h"
#include "codec/BundleViewV7.h"

BpSourcePattern::BpSourcePattern() : m_running(false) {

}

BpSourcePattern::~BpSourcePattern() {
    Stop();
    std::cout << "totalNonAdminRecordBpv6PayloadBytesRx: " << m_totalNonAdminRecordBpv6PayloadBytesRx << "\n";
    std::cout << "totalNonAdminRecordBpv6BundleBytesRx: " << m_totalNonAdminRecordBpv6BundleBytesRx << "\n";
    std::cout << "totalNonAdminRecordBpv6BundlesRx: " << m_totalNonAdminRecordBpv6BundlesRx << "\n";

    std::cout << "totalNonAdminRecordBpv7PayloadBytesRx: " << m_totalNonAdminRecordBpv7PayloadBytesRx << "\n";
    std::cout << "totalNonAdminRecordBpv7BundleBytesRx: " << m_totalNonAdminRecordBpv7BundleBytesRx << "\n";
    std::cout << "totalNonAdminRecordBpv7BundlesRx: " << m_totalNonAdminRecordBpv7BundlesRx << "\n";
    for (std::size_t i = 0; i < m_hopCounts.size(); ++i) {
        if (m_hopCounts[i] != 0) {
            std::cout << "received " << m_hopCounts[i] << " bundles with a hop count of " << i << ".\n";
        }
    }
}

void BpSourcePattern::Stop() {
    m_running = false;
//    boost::this_thread::sleep(boost::posix_time::seconds(1));
    if(m_bpSourcePatternThreadPtr) {
        m_bpSourcePatternThreadPtr->join();
        m_bpSourcePatternThreadPtr.reset(); //delete it
    }

    m_outductManager.StopAllOutducts();
    if (Outduct * outduct = m_outductManager.GetOutductByOutductUuid(0)) {
        outduct->GetOutductFinalStats(m_outductFinalStats);
    }
    
}

void BpSourcePattern::Start(OutductsConfig_ptr & outductsConfigPtr, InductsConfig_ptr & inductsConfigPtr, bool custodyTransferUseAcs,
    const cbhe_eid_t & myEid, uint32_t bundleRate, const cbhe_eid_t & finalDestEid, const uint64_t myCustodianServiceId, const unsigned int bundleSendTimeoutSeconds,
    const bool requireRxBundleBeforeNextTx, const bool forceDisableCustody, const bool useBpVersion7) {
    if (m_running) {
        std::cerr << "error: BpSourcePattern::Start called while BpSourcePattern is already running" << std::endl;
        return;
    }
    m_bundleSendTimeoutSeconds = bundleSendTimeoutSeconds;
    m_finalDestinationEid = finalDestEid;
    m_myEid = myEid;
    m_myCustodianServiceId = myCustodianServiceId;
    m_myCustodianEid.Set(m_myEid.nodeId, myCustodianServiceId);
    m_myCustodianEidUriString = Uri::GetIpnUriString(m_myEid.nodeId, myCustodianServiceId);
    m_detectedNextCustodianSupportsCteb = false;
    m_requireRxBundleBeforeNextTx = requireRxBundleBeforeNextTx;
    m_useBpVersion7 = useBpVersion7;
    m_linkIsDown = false;
    m_nextBundleId = 0;
    m_hopCounts.assign(256, 0);
    m_lastPreviousNode.Set(0, 0);

    m_totalNonAdminRecordBpv6PayloadBytesRx = 0;
    m_totalNonAdminRecordBpv6BundleBytesRx = 0;
    m_totalNonAdminRecordBpv6BundlesRx = 0;

    m_totalNonAdminRecordBpv7PayloadBytesRx = 0;
    m_totalNonAdminRecordBpv7BundleBytesRx = 0;
    m_totalNonAdminRecordBpv7BundlesRx = 0;

    m_tcpclInductPtr = NULL;

    OutductOpportunisticProcessReceivedBundleCallback_t outductOpportunisticProcessReceivedBundleCallback; //"null" function by default
    m_custodyTransferUseAcs = custodyTransferUseAcs;
    if (inductsConfigPtr) {
        m_currentlySendingBundleIdSet.reserve(1024); //todo
        m_useCustodyTransfer = true;
        m_inductManager.LoadInductsFromConfig(boost::bind(&BpSourcePattern::WholeRxBundleReadyCallback, this, boost::placeholders::_1),
            *inductsConfigPtr, m_myEid.nodeId, UINT16_MAX, 1000000, //todo 1MB max bundle size on custody signals
            boost::bind(&BpSourcePattern::OnNewOpportunisticLinkCallback, this, boost::placeholders::_1, boost::placeholders::_2),
            boost::bind(&BpSourcePattern::OnDeletedOpportunisticLinkCallback, this, boost::placeholders::_1));
    }
    else if ((outductsConfigPtr)
        && ((outductsConfigPtr->m_outductElementConfigVector[0].convergenceLayer == "tcpcl_v3") || (outductsConfigPtr->m_outductElementConfigVector[0].convergenceLayer == "tcpcl_v4"))
        && (outductsConfigPtr->m_outductElementConfigVector[0].tcpclAllowOpportunisticReceiveBundles)
        )
    {
        m_useCustodyTransfer = true;
        outductOpportunisticProcessReceivedBundleCallback = boost::bind(&BpSourcePattern::WholeRxBundleReadyCallback, this, boost::placeholders::_1);
        std::cout << "this bpsource pattern detected tcpcl convergence layer which is bidirectional.. supporting custody transfer\n";
    }
    else {
        m_useCustodyTransfer = false;
    }

    if (forceDisableCustody) { //for bping which needs to receive echo packets instead of admin records
        m_useCustodyTransfer = false;
    }

    
    if (outductsConfigPtr) {
        m_currentlySendingBundleIdSet.reserve(outductsConfigPtr->m_outductElementConfigVector[0].bundlePipelineLimit);
        m_useInductForSendingBundles = false;
        if (!m_outductManager.LoadOutductsFromConfig(*outductsConfigPtr, m_myEid.nodeId, UINT16_MAX,
            10000000, //todo 10MB max rx opportunistic bundle
            outductOpportunisticProcessReceivedBundleCallback,
            boost::bind(&BpSourcePattern::OnFailedBundleVecSendCallback, this, boost::placeholders::_1, boost::placeholders::_2, boost::placeholders::_3),
            OnFailedBundleZmqSendCallback_t(), //bpsourcepattern only sends vec8 bundles (not zmq) so this will never be needed
            boost::bind(&BpSourcePattern::OnSuccessfulBundleSendCallback, this, boost::placeholders::_1, boost::placeholders::_2),
            boost::bind(&BpSourcePattern::OnOutductLinkStatusChangedCallback, this, boost::placeholders::_1, boost::placeholders::_2)))
        {
            return;
        }
    }
    else {
        m_useInductForSendingBundles = true;
    }

    m_running = true;
    m_allOutductsReady = false;
   
    
    m_bpSourcePatternThreadPtr = boost::make_unique<boost::thread>(
        boost::bind(&BpSourcePattern::BpSourcePatternThreadFunc, this, bundleRate)); //create and start the worker thread



}


void BpSourcePattern::BpSourcePatternThreadFunc(uint32_t bundleRate) {

    boost::mutex waitingForRxBundleBeforeNextTxMutex;
    boost::mutex::scoped_lock waitingForRxBundleBeforeNextTxLock(waitingForRxBundleBeforeNextTxMutex);

    boost::mutex waitingForBundlePipelineFreeMutex;
    boost::mutex::scoped_lock waitingForBundlePipelineFreeLock(waitingForBundlePipelineFreeMutex);

    while (m_running) {
        if (m_useInductForSendingBundles) {
            std::cout << "Waiting for Tcpcl opportunistic link on the induct to become available for forwarding bundles..." << std::endl;
            boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
            if (m_tcpclInductPtr) {
                std::cout << "Induct opportunistic link ready to forward" << std::endl;
                break;
            }
        }
        else {
            std::cout << "Waiting for Outduct to become ready to forward..." << std::endl;
            boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
            if (m_outductManager.AllReadyToForward()) {
                std::cout << "Outduct ready to forward" << std::endl;
                break;
            }
        }
    }
    if (!m_running) {
        std::cout << "BpGen Terminated before a connection could be made" << std::endl;
        return;
    }
    boost::this_thread::sleep(boost::posix_time::milliseconds(1000)); //todo make sure connection from hdtn to bpgen induct
    m_allOutductsReady = true;

    #define BP_MSG_BUFSZ             (65536 * 100) //todo

    boost::posix_time::time_duration sleepValTimeDuration = boost::posix_time::special_values::neg_infin;
    Outduct* outduct = m_outductManager.GetOutductByOutductUuid(0);
    uint64_t outductMaxBundlesInPipeline = 0;
    if (outduct) {
        outductMaxBundlesInPipeline = outduct->GetOutductMaxBundlesInPipeline();
    }
    if(bundleRate) {
        std::cout << "Generating up to " << bundleRate << " bundles / second" << std::endl;
        const double sval = 1000000.0 / bundleRate;   // sleep val in usec
        ////sval *= BP_MSG_NBUF;
        const uint64_t sValU64 = static_cast<uint64_t>(sval);
        sleepValTimeDuration = boost::posix_time::microseconds(sValU64);
        std::cout << "Sleeping for " << sValU64 << " usec between bursts" << std::endl;
    }
    else {
        if (m_useInductForSendingBundles) {
            if (m_tcpclInductPtr) {
                std::cout << "bundle rate of zero used.. Going as fast as possible by allowing up to ??? unacked bundles" << std::endl;
            }
            else {
                std::cerr << "error: null induct" << std::endl;
                return;
            }
        }
        else {
            if (outduct) {
                if (outduct != m_outductManager.GetOutductByFinalDestinationEid_ThreadSafe(m_finalDestinationEid)) {
                    std::cerr << "error, outduct 0 does not support finalDestinationEid " << m_finalDestinationEid << "\n";
                    return;
                }
                std::cout << "bundle rate of zero used.. Going as fast as possible by allowing up to " << outductMaxBundlesInPipeline << " unacked bundles" << std::endl;
            }
            else {
                std::cerr << "error: null outduct" << std::endl;
                return;
            }
        }
        
    }


    std::size_t numEventsTooManyUnackedBundles = 0;
    m_bundleCount = 0;
    m_numRfc5050CustodyTransfers = 0;
    m_numAcsCustodyTransfers = 0;
    m_numAcsPacketsReceived = 0;
    uint64_t bundle_data = 0;
    uint64_t raw_data = 0;

   

    uint64_t lastTimeRfc5050 = 0;
    uint64_t lastMillisecondsSinceStartOfYear2000 = 0;
    
    uint64_t seq = 0;
    
    uint64_t nextCtebCustodyId = 0;


    boost::mutex localMutex;
    boost::mutex::scoped_lock lock(localMutex);

    boost::asio::io_service ioService;
    boost::asio::deadline_timer deadlineTimer(ioService, sleepValTimeDuration);
    std::vector<uint8_t> bundleToSend;
    boost::posix_time::ptime startTime = boost::posix_time::microsec_clock::universal_time();
    bool isGeneratingNewBundles = true;
    while (m_running) { //keep thread alive if running
                
        if(bundleRate) {
            boost::system::error_code ec;
            deadlineTimer.wait(ec);
            if(ec) {
                std::cout << "timer error: " << ec.message() << std::endl;
                return;
            }
            deadlineTimer.expires_at(deadlineTimer.expires_at() + sleepValTimeDuration);
        }
        
        uint64_t payloadSizeBytes;
        uint64_t bundleLength;
        uint64_t bundleId;
        if (m_queueBundlesThatFailedToSend.size()) {
            m_mutexQueueBundlesThatFailedToSend.lock();
            bundle_userdata_pair_t& bup = m_queueBundlesThatFailedToSend.front();
            bundleid_payloadsize_pair_t& bpp = bup.second;
            bundleId = bpp.first;
            payloadSizeBytes = bpp.second;
            bundleToSend = std::move(bup.first);
            m_queueBundlesThatFailedToSend.pop();
            m_mutexQueueBundlesThatFailedToSend.unlock();
            bundleLength = bundleToSend.size();
        }
        else if (isGeneratingNewBundles) {
            payloadSizeBytes = GetNextPayloadLength_Step1();
            if (payloadSizeBytes == 0) {
                std::cout << "payloadSizeBytes == 0... out of work.. waiting for all bundles to fully transmit before exiting\n";
                isGeneratingNewBundles = false;
                continue;
            }
            bundleId = m_nextBundleId++;
            if (m_useBpVersion7) {
                BundleViewV7 bv;
                Bpv7CbhePrimaryBlock& primary = bv.m_primaryBlockView.header;
                //primary.SetZero();
                primary.m_bundleProcessingControlFlags = BPV7_BUNDLEFLAG::NOFRAGMENT;  //All BP endpoints identified by ipn-scheme endpoint IDs are singleton endpoints.
                primary.m_sourceNodeId = m_myEid;
                primary.m_destinationEid = m_finalDestinationEid;
                if (m_useCustodyTransfer) { //not supported yet
                    primary.m_bundleProcessingControlFlags |= BPV7_BUNDLEFLAG::RECEPTION_STATUS_REPORTS_REQUESTED; //??
                    primary.m_reportToEid.Set(m_myEid.nodeId, m_myCustodianServiceId);
                }
                else {
                    primary.m_reportToEid.Set(0, 0);
                }
                primary.m_creationTimestamp.SetTimeFromNow();
                if (primary.m_creationTimestamp.millisecondsSinceStartOfYear2000 == lastMillisecondsSinceStartOfYear2000) {
                    ++seq;
                }
                else {
                    seq = 0;
                }
                lastMillisecondsSinceStartOfYear2000 = primary.m_creationTimestamp.millisecondsSinceStartOfYear2000;
                primary.m_creationTimestamp.sequenceNumber = seq;
                primary.m_lifetimeMilliseconds = 1000000;
                primary.m_crcType = BPV7_CRC_TYPE::CRC32C;
                bv.m_primaryBlockView.SetManuallyModified();

                //add hop count block (before payload last block)
                {
                    std::unique_ptr<Bpv7CanonicalBlock> blockPtr = boost::make_unique<Bpv7HopCountCanonicalBlock>();
                    Bpv7HopCountCanonicalBlock& block = *(reinterpret_cast<Bpv7HopCountCanonicalBlock*>(blockPtr.get()));

                    block.m_blockProcessingControlFlags = BPV7_BLOCKFLAG::REMOVE_BLOCK_IF_IT_CANT_BE_PROCESSED; //something for checking against
                    block.m_blockNumber = 2;
                    block.m_crcType = BPV7_CRC_TYPE::CRC32C;
                    block.m_hopLimit = 100; //Hop limit MUST be in the range 1 through 255.
                    block.m_hopCount = 0; //the hop count value SHOULD initially be zero and SHOULD be increased by 1 on each hop.
                    bv.AppendMoveCanonicalBlock(blockPtr);
                }

                //append payload block (must be last block)
                {
                    std::unique_ptr<Bpv7CanonicalBlock> payloadBlockPtr = boost::make_unique<Bpv7CanonicalBlock>();
                    Bpv7CanonicalBlock& payloadBlock = *payloadBlockPtr;
                    //payloadBlock.SetZero();

                    payloadBlock.m_blockTypeCode = BPV7_BLOCK_TYPE_CODE::PAYLOAD;
                    payloadBlock.m_blockProcessingControlFlags = BPV7_BLOCKFLAG::NO_FLAGS_SET;
                    payloadBlock.m_blockNumber = 1; //must be 1
                    payloadBlock.m_crcType = BPV7_CRC_TYPE::CRC32C;
                    payloadBlock.m_dataLength = payloadSizeBytes;
                    payloadBlock.m_dataPtr = NULL; //NULL will preallocate (won't copy or compute crc, user must do that manually below)
                    bv.AppendMoveCanonicalBlock(payloadBlockPtr);
                }

                //render bundle to the front buffer
                if (!bv.Render(payloadSizeBytes + 1000)) {
                    std::cout << "error rendering bpv7 bundle\n";
                    return;
                }

                BundleViewV7::Bpv7CanonicalBlockView& payloadBlockView = bv.m_listCanonicalBlockView.back(); //payload block must be the last block

                //manually copy data to preallocated space and compute crc
                if (!CopyPayload_Step2(payloadBlockView.headerPtr->m_dataPtr)) { //m_dataPtr now points to new allocated or copied data within the serialized block (from after Render())
                    std::cout << "copy payload error\n";
                    m_running = false;
                    continue;
                }
                payloadBlockView.headerPtr->RecomputeCrcAfterDataModification((uint8_t*)payloadBlockView.actualSerializedBlockPtr.data(), payloadBlockView.actualSerializedBlockPtr.size()); //recompute crc

                //move the bundle out of bundleView
                bundleToSend = std::move(bv.m_frontBuffer);
                bundleLength = bundleToSend.size();
            }
            else { //bp version 6
                BundleViewV6 bv;
                Bpv6CbhePrimaryBlock& primary = bv.m_primaryBlockView.header;
                //primary.SetZero();
                primary.m_bundleProcessingControlFlags = BPV6_BUNDLEFLAG::PRIORITY_EXPEDITED | BPV6_BUNDLEFLAG::SINGLETON | BPV6_BUNDLEFLAG::NOFRAGMENT;
                if (m_useCustodyTransfer) {
                    primary.m_bundleProcessingControlFlags |= BPV6_BUNDLEFLAG::CUSTODY_REQUESTED;
                    primary.m_custodianEid.Set(m_myEid.nodeId, m_myCustodianServiceId);
                }
                primary.m_sourceNodeId = m_myEid;
                primary.m_destinationEid = m_finalDestinationEid;

                primary.m_creationTimestamp.SetTimeFromNow();
                if (primary.m_creationTimestamp.secondsSinceStartOfYear2000 == lastTimeRfc5050) {
                    ++seq;
                }
                else {
                    seq = 0;
                }
                lastTimeRfc5050 = primary.m_creationTimestamp.secondsSinceStartOfYear2000;
                primary.m_creationTimestamp.sequenceNumber = seq;
                primary.m_lifetimeSeconds = 1000;
                bv.m_primaryBlockView.SetManuallyModified();




                if (m_useCustodyTransfer) {
                    if (m_custodyTransferUseAcs) {
                        const uint64_t ctebCustodyId = nextCtebCustodyId++;
                        //add cteb
                        {
                            std::unique_ptr<Bpv6CanonicalBlock> blockPtr = boost::make_unique<Bpv6CustodyTransferEnhancementBlock>();
                            Bpv6CustodyTransferEnhancementBlock& block = *(reinterpret_cast<Bpv6CustodyTransferEnhancementBlock*>(blockPtr.get()));
                            //block.SetZero();

                            block.m_blockProcessingControlFlags = BPV6_BLOCKFLAG::NO_FLAGS_SET; //something for checking against
                            block.m_custodyId = ctebCustodyId;
                            block.m_ctebCreatorCustodianEidString = m_myCustodianEidUriString;
                            bv.AppendMoveCanonicalBlock(blockPtr);
                        }

                        m_mutexCtebSet.lock();
                        FragmentSet::InsertFragment(m_outstandingCtebCustodyIdsFragmentSet, FragmentSet::data_fragment_t(ctebCustodyId, ctebCustodyId));
                        m_mutexCtebSet.unlock();
                    }
                    //always prepare for rfc5050 style custody transfer if next hop doesn't support acs
                    if (!m_detectedNextCustodianSupportsCteb) { //prevent map from filling up endlessly if we're using cteb
                        cbhe_bundle_uuid_nofragment_t uuid = primary.GetCbheBundleUuidNoFragmentFromPrimary();
                        m_mutexBundleUuidSet.lock();
                        const bool success = m_cbheBundleUuidSet.insert(uuid).second;
                        m_mutexBundleUuidSet.unlock();
                        if (!success) {
                            std::cerr << "error insert bundle uuid\n";
                            m_running = false;
                            continue;
                        }
                    }

                }

                //append payload block (must be last block)
                {
                    std::unique_ptr<Bpv6CanonicalBlock> payloadBlockPtr = boost::make_unique<Bpv6CanonicalBlock>();
                    Bpv6CanonicalBlock& payloadBlock = *payloadBlockPtr;
                    //payloadBlock.SetZero();

                    payloadBlock.m_blockTypeCode = BPV6_BLOCK_TYPE_CODE::PAYLOAD;
                    payloadBlock.m_blockProcessingControlFlags = BPV6_BLOCKFLAG::NO_FLAGS_SET;
                    payloadBlock.m_blockTypeSpecificDataLength = payloadSizeBytes;
                    payloadBlock.m_blockTypeSpecificDataPtr = NULL; //NULL will preallocate (won't copy or compute crc, user must do that manually below)
                    bv.AppendMoveCanonicalBlock(payloadBlockPtr);
                }

                //render bundle to the front buffer
                if (!bv.Render(payloadSizeBytes + 1000)) {
                    std::cout << "error rendering bpv7 bundle\n";
                    return;
                }

                BundleViewV6::Bpv6CanonicalBlockView& payloadBlockView = bv.m_listCanonicalBlockView.back(); //payload block is the last block in this case

                //manually copy data to preallocated space and compute crc
                if (!CopyPayload_Step2(payloadBlockView.headerPtr->m_blockTypeSpecificDataPtr)) { //m_dataPtr now points to new allocated or copied data within the serialized block (from after Render())
                    std::cout << "copy payload error\n";
                    m_running = false;
                    continue;
                }

                //move the bundle out of bundleView
                bundleToSend = std::move(bv.m_frontBuffer);
                bundleLength = bundleToSend.size();

            }
        }
        else if (m_currentlySendingBundleIdSet.empty()) { //natural stopping criteria
            std::cout << "all bundles generated and fully sent\n";
            m_running = false;
            continue;
        }
        else { //bundles are still being sent
            m_waitingForBundlePipelineFreeConditionVariable.timed_wait(waitingForBundlePipelineFreeLock, boost::posix_time::milliseconds(10));
            continue;
        }
        
        std::vector<uint8_t> bundleToSendUserData(sizeof(bundleid_payloadsize_pair_t));
        bundleid_payloadsize_pair_t* bundleToSendUserDataPairPtr = (bundleid_payloadsize_pair_t*)bundleToSendUserData.data();
        bundleToSendUserDataPairPtr->first = bundleId;
        bundleToSendUserDataPairPtr->second = payloadSizeBytes;
        
        //send message
        do { //try to send at least once before terminating
            if (m_linkIsDown) {
                //note BpSource has no routing capability so it must send to the only connection available to it
                std::cerr << "BpSourcePattern waiting for linkup event.. retrying in 1 second\n";
                boost::this_thread::sleep(boost::posix_time::seconds(1));
            }
            else { //link is not down, proceed
                m_isWaitingForRxBundleBeforeNextTx = true;
                bool successForward = false;


                if (!m_useInductForSendingBundles) { //outduct for forwarding bundles
                    boost::posix_time::ptime timeoutExpiry(boost::posix_time::special_values::not_a_date_time);
                    bool timeout = false;
                    while (m_currentlySendingBundleIdSet.size() >= outductMaxBundlesInPipeline) {
                        const boost::posix_time::ptime nowTime = boost::posix_time::microsec_clock::universal_time();
                        if (timeoutExpiry == boost::posix_time::special_values::not_a_date_time) {
                            timeoutExpiry = nowTime + boost::posix_time::seconds(m_bundleSendTimeoutSeconds);
                        }
                        if (timeoutExpiry <= nowTime) {
                            timeout = true;
                            break;
                        }
                        m_waitingForBundlePipelineFreeConditionVariable.timed_wait(waitingForBundlePipelineFreeLock, boost::posix_time::milliseconds(10));
                    }
                    if (timeout) {
                        std::cerr << "BpSourcePattern was unable to send a bundle for " << m_bundleSendTimeoutSeconds << " seconds on the outduct.. retrying in 1 second" << std::endl;
                        boost::this_thread::sleep(boost::posix_time::seconds(1));
                    }
                    else { //proceed to send bundle (no timeout)
                        //insert bundleId right before forward
                        m_mutexCurrentlySendingBundleIdSet.lock();
                        m_currentlySendingBundleIdSet.insert(bundleId); //ok if already exists
                        m_mutexCurrentlySendingBundleIdSet.unlock();

                        if (!outduct->Forward(bundleToSend, std::move(bundleToSendUserData))) {
                            std::cerr << "BpSourcePattern unable to send bundle on the outduct.. retrying in 1 second\n";
                            boost::this_thread::sleep(boost::posix_time::seconds(1));
                        }
                        else {
                            successForward = true;
                        }
                    }
                }
                else { //induct for forwarding bundles
                    if (!m_tcpclInductPtr->ForwardOnOpportunisticLink(m_tcpclOpportunisticRemoteNodeId, bundleToSend, m_bundleSendTimeoutSeconds)) {
                        //note BpSource has no routing capability so it must send to the only connection available to it
                        std::cerr << "BpSourcePattern was unable to send a bundle for " << m_bundleSendTimeoutSeconds << " seconds on the opportunistic induct.. retrying in 1 second" << std::endl;
                        boost::this_thread::sleep(boost::posix_time::seconds(1));
                    }
                    else {
                        successForward = true;
                    }
                }
                if (successForward) { //success forward
                    if (bundleToSend.size() != 0) {
                        std::cerr << "error in BpGenAsync::BpGenThreadFunc: bundleToSend was not moved in Forward" << std::endl;
                        std::cerr << "bundleToSend.size() : " << bundleToSend.size() << std::endl;
                    }
                    ++m_bundleCount;
                    bundle_data += payloadSizeBytes;     // payload data
                    raw_data += bundleLength; // bundle overhead + payload data
                    while (m_requireRxBundleBeforeNextTx && m_running && m_isWaitingForRxBundleBeforeNextTx) {
                        m_waitingForRxBundleBeforeNextTxConditionVariable.timed_wait(waitingForRxBundleBeforeNextTxLock, boost::posix_time::milliseconds(10));
                    }
                    break;
                }
            } //end if link is not down
        } while (m_running);
    }
    //todo m_outductManager.StopAllOutducts(); //wait for all pipelined bundles to complete before getting a finishedTime
    boost::posix_time::ptime finishedTime = boost::posix_time::microsec_clock::universal_time();

    std::cout << "bundle_count: " << m_bundleCount << std::endl;
    std::cout << "bundle_data (payload data): " << bundle_data << " bytes" << std::endl;
    std::cout << "raw_data (bundle overhead + payload data): " << raw_data << " bytes" << std::endl;
    if (bundleRate == 0) {
        std::cout << "numEventsTooManyUnackedBundles: " << numEventsTooManyUnackedBundles << std::endl;
    }

    if (m_useInductForSendingBundles) {
        std::cout << "BpSourcePattern Keeping Tcpcl Induct Opportunistic link open for 4 seconds to finish sending" << std::endl;
        boost::this_thread::sleep(boost::posix_time::seconds(4));
    }
    else if (Outduct * outduct = m_outductManager.GetOutductByOutductUuid(0)) {
        if (outduct->GetConvergenceLayerName() == "ltp_over_udp") {
            std::cout << "BpSourcePattern Keeping UDP open for 4 seconds to acknowledge report segments" << std::endl;
            boost::this_thread::sleep(boost::posix_time::seconds(4));
        }
    }
    if (m_useCustodyTransfer) {
        uint64_t lastNumCustodyTransfers = UINT64_MAX;
        while (true) {
            const uint64_t totalCustodyTransfers = std::max(m_numRfc5050CustodyTransfers, m_numAcsCustodyTransfers);
            if (totalCustodyTransfers == m_bundleCount) {
                std::cout << "BpSourcePattern received all custody transfers" << std::endl;
                break;
            }
            else if (totalCustodyTransfers != lastNumCustodyTransfers) {
                lastNumCustodyTransfers = totalCustodyTransfers;
                std::cout << "BpSourcePattern waiting for an additional 10 seconds to receive custody transfers" << std::endl;
                boost::this_thread::sleep(boost::posix_time::seconds(10));
            }
            else {
                std::cout << "BpSourcePattern received no custody transfers for the last 10 seconds.. exiting" << std::endl;
                break;
            }
        }
    }

    if (m_currentlySendingBundleIdSet.size()) { 
        std::cout << "error in BpSourcePattern, bundles were still being sent before termination\n";
    }

    std::cout << "m_numRfc5050CustodyTransfers: " << m_numRfc5050CustodyTransfers << std::endl;
    std::cout << "m_numAcsCustodyTransfers: " << m_numAcsCustodyTransfers << std::endl;
    std::cout << "m_numAcsPacketsReceived: " << m_numAcsPacketsReceived << std::endl;

    boost::posix_time::time_duration diff = finishedTime - startTime;
    {
        const double rateMbps = (bundle_data * 8.0) / (diff.total_microseconds());
        printf("Sent bundle_data (payload data) at %0.4f Mbits/sec\n", rateMbps);
    }
    {
        const double rateMbps = (raw_data * 8.0) / (diff.total_microseconds());
        printf("Sent raw_data (bundle overhead + payload data) at %0.4f Mbits/sec\n", rateMbps);
    }

    std::cout << "BpSourcePattern::BpSourcePatternThreadFunc thread exiting\n";
}

void BpSourcePattern::WholeRxBundleReadyCallback(padded_vector_uint8_t & wholeBundleVec) {
    //if more than 1 Induct, must protect shared resources with mutex.  Each Induct has
    //its own processing thread that calls this callback
    const uint8_t firstByte = wholeBundleVec[0];
    const bool isBpVersion6 = (firstByte == 6);
    const bool isBpVersion7 = (firstByte == ((4U << 5) | 31U));  //CBOR major type 4, additional information 31 (Indefinite-Length Array)
    if (isBpVersion6) {
        BundleViewV6 bv;
        if (!bv.LoadBundle(wholeBundleVec.data(), wholeBundleVec.size())) {
            std::cerr << "malformed Bpv6 BpSourcePattern received\n";
            return;
        }
        //check primary
        const Bpv6CbhePrimaryBlock & primary = bv.m_primaryBlockView.header;
        const cbhe_eid_t & receivedFinalDestinationEid = primary.m_destinationEid;
        static const BPV6_BUNDLEFLAG requiredPrimaryFlagsForCustody = BPV6_BUNDLEFLAG::ADMINRECORD; // | BPV6_BUNDLEFLAG::SINGLETON | BPV6_BUNDLEFLAG::NOFRAGMENT
        if ((primary.m_bundleProcessingControlFlags & requiredPrimaryFlagsForCustody) != requiredPrimaryFlagsForCustody) { //assume non-admin-record bundle (perhaps a bpecho bundle)


            if (receivedFinalDestinationEid != m_myEid) {
                std::cerr << "BpSourcePattern received a bundle with final destination " << receivedFinalDestinationEid
                    << " that does not match this destination " << m_myEid << "\n";
                return;
            }

            std::vector<BundleViewV6::Bpv6CanonicalBlockView*> blocks;
            bv.GetCanonicalBlocksByType(BPV6_BLOCK_TYPE_CODE::PAYLOAD, blocks);
            if (blocks.size() != 1) {
                std::cerr << "error BpSourcePattern received a non-admin-record bundle with no payload block\n";
                return;
            }
            Bpv6CanonicalBlock & payloadBlock = *(blocks[0]->headerPtr);
            m_totalNonAdminRecordBpv6PayloadBytesRx += payloadBlock.m_blockTypeSpecificDataLength;
            m_totalNonAdminRecordBpv6BundleBytesRx += bv.m_renderedBundle.size();
            ++m_totalNonAdminRecordBpv6BundlesRx;

            if (!ProcessNonAdminRecordBundlePayload(payloadBlock.m_blockTypeSpecificDataPtr, payloadBlock.m_blockTypeSpecificDataLength)) {
                std::cerr << "error ProcessNonAdminRecordBundlePayload\n";
                return;
            }
            m_isWaitingForRxBundleBeforeNextTx = false;
            m_waitingForRxBundleBeforeNextTxConditionVariable.notify_one();
        }
        else { //admin record

            if (receivedFinalDestinationEid != m_myCustodianEid) {
                std::cerr << "BpSourcePattern received an admin record bundle with final destination "
                    << Uri::GetIpnUriString(receivedFinalDestinationEid.nodeId, receivedFinalDestinationEid.serviceId)
                    << " that does not match this custodial destination " << Uri::GetIpnUriString(m_myCustodianEid.nodeId, m_myCustodianEid.serviceId) << "\n";
                return;
            }

            std::vector<BundleViewV6::Bpv6CanonicalBlockView*> blocks;
            bv.GetCanonicalBlocksByType(BPV6_BLOCK_TYPE_CODE::PAYLOAD, blocks);
            if (blocks.size() != 1) {
                std::cerr << "error BpSourcePattern received an admin-record bundle with no payload block\n";
                return;
            }
            Bpv6AdministrativeRecord* adminRecordBlockPtr = dynamic_cast<Bpv6AdministrativeRecord*>(blocks[0]->headerPtr.get());
            if (adminRecordBlockPtr == NULL) {
                std::cerr << "error BpSourcePattern cannot cast payload block to admin record\n";
                return;
            }
            const BPV6_ADMINISTRATIVE_RECORD_TYPE_CODE adminRecordType = adminRecordBlockPtr->m_adminRecordTypeCode;
            if (adminRecordType == BPV6_ADMINISTRATIVE_RECORD_TYPE_CODE::AGGREGATE_CUSTODY_SIGNAL) {
                m_detectedNextCustodianSupportsCteb = true;
                ++m_numAcsPacketsReceived;
                //check acs
                Bpv6AdministrativeRecordContentAggregateCustodySignal * acsPtr = dynamic_cast<Bpv6AdministrativeRecordContentAggregateCustodySignal*>(adminRecordBlockPtr->m_adminRecordContentPtr.get());
                if (acsPtr == NULL) {
                    std::cerr << "error BpSourcePattern cannot cast admin record content to Bpv6AdministrativeRecordContentAggregateCustodySignal\n";
                    return;
                }
                Bpv6AdministrativeRecordContentAggregateCustodySignal & acs = *(reinterpret_cast<Bpv6AdministrativeRecordContentAggregateCustodySignal*>(acsPtr));
                if (!acs.DidCustodyTransferSucceed()) {
                    std::cerr << "error acs custody transfer failed with reason code " << acs.GetReasonCode() << "\n";
                    return;
                }

                m_mutexCtebSet.lock();
                for (std::set<FragmentSet::data_fragment_t>::const_iterator it = acs.m_custodyIdFills.cbegin(); it != acs.m_custodyIdFills.cend(); ++it) {
                    m_numAcsCustodyTransfers += (it->endIndex + 1) - it->beginIndex;
                    FragmentSet::RemoveFragment(m_outstandingCtebCustodyIdsFragmentSet, *it);
                }
                m_mutexCtebSet.unlock();
            }
            else if (adminRecordType == BPV6_ADMINISTRATIVE_RECORD_TYPE_CODE::CUSTODY_SIGNAL) { //rfc5050 style custody transfer
                Bpv6AdministrativeRecordContentCustodySignal * csPtr = dynamic_cast<Bpv6AdministrativeRecordContentCustodySignal*>(adminRecordBlockPtr->m_adminRecordContentPtr.get());
                if (csPtr == NULL) {
                    std::cerr << "error BpSourcePattern cannot cast admin record content to Bpv6AdministrativeRecordContentCustodySignal\n";
                    return;
                }
                Bpv6AdministrativeRecordContentCustodySignal & cs = *(reinterpret_cast<Bpv6AdministrativeRecordContentCustodySignal*>(csPtr));
                if (!cs.DidCustodyTransferSucceed()) {
                    std::cerr << "rfc5050 custody transfer failed with reason code " << cs.GetReasonCode() << "\n";
                    return;
                }
                if (cs.m_isFragment) {
                    std::cerr << "error custody signal with fragmentation received\n";
                    return;
                }
                cbhe_bundle_uuid_nofragment_t uuid;
                if (!Uri::ParseIpnUriString(cs.m_bundleSourceEid, uuid.srcEid.nodeId, uuid.srcEid.serviceId)) {
                    std::cerr << "error custody signal with bad ipn string\n";
                    return;
                }
                uuid.creationSeconds = cs.m_copyOfBundleCreationTimestamp.secondsSinceStartOfYear2000;
                uuid.sequence = cs.m_copyOfBundleCreationTimestamp.sequenceNumber;
                m_mutexBundleUuidSet.lock();
                const bool success = (m_cbheBundleUuidSet.erase(uuid) != 0);
                m_mutexBundleUuidSet.unlock();
                if (!success) {
                    std::cerr << "error rfc5050 custody signal received but bundle uuid not found\n";
                    return;
                }
                ++m_numRfc5050CustodyTransfers;
            }
            else {
                std::cerr << "error unknown admin record type\n";
                return;
            }
        }
    }
    else if (isBpVersion7) {
        BundleViewV7 bv;
        if (!bv.LoadBundle(wholeBundleVec.data(), wholeBundleVec.size())) {
            std::cerr << "malformed Bpv7 BpSourcePattern received\n";
            return;
        }
        Bpv7CbhePrimaryBlock & primary = bv.m_primaryBlockView.header;
        const cbhe_eid_t finalDestEid = primary.m_destinationEid;
        const cbhe_eid_t srcEid = primary.m_sourceNodeId;



        //get previous node
        std::vector<BundleViewV7::Bpv7CanonicalBlockView*> blocks;
        bv.GetCanonicalBlocksByType(BPV7_BLOCK_TYPE_CODE::PREVIOUS_NODE, blocks);
        if (blocks.size() > 1) {
            std::cout << "error in BpSourcePattern::Process: version 7 bundle received has multiple previous node blocks\n";
            return;
        }
        else if (blocks.size() == 1) {
            if (Bpv7PreviousNodeCanonicalBlock* previousNodeBlockPtr = dynamic_cast<Bpv7PreviousNodeCanonicalBlock*>(blocks[0]->headerPtr.get())) {
                if (m_lastPreviousNode != previousNodeBlockPtr->m_previousNode) {
                    m_lastPreviousNode = previousNodeBlockPtr->m_previousNode;
                    std::cout << "bp version 7 bundles coming in from previous node " << m_lastPreviousNode << "\n";
                }
            }
            else {
                std::cout << "error in BpSourcePattern::Process: dynamic_cast to Bpv7PreviousNodeCanonicalBlock failed\n";
                return;
            }
        }

        //get hop count if exists
        bv.GetCanonicalBlocksByType(BPV7_BLOCK_TYPE_CODE::HOP_COUNT, blocks);
        if (blocks.size() > 1) {
            std::cout << "error in BpSourcePattern::Process: version 7 bundle received has multiple hop count blocks\n";
            return;
        }
        else if (blocks.size() == 1) {
            if (Bpv7HopCountCanonicalBlock* hopCountBlockPtr = dynamic_cast<Bpv7HopCountCanonicalBlock*>(blocks[0]->headerPtr.get())) {
                //the hop count value SHOULD initially be zero and SHOULD be increased by 1 on each hop.
                const uint64_t newHopCount = hopCountBlockPtr->m_hopCount + 1;
                //When a bundle's hop count exceeds its
                //hop limit, the bundle SHOULD be deleted for the reason "hop limit
                //exceeded", following the bundle deletion procedure defined in
                //Section 5.10.
                //Hop limit MUST be in the range 1 through 255.
                if ((newHopCount > hopCountBlockPtr->m_hopLimit) || (newHopCount > 255)) {
                    std::cout << "notice: BpSourcePattern::Process dropping version 7 bundle with hop count " << newHopCount << "\n";
                    return;
                }
                ++m_hopCounts[newHopCount];
            }
            else {
                std::cout << "error in BpSourcePattern::Process: dynamic_cast to Bpv7HopCountCanonicalBlock failed\n";
                return;
            }
        }

        //get payload block
        bv.GetCanonicalBlocksByType(BPV7_BLOCK_TYPE_CODE::PAYLOAD, blocks);

        if (blocks.size() != 1) {
            std::cerr << "error in BpSourcePattern::Process: Bpv7 payload block not found\n";
            return;
        }
        Bpv7CanonicalBlock & payloadBlock = *(blocks[0]->headerPtr);
        const uint64_t payloadDataLength = payloadBlock.m_dataLength;
        const uint8_t * payloadDataPtr = payloadBlock.m_dataPtr;
        m_totalNonAdminRecordBpv7PayloadBytesRx += payloadDataLength;
        m_totalNonAdminRecordBpv7BundleBytesRx += bv.m_renderedBundle.size();;
        ++m_totalNonAdminRecordBpv7BundlesRx;

        if (!ProcessNonAdminRecordBundlePayload(payloadDataPtr, payloadDataLength)) {
            std::cerr << "error ProcessNonAdminRecordBundlePayload\n";
            return;
        }
        m_isWaitingForRxBundleBeforeNextTx = false;
        m_waitingForRxBundleBeforeNextTxConditionVariable.notify_one();
    }
}

bool BpSourcePattern::ProcessNonAdminRecordBundlePayload(const uint8_t * data, const uint64_t size) {
    return true;
}

void BpSourcePattern::OnNewOpportunisticLinkCallback(const uint64_t remoteNodeId, Induct * thisInductPtr) {
    if (m_tcpclInductPtr = dynamic_cast<TcpclInduct*>(thisInductPtr)) {
        std::cout << "New opportunistic link detected on Tcpcl induct for ipn:" << remoteNodeId << ".*\n";
        m_tcpclOpportunisticRemoteNodeId = remoteNodeId;
    }
    else if (m_tcpclInductPtr = dynamic_cast<TcpclV4Induct*>(thisInductPtr)) {
        std::cout << "New opportunistic link detected on TcpclV4 induct for ipn:" << remoteNodeId << ".*\n";
        m_tcpclOpportunisticRemoteNodeId = remoteNodeId;
    }
    else {
        std::cerr << "error in BpSourcePattern::OnNewOpportunisticLinkCallback: Induct ptr cannot cast to TcpclInduct or TcpclV4Induct\n";
    }
}
void BpSourcePattern::OnDeletedOpportunisticLinkCallback(const uint64_t remoteNodeId) {
    m_tcpclOpportunisticRemoteNodeId = 0;
    std::cout << "Deleted opportunistic link on Tcpcl induct for ipn:" << remoteNodeId << ".*\n";
}

void BpSourcePattern::OnFailedBundleVecSendCallback(std::vector<uint8_t>& movableBundle, std::vector<uint8_t>& userData, uint64_t outductUuid) {
    bundleid_payloadsize_pair_t * p = (bundleid_payloadsize_pair_t*)userData.data();
    const uint64_t bundleId = p->first;
    std::cout << "Bundle failed to send: id=" << bundleId << " bundle size=" << movableBundle.size() << "\n";
    std::size_t sizeErased;
    {
        boost::mutex::scoped_lock lock(m_mutexQueueBundlesThatFailedToSend);
        boost::mutex::scoped_lock lock2(m_mutexCurrentlySendingBundleIdSet);
        m_queueBundlesThatFailedToSend.emplace(std::move(movableBundle), std::move(*p));
        sizeErased = m_currentlySendingBundleIdSet.erase(bundleId);
    }
    if (sizeErased == 0) {
        std::cout << "Error in BpSourcePattern::OnFailedBundleVecSendCallback: cannot find bundleId " << bundleId << "\n";
    }
    
    if (!m_linkIsDown) {
        std::cout << "Setting link status to DOWN\n";
        m_linkIsDown = true;
    }
    m_waitingForBundlePipelineFreeConditionVariable.notify_one();
}
void BpSourcePattern::OnSuccessfulBundleSendCallback(std::vector<uint8_t>& userData, uint64_t outductUuid) {
    bundleid_payloadsize_pair_t* p = (bundleid_payloadsize_pair_t*)userData.data();
    const uint64_t bundleId = p->first;
    //std::cout << "Bundle sent: id=" << bundleId << "\n";
    
    m_mutexCurrentlySendingBundleIdSet.lock();
    const std::size_t sizeErased = m_currentlySendingBundleIdSet.erase(bundleId);
    m_mutexCurrentlySendingBundleIdSet.unlock();
    if (sizeErased == 0) {
        std::cout << "Error in BpSourcePattern::OnSuccessfulBundleSendCallback: cannot find bundleId " << bundleId << "\n";
    }

    if (m_linkIsDown) {
        std::cout << "Setting link status to UP\n";
        m_linkIsDown = false;
    }
    m_waitingForBundlePipelineFreeConditionVariable.notify_one();
}
void BpSourcePattern::OnOutductLinkStatusChangedCallback(bool isLinkDownEvent, uint64_t outductUuid) {
    std::cout << "OnOutductLinkStatusChangedCallback isLinkDownEvent:" << isLinkDownEvent << " outductUuid " << outductUuid << "\n";
    const bool linkIsAlreadyDown = m_linkIsDown;
    if (isLinkDownEvent && (!linkIsAlreadyDown)) {
        std::cout << "Setting link status to DOWN\n";
    }
    else if ((!isLinkDownEvent) && linkIsAlreadyDown) {
        std::cout << "Setting link status to UP\n";
    }
    m_linkIsDown = isLinkDownEvent;
    m_waitingForBundlePipelineFreeConditionVariable.notify_one();
}
