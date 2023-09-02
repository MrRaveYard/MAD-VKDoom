
#include "templates.h"
#include "doom_levelmesh.h"
#include "g_levellocals.h"
#include "texturemanager.h"
#include "playsim/p_lnspec.h"


#include "c_dispatch.h"
#include "g_levellocals.h"

#include "common/rendering/vulkan/accelstructs/vk_lightmap.h"
#include <vulkan/accelstructs/halffloat.h>

CCMD(dumplevelmesh)
{
	if (level.levelMesh)
	{
		level.levelMesh->DumpMesh(FString("levelmesh.obj"));
		Printf("Level mesh exported.");
	}
	else
	{
		Printf("No level mesh. Perhaps your level has no lightmap loaded?");
	}
}

DoomLevelMesh::DoomLevelMesh(FLevelLocals &doomMap)
{
	SunColor = doomMap.SunColor; // TODO keep only one copy?
	SunDirection = doomMap.SunDirection;

	for (unsigned int i = 0; i < doomMap.sides.Size(); i++)
	{
		CreateSideSurfaces(doomMap, &doomMap.sides[i]);
	}

	CreateSubsectorSurfaces(doomMap);

	for (size_t i = 0; i < Surfaces.Size(); i++)
	{
		const auto &s = Surfaces[i];
		int numVerts = s.numVerts;
		unsigned int pos = s.startVertIndex;
		FVector3* verts = &MeshVertices[pos];

		for (int j = 0; j < numVerts; j++)
		{
			MeshUVIndex.Push(j);
		}

		if (s.Type == ST_FLOOR || s.Type == ST_CEILING)
		{
			for (int j = 2; j < numVerts; j++)
			{
				if (!IsDegenerate(verts[0], verts[j - 1], verts[j]))
				{
					MeshElements.Push(pos);
					MeshElements.Push(pos + j - 1);
					MeshElements.Push(pos + j);
					MeshSurfaceIndexes.Push((int)i);
				}
			}
		}
		else if (s.Type == ST_MIDDLESIDE || s.Type == ST_UPPERSIDE || s.Type == ST_LOWERSIDE)
		{
			if (!IsDegenerate(verts[0], verts[1], verts[2]))
			{
				MeshElements.Push(pos + 0);
				MeshElements.Push(pos + 1);
				MeshElements.Push(pos + 2);
				MeshSurfaceIndexes.Push((int)i);
			}
			if (!IsDegenerate(verts[1], verts[2], verts[3]))
			{
				MeshElements.Push(pos + 3);
				MeshElements.Push(pos + 2);
				MeshElements.Push(pos + 1);
				MeshSurfaceIndexes.Push((int)i);
			}
		}
	}

	SetupLightmapUvs();
	BindLightmapSurfacesToGeometry(doomMap);

	Collision = std::make_unique<TriangleMeshShape>(MeshVertices.Data(), MeshVertices.Size(), MeshElements.Data(), MeshElements.Size());
}

