// Copyright (c) 2015-2018 LG Electronics, Inc.
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


#include "channelmanager.h"
#include "bluetoothsppprofileservice.h"
#include "ls2utils.h"
#include "logging.h"
#include "clientwatch.h"

#define BLUETOOTH_PROFILE_SPP_MAX_CHANNEL_ID 999

typedef struct {
	BluetoothSppChannelId channelId;
	guint eventSourceId;
	ChannelManager *manager;
	std::string adapterAddress;
} DataReceivedInfo;

typedef struct {
	guint eventSourceId;
	ChannelManager *manager;
	void *readDataInfo;
} TimeoutInfo;

ChannelManager::ChannelManager() :
        mNextChannelId(1)
{

}

ChannelManager::~ChannelManager()
{
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		while (!channelInfo->receiveQueue.empty())
		{
			QueueData *queue = channelInfo->receiveQueue.front();
			if (queue)
				delete queue;

			channelInfo->receiveQueue.pop();
		}
		delete channelInfo;
	}
	mChannelInfo.clear();

	for (auto itMap = mReadDataSubscriptions.begin(); itMap != mReadDataSubscriptions.end(); itMap++)
	{
		ReadDataInfo *dataInfo = *itMap;
		if (NULL == dataInfo)
			continue;

		delete dataInfo;
	}
	mReadDataSubscriptions.clear();

	for (auto itMap = mCreateChannelSubscriptons.begin(); itMap != mCreateChannelSubscriptons.end(); itMap++)
	{
		CreateChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		delete channelInfo;
	}
	mCreateChannelSubscriptons.clear();
	mConnectingChannels.clear();


}

void ChannelManager::notifyReceivedData(const std::string &adapterAddress, const BluetoothSppChannelId channelId)
{
	ChannelInfo *channelInfo = getChannelInfo(channelId);
	if (NULL == channelInfo)
		return;

	std::lock_guard<std::mutex> guard(cmMutex);
	bool found = false;
	for (auto itMap = mReadDataSubscriptions.begin(); itMap != mReadDataSubscriptions.end(); itMap++)
	{
		ReadDataInfo *dataInfo = *itMap;
		if (NULL == dataInfo)
			continue;

		if ((dataInfo->stackChannelId == channelId) || ((dataInfo->userChannelId == EMPTY_STRING) &&
		        (dataInfo->appName != EMPTY_STRING) && (dataInfo->appName == channelInfo->appName)))
		{
			found = true;
			makeDataBuffer(channelInfo);
			postToReadDataSubscriber(channelInfo->dataBuffer.buffer, channelInfo->dataBuffer.size, dataInfo->watch, adapterAddress,
			        channelInfo->userChannelId);
		}
	}

	if (found)
		channelInfo->dataBuffer.size = 0;
}

void ChannelManager::makeDataBuffer(ChannelInfo *channelInfo)
{
	if (NULL == channelInfo)
		return;

	if (channelInfo->dataBuffer.size > 0)
		return;

	while (!channelInfo->receiveQueue.empty())
	{
		QueueData *queue = channelInfo->receiveQueue.front();
		if (queue)
		{
			if (MAX_BUFFER_SIZE - channelInfo->dataBuffer.size < queue->size)
				return;

			if (queue->data)
			{
				memcpy(channelInfo->dataBuffer.buffer + channelInfo->dataBuffer.size, queue->data, queue->size);
				channelInfo->dataBuffer.size += queue->size;

				delete queue->data;
			}
			delete queue;
		}
		channelInfo->receiveQueue.pop();
	}
}

void ChannelManager::postToReadDataSubscriber(const uint8_t *data, const uint32_t size, const LSUtils::ClientWatch *watch,
        const std::string &adapterAddress, const std::string &channelId)
{
	if (NULL == data || 0 == size)
		return;

	gchar *gdata = g_base64_encode(data, size);
	if (NULL == gdata)
		return;

	pbnjson::JValue responseObj = pbnjson::Object();
	responseObj.put("returnValue", true);
	responseObj.put("adapterAddress", adapterAddress);
	responseObj.put("subscribed", true);
	responseObj.put("channelId", channelId);
	responseObj.put("data", gdata);
	LSUtils::postToClient(watch->getMessage(), responseObj);
	g_free(gdata);
}

void ChannelManager::deleteReadDataSubscription(const void *readData)
{
	for (auto itMap = mReadDataSubscriptions.begin(); itMap != mReadDataSubscriptions.end(); itMap++)
	{
		ReadDataInfo *dataInfo = *itMap;
		if ((dataInfo != NULL) && (readData == dataInfo))
		{
			BT_DEBUG("[deleteReadDataSubscription] channelId:%s", dataInfo->userChannelId.c_str());

			delete dataInfo;
			mReadDataSubscriptions.erase(itMap);
			break;
		}
	}
}

