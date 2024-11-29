/*
	gbamp_cf.c
	By chishm (Michael Chisholm)

	Routines for reading a compact flash card
	using the GBA Movie Player.

	Some FAT routines are based on those in fat.c, which
	is part of avrlib by Pascal Stang.

	CF routines modified with help from Darkfader

	This software is completely free. No warranty is provided.
	If you use it, please give credit and/or email me about your
	project at chishm@hotmail.com
*/

#include "gbamp_cf.h"

//---------------------------------------------------------------
// Customizable features

// Use DMA to read the card
#ifndef NDS				// DMA doesn't work on the ARM9 of the NDS
 #define  _CF_USE_DMA 0	// Remove this if DMA is unavailable
#endif

//----------------------------------------------------------------
// Data	types
#ifndef	NULL
 #define	NULL	0
#endif

//----------------------------------------------------------------
// NDS memory access control register
#ifdef NDS
 #ifndef WAIT_CR
  #define WAIT_CR (*(vu16*)0x04000204)
 #endif
#endif

//---------------------------------------------------------------
// DMA
#ifdef _CF_USE_DMA
 #include "gba_dma.h"
#endif

//---------------------------------------------------------------
// CF Addresses & Commands

#define GAME_PAK		0x08000000			// Game pack start address

#define CF_REG_STS		*(vu16*)(GAME_PAK + 0x018C0000)	// Status of the CF Card / Device control
#define CF_REG_CMD		*(vu16*)(GAME_PAK + 0x010E0000)	// Commands sent to control chip and status return
#define CF_REG_ERR		*(vu16*)(GAME_PAK + 0x01020000)	// Errors / Features

#define CF_REG_SEC		*(vu16*)(GAME_PAK + 0x01040000)	// Number of sector to transfer
#define CF_REG_LBA1		*(vu16*)(GAME_PAK + 0x01060000)	// 1st byte of sector address
#define CF_REG_LBA2		*(vu16*)(GAME_PAK + 0x01080000)	// 2nd byte of sector address
#define CF_REG_LBA3		*(vu16*)(GAME_PAK + 0x010A0000)	// 3rd byte of sector address
#define CF_REG_LBA4		*(vu16*)(GAME_PAK + 0x010C0000)	// last nibble of sector address | 0xE0

#define CF_DATA			(vu16*)(GAME_PAK + 0x01000000)		// Pointer to buffer of CF data transered from card

// Card status
#define CF_STS_INSERTED		0x50
#define CF_STS_REMOVED		0x00
#define CF_STS_READY		0x58

#define CF_STS_DRQ			0x08
#define CF_STS_BUSY			0x80

// Card commands
#define CF_CMD_LBA			0xE0
#define CF_CMD_READ			0x20
#define CF_CMD_WRITE		0x30

#define CARD_TIMEOUT	50000

#define BYTE_PER_READ 512

//-----------------------------------------------------------------
// FAT constants
#define CLUSTER_EOF_16	0xFFFF
#define	CLUSTER_EOF	0x0FFFFFFF
#define CLUSTER_FREE	0x0000
#define CLUSTER_FIRST	0x0002

//-----------------------------------------------------------------
// long file name constants
#define LFN_END 0x40
#define LFN_DEL 0x80

//-----------------------------------------------------------------
// Data Structures

// Take care of packing for GCC - it doesn't obey pragma pack()
// properly for ARM targets.
#ifdef __GNUC__
 #define __PACKED __attribute__ ((__packed__))
#else
 #define __PACKED 
 #pragma pack(1)
#endif

// Boot Sector - must be packed
typedef struct
{
	__PACKED	u8	jmpBoot[3];
	__PACKED	u8	OEMName[8];
	// BIOS Parameter Block
	__PACKED	u16	bytesPerSector;
	__PACKED	u8	sectorsPerCluster;
	__PACKED	u16	reservedSectors;
	__PACKED	u8	numFATs;
	__PACKED	u16	rootEntries;
	__PACKED	u16	numSectorsSmall;
	__PACKED	u8	mediaDesc;
	__PACKED	u16	sectorsPerFAT;
	__PACKED	u16	sectorsPerTrk;
	__PACKED	u16	numHeads;
	__PACKED	u32	numHiddenSectors;
	__PACKED	u32	numSectors;
	union	// Different types of extended BIOS Parameter Block for FAT16 and FAT32
	{
		struct  
		{
			// Ext BIOS Parameter Block for FAT16
			__PACKED	u8	driveNumber;
			__PACKED	u8	reserved1;
			__PACKED	u8	extBootSig;
			__PACKED	u32	volumeID;
			__PACKED	u8	volumeLabel[11];
			__PACKED	u8	fileSysType[8];
			// Bootcode
			__PACKED	u8	bootCode[448];
		}	fat16;
		struct  
		{
			// FAT32 extended block
			__PACKED	u32	sectorsPerFAT32;
			__PACKED	u16	extFlags;
			__PACKED	u16	fsVer;
			__PACKED	u32	rootClus;
			__PACKED	u16	fsInfo;
			__PACKED	u16	bkBootSec;
			__PACKED	u8	reserved[12];
			// Ext BIOS Parameter Block for FAT16
			__PACKED	u8	driveNumber;
			__PACKED	u8	reserved1;
			__PACKED	u8	extBootSig;
			__PACKED	u32	volumeID;
			__PACKED	u8	volumeLabel[11];
			__PACKED	u8	fileSysType[8];
			// Bootcode
			__PACKED	u8	bootCode[420];
		}	fat32;
	}	extBlock;

	__PACKED	u16	bootSig;

}	BOOT_SEC;

// Directory entry - must be packed
typedef struct
{
	__PACKED	u8	name[8];
	__PACKED	u8	ext[3];
	__PACKED	u8	attrib;
	__PACKED	u8	reserved;
	__PACKED	u8	cTime_ms;
	__PACKED	u16	cTime;
	__PACKED	u16	cDate;
	__PACKED	u16	aDate;
	__PACKED	u16	startClusterHigh;
	__PACKED	u16	mTime;
	__PACKED	u16	mDate;
	__PACKED	u16	startCluster;
	__PACKED	u32	fileSize;
}	DIR_ENT;

// Long file name directory entry - must be packed
typedef struct
{
	__PACKED	u8 ordinal;	// Position within LFN
	__PACKED	u16 char0;	
	__PACKED	u16 char1;
	__PACKED	u16 char2;
	__PACKED	u16 char3;
	__PACKED	u16 char4;
	__PACKED	u8 flag;	// Should be equal to ATTRIB_LFN
	__PACKED	u8 reserved1;	// Always 0x00
	__PACKED	u8 checkSum;	// Checksum of short file name (alias)
	__PACKED	u16 char5;
	__PACKED	u16 char6;
	__PACKED	u16 char7;
	__PACKED	u16 char8;
	__PACKED	u16 char9;
	__PACKED	u16 char10;
	__PACKED	u16 reserved2;	// Always 0x0000
	__PACKED	u16 char11;
	__PACKED	u16 char12;
}	DIR_ENT_LFN;

// End of packed structs
#ifdef __PACKED
 #undef __PACKED
#endif
#ifndef __GNUC__
 #pragma pack()
#endif


// File information - no need to pack
typedef struct
{
	u32 firstCluster;
	u32 length;
	u32 curPos;
	u32 curClus;			// Current cluster to read from
	int curSect;			// Current sector within cluster
	int curByte;			// Current byte within sector
	char readBuffer[512];	// Buffer used for unaligned reads
	u32 appClus;			// Cluster to append to
	int appSect;			// Sector within cluster for appending
	int appByte;			// Byte within sector for appending
	bool read;	// Can read from file
	bool write;	// Can write to file
	bool append;// Can append to file
	bool inUse;	// This file is open
	u32 dirEntSector;	// The sector where the directory entry is stored
	int dirEntOffset;	// The offset within the directory sector
}	FAT_FILE;


//-----------------------------------------------------------------
// Global Variables

// _VARS_IN_RAM variables are stored in the largest section of WRAM 
// available: IWRAM on NDS ARM7, EWRAM on NDS ARM9 and GBA

// Files
_VARS_IN_RAM FAT_FILE openFiles[MAX_FILES_OPEN];

// Long File names
_VARS_IN_RAM char lfnName[MAX_FILENAME_LENGTH];
bool lfnExists;

// Locations on card
int discRootDir;
int discRootDirClus;
int discFAT;
int discSecPerFAT;
int discNumSec;
int discData;
int discBytePerSec;
int discSecPerClus;
int discBytePerClus;

bool discFAT32;

// Info about FAT
u32 fatLastCluster;
u32 fatFirstFree;
bool fatWriteUpdate;
u32 fatCurSector;

// fatWriteBuffer used to reduce wear on the CF card from multiple 
_VARS_IN_RAM char fatWriteBuffer[BYTE_PER_READ];

// Current working directory
u32 curWorkDirCluster;

// Position of the directory entry last retreived with FAT_GetDirEntry
u32 wrkDirCluster;
int wrkDirSector;
int wrkDirOffset;

// Global sector buffer to save on stack space
_VARS_IN_RAM unsigned char globalBuffer[BYTE_PER_READ];

//-----------------------------------------------------------------
// Functions contained in this file - predeclarations
char ucase (char character);
_CODE_IN_RAM bool CF_IsInserted (void);
_CODE_IN_RAM bool CF_ClearStatus (void);
_CODE_IN_RAM bool CF_ReadSector (u32 sector, void* buffer);
_CODE_IN_RAM bool CF_WriteSector (u32 sector, void* buffer);
u32 CF_LastSector (void);
bool FAT_AddDirEntry (const char* path, DIR_ENT newDirEntry);
bool FAT_ClearLinks (u32 cluster);
bool FAT_DeleteFile (const char* path);
DIR_ENT FAT_DirEntFromPath (const char* path);
u32 FAT_FirstFreeCluster(void);
DIR_ENT FAT_GetDirEntry ( u32 dirCluster, int entry, int origin);
u32 FAT_LinkFreeCluster(u32 cluster);
u32 FAT_NextCluster(u32 cluster);
bool FAT_WriteFatEntry (u32 cluster, int value);
bool FAT_GetFilename (DIR_ENT dirEntry, char* alias);

bool FAT_InitFiles (void);
bool FAT_FreeFiles (void);
bool FAT_CWD (const char* path);
int FAT_FindFirstFile(char* filename);
int FAT_FindNextFile(char* filename);
bool FAT_GetAlias (char* alias);
bool FAT_GetLongFilename (char* filename);