void DoomLevelMesh::BindLightmapSurfacesToGeometry(FLevelLocals& doomMap)
{
	// Allocate room for all surfaces

	unsigned int allSurfaces = 0;

	for (unsigned int i = 0; i < doomMap.sides.Size(); i++)
		allSurfaces += 4 + doomMap.sides[i].sector->e->XFloor.ffloors.Size();

	for (unsigned int i = 0; i < doomMap.subsectors.Size(); i++)
		allSurfaces += 2 + doomMap.subsectors[i].sector->e->XFloor.ffloors.Size() * 2;

	doomMap.LMSurfaces.Resize(allSurfaces);
	memset(&doomMap.LMSurfaces[0], 0, sizeof(DoomLevelMeshSurface*) * allSurfaces);

	// Link the surfaces to sectors, sides and their 3D floors

	unsigned int offset = 0;
	for (unsigned int i = 0; i < doomMap.sides.Size(); i++)
	{
		auto& side = doomMap.sides[i];
		side.lightmap = &doomMap.LMSurfaces[offset];
		offset += 4 + side.sector->e->XFloor.ffloors.Size();
	}
	for (unsigned int i = 0; i < doomMap.subsectors.Size(); i++)
	{
		auto& subsector = doomMap.subsectors[i];
		unsigned int count = 1 + subsector.sector->e->XFloor.ffloors.Size();
		subsector.lightmap[0] = &doomMap.LMSurfaces[offset];
		subsector.lightmap[1] = &doomMap.LMSurfaces[offset + count];
		offset += count * 2;
	}

	// Copy and build properties
	size_t index = 0;
	for (auto& surface : Surfaces)
	{
		surface.TexCoords = (float*)&LightmapUvs[surface.startUvIndex];

		surface.LightmapNum = surface.atlasPageIndex;

		if (surface.Type == ST_FLOOR || surface.Type == ST_CEILING)
		{
			surface.Subsector = &doomMap.subsectors[surface.typeIndex];
			if (surface.Subsector->firstline && surface.Subsector->firstline->sidedef)
				surface.Subsector->firstline->sidedef->sector->HasLightmaps = true;
			SetSubsectorLightmap(&surface);
		}
		else
		{
			surface.Side = &doomMap.sides[surface.typeIndex];
			SetSideLightmap(&surface);
		}
	}
}

void DoomLevelMesh::SetSubsectorLightmap(DoomLevelMeshSurface* surface)
{
	if (!surface->ControlSector)
	{
		int index = surface->Type == ST_CEILING ? 1 : 0;
		surface->Subsector->lightmap[index][0] = surface;
	}
	else
	{
		int index = surface->Type == ST_CEILING ? 0 : 1;
		const auto& ffloors = surface->Subsector->sector->e->XFloor.ffloors;
		for (unsigned int i = 0; i < ffloors.Size(); i++)
		{
			if (ffloors[i]->model == surface->ControlSector)
			{
				surface->Subsector->lightmap[index][i + 1] = surface;
			}
		}
	}
}

void DoomLevelMesh::SetSideLightmap(DoomLevelMeshSurface* surface)
{
	if (!surface->ControlSector)
	{
		if (surface->Type == ST_UPPERSIDE)
		{
			surface->Side->lightmap[0] = surface;
		}
		else if (surface->Type == ST_MIDDLESIDE)
		{
			surface->Side->lightmap[1] = surface;
			surface->Side->lightmap[2] = surface;
		}
		else if (surface->Type == ST_LOWERSIDE)
		{
			surface->Side->lightmap[3] = surface;
		}
	}
	else
	{
		const auto& ffloors = surface->Side->sector->e->XFloor.ffloors;
		for (unsigned int i = 0; i < ffloors.Size(); i++)
		{
			if (ffloors[i]->model == surface->ControlSector)
			{
				surface->Side->lightmap[4 + i] = surface;
			}
		}
	}
}

