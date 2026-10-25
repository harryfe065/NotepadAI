local L = {}

L.lexer = "gdscript"

L.singleLineComment = "# "

L.tabSize = 4

L.first_line = {
	"^#!.*godot"
}

L.extensions = {
	"gd",
}

L.properties = {
	["lexer.gdscript.keywords2.no.sub.identifiers"] = "1",
}

-- Godot 4 keywords (docs: GDScript reference > Keywords) plus the reserved
-- operators and literals.
L.keywords = {
	[0] = "and as assert await break breakpoint class class_name const continue elif else enum extends false for func if in is match namespace not null or pass preload return self signal static super trait true var void when while yield INF NAN PI TAU",
	[1] = "AABB Array Basis Callable Color Dictionary NodePath Object PackedByteArray PackedColorArray PackedFloat32Array PackedFloat64Array PackedInt32Array PackedInt64Array PackedStringArray PackedVector2Array PackedVector3Array PackedVector4Array Plane Projection Quaternion RID Rect2 Rect2i Signal String StringName Transform2D Transform3D Variant Vector2 Vector2i Vector3 Vector3i Vector4 Vector4i bool float int " ..
		"Color8 abs absf absi acos acosh angle_difference asin asinh atan atan2 atanh bezier_derivative bezier_interpolate bytes_to_var bytes_to_var_with_objects ceil ceilf ceili char clamp clampf clampi convert cos cosh cubic_interpolate cubic_interpolate_angle db_to_linear deg_to_rad dict_to_inst ease error_string exp floor floorf floori fmod fposmod get_stack hash inst_to_dict instance_from_id inverse_lerp is_equal_approx is_finite is_inf is_instance_id_valid is_instance_of is_instance_valid is_nan is_same is_zero_approx len lerp lerp_angle lerpf linear_to_db load log max maxf maxi min minf mini move_toward nearest_po2 ord pingpong posmod pow print print_debug print_rich print_stack printerr printraw prints printt push_error push_warning rad_to_deg rand_from_seed randf randf_range randfn randi randi_range randomize range remap rotate_toward round roundf roundi seed sign signf signi sin sinh smoothstep snapped snappedf snappedi sqrt step_decimals str str_to_var tan tanh type_convert type_exists type_string typeof var_to_bytes var_to_bytes_with_objects var_to_str weakref wrap wrapf wrapi",
}

L.styles = {
	["DEFAULT"] = {
		id = 0,
		fgColor = rgb(0x000000),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENTLINE"] = {
		id = 1,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
	},
	["NUMBER"] = {
		id = 2,
		fgColor = rgb(0xFF0000),
		bgColor = rgb(0xFFFFFF),
	},
	["STRING"] = {
		id = 3,
		fgColor = rgb(0x808080),
		bgColor = rgb(0xFFFFFF),
	},
	["CHARACTER"] = {
		id = 4,
		fgColor = rgb(0x808080),
		bgColor = rgb(0xFFFFFF),
	},
	["KEYWORDS"] = {
		id = 5,
		fgColor = rgb(0x0000FF),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["TRIPLE"] = {
		id = 6,
		fgColor = rgb(0xFF8000),
		bgColor = rgb(0xFFFFFF),
	},
	["TRIPLEDOUBLE"] = {
		id = 7,
		fgColor = rgb(0xFF8000),
		bgColor = rgb(0xFFFFFF),
	},
	["CLASSNAME"] = {
		id = 8,
		fgColor = rgb(0x000000),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["FUNCNAME"] = {
		id = 9,
		fgColor = rgb(0xFF00FF),
		bgColor = rgb(0xFFFFFF),
	},
	["OPERATOR"] = {
		id = 10,
		fgColor = rgb(0x000080),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["IDENTIFIER"] = {
		id = 11,
		fgColor = rgb(0x000000),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENTBLOCK"] = {
		id = 12,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
	},
	["STRINGEOL"] = {
		id = 13,
		fgColor = rgb(0x808080),
		bgColor = rgb(0xFFFFFF),
	},
	["BUILTINS"] = {
		id = 14,
		fgColor = rgb(0x880088),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["ANNOTATION"] = {
		id = 15,
		fgColor = rgb(0xFF8000),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 2,
	},
	["NODEPATH"] = {
		id = 16,
		fgColor = rgb(0x008080),
		bgColor = rgb(0xFFFFFF),
	},
}
return L
