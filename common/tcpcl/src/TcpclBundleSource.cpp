#include <string>
#include <iostream>
#include "TcpclBundleSource.h"
#include <boost/lexical_cast.hpp>
#include <boost/make_shared.hpp>
#include <boost/make_unique.hpp>
#include "Uri.h"

TcpclBundleSource::TcpclBundleSource(const uint16_t desiredKeeAliveIntervlSeconds, const uint64_t myNodeId, 
    const std::string & expectedRemoteEidUri, const unsigned int maxUnacked, const uint64_t maxFragmentSize) :
m_work(m_ioService), //prevent stopping of ioservice until destructor
m_resolver(m_ioService),
m_noKeepAlivePacketReceivedTimer(m_ioService),
m_needToSendKeepAliveMessageTimer(m_ioService),
m_sendShutdownMessageTimeoutTimer(m_ioService),
m_reconnectAfterShutdownTimer(m_ioService),
m_reconnectAfterOnConnectErrorTimer(m_ioService),
m_keepAliveIntervalSeconds(desiredKeeAliveIntervlSeconds),
m_reconnectionDelaySecondsIfNotZero(3), //default 3 unless remote says 0 in shutdown message
MAX_UNACKED(maxUnacked),
m_bytesToAckCb(MAX_UNACKED),
m_bytesToAckCbVec(MAX_UNACKED),
m_fragmentBytesToAckCbVec(MAX_UNACKED),
m_fragmentVectorIndexCbVec(MAX_UNACKED),
M_MAX_FRAGMENT_SIZE(maxFragmentSize),
m_readyToForward(false),
m_tcpclShutdownComplete(true),
m_shutdownCalled(false),
m_useLocalConditionVariableAckReceived(false), //for destructor only
M_DESIRED_KEEPALIVE_INTERVAL_SECONDS(desiredKeeAliveIntervlSeconds),

//ion 3.7.2 source code tcpcli.c line 1199 uses service number 0 for contact header:
//isprintf(eid, sizeof eid, "ipn:" UVAST_FIELDSPEC ".0", getOwnNodeNbr());
M_THIS_EID_STRING(Uri::GetIpnUriString(myNodeId, 0)),

m_totalDataSegmentsAcked(0),
m_totalBytesAcked(0),
m_totalDataSegmentsSent(0),
m_totalFragmentedAcked(0),
m_totalFragmentedSent(0),
m_totalBundleBytesSent(0)
{
    uint64_t remoteNodeId, remoteServiceId;
    if (!Uri::ParseIpnUriString(expectedRemoteEidUri, remoteNodeId, remoteServiceId)) {
        std::cerr << "error in TcpclBundleSource constructor: error parsing remote EID URI string " << expectedRemoteEidUri
            << " .. TCPCL will fail the Contact Header Callback.  Correct the \"nextHopEndpointId\" field in the outducts config." << std::endl;
    }
    else {
        //ion 3.7.2 source code tcpcli.c line 1199 uses service number 0 for contact header:
        //isprintf(eid, sizeof eid, "ipn:" UVAST_FIELDSPEC ".0", getOwnNodeNbr());
        M_EXPECTED_REMOTE_CONTACT_HEADER_EID_STRING = Uri::GetIpnUriString(remoteNodeId, 0);
    }

    for (unsigned int i = 0; i < MAX_UNACKED; ++i) {
        m_fragmentBytesToAckCbVec[i].reserve(100);
    }

    m_handleTcpSendCallback = boost::bind(&TcpclBundleSource::HandleTcpSend, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred);
    m_handleTcpSendShutdownCallback = boost::bind(&TcpclBundleSource::HandleTcpSendShutdown, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred);

    m_ioServiceThreadPtr = boost::make_unique<boost::thread>(boost::bind(&boost::asio::io_service::run, &m_ioService));


    m_tcpcl.SetContactHeaderReadCallback(boost::bind(&TcpclBundleSource::ContactHeaderCallback, this, boost::placeholders::_1, boost::placeholders::_2, boost::placeholders::_3));
    m_tcpcl.SetDataSegmentContentsReadCallback(boost::bind(&TcpclBundleSource::DataSegmentCallback, this, boost::placeholders::_1, boost::placeholders::_2, boost::placeholders::_3));
    m_tcpcl.SetAckSegmentReadCallback(boost::bind(&TcpclBundleSource::AckCallback, this, boost::placeholders::_1));
    m_tcpcl.SetBundleRefusalCallback(boost::bind(&TcpclBundleSource::BundleRefusalCallback, this, boost::placeholders::_1));
    m_tcpcl.SetNextBundleLengthCallback(boost::bind(&TcpclBundleSource::NextBundleLengthCallback, this, boost::placeholders::_1));
    m_tcpcl.SetKeepAliveCallback(boost::bind(&TcpclBundleSource::KeepAliveCallback, this));
    m_tcpcl.SetShutdownMessageCallback(boost::bind(&TcpclBundleSource::ShutdownCallback, this, boost::placeholders::_1, boost::placeholders::_2, boost::placeholders::_3, boost::placeholders::_4));

}

