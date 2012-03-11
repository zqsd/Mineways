/*
Copyright (c) 2011, Eric Haines
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.
*/


// ObjFile.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "rwpng.h"
#include "blockInfo.h"
#include "cache.h"
#include "MinewaysMap.h"
#include "vector.h"
#include <assert.h>
#include <string.h>
#include <math.h>
#include <time.h>

static PORTAFILE gModelFile;
static PORTAFILE gMtlFile;
static PORTAFILE gPngFile;  // for terrain.png input (not texture output)

#define NO_GROUP_SET 0
#define BOUNDARY_AIR_GROUP 1

typedef struct BoxCell {
    unsigned char type;
    unsigned char origType;
    unsigned char flatFlags;	// top face's type, for "merged" snow, redstone, etc. in cell above
    unsigned char data;     // extra data for block (wool color, etc.)
    int group;	// for 3D printing, what connected group a block is part of
} BoxCell;

typedef struct BoxGroup 
{
    int groupID;	// which group number am I? Always matches index of gGroupInfo array
    int population;	// how many in the group?
    int solid;		// solid or air group?
    IBox bounds;	// the box that this group occupies. Not valid if population is 0 (merged)
} BoxGroup;

static BoxCell *gBoxData = NULL;
static IPoint gBoxSize;
static int gBoxSizeYZ = -999;
static int gBoxSizeXYZ = -999;
// the box bounds of gBoxData that has something in it, before processing
static IBox gSolidBox;
// the box bounds of gBoxData that has something in it, +1 in all directions for air
// Basically, gSolidBox + 1 in all directions, but generated once for readability
static IBox gAirBox;
// Dimensions of what truly has stuff in it, after all processing is done and we're ready to write
static Point gFilledBoxSize;    // in centimeters

static IBox gSolidWorldBox;  // area of solid box in world coordinates
static IPoint gWorld2BoxOffset;

typedef struct FaceRecord {
    int type;	// block id
    int faceIndex;	// tie breaker, so that faces get near each other in location
    int vertexIndex[4];
    int normalIndex;    // always the same! normals all the same for the face
    int uvIndex[4];
} FaceRecord;

typedef struct SwatchComposite {
    int swatchLoc;
    int backgroundSwatchLoc;
    int angle;
    int compositeSwatchLoc;
    struct SwatchComposite *next;
} SwatchComposite;

typedef struct Model {
    float scale;    // size of a block, in meters
    Point center;

    // the standard normals, already rotated into position.
    // first six are the block directions
    // next four are for diagonal billboards
    // last eight are for angled tracks
    Vector normals[18];

    Point *vertices;    // vertices to be output, in a given order
    // a little indirect: there is one of these for every grid *corner* location.
    // The index is therefore a block location, possibly +1 in X, Y, and Z
    // (use gFaceToVertexOffset[face][corner 0-3] to get these offsets)
    // What is returned is the index into the vertices[] array itself, where to
    // find the vertex information.
    int *vertexIndices;
    int vertexCount;    // lowest unused vertex index;
    int vertexListSize;

    // Example: lava is the first block encountered. Its type value is 10. Since this is the first set of UVs, what would
    // be set is:
    // uvType[0] = 10;
    // uvSet[10] = 0; - was -1, the index of which is the first material to be output is put here
    // and textureUsedCount would be increased by 1.
    // Another lava block is encountered. Since uvSet[10] is 1, we don't need to store anything new; lava will already be output now.
    // Now dirt is encountered, type value 3. uvSet[3] is 0, so we need to note the material is getting used. What gets set is
    // uvType[textureUsedCount] = 3;
    // uvSet[3] = 1; - textureUsedCount is 1, so this is stored
    // and textureUsedCount would be increased by 1 to 2.
    // The uvType list gives which materials are used, which is the list of what will generate uvs (done entirely by using the "type" value
    // and mapping it to the output texture) and showing which texture set maps to what values.
    int uvIndexToSwatch[NUM_MAX_SWATCHES];
    // the uvSet is accessed by swatch location, and returns the index into uvType of the set of UV coordinates to output
    // This is the swatch location (same as the block type)
    int uvSwatchToIndex[NUM_MAX_SWATCHES];
    int uvSwatchToType[NUM_MAX_SWATCHES];
    // how many texture swatches were used; also total UVs used, divided by 4.
    int textureUsedCount;

    int billboardCount;
    IBox billboardBounds;

    FaceRecord **faceList;
    int faceCount;
    int faceSize;

    int mtlList[NUM_BLOCKS];
    int mtlCount;

    progimage_info inputTerrainImage;

    int textureResolution;  // size of output texture
    float invTextureResolution; // inverse, commonly used
    int tileSize;    // number of pixels in a tile, e.g. 16
    int swatchSize;  // number of pixels in a swatch, e.g. 18
    int swatchesPerRow;  // a swatch is a tile with a +1 border (or SWATCH_BORDER) around it
    float textureUVPerSwatch; // how much a swatch spans in UV space
    int swatchCount;        // total number of swatches generated
    int swatchListSize;         // the absolute maximum number of swatches available
    SwatchComposite *swatchCompositeList;   // additional swatches of combinations of two types
    SwatchComposite *swatchCompositeListEnd;   // last one on list
    progimage_info *pPNGtexture;
    int usesRGB;    // 1 if the RGB (only) texture is used and so should be output
    int usesRGBA;   // 1 if the RGBA texture is used
    int usesAlpha;   // 1 if the Alpha-only texture is used
} Model;

static Model gModel;

typedef struct CompositeSwatchPreset
{
    int cutoutSwatch;
    int backgroundSwatch;
} CompositeSwatchPreset;

typedef struct FillAlpha {
    int cutout;
    int underlay;
} FillAlpha;

// The size of the border added to a tile to make a swatch. So a 16x16 tile makes an 18x18 swatch.
// The whole point here is simple: bilinear interpolation is done on swatches when rendering textures
// (unless your renderer is cool enough to allow you to turn it off and so get the block Minecraft look).
// To avoid bleeding colors from one tile to another, we have to add a 1 pixel border around the tile
// to make a swatch. Note that this border does not help if mipmapping is on: far enough away, the swatches
// will bleed through to each other. Increase the swatch border to avoid this problem. The downside is that
// a larger border will use up more swatches, so you could run out of swatches made on the fly for composites.
// A composite is something like, say, a ladder or torch put on top of a background swatch, like grass or
// stone. We make these on the fly as needed and add them to the end of the texture, until we run out.
// NOTE! This is not really supported to increase to 2 or more. There needs to be additional code to clamp,
// to run a loop that copies a particular row or column again and again along the edge. Search for SWATCH_BORDER
// and you'll see where the clamps occur.
#define SWATCH_BORDER 1

#define SWATCH_TO_COL_ROW( s, c, r ) \
    (c) = (s) % gModel.swatchesPerRow; \
    (r) = (s) / gModel.swatchesPerRow; \

static char gFacetNormalString[6][256];

static int gSolidGroups = -999;
static int gAirGroups = -999;

static int gGroupListSize = 0;
static BoxGroup *gGroupList = NULL;
static int gGroupCount = -999;

// offsets in box coordinates to the neighboring faces
static int gFaceOffset[6];

static ProgressCallback *gpCallback;

static Options *gOptions;

static FileList *gOutputFileList;

static int gPhysMtl;

static float gUnitsScale=1.0f;

static int gExportBillboards=0;

// extra face directions, for normals
#define DIRECTION_LO_X_LO_Z 6
#define DIRECTION_LO_X_HI_Z 7
#define DIRECTION_HI_X_LO_Z 8
#define DIRECTION_HI_X_HI_Z 9

// for rails
#define DIRECTION_LO_X_LO_Y 10
#define DIRECTION_LO_Z_LO_Y 11
#define DIRECTION_HI_X_LO_Y 12
#define DIRECTION_HI_Z_LO_Y 13
#define DIRECTION_LO_X_HI_Y 14
#define DIRECTION_LO_Z_HI_Y 15
#define DIRECTION_HI_X_HI_Y 16
#define DIRECTION_HI_Z_HI_Y 17

#define OSQRT2 0.707106781f

// types of billboards
#define BB_FULL_CROSS   1
#define BB_GRID         2
#define BB_TORCH        3
#define BB_RAILS        4
#define BB_FIRE         5


#define SWATCH_INDEX( col, row ) (NUM_BLOCKS + (col) + (row)*16)

// these are swatches that we will use for other things;
// The swatches reused are the "breaking block" animations, which we'll never need
#define TORCH_TOP               SWATCH_INDEX( 0, 15 )
#define RS_TORCH_TOP_ON         SWATCH_INDEX( 1, 15 )
#define RS_TORCH_TOP_OFF        SWATCH_INDEX( 2, 15 )
#define REDSTONE_WIRE_ANGLED_2  SWATCH_INDEX( 3, 15 )
#define REDSTONE_WIRE_3         SWATCH_INDEX( 4, 15 )
// these spots are used for compositing, as temporary places to put swatches to edit
#define SWATCH_WORKSPACE        SWATCH_INDEX( 13, 13 )
#define SWATCH_WORKSPACE2       SWATCH_INDEX( 12, 13 )


wchar_t gOutputFilePath[MAX_PATH];
wchar_t gOutputFileRoot[MAX_PATH];
wchar_t gOutputFileRootClean[MAX_PATH]; // used by all files that are referenced inside text files
char gOutputFileRootCleanChar[MAX_PATH];

// how many blocks are needed to make a thick enough wall
static int gWallBlockThickness = -999;
// how many is the user specifying for hollow walls
static int gHollowBlockThickness = -999;

static int gBlockCount = -999;

static int gDebugTransparentType = -999;

static long gMySeed = 12345;

// set to true if we are using a newer terrain.png that includes jungle leaves
static int gJungleExists=0;


// these should not be relied on for much of anything during processing,
// the fields are filled out until writing.
typedef struct ExportStatistics {
    int numBlocks;
    int numGroups;
    int numSolidGroups;
    int numAirGroups;
    int bubblesFound;
    int solidGroupsMerged;
    int numberManifoldPasses;
    int nonManifoldEdgesFound;
    int blocksManifoldWelded;
    int blocksCornertipWelded;
    int blocksHollowed;
    int blocksSuperHollowed;
    int floaterGroupsDeleted;
    int blocksFloaterDeleted;
    float density;  // value 0 to 1, number of blocks filled.
} ExportStatistics;

static ExportStatistics gStats;

typedef struct TypeTile {
    int type;	// block id
    int col;    // location on terrain.png
    int row;
    float colorMult[3]; // how much to scale map color - currently not used, just an idea. Idea is for making pine different than oak, than grass.
} TypeTile;


// given a face direction and a vertex number 0-3, give the relative vertex location (offset of 0 or 1) for X, Y, Z
static int gFaceToVertexOffset[6][4][3] =
{
    {
        {0,0,0},{0,0,1},{0,1,1},{0,1,0}	// -X
    },
    {
        {1,0,1},{0,0,1},{0,0,0},{1,0,0}	// -Y
    },
    {
        {1,0,0},{0,0,0},{0,1,0},{1,1,0}	// -Z
    },
    {
        {1,0,1},{1,0,0},{1,1,0},{1,1,1}	// +X.
    },
    {
        {0,1,1},{1,1,1},{1,1,0},{0,1,0}	// +Y
    },
    {
        {0,0,1},{1,0,1},{1,1,1},{0,1,1}	// +Z
    }
};

static int gFaceDirectionVector[6][3] =
{
    {-1,0,0},{0,-1,0},{0,0,-1},
    { 1,0,0},{0, 1,0},{0,0, 1}
};

#define WERROR(x) if(x) { assert(0); PortaClose(gModelFile); return MW_CANNOT_WRITE_TO_FILE; }


// feed world coordinate in to get box index
#define WORLD_TO_BOX_INDEX(x,y,z) (((x)+gWorld2BoxOffset[X])*gBoxSizeYZ + ((z)+gWorld2BoxOffset[Z])*gBoxSize[Y] + (y)+gWorld2BoxOffset[Y])

// feed relative XYZ indices inside box to get index number
#define BOX_INDEXV(pt)	((pt)[X]*gBoxSizeYZ + (pt)[Z]*gBoxSize[Y] + (pt)[Y])
#define BOX_INDEX(x,y,z)	((x)*gBoxSizeYZ + (z)*gBoxSize[Y] + (y))

// feed chunk number and location to get index inside chunk's data
#define CHUNK_INDEX(bx,bz,x,y,z) (  (y)+ \
                                             (((z)-(bz)*16)+ \
                                             ((x)-(bx)*16)*16)*128)

#define UPDATE_PROGRESS(p)		if (*gpCallback){ (*gpCallback)(p);}

#define AREA_IN_CM2     (gModel.faceCount * gModel.scale * gModel.scale * METERS_TO_CM * METERS_TO_CM)

// when should output be part of the progress bar? That is, 0.80 means 80% done when we start outputting the file
#define PG_DB 0.05f
#define PG_OUTPUT 0.10f
#define PG_TEXTURE 0.45f
#define PG_CLEANUP 0.5f
// leave some time for zipping files
#define PG_END 0.70f

#define NO_INDEX_SET 0xffffffff

// alpha for group debug mode
#define DEBUG_DISPLAY_ALPHA 0.2f

// type for weld edge debug display
#define DEBUG_EDGE_TOUCH_TYPE BLOCK_LAVA
#define DEBUG_CORNER_TOUCH_TYPE 129

// For super-hollow, we identify hollowed areas with group 0
#define HOLLOW_AIR_GROUP 0
// The surround air group is all the air around the model (in all 6 directions, since there's a border of air)
#define SURROUND_AIR_GROUP 1


typedef struct TouchCell {
    int obscurity;
    int count;
    int connections;
} TouchCell;

TouchCell *gTouchGrid;

static int gTouchSize;

typedef struct TouchRecord {
    int boxIndex;
    int obscurity;
    int count;
    float distance;   // really, distance squared
} TouchRecord;

// read these as: given my location, and I'm an air block, TOUCH_MX_MY means
// "in the X=-1 and Y=-1 (and Z=0) direction there is another air block"
#define TOUCH_MX_MY 0x001
#define TOUCH_MX_MZ 0x002
#define TOUCH_MX_PY 0x004
#define TOUCH_MX_PZ 0x008

#define TOUCH_MY_MZ 0x010
#define TOUCH_MY_PZ 0x020

#define TOUCH_PX_MY 0x040
#define TOUCH_PX_MZ 0x080
#define TOUCH_PX_PY 0x100
#define TOUCH_PX_PZ 0x200

#define TOUCH_PY_MZ 0x400
#define TOUCH_PY_PZ 0x800


// anything flattened (made into a decal put on a neighboring solid block) marks the flatFlags in the block itself,
// so that the block knows to look that direction for the flattened decal to apply.
#define FLAT_FACE_LO_X  0x01
#define FLAT_FACE_HI_X  0x02
#define FLAT_FACE_LO_Z  0x04
#define FLAT_FACE_HI_Z  0x08
#define FLAT_FACE_ABOVE 0x10
// vines only:
#define FLAT_FACE_BELOW 0x20

// suffixes for OBJ files' material textures
#define PNG_RGB_SUFFIXCHAR "-RGB"
#define PNG_RGBA_SUFFIXCHAR "-RGBA"
#define PNG_ALPHA_SUFFIXCHAR "-Alpha"
#define PNG_RGB_SUFFIX L"-RGB"
#define PNG_RGBA_SUFFIX L"-RGBA"
#define PNG_ALPHA_SUFFIX L"-Alpha"

static void initializeWorldData( IBox *worldBox, int xmin, int ymin, int zmin, int xmax, int ymax, int zmax );
static void initializeModelData();

static int readTerrainPNG( const wchar_t *curDir, progimage_info *pII, wchar_t *terrainFileName );

static int populateBox(const wchar_t *world, IBox *box);
static void extractChunk(const wchar_t *world, int bx, int bz, IBox *box );

static int filterBox();
static int computeFlatFlags( int boxIndex );
static int saveBillboard( int boxIndex, int type );
static int saveBillboardFaces( int boxIndex, int type, int billboardType );
static void checkGroupListSize();
static void checkVertexListSize();
static void checkFaceListSize();

static void findGroups();
static void addVolumeToGroup( int groupID, int minx, int miny, int minz, int maxx, int maxy, int maxz );
static void propagateSeed(IPoint point, BoxGroup *groupInfo, IPoint *seedStack, int *seedCount);
static int getNeighbor( int faceDirection, IPoint newPoint );
static void getNeighborUnsafe( int faceDirection, IPoint newPoint );

static void checkAndRemoveBubbles();
static void findNeighboringGroups( IBox *bounds, int groupID, int *neighborGroups );

//static void establishGroupBounds();

static void fillGroups( IBox *bounds, int masterGroupID, int solid, int fillType, int *targetGroupIDs );

static void addBounds( IPoint loc, IBox *bounds );
static void addBoundsToBounds( IBox inBounds, IBox *bounds );

static int connectCornerTips();
static int checkForCorner(int boxIndex, int offx, int offz);

static int fixTouchingEdges();
static int touchRecordCompare( void *context, const void *str1, const void *str2);
static void checkForTouchingEdge(int boxIndex, int offx, int offy, int offz);
static int computeObscurity( int boxIndex );
static void decrementNeighbors( int boxIndex );
static float getDistanceSquared( Point loc1, Point loc2 );
static void boxIndexToLoc( IPoint loc, int boxIndex );

static void deleteFloatingGroups();
static int determineScaleAndHollowAndMelt();
static void scaleByCost();
static void hollowBottomOfModel();
static void meltSnow();
static void hollowSeed( int x, int y, int z, IPoint *seedList, int *seedCount );

static void generateBlockDataAndStatistics();
static int faceIdCompare( void *context, const void *str1, const void *str2);

static int getDimensionsAndCount( Point dimensions );
static void rotateLocation( Point pt );
static void checkAndCreateFaces( int boxIndex, IPoint loc );
static int checkMakeFace( int type, int neighborType, int view3D );
static void saveVertices( int boxIndex, int faceDirection, IPoint loc );
static void saveFaceLoop( int boxIndex, int faceDirection );
static int getMaterialUsingGroup( int groupID );
static int getSwatch( int type, int dataVal, int faceDirection, int backgroundIndex, int uvIndices[4] );
static int getCompositeSwatch( int swatchLoc, int backgroundIndex, int faceDirection, int angle );
static int createCompositeSwatch( int swatchLoc, int backgroundSwatchLoc, int angle );

static void flipIndicesLeftRight( int localIndices[4] );
static void rotateIndices( int localIndices[4], int angle );
static void saveTextureUVs( int swatchLoc, int type );

static void freeModel( Model *pModel );

static int writeAsciiSTLBox( const wchar_t *world, IBox *box );
static int writeBinarySTLBox( const wchar_t *world, IBox *box );
static int writeOBJBox( const wchar_t *world, IBox *box );
static int writeOBJTextureUVs( int type );
static int writeOBJMtlFile();

static int writeVRML2Box( const wchar_t *world, IBox *box );
static int writeVRMLTextureUVs( int type );

static int writeLines( HANDLE file, char **textLines, int lines );

static int writeStatistics( HANDLE fh, const char *justWorldFileName, IBox *worldBox );

static float computeMaterialCost( int printMaterialType, float blockEdgeSize, int numBlocks, float densityRatio );
static int finalModelChecks();

static void addOutputFilenameToList(wchar_t *filename);

static void spacesToUnderlines( wchar_t *targetString );
static void spacesToUnderlinesChar( char *targetString );

static int createBaseMaterialTexture();

static void copyPNGArea(progimage_info *dst, int dst_x_min, int dst_y_min, int size_x, int size_y, progimage_info *src, int src_x_min, int src_y_min);
static int tileIsSemitransparent(progimage_info *src, int col, int row);
static int tileIsCutout(progimage_info *src, int col, int row);
static int tileIsOpaque(progimage_info *src, int col, int row);
static void setColorPNGArea(progimage_info *dst, int dst_x_min, int dst_y_min, int size_x, int size_y, unsigned int value);
static void stretchSwatchToTop(progimage_info *dst, int swatchIndex, float startStretch);
static void copyPNGTile(progimage_info *dst, int dst_x, int dst_y, int tileSize, progimage_info *src, int src_x, int src_y);
static void setColorPNGTile(progimage_info *dst, int x, int y, int tileSize, unsigned int value);
static void addNoisePNGTile(progimage_info *dst, int x, int y, int tileSize, unsigned char r, unsigned char g, unsigned char b, unsigned char a, float noise );
static void multiplyPNGTile(progimage_info *dst, int x, int y, int tileSize, unsigned char r, unsigned char g, unsigned char b, unsigned char a );
//static void grayZeroAlphasPNG(progimage_info *dst, unsigned char r, unsigned char g, unsigned char b ); TODO remove?
//static void fillZeroAlphasPNGSwatch(progimage_info *dst, int destSwatch, int sourceSwatch, int swatchSize, int swatchesPerRow );
static void rotatePNGTile(progimage_info *dst, int dcol, int drow, int scol, int srow, int angle, int swatchSize );
static void blendTwoSwatches( progimage_info *dst, int txrSwatch, int solidSwatch, float blend, unsigned char alpha );
static void compositePNGSwatches(progimage_info *dst, int dstSwatch, int overSwatch, int underSwatch, int swatchSize, int swatchesPerRow, int forceSolid );
static int convertRGBAtoRGBandWrite(progimage_info *src, wchar_t *filename);
static void convertAlphaToGrayscale( progimage_info *dst );

static void ensureSuffix( wchar_t *dst, const wchar_t *src, const wchar_t *suffix );
static void removeSuffix( wchar_t *dst, const wchar_t *src, const wchar_t *suffix );
//static const wchar_t *removePath( const wchar_t *src );
static const char *removePathChar( const char *src );

static void wcharToChar( const wchar_t *inWString, char *outString );
static void charToWchar( char *inString, wchar_t *outWString );
static void getPathAndRoot( const wchar_t *src, int fileType, wchar_t *path, wchar_t *root );
static void concatFileName2(wchar_t *dst, const wchar_t *src1, const wchar_t *src2);
static void concatFileName3(wchar_t *dst, const wchar_t *src1, const wchar_t *src2, const wchar_t *src3);
static void concatFileName4(wchar_t *dst, const wchar_t *src1, const wchar_t *src2, const wchar_t *src3, const wchar_t *src4);
static void wcharCleanse( wchar_t *wstring );

static void myseedrand( long seed );
static double myrand();


void ChangeCache( int size )
{
    Change_Cache_Size(size);
}
////////////////////////////////////////////////////////
//
// Main code begins

//world = path to world saves
//min* = minimum block location of box to save
//max* = maximum block location of box to save

// Return 0 if all is well, errcode if something went wrong.
// User should be warned if nothing found to export.
int SaveVolume( wchar_t *saveFileName, int fileType, Options *options, const wchar_t *world, const wchar_t *curDir, int xmin, int ymin, int zmin, int xmax, int ymax, int zmax, 
    ProgressCallback callback, wchar_t *terrainFileName, FileList *outputFileList )
{
//#ifdef WIN32
//    DWORD br;
//#endif

    IBox worldBox;
    int retCode = MW_NO_ERROR;
    int newRetCode;
    int needDifferentTextures = 0;

    memset(&gStats,0,sizeof(ExportStatistics));
    // clear all of gModel to zeroes
    memset(&gModel,0,sizeof(Model));

    // reset random number seed
    myseedrand(12345);

    //test of making cache large: Change_Cache_Size( 15000 );

    gOptions = options;
    gOptions->totalBlocks = 0;
    gOptions->cost = 0.0f;
    gpCallback = &callback;
    gOutputFileList = outputFileList;

    // get path name and root of output file name as separate globals. Also have a
    // "clean" (no extended characters, spaces turns to _) version of the output name, for the files
    // that are referenced, such as material and texture files. We will
    // use these elements to then build up the output names.
    getPathAndRoot( saveFileName, fileType, gOutputFilePath, gOutputFileRoot );
    wcscpy_s(gOutputFileRootClean,MAX_PATH,gOutputFileRoot);
    wcharCleanse(gOutputFileRootClean);
    spacesToUnderlines(gOutputFileRootClean);
    wcharToChar(gOutputFileRootClean, gOutputFileRootCleanChar);

    // first things very first: if full texturing is wanted, check if the texture is readable
    if ( gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES )
    {
        retCode = readTerrainPNG(curDir,&gModel.inputTerrainImage,terrainFileName);
        if ( retCode >= MW_BEGIN_ERRORS )
        {
            // nothing in box, so end.
            goto Exit;
        }
        gModel.tileSize = gModel.inputTerrainImage.width/16;
    }

    // write texture, if needed
    if (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE)
    {
        // make it twice as large if we're outputting image textures, too- we need the space
        if ( gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES )
        {
            gModel.textureResolution = 2*gModel.inputTerrainImage.width;
        }
        else
        {
            // fixed 256 x 256 - we could actually make this texture quite small
            gModel.textureResolution = 256;
            gModel.inputTerrainImage.width = 256;    // really, no image, but act like there is
        }
        // there are always 16 tiles in terrain.png, so we divide by this.
        gModel.tileSize = gModel.inputTerrainImage.width/16;
        gModel.swatchSize = 2 + gModel.tileSize;
        gModel.invTextureResolution = 1.0f / (float)gModel.textureResolution;
        gModel.swatchesPerRow = (int)(gModel.textureResolution / gModel.swatchSize);
        gModel.textureUVPerSwatch = (float)gModel.swatchSize / (float)gModel.textureResolution; // e.g. 18 / 256
        gModel.swatchListSize = gModel.swatchesPerRow*gModel.swatchesPerRow;

        retCode |= createBaseMaterialTexture();
    }

    gPhysMtl = gOptions->pEFD->comboPhysicalMaterial[gOptions->pEFD->fileType];

    gUnitsScale = unitTypeTable[gOptions->pEFD->comboModelUnits[gOptions->pEFD->fileType]].unitsPerMeter;

    gBoxData = NULL;

    initializeWorldData( &worldBox, xmin, ymin, zmin, xmax, ymax, zmax );

    retCode = populateBox(world, &worldBox);
    if ( retCode >= MW_BEGIN_ERRORS )
    {
        // nothing in box, so end.
        goto Exit;
    }

    initializeModelData();

    UPDATE_PROGRESS(0.10f*PG_DB);

    newRetCode = filterBox();
    // always return the worst error
    retCode = max(newRetCode,retCode);
    if ( retCode >= MW_BEGIN_ERRORS )
    {
        // problem found
        goto Exit;
    }
    UPDATE_PROGRESS(0.80f*PG_DB);
    newRetCode = determineScaleAndHollowAndMelt();
    retCode = max(newRetCode,retCode);
    if ( retCode >= MW_BEGIN_ERRORS )
    {
        // problem found
        goto Exit;
    }
    UPDATE_PROGRESS(PG_DB);

    // TODO idea: not sure it's needed, but we could provide a "melt" function, in which all objects of
    // a given ID are removed from the final model before output. This gives the user a way to connect
    // hollowed areas with interiors and let the building material out of "escape holes".

    // create database and compute statistics for output
    generateBlockDataAndStatistics();

    UPDATE_PROGRESS(PG_OUTPUT);

    switch ( fileType )
    {
    case FILE_TYPE_WAVEFRONT_REL_OBJ:
    case FILE_TYPE_WAVEFRONT_ABS_OBJ:
        // for OBJ, we may use more than one texture
        needDifferentTextures = 1;
        newRetCode = writeOBJBox( world, &worldBox );
        break;
    case FILE_TYPE_BINARY_MAGICS_STL:
    case FILE_TYPE_BINARY_VISCAM_STL:
        newRetCode = writeBinarySTLBox( world, &worldBox );
        break;
    case FILE_TYPE_ASCII_STL:
        newRetCode = writeAsciiSTLBox( world, &worldBox );
        break;
    case FILE_TYPE_VRML2:
        newRetCode = writeVRML2Box( world, &worldBox );
        break;
    }

    retCode = max(newRetCode,retCode);
    if ( retCode >= MW_BEGIN_ERRORS )
    {
        // problem found
        goto Exit;
    }

    retCode = finalModelChecks();

    // done!
    Exit:

    // write out texture file, if any input data
    UPDATE_PROGRESS(PG_TEXTURE);
    if ( gModel.pPNGtexture != NULL )
    {
#define FA_TABLE_SIZE 54
#define FA_TABLE__VIEW_SIZE 17
		static FillAlpha faTable[FA_TABLE_SIZE] =
		{
			// stuff that is put in always, fringes that need to be filled
			{ SWATCH_INDEX( 5, 9 ), BLOCK_BLACK_WOOL }, // bed
			{ SWATCH_INDEX( 6, 9 ), BLOCK_BLACK_WOOL }, // bed
			{ SWATCH_INDEX( 7, 9 ), BLOCK_BLACK_WOOL }, // bed
			{ SWATCH_INDEX( 8, 9 ), BLOCK_BLACK_WOOL }, // bed
			{ SWATCH_INDEX( 5, 4 ), BLOCK_CACTUS }, // cactus
			{ SWATCH_INDEX( 6, 4 ), BLOCK_CACTUS }, // cactus
			{ SWATCH_INDEX( 7, 4 ), BLOCK_CACTUS }, // cactus
			{ SWATCH_INDEX( 9, 7 ), BLOCK_CAKE }, // cake
			{ SWATCH_INDEX( 10, 7 ), BLOCK_CAKE }, // cake
			{ SWATCH_INDEX( 11, 7 ), BLOCK_CAKE }, // cake
			{ SWATCH_INDEX( 12, 7 ), BLOCK_CAKE }, // cake
			{ SWATCH_INDEX( 10, 8 ), BLOCK_BLACK_WOOL }, // cauldron
			{ SWATCH_INDEX( 10, 9 ), BLOCK_BLACK_WOOL }, // cauldron
			{ SWATCH_INDEX( 11, 9 ), BLOCK_BLACK_WOOL }, // cauldron
			{ SWATCH_INDEX( 15, 9 ), SWATCH_INDEX(15,10) }, // ender portal (should be filled, but just in case, due to stretch)
			{ SWATCH_INDEX( 15, 14 ), BLOCK_LAVA }, // lava, in case it's not filled
			{ SWATCH_INDEX( 15, 15 ), BLOCK_STATIONARY_LAVA }, // stationary lava, in case it's not present


			// stuff filled in for 3D printing only
			{ SWATCH_INDEX( 11, 0 ), SWATCH_INDEX( 6, 3 ) }, // spiderweb over stone block
			{ SWATCH_INDEX( 12, 0 ), SWATCH_INDEX( 0, 0 ) }, // red flower over grass
			{ SWATCH_INDEX( 13, 0 ), SWATCH_INDEX( 0, 0 ) }, // yellow flower over grass
			{ SWATCH_INDEX( 15, 0 ), SWATCH_INDEX( 0, 0 ) }, // sapling over grass
			{ SWATCH_INDEX( 12, 1 ), SWATCH_INDEX( 0, 0 ) }, // red mushroom over grass
			{ SWATCH_INDEX( 13, 1 ), SWATCH_INDEX( 0, 0 ) }, // brown mushroom over grass
			{ SWATCH_INDEX( 1, 3 ), BLOCK_GLASS }, // glass over glass color
			{ SWATCH_INDEX( 4, 3 ), BLOCK_AIR }, // transparent leaves over air (black) - doesn't really matter, not used when printing anyway
			{ SWATCH_INDEX( 7, 3 ), SWATCH_INDEX( 2, 1 ) }, // dead bush over grass
			{ SWATCH_INDEX( 8, 3 ), SWATCH_INDEX( 0, 0 ) }, // sapling over grass
			{ SWATCH_INDEX( 1, 4 ), SWATCH_INDEX( 1, 0 ) }, // spawner over stone
			{ SWATCH_INDEX( 9, 4 ), SWATCH_INDEX( 0, 0 ) }, // reeds over grass
			{ SWATCH_INDEX( 15, 4 ), SWATCH_INDEX( 0, 0 ) }, // sapling over grass
			{ SWATCH_INDEX( 1, 5 ), SWATCH_INDEX( 6, 0 ) }, // wooden door top over slab top
			{ SWATCH_INDEX( 2, 5 ), SWATCH_INDEX( 6, 0 ) }, // door top over slab top
			{ SWATCH_INDEX( 5, 5 ), SWATCH_INDEX( 6, 3 ) }, // iron bars over stone block
			{ SWATCH_INDEX( 8, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
			{ SWATCH_INDEX( 9, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
			{ SWATCH_INDEX( 10, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
			{ SWATCH_INDEX( 11, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
			{ SWATCH_INDEX( 12, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
			{ SWATCH_INDEX( 13, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
			{ SWATCH_INDEX( 14, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
			{ SWATCH_INDEX( 15, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
			{ SWATCH_INDEX( 0, 6 ), SWATCH_INDEX( 1, 0 ) }, // lever over stone
			{ SWATCH_INDEX( 15, 6 ), SWATCH_INDEX( 6, 5 ) }, // stem over farmland
			{ SWATCH_INDEX( 15, 7 ), SWATCH_INDEX( 6, 5 ) }, // dead stem over farmland
			{ SWATCH_INDEX( 4, 8 ), BLOCK_AIR }, // leaves over air (black) - doesn't really matter, not used
			{ SWATCH_INDEX( 10, 8 ), BLOCK_AIR }, // cauldron over air (black)
			{ SWATCH_INDEX( 12, 8 ), BLOCK_AIR }, // cake over air (black) - what's this for, anyway?
			{ SWATCH_INDEX( 4, 9 ), BLOCK_GLASS }, // glass pane over glass color (more interesting than stone, and lets you choose)
			{ SWATCH_INDEX( 13, 9 ), SWATCH_INDEX( 1, 0 ) }, // brewing stand over stone
			{ SWATCH_INDEX( 15, 10 ), SWATCH_INDEX( 1, 0 ) }, // end portal thingy (unused) over stone
			{ SWATCH_INDEX( 4, 12 ), BLOCK_AIR }, // jungle leaves over air (black) - doesn't really matter, not used
			{ SWATCH_INDEX( 2, 14 ), SWATCH_INDEX( 8, 6 ) }, // nether wart over soul sand
			{ SWATCH_INDEX( 3, 14 ), SWATCH_INDEX( 8, 6 ) }, // nether wart over soul sand
			{ SWATCH_INDEX( 4, 14 ), SWATCH_INDEX( 8, 6 ) }, // nether wart over soul sand
		};

		int rc;
		int i;

		// fill in all alphas that 3D export wants filled; always fill in cactus, cake, and bed fringes, for example;
		// For printing we also then composite over other backgrounds as the defaults.
		int faTableCount = ( gOptions->exportFlags & EXPT_3DPRINT ) ? FA_TABLE_SIZE : FA_TABLE__VIEW_SIZE;
		for ( i = 0; i < faTableCount; i++ )
		{
			compositePNGSwatches( gModel.pPNGtexture,
				faTable[i].cutout, faTable[i].cutout, faTable[i].underlay,
				gModel.swatchSize, gModel.swatchesPerRow, 0 );
		}

        // final swatch cleanup if textures are used and we're doing 3D printing
        if ( (gOptions->exportFlags & EXPT_3DPRINT) && (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES) )
        {
            int i;


#define FA_FINAL_TABLE_SIZE 19
            static FillAlpha faFinalTable[] =
            {
                { SWATCH_INDEX( 0, 8 ), SWATCH_INDEX( 1, 0 ) }, // rail over stone
                { SWATCH_INDEX( 0, 7 ), SWATCH_INDEX( 1, 0 ) }, // curved rail over stone
                { SWATCH_INDEX( 0, 5 ), SWATCH_INDEX( 1, 0 ) }, // torch over stone
                { SWATCH_INDEX( 4, 10 ), SWATCH_INDEX( 1, 0 ) }, // wire over stone
                { SWATCH_INDEX( 3, 5 ), SWATCH_INDEX( 1, 0 ) }, // ladder over stone
                { SWATCH_INDEX( 3, 11 ), SWATCH_INDEX( 1, 0 ) }, // powered rail over stone
                { SWATCH_INDEX( 3, 10 ), SWATCH_INDEX( 1, 0 ) }, // unpowered rail over stone
                { SWATCH_INDEX( 3, 12 ), SWATCH_INDEX( 1, 0 ) }, // detector rail over stone
                { SWATCH_INDEX( 3, 6 ), SWATCH_INDEX( 1, 0 ) }, // redstone torch on over stone
                { SWATCH_INDEX( 3, 7 ), SWATCH_INDEX( 1, 0 ) }, // redstone torch off over stone
                { SWATCH_INDEX( 12, 4 ), SWATCH_INDEX( 15, 13 ) }, // lily pad over water
				{ SWATCH_INDEX( 4, 5 ), SWATCH_INDEX( 1, 0 ) }, // trapdoor over stone
				{ SWATCH_INDEX( 15, 8 ), SWATCH_INDEX( 0, 0 ) }, // vines over grass

                // stuff we don't use, so don't need defaults for
                { SWATCH_INDEX( 5, 10 ), SWATCH_INDEX( 1, 0 ) }, // wire over stone
                { SWATCH_INDEX( 4, 11 ), SWATCH_INDEX( 1, 0 ) }, // wire over stone
                { SWATCH_INDEX( 5, 11 ), SWATCH_INDEX( 1, 0 ) }, // wire over stone

                { SWATCH_INDEX( 0, 15 ), SWATCH_INDEX( 1, 0 ) }, // torch top
                { SWATCH_INDEX( 1, 15 ), SWATCH_INDEX( 1, 0 ) }, // redstone torch on top
                { SWATCH_INDEX( 2, 15 ), SWATCH_INDEX( 1, 0 ) }, // redstone torch off top
            };

            // now that we're totally done, fill in the main pieces which were left empty as templates;
            // these probably won't get used (and really, we could use them as the initial composited swatches, which we created - silly duplication TODO >>>>>),
            for ( i = 0; i < FA_FINAL_TABLE_SIZE; i++ )
            {
                compositePNGSwatches( gModel.pPNGtexture,
                    faFinalTable[i].cutout, faFinalTable[i].cutout, faFinalTable[i].underlay,
                    gModel.swatchSize, gModel.swatchesPerRow, 0 );
            }
        }

        // if we're debugging groups, make the largest group transparent and set it to red
        if ( (gOptions->exportFlags & EXPT_DEBUG_SHOW_GROUPS) &&
            (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE) )
        {
            unsigned char a = (unsigned char)(DEBUG_DISPLAY_ALPHA * 255);
            unsigned char r = 0xff;
            unsigned char g = 0x00;
            unsigned char b = 0x00;
            unsigned int color = (a<<24)|(b<<16)|(g<<8)|r;
            int col, row;
            
            SWATCH_TO_COL_ROW( gDebugTransparentType, col, row );
            setColorPNGTile( gModel.pPNGtexture, col, row, gModel.swatchSize, color );
        }

        // do we need three textures, or just the one RGBA texture?
        if ( needDifferentTextures )
        {
            // need all three
            wchar_t textureRGB[MAX_PATH];
            wchar_t textureRGBA[MAX_PATH];
            wchar_t textureAlpha[MAX_PATH];

            // Write them out! We need three texture file names: -RGB, -RGBA, -Alpha.
            // The RGB/RGBA split is needed for fast previewers like G3D to gain additional speed
            // The all-alpha image is needed for various renderers to properly read cutouts
            concatFileName4(textureRGB, gOutputFilePath, gOutputFileRootClean, PNG_RGB_SUFFIX, L".png");
            concatFileName4(textureRGBA, gOutputFilePath, gOutputFileRootClean, PNG_RGBA_SUFFIX, L".png");
            concatFileName4(textureAlpha, gOutputFilePath, gOutputFileRootClean, PNG_ALPHA_SUFFIX, L".png");

            if ( gModel.usesRGBA )
            {
                // output RGBA version
                rc = writepng(gModel.pPNGtexture,4,textureRGBA);
                addOutputFilenameToList(textureRGBA);
                assert(rc == 0);
                retCode |= rc ? MW_CANNOT_CREATE_FILE : MW_NO_ERROR;
            }

            if ( gModel.usesRGB )
            {
                // output RGB version
                rc = convertRGBAtoRGBandWrite(gModel.pPNGtexture,textureRGB);
                assert(rc == 0);
                retCode |= rc ? MW_CANNOT_CREATE_FILE : MW_NO_ERROR;
            }

            if ( gModel.usesAlpha )
            {
                // output Alpha version, which is actually RGBA, to make 3DS MAX happy
                convertAlphaToGrayscale( gModel.pPNGtexture );
                rc = writepng(gModel.pPNGtexture,4,textureAlpha);
                addOutputFilenameToList(textureAlpha);
                assert(rc == 0);
                retCode |= rc ? MW_CANNOT_CREATE_FILE : MW_NO_ERROR;
            }
        }
        else
        {
            // just the one (VRML). If we're printing, we can convert this one down to RGB
            wchar_t textureFileName[MAX_PATH];
            concatFileName3(textureFileName,gOutputFilePath,gOutputFileRootClean,L".png");
            if ( gOptions->exportFlags & EXPT_3DPRINT )
            {
                rc = convertRGBAtoRGBandWrite(gModel.pPNGtexture,textureFileName);
            }
            else
            {
                rc = writepng(gModel.pPNGtexture,4,textureFileName);
                addOutputFilenameToList(textureFileName);
            }
            assert(rc == 0);
            retCode |= rc ? MW_CANNOT_CREATE_FILE : MW_NO_ERROR;
        }

        writepng_cleanup(gModel.pPNGtexture);
        free(gModel.pPNGtexture->image_data);
    }

    freeModel( &gModel );

    if ( gBoxData )
        free(gBoxData);
    gBoxData = NULL;

    if ( gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES )
    {
        readpng_cleanup(1,&gModel.inputTerrainImage);
    }

    UPDATE_PROGRESS(PG_END);

    return retCode;
}

static void initializeWorldData( IBox *worldBox, int xmin, int ymin, int zmin, int xmax, int ymax, int zmax )
{
    // clean up a bit: make sure max>=min, and limit Y
    ymin = clamp(ymin,0,127);
    ymax = clamp(ymax,0,127);

    // we don't really require one to be min or max, we take the range
    if ( xmin > xmax ) swapint(xmin,xmax);
    if ( ymin > ymax ) swapint(ymin,ymax);
    if ( zmin > zmax ) swapint(zmin,zmax);

    // add an air border of 1 block around the whole box
    gBoxSize[X] = xmax - xmin + 3;
    gBoxSize[Y] = ymax - ymin + 3;
    gBoxSize[Z] = zmax - zmin + 3;
    // scale for X index value
    gBoxSizeYZ = gBoxSize[Y] * gBoxSize[Z];
    gBoxSizeXYZ = gBoxSize[X] * gBoxSizeYZ;

    gFaceOffset[0] = -gBoxSizeYZ;	// -X
    gFaceOffset[1] = -1;			// -Y
    gFaceOffset[2] = -gBoxSize[Y];	// -Z
    gFaceOffset[3] =  gBoxSizeYZ;	// +X
    gFaceOffset[4] =  1;			// +Y
    gFaceOffset[5] =  gBoxSize[Y];	// +Z

    // what to add to a world coordinate to get a box coordinate;
    // the -1 is the "air" border of one block
    gWorld2BoxOffset[X] = 1 - xmin;
    gWorld2BoxOffset[Y] = 1 - ymin;
    gWorld2BoxOffset[Z] = 1 - zmin;

    Vec3Scalar( worldBox->min, =, xmin, ymin, zmin );
    Vec3Scalar( worldBox->max, =, xmax, ymax, zmax );
}

static void initializeModelData()
{
    int i, x,y,z, boxIndex, faceDirection;

    // allocate vertex index array for box (we can ignore all the outer edge
    // box cells, which is why this array is one smaller).
    // For convenience, we make it the same size anyway. So, index 0 is
    // the -x/-y/-z corner of box 0, which will never be used.
    gModel.vertexIndices = (int*)malloc(gBoxSizeXYZ*sizeof(int));   // this one never needs realloc
    gModel.vertexListSize = gBoxSizeXYZ;
    gModel.vertices = (Point*)malloc(gBoxSizeXYZ*sizeof(Point));

    VecScalar( gModel.billboardBounds.min, =,  999999);
    VecScalar( gModel.billboardBounds.max, =, -999999);

    // NO_INDEX_SET means vertex is not used
    for ( i = 0; i < gBoxSizeXYZ; i++ )
        gModel.vertexIndices[i] = NO_INDEX_SET;

    // count about how many faces we'll need to store and sort for output; this code will probably have to change
    // as we get more involved faces (welds, etc.)
    for ( x = gSolidBox.min[X]; x <= gSolidBox.max[X]; x++ )
    {
        for ( z = gSolidBox.min[Z]; z <= gSolidBox.max[Z]; z++ )
        {
            boxIndex = BOX_INDEX(x,gSolidBox.min[Y],z);
            for ( y = gSolidBox.min[Y]; y <= gSolidBox.max[Y]; y++, boxIndex++ )
            {
                for ( faceDirection = 0; faceDirection < 6; faceDirection++ )
                {
                    if ( gBoxData[boxIndex].type > BLOCK_AIR ) 
                    {
                        if ( gBoxData[boxIndex + gFaceOffset[faceDirection]].type <= BLOCK_AIR )
                            gModel.faceSize++;
                    }
                }
            }
        }
    }
    // double the face size count, just to avoid frequent reallocation
    // - it can sometimes get even higher, with foliage + billboards
    gModel.faceSize = gModel.faceSize*2 + 1;
    gModel.faceList = (FaceRecord**)malloc(gModel.faceSize*sizeof(FaceRecord*));

    if (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE)
    {
        for ( i = 0; i < NUM_MAX_SWATCHES; i++ )
        {
            gModel.uvSwatchToIndex[i] = -1;
        }
    }
}

static int readTerrainPNG( const wchar_t *curDir, progimage_info *pII, wchar_t *selectedTerrainFileName )
{
    // file should be in same directory as .exe, sort of
    wchar_t defaultTerrainFileName[MAX_PATH];
    int rc=0;
    int foundPower=0;
    int i;
    int tryDefault=1;

    memset(pII,0,sizeof(progimage_info));

    if ( wcslen(selectedTerrainFileName) > 0 )
    {
        rc = readpng(pII,selectedTerrainFileName);

        // did we open the PNG successfully?
        if ( rc == 0 )
        {
            // yes, so don't open default
            tryDefault=0;
        }
    }

    if ( tryDefault )
    {
        concatFileName2(defaultTerrainFileName,curDir,L"\\terrain.png");

        memset(pII,0,sizeof(progimage_info));
        rc = readpng(pII,defaultTerrainFileName);
    }

    if ( rc )
        return MW_CANNOT_READ_IMAGE_FILE;

    if ( pII->width != pII->height )
        return MW_IMAGE_WRONG_SIZE;

    // what power of two is the image? Start at 16x16 as a minimum,
    // since we need 16x16 images stored.
    for ( i = 4; i < 16 && !foundPower; i++ )
    {
        if ( pII->width == (1<<i) )
            foundPower = 1;
    }

    if ( !foundPower )
        return MW_IMAGE_WRONG_SIZE;

    return MW_NO_ERROR;
}

static int populateBox(const wchar_t *world, IBox *worldBox)
{
    int startxblock, startzblock;
    int endxblock, endzblock;
    int blockX, blockZ;

    gBoxData = (BoxCell*)malloc(gBoxSizeXYZ*sizeof(BoxCell));
    // set all values to "air"
    memset(gBoxData,0x0,gBoxSizeXYZ*sizeof(BoxCell));

    // grab the data block needed, with a border of "air", 0, around the set
    startxblock=(int)floor((float)worldBox->min[X]/16.0f);
    startzblock=(int)floor((float)worldBox->min[Z]/16.0f);
    endxblock=(int)floor((float)worldBox->max[X]/16.0f);
    endzblock=(int)floor((float)worldBox->max[Z]/16.0f);

    // get bounds on Y coordinates, since top part of box is usually air
    VecScalar( gSolidWorldBox.min, =,  999999 );
    VecScalar( gSolidWorldBox.max, =, -999999 );

    // x increases (old) south, decreases north
    for ( blockX=startxblock; blockX<=endxblock; blockX++ )
    {
        //UPDATE_PROGRESS( 0.1f*(blockX-startxblock+1)/(endxblock-startxblock+1) );
        // z increases west, decreases east
        for ( blockZ=startzblock; blockZ<=endzblock; blockZ++ )
        {
            // this method also sets gSolidWorldBox
            extractChunk(world,blockX,blockZ,worldBox);
        }
    }
    if (gSolidWorldBox.min[Y] > gSolidWorldBox.max[Y])
    {
        // nothing to do: there is nothing in the box
        return MW_NO_BLOCKS_FOUND;
    }

    // convert to solid relative box (0 through boxSize-1)
    Vec3Op( gSolidBox.min, =, gSolidWorldBox.min, +, gWorld2BoxOffset );
    Vec3Op( gSolidBox.max, =, gSolidWorldBox.max, +, gWorld2BoxOffset );

    // adjust to box coordinates, and add 1 air fringe
    Vec2Op( gAirBox.min, =, -1 + gSolidBox.min );
    Vec2Op( gAirBox.max, =,  1 + gSolidBox.max );
    assert( (gAirBox.min[Y] >= 0) && (gAirBox.max[Y] < gBoxSize[Y]) );

    return MW_NO_ERROR;
}

// copy relevant part of a given chunk to the box data grid
static void extractChunk(const wchar_t *world, int bx, int bz, IBox *worldBox )
{
    int chunkX, chunkZ;

    int loopXmin, loopZmin;
    int loopXmax, loopZmax;
    int x,y,z;

    int chunkIndex, boxIndex;
    int blockID;

    IPoint loc;
    //unsigned char dataVal;

    WorldBlock *block;
    block=(WorldBlock *)Cache_Find(bx,bz);

    if (block==NULL)
    {
        wchar_t directory[256];
        wcsncpy_s(directory,256,world,255);
        wcsncat_s(directory,256,L"/",1);
        if (gOptions->worldType&HELL)
        {
            wcsncat_s(directory,256,L"DIM-1/",6);
        }
        if (gOptions->worldType&ENDER)
        {
            wcsncat_s(directory,256,L"DIM1/",5);
        }

        block=LoadBlock(directory,bx,bz);
        if (block==NULL) //blank tile, nothing to do
            return;

        Cache_Add(bx,bz,block);
    }

    // loop through area of box that overlaps with this chunk
    chunkX = bx * 16;
    chunkZ = bz * 16;

    loopXmin = max(worldBox->min[X],chunkX);
    loopZmin = max(worldBox->min[Z],chunkZ);

    loopXmax = min(worldBox->max[X],chunkX+15);
    loopZmax = min(worldBox->max[Z],chunkZ+15);

    for ( x = loopXmin; x <= loopXmax; x++ ) {
        for ( z = loopZmin; z <= loopZmax; z++ ) {
            boxIndex = WORLD_TO_BOX_INDEX(x,worldBox->min[Y],z);
            chunkIndex = CHUNK_INDEX(bx,bz,x,worldBox->min[Y],z);
            for ( y = worldBox->min[Y]; y <= worldBox->max[Y]; y++, boxIndex++ ) {
                // Get the extra values (orientation, type) for the blocks
                unsigned char dataVal = block->data[chunkIndex/2];
                if ( chunkIndex & 0x01 )
                    dataVal = dataVal >> 4;
                else
                    dataVal &= 0xf;
                gBoxData[boxIndex].data = dataVal;
                blockID = gBoxData[boxIndex].origType = 
                    gBoxData[boxIndex].type = block->grid[chunkIndex++];
                // get bounds on y searches:
                // search for air, and if not found then
                // add to vertical bounds.
                if ( blockID > BLOCK_AIR )
                {
                    Vec3Scalar( loc, =, x,y,z );
                    addBounds(loc,&gSolidWorldBox);

                    // special: if it's a wire, clear the data value. We use this later for
                    // how the wires actually connect to each other.
                    if ( blockID == BLOCK_REDSTONE_WIRE )
                    {
                        gBoxData[boxIndex].data = 0x0;
                    }
                }
            }
        }
    }
}

// remove snow blocks and anything else not desired
static int filterBox()
{
    int boxIndex;
    int x,y,z;
    // Push flattop onto block below
    int flatten = gOptions->pEFD->chkMergeFlattop;

    int retCode = MW_NO_ERROR;
    int foundBlock = 0;

    // true only if we're exporting full textures and for rendering only, and billboards are flagged as visible
    gExportBillboards = (!(gOptions->exportFlags & EXPT_3DPRINT)) &&
        (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES) &&
        gOptions->pEFD->chkExportAll;

    // Filter out all stuff that is not to be rendered. If billboards are in use, create these
    // as found
    for ( x = gSolidBox.min[X]; x <= gSolidBox.max[X]; x++ )
    {
        for ( z = gSolidBox.min[Z]; z <= gSolidBox.max[Z]; z++ )
        {
            boxIndex = BOX_INDEX(x,gSolidBox.min[Y],z);
            for ( y = gSolidBox.min[Y]; y <= gSolidBox.max[Y]; y++, boxIndex++ )
            {
                // sorry, air is never allowed to turn solid
                if ( gBoxData[boxIndex].type != BLOCK_AIR )
                {
                    int flags = gBlockDefinitions[gBoxData[boxIndex].type].flags;

                    // check if it's something to be filtered out: not in the output list or alpha is 0
                    if ( !(flags & gOptions->saveFilterFlags) ||
                        gBlockDefinitions[gBoxData[boxIndex].type].alpha <= 0.0 ) {
                            // things that should not be saved should be gone, gone, gone
                            gBoxData[boxIndex].type =   gBoxData[boxIndex].origType = BLOCK_AIR;
                            gBoxData[boxIndex].data = 0x0;
                    }
                    else
                    {
                        // check: is it a billboard we can export? Clear it out if so.
                        if ( gExportBillboards && (flags & (BLF_BILLBOARD|BLF_SMALL_BILLBOARD)) )
                        {
                            if ( saveBillboard( boxIndex, gBoxData[boxIndex].type ) )
                            {
                                // this block is then cleared out, since it's been processed.
                                gBoxData[boxIndex].type = BLOCK_AIR;
                                foundBlock = 1;
                            }
                        }

                        // not filtered out by the basics or billboard
                        if ( flatten && ( gBlockDefinitions[gBoxData[boxIndex].type].flags & (BLF_FLATTOP|BLF_FLATSIDE) ) )
                        {
                            // this block is redstone, a rail, a ladder, etc. - shove its face to the top of the next cell down,
                            // or to its neighbor, or both (depends on dataval),
                            // instead of rendering a block for it.

                            // was: gBoxData[boxIndex-1].flatFlags = gBoxData[boxIndex].type;
                            // if object was indeed flattened, set it to air
                            if ( computeFlatFlags( boxIndex ) )
                            {
                                gBoxData[boxIndex].type = BLOCK_AIR;
                            }
                        }
                        // note that we found any sort of block that was valid (flats don't count, whatever
                        // they're pushed against needs to exist, too)
                        foundBlock |= (gBoxData[boxIndex].type > BLOCK_AIR);
                    }
                }
            }
        }
    }
    UPDATE_PROGRESS(0.20f*PG_DB);
    if ( foundBlock == 0 )
        // everything got filtered out!
        return MW_NO_BLOCKS_FOUND;

    // If we'd doing 3D printing, there are lots of things to worry about. Here are some:
    // 1) Hollow spots in the model that can't get cleared out of material. You need to make fair-size holes so that
    //    the unsolidified material inside can be removed. Fix by filling in these "bubbles".
    // 2) Non-manifold edges. For Minecraft, this always means "where two cubes touch at only an edge", i.e.
    //    diagonal (along exactly two axes - touching at a corner is fine). Fix by adding "weld blocks" next to them.
    // 3) Hanging objects. Tree tops, for example, commonly will sit in the air at the boundaries of the selection.
    //    Fix by deleting any groups of objects smaller than 16 blocks that don't touch the ground. 16 is chosen as
    //    16 blocks is the minimum to make a chain link at the end of a chain (a 5x5 square minus the 3x3 middle),
    //    which *is* legal.
    // 4) Unattached objects. While it's perfectly legal to have blocks touch at corners, the object might fall apart
    //    when created. Option: weld such blocks together if they're part of different groups.
    // 5) Wasted material. While bubbles are illegal, it's fine to hollow out the base of any object, working from the
    //    bottom up. Option: delete a block at the base if it has neighboring blocks in all 8 positions on this level
    //    and all 9 positions on the level above. This may be overconservative in some cases, but is safe. Mark all
    //    these positions, working up the object, then delete.

    gSolidGroups = gAirGroups = 0;

    // do we need groups and neighbors?
    if ( gOptions->exportFlags & (EXPT_FILL_BUBBLES|EXPT_CONNECT_PARTS|EXPT_DELETE_FLOATING_OBJECTS) )
    {
        int foundTouching = 0;
        gGroupListSize = 200;
        gGroupList = (BoxGroup *)malloc(gGroupListSize*sizeof(BoxGroup));
        memset(gGroupList,0,gGroupListSize*sizeof(BoxGroup));
        gGroupCount = 0;

        findGroups();

        // We now know a bit about how the blocks are grouped. The first group is always air, the second
        // group is always solid. If there are any more air groups, these are bubbles surround by one or more
        // solid groups and must be filled (and the solid groups merged). If there are corner tips that touch,
        // and the solid groups are different, and one of the solid groups is hanging in space, we should weld.
        // If there are objects that hang in the air (we tighten the Y bounds, so there will always be at least
        // one object that doesn't), if the group is small (16 or less), then delete it.

        // Note that if there is only one solid group, we don't need further processing: this group is what will
        // be cleaned up (manifold), hollowed, and exported.

        // Now that we have the groups and their bounds, start processing them.

        // 1) Hollow spots in the model that can't get cleared out of material. You need to make fair-size holes so that
        //    the unsolidified material inside can be removed. Fix by filling in these "bubbles".
        // Really, we could start the count at 2, since the first group is air,
        // the second group is something solid, and is a group at the base (minimum Y).
        // If we're deleting floaters, we need to merge groups first. This is done by merging bubbles but not changing from AIR.
        if ( gOptions->exportFlags & (EXPT_FILL_BUBBLES|EXPT_DELETE_FLOATING_OBJECTS) )
        {
            if ( gAirGroups > 1 )
            {
                checkAndRemoveBubbles();
            }
        }
        UPDATE_PROGRESS(0.40f*PG_DB);

        // 2) Non-manifold edges. For Minecraft, this always means "where two cubes touch at only an edge", i.e.
        //    diagonal (along exactly two axes - touching at a corner is fine). Fix by adding "weld blocks" next to them.
        // These are good to do next, as we want to build up all material before carving away.
        // If two different groups are welded together, add them to the same meta group.
        // Note that a single group can have non-manifold geometry, so this test must always be done (if flagged)
        // Also, check if when a block is next to be added, would it *make* another manifold edge? If so, pass it by until
        // we're desperate. If any like this are added, need to run manifold again (but do this later, after joining tips
        // below).
        do
        {
            if ( gOptions->exportFlags & EXPT_CONNECT_PARTS )
            {
                foundTouching = fixTouchingEdges();
            }

            // 4) Unattached corners. While it's perfectly legal to have blocks touch at corner tips, the object might fall apart
            //    when created. Option: weld such blocks together if they're part of different groups.
            // This is why we need metagroups, because holes could get filled and groups get joined.
            // If there is only one solid original group, we don't have to do this test.
            // We do this after doing edge part connections: our pass here puts in one block that will then get joined to
            // other parts out there, by edge welding. We add *after* because we want parts to get connected "normally" by
            // connecting edges.
            if ( gOptions->exportFlags & EXPT_CONNECT_CORNER_TIPS )
            {
                assert( gOptions->exportFlags & EXPT_CONNECT_PARTS );
                if ( gSolidGroups > 1 )
                {
                    foundTouching |= connectCornerTips();
                }
            }

            // run again if any non-manifold made manifold.
            // we must process until we find *no* non-manifold edges that are fixed. Fixing can create
            // more non-manifold (touching) edges.
        } while ( foundTouching );

        UPDATE_PROGRESS(0.70f*PG_DB);


        // 3) Hanging objects. Tree tops, for example, commonly will sit in the air at the boundaries of the selection.
        //    Fix by deleting any groups of objects smaller than 16 blocks that don't touch the ground. 16 is chosen as
        //    16 blocks is the minimum to make a chain link at the end of a chain (a 5x5 square minus the 3x3 middle),
        //    which *is* legal.
        // Now delete any tiny objects, unless there's only one group
        // Make this an option, as a person could be making "charms" and having
        // a bunch of little objects in a single order.
        if ( gOptions->exportFlags & EXPT_DELETE_FLOATING_OBJECTS )
        {
            // delete only if there's more than one solid group. One solid group means this is the object to output.
            if ( gSolidGroups > 1 )
            {
                // delete only groups that have a min Y > the base gMinY+1 level, i.e. aren't at ground level
                // OR delete tree (even at ground level - who wants a tree that will fall over?).
                deleteFloatingGroups();
            }

            // it's possible that all groups are deleted
            if ( gSolidGroups == 0 )
            {
                retCode = MW_ALL_BLOCKS_DELETED;
                goto Exit;
            }
        }

        // if debug for groups is on, and materials are being output, then
        // change the alpha for the largest group to be semitransparent. In
        // this way you can see the small groups left over much more easily.
        // Set this while gGroupList is still around.
        if ( (gOptions->exportFlags & EXPT_DEBUG_SHOW_GROUPS) &&
            (gOptions->exportFlags & EXPT_OUTPUT_MATERIALS) )
        {
            int groupMaxID=-1;
            int maxPop = -1;
            int i;
            for ( i = 0; i < gGroupCount; i++ )
            {
                if ( gGroupList[i].population > maxPop && gGroupList[i].solid )
                {
                    groupMaxID = gGroupList[i].groupID;
                    maxPop = gGroupList[i].population;
                }
            }
            assert(groupMaxID>=0);
            // now we know which group is the largest. Set its
            // alpha to semitransparent when material and texture are
            // output
            gDebugTransparentType = getMaterialUsingGroup(groupMaxID);
        }

        Exit:
        free(gGroupList);
    }
    return retCode;
}

static int computeFlatFlags( int boxIndex )
{
    // for this box's contents, mark the neighbor(s) that should receive
    // its flatness
    IPoint loc;

    switch ( gBoxData[boxIndex].type )
    {
        // easy ones: flattops
    case BLOCK_RAIL:
        if ( gBoxData[boxIndex].data >= 6 )
        {
            // curved rail bit, it's always just flat
            gBoxData[boxIndex-1].flatFlags |= FLAT_FACE_ABOVE;
            break;
        }
        // NOTE: if curve test failed, needed only for basic rails, continue on through tilted track tests
        // SO - don't dare put a break here, we need to flow through, to avoid repeating all this code
    case BLOCK_POWERED_RAIL:
    case BLOCK_DETECTOR_RAIL:
        // only pay attention to sloped rails, as these mark sides;
        // remove top bit, as that's whether it's powered
        switch ( gBoxData[boxIndex].data & 0x7 )
        {
        case 2: // new east, +X
            gBoxData[boxIndex+gBoxSizeYZ].flatFlags |= FLAT_FACE_LO_X;
            break;
        case 3:
            gBoxData[boxIndex-gBoxSizeYZ].flatFlags |= FLAT_FACE_HI_X;
            break;
        case 4:
            gBoxData[boxIndex-gBoxSize[Y]].flatFlags |= FLAT_FACE_HI_Z;
            break;
        case 5:
            gBoxData[boxIndex+gBoxSize[Y]].flatFlags |= FLAT_FACE_LO_Z;
            break;
        default:
            // don't do anything, this rail is not sloped; continue on down to mark top face
            break;
        }
        gBoxData[boxIndex-1].flatFlags |= FLAT_FACE_ABOVE;
        break;

        // the block below this one, if solid, gets marked
    case BLOCK_STONE_PRESSURE_PLATE:
    case BLOCK_WOODEN_PRESSURE_PLATE:
    case BLOCK_SNOW:
    case BLOCK_REDSTONE_REPEATER_OFF:
    case BLOCK_REDSTONE_REPEATER_ON:
    case BLOCK_LILY_PAD:
        gBoxData[boxIndex-1].flatFlags |= FLAT_FACE_ABOVE;
        break;

    case BLOCK_TORCH:
    case BLOCK_REDSTONE_TORCH_OFF:
    case BLOCK_REDSTONE_TORCH_ON:
        switch ( gBoxData[boxIndex].data )
        {
        case 1: // new east, +X
            gBoxData[boxIndex-gBoxSizeYZ].flatFlags |= FLAT_FACE_HI_X;
            break;
        case 2:
            gBoxData[boxIndex+gBoxSizeYZ].flatFlags |= FLAT_FACE_LO_X;
            break;
        case 3:
            gBoxData[boxIndex-gBoxSize[Y]].flatFlags |= FLAT_FACE_HI_Z;
            break;
        case 4:
            gBoxData[boxIndex+gBoxSize[Y]].flatFlags |= FLAT_FACE_LO_Z;
            break;
        case 5:
            gBoxData[boxIndex-1].flatFlags |= FLAT_FACE_ABOVE;
            break;
        default:
            // don't do anything, this redstone is not touching a side
            break;
        }
        break;

    case BLOCK_REDSTONE_WIRE: // 0x37
        gBoxData[boxIndex-1].flatFlags |= FLAT_FACE_ABOVE;
        // look to see whether there is wire neighboring and above: if so, run this wire
        // up the sides of the blocks

        // first, is the block above air or glass? If not, then no wires will run up sides.
        if ( (gBoxData[boxIndex+1].origType == BLOCK_AIR) ||
            (gBoxData[boxIndex+1].origType == BLOCK_GLASS) ||
            (gBoxData[boxIndex+1].origType == BLOCK_GLASS_PANE) )
        {
            // first hurdle passed - now check each in turn: is block above wire. If so,
            // then these will connect. Note we must check again origType, as wires get culled out
            // as we go through the blocks.
            if ( gBoxData[boxIndex+1+gBoxSizeYZ].origType == BLOCK_REDSTONE_WIRE )
            {
                gBoxData[boxIndex+gBoxSizeYZ].flatFlags |= FLAT_FACE_LO_X;
                gBoxData[boxIndex+1+gBoxSizeYZ].data |= FLAT_FACE_LO_X;
                gBoxData[boxIndex].data |= FLAT_FACE_HI_X;
            }
            if ( gBoxData[boxIndex+1-gBoxSizeYZ].origType == BLOCK_REDSTONE_WIRE )
            {
                gBoxData[boxIndex-gBoxSizeYZ].flatFlags |= FLAT_FACE_HI_X;
                gBoxData[boxIndex+1-gBoxSizeYZ].data |= FLAT_FACE_HI_X;
                gBoxData[boxIndex].data |= FLAT_FACE_LO_X;
            }
            if ( gBoxData[boxIndex+1+gBoxSize[Y]].origType == BLOCK_REDSTONE_WIRE )
            {
                gBoxData[boxIndex+gBoxSize[Y]].flatFlags |= FLAT_FACE_LO_Z;
                gBoxData[boxIndex+1+gBoxSize[Y]].data |= FLAT_FACE_LO_Z;
                gBoxData[boxIndex].data |= FLAT_FACE_HI_Z;
            }
            if ( gBoxData[boxIndex+1-gBoxSize[Y]].origType == BLOCK_REDSTONE_WIRE )
            {
                gBoxData[boxIndex-gBoxSize[Y]].flatFlags |= FLAT_FACE_HI_Z;
                gBoxData[boxIndex+1-gBoxSize[Y]].data |= FLAT_FACE_HI_Z;
                gBoxData[boxIndex].data |= FLAT_FACE_LO_Z;
            }
        }
        // finally, check the +X and +Z neighbors on this level: if wire, connect them.
        // Note that the other wires in the world will pick up the other 6 possibilities:
        // -X and -Z on this level (by these same tests below) and the 4 "wires down a level"
        // possibilities (by these same tests above).
        // Test *all* things that redstone connects to. This could be a table, for speed.
        if ( gBoxData[boxIndex+gBoxSizeYZ].origType == BLOCK_REDSTONE_WIRE ||
            gBoxData[boxIndex+gBoxSizeYZ].origType == BLOCK_WOODEN_PRESSURE_PLATE ||
            gBoxData[boxIndex+gBoxSizeYZ].origType == BLOCK_STONE_PRESSURE_PLATE ||
            gBoxData[boxIndex+gBoxSizeYZ].origType == BLOCK_LEVER ||
            gBoxData[boxIndex+gBoxSizeYZ].origType == BLOCK_REDSTONE_REPEATER_OFF ||
            gBoxData[boxIndex+gBoxSizeYZ].origType == BLOCK_REDSTONE_REPEATER_ON ||
            gBoxData[boxIndex+gBoxSizeYZ].origType == BLOCK_DETECTOR_RAIL ||
            gBoxData[boxIndex+gBoxSizeYZ].origType == BLOCK_REDSTONE_TORCH_ON ||
            gBoxData[boxIndex+gBoxSizeYZ].origType == BLOCK_REDSTONE_TORCH_OFF
            )
        {
            if ( gBoxData[boxIndex+gBoxSizeYZ].origType == BLOCK_REDSTONE_WIRE )
                gBoxData[boxIndex+gBoxSizeYZ].data |= FLAT_FACE_LO_X;
            gBoxData[boxIndex].data |= FLAT_FACE_HI_X;
        }
        if ( gBoxData[boxIndex+gBoxSize[Y]].origType == BLOCK_REDSTONE_WIRE ||
            gBoxData[boxIndex+gBoxSize[Y]].origType == BLOCK_WOODEN_PRESSURE_PLATE ||
            gBoxData[boxIndex+gBoxSize[Y]].origType == BLOCK_STONE_PRESSURE_PLATE ||
            gBoxData[boxIndex+gBoxSize[Y]].origType == BLOCK_LEVER ||
            gBoxData[boxIndex+gBoxSize[Y]].origType == BLOCK_REDSTONE_REPEATER_OFF ||
            gBoxData[boxIndex+gBoxSize[Y]].origType == BLOCK_REDSTONE_REPEATER_ON ||
            gBoxData[boxIndex+gBoxSize[Y]].origType == BLOCK_DETECTOR_RAIL ||
            gBoxData[boxIndex+gBoxSize[Y]].origType == BLOCK_REDSTONE_TORCH_ON ||
            gBoxData[boxIndex+gBoxSize[Y]].origType == BLOCK_REDSTONE_TORCH_OFF
            )
        {
            if ( gBoxData[boxIndex+gBoxSize[Y]].origType == BLOCK_REDSTONE_WIRE )
                gBoxData[boxIndex+gBoxSize[Y]].data |= FLAT_FACE_LO_Z;
            gBoxData[boxIndex].data |= FLAT_FACE_HI_Z;
        }
        // catch redstone torches at the -X and -Z faces
        if ( gBoxData[boxIndex-gBoxSizeYZ].origType == BLOCK_REDSTONE_TORCH_ON ||
            gBoxData[boxIndex-gBoxSizeYZ].origType == BLOCK_REDSTONE_TORCH_OFF ||
            gBoxData[boxIndex-gBoxSizeYZ].origType == BLOCK_WOODEN_PRESSURE_PLATE ||
            gBoxData[boxIndex-gBoxSizeYZ].origType == BLOCK_STONE_PRESSURE_PLATE ||
            gBoxData[boxIndex-gBoxSizeYZ].origType == BLOCK_LEVER ||
            gBoxData[boxIndex-gBoxSizeYZ].origType == BLOCK_REDSTONE_REPEATER_OFF ||
            gBoxData[boxIndex-gBoxSizeYZ].origType == BLOCK_REDSTONE_REPEATER_ON ||
            gBoxData[boxIndex-gBoxSizeYZ].origType == BLOCK_DETECTOR_RAIL
            )
        {
            gBoxData[boxIndex].data |= FLAT_FACE_LO_X;
        }
        if ( gBoxData[boxIndex-gBoxSize[Y]].origType == BLOCK_REDSTONE_TORCH_ON ||
            gBoxData[boxIndex-gBoxSize[Y]].origType == BLOCK_REDSTONE_TORCH_OFF ||
            gBoxData[boxIndex-gBoxSize[Y]].origType == BLOCK_WOODEN_PRESSURE_PLATE ||
            gBoxData[boxIndex-gBoxSize[Y]].origType == BLOCK_STONE_PRESSURE_PLATE ||
            gBoxData[boxIndex-gBoxSize[Y]].origType == BLOCK_LEVER ||
            gBoxData[boxIndex-gBoxSize[Y]].origType == BLOCK_REDSTONE_REPEATER_OFF ||
            gBoxData[boxIndex-gBoxSize[Y]].origType == BLOCK_REDSTONE_REPEATER_ON ||
            gBoxData[boxIndex-gBoxSize[Y]].origType == BLOCK_DETECTOR_RAIL
            )
        {
            gBoxData[boxIndex].data |= FLAT_FACE_LO_Z;
        }

        break;

    case BLOCK_LADDER:
    case BLOCK_WALL_SIGN:
        switch ( gBoxData[boxIndex].data )
        {
        case 2: // new north, -Z
            gBoxData[boxIndex+gBoxSize[Y]].flatFlags |= FLAT_FACE_LO_Z;
            break;
        case 3: // new south, +Z
            gBoxData[boxIndex-gBoxSize[Y]].flatFlags |= FLAT_FACE_HI_Z;
            break;
        case 4: // new west, -X
            gBoxData[boxIndex+gBoxSizeYZ].flatFlags |= FLAT_FACE_LO_X;
            break;
        case 5: // new east, +X
            gBoxData[boxIndex-gBoxSizeYZ].flatFlags |= FLAT_FACE_HI_X;
            break;
        default:
            assert(0);
            return 0;
        }
        break;

    case BLOCK_LEVER:
        switch ( gBoxData[boxIndex].data & 0x7 )
        {
        case 1: // new east, +X
            gBoxData[boxIndex-gBoxSizeYZ].flatFlags |= FLAT_FACE_HI_X;
            break;
        case 2:
            gBoxData[boxIndex+gBoxSizeYZ].flatFlags |= FLAT_FACE_LO_X;
            break;
        case 3:
            gBoxData[boxIndex-gBoxSize[Y]].flatFlags |= FLAT_FACE_HI_Z;
            break;
        case 4:
            gBoxData[boxIndex+gBoxSize[Y]].flatFlags |= FLAT_FACE_LO_Z;
            break;
        case 5:
        case 6:
            gBoxData[boxIndex-1].flatFlags |= FLAT_FACE_ABOVE;
            break;
        default:
            assert(0);
            return 0;
        }
        break;
    case BLOCK_STONE_BUTTON:
        switch ( gBoxData[boxIndex].data & 0x7 )
        {
        case 4: // new north, -Z
            gBoxData[boxIndex+gBoxSize[Y]].flatFlags |= FLAT_FACE_LO_Z;
            break;
        case 3: // new south, +Z
            gBoxData[boxIndex-gBoxSize[Y]].flatFlags |= FLAT_FACE_HI_Z;
            break;
        case 2: // new west, -X
            gBoxData[boxIndex+gBoxSizeYZ].flatFlags |= FLAT_FACE_LO_X;
            break;
        case 1: // new east, +X
            gBoxData[boxIndex-gBoxSizeYZ].flatFlags |= FLAT_FACE_HI_X;
            break;
        default:
            assert(0);
            return 0;
        }
        break;

    case BLOCK_TRAPDOOR:
        if ( gBoxData[boxIndex].data & 0x4 )
        {
            switch ( gBoxData[boxIndex].data & 0x3 )
            {
            case 0: // new north, -Z
                gBoxData[boxIndex+gBoxSize[Y]].flatFlags |= FLAT_FACE_LO_Z;
                break;
            case 1: // new south, +Z
                gBoxData[boxIndex-gBoxSize[Y]].flatFlags |= FLAT_FACE_HI_Z;
                break;
            case 2: // new west, -X
                gBoxData[boxIndex+gBoxSizeYZ].flatFlags |= FLAT_FACE_LO_X;
                break;
            case 3: // new east, +X
                gBoxData[boxIndex-gBoxSizeYZ].flatFlags |= FLAT_FACE_HI_X;
                break;
            default:
                assert(0);
                return 0;
            }
        }
        else
        {
            // not open, so connected to floor (if any!). Very special case:
            // if there's nothing below trapdoor, block below is set to trapdoor, if
            // Y is not too low
            if ( gBoxData[boxIndex-1].origType == BLOCK_AIR )
            {
                boxIndexToLoc(loc, boxIndex);
                if ( loc[Y] > gSolidBox.min[Y] )
                {
                    gBoxData[boxIndex-1].origType = BLOCK_TRAPDOOR;
                }
            }
            else
            {
                // mark the solid box, as usual
                gBoxData[boxIndex-1].flatFlags |= FLAT_FACE_ABOVE;
            }
        }
        break;

	case BLOCK_VINES:
		// first, if this block was not originally a vine, then forget it - this block was generated
		// by a vine spreading to its neighbor - see below.
		if ( gBoxData[boxIndex].origType != BLOCK_VINES )
		{
			return 0;
		}
		// the rules: vines can cover up to four sides, or if no bits set, top of overhanging block.
		// The overhanging block stops side faces from appearing, essentially.
		if ( gBoxData[boxIndex].data == 0 )
		{
			// top face, flatten to bottom of block above, if the neighbor exists. If it doesn't,
			// something odd is going on (this shouldn't happen).
			if ( gBoxData[boxIndex+1].origType != BLOCK_AIR )
			{
				gBoxData[boxIndex+1].flatFlags |= FLAT_FACE_BELOW;
			}
			else
			{
				// fill the block with vine, since we can't flatten it above for some reason
				// (it could well be that 
				return 0;
			}
		}
		// side faces: if it can be flattened to neighbor, great, do it, else actually *put*
		// a vine block in the empty neighbor space. Might work...
		else
		{
			if ( gBoxData[boxIndex].data & 0x1 )
			{
				// south face (+Z)
				// is there a neighbor large enough to composite a vine onto?
				// Right now we keep vines solid if they are next to leaves, so that the vines hanging down in air below these
				// will continue to look OK, i.e. fully connected as vines.
				// TODO >>>>> shift the "air vines" inwards, as shown in the "else" statement. However, this code here is not
				// really the place to do it - vines could extend past the border, and if "seal tunnels" etc. is done things go
				// very wrong.
				if ( gBlockDefinitions[gBoxData[boxIndex+gBoxSize[Y]].type].flags & (BLF_WHOLE|BLF_ALMOST_WHOLE|BLF_STAIRS|BLF_HALF) &&
					 gBoxData[boxIndex+gBoxSize[Y]].type != BLOCK_LEAVES )
				{
					// neighbor's a whole block, so shove the vine onto it
					gBoxData[boxIndex+gBoxSize[Y]].flatFlags |= FLAT_FACE_LO_Z;
				}
				else
				{
					// force the block to become a vine - could be weird if there was something else here.
					// This is not quite legal, first of all because we might set a location to solid that's outside the border
					//gBoxData[boxIndex+gBoxSize[Y]].type = BLOCK_VINES;
					//gBoxData[boxIndex+gBoxSize[Y]].data = 0x0;
					return 0;
				}
			}
			if ( gBoxData[boxIndex].data & 0x2 )
			{
				// west face (-X)
				// is there a neighbor?
				if ( gBlockDefinitions[gBoxData[boxIndex-gBoxSizeYZ].type].flags & (BLF_WHOLE|BLF_ALMOST_WHOLE|BLF_STAIRS|BLF_HALF) &&
					gBoxData[boxIndex-gBoxSizeYZ].type != BLOCK_LEAVES )
				{
					// neighbor's a whole block, so shove the vine onto it
					gBoxData[boxIndex-gBoxSizeYZ].flatFlags |= FLAT_FACE_HI_X;
				}
				else
				{
					// force the block to become a vine - could be weird if there was something else here.
					//gBoxData[boxIndex-gBoxSizeYZ].type = BLOCK_VINES;
					//gBoxData[boxIndex-gBoxSizeYZ].data = 0x0;
					return 0;
				}
			}
			if ( gBoxData[boxIndex].data & 0x4 )
			{
				// north face (-Z)
				// is there a neighbor?
				if ( gBlockDefinitions[gBoxData[boxIndex-gBoxSize[Y]].type].flags & (BLF_WHOLE|BLF_ALMOST_WHOLE|BLF_STAIRS|BLF_HALF) &&
					gBoxData[boxIndex-gBoxSize[Y]].type != BLOCK_LEAVES )
				{
					// neighbor's a real-live whole block, so shove the vine onto it
					gBoxData[boxIndex-gBoxSize[Y]].flatFlags |= FLAT_FACE_HI_Z;
				}
				else
				{
					// TODO >>>>> for rendering export, we really want vines to always be offset billboards, I believe
					// force the block to become a vine - could be weird if there was something else here.
					//gBoxData[boxIndex-gBoxSize[Y]].type = BLOCK_VINES;
					//gBoxData[boxIndex-gBoxSize[Y]].data = 0x0;
					return 0;
				}
			}
			if ( gBoxData[boxIndex].data & 0x8 )
			{
				// east face (+X)
				// is there a neighbor?
				if ( gBlockDefinitions[gBoxData[boxIndex+gBoxSizeYZ].type].flags & (BLF_WHOLE|BLF_ALMOST_WHOLE|BLF_STAIRS|BLF_HALF) &&
					gBoxData[boxIndex+gBoxSizeYZ].type != BLOCK_LEAVES )
				{
					// neighbor's a whole block, so shove the vine onto it
					gBoxData[boxIndex+gBoxSizeYZ].flatFlags |= FLAT_FACE_LO_X;
				}
				else
				{
					// force the block to become a vine - could be weird if there was something else here.
					//gBoxData[boxIndex+gBoxSizeYZ].type = BLOCK_VINES;
					//gBoxData[boxIndex+gBoxSizeYZ].data = 0x0;
					return 0;
				}
			}
		}
		break;

    default:
        // something needs to be added to the cases above!
        assert(0);
        return 0;

        // TODO: we *could* mark doors like this, checking if the door is next to a wall.
        // But, that's pretty rare, I think: doors are usually closed and in air, so we
        // always render them as blocks (user can always delete doors if they don't like it).
        // If we *did* flatten doors, we'd have to return a code saying the door did get flattened
        // and should be removed.
    }

    return 1;
}

static int saveBillboard( int boxIndex, int type )
{
    switch ( type )
    {
    case BLOCK_SAPLING:
    case BLOCK_COBWEB:
    case BLOCK_DANDELION:
    case BLOCK_ROSE:
    case BLOCK_RED_MUSHROOM:
    case BLOCK_BROWN_MUSHROOM:
    case BLOCK_TALL_GRASS:
    case BLOCK_DEAD_BUSH:
    case BLOCK_SUGAR_CANE:
    case BLOCK_PUMPKIN_STEM:
    case BLOCK_MELON_STEM:
        return saveBillboardFaces( boxIndex, type, BB_FULL_CROSS );
        break;
    case BLOCK_CROPS:
    case BLOCK_NETHER_WART:
        return saveBillboardFaces( boxIndex, type, BB_GRID );
        break;
    case BLOCK_TORCH:
    case BLOCK_REDSTONE_TORCH_OFF:
    case BLOCK_REDSTONE_TORCH_ON:
        return saveBillboardFaces( boxIndex, type, BB_TORCH );
        break;
    case BLOCK_RAIL:
    case BLOCK_POWERED_RAIL:
    case BLOCK_DETECTOR_RAIL:
        return saveBillboardFaces( boxIndex, type, BB_RAILS );
        break;
    case BLOCK_FIRE:
        return saveBillboardFaces( boxIndex, type, BB_FIRE );
        break;
    }

    return 0;
}

// What we need to do here:
// 1) add two faces for given angle
// 2) save the vertex locations once for both faces
// 3) for each face, set the loop, the vertex indices, the normal indices (really, just face direction), and the texture indices
static int saveBillboardFaces( int boxIndex, int type, int billboardType )
{
    int i, j, fc, swatchLoc, startUvIndex;
    FaceRecord *face;
    int faceDir[8];
    Point vertexOffsets[4][4];
    IPoint anchor;
    int faceCount = 0;
    int startVertexCount = 0;   // doesn't really need initialization, but makes compilers happy
    int doubleSided = 1;
    int dataVal = gBoxData[boxIndex].data;

    swatchLoc = SWATCH_INDEX( gBlockDefinitions[type].txrX, gBlockDefinitions[type].txrY );

    // some types use data values for which billboard to use
    switch ( type )
    {
    case BLOCK_SAPLING:
        switch ( dataVal & 0x3 )
        {
        case 0:
        default:
            // set OK already
            break;
        case 1:
            swatchLoc = SWATCH_INDEX(15,4);
            // spruce
            break;
        case 2:
            // birch
            swatchLoc = SWATCH_INDEX(15,3);
            break;
        case 3:
        case 4:
            // jungle (? - icon not shown properly at http://www.minecraftwiki.net/wiki/Block_ids#Beta_1.5_and_after)
            if ( gJungleExists )
            {
                swatchLoc = SWATCH_INDEX(14,1);
            }
            break;
        }
        break;
    case BLOCK_TALL_GRASS:
        switch ( dataVal )
        {
        case 0:
            // dead bush appearance
            swatchLoc = SWATCH_INDEX(7,3);
            break;
        case 1:
        default:
            // set OK already
            break;
        case 2:
            // fern
            swatchLoc = SWATCH_INDEX(8,3);
            break;
        }
        break;
    case BLOCK_TORCH:
    case BLOCK_REDSTONE_REPEATER_OFF:
    case BLOCK_REDSTONE_REPEATER_ON:
        // is torch not standing up?
        if ( dataVal != 0x5 )
        {
            // it'll get flattened instead
            return 0;
        }
        break;
    case BLOCK_CROPS:
        // adjust for growth
        swatchLoc += ( dataVal - 7 );
        break;
    case BLOCK_NETHER_WART:
        if ( dataVal == 0 )
        {
            swatchLoc -= 2;
        }
        else if ( dataVal <= 2 )
        {
            swatchLoc--;
        }
        break;
    case BLOCK_PUMPKIN_STEM:
    case BLOCK_MELON_STEM:
        // TODO: could offset about height of stem
        break;
    case BLOCK_POWERED_RAIL:
        if ( !(dataVal & 0x8) )
        {
            // unpowered rail
            swatchLoc = SWATCH_INDEX( 3, 10 );
        }
        // fall through:
    case BLOCK_DETECTOR_RAIL:
        // if not a normal rail, there are no curve bits, so mask off upper bit, which is
        // whether the rail is powered or not.
        dataVal &= 0x7;
        // fall through:
    case BLOCK_RAIL:
        switch ( dataVal )
        {
        case 2:
        case 3:
        case 4:
        case 5:
            // sloping, so continue
            break;
        default:
            // it's a flat
            return(0);
        }
        break;
    }

    // given billboardType, set up number of faces, normals for each, coordinate offsets for each;
    // the UV indices should always be the same for each, so we don't need to touch those
    switch ( billboardType )
    {
    case BB_FULL_CROSS:
        {
            float texelWidth,texelLow,texelHigh;

            // approximate width of billboard across block, eyeballing it.
            texelWidth = 14.5f/16.0f;
            texelLow = (1.0f - texelWidth)/2.0f;
            texelHigh = (1.0f + texelWidth)/2.0f;
            faceCount = 4;
            // two paired billboards
            faceDir[0] = DIRECTION_LO_X_HI_Z;
            faceDir[1] = DIRECTION_HI_X_LO_Z;
            faceDir[2] = DIRECTION_HI_X_HI_Z;
            faceDir[3] = DIRECTION_LO_X_LO_Z;

            Vec3Scalar( vertexOffsets[0][0], =, texelLow,0,texelLow );
            Vec3Scalar( vertexOffsets[0][1], =, texelHigh,0,texelHigh );
            Vec3Scalar( vertexOffsets[0][2], =, texelHigh,1,texelHigh );
            Vec3Scalar( vertexOffsets[0][3], =, texelLow,1,texelLow );

            Vec3Scalar( vertexOffsets[1][0], =, texelLow,0,texelHigh );
            Vec3Scalar( vertexOffsets[1][1], =, texelHigh,0,texelLow );
            Vec3Scalar( vertexOffsets[1][2], =, texelHigh,1,texelLow );
            Vec3Scalar( vertexOffsets[1][3], =, texelLow,1,texelHigh );
        }
        break;
    case BB_GRID:
    case BB_FIRE:
        {
            float texelWidth,texelLow,texelHigh;

            // width is space between parallel billboards
            // Fire goes to the edge of the block.
            texelWidth = (billboardType == BB_FIRE) ? 1.0f : 0.5f;
            texelLow = (1.0f - texelWidth)/2.0f;
            texelHigh = (1.0f + texelWidth)/2.0f;
            faceCount = 8;  // really, 4, single-sided, for torches
            // two paired billboards
            faceDir[0] = DIRECTION_BLOCK_SIDE_LO_X;
            faceDir[1] = DIRECTION_BLOCK_SIDE_HI_X;
            faceDir[2] = DIRECTION_BLOCK_SIDE_LO_X;
            faceDir[3] = DIRECTION_BLOCK_SIDE_HI_X;
            faceDir[4] = DIRECTION_BLOCK_SIDE_HI_Z;
            faceDir[5] = DIRECTION_BLOCK_SIDE_LO_Z;
            faceDir[6] = DIRECTION_BLOCK_SIDE_HI_Z;
            faceDir[7] = DIRECTION_BLOCK_SIDE_LO_Z;

            Vec3Scalar( vertexOffsets[0][0], =, texelLow,0,0 );
            Vec3Scalar( vertexOffsets[0][1], =, texelLow,0,1 );
            Vec3Scalar( vertexOffsets[0][2], =, texelLow,1,1 );
            Vec3Scalar( vertexOffsets[0][3], =, texelLow,1,0 );

            Vec3Scalar( vertexOffsets[1][0], =, texelHigh,0,0 );
            Vec3Scalar( vertexOffsets[1][1], =, texelHigh,0,1 );
            Vec3Scalar( vertexOffsets[1][2], =, texelHigh,1,1 );
            Vec3Scalar( vertexOffsets[1][3], =, texelHigh,1,0 );

            Vec3Scalar( vertexOffsets[2][0], =, 0,0,texelLow );
            Vec3Scalar( vertexOffsets[2][1], =, 1,0,texelLow );
            Vec3Scalar( vertexOffsets[2][2], =, 1,1,texelLow );
            Vec3Scalar( vertexOffsets[2][3], =, 0,1,texelLow );

            Vec3Scalar( vertexOffsets[3][0], =, 0,0,texelHigh );
            Vec3Scalar( vertexOffsets[3][1], =, 1,0,texelHigh );
            Vec3Scalar( vertexOffsets[3][2], =, 1,1,texelHigh );
            Vec3Scalar( vertexOffsets[3][3], =, 0,1,texelHigh );
        }
        break;
    case BB_TORCH:
        // could use this for cactus, like a torch, but you also have to add the top and bottom. Too much work for me...
        {
            float texelWidth,texelLow,texelHigh;

            // width is space between parallel billboards
            // Annoyingly, it all depends on how you interpolate your textures. Torches will show
            // gaps if you use bilinear interpolation, but the size of the gap depends on the texture resolution;
            // higher is narrower. So compromise by pulling in the width a bit more. Since torches are not beautiful
            // anyway, I'm fine with this.
            texelWidth = 0.125f;
            // This version gives a bigger fringe, but I don't think it's really needed
            //texelWidth = (float)(gModel.tileSize/8 - 1)/(float)gModel.tileSize;
            // torches are single-sided, all pointing out (note torch has no tip currently)
            doubleSided = 0;
            texelLow = (1.0f - texelWidth)/2.0f;
            texelHigh = (1.0f + texelWidth)/2.0f;
            faceCount = 8;  // really, 4, single-sided, for torches
            // two paired billboards
            faceDir[0] = DIRECTION_BLOCK_SIDE_LO_X;
            faceDir[1] = DIRECTION_BLOCK_SIDE_LO_X; // lame doubling of unused data
            faceDir[2] = DIRECTION_BLOCK_SIDE_HI_X;
            faceDir[3] = DIRECTION_BLOCK_SIDE_HI_X;
            faceDir[4] = DIRECTION_BLOCK_SIDE_LO_Z;
            faceDir[5] = DIRECTION_BLOCK_SIDE_LO_Z;
            faceDir[6] = DIRECTION_BLOCK_SIDE_HI_Z;
            faceDir[7] = DIRECTION_BLOCK_SIDE_HI_Z;

            Vec3Scalar( vertexOffsets[0][0], =, texelLow,0,0 );
            Vec3Scalar( vertexOffsets[0][1], =, texelLow,0,1 );
            Vec3Scalar( vertexOffsets[0][2], =, texelLow,1,1 );
            Vec3Scalar( vertexOffsets[0][3], =, texelLow,1,0 );

            Vec3Scalar( vertexOffsets[1][0], =, texelHigh,0,1 );
            Vec3Scalar( vertexOffsets[1][1], =, texelHigh,0,0 );
            Vec3Scalar( vertexOffsets[1][2], =, texelHigh,1,0 );
            Vec3Scalar( vertexOffsets[1][3], =, texelHigh,1,1 );

            Vec3Scalar( vertexOffsets[2][0], =, 1,0,texelLow );
            Vec3Scalar( vertexOffsets[2][1], =, 0,0,texelLow );
            Vec3Scalar( vertexOffsets[2][2], =, 0,1,texelLow );
            Vec3Scalar( vertexOffsets[2][3], =, 1,1,texelLow );

            Vec3Scalar( vertexOffsets[3][0], =, 0,0,texelHigh );
            Vec3Scalar( vertexOffsets[3][1], =, 1,0,texelHigh );
            Vec3Scalar( vertexOffsets[3][2], =, 1,1,texelHigh );
            Vec3Scalar( vertexOffsets[3][3], =, 0,1,texelHigh );
        }
        break;
    case BB_RAILS:
        {
            float texelWidth,texelLow,texelHigh;

            // approximate width of billboard across block, eyeballing it.
            texelWidth = 1.0f;
            texelLow = (1.0f - texelWidth)/2.0f;
            texelHigh = (1.0f + texelWidth)/2.0f;
            faceCount = 2;

            switch ( dataVal )
            {
            case 2: // ascend east +x
                // two paired billboards
                faceDir[0] = DIRECTION_LO_X_HI_Y;
                faceDir[1] = DIRECTION_HI_X_LO_Y;

                Vec3Scalar( vertexOffsets[0][0], =, texelHigh,texelHigh,1 );
                Vec3Scalar( vertexOffsets[0][1], =, texelHigh,texelHigh,0 );
                Vec3Scalar( vertexOffsets[0][2], =, texelLow,texelLow,0 );
                Vec3Scalar( vertexOffsets[0][3], =, texelLow,texelLow,1 );
                break;
            case 3: // ascend west -x
                faceDir[0] = DIRECTION_HI_X_HI_Y;
                faceDir[1] = DIRECTION_LO_X_LO_Y;

                Vec3Scalar( vertexOffsets[0][0], =, texelLow,texelHigh,0 );
                Vec3Scalar( vertexOffsets[0][1], =, texelLow,texelHigh,1 );
                Vec3Scalar( vertexOffsets[0][2], =, texelHigh,texelLow,1 );
                Vec3Scalar( vertexOffsets[0][3], =, texelHigh,texelLow,0 );
                break;
            case 4: // ascend north -z
                faceDir[0] = DIRECTION_HI_Z_HI_Y;
                faceDir[1] = DIRECTION_LO_Z_LO_Y;

                Vec3Scalar( vertexOffsets[0][0], =, 0,texelLow,texelHigh );
                Vec3Scalar( vertexOffsets[0][1], =, 1,texelLow,texelHigh );
                Vec3Scalar( vertexOffsets[0][2], =, 1,texelHigh,texelLow );
                Vec3Scalar( vertexOffsets[0][3], =, 0,texelHigh,texelLow );
                break;
            case 5: // ascend south +z
                faceDir[0] = DIRECTION_LO_Z_HI_Y;
                faceDir[1] = DIRECTION_HI_Z_LO_Y;

                Vec3Scalar( vertexOffsets[0][0], =, 0,texelHigh,texelHigh );
                Vec3Scalar( vertexOffsets[0][1], =, 1,texelHigh,texelHigh );
                Vec3Scalar( vertexOffsets[0][2], =, 1,texelLow,texelLow );
                Vec3Scalar( vertexOffsets[0][3], =, 0,texelLow,texelLow );
                break;
            default:
                // it's a flat
                return(0);
            }
        }
        break;
    default:
        assert(0);
        return 0;
    }


    // box value
    boxIndexToLoc( anchor, boxIndex );

    // flag UVs as being used for this swatch, so that the actual four UV values
    // are saved.
    saveTextureUVs( swatchLoc, type );

    // get the four UV texture vertices, based on type of block
    startUvIndex = 4*gModel.uvSwatchToIndex[swatchLoc];
    assert(startUvIndex>=0);

    for ( i = 0; i < faceCount; i++ )
    {
        // torches are 4 sides facing out: don't output 8 sides
        if ( doubleSided || (i % 2 == 0))
        {
            face = (FaceRecord *)malloc(sizeof(FaceRecord));

            // if  we sort, we want to keep faces in the order generated, which is
            // generally cache-coherent (and also just easier to view in the file)
            face->faceIndex = gModel.faceCount;
            face->type = type;

            // always the same normal, which directly corresponds to the normals[] array in gModel
            face->normalIndex = faceDir[i];

            // two faces have the same vertices and uv's, just the normal is reversed.
            fc = i/2;

            // first or second half of billboard (we assume polygons are one-sided)
            if ( i % 2 == 0 )
            {
                // first half of billboard
                startVertexCount = gModel.vertexCount;

                // get four face indices for the four corners of the billboard, and always create each
                for ( j = 0; j < 4; j++ )
                {
                    // get vertex location and store location
                    float *pt;
                    checkVertexListSize();

                    pt = (float *)gModel.vertices[gModel.vertexCount];

                    pt[X] = (float)(anchor[X] + vertexOffsets[fc][j][X]);
                    pt[Y] = (float)(anchor[Y] + vertexOffsets[fc][j][Y]);
                    pt[Z] = (float)(anchor[Z] + vertexOffsets[fc][j][Z]);

                    face->vertexIndex[j] = startVertexCount + j;
                    face->uvIndex[j] = startUvIndex + j;

                    gModel.vertexCount++;
                    assert( gModel.vertexCount <= gModel.vertexListSize );
                }
            }
            else
            {
                // second half of billboard
                // vertices already created by code above, for previous billboard
                // reverse both loops

                // use startVertexCount for the locations of the four vertices;
                // these are always the same
                for ( j = 0; j < 4; j++ )
                {
                    face->vertexIndex[3-j] = startVertexCount + j;
                    face->uvIndex[3-j] = startUvIndex + j;
                }
            }

            // all set, so save it away
            checkFaceListSize();
            gModel.faceList[gModel.faceCount++] = face;
        }
    }

    gModel.billboardCount++;

    addBounds( anchor, &gModel.billboardBounds );
    VecScalar( anchor, +=, 1 );
    addBounds( anchor, &gModel.billboardBounds );

    return 1;
}

static void checkGroupListSize()
{
    // there's some play with the group count, e.g. group 0 is not used, so
    // resize should happen a bit earlier. 3 for safety.
    if (gGroupCount == gGroupListSize)
    {
        BoxGroup *groups;
        gGroupListSize *= 2;
        groups = (BoxGroup*)malloc(gGroupListSize*sizeof(BoxGroup));
        memcpy( groups, gGroupList, gGroupCount*sizeof(BoxGroup));
        free( gGroupList );
        gGroupList = groups;
    }
}
static void checkVertexListSize()
{
    if (gModel.vertexCount == gModel.vertexListSize)
    {
        Point *vertices;
        gModel.vertexListSize *= 2;
        vertices = (Point*)malloc(gModel.vertexListSize*sizeof(Point));
        memcpy( vertices, gModel.vertices, gModel.vertexCount*sizeof(Point));
        free( gModel.vertices );
        gModel.vertices = vertices;
    }
}
static void checkFaceListSize()
{
    if (gModel.faceCount == gModel.faceSize)
    {
        FaceRecord **faceList;
        gModel.faceSize *= 2;
        faceList = (FaceRecord**)malloc(gModel.faceSize*sizeof(FaceRecord*));
        memcpy( faceList, gModel.faceList, gModel.faceCount*sizeof(FaceRecord*));
        free( gModel.faceList );
        gModel.faceList = faceList;
    }
}

static void findGroups()
{
    int boxIndex;
    IPoint loc;
    IPoint seedLoc;
    IPoint *seedStack = (IPoint *)malloc(gBoxSizeXYZ*sizeof(IPoint));
    int seedCount = 0;
    BoxGroup *pGroup;

    gGroupList[SURROUND_AIR_GROUP].population = 0;

    // check if we're sealing off tunnels. If so, all we need to do is add the sides and bottom to the air group,
    // the first real group. The "top of the box" air is then used as the seed area, and the rest of the first
    // "outside" air group is finished filling in, as normal. In this way the sides and bottom are "sealed", the
    // air touching the sides and bottom will be put in their own groups (if they don't attach to the outside air
    // group via some tunnel) and will get filled in later as bubbles. You with me?
    // TODO: note in documentation that sealing the side tunnels actually fills them in only if filling air bubbles is done.
    // This process below simply makes sure that the air groups are separate from the outside air group, not that they're filled.
    if ( gOptions->exportFlags & EXPT_SEAL_SIDE_TUNNELS )
    {
        // Note that we actually seal the originally-specified box, so that we don't seal the shrink-wrapped area around the box.
        // -Y face (i.e. bottom)
        addVolumeToGroup( SURROUND_AIR_GROUP, 0, 0, 0, gBoxSize[X]-1, 0, gBoxSize[Z]-1 );
        // -Z face - vary X & Y; note we don't do bottom, since it's already done by -Y
        addVolumeToGroup( SURROUND_AIR_GROUP, 0, 1, 0, gBoxSize[X]-1, gBoxSize[Y]-1, 0 );
        // +Z face
        addVolumeToGroup( SURROUND_AIR_GROUP, 0, 1, gBoxSize[Z]-1, gBoxSize[X]-1, gBoxSize[Y]-1, gBoxSize[Z]-1 );
        // -X face - vary Y & Z; note we don't do columns at corners, since it's already done by -Z and +Z
        addVolumeToGroup( SURROUND_AIR_GROUP, 0, 1, 1,  0, gBoxSize[Y]-1, gBoxSize[Z]-2 );
        // +X face
        addVolumeToGroup( SURROUND_AIR_GROUP, gBoxSize[X]-1, 1, 1, gBoxSize[X]-1, gBoxSize[Y]-1, gBoxSize[Z]-2 );
    }

    // We test *all* cells, including surrounding air, for connectivity to others. The idea here is to have the surrounding air group
    // be created first, and find all connecting air. Note that the tunnel sealing, above, will stop this percolation of air along the sides and
    // bottom, as those cells will already have a group.
    for ( loc[X] = gAirBox.min[X]; loc[X] <= gAirBox.max[X]; loc[X]++ )
    {
        for ( loc[Z] = gAirBox.min[Z]; loc[Z] <= gAirBox.max[Z]; loc[Z]++ )
        {
            // IMPORTANT: Note that we start at the top and work down, as we want to ensure that outside air is the top group.
            boxIndex = BOX_INDEX(loc[X],gAirBox.max[Y],loc[Z]);
            for ( loc[Y] = gAirBox.max[Y]; loc[Y] >= gAirBox.min[Y]; loc[Y]--, boxIndex-- )
            {
                // check if the object has no group
                if ( gBoxData[boxIndex].group == NO_GROUP_SET )
                {
                    gGroupCount++;
                    checkGroupListSize();
                    // no group, so make a new seed.
                    // Note that group index 0 is not used at all.
                    pGroup = &gGroupList[gGroupCount];
                    pGroup->groupID = gGroupCount;
                    pGroup->population = (gGroupCount == SURROUND_AIR_GROUP) ? pGroup->population+1 : 1;   // the solid air group might already exist with a population
                    // the solid air group will need to have its bounds fixed at the end if tunnel sealing is going on
                    Vec2Op( pGroup->bounds.min, =, loc );
                    Vec2Op( pGroup->bounds.max, =, loc );
                    pGroup->solid = (gBoxData[boxIndex].type > BLOCK_AIR);

                    gBoxData[boxIndex].group = gGroupCount;
                    if ( pGroup->solid )
                        gSolidGroups++;
                    else
                        gAirGroups++;
                    // propagate seed: make neighbors with same type (solid or air) and no group to be this group.
                    propagateSeed(loc,pGroup,seedStack,&seedCount);

                    // When this is done, seedStack has a list of seeds that had no ID before and now have one.
                    // Each of these seeds' neighbors needs to be tested .

                    // while seedCount > 0, propagate
                    while ( seedCount > 0 )
                    {
                        // copy test point over, so we don't trample it when seedStack gets increased
                        seedCount--;
                        Vec2Op( seedLoc, =, seedStack[seedCount]);
                        propagateSeed(seedLoc,pGroup,seedStack,&seedCount);
                    }
                }
            }
        }
    }

    if ( gOptions->exportFlags & EXPT_SEAL_SIDE_TUNNELS )
    {
        // fix solid group, make sure its bounds are the world
        addBoundsToBounds( gAirBox, &gGroupList[SURROUND_AIR_GROUP].bounds );
    }

    free(seedStack);
}


// Add a volume of space to a group. Group is assumed to exist, have solidity assigned, blocks are assumed to not be in a group already.
static void addVolumeToGroup( int groupID, int minx, int miny, int minz, int maxx, int maxy, int maxz )
{
    int boxIndex;
    IPoint loc;
    BoxGroup *pGroup = &gGroupList[groupID];
    // actually meaningless for our particular use of this method right now,
    // as (a) the bounds are set when the group is created (which is later),
    // and (b) the group's bounds at this point are all zeros (which is OK in this case,
    // but normally is not what bounds should have).
    //IBox bounds;
    //Vec3Scalar( bounds.min, =, minx, miny, minz );
    //Vec3Scalar( bounds.max, =, maxx, maxy, maxz ); 
    //addBoundsToBounds( bounds, &pGroup->bounds );

    for ( loc[X] = minx; loc[X] <= maxx; loc[X]++ )
    {
        for ( loc[Z] = minz; loc[Z] <= maxz; loc[Z]++ )
        {
            boxIndex = BOX_INDEX(loc[X],miny,loc[Z]);
            // Note that we start at the top and work down, as we want to ensure that outside air is the top group.
            for ( loc[Y] = miny; loc[Y] <= maxy; loc[Y]++, boxIndex++ )
            {
                assert(gBoxData[boxIndex].group == NO_GROUP_SET);

                pGroup->population++;   // the solid air group might already exist with a population
                gBoxData[boxIndex].group = groupID;
            }
        }
    }
}


static void propagateSeed(IPoint point, BoxGroup *pGroup, IPoint *seedStack, int *seedCount)
{
    int faceDirection;
    int boxIndex,newBoxIndex;
    IPoint newPt;
    int sindex;

    // special test: if the seed is air, and the original type of this block was solid, not air (was: was an entrance), then
    // don't propagate the seed. This is useful for structures, where you don't necessarily want to see
    // the ladder or whatever that closes the building off from the world but you do want the ladder
    // to stop propagation. Note that this means two air groups can be next to each other, i.e., neighbors,
    // so we need to check for this case elsewhere.
    if ( (gOptions->exportFlags & EXPT_SEAL_ENTRANCES) && !pGroup->solid )
    {
        boxIndex = BOX_INDEXV(point);
        if ( gBlockDefinitions[gBoxData[boxIndex].origType].flags & BLF_ENTRANCE )
		// In this way, you can use things like snow blocks set to display an alpha of 0 to seal off entrances,
		// and the hole will be visible at the end.  TODO: document - removed, too obscure!!!
        //if ( gBoxData[boxIndex].origType > BLOCK_AIR )
        {
            // This air block was actually something (like a ladder) that got culled out early on. Use it to seal the entrance.
            // Old code: This air block is actually an entrance, so don't propagate it further.
            return;
        }
    }

    // check the six neighbors for if they're not in a group already and have the same type, filled or unfilled
    for ( faceDirection = 0; faceDirection < 6; faceDirection++ )
    {
        // check that new location is valid for testing
        Vec2Op( newPt, =, point );
        if ( getNeighbor( faceDirection, newPt ) )
        {
            newBoxIndex = BOX_INDEXV(newPt);
            // is neighbor not in a group, and the same sort of thing as our seed (solid or not)?
            if ( (gBoxData[newBoxIndex].group == NO_GROUP_SET) &&
                ((gBoxData[newBoxIndex].type > BLOCK_AIR) == pGroup->solid) ) {

                // note the block is a part of this group now
                gBoxData[newBoxIndex].group = pGroup->groupID;
                // update the group's population, and check if this one touches a side.
                pGroup->population++;
                addBounds(newPt,&pGroup->bounds);

                // and add it to the list of potential seeds to continue to propagate
                sindex = *seedCount;
                Vec2Op(seedStack[sindex], =, newPt);
                (*seedCount)++;
                // it's possible that we could run out, I think: each seed could add its six neighbors, etc.?
                assert((*seedCount)<gBoxSizeXYZ);
            }
        }
    }
}

// return 1 if there's a valid neighbor, which is put in newx, etc.
// NOTE: it's important that "point" be copied over here, not as a point. It is changed locally.
static int getNeighbor( int faceDirection, IPoint newPoint )
{
    switch (faceDirection)
    {
    case DIRECTION_BLOCK_SIDE_LO_X: // -X
        newPoint[X]--;
        if ( newPoint[X] < gAirBox.min[X] ) return 0;
        break;
    case DIRECTION_BLOCK_BOTTOM: // -Y
        newPoint[Y]--;
        if ( newPoint[Y] < gAirBox.min[Y] ) return 0;
        break;
    case DIRECTION_BLOCK_SIDE_LO_Z: // -Z
        newPoint[Z]--;
        if ( newPoint[Z] < gAirBox.min[Z] ) return 0;
        break;
    case DIRECTION_BLOCK_SIDE_HI_X: // +X
        newPoint[X]++;
        if ( newPoint[X] > gAirBox.max[X] ) return 0;
        break;
    case DIRECTION_BLOCK_TOP: // +Y
        newPoint[Y]++;
        if ( newPoint[Y] > gAirBox.max[Y] ) return 0;
        break;
    case DIRECTION_BLOCK_SIDE_HI_Z: // +Z
        newPoint[Z]++;
        if ( newPoint[Z] > gAirBox.max[Z] ) return 0;
        break;
    }
    return 1;
}

// return 1 if there's a valid neighbor, which is put in newx, etc.
// Use only if you know that the block is not on a face
// NOTE: it's important that "point" be copied over here, passed directly and not as a pointer. It is changed locally.
static void getNeighborUnsafe( int faceDirection, IPoint newPoint )
{
    switch (faceDirection)
    {
    case DIRECTION_BLOCK_SIDE_LO_X: // -X
        newPoint[X]--;
        break;
    case DIRECTION_BLOCK_BOTTOM: // -Y
        newPoint[Y]--;
        break;
    case DIRECTION_BLOCK_SIDE_LO_Z: // -Z
        newPoint[Z]--;
        break;
    case DIRECTION_BLOCK_SIDE_HI_X: // +X
        newPoint[X]++;
        break;
    case DIRECTION_BLOCK_TOP: // +Y
        newPoint[Y]++;
        break;
    case DIRECTION_BLOCK_SIDE_HI_Z: // +Z
        newPoint[Z]++;
        break;
    }
}

static void checkAndRemoveBubbles()
{
    int i, groupID, maxPop, masterGroupID;
    IBox bounds;

    int *neighborGroups = (int *)malloc((gGroupCount+1)*sizeof(int));

    // if we are simply merging groups, then air bubbles are left as air and merely act to merge groups (I hope...)
    int fillType = (gOptions->exportFlags & EXPT_FILL_BUBBLES) ? BLOCK_GLASS : BLOCK_AIR;

    // only the very first group, which is air, will not be a bubble. All other air groups will be.
    for ( i = SURROUND_AIR_GROUP+1; i <= gGroupCount; i++ )
    {
        // is this group air?
        if ( !gGroupList[i].solid )
        {
            // a bubble is found, so merge it, along with all solid groups that border it, into one solid group.
            memset(neighborGroups,0,(gGroupCount+1)*sizeof(int));

            // the first group should always be the outside air group, the second should be solid. Just in case,
            // see if this assumption is wrong. Really, we could probably change i = 2 as the starting condition.
            assert(i>=2);

            // Find which solid groups are neighbors of this group i.
            // Search the given bounds.
            neighborGroups[gGroupList[i].groupID] = 1;
            findNeighboringGroups( &gGroupList[i].bounds, gGroupList[i].groupID, neighborGroups );

            // find the highest population solid group on the list: this will be the one we merge to.
            // Turn this group off in this list.
            maxPop = 0;
            masterGroupID = -1;
            // we loop through all groups here, as initial air group should get turned off from list
            for ( groupID = SURROUND_AIR_GROUP; groupID <= gGroupCount; groupID++ )
            {
                // check if the group is a neighbor
                if ( neighborGroups[groupID] > 0 )
                {
                    // neighboring group to air *could* also be air (and the original
                    // group itself is air); check it's solid
                    if ( gGroupList[groupID].solid)
                    {
                        // solid: find if it's larger than any previously-examined solid group
                        if ( gGroupList[groupID].population > maxPop )
                        {
                            assert(gGroupList[groupID].solid == 1);
                            maxPop = gGroupList[groupID].population;
                            masterGroupID = groupID;
                        }
                        // at the same time, decrement count by how many groups will get merged
                        // Really, at this point, all neighborGroups should be solid, but someday
                        // I want to allow air groups to border each other, and for bubbles to
                        // get formed inside of buildings, etc.
                        gStats.solidGroupsMerged++;
                        gSolidGroups--;
                        assert(gSolidGroups >= 0);
                    }
                    else
                    {
                        // since the neighbor is air, simply don't fill it, clear it from the list
                        neighborGroups[groupID] = 0;
                    }
                }
            }
            // add one back in for the master solid group that won't get merged
            gStats.solidGroupsMerged--;
            gSolidGroups++;
            assert(gSolidGroups >= 1);

            // it can be the case that there *are* no solid neighbors next to the bubble: for example,
            // six torches can surround a single bubble of air. In this case, just make the bubble solid
            // and as other bubbles are processed they'll merge with this solid (or not).
            if ( masterGroupID >= 0 )
            {
               // A solid group was found next to the bubble

                // Take the biggest group out of the list, as it's the one everyone merges to.
                neighborGroups[masterGroupID] = 0;
                // Get the union of the bounds of all groups found to merge.
                VecScalar( bounds.min, =,  999999);
                VecScalar( bounds.max, =, -999999);
                // Turn the air bubble itself back on, as it will also be filled
                neighborGroups[i] = 1;
                for ( groupID = SURROUND_AIR_GROUP+1; groupID <= gGroupCount; groupID++ )
                {
                    if ( neighborGroups[groupID] > 0 )
                    {
                        addBoundsToBounds( gGroupList[groupID].bounds, &bounds );
                    }
                }
                assert( bounds.max[Y] >= bounds.min[Y]);

                // Go through this sub-box and change all groups tagged to the new fill group.
                // We can fill air with whatever we want, since it won't be visible.
                fillGroups( &bounds, masterGroupID, 1, fillType, neighborGroups );
                // delete the air group from the count
                gStats.bubblesFound++;
                gAirGroups--;
                assert(gAirGroups>=1);
            }
            else
            {
                // no solid group found, so make only the bubble itself solid

                // Get the union of the bounds of all groups found to merge.
                bounds = gGroupList[i].bounds;
                // Turn the air bubble itself on, as it will be (the only thing) filled
                neighborGroups[i] = 1;

                // Go through this sub-box and change all groups tagged to the new fill group.
                // We can fill air with whatever we want, since it won't be visible.
                fillGroups( &bounds, i, 1, fillType, neighborGroups );

                // delete the air group from the count
                gStats.bubblesFound++;
                gAirGroups--;
                assert(gAirGroups>=1);
            }
        }
    }
    free(neighborGroups);
}

// go through box. Whichever blocks have same group, mark all neighbors into neighborGroups array.
// Note that the groupID neighborGroup entry is likely to be set, also.
static void findNeighboringGroups( IBox *bounds, int groupID, int *neighborGroups )
{
    int x,y,z;
    int boxIndex, faceDirection;

    assert(bounds->min[X]>0);
    assert(bounds->min[Y]>0);
    assert(bounds->min[Z]>0);
    assert(bounds->max[X]<gBoxSize[X]-1);
    assert(bounds->max[Y]<gBoxSize[Y]-1);
    assert(bounds->max[Z]<gBoxSize[Z]-1);

    for ( x = bounds->min[X]; x <= bounds->max[X]; x++ )
    {
        for ( z = bounds->min[Z]; z <= bounds->max[Z]; z++ )
        {
            boxIndex = BOX_INDEX(x,bounds->min[Y],z);
            for ( y = bounds->min[Y]; y <= bounds->max[Y]; y++, boxIndex++ )
            {
                if ( gBoxData[boxIndex].group == groupID )
                {
                    // mark the neighbors
                    for ( faceDirection = 0; faceDirection < 6; faceDirection++ )
                    {
                        // get the neighbor's box index, to get the neighbor box's group,
                        // and set this as a group that touches the group specified. Simply set them
                        // all, again and again, brute force.
                        // Note that we don't have to check if a neighbor block location is valid! They're all inside air.
                        neighborGroups[gBoxData[boxIndex + gFaceOffset[faceDirection]].group] = 1;
                    }
                }
            }
        }
    }
}


// set the bounds for all groups, from the box data's assigned groups.
//static void establishGroupBounds()
//{
//	int i, boxIndex, groupIndex;
//	IPoint loc;
//	// get the bounds for each group. This tells us whether each group touches the full box faces,
//	// and gives us a limited volume to search when a group is merged with another group.
//
//	// first initialize bounds, except group 1, surrounding air, which is known at this point.
//	Vec2Op( gGroupList[SURROUND_AIR_GROUP].bounds.min, =, gAirBox.min );
//	Vec2Op( gGroupList[SURROUND_AIR_GROUP].bounds.max, =, gAirBox.max );
//
//	for ( i = SURROUND_AIR_GROUP+1; i <= gGroupCount; i++ )
//	{
//		VecScalar( gGroupList[i].bounds.min, =,  999999);
//		VecScalar( gGroupList[i].bounds.max, =, -999999);
//	}
//
//	// look at all locations and set. We can not bother with the air coating (group 1), as that one's done anyways
//    for ( loc[X] = gSolidBox.min[X]; loc[X] <= gSolidBox.max[X]; loc[X]++ )
//    {
//        for ( loc[Z] = gSolidBox.min[Z]; loc[Z] <= gSolidBox.max[Z]; loc[Z]++ )
//        {
//            boxIndex = BOX_INDEX(loc[X],gSolidBox.min[Y],loc[Z]);
//            for ( loc[Y] = gSolidBox.min[Y]; loc[Y] <= gSolidBox.max[Y]; loc[Y]++, boxIndex++ )
//            {
//				// get the group of the block
//				groupIndex = gBoxData[boxIndex].group;
//				assert(groupIndex >= SURROUND_AIR_GROUP );
//				if ( groupIndex > SURROUND_AIR_GROUP )
//				{
//					// group is not the surrounding air group (which is always first and whose bounds are known),
//					// so update its bounds with the location.
//					addBounds( loc, &gGroupList[groupIndex].bounds );
//				}
//			}
//		}
//	}
//}


// fill the group given with whatever is specified, if the group is of different
// solidity than the master group. So, if the master group is solid, air blocks in
// the target list will be turned to fillType. If the master group is air, solid blocks
// in the target list will be turned to fillType (usually air).
// Brute force it - we could be more clever, but that's OK
static void fillGroups( IBox *bounds, int masterGroupID, int solid, int fillType, int *targetGroupIDs )
{
    int x,y,z;
    int boxIndex;

    // if solid, we can not bother with the fringe of the box
    IPoint loc;
    BoxGroup *pGroup;

    BoxGroup *pMasterGroup = &gGroupList[masterGroupID];

    for ( x = bounds->min[X]; x <= bounds->max[X]; x++ )
    {
        for ( z = bounds->min[Z]; z <= bounds->max[Z]; z++ )
        {
            boxIndex = BOX_INDEX(x,bounds->min[Y],z);
            for ( y = bounds->min[Y]; y <= bounds->max[Y]; y++, boxIndex++ )
            {
                // is this group one that should get filled by the master group?
                if ( targetGroupIDs[gBoxData[boxIndex].group] > 0 )
                {
                    // found one to fill, transfer it to master group
                    pGroup = &gGroupList[gBoxData[boxIndex].group];
                    if ( pGroup->solid != solid )
                    {
                        // target and master differ in solidity
                        // target is not solid, and master is solid, so needs to be made solid OR
                        // target is solid, and master is not solid, so need to be made air (fillType is
                        // assumed to be set correctly, to air.
                        // Note that we don't bother setting the "data" to anything;
                        // we assume the master fill type doesn't have a sub-field.

						// special test: if a location is surrounded by trees and leaves and air, fill in with a leaf
						if ( solid && fillType == BLOCK_GLASS )
						{
							int i;
							int leafFound = 0;
							int woodSearch = 1;
							unsigned char leafData = 0;
							for ( i = 0; i < 6 && woodSearch; i++ )
							{
								int index = boxIndex+gFaceOffset[i];
								// leaf found?
								if ( gBlockDefinitions[gBoxData[index].type].flags & BLF_LEAF_PART )
								{
									leafFound = 1;
									leafData = gBoxData[index].data;
								}
								else if ( !(gBlockDefinitions[gBoxData[index].type].flags & BLF_TREE_PART) && gBoxData[index].type != BLOCK_AIR )
								{
									// not a leaf, log, or air, so we won't fill it in.
									woodSearch = 0;
								}
							}
							if ( woodSearch && leafFound )
							{
								// leaf fill
								gBoxData[boxIndex].type = BLOCK_LEAVES;
								gBoxData[boxIndex].data = leafData;
							}
							else
							{
								// normal fill
								gBoxData[boxIndex].type = (unsigned char)fillType;
							}
						}
						else
						{
							gBoxData[boxIndex].type = (unsigned char)fillType;
						}
                        gBoxData[boxIndex].data = 0x0;
                    }
                    // transfer to master group
                    gBoxData[boxIndex].group = masterGroupID;

                    // note that this will make this group's bounds invalid,
                    // but since the group is going away, it doesn't matter
                    pGroup->population--;
                    pMasterGroup->population++;
                    Vec3Scalar( loc, =, x,y,z);
                    addBounds( loc, &pMasterGroup->bounds );
                }
            }
        }
    }
}

static void addBounds( IPoint loc, IBox *bounds )
{
    if ( loc[X] < bounds->min[X] )
        bounds->min[X] = loc[X];
    if ( loc[X] > bounds->max[X] )
        bounds->max[X] = loc[X];
    if ( loc[Y] < bounds->min[Y] )
        bounds->min[Y] = loc[Y];
    if ( loc[Y] > bounds->max[Y] )
        bounds->max[Y] = loc[Y];
    if ( loc[Z] < bounds->min[Z] )
        bounds->min[Z] = loc[Z];
    if ( loc[Z] > bounds->max[Z] )
        bounds->max[Z] = loc[Z];
}

// Add the first bounds to the seconds bounds
static void addBoundsToBounds( IBox inBounds, IBox *bounds )
{
    if ( inBounds.min[X] < bounds->min[X] )
        bounds->min[X] = inBounds.min[X];
    if ( inBounds.max[X] > bounds->max[X] )
        bounds->max[X] = inBounds.max[X];
    if ( inBounds.min[Y] < bounds->min[Y] )
        bounds->min[Y] = inBounds.min[Y];
    if ( inBounds.max[Y] > bounds->max[Y] )
        bounds->max[Y] = inBounds.max[Y];
    if ( inBounds.min[Z] < bounds->min[Z] )
        bounds->min[Z] = inBounds.min[Z];
    if ( inBounds.max[Z] > bounds->max[Z] )
        bounds->max[Z] = inBounds.max[Z];
}

static int connectCornerTips()
{
    int boxIndex;
    int x,y,z;
    int filledTip=0;

    // Find solid cells to test for tips to connect. Note we test in +Y order,
    // looking below at the previous Y plane for connectors. In this way we will
    // avoid interactions of new blocks getting added and causing some cascade
    // effect of connections with other tips now touched.

    // Note that we *always* add below for now. A more elaborate algorithm would
    // be to look at the 6 possible boxes to add to and use the one with the
    // largest obscurity factor. Too involved!
    for ( y = gSolidBox.min[Y]+1; y <= gSolidBox.max[Y]; y++ )
    {
        for ( x = gSolidBox.min[X]; x <= gSolidBox.max[X]; x++ )
        {
            for ( z = gSolidBox.min[Z]; z <= gSolidBox.max[Z]; z++ )
            {
                boxIndex = BOX_INDEX(x,y,z);

                // check if it's solid - if so, we'll check if the spot below is air
                if ( gBoxData[boxIndex].type > BLOCK_AIR )
                {
                    // quick out: if -Y cell is air, then continue checking, else we're done!
                    if ( gBoxData[boxIndex-1].type == BLOCK_AIR)
                    {
                        int hasCorner = checkForCorner(boxIndex,-1,-1);
                        if (!hasCorner)
                            hasCorner = checkForCorner(boxIndex,-1, 1);
                        if (!hasCorner)
                            hasCorner = checkForCorner(boxIndex, 1,-1);
                        if (!hasCorner)
                            hasCorner = checkForCorner(boxIndex, 1, 1);

                        // did we find at least one diagonal tip?
                        if ( hasCorner )
                        {
                            // add cell to group above
                            IPoint loc;
                            int airBoxIndex = boxIndex-1;
                            assert(gBoxData[airBoxIndex].type == BLOCK_AIR );
                            if ( gOptions->exportFlags & EXPT_DEBUG_SHOW_WELDS )
                            {
                                gBoxData[airBoxIndex].type = DEBUG_CORNER_TOUCH_TYPE;
                            }
                            else
                            {
                                // copy it over
                                gBoxData[airBoxIndex].type = gBoxData[boxIndex].type;
                                gBoxData[airBoxIndex].data = gBoxData[boxIndex].data;
                            }
                            gStats.blocksCornertipWelded++;

                            // we don't know which item on the group list is the air block's
                            // group, so can't easily subtract one from its population. But, we
                            // don't really care about the air group populations, ever.
                            gBoxData[airBoxIndex].group = gBoxData[boxIndex].group;
                            assert(gGroupList[gBoxData[boxIndex].group].solid);
                            gGroupList[gBoxData[boxIndex].group].population++;
                            Vec3Scalar( loc, =, x, y-1, z );
                            addBounds( loc, &gGroupList[gBoxData[boxIndex].group].bounds );

                            filledTip = 1;
                        }
                    }
                }
            }
        }
    }

    return filledTip;
}

static int checkForCorner(int boxIndex, int offx, int offz)
{
    int tipCornerIndex = boxIndex + offx*gBoxSizeYZ - 1 + offz*gBoxSize[Y];
    // check diagonally opposite corner in -Y direction to see if it's solid.
    // If so, check if the groups do not match (meaning they are disconnected parts, like
    // a balloon string).
    // If so, continue search, as these two could get joined.
    if ( (gBoxData[tipCornerIndex].type != BLOCK_AIR) &&
        (gBoxData[tipCornerIndex].group != gBoxData[boxIndex].group) )
    {
        // solid, so now check 2x2x2 to see if there are just two filled cells (which must be the original
        // and the diagonal ones).
        int i;
        // cleverness: by avoid index 0 (where x, y, and z are all zero) and index 1 (where they're all one)
        // we can entirely avoid testing the original and diagonal blocks!
        for ( i = 1; i < 7; i++ )
        {
            int x = (i>=4);
            int y = ((i%4)>=2);
            int z = i%2;

            if ( gBoxData[ boxIndex + x*offx*gBoxSizeYZ - y + z*offz*gBoxSize[Y] ].type != BLOCK_AIR )
                // one of the six is not air - return
                return 0;
        }
        return 1;
    }
    else
    {
        // air, or groups match,
        return 0;
    }
}

// return 1 if any fixes occurred.
static int fixTouchingEdges()
{
    // strategy: walk through solid box. Look at 6 of 12 edges if cell is solid:
    // does 2x2x1 have the right pattern of two diagonally-opposite solids?
    // If so, mark the two empty cells with a count and with flags pointing to the
    // neighboring cell.

    // Once all empty cells are marked like this, go through them and make records for sorting:
    // record has box index, count, and distance from center of model (computed in first pass,
    // as a by-product). Sort these records, first by count, then distance as a tie-breaker.
    // Fill in this list of empties: note that you have to decrement the count of the *grid box*
    // of the empty counts, since each edge getting repaired has +2 count, so both counts must
    // be subtracted. Some records will therefore have a count of zero when they're found on the
    // sorted list, as previous elements processed will have decremented their counts.
    //
    // When filling a location, do a merge of groups if needed. Choose material for new cell as
    // that of the +Y object first, then X and Z, then -Y last. Update group population and bounds.
    int boxIndex;
    int x,y,z;
    int touchCount,i;
    Point avgLoc,floc;
    int solidBlocks=0;
    TouchRecord *touchList;
    int maxVal;

    gTouchGrid = (TouchCell*)malloc(gBoxSizeXYZ*sizeof(TouchCell));
    memset(gTouchGrid,0,gBoxSizeXYZ*sizeof(TouchCell));

    gTouchSize = 0;

    VecScalar(avgLoc, =, 0.0f);

    // Find solid cells to test for edges to connect
    for ( x = gSolidBox.min[X]; x <= gSolidBox.max[X]; x++ )
    {
        for ( z = gSolidBox.min[Z]; z <= gSolidBox.max[Z]; z++ )
        {
            boxIndex = BOX_INDEX(x,gSolidBox.min[Y],z);
            for ( y = gSolidBox.min[Y]; y <= gSolidBox.max[Y]; y++, boxIndex++ )
            {
                // check if it's solid - if so, add to average center computations,
                // and then see if there are any edges that touch
                if ( gBoxData[boxIndex].type > BLOCK_AIR )
                {
                    Vec3Scalar( avgLoc, +=, x, y, z);
                    solidBlocks++;

                    // Test the following edges:
                    // +X face and the four edges there
                    // +Z+Y and +Z-Y
                    // This order guarantees us that we'll always count forward into grid,
                    // which I'm not sure is important (I once thought it was), i.e. that
                    // we will never examine cells for solidity that have already been touched.

                    // quick out: if +X cell is air, +X face edges are processed, else all can be ignored
                    if ( gBoxData[boxIndex+gBoxSizeYZ].type == BLOCK_AIR)
                    {
                        checkForTouchingEdge(boxIndex,1,-1, 0);
                        checkForTouchingEdge(boxIndex,1, 0,-1);
                        checkForTouchingEdge(boxIndex,1, 1, 0);
                        checkForTouchingEdge(boxIndex,1, 0, 1);
                    }
                    // quick out, if +Z cell is air, the two +Z face edges are processed, else all can be ignored
                    if ( gBoxData[boxIndex+gBoxSize[Y]].type == BLOCK_AIR)
                    {
                        checkForTouchingEdge(boxIndex,0,-1,1);
                        checkForTouchingEdge(boxIndex,0, 1,1);
                    }
                }
            }
        }
    }
    assert( solidBlocks );

    // were no touching edges found? Then return!
    if ( gTouchSize == 0 )
        return 0;

    // get average location
    VecScalar( avgLoc, /=, solidBlocks );

    // now find largest of three, X,Y,Z, and subtract it from Y center. We want to bias towards putting
    // blocks *under* other blocks, since we tend to look down on terrain.
    maxVal = ( gSolidBox.max[X]-gSolidBox.min[X] > gSolidBox.max[Z]-gSolidBox.min[Z] ) ? gSolidBox.max[X]-gSolidBox.min[X]+1 : gSolidBox.max[Z]-gSolidBox.min[Z]+1;
    maxVal = ( maxVal > gSolidBox.max[Y]-gSolidBox.min[Y] ) ? maxVal : gSolidBox.max[Y]-gSolidBox.min[Y]+1;
    avgLoc[Y] -= maxVal;

    // allocate space for touched air cells
    touchList = (TouchRecord *)malloc(gTouchSize*sizeof(TouchRecord));
    touchCount = 0;

    // go through the grid, collecting up the locations needing processing
    for ( x = gSolidBox.min[X]; x <= gSolidBox.max[X]; x++ )
    {
        for ( z = gSolidBox.min[Z]; z <= gSolidBox.max[Z]; z++ )
        {
            boxIndex = BOX_INDEX(x,gSolidBox.min[Y],z);
            for ( y = gSolidBox.min[Y]; y <= gSolidBox.max[Y]; y++, boxIndex++ )
            {
                // find potential fill locations and put them in a list
                if ( gTouchGrid[boxIndex].count > 0 )
                {
                    // here's a possible manifold-fill location, an air block that if
                    // we fill it in we will get rid of a manifold edge.
                    // So add a record.
                    touchList[touchCount].obscurity = gTouchGrid[boxIndex].obscurity;
                    touchList[touchCount].count = gTouchGrid[boxIndex].count;
                    touchList[touchCount].boxIndex = boxIndex;
                    assert(gBoxData[boxIndex].type == BLOCK_AIR );

                    Vec3Scalar(floc, = (float), x,y,z);
                    touchList[touchCount].distance = getDistanceSquared(floc, avgLoc);
                    touchCount++;
                }
            }
        }
    }

    assert( touchCount == gTouchSize );

    // we have the list of touched air cells that could be filled - sort it!
    qsort_s(touchList,touchCount,sizeof(TouchRecord),touchRecordCompare,NULL);

    // Now we have a sorted list of places to fill in, in importance order.
    // Check if the count *of the original touch grid cell* is positive.
    // Fill each in, decrement the neighboring air cells associated with each edge.
    // Also make sure to decrement the neighbor flags of the neighboring air cells (PITA).
    //
    // Then make the air cell into a solid thing. When filling:
    // First, choose the material: whatever is at +Y should be used, then XZXZ, then -Y
    // - good as any strategy
    // Next, see if all neighbors are in same group. Find the largest of the groups.
    // Add the new cell, and its bounds, to this largest group.
    // Merge the other groups to the largest group.

    for ( i = 0; i < touchCount; i++ )
    {
        // note that the ONLY thing we should be accessing from the touchList is the boxIndex.
        // We know the order is sorted now, so the other two bits of data (*especially* the count)
        // are useless. We're going to decrement the count on the gTouchGrid and its neighbors,
        // which is why this count is the one to access.
        boxIndex = touchList[i].boxIndex;

        // Any edges left to fix in the touch grid cell on the sorted list? Previous operations
        // might have decremented its count to 0.

        if ( gTouchGrid[boxIndex].count > 0 )
        {
            int boxMtlIndex=-999;
            int foundBlock=0;
            int faceGroupIDCount=0;
            int faceGroupID[6];
            int masterGroupID;
            int maxPop;
            int i;
            IPoint loc;

            // decrement this cell's neighbors' counts and pointers to it, each by one
            decrementNeighbors( boxIndex );

            // choose material for fill: +Y neighbor, on down to -Y - use an array to test
            // also get groups of solid blocks found

            // find 6 neighboring solids, if they exist (at least two will)
            for ( i = 0; i < 6; i++ )
            {
                int index = boxIndex+gFaceOffset[i];
                foundBlock = (gBoxData[index].type > BLOCK_AIR);
                if ( foundBlock )
                {
                    int j;
                    int foundGroup=0;
                    int groupID = gBoxData[index].group;
                    if ( boxMtlIndex < 0)
                        // store away the index of the first material found
                        boxMtlIndex = index;

                    for ( j = 0; j < faceGroupIDCount && !foundGroup; j++ )
                    {
                        if ( faceGroupID[j] == groupID )
                        {
                            foundGroup = 1;
                        }
                    }
                    if ( !foundGroup )
                    {
                        // add a new group to list
                        faceGroupID[faceGroupIDCount] = groupID;
                        faceGroupIDCount++;
                    }
                }
            }
            assert(faceGroupIDCount>0);

            // Now time for fun with the group list:
            // Which group is the largest? That one will be used to merge the rest. The new block also
            // gets this group ID, adds to pop.
            masterGroupID = faceGroupID[0];
            maxPop = gGroupList[faceGroupID[0]].population;
            for ( i = 1; i < faceGroupIDCount; i++ )
            {
                if ( gGroupList[faceGroupID[i]].population > maxPop )
                {
                    assert(gGroupList[faceGroupID[i]].solid);
                    masterGroupID = faceGroupID[i];
                    maxPop = gGroupList[masterGroupID].population;
                }
            }

            // tada! The actual work: the air block is now filled
            // if weld debugging is going on, we should make these some special color - what?
            assert(gBoxData[boxIndex].type == BLOCK_AIR );
            if ( gOptions->exportFlags & EXPT_DEBUG_SHOW_WELDS )
            {
                gBoxData[boxIndex].type = DEBUG_EDGE_TOUCH_TYPE;
            }
            else
            {
                gBoxData[boxIndex].type = gBoxData[boxMtlIndex].type;
                gBoxData[boxIndex].data = gBoxData[boxMtlIndex].data;
            }
            gStats.blocksManifoldWelded++;

            // we don't know which item on the group list is the air block's
            // group, so can't easily subtract one from its population. But, we
            // don't really care about the air group populations, ever.
            gBoxData[boxIndex].group = masterGroupID;
            gGroupList[masterGroupID].population++;
            boxIndexToLoc( loc, boxIndex );
            addBounds( loc, &gGroupList[masterGroupID].bounds );
            assert(gGroupList[masterGroupID].solid);

            // If multiple groups found, then we must merge all the groups on neighborList.
            if ( faceGroupIDCount > 1 )
            {
                IBox bounds;
                int *neighborGroups = (int *)malloc((gGroupCount+1)*sizeof(int));
                memset(neighborGroups,0,(gGroupCount+1)*sizeof(int));

                gStats.solidGroupsMerged += (faceGroupIDCount-1);
                gSolidGroups -= (faceGroupIDCount-1);
                assert(gSolidGroups >= 1);

                // Get the union of the bounds of all groups found to merge.
                VecScalar( bounds.min, =,  999999);
                VecScalar( bounds.max, =, -999999);
                // mark all the neighbor groups that are to merge
                for ( i = 0; i < faceGroupIDCount; i++ )
                {
                    if ( faceGroupID[i] != masterGroupID )
                    {
                        neighborGroups[faceGroupID[i]] = 1;
                        addBoundsToBounds( gGroupList[faceGroupID[i]].bounds, &bounds );
                    }
                }

                assert( bounds.max[Y] >= bounds.min[Y]);

                // Go through this sub-box and change all groups tagged to the new fill group.
                // We can fill air with whatever we want, since it won't be visible.
                // We don't actually want to fill with bedrock, but put that in for debugging -
                // really, groups just get merged. Also, note that the method below adds to
                // the master group's bounds.
                fillGroups( &bounds, masterGroupID, 1, BLOCK_LAVA, neighborGroups );

                free(neighborGroups);
            }

            // Note that we do not check if this air block getting filled gives a new
            // manifold edge somewhere or not, as some later block filling in might
            // make it go away, or change things some other way. TODO We could actually
            // check here, but it's not a huge deal to check everything again, I hope...
        }
    }

    free(touchList);
    free(gTouchGrid);
    gStats.numberManifoldPasses++;

    return touchCount ? 1 : 0;
}

static int touchRecordCompare( void* context, const void *str1, const void *str2)
{
    TouchRecord *t1;
    TouchRecord *t2;
    t1 = (TouchRecord*)str1;
    t2 = (TouchRecord*)str2;
    context;    // make a useless reference to the unused variable, to avoid C4100 warning
    // Blocks that had something in them originally (e.g. rails, redstone, or other things that got flattened)
    // are more significant than blocks of air, so the air should get covered up first so the rails aren't covered.
    // if the blocks are both air, or were both solid, then we need a different thing to test on.
    if ( gBoxData[t1->boxIndex].origType == gBoxData[t2->boxIndex].origType )
    {
        // elements that are in more of a crevice (more faces covered by solid neighbors) get filled first
        if ( t1->obscurity == t2->obscurity )
        {
            // elements fixing multiple manifold problems are filled before those fixing just one
            if ( t1->count == t2->count )
            {
                // elements closer to the center of the model (or center-bottom) should be filled in first, over those farther away
                return ( (t1->distance < t2->distance) ? -1 : ((t1->distance == t2->distance)) ? 0 : 1 );
            }
            else return ( (t1->count > t2->count) ? -1 : 1 );
        }
        else return ( (t1->obscurity > t2->obscurity) ? -1 : 1 );
    }
    else return ( (gBoxData[t1->boxIndex].origType < gBoxData[t2->boxIndex].origType ) ? -1 : 1 );
}


static void checkForTouchingEdge(int boxIndex, int offx, int offy, int offz)
{
    // we assume the location itself is solid. Check if diagonal is solid
    int otherSolidIndex = boxIndex + offx*gBoxSizeYZ + offy + offz*gBoxSize[Y];
    if ( gBoxData[otherSolidIndex].type > BLOCK_AIR )
    {
        // so far so good, both are solid, so we have two diagonally-opposite blocks
        if ( (gOptions->exportFlags & EXPT_CONNECT_ALL_EDGES) ||
            ( gBoxData[boxIndex].group != gBoxData[otherSolidIndex].group ) )
        {
            int n1index=-999;
            int n2index=-999;
            int n1neighbor=-999;
            int n2neighbor=-999;
            int foundPair = 0;
            // is this the +X face or +Z face
            if ( offx == 1 )
            {
                // we're checking the +X face. The calling method has already tested +X and knows it's air.
                // So just use the other two offsets to check if the other direction is air.

                // so begins the brute force. There's probably some clever way to do this...
                if ( gBoxData[boxIndex + offy + offz*gBoxSize[Y]].type == BLOCK_AIR )
                {
                    // manifold found! So, mark the two air blocks, +X and y/z offset, and put the proper
                    // TOUCH_ flags in the touch grid.
                    foundPair = 1;
                    gStats.nonManifoldEdgesFound++;
                    n1index = boxIndex+gBoxSizeYZ;
                    if ( offy > 0 )
                    {
                        // +X+Y
                        n2index = boxIndex+1;
                        n1neighbor = TOUCH_MX_PY;
                        n2neighbor = TOUCH_PX_MY;
                    }
                    else if ( offy < 0 )
                    {
                        // +X-Y
                        n2index = boxIndex-1;
                        n1neighbor = TOUCH_MX_MY;
                        n2neighbor = TOUCH_PX_PY;
                    }
                    else if ( offz > 0)
                    {
                        // +X+Z
                        n2index = boxIndex+gBoxSize[Y];
                        n1neighbor = TOUCH_MX_PZ;
                        n2neighbor = TOUCH_PX_MZ;
                    }
                    else
                    {
                        // +X-Z
                        n2index = boxIndex-gBoxSize[Y];
                        n1neighbor = TOUCH_MX_MZ;
                        n2neighbor = TOUCH_PX_PZ;
                   }
                }
            }
            else
            {
                // we're on the +Z face, just need to test the Y offset for AIR
                assert(offz == 1);
                if ( gBoxData[boxIndex + offy].type == BLOCK_AIR )
                {
                    foundPair = 1;
                    assert(offx == 0);
                    gStats.nonManifoldEdgesFound++;
                    n1index = boxIndex+gBoxSize[Y];
                    // we know Z face is touched, now is it +Y or -Y?
                    if ( offy > 0)
                    {
                        // +Z+Y is the solid neighbor, so the +Z air neighbors +Y and vice versa:
                        n2index = boxIndex+1;
                        n1neighbor = TOUCH_PY_MZ;
                        n2neighbor = TOUCH_MY_PZ;
                    }
                    else
                    {
                        // +Z-Y is the solid neighbor
                        assert(offy == -1);
                        n2index = boxIndex-1;
                        n1neighbor = TOUCH_MY_MZ;
                        n2neighbor = TOUCH_PY_PZ;
                    }
                }
            }
            if ( foundPair )
            {
                // a pair is found. Compute obscurity for each (how much the block is blocked),
                // and whichever has the highest obscurity win
                int n1obscurity,n2obscurity,obscurityMatches;
                assert(n1index>=0);
                assert(n2index>=0);
                n1obscurity = computeObscurity(n1index);
                n2obscurity = computeObscurity(n2index);
                obscurityMatches = (n1obscurity == n2obscurity);

                // We check if one obscurity is larger than the other;
                // if it is, it wins, and the other won't need to get added.
                // Note that the count in a cell, by this code, then does *not* match
                // how many neighbors it will subtract from others when it is filled.
                if ( n1obscurity >= n2obscurity)
                {
                    // adding a new cell? Note it
                    if ( gTouchGrid[n1index].count == 0 )
                        gTouchSize++;
                    // add the fact that this cell touches one manifold edge
                    gTouchGrid[n1index].count++;
                    // note which neighbor it connects to
                    gTouchGrid[n1index].connections |=  obscurityMatches ? n1neighbor : 0x0;
                }
                if ( n2obscurity >= n1obscurity)
                {
                    if ( gTouchGrid[n2index].count == 0 )
                        gTouchSize++;
                    gTouchGrid[n2index].count++;
                    gTouchGrid[n2index].connections |= obscurityMatches ? n2neighbor : 0x0;
                }

                //// adding a new cell? Note it
                //if ( gTouchGrid[n1index].count == 0 )
                //    gTouchSize++;
                //// add the fact that this cell touches one manifold edge
                //gTouchGrid[n1index].count++;
                //// note which neighbor it connects to
                //gTouchGrid[n1index].connections |= n1neighbor;

                //if ( gTouchGrid[n2index].count == 0 )
                //    gTouchSize++;
                //gTouchGrid[n2index].count++;
                //gTouchGrid[n2index].connections |= n2neighbor;
            }
        }
    }
}

// count how many of the six directions for this cell are blocked by something solid
static int computeObscurity( int boxIndex )
{
    int obscurity = gTouchGrid[boxIndex].obscurity;

    // we know that obscurity must be 2 or more; so 0 means "not set"
    if ( obscurity == 0 )
    {
        int faceDirection;
        IPoint loc;
        boxIndexToLoc( loc, boxIndex );

        for ( faceDirection = 0; faceDirection < 6; faceDirection++ )
        {
            // we want to get the start & ending locations and increment for the loop to
            // check obscurity.
            int start = boxIndex;
            int incr=-999;
            int hit=0;
            int i, cellIndex, cellsToLoop;
            int axis = faceDirection % 3;

            switch (axis)
            {
            case X:
                incr=gBoxSizeYZ;
                break;
            case Y:
                incr=1;
                break;
            case Z:
                incr=gBoxSize[Y];
                break;
            default:
                assert(0);
            }

            if ( faceDirection < 3 )
            {
                // traveling negative
                incr = -incr;
                cellsToLoop = loc[axis] - gSolidBox.min[axis];
            }
            else
            {
                // traveling positive
                cellsToLoop = gSolidBox.max[axis] - loc[axis];
            }
            // get past first cell (where we started)
            start += incr;

            // now check the stretch of cells in the given direction
            for ( i = 0, cellIndex = start; i < cellsToLoop && !hit; i++, cellIndex += incr )
            {
                if ( gBoxData[cellIndex].type > BLOCK_AIR )
                    hit = 1;
            }
            obscurity += hit;
        }

    }
    return obscurity;
}

// given location, turn off neighbors from grid
static void decrementNeighbors( int boxIndex )
{
    int nc = 0;
    int offset;
    int connections = gTouchGrid[boxIndex].connections;

    if ( connections & TOUCH_MX_MY )
    {
        nc++;
        offset = boxIndex - gBoxSizeYZ - 1;
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_PX_PY;
    }
    if ( connections & TOUCH_MX_MZ )
    {
        nc++;
        offset = boxIndex - gBoxSizeYZ - gBoxSize[Y];
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_PX_PZ;
    }
    if ( connections & TOUCH_MY_MZ )
    {
        nc++;
        offset = boxIndex - 1 - gBoxSize[Y];
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_PY_PZ;
    }

    if ( connections & TOUCH_MX_PY )
    {
        nc++;
        offset = boxIndex - gBoxSizeYZ + 1;
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_PX_MY;
    }
    if ( connections & TOUCH_MX_PZ )
    {
        nc++;
        offset = boxIndex - gBoxSizeYZ + gBoxSize[Y];
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_PX_MZ;
    }
    if ( connections & TOUCH_MY_PZ )
    {
        nc++;
        offset = boxIndex - 1 + gBoxSize[Y];
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_PY_MZ;
    }

    if ( connections & TOUCH_PX_MY )
    {
        nc++;
        offset = boxIndex + gBoxSizeYZ - 1;
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_MX_PY;
    }
    if ( connections & TOUCH_PX_MZ )
    {
        nc++;
        offset = boxIndex + gBoxSizeYZ - gBoxSize[Y];
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_MX_PZ;
    }
    if ( connections & TOUCH_PY_MZ )
    {
        nc++;
        offset = boxIndex + 1 - gBoxSize[Y];
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_MY_PZ;
    }

    if ( connections & TOUCH_PX_PY )
    {
        nc++;
        offset = boxIndex + gBoxSizeYZ + 1;
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_MX_MY;
    }
    if ( connections & TOUCH_PX_PZ )
    {
        nc++;
        offset = boxIndex + gBoxSizeYZ + gBoxSize[Y];
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_MX_MZ;
    }
    if ( connections & TOUCH_PY_PZ )
    {
        nc++;
        offset = boxIndex + 1 + gBoxSize[Y];
        gTouchGrid[offset].count--;
        assert(gTouchGrid[offset].count>=0);
        gTouchGrid[offset].connections &= ~TOUCH_MY_MZ;
    }
    // we should have cleared as many as we had in the cell
    // Well, no longer true: we can now have a count > nc,
    // since we now use obscurity to win early on.
    assert(nc <= gTouchGrid[boxIndex].count);

    // clear cell itself
    gTouchGrid[boxIndex].connections = 0;
    gTouchGrid[boxIndex].count = 0;
}


static float getDistanceSquared( Point loc1, Point loc2 )
{
    Vector vec;
    Vec3Op( vec, =, loc1, -, loc2 );
    Vec2Op( vec, *=, vec );
    return ( vec[X] + vec[Y] + vec[Z] );
}

static void boxIndexToLoc( IPoint loc, int boxIndex )
{
    int yzLeft;
    loc[X] = boxIndex / gBoxSizeYZ;
    yzLeft = boxIndex % gBoxSizeYZ;
    loc[Z] = yzLeft / gBoxSize[Y];
    loc[Y] = yzLeft % gBoxSize[Y];
}


static void deleteFloatingGroups()
{
    BoxGroup *pGroup;
    int deleteGroup;
    int i,x,y,z;
    int boxIndex;
    int treeParts;
    int *neighborGroups;

    // Find largest group and don't delete it, so there's always something left.
    // If there's a tie on size, lower Y bounds is the tie breaker
    // [don't need to look at air group]
    int survivorGroup = -999;
    int maxPop = -999;
    int minY=999;
    for ( i = SURROUND_AIR_GROUP+1; i <= gGroupCount; i++ )
    {
        pGroup = &gGroupList[i];
        // is this group solid
        if ( pGroup->solid )
        {
            // if this group is larger than any other group, or equal to another
            // group but lower down (nearer to ground), make sure to save it by
            // making it designated as the largest group.
            if ( (pGroup->population > maxPop) ||
                ( (pGroup->population == maxPop) && (pGroup->bounds.min[Y] < minY)))
            {
                maxPop = pGroup->population;
                survivorGroup = i;
                minY = pGroup->bounds.min[Y];
            }
        }
    }
    assert(maxPop > 0);

    // [don't need to look at air group]
    for ( i = SURROUND_AIR_GROUP+1; i <= gGroupCount; i++ )
    {
        pGroup = &gGroupList[i];
        // is this group solid and does it have anything in it (not already merged)?
        if ( pGroup->solid && (pGroup->population > 0) && i != survivorGroup )
        {
            deleteGroup = 0;
            // is it too small and is it not touching ground level?
            if ( pGroup->population < gOptions->pEFD->floaterCountVal )
                // this is the "touching ground level" test, but it's too annoying - leaves tiny clumps at the base
                // && (pGroup->bounds.min[Y] > gSolidBox.min[Y]) )
            {
                deleteGroup = 1;
            }
            else
            {
                // If not too small, is it all tree?
                // Test all objects in group - are they all tree parts?

                // TODO?: a better rule might be "if any group is hanging and is also touching the solid box X or Z faces, delete it"
                // This would certainly be good for trees, though I think deleting hanging trees always is a safe thing.

                // assume delete unless we find something non-wood
                deleteGroup = 1;
                treeParts = 0x0;
                for ( x = pGroup->bounds.min[X]; deleteGroup && x <= pGroup->bounds.max[X]; x++ )
                {
                    for ( z = pGroup->bounds.min[Z]; deleteGroup && z <= pGroup->bounds.max[Z]; z++ )
                    {
                        boxIndex = BOX_INDEX(x,pGroup->bounds.min[Y],z);
                        for ( y = pGroup->bounds.min[Y]; deleteGroup && y <= pGroup->bounds.max[Y]; y++, boxIndex++ )
                        {
                            // is this group one that should get filled by the master group?
                            if ( gBoxData[boxIndex].group == i)
                            {
                                // group matches: is it a tree part? Or is it a glass bubble that is
                                // is surrounded by tree bits? (this can happen, some trees grow funny)
                                if ( (gBlockDefinitions[gBoxData[boxIndex].type].flags & BLF_TREE_PART) ||
                                    (gBoxData[boxIndex].origType == BLOCK_AIR) || (gBoxData[boxIndex].origType == BLOCK_VINES) )
                                {
                                    // tree part, mark which parts
                                    treeParts |= gBlockDefinitions[gBoxData[boxIndex].type].flags;
                                }
                                else
                                {
                                    deleteGroup = 0;
                                }
                            }
                        }
                    }
                }
                if ( deleteGroup )
                {
                    // final test; were there any leaves? If no leaves in leaves & trunk, then just trunk,
                    // which could be fine: totem pole, house, etc.
                    if ( !(treeParts & BLF_LEAF_PART) )
                    {
                        // no leaves, so don't delete
                        deleteGroup = 0;
                    }
                }
            }
            // ok, tests done: delete?
            if ( deleteGroup )
            {
                assert( i == pGroup->groupID );
                neighborGroups = (int *)malloc((gGroupCount+1)*sizeof(int));
                memset(neighborGroups,0,(gGroupCount+1)*sizeof(int));
                neighborGroups[i] = 1;
                gStats.blocksFloaterDeleted += pGroup->population;
                fillGroups( &(pGroup->bounds), SURROUND_AIR_GROUP, 0, BLOCK_AIR, neighborGroups );
                gStats.floaterGroupsDeleted++;
                gSolidGroups--;
                assert(gSolidGroups >= 0);
                free(neighborGroups);
            }
        }
    }
}

static int determineScaleAndHollowAndMelt()
{
    // get these for statistics and for autoscaling.
    // If returns 0, nothing in box. Abort output.
    gBlockCount = getDimensionsAndCount(gFilledBoxSize);
    if ( gBlockCount + gModel.billboardCount == 0 )
    {
        // there really should be something in the box
        assert(0);
        return MW_NO_BLOCKS_FOUND;
    }

    // fill in statistics - the density is actually used for cost scaling
    gStats.numBlocks = gBlockCount;
    gStats.density = (float)gStats.numBlocks / (float)(gFilledBoxSize[X]*gFilledBoxSize[Y]*gFilledBoxSize[Z]);
    gStats.numGroups = gAirGroups + gSolidGroups;
    gStats.numAirGroups = gAirGroups;
    gStats.numSolidGroups = gSolidGroups;

    // determine scale for vertices at this point.    
    if ( gOptions->pEFD->radioScaleToHeight )
    {
        // scale to desired height
        gModel.scale = gOptions->pEFD->modelHeightVal*CM_TO_METERS / gFilledBoxSize[Y];
    }
    else if ( gOptions->pEFD->radioScaleToMaterial )
    {
        // autoscale to cheapest size - good for test objects
        // You can make them smaller still, but have to be careful about wall thickness

        // in meters - internally we use meters throughout
        float minWall = mtlCostTable[gPhysMtl].minWall;

        // check that dimensions are big enough
        float sum = gFilledBoxSize[X] + gFilledBoxSize[Y] + gFilledBoxSize[Z];

        // for colored sandstone, sum must be > 65 mm, which is 0.065 - this is often the limiter
        gModel.scale = mtlCostTable[gPhysMtl].minDimensionSum / sum;

        // minimum wall thickness trumps minimum dimension sum
        if ( gModel.scale < minWall )
        {
            gModel.scale = minWall;
        }
    }
    else if ( gOptions->pEFD->radioScaleByBlock )
    {
        // simple straight up block size
        gModel.scale = gOptions->pEFD->blockSizeVal[gOptions->pEFD->fileType]*MM_TO_METERS;
    }
    else
    {
        assert(gOptions->pEFD->radioScaleByCost);
        scaleByCost();
    }

    // now compute how many blocks are needed for a minimally-thick wall
    gWallBlockThickness = (int)(ceil(mtlCostTable[gPhysMtl].minWall/gModel.scale));

    // 5) Wasted material. While bubbles are illegal, it's fine to hollow out the base of any object, working from the
    //    bottom up. Option: delete a block at the base if it has neighboring blocks in all 8 positions on this level
    //    and all 9 positions on the level above. This may be overconservative in some cases, but is safe. Mark all
    //    these positions, working up the object, then delete.
    // Now hollow - should be the last thing done, as we always want to build up before deleting
    if ( gOptions->exportFlags & EXPT_HOLLOW_BOTTOM )
    {
        // Check each solid block on the bottom layer (so we start +2 in from X & Z edges)
        // If the 9 neighbors above and 8 neighbors on this level are all solid, we can clear
        // this location. Save the location in a list and move on (since this location will
        // affect other picks).
        // [We could try to share neighbors samples from location to location, but that's messy.]
        hollowBottomOfModel();
    }

    if ( gOptions->pEFD->chkMeltSnow )
    {
        // Simple: remove any snow blocks found. This is useful to use in conjunction with hollowing: the hollow
        // operation clears the base, then you melt snow on the floor of your building, just above this hollow
        // area. Now your building's interior is connected to the hollow area below and so will clear of material.
        meltSnow();
    }

    // If we're scaling by cost, we now have the *real* block count after hollowing. We needed stats
    // before for things like wall thickness, which hollowing uses. If we hollowed at all (check stats), we need to
    // rescale to a new (larger) block size, since we can now afford more material. This is actually good, in that
    // blocks will never get smaller (walls won't get thinner), but it might make for slightly thicker walls than
    // needed, if gWallBlockThickness could have been decreased.
    if ( gOptions->pEFD->radioScaleByCost )
    {
        scaleByCost();
    }

    return MW_NO_ERROR;
}

// find model scale, given cost
static void scaleByCost()
{
    float materialBudget,budgetPerBlock;

    // scale by cost
    assert(gOptions->pEFD->radioScaleByCost);

    // white gets tricky because of the discount - won't affect anything else:
    // http://www.shapeways.com/blog/archives/490-Significant-price-reduction-on-dense-models.html

    materialBudget = gOptions->pEFD->costVal - mtlCostTable[gPhysMtl].costHandling;
    // the dialog should not allow this to happen:
    assert(materialBudget > 0.0f);

    // ceramics are a special case, based on surface area instead of volume
    if ( mtlCostTable[gPhysMtl].costPerSquareCentimeter > 0.0f )
    {
        // solve for gModel.scale:
        // mtlCostTable[printMaterialType].costPerSquareCentimeter * area = cost
        // scale = cost / ( costPerCM2 * area );
        gModel.scale = materialBudget / (mtlCostTable[gPhysMtl].costPerSquareCentimeter * AREA_IN_CM2);
        assert( mtlCostTable[gPhysMtl].costPerCubicCentimeter == 0.0f );   // in case things change
        return;
    }

    // how much money we can spend on each block
    budgetPerBlock = materialBudget / (float)gBlockCount;

    // so a cubic centimeter costs say 75 cents
    // budgetPerBlock / COST_COLOR_CCM - how much we can spend for each block.
    // Take the cube root of this to get the size in cm,
    // convert to meters to get size in meters
    gModel.scale = (float)pow((double)(budgetPerBlock / mtlCostTable[gPhysMtl].costPerCubicCentimeter),1.0/3.0) * CM_TO_METERS;

    // fill in statistics:
    // if density > 10%, and the cubic centimeters of material over 20, then discount applies.
    if ( (gStats.density > mtlCostTable[gPhysMtl].costDiscountDensityLevel) && (pow((double)(gModel.scale*METERS_TO_CM),3.0)*gBlockCount > mtlCostTable[gPhysMtl].costDiscountCCMLevel ) )
    {
            // We wimp out here by simply incrementing the scale by 0.1 mm (which is smaller than the minimum detail) until the cost is reached
        while ( gOptions->pEFD->costVal > computeMaterialCost( gPhysMtl, gModel.scale, gBlockCount, gStats.density ))
        {
            gModel.scale += 0.1f*MM_TO_METERS;
        }
    }
}

static void hollowBottomOfModel()
{
    int x,y,z,boxIndex;
    int listCount,addPost;
    int listFound=1;
    int survived,dir,neighborCount,neighborIndex;
    IPoint loc;
    // we could actually allocate less, but don't want to risk allocing 0 (or less than 0!)
    int *listToChange=(int *)malloc((gSolidBox.max[X]-gSolidBox.min[X]+1)*(gSolidBox.max[Z]-gSolidBox.min[Z]+1)*sizeof(int));

    IPoint *seedStack;
    int seedCount;

    int hollowListSize = gBoxSize[X]*gBoxSize[Z]*sizeof(unsigned char);
    unsigned char *hollowDone = (unsigned char *)malloc(hollowListSize);
        memset(hollowDone,0,hollowListSize);

    assert(gOptions->pEFD->hollowThicknessVal[gOptions->pEFD->fileType] > 0.0f);
    gHollowBlockThickness = (int)(ceil(gOptions->pEFD->hollowThicknessVal[gOptions->pEFD->fileType]*MM_TO_METERS/gModel.scale));

    // an arbitrary guess: if the box is long in one dimension or the other, add posts for support
    addPost = ( gSolidBox.max[X] > gSolidBox.min[X] + 8 + 2*gHollowBlockThickness ) || ( gSolidBox.max[Z] > gSolidBox.min[Z] + 8 + 2*gHollowBlockThickness );

    for ( y = gSolidBox.min[Y]; (y < gSolidBox.max[Y]) && listFound; y++ )
    {
        listCount = 0;
        // we know the walls have to be at least gHollowBlockThickness thick, so don't bother checking those.
        for ( x = gSolidBox.min[X]+gHollowBlockThickness; x <= gSolidBox.max[X]-gHollowBlockThickness; x++ )
        {
            for ( z = gSolidBox.min[Z]+gHollowBlockThickness; z <= gSolidBox.max[Z]-gHollowBlockThickness; z++ )
            {
                boxIndex = BOX_INDEX(x,y,z);
                // First check that we can continue to hollow things upward from base, then
                // check 9 above and 8 on sides.
                if ( !hollowDone[x*gBoxSize[Z]+z] )
                {
                    // check if the block is in a corner: if so, don't hollow it, in order to
                    // give some support to the walls being formed. TODO with Shapeways tests: is this necessary? I
                    // like to think it makes for a more solid model.
                    // This test is a little wasteful, it normally fails just 4 times and then the hollowDone
                    // is set for those post positions. However, we need to make sure the posts get their own 0
                    // group in the "else".
                    if ( !addPost || ( (x > gSolidBox.min[X]+gHollowBlockThickness) && (x < gSolidBox.max[X]-gHollowBlockThickness) )
                        || ( (z > gSolidBox.min[Z]+gHollowBlockThickness) && (z < gSolidBox.max[Z]-gHollowBlockThickness) ) )
                    {
                        // brute force the 3x3 above and 3x3 in the middle layer: all solid?
                        if ( gBoxData[boxIndex-1].type == BLOCK_AIR &&    // if block below is air
                            gBoxData[boxIndex].type != BLOCK_AIR &&    // if block is solid
                            gBoxData[boxIndex+1].type != BLOCK_AIR &&   // +Y
                            gBoxData[boxIndex-gBoxSizeYZ].type != BLOCK_AIR &&  // -X
                            gBoxData[boxIndex+gBoxSizeYZ].type != BLOCK_AIR &&  // +X
                            gBoxData[boxIndex-gBoxSize[Y]].type != BLOCK_AIR &&  // -Z
                            gBoxData[boxIndex+gBoxSize[Y]].type != BLOCK_AIR &&  // +Z
                            gBoxData[boxIndex-gBoxSizeYZ-gBoxSize[Y]].type != BLOCK_AIR &&  // -X-Z
                            gBoxData[boxIndex+gBoxSizeYZ-gBoxSize[Y]].type != BLOCK_AIR &&  // +X-Z
                            gBoxData[boxIndex-gBoxSizeYZ+gBoxSize[Y]].type != BLOCK_AIR &&  // -X+Z
                            gBoxData[boxIndex+gBoxSizeYZ+gBoxSize[Y]].type != BLOCK_AIR &&  // +X+Z
                            gBoxData[boxIndex-gBoxSizeYZ+1].type != BLOCK_AIR &&  // -X+Y
                            gBoxData[boxIndex+gBoxSizeYZ+1].type != BLOCK_AIR &&  // +X+Y
                            gBoxData[boxIndex-gBoxSize[Y]+1].type != BLOCK_AIR &&  // -Z+Y
                            gBoxData[boxIndex+gBoxSize[Y]+1].type != BLOCK_AIR &&  // +Z+Y
                            gBoxData[boxIndex-gBoxSizeYZ-gBoxSize[Y]+1].type != BLOCK_AIR &&  // -X-Z+Y
                            gBoxData[boxIndex+gBoxSizeYZ-gBoxSize[Y]+1].type != BLOCK_AIR &&  // +X-Z+Y
                            gBoxData[boxIndex-gBoxSizeYZ+gBoxSize[Y]+1].type != BLOCK_AIR &&  // -X+Z+Y
                            gBoxData[boxIndex+gBoxSizeYZ+gBoxSize[Y]+1].type != BLOCK_AIR )  // +X+Z+Y
                        {
                            // OK, this one can be deleted. Now check extra width, if any
                            if ( gHollowBlockThickness > 1 )
                            {
                                // check neighbors in the 5 directions to see if all solid
                                survived = 1;
                                for ( dir = 0; survived && (dir < 6); dir++ )
                                {
                                    if ( dir != DIRECTION_BLOCK_BOTTOM)
                                    {
                                        Vec3Scalar( loc, =, x,y,z);
                                        neighborCount = gHollowBlockThickness-1;
                                        // get the next neighbor over, which we already checked above
                                        getNeighborUnsafe( dir, loc );
                                        // and get the next neighbor of *it* - note that getNeighbor changes the loc
                                        while ( survived && getNeighbor( dir, loc ) && neighborCount > 0 )
                                        {
                                            neighborIndex = BOX_INDEXV(loc);
                                            // is neighbor not in a group, and the same sort of thing as our seed (solid or not)?
                                            if ( gBoxData[neighborIndex].type == BLOCK_AIR )
                                            {
                                                survived = 0;
                                            }
                                            neighborCount--;
                                        }
                                    }
                                }
                                if ( survived )
                                {
                                    // area all around is thick enough, so we can delete it
                                    listToChange[listCount++] = boxIndex;
                                }
                            }
                            else
                            {
                                // simple 1-block-thick wall case - we can delete it
                                listToChange[listCount++] = boxIndex;
                            }
                        }
                        else
                        {
                            // We have reached the limits of hollowing upwards, so stop hollowing
                            // hollowDone is now an array showing the maximum height reached, +1
                            hollowDone[x*gBoxSize[Z]+z] = (unsigned char)y;
                        }
                    }
                    else
                    {
                        // corner post hit - mark it as a post so it doesn't get super-hollowed later
                        // do this to only solid objects. This is done until we hit air.
                        // TODO: when we hit air we could continue, not sure that helps...
                        if ( !hollowDone[x*gBoxSize[Z]+z] )
                            if (gBoxData[boxIndex].type > BLOCK_AIR)
                                gBoxData[boxIndex].group = HOLLOW_AIR_GROUP;
                            else
                                // stop making a post if we hit air. This OK? TODO
                                hollowDone[x*gBoxSize[Z]+z] = (unsigned char)y;
                    }
                }
            }
        }
        // delete any found. If none found, we're done.
        listFound = listCount;  // note if a list was found
        if ( listCount )
        {
            while ( listCount > 0 )
            {
                listCount--;
                assert(listToChange[listCount]>=0 && listToChange[listCount]<gBoxSizeXYZ);
                // note at this point we're not messing with populations, since hollow is the very last operation.
                // If this changes, need to decrement and add to populations here, and we'd need to get the new bounds
                // for any groups that lost anything (and gained anything), etc.
                gBoxData[listToChange[listCount]].type = BLOCK_AIR;
                // must track block count now, as it's been computed
                gBlockCount--;
                // special use of group 0 - for hollow
                gBoxData[listToChange[listCount]].group = HOLLOW_AIR_GROUP;
                gStats.blocksHollowed++;
            }
        }
    }
    free(listToChange);

    if ( gOptions->exportFlags & EXPT_SUPER_HOLLOW_BOTTOM )
    {
        // Now do superhollowing:
        // Check all solids next to the hollow areas: can the hollows be expanded outward?
        // We use the hollowDone "heightmap" to generate seeds: if the height of an XZ position is lower or equal to the height
        // of its four neighboring hollowDones then the elements at its height to the maximum of the other 4 heights - 1 should be checked
        // as possible seeds.
        seedStack = (IPoint *)malloc(gBoxSizeXYZ*sizeof(IPoint));
        seedCount = 0;
        gStats.blocksSuperHollowed = 0;
        for ( x = gSolidBox.min[X]+gHollowBlockThickness; x <= gSolidBox.max[X]-gHollowBlockThickness; x++ )
        {
            for ( z = gSolidBox.min[Z]+gHollowBlockThickness; z <= gSolidBox.max[Z]-gHollowBlockThickness; z++ )
            {
                // we know we can't extend upwards beyond this height, at least not directly, so see if we can expand sideways.
                int hollowHeight = hollowDone[x*gBoxSize[Z]+z];

                int maxNeighborHeight = hollowDone[(x-1)*gBoxSize[Z]+z];
                int nextNeighborHeight = hollowDone[(x+1)*gBoxSize[Z]+z];
                if ( nextNeighborHeight > maxNeighborHeight )
                    maxNeighborHeight = nextNeighborHeight;
                nextNeighborHeight = hollowDone[(x+1)*gBoxSize[Z]+z];
                if ( nextNeighborHeight > maxNeighborHeight )
                    maxNeighborHeight = nextNeighborHeight;
                nextNeighborHeight = hollowDone[(x+1)*gBoxSize[Z]+z];
                if ( nextNeighborHeight > maxNeighborHeight )
                    maxNeighborHeight = nextNeighborHeight;

                if ( maxNeighborHeight > hollowHeight )
                {
                    // cliff-face found, so see all from hollowHeight to max height - 1
                    for ( y = hollowHeight; y < maxNeighborHeight; y++ )
                    {
                        // propagate seed: make neighbors with same type (solid or air) and no group to be this group.
                        hollowSeed( x, y, z, seedStack,&seedCount);

                        // When this is done, seedStack has a list of seeds that had no ID before and now have one.
                        // Each of these seeds' neighbors needs to be tested.

                        // while seedCount > 0, propagate
                        while ( seedCount > 0 )
                        {
                            // copy test point over, so we don't trample it when seedStack gets increased
                            seedCount--;
                            hollowSeed(seedStack[seedCount][X],seedStack[seedCount][Y],seedStack[seedCount][Z],seedStack,&seedCount);
                        }
                    }
                }
            }
        }
        free(seedStack);
    }

    free(hollowDone);
}

static void hollowSeed( int x, int y, int z, IPoint *seedList, int *seedCount )
{
    IPoint loc;
    // recursively check this seed location and all neighbors for whether this location can be hollowed out
    int boxIndex = BOX_INDEX(x,y,z);
    
    // first, is it already empty? or marked as part of hollow (as the posts are)?
    if ( gBoxData[boxIndex].type != BLOCK_AIR && gBoxData[boxIndex].group != HOLLOW_AIR_GROUP )
    {
        // OK, it can be tested and could spawn more seeds
        int neighborBoxIndex,dir;

        // first test to see if the 26 neighbors are all either solid, or have group 0 (hollow).
        // We do not want to tunnel through any walls and reach air that's not part of the hollow group.
        int ok = 1;
        for ( loc[X] = x-1; ok && loc[X] <= x+1; loc[X]++ )
        {
            for ( loc[Z] = z-1; ok && loc[Z] <= z+1; loc[Z]++ )
            {
                neighborBoxIndex = BOX_INDEX(loc[X],y-1,loc[Z]);
                for ( loc[Y] = y-1; ok && loc[Y] <= y+1; loc[Y]++, neighborBoxIndex++ )
                {
                    if ( gBoxData[neighborBoxIndex].type == BLOCK_AIR &&
                        gBoxData[neighborBoxIndex].group != HOLLOW_AIR_GROUP )
                    {
                        // outside air found, so can't grow that direction
                        ok = 0;
                    }
                }
            }
        }

        // survived initial test. If desired wall thickness is > 1, check the surrounding solid
        // blocks in the 6 directions.
        // This test is a little "dangerous", in that if the wall thickness is very high, it should
        // really be testing a large block of neighbors to make sure it's large enough. This is just
        // a quick and dirty version. We could instead have the loop above go from -gHollowBlockThickness
        // to +gHollowBlockThickness. This quick and dirty is a little more aggressive, though, cleaning
        // out as much as it can.
        if ( gHollowBlockThickness > 1 )
        {
            for ( dir = 0; ok && (dir < 6); dir++ )
            {
                int neighborCount = gHollowBlockThickness-1;
                Vec3Scalar( loc, =, x,y,z);
                // get the next neighbor over, which we already checked above
                getNeighborUnsafe( dir, loc );
                // and get the next neighbor of *it*. Note that getNeighbor changes the loc
                while ( ok && getNeighbor( dir, loc ) && neighborCount > 0 )
                {
                    neighborBoxIndex = BOX_INDEXV(loc);
                    // is neighbor not in a group, and the same sort of thing as our seed (solid or not)?
                    if ( gBoxData[neighborBoxIndex].type == BLOCK_AIR &&
                        gBoxData[neighborBoxIndex].group != HOLLOW_AIR_GROUP )
                    {
                        ok = 0;
                    }
                    neighborCount--;
                }
            }
        }

        if ( ok )
        {
            // survived! Turn it hollow, and check the six seeds. Recursion, here we come.
            gBoxData[boxIndex].type = BLOCK_AIR;
            gBoxData[boxIndex].group = HOLLOW_AIR_GROUP;
            gStats.blocksSuperHollowed++;
            // must track block count now, as it's been computed
            gBlockCount--;

            for ( dir = 0; dir < 6; dir++ )
            {
                int sindex = *seedCount;
                Vec3Scalar( seedList[sindex], =, x+gFaceDirectionVector[dir][X], y+gFaceDirectionVector[dir][Y], z+gFaceDirectionVector[dir][Z] );
                (*seedCount)++;
                // it's possible that we could run out, I think: each seed could add its six neighbors, etc.?
                assert((*seedCount)<gBoxSizeXYZ);
           }
        }
    }
}

static void meltSnow()
{
    int x,y,z,boxIndex;
    for ( x = gSolidBox.min[X]; x <= gSolidBox.max[X]; x++ )
    {
        for ( z = gSolidBox.min[Z]; z <= gSolidBox.max[Z]; z++ )
        {
            boxIndex = BOX_INDEX(x,gSolidBox.min[Y],z);
            for ( y = gSolidBox.min[Y]; y <= gSolidBox.max[Y]; y++, boxIndex++ )
            {
                if ( gBoxData[boxIndex].type == BLOCK_SNOW_BLOCK )
                {
                    // melting time
                    gBoxData[boxIndex].type = BLOCK_AIR;
                    // We don't know if it's true that this is the right air group, but who cares,
                    // it's the last operation before exporting the model itself. Still, give it some
                    // group, just in case...
                    gBoxData[boxIndex].group = SURROUND_AIR_GROUP;
                    gStats.blocksHollowed++;
                }
            }
        }
    }
}

static void generateBlockDataAndStatistics()
{
    int i, boxIndex, normalCount;
    IPoint loc;
    float pgFaceStart,pgFaceOffset;

    // Minecraft's great, just six normals does it (mostly)
    // Billboards have an additional 4 at this point, rails 8 (top and bottom)
    Point normals[18] = {{-1,0,0},{0,-1,0},{0,0,-1},{1,0,0},{0,1,0},{0,0,1},
        {-OSQRT2,0,-OSQRT2},
        {-OSQRT2,0, OSQRT2},
        { OSQRT2,0,-OSQRT2},
        { OSQRT2,0, OSQRT2},

        {-OSQRT2,-OSQRT2,      0},  // DIRECTION_LO_X_LO_Y
        {      0,-OSQRT2,-OSQRT2},  // DIRECTION_LO_Z_LO_Y
        { OSQRT2,-OSQRT2,      0},  // DIRECTION_HI_X_LO_Y
        {      0,-OSQRT2, OSQRT2},  // DIRECTION_HI_Z_LO_Y
        {-OSQRT2, OSQRT2,      0},  // DIRECTION_LO_X_HI_Y
        {      0, OSQRT2,-OSQRT2},  // DIRECTION_LO_Z_HI_Y
        { OSQRT2, OSQRT2,      0},  // DIRECTION_HI_X_HI_Y
        {      0, OSQRT2, OSQRT2}   // DIRECTION_HI_Z_HI_Y
        };


    // Add the following to this, everyone does it
    if ( gOptions->pEFD->chkCenterModel )
    {
        // Compute center for output. This could be more elaborate, but our goal
        // here is simply to approximately center in X and Z and put min.Y at 0 level

        gModel.center[Y] = (float)gSolidBox.min[Y];
        // Note that we don't perfectly center all the time, but rather try to make blocks
        // align with whole numbers. If you output in meters, you get no fractions, so get
        // a smaller OBJ file.
        gModel.center[X] = (float)floor((float)(gSolidBox.max[X]+gSolidBox.min[X]+1)/2.0f);
        gModel.center[Z] = (float)floor((float)(gSolidBox.max[Z]+gSolidBox.min[Z]+1)/2.0f);
    }
    else
    {
        // keep world coordinates (may get rescaled, of course)
        Vec2Op(gModel.center, =, (float)gWorld2BoxOffset);
    }

    // get the normals into their proper orientations
    normalCount = gExportBillboards ? 18 : 6;
    for ( i = 0; i < normalCount; i++ )
    {
        rotateLocation(normals[i]);
        Vec2Op(gModel.normals[i], =, normals[i]);
    }

    pgFaceStart = PG_DB+0.01f;
    UPDATE_PROGRESS(pgFaceStart);
    pgFaceOffset = PG_OUTPUT - PG_DB - 0.05f;   // save 0.04 for sorting

    // go through blocks and see which is solid; use solid blocks to generate faces
    for ( loc[X] = gSolidBox.min[X]; loc[X] <= gSolidBox.max[X]; loc[X]++ )
    {
        UPDATE_PROGRESS( pgFaceStart + pgFaceOffset*((float)(loc[X]-gSolidBox.min[X])/(float)(gSolidBox.max[X]-gSolidBox.min[X])));
        for ( loc[Z] = gSolidBox.min[Z]; loc[Z] <= gSolidBox.max[Z]; loc[Z]++ )
        {
            boxIndex = BOX_INDEX(loc[X],gSolidBox.min[Y],loc[Z]);
            for ( loc[Y] = gSolidBox.min[Y]; loc[Y] <= gSolidBox.max[Y]; loc[Y]++, boxIndex++ )
            {
                // if it's not air (everything too small has been turned into air)
                // then output it
                if ( gBoxData[boxIndex].type > BLOCK_AIR ) 
                {
                    // block is solid, may need to output some faces.
                    checkAndCreateFaces(boxIndex,loc);
                }
            }
        }
    }

    UPDATE_PROGRESS(pgFaceStart + pgFaceOffset);

    // now that we have the scale and world offset, and all vertices are now generated, transform all points to their proper locations
    for ( i = 0; i < gModel.vertexCount; i++ )
    {
        float *pt = (float *)gModel.vertices[i];
        float anchor[3];
        Vec2Op( anchor, =, gModel.vertices[i] );
        pt[X] = (float)(anchor[X] - gModel.center[X])*gModel.scale*gUnitsScale;
        pt[Y] = (float)(anchor[Y] - gModel.center[Y])*gModel.scale*gUnitsScale;
        pt[Z] = (float)(anchor[Z] - gModel.center[Z])*gModel.scale*gUnitsScale;

        // rotate location as needed
        rotateLocation( pt );
    }

    // If we are grouping by material (e.g., STL does not need this), then we need to sort by material
    if ( gOptions->exportFlags & EXPT_GROUP_BY_MATERIAL )
    {
        qsort_s(gModel.faceList,gModel.faceCount,sizeof(FaceRecord*),faceIdCompare,NULL);
    }
}
static int faceIdCompare( void* context, const void *str1, const void *str2)
{
    FaceRecord *f1;
    FaceRecord *f2;
    f1 = *(FaceRecord**)str1;
    f2 = *(FaceRecord**)str2;
    context;    // make a useless reference to the unused variable, to avoid C4100 warning
    if ( f1->type == f2->type )
    {
        // Not necessary, but...
        // Tie break is face loop starting vertex, so that data is
        // output with some coherence. May help mesh caching and memory access.
        // Also, the data just looks more tidy in the file.
        return ( (f1->faceIndex < f2->faceIndex) ? -1 : ((f1->faceIndex == f2->faceIndex)) ? 0 : 1 );
    }
    else return ( (f1->type < f2->type) ? -1 : 1 );
}


// return 0 if nothing solid in box
// Note that the dimensions are returned in floats, for later use for statistics
static int getDimensionsAndCount( Point dimensions )
{
    IPoint loc;
    int boxIndex;
    IBox bounds;
    int count = 0;
    VecScalar( bounds.min, =,  999999);
    VecScalar( bounds.max, =, -999999);

    // do full grid here, in case supports or other stuff gets added at the end
    //for ( loc[X] = 0; loc[X] < gBoxSize[X]; loc[X]++ )
    //{
    //    for ( loc[Z] = 0; loc[Z] < gBoxSize[Z]; loc[Z]++ )
    //    {
    //        boxIndex = BOX_INDEX(loc[X],0,loc[Z]);
    //        for ( loc[Y] = 0; loc[Y] < gBoxSize[Y]; loc[Y]++, boxIndex++ )

    // search air block, in case something got added around fringe, or some subtraction
    // pulled box in.
    for ( loc[X] = gAirBox.min[X]; loc[X] <= gAirBox.max[X]; loc[X]++ )
    {
        for ( loc[Z] = gAirBox.min[Z]; loc[Z] <= gAirBox.max[Z]; loc[Z]++ )
        {
            boxIndex = BOX_INDEX(loc[X],gAirBox.min[Y],loc[Z]);
            for ( loc[Y] = gAirBox.min[Y]; loc[Y] <= gAirBox.max[Y]; loc[Y]++, boxIndex++ )
            {
                // if it's not air, then it's valid - update bounds
                if ( gBoxData[boxIndex].type > BLOCK_AIR) 
                {
                    // block is solid, may need to output some faces.
                    addBounds( loc, &bounds );
                    count++;
                }
            }
        }
    }

    if ( gExportBillboards )
    {
        // add in billboard object count and bounds
        addBoundsToBounds( gModel.billboardBounds, &bounds );
    }

    // anything in the box?
    if ( bounds.min[X] > bounds.max[X] )
        return 0;

    // note conversion from int to float here
    Vec3Op( dimensions, =, 1.0f + (float)bounds.max, -, (float)bounds.min);
    return count;
}


static void rotateLocation( Point pt )
{
    float tempf;

    // check if rotation around an axis
    if ( !gOptions->pEFD->radioRotate0 )
    {
        // rotation will occur
        if ( gOptions->pEFD->radioRotate90 )
        {
            tempf = pt[X];
            pt[X] = -pt[Z];
            pt[Z] = tempf;
        }
        else if ( gOptions->pEFD->radioRotate180 )
        {
            pt[X] = -pt[X];
            pt[Z] = -pt[Z];
        }
        else
        {
            tempf = -pt[X];
            pt[X] = pt[Z];
            pt[Z] = tempf;
        }
    }

    // check if we rotate Y axis to be Z axis as up
    if ( gOptions->pEFD->chkMakeZUp[gOptions->pEFD->fileType] )
    {
        // Rotate +Y to +Z
        tempf = pt[Y];
        pt[Y] = -pt[Z];
        pt[Z] = tempf;
    }
}

// check if a solid block (assumed) is next to something that causes a face to be created
static void checkAndCreateFaces( int boxIndex, IPoint loc )
{
    int faceDirection;
    int neighborType;
    int type = gBoxData[boxIndex].type;
    int view3D;
    view3D = !(gOptions->exportFlags & EXPT_3DPRINT);

    // only solid blocks should be passed in here.
    assert(type != BLOCK_AIR);

    for ( faceDirection = 0; faceDirection < 6; faceDirection++ )
    {
        neighborType = gBoxData[boxIndex + gFaceOffset[faceDirection]].type;
        // if neighbor is air, or if we're outputting a model for viewing
        // (not printing) and it is transparent and our object is not transparent,
        // then output a face. This latter condition gives lakes bottoms.
        // TODO: do we care if two transparent objects are touching each other? (Ice & water?)
        // Right now water and ice touching will generate no faces, which I think is fine.
        // so, create a face?
        if ( checkMakeFace( type, neighborType, view3D ) )
        {
            // Air (or water, or portal) found next to solid block: time to write it out.
            // First write out any vertices that are needed (this may do nothing, if they're
            // already written out by previous faces doing output of the same vertices).
            saveVertices( boxIndex, faceDirection, loc );
            saveFaceLoop( boxIndex, faceDirection );
        }
    }
}

static int checkMakeFace( int type, int neighborType, int view3D )
{
    int makeFace = 0;
    if ( neighborType <= BLOCK_AIR )
    {
        makeFace = 1;
    } else if ( view3D )
    {
        // special cases for viewing

        // if the neighbor is semitransparent (glass, water, etc.)
        if (gBlockDefinitions[neighborType].alpha < 1.0f)
        {
            // and this object is a different
            // type - including ice on water, glass next to water, etc. - then output face
            if ( neighborType != type )
            {
                makeFace = 1;
            }
        }
        // look for blocks with cutouts next to them
        if ( (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES) &&
            (gBlockDefinitions[neighborType].flags & BLF_CUTOUTS) )
        {
            //(neighborType == BLOCK_LEAVES) ||
            //(neighborType == BLOCK_SUGAR_CANE) ||
            //(neighborType == BLOCK_CROPS) ||
            //(neighborType == BLOCK_NETHER_WART) ||
            //(neighborType == BLOCK_MONSTER_SPAWNER)||
            //(neighborType == BLOCK_COBWEB) ||
            //((neighborType == BLOCK_IRON_BARS) && (type != BLOCK_IRON_BARS)) ||
            //((neighborType == BLOCK_VINES) && (type != BLOCK_VINES) ) )

            if ( !((neighborType == BLOCK_GLASS) && (type == BLOCK_GLASS)) &&
                !((neighborType == BLOCK_GLASS_PANE) && (type == BLOCK_GLASS_PANE)) &&
                !((neighborType == BLOCK_VINES) && (type == BLOCK_VINES)) &&
                !((neighborType == BLOCK_IRON_BARS) && (type == BLOCK_IRON_BARS)) )
            {
                // anything neighboring a leaf block should be created, since leaves can be seen through. This does
                // include other leaf blocks. Do similar for iron bars and vines, but neighbors that are also iron bars
                // or vines don't generate faces in between (since we're faking bars & vines by putting them on blocks).
                // TODO if vines actually border some solid surface, vs. hanging down, they should get flatsided:
                // add as a composite, like ladder pretty much.
                makeFace = 1;
            }
        }
    }

    return makeFace;
}
// check if each face vertex has an index;
// if it doesn't, give it one and save out the vertex location itself
static void saveVertices( int boxIndex, int faceDirection, IPoint loc )
{
    int vertexIndex;
    int i;
    IPoint offset;
    float *pt;

    // four vertices to output, check that they exist
    for ( i = 0; i < 4; i++ )
    {
        Vec2Op( offset, =, gFaceToVertexOffset[faceDirection][i]);
        // gFaceToVertexOffset[6][4][3] gives the X,Y,Z offsets to the
        // vertex to be written for this box
        vertexIndex = boxIndex +
            offset[X] * gBoxSizeYZ +
            offset[Y] +
            offset[Z] * gBoxSize[Y];

        if ( vertexIndex < 0 || vertexIndex > gBoxSizeXYZ )
        {
            assert(0);
            return;
        }

        if ( gModel.vertexIndices[vertexIndex] == NO_INDEX_SET )
        {
            // need to give an index and write out vertex location
            checkVertexListSize();
            gModel.vertexIndices[vertexIndex] = gModel.vertexCount;
            pt = (float *)gModel.vertices[gModel.vertexCount];

            // for now, we use exactly the same coordinates as Minecraft does.
            //xOut = (float)(1-gWorld2BoxOffset[X] + xloc + xoff);
            //yOut = (float)(1-gWorld2BoxOffset[Y] + yloc + yoff);
            //zOut = (float)(1-gWorld2BoxOffset[Z] + zloc + zoff);
            // centered on origin, good for Blender import. I put Y==0, X & Z centered
            pt[X] = (float)(loc[X] + offset[X]);
            pt[Y] = (float)(loc[Y] + offset[Y]);
            pt[Z] = (float)(loc[Z] + offset[Z]);

            gModel.vertexCount++;
            assert( gModel.vertexCount <= gModel.vertexListSize );
        }
    }
}

static void saveFaceLoop( int boxIndex, int faceDirection )
{
    int i;
    FaceRecord *face;
    int dataVal = 0;
    unsigned char originalType = gBoxData[boxIndex].type;

    face = (FaceRecord *)malloc(sizeof(FaceRecord));

    // if  we sort, we want to keep faces in the order generated, which is
    // generally cache-coherent (and also just easier to view in the file)
    face->faceIndex = gModel.faceCount;

    // always the same normal, which directly corresponds to the normals[6] array in gModel
    face->normalIndex = faceDirection;

    // get four face indices for the four corners
    for ( i = 0; i < 4; i++ )
    {
        int vertexIndex;
        IPoint loc;

        Vec2Op( loc, =, gFaceToVertexOffset[faceDirection][i]);

        vertexIndex = boxIndex +
            loc[X] * gBoxSizeYZ +
            loc[Y] +
            loc[Z] * gBoxSize[Y];

        face->vertexIndex[i] = gModel.vertexIndices[vertexIndex];
    }

    if (gOptions->exportFlags & (EXPT_OUTPUT_MATERIALS|EXPT_OUTPUT_TEXTURE))
    {
        // for debugging: instead of outputting material, output group ID
        // as the material
        if (gOptions->exportFlags & EXPT_DEBUG_SHOW_GROUPS)
        {
            face->type = getMaterialUsingGroup(gBoxData[boxIndex].group);
        }
        else
        {
            // if we're doing FLATTOP compression, the topId for the block will
            // have been set to what is above the block before now (in filter).
            // If the value is not 0 (air), use that material instead
            int special = 0;
            if ( gBoxData[boxIndex].flatFlags )
            {
                switch ( faceDirection )
                {
				case DIRECTION_BLOCK_TOP:
					if ( gBoxData[boxIndex].flatFlags & FLAT_FACE_ABOVE )
					{
						face->type = gBoxData[boxIndex+1].origType;
						dataVal = gBoxData[boxIndex+1].data;    // this should still be intact, even if neighbor block is cleared to air
						special = 1;
					}
					break;
				case DIRECTION_BLOCK_BOTTOM:
					if ( gBoxData[boxIndex].flatFlags & FLAT_FACE_BELOW )
					{
						face->type = gBoxData[boxIndex-1].origType;
						dataVal = gBoxData[boxIndex-1].data;    // this should still be intact, even if neighbor block is cleared to air
						special = 1;
					}
					break;
                case DIRECTION_BLOCK_SIDE_LO_X:
                    if ( gBoxData[boxIndex].flatFlags & FLAT_FACE_LO_X )
                    {
                        face->type = gBoxData[boxIndex-gBoxSizeYZ].origType;
                        dataVal = gBoxData[boxIndex-gBoxSizeYZ].data;
                        special = 1;
                    }
                    break;
                case DIRECTION_BLOCK_SIDE_HI_X:
                    if ( gBoxData[boxIndex].flatFlags & FLAT_FACE_HI_X )
                    {
                        face->type = gBoxData[boxIndex+gBoxSizeYZ].origType;
                        dataVal = gBoxData[boxIndex+gBoxSizeYZ].data;
                        special = 1;
                    }
                    break;
                case DIRECTION_BLOCK_SIDE_LO_Z:
                    if ( gBoxData[boxIndex].flatFlags & FLAT_FACE_LO_Z )
                    {
                        face->type = gBoxData[boxIndex-gBoxSize[Y]].origType;
                        dataVal = gBoxData[boxIndex-gBoxSize[Y]].data;
                        special = 1;
                    }
                    break;
                case DIRECTION_BLOCK_SIDE_HI_Z:
                    if ( gBoxData[boxIndex].flatFlags & FLAT_FACE_HI_Z )
                    {
                        face->type = gBoxData[boxIndex+gBoxSize[Y]].origType;
                        dataVal = gBoxData[boxIndex+gBoxSize[Y]].data;
                        special = 1;
                    }
                    break;
                default:
                    // only direction left is down, and nothing gets merged with those faces
                    break;
                }
            }
            if ( !special )
            {
                face->type = originalType;
                dataVal = gBoxData[boxIndex].data;
            }
            else
            {
                // A flattening has happened.
                // Test just in case something's wedged
                if ( face->type == BLOCK_AIR )
                {
                    assert(0);
                    face->type = originalType;
                    return;
                }
            }
        }

        assert(face->type);
    }
    // else no material, so type is not needed

    if ( gOptions->exportFlags & EXPT_OUTPUT_TEXTURE )
    {
        // I guess we really don't need the swatch location returned; it's
        // main effect is to set the proper indices in the texture map itself
        // and note that the swatch is being used
        (int)getSwatch( face->type, dataVal, faceDirection, boxIndex, face->uvIndex );

        // if we're exporting the full texture, then a composite was used:
        // we might then actually want to use the original type
        //if ( Options.exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES )
        //{
        //    face->type = originalType;
        //}
    }

    checkFaceListSize();
    gModel.faceList[gModel.faceCount++] = face;
    // make sure we're not running off the edge, out of memory.
    // We don't need this memory when not writing out materials, as we instantly write out the faces
}

static int getMaterialUsingGroup( int groupID )
{
    int type = (NUM_BLOCKS - groupID) % NUM_BLOCKS;
    // if there are a huge number of groups, value could reach 0 or lower (?).
    if (type <= BLOCK_AIR )
    {
        type += NUM_BLOCKS-1;
    }
    return type;
}

#define SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, sx,sy, bx,by ) \
    if ( (faceDirection) == DIRECTION_BLOCK_BOTTOM )          \
        swatchLoc = SWATCH_XY_TO_INDEX( (bx), (by) );             \
    else if ( (faceDirection) != DIRECTION_BLOCK_TOP )        \
        swatchLoc = SWATCH_XY_TO_INDEX( (sx), (sy) );

#define SWATCH_SWITCH_SIDE( faceDirection, sx,sy )      \
    if ( ((faceDirection) != DIRECTION_BLOCK_BOTTOM) && ((faceDirection) != DIRECTION_BLOCK_TOP))      \
    swatchLoc = SWATCH_XY_TO_INDEX( (sx), (sy) );

// note that, for flattops and sides, the dataVal passed in is indeed the data value of the neighboring flattop being merged
static int getSwatch( int type, int dataVal, int faceDirection, int backgroundIndex, int uvIndices[4] )
{
    int swatchLoc,startUvIndex,col,head,bottom,angle,inside,outside,stem;
    int localIndices[4] = { 0, 1, 2, 3 };
    int xoff,xstart,dir,dirBit,frontLoc;
    // north is 0, east is 1, south is 2, west is 3
    static int faceRot[6] = { 0, 0, 1, 2, 0, 3 };


    // outputting swatches, or this block doesn't have a good official texture?
    if ( (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_SWATCHES) || 
        !( gBlockDefinitions[type].flags & BLF_IMAGE_TEXTURE) )
    {
        // use a solid color
        // TODO: note that this is not as fleshed out (e.g. I don't know if snow works) as full textures, which is the popular mode.
        swatchLoc = type;
        switch ( type )
        {
        case BLOCK_DOUBLE_SLAB:
        case BLOCK_STONE_SLAB:
            switch ( dataVal )
            {
            default:
                assert(0);
            case 6:
                // true stone? Doesn't seem to be different than case 0. See http://www.minecraftwiki.net/wiki/Block_ids#Slab_and_Double_Slab_material
            case 0:
                // no change, default stone "cement" BLOCK_STONE_SLAB
                break;
            case 1:
                // sandstone
                swatchLoc = BLOCK_SANDSTONE;
                break;
            case 2:
                // wooden
                swatchLoc = BLOCK_WOODEN_PLANKS;
                break;
            case 3:
                // cobblestone
                swatchLoc = BLOCK_COBBLESTONE;
                break;
            case 4:
                // brick
                swatchLoc = BLOCK_BRICK;
                break;
            case 5:
                // stone brick
                swatchLoc = BLOCK_STONE_BRICKS;
                break;
            }
            break;
        }
    }
    else
    {
        // use the textures:
        // go past the NUM_BLOCKS solid colors, use the txrX and txrY to find which to go to.
        swatchLoc = SWATCH_INDEX( gBlockDefinitions[type].txrX, gBlockDefinitions[type].txrY );

        // now do anything special needed for the particular type, data, and face direction
        switch ( type )
        {
        case BLOCK_GRASS:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 3,0, 2,0 );
            if ( faceDirection != DIRECTION_BLOCK_TOP && faceDirection != DIRECTION_BLOCK_BOTTOM )
            {
                // check if block above is snow; if so, use snow side tile; note we
                // check against the original type, since the snow block is likely to be flattened
                if ( gBoxData[backgroundIndex+1].origType == BLOCK_SNOW )
                {
                    swatchLoc = SWATCH_INDEX( 4, 4 );
                }
            }
            break;
        case BLOCK_DOUBLE_SLAB:
        case BLOCK_STONE_SLAB:
            switch ( dataVal )
            {
            default:
                assert(0);
            case 6:
                // true stone? Doesn't seem to be different than case 0. See http://www.minecraftwiki.net/wiki/Block_ids#Slab_and_Double_Slab_material
            case 0:
                // no change, default stone "cement" BLOCK_STONE_SLAB
                // might want to use stone side pattern
                SWATCH_SWITCH_SIDE( faceDirection, 5,0 );
                break;
            case 1:
                // sandstone
                swatchLoc = SWATCH_INDEX( gBlockDefinitions[BLOCK_SANDSTONE].txrX, gBlockDefinitions[BLOCK_SANDSTONE].txrY );
                break;
            case 2:
                // wooden
                swatchLoc = SWATCH_INDEX( gBlockDefinitions[BLOCK_WOODEN_PLANKS].txrX, gBlockDefinitions[BLOCK_WOODEN_PLANKS].txrY );
                break;
            case 3:
                // cobblestone
                swatchLoc = SWATCH_INDEX( gBlockDefinitions[BLOCK_COBBLESTONE].txrX, gBlockDefinitions[BLOCK_COBBLESTONE].txrY );
                break;
            case 4:
                // brick
                swatchLoc = SWATCH_INDEX( gBlockDefinitions[BLOCK_BRICK].txrX, gBlockDefinitions[BLOCK_BRICK].txrY );
                break;
            case 5:
                // stone brick
                swatchLoc = SWATCH_INDEX( gBlockDefinitions[BLOCK_STONE_BRICKS].txrX, gBlockDefinitions[BLOCK_STONE_BRICKS].txrY );
                break;
            }
            break;
        case BLOCK_LOG:
            // use data to figure out which side
            switch ( dataVal )
            {
            case 1: // spruce
                SWATCH_SWITCH_SIDE( faceDirection, 4,7 );
                break;
            case 2: // birch
                SWATCH_SWITCH_SIDE( faceDirection, 5,7 );
                break;
            case 3: // jungle
                if ( gJungleExists )
                {
                    SWATCH_SWITCH_SIDE( faceDirection, 9,9 );
                }
                else
                {
                    SWATCH_SWITCH_SIDE( faceDirection, 4,1 );
                }
                break;
            default: // normal log
                SWATCH_SWITCH_SIDE( faceDirection, 4,1 );
            }
            break;
        case BLOCK_LEAVES:
            // if we're using print export, go with the non-fancy leaves (not transparent)
            col = ( gOptions->exportFlags & EXPT_3DPRINT ) ? 5 : 4;
            switch ( dataVal & 0x3 )
            {
            case 0: // normal tree (oak)
            default:
                swatchLoc = SWATCH_INDEX( col, 3 );
                break;
            case 1: // Spruce leaves
                swatchLoc = SWATCH_INDEX( col, 8 );
                break;
            case 2: // birch - shaded differently in tile code, same as normal tree otherwise
                swatchLoc = SWATCH_INDEX( col, 3 );
                break;
            case 3: // jungle - switch depending on whether jungle exists!
                if ( gJungleExists )
                {
                    swatchLoc = SWATCH_INDEX( col, 12 );
                }
                else
                {
                    swatchLoc = SWATCH_INDEX( col, 3 );
                }
                break;
           }
            break;
        case BLOCK_DISPENSER:
        case BLOCK_FURNACE:
        case BLOCK_BURNING_FURNACE:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 13,2, 14,3 );
            if ( (faceDirection != DIRECTION_BLOCK_TOP) && (faceDirection != DIRECTION_BLOCK_BOTTOM) )
            {
                switch ( type )
                {
                case BLOCK_DISPENSER:
                    frontLoc = SWATCH_INDEX( 14, 2 );
                    break;
                case BLOCK_FURNACE:
                    frontLoc = SWATCH_INDEX( 12, 2 );
                    break;
                case BLOCK_BURNING_FURNACE:
                default:
                    frontLoc = SWATCH_INDEX( 13, 3 );
                    break;
                }
                switch ( dataVal )
                {
                case 2: // North
                    if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_Z )
                        swatchLoc = frontLoc;
                    break;
                case 3: // South, which is really the west side, i.e. HI_Z
                    if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_Z )
                        swatchLoc = frontLoc;
                    break;
                case 4: // West
                    if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_X )
                        swatchLoc = frontLoc;
                    break;
                case 5: // East
                default:
                    if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_X )
                        swatchLoc = frontLoc;
                    break;
                }
            }
            break;
        case BLOCK_POWERED_RAIL:
            if ( !(dataVal & 0x8) )
            {
                // unpowered rail
                swatchLoc = SWATCH_INDEX( 3, 10 );
            }
            // fall through:
        case BLOCK_DETECTOR_RAIL:
            // if not a normal rail, there are no curve bits, so mask off upper bit, which is
            // whether the rail is powered or not.
            dataVal &= 0x7;
            // fall through:
        case BLOCK_RAIL:
            // get data of track itself
            switch ( dataVal )
            {
            case 0:
            case 4:
            case 5:
                // horizontal straight (plus sloping)
                break;

            case 2:
            case 3:
                // sloping: if side, don't rotate!
                if ( faceDirection != DIRECTION_BLOCK_TOP )
                    break;
                // else continue through and rotate indices, it's on the ground
            case 1:
                // vertical straight (plus sloping)
                rotateIndices( localIndices, 90 );
                break;

            case 6:
            case 7:
            case 8:
            case 9:
                // curved piece
                assert(type == BLOCK_RAIL );
                swatchLoc = SWATCH_INDEX( 0, 7 );
                rotateIndices( localIndices, 90*(dataVal-6));
                break;
            }
            swatchLoc = getCompositeSwatch( swatchLoc, backgroundIndex, faceDirection, 0 );
            break;
        case BLOCK_SANDSTONE:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 0,12, 0,13 );
            break;
        case BLOCK_NOTEBLOCK:
            SWATCH_SWITCH_SIDE( faceDirection, 10,4 );
            break;
        case BLOCK_BED:
            if ( (faceDirection != DIRECTION_BLOCK_TOP) && (faceDirection != DIRECTION_BLOCK_BOTTOM) )
            {
                // side of bed - head or foot?
                if ( dataVal & 0x8 )
                {
                    // head of bed.
                    xoff = 1;
                    xstart = 7;
                }
                else
                {
                    xoff = -1;
                    xstart = 6;
                }

                // dataVal gives which way it points 7,9 and 8,9 vs. 5,9, 6,9
                switch ( ((dataVal & 0x3) - faceRot[faceDirection] + 4) % 4 )
                {
                case 0: // south
                    swatchLoc = SWATCH_INDEX( xstart, 9 );
                    break;
                case 1:
                    swatchLoc = SWATCH_INDEX( xoff+xstart, 9 );
                    flipIndicesLeftRight( localIndices );
                    break;
                case 2:
                    swatchLoc = SWATCH_INDEX( xstart, 9 );
                    flipIndicesLeftRight( localIndices );
                    break;
                case 3:
                    swatchLoc = SWATCH_INDEX( xoff + xstart, 9 );
                    break;
                default:
                    if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_X )
                        swatchLoc = SWATCH_INDEX( 14, 2 );
                    break;
                }
            }
            else if (faceDirection == DIRECTION_BLOCK_TOP)
            {
                // top surface of bed
                // head or foot?
                if ( dataVal & 0x8 )
                {
                    // head
                    swatchLoc = SWATCH_INDEX( 7, 8 );
                }
                // rotate as needed (head and foot rotate the same)
                switch ( dataVal & 0x3 )
                {
                case 0: // south
                    rotateIndices( localIndices, 90 );
                    break;
                case 1: // west
                    rotateIndices( localIndices, 180 );
                    break;
                case 2: // north
                    rotateIndices( localIndices, 270 );
                    break;
                case 3: // east
                    rotateIndices( localIndices, 0 );
                    break;
                }
            }
            else
            {
                // bottom of bed is always wood
                swatchLoc = SWATCH_INDEX( 4, 0 );
            }
            break;
        case BLOCK_STICKY_PISTON:
        case BLOCK_PISTON:
            // 10,6 sticky head, 11,6 head, 12,6 side, 13,6 bottom, 14,6 extended top
            head = bottom = 0;
            dir = dataVal & 7;
            angle = 0;
            switch ( dir )
            {
            case 0: // pointing down
            case 1: // pointing up
                if ( faceDirection == DIRECTION_BLOCK_BOTTOM )
                {
                    head = 1 - dir;
                    bottom = dir;
                }
                else if ( faceDirection == DIRECTION_BLOCK_TOP )
                {
                    head = dir;
                    bottom = 1 - dir;
                }
                // else it's a side, and since sides are aligned, rotate all 180 or none
                else
                {
                    angle=180*(1-dir);
                }
                break;
            case 2: // pointing east (new north)
            case 3: // pointing west (new south)
                dirBit = dir - 2;
                if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_Z )
                {
                    head = 1 - dirBit;
                    bottom = dirBit;
                }
                else if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_Z )
                {
                    head = dirBit;
                    bottom = 1 - dirBit;
                }
                // else it's a side
                if ( ( faceDirection == DIRECTION_BLOCK_BOTTOM ) ||
                    ( faceDirection == DIRECTION_BLOCK_TOP ) )
                {
                    angle = dirBit*180;
                }
                else if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_X )
                {
                    angle = 90 + dirBit*180;
                }
                else
                {
                    angle = 270 + dirBit*180;
                }
                break;
            case 4: // pointing north (new west)
            case 5: // pointing south (new east)
                dirBit = dir - 4;
                if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_X )
                {
                    head = 1 - dirBit;
                    bottom = dirBit;
                }
                else if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_X )
                {
                    head = dirBit;
                    bottom = 1 - dirBit;
                }
                else if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_Z )
                {
                    angle = 270 + dirBit*180;
                }
                else if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_Z )
                {
                    angle = 90 + dirBit*180;
                }
                else
                {
                    angle = 270 + dirBit*180;
                }
                break;
            }
            // ok, now we know head vs. bottom vs. side & angle
            if ( head )
            {
                // sticky, extended, or regular?
                if ( dataVal & 8 )
                {
                    // silly, since it's covered...
                    //assert(0);  // should never get here if extended
                    swatchLoc = SWATCH_INDEX( 14, 6 );
                }
                else
                {
                    if ( type == BLOCK_STICKY_PISTON )
                    {
                        swatchLoc = SWATCH_INDEX( 10, 6 );
                    }
                    else
                    {
                        swatchLoc = SWATCH_INDEX( 11, 6 );
                    }
                }
            }
            else if ( bottom )
            {
                // easy!
                swatchLoc = SWATCH_INDEX( 13, 6 );
            }
            else
            {
                // side
                swatchLoc = SWATCH_INDEX( 12, 6 );
                rotateIndices( localIndices, angle );
            }
            break;
        case BLOCK_TNT:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 8,0, 10,0 );
            break;
        case BLOCK_BOOKSHELF:
            SWATCH_SWITCH_SIDE( faceDirection, 3,2 );
            break;
        case BLOCK_WOODEN_DOOR:
            // top half is default
            if ( (faceDirection == DIRECTION_BLOCK_TOP) ||
                (faceDirection == DIRECTION_BLOCK_BOTTOM) )
            {
                swatchLoc = SWATCH_INDEX( 4, 0 );
            }
            else if ( !(dataVal & 0x8) )
            {
                swatchLoc = SWATCH_INDEX( 1, 6 );
            }
            break;
        case BLOCK_IRON_DOOR:
            // top half is default
            if ( (faceDirection == DIRECTION_BLOCK_TOP) ||
                (faceDirection == DIRECTION_BLOCK_BOTTOM) )
            {
                swatchLoc = SWATCH_INDEX( 6, 1 );
            }
            else if ( !(dataVal & 0x8) )
            {
                swatchLoc = SWATCH_INDEX( 2, 6 );
            }
            break;
        case BLOCK_TORCH:
        case BLOCK_REDSTONE_TORCH_ON:
        case BLOCK_REDSTONE_TORCH_OFF:
            // is torch in middle of block?
            if ( dataVal == 5 )
            {
                // use the "from above" torch
                if ( type == BLOCK_TORCH )
                    swatchLoc = TORCH_TOP;
                else if ( type == BLOCK_REDSTONE_TORCH_ON )
                    swatchLoc = RS_TORCH_TOP_ON;
                if ( type == BLOCK_REDSTONE_TORCH_OFF )
                    swatchLoc = RS_TORCH_TOP_OFF;
            }
            swatchLoc = getCompositeSwatch( swatchLoc, backgroundIndex, faceDirection, 0 );
            break;
        case BLOCK_LEVER:
            angle = (dataVal & 0x8 ) ? 180 : 0;
            if ( (dataVal & 0x7) == 5 )
                angle += 180;
            else if ( (dataVal & 0x7) == 6 )
                angle += 90;
            swatchLoc = getCompositeSwatch( swatchLoc, backgroundIndex, faceDirection, angle );
            break;
        case BLOCK_CHEST:
        case BLOCK_LOCKED_CHEST:
            SWATCH_SWITCH_SIDE( faceDirection, 10,1 );
            switch ( dataVal )
            {
            case 3: // new south
                if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_Z )
                {
                    swatchLoc = SWATCH_INDEX( 11, 1 );
                    if ( gBoxData[backgroundIndex+gBoxSizeYZ].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 9, 2 );
                    }
                    else if ( gBoxData[backgroundIndex-gBoxSizeYZ].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 10, 2 );
                    }
                }
                else if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_Z )
                {
                    // back of chest, on possibly long face
                    // is neighbor to north a chest, too?
                    if ( gBoxData[backgroundIndex+gBoxSizeYZ].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 10, 3 );
                    }
                    else if ( gBoxData[backgroundIndex-gBoxSizeYZ].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 9, 3 );
                    }
                }
                break;
            case 4: // west
                if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_X )
                {
                    swatchLoc = SWATCH_INDEX( 11, 1 );
                    if ( gBoxData[backgroundIndex-gBoxSize[Y]].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 10, 2 );
                    }
                    else if ( gBoxData[backgroundIndex+gBoxSize[Y]].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 9, 2 );
                    }
                }
                else if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_X )
                {
                    // back of chest, on possibly long face
                    // is neighbor to north a chest, too?
                    if ( gBoxData[backgroundIndex-gBoxSize[Y]].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 9, 3 );
                    }
                    else if ( gBoxData[backgroundIndex+gBoxSize[Y]].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 10, 3 );
                    }
                }
                break;
            case 2: // north
                if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_Z )
                {
                    swatchLoc = SWATCH_INDEX( 11, 1 );
                    if ( gBoxData[backgroundIndex-gBoxSizeYZ].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 9, 2 );
                    }
                    else if ( gBoxData[backgroundIndex+gBoxSizeYZ].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 10, 2 );
                    }
                }
                else if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_Z )
                {
                    // back of chest, on possibly long face
                    // is neighbor to north a chest, too?
                    if ( gBoxData[backgroundIndex-gBoxSizeYZ].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 10, 3 );
                    }
                    else if ( gBoxData[backgroundIndex+gBoxSizeYZ].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 9, 3 );
                    }
                }
                break;
            case 5: // new east
                if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_X )
                {
                    swatchLoc = SWATCH_INDEX( 11, 1 );
                    if ( gBoxData[backgroundIndex+gBoxSize[Y]].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 10, 2 );
                    }
                    else if ( gBoxData[backgroundIndex-gBoxSize[Y]].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 9, 2 );
                    }
                }
                else if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_X )
                {
                    // back of chest, on possibly long face
                    // is neighbor to north a chest, too?
                    if ( gBoxData[backgroundIndex+gBoxSize[Y]].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 9, 3 );
                    }
                    else if ( gBoxData[backgroundIndex-gBoxSize[Y]].type == BLOCK_CHEST )
                    {
                        swatchLoc = SWATCH_INDEX( 10, 3 );
                    }
                }
                break;
            }
            break;
        case BLOCK_CRAFTING_TABLE:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 11,3, 4,0 );
            if ( ( faceDirection == DIRECTION_BLOCK_SIDE_LO_X ) || ( faceDirection == DIRECTION_BLOCK_SIDE_LO_Z ) )
            {
                SWATCH_SWITCH_SIDE( faceDirection, 12,3 );
            }
            break;
        case BLOCK_CACTUS:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 6,4, 7,4 );
            break;
        case BLOCK_PUMPKIN:
        case BLOCK_JACK_O_LANTERN:
            SWATCH_SWITCH_SIDE( faceDirection, 6,7 );
            xoff = ( type == BLOCK_PUMPKIN ) ? 7 : 8;
            if ( ( faceDirection != DIRECTION_BLOCK_TOP ) && ( faceDirection != DIRECTION_BLOCK_BOTTOM ) )
            {
                switch ( dataVal )
                {
                case 0: // south
                    if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_Z )
                    {
                        swatchLoc = SWATCH_INDEX( xoff, 7 );
                    }
                    break;
                case 1: // west
                    if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_X )
                    {
                        swatchLoc = SWATCH_INDEX( xoff, 7 );
                    }
                    break;
                case 2: // north
                    if ( faceDirection == DIRECTION_BLOCK_SIDE_LO_Z )
                    {
                        swatchLoc = SWATCH_INDEX( xoff, 7 );
                    }
                    break;
                case 3: // east
                    if ( faceDirection == DIRECTION_BLOCK_SIDE_HI_X )
                    {
                        swatchLoc = SWATCH_INDEX( xoff, 7 );
                    }
                    break;
                }
            }
            break;
        case BLOCK_JUKEBOX:
            SWATCH_SWITCH_SIDE( faceDirection, 10,4 );
            break;
        case BLOCK_CAKE:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 10,7, 12,7 );
            break;
        case BLOCK_REDSTONE_REPEATER_OFF:
        case BLOCK_REDSTONE_REPEATER_ON:
            switch ( dataVal & 0x3 )
            {
            case 0: // south
                rotateIndices( localIndices, 0 );
                break;
            case 1: // west
                rotateIndices( localIndices, 90 );
                break;
            case 2: // north
                rotateIndices( localIndices, 180 );
                break;
            case 3: // east
                rotateIndices( localIndices, 270 );
                break;
            }
            break;
        case BLOCK_REDSTONE_WIRE:
            angle = 0;
            if ( faceDirection == DIRECTION_BLOCK_TOP )
            {
                // flat wire
                switch( dataVal )
                {
                case 0x0:
                    // no connections, just a dot
                    swatchLoc = SWATCH_INDEX( 4, 11);
                    break;

                case FLAT_FACE_LO_X:
                case FLAT_FACE_HI_X:
                // one node, but it's a two-way in Minecraft: no single branch
                case FLAT_FACE_LO_X|FLAT_FACE_HI_X:
                    swatchLoc = SWATCH_INDEX( 5, 10 );
                    break;
                case FLAT_FACE_LO_Z:
                case FLAT_FACE_HI_Z:
                case FLAT_FACE_LO_Z|FLAT_FACE_HI_Z:
                    angle = 90;
                    swatchLoc = SWATCH_INDEX( 5, 10 );
                    break;

                // angled 2 wire:
                case FLAT_FACE_LO_X|FLAT_FACE_LO_Z:
                    angle = 270;
                    swatchLoc = REDSTONE_WIRE_ANGLED_2;
                    break;
                case FLAT_FACE_LO_Z|FLAT_FACE_HI_X:
                    angle = 0;
                    swatchLoc = REDSTONE_WIRE_ANGLED_2;
                    break;
                case FLAT_FACE_HI_X|FLAT_FACE_HI_Z:
                    angle = 90;
                    swatchLoc = REDSTONE_WIRE_ANGLED_2;
                    break;
                case FLAT_FACE_HI_Z|FLAT_FACE_LO_X:
                    angle = 180;
                    swatchLoc = REDSTONE_WIRE_ANGLED_2;
                    break;

                // 3 wire
                case FLAT_FACE_LO_X|FLAT_FACE_LO_Z|FLAT_FACE_HI_X:
                    angle = 270;
                    swatchLoc = REDSTONE_WIRE_3;
                    break;
                case FLAT_FACE_LO_Z|FLAT_FACE_HI_X|FLAT_FACE_HI_Z:
                    angle = 0;
                    swatchLoc = REDSTONE_WIRE_3;
                    break;
                case FLAT_FACE_HI_X|FLAT_FACE_HI_Z|FLAT_FACE_LO_X:
                    angle = 90;
                    swatchLoc = REDSTONE_WIRE_3;
                    break;
                case FLAT_FACE_HI_Z|FLAT_FACE_LO_X|FLAT_FACE_LO_Z:
                    angle = 180;
                    swatchLoc = REDSTONE_WIRE_3;
                    break;

                default:
                    assert(0);
                case FLAT_FACE_LO_X|FLAT_FACE_LO_Z|FLAT_FACE_HI_X|FLAT_FACE_HI_Z:
                    // easy: 4 way, it's already done
                    break;
                }
            }
            else
            {
                // vertical wire
                SWATCH_SWITCH_SIDE( faceDirection, 5,10 );
                angle = 270;
            }
            swatchLoc = getCompositeSwatch( swatchLoc, backgroundIndex, faceDirection, angle );
            break;
        case BLOCK_STONE_BRICKS:
            switch ( dataVal )
            {
            default:
                assert(0);
            case 0:
                // no change
                break;
            case 1:
                // mossy
                swatchLoc = SWATCH_INDEX( 4, 6 );
                break;
            case 2:
                // cracked
                swatchLoc = SWATCH_INDEX( 5, 6 );
                break;
            }
            break;
        case BLOCK_TRAPDOOR:
        case BLOCK_LADDER:
        case BLOCK_LILY_PAD:
            swatchLoc = getCompositeSwatch( swatchLoc, backgroundIndex, faceDirection, 0 );
            break;
		case BLOCK_VINES:
			// special case (and I'm still not sure about this), if background is air, then
			// just use the default vine, whatever it is
			if ( gBoxData[backgroundIndex].type == BLOCK_AIR || gBoxData[backgroundIndex].type == BLOCK_VINES )
			{
				swatchLoc = SWATCH_INDEX( 15, 8 );
			}
			else
			{
				swatchLoc = getCompositeSwatch( swatchLoc, backgroundIndex, faceDirection, 0 );
			}
			break;
        case BLOCK_HIDDEN_SILVERFISH:
            switch ( dataVal )
            {
            case 0:
            default:
                // default
                break;
            case 1: // cobblestone
                swatchLoc = SWATCH_INDEX( 0, 1 );
                break;
            case 2: // stone brick
                swatchLoc = SWATCH_INDEX( 6, 3 );
                break;
            }
            break;
        case BLOCK_BROWN_MUSHROOM:
        case BLOCK_RED_MUSHROOM:
            inside = SWATCH_INDEX( 14, 8 );
            outside = SWATCH_INDEX( (type == BLOCK_BROWN_MUSHROOM) ? 14 : 13, 7 );
            stem = SWATCH_INDEX( 13, 8 );
            swatchLoc = inside;
            switch ( dataVal )
            {
            case 0: //0	 Fleshy piece	 Pores on all sides
                // done above, it's the default: swatchLoc = inside;
                break;
            case 1: //1	 Corner piece	 Cap texture on top, West and North
                if ( (faceDirection == DIRECTION_BLOCK_TOP) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_LO_X) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_LO_Z) )
                {
                    swatchLoc = outside;
                }
                break;
            case 2: //2	 Side piece	 Cap texture on top and North
                if ( (faceDirection == DIRECTION_BLOCK_TOP) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_LO_X) )
                {
                    swatchLoc = outside;
                }
                break;
            case 3: //3	 Corner piece	 Cap texture on top, North and East
                if ( (faceDirection == DIRECTION_BLOCK_TOP) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_HI_X) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_LO_Z) )
                {
                    swatchLoc = outside;
                }
                break;
            case 4: //4	 Side piece	 Cap texture on top and West
                if ( (faceDirection == DIRECTION_BLOCK_TOP) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_LO_X) )
                {
                    swatchLoc = outside;
                }
                break;
            case 5: //5	 Top piece	 Cap texture on top
                if ( faceDirection == DIRECTION_BLOCK_TOP )
                {
                    swatchLoc = outside;
                }
                break;
            case 6: //6	 Side piece	 Cap texture on top and East
                if ( (faceDirection == DIRECTION_BLOCK_TOP) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_HI_X) )
                {
                    swatchLoc = outside;
                }
                break;
            case 7: //7	 Corner piece	 Cap texture on top, South and West
                if ( (faceDirection == DIRECTION_BLOCK_TOP) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_LO_X) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_HI_Z) )
                {
                    swatchLoc = outside;
                }
                break;
            case 8: //8	 Side piece	 Cap texture on top and South
                if ( (faceDirection == DIRECTION_BLOCK_TOP) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_HI_Z) )
                {
                    swatchLoc = outside;
                }
                break;
            case 9: //9	 Corner piece	 Cap texture on top, East and South
                if ( (faceDirection == DIRECTION_BLOCK_TOP) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_HI_X) ||
                    (faceDirection == DIRECTION_BLOCK_SIDE_HI_Z) )
                {
                    swatchLoc = outside;
                }
                break;
            case 10: //10	 Stem piece	 Stem texture on all four sides, pores on top and bottom
                swatchLoc = inside;
                SWATCH_SWITCH_SIDE( faceDirection, 13,8 );
                break;
            }
            break;
        case BLOCK_MELON:
            SWATCH_SWITCH_SIDE( faceDirection, 8,8 );
            break;
        case BLOCK_MYCELIUM:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 13,4, 2,0 );
            break;
        case BLOCK_ENCHANTMENT_TABLE:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 6,11, 7,11 );
            break;
        case BLOCK_BREWING_STAND:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 13,9, 12,9 );
            break;
        case BLOCK_CAULDRON:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 10,9, 11,9 );
            break;
        case BLOCK_END_PORTAL_FRAME:
            SWATCH_SWITCH_SIDE_BOTTOM( faceDirection, 15,9, 15,10 );
            break;
        }
    }

    // flag UVs as being used for this swatch, so that the actual four UV values
    // are saved.
    saveTextureUVs( swatchLoc, type );

    // get four UV texture vertices, based on type of block
    startUvIndex = 4*gModel.uvSwatchToIndex[swatchLoc];
    assert(startUvIndex>=0);
    
    if ( gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES )
    {
        // let the adjustments begin!
        if ( faceDirection == DIRECTION_BLOCK_BOTTOM )
        {
            // -Y is unique: the textures are actually flipped! 2,1,4,3
            uvIndices[0] = startUvIndex + localIndices[1];
            uvIndices[1] = startUvIndex + localIndices[0];
            uvIndices[2] = startUvIndex + localIndices[3];
            uvIndices[3] = startUvIndex + localIndices[2];
        }
        else
        {
            // Normal case (note that pistons go through this, too, but we compensate
            // earlier - easier than testing for this special case here)
            uvIndices[0] = startUvIndex + localIndices[0];
            uvIndices[1] = startUvIndex + localIndices[1];
            uvIndices[2] = startUvIndex + localIndices[2];
            uvIndices[3] = startUvIndex + localIndices[3];
        }
    }
    else
    {
        // easy: just use the color swatch
        uvIndices[0] = startUvIndex;
        uvIndices[1] = startUvIndex+1;
        uvIndices[2] = startUvIndex+2;
        uvIndices[3] = startUvIndex+3;
    }
 
    return swatchLoc;
}

// if we have a cutout swatch, we need to also add the swatch to the library
// Note that currently we don't rotate the background texture properly compared to the swatch itself - tough.
static int getCompositeSwatch( int swatchLoc, int backgroundIndex, int faceDirection, int angle )
{
    // does library have type/backgroundType desired?
    SwatchComposite *pSwatch = gModel.swatchCompositeList;
    int dummy[4];
    int backgroundSwatchLoc = getSwatch( gBoxData[backgroundIndex].type, gBoxData[backgroundIndex].data, faceDirection, 0, dummy );

    while ( pSwatch )
    {
        if ( (pSwatch->swatchLoc == swatchLoc) && 
            (pSwatch->angle == angle) &&
            (pSwatch->backgroundSwatchLoc == backgroundSwatchLoc) )
            return pSwatch->compositeSwatchLoc;

        pSwatch = pSwatch->next;
    }

    // can't find swatch, so see if we can make it
    if ( gModel.swatchCount >= gModel.swatchListSize )
    {
        // no room for more swatches. Plan B: find the default swatch for this type
        pSwatch = gModel.swatchCompositeList;
        while ( pSwatch )
        {
            // any port in a storm: use the first match found (there should always be one)
            if ( pSwatch->swatchLoc == swatchLoc )
                return pSwatch->compositeSwatchLoc;

            pSwatch = pSwatch->next;
        }
        assert(0);  // you need to create a default type/backgroundType swatch for this type
        return 0;
    }

    // make it
    return createCompositeSwatch( swatchLoc, backgroundSwatchLoc, angle );
}

// take the cutout at swatchLoc, with given type and subtype, and put it over the background at background index, for faceDirection
static int createCompositeSwatch( int swatchLoc, int backgroundSwatchLoc, int angle )
{
    int scol,srow,dcol,drow;
    SwatchComposite *pSwatch = (SwatchComposite *)malloc(sizeof(SwatchComposite));
    pSwatch->swatchLoc = swatchLoc;
    pSwatch->angle = angle;
    pSwatch->backgroundSwatchLoc = backgroundSwatchLoc;
    pSwatch->next = NULL;

    // The real work: take the swatch and composite it over the background swatch - this should never recurse! If it does, then a background is a composite, too,
    // which should not happen

    if ( angle != 0 )
    {
        // rotate swatch by angle, then apply to background.
        SWATCH_TO_COL_ROW( swatchLoc, scol, srow );
        // set to new, temporary location for the swatch, for compositing
        swatchLoc = SWATCH_WORKSPACE;
        SWATCH_TO_COL_ROW( swatchLoc, dcol, drow );
        rotatePNGTile(gModel.pPNGtexture, dcol, drow, scol, srow, angle, gModel.swatchSize );
    }

    // where result goes, over, under
    compositePNGSwatches( gModel.pPNGtexture, gModel.swatchCount, swatchLoc, backgroundSwatchLoc, gModel.swatchSize, gModel.swatchesPerRow, 0 );
    pSwatch->compositeSwatchLoc = gModel.swatchCount++;

    // link it up
    if ( gModel.swatchCompositeListEnd != NULL )
    {
        gModel.swatchCompositeListEnd->next = pSwatch;
        gModel.swatchCompositeListEnd = pSwatch;
    }
    else
    {
        // first swatch ever added to list
        gModel.swatchCompositeList = gModel.swatchCompositeListEnd = pSwatch;
    }

    return pSwatch->compositeSwatchLoc;
}


// 0,0 1,0 1,1 0,1 is 0,1,2,3 
static void flipIndicesLeftRight( int localIndices[4] )
{
    int tmp;
    tmp = localIndices[0];
    localIndices[0] = localIndices[1];
    localIndices[1] = tmp;
    tmp = localIndices[2];
    localIndices[2] = localIndices[3];
    localIndices[3] = tmp;
}

static void rotateIndices( int localIndices[4], int angle )
{
    switch ( (angle+360)%360 )
    {
    case 0: // do nothing
        break;
    case 90: // rotate 90 clockwise - you rotate the UV's counterclockwise
        localIndices[0] = 1;
        localIndices[1] = 2;
        localIndices[2] = 3;
        localIndices[3] = 0;
        break;
    case 180:
        localIndices[0] = 2;
        localIndices[1] = 3;
        localIndices[2] = 0;
        localIndices[3] = 1;
        break;
    case 270:
        localIndices[0] = 3;
        localIndices[1] = 0;
        localIndices[2] = 1;
        localIndices[3] = 2;
        break;
    }
}

static void saveTextureUVs( int swatchLoc, int type )
{
    // has this UV location been set before? That is, for this particular face.
    // Note that if we have to rotate or flip 
    if ( gModel.uvSwatchToIndex[swatchLoc] < 0 )
    {
        // could count users, not sure if there's value there
        gModel.uvIndexToSwatch[gModel.textureUsedCount] = swatchLoc;
        gModel.uvSwatchToIndex[swatchLoc] = gModel.textureUsedCount;
        gModel.uvSwatchToType[swatchLoc] = type;
        gModel.textureUsedCount++;
    }
}

static void freeModel(Model *pModel)
{
    if ( pModel->vertices )
    {
        free(pModel->vertices);
        pModel->vertices = NULL;
    }
    if ( pModel->vertexIndices )
    {
        free(pModel->vertexIndices);
        pModel->vertexIndices = NULL;
    }
    if ( pModel->faceList )
    {
        int i;
        for ( i = 0; i < pModel->faceCount; i++ )
        {
            if ( i % 1000 == 0 )
                UPDATE_PROGRESS( PG_CLEANUP + 0.8f*(PG_END-PG_CLEANUP)*((float)i/(float)pModel->faceCount));
            assert( pModel->faceList[i] );
            free( pModel->faceList[i] );
        }
        free(pModel->faceList);
        pModel->faceList = NULL;
        pModel->faceSize = 0;
    }

    if ( gModel.swatchCompositeList )
    {
        SwatchComposite *pSwatch = gModel.swatchCompositeList;
        do
        {
            SwatchComposite *pNextSwatch;
            pNextSwatch = pSwatch->next;
            free(pSwatch);
            pSwatch = pNextSwatch;
        } while ( pSwatch );
    }
}

// return 0 if no write
static int writeOBJBox( const wchar_t *world, IBox *worldBox )
{
    // set to 1 if you want absolute (positive) indices used in the faces
    int absoluteIndices = (gOptions->exportFlags & EXPT_OUTPUT_NEUTRAL_MATERIAL) ? 1 : 0;

#ifdef WIN32
    DWORD br;
#endif
    wchar_t objFileNameWithSuffix[MAX_PATH];

    char outputString[MAX_PATH];
    char mtlName[MAX_PATH];
    const char *justWorldFileName;
    char justMtlFileName[MAX_PATH];

    int i, normalCount;

    int exportMaterials;

    int retCode = MW_NO_ERROR;

    char worldNameUnderlined[256];
    int prevType;

    FaceRecord *pFace;
    int outputFaceDirection;

    char worldChar[MAX_PATH];

    exportMaterials = gOptions->exportFlags & EXPT_OUTPUT_MATERIALS;

    concatFileName3(objFileNameWithSuffix,gOutputFilePath,gOutputFileRoot,L".obj");

    // create the Wavefront OBJ file
    //DeleteFile(objFileNameWithSuffix);
    gModelFile=PortaCreate(objFileNameWithSuffix);
    addOutputFilenameToList(objFileNameWithSuffix);
    if (gModelFile == INVALID_HANDLE_VALUE)
        return MW_CANNOT_CREATE_FILE;

    wcharToChar(world,worldChar);
    justWorldFileName = removePathChar(worldChar);

    sprintf_s(outputString,256,"# Wavefront OBJ file made by Mineways, http://mineways.com\n" );
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    retCode = writeStatistics( gModelFile, justWorldFileName, worldBox );
    if ( retCode >= MW_BEGIN_ERRORS )
        goto Exit;

    // If we use materials, say where the file is
    if ( exportMaterials )
    {
        sprintf_s(justMtlFileName,MAX_PATH,"%s.mtl",gOutputFileRootCleanChar);

        sprintf_s(outputString,256,"\nmtllib %s\n", justMtlFileName );
        WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));
    }

    // replace spaces with underscores for world name output
    strcpy_s(worldNameUnderlined,256,justWorldFileName);
    spacesToUnderlinesChar(worldNameUnderlined);

    // Object name
    sprintf_s(outputString,256,"\no %s__%d_%d_%d_to_%d_%d_%d\n", worldNameUnderlined,
        worldBox->min[X], worldBox->min[Y], worldBox->min[Z],
        worldBox->max[X], worldBox->max[Y], worldBox->max[Z] );
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    // write out normals, texture coordinates, vertices, and then faces grouped by material
    normalCount = gExportBillboards ? 18 : 6;
    for ( i = 0; i < normalCount; i++ )
    {
        sprintf_s(outputString,256,"vn %g %g %g\n", gModel.normals[i][0], gModel.normals[i][1], gModel.normals[i][2]);
        WERROR(PortaWrite(gModelFile, outputString, strlen(outputString)));
    }

    if ( gOptions->exportFlags & EXPT_OUTPUT_TEXTURE )
    {
        for ( i = 0; i < gModel.textureUsedCount; i++ )
        {
            retCode |= writeOBJTextureUVs(gModel.uvIndexToSwatch[i]);
            if (retCode >= MW_BEGIN_ERRORS)
                goto Exit;
        }
    }

    for ( i = 0; i < gModel.vertexCount; i++ )
    {
        if ( i % 1000 == 0 )
            UPDATE_PROGRESS( PG_OUTPUT + 0.5f*(PG_TEXTURE-PG_OUTPUT)*((float)i/(float)gModel.vertexCount));

        sprintf_s(outputString,256,"v %g %g %g\n", gModel.vertices[i][X], gModel.vertices[i][Y], gModel.vertices[i][Z] );
        WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));
    }

    //if ( exportMaterials && (gOptions->exportFlags & EXPT_OUTPUT_NEUTRAL_MATERIAL) )
    //{
    //    sprintf_s(outputString,256,"\ng world\nusemtl object_material\n");
    //    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));
    //}

    prevType = -1;
    for ( i = 0; i < gModel.faceCount; i++ )
    {
        if ( i % 1000 == 0 )
            UPDATE_PROGRESS( PG_OUTPUT + 0.5f*(PG_TEXTURE-PG_OUTPUT) + 0.5f*(PG_TEXTURE-PG_OUTPUT)*((float)i/(float)gModel.faceCount));

        if ( exportMaterials )
        {
            //// should there be just one single material in this OBJ file?
            //if ( !(gOptions->exportFlags & EXPT_OUTPUT_NEUTRAL_MATERIAL) )
            //{
                // did we reach a new material?
                if ( prevType != gModel.faceList[i]->type )
                {
                    prevType = gModel.faceList[i]->type;
                    // new ID encountered, so output it: material name, and group
                    // group isn't really required, but can be useful
                    strcpy_s(mtlName,256,gBlockDefinitions[prevType].name);

                    // substitute ' ' to '_'
                    spacesToUnderlinesChar( mtlName );
                    // g materialName - useful?
                    // usemtl materialName
                    sprintf_s(outputString,256,"\ng %s\nusemtl %s\n", mtlName, mtlName);
                    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

                    // note which material is to be output
                    gModel.mtlList[gModel.mtlCount++] = prevType;
                }
            //}
        }

        // output the actual face
        pFace = gModel.faceList[i];

        if ( absoluteIndices )
        {
            outputFaceDirection = pFace->normalIndex+1;
        }
        else
        {
            outputFaceDirection = pFace->normalIndex-normalCount;
        }

        // first, are there texture coordinates?
        if ( gOptions->exportFlags & EXPT_OUTPUT_TEXTURE )
        {
            if ( absoluteIndices )
            {
                sprintf_s(outputString,256,"f %d/%d/%d %d/%d/%d %d/%d/%d %d/%d/%d\n",
                    pFace->vertexIndex[0]+1, pFace->uvIndex[0]+1, outputFaceDirection,
                    pFace->vertexIndex[1]+1, pFace->uvIndex[1]+1, outputFaceDirection,
                    pFace->vertexIndex[2]+1, pFace->uvIndex[2]+1, outputFaceDirection,
                    pFace->vertexIndex[3]+1, pFace->uvIndex[3]+1, outputFaceDirection
                    );
            }
            else
            {
                sprintf_s(outputString,256,"f %d/%d/%d %d/%d/%d %d/%d/%d %d/%d/%d\n",
                    pFace->vertexIndex[0]-gModel.vertexCount, pFace->uvIndex[0]-4*gModel.textureUsedCount, outputFaceDirection,
                    pFace->vertexIndex[1]-gModel.vertexCount, pFace->uvIndex[1]-4*gModel.textureUsedCount, outputFaceDirection,
                    pFace->vertexIndex[2]-gModel.vertexCount, pFace->uvIndex[2]-4*gModel.textureUsedCount, outputFaceDirection,
                    pFace->vertexIndex[3]-gModel.vertexCount, pFace->uvIndex[3]-4*gModel.textureUsedCount, outputFaceDirection
                    );
            }
        }
        else
        {
            if ( absoluteIndices )
            {
                sprintf_s(outputString,256,"f %d//%d %d//%d %d//%d %d//%d\n",
                    pFace->vertexIndex[0]+1, outputFaceDirection,
                    pFace->vertexIndex[1]+1, outputFaceDirection,
                    pFace->vertexIndex[2]+1, outputFaceDirection,
                    pFace->vertexIndex[3]+1, outputFaceDirection
                    );
            }
            else
            {
                sprintf_s(outputString,256,"f %d//%d %d//%d %d//%d %d//%d\n",
                    pFace->vertexIndex[0]-gModel.vertexCount, outputFaceDirection,
                    pFace->vertexIndex[1]-gModel.vertexCount, outputFaceDirection,
                    pFace->vertexIndex[2]-gModel.vertexCount, outputFaceDirection,
                    pFace->vertexIndex[3]-gModel.vertexCount, outputFaceDirection
                    );
            }
        }
        WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));
    }

    Exit:
    PortaClose(gModelFile);

    // should we call it a day here?
    if ( retCode >= MW_BEGIN_ERRORS )
        return retCode;

    // write materials file
    if ( exportMaterials )
    {
        // write material file
        retCode |= writeOBJMtlFile();
        if ( retCode >= MW_BEGIN_ERRORS )
            return retCode;
    }

    return retCode;
}

static void getTextureBounds( int swatchLoc, float *umin, float *umax, float *vmin, float *vmax )
{
    int row,col;

    // get the swatch location
    SWATCH_TO_COL_ROW( swatchLoc, col, row );

    *umin = (float)col * gModel.textureUVPerSwatch + gModel.invTextureResolution;
    *umax = (float)(col+1) * gModel.textureUVPerSwatch - gModel.invTextureResolution;
    *vmin = 1.0f - ((float)row * gModel.textureUVPerSwatch + gModel.invTextureResolution);
    *vmax = 1.0f - ((float)(row+1) * gModel.textureUVPerSwatch - gModel.invTextureResolution);
}

static int writeOBJTextureUVs( int type )
{
#ifdef WIN32
    DWORD br;
#endif
    char outputString[1024];
    float umin, umax, vmin, vmax;

    assert( gModel.uvSwatchToIndex[type] >= 0 );
    assert( type < NUM_MAX_SWATCHES );
    assert( gModel.uvSwatchToType[type] < NUM_BLOCKS );
 
    // sample the center of the swatch, so that there's no leakage.
    // Really, it's overkill that we're using 18x18 color swatches and
    // sampling the 16x16 center, but it makes it nice for people wanting to
    // edit the texture directly and clears the way for true texture output.
    getTextureBounds( type, &umin, &umax, &vmin, &vmax );

    sprintf_s(outputString,1024,"# texture swatch: %s\nvt %g %g\nvt %g %g\nvt %g %g\nvt %g %g\n",
        gBlockDefinitions[gModel.uvSwatchToType[type]].name,
        umin,vmax,
        umax,vmax,
        umax,vmin,
        umin,vmin);
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    return 1;
}


static int writeOBJMtlFile()
{
#ifdef WIN32
    DWORD br;
#endif
    int type,i;
    wchar_t mtlFileName[MAX_PATH];
    char mtlName[MAX_PATH];
    char outputString[512];
    double fRed,fGreen,fBlue;
    double ka, kd, ke;
    double alpha;

    char textureRGB[MAX_PATH];
    char textureRGBA[MAX_PATH];
    char textureAlpha[MAX_PATH];

    concatFileName3(mtlFileName, gOutputFilePath, gOutputFileRootClean, L".mtl");

    gMtlFile=PortaCreate(mtlFileName);
    addOutputFilenameToList(mtlFileName);
    if (gMtlFile == INVALID_HANDLE_VALUE)
        return MW_CANNOT_CREATE_FILE;

    sprintf_s(outputString,512,"Wavefront OBJ material file\n# Contains %d materials\n\n", gModel.mtlCount);
    WERROR(PortaWrite(gMtlFile, outputString, strlen(outputString) ));

    if (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE )
    {
        // Write them out! We need three texture file names: -RGB, -RGBA, -Alpha.
        // The RGB/RGBA split is needed for fast previewers like G3D to gain additional speed
        // The all-alpha image is needed for various renderers to properly read cutouts, since map_d is poorly defined
        sprintf_s(textureRGB,MAX_PATH,"%s%s.png",gOutputFileRootCleanChar,PNG_RGB_SUFFIXCHAR);
        sprintf_s(textureRGBA,MAX_PATH,"%s%s.png",gOutputFileRootCleanChar,PNG_RGBA_SUFFIXCHAR);
        sprintf_s(textureAlpha,MAX_PATH,"%s%s.png",gOutputFileRootCleanChar,PNG_ALPHA_SUFFIXCHAR);
    }

    for ( i = 0; i < gModel.mtlCount; i++ )
    {
        char tfString[256];
        char mapdString[256];
        char keString[256];
        char *typeTextureFileName;
        type = gModel.mtlList[i];

        // print header: material name, and group
        // group isn't really required, but can be useful
        strcpy_s(mtlName,256,gBlockDefinitions[type].name);
        spacesToUnderlinesChar(mtlName);

        // if we want a neutral material, set to white
        if (gOptions->exportFlags & EXPT_OUTPUT_NEUTRAL_MATERIAL)
        {
            fRed = fGreen = fBlue = 1.0f;
        }
        else
        {
            // use color in file, which is nice for previewing. Blender and 3DS MAX like this, for example
            fRed = (gBlockDefinitions[type].color >> 16)/255.0f;
            fGreen = ((gBlockDefinitions[type].color >> 8) & 0xff)/255.0f;
            fBlue = (gBlockDefinitions[type].color & 0xff)/255.0f;
        }

        // good for blender:
        ka = 0.2;
        kd = 1.0;
        // looks bad in G3D, but if you want to emit light from various emitters - torches, etc. - make it non-zero.
        // If nothing else you can identify emitters when you see 'Ke 0 0 0' in the material.
        ke = 0.0f;
        // 3d printers cannot print semitransparent surfaces, so set alpha to 1.0 so what you preview
        // is what you get. TODO Should we turn off alpha for textures, as the textures themselves will have alpha in them - this is
        // in case the model viewer tries to multiply alpha by texture; also, VRML just has one material for textures, generic.
        // Really, we could have no colors at all when textures are output, but the colors are useful for previewers that
        // do not support textures (e.g. Blender).
        //alpha = ( (gOptions->exportFlags & EXPT_3DPRINT) || (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE)) ? 1.0f : gBlockDefinitions[type].alpha;
        // Well, hmmm, alpha is useful in previewing (no textures displayed), at least for OBJ files
        // alpha = (gOptions->exportFlags & EXPT_3DPRINT) ? 1.0f : gBlockDefinitions[type].alpha;
        alpha = gBlockDefinitions[type].alpha;
        if (gOptions->exportFlags & EXPT_DEBUG_SHOW_GROUPS)
        {
            // if showing groups, make the alpha of the largest group transparent
            if ( gDebugTransparentType == type )
            {
                alpha = DEBUG_DISPLAY_ALPHA;
            }
			else
			{
				alpha = 1.0f;
			}
        }
        else if ( gOptions->exportFlags & EXPT_3DPRINT )
        {
            // for 3d printing, alpha is always 1.0
            alpha = 1.0f;
        }
        // if semitransparent, and a truly transparent thing, then alpha is used; otherwise it's probably a cutout and the overall alpha should be 1.0f
        if ( alpha < 1.0f && (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES) && !(gBlockDefinitions[type].flags & BLF_TRANSPARENT) )
        {
            alpha = 1.0f;
        }

        if ( alpha < 1.0f )
        {
            // semitransparent block, such as water
            gModel.usesRGBA = 1;
            gModel.usesAlpha = 1;
            sprintf_s(tfString,256,"Tf %g %g %g\n", 1.0f-(float)(fRed*alpha), 1.0f-(float)(fGreen*alpha), 1.0f-(float)(fBlue*alpha) );
        }
        else
        {
            tfString[0] = '\0';
        }

        // export map_d only if CUTOUTS.
        if (!(gOptions->exportFlags & EXPT_3DPRINT) && (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES) && (alpha < 1.0 || (gBlockDefinitions[type].flags & BLF_CUTOUTS)) )
        {
            // cutouts or alpha
            gModel.usesRGBA = 1;
            gModel.usesAlpha = 1;
            typeTextureFileName = textureRGBA;
            sprintf_s(mapdString,256,"map_d %s\n", textureAlpha );
        }
        else
        {
            gModel.usesRGB = 1;
            typeTextureFileName = textureRGB;
            mapdString[0] = '\0';
        }
        if (!(gOptions->exportFlags & EXPT_3DPRINT) && (gBlockDefinitions[type].flags & BLF_EMITTER) )
        {
            // emitter
            sprintf_s(keString,256,"Ke %g %g %g\n", (float)(fRed*ke), (float)(fGreen*ke), (float)(fBlue*ke) );
        }
        else
        {
            keString[0] = '\0';
        }

        if (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE)
        {
            sprintf_s(outputString,512,
                "newmtl %s\n"
                "Ns 0\n"	// specular highlight power
                "Ka %g %g %g\n"
                "Kd %g %g %g\n"
                "Ks 0 0 0\n"
                "%s"
                "map_Ka %s\n"
                "map_Kd %s\n"
                "%s" // map_d, if there's a cutout
                // "Ni 1.0\n" - Blender likes to output this - no idea what it is
                "illum %d\n"
                "# d %g\n"	// some use Tr here - Blender likes "d"
                "# Tr %g\n"	// we put both, in hopes of helping both types of importer; comment out one, as 3DS MAX doesn't like it
                "%s\n"	//Tf, if needed
                ,
                // colors are premultiplied by alpha, Wavefront OBJ doesn't want that
                mtlName,
                (float)(fRed*ka), (float)(fGreen*ka), (float)(fBlue*ka), 
                (float)(fRed*kd), (float)(fGreen*kd), (float)(fBlue*kd),
                keString,
                typeTextureFileName, typeTextureFileName, mapdString,
                (alpha < 1.0f ? 4 : 2),
                (float)(alpha),
                (float)(alpha),
                tfString);
        }
        else
        {
            sprintf_s(outputString,512,
                "newmtl %s\n"
                "Ns 0\n"	// specular highlight power
                "Ka %g %g %g\n"
                "Kd %g %g %g\n"
                "Ks 0 0 0\n"
                "%s"
                // "Ni 1.0\n" - Blender likes to output this - no idea what it is
                "illum %d\n"
                "# d %g\n"	// some use Tr here - Blender likes "d"
                "# Tr %g\n"	// we put both, in hopes of helping both types of importer; comment out one, as 3DS MAX doesn't like it
                "%s\n"	//Tf, if needed
                ,
                // colors are premultiplied by alpha, Wavefront OBJ doesn't want that
                mtlName,
                (float)(fRed*ka), (float)(fGreen*ka), (float)(fBlue*ka), 
                (float)(fRed*kd), (float)(fGreen*kd), (float)(fBlue*kd),
                keString,
                (alpha < 1.0f ? 4 : 2),
                (float)(alpha),
                (float)(alpha),
                tfString);
        }
        WERROR(PortaWrite(gMtlFile, outputString, strlen(outputString) ));
    }

    PortaClose(gMtlFile);

    return MW_NO_ERROR;
}

static int createBaseMaterialTexture()
{
    int row,col,srow,scol;
    int keepGoing;
    int i,adj,idx;
	//int faTableCount;
    unsigned char r,g,b,a;
    unsigned int color;
    int useTextureImage;
    int addNoise = 0;

    progimage_info *mainprog;

    // all the blocks that need premultiplication by a color.
    // See http://www.minecraftwiki.net/wiki/File:TerrainGuide.png
#define MULT_TABLE_SIZE 19
    static TypeTile multTable[MULT_TABLE_SIZE] = {
        { BLOCK_GRASS /* grass */, 0,0, {1.0f,1.0f,1.0f} },
        { BLOCK_GRASS /* fancy grass? */, 6,2, {1.0f,1.0f,1.0f} },
        { BLOCK_TALL_GRASS /* tall grass */, 7,2, {1.0f,1.0f,1.0f} },
        { BLOCK_GRASS /* grass? */, 8,2, {1.0f,1.0f,1.0f} },
        { BLOCK_LEAVES /* leaves, fancy */, 4,3, {1.0f,1.0f,1.0f} },
        { BLOCK_LEAVES /* leaves, fast */, 5,3, {1.0f,1.0f,1.0f} },
        { BLOCK_TALL_GRASS /* fern */, 8,3, {1.0f,1.0f,1.0f} },
        { BLOCK_LILY_PAD /* lily pad */, 12,4, {1.0f,1.0f,1.0f} },
        { BLOCK_PUMPKIN_STEM /* pumpkin stem */, 15,6, {1.0f,1.0f,1.0f} },
        { BLOCK_PUMPKIN_STEM /* pumpkin stem, dead */, 15,7, {1.0f,1.0f,1.0f} }, /* probably need a different color */
        { BLOCK_VINES /* vines */, 15,8, {1.0f,1.0f,1.0f} },
        { BLOCK_LEAVES /* pine leaves fancy */, 4,8, {1.0f,1.0f,1.0f} }, //{57.0f/89.0f,90.0f/116.0f,57.0f/59.0f} },
        { BLOCK_LEAVES /* pine leaves fast */, 5,8, {1.0f,1.0f,1.0f} }, //{57.0f/89.0f,90.0f/116.0f,57.0f/59.0f} },
        { BLOCK_REDSTONE_WIRE /* redstone wire */, 4,10, {1.0f,1.0f,1.0f} },
        { BLOCK_REDSTONE_WIRE /* redstone wire */, 5,10, {1.0f,1.0f,1.0f} },
        { BLOCK_REDSTONE_TORCH_ON /* redstone */, 4,11, {1.0f,1.0f,1.0f} },
        { BLOCK_REDSTONE_TORCH_ON /* redstone */, 5,11, {1.0f,1.0f,1.0f} },
        { BLOCK_LEAVES /* jungle leaves, fancy */, 4,12, {1.0f,1.0f,1.0f} },
        { BLOCK_LEAVES /* jungle leaves, fast */, 5,12, {1.0f,1.0f,1.0f} }
    };

    // the blocks that should be solid if valid water tile is not found
    int solidCount = 5;
    static int solidTable[] = { BLOCK_WATER, BLOCK_STATIONARY_WATER, BLOCK_LAVA, BLOCK_STATIONARY_LAVA, BLOCK_FIRE };

    // Create basic composite files for flattops and flatsides with alpha cutouts in them
    // ladder, trapdoor, three torches, two rails, powered rail, detector rail, lily, wire
#define COMPOSITE_TABLE_SIZE 22
    CompositeSwatchPreset compositeTable[COMPOSITE_TABLE_SIZE] =
    { 
        /* MUST BE FIRST IN LIST, code rotates this one - rotated */ { SWATCH_INDEX( 5, 10 ), /* BLOCK_REDSTONE_WIRE */ SWATCH_INDEX( 1, 0 ) }, // wire over stone, vertical
        { SWATCH_INDEX( 0, 8 ), /* BLOCK_RAILS */ SWATCH_INDEX( 1, 0 ) }, // rail over stone
        { SWATCH_INDEX( 0, 7 ), /* BLOCK_RAILS */ SWATCH_INDEX( 1, 0 ) }, // curved rail over stone
        { SWATCH_INDEX( 0, 5 ), /* BLOCK_TORCH */ SWATCH_INDEX( 1, 0 ) }, // torch over stone
        { TORCH_TOP, /* BLOCK_TORCH */ SWATCH_INDEX( 1, 0 ) }, // torch top over stone
        { SWATCH_INDEX( 4, 10 ), /* BLOCK_REDSTONE_WIRE */ SWATCH_INDEX( 1, 0 ) }, // wire over stone
        { SWATCH_INDEX( 5, 10 ), /* BLOCK_REDSTONE_WIRE */ SWATCH_INDEX( 1, 0 ) }, // 2-wire straight over stone
        { SWATCH_INDEX( 4, 11 ), /* BLOCK_REDSTONE_WIRE */ SWATCH_INDEX( 1, 0 ) }, // no-wire dot over stone (some texture packs don't use this?)
        { REDSTONE_WIRE_ANGLED_2, /* BLOCK_REDSTONE_WIRE */ SWATCH_INDEX( 1, 0 ) }, // 2-wire angled over stone
        { REDSTONE_WIRE_3, /* BLOCK_REDSTONE_WIRE */ SWATCH_INDEX( 1, 0 ) }, // 3-wire over stone
        { SWATCH_INDEX( 3, 5 ), /* BLOCK_LADDER */ SWATCH_INDEX( 1, 0 ) }, // ladder over stone
        { SWATCH_INDEX( 3, 11 ), /* BLOCK_POWERED_RAIL */ SWATCH_INDEX( 1, 0 ) }, // powered rail over stone
        { SWATCH_INDEX( 3, 10 ), /* BLOCK_POWERED_RAIL */ SWATCH_INDEX( 1, 0 ) }, // unpowered rail over stone
        { SWATCH_INDEX( 3, 12 ), /* BLOCK_DETECTOR_RAIL */ SWATCH_INDEX( 1, 0 ) }, // detector rail over stone
        { SWATCH_INDEX( 3, 6 ), /* BLOCK_REDSTONE_TORCH_ON */ SWATCH_INDEX( 1, 0 ) }, // redstone torch on over stone
        { RS_TORCH_TOP_ON, /* BLOCK_REDSTONE_TORCH_ON */ SWATCH_INDEX( 1, 0 ) }, // redstone torch on over stone
        { SWATCH_INDEX( 3, 7 ), /* BLOCK_REDSTONE_TORCH_OFF */ SWATCH_INDEX( 1, 0 ) }, // redstone torch off over stone
        { RS_TORCH_TOP_OFF, /* BLOCK_REDSTONE_TORCH_OFF */ SWATCH_INDEX( 1, 0 ) }, // redstone torch off over stone
        { SWATCH_INDEX( 0, 6 ), /* BLOCK_LEVER */ SWATCH_INDEX( 1, 0 ) }, // lever over stone
        { SWATCH_INDEX( 12, 4 ), /* BLOCK_LILY_PAD */ SWATCH_INDEX( 15,13 ) }, // lily pad over water
        { SWATCH_INDEX( 4, 5 ), /* BLOCK_TRAPDOOR */ SWATCH_INDEX( 1, 0 ) }, // trapdoor over stone
		{ SWATCH_INDEX( 15, 8 ), /* BLOCK_VINES */ SWATCH_INDEX( 1, 0 ) }, // vines over stone
    };

//#define FA_TABLE_SIZE 55
//#define FA_TABLE__VIEW_SIZE 17
//    static FillAlpha faTable[FA_TABLE_SIZE] =
//    {
//        // stuff that is put in always, fringes that need to be filled
//        { SWATCH_INDEX( 5, 9 ), BLOCK_BLACK_WOOL }, // bed
//        { SWATCH_INDEX( 6, 9 ), BLOCK_BLACK_WOOL }, // bed
//        { SWATCH_INDEX( 7, 9 ), BLOCK_BLACK_WOOL }, // bed
//        { SWATCH_INDEX( 8, 9 ), BLOCK_BLACK_WOOL }, // bed
//        { SWATCH_INDEX( 5, 4 ), BLOCK_CACTUS }, // cactus
//        { SWATCH_INDEX( 6, 4 ), BLOCK_CACTUS }, // cactus
//        { SWATCH_INDEX( 7, 4 ), BLOCK_CACTUS }, // cactus
//        { SWATCH_INDEX( 9, 7 ), BLOCK_CAKE }, // cake
//        { SWATCH_INDEX( 10, 7 ), BLOCK_CAKE }, // cake
//        { SWATCH_INDEX( 11, 7 ), BLOCK_CAKE }, // cake
//        { SWATCH_INDEX( 12, 7 ), BLOCK_CAKE }, // cake
//        { SWATCH_INDEX( 10, 8 ), BLOCK_BLACK_WOOL }, // cauldron
//        { SWATCH_INDEX( 10, 9 ), BLOCK_BLACK_WOOL }, // cauldron
//        { SWATCH_INDEX( 11, 9 ), BLOCK_BLACK_WOOL }, // cauldron
//        { SWATCH_INDEX( 15, 9 ), SWATCH_INDEX(15,10) }, // ender portal (should be filled, but just in case, due to stretch)
//        { SWATCH_INDEX( 15, 14 ), BLOCK_LAVA }, // lava, in case it's not filled
//        { SWATCH_INDEX( 15, 15 ), BLOCK_STATIONARY_LAVA }, // stationary lava, in case it's not present
//
//
//        // stuff filled in for 3D printing only
//        { SWATCH_INDEX( 11, 0 ), SWATCH_INDEX( 6, 3 ) }, // spiderweb over stone block
//        { SWATCH_INDEX( 12, 0 ), SWATCH_INDEX( 0, 0 ) }, // red flower over grass
//        { SWATCH_INDEX( 13, 0 ), SWATCH_INDEX( 0, 0 ) }, // yellow flower over grass
//        { SWATCH_INDEX( 15, 0 ), SWATCH_INDEX( 0, 0 ) }, // sapling over grass
//        { SWATCH_INDEX( 12, 1 ), SWATCH_INDEX( 0, 0 ) }, // red mushroom over grass
//        { SWATCH_INDEX( 13, 1 ), SWATCH_INDEX( 0, 0 ) }, // brown mushroom over grass
//        { SWATCH_INDEX( 1, 3 ), BLOCK_GLASS }, // glass over glass color
//        { SWATCH_INDEX( 4, 3 ), BLOCK_AIR }, // transparent leaves over air (black) - doesn't really matter, not used when printing anyway
//        { SWATCH_INDEX( 7, 3 ), SWATCH_INDEX( 2, 1 ) }, // dead bush over grass
//        { SWATCH_INDEX( 8, 3 ), SWATCH_INDEX( 0, 0 ) }, // sapling over grass
//        { SWATCH_INDEX( 1, 4 ), SWATCH_INDEX( 1, 0 ) }, // spawner over stone
//        { SWATCH_INDEX( 9, 4 ), SWATCH_INDEX( 0, 0 ) }, // reeds over grass
//        { SWATCH_INDEX( 15, 4 ), SWATCH_INDEX( 0, 0 ) }, // sapling over grass
//        { SWATCH_INDEX( 1, 5 ), SWATCH_INDEX( 6, 0 ) }, // wooden door top over slab top
//        { SWATCH_INDEX( 2, 5 ), SWATCH_INDEX( 6, 0 ) }, // door top over slab top
//        { SWATCH_INDEX( 5, 5 ), SWATCH_INDEX( 6, 3 ) }, // iron bars over stone block
//        { SWATCH_INDEX( 8, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
//        { SWATCH_INDEX( 9, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
//        { SWATCH_INDEX( 10, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
//        { SWATCH_INDEX( 11, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
//        { SWATCH_INDEX( 12, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
//        { SWATCH_INDEX( 13, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
//        { SWATCH_INDEX( 14, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
//        { SWATCH_INDEX( 15, 5 ), SWATCH_INDEX( 6, 5 ) }, // crops over farmland
//        { SWATCH_INDEX( 0, 6 ), SWATCH_INDEX( 1, 0 ) }, // lever over stone
//        { SWATCH_INDEX( 15, 6 ), SWATCH_INDEX( 6, 5 ) }, // stem over farmland
//        { SWATCH_INDEX( 15, 7 ), SWATCH_INDEX( 6, 5 ) }, // dead stem over farmland
//        { SWATCH_INDEX( 4, 8 ), BLOCK_AIR }, // leaves over air (black) - doesn't really matter, not used
//        { SWATCH_INDEX( 10, 8 ), BLOCK_AIR }, // cauldron over air (black)
//        { SWATCH_INDEX( 12, 8 ), BLOCK_AIR }, // cake over air (black) - what's this for, anyway?
//        { SWATCH_INDEX( 15, 8 ), SWATCH_INDEX( 0, 0 ) }, // vines over grass
//        { SWATCH_INDEX( 4, 9 ), BLOCK_GLASS }, // glass pane over glass color (more interesting than stone, and lets you choose)
//        { SWATCH_INDEX( 13, 9 ), SWATCH_INDEX( 1, 0 ) }, // brewing stand over stone
//        { SWATCH_INDEX( 15, 10 ), SWATCH_INDEX( 1, 0 ) }, // end portal thingy (unused) over stone
//        { SWATCH_INDEX( 4, 12 ), BLOCK_AIR }, // jungle leaves over air (black) - doesn't really matter, not used
//        { SWATCH_INDEX( 2, 14 ), SWATCH_INDEX( 8, 6 ) }, // nether wart over soul sand
//        { SWATCH_INDEX( 3, 14 ), SWATCH_INDEX( 8, 6 ) }, // nether wart over soul sand
//        { SWATCH_INDEX( 4, 14 ), SWATCH_INDEX( 8, 6 ) }, // nether wart over soul sand
//    };
//
// an edge has 3 states: ignore (do nothing), repeat other side to it, or clamp (copy neighbor) to it
#define SBIT_REPEAT_SIDES       0x01
#define SBIT_REPEAT_TOP_BOTTOM  0x02
#define SBIT_CLAMP_BOTTOM       0x04
#define SBIT_CLAMP_TOP          0x08
#define SBIT_CLAMP_RIGHT        0x10
#define SBIT_CLAMP_LEFT         0x20

// types of blocks: tiling, billboard, and sides (which tile only horizontally)
#define SWATCH_REPEAT_ALL                   (SBIT_REPEAT_SIDES|SBIT_REPEAT_TOP_BOTTOM)
#define SWATCH_REPEAT_SIDES_ELSE_CLAMP      (SBIT_REPEAT_SIDES|SBIT_CLAMP_BOTTOM|SBIT_CLAMP_TOP)
#define SWATCH_TILE_BOTTOM_AND_TOP          SBIT_REPEAT_TOP_BOTTOM
#define SWATCH_CLAMP_BOTTOM_AND_RIGHT       (SBIT_CLAMP_BOTTOM|SBIT_CLAMP_RIGHT)
#define SWATCH_CLAMP_ALL_BUT_TOP            (SBIT_CLAMP_BOTTOM|SBIT_CLAMP_RIGHT|SBIT_CLAMP_LEFT)
#define SWATCH_CLAMP_ALL                    (SBIT_CLAMP_TOP|SBIT_CLAMP_BOTTOM|SBIT_CLAMP_RIGHT|SBIT_CLAMP_LEFT)

    int swatchHoldTable[256];
    for ( i = 0; i < 256; i++ )
    {
        swatchHoldTable[i] = SWATCH_REPEAT_ALL;
    }

    // billboards are different:
    swatchHoldTable[11] = swatchHoldTable[12] = swatchHoldTable[13] = swatchHoldTable[15] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[1*16+12] = swatchHoldTable[1*16+13] = swatchHoldTable[1*16+14] = swatchHoldTable[1*16+15] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[2*16+7] = swatchHoldTable[2*16+15] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[3*16+7] = swatchHoldTable[3*16+8] = swatchHoldTable[3*16+15] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[4*16+12] = swatchHoldTable[4*16+15] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[5*16+0] = swatchHoldTable[5*16+8] = swatchHoldTable[5*16+9] = swatchHoldTable[5*16+10] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[5*16+11] = swatchHoldTable[5*16+12] = swatchHoldTable[5*16+13] = swatchHoldTable[5*16+14] = swatchHoldTable[5*16+15] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[6*16+0] = swatchHoldTable[6*16+15] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[7*16+3] = swatchHoldTable[7*16+9] = swatchHoldTable[7*16+10] = swatchHoldTable[7*16+11] = swatchHoldTable[7*16+12] = swatchHoldTable[7*16+15] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[8*16+12] = swatchHoldTable[8*16+15] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[9*16+13] = SBIT_CLAMP_BOTTOM;
    swatchHoldTable[14*16+2] = swatchHoldTable[14*16+3] = swatchHoldTable[14*16+4] = SBIT_CLAMP_BOTTOM;

    // side blocks without repeat
    swatchHoldTable[0*16+3] = swatchHoldTable[2*16+6] = swatchHoldTable[4*16+4] = swatchHoldTable[4*16+13] = SWATCH_REPEAT_SIDES_ELSE_CLAMP;

    // tile bottom and top: ladders, rails
    swatchHoldTable[6*16+3] = swatchHoldTable[8*16+0] = swatchHoldTable[10*16+3] = swatchHoldTable[11*16+3] = swatchHoldTable[12*16+3] = SWATCH_TILE_BOTTOM_AND_TOP;

    // clamp bottom and right (curved rail, only)
    swatchHoldTable[7*16+0] = SWATCH_CLAMP_BOTTOM_AND_RIGHT;

    // clamp things we'll stretch
    swatchHoldTable[9*16+5] = swatchHoldTable[9*16+6] = swatchHoldTable[9*16+7] = swatchHoldTable[9*16+8] = SWATCH_CLAMP_ALL_BUT_TOP;
    swatchHoldTable[11*16+6] = SWATCH_CLAMP_ALL_BUT_TOP;

    // tiles that fill block but should not repeat smoothly: chests. Currently we tile furnaces and so on by default, but these could be clamped, too.
    swatchHoldTable[1*16+9] = swatchHoldTable[1*16+10] = swatchHoldTable[1*16+11] = SWATCH_CLAMP_ALL;
    swatchHoldTable[2*16+9] = 
        swatchHoldTable[2*16+10] = SWATCH_CLAMP_ALL;
    swatchHoldTable[3*16+9] = 
        swatchHoldTable[3*16+10] = SWATCH_CLAMP_ALL;


    mainprog = (progimage_info *)malloc(sizeof(progimage_info));
    memset(mainprog,0,sizeof(progimage_info));

    mainprog->gamma = 0.0;
    mainprog->width = gModel.textureResolution;
    mainprog->height = gModel.textureResolution;
    mainprog->have_time = 0;
    mainprog->modtime;
    mainprog->color_type = PNG_COLOR_TYPE_RGB_ALPHA;	// RGBA - PNG_COLOR_TYPE_RGB_ALPHA
    //FILE *infile;
    //returns: void *png_ptr;
    //returns: void *info_ptr;
    mainprog->bit_depth = 8;
    mainprog->interlaced = PNG_INTERLACE_NONE;
    mainprog->have_bg = 0;
    mainprog->bg_red;
    mainprog->bg_green;
    mainprog->bg_blue;
    //uch *image_data;
    mainprog->image_data = (unsigned char *)malloc(gModel.textureResolution*gModel.textureResolution*4*sizeof(unsigned char));
    //uch **row_pointers;
    //mainprog->row_pointers = (uch **)malloc(256*sizeof(uch *));
    mainprog->have_text = TEXT_TITLE|TEXT_AUTHOR|TEXT_DESC;
    mainprog->title = "Mineways model texture";
    mainprog->author = "mineways.com";
    mainprog->desc = "Mineways texture file for model, generated from user's terrain.png";
    mainprog->copyright;
    mainprog->email;
    mainprog->url;
    //mainprog->jmpbuf;	

    // clear
    memset(mainprog->image_data,0,gModel.textureResolution*gModel.textureResolution*4*sizeof(unsigned char));
    gModel.pPNGtexture = mainprog;

    useTextureImage = (gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES);

    // we fill the first NUM_BLOCKS with solid colors
    keepGoing = 1;
    gModel.swatchCount = 0;

    // crazy code: add noise when using swatches, but for VRML we don't add noise if we're
    // actually using material-only or no material modes (VRML always exports textures)
    if ( gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_SWATCHES )
    {
        addNoise = 1;
        // check if VRML, and in anything but rich texture mode
        if ( (gOptions->pEFD->fileType == FILE_TYPE_VRML2) && (!gOptions->pEFD->radioExportSolidTexture[gOptions->pEFD->fileType] ) )
        {
            addNoise = 0;
        }
    }
    for ( row = 0; row < gModel.swatchesPerRow && keepGoing; row++ )
    {
        for ( col = 0; col < gModel.swatchesPerRow && keepGoing; col++ )
        {
            // VRML only uses textures for output: set solid color to white if 
            // no color should be output
            if ( gOptions->pEFD->radioExportNoMaterials[gOptions->pEFD->fileType] )
            {
                r = g = b = a = 255;
                assert(gOptions->pEFD->fileType == FILE_TYPE_VRML2);
            }
            else
            {
                // fill with a solid color
                r=(unsigned char)(gBlockDefinitions[gModel.swatchCount].color>>16);
                g=(unsigned char)((gBlockDefinitions[gModel.swatchCount].color>>8) & 0xff);
                b=(unsigned char)(gBlockDefinitions[gModel.swatchCount].color & 0xff);
                a = (unsigned char)(gBlockDefinitions[gModel.swatchCount].alpha*255.0f);

                if (gOptions->exportFlags & EXPT_DEBUG_SHOW_GROUPS)
                {
                    // make sure group is opaque: it can happen
                    // that the debug group got turned off in a color scheme.
                    a = 255;
                }
                // if the basic solid swatches are to be printed, or to be used as baselines
                // for compositing with texture images, they must be opaque
                else if ( gOptions->exportFlags & (EXPT_3DPRINT|EXPT_OUTPUT_TEXTURE_IMAGES) )
                {
                    a = 255;
                }
            }

            // Order in PNG file is ABGR
            color = (a<<24)|(b<<16)|(g<<8)|r;

            setColorPNGTile( mainprog, col, row, gModel.swatchSize, color );

            if ( addNoise )
            {
                // if we're using a textured swatch, multiply the boring solid colors with some noise
                // to make textures richer
                addNoisePNGTile( mainprog, col, row, gModel.swatchSize, r, g, b, a, 0.1f );
            }

            gModel.swatchCount++;
            keepGoing = (gModel.swatchCount < NUM_BLOCKS);
        }
    }

    if ( useTextureImage )
    {
        int dstCol,dstRow;
        // we then convert *all* 256 tiles in terrain.png to 18x18 or whatever tiles, adding a 1 pixel border (SWATCH_BORDER)
        // around each (since with tile mosaics, we can't clamp to border, nor can we know that the renderer
        // will clamp and get blocky pixels)
        for ( row = 0; row < 16; row++ )
        {
            for ( col = 0; col < 16; col++ )
            {
                SWATCH_TO_COL_ROW( gModel.swatchCount, dstCol, dstRow );
                // main copy
                copyPNGArea( mainprog, 
                    gModel.swatchSize*dstCol+SWATCH_BORDER, // upper left corner destination
                    gModel.swatchSize*dstRow+SWATCH_BORDER,
                    gModel.tileSize, gModel.tileSize, // width, height to copy
                    &gModel.inputTerrainImage,
                    gModel.tileSize*col, // from
                    gModel.tileSize*row
                    );


                // copy left and right edges only if block is solid - billboards don't tile
                if ( swatchHoldTable[row*16+col] & SBIT_REPEAT_SIDES )
                {
                    // copy right edge from left side of tile
                    copyPNGArea( mainprog, 
                        gModel.swatchSize*(dstCol+1)-SWATCH_BORDER,    // copy to rightmost column
                        gModel.swatchSize*dstRow+SWATCH_BORDER,  // one down from top
                        SWATCH_BORDER, gModel.tileSize,  // 1 wide
                        &gModel.inputTerrainImage,
                        gModel.tileSize*col,
                        gModel.tileSize*row
                        );
                    // copy left edge from right side of tile
                    copyPNGArea( mainprog, 
                        gModel.swatchSize*dstCol,    // copy to leftmost column
                        gModel.swatchSize*dstRow+SWATCH_BORDER,
                        SWATCH_BORDER, gModel.tileSize,  // 1 wide
                        &gModel.inputTerrainImage,
                        gModel.tileSize*(col+1)-SWATCH_BORDER,
                        gModel.tileSize*row
                        );
                }
                else 
                {
                    if ( swatchHoldTable[row*16+col] & SBIT_CLAMP_LEFT )
                    {
                        // copy left edge from left side of tile
                        copyPNGArea( mainprog, 
                            gModel.swatchSize*dstCol,    // copy to leftmost column
                            gModel.swatchSize*dstRow+SWATCH_BORDER,  // one down from top
                            SWATCH_BORDER, gModel.tileSize,  // 1 wide
                            &gModel.inputTerrainImage,
                            gModel.tileSize*col,
                            gModel.tileSize*row
                            );
                    }
                    if ( swatchHoldTable[row*16+col] & SBIT_CLAMP_RIGHT )
                    {
                        // copy right edge from right side of tile
                        copyPNGArea( mainprog, 
                            gModel.swatchSize*(dstCol+1)-SWATCH_BORDER,    // copy to rightmost column
                            gModel.swatchSize*dstRow+SWATCH_BORDER,  // one down from top
                            SWATCH_BORDER, gModel.tileSize,  // 1 wide
                            &gModel.inputTerrainImage,
                            gModel.tileSize*(col+1)-SWATCH_BORDER,
                            gModel.tileSize*row
                            );
                    }
                }


                // top edge
                if ( swatchHoldTable[row*16+col] & SBIT_CLAMP_BOTTOM )
                {
                    // hold and repeat bottom of billboard, and of any "side" blocks where top and bottom don't tile
                    // NOTE: this really won't work for SWATCH_BORDER > 1, you really need to loop through each
                    // row one by one and copy it.
                    copyPNGArea( mainprog, 
                        gModel.swatchSize*dstCol, 
                        gModel.swatchSize*(dstRow+1)-SWATCH_BORDER,    // copy to bottom of dstRow
                        gModel.swatchSize, SWATCH_BORDER,     // 1 high
                        mainprog,
                        gModel.swatchSize*dstCol,
                        gModel.swatchSize*(dstRow+1)-SWATCH_BORDER-1  // copy bottom dstRow that exists
                        );
                }
                if ( swatchHoldTable[row*16+col] & SBIT_CLAMP_TOP )
                {
                    // hold and repeat top (otherwise, top remains all zeroes, which is good for billboards)
                    copyPNGArea( mainprog, 
                        gModel.swatchSize*dstCol, 
                        gModel.swatchSize*dstRow,    // copy to top of dstRow
                        gModel.swatchSize, SWATCH_BORDER,     // 1 high
                        mainprog,
                        gModel.swatchSize*dstCol,
                        gModel.swatchSize*dstRow+SWATCH_BORDER  // copy top dstRow that exists
                        );
                }
                else if ( swatchHoldTable[row*16+col] & SBIT_REPEAT_TOP_BOTTOM )
                {
                    // repeat tile
                    // copy upper fringe from self!
                    copyPNGArea( mainprog, 
                        gModel.swatchSize*dstCol, 
                        gModel.swatchSize*dstRow,    // copy to top of dstRow
                        gModel.swatchSize, SWATCH_BORDER,     // 1 high
                        mainprog,
                        gModel.swatchSize*dstCol,
                        gModel.swatchSize*(dstRow+1)-SWATCH_BORDER-1   // copy bottom dstRow that exists
                        );
                    // copy lower fringe
                    copyPNGArea( mainprog, 
                        gModel.swatchSize*dstCol, 
                        gModel.swatchSize*(dstRow+1)-SWATCH_BORDER,    // copy to bottom of dstRow
                        gModel.swatchSize, SWATCH_BORDER,     // 1 high
                        mainprog,
                        gModel.swatchSize*dstCol,
                        gModel.swatchSize*dstRow+SWATCH_BORDER  // copy top dstRow that exists
                        );
                }
                gModel.swatchCount++;
            }
        }

        // test if water tile is semitransparent throughout - if not, then we don't want to use water, lava, and file tiles.
        if ( tileIsSemitransparent( &gModel.inputTerrainImage, 15, 12 ) &&
             tileIsOpaque( &gModel.inputTerrainImage, 15, 14 ) )
        {
            // Water is special, and we want to provide more user control for it, to be able to give a deep
            // blue, etc. We therefore blend between the water texture and the water swatch based on alpha:
            // the higher the alpha (more opaque) the water color is set, the more it contributes to the
            // water texture.
            for ( i = 0; i < 2; i++ )
            {
                blendTwoSwatches( mainprog, SWATCH_INDEX( 15, 12+i ), BLOCK_WATER+i, gBlockDefinitions[BLOCK_WATER+i].alpha,
                    (gOptions->exportFlags & EXPT_3DPRINT) ? 255 : (unsigned char)(gBlockDefinitions[BLOCK_WATER+i].alpha*255) );
            }
        }
        else
        {
            int srcCol,srcRow;

            // can't use water (15,12), lava (15,14), and fire (15,1), so just copy solid color over
            for ( i = 0; i < solidCount; i++ )
            {
                int id = solidTable[i];

                SWATCH_TO_COL_ROW( SWATCH_INDEX(gBlockDefinitions[id].txrX,gBlockDefinitions[id].txrY), dstCol, dstRow );
                SWATCH_TO_COL_ROW( id, srcCol, srcRow );
                copyPNGArea( mainprog, 
                    gModel.swatchSize*dstCol, // upper left corner destination
                    gModel.swatchSize*dstRow,
                    gModel.swatchSize, gModel.swatchSize, // width, height to copy
                    mainprog,
                    gModel.swatchSize*srcCol, // from
                    gModel.swatchSize*srcRow
                    );
            }
        }

        // test if jungle leaves exist (which used to be oak leaves): if not, then use oak leaves instead
        gJungleExists = tileIsCutout( &gModel.inputTerrainImage, 4, 12 ) &&
            tileIsOpaque( &gModel.inputTerrainImage, 5, 12 );

        // insane corrective: truly tile between chest shared edges, grabbing samples from *adjacent* swatch
        for ( i = 0; i < 2; i++ )
        {
            SWATCH_TO_COL_ROW( SWATCH_INDEX(10,2+i), col, row );
            // copy left edge from left side of tile
            copyPNGArea( mainprog, 
                gModel.swatchSize*col,    // copy to rightmost column
                gModel.swatchSize*row,  // full column
                SWATCH_BORDER, gModel.swatchSize,  // 1 wide
                mainprog,
                gModel.swatchSize*col-2*SWATCH_BORDER,
                gModel.swatchSize*row
                );
            copyPNGArea( mainprog, 
                gModel.swatchSize*col-SWATCH_BORDER,    // copy to rightmost column
                gModel.swatchSize*row,  // full column
                SWATCH_BORDER, gModel.swatchSize,  // 1 wide
                mainprog,
                gModel.swatchSize*col+SWATCH_BORDER,
                gModel.swatchSize*row
                );
        }

        // These tiles come in grayscale and must be multiplied by some color.
        // They're grass, redstone wire, leaves, stems, etc.
        // Multiply these blocks by the solid color, e.g. grass, other white tiles
        for ( i = 0; i < MULT_TABLE_SIZE; i++ )
        {
            adj = multTable[i].type;
            color = gBlockDefinitions[adj].color;
            r = (unsigned char)(clamp((float)(color>>16) * multTable[i].colorMult[0],0.0f,255.0f));
            g = (unsigned char)(clamp((float)((color>>8) & 0xff) * multTable[i].colorMult[1],0.0f,255.0f));
            b = (unsigned char)(clamp((float)(color & 0xff) * multTable[i].colorMult[2],0.0f,255.0f));
            a = (unsigned char)(gBlockDefinitions[adj].alpha * 255);

            //// special case: for pine leaves, modify green multiplier
            //if ( adj == BLOCK_LEAVES && multTable[i].row == 8 )
            //{
            //    r = (unsigned char)(clamp((float)r * 72.0f/64.0f,0.0f,255.0f));
            //    g = (unsigned char)(clamp((float)g * 113.0f/162.0f,0.0f,255.0f));
            //    b = (unsigned char)(clamp((float)b * 72.0f/22.0f,0.0f,255.0f));
            //}

            idx = SWATCH_INDEX( multTable[i].col, multTable[i].row );
            SWATCH_TO_COL_ROW( idx, dstCol, dstRow );

            multiplyPNGTile(mainprog, dstCol,dstRow, gModel.swatchSize, r, g, b, a );
        }

        // Make a "top of torch" template for the torch, and redstone torch on and off
        // Torch tops goes in 0,15 / 1,15 redstone on / 2,15 redstone off
        SWATCH_TO_COL_ROW( TORCH_TOP, col, row );
        SWATCH_TO_COL_ROW( SWATCH_INDEX(0,5), scol, srow );
        copyPNGTile(mainprog, col, row, gModel.swatchSize, mainprog, scol, srow );
        // clear bottom half of torch
        setColorPNGArea(mainprog, col*gModel.swatchSize, row*gModel.swatchSize + gModel.tileSize*10/16 + SWATCH_BORDER, gModel.swatchSize, gModel.tileSize*6/16 + SWATCH_BORDER, 0x0 );
        
        SWATCH_TO_COL_ROW( RS_TORCH_TOP_ON, col, row );
        SWATCH_TO_COL_ROW( SWATCH_INDEX(3,6), scol, srow );
        copyPNGTile(mainprog, col, row, gModel.swatchSize, mainprog, scol, srow );
        // clear bottom half of torch
        setColorPNGArea(mainprog, col*gModel.swatchSize, row*gModel.swatchSize + gModel.tileSize*10/16 + SWATCH_BORDER, gModel.swatchSize, gModel.tileSize*6/16 + SWATCH_BORDER, 0x0 );

        SWATCH_TO_COL_ROW( RS_TORCH_TOP_OFF, col, row );
        SWATCH_TO_COL_ROW( SWATCH_INDEX(3,7), scol, srow );
        copyPNGTile(mainprog, col, row, gModel.swatchSize, mainprog, scol, srow );
        // clear bottom half of torch
        setColorPNGArea(mainprog, col*gModel.swatchSize, row*gModel.swatchSize + gModel.tileSize*10/16 + SWATCH_BORDER, gModel.swatchSize, gModel.tileSize*6/16 + SWATCH_BORDER, 0x0 );


        // add compositing dot to 4-way wire template
        // Note that currently we have only "wire on" for all wires, no "wire off". We'd need new templates for all wires off, and we'd need an extra bit
        // to note that the wire was powered (right now we're cheating and using the data field, normally wire power, to instead hold which directions wires
        // are in).
        compositePNGSwatches(mainprog,SWATCH_INDEX(4,10),SWATCH_INDEX(4,11),SWATCH_INDEX(4,10),gModel.swatchSize,gModel.swatchesPerRow,0);

        // make 3 way wire, then make 2 way angled wire from it
        SWATCH_TO_COL_ROW( REDSTONE_WIRE_3, col, row );
        SWATCH_TO_COL_ROW( SWATCH_INDEX(4,10), scol, srow );
        copyPNGTile(mainprog, col, row, gModel.swatchSize, mainprog, scol, srow );
        // clear left half of wire
        setColorPNGArea(mainprog, col*gModel.swatchSize, row*gModel.swatchSize, gModel.tileSize*5/16 + SWATCH_BORDER, gModel.swatchSize, 0x0 );

        SWATCH_TO_COL_ROW( REDSTONE_WIRE_ANGLED_2, col, row );
        SWATCH_TO_COL_ROW( REDSTONE_WIRE_3, scol, srow );
        copyPNGTile(mainprog, col, row, gModel.swatchSize, mainprog, scol, srow );
        // clear bottom half of wire
        setColorPNGArea(mainprog, col*gModel.swatchSize, row*gModel.swatchSize + gModel.tileSize*11/16 + SWATCH_BORDER, gModel.swatchSize, gModel.tileSize*5/16 + SWATCH_BORDER, 0x0 );

        // Make sure single dot of wire has something in it:
        // copy 2-way to temp area, chop limbs, composite dot and copy back to 4,11
        SWATCH_TO_COL_ROW( SWATCH_WORKSPACE2, col, row );
        SWATCH_TO_COL_ROW( REDSTONE_WIRE_ANGLED_2, scol, srow );
        copyPNGTile(mainprog, col, row, gModel.swatchSize, mainprog, scol, srow );
        // clear right of wire
        setColorPNGArea(mainprog, col*gModel.swatchSize + gModel.tileSize*11/16 + SWATCH_BORDER, row*gModel.swatchSize, gModel.tileSize*5/16 + SWATCH_BORDER, gModel.swatchSize, 0x0 );
        // clear top of wire
        setColorPNGArea(mainprog, col*gModel.swatchSize, row*gModel.swatchSize, gModel.swatchSize, gModel.tileSize*5/16 + SWATCH_BORDER, 0x0 );
        // composite "lit bit" over wire, if any, and put into lit bit's place
        compositePNGSwatches(mainprog,SWATCH_INDEX(4,11),SWATCH_INDEX(4,11),SWATCH_WORKSPACE2,gModel.swatchSize,gModel.swatchesPerRow,0);



        // stretch tiles to fill the area
        // plus one for the border
        // enchantment table
        stretchSwatchToTop(mainprog, SWATCH_INDEX(6,11), (float)(gModel.swatchSize*(4.0/16.0)+(float)SWATCH_BORDER)/(float)gModel.swatchSize );

        // bed
        stretchSwatchToTop(mainprog, SWATCH_INDEX(5,9), (float)(gModel.swatchSize*(7.0/16.0)+(float)SWATCH_BORDER)/(float)gModel.swatchSize );
        stretchSwatchToTop(mainprog, SWATCH_INDEX(6,9), (float)(gModel.swatchSize*(7.0/16.0)+(float)SWATCH_BORDER)/(float)gModel.swatchSize );
        stretchSwatchToTop(mainprog, SWATCH_INDEX(7,9), (float)(gModel.swatchSize*(7.0/16.0)+(float)SWATCH_BORDER)/(float)gModel.swatchSize );
        stretchSwatchToTop(mainprog, SWATCH_INDEX(8,9), (float)(gModel.swatchSize*(7.0/16.0)+(float)SWATCH_BORDER)/(float)gModel.swatchSize );

        // cake
        stretchSwatchToTop(mainprog, SWATCH_INDEX(10,7), (float)(gModel.swatchSize*(8.0/16.0)+(float)SWATCH_BORDER)/(float)gModel.swatchSize );
        stretchSwatchToTop(mainprog, SWATCH_INDEX(11,7), (float)(gModel.swatchSize*(8.0/16.0)+(float)SWATCH_BORDER)/(float)gModel.swatchSize );

        // ender portal
        stretchSwatchToTop(mainprog, SWATCH_INDEX(15,9), (float)(gModel.swatchSize*(3.0/16.0)+(float)SWATCH_BORDER)/(float)gModel.swatchSize );

        // make the baseline composites - really, we should add these in-place >>>>> TODO
        for ( i = 0; i < COMPOSITE_TABLE_SIZE; i++ )
        {
            createCompositeSwatch( compositeTable[i].cutoutSwatch, compositeTable[i].backgroundSwatch, (i==0)?90:0 );
        }

  //      // fill in all alphas that 3D export wants filled; always fill in cactus, cake, and bed fringes, for example;
		//// For printing we also then composite over other backgrounds as the defaults.
  //      faTableCount = ( gOptions->exportFlags & EXPT_3DPRINT ) ? FA_TABLE_SIZE : FA_TABLE__VIEW_SIZE;
  //      for ( i = 0; i < faTableCount; i++ )
  //      {
  //          compositePNGSwatches( mainprog,
  //              faTable[i].cutout, faTable[i].cutout, faTable[i].underlay,
  //              gModel.swatchSize, gModel.swatchesPerRow, 0 );
  //      }

        // for print, all tiles must not have any alphas;
		// we used to strip these away, but now we simply don't output the alpha channel and just export RGB,
		// so no longer need to do this process.
        //if ( gOptions->exportFlags & EXPT_3DPRINT )
        //{
        //    // finally, go through all alphas: if alpha is < 1, composite the texture onto its corresponding type (solid) block
        //    for ( i = 0; i < NUM_BLOCKS; i++ )
        //    {
        //        if ( gBlockDefinitions[i].alpha < 1.0f )
        //        {
        //            int swatchLoc = SWATCH_INDEX( gBlockDefinitions[i].txrX, gBlockDefinitions[i].txrY );
        //            compositePNGSwatches( mainprog,
        //                swatchLoc, swatchLoc, i,
        //                gModel.swatchSize, gModel.swatchesPerRow, 1 );
        //        }
        //    }
        //}
    }

    return MW_NO_ERROR;
}

static int writeBinarySTLBox( const wchar_t *world, IBox *worldBox )
{
#ifdef WIN32
    DWORD br;
#endif

    wchar_t stlFileNameWithSuffix[MAX_PATH];
    const char *justWorldFileName;
    char worldNameUnderlined[MAX_PATH];

    char outputString[256];

    int faceNo,i;

    int retCode = MW_NO_ERROR;

    FaceRecord *pFace;
    Point *vertex[4];
    unsigned int numTri = gModel.faceCount*2;

    int colorBytes;
    unsigned short outColor = 0;
    unsigned char r,g,b;
    // export color if file format mode set that way
    int writeColor = (gOptions->exportFlags & (EXPT_OUTPUT_MATERIALS|EXPT_OUTPUT_TEXTURE));

    wchar_t statsFileName[MAX_PATH];

    HANDLE statsFile;

    char worldChar[MAX_PATH];

    // if no color output, don't use isMagics
    int isMagics = writeColor && (gOptions->pEFD->fileType == FILE_TYPE_BINARY_MAGICS_STL);

    concatFileName3(stlFileNameWithSuffix, gOutputFilePath, gOutputFileRoot, L".stl");

    // create the STL file
    gModelFile = PortaCreate(stlFileNameWithSuffix);
    addOutputFilenameToList(stlFileNameWithSuffix);
    if (gModelFile == INVALID_HANDLE_VALUE)
        return MW_CANNOT_CREATE_FILE;

    // find last \ in world string
    wcharToChar(world,worldChar);
    justWorldFileName = removePathChar(worldChar);

    // replace spaces with underscores for world name output
    strcpy_s(worldNameUnderlined,256,justWorldFileName);
    spacesToUnderlinesChar(worldNameUnderlined);

    if ( isMagics )
    {
        unsigned int allFF = 0xffffffff;
        // Start with a default color of white - we'll always replace it
        // make it all 0x20 as we will output exactly 80 characters
        sprintf_s(outputString,256,"COLOR=");
        // start to write file
        WERROR(PortaWrite(gModelFile, outputString, 6 ));
        WERROR(PortaWrite(gModelFile, &allFF, 4 ));
        // in the example file, all the rest was 0x20's (space)
        memset(outputString,0x20,256);
        WERROR(PortaWrite(gModelFile, outputString, 70 ));
    }
    else
    {
        // Object name - give them a hint where it's from: world name and coordinates
        // make it all NULL as we will output exactly 80 characters
        memset(outputString,0,256);
        sprintf_s(outputString,256,"Mineways.com: world %s %d %d %d to %d %d %d\n", worldNameUnderlined,
            worldBox->min[X], worldBox->min[Y], worldBox->min[Z],
            worldBox->max[X], worldBox->max[Y], worldBox->max[Z] );
        // start to write file
        WERROR(PortaWrite(gModelFile, outputString, 80 ));
    }

    // number of triangles in model, unsigned in
    WERROR(PortaWrite(gModelFile, &numTri, 4 ));

    // write out the faces, it's just that simple
    for ( faceNo = 0; faceNo < gModel.faceCount; faceNo++ )
    {
        if ( faceNo % 1000 == 0 )
            UPDATE_PROGRESS( PG_OUTPUT + (PG_TEXTURE-PG_OUTPUT)*((float)faceNo/(float)gModel.faceCount));

        pFace = gModel.faceList[faceNo];
        // get four face indices for the four corners
        for ( i = 0; i < 4; i++ )
        {
            vertex[i] = &gModel.vertices[pFace->vertexIndex[i]];
        }

        for ( i = 0; i < 2; i++ )
        {
            // 3 float normals
            WERROR(PortaWrite(gModelFile, gModel.normals, 12 ));

            WERROR(PortaWrite(gModelFile, vertex[0], 12 ));
            WERROR(PortaWrite(gModelFile, vertex[i+1], 12 ));
            WERROR(PortaWrite(gModelFile, vertex[i+2], 12 ));

            if ( writeColor )
            {
                // http://en.wikipedia.org/wiki/Stl_file_format#Colour_in_binary_STL
                if ( isMagics )
                {
                    // Materialise Magics 
                    colorBytes = gBlockDefinitions[pFace->type].color;
                    r=(unsigned char)(colorBytes>>16);
                    g=(unsigned char)(colorBytes>>8);
                    b=(unsigned char)(colorBytes);
                    r=r*31/255;
                    g=g*31/255;
                    b=b*31/255;
                    // topmost bit says the global color is used, so we turn it off so the per-face color here is used.
                    outColor = (b<<10) | (g<<5) | r;
                }
                else
                {
                    // VisCAM/SolidView 
                    colorBytes = gBlockDefinitions[pFace->type].color;
                    r=(unsigned char)(colorBytes>>16);
                    g=(unsigned char)(colorBytes>>8);
                    b=(unsigned char)(colorBytes);
                    r=r*31/255;
                    g=g*31/255;
                    b=b*31/255;
                    // topmost bit says this color is valid. Note order is reverse of Magics' order, above.
                    outColor = (1<<15) | (r<<10) | (g<<5) | b;
                }
            }
            WERROR(PortaWrite(gModelFile, &outColor, 2 ));
        }
    }

    // if not ok, then we will have closed the file earlier
    PortaClose(gModelFile);

    concatFileName3(statsFileName, gOutputFilePath, gOutputFileRoot, L".txt");

    // write the stats to a separate file
    statsFile = PortaCreate(statsFileName);
    addOutputFilenameToList(statsFileName);
    if (statsFile == INVALID_HANDLE_VALUE)
        return MW_CANNOT_CREATE_FILE;

    //
    retCode = writeStatistics( statsFile, justWorldFileName, worldBox );
    if ( retCode >= MW_BEGIN_ERRORS )
        return retCode;

    PortaClose(statsFile);

    return retCode;
}

static int writeAsciiSTLBox( const wchar_t *world, IBox *worldBox )
{
#ifdef WIN32
    DWORD br;
#endif

    wchar_t stlFileNameWithSuffix[MAX_PATH];
    const char *justWorldFileName;
    char worldNameUnderlined[MAX_PATH];
    wchar_t statsFileName[MAX_PATH];

    HANDLE statsFile;

    char outputString[256];

    int faceNo,i;

    int retCode = MW_NO_ERROR;

    int normalIndex;
    FaceRecord *pFace;
    Point *vertex[4],*pt;

    char worldChar[MAX_PATH];

    concatFileName3(stlFileNameWithSuffix, gOutputFilePath, gOutputFileRoot, L".stl");

    // create the STL file
    gModelFile = PortaCreate(stlFileNameWithSuffix);
    addOutputFilenameToList(stlFileNameWithSuffix);
    if (gModelFile == INVALID_HANDLE_VALUE)
        return MW_CANNOT_CREATE_FILE;

    // find last \ in world string
    wcharToChar(world,worldChar);
    justWorldFileName = removePathChar(worldChar);

    // replace spaces with underscores for world name output
    strcpy_s(worldNameUnderlined,256,justWorldFileName);
    spacesToUnderlinesChar(worldNameUnderlined);

    // Object name - give them a hint where it's from: world name and coordinates
    sprintf_s(outputString,256,"solid %s__%d_%d_%d_to_%d_%d_%d\n", worldNameUnderlined,
        worldBox->min[X], worldBox->min[Y], worldBox->min[Z],
        worldBox->max[X], worldBox->max[Y], worldBox->max[Z] );
    // start to write file
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    // ready the normals for direct output, since we reuse them a zillion times
    for ( i = 0; i < 6; i++ )
    {
        sprintf_s(gFacetNormalString[i],256,"facet normal %e %e %e\n",gModel.normals[i][X],gModel.normals[i][Y],gModel.normals[i][Z]);
    }

    // write out the faces, it's just that simple
    for ( faceNo = 0; faceNo < gModel.faceCount; faceNo++ )
    {
        if ( faceNo % 1000 == 0 )
            UPDATE_PROGRESS( PG_OUTPUT + (PG_TEXTURE-PG_OUTPUT)*((float)faceNo/(float)gModel.faceCount));

        pFace = gModel.faceList[faceNo];
        // get four face indices for the four corners
        for ( i = 0; i < 4; i++ )
        {
            vertex[i] = &gModel.vertices[pFace->vertexIndex[i]];
        }

        normalIndex = pFace->normalIndex;
        //facet normal 0.000000e+000 -1.000000e+000 0.000000e+000
        //  outer loop
        //    vertex  1.000000e-002 3.000000e-002 -2.000000e-003
        //    vertex  1.200000e-002 3.000000e-002 -2.000000e-003
        //    vertex  1.200000e-002 3.000000e-002 -0.000000e+000
        //  endloop
        //endfacet
        for ( i = 0; i < 2; i++ )
        {
            WERROR(PortaWrite(gModelFile, gFacetNormalString[normalIndex], strlen(gFacetNormalString[normalIndex]) ));
            WERROR(PortaWrite(gModelFile, "outer loop\n", strlen("outer loop\n") ));

            pt = vertex[0];
            sprintf_s(outputString,256,"vertex  %e %e %e\n",(double)((*pt)[X]),(double)((*pt)[Y]),(double)((*pt)[Z]));
            WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));
            pt = vertex[i+1];
            sprintf_s(outputString,256,"vertex  %e %e %e\n",(double)((*pt)[X]),(double)((*pt)[Y]),(double)((*pt)[Z]));
            WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));
            pt = vertex[i+2];
            sprintf_s(outputString,256,"vertex  %e %e %e\n",(double)((*pt)[X]),(double)((*pt)[Y]),(double)((*pt)[Z]));
            WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

            WERROR(PortaWrite(gModelFile, "endloop\nendfacet\n", strlen("endloop\nendfacet\n") ));
        }
    }

    sprintf_s(outputString,256,"endsolid %s\n",worldNameUnderlined);
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    // if not ok, then we will have closed the file earlier
    PortaClose(gModelFile);

    concatFileName3(statsFileName, gOutputFilePath, gOutputFileRoot, L".txt");

    // write the stats to a separate file
    statsFile = PortaCreate(statsFileName);
    addOutputFilenameToList(statsFileName);
    if (statsFile == INVALID_HANDLE_VALUE)
        return MW_CANNOT_CREATE_FILE;

    //
    retCode = writeStatistics( statsFile, justWorldFileName, worldBox );
    if ( retCode >= MW_BEGIN_ERRORS )
        return retCode;

    PortaClose(statsFile);
 
    return retCode;
}


static int writeVRML2Box( const wchar_t *world, IBox *worldBox )
{
#ifdef WIN32
DWORD br;
#endif

    wchar_t wrlFileNameWithSuffix[MAX_PATH];
    const char *justWorldFileName;
    char justTextureFileName[MAX_PATH];	// without path

    char outputString[256];

    int i,j;

    int retCode = MW_NO_ERROR;

    //char worldNameUnderlined[256];

    FaceRecord *pFace;

    char worldChar[MAX_PATH];

#define HEADER_COUNT 8
    char *header[HEADER_COUNT] = {
        "\nNavigationInfo {\n",
        "  type [ \"EXAMINE\", \"ANY\" ]\n",
        "}\n",
        "Transform {\n",
        "  scale 1 1 1\n",
        "  translation 0 0 0\n",
        "  children\n",
        "  [\n"
    };

#define SHAPESTART_COUNT 5
    char *shapeStart[SHAPESTART_COUNT] = {
        "    Shape\n",
        "    {\n",
        "      geometry IndexedFaceSet\n",
        "      {\n",
        "        creaseAngle .5\n"
    };

#define SHAPECONTINUE_COUNT 4
    char *shapeContinue[SHAPECONTINUE_COUNT] = {
        //"        solid FALSE\n",   // TODO - could probably set this true?
        "        coord Coordinate\n",
        "        {\n",
        "          point\n",
        "          [\n"
    };


#define WRL_MATERIAL_TEXT_COUNT 11
    char *materialText[WRL_MATERIAL_TEXT_COUNT] = {
        "        ]\n",
        "      }\n",
        "      appearance Appearance\n",
        "      {\n",
        "        material Material\n",
        "        {\n",
        "	       ambientIntensity 0.2\n",
        "	       diffuseColor 0.9 0.9 0.9\n",
        "	       specularColor .1 .1 .1\n",
        "	       shininess .5\n",
        "        }\n"
    };

    concatFileName3(wrlFileNameWithSuffix, gOutputFilePath, gOutputFileRoot, L".wrl");

    // create the VRML wrl file
    gModelFile=PortaCreate(wrlFileNameWithSuffix);
    addOutputFilenameToList(wrlFileNameWithSuffix);
    if (gModelFile == INVALID_HANDLE_VALUE)
        return MW_CANNOT_CREATE_FILE;

    wcharToChar(world,worldChar);
    justWorldFileName = removePathChar(worldChar);

    sprintf_s(outputString,256,"#VRML V2.0 utf8\n\n# VRML 97 (VRML2) file made by Mineways, http://mineways.com\n" );
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    retCode = writeStatistics( gModelFile, justWorldFileName, worldBox );
    if ( retCode >= MW_BEGIN_ERRORS )
        goto Exit;

    retCode |= writeLines( gModelFile, header, HEADER_COUNT );
    if ( retCode >= MW_BEGIN_ERRORS )
        goto Exit;

    retCode |= writeLines( gModelFile, shapeStart, SHAPESTART_COUNT );
    if ( retCode >= MW_BEGIN_ERRORS )
        goto Exit;

    // definitely solid if 3d printing.
    sprintf_s(outputString,256,"        solid %s\n", ( gOptions->exportFlags & EXPT_3DPRINT ) ? "TRUE" : "FALSE" );
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    retCode |= writeLines( gModelFile, shapeContinue, SHAPECONTINUE_COUNT );
    if ( retCode >= MW_BEGIN_ERRORS )
        goto Exit;

    // Note that we just dump everything to a single indexed face set. This format is here purely to
    // allow colored sandstone models to be created.
    for ( i = 0; i < gModel.vertexCount; i++ )
    {
        if ( i % 1000 == 0 )
            UPDATE_PROGRESS( PG_OUTPUT + 0.4f*(PG_TEXTURE-PG_OUTPUT)*((float)i/(float)gModel.vertexCount));

        if ( i == gModel.vertexCount-1)
        {
            // no comma at end
            sprintf_s(outputString,256,"            %g %g %g\n", gModel.vertices[i][X], gModel.vertices[i][Y], gModel.vertices[i][Z] );
        }
        else
        {
            sprintf_s(outputString,256,"            %g %g %g,\n", gModel.vertices[i][X], gModel.vertices[i][Y], gModel.vertices[i][Z] );
        }
        WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));
    }

    // note we don't need to output the normals, since the crease angle will compute them properly! Could output them for speed.
    //// write out normals, texture coordinates, vertices, and then faces grouped by material
    //for ( i = 0; i < 6; i++ )
    //{
    //    sprintf_s(outputString,256,"vn %g %g %g\n", gModel.normals[i][0], gModel.normals[i][1], gModel.normals[i][2]);
    //    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString)));
    //}

    strcpy_s(outputString,256,"          ]\n        }\n\n        texCoord TextureCoordinate\n        {\n          point\n          [\n");
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    for ( i = 0; i < gModel.textureUsedCount; i++ )
    {
        retCode |=  writeVRMLTextureUVs(gModel.uvIndexToSwatch[i] );
        if ( retCode >= MW_BEGIN_ERRORS )
            goto Exit;
    }

    strcpy_s(outputString,256,"          ]\n        }\n        texCoordIndex\n          [\n");
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    // output texture coordinate loops
    for ( i = 0; i < gModel.faceCount; i++ )
    {
        if ( i % 1000 == 0 )
            UPDATE_PROGRESS( PG_OUTPUT + 0.4f*(PG_TEXTURE-PG_OUTPUT) +  0.3f*(PG_TEXTURE-PG_OUTPUT)*((float)i/(float)gModel.faceCount));

        // output the face loop
        pFace = gModel.faceList[i];

        for ( j = 0; j < 2; j++ )
        {
            sprintf_s(outputString,256,"          %d %d %d -1\n",
                pFace->uvIndex[0],
                pFace->uvIndex[j+1],
                pFace->uvIndex[j+2]);
            WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));
        }
    }

    strcpy_s(outputString,256,"          ]\n        coordIndex\n          [\n");
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    // output vertex coordinate loops
    for ( i = 0; i < gModel.faceCount; i++ )
    {
        if ( i % 1000 == 0 )
            UPDATE_PROGRESS( PG_OUTPUT + 0.7f*(PG_TEXTURE-PG_OUTPUT) + 0.3f*(PG_TEXTURE-PG_OUTPUT)*((float)i/(float)gModel.faceCount));

        // output the face loop
        pFace = gModel.faceList[i];

        for ( j = 0; j < 2; j++ )
        {
            // comma test
            if ( i == gModel.faceCount-1 && j == 2 )
            {
                sprintf_s(outputString,256,"          %d,%d,%d,-1\n",
                    pFace->vertexIndex[0],
                    pFace->vertexIndex[j+1],
                    pFace->vertexIndex[j+2]);
            }
            else
            {
                sprintf_s(outputString,256,"          %d,%d,%d,-1,\n",
                    pFace->vertexIndex[0],
                    pFace->vertexIndex[j+1],
                    pFace->vertexIndex[j+2]);
            }
            WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));
        }
    }

    // generic material
    retCode |= writeLines( gModelFile, materialText, WRL_MATERIAL_TEXT_COUNT );
    if ( retCode >= MW_BEGIN_ERRORS )
        goto Exit;

    // output texture file name
    // get texture name to export, if needed
    sprintf_s(justTextureFileName,MAX_PATH,"%s.png",gOutputFileRootCleanChar);
    sprintf_s(outputString,256,"        texture ImageTexture { url \"%s\" }\n", justTextureFileName );
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    // close her up
    strcpy_s(outputString,256,"      }\n    }\n  ]\n}\n");
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));


    Exit:
    PortaClose(gModelFile);

    return retCode;
}

static int writeVRMLTextureUVs( int type )
{
#ifdef WIN32
    DWORD br;
#endif
    char outputString[1024];
    float umin, umax, vmin, vmax;

    assert( gModel.uvSwatchToIndex[type] >= 0 );
    assert( type < NUM_MAX_SWATCHES );
    assert( gModel.uvSwatchToType[type] < NUM_BLOCKS );

    // sample the center of the swatch, so that there's no leakage.
    // Really, it's overkill that we're using 18x18 color swatches and
    // sampling the 16x16 center, but it makes it nice for people wanting to
    // edit the texture directly and clears the way for true texture output.
    getTextureBounds( type, &umin, &umax, &vmin, &vmax );

    sprintf_s(outputString,1024,"           # %s\n            %g %g\n            %g %g\n            %g %g\n            %g %g\n",
        gBlockDefinitions[gModel.uvSwatchToType[type]].name,
        umin,vmax,
        umax,vmax,
        umax,vmin,
        umin,vmin);
    WERROR(PortaWrite(gModelFile, outputString, strlen(outputString) ));

    return MW_NO_ERROR;
}

static int writeLines( HANDLE file, char **textLines, int lines )
{
#ifdef WIN32
    DWORD br;
#endif

    int i;
    for ( i = 0; i < lines; i++ )
    {
        WERROR(PortaWrite(file, textLines[i], strlen(textLines[i]) ));
    }

    return MW_NO_ERROR;
}

static float max3( Point pt )
{
    float retVal = max( pt[0], pt[1] );
    return max( retVal, pt[2] );
}
static float med3( Point pt )
{
    float retVal = max( pt[0], pt[1] );
    if ( retVal > pt[2] )
    {
        // old retVal is maximum, so compare other two
        retVal = min( pt[0], pt[1] );
        retVal = max( retVal, pt[2] );
    }
    return retVal;
}
static float min3( Point pt )
{
    float retVal = min( pt[0], pt[1] );
    return min( retVal, pt[2] );
}

static int writeStatistics( HANDLE fh, const char *justWorldFileName, IBox *worldBox )
{
#ifdef WIN32
    DWORD br;
#endif

    char outputString[256];
    char timeString[256];
    char warningString[256];
    errno_t errNum;
    struct tm newtime;
    __time32_t aclock;

    int radio;
    float angle;

    char *outputTypeString[] = {
        "Export no materials",
        "Export solid material colors only (no textures)",
        "Export richer color textures",
        "Export full color texture patterns"
    };

    float inCM = gModel.scale * METERS_TO_CM;
    float inCM3 = inCM * inCM * inCM;

    sprintf_s(outputString,256,"# Extracted from Minecraft world %s\n", justWorldFileName );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    _time32( &aclock );   // Get time in seconds.
    _localtime32_s( &newtime, &aclock );   // Convert time to struct tm form.

    // Print local time as a string.
    errNum = asctime_s(timeString, 32, &newtime);
    if (!errNum)
    {
        sprintf_s(outputString,256,"# %s", timeString );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    sprintf_s(outputString,256,"# Created for %s\n", (gOptions->exportFlags & EXPT_3DPRINT) ? "3D printing" : "Viewing" );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    if ( gOptions->exportFlags & EXPT_3DPRINT )
    {
        // If we add materials, put the material chosen here.
        sprintf_s(outputString,256,"\n# Cost estimate for this model:\n");
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        sprintf_s(warningString,256,"%s", (gModel.scale < mtlCostTable[PRINT_MATERIAL_WHITE_STRONG_FLEXIBLE].minWall) ? " *** WARNING, thin wall ***" : "" );
        sprintf_s(outputString,256,"#   if made using the white, strong & flexible material: $ %0.2f%s\n",
            computeMaterialCost( PRINT_MATERIAL_WHITE_STRONG_FLEXIBLE, gModel.scale, gBlockCount, gStats.density ),
            warningString);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        sprintf_s(warningString,256,"%s", (gModel.scale < mtlCostTable[PRINT_MATERIAL_FULL_COLOR_SANDSTONE].minWall) ? " *** WARNING, thin wall ***" : "" );
        sprintf_s(outputString,256,"#   if made using the full color sandstone material:     $ %0.2f%s\n",
            computeMaterialCost( PRINT_MATERIAL_FULL_COLOR_SANDSTONE, gModel.scale, gBlockCount, gStats.density ),
            warningString);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        // if material is not one of these, print its cost
        if ( gPhysMtl > PRINT_MATERIAL_FULL_COLOR_SANDSTONE )
        {
            sprintf_s(warningString,256,"%s", (gModel.scale < mtlCostTable[gPhysMtl].minWall) ? " *** WARNING, thin wall ***" : "" );
            sprintf_s(outputString,256,
                "#   if made using the %s material:     $ %0.2f%s\n",
                mtlCostTable[gPhysMtl].name,
                computeMaterialCost( gPhysMtl, gModel.scale, gBlockCount, gStats.density ),
                warningString);
            WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
        }
        gOptions->cost = computeMaterialCost( gPhysMtl, gModel.scale, gBlockCount, gStats.density );

        sprintf_s(outputString,256, "# For %s printer, minimum wall is %g mm, maximum size is %g x %g x %g cm\n", mtlCostTable[gPhysMtl].name, mtlCostTable[gPhysMtl].minWall*METERS_TO_MM,
            mtlCostTable[gPhysMtl].maxSize[0], mtlCostTable[gPhysMtl].maxSize[1], mtlCostTable[gPhysMtl].maxSize[2] );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    sprintf_s(outputString,256,"# Units for the model vertex data itself: %s\n", unitTypeTable[gOptions->pEFD->comboModelUnits[gOptions->pEFD->fileType]].name );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    if ( gOptions->exportFlags & EXPT_3DPRINT )
    {
        float area, volume, sumOfDimensions;
        char errorString[256];

        if ( inCM * max3(gFilledBoxSize) > mtlCostTable[gPhysMtl].maxSize[0] ||
            inCM * med3(gFilledBoxSize) > mtlCostTable[gPhysMtl].maxSize[1] ||
            inCM * min3(gFilledBoxSize) > mtlCostTable[gPhysMtl].maxSize[2] )
        {
            sprintf_s(errorString,256," *** WARNING, too large for %s printer", mtlCostTable[gPhysMtl].name);
        }
        else
        {
            errorString[0] = '\0';
        }

        gOptions->dim_cm[X] = inCM * gFilledBoxSize[X];
        gOptions->dim_cm[Y] = inCM * gFilledBoxSize[Y];
        gOptions->dim_cm[Z] = inCM * gFilledBoxSize[Z];
        sprintf_s(outputString,256,"\n# world dimensions: %0.2f x %0.2f x %0.2f cm%s\n",
            gOptions->dim_cm[X], gOptions->dim_cm[Y], gOptions->dim_cm[Z], errorString);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        gOptions->dim_inches[X] = inCM * gFilledBoxSize[X]/2.54f;
        gOptions->dim_inches[Y] = inCM * gFilledBoxSize[Y]/2.54f;
        gOptions->dim_inches[Z] = inCM * gFilledBoxSize[Z]/2.54f;
        sprintf_s(outputString,256,"#   in inches: %0.2f x %0.2f x %0.2f inches%s\n",
            gOptions->dim_inches[X], gOptions->dim_inches[Y], gOptions->dim_inches[Z], errorString );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        sprintf_s(outputString,256,"# each block is %0.2f mm on a side, and has a volume of %g mm^3\n", gModel.scale*METERS_TO_MM, inCM3*1000 );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        sumOfDimensions = 10*inCM *(gFilledBoxSize[X]+gFilledBoxSize[Y]+gFilledBoxSize[Z]);
        sprintf_s(outputString,256,"# sum of dimensions: %g mm\n", sumOfDimensions );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        volume = inCM3 * gBlockCount;
        sprintf_s(outputString,256,"# volume is %g cm^3\n", volume );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        area = AREA_IN_CM2 ;
        sprintf_s(outputString,256,"# surface area is %g cm^2\n", area );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        sprintf_s(outputString,256,"# block density: %d%% of volume\n",
            (int)(gStats.density*100.0f+0.5f));
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    // write out a summary, useful for various reasons
    if ( gExportBillboards )
    {
        sprintf_s(outputString,256,"\n# %d vertices, %d faces (%d triangles), %d blocks, %d billboards\n", gModel.vertexCount, gModel.faceCount, 2*gModel.faceCount, gBlockCount, gModel.billboardCount);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }
    else
    {
        sprintf_s(outputString,256,"\n# %d vertices, %d faces (%d triangles), %d blocks\n", gModel.vertexCount, gModel.faceCount, 2*gModel.faceCount, gBlockCount);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }
    gOptions->totalBlocks = gBlockCount;

    sprintf_s(outputString,256,"# block dimensions: X=%g by Y=%g (height) by Z=%g blocks\n", gFilledBoxSize[X], gFilledBoxSize[Y], gFilledBoxSize[Z] );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    Vec2Op(gOptions->dimensions, =, (int)gFilledBoxSize);

    // Summarize all the options used for output
    sprintf_s(outputString,256,"\n# Selection location: %d, %d, %d to %d, %d, %d\n\n",
        worldBox->min[X], worldBox->min[Y], worldBox->min[Z],
        worldBox->max[X], worldBox->max[Y], worldBox->max[Z] );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    if ( gOptions->exportFlags & EXPT_OUTPUT_MATERIALS )
    {
        if ( gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_SWATCHES )
            radio = 2;
        else if ( gOptions->exportFlags & EXPT_OUTPUT_TEXTURE_IMAGES )
            radio = 3;
        else
            radio = 1;
    }
    else
        radio = 0;

    sprintf_s(outputString,256,"# File type: %s\n", outputTypeString[radio] );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    sprintf_s(outputString,256,"# Make Z direction up: %s\n", gOptions->pEFD->chkMakeZUp ? "YES" : "no" );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    sprintf_s(outputString,256,"# Center model: %s\n", gOptions->pEFD->chkCenterModel ? "YES" : "no" );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    sprintf_s(outputString,256,"# Export all block types: %s\n", gOptions->pEFD->chkExportAll ? "YES" : "no" );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    sprintf_s(outputString,256,"# Merge flat blocks with neighbors: %s\n", gOptions->pEFD->chkMergeFlattop ? "YES" : "no" );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    if ( gOptions->pEFD->radioRotate0 )
        angle = 0;
    else if ( gOptions->pEFD->radioRotate90 )
        angle = 90;
    else if ( gOptions->pEFD->radioRotate180 )
        angle = 180;
    else
    {
        angle = 270;
        assert(gOptions->pEFD->radioRotate270);
    }

    sprintf_s(outputString,256,"# Rotate model %d degrees\n", angle );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    if ( gOptions->pEFD->radioScaleByBlock )
    {
        sprintf_s(outputString,256,"# Scale model by making each block %g mm high\n", gOptions->pEFD->blockSizeVal[gOptions->pEFD->fileType] );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }
    else if ( gOptions->pEFD->radioScaleByCost )
    {
        sprintf_s(outputString,256,"# Scale model by aiming for a cost of %0.2f for the %s material\n", gOptions->pEFD->costVal, mtlCostTable[gPhysMtl] );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }
    else if ( gOptions->pEFD->radioScaleToHeight )
    {
        sprintf_s(outputString,256,"# Scale model by fitting to a height of %g cm\n", gOptions->pEFD->modelHeightVal );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }
    else if ( gOptions->pEFD->radioScaleToMaterial )
    {
        sprintf_s(outputString,256,"# Scale model by using the minimum wall thickness for the %s material\n", mtlCostTable[gPhysMtl] );
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    sprintf_s(outputString,256,"# Data operation options:\n#   Fill air bubbles: %s; Seal off entrances: %s; Fill in isolated tunnels in base of model: %s\n",
        (gOptions->pEFD->chkFillBubbles ? "YES" : "no"),
        (gOptions->pEFD->chkSealEntrances ? "YES" : "no"),
        (gOptions->pEFD->chkSealSideTunnels ? "YES" : "no"));
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    sprintf_s(outputString,256,"#   Connect parts sharing an edge: %s; Connect corner tips: %s; Weld all shared edges: %s\n",
        (gOptions->pEFD->chkConnectParts ? "YES" : "no"),
        (gOptions->pEFD->chkConnectCornerTips ? "YES" : "no"),
        (gOptions->pEFD->chkShowWelds ? "YES" : "no"));
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    sprintf_s(outputString,256,"#   Delete floating objects: trees and parts smaller than %d blocks: %s\n",
        gOptions->pEFD->floaterCountVal,
        (gOptions->pEFD->chkDeleteFloaters ? "YES" : "no"));
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    sprintf_s(outputString,256,"#   Hollow out bottom of model, making the walls %g mm thick: %s; Superhollow: %s\n",
        gOptions->pEFD->hollowThicknessVal[gOptions->pEFD->fileType],
        (gOptions->pEFD->chkHollow ? "YES" : "no"),
        (gOptions->pEFD->chkSuperHollow ? "YES" : "no"));
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    sprintf_s(outputString,256,"# Melt snow blocks: %s\n", gOptions->pEFD->chkMeltSnow ? "YES" : "no" );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    sprintf_s(outputString,256,"#   Debug: show separate parts as colors: %s\n", gOptions->pEFD->chkShowParts ? "YES" : "no" );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    sprintf_s(outputString,256,"#   Debug: show weld blocks in bright colors: %s\n", gOptions->pEFD->chkShowWelds ? "YES" : "no" );
    WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

    // write out processing stats for 3D printing
    if ( gOptions->exportFlags & (EXPT_FILL_BUBBLES|EXPT_CONNECT_PARTS|EXPT_DELETE_FLOATING_OBJECTS) )
    {
        sprintf_s(outputString,256,"\n# Cleanup processing summary:\n#   Solid parts: %d\n",
            gStats.numSolidGroups);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    if ( gOptions->exportFlags & EXPT_FILL_BUBBLES )
    {
        sprintf_s(outputString,256,"#   Air bubbles found and filled (with glass): %d\n",
            gStats.bubblesFound);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    if ( gOptions->exportFlags & (EXPT_FILL_BUBBLES|EXPT_CONNECT_PARTS) )
    {
        sprintf_s(outputString,256,"#   Total solid parts merged: %d\n",
            gStats.solidGroupsMerged);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    if ( gOptions->exportFlags & EXPT_CONNECT_PARTS )
    {
        sprintf_s(outputString,256,"#   Number of edge passes made: %d\n",
            gStats.numberManifoldPasses);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        sprintf_s(outputString,256,"#     Edges found to fix: %d\n",
            gStats.nonManifoldEdgesFound);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        sprintf_s(outputString,256,"#     Weld blocks added: %d\n",
            gStats.blocksManifoldWelded);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    if ( gOptions->exportFlags & EXPT_CONNECT_CORNER_TIPS )
    {
        sprintf_s(outputString,256,"#     Tip blocks added: %d\n",
            gStats.blocksCornertipWelded);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    if ( gOptions->exportFlags & EXPT_DELETE_FLOATING_OBJECTS )
    {
        sprintf_s(outputString,256,"#   Floating parts removed: %d\n",
            gStats.floaterGroupsDeleted);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        sprintf_s(outputString,256,"#     In these floaters, total blocks removed: %d\n",
            gStats.blocksFloaterDeleted);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    if ( gOptions->exportFlags & EXPT_HOLLOW_BOTTOM )
    {
        sprintf_s(outputString,256,"#   Blocks removed by hollowing: %d\n",
            gStats.blocksHollowed);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));

        sprintf_s(outputString,256,"#   Blocks removed by further super-hollowing (i.e. not just vertical hollowing): %d\n",
            gStats.blocksSuperHollowed);
        WERROR(PortaWrite(fh, outputString, strlen(outputString) ));
    }

    return MW_NO_ERROR;
}

// final checks:
// if color, is sum of dimensions too small?
// else, is wall thickness dangerous?
static int finalModelChecks()
{
    int retCode = MW_NO_ERROR;

    // go from most to least serious
    // were there multiple groups?
    if ( gSolidGroups > 1 && (gOptions->exportFlags & EXPT_3DPRINT) )
    {
        // we care only if exporting to 3D print
        retCode |= MW_MULTIPLE_GROUPS_FOUND;
    }
    if ( gOptions->exportFlags & EXPT_3DPRINT )
    {
        float inCM = gModel.scale * METERS_TO_CM;
        // check that dimensions are not too large
        if ( inCM * max3(gFilledBoxSize) > mtlCostTable[gPhysMtl].maxSize[0] ||
            inCM * med3(gFilledBoxSize) > mtlCostTable[gPhysMtl].maxSize[1] ||
            inCM * min3(gFilledBoxSize) > mtlCostTable[gPhysMtl].maxSize[2] )
        {
            retCode |= MW_AT_LEAST_ONE_DIMENSION_TOO_HIGH;
        }
        // check dimension sum for material output
        // Really needed only for colored sandstone: http://www.shapeways.com/design-rules/full_color_sandstone
        if ( (gFilledBoxSize[X]+gFilledBoxSize[Y]+gFilledBoxSize[Z]) < mtlCostTable[gPhysMtl].minDimensionSum*METERS_TO_MM) {
            retCode |= MW_SUM_OF_DIMENSIONS_IS_LOW;
        }
        if ( gModel.scale < mtlCostTable[gPhysMtl].minWall )
        {
            retCode |= MW_WALLS_MIGHT_BE_THIN;
        }

        // it's not clear from http://www.shapeways.com/tutorials/how_to_use_meshlab_and_netfabb whether
        // it's one million polygons (we use quads) or one million triangles. We opt for triangles here:
        if ( gModel.faceCount*2 > 1000000 )
        {
            retCode |= MW_TOO_MANY_POLYGONS;
        }
    }

    return retCode;
}

static float computeMaterialCost( int printMaterialType, float blockEdgeSize, int numBlocks, float densityRatio )
{
    float ccmMaterial = (float)pow((double)(blockEdgeSize*METERS_TO_CM),3.0)*(float)numBlocks;
    if ( densityRatio > mtlCostTable[printMaterialType].costDiscountDensityLevel && ccmMaterial > mtlCostTable[printMaterialType].costDiscountCCMLevel)
    {
        // density & size discount applies.
        return ( mtlCostTable[printMaterialType].costHandling + 
            mtlCostTable[printMaterialType].costPerSquareCentimeter * AREA_IN_CM2 +    // ceramics only
            mtlCostTable[printMaterialType].costPerCubicCentimeter * 20.0f +
            // we assume discount is always 50% here:
            0.50f * mtlCostTable[printMaterialType].costPerCubicCentimeter * (ccmMaterial - mtlCostTable[printMaterialType].costDiscountCCMLevel));
    }
    else
    {
        return ( mtlCostTable[printMaterialType].costHandling + 
            mtlCostTable[printMaterialType].costPerSquareCentimeter * AREA_IN_CM2 +    // ceramics only
            mtlCostTable[printMaterialType].costPerCubicCentimeter * ccmMaterial);
    }
}

static void addOutputFilenameToList(wchar_t *filename)
{
    assert ( gOutputFileList->count < MAX_OUTPUT_FILES );

    wcsncpy_s(gOutputFileList->name[gOutputFileList->count],MAX_PATH,filename,MAX_PATH);
    gOutputFileList->count++;
}

// substitute ' ' to '_'
static void spacesToUnderlines( wchar_t *targetString )
{
    wchar_t *strPtr;

    strPtr = wcschr(targetString,(int)' ');
    while ( strPtr )
    {
        // changes targetString itself
        *strPtr = '_';
        strPtr = wcschr(targetString,(int)' ');
    }
}

// substitute ' ' to '_'
static void spacesToUnderlinesChar( char *targetString )
{
    char *strPtr;

    strPtr = strchr(targetString,(int)' ');
    while ( strPtr )
    {
        // changes targetString itself
        *strPtr = '_';
        strPtr = strchr(targetString,(int)' ');
    }
}

#define GET_PNG_TEXEL( r,g,b,a, ip ) \
    (r) = (unsigned char)((ip) & 0xff); \
    (g) = (unsigned char)(((ip)>>8) & 0xff); \
    (b) = (unsigned char)(((ip)>>16) & 0xff); \
    (a) = (unsigned char)(((ip)>>24) & 0xff);

#define SET_PNG_TEXEL( ip, r,g,b,a ) \
    (ip) = ((a)<<24)|((b)<<16)|((g)<<8)|(r);

static void copyPNGArea(progimage_info *dst, int dst_x_min, int dst_y_min, int size_x, int size_y, progimage_info *src, int src_x_min, int src_y_min)
{
    int row;
    int dst_offset,src_offset;

    for ( row = 0; row < size_y; row++ )
    {
        dst_offset = ((dst_y_min+row)*dst->width + dst_x_min) * 4;
        src_offset = ((src_y_min+row)*src->width + src_x_min) * 4;
        memcpy(dst->image_data+dst_offset, src->image_data+src_offset, size_x*4);
    }
}

static int tileIsSemitransparent(progimage_info *src, int col, int row)
{
    int r,c;

    for ( r = 0; r < gModel.tileSize; r++ )
    {
        unsigned char *src_offset = src->image_data + ((row*gModel.tileSize + r)*src->width + col*gModel.tileSize) * 4 + 3;
        for ( c = 0; c < gModel.tileSize; c++ )
        {
            if ( *src_offset <= 0 || *src_offset >= 255 )
            {
                return 0;
            }
            src_offset += 4;
        }
    }
    return 1;
}

static int tileIsCutout(progimage_info *src, int col, int row)
{
    int r,c;
    int clearFound=0;
    int solidFound=0;

    for ( r = 0; r < gModel.tileSize; r++ )
    {
        unsigned char *src_offset = src->image_data + ((row*gModel.tileSize + r)*src->width + col*gModel.tileSize) * 4 + 3;
        for ( c = 0; c < gModel.tileSize; c++ )
        {
            // we could also test if the tile is semitransparent and reject if it is;
            // this seems extreme, I can imagine alternate texture packs going semitransparent
            //if ( *src_offset > 0 || *src_offset < 255 )
            //{
            //    return 0;
            //}
            //else
            // look for if pixels found that are fully transparent or fully solid
            if ( *src_offset == 0 )
            {
                clearFound=1;
            }
            else if ( *src_offset == 255 )
            {
                solidFound=1;
            }
            src_offset += 4;
        }
    }
    return (clearFound && solidFound);
}

static int tileIsOpaque(progimage_info *src, int col, int row)
{
    int r,c;

    for ( r = 0; r < gModel.tileSize; r++ )
    {
        unsigned char *src_offset = src->image_data + ((row*gModel.tileSize + r)*src->width + col*gModel.tileSize) * 4 + 3;
        for ( c = 0; c < gModel.tileSize; c++ )
        {
            if ( *src_offset < 255 )
            {
                return 0;
            }
            src_offset += 4;
        }
    }
    return 1;
}

static void setColorPNGArea(progimage_info *dst, int dst_x_min, int dst_y_min, int size_x, int size_y, unsigned int value)
{
    int row,col;
    int dst_offset;
    unsigned int *di = ((unsigned int *)(dst->image_data)) + (dst_y_min * dst->width + dst_x_min);

    for ( row = 0; row < size_y; row++ )
    {
        dst_offset = row * dst->width;
        for ( col = 0; col < size_x; col++ )
        {
            *(di+dst_offset+col) = value;
        }
    }
}

// startStretch is how far down the stretch should start from, e.g. 0.25 means start 1/4th the way down and stretch 3/4ths of the content up
static void stretchSwatchToTop(progimage_info *dst, int swatchIndex, float startStretch)
{
    int row,drow,dcol,dst_offset,src_offset;
    unsigned int *di,*si;
    SWATCH_TO_COL_ROW( swatchIndex, dcol, drow );
    di = ((unsigned int *)(dst->image_data)) + (drow * gModel.swatchSize * dst->width + dcol * gModel.swatchSize);
    si = di + (int)(startStretch * gModel.swatchSize) * dst->width;

    for ( row = 0; row < gModel.swatchSize; row++ )
    {
        dst_offset = row * dst->width;
        src_offset = (int)(row * (1.0f-startStretch)) * dst->width ;

        memcpy(di+dst_offset, si+src_offset, gModel.swatchSize*4);
    }
}

static void copyPNGTile(progimage_info *dst, int dst_x, int dst_y, int tileSize, progimage_info *src, int src_x, int src_y)
{
    copyPNGArea(dst, dst_x*tileSize, dst_y*tileSize, tileSize, tileSize, src, src_x*tileSize, src_y*tileSize );
}

static void setColorPNGTile(progimage_info *dst, int x, int y, int tileSize, unsigned int value)
{
    int row, col;
    unsigned int *di;

    assert( x*tileSize+tileSize-1 < dst->width );

    for ( row = 0; row < tileSize; row++ )
    {
        di = ((unsigned int *)(dst->image_data)) + ((y*tileSize + row) * dst->width + x*tileSize);
        for ( col = 0; col < tileSize; col++ )
        {
            *di++ = value;
        }
    }
}

// value is the amount to subtract things from white: 0 is no noise, 1.0 is total noise
static void addNoisePNGTile(progimage_info *dst, int x, int y, int tileSize, unsigned char r, unsigned char g, unsigned char b, unsigned char a, float noise )
{
    int row, col;
    unsigned int *di;

    assert( x*tileSize+tileSize-1 < dst->width );

    for ( row = 0; row < tileSize; row++ )
    {
        di = ((unsigned int *)(dst->image_data)) + ((y*tileSize + row) * dst->width + x*tileSize);
        for ( col = 0; col < tileSize; col++ )
        {
            double grayscale = 1.0 - noise * myrand();
            unsigned char newr = (unsigned char)((double)r * grayscale);
            unsigned char newg = (unsigned char)((double)g * grayscale);
            unsigned char newb = (unsigned char)((double)b * grayscale);
            unsigned int value = (a<<24)|(newb<<16)|(newg<<8)|newr;
            *di++ = value;
        }
    }
}

static void multiplyPNGTile(progimage_info *dst, int x, int y, int tileSize, unsigned char r, unsigned char g, unsigned char b, unsigned char a )
{
    int row, col;
    unsigned int *di;

    assert( x*tileSize+tileSize-1 < dst->width );

    for ( row = 0; row < tileSize; row++ )
    {
        di = ((unsigned int *)(dst->image_data)) + ((y*tileSize + row) * dst->width + x*tileSize);
        for ( col = 0; col < tileSize; col++ )
        {
            unsigned int value = *di;
            unsigned char dr,dg,db,da;
            GET_PNG_TEXEL(dr,dg,db,da, value);
            SET_PNG_TEXEL(*di, (unsigned char)(dr * r / 255), (unsigned char)(dg * g / 255), (unsigned char)(db * b / 255), (unsigned char)(da * a / 255));
            di++;
        }
    }
}

// in-place form of compositePNGSwatches, with no blending: fill in destination with source wherever destination is 0 alpha
//static int fillZeroAlphasPNGSwatch(progimage_info *dst, int destSwatch, int sourceSwatch, int swatchSize, int swatchesPerRow)
//{
//    int scol = sourceSwatch % swatchesPerRow;
//    int srow = sourceSwatch / swatchesPerRow;
//    int dcol = destSwatch % swatchesPerRow;
//    int drow = destSwatch / swatchesPerRow;
//    int row, col;
//    unsigned int *si = (unsigned int *)(dst->image_data) + srow*swatchSize*dst->width + scol*swatchSize;
//    unsigned int *di = (unsigned int *)(dst->image_data) + drow*swatchSize*dst->width + dcol*swatchSize;
//    unsigned int *csi, *cdi;
//
//    for ( row = 0; row < swatchSize; row++ )
//    {
//        int offset = row*dst->width;
//        csi = si + offset;
//        cdi = di + offset;
//        for ( col = 0; col < swatchSize; col++ )
//        {
//            unsigned char da = (unsigned char)(((*cdi)>>24) & 0xff);
//            // if alpha is zero, set it to swatch color
//            if ( da == 0 )
//            {
//                *cdi++ = *csi++;
//            }
//            else
//            {
//                cdi++;
//                csi++;
//            }
//        }
//    }
//    return 0;
//
//}
//

static void rotatePNGTile(progimage_info *dst, int dcol, int drow, int scol, int srow, int angle, int swatchSize )
{
    int row, col;
    int m00,m01,m10,m11,offset0,offset1;
    unsigned int *dul = ((unsigned int*)dst->image_data) + drow*swatchSize*dst->width + dcol*swatchSize;
    unsigned int *sul = ((unsigned int*)dst->image_data) + srow*swatchSize*dst->width + scol*swatchSize;

    // cannot rotate in place: need to copy to somewhere in a separate call, then rotate back to final destination
    assert( ( dcol != scol ) || ( drow != srow ) );


    switch ( angle )
    {
    default:
    case 0:
        m00 = m11 = 1;
        m01 = m10 = 0;
        offset0 = offset1 = 0;
        break;
    case 90:
        m01 = 1;
        m10 = -1;
        m00 = m11 = 0;
        offset0 = swatchSize-1;
        offset1 = 0;
        break;
    case 180:
        m00 = m11 = -1;
        m01 = m10 = 0;
        offset0 = offset1 = swatchSize-1;
        break;
    case 270:
        m01 = -1;
        m10 = 1;
        m00 = m11 = 0;
        offset0 = 0;
        offset1 = swatchSize-1;
        break;
    }
    {
        for ( row = 0; row < swatchSize; row++ )
        {
            for ( col = 0; col < swatchSize; col++ )
            {
                unsigned int *di;
                unsigned int *si = sul + col + row*dst->width;
                int docol = col*m00 + row*m10 + offset0;
                int dorow = col*m01 + row*m11 + offset1;
                di = dul + docol + dorow*dst->width;
                *di = *si;
            }
        }
    }
}

static void blendTwoSwatches( progimage_info *dst, int txrSwatch, int solidSwatch, float blend, unsigned char alpha )
{
    // these are swatch locations in index form (not yet multiplied by swatch size itself)
    int tcol = txrSwatch % gModel.swatchesPerRow;
    int trow = txrSwatch / gModel.swatchesPerRow;
    int scol = solidSwatch % gModel.swatchesPerRow;
    int srow = solidSwatch / gModel.swatchesPerRow;
    // upper left corner, starting location, of each: over, under, destination
    unsigned int *ti = (unsigned int *)(dst->image_data) + trow*gModel.swatchSize*dst->width + tcol*gModel.swatchSize;
    unsigned int *si = (unsigned int *)(dst->image_data) + srow*gModel.swatchSize*dst->width + scol*gModel.swatchSize;

    int row,col;
    unsigned char tr,tg,tb,ta;
    unsigned char sr,sg,sb,sa;
    unsigned int *cti,*csi;

    for ( row = 0; row < gModel.swatchSize; row++ )
    {
        int offset = row*dst->width;
        cti = ti + offset;
        csi = si + offset;

        for ( col = 0; col < gModel.swatchSize; col++ )
        {
            float oneMinusBlend = 1.0f - blend;

            GET_PNG_TEXEL( tr,tg,tb,ta, *cti );
            GET_PNG_TEXEL( sr,sg,sb,sa, *csi );

            tr = (unsigned char)(tr*oneMinusBlend + sr*blend);
            tg = (unsigned char)(tg*oneMinusBlend + sg*blend);
            tb = (unsigned char)(tb*oneMinusBlend + sb*blend);
            ta = alpha;
            SET_PNG_TEXEL( *cti, tr,tg,tb,ta );
            cti++;
            csi++;
        }
    }
}

static void compositePNGSwatches(progimage_info *dst, int dstSwatch, int overSwatch, int underSwatch, int swatchSize, int swatchesPerRow, int forceSolid )
{
    // these are swatch locations in index form (not yet multiplied by swatch size itself)
    int ocol = overSwatch % swatchesPerRow;
    int orow = overSwatch / swatchesPerRow;
    int ucol = underSwatch % swatchesPerRow;
    int urow = underSwatch / swatchesPerRow;
    int dcol = dstSwatch % swatchesPerRow;
    int drow = dstSwatch / swatchesPerRow;
    // upper left corner, starting location, of each: over, under, destination
    unsigned int *overi = (unsigned int *)(dst->image_data) + orow*swatchSize*dst->width + ocol*swatchSize;
    unsigned int *underi = (unsigned int *)(dst->image_data) + urow*swatchSize*dst->width + ucol*swatchSize;
    unsigned int *dsti = (unsigned int *)(dst->image_data) + drow*swatchSize*dst->width + dcol*swatchSize;

    int row,col;
    unsigned char or,og,ob,oa;
    unsigned char ur,ug,ub,ua;
    unsigned char dr,dg,db,da;
    unsigned int *coveri,*cunderi,*cdsti;

    for ( row = 0; row < swatchSize; row++ )
    {
        int offset = row*dst->width;
        coveri = overi + offset;
        cunderi = underi + offset;
        cdsti = dsti + offset;
        for ( col = 0; col < swatchSize; col++ )
        {
            unsigned char oma;

            GET_PNG_TEXEL( or,og,ob,oa, *coveri );
            GET_PNG_TEXEL( ur,ug,ub,ua, *cunderi );

            oma = 255 - oa;

            if ( forceSolid )
                ua = 255;

            // finally have all data: composite o over u
            if ( oa == 0 )
            {
                // copy the under pixel
                *cdsti = *cunderi;
            }
            else if ( oma == 0 )
            {
                // copy the over pixel
                *cdsti = *coveri;
            }
            else
            {
                // full blend must be done: use over, http://en.wikipedia.org/wiki/Alpha_compositing
                dr = (unsigned char)((or*oa*255 + ur*ua*oma)/(255*255));
                dg = (unsigned char)((og*oa*255 + ug*ua*oma)/(255*255));
                db = (unsigned char)((ob*oa*255 + ub*ua*oma)/(255*255));
                da = (unsigned char)((oa*255 + ua*oma)/255);
                SET_PNG_TEXEL( *cdsti, dr,dg,db,da );
            }
            coveri++;
            cunderi++;
            cdsti++;
        }
    }
}

static int convertRGBAtoRGBandWrite(progimage_info *src, wchar_t *filename)
{
    int retCode = 0;
    int row, col;
    progimage_info dst;
    unsigned char *imageDst, *imageSrc;

    dst = *src;

    dst.color_type = PNG_COLOR_TYPE_RGB;
    dst.image_data = (unsigned char *)malloc(dst.width*dst.height*3*sizeof(unsigned char));
    dst.have_text = TEXT_TITLE|TEXT_AUTHOR|TEXT_DESC;
    dst.title = "Mineways RGB model texture";
    dst.author = "mineways.com";
    dst.desc = "Mineways texture file for model, generated from user's terrain.png";

    imageSrc = src->image_data;
    imageDst = dst.image_data;

    for ( row = 0; row < dst.height; row++ )
    {
        for ( col = 0; col < dst.width; col++ )
        {
            // copy RGB only
            *imageDst++ = *imageSrc++;
            *imageDst++ = *imageSrc++;
            *imageDst++ = *imageSrc++;
            imageSrc++;
        }
    }

    retCode = writepng( &dst, 3, filename );
    addOutputFilenameToList(filename);

    writepng_cleanup(&dst);
    free(dst.image_data);

    return retCode;
}

// for debugging
static void convertAlphaToGrayscale( progimage_info *dst )
{
    int row,col;
    unsigned int *di = ((unsigned int *)(dst->image_data));

    for ( row = 0; row < dst->height; row++ )
    {
        for ( col = 0; col < dst->width; col++ )
        {
            // get alpha of pixel, use as grayscale
            unsigned int value = *di;
            unsigned char dr,dg,db,da;
            GET_PNG_TEXEL(dr,dg,db,da, value);
            SET_PNG_TEXEL(*di, da, da, da, 255);
            di++;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////

//Sets the colors used from the color scheme.
//palette should be in RGBA format
void SetExportPalette(unsigned int *palette,int num)
{
    unsigned char r,g,b;
    unsigned char ra,ga,ba;
    float a;
    int i;

    for (i=0;i<num;i++)
    {
        r=(unsigned char)(palette[i]>>24);
        g=(unsigned char)(palette[i]>>16);
        b=(unsigned char)(palette[i]>>8);
        a=((float)(palette[i]&0xff))/255.0f;
        ra=(unsigned char)(r*a); //premultiply alpha
        ga=(unsigned char)(g*a);
        ba=(unsigned char)(b*a);
        gBlockDefinitions[i].color=(r<<16)|(g<<8)|b;
        gBlockDefinitions[i].pcolor=(ra<<16)|(ga<<8)|ba;
        gBlockDefinitions[i].alpha=a;
    }
}

///////////////////////////////////////////////////////////
//
// Utility functions, headers not included above
// suffix is of form ".stl" - includes dot
static void ensureSuffix( wchar_t *dst, const wchar_t *src, const wchar_t *suffix )
{
    int hasSuffix=0;

    // Prep file name: see if it has suffix already
    if ( wcsnlen(src,MAX_PATH) > wcsnlen(suffix,MAX_PATH) )
    {
        // look for suffix
        wchar_t foundSuffix[MAX_PATH];
        wcsncpy_s(foundSuffix,MAX_PATH,src + wcsnlen(src,MAX_PATH)-4,20);
        _wcslwr_s(foundSuffix,MAX_PATH);
        if (wcscmp(foundSuffix,suffix) == 0)
        {
            hasSuffix = 1;
        }
    }

    wcscpy_s(dst,MAX_PATH,src);
    // was the suffix found?
    if ( !hasSuffix )
    {
        //// see if it has *any* suffix
        //const wchar_t *noPath = removePath( dst );
        //wchar_t *dstDot = wcsrchr(noPath,(wchar_t )'.');
        //if ( dstDot )
        //{
        //    // found a dot in the filename itself - get rid of this suffix
        //    *dstDot = (wchar_t)0;
        //}
        // and add the suffix
        wcsncat_s(dst,MAX_PATH,suffix,20);  // 20's safe for a suffix
    }
}

static void removeSuffix( wchar_t *dst, const wchar_t *src, const wchar_t *suffix )
{
    // glue the suffix on if it's missing
    ensureSuffix( dst, src, suffix );
    // then remove it. Yeah, pretty silly, but minimal code
    if ( wcslen(dst) > wcslen(suffix) )
        dst[wcslen(dst)-wcslen(suffix)] = (wchar_t)0;
}

//static const wchar_t *removePath( const wchar_t *src )
//{
//    // find last \ in string
//    const wchar_t *strPtr = wcsrchr(src,(int)'\\');
//    if ( strPtr )
//        // found a \, so move up past it
//        strPtr++;
//    else
//    {
//        // look for /
//        strPtr = wcsrchr(src,(int)'/');
//        if ( strPtr )
//            // found a /, so move up past it
//            strPtr++;
//        else
//            // no \ or / found, just return string itself
//            return src;
//    }
//
//    return strPtr;
//}

static const char *removePathChar( const char *src )
{
    // find last \ in string
    const char *strPtr = strrchr(src,(int)'\\');
    if ( strPtr )
        // found a \, so move up past it
        strPtr++;
    else
    {
        // look for /
        const char *strPtr = strrchr(src,(int)'/');
        if ( strPtr )
            // found a /, so move up past it
            strPtr++;
        else
            // no \ or / found, just return string itself
            return src;
    }

    return strPtr;
}

static void getPathAndRoot( const wchar_t *src, int fileType, wchar_t *path, wchar_t *root )
{
    wchar_t *rootPtr;
    wchar_t tfilename[MAX_PATH];

    wcscpy_s(path,MAX_PATH,src);
    // find last \ in string
    rootPtr = wcsrchr(path,(wchar_t)'\\');
    if ( rootPtr )
        // found a \, so move up past it
        rootPtr++;
    else
    {
        // look for /
        rootPtr = wcsrchr(src,(wchar_t)'/');
        if ( rootPtr )
            // found a /, so move up past it
            rootPtr++;
        else
            // no \ or / found, just return string itself
            rootPtr = path;
    }
    // split at rootPtr
    wcscpy_s(tfilename,MAX_PATH,rootPtr);
    // this sets last character of path to null (end it)
    *rootPtr = (wchar_t)0;

    // The root doesn't really have the root at this point.
    // RemoveSuffix, depending on the file type
    switch ( fileType )
    {
    case FILE_TYPE_WAVEFRONT_REL_OBJ:
    case FILE_TYPE_WAVEFRONT_ABS_OBJ:
        removeSuffix(root,tfilename,L".obj");
        break;
    case FILE_TYPE_BINARY_MAGICS_STL:
    case FILE_TYPE_BINARY_VISCAM_STL:
    case FILE_TYPE_ASCII_STL:
        removeSuffix(root,tfilename,L".stl");
        break;
    case FILE_TYPE_VRML2:
        removeSuffix(root,tfilename,L".wrl");
        break;
    }
}

static void concatFileName2(wchar_t *dst, const wchar_t *src1, const wchar_t *src2)
{
    wcscpy_s(dst,MAX_PATH,src1);
    wcscat_s(dst,MAX_PATH-wcslen(dst),src2);
}

static void concatFileName3(wchar_t *dst, const wchar_t *src1, const wchar_t *src2, const wchar_t *src3)
{
    wcscpy_s(dst,MAX_PATH,src1);
    wcscat_s(dst,MAX_PATH-wcslen(dst),src2);
    wcscat_s(dst,MAX_PATH-wcslen(dst),src3);
}

static void concatFileName4(wchar_t *dst, const wchar_t *src1, const wchar_t *src2, const wchar_t *src3, const wchar_t *src4)
{
    wcscpy_s(dst,MAX_PATH,src1);
    wcscat_s(dst,MAX_PATH-wcslen(dst),src2);
    wcscat_s(dst,MAX_PATH-wcslen(dst),src3);
    wcscat_s(dst,MAX_PATH-wcslen(dst),src4);
}

// assumes MAX_PATH length for both strings
static void wcharToChar( const wchar_t *inWString, char *outString )
{
    //WideCharToMultiByte(CP_UTF8,0,inWString,-1,outString,MAX_PATH,NULL,NULL);
    int i;
    int oct=0;

    for ( i = 0; i < MAX_PATH; i++ )
    {
        int val = inWString[i];
        if (val >= 0 && val < 128)
        {
            outString[oct++] = (char)val;
        }
        if (val == 0)
        {
            // done: did anything get copied? (other than 0 at end)
            if ( oct <= 1 )
            {
                strcpy_s(outString,MAX_PATH,"mwExport");
            }
            return;
        }
    }
    // it is unlikely that the loop above didn't terminate and return
    assert(0);
    return;
}

static void charToWchar( char *inString, wchar_t *outWString )
{
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,inString,-1,outWString,MAX_PATH);
    //MultiByteToWideChar(CP_UTF8,0,inString,-1,outWString,MAX_PATH);
}

static void wcharCleanse( wchar_t *wstring )
{
    char tempString[MAX_PATH];
    wcharToChar( wstring, tempString );
    charToWchar( tempString, wstring );
}

#define M1  134456
#define IA1   8121
#define IC1  28411
#define RM1 1.0/M1

static void myseedrand( long seed )
{
    gMySeed = seed;
}

static double myrand()
{
    gMySeed = (IC1+gMySeed*IA1) % M1;
    return gMySeed * RM1;
}