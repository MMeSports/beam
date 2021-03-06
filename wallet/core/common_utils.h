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
#pragma once

#include "wallet/core/wallet_db.h"
#include <string>

namespace beam::wallet
{
    WalletAddress GenerateNewAddress(const IWalletDB::Ptr& walletDB, const std::string& label,
        WalletAddress::ExpirationStatus expirationStatus = WalletAddress::ExpirationStatus::OneDay,
        bool saveRequired = true
    );

    bool ReadTreasury(ByteBuffer&, const std::string& sPath);
    std::string TxIDToString(const TxID& txId);

    struct Change {
        //
        // if assetId is BEAM then changeAsset == changeBeam
        //
        Amount changeBeam = 0U;
        Amount changeAsset = 0U;
        Asset::ID assetId = Asset::s_BeamID;
    };

    Change CalcChange(const IWalletDB::Ptr& walletDB, Amount amountAsset, Amount beamFee, Asset::ID assetId);
    Amount AccumulateCoinsSum(const std::vector<Coin>& vSelStd, const std::vector<ShieldedCoin>& vSelShielded);

    struct ShieldedCoinsSelectionInfo
    {
        Amount requestedSum = 0U;
        Amount selectedSumAsset = 0U; // if assetId is BEAM then selectedSumAsset == selectedSumBeam
        Amount selectedSumBeam = 0U;
        Amount requestedFee = 0U;
        Amount selectedFee = 0U;
        Amount minimalFee = 0U;
        Amount shieldedInputsFee = 0U;
        Amount shieldedOutputsFee = 0U;
        Amount changeBeam = 0U;
        Amount changeAsset = 0U; // if assetId is BEAM then changeAsset == changeBeam

        Asset::ID assetID = Asset::s_BeamID;
        bool isEnought = true;
    };
    ShieldedCoinsSelectionInfo CalcShieldedCoinSelectionInfo(Height, const IWalletDB::Ptr& walletDB, Amount requestedSum, Amount requestedFee, Asset::ID assetId, bool isPushTx = false);

    class BaseTxBuilder;
    Amount GetFeeWithAdditionalValueForShieldedInputs(const BaseTxBuilder& builder);
}