TcpclBundleSource::~TcpclBundleSource() {
    Stop();
}

void TcpclBundleSource::Stop() {
    //prevent TcpclBundleSource from exiting before all bundles sent and acked
    boost::mutex localMutex;
    boost::mutex::scoped_lock lock(localMutex);
    m_useLocalConditionVariableAckReceived = true;
    std::size_t previousUnacked = std::numeric_limits<std::size_t>::max();
    for (unsigned int attempt = 0; attempt < 10; ++attempt) {
        const std::size_t numUnacked = GetTotalDataSegmentsUnacked();
        if (numUnacked) {
            std::cout << "notice: TcpclBundleSource destructor waiting on " << numUnacked << " unacked bundles" << std::endl;

            std::cout << "   acked: " << m_totalDataSegmentsAcked << std::endl;
            std::cout << "   total sent: " << m_totalDataSegmentsSent << std::endl;

            if (previousUnacked > numUnacked) {
                previousUnacked = numUnacked;
                attempt = 0;
            }
            m_localConditionVariableAckReceived.timed_wait(lock, boost::posix_time::milliseconds(250)); // call lock.unlock() and blocks the current thread
            //thread is now unblocked, and the lock is reacquired by invoking lock.lock()
            continue;
        }
        break;
    }

    DoTcpclShutdown(true, false);
    while (!m_tcpclShutdownComplete) {
        boost::this_thread::sleep(boost::posix_time::milliseconds(250));
    }
    m_tcpAsyncSenderPtr.reset(); //stop this first
    m_ioService.stop(); //ioservice requires stopping before join because of the m_work object

    if(m_ioServiceThreadPtr) {
        m_ioServiceThreadPtr->join();
        m_ioServiceThreadPtr.reset(); //delete it
    }

    //print stats
    std::cout << "m_totalDataSegmentsAcked " << m_totalDataSegmentsAcked << std::endl;
    std::cout << "m_totalBytesAcked " << m_totalBytesAcked << std::endl;
    std::cout << "m_totalDataSegmentsSent " << m_totalDataSegmentsSent << std::endl;
    std::cout << "m_totalFragmentedAcked " << m_totalFragmentedAcked << std::endl;
    std::cout << "m_totalFragmentedSent " << m_totalFragmentedSent << std::endl;
    std::cout << "m_totalBundleBytesSent " << m_totalBundleBytesSent << std::endl;
}

std::size_t TcpclBundleSource::GetTotalDataSegmentsAcked() {
    return m_totalDataSegmentsAcked;
}

std::size_t TcpclBundleSource::GetTotalDataSegmentsSent() {
    return m_totalDataSegmentsSent;
}

std::size_t TcpclBundleSource::GetTotalDataSegmentsUnacked() {
    return GetTotalDataSegmentsSent() - GetTotalDataSegmentsAcked();
}

std::size_t TcpclBundleSource::GetTotalBundleBytesAcked() {
    return m_totalBytesAcked;
}

std::size_t TcpclBundleSource::GetTotalBundleBytesSent() {
    return m_totalBundleBytesSent;
}

std::size_t TcpclBundleSource::GetTotalBundleBytesUnacked() {
    return GetTotalBundleBytesSent() - GetTotalBundleBytesAcked();
}

