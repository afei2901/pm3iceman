//-----------------------------------------------------------------------------
// Gerhard de Koning Gans - May 2008
// Hagen Fritsch - June 2010
// Gerhard de Koning Gans - May 2011
// Gerhard de Koning Gans - June 2012 - Added iClass card and reader emulation
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Routines to support iClass.
//-----------------------------------------------------------------------------
// Based on ISO14443a implementation. Still in experimental phase.
// Contribution made during a security research at Radboud University Nijmegen
// 
// Please feel free to contribute and extend iClass support!!
//-----------------------------------------------------------------------------
//
// FIX:
// ====
// We still have sometimes a demodulation error when snooping iClass communication.
// The resulting trace of a read-block-03 command may look something like this:
//
//  +  22279:    :     0c  03  e8  01    
//
//    ...with an incorrect answer...
//
//  +     85:   0: TAG ff! ff! ff! ff! ff! ff! ff! ff! bb  33  bb  00  01! 0e! 04! bb     !crc
//
// We still left the error signalling bytes in the traces like 0xbb
//
// A correct trace should look like this:
//
// +  21112:    :     0c  03  e8  01    
// +     85:   0: TAG ff  ff  ff  ff  ff  ff  ff  ff  ea  f5    
//
//-----------------------------------------------------------------------------

#include "apps.h"
#include "cmd.h"
// Needed for CRC in emulation mode;
// same construction as in ISO 14443;
// different initial value (CRC_ICLASS)
#include "iso14443crc.h"
#include "iso15693tools.h"
#include "protocols.h"
#include "optimized_cipher.h"
#include "usb_cdc.h" // for usb_poll_validate_length

static int timeout = 4096;
static int SendIClassAnswer(uint8_t *resp, int respLen, int delay);

#define MODE_SIM_CSN        0
#define MODE_EXIT_AFTER_MAC 1
#define MODE_FULLSIM        2

#ifndef ICLASS_DMA_BUFFER_SIZE
# define ICLASS_DMA_BUFFER_SIZE 256
#endif

// The length of a received command will in most cases be no more than 18 bytes.
// 32 should be enough!
#ifndef ICLASS_BUFFER_SIZE 
	#define ICLASS_BUFFER_SIZE 32
#endif

int doIClassSimulation(int simulationMode, uint8_t *reader_mac_buf);

//-----------------------------------------------------------------------------
// The software UART that receives commands from the reader, and its state
// variables.
//-----------------------------------------------------------------------------
typedef struct {
    enum {
        STATE_UNSYNCD,
        STATE_START_OF_COMMUNICATION,
		STATE_RECEIVING
    }       state;
    uint16_t    shiftReg;
    int     bitCnt;
    int     byteCnt;
//    int     byteCntMax;
    int     posCnt;
    int     nOutOfCnt;
    int     OutOfCnt;
    int     syncBit;
    int     samples;
    int     highCnt;
    int     swapper;
    int     counter;
    int     bitBuffer;
    int     dropPosition;
    uint8_t *output;
} tUart;

typedef struct {
    enum {
        DEMOD_UNSYNCD,
		DEMOD_START_OF_COMMUNICATION,
		DEMOD_START_OF_COMMUNICATION2,
		DEMOD_START_OF_COMMUNICATION3,
		DEMOD_SOF_COMPLETE,
		DEMOD_MANCHESTER_D,
		DEMOD_MANCHESTER_E,
		DEMOD_END_OF_COMMUNICATION,
		DEMOD_END_OF_COMMUNICATION2,
		DEMOD_MANCHESTER_F,
        DEMOD_ERROR_WAIT
    }       state;
    int     bitCount;
    int     posCount;
	int     syncBit;
    uint16_t    shiftReg;
	int     buffer;
	int     buffer2;
	int		buffer3;
	int     buff;
	int     samples;
    int     len;
	enum {
		SUB_NONE,
		SUB_FIRST_HALF,
		SUB_SECOND_HALF,
		SUB_BOTH
	}		sub;
    uint8_t   *output;
} tDemod;

static tUart Uart;
static void UartReset(){
	Uart.state = STATE_UNSYNCD;
	Uart.shiftReg = 0;
    Uart.bitCnt = 0;
    Uart.byteCnt = 0;
    Uart.posCnt = 0;
    Uart.nOutOfCnt = 0;
    Uart.OutOfCnt = 0;
    Uart.syncBit = 0;
    Uart.samples = 0;
    Uart.highCnt = 0;
    Uart.swapper = 0;
    Uart.counter = 0;
    Uart.bitBuffer = 0;
    Uart.dropPosition = 0;
}
static void UartInit(uint8_t *data){
	Uart.output = data;
	UartReset();
}


/*
* READER TO CARD
*  1 out of 4 Decoding
*  1 out of 256 Decoding
*/
static RAMFUNC int OutOfNDecoding(int bit) {
	//int error = 0;
	int bitright;

	if (!Uart.bitBuffer) {
		Uart.bitBuffer = bit ^ 0xFF0;
		return false;
	} else {
		Uart.bitBuffer <<= 4;
		Uart.bitBuffer ^= bit;
	}
	
	/*if (Uart.swapper) {
		Uart.output[Uart.byteCnt] = Uart.bitBuffer & 0xFF;
		Uart.byteCnt++;
		Uart.swapper = 0;
		if (Uart.byteCnt > 15) return true;
	}
	else {
		Uart.swapper = 1;
	}*/

	if (Uart.state != STATE_UNSYNCD) {
		Uart.posCnt++;

		if ((Uart.bitBuffer & Uart.syncBit) ^ Uart.syncBit)
			bit = 0;
		else
			bit = 1;
		
		if (((Uart.bitBuffer << 1) & Uart.syncBit) ^ Uart.syncBit)
			bitright = 0;
		else
			bitright = 1;
		
		if(bit != bitright) 
			bit = bitright;

		
		// So, now we only have to deal with *bit*, lets see...
		if (Uart.posCnt == 1) {
			// measurement first half bitperiod
			if (!bit) {
				// Drop in first half means that we are either seeing
				// an SOF or an EOF.

				if (Uart.nOutOfCnt == 1) {
					// End of Communication
					Uart.state = STATE_UNSYNCD;
					Uart.highCnt = 0;
					if (Uart.byteCnt == 0) {
						// Its not straightforward to show single EOFs
						// So just leave it and do not return TRUE
						Uart.output[0] = 0xf0;
						Uart.byteCnt++;
					} else {
						return true;
					}
				} else if (Uart.state != STATE_START_OF_COMMUNICATION) {
					// When not part of SOF or EOF, it is an error
					Uart.state = STATE_UNSYNCD;
					Uart.highCnt = 0;
					//error = 4;
				}
			}
		} else {
			// measurement second half bitperiod
			// Count the bitslot we are in... (ISO 15693)
			Uart.nOutOfCnt++;
			
			if (!bit) {
				if (Uart.dropPosition) {
					if (Uart.state == STATE_START_OF_COMMUNICATION) {
						//error = 1;
					} else {
						//error = 7;
					}
					// It is an error if we already have seen a drop in current frame
					Uart.state = STATE_UNSYNCD;
					Uart.highCnt = 0;
				} else {
					Uart.dropPosition = Uart.nOutOfCnt;
				}
			}
			Uart.posCnt = 0;
			
			if (Uart.nOutOfCnt == Uart.OutOfCnt && Uart.OutOfCnt == 4) {
				Uart.nOutOfCnt = 0;
				
				if (Uart.state == STATE_START_OF_COMMUNICATION) {
					if (Uart.dropPosition == 4) {
						Uart.state = STATE_RECEIVING;
						Uart.OutOfCnt = 256;
					} else if (Uart.dropPosition == 3) {
						Uart.state = STATE_RECEIVING;
						Uart.OutOfCnt = 4;
						//Uart.output[Uart.byteCnt] = 0xdd;
						//Uart.byteCnt++;
					} else {
						Uart.state = STATE_UNSYNCD;
						Uart.highCnt = 0;
					}
					Uart.dropPosition = 0;
				} else {
					// RECEIVING DATA
					// 1 out of 4
					if (!Uart.dropPosition) {
						Uart.state = STATE_UNSYNCD;
						Uart.highCnt = 0;
						//error = 9;
					} else {
						Uart.shiftReg >>= 2;
						
						// Swap bit order
						Uart.dropPosition--;
						//if(Uart.dropPosition == 1) { Uart.dropPosition = 2; }
						//else if(Uart.dropPosition == 2) { Uart.dropPosition = 1; }
						
						Uart.shiftReg ^= ((Uart.dropPosition & 0x03) << 6);
						Uart.bitCnt += 2;
						Uart.dropPosition = 0;

						if (Uart.bitCnt == 8) {
							Uart.output[Uart.byteCnt] = (Uart.shiftReg & 0xff);
							Uart.byteCnt++;
							Uart.bitCnt = 0;
							Uart.shiftReg = 0;
						}
					}
				}
			} else if (Uart.nOutOfCnt == Uart.OutOfCnt) {
				// RECEIVING DATA
				// 1 out of 256
				if (!Uart.dropPosition) {
					Uart.state = STATE_UNSYNCD;
					Uart.highCnt = 0;
					//error = 3;
				} else {
					Uart.dropPosition--;
					Uart.output[Uart.byteCnt] = (Uart.dropPosition & 0xff);
					Uart.byteCnt++;
					Uart.bitCnt = 0;
					Uart.shiftReg = 0;
					Uart.nOutOfCnt = 0;
					Uart.dropPosition = 0;
				}
			}

			/*if (error) {
				Uart.output[Uart.byteCnt] = 0xAA;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = error & 0xFF;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = 0xAA;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = (Uart.bitBuffer >> 8) & 0xFF;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = Uart.bitBuffer & 0xFF;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = (Uart.syncBit >> 3) & 0xFF;
				Uart.byteCnt++;
				Uart.output[Uart.byteCnt] = 0xAA;
				Uart.byteCnt++;
				return true;
			}*/
		}
	} else {
		bit = Uart.bitBuffer & 0xf0;
		bit >>= 4;
		bit ^= 0x0F; // drops become 1s ;-)
		if (bit) {
			// should have been high or at least (4 * 128) / fc
			// according to ISO this should be at least (9 * 128 + 20) / fc
			if (Uart.highCnt == 8) {
				// we went low, so this could be start of communication
				// it turns out to be safer to choose a less significant
				// syncbit... so we check whether the neighbour also represents the drop
				Uart.posCnt = 1;   // apparently we are busy with our first half bit period
				Uart.syncBit = bit & 8;
				Uart.samples = 3;
				
				if (!Uart.syncBit)	{ Uart.syncBit = bit & 4; Uart.samples = 2; }
				else if (bit & 4)	{ Uart.syncBit = bit & 4; Uart.samples = 2; bit <<= 2; }
				
				if (!Uart.syncBit)	{ Uart.syncBit = bit & 2; Uart.samples = 1; }
				else if (bit & 2)	{ Uart.syncBit = bit & 2; Uart.samples = 1; bit <<= 1; }
				
				if (!Uart.syncBit)	{ Uart.syncBit = bit & 1; Uart.samples = 0;
					if (Uart.syncBit && (Uart.bitBuffer & 8)) {
						Uart.syncBit = 8;

						// the first half bit period is expected in next sample
						Uart.posCnt = 0;
						Uart.samples = 3;
					}
				} else if (bit & 1)	{ Uart.syncBit = bit & 1; Uart.samples = 0; }

				Uart.syncBit <<= 4;
				Uart.state = STATE_START_OF_COMMUNICATION;
				Uart.bitCnt = 0;
				Uart.byteCnt = 0;
				Uart.nOutOfCnt = 0;
				Uart.OutOfCnt = 4; // Start at 1/4, could switch to 1/256
				Uart.dropPosition = 0;
				Uart.shiftReg = 0;
				//error = 0;
			} else {
				Uart.highCnt = 0;
			}
		} else {
			if (Uart.highCnt < 8)
				Uart.highCnt++;
		}
	}
    return false;
}

