//========= Copyright © 1996-2002, Valve LLC, All rights reserved. ============
//
// Purpose:
//
// $NoKeywords: $
//=============================================================================

// Triangle rendering, if any

#include "hud.h"
#include "cl_util.h"

// Triangle rendering apis are in gEngfuncs.pTriAPI

#include "const.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "triangleapi.h"
#include "Exports.h"

#include "particleman.h"
#include "tri.h"
extern IParticleMan* g_pParticleMan;

// buz start

#include "PlatformHeaders.h"
#include "SDL2/SDL_opengl.h"

#include "r_studioint.h"
extern engine_studio_api_t IEngineStudio;

#include "com_model.h"
#include "studio.h"

#include "StudioModelRenderer.h"
#include "GameStudioModelRenderer.h"
extern CGameStudioModelRenderer g_StudioRenderer;

#define SURF_PLANEBACK 2
#define SURF_DRAWSKY 4
#define SURF_DRAWSPRITE 8
#define SURF_DRAWTURB 0x10
#define SURF_DRAWTILED 0x20
#define SURF_DRAWBACKGROUND 0x40
#define SURF_UNDERWATER 0x80
#define SURF_DONTWARP 0x100
#define BACKFACE_EPSILON 0.01

// 0-2 are axial planes
#define PLANE_X 0
#define PLANE_Y 1
#define PLANE_Z 2

// buz end

// buz start

void ClearBuffer(void);
extern bool g_bShadows;

mleaf_t* Mod_PointInLeaf(Vector p, model_t* model) // quake's func
{
	mnode_t* node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return (mleaf_t*)node;
		mplane_t* plane = node->plane;
		float d = DotProduct(p, plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return NULL; // never reached
}

model_t* g_pworld;
int g_visframe;
int g_framecount;
Vector g_lightvec;
Vector g_dynlightvec;
void RecursiveDrawWorld(mnode_t* node, model_s* pmodel)
{
	if (node->contents == CONTENTS_SOLID)
		return;

	if (node->visframe != g_visframe)
		return;

	if (node->contents < 0)
		return; // faces already marked by engine

	// recurse down the children, Order doesn't matter
	RecursiveDrawWorld(node->children[0],pmodel);
	RecursiveDrawWorld(node->children[1],pmodel);

	// draw stuff
	int c = node->numsurfaces;
	if (c)
	{
		msurface_t* surf = pmodel->surfaces + node->firstsurface;

		for (; c; c--, surf++)
		{
			if (surf->visframe != g_framecount)
				continue;

			if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_UNDERWATER))
				continue;

			// cull from light vector

			float dot;
			float dotdyn;
			mplane_t* plane = surf->plane;

			
			if (g_dynlightvec != g_vecZero)
			{
				switch (plane->type)
				{
				case PLANE_X:
					dot = g_dynlightvec[0];
					break;
				case PLANE_Y:
					dot = g_dynlightvec[0];
					break;
				case PLANE_Z:
					dot = g_dynlightvec[0];
					break;
				default:
					dot = DotProduct(g_dynlightvec, plane->normal);
					break;
				}
			}
			else
			{
				switch (plane->type)
				{
				case PLANE_X:
					dot = g_lightvec[0];
					break;
				case PLANE_Y:
					dot = g_lightvec[1];
					break;
				case PLANE_Z:
					dot = g_lightvec[2];
					break;
				default:
					dot = DotProduct(g_lightvec, plane->normal);
					break;
				}
			}

			if ((dot > 0) && (surf->flags & SURF_PLANEBACK))
				continue;

			if ((dot < 0) && !(surf->flags & SURF_PLANEBACK))
				continue;

			glpoly_t* p = surf->polys;
			float* v = p->verts[0];

			glBegin(GL_POLYGON);
			for (int i = 0; i < p->numverts; i++, v += VERTEXSIZE)
			{
				glTexCoord2f(v[3], v[4]);
				glVertex3fv(v);
			}
			glEnd();
		}
	}
}

// buz end


// TEXTURES
unsigned int g_uiScreenTex = 0;
unsigned int g_uiGlowTex = 0;

// FUNCTIONS
bool InitScreenGlow(void);
void RenderScreenGlow(void);
void DrawQuad(int width, int height, int ofsX = 0, int ofsY = 0);

cvar_t *glow_blur_steps, *glow_darken_steps, *glow_strength;

bool InitScreenGlow(void)
{
	// register the CVARs
	glow_blur_steps = gEngfuncs.pfnRegisterVariable("glow_blur_steps", "2", FCVAR_ARCHIVE);
	glow_darken_steps = gEngfuncs.pfnRegisterVariable("glow_darken_steps", "3", FCVAR_ARCHIVE);
	glow_strength = gEngfuncs.pfnRegisterVariable("glow_strength", "1", FCVAR_ARCHIVE);

	return true;
}