bool TcpclBundleSource::Forward(std::vector<uint8_t> & dataVec) {

    if(!m_readyToForward) {
        std::cerr << "link not ready to forward yet" << std::endl;
        return false;
    }
    
    
    const unsigned int writeIndex = m_bytesToAckCb.GetIndexForWrite(); //don't put this in tcp async write callback
    if (writeIndex == UINT32_MAX) { //push check
        std::cerr << "Error in TcpclBundleSource::Forward.. too many unacked packets" << std::endl;
        return false;
    }
    m_bytesToAckCbVec[writeIndex] = dataVec.size();
   

    ++m_totalDataSegmentsSent;
    m_totalBundleBytesSent += dataVec.size();

    std::vector<uint64_t> & currentFragmentBytesVec = m_fragmentBytesToAckCbVec[writeIndex];
    currentFragmentBytesVec.resize(0); //will be zero size if not fragmented
    m_fragmentVectorIndexCbVec[writeIndex] = 0; //used by the ack callback
    std::vector<TcpAsyncSenderElement*> elements;

    if (M_MAX_FRAGMENT_SIZE && (dataVec.size() > M_MAX_FRAGMENT_SIZE)) {
        elements.reserve((dataVec.size() / M_MAX_FRAGMENT_SIZE) + 2);
        uint64_t dataIndex = 0;
        const std::size_t dataVecSize = dataVec.size();
        while (true) {
            uint64_t bytesToSend = std::min(dataVecSize - dataIndex, M_MAX_FRAGMENT_SIZE);
            const bool isStartSegment = (dataIndex == 0);
            const bool isEndSegment = ((bytesToSend + dataIndex) == dataVecSize);

            TcpAsyncSenderElement * el = new TcpAsyncSenderElement();
            el->m_underlyingData.resize(1 + isEndSegment);
            el->m_constBufferVec.resize(2);
            Tcpcl::GenerateDataSegmentHeaderOnly(el->m_underlyingData[0], isStartSegment, isEndSegment, bytesToSend);
            el->m_constBufferVec[0] = boost::asio::buffer(el->m_underlyingData[0]);
            el->m_constBufferVec[1] = boost::asio::buffer(dataVec.data() + dataIndex, bytesToSend);
            if (isEndSegment) {
                el->m_underlyingData[1] = std::move(dataVec);
            }

            el->m_onSuccessfulSendCallbackByIoServiceThreadPtr = &m_handleTcpSendCallback;
            elements.push_back(el);
            

            dataIndex += bytesToSend;
            currentFragmentBytesVec.push_back(dataIndex); //bytes to ack must be cumulative of fragments

            if (isEndSegment) {
                break;
            }
        }
    }

    m_bytesToAckCb.CommitWrite(); //pushed

    if(elements.size()) { //is fragmented
        m_totalFragmentedSent += elements.size();
        for (std::size_t i = 0; i < elements.size(); ++i) {
            m_tcpAsyncSenderPtr->AsyncSend_ThreadSafe(elements[i]);
        }
    }
    else {
        TcpAsyncSenderElement * el = new TcpAsyncSenderElement();
        el->m_underlyingData.resize(2);
        Tcpcl::GenerateDataSegmentHeaderOnly(el->m_underlyingData[0], true, true, dataVec.size());
        el->m_underlyingData[1] = std::move(dataVec);
        el->m_constBufferVec.resize(2);
        el->m_constBufferVec[0] = boost::asio::buffer(el->m_underlyingData[0]);
        el->m_constBufferVec[1] = boost::asio::buffer(el->m_underlyingData[1]);
        el->m_onSuccessfulSendCallbackByIoServiceThreadPtr = &m_handleTcpSendCallback;
        m_tcpAsyncSenderPtr->AsyncSend_ThreadSafe(el);
    }
    
    return true;
}

