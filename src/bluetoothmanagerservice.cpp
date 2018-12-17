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


#include <pbnjson.hpp>
#include <assert.h>
#undef NDEBUG

#include <iostream>
#include <fstream>

#include "bluetoothmanagerservice.h"
#include "bluetoothdevice.h"
#include "bluetoothsilfactory.h"
#include "bluetoothserviceclasses.h"
#include "bluetootherrors.h"
#include "bluetoothftpprofileservice.h"
#include "bluetoothoppprofileservice.h"
#include "bluetootha2dpprofileservice.h"
#include "bluetoothgattprofileservice.h"
#include "bluetoothpbapprofileservice.h"
#include "bluetoothavrcpprofileservice.h"
#include "bluetoothsppprofileservice.h"
#include "bluetoothhfpprofileservice.h"
#include "bluetoothpanprofileservice.h"
#include "bluetoothhidprofileservice.h"
#include "bluetoothgattancsprofile.h"
#include "ls2utils.h"
#include "clientwatch.h"
#include "logging.h"
#include "config.h"
#include "utils.h"

#define BLUETOOTH_LE_START_SCAN_MAX_ID 999
#define MAX_ADVERTISING_DATA_BYTES 31

using namespace std::placeholders;

std::map<std::string, BluetoothPairingIOCapability> pairingIOCapability =
{
	{"NoInputNoOutput", BLUETOOTH_PAIRING_IO_CAPABILITY_NO_INPUT_NO_OUTPUT},
	{"DisplayOnly", BLUETOOTH_PAIRING_IO_CAPABILITY_DISPLAY_ONLY},
	{"DisplayYesNo", BLUETOOTH_PAIRING_IO_CAPABILITY_DISPLAY_YES_NO},
	{"KeyboardOnly", BLUETOOTH_PAIRING_IO_CAPABILITY_KEYBOARD_ONLY},
	{"KeyboardDisplay", BLUETOOTH_PAIRING_IO_CAPABILITY_KEYBOARD_DISPLAY}
};

BluetoothManagerService::BluetoothManagerService() :
	LS::Handle("com.webos.service.bluetooth2"),
	mPowered(false),
	mAdvertising(false),
	mDiscovering(false),
	mWoBleEnabled(false),
	mKeepAliveEnabled(false),
	mKeepAliveInterval(1),
	mDiscoveryTimeout(0),
	mDiscoverable(false),
	mDiscoverableTimeout(0),
	mClassOfDevice(0),
	mSil(0),
	mDefaultAdapter(0),
	mOutgoingPairingWatch(0),
	mIncomingPairingWatch(0),
	mAdvertisingWatch(0)
{
	std::string bluetoothCapability = WEBOS_BLUETOOTH_PAIRING_IO_CAPABILITY;
	const char* capabilityOverride = getenv("WEBOS_BLUETOOTH_PAIRING_IO_CAPABILITY");
	if (capabilityOverride != NULL)
		bluetoothCapability = capabilityOverride;

	if (pairingIOCapability.find(bluetoothCapability) != pairingIOCapability.end())
		mPairingIOCapability = pairingIOCapability[bluetoothCapability];
	else
	{
		BT_WARNING(MSGID_INVALID_PAIRING_CAPABILITY, 0, "Pairing capability not valid, fallback to simple pairing");
		mPairingIOCapability = BLUETOOTH_PAIRING_IO_CAPABILITY_NO_INPUT_NO_OUTPUT;
	}

	mEnabledServiceClasses = split(std::string(WEBOS_BLUETOOTH_ENABLED_SERVICE_CLASSES), ' ');

	mWoBleTriggerDevices.clear();
	createProfiles();

	BT_DEBUG("Creating SIL for API version %d, capability %s", BLUETOOTH_SIL_API_VERSION, bluetoothCapability.c_str());
	mSil = BluetoothSILFactory::create(BLUETOOTH_SIL_API_VERSION, mPairingIOCapability);

	if (mSil)
	{
		mSil->registerObserver(this);
		assignDefaultAdapter();
	}

	LS_CREATE_CATEGORY_BEGIN(BluetoothManagerService, adapter)
		LS_CATEGORY_METHOD(setState)
		LS_CATEGORY_METHOD(getStatus)
		LS_CATEGORY_METHOD(queryAvailable)
		LS_CATEGORY_METHOD(startDiscovery)
		LS_CATEGORY_METHOD(cancelDiscovery)
		LS_CATEGORY_METHOD(pair)
		LS_CATEGORY_METHOD(unpair)
		LS_CATEGORY_METHOD(supplyPasskey)
		LS_CATEGORY_METHOD(supplyPinCode)
		LS_CATEGORY_METHOD(supplyPasskeyConfirmation)
		LS_CATEGORY_METHOD(cancelPairing)
		LS_CATEGORY_METHOD(awaitPairingRequests)
	LS_CREATE_CATEGORY_END

	LS_CREATE_CATEGORY_BEGIN(BluetoothManagerService, adapter_internal)
		LS_CATEGORY_METHOD(setWoBle)
		LS_CATEGORY_METHOD(setWoBleTriggerDevices)
		LS_CATEGORY_METHOD(getWoBleStatus)
		LS_CATEGORY_METHOD(sendHciCommand)
		LS_CATEGORY_METHOD(setTrace)
		LS_CATEGORY_METHOD(getTraceStatus)
		LS_CATEGORY_METHOD(setKeepAlive)
		LS_CATEGORY_METHOD(getKeepAliveStatus)
		LS_CATEGORY_MAPPED_METHOD(startDiscovery, startFilteringDiscovery)
	LS_CREATE_CATEGORY_END

	LS_CREATE_CATEGORY_BEGIN(BluetoothManagerService, device)
		LS_CATEGORY_MAPPED_METHOD(getStatus, getDeviceStatus)
		LS_CATEGORY_MAPPED_METHOD(setState, setDeviceState)
	LS_CREATE_CATEGORY_END

	LS_CREATE_CATEGORY_BEGIN(BluetoothManagerService, device_internal)
		LS_CATEGORY_METHOD(getLinkKey)
		LS_CATEGORY_METHOD(startSniff)
		LS_CATEGORY_METHOD(stopSniff)
		LS_CATEGORY_MAPPED_METHOD(getStatus, getFilteringDeviceStatus)
	LS_CREATE_CATEGORY_END

	LS_CREATE_CATEGORY_BEGIN(BluetoothManagerService, le)
		LS_CATEGORY_METHOD(configureAdvertisement)
		LS_CATEGORY_METHOD(startAdvertising)
		LS_CATEGORY_METHOD(updateAdvertising)
		LS_CATEGORY_METHOD(stopAdvertising)
		LS_CATEGORY_METHOD(disableAdvertising)
		LS_CATEGORY_MAPPED_METHOD(getStatus, getAdvStatus)
		LS_CATEGORY_METHOD(startScan)
	LS_CREATE_CATEGORY_END

	registerCategory("/adapter", LS_CATEGORY_TABLE_NAME(adapter), NULL, NULL);
	setCategoryData("/adapter", this);

	registerCategory("/adapter/internal", LS_CATEGORY_TABLE_NAME(adapter_internal), NULL, NULL);
	setCategoryData("/adapter/internal", this);

	registerCategory("/device", LS_CATEGORY_TABLE_NAME(device), NULL, NULL);
	setCategoryData("/device", this);

	registerCategory("/device/internal", LS_CATEGORY_TABLE_NAME(device_internal), NULL, NULL);
	setCategoryData("/device/internal", this);

	registerCategory("/le", LS_CATEGORY_TABLE_NAME(le), NULL, NULL);
	setCategoryData("/le", this);

	mGetStatusSubscriptions.setServiceHandle(this);
	mGetDevicesSubscriptions.setServiceHandle(this);
	mQueryAvailableSubscriptions.setServiceHandle(this);
	mGetAdvStatusSubscriptions.setServiceHandle(this);
	mGetKeepAliveStatusSubscriptions.setServiceHandle(this);
}

BluetoothManagerService::~BluetoothManagerService()
{
	BT_DEBUG("Shutting down bluetooth manager service ...");

	if (mSil)
		delete mSil;

	BluetoothSILFactory::freeSILHandle();
}

bool BluetoothManagerService::isServiceClassEnabled(const std::string &serviceClass)
{
	for (auto currentServiceClass : mEnabledServiceClasses)
	{
		if (currentServiceClass == serviceClass)
			return true;
	}

	return false;
}

bool BluetoothManagerService::isDefaultAdapterAvailable() const
{
	return mDefaultAdapter != 0;
}

bool BluetoothManagerService::isAdapterAvailable(const std::string &address)
{
	//TODO: When we have multiple adapters, look for address==one of the available adapterAddresses
	//      currently only one default adapter is supported
	std::string convertedAddress = convertToLower(address);
	return (mAddress.compare(convertedAddress) == 0);
}

bool BluetoothManagerService::isRequestedAdapterAvailable(LS::Message &request, const pbnjson::JValue &requestObj, std::string &adapterAddress)
{
	if (requestObj.hasKey("adapterAddress"))
		adapterAddress = convertToLower(requestObj["adapterAddress"].asString());
	else
		adapterAddress = mAddress;

	if (!isAdapterAvailable(adapterAddress))
	{
		LSUtils::respondWithError(request, BT_ERR_INVALID_ADAPTER_ADDRESS);
		return false;
	}
	return true;
}

bool BluetoothManagerService::isRoleEnable(const std::string &role)
{
	for (auto profile : mSupportedServiceClasses)
	{
		if(convertToLower(profile.getMnemonic()) == convertToLower(role))
		{
			return true;
		}
	}
	return false;
}

std::string BluetoothManagerService::getMessageOwner(LSMessage *message)
{
        if (NULL == message)
                return EMPTY_STRING;

        std::string returnName = EMPTY_STRING;
        const char *appName = LSMessageGetApplicationID(message);
        if (NULL == appName)
        {
                appName = LSMessageGetSenderServiceName(message);
                if (appName != NULL)
                        returnName = appName;
        }
        else
        {
                returnName = appName;
                std::size_t found = returnName.find_first_of(" ");
                if (found != std::string::npos)
                        returnName.erase(found, returnName.size()-found);
        }

        return returnName;
}

int BluetoothManagerService::getAdvSize(AdvertiseData advData, bool flagRequired)
{
	//length (1byte) + type (1byte) + flag (1byte)
	int flagsFieldBytes = 3;
	// length (1byte) + type (1 byte)
	int overheadBytesPerField = 2;
	int numUuuid = 0;
	//Currently only 16-bit uuid supported
	int uuidSize = 2;

	int size = flagRequired ? flagsFieldBytes : 0;

	if (!advData.services.empty())
	{
		numUuuid = advData.services.size();
		for (auto it = advData.services.begin(); it != advData.services.end(); it++)
		{
			auto data = it->second;
			if (!(it->second.empty()))
			{
				size = size + overheadBytesPerField + data.size();
				break;
			}
		}
	}

	if (!advData.manufacturerData.empty())
	{
		size = size + overheadBytesPerField + advData.manufacturerData.size();
	}

	if (numUuuid)
	{
		size = size + overheadBytesPerField + (numUuuid * uuidSize);
	}

	for (auto it = advData.proprietaryData.begin(); it != advData.proprietaryData.end(); it++)
	{
		auto data = it->data;
		size = size + data.size() + overheadBytesPerField;
	}

	if (advData.includeTxPower)
	{
		size += overheadBytesPerField + 1; // tx power level value is one byte.
	}

	if (advData.includeName)
	{
		size += overheadBytesPerField + mName.length();
	}

	return size;
}

bool BluetoothManagerService::getAdvertisingState() {
	return mAdvertising;
}

void BluetoothManagerService::setAdvertisingState(bool advertising) {
	mAdvertising = advertising;
}

BluetoothAdapter* BluetoothManagerService::getDefaultAdapter() const
{
	return mDefaultAdapter;
}

std::string BluetoothManagerService::getAddress() const
{
	return mAddress;
}

bool BluetoothManagerService::isDeviceAvailable(const std::string &address) const
{
	std::string convertedAddress = convertToLower(address);
	auto deviceIter = mDevices.find(convertedAddress);
	if (deviceIter == mDevices.end())
		return false;

	BluetoothDevice *device = deviceIter->second;
	if(device->getAddress() == convertedAddress)
		return true;

	return false;
}

void BluetoothManagerService::createProfiles()
{
	if (isServiceClassEnabled("FTP"))
		mProfiles.push_back(new BluetoothFtpProfileService(this));

	if (isServiceClassEnabled("OPP"))
		mProfiles.push_back(new BluetoothOppProfileService(this));

	if (isServiceClassEnabled("A2DP"))
		mProfiles.push_back(new BluetoothA2dpProfileService(this));

	if (isServiceClassEnabled("GATT"))
	{
		BluetoothGattProfileService *gattService = new BluetoothGattProfileService(this);

		if (isServiceClassEnabled("ANCS")) {
			new BluetoothGattAncsProfile(this, gattService);
			//BluetoothGattAncsProfile registers with gattService
		}
		mProfiles.push_back(gattService);
	}
	if (isServiceClassEnabled("PBAP"))
		mProfiles.push_back(new BluetoothPbapProfileService(this));

	if (isServiceClassEnabled("AVRCP"))
		mProfiles.push_back(new BluetoothAvrcpProfileService(this));

	if (isServiceClassEnabled("SPP"))
		mProfiles.push_back(new BluetoothSppProfileService(this));

	if (isServiceClassEnabled("HFP"))
		mProfiles.push_back(new BluetoothHfpProfileService(this));

	if (isServiceClassEnabled("PAN"))
		mProfiles.push_back(new BluetoothPanProfileService(this));

	if (isServiceClassEnabled("HID"))
		mProfiles.push_back(new BluetoothHidProfileService(this));
}

void BluetoothManagerService::notifySubscribersAboutStateChange()
{
	pbnjson::JValue responseObj = pbnjson::Object();

	appendCurrentStatus(responseObj);

	responseObj.put("returnValue", true);

	LSUtils::postToSubscriptionPoint(&mGetStatusSubscriptions, responseObj);
}

void BluetoothManagerService::notifySubscribersFilteredDevicesChanged()
{
	pbnjson::JValue responseObj = pbnjson::Object();

	for (auto watchIter : mGetDevicesWatches)
	{
		std::string senderName = watchIter.first;
		appendFilteringDevices(senderName, responseObj);
		responseObj.put("returnValue", true);
		LSUtils::postToClient(watchIter.second->getMessage(), responseObj);
	}
}

void BluetoothManagerService::notifySubscribersDevicesChanged()
{
	pbnjson::JValue responseObj = pbnjson::Object();

	appendDevices(responseObj);

	responseObj.put("returnValue", true);

	LSUtils::postToSubscriptionPoint(&mGetDevicesSubscriptions, responseObj);
}

void BluetoothManagerService::notifySubscriberLeDevicesChanged()
{
	for (auto watchIter : mStartScanWatches)
	{
		pbnjson::JValue responseObj = pbnjson::Object();
		appendLeDevices(responseObj);

		responseObj.put("returnValue", true);
		LSUtils::postToClient(watchIter.second->getMessage(), responseObj);
	}
}

void BluetoothManagerService::notifySubscriberLeDevicesChangedbyScanId(uint32_t scanId)
{
	auto watchIter = mStartScanWatches.find(scanId);
	if (watchIter == mStartScanWatches.end())
		return;

	LSUtils::ClientWatch *watch = watchIter->second;
	pbnjson::JValue responseObj = pbnjson::Object();

	appendLeDevicesByScanId(responseObj, scanId);

	responseObj.put("returnValue", true);

	LSUtils::postToClient(watch->getMessage(), responseObj);
}

void BluetoothManagerService::notifySubscribersAdvertisingChanged(std::string adapterAddress)
{
	pbnjson::JValue responseObj = pbnjson::Object();

	responseObj.put("adapterAddress", adapterAddress);
	responseObj.put("advertising", mAdvertising);
	responseObj.put("returnValue",true);
	responseObj.put("subscribed",true);

	LSUtils::postToSubscriptionPoint(&mGetAdvStatusSubscriptions, responseObj);
}

void BluetoothManagerService::notifySubscribersAdaptersChanged()
{
	pbnjson::JValue responseObj = pbnjson::Object();

	appendAvailableStatus(responseObj);

	responseObj.put("returnValue", true);

	LSUtils::postToSubscriptionPoint(&mQueryAvailableSubscriptions, responseObj);
}

void BluetoothManagerService::adaptersChanged()
{
	BT_INFO("MANAGER_SERVICE", 0, "Observer is called : [%s : %d]", __FUNCTION__, __LINE__);

	assignDefaultAdapter();

	notifySubscribersAdaptersChanged();
}

void BluetoothManagerService::initializeProfiles()
{
	for (auto profile : mProfiles)
	{
		profile->initialize();
	}
}

void BluetoothManagerService::resetProfiles()
{
	for (auto profile : mProfiles)
	{
		profile->reset();
	}
}

