/**
 * @file
 */
/******************************************************************************
 * Copyright 2012, 2013, Qualcomm Innovation Center, Inc.
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

#include <stdarg.h>

#include "aj_target.h"
#include "aj_status.h"
#include "aj_msg.h"
#include "aj_bufio.h"
#include "aj_connect.h"
#include "aj_guid.h"
#include "aj_util.h"
#include "aj_crypto.h"
#include "aj_introspect.h"
#include "aj_std.h"
#include "aj_debug.h"
#include "aj_bus.h"

#if HOST_IS_LITTLE_ENDIAN
#define HOST_ENDIANESS AJ_LITTLE_ENDIAN
#endif

#if HOST_IS_BIG_ENDIAN
#define HOST_ENDIANESS AJ_BIG_ENDIAN
#endif

#define AJ_STRUCT_CLOSE          ')'
#define AJ_DICT_ENTRY_CLOSE      '}'

/*
 * The size of the MAC for encrypted messages
 */
#define MAC_LENGTH 8

/*
 * The types for each of the header fields.
 */
static const uint8_t TypeForHdr[] = {
    AJ_ARG_INVALID,
    AJ_ARG_OBJ_PATH,    /* AJ_HDR_OBJ_PATH     */
    AJ_ARG_STRING,      /* AJ_HDR_INTERFACE    */
    AJ_ARG_STRING,      /* AJ_HDR_MEMBER       */
    AJ_ARG_STRING,      /* AJ_HDR_ERROR_NAME   */
    AJ_ARG_UINT32,      /* AJ_HDR_REPLY_SERIAL */
    AJ_ARG_STRING,      /* AJ_HDR_DESTINATION  */
    AJ_ARG_STRING,      /* AJ_HDR_SENDER       */
    AJ_ARG_SIGNATURE,   /* AJ_HDR_SIGNATURE    */
    AJ_ARG_UINT32,      /* AJ_HDR_HANDLES      */
    AJ_ARG_INVALID,
    AJ_ARG_INVALID,
    AJ_ARG_INVALID,
    AJ_ARG_INVALID,
    AJ_ARG_INVALID,
    AJ_ARG_INVALID,
    AJ_ARG_UINT32,      /* AJ_HDR_TIMESTAMP         */
    AJ_ARG_UINT16,      /* AJ_HDR_TIME_TO_LIVE      */
    AJ_ARG_UINT32,      /* AJ_HDR_COMPRESSION_TOKEN */
    AJ_ARG_UINT32       /* AJ_HDR_SESSION_ID        */
};

#define AJ_SCALAR    0x10
#define AJ_CONTAINER 0x20
#define AJ_STRING    0x40
#define AJ_VARIANT   0x80

/**
 * Characterizes the various argument types
 */
static const uint8_t TypeFlags[] = {
    0x08 | AJ_CONTAINER,  /* AJ_ARG_STRUCT            '('  */
    0x04 | AJ_CONTAINER,  /* AJ_ARG_ARRAY             'a'  */
    0x04 | AJ_SCALAR,     /* AJ_ARG_BOOLEAN           'b'  */
    0,
    0x08 | AJ_SCALAR,     /* AJ_ARG_DOUBLE            'd'  */
    0,
    0,
    0x01 | AJ_STRING,     /* AJ_ARG_SIGNATURE         'g'  */
    0x04 | AJ_SCALAR,     /* AJ_ARG_HANDLE            'h'  */
    0x04 | AJ_SCALAR,     /* AJ_ARG_INT32             'i'  */
    0,
    0,
    0,
    0,
    0x02 | AJ_SCALAR,     /* AJ_ARG_INT16             'n'  */
    0x04 | AJ_STRING,     /* AJ_ARG_OBJ_PATH          'o'  */
    0,
    0x02 | AJ_SCALAR,     /* AJ_ARG_UINT16            'q'  */
    0,
    0x04 | AJ_STRING,     /* AJ_ARG_STRING            's'  */
    0x08 | AJ_SCALAR,     /* AJ_ARG_UINT64            't'  */
    0x04 | AJ_SCALAR,     /* AJ_ARG_UINT32            'u'  */
    0x01 | AJ_VARIANT,    /* AJ_ARG_VARIANT           'v'  */
    0,
    0x08 | AJ_SCALAR,     /* AJ_ARG_INT64             'x'  */
    0x01 | AJ_SCALAR,     /* AJ_ARG_BYTE              'y'  */
    0,
    0x08 | AJ_CONTAINER,  /* AJ_ARG_DICT_ENTRY        '{'  */
};

/**
 * Macros for indexing into TypeFlags
 */
#define TYPE_FLAG(t) TypeFlags[((t) == AJ_ARG_STRUCT) ? 0 : (t) - 96]

/**
 * Extract the alignment from the TypeFlags
 */
#define ALIGNMENT(t) (TYPE_FLAG(t) & 0xF)

/*
 * For scalar types returns the size of the type
 */
#define SizeOfType(typeId) (TYPE_FLAG(typeId) & 0xF)

/*
 *  Returns true if the specified type is represented as a number
 */
#define IsScalarType(typeId) (TYPE_FLAG(typeId) & AJ_SCALAR)

/*
 * A basic type is a scalar or one of the string types
 */
#define IsBasicType(typeId) (TYPE_FLAG(typeId) & (AJ_STRING | AJ_SCALAR))

/*
 * Checks that the current message is closed
 */
#ifndef NDEBUG
static AJ_Message* currentMsg = NULL;
#endif

static void InitArg(AJ_Arg* arg, uint8_t typeId, const void* val)
{
    if (arg) {
        arg->typeId = typeId;
        arg->flags = 0;
        arg->len = 0;
        arg->val.v_data = (void*)val;
        arg->sigPtr = NULL;
        arg->container = NULL;
    }
}

/*
 * Returns the number of bytes of padding to align the type
 */
static uint32_t PadForType(char typeId, AJ_IOBuffer* ioBuf)
{
    uint8_t* base = (ioBuf->direction == AJ_IO_BUF_RX) ? ioBuf->readPtr : ioBuf->writePtr;
    uint32_t offset = (uint32_t)(base - ioBuf->bufStart);
    uint32_t alignment = ALIGNMENT(typeId);
    return (alignment - offset) & (alignment - 1);
}

#define ENDSWAP16(v) (((v) >> 8) | ((v) << 8))
#define ENDSWAP32(v) (((v) >> 24) | (((v) & 0xFF0000) >> 8) | (((v) & 0x00FF00) << 8) | ((v) << 24))

static void EndianSwap(AJ_Message* msg, uint8_t typeId, void* data, uint32_t num)
{
    if (msg->hdr->endianess != HOST_ENDIANESS) {
        switch (SizeOfType(typeId)) {
        case 2:
            {
                uint16_t* p = (uint16_t*)data;
                while (num--) {
                    uint16_t v = *p;
                    *p++ = ENDSWAP16(v);
                }
            }
            break;

        case 4:
            {
                uint32_t* p = (uint32_t*)data;
                while (num--) {
                    uint32_t v = *p;
                    *p++ = ENDSWAP32(v);
                }
            }
            break;

        case 8:
            {
                uint32_t* p = (uint32_t*)data;
                while (num--) {
                    uint32_t v = p[0];
                    uint32_t u = p[1];
                    *p++ = ENDSWAP32(u);
                    *p++ = ENDSWAP32(v);
                }
            }
            break;
        }
    }
}

