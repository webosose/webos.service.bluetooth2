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


#ifndef CHANNELMANAGER_H
#define CHANNELMANAGER_H

#include <string>
#include <unordered_map>
#include <map>
#include <mutex>

#include <pbnjson.hpp>
#include <bluetooth-sil-api.h>
#include <luna-service2/lunaservice.hpp>

#define MAX_BUFFER_SIZE (1024*5)
#define EMPTY_STRING ""

namespace pbnjson
{
	class JValue;
}

namespace LS
{
	class SubscriptionPoint;
}

namespace LSUtils
{
	class ClientWatch;
}

class ChannelManager
{
public:
	ChannelManager();
	~ChannelManager();

	typedef struct {
		uint32_t size;
		uint8_t buffer[MAX_BUFFER_SIZE];
	} DataBuffer;

	std::string getUserChannelId(const BluetoothSppChannelId channelId);
	std::string getUserChannelId(const std::string &uuid);
	BluetoothSppChannelId getStackChannelId(const std::string &channelId);
	std::string getUuid(const BluetoothSppChannelId channelId);
	bool isChannelConnecting(const std::string &uuid);
	void markChannelAsConnecting(const std::string &uuid);
	void markChannelAsNotConnecting(const std::string &uuid);
	bool isChannelConnected(const BluetoothSppChannelId channelId);
	bool isChannelConnected(const std::string &address);
	std::string markChannelAsConnected(const BluetoothSppChannelId channelId, const std::string &address, const std::string &uuid,
	        LSMessage *message = NULL);
	std::string markChannelAsNotConnected(const BluetoothSppChannelId channelId, const std::string &adapterAddress);
	pbnjson::JValue getConnectedChannels(const std::string &address);
	void addReceiveQueue(const std::string &adapterAddress, const BluetoothSppChannelId channelId, const uint8_t *data,
	        const uint32_t size);
	const DataBuffer *getChannelBufferData(std::string &channelId, const std::string &appName);
	void notifyReceivedData(const std::string &adapterAddress, const BluetoothSppChannelId channelId);
	std::string getMessageOwner(LSMessage *message);
	std::string getChannelAppName(const std::string &channelId);
	void setChannelAppName(const std::string &channelId, std::string appName);
	void *addReadDataSubscription(const std::string &channelId, const int timeout, LSUtils::ClientWatch *watch, const std::string &appName);
	void deleteReadDataSubscription(const void *readData);
	LSUtils::ClientWatch *getCreateChannelSubscription(const std::string &uuid);
	void addCreateChannelSubscripton(const std::string &uuid, LSUtils::ClientWatch *watch, LSMessage *message);
	std::string getCreateChannelAppName(const std::string &uuid);
	void deleteCreateChannelSubscription(const std::string &uuid);

private:
	typedef struct {
		uint32_t size;
		uint8_t *data;
	} QueueData;

	typedef struct {
		BluetoothSppChannelId stackChannelId;
		std::string userChannelId;
		std::string address;
		std::string appName;
		DataBuffer dataBuffer;
		std::queue<QueueData *> receiveQueue;
	} ChannelInfo;

	typedef struct {
		int32_t timeout;
		LSUtils::ClientWatch *watch;
		BluetoothSppChannelId stackChannelId;
		std::string userChannelId;
		std::string appName;
	} ReadDataInfo;

	typedef struct {
		std::string appName;
		LSUtils::ClientWatch *watch;
	} CreateChannelInfo;

	uint32_t mNextChannelId;
	std::map<std::string, ChannelInfo *> mChannelInfo;
	std::unordered_map<std::string, CreateChannelInfo *> mCreateChannelSubscriptons;
	std::vector<ReadDataInfo *> mReadDataSubscriptions;
	std::vector<std::string> mConnectingChannels;
	std::mutex cmMutex;

	void postToReadDataSubscriber(const uint8_t *data, const uint32_t size, const LSUtils::ClientWatch *watch,
	        const std::string &adapterAddress, const std::string &channelId);
	ChannelInfo *getChannelInfo(const std::string &uuid);
	ChannelInfo *getChannelInfo(const BluetoothSppChannelId channelId);
	void makeDataBuffer(ChannelInfo *channelInfo);
};

#endif // CHANNELMANAGER_H
