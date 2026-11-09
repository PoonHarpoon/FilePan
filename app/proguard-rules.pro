# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# JNI: keep native method names and the types in their signatures.
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# Classes the native code looks up by name and calls back into by method name and signature
# (jni/native_host.cpp, jni/document_provider.cpp, jni/security_keys.cpp).
-keep class app.phsp.filepan.NativeHost {
    <methods>;
}
-keep class app.phsp.filepan.DocumentTrees {
    <methods>;
}
-keep class app.phsp.filepan.SecurityKeys {
    <methods>;
}