bool TcpclBundleSource::Forward(zmq::message_t & dataZmq) {

    if (!m_readyToForward) {
        std::cerr << "link not ready to forward yet" << std::endl;
        return false;
    }


    const unsigned int writeIndex = m_bytesToAckCb.GetIndexForWrite(); //don't put this in tcp async write callback
    if (writeIndex == UINT32_MAX) { //push check
        std::cerr << "Error in TcpclBundleSource::Forward.. too many unacked packets" << std::endl;
        return false;
    }
    m_bytesToAckCbVec[writeIndex] = dataZmq.size();

    ++m_totalDataSegmentsSent;
    m_totalBundleBytesSent += dataZmq.size();

    std::vector<uint64_t> & currentFragmentBytesVec = m_fragmentBytesToAckCbVec[writeIndex];
    currentFragmentBytesVec.resize(0); //will be zero size if not fragmented
    m_fragmentVectorIndexCbVec[writeIndex] = 0; //used by the ack callback
    std::vector<TcpAsyncSenderElement*> elements;

    if (M_MAX_FRAGMENT_SIZE && (dataZmq.size() > M_MAX_FRAGMENT_SIZE)) {
        elements.reserve((dataZmq.size() / M_MAX_FRAGMENT_SIZE) + 2);
        uint64_t dataIndex = 0;
        const std::size_t dataZmqSize = dataZmq.size();
        while (true) {
            uint64_t bytesToSend = std::min(dataZmq.size() - dataIndex, M_MAX_FRAGMENT_SIZE);
            const bool isStartSegment = (dataIndex == 0);
            const bool isEndSegment = ((bytesToSend + dataIndex) == dataZmqSize);

            TcpAsyncSenderElement * el = new TcpAsyncSenderElement();
            el->m_underlyingData.resize(1);
            el->m_constBufferVec.resize(2);
            Tcpcl::GenerateDataSegmentHeaderOnly(el->m_underlyingData[0], isStartSegment, isEndSegment, bytesToSend);
            el->m_constBufferVec[0] = boost::asio::buffer(el->m_underlyingData[0]);
            el->m_constBufferVec[1] = boost::asio::buffer((static_cast<uint8_t*>(dataZmq.data())) + dataIndex, bytesToSend);
            if (isEndSegment) {
                el->m_underlyingDataZmq = boost::make_unique<zmq::message_t>(std::move(dataZmq));
            }

            el->m_onSuccessfulSendCallbackByIoServiceThreadPtr = &m_handleTcpSendCallback;
            elements.push_back(el);


            dataIndex += bytesToSend;
            currentFragmentBytesVec.push_back(dataIndex); //bytes to ack must be cumulative of fragments

            if (isEndSegment) {
                break;
            }
        }
    }

    m_bytesToAckCb.CommitWrite(); //pushed

    if (elements.size()) { //is fragmented
        m_totalFragmentedSent += elements.size();
        for (std::size_t i = 0; i < elements.size(); ++i) {
            m_tcpAsyncSenderPtr->AsyncSend_ThreadSafe(elements[i]);
        }
    }
    else {
        TcpAsyncSenderElement * el = new TcpAsyncSenderElement();
        el->m_underlyingData.resize(1);
        Tcpcl::GenerateDataSegmentHeaderOnly(el->m_underlyingData[0], true, true, dataZmq.size());
        el->m_underlyingDataZmq = boost::make_unique<zmq::message_t>(std::move(dataZmq));
        el->m_constBufferVec.resize(2);
        el->m_constBufferVec[0] = boost::asio::buffer(el->m_underlyingData[0]);
        el->m_constBufferVec[1] = boost::asio::buffer(boost::asio::buffer(el->m_underlyingDataZmq->data(), el->m_underlyingDataZmq->size()));
        el->m_onSuccessfulSendCallbackByIoServiceThreadPtr = &m_handleTcpSendCallback;
        m_tcpAsyncSenderPtr->AsyncSend_ThreadSafe(el);
    }

    return true;
}

bool TcpclBundleSource::Forward(const uint8_t* bundleData, const std::size_t size) {
    std::vector<uint8_t> vec(bundleData, bundleData + size);
    return Forward(vec);
}


void TcpclBundleSource::Connect(const std::string & hostname, const std::string & port) {

    boost::asio::ip::tcp::resolver::query query(hostname, port);
    m_resolver.async_resolve(query, boost::bind(&TcpclBundleSource::OnResolve,
                                                this, boost::asio::placeholders::error,
                                                boost::asio::placeholders::results));
}

void TcpclBundleSource::OnResolve(const boost::system::error_code & ec, boost::asio::ip::tcp::resolver::results_type results) { // Resolved endpoints as a range.
    if(ec) {
        std::cerr << "Error resolving: " << ec.message() << std::endl;
    }
    else {
        std::cout << "resolved host to " << results->endpoint().address() << ":" << results->endpoint().port() << ".  Connecting..." << std::endl;
        m_tcpSocketPtr = boost::make_shared<boost::asio::ip::tcp::socket>(m_ioService);
        m_resolverResults = results;
        boost::asio::async_connect(
            *m_tcpSocketPtr,
            m_resolverResults,
            boost::bind(
                &TcpclBundleSource::OnConnect,
                this,
                boost::asio::placeholders::error));
    }
}