int FAT_fopen(const char* path, const char* mode);
bool FAT_fclose (int file);
bool FAT_feof(int file);
int FAT_fseek(int file, u32 offset, int origin);
long int FAT_ftell (int file);
u32 FAT_fread (void* buffer, u32 size, u32 count, int file);
u32 FAT_fwrite (void* buffer, u32 size, u32 count, int file);

/*-----------------------------------------------------------------
ucase
Returns the uppercase version of the given char
char IN: a character
char return OUT: uppercase version of character
-----------------------------------------------------------------*/
char ucase (char character)
{
	if ((character > 0x60) && (character < 0x7B))
		character = character - 0x20;
	return (character);
}

/*-----------------------------------------------------------------
CF_IsInserted
Is a compact flash card inserted?
bool return OUT:  true if a CF card is inserted
-----------------------------------------------------------------*/
_CODE_IN_RAM bool CF_IsInserted (void) 
{
	// If running on an NDS, make sure the correct CPU can access
	// the GBA cart. First implemented by SaTa.
#ifdef NDS
 #ifdef ARM9
	WAIT_CR &= ~(0x8080);
 #endif
 #ifdef ARM7
	WAIT_CR |= (0x8080);
 #endif
#endif
	// Change register, then check if value did change
	CF_REG_STS = CF_STS_INSERTED;
	return (CF_REG_STS == CF_STS_INSERTED);
}


/*-----------------------------------------------------------------
CF_ClearStatus
Is a compact flash card inserted?
bool return OUT:  true if a CF card is inserted
-----------------------------------------------------------------*/
_CODE_IN_RAM bool CF_ClearStatus (void) 
{
	int i;
	
	// Wait until CF card is finished previous commands
	i=0;
	while ((CF_REG_CMD & CF_STS_BUSY) && (i < CARD_TIMEOUT))
	{
		i++;
	}

	// Wait until card is ready for commands
	i = 0;
	while ((!(CF_REG_STS & CF_STS_INSERTED)) && (i < CARD_TIMEOUT))
	{
		i++;
	}
	if (i >= CARD_TIMEOUT)
		return false;

	return true;
}


/*-----------------------------------------------------------------
CF_ReadSector
Read 512 byte sector numbered "sector" into "buffer"
u32 sector IN: address of 512 byte sector on CF card to read
void* buffer OUT: pointer to 512 byte buffer to store data in
bool return OUT: true if successful
-----------------------------------------------------------------*/
_CODE_IN_RAM bool CF_ReadSector (u32 sector, void* buffer)
{
	int i;
	#ifndef _CF_USE_DMA
	 u16 *buff;
	#endif
	
	// Wait until CF card is finished previous commands
	i=0;
	while ((CF_REG_CMD & CF_STS_BUSY) && (i < CARD_TIMEOUT))
	{
		i++;
	}

	// Wait until card is ready for commands
	i = 0;
	while ((!(CF_REG_STS & CF_STS_INSERTED)) && (i < CARD_TIMEOUT))
	{
		i++;
	}
	if (i >= CARD_TIMEOUT)
		return false;

	// Only read one sector
	CF_REG_SEC = 0x01;	
	
	// Set read sector
	CF_REG_LBA1 = sector & 0xFF;						// 1st byte of sector number
	CF_REG_LBA2 = (sector >> 8) & 0xFF;					// 2nd byte of sector number
	CF_REG_LBA3 = (sector >> 16) & 0xFF;				// 3rd byte of sector number
	CF_REG_LBA4 = ((sector >> 24) & 0x0F )| CF_CMD_LBA;	// last nibble of sector number
	
	// Set command to read
	CF_REG_CMD = CF_CMD_READ;
	
	// Wait until card is ready for reading
	i = 0;
	while ((CF_REG_STS != CF_STS_READY) && (i < CARD_TIMEOUT))
	{
		i++;
	}
	if (i >= CARD_TIMEOUT)
		return false;
	
	// Read data
	#ifdef _CF_USE_DMA
	 DMA3COPY ( CF_DATA, buffer, 256 | DMA16 | DMA_ENABLE | DMA_SRC_FIXED);
	#else
	 i=256;
   	 buff= (u16*)buffer;
  	 while(i--)
      	 	*buff++ = *CF_DATA; 
	#endif
	return true;
}


/*-----------------------------------------------------------------
CF_WriteSector
Write 512 byte sector numbered "sector" from "buffer"
u32 sector IN: address of 512 byte sector on CF card to read
void* buffer IN: pointer to 512 byte buffer to read data from
bool return OUT: true if successful
-----------------------------------------------------------------*/
_CODE_IN_RAM bool CF_WriteSector (u32 sector, void* buffer)
{
	int i;
	#ifndef _CF_USE_DMA
	 u16 *buff;
	#endif
	
	// Wait until CF card is finished previous commands
	i=0;
	while ((CF_REG_CMD & CF_STS_BUSY) && (i < CARD_TIMEOUT))
	{
		i++;
	}

	// Wait until card is ready for commands
	i = 0;
	while ((!(CF_REG_STS & CF_STS_INSERTED)) && (i < CARD_TIMEOUT))
	{
		i++;
	}
	if (i >= CARD_TIMEOUT)
		return false;

	// Only write one sector
	CF_REG_SEC = 0x01;	
	
	// Set write sector
	CF_REG_LBA1 = sector & 0xFF;						// 1st byte of sector number
	CF_REG_LBA2 = (sector >> 8) & 0xFF;					// 2nd byte of sector number
	CF_REG_LBA3 = (sector >> 16) & 0xFF;				// 3rd byte of sector number
	CF_REG_LBA4 = ((sector >> 24) & 0x0F )| CF_CMD_LBA;	// last nibble of sector number
	
	// Set command to write
	CF_REG_CMD = CF_CMD_WRITE;
	
	// Wait until card is ready for writing
	i = 0;
	while ((CF_REG_STS != CF_STS_READY) && (i < CARD_TIMEOUT))
	{
		i++;
	}
	if (i >= CARD_TIMEOUT)
		return false;

	// Write data
	#ifdef _CF_USE_DMA
	 DMA3COPY( buffer, CF_DATA, 256 | DMA16 | DMA_ENABLE | DMA_DST_FIXED);
	#else
	 i=256;
   	 buff= (u16*)buffer;
  	 while(i--)
      	 	*CF_DATA = *buff++; 
	#endif

	return true;
}


/*-----------------------------------------------------------------
CF_LastSector
Returns the number of the last sector accessed as an u32
Best to use this before using any other reads of CF when first 
starting program, to get the last sector where the program is stored
u32 return OUT: Address of last read sector
-----------------------------------------------------------------*/
u32 CF_LastSector (void)
{
	return ( CF_REG_LBA1 + (CF_REG_LBA2 << 8) + (CF_REG_LBA3 << 16) );
}


/*-----------------------------------------------------------------
FAT routines
-----------------------------------------------------------------*/
#define FAT_ClustToSect(m) \
	(((m-2) * discSecPerClus) + discData)

/*-----------------------------------------------------------------
FAT_NextCluster
Internal function - gets the cluster linked from input cluster
-----------------------------------------------------------------*/
u32 FAT_NextCluster(u32 cluster)
{
	u32 nextCluster;
	u32 fatMask;
	int fatOffset;
	int sector;
	int offset;

	
	if (discFAT32)
	{
		// four FAT bytes (32 bits) for every cluster
		fatOffset = cluster << 2;
		// set the FAT bit mask
		fatMask = 0x0FFFFFFF;
	}
	else
	{
		// two FAT bytes (16 bits) for every cluster
		fatOffset = cluster << 1;
		// set the FAT bit mask
		fatMask = 0xFFFF;
	}
	
	if ((cluster & fatMask) == CLUSTER_EOF || (cluster & fatMask) == CLUSTER_FREE)
		return (cluster);

	// calculate the FAT sector that we're interested in
	sector = discFAT + (fatOffset / discBytePerSec);
	// calculate offset of the our entry within that FAT sector
	offset = fatOffset % discBytePerSec;

	// If write buffer contains wrong sector
	if (sector != fatCurSector)
	{
		// If the write buffer is not empty
		if (fatWriteUpdate && (fatCurSector > 0))
		{
			// flush fat write buffer to disc
			CF_WriteSector(fatCurSector, fatWriteBuffer);
			fatWriteUpdate = false;
		}
		// Load correct sector to buffer
		fatCurSector = sector;
		CF_ReadSector(fatCurSector, fatWriteBuffer);
	}

	// read the nextCluster value
	nextCluster = (*((u32*) &(fatWriteBuffer[offset]))) & fatMask;

	// check to see if we're at the end of the chain
	if (discFAT32)
	{
		if (nextCluster >= 0x0FFFFFF7)
		{
			nextCluster = CLUSTER_EOF;
		}
	}
	else
	{
		if (nextCluster >= 0xFFF7)
		{
			nextCluster = CLUSTER_EOF;
		}
	}
	return nextCluster;
}


/*-----------------------------------------------------------------
FAT_FirstFreeCluster
Internal function - gets the first available free cluster
-----------------------------------------------------------------*/
u32 FAT_FirstFreeCluster(void)
{
	// Start at first valid cluster
	if (fatFirstFree < CLUSTER_FIRST)
		fatFirstFree = CLUSTER_FIRST;

	while ((FAT_NextCluster(fatFirstFree) != CLUSTER_FREE) && (fatFirstFree <= fatLastCluster))
	{
		fatFirstFree++;
	}
	if (fatFirstFree > fatLastCluster)
	{
		return CLUSTER_EOF;
	}
	return fatFirstFree;
}


