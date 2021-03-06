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

#include "aj_target.h"
#include "aj_status.h"
#include "aj_util.h"
#include "aj_net.h"
#include "aj_disco.h"
#include "aj_debug.h"

typedef struct _NSHeader {
    uint8_t version;
    uint8_t qCount;
    uint8_t aCount;
    uint8_t ttl;
    uint8_t flags;
    uint8_t nameCount;
} NSHeader;

/*
 * Message V1 flag definitions
 */
#define U6_FLAG 0x01
#define R6_FLAG 0x02
#define U4_FLAG 0x04
#define R4_FLAG 0x08
#define C_FLAG  0x10
#define G_FLAG  0x20

#define MSG_TYPE(flags) ((flags) & 0xC0)

#define WHO_HAS_MSG   0x80
#define IS_AT_MSG     0x40

#define MSG_VERSION(flags)  ((flags) & 0x0F)

#define MSG_V0 0x00
#define MSG_V1 0x01
#define NSV_V1 0x10

static AJ_Status ComposeWhoHas(AJ_IOBuffer* txBuf, const char* prefix)
{
    size_t preLen = strlen(prefix);
    NSHeader* hdr = (NSHeader*)txBuf->writePtr;
    uint8_t* p = txBuf->writePtr + 6;
    size_t outLen = (6 + preLen + 2);

    if (outLen > AJ_IO_BUF_SPACE(txBuf)) {
        return AJ_ERR_RESOURCES;
    }
    hdr->version = MSG_V1 | NSV_V1;
    hdr->qCount = 1;
    hdr->aCount = 0;
    hdr->ttl = 0;
    hdr->flags = WHO_HAS_MSG;
    hdr->nameCount = 1;
    *p++ = (uint8_t)(preLen + 1);
    memcpy(p, prefix, preLen);
    /*
     * Tack wild-card onto the end of the name to indicate it's prefix
     */
    p[preLen] = '*';
    txBuf->writePtr += outLen;
    return AJ_OK;
}

static AJ_Status ParseIsAt(AJ_IOBuffer* rxBuf, const char* prefix, AJ_Service* service)
{
    AJ_Status status = AJ_ERR_NO_MATCH;
    size_t preLen = strlen(prefix);
    NSHeader* hdr = (NSHeader*)rxBuf->readPtr;
    uint32_t len = AJ_IO_BUF_AVAIL(rxBuf);
    uint8_t* p = rxBuf->readPtr + 4;
    uint8_t* eod = (uint8_t*)hdr + len;

    service->addrTypes = 0;

    /*
     * Silently ignore versions we don't know how to parse
     */
    if (MSG_VERSION(hdr->version) != MSG_V1) {
        return status;
    }
    /*
     * Questions come in first - we currently ignore them
     */
    while (hdr->qCount--) {
        uint8_t flags = *p++;
        uint8_t nameCount = *p++;
        /*
         * Questions must be WHO_HAS messages
         */
        if (MSG_TYPE(flags) != WHO_HAS_MSG) {
            return AJ_ERR_INVALID;
        }
        while (nameCount--) {
            uint8_t sz = *p++;
            p += sz;
            if (p > eod) {
                status = AJ_ERR_END_OF_DATA;
                goto Exit;
            }
        }
    }
    /*
     * Now the answers - this is what we are looking for
     */
    while (hdr->aCount--) {
        uint8_t flags = *p++;
        uint8_t nameCount = *p++;
        /*
         * Answers must be IS_AT messages
         */
        if (MSG_TYPE(flags) != IS_AT_MSG) {
            return AJ_ERR_INVALID;
        }
        /*
         * Must be reliable IPV4 or IPV6
         */
        if (!(flags & (R4_FLAG | R6_FLAG))) {
            return status;
        }
        /*
         * Get transport mask
         */
        service->transportMask = (p[0] << 8) | p[1];
        p += 2;
        /*
         * Decode addresses
         */
        if (flags & R4_FLAG) {
            memcpy(&service->ipv4, p, sizeof(service->ipv4));
            p += sizeof(service->ipv4);
            service->ipv4port = (p[0] << 8) | p[1];
            p += 2;
            service->addrTypes |= AJ_ADDR_IPV4;
        }
        if (flags & U4_FLAG) {
            p += sizeof(service->ipv4) + 2;
        }
        if (flags & R6_FLAG) {
            memcpy(&service->ipv6, p, sizeof(service->ipv6));
            p += sizeof(service->ipv6);
            service->ipv6port = (p[0] << 8) | p[1];
            p += 2;
            service->addrTypes |= AJ_ADDR_IPV6;
        }
        if (flags & U6_FLAG) {
            p += sizeof(service->ipv6) + 2;
        }
        /*
         * Skip guid if it's present
         */
        if (flags & G_FLAG) {
            uint8_t sz = *p++;
            len -= 1 + sz;
            p += sz;
        }
        if (p >= eod) {
            return AJ_ERR_END_OF_DATA;
        }
        /*
         * Iterate over the names
         */
        while (nameCount--) {
            uint8_t sz = *p++;
            {
                char sav = p[sz];
                p[sz] = 0;
                AJ_Printf("Found %s IP %x\n", p, service->addrTypes);
                p[sz] = sav;
            }
            if ((preLen <= sz) && (memcmp(p, prefix, preLen) == 0)) {
                status = AJ_OK;
                goto Exit;
            }
            p += sz;
            if (p > eod) {
                status = AJ_ERR_END_OF_DATA;
                goto Exit;
            }
        }
    }

Exit:
    return status;
}

/*
 * How many times we sent WHO-HAS
 */
#define WHO_HAS_REPEAT         4

/*
 * How long to wait for a response to our WHO-HAS
 */
#define RX_TIMEOUT          1000

AJ_Status AJ_Discover(const char* prefix, AJ_Service* service, uint32_t timeout)
{
    AJ_Status status;
    AJ_Time stopwatch;
    AJ_Time recvStopWatch;
    AJ_NetSocket sock;

    AJ_Printf("Starting discovery\n");
    /*
     * Initialize the timer
     */
    AJ_InitTimer(&stopwatch);
    /*
     * Enable multicast I/O for the discovery packets.
     */
    status = AJ_Net_MCastUp(&sock);
    if (status != AJ_OK) {
        return status;
    }
    while ((int32_t)timeout > 0) {
        AJ_IO_BUF_RESET(&sock.tx);
        ComposeWhoHas(&sock.tx, prefix);
        status = sock.tx.send(&sock.tx);
        AJ_Printf("Sending who-has \"%s\". Result = %d\n", prefix, status);
        /*
         * Pause between sending each WHO-HAS
         */

        AJ_InitTimer(&recvStopWatch);
        while (TRUE) {
            AJ_IO_BUF_RESET(&sock.rx);
            status = sock.rx.recv(&sock.rx, AJ_IO_BUF_SPACE(&sock.rx), RX_TIMEOUT);
            if (status == AJ_OK) {
                memset(service, 0, sizeof(AJ_Service));
                status = ParseIsAt(&sock.rx, prefix, service);
                if (status == AJ_OK) {
                    goto _Exit;
                }
            }
            if (AJ_GetElapsedTime(&recvStopWatch, TRUE) > RX_TIMEOUT) {
                break;
            }
        }

        timeout -= AJ_GetElapsedTime(&stopwatch, FALSE);
    }
_Exit:
    /*
     * All done with multicast for now
     */
    AJ_Net_MCastDown(&sock);

    AJ_Printf("Stopping discovery\n");
    return status;
}
