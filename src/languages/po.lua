local L = {}

L.lexer = "po"

L.singleLineComment = "# "

L.extensions = {
	"po",
	"pot",
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
	["MSGID"] = {
		id = 2,
		fgColor = rgb(0x0000FF),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["MSGID TEXT"] = {
		id = 3,
		fgColor = rgb(0x808080),
		bgColor = rgb(0xFFFFFF),
	},
	["MSGSTR"] = {
		id = 4,
		fgColor = rgb(0x0000FF),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["MSGSTR TEXT"] = {
		id = 5,
		fgColor = rgb(0x800000),
		bgColor = rgb(0xFFFFFF),
	},
	["MSGCTXT"] = {
		id = 6,
		fgColor = rgb(0x8000FF),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["MSGCTXT TEXT"] = {
		id = 7,
		fgColor = rgb(0x8000FF),
		bgColor = rgb(0xFFFFFF),
	},
	["FUZZY"] = {
		id = 8,
		fgColor = rgb(0xFF8000),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["PROGRAMMER COMMENT"] = {
		id = 9,
		fgColor = rgb(0x008080),
		bgColor = rgb(0xFFFFFF),
	},
	["REFERENCE"] = {
		id = 10,
		fgColor = rgb(0x808080),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 2,
	},
	["FLAGS"] = {
		id = 11,
		fgColor = rgb(0x804000),
		bgColor = rgb(0xFFFFFF),
	},
	["MSGID TEXT EOL"] = {
		id = 12,
		fgColor = rgb(0x808080),
		bgColor = rgb(0xFFFFFF),
	},
	["MSGSTR TEXT EOL"] = {
		id = 13,
		fgColor = rgb(0x800000),
		bgColor = rgb(0xFFFFFF),
	},
	["MSGCTXT TEXT EOL"] = {
		id = 14,
		fgColor = rgb(0x8000FF),
		bgColor = rgb(0xFFFFFF),
	},
	["ERROR"] = {
		id = 15,
		fgColor = rgb(0xFF0000),
		bgColor = rgb(0xFFFFFF),
	},
}
return L
