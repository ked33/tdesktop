#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {
void OfferSettingsRecovery();
void ExportConfiguration(not_null<Window::SessionController*> controller);
void ImportConfiguration(not_null<Window::SessionController*> controller);
} // namespace Settings