void BluetoothManagerService::assignDefaultAdapter()
{
	if (!mSil)
		return;

	mDefaultAdapter = mSil->getDefaultAdapter();

	if (!mDefaultAdapter)
	{
		resetProfiles();
		return;
	}

	mDefaultAdapter->registerObserver(this);

	initializeProfiles();

	BT_DEBUG("Updating properties from default adapter");
	mDefaultAdapter->getAdapterProperties([this](BluetoothError error, const BluetoothPropertiesList &properties) {
		if (error != BLUETOOTH_ERROR_NONE)
			return;

		updateFromAdapterProperties(properties);
	});

	// Initialize pairable only for a NoInputNoOutput device
	if (mPairingIOCapability == BLUETOOTH_PAIRING_IO_CAPABILITY_NO_INPUT_NO_OUTPUT)
		setPairableState(true);

}

BluetoothDevice* BluetoothManagerService::findDevice(const std::string &address) const
{
	std::string convertedAddress = convertToLower(address);
	auto deviceIter = mDevices.find(convertedAddress);
	if (deviceIter == mDevices.end())
	{
		convertedAddress = convertToUpper(address);
		auto deviceIter = mDevices.find(convertedAddress);
		if (deviceIter == mDevices.end())
			return 0;
	}

	return deviceIter->second;
}

BluetoothDevice* BluetoothManagerService::findLeDevice(const std::string &address) const
{
	std::string convertedAddress = convertToLower(address);
	auto deviceIter = mLeDevices.find(convertedAddress);
	if (deviceIter == mLeDevices.end())
	{
		convertedAddress = convertToUpper(address);
		auto deviceIter = mLeDevices.find(convertedAddress);
		if (deviceIter == mLeDevices.end())
			return 0;
	}

	return deviceIter->second;
}

BluetoothLinkKey BluetoothManagerService::findLinkKey(const std::string &address) const
{
	std::string convertedAddress = convertToLower(address);
	auto linkKeyIter = mLinkKeys.find(convertedAddress);
	if (linkKeyIter == mLinkKeys.end())
	{
		convertedAddress = convertToUpper(address);
		auto linkKeyIter = mLinkKeys.find(convertedAddress);
		if (linkKeyIter == mLinkKeys.end())
			return std::vector<int32_t>();
	}

	return linkKeyIter->second;
}

void BluetoothManagerService::adapterStateChanged(bool powered)
{
	BT_INFO("MANAGER_SERVICE", 0, "Observer is called : [%s : %d]", __FUNCTION__, __LINE__);

	if (powered == mPowered)
		return;

	BT_INFO("Manager", 0, "Bluetooth adapter state has changed to %s", powered ? "powered" : "not powered");

	mPowered = powered;

	if ( mPowered == true )
	{
		bt_ready_msg2kernel();
		write_kernel_log("[bt_time] mPowered is true ");
	}

	notifySubscribersAboutStateChange();
}

void BluetoothManagerService::adapterHciTimeoutOccurred()
{
	BT_INFO("MANAGER_SERVICE", 0, "Observer is called : [%s : %d]", __FUNCTION__, __LINE__);
	BT_CRITICAL( "Module Error", 0, "Failed to adapterHciTimeoutOccurred" );
}

void BluetoothManagerService::discoveryStateChanged(bool active)
{
	BT_INFO("MANAGER_SERVICE", 0, "Observer is called : [%s : %d] active : %d", __FUNCTION__, __LINE__, active);

	if (mDiscovering == active)
		return;

	BT_DEBUG("Bluetooth adapter discovery state has changed to %s", active ? "active" : "not active");

	mDiscovering = active;

	notifySubscribersAboutStateChange();
}

void BluetoothManagerService::adapterPropertiesChanged(BluetoothPropertiesList properties)
{
	BT_DEBUG("Bluetooth adapter properties have changed");
	updateFromAdapterProperties(properties);
}

void BluetoothManagerService::updateFromAdapterProperties(const BluetoothPropertiesList &properties)
{
	bool changed = false;
	bool pairableValue = false;
	bool adaptersChanged = false;

	for(auto prop : properties)
	{
		switch (prop.getType())
		{
		case BluetoothProperty::Type::NAME:
			mName = prop.getValue<std::string>();
			changed = true;
			BT_DEBUG("Bluetooth adapter name has changed to %s", mName.c_str());
			break;
		case BluetoothProperty::Type::ALIAS:
			mName = prop.getValue<std::string>();
			changed = true;
			BT_DEBUG("Bluetooth adapter alias name has changed to %s", mName.c_str());
			break;
		case BluetoothProperty::Type::STACK_NAME:
			mStackName = prop.getValue<std::string>();
			changed = true;
			BT_DEBUG("Bluetooth stack name has changed to %s", mStackName.c_str());
			break;
		case BluetoothProperty::Type::STACK_VERSION:
			mStackVersion = prop.getValue<std::string>();
			changed = true;
			BT_DEBUG("Bluetooth stack version has changed to %s", mStackVersion.c_str());
			break;
		case BluetoothProperty::Type::FIRMWARE_VERSION:
			mFirmwareVersion = prop.getValue<std::string>();
			changed = true;

			// Add firmware legnth limitation due to Instart menu size.
			BT_DEBUG("Bluetooth module firmware full version has changed to %s", mFirmwareVersion.c_str());
			if ( mFirmwareVersion.size() > 11 )
				mFirmwareVersion = mFirmwareVersion.substr(0, 11);
			BT_DEBUG("Bluetooth module firmware crop version has changed to %s", mFirmwareVersion.c_str());

            if ( mFirmwareVersion == "")  // to Instart menu mFirmwareVersion : WEBDQMS-47082
                mFirmwareVersion = "NULL";

			break;
		case BluetoothProperty::Type::BDADDR:
			mAddress = convertToLower(prop.getValue<std::string>());
			changed = true;
			adaptersChanged = true;
			BT_DEBUG("Bluetooth adapter address has changed to %s", mAddress.c_str());
			break;
		case BluetoothProperty::Type::DISCOVERY_TIMEOUT:
			mDiscoveryTimeout = prop.getValue<uint32_t>();
			changed = true;
			BT_DEBUG("Bluetooth adapter discovery timeout has changed to %d", mDiscoveryTimeout);
			break;
		case BluetoothProperty::Type::DISCOVERABLE:
			mDiscoverable = prop.getValue<bool>();
			changed = true;
			BT_DEBUG("Bluetooth adapter discoverable state has changed to %s", mDiscoverable ? "discoverable" : "not discoverable");
			break;
		case BluetoothProperty::Type::DISCOVERABLE_TIMEOUT:
			mDiscoverableTimeout = prop.getValue<uint32_t>();
			changed = true;
			BT_DEBUG("Bluetooth adapter discoverable timeout has changed to %d", mDiscoverableTimeout);
			break;
		case BluetoothProperty::Type::UUIDS:
			updateSupportedServiceClasses(prop.getValue<std::vector<std::string>>());
			adaptersChanged = true;
			break;
		case BluetoothProperty::Type::CLASS_OF_DEVICE:
			mClassOfDevice = prop.getValue<uint32_t>();
			adaptersChanged = true;
			BT_DEBUG("Bluetooth adapter class of device updated to %d", mClassOfDevice);
			break;
		case BluetoothProperty::Type::PAIRABLE:
			pairableValue = prop.getValue<bool>();
			BT_DEBUG("Bluetooth adapter pairable state has changed to %s", pairableValue ? "pairable" : "not pairable");
			// If pairable has changed from true to false, it means PairableTimeout has
			// reached, so cancel the incoming subscription on awaitPairingRequests
			if (mPairState.isPairable() && pairableValue == false)
				cancelIncomingPairingSubscription();
			else if (BLUETOOTH_PAIRING_IO_CAPABILITY_NO_INPUT_NO_OUTPUT != mPairingIOCapability)
				mPairState.setPairable(pairableValue);
			break;
		case BluetoothProperty::Type::PAIRABLE_TIMEOUT:
			mPairState.setPairableTimeout(prop.getValue<uint32_t>());
			changed = true;
			BT_DEBUG("Bluetooth adapter pairable timeout has changed to %d", mPairState.getPairableTimeout());
			break;
		default:
			break;
		}
	}

	if (changed)
		notifySubscribersAboutStateChange();
	if (adaptersChanged)
		notifySubscribersAdaptersChanged();
}

void BluetoothManagerService::updateSupportedServiceClasses(const std::vector<std::string> uuids)
{
	mSupportedServiceClasses.clear();

	for (auto uuid : uuids)
	{
		std::string luuid = convertToLower(uuid);
		auto serviceClassInfo = allServiceClasses.find(luuid);
		if (serviceClassInfo == allServiceClasses.end())
		{
			// We don't have an entry in our list so we don't support the profile at all
			continue;
		}

		bool enabled = false;
		for (auto enabledServiceClass : mEnabledServiceClasses)
		{
			if (serviceClassInfo->second.getMnemonic().find(enabledServiceClass) != std::string::npos)
			{
				enabled = true;
				break;
			}
		}

		if (!enabled)
		{
			BT_DEBUG("SIL supports profile %s but support for it isn't enabled", serviceClassInfo->second.getMnemonic().c_str());
			continue;
		}

		mSupportedServiceClasses.push_back(serviceClassInfo->second);
	}

	// Sanity check if all enabled profiles are supported by the SIL
	for (auto serviceClass : mEnabledServiceClasses)
	{
		bool found = false;

		for (auto adapterSupportedServiceClass : mSupportedServiceClasses)
		{
			if (adapterSupportedServiceClass.getMnemonic().find(serviceClass) != std::string::npos)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			BT_WARNING(MSGID_ENABLED_PROFILE_NOT_SUPPORTED_BY_SIL, 0,
					   "Profile %s should be supported but isn't by the loaded SIL module",
					   serviceClass.c_str());

			// We will let the service continue to work here but all profile
			// specific actions will fail cause not supported by the SIL and
			// will produce further warnings in the logs.
		}
	}
}

void BluetoothManagerService::adapterKeepAliveStateChanged(bool enabled)
{
	BT_INFO("MANAGER_SERVICE", 0, "Observer is called : [%s : %d] enabled : %d", __FUNCTION__, __LINE__, enabled);

	if (mKeepAliveEnabled == enabled)
		return;
	else
		mKeepAliveEnabled = enabled;

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("returnValue", true);
	responseObj.put("adapterAddress", mAddress);
	responseObj.put("subscribed", true);
	responseObj.put("keepAliveEnabled", mKeepAliveEnabled);
	responseObj.put("keepAliveInterval", (int32_t)mKeepAliveInterval);

	LSUtils::postToSubscriptionPoint(&mGetKeepAliveStatusSubscriptions, responseObj);
}

void BluetoothManagerService::deviceFound(BluetoothPropertiesList properties)
{
	BluetoothDevice *device = new BluetoothDevice(properties);
	BT_DEBUG("Found a new device");
	mDevices.insert(std::pair<std::string, BluetoothDevice*>(device->getAddress(), device));

	notifySubscribersFilteredDevicesChanged();
	notifySubscribersDevicesChanged();
}

void BluetoothManagerService::deviceFound(const std::string &address, BluetoothPropertiesList properties)
{
    auto device = findDevice(address);
    if (!device) {
        BluetoothDevice *device = new BluetoothDevice(properties);
		BT_DEBUG("Found a new device");
		mDevices.insert(std::pair<std::string, BluetoothDevice*>(device->getAddress(), device));
    }
    else {
        device->update(properties);
    }
	notifySubscribersFilteredDevicesChanged();
	notifySubscribersDevicesChanged();
}

void BluetoothManagerService::devicePropertiesChanged(const std::string &address, BluetoothPropertiesList properties)
{
	BT_DEBUG("Properties of device %s have changed", address.c_str());

	auto device = findDevice(address);
	if (device && device->update(properties))
	{
		notifySubscribersFilteredDevicesChanged();
		notifySubscribersDevicesChanged();
	}
}

void BluetoothManagerService::deviceRemoved(const std::string &address)
{
	BT_DEBUG("Device %s has disappeared", address.c_str());

	auto deviceIter = mDevices.find(address);
	if (deviceIter == mDevices.end())
		return;

	BluetoothDevice *device = deviceIter->second;
	mDevices.erase(deviceIter);
	delete device;
	notifySubscribersFilteredDevicesChanged();
	notifySubscribersDevicesChanged();
}

void BluetoothManagerService::leDeviceFound(const std::string &address, BluetoothPropertiesList properties)
{
	auto device = findLeDevice(address);
	if (!device)
	{
		BluetoothDevice *device = new BluetoothDevice(properties);
		BT_DEBUG("Found a new LE device");
		mLeDevices.insert(std::pair<std::string, BluetoothDevice*>(device->getAddress(), device));
	}
	else
	{
		device->update(properties);
	}

	notifySubscriberLeDevicesChanged();
}

void BluetoothManagerService::leDevicePropertiesChanged(const std::string &address, BluetoothPropertiesList properties)
{
	BT_DEBUG("Properties of device %s have changed", address.c_str());

	auto device = findLeDevice(address);
	if (device && device->update(properties))
		notifySubscriberLeDevicesChanged();
}

void BluetoothManagerService::leDeviceRemoved(const std::string &address)
{
	BT_DEBUG("Device %s has disappeared", address.c_str());

	auto deviceIter = mLeDevices.find(address);
	if (deviceIter == mLeDevices.end())
		return;

	BluetoothDevice *device = deviceIter->second;
	mLeDevices.erase(deviceIter);
	delete device;

	notifySubscriberLeDevicesChanged();
}

void BluetoothManagerService::leDeviceFoundByScanId(uint32_t scanId, BluetoothPropertiesList properties)
{
	BluetoothDevice *device = new BluetoothDevice(properties);
	BT_DEBUG("Found a new LE device by %d", scanId);

	auto devicesIter = mLeDevicesByScanId.find(scanId);
	if (devicesIter == mLeDevicesByScanId.end())
	{
		std::unordered_map<std::string, BluetoothDevice*> devices;
		devices.insert(std::pair<std::string, BluetoothDevice*>(device->getAddress(), device));

		mLeDevicesByScanId.insert(std::pair<uint32_t, std::unordered_map<std::string, BluetoothDevice*>>(scanId, devices));
	}
	else
		(devicesIter->second).insert(std::pair<std::string, BluetoothDevice*>(device->getAddress(), device));

	notifySubscriberLeDevicesChangedbyScanId(scanId);
}

void BluetoothManagerService::leDevicePropertiesChangedByScanId(uint32_t scanId, const std::string &address, BluetoothPropertiesList properties)
{
	BT_DEBUG("Properties of device %s have changed by %d", address.c_str(), scanId);

	auto devicesIter = mLeDevicesByScanId.find(scanId);
	if (devicesIter == mLeDevicesByScanId.end())
		return;

	auto deviceIter = (devicesIter->second).find(address);
	if (deviceIter == (devicesIter->second).end())
		return;

	BluetoothDevice *device = deviceIter->second;
	if (device && device->update(properties))
	{
		notifySubscriberLeDevicesChangedbyScanId(scanId);
	}
}

void BluetoothManagerService::leDeviceRemovedByScanId(uint32_t scanId, const std::string &address)
{
	BT_DEBUG("Device %s has disappeared in %d", address.c_str(), scanId);

	auto devicesIter = mLeDevicesByScanId.find(scanId);
	if (devicesIter == mLeDevicesByScanId.end())
		return;

	auto deviceIter = (devicesIter->second).find(address);
	if (deviceIter == (devicesIter->second).end())
		return;

	BluetoothDevice *device = deviceIter->second;
	(devicesIter->second).erase(deviceIter);
	delete device;

	notifySubscriberLeDevicesChangedbyScanId(scanId);
}

void BluetoothManagerService::deviceLinkKeyCreated(const std::string &address, BluetoothLinkKey LinkKey)
{
	BT_DEBUG("Link Key of device(%s) is created", address.c_str());

	mLinkKeys.insert(std::pair<std::string, BluetoothLinkKey>(address, LinkKey));
}

void BluetoothManagerService::deviceLinkKeyDestroyed(const std::string &address, BluetoothLinkKey LinkKey)
{
	BT_DEBUG("Link Key of device(%s) is created", address.c_str());

	auto linkKeyIter = mLinkKeys.find(address);
	if (linkKeyIter == mLinkKeys.end())
		return;

	mLinkKeys.erase(linkKeyIter);
}

