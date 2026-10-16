file(MAKE_DIRECTORY "${PACKAGE_DIR}")
set(PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")

# Determine configuration for multi- or single-config generators
if(CMAKE_CONFIGURATION_TYPES) # multi-config generator (VS, Xcode)
	set(PACKAGE_CONFIG "$<CONFIG>")
else() # single-config generator (Ninja, Makefiles)
	if(NOT CMAKE_BUILD_TYPE)
		set(PACKAGE_CONFIG "Debug")
	else()
		set(PACKAGE_CONFIG "${CMAKE_BUILD_TYPE}")
	endif()
endif()

# Path to your executable
set(TARGET_EXE "$<TARGET_FILE:NotepadAI>")

# Build the list of arguments for windeployqt
set(WINDEPLOYQT_ARGS --no-translations --no-system-d3d-compiler --no-compiler-runtime --no-opengl-sw)
if(PACKAGE_CONFIG STREQUAL "Debug")
	list(APPEND WINDEPLOYQT_ARGS --debug)
endif()
list(APPEND WINDEPLOYQT_ARGS "${PACKAGE_DIR}/NotepadAI.exe")

# The media plugin (add-media-preview) sits next to the exe; windeployqt
# deploys its Qt Multimedia / Pdf modules and the FFmpeg backend with it.
set(MEDIA_PLUGIN_COPY "")
if(TARGET NotepadAIMedia)
	list(APPEND WINDEPLOYQT_ARGS "${PACKAGE_DIR}/NotepadAIMedia.dll")
	set(MEDIA_PLUGIN_COPY COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:NotepadAIMedia>" "${PACKAGE_DIR}/NotepadAIMedia.dll")
endif()

file(GLOB EXTRA_DLLS "${EXTRA_DLL_DIR}/*.dll")

# Define the package target
add_custom_target(package
	COMMENT "Packaging NotepadAI for distribution"
	VERBATIM

	# Copy executable
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${TARGET_EXE}"
		"${PACKAGE_DIR}/NotepadAI.exe"

	# Copy LICENSE
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/LICENSE"
		"${PACKAGE_DIR}/LICENSE"

	# Copy the two extra DLLs
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/deploy/windows/libcrypto-1_1-x64.dll"
		"${PACKAGE_DIR}/libcrypto-1_1-x64.dll"

	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/deploy/windows/libssl-1_1-x64.dll"
		"${PACKAGE_DIR}/libssl-1_1-x64.dll"

	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/deploy/windows/WebView2Loader.dll"
		"${PACKAGE_DIR}/WebView2Loader.dll"

	${MEDIA_PLUGIN_COPY}

	# Run windeployqt with correct flags
	COMMAND windeployqt ${WINDEPLOYQT_ARGS}
)

set(ZIP_FILE "${CMAKE_BINARY_DIR}/NotepadAI-v${PROJECT_VERSION}.zip")
add_custom_target(zip
	DEPENDS package
	COMMENT "Creating zip archive of NotepadAI package"
	VERBATIM
	COMMAND 7z a -tzip
		"${ZIP_FILE}"
		"${PACKAGE_DIR}/*"
		-x!libcrypto-1_1-x64.dll
		-x!libssl-1_1-x64.dll
)

set(NSIS_SCRIPT "${CMAKE_SOURCE_DIR}/installer/installer.nsi")
add_custom_target(installer
	DEPENDS package
	COMMENT "Building NSIS installer for NotepadAI"
	VERBATIM
	COMMAND makensis /V4 "${NSIS_SCRIPT}"
)