void TcpclBundleSource::OnConnect(const boost::system::error_code & ec) {

    if (ec) {
        if (ec != boost::asio::error::operation_aborted) {
            std::cerr << "Error in OnConnect: " << ec.value() << " " << ec.message() << "\n";
            std::cout << "Will try to reconnect after 2 seconds" << std::endl;
            m_reconnectAfterOnConnectErrorTimer.expires_from_now(boost::posix_time::seconds(2));
            m_reconnectAfterOnConnectErrorTimer.async_wait(boost::bind(&TcpclBundleSource::OnReconnectAfterOnConnectError_TimerExpired, this, boost::asio::placeholders::error));
        }
        return;
    }

    std::cout << "connected.. sending contact header..\n";
    m_tcpclShutdownComplete = false;

    
    if(m_tcpSocketPtr) {
        m_tcpAsyncSenderPtr = boost::make_unique<TcpAsyncSender>(m_tcpSocketPtr, m_ioService);

        TcpAsyncSenderElement * el = new TcpAsyncSenderElement();
        el->m_underlyingData.resize(1);
        Tcpcl::GenerateContactHeader(el->m_underlyingData[0], CONTACT_HEADER_FLAGS::REQUEST_ACK_OF_BUNDLE_SEGMENTS, m_keepAliveIntervalSeconds, M_THIS_EID_STRING);
        el->m_constBufferVec.emplace_back(boost::asio::buffer(el->m_underlyingData[0])); //only one element so resize not needed
        el->m_onSuccessfulSendCallbackByIoServiceThreadPtr = &m_handleTcpSendCallback;
        m_tcpAsyncSenderPtr->AsyncSend_NotThreadSafe(el); //OnConnect runs in ioService thread so no thread safety needed

        StartTcpReceive();
    }
}

void TcpclBundleSource::OnReconnectAfterOnConnectError_TimerExpired(const boost::system::error_code& e) {
    if (e != boost::asio::error::operation_aborted) {
        // Timer was not cancelled, take necessary action.
        std::cout << "Trying to reconnect..." << std::endl;
        boost::asio::async_connect(
            *m_tcpSocketPtr,
            m_resolverResults,
            boost::bind(
                &TcpclBundleSource::OnConnect,
                this,
                boost::asio::placeholders::error));
    }
}


void TcpclBundleSource::HandleTcpSend(const boost::system::error_code& error, std::size_t bytes_transferred) {
    if (error) {
        std::cerr << "error in TcpclBundleSource::HandleTcpSend: " << error.message() << std::endl;
        DoTcpclShutdown(true, false);
    }
}

void TcpclBundleSource::HandleTcpSendShutdown(const boost::system::error_code& error, std::size_t bytes_transferred) {
    if (error) {
        std::cerr << "error in TcpclBundleSource::HandleTcpSendShutdown: " << error.message() << std::endl;
    }
    else {
        m_sendShutdownMessageTimeoutTimer.cancel();
    }
}

void TcpclBundleSource::StartTcpReceive() {
    m_tcpSocketPtr->async_read_some(
        boost::asio::buffer(m_tcpReadSomeBuffer, 2000),
        boost::bind(&TcpclBundleSource::HandleTcpReceiveSome, this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred));
}
void TcpclBundleSource::HandleTcpReceiveSome(const boost::system::error_code & error, std::size_t bytesTransferred) {
    if (!error) {
        //std::cout << "received " << bytesTransferred << "\n";

        //because TcpclBundleSource will not receive much data from the destination,
        //a separate thread is not needed to process it, but rather this
        //io_service thread will do the processing
        m_tcpcl.HandleReceivedChars(m_tcpReadSomeBuffer, bytesTransferred);
        StartTcpReceive(); //restart operation only if there was no error
    }
    else if (error == boost::asio::error::eof) {
        std::cout << "Tcp connection closed cleanly by peer" << std::endl;
        DoTcpclShutdown(false, false);
    }
    else if (error != boost::asio::error::operation_aborted) {
        std::cerr << "Error in UartMcu::HandleTcpReceiveSome: " << error.message() << std::endl;
    }
}