bool BluetoothManagerService::setState(LSMessage &msg)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&msg);
	pbnjson::JValue requestObj;
	BluetoothPropertiesList propertiesToChange;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema =  STRICT_SCHEMA(PROPS_8(
                                    PROP(adapterAddress, string), PROP(name, string), PROP(powered, boolean),
                                    PROP(discoveryTimeout, integer), PROP(discoverable, boolean),
                                    PROP(discoverableTimeout, integer), PROP(pairable, boolean),
                                    PROP(pairableTimeout, integer)));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);
		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	if (requestObj.hasKey("discoveryTimeout"))
	{
		int32_t discoveryTO = requestObj["discoveryTimeout"].asNumber<int32_t>();

		if (discoveryTO < 0)
		{
				LSUtils::respondWithError(request, retrieveErrorText(BT_ERR_DISCOVERY_TO_NEG_VALUE) + std::to_string(discoveryTO), BT_ERR_DISCOVERY_TO_NEG_VALUE);
				return true;
		}
		else
		{
			uint32_t discoveryTimeout = (uint32_t) discoveryTO;
			if (discoveryTimeout != mDiscoveryTimeout)
			{
				propertiesToChange.push_back(BluetoothProperty(BluetoothProperty::Type::DISCOVERY_TIMEOUT, discoveryTimeout));
			}
		}
	}

	if (requestObj.hasKey("discoverableTimeout"))
	{
		int32_t discoverableTO = requestObj["discoverableTimeout"].asNumber<int32_t>();

		if (discoverableTO < 0)
		{
			LSUtils::respondWithError(request, retrieveErrorText(BT_ERR_DISCOVERABLE_TO_NEG_VALUE) + std::to_string(discoverableTO), BT_ERR_DISCOVERABLE_TO_NEG_VALUE);
			return true;
		}
		else
		{
			uint32_t discoverableTimeout = (uint32_t) discoverableTO;
			if (discoverableTimeout != mDiscoverableTimeout)
			{
				propertiesToChange.push_back(BluetoothProperty(BluetoothProperty::Type::DISCOVERABLE_TIMEOUT, (uint32_t) discoverableTimeout));
			}
		}
	}

	if (requestObj.hasKey("pairableTimeout"))
	{
		int32_t pairableTO = requestObj["pairableTimeout"].asNumber<int32_t>();

		if (pairableTO < 0)
		{
			LSUtils::respondWithError(request, retrieveErrorText(BT_ERR_PAIRABLE_TO_NEG_VALUE) + std::to_string(pairableTO), BT_ERR_PAIRABLE_TO_NEG_VALUE);
			return true;
		}
		else
		{
			uint32_t pairableTimeout = (uint32_t) pairableTO;
			if (pairableTimeout != mPairState.getPairableTimeout())
			{
				propertiesToChange.push_back(BluetoothProperty(BluetoothProperty::Type::PAIRABLE_TIMEOUT, (uint32_t) pairableTimeout));
			}
		}
	}

	if (requestObj.hasKey("powered"))
	{
		bool powered = requestObj["powered"].asBool();

		if(powered != mPowered)
		{
			BluetoothError error;

            BT_INFO("Manager", 0, "mDefaultAdapter = powered :%d", powered );

			if (powered)
				error = mDefaultAdapter->enable();
			else
				error = mDefaultAdapter->disable();

			if (error != BLUETOOTH_ERROR_NONE)
			{
				LSUtils::respondWithError(request, BT_ERR_POWER_STATE_CHANGE_FAIL);
				return true;
			}
		}
	}

	if (requestObj.hasKey("name"))
	{
		std::string name = requestObj["name"].asString();

		if (name.compare(mName) != 0)
		{
			propertiesToChange.push_back(BluetoothProperty(BluetoothProperty::Type::ALIAS, name));
		}
	}

	if (requestObj.hasKey("discoverable"))
	{
		bool discoverable = requestObj["discoverable"].asBool();

		if (discoverable != mDiscoverable)
		{
			propertiesToChange.push_back(BluetoothProperty(BluetoothProperty::Type::DISCOVERABLE, discoverable));
		}
	}

	if (requestObj.hasKey("pairable"))
	{
		bool pairable = requestObj["pairable"].asBool();

		if (pairable != mPairState.isPairable())
		{
			propertiesToChange.push_back(BluetoothProperty(BluetoothProperty::Type::PAIRABLE, pairable));
		}
	}

	// if we don't have any properties to set we can just respond to the caller
	if (propertiesToChange.size() == 0)
	{
		pbnjson::JValue responseObj = pbnjson::Object();
		responseObj.put("returnValue", true);
		responseObj.put("adapterAddress", adapterAddress);

		LSUtils::postToClient(request, responseObj);
	}
	else
	{
		BT_INFO("MANAGER_SERVICE", 0, "Service calls SIL API : setAdapterProperties");
		mDefaultAdapter->setAdapterProperties(propertiesToChange, std::bind(&BluetoothManagerService::handleStatePropertiesSet, this, propertiesToChange, request, adapterAddress, _1));
	}

	return true;
}

bool BluetoothManagerService::setPairableState(bool value)
{
	BT_DEBUG("Setting pairable to %d", value);
	bool retVal=false;

	if (!mDefaultAdapter)
	{
		return false;
	}

	auto pairableCB = [this, value, &retVal](BluetoothError error) {
		if (error == BLUETOOTH_ERROR_NONE)
		{
			BT_DEBUG("Pairable value set in SIL with no errors");
			mPairState.setPairable(value);
			notifySubscribersAboutStateChange();
			retVal = true;
		}
	};
	mDefaultAdapter->setAdapterProperty(BluetoothProperty(BluetoothProperty::Type::PAIRABLE, value), pairableCB);

	return retVal;
}

