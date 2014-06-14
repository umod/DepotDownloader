#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

int FindSteamProcessID();
BOOL ProcessHasModuleLoaded(const int iProcessID, const char * szModuleName);
BOOL SelfInjectIntoSteam(const HWND hWindow, const int iSteamProcessID, const char * szNetHookDllPath);
BOOL InjectEjection(const HWND hWindow, const int iSteamProcessID);

//
// RunDLL Interface
//
// rundll32.exe C:\Path\To\NetHook2.dll,Inject
// rundll32.exe C:\Path\To\NetHook2.dll,Eject
//

#pragma comment(linker, "/EXPORT:Inject=?Inject@@YGXPAUHWND__@@PAUHINSTANCE__@@PADH@Z")
__declspec(dllexport) void CALLBACK Inject(HWND hWindow, HINSTANCE hInstance, LPSTR lpszCommandLine, int nCmdShow);

#pragma comment(linker, "/EXPORT:Eject=?Eject@@YGXPAUHWND__@@PAUHINSTANCE__@@PADH@Z")
__declspec(dllexport) void CALLBACK Eject(HWND hWindow, HINSTANCE hInstance, LPSTR lpszCommandLine, int nCmdShow);

void CALLBACK Inject(HWND hWindow, HINSTANCE hInstance, LPSTR lpszCommandLine, int nCmdShow)
{
	int iSteamProcessID = FindSteamProcessID();
	if (iSteamProcessID <= 0)
	{
		MessageBoxA(hWindow, "Unable to find Steam. Make sure Steam is running, then try again.", "NetHook2", MB_OK | MB_ICONASTERISK);
		return;
	}

	char szNethookDllPath[MAX_PATH];
	ZeroMemory(szNethookDllPath, sizeof(szNethookDllPath));
	int result = GetModuleFileNameA((HINSTANCE)&__ImageBase, szNethookDllPath, sizeof(szNethookDllPath));

	BOOL bInjected = SelfInjectIntoSteam(hWindow, iSteamProcessID, szNethookDllPath);
	if (!bInjected)
	{
		// Do nothing, SelfInjectIntoSteam already shows a messagebox with details.
	}
}

void CALLBACK Eject(HWND hWindow, HINSTANCE hInstance, LPSTR lpszCommandLine, int nCmdShow)
{
	int iSteamProcessID = FindSteamProcessID();
	if (iSteamProcessID <= 0)
	{
		MessageBoxA(hWindow, "Unable to find Steam. Make sure Steam is running, then try again.", "NetHook2", MB_OK | MB_ICONASTERISK);
		return;
	}

	char szNethookDllPath[MAX_PATH];
	ZeroMemory(szNethookDllPath, sizeof(szNethookDllPath));
	int result = GetModuleFileNameA((HINSTANCE)&__ImageBase, szNethookDllPath, sizeof(szNethookDllPath));

	if (!ProcessHasModuleLoaded(iSteamProcessID, szNethookDllPath))
	{
		MessageBoxA(hWindow, "Unable to eject NetHook2: This instance of Steam does not have NetHook2 loaded.", "NetHook2", MB_OK | MB_ICONASTERISK);
		return;
	}
	
	BOOL bInjected = InjectEjection(hWindow, iSteamProcessID);
	if (!bInjected)
	{
		// Do nothing, InjectEjection already shows a messagebox with details.
	}
}

//
// Process Helpers
//
int FindSteamProcessID()
{
	DWORD cbNeeded = 0;
	const int MAX_NUM_PROCESSES = 2048; // Be generous
	DWORD piProcesses[MAX_NUM_PROCESSES];

	if (!EnumProcesses(piProcesses, sizeof(piProcesses), &cbNeeded))
	{
		return -1;
	}

	int iNumProcesses = cbNeeded / sizeof(DWORD);
	for (int i = 0; i < iNumProcesses; i++)
	{
		DWORD pid = piProcesses[i];

		HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pid);
		if (hProcess != NULL)
		{
			char szProcessPath[MAX_PATH];
			DWORD dwPathLength = GetModuleFileNameEx(hProcess, 0, szProcessPath, sizeof(szProcessPath));
			const char * szEndsWithKey = "\\steam.exe";
			unsigned int cubEndsWithKey = strlen(szEndsWithKey);

			if (dwPathLength != 0 && dwPathLength > cubEndsWithKey)
			{
				if (_stricmp(szProcessPath + dwPathLength - cubEndsWithKey, szEndsWithKey) == 0)
				{
					CloseHandle(hProcess);
					return pid;
				}
			}

			CloseHandle(hProcess);
		}
	}

	return -1;
}

