// Copyright (c) 2014-2018 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0


#include "bluetoothpairstate.h"
#include "bluetoothdevice.h"

BluetoothPairState::BluetoothPairState() :
		mPairing(false),
		mPairable(false),
		mPairableTimeout(30),
		mIncoming(false),
		mDevice(0)
{
}

BluetoothPairState::~BluetoothPairState()
{
}

bool BluetoothPairState::isPairable() const
{
	return mPairable;
}

uint32_t BluetoothPairState::getPairableTimeout() const
{
	return mPairableTimeout;
}

bool BluetoothPairState::isPairing() const
{
	return mPairing;
}

bool BluetoothPairState::isIncoming() const
{
	return mIncoming;
}

bool BluetoothPairState::isOutgoing() const
{
	return !mIncoming;
}

void BluetoothPairState::stopPairing()
{
	mPairing = false;
	mIncoming = false;

	if (mDevice)
		mDevice->setPairing(false);

	mDevice = 0;
}

void BluetoothPairState::startPairing(BluetoothDevice* device)
{
	mPairing = true;
	mDevice = device;

	if (mDevice)
		mDevice->setPairing(true);
}
