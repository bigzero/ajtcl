/**
 * @file
 */
/******************************************************************************
 * Copyright 2013, Qualcomm Innovation Center, Inc.
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

#include "aj_target.h"
#include "alljoyn.h"


#define CONNECT_TIMEOUT    (1000ul * 200)
#define UNMARSHAL_TIMEOUT  (1000ul * 5)
#define METHOD_TIMEOUT     (1000ul * 3)

/// globals
AJ_Status status = AJ_OK;
AJ_BusAttachment bus;
uint8_t connected = FALSE;
uint32_t sessionId = 0ul;
AJ_Status authStatus = AJ_ERR_NULL;

static const char ServiceName[] = "org.alljoyn.Bus.test.bastress";
static const uint16_t ServicePort = 25;
static uint32_t authenticate = TRUE;

static const char* const testInterface[] = {
    "org.alljoyn.Bus.test.bastress",
    "?cat inStr1<s inStr2<s outStr>s",
    NULL
};


static const AJ_InterfaceDescription testInterfaces[] = {
    testInterface,
    NULL
};

/**
 * Objects implemented by the application
 */
static const AJ_Object AppObjects[] = {
    { "/sample", testInterfaces },
    { NULL }
};

#define APP_MY_CAT  AJ_APP_MESSAGE_ID(0, 0, 0)


/*
 * Let the application do some work
 */
void AppDoWork()
{
    AJ_Printf("AppDoWork\n");
}


static const char PWD[] = "1234";

static uint32_t PasswordCallback(uint8_t* buffer, uint32_t bufLen)
{
    memcpy(buffer, PWD, sizeof(PWD));
    return sizeof(PWD) - 1;
}

void AuthCallback(const void* context, AJ_Status status)
{
    *((AJ_Status*)context) = status;
}


AJ_Status AppHandleCat(AJ_Message* msg)
{
    AJ_Status status = AJ_OK;
    AJ_Message reply;
    char* partA;
    char* partB;
    char* totalString;
    AJ_Printf("%s:%d:%s %d\n", __FILE__, __LINE__, __FUNCTION__, 0);

    AJ_UnmarshalArgs(msg, "ss", &partA, &partB);

    totalString = (char*) AJ_Malloc(strlen(partA) + strlen(partB));
    strcpy(totalString, partA);
    strcpy(totalString + strlen(partA), partB);

    AJ_MarshalReplyMsg(msg, &reply);
    AJ_MarshalArgs(&reply, "s", totalString);

    status = AJ_DeliverMsg(&reply);
    AJ_Free(totalString);
    return status;
}

int AJ_Main()
{
    // you're connected now, so print out the data:
    AJ_Printf("You're connected to the network\n");
    AJ_Initialize();
    AJ_PrintXML(AppObjects);
    AJ_RegisterObjects(AppObjects, NULL);

    while (TRUE) {
        AJ_Message msg;

        if (!connected) {
            status = AJ_StartService(&bus, NULL, CONNECT_TIMEOUT, ServicePort, ServiceName, AJ_NAME_REQ_DO_NOT_QUEUE, NULL);
            if (status == AJ_OK) {
                AJ_Printf("StartService returned %d\n", status);
                connected = TRUE;
                if (authenticate) {
                    AJ_BusSetPasswordCallback(&bus, PasswordCallback);
                } else {
                    authStatus = AJ_OK;
                }
            } else {
                AJ_Printf("StartClient returned %d\n", status);
                continue;
            }
        }

        status = AJ_UnmarshalMsg(&bus, &msg, UNMARSHAL_TIMEOUT);
        if (status != AJ_OK) {
            if (status == AJ_ERR_TIMEOUT) {
                AppDoWork();
                continue;
            }
        }

        if (status == AJ_OK) {
            switch (msg.msgId) {

            case AJ_METHOD_ACCEPT_SESSION:
                {
                    uint16_t port;
                    char* joiner;
                    AJ_Printf("Accepting...\n");
                    AJ_UnmarshalArgs(&msg, "qus", &port, &sessionId, &joiner);
                    status = AJ_BusReplyAcceptSession(&msg, TRUE);
                    if (status == AJ_OK) {
                        AJ_Printf("Accepted session session_id=%u joiner=%s\n", sessionId, joiner);
                    } else {
                        AJ_Printf("AJ_BusReplyAcceptSession: error %d\n", status);
                    }
                }
                break;

            case APP_MY_CAT:
                status = AppHandleCat(&msg);
                break;

            case AJ_SIGNAL_SESSION_LOST:
                /*
                 * don't force a disconnect, be ready to accept another session
                 */
                break;

            default:
                /*
                 * Pass to the built-in handlers
                 */
                status = AJ_BusHandleBusMessage(&msg);
                break;
            }
        }
        /*
         * Messages must be closed to free resources
         */
        AJ_CloseMsg(&msg);

        if (status == AJ_ERR_READ) {
            AJ_Printf("AllJoyn disconnect\n");
            AJ_Disconnect(&bus);
            connected = FALSE;
            /*
             * Sleep a little while before trying to reconnect
             */
            AJ_Sleep(10 * 1000);
        }
    }

    return 0;
}

#ifdef AJ_MAIN
int main()
{
    return AJ_Main();
}
#endif
