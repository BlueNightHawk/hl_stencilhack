// BLUENIGHTHAWK : Stencil Buffer Hack

#include "cl_dll.h"
#include "PlatformHeaders.h"
#include <Psapi.h>
#include "cvardef.h"
#include "cl_util.h"

#include "SDL2/SDL.h"
#include "SDL2/SDL_video.h"
#include "SDL2/SDL_opengl.h"

#include <string>

extern cl_enginefunc_t gEngfuncs;
SDL_Window* firstwindow = NULL;
SDL_Window* window = NULL;
SDL_GLContext gl_context;

void HL_ImGUI_Draw();
int HL_ImGUI_ProcessEvent(void* data, SDL_Event* event);

int RegRead(const char* valuename);
bool RegWrite(const char* valuename, int value);
bool RegCreate(const char* subkey);

bool bQueueRestart = false;
bool bFullScreen = false;

int iWidth = 0, iHeight = 0;

void HL_ToggleFullScreen(SDL_Window* window, int mode);
bool Windowed();

// To draw imgui on top of Half-Life, we take a detour from certain engine's function into HL_ImGUI_Draw function
void HL_ImGUI_Init()
{
	// window = SDL_GetWindowFromID(1);
	firstwindow = SDL_GetWindowFromID(1);
	iWidth = RegRead("ScreenWidth");
	iHeight = RegRead("ScreenHeight");
	RegCreate("SDL_FullScreen");
	int iSDLFullscreen = RegRead("SDL_FullScreen");

	if (!Windowed())
	{
		RegWrite("ScreenWindowed", 1);
		RegWrite("SDL_FullScreen", 1);
		bQueueRestart = true;
		return;
	}
	else if (iSDLFullscreen == 1)
	{
		RegWrite("ScreenWindowed", 100);
		bFullScreen = true;
	}
		// One of the final steps before drawing a frame is calling SDL_GL_SwapWindow function
	// It must be prevented, so imgui could be drawn before calling SDL_GL_SwapWindow

	// This will hold the constant address of x86 CALL command, which looks like this
	// E8 FF FF FF FF
	// Last 4 bytes specify an offset from this address + 5 bytes of command itself
	unsigned int origin = NULL;

	// We're scanning 1 MB at the beginning of hw.dll for a certain sequence of bytes
	// Based on location of that sequnce, the location of CALL command is calculated
	MODULEINFO module_info;
	if (GetModuleInformation(GetCurrentProcess(), GetModuleHandle("hw.dll"), &module_info, sizeof(module_info)))
	{
		origin = (unsigned int)module_info.lpBaseOfDll;

		const int MEGABYTE = 1024 * 1024;
		char* slice = new char[MEGABYTE];
		ReadProcessMemory(GetCurrentProcess(), (const void*)origin, slice, MEGABYTE, NULL);

		unsigned char magic[] = {0x8B, 0x4D, 0x08, 0x83, 0xC4, 0x08, 0x89, 0x01, 0x5D, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0xA1};

		for (unsigned int i = 0; i < MEGABYTE - 16; i++)
		{
			bool sequenceIsMatching = memcmp(slice + i, magic, 16) == 0;
			if (sequenceIsMatching)
			{
				origin += i + 27;
				break;
			}
		}

		delete[] slice;

		char opCode[1];
		ReadProcessMemory(GetCurrentProcess(), (const void*)origin, opCode, 1, NULL);
		if (opCode[0] != 0xFFFFFFE8)
		{
			gEngfuncs.Con_DPrintf("Failed to embed ImGUI: expected CALL OP CODE, but it wasn't there\n");
			return;
		}
	}
	else
	{
		gEngfuncs.Con_DPrintf("Failed to embed ImGUI: failed to get hw.dll memory base address\n");
		return;
	}

	// create new window with stencil buffer


	// To make a detour, an offset to dedicated function must be calculated and then correctly replaced
	unsigned int detourFunctionAddress = (unsigned int)&HL_ImGUI_Draw;
	unsigned int offset = (detourFunctionAddress)-origin - 5;

	// The resulting offset must be little endian, so
	// 0x0A852BA1 => A1 2B 85 0A
	char offsetBytes[4];
	for (int i = 0; i < 4; i++)
	{
		offsetBytes[i] = (offset >> (i * 8));
	}

	// This is WinAPI call, blatantly overwriting the memory with raw pointer would crash the program
	// Notice the 1 byte offset from the origin
	WriteProcessMemory(GetCurrentProcess(), (void*)(origin + 1), offsetBytes, 4, NULL);

	// Setup window
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);

	SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_GetWindowFlags(firstwindow));

	window = SDL_CreateWindow(SDL_GetWindowTitle(firstwindow), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, iWidth, iHeight, window_flags);

	gl_context = SDL_GL_CreateContext(window);
	SDL_GL_MakeCurrent(window, gl_context);
	SDL_HideWindow(firstwindow);

	// Fix low fps on initial launch...
	SDL_MinimizeWindow(window);
	SDL_RestoreWindow(window);

	glDisable(GL_STENCIL_TEST);
	glStencilMask((GLuint)~0);
	glStencilFunc(GL_EQUAL, 0, ~0);
	glStencilOp(GL_KEEP, GL_INCR, GL_INCR);

	SDL_AddEventWatch(HL_ImGUI_ProcessEvent, NULL);
