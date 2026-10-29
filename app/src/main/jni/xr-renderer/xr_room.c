// 3D rooms: baked models drawn per eye into their own projection layer, with the
// picture hung on the far wall.
#include "xr_renderer.h"
#include "xr_shaders.h"
#include "xr_atlas.h"
#include "xr_roommesh.h"

// Derived per-room geometry and lighting, in viewer space
typedef struct {
    float halfWidth;
    float floorY;
    // Floor under the picture; lower than floorY (the viewer's tier) in a raked room
    float screenFloorY;
    float ceilingY;
    // Picture wall and the wall behind the viewer
    float screenZ;
    float backZ;
    // Light source position for the baked spill
    Vec3 screenAt;
    // Model-space point mapped to the viewer origin: geometry is (model - anchor) * scale
    Vec3 anchor;
    // Mount height, and offset from the wall to avoid z-fighting
    float screenMountY;
    float screenProud;
    // Screen anchor size: the largest the picture is hung. The room sizes the picture
    // itself and fits it inside with its aspect kept.
    float screenWidth;
    float screenHeight;
    // Whether the picture may extend past walls, floor and ceiling
    int open;
    // Distance at which the spill halves
    float spillRadius;
    // Spill taken by a fully lit vertex
    float spillGain;
    // Atlas contribution to textured parts, and overall dimming
    float texMix;
    float dim;
    unsigned seed;
} RoomParams;

// About 1/255 of dither per vertex, to stop shallow light gradients banding
static float roomDither(unsigned seed, unsigned index) {
    unsigned h = seed + index * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return ((float)(h & 0xffffu) / 65535.0f - 0.5f) * (2.0f / 255.0f);
}

// Spill weight at a point: distance and facing to a single point at the default
// screen position.
static float roomSpillWeight(const RoomParams* p, Vec3 pos, Vec3 normal) {
    Vec3 toScreen = vecSub(p->screenAt, pos);
    float dist = sqrtf(vecDot(toScreen, toScreen));
    if (dist < 1e-4f) {
        return 1.0f;
    }
    Vec3 dir = { toScreen.x / dist, toScreen.y / dist, toScreen.z / dist };
    float facing = vecDot(normal, dir);
    if (facing <= 0.0f) {
        return 0.0f;
    }

    float ratio = dist / p->spillRadius;
    float weight = facing / (1.0f + ratio * ratio);

    // Fade to zero behind the viewer over the back half of the room
    if (pos.z > 0.0f && p->backZ > 0.0f) {
        float behind = 1.0f - pos.z / p->backZ;
        weight *= behind > 0.0f ? behind : 0.0f;
    }
    return weight;
}

static void roomWriteVertex(const RoomParams* p, float* verts, int index, Vec3 pos,
                            const float* rgb, float spill, float u, float v) {
    float dither = roomDither(p->seed, (unsigned)index);
    float* out = verts + (size_t)index * ROOM_VERTEX_FLOATS;
    out[0] = pos.x;
    out[1] = pos.y;
    out[2] = pos.z;
    out[3] = rgb[0] + dither;
    out[4] = rgb[1] + dither;
    out[5] = rgb[2] + dither;
    out[6] = spill;
    out[7] = u;
    out[8] = v;
    out[9] = 0.0f;
}

// Active room: the debug override, else the picker's. 0 means no room.
int roomEffective(XrCtx* ctx) {
    return ctx->roomOverride >= 0 ? ctx->roomOverride : ctx->roomStyle;
}

// A room as measured from its model, in model space and metres. Each model has a
// Viewer_Seated node and a ScreenAnchor empty (picture centre and maximum size).
// All values go through the same (model - anchor) * scale as the geometry.
typedef struct {
    // Viewer_Seated position, and the viewer's offset from it (up, toward the screen),
    // tuned in headset
    Vec3 eye;
    float eyeRaise;
    float eyeForward;
    // ScreenAnchor origin and size (the picture's maximum)
    Vec3 screen;
    float screenWidth;
    float screenHeight;
    // Starting picture width as a fraction of the anchor, aspect kept
    float screenFraction;
    // Architecture bounds
    float floorY;
    float ceilingY;
    float halfWidth;
    float backZ;
    // Spill falloff radius and gain
    float spillRadius;
    float spillGain;
    // Starting brightness factor over the baked colours; the Room tab adjusts it
    float dim;
    // Dither seed; only needs to differ per room
    unsigned seed;
    float scale;
    // Whether the picture may be resized inside the anchor. Only for rooms where the
    // picture stands in open space, not on a wall built around it.
    int resizable;
    // Whether the picture may extend past the room bounds (open sky), or is clamped to
    // the walls
    int open;
    // Whether the glow starts on; off in dark rooms
    int glow;
} RoomModel;

