#include "peloader.h"

#define MAX(a,b) (a>b?a:b)

#ifdef _WIN64
typedef ULONGLONG	QDWORD;
typedef PULONGLONG	PQDWORD;
#else
typedef DWORD	QDWORD;
typedef PDWORD	PQDWORD;
#endif

// �����Ա
typedef struct _SINGLELIST_ENTRY {
	struct _SINGLELIST_ENTRY* Next;
} SINGLELIST_ENTRY;

// ����ͷ
typedef struct _SINGLELIST_HEADER {
	UINT              Count;
	SINGLELIST_ENTRY* Head;
} SINGLELIST_HEADER;

// DLLģ�������Ա
typedef struct MODULE_ENTRY {
	SINGLELIST_ENTRY Entry;
	LPCSTR           ModuleName;
	HMODULE          Module;
	UINT             Count;
} MODULE_ENTRY;

// PE�Զ�������
typedef struct _PELOADERDATA {
	SINGLELIST_HEADER  List;
	DWORD              Flags;
	PE_IMPORT_CALLBACK ImportCallback;
	LPVOID             lParam;
} PELOADERDATA;

// DLL��ڵ�
typedef BOOL(APIENTRY* DLLMAIN)(
	HMODULE	hModule,
	DWORD	fdwReason,
	LPVOID	lpvReserved
	);

// �����ڴ�
static LPVOID MemAlloc(LPVOID lpAddress, SIZE_T dwSize)
{
	LPVOID lpMemAddress = VirtualAlloc(lpAddress, dwSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);	//ָ����ַ
	if (NULL == lpMemAddress) lpMemAddress = VirtualAlloc(NULL, dwSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);	//�����ַ
	return lpMemAddress;
}

// �ͷ��ڴ�
static void MemFree(LPVOID lpAddress)
{
	VirtualFree(lpAddress, 0, MEM_RELEASE);
}

// ���������С
static DWORD AlignedSize(DWORD dwOrigin, DWORD dwAlignment)
{
	return (dwOrigin + dwAlignment - 1) / dwAlignment * dwAlignment;
}

// ѹ���Ա
static int SListEntryPush(SINGLELIST_HEADER* list_head, SINGLELIST_ENTRY* list_entry)
{
	list_entry->Next = list_head->Head;
	list_head->Head = list_entry;
	return ++list_head->Count;
}

// ������Ա
static SINGLELIST_ENTRY* ListEntryPop(SINGLELIST_HEADER* list_head)
{
	SINGLELIST_ENTRY* entry = list_head->Head;

	if (NULL != entry) {
		list_head->Head = entry->Next;
	}

	return entry;
}

// ���PEͷ
static BOOL CheckPeHeader(LPBYTE lpData, DWORD dwLen)
{
	if (NULL == lpData) {
		return FALSE;
	}

	if (dwLen < sizeof(IMAGE_DOS_HEADER)) {
		return FALSE;
	}

	PIMAGE_DOS_HEADER lpDosHeader = (PIMAGE_DOS_HEADER)lpData;
	if (IMAGE_DOS_SIGNATURE != lpDosHeader->e_magic) {	//MZ
		return FALSE;
	}

	if (dwLen < lpDosHeader->e_lfanew + sizeof(IMAGE_NT_HEADERS)) {
		return FALSE;
	}

	PIMAGE_NT_HEADERS lpNtHeader = (PIMAGE_NT_HEADERS)(lpData + lpDosHeader->e_lfanew);
	if (IMAGE_NT_SIGNATURE != lpNtHeader->Signature) {	//PE
		return FALSE;
	}

#ifdef _WIN64
	if (IMAGE_FILE_MACHINE_AMD64 != lpNtHeader->FileHeader.Machine) {	//AMD64
		return FALSE;
	}
#else
	if (IMAGE_FILE_MACHINE_I386 != lpNtHeader->FileHeader.Machine) {	//I386
		return FALSE;
	}
#endif

	if (IMAGE_FILE_EXECUTABLE_IMAGE != (lpNtHeader->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE)) {	//��ִ��
		return FALSE;
	}

	if (lpNtHeader->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER)) {
		return FALSE;
	}

	PIMAGE_SECTION_HEADER lpSectionHeader;
	WORD wOptionalHeaderOffset = lpNtHeader->FileHeader.SizeOfOptionalHeader - sizeof(IMAGE_OPTIONAL_HEADER);
	lpSectionHeader = (PIMAGE_SECTION_HEADER)(lpData + lpDosHeader->e_lfanew + sizeof(IMAGE_NT_HEADERS) + wOptionalHeaderOffset);
	for (WORD i = 0; i < lpNtHeader->FileHeader.NumberOfSections; i++)
	{
		if ((lpSectionHeader[i].PointerToRawData + lpSectionHeader[i].SizeOfRawData) > dwLen) {
			return FALSE;
		}
	}

	return TRUE;
}

