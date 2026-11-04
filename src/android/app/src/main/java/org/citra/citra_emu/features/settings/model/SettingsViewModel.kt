// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.
// Modified by the Claudhar Project in 2026: Azahar's per-application settings ported onto 2126.2.

package org.citra.citra_emu.features.settings.model

import androidx.lifecycle.ViewModel

class SettingsViewModel : ViewModel() {
    /** a local copy of settings that is used while editing, but not currently active */
    val editingSettings = CitraSettings()
}