/*
 * Computes total size of a message - note header is padded to an 8 byte boundary
 */
static uint32_t MessageLen(AJ_Message* msg)
{
    return sizeof(AJ_MsgHeader) + ((msg->hdr->headerLen + 7) & 0xFFFFFFF8) + msg->hdr->bodyLen;
}

static void InitNonce(AJ_Message* msg, uint8_t role, uint8_t* nonce)
{
    uint32_t serial = msg->hdr->serialNum;
    nonce[0] = role;
    nonce[1] = (uint8_t)(serial >> 24);
    nonce[2] = (uint8_t)(serial >> 16);
    nonce[3] = (uint8_t)(serial >> 8);
    nonce[4] = (uint8_t)(serial);
}

static AJ_Status DecryptMessage(AJ_Message* msg)
{
    AJ_IOBuffer* ioBuf = &msg->bus->sock.rx;
    AJ_Status status;
    uint8_t key[16];
    uint8_t nonce[5];
    uint8_t role = AJ_ROLE_KEY_UNDEFINED;
    uint32_t mlen = MessageLen(msg);
    uint32_t hLen = mlen - msg->hdr->bodyLen;

    /*
     * TODO - handle case where msg endianess is different than the host endianess
     */
    if (msg->hdr->endianess != HOST_ENDIANESS) {
        return AJ_ERR_SECURITY;
    }
    /*
     * Use the group key for multicast and broadcast signals the session key otherwise.
     */
    if ((msg->hdr->msgType == AJ_MSG_SIGNAL) && !msg->destination) {
        status = AJ_GetGroupKey(msg->sender, key);
    } else {
        status = AJ_GetSessionKey(msg->sender, key, &role);
        /*
         * We use the oppsite role when decrypting.
         */
        role ^= 3;
    }
    if (status != AJ_OK) {
        status = AJ_ERR_SECURITY;
    } else {
        InitNonce(msg, role, nonce);
        status = AJ_Decrypt_CCM(key, ioBuf->bufStart, mlen - MAC_LENGTH, hLen, MAC_LENGTH, nonce, sizeof(nonce));
    }
    return status;
}

static AJ_Status EncryptMessage(AJ_Message* msg)
{
    AJ_IOBuffer* ioBuf = &msg->bus->sock.tx;
    AJ_Status status;
    uint8_t key[16];
    uint8_t nonce[5];
    uint8_t role = AJ_ROLE_KEY_UNDEFINED;
    uint32_t mlen = MessageLen(msg);
    uint32_t hlen = mlen - msg->hdr->bodyLen;

    /*
     * Check there is room to append the MAC
     */
    if (AJ_IO_BUF_SPACE(ioBuf) < MAC_LENGTH) {
        return AJ_ERR_RESOURCES;
    }
    msg->hdr->bodyLen += MAC_LENGTH;
    ioBuf->writePtr += MAC_LENGTH;
    /*
     * Use the group key for multicast and broadcast signals the session key otherwise.
     */
    if ((msg->hdr->msgType == AJ_MSG_SIGNAL) && !msg->destination) {
        status = AJ_GetGroupKey(NULL, key);
    } else {
        status = AJ_GetSessionKey(msg->destination, key, &role);
    }
    if (status != AJ_OK) {
        status = AJ_ERR_SECURITY;
    } else {
        InitNonce(msg, role, nonce);
        status = AJ_Encrypt_CCM(key, ioBuf->bufStart, mlen, hlen, MAC_LENGTH, nonce, sizeof(nonce));
    }
    return status;
}

AJ_Status AJ_DeliverMsg(AJ_Message* msg)
{
    AJ_Status status = AJ_OK;
    AJ_IOBuffer* ioBuf = &msg->bus->sock.tx;

    /*
     * If the header has already been marshaled (due to partial delivery) it will be NULL
     */
    if (msg->hdr) {
        /*
         * Write the final body length to the header
         */
        msg->hdr->bodyLen = msg->bodyBytes;
        AJ_DumpMsg("SENDING", msg, TRUE);
        if (msg->hdr->flags & AJ_FLAG_ENCRYPTED) {
            status = EncryptMessage(msg);
        }
    } else {
        /*
         * Check that the entire body was written
         */
        if (msg->bodyBytes) {
            status = AJ_ERR_MARSHAL;
        }
    }
    if (status == AJ_OK) {
        //#pragma calls = AJ_Net_Send
        status = ioBuf->send(ioBuf);
    }
    memset(msg, 0, sizeof(AJ_Message));
    return status;
}

/*
 * Timeout after we have started to unmarshal a message
 */
#define UNMARSHAL_TIMEOUT 550ul

/*
 * Make sure we have the required number of bytes in the I/O buffer
 */
static AJ_Status LoadBytes(AJ_IOBuffer* ioBuf, uint16_t numBytes, uint8_t pad)
{
    AJ_Status status = AJ_OK;

    numBytes += pad;
    /*
     * Needs to be enough headroom in the buffer to satisfy the read
     */
    if (numBytes > (ioBuf->bufSize - AJ_IO_BUF_CONSUMED(ioBuf))) {
        return AJ_ERR_RESOURCES;
    }
    while (AJ_IO_BUF_AVAIL(ioBuf) < numBytes) {
        //#pragma calls = AJ_Net_Recv
        status = ioBuf->recv(ioBuf, numBytes - AJ_IO_BUF_AVAIL(ioBuf), UNMARSHAL_TIMEOUT);
        if (status != AJ_OK) {
            /*
             * Timeouts after we have started to unmarshal a message are a bad sign.
             */
            if (status == AJ_ERR_TIMEOUT) {
                status = AJ_ERR_READ;
            }
            break;
        }
    }
    /*
     * Skip over pad bytes (The wire protocol says these should be zeroes)
     */
    ioBuf->readPtr += pad;
    return status;
}

/*
 * Write bytes to an I/O buffer
 */
static AJ_Status WriteBytes(AJ_Message* msg, const void* data, size_t numBytes, size_t pad)
{
    AJ_Status status = AJ_OK;
    AJ_IOBuffer* ioBuf = &msg->bus->sock.tx;
    if (numBytes && !data) {
        return AJ_ERR_NULL;
    }
    while (numBytes + pad) {
        size_t canWrite = AJ_IO_BUF_SPACE(ioBuf);
        if ((numBytes + pad) > canWrite) {
            /*
             * If we have already marshaled the header we can write what we have in the buffer
             */
            if (msg->hdr) {
                status = AJ_ERR_RESOURCES;
            } else {
                //#pragma calls = AJ_Net_Send
                status = ioBuf->send(ioBuf);
            }
            if (status != AJ_OK) {
                break;
            }
            canWrite = AJ_IO_BUF_SPACE(ioBuf);
        }
        /*
         * Write pad bytes
         */
        while (pad) {
            *ioBuf->writePtr++ = 0;
            --canWrite;
            --pad;
        }
        if (numBytes < canWrite) {
            canWrite = numBytes;
        }
        memcpy(ioBuf->writePtr, data, canWrite);
        ioBuf->writePtr += canWrite;
        numBytes -= canWrite;
    }
    return status;
}

