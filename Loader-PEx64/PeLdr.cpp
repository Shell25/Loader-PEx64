#include "PeLdr.h"
#include "Debug.h"
#include "PEB.h"
#include <fstream>
#include <iostream>

using namespace std;

#include <strsafe.h>

#ifndef NTSTATUS
#define NTSTATUS	LONG
#endif

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS  ((NTSTATUS)0x00000000L)
#endif

#pragma warning(disable: 4995)

static
BOOL PeLdrNeedSelfRelocation(PE_LDR_PARAM *pe)
{
	DWORD64				dwMyBase;
	PIMAGE_DOS_HEADER	pMyDosHeader;
	PIMAGE_NT_HEADERS	pMyNtHeaders;

	DMSG("Checking for self relocation");

	dwMyBase = (DWORD64)GetModuleHandle(NULL);
	if (!dwMyBase) 
	{
		EMSG("Failed to get our loaded address");
		return FALSE;
	}

	pMyDosHeader = (PIMAGE_DOS_HEADER)dwMyBase;
	pMyNtHeaders = (PIMAGE_NT_HEADERS)(dwMyBase + pMyDosHeader->e_lfanew);

	if (pMyNtHeaders->Signature != IMAGE_NT_SIGNATURE)
	{
		EMSG("Failed to find our own headers");
		return FALSE;
	}

	DMSG("MyBase: 0x%08x MySize: %d", dwMyBase, pMyNtHeaders->OptionalHeader.SizeOfImage);

	if ((pe->pNtHeaders->OptionalHeader.ImageBase >= dwMyBase) &&
		(pe->pNtHeaders->OptionalHeader.ImageBase < (dwMyBase + pMyNtHeaders->OptionalHeader.SizeOfImage)))
	{
		DMSG("Self relocation required");
		return TRUE;
	}

	return FALSE;
}