void DoomLevelMesh::CreateSideSurfaces(FLevelLocals &doomMap, side_t *side)
{
	sector_t *front;
	sector_t *back;

	front = side->sector;
	back = (side->linedef->frontsector == front) ? side->linedef->backsector : side->linedef->frontsector;

	if (IsControlSector(front))
		return;

	FVector2 v1 = ToFVector2(side->V1()->fPos());
	FVector2 v2 = ToFVector2(side->V2()->fPos());

	float v1Top = (float)front->ceilingplane.ZatPoint(v1);
	float v1Bottom = (float)front->floorplane.ZatPoint(v1);
	float v2Top = (float)front->ceilingplane.ZatPoint(v2);
	float v2Bottom = (float)front->floorplane.ZatPoint(v2);

	int typeIndex = side->Index();

	FVector2 dx(v2.X - v1.X, v2.Y - v1.Y);
	float distance = dx.Length();

	// line_horizont consumes everything
	if (side->linedef->special == Line_Horizon && front != back)
	{
		DoomLevelMeshSurface surf;
		surf.Type = ST_MIDDLESIDE;
		surf.typeIndex = typeIndex;
		surf.bSky = front->GetTexture(sector_t::floor) == skyflatnum || front->GetTexture(sector_t::ceiling) == skyflatnum;

		FVector3 verts[4];
		verts[0].X = verts[2].X = v1.X;
		verts[0].Y = verts[2].Y = v1.Y;
		verts[1].X = verts[3].X = v2.X;
		verts[1].Y = verts[3].Y = v2.Y;
		verts[0].Z = v1Bottom;
		verts[1].Z = v2Bottom;
		verts[2].Z = v1Top;
		verts[3].Z = v2Top;

		surf.startVertIndex = MeshVertices.Size();
		surf.numVerts = 4;
		MeshVertices.Push(verts[0]);
		MeshVertices.Push(verts[1]);
		MeshVertices.Push(verts[2]);
		MeshVertices.Push(verts[3]);

		surf.plane = ToPlane(verts[0], verts[1], verts[2]);
		Surfaces.Push(surf);
		return;
	}

	if (back)
	{
		for (unsigned int j = 0; j < front->e->XFloor.ffloors.Size(); j++)
		{
			F3DFloor *xfloor = front->e->XFloor.ffloors[j];

			// Don't create a line when both sectors have the same 3d floor
			bool bothSides = false;
			for (unsigned int k = 0; k < back->e->XFloor.ffloors.Size(); k++)
			{
				if (back->e->XFloor.ffloors[k] == xfloor)
				{
					bothSides = true;
					break;
				}
			}
			if (bothSides)
				continue;

			DoomLevelMeshSurface surf;
			surf.Type = ST_MIDDLESIDE;
			surf.typeIndex = typeIndex;
			surf.ControlSector = xfloor->model;
			surf.bSky = false;

			FVector3 verts[4];
			verts[0].X = verts[2].X = v2.X;
			verts[0].Y = verts[2].Y = v2.Y;
			verts[1].X = verts[3].X = v1.X;
			verts[1].Y = verts[3].Y = v1.Y;
			verts[0].Z = (float)xfloor->model->floorplane.ZatPoint(v2);
			verts[1].Z = (float)xfloor->model->floorplane.ZatPoint(v1);
			verts[2].Z = (float)xfloor->model->ceilingplane.ZatPoint(v2);
			verts[3].Z = (float)xfloor->model->ceilingplane.ZatPoint(v1);

			surf.startVertIndex = MeshVertices.Size();
			surf.numVerts = 4;
			MeshVertices.Push(verts[0]);
			MeshVertices.Push(verts[1]);
			MeshVertices.Push(verts[2]);
			MeshVertices.Push(verts[3]);

			surf.plane = ToPlane(verts[0], verts[1], verts[2]);
			Surfaces.Push(surf);
		}

		float v1TopBack = (float)back->ceilingplane.ZatPoint(v1);
		float v1BottomBack = (float)back->floorplane.ZatPoint(v1);
		float v2TopBack = (float)back->ceilingplane.ZatPoint(v2);
		float v2BottomBack = (float)back->floorplane.ZatPoint(v2);

		if (v1Top == v1TopBack && v1Bottom == v1BottomBack && v2Top == v2TopBack && v2Bottom == v2BottomBack)
		{
			return;
		}

		// bottom seg
		if (v1Bottom < v1BottomBack || v2Bottom < v2BottomBack)
		{
			if (IsBottomSideVisible(side))
			{
				DoomLevelMeshSurface surf;

				FVector3 verts[4];
				verts[0].X = verts[2].X = v1.X;
				verts[0].Y = verts[2].Y = v1.Y;
				verts[1].X = verts[3].X = v2.X;
				verts[1].Y = verts[3].Y = v2.Y;
				verts[0].Z = v1Bottom;
				verts[1].Z = v2Bottom;
				verts[2].Z = v1BottomBack;
				verts[3].Z = v2BottomBack;

				surf.startVertIndex = MeshVertices.Size();
				surf.numVerts = 4;
				MeshVertices.Push(verts[0]);
				MeshVertices.Push(verts[1]);
				MeshVertices.Push(verts[2]);
				MeshVertices.Push(verts[3]);

				surf.plane = ToPlane(verts[0], verts[1], verts[2]);
				surf.Type = ST_LOWERSIDE;
				surf.typeIndex = typeIndex;
				surf.bSky = false;
				surf.ControlSector = nullptr;

				Surfaces.Push(surf);
			}

			v1Bottom = v1BottomBack;
			v2Bottom = v2BottomBack;
		}

		// top seg
		if (v1Top > v1TopBack || v2Top > v2TopBack)
		{
			bool bSky = IsTopSideSky(front, back, side);
			if (bSky || IsTopSideVisible(side))
			{
				DoomLevelMeshSurface surf;

				FVector3 verts[4];
				verts[0].X = verts[2].X = v1.X;
				verts[0].Y = verts[2].Y = v1.Y;
				verts[1].X = verts[3].X = v2.X;
				verts[1].Y = verts[3].Y = v2.Y;
				verts[0].Z = v1TopBack;
				verts[1].Z = v2TopBack;
				verts[2].Z = v1Top;
				verts[3].Z = v2Top;

				surf.startVertIndex = MeshVertices.Size();
				surf.numVerts = 4;
				MeshVertices.Push(verts[0]);
				MeshVertices.Push(verts[1]);
				MeshVertices.Push(verts[2]);
				MeshVertices.Push(verts[3]);

				surf.plane = ToPlane(verts[0], verts[1], verts[2]);
				surf.Type = ST_UPPERSIDE;
				surf.typeIndex = typeIndex;
				surf.bSky = bSky;
				surf.ControlSector = nullptr;

				Surfaces.Push(surf);
			}

			v1Top = v1TopBack;
			v2Top = v2TopBack;
		}
	}

	// middle seg
	if (back == nullptr)
	{
		DoomLevelMeshSurface surf;
		surf.bSky = false;

		FVector3 verts[4];
		verts[0].X = verts[2].X = v1.X;
		verts[0].Y = verts[2].Y = v1.Y;
		verts[1].X = verts[3].X = v2.X;
		verts[1].Y = verts[3].Y = v2.Y;
		verts[0].Z = v1Bottom;
		verts[1].Z = v2Bottom;
		verts[2].Z = v1Top;
		verts[3].Z = v2Top;

		surf.startVertIndex = MeshVertices.Size();
		surf.numVerts = 4;
		surf.bSky = false;
		MeshVertices.Push(verts[0]);
		MeshVertices.Push(verts[1]);
		MeshVertices.Push(verts[2]);
		MeshVertices.Push(verts[3]);

		surf.plane = ToPlane(verts[0], verts[1], verts[2]);
		surf.Type = ST_MIDDLESIDE;
		surf.typeIndex = typeIndex;
		surf.ControlSector = nullptr;

		Surfaces.Push(surf);
	}
}

