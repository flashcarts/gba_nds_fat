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

// When compiling for NDS, make sure NDS is defined
// If using this on the ARM7, you will need to explicitly define NDS
#ifdef ARM9
 #ifndef NDS
  #define NDS 1
 #endif
#endif

#ifdef NDS
 #include <nds/jtypes.h>
/*
 #ifndef __cplusplus
  #ifndef bool
   typedef enum { false, true } bool;
  #endif
 #endif
*/
// Appropriate placement of CF functions and data
 #ifdef ARM7
  #define _CODE_IN_RAM __attribute__((section (".iwram"),long_call))
  #define _VARS_IN_RAM __attribute__((section (".bss")))
 #else
  #define _CODE_IN_RAM __attribute__((section (".ewram"),long_call))
  #define _VARS_IN_RAM __attribute__ ((section (".sbss")))
 #endif
#else
 #include "gba_types.h"
 #define _CODE_IN_RAM __attribute__((section (".iwram"),long_call))
 #define _VARS_IN_RAM __attribute__ ((section (".sbss")))
#endif

//---------------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif
//---------------------------------------------------------------------------------

//---------------------------------------------------------------
// Important constants

// Increase this to open more files, decrease to save memory
#define MAX_FILES_OPEN	4	// Maximum number of files open at once

#define MAX_FILENAME_LENGTH 256	// Maximum LFN length. Don't change this one

//-----------------------------------------------------------------
// FAT constants

#define FILE_LAST 0x00
#define FILE_FREE 0xE5

#define ATTRIB_ARCH	0x20
#define ATTRIB_DIR	0x10
#define ATTRIB_LFN	0x0F
#define ATTRIB_VOL	0x08
#define ATTRIB_HID	0x02
#define ATTRIB_SYS	0x04
#define ATTRIB_RO	0x01

#define FAT16_ROOT_DIR_CLUSTER 0x00

// File Constants
#ifndef EOF
#define EOF -1
#define SEEK_SET	0
#define SEEK_CUR	1
#define SEEK_END	2
#endif

// Directory Constants
#define DIR_END	0
#define DIR_FILE	1
#define DIR_DIR	2

//-----------------------------------------------------------------
// CF Card functions

/*-----------------------------------------------------------------
CF_IsInserted
Is a compact flash card inserted?
bool return OUT:  true if a CF card is inserted
-----------------------------------------------------------------*/
_CODE_IN_RAM bool CF_IsInserted (void);

/*-----------------------------------------------------------------
FAT_InitFiles
Reads the FAT information from the CF card.
You need to call this before reading any files.
bool return OUT: true if successful.
-----------------------------------------------------------------*/
bool FAT_InitFiles (void);

/*-----------------------------------------------------------------
FAT_FreeFiles
Closes all open files then resets the CF card.
Call this before exiting back to the GBAMP
bool return OUT: true if successful.
-----------------------------------------------------------------*/
bool FAT_FreeFiles (void);

/*-----------------------------------------------------------------
FAT_GetAlias
Get the alias (short name) of the last file or directory entry read
	using GetDirEntry. Works for FindFirstFile and FindNextFile
char* alias OUT: will be filled with the alias (short filename),
	should be at least 13 bytes long
bool return OUT: return true if successful
-----------------------------------------------------------------*/
bool FAT_GetAlias (char* alias);

/*-----------------------------------------------------------------
FAT_GetLongFilename
Get the long name of the last file or directory retrived with 
	GetDirEntry. Also works for FindFirstFile and FindNextFile
char* filename: OUT will be filled with the filename, should be at
	least 256 bytes long
bool return OUT: return true if successful
-----------------------------------------------------------------*/
bool FAT_GetLongFilename (char* filename);

/*-----------------------------------------------------------------
FAT_GetFileSize
Get the file size of the last file found or openned.
This idea is based on a modification by MoonShine
u32 return OUT: the file size
-----------------------------------------------------------------*/
u32 FAT_GetFileSize (void);

/*-----------------------------------------------------------------
FAT_GetFileCluster
Get the first cluster of the last file found or openned.
u32 return OUT: the file start cluster
-----------------------------------------------------------------*/
u32 FAT_GetFileCluster (void);

/*-----------------------------------------------------------------
FAT_FindNextFile
Gets the name of the next directory entry
	(can be a file or subdirectory)
char* filename: OUT filename, must be at least 13 chars long
int return: OUT returns 0 if failed, 1 if it found a file and 2 if 
	it found a directory
-----------------------------------------------------------------*/
int FAT_FindNextFile(char* filename);

/*-----------------------------------------------------------------
FAT_FindFirstFile
Gets the name of the first directory entry and resets the count
	(can be a file or subdirectory)
char* filename: OUT filename, must be at least 13 chars long
int return: OUT returns 0 if failed, 1 if it found a file and 2 if 
	it found a directory
-----------------------------------------------------------------*/
int FAT_FindFirstFile(char* filename);

/*-----------------------------------------------------------------
FAT_CWD
Changes the current working directory
const char* path: IN null terminated string of directory separated by 
	forward slashes, / is root
bool return: OUT returns true if successful
-----------------------------------------------------------------*/
bool FAT_CWD (const char* path);


//-----------------------------------------------------------------
// File functions

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
int FAT_fopen(const char* path, const char* mode);

/*-----------------------------------------------------------------
FAT_fclose(file)
Closes a file
int file: IN handle of the file to close
bool return OUT: true if successful, false if not
-----------------------------------------------------------------*/
bool FAT_fclose (int file);

/*-----------------------------------------------------------------
FAT_ftell(file)
Returns the current position in a file
int file: IN handle of an open file
long int OUT: Current position
-----------------------------------------------------------------*/
long int FAT_ftell (int file);

/*-----------------------------------------------------------------
FAT_fseek(file, offset, origin)
Seeks to specified byte position in file
int file: IN handle of an open file
u32 offset IN: position to seek to, relative to origin
int origin IN: origin to seek from
int OUT: Returns 0 if successful, -1 if not
-----------------------------------------------------------------*/
int FAT_fseek(int file, u32 offset, int origin);

/*-----------------------------------------------------------------
FAT_fread(buffer, size, count, file)
Reads in length number of bytes into buffer from file, starting
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
u32 FAT_fread (void* buffer, u32 size, u32 count, int file);

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
u32 FAT_fwrite (void* buffer, u32 size, u32 count, int file);

/*-----------------------------------------------------------------
FAT_feof(file)
Returns true if the end of file has been reached
int file IN: Handle of an open file
bool return OUT: true if EOF, false if not
-----------------------------------------------------------------*/
bool FAT_feof(int file);

/*-----------------------------------------------------------------
FAT_DeleteFile(file)
Deletes the file sepecified in path
const char* path IN: Path and filename of file to delete
bool return OUT: true if successful
-----------------------------------------------------------------*/
bool FAT_DeleteFile (const char* path);

/*-----------------------------------------------------------------
FAT_fgetc (handle)
Gets the next character in the file
int handle IN: Handle of open file
bool return OUT: character if successful, EOF if not
-----------------------------------------------------------------*/
char FAT_fgetc (int handle);

/*-----------------------------------------------------------------
FAT_fputc (character, handle)
Writes the given character into the file
char c IN: Character to be written
int handle IN: Handle of open file
bool return OUT: character if successful, EOF if not
-----------------------------------------------------------------*/
char FAT_fputc (char c, int handle);

//---------------------------------------------------------------------------------
#ifdef __cplusplus
}	   // extern "C"
#endif
//---------------------------------------------------------------------------------