void BluetoothManagerService::handleStatePropertiesSet(BluetoothPropertiesList properties, LS::Message &request, std::string &adapterAddress, BluetoothError error)
{
	BT_INFO("MANAGER_SERVICE", 0, "Return of handleStatePropertiesSet is %d", error);

	if (BLUETOOTH_ERROR_NONE != error)
	{
		LSUtils::respondWithError(request, error);
		return;
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("returnValue", true);
	responseObj.put("adapterAddress", adapterAddress);

	LSUtils::postToClient(request, responseObj);
}

void BluetoothManagerService::handleDeviceStatePropertiesSet(BluetoothPropertiesList properties, BluetoothDevice *device, LS::Message &request, const std::string &adapterAddress, BluetoothError error)
{
	BT_INFO("MANAGER_SERVICE", 0, "Return of handleDeviceStatePropertiesSet is %d", error);

	if (BLUETOOTH_ERROR_NONE != error)
	{
		LSUtils::respondWithError(request, error);
		return;
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("adapterAddress", adapterAddress);
	if (device && device->update(properties))
		responseObj.put("returnValue", true);
	else
		responseObj.put("returnValue", false);

	LSUtils::postToClient(request, responseObj);
}

void BluetoothManagerService::appendCurrentStatus(pbnjson::JValue &object)
{
	pbnjson::JValue adaptersObj = pbnjson::Array();

	pbnjson::JValue adapterObj = pbnjson::Object();
	adapterObj.put("powered", mPowered);
	adapterObj.put("name", mName);
	adapterObj.put("adapterAddress", mAddress);
	adapterObj.put("discovering", mDiscovering);
	// pbnjson doesn't support unsigned int, so using int32_t for discoveryTimeout
	// and discoverableTimeout
	adapterObj.put("discoveryTimeout", (int32_t) mDiscoveryTimeout);
	adapterObj.put("discoverable", mDiscoverable);
	adapterObj.put("discoverableTimeout", (int32_t) mDiscoverableTimeout);
	adapterObj.put("pairable", mPairState.isPairable());
	adapterObj.put("pairableTimeout", (int32_t) mPairState.getPairableTimeout());
	adapterObj.put("pairing", mPairState.isPairing());

	adaptersObj.append(adapterObj);

	object.put("adapters", adaptersObj);
}

void BluetoothManagerService::appendAvailableStatus(pbnjson::JValue &object)
{
	pbnjson::JValue adaptersObj = pbnjson::Array();

	if (mDefaultAdapter)
	{
		pbnjson::JValue adapterObj = pbnjson::Object();
		adapterObj.put("adapterAddress", mAddress);
		adapterObj.put("default", true);
		// pbnjson doesn't support unsigned int, so using int32_t for classOfDevice
		adapterObj.put("classOfDevice", (int32_t)mClassOfDevice);
		adapterObj.put("stackName", mStackName);
		adapterObj.put("stackVersion", mStackVersion);
		adapterObj.put("firmwareVersion", mFirmwareVersion);
		appendSupportedServiceClasses(adapterObj, mSupportedServiceClasses);

		adaptersObj.append(adapterObj);
	}

	object.put("adapters", adaptersObj);
}

void BluetoothManagerService::appendFilteringDevices(std::string senderName, pbnjson::JValue &object)
{
	pbnjson::JValue devicesObj = pbnjson::Array();

	for (auto deviceIter : mDevices)
	{
		auto device = deviceIter.second;
		pbnjson::JValue deviceObj = pbnjson::Object();

		auto filterClassOfDevices = mFilterClassOfDevices.find(senderName);
		if (filterClassOfDevices != mFilterClassOfDevices.end())
			if((((int32_t)(filterClassOfDevices->second) & (int32_t)(device->getClassOfDevice())) != (int32_t)(filterClassOfDevices->second)))
				continue;

		if(device->getTypeAsString() == "bredr")
		{
			if(mFilterUuids.size() > 0)
			{
				auto filterUuid = mFilterUuids.find(senderName);
				if(filterUuid->second.c_str() != NULL)
				{
					auto uuidIter = std::find(device->getUuids().begin(), device->getUuids().end(), filterUuid->second);
					if (filterUuid != mFilterUuids.end() && uuidIter != device->getUuids().end())
						continue;
				}
			}
		}

		deviceObj.put("name", device->getName());
		deviceObj.put("address", device->getAddress());
		deviceObj.put("typeOfDevice", device->getTypeAsString());
		deviceObj.put("classOfDevice", (int32_t) device->getClassOfDevice());
		deviceObj.put("paired", device->getPaired());
		deviceObj.put("pairing", device->getPairing());
		deviceObj.put("trusted", device->getTrusted());
		deviceObj.put("blocked", device->getBlocked());
		deviceObj.put("rssi", device->getRssi());

		if(device->getPaired())
			deviceObj.put("adapterAddress", mAddress);
		else
			deviceObj.put("adapterAddress", "");

		appendManufacturerData(deviceObj, device->getManufacturerData());
		appendSupportedServiceClasses(deviceObj, device->getSupportedServiceClasses());
		appendConnectedProfiles(deviceObj, device->getAddress());
		devicesObj.append(deviceObj);
	}

	object.put("devices", devicesObj);
}

void BluetoothManagerService::appendLeDevices(pbnjson::JValue &object)
{
	pbnjson::JValue devicesObj = pbnjson::Array();

	for (auto deviceIter : mLeDevices)
	{
		auto device = deviceIter.second;
		pbnjson::JValue deviceObj = pbnjson::Object();

		deviceObj.put("address", device->getAddress());
		deviceObj.put("rssi", device->getRssi());

		appendScanRecord(deviceObj, device->getScanRecord());
		devicesObj.append(deviceObj);
	}

	object.put("devices", devicesObj);
}

void BluetoothManagerService::appendLeDevicesByScanId(pbnjson::JValue &object, uint32_t scanId)
{
	auto devicesIter = mLeDevicesByScanId.find(scanId);
	if (devicesIter == mLeDevicesByScanId.end())
		return;

	std::unordered_map<std::string, BluetoothDevice*> devices = devicesIter->second;
	pbnjson::JValue devicesObj = pbnjson::Array();

	for (auto deviceIter : devices)
	{
		auto device = deviceIter.second;
		pbnjson::JValue deviceObj = pbnjson::Object();

        if(!device->getName().compare("LGE MR18")) {
            BT_INFO("Manager", 0, "name: %s, address: %s, paired: %d, rssi: %d, blocked: %d\n", device->getName().c_str(), device->getAddress().c_str(), device->getPaired(), device->getRssi(), device->getBlocked());
        }

		deviceObj.put("name", device->getName());
		deviceObj.put("address", device->getAddress());
		deviceObj.put("typeOfDevice", device->getTypeAsString());
		deviceObj.put("classOfDevice", (int32_t) device->getClassOfDevice());
		deviceObj.put("paired", device->getPaired());
		deviceObj.put("pairing", device->getPairing());
		deviceObj.put("trusted", device->getTrusted());
		deviceObj.put("blocked", device->getBlocked());
		deviceObj.put("rssi", device->getRssi());

		if(device->getPaired())
			deviceObj.put("adapterAddress", mAddress);
		else
			deviceObj.put("adapterAddress", "");

		appendManufacturerData(deviceObj, device->getManufacturerData());
		appendScanRecord(deviceObj, device->getScanRecord());
		appendSupportedServiceClasses(deviceObj, device->getSupportedServiceClasses());
		appendConnectedProfiles(deviceObj, device->getAddress());
		devicesObj.append(deviceObj);
	}

	object.put("devices", devicesObj);
}

void BluetoothManagerService::appendDevices(pbnjson::JValue &object)
{
	pbnjson::JValue devicesObj = pbnjson::Array();

	for (auto deviceIter : mDevices)
	{
		auto device = deviceIter.second;
		pbnjson::JValue deviceObj = pbnjson::Object();

        if(!device->getName().compare("LGE MR18")) {
            BT_INFO("Manager", 0, "name: %s, address: %s, paired: %d, rssi: %d, blocked: %d\n", device->getName().c_str(), device->getAddress().c_str(), device->getPaired(), device->getRssi(), device->getBlocked());
        }

		deviceObj.put("name", device->getName());
		deviceObj.put("address", device->getAddress());
		deviceObj.put("typeOfDevice", device->getTypeAsString());
		deviceObj.put("classOfDevice", (int32_t) device->getClassOfDevice());
		deviceObj.put("paired", device->getPaired());
		deviceObj.put("pairing", device->getPairing());
		deviceObj.put("trusted", device->getTrusted());
		deviceObj.put("blocked", device->getBlocked());
		deviceObj.put("rssi", device->getRssi());

		if(device->getPaired())
			deviceObj.put("adapterAddress", mAddress);
		else
			deviceObj.put("adapterAddress", "");

		appendManufacturerData(deviceObj, device->getManufacturerData());
		appendSupportedServiceClasses(deviceObj, device->getSupportedServiceClasses());
		appendConnectedProfiles(deviceObj, device->getAddress());
		appendScanRecord(deviceObj, device->getScanRecord());
		devicesObj.append(deviceObj);
	}

	object.put("devices", devicesObj);
}

void BluetoothManagerService::appendScanRecord(pbnjson::JValue &object, const std::vector<uint8_t> scanRecord)
{
	pbnjson::JValue scanRecordArray = pbnjson::Array();

	for (int i = 0; i < scanRecord.size(); i++)
		scanRecordArray.append(scanRecord[i]);

	object.put("scanRecord", scanRecordArray);
}

void BluetoothManagerService::appendManufacturerData(pbnjson::JValue &object, const std::vector<uint8_t> manufacturerData)
{
	pbnjson::JValue manufacturerDataObj = pbnjson::Object();
	unsigned int i = 0;

	if(manufacturerData.size() > 2)
	{
		pbnjson::JValue idArray = pbnjson::Array();
		for (i = 0; i < 2; i++)
			idArray.append(manufacturerData[i]);

		pbnjson::JValue dataArray = pbnjson::Array();
		for (i = 2; i < manufacturerData.size(); i++)
			dataArray.append(manufacturerData[i]);

		manufacturerDataObj.put("companyId", idArray);
		manufacturerDataObj.put("data", dataArray);
	}

	object.put("manufacturerData", manufacturerDataObj);
}

void BluetoothManagerService::appendSupportedServiceClasses(pbnjson::JValue &object, const std::vector<BluetoothServiceClassInfo> &supportedServiceClasses)
{
	pbnjson::JValue supportedProfilesObj = pbnjson::Array();

	for (auto profile : supportedServiceClasses)
	{
		pbnjson::JValue profileObj = pbnjson::Object();

		profileObj.put("mnemonic", profile.getMnemonic());

		// Only set the category if we have one. If we don't have one then the
		// profile doesn't have any support in here and we don't need to expose
		// a non existing category name
		std::string category = profile.getMethodCategory();
		if (!category.empty())
			profileObj.put("category", profile.getMethodCategory());

		supportedProfilesObj.append(profileObj);
	}

	object.put("serviceClasses", supportedProfilesObj);
}

void BluetoothManagerService::appendConnectedProfiles(pbnjson::JValue &object, const std::string deviceAddress)
{
	pbnjson::JValue connectedProfilesObj = pbnjson::Array();

	for (auto profile : mProfiles)
	{
		if (profile->isDeviceConnected(deviceAddress))
			connectedProfilesObj.append(convertToLower(profile->getName()));
	}

	object.put("connectedProfiles", connectedProfilesObj);
}

bool BluetoothManagerService::getStatus(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;
	bool subscribed = false;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, SCHEMA_1(PROP(subscribe, boolean)), &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	pbnjson::JValue responseObj = pbnjson::Object();

	if (request.isSubscription())
	{
		mGetStatusSubscriptions.subscribe(request);
		subscribed = true;
	}

	appendCurrentStatus(responseObj);

	responseObj.put("returnValue", true);
	responseObj.put("subscribed", subscribed);

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::queryAvailable(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;
	bool subscribed = false;

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, SCHEMA_1(PROP(subscribe, boolean)), &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	pbnjson::JValue responseObj = pbnjson::Object();

	if (request.isSubscription())
	{
		mQueryAvailableSubscriptions.subscribe(request);
		subscribed = true;
	}

	appendAvailableStatus(responseObj);

	responseObj.put("returnValue", true);
	responseObj.put("subscribed", subscribed);

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::startFilteringDiscovery(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	if (!mPowered)
	{
		LSUtils::respondWithError(request, BT_ERR_START_DISC_ADAPTER_OFF_ERR);
		return true;
	}

	const std::string schema =  STRICT_SCHEMA(PROPS_2(PROP(typeOfDevice, string), PROP(accessCode, string)));
	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	std::string adapterAddress;
	std::string typeOfDevice;
	std::string accessCode;
	TransportType transportType = TransportType::BT_TRANSPORT_TYPE_NONE;
	InquiryAccessCode inquiryAccessCode = InquiryAccessCode::BT_ACCESS_CODE_NONE; // CID 166097
	uint8_t mergedTransportType = 0;
	uint8_t mergedInquiryAccessCode = 0;

	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	const char* senderName = LSMessageGetApplicationID(&message);
	if(senderName == NULL)
	{
	    senderName = LSMessageGetSenderServiceName(&message);
		if(senderName == NULL)
		{
			LSUtils::respondWithError(request, BT_ERR_START_DISC_FAIL);
			return true;
		}
	}

	if (requestObj.hasKey("typeOfDevice"))
	{
		std::string typeOfDevice = requestObj["typeOfDevice"].asString();
		if(typeOfDevice == "none")
			transportType = TransportType::BT_TRANSPORT_TYPE_NONE;
		else if(typeOfDevice == "bredr")
			transportType = TransportType::BT_TRANSPORT_TYPE_BR_EDR;
		else if(typeOfDevice == "ble")
			transportType = TransportType::BT_TRANSPORT_TYPE_LE;
		else
			transportType = TransportType::BT_TRANSPORT_TYPE_DUAL;
	}

	if (requestObj.hasKey("accessCode"))
	{
		std::string accessCode = requestObj["accessCode"].asString();
		if(accessCode == "none")
			inquiryAccessCode = InquiryAccessCode::BT_ACCESS_CODE_NONE;
		else if(accessCode == "liac")
			inquiryAccessCode = InquiryAccessCode::BT_ACCESS_CODE_LIMIT;
		else
			inquiryAccessCode = InquiryAccessCode::BT_ACCESS_CODE_GENERAL;
	}

	BluetoothError error = BLUETOOTH_ERROR_NONE;
	// Outgoing pairing performs in two steps, cancelDiscovery() and pair().
	// startDiscovery request in the middle of pairing must be ignored.
	if (!mPairState.isPairing())
		error = mDefaultAdapter->startDiscovery(transportType, inquiryAccessCode);
	else
	{
		LSUtils::respondWithError(request, BT_ERR_PAIRING_IN_PROG);
		return true;
	}

	if (error != BLUETOOTH_ERROR_NONE)
	{
		LSUtils::respondWithError(request, BT_ERR_START_DISC_FAIL);
		return true;
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("returnValue", true);
	responseObj.put("adapterAddress", adapterAddress);

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::startDiscovery(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	if (!mPowered)
	{
		LSUtils::respondWithError(request, BT_ERR_START_DISC_ADAPTER_OFF_ERR);
		return true;
	}

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, SCHEMA_1(PROP(adapterAddress, string)), &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	BluetoothError error = BLUETOOTH_ERROR_NONE;
	// Outgoing pairing performs in two steps, cancelDiscovery() and pair().
	// startDiscovery request in the middle of pairing must be ignored.
	if (!mPairState.isPairing())
		error = mDefaultAdapter->startDiscovery();

	if (error != BLUETOOTH_ERROR_NONE)
	{
		LSUtils::respondWithError(request, BT_ERR_START_DISC_FAIL);
		return true;
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("returnValue", true);
	responseObj.put("adapterAddress", adapterAddress);

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::cancelDiscovery(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	if (!mPowered)
	{
		LSUtils::respondWithError(request, BT_ERR_DISC_STOP_ADAPTER_OFF_ERR);
		return true;
	}

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, SCHEMA_1(PROP(adapterAddress, string)), &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);

	mDefaultAdapter->cancelDiscovery([requestMessage, this, adapterAddress](BluetoothError error) {

		if (error != BLUETOOTH_ERROR_NONE)
		{
			LSUtils::respondWithError(requestMessage, BT_ERR_STOP_DISC_FAIL);
		}
		else
		{
			pbnjson::JValue responseObj = pbnjson::Object();
			responseObj.put("returnValue", true);
			responseObj.put("adapterAddress", adapterAddress);
			LSUtils::postToClient(requestMessage, responseObj);
		}

		const char* senderName = LSMessageGetApplicationID(requestMessage);
		if( senderName == NULL )
		{
			senderName = LSMessageGetSenderServiceName(requestMessage);
		}

		if(senderName != NULL)
		{
			auto watchIter = mGetDevicesWatches.find(senderName);
			if (watchIter == mGetDevicesWatches.end())
				return;

			LSUtils::ClientWatch *watch = watchIter->second;
			mGetDevicesWatches.erase(watchIter);
			delete watch;
		}
	});

	return true;
}

bool BluetoothManagerService::getLinkKey(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_2(PROP(address, string), PROP(adapterAddress, string)) REQUIRED_1(address));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	std::string address = requestObj["address"].asString();
	auto device = findDevice(address);
	if (!device)
	{
		LSUtils::respondWithError(request, BT_ERR_DEVICE_NOT_AVAIL);
		return true;
	}

	auto linkKey = findLinkKey(address);

	pbnjson::JValue linkKeyArray = pbnjson::Array();
	for (size_t i=0; i < linkKey.size(); i++)
		linkKeyArray.append((int32_t) linkKey[i]);

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("returnValue", true);
	responseObj.put("adapterAddress", adapterAddress);
	responseObj.put("address", address);
	responseObj.put("linkKey", linkKeyArray);

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::startSniff(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_6(PROP(address, string), PROP(adapterAddress, string),
													PROP(minInterval, integer), PROP(maxInterval, integer),
													PROP(attempt, integer), PROP(timeout, integer))
													REQUIRED_5(address, minInterval, maxInterval, attempt, timeout));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	std::string address = requestObj["address"].asString();
	auto device = findDevice(address);
	if (!device)
	{
		LSUtils::respondWithError(request, BT_ERR_DEVICE_NOT_AVAIL);
		return true;
	}

	int minInterval = 0, maxInterval = 0, attempt = 0, timeout = 0;

	if (requestObj.hasKey("minInterval"))
		minInterval = requestObj["minInterval"].asNumber<int32_t>();

	if (requestObj.hasKey("maxInterval"))
		maxInterval = requestObj["maxInterval"].asNumber<int32_t>();

	if (requestObj.hasKey("attempt"))
		attempt = requestObj["attempt"].asNumber<int32_t>();

	if (requestObj.hasKey("timeout"))
		timeout = requestObj["timeout"].asNumber<int32_t>();

	BluetoothError error;
	pbnjson::JValue responseObj = pbnjson::Object();

	error = mDefaultAdapter->startSniff(address, minInterval, maxInterval, attempt, timeout);
	if (BLUETOOTH_ERROR_NONE == error)
	{
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("returnValue", true);
	}
	else
	{
		appendErrorResponse(responseObj, error);
	}

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::stopSniff(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_2(PROP(address, string), PROP(adapterAddress, string))
													REQUIRED_1(address));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	std::string address = requestObj["address"].asString();
	auto device = findDevice(address);
	if (!device)
	{
		LSUtils::respondWithError(request, BT_ERR_DEVICE_NOT_AVAIL);
		return true;
	}

	BluetoothError error;
	pbnjson::JValue responseObj = pbnjson::Object();

	error = mDefaultAdapter->stopSniff(address);
	if (BLUETOOTH_ERROR_NONE == error)
	{
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("returnValue", true);
	}
	else
	{
		appendErrorResponse(responseObj, error);
	}

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::getFilteringDeviceStatus(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;
	bool subscribed = false;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema =  STRICT_SCHEMA(PROPS_4(PROP(subscribe, boolean), PROP(adapterAddress, string), PROP(classOfDevice, integer), PROP(uuid, string)));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	std::string appName = getMessageOwner(request.get());
    if (appName.compare("") == 0)
    {
            LSUtils::respondWithError(request, BT_ERR_MESSAGE_OWNER_MISSING, true);
            return true;
    }

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	const char* senderName = LSMessageGetApplicationID(&message);
	if(senderName == NULL)
	{
		senderName = LSMessageGetSenderServiceName(&message);
		if(senderName == NULL)
		{
			LSUtils::respondWithError(request, BT_ERR_START_DISC_FAIL);
			return true;
		}
	}

	if (requestObj.hasKey("classOfDevice"))
	{
		if(mFilterClassOfDevices.find(appName) != mFilterClassOfDevices.end())
			mFilterClassOfDevices[appName] = requestObj["classOfDevice"].asNumber<int32_t>();
		else
			mFilterClassOfDevices.insert(std::pair<std::string, int32_t>(appName, requestObj["classOfDevice"].asNumber<int32_t>()));
	}
	else
	{
		if(mFilterClassOfDevices.find(appName) != mFilterClassOfDevices.end())
			mFilterClassOfDevices[appName] = 0;
		else
			mFilterClassOfDevices.insert(std::pair<std::string, int32_t>(appName, 0));
	}

	if (requestObj.hasKey("uuid"))
	{
		if(mFilterUuids.find(appName) != mFilterUuids.end())
			mFilterUuids[appName] = requestObj["uuid"].asString();
		else
			mFilterUuids.insert(std::pair<std::string, std::string>(appName, requestObj["uuid"].asString()));
	}
	else
	{
		if(mFilterUuids.find(appName) != mFilterUuids.end())
			mFilterUuids[appName] = std::string();
		else
			mFilterUuids.insert(std::pair<std::string, std::string>(appName, std::string()));
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	if (request.isSubscription())
	{
		LSUtils::ClientWatch *watch = new LSUtils::ClientWatch(get(), &message, nullptr);
		if(mGetDevicesWatches.find(senderName) != mGetDevicesWatches.end())
			mGetDevicesWatches[senderName] = watch;
		else
			mGetDevicesWatches.insert(std::pair<std::string, LSUtils::ClientWatch*>(senderName, watch));

		subscribed = true;
	}

	appendFilteringDevices(senderName, responseObj);

	responseObj.put("returnValue", true);
	responseObj.put("subscribed", subscribed);
	responseObj.put("adapterAddress", adapterAddress);

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::getDeviceStatus(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
        int parseError = 0;
	bool subscribed = false;

	const std::string schema =  STRICT_SCHEMA(PROPS_3(PROP(subscribe, boolean), PROP(adapterAddress, string), PROP(classOfDevice, integer)));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	if (request.isSubscription())
	{
		mGetDevicesSubscriptions.subscribe(request);
		subscribed = true;
	}

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	pbnjson::JValue responseObj = pbnjson::Object();

	appendDevices(responseObj);

	responseObj.put("returnValue", true);
	responseObj.put("subscribed", subscribed);
	responseObj.put("adapterAddress", adapterAddress);

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::setDeviceState(LSMessage &msg)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&msg);
	pbnjson::JValue requestObj;
	BluetoothPropertiesList propertiesToChange;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema =  STRICT_SCHEMA(PROPS_4(
                                    PROP(address, string), PROP(trusted, boolean), PROP(blocked, boolean), PROP(adapterAddress, string))
                                    REQUIRED_1(address));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!requestObj.hasKey("address"))
			LSUtils::respondWithError(request, BT_ERR_ADDR_PARAM_MISSING);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	std::string address = requestObj["address"].asString();
	auto device = findDevice(address);
	if (!device)
	{
		LSUtils::respondWithError(request, BT_ERR_DEVICE_NOT_AVAIL);
		return true;
	}

	if (requestObj.hasKey("trusted"))
	{
		bool trusted = requestObj["trusted"].asBool();
		trusted = requestObj["trusted"].asBool();

		if (trusted != device->getTrusted())
			propertiesToChange.push_back(BluetoothProperty(BluetoothProperty::Type::TRUSTED, trusted));
	}

	if (requestObj.hasKey("blocked"))
	{
		bool blocked = requestObj["blocked"].asBool();

		if (blocked != device->getBlocked())
			propertiesToChange.push_back(BluetoothProperty(BluetoothProperty::Type::BLOCKED, blocked));
	}

	if (propertiesToChange.size() == 0)
		LSUtils::respondWithError(request, BT_ERR_NO_PROP_CHANGE);
	else
		mDefaultAdapter->setDeviceProperties(address, propertiesToChange, std::bind(&BluetoothManagerService::handleDeviceStatePropertiesSet, this, propertiesToChange, device, request, adapterAddress, _1));

	return true;
}

bool BluetoothManagerService::pair(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_3(PROP(address, string),
                                              PROP_WITH_VAL_1(subscribe, boolean, true), PROP(adapterAddress,string))
                                              REQUIRED_2(address,subscribe));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!requestObj.hasKey("address"))
			LSUtils::respondWithError(request, BT_ERR_ADDR_PARAM_MISSING);

		else if (!request.isSubscription())
			LSUtils::respondWithError(request, BT_ERR_MTHD_NOT_SUBSCRIBED);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	if(mOutgoingPairingWatch)
	{
		LSUtils::respondWithError(request, BT_ERR_ALLOW_ONE_SUBSCRIBE);
		return true;
	}

	if (mPairState.isPairing())
	{
		LSUtils::respondWithError(request, BT_ERR_PAIRING_IN_PROG);
		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	std::string address = requestObj["address"].asString();

	BluetoothDevice *device = findDevice(address);
	if (!device)
	{
		LSUtils::respondWithError(request, BT_ERR_DEVICE_NOT_AVAIL);
		return true;
	}

	if (device->getPaired())
	{
		LSUtils::respondWithError(request, BLUETOOTH_ERROR_DEVICE_ALREADY_PAIRED);
		return true;
	}

	mOutgoingPairingWatch = new LSUtils::ClientWatch(get(), &message, [this]() {
		notifyPairingListenerDropped(false);
	});

	mPairState.markAsOutgoing();

	// We have to send a response to the client immediately
	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("adapterAddress", adapterAddress);
	responseObj.put("subscribed", true);
	responseObj.put("returnValue", true);
	LSUtils::postToClient(request, responseObj);

	startPairing(device);

	return true;
}

bool BluetoothManagerService::supplyPasskey(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}


	const std::string schema = STRICT_SCHEMA(PROPS_3(PROP(address, string), PROP(passkey, integer), PROP(adapterAddress, string))
                                              REQUIRED_2(address, passkey));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!requestObj.hasKey("address"))
			LSUtils::respondWithError(request, BT_ERR_ADDR_PARAM_MISSING);

		else if (!requestObj.hasKey("passkey"))
			LSUtils::respondWithError(request, BT_ERR_PASSKEY_PARAM_MISSING);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	std::string address = requestObj["address"].asString();
	uint32_t passkey = requestObj["passkey"].asNumber<int32_t>();

	BluetoothError error = mDefaultAdapter->supplyPairingSecret(address, passkey);
	pbnjson::JValue responseObj = pbnjson::Object();
	if (BLUETOOTH_ERROR_NONE == error)
	{
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("returnValue", true);
	}
	else
	{
		appendErrorResponse(responseObj, error);
	}

	LSUtils::postToClient(request, responseObj);

	if (mPairState.isIncoming())
		stopPairing();

	return true;
}

bool BluetoothManagerService::supplyPinCode(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}


	const std::string schema = STRICT_SCHEMA(PROPS_3(PROP(address, string), PROP(pin, string), PROP(adapterAddress, string))
                                             REQUIRED_2(address, pin));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!requestObj.hasKey("address"))
			LSUtils::respondWithError(request, BT_ERR_ADDR_PARAM_MISSING);

		else if (!requestObj.hasKey("pin"))
			LSUtils::respondWithError(request, BT_ERR_PIN_PARAM_MISSING);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	std::string address = requestObj["address"].asString();
	std::string pin = requestObj["pin"].asString();

	BluetoothError error = mDefaultAdapter->supplyPairingSecret(address, pin);
	pbnjson::JValue responseObj = pbnjson::Object();
	if (BLUETOOTH_ERROR_NONE == error)
	{
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("returnValue", true);
	}
	else
	{
		appendErrorResponse(responseObj, error);
	}

	LSUtils::postToClient(request, responseObj);

	if (mPairState.isIncoming())
		stopPairing();

	return true;
}

bool BluetoothManagerService::supplyPasskeyConfirmation(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_3(PROP(address, string), PROP(accept, boolean), PROP(adapterAddress, string))
                                              REQUIRED_2(address, accept));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!requestObj.hasKey("address"))
			LSUtils::respondWithError(request, BT_ERR_ADDR_PARAM_MISSING);

		else if (!requestObj.hasKey("accept"))
			LSUtils::respondWithError(request, BT_ERR_ACCEPT_PARAM_MISSING);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	if (!mPairState.isPairing())
	{
		LSUtils::respondWithError(request, BT_ERR_NO_PAIRING);
		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	std::string address = requestObj["address"].asString();
	bool accept = requestObj["accept"].asBool();

	BluetoothError error = mDefaultAdapter->supplyPairingConfirmation(address, accept);

	pbnjson::JValue responseObj = pbnjson::Object();
	if (BLUETOOTH_ERROR_NONE == error)
	{
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("returnValue", true);
	}
	else
	{
		appendErrorResponse(responseObj, error);
	}

	LSUtils::postToClient(request, responseObj);

	// For an incoming pairing request we're done at this point. Either
	// the user accepted the pairing request or not but we don't have to
	// track that anymore. Service users will get notified about a newly
	// paired device once its state switched to paired.
	if (mPairState.isIncoming())
		stopPairing();

	return true;
}

bool BluetoothManagerService::cancelPairing(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_2(PROP(address, string), PROP(adapterAddress, string)) REQUIRED_1(address));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!requestObj.hasKey("address"))
			LSUtils::respondWithError(request, BT_ERR_ADDR_PARAM_MISSING);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	if (!mPairState.isPairing())
	{
		LSUtils::respondWithError(request, BT_ERR_NO_PAIRING);
		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	std::string address = requestObj["address"].asString();
	auto device = findDevice(address);
	if (!device)
	{
		LSUtils::respondWithError(request, BT_ERR_DEVICE_NOT_AVAIL);
		return true;
	}

	if (mPairState.getDevice()->getAddress() != address)
	{
		LSUtils::respondWithError(request, BT_ERR_NO_PAIRING_FOR_REQUESTED_ADDRESS);
		return true;
	}

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);

	auto cancelPairingCallback = [this, requestMessage, device, adapterAddress](BluetoothError error) {
		pbnjson::JValue responseObj = pbnjson::Object();
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("returnValue", true);

		LSUtils::postToClient(requestMessage, responseObj);
		LSMessageUnref(requestMessage);

		pbnjson::JValue subscriptionResponseObj = pbnjson::Object();

		if (BLUETOOTH_ERROR_NONE == error)
		{
			BT_DEBUG("Cancel pairing success");
			// When an incoming pairing request is canceled we don't drop the
			// subscription
			subscriptionResponseObj.put("adapterAddress", adapterAddress);
			subscriptionResponseObj.put("subscribed", mPairState.isIncoming());
			subscriptionResponseObj.put("returnValue", false);
			subscriptionResponseObj.put("request", "endPairing");
			subscriptionResponseObj.put("errorCode", (int32_t)BT_ERR_PAIRING_CANCELED);
			subscriptionResponseObj.put("errorText", retrieveErrorText(BT_ERR_PAIRING_CANCELED));
		}
		else
		{
			BT_DEBUG("Cancel pairing failed");
			subscriptionResponseObj.put("adapterAddress", adapterAddress);
			subscriptionResponseObj.put("subscribed", true);
			subscriptionResponseObj.put("returnValue", true);
			subscriptionResponseObj.put("request", "continuePairing");
		}

		if (mPairState.isOutgoing())
		{
			BT_DEBUG("Canceling outgoing pairing");
			if (mOutgoingPairingWatch)
				LSUtils::postToClient(mOutgoingPairingWatch->getMessage(), subscriptionResponseObj);
		}
		else if (mPairState.isIncoming())
		{
			BT_DEBUG("Canceling incoming pairing");
			if(mIncomingPairingWatch)
				LSUtils::postToClient(mIncomingPairingWatch->getMessage(), subscriptionResponseObj);
		}

		if (BLUETOOTH_ERROR_NONE == error)
			stopPairing();
	};

	BT_DEBUG("Initiating cancel pair call to the SIL for address %s", address.c_str());
	mDefaultAdapter->cancelPairing(address, cancelPairingCallback);

	return true;
}

