#include "generator.h"
#include "layers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int mapOceanMixMod(const Layer * l, int * out, int x, int z, int w, int h)
{
    int *otyp;
    int i, j;
    l->p2->getMap(l->p2, out, x, z, w, h);

    otyp = (int *) malloc(w*h*sizeof(int));
    memcpy(otyp, out, w*h*sizeof(int));

    l->p->getMap(l->p, out, x, z, w, h);


    for (j = 0; j < h; j++)
    {
        for (i = 0; i < w; i++)
        {
            int landID, oceanID;

            landID = out[i + j*w];

            if (!isOceanic(landID))
                continue;

            oceanID = otyp[i + j*w];

            if (landID == deep_ocean)
            {
                switch (oceanID)
                {
                case lukewarm_ocean:
                    oceanID = deep_lukewarm_ocean;
                    break;
                case ocean:
                    oceanID = deep_ocean;
                    break;
                case cold_ocean:
                    oceanID = deep_cold_ocean;
                    break;
                case frozen_ocean:
                    oceanID = deep_frozen_ocean;
                    break;
                }
            }

            out[i + j*w] = oceanID;
        }
    }

    free(otyp);

    return 0;
}

void setupGenerator(Generator *g, int mc, uint32_t flags)
{
    g->mc = mc;
    g->dim = 1000; // not initialized
    g->flags = flags;
    g->seed = 0;
    g->sha = 0;

    if (mc <= MC_1_17)
    {
        setupLayerStack(&g->ls, mc, flags & LARGE_BIOMES);
        g->entry = NULL;
        if (flags & FORCE_OCEAN_VARIANTS && mc >= MC_1_13)
        {
            g->ls.entry_16 = setupLayer(
                g->xlayer+2, &mapOceanMixMod, mc, 1, 0, 0,
                g->ls.entry_16, &g->ls.layers[L_ZOOM_16_OCEAN]);

            g->ls.entry_64 = setupLayer(
                g->xlayer+3, &mapOceanMixMod, mc, 1, 0, 0,
                g->ls.entry_64, &g->ls.layers[L_ZOOM_64_OCEAN]);

            g->ls.entry_256 = setupLayer(
                g->xlayer+4, &mapOceanMixMod, mc, 1, 0, 0,
                g->ls.entry_256, &g->ls.layers[L_OCEAN_TEMP_256]);
        }
    }
    else
    {
        initBiomeNoise(&g->bn, mc);
    }
}

void applySeed(Generator *g, int dim, uint64_t seed)
{
    g->dim = dim;
    g->seed = seed;
    g->sha = 0;

    if (dim == 0)
    {
        if (g->mc <= MC_1_17)
            setLayerSeed(g->entry ? g->entry : g->ls.entry_1, seed);
        else
            setBiomeSeed(&g->bn, seed, g->flags & LARGE_BIOMES);
    }
    else if (dim == -1 && g->mc >= MC_1_16)
    {
        setNetherSeed(&g->nn, seed);
    }
    else if (dim == +1 && g->mc >= MC_1_9)
    {
        setEndSeed(&g->en, seed);
    }
    if (g->mc >= MC_1_15)
    {
        if (g->mc <= MC_1_17 && dim == 0 && !g->entry)
            g->sha = g->ls.entry_1->startSalt;
        else
            g->sha = getVoronoiSHA(seed);
    }
}


size_t getMinCacheSize(const Generator *g, int scale, int sx, int sy, int sz)
{
    if (sy == 0)
        sy = 1;

    size_t len = (size_t)sx * sz * sy;
    if (g->mc <= MC_1_17 && g->dim == 0)
    {   // recursively check the layer stack for the max buffer
        const Layer *entry = getLayerForScale(g, scale);
        if (!entry) {
            printf("getMinCacheSize(): failed to determine scaled entry\n");
            exit(1);
        }
        size_t len2d = getMinLayerCacheSize(entry, sx, sz);
        len += len2d - sx*sz;
    }
    else if (scale <= 1)
    {   // allocate space for temporary copy of voronoi source
        sx = ((sx+3) >> 2) + 2;
        sy = ((sy+3) >> 2) + 2;
        sz = ((sz+3) >> 2) + 2;
        len += sx * sy * sz;
    }

    return len;
}