//=============================================================================
// Manchester
//=============================================================================
static tDemod Demod;
static void DemodReset() {
	Demod.bitCount = 0;
	Demod.posCount = 0;
	Demod.syncBit = 0;
	Demod.shiftReg = 0;
	Demod.buffer = 0;
	Demod.buffer2 = 0;
	Demod.buffer3 = 0;
	Demod.buff = 0;
	Demod.samples = 0;
	Demod.len = 0;
	Demod.sub = SUB_NONE;
	Demod.state = DEMOD_UNSYNCD;
}
static void DemodInit(uint8_t *data) {
	Demod.output = data;
	DemodReset();
}

// UART debug 
// it adds the debug values which will be put in the tracelog,
// visible on client when running  'hf list iclass'
/*
pm3 --> hf li iclass
Recorded Activity (TraceLen = 162 bytes)
      Start |        End | Src | Data (! denotes parity error)                                   | CRC | Annotation         |
------------|------------|-----|-----------------------------------------------------------------|-----|--------------------|
          0 |          0 | Rdr |0a                                                               |     | ACTALL
       1280 |       1280 | Tag |bb! 33! bb! 01  02  04  08  bb!                                  |  ok |
       1280 |       1280 | Rdr |0c                                                               |     | IDENTIFY
       1616 |       1616 | Tag |bb! 33! bb! 00! 02  00! 02  bb!                                  |  ok |
       1616 |       1616 | Rdr |0a                                                               |     | ACTALL
       2336 |       2336 | Tag |bb! d4! bb! 02  08  00! 08  bb!                                  |  ok |
       2336 |       2336 | Rdr |0c                                                               |     | IDENTIFY
       2448 |       2448 | Tag |bb! 33! bb! 00! 00! 00! 02  bb!                                  |  ok |
       2448 |       2448 | Rdr |0a                                                               |     | ACTALL
       2720 |       2720 | Tag |bb! d4! bb! 08  0b  01  04  bb!                                  |  ok |
       2720 |       2720 | Rdr |0c                                                               |     | IDENTIFY
       3232 |       3232 | Tag |bb! d4! bb! 02  02  08  04  bb!                                  |  ok |
*/
static void uart_debug(int error, int bit) {
	Demod.output[Demod.len] = 0xBB;
	Demod.len++;
	Demod.output[Demod.len] = error & 0xFF;
	Demod.len++;
	Demod.output[Demod.len] = 0xBB;
	Demod.len++;
	Demod.output[Demod.len] = bit & 0xFF;
	Demod.len++;
	Demod.output[Demod.len] = Demod.buffer & 0xFF;
	Demod.len++;
	// Look harder ;-)
	Demod.output[Demod.len] = Demod.buffer2 & 0xFF;
	Demod.len++;
	Demod.output[Demod.len] = Demod.syncBit & 0xFF;
	Demod.len++;
	Demod.output[Demod.len] = 0xBB;
	Demod.len++;
}

/*
* CARD TO READER
* in ISO15693-2 mode -  Manchester 
* in ISO 14443b - BPSK coding
*
* Timings:
*  ISO 15693-2 
*           Tout = 330 µs, Tprog 1 = 4 to 15 ms, Tslot = 330 µs + (number of slots x 160 µs)
*  ISO 14443a
*			Tout = 100 µs, Tprog = 4 to 15 ms, Tslot = 100 µs+ (number of slots x 80 µs)
*  ISO 14443b
			 Tout = 76 µs, Tprog = 4 to 15 ms, Tslot = 119 µs+ (number of slots x 150 µs)
*
*
*  So for current implementation in ISO15693, its 330 µs from end of reader, to start of card.
*/
static RAMFUNC int ManchesterDecoding(int v) {
	int bit;
	int modulation;
	int error = 0;

	bit = Demod.buffer;
	Demod.buffer = Demod.buffer2;
	Demod.buffer2 = Demod.buffer3;
	Demod.buffer3 = v;

	// too few bits?
	if (Demod.buff < 3) {
		Demod.buff++;
		return false;
	}

	if (Demod.state == DEMOD_UNSYNCD) {
		Demod.output[Demod.len] = 0xfa;
		Demod.syncBit = 0;
		//Demod.samples = 0;
		Demod.posCount = 1;		// This is the first half bit period, so after syncing handle the second part

		if (bit & 0x08)
			Demod.syncBit = 0x08;

		if (bit & 0x04) {
			if (Demod.syncBit)
				bit <<= 4;

			Demod.syncBit = 0x04;
		}

		if (bit & 0x02) {
			if (Demod.syncBit)
				bit <<= 2;

			Demod.syncBit = 0x02;
		}

		if (bit & 0x01 && Demod.syncBit)
			Demod.syncBit = 0x01;

		if (Demod.syncBit) {
			Demod.len = 0;
			Demod.state = DEMOD_START_OF_COMMUNICATION;
			Demod.sub = SUB_FIRST_HALF;
			Demod.bitCount = 0;
			Demod.shiftReg = 0;
			Demod.samples = 0;

			if (Demod.posCount) {

				switch (Demod.syncBit) {
					case 0x08: Demod.samples = 3; break;
					case 0x04: Demod.samples = 2; break;
					case 0x02: Demod.samples = 1; break;
					case 0x01: Demod.samples = 0; break;
				}
				// SOF must be long burst... otherwise stay unsynced!!!
				if (!(Demod.buffer & Demod.syncBit) || !(Demod.buffer2 & Demod.syncBit))
					Demod.state = DEMOD_UNSYNCD;

			} else {
				// SOF must be long burst... otherwise stay unsynced!!!
				if (!(Demod.buffer2 & Demod.syncBit) || !(Demod.buffer3 & Demod.syncBit)) {
					Demod.state = DEMOD_UNSYNCD;
					error = 0x88;
					uart_debug(error, bit);
					return false;
				}				
			}
			error = 0;
		}
		return false;
	}

	// state is DEMOD is in SYNC from here on.
	
	modulation = bit & Demod.syncBit;
	modulation |= ((bit << 1) ^ ((Demod.buffer & 0x08) >> 3)) & Demod.syncBit;
	Demod.samples += 4;

	if (Demod.posCount == 0) {
		Demod.posCount = 1;
		Demod.sub = (modulation) ? SUB_FIRST_HALF : SUB_NONE;
		return false;
	}

	Demod.posCount = 0;

	if (modulation) {
		
		if (Demod.sub == SUB_FIRST_HALF)
			Demod.sub = SUB_BOTH;
		else
			Demod.sub = SUB_SECOND_HALF;
	}
	
	if (Demod.sub == SUB_NONE) {
		if (Demod.state == DEMOD_SOF_COMPLETE) {
			Demod.output[Demod.len] = 0x0f;
			Demod.len++;
			Demod.state = DEMOD_UNSYNCD;
			return true;
		} else {
			Demod.state = DEMOD_ERROR_WAIT;
			error = 0x33;
		}
	}

	switch (Demod.state) {
		
		case DEMOD_START_OF_COMMUNICATION:
			if (Demod.sub == SUB_BOTH) {

				Demod.state = DEMOD_START_OF_COMMUNICATION2;
				Demod.posCount = 1;
				Demod.sub = SUB_NONE;
			} else {
				Demod.output[Demod.len] = 0xab;
				Demod.state = DEMOD_ERROR_WAIT;
				error = 0xd2;
			}
			break;
			
		case DEMOD_START_OF_COMMUNICATION2:
			if (Demod.sub == SUB_SECOND_HALF) {
				Demod.state = DEMOD_START_OF_COMMUNICATION3;
			} else {
				Demod.output[Demod.len] = 0xab;
				Demod.state = DEMOD_ERROR_WAIT;
				error = 0xd3;
			}
			break;
			
		case DEMOD_START_OF_COMMUNICATION3:
			if (Demod.sub == SUB_SECOND_HALF) {
				Demod.state = DEMOD_SOF_COMPLETE;
			} else {
				Demod.output[Demod.len] = 0xab;
				Demod.state = DEMOD_ERROR_WAIT;
				error = 0xd4;
			}
			break;
			
		case DEMOD_SOF_COMPLETE:
		case DEMOD_MANCHESTER_D:
		case DEMOD_MANCHESTER_E:
			// OPPOSITE FROM ISO14443 - 11110000 = 0 (1 in 14443)
			//                          00001111 = 1 (0 in 14443)
			if (Demod.sub == SUB_SECOND_HALF) { // SUB_FIRST_HALF
				Demod.bitCount++;
				Demod.shiftReg = (Demod.shiftReg >> 1) ^ 0x100;
				Demod.state = DEMOD_MANCHESTER_D;
			} else if (Demod.sub == SUB_FIRST_HALF) { // SUB_SECOND_HALF
				Demod.bitCount++;
				Demod.shiftReg >>= 1;
				Demod.state = DEMOD_MANCHESTER_E;
			} else if (Demod.sub == SUB_BOTH) {
				Demod.state = DEMOD_MANCHESTER_F;
			} else {
				Demod.state = DEMOD_ERROR_WAIT;
				error = 0x55;
			}
			break;

		case DEMOD_MANCHESTER_F:
			// Tag response does not need to be a complete byte!
			if (Demod.len > 0 || Demod.bitCount > 0) {
				if (Demod.bitCount > 1) {  // was > 0, do not interpret last closing bit, is part of EOF
					Demod.shiftReg >>= (9 - Demod.bitCount);	// right align data
					Demod.output[Demod.len] = Demod.shiftReg & 0xff;
					Demod.len++;
				}

				Demod.state = DEMOD_UNSYNCD;
				return true;
			} else {
				Demod.output[Demod.len] = 0xad;
				Demod.state = DEMOD_ERROR_WAIT;
				error = 0x03;
			}
			break;

		case DEMOD_ERROR_WAIT:
			Demod.state = DEMOD_UNSYNCD;
			break;

		default:
			Demod.output[Demod.len] = 0xdd;
			Demod.state = DEMOD_UNSYNCD;
			break;
	}

	if (Demod.bitCount >= 8) {
		Demod.shiftReg >>= 1;
		Demod.output[Demod.len] = (Demod.shiftReg & 0xff);
		Demod.len++;
		Demod.bitCount = 0;
		Demod.shiftReg = 0;
	}

	if (error) {
		uart_debug(error, bit);
		return true;
	}
	
    return false;
}