/*-----------------------------------------------------------------
FAT_WriteFatEntry
Internal function - writes FAT information about a cluster
-----------------------------------------------------------------*/
bool FAT_WriteFatEntry (u32 cluster, int value)
{
	int fatOffset;
	u32 fatMask;

	int sector, offset;

	// Flush fatWriteBuffer to disk if requested and allowable
	if ((cluster == CLUSTER_FREE) && fatWriteUpdate && (fatCurSector > 0))
	{
		// flush fat write buffer to disc
		CF_WriteSector(fatCurSector, fatWriteBuffer);
		fatWriteUpdate = false;
		fatCurSector = 0;
		return true;
	}

	
	if ((cluster < 0x0002) || (cluster > fatLastCluster))
		return false;

	if (discFAT32)
	{
		// four FAT bytes (32 bits) for every cluster
		fatOffset = cluster << 2;
		// set the FAT bit mask
		fatMask = 0x0FFFFFFF;
	}
	else
	{
		// two FAT bytes (16 bits) for every cluster
		fatOffset = cluster << 1;
		// set the FAT bit mask
		fatMask = 0xFFFF;
	}

	value = value & fatMask;

	// calculate the FAT sector that we're interested in
	sector = discFAT + (fatOffset / discBytePerSec);
	// calculate offset of the our entry within that FAT sector
	offset = fatOffset % discBytePerSec;

	// If write buffer contains wrong sector
	if (sector != fatCurSector)
	{
		// If the write buffer is not empty
		if (fatWriteUpdate && (fatCurSector > 0))
		{
			// flush fat write buffer to disc
			CF_WriteSector(fatCurSector, fatWriteBuffer);
		}
		// Load correct sector to buffer
		fatCurSector = sector;
		CF_ReadSector(fatCurSector, fatWriteBuffer);
	}

	// Store value
	if (discFAT32)
	{
		(*((u32*) &(fatWriteBuffer[offset]))) = value;
	} 
	else
	{
		(*((u16*) &(fatWriteBuffer[offset]))) = value;
	}

	fatWriteUpdate = true;

	return true;
}


/*-----------------------------------------------------------------
FAT_LinkFreeCluster
Internal function - gets the first available free cluster, sets it
to end of file, links the input cluster to it then returns the 
cluster number
-----------------------------------------------------------------*/
u32 FAT_LinkFreeCluster(u32 cluster)
{
	u32 firstFree;
	u32 curLink;

	firstFree = FAT_FirstFreeCluster();

	// If couldn't get a free cluster then return
	if (firstFree == CLUSTER_EOF)
		return CLUSTER_FREE;

	// Link only if the current cluster is valid
	if ((cluster >= CLUSTER_FIRST) && (cluster < fatLastCluster))
	{
		curLink = FAT_NextCluster (cluster);
		if ((curLink >= CLUSTER_FIRST) && (curLink < fatLastCluster))
		{
			return curLink;	// Return the current link - don't allocate a new one
		}
		FAT_WriteFatEntry (cluster, firstFree);
	}
	FAT_WriteFatEntry (firstFree, CLUSTER_EOF);

	// Flush fat write buffer for safety
	FAT_WriteFatEntry (CLUSTER_FREE, 0);

	return firstFree;
}


/*-----------------------------------------------------------------
FAT_ClearLinks
Internal function - frees any cluster used by a file
-----------------------------------------------------------------*/
bool FAT_ClearLinks (u32 cluster)
{
	u32 nextCluster;
	
	if ((cluster == CLUSTER_EOF) || (cluster == CLUSTER_FREE))
		return false;

	nextCluster = cluster;

	do {
		cluster = nextCluster;
		nextCluster = FAT_NextCluster (cluster);
		FAT_WriteFatEntry (cluster, CLUSTER_FREE);
		if (cluster < fatFirstFree)
		{
			fatFirstFree = cluster;	// Free cluster is pushed back
		}
	} while ((cluster != CLUSTER_EOF) && (cluster != CLUSTER_FREE) && (cluster != nextCluster));


	// Flush fat write buffer
	FAT_WriteFatEntry (CLUSTER_FREE, 0);

	return true;
}


/*-----------------------------------------------------------------
FAT_InitFiles
Reads the FAT information from the CF card.
You need to call this before reading any files.
bool return OUT: true if successful.
-----------------------------------------------------------------*/
bool FAT_InitFiles (void)
{
	int i;
	int bootSector;
	BOOT_SEC* bootSec;
	
	if (!CF_IsInserted())
	{
		return (false);
	}
	
	// Read first sector of CF card
	CF_ReadSector (0, globalBuffer);
	// Check if there is a FAT string, which indicates this is a boot sector
	if ((globalBuffer[0x36] == 'F') && (globalBuffer[0x37] == 'A') && (globalBuffer[0x38] == 'T'))
	{
		bootSector = 0;
	}
	// Check for FAT32
	else if ((globalBuffer[0x52] == 'F') && (globalBuffer[0x53] == 'A') && (globalBuffer[0x54] == 'T'))
	{
		bootSector = 0;
	}
	else	// This is an MBR
	{
		// Find first valid partition from MBR
		for (i=0x1BE; (i < 0x1FE) && (globalBuffer[i] != 0x80) && (globalBuffer[i+0x04] == 0x00); i+= 0x10);
		// Go to first valid partition
		bootSector = globalBuffer[0x8 + i] + (globalBuffer[0x9 + i] << 8) + (globalBuffer[0xA + i] << 16) + ((globalBuffer[0xB + i] << 24) & 0x0F);
	}

	// Read in boot sector
	bootSec = (BOOT_SEC*) globalBuffer;
	CF_ReadSector (bootSector,  bootSec);
	
	// Store required information about the file system
	if (bootSec->sectorsPerFAT != 0)
	{
		discSecPerFAT = bootSec->sectorsPerFAT;
	}
	else
	{
		discSecPerFAT = bootSec->extBlock.fat32.sectorsPerFAT32;
	}
	
	if (bootSec->numSectorsSmall != 0)
	{
		discNumSec = bootSec->numSectorsSmall;
	}
	else
	{
		discNumSec = bootSec->numSectors;
	}

	discBytePerSec = BYTE_PER_READ;	// Sector size is redefined to be 512 bytes
	discSecPerClus = bootSec->sectorsPerCluster * bootSec->bytesPerSector / BYTE_PER_READ;
	discBytePerClus = discBytePerSec * discSecPerClus;
	discFAT = bootSector + bootSec->reservedSectors;

	discRootDir = discFAT + (bootSec->numFATs * discSecPerFAT);
	discData = discRootDir + ((bootSec->rootEntries * sizeof(DIR_ENT)) / discBytePerSec);

	// Store info about FAT
	fatLastCluster = (discNumSec - discData) / bootSec->sectorsPerCluster;
	fatFirstFree = CLUSTER_FIRST;
	fatCurSector = 0;
	fatWriteUpdate = false;

	if (fatLastCluster < 4085)
	{
		return false;	// FAT12 volume - unsupported
	}
	else if (fatLastCluster < 65525)
	{
		discFAT32 = false;	// FAT16 volume
	}
	else
	{
		discFAT32 = true;	// FAT32 volume
	}

	if (!discFAT32)
	{
		discRootDirClus = FAT16_ROOT_DIR_CLUSTER;
	}
	else	// Set up for the FAT32 way
	{
		discRootDirClus = bootSec->extBlock.fat32.rootClus;
		// Check if FAT mirroring is enabled
		if (!(bootSec->extBlock.fat32.extFlags & 0x80))
		{
			// Use the active FAT
			discFAT = discFAT + ( discSecPerFAT * (bootSec->extBlock.fat32.extFlags & 0x0F));
		}
	}

	// Set current directory to the root
	curWorkDirCluster = discRootDirClus;
	wrkDirCluster = discRootDirClus;
	wrkDirSector = 0;
	wrkDirOffset = 0;

	// Set all files to free
	for (i=0; i < MAX_FILES_OPEN; i++)
	{
		openFiles[i].inUse = false;
	}

	// No long filenames so far
	lfnExists = false;
	for (i = 0; i < MAX_FILENAME_LENGTH; i++)
	{
		lfnName[i] = '\0';
	}

	return (true);
}

/*-----------------------------------------------------------------
FAT_FreeFiles
Closes all open files then resets the CF card.
Call this before exiting back to the GBAMP
bool return OUT: true if successful.
-----------------------------------------------------------------*/
bool FAT_FreeFiles (void)
{
	int i;

	// Close all open files
	for (i=0; i < MAX_FILES_OPEN; i++)
	{
		if (openFiles[i].inUse == true)
		{
			FAT_fclose(i);
		}
	}

	// Clear card status
	CF_ClearStatus();

	// Return status of card
	return CF_IsInserted();
}


