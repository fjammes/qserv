// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 LSST Corporation.
 *
 * This product includes software developed by the
 * LSST Project (http://www.lsst.org/).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the LSST License Statement and
 * the GNU General Public License along with this program.  If not,
 * see <http://www.lsstcorp.org/LegalNotices/>.
 *
 */
#ifndef LSST_QSERV_LOADER_MWORKERLIST_H_
#define LSST_QSERV_LOADER_MWORKERLIST_H_

// system headers
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

// Qserv headers
#include "loader/Updateable.h"
#include "loader/BufferUdp.h"
#include "loader/DoList.h"
#include "loader/NetworkAddress.h"
#include "loader/StringRange.h"


namespace lsst {
namespace qserv {
namespace loader {

class Central;
class CentralWorker;
class CentralMaster;
class LoaderMsg;


/// Standard information for a single worker, IP address, key range, timeouts.
class MWorkerListItem : public std::enable_shared_from_this<MWorkerListItem> {
public:
    using Ptr = std::shared_ptr<MWorkerListItem>;
    using WPtr = std::weak_ptr<MWorkerListItem>;

    static MWorkerListItem::Ptr create(uint32_t name, CentralMaster *central) {
        return MWorkerListItem::Ptr(new MWorkerListItem(name, central));
    }
    static MWorkerListItem::Ptr create(uint32_t name, NetworkAddress const& udpAddress,
                                       NetworkAddress const& tcpAddress, CentralMaster *central) {
        return MWorkerListItem::Ptr(new MWorkerListItem(name, udpAddress, tcpAddress, central));
    }


    MWorkerListItem() = delete;
    MWorkerListItem(MWorkerListItem const&) = delete;
    MWorkerListItem& operator=(MWorkerListItem const&) = delete;

    virtual ~MWorkerListItem() = default;

    NetworkAddress getUdpAddress() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return *_udpAddress;
    }

    NetworkAddress getTcpAddress() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return *_tcpAddress;
    }

    uint32_t getId() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return _wId;
    }

    StringRange getRangeString() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return _range;
    }

    bool isActive() const { return _active; }
    void setActive(bool val) { _active = val; }

    void addDoListItems(Central *central);

    void setRangeStr(StringRange const& strRange);
    void setAllInclusiveRange();

    void setNeighborsInfo(NeighborsInfo const& nInfo);
    int  getKeyCount() const;

    void setRightNeighbor(MWorkerListItem::Ptr const& item);
    void setLeftNeighbor(MWorkerListItem::Ptr const& item);

    void flagNeedToSendList();

    void sendListToWorkerInfoReceived();

    friend std::ostream& operator<<(std::ostream& os, MWorkerListItem const& item);
private:
    MWorkerListItem(uint32_t wId, CentralMaster* central) : _wId(wId), _central(central) {}
    MWorkerListItem(uint32_t wId,
                    NetworkAddress const& udpAddress,
                    NetworkAddress const& tcpAddress,
                    CentralMaster* central)
         : _wId(wId),
           _udpAddress(new NetworkAddress(udpAddress)),
           _tcpAddress(new NetworkAddress(tcpAddress)),
           _central(central) {}

    uint32_t _wId; ///< Worker Id
    NetworkAddress::UPtr _udpAddress{new NetworkAddress("", 0)}; ///< empty string indicates address is not valid.
    NetworkAddress::UPtr _tcpAddress{new NetworkAddress("", 0)}; ///< empty string indicates address is not valid.
    TimeOut _lastContact{std::chrono::minutes(10)};  ///< Last time information was received from this worker
    StringRange _range;       ///< min and max range for this worker.
    NeighborsInfo _neighborsInfo; ///< information used to set neighbors.
    mutable std::mutex _mtx;  ///< protects _name, _address, _range

    std::atomic<bool> _active{false}; ///< true when worker has been given a valid range, or a neighbor.

    CentralMaster* _central;

    // Occasionally send a list of all workers to the worker represented by this object.
    struct SendListToWorker : public DoListItem {
        SendListToWorker(MWorkerListItem::Ptr const& mWorkerListItem_, CentralMaster *central_) :
            mWorkerListItem(mWorkerListItem_), central(central_) {}
        MWorkerListItem::WPtr mWorkerListItem;
        CentralMaster *central;
        util::CommandTracked::Ptr createCommand() override;
    };
    DoListItem::Ptr _sendListToWorker;

    // Occasionally ask this worker for information about its list of keys, if it hasn't
    // been heard from.
    struct ReqWorkerKeyInfo : public DoListItem {
        ReqWorkerKeyInfo(MWorkerListItem::Ptr const& mWorkerListItem_, CentralMaster *central_) :
            mWorkerListItem(mWorkerListItem_), central(central_) {}
        MWorkerListItem::WPtr mWorkerListItem;
        CentralMaster *central;
        util::CommandTracked::Ptr createCommand() override;
    };
    DoListItem::Ptr _reqWorkerKeyInfo;
    std::mutex _doListItemsMtx; ///< protects _sendListToWorker

};


class MWorkerList : public DoListItem {
public:
    using Ptr = std::shared_ptr<MWorkerList>;

    MWorkerList(CentralMaster* central) : _central(central) {} // MUST be created as shared pointer.
    MWorkerList() = delete;
    MWorkerList(MWorkerList const&) = delete;
    MWorkerList& operator=(MWorkerList const&) = delete;

    virtual ~MWorkerList() = default;

    ///// Master only //////////////////////
    // Returns pointer to new item if an item was created.
    MWorkerListItem::Ptr addWorker(std::string const& ip, int udpPort, int tcpPort);

    /// Returns true of message could be parsed and a send will be attempted.
    /// It sends a list of worker names. The worker then asks for each name individually
    ///  to get ips, ports, and ranges.
    bool sendListTo(uint64_t msgId, std::string const& ip, short port,
                    std::string const& outHostName, short ourPort);

    util::CommandTracked::Ptr createCommand() override;
    util::CommandTracked::Ptr createCommandMaster(CentralMaster* centralM);

    //////////////////////////////////////////
    /// Nearly the same on Worker and Master
    size_t getNameMapSize() {
        std::lock_guard<std::mutex> lck(_mapMtx);
        return _wIdMap.size();
    }

    MWorkerListItem::Ptr getWorkerWithId(uint32_t id) {
        std::lock_guard<std::mutex> lck(_mapMtx);
        auto iter = _wIdMap.find(id);
        if (iter == _wIdMap.end()) { return nullptr; }
        return iter->second;
    }

    /// @Return 2 lists. One of active workers, one of inactive workers. Both lists are copies.
    std::pair<std::vector<MWorkerListItem::Ptr>, std::vector<MWorkerListItem::Ptr>>
        getActiveInactiveWorkerLists();

    std::string dump() const;

protected:
    void _flagListChange();

    CentralMaster* _central;
    std::map<uint32_t, MWorkerListItem::Ptr> _wIdMap;
    std::map<NetworkAddress, MWorkerListItem::Ptr> _ipMap;
    bool _wListChanged{false}; ///< true if the list has changed
    BufferUdp::Ptr _stateListData; ///< message
    uint32_t _totalNumberOfWorkers{0}; ///< total number of workers according to the master.
    mutable std::mutex _mapMtx; ///< protects _nameMap, _ipMap, _wListChanged

    std::atomic<uint32_t> _sequenceName{1}; ///< Source of names for workers. 0 is invalid name.

};


}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_MWORKERLIST_H_