//=============================================================================
// Finally, a `sniffer' for iClass communication
// Both sides of communication!
//=============================================================================

static void iclass_setup_sniff(void){
	if (MF_DBGLEVEL > 3) Dbprintf("iclass_setup_sniff Enter");

	LEDsoff();

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	
	FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

	// connect Demodulated Signal to ADC:
	SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

	// Set up the synchronous serial port
	FpgaSetupSsc();

	BigBuf_free(); BigBuf_Clear_ext(false); 
	clear_trace();
	set_tracing(true);

	// Initialize Demod and Uart structs
	DemodInit(BigBuf_malloc(ICLASS_BUFFER_SIZE));
	UartInit(BigBuf_malloc(ICLASS_BUFFER_SIZE));

	if (MF_DBGLEVEL > 1) {
		// Print debug information about the buffer sizes
		Dbprintf("Snooping buffers initialized:");
		Dbprintf("  Trace: %i bytes", BigBuf_max_traceLen());
		Dbprintf("  Reader -> tag: %i bytes", ICLASS_BUFFER_SIZE);
		Dbprintf("  tag -> Reader: %i bytes", ICLASS_BUFFER_SIZE);
		Dbprintf("  DMA: %i bytes", ICLASS_DMA_BUFFER_SIZE);
	}

	// Set FPGA in the appropriate mode
    // put the FPGA in the appropriate mode
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_SNIFFER);
	SpinDelay(200);
 
	// Start the SSP timer
	StartCountSspClk();
	
	LED_A_ON();
	if (MF_DBGLEVEL > 3) Dbprintf("iclass_setup_sniff Exit");
}

//-----------------------------------------------------------------------------
// Record the sequence of commands sent by the reader to the tag, with
// triggering so that we start recording at the point that the tag is moved
// near the reader.
//-----------------------------------------------------------------------------
// turn off afterwards
void RAMFUNC SniffIClass(void) {

	uint8_t previous_data = 0;
	int maxDataLen = 0; //  datalen = 0; 
	uint32_t time_0 = 0, time_start = 0, time_stop  = 0;
    uint32_t sniffCounter = 0;

	bool TagIsActive = false;
	bool ReaderIsActive = false;
	
	iclass_setup_sniff();
	
    // The DMA buffer, used to stream samples from the FPGA
    uint8_t *dmaBuf = BigBuf_malloc(ICLASS_DMA_BUFFER_SIZE);
    uint8_t *data = dmaBuf;

	// Setup and start DMA.
	if ( !FpgaSetupSscDma(dmaBuf, ICLASS_DMA_BUFFER_SIZE) ){
		if (MF_DBGLEVEL > 1) DbpString("FpgaSetupSscDma failed. Exiting"); 
		return;
	}

	// time ZERO, the point from which it all is calculated.
	time_0 = GetCountSspClk();

    // loop and listen
	while (!BUTTON_PRESS()) {
        WDT_HIT();

		previous_data = *data;
		sniffCounter++;	
		data++;

		if (data == dmaBuf + ICLASS_DMA_BUFFER_SIZE) {
			data = dmaBuf;
			AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dmaBuf;
			AT91C_BASE_PDC_SSC->PDC_RNCR = ICLASS_DMA_BUFFER_SIZE;
		}
		// number of bytes we have processed so far
		//int register readBufDataP = data - dmaBuf;
		// number of bytes already transferred		
		//int register dmaBufDataP = ICLASS_DMA_BUFFER_SIZE - AT91C_BASE_PDC_SSC->PDC_RCR;
		/*
		if (readBufDataP <= dmaBufDataP)
			datalen = dmaBufDataP - readBufDataP;
		else 
			datalen = ICLASS_DMA_BUFFER_SIZE - readBufDataP + dmaBufDataP;
		*/
		// test for length of buffer
		/*
		if (datalen > maxDataLen) {
			maxDataLen = datalen;
			if (datalen > (9 * ICLASS_DMA_BUFFER_SIZE / 10)) {
				Dbprintf("blew circular buffer! datalen=%d", datalen);
				break;
			}
		}
		*/
		// this part basically does wait until our DMA buffer got a value.
		// well it loops, but the purpose is to wait.
		//if (datalen < 1) continue;
			
		// these two, is more of a "reset" the DMA buffers,  re-init.
		// primary buffer was stopped( <-- we lost data!
		/*
		if (!AT91C_BASE_PDC_SSC->PDC_RCR) {
			AT91C_BASE_PDC_SSC->PDC_RPR = (uint32_t) dmaBuf;
			AT91C_BASE_PDC_SSC->PDC_RCR = ICLASS_DMA_BUFFER_SIZE;
//			Dbprintf("Primary buffer ERROR!!! data length: %d", datalen); // temporary
		}
		*/
		/*
		// secondary buffer sets as primary, secondary buffer was stopped
		if (!AT91C_BASE_PDC_SSC->PDC_RNCR) {
			AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dmaBuf;
			AT91C_BASE_PDC_SSC->PDC_RNCR = ICLASS_DMA_BUFFER_SIZE;
//			Dbprintf("Seconday buffer ERROR!!! data length: %d", datalen); // temporary	
		}*/

		if (sniffCounter & 0x01) {
			// no need to try decoding reader data if the tag is sending
			// READER TO CARD
			if (!TagIsActive) {
				LED_C_INV();
				// HIGH nibble is always reader data.
				uint8_t readerdata = (previous_data & 0xF0) | (*data >> 4);
				if ( OutOfNDecoding(readerdata) ) {
					time_stop = GetCountSspClk() - time_0;
					LogTrace(Uart.output, Uart.byteCnt, time_start, time_stop, NULL, true);
					DemodReset();
					UartReset();
				} else {
					time_start = GetCountSspClk() - time_0;
				}
				ReaderIsActive = (Uart.state != STATE_UNSYNCD);		
			}
		}
		if ( sniffCounter % 3) {
			// need two samples to feed Manchester
			// no need to try decoding tag data if the reader is sending - and we cannot afford the time
			// CARD TO READER
			if (!ReaderIsActive) {
				LED_C_INV();
				// LOW nibble is always tag data.
				uint8_t tagdata = (previous_data << 4) | (*data & 0x0F);
				if (ManchesterDecoding(tagdata)) {
					time_stop = GetCountSspClk() - time_0;
					LogTrace(Demod.output, Demod.len, time_start, time_stop, NULL, false);
					DemodReset();
					UartReset();
				} else {
					time_start = GetCountSspClk() - time_0;
				}
				TagIsActive = (Demod.state != DEMOD_UNSYNCD);
			}
		}
	} // end main loop

	if (MF_DBGLEVEL >= 1) {	
		DbpString("Sniff statistics:");	
		Dbprintf(" maxDataLen=%x, Uart.state=%x, Uart.byteCnt=%x", maxDataLen, Uart.state, Uart.byteCnt);
		Dbprintf(" Tracelen=%x, Uart.output[0]=%x", BigBuf_get_traceLen(), (int)Uart.output[0]);
		Dbhexdump(ICLASS_DMA_BUFFER_SIZE, data, false);
		uint8_t r[128] = {0}; 
		uint8_t t[128] = {0};
		uint16_t i;
		uint8_t j;
		for (i=0, j=0; i<ICLASS_DMA_BUFFER_SIZE; i += 2, j++) {
			r[j] = (data[i] & 0xF0)  | (data[i+1] >> 4);
			t[j] = (data[i] << 4) | (data[i+1] & 0xF);
		}
		DbpString("reader:");
		Dbhexdump(sizeof(r), r, false);
		DbpString("tag:");
		Dbhexdump(sizeof(t), t, false);
	}
	
	switch_off(); 
}

void rotateCSN(uint8_t* originalCSN, uint8_t* rotatedCSN) {
	int i; 
	for(i = 0; i < 8; i++)
		rotatedCSN[i] = (originalCSN[i] >> 3) | (originalCSN[(i+1)%8] << 5);
}

