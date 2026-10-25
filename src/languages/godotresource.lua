local L = {}

-- Godot's text resource and ConfigFile formats: [section attr="..."] headers,
-- key = value lines, ';' comments. LexProps styles the whole key up to '=',
-- which keeps slash paths (display/window/size/viewport_width) in one piece;
-- LexTOML would split them at every '/'.
L.lexer = "props"

L.singleLineComment = "; "

L.extensions = {
	"tscn",
	"tres",
	"escn",
	"godot",
	"import",
	"gdextension",
	"remap",
	"gdnlib",
	"gdns",
}

L.filenames = {
	"export_presets.cfg",
	"override.cfg",
}

L.keywords = {
}

L.styles = {
	["DEFAULT"] = {
		id = 0,
		fgColor = rgb(0x000000),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENT"] = {
		id = 1,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
	},
	["SECTION"] = {
		id = 2,
		fgColor = rgb(0x8000FF),
		bgColor = rgb(0xF2F4FF),
		fontStyle = 9,
	},
	["ASSIGNMENT"] = {
		id = 3,
		fgColor = rgb(0xFF0000),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["DEFVAL"] = {
		id = 4,
		fgColor = rgb(0xFF0000),
		bgColor = rgb(0xFFFFFF),
	},
	["KEY"] = {
		id = 5,
		fgColor = rgb(0x0000FF),
		bgColor = rgb(0xFFFFFF),
	},
}
return L