bool BluetoothManagerService::unpair(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_2(PROP(address, string), PROP(adapterAddress, string)) REQUIRED_1(address));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!requestObj.hasKey("address"))
			LSUtils::respondWithError(request, BT_ERR_ADDR_PARAM_MISSING);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	std::string address = requestObj["address"].asString();
	auto device = findDevice(address);
	if (!device)
	{
		LSUtils::respondWithError(request, BT_ERR_DEVICE_NOT_AVAIL);
		return true;
	}

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);

	auto unpairCallback = [requestMessage, this, adapterAddress](BluetoothError error) {
		if (error != BLUETOOTH_ERROR_NONE)
		{
			LSUtils::respondWithError(requestMessage, BT_ERR_UNPAIR_FAIL);
			return;
		}

		pbnjson::JValue responseObj = pbnjson::Object();
		responseObj.put("returnValue", true);
		responseObj.put("adapterAddress", adapterAddress);

		LSUtils::postToClient(requestMessage, responseObj);

		LSMessageUnref(requestMessage);
	};

	mDefaultAdapter->unpair(address, unpairCallback);

	return true;
}

bool BluetoothManagerService::awaitPairingRequests(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_2(PROP_WITH_VAL_1(subscribe, boolean, true), PROP(adapterAddress, string)) REQUIRED_1(subscribe));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!request.isSubscription())
			LSUtils::respondWithError(request, BT_ERR_MTHD_NOT_SUBSCRIBED);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	if(mIncomingPairingWatch)
	{
		LSUtils::respondWithError(request, BT_ERR_ALLOW_ONE_SUBSCRIBE);
		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	mIncomingPairingWatch = new LSUtils::ClientWatch(get(), &message, [this]() {
		notifyPairingListenerDropped(true);
	});

	pbnjson::JValue responseObj = pbnjson::Object();

	if (setPairableState(true))
	{
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("subscribed", true);
		responseObj.put("returnValue", true);
		LSUtils::postToClient(mIncomingPairingWatch->getMessage(), responseObj);
	}
	else
	{
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("subscribed", false);
		responseObj.put("returnValue", false);
		responseObj.put("errorCode", (int32_t)BT_ERR_PAIRABLE_FAIL);
		responseObj.put("errorText", retrieveErrorText(BT_ERR_PAIRABLE_FAIL));
		LSUtils::postToClient(mIncomingPairingWatch->getMessage(), responseObj);
	}

	return true;
}

bool BluetoothManagerService::setWoBle(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_3(PROP(woBleEnabled, boolean), PROP(adapterAddress, string) , PROP(suspend, boolean)) REQUIRED_2(woBleEnabled, suspend));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!requestObj.hasKey("woBleEnabled"))
			LSUtils::respondWithError(request, BT_ERR_WOBLE_SET_WOBLE_PARAM_MISSING);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	BluetoothError error = BLUETOOTH_ERROR_NONE;
	bool woBleEnabled = false;
	bool suspend = false;

	if (requestObj.hasKey("suspend"))
		suspend = requestObj["suspend"].asBool();

	if (requestObj.hasKey("woBleEnabled"))
	{
		woBleEnabled = requestObj["woBleEnabled"].asBool();
		if (woBleEnabled)
			error = mDefaultAdapter->enableWoBle(suspend);
		else
			error = mDefaultAdapter->disableWoBle(suspend);
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	if (BLUETOOTH_ERROR_NONE == error)
	{
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("returnValue", true);

		mWoBleEnabled = woBleEnabled;
	}
	else
	{
		appendErrorResponse(responseObj, error);
	}

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::setWoBleTriggerDevices(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_2(ARRAY(triggerDevices, string), PROP(adapterAddress, string)) REQUIRED_1(triggerDevices));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!requestObj.hasKey("triggerDevices"))
			LSUtils::respondWithError(request, BT_ERR_WOBLE_SET_WOBLE_TRIGGER_DEVICES_PARAM_MISSING);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	BluetoothError error = BLUETOOTH_ERROR_NONE;
	BluetoothWoBleTriggerDeviceList triggerDevices;

	if (requestObj.hasKey("triggerDevices"))
	{
		auto tiggerDevicesObjArray = requestObj["triggerDevices"];
		for (int n = 0; n < tiggerDevicesObjArray.arraySize(); n++)
		{
			pbnjson::JValue element = tiggerDevicesObjArray[n];
			triggerDevices.push_back(element.asString());
		}

		error = mDefaultAdapter->setWoBleTriggerDevices(triggerDevices);
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	if (BLUETOOTH_ERROR_NONE == error)
	{
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("returnValue", true);

		mWoBleTriggerDevices.clear();
		mWoBleTriggerDevices.assign(triggerDevices.begin(),triggerDevices.end());
	}
	else
	{
		appendErrorResponse(responseObj, error);
	}

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::getWoBleStatus(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_1(PROP(adapterAddress, string)));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("adapterAddress", adapterAddress);
	responseObj.put("returnValue", true);
	responseObj.put("woBleEnabled", mWoBleEnabled);

	pbnjson::JValue triggerDevicesObj = pbnjson::Array();
	for (auto triggerDevice : mWoBleTriggerDevices)
	{
		triggerDevicesObj.append(triggerDevice);
	}
	responseObj.put("woBleTriggerDevices", triggerDevicesObj);

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::sendHciCommand(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_3(PROP(ogf, integer), PROP(ocf, integer), ARRAY(parameters, integer)) REQUIRED_3(ogf, ocf, parameters));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	uint16_t ogf = 0;
	uint16_t ocf = 0;
	BluetoothHCIParameterList parameters;

	if(requestObj.hasKey("ogf"))
		ogf = requestObj["ogf"].asNumber<int32_t>();

	if(requestObj.hasKey("ocf"))
		ocf = requestObj["ocf"].asNumber<int32_t>();

	if (requestObj.hasKey("parameters"))
	{
		auto parametersObjArray = requestObj["parameters"];
		for (int n = 0; n < parametersObjArray.arraySize(); n++)
		{
			pbnjson::JValue element = parametersObjArray[n];
			parameters.push_back(element.asNumber<int32_t>());
		}
	}

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);

	auto sendHciCommandCallback = [this, requestMessage, adapterAddress](BluetoothError error, uint16_t eventCode, BluetoothHCIParameterList parameters ) {
		LS::Message request(requestMessage);
		pbnjson::JValue responseObj = pbnjson::Object();
		if (error != BLUETOOTH_ERROR_NONE)
		{
			appendErrorResponse(responseObj, error);
			LSUtils::postToClient(requestMessage, responseObj);
			LSMessageUnref(requestMessage);
			return;
		}

		responseObj.put("returnValue", true);
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("eventCode", (int32_t)eventCode);

		pbnjson::JValue parametersArray = pbnjson::Array();
		for (size_t i=0; i < parameters.size(); i++)
					parametersArray.append((int32_t) parameters[i]);

		responseObj.put("eventParameters", parametersArray);

		LSUtils::postToClient(request, responseObj);
		LSMessageUnref(requestMessage);

	};
	mDefaultAdapter->sendHciCommand(ogf, ocf, parameters, sendHciCommandCallback);
	return true;
}

bool BluetoothManagerService::setTrace(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_7(
									PROP(stackTraceEnabled, boolean), PROP(snoopTraceEnabled, boolean),
									PROP(stackTraceLevel, integer), PROP(isTraceLogOverwrite, boolean),
									PROP(stackLogPath, string), PROP(snoopLogPath, string),
									PROP(adapterAddress, string)));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);
		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	BluetoothError error;

	if (requestObj.hasKey("stackTraceLevel"))
	{
		int stackTraceLevel = requestObj["stackTraceLevel"].asNumber<int32_t>();
		error = mDefaultAdapter->setStackTraceLevel(stackTraceLevel);

		if (error != BLUETOOTH_ERROR_NONE)
		{
			LSUtils::respondWithError(request, BT_ERR_STACK_TRACE_LEVEL_CHANGE_FAIL);
			return true;
		}
	}

	if (requestObj.hasKey("stackLogPath"))
	{
		std::string stackLogPath = requestObj["stackLogPath"].asString();
		error = mDefaultAdapter->setLogPath(TraceType::BT_TRACE_TYPE_STACK, stackLogPath);

		if (error != BLUETOOTH_ERROR_NONE)
		{
			LSUtils::respondWithError(request, BT_ERR_STACK_LOG_PATH_CHANGE_FAIL);
			return true;
		}
	}

	if (requestObj.hasKey("snoopLogPath"))
	{
		std::string snoopLogPath = requestObj["snoopLogPath"].asString();
		error = mDefaultAdapter->setLogPath(TraceType::BT_TRACE_TYPE_SNOOP, snoopLogPath);

		if (error != BLUETOOTH_ERROR_NONE)
		{
			LSUtils::respondWithError(request, BT_ERR_SNOOP_LOG_PATH_CHANGE_FAIL);
			return true;
		}
	}

	if (requestObj.hasKey("isTraceLogOverwrite"))
	{
		bool isTraceLogOverwrite = requestObj["isTraceLogOverwrite"].asBool();
		error = mDefaultAdapter->setTraceOverwrite(isTraceLogOverwrite);

		if (error != BLUETOOTH_ERROR_NONE)
		{
			LSUtils::respondWithError(request, BT_ERR_SNOOP_TRACE_STATE_CHANGE_FAIL);
			return true;
		}
	}

	if (requestObj.hasKey("snoopTraceEnabled"))
	{
		bool snoopTraceEnabled = requestObj["snoopTraceEnabled"].asBool();
		if (snoopTraceEnabled)
			error = mDefaultAdapter->enableTrace(TraceType::BT_TRACE_TYPE_SNOOP);
		else
			error = mDefaultAdapter->disableTrace(TraceType::BT_TRACE_TYPE_SNOOP);

		if (error != BLUETOOTH_ERROR_NONE)
		{
			LSUtils::respondWithError(request, BT_ERR_SNOOP_TRACE_STATE_CHANGE_FAIL);
			return true;
		}
	}

	if (requestObj.hasKey("stackTraceEnabled"))
	{
		bool stackTraceEnabled = requestObj["stackTraceEnabled"].asBool();
		if (stackTraceEnabled)
			error = mDefaultAdapter->enableTrace(TraceType::BT_TRACE_TYPE_STACK);
		else
			error = mDefaultAdapter->disableTrace(TraceType::BT_TRACE_TYPE_STACK);

		if (error != BLUETOOTH_ERROR_NONE)
		{
			LSUtils::respondWithError(request, BT_ERR_STACK_TRACE_STATE_CHANGE_FAIL);
			return true;
		}
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("adapterAddress", adapterAddress);
	responseObj.put("returnValue", true);


	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::getTraceStatus(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_1(PROP(adapterAddress, string)));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);

	auto getTraceStatusCallback  = [this, requestMessage, adapterAddress](BluetoothError error,
																bool stackTraceEnabled, bool snoopTraceEnabled,
																int stackTraceLevel,
																const std::string &stackLogPath,
																const std::string &snoopLogPath,
																bool  IsTraceLogOverwrite) {
			pbnjson::JValue responseObj = pbnjson::Object();
			if (error != BLUETOOTH_ERROR_NONE)
			{
				appendErrorResponse(responseObj, error);
				LSUtils::postToClient(requestMessage, responseObj);
				LSMessageUnref(requestMessage);
				return;
			}

			responseObj.put("returnValue", true);
			responseObj.put("adapterAddress", adapterAddress);
			responseObj.put("stackTraceEnabled", stackTraceEnabled);
			responseObj.put("snoopTraceEnabled", snoopTraceEnabled);
			responseObj.put("stackTraceLevel", stackTraceLevel);
			responseObj.put("stackLogPath", stackLogPath);
			responseObj.put("snoopLogPath", snoopLogPath);
			responseObj.put("IsTraceLogOverwrite", IsTraceLogOverwrite);

			LSUtils::postToClient(requestMessage, responseObj);
			LSMessageUnref(requestMessage);
	};
	mDefaultAdapter->getTraceStatus(getTraceStatusCallback);

	return true;
}

bool BluetoothManagerService::setKeepAlive(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_3(PROP(keepAliveEnabled, boolean), PROP(adapterAddress, string), PROP(keepAliveInterval, integer)));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);
		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	BluetoothError error = BLUETOOTH_ERROR_NONE;
	bool keepAliveEnabled = false;

	if (requestObj.hasKey("keepAliveInterval"))
	{
		int keepAliveInterval = requestObj["keepAliveInterval"].asNumber<int32_t>();
		error = mDefaultAdapter->setKeepAliveInterval(keepAliveInterval);

		if (error != BLUETOOTH_ERROR_NONE)
		{
			LSUtils::respondWithError(request, BT_ERR_KEEP_ALIVE_INTERVAL_CHANGE_FAIL);
			return true;
		}

		mKeepAliveInterval = (uint32_t)keepAliveInterval;
	}

	if (requestObj.hasKey("keepAliveEnabled"))
	{
		keepAliveEnabled = requestObj["keepAliveEnabled"].asBool();
		if (keepAliveEnabled != mKeepAliveEnabled)
		{
			if (keepAliveEnabled)
				error = mDefaultAdapter->enableKeepAlive();
			else
				error = mDefaultAdapter->disableKeepAlive();
		}
		else
			error = BLUETOOTH_ERROR_NONE;
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	if (BLUETOOTH_ERROR_NONE == error)
	{
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("returnValue", true);

		mKeepAliveEnabled = keepAliveEnabled;
	}
	else
	{
		appendErrorResponse(responseObj, error);
	}

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::getKeepAliveStatus(LSMessage &message)
{
	BT_INFO("MANAGER_SERVICE", 0, "Luna API is called : [%s : %d]", __FUNCTION__, __LINE__);

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;
	bool subscribed = false;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, SCHEMA_1(PROP(subscribe, boolean)), &parseError))
	{
		if (parseError == JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		else
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		return true;
	}

	pbnjson::JValue responseObj = pbnjson::Object();

	if (request.isSubscription())
	{
		mGetKeepAliveStatusSubscriptions.subscribe(request);
		subscribed = true;
	}

	responseObj.put("returnValue", true);
	responseObj.put("subscribed", subscribed);
	responseObj.put("adapterAddress", mAddress);

	if (subscribed)
	{
		responseObj.put("keepAliveEnabled", mKeepAliveEnabled);
		responseObj.put("keepAliveInterval", (int32_t)mKeepAliveInterval);
	}


	LSUtils::postToClient(request, responseObj);

	return true;
}