int *allocCache(const Generator *g, Range r)
{
    size_t len = getMinCacheSize(g, r.scale, r.sx, r.sy, r.sz);
    return (int*) calloc(len, sizeof(int));
}

int genBiomes(const Generator *g, int *cache, Range r)
{
    int err = 1;
    int i, k;

    if (g->dim == 0)
    {
        if (g->mc <= MC_1_17)
        {
            const Layer *entry = getLayerForScale(g, r.scale);
            if (!entry) return -1;
            err = genArea(entry, cache, r.x, r.z, r.sx, r.sz);
            if (err) return err;
            for (k = 1; k < r.sy; k++)
            {   // overworld has no vertical noise: expanding 2D into 3D
                for (i = 0; i < r.sx*r.sz; i++)
                    cache[k*r.sx*r.sz + i] = cache[i];
            }
            return 0;
        }
        else
        {
            return genBiomeNoiseScaled(&g->bn, cache, r, g->mc, g->sha);
        }
    }
    else if (g->dim == -1)
    {
        return genNetherScaled(&g->nn, cache, r, g->mc, g->sha);
    }
    else if (g->dim == +1)
    {
        return genEndScaled(&g->en, cache, r, g->mc, g->sha);
    }

    return err;
}

int getBiomeAt(const Generator *g, int scale, int x, int y, int z)
{
    Range r = {scale, x, z, 1, 1, y, 1};
    int *ids = allocCache(g, r);
    int id = genBiomes(g, ids, r);
    if (id == 0)
        id = ids[0];
    else
        id = none;
    free(ids);
    return id;
}

const Layer *getLayerForScale(const Generator *g, int scale)
{
    if (g->mc > MC_1_17)
        return NULL;
    switch (scale)
    {
    case 0:   return g->entry;
    case 1:   return g->ls.entry_1;
    case 4:   return g->ls.entry_4;
    case 16:  return g->ls.entry_16;
    case 64:  return g->ls.entry_64;
    case 256: return g->ls.entry_256;
    default:
        return NULL;
    }
}


Layer *setupLayer(Layer *l, mapfunc_t *map, int mc,
    int8_t zoom, int8_t edge, uint64_t saltbase, Layer *p, Layer *p2)
{
    //Layer *l = g->layers + layerId;
    l->getMap = map;
    l->mc = mc;
    l->zoom = zoom;
    l->edge = edge;
    l->scale = 0;
    if (saltbase == 0 || saltbase == LAYER_INIT_SHA)
        l->layerSalt = saltbase;
    else
        l->layerSalt = getLayerSalt(saltbase);
    l->startSalt = 0;
    l->startSeed = 0;
    l->noise = NULL;
    l->data = NULL;
    l->p = p;
    l->p2 = p2;
    return l;
}

static void setupScale(Layer *l, int scale)
{
    l->scale = scale;
    if (l->p)
        setupScale(l->p, scale * l->zoom);
    if (l->p2)
        setupScale(l->p2, scale * l->zoom);
}

