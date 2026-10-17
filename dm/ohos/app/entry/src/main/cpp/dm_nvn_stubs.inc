// ⚠ 生成文件，请勿手改。生成器：dm/ohos/tools/gen_nvn_stubs.py
// 输入：dm/gfx/nvn/names.txt（523 个名字 = NVN 完整 API 面）
//
// 内容：每名字一个「记账桩」+ 族枚举 + 名字表 + 桩地址表。
// 用途：让 GetProcAddress 按名字发地址，从日志里看出游戏**先真正调用哪个族/哪个函数**
//       （NVN 共 523 个 API，只能按真实调用顺序排期实现）。
// 行为：桩一律 return 0 —— 与「全返同一个通用桩」完全等价，纯观测、不改语义。

// ---- 1. 族枚举（顺序 = 生成器 FAMILIES，改顺序要重新生成）----
enum {
    NVN_FAM_COMMAND,
    NVN_FAM_TEXTURE,
    NVN_FAM_SAMPLER,
    NVN_FAM_DEVICE,
    NVN_FAM_QUEUE,
    NVN_FAM_WINDOW,
    NVN_FAM_MULTISAMPLE,
    NVN_FAM_MEMPOOL,
    NVN_FAM_BUFFER,
    NVN_FAM_BLEND,
    NVN_FAM_DEPTH,
    NVN_FAM_EVENT,
    NVN_FAM_VERTEX,
    NVN_FAM_POLYGON,
    NVN_FAM_PROGRAM,
    NVN_FAM_COLOR,
    NVN_FAM_SYNC,
    NVN_FAM_CHANNEL,
    NVN_FAM__N
};
static const char *const g_nvn_fam_name[NVN_FAM__N] = {
    "CommandBuffer",
    "Texture",
    "Sampler",
    "Device",
    "Queue",
    "Window",
    "Multisample",
    "MemoryPool",
    "Buffer",
    "Blend",
    "Depth",
    "Event",
    "Vertex",
    "Polygon",
    "Program",
    "Color",
    "Sync",
    "Channel",
};

// ---- 2. 类型 ----
typedef struct { const char *name; int fam; uint64_t calls; } nvn_api_t;
typedef uint64_t (*nvn_thunk_fn)(void);

