/*
		   _
  _ __  __| |_ ___ __
 | '_ \/ _` | '_\ V /
 | .__/\__,_|_|  \_/
 |_|

 Copyright (c) 2019 Samuel Tulach - All rights reserved

*/

#include <ntifs.h>
#include <ntstrsafe.h>
#include "Log.h"
#include "Structs.h"
#include "Private.h"
#include "Funcs.h"

/// <summary>
/// Driver main entry point
/// </summary>
/// <param name="DriverObject">DriverObject pointer</param>
/// <returns>RegistryPath pointer</returns>
NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath) 
{
	// Both are undefined when we manual map the driver
	UNREFERENCED_PARAMETER(DriverObject);
	UNREFERENCED_PARAMETER(RegistryPath);
	
	// Print some copyright because that's what matters the most
	Log("\n\npdrv\nCopyright (c) 2019 Samuel Tulach - All rights reserved\n\n");

	PSYSTEM_SERVICE_DESCRIPTOR_TABLE test = GetSSDTBase();
	Log("SSDT: %p", (void*)test);

	ULONG64 v_tid[0x256] = { 0 };
	ULONG thread_num = 0;
	NTSTATUS status = GetDriverThreads("EasyAntiCheat.sys", &thread_num, v_tid);
	if (!NT_SUCCESS(status))
	{
		Log("[-] Failed to get AC threads. Status code: %X.", status);
	}
	Log("%u threads found", thread_num);

	// Return dummy status
	return STATUS_SUCCESS;
}