BOOL ProcessHasModuleLoaded(const int iProcessID, const char * szModuleName)
{
	HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, iProcessID);
	if (hProcess != NULL)
	{
		HMODULE hModules[1024];
		DWORD cbNeeded;
		if (EnumProcessModules(hProcess, hModules, sizeof(hModules), &cbNeeded))
		{
			int iNumModules = cbNeeded / sizeof(HMODULE);

			for (int i = 0; i < iNumModules; i++)
			{
				char szModulePath[MAX_PATH];
				ZeroMemory(szModulePath, sizeof(szModulePath));

				if (GetModuleFileNameExA(hProcess, hModules[i], szModulePath, sizeof(szModulePath)))
				{
					if (_stricmp(szModulePath, szModuleName) == 0)
					{
						CloseHandle(hProcess);
						return true;
					}
				}
			}
		}

		CloseHandle(hProcess);
	}
	return false;
}

//
// Code Cave
//
typedef HMODULE (WINAPI *GetModuleHandleAPtr)(LPCSTR);
typedef BOOL (WINAPI *FreeLibraryPtr)(HMODULE);

struct EjectParams
{
	GetModuleHandleAPtr GetModuleHandleA;
	FreeLibraryPtr FreeLibrary;
	char szModuleName[1024];
};

//
// This function can not have any (absolute?) 'jmp' statements.
// Any referenced functions must come from function pointers in EjectParams.
#ifdef __MSVC_RUNTIME_CHECKS
#error /RTC is not allowed.
#endif
static DWORD WINAPI _Eject(LPVOID lpThreadParameter)
{
	struct EjectParams * pParams = (struct EjectParams *)lpThreadParameter;
	HMODULE hModule = pParams->GetModuleHandleA(pParams->szModuleName);
	pParams->FreeLibrary(hModule);

	return 0;
}

//
// Code Injection
//

// Inject NetHook2 via LoadLibrary
BOOL SelfInjectIntoSteam(const HWND hWindow, const int iSteamProcessID, const char * szNetHookDllPath)
{
	HANDLE hSteamProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, iSteamProcessID);
	if (hSteamProcess == NULL)
	{
		MessageBoxA(hWindow, "Unable to open Steam process.", "NetHook2", MB_OK | MB_ICONASTERISK);
		return false;
	}

	HMODULE hKernel32Module = GetModuleHandleA("kernel32.dll");
	if (hKernel32Module == NULL)
	{
		MessageBoxA(hWindow, "Unable to open load kernel32.dll.", "NetHook2", MB_OK | MB_ICONASTERISK);
		CloseHandle(hSteamProcess);
		return false;
	}

	LPVOID pLoadLibraryA = (LPVOID)GetProcAddress(hKernel32Module, "LoadLibraryA");
	if (pLoadLibraryA == NULL)
	{
		MessageBoxA(hWindow, "Unable to find LoadLibraryA.", "NetHook2", MB_OK | MB_ICONASTERISK);
		CloseHandle(hSteamProcess);
		return false;
	}

	LPVOID pArgBuffer = VirtualAllocEx(hSteamProcess, NULL, strlen(szNetHookDllPath), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (pArgBuffer == NULL)
	{
		MessageBoxA(hWindow, "Unable to allocate memory inside Steam.", "NetHook2", MB_OK | MB_ICONASTERISK);
		CloseHandle(hSteamProcess);
		return false;
	}

	BOOL bWritten = WriteProcessMemory(hSteamProcess, pArgBuffer, szNetHookDllPath, strlen(szNetHookDllPath), NULL);
	if (!bWritten)
	{
		MessageBoxA(hWindow, "Unable to write to allocated memory inside Steam.", "NetHook2", MB_OK | MB_ICONASTERISK);
		VirtualFreeEx(hSteamProcess, pArgBuffer, 0, MEM_RELEASE);
		CloseHandle(hSteamProcess);
		return false;
	}

	HANDLE hRemoteThread = CreateRemoteThread(hSteamProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLibraryA, pArgBuffer, NULL, NULL);
	if (hRemoteThread == NULL)
	{
		MessageBoxA(hWindow, "Unable to create remote thread inside Steam.", "NetHook2", MB_OK | MB_ICONASTERISK);
		VirtualFreeEx(hSteamProcess, pArgBuffer, 0, MEM_RELEASE);
		CloseHandle(hSteamProcess);
		return false;
	}

	if (WaitForSingleObject(hRemoteThread, 5000 /* milliseconds */) == WAIT_TIMEOUT)
	{
		MessageBoxA(hWindow, "Injection timed out.", "NetHook2", MB_OK | MB_ICONASTERISK);
		VirtualFreeEx(hSteamProcess, pArgBuffer, 0, MEM_RELEASE);
		CloseHandle(hSteamProcess);
		return false;
	}

	VirtualFreeEx(hSteamProcess, pArgBuffer, 0, MEM_RELEASE);

	CloseHandle(hSteamProcess);
	return true;
}

