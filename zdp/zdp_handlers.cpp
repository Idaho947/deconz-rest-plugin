/*
 * Copyright (c) 2021 dresden elektronik ingenieurtechnik gmbh.
 * All rights reserved.
 *
 * The software in this package is published under the terms of the BSD
 * style license a copy of which has been included with this distribution in
 * the LICENSE.txt file.
 *
 */

#include "de_web_plugin.h"
#include "de_web_plugin_private.h"
#include "zdp_handlers.h"

/*! Handle the case that a node (re)joins the network.
    \param ind a ZDP DeviceAnnce_req
 */
void DeRestPluginPrivate::handleDeviceAnnceIndication(const deCONZ::ApsDataIndication &ind)
{
    std::vector<LightNode>::iterator i = nodes.begin();
    std::vector<LightNode>::iterator end = nodes.end();

    quint16 nwk;
    quint64 ext;
    quint8 macCapabilities;

    {
        QDataStream stream(ind.asdu());
        stream.setByteOrder(QDataStream::LittleEndian);

        quint8 seq;

        stream >> seq;
        stream >> nwk;
        stream >> ext;
        stream >> macCapabilities;
    }

    for (; i != end; ++i)
    {
        if (i->state() != LightNode::StateNormal)
        {
            continue;
        }

        if (i->address().ext() == ext)
        {
            i->rx();
            i->setValue(RAttrLastAnnounced, i->lastRx().toUTC());

            // clear to speedup polling
            for (NodeValue &val : i->zclValues())
            {
                val.timestamp = QDateTime();
                val.timestampLastReport = QDateTime();
                val.timestampLastConfigured = QDateTime();
            }

            i->setLastAttributeReportBind(0);

            std::vector<RecoverOnOff>::iterator rc = recoverOnOff.begin();
            std::vector<RecoverOnOff>::iterator rcend = recoverOnOff.end();
            for (; rc != rcend; ++rc)
            {
                if (rc->address.ext() == ext || rc->address.nwk() == nwk)
                {
                    rc->idleTotalCounterCopy -= 60; // speedup release
                    // light was off before, turn off again
                    if (!rc->onOff)
                    {
                        DBG_Printf(DBG_INFO, "Turn off light 0x%016llX again after powercycle\n", rc->address.ext());
                        TaskItem task;
                        task.lightNode = &*i;
                        task.req.dstAddress().setNwk(nwk);
                        task.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
                        task.req.setDstEndpoint(task.lightNode->haEndpoint().endpoint());
                        task.req.setSrcEndpoint(getSrcEndpoint(task.lightNode, task.req));
                        task.req.setDstAddressMode(deCONZ::ApsNwkAddress);
                        task.req.setSendDelay(1000);
                        queryTime = queryTime.addSecs(5);
                        addTaskSetOnOff(task, ONOFF_COMMAND_OFF, 0);
                    }
                    else if (rc->bri > 0 && rc->bri < 256)
                    {
                        DBG_Printf(DBG_INFO, "Turn on light 0x%016llX on again with former brightness after powercycle\n", rc->address.ext());
                        TaskItem task;
                        task.lightNode = &*i;
                        task.req.dstAddress().setNwk(nwk);
                        task.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
                        task.req.setDstEndpoint(task.lightNode->haEndpoint().endpoint());
                        task.req.setSrcEndpoint(getSrcEndpoint(task.lightNode, task.req));
                        task.req.setDstAddressMode(deCONZ::ApsNwkAddress);
                        task.req.setSendDelay(1000);
                        queryTime = queryTime.addSecs(5);
                        addTaskSetBrightness(task, rc->bri, true);
                    }
                    break;
                }
            }

            deCONZ::Node *node = i->node();
            if (node && node->endpoints().end() == std::find(node->endpoints().begin(),
                                                     node->endpoints().end(),
                                                     i->haEndpoint().endpoint()))
            {
                continue; // not a active endpoint
            }

            ResourceItem *item = i->item(RStateReachable);

            if (item)
            {
                item->setValue(true); // refresh timestamp after device announce
                if (i->state() == LightNode::StateNormal)
                {
                    Event e(i->prefix(), RStateReachable, i->id(), item);
                    enqueueEvent(e);
                }

                updateEtag(gwConfigEtag);
            }

            DBG_Printf(DBG_INFO, "DeviceAnnce of LightNode: %s Permit Join: %i\n", qPrintable(i->address().toStringExt()), gwPermitJoinDuration);

            // force reading attributes
            i->enableRead(READ_GROUPS | READ_SCENES);

            // bring to front to force next polling
            const PollNodeItem pollItem(i->uniqueId(), i->prefix());
            pollNodes.push_front(pollItem);

            for (uint32_t ii = 0; ii < 32; ii++)
            {
                uint32_t item = 1 << ii;
                if (i->mustRead(item))
                {
                    i->setNextReadTime(item, queryTime);
                    i->setLastRead(item, idleTotalCounter);
                }
            }

            queryTime = queryTime.addSecs(1);
            updateEtag(i->etag);

        }
    }

    int found = 0;
    std::vector<Sensor>::iterator si = sensors.begin();
    std::vector<Sensor>::iterator send = sensors.end();

    for (; si != send; ++si)
    {
        if (si->deletedState() != Sensor::StateNormal)
        {
            continue;
        }

        if (si->address().ext() == ext)
        {
            si->rx();
            found++;
            DBG_Printf(DBG_INFO, "DeviceAnnce of SensorNode: 0x%016llX [1]\n", si->address().ext());

            ResourceItem *item = si->item(RConfigReachable);
            if (item)
            {
                item->setValue(true); // refresh timestamp after device announce
                Event e(si->prefix(), RConfigReachable, si->id(), item);
                enqueueEvent(e);
            }
            checkSensorGroup(&*si);
            checkSensorBindingsForAttributeReporting(&*si);
            checkSensorBindingsForClientClusters(&*si);
            updateSensorEtag(&*si);

            if (searchSensorsState == SearchSensorsActive && si->node())
            {
                // address changed?
                if (si->address().nwk() != nwk)
                {
                    DBG_Printf(DBG_INFO, "\tnwk address changed 0x%04X -> 0x%04X [2]\n", si->address().nwk(), nwk);
                    // indicator that the device was resettet
                    si->address().setNwk(nwk);

                    if (searchSensorsState == SearchSensorsActive &&
                        si->deletedState() == Sensor::StateNormal)
                    {
                        updateSensorEtag(&*si);
                        Event e(RSensors, REventAdded, si->id());
                        enqueueEvent(e);
                    }
                }

                // clear to speedup polling
                for (NodeValue &val : si->zclValues())
                {
                    val.timestamp = QDateTime();
                    val.timestampLastReport = QDateTime();
                    val.timestampLastConfigured = QDateTime();
                }

                addSensorNode(si->node()); // check if somethings needs to be updated
            }

            if (si->type() == QLatin1String("ZHATime"))
            {
                if (!si->mustRead(READ_TIME))
                {
                    DBG_Printf(DBG_INFO, "  >>> %s sensor %s: set READ_TIME from handleDeviceAnnceIndication()\n", qPrintable(si->type()), qPrintable(si->name()));
                    si->enableRead(READ_TIME);
                    si->setLastRead(READ_TIME, idleTotalCounter);
                    si->setNextReadTime(READ_TIME, queryTime);
                    queryTime = queryTime.addSecs(1);
                }
            }
        }
    }

    if (searchSensorsState == SearchSensorsActive)
    {
        if (!found && apsCtrl)
        {
            int i = 0;
            const deCONZ::Node *node;

            // try to add sensor nodes even if they existed in deCONZ bevor and therefore
            // no node added event will be triggert in this phase
            while (apsCtrl->getNode(i, &node) == 0)
            {
                if (ext == node->address().ext())
                {
                    addSensorNode(node);
                    break;
                }
                i++;
            }
        }

        deCONZ::ZclFrame zclFrame; // dummy
        handleIndicationSearchSensors(ind, zclFrame);
    }
}