// ---- 3. 名字表（顺序 = 输入文件顺序，保持稳定便于 diff）----
static nvn_api_t g_nvn_api[] = {
    { "nvnDeviceBuilderSetDefaults", NVN_FAM_DEVICE },
    { "nvnDeviceBuilderSetFlags", NVN_FAM_DEVICE },
    { "nvnDeviceBuilderGetFlags", NVN_FAM_DEVICE },
    { "nvnDeviceInitialize", NVN_FAM_DEVICE },
    { "nvnDeviceFinalize", NVN_FAM_DEVICE },
    { "nvnDeviceSetDebugLabel", NVN_FAM_DEVICE },
    { "nvnDeviceGetProcAddress", NVN_FAM_DEVICE },
    { "nvnDeviceGetInteger", NVN_FAM_DEVICE },
    { "nvnDeviceGetCurrentTimestampInNanoseconds", NVN_FAM_DEVICE },
    { "nvnDeviceSetIntermediateShaderCache", NVN_FAM_DEVICE },
    { "nvnDeviceGetTextureHandle", NVN_FAM_DEVICE },
    { "nvnDeviceGetTexelFetchHandle", NVN_FAM_DEVICE },
    { "nvnDeviceGetImageHandle", NVN_FAM_DEVICE },
    { "nvnDeviceInstallDebugCallback", NVN_FAM_DEVICE },
    { "nvnDeviceGenerateDebugDomainId", NVN_FAM_DEVICE },
    { "nvnDeviceSetWindowOriginMode", NVN_FAM_DEVICE },
    { "nvnDeviceSetDepthMode", NVN_FAM_DEVICE },
    { "nvnDeviceRegisterFastClearColor", NVN_FAM_DEVICE },
    { "nvnDeviceRegisterFastClearColori", NVN_FAM_DEVICE },
    { "nvnDeviceRegisterFastClearColorui", NVN_FAM_DEVICE },
    { "nvnDeviceRegisterFastClearDepth", NVN_FAM_DEVICE },
    { "nvnDeviceGetWindowOriginMode", NVN_FAM_DEVICE },
    { "nvnDeviceGetDepthMode", NVN_FAM_DEVICE },
    { "nvnDeviceGetTimestampInNanoseconds", NVN_FAM_DEVICE },
    { "nvnDeviceApplyDeferredFinalizes", NVN_FAM_DEVICE },
    { "nvnDeviceFinalizeCommandHandle", NVN_FAM_DEVICE },
    { "nvnDeviceWalkDebugDatabase", NVN_FAM_DEVICE },
    { "nvnDeviceGetSeparateTextureHandle", NVN_FAM_DEVICE },
    { "nvnDeviceGetSeparateSamplerHandle", NVN_FAM_DEVICE },
    { "nvnDeviceIsExternalDebuggerAttached", NVN_FAM_DEVICE },
    { "nvnQueueGetError", NVN_FAM_QUEUE },
    { "nvnQueueGetTotalCommandMemoryUsed", NVN_FAM_QUEUE },
    { "nvnQueueGetTotalControlMemoryUsed", NVN_FAM_QUEUE },
    { "nvnQueueGetTotalComputeMemoryUsed", NVN_FAM_QUEUE },
    { "nvnQueueResetMemoryUsageCounts", NVN_FAM_QUEUE },
    { "nvnQueueBuilderSetDevice", NVN_FAM_QUEUE },
    { "nvnQueueBuilderSetDefaults", NVN_FAM_QUEUE },
    { "nvnQueueBuilderSetFlags", NVN_FAM_QUEUE },
    { "nvnQueueBuilderSetCommandMemorySize", NVN_FAM_QUEUE },
    { "nvnQueueBuilderSetComputeMemorySize", NVN_FAM_QUEUE },
    { "nvnQueueBuilderSetControlMemorySize", NVN_FAM_QUEUE },
    { "nvnQueueBuilderGetQueueMemorySize", NVN_FAM_QUEUE },
    { "nvnQueueBuilderSetQueueMemory", NVN_FAM_QUEUE },
    { "nvnQueueBuilderSetCommandFlushThreshold", NVN_FAM_QUEUE },
    { "nvnQueueBuilderSetQueuePriority", NVN_FAM_QUEUE },
    { "nvnQueueBuilderGetQueuePriority", NVN_FAM_QUEUE },
    { "nvnQueueBuilderGetDevice", NVN_FAM_QUEUE },
    { "nvnQueueBuilderGetFlags", NVN_FAM_QUEUE },
    { "nvnQueueBuilderGetCommandMemorySize", NVN_FAM_QUEUE },
    { "nvnQueueBuilderGetComputeMemorySize", NVN_FAM_QUEUE },
    { "nvnQueueBuilderGetControlMemorySize", NVN_FAM_QUEUE },
    { "nvnQueueBuilderGetCommandFlushThreshold", NVN_FAM_QUEUE },
    { "nvnQueueBuilderGetMemorySize", NVN_FAM_QUEUE },
    { "nvnQueueBuilderGetMemory", NVN_FAM_QUEUE },
    { "nvnQueueInitialize", NVN_FAM_QUEUE },
    { "nvnQueueFinalize", NVN_FAM_QUEUE },
    { "nvnQueueSetDebugLabel", NVN_FAM_QUEUE },
    { "nvnQueueSubmitCommands", NVN_FAM_QUEUE },
    { "nvnQueueFlush", NVN_FAM_QUEUE },
    { "nvnQueueFinish", NVN_FAM_QUEUE },
    { "nvnQueuePresentTexture", NVN_FAM_QUEUE },
    { "nvnQueueAcquireTexture", NVN_FAM_QUEUE },
    { "nvnWindowBuilderSetDevice", NVN_FAM_WINDOW },
    { "nvnWindowBuilderSetDefaults", NVN_FAM_WINDOW },
    { "nvnWindowBuilderSetNativeWindow", NVN_FAM_WINDOW },
    { "nvnWindowBuilderSetTextures", NVN_FAM_WINDOW },
    { "nvnWindowBuilderSetPresentInterval", NVN_FAM_WINDOW },
    { "nvnWindowBuilderSetNumActiveTextures", NVN_FAM_WINDOW },
    { "nvnWindowBuilderGetDevice", NVN_FAM_WINDOW },
    { "nvnWindowBuilderGetNumTextures", NVN_FAM_WINDOW },
    { "nvnWindowBuilderGetTexture", NVN_FAM_WINDOW },
    { "nvnWindowBuilderGetNativeWindow", NVN_FAM_WINDOW },
    { "nvnWindowBuilderGetPresentInterval", NVN_FAM_WINDOW },
    { "nvnWindowBuilderGetNumActiveTextures", NVN_FAM_WINDOW },
    { "nvnWindowInitialize", NVN_FAM_WINDOW },
    { "nvnWindowFinalize", NVN_FAM_WINDOW },
    { "nvnWindowSetDebugLabel", NVN_FAM_WINDOW },
    { "nvnWindowAcquireTexture", NVN_FAM_WINDOW },
    { "nvnWindowGetNativeWindow", NVN_FAM_WINDOW },
    { "nvnWindowGetPresentInterval", NVN_FAM_WINDOW },
    { "nvnWindowSetPresentInterval", NVN_FAM_WINDOW },
    { "nvnWindowSetCrop", NVN_FAM_WINDOW },
    { "nvnWindowGetCrop", NVN_FAM_WINDOW },
    { "nvnWindowSetNumActiveTextures", NVN_FAM_WINDOW },
    { "nvnWindowGetNumActiveTextures", NVN_FAM_WINDOW },
    { "nvnWindowGetNumTextures", NVN_FAM_WINDOW },
    { "nvnProgramInitialize", NVN_FAM_PROGRAM },
    { "nvnProgramFinalize", NVN_FAM_PROGRAM },
    { "nvnProgramSetDebugLabel", NVN_FAM_PROGRAM },
    { "nvnProgramSetShaders", NVN_FAM_PROGRAM },
    { "nvnProgramSetShadersExt", NVN_FAM_PROGRAM },
    { "nvnProgramSetSampleShading", NVN_FAM_PROGRAM },
    { "nvnMemoryPoolBuilderSetDevice", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolBuilderSetDefaults", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolBuilderSetStorage", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolBuilderSetFlags", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolBuilderGetDevice", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolBuilderGetMemory", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolBuilderGetSize", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolBuilderGetFlags", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolInitialize", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolSetDebugLabel", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolFinalize", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolMap", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolFlushMappedRange", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolInvalidateMappedRange", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolGetBufferAddress", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolMapVirtual", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolGetSize", NVN_FAM_MEMPOOL },
    { "nvnMemoryPoolGetFlags", NVN_FAM_MEMPOOL },
    { "nvnTexturePoolInitialize", NVN_FAM_TEXTURE },
    { "nvnTexturePoolSetDebugLabel", NVN_FAM_TEXTURE },
    { "nvnTexturePoolFinalize", NVN_FAM_TEXTURE },
    { "nvnTexturePoolRegisterTexture", NVN_FAM_TEXTURE },
    { "nvnTexturePoolRegisterImage", NVN_FAM_TEXTURE },
    { "nvnTexturePoolGetMemoryPool", NVN_FAM_TEXTURE },
    { "nvnTexturePoolGetMemoryOffset", NVN_FAM_TEXTURE },
    { "nvnTexturePoolGetSize", NVN_FAM_TEXTURE },
    { "nvnSamplerPoolInitialize", NVN_FAM_SAMPLER },
    { "nvnSamplerPoolSetDebugLabel", NVN_FAM_SAMPLER },
    { "nvnSamplerPoolFinalize", NVN_FAM_SAMPLER },
    { "nvnSamplerPoolRegisterSampler", NVN_FAM_SAMPLER },
    { "nvnSamplerPoolRegisterSamplerBuilder", NVN_FAM_SAMPLER },
    { "nvnSamplerPoolGetMemoryPool", NVN_FAM_SAMPLER },
    { "nvnSamplerPoolGetMemoryOffset", NVN_FAM_SAMPLER },
    { "nvnSamplerPoolGetSize", NVN_FAM_SAMPLER },
    { "nvnBufferBuilderSetDevice", NVN_FAM_BUFFER },
    { "nvnBufferBuilderSetDefaults", NVN_FAM_BUFFER },
    { "nvnBufferBuilderSetStorage", NVN_FAM_BUFFER },
    { "nvnBufferBuilderGetDevice", NVN_FAM_BUFFER },
    { "nvnBufferBuilderGetMemoryPool", NVN_FAM_BUFFER },
    { "nvnBufferBuilderGetMemoryOffset", NVN_FAM_BUFFER },
    { "nvnBufferBuilderGetSize", NVN_FAM_BUFFER },
    { "nvnBufferInitialize", NVN_FAM_BUFFER },
    { "nvnBufferSetDebugLabel", NVN_FAM_BUFFER },
    { "nvnBufferFinalize", NVN_FAM_BUFFER },
    { "nvnBufferMap", NVN_FAM_BUFFER },
    { "nvnBufferGetAddress", NVN_FAM_BUFFER },
    { "nvnBufferFlushMappedRange", NVN_FAM_BUFFER },
    { "nvnBufferInvalidateMappedRange", NVN_FAM_BUFFER },
    { "nvnBufferGetMemoryPool", NVN_FAM_BUFFER },
    { "nvnBufferGetMemoryOffset", NVN_FAM_BUFFER },
    { "nvnBufferGetSize", NVN_FAM_BUFFER },
    { "nvnBufferGetDebugID", NVN_FAM_BUFFER },
    { "nvnTextureBuilderSetDevice", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetDefaults", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetFlags", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetTarget", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetWidth", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetHeight", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetDepth", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetSize1D", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetSize2D", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetSize3D", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetLevels", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetFormat", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetSamples", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetSwizzle", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetDepthStencilMode", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetStorageSize", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetStorageAlignment", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetStorage", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetPackagedTextureData", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetPackagedTextureLayout", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetStride", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderSetGLTextureName", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetStorageClass", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetDevice", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetFlags", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetTarget", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetWidth", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetHeight", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetDepth", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetLevels", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetFormat", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetSamples", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetSwizzle", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetDepthStencilMode", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetPackagedTextureData", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetPackagedTextureLayout", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetStride", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetSparseTileLayout", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetGLTextureName", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetZCullStorageSize", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetMemoryPool", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetMemoryOffset", NVN_FAM_TEXTURE },
    { "nvnTextureBuilderGetRawStorageClass", NVN_FAM_TEXTURE },
    { "nvnTextureViewSetDefaults", NVN_FAM_TEXTURE },
    { "nvnTextureViewSetLevels", NVN_FAM_TEXTURE },
    { "nvnTextureViewSetLayers", NVN_FAM_TEXTURE },
    { "nvnTextureViewSetFormat", NVN_FAM_TEXTURE },
    { "nvnTextureViewSetSwizzle", NVN_FAM_TEXTURE },
    { "nvnTextureViewSetDepthStencilMode", NVN_FAM_TEXTURE },
    { "nvnTextureViewSetTarget", NVN_FAM_TEXTURE },
    { "nvnTextureViewGetLevels", NVN_FAM_TEXTURE },
    { "nvnTextureViewGetLayers", NVN_FAM_TEXTURE },
    { "nvnTextureViewGetFormat", NVN_FAM_TEXTURE },
    { "nvnTextureViewGetSwizzle", NVN_FAM_TEXTURE },
    { "nvnTextureViewGetDepthStencilMode", NVN_FAM_TEXTURE },
    { "nvnTextureViewGetTarget", NVN_FAM_TEXTURE },
    { "nvnTextureViewCompare", NVN_FAM_TEXTURE },
    { "nvnTextureInitialize", NVN_FAM_TEXTURE },
    { "nvnTextureGetZCullStorageSize", NVN_FAM_TEXTURE },
    { "nvnTextureFinalize", NVN_FAM_TEXTURE },
    { "nvnTextureSetDebugLabel", NVN_FAM_TEXTURE },
    { "nvnTextureGetStorageClass", NVN_FAM_TEXTURE },
    { "nvnTextureGetViewOffset", NVN_FAM_TEXTURE },
    { "nvnTextureGetFlags", NVN_FAM_TEXTURE },
    { "nvnTextureGetTarget", NVN_FAM_TEXTURE },
    { "nvnTextureGetWidth", NVN_FAM_TEXTURE },
    { "nvnTextureGetHeight", NVN_FAM_TEXTURE },
    { "nvnTextureGetDepth", NVN_FAM_TEXTURE },
    { "nvnTextureGetLevels", NVN_FAM_TEXTURE },
    { "nvnTextureGetFormat", NVN_FAM_TEXTURE },
    { "nvnTextureGetSamples", NVN_FAM_TEXTURE },
    { "nvnTextureGetSwizzle", NVN_FAM_TEXTURE },
    { "nvnTextureGetDepthStencilMode", NVN_FAM_TEXTURE },
    { "nvnTextureGetStride", NVN_FAM_TEXTURE },
    { "nvnTextureGetTextureAddress", NVN_FAM_TEXTURE },
    { "nvnTextureGetSparseTileLayout", NVN_FAM_TEXTURE },
    { "nvnTextureWriteTexels", NVN_FAM_TEXTURE },
    { "nvnTextureWriteTexelsStrided", NVN_FAM_TEXTURE },
    { "nvnTextureReadTexels", NVN_FAM_TEXTURE },
    { "nvnTextureReadTexelsStrided", NVN_FAM_TEXTURE },
    { "nvnTextureFlushTexels", NVN_FAM_TEXTURE },
    { "nvnTextureInvalidateTexels", NVN_FAM_TEXTURE },
    { "nvnTextureGetMemoryPool", NVN_FAM_TEXTURE },
    { "nvnTextureGetMemoryOffset", NVN_FAM_TEXTURE },
    { "nvnTextureGetStorageSize", NVN_FAM_TEXTURE },
    { "nvnTextureCompare", NVN_FAM_TEXTURE },
    { "nvnTextureGetDebugID", NVN_FAM_TEXTURE },
    { "nvnTextureGetRawStorageClass", NVN_FAM_TEXTURE },
    { "nvnSamplerBuilderSetDevice", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetDefaults", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetMinMagFilter", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetWrapMode", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetLodClamp", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetLodBias", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetCompare", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetBorderColor", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetBorderColori", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetBorderColorui", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetMaxAnisotropy", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetReductionFilter", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderSetLodSnap", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetDevice", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetMinMagFilter", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetWrapMode", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetLodClamp", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetLodBias", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetCompare", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetBorderColor", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetBorderColori", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetBorderColorui", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetMaxAnisotropy", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetReductionFilter", NVN_FAM_SAMPLER },
    { "nvnSamplerBuilderGetLodSnap", NVN_FAM_SAMPLER },
    { "nvnSamplerInitialize", NVN_FAM_SAMPLER },
    { "nvnSamplerFinalize", NVN_FAM_SAMPLER },
    { "nvnSamplerSetDebugLabel", NVN_FAM_SAMPLER },
    { "nvnSamplerGetMinMagFilter", NVN_FAM_SAMPLER },
    { "nvnSamplerGetWrapMode", NVN_FAM_SAMPLER },
    { "nvnSamplerGetLodClamp", NVN_FAM_SAMPLER },
    { "nvnSamplerGetLodBias", NVN_FAM_SAMPLER },
    { "nvnSamplerGetCompare", NVN_FAM_SAMPLER },
    { "nvnSamplerGetBorderColor", NVN_FAM_SAMPLER },
    { "nvnSamplerGetBorderColori", NVN_FAM_SAMPLER },
    { "nvnSamplerGetBorderColorui", NVN_FAM_SAMPLER },
    { "nvnSamplerGetMaxAnisotropy", NVN_FAM_SAMPLER },
    { "nvnSamplerGetReductionFilter", NVN_FAM_SAMPLER },
    { "nvnSamplerCompare", NVN_FAM_SAMPLER },
    { "nvnSamplerGetDebugID", NVN_FAM_SAMPLER },
    { "nvnBlendStateSetDefaults", NVN_FAM_BLEND },
    { "nvnBlendStateSetBlendTarget", NVN_FAM_BLEND },
    { "nvnBlendStateSetBlendFunc", NVN_FAM_BLEND },
    { "nvnBlendStateSetBlendEquation", NVN_FAM_BLEND },
    { "nvnBlendStateSetAdvancedMode", NVN_FAM_BLEND },
    { "nvnBlendStateSetAdvancedOverlap", NVN_FAM_BLEND },
    { "nvnBlendStateSetAdvancedPremultipliedSrc", NVN_FAM_BLEND },
    { "nvnBlendStateSetAdvancedNormalizedDst", NVN_FAM_BLEND },
    { "nvnBlendStateGetBlendTarget", NVN_FAM_BLEND },
    { "nvnBlendStateGetBlendFunc", NVN_FAM_BLEND },
    { "nvnBlendStateGetBlendEquation", NVN_FAM_BLEND },
    { "nvnBlendStateGetAdvancedMode", NVN_FAM_BLEND },
    { "nvnBlendStateGetAdvancedOverlap", NVN_FAM_BLEND },
    { "nvnBlendStateGetAdvancedPremultipliedSrc", NVN_FAM_BLEND },
    { "nvnBlendStateGetAdvancedNormalizedDst", NVN_FAM_BLEND },
    { "nvnColorStateSetDefaults", NVN_FAM_COLOR },
    { "nvnColorStateSetBlendEnable", NVN_FAM_COLOR },
    { "nvnColorStateSetLogicOp", NVN_FAM_COLOR },
    { "nvnColorStateSetAlphaTest", NVN_FAM_COLOR },
    { "nvnColorStateGetBlendEnable", NVN_FAM_COLOR },
    { "nvnColorStateGetLogicOp", NVN_FAM_COLOR },
    { "nvnColorStateGetAlphaTest", NVN_FAM_COLOR },
    { "nvnChannelMaskStateSetDefaults", NVN_FAM_CHANNEL },
    { "nvnChannelMaskStateSetChannelMask", NVN_FAM_CHANNEL },
    { "nvnChannelMaskStateGetChannelMask", NVN_FAM_CHANNEL },
    { "nvnMultisampleStateSetDefaults", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetMultisampleEnable", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetSamples", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetAlphaToCoverageEnable", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetAlphaToCoverageDither", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetMultisampleEnable", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetSamples", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetAlphaToCoverageEnable", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetAlphaToCoverageDither", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetRasterSamples", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetRasterSamples", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetCoverageModulationMode", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetCoverageModulationMode", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetCoverageToColorEnable", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetCoverageToColorEnable", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetCoverageToColorOutput", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetCoverageToColorOutput", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetSampleLocationsEnable", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetSampleLocationsEnable", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetSampleLocationsGrid", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetSampleLocationsGridEnable", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateGetSampleLocationsGridEnable", NVN_FAM_MULTISAMPLE },
    { "nvnMultisampleStateSetSampleLocations", NVN_FAM_MULTISAMPLE },
    { "nvnPolygonStateSetDefaults", NVN_FAM_POLYGON },
    { "nvnPolygonStateSetCullFace", NVN_FAM_POLYGON },
    { "nvnPolygonStateSetFrontFace", NVN_FAM_POLYGON },
    { "nvnPolygonStateSetPolygonMode", NVN_FAM_POLYGON },
    { "nvnPolygonStateSetPolygonOffsetEnables", NVN_FAM_POLYGON },
    { "nvnPolygonStateGetCullFace", NVN_FAM_POLYGON },
    { "nvnPolygonStateGetFrontFace", NVN_FAM_POLYGON },
    { "nvnPolygonStateGetPolygonMode", NVN_FAM_POLYGON },
    { "nvnPolygonStateGetPolygonOffsetEnables", NVN_FAM_POLYGON },
    { "nvnDepthStencilStateSetDefaults", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateSetDepthTestEnable", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateSetDepthWriteEnable", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateSetDepthFunc", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateSetStencilTestEnable", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateSetStencilFunc", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateSetStencilOp", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateGetDepthTestEnable", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateGetDepthWriteEnable", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateGetDepthFunc", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateGetStencilTestEnable", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateGetStencilFunc", NVN_FAM_DEPTH },
    { "nvnDepthStencilStateGetStencilOp", NVN_FAM_DEPTH },
    { "nvnVertexAttribStateSetDefaults", NVN_FAM_VERTEX },
    { "nvnVertexAttribStateSetFormat", NVN_FAM_VERTEX },
    { "nvnVertexAttribStateSetStreamIndex", NVN_FAM_VERTEX },
    { "nvnVertexAttribStateGetFormat", NVN_FAM_VERTEX },
    { "nvnVertexAttribStateGetStreamIndex", NVN_FAM_VERTEX },
    { "nvnVertexStreamStateSetDefaults", NVN_FAM_VERTEX },
    { "nvnVertexStreamStateSetStride", NVN_FAM_VERTEX },
    { "nvnVertexStreamStateSetDivisor", NVN_FAM_VERTEX },
    { "nvnVertexStreamStateGetStride", NVN_FAM_VERTEX },
    { "nvnVertexStreamStateGetDivisor", NVN_FAM_VERTEX },
    { "nvnCommandBufferInitialize", NVN_FAM_COMMAND },
    { "nvnCommandBufferFinalize", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetDebugLabel", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetMemoryCallback", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetMemoryCallbackData", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetCommandMemoryCallbackEnabled", NVN_FAM_COMMAND },
    { "nvnCommandBufferAddCommandMemory", NVN_FAM_COMMAND },
    { "nvnCommandBufferAddControlMemory", NVN_FAM_COMMAND },
    { "nvnCommandBufferGetCommandMemorySize", NVN_FAM_COMMAND },
    { "nvnCommandBufferGetCommandMemoryUsed", NVN_FAM_COMMAND },
    { "nvnCommandBufferGetCommandMemoryFree", NVN_FAM_COMMAND },
    { "nvnCommandBufferGetControlMemorySize", NVN_FAM_COMMAND },
    { "nvnCommandBufferGetControlMemoryUsed", NVN_FAM_COMMAND },
    { "nvnCommandBufferGetControlMemoryFree", NVN_FAM_COMMAND },
    { "nvnCommandBufferBeginRecording", NVN_FAM_COMMAND },
    { "nvnCommandBufferEndRecording", NVN_FAM_COMMAND },
    { "nvnCommandBufferCallCommands", NVN_FAM_COMMAND },
    { "nvnCommandBufferCopyCommands", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindBlendState", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindChannelMaskState", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindColorState", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindMultisampleState", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindPolygonState", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindDepthStencilState", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindVertexAttribState", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindVertexStreamState", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindProgram", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindVertexBuffer", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindVertexBuffers", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindUniformBuffer", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindUniformBuffers", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindTransformFeedbackBuffer", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindTransformFeedbackBuffers", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindStorageBuffer", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindStorageBuffers", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindTexture", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindTextures", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindImage", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindImages", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetPatchSize", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetInnerTessellationLevels", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetOuterTessellationLevels", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetPrimitiveRestart", NVN_FAM_COMMAND },
    { "nvnCommandBufferBeginTransformFeedback", NVN_FAM_COMMAND },
    { "nvnCommandBufferEndTransformFeedback", NVN_FAM_COMMAND },
    { "nvnCommandBufferPauseTransformFeedback", NVN_FAM_COMMAND },
    { "nvnCommandBufferResumeTransformFeedback", NVN_FAM_COMMAND },
    { "nvnCommandBufferDrawTransformFeedback", NVN_FAM_COMMAND },
    { "nvnCommandBufferDrawArrays", NVN_FAM_COMMAND },
    { "nvnCommandBufferDrawElements", NVN_FAM_COMMAND },
    { "nvnCommandBufferDrawElementsBaseVertex", NVN_FAM_COMMAND },
    { "nvnCommandBufferDrawArraysInstanced", NVN_FAM_COMMAND },
    { "nvnCommandBufferDrawElementsInstanced", NVN_FAM_COMMAND },
    { "nvnCommandBufferDrawArraysIndirect", NVN_FAM_COMMAND },
    { "nvnCommandBufferDrawElementsIndirect", NVN_FAM_COMMAND },
    { "nvnCommandBufferMultiDrawArraysIndirectCount", NVN_FAM_COMMAND },
    { "nvnCommandBufferMultiDrawElementsIndirectCount", NVN_FAM_COMMAND },
    { "nvnCommandBufferClearColor", NVN_FAM_COMMAND },
    { "nvnCommandBufferClearColori", NVN_FAM_COMMAND },
    { "nvnCommandBufferClearColorui", NVN_FAM_COMMAND },
    { "nvnCommandBufferClearDepthStencil", NVN_FAM_COMMAND },
    { "nvnCommandBufferDispatchCompute", NVN_FAM_COMMAND },
    { "nvnCommandBufferDispatchComputeIndirect", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetViewport", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetViewports", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetViewportSwizzles", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetScissor", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetScissors", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetDepthRange", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetDepthBounds", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetDepthRanges", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetTiledCacheAction", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetTiledCacheTileSize", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindSeparateTexture", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindSeparateSampler", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindSeparateTextures", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindSeparateSamplers", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetStencilValueMask", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetStencilMask", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetStencilRef", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetBlendColor", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetPointSize", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetLineWidth", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetPolygonOffsetClamp", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetAlphaRef", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetSampleMask", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetRasterizerDiscard", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetDepthClamp", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetConservativeRasterEnable", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetConservativeRasterDilate", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetSubpixelPrecisionBias", NVN_FAM_COMMAND },
    { "nvnCommandBufferCopyBufferToTexture", NVN_FAM_COMMAND },
    { "nvnCommandBufferCopyTextureToBuffer", NVN_FAM_COMMAND },
    { "nvnCommandBufferCopyTextureToTexture", NVN_FAM_COMMAND },
    { "nvnCommandBufferCopyBufferToBuffer", NVN_FAM_COMMAND },
    { "nvnCommandBufferClearBuffer", NVN_FAM_COMMAND },
    { "nvnCommandBufferClearTexture", NVN_FAM_COMMAND },
    { "nvnCommandBufferClearTexturei", NVN_FAM_COMMAND },
    { "nvnCommandBufferClearTextureui", NVN_FAM_COMMAND },
    { "nvnCommandBufferUpdateUniformBuffer", NVN_FAM_COMMAND },
    { "nvnCommandBufferReportCounter", NVN_FAM_COMMAND },
    { "nvnCommandBufferResetCounter", NVN_FAM_COMMAND },
    { "nvnCommandBufferReportValue", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetRenderEnable", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetRenderEnableConditional", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetRenderTargets", NVN_FAM_COMMAND },
    { "nvnCommandBufferDiscardColor", NVN_FAM_COMMAND },
    { "nvnCommandBufferDiscardDepthStencil", NVN_FAM_COMMAND },
    { "nvnCommandBufferDownsample", NVN_FAM_COMMAND },
    { "nvnCommandBufferTiledDownsample", NVN_FAM_COMMAND },
    { "nvnCommandBufferDownsampleTextureView", NVN_FAM_COMMAND },
    { "nvnCommandBufferTiledDownsampleTextureView", NVN_FAM_COMMAND },
    { "nvnCommandBufferBarrier", NVN_FAM_COMMAND },
    { "nvnCommandBufferWaitSync", NVN_FAM_COMMAND },
    { "nvnCommandBufferFenceSync", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetTexturePool", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetSamplerPool", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetShaderScratchMemory", NVN_FAM_COMMAND },
    { "nvnCommandBufferSaveZCullData", NVN_FAM_COMMAND },
    { "nvnCommandBufferRestoreZCullData", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetCopyRowStride", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetCopyImageStride", NVN_FAM_COMMAND },
    { "nvnCommandBufferGetCopyRowStride", NVN_FAM_COMMAND },
    { "nvnCommandBufferGetCopyImageStride", NVN_FAM_COMMAND },
    { "nvnCommandBufferDrawTexture", NVN_FAM_COMMAND },
    { "nvnProgramSetSubroutineLinkage", NVN_FAM_PROGRAM },
    { "nvnCommandBufferSetProgramSubroutines", NVN_FAM_COMMAND },
    { "nvnCommandBufferBindCoverageModulationTable", NVN_FAM_COMMAND },
    { "nvnCommandBufferResolveDepthBuffer", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetColorReductionEnable", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetColorReductionThresholds", NVN_FAM_COMMAND },
    { "nvnCommandBufferPushDebugGroupStatic", NVN_FAM_COMMAND },
    { "nvnCommandBufferPushDebugGroupDynamic", NVN_FAM_COMMAND },
    { "nvnCommandBufferPushDebugGroup", NVN_FAM_COMMAND },
    { "nvnCommandBufferPopDebugGroup", NVN_FAM_COMMAND },
    { "nvnCommandBufferPopDebugGroupId", NVN_FAM_COMMAND },
    { "nvnCommandBufferInsertDebugMarkerStatic", NVN_FAM_COMMAND },
    { "nvnCommandBufferInsertDebugMarkerDynamic", NVN_FAM_COMMAND },
    { "nvnCommandBufferInsertDebugMarker", NVN_FAM_COMMAND },
    { "nvnCommandBufferGetMemoryCallback", NVN_FAM_COMMAND },
    { "nvnCommandBufferGetMemoryCallbackData", NVN_FAM_COMMAND },
    { "nvnCommandBufferIsRecording", NVN_FAM_COMMAND },
    { "nvnSyncInitialize", NVN_FAM_SYNC },
    { "nvnSyncFinalize", NVN_FAM_SYNC },
    { "nvnSyncSetDebugLabel", NVN_FAM_SYNC },
    { "nvnQueueFenceSync", NVN_FAM_QUEUE },
    { "nvnSyncWait", NVN_FAM_SYNC },
    { "nvnQueueWaitSync", NVN_FAM_QUEUE },
    { "nvnSyncInitializeFromFencedGLSync", NVN_FAM_SYNC },
    { "nvnSyncCreateGLSync", NVN_FAM_SYNC },
    { "nvnEventBuilderSetDefaults", NVN_FAM_EVENT },
    { "nvnEventBuilderSetStorage", NVN_FAM_EVENT },
    { "nvnEventBuilderGetStorage", NVN_FAM_EVENT },
    { "nvnEventBuilderGetMemoryPool", NVN_FAM_EVENT },
    { "nvnEventBuilderGetMemoryOffset", NVN_FAM_EVENT },
    { "nvnEventInitialize", NVN_FAM_EVENT },
    { "nvnEventFinalize", NVN_FAM_EVENT },
    { "nvnEventGetValue", NVN_FAM_EVENT },
    { "nvnEventSignal", NVN_FAM_EVENT },
    { "nvnEventGetMemoryPool", NVN_FAM_EVENT },
    { "nvnEventGetMemoryOffset", NVN_FAM_EVENT },
    { "nvnCommandBufferWaitEvent", NVN_FAM_COMMAND },
    { "nvnCommandBufferSignalEvent", NVN_FAM_COMMAND },
    { "nvnCommandBufferSetStencilCullCriteria", NVN_FAM_COMMAND },
    { "nvnCommandBufferInsertCheckpointStatic", NVN_FAM_COMMAND },
    { "nvnCommandBufferInsertCheckpointDynamic", NVN_FAM_COMMAND },
    { "nvnDeviceSetAutomaticCheckpointsEnable", NVN_FAM_DEVICE },
    { "nvnDeviceSetAutomaticCheckpointsSamplingInterval", NVN_FAM_DEVICE },
    { "nvnDeviceSetAutomaticCheckpointsByDebugGroup", NVN_FAM_DEVICE },
    { "nvnDeviceSetCheckpointCallStacksEnable", NVN_FAM_DEVICE },
    { "nvnDeviceSetCheckpointMaxCallStacksDepth", NVN_FAM_DEVICE },
};
#define NVN_API_N ((int)(sizeof g_nvn_api / sizeof g_nvn_api[0]))

// ---- 4. 每名字一个记账桩（体内只有一次原子自增 + 首调/里程碑日志）----
#define NVN_TH_DEF(I) static uint64_t nvn_th_##I(void) { return nvn_api_touch(I); }
NVN_TH_DEF(0)
NVN_TH_DEF(1)
NVN_TH_DEF(2)
NVN_TH_DEF(3)
NVN_TH_DEF(4)
NVN_TH_DEF(5)
NVN_TH_DEF(6)
NVN_TH_DEF(7)
NVN_TH_DEF(8)
NVN_TH_DEF(9)
NVN_TH_DEF(10)
NVN_TH_DEF(11)
NVN_TH_DEF(12)
NVN_TH_DEF(13)
NVN_TH_DEF(14)
NVN_TH_DEF(15)
NVN_TH_DEF(16)
NVN_TH_DEF(17)
NVN_TH_DEF(18)
NVN_TH_DEF(19)
NVN_TH_DEF(20)
NVN_TH_DEF(21)
NVN_TH_DEF(22)
NVN_TH_DEF(23)
NVN_TH_DEF(24)
NVN_TH_DEF(25)
NVN_TH_DEF(26)
NVN_TH_DEF(27)
NVN_TH_DEF(28)
NVN_TH_DEF(29)
NVN_TH_DEF(30)
NVN_TH_DEF(31)
NVN_TH_DEF(32)
NVN_TH_DEF(33)
NVN_TH_DEF(34)
NVN_TH_DEF(35)
NVN_TH_DEF(36)
NVN_TH_DEF(37)
NVN_TH_DEF(38)
NVN_TH_DEF(39)
NVN_TH_DEF(40)
NVN_TH_DEF(41)
NVN_TH_DEF(42)
NVN_TH_DEF(43)
NVN_TH_DEF(44)
NVN_TH_DEF(45)
NVN_TH_DEF(46)
NVN_TH_DEF(47)
NVN_TH_DEF(48)
NVN_TH_DEF(49)
NVN_TH_DEF(50)
NVN_TH_DEF(51)
NVN_TH_DEF(52)
NVN_TH_DEF(53)
NVN_TH_DEF(54)
NVN_TH_DEF(55)
NVN_TH_DEF(56)
NVN_TH_DEF(57)
NVN_TH_DEF(58)
NVN_TH_DEF(59)
NVN_TH_DEF(60)
NVN_TH_DEF(61)
NVN_TH_DEF(62)
NVN_TH_DEF(63)
NVN_TH_DEF(64)
NVN_TH_DEF(65)
NVN_TH_DEF(66)
NVN_TH_DEF(67)
NVN_TH_DEF(68)
NVN_TH_DEF(69)
NVN_TH_DEF(70)
NVN_TH_DEF(71)
NVN_TH_DEF(72)
NVN_TH_DEF(73)
NVN_TH_DEF(74)
NVN_TH_DEF(75)
NVN_TH_DEF(76)
NVN_TH_DEF(77)
NVN_TH_DEF(78)
NVN_TH_DEF(79)
NVN_TH_DEF(80)
NVN_TH_DEF(81)
NVN_TH_DEF(82)
NVN_TH_DEF(83)
NVN_TH_DEF(84)
NVN_TH_DEF(85)
NVN_TH_DEF(86)
NVN_TH_DEF(87)
NVN_TH_DEF(88)
NVN_TH_DEF(89)
NVN_TH_DEF(90)
NVN_TH_DEF(91)
NVN_TH_DEF(92)
NVN_TH_DEF(93)
NVN_TH_DEF(94)
NVN_TH_DEF(95)
NVN_TH_DEF(96)
NVN_TH_DEF(97)
NVN_TH_DEF(98)
NVN_TH_DEF(99)
NVN_TH_DEF(100)
NVN_TH_DEF(101)
NVN_TH_DEF(102)
NVN_TH_DEF(103)
NVN_TH_DEF(104)
NVN_TH_DEF(105)
NVN_TH_DEF(106)
NVN_TH_DEF(107)
NVN_TH_DEF(108)
NVN_TH_DEF(109)
NVN_TH_DEF(110)
NVN_TH_DEF(111)
NVN_TH_DEF(112)
NVN_TH_DEF(113)
NVN_TH_DEF(114)
NVN_TH_DEF(115)
NVN_TH_DEF(116)
NVN_TH_DEF(117)
NVN_TH_DEF(118)
NVN_TH_DEF(119)
NVN_TH_DEF(120)
NVN_TH_DEF(121)
NVN_TH_DEF(122)
NVN_TH_DEF(123)
NVN_TH_DEF(124)
NVN_TH_DEF(125)
NVN_TH_DEF(126)
NVN_TH_DEF(127)
NVN_TH_DEF(128)
NVN_TH_DEF(129)
NVN_TH_DEF(130)
NVN_TH_DEF(131)
NVN_TH_DEF(132)
NVN_TH_DEF(133)
NVN_TH_DEF(134)
NVN_TH_DEF(135)
NVN_TH_DEF(136)
NVN_TH_DEF(137)
NVN_TH_DEF(138)
NVN_TH_DEF(139)
NVN_TH_DEF(140)
NVN_TH_DEF(141)
NVN_TH_DEF(142)
NVN_TH_DEF(143)
NVN_TH_DEF(144)
NVN_TH_DEF(145)
NVN_TH_DEF(146)
NVN_TH_DEF(147)
NVN_TH_DEF(148)
NVN_TH_DEF(149)
NVN_TH_DEF(150)
NVN_TH_DEF(151)
NVN_TH_DEF(152)
NVN_TH_DEF(153)
NVN_TH_DEF(154)
NVN_TH_DEF(155)
NVN_TH_DEF(156)
NVN_TH_DEF(157)
NVN_TH_DEF(158)
NVN_TH_DEF(159)
NVN_TH_DEF(160)
NVN_TH_DEF(161)
NVN_TH_DEF(162)
NVN_TH_DEF(163)
NVN_TH_DEF(164)
NVN_TH_DEF(165)
NVN_TH_DEF(166)
NVN_TH_DEF(167)
NVN_TH_DEF(168)
NVN_TH_DEF(169)
NVN_TH_DEF(170)
NVN_TH_DEF(171)
NVN_TH_DEF(172)
NVN_TH_DEF(173)
NVN_TH_DEF(174)
NVN_TH_DEF(175)
NVN_TH_DEF(176)
NVN_TH_DEF(177)
NVN_TH_DEF(178)
NVN_TH_DEF(179)
NVN_TH_DEF(180)
NVN_TH_DEF(181)
NVN_TH_DEF(182)
NVN_TH_DEF(183)
NVN_TH_DEF(184)
NVN_TH_DEF(185)
NVN_TH_DEF(186)
NVN_TH_DEF(187)
NVN_TH_DEF(188)
NVN_TH_DEF(189)
NVN_TH_DEF(190)
NVN_TH_DEF(191)
NVN_TH_DEF(192)
NVN_TH_DEF(193)
NVN_TH_DEF(194)
NVN_TH_DEF(195)
NVN_TH_DEF(196)
NVN_TH_DEF(197)
NVN_TH_DEF(198)
NVN_TH_DEF(199)
NVN_TH_DEF(200)
NVN_TH_DEF(201)
NVN_TH_DEF(202)
NVN_TH_DEF(203)
NVN_TH_DEF(204)
NVN_TH_DEF(205)
NVN_TH_DEF(206)
NVN_TH_DEF(207)
NVN_TH_DEF(208)
NVN_TH_DEF(209)
NVN_TH_DEF(210)
NVN_TH_DEF(211)
NVN_TH_DEF(212)
NVN_TH_DEF(213)
NVN_TH_DEF(214)
NVN_TH_DEF(215)
NVN_TH_DEF(216)
NVN_TH_DEF(217)
NVN_TH_DEF(218)
NVN_TH_DEF(219)
NVN_TH_DEF(220)
NVN_TH_DEF(221)
NVN_TH_DEF(222)
NVN_TH_DEF(223)
NVN_TH_DEF(224)
NVN_TH_DEF(225)
NVN_TH_DEF(226)
NVN_TH_DEF(227)
NVN_TH_DEF(228)
NVN_TH_DEF(229)
NVN_TH_DEF(230)
NVN_TH_DEF(231)
NVN_TH_DEF(232)
NVN_TH_DEF(233)
NVN_TH_DEF(234)
NVN_TH_DEF(235)
NVN_TH_DEF(236)
NVN_TH_DEF(237)
NVN_TH_DEF(238)
NVN_TH_DEF(239)
NVN_TH_DEF(240)
NVN_TH_DEF(241)
NVN_TH_DEF(242)
NVN_TH_DEF(243)
NVN_TH_DEF(244)
NVN_TH_DEF(245)
NVN_TH_DEF(246)
NVN_TH_DEF(247)
NVN_TH_DEF(248)
NVN_TH_DEF(249)
NVN_TH_DEF(250)
NVN_TH_DEF(251)
NVN_TH_DEF(252)
NVN_TH_DEF(253)
NVN_TH_DEF(254)
NVN_TH_DEF(255)
NVN_TH_DEF(256)
NVN_TH_DEF(257)
NVN_TH_DEF(258)
NVN_TH_DEF(259)
NVN_TH_DEF(260)
NVN_TH_DEF(261)
NVN_TH_DEF(262)
NVN_TH_DEF(263)
NVN_TH_DEF(264)
NVN_TH_DEF(265)
NVN_TH_DEF(266)
NVN_TH_DEF(267)
NVN_TH_DEF(268)
NVN_TH_DEF(269)
NVN_TH_DEF(270)
NVN_TH_DEF(271)
NVN_TH_DEF(272)
NVN_TH_DEF(273)
NVN_TH_DEF(274)
NVN_TH_DEF(275)
NVN_TH_DEF(276)
NVN_TH_DEF(277)
NVN_TH_DEF(278)
NVN_TH_DEF(279)
NVN_TH_DEF(280)
NVN_TH_DEF(281)
NVN_TH_DEF(282)
NVN_TH_DEF(283)
NVN_TH_DEF(284)
NVN_TH_DEF(285)
NVN_TH_DEF(286)
NVN_TH_DEF(287)
NVN_TH_DEF(288)
NVN_TH_DEF(289)
NVN_TH_DEF(290)
NVN_TH_DEF(291)
NVN_TH_DEF(292)
NVN_TH_DEF(293)
NVN_TH_DEF(294)
NVN_TH_DEF(295)
NVN_TH_DEF(296)
NVN_TH_DEF(297)
NVN_TH_DEF(298)
NVN_TH_DEF(299)
NVN_TH_DEF(300)
NVN_TH_DEF(301)
NVN_TH_DEF(302)
NVN_TH_DEF(303)
NVN_TH_DEF(304)
NVN_TH_DEF(305)
NVN_TH_DEF(306)
NVN_TH_DEF(307)
NVN_TH_DEF(308)
NVN_TH_DEF(309)
NVN_TH_DEF(310)
NVN_TH_DEF(311)
NVN_TH_DEF(312)
NVN_TH_DEF(313)
NVN_TH_DEF(314)
NVN_TH_DEF(315)
NVN_TH_DEF(316)
NVN_TH_DEF(317)
NVN_TH_DEF(318)
NVN_TH_DEF(319)
NVN_TH_DEF(320)
NVN_TH_DEF(321)
NVN_TH_DEF(322)
NVN_TH_DEF(323)
NVN_TH_DEF(324)
NVN_TH_DEF(325)
NVN_TH_DEF(326)
NVN_TH_DEF(327)
NVN_TH_DEF(328)
NVN_TH_DEF(329)
NVN_TH_DEF(330)
NVN_TH_DEF(331)
NVN_TH_DEF(332)
NVN_TH_DEF(333)
NVN_TH_DEF(334)
NVN_TH_DEF(335)
NVN_TH_DEF(336)
NVN_TH_DEF(337)
NVN_TH_DEF(338)
NVN_TH_DEF(339)
NVN_TH_DEF(340)
NVN_TH_DEF(341)
NVN_TH_DEF(342)
NVN_TH_DEF(343)
NVN_TH_DEF(344)
NVN_TH_DEF(345)
NVN_TH_DEF(346)
NVN_TH_DEF(347)
NVN_TH_DEF(348)
NVN_TH_DEF(349)
NVN_TH_DEF(350)
NVN_TH_DEF(351)
NVN_TH_DEF(352)
NVN_TH_DEF(353)
NVN_TH_DEF(354)
NVN_TH_DEF(355)
NVN_TH_DEF(356)
NVN_TH_DEF(357)
NVN_TH_DEF(358)
NVN_TH_DEF(359)
NVN_TH_DEF(360)
NVN_TH_DEF(361)
NVN_TH_DEF(362)
NVN_TH_DEF(363)
NVN_TH_DEF(364)
NVN_TH_DEF(365)
NVN_TH_DEF(366)
NVN_TH_DEF(367)
NVN_TH_DEF(368)
NVN_TH_DEF(369)
NVN_TH_DEF(370)
NVN_TH_DEF(371)
NVN_TH_DEF(372)
NVN_TH_DEF(373)
NVN_TH_DEF(374)
NVN_TH_DEF(375)
NVN_TH_DEF(376)
NVN_TH_DEF(377)
NVN_TH_DEF(378)
NVN_TH_DEF(379)
NVN_TH_DEF(380)
NVN_TH_DEF(381)
NVN_TH_DEF(382)
NVN_TH_DEF(383)
NVN_TH_DEF(384)
NVN_TH_DEF(385)
NVN_TH_DEF(386)
NVN_TH_DEF(387)
NVN_TH_DEF(388)
NVN_TH_DEF(389)
NVN_TH_DEF(390)
NVN_TH_DEF(391)
NVN_TH_DEF(392)
NVN_TH_DEF(393)
NVN_TH_DEF(394)
NVN_TH_DEF(395)
NVN_TH_DEF(396)
NVN_TH_DEF(397)
NVN_TH_DEF(398)
NVN_TH_DEF(399)
NVN_TH_DEF(400)
NVN_TH_DEF(401)
NVN_TH_DEF(402)
NVN_TH_DEF(403)
NVN_TH_DEF(404)
NVN_TH_DEF(405)
NVN_TH_DEF(406)
NVN_TH_DEF(407)
NVN_TH_DEF(408)
NVN_TH_DEF(409)
NVN_TH_DEF(410)
NVN_TH_DEF(411)
NVN_TH_DEF(412)
NVN_TH_DEF(413)
NVN_TH_DEF(414)
NVN_TH_DEF(415)
NVN_TH_DEF(416)
NVN_TH_DEF(417)
NVN_TH_DEF(418)
NVN_TH_DEF(419)
NVN_TH_DEF(420)
NVN_TH_DEF(421)
NVN_TH_DEF(422)
NVN_TH_DEF(423)
NVN_TH_DEF(424)
NVN_TH_DEF(425)
NVN_TH_DEF(426)
NVN_TH_DEF(427)
NVN_TH_DEF(428)
NVN_TH_DEF(429)
NVN_TH_DEF(430)
NVN_TH_DEF(431)
NVN_TH_DEF(432)
NVN_TH_DEF(433)
NVN_TH_DEF(434)
NVN_TH_DEF(435)
NVN_TH_DEF(436)
NVN_TH_DEF(437)
NVN_TH_DEF(438)
NVN_TH_DEF(439)
NVN_TH_DEF(440)
NVN_TH_DEF(441)
NVN_TH_DEF(442)
NVN_TH_DEF(443)
NVN_TH_DEF(444)
NVN_TH_DEF(445)
NVN_TH_DEF(446)
NVN_TH_DEF(447)
NVN_TH_DEF(448)
NVN_TH_DEF(449)
NVN_TH_DEF(450)
NVN_TH_DEF(451)
NVN_TH_DEF(452)
NVN_TH_DEF(453)
NVN_TH_DEF(454)
NVN_TH_DEF(455)
NVN_TH_DEF(456)
NVN_TH_DEF(457)
NVN_TH_DEF(458)
NVN_TH_DEF(459)
NVN_TH_DEF(460)
NVN_TH_DEF(461)
NVN_TH_DEF(462)
NVN_TH_DEF(463)
NVN_TH_DEF(464)
NVN_TH_DEF(465)
NVN_TH_DEF(466)
NVN_TH_DEF(467)
NVN_TH_DEF(468)
NVN_TH_DEF(469)
NVN_TH_DEF(470)
NVN_TH_DEF(471)
NVN_TH_DEF(472)
NVN_TH_DEF(473)
NVN_TH_DEF(474)
NVN_TH_DEF(475)
NVN_TH_DEF(476)
NVN_TH_DEF(477)
NVN_TH_DEF(478)
NVN_TH_DEF(479)
NVN_TH_DEF(480)
NVN_TH_DEF(481)
NVN_TH_DEF(482)
NVN_TH_DEF(483)
NVN_TH_DEF(484)
NVN_TH_DEF(485)
NVN_TH_DEF(486)
NVN_TH_DEF(487)
NVN_TH_DEF(488)
NVN_TH_DEF(489)
NVN_TH_DEF(490)
NVN_TH_DEF(491)
NVN_TH_DEF(492)
NVN_TH_DEF(493)
NVN_TH_DEF(494)
NVN_TH_DEF(495)
NVN_TH_DEF(496)
NVN_TH_DEF(497)
NVN_TH_DEF(498)
NVN_TH_DEF(499)
NVN_TH_DEF(500)
NVN_TH_DEF(501)
NVN_TH_DEF(502)
NVN_TH_DEF(503)
NVN_TH_DEF(504)
NVN_TH_DEF(505)
NVN_TH_DEF(506)
NVN_TH_DEF(507)
NVN_TH_DEF(508)
NVN_TH_DEF(509)
NVN_TH_DEF(510)
NVN_TH_DEF(511)
NVN_TH_DEF(512)
NVN_TH_DEF(513)
NVN_TH_DEF(514)
NVN_TH_DEF(515)
NVN_TH_DEF(516)
NVN_TH_DEF(517)
NVN_TH_DEF(518)
NVN_TH_DEF(519)
NVN_TH_DEF(520)
NVN_TH_DEF(521)
NVN_TH_DEF(522)

// ---- 5. 桩地址表（与 g_nvn_api 同下标）----
static const nvn_thunk_fn g_nvn_thunks[NVN_API_N] = {
    nvn_th_0,
    nvn_th_1,
    nvn_th_2,
    nvn_th_3,
    nvn_th_4,
    nvn_th_5,
    nvn_th_6,
    nvn_th_7,
    nvn_th_8,
    nvn_th_9,
    nvn_th_10,
    nvn_th_11,
    nvn_th_12,
    nvn_th_13,
    nvn_th_14,
    nvn_th_15,
    nvn_th_16,
    nvn_th_17,
    nvn_th_18,
    nvn_th_19,
    nvn_th_20,
    nvn_th_21,
    nvn_th_22,
    nvn_th_23,
    nvn_th_24,
    nvn_th_25,
    nvn_th_26,
    nvn_th_27,
    nvn_th_28,
    nvn_th_29,
    nvn_th_30,
    nvn_th_31,
    nvn_th_32,
    nvn_th_33,
    nvn_th_34,
    nvn_th_35,
    nvn_th_36,
    nvn_th_37,
    nvn_th_38,
    nvn_th_39,
    nvn_th_40,
    nvn_th_41,
    nvn_th_42,
    nvn_th_43,
    nvn_th_44,
    nvn_th_45,
    nvn_th_46,
    nvn_th_47,
    nvn_th_48,
    nvn_th_49,
    nvn_th_50,
    nvn_th_51,
    nvn_th_52,
    nvn_th_53,
    nvn_th_54,
    nvn_th_55,
    nvn_th_56,
    nvn_th_57,
    nvn_th_58,
    nvn_th_59,
    nvn_th_60,
    nvn_th_61,
    nvn_th_62,
    nvn_th_63,
    nvn_th_64,
    nvn_th_65,
    nvn_th_66,
    nvn_th_67,
    nvn_th_68,
    nvn_th_69,
    nvn_th_70,
    nvn_th_71,
    nvn_th_72,
    nvn_th_73,
    nvn_th_74,
    nvn_th_75,
    nvn_th_76,
    nvn_th_77,
    nvn_th_78,
    nvn_th_79,
    nvn_th_80,
    nvn_th_81,
    nvn_th_82,
    nvn_th_83,
    nvn_th_84,
    nvn_th_85,
    nvn_th_86,
    nvn_th_87,
    nvn_th_88,
    nvn_th_89,
    nvn_th_90,
    nvn_th_91,
    nvn_th_92,
    nvn_th_93,
    nvn_th_94,
    nvn_th_95,
    nvn_th_96,
    nvn_th_97,
    nvn_th_98,
    nvn_th_99,
    nvn_th_100,
    nvn_th_101,
    nvn_th_102,
    nvn_th_103,
    nvn_th_104,
    nvn_th_105,
    nvn_th_106,
    nvn_th_107,
    nvn_th_108,
    nvn_th_109,
    nvn_th_110,
    nvn_th_111,
    nvn_th_112,
    nvn_th_113,
    nvn_th_114,
    nvn_th_115,
    nvn_th_116,
    nvn_th_117,
    nvn_th_118,
    nvn_th_119,
    nvn_th_120,
    nvn_th_121,
    nvn_th_122,
    nvn_th_123,
    nvn_th_124,
    nvn_th_125,
    nvn_th_126,
    nvn_th_127,
    nvn_th_128,
    nvn_th_129,
    nvn_th_130,
    nvn_th_131,
    nvn_th_132,
    nvn_th_133,
    nvn_th_134,
    nvn_th_135,
    nvn_th_136,
    nvn_th_137,
    nvn_th_138,
    nvn_th_139,
    nvn_th_140,
    nvn_th_141,
    nvn_th_142,
    nvn_th_143,
    nvn_th_144,
    nvn_th_145,
    nvn_th_146,
    nvn_th_147,
    nvn_th_148,
    nvn_th_149,
    nvn_th_150,
    nvn_th_151,
    nvn_th_152,
    nvn_th_153,
    nvn_th_154,
    nvn_th_155,
    nvn_th_156,
    nvn_th_157,
    nvn_th_158,
    nvn_th_159,
    nvn_th_160,
    nvn_th_161,
    nvn_th_162,
    nvn_th_163,
    nvn_th_164,
    nvn_th_165,
    nvn_th_166,
    nvn_th_167,
    nvn_th_168,
    nvn_th_169,
    nvn_th_170,
    nvn_th_171,
    nvn_th_172,
    nvn_th_173,
    nvn_th_174,
    nvn_th_175,
    nvn_th_176,
    nvn_th_177,
    nvn_th_178,
    nvn_th_179,
    nvn_th_180,
    nvn_th_181,
    nvn_th_182,
    nvn_th_183,
    nvn_th_184,
    nvn_th_185,
    nvn_th_186,
    nvn_th_187,
    nvn_th_188,
    nvn_th_189,
    nvn_th_190,
    nvn_th_191,
    nvn_th_192,
    nvn_th_193,
    nvn_th_194,
    nvn_th_195,
    nvn_th_196,
    nvn_th_197,
    nvn_th_198,
    nvn_th_199,
    nvn_th_200,
    nvn_th_201,
    nvn_th_202,
    nvn_th_203,
    nvn_th_204,
    nvn_th_205,
    nvn_th_206,
    nvn_th_207,
    nvn_th_208,
    nvn_th_209,
    nvn_th_210,
    nvn_th_211,
    nvn_th_212,
    nvn_th_213,
    nvn_th_214,
    nvn_th_215,
    nvn_th_216,
    nvn_th_217,
    nvn_th_218,
    nvn_th_219,
    nvn_th_220,
    nvn_th_221,
    nvn_th_222,
    nvn_th_223,
    nvn_th_224,
    nvn_th_225,
    nvn_th_226,
    nvn_th_227,
    nvn_th_228,
    nvn_th_229,
    nvn_th_230,
    nvn_th_231,
    nvn_th_232,
    nvn_th_233,
    nvn_th_234,
    nvn_th_235,
    nvn_th_236,
    nvn_th_237,
    nvn_th_238,
    nvn_th_239,
    nvn_th_240,
    nvn_th_241,
    nvn_th_242,
    nvn_th_243,
    nvn_th_244,
    nvn_th_245,
    nvn_th_246,
    nvn_th_247,
    nvn_th_248,
    nvn_th_249,
    nvn_th_250,
    nvn_th_251,
    nvn_th_252,
    nvn_th_253,
    nvn_th_254,
    nvn_th_255,
    nvn_th_256,
    nvn_th_257,
    nvn_th_258,
    nvn_th_259,
    nvn_th_260,
    nvn_th_261,
    nvn_th_262,
    nvn_th_263,
    nvn_th_264,
    nvn_th_265,
    nvn_th_266,
    nvn_th_267,
    nvn_th_268,
    nvn_th_269,
    nvn_th_270,
    nvn_th_271,
    nvn_th_272,
    nvn_th_273,
    nvn_th_274,
    nvn_th_275,
    nvn_th_276,
    nvn_th_277,
    nvn_th_278,
    nvn_th_279,
    nvn_th_280,
    nvn_th_281,
    nvn_th_282,
    nvn_th_283,
    nvn_th_284,
    nvn_th_285,
    nvn_th_286,
    nvn_th_287,
    nvn_th_288,
    nvn_th_289,
    nvn_th_290,
    nvn_th_291,
    nvn_th_292,
    nvn_th_293,
    nvn_th_294,
    nvn_th_295,
    nvn_th_296,
    nvn_th_297,
    nvn_th_298,
    nvn_th_299,
    nvn_th_300,
    nvn_th_301,
    nvn_th_302,
    nvn_th_303,
    nvn_th_304,
    nvn_th_305,
    nvn_th_306,
    nvn_th_307,
    nvn_th_308,
    nvn_th_309,
    nvn_th_310,
    nvn_th_311,
    nvn_th_312,
    nvn_th_313,
    nvn_th_314,
    nvn_th_315,
    nvn_th_316,
    nvn_th_317,
    nvn_th_318,
    nvn_th_319,
    nvn_th_320,
    nvn_th_321,
    nvn_th_322,
    nvn_th_323,
    nvn_th_324,
    nvn_th_325,
    nvn_th_326,
    nvn_th_327,
    nvn_th_328,
    nvn_th_329,
    nvn_th_330,
    nvn_th_331,
    nvn_th_332,
    nvn_th_333,
    nvn_th_334,
    nvn_th_335,
    nvn_th_336,
    nvn_th_337,
    nvn_th_338,
    nvn_th_339,
    nvn_th_340,
    nvn_th_341,
    nvn_th_342,
    nvn_th_343,
    nvn_th_344,
    nvn_th_345,
    nvn_th_346,
    nvn_th_347,
    nvn_th_348,
    nvn_th_349,
    nvn_th_350,
    nvn_th_351,
    nvn_th_352,
    nvn_th_353,
    nvn_th_354,
    nvn_th_355,
    nvn_th_356,
    nvn_th_357,
    nvn_th_358,
    nvn_th_359,
    nvn_th_360,
    nvn_th_361,
    nvn_th_362,
    nvn_th_363,
    nvn_th_364,
    nvn_th_365,
    nvn_th_366,
    nvn_th_367,
    nvn_th_368,
    nvn_th_369,
    nvn_th_370,
    nvn_th_371,
    nvn_th_372,
    nvn_th_373,
    nvn_th_374,
    nvn_th_375,
    nvn_th_376,
    nvn_th_377,
    nvn_th_378,
    nvn_th_379,
    nvn_th_380,
    nvn_th_381,
    nvn_th_382,
    nvn_th_383,
    nvn_th_384,
    nvn_th_385,
    nvn_th_386,
    nvn_th_387,
    nvn_th_388,
    nvn_th_389,
    nvn_th_390,
    nvn_th_391,
    nvn_th_392,
    nvn_th_393,
    nvn_th_394,
    nvn_th_395,
    nvn_th_396,
    nvn_th_397,
    nvn_th_398,
    nvn_th_399,
    nvn_th_400,
    nvn_th_401,
    nvn_th_402,
    nvn_th_403,
    nvn_th_404,
    nvn_th_405,
    nvn_th_406,
    nvn_th_407,
    nvn_th_408,
    nvn_th_409,
    nvn_th_410,
    nvn_th_411,
    nvn_th_412,
    nvn_th_413,
    nvn_th_414,
    nvn_th_415,
    nvn_th_416,
    nvn_th_417,
    nvn_th_418,
    nvn_th_419,
    nvn_th_420,
    nvn_th_421,
    nvn_th_422,
    nvn_th_423,
    nvn_th_424,
    nvn_th_425,
    nvn_th_426,
    nvn_th_427,
    nvn_th_428,
    nvn_th_429,
    nvn_th_430,
    nvn_th_431,
    nvn_th_432,
    nvn_th_433,
    nvn_th_434,
    nvn_th_435,
    nvn_th_436,
    nvn_th_437,
    nvn_th_438,
    nvn_th_439,
    nvn_th_440,
    nvn_th_441,
    nvn_th_442,
    nvn_th_443,
    nvn_th_444,
    nvn_th_445,
    nvn_th_446,
    nvn_th_447,
    nvn_th_448,
    nvn_th_449,
    nvn_th_450,
    nvn_th_451,
    nvn_th_452,
    nvn_th_453,
    nvn_th_454,
    nvn_th_455,
    nvn_th_456,
    nvn_th_457,
    nvn_th_458,
    nvn_th_459,
    nvn_th_460,
    nvn_th_461,
    nvn_th_462,
    nvn_th_463,
    nvn_th_464,
    nvn_th_465,
    nvn_th_466,
    nvn_th_467,
    nvn_th_468,
    nvn_th_469,
    nvn_th_470,
    nvn_th_471,
    nvn_th_472,
    nvn_th_473,
    nvn_th_474,
    nvn_th_475,
    nvn_th_476,
    nvn_th_477,
    nvn_th_478,
    nvn_th_479,
    nvn_th_480,
    nvn_th_481,
    nvn_th_482,
    nvn_th_483,
    nvn_th_484,
    nvn_th_485,
    nvn_th_486,
    nvn_th_487,
    nvn_th_488,
    nvn_th_489,
    nvn_th_490,
    nvn_th_491,
    nvn_th_492,
    nvn_th_493,
    nvn_th_494,
    nvn_th_495,
    nvn_th_496,
    nvn_th_497,
    nvn_th_498,
    nvn_th_499,
    nvn_th_500,
    nvn_th_501,
    nvn_th_502,
    nvn_th_503,
    nvn_th_504,
    nvn_th_505,
    nvn_th_506,
    nvn_th_507,
    nvn_th_508,
    nvn_th_509,
    nvn_th_510,
    nvn_th_511,
    nvn_th_512,
    nvn_th_513,
    nvn_th_514,
    nvn_th_515,
    nvn_th_516,
    nvn_th_517,
    nvn_th_518,
    nvn_th_519,
    nvn_th_520,
    nvn_th_521,
    nvn_th_522,
};
