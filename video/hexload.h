#ifndef HEXLOAD_H
#define HEXLOAD_H

#include <stdbool.h>
#include "CRC16.h"
#include "CRC32.h"

extern void printFmt(const char *format, ...);
extern HardwareSerial DBGSerial;

CRC16 ihexcrc(0x8005, 0x0, 0x0, false, false);
CRC32 crc32,crc32tmp;

#define DEF_LOAD_ADDRESS 0x040000
#define DEF_U_BYTE  ((DEF_LOAD_ADDRESS >> 16) & 0xFF)

void VDUStreamProcessor::sendKeycodeByte(uint8_t b, bool waitforack) {
	uint8_t packet[] = {b,0};
	send_packet(PACKET_KEYCODE, sizeof packet, packet);                    
	if(waitforack) readByte_b();
}

// Receive a single iHex Nibble from the external Debug serial interface
uint8_t getIHexNibble(bool addcrc) {
	uint8_t nibble, input;

	while(!DBGSerial.available());
	input = toupper(DBGSerial.read());
	if(addcrc) ihexcrc.add(input);
	if((input >= '0') && input <='9') nibble = input - '0';
	else nibble = input - 'A' + 10;
	// illegal characters will be dealt with by checksum later
	return nibble;
}

// Receive a byte from the external Debug serial interface as two iHex nibbles
uint8_t getIHexByte(bool addcrc) {
	uint8_t value;
	value = getIHexNibble(addcrc) << 4;
	value |= getIHexNibble(addcrc);
	return value;  
}

uint32_t getIHexUINT32(bool addcrc) {
	uint32_t value;
	value =  ((uint32_t)getIHexByte(addcrc)) << 24;
	value |= ((uint32_t)getIHexByte(addcrc)) << 16;
	value |= ((uint32_t)getIHexByte(addcrc)) << 8;
	value |= getIHexByte(addcrc);
	return value;
}

void echo_checksum(uint8_t ihexchecksum, uint8_t ez80checksum, bool retransmit) {

	if(ez80checksum) printFmt("*");
	if(ihexchecksum) {printFmt("X"); return;}
	if(retransmit) {printFmt("(R)"); return;}
	printFmt(".");
}

void writeCRC16(uint16_t crc) {
	DBGSerial.write((uint8_t)(crc & 0xFF));
	DBGSerial.write((uint8_t)(((crc >> 8) & 0xFF)));
}

void writeCRC32(uint32_t crc) {
	DBGSerial.write((uint8_t)(crc & 0xFF));
	DBGSerial.write((uint8_t)(((crc >> 8) & 0xFF)));
	DBGSerial.write((uint8_t)(((crc >> 16) & 0xFF)));
	DBGSerial.write((uint8_t)(((crc >> 24) & 0xFF)));
}