/*
 * Write pad bytes to an I/O buffer
 */
#define WritePad(msg, pad) WriteBytes(msg, NULL, 0, pad)


AJ_Status AJ_CloseMsg(AJ_Message* msg)
{
    AJ_Status status = AJ_OK;
    /*
     * This function is idempotent
     */
    if (msg->bus) {
        AJ_IOBuffer* ioBuf = &msg->bus->sock.rx;
        /*
         * Skip any unconsumed bytes
         */
        while (msg->bodyBytes) {
            uint16_t sz = AJ_IO_BUF_AVAIL(ioBuf);
            sz = min(sz, msg->bodyBytes);
            if (!sz) {
                AJ_IO_BUF_RESET(ioBuf);
                sz = min(msg->bodyBytes, ioBuf->bufSize);
            }
            status = LoadBytes(ioBuf, sz, 0);
            if (status != AJ_OK) {
                break;
            }
            msg->bodyBytes -= sz;
            ioBuf->readPtr += sz;
        }
        memset(msg, 0, sizeof(AJ_Message));
#ifndef NDEBUG
        currentMsg = NULL;
#endif
    }
    return status;
}

/**
 * Get the length of the signature of the first complete type in sig
 */
static uint8_t CompleteTypeSigLen(const char* sig)
{
    if (sig) {
        const char* start = sig;
        int32_t open = 0;
        while (*sig) {
            char typeId = *sig++;
            if (typeId == AJ_STRUCT_CLOSE || typeId == AJ_DICT_ENTRY_CLOSE) {
                if (!open) {
                    return 0;
                }
                --open;
                if (!open) {
                    break;
                }
            } else if (typeId == AJ_ARG_STRUCT || typeId == AJ_ARG_DICT_ENTRY) {
                ++open;
            } else if (!open && typeId != AJ_ARG_ARRAY) {
                break;
            }
        }
        return (uint8_t)(sig - start);
    } else {
        return 0;
    }
}

/*
 * Forward declaration
 */
static AJ_Status Unmarshal(AJ_Message* msg, const char** sig, AJ_Arg* arg);

/*
 * Unmarshal a struct or dict entry argument.
 *
 * @param msg      The message
 * @param sig      The struct signature
 * @param arg      Pointer to the arg structure to return
 */
static AJ_Status UnmarshalStruct(AJ_Message* msg, const char** sig, AJ_Arg* arg, uint8_t pad)
{
    AJ_IOBuffer* ioBuf = &msg->bus->sock.rx;
    AJ_Status status = LoadBytes(ioBuf, 0, pad);
    arg->val.v_data = ioBuf->readPtr;
    arg->sigPtr = *sig;
    /*
     * Consume the entire struct signature.
     */
    *sig -= 1;
    *sig += CompleteTypeSigLen(*sig);
    return status;
}

/*
 * Unmarshal an array argument.
 *
 * @param msg   The message
 * @param sig   The array element signature
 * @param arg   Pointer to the structure to return the array
 */
static AJ_Status UnmarshalArray(AJ_Message* msg, const char** sig, AJ_Arg* arg, uint8_t pad)
{
    AJ_Status status;
    AJ_IOBuffer* ioBuf = &msg->bus->sock.rx;
    char typeId = **sig;
    uint32_t numBytes;

    /*
     * Get the byte count for the array
     */
    status = LoadBytes(ioBuf, 4, pad);
    if (status != AJ_OK) {
        return status;
    }
    EndianSwap(msg, AJ_ARG_UINT32, ioBuf->readPtr, 1);
    numBytes = *((uint32_t*)ioBuf->readPtr);
    ioBuf->readPtr += 4;
    /*
     * We are already aligned on 4 byte boundary but there may be padding after the array length if
     * the array element types align on an 8 byte boundary.
     */
    pad = PadForType(typeId, ioBuf);
    status = LoadBytes(ioBuf, numBytes, pad);
    if (status != AJ_OK) {
        return status;
    }
    arg->val.v_data = ioBuf->readPtr;
    arg->sigPtr = *sig;
    arg->len = numBytes;
    if (IsScalarType(typeId)) {
        /*
         * For scalar types we do an inplace endian swap (if needed) and return a pointer into the read buffer.
         */
        EndianSwap(msg, typeId, (void*)arg->val.v_data, arg->len);
        ioBuf->readPtr += numBytes;
        arg->typeId = typeId;
        arg->flags = AJ_ARRAY_FLAG;
    } else {
        /*
         * For all other types the elements must be individually unmarshalled.
         */
        arg->typeId = AJ_ARG_ARRAY;
    }
    /*
     * Consume the array element signature.
     */
    *sig += CompleteTypeSigLen(*sig);
    return status;
}

/*
 * Unmarshal a single argument
 */
static AJ_Status Unmarshal(AJ_Message* msg, const char** sig, AJ_Arg* arg)
{
    AJ_Status status;
    AJ_IOBuffer* ioBuf = &msg->bus->sock.rx;
    char typeId;
    uint32_t pad;
    uint32_t sz;

    memset(arg, 0, sizeof(AJ_Arg));

    if (!*sig || !**sig) {
        return AJ_ERR_END_OF_DATA;
    }

    typeId = **sig;
    *sig += 1;
    pad = PadForType(typeId, ioBuf);

    if (IsScalarType(typeId)) {
        sz = SizeOfType(typeId);
        status = LoadBytes(ioBuf, sz, pad);
        if (status != AJ_OK) {
            return status;
        }
        /*
         * For numeric types we just return a pointer into the buffer
         */
        arg->typeId = typeId;
        arg->val.v_byte = ioBuf->readPtr;
        arg->len = 0;
        ioBuf->readPtr += sz;
        EndianSwap(msg, typeId, (void*)arg->val.v_data, 1);
    } else if (TYPE_FLAG(typeId) & (AJ_STRING | AJ_VARIANT)) {
        /*
         * Length field for a signature is 1 byte, for regular strings its 4 bytes
         */
        uint32_t lenSize = ALIGNMENT(typeId);
        /*
         * Read the string length. Note the length doesn't include the terminating NUL
         * so an empty string in encoded as two zero bytes.
         */
        status = LoadBytes(ioBuf, lenSize, pad);
        if (status != AJ_OK) {
            return status;
        }
        if (lenSize == 4) {
            EndianSwap(msg, AJ_ARG_UINT32, ioBuf->readPtr, 1);
            sz = *((uint32_t*)ioBuf->readPtr);
        } else {
            sz = (uint32_t)(*ioBuf->readPtr);
        }
        ioBuf->readPtr += lenSize;
        status = LoadBytes(ioBuf, sz + 1, 0);
        if (status != AJ_OK) {
            return status;
        }
        arg->typeId = typeId;
        arg->len = sz;
        arg->val.v_string = (char*)ioBuf->readPtr;
        ioBuf->readPtr += sz + 1;
        /*
         * If unmarshalling a variant store offset to start of signature
         */
        if (typeId == AJ_ARG_VARIANT) {
            msg->varOffset = (uint8_t)(sz + 1);
        }
    } else if (typeId == AJ_ARG_ARRAY) {
        status = UnmarshalArray(msg, sig, arg, pad);
    } else if ((typeId == AJ_ARG_STRUCT) || (typeId == AJ_ARG_DICT_ENTRY)) {
        arg->typeId = typeId;
        status = UnmarshalStruct(msg, sig, arg, pad);
    } else {
        status = AJ_ERR_UNMARSHAL;
    }
    return status;
}