void setupLayerStack(LayerStack *g, int mc, int largeBiomes)
{
    if (mc < MC_1_3)
        largeBiomes = 0;

    memset(g, 0, sizeof(LayerStack));
    Layer *p, *l = g->layers;
    // L: layer
    // M: mapping function
    // V: minecraft version
    // Z: zoom
    // E: edge
    // S: salt base
    // P1: parent 1
    // P2: parent 2

    //             L                       M               V   Z  E  S     P1 P2
    p = setupLayer(l+L_CONTINENT_4096,     mapContinent,   mc, 1, 0, 1,    0, 0);
    p = setupLayer(l+L_ZOOM_2048,          mapZoomFuzzy,   mc, 2, 3, 2000, p, 0);
    p = setupLayer(l+L_LAND_2048,          mapLand,        mc, 1, 2, 1,    p, 0);
    p = setupLayer(l+L_ZOOM_1024,          mapZoom,        mc, 2, 3, 2001, p, 0);
    p = setupLayer(l+L_LAND_1024_A,        mapLand,        mc, 1, 2, 2,    p, 0);

    if (mc <= MC_1_6)
    {   //             L                   M               V   Z  E  S     P1 P2
        p = setupLayer(l+L_SNOW_1024,      mapSnow16,      mc, 1, 2, 2,    p, 0);
        p = setupLayer(l+L_ZOOM_512,       mapZoom,        mc, 2, 3, 2002, p, 0);
        p = setupLayer(l+L_LAND_512,       mapLand16,      mc, 1, 2, 3,    p, 0);
        p = setupLayer(l+L_ZOOM_256,       mapZoom,        mc, 2, 3, 2003, p, 0);
        p = setupLayer(l+L_LAND_256,       mapLand16,      mc, 1, 2, 4,    p, 0);
        p = setupLayer(l+L_MUSHROOM_256,   mapMushroom,    mc, 1, 2, 5,    p, 0);
        p = setupLayer(l+L_BIOME_256,      mapBiome,       mc, 1, 0, 200,  p, 0);
        p = setupLayer(l+L_ZOOM_128,       mapZoom,        mc, 2, 3, 1000, p, 0);
        p = setupLayer(l+L_ZOOM_64,        mapZoom,        mc, 2, 3, 1001, p, 0);
        // river noise layer chain, also used to determine where hills generate
        p = setupLayer(l+L_NOISE_256,      mapNoise,       mc, 1, 0, 100,
                       l+L_MUSHROOM_256, 0);
    }
    else
    {   //             L                   M               V   Z  E  S     P1 P2
        p = setupLayer(l+L_LAND_1024_B,    mapLand,        mc, 1, 2, 50,   p, 0);
        p = setupLayer(l+L_LAND_1024_C,    mapLand,        mc, 1, 2, 70,   p, 0);
        p = setupLayer(l+L_ISLAND_1024,    mapIsland,      mc, 1, 2, 2,    p, 0);
        p = setupLayer(l+L_SNOW_1024,      mapSnow,        mc, 1, 2, 2,    p, 0);
        p = setupLayer(l+L_LAND_1024_D,    mapLand,        mc, 1, 2, 3,    p, 0);
        p = setupLayer(l+L_COOL_1024,      mapCool,        mc, 1, 2, 2,    p, 0);
        p = setupLayer(l+L_HEAT_1024,      mapHeat,        mc, 1, 2, 2,    p, 0);
        p = setupLayer(l+L_SPECIAL_1024,   mapSpecial,     mc, 1, 2, 3,    p, 0);
        p = setupLayer(l+L_ZOOM_512,       mapZoom,        mc, 2, 3, 2002, p, 0);
        p = setupLayer(l+L_ZOOM_256,       mapZoom,        mc, 2, 3, 2003, p, 0);
        p = setupLayer(l+L_LAND_256,       mapLand,        mc, 1, 2, 4,    p, 0);
        p = setupLayer(l+L_MUSHROOM_256,   mapMushroom,    mc, 1, 2, 5,    p, 0);
        p = setupLayer(l+L_DEEP_OCEAN_256, mapDeepOcean,   mc, 1, 2, 4,    p, 0);
        p = setupLayer(l+L_BIOME_256,      mapBiome,       mc, 1, 0, 200,  p, 0);
        if (mc >= MC_1_14)
            p = setupLayer(l+L_BAMBOO_256, mapBamboo,      mc, 1, 0, 1001, p, 0);
        p = setupLayer(l+L_ZOOM_128,       mapZoom,        mc, 2, 3, 1000, p, 0);
        p = setupLayer(l+L_ZOOM_64,        mapZoom,        mc, 2, 3, 1001, p, 0);
        p = setupLayer(l+L_BIOME_EDGE_64,  mapBiomeEdge,   mc, 1, 2, 1000, p, 0);
        // river noise layer chain, also used to determine where hills generate
        p = setupLayer(l+L_RIVER_INIT_256, mapNoise,       mc, 1, 0, 100,
                       l+L_DEEP_OCEAN_256, 0);
    }

    if (mc <= MC_1_12)
    {
        p = setupLayer(l+L_ZOOM_128_HILLS, mapZoom,        mc, 2, 3, 0,    p, 0);
        p = setupLayer(l+L_ZOOM_64_HILLS,  mapZoom,        mc, 2, 3, 0,    p, 0);
    }
    else if (mc >= MC_1_1)
    {
        p = setupLayer(l+L_ZOOM_128_HILLS, mapZoom,        mc, 2, 3, 1000, p, 0);
        p = setupLayer(l+L_ZOOM_64_HILLS,  mapZoom,        mc, 2, 3, 1001, p, 0);
    }

    if (mc <= MC_1_0)
    {   //             L                   M               V   Z  E  S     P1 P2
        p = setupLayer(l+L_ZOOM_32,        mapZoom,        mc, 2, 3, 1000,
                       l+L_ZOOM_64, 0);
        p = setupLayer(l+L_LAND_32,        mapLand16,      mc, 1, 2, 3,    p, 0);
        // NOTE: reusing slot for shore:16, but scale is 1:32
        p = setupLayer(l+L_SHORE_16,       mapShore,       mc, 1, 2, 1000, p, 0);
        p = setupLayer(l+L_ZOOM_16,        mapZoom,        mc, 2, 3, 1001, p, 0);
        p = setupLayer(l+L_ZOOM_8,         mapZoom,        mc, 2, 3, 1002, p, 0);
        p = setupLayer(l+L_ZOOM_4,         mapZoom,        mc, 2, 3, 1003, p, 0);

        p = setupLayer(l+L_SMOOTH_4,       mapSmooth,      mc, 1, 2, 1000, p, 0);

        // river layer chain
        p = setupLayer(l+L_ZOOM_128_RIVER, mapZoom,        mc, 2, 3, 1000,
                       l+L_NOISE_256, 0);
        p = setupLayer(l+L_ZOOM_64_RIVER,  mapZoom,        mc, 2, 3, 1001, p, 0);
        p = setupLayer(l+L_ZOOM_32_RIVER,  mapZoom,        mc, 2, 3, 1002, p, 0);
        p = setupLayer(l+L_ZOOM_16_RIVER,  mapZoom,        mc, 2, 3, 1003, p, 0);
        p = setupLayer(l+L_ZOOM_8_RIVER,   mapZoom,        mc, 2, 3, 1004, p, 0);
        p = setupLayer(l+L_ZOOM_4_RIVER,   mapZoom,        mc, 2, 3, 1005, p, 0);

        p = setupLayer(l+L_RIVER_4,        mapRiver,       mc, 1, 2, 1,    p, 0);
        p = setupLayer(l+L_SMOOTH_4_RIVER, mapSmooth,      mc, 1, 2, 1000, p, 0);
    }
    else if (mc <= MC_1_6)
    {
        p = setupLayer(l+L_HILLS_64,       mapHills,       mc, 1, 2, 1000,
                       l+L_ZOOM_64, l+L_ZOOM_64_HILLS);

        p = setupLayer(l+L_ZOOM_32,        mapZoom,        mc, 2, 3, 1000, p, 0);
        p = setupLayer(l+L_LAND_32,        mapLand16,      mc, 1, 2, 3,    p, 0);
        p = setupLayer(l+L_ZOOM_16,        mapZoom,        mc, 2, 3, 1001, p, 0);
        p = setupLayer(l+L_SHORE_16,       mapShore,       mc, 1, 2, 1000, p, 0);
        p = setupLayer(l+L_SWAMP_RIVER_16, mapSwampRiver,  mc, 1, 0, 1000, p, 0);
        p = setupLayer(l+L_ZOOM_8,         mapZoom,        mc, 2, 3, 1002, p, 0);
        p = setupLayer(l+L_ZOOM_4,         mapZoom,        mc, 2, 3, 1003, p, 0);

        if (largeBiomes)
        {
            p = setupLayer(l+L_ZOOM_LARGE_A, mapZoom,      mc, 2, 3, 1004, p, 0);
            p = setupLayer(l+L_ZOOM_LARGE_B, mapZoom,      mc, 2, 3, 1005, p, 0);
        }

        p = setupLayer(l+L_SMOOTH_4,       mapSmooth,      mc, 1, 2, 1000, p, 0);

        // river layer chain
        p = setupLayer(l+L_ZOOM_128_RIVER, mapZoom,        mc, 2, 3, 1000,
                       l+L_NOISE_256, 0);
        p = setupLayer(l+L_ZOOM_64_RIVER,  mapZoom,        mc, 2, 3, 1001, p, 0);
        p = setupLayer(l+L_ZOOM_32_RIVER,  mapZoom,        mc, 2, 3, 1002, p, 0);
        p = setupLayer(l+L_ZOOM_16_RIVER,  mapZoom,        mc, 2, 3, 1003, p, 0);
        p = setupLayer(l+L_ZOOM_8_RIVER,   mapZoom,        mc, 2, 3, 1004, p, 0);
        p = setupLayer(l+L_ZOOM_4_RIVER,   mapZoom,        mc, 2, 3, 1005, p, 0);

        if (largeBiomes)
        {
            p = setupLayer(l+L_ZOOM_L_RIVER_A, mapZoom,    mc, 2, 3, 1006, p, 0);
            p = setupLayer(l+L_ZOOM_L_RIVER_B, mapZoom,    mc, 2, 3, 1007, p, 0);
        }

        p = setupLayer(l+L_RIVER_4,        mapRiver,       mc, 1, 2, 1,    p, 0);
        p = setupLayer(l+L_SMOOTH_4_RIVER, mapSmooth,      mc, 1, 2, 1000, p, 0);
    }
    else
    {
        p = setupLayer(l+L_HILLS_64,       mapHills,       mc, 1, 2, 1000,
                       l+L_BIOME_EDGE_64, l+L_ZOOM_64_HILLS);

        p = setupLayer(l+L_SUNFLOWER_64,   mapSunflower,   mc, 1, 0, 1001, p, 0);
        p = setupLayer(l+L_ZOOM_32,        mapZoom,        mc, 2, 3, 1000, p, 0);
        p = setupLayer(l+L_LAND_32,        mapLand,        mc, 1, 2, 3,    p, 0);
        p = setupLayer(l+L_ZOOM_16,        mapZoom,        mc, 2, 3, 1001, p, 0);
        p = setupLayer(l+L_SHORE_16,       mapShore,       mc, 1, 2, 1000, p, 0);
        p = setupLayer(l+L_ZOOM_8,         mapZoom,        mc, 2, 3, 1002, p, 0);
        p = setupLayer(l+L_ZOOM_4,         mapZoom,        mc, 2, 3, 1003, p, 0);

        if (largeBiomes)
        {
            p = setupLayer(l+L_ZOOM_LARGE_A, mapZoom,      mc, 2, 3, 1004, p, 0);
            p = setupLayer(l+L_ZOOM_LARGE_B, mapZoom,      mc, 2, 3, 1005, p, 0);
        }

        p = setupLayer(l+L_SMOOTH_4,       mapSmooth,      mc, 1, 2, 1000, p, 0);

        // river layer chain
        p = setupLayer(l+L_ZOOM_128_RIVER, mapZoom,        mc, 2, 3, 1000,
                       l+L_RIVER_INIT_256, 0);
        p = setupLayer(l+L_ZOOM_64_RIVER,  mapZoom,        mc, 2, 3, 1001, p, 0);
        p = setupLayer(l+L_ZOOM_32_RIVER,  mapZoom,        mc, 2, 3, 1000, p, 0);
        p = setupLayer(l+L_ZOOM_16_RIVER,  mapZoom,        mc, 2, 3, 1001, p, 0);
        p = setupLayer(l+L_ZOOM_8_RIVER,   mapZoom,        mc, 2, 3, 1002, p, 0);
        p = setupLayer(l+L_ZOOM_4_RIVER,   mapZoom,        mc, 2, 3, 1003, p, 0);

        if (largeBiomes && mc == MC_1_7)
        {
            p = setupLayer(l+L_ZOOM_L_RIVER_A, mapZoom,    mc, 2, 3, 1004, p, 0);
            p = setupLayer(l+L_ZOOM_L_RIVER_B, mapZoom,    mc, 2, 3, 1005, p, 0);
        }

        p = setupLayer(l+L_RIVER_4,        mapRiver,       mc, 1, 2, 1,    p, 0);
        p = setupLayer(l+L_SMOOTH_4_RIVER, mapSmooth,      mc, 1, 2, 1000, p, 0);
    }

    p = setupLayer(l+L_RIVER_MIX_4, mapRiverMix, mc, 1, 0, 100,
                   l+L_SMOOTH_4, l+L_SMOOTH_4_RIVER);


    if (mc <= MC_1_12)
    {
        p = setupLayer(l+L_VORONOI_1, mapVoronoi114, mc, 4, 3, 10, p, 0);
    }
    else
    {
        // ocean variants
        p = setupLayer(l+L_OCEAN_TEMP_256, mapOceanTemp,   mc, 1, 0, 2,    0, 0);
        p->noise = &g->oceanRnd;
        p = setupLayer(l+L_ZOOM_128_OCEAN, mapZoom,        mc, 2, 3, 2001, p, 0);
        p = setupLayer(l+L_ZOOM_64_OCEAN,  mapZoom,        mc, 2, 3, 2002, p, 0);
        p = setupLayer(l+L_ZOOM_32_OCEAN,  mapZoom,        mc, 2, 3, 2003, p, 0);
        p = setupLayer(l+L_ZOOM_16_OCEAN,  mapZoom,        mc, 2, 3, 2004, p, 0);
        p = setupLayer(l+L_ZOOM_8_OCEAN,   mapZoom,        mc, 2, 3, 2005, p, 0);
        p = setupLayer(l+L_ZOOM_4_OCEAN,   mapZoom,        mc, 2, 3, 2006, p, 0);
        p = setupLayer(l+L_OCEAN_MIX_4,    mapOceanMix,    mc, 1, 17, 100,
                       l+L_RIVER_MIX_4, l+L_ZOOM_4_OCEAN);

        if (mc <= MC_1_14)
            p = setupLayer(l+L_VORONOI_1, mapVoronoi114, mc, 4, 3, 10, p, 0);
        else
            p = setupLayer(l+L_VORONOI_1, mapVoronoi, mc, 4, 3, LAYER_INIT_SHA, p, 0);
    }

    g->entry_1 = p;
    g->entry_4 = l + (mc <= MC_1_12 ? L_RIVER_MIX_4 : L_OCEAN_MIX_4);
    if (largeBiomes)
    {
        g->entry_16 = l + L_ZOOM_4;
        g->entry_64 = l + (mc <= MC_1_6 ? L_SWAMP_RIVER_16 : L_SHORE_16);
        g->entry_256 = l + (mc <= MC_1_7 ? L_HILLS_64 : L_SUNFLOWER_64);
    }
    else if (mc >= MC_1_1)
    {
        g->entry_16 = l + (mc <= MC_1_6 ? L_SWAMP_RIVER_16 : L_SHORE_16);
        g->entry_64 = l + (mc <= MC_1_7 ? L_HILLS_64 : L_SUNFLOWER_64);
        g->entry_256 = l + (mc <= MC_1_14 ? L_BIOME_256 : L_BAMBOO_256);
    }
    else
    {
        g->entry_16 = l + L_ZOOM_16;
        g->entry_64 = l + L_ZOOM_64;
        g->entry_256 = l + L_BIOME_256;
    }
    setupScale(g->entry_1, 1);
}


