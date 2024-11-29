/*

	disc_io.c

	uniformed io-interface to work with Chishm's FAT library

	Written by MightyMax
  
	Modified by Chishm:
	2005-11-06
		* Added WAIT_CR modifications for NDS
	
*/

#include "disc_io.h"

#ifdef NDS
	#include <nds.h>
#endif


// Include known io-interfaces:
#include "io_mpcf.h"
#include "io_m3cf.h"
#include "io_sccf.h"
#include "io_fcsr.h"

LPIO_INTERFACE active_interface ;

bool disc_Init(void) 
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

	// check if we have a M3 perfect CF plugged in
	active_interface = M3CF_GetInterface() ;
	if (active_interface->fn_StartUp())
	{
		// set M3 CF as default IO
		return true ;
	} ;

	// check if we have a GBA Movie Player plugged in
	active_interface = MPCF_GetInterface() ;
	if (active_interface->fn_StartUp())
	{
		// set GBAMP as default IO
		return true ;
	} ;

	// check if we have a SuperCard CF plugged in
	active_interface = SCCF_GetInterface() ;
	if (active_interface->fn_StartUp())
	{
		// set SC CF as default IO
		return true ;
	} ;

	// check if we have a GBA Flash Cart plugged in
	active_interface = FCSR_GetInterface() ;
	if (active_interface->fn_StartUp())
	{
		// set FC as default IO
		return true ;
	} ;

	// could not find a working IO Interface
	active_interface = 0 ;
	return false ;
} 

bool disc_IsInserted(void) 
{
	if (active_interface) return active_interface->fn_IsInserted() ;
	return false ;
} 

bool disc_ReadSectors(u32 sector, u8 numSecs, void* buffer) 
{
	if (active_interface) return active_interface->fn_ReadSectors(sector,numSecs,buffer) ;
	return false ;
} 

bool disc_WriteSectors(u32 sector, u8 numSecs, void* buffer) 
{
	if (active_interface) return active_interface->fn_WriteSectors(sector,numSecs,buffer) ;
	return false ;
} 

bool disc_ClearStatus(void) 
{
	if (active_interface) return active_interface->fn_ClearStatus() ;
	return false ;
} 

bool disc_Shutdown(void) 
{
	if (active_interface) active_interface->fn_Shutdown() ;
	active_interface = 0 ;
	return true ;
} 

u32	disc_HostType (void)
{
	if (active_interface)
	{
		return active_interface->ul_ioType;
	} else {
		return 0;
	}
}