static const AJ_MsgHeader internalErrorHdr = { HOST_ENDIANESS, AJ_MSG_ERROR, 0, 0, 0, 1, 0 };

AJ_Status AJ_UnmarshalMsg(AJ_BusAttachment* bus, AJ_Message* msg, uint32_t timeout)
{
    AJ_Status status;
    AJ_IOBuffer* ioBuf = &bus->sock.rx;
    uint8_t* endOfHeader;
    uint32_t hdrPad;
    /*
     * Clear message then set the bus
     */
    memset(msg, 0, sizeof(AJ_Message));
    msg->msgId = AJ_INVALID_MSG_ID;
    msg->bus = bus;
    /*
     * Move any unconsumed data to the start of the I/O buffer
     */
    AJ_IOBufRebase(ioBuf);
    /*
     * Load the message header
     */
    while (AJ_IO_BUF_AVAIL(ioBuf) < sizeof(AJ_MsgHeader)) {
        //#pragma calls = AJ_Net_Recv
        status = ioBuf->recv(ioBuf, sizeof(AJ_MsgHeader) - AJ_IO_BUF_AVAIL(ioBuf), timeout);
        if (status != AJ_OK) {
            /*
             * If there were no messages to receive check if we have any methods call that have
             * timed-out and if so generate an internal error message to allow the application to
             * proceed.
             */
            if ((status == AJ_ERR_TIMEOUT) && AJ_TimedOutMethodCall(msg)) {
                msg->hdr = (AJ_MsgHeader*)&internalErrorHdr;
                msg->error = AJ_ErrTimeout;
                msg->sender = AJ_GetUniqueName(msg->bus);
                msg->destination = msg->sender;
                status = AJ_OK;
            }

            return status;
        }
    }
    /*
     * Header was unmarsalled directly into the rx buffer
     */
    msg->hdr = (AJ_MsgHeader*)ioBuf->bufStart;
    ioBuf->readPtr += sizeof(AJ_MsgHeader);
    /*
     * Quick sanity check on the header - unrecoverable error if this check fails
     */
    if ((msg->hdr->endianess != AJ_LITTLE_ENDIAN) && (msg->hdr->endianess != AJ_BIG_ENDIAN)) {
        return AJ_ERR_READ;
    }
    /*
     * Endian swap header info - conventiently they are contiguous in the header
     */
    EndianSwap(msg, AJ_ARG_INT32, &msg->hdr->bodyLen, 3);
    msg->bodyBytes = msg->hdr->bodyLen;
    /*
     * The header is null padded to an 8 bytes boundary
     */
    hdrPad = (8 - msg->hdr->headerLen) & 7;
    /*
     * Load the header
     */
    status = LoadBytes(ioBuf, msg->hdr->headerLen + hdrPad, 0);
    if (status != AJ_OK) {
        return status;
    }
#ifndef NDEBUG
    /*
     * Check that messages are getting closed
     */
    AJ_ASSERT(!currentMsg);
    currentMsg = msg;
#endif
    /*
     * Assume an empty signature
     */
    msg->signature = "";
    /*
     * We have the header in the buffer now we can unmarshal the header fields
     */
    endOfHeader = ioBuf->bufStart + sizeof(AJ_MsgHeader) + msg->hdr->headerLen;
    while (ioBuf->readPtr < endOfHeader) {
        const char* fieldSig;
        uint8_t fieldId;
        AJ_Arg hdrVal;
        /*
         * Custom unmarshal the header field - signature is "(yv)" so starts off with STRUCT aligment.
         */
        status = LoadBytes(ioBuf, 4, PadForType(AJ_ARG_STRUCT, ioBuf));
        if (status != AJ_OK) {
            break;
        }
        fieldId = ioBuf->readPtr[0];
        fieldSig = (const char*)&ioBuf->readPtr[2];
        ioBuf->readPtr += 4;
        /*
         * Now unmarshal the field value
         */
        status = Unmarshal(msg, &fieldSig, &hdrVal);
        if (status != AJ_OK) {
            break;
        }
        /*
         * Check the field has the type we expect - we ignore fields we don't know
         */
        if ((fieldId <= AJ_HDR_SESSION_ID) && (TypeForHdr[fieldId] != hdrVal.typeId)) {
            status = AJ_ERR_UNMARSHAL;
            break;
        }
        /*
         * Set the field value in the message
         */
        switch (fieldId) {
        case AJ_HDR_OBJ_PATH:
            msg->objPath = hdrVal.val.v_objPath;
            break;

        case AJ_HDR_INTERFACE:
            msg->iface = hdrVal.val.v_string;
            break;

        case AJ_HDR_MEMBER:
            msg->member = hdrVal.val.v_string;
            break;

        case AJ_HDR_ERROR_NAME:
            msg->error = hdrVal.val.v_string;
            break;

        case AJ_HDR_REPLY_SERIAL:
            msg->replySerial = *(hdrVal.val.v_uint32);
            break;

        case AJ_HDR_DESTINATION:
            msg->destination = hdrVal.val.v_string;
            break;

        case AJ_HDR_SENDER:
            msg->sender = hdrVal.val.v_string;
            break;

        case AJ_HDR_SIGNATURE:
            msg->signature = hdrVal.val.v_signature;
            break;

        case AJ_HDR_TIMESTAMP:
            msg->timestamp = *(hdrVal.val.v_uint32);
            break;

        case AJ_HDR_TIME_TO_LIVE:
            msg->ttl = *(hdrVal.val.v_uint32);
            break;

        case AJ_HDR_SESSION_ID:
            msg->sessionId = *(hdrVal.val.v_uint32);
            break;

        case AJ_HDR_HANDLES:
        case AJ_HDR_COMPRESSION_TOKEN:
        default:
            /* Ignored */
            break;
        }
    }
    if (status == AJ_OK) {
        AJ_ASSERT(ioBuf->readPtr == endOfHeader);
        /*
         * Consume the header pad bytes.
         */
        ioBuf->readPtr += hdrPad;
        /*
         * If the message is encrypted load the entire message body and decrypt it.
         */
        if (msg->hdr->flags & AJ_FLAG_ENCRYPTED) {
            status = LoadBytes(ioBuf, msg->hdr->bodyLen, 0);
            if (status == AJ_OK) {
                status = DecryptMessage(msg);
            }
        }
        /*
         * Toggle the AUTO_START flag so in the API no flags == 0
         *
         * Note we must do this after decrypting the message or message authentication will fail.
         */
        msg->hdr->flags ^= AJ_FLAG_AUTO_START;
        /*
         * If the message looks good try to identify it.
         */
        if (status == AJ_OK) {
            status = AJ_IdentifyMessage(msg);
        }
    } else {
        /*
         * Consume entire header
         */
        ioBuf->readPtr = endOfHeader + hdrPad;
    }
    if (status == AJ_OK) {
        AJ_DumpMsg("RECEIVED", msg, FALSE);
    } else {
        /*
         * TODO - should this be silent?
         */
        AJ_Printf("Discarding bad message %d\n", status);
        AJ_DumpMsg("DISCARDING", msg, FALSE);
        AJ_CloseMsg(msg);
    }

    return status;
}