void DoomLevelMesh::CreateFloorSurface(FLevelLocals &doomMap, subsector_t *sub, sector_t *sector, int typeIndex, bool is3DFloor)
{
	DoomLevelMeshSurface surf;
	surf.bSky = IsSkySector(sector, sector_t::floor);

	secplane_t plane;
	if (!is3DFloor)
	{
		plane = sector->floorplane;
	}
	else
	{
		plane = sector->ceilingplane;
		plane.FlipVert();
	}

	surf.numVerts = sub->numlines;
	surf.startVertIndex = MeshVertices.Size();
	MeshVertices.Resize(surf.startVertIndex + surf.numVerts);
	FVector3* verts = &MeshVertices[surf.startVertIndex];

	for (int j = 0; j < surf.numVerts; j++)
	{
		seg_t *seg = &sub->firstline[(surf.numVerts - 1) - j];
		FVector2 v1 = ToFVector2(seg->v1->fPos());

		verts[j].X = v1.X;
		verts[j].Y = v1.Y;
		verts[j].Z = (float)plane.ZatPoint(verts[j]);
	}

	surf.Type = ST_FLOOR;
	surf.typeIndex = typeIndex;
	surf.ControlSector = is3DFloor ? sector : nullptr;
	surf.plane = FVector4((float)plane.Normal().X, (float)plane.Normal().Y, (float)plane.Normal().Z, (float)plane.D);

	Surfaces.Push(surf);
}