/*-----------------------------------------------------------------
FAT_GetDirEntry
Return the file info structure of the next valid file entry
u32 dirCluster: IN cluster of subdirectory table
int entry: IN the desired file entry
int origin IN: relative position of the entry
DIR_ENT return OUT: desired dirEntry. First char will be FILE_FREE if 
	the entry does not exist.
-----------------------------------------------------------------*/
DIR_ENT FAT_GetDirEntry ( u32 dirCluster, int entry, int origin)
{
	DIR_ENT dir;
	DIR_ENT_LFN lfn;
	int firstSector = 0;
	bool notFound = false;
	bool found = false;
	int maxSectors;
	int lfnPos, aliasPos;
	u8 lfnChkSum, chkSum;

	dir.name[0] = FILE_FREE; // default to no file found

	// Check if fat has been initialised
	if (discBytePerSec == 0)
	{
		return (dir);
	}
	
	switch (origin) 
	{
	case SEEK_SET:
		wrkDirCluster = dirCluster;
		wrkDirSector = 0;
		wrkDirOffset = -1;
		break;
	case SEEK_CUR:	// Don't change anything
		break;
	case SEEK_END:	// Find entry signifying end of directory
		// Subtraction will never reach 0, so it keeps going 
		// until reaches end of directory
		wrkDirCluster = dirCluster;
		wrkDirSector = 0;
		wrkDirOffset = -1;
		entry = -1;
		break;
	default:
		return dir;
	}

	lfnChkSum = 0;
	maxSectors = (wrkDirCluster == FAT16_ROOT_DIR_CLUSTER ? (discData - discRootDir) : discSecPerClus);

	// Scan Dir for correct entry
	firstSector = (wrkDirCluster == FAT16_ROOT_DIR_CLUSTER ? discRootDir : FAT_ClustToSect(wrkDirCluster));
	CF_ReadSector (firstSector + wrkDirSector, globalBuffer);
	found = false;
	notFound = false;
	do {
		wrkDirOffset++;
		if (wrkDirOffset == BYTE_PER_READ / sizeof (DIR_ENT))
		{
			wrkDirOffset = 0;
			wrkDirSector++;
			if ((wrkDirSector == discSecPerClus) && (wrkDirCluster != FAT16_ROOT_DIR_CLUSTER))
			{
				wrkDirSector = 0;
				wrkDirCluster = FAT_NextCluster(wrkDirCluster);
				if (wrkDirCluster == CLUSTER_EOF)
				{
					notFound = true;
				}
				firstSector = FAT_ClustToSect(wrkDirCluster);		
			}
			else if ((wrkDirCluster == FAT16_ROOT_DIR_CLUSTER) && (wrkDirSector == (discData - discRootDir)))
			{
				notFound = true;	// Got to end of root dir
			}
			CF_ReadSector (firstSector + wrkDirSector, globalBuffer);
		}
		dir = ((DIR_ENT*) globalBuffer)[wrkDirOffset];
		if ((dir.name[0] != FILE_FREE) && (dir.name[0] > 0x20) && ((dir.attrib & ATTRIB_VOL) != ATTRIB_VOL))
		{
			entry--;
			if (lfnExists)
			{
				// Calculate file checksum
				chkSum = 0;
				for (aliasPos=0; aliasPos < 11; aliasPos++)
				{
					// NOTE: The operation is an unsigned char rotate right
					chkSum = ((chkSum & 1) ? 0x80 : 0) + (chkSum >> 1) + (aliasPos < 8 ? dir.name[aliasPos] : dir.ext[aliasPos - 8]);
				}
				if (chkSum != lfnChkSum)
				{
					lfnExists = false;
					lfnName[0] = '\0';
				}
			}
			if (entry == 0) 
			{
				if (!lfnExists)
				{
					FAT_GetFilename (dir, lfnName);
				}
				found = true;
			}
		}
		else if (dir.name[0] == FILE_LAST)
		{
			if (origin == SEEK_END)
			{
				found = true;
			}
			else
			{
				notFound = true;
			}
		}
		else if (dir.attrib == ATTRIB_LFN)
		{
			lfn = ((DIR_ENT_LFN*) globalBuffer)[wrkDirOffset];
			if (lfn.ordinal & LFN_END)
			{
				lfnExists = true;
				lfnName[(lfn.ordinal & ~LFN_END) * 13] = '\0';	// Set end of lfn to null character
				lfnChkSum = lfn.checkSum;
			}
			if (lfn.ordinal & LFN_DEL)
			{
				lfnExists = false;
			}
			if (lfnChkSum != lfn.checkSum)
			{
				lfnExists = false;
			}
			if (lfnExists)
			{
				lfnPos = ((lfn.ordinal & ~LFN_END) - 1) * 13;
				lfnName[lfnPos + 0] = lfn.char0 & 0xFF;
				lfnName[lfnPos + 1] = lfn.char1 & 0xFF;
				lfnName[lfnPos + 2] = lfn.char2 & 0xFF;
				lfnName[lfnPos + 3] = lfn.char3 & 0xFF;
				lfnName[lfnPos + 4] = lfn.char4 & 0xFF;
				lfnName[lfnPos + 5] = lfn.char5 & 0xFF;
				lfnName[lfnPos + 6] = lfn.char6 & 0xFF;
				lfnName[lfnPos + 7] = lfn.char7 & 0xFF;
				lfnName[lfnPos + 8] = lfn.char8 & 0xFF;
				lfnName[lfnPos + 9] = lfn.char9 & 0xFF;
				lfnName[lfnPos + 10] = lfn.char10 & 0xFF;
				lfnName[lfnPos + 11] = lfn.char11 & 0xFF;
				lfnName[lfnPos + 12] = lfn.char12 & 0xFF;
			}
		}
	} while (!found && !notFound);
	
	// If no file is found, return FILE_FREE
	if (notFound)
	{
		dir.name[0] = FILE_FREE;
	}

	return (dir);
}


/*-----------------------------------------------------------------
FAT_GetLongFilename
Get the long name of the last file or directory retrived with 
	GetDirEntry. Also works for FindFirstFile and FindNextFile
char* filename: OUT will be filled with the filename, should be at
	least 256 bytes long
bool return OUT: return true if successful
-----------------------------------------------------------------*/
bool FAT_GetLongFilename (char* filename)
{
	int i;
	if (filename == NULL)
		return false;
	for (i =0; (i < MAX_FILENAME_LENGTH - 1) && (lfnName[i] != '\0'); i++)
	{
		filename[i] = lfnName[i];
	}
	filename[i] = '\0';
	if (!lfnExists)
	{
		return false;
	}
	else
	{
		return true;
	}
}


/*-----------------------------------------------------------------
FAT_GetFilename
Get the alias (short name) of the file or directory stored in 
	dirEntry
DIR_ENT dirEntry: IN a valid directory table entry
char* alias OUT: will be filled with the alias (short filename),
	should be at least 13 bytes long
bool return OUT: return true if successful
-----------------------------------------------------------------*/
bool FAT_GetFilename (DIR_ENT dirEntry, char* alias)
{
	int i=0;
	int j=0;

	alias[0] = '\0';
	if (dirEntry.name[0] != FILE_FREE)
	{
		if (dirEntry.name[0] == '.')
		{
			alias[0] = '.';
			if (dirEntry.name[1] == '.')
			{
				alias[1] = '.';
				alias[2] = '\0';
			}
			else
			{
				alias[1] = '\0';
			}
		}
		else
		{		
			// Copy the filename from the dirEntry to the string
			for (i = 0; (i < 8) && (dirEntry.name[i] != ' '); i++)
			{
				alias[i] = dirEntry.name[i];
			}
			// Copy the extension from the dirEntry to the string
			if (dirEntry.ext[0] != ' ')
			{
				alias[i++] = '.';
				for ( j = 0; (j < 3) && (dirEntry.ext[j] != ' '); j++)
				{
					alias[i++] = dirEntry.ext[j];
				}
			}
			alias[i] = '\0';
		}
	}

	return (alias[0] != '\0');
}

/*-----------------------------------------------------------------
FAT_GetAlias
Get the alias (short name) of the last file or directory entry read
	using GetDirEntry. Works for FindFirstFile and FindNextFile
char* alias OUT: will be filled with the alias (short filename),
	should be at least 13 bytes long
bool return OUT: return true if successful
-----------------------------------------------------------------*/
bool FAT_GetAlias (char* alias)
{
	if (alias == NULL)
	{
		return false;
	}
	// Read in the last accessed directory entry
	CF_ReadSector ((wrkDirCluster == FAT16_ROOT_DIR_CLUSTER ? discRootDir : FAT_ClustToSect(wrkDirCluster)) + wrkDirSector, globalBuffer);
	
	return 	FAT_GetFilename (((DIR_ENT*)globalBuffer)[wrkDirOffset], alias);
}

/*-----------------------------------------------------------------
FAT_GetFileSize
Get the file size of the last file found or openned.
This idea is based on a modification by MoonShine
u32 return OUT: the file size
-----------------------------------------------------------------*/
u32 FAT_GetFileSize (void)
{
	// Read in the last accessed directory entry
	CF_ReadSector ((wrkDirCluster == FAT16_ROOT_DIR_CLUSTER ? discRootDir : FAT_ClustToSect(wrkDirCluster)) + wrkDirSector, globalBuffer);
	
	return 	((DIR_ENT*)globalBuffer)[wrkDirOffset].fileSize;
}

/*-----------------------------------------------------------------
FAT_GetFileCluster
Get the first cluster of the last file found or openned.
u32 return OUT: the file start cluster
-----------------------------------------------------------------*/
u32 FAT_GetFileCluster (void)
{
	// Read in the last accessed directory entry
	CF_ReadSector ((wrkDirCluster == FAT16_ROOT_DIR_CLUSTER ? discRootDir : FAT_ClustToSect(wrkDirCluster)) + wrkDirSector, globalBuffer);
	
	return 	(((DIR_ENT*)globalBuffer)[wrkDirOffset].startCluster) | (((DIR_ENT*)globalBuffer)[wrkDirOffset].startClusterHigh << 16);
}



/*-----------------------------------------------------------------
FAT_DirEntFromPath
Finds the directory entry for a file or directory from a path
Path separator is a forward slash /
const char* path: IN null terminated string of path.
DIR_ENT return OUT: dirEntry of found file. First char will be FILE_FREE
	if the file was not found
-----------------------------------------------------------------*/
DIR_ENT FAT_DirEntFromPath (const char* path)
{
	int pathPos;
	char name[MAX_FILENAME_LENGTH];
	char alias[13];
	int namePos;
	bool found, notFound;
	DIR_ENT dirEntry;
	u32 dirCluster;
	bool flagLFN;
	
	// Start at beginning of path
	pathPos = 0;
	
	if (path[pathPos] == '/') 
	{
		dirCluster = discRootDirClus;	// Start at root directory
	}
	else
	{
		dirCluster = curWorkDirCluster;	// Start at current working dir
	}
	
	// Eat any slash /
	while ((path[pathPos] == '/') && (path[pathPos] != '\0'))
	{
		pathPos++;
	}
	
	// Search until can't continue
	found = false;
	notFound = false;
	while (!notFound && !found)
	{
		flagLFN = false;
		// Copy name from path
		namePos = 0;
		if (path[pathPos] == '.')
		{
			// Dot entry or double dot entry
			name[namePos] = '.';
			namePos++;
			pathPos++;
			if (path[pathPos] == '.')
			{
				name[namePos] = '.';
				namePos++;
				pathPos++;
			}
		}
		else
		{
			// Copy name from path
			namePos = 0;
			while ((namePos < MAX_FILENAME_LENGTH - 1) && (path[pathPos] != '\0') && (path[pathPos] != '/'))
			{
				name[namePos] = ucase(path[pathPos]);
				if ((name[namePos] <= ' ') || ((name[namePos] >= ':') && (name[namePos] <= '?'))) // Invalid character
				{
					flagLFN = true;
				}
				namePos++;
				pathPos++;
			}
			// Check if a long filename was specified
			if (namePos > 12)
			{
				flagLFN = true;
			}
		}
		
		// Add end of string char
		name[namePos] = '\0';

		// Move through path to correct place
		while ((path[pathPos] != '/') && (path[pathPos] != '\0'))
			pathPos++;
		// Eat any slash /
		while ((path[pathPos] == '/') && (path[pathPos] != '\0'))
		{
			pathPos++;
		}

		// Search current Dir for correct entry
		dirEntry = FAT_GetDirEntry (dirCluster, 1, SEEK_SET);
		while ( !found && !notFound)
		{
			// Match filename
			found = true;
			for (namePos = 0; (namePos < MAX_FILENAME_LENGTH) && found && (name[namePos] != '\0') && (lfnName[namePos] != '\0'); namePos++)
			{
				if (name[namePos] != ucase(lfnName[namePos]))
				{
					found = false;
				}
			}
			if ((name[namePos] == '\0') != (lfnName[namePos] == '\0'))
			{
				found = false;
			}

			// Check against alias as well.
			if (!found)
			{
				FAT_GetFilename(dirEntry, alias);
				found = true;
				for (namePos = 0; (namePos < 13) && found && (name[namePos] != '\0') && (alias[namePos] != '\0'); namePos++)
				{
					if (name[namePos] != ucase(alias[namePos]))
					{
						found = false;
					}
				}
				if ((name[namePos] == '\0') != (alias[namePos] == '\0'))
				{
					found = false;
				}
			}

			if (dirEntry.name[0] == FILE_FREE)
				// Couldn't find specified file
			{
				found = false;
				notFound = true;
			}
			if (!found && !notFound)
			{
				dirEntry = FAT_GetDirEntry (dirCluster, 1, SEEK_CUR);
			}
		}
		
		if (found && ((dirEntry.attrib & ATTRIB_DIR) == ATTRIB_DIR) && (path[pathPos] != '\0'))
			// It has found a directory from within the path that needs to be followed
		{
			found = false;
			dirCluster = dirEntry.startCluster | (dirEntry.startClusterHigh << 16);
		}
	}
	
	if (notFound)
	{
		dirEntry.name[0] = FILE_FREE;
		dirEntry.attrib = 0x00;
	}

	return (dirEntry);
}