void BluetoothManagerService::requestPairingSecret(const std::string &address, BluetoothPairingSecretType type)
{
	pbnjson::JValue responseObj = pbnjson::Object();

	// If we're not pairing yet then this is a pairing request from a remote device
	if (!mPairState.isPairing())
	{
		beginIncomingPair(address);
	}

	if (type == BLUETOOTH_PAIRING_SECRET_TYPE_PASSKEY)
	{
		responseObj.put("adapterAddress", mAddress);
		responseObj.put("subscribed", true);
		responseObj.put("returnValue", true);
		responseObj.put("address", address);
		responseObj.put("request", "enterPasskey");
	}
	else if (type == BLUETOOTH_PAIRING_SECRET_TYPE_PIN)
	{
		responseObj.put("adapterAddress", mAddress);
		responseObj.put("subscribed", true);
		responseObj.put("returnValue", true);
		responseObj.put("address", address);
		responseObj.put("request", "enterPinCode");
	}

	if (mPairState.isIncoming() && mIncomingPairingWatch)
	{
		auto device = findDevice(address);
		if (device)
			responseObj.put("name", device->getName());

		LSUtils::postToClient(mIncomingPairingWatch->getMessage(), responseObj);
	}
	else if (mPairState.isOutgoing() && mOutgoingPairingWatch)
	{
		LSUtils::postToClient(mOutgoingPairingWatch->getMessage(), responseObj);
	}
	else
	{
		stopPairing();
	}
}

void BluetoothManagerService::displayPairingConfirmation(const std::string &address, BluetoothPasskey passkey)
{
	BT_DEBUG("Received display pairing confirmation request from SIL for address %s, passkey %d", address.c_str(), passkey);

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("request", "confirmPasskey");
	responseObj.put("passkey", (int) passkey);

	// If we're not pairing yet then this is a pairing request from a remote device
	if (!mPairState.isPairing())
	{
		beginIncomingPair(address);
		responseObj.put("address", address);
	}

	responseObj.put("adapterAddress", mAddress);
	responseObj.put("subscribed", true);
	responseObj.put("returnValue", true);

	if (mPairState.isIncoming() && mIncomingPairingWatch)
	{
		auto device = findDevice(address);
		if (device)
			responseObj.put("name", device->getName());

		LSUtils::postToClient(mIncomingPairingWatch->getMessage(), responseObj);
	}
	else if (mPairState.isOutgoing() && mOutgoingPairingWatch)
	{
		LSUtils::postToClient(mOutgoingPairingWatch->getMessage(), responseObj);
	}
	else
	{
		stopPairing();
	}
}

void BluetoothManagerService::pairingCanceled()
{
	BT_DEBUG ("Pairing has been canceled from remote user");
	if (!(mPairState.isPairing()))
		return;

	pbnjson::JValue subscriptionResponseObj = pbnjson::Object();
	subscriptionResponseObj.put("adapterAddress", mAddress);
	subscriptionResponseObj.put("subscribed", true);
	subscriptionResponseObj.put("returnValue", false);
	subscriptionResponseObj.put("request", "endPairing");
	subscriptionResponseObj.put("errorCode", (int32_t)BT_ERR_PAIRING_CANCEL_TO);
	subscriptionResponseObj.put("errorText", retrieveErrorText(BT_ERR_PAIRING_CANCEL_TO));

	if (mPairState.isIncoming() && mIncomingPairingWatch)
		LSUtils::postToClient(mIncomingPairingWatch->getMessage(), subscriptionResponseObj);

	if (mPairState.isOutgoing() && mOutgoingPairingWatch)
		LSUtils::postToClient(mOutgoingPairingWatch->getMessage(), subscriptionResponseObj);

	stopPairing();
}

void BluetoothManagerService::displayPairingSecret(const std::string &address, const std::string &pin)
{
	pbnjson::JValue responseObj = pbnjson::Object();

	// If we're not pairing yet then this is a pairing request from a remote device
	if (!mPairState.isPairing())
	{
		beginIncomingPair(address);
	}
	responseObj.put("adapterAddress", mAddress);
	responseObj.put("subscribed", true);
	responseObj.put("returnValue", true);
	responseObj.put("request", "displayPinCode");
	responseObj.put("address", address);
	responseObj.put("pin", pin);

	if (mPairState.isIncoming() && mIncomingPairingWatch)
	{
		auto device = findDevice(address);
		if (device)
			responseObj.put("name", device->getName());

		LSUtils::postToClient(mIncomingPairingWatch->getMessage(), responseObj);
	}
	else if (mPairState.isOutgoing() && mOutgoingPairingWatch)
	{
		LSUtils::postToClient(mOutgoingPairingWatch->getMessage(), responseObj);
	}
	else
	{
		stopPairing();
	}
}

void BluetoothManagerService::displayPairingSecret(const std::string &address, BluetoothPasskey passkey)
{
	pbnjson::JValue responseObj = pbnjson::Object();

	// If we're not pairing yet then this is a pairing request from a remote device
	if (!mPairState.isPairing())
	{
		beginIncomingPair(address);
	}
	responseObj.put("adapterAddress", mAddress);
	responseObj.put("subscribed", true);
	responseObj.put("returnValue", true);
	responseObj.put("request", "displayPasskey");
	responseObj.put("address", address);
	responseObj.put("passkey", (int)passkey);

	if (mPairState.isIncoming() && mIncomingPairingWatch)
	{
		auto device = findDevice(address);
		if (device)
			responseObj.put("name", device->getName());

		LSUtils::postToClient(mIncomingPairingWatch->getMessage(), responseObj);
	}
	else if (mPairState.isOutgoing() && mOutgoingPairingWatch)
	{
		LSUtils::postToClient(mOutgoingPairingWatch->getMessage(), responseObj);
	}
	else
	{
		stopPairing();
	}
}

void BluetoothManagerService::beginIncomingPair(const std::string &address)
{
	pbnjson::JValue responseObj = pbnjson::Object();

	BT_DEBUG("%s: address %s", __func__, address.c_str());

	if (mPairState.isPairing())
	{
		BT_WARNING(MSGID_INCOMING_PAIR_REQ_FAIL, 0, "Incoming pairing request received but cannot process since we are pairing with another device");
		return;
	}

	if (!mIncomingPairingWatch)
		return;

	auto device = findDevice(address);
	if (device)
	{
		mPairState.markAsIncoming();

		responseObj.put("adapterAddress", mAddress);
		responseObj.put("request", "incomingPairRequest");
		responseObj.put("address", address);
		responseObj.put("name", device->getName());
		responseObj.put("subscribed", true);
		responseObj.put("returnValue", true);
		LSUtils::postToClient(mIncomingPairingWatch->getMessage(), responseObj);

		startPairing(device);
	}
	else
	{
		responseObj.put("adapterAddress", mAddress);
		responseObj.put("subscribed", true);
		responseObj.put("returnValue", false);
		responseObj.put("errorText", retrieveErrorText(BT_ERR_INCOMING_PAIR_DEV_UNAVAIL));
		responseObj.put("errorCode", (int32_t)BT_ERR_INCOMING_PAIR_DEV_UNAVAIL);
		LSUtils::postToClient(mIncomingPairingWatch->getMessage(), responseObj);
	}
}

void BluetoothManagerService::abortPairing(bool incoming)
{
	bool cancelPairing = false;

	BT_DEBUG("Abort pairing");

	if (incoming)
	{
		// Pairable should always be true for a device with no input and output
		// simple pairs in that case

		/*
		 * Based on the problem described in PLAT-9396, we comment this part to
		 * maintain the pairing status even when user quit subscribing awaitPairingRequest
		 * Once EMS (Event Monitoring Service) is introduced in the build later, we can
		 * uncomment this part.
		 * For now, to maintin the functionality of incoming pairing using com.webos.service.bms,
		 * this routine will be commented. Check PLAT-9396 for more detail.
		 * PLAT-9808 is created to recover this later.
		if (mPairingIOCapability != BLUETOOTH_PAIRING_IO_CAPABILITY_NO_INPUT_NO_OUTPUT)
			setPairableState(false);

		if (mPairState.isPairing() && mPairState.isIncoming())
			cancelPairing = true;
		*/

		if (mIncomingPairingWatch)
		{
			delete mIncomingPairingWatch;
			mIncomingPairingWatch = 0;
		}
	}
	else
	{
		if (mPairState.isPairing() && mPairState.isOutgoing())
			cancelPairing = true;

		if (mOutgoingPairingWatch)
		{
			delete mOutgoingPairingWatch;
			mOutgoingPairingWatch = 0;
		}
	}

	if (cancelPairing)
	{
		// No need to call handleCancelResponse as callback, since we lost the subscriber and
		// we need not respond to the subscriber anymore.

		auto abortPairingCb = [this](BluetoothError error) {
			if (BLUETOOTH_ERROR_NONE == error)
			{
				BT_DEBUG("Pairing has been aborted");
			}
		};

		BluetoothDevice *device = mPairState.getDevice();
		if (device && mDefaultAdapter)
			mDefaultAdapter->cancelPairing(device->getAddress(), abortPairingCb);

		stopPairing();
	}
}

bool BluetoothManagerService::notifyPairingListenerDropped(bool incoming)
{
	BT_DEBUG("Pairing listener dropped (incoming %d)", incoming);

	if ((incoming && mIncomingPairingWatch) || (!incoming && mOutgoingPairingWatch))
		abortPairing(incoming);

	return true;
}

void BluetoothManagerService::notifyStartScanListenerDropped(uint32_t scanId)
{
	BT_DEBUG("StartScan listener dropped");

	auto watchIter = mStartScanWatches.find(scanId);
	if (watchIter == mStartScanWatches.end())
		return;

	LSUtils::ClientWatch *watch = watchIter->second;
	pbnjson::JValue responseObj = pbnjson::Object();

	responseObj.put("subscribed", false);
	responseObj.put("returnValue", false);
	responseObj.put("adapterAddress", mAddress);

	LSUtils::postToClient(watch->getMessage(), responseObj);

	mStartScanWatches.erase(watchIter);
	delete watch;

	mDefaultAdapter->removeLeDiscoveryFilter(scanId);

	if (mStartScanWatches.size() == 0)
		mDefaultAdapter->cancelLeDiscovery();
}

bool BluetoothManagerService::notifyAdvertisingDisabled(uint8_t advertiserId)
{
	notifySubscribersAdvertisingChanged(mAddress);

	BT_DEBUG("Advertiser(%d) disabled", advertiserId);

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("advertiserId", advertiserId);
	responseObj.put("subscribed", false);
	responseObj.put("adapterAddress", mAddress);
	responseObj.put("returnValue", true);
	LSUtils::postToClient(mAdvertisingWatch->getMessage(), responseObj);

	return true;
}

bool BluetoothManagerService::notifyAdvertisingDropped(uint8_t advertiserId)
{
	BT_DEBUG("Advertiser(%d) dropped", advertiserId);

	std::string adapterAddress = mAddress;
	auto leAdvEnableCallback = [this, advertiserId, adapterAddress](BluetoothError enableError)
	{
		auto unregisterAdvCallback = [this,adapterAddress, advertiserId](BluetoothError registerError)
		{
			pbnjson::JValue responseObj = pbnjson::Object();

			if (BLUETOOTH_ERROR_NONE == registerError)
			{
				notifySubscribersAdvertisingChanged(adapterAddress);
				responseObj.put("advertiserId", advertiserId);
			}
			else
			{
				appendErrorResponse(responseObj, registerError);
			}

			responseObj.put("adapterAddress", mAddress);
			responseObj.put("subscribed", false);
			responseObj.put("returnValue", true);
			LSUtils::postToClient(mAdvertisingWatch->getMessage(), responseObj);
		};
		mDefaultAdapter->unregisterAdvertiser(advertiserId, unregisterAdvCallback);

		if (enableError != BLUETOOTH_ERROR_NONE)
		{
			pbnjson::JValue responseObj = pbnjson::Object();
			responseObj.put("adapterAddress", adapterAddress);
			appendErrorResponse(responseObj, enableError);
			LSUtils::postToClient(mAdvertisingWatch->getMessage(), responseObj);
		}
	};

	mDefaultAdapter->disableAdvertiser(advertiserId, leAdvEnableCallback);
	return true;
}

void BluetoothManagerService::cancelDiscoveryCallback(BluetoothDevice *device, BluetoothError error)
{
	pbnjson::JValue responseObj = pbnjson::Object();

	if (error != BLUETOOTH_ERROR_NONE)
	{
		BT_DEBUG("%s: Error is %d", __func__, error);
		if (mPairState.isOutgoing() && mOutgoingPairingWatch)
		{
			responseObj.put("adapterAddress", mAddress);
			responseObj.put("subscribed", false);
			responseObj.put("returnValue", false);
			responseObj.put("errorText", retrieveErrorText(BT_ERR_STOP_DISC_FAIL));
			responseObj.put("errorCode", (int32_t)BT_ERR_STOP_DISC_FAIL);
			LSUtils::postToClient(mOutgoingPairingWatch->getMessage(), responseObj);

			stopPairing();

			delete mOutgoingPairingWatch;
			mOutgoingPairingWatch = 0;
		}

		if (mPairState.isIncoming() && mIncomingPairingWatch)
		{
			responseObj.put("adapterAddress", mAddress);
			responseObj.put("subscribed", true);
			responseObj.put("returnValue", false);
			responseObj.put("errorText", retrieveErrorText(BT_ERR_STOP_DISC_FAIL));
			responseObj.put("errorCode", (int32_t)BT_ERR_STOP_DISC_FAIL);
			LSUtils::postToClient(mIncomingPairingWatch->getMessage(), responseObj);
		}
	}
	else
	{
		BT_DEBUG("%s: No error", __func__);
		if (mPairState.isOutgoing() && mOutgoingPairingWatch)
		{
			// Make sure discovery is canceled
			if (!getDiscoveringState())
			{
				BT_DEBUG("%s: Discovery state is disabled", __func__);
				std::string address = device->getAddress();
				auto pairCallback = [this](BluetoothError error)
				{
					pbnjson::JValue responseObj = pbnjson::Object();
					BT_DEBUG("Outgoing pairing process finished");

					if (!mPairState.isPairing())
						return;

					if (BLUETOOTH_ERROR_NONE == error)
					{
						responseObj.put("adapterAddress", mAddress);
						responseObj.put("subscribed", false);
						responseObj.put("returnValue", true);
						responseObj.put("request", "endPairing");
					}
					else
					{
						responseObj.put("adapterAddress", mAddress);
						responseObj.put("subscribed", false);
						responseObj.put("request", "endPairing");
						appendErrorResponse(responseObj, error);
					}
					stopPairing();

					if (mOutgoingPairingWatch)
					{
						LSUtils::postToClient(mOutgoingPairingWatch->getMessage(), responseObj);
						delete mOutgoingPairingWatch;
						mOutgoingPairingWatch = 0;
					}
				};

				getDefaultAdapter()->pair(address, pairCallback);
			}
			else
			{
				BT_DEBUG("%s: No error, but discovery state is still enabled", __func__);
				responseObj.put("adapterAddress", mAddress);
				responseObj.put("subscribed", false);
				responseObj.put("returnValue", false);
				responseObj.put("errorText", retrieveErrorText(BT_ERR_STOP_DISC_FAIL));
				responseObj.put("errorCode", (int32_t)BT_ERR_STOP_DISC_FAIL);

				stopPairing();

				LSUtils::postToClient(mOutgoingPairingWatch->getMessage(), responseObj);
				delete mOutgoingPairingWatch;
				mOutgoingPairingWatch = 0;
			}
		}
	}
}

void BluetoothManagerService::startPairing(BluetoothDevice *device)
{
	mPairState.startPairing(device);
	notifySubscribersAboutStateChange();
	notifySubscribersFilteredDevicesChanged();
	notifySubscribersDevicesChanged();

	// Device discovery needs to be stopped for pairing
	mDefaultAdapter->cancelDiscovery(std::bind(&BluetoothManagerService::cancelDiscoveryCallback, this, device, _1));
}

void BluetoothManagerService::stopPairing()
{
	mPairState.stopPairing();

	notifySubscribersAboutStateChange();
	notifySubscribersFilteredDevicesChanged();
	notifySubscribersDevicesChanged();
}

void BluetoothManagerService::cancelIncomingPairingSubscription()
{
	BT_DEBUG("Cancel incoming pairing subscription since pairable timeout has reached");

	// Pairable should always be true for a device with no input and output - simple pairs in that case
	if (mPairState.isPairable() && (mPairingIOCapability != BLUETOOTH_PAIRING_IO_CAPABILITY_NO_INPUT_NO_OUTPUT))
	{
		if (mIncomingPairingWatch)
		{
			pbnjson::JValue responseObj = pbnjson::Object();
			responseObj.put("adapterAddress", mAddress);
			responseObj.put("subscribed", false);
			responseObj.put("returnValue", false);
			responseObj.put("errorText", retrieveErrorText(BT_ERR_PAIRABLE_TO));
			responseObj.put("errorCode", (int32_t)BT_ERR_PAIRABLE_TO);
			LSUtils::postToClient(mIncomingPairingWatch->getMessage(), responseObj);

			delete mIncomingPairingWatch;
			mIncomingPairingWatch = 0;
		}

		setPairableState(false);
		if (mPairState.isPairing())
			stopPairing();
	}
}

bool BluetoothManagerService::getPowered()
{
	return mPowered;
}