// Inject the 'Eject' code cave instructions
// This seems to be the only way to not crash Steam when ejecting.
BOOL InjectEjection(const HWND hWindow, const int iSteamProcessID)
{
	HANDLE hSteamProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, iSteamProcessID);
	if (hSteamProcess == NULL)
	{
		MessageBoxA(hWindow, "Unable to open Steam process.", "NetHook2", MB_OK | MB_ICONASTERISK);
		return false;
	}

	HMODULE hKernel32Module = GetModuleHandleA("kernel32.dll");
	if (hKernel32Module == NULL)
	{
		MessageBoxA(hWindow, "Unable to open load kernel32.dll.", "NetHook2", MB_OK | MB_ICONASTERISK);
		CloseHandle(hSteamProcess);
		return false;
	}

	struct EjectParams params;
	
	LPVOID pEjectParams = VirtualAllocEx(hSteamProcess, NULL, sizeof(struct EjectParams), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (pEjectParams == NULL)
	{
		MessageBoxA(hWindow, "Unable to allocate memory inside Steam.", "NetHook2", MB_OK | MB_ICONASTERISK);
		CloseHandle(hSteamProcess);
		return false;
	}

	params.FreeLibrary = (FreeLibraryPtr)GetProcAddress(hKernel32Module, "FreeLibrary");
	params.GetModuleHandleA = (GetModuleHandleAPtr)GetProcAddress(hKernel32Module, "GetModuleHandleA");
	strcpy(params.szModuleName, "NetHook2");

	BOOL bWritten = WriteProcessMemory(hSteamProcess, pEjectParams, &params, sizeof(params), NULL);
	if (!bWritten)
	{
		MessageBoxA(hWindow, "Unable to write to allocated memory inside Steam.", "NetHook2", MB_OK | MB_ICONASTERISK);
		VirtualFreeEx(hSteamProcess, pEjectParams, 0, MEM_RELEASE);
		CloseHandle(hSteamProcess);
		return false;
	}

	int cubRemoteFunc = 0x1000; // Be generous, we can't precisely measure this.
	LPVOID pRemoteFunc = VirtualAllocEx(hSteamProcess, NULL, cubRemoteFunc, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (pRemoteFunc == NULL)
	{
		MessageBoxA(hWindow, "Unable to allocate executable memory inside Steam.", "NetHook2", MB_OK | MB_ICONASTERISK);
		CloseHandle(hSteamProcess);
		return false;
	}

	bWritten = WriteProcessMemory(hSteamProcess, pRemoteFunc, &_Eject, cubRemoteFunc, NULL);
	if (!bWritten)
	{
		MessageBoxA(hWindow, "Unable to write to executable allocated memory inside Steam.", "NetHook2", MB_OK | MB_ICONASTERISK);
		VirtualFreeEx(hSteamProcess, pRemoteFunc, 0, MEM_RELEASE);
		VirtualFreeEx(hSteamProcess, pEjectParams, 0, MEM_RELEASE);
		CloseHandle(hSteamProcess);
		return false;
	}

	HANDLE hRemoteThread = CreateRemoteThread(hSteamProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pRemoteFunc, pEjectParams , NULL, NULL);
	if (hRemoteThread == NULL)
	{
		MessageBoxA(hWindow, "Unable to create remote thread inside Steam.", "NetHook2", MB_OK | MB_ICONASTERISK);
		VirtualFreeEx(hSteamProcess, pRemoteFunc, 0, MEM_RELEASE);
		VirtualFreeEx(hSteamProcess, pEjectParams, 0, MEM_RELEASE);
		CloseHandle(hSteamProcess);
		return false;
	}

	if (WaitForSingleObject(hRemoteThread, 5000 /* milliseconds */) == WAIT_TIMEOUT)
	{
		MessageBoxA(hWindow, "Injection timed out.", "NetHook2", MB_OK | MB_ICONASTERISK);
		VirtualFreeEx(hSteamProcess, pRemoteFunc, 0, MEM_RELEASE);
		VirtualFreeEx(hSteamProcess, pEjectParams, 0, MEM_RELEASE);
		CloseHandle(hSteamProcess);
		return false;
	}

	VirtualFreeEx(hSteamProcess, pRemoteFunc, 0, MEM_RELEASE);
	VirtualFreeEx(hSteamProcess, pEjectParams, 0, MEM_RELEASE);

	CloseHandle(hSteamProcess);
	return true;
}