// Indexed by style - ROOM_STYLE_FIRST. Room tab defaults live in xr_shared.h.
static const RoomModel ROOM_MODELS[] = {
    // Home Theater: picture on the front wall at 80% of the anchor (2.88 m). Brightness
    // tuned in headset.
    {
        .eye = { 0.0f, 1.15f, 1.12f }, .eyeRaise = 0.35f, .eyeForward = 0.10f,
        .screen = { 0.0f, 1.55f, -3.132f }, .screenWidth = 3.6f, .screenHeight = 2.025f,
        .screenFraction = ROOM_THEATER_SCREEN / 100.0f,
        .floorY = -0.14f, .ceilingY = 2.84f, .halfWidth = 2.8f, .backZ = 4.41f,
        .spillRadius = 3.0f, .spillGain = 0.55f, .dim = ROOM_THEATER_BRIGHTNESS / 100.0f,
        .seed = 0xc2b2ae35u, .scale = 1.0f, .resizable = ROOM_THEATER_RESIZABLE, .open = 0,
        .glow = ROOM_THEATER_GLOW,
    },
    // Grand Cinema: raked auditorium, viewed partway up; the picture fills the screen and
    // can't be resized.
    {
        .eye = { 0.0f, 4.8f, 6.0f }, .eyeRaise = 0.40f, .eyeForward = 0.0f,
        .screen = { 0.0f, 7.4f, -15.42f }, .screenWidth = 22.0f, .screenHeight = 12.375f,
        .screenFraction = ROOM_GRAND_CINEMA_SCREEN / 100.0f,
        .floorY = -0.3f, .ceilingY = 16.3f, .halfWidth = 14.3f, .backZ = 16.3f,
        .spillRadius = 8.0f, .spillGain = 0.55f, .dim = ROOM_GRAND_CINEMA_BRIGHTNESS / 100.0f,
        .seed = 0x85ebca6bu, .scale = 1.0f, .resizable = ROOM_GRAND_CINEMA_RESIZABLE,
        .open = 0, .glow = ROOM_GRAND_CINEMA_GLOW,
    },
    // Synthwave: open ground and sky, walls pushed out of reach. Picture 14 m wide at
    // y 4.5 (bottom 0.5 m off the ground), overriding the model's 10 x 5.625 at y 3.1.
    {
        .eye = { 0.0f, 1.2f, 0.0f }, .eyeRaise = 0.35f, .eyeForward = 0.10f,
        .screen = { 0.0f, 4.5f, -14.0f }, .screenWidth = 14.0f, .screenHeight = 7.875f,
        .screenFraction = ROOM_SYNTHWAVE_SCREEN / 100.0f,
        .floorY = 0.0f, .ceilingY = 1800.0f, .halfWidth = 1800.0f, .backZ = 1800.0f,
        .spillRadius = 6.0f, .spillGain = 0.4f, .dim = ROOM_SYNTHWAVE_BRIGHTNESS / 100.0f,
        .seed = 0x2545f491u, .scale = 1.0f, .resizable = ROOM_SYNTHWAVE_RESIZABLE,
        .open = 1, .glow = ROOM_SYNTHWAVE_GLOW,
    },
};

// One row per style
_Static_assert(sizeof(ROOM_MODELS) / sizeof(ROOM_MODELS[0])
               == ROOM_STYLE_LAST - ROOM_STYLE_FIRST + 1, "a room style with no row");

static int bakedRoomStyle(int style) {
    return style >= ROOM_STYLE_FIRST && style <= ROOM_STYLE_LAST;
}

// Unknown styles fall back to the first room
static const RoomModel* roomModel(int style) {
    if (!bakedRoomStyle(style)) {
        return &ROOM_MODELS[0];
    }
    return &ROOM_MODELS[style - ROOM_STYLE_FIRST];
}

// Viewer-space parameters for a style at a scale. The anchor is the seated eye point
// plus the offsets.
static RoomParams roomParams(int style, float scale) {
    const RoomModel* m = roomModel(style);

    RoomParams p;
    memset(&p, 0, sizeof(p));
    Vec3 anchor = { m->eye.x, m->eye.y + m->eyeRaise, m->eye.z - m->eyeForward };
    p.anchor = anchor;
    p.halfWidth = m->halfWidth * scale;
    p.floorY = (m->floorY - anchor.y) * scale;
    // Single floor level in all current rooms
    p.screenFloorY = p.floorY;
    p.ceilingY = (m->ceilingY - anchor.y) * scale;
    p.screenZ = (m->screen.z - anchor.z) * scale;
    p.backZ = (m->backZ - anchor.z) * scale;
    p.screenMountY = (m->screen.y - anchor.y) * scale;
    // Flush with the anchor
    p.screenProud = 0.0f;
    p.screenWidth = m->screenWidth * scale;
    p.screenHeight = m->screenHeight * scale;
    p.open = m->open;
    Vec3 screenAt = { (m->screen.x - anchor.x) * scale, p.screenMountY, p.screenZ };
    p.screenAt = screenAt;
    p.spillRadius = m->spillRadius * scale;
    p.spillGain = m->spillGain;
    p.texMix = 1.0f;
    p.dim = m->dim;
    p.seed = m->seed;
    return p;
}

// Whether the picture may be resized in this style's room; false outside a room
int roomResizable(int style) {
    return bakedRoomStyle(style) && roomModel(style)->resizable;
}

// Initialises each room's levels from the table, until preferences arrive
void roomLevelsFromTable(XrCtx* ctx) {
    for (int style = ROOM_STYLE_FIRST; style <= ROOM_STYLE_LAST; style++) {
        const RoomModel* m = roomModel(style);
        ctx->roomBrightness[style] = (int)roundf(m->dim * 100.0f);
        ctx->roomGlow[style] = m->glow != 0;
        ctx->roomLightLevel[style] = ROOM_LIGHT_DEFAULT;
        ctx->roomScreen[style] = (int)roundf(m->screenFraction * 100.0f);
    }
}