void DoomLevelMesh::CreateCeilingSurface(FLevelLocals &doomMap, subsector_t *sub, sector_t *sector, int typeIndex, bool is3DFloor)
{
	DoomLevelMeshSurface surf;
	surf.bSky = IsSkySector(sector, sector_t::ceiling);

	secplane_t plane;
	if (!is3DFloor)
	{
		plane = sector->ceilingplane;
	}
	else
	{
		plane = sector->floorplane;
		plane.FlipVert();
	}

	surf.numVerts = sub->numlines;
	surf.startVertIndex = MeshVertices.Size();
	MeshVertices.Resize(surf.startVertIndex + surf.numVerts);
	FVector3* verts = &MeshVertices[surf.startVertIndex];

	for (int j = 0; j < surf.numVerts; j++)
	{
		seg_t *seg = &sub->firstline[j];
		FVector2 v1 = ToFVector2(seg->v1->fPos());

		verts[j].X = v1.X;
		verts[j].Y = v1.Y;
		verts[j].Z = (float)plane.ZatPoint(verts[j]);
	}

	surf.Type = ST_CEILING;
	surf.typeIndex = typeIndex;
	surf.ControlSector = is3DFloor ? sector : nullptr;
	surf.plane = FVector4((float)plane.Normal().X, (float)plane.Normal().Y, (float)plane.Normal().Z, (float)plane.D);

	Surfaces.Push(surf);
}

void DoomLevelMesh::CreateSubsectorSurfaces(FLevelLocals &doomMap)
{
	for (unsigned int i = 0; i < doomMap.subsectors.Size(); i++)
	{
		subsector_t *sub = &doomMap.subsectors[i];

		if (sub->numlines < 3)
		{
			continue;
		}

		sector_t *sector = sub->sector;
		if (!sector || IsControlSector(sector))
			continue;

		CreateFloorSurface(doomMap, sub, sector, i, false);
		CreateCeilingSurface(doomMap, sub, sector, i, false);

		for (unsigned int j = 0; j < sector->e->XFloor.ffloors.Size(); j++)
		{
			CreateFloorSurface(doomMap, sub, sector->e->XFloor.ffloors[j]->model, i, true);
			CreateCeilingSurface(doomMap, sub, sector->e->XFloor.ffloors[j]->model, i, true);
		}
	}
}

bool DoomLevelMesh::IsTopSideSky(sector_t* frontsector, sector_t* backsector, side_t* side)
{
	return IsSkySector(frontsector, sector_t::ceiling) && IsSkySector(backsector, sector_t::ceiling);
}

bool DoomLevelMesh::IsTopSideVisible(side_t* side)
{
	auto tex = TexMan.GetGameTexture(side->GetTexture(side_t::top), true);
	return tex && tex->isValid();
}

bool DoomLevelMesh::IsBottomSideVisible(side_t* side)
{
	auto tex = TexMan.GetGameTexture(side->GetTexture(side_t::bottom), true);
	return tex && tex->isValid();
}

bool DoomLevelMesh::IsSkySector(sector_t* sector, int plane)
{
	// plane is either sector_t::ceiling or sector_t::floor
	return sector->GetTexture(plane) == skyflatnum;
}

bool DoomLevelMesh::IsControlSector(sector_t* sector)
{
	//return sector->controlsector;
	return false;
}

