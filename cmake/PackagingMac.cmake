message(STATUS "Configuring macOS packaging")

set(INSTALL_DIR ${CMAKE_BINARY_DIR}/install)

# Custom Info.plist
set_target_properties(NotepadAI PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/deploy/macos/info.plist
)

# Application icon
set(APP_ICON_MACOS ${CMAKE_SOURCE_DIR}/icon/NotepadAI.icns)

set_source_files_properties(${APP_ICON_MACOS}
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
)

target_sources(NotepadAI PRIVATE ${APP_ICON_MACOS})

set_target_properties(NotepadAI PROPERTIES
    MACOSX_BUNDLE_ICON_FILE NotepadAI.icns
)

install(FILES ${APP_ICON_MACOS}
    DESTINATION NotepadAI.app/Contents/Resources
)

add_custom_target(install_local
    COMMAND ${CMAKE_COMMAND}
        --install ${CMAKE_BINARY_DIR}
        --prefix ${INSTALL_DIR}
    DEPENDS NotepadAI
)

find_program(MACDEPLOYQT_EXECUTABLE macdeployqt REQUIRED)

# The media plugin (add-media-preview) lives in Contents/PlugIns; macdeployqt
# must see it to bundle Qt Multimedia / Pdf and fix its rpaths.
set(MACDEPLOYQT_MEDIA_ARGS "")
if(TARGET NotepadAIMedia)
    set(MACDEPLOYQT_MEDIA_ARGS -executable=${INSTALL_DIR}/NotepadAI.app/Contents/PlugIns/NotepadAIMedia.so)
endif()

add_custom_target(dmg
    COMMAND ${MACDEPLOYQT_EXECUTABLE}
        ${INSTALL_DIR}/NotepadAI.app
        ${MACDEPLOYQT_MEDIA_ARGS}
        -dmg
    COMMAND ${CMAKE_COMMAND} -E rename
        ${INSTALL_DIR}/NotepadAI.dmg
        ${CMAKE_BINARY_DIR}/NotepadAI-v${PROJECT_VERSION}.dmg
    DEPENDS install_local
)