// Picture size in a room, percent of the anchor; fixed-size rooms always use 100
int roomScreenPercent(XrCtx* ctx, int style) {
    if (!bakedRoomStyle(style)) {
        return ROOM_SCREEN_MAX;
    }
    return roomScreenClamp(ctx->roomScreen[style], roomResizable(style));
}

// Glow on: the room's own switch in a room, else the global one
int roomGlowOn(XrCtx* ctx, int style) {
    return bakedRoomStyle(style) ? ctx->roomGlow[style] : ctx->ambilightOn;
}

// Whether the picture size is being dragged (suppresses per-step logging)
static int roomSizeDragging(XrCtx* ctx) {
    return ctx->grabMode == GRAB_RESIZE
            || (ctx->cogDragSlider == COG_ROOM_ROW_SIZE && ctx->cogDragFace == COG_FACE_ROOM);
}

// Room scale: the model's, unless overridden by a property within range
static float roomScale(XrCtx* ctx, int style) {
    if (!bakedRoomStyle(style)) {
        return 1.0f;
    }
    float scale = ctx->roomScaleOverride > 0.0f ? ctx->roomScaleOverride
                                                : roomModel(style)->scale;
    if (scale < ROOM_SCALE_MIN) {
        scale = ROOM_SCALE_MIN;
    }
    if (scale > ROOM_SCALE_MAX) {
        scale = ROOM_SCALE_MAX;
    }
    return scale;
}

// Far plane for this room: the larger of geometry reach and the table's walls, with
// margin, but never below ROOM_FAR_MIN_M.
static float roomFarPlane(const RoomParams* p, float reach) {
    float walls = p->halfWidth;
    if (fabsf(p->backZ) > walls) {
        walls = fabsf(p->backZ);
    }
    if (p->ceilingY > walls) {
        walls = p->ceilingY;
    }
    if (reach > walls) {
        walls = reach;
    }
    float far = walls * ROOM_FAR_MARGIN;
    return far < ROOM_FAR_MIN_M ? ROOM_FAR_MIN_M : far;
}

// Room dimming: the room's brightness row, overridden by the debug property. Applied
// at draw time, so no rebuild.
static float roomDim(XrCtx* ctx) {
    if (ctx->roomDimOverride <= 0.0f) {
        int style = ctx->roomBuiltStyle;
        return bakedRoomStyle(style) ? ctx->roomBrightness[style] / 100.0f : ctx->roomDim;
    }
    float dim = ctx->roomDimOverride;
    if (dim < ROOM_DIM_MIN) {
        dim = ROOM_DIM_MIN;
    }
    if (dim > ROOM_DIM_MAX) {
        dim = ROOM_DIM_MAX;
    }
    return dim;
}

// In a room the picture hangs on the far wall. The user's placement is saved on entry
// and restored on exit; preferences are not touched.
void applyRoomPlacement(XrCtx* ctx, int style, float aspect, int reseeded) {
    int roomOn = style > 0;
    if (roomOn && !ctx->roomHoldingScreen) {
        ctx->savedScreenPose = ctx->screenPose;
        ctx->savedScreenWidth = ctx->screenWidth;
        ctx->savedScreenRadius = ctx->screenRadius;
        ctx->roomHoldingScreen = 1;
        // Drop any grab, which would fight the wall
        ctx->grabMode = GRAB_NONE;
    }
    else if (roomOn && reseeded) {
        // A reset landed while in the room; restore that placement on exit
        ctx->savedScreenPose = ctx->screenPose;
        ctx->savedScreenWidth = ctx->screenWidth;
        ctx->savedScreenRadius = ctx->screenRadius;
    }
    else if (!roomOn && ctx->roomHoldingScreen) {
        ctx->screenPose = ctx->savedScreenPose;
        ctx->screenWidth = ctx->savedScreenWidth;
        ctx->screenRadius = ctx->savedScreenRadius;
        ctx->roomHoldingScreen = 0;
        // A held corner would carry on as a free resize of the restored placement
        ctx->grabMode = GRAB_NONE;
        if (ctx->recentredInRoom) {
            ctx->recentredInRoom = 0;
            ctx->poseDirty = 1;
        }
    }
    if (!roomOn) {
        ctx->roomPlacedStyle = 0;
        return;
    }

    // Same scale as the geometry
    RoomParams p = roomParams(style, roomScale(ctx, style));
    // Fit the picture inside the anchor with its aspect kept, then scale by the room's
    // size setting
    float width = p.screenWidth;
    if (aspect > 0.0f && p.screenHeight > 0.0f && width * aspect > p.screenHeight) {
        width = p.screenHeight / aspect;
    }
    width *= roomScreenPercent(ctx, style) / 100.0f;
    // Clamp to the wall, except in open rooms
    if (!p.open) {
        float maxWidth = 2.0f * p.halfWidth - 0.4f;
        float maxHeight = (p.ceilingY - p.floorY) - 0.3f;
        if (width > maxWidth) {
            width = maxWidth;
        }
        if (width * aspect > maxHeight) {
            width = maxHeight / aspect;
        }
    }
    float height = width * aspect;
    // Keep the picture between floor and ceiling. Uses the floor under the picture, not
    // the viewer's tier. Open rooms are left alone.
    float mount = p.screenMountY;
    if (!p.open) {
        float lowest = p.screenFloorY + height * 0.5f + 0.1f;
        float highest = p.ceilingY - height * 0.5f - 0.1f;
        if (mount < lowest) {
            mount = lowest;
        }
        if (mount > highest) {
            mount = highest;
        }
    }

    // Identity orientation: square to the wall, facing the viewer
    memset(&ctx->screenPose, 0, sizeof(ctx->screenPose));
    ctx->screenPose.orientation.w = 1.0f;
    ctx->screenPose.position.x = p.screenAt.x;
    ctx->screenPose.position.y = mount;
    ctx->screenPose.position.z = p.screenZ + p.screenProud;
    ctx->screenWidth = width;
    // Log once per room/aspect, and once after a resize ends
    if (style != ctx->roomPlacedStyle
            || (fabsf(width - ctx->roomPlacedWidth) > 0.001f && !roomSizeDragging(ctx))) {
        ctx->roomPlacedStyle = style;
        ctx->roomPlacedWidth = width;
        LOGEV("room %d hangs the picture %.2f by %.2f m, centre y %.2f z %.2f",
              style, width, height, mount, ctx->screenPose.position.z);
    }
}