//-----------------------------------------------------------------------------
// Wait for commands from reader
// Stop when button is pressed
// Or return TRUE when command is captured
//-----------------------------------------------------------------------------
static bool GetIClassCommandFromReader(uint8_t *received, int *len, int maxLen) {	
    // Set FPGA mode to "simulated ISO 14443 tag", no modulation (listen
    // only, since we are receiving, not transmitting).
    // Signal field is off with the appropriate LED
    LED_D_OFF();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_TAGSIM_LISTEN);

	// Now run a `software UART' on the stream of incoming samples.
	UartInit(received);
	
	// clear RXRDY:
    uint8_t b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;

	while (!BUTTON_PRESS()) {
        WDT_HIT();

        //if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY))
            // AT91C_BASE_SSC->SSC_THR = 0x00;

        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
            b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;

			if (OutOfNDecoding(b & 0x0f)) {
				*len = Uart.byteCnt;
				return true;
			}
        }
    }
	return false;
}

static uint8_t encode4Bits(const uint8_t b) {
	// OTA, the least significant bits first
	// Manchester encoding added
	//         The columns are
	//               1 - Bit value to send
	//               2 - Reversed (big-endian)
	//               3 - Machester Encoded
	//               4 - Hex values

	uint8_t c = b & 0xF;
	switch (c) {
							//  1       2         3         4
	  case 15: return 0x55; // 1111 -> 1111 -> 01010101 -> 0x55
	  case 14: return 0x95; // 1110 -> 0111 -> 10010101 -> 0x95
	  case 13: return 0x65; // 1101 -> 1011 -> 01100101 -> 0x65
	  case 12: return 0xa5; // 1100 -> 0011 -> 10100101 -> 0xa5
	  case 11: return 0x59; // 1011 -> 1101 -> 01011001 -> 0x59
	  case 10: return 0x99; // 1010 -> 0101 -> 10011001 -> 0x99
	  case 9:  return 0x69; // 1001 -> 1001 -> 01101001 -> 0x69
	  case 8:  return 0xa9; // 1000 -> 0001 -> 10101001 -> 0xa9
	  case 7:  return 0x56; // 0111 -> 1110 -> 01010110 -> 0x56
	  case 6:  return 0x96; // 0110 -> 0110 -> 10010110 -> 0x96
	  case 5:  return 0x66; // 0101 -> 1010 -> 01100110 -> 0x66
	  case 4:  return 0xa6; // 0100 -> 0010 -> 10100110 -> 0xa6
	  case 3:  return 0x5a; // 0011 -> 1100 -> 01011010 -> 0x5a
	  case 2:  return 0x9a; // 0010 -> 0100 -> 10011010 -> 0x9a
	  case 1:  return 0x6a; // 0001 -> 1000 -> 01101010 -> 0x6a
	  default: return 0xaa; // 0000 -> 0000 -> 10101010 -> 0xaa
	}
}

//-----------------------------------------------------------------------------
// Prepare tag messages
//-----------------------------------------------------------------------------
static void CodeIClassTagAnswer(const uint8_t *cmd, int len) {
	/*
	 * SOF comprises 3 parts;
	 * * An unmodulated time of 56.64 us
	 * * 24 pulses of 423.75 KHz (fc/32)
	 * * A logic 1, which starts with an unmodulated time of 18.88us
	 *   followed by 8 pulses of 423.75kHz (fc/32)
	 *
	 *
	 * EOF comprises 3 parts:
	 * - A logic 0 (which starts with 8 pulses of fc/32 followed by an unmodulated
	 *   time of 18.88us.
	 * - 24 pulses of fc/32
	 * - An unmodulated time of 56.64 us
	 *
	 *
	 * A logic 0 starts with 8 pulses of fc/32
	 * followed by an unmodulated time of 256/fc (~18,88us).
	 *
	 * A logic 0 starts with unmodulated time of 256/fc (~18,88us) followed by
	 * 8 pulses of fc/32 (also 18.88us)
	 *
	 * The mode FPGA_HF_SIMULATOR_MODULATE_424K_8BIT which we use to simulate tag,
	 * works like this.
	 * - A 1-bit input to the FPGA becomes 8 pulses on 423.5kHz (fc/32) (18.88us).
	 * - A 0-bit input to the FPGA becomes an unmodulated time of 18.88us
	 *
	 * In this mode 
	 * SOF can be written as 00011101 = 0x1D
	 * EOF can be written as 10111000 = 0xb8
	 * logic 1 be written as 01 = 0x1
	 * logic 0 be written as 10 = 0x2
	 *
	 * */
	ToSendReset();

	// Send SOF
	ToSend[++ToSendMax] = 0x1D;
	
	int i;
	for(i = 0; i < len; i++) {
		uint8_t b = cmd[i];
		ToSend[++ToSendMax] = encode4Bits(b & 0xF); 		// least significant half
		ToSend[++ToSendMax] = encode4Bits((b >> 4) & 0xF);	// most significant half
	}

	// Send EOF
	ToSend[++ToSendMax] = 0xB8;
	//lastProxToAirDuration  = 8*ToSendMax - 3*8 - 3*8;//Not counting zeroes in the beginning or end
	// Convert from last byte pos to length
	ToSendMax++;
}

// Only SOF 
static void CodeIClassTagSOF() {
	//So far a dummy implementation, not used
	//int lastProxToAirDuration =0;

	ToSendReset();
	// Send SOF
	ToSend[++ToSendMax] = 0x1D;
	//	lastProxToAirDuration  = 8*ToSendMax - 3*8;//Not counting zeroes in the beginning

	// Convert from last byte pos to length
	ToSendMax++;
}

/**
 * @brief SimulateIClass simulates an iClass card.
 * @param arg0 type of simulation
 *			- 0 uses the first 8 bytes in usb data as CSN
 *			- 2 "dismantling iclass"-attack. This mode iterates through all CSN's specified
 *			in the usb data. This mode collects MAC from the reader, in order to do an offline
 *			attack on the keys. For more info, see "dismantling iclass" and proxclone.com.
 *			- Other : Uses the default CSN (031fec8af7ff12e0)
 * @param arg1 - number of CSN's contained in datain (applicable for mode 2 only)
 * @param arg2
 * @param datain
 */
// turn off afterwards
void SimulateIClass(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint8_t *datain) {

	if (MF_DBGLEVEL > 3) Dbprintf("iclass_simulate Enter");

	LEDsoff();

	FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);

	
	// this will clear out bigbuf memory ...
	FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

	FpgaSetupSsc();

	SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

	// Enable and clear the trace
	clear_trace();
	set_tracing(true);

	uint32_t simType = arg0;
	uint32_t numberOfCSNS = arg1;

	//Use the emulator memory for SIM
	uint8_t *emulator = BigBuf_get_EM_addr();
	uint8_t mac_responses[USB_CMD_DATA_SIZE] = { 0 };
	
	if (simType == 0) {
		// Use the CSN from commandline
		memcpy(emulator, datain, 8);
		doIClassSimulation(MODE_SIM_CSN, NULL);
	} else if (simType == 1) {
		//Default CSN
		uint8_t csn_crc[] = { 0x03, 0x1f, 0xec, 0x8a, 0xf7, 0xff, 0x12, 0xe0, 0x00, 0x00 };
		// Use the CSN from commandline
		memcpy(emulator, csn_crc, 8);
		doIClassSimulation(MODE_SIM_CSN, NULL);
	} else if (simType == 2) {

		Dbprintf("Going into attack mode, %d CSNS sent", numberOfCSNS);
		// In this mode, a number of csns are within datain. We'll simulate each one, one at a time
		// in order to collect MAC's from the reader. This can later be used in an offlne-attack
		// in order to obtain the keys, as in the "dismantling iclass"-paper.
		int i = 0;
		for (; i < numberOfCSNS && i*8 + 8 < USB_CMD_DATA_SIZE; i++) {
			// The usb data is 512 bytes, fitting 65 8-byte CSNs in there.

			memcpy(emulator, datain + (i*8), 8);
			
			if (doIClassSimulation(MODE_EXIT_AFTER_MAC, mac_responses+i*8)) {
				// Button pressed
				cmd_send(CMD_ACK, CMD_SIMULATE_TAG_ICLASS, i, 0, mac_responses, i*8);
				goto out;
			}
		}
		cmd_send(CMD_ACK, CMD_SIMULATE_TAG_ICLASS, i, 0, mac_responses, i*8);

	} else if (simType == 3){
		//This is 'full sim' mode, where we use the emulator storage for data.
		doIClassSimulation(MODE_FULLSIM, NULL);
	} else if (simType == 4){

		// This is the KEYROLL version of sim 2.
		// the collected data (mac_response) is doubled out since we are trying to collect both keys in the keyroll process.
		// Keyroll iceman  9 csns * 8 * 2 = 144
		// keyroll CARL55  15csns * 8 * 2 = 15 * 8 * 2 = 240		
		Dbprintf("Going into attack keyroll mode, %d CSNS sent", numberOfCSNS);
		// In this mode, a number of csns are within datain. We'll simulate each one, one at a time
		// in order to collect MAC's from the reader. This can later be used in an offlne-attack
		// in order to obtain the keys, as in the "dismantling iclass"-paper.
		
		// keyroll mode,   reader swaps between old key and new key alternatively when fail a authentication.
		// attack below is same as SIM 2, but we run the CSN twice to collected the mac for both keys.
		int i = 0;
		// The usb data is 512 bytes, fitting 65 8-byte CSNs in there.  iceman fork uses 9 CSNS
		for (; i < numberOfCSNS && i*8 + 8 < USB_CMD_DATA_SIZE; i++) {

			memcpy(emulator, datain + (i*8), 8);
			
			// keyroll 1 			
			if (doIClassSimulation(MODE_EXIT_AFTER_MAC, mac_responses + i*8 )) {
				cmd_send(CMD_ACK, CMD_SIMULATE_TAG_ICLASS, i*2, 0, mac_responses, i * 8 * 2);
				// Button pressed
				goto out; 
			}

			// keyroll 2
			if (doIClassSimulation(MODE_EXIT_AFTER_MAC, mac_responses + (i + numberOfCSNS) * 8 )) {
				cmd_send(CMD_ACK, CMD_SIMULATE_TAG_ICLASS, i*2, 0, mac_responses, i * 8 * 2);
				// Button pressed
				goto out; 
			}			
		}
		// double the amount of collected data.
		cmd_send(CMD_ACK, CMD_SIMULATE_TAG_ICLASS, i*2, 0, mac_responses, i * 8 * 2 );
	
	} else {
		// We may want a mode here where we hardcode the csns to use (from proxclone).
		// That will speed things up a little, but not required just yet.
		DbpString("The mode is not implemented, reserved for future use");
	}