bool DoomLevelMesh::IsDegenerate(const FVector3 &v0, const FVector3 &v1, const FVector3 &v2)
{
	// A degenerate triangle has a zero cross product for two of its sides.
	float ax = v1.X - v0.X;
	float ay = v1.Y - v0.Y;
	float az = v1.Z - v0.Z;
	float bx = v2.X - v0.X;
	float by = v2.Y - v0.Y;
	float bz = v2.Z - v0.Z;
	float crossx = ay * bz - az * by;
	float crossy = az * bx - ax * bz;
	float crossz = ax * by - ay * bx;
	float crosslengthsqr = crossx * crossx + crossy * crossy + crossz * crossz;
	return crosslengthsqr <= 1.e-6f;
}

void DoomLevelMesh::DumpMesh(const FString& filename) const
{
	auto f = fopen(filename.GetChars(), "w");

	fprintf(f, "# DoomLevelMesh debug export\n");
	fprintf(f, "# MeshVertices: %d, MeshElements: %d\n", MeshVertices.Size(), MeshElements.Size());

	double scale = 1 / 10.0;

	for (const auto& v : MeshVertices)
	{
		fprintf(f, "v %f %f %f\n", v.X * scale, v.Y * scale, v.Z * scale);
	}

	{
		for (const auto& uv : LightmapUvs)
		{
			fprintf(f, "vt %f %f\n", uv.X, uv.Y);
		}
	}

	const auto s = MeshElements.Size();
	for (unsigned i = 0; i + 2 < s; i += 3)
	{
		// fprintf(f, "f %d %d %d\n", MeshElements[i] + 1, MeshElements[i + 1] + 1, MeshElements[i + 2] + 1);
		fprintf(f, "f %d/%d %d/%d %d/%d\n",
			MeshElements[i + 0] + 1, MeshElements[i + 0] + 1,
			MeshElements[i + 1] + 1, MeshElements[i + 1] + 1,
			MeshElements[i + 2] + 1, MeshElements[i + 2] + 1);

	}

	fclose(f);
}

void DoomLevelMesh::SetupLightmapUvs()
{
	LMTextureSize = 1024; // TODO cvar

	std::vector<LevelMeshSurface*> sortedSurfaces;
	sortedSurfaces.reserve(Surfaces.Size());

	for (auto& surface : Surfaces)
	{
		BuildSurfaceParams(LMTextureSize, LMTextureSize, surface);
		sortedSurfaces.push_back(&surface);
	}

	// VkLightmapper old ZDRay properties
	for (auto& surface : Surfaces)
	{
		for (int i = 0; i < surface.numVerts; ++i)
		{
			surface.verts.Push(MeshVertices[surface.startVertIndex + i]);
		}

		for (int i = 0; i < surface.numVerts; ++i)
		{
			surface.uvs.Push(LightmapUvs[surface.startUvIndex + i]);
		}

		surface.texPixels.resize(surface.texWidth * surface.texHeight);
	}

	BuildSmoothingGroups();

	std::sort(sortedSurfaces.begin(), sortedSurfaces.end(), [](LevelMeshSurface* a, LevelMeshSurface* b) { return a->texHeight != b->texHeight ? a->texHeight > b->texHeight : a->texWidth > b->texWidth; });

	RectPacker packer(LMTextureSize, LMTextureSize, RectPacker::Spacing(0));

	for (LevelMeshSurface* surf : sortedSurfaces)
	{
		FinishSurface(LMTextureSize, LMTextureSize, packer, *surf);
	}

	// You have no idea how long this took me to figure out...

	// Reorder vertices into renderer format
	for (LevelMeshSurface& surface : Surfaces)
	{
		if (surface.Type == ST_FLOOR)
		{
			// reverse vertices on floor
			for (int j = surface.startUvIndex + surface.numVerts - 1, k = surface.startUvIndex; j > k; j--, k++)
			{
				std::swap(LightmapUvs[k], LightmapUvs[j]);
			}
		}
		else if (surface.Type != ST_CEILING) // walls
		{
			// from 0 1 2 3
			// to   0 2 1 3
			std::swap(LightmapUvs[surface.startUvIndex + 1], LightmapUvs[surface.startUvIndex + 2]);
			std::swap(LightmapUvs[surface.startUvIndex + 2], LightmapUvs[surface.startUvIndex + 3]);
		}
	}

	LMTextureCount = (int)packer.getNumPages();
}