/* Recursively calculates the minimum buffer size required to generate an area
 * of the specified size from the current layer onwards.
 */
static void getMaxArea(
    const Layer *layer, int areaX, int areaZ, int *maxX, int *maxZ, size_t *siz)
{
    if (layer == NULL)
        return;

    areaX += layer->edge;
    areaZ += layer->edge;

    // multi-layers and zoom-layers use a temporary copy of their parent area
    if (layer->p2 || layer->zoom != 1)
        *siz += areaX * areaZ;

    if (areaX > *maxX) *maxX = areaX;
    if (areaZ > *maxZ) *maxZ = areaZ;

    if (layer->zoom == 2)
    {
        areaX >>= 1;
        areaZ >>= 1;
    }
    else if (layer->zoom == 4)
    {
        areaX >>= 2;
        areaZ >>= 2;
    }

    getMaxArea(layer->p, areaX, areaZ, maxX, maxZ, siz);
    if (layer->p2)
        getMaxArea(layer->p2, areaX, areaZ, maxX, maxZ, siz);
}

size_t getMinLayerCacheSize(const Layer *layer, int sizeX, int sizeZ)
{
    int maxX = sizeX, maxZ = sizeZ;
    size_t bufsiz = 0;
    getMaxArea(layer, sizeX, sizeZ, &maxX, &maxZ, &bufsiz);
    return bufsiz + maxX * (size_t)maxZ;
}

int genArea(const Layer *layer, int *out, int areaX, int areaZ, int areaWidth, int areaHeight)
{
    memset(out, 0, areaWidth*areaHeight*sizeof(*out));
    return layer->getMap(layer, out, areaX, areaZ, areaWidth, areaHeight);
}