// Whether the model and every atlas it uses have arrived for this style. Only one
// room is resident.
static int roomAssetsReady(XrCtx* ctx, int style) {
    if (!ctx->roomModelReady || ctx->roomModelStyle != style) {
        return 0;
    }
    for (int i = 0; i < ctx->roomAtlasCount; i++) {
        if (!ctx->roomTextureReady[i] || ctx->roomTextureStyle[i] != style) {
            return 0;
        }
    }
    return 1;
}

// Writes the model's vertices into the room buffer layout, applying anchor and scale
// (normals unchanged under uniform scale) and baking the spill. Textured parts get
// their colour from the atlas at draw time. Returns the farthest vertex distance.
static float buildModelRoomVertices(XrCtx* ctx, const RoomParams* p, float scale,
                                    float* verts) {
    float furthest = 0.0f;
    for (int i = 0; i < ctx->roomModelVertexCount; i++) {
        const float* src = ctx->roomModelVerts + (size_t)i * ROOM_MODEL_FLOATS;
        Vec3 pos = { (src[0] - p->anchor.x) * scale,
                     (src[1] - p->anchor.y) * scale,
                     (src[2] - p->anchor.z) * scale };
        float away = pos.x * pos.x + pos.y * pos.y + pos.z * pos.z;
        if (away > furthest) {
            furthest = away;
        }
        Vec3 normal = { src[3], src[4], src[5] };
        roomWriteVertex(p, verts, i, pos, src + 8, roomSpillWeight(p, pos, normal),
                        src[6], src[7]);
    }
    return sqrtf(furthest);
}

// Builds a style's geometry into the GL buffers. Rebuilt on room change rather than
// keeping every room resident.
static int uploadRoomGeometry(XrCtx* ctx, int style) {
    // No room (or assets pending): empty buffers, black clear
    if (style <= 0) {
        ctx->roomVertexCount = 0;
        ctx->roomIndexCount = 0;
        ctx->roomPartCount = 0;
        ctx->roomFarZ = ROOM_FAR_MIN_M;
        ctx->roomClear[0] = 0.0f;
        ctx->roomClear[1] = 0.0f;
        ctx->roomClear[2] = 0.0f;
        return 1;
    }
    if (!bakedRoomStyle(style) || !roomAssetsReady(ctx, style)) {
        return 0;
    }
    float scale = roomScale(ctx, style);
    RoomParams params = roomParams(style, scale);
    int vertexCount = ctx->roomModelVertexCount;
    int indexCount = ctx->roomModelIndexCount;
    float* verts = malloc((size_t)vertexCount * ROOM_VERTEX_FLOATS * sizeof(float));
    if (verts == NULL) {
        LOGE("room geometry allocation failed");
        return 0;
    }
    float reach = buildModelRoomVertices(ctx, &params, scale, verts);
    if (ctx->roomVertexBuffer == 0) {
        glGenBuffers(1, &ctx->roomVertexBuffer);
    }
    glBindBuffer(GL_ARRAY_BUFFER, ctx->roomVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)vertexCount * ROOM_VERTEX_FLOATS * sizeof(float),
                 verts, GL_STATIC_DRAW);
    if (ctx->roomIndexBuffer == 0) {
        glGenBuffers(1, &ctx->roomIndexBuffer);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->roomIndexBuffer);
    // The model's own indices; parts draw them as-is
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)indexCount * sizeof(uint32_t),
                 ctx->roomModelIndices, GL_STATIC_DRAW);
    // Other passes use client arrays, so unbind
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    free(verts);

    ctx->roomVertexCount = vertexCount;
    ctx->roomIndexCount = indexCount;
    memcpy(ctx->roomParts, ctx->roomModelParts,
           (size_t)ctx->roomModelPartCount * sizeof(RoomMeshPart));
    ctx->roomPartCount = ctx->roomModelPartCount;
    ctx->roomFarZ = roomFarPlane(&params, reach);
    ctx->roomSpillGain = params.spillGain;
    ctx->roomTexMix = params.texMix;
    ctx->roomDim = params.dim;
    // Clear colour: only visible before the first draw, since the shell is closed
    ctx->roomClear[0] = 0.010f;
    ctx->roomClear[1] = 0.010f;
    ctx->roomClear[2] = 0.012f;
    LOGEV("room ready, style %d, scale %.2f, %d vertices, %d indices, %d parts, far %.0f m, "
          "open %d, resizable %d, screen %d percent of its anchor, glow %s, brightness %d, "
          "light level %d", style, scale, vertexCount, indexCount, ctx->roomPartCount,
          (double)ctx->roomFarZ, params.open, roomResizable(style),
          roomScreenPercent(ctx, style), ctx->roomGlow[style] ? "on" : "off",
          ctx->roomBrightness[style], ctx->roomLightLevel[style]);
    return 1;
}

