#ifndef _HDTN_INGRESS_H
#define _HDTN_INGRESS_H

#include <stdint.h>

#include "message.hpp"
//#include "util/tsc.h"
#include "zmq.hpp"

#include "CircularIndexBufferSingleProducerSingleConsumerConfigurable.h"
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include "HdtnConfig.h"
#include "InductManager.h"
#include <list>
#include <unordered_set>
#include <queue>
#include <boost/atomic.hpp>
#include "TcpclInduct.h"
#include "TcpclV4Induct.h"
#include "Telemetry.h"
#include "ingress_async_lib_export.h"

namespace hdtn {

typedef struct IngressTelemetry {
    uint64_t totalBundles;
    uint64_t totalBytes;
    uint64_t totalZmsgsIn;
    uint64_t totalZmsgsOut;
    uint64_t bundlesSecIn;
    uint64_t mBitsSecIn;
    uint64_t zmsgsSecIn;
    uint64_t zmsgsSecOut;
    double elapsed;
} IngressTelemetry;

class Ingress {
public:
    INGRESS_ASYNC_LIB_EXPORT Ingress();  // initialize message buffers
    INGRESS_ASYNC_LIB_EXPORT ~Ingress();
    INGRESS_ASYNC_LIB_EXPORT void Stop();
    INGRESS_ASYNC_LIB_EXPORT void SchedulerEventHandler();
    INGRESS_ASYNC_LIB_EXPORT int Init(const HdtnConfig & hdtnConfig, zmq::context_t * hdtnOneProcessZmqInprocContextPtr = NULL);
private:
    INGRESS_ASYNC_LIB_NO_EXPORT bool ProcessPaddedData(uint8_t * bundleDataBegin, std::size_t bundleCurrentSize,
        std::unique_ptr<zmq::message_t> & zmqPaddedMessageUnderlyingDataUniquePtr, padded_vector_uint8_t & paddedVecMessageUnderlyingData, const bool usingZmqData, const bool needsProcessing);
    INGRESS_ASYNC_LIB_NO_EXPORT void ReadZmqAcksThreadFunc();
    INGRESS_ASYNC_LIB_NO_EXPORT void ReadTcpclOpportunisticBundlesFromEgressThreadFunc();
    INGRESS_ASYNC_LIB_NO_EXPORT void WholeBundleReadyCallback(padded_vector_uint8_t & wholeBundleVec);
    INGRESS_ASYNC_LIB_NO_EXPORT void OnNewOpportunisticLinkCallback(const uint64_t remoteNodeId, Induct * thisInductPtr);
    INGRESS_ASYNC_LIB_NO_EXPORT void OnDeletedOpportunisticLinkCallback(const uint64_t remoteNodeId);
    INGRESS_ASYNC_LIB_NO_EXPORT void SendOpportunisticLinkMessages(const uint64_t remoteNodeId, bool isAvailable);
public:
    uint64_t m_bundleCountStorage;
    boost::atomic_uint64_t m_bundleCountEgress;
    uint64_t m_bundleCount;
    boost::atomic_uint64_t m_bundleData;
    double m_elapsed;

private:
    struct EgressToIngressAckingSet {
        EgressToIngressAckingSet() {
            //By default, unordered_set containers have a max_load_factor of 1.0.
            m_ingressToEgressCustodyIdSet.reserve(500); //TODO
        }
        std::size_t GetSetSize() {
            return m_ingressToEgressCustodyIdSet.size();
        }
        void PushMove_ThreadSafe(const uint64_t ingressToEgressCustody) {
            boost::mutex::scoped_lock lock(m_mutex);
            m_ingressToEgressCustodyIdSet.emplace(ingressToEgressCustody);
        }
        bool CompareAndPop_ThreadSafe(const uint64_t ingressToEgressCustody) {
            m_mutex.lock();
            const std::size_t retVal = m_ingressToEgressCustodyIdSet.erase(ingressToEgressCustody);
            m_mutex.unlock();
            return (retVal != 0);
        }
        void WaitUntilNotifiedOr250MsTimeout() {
            boost::mutex::scoped_lock lock(m_mutex);
            m_conditionVariable.timed_wait(lock, boost::posix_time::milliseconds(250)); // call lock.unlock() and blocks the current thread
        }
        void NotifyAll() {            
            m_conditionVariable.notify_all();
        }
        boost::mutex m_mutex;
        boost::condition_variable m_conditionVariable;
        std::unordered_set<uint64_t> m_ingressToEgressCustodyIdSet;
    };

    std::unique_ptr<zmq::context_t> m_zmqCtxPtr;
    std::unique_ptr<zmq::socket_t> m_zmqPushSock_boundIngressToConnectingEgressPtr;
    std::unique_ptr<zmq::socket_t> m_zmqPullSock_connectingEgressToBoundIngressPtr;
    std::unique_ptr<zmq::socket_t> m_zmqPullSock_connectingEgressBundlesOnlyToBoundIngressPtr;
    std::unique_ptr<zmq::socket_t> m_zmqPushSock_boundIngressToConnectingStoragePtr;
    std::unique_ptr<zmq::socket_t> m_zmqPullSock_connectingStorageToBoundIngressPtr;
    std::unique_ptr<zmq::socket_t> m_zmqSubSock_boundSchedulerToConnectingIngressPtr;

    std::unique_ptr<zmq::socket_t> m_zmqRepSock_connectingGuiToFromBoundIngressPtr;

    //std::shared_ptr<zmq::context_t> m_zmqTelemCtx;
    //std::shared_ptr<zmq::socket_t> m_zmqTelemSock;

    InductManager m_inductManager;
    HdtnConfig m_hdtnConfig;
    cbhe_eid_t M_HDTN_EID_CUSTODY;
    cbhe_eid_t M_HDTN_EID_ECHO;
    boost::posix_time::time_duration M_MAX_INGRESS_BUNDLE_WAIT_ON_EGRESS_TIME_DURATION;
    
    std::unique_ptr<boost::thread> m_threadZmqAckReaderPtr;
    std::unique_ptr<boost::thread> m_threadTcpclOpportunisticBundlesFromEgressReaderPtr;
    std::queue<uint64_t> m_storageAckQueue;
    boost::mutex m_storageAckQueueMutex;
    boost::condition_variable m_conditionVariableStorageAckReceived;
    std::map<uint64_t, EgressToIngressAckingSet> m_egressAckMapSet; //final dest node id to set
    boost::mutex m_egressAckMapSetMutex;
    boost::mutex m_ingressToEgressZmqSocketMutex;
    boost::mutex m_eidAvailableSetMutex;
    std::size_t m_eventsTooManyInStorageQueue;
    std::size_t m_eventsTooManyInEgressQueue;
    volatile bool m_running;
    boost::atomic_uint64_t m_ingressToEgressNextUniqueIdAtomic;
    uint64_t m_ingressToStorageNextUniqueId;
    std::set<cbhe_eid_t> m_finalDestEidAvailableSet;
    std::set<uint64_t> m_finalDestNodeIdAvailableSet;
    std::vector<uint64_t> m_schedulerRxBufPtrToStdVec64;

    std::map<uint64_t, Induct*> m_availableDestOpportunisticNodeIdToTcpclInductMap;
    boost::mutex m_availableDestOpportunisticNodeIdToTcpclInductMapMutex;
};


}  // namespace hdtn

#endif  //_HDTN_INGRESS_H
