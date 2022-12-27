/***
*
*	Copyright (c) 2012, AGHL.RU. All rights reserved.
*
****/
//
// Memory.cpp
//
// Runtime memory searching/patching
//

#include "PlatformHeaders.h"
#include <psapi.h>
#include <process.h>
#include <tlhelp32.h>
#include <Winternl.h>

#include "hud.h"
#include "memory.h"
#include "cvardef.h"
#include "cl_util.h"
#include "parsemsg.h"

#include "SDL2/SDL.h"
#include "SDL2/SDL_video.h"

#define MAX_PATTERN 128


typedef void (__fastcall *ThisCallIntInt)(void *, int, int, int);


bool g_bPatchStatusPrinted = false;
char g_szPatchErrors[2048];

/* Engine addresses */
size_t g_EngineModuleBase = 0, g_EngineModuleEnd = 0;

/* GameUI fix variables */
int g_iLinesCounter = 0;
ThisCallIntInt g_pFunctionReplacedByCounter;
ThisCallIntInt g_pFunctionReplacedBySubst;
size_t (*g_pGet_VGUI_System009)(void);
size_t *g_pSystem = 0;



/* Videomode variables */
bool g_bGotVideoModeData = false;
HWND g_hWnd = NULL;
size_t g_pVideoAbstraction = NULL;
bool g_bNewerBuild = false;
bool g_bLockedFullscreen = false;
int m_iDisplayFrequency;

#define ThreadQuerySetWin32StartAddress 9
typedef NTSTATUS NTAPI NtQueryInformationThreadProto(HANDLE ThreadHandle, THREADINFOCLASS ThreadInformationClass, PVOID ThreadInformation, ULONG ThreadInformationLength, PULONG ReturnLength);

extern SDL_Window* window, *firstwindow;

bool GetModuleAddress(const char *moduleName, size_t &moduleBase, size_t &moduleEnd)
{
	HANDLE hProcess = GetCurrentProcess();
	HMODULE hModuleDll = GetModuleHandle(moduleName);
	if (!hProcess || !hModuleDll) return false;
	MODULEINFO moduleInfo;
	GetModuleInformation(hProcess, hModuleDll, &moduleInfo, sizeof(moduleInfo));
	moduleBase = (size_t)moduleInfo.lpBaseOfDll;
	moduleEnd = (size_t)moduleInfo.lpBaseOfDll + (size_t)moduleInfo.SizeOfImage - 1;
	return true;
}
// Searches for engine address in memory
void GetEngineModuleAddress(void)
{
	if (GetModuleAddress("hw.dll", g_EngineModuleBase, g_EngineModuleEnd) ||	// Try Hardware engine
		GetModuleAddress("sw.dll", g_EngineModuleBase, g_EngineModuleEnd) ||	// Try Software engine
		GetModuleAddress("hl.exe", g_EngineModuleBase, g_EngineModuleEnd))		// Try Encrypted engine
		return;
	// Get process base module name in case it differs from hl.exe
	char moduleName[256];
	if (!GetModuleFileName(NULL, moduleName, ARRAYSIZE(moduleName)))
		return;
	char *baseName = strrchr(moduleName, '\\');
	if (baseName == NULL)
		return;
	baseName++;
	GetModuleAddress(baseName, g_EngineModuleBase, g_EngineModuleEnd);
}