void TcpclBundleSource::ContactHeaderCallback(CONTACT_HEADER_FLAGS flags, uint16_t keepAliveIntervalSeconds, const std::string & localEid) {
    if (localEid != M_EXPECTED_REMOTE_CONTACT_HEADER_EID_STRING) {
        std::cout << "error in TcpclBundleSource::ContactHeaderCallback: received wrong contact header back from "
            << m_localEid << " but expected " << M_EXPECTED_REMOTE_CONTACT_HEADER_EID_STRING 
            << " .. TCPCL will not forward bundles.  Correct the \"nextHopEndpointId\" field in the outducts config." << std::endl;
        DoTcpclShutdown(false, false);
        return;
    }
    m_contactHeaderFlags = flags;
    //The keepalive_interval parameter is set to the minimum value from
    //both contact headers.  If one or both contact headers contains the
    //value zero, then the keepalive feature (described in Section 5.6)
    //is disabled.
    if(m_keepAliveIntervalSeconds > keepAliveIntervalSeconds) {
        std::cout << "notice: host has requested a smaller keepalive interval of " << keepAliveIntervalSeconds << " seconds." << std::endl;
        m_keepAliveIntervalSeconds = keepAliveIntervalSeconds;
    }
    m_localEid = localEid;
    std::cout << "received expected contact header back from " << m_localEid << std::endl;
    m_readyToForward = true;


    if(m_keepAliveIntervalSeconds) { //non-zero
        std::cout << "using " << keepAliveIntervalSeconds << " seconds for keepalive\n";

        // * 2 =>
        //If no message (KEEPALIVE or other) has been received for at least
        //twice the keepalive_interval, then either party MAY terminate the
        //session by transmitting a one-byte SHUTDOWN message (as described in
        //Table 2) and by closing the TCP connection.
        m_noKeepAlivePacketReceivedTimer.expires_from_now(boost::posix_time::seconds(m_keepAliveIntervalSeconds * 2));
        m_noKeepAlivePacketReceivedTimer.async_wait(boost::bind(&TcpclBundleSource::OnNoKeepAlivePacketReceived_TimerExpired, this, boost::asio::placeholders::error));


        m_needToSendKeepAliveMessageTimer.expires_from_now(boost::posix_time::seconds(m_keepAliveIntervalSeconds));
        m_needToSendKeepAliveMessageTimer.async_wait(boost::bind(&TcpclBundleSource::OnNeedToSendKeepAliveMessage_TimerExpired, this, boost::asio::placeholders::error));
    }

}

void TcpclBundleSource::DataSegmentCallback(std::vector<uint8_t> & dataSegmentDataVec, bool isStartFlag, bool isEndFlag) {

    std::cout << "TcpclBundleSource should never enter DataSegmentCallback" << std::endl;
}

void TcpclBundleSource::AckCallback(uint64_t totalBytesAcknowledged) {
    const unsigned int readIndex = m_bytesToAckCb.GetIndexForRead();
    if(readIndex == UINT32_MAX) { //empty
        std::cerr << "error: AckCallback called with empty queue" << std::endl;
    }
    else {
        std::vector<uint64_t> & currentFragmentBytesVec = m_fragmentBytesToAckCbVec[readIndex];
        if (currentFragmentBytesVec.size()) { //this was fragmented
            uint64_t & fragIdxRef = m_fragmentVectorIndexCbVec[readIndex];
            const uint64_t expectedFragByteToAck = currentFragmentBytesVec[fragIdxRef++];
            if (fragIdxRef == currentFragmentBytesVec.size()) {
                currentFragmentBytesVec.resize(0);
            }
            if (expectedFragByteToAck == totalBytesAcknowledged) {
                ++m_totalFragmentedAcked;
            }
            else {
                std::cerr << "error in TcpclBundleSource::AckCallback: wrong fragment bytes acked: expected " << expectedFragByteToAck << " but got " << totalBytesAcknowledged << std::endl;
            }
        }
        
        //now ack the entire bundle
        if (currentFragmentBytesVec.empty()) {
            if (m_bytesToAckCbVec[readIndex] == totalBytesAcknowledged) {
                ++m_totalDataSegmentsAcked;
                m_totalBytesAcked += m_bytesToAckCbVec[readIndex];
                m_bytesToAckCb.CommitRead();
                if (m_onSuccessfulAckCallback) {
                    m_onSuccessfulAckCallback();
                }
                if (m_useLocalConditionVariableAckReceived) {
                    m_localConditionVariableAckReceived.notify_one();
                }
            }
            else {
                std::cerr << "error in TcpclBundleSource::AckCallback: wrong bytes acked: expected " << m_bytesToAckCbVec[readIndex] << " but got " << totalBytesAcknowledged << std::endl;
            }
        }
    }
    
    
}

void TcpclBundleSource::BundleRefusalCallback(BUNDLE_REFUSAL_CODES refusalCode) {
    std::cout << "error: BundleRefusalCallback not implemented yet in TcpclBundleSource" << std::endl;
}

void TcpclBundleSource::NextBundleLengthCallback(uint64_t nextBundleLength) {
    std::cout << "TcpclBundleSource should never enter NextBundleLengthCallback" << std::endl;
}

