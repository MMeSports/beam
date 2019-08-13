// Copyright 2019 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "wallet/laser/mediator.h"

#include "core/negotiator.h"
#include "core/lightning_codes.h"
#include "wallet/common_utils.h"
#include "wallet/laser/connection.h"
#include "wallet/laser/receiver.h"
#include "utility/helpers.h"
#include "utility/logger.h"

namespace
{
    const beam::Height kSafeForgetHeight = 8;

    bool IsValidTimeStamp(beam::Timestamp currentBlockTime_s)
    {
        beam::Timestamp currentTime_s = beam::getTimestamp();
        const beam::Timestamp tolerance_s = 60 * 8;
        currentBlockTime_s += tolerance_s;

        if (currentTime_s > currentBlockTime_s)
        {
            LOG_INFO() << "It seems that node is not up to date";
            return false;
        }
        return true;
    }

}  // namespace

namespace beam::wallet::laser
{
Mediator::Mediator(const IWalletDB::Ptr& walletDB)
    : m_pWalletDB(walletDB)
{
    m_myInAddr.m_walletID = Zero;
}

Mediator::~Mediator()
{

}

void Mediator::OnNewTip()
{
    Block::SystemState::Full tip;
    get_History().get_Tip(tip);
    if (!IsValidTimeStamp(tip.m_TimeStamp))
    {
        return;
    }

    Block::SystemState::ID id;
    if (tip.m_Height)
        tip.get_ID(id);
    else
        ZeroObject(id);
    m_pWalletDB->setSystemStateID(id);
    LOG_INFO() << "LASER current state is " << id;

    for (auto& openIt : m_openQueue)
    {
        openIt();
    }
    m_openQueue.clear();

    UpdateChannels();

    for (const auto& readyForCloseChannel : m_readyForForgetChannels)
    {
        ForgetChannel(readyForCloseChannel);
        LOG_INFO() << "ForgetChannel: "
                   << to_hex(readyForCloseChannel->m_pData,
                             readyForCloseChannel->nBytes);
    }
    m_readyForForgetChannels.clear();
}

void Mediator::OnRolledBack()
{
    LOG_DEBUG() << "LASER OnRolledBack";
    for (auto& it: m_channels)
    {
        auto& ch = it.second;
        ch->OnRolledBack();
        if (ch->TransformLastState())
        {
            ch->UpdateRestorePoint();
            m_pWalletDB->saveLaserChannel(*ch);
        }
    }
}

Block::SystemState::IHistory& Mediator::get_History()
{
    return m_pWalletDB->get_History();
}

void Mediator::OnOwnedNode(const PeerID&, bool bUp)
{

}

IWalletDB::Ptr Mediator::getWalletDB()
{
    return m_pWalletDB;
}

proto::FlyClient::INetwork& Mediator::get_Net()
{
    return *m_pConnection;
}

void Mediator::OnMsg(const ChannelIDPtr& chID, Blob&& blob)
{
    auto inChID = std::make_shared<ChannelID>(Zero);
	beam::Negotiator::Storage::Map dataIn;

	try
    {
		Deserializer der;
		der.reset(blob.p, blob.n);

		der & (*inChID);
		der & Cast::Down<FieldMap>(dataIn);
	}
	catch (const std::exception&)
    {
		return;
	}

    if (!chID && m_pInputReceiver)
    {
        LOG_INFO() << "LASER Create incoming: "
                   << to_hex(inChID->m_pData , inChID->nBytes);
        OnIncoming(inChID, dataIn);
    }
    else
    {
        if (*chID != *inChID)
        {
            LOG_ERROR() << "Unknown channel ID: "
                        << to_hex(inChID->m_pData , inChID->nBytes);
            return;
        }
        inChID = chID;
    }

    auto it = m_channels.find(inChID);
    if (it == m_channels.end())
    {
        LOG_ERROR() << "Channel not found ID: "
                    << to_hex(inChID->m_pData , inChID->nBytes);
        return;
    }
    auto& ch = it->second;

    ch->OnPeerData(dataIn);
    UpdateChannelExterior(ch);
    ch->LogNewState();
}

bool  Mediator::Decrypt(const ChannelIDPtr& chID, uint8_t* pMsg, Blob* blob)
{
    if (!proto::Bbs::Decrypt(pMsg, blob->n, get_skBbs(chID)))
		return false;

	blob->p = pMsg;
    return true;
}

void Mediator::SetNetwork(const proto::FlyClient::NetworkStd::Ptr& net)
{
    m_pConnection = std::make_shared<Connection>(net);
}

void Mediator::WaitIncoming(Amount aMy, Amount fee)
{
    m_myInAllowed = aMy;
    m_feeAllowed = fee;

    m_pInputReceiver = std::make_unique<Receiver>(*this, nullptr);
    m_myInAddr = GenerateNewAddress(
        m_pWalletDB,
        "laser_in",
        WalletAddress::ExpirationStatus::Never,
        false);

    BbsChannel ch;
    m_myInAddr.m_walletID.m_Channel.Export(ch);
    m_pConnection->BbsSubscribe(ch, getTimestamp(), m_pInputReceiver.get());
    LOG_DEBUG() << "LASER WAIT IN subscribed: " << ch;
}

bool Mediator::Serve(const std::vector<std::string>& channelIDsStr)
{
    uint64_t count = 0;
    for (const auto& channelIDStr: channelIDsStr)
    {
        LOG_DEBUG() << "Channel: " << channelIDStr << " restore process started";
        auto chId = RestoreChannel(channelIDStr);
        if (chId)
        {
            LOG_DEBUG() << "Channel: " << channelIDStr
                        << " restore process finished";
            auto& ch = m_channels[chId];
            if (ch && ch->CanBeServed())
            {
                ch->Subscribe();
                ++count;
            }
        }
    }
    LOG_INFO() << "LASER serve: " << count << " channels";
    return count != 0;
}

void Mediator::OpenChannel(Amount aMy,
                           Amount aTrg,
                           Amount fee,
                           const WalletID& receiverWalletID,
                           Height locktime)
{
    auto myOutAddr = GenerateNewAddress(
            m_pWalletDB,
            "laser_out",
            WalletAddress::ExpirationStatus::Never,
            false);        

    auto channel = std::make_unique<Channel>(
        *this, myOutAddr, receiverWalletID,
        fee, aMy, aTrg, locktime);
    channel->Subscribe();

    auto chID = channel->get_chID();
    m_channels[chID] = std::move(channel);
    
    m_openQueue.emplace_back([this, chID] () {
        OpenInternal(chID);
    });
}

void Mediator::Close(const std::vector<std::string>& channelIDsStr)
{
    for (const auto& chIdStr: channelIDsStr)
    {
        auto chId = RestoreChannel(chIdStr);
        if (chId)
        {
            auto& ch = m_channels[chId];
            if (ch && ch->CanBeClosed())
            {
                ch->Close();
                if (ch->TransformLastState())
                {
                    m_pWalletDB->saveLaserChannel(*ch);
                    ch->LogNewState();
                    if (!ch->IsNegotiating() &&
                        ch->IsSafeToForget(kSafeForgetHeight))
                    {
                        m_readyForForgetChannels.push_back(ch->get_chID());
                    }
                }
            }   
        }
    }
}

void Mediator::Delete(const std::vector<std::string>& channelIDsStr)
{
    for (const auto& chIdStr: channelIDsStr)
    {
        auto chId = RestoreChannel(chIdStr);
        if (chId)
        {
            auto& ch = m_channels[chId];
            if (ch &&
                ch->get_State() == Lightning::Channel::State::Closed &&
                m_pWalletDB->removeLaserChannel(*ch))
            {
                LOG_INFO() << "Channel: "
                           << to_hex(chId->m_pData, chId->nBytes) << " deleted";
            }
            else
            {
                LOG_ERROR() << "Channel: "
                            << to_hex(chId->m_pData, chId->nBytes)
                            << " not found or not closed";
            }
            
        }
    }
}

size_t Mediator::getChannelsCount() const
{
    return m_channels.size();
}

void Mediator::AddObserver(Observer* observer)
{
    m_observers.push_back(observer);
}

void Mediator::RemoveObserver(Observer* observer)
{
    m_observers.erase(
        m_observers.begin(),
        std::remove(m_observers.begin(), m_observers.end(), observer));
}

bool Mediator::Transfer(Amount amount, const std::string& channelIDStr)
{
    auto chId = RestoreChannel(channelIDStr);

    auto& ch = m_channels[chId];
    return ch ? ch->Transfer(amount) : false;
}

ECC::Scalar::Native Mediator::get_skBbs(const ChannelIDPtr& chID)
{
    auto& addr = chID ? m_channels[chID]->get_myAddr() : m_myInAddr;
    auto& wid = addr.m_walletID;
    if (wid != Zero)
    {    
        PeerID peerID;
        ECC::Scalar::Native sk;
        m_pWalletDB->get_MasterKdf()->DeriveKey(
            sk, Key::ID(addr.m_OwnID, Key::Type::Bbs));
        proto::Sk2Pk(peerID, sk);
        return wid.m_Pk == peerID ? sk : Zero;        
    }
    return Zero;
}

void Mediator::OnIncoming(const ChannelIDPtr& chID,
                          Negotiator::Storage::Map& dataIn)
{
    WalletID trgWid;
    if (!dataIn.Get(trgWid, Channel::Codes::MyWid))
        return;

    Amount fee;
    if (!dataIn.Get(fee, beam::Lightning::Codes::Fee))
        return;
    if (fee != m_feeAllowed)
    {
        LOG_ERROR() << "Incoming with incorrect FEE detected";
        return;
    }

    Amount aMy;
    if (!dataIn.Get(aMy, beam::Lightning::Codes::ValueYours))
        return;
    if (aMy != m_myInAllowed)
    {
        LOG_ERROR() << "Incoming with incorrect AMOUNT detected";
        return;
    }
    
    Amount aTrg;
    if (!dataIn.Get(aTrg, beam::Lightning::Codes::ValueMy))
        return;

    Height locktime;
    if (!dataIn.Get(locktime, beam::Lightning::Codes::HLock))
        return;           

    BbsChannel ch;
    m_myInAddr.m_walletID.m_Channel.Export(ch);
    m_pConnection->BbsSubscribe(ch, 0, nullptr);
    LOG_INFO() << "LASER WAIT IN unsubscribed: " << ch;

    auto channel = std::make_unique<Channel>(
        *this, chID, m_myInAddr, trgWid, fee, aMy, aTrg, locktime);
    channel->Subscribe();
    m_channels[channel->get_chID()] = std::move(channel);

    m_pInputReceiver.reset();
}

void Mediator::OpenInternal(const ChannelIDPtr& chID)
{
    Block::SystemState::Full tip;
    get_History().get_Tip(tip);

    HeightRange openWindow;
    openWindow.m_Min = tip.m_Height;
    openWindow.m_Max = openWindow.m_Min + kDefaultTxLifetime;

    auto& ch = m_channels[chID];
    if (ch && ch->Open(openWindow))
    {
        LOG_INFO() << "LASER opening channel: "
                << to_hex(chID->m_pData, chID->nBytes);
        return;
    }


    LOG_ERROR() << "Laser open channel fail";
    m_readyForForgetChannels.push_back(chID);
}

void Mediator::ForgetChannel(const ChannelIDPtr& chID)
{
    auto it = m_channels.find(chID);
    if (it != m_channels.end())
    {
        auto& ch = it->second;
        auto state = ch->get_State();
        ch->Forget();
        ch->Unsubscribe();
        if (state == Lightning::Channel::State::OpenFailed ||
            state == Lightning::Channel::State::None)
        {
            for (auto observer : m_observers)
            {
                observer->OnOpenFailed(chID);
            }
        }
        if (state == Lightning::Channel::State::Closed)
        {
            for (auto observer : m_observers)
            {
                observer->OnClosed(chID);
            }
        }
    }
}

ChannelIDPtr Mediator::RestoreChannel(const std::string& channelIDStr)
{
    auto chId = Channel::ChIdFromString(channelIDStr);
    if (!chId)
    {
        LOG_ERROR() << "Incorrect channel ID format: "
                    << to_hex(chId->m_pData, chId->nBytes);
        return nullptr;
    }

    bool isConnected = false;
    for (const auto& it : m_channels)
    {
        isConnected = *(it.first) == *chId;
        if (isConnected)
        {
            chId = it.first;
            LOG_INFO() << "LASER channel "
                       << to_hex(chId->m_pData , chId->nBytes)
                       << " already connected";
            break;
        }
    }

    if (!isConnected)
    {
        if (!RestoreChannelInternal(chId))
        {
            LOG_INFO() << "LASER channel "
                       << to_hex(chId->m_pData , chId->nBytes)
                       << " not saved in DB";
            return nullptr;
        }
    }

    return chId;
}

bool Mediator::RestoreChannelInternal(const ChannelIDPtr& chID)
{
    TLaserChannelEntity chDBEntity;
    if (m_pWalletDB->getLaserChannel(chID, &chDBEntity) &&
        *chID == std::get<0>(chDBEntity))
    {
        auto& myWID = std::get<1>(chDBEntity);
        auto myAddr = m_pWalletDB->getAddress(myWID, true);
        if (!myAddr) {
            LOG_ERROR() << "Can't load address from DB: "
                        << std::to_string(myWID);
            return false;
        }
        m_channels[chID] = std::make_unique<Channel>(
            *this, chID, *myAddr, chDBEntity);
        return true;
    }
    return false;
}

void Mediator::UpdateChannels()
{
    for (auto& it: m_channels)
    {
        auto& ch = it.second;
        if (!ch->CanBeServed()) continue;
        ch->Update();
        UpdateChannelExterior(ch);
        ch->LogNewState();
    }
}

void Mediator::UpdateChannelExterior(const std::unique_ptr<Channel>& ch)
{
    auto lastState = ch->get_LastState();
    auto state = ch->get_State();
    if (ch->TransformLastState() &&
        state >= Lightning::Channel::State::Opening1)
    {
        ch->UpdateRestorePoint();
        m_pWalletDB->saveLaserChannel(*ch);
        
        if (state == Lightning::Channel::State::Open)
        {
            for (auto observer : m_observers)
            {
                observer->OnOpened(ch->get_chID());
            }
        }
        if (lastState == Lightning::Channel::State::Open &&
            state == Lightning::Channel::State::Updating)
        {
            for (auto observer : m_observers)
            {
                observer->OnUpdateStarted(ch->get_chID());
            }
        }
        if (lastState == Lightning::Channel::State::Updating &&
            state == Lightning::Channel::State::Open)
        {
            for (auto observer : m_observers)
            {
                observer->OnUpdateFinished(ch->get_chID());
            }
        }
    }

    if (!ch->IsNegotiating() && ch->IsSafeToForget(kSafeForgetHeight))
    {
        m_readyForForgetChannels.push_back(ch->get_chID());
    }
}

}  // namespace beam::wallet::laser