void DoomLevelMesh::FinishSurface(int lightmapTextureWidth, int lightmapTextureHeight, RectPacker& packer, LevelMeshSurface& surface)
{
	int sampleWidth = surface.texWidth;
	int sampleHeight = surface.texHeight;

	auto result = packer.insert(sampleWidth, sampleHeight);
	int x = result.pos.x, y = result.pos.y;
	surface.atlasPageIndex = (int)result.pageIndex;

	// calculate final texture coordinates
	for (int i = 0; i < (int)surface.numVerts; i++)
	{
		auto& u = LightmapUvs[surface.startUvIndex + i].X;
		auto& v = LightmapUvs[surface.startUvIndex + i].Y;
		u = (u + x) / (float)lightmapTextureWidth;
		v = (v + y) / (float)lightmapTextureHeight;
	}
	
	surface.atlasX = x;
	surface.atlasY = y;

#if 0
	while (result.pageIndex >= textures.size())
	{
		textures.push_back(std::make_unique<LightmapTexture>(textureWidth, textureHeight));
	}

	uint16_t* currentTexture = textures[surface->atlasPageIndex]->Pixels();

	FVector3* colorSamples = surface->texPixels.data();
	// store results to lightmap texture
	for (int i = 0; i < sampleHeight; i++)
	{
		for (int j = 0; j < sampleWidth; j++)
		{
			// get texture offset
			int offs = ((textureWidth * (i + surface->atlasY)) + surface->atlasX) * 3;

			// convert RGB to bytes
			currentTexture[offs + j * 3 + 0] = floatToHalf(clamp(colorSamples[i * sampleWidth + j].x, 0.0f, 65000.0f));
			currentTexture[offs + j * 3 + 1] = floatToHalf(clamp(colorSamples[i * sampleWidth + j].y, 0.0f, 65000.0f));
			currentTexture[offs + j * 3 + 2] = floatToHalf(clamp(colorSamples[i * sampleWidth + j].z, 0.0f, 65000.0f));
		}
	}
#endif
}

BBox DoomLevelMesh::GetBoundsFromSurface(const LevelMeshSurface& surface) const
{
	constexpr float M_INFINITY = 1e30f; // TODO cleanup

	FVector3 low(M_INFINITY, M_INFINITY, M_INFINITY);
	FVector3 hi(-M_INFINITY, -M_INFINITY, -M_INFINITY);

	for (int i = int(surface.startVertIndex); i < int(surface.startVertIndex) + surface.numVerts; i++)
	{
		for (int j = 0; j < 3; j++)
		{
			if (MeshVertices[i][j] < low[j])
			{
				low[j] = MeshVertices[i][j];
			}
			if (MeshVertices[i][j] > hi[j])
			{
				hi[j] = MeshVertices[i][j];
			}
		}
	}

	BBox bounds;
	bounds.Clear();
	bounds.min = low;
	bounds.max = hi;
	return bounds;
}

DoomLevelMesh::PlaneAxis DoomLevelMesh::BestAxis(const FVector4& p)
{
	float na = fabs(float(p.X));
	float nb = fabs(float(p.Y));
	float nc = fabs(float(p.Z));

	// figure out what axis the plane lies on
	if (na >= nb && na >= nc)
	{
		return AXIS_YZ;
	}
	else if (nb >= na && nb >= nc)
	{
		return AXIS_XZ;
	}

	return AXIS_XY;
}

