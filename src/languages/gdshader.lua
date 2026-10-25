local L = {}

-- Godot shading language is GLSL-like (C comments, #include/#define), so it
-- runs on LexCPP. Word lists follow the Godot 4 shader reference.
L.lexer = "cpp"

L.singleLineComment = "// "

L.tabSize = 4

L.extensions = {
	"gdshader",
	"gdshaderinc",
}

L.properties = {
	["fold.cpp.comment.explicit"] = "0",
	["lexer.cpp.track.preprocessor"] = "0",
}

L.keywords = {
	-- Declarations, qualifiers, flow control.
	[0] = "break case const continue default discard do else false flat for global group_uniforms highp if in inout instance lowp mediump out render_mode return shader_type smooth stencil_mode struct switch true uniform varying while",
	-- Data types.
	[1] = "bool bvec2 bvec3 bvec4 float int isampler2D isampler2DArray isampler3D ivec2 ivec3 ivec4 mat2 mat3 mat4 sampler2D sampler2DArray sampler3D samplerCube samplerCubeArray samplerExternalOES uint usampler2D usampler2DArray usampler3D uvec2 uvec3 uvec4 vec2 vec3 vec4 void",
	-- Built-in functions, variables, constants and defines; shader types,
	-- render/stencil modes and uniform hints.
	[3] = "abs acos acosh all any asin asinh atan atanh bitCount bitfieldExtract bitfieldInsert bitfieldReverse ceil clamp cos cosh cross dFdx dFdxCoarse dFdxFine dFdy dFdyCoarse dFdyFine degrees determinant distance dot emit_subparticle equal exp exp2 faceforward findLSB findMSB floatBitsToInt floatBitsToUint floor fma fract frexp fwidth fwidthCoarse fwidthFine greaterThan greaterThanEqual imulExtended intBitsToFloat inverse inversesqrt isinf isnan ldexp length lessThan lessThanEqual log log2 matrixCompMult max min mix mod modf normalize not notEqual outerProduct packHalf2x16 packSnorm2x16 packSnorm4x8 packUnorm2x16 packUnorm4x8 pow radians reflect refract round roundEven screen_uv_to_sdf sdf_to_screen_uv sign sin sinh smoothstep sqrt step tan tanh texelFetch texture textureGather textureGrad textureLod textureProj textureProjGrad textureProjLod textureQueryLevels textureQueryLod textureSize texture_sdf texture_sdf_normal transpose trunc uaddCarry uintBitsToFloat umulExtended unpackHalf2x16 unpackSnorm2x16 unpackSnorm4x8 unpackUnorm2x16 unpackUnorm4x8 usubBorrow " ..
		"ACTIVE ALBEDO ALPHA ALPHA_ANTIALIASING_EDGE ALPHA_HASH_SCALE ALPHA_SCISSOR_THRESHOLD ALPHA_TEXTURE_COORDINATE AMOUNT_RATIO ANISOTROPY ANISOTROPY_FLOW AO AO_LIGHT_AFFECT AT_CUBEMAP_PASS AT_HALF_RES_PASS AT_LIGHT_PASS AT_QUARTER_RES_PASS ATTENUATION ATTRACTOR_FORCE BACKLIGHT BENT_NORMAL_MAP BINORMAL BONE_INDICES BONE_WEIGHTS CAMERA_DIRECTION_WORLD CAMERA_POSITION_WORLD CAMERA_VISIBLE_LAYERS CANVAS_MATRIX CLEARCOAT CLEARCOAT_GLOSS CLIP_SPACE_FAR COLLIDED COLLISION_DEPTH COLLISION_NORMAL COLOR CUSTOM CUSTOM0 CUSTOM1 CUSTOM2 CUSTOM3 DELTA DENSITY DEPTH DEPTH_TEXTURE DIFFUSE_LIGHT E EMISSION EMISSION_TRANSFORM EMITTER_VELOCITY EYEDIR EYE_OFFSET FLAG_EMIT_COLOR FLAG_EMIT_CUSTOM FLAG_EMIT_POSITION FLAG_EMIT_ROT_SCALE FLAG_EMIT_VELOCITY FOG FRAGCOORD FRONT_FACING HALF_RES_COLOR INDEX INSTANCE_CUSTOM INSTANCE_ID INTERPOLATE_TO_END INV_PROJECTION_MATRIX INV_VIEW_MATRIX IN_SHADOW_PASS IRRADIANCE IS_MULTIVIEW LIFETIME LIGHT LIGHT_COLOR LIGHT_DIRECTION LIGHT_ENERGY LIGHT_IS_DIRECTIONAL LIGHT_POSITION LIGHT_VERTEX LIGHT0_COLOR LIGHT0_DIRECTION LIGHT0_ENABLED LIGHT0_ENERGY LIGHT0_SIZE LIGHT1_COLOR LIGHT1_DIRECTION LIGHT1_ENABLED LIGHT1_ENERGY LIGHT1_SIZE LIGHT2_COLOR LIGHT2_DIRECTION LIGHT2_ENABLED LIGHT2_ENERGY LIGHT2_SIZE LIGHT3_COLOR LIGHT3_DIRECTION LIGHT3_ENABLED LIGHT3_ENERGY LIGHT3_SIZE MAIN_CAM_INV_VIEW_MATRIX MASS METALLIC MODELVIEW_MATRIX MODELVIEW_NORMAL_MATRIX MODEL_MATRIX MODEL_NORMAL_MATRIX NODE_POSITION_VIEW NODE_POSITION_WORLD NORMAL NORMAL_MAP NORMAL_MAP_DEPTH NORMAL_TEXTURE NUMBER OBJECT_POSITION OUTPUT_IS_SRGB PI POINT_COORD POINT_SIZE POSITION PREMUL_ALPHA_FACTOR PROJECTION_MATRIX QUARTER_RES_COLOR RADIANCE RANDOM_SEED REGION_RECT RESTART RESTART_COLOR RESTART_CUSTOM RESTART_POSITION RESTART_ROT_SCALE RESTART_VELOCITY RIM RIM_TINT ROUGHNESS SCREEN_MATRIX SCREEN_PIXEL_SIZE SCREEN_TEXTURE SCREEN_UV SDF SHADOW_MODULATE SHADOW_VERTEX SIZE SKY_COORDS SPECULAR SPECULAR_AMOUNT SPECULAR_LIGHT SPECULAR_SHININESS SPECULAR_SHININESS_TEXTURE SSS_STRENGTH SSS_TRANSMITTANCE_BOOST SSS_TRANSMITTANCE_COLOR SSS_TRANSMITTANCE_DEPTH TANGENT TAU TEXTURE TEXTURE_PIXEL_SIZE TIME TRANSFORM USERDATA1 USERDATA2 USERDATA3 USERDATA4 USERDATA5 USERDATA6 UV UV2 UVW VELOCITY VERTEX VERTEX_ID VIEW VIEWPORT_SIZE VIEW_INDEX VIEW_MATRIX VIEW_MONO_LEFT VIEW_RIGHT WORLD_POSITION Z_CLIP_SCALE CURRENT_RENDERER RENDERER_COMPATIBILITY RENDERER_FORWARD_PLUS RENDERER_MOBILE " ..
		"canvas_item fog particles sky spatial " ..
		"alpha_to_coverage alpha_to_coverage_and_one ambient_light_disabled blend_add blend_disabled blend_mix blend_mul blend_premul_alpha blend_sub collision_use_scale cull_back cull_disabled cull_front debug_shadow_splits depth_draw_always depth_draw_never depth_draw_opaque depth_prepass_alpha depth_test_default depth_test_disabled depth_test_inverted diffuse_burley diffuse_lambert diffuse_lambert_wrap diffuse_toon disable_fog disable_force disable_velocity ensure_correct_normals fog_disabled keep_data light_only particle_trails shadow_to_opacity shadows_disabled skip_vertex_transform specular_disabled specular_schlick_ggx specular_toon sss_mode_skin unshaded use_half_res_pass use_quarter_res_pass vertex_lighting wireframe world_vertex_coords " ..
		"compare_always compare_equal compare_greater compare_greater_or_equal compare_less compare_less_or_equal compare_not_equal write_if_depth_fail " ..
		"filter_linear filter_linear_mipmap filter_linear_mipmap_anisotropic filter_nearest filter_nearest_mipmap filter_nearest_mipmap_anisotropic hint_anisotropy hint_default_black hint_default_transparent hint_default_white hint_depth_texture hint_enum hint_normal hint_normal_roughness_texture hint_range hint_roughness_a hint_roughness_b hint_roughness_g hint_roughness_gray hint_roughness_normal hint_roughness_r hint_screen_texture instance_index repeat_disable repeat_enable source_color",
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