/*! Handle node descriptor response.
    \param ind a ZDP NodeDescriptor_rsp
 */
void DeRestPluginPrivate::handleNodeDescriptorResponseIndication(const deCONZ::ApsDataIndication &ind)
{
    patchNodeDescriptor(ind);
}

/*! Handle mgmt lqi response.
    \param ind a ZDP MgmtLqi_rsp
 */
void DeRestPluginPrivate::handleMgmtLqiRspIndication(const deCONZ::ApsDataIndication &ind)
{
    quint8 zdpSeq;
    quint8 zdpStatus;
    quint8 neighEntries;
    quint8 startIndex;
    quint8 listCount;

    QDataStream stream(ind.asdu());
    stream.setByteOrder(QDataStream::LittleEndian);

    stream >> zdpSeq;
    stream >> zdpStatus;
    stream >> neighEntries;
    stream >> startIndex;
    stream >> listCount;

    if (stream.status() == QDataStream::ReadPastEnd)
    {
        return;
    }

    if ((startIndex + listCount) >= neighEntries || listCount == 0)
    {
        // finish
        for (LightNode &l : nodes)
        {
            if (l.address().ext() == ind.srcAddress().ext())
            {
                l.rx();
            }
        }
    }
}

/*! Handle IEEE address request indication.
    \param ind a ZDP IeeeAddress_req
 */