ChannelManager::ChannelInfo *ChannelManager::getChannelInfo(const std::string &uuid)
{
	auto findIter = mChannelInfo.find(uuid);
	if (findIter == mChannelInfo.end())
		return NULL;

	ChannelInfo *channelInfo = findIter->second;
	return channelInfo;
}

ChannelManager::ChannelInfo *ChannelManager::getChannelInfo(const BluetoothSppChannelId channelId)
{
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (channelInfo->stackChannelId == channelId)
			return channelInfo;
	}

	return NULL;
}

std::string ChannelManager::getUserChannelId(const BluetoothSppChannelId channelId)
{
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (channelInfo->stackChannelId == channelId)
			return channelInfo->userChannelId;
	}

	return EMPTY_STRING;
}

std::string ChannelManager::getUserChannelId(const std::string &uuid)
{
	auto findIter = mChannelInfo.find(uuid);
	if (findIter == mChannelInfo.end())
		return EMPTY_STRING;

	ChannelInfo *channelInfo = findIter->second;
	if (channelInfo)
		return channelInfo->userChannelId;

	return EMPTY_STRING;
}

BluetoothSppChannelId ChannelManager::getStackChannelId(const std::string &channelId)
{
	if (EMPTY_STRING == channelId)
		return BLUETOOTH_SPP_CHANNEL_ID_INVALID;

	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (channelInfo->userChannelId == channelId)
			return channelInfo->stackChannelId;
	}

	return BLUETOOTH_SPP_CHANNEL_ID_INVALID;
}

std::string ChannelManager::getUuid(const BluetoothSppChannelId channelId)
{
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (channelInfo->stackChannelId == channelId)
			return itMap->first;
	}

	return EMPTY_STRING;
}

bool ChannelManager::isChannelConnecting(const std::string &uuid)
{
	return (std::find(mConnectingChannels.begin(), mConnectingChannels.end(), uuid) != mConnectingChannels.end());
}

void ChannelManager::markChannelAsConnecting(const std::string &uuid)
{
	if (isChannelConnecting(uuid))
		return;

	mConnectingChannels.push_back(uuid);
}

void ChannelManager::markChannelAsNotConnecting(const std::string &uuid)
{
	auto findIter = std::find(mConnectingChannels.begin(), mConnectingChannels.end(), uuid);
	if (findIter == mConnectingChannels.end())
		return;

	mConnectingChannels.erase(findIter);
}

bool ChannelManager::isChannelConnected(const BluetoothSppChannelId channelId)
{
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (channelInfo->stackChannelId == channelId)
			return true;
	}

	return false;
}

bool ChannelManager::isChannelConnected(const std::string &address)
{
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (channelInfo->address == address)
			return true;
	}

	return false;
}

std::string ChannelManager::markChannelAsConnected(const BluetoothSppChannelId channelId,
        const std::string &address, const std::string &uuid, LSMessage *message)
{
	if (isChannelConnected(channelId))
		return EMPTY_STRING;

	if (mNextChannelId > BLUETOOTH_PROFILE_SPP_MAX_CHANNEL_ID)
		mNextChannelId = 1;

	// Make a new channelId convert to string
	std::string userChannelIdStr = std::to_string(mNextChannelId);
	auto padStr = [](std::string &str, const size_t num, const char paddingChar)
	{
		if (num > str.size())
			str.insert(0, num - str.size(), paddingChar);
	};
	padStr(userChannelIdStr, 3, '0');
	mNextChannelId++;

	std::string appName = getMessageOwner(message);
	ChannelInfo *channelInfo = new ChannelInfo();
	channelInfo->stackChannelId = channelId;
	channelInfo->userChannelId = userChannelIdStr;
	channelInfo->address = address;
	channelInfo->appName = (EMPTY_STRING == appName) ? getCreateChannelAppName(uuid) : appName;

	BT_DEBUG("[markChannelAsConnected] create channel(channelId:%s, appName:%s, address:%s)",
	        userChannelIdStr.c_str(), channelInfo->appName.c_str(), address.c_str());

	mChannelInfo.insert(std::pair<std::string, ChannelInfo *>(uuid, channelInfo));
	markChannelAsNotConnecting(uuid);

	return userChannelIdStr;
}

