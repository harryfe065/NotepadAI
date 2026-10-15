# Blender probe for test_embedded_window_win32 (blender* tests): logs every
# non-mouse-move event Blender's window manager sees, with its modifier flags,
# to the file named by NOTEPADAI_BLENDER_LOG. Run with:
#   blender --factory-startup --python embed_blender_log_events.py
import os

import bpy

LOG = os.environ["NOTEPADAI_BLENDER_LOG"]
SKIP = {"MOUSEMOVE", "INBETWEEN_MOUSEMOVE", "TIMER", "TIMER_REPORT", "TIMERREGION", "NONE"}


def log(line):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


class WM_OT_notepadai_log_events(bpy.types.Operator):
    bl_idname = "wm.notepadai_log_events"
    bl_label = "NotepadAI log events"

    def modal(self, context, event):
        if event.type not in SKIP:
            log(f"{event.type} {event.value} shift={int(event.shift)} ctrl={int(event.ctrl)} alt={int(event.alt)}")
        return {"PASS_THROUGH"}

    def invoke(self, context, event):
        context.window_manager.modal_handler_add(self)
        return {"RUNNING_MODAL"}


def start():
    window = bpy.context.window_manager.windows[0]
    with bpy.context.temp_override(window=window, screen=window.screen):
        bpy.ops.wm.notepadai_log_events("INVOKE_DEFAULT")
    log("READY")
    return None


bpy.utils.register_class(WM_OT_notepadai_log_events)
bpy.app.timers.register(start, first_interval=1.0)
