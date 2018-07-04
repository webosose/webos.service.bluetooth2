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



#ifndef LOGGING_H
#define LOGGING_H

#include <PmLogLib.h>

extern PmLogContext logContext;

#define BT_CRITICAL(msgid, kvcount, ...) \
	PmLogCritical(logContext, msgid, kvcount, ##__VA_ARGS__)

#define BT_ERROR(msgid, kvcount, ...) \
	PmLogError(logContext, msgid, kvcount,##__VA_ARGS__)

#define BT_WARNING(msgid, kvcount, ...) \
	PmLogWarning(logContext, msgid, kvcount, ##__VA_ARGS__)

#define BT_INFO(msgid, kvcount, ...) \
	PmLogInfo(logContext, msgid, kvcount, ##__VA_ARGS__)

#define BT_DEBUG(fmt, ...) \
	PmLogDebug(logContext, "%s:%s() " fmt, __FILE__, __FUNCTION__, ##__VA_ARGS__)

#define MSGID_LS2_FAILED_TO_SEND                    "LS2_FAILED_TO_SEND"
#define MSGID_SIL_WRONG_API                         "SIL_WRONG_API"
#define MSGID_SIL_DOESNT_EXIST                      "SIL_DOESNT_EXIST"
#define MSGID_ENABLED_PROFILE_NOT_SUPPORTED_BY_SIL  "ENABLED_PROFILE_NOT_SUPP_BY_SIL"
#define MSGID_NO_PAIRING_SUBSCRIBER                 "NO_PAIRING_SUBSCRIBER"
#define MSGID_LS2_FAIL_REGISTER_CANCEL_NOTI         "LS2_FAIL_REGISTER_CANCEL_NOTI"
#define MSGID_INVALID_PAIRING_CAPABILITY            "INVALID_PAIRING_CAPABILITY"
#define MSGID_SUBSCRIPTION_CLIENT_DROPPED           "SUBSCRIPTION_CLIENT_DROPPED"
#define MSGID_INCOMING_PAIR_REQ_FAIL                "INCOMING_PAIR_REQ_FAIL"
#define MSGID_UNPAIR_FROM_ANCS_FAILED               "OUTGOING_UNPAIR_FROM_ANCS_FAIL"

#endif // LOGGING_H
