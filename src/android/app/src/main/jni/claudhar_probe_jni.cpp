// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// JNI entry for Claudhar's Phase 0 device probe: NativeLibrary.runDeviceProbe() in
// Kotlin. It returns the native report (CPU, then Vulkan); the Kotlin side adds the
// device identity, displays and input devices it can read from Android directly.

#include <jni.h>
#include "claudhar/probe/probe.h"

extern "C" JNIEXPORT jstring JNICALL
Java_org_citra_citra_1emu_NativeLibrary_runDeviceProbe(JNIEnv* env, [[maybe_unused]] jobject obj) {
    const Claudhar::Probe::Options options{};
    const std::string report =
        Claudhar::Probe::ReportCpu(options) + Claudhar::Probe::ReportVulkan(options);
    return env->NewStringUTF(report.c_str());
}