// Converts HEX string containing pairs of symbols 0-9, A-F, a-f with possible space splitting into byte array
size_t ConvertHexString(const char *srcHexString, unsigned char *outBuffer, size_t bufferSize)
{
	unsigned char *in = (unsigned char *)srcHexString;
	unsigned char *out = outBuffer;
	unsigned char *end = outBuffer + bufferSize;
	bool low = false;
	uint8_t byte = 0;
	while (*in && out < end)
	{
		if (*in >= '0' && *in <= '9') { byte |= *in - '0'; }
		else if (*in >= 'A' && *in <= 'F') { byte |= *in - 'A' + 10; }
		else if (*in >= 'a' && *in <= 'f') { byte |= *in - 'a' + 10; }
		else if (*in == ' ') { in++; continue; }

		if (!low)
		{
			byte = byte << 4;
			in++;
			low = true;
			continue;
		}
		low = false;

		*out = byte;
		byte = 0;

		in++;
		out++;
	}
	return out - outBuffer;
}
size_t MemoryFindForward(size_t start, size_t end, const unsigned char *pattern, const unsigned char *mask, size_t pattern_len)
{
	// Ensure start is lower than the end
	if (start > end)
	{
		size_t reverse = end;
		end = start;
		start = reverse;
	}

	unsigned char *cend = (unsigned char*)(end - pattern_len + 1);
	unsigned char *current = (unsigned char*)(start);

	// Just linear search for sequence of bytes from the start till the end minus pattern length
	size_t i;
	if (mask)
	{
		// honoring mask
		while (current < cend)
		{
			for (i = 0; i < pattern_len; i++)
			{
				if ((current[i] & mask[i]) != (pattern[i] & mask[i]))
					break;
			}

			if (i == pattern_len)
				return (size_t)(void*)current;

			current++;
		}
	}
	else
	{
		// without mask
		while (current < cend)
		{
			for (i = 0; i < pattern_len; i++)
			{
				if (current[i] != pattern[i])
					break;
			}

			if (i == pattern_len)
				return (size_t)(void*)current;

			current++;
		}
	}

	return NULL;
}
// Signed char versions assume pattern and mask are in HEX string format and perform conversions
size_t MemoryFindForward(size_t start, size_t end, const char *pattern, const char *mask)
{
	unsigned char p[MAX_PATTERN];
	unsigned char m[MAX_PATTERN];
	size_t pl = ConvertHexString(pattern, p, sizeof(p));
	size_t ml = mask != NULL ? ConvertHexString(mask, m, sizeof(m)) : 0;
	return MemoryFindForward(start, end, p, mask != NULL ? m : NULL, pl >= ml ? pl : ml);
}
size_t MemoryFindBackward(size_t start, size_t end, const unsigned char *pattern, const unsigned char *mask, size_t pattern_len)
{
	// Ensure start is higher than the end
	if (start < end)
	{
		size_t reverse = end;
		end = start;
		start = reverse;
	}

	unsigned char *cend = (unsigned char*)(end);
	unsigned char *current = (unsigned char*)(start - pattern_len);

	// Just linear search backward for sequence of bytes from the start minus pattern length till the end
	size_t i;
	if (mask)
	{
		// honoring mask
		while (current >= cend)
		{
			for (i = 0; i < pattern_len; i++)
			{
				if ((current[i] & mask[i]) != (pattern[i] & mask[i]))
					break;
			}

			if (i == pattern_len)
				return (size_t)(void*)current;

			current--;
		}
	}
	else
	{
		// without mask
		while (current >= cend)
		{
			for (i = 0; i < pattern_len; i++)
			{
				if (current[i] != pattern[i])
					break;
			}

			if (i == pattern_len)
				return (size_t)(void*)current;

			current--;
		}
	}

	return NULL;
}
// Signed char versions assume pattern and mask are in HEX string format and perform conversions
size_t MemoryFindBackward(size_t start, size_t end, const char *pattern, const char *mask)
{
	unsigned char p[MAX_PATTERN];
	unsigned char m[MAX_PATTERN];
	size_t pl = ConvertHexString(pattern, p, sizeof(p));
	size_t ml = mask != NULL ? ConvertHexString(mask, m, sizeof(m)) : 0;
	return MemoryFindBackward(start, end, p, mask != NULL ? m : NULL, pl >= ml ? pl : ml);
}

