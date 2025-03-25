// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2005-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
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

#ifndef __GLC_DYNLIGHT_H
#define __GLC_DYNLIGHT_H

#include "tarray.h"
#include "vectors.h"

enum FDynLightInfoFlags
{
	LIGHTINFO_ATTENUATED = 1,
	LIGHTINFO_SHADOWMAPPED = 2,
	LIGHTINFO_SPOT = 4,
	LIGHTINFO_TRACE = 8,
	LIGHTINFO_SUN = 16,
};

struct FDynLightInfo
{
	float x;
	float y;
	float z;
	float padding0; // 4
	float r;
	float g;
	float b;
	float padding1; // 8
	float spotDirX;
	float spotDirY;
	float spotDirZ;
	float padding2; // 12
	float radius;
	float linearity;
	float softShadowRadius;
	float strength; // 16
	float spotInnerAngle;
	float spotOuterAngle;
	int shadowIndex;
	int flags; // 20
};

enum FDynLightDataArrays
{
	LIGHTARRAY_NORMAL,
	LIGHTARRAY_SUBTRACTIVE,
	LIGHTARRAY_ADDITIVE,
};

#define MAX_LIGHT_DATA 8192

struct FDynLightData
{
	TArray<FDynLightInfo> arrays[3];

	void Clear()
	{
		arrays[LIGHTARRAY_NORMAL].Clear();
		arrays[LIGHTARRAY_SUBTRACTIVE].Clear();
		arrays[LIGHTARRAY_ADDITIVE].Clear();
	}

};

struct sun_trace_cache_t
{
	DVector3 Pos = DVector3(-12345678.0, -12345678.0, -12345678.0);
	bool SunResult = false;
};

#define TRACELIGHT_CACHE_MAX_VALUE_R ((1 << 10) - 1)
#define TRACELIGHT_CACHE_MAX_VALUE_G ((1 << 10) - 1)
#define TRACELIGHT_CACHE_MAX_VALUE_B ((1 << 10) - 1)

class TraceLightVoxelCache
{
public:
	struct Probe
	{
		uint32_t r : 10;
		uint32_t g : 10;
		uint32_t b : 10;
		uint32_t isValid : 1;

		inline Probe() : r(0), g(0), b(0), isValid(0) {}

		inline void Set(float r, float g, float b)
		{
			isValid = true;
			this->r = std::clamp(uint32_t(r * 256.0f), uint32_t(0), uint32_t(TRACELIGHT_CACHE_MAX_VALUE_R));
			this->g = std::clamp(uint32_t(g * 256.0f), uint32_t(0), uint32_t(TRACELIGHT_CACHE_MAX_VALUE_G));
			this->b = std::clamp(uint32_t(b * 256.0f), uint32_t(0), uint32_t(TRACELIGHT_CACHE_MAX_VALUE_B));
		}

		inline void Get(float& r, float& g, float& b)
		{
			r = float(this->r) / 256.0f;
			g = float(this->g) / 256.0f;
			b = float(this->b) / 256.0f;
		}
	};

	static_assert(sizeof(Probe) == sizeof(uint32_t));

private:
	FVector3 min;
	float gridSize;
	FVector3 max;
	uint32_t indexMulY, indexMulZ, probeCount;
	std::unique_ptr<Probe[]> entries;

public:
	inline TraceLightVoxelCache(const FVector3& min, const FVector3& max, unsigned gridSize)
		: min(min), gridSize(gridSize), max(max)
	{
		uint64_t sizeX, sizeY, sizeZ, count;

		while (true)
		{
			sizeX = std::max(uint64_t(1), uint64_t(ceilf((max.X - min.X) / gridSize)));
			sizeY = std::max(uint64_t(1), uint64_t(ceilf((max.Y - min.Y) / gridSize)));
			sizeZ = std::max(uint64_t(1), uint64_t(ceilf((max.Z - min.Z) / gridSize)));

			count = sizeX * sizeY * sizeZ;

			if (count > 8192 * 4) // sanity limit
			{
				this->gridSize *= 2;
				gridSize *= 2;
				continue;
			}
			break;
		}

		indexMulY = sizeX;
		indexMulZ = sizeX * sizeY;
		entries.reset(new Probe[probeCount = std::max(1u, uint32_t(count))]());
	}

	inline size_t Size() const
	{
		return probeCount;
	}

	inline Probe& Get(const FVector3& pos)
	{
		FVector3 off = pos;

		off.X = std::clamp(off.X, min.X, max.X) - min.X;
		off.Y = std::clamp(off.Y, min.Y, max.Y) - min.Y;
		off.Z = std::clamp(off.Z, min.Z, max.Z) - min.Z;

		uint32_t x = static_cast<uint32_t>(off.X / gridSize);
		uint32_t y = static_cast<uint32_t>(off.Y / gridSize);
		uint32_t z = static_cast<uint32_t>(off.Z / gridSize);

		const auto index = (z * indexMulZ) + (y * indexMulY) + x; // effective order is: [Z][Y][X];

		return entries[index < probeCount ? index : 0u];
	}
};

extern thread_local FDynLightData lightdata;


#endif
