local L = {}

-- GLSL (OpenGL / Vulkan dialect), as used by Godot compute shaders
-- (RDShaderFile, with #[compute] section markers) and glslang's stage
-- extensions. Layout qualifiers that are also common variable names
-- (offset, index, set, location, component, align) are left out so they
-- never highlight user identifiers.
L.lexer = "cpp"

L.singleLineComment = "// "

L.tabSize = 4

L.first_line = {
	"^#version%s+%d+",
	"^#%[compute%]",
}

L.extensions = {
	"glsl",
	"vert",
	"frag",
	"geom",
	"comp",
	"tesc",
	"tese",
}

L.properties = {
	["fold.cpp.comment.explicit"] = "0",
	["lexer.cpp.track.preprocessor"] = "0",
}

L.keywords = {
	-- Declarations, qualifiers, flow control.
	[0] = "attribute break buffer case centroid coherent const continue default discard do else false flat for highp if in inout invariant layout lowp mediump noperspective out patch precise precision readonly restrict return sample shared smooth struct subroutine switch true uniform varying volatile while writeonly",
	-- Data types.
	[1] = "atomic_uint bool bvec2 bvec3 bvec4 double dmat2 dmat2x2 dmat2x3 dmat2x4 dmat3 dmat3x2 dmat3x3 dmat3x4 dmat4 dmat4x2 dmat4x3 dmat4x4 dvec2 dvec3 dvec4 float int ivec2 ivec3 ivec4 mat2 mat2x2 mat2x3 mat2x4 mat3 mat3x2 mat3x3 mat3x4 mat4 mat4x2 mat4x3 mat4x4 uint uvec2 uvec3 uvec4 vec2 vec3 vec4 void " ..
		"sampler sampler1D sampler1DArray sampler1DArrayShadow sampler1DShadow sampler2D sampler2DArray sampler2DArrayShadow sampler2DMS sampler2DMSArray sampler2DRect sampler2DRectShadow sampler2DShadow sampler3D samplerBuffer samplerCube samplerCubeArray samplerCubeArrayShadow samplerCubeShadow samplerShadow " ..
		"isampler1D isampler1DArray isampler2D isampler2DArray isampler2DMS isampler2DMSArray isampler2DRect isampler3D isamplerBuffer isamplerCube isamplerCubeArray usampler1D usampler1DArray usampler2D usampler2DArray usampler2DMS usampler2DMSArray usampler2DRect usampler3D usamplerBuffer usamplerCube usamplerCubeArray " ..
		"image1D image1DArray image2D image2DArray image2DMS image2DMSArray image2DRect image3D imageBuffer imageCube imageCubeArray iimage1D iimage1DArray iimage2D iimage2DArray iimage2DMS iimage2DMSArray iimage2DRect iimage3D iimageBuffer iimageCube iimageCubeArray uimage1D uimage1DArray uimage2D uimage2DArray uimage2DMS uimage2DMSArray uimage2DRect uimage3D uimageBuffer uimageCube uimageCubeArray " ..
		"texture1D texture1DArray texture2D texture2DArray texture2DMS texture2DMSArray texture3D textureBuffer textureCube textureCubeArray subpassInput subpassInputMS isubpassInput isubpassInputMS usubpassInput usubpassInputMS",
	-- Built-in functions and variables, layout qualifiers, image formats.
	[3] = "EmitStreamVertex EmitVertex EndPrimitive EndStreamPrimitive abs acos acosh all any asin asinh atan atanh atomicAdd atomicAnd atomicCompSwap atomicCounter atomicCounterDecrement atomicCounterIncrement atomicExchange atomicMax atomicMin atomicOr atomicXor barrier bitCount bitfieldExtract bitfieldInsert bitfieldReverse ceil clamp cos cosh cross dFdx dFdxCoarse dFdxFine dFdy dFdyCoarse dFdyFine degrees determinant distance dot equal exp exp2 faceforward findLSB findMSB floatBitsToInt floatBitsToUint floor fma fract frexp fwidth fwidthCoarse fwidthFine greaterThan greaterThanEqual groupMemoryBarrier imageAtomicAdd imageAtomicAnd imageAtomicCompSwap imageAtomicExchange imageAtomicMax imageAtomicMin imageAtomicOr imageAtomicXor imageLoad imageSamples imageSize imageStore imulExtended intBitsToFloat interpolateAtCentroid interpolateAtOffset interpolateAtSample inverse inversesqrt isinf isnan ldexp length lessThan lessThanEqual log log2 matrixCompMult max memoryBarrier memoryBarrierAtomicCounter memoryBarrierBuffer memoryBarrierImage memoryBarrierShared min mix mod modf normalize not notEqual outerProduct packDouble2x32 packHalf2x16 packSnorm2x16 packSnorm4x8 packUnorm2x16 packUnorm4x8 pow radians reflect refract round roundEven sign sin sinh smoothstep sqrt step subpassLoad tan tanh texelFetch texelFetchOffset texture textureGather textureGatherOffset textureGatherOffsets textureGrad textureGradOffset textureLod textureLodOffset textureOffset textureProj textureProjGrad textureProjGradOffset textureProjLod textureProjLodOffset textureProjOffset textureQueryLevels textureQueryLod textureSamples textureSize transpose trunc uaddCarry uintBitsToFloat umulExtended unpackDouble2x32 unpackHalf2x16 unpackSnorm2x16 unpackSnorm4x8 unpackUnorm2x16 unpackUnorm4x8 usubBorrow " ..
		"gl_BaseInstance gl_BaseVertex gl_ClipDistance gl_CullDistance gl_DrawID gl_FragColor gl_FragCoord gl_FragData gl_FragDepth gl_FrontFacing gl_GlobalInvocationID gl_HelperInvocation gl_InstanceID gl_InstanceIndex gl_InvocationID gl_Layer gl_LocalInvocationID gl_LocalInvocationIndex gl_NumSubgroups gl_NumWorkGroups gl_PatchVerticesIn gl_PerVertex gl_PointCoord gl_PointSize gl_Position gl_PrimitiveID gl_PrimitiveIDIn gl_SampleID gl_SampleMask gl_SampleMaskIn gl_SamplePosition gl_SubgroupID gl_SubgroupInvocationID gl_SubgroupSize gl_TessCoord gl_TessLevelInner gl_TessLevelOuter gl_VertexID gl_VertexIndex gl_ViewIndex gl_ViewportIndex gl_WorkGroupID gl_WorkGroupSize gl_in gl_out " ..
		"binding column_major constant_id early_fragment_tests equal_spacing fractional_even_spacing fractional_odd_spacing input_attachment_index isolines line_strip lines_adjacency local_size_x local_size_x_id local_size_y local_size_y_id local_size_z local_size_z_id max_vertices origin_upper_left pixel_center_integer point_mode push_constant row_major std140 std430 triangle_strip triangles_adjacency " ..
		"r11f_g11f_b10f r16 r16_snorm r16f r16i r16ui r32f r32i r32ui r8 r8_snorm r8i r8ui rg16 rg16_snorm rg16f rg16i rg16ui rg32f rg32i rg32ui rg8 rg8_snorm rg8i rg8ui rgb10_a2 rgb10_a2ui rgba16 rgba16_snorm rgba16f rgba16i rgba16ui rgba32f rgba32i rgba32ui rgba8 rgba8_snorm rgba8i rgba8ui",
	[5] = "FIXME TODO XXX HACK NOTE",
}

L.styles = {
	["PREPROCESSOR"] = {
		id = 9,
		fgColor = rgb(0x804000),
		bgColor = rgb(0xFFFFFF),
	},
	["DEFAULT"] = {
		id = 11,
		fgColor = rgb(0x000000),
		bgColor = rgb(0xFFFFFF),
	},
	["INSTRUCTION WORD"] = {
		id = 5,
		fgColor = rgb(0x0000FF),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["TYPE WORD"] = {
		id = 16,
		fgColor = rgb(0x8000FF),
		bgColor = rgb(0xFFFFFF),
	},
	["BUILTINS"] = {
		id = 19,
		fgColor = rgb(0x880088),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["NUMBER"] = {
		id = 4,
		fgColor = rgb(0xFF8000),
		bgColor = rgb(0xFFFFFF),
	},
	["STRING"] = {
		id = 6,
		fgColor = rgb(0x808080),
		bgColor = rgb(0xFFFFFF),
	},
	["CHARACTER"] = {
		id = 7,
		fgColor = rgb(0x808080),
		bgColor = rgb(0xFFFFFF),
	},
	["OPERATOR"] = {
		id = 10,
		fgColor = rgb(0x000080),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["COMMENT"] = {
		id = 1,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENT LINE"] = {
		id = 2,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENT DOC"] = {
		id = 3,
		fgColor = rgb(0x008080),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENT LINE DOC"] = {
		id = 15,
		fgColor = rgb(0x008080),
		bgColor = rgb(0xFFFFFF),
	},
	["PREPROCESSOR COMMENT"] = {
		id = 23,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
	},
	["PREPROCESSOR COMMENT DOC"] = {
		id = 24,
		fgColor = rgb(0x008080),
		bgColor = rgb(0xFFFFFF),
	},
	["TASK MARKER"] = {
		id = 26,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
}
return L
