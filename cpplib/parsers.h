#pragma once
#include "graphics.h"
#include "file_system.h"
#include <stdint.h>

struct StackAllocator;

struct MeshData
{
	float *vertices;
	uint16_t *indices;
	uint32_t index_count;
	uint32_t vertex_count;
	uint32_t vertex_stride;
};

typedef uint32_t sid;

struct AssetInfo
{
	char *path;
	uint32_t type;
};

const uint32_t ASSET_MAX_PATH_LENGTH = 50;
const uint32_t ASSET_MAX_NAME_LENGTH = 50;

struct AssetDatabase
{
	uint32_t asset_count;
	sid *keys;
	AssetInfo *asset_infos;
};

namespace parsers
{
	MeshData get_mesh_from_obj(File obj_file, StackAllocator *stack_allocator);
	AssetDatabase get_assets_db_from_adf(File adfFile, StackAllocator *stack_allocator);
}