bool BluetoothManagerService::configureAdvertisement(LSMessage &message)
{

	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema = STRICT_SCHEMA(PROPS_9(PROP(adapterAddress, string), PROP(connectable, boolean), PROP(includeTxPower, boolean),
                                                     PROP(TxPower,integer), PROP(includeName, boolean), PROP(isScanResponse, boolean),
													 ARRAY(manufacturerData, integer),
                                                     OBJARRAY(services, OBJSCHEMA_2(PROP(uuid, string), ARRAY(data,integer))),
			                                         OBJARRAY(proprietaryData, OBJSCHEMA_2(PROP(type, integer), ARRAY(data, integer)))));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);
		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);
		return true;
	}

	std::string adapterAddress;
	bool connectable = true;
	bool includeTxPower = false;
	bool includeName = false;
	bool isScanResponse = false;
	uint8_t TxPower = 0x00;

	if(requestObj.hasKey("adapterAddress"))
		adapterAddress = requestObj["adapterAddress"].asString();
	else
		adapterAddress = mAddress;

	if (!isAdapterAvailable(adapterAddress))
	{
		LSUtils::respondWithError(request, BT_ERR_INVALID_ADAPTER_ADDRESS);
		return true;
	}

	if(requestObj.hasKey("connectable"))
		connectable = requestObj["connectable"].asBool();
	else
		connectable = true;

	if(requestObj.hasKey("includeTxPower"))
		includeTxPower = requestObj["includeTxPower"].asBool();

	if(requestObj.hasKey("TxPower"))
		TxPower = (uint8_t) requestObj["TxPower"].asNumber<int32_t>();

	if(requestObj.hasKey("includeName"))
		includeName = requestObj["includeName"].asBool();

	if(requestObj.hasKey("isScanResponse"))
		isScanResponse = requestObj["isScanResponse"].asBool();

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);

	if ((!requestObj.hasKey("manufacturerData") && !requestObj.hasKey("services") && !requestObj.hasKey("proprietaryData") && !isScanResponse))
	{
		pbnjson::JValue responseObj = pbnjson::Object();
		responseObj.put("adapterAddress", adapterAddress);
		responseObj.put("returnValue", false);
		responseObj.put("errorText",retrieveErrorText(BT_ERR_BLE_ADV_CONFIG_DATA_PARAM_MISSING));
		responseObj.put("errorCode", BT_ERR_BLE_ADV_CONFIG_DATA_PARAM_MISSING);

		LSUtils::postToClient(request,responseObj);
		return true;
	}

	BluetoothLowEnergyServiceList serviceList;
	BluetoothLowEnergyData manufacturerData;
	ProprietaryDataList proprietaryDataList;
	bool serviceDataFound = false;

	if(requestObj.hasKey("services"))
	{
		auto servicesObjArray = requestObj["services"];
		for(int i = 0; i < servicesObjArray.arraySize(); i++)
		{
			auto serviceObj = servicesObjArray[i];
			if(serviceObj.hasKey("data") && !serviceDataFound)
			{
				auto serviceDataArray = serviceObj["data"];
				BluetoothLowEnergyData serviceData;

				for(int j = 0; j < serviceDataArray.arraySize(); j++)
				{
					pbnjson::JValue element = serviceDataArray[j];
					serviceData.push_back((uint8_t)element.asNumber<int32_t>());
				}

				if(serviceObj.hasKey("uuid"))
				{
					serviceList[serviceObj["uuid"].asString()] = serviceData;
					serviceDataFound = true;
				}
				else
				{
					pbnjson::JValue responseObj = pbnjson::Object();
					responseObj.put("adapterAddress", adapterAddress);
					responseObj.put("returnValue", false);
					responseObj.put("errorText",retrieveErrorText(BT_ERR_BLE_ADV_UUID_FAIL));
					responseObj.put("errorCode", BT_ERR_BLE_ADV_UUID_FAIL);

					LSUtils::postToClient(request,responseObj);
					return true;
				}
			}
			else if(serviceObj.hasKey("data") && serviceDataFound)
			{
				pbnjson::JValue responseObj = pbnjson::Object();
				responseObj.put("adapterAddress", adapterAddress);
				responseObj.put("returnValue", false);
				responseObj.put("errorText",retrieveErrorText(BT_ERR_BLE_ADV_SERVICE_DATA_FAIL));
				responseObj.put("errorCode", BT_ERR_BLE_ADV_SERVICE_DATA_FAIL);

				LSUtils::postToClient(request,responseObj);
				return true;
			}
			else
			{
				serviceList[serviceObj["uuid"].asString()];
			}
		}
	}

	if (requestObj.hasKey("manufacturerData"))
	{
		auto manufacturerDataArray = requestObj["manufacturerData"];
		for(int i = 0; i < manufacturerDataArray.arraySize(); i++)
		{
			pbnjson::JValue element = manufacturerDataArray[i];
			manufacturerData.push_back((uint8_t)element.asNumber<int32_t>());
		}
	}

	if (requestObj.hasKey("proprietaryData"))
	{
		auto proprietaryObjArray = requestObj["proprietaryData"];
		for(int i = 0; i < proprietaryObjArray.arraySize(); i++)
		{
			ProprietaryData proprietaryData;
			auto proprietaryObj = proprietaryObjArray[i];
			proprietaryData.type = (uint8_t)(proprietaryObj["type"].asNumber<int32_t>());

			auto proprietaryArray = proprietaryObj["data"];
			for(int j = 0; j < proprietaryArray.arraySize(); j++)
			{
				pbnjson::JValue element = proprietaryArray[j];
				proprietaryData.data.push_back((uint8_t)element.asNumber<int32_t>());
			}
			proprietaryDataList.push_back(proprietaryData);
		}
	}

	auto leConfigCallback = [this,requestMessage,adapterAddress](BluetoothError error) {
		pbnjson::JValue responseObj = pbnjson::Object();

		if (BLUETOOTH_ERROR_NONE == error)
		{
			responseObj.put("adapterAddress", adapterAddress);
			responseObj.put("returnValue", true);
		}
		else
		{
			responseObj.put("adapterAddress", adapterAddress);
			appendErrorResponse(responseObj, error);
		}
		LSUtils::postToClient(requestMessage, responseObj);
		LSMessageUnref(requestMessage);
	};
	mDefaultAdapter->configureAdvertisement(connectable, includeTxPower, includeName, isScanResponse,
	                                        manufacturerData, serviceList, proprietaryDataList, leConfigCallback, TxPower);
	return true;

}

bool BluetoothManagerService::setAdvertiseData(LSMessage &message, pbnjson::JValue &value, AdvertiseData &data, bool isScanRsp)
{
	LS::Message request(&message);
	AdvertiseData *advData = &data;

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);

	BluetoothLowEnergyServiceList serviceList;
	auto advertiseObj = isScanRsp ? value["scanResponse"] : value["advertiseData"];
	if(advertiseObj.hasKey("services"))
	{
		bool serviceDataFound = false;
		auto servicesObjArray = advertiseObj["services"];
		for(int i = 0; i < servicesObjArray.arraySize(); i++)
		{
			auto serviceObj = servicesObjArray[i];
			if(serviceObj.hasKey("data") && !serviceDataFound)
			{
				auto serviceDataArray = serviceObj["data"];
				BluetoothLowEnergyData serviceData;
				for(int j = 0; j < serviceDataArray.arraySize(); j++)
				{
					pbnjson::JValue element = serviceDataArray[j];
					serviceData.push_back((uint8_t)element.asNumber<int32_t>());
				}
				if(serviceObj.hasKey("uuid"))
				{
					serviceList[serviceObj["uuid"].asString()] = serviceData;
					serviceDataFound = true;
				}
				else
				{
					pbnjson::JValue responseObj = pbnjson::Object();
					responseObj.put("adapterAddress", mAddress);
					responseObj.put("returnValue", false);
					responseObj.put("errorText",retrieveErrorText(BT_ERR_BLE_ADV_UUID_FAIL));
					responseObj.put("errorCode", BT_ERR_BLE_ADV_UUID_FAIL);

					LSUtils::postToClient(requestMessage,responseObj);
					LSMessageUnref(requestMessage);
					return false;
				}
			}
			else if(serviceObj.hasKey("data") && serviceDataFound)
			{
				pbnjson::JValue responseObj = pbnjson::Object();
				responseObj.put("adapterAddress", mAddress);
				responseObj.put("returnValue", false);
				responseObj.put("errorText",retrieveErrorText(BT_ERR_BLE_ADV_SERVICE_DATA_FAIL));
				responseObj.put("errorCode", BT_ERR_BLE_ADV_SERVICE_DATA_FAIL);

				LSUtils::postToClient(requestMessage,responseObj);
				LSMessageUnref(requestMessage);
				return false;
			}
			else
			{
				serviceList[serviceObj["uuid"].asString()];
			}
		}
		advData->services = serviceList;
	}

	if (advertiseObj.hasKey("manufacturerData"))
	{
		auto manufacturerDataArray = advertiseObj["manufacturerData"];
		for(int i = 0; i < manufacturerDataArray.arraySize(); i++)
		{
			pbnjson::JValue element = manufacturerDataArray[i];
			advData->manufacturerData.push_back((uint8_t)element.asNumber<int32_t>());
		}
	}

	if (advertiseObj.hasKey("proprietaryData"))
	{
		auto proprietaryObjArray = advertiseObj["proprietaryData"];
		for(int i = 0; i < proprietaryObjArray.arraySize(); i++)
		{
			ProprietaryData proprietaryData;
			auto proprietaryObj = proprietaryObjArray[i];
			proprietaryData.type = (uint8_t)(proprietaryObj["type"].asNumber<int32_t>());

			auto proprietaryArray = proprietaryObj["data"];
			for(int j = 0; j < proprietaryArray.arraySize(); j++)
			{
				pbnjson::JValue element = proprietaryArray[j];
				proprietaryData.data.push_back((uint8_t)element.asNumber<int32_t>());
			}
			advData->proprietaryData.push_back(proprietaryData);
		}
	}

	if (advertiseObj.hasKey("includeTxPower"))
	{
		advData->includeTxPower = advertiseObj["includeTxPower"].asBool();
	}

	if (advertiseObj.hasKey("includeName"))
	{
		if(advertiseObj["includeName"].asBool())
		{
			if(!isScanRsp)
			{
				pbnjson::JValue responseObj = pbnjson::Object();
				responseObj.put("adapterAddress", mAddress);
				responseObj.put("returnValue", false);
				responseObj.put("errorText",retrieveErrorText(BT_ERR_BLE_ADV_CONFIG_FAIL));
				responseObj.put("errorCode", BT_ERR_BLE_ADV_CONFIG_FAIL);

				LSUtils::postToClient(requestMessage,responseObj);
				LSMessageUnref(requestMessage);
			}
			else
			{
				advData->includeName = advertiseObj["includeName"].asBool();
			}
		}
		else
			advData->includeName = false;
	}
	return true;
}

bool BluetoothManagerService::startAdvertising(LSMessage &message)
{
	BT_DEBUG("BluetoothManagerService::%s %d \n", __FUNCTION__, __LINE__);
	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema =  STRICT_SCHEMA(PROPS_5(PROP(adapterAddress, string), PROP(subscribe, boolean),
											  OBJECT(settings, OBJSCHEMA_5(PROP(connectable, boolean), PROP(txPower, integer),
													  PROP(minInterval, integer), PROP(maxInterval, integer), PROP(timeout, integer))),
											  OBJECT(advertiseData, OBJSCHEMA_5(PROP(includeTxPower, boolean), PROP(includeName, boolean),
													  ARRAY(manufacturerData, integer), OBJARRAY(services, OBJSCHEMA_2(PROP(uuid, string),ARRAY(data,integer))),
													  OBJARRAY(proprietaryData, OBJSCHEMA_2(PROP(type, integer), ARRAY(data, integer))))),
											  OBJECT(scanResponse, OBJSCHEMA_5(PROP(includeTxPower, boolean), PROP(includeName, boolean),
													  ARRAY(manufacturerData, integer), OBJARRAY(services, OBJSCHEMA_2(PROP(uuid, string),ARRAY(data,integer))),
													  OBJARRAY(proprietaryData, OBJSCHEMA_2(PROP(type, integer), ARRAY(data, integer)))))));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}
	std::string adapterAddress;
	AdvertiserInfo advInfo;
	memset(&advInfo, 0, sizeof(AdvertiserInfo));
	//Assign default value true
	advInfo.settings.connectable = true;
	BT_DEBUG("BluetoothManagerService::%s %d advertiseData.includeTxPower:%d", __FUNCTION__, __LINE__, advInfo.advertiseData.includeTxPower);
	BT_DEBUG("BluetoothManagerService::%s %d scanResponse.includeTxPower:%d", __FUNCTION__, __LINE__, advInfo.scanResponse.includeTxPower);

	if(requestObj.hasKey("adapterAddress"))
		adapterAddress = requestObj["adapterAddress"].asString();
	else
		adapterAddress = mAddress;

	if (requestObj.hasKey("settings"))
	{
		auto settingsObj = requestObj["settings"];
		if (settingsObj.hasKey("connectable"))
			advInfo.settings.connectable = settingsObj["connectable"].asBool();

		if (settingsObj.hasKey("minInterval"))
			advInfo.settings.minInterval = settingsObj["minInterval"].asNumber<int32_t>();

		if (settingsObj.hasKey("maxInterval"))
			advInfo.settings.maxInterval = settingsObj["maxInterval"].asNumber<int32_t>();

		if (settingsObj.hasKey("txPower"))
			advInfo.settings.txPower = settingsObj["txPower"].asNumber<int32_t>();

		if (settingsObj.hasKey("timeout"))
			advInfo.settings.timeout = settingsObj["timeout"].asNumber<int32_t>();
	}

	if(requestObj.hasKey("advertiseData"))
	{
		if(!setAdvertiseData(message, requestObj,advInfo.advertiseData, false))
			return true;
	}

	if(requestObj.hasKey("scanResponse"))
	{
		if(!setAdvertiseData(message, requestObj, advInfo.scanResponse, true))
			return true;
	}

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);

	if (requestObj.hasKey("settings") || requestObj.hasKey("advertiseData") || requestObj.hasKey("advertiseData"))
	{
		mAdvertisingWatch = new LSUtils::ClientWatch(get(), &message, nullptr);
		auto leRegisterAdvCallback = [this,requestMessage,advInfo,adapterAddress](BluetoothError error, uint8_t advertiserId) {

			pbnjson::JValue responseObj = pbnjson::Object();
			if (BLUETOOTH_ERROR_NONE == error)
			{
				auto leStartAdvCallback = [this,requestMessage,advertiserId,adapterAddress](BluetoothError error) {
					pbnjson::JValue responseObj = pbnjson::Object();

					if (BLUETOOTH_ERROR_NONE == error)
					{
						responseObj.put("adapterAddress", adapterAddress);
						responseObj.put("returnValue", true);
						responseObj.put("advertiserId", advertiserId);
						notifySubscribersAdvertisingChanged(adapterAddress);
					}
					else
					{
						responseObj.put("adapterAddress", adapterAddress);
						appendErrorResponse(responseObj, error);
					}
					LSUtils::postToClient(requestMessage, responseObj);
					LSMessageUnref(requestMessage);
				};

				LS::Message request(requestMessage);
				if(request.isSubscription())
					mAdvertisingWatch->setCallback(std::bind(&BluetoothManagerService::notifyAdvertisingDropped, this, advertiserId));

				mDefaultAdapter->startAdvertising(advertiserId, advInfo.settings, advInfo.advertiseData, advInfo.scanResponse, leStartAdvCallback);
			}
			else
			{
				responseObj.put("adapterAddress", adapterAddress);
				appendErrorResponse(responseObj, error);
				LSUtils::postToClient(requestMessage, responseObj);
				LSMessageUnref(requestMessage);
			}
		};

		if (getAdvSize(advInfo.advertiseData, true) > MAX_ADVERTISING_DATA_BYTES ||
						getAdvSize(advInfo.scanResponse, false) > MAX_ADVERTISING_DATA_BYTES)
		{
			LSUtils::respondWithError(request, BT_ERR_BLE_ADV_EXCEED_SIZE_LIMIT);
			return true;
		}

		mDefaultAdapter->registerAdvertiser(leRegisterAdvCallback);
	}
	else
	{
		auto leStartAdvCallback = [this,requestMessage,adapterAddress](BluetoothError error) {

			pbnjson::JValue responseObj = pbnjson::Object();

			if (BLUETOOTH_ERROR_NONE == error)
			{
				responseObj.put("adapterAddress", adapterAddress);
				responseObj.put("returnValue", true);
				mAdvertising = true;

				notifySubscribersAdvertisingChanged(adapterAddress);
			}
			else
			{
				responseObj.put("adapterAddress", adapterAddress);
				appendErrorResponse(responseObj, error);
			}

			LSUtils::postToClient(requestMessage, responseObj);
			LSMessageUnref(requestMessage);
		};

		mDefaultAdapter->startAdvertising(leStartAdvCallback);
	}

	return true;

}