// The style that can be built now, or 0 (black void) until its model and atlas arrive.
static int buildableRoomStyle(XrCtx* ctx, int style) {
    return roomAssetsReady(ctx, style) ? style : 0;
}

// Room eye size: the runtime's recommendation capped by the tier (Low halves it), or a
// fixed size for Ultra capped by the runtime's maximum. Also used by the controller
// models.
void worldEyeSize(XrCtx* ctx, int* outW, int* outH) {
    int tier = ctx->envResTier;
    int eyeW;
    int eyeH;
    if (tier == ENV_RES_ULTRA) {
        eyeW = ROOM_ULTRA_EYE;
        eyeH = ROOM_ULTRA_EYE;
        if (ctx->maxEyeWidth > 0 && eyeW > ctx->maxEyeWidth) {
            eyeW = ctx->maxEyeWidth;
        }
        if (ctx->maxEyeHeight > 0 && eyeH > ctx->maxEyeHeight) {
            eyeH = ctx->maxEyeHeight;
        }
    }
    else {
        int maxEye = tier == ENV_RES_LOW ? ROOM_MAX_EYE
                   : tier == ENV_RES_HIGH ? ROOM_MAX_EYE_FULL
                   : ROOM_MAX_EYE_STANDARD;
        eyeW = ctx->recommendedEyeWidth > 0 ? ctx->recommendedEyeWidth : 1024;
        eyeH = ctx->recommendedEyeHeight > 0 ? ctx->recommendedEyeHeight : 1024;
        if (tier == ENV_RES_LOW) {
            eyeW /= 2;
            eyeH /= 2;
        }
        if (eyeW > maxEye) {
            eyeW = maxEye;
        }
        if (eyeH > maxEye) {
            eyeH = maxEye;
        }
    }
    *outW = eyeW;
    *outH = eyeH;
}