void TcpclBundleSource::KeepAliveCallback() {
    std::cout << "In TcpclBundleSource::KeepAliveCallback, received keepalive packet\n";
    // * 2 =>
    //If no message (KEEPALIVE or other) has been received for at least
    //twice the keepalive_interval, then either party MAY terminate the
    //session by transmitting a one-byte SHUTDOWN message (as described in
    //Table 2) and by closing the TCP connection.
    m_noKeepAlivePacketReceivedTimer.expires_from_now(boost::posix_time::seconds(m_keepAliveIntervalSeconds * 2)); //cancels active timer with cancel flag in callback
    m_noKeepAlivePacketReceivedTimer.async_wait(boost::bind(&TcpclBundleSource::OnNoKeepAlivePacketReceived_TimerExpired, this, boost::asio::placeholders::error));
}

void TcpclBundleSource::ShutdownCallback(bool hasReasonCode, SHUTDOWN_REASON_CODES shutdownReasonCode,
                                         bool hasReconnectionDelay, uint64_t reconnectionDelaySeconds)
{
    std::cout << "remote has requested shutdown\n";
    if(hasReasonCode) {
        std::cout << "reason for shutdown: "
                  << ((shutdownReasonCode == SHUTDOWN_REASON_CODES::BUSY) ? "busy" :
                     (shutdownReasonCode == SHUTDOWN_REASON_CODES::IDLE_TIMEOUT) ? "idle timeout" :
                     (shutdownReasonCode == SHUTDOWN_REASON_CODES::VERSION_MISMATCH) ? "version mismatch" :  "unassigned")   << std::endl;
    }
    if(hasReconnectionDelay) {
        m_reconnectionDelaySecondsIfNotZero = reconnectionDelaySeconds;
        std::cout << "requested reconnection delay: " << reconnectionDelaySeconds << " seconds" << std::endl;
    }
    DoTcpclShutdown(false, false);
}

void TcpclBundleSource::OnNoKeepAlivePacketReceived_TimerExpired(const boost::system::error_code& e) {
    if (e != boost::asio::error::operation_aborted) {
        // Timer was not cancelled, take necessary action.
        DoTcpclShutdown(true, true);
    }
    else {
        //std::cout << "timer cancelled\n";
    }
}

void TcpclBundleSource::OnNeedToSendKeepAliveMessage_TimerExpired(const boost::system::error_code& e) {
    if (e != boost::asio::error::operation_aborted) {
        // Timer was not cancelled, take necessary action.
        //SEND KEEPALIVE PACKET
        if(m_tcpSocketPtr) {
            TcpAsyncSenderElement * el = new TcpAsyncSenderElement();
            el->m_underlyingData.resize(1);
            Tcpcl::GenerateKeepAliveMessage(el->m_underlyingData[0]);
            el->m_constBufferVec.emplace_back(boost::asio::buffer(el->m_underlyingData[0])); //only one element so resize not needed
            el->m_onSuccessfulSendCallbackByIoServiceThreadPtr = &m_handleTcpSendCallback;
            m_tcpAsyncSenderPtr->AsyncSend_NotThreadSafe(el); //timer runs in same thread as socket so special thread safety not needed

            m_needToSendKeepAliveMessageTimer.expires_from_now(boost::posix_time::seconds(m_keepAliveIntervalSeconds));
            m_needToSendKeepAliveMessageTimer.async_wait(boost::bind(&TcpclBundleSource::OnNeedToSendKeepAliveMessage_TimerExpired, this, boost::asio::placeholders::error));
        }
    }
    else {
        //std::cout << "timer cancelled\n";
    }
}

void TcpclBundleSource::DoTcpclShutdown(bool sendShutdownMessage, bool reasonWasTimeOut) {
    boost::asio::post(m_ioService, boost::bind(&TcpclBundleSource::DoHandleSocketShutdown, this, sendShutdownMessage, reasonWasTimeOut));
}