bool BluetoothManagerService::disableAdvertising(LSMessage &message)
{
	BT_DEBUG("BluetoothManagerService::%s %d \n", __FUNCTION__, __LINE__);
	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema =  STRICT_SCHEMA(PROPS_2(PROP(adapterAddress, string), PROP(advertiserId, integer)) REQUIRED_1(advertiserId));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else if (!requestObj.hasKey("advertiserId"))
			LSUtils::respondWithError(request, BT_ERR_GATT_ADVERTISERID_PARAM_MISSING);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;

	uint8_t advertiserId = (uint8_t)requestObj["advertiserId"].asNumber<int32_t>();

	if(requestObj.hasKey("adapterAddress"))
		adapterAddress = requestObj["adapterAddress"].asString();
	else
		adapterAddress = mAddress;

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);

	auto leAdvEnableCallback = [this, advertiserId, adapterAddress](BluetoothError error)
	{
		auto unregisterAdvCallback = [this,adapterAddress, advertiserId](BluetoothError error)
		{
			pbnjson::JValue responseObj = pbnjson::Object();

			if (BLUETOOTH_ERROR_NONE == error)
			{
				notifySubscribersAdvertisingChanged(adapterAddress);
				responseObj.put("advertiserId", advertiserId);
			}
			else
			{
				appendErrorResponse(responseObj, error);
			}

			responseObj.put("adapterAddress", mAddress);
			responseObj.put("subscribed", false);
			responseObj.put("returnValue", true);
			LSUtils::postToClient(mAdvertisingWatch->getMessage(), responseObj);
		};
		mDefaultAdapter->unregisterAdvertiser(advertiserId, unregisterAdvCallback);

		if (error != BLUETOOTH_ERROR_NONE)
		{
			pbnjson::JValue responseObj = pbnjson::Object();
			responseObj.put("adapterAddress", adapterAddress);
			appendErrorResponse(responseObj, error);
			LSUtils::postToClient(mAdvertisingWatch->getMessage(), responseObj);
		}
	};

	mDefaultAdapter->disableAdvertiser(advertiserId, leAdvEnableCallback);

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("advertiserId", advertiserId);
	responseObj.put("adapterAddress", mAddress);
	responseObj.put("returnValue", true);
	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::updateAdvertising(LSMessage &message)
{
	BT_DEBUG("BluetoothManagerService::%s %d \n", __FUNCTION__, __LINE__);
	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;
	bool isSettingsChanged = false;
	bool isAdvDataChanged = false;
	bool isScanRspChanged = false;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema =  STRICT_SCHEMA(PROPS_5(PROP(adapterAddress, string), PROP(advertiserId, integer),
											  OBJECT(settings, OBJSCHEMA_5(PROP(connectable, boolean), PROP(txPower, integer),
													  PROP(minInterval, integer), PROP(maxInterval, integer), PROP(timeout, integer))),
											  OBJECT(advertiseData, OBJSCHEMA_5(PROP(includeTxPower, boolean), PROP(includeName, boolean),
													  ARRAY(manufacturerData, integer), OBJARRAY(services, OBJSCHEMA_2(PROP(uuid, string),ARRAY(data,integer))),
													  OBJARRAY(proprietaryData, OBJSCHEMA_2(PROP(type, integer), ARRAY(data, integer))))),
											  OBJECT(scanResponse, OBJSCHEMA_5(PROP(includeTxPower, boolean), PROP(includeName, boolean),
													  ARRAY(manufacturerData, integer), OBJARRAY(services, OBJSCHEMA_2(PROP(uuid, string),ARRAY(data,integer))),
													  OBJARRAY(proprietaryData, OBJSCHEMA_2(PROP(type, integer), ARRAY(data, integer)))))) REQUIRED_1(advertiserId));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}
	std::string adapterAddress;
	AdvertiserInfo advInfo;
	memset(&advInfo, 0, sizeof(AdvertiserInfo));
	BT_DEBUG("BluetoothManagerService::%s %d advertiseData.includeTxPower:%d", __FUNCTION__, __LINE__, advInfo.advertiseData.includeTxPower);
	BT_DEBUG("BluetoothManagerService::%s %d scanResponse.includeTxPower:%d", __FUNCTION__, __LINE__, advInfo.scanResponse.includeTxPower);

	uint8_t advertiserId = (uint8_t)requestObj["advertiserId"].asNumber<int32_t>();

	if(requestObj.hasKey("adapterAddress"))
		adapterAddress = requestObj["adapterAddress"].asString();
	else
		adapterAddress = mAddress;

	if (requestObj.hasKey("settings"))
	{
		isSettingsChanged = true;
		auto settingsObj = requestObj["settings"];
		if (settingsObj.hasKey("connectable"))
			advInfo.settings.connectable = settingsObj["connectable"].asBool();

		if (settingsObj.hasKey("minInterval"))
			advInfo.settings.minInterval = settingsObj["minInterval"].asNumber<int32_t>();

		if (settingsObj.hasKey("maxInterval"))
			advInfo.settings.maxInterval = settingsObj["maxInterval"].asNumber<int32_t>();

		if (settingsObj.hasKey("txPower"))
			advInfo.settings.txPower = settingsObj["txPower"].asNumber<int32_t>();

		if (settingsObj.hasKey("timeout"))
			advInfo.settings.timeout = settingsObj["timeout"].asNumber<int32_t>();
	}

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);
	auto leUpdateAdvCallback = [this,requestMessage,adapterAddress](BluetoothError error) {
		if (BLUETOOTH_ERROR_NONE != error)
		{
			pbnjson::JValue responseObj = pbnjson::Object();
			responseObj.put("adapterAddress", adapterAddress);
			appendErrorResponse(responseObj, error);
			LSUtils::postToClient(requestMessage, responseObj);
			LSMessageUnref(requestMessage);
		}
	};

	if(requestObj.hasKey("advertiseData"))
	{
		isAdvDataChanged = true;
		if(!setAdvertiseData(message, requestObj,advInfo.advertiseData, false))
			return true;

		mDefaultAdapter->setAdvertiserData(advertiserId, false, advInfo.advertiseData, leUpdateAdvCallback);
	}

	if(requestObj.hasKey("scanResponse"))
	{
		isScanRspChanged = true;
		if(!setAdvertiseData(message, requestObj, advInfo.scanResponse, true))
			return true;

		mDefaultAdapter->setAdvertiserData(advertiserId, true, advInfo.scanResponse, leUpdateAdvCallback);
	}

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("advertiserId", advertiserId);
	responseObj.put("adapterAddress", mAddress);
	responseObj.put("returnValue", true);
	LSUtils::postToClient(request, responseObj);

	return true;
}

void BluetoothManagerService::updateAdvertiserData(LSMessage *requestMessage, uint8_t advertiserId, AdvertiserInfo advInfo,
		bool isSettingsChanged, bool isAdvDataChanged, bool isScanRspChanged)
{
	if(isSettingsChanged)
	{
		auto leAdvSettingCallback = [this,requestMessage,advertiserId,isAdvDataChanged,isScanRspChanged,advInfo](BluetoothError error)
		{
			pbnjson::JValue responseObj = pbnjson::Object();
			if (BLUETOOTH_ERROR_NONE == error)
			{
				notifySubscribersAdvertisingChanged(mAddress);
			}
			else
			{
				responseObj.put("adapterAddress", mAddress);
				appendErrorResponse(responseObj, error);
				LSUtils::postToClient(requestMessage, responseObj);
				LSMessageUnref(requestMessage);
			}
		};

		mDefaultAdapter->setAdvertiserParameters(advertiserId, advInfo.settings, leAdvSettingCallback);
	}

	if(isAdvDataChanged)
	{
		auto leAdvDataChangedCallback = [this,requestMessage,advertiserId,isScanRspChanged,advInfo](BluetoothError error)
		{
			pbnjson::JValue responseObj = pbnjson::Object();
			if (BLUETOOTH_ERROR_NONE == error)
			{
				notifySubscribersAdvertisingChanged(mAddress);
			}
			else
			{
				responseObj.put("adapterAddress", mAddress);
				appendErrorResponse(responseObj, error);
				LSUtils::postToClient(requestMessage, responseObj);
				LSMessageUnref(requestMessage);
			}
		};

		mDefaultAdapter->setAdvertiserData(advertiserId, false, advInfo.advertiseData, leAdvDataChangedCallback);
	}

	if(isScanRspChanged)
	{
		auto leScanRspChangedCallback = [this,requestMessage,advertiserId,advInfo](BluetoothError error)
		{
			pbnjson::JValue responseObj = pbnjson::Object();
			if (BLUETOOTH_ERROR_NONE == error)
			{
				notifySubscribersAdvertisingChanged(mAddress);
			}
			else
			{
				responseObj.put("adapterAddress", mAddress);
				appendErrorResponse(responseObj, error);
				LSUtils::postToClient(requestMessage, responseObj);
				LSMessageUnref(requestMessage);
			}
		};
		mDefaultAdapter->setAdvertiserData(advertiserId, true, advInfo.scanResponse, leScanRspChangedCallback);
	}
}

bool BluetoothManagerService::stopAdvertising(LSMessage &message)
{
	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema =  STRICT_SCHEMA(PROPS_1(PROP(adapterAddress, string)));

	if (!LSUtils::parsePayload(request.getPayload(), requestObj, schema, &parseError))
	{
		if (parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else
			LSUtils::respondWithError(request, BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress = mAddress;

	if(requestObj.hasKey("adapterAddress"))
	{
		adapterAddress = requestObj["adapterAddress"].asString();
	}

	LSMessage *requestMessage = request.get();
	LSMessageRef(requestMessage);

	auto leStopAdvCallback = [this,requestMessage,adapterAddress](BluetoothError error) {

		pbnjson::JValue responseObj = pbnjson::Object();

		if (BLUETOOTH_ERROR_NONE == error)
		{
			responseObj.put("adapterAddress", adapterAddress);
			responseObj.put("returnValue", true);
			mAdvertising = false;

			notifySubscribersAdvertisingChanged(adapterAddress);
		}
		else
		{
			responseObj.put("adapterAddress", adapterAddress);
			appendErrorResponse(responseObj, error);
		}

		LSUtils::postToClient(requestMessage, responseObj);
		LSMessageUnref(requestMessage);
	};

	mDefaultAdapter->stopAdvertising(leStopAdvCallback);
	return true;
}

bool BluetoothManagerService::getAdvStatus(LSMessage &message)
{
	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema =  STRICT_SCHEMA(PROPS_2(PROP(adapterAddress, string),PROP(subscribe,boolean)));

	if(!LSUtils::parsePayload(request.getPayload(),requestObj,schema,&parseError))
	{
		if(parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);

		else
			LSUtils::respondWithError(request,BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress = mAddress;

	if(requestObj.hasKey("adapterAddress"))
		adapterAddress = requestObj["adapterAddress"].asString();

	pbnjson::JValue responseObj = pbnjson::Object();

	if (request.isSubscription())
	{
		mGetAdvStatusSubscriptions.subscribe(request);
		responseObj.put("subscribed", true);
	}

	responseObj.put("adapterAddress",adapterAddress);
	responseObj.put("advertising", mAdvertising);
	responseObj.put("returnValue", true);

	LSUtils::postToClient(request, responseObj);

	return true;
}

bool BluetoothManagerService::startScan(LSMessage &message)
{
	LS::Message request(&message);
	pbnjson::JValue requestObj;
	int parseError = 0;
	int32_t leScanId = -1;
	bool subscribed = false;

	if (!mDefaultAdapter)
	{
		LSUtils::respondWithError(request, BT_ERR_ADAPTER_NOT_AVAILABLE);
		return true;
	}

	const std::string schema =  STRICT_SCHEMA(PROPS_7(PROP(address, string), PROP(name, string),
													PROP(subscribe, boolean), PROP(adapterAddress, string),
													OBJECT(serviceUuid, OBJSCHEMA_2(PROP(uuid, string), PROP(mask, string))),
													OBJECT(serviceData, OBJSCHEMA_3(PROP(uuid, string), ARRAY(data, integer), ARRAY(mask, integer))),
													OBJECT(manufacturerData, OBJSCHEMA_3(PROP(id, integer), ARRAY(data, integer), ARRAY(mask, integer)))) REQUIRED_1(subscribe));

	if(!LSUtils::parsePayload(request.getPayload(),requestObj,schema,&parseError))
	{
		if(parseError != JSON_PARSE_SCHEMA_ERROR)
			LSUtils::respondWithError(request, BT_ERR_BAD_JSON);
		else if (!request.isSubscription())
			LSUtils::respondWithError(request, BT_ERR_MTHD_NOT_SUBSCRIBED);
		else
			LSUtils::respondWithError(request,BT_ERR_SCHEMA_VALIDATION_FAIL);

		return true;
	}

	std::string adapterAddress;
	if (!isRequestedAdapterAvailable(request, requestObj, adapterAddress))
		return true;

	BluetoothLeDiscoveryFilter leFilter;
	BluetoothLeServiceUuid serviceUuid;
	BluetoothLeServiceData serviceData;
	BluetoothManufacturerData manufacturerData;

	if (requestObj.hasKey("address"))
	{
		std::string address = requestObj["address"].asString();
		leFilter.setAddress(address);
	}

	if (requestObj.hasKey("name"))
	{
		std::string name = requestObj["name"].asString();
		leFilter.setName(name);
	}

	if (requestObj.hasKey("serviceUuid"))
	{
		pbnjson::JValue serviceUuidObj = requestObj["serviceUuid"];

		if (serviceUuidObj.hasKey("uuid"))
		{
			std::string uuid = serviceUuidObj["uuid"].asString();
			serviceUuid.setUuid(uuid);
		}

		if (serviceUuidObj.hasKey("mask"))
		{
			std::string mask = serviceUuidObj["mask"].asString();
			serviceUuid.setMask(mask);
		}

		leFilter.setServiceUuid(serviceUuid);
	}

	if (requestObj.hasKey("serviceData"))
	{
		pbnjson::JValue serviceDataObj = requestObj["serviceData"];

		if (serviceDataObj.hasKey("uuid"))
		{
			std::string uuid = serviceDataObj["uuid"].asString();
			serviceData.setUuid(uuid);
		}

		if (serviceDataObj.hasKey("data"))
		{
			BluetoothLowEnergyData data;
			auto dataObjArray = serviceDataObj["data"];
			for (int n = 0; n < dataObjArray.arraySize(); n++)
			{
				pbnjson::JValue element = dataObjArray[n];
				data.push_back((uint8_t)element.asNumber<int32_t>());
			}

			serviceData.setData(data);
		}

		if (serviceDataObj.hasKey("mask"))
		{
			BluetoothLowEnergyMask mask;
			auto maskObjArray = serviceDataObj["mask"];
			for (int n = 0; n < maskObjArray.arraySize(); n++)
			{
				pbnjson::JValue element = maskObjArray[n];
				mask.push_back((uint8_t)element.asNumber<int32_t>());
			}

			serviceData.setMask(mask);
		}

		leFilter.setServiceData(serviceData);

	}

	if (requestObj.hasKey("manufacturerData"))
	{
		pbnjson::JValue manufacturerDataObj = requestObj["manufacturerData"];

		if (manufacturerDataObj.hasKey("id"))
		{
			int32_t id = manufacturerDataObj["id"].asNumber<int32_t>();
			manufacturerData.setId(id);
		}

		if (manufacturerDataObj.hasKey("data"))
		{
			BluetoothLowEnergyData data;
			auto dataObjArray = manufacturerDataObj["data"];
			for (int n = 0; n < dataObjArray.arraySize(); n++)
			{
				pbnjson::JValue element = dataObjArray[n];
				data.push_back((uint8_t)element.asNumber<int32_t>());
			}

			manufacturerData.setData(data);
		}

		if (manufacturerDataObj.hasKey("mask"))
		{
			BluetoothLowEnergyMask mask;
			auto maskObjArray = manufacturerDataObj["mask"];
			for (int n = 0; n < maskObjArray.arraySize(); n++)
			{
				pbnjson::JValue element = maskObjArray[n];
				mask.push_back((uint8_t)element.asNumber<int32_t>());
			}

			manufacturerData.setMask(mask);
		}

		leFilter.setManufacturerData(manufacturerData);
	}

	if (request.isSubscription())
	{
		leScanId = mDefaultAdapter->addLeDiscoveryFilter(leFilter);
		if (leScanId < 0)
		{
			LSUtils::respondWithError(request, BT_ERR_START_DISC_FAIL);
			return true;
		}

		LSUtils::ClientWatch *watch = new LSUtils::ClientWatch(get(), &message,
		                    std::bind(&BluetoothManagerService::notifyStartScanListenerDropped, this, leScanId));

		mStartScanWatches.insert(std::pair<uint32_t, LSUtils::ClientWatch*>(leScanId, watch));
		subscribed = true;
	}

	pbnjson::JValue responseObj = pbnjson::Object();

	BluetoothError error = BLUETOOTH_ERROR_NONE;

	if (mStartScanWatches.size() == 1)
		error = mDefaultAdapter->startLeDiscovery();

	if (error != BLUETOOTH_ERROR_NONE)
	{
		LSUtils::respondWithError(request, BT_ERR_START_DISC_FAIL);
		return true;
	}

	responseObj.put("returnValue", true);
	responseObj.put("subscribed", subscribed);
	responseObj.put("adapterAddress", adapterAddress);

	LSUtils::postToClient(request, responseObj);

	if (leScanId > 0)
		mDefaultAdapter->matchLeDiscoveryFilterDevices(leFilter, leScanId);

	return true;
}

void BluetoothManagerService::leConnectionRequest(const std::string &address, bool state)
{
	for (auto profile : mProfiles)
	{
		if (profile->getName() == "GATT")
		{
			auto gattProfile = dynamic_cast<BluetoothGattProfileService *>(profile);
			if (gattProfile)
				gattProfile->incomingLeConnectionRequest(address, state);
		}
	}
}

// vim: noai:ts=4:sw=4:ss=4:expandtab