void DeRestPluginPrivate::handleIeeeAddressReqIndication(const deCONZ::ApsDataIndication &ind)
{
    if (!apsCtrl)
    {
        return;
    }

    quint8 seq;
    quint64 extAddr;
    quint16 nwkAddr;
    quint8 reqType;
    quint8 startIndex;

    {
        QDataStream stream(ind.asdu());
        stream.setByteOrder(QDataStream::LittleEndian);

        stream >> seq;
        stream >> nwkAddr;
        stream >> reqType;
        stream >> startIndex;
    }

    if (nwkAddr != apsCtrl->getParameter(deCONZ::ParamNwkAddress))
    {
        return;
    }

    deCONZ::ApsDataRequest req;

    req.setProfileId(ZDP_PROFILE_ID);
    req.setSrcEndpoint(ZDO_ENDPOINT);
    req.setDstEndpoint(ZDO_ENDPOINT);
    req.setClusterId(ZDP_IEEE_ADDR_RSP_CLID);
    req.setDstAddressMode(deCONZ::ApsNwkAddress);
    req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
    req.dstAddress() = ind.srcAddress();

    QDataStream stream(&req.asdu(), QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);

    extAddr = apsCtrl->getParameter(deCONZ::ParamMacAddress);

    quint8 status = ZDP_SUCCESS;
    stream << seq;
    stream << status;
    stream << extAddr;
    stream << nwkAddr;

    if (reqType == 0x01) // extended request type
    {
        stream << (quint8)0; // num of assoc devices
        stream << (quint8)0; // start index
    }

    if (apsCtrlWrapper.apsdeDataRequest(req) == deCONZ::Success)
    {

    }
}

/*! Handle NWK address request indication.
    \param ind a ZDP NwkAddress_req
 */
void DeRestPluginPrivate::handleNwkAddressReqIndication(const deCONZ::ApsDataIndication &ind)
{
    if (!apsCtrl)
    {
        return;
    }

    quint8 seq;
    quint16 nwkAddr;
    quint64 extAddr;
    quint8 reqType;
    quint8 startIndex;

    {
        QDataStream stream(ind.asdu());
        stream.setByteOrder(QDataStream::LittleEndian);

        stream >> seq;
        stream >> extAddr;
        stream >> reqType;
        stream >> startIndex;
    }

    if (extAddr != apsCtrl->getParameter(deCONZ::ParamMacAddress))
    {
        return;
    }

    deCONZ::ApsDataRequest req;

    req.setProfileId(ZDP_PROFILE_ID);
    req.setSrcEndpoint(ZDO_ENDPOINT);
    req.setDstEndpoint(ZDO_ENDPOINT);
    req.setClusterId(ZDP_NWK_ADDR_RSP_CLID);
    req.setDstAddressMode(deCONZ::ApsNwkAddress);
    req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
    req.dstAddress() = ind.srcAddress();

    QDataStream stream(&req.asdu(), QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);

    nwkAddr = apsCtrl->getParameter(deCONZ::ParamNwkAddress);
    quint8 status = ZDP_SUCCESS;
    stream << seq;
    stream << status;
    stream << extAddr;
    stream << nwkAddr;

    if (reqType == 0x01) // extended request type
    {
        stream << (quint8)0; // num of assoc devices
        stream << (quint8)0; // start index
    }

    if (apsCtrlWrapper.apsdeDataRequest(req) == deCONZ::Success)
    {

    }
}
