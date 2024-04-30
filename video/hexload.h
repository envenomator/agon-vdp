#ifndef HEXLOAD_H
#define HEXLOAD_H

#include <stdbool.h>
#include "CRC16.h"
#include "CRC32.h"

extern void printFmt(const char *format, ...);
extern HardwareSerial DBGSerial;

CRC16 ihexcrc(0x8005, 0x0, 0x0, false, false);
CRC32 crc32;

#define DEF_LOAD_ADDRESS 0x040000
#define DEF_U_BYTE  ((DEF_LOAD_ADDRESS >> 16) & 0xFF)

void VDUStreamProcessor::sendKeycodeByte(uint8_t b, bool waitforack) {
	uint8_t packet[] = {b,0};
	send_packet(PACKET_KEYCODE, sizeof packet, packet);                    
	if(waitforack) readByte_b();
}

// Receive a single iHex Nibble from the external Debug serial interface
uint8_t getIHexNibble(void) {
	uint8_t nibble, input;

	while(!DBGSerial.available());
	input = toupper(DBGSerial.read());
	ihexcrc.add(input);
	if((input >= '0') && input <='9') nibble = input - '0';
	else nibble = input - 'A' + 10;
	// illegal characters will be dealt with by checksum later
	return nibble;
}

// Receive a byte from the external Debug serial interface as two iHex nibbles
uint8_t getIHexByte(void) {
	uint8_t value;
	value = getIHexNibble() << 4;
	value |= getIHexNibble();
	return value;  
}

void echo_checksum(uint8_t ihexchecksum, uint8_t ez80checksum, uint8_t frameid) {
	if((ihexchecksum == 0) && (ez80checksum == 0)) {
		if(frameid == '0') printFmt(".");
		else printFmt("(R)");
	}
	if(ihexchecksum) printFmt("X");
	if(ez80checksum) printFmt("*");
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
	uint8_t u,h,l,tmp;
	uint8_t bytecount;
	uint8_t recordtype;
	uint8_t data;
	uint8_t ihexchecksum,ez80checksum;
	bool	extendedformat;
	bool done,printdefaultaddress,segmentmode,no_startrecord;
	bool rom_area;
	uint16_t errorcount;
	uint8_t frameid;

	printFmt("Receiving Intel HEX records - VDP:%d 8N1\r\n\r\n", SERIALBAUDRATE);
	u = DEF_U_BYTE;
	errorcount = 0;
	done = false;
	printdefaultaddress = true;
	segmentmode = false;
	no_startrecord = false;
	rom_area = false;

	while(!done) {
		frameid = 0;
		data = 0;
		extendedformat = false;
		ihexcrc.restart();
		while(data != ':') if(DBGSerial.available() > 0) data = DBGSerial.read(); // hunt for start of recordtype
		while(!DBGSerial.available());
		if(DBGSerial.peek() == ':') {
			extendedformat = true;
			DBGSerial.read();
			while(!DBGSerial.available());
			frameid = DBGSerial.read();
			while(!DBGSerial.available());
			DBGSerial.read();
		}
		ihexcrc.add(':');

		bytecount = getIHexByte();  // number of bytes in this record
		h = getIHexByte();      	// middle byte of address
		l = getIHexByte();      	// lower byte of address 
		recordtype = getIHexByte(); // record type

		ihexchecksum = bytecount + h + l + recordtype;  // init control checksum
		if(segmentmode) {
			//seg_effective_address += (((uint32_t)h << 8) | l);
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
					data = getIHexByte();
					crc32.add(data);
					sendKeycodeByte(data, false);
					ihexchecksum += data;			// update ihexchecksum
					ez80checksum += data;			// update checksum from bytes sent to the ez80
				}
				ez80checksum += readByte_b();		// get feedback from ez80 - a 2s complement to the sum of all received bytes, total 0 if no errorcount      
				tmp = getIHexByte();
				ihexchecksum += tmp;		// finalize checksum with actual checksum byte in record, total 0 if no errorcount
				if(ihexchecksum || ez80checksum) errorcount++;
//				if(u >= DEF_U_BYTE) echo_checksum(ihexchecksum,ez80checksum);
				if(u >= DEF_U_BYTE) echo_checksum(ihexchecksum,ez80checksum,frameid);
				else printFmt("R");
				if(extendedformat) writeCRC16(ihexcrc.calc());
				break;
			case 2: // Extended Segment Address record
				printdefaultaddress = false;
				segmentmode = true;
				tmp = getIHexByte();              // segment 16-bit base address MSB
				ihexchecksum += tmp;
				segment_address = tmp << 8;
				tmp = getIHexByte();              // segment 16-bit base address LSB
				ihexchecksum += tmp;
				segment_address |= tmp;
				segment_address = segment_address << 4; // resulting segment base address in 20-bit space
				tmp = getIHexByte();
						ihexchecksum += tmp;		// finalize checksum with actual checksum byte in record, total 0 if no errorcount
						if(ihexchecksum) errorcount++;
//						echo_checksum(ihexchecksum,0);		// only echo local checksum errorcount, no ez80<=>ESP packets in this case
						echo_checksum(ihexchecksum,0,frameid);		// only echo local checksum errorcount, no ez80<=>ESP packets in this case

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
				tmp = getIHexByte();
				sendKeycodeByte(0, true);       	// end transmission
				done = true;
				if(extendedformat) writeCRC16(ihexcrc.calc());
				break;
			case 4: // extended linear address record, only update U byte for next transmission to the ez80
				printdefaultaddress = false;
				segmentmode = false;
				tmp = getIHexByte();
				ihexchecksum += tmp;		// ignore top byte of 32bit address, only using 24bit
				u = getIHexByte();
				ihexchecksum += u;
				tmp = getIHexByte();
				ihexchecksum += tmp;		// finalize checksum with actual checksum byte in record, total 0 if no errorcount
				if(ihexchecksum) errorcount++;
//				echo_checksum(ihexchecksum,0);		// only echo local checksum errorcount, no ez80<=>ESP packets in this case
				echo_checksum(ihexchecksum,0,frameid);		// only echo local checksum errorcount, no ez80<=>ESP packets in this case
				if(u >= DEF_U_BYTE) printFmt("\r\nAddress 0x%02X0000\r\n", u);
				else {
					printFmt("\r\nERROR: Address 0x%02X0000 in ROM area\r\n", u);
					rom_area = true;
				}
				if(extendedformat) writeCRC16(ihexcrc.calc());
				break;
			default: // ignore other (non I32Hex) records
				if(extendedformat) writeCRC16(ihexcrc.calc());
				break;
		}
	}

	//uint32_t crc32 = 0x3bffd717;
	if(extendedformat) {
		writeCRC32(crc32.calc());
		uint8_t data = 0;
		while(data != 'R') if(DBGSerial.available() > 0) data = DBGSerial.read();
		while(!DBGSerial.available());
		uint8_t result = DBGSerial.read();
		printFmt("\r\n\r\nCRC32 0x%08X - ", crc32.calc());
		if(result == '1') printFmt("OK\r\n");
		else printFmt("ERROR\r\n");
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