AJ_Status AJ_UnmarshalArg(AJ_Message* msg, AJ_Arg* arg)
{
    AJ_Status status;
    AJ_IOBuffer* ioBuf = &msg->bus->sock.rx;
    AJ_Arg* container = msg->outer;
    uint8_t* argStart = ioBuf->readPtr;
    size_t consumed;

    if (msg->varOffset) {
        /*
         * Unmarshaling a variant - get the signature from the I/O buffer
         */
        const char* sig = (const char*)(ioBuf->readPtr - msg->varOffset);
        msg->varOffset = 0;
        status = Unmarshal(msg, &sig, arg);
    } else if (container) {
        /*
         * Unmarshaling a component of a container use the container's signature
         */
        if (container->typeId == AJ_ARG_ARRAY) {
            size_t len = (uint16_t)(ioBuf->readPtr - (uint8_t*)container->val.v_data);
            /*
             * Return an error status if there are no more array elements.
             */
            if (len == container->len) {
                memset(arg, 0, sizeof(AJ_Arg));
                status = AJ_ERR_NO_MORE;
            } else {
                const char* sig = container->sigPtr;
                status = Unmarshal(msg, &sig, arg);
            }
        } else {
            status = Unmarshal(msg, &container->sigPtr, arg);
        }
    } else {
        const char* sig = msg->signature + msg->sigOffset;
        /*
         * Unmarshalling anything else use the message signature
         */
        status = Unmarshal(msg, &sig, arg);
        msg->sigOffset = (uint8_t)(sig - msg->signature);
    }
    consumed = (ioBuf->readPtr - argStart);
    if (consumed > msg->bodyBytes) {
        /*
         * Unrecoverable
         */
        status = AJ_ERR_READ;
    } else {
        msg->bodyBytes -= (uint16_t)consumed;
    }
    return status;
}

AJ_Status AJ_UnmarshalArgs(AJ_Message* msg, const char* sig, ...)
{
    AJ_Status status = AJ_OK;
    AJ_Arg arg;
    va_list argp;

    va_start(argp, sig);
    while (*sig) {
        uint8_t typeId = (uint8_t)*sig++;
        void* val = va_arg(argp, void*);

        if (!IsBasicType(typeId)) {
            status = AJ_ERR_UNEXPECTED;
            break;
        }
        status = AJ_UnmarshalArg(msg, &arg);
        if (status != AJ_OK) {
            break;
        }
        if (arg.typeId != typeId) {
            status = AJ_ERR_UNMARSHAL;
            break;
        }
        if (IsScalarType(typeId)) {
            switch (SizeOfType(typeId)) {
            case 1:
                *((uint8_t*)val) = *arg.val.v_byte;
                break;

            case 2:
                *((uint16_t*)val) = *arg.val.v_uint16;
                break;

            case 4:
                *((uint32_t*)val) = *arg.val.v_uint32;
                break;

            case 8:
                *((uint64_t*)val) = *arg.val.v_uint64;
                break;
            }
        } else {
            *((const char**)val) = arg.val.v_string;
        }
    }
    va_end(argp);
    return status;
}

AJ_Status AJ_UnmarshalRaw(AJ_Message* msg, const void** data, size_t len, size_t* actual)
{
    AJ_Status status;
    size_t sz;
    AJ_IOBuffer* ioBuf = &msg->bus->sock.rx;

    /*
     * A soon as we start marshaling raw the header will become invalid so NULL it out
     */
    if (msg->hdr) {
        uint8_t typeId = msg->signature[msg->sigOffset];
        uint8_t pad;
        /*
         * There must be arguments to unmarshal
         */
        if (!typeId) {
            return AJ_ERR_SIGNATURE;
        }
        /*
         * There may be padding before the argument
         */
        pad = PadForType(typeId, ioBuf);
        if (pad > msg->bodyBytes) {
            return AJ_ERR_UNMARSHAL;
        }
        LoadBytes(ioBuf, 0, pad);
        msg->bodyBytes -= pad;
        /*
         * Standard signature matching is now meaningless
         */
        msg->signature = "";
        msg->sigOffset = 0;
        msg->hdr = NULL;
    }
    /*
     * Return an error if caller is attempting read off the end of the body
     */
    if (len > msg->bodyBytes) {
        return AJ_ERR_UNMARSHAL;
    }
    /*
     * We want to return the requested data as contiguous bytes if possible
     */
    sz = AJ_IO_BUF_AVAIL(ioBuf);
    if (sz < len) {
        AJ_IOBufRebase(ioBuf);
    }
    /*
     * If we try to load more than the buffer size we will get an error
     */
    status = LoadBytes(ioBuf, (uint16_t)min(len, ioBuf->bufSize), 0);
    if (status == AJ_OK) {
        sz = AJ_IO_BUF_AVAIL(ioBuf);
        if (sz < len) {
            len = sz;
        }
        *data = ioBuf->readPtr;
        *actual = len;
        ioBuf->readPtr += len;
        msg->bodyBytes -= (uint16_t)len;
    }
    return status;
}

AJ_Status AJ_UnmarshalContainer(AJ_Message* msg, AJ_Arg* arg, uint8_t typeId)
{
    AJ_Status status = AJ_ERR_UNMARSHAL;

    if ((TYPE_FLAG(typeId) & AJ_CONTAINER)) {
        status = AJ_UnmarshalArg(msg, arg);
        if (status == AJ_OK) {
            if (arg->typeId == typeId) {
                arg->container = msg->outer;
                msg->outer = arg;
            } else {
                status =  AJ_ERR_UNMARSHAL;
            }
        }
    }
    return status;
}