static
BOOL PeLdrApplyImageRelocations(DWORD64 dwImageBase, UINT_PTR iRelocOffset)
{
	PIMAGE_DOS_HEADER			pDosHeader;
	PIMAGE_NT_HEADERS			pNtHeaders;
	DWORD64						x;
	DWORD64						dwTmp;
	PIMAGE_BASE_RELOCATION		pBaseReloc;
	PIMAGE_RELOC				pReloc;

	DMSG("Applying Image Relocation (Base: 0x%08x RelocOffset: 0x%08x)",
		dwImageBase, iRelocOffset);

	pDosHeader = (PIMAGE_DOS_HEADER)dwImageBase;
	pNtHeaders = (PIMAGE_NT_HEADERS)(dwImageBase + pDosHeader->e_lfanew);

	pBaseReloc = (PIMAGE_BASE_RELOCATION)(dwImageBase +
			pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);

	while (pBaseReloc->SizeOfBlock) 
	{
		x = dwImageBase + pBaseReloc->VirtualAddress;
		dwTmp = (pBaseReloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(IMAGE_RELOC);
		pReloc = (PIMAGE_RELOC)(((DWORD64)pBaseReloc) + sizeof(IMAGE_BASE_RELOCATION));

		while (dwTmp--)
		{
			switch (pReloc->type) 
			{
			case IMAGE_REL_BASED_DIR64:
				*((UINT_PTR*)(x + pReloc->offset)) += iRelocOffset;
				break;
			case IMAGE_REL_BASED_HIGHLOW:
				*((DWORD64*)(x + pReloc->offset)) += (DWORD64)iRelocOffset;
				break;
			case IMAGE_REL_BASED_HIGH:
				*((WORD*)(x + pReloc->offset)) += HIWORD(iRelocOffset);
				break;
			case IMAGE_REL_BASED_LOW:
				*((WORD*)(x + pReloc->offset)) += LOWORD(iRelocOffset);
				break;
			case IMAGE_REL_BASED_ABSOLUTE:
				break;
			default:
				DMSG("Unknown relocation type: 0x%08x", pReloc->type);
				break;
			}
			pReloc += 1;
		}
		pBaseReloc = (PIMAGE_BASE_RELOCATION)(((DWORD64)pBaseReloc) + pBaseReloc->SizeOfBlock);
	}
	return TRUE;
}

static
BOOL PeLdrProcessIAT(DWORD64 dwImageBase)
{
	BOOL						ret = FALSE;
	PIMAGE_DOS_HEADER			pDosHeader;
	PIMAGE_NT_HEADERS			pNtHeaders;
	PIMAGE_IMPORT_DESCRIPTOR	pImportDesc;
	PIMAGE_THUNK_DATA			pThunkData;
	PIMAGE_THUNK_DATA			pThunkDataOrig;
	PIMAGE_IMPORT_BY_NAME		pImportByName;
	PIMAGE_EXPORT_DIRECTORY		pExportDir;
	DWORD64						flError = 0;
	DWORD64						dwTmp;
	BYTE						*pLibName;
	HMODULE						hMod;

	DMSG("Processing IAT (Image Base: 0x%08x)", dwImageBase);

	pDosHeader = (PIMAGE_DOS_HEADER)dwImageBase;
	pNtHeaders = (PIMAGE_NT_HEADERS)(dwImageBase + pDosHeader->e_lfanew);

	do {
		pImportDesc = (PIMAGE_IMPORT_DESCRIPTOR)(dwImageBase +
			pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

		if (!pImportDesc)
		{
			DMSG("IAT not found");
			break;
		}

		while ((pImportDesc->Name != 0) && (!flError)) 
		{
			pLibName = (BYTE*)(dwImageBase + pImportDesc->Name);
			DMSG("Loading Library and processing Imports: %s", (CHAR*)pLibName);

			if (pImportDesc->ForwarderChain != -1) 
			{
				DMSG("FIXME: Cannot handle Import Forwarding");
				//break;
			}

			hMod = LoadLibraryA((CHAR*)pLibName);
			if (!hMod) 
			{
				DMSG("Failed to load library: %s", pLibName);
				flError = 1;
				break;
			}

			pThunkData = (PIMAGE_THUNK_DATA)(dwImageBase + pImportDesc->FirstThunk);
			if (pImportDesc->Characteristics == 0)
				// Borland compilers doesn't produce Hint Table
				pThunkDataOrig = pThunkData;
			else
				// Hint Table 
				pThunkDataOrig = (PIMAGE_THUNK_DATA)(dwImageBase + pImportDesc->Characteristics);

			while (pThunkDataOrig->u1.AddressOfData != 0)
			{
				if (pThunkDataOrig->u1.Ordinal & IMAGE_ORDINAL_FLAG)
				{
					// Import via. Export Ordinal 
					PIMAGE_DOS_HEADER		_dos;
					PIMAGE_NT_HEADERS		_nt;

					_dos = (PIMAGE_DOS_HEADER)hMod;
					_nt = (PIMAGE_NT_HEADERS)(((DWORD64)hMod) + _dos->e_lfanew);

					pExportDir = (PIMAGE_EXPORT_DIRECTORY)
						(((DWORD64)hMod) + _nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
					dwTmp = (((DWORD64)hMod) + pExportDir->AddressOfFunctions) + (((IMAGE_ORDINAL(pThunkDataOrig->u1.Ordinal) - pExportDir->Base)) * sizeof(DWORD64));
					dwTmp = ((DWORD64)hMod) + *((DWORD64*)dwTmp);

					pThunkData->u1.Function = dwTmp;
				}
				else 
				{
					pImportByName = (PIMAGE_IMPORT_BY_NAME)
						(dwImageBase + pThunkDataOrig->u1.AddressOfData);
					pThunkData->u1.Function = (DWORD64)GetProcAddress(hMod, (LPCSTR)pImportByName->Name);

					if (!pThunkData->u1.Function) 
					{
						DMSG("Failed to resolve API: %s!%s",
							(CHAR*)pLibName, (CHAR*)pImportByName->Name);
						flError = 1;
						break;
					}
				}
				pThunkDataOrig++;
				pThunkData++;
			}
			pImportDesc++;
		}

		if (!flError)
		{
			ret = TRUE;
		}

	} while (0);
	return ret;
}

static
BOOL PeLdrRelocateAndContinue(PE_LDR_PARAM *pe, VOID *pContFunc, VOID *pParam)
{
	PIMAGE_DOS_HEADER	pMyDosHeader;
	PIMAGE_NT_HEADERS	pMyNtHeaders;
	DWORD64				dwNewBase;
	DWORD64				dwMyBase;
	DWORD64				dwAddr;
	UINT_PTR			iRelocOffset;

	DMSG("Relocating loader image (Continue Function: 0x%08x)", (DWORD64)pContFunc);

	dwMyBase = (DWORD64)GetModuleHandle(NULL);
	if (!dwMyBase) 
	{
		EMSG("Failed to get our loaded address");
		return FALSE;
	}

	pMyDosHeader = (PIMAGE_DOS_HEADER)dwMyBase;
	pMyNtHeaders = (PIMAGE_NT_HEADERS)(dwMyBase + pMyDosHeader->e_lfanew);

	dwNewBase = (DWORD64)VirtualAlloc(NULL, pMyNtHeaders->OptionalHeader.SizeOfImage + 1,
		MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!dwNewBase) 
	{
		EMSG("Failed to allocate memory for self relocation");
		return FALSE;
	}

	DMSG("New Loader Base: 0x%08x", dwNewBase);
	pe->dwLoaderRelocatedBase = dwNewBase;

	CopyMemory((VOID*)dwNewBase, (VOID*)dwMyBase,
		pMyNtHeaders->OptionalHeader.SizeOfImage);

	if (!PeLdrProcessIAT(dwNewBase)) 
	{
		EMSG("Failed to process IAT for relocated image");
		return FALSE;
	}

	iRelocOffset = dwNewBase - dwMyBase;
	if (!PeLdrApplyImageRelocations(dwNewBase, iRelocOffset)) 
	{
		EMSG("Failed to apply relocations on relocated image");
		return FALSE;
	}

	pe->dwLoaderBase = dwNewBase;

	dwAddr = ((DWORD64)pContFunc) - dwMyBase;
	dwAddr += dwNewBase;

	DMSG("Jumping to relocated image (Relocated Continue Function: 0x%08x)", dwAddr);
	
	void(*relocatedFunction)(VOID*) = (void(*)(VOID*))dwAddr;
	relocatedFunction(pParam);
	//__asm {
	//	mov eax, pParam
	//		push eax
	//		mov eax, dwAddr
	//		call eax
	//}

	return TRUE;
}

static
BOOL PeLdrLoadImage(PE_LDR_PARAM *pe)
{
	HANDLE	hFile = NULL;
	HANDLE	hMap = NULL;
	BOOL	ret = FALSE;
	_PPEB	peb;

	if (!pe)
	{
		return 0;
	}

	DMSG("Creating Map View of File");
	int size = 0;

	ifstream inputFile("test.exe", ios::in | ios::binary | ios::ate | ios::out);
	size = inputFile.tellg();
	inputFile.seekg(ios::beg);

	char* buffer = new char[size];
	inputFile.read(buffer, size);
	inputFile.close();

	pe->dwImage = (DWORD64)buffer;


	if (!pe->dwImage)
	{
		DMSG("Failed to obtain a map view of PE File");
		return 0;
	}
	DMSG("Map View of File created");

	pe->pDosHeader = ((PIMAGE_DOS_HEADER)pe->dwImage);
	if (pe->pDosHeader->e_magic != IMAGE_DOS_SIGNATURE)//pe->pDosHeader
	{
		DMSG("DOS Signature invalid");
		return 0;
	}

	pe->pNtHeaders = (PIMAGE_NT_HEADERS)(((DWORD64)pe->dwImage) + pe->pDosHeader->e_lfanew);
	if (pe->pNtHeaders->Signature != IMAGE_NT_SIGNATURE)//pe->pNtHeaders
	{
		DMSG("NT Signature mismatch");
		return 0;
	}

	peb = (_PPEB)__readgsqword(0x60);

	ret = TRUE;

out:

	return ret;
}

static
BOOL PeLdrExecuteEP(PE_LDR_PARAM *pe)
{
	DWORD	dwOld;
	DWORD64	dwEP;
	_PPEB	peb;

	// TODO: Fix permission as per section flags
	if (!VirtualProtect((LPVOID)pe->dwMapBase, pe->pNtHeaders->OptionalHeader.SizeOfImage,
		PAGE_EXECUTE_READWRITE, &dwOld))
	{
		DMSG("Failed to change mapping protection");
		return FALSE;
	}


	DMSG("Fixing Image Base address in PEB");
	peb = (_PPEB)__readgsqword(0x60);
	peb->lpImageBaseAddress = (LPVOID)pe->dwMapBase;

	dwEP = pe->dwMapBase + pe->pNtHeaders->OptionalHeader.AddressOfEntryPoint;
	DMSG("Executing Entry Point: 0x%08x", dwEP);

	void(*enterPoint)(void) = (void(*)(void) )dwEP;
	enterPoint();
	//__asm {
	//	mov eax, dwEP
	//		call eax
	//		int 3
	//}

	return TRUE;
}

static
BOOL PeLdrApplyRelocations(PE_LDR_PARAM *pe)
{
	UINT_PTR	iRelocOffset;

	if (pe->dwMapBase == pe->pNtHeaders->OptionalHeader.ImageBase) 
	{
		DMSG("Relocation not required");
		return TRUE;
	}

	if (!pe->pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) 
	{
		DMSG("PE required relocation but no relocatiom information found");
		return FALSE;
	}

	iRelocOffset = pe->dwMapBase - pe->pNtHeaders->OptionalHeader.ImageBase;
	return PeLdrApplyImageRelocations(pe->dwMapBase, iRelocOffset);
}

static
BOOL PeLdrMapImage(PE_LDR_PARAM *pe)
{
	DWORD64						i;
	MEMORY_BASIC_INFORMATION	mi;
	PIMAGE_SECTION_HEADER		pSectionHeader;
	BOOL						ret = FALSE;

	NTSTATUS(NTAPI *NtUnmapViewOfSection)
		(HANDLE, LPVOID) = NULL;
	if (!pe)
	{
		return ret;
	}

	DMSG("Mapping Target PE File");
	DMSG("Loader Base Orig: 0x%08x New: 0x%08x",
		pe->dwLoaderBase, pe->dwLoaderRelocatedBase);

	NtUnmapViewOfSection = (NTSTATUS(NTAPI *)(HANDLE, LPVOID))
		GetProcAddress(GetModuleHandle(TEXT("ntdll.dll")), "ZwUnmapViewOfSection");

	if (!NtUnmapViewOfSection)
	{
		DMSG("Failed to resolve address of NtUnmapViewOfSection");
	}

	do 
	{
		DMSG("Target PE Load Base: 0x%08x Image Size: 0x%08x",
			pe->pNtHeaders->OptionalHeader.ImageBase,
			pe->pNtHeaders->OptionalHeader.SizeOfImage);

		// Find the size of our mapping
		i = pe->dwLoaderBase;
		while (VirtualQuery((LPVOID)i, &mi, sizeof(mi)))
		{
			if (mi.State == MEM_FREE)
			{
				break;
			}

			i += mi.RegionSize;
		}

		if ((pe->pNtHeaders->OptionalHeader.ImageBase >= pe->dwLoaderBase) &&
			(pe->pNtHeaders->OptionalHeader.ImageBase < i))
		{
			if (NtUnmapViewOfSection) 
			{
				DMSG("Unmapping original loader mapping");
				if (NtUnmapViewOfSection(GetCurrentProcess(), (VOID*)pe->dwLoaderBase) == STATUS_SUCCESS) 
				{
					pe->dwMapBase = (DWORD64)VirtualAlloc((LPVOID)pe->pNtHeaders->OptionalHeader.ImageBase,
						pe->pNtHeaders->OptionalHeader.SizeOfImage + 1,
						MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				}
				else 
				{
					EMSG("Failed to unmap original loader mapping");
				}
			}
		}

		pe->dwMapBase = (DWORD64)VirtualAlloc((LPVOID)pe->pNtHeaders->OptionalHeader.ImageBase,
			pe->pNtHeaders->OptionalHeader.SizeOfImage + 1,
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

		if (!pe->dwMapBase)
		{
			EMSG("Failed to allocate PE ImageBase: 0x%08x",
				pe->pNtHeaders->OptionalHeader.ImageBase);
		}

		if (!pe->dwMapBase)
		{
			DMSG("Attempting to allocate new memory");

			if (!pe->pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
				EMSG("Failed to map required memory address, need relocation to continue");
				EMSG("[WARNING] Forcing re-use of mapped memory");

				pe->dwMapBase = (DWORD64)pe->pNtHeaders->OptionalHeader.ImageBase;
			}
			else 
			{
				pe->dwMapBase = (DWORD64)VirtualAlloc(NULL,
					pe->pNtHeaders->OptionalHeader.SizeOfImage + 1,
					MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			}
		}

		if (!pe->dwMapBase)
		{
			EMSG("Failed to map memory for Target PE");
			break;
		}

		DMSG("Allocated memory for Target PE: 0x%08x", pe->dwMapBase);

		DMSG("Copying Headers");
		CopyMemory((LPVOID)pe->dwMapBase, (LPVOID)pe->dwImage,
			pe->pNtHeaders->OptionalHeader.SizeOfHeaders);

		DMSG("Copying Sections");
		pSectionHeader = IMAGE_FIRST_SECTION(pe->pNtHeaders);
		for (i = 0; i < pe->pNtHeaders->FileHeader.NumberOfSections; i++)
		{
			DMSG("  Copying Section: %s", (CHAR*)pSectionHeader[i].Name);

			CopyMemory(
				(LPVOID)(pe->dwMapBase + pSectionHeader[i].VirtualAddress),
				(LPVOID)(pe->dwImage + pSectionHeader[i].PointerToRawData),
				pSectionHeader[i].SizeOfRawData
				);
		}

		ret = TRUE;
	} while (0);

	return ret;
}

static
BOOL PeLdrRunImage(PE_LDR_PARAM *pe)
{
	if (!PeLdrMapImage(pe))
		return FALSE;
	if (!PeLdrProcessIAT(pe->dwMapBase))
		return FALSE;
	if (!PeLdrApplyRelocations(pe))
		return FALSE;
	if (!PeLdrExecuteEP(pe))
		return FALSE;

	return TRUE;
}

BOOL PeLdrStart(PE_LDR_PARAM *pe)
{
	if (!PeLdrLoadImage(pe))
		return FALSE;
	if (PeLdrNeedSelfRelocation(pe))
		return PeLdrRelocateAndContinue(pe, (VOID*)PeLdrRunImage, (VOID*)pe);
	else
		return PeLdrRunImage(pe);

	return TRUE;
}

VOID PeLdrInit(PE_LDR_PARAM *pe)
{
	ZeroMemory(pe, sizeof(PE_LDR_PARAM));
}