std::string ChannelManager::markChannelAsNotConnected(const BluetoothSppChannelId channelId, const std::string &adapterAddress)
{
	std::string appName = EMPTY_STRING;
	std::string address = EMPTY_STRING;
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (channelInfo->stackChannelId == channelId)
		{
			address = channelInfo->address;
			appName = channelInfo->appName;

			BT_DEBUG("[markChannelAsNotConnected] delete channel(channelId:%s, appName:%s, address:%s)",
			        channelInfo->userChannelId.c_str(), appName.c_str(), address.c_str());

			delete channelInfo;
			mChannelInfo.erase(itMap);
			break;
		}
	}

	for (auto itMap = mReadDataSubscriptions.begin(); itMap != mReadDataSubscriptions.end();)
	{
		ReadDataInfo *dataInfo = *itMap;
		if (NULL == dataInfo)
		{
			itMap++;
			continue;
		}

		if (dataInfo->stackChannelId == channelId)
		{
			pbnjson::JValue responseObj = pbnjson::Object();
			responseObj.put("returnValue", false);
			responseObj.put("disconnectByRemote", true);
			responseObj.put("subscribed", false);
			responseObj.put("adapterAddress", adapterAddress);
			LSUtils::postToClient(dataInfo->watch->getMessage(), responseObj);

			appName = dataInfo->appName;
			mReadDataSubscriptions.erase(itMap);

			BT_DEBUG("[markChannelAsNotConnected] delete readsubscription(appName:%s, channelId:%s)",
			        appName.c_str(), dataInfo->userChannelId.c_str());

			delete dataInfo;
			continue;
		}

		itMap++;
	}

	if (EMPTY_STRING == appName)
		return address;

	int otherChannelOfApp = 0;
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if ((channelInfo->stackChannelId != BLUETOOTH_SPP_CHANNEL_ID_INVALID) &&
		        (appName == channelInfo->appName))
			otherChannelOfApp++;
	}

	if (otherChannelOfApp != 0)
		return address;

	// delete app read subcription(channelId is "")
	for (auto itMap = mReadDataSubscriptions.begin(); itMap != mReadDataSubscriptions.end();)
	{
		ReadDataInfo *dataInfo = *itMap;
		if (NULL == dataInfo)
		{
			itMap++;
			continue;
		}

		if ((EMPTY_STRING == dataInfo->userChannelId) && (appName == dataInfo->appName))
		{
			BT_DEBUG("[markChannelAsNotConnected] delete readsubscription(appName:%s)", appName.c_str());

			pbnjson::JValue responseObj = pbnjson::Object();
			responseObj.put("returnValue", true);
			responseObj.put("disconnectByRemote", true);
			responseObj.put("subscribed", false);
			LSUtils::postToClient(dataInfo->watch->getMessage(), responseObj);

			mReadDataSubscriptions.erase(itMap);
			delete dataInfo;
			continue;
		}

		itMap++;
	}

	return address;
}

pbnjson::JValue ChannelManager::getConnectedChannels(const std::string &address)
{
	pbnjson::JValue connectedChannels = pbnjson::Array();
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (channelInfo->address == address)
			connectedChannels.append(channelInfo->userChannelId);
	}

	return connectedChannels;
}

LSUtils::ClientWatch *ChannelManager::getCreateChannelSubscription(const std::string &uuid)
{
	auto findIter = mCreateChannelSubscriptons.find(uuid);
	if (findIter == mCreateChannelSubscriptons.end())
		return NULL;

	CreateChannelInfo *createChannelInfo = findIter->second;
	if (NULL == createChannelInfo)
		return NULL;

	return createChannelInfo->watch;
}

std::string ChannelManager::getCreateChannelAppName(const std::string &uuid)
{
	auto findIter = mCreateChannelSubscriptons.find(uuid);
	if (findIter == mCreateChannelSubscriptons.end())
		return EMPTY_STRING;

	CreateChannelInfo *createChannelInfo = findIter->second;
	if (NULL == createChannelInfo)
		return EMPTY_STRING;

	return createChannelInfo->appName;
}

void ChannelManager::addCreateChannelSubscripton(const std::string &uuid, LSUtils::ClientWatch *watch,
        LSMessage *message)
{
	CreateChannelInfo *createChannelInfo = new CreateChannelInfo();
	createChannelInfo->appName = getMessageOwner(message);
	createChannelInfo->watch = watch;

	mCreateChannelSubscriptons.insert(std::pair<std::string, CreateChannelInfo *>(uuid, createChannelInfo));
}

void ChannelManager::deleteCreateChannelSubscription(const std::string &uuid)
{
	auto findIter = mCreateChannelSubscriptons.find(uuid);
	if (findIter == mCreateChannelSubscriptons.end())
		return;

	mCreateChannelSubscriptons.erase(findIter);
	if (findIter->second)
		delete findIter->second;
}

