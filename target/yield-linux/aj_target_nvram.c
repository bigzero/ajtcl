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

#include "aj_nvram.h"
#include "aj_target_nvram.h"

uint8_t AJ_EMULATED_NVRAM[AJ_NVRAM_SIZE];
uint8_t* AJ_NVRAM_BASE_ADDRESS;

extern void AJ_NVRAM_Layout_Print();

void AJ_NVRAM_Init()
{
    AJ_NVRAM_BASE_ADDRESS = AJ_EMULATED_NVRAM;
    _AJ_LoadNVFromFile();
    if (*((uint32_t*)AJ_NVRAM_BASE_ADDRESS) != AJ_NV_SENTINEL) {
        _AJ_EraseNVRAM();
        _AJ_StoreNVToFile();
    }
}

void _AJ_NV_Write(void* dest, void* buf, uint16_t size)
{
    memcpy(dest, buf, size);
    _AJ_StoreNVToFile();
}

void _AJ_EraseNVRAM()
{
    memset((uint8_t*)AJ_NVRAM_BASE_ADDRESS, INVALID_DATA_BYTE, AJ_NVRAM_SIZE);
    *((uint32_t*)AJ_NVRAM_BASE_ADDRESS) = AJ_NV_SENTINEL;
    _AJ_StoreNVToFile();
}

AJ_Status _AJ_LoadNVFromFile()
{
    FILE* f = fopen("ajlite.nvram", "r");
    if (f == NULL) {
        AJ_Printf("Error: LoadNVFromFile() failed\n");
        return AJ_ERR_FAILURE;
    }

    memset(AJ_NVRAM_BASE_ADDRESS, INVALID_DATA_BYTE, AJ_NVRAM_SIZE);
    fread(AJ_NVRAM_BASE_ADDRESS, AJ_NVRAM_SIZE, 1, f);
    fclose(f);
    return AJ_OK;
}

AJ_Status _AJ_StoreNVToFile()
{
    FILE* f = fopen("ajlite.nvram", "w");
    if (!f) {
        AJ_Printf("Error: StoreNVToFile() failed\n");
        return AJ_ERR_FAILURE;
    }

    fwrite(AJ_NVRAM_BASE_ADDRESS, AJ_NVRAM_SIZE, 1, f);
    fclose(f);
    return AJ_OK;
}

// Compact the storage by removing invalid entries
AJ_Status _AJ_CompactNVStorage()
{
    uint16_t capacity = 0;
    uint16_t id = 0;
    uint16_t* data = (uint16_t*)(AJ_NVRAM_BASE_ADDRESS + SENTINEL_OFFSET);
    uint8_t* writePtr = (uint8_t*)data;
    uint16_t entrySize = 0;
    uint16_t garbage = 0;
    //AJ_NVRAM_Layout_Print();
    while ((uint8_t*)data < (uint8_t*)AJ_NVRAM_END_ADDRESS && *data != INVALID_DATA) {
        id = *data;
        capacity = *(data + 1);
        entrySize = ENTRY_HEADER_SIZE + capacity;
        if (id != INVALID_ID) {
            _AJ_NV_Write(writePtr, data, entrySize);
            writePtr += entrySize;
        } else {
            garbage += entrySize;
        }
        data += entrySize >> 1;
    }

    memset(writePtr, INVALID_DATA_BYTE, garbage);
    _AJ_StoreNVToFile();
    //AJ_NVRAM_Layout_Print();
    return AJ_OK;
}