void TcpclBundleSource::DoHandleSocketShutdown(bool sendShutdownMessage, bool reasonWasTimeOut) {
    if (!m_shutdownCalled) {
        m_shutdownCalled = true;
        // Timer was cancelled as expected.  This method keeps socket shutdown within io_service thread.

        m_readyToForward = false;
        if (sendShutdownMessage && m_tcpAsyncSenderPtr && m_tcpSocketPtr) {
            std::cout << "Sending shutdown packet to cleanly close tcpcl.. " << std::endl;
            TcpAsyncSenderElement * el = new TcpAsyncSenderElement();
            el->m_underlyingData.resize(1);
            //For the requested delay, in seconds, the value 0 SHALL be interpreted as an infinite delay,
            //i.e., that the connecting node MUST NOT re - establish the connection.
            if (reasonWasTimeOut) {
                Tcpcl::GenerateShutdownMessage(el->m_underlyingData[0], true, SHUTDOWN_REASON_CODES::IDLE_TIMEOUT, true, 0);
            }
            else {
                Tcpcl::GenerateShutdownMessage(el->m_underlyingData[0], false, SHUTDOWN_REASON_CODES::UNASSIGNED, true, 0);
            }

            el->m_constBufferVec.emplace_back(boost::asio::buffer(el->m_underlyingData[0])); //only one element so resize not needed
            el->m_onSuccessfulSendCallbackByIoServiceThreadPtr = &m_handleTcpSendShutdownCallback;
            m_tcpAsyncSenderPtr->AsyncSend_NotThreadSafe(el); //HandleSocketShutdown runs in same thread as socket so special thread safety not needed

            m_sendShutdownMessageTimeoutTimer.expires_from_now(boost::posix_time::seconds(3));
        }
        else {
            m_sendShutdownMessageTimeoutTimer.expires_from_now(boost::posix_time::seconds(0));
        }
        m_sendShutdownMessageTimeoutTimer.async_wait(boost::bind(&TcpclBundleSource::OnSendShutdownMessageTimeout_TimerExpired, this, boost::asio::placeholders::error));
    }
}

void TcpclBundleSource::OnSendShutdownMessageTimeout_TimerExpired(const boost::system::error_code& e) {
    if (e != boost::asio::error::operation_aborted) {
        // Timer was not cancelled, take necessary action.
        std::cout << "Notice: No TCPCL shutdown message was sent (not required)." << std::endl;
    }
    else {
        //std::cout << "timer cancelled\n";
        std::cout << "TCPCL shutdown message was sent." << std::endl;
    }

    //final code to shut down tcp sockets
    if (m_tcpSocketPtr) {
        if (m_tcpSocketPtr->is_open()) {
            try {
                std::cout << "shutting down TcpclBundleSource TCP socket.." << std::endl;
                m_tcpSocketPtr->shutdown(boost::asio::socket_base::shutdown_type::shutdown_both);
            }
            catch (const boost::system::system_error & e) {
                std::cerr << "error in TcpclBundleSource::OnSendShutdownMessageTimeout_TimerExpired: " << e.what() << std::endl;
            }
            try {
                std::cout << "closing TcpclBundleSource TCP socket socket.." << std::endl;
                m_tcpSocketPtr->close();
            }
            catch (const boost::system::system_error & e) {
                std::cerr << "error in TcpclBundleSource::OnSendShutdownMessageTimeout_TimerExpired: " << e.what() << std::endl;
            }
        }
        //don't delete the tcp socket or async sender because the Forward function is multi-threaded without a mutex to
        //increase speed, so prevent a race condition that would cause a null pointer exception
        //std::cout << "deleting tcp socket" << std::endl;
        //m_tcpSocketPtr = boost::shared_ptr<boost::asio::ip::tcp::socket>();
    }
    m_needToSendKeepAliveMessageTimer.cancel();
    m_noKeepAlivePacketReceivedTimer.cancel();
    m_tcpcl.InitRx(); //reset states
    m_tcpclShutdownComplete = true;
    if (m_reconnectionDelaySecondsIfNotZero) {
        m_reconnectAfterShutdownTimer.expires_from_now(boost::posix_time::seconds(m_reconnectionDelaySecondsIfNotZero));
        m_reconnectAfterShutdownTimer.async_wait(boost::bind(&TcpclBundleSource::OnNeedToReconnectAfterShutdown_TimerExpired, this, boost::asio::placeholders::error));
    }
}

void TcpclBundleSource::OnNeedToReconnectAfterShutdown_TimerExpired(const boost::system::error_code& e) {
    if (e != boost::asio::error::operation_aborted) {
        // Timer was not cancelled, take necessary action.
        std::cout << "Trying to reconnect..." << std::endl;
        m_tcpAsyncSenderPtr.reset();
        m_tcpSocketPtr = boost::make_shared<boost::asio::ip::tcp::socket>(m_ioService);
        m_shutdownCalled = false;
        boost::asio::async_connect(
            *m_tcpSocketPtr,
            m_resolverResults,
            boost::bind(
                &TcpclBundleSource::OnConnect,
                this,
                boost::asio::placeholders::error));
    }
}

bool TcpclBundleSource::ReadyToForward() {
    return m_readyToForward;
}

void TcpclBundleSource::SetOnSuccessfulAckCallback(const OnSuccessfulAckCallback_t & callback) {
    m_onSuccessfulAckCallback = callback;
}