void VDUStreamProcessor::vdu_sys_hexload(void) {
	uint32_t segment_address;
	uint32_t crc32target;
	uint8_t u,h,l,tmp;
	uint8_t bytecount;
	uint8_t recordtype,subtype;
	uint8_t data;
	uint8_t ihexchecksum,ez80checksum;
	bool	extendedformat;
	bool done,printdefaultaddress,segmentmode,no_startrecord;
	bool retransmit;
	bool rom_area;
	uint16_t errorcount;
	uint8_t prevframeid,frameid;

	printFmt("Receiving Intel HEX records - VDP:%d 8N1\r\n\r\n", SERIALBAUDRATE);
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
		data = 0;
		retransmit = false;
		ihexcrc.restart();
		while(data != ':') if(DBGSerial.available() > 0) data = DBGSerial.read(); // hunt for start of recordtype
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
			while(!DBGSerial.available());
			DBGSerial.read();
		}
		ihexcrc.add(':');

		bytecount = getIHexByte(true);  // number of bytes in this record
		h = getIHexByte(true);      	// middle byte of address
		l = getIHexByte(true);      	// lower byte of address 
		recordtype = getIHexByte(true); // record type

		ihexchecksum = bytecount + h + l + recordtype;  // init control checksum
		if(segmentmode) {
			u = ((segment_address + (((uint32_t)h << 8) | l)) & 0xFF0000) >> 16;
			h = ((segment_address + (((uint32_t)h << 8) | l)) & 0xFF00) >> 8;
			l = (segment_address + (((uint32_t)h << 8) | l)) & 0xFF;
		}
		ez80checksum = 1 + u + h + l + bytecount; 		// to be transmitted as a potential packet to the ez80

		switch(recordtype) {
			case 0: // data record
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
					ihexchecksum += data;			// update ihexchecksum
					ez80checksum += data;			// update checksum from bytes sent to the ez80
				}
				ez80checksum += readByte_b();		// get feedback from ez80 - a 2s complement to the sum of all received bytes, total 0 if no errorcount      
				tmp = getIHexByte(true);
				ihexchecksum += tmp;		// finalize checksum with actual checksum byte in record, total 0 if no errorcount
				if(ihexchecksum || ez80checksum) errorcount++;
				if(u >= DEF_U_BYTE) echo_checksum(ihexchecksum,ez80checksum,retransmit);
				else printFmt("R");
				if(extendedformat) writeCRC16(ihexcrc.calc());
				break;
			case 2: // Extended Segment Address record
				printdefaultaddress = false;
				segmentmode = true;
				tmp = getIHexByte(true);              // segment 16-bit base address MSB
				ihexchecksum += tmp;
				segment_address = tmp << 8;
				tmp = getIHexByte(true);              // segment 16-bit base address LSB
				ihexchecksum += tmp;
				segment_address |= tmp;
				segment_address = segment_address << 4; // resulting segment base address in 20-bit space
				tmp = getIHexByte(true);
						ihexchecksum += tmp;		// finalize checksum with actual checksum byte in record, total 0 if no errorcount
						if(ihexchecksum) errorcount++;
						echo_checksum(ihexchecksum,0,retransmit);		// only echo local checksum errorcount, no ez80<=>ESP packets in this case

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
				if(extendedformat) writeCRC16(ihexcrc.calc());
				break;
			case 1: // end of file record
				tmp = getIHexByte(true);
				sendKeycodeByte(0, true);       	// end transmission
				done = true;
				if(extendedformat) writeCRC16(ihexcrc.calc());
				break;
			case 4: // extended linear address record, only update U byte for next transmission to the ez80
				printdefaultaddress = false;
				segmentmode = false;
				tmp = getIHexByte(true);
				ihexchecksum += tmp;		// ignore top byte of 32bit address, only using 24bit
				u = getIHexByte(true);
				ihexchecksum += u;
				tmp = getIHexByte(true);
				ihexchecksum += tmp;		// finalize checksum with actual checksum byte in record, total 0 if no errorcount
				if(ihexchecksum) errorcount++;
				echo_checksum(ihexchecksum,0,retransmit);		// only echo local checksum errorcount, no ez80<=>ESP packets in this case
				if(u >= DEF_U_BYTE) printFmt("\r\nAddress 0x%02X0000\r\n", u);
				else {
					printFmt("\r\nERROR: Address 0x%02X0000 in ROM area\r\n", u);
					rom_area = true;
				}
				if(extendedformat) writeCRC16(ihexcrc.calc());
				break;
			case 0xFF: // record never sent in extended mode
				getIHexByte(true);
				subtype = getIHexByte(true);
				switch(subtype) {
					case 0:
						extendedformat = true;
						crc32target = getIHexUINT32(true);
						getIHexByte(true);
						writeCRC16(ihexcrc.calc());
						break;
					default:
						break;
				}
				break;
			default: // ignore other (non I32Hex) records
				if(extendedformat) writeCRC16(ihexcrc.calc());
				break;
		}
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