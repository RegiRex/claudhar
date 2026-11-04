// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.
// Modified by the Claudhar Project in 2026: Azahar's per-application settings ported onto 2126.2.

package org.citra.citra_emu.features.settings.model

interface AbstractSetting<T> {
    val key: String
    val section: String
    val defaultValue: T
    val isRuntimeEditable: Boolean

    fun valueToString(value: T): String = value.toString()

    fun valueFromString(string: String): T?

    fun defaultValueToString(): String = valueToString(defaultValue)
}
