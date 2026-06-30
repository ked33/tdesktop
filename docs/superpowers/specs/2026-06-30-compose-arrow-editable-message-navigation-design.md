# Compose Arrow Editable Message Navigation

## Goal

Change message compose arrow-key behavior so editable outgoing message
history can be reused without unexpectedly scrolling the chat list or
entering edit mode.

## Behavior

Plain Up/Down navigation is active only when the compose field is empty and
not editing, or after this feature has already filled the field from an
editable message and the cursor is at the relevant text boundary. Up continues
editable-message navigation only from the start of the filled text, and Down
continues it only from the end. Other cursor positions stay with the compose
field so multiline cursor movement keeps working. Up selects an older editable
message, Down selects a newer editable message. The selected message text is
written into the compose field without entering edit mode. At either end of
the editable-message list the selection stays where it is; the key is consumed
and the chat list is not scrolled.

Alt+Shift+Up/Down uses the same editable-message ordering, but opens the
selected message in edit mode and keeps edit mode active while moving through
older or newer candidates. At the ends it also stays on the current candidate
and does not scroll the chat. Alt+Up/Down is intentionally not used because it
conflicts more often with platform, menu, and input-method navigation.

Ctrl+Up/Down keeps the existing reply-target navigation behavior.

## Implementation

The implementation stays on the existing compose input path:

- `ComposeControls` detects plain and Alt arrow requests and emits a typed
  editable-message navigation request.
- `ListWidget` owns candidate lookup, reusing the current editable predicate:
  `allowsEdit(now) && !isUploading()`, plus the existing grouped-message
  `findItemToEdit()` mapping.
- Chat, scheduled, and shortcut-message section widgets connect the request to
  `ListWidget`, then either write `PrepareEditText(item)` into
  `ComposeControls` or route edit-mode navigation through the existing
  `ListWidget::editMessageRequestNotify()` path.
- The legacy main-chat `HistoryWidget` key path mirrors the same behavior
  directly, because it still has its own empty-field Up handling that otherwise
  enters edit mode through `HistoryWidget::editMessage()`.
- PR #248's `Alt+Shift+Up/Down` shortcut choice is adopted because it avoids
  the `Alt+Up/Down` conflict, but the implementation remains integrated with
  the existing `ComposeControls` request flow and only mirrors the behavior in
  `HistoryWidget` for the legacy path.

The feature avoids global shortcuts and does not alter message-list keyboard
handling outside compose input requests.

## Validation

Local native build is unavailable in this checkout. Validation is static:

- inspect the touched call chain,
- run source syntax/build-light checks available locally,
- run `git diff --check`,
- verify touched text files remain CRLF-only and UTF-8 without BOM,
- commit and push to `ked33/merge` for GitHub Actions.
