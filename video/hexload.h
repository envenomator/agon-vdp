#ifndef HEXLOAD_H
#define HEXLOAD_H

#include <stdbool.h>
#include "CRC16.h"
#include "CRC32.h"

extern void printFmt(const char *format, ...);
extern HardwareSerial DBGSerial;

CRC16 linecrc16(0x8005, 0x0, 0x0, false, false);
CRC32 crc32,crc32tmp;

#define DEF_LOAD_ADDRESS			0x040000
#define DEF_U_BYTE  				((DEF_LOAD_ADDRESS >> 16) & 0xFF)
//#define OVERRUNTIMEOUT				(100000/(SERIALBAUDRATE/10) + 2)
#define OVERRUNTIMEOUT				5000
#define IHEX_RECORD_DATA			0
#define IHEX_RECORD_EOF				1
#define IHEX_RECORD_SEGMENT			2 //Extended Segment Address record
#define IHEX_RECORD_LINEAR			4 //Extended Linear address record,
#define IHEX_RECORD_EXTENDEDMODE	0xFF

void VDUStreamProcessor::sendKeycodeByte(uint8_t b, bool waitforack) {
	uint8_t packet[] = {b,0};
	send_packet(PACKET_KEYCODE, sizeof packet, packet);                    
	if(waitforack) readByte_b();
}

uint8_t serialRx_t(void) {
	uint32_t start = micros();
	while(DBGSerial.available() == 0) {
		if((micros() - start) > OVERRUNTIMEOUT) return 0;
	}
	return DBGSerial.read();
}

uint8_t serialRx_b(void) {
	uint32_t start = micros();
	while(DBGSerial.available() == 0);
	return DBGSerial.read();
}

void waitHexMarker(void) {
	uint8_t data = 0;
	while(data != ':') data = serialRx_b();
}

// Receive a single iHex Nibble from the external Debug serial interface
uint8_t getIHexNibble(bool addcrc, bool blocked) {
	uint8_t nibble, input;
	if(blocked) input = toupper(serialRx_b());
	else input = toupper(serialRx_t());
	if(addcrc) linecrc16.add(input);
	if((input >= '0') && input <='9') nibble = input - '0';
	else nibble = input - 'A' + 10;
	// illegal characters will be dealt with by checksum later
	return nibble;
}

// Receive a byte from the external Debug serial interface as two iHex nibbles
uint8_t getIHexByte(bool addcrc) {
	uint8_t value;
	value = getIHexNibble(addcrc, false) << 4;
	value |= getIHexNibble(addcrc, false);
	return value;  
}

//uint8_t getIHexByte_blocked(bool addcrc) {
//	uint8_t value;
//	value = getIHexNibble(addcrc, true) << 4;
//	value |= getIHexNibble(addcrc, true);
//	return value;  
//}

uint32_t getIHexUINT32(bool addcrc) {
	uint32_t value;
	value =  ((uint32_t)getIHexByte(addcrc)) << 24;
	value |= ((uint32_t)getIHexByte(addcrc)) << 16;
	value |= ((uint32_t)getIHexByte(addcrc)) << 8;
	value |= getIHexByte(addcrc);
	return value;
}

void echo_checksum(uint8_t ihexlinechecksum, uint8_t ez80checksum, bool retransmit) {
	if(retransmit) printFmt("R");
	if(ez80checksum) printFmt("*");
	if(ihexlinechecksum) {printFmt("X"); return;}
	printFmt(".");
}

void serialTx_uint16(uint16_t crc) {
	DBGSerial.write((uint8_t)(crc & 0xFF));
	DBGSerial.write((uint8_t)(((crc >> 8) & 0xFF)));
}

void writeCRC32(uint32_t crc) {
	serialTx_uint16(crc & 0xFFFF);
	serialTx_uint16((crc >> 16) & 0xFFFF);
}