/*-----------------------------------------------------------------
FAT_AddDirEntry
Creates a new dir entry for a file
Path separator is a forward slash /
const char* path: IN null terminated string of path to file.
DIR_ENT newDirEntry IN: The directory entry to use.
int file IN: The file being added (optional, use -1 if not used)
bool return OUT: true if successful
-----------------------------------------------------------------*/
bool FAT_AddDirEntry (const char* path, DIR_ENT newDirEntry)
{
	char filename[MAX_FILENAME_LENGTH];
	int filePos, pathPos, aliasPos;
	char tempChar;
	bool flagLFN;
	char fileAlias[13];
	int tailNum;
	
	unsigned char chkSum = 0;
	
	u32 oldWorkDirCluster;
	
	DIR_ENT* dirEntries = (DIR_ENT*)globalBuffer;
	u32 dirCluster;
	int secOffset;
	int entryOffset;
	int maxSectors;
	u32 firstSector;

	DIR_ENT_LFN lfnEntry;
	int lfnPos = 0;
	
	// Store current working directory
	oldWorkDirCluster = curWorkDirCluster;

	// Find filename within path and change to correct directory
	if (path[0] == '/')
	{
		curWorkDirCluster = discRootDirClus;
	}
	
	pathPos = 0;
	filePos = 0;
	flagLFN = false;

	while (path[pathPos + filePos] != '\0')
	{
		if (path[pathPos + filePos] == '/')
		{
			filename[filePos] = '\0';
			FAT_CWD(filename);
			pathPos += filePos + 1;
			filePos = 0;
		}
		filename[filePos] = path[pathPos + filePos];
		filePos++;
	}
	
	// Skip over last slashes
	while (path[pathPos] == '/')
		pathPos++;
	
	// Copy name from path
	filePos = 0;
	while ((filePos < MAX_FILENAME_LENGTH - 1) && (path[pathPos] != '\0'))
	{
		filename[filePos] = path[pathPos];
		if ((filename[filePos] <= ' ') || ((filename[filePos] >= ':') && (filename[filePos] <= '?'))) // Invalid character
		{
			flagLFN = true;
		}
		filePos++;
		pathPos++;
	}
	
	// Check if a long filename was specified
	if (filePos > 12)
	{
		flagLFN = true;
	}
	
	if (filePos == 0)	// No filename
	{
		return false;
	}
	
	lfnPos = (filePos - 1) / 13;

	// Add end of string char
	filename[filePos++] = '\0';
	// Clear remaining chars
	while (filePos < MAX_FILENAME_LENGTH)
		filename[filePos++] = 0x01;	// Set for LFN compatibility
	
	
	if (flagLFN)
	{
		// Generate short filename - always a 2 digit number for tail
		// Get first 5 chars of alias from LFN
		aliasPos = 0;
		for (filePos = 0; (aliasPos < 5) && (filename[filePos] != '\0') ; filePos++)
		{
			tempChar = ucase(filename[filePos]);
			if ((tempChar > ' ' && tempChar < ':') || tempChar > '?')
				fileAlias[aliasPos++] = tempChar;
		}
		// Pad Alias with underscores
		while (aliasPos < 5)
			fileAlias[aliasPos++] = '_';
		
		fileAlias[5] = '~';
		fileAlias[8] = '.';
		fileAlias[9] = ' ';
		fileAlias[10] = ' ';
		fileAlias[11] = ' ';
		while(filename[filePos] != '\0')
		{
			filePos++;
			if (filename[filePos] == '.')
			{
				pathPos = filePos;
			}
		}
		filePos = pathPos + 1;	//pathPos is used as a temporary variable
		// Copy first 3 characters of extension
		for (aliasPos = 9; (aliasPos < 12) && (filename[filePos] != '\0'); filePos++)
		{
			tempChar = ucase(filename[filePos]);
			if ((tempChar > ' ' && tempChar < ':') || tempChar > '?')
				fileAlias[aliasPos++] = tempChar;
		}
		
		// Pad Alias extension with spaces
		while (aliasPos < 12)
			fileAlias[aliasPos++] = ' ';
		
		fileAlias[12] = '\0';
		
		
		// Get a valid tail number
		tailNum = 0;
		do {
			tailNum++;
			fileAlias[6] = 0x30 + ((tailNum / 10) % 10);	// 10's digit
			fileAlias[7] = 0x30 + (tailNum % 10);	// 1's digit
		} while ((FAT_DirEntFromPath(fileAlias).name[0] != FILE_FREE) && (tailNum < 100));
		
		if (tailNum < 100)	// Found an alias not being used
		{
			// Calculate file checksum
			chkSum = 0;
			for (aliasPos=0; aliasPos < 12; aliasPos++)
			{
				// Skip '.'
				if (fileAlias[aliasPos] == '.')
					aliasPos++;
				// NOTE: The operation is an unsigned char rotate right
				chkSum = ((chkSum & 1) ? 0x80 : 0) + (chkSum >> 1) + fileAlias[aliasPos];
			}
		}
		else	// Couldn't find a valid alias
		{
			return false;
		}
	}
	else	// Its not a long file name
	{
		// Just copy alias straight from filename
		for (aliasPos = 0; aliasPos < 13; aliasPos++)
		{
			tempChar = ucase(filename[aliasPos]);
			if ((tempChar > ' ' && tempChar < ':') || tempChar > '?')
				fileAlias[aliasPos] = tempChar;
		}
		fileAlias[12] = '\0';
	}
	
	// Change dirEntry name to match alias
	for (aliasPos = 0; ((fileAlias[aliasPos] != '.') && (fileAlias[aliasPos] != '\0') && (aliasPos < 8)); aliasPos++)
	{
		newDirEntry.name[aliasPos] = fileAlias[aliasPos];
	}
	while (aliasPos < 8)
	{
		newDirEntry.name[aliasPos++] = ' ';
	}
	aliasPos = 0;
	while ((fileAlias[aliasPos] != '.') && (fileAlias[aliasPos] != '\0'))
		aliasPos++;
	filePos = 0;
	while (( filePos < 3 ) && (fileAlias[aliasPos] != '\0'))
	{
		tempChar = fileAlias[aliasPos++];
		if ((tempChar > ' ' && tempChar < ':' && tempChar!='.') || tempChar > '?')
			newDirEntry.ext[filePos++] = tempChar;
	}
	while (filePos < 3)
	{
		newDirEntry.ext[filePos++] = ' ';
	}

	// Scan Dir for free entry
	dirCluster = curWorkDirCluster;
	secOffset = 0;
	entryOffset = 0;
	maxSectors = (dirCluster == FAT16_ROOT_DIR_CLUSTER ? (discData - discRootDir) : discSecPerClus);
	firstSector = (dirCluster == FAT16_ROOT_DIR_CLUSTER ? discRootDir : FAT_ClustToSect(dirCluster));
	CF_ReadSector (firstSector + secOffset, dirEntries);
	
	// If we are adding a long file name, then we need to add it to the end of the directory to make it fit
	while (((dirEntries[entryOffset].name[0] != FILE_FREE) && (dirEntries[entryOffset].name[0] != FILE_LAST) && !flagLFN) || ((dirEntries[entryOffset].name[0] != FILE_LAST) && flagLFN))
	{
		entryOffset++;
		if (entryOffset == BYTE_PER_READ / sizeof (DIR_ENT))
		{
			entryOffset = 0;
			secOffset++;
			if ((secOffset == discSecPerClus) && (dirCluster != FAT16_ROOT_DIR_CLUSTER))
			{
				secOffset = 0;
				if (FAT_NextCluster(dirCluster) == CLUSTER_EOF)
				{
					dirCluster = FAT_LinkFreeCluster(dirCluster);
					dirEntries[0].name[0] = FILE_LAST;
				}
				else
				{
					dirCluster = FAT_NextCluster(dirCluster);
				}
				firstSector = FAT_ClustToSect(dirCluster);		
			}
			else if ((dirCluster == FAT16_ROOT_DIR_CLUSTER) && (secOffset == (discData - discRootDir)))
			{
				return false;	// Got to end of root dir - can't fit in more files
			}
			if (dirEntries[entryOffset].name[0] != FILE_LAST)
			{
				CF_ReadSector (firstSector + secOffset, dirEntries);
			}
		}
	}
	
	// Add new directory entry
	if (dirEntries[entryOffset].name[0] == FILE_LAST)	// Have to add it to end
	{
		if (flagLFN)
		{
			// Generate LFN entries
			lfnEntry.ordinal = LFN_END;
			while (lfnPos >= 0)
			{
				lfnEntry.ordinal |= lfnPos + 1;
				lfnEntry.char0 = filename [lfnPos * 13 + 0];
				lfnEntry.char1 = (filename [lfnPos * 13 + 1] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 1]);
				lfnEntry.char2 = (filename [lfnPos * 13 + 2] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 2]);
				lfnEntry.char3 = (filename [lfnPos * 13 + 3] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 3]);
				lfnEntry.char4 = (filename [lfnPos * 13 + 4] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 4]);
				lfnEntry.char5 = (filename [lfnPos * 13 + 5] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 5]);
				lfnEntry.char6 = (filename [lfnPos * 13 + 6] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 6]);
				lfnEntry.char7 = (filename [lfnPos * 13 + 7] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 7]);
				lfnEntry.char8 = (filename [lfnPos * 13 + 8] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 8]);
				lfnEntry.char9 = (filename [lfnPos * 13 + 9] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 9]);
				lfnEntry.char10 = (filename [lfnPos * 13 + 10] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 10]);
				lfnEntry.char11 = (filename [lfnPos * 13 + 11] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 11]);
				lfnEntry.char12 = (filename [lfnPos * 13 + 12] == 0x01 ? 0xFFFF : filename [lfnPos * 13 + 12]);
				lfnEntry.checkSum = chkSum;
				lfnEntry.flag = ATTRIB_LFN;
				lfnEntry.reserved1 = 0;
				lfnEntry.reserved2 = 0;

				*((DIR_ENT_LFN*)&dirEntries[entryOffset]) = lfnEntry;
				lfnPos --;
				lfnEntry.ordinal = 0;
				// Move to next entry
				entryOffset++;
				if (entryOffset == BYTE_PER_READ / sizeof (DIR_ENT))
				{
					// Write out the current sector if we need to
					entryOffset = 0;
					CF_WriteSector (firstSector + secOffset, dirEntries);
					secOffset++;
					if ((secOffset == discSecPerClus) && (dirCluster != FAT16_ROOT_DIR_CLUSTER))
					{
						secOffset = 0;
						if (FAT_NextCluster(dirCluster) == CLUSTER_EOF)
						{
							dirCluster = FAT_LinkFreeCluster(dirCluster);
							dirEntries[0].name[0] = FILE_LAST;
						}
						else
						{
							dirCluster = FAT_NextCluster(dirCluster);
						}
						firstSector = FAT_ClustToSect(dirCluster);		
					}
					else if ((dirCluster == FAT16_ROOT_DIR_CLUSTER) && (secOffset == (discData - discRootDir)))
					{
						return false;	// Got to end of root dir - can't fit in more files
					}
					CF_ReadSector (firstSector + secOffset, dirEntries);
				}
			}
		}	// end writing long filename entries

		dirEntries[entryOffset] = newDirEntry;

		entryOffset++;
		if (entryOffset != BYTE_PER_READ / sizeof (DIR_ENT))
			dirEntries[entryOffset].name[0] = FILE_LAST;
		// Write entries
		CF_WriteSector (firstSector + secOffset, dirEntries);
		
		// Move last file to next entry
		if ((dirCluster != FAT16_ROOT_DIR_CLUSTER) && (entryOffset == BYTE_PER_READ / sizeof (DIR_ENT)))
		{
			entryOffset = 0;
			secOffset++;
			if (secOffset == discSecPerClus)
			{
				secOffset = 0;
				if (FAT_NextCluster(dirCluster) == CLUSTER_EOF)
				{
					dirCluster = FAT_LinkFreeCluster(dirCluster);
					dirEntries[0].name[0] = FILE_LAST;
				}
				else
				{
					dirCluster = FAT_NextCluster(dirCluster);
				}				
				/*
				dirCluster = FAT_NextCluster(dirCluster);
				if (dirCluster == CLUSTER_EOF)
				{
					dirCluster = FAT_LinkFreeCluster(dirCluster);
				}
				*/
				firstSector = FAT_ClustToSect(dirCluster);		
			}
			dirEntries[entryOffset].name[0] = FILE_LAST;
			// Write new dir sector
			CF_WriteSector (firstSector + secOffset, dirEntries);
		}
	}
	else	// Can fit new entry in amongst old ones
	{
		dirEntries[entryOffset] = newDirEntry;
		// Write entry amongst the rest of the directory
		CF_WriteSector (firstSector + secOffset, dirEntries);
	}

	// Change back to Working DIR
	curWorkDirCluster = oldWorkDirCluster;

	// Flush fat write buffer
	FAT_WriteFatEntry (CLUSTER_FREE, 0);

	return true;
}



