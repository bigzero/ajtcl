/**
 * @file
 */
/******************************************************************************
 * Copyright 2013, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 ******************************************************************************/

#include <stdint.h>
#include <stddef.h>

#include "due_led.h"

#include <alljoyn.h>

void DUE_led_timed(uint32_t msec);
void DUE_led(uint8_t on);

static const char ServiceName[] = "org.alljoyn.sample.ledservice";
static const uint16_t ServicePort = 24;


static const char* testInterface[] = {
    "org.alljoyn.sample.ledcontroller",
    "?Flash msec<u",
    "?On",
    "?Off",
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
    { "/org/alljoyn/sample/ledcontroller", testInterfaces },
    { NULL }
};

/*
 * Message identifiers for the method calls this application implements
 */

#define APP_FLASH   AJ_APP_MESSAGE_ID(0, 0, 0)
#define APP_ON      AJ_APP_MESSAGE_ID(0, 0, 1)
#define APP_OFF     AJ_APP_MESSAGE_ID(0, 0, 2)

static void AppDoWork()
{
    /*
     * This function is called if there are no messages to unmarshal
     */
    printf("do work\n");
}

static const char PWD[] = "ABCDEFGH";

static uint32_t PasswordCallback(uint8_t* buffer, uint32_t bufLen)
{
    memcpy(buffer, PWD, sizeof(PWD));
    return sizeof(PWD) - 1;
}

static AJ_Status AppHandleFlash(AJ_Message* msg)
{
    AJ_Message reply;
    uint32_t timeout;
    AJ_UnmarshalArgs(msg, "u", &timeout);
    printf("AppHandleFlash(%u)\n", timeout);

    DUE_led_timed(timeout);


    AJ_MarshalReplyMsg(msg, &reply);
    return AJ_DeliverMsg(&reply);
}

static AJ_Status AppHandleOnOff(AJ_Message* msg, uint8_t on)
{
    AJ_Message reply;

    printf("AppHandleOnOff(%u)\n", on);
    DUE_led(on);

    AJ_MarshalReplyMsg(msg, &reply);
    return AJ_DeliverMsg(&reply);
}


#define CONNECT_TIMEOUT    (1000 * 1000)
#define UNMARSHAL_TIMEOUT  (1000 * 5)

int AJ_Main(void)
{
    AJ_Status status = AJ_OK;
    AJ_BusAttachment bus;
    uint8_t connected = FALSE;
    uint32_t sessionId = 0;

    /*
     * One time initialization before calling any other AllJoyn APIs
     */
    AJ_Initialize();

    AJ_PrintXML(AppObjects);
    AJ_RegisterObjects(AppObjects, NULL);


    while (TRUE) {
        AJ_Message msg;

        if (!connected) {
            status = AJ_StartService(&bus, "org.alljoyn.router", CONNECT_TIMEOUT, ServicePort, ServiceName, AJ_NAME_REQ_DO_NOT_QUEUE);
            if (status != AJ_OK) {
                continue;
            }
            printf("StartService returned AJ_OK; running %s:%u\n", ServiceName, ServicePort);
            connected = TRUE;
            AJ_BusSetPasswordCallback(&bus, PasswordCallback);
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
                    printf("Accepting...\n");
                    uint16_t port;
                    char* joiner;
                    AJ_UnmarshalArgs(&msg, "qus", &port, &sessionId, &joiner);
                    status = AJ_BusReplyAcceptSession(&msg, TRUE);

                    if (status == AJ_OK) {
                        printf("Accepted session session_id=%u joiner=%s\n", sessionId, joiner);
                    } else {
                        printf("AJ_BusReplyAcceptSession: error %d\n", status);
                    }
                }
                break;

            case APP_FLASH:
                status = AppHandleFlash(&msg);
                break;

            case APP_ON:
                AppHandleOnOff(&msg, TRUE);
                break;

            case APP_OFF:
                AppHandleOnOff(&msg, FALSE);
                break;

            case AJ_SIGNAL_SESSION_LOST:
                /*
                 * Force a disconnect
                 */
                status = AJ_ERR_READ;
                break;

            default:
                /*
                 * Pass to the built-in bus message handlers
                 */
                status = AJ_BusHandleBusMessage(&msg);
                break;
            }
        }
        /*
         * Unarshaled messages must be closed to free resources
         */
        AJ_CloseMsg(&msg);

        if (status == AJ_ERR_READ) {
            printf("AllJoyn disconnect\n");
            AJ_Disconnect(&bus);
            connected = FALSE;
            /*
             * Sleep a little while before trying to reconnect
             */
            AJ_Sleep(10 * 1000);
        }
    }
    printf("svclite EXIT %d\n", status);

    return status;
}

