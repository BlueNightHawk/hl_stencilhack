// BLUENIGHTHAWK : Stencil Buffer Hack

#include "cl_dll.h"
#include "PlatformHeaders.h"
#include <Psapi.h>

#include "SDL2/SDL.h"
#include "SDL2/SDL_video.h"
#include "SDL2/SDL_opengl.h"

extern cl_enginefunc_t gEngfuncs;
SDL_Window* firstwindow = NULL;
SDL_Window* window = NULL;
SDL_GLContext gl_context;

void HL_ImGUI_Draw();
int HL_ImGUI_ProcessEvent(void* data, SDL_Event* event);

// To draw imgui on top of Half-Life, we take a detour from certain engine's function into HL_ImGUI_Draw function
void HL_ImGUI_Init()
{
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

	firstwindow = SDL_GetWindowFromID(1);

	int iWidth, iHeight;

	SDL_GetWindowSize(firstwindow, &iWidth, &iHeight);

	// Setup window
//	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE); 
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8); 
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
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
}

void HL_ImGUI_Deinit()
{
    SDL_GL_DeleteContext(gl_context);
	SDL_DestroyWindow(window);
	SDL_DestroyWindow(firstwindow);

	SDL_DelEventWatch(HL_ImGUI_ProcessEvent, NULL);
}

void HL_ImGUI_Draw()
{
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