// Replaces double word on specified address with new dword, returns old dword
uint32_t HookDWord(size_t origAddr, uint32_t newDWord)
{
	DWORD oldProtect;
	uint32_t origDWord = *(size_t *)origAddr;
	VirtualProtect((size_t *)origAddr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
	*(size_t *)origAddr = newDWord;
	VirtualProtect((size_t *)origAddr, 4, oldProtect, &oldProtect);
	return origDWord;
}
// Exchanges bytes between memory address and bytes array
void ExchangeMemoryBytes(size_t *origAddr, size_t *dataAddr, uint32_t size)
{
	DWORD oldProtect;
	VirtualProtect(origAddr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
	unsigned char data[MAX_PATTERN];
	int32_t iSize = size;
	while (iSize > 0)
	{
		size_t s = iSize <= MAX_PATTERN ? iSize : MAX_PATTERN;
		memcpy(data, origAddr, s);
		memcpy((void *)origAddr, (void *)dataAddr, s);
		memcpy((void *)dataAddr, data, s);
		iSize -= MAX_PATTERN;
	}
	VirtualProtect(origAddr, size, oldProtect, &oldProtect);
}


// Videomode changing code
BOOL __stdcall EnumWindowsCallback(HWND hWnd, LPARAM lParam)
{
	DWORD windowPID;
	GetWindowThreadProcessId(hWnd, &windowPID);
	if (windowPID == (DWORD)lParam)
	{
		char className[32];
		GetClassName(hWnd, className, sizeof(className));
		if (strcmp(className, "Valve001") == 0)//"SDL_app"
		{
			g_hWnd = hWnd;
		}
	}
	return TRUE;
}

extern int iWidth, iHeight;

void __CmdFunc_ToggleFullScreen(void)
{
	if (!g_bGotVideoModeData)
	{
		gEngfuncs.Con_Printf("Toggle full screen feature works only in OpenGL or D3D mode.\n");
		return;
	}

	bool *windowed = (bool *)(g_pVideoAbstraction + 440);

	int argi = -1;
	const int argc = gEngfuncs.Cmd_Argc();
	if (argc > 1)
	{
		const char *args = gEngfuncs.Cmd_Argv(1);
		if (args && args[0])
		{
			if (_stricmp(args, "lock") == 0)
			{
				if (g_bNewerBuild)
				{
					gEngfuncs.Con_Printf("Lock full screen feature works only on old builds (non-SDL).\n");
					return;
				}
				if (g_bLockedFullscreen)
				{
					// toggle style
					*windowed = false;
					g_bLockedFullscreen = false;
					gEngfuncs.Con_Printf("Full screen mode unlocked.\n");
					return;
				}
				if (*windowed)
				{
					gEngfuncs.Con_Printf("Can't lock, should be in full screen mode.\n");
					return;
				}
				*windowed = true;
				g_bLockedFullscreen = true;
				gEngfuncs.Con_Printf("Full screen mode locked.\n");
				return;
			}
			if (_stricmp(args, "unlock") == 0)
			{
				if (g_bNewerBuild)
				{
					gEngfuncs.Con_Printf("Lock full screen feature works only on old builds (non-SDL).\n");
					return;
				}
				if (g_bLockedFullscreen)
				{
					*windowed = false;
					g_bLockedFullscreen = false;
					gEngfuncs.Con_Printf("Full screen mode unlocked.\n");
				}
				return;
			}
			argi = atoi(args);
		}
	}

	if (g_bLockedFullscreen)
	{
		*windowed = false;
		g_bLockedFullscreen = false;
		gEngfuncs.Con_Printf("Full screen mode unlocked.\n");
	}

	int width = iWidth;
	int height = iHeight;

	bool success;
	if (argi == 1 || argi == 2 || (argi == -1 && *windowed))
	{
		if (!g_bNewerBuild)
		{
			if (argi != 2)
			{
				// Change display mode to fullscreen
				DEVMODE dm;
				dm.dmSize = sizeof(DEVMODE);
				dm.dmPelsWidth = width;
				dm.dmPelsHeight = height;
				dm.dmBitsPerPel = 32;
				dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
				dm.dmDisplayFrequency = m_iDisplayFrequency;
				dm.dmFields |= DM_DISPLAYFREQUENCY;
				success = ChangeDisplaySettings(&dm, 0) == DISP_CHANGE_SUCCESSFUL;
				if (!success)
				{
					dm.dmDisplayFrequency = 0;
					dm.dmFields &= ~DM_DISPLAYFREQUENCY;
					success = ChangeDisplaySettings(&dm, 0) == DISP_CHANGE_SUCCESSFUL;
					if (!success)
					{
						ConsolePrint("Failed to change video mode to full screen.\n");
						return;
					}
				}
				// Normal fullscreen
				SetWindowLongPtr(g_hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS);
				MoveWindow(g_hWnd, 0, 0, width, height, TRUE);
			}
			else
			{
				if (!*windowed)
				{
					// Reset display mode
					success = ChangeDisplaySettings(0, 0) == DISP_CHANGE_SUCCESSFUL;
					if (!success)
					{
						ConsolePrint("Failed to reset video mode.\n");
						return;
					}
				}
				// Borderless fullscreen
				SetWindowLongPtr(g_hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_SYSMENU | WS_MINIMIZEBOX);
				MoveWindow(g_hWnd, 0, 0, width, height, TRUE);
			}
		}
		else
		{
			// Zero position if centered
			int x, y;
			SDL_GetWindowPosition(window, &x, &y);
			if (SDL_WINDOWPOS_ISCENTERED(x) && SDL_WINDOWPOS_ISCENTERED(y))
				SDL_SetWindowPosition(window, 0, 0);
			if (argi != 2)
			{
				if (!*windowed)
					SDL_SetWindowFullscreen(window, 0);
				SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
			}
			else
			{
				if (!*windowed)
					SDL_SetWindowFullscreen(window, 0);
				SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
			}
		}

		*windowed = false;
	}
	else // argi == 0 || !*windowed
	{
		if (!g_bNewerBuild)
		{
			// Reset display mode
			success = ChangeDisplaySettings(0, 0) == DISP_CHANGE_SUCCESSFUL;
			if (!success)
			{
				ConsolePrint("Failed to reset video mode.\n");
				return;
			}

			RECT rect;
			GetClientRect(GetDesktopWindow(), &rect);
			rect.left = rect.right / 2 - width / 2;
			rect.top = rect.bottom / 2 - height / 2;
			rect.right = rect.left + width;
			rect.bottom = rect.top + height;

			SetWindowLongPtr(g_hWnd, GWL_STYLE, WS_CAPTION | WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_SYSMENU | WS_MINIMIZEBOX);
			AdjustWindowRect(&rect, WS_CAPTION | WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
			MoveWindow(g_hWnd, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
		}
		else
		{
			SDL_SetWindowFullscreen(window, 0);
			// Position to the desktop center
			int x, y;
			SDL_GetWindowPosition(window, &x, &y);
			if (x <= 0 && y <= 0)
				SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
		}

		*windowed = true;
	}
}

bool Windowed()
{
	bool* windowed = (bool*)(g_pVideoAbstraction + 440);
	return *windowed;
}

void HL_ToggleFullScreen(SDL_Window*window, int mode)
{
	if (!g_bGotVideoModeData)
	{
		gEngfuncs.Con_Printf("Toggle full screen feature works only in OpenGL or D3D mode.\n");
		return;
	}

	bool* windowed = (bool*)(g_pVideoAbstraction + 440);

	int argi = mode;

	int width = iWidth;
	int height = iHeight;

	bool success;
	if (argi == 1 || argi == 2 || (argi == -1 && *windowed))
	{
		if (!g_bNewerBuild)
		{
			if (argi != 2)
			{
				// Change display mode to fullscreen
				DEVMODE dm;
				dm.dmSize = sizeof(DEVMODE);
				dm.dmPelsWidth = width;
				dm.dmPelsHeight = height;
				dm.dmBitsPerPel = 32;
				dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
				dm.dmDisplayFrequency = m_iDisplayFrequency;
				dm.dmFields |= DM_DISPLAYFREQUENCY;
				success = ChangeDisplaySettings(&dm, 0) == DISP_CHANGE_SUCCESSFUL;
				if (!success)
				{
					dm.dmDisplayFrequency = 0;
					dm.dmFields &= ~DM_DISPLAYFREQUENCY;
					success = ChangeDisplaySettings(&dm, 0) == DISP_CHANGE_SUCCESSFUL;
					if (!success)
					{
						ConsolePrint("Failed to change video mode to full screen.\n");
						return;
					}
				}
				// Normal fullscreen
				SetWindowLongPtr(g_hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS);
				MoveWindow(g_hWnd, 0, 0, width, height, TRUE);
			}
			else
			{
				if (!*windowed)
				{
					// Reset display mode
					success = ChangeDisplaySettings(0, 0) == DISP_CHANGE_SUCCESSFUL;
					if (!success)
					{
						ConsolePrint("Failed to reset video mode.\n");
						return;
					}
				}
				// Borderless fullscreen
				SetWindowLongPtr(g_hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_SYSMENU | WS_MINIMIZEBOX);
				MoveWindow(g_hWnd, 0, 0, width, height, TRUE);
			}
		}
		else
		{
			// Zero position if centered
			int x, y;
			SDL_GetWindowPosition(window, &x, &y);
			if (SDL_WINDOWPOS_ISCENTERED(x) && SDL_WINDOWPOS_ISCENTERED(y))
				SDL_SetWindowPosition(window, 0, 0);
			if (argi != 2)
			{
				if (!*windowed)
					SDL_SetWindowFullscreen(window, 0);
				SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
			}
			else
			{
				if (!*windowed)
					SDL_SetWindowFullscreen(window, 0);
				SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
			}
		}

		*windowed = false;
	}
	else // argi == 0 || !*windowed
	{
		if (!g_bNewerBuild)
		{
			// Reset display mode
			success = ChangeDisplaySettings(0, 0) == DISP_CHANGE_SUCCESSFUL;
			if (!success)
			{
				ConsolePrint("Failed to reset video mode.\n");
				return;
			}

			RECT rect;
			GetClientRect(GetDesktopWindow(), &rect);
			rect.left = rect.right / 2 - width / 2;
			rect.top = rect.bottom / 2 - height / 2;
			rect.right = rect.left + width;
			rect.bottom = rect.top + height;

			SetWindowLongPtr(g_hWnd, GWL_STYLE, WS_CAPTION | WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_SYSMENU | WS_MINIMIZEBOX);
			AdjustWindowRect(&rect, WS_CAPTION | WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
			MoveWindow(g_hWnd, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
		}
		else
		{
			SDL_SetWindowFullscreen(window, 0);
			// Position to the desktop center
			int x, y;
			SDL_GetWindowPosition(window, &x, &y);
			if (x <= 0 && y <= 0)
				SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
		}

		*windowed = true;
	}
}

void FindVideoModeData()
{
	// Find engine video abstraction object
	const char data1[] = "AVVideoMode_Direct3DWindowed";
	size_t addr1 = MemoryFindForward(g_EngineModuleBase, g_EngineModuleEnd, (unsigned char *)data1, NULL, sizeof(data1) - 1);
	if (!addr1)
	{
		// Software engine
		return;
	}
	const char data2[] = "No error";
	const char data3[] = "Generic failure";
	size_t addr2 = MemoryFindForward(addr1, addr1 + 64, (unsigned char *)data2, NULL, sizeof(data2));
	if (!addr2)
	{
		addr2 = MemoryFindForward(addr1, addr1 + 64, (unsigned char *)data3, NULL, sizeof(data3));
		if (!addr2)
		{
			strncat(g_szPatchErrors, "Video abstraction object not found: 1.\n", sizeof(g_szPatchErrors) - strlen(g_szPatchErrors) - 1);
			return;
		}
		g_bNewerBuild = true;
	}
	size_t ptr1 = *(size_t *)(addr2 - 4) - (g_bNewerBuild ? 4 : 8);
	if (ptr1 < g_EngineModuleBase || g_EngineModuleEnd < ptr1)
	{
		strncat(g_szPatchErrors, "Video abstraction object not found: 2.\n", sizeof(g_szPatchErrors) - strlen(g_szPatchErrors) - 1);
		return;
	}
	g_pVideoAbstraction = *(size_t *)ptr1;
	// Potentially unsafe, object is on heap
	if (*(size_t *)g_pVideoAbstraction < g_EngineModuleBase || g_EngineModuleEnd < *(size_t *)g_pVideoAbstraction)
	{
		strncat(g_szPatchErrors, "Video abstraction object not found: 3.\n", sizeof(g_szPatchErrors) - strlen(g_szPatchErrors) - 1);
		return;
	}
	char *name = (*(char*(**)(void)) (*(size_t *)g_pVideoAbstraction))();
	if (strcmp(name, "gl") != 0)
	{
		// Will output info in command handler
		return;
	}

	if (!g_bNewerBuild)
	{
		// Get window handle
		if (EnumWindows(&EnumWindowsCallback, GetCurrentProcessId()) == FALSE || g_hWnd == NULL)
		{
			strncat(g_szPatchErrors, "Failed to get window handle.\n", sizeof(g_szPatchErrors) - strlen(g_szPatchErrors) - 1);
			return;
		}
	}

	g_bGotVideoModeData = true;
}


// Applies engine patches
void PatchEngine(void)
{
	if (!g_EngineModuleBase) GetEngineModuleAddress();
	if (!g_EngineModuleBase)
	{
		strncat(g_szPatchErrors, "Engine patch: module base not found.\n", sizeof(g_szPatchErrors) - strlen(g_szPatchErrors) - 1);
		return;
	}

	FindVideoModeData();
}
// Removes engine patches
void UnPatchEngine(void)
{
}