bool VidInitScreenGlow()
{
	// create a load of blank pixels to create textures with
	unsigned char* pBlankTex = new unsigned char[ScreenWidth * ScreenHeight * 3];
	memset(pBlankTex, 0, ScreenWidth * ScreenHeight * 3);

	// Create the SCREEN-HOLDING TEXTURE
	glGenTextures(1, &g_uiScreenTex);
	glBindTexture(GL_TEXTURE_RECTANGLE_NV, g_uiScreenTex);
	glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_RECTANGLE_NV, 0, GL_RGB8, ScreenWidth, ScreenHeight, 0, GL_RGB8, GL_UNSIGNED_BYTE, pBlankTex);

	// Create the BLURRED TEXTURE
	glGenTextures(1, &g_uiGlowTex);
	glBindTexture(GL_TEXTURE_RECTANGLE_NV, g_uiGlowTex);
	glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_RECTANGLE_NV, 0, GL_RGB8, ScreenWidth / 2, ScreenHeight / 2, 0, GL_RGB8, GL_UNSIGNED_BYTE, pBlankTex);

	// free the memory
	delete[] pBlankTex;

	return true;
}


void DrawQuad(int width, int height, int ofsX, int ofsY)
{
	glTexCoord2f(ofsX, ofsY);
	glVertex3f(0, 1, -1);
	glTexCoord2f(ofsX, height + ofsY);
	glVertex3f(0, 0, -1);
	glTexCoord2f(width + ofsX, height + ofsY);
	glVertex3f(1, 0, -1);
	glTexCoord2f(width + ofsX, ofsY);
	glVertex3f(1, 1, -1);
}

void RenderScreenGlow(void)
{
	// check to see if (a) we can render it, and (b) we're meant to render it

	if (IEngineStudio.IsHardware() != 1)
		return;

	if ((int)glow_blur_steps->value == 0 || (int)glow_strength->value == 0)
		return;

	// enable some OpenGL stuff
	glEnable(GL_TEXTURE_RECTANGLE_NV);
	glColor3f(1, 1, 1);
	glDisable(GL_DEPTH_TEST);

     // STEP 1: Grab the screen and put it into a texture

	glBindTexture(GL_TEXTURE_RECTANGLE_NV, g_uiScreenTex);
	glCopyTexImage2D(GL_TEXTURE_RECTANGLE_NV, 0, GL_RGB, 0, 0, ScreenWidth, ScreenHeight, 0);

     // STEP 2: Set up an orthogonal projection

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, 1, 1, 0, 0.1, 100);

     // STEP 3: Render the current scene to a new, lower-res texture, darkening non-bright areas of the scene
	// by multiplying it with itself a few times.

	glViewport(0, 0, ScreenWidth / 2, ScreenHeight / 2);

	glBindTexture(GL_TEXTURE_RECTANGLE_NV, g_uiScreenTex);

	glBlendFunc(GL_DST_COLOR, GL_ZERO);

	glDisable(GL_BLEND);

	glBegin(GL_QUADS);
	DrawQuad(ScreenWidth, ScreenHeight);
	glEnd();

	glEnable(GL_BLEND);

	glBegin(GL_QUADS);
	for (int i = 0; i < (int)glow_darken_steps->value; i++)
		DrawQuad(ScreenWidth, ScreenHeight);
	glEnd();

	glBindTexture(GL_TEXTURE_RECTANGLE_NV, g_uiGlowTex);
	glCopyTexImage2D(GL_TEXTURE_RECTANGLE_NV, 0, GL_RGB, 0, 0, ScreenWidth / 2, ScreenHeight / 2, 0);


     // STEP 4: Blur the now darkened scene in the horizontal direction.

	float blurAlpha = 1 / (glow_blur_steps->value * 2 + 1);

	glColor4f(1, 1, 1, blurAlpha);

	glBlendFunc(GL_SRC_ALPHA, GL_ZERO);

	glBegin(GL_QUADS);
	DrawQuad(ScreenWidth / 2, ScreenHeight / 2);
	glEnd();

	glBlendFunc(GL_SRC_ALPHA, GL_ONE);

	glBegin(GL_QUADS);
	for (int i = 1; i <= (int)glow_blur_steps->value; i++)
	{
		DrawQuad(ScreenWidth / 2, ScreenHeight / 2, -i, 0);
		DrawQuad(ScreenWidth / 2, ScreenHeight / 2, i, 0);
	}
	glEnd();

	glCopyTexImage2D(GL_TEXTURE_RECTANGLE_NV, 0, GL_RGB, 0, 0, ScreenWidth / 2, ScreenHeight / 2, 0);

     // STEP 5: Blur the horizontally blurred image in the vertical direction.

	glBlendFunc(GL_SRC_ALPHA, GL_ZERO);

	glBegin(GL_QUADS);
	DrawQuad(ScreenWidth / 2, ScreenHeight / 2);
	glEnd();

	glBlendFunc(GL_SRC_ALPHA, GL_ONE);

	glBegin(GL_QUADS);
	for (int i = 1; i <= (int)glow_blur_steps->value; i++)
	{
		DrawQuad(ScreenWidth / 2, ScreenHeight / 2, 0, -i);
		DrawQuad(ScreenWidth / 2, ScreenHeight / 2, 0, i);
	}
	glEnd();

	glCopyTexImage2D(GL_TEXTURE_RECTANGLE_NV, 0, GL_RGB, 0, 0, ScreenWidth / 2, ScreenHeight / 2, 0);

     // STEP 6: Combine the blur with the original image.

	glViewport(0, 0, ScreenWidth, ScreenHeight);

	glDisable(GL_BLEND);

	glBegin(GL_QUADS);
	DrawQuad(ScreenWidth / 2, ScreenHeight / 2);
	glEnd();

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);

	glBegin(GL_QUADS);
	for (int i = 1; i < (int)glow_strength->value; i++)
	{
		DrawQuad(ScreenWidth / 2, ScreenHeight / 2);
	}
	glEnd();

	glBindTexture(GL_TEXTURE_RECTANGLE_NV, g_uiScreenTex);

	glBegin(GL_QUADS);
	DrawQuad(ScreenWidth, ScreenHeight);
	glEnd();

     // STEP 7: Restore the original projection and modelview matrices and disable rectangular textures.

	glMatrixMode(GL_PROJECTION);
	glPopMatrix();

	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();

	glDisable(GL_TEXTURE_RECTANGLE_NV);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
}