out:	
	switch_off(); 
}
void AppendCrc(uint8_t* data, int len) {
	ComputeCrc14443(CRC_ICLASS, data, len, data+len, data+len+1);
}

/**
 * @brief Does the actual simulation
 * @param csn - csn to use
 * @param breakAfterMacReceived if true, returns after reader MAC has been received.
 */
int doIClassSimulation( int simulationMode, uint8_t *reader_mac_buf) {

	// free eventually allocated BigBuf memory
	BigBuf_free_keep_EM();
	
	State cipher_state;

	uint8_t *csn = BigBuf_get_EM_addr();
	uint8_t *emulator = csn;
	uint8_t sof_data[] = { 0x0F} ;
	
	// CSN followed by two CRC bytes
	uint8_t anticoll_data[10] = { 0 };
	uint8_t csn_data[10] = { 0 };
	memcpy(csn_data, csn, sizeof(csn_data));
	Dbprintf("Simulating CSN %02x%02x%02x%02x%02x%02x%02x%02x", csn[0], csn[1], csn[2], csn[3], csn[4], csn[5], csn[6], csn[7]);

	// Construct anticollision-CSN
	rotateCSN(csn_data, anticoll_data);

	// Compute CRC on both CSNs
	ComputeCrc14443(CRC_ICLASS, anticoll_data, 8, &anticoll_data[8], &anticoll_data[9]);
	ComputeCrc14443(CRC_ICLASS, csn_data, 8, &csn_data[8], &csn_data[9]);

	uint8_t diversified_key[8] = { 0 };
	// e-Purse
	uint8_t card_challenge_data[8] = { 0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
	if (simulationMode == MODE_FULLSIM) {
		//The diversified key should be stored on block 3
		//Get the diversified key from emulator memory
		memcpy(diversified_key, emulator+(8*3),8);

		//Card challenge, a.k.a e-purse is on block 2
		memcpy(card_challenge_data, emulator + (8 * 2) ,8);
		//Precalculate the cipher state, feeding it the CC
		cipher_state = opt_doTagMAC_1(card_challenge_data, diversified_key);
	}

	int exitLoop = 0;
	// Reader 0a
	// Tag    0f
	// Reader 0c
	// Tag    anticoll. CSN
	// Reader 81 anticoll. CSN
	// Tag    CSN

	uint8_t *modulated_response;
	int modulated_response_size = 0;
	uint8_t* trace_data = NULL;
	int trace_data_size = 0;

	// Respond SOF -- takes 1 bytes
	uint8_t *resp_sof = BigBuf_malloc(2);
	int resp_sof_Len;

	// Anticollision CSN (rotated CSN)
	// 22: Takes 2 bytes for SOF/EOF and 10 * 2 = 20 bytes (2 bytes/byte)
	uint8_t *resp_anticoll = BigBuf_malloc(28);
	int resp_anticoll_len;

	// CSN
	// 22: Takes 2 bytes for SOF/EOF and 10 * 2 = 20 bytes (2 bytes/byte)
	uint8_t *resp_csn = BigBuf_malloc(30);
	int resp_csn_len;

	// configuration  picopass 2ks
	uint8_t *resp_conf = BigBuf_malloc(20);
	int resp_conf_len;
	uint8_t conf_data[10] = {0x12,0xFF,0xFF,0xFF,0x7F,0x1F,0xFF,0x3C,0x00,0x00};
	ComputeCrc14443(CRC_ICLASS, conf_data, 8, &conf_data[8], &conf_data[9]);
	
	// e-Purse
	// 18: Takes 2 bytes for SOF/EOF and 8 * 2 = 16 bytes (2 bytes/bit)
	uint8_t *resp_cc = BigBuf_malloc(20);
	int resp_cc_len;

	// Application Issuer Area 
	uint8_t *resp_aia = BigBuf_malloc(20);
	int resp_aia_len;
	uint8_t aia_data[10] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00};
	ComputeCrc14443(CRC_ICLASS, aia_data, 8, &aia_data[8], &aia_data[9]);

	// receive command
	uint8_t *receivedCmd = BigBuf_malloc(MAX_FRAME_SIZE);
	int len = 0;

	// Prepare card messages
	ToSendMax = 0;

	// First card answer: SOF
	CodeIClassTagSOF();
	memcpy(resp_sof, ToSend, ToSendMax); resp_sof_Len = ToSendMax;
	if ( MF_DBGLEVEL ==  MF_DBG_EXTENDED) {
		DbpString("SOF"); 
		PrintToSendBuffer();
	}
	
	// Anticollision CSN
	CodeIClassTagAnswer(anticoll_data, sizeof(anticoll_data));
	memcpy(resp_anticoll, ToSend, ToSendMax); resp_anticoll_len = ToSendMax;
	if ( MF_DBGLEVEL ==  MF_DBG_EXTENDED) {
		DbpString("ANTI COLL CSN"); 
		PrintToSendBuffer();
	}
	
	// CSN
	CodeIClassTagAnswer(csn_data, sizeof(csn_data));
	memcpy(resp_csn, ToSend, ToSendMax); resp_csn_len = ToSendMax;
	if ( MF_DBGLEVEL ==  MF_DBG_EXTENDED) {
		DbpString("CSN");
		PrintToSendBuffer();
	}

	// Configuration
	CodeIClassTagAnswer(conf_data, sizeof(conf_data));
	memcpy(resp_conf, ToSend, ToSendMax); resp_conf_len = ToSendMax;
	if ( MF_DBGLEVEL ==  MF_DBG_EXTENDED) {
		DbpString("Configuration"); 
		PrintToSendBuffer();
	}
	
	// e-Purse
	CodeIClassTagAnswer(card_challenge_data, sizeof(card_challenge_data));
	memcpy(resp_cc, ToSend, ToSendMax); resp_cc_len = ToSendMax;
	if ( MF_DBGLEVEL ==  MF_DBG_EXTENDED) {
		DbpString("e-Purse"); 
		PrintToSendBuffer();
	}

	// Application Issuer Area
	CodeIClassTagAnswer(aia_data, sizeof(aia_data));
	memcpy(resp_aia, ToSend, ToSendMax); resp_aia_len = ToSendMax;
	if ( MF_DBGLEVEL ==  MF_DBG_EXTENDED) {
		DbpString("Application Issuer Data"); 
		PrintToSendBuffer();
	}

	//This is used for responding to READ-block commands or other data which is dynamically generated
	//First the 'trace'-data, not encoded for FPGA
	uint8_t *data_generic_trace = BigBuf_malloc(8 + 2);//8 bytes data + 2byte CRC is max tag answer

	//Then storage for the modulated data
	//Each bit is doubled when modulated for FPGA, and we also have SOF and EOF (2 bytes)
	uint8_t *data_response = BigBuf_malloc( (8+2) * 2 + 2);

	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_TAGSIM_LISTEN);

	StartCountSspClk();

	// To control where we are in the protocol
	int cmdsRecvd = 0;
	uint32_t time_0 = GetCountSspClk();
	uint32_t t2r_time = 0, r2t_time = 0;

	LED_A_ON();
	bool buttonPressed = false;
	uint8_t response_delay = 1;
	while (!exitLoop) {

		WDT_HIT();

		response_delay = 200;
		// receivedCmd[0] = 0;	receivedCmd[1] = 0;	receivedCmd[2] = 0;	receivedCmd[3] = 0;
		// receivedCmd[4] = 0;	receivedCmd[5] = 0;	receivedCmd[6] = 0;	receivedCmd[7] = 0;
		// receivedCmd[8] = 0;	receivedCmd[9] = 0;	receivedCmd[10] = 0; receivedCmd[11] = 0;
		// receivedCmd[12] = 0;receivedCmd[13] = 0;receivedCmd[14] = 0; receivedCmd[15] = 0;
		
		//Signal tracer, can be used to get a trigger for an oscilloscope..
		LED_B_OFF(); LED_C_OFF();

		if (!GetIClassCommandFromReader(receivedCmd, &len, 0)) {
			buttonPressed = true;
			exitLoop = true;
			continue;
		}

		r2t_time = GetCountSspClk();

		LED_C_ON();	//Signal tracer

		if (receivedCmd[0] == ICLASS_CMD_ACTALL ) {  // 0x0A
			// Reader in anticollission phase
			modulated_response = resp_sof; modulated_response_size = resp_sof_Len; //order = 1;
			trace_data = sof_data;
			trace_data_size = sizeof(sof_data);
		} else if (receivedCmd[0] == ICLASS_CMD_READ_OR_IDENTIFY && len == 1) { // 0x0C
			// Reader asks for anticollission CSN
			modulated_response = resp_anticoll; modulated_response_size = resp_anticoll_len; //order = 2;
			trace_data = anticoll_data;
			trace_data_size = sizeof(anticoll_data);
		} else if (receivedCmd[0] == ICLASS_CMD_SELECT) { // 0x81
			// Reader selects anticollission CSN.
			// Tag sends the corresponding real CSN
			modulated_response = resp_csn; modulated_response_size = resp_csn_len; //order = 3;
			trace_data = csn_data;
			trace_data_size = sizeof(csn_data);
		} else if (receivedCmd[0] == ICLASS_CMD_READCHECK_KD) { // 0x88
			// Read e-purse (88 02)
			modulated_response = resp_cc; modulated_response_size = resp_cc_len; //order = 4;
			trace_data = card_challenge_data;
			trace_data_size = sizeof(card_challenge_data);
			LED_B_ON();
		} else if (receivedCmd[0] == ICLASS_CMD_CHECK) { // 0x05
			// Reader random and reader MAC!!!
			if (simulationMode == MODE_FULLSIM) {
				//NR, from reader, is in receivedCmd +1
				opt_doTagMAC_2(cipher_state,receivedCmd+1,data_generic_trace,diversified_key);

				trace_data = data_generic_trace;
				trace_data_size = 4;
				CodeIClassTagAnswer(trace_data , trace_data_size);
				memcpy(data_response, ToSend, ToSendMax);
				modulated_response = data_response;
				modulated_response_size = ToSendMax;
				response_delay = 0;//We need to hurry here...
				//exitLoop = true;
			} else {
				//Not fullsim, we don't respond
				// We do not know what to answer, so lets keep quiet
				modulated_response = resp_sof; modulated_response_size = 0;
				trace_data = NULL;
				trace_data_size = 0;
				
				if (simulationMode == MODE_EXIT_AFTER_MAC) {
					// dbprintf:ing ...
					Dbprintf("CSN: %02x %02x %02x %02x %02x %02x %02x %02x", csn[0], csn[1], csn[2], csn[3], csn[4], csn[5], csn[6], csn[7]);
					Dbprintf("RDR:  (len=%02d): %02x %02x %02x %02x %02x %02x %02x %02x %02x", len,
							receivedCmd[0], receivedCmd[1], receivedCmd[2],
							receivedCmd[3], receivedCmd[4], receivedCmd[5],
							receivedCmd[6], receivedCmd[7], receivedCmd[8]);

					if (reader_mac_buf != NULL)	{
						memcpy(reader_mac_buf, receivedCmd+1, 8);
					}
					exitLoop = true;
				}
			}

		} else if (receivedCmd[0] == ICLASS_CMD_HALT && len == 1) {
			// Reader ends the session
			modulated_response = resp_sof; modulated_response_size = 0; //order = 0;
			trace_data = NULL;
			trace_data_size = 0;
		// sim 2 / 4,   
		} else if (simulationMode == MODE_EXIT_AFTER_MAC && receivedCmd[0] == ICLASS_CMD_READ_OR_IDENTIFY && len == 4){
			// block0,1,2,5 is always readable.
			uint16_t blk = receivedCmd[1];
			switch (blk){
				case 0:	// csn (0c 00)
					modulated_response = resp_csn; modulated_response_size = resp_csn_len;
					trace_data = csn_data;
					trace_data_size = sizeof(csn_data);
					break;
				case 1:	// configuration (0c 01)
					modulated_response = resp_conf; modulated_response_size = resp_conf_len;
					trace_data = conf_data;
					trace_data_size = sizeof(conf_data);
					break;
				case 2: // e-purse (0c 02)	
					modulated_response = resp_cc; modulated_response_size = resp_cc_len;
					trace_data = card_challenge_data;
					trace_data_size = sizeof(card_challenge_data);
					break;
				case 5:// Application Issuer Area (0c 05)
					modulated_response = resp_aia; modulated_response_size = resp_aia_len;
					trace_data = aia_data;
					trace_data_size = sizeof(aia_data);
					break;
				default: break;
			}						
			
		} else if (simulationMode == MODE_FULLSIM && receivedCmd[0] == ICLASS_CMD_READ_OR_IDENTIFY && len == 4){
			//Read block
			uint16_t blk = receivedCmd[1];
			//Take the data...
			memcpy(data_generic_trace, emulator+(blk << 3),8);
			//Add crc
			AppendCrc(data_generic_trace, 8);
			trace_data = data_generic_trace;
			trace_data_size = 10;
			CodeIClassTagAnswer(trace_data , trace_data_size);
			memcpy(data_response, ToSend, ToSendMax);
			modulated_response = data_response;
			modulated_response_size = ToSendMax;
		} else if (simulationMode == MODE_FULLSIM && receivedCmd[0] == ICLASS_CMD_UPDATE) {
			
			//Probably the reader wants to update the nonce. Let's just ignore that for now.
			// OBS! If this is implemented, don't forget to regenerate the cipher_state
			//We're expected to respond with the data+crc, exactly what's already in the receivedcmd
			//receivedcmd is now UPDATE 1b | ADDRESS 1b| DATA 8b| Signature 4b or CRC 2b|

			//Take the data...
			memcpy(data_generic_trace, receivedCmd+2,8);
			//Add crc
			AppendCrc(data_generic_trace, 8);
			trace_data = data_generic_trace;
			trace_data_size = 10;
			CodeIClassTagAnswer(trace_data , trace_data_size);
			memcpy(data_response, ToSend, ToSendMax);
			modulated_response = data_response;
			modulated_response_size = ToSendMax;
//		} else if(receivedCmd[0] == ICLASS_CMD_PAGESEL)	{  // 0x84
			//Pagesel
			//Pagesel enables to select a page in the selected chip memory and return its configuration block
			//Chips with a single page will not answer to this command
			// It appears we're fine ignoring this.
			//Otherwise, we should answer 8bytes (block) + 2bytes CRC
//		} else if(receivedCmd[0] == ICLASS_CMD_DETECT)	{  // 0x0F
		} else {
			//#db# Unknown command received from reader (len=5): 26 1 0 f6 a 44 44 44 44
			// Never seen this command before
			if ( MF_DBGLEVEL ==  MF_DBG_EXTENDED) {
				Dbprintf("Unhandled command received from reader (len %d) | %02x %02x %02x %02x %02x %02x %02x %02x %02x",
					len,
					receivedCmd[0], receivedCmd[1], receivedCmd[2], receivedCmd[3], 
					receivedCmd[4], receivedCmd[5], receivedCmd[6], receivedCmd[7], receivedCmd[8]
				);
			}
			
			// Do not respond
			modulated_response = resp_sof;
			modulated_response_size = 0; //order = 0;
			trace_data = NULL;
			trace_data_size = 0;
		}

		cmdsRecvd++;
		
		/**
		A legit tag has about 380us delay between reader EOT and tag SOF.
		**/
		if (modulated_response_size > 0) {
			SendIClassAnswer(modulated_response, modulated_response_size, response_delay);
			t2r_time = (GetCountSspClk() - time_0) << 4;
		}

		LogTrace(receivedCmd, len, (r2t_time - time_0)<< 4, (r2t_time - time_0) << 4, NULL, true);

		if (trace_data != NULL) {
			LogTrace(trace_data, trace_data_size, t2r_time, t2r_time, NULL, false);
			if ( MF_DBGLEVEL ==  MF_DBG_EXTENDED) DbpString("trace written");
		}
	}

	LEDsoff();
	
	if (buttonPressed)
		DbpString("Button pressed");
	
	return buttonPressed;
}