AJ_Status AJ_UnmarshalCloseContainer(AJ_Message* msg, AJ_Arg* arg)
{
    AJ_ASSERT(TYPE_FLAG(arg->typeId) & AJ_CONTAINER);
    AJ_ASSERT(msg->outer == arg);

    msg->outer = arg->container;

    if (arg->typeId == AJ_ARG_ARRAY) {
        AJ_IOBuffer* ioBuf = &msg->bus->sock.rx;
        /*
         * Check that all the array elements have been unmarshaled
         */
        size_t len = (uint16_t)(ioBuf->readPtr - (uint8_t*)arg->val.v_data);
        if (len != arg->len) {
            return AJ_ERR_UNMARSHAL;
        }
    } else {
        /*
         * Check that all of the struct elements have been unmarshaled
         */
        if ((arg->typeId == AJ_ARG_STRUCT) && (*arg->sigPtr != AJ_STRUCT_CLOSE)) {
            return AJ_ERR_SIGNATURE;
        }
        if ((arg->typeId == AJ_ARG_DICT_ENTRY) && (*arg->sigPtr != AJ_DICT_ENTRY_CLOSE)) {
            return AJ_ERR_SIGNATURE;
        }
    }
    return AJ_OK;
}

AJ_Status AJ_UnmarshalVariant(AJ_Message* msg, const char** sig)
{
    AJ_Arg arg;
    AJ_Status status = AJ_UnmarshalArg(msg, &arg);
    if (status == AJ_OK) {
        if (sig) {
            *sig = arg.val.v_string;
        }
    }
    return status;
}

/*
 * Forward declaration
 */
static AJ_Status Marshal(AJ_Message* msg, const char** sig, AJ_Arg* arg);

static AJ_Status MarshalContainer(AJ_Message* msg, const char** sig, AJ_Arg* arg, uint8_t pad)
{
    AJ_Status status;
    AJ_IOBuffer* ioBuf = &msg->bus->sock.tx;

    *sig -= 1;
    arg->sigPtr = *sig + 1;
    if (**sig == AJ_ARG_ARRAY) {
        /*
         * Reserve space for the length and save a pointer to it
         */
        status = WriteBytes(msg, NULL, 0, pad + 4);
        arg->val.v_data = ioBuf->writePtr - 4;
        /*
         * Might need to pad if the elements align on an 8 byte boundary
         */
        if (status == AJ_OK) {
            status = WritePad(msg, PadForType(arg->sigPtr[0], ioBuf));
        }
    } else {
        status = WritePad(msg, pad);
    }
    /*
     * Consume container signature
     */
    *sig += CompleteTypeSigLen(*sig);
    return status;
}

static AJ_Status Marshal(AJ_Message* msg, const char** sig, AJ_Arg* arg)
{
    AJ_Status status = AJ_OK;
    AJ_IOBuffer* ioBuf = &msg->bus->sock.tx;
    char typeId = **sig;
    uint32_t pad = PadForType(typeId, ioBuf);
    size_t sz;

    if (!arg) {
        return AJ_ERR_NULL;
    }
    *sig += 1;
    if (IsScalarType(arg->typeId)) {
        if (arg->flags & AJ_ARRAY_FLAG) {
            if ((typeId != AJ_ARG_ARRAY) || (**sig != arg->typeId)) {
                return AJ_ERR_MARSHAL;
            }
            *sig += 1;
            sz = arg->len;
            status = WriteBytes(msg, &sz, 4, pad);
            if (status == AJ_OK) {
                /*
                 * May need to pad if the elements required 8 byte alignment
                 */
                pad = PadForType(arg->typeId, ioBuf);
            }
        } else {
            if (typeId != arg->typeId) {
                return AJ_ERR_MARSHAL;
            }
            sz = SizeOfType(typeId);
        }
        if (status == AJ_OK) {
            status = WriteBytes(msg, arg->val.v_data, sz, pad);
        }
    } else if (TYPE_FLAG(typeId) & (AJ_STRING | AJ_VARIANT)) {
        if (typeId != arg->typeId) {
            return AJ_ERR_MARSHAL;
        }
        sz = arg->len ? arg->len : strlen(arg->val.v_string);
        /*
         * Length field for a signature is 1 byte, for regular strings its 4 bytes
         */
        if (ALIGNMENT(typeId) == 1) {
            uint8_t szu8 = (uint8_t)sz;
            if (sz > 255) {
                return AJ_ERR_MARSHAL;
            }
            status = WriteBytes(msg, &szu8, 1, pad);
        } else {
            status = WriteBytes(msg, &sz, 4, pad);
        }
        if (status == AJ_OK) {
            status = WriteBytes(msg, arg->val.v_string, sz, 0);
            /*
             * String must be NUL terminated on the wire
             */
            if (status == AJ_OK) {
                status = WritePad(msg, 1);
            }
            /*
             * If marshalling a variant store offset to start of signature
             */
            if (typeId == AJ_ARG_VARIANT) {
                msg->varOffset = (uint8_t)(sz + 1);
            }
        }
    } else if (TYPE_FLAG(typeId) & AJ_CONTAINER) {
        if (typeId != arg->typeId) {
            return AJ_ERR_MARSHAL;
        }
        status = MarshalContainer(msg, sig, arg, pad);
    } else {
        return AJ_ERR_MARSHAL;
    }
    return status;
}

