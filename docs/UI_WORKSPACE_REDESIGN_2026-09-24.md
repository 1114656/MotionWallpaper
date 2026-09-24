# Native wallpaper workspace

The development UI now follows the selected light Motion design. The app remains a native WinUI 3 controller using the existing Agent/Renderer and media library.

## Interaction

1. Select all displays or a numbered physical display. The current assignment is shown separately from the preview.
2. Choose a wallpaper in the library. This only changes a local draft.
3. Use the explicit Apply button. A single-display command preserves other assignments; applying to all displays intentionally clears overrides. Disconnected targets reject application rather than falling back to all displays.

Importing continues to queue performance-copy generation according to the selected quality mode, but no longer silently replaces the desktop. Quality is explicitly labeled global. Tray/activity playback preferences retain their shared durable state.

Navigation separates wallpaper selection, display layout/runtime diagnostics, performance-copy tasks, playback/power settings, and library storage/backup operations. Existing group management, filters, favorites, media actions, and import controls remain available from the library pane.

Settings fills the available page width with two equal columns: playback and scenes on the left, screensaver/power and advanced options on the right. Below a 1050-DIP settings viewport it stacks into one column; longer descriptions wrap so controls remain readable. Reflow does not change playback preferences.

## Resource behavior

The workspace uses small gallery thumbnails and one bounded decoded still. It reuses the existing bounded still extraction implementation in a separate 128 MiB workspace cache, with cancellation and media-library leases. It does not start an animated UI video preview. Screen identification uses temporary, nonactivating numbered overlays that close after three seconds.

## Validation

`WallpaperAssignmentTests.h` covers disconnected targets, empty topology, invalid identifiers, single-display scope, duplicate application, all-display override replacement, and preservation of global quality/playback settings. Native UI screenshots and the initial design comparison are retained in `ui-redesign-qa/`. Settings layout verification is recorded below; the broader workspace's final visual acceptance remains pending.

Run `Start-Dev.cmd` to use the development build with persistent `build/Config` and `build/Wallpapers`. No installer is needed.

### Settings layout follow-up

The updated development build passed all 123 native checks with no build warnings or errors. Native-window inspection verified the two-column layout, narrowing to one column, scrolling to the last advanced setting, and expanding back to two columns. An initial resize check caught a stale `ActualWidth` binding; the page now updates its content width directly from `SizeChanged`.

Evidence: `ui-redesign-qa/settings-two-columns.png`, `ui-redesign-qa/settings-narrow.png`, and `ui-redesign-qa/settings-narrow-bottom.png`. This follow-up changed layout only and did not toggle playback or apply any scene or wallpaper.