PFNGLACTIVETEXTUREARBPROC glActiveTextureARB = NULL;

model_s* GetModelByIndex(int i);
	/*
=================
HUD_DrawNormalTriangles

Non-transparent triangles-- add them here
=================
*/
void DLLEXPORT HUD_DrawNormalTriangles()
{
	gHUD.m_Spectator.DrawOverview();
}


/*
=================
HUD_DrawTransparentTriangles

Render any triangles with transparent rendermode needs here
=================
*/
void DLLEXPORT HUD_DrawTransparentTriangles()
{
	ClearBuffer();

	// buz start
	// òóò ìîæíî áþëî ïðîñòî íàðèñîâàòü ñåðûé êâàäðàò íà âåñü ýêðàí, êàê ýòî ÷àñòî äåëàþò
	// â ñòåíñèëüíûõ òåíÿõ òàêîãî ðîäà, îäíàêî âìåñòî ýòîãî ÿ ïðîáåãàþñü ïî ïîëèãîíàì world'à,
	// è ðèñóþ ñåðûì òîëüêî òå, êîòîðûå îáðàùåíû ê "ñîëíûøêó", ÷òîáû òåíü íå ðèñîâàëàñü
	// íà "îáðàòíûõ" ñòåíêàõ.
	if (g_bShadows && IEngineStudio.IsHardware())
	{
		if (NULL == glActiveTextureARB)
			glActiveTextureARB = (PFNGLACTIVETEXTUREARBPROC)wglGetProcAddress("glActiveTextureARB");

		glPushAttrib(GL_ALL_ATTRIB_BITS);

		// buz: workaround half-life's bug, when multitexturing left enabled after
		// rendering brush entities
		glActiveTextureARB(GL_TEXTURE1_ARB);
		glDisable(GL_TEXTURE_2D);
		glActiveTextureARB(GL_TEXTURE0_ARB);

		glDepthMask(GL_FALSE);
		glDisable(GL_TEXTURE_2D);
		glDisable(GL_CULL_FACE);
		glEnable(GL_BLEND);
		glBlendFunc(GL_DST_COLOR, GL_ZERO);
		glColor4f(0.5, 0.5, 0.5, 1);

		glStencilFunc(GL_NOTEQUAL, 0, ~0);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		glEnable(GL_STENCIL_TEST);

		int i = 1;
		// get current visframe number
		g_pworld = gEngfuncs.GetEntityByIndex(0)->model;
#if 0
		model_s* pmdl = GetModelByIndex(0);
		while (pmdl)
		{
			pmdl = GetModelByIndex(i);
			if (pmdl && pmdl->type == mod_brush)
				g_pworld = pmdl;
			i++;
		}
#endif
		mleaf_t* leaf = Mod_PointInLeaf(g_StudioRenderer.m_vRenderOrigin, g_pworld);
		g_visframe = leaf->visframe;

		// get current frame number
		g_framecount = g_StudioRenderer.m_nFrameCount;

		// get light vector
		g_StudioRenderer.GetShadowVector(g_lightvec);

		g_dynlightvec = g_vecZero;

		// draw world
		RecursiveDrawWorld(g_pworld->nodes, g_pworld);

		// get light vector
		g_dynlightvec = g_StudioRenderer.m_ShadowDir;

		// draw world
		if (g_dynlightvec != g_vecZero)
			RecursiveDrawWorld(g_pworld->nodes, g_pworld);

		glPopAttrib();
	}

	g_bShadows = false;
	// buz end
	if (g_pParticleMan)
		g_pParticleMan->Update();
}
