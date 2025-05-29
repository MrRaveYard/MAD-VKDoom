// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_light.cpp
** Light level / fog management / dynamic lights
**
*/

#include "c_dispatch.h"
#include "a_dynlight.h" 
#include "p_local.h"
#include "p_effect.h"
#include "g_level.h"
#include "g_levellocals.h"
#include "actorinlines.h"
#include "hw_drawcontext.h"
#include "hw_dynlightdata.h"
#include "hw_shadowmap.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "models.h"
#include <cmath>	// needed for std::floor on mac
#include "hw_cvars.h"

EXTERN_CVAR(Int, gl_cachespritetracelights);

template<class T>
T smoothstep(const T edge0, const T edge1, const T x)
{
	auto t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

class ActorTraceStaticLight
{
public:
	ActorTraceStaticLight(AActor* actor) : Actor(actor)
	{
		if (Actor && (Actor->Pos() != Actor->StaticLightsTraceCache.Pos || (Actor->Sector && (Actor->Sector->Flags & SECF_LM_DYNAMIC) && lm_dynamic)))
		{
			Actor->StaticLightsTraceCache.Pos = Actor->Pos();
			Actor->StaticLightsTraceCache.SunResult = false;
			ActorMoved = true;
		}
	}

	bool TraceLightVisbility(FLightNode* node, const FVector3& L, float dist, bool ignoreCache)
	{
		FDynamicLight* light = node->lightsource;
		if (!light->TraceActors() || !level.levelMesh || !Actor)
			return true;

		unsigned index = light->ActorList.SortedFind(Actor, false);

		if (!ignoreCache && !ActorMoved && index < light->ActorList.Size() && light->ActorList[index] == Actor)
		{
			bool traceResult = light->ActorResult[index];
			return traceResult;
		}
		else
		{

			bool traceResult = !level.levelMesh->Trace(FVector3((float)light->Pos.X, (float)light->Pos.Y, (float)light->Pos.Z), FVector3(-L.X, -L.Y, -L.Z), dist);
			if(index == light->ActorList.Size() || light->ActorList[index] != Actor)
			{
				light->ActorList.Insert(index, Actor);
				light->ActorResult.Insert(index, traceResult);
			}
			else
			{
				light->ActorResult[index] = traceResult;
			}
			return traceResult;
		}
	}

	static bool TraceSunVisibility(float x, float y, float z, sun_trace_cache_t *cache, bool moved)
	{
		if (!level.lightmaps || !cache)
			return false;

		if (!moved)
		{
			bool traceResult = cache->SunResult;
			return traceResult;
		}
		else
		{
			bool traceResult = level.levelMesh->TraceSky(FVector3(x, y, z), level.SunDirection, 65536.0f);
			cache->SunResult = traceResult;
			return traceResult;
		}
	}

	AActor* Actor;
	bool ActorMoved = false;
};

//==========================================================================
//
// Sets a single light value from all dynamic lights affecting the specified location
//
//==========================================================================

static float mix(float a, float b, float t)
{
	return a * (1.0f - t) + t * b;
}

float inverseSquareAttenuation(float dist, float radius, float strength, float linearity)
{
	float a = dist / radius;
	float b = clamp(1.0f - a * a * a * a, 0.0f, 1.0f);
	return mix(((b * b) / (dist * dist + 1.0f) * strength), clamp((radius - dist) / radius, 0.0f, 1.0f), linearity);
}

void HWDrawInfo::GetDynSpriteLight(AActor *self, sun_trace_cache_t * traceCache, double x, double y, double z, FLightNode *node, int portalgroup, float *out, bool fullbright, subsector_t* subsector)
{
	if (fullbright || get_gl_spritelight() > 0)
		return;

	if (gl_cachespritetracelights <= 0)
	{
		subsector = nullptr;
	}

	TraceLightVoxelCache::Probe* probe = subsector && subsector->traceLightCache ? &subsector->traceLightCache->Get(FVector3(float(x), float(y), float(z))) : nullptr;

	bool calculateTraceLights = subsector;

	if (probe && probe->isValid)
	{
		probe->Get(out[0], out[1], out[2]);
		calculateTraceLights = false;

		// Printf("%p | %p | %.2f %.2f %.2f | %d %d %d (CACHED)\n", subsector->traceLightCache, probe, out[0], out[1], out[2], probe->r, probe->g, probe->b);
	}
	else
	{
		out[0] = out[1] = out[2] = 0.f;
	}

	FDynamicLight* light;
	float frac, lr, lg, lb;
	float radius;

	float traceLights[3] = { 0.0f, 0.0f, 0.0f };
	bool hasTraceLight = false;

	ActorTraceStaticLight staticLight(self);

	if ((!subsector || calculateTraceLights) && ActorTraceStaticLight::TraceSunVisibility(x, y, z, traceCache, (self ? staticLight.ActorMoved : traceCache ? traceCache->Pos != DVector3(x, y, z) : false)))
	{
		if(!self && traceCache)
		{
			traceCache->Pos = DVector3(x, y, z);
		}

		if (!Level->SunColor.isZero())
		{
			hasTraceLight = true;
		}

		traceLights[0] += Level->SunColor.X * Level->SunIntensity;
		traceLights[1] += Level->SunColor.Y * Level->SunIntensity;
		traceLights[2] += Level->SunColor.Z * Level->SunIntensity;

		out[0] += Level->SunColor.X * Level->SunIntensity;
		out[1] += Level->SunColor.Y * Level->SunIntensity;
		out[2] += Level->SunColor.Z * Level->SunIntensity;
	}


	// Go through both light lists
	while (node)
	{
		light=node->lightsource;
		if (light->ShouldLightActor(self) && (!subsector || calculateTraceLights || !light->Trace()))
		{
			float dist;
			FVector3 L;

			// This is a performance critical section of code where we cannot afford to let the compiler decide whether to inline the function or not.
			// This will do the calculations explicitly rather than calling one of AActor's utility functions.
			if (Level->Displacements.size > 0)
			{
				int fromgroup = light->Sector->PortalGroup;
				int togroup = portalgroup;
				if (fromgroup == togroup || fromgroup == 0 || togroup == 0) goto direct;

				DVector2 offset = Level->Displacements.getOffset(fromgroup, togroup);
				L = FVector3(x - (float)(light->X() + offset.X), y - (float)(light->Y() + offset.Y), z - (float)light->Z());
			}
			else
			{
			direct:
				L = FVector3(x - (float)light->X(), y - (float)light->Y(), z - (float)light->Z());
			}

			dist = (float)L.LengthSquared();
			radius = light->GetRadius();

			if (radius > 0 && dist < radius * radius)
			{
				dist = sqrtf(dist);	// only calculate the square root if we really need it.

				if (light->IsSpot() || light->TraceActors())
					L *= -1.0f / dist;

				if (staticLight.TraceLightVisbility(node, L, dist, light->updated))
				{
					if(level.info->lightattenuationmode == ELightAttenuationMode::INVERSE_SQUARE)
					{
						frac = (inverseSquareAttenuation(std::max(dist, sqrt(radius) * 2), radius, light->GetStrength(), light->GetLinearity()));
					}
					else
					{
						frac = 1.0f - (dist / radius);
					}

					if (light->IsSpot())
					{
						DAngle negPitch = -*light->pPitch;
						DAngle Angle = light->target->Angles.Yaw;
						double xyLen = negPitch.Cos();
						double spotDirX = -Angle.Cos() * xyLen;
						double spotDirY = -Angle.Sin() * xyLen;
						double spotDirZ = -negPitch.Sin();
						double cosDir = L.X * spotDirX + L.Y * spotDirY + L.Z * spotDirZ;
						frac *= (float)smoothstep(light->pSpotOuterAngle->Cos(), light->pSpotInnerAngle->Cos(), cosDir);
					}

					if (frac > 0 && (!light->shadowmapped || (self && light->TraceActors()) || screen->mShadowMap->ShadowTest(light->Pos, { x, y, z })))
					{
						lr = light->GetRed() / 255.0f;
						lg = light->GetGreen() / 255.0f;
						lb = light->GetBlue() / 255.0f;

						if (light->target && (light->target->renderflags2 & RF2_LIGHTMULTALPHA))
						{
							float alpha = (float)light->target->Alpha;
							lr *= alpha;
							lg *= alpha;
							lb *= alpha;
						}

						// Get GLDEFS intensity
						lr *= light->GetLightDefIntensity();
						lg *= light->GetLightDefIntensity();
						lb *= light->GetLightDefIntensity();

						if (light->IsSubtractive())
						{
							float bright = (float)FVector3(lr, lg, lb).Length();
							FVector3 lightColor(lr, lg, lb);
							lr = (bright - lr) * -1;
							lg = (bright - lg) * -1;
							lb = (bright - lb) * -1;
						}

						if (light->Trace())
						{
							traceLights[0] += lr * frac;
							traceLights[1] += lg * frac;
							traceLights[2] += lb * frac;
							hasTraceLight = true;
						}

						out[0] += lr * frac;
						out[1] += lg * frac;
						out[2] += lb * frac;
					}
				}
			}
		}
		node = node->nextLight;
	}

	// Save to cache
	if (calculateTraceLights && hasTraceLight)
	{
		if (!subsector->traceLightCache)
		{
			FVector3 min, max;

			const auto& box = subsector->bbox;

			min.X = box.Left();
			min.Y = box.Bottom();
			min.Z = subsector->sector->floorplane.ZatPoint(min); // TODO better solution

			max.X = box.Right();
			max.Y = box.Top();
			max.Z = subsector->sector->ceilingplane.ZatPoint(max);

			if (min.Z > max.Z)
			{
				std::swap(min.Z, max.Z);
			}

			Level->TracelightVoxelCaches[subsector->Index()].reset(subsector->traceLightCache = new TraceLightVoxelCache(min, max, 32)); // allocate new cache
		
			if(developer >= 5)
				Printf("Allocated %lu tracelight probes\n", subsector->traceLightCache->Size());
		}
		
		if (!probe)
		{
			probe = &subsector->traceLightCache->Get(FVector3(float(x), float(y), float(z)));
		}
		
		probe->Set(traceLights[0], traceLights[1], traceLights[2]);

		if (developer >= 11)
		{
			Printf("Tracelight probe: %p | %p | %.2f %.2f %.2f | %d %d %d | %f %f %f\n", subsector->traceLightCache, probe, traceLights[0], traceLights[1], traceLights[2], probe->r, probe->g, probe->b, out[0], out[1], out[2]);
			probe->Get(traceLights[0], traceLights[1], traceLights[2]);
			Printf("Resulting value: %.2f %.2f %.2f\n", traceLights[0], traceLights[1], traceLights[2]);
		}
	}
}

ADD_STAT(tracelightcache)
{
	FString out;

	unsigned totalProbes = 0;
	unsigned subsectors = 0;

	for (const auto& cache : level.TracelightVoxelCaches)
	{
		if (cache)
		{
			++subsectors;
			totalProbes += cache->Size();
		}
	}

	out.Format(
		"Subsectors: %u\n"
		"Alllocated probes: %u\n"
		"Estimated memory usage: %.3fMB\n",
		subsectors,
		totalProbes,
		double(totalProbes * 4) / 1000.0 / 1000.0);

	return out;
}

void HWDrawInfo::GetDynSpriteLight(AActor *thing, particle_t *particle, sun_trace_cache_t * traceCache, float *out)
{
	if (thing)
	{
		GetDynSpriteLight(thing, &thing->StaticLightsTraceCache, thing->X(), thing->Y(), thing->Center(), thing->section->lighthead, thing->Sector->PortalGroup, out, (thing->flags5 & MF5_BRIGHT), thing->subsector);
	}
	else if (particle)
	{
		GetDynSpriteLight(nullptr, traceCache, particle->Pos.X, particle->Pos.Y, particle->Pos.Z, particle->subsector->section->lighthead, particle->subsector->sector->PortalGroup, out, (particle->flags & SPF_FULLBRIGHT), particle->subsector);
	}
}


void HWDrawInfo::GetDynSpriteLightList(AActor *self, double x, double y, double z, sun_trace_cache_t * traceCache, FDynLightData &modellightdata, bool isModel)
{
	modellightdata.Clear();

	if (self && (self->flags5 & MF5_BRIGHT))
		return;

	auto &addedLights = drawctx->addedLightsArray;

	addedLights.Clear();

	float actorradius = self ? (float)self->RenderRadius() : 1;
	float radiusSquared = actorradius * actorradius;
	dl_validcount++;

	ActorTraceStaticLight staticLight(self);

	int gl_spritelight = get_gl_spritelight();

	if(isModel && gl_fakemodellight)
	{
		//fake light for contrast
		AddSunLightToList(modellightdata, x, y, z, FVector3(Level->SunDirection.X + 180, 45, 0), Level->SunColor * Level->SunIntensity * gl_fakemodellightintensity, false);
	}

	if ((level.lightmaps && gl_spritelight > 0) || ActorTraceStaticLight::TraceSunVisibility(x, y, z, traceCache, (self ? staticLight.ActorMoved : traceCache ? traceCache->Pos != DVector3(x, y, z) : false)))
	{
		AddSunLightToList(modellightdata, x, y, z, Level->SunDirection, Level->SunColor * Level->SunIntensity, gl_spritelight > 0);
	}

	BSPWalkCircle(Level, x, y, radiusSquared, [&](subsector_t *subsector) // Iterate through all subsectors potentially touched by actor
	{
		auto section = subsector->section;
		if (section->validcount == dl_validcount) return;	// already done from a previous subsector.
		FLightNode * node = section->lighthead;
		while (node) // check all lights touching a subsector
		{
			FDynamicLight *light = node->lightsource;
			if (light->ShouldLightActor(self))
			{
				int group = subsector->sector->PortalGroup;
				DVector3 pos = light->PosRelative(group);
				float radius = (float)(light->GetRadius() + actorradius);
				double dx = pos.X - x;
				double dy = pos.Y - y;
				double dz = pos.Z - z;
				double distSquared = dx * dx + dy * dy + dz * dz;
				if (distSquared < radius * radius) // Light and actor touches
				{
					unsigned index = addedLights.SortedFind(light, false);
					if(index == addedLights.Size() || addedLights[index] != light) // Check if we already added this light from a different subsector (use binary search instead of linear search)
					{
						FVector3 L(dx, dy, dz);
						float dist = sqrtf(distSquared);
						if (gl_spritelight == 0 && light->TraceActors())
							L *= 1.0f / dist;

						if (gl_spritelight > 0 || staticLight.TraceLightVisbility(node, L, dist, light->updated))
						{
							AddLightToList(modellightdata, group, light, true, gl_spritelight > 0);
						}

						addedLights.Insert(index, light);
					}
				}
			}
			node = node->nextLight;
		}
	});
}

void HWDrawInfo::GetDynSpriteLightList(AActor *thing, particle_t *particle, sun_trace_cache_t * traceCache, FDynLightData &modellightdata, bool isModel)
{
	if (thing)
	{
		GetDynSpriteLightList(thing, thing->X(), thing->Y(), thing->Center(), &thing->StaticLightsTraceCache, modellightdata, isModel);
	}
	else if (particle)
	{
		if(particle->flags & SPF_FULLBRIGHT) return;
		GetDynSpriteLightList(nullptr, particle->Pos.X, particle->Pos.Y, particle->Pos.Z, traceCache, modellightdata, isModel);
	}
}