/**
 * @brief sends our simulated tag answer 
 * @param resp
 * @param respLen
 * @param delay
 */
static int SendIClassAnswer(uint8_t *resp, int respLen, int delay) {
	int i = 0, d = 0;
	uint8_t b = 0;

	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_MODULATE_424K_8BIT);	
	AT91C_BASE_SSC->SSC_THR = 0x00;
	//FpgaSetupSsc();

	while (!BUTTON_PRESS()) {
		if ( (AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY)){
			b = AT91C_BASE_SSC->SSC_RHR; (void) b;
		}
		if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY)){
			b = 0x00;
			if (d < delay) {
				d++;
			} else {
				if ( i < respLen){
					b = resp[i];
					//Hack
					//b = 0xAC;
				}
				i++;
			}
			AT91C_BASE_SSC->SSC_THR = b;
		}
//		if (i > respLen + 4) break;
		if (i > respLen + 1) break;
	}
	return 0;
}

/// THE READER CODE

//-----------------------------------------------------------------------------
// Transmit the command (to the tag) that was placed in ToSend[].
//-----------------------------------------------------------------------------
static void TransmitIClassCommand(const uint8_t *cmd, int len, int *samples, int *wait) {

	int c;
	volatile uint32_t r;
	bool firstpart = true;
	uint8_t sendbyte;

	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_READER_MOD);
	AT91C_BASE_SSC->SSC_THR = 0x00;
	//SpinDelay(200);

	if (wait) {
		if (*wait < 10) *wait = 10;

		for (c = 0; c < *wait;) {
			
			WDT_HIT();

			if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY)) {
				AT91C_BASE_SSC->SSC_THR = 0x00;		// For exact timing!
				c++;
			}
			if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
				r = AT91C_BASE_SSC->SSC_RHR; (void)r;
			}
		}
	}

	c = 0;

	for(;;) {

		WDT_HIT();

		if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY)) {

			// DOUBLE THE SAMPLES!
			if (firstpart) {
				sendbyte = (cmd[c] & 0xf0) | (cmd[c] >> 4); 
			} else {
				sendbyte = (cmd[c] & 0x0f) | (cmd[c] << 4);
				c++;
			}

			if(sendbyte == 0xff)
				sendbyte = 0xfe;

			AT91C_BASE_SSC->SSC_THR = sendbyte;
			firstpart = !firstpart;

			if (c >= len) break;
		}

		if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
			r = AT91C_BASE_SSC->SSC_RHR; (void)r;
		}		
	}

	if (samples) {
		if (wait)
			*samples = (c + *wait) << 3;
		else
			*samples = c << 3;	
	}
}

//-----------------------------------------------------------------------------
// Prepare iClass reader command to send to FPGA
//-----------------------------------------------------------------------------
void CodeIClassCommand(const uint8_t* cmd, int len) {
	int i, j, k;
	uint8_t b;

	ToSendReset();

	// (SOC) Start of Communication: 1 out of 4
	ToSend[++ToSendMax] = 0xf0;
	ToSend[++ToSendMax] = 0x00;
	ToSend[++ToSendMax] = 0x0f;
	ToSend[++ToSendMax] = 0x00;

	// Modulate the bytes 
	for (i = 0; i < len; i++) {
		b = cmd[i];
		for (j = 0; j < 4; j++) {
			for (k = 0; k < 4; k++) {

				if (k == (b & 3))
					ToSend[++ToSendMax] = 0xf0;
				else
					ToSend[++ToSendMax] = 0x00;			
			}
			b >>= 2;
		}
	}

	// (EOC) End of Communication
	ToSend[++ToSendMax] = 0x00;
	ToSend[++ToSendMax] = 0x00;
	ToSend[++ToSendMax] = 0xf0;
	ToSend[++ToSendMax] = 0x00;

	// Convert from last character reference to length
	ToSendMax++;
}