// Creates the room's swapchain, depth buffer and program on first use (mid session).
static int initRoom(XrCtx* ctx) {
    if (ctx->roomReady) {
        return 1;
    }
    if (ctx->roomFailed || ctx->session == XR_NULL_HANDLE) {
        return 0;
    }
    ctx->roomFailed = 1;

    // Colour and depth come to a few hundred MB, so nothing exists until a room is picked
    int tier = ctx->envResTier;
    int eyeW;
    int eyeH;
    worldEyeSize(ctx, &eyeW, &eyeH);

    // Side by side, like the stereo video swapchain
    if (!createArtSwapchain(ctx, eyeW * ROOM_EYES, eyeH, "create room swapchain",
                            &ctx->roomSwapchain, &ctx->roomImages, &ctx->roomImageCount)) {
        return 0;
    }
    LOGEV("swapchains alive %d with the room's", ctx->swapchainsAlive);

    // Only pass with a depth buffer
    glGenRenderbuffers(1, &ctx->roomDepthBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, ctx->roomDepthBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eyeW * ROOM_EYES, eyeH);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    // Colour attaches per frame (swapchain image); only depth is fixed
    glGenFramebuffers(1, &ctx->roomFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->roomFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              ctx->roomDepthBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GLuint vs = compileShader(GL_VERTEX_SHADER, ROOM_VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, ROOM_FRAGMENT_SRC);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    ctx->roomProgram = glCreateProgram();
    glAttachShader(ctx->roomProgram, vs);
    glAttachShader(ctx->roomProgram, fs);
    glBindAttribLocation(ctx->roomProgram, 0, "a_position");
    glBindAttribLocation(ctx->roomProgram, 1, "a_color");
    glBindAttribLocation(ctx->roomProgram, 2, "a_spill");
    glBindAttribLocation(ctx->roomProgram, 3, "a_uv");
    glLinkProgram(ctx->roomProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(ctx->roomProgram, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(ctx->roomProgram, sizeof(log), NULL, log);
        LOGE("room program link failed: %s", log);
        return 0;
    }
    ctx->roomViewProjUniform = glGetUniformLocation(ctx->roomProgram, "u_viewproj");
    ctx->roomSpillGainUniform = glGetUniformLocation(ctx->roomProgram, "u_spillGain");
    ctx->roomTexMixUniform = glGetUniformLocation(ctx->roomProgram, "u_texMix");
    ctx->roomDimUniform = glGetUniformLocation(ctx->roomProgram, "u_dim");
    glUseProgram(ctx->roomProgram);
    glUniform1i(glGetUniformLocation(ctx->roomProgram, "u_ambi"), 0);
    glUniform1i(glGetUniformLocation(ctx->roomProgram, "u_room"), 1);

    // The atlas sampler is always read, so bind a white texture until an atlas loads
    glGenTextures(1, &ctx->roomWhiteTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->roomWhiteTexture);
    const unsigned char white[4] = { 255, 255, 255, 255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Build the requested room now so the first frame shows it
    int wanted = roomEffective(ctx);
    int style = buildableRoomStyle(ctx, wanted);
    if (!uploadRoomGeometry(ctx, style)) {
        return 0;
    }
    ctx->roomBuiltStyle = style;
    ctx->roomWantedStyle = wanted;
    ctx->roomAssetsSeen = roomAssetsReady(ctx, wanted);
    ctx->roomWantedScale = roomScale(ctx, style);

    ctx->roomEyeWidth = eyeW;
    ctx->roomEyeHeight = eyeH;
    ctx->roomReady = 1;
    ctx->roomFailed = 0;
    const char* tierName = tier == ENV_RES_LOW ? "low"
                         : tier == ENV_RES_HIGH ? "high"
                         : tier == ENV_RES_ULTRA ? "ultra"
                         : "standard";
    LOGEV("room ready at %dx%d per eye, env res %s", eyeW, eyeH, tierName);
    return 1;
}

// Locates both eyes for this frame. Room only.
static int locateRoomViews(XrCtx* ctx) {
    XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = ctx->predictedDisplayTime;
    locateInfo.space = ctx->localSpace;

    XrViewState state = { XR_TYPE_VIEW_STATE };
    XrView views[ROOM_EYES];
    for (int eye = 0; eye < ROOM_EYES; eye++) {
        views[eye].type = XR_TYPE_VIEW;
        views[eye].next = NULL;
    }
    uint32_t count = 0;
    if (XR_FAILED(xrLocateViews(ctx->session, &locateInfo, &state, ROOM_EYES, &count, views))
            || count < ROOM_EYES) {
        return 0;
    }
    // Need position too, or the room would follow the head
    XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT
            | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((state.viewStateFlags & needed) != needed) {
        return 0;
    }

    for (int eye = 0; eye < ROOM_EYES; eye++) {
        ctx->roomViews[eye] = views[eye];
    }
    ctx->roomViewsValid = 1;
    return 1;
}

// Builds or rebuilds whatever the room needs. Must run before the frame's timer query
// opens: creating a swapchain or buffer inside it corrupts later samples on this
// driver.
void prepareRoom(XrCtx* ctx) {
    if (!initRoom(ctx)) {
        return;
    }
    // Rebuild only when the wanted style, its asset readiness or the scale changes. A
    // failed build keeps the current room and is not retried.
    int wanted = roomEffective(ctx);
    // Readiness for the wanted room only, so another room's assets don't count
    int assets = roomAssetsReady(ctx, wanted);
    int style = buildableRoomStyle(ctx, wanted);
    float scale = roomScale(ctx, style);
    if (wanted == ctx->roomWantedStyle && assets == ctx->roomAssetsSeen
            && scale == ctx->roomWantedScale) {
        return;
    }
    // Decided before recording, since the scale is part of both
    int rebuild = style != ctx->roomBuiltStyle || scale != ctx->roomWantedScale;
    ctx->roomWantedStyle = wanted;
    ctx->roomAssetsSeen = assets;
    ctx->roomWantedScale = scale;

    if (rebuild && uploadRoomGeometry(ctx, style)) {
        ctx->roomBuiltStyle = style;
    }
}

// Draws the room once per eye into its half. Framebuffer already bound and cleared.
static void drawRoomEyes(XrCtx* ctx) {
    glUseProgram(ctx->roomProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);
    // Picture spill only once a colour sample exists and the room light is on; scaled by
    // the room's light level. Independent of the ambilight glow.
    int lit = ctx->ambiSeeded && ctx->roomLightOn;
    int built = ctx->roomBuiltStyle;
    float level = bakedRoomStyle(built) ? ctx->roomLightLevel[built] / 100.0f : 1.0f;
    glUniform1f(ctx->roomSpillGainUniform, lit ? ctx->roomSpillGain * level : 0.0f);
    glUniform1f(ctx->roomDimUniform, roomDim(ctx));

    glBindBuffer(GL_ARRAY_BUFFER, ctx->roomVertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->roomIndexBuffer);
    GLsizei stride = ROOM_VERTEX_FLOATS * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (const void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(7 * sizeof(float)));
    glEnableVertexAttribArray(3);

    // Parts bind their atlas on unit 1
    glActiveTexture(GL_TEXTURE1);
    for (int eye = 0; eye < ROOM_EYES; eye++) {
        glViewport(eye * ctx->roomEyeWidth, 0, ctx->roomEyeWidth, ctx->roomEyeHeight);

        float proj[16];
        float view[16];
        float viewProj[16];
        // Near plane allows walking into walls without clipping
        projectionFromFov(proj, ctx->roomViews[eye].fov, ROOM_NEAR_M, ctx->roomFarZ);
        viewFromPose(view, ctx->roomViews[eye].pose);
        matMul(viewProj, proj, view);
        glUniformMatrix4fv(ctx->roomViewProjUniform, 1, GL_FALSE, viewProj);

        // Per part: its atlas, or vertex colours with the mix inverted. Only atlases of the
        // built room are used; white stands in for a missing one.
        for (int i = 0; i < ctx->roomPartCount; i++) {
            const RoomMeshPart* part = &ctx->roomParts[i];
            int textured = part->atlas >= 0;
            GLuint texture = ctx->roomWhiteTexture;
            if (textured && ctx->roomTextureReady[part->atlas]
                    && ctx->roomTextureStyle[part->atlas] == ctx->roomBuiltStyle) {
                texture = ctx->roomTextures[part->atlas];
            }
            glBindTexture(GL_TEXTURE_2D, texture);
            glUniform1f(ctx->roomTexMixUniform, textured ? ctx->roomTexMix : 0.0f);
            glDrawElements(GL_TRIANGLES, (GLsizei)part->indexCount, GL_UNSIGNED_INT,
                           (const void*)((size_t)part->firstIndex * sizeof(uint32_t)));
        }
    }
    // Leave unit 0 active for the other passes
    glActiveTexture(GL_TEXTURE0);
}

// Renders the room into its swapchain; the layer is submitted in endFrame with the same
// poses. Nothing is created here (see prepareRoom).
void renderRoom(XrCtx* ctx) {
    if (!ctx->roomReady) {
        return;
    }
    // No views: keep the previous image and poses
    if (!locateRoomViews(ctx)) {
        return;
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->roomSwapchain, &acquire, &index),
                 "acquire room image")) {
        return;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->roomSwapchain, &wait);

    // Open the query only after the swapchain wait; a query spanning it returns garbage on
    // this driver
    int roomTiming = ctx->timerSupported && !ctx->captureRequested
            && !ctx->roomTimerPending[ctx->roomTimerSlot];
    if (roomTiming) {
        pfnBeginQuery(GL_TIME_ELAPSED_EXT, ctx->roomTimerQueries[ctx->roomTimerSlot]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->roomFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->roomImages[index].image, 0);
    // Check completeness once, on the first frame (the colour attachment is a swapchain
    // image)
    if (!ctx->roomRendered) {
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("room framebuffer incomplete: 0x%x", status);
        }
    }
    // Colours are authored gamma encoded, like the video
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glClearColor(ctx->roomClear[0], ctx->roomClear[1], ctx->roomClear[2], 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    // Only the clear until the model and atlas arrive
    if (ctx->roomIndexCount > 0) {
        drawRoomEyes(ctx);
    }

    if (roomTiming) {
        pfnEndQuery(GL_TIME_ELAPSED_EXT);
        ctx->roomTimerPending[ctx->roomTimerSlot] = 1;
        ctx->roomTimerPendingFrames[ctx->roomTimerSlot] = 0;
        ctx->roomTimerSlot = 1 - ctx->roomTimerSlot;
    }

    glDisable(GL_DEPTH_TEST);
    // Restore state for the other passes: no buffers bound, attribs 2 and 3 off
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->roomSwapchain, &release);
    ctx->roomRendered = 1;
}

// Frees one atlas slot. Each upload creates a fresh texture.
static void releaseRoomTexture(XrCtx* ctx, int slot) {
    if (ctx->roomTextures[slot] != 0) {
        glDeleteTextures(1, &ctx->roomTextures[slot]);
        ctx->roomTextures[slot] = 0;
    }
    ctx->roomTextureReady[slot] = 0;
    ctx->roomTextureStyle[slot] = 0;
}

// Frees all atlases of the resident room
static void releaseRoomTextures(XrCtx* ctx) {
    for (int slot = 0; slot < ROOM_MESH_ATLASES_MAX; slot++) {
        releaseRoomTexture(ctx, slot);
    }
}

// Baked room style for a picker cell, or 0 (logged)
static int bakedStyleForCell(int cell, const char* what) {
    int style = roomStyleForCell(cell);
    if (!bakedRoomStyle(style)) {
        LOGW("room %s for cell %d, which is not a baked room, ignoring it", what, cell);
        return 0;
    }
    return style;
}

// A baked room's model (pre-flattened by the bake script: positions, normals, uvs,
// colours, parts), read in Java and parsed here. Frame loop.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRoomModel(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject buffer,
                                                                   jint length, jint cell) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL || length <= 0) {
        return;
    }
    int style = bakedStyleForCell(cell, "model");
    if (style == 0) {
        return;
    }
    const unsigned char* data = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    if (data == NULL || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)length) {
        return;
    }
    RoomMeshInfo info;
    if (!roomMeshParse(data, (size_t)length, &info)) {
        LOGW("room model for style %d is not a mesh the renderer reads (%d bytes)", style, length);
        return;
    }

    size_t vertexBytes = (size_t)info.vertexCount * ROOM_MODEL_FLOATS * sizeof(float);
    size_t indexBytes = (size_t)info.indexCount * sizeof(uint32_t);
    float* verts = malloc(vertexBytes);
    uint32_t* indices = malloc(indexBytes);
    if (verts == NULL || indices == NULL) {
        free(verts);
        free(indices);
        LOGE("room model allocation failed");
        return;
    }
    memcpy(verts, data + info.vertexOffset, vertexBytes);
    memcpy(indices, data + info.indexOffset, indexBytes);

    // New room: free the old one's atlases first
    if (ctx->roomModelStyle != style) {
        releaseRoomTextures(ctx);
    }
    // Kept in model space so the scale can change without re-reading
    free(ctx->roomModelVerts);
    free(ctx->roomModelIndices);
    ctx->roomModelVerts = verts;
    ctx->roomModelIndices = indices;
    ctx->roomModelVertexCount = (int)info.vertexCount;
    ctx->roomModelIndexCount = (int)info.indexCount;
    memcpy(ctx->roomModelParts, info.parts, (size_t)info.partCount * sizeof(RoomMeshPart));
    ctx->roomModelPartCount = (int)info.partCount;
    ctx->roomAtlasCount = (int)info.atlasCount;
    ctx->roomModelStyle = style;
    ctx->roomModelReady = 1;
    LOGEV("room model ready, style %d, %u vertices, %u triangles, %u parts, %d painted from "
          "vertex colours, %u atlases", style, info.vertexCount, info.indexCount / 3,
          info.partCount, roomMeshPaintedParts(&info), info.atlasCount);
}