static AJ_Status MarshalMsg(AJ_Message* msg, uint8_t msgType, uint32_t msgId, uint8_t flags)
{
    AJ_Status status = AJ_OK;
    AJ_IOBuffer* ioBuf = &msg->bus->sock.tx;
    uint8_t fieldId;
    uint8_t secure = FALSE;

    /*
     * Use the msgId to lookup information in the object and interface descriptions to
     * initialize the message header fields.
     */
    status = AJ_InitMessageFromMsgId(msg, msgId, msgType, &secure);
    if (status != AJ_OK) {
        return status;
    }

    AJ_IO_BUF_RESET(ioBuf);

    msg->hdr = (AJ_MsgHeader*)ioBuf->bufStart;
    memset(msg->hdr, 0, sizeof(AJ_MsgHeader));
    ioBuf->writePtr += sizeof(AJ_MsgHeader);

    msg->hdr->endianess = HOST_ENDIANESS;
    msg->hdr->msgType = msgType;
    msg->hdr->flags = flags;
    if (secure) {
        msg->hdr->flags |= AJ_FLAG_ENCRYPTED;
    }

    /*
     * The wire-protocol calls this flag NO_AUTO_START we toggle the meaning in the API
     * so the default flags value can be zero.
     */
    msg->hdr->flags ^= AJ_FLAG_AUTO_START;
    /*
     * Serial number cannot be zero (wire-spec wierdness)
     */
    do { msg->hdr->serialNum = msg->bus->serial++; } while (msg->bus->serial == 1);
    /*
     * Marshal the header fields
     */
    for (fieldId = AJ_HDR_OBJ_PATH; fieldId <= AJ_HDR_SESSION_ID; ++fieldId) {
        char typeId = TypeForHdr[fieldId];
        char buf[4];
        const char* fieldSig = &buf[2];
        AJ_Arg hdrVal;
        /*
         * Skip field id's that are not currently used.
         */
        if (typeId == AJ_ARG_INVALID) {
            continue;
        }
        InitArg(&hdrVal, typeId, NULL);
        switch (fieldId) {
        case AJ_HDR_OBJ_PATH:
            if ((msgType == AJ_MSG_METHOD_CALL) || (msgType == AJ_MSG_SIGNAL)) {
                hdrVal.val.v_objPath = msg->objPath;
            }
            break;

        case AJ_HDR_INTERFACE:
            hdrVal.val.v_string = msg->iface;
            break;

        case AJ_HDR_MEMBER:
            if (msgType != AJ_MSG_ERROR) {
                int32_t len = AJ_StringFindFirstOf(msg->member, " ");
                hdrVal.val.v_string = msg->member;
                hdrVal.len = (len >= 0) ? len : 0;
            }
            break;

        case AJ_HDR_ERROR_NAME:
            if (msgType == AJ_MSG_ERROR) {
                hdrVal.val.v_string = msg->error;
            }
            break;

        case AJ_HDR_REPLY_SERIAL:
            if ((msgType == AJ_MSG_METHOD_RET) || (msgType == AJ_MSG_ERROR)) {
                hdrVal.val.v_uint32 = &msg->replySerial;
            }
            break;

        case AJ_HDR_DESTINATION:
            hdrVal.val.v_string = msg->destination;
            break;

        case AJ_HDR_SENDER:
            hdrVal.val.v_string = AJ_GetUniqueName(msg->bus);
            break;

        case AJ_HDR_SIGNATURE:
            hdrVal.val.v_signature = msg->signature;
            break;

        case AJ_HDR_TIMESTAMP:
            if (msg->ttl) {
                AJ_Time timer;
                timer.seconds = 0;
                timer.milliseconds = 0;
                msg->timestamp = AJ_GetElapsedTime(&timer, FALSE);
                hdrVal.val.v_uint32 = &msg->timestamp;
            }
            break;

        case AJ_HDR_TIME_TO_LIVE:
            if (msg->ttl) {
                hdrVal.val.v_uint32 = &msg->ttl;
            }
            break;

        case AJ_HDR_SESSION_ID:
            if (msg->sessionId) {
                hdrVal.val.v_uint32 = &msg->sessionId;
            }
            break;

        case AJ_HDR_HANDLES:
        case AJ_HDR_COMPRESSION_TOKEN:
        default:
            continue;
        }
        /*
         * Ignore empty fields.
         */
        if (!hdrVal.val.v_data) {
            continue;
        }
        /*
         * Custom marshal the header field - signature is "(yv)" so starts off with STRUCT aligment.
         */
        buf[0] = fieldId;
        buf[1] = 1;
        buf[2] = typeId;
        buf[3] = 0;
        WriteBytes(msg, buf, 4, PadForType(AJ_ARG_STRUCT, ioBuf));
        /*
         * Now marshal the field value
         */
        Marshal(msg, &fieldSig, &hdrVal);
    }
    if (status == AJ_OK) {
        /*
         * Write the header length
         */
        msg->hdr->headerLen = (uint32_t)((ioBuf->writePtr - ioBuf->bufStart) - sizeof(AJ_MsgHeader));
        /*
         * Header must be padded to an 8 byte boundary
         */
        status = WritePad(msg, (8 - msg->hdr->headerLen) & 7);
    }
    return status;
}

AJ_Status AJ_MarshalArg(AJ_Message* msg, AJ_Arg* arg)
{
    AJ_Status status;
    AJ_IOBuffer* ioBuf = &msg->bus->sock.tx;
    uint8_t* argStart = ioBuf->writePtr;

    if (msg->varOffset) {
        /*
         * Marshaling a variant - get the signature from the I/O buffer
         */
        const char* sig = (const char*)(argStart - msg->varOffset);
        msg->varOffset = 0;
        status = Marshal(msg, &sig, arg);
    } else if (msg->outer) {
        /*
         * Marshaling a component of a container use the container's signature
         */
        const char* sig = msg->outer->sigPtr;
        if (!*sig) {
            return AJ_ERR_END_OF_DATA;
        }
        status = Marshal(msg, &sig, arg);
        /*
         * Only advance the signature for struct elements
         */
        if (msg->outer->typeId != AJ_ARG_ARRAY) {
            msg->outer->sigPtr = sig;
        }
    } else {
        const char* sig = msg->signature + msg->sigOffset;
        /*
         * Marshalling anything else use the message signature
         */
        if (!*sig) {
            return AJ_ERR_END_OF_DATA;
        }
        status = Marshal(msg, &sig, arg);
        msg->sigOffset = (uint8_t)(sig - msg->signature);
    }
    if (status == AJ_OK) {
        msg->bodyBytes += (uint16_t)(ioBuf->writePtr - argStart);
    } else {
        AJ_ReleaseReplyContext(msg);
    }
    return status;
}

AJ_Arg* AJ_InitArg(AJ_Arg* arg, uint8_t typeId, uint8_t flags, const void* val, size_t len)
{
    if (!IsBasicType(typeId)) {
        memset(arg, 0, sizeof(AJ_Arg));
        return NULL;
    } else {
        arg->typeId = typeId;
        arg->flags = flags;
        arg->len = (uint16_t)len;
        arg->val.v_data = (void*)val;
        arg->sigPtr = NULL;
        arg->container = NULL;
        return arg;
    }
}

AJ_Status AJ_MarshalArgs(AJ_Message* msg, const char* sig, ...)
{
    AJ_Status status = AJ_OK;
    AJ_Arg arg;
    va_list argp;

    va_start(argp, sig);
    while (*sig) {
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        uint8_t typeId = (uint8_t)*sig++;
        void* val;
        if (!IsBasicType(typeId)) {
            status = AJ_ERR_UNEXPECTED;
            break;
        }
        if (IsScalarType(typeId)) {
            if (SizeOfType(typeId) == 8) {
                u64 = va_arg(argp, uint64_t);
                val = &u64;
            } else if (SizeOfType(typeId) == 4) {
                u32 = va_arg(argp, uint32_t);
                val = &u32;
            } else if (SizeOfType(typeId) == 2) {
                u16 = (uint16_t)va_arg(argp, uint32_t);
                val = &u16;
            } else {
                u8 = (uint8_t)va_arg(argp, uint32_t);
                val = &u8;
            }
        } else {
            val = va_arg(argp, char*);
        }
        InitArg(&arg, typeId, val);
        status = AJ_MarshalArg(msg, &arg);
        if (status != AJ_OK) {
            break;
        }
    }
    va_end(argp);
    return status;
}

AJ_Status AJ_DeliverMsgPartial(AJ_Message* msg, uint32_t bytesRemaining)
{
    AJ_IOBuffer* ioBuf = &msg->bus->sock.tx;
    uint8_t typeId = msg->signature[msg->sigOffset];
    size_t pad;

    AJ_ASSERT(!msg->outer);

    if (!msg->hdr || !bytesRemaining) {
        return AJ_ERR_UNEXPECTED;
    }
    /*
     * Partial delivery not currently supported for messages that must be encrypted.
     */
    if (msg->hdr->flags & AJ_FLAG_ENCRYPTED) {
        return AJ_ERR_SECURITY;
    }
    /*
     * There must be arguments to marshal
     */
    if (!typeId) {
        return AJ_ERR_SIGNATURE;
    }
    /*
     * Pad to the start of the argument.
     */
    pad = PadForType(typeId, ioBuf);
    if (pad) {
        AJ_Status status = WritePad(msg, pad);
        if (status != AJ_OK) {
            return status;
        }
    }
    /*
     * Set the body length in the header buffer.
     */
    msg->hdr->bodyLen = (uint32_t)(msg->bodyBytes + pad + bytesRemaining);
    AJ_DumpMsg("SENDING(partial)", msg, FALSE);
    /*
     * The buffer space occupied by the header is going to be overwritten
     * so the header is going to become invalid.
     */
    msg->hdr = NULL;
    /*
     * From now on we are going to count down the remaining body bytes
     */
    msg->bodyBytes = (uint32_t)bytesRemaining;
    /*
     * Standard signature matching is now meaningless
     */
    msg->signature = "";
    msg->sigOffset = 0;;

    return AJ_OK;
}

