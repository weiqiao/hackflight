/*
   msp.cpp : MSP (Multiwii Serial Protocol) class implementation

   Adapted from https://github.com/multiwii/baseflight/blob/master/src/serial.c

   This file is part of Hackflight.

   Hackflight is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   Hackflight is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with Hackflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef __arm__
extern "C" {
#else
#include <stdio.h>
#endif

#include <string.h> // for memset

#include "mw.hpp"

#define MSP_REBOOT               68     // in message  reboot settings
#define MSP_RC                   105    // out message 8 rc chan and more
#define MSP_ATTITUDE             108    // out message 2 angles 1 heading
#define MSP_ALTITUDE             109    // out message altitude, variometer
#define MSP_BARO_SONAR_RAW       126    // out message
#define MSP_SET_RAW_RC           200    // in message  8 rc chan
#define MSP_SET_MOTOR            214    // in message  PropBalance function

void MSP::serialize8(uint8_t a)
{
    this->_board->serialWriteByte(a);
    portState.checksum ^= a;
}

void MSP::serialize16(int16_t a)
{
    serialize8(a & 0xFF);
    serialize8((a >> 8) & 0xFF);
}

uint8_t MSP::read8(void)
{
    return portState.inBuf[portState.indRX++] & 0xff;
}

uint16_t MSP::read16(void)
{
    uint16_t t = read8();
    t += (uint16_t)read8() << 8;
    return t;
}

uint32_t MSP::read32(void)
{
    uint32_t t = read16();
    t += (uint32_t)read16() << 16;
    return t;
}

void MSP::serialize32(uint32_t a)
{
    serialize8(a & 0xFF);
    serialize8((a >> 8) & 0xFF);
    serialize8((a >> 16) & 0xFF);
    serialize8((a >> 24) & 0xFF);
}


void MSP::headSerialResponse(uint8_t err, uint8_t s)
{
    serialize8('$');
    serialize8('M');
    serialize8(err ? '!' : '>');
    portState.checksum = 0;               // start calculating a new checksum
    serialize8(s);
    serialize8(portState.cmdMSP);
}

void MSP::headSerialReply(uint8_t s)
{
    headSerialResponse(0, s);
}

void MSP::headSerialError(uint8_t s)
{
    headSerialResponse(1, s);
}

void MSP::tailSerialReply(void)
{
    serialize8(portState.checksum);
}

void MSP::init(Board * board, IMU * imu, Position * position, Mixer * mixer, RC * rc)
{
    this->_board = board;
    this->_imu = imu;
    this->_position = position;
    this->_mixer = mixer;
    this->_rc = rc;

    memset(&this->portState, 0, sizeof(this->portState));
}

void MSP::update(bool armed)
{
    static bool pendReboot;

    // pendReboot will be set for flashing
    this->_board->checkReboot(pendReboot);

    while (this->_board->serialAvailableBytes()) {

        uint8_t c = this->_board->serialReadByte();

        if (portState.c_state == IDLE) {
            portState.c_state = (c == '$') ? HEADER_START : IDLE;
            if (portState.c_state == IDLE && !armed) {
                if (c == '#')
                    ;
                else if (c == CONFIG_REBOOT_CHARACTER) 
                    this->_board->reboot();
            }
        } else if (portState.c_state == HEADER_START) {
            portState.c_state = (c == 'M') ? HEADER_M : IDLE;
        } else if (portState.c_state == HEADER_M) {
            portState.c_state = (c == '<') ? HEADER_ARROW : IDLE;
        } else if (portState.c_state == HEADER_ARROW) {
            if (c > INBUF_SIZE) {       // now we are expecting the payload size
                portState.c_state = IDLE;
                continue;
            }
            portState.dataSize = c;
            portState.offset = 0;
            portState.checksum = 0;
            portState.indRX = 0;
            portState.checksum ^= c;
            portState.c_state = HEADER_SIZE;      // the command is to follow
        } else if (portState.c_state == HEADER_SIZE) {
            portState.cmdMSP = c;
            portState.checksum ^= c;
            portState.c_state = HEADER_CMD;
        } else if (portState.c_state == HEADER_CMD && 
                portState.offset < portState.dataSize) {
            portState.checksum ^= c;
            portState.inBuf[portState.offset++] = c;
        } else if (portState.c_state == HEADER_CMD && portState.offset >= portState.dataSize) {

            if (portState.checksum == c) {        // compare calculated and transferred checksum

                switch (portState.cmdMSP) {

                    case MSP_SET_RAW_RC:
                        for (uint8_t i = 0; i < 8; i++)
                            this->_rc->data[i] = read16();
                        headSerialReply(0);
                        break;

                    case MSP_SET_MOTOR:
                        for (uint8_t i = 0; i < 4; i++)
                            this->_mixer->motorsDisarmed[i] = read16();
                        headSerialReply(0);
                        break;

                    case MSP_RC:
                        headSerialReply(16);
                        for (uint8_t i = 0; i < 8; i++)
                            serialize16(this->_rc->data[i]);
                        break;

                    case MSP_ATTITUDE:
                        headSerialReply(6);
                        for (uint8_t i = 0; i < 3; i++)
                            serialize16(this->_imu->angle[i]);
                        break;

                    case MSP_BARO_SONAR_RAW:
                        //headSerialReply(8);
                        //serialize32(baroPressure);
                        //serialize32(sonarDistance);
                        break;

                    case MSP_ALTITUDE:
                        headSerialReply(6);
                        serialize32(this->_position->estAlt);
                        serialize16(this->_position->vario);
                        break;

                    case MSP_REBOOT:
                        headSerialReply(0);
                        pendReboot = true;
                        break;

                    // don't know how to handle the (valid) message, indicate error MSP $M!
                    default:                   
                        headSerialError(0);
                        break;
                }
                tailSerialReply();
            }
            portState.c_state = IDLE;
        }
    }
}

#ifdef __arm__
} // extern "C"
#endif