// One room's stored Room tab levels, by picker cell. Sent for every room at session
// start; values are clamped.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetRoomLevels(JNIEnv* env, jobject thiz,
                                                                jlong handle, jint cell,
                                                                jint brightness, jboolean glow,
                                                                jint light, jint screen) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    int style = bakedStyleForCell(cell, "levels");
    if (style == 0) {
        return;
    }
    ctx->roomBrightness[style] = brightness < ROOM_BRIGHTNESS_MIN ? ROOM_BRIGHTNESS_MIN
            : (brightness > ROOM_BRIGHTNESS_MAX ? ROOM_BRIGHTNESS_MAX : brightness);
    ctx->roomGlow[style] = glow ? 1 : 0;
    ctx->roomLightLevel[style] = light < ROOM_LIGHT_MIN ? ROOM_LIGHT_MIN
            : (light > ROOM_LIGHT_MAX ? ROOM_LIGHT_MAX : light);
    ctx->roomScreen[style] = roomScreenClamp(screen, roomResizable(style));
    LOGEV("room %d levels: brightness %d, glow %s, light level %d, screen %d percent",
          style, ctx->roomBrightness[style], ctx->roomGlow[style] ? "on" : "off",
          ctx->roomLightLevel[style], ctx->roomScreen[style]);
}

