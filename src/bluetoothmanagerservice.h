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


#ifndef BLUETOOTH_MANAGER_H_
#define BLUETOOTH_MANAGER_H_

#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <glib.h>
#include <luna-service2/lunaservice.hpp>
#include <bluetooth-sil-api.h>
#include "bluetoothpairstate.h"

class BluetoothProfileService;
class BluetoothDevice;
class BluetoothServiceClassInfo;

namespace pbnjson
{
	class JValue;
}

namespace LS
{
	class Message;
	class ServerStatus;
}

namespace LSUtils
{
	class ClientWatch;
}

typedef struct
{
	AdvertiseData advertiseData;
	AdvertiseData scanResponse;
	AdvertiseSettings settings;
} AdvertiserInfo;

class BluetoothManagerService :
		public LS::Handle,
		public BluetoothSILStatusObserver,
		public BluetoothAdapterStatusObserver
{
public:
	BluetoothManagerService();
	~BluetoothManagerService();

	//Observer callbacks
	void adaptersChanged();
	void adapterStateChanged(bool powered);
	void adapterHciTimeoutOccurred();
	void discoveryStateChanged(bool active);
	void adapterPropertiesChanged(BluetoothPropertiesList properties);
	void adapterKeepAliveStateChanged(bool enabled);
	void deviceFound(BluetoothPropertiesList properties);
	void deviceFound(const std::string &address, BluetoothPropertiesList properties);
	void deviceRemoved(const std::string &address);
	void devicePropertiesChanged(const std::string &address, BluetoothPropertiesList properties);
	void leDeviceFoundByScanId(uint32_t scanId, BluetoothPropertiesList properties);
	void leDeviceRemovedByScanId(uint32_t scanId, const std::string &address);
	void leDevicePropertiesChangedByScanId(uint32_t scanId, const std::string &address, BluetoothPropertiesList properties);
	void deviceLinkKeyCreated(const std::string &address, BluetoothLinkKey LinkKey);
	void deviceLinkKeyDestroyed(const std::string &address, BluetoothLinkKey LinkKey);
	void requestPairingSecret(const std::string &address, BluetoothPairingSecretType type);
	void displayPairingSecret(const std::string &address, const std::string &pin);
	void displayPairingSecret(const std::string &address, BluetoothPasskey passkey);
	void displayPairingConfirmation(const std::string &address, BluetoothPasskey passkey);
	void pairingCanceled();
	void leConnectionRequest(const std::string &address, bool state);
	void requestReset();

	bool isDefaultAdapterAvailable() const;
	bool isDeviceAvailable(const std::string &address) const;
	BluetoothAdapter* getDefaultAdapter() const;
	std::string getAddress() const;

	void initializeProfiles();
	void resetProfiles();

	BluetoothDevice* findDevice(const std::string &address) const;
	BluetoothLinkKey findLinkKey(const std::string &address) const;
	bool getPowered();
	bool isAdapterAvailable(const std::string &address);
	bool isRequestedAdapterAvailable(LS::Message &request, const pbnjson::JValue &requestObj, std::string &adapterAddress);
	bool getAdvertisingState();
	void setAdvertisingState(bool advertising);
	bool isRoleEnable(const std::string &role);
	std::string getMessageOwner(LSMessage *message);
	int getAdvSize(AdvertiseData advData, bool flagRequired);


private:
	bool setState(LSMessage &message);
	bool getStatus(LSMessage &message);
	bool queryAvailable(LSMessage &message);
	bool startFilteringDiscovery(LSMessage &message);
	bool getFilteringDeviceStatus(LSMessage &message);
	bool startDiscovery(LSMessage &message);
	bool cancelDiscovery(LSMessage &message);
	bool getDeviceStatus(LSMessage &message);
	bool setDeviceState(LSMessage &msg);
	bool pair(LSMessage &message);
	bool unpair(LSMessage &message);
	bool supplyPasskey(LSMessage &message);
	bool supplyPinCode(LSMessage &message);
	bool supplyPasskeyConfirmation(LSMessage &message);
	bool cancelPairing(LSMessage &message);
	bool awaitPairingRequests(LSMessage &message);
	bool setWoBle(LSMessage &message);
	bool setWoBleTriggerDevices(LSMessage &message);
	bool getWoBleStatus(LSMessage &message);
	bool sendHciCommand(LSMessage &message);
	bool setAdvertiseData(LSMessage &message, pbnjson::JValue &value, AdvertiseData &data, bool isScanRsp);
	void updateAdvertiserData(LSMessage *requestMessage, uint8_t advertiserId, AdvertiserInfo advInfo,
			bool isSettingsChanged, bool isAdvDataChanged, bool isScanRspChanged);
	bool setTrace(LSMessage &message);
	bool getTraceStatus(LSMessage &message);
	bool getLinkKey(LSMessage &message);
	bool setKeepAlive(LSMessage &message);
	bool getKeepAliveStatus(LSMessage &message);
	bool startSniff(LSMessage &message);
	bool stopSniff(LSMessage &message);


#ifdef WBS_UPDATE_FIRMWARE
	bool updateFirmware(LSMessage &message);
#endif

	void appendCurrentStatus(pbnjson::JValue &object);
	void appendFilteringDevices(std::string senderName, pbnjson::JValue &object);
	void appendDevices(pbnjson::JValue &object);
	void appendLeDevicesByScanId(pbnjson::JValue &object, uint32_t scanId);
	void appendSupportedServiceClasses(pbnjson::JValue &object, const std::vector<BluetoothServiceClassInfo> &supportedProfiles);
	void appendConnectedProfiles(pbnjson::JValue &object, const std::string deviceAddress);
	void appendAvailableStatus(pbnjson::JValue &object);
	void appendManufacturerData(pbnjson::JValue &object, const std::vector<uint8_t> manufacturerData);
	void appendScanRecord(pbnjson::JValue &object, const std::vector<uint8_t> scanRecord);

	void notifySubscriberLeDevicesChanged(uint32_t scanId);
	void notifySubscribersAboutStateChange();
	void notifySubscribersFilteredDevicesChanged();
	void notifySubscribersDevicesChanged();
	void notifySubscribersAdvertisingChanged(std::string adapterAddress);
	void notifySubscribersAdaptersChanged();

	void handleStatePropertiesSet(BluetoothPropertiesList properties, LS::Message &request, std::string &adapterAddress, BluetoothError error);
	void handleDeviceStatePropertiesSet(BluetoothPropertiesList properties, BluetoothDevice *device, LS::Message &request, const std::string &adapterAddress, BluetoothError error);

	void updateFromAdapterProperties(const BluetoothPropertiesList &properties);
	void assignDefaultAdapter();

	void updateSupportedServiceClasses(const std::vector<std::string> uuids);

	bool isServiceClassEnabled(const std::string& serviceClass);

	void createProfiles();

	void beginIncomingPair(const std::string &address);
	void abortPairing(bool incoming);

	bool notifyPairingListenerDropped(bool incoming);
	void notifyStartScanListenerDropped(uint32_t scanId);

	bool notifyAdvertisingDropped(uint8_t advertiserId);
	bool notifyAdvertisingDisabled(uint8_t advertiserId);

	void postToClient(LSMessage *message, pbnjson::JValue &object);

	void startPairing(BluetoothDevice *device);
	void stopPairing();

	bool setPairableState(bool value);
	void cancelIncomingPairingSubscription();
	bool getDiscoveringState() const {return mDiscovering; }

	bool pairCallback (BluetoothError error);
	void cancelDiscoveryCallback(BluetoothDevice *device, BluetoothError error);

	//BLE
	bool configureAdvertisement(LSMessage &message);
	bool startAdvertising(LSMessage &message);
	bool updateAdvertising(LSMessage &message);
    bool disableAdvertising(LSMessage &message);
	bool stopAdvertising(LSMessage &message);
	bool getAdvStatus(LSMessage &message);
	bool startScan(LSMessage &message);
private:
	std::vector<BluetoothProfileService*> mProfiles;
	std::string mName;
	std::string mAddress;
	std::string mStackName;
	std::string mStackVersion;
	std::string mFirmwareVersion;
	bool mPowered;
	bool mAdvertising;
	bool mDiscovering;
	bool mWoBleEnabled;
	bool mKeepAliveEnabled;
	uint32_t mKeepAliveInterval;
	uint32_t mDiscoveryTimeout;
	uint32_t mNextLeScanId;
	bool mDiscoverable;
	uint32_t mDiscoverableTimeout;
	uint32_t mClassOfDevice;
	BluetoothSIL *mSil;
	BluetoothAdapter *mDefaultAdapter;
	std::unordered_map<std::string, BluetoothDevice*> mDevices;
	std::unordered_map<std::string, BluetoothLinkKey> mLinkKeys;
	std::vector<BluetoothServiceClassInfo> mSupportedServiceClasses;
	std::vector<std::string> mEnabledServiceClasses;
	BluetoothWoBleTriggerDeviceList mWoBleTriggerDevices;
	BluetoothPairState mPairState;
	BluetoothPairingIOCapability mPairingIOCapability;

	LSUtils::ClientWatch *mOutgoingPairingWatch;
	LSUtils::ClientWatch *mIncomingPairingWatch;
	LSUtils::ClientWatch *mAdvertisingWatch;

	std::unordered_map<uint8_t, AdvertiserInfo*> mAdvertisers;
	std::unordered_map<std::string, int32_t> mFilterClassOfDevices;
	std::unordered_map<std::string, std::string> mFilterUuids;
	std::unordered_map<uint32_t, std::unordered_map<std::string, BluetoothDevice*>> mLeDevicesByScanId;

	LS::SubscriptionPoint mGetStatusSubscriptions;
	LS::SubscriptionPoint mGetAdvStatusSubscriptions;
	LS::SubscriptionPoint mGetDevicesSubscriptions;
	LS::SubscriptionPoint mQueryAvailableSubscriptions;
	LS::SubscriptionPoint mGetKeepAliveStatusSubscriptions;

	std::unordered_map<std::string, LSUtils::ClientWatch*> mGetDevicesWatches;
	std::unordered_map<uint32_t, LSUtils::ClientWatch*> mStartScanWatches;
};

#endif

// vim: noai:ts=4:sw=4:ss=4:expandtab
