#include <pbnjson.hpp>
#include <luna-service2/lunaservice.hpp>
#include "logging.h"
#include "ls2utils.h"

void LSUtils::postToClient(LS::Message &message, pbnjson::JValue &object)
{
	std::string payload;
	LSUtils::generatePayload(object, payload);

	try
	{
		message.respond(payload.c_str());
	}
	catch (LS::Error &error)
	{
		BT_ERROR(MSGID_LS2_FAILED_TO_SEND, 0, "Failed to submit response: %s", error.what());
	}
}