void ReaderTransmitIClass(uint8_t* frame, int len) {

	int wait = 0, samples = 0;

	// This is tied to other size changes
	CodeIClassCommand(frame, len);

	// Select the card
	TransmitIClassCommand(ToSend, ToSendMax, &samples, &wait);
	if (trigger)
		LED_A_ON();

	// Store reader command in buffer
	//uint8_t par[len/8];
	//GetParity(frame, len, par);
	//LogTrace(frame, len, rsamples, rsamples, par, true);
	LogTrace(frame, len, rsamples, rsamples, NULL, true);
}

//-----------------------------------------------------------------------------
// Wait a certain time for tag response
//  If a response is captured return TRUE
//  If it takes too long return FALSE
//-----------------------------------------------------------------------------
static int GetIClassAnswer(uint8_t* receivedResponse, int maxLen, int *samples, int *elapsed) {
	// buffer needs to be 512 bytes
	// maxLen is not used...

	int c = 0;
	bool skip = false;

	// Setup UART/DEMOD to receive 
	DemodInit(receivedResponse);

	if (elapsed) *elapsed = 0;

	// Set FPGA mode to "reader listen mode", no modulation (listen
	// only, since we are receiving, not transmitting).
	FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_READER_LISTEN);

	// clear RXRDY:
    uint8_t b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;

	while (!BUTTON_PRESS()) {
		WDT_HIT();

		if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY)) {
			AT91C_BASE_SSC->SSC_THR = 0x00;  
			// To make use of exact timing of next command from reader!!
			if (elapsed) (*elapsed)++;
		}

		if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
			if (c >= timeout) return false;

			c++;
			
			b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
			
			skip = !skip;			
			if (skip) continue;
		
			if (ManchesterDecoding(b & 0x0f)) {
				if (samples) 
					*samples = c << 3;
				return  true;
			}
		}
	}
	return false;
}

int ReaderReceiveIClass(uint8_t* receivedAnswer) {
	int samples = 0;

	if (!GetIClassAnswer(receivedAnswer, 0, &samples, NULL)) 
		return false;

	rsamples += samples;

	LogTrace(receivedAnswer, Demod.len, rsamples, rsamples, NULL, false);

	if (samples == 0) 
		return false;
	return Demod.len;
}

void setupIclassReader() {

	LEDsoff();

    // Start from off (no field generated)
    // Signal field is off with the appropriate LED
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
	
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
	
    FpgaSetupSsc();

    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

    // Reset trace buffer
	clear_trace();
	set_tracing(true);

    // Now give it time to spin up.
    // Signal field is on with the appropriate LED
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_READER_MOD);
    SpinDelay(200);

	// Start the timer
	StartCountSspClk();
	
    LED_A_ON();
}

bool sendCmdGetResponseWithRetries(uint8_t* command, size_t cmdsize, uint8_t* resp, uint8_t expected_size, uint8_t retries) {
	while (retries-- > 0) {
		
		ReaderTransmitIClass(command, cmdsize);
		
		if (expected_size == ReaderReceiveIClass(resp))
			return true;
	}
	return false;
}

/**
 * @brief Talks to an iclass tag, sends the commands to get CSN and CC.
 * @param card_data where the CSN and CC are stored for return
 * @return 0 = fail
 *         1 = Got CSN
 *         2 = Got CSN and CC
 */