/*-----------------------------------------------------------------
FAT_FindNextFile
Gets the name of the next directory entry
	(can be a file or subdirectory)
char* filename: OUT filename, must be at least 13 chars long
int return OUT: returns 0 if failed, 1 if it found a file and 2 if 
	it found a directory
-----------------------------------------------------------------*/
int FAT_FindNextFile(char* filename)
{
	// Get the next directory entry
	DIR_ENT file;
	file = FAT_GetDirEntry (curWorkDirCluster, 1, SEEK_CUR);

	if (file.name[0] == FILE_FREE)
	{
		return DIR_END;	// Did not find a file
	}

	// Get the filename
	if (filename != NULL)
		FAT_GetFilename (file, filename);

	if ((file.attrib & ATTRIB_DIR) != 0)
	{
		return DIR_DIR;	// Found a directory
	}
	else
	{
		return DIR_FILE;	// Found a file
	}
}

/*-----------------------------------------------------------------
FAT_FindFirstFile
Gets the name of the first directory entry and resets the count
	(can be a file or subdirectory)
char* filename: OUT filename, must be at least 13 chars long
int return: OUT returns 0 if failed, 1 if it found a file and 2 if 
	it found a directory
-----------------------------------------------------------------*/
int FAT_FindFirstFile(char* filename)
{
	// Get the first directory entry
	DIR_ENT file;
	file = FAT_GetDirEntry (curWorkDirCluster, 1, SEEK_SET);
	
	if (file.name[0] == FILE_FREE)
	{
		return DIR_END;	// Did not find a file
	}

	// Get the filename
	if (filename != NULL)
		FAT_GetFilename (file, filename);

	if ((file.attrib & ATTRIB_DIR) != 0)
	{
		return DIR_DIR;	// Found a directory
	}
	else
	{
		return DIR_FILE;	// Found a file
	}
}


/*-----------------------------------------------------------------
FAT_CWD
Changes the current working directory
const char* path: IN null terminated string of directory separated by 
	forward slashes, / is root
bool return: OUT returns true if successful
-----------------------------------------------------------------*/
bool FAT_CWD (const char* path)
{
	DIR_ENT dir;
	if (path[0] == '/' && path[1] == '\0')
	{
		curWorkDirCluster = discRootDirClus;
		return true;
	}
	if (path[0] == '\0')	// Return true if changing relative to nothing
	{
		return true;
	}
	
	dir = FAT_DirEntFromPath (path);

	if (((dir.attrib & ATTRIB_DIR) == ATTRIB_DIR) && (dir.name[0] != FILE_FREE))
	{
		// Change directory
		curWorkDirCluster = dir.startCluster | (dir.startClusterHigh << 16);

		// Move to correct cluster for root directory
		if (curWorkDirCluster == FAT16_ROOT_DIR_CLUSTER)
		{
			curWorkDirCluster = discRootDirClus;
		}

		// Reset file position in directory
		wrkDirCluster = curWorkDirCluster;
		wrkDirSector = 0;
		wrkDirOffset = -1;
		return true;
	}
	else
	{ 
		// Couldn't change directory - wrong path specified
		return false;
	}
}