//	*firstwindow = *window;

	if (bFullScreen)
	{
		HL_ToggleFullScreen(firstwindow, 1);
		HL_ToggleFullScreen(window, 1);
	}

	//SDL_DestroyWindow(firstwindow);
}
void HL_ImGUI_Deinit()
{
	if (bFullScreen && RegRead("ScreenWindowed") == 1)
	{
		RegWrite("SDL_FullScreen", 0);
		RegWrite("ScreenWindowed", 1);
	}
	else if (bFullScreen && RegRead("ScreenWindowed") == 0)
	{
		RegWrite("SDL_FullScreen", 0);
		RegWrite("ScreenWindowed", 0);
	}
	else if (bFullScreen && RegRead("ScreenWindowed") == 100)
	{
		RegWrite("SDL_FullScreen", 0);
		RegWrite("ScreenWindowed", 0);
	}

	
    SDL_GL_DeleteContext(gl_context);
	SDL_DestroyWindow(window);
	SDL_DestroyWindow(firstwindow);

	SDL_DelEventWatch(HL_ImGUI_ProcessEvent, NULL);
}

void HL_ImGUI_Draw()
{
	static cvar_s* rawinput = nullptr;

	if (!rawinput)
		rawinput = gEngfuncs.pfnGetCvarPointer("m_rawinput");

	SDL_GL_SwapWindow(firstwindow);
	SDL_GL_SwapWindow(window);
}

int HL_ImGUI_ProcessEvent(void* data, SDL_Event* event)
{
	switch (event->type)
	{
	case SDL_WINDOWEVENT:
		switch (event->window.event)
		{
		case SDL_WINDOWEVENT_CLOSE: // exit game
			gEngfuncs.pfnClientCmd("quit");
			break;

		default:
			break;
		}
		break;
	}
	return 1;
}

// Read data from registry
BOOL readDwordValueRegistry(HKEY hKeyParent, LPCSTR subkey, LPCSTR valueName, DWORD* readData)
{
	HKEY hKey;
	DWORD Ret;
	// Check if the registry exists
	Ret = RegOpenKeyEx(
		hKeyParent,
		subkey,
		0,
		KEY_READ,
		&hKey);
	if (Ret == ERROR_SUCCESS)
	{
		DWORD data;
		DWORD len = sizeof(DWORD); // size of data
		Ret = RegQueryValueEx(
			hKey,
			valueName,
			NULL,
			NULL,
			(LPBYTE)(&data),
			&len);
		if (Ret == ERROR_SUCCESS)
		{
			RegCloseKey(hKey);
			(*readData) = data;
			return TRUE;
		}
		RegCloseKey(hKey);
		return TRUE;
	}
	else
	{
		return FALSE;
	}
}

int RegRead(const char* valuename)
{
	DWORD outvalue = 0;
	readDwordValueRegistry(HKEY_CURRENT_USER, "Software\\Valve\\Half-Life\\Settings", valuename, &outvalue);
	return (int)outvalue;
}

BOOL WriteInRegistry(HKEY hKeyParent, LPCSTR subkey, LPCSTR valueName, DWORD data)
{
	DWORD Ret; // use to check status
	HKEY hKey; // key
	// Open the key
	Ret = RegOpenKeyEx(
		hKeyParent,
		subkey,
		0,
		KEY_WRITE,
		&hKey);
	if (Ret == ERROR_SUCCESS)
	{
		// Set the value in key
		if (ERROR_SUCCESS !=
			RegSetValueEx(
				hKey,
				valueName,
				0,
				REG_DWORD,
				reinterpret_cast<BYTE*>(&data),
				sizeof(data)))
		{
			RegCloseKey(hKey);
			return FALSE;
		}
		// close the key
		RegCloseKey(hKey);
		return TRUE;
	}
	return FALSE;
}

bool RegWrite(const char* valuename, int value)
{
	return WriteInRegistry(HKEY_CURRENT_USER, "Software\\Valve\\Half-Life\\Settings", valuename, (DWORD)value);
}

BOOL CreateRegistryKey(HKEY hKeyParent, LPCSTR subkey)
{
	DWORD dwDisposition; // It verify new key is created or open existing key
	HKEY hKey;
	DWORD Ret;
	Ret =
		RegCreateKeyEx(
			hKeyParent,
			subkey,
			0,
			NULL,
			REG_OPTION_NON_VOLATILE,
			KEY_ALL_ACCESS,
			NULL,
			&hKey,
			&dwDisposition);
	if (Ret != ERROR_SUCCESS)
	{
		printf("Error opening or creating new key\n");
		return FALSE;
	}
	RegCloseKey(hKey); // close the key
	return TRUE;
}

bool RegCreate(const char*subkey)
{
	return CreateRegistryKey(HKEY_CURRENT_USER, ((std::string)("Software\\Valve\\Half-Life\\Settings\\") + subkey).c_str());
}

