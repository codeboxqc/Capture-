## 2024-06-05 - Auto-hide interface on recording
**Learning:** Screen recording applications should prioritize minimizing their own interface footprint when starting a capture. Users often record their full screen and capturing the application's UI before manually minimizing it leads to unwanted frames.
**Action:** Always consider the target application domain when determining the primary interaction state. For screen recorders, the application should intuitively transition out of the way to a system tray or minimized state automatically when capturing starts to provide a clean recording start.
## 2024-06-05 - Tray icon integration
**Learning:** Screen recording applications should provide a way to interact with the application when it's minimized (like a tray icon). Also, tooltips with bullet points help improve the usability.
**Action:** Adding system tray interaction allows users to restore the minimized window easily when they want to stop recording or access settings.