/*-----------------------------------------------------------------
FAT_fopen(filename, mode)
Opens a file
const char* path: IN null terminated string of filename and path 
	separated by forward slashes, / is root
const char* mode: IN mode to open file in
	Supported modes: "r", "r+", "w", "w+", "a", "a+", don't use
	"b" or "t" in any mode, as all files are openned in binary mode
int return: OUT handle to open file, returns -1 if the file 
	couldn't be openned
-----------------------------------------------------------------*/
int FAT_fopen(const char* path, const char* mode)
{
	int file;
	DIR_ENT dirEntry;
	u32 startCluster;
	int clusCount;

		
	// Get the dirEntry for the path specified
	dirEntry = FAT_DirEntFromPath (path);
	
	// Check that it is not a directory
	if (dirEntry.attrib & ATTRIB_DIR)
	{
		return -1;
	}
	
	// Find a free file buffer
	for (file = 0; (file < MAX_FILES_OPEN) && (openFiles[file].inUse == true); file++);
	
	if (file == MAX_FILES_OPEN) // No free files
	{
		return -1;
	}

	// Remember where directory entry was
	openFiles[file].dirEntSector = (wrkDirCluster == FAT16_ROOT_DIR_CLUSTER ? discRootDir : FAT_ClustToSect(wrkDirCluster)) + wrkDirSector;
	openFiles[file].dirEntOffset = wrkDirOffset;

	if (ucase(mode[0]) == 'R')
	{
		if (dirEntry.name[0] == FILE_FREE)	// File must exist
		{
			return -1;
		}
		
		openFiles[file].read = true;
		openFiles[file].write = (mode[1] == '+');
		openFiles[file].append = false;
		
		// Store information about position within the file, for use
		// by FAT_fread, FAT_fseek, etc.
		openFiles[file].firstCluster = dirEntry.startCluster | (dirEntry.startClusterHigh << 16);
	
		// Check if file is openned for random. If it is, and currently has no cluster, one must be 
		// assigned to it.
		if (openFiles[file].write && openFiles[file].firstCluster == CLUSTER_FREE)
		{
			openFiles[file].firstCluster = FAT_LinkFreeCluster (CLUSTER_FREE);
			if (openFiles[file].firstCluster == CLUSTER_FREE)	// Couldn't get a free cluster
			{
				return -1;
			}
			// Flush fat write buffer
			FAT_WriteFatEntry (CLUSTER_FREE, 0);

			// Store cluster position into the directory entry
			dirEntry.startCluster = (openFiles[file].firstCluster & 0xFFFF);
			dirEntry.startClusterHigh = ((openFiles[file].firstCluster >> 16) & 0xFFFF);
			CF_ReadSector (openFiles[file].dirEntSector, globalBuffer);
			((DIR_ENT*) globalBuffer)[openFiles[file].dirEntOffset] = dirEntry;
			CF_WriteSector (openFiles[file].dirEntSector, globalBuffer);
		}
			
		openFiles[file].length = dirEntry.fileSize;
		openFiles[file].curPos = 0;
		openFiles[file].curClus = dirEntry.startCluster | (dirEntry.startClusterHigh << 16);
		openFiles[file].curSect = 0;
		openFiles[file].curByte = 0;

		// Not appending
		openFiles[file].appByte = 0;
		openFiles[file].appClus = 0;
		openFiles[file].appSect = 0;
	
		CF_ReadSector( FAT_ClustToSect( openFiles[file].curClus), openFiles[file].readBuffer);
		openFiles[file].inUse = true;	// We're using this file now

		return file;
	}	// mode "r"

	if (ucase(mode[0]) == 'W')
	{
		if (dirEntry.name[0] == FILE_FREE)	// Create file if it doesn't exist
		{
			dirEntry.attrib = ATTRIB_ARCH;
			dirEntry.reserved = 0;
			
			// Time and date set to 'origin of computer universe'
			dirEntry.cTime_ms = 0;
			dirEntry.cTime = 0;
			dirEntry.cDate = 0;
			dirEntry.aDate = 0;
			dirEntry.mTime = 0;
			dirEntry.mDate = 0;
		}
		else	// Already a file entry 
		{
			// Free any clusters used
			FAT_ClearLinks (dirEntry.startCluster | (dirEntry.startClusterHigh << 16));
		}
		
		// Get a cluster to use
		startCluster = FAT_LinkFreeCluster (CLUSTER_FREE);
		if (startCluster == CLUSTER_FREE)	// Couldn't get a free cluster
		{
			return -1;
		}
		// Flush fat write buffer
		FAT_WriteFatEntry (CLUSTER_FREE, 0);

		// Store cluster position into the directory entry
		dirEntry.startCluster = (startCluster & 0xFFFF);
		dirEntry.startClusterHigh = ((startCluster >> 16) & 0xFFFF);

		// The file has no data in it - its over written so should be empty
		dirEntry.fileSize = 0;

		if (dirEntry.name[0] == FILE_FREE)	// No file
		{
			// Have to create a new entry
			if(!FAT_AddDirEntry (path, dirEntry))
				return -1;

			// Get the newly created dirEntry
			dirEntry = FAT_DirEntFromPath (path);

			// Remember where directory entry was
			openFiles[file].dirEntSector = (wrkDirCluster == FAT16_ROOT_DIR_CLUSTER ? discRootDir : FAT_ClustToSect(wrkDirCluster)) + wrkDirSector;
			openFiles[file].dirEntOffset = wrkDirOffset;
		}
		else	// Already a file
		{
			// Just modify the old entry
			CF_ReadSector (openFiles[file].dirEntSector, globalBuffer);
			((DIR_ENT*) globalBuffer)[openFiles[file].dirEntOffset] = dirEntry;
			CF_WriteSector (openFiles[file].dirEntSector, globalBuffer);
		}
		

		// Now that file is created, open it
		openFiles[file].read = (mode[1] == '+');
		openFiles[file].write = true;
		openFiles[file].append = false;
		
		// Store information about position within the file, for use
		// by FAT_fread, FAT_fseek, etc.
		openFiles[file].firstCluster = startCluster;
		openFiles[file].length = 0;	// Should always have 0 bytes if openning in "w" mode
		openFiles[file].curPos = 0;
		openFiles[file].curClus = startCluster;
		openFiles[file].curSect = 0;
		openFiles[file].curByte = 0;

		// Not appending
		openFiles[file].appByte = 0;
		openFiles[file].appClus = 0;
		openFiles[file].appSect = 0;
		
		CF_ReadSector( FAT_ClustToSect(openFiles[file].curClus), openFiles[file].readBuffer);
		openFiles[file].inUse = true;	// We're using this file now

		return file;
	}

	if (ucase(mode[0]) == 'A')
	{
		if (dirEntry.name[0] == FILE_FREE)	// Create file if it doesn't exist
		{
			dirEntry.attrib = ATTRIB_ARCH;
			dirEntry.reserved = 0;
			
			// Time and date set to 'origin of computer universe'
			dirEntry.cTime_ms = 0;
			dirEntry.cTime = 0;
			dirEntry.cDate = 0;
			dirEntry.aDate = 0;
			dirEntry.mTime = 0;
			dirEntry.mDate = 0;

			// The file has no data in it
			dirEntry.fileSize = 0;

			// Get a cluster to use
			startCluster = FAT_LinkFreeCluster (CLUSTER_FREE);
			if (startCluster == CLUSTER_FREE)	// Couldn't get a free cluster
			{
				return -1;
			}
			dirEntry.startCluster = (startCluster & 0xFFFF);
			dirEntry.startClusterHigh = ((startCluster >> 16) & 0xFFFF);
			
			if(!FAT_AddDirEntry (path, dirEntry))
				return -1;
			
			// Get the newly created dirEntry
			dirEntry = FAT_DirEntFromPath (path);

			// Remember where directory entry was
			openFiles[file].dirEntSector = (wrkDirCluster == FAT16_ROOT_DIR_CLUSTER ? discRootDir : FAT_ClustToSect(wrkDirCluster)) + wrkDirSector;
			openFiles[file].dirEntOffset = wrkDirOffset;
		}
		else	// File already exists - reuse the old directory entry
		{
			startCluster = dirEntry.startCluster | (dirEntry.startClusterHigh << 16);
			// If it currently has no cluster, one must be assigned to it.
			if (startCluster == CLUSTER_FREE)
			{
				openFiles[file].firstCluster = FAT_LinkFreeCluster (CLUSTER_FREE);
				if (openFiles[file].firstCluster == CLUSTER_FREE)	// Couldn't get a free cluster
				{
					return -1;
				}
				// Flush fat write buffer
				FAT_WriteFatEntry (CLUSTER_FREE, 0);
				
				// Store cluster position into the directory entry
				dirEntry.startCluster = (openFiles[file].firstCluster & 0xFFFF);
				dirEntry.startClusterHigh = ((openFiles[file].firstCluster >> 16) & 0xFFFF);
				CF_ReadSector (openFiles[file].dirEntSector, globalBuffer);
				((DIR_ENT*) globalBuffer)[openFiles[file].dirEntOffset] = dirEntry;
				CF_WriteSector (openFiles[file].dirEntSector, globalBuffer);
			}
		}

		// Now that file is created, open it
		openFiles[file].read = (mode[1] == '+');
		openFiles[file].write = false;
		openFiles[file].append = true;
		
		// We are appending, calculate end of file
		
		// Follow cluster list until desired one is found
		openFiles[file].appClus = startCluster;
		
		// Follow cluster list until desired one is found
		for (clusCount = dirEntry.fileSize / discBytePerClus; clusCount > 0; clusCount --)
		{
			openFiles[file].appClus = FAT_NextCluster (openFiles[file].appClus);
		}

		// Calculate the sector and byte of the current position,
		// and store them
		openFiles[file].appSect = (dirEntry.fileSize % discBytePerClus) / BYTE_PER_READ;
		openFiles[file].appByte = dirEntry.fileSize % BYTE_PER_READ;

		// Store information about position within the file, for use
		// by FAT_fread, FAT_fseek, etc.
		openFiles[file].firstCluster = startCluster;
		openFiles[file].length = dirEntry.fileSize;
		openFiles[file].curPos = dirEntry.fileSize;
		openFiles[file].curClus = openFiles[file].appClus;
		openFiles[file].curSect = openFiles[file].appSect;
		openFiles[file].curByte = openFiles[file].appByte;
		
		// Read into buffer
		CF_ReadSector( FAT_ClustToSect(openFiles[file].curClus) + openFiles[file].curSect, openFiles[file].readBuffer);
		openFiles[file].inUse = true;	// We're using this file now

		return file;
	}

	// Can only reach here if a bad mode was specified
	return -1;
}

/*-----------------------------------------------------------------
FAT_fclose(file)
Closes a file
int file: IN handle of the file to close
bool return OUT: true if successful, false if not
-----------------------------------------------------------------*/
bool FAT_fclose (int file)
{
	// Clear memory used by file information
	if ((file >= 0) && (file < MAX_FILES_OPEN) && (openFiles[file].inUse == true))
	{
		if (openFiles[file].write || openFiles[file].append)
		{
			// Write new length back to directory entry
			CF_ReadSector (openFiles[file].dirEntSector, globalBuffer);
			((DIR_ENT*)globalBuffer)[openFiles[file].dirEntOffset].fileSize = openFiles[file].length;
			CF_WriteSector (openFiles[file].dirEntSector, globalBuffer);
		}
		openFiles[file].inUse = false;		
		return true;
	}
	else
	{
		return false;
	}
}

/*-----------------------------------------------------------------
FAT_ftell(file)
Returns the current position in a file
int file: IN handle of an open file
long int OUT: Current position
-----------------------------------------------------------------*/
long int FAT_ftell (int file)
{
	// Return the position as specified in the FAT_FILE structure
	if ((file >= 0) && (file < MAX_FILES_OPEN) && (openFiles[file].inUse == true))
	{
		return openFiles[file].curPos;
	}
	else
	{
		// Return -1 if no file was given
		return -1;
	}
}

/*-----------------------------------------------------------------
FAT_fseek(file, offset, origin)
Seeks to specified byte position in file
int file: IN handle of an open file
u32 offset IN: position to seek to, relative to origin
int origin IN: origin to seek from
int OUT: Returns 0 if successful, -1 if not
-----------------------------------------------------------------*/
int FAT_fseek(int file, u32 offset, int origin)
{
	u32 cluster;
	int clusCount;
	u32 position;

	if ((file < 0) || (file >= MAX_FILES_OPEN) || (openFiles[file].inUse == false))	// invalid file
	{
		return -1;
	}

	// Can't seek in append only mode
	if (!openFiles[file].read && !openFiles[file].write)	
	{
		return -1;
	}

	switch (origin) 
	{
	case SEEK_SET:
		position = offset;
		break;
	case SEEK_CUR:
		position = openFiles[file].curPos + offset;
		break;
	case SEEK_END:
		position = openFiles[file].length;
		break;
	default:
		return -1;
	}

	if (position > openFiles[file].length)
	{
		// Tried to go past end of file
		position = openFiles[file].length;
	}
	
	// Save position
	openFiles[file].curPos = position;

	cluster = openFiles[file].firstCluster;

	// Follow cluster list until desired one is found
	for (clusCount = position / discBytePerClus; clusCount > 0; clusCount --)
	{
		cluster = FAT_NextCluster (cluster);
	}
	openFiles[file].curClus = cluster;
	
	// Calculate the sector and byte of the current position,
	// and store them
	openFiles[file].curSect = (position % discBytePerClus) / BYTE_PER_READ;
	openFiles[file].curByte = position % BYTE_PER_READ;

	// Reload sector buffer for new position in file
	CF_ReadSector( openFiles[file].curSect + FAT_ClustToSect(openFiles[file].curClus), openFiles[file].readBuffer);

	return position;
}