// GL format for the atlas tool's ASTC block size
static GLenum roomAtlasFormat(uint32_t block) {
    if (block == 4) {
        return GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
    }
    if (block == 8) {
        return GL_COMPRESSED_RGBA_ASTC_8x8_KHR;
    }
    return GL_COMPRESSED_RGBA_ASTC_6x6_KHR;
}

// One room atlas (.atlas file from tools/atlas_astc.py) into its slot. All mip levels
// are already ASTC, uploaded as-is. Plain texture, not a swapchain. Frame loop.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRoomAtlas(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject buffer,
                                                                   jint cell, jint slot) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL) {
        return;
    }
    if (slot < 0 || slot >= ROOM_MESH_ATLASES_MAX) {
        LOGW("room atlas for cell %d is in slot %d, which there is not", cell, slot);
        return;
    }
    int style = bakedStyleForCell(cell, "atlas");
    if (style == 0) {
        return;
    }
    // Clear the slot first, so a refused atlas leaves the void rather than a wrong atlas
    releaseRoomTexture(ctx, slot);
    if (!ctx->astcSupported) {
        LOGW("room atlas for style %d is ASTC and this GPU has no "
             "GL_KHR_texture_compression_astc_ldr", style);
        return;
    }
    const unsigned char* data = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    jlong size = (*env)->GetDirectBufferCapacity(env, buffer);
    AtlasInfo info;
    if (data == NULL || size <= 0 || !atlasParse(data, (size_t)size, &info)) {
        LOGW("room atlas for style %d is not an atlas the renderer reads (%lld bytes)",
             style, (long long)size);
        return;
    }

    int64_t started = nowNs();
    GLenum format = roomAtlasFormat(info.blockWidth);
    glGenTextures(1, &ctx->roomTextures[slot]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->roomTextures[slot]);
    // Clear stale GL errors so the check below is this upload's
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++) {
    }
    // Rows are top down, matching the model's uvs, so no flip
    for (int i = 0; i < info.levels; i++) {
        glCompressedTexImage2D(GL_TEXTURE_2D, i, format,
                               (GLsizei)atlasLevelSize(info.width, i),
                               (GLsizei)atlasLevelSize(info.height, i), 0,
                               (GLsizei)info.lengths[i], data + info.offsets[i]);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, info.levels - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (ctx->roomAnisotropy > 1.0f) {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, ctx->roomAnisotropy);
    }
    GLenum error = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (error != GL_NO_ERROR) {
        releaseRoomTexture(ctx, slot);
        LOGW("room atlas %ux%u for style %d, slot %d refused, GL error 0x%x",
             info.width, info.height, style, slot, error);
        return;
    }

    ctx->roomTextureStyle[slot] = style;
    ctx->roomTextureReady[slot] = 1;
    LOGEV("room atlas %ux%u ASTC %ux%u ready, style %d, slot %d, %d levels, %.1f MB, "
          "anisotropy %.0f, upload calls %.1f ms",
          info.width, info.height, info.blockWidth, info.blockHeight, style, slot, info.levels,
          (double)size / (1024.0 * 1024.0), ctx->roomAnisotropy, (nowNs() - started) / 1e6);
}