const ChannelManager::DataBuffer *ChannelManager::getChannelBufferData(std::string &channelId, const std::string &appName)
{
	DataBuffer *dataBuffer = NULL;
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (EMPTY_STRING == channelId)
		{
			if ((appName != EMPTY_STRING) && (channelInfo->appName != EMPTY_STRING) && (channelInfo->appName == appName))
			{
				channelId = channelInfo->userChannelId;

				dataBuffer = new DataBuffer();
				std::lock_guard<std::mutex> guard(cmMutex);
				if (channelInfo->dataBuffer.size == 0)
					makeDataBuffer(channelInfo);
				dataBuffer->size = channelInfo->dataBuffer.size;
				memcpy(dataBuffer->buffer, channelInfo->dataBuffer.buffer, channelInfo->dataBuffer.size);
				channelInfo->dataBuffer.size = 0;
				break;
			}
		}
		else
		{
			if (channelInfo->userChannelId == channelId)
			{
				dataBuffer = new DataBuffer();
				std::lock_guard<std::mutex> guard(cmMutex);
				if (channelInfo->dataBuffer.size == 0)
					makeDataBuffer(channelInfo);
				dataBuffer->size = channelInfo->dataBuffer.size;
				memcpy(dataBuffer->buffer, channelInfo->dataBuffer.buffer, channelInfo->dataBuffer.size);
				channelInfo->dataBuffer.size = 0;
				break;
			}
		}
	}

	return dataBuffer;
}

void ChannelManager::addReceiveQueue(const std::string &adapterAddress, const BluetoothSppChannelId channelId,
        const uint8_t *data, const uint32_t size)
{
	if (0 == size)
		return;

	std::string userChannelId = EMPTY_STRING;
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (channelInfo->stackChannelId == channelId)
		{
			userChannelId = channelInfo->userChannelId;
			QueueData *queue = new QueueData();
			queue->data = new uint8_t[size];
			queue->size = size;
			memcpy(queue->data, data, size);
			std::lock_guard<std::mutex> guard(cmMutex);
			channelInfo->receiveQueue.push(queue);
			break;
		}
	}

	auto dataReceivedCallback = [] (gpointer user_data) -> gboolean {
		DataReceivedInfo *userData = (DataReceivedInfo *)user_data;
		if (NULL == userData)
			return FALSE;

		ChannelManager *manager = userData->manager;
		BluetoothSppChannelId channelId = userData->channelId;
		std::string adapterAddress = userData->adapterAddress;
		g_source_remove(userData->eventSourceId);
		delete userData;

		manager->notifyReceivedData(adapterAddress, channelId);

		return FALSE;
	};

	DataReceivedInfo *userData = new DataReceivedInfo();
	userData->channelId = channelId;
	userData->manager = this;
	userData->adapterAddress = adapterAddress;
	guint eventSourceId = g_idle_add(dataReceivedCallback, (gpointer)userData);
	userData->eventSourceId = eventSourceId;
}

void *ChannelManager::addReadDataSubscription(const std::string &channelId, const int timeout, LSUtils::ClientWatch *watch,
        const std::string &appName)
{
	ReadDataInfo *readDataInfo = new ReadDataInfo();
	readDataInfo->timeout = timeout;
	readDataInfo->watch = watch;
	readDataInfo->userChannelId = channelId;
	readDataInfo->stackChannelId = getStackChannelId(channelId);
	readDataInfo->appName = appName;

	BT_DEBUG("[addReadDataSubscription] channelId:%s, appName:%s, timeout:%d", channelId.c_str(), appName.c_str(), timeout);
	mReadDataSubscriptions.push_back(readDataInfo);

	auto timeoutExpiredCallback = [] (gpointer user_data) -> gboolean {
		TimeoutInfo *userData = (TimeoutInfo *)user_data;
		if (NULL == userData)
			return FALSE;

		ChannelManager *manager = userData->manager;
		g_source_remove(userData->eventSourceId);
		void *readDataInfo = userData->readDataInfo;
		delete userData;

		BT_DEBUG("[timeoutExpired] readDataInfo:%p", readDataInfo);

		manager->deleteReadDataSubscription(readDataInfo);

		return FALSE;
	};

	if (timeout > 0)
	{
		TimeoutInfo *userData = new TimeoutInfo();
		userData->manager = this;
		userData->readDataInfo = (void *)readDataInfo;
		guint eventSourceId = g_timeout_add_seconds(timeout, timeoutExpiredCallback, (gpointer)userData);
		userData->eventSourceId = eventSourceId;
	}

	return readDataInfo;
}

std::string ChannelManager::getMessageOwner(LSMessage *message)
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

std::string ChannelManager::getChannelAppName(const std::string &channelId)
{
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			continue;

		if (channelInfo->userChannelId == channelId)
			return channelInfo->appName;
	}

	return EMPTY_STRING;
}

void ChannelManager::setChannelAppName(const std::string &channelId, std::string appName)
{
	for (auto itMap = mChannelInfo.begin(); itMap != mChannelInfo.end(); itMap++)
	{
		ChannelInfo *channelInfo = itMap->second;
		if (NULL == channelInfo)
			return;

		if (channelInfo->userChannelId == channelId)
			channelInfo->appName = appName;
	}
}