/*-----------------------------------------------------------------
FAT_fread(buffer, size, count, file)
Reads in size * count bytes into buffer from file, starting
	from current position. It then sets the current position to the
	byte after the last byte read. If it reaches the end of file
	before filling the buffer then it stops reading.
void* buffer OUT: Pointer to buffer to fill. Should be at least as
	big as the number of bytes required
u32 size IN: size of each item to read
u32 count IN: number of items to read
int file IN: Handle of an open file
u32 OUT: returns the actual number of bytes read
-----------------------------------------------------------------*/
u32 FAT_fread (void* buffer, u32 size, u32 count, int file)
{
	int curByte;
	int curSect;
	u32 curClus;
	
	int dataPos = 0;
	int chunks;
	int beginBytes;

	char* data;
	u32 length;

	// Can't read non-existant files
	if ((file < 0) || (file >= MAX_FILES_OPEN) || (openFiles[file].inUse == false) || size == 0 || count == 0 || buffer == NULL)
		return 0;

	// Can only read files openned for reading
	if (!openFiles[file].read)
		return 0;

	curByte = openFiles[file].curByte;
	curSect = openFiles[file].curSect;
	curClus = openFiles[file].curClus;

	data = (char*)buffer;
	length = size * count;

	
	// Don't read past end of file
	if (length + openFiles[file].curPos > openFiles[file].length)
		length = openFiles[file].length - openFiles[file].curPos;

	// Number of bytes needed to read to align with a sector
	beginBytes = (BYTE_PER_READ < length + curByte ? (BYTE_PER_READ - curByte) : length);

	// Read first part from buffer, to align with sector boundary
	for (dataPos = 0 ; dataPos < beginBytes; dataPos++)
	{
		data[dataPos] = openFiles[file].readBuffer[curByte++];
	}

	// Read in all the 512 byte chunks of the file directly, saving time
	for ( chunks = ((int)length - beginBytes) / BYTE_PER_READ; chunks > 0; chunks--)
	{
		curSect++;
		if (curSect >= discSecPerClus)
		{
			curSect = 0;
			curClus = FAT_NextCluster (curClus);
		}

		CF_ReadSector( curSect + FAT_ClustToSect( curClus), data + dataPos);
		dataPos += BYTE_PER_READ;
	}

	// Take care of any bytes left over before end of read
	if (dataPos < length)
	{

		// Update the read buffer
		curSect++;
		curByte = 0;
		if (curSect >= discSecPerClus)
		{
			curSect = 0;
			curClus = FAT_NextCluster (curClus);
		}
		CF_ReadSector( curSect + FAT_ClustToSect( curClus), openFiles[file].readBuffer);
		
		// Read in last partial chunk
		for (; dataPos < length; dataPos++)
		{
			data[dataPos] = openFiles[file].readBuffer[curByte];
			curByte++;
		}
	}
	
	// Update file information
	openFiles[file].curByte = curByte;
	openFiles[file].curSect = curSect;
	openFiles[file].curClus = curClus;
	openFiles[file].curPos = openFiles[file].curPos + dataPos;
	return dataPos;
}


/*-----------------------------------------------------------------
FAT_fwrite(buffer, size, count, file)
Writes size * count bytes into file from buffer, starting
	from current position. It then sets the current position to the
	byte after the last byte written. If the file was openned in 
	append mode it always writes to the end of the file.
void* buffer IN: Pointer to buffer containing data. Should be at 
	least as big as the number of bytes to be written.
u32 size IN: size of each item to write
u32 count IN: number of items to write
int file IN: Handle of an open file
u32 OUT: returns the actual number of bytes written
-----------------------------------------------------------------*/
u32 FAT_fwrite (void* buffer, u32 size, u32 count, int file)
{
	int curByte;
	int curSect;
	u32 curClus;

	u32 tempNextCluster;
	
	int dataPos = 0;
	int chunks;
	int beginBytes;

	char* data;
	u32 length;

	if ((file < 0) || (file >= MAX_FILES_OPEN) || (openFiles[file].inUse == false) || size == 0 || count == 0 || buffer == NULL)
		return 0;

	if (openFiles[file].write)
	{
		// Write at current read pointer
		curByte = openFiles[file].curByte;
		curSect = openFiles[file].curSect;
		curClus = openFiles[file].curClus;
	}
	else if (openFiles[file].append)
	{
		// Write at end of file
		curByte = openFiles[file].appByte;
		curSect = openFiles[file].appSect;
		curClus = openFiles[file].appClus;
	}
	else
	{
		return 0;
	}


	data = (char*)buffer;
	length = size * count;

	// Number of bytes needed to write to align with a sector
	beginBytes = (BYTE_PER_READ < length + curByte ? (BYTE_PER_READ - curByte) : length);

	// Read in current sector to a buffer, so we don't clear data unintentionally
	CF_ReadSector (curSect + FAT_ClustToSect(curClus), openFiles[file].readBuffer);

	// Write first part to buffer, to align with sector boundary
	for (dataPos = 0 ; dataPos < beginBytes; dataPos++)
	{
		openFiles[file].readBuffer[curByte++] = data[dataPos];
	}

	// Write buffer back to disk
	CF_WriteSector (curSect + FAT_ClustToSect(curClus), openFiles[file].readBuffer);
	// Write all the 512 byte chunks of the file directly, saving time
	for ( chunks = ((int)length - beginBytes) / BYTE_PER_READ; chunks > 0; chunks--)
	{
		curSect++;
		if (curSect >= discSecPerClus)
		{
			curSect = 0;
			tempNextCluster = FAT_NextCluster(curClus);
			if ((tempNextCluster == CLUSTER_EOF) || (tempNextCluster == CLUSTER_FREE))
			{
				// Ran out of clusters so get a new one
				curClus = FAT_LinkFreeCluster(curClus);
				if (curClus == CLUSTER_FREE) // Couldn't get a cluster, so abort
				{
					return 0;
				}
			}
			else
			{
				curClus = FAT_NextCluster(curClus);
			}
		}

		CF_WriteSector( curSect + FAT_ClustToSect(curClus), data + dataPos);
		dataPos += BYTE_PER_READ;
	}

	// Take care of any bytes left over before end of read
	if (dataPos < length)
	{

		// Update the read buffer
		curSect++;
		curByte = 0;
		if (curSect >= discSecPerClus)
		{
			curSect = 0;
			tempNextCluster = FAT_NextCluster(curClus);
			if ((tempNextCluster == CLUSTER_EOF) || (tempNextCluster == CLUSTER_FREE))
			{
				curClus = FAT_LinkFreeCluster(curClus);
			}
			else
			{
				curClus = FAT_NextCluster(curClus);
			}
		}
		CF_ReadSector( curSect + FAT_ClustToSect( curClus), openFiles[file].readBuffer);
		
		// Write in last partial chunk to buffer
		for (; dataPos < length; dataPos++)
		{
			openFiles[file].readBuffer[curByte] = data[dataPos];
			curByte++;
		}

		// Write buffer back to disk
		CF_WriteSector( curSect + FAT_ClustToSect(curClus), openFiles[file].readBuffer);
	}
	
	// Update file information
	if (openFiles[file].write)	// Writing also shifts the read pointer
	{
		openFiles[file].curByte = curByte;
		openFiles[file].curSect = curSect;
		openFiles[file].curClus = curClus;
		openFiles[file].curPos = openFiles[file].curPos + dataPos;
		if (openFiles[file].length < openFiles[file].curPos)
		{
			openFiles[file].length = openFiles[file].curPos;
		}
	}
	else if (openFiles[file].append)	// Appending doesn't affect the read pointer
	{
		openFiles[file].appByte = curByte;
		openFiles[file].appSect = curSect;
		openFiles[file].appClus = curClus;
		openFiles[file].length = openFiles[file].length + dataPos;
	}

	return dataPos;
}


/*-----------------------------------------------------------------
FAT_feof(file)
Returns true if the end of file has been reached
int file IN: Handle of an open file
bool return OUT: true if EOF, false if not
-----------------------------------------------------------------*/
bool FAT_feof(int file)
{
	if ((file < 0) || (file >= MAX_FILES_OPEN) || (openFiles[file].inUse == false))
		return true;	// Return eof on invalid files

	return (openFiles[file].length == openFiles[file].curPos);
}


/*-----------------------------------------------------------------
FAT_DeleteFile(file)
Deletes the file sepecified in path
const char* path IN: Path and filename of file to delete
bool return OUT: true if successful
-----------------------------------------------------------------*/
bool FAT_DeleteFile (const char* path)
{
	DIR_ENT dirEntry;
	dirEntry = FAT_DirEntFromPath (path);
	
	if (dirEntry.name[0] == FILE_FREE)
	{
		return false;
	}
	
	// Don't delete directories!
	if (dirEntry.attrib & ATTRIB_DIR)
	{
		return false;
	}

	// Free any clusters used
	FAT_ClearLinks (dirEntry.startCluster | (dirEntry.startClusterHigh << 16));

	// Remove Directory entry

	CF_ReadSector ( (wrkDirCluster == FAT16_ROOT_DIR_CLUSTER ? discRootDir : FAT_ClustToSect(wrkDirCluster)) + wrkDirSector , globalBuffer);
	((DIR_ENT*)globalBuffer)[wrkDirOffset].name[0] = FILE_FREE;
	CF_WriteSector ( (wrkDirCluster == FAT16_ROOT_DIR_CLUSTER ? discRootDir : FAT_ClustToSect(wrkDirCluster)) + wrkDirSector , globalBuffer);
		
	return true;
}


/*-----------------------------------------------------------------
FAT_fgetc (handle)
Gets the next character in the file
int handle IN: Handle of open file
bool return OUT: character if successful, EOF if not
-----------------------------------------------------------------*/
char FAT_fgetc (int handle)
{
	char c;
	return (FAT_fread(&c, 1, 1, handle) == 1) ? c : EOF;
}


/*-----------------------------------------------------------------
FAT_fputc (character, handle)
Writes the given character into the file
char c IN: Character to be written
int handle IN: Handle of open file
bool return OUT: character if successful, EOF if not
-----------------------------------------------------------------*/
char FAT_fputc (char c, int handle)
{
	return (FAT_fwrite(&c, 1, 1, handle) == 1) ? c : EOF;
}


