// Copyright (c) 2014-2019 LG Electronics, Inc.
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


#include <dlfcn.h>
#include <string>
#include <glib.h>
#include <bluetooth-sil-api.h>

#include "config.h"
#include "logging.h"
#include "bluetoothsilfactory.h"
#include "utils.h"

typedef BluetoothSIL *(*CreateSILFunc)(unsigned int version, BluetoothPairingIOCapability capability);

void *BluetoothSILFactory::SILHandle = nullptr;

BluetoothSIL *BluetoothSILFactory::create(unsigned int version, BluetoothPairingIOCapability capability)
{
	std::vector<std::string> names = split(WEBOS_BLUETOOTH_SIL, ' ');
	std::string name = names[0];

	const char *overrideSIL = getenv("WEBOS_BLUETOOTH_SIL");
	if (overrideSIL)
	{
		name = overrideSIL;
	}

	BT_DEBUG("name = %s,  overrideSIL =%s  \n", name.c_str(), overrideSIL);

	std::string basePath = WEBOS_BLUETOOTH_SIL_BASE_PATH;

	const char *overrideSILBasePath = getenv("WEBOS_BLUETOOTH_SIL_BASE_PATH");
	if (overrideSILBasePath)
	{
		basePath = overrideSILBasePath;
	}

	name += ".so";

	BT_INFO("SILFACTORY", 0, "Trying to use SIL file name as %s\n", name.c_str());

	char* path = g_build_path("/", basePath.c_str(), name.c_str(), NULL);

	BT_INFO("SILFACTORY", 0, "Trying to load SIL from path %s\n", path);

	SILHandle = dlopen(path, RTLD_NOW);

	if (!SILHandle)
	{
		BT_CRITICAL(MSGID_SIL_DOESNT_EXIST, 0, "Failed to load SIL from path %s, err = %s", path, dlerror());
		return 0;
	}

	CreateSILFunc createSIL = reinterpret_cast<CreateSILFunc>(dlsym(SILHandle,
	                          "createBluetoothSIL"));

	if (!createSIL)
	{
		BT_CRITICAL(MSGID_SIL_WRONG_API, 0,
		            "SIL module doesn't expose the required API");
		dlclose(SILHandle);
		return 0;
	}

	BluetoothSIL *sil = createSIL(version, capability);

	if (!sil)
	{
		BT_DEBUG("Failed to create SIL for API version %d", version);
		dlclose(SILHandle);
		return 0;
	}

	BT_DEBUG("Successfully created SIL from %s", path);

	g_free(path);
	return sil;
}

void BluetoothSILFactory::freeSILHandle()
{
	BT_INFO("SILFACTORY", 0, "Free SIL handle\n");

	if (SILHandle)
		dlclose(SILHandle);
}
