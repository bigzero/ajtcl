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

#include "Arduino.h"
#include "aj_target.h"
#include "aj_util.h"

typedef struct time_struct {

    /* The number of milliseconds in the time. */
    uint32_t milliseconds;

} TIME_STRUCT;

void AJ_Sleep(uint32_t time)
{
    delay(time);
}

uint32_t AJ_GetElapsedTime(AJ_Time* timer, uint8_t cumulative)
{
    uint32_t elapsed;
    TIME_STRUCT now;

    now.milliseconds =  millis();
    elapsed = (uint32_t)now.milliseconds - (timer->seconds * 1000 + timer->milliseconds);  // watch for wraparound
    if (!cumulative) {
        timer->seconds = (uint32_t)(now.milliseconds / 1000);
        timer->milliseconds = (uint16_t)(now.milliseconds % 1000);
    }
    return elapsed;
}

void* AJ_Malloc(size_t sz)
{
    return malloc(sz);
}

void AJ_Free(void* mem)
{
    if (mem) {
        free(mem);
    }
}

void ram_diag()
{
    printf("SRAM usage (stack, heap, static): %d, %d, %d\n",
           stack_used(),
           heap_used(),
           static_used());
}

uint8_t AJ_StartReadFromStdIn()
{
    return FALSE;
}

uint8_t AJ_StopReadFromStdIn()
{
    return FALSE;
}

char* AJ_GetCmdLine(char* buf, size_t num)
{
    if (Serial.available() > 0) {
        int countBytesRead;
        // read the incoming bytes until a newline character:
        countBytesRead = Serial.readBytesUntil('\n', buf, num);
        buf[countBytesRead] = '\0';
        return buf;
    } else {
        return NULL;
    }
}