void DoomLevelMesh::BuildSurfaceParams(int lightMapTextureWidth, int lightMapTextureHeight, LevelMeshSurface& surface)
{
	BBox bounds;
	FVector3 roundedSize;
	FVector3 tOrigin;
	int width;
	int height;
	float d;

	const FVector4& plane = surface.plane;
	bounds = GetBoundsFromSurface(surface);
	surface.bounds = bounds;

	if (surface.sampleDimension <= 0)
	{
		surface.sampleDimension = 16;
	}
	//surface->sampleDimension = Math::RoundPowerOfTwo(surface->sampleDimension);

	// round off dimensions
	for (int i = 0; i < 3; i++)
	{
		bounds.min[i] = surface.sampleDimension * (floor(bounds.min[i] / surface.sampleDimension) - 1);
		bounds.max[i] = surface.sampleDimension * (ceil(bounds.max[i] / surface.sampleDimension) + 1);

		roundedSize[i] = (bounds.max[i] - bounds.min[i]) / surface.sampleDimension;
	}

	FVector3 tCoords[2] = { FVector3(0.0f, 0.0f, 0.0f), FVector3(0.0f, 0.0f, 0.0f) };

	PlaneAxis axis = BestAxis(plane);

	switch (axis)
	{
	case AXIS_YZ:
		width = (int)roundedSize.Y;
		height = (int)roundedSize.Z;
		tCoords[0].Y = 1.0f / surface.sampleDimension;
		tCoords[1].Z = 1.0f / surface.sampleDimension;
		break;

	case AXIS_XZ:
		width = (int)roundedSize.X;
		height = (int)roundedSize.Z;
		tCoords[0].X = 1.0f / surface.sampleDimension;
		tCoords[1].Z = 1.0f / surface.sampleDimension;
		break;

	case AXIS_XY:
		width = (int)roundedSize.X;
		height = (int)roundedSize.Y;
		tCoords[0].X = 1.0f / surface.sampleDimension;
		tCoords[1].Y = 1.0f / surface.sampleDimension;
		break;
	}

	// clamp width
	if (width > lightMapTextureWidth - 2)
	{
		tCoords[0] *= ((float)(lightMapTextureWidth - 2) / (float)width);
		width = (lightMapTextureWidth - 2);
	}

	// clamp height
	if (height > lightMapTextureHeight - 2)
	{
		tCoords[1] *= ((float)(lightMapTextureHeight - 2) / (float)height);
		height = (lightMapTextureHeight - 2);
	}

	surface.translateWorldToLocal = bounds.min;
	surface.projLocalToU = tCoords[0];
	surface.projLocalToV = tCoords[1];

	surface.startUvIndex = AllocUvs(surface.numVerts);

	for (int i = 0; i < surface.numVerts; i++)
	{
		FVector3 tDelta = MeshVertices[surface.startVertIndex + i] - surface.translateWorldToLocal;

		LightmapUvs[surface.startUvIndex + i].X = (tDelta | surface.projLocalToU);
		LightmapUvs[surface.startUvIndex + i].Y = (tDelta | surface.projLocalToV);
	}


	tOrigin = bounds.min;

	// project tOrigin and tCoords so they lie on the plane
	d = ((bounds.min | FVector3(plane.X, plane.Y, plane.Z)) - plane.W) / plane[axis]; //d = (plane->PointToDist(bounds.min)) / plane->Normal()[axis];
	tOrigin[axis] -= d;

	for (int i = 0; i < 2; i++)
	{
		tCoords[i].MakeUnit();
		d = (tCoords[i] | FVector3(plane.X, plane.Y, plane.Z)) / plane[axis]; //d = dot(tCoords[i], plane->Normal()) / plane->Normal()[axis];
		tCoords[i][axis] -= d;
	}

	surface.texWidth = width;
	surface.texHeight = height;
	surface.worldOrigin = tOrigin;
	surface.worldStepX = tCoords[0] * (float)surface.sampleDimension;
	surface.worldStepY = tCoords[1] * (float)surface.sampleDimension;
}