uint8_t handshakeIclassTag_ext(uint8_t *card_data, bool use_credit_key) {

	// act_all...
	static uint8_t act_all[]      = { ICLASS_CMD_ACTALL };
	static uint8_t identify[]     = { ICLASS_CMD_READ_OR_IDENTIFY, 0x00, 0x73, 0x33 };
	static uint8_t select[]       = { ICLASS_CMD_SELECT, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static uint8_t readcheck_cc[] = { ICLASS_CMD_READCHECK_KD, 0x02 };

	if  (use_credit_key) 
		readcheck_cc[0] = ICLASS_CMD_READCHECK_KC;

	uint8_t resp[ICLASS_BUFFER_SIZE] = {0};
	uint8_t read_status = 0;

	// Send act_all
	ReaderTransmitIClass(act_all, 1);

	// Card present?
	if (!ReaderReceiveIClass(resp)) return read_status;//Fail

	//Send Identify
	ReaderTransmitIClass(identify, 1);

	//We expect a 10-byte response here, 8 byte anticollision-CSN and 2 byte CRC
	uint8_t len = ReaderReceiveIClass(resp);
	if (len != 10) return read_status;//Fail

	//Copy the Anti-collision CSN to our select-packet
	memcpy(&select[1], resp, 8);
	//Select the card
	ReaderTransmitIClass(select, sizeof(select));
	//We expect a 10-byte response here, 8 byte CSN and 2 byte CRC
	len  = ReaderReceiveIClass(resp);
	if (len != 10) return read_status;//Fail

	//Success - level 1, we got CSN
	//Save CSN in response data
	memcpy(card_data, resp, 8);

	//Flag that we got to at least stage 1, read CSN
	read_status = 1;

	// Card selected, now read e-purse (cc) (only 8 bytes no CRC)
	ReaderTransmitIClass(readcheck_cc, sizeof(readcheck_cc));
	if (ReaderReceiveIClass(resp) == 8) {
		//Save CC (e-purse) in response data
		memcpy(card_data+8, resp, 8);
		read_status++;
	}

	return read_status;
}
uint8_t handshakeIclassTag(uint8_t *card_data){
	return handshakeIclassTag_ext(card_data, false);
}

// Reader iClass Anticollission
// turn off afterwards
void ReaderIClass(uint8_t arg0) {

	uint8_t card_data[6 * 8] = {0};
	memset(card_data, 0xFF, sizeof(card_data));
	uint8_t last_csn[8] = {0,0,0,0,0,0,0,0};
	uint8_t resp[ICLASS_BUFFER_SIZE];
	memset(resp, 0xFF, sizeof(resp));
	
	//Read conf block CRC(0x01) => 0xfa 0x22
	uint8_t readConf[] = { ICLASS_CMD_READ_OR_IDENTIFY, 0x01, 0xfa, 0x22};
	
	//Read App Issuer Area block CRC(0x05) => 0xde  0x64
	uint8_t readAA[] = { ICLASS_CMD_READ_OR_IDENTIFY, 0x05, 0xde, 0x64};

    int read_status= 0;
	uint8_t result_status = 0;
	// flag to read until one tag is found successfully
    bool abort_after_read = arg0 & FLAG_ICLASS_READER_ONLY_ONCE;
	
	// flag to only try 5 times to find one tag then return
	bool try_once = arg0 & FLAG_ICLASS_READER_ONE_TRY;
	
	// if neither abort_after_read nor try_once then continue reading until button pressed.

	bool use_credit_key = arg0 & FLAG_ICLASS_READER_CEDITKEY;
	// test flags for what blocks to be sure to read
	uint8_t flagReadConfig = arg0 & FLAG_ICLASS_READER_CONF;
	uint8_t flagReadCC = arg0 & FLAG_ICLASS_READER_CC;
	uint8_t flagReadAA = arg0 & FLAG_ICLASS_READER_AA;

	setupIclassReader();

	uint16_t tryCnt = 0;
	bool userCancelled = BUTTON_PRESS() || usb_poll_validate_length();
	while (!userCancelled) {

		WDT_HIT();

		// if only looking for one card try 2 times if we missed it the first time
		if (try_once && tryCnt > 2) break; 
		
		tryCnt++;

		read_status = handshakeIclassTag_ext(card_data, use_credit_key);

		if (read_status == 0) continue;
		if (read_status == 1) result_status = FLAG_ICLASS_READER_CSN;
		if (read_status == 2) result_status = FLAG_ICLASS_READER_CSN | FLAG_ICLASS_READER_CC;

		// handshakeIclass returns CSN|CC, but the actual block
		// layout is CSN|CONFIG|CC, so here we reorder the data,
		// moving CC forward 8 bytes
		memcpy(card_data+16, card_data+8, 8);

		//Read block 1, config
		if (flagReadConfig) {
			if (sendCmdGetResponseWithRetries(readConf, sizeof(readConf), resp, 10, 10)) {
				result_status |= FLAG_ICLASS_READER_CONF;
				memcpy(card_data+8, resp, 8);
			} else {
				DbpString("Failed to dump config block");
			}
		}

		//Read block 5, AA
		if (flagReadAA) {
			if (sendCmdGetResponseWithRetries(readAA, sizeof(readAA), resp, 10, 10)) {
				result_status |= FLAG_ICLASS_READER_AA;
				memcpy(card_data+(8*5), resp, 8);
			} else {
				//DbpString("Failed to dump AA block");
			}
		}

		// 0 : CSN
		// 1 : Configuration
		// 2 : e-purse
		// (3,4 write-only, kc and kd)
		// 5 Application issuer area
		//
		//Then we can 'ship' back the 8 * 5 bytes of data,
		// with 0xFF:s in block 3 and 4.

		LED_B_ON();
		//Send back to client, but don't bother if we already sent this - 
		//  only useful if looping in arm (not try_once && not abort_after_read)			
		if (memcmp(last_csn, card_data, 8) != 0) {
			// If caller requires that we get Conf, CC, AA, continue until we got it
			if ( (result_status ^ FLAG_ICLASS_READER_CSN ^ flagReadConfig ^ flagReadCC ^ flagReadAA) == 0) {
				cmd_send(CMD_ACK, result_status, 0, 0, card_data, sizeof(card_data) );
				if (abort_after_read) 
					goto out;

				//Save that we already sent this....
				memcpy(last_csn, card_data, 8);
			}
		}
		LED_B_OFF();
		userCancelled = BUTTON_PRESS() || usb_poll_validate_length();
	}
	if (userCancelled)
		cmd_send(CMD_ACK, 0xFF, 0, 0, card_data, 0);
	else
		cmd_send(CMD_ACK, 0, 0, 0, card_data, 0);		

out:    
	switch_off(); 
}

// turn off afterwards
void ReaderIClass_Replay(uint8_t arg0, uint8_t *MAC) {

	uint8_t card_data[USB_CMD_DATA_SIZE] = {0};
	uint16_t block_crc_LUT[255] = {0};

	//Generate a lookup table for block crc
	for (int block = 0; block < 255; block++){
		char bl = block;
		block_crc_LUT[block] = iclass_crc16(&bl ,1);
	}
	
	//Dbprintf("Lookup table: %02x %02x %02x" ,block_crc_LUT[0],block_crc_LUT[1],block_crc_LUT[2]);

	uint8_t check[] = { 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	uint8_t read[]  = { 0x0c, 0x00, 0x00, 0x00 };
	
    uint16_t crc = 0;
	uint8_t cardsize = 0;
	uint8_t mem = 0;
	
	static struct memory_t{
	  int k16;
	  int book;
	  int k2;
	  int lockauth;
	  int keyaccess;
	} memory;
	
	uint8_t resp[ICLASS_BUFFER_SIZE];
	
    setupIclassReader();

	while (!BUTTON_PRESS()) {
	
		WDT_HIT();
		
		uint8_t read_status = handshakeIclassTag(card_data);
		if (read_status < 2) continue;

		//for now replay captured auth (as cc not updated)
		memcpy(check+5, MAC, 4);

		if (!sendCmdGetResponseWithRetries(check, sizeof(check), resp, 4, 5)) {
			DbpString("Error: Authentication Fail!");
			continue;
		}

		//first get configuration block (block 1)
		crc = block_crc_LUT[1];
		read[1] = 1;
		read[2] = crc >> 8;
		read[3] = crc & 0xff;

		if (!sendCmdGetResponseWithRetries(read, sizeof(read), resp, 10, 10)) {
			DbpString("Dump config (block 1) failed");
			continue;
		}

		mem = resp[5];
		memory.k16 = (mem & 0x80);
		memory.book = (mem & 0x20);
		memory.k2 = (mem & 0x8);
		memory.lockauth = (mem & 0x2);
		memory.keyaccess = (mem & 0x1);

		cardsize = memory.k16 ? 255 : 32;

		WDT_HIT();
		//Set card_data to all zeroes, we'll fill it with data
		memset(card_data, 0x0, USB_CMD_DATA_SIZE);
		uint8_t failedRead = 0;
		uint32_t stored_data_length = 0;

		//then loop around remaining blocks
		for (int block=0; block < cardsize; block++) {

			read[1] = block;
			crc = block_crc_LUT[block];
			read[2] = crc >> 8;
			read[3] = crc & 0xff;

			if (sendCmdGetResponseWithRetries(read, sizeof(read), resp, 10, 10)) {
				Dbprintf("     %02x: %02x %02x %02x %02x %02x %02x %02x %02x",
					block, resp[0], resp[1], resp[2],
					resp[3], resp[4], resp[5],
					resp[6], resp[7]
				);

				//Fill up the buffer
				memcpy(card_data + stored_data_length, resp, 8);
				stored_data_length += 8;
				if (stored_data_length + 8 > USB_CMD_DATA_SIZE) {
					//Time to send this off and start afresh
					cmd_send(CMD_ACK,
							 stored_data_length,//data length
							 failedRead,//Failed blocks?
							 0,//Not used ATM
							 card_data,
							 stored_data_length
					);
					//reset
					stored_data_length = 0;
					failedRead = 0;
				}
			} else {
				failedRead = 1;
				stored_data_length += 8;//Otherwise, data becomes misaligned
				Dbprintf("Failed to dump block %d", block);
			}
		}

		//Send off any remaining data
		if (stored_data_length > 0) {
			cmd_send(CMD_ACK,
					 stored_data_length,//data length
					 failedRead,//Failed blocks?
					 0,//Not used ATM
					 card_data,
					 stored_data_length
			);
		}
		//If we got here, let's break
		break;
	}
	//Signal end of transmission
	cmd_send(CMD_ACK,
			 0,//data length
			 0,//Failed blocks?
			 0,//Not used ATM
			 card_data,
			 0
	);

	switch_off(); 
}

// turn off afterwards
void iClass_ReadCheck(uint8_t	blockNo, uint8_t keyType) {
	uint8_t readcheck[] = { keyType, blockNo };
	uint8_t resp[] = {0,0,0,0,0,0,0,0};
	size_t isOK = 0;
	isOK = sendCmdGetResponseWithRetries(readcheck, sizeof(readcheck), resp, sizeof(resp), 6);
	cmd_send(CMD_ACK,isOK,0,0,0,0);
	switch_off(); 
}

// used with function select_and_auth (cmdhficlass.c) 
// which needs to authenticate before doing more things like read/write
void iClass_Authentication(uint8_t *MAC) {
	uint8_t check[] = { ICLASS_CMD_CHECK, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	uint8_t resp[ICLASS_BUFFER_SIZE];
	memcpy(check+5, MAC, 4);
	bool isOK;
	isOK = sendCmdGetResponseWithRetries(check, sizeof(check), resp, 4, 6);
	cmd_send(CMD_ACK,isOK,0,0,0,0);
}

bool iClass_ReadBlock(uint8_t blockNo, uint8_t *readdata) {
	uint8_t readcmd[] = {ICLASS_CMD_READ_OR_IDENTIFY, blockNo, 0x00, 0x00}; //0x88, 0x00 // can i use 0C?
	char bl = blockNo;
	uint16_t crc = iclass_crc16(&bl, 1);
	readcmd[2] = crc >> 8;
	readcmd[3] = crc & 0xff;
	uint8_t resp[] = {0,0,0,0,0,0,0,0,0,0};

	bool isOK = sendCmdGetResponseWithRetries(readcmd, sizeof(readcmd), resp, 10, 10);
	memcpy(readdata, resp, sizeof(resp));
	return isOK;
}

// turn off afterwards
void iClass_ReadBlk(uint8_t blockno) {
	uint8_t readblockdata[] = {0,0,0,0,0,0,0,0,0,0};
	bool isOK = false;
	isOK = iClass_ReadBlock(blockno, readblockdata);
	cmd_send(CMD_ACK, isOK, 0, 0, readblockdata, 8);
	switch_off(); 
}

// turn off afterwards
void iClass_Dump(uint8_t blockno, uint8_t numblks) {
	uint8_t readblockdata[] = {0,0,0,0,0,0,0,0,0,0};
	bool isOK = false;
	uint8_t blkCnt = 0;

	BigBuf_free();
	uint8_t *dataout = BigBuf_malloc(255*8);
	if (dataout == NULL){
		DbpString("out of memory");
		OnError(1);
		return;
	}
	// fill mem with 0xFF
	memset(dataout, 0xFF, 255*8);

	for (;blkCnt < numblks; blkCnt++) {
		isOK = iClass_ReadBlock(blockno + blkCnt, readblockdata);
		
		// 0xBB is the internal debug separator byte..
		if (!isOK || (readblockdata[0] == 0xBB || readblockdata[7] == 0xBB || readblockdata[2] == 0xBB)) { //try again
			isOK = iClass_ReadBlock(blockno + blkCnt, readblockdata);
			if (!isOK) {
				Dbprintf("Block %02X failed to read", blkCnt + blockno);
				break;
			}
		}
		memcpy(dataout + (blkCnt * 8), readblockdata, 8);
	}
	//return pointer to dump memory in arg3
	cmd_send(CMD_ACK, isOK, blkCnt, BigBuf_max_traceLen(), 0, 0);
	switch_off(); 
	BigBuf_free();
}

bool iClass_WriteBlock_ext(uint8_t blockNo, uint8_t *data) {
	uint8_t write[] = { ICLASS_CMD_UPDATE, blockNo, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	memcpy(write+2, data, 12); // data + mac
	char *wrCmd = (char *)(write+1); 
	uint16_t crc = iclass_crc16(wrCmd, 13);
	write[14] = crc >> 8;
	write[15] = crc & 0xff;
	uint8_t resp[] = {0,0,0,0,0,0,0,0,0,0};

	bool isOK = sendCmdGetResponseWithRetries(write, sizeof(write), resp, sizeof(resp), 10);
	if (isOK) { //if reader responded correctly
		//Dbprintf("WriteResp: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",resp[0],resp[1],resp[2],resp[3],resp[4],resp[5],resp[6],resp[7],resp[8],resp[9]);

		//if response is not equal to write values
		if (memcmp(write + 2, resp, 8)) {
			 //if not programming key areas (note key blocks don't get programmed with actual key data it is xor data)
			if (blockNo != 3 && blockNo != 4) {
				//error try again
				isOK = sendCmdGetResponseWithRetries(write, sizeof(write), resp, sizeof(resp), 10);
			} 			
		}
	}
	return isOK;
}

// turn off afterwards
void iClass_WriteBlock(uint8_t blockNo, uint8_t *data) {
	bool isOK = iClass_WriteBlock_ext(blockNo, data);
	if (isOK)
		Dbprintf("Write block [%02x] successful", blockNo);
    else
		Dbprintf("Write block [%02x] failed", blockNo);		
	
	cmd_send(CMD_ACK,isOK,0,0,0,0);
	switch_off(); 
}

// turn off afterwards
void iClass_Clone(uint8_t startblock, uint8_t endblock, uint8_t *data) {
	int i, written = 0;
	int total_block = (endblock - startblock) + 1;
	for (i = 0; i < total_block; i++){
		// block number
		if (iClass_WriteBlock_ext(i + startblock, data + ( i*12 ) )){
			Dbprintf("Write block [%02x] successful", i + startblock);
			written++;
		} else {
			if (iClass_WriteBlock_ext(i + startblock, data + ( i*12 ) )){
				Dbprintf("Write block [%02x] successful", i + startblock);
				written++;
			} else {
				Dbprintf("Write block [%02x] failed", i + startblock);
			}
		}
	}
	if (written == total_block)
		DbpString("Clone complete");
	else
		DbpString("Clone incomplete");   

	cmd_send(CMD_ACK,1,0,0,0,0);
	switch_off(); 
}