// �ض���
static BOOL DoRelocation(ULONG_PTR lpMemModule)
{
	PIMAGE_DOS_HEADER lpDosHeader = (PIMAGE_DOS_HEADER)lpMemModule;
	PIMAGE_NT_HEADERS lpNtHeader = (PIMAGE_NT_HEADERS)(lpMemModule + lpDosHeader->e_lfanew);
	QDWORD dwDelta = (QDWORD)(lpMemModule - lpNtHeader->OptionalHeader.ImageBase);

	if (0 == dwDelta || 0 == lpNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
		return TRUE;
	}

	DWORD dwRelocationOffset = lpNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
	PIMAGE_BASE_RELOCATION lpBaseRelocation = (PIMAGE_BASE_RELOCATION)(lpMemModule + dwRelocationOffset);
	while (0 != lpBaseRelocation->VirtualAddress)
	{
		DWORD dwRelocationSize = (lpBaseRelocation->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
		for (DWORD i = 0; i < dwRelocationSize; i++)
		{
			WORD wRelocationValue = *((PWORD)(lpMemModule + dwRelocationOffset + sizeof(IMAGE_BASE_RELOCATION) + i * sizeof(WORD)));
			WORD wRelocationType = wRelocationValue >> 12;

			if (IMAGE_REL_BASED_DIR64 == wRelocationType && sizeof(PULONGLONG) == sizeof(PQDWORD))
			{
				PQDWORD lpAddress = (PQDWORD)(lpMemModule + lpBaseRelocation->VirtualAddress + (wRelocationValue & 4095));
				*lpAddress += dwDelta;
			}
			else if (IMAGE_REL_BASED_HIGHLOW == wRelocationType && sizeof(PDWORD) == sizeof(PQDWORD))
			{
				PQDWORD lpAddress = (PQDWORD)(lpMemModule + lpBaseRelocation->VirtualAddress + (wRelocationValue & 4095));
				*lpAddress += dwDelta;
			}
			else if (IMAGE_REL_BASED_ABSOLUTE != wRelocationType)
			{
				return FALSE;
			}
		}

		dwRelocationOffset += lpBaseRelocation->SizeOfBlock;
		lpBaseRelocation = (PIMAGE_BASE_RELOCATION)(lpMemModule + dwRelocationOffset);
	}

	return TRUE;
}

// �ͷŵ���ģ��
static VOID FreeRavAddress(PELOADERDATA* lpPeData, ULONG_PTR lpMemModule, DWORD dwSize)
{
	PIMAGE_DOS_HEADER lpDosHeader = (PIMAGE_DOS_HEADER)lpMemModule;
	PIMAGE_NT_HEADERS lpNtHeader = (PIMAGE_NT_HEADERS)(lpMemModule + lpDosHeader->e_lfanew);

	if (lpNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size > 0)
	{
		DWORD i = 0;
		PIMAGE_IMPORT_DESCRIPTOR lpImportDescriptor = (PIMAGE_IMPORT_DESCRIPTOR)(lpMemModule + lpNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

		while (0 != lpImportDescriptor[i].Name && (0 == dwSize || i < dwSize))
		{
			DWORD j = 0;
			QDWORD realIAT = 0;

			LPCSTR lpModuleName = (LPCSTR)(lpMemModule + lpImportDescriptor[i].Name);	// ģ����
			DWORD dwFirstThunk = lpImportDescriptor[i].OriginalFirstThunk ? lpImportDescriptor[i].OriginalFirstThunk : lpImportDescriptor[i].FirstThunk;	// IAT��
			while (0 != (realIAT = ((PQDWORD)(lpMemModule + dwFirstThunk))[j]))
			{
				LPCSTR lpProcName = realIAT & IMAGE_ORDINAL_FLAG ? (LPCSTR)(realIAT & 65535) : (LPCSTR)(lpMemModule + realIAT + 2);

				if (NULL != lpPeData->ImportCallback) {
					lpPeData->ImportCallback(lpPeData->lParam, PE_IMPORTS_TYPE_FREE, lpModuleName, lpProcName, NULL);
				}
				
				j++;
			}

			i++;
		}

		MODULE_ENTRY* entry = NULL;
		while (NULL != (entry = (MODULE_ENTRY*)ListEntryPop(&lpPeData->List)))
		{
			for (UINT i = 0; i < entry->Count; i++) {
				FreeLibrary(entry->Module);
			}

			free(entry);
		}
	}
}

// ��䵼���
static BOOL FillRavAddress(PELOADERDATA* lpPeData, ULONG_PTR lpMemModule, PE_IMPORT_CALLBACK fnImportCallback, LPVOID lParam)
{
	PIMAGE_DOS_HEADER lpDosHeader = (PIMAGE_DOS_HEADER)lpMemModule;
	PIMAGE_NT_HEADERS lpNtHeader = (PIMAGE_NT_HEADERS)(lpMemModule + lpDosHeader->e_lfanew);

	if (0 == lpNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
		return TRUE;
	}

	DWORD i = 0;
	PIMAGE_IMPORT_DESCRIPTOR lpImportDescriptor = (PIMAGE_IMPORT_DESCRIPTOR)(lpMemModule + lpNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
	while (0 != lpImportDescriptor[i].Name)
	{
		DWORD j = 0;
		QDWORD realIAT = 0;
		HMODULE hModule = NULL;

		LPCSTR lpModuleName = (LPCSTR)(lpMemModule + lpImportDescriptor[i].Name);	// ģ����
		DWORD dwFirstThunk = lpImportDescriptor[i].OriginalFirstThunk ? lpImportDescriptor[i].OriginalFirstThunk : lpImportDescriptor[i].FirstThunk;	// IAT��
		while (0 != (realIAT = ((PQDWORD)(lpMemModule + dwFirstThunk))[j]))
		{
			// ��� �� ����
			LPCSTR lpProcName = realIAT & IMAGE_ORDINAL_FLAG ? (LPCSTR)(realIAT & 65535) : (LPCSTR)(lpMemModule + realIAT + 2);

			// ������ַ
			FARPROC lpAddress = NULL;
			BOOL bPreventDefault = FALSE;
			if (NULL != fnImportCallback)
			{
				// ͨ���ص���ȡ��ַ
				bPreventDefault = fnImportCallback(lParam, PE_IMPORTS_TYPE_INIT, lpModuleName, lpProcName, &lpAddress);
			}

			// Ĭ�Ϸ�ʽ
			if (FALSE == bPreventDefault)
			{
				lpAddress = NULL;
				if (NULL == hModule)
				{
					// ����ģ��
					hModule = LoadLibraryExA(lpModuleName, NULL, 0);
					if (NULL != hModule)
					{
						// ����
						MODULE_ENTRY* entry = (MODULE_ENTRY*)lpPeData->List.Head;
						while (NULL != entry)
						{
							// ��Ϊͬһ���ڴ棬ֱ�ӱȽ�ָ��
							if (entry->ModuleName == lpModuleName) {
								break;
							}

							// ��һ��
							entry = (MODULE_ENTRY*)entry->Entry.Next;
						}

						if (NULL != entry) {
							entry->Count++; // ������+1
						}
						else
						{
							// ��������
							entry = (MODULE_ENTRY*)malloc(sizeof(MODULE_ENTRY));
							if (NULL != entry)
							{
								entry->Count = 1;
								entry->Module = hModule;
								entry->ModuleName = lpModuleName;
								SListEntryPush(&lpPeData->List, (SINGLELIST_ENTRY*)entry);
							}
							else
							{
								// �����ͷ�
								FreeLibrary(hModule);
								hModule = NULL;
							}
						}
					}
				}

				if (NULL != hModule)
				{
					// ȡ�ú���ָ��
					lpAddress = GetProcAddress(hModule, lpProcName);
				}
			}

			// �Ƿ���Ч��ַ
			if (NULL != lpAddress)
			{
				// ��Ч �ѵ�ַд��FirstThunk
				((FARPROC*)(lpMemModule + lpImportDescriptor[i].FirstThunk))[j] = lpAddress;
			}
			else
			{
				// ����ʧ��
				FreeRavAddress(lpPeData, lpMemModule, i + 1);
				return FALSE;
			}

			j++;
		}

		i++;
	}
	
	return TRUE;
}

// ����ģ��
HMODULE WINAPI PeLoader_LoadLibrary(LPBYTE lpData, DWORD dwLen, DWORD dwFlags, PE_IMPORT_CALLBACK fnImportCallback, LPVOID lParam)
{
	// ���PEͷ
	if (FALSE == CheckPeHeader(lpData, dwLen)) {
		return NULL;
	}

	// PEͷ
	PIMAGE_DOS_HEADER lpDosHeader = (PIMAGE_DOS_HEADER)lpData;
	PIMAGE_NT_HEADERS lpNtHeader = (PIMAGE_NT_HEADERS)(lpData + lpDosHeader->e_lfanew);

	// ����ӳ���С
	WORD wOptionalHeaderOffset = lpNtHeader->FileHeader.SizeOfOptionalHeader - sizeof(IMAGE_OPTIONAL_HEADER);
	PIMAGE_SECTION_HEADER lpSectionHeader = (PIMAGE_SECTION_HEADER)(lpData + lpDosHeader->e_lfanew + sizeof(IMAGE_NT_HEADERS) + wOptionalHeaderOffset);
	DWORD dwSizeOfImage = lpNtHeader->OptionalHeader.SizeOfImage;

	// ȡ���ֵ
	for (WORD i = 0; i < lpNtHeader->FileHeader.NumberOfSections; i++) {
		dwSizeOfImage = MAX(dwSizeOfImage, AlignedSize(lpSectionHeader[i].VirtualAddress + MAX(lpSectionHeader[i].SizeOfRawData, lpSectionHeader[i].Misc.VirtualSize), lpNtHeader->OptionalHeader.SectionAlignment));
	}

	// �����쳣
	if (0 == dwSizeOfImage) {
		return NULL;
	}

	// �����ڴ� ĩβ�����Զ�������
	ULONG_PTR lpMemModule = (ULONG_PTR)MemAlloc((LPVOID)lpNtHeader->OptionalHeader.ImageBase, dwSizeOfImage + sizeof(PELOADERDATA));
	if (NULL == (LPVOID)lpMemModule) {
		return NULL;
	}

	// ��������
	memcpy((LPVOID)lpMemModule, lpData, lpDosHeader->e_lfanew + sizeof(IMAGE_NT_HEADERS) + wOptionalHeaderOffset + lpNtHeader->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER));
	for (WORD i = 0; i < lpNtHeader->FileHeader.NumberOfSections; i++)
	{
		if (0 != lpSectionHeader[i].SizeOfRawData && 0 != lpSectionHeader[i].VirtualAddress) {
			memcpy((LPVOID)(lpMemModule + lpSectionHeader[i].VirtualAddress), lpData + lpSectionHeader[i].PointerToRawData, lpSectionHeader[i].SizeOfRawData);
		}
	}

	// �Զ�������
	PELOADERDATA* lpPeData = (PELOADERDATA*)(lpMemModule + dwSizeOfImage);
	{
		lpPeData->lParam = lParam;
		lpPeData->ImportCallback = fnImportCallback;
		lpPeData->Flags = dwFlags;
		lpPeData->List.Head = NULL;
		lpPeData->List.Count = 0;
	}

	// �ض���
	if (FALSE != DoRelocation(lpMemModule))
	{
		// ����ʼ��ģ��
		if (dwFlags == DONT_RESOLVE_DLL_REFERENCES) {
			return (HMODULE)lpMemModule;
		}

		// ��䵼���
		if (FALSE != FillRavAddress(lpPeData, lpMemModule, fnImportCallback, lParam))
		{
			// ��ִ�����
			if (IMAGE_FILE_DLL != (lpNtHeader->FileHeader.Characteristics & IMAGE_FILE_DLL) || (dwFlags == LOAD_LIBRARY_AS_DATAFILE)) {
				return (HMODULE)lpMemModule;
			}

			// �Ƿ�������
			if (0 == lpNtHeader->OptionalHeader.AddressOfEntryPoint) {
				return (HMODULE)lpMemModule;
			}

			// ִ�����
			DLLMAIN dllmain = (DLLMAIN)(lpMemModule + lpNtHeader->OptionalHeader.AddressOfEntryPoint);
			if (FALSE != dllmain((HMODULE)lpMemModule, DLL_PROCESS_ATTACH, NULL)) {
				return (HMODULE)lpMemModule;
			}

			FreeRavAddress(lpPeData, lpMemModule, 0);
		}
	}

	MemFree((LPVOID)lpMemModule);
	return NULL;
}

// �ͷ�ģ��
VOID WINAPI PeLoader_FreeLibrary(HMODULE hMemModule)
{
	ULONG_PTR lpMemModule = (ULONG_PTR)hMemModule;
	PIMAGE_DOS_HEADER lpDosHeader = (PIMAGE_DOS_HEADER)lpMemModule;
	PIMAGE_NT_HEADERS lpNtHeader = (PIMAGE_NT_HEADERS)(lpMemModule + lpDosHeader->e_lfanew);

	//�����С
	WORD wOptionalHeaderOffset = lpNtHeader->FileHeader.SizeOfOptionalHeader - sizeof(IMAGE_OPTIONAL_HEADER);
	PIMAGE_SECTION_HEADER lpSectionHeader = (PIMAGE_SECTION_HEADER)(lpMemModule + lpDosHeader->e_lfanew + sizeof(IMAGE_NT_HEADERS) + wOptionalHeaderOffset);
	DWORD dwSizeOfImage = lpNtHeader->OptionalHeader.SizeOfImage;

	// ȡ���ֵ
	for (WORD i = 0; i < lpNtHeader->FileHeader.NumberOfSections; i++) {
		dwSizeOfImage = MAX(dwSizeOfImage, AlignedSize(lpSectionHeader[i].VirtualAddress + MAX(lpSectionHeader[i].SizeOfRawData, lpSectionHeader[i].Misc.VirtualSize), lpNtHeader->OptionalHeader.SectionAlignment));
	}

	//���ݼ��ط�ʽ����
	PELOADERDATA* lpPeData = (PELOADERDATA*)(lpMemModule + dwSizeOfImage);
	if (IMAGE_FILE_DLL == (lpNtHeader->FileHeader.Characteristics & IMAGE_FILE_DLL) && (DONT_RESOLVE_DLL_REFERENCES != lpPeData->Flags) && (LOAD_LIBRARY_AS_DATAFILE != lpPeData->Flags))
	{
		if (0 != lpNtHeader->OptionalHeader.AddressOfEntryPoint) {
			DLLMAIN dllmain = (DLLMAIN)(lpMemModule + lpNtHeader->OptionalHeader.AddressOfEntryPoint);
			dllmain((HMODULE)lpMemModule, DLL_PROCESS_DETACH, NULL);
		}
	}

	if (DONT_RESOLVE_DLL_REFERENCES != lpPeData->Flags) {
		FreeRavAddress(lpPeData, lpMemModule, 0);
	}

	MemFree((LPVOID)lpMemModule);
}

// ȡ������ַ
FARPROC WINAPI PeLoader_GetProcAddress(HMODULE hMemModule, LPCSTR lpProcName)
{
	ULONG_PTR lpMemModule = (ULONG_PTR)hMemModule;
	PIMAGE_DOS_HEADER lpDosHeader = (PIMAGE_DOS_HEADER)lpMemModule;
	PIMAGE_NT_HEADERS lpNtHeader = (PIMAGE_NT_HEADERS)(lpMemModule + lpDosHeader->e_lfanew);

	// �Ƿ���ڵ�����
	if (lpNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size > 0)
	{
		// �������ַ
		PIMAGE_EXPORT_DIRECTORY lpExportDirectory = (PIMAGE_EXPORT_DIRECTORY)(lpMemModule + lpNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
		PDWORD lpAddressOfFunctions = (PDWORD)(lpMemModule + lpExportDirectory->AddressOfFunctions);
		DWORD dwOrdinals = (DWORD)((ULONG_PTR)lpProcName - lpExportDirectory->Base);

		// �Ƿ����
		if (dwOrdinals >= 0 && dwOrdinals <= lpExportDirectory->NumberOfFunctions)
		{
			// ֱ�Ӹ�������ȡ�ý��
			return (FARPROC)(lpMemModule + lpAddressOfFunctions[dwOrdinals]);
		}
		else
		{
			// ����ָ������
			PDWORD lpAddressOfNames = (PDWORD)(lpMemModule + lpExportDirectory->AddressOfNames);
			PWORD lpAddressOfNameOrdinals = (PWORD)(lpMemModule + lpExportDirectory->AddressOfNameOrdinals);

			for (DWORD i = 0; i < lpExportDirectory->NumberOfNames; i++)
			{
				LPSTR lpName = (LPSTR)(lpMemModule + lpAddressOfNames[i]);
				if (0 == strcmp(lpProcName, lpName))
				{
					dwOrdinals = lpAddressOfNameOrdinals[i];
					if (dwOrdinals >= 0 && dwOrdinals <= lpExportDirectory->NumberOfFunctions) {
						return (FARPROC)(lpMemModule + lpAddressOfFunctions[dwOrdinals]);
					}
				}
			}
		}
	}

	return NULL;
}

// ȡ��ڵ�
FARPROC WINAPI PeLoader_GetEntryPoint(HMODULE hMemModule)
{
	ULONG_PTR lpMemModule = (ULONG_PTR)hMemModule;
	PIMAGE_DOS_HEADER lpDosHeader = (PIMAGE_DOS_HEADER)lpMemModule;
	PIMAGE_NT_HEADERS lpNtHeader = (PIMAGE_NT_HEADERS)(lpMemModule + lpDosHeader->e_lfanew);
	return (FARPROC)(lpMemModule + lpNtHeader->OptionalHeader.AddressOfEntryPoint);
}