void VDUStreamProcessor::vdu_sys_hexload(void) {
	uint32_t 	segment_address;
	uint32_t 	crc32target;
	uint8_t 	u,h,l,tmp;
	uint8_t 	bytecount;
	uint8_t 	recordtype,subtype;
	uint8_t 	data;
	uint8_t 	ihexlinechecksum,ez80checksum;
	bool		extendedformat;
	bool 		done,printdefaultaddress,segmentmode,no_startrecord;
	bool 		retransmit;
	bool 		rom_area;
	uint16_t 	errorcount;
	uint8_t 	prevframeid,frameid;

	printFmt("Receiving Intel HEX records - VDP:%d 8N1 - %d\r\n\r\n", SERIALBAUDRATE, OVERRUNTIMEOUT);

	u = DEF_U_BYTE;
	errorcount = 0;
	done = false;
	printdefaultaddress = true;
	segmentmode = false;
	no_startrecord = false;
	rom_area = false;

	crc32.restart();
	crc32tmp.restart();
	crc32target = 0;
	prevframeid = 0xff;
	extendedformat = false;

	while(!done) {
		retransmit = false;
		linecrc16.restart();
		waitHexMarker();
		if(extendedformat) {
			frameid = getIHexByte(false);
			if(frameid != prevframeid) {
				prevframeid = frameid;
				crc32 = crc32tmp;
			}
			else {
				retransmit = true;
				crc32tmp = crc32;
			}
			//waitHexMarker();
		}
		linecrc16.add(':');

		// Get standard frame headers
		bytecount = getIHexByte(true);  // number of bytes in this record
		h = getIHexByte(true);      	// middle byte of address
		l = getIHexByte(true);      	// lower byte of address 
		recordtype = getIHexByte(true); // record type

		ihexlinechecksum = bytecount + h + l + recordtype;  // init control checksum
		if(segmentmode) {
			u = ((segment_address + (((uint32_t)h << 8) | l)) & 0xFF0000) >> 16;
			h = ((segment_address + (((uint32_t)h << 8) | l)) & 0xFF00) >> 8;
			l = (segment_address + (((uint32_t)h << 8) | l)) & 0xFF;
		}
		ez80checksum = 1 + u + h + l + bytecount; 		// to be transmitted as a potential packet to the ez80

		switch(recordtype) {
			case IHEX_RECORD_DATA:
				if(printdefaultaddress) {
					printFmt("\r\nAddress 0x%02x0000 (default)\r\n", DEF_U_BYTE);
					printdefaultaddress = false;
					no_startrecord = true;
				}
				sendKeycodeByte(1, true);			// ez80 data-package start indicator
				sendKeycodeByte(u, true);      		// transmit full address in each package  
				sendKeycodeByte(h, true);
				sendKeycodeByte(l, true);
				sendKeycodeByte(bytecount, true);	// number of bytes to send in this package
				while(bytecount--) {
					data = getIHexByte(true);
					crc32tmp.add(data);
					sendKeycodeByte(data, false);
					ihexlinechecksum += data;			// update ihexlinechecksum
					ez80checksum += data;			// update checksum from bytes sent to the ez80
				}
				ez80checksum += readByte_b();		// get feedback from ez80 - a 2s complement to the sum of all received bytes, total 0 if no errorcount      
				ihexlinechecksum += getIHexByte(true);		// finalize checksum with actual checksum byte in record, total 0 if no errorcount
				if(ihexlinechecksum || ez80checksum) errorcount++;
				if(u >= DEF_U_BYTE) echo_checksum(ihexlinechecksum,ez80checksum,retransmit);
				else printFmt("#");
				break;
			case IHEX_RECORD_SEGMENT:
				printdefaultaddress = false;
				segmentmode = true;
				tmp = getIHexByte(true);              // segment 16-bit base address MSB
				ihexlinechecksum += tmp;
				segment_address = tmp << 8;
				tmp = getIHexByte(true);              // segment 16-bit base address LSB
				ihexlinechecksum += tmp;
				segment_address |= tmp;
				segment_address = segment_address << 4; // resulting segment base address in 20-bit space
				tmp = getIHexByte(true);
						ihexlinechecksum += tmp;		// finalize checksum with actual checksum byte in record, total 0 if no errorcount
						if(ihexlinechecksum) errorcount++;
						echo_checksum(ihexlinechecksum,0,retransmit);		// only echo local checksum errorcount, no ez80<=>ESP packets in this case

				if(no_startrecord) {
					printFmt("\r\nSegment address 0x%06X", segment_address);
					segment_address += DEF_LOAD_ADDRESS;
					printFmt(" - effective 0x%06X\r\n", segment_address);
				}
				else printFmt("\r\nAddress 0x%06X\r\n", segment_address);
						if(segment_address < DEF_LOAD_ADDRESS) {
					printFmt("ERROR: Address in ROM area\r\n", segment_address);
					rom_area = true;
				}
				break;
			case IHEX_RECORD_EOF:
				tmp = getIHexByte(true);
				sendKeycodeByte(0, true);       	// end transmission
				done = true;
				break;
			case IHEX_RECORD_LINEAR:  // only update U byte for next transmission to the ez80
				printdefaultaddress = false;
				segmentmode = false;
				tmp = getIHexByte(true);
				ihexlinechecksum += tmp;		// ignore top byte of 32bit address, only using 24bit
				u = getIHexByte(true);
				ihexlinechecksum += u;
				tmp = getIHexByte(true);
				ihexlinechecksum += tmp;		// finalize checksum with actual checksum byte in record, total 0 if no errorcount
				if(ihexlinechecksum) errorcount++;
				echo_checksum(ihexlinechecksum,0,retransmit);	// only echo local checksum errorcount, no ez80<=>ESP packets in this case
				if(u >= DEF_U_BYTE) printFmt("\r\nAddress 0x%02X0000\r\n", u);
				else {
					printFmt("\r\nERROR: Address 0x%02X0000 in ROM area\r\n", u);
					rom_area = true;
				}
				break;
			case IHEX_RECORD_EXTENDEDMODE: // record never sent in extended mode
				getIHexByte(true);
				subtype = getIHexByte(true);
				switch(subtype) {
					case 0:
						extendedformat = true;
						crc32target = getIHexUINT32(true);
						getIHexByte(true);
						printFmt("Extended mode\r\n");
						break;
					default:
						break;
				}
				break;
			default: // ignore other (non I32Hex) records
				break;
		}
		if(extendedformat) serialTx_uint16(linecrc16.calc());
	}

	crc32 = crc32tmp;
	if(extendedformat) {
		writeCRC32(crc32.calc());
		printFmt("\r\n\r\nCRC32 0x%08X\r\n", crc32.calc());
		if(crc32.calc() == crc32target) printFmt("OK\r\n");
		else printFmt("ERROR 0x%08X expected\r\n", crc32target);
	}
	else {
		printFmt("\r\nOK\r\n");
		if(errorcount) {
			printFmt("\r\n%d error(s)\r\n",errorcount);
		}
	}
	if(rom_area) printFmt("\r\nHEX data overlapping ROM area, transfer unsuccessful\r\nERROR\r\n");
	printFmt("VDP done\r\n");   
}

#endif // HEXLOAD_H