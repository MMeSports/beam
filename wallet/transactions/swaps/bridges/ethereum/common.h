// Copyright 2020 The Beam Team
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

#include <string>
#include <bitcoin/bitcoin.hpp>
#include "core/ecc.h"

namespace beam::ethereum
{
std::string ConvertEthAddressToStr(const libbitcoin::short_hash& addr);
libbitcoin::short_hash ConvertStrToEthAddress(const std::string& addressStr);
ECC::uintBig ConvertStrToUintBig(const std::string& number, bool hex = true);
std::string AddHexPrefix(const std::string& value);
std::string RemoveHexPrefix(const std::string& value);
} // namespace beam::ethereum