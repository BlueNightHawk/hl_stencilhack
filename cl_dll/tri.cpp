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

			if (g_dynlightvec != g_vecZero)
			{
				switch (plane->type)
				{
				case PLANE_X:
					dotdyn = g_dynlightvec[0];
					break;
				case PLANE_Y:
					dotdyn = g_dynlightvec[0];
					break;
				case PLANE_Z:
					dotdyn = g_dynlightvec[0];
					break;
				default:
					dotdyn = DotProduct(g_dynlightvec, plane->normal);
					break;
				}
				if ((dot > 0) && (surf->flags & SURF_PLANEBACK) && (dotdyn > 0))
					continue;

				if ((dot < 0) && !(surf->flags & SURF_PLANEBACK) && (dotdyn < 0))
					continue;
			}
			else
			{
				if ((dot > 0) && (surf->flags & SURF_PLANEBACK))
					continue;

				if ((dot < 0) && !(surf->flags & SURF_PLANEBACK))
					continue;
			}
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
	//	RecClDrawNormalTriangles();
	ClearBuffer();

	gHUD.m_Spectator.DrawOverview();

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

		// get current visframe number
		g_pworld = gEngfuncs.GetEntityByIndex(0)->model;
		mleaf_t* leaf = Mod_PointInLeaf(g_StudioRenderer.m_vRenderOrigin, g_pworld);
		g_visframe = leaf->visframe;

		// get current frame number
		g_framecount = g_StudioRenderer.m_nFrameCount;

		// get light vector
		g_StudioRenderer.GetShadowVector(g_lightvec);

		// get light vector
		g_dynlightvec = g_StudioRenderer.m_ShadowDir;

		// draw world
		RecursiveDrawWorld(g_pworld->nodes,g_pworld);

		// draw world
	//	RecursiveDrawWorld(g_pworld->nodes, g_pworld);

		glPopAttrib();
	}

	g_bShadows = false;
	// buz end
}


/*
=================
HUD_DrawTransparentTriangles

Render any triangles with transparent rendermode needs here
=================
*/
void DLLEXPORT HUD_DrawTransparentTriangles()
{
	//	RecClDrawTransparentTriangles();


	if (g_pParticleMan)
		g_pParticleMan->Update();
}
