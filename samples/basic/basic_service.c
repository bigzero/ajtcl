/**
 * @file
 */
/******************************************************************************
 * Copyright 2012-2013, Qualcomm Innovation Center, Inc.
 *
 *    All rights reserved.
 *    This file is licensed under the 3-clause BSD license in the NOTICE.txt
 *    file for this project. A copy of the 3-clause BSD license is found at:
 *
 *        http://opensource.org/licenses/BSD-3-Clause.
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the license is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the license for the specific language governing permissions and
 *    limitations under the license.
 ******************************************************************************/

#include <stdio.h>

#include "alljoyn.h"

#define CONNECT_ATTEMPTS   10
static const char ServiceName[] = "org.alljoyn.Bus.sample";
static const char ServicePath[] = "/sample";
static const uint16_t ServicePort = 25;

/**
 * The interface name followed by the method signatures.
 *
 * See also .\inc\aj_introspect.h
 */
static const char* const sampleInterface[] = {
    "org.alljoyn.Bus.sample",   /* The first entry is the interface name. */
    "?Dummy foo<i",             /* This is just a dummy entry at index 0 for illustration purposes. */
    "?cat inStr1<s inStr2<s outStr>s", /* Method at index 1. */
    NULL
};

/**
 * A NULL terminated collection of all interfaces.
 */
static const AJ_InterfaceDescription sampleInterfaces[] = {
    sampleInterface,
    NULL
};

/**
 * Objects implemented by the application. The first member in the AJ_Object structure is the path.
 * The second is the collection of all interfaces at that path.
 */
static const AJ_Object AppObjects[] = {
    { ServicePath, sampleInterfaces },
    { NULL }
};

/*
 * The value of the arguments are the indices of the
 * object path in AppObjects (above), interface in sampleInterfaces (above), and
 * member indices in the interface.
 * The 'cat' index is 1 because the first entry in sampleInterface is the interface name.
 * This makes the first index (index 0 of the methods) the second string in
 * sampleInterface[] which, for illustration purposes is a dummy entry.
 * The index of the method we implement for basic_service, 'cat', is 1 which is the third string
 * in the array of strings sampleInterface[].
 *
 * See also .\inc\aj_introspect.h
 */
#define BASIC_SERVICE_CAT AJ_APP_MESSAGE_ID(0, 0, 1)

static AJ_Status AppHandleCat(AJ_Message* msg)
{
#define BUFFER_SIZE 256
    const char* string0;
    const char* string1;
    char buffer[BUFFER_SIZE];
    AJ_Message reply;
    AJ_Arg replyArg;

    AJ_UnmarshalArgs(msg, "ss", &string0, &string1);
    AJ_MarshalReplyMsg(msg, &reply);

    /* We have the arguments. Now do the concatenation. */
    strncpy(buffer, string0, BUFFER_SIZE);
    buffer[BUFFER_SIZE - 1] = '\0';
    strncat(buffer, string1, BUFFER_SIZE - strlen(buffer));
    buffer[BUFFER_SIZE - 1] = '\0';

    AJ_InitArg(&replyArg, AJ_ARG_STRING, 0, buffer, 0);
    AJ_MarshalArg(&reply, &replyArg);

    return AJ_DeliverMsg(&reply);

#undef BUFFER_SIZE
}

/* All times are expressed in milliseconds. */
#define CONNECT_TIMEOUT     (1000 * 60)
#define UNMARSHAL_TIMEOUT   (1000 * 5)
#define SLEEP_TIME          (1000 * 2)

int AJ_Main(void)
{
    AJ_Status status = AJ_OK;
    AJ_BusAttachment bus;
    uint8_t connected = FALSE;
    uint32_t sessionId = 0;

    /* One time initialization before calling any other AllJoyn APIs. */
    AJ_Initialize();

    /* This is for debug purposes and is optional. */
    AJ_PrintXML(AppObjects);

    AJ_RegisterObjects(AppObjects, NULL);

    while (TRUE) {
        AJ_Message msg;

        if (!connected) {
            status = AJ_StartService(&bus,
                                     NULL,
                                     CONNECT_TIMEOUT,
                                     ServicePort,
                                     ServiceName,
                                     AJ_NAME_REQ_DO_NOT_QUEUE,
                                     NULL);

            if (status != AJ_OK) {
                continue;
            }

            printf("StartService returned %d, session_id=%u\n", status, sessionId);
            connected = TRUE;
        }

        status = AJ_UnmarshalMsg(&bus, &msg, UNMARSHAL_TIMEOUT);

        if (AJ_ERR_TIMEOUT == status) {
            continue;
        }

        if (AJ_OK == status) {
            switch (msg.msgId) {
            case AJ_METHOD_ACCEPT_SESSION:
                {
                    uint16_t port;
                    char* joiner;
                    AJ_UnmarshalArgs(&msg, "qus", &port, &sessionId, &joiner);
                    status = AJ_BusReplyAcceptSession(&msg, TRUE);
                    printf("Accepted session session_id=%u joiner=%s\n", sessionId, joiner);
                }
                break;

            case BASIC_SERVICE_CAT:
                status = AppHandleCat(&msg);
                break;

            case AJ_SIGNAL_SESSION_LOST:
                /* Force a disconnect. */
                status = AJ_ERR_READ;
                break;

            default:
                /* Pass to the built-in handlers. */
                status = AJ_BusHandleBusMessage(&msg);
                break;
            }
        }

        /* Messages MUST be discarded to free resources. */
        AJ_CloseMsg(&msg);

        if (status == AJ_ERR_READ) {
            printf("AllJoyn disconnect.\n");
            AJ_Disconnect(&bus);
            connected = FALSE;

            /* Sleep a little while before trying to reconnect. */
            AJ_Sleep(SLEEP_TIME);
        }
    }

    printf("Basic service exiting with status %d.\n", status);

    return status;
}

#ifdef AJ_MAIN
int main()
{
    return AJ_Main();
}
#endif
