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

#undef WIFI_UDP_WORKING

#include <SPI.h>
#ifdef WIFI_UDP_WORKING
#include <WiFi.h>
#else
#include <Ethernet.h>
#endif

#include <alljoyn.h>

#include "due_led.h"
#include <stdint.h>

int led = 13;

#ifdef WIFI_UDP_WORKING
char ssid[] = "yourNetwork";     // the name of your network
int status = WL_IDLE_STATUS;     // the Wifi radio's status
#endif


void DUE_led_timed(uint32_t msec)
{
    printf("DUE_led_timed\n");
    digitalWrite(led, HIGH); // turn the LED on (HIGH is the voltage level)
    delay(msec);             // wait for a second
    digitalWrite(led, LOW);  // turn the LED off by making the voltage LOW
    delay(msec);             // wait for a second
}

void DUE_led(uint8_t on)
{
    printf("DUE_led(%u)\n", on);
    digitalWrite(led, on ? HIGH : LOW); // turn the LED on (HIGH is the voltage level)
}


// the setup routine runs once when you press reset:
void setup() {
    // initialize the digital pin as an output.
    pinMode(led, OUTPUT);

    Serial.begin(115200);
    while (!Serial) ;

    digitalWrite(led, LOW);


#ifdef WIFI_UDP_WORKING
    // check for the presence of the shield:
    if (WiFi.status() == WL_NO_SHIELD) {
        printf("WiFi shield not present\n");
        // don't continue:
        while (true) ;
    }

    // attempt to connect to Wifi network:
    while (wifiStatus != WL_CONNECTED) {
        Serial.print("Attempting to connect to open SSID: ");
        Serial.println(ssid);
        status = WiFi.begin(ssid);

        // wait 10 seconds for connection:
        delay(10000);

        IPAddress ip = WiFi.localIP();
        Serial.print("Connected: ");
        Serial.println(ip);
    }
#else
    byte mac[] = { 0x00, 0xAA, 0xBB, 0xCC, 0xDE, 0x02 };
    // start the Ethernet connection:
    if (Ethernet.begin(mac) == 0) {
        printf("Failed to configure Ethernet using DHCP\n");
        // no point in carrying on, so do nothing forevermore:
        for (;;)
            ;
    }
#endif
}

// the loop routine runs over and over again forever:
void loop() {
    printf("Hello\n");
    AJ_Main();
}