AJ_Status AJ_MarshalRaw(AJ_Message* msg, const void* data, size_t len)
{
    if (msg->hdr) {
        return AJ_ERR_UNEXPECTED;
    }
    /*
     * It is a fatal error to write too many bytes
     */
    if (len > msg->bodyBytes) {
        return AJ_ERR_WRITE;
    }
    msg->bodyBytes -= (uint32_t)len;
    return WriteBytes(msg, data, len, 0);
}

AJ_Status AJ_MarshalContainer(AJ_Message* msg, AJ_Arg* arg, uint8_t typeId)
{
    AJ_Status status;

    InitArg(arg, typeId, NULL);
    status = AJ_MarshalArg(msg, arg);
    if (status == AJ_OK) {
        arg->container = msg->outer;
        msg->outer = arg;
    }
    return status;
}

AJ_Status AJ_MarshalCloseContainer(AJ_Message* msg, AJ_Arg* arg)
{
    AJ_IOBuffer* ioBuf = &msg->bus->sock.tx;
    AJ_Status status = AJ_OK;

    AJ_ASSERT(TYPE_FLAG(arg->typeId) & AJ_CONTAINER);
    AJ_ASSERT(msg->outer == arg);

    msg->outer = arg->container;

    if (arg->typeId == AJ_ARG_ARRAY) {
        uint32_t lenOffset = (uint32_t)((uint8_t*)arg->val.v_data - ioBuf->bufStart);
        /*
         * The length we marshal does not include the length field itself.
         */
        arg->len = (uint16_t)(ioBuf->writePtr - (uint8_t*)arg->val.v_data) - 4;
        /*
         * If the array element is 8 byte aligned and the array is not empty check if there was
         * padding after the length. The length we marshal should not include the padding.
         */

        if ((ALIGNMENT(*arg->sigPtr) == 8) && !(lenOffset & 4) && arg->len) {
            arg->len -= 4;
        }
        /*
         * Write array length into the buffer
         */
        *(arg->val.v_uint32) = arg->len;
    } else {
        arg->len = 0;
        /*
         * Check the signature is correctly closed.
         */
        if ((arg->typeId == AJ_ARG_STRUCT) && (*arg->sigPtr != AJ_STRUCT_CLOSE)) {
            return AJ_ERR_SIGNATURE;
        }
        if ((arg->typeId == AJ_ARG_DICT_ENTRY) && (*arg->sigPtr != AJ_DICT_ENTRY_CLOSE)) {
            return AJ_ERR_SIGNATURE;
        }
    }
    return status;
}

AJ_Status AJ_MarshalVariant(AJ_Message* msg, const char* sig)
{
    AJ_Arg arg;

    /*
     * A variant type must be a single complete type
     */
    if (CompleteTypeSigLen(sig) != strlen(sig)) {
        return AJ_ERR_UNEXPECTED;
    }
    InitArg(&arg, AJ_ARG_VARIANT, sig);
    return AJ_MarshalArg(msg, &arg);
}

AJ_Status AJ_MarshalMethodCall(AJ_BusAttachment* bus, AJ_Message* msg, uint32_t msgId, const char* destination, AJ_SessionId sessionId, uint8_t flags, uint32_t timeout)
{
    AJ_Status status;

    memset(msg, 0, sizeof(AJ_Message));
    msg->bus = bus;
    msg->destination = destination;
    msg->sessionId = sessionId;
    status = MarshalMsg(msg, AJ_MSG_METHOD_CALL, msgId, flags);
    if (status == AJ_OK) {
        status = AJ_AllocReplyContext(msg, timeout);
    }
    return status;
}

AJ_Status AJ_MarshalSignal(AJ_BusAttachment* bus, AJ_Message* msg, uint32_t msgId, const char* destination, AJ_SessionId sessionId, uint8_t flags, uint32_t ttl)
{
    memset(msg, 0, sizeof(AJ_Message));
    msg->bus = bus;
    msg->destination = destination;
    msg->sessionId = sessionId;
    msg->ttl = ttl;
    return MarshalMsg(msg, AJ_MSG_SIGNAL, msgId, flags);
}

AJ_Status AJ_MarshalReplyMsg(const AJ_Message* methodCall, AJ_Message* reply)
{
    AJ_ASSERT(methodCall->hdr->msgType == AJ_MSG_METHOD_CALL);
    memset(reply, 0, sizeof(AJ_Message));
    reply->bus = methodCall->bus;
    reply->destination = methodCall->sender;
    reply->sessionId = methodCall->sessionId;
    reply->replySerial = methodCall->hdr->serialNum;
    return MarshalMsg(reply, AJ_MSG_METHOD_RET, methodCall->msgId, methodCall->hdr->flags & AJ_FLAG_ENCRYPTED);
}

AJ_Status AJ_MarshalErrorMsg(const AJ_Message* methodCall, AJ_Message* reply, const char* error)
{
    AJ_ASSERT(methodCall->hdr->msgType == AJ_MSG_METHOD_CALL);
    memset(reply, 0, sizeof(AJ_Message));
    reply->bus = methodCall->bus;
    reply->destination = methodCall->sender;
    reply->sessionId = methodCall->sessionId;
    reply->replySerial = methodCall->hdr->serialNum;
    reply->error = error;
    return MarshalMsg(reply, AJ_MSG_ERROR, methodCall->msgId, methodCall->hdr->flags & AJ_FLAG_ENCRYPTED);
}


AJ_Status AJ_MarshalStatusMsg(const AJ_Message* methodCall, AJ_Message* reply, AJ_Status status)
{
    switch (status) {
    case AJ_ERR_NO_MATCH:
        status = AJ_MarshalErrorMsg(methodCall, reply, AJ_ErrServiceUnknown);
        break;

    case  AJ_ERR_SECURITY:
        status = AJ_MarshalErrorMsg(methodCall, reply, AJ_ErrSecurityViolation);
        /*
         * We get a security violation error so if we encrypt the error message the receiver
         * won't be able to decrypt it. We can fix this by clearing the header flags.
         */
        if (status == AJ_OK) {
            reply->hdr->flags = 0;
        }
        break;

    default:
        status = AJ_MarshalErrorMsg(methodCall, reply, AJ_ErrRejected);
        break;
    }
    return status;
}
