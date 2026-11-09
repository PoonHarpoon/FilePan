// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The natives of app.phsp.filepan.NativeHost, and Platform implemented by calling back into it.
//
// Kotlin -> native: each call forwards one event to AppHost, which posts it to the UI or render
// thread (surfaceDestroyed and redraw block until the render thread is done).
// Native -> Kotlin: Platform methods run on the native UI thread, which is attached to the VM
// once for its whole life. The Kotlin methods they call return quickly, posting to the main
// thread where they need it. apkIcon and the document provider are the exception: they run on I/O
// pool threads, each attached on its first call and detached when it exits.

#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "filepan/app/app.hpp"
#include "filepan/runtime/app_host.hpp"
#include "filepan/text/font_provider.hpp"
#include "filepan/unique_fd.hpp"
#include "document_provider.hpp"
#include "security_keys.hpp"
#include "jni_util.hpp"

namespace filepan::jni {

namespace {

constexpr char kNativeHostClass[] = "app/phsp/filepan/NativeHost";

// Android's KeyEvent.META_* bits.
constexpr jint kMetaShiftOn = 0x1;
constexpr jint kMetaAltOn = 0x2;
constexpr jint kMetaCtrlOn = 0x1000;
constexpr jint kMetaMetaOn = 0x10000;

JavaVM *gVm = nullptr;
thread_local JNIEnv *tEnv = nullptr;

struct Methods {
    jmethodID hasAllFilesAccess;
    jmethodID requestAllFilesAccess;
    jmethodID storageVolumes;
    jmethodID openFile;
    jmethodID shareFiles;
    jmethodID answerDocumentRequest;
    jmethodID setClipboardText;
    jmethodID clipboardText;
    jmethodID showIme;
    jmethodID hideIme;
    jmethodID updateIme;
    jmethodID updateSemantics;
    jmethodID announce;
    jmethodID jobsActive;
    jmethodID updateJobNotification;
    jmethodID performHaptic;
    jmethodID finishActivity;
    jmethodID openUrl;
    jmethodID isNetworkMetered;
    jmethodID hasNotificationPermission;
    jmethodID requestNotificationPermission;
    jmethodID openNotificationSettings;
    jmethodID setLightSystemBars;
    jmethodID protectSecret;
    jmethodID unprotectSecret;
    jmethodID pickDocument;
    jmethodID saveDocument;
    jmethodID apkIcon;
    jmethodID documentTrees;
    jmethodID securityKeys;
    jmethodID openDocumentTree;
    jmethodID releaseDocumentTree;
} gMethods;
jclass gStringClass = nullptr;

// A String[] of strings; null (with the exception cleared) if it couldn't be made.
jobjectArray newStringArray(JNIEnv *env, const std::vector<std::string> &strings, const char *where) {
    jobjectArray array = env->NewObjectArray(static_cast<jsize>(strings.size()), gStringClass, nullptr);
    if (array == nullptr) {
        clearException(env, where);
        return nullptr;
    }
    for (std::size_t i = 0; i < strings.size(); ++i) {
        jstring s = newString(env, strings[i]);
        env->SetObjectArrayElement(array, static_cast<jsize>(i), s);
        env->DeleteLocalRef(s);
    }
    return array;
}

// Reads the little-endian records Kotlin writes for storageVolumes().
class Reader {
public:
    explicit Reader(const std::string &bytes) : bytes_(bytes) {}
    template <typename T>
    T get() {
        T value{};
        if (at_ + sizeof(T) <= bytes_.size()) {
            std::memcpy(&value, bytes_.data() + at_, sizeof(T));
        }
        at_ += sizeof(T);
        return value;
    }
    std::string str() {
        const auto length = get<std::uint32_t>();
        if (at_ + length > bytes_.size()) {
            at_ = bytes_.size() + 1;
            return {};
        }
        std::string out = bytes_.substr(at_, length);
        at_ += length;
        return out;
    }
    [[nodiscard]] bool ok() const { return at_ <= bytes_.size(); }

private:
    const std::string &bytes_;
    std::size_t at_ = 0;
};

// A native thread other than the UI one (an I/O pool worker), attached to the VM on its first
// call into Kotlin and detached when it exits. As a daemon, so it never holds up the VM.
class WorkerAttachment {
public:
    WorkerAttachment() {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "FilePan I/O", nullptr};
        if (gVm->AttachCurrentThreadAsDaemon(&env_, &args) != JNI_OK) {
            __android_log_print(ANDROID_LOG_ERROR, "FilePan", "Couldn't attach an I/O thread");
            env_ = nullptr;
        }
    }
    ~WorkerAttachment() {
        if (env_ != nullptr) {
            gVm->DetachCurrentThread();
        }
    }
    WorkerAttachment(const WorkerAttachment &) = delete;
    WorkerAttachment &operator=(const WorkerAttachment &) = delete;
    [[nodiscard]] JNIEnv *env() const { return env_; }

private:
    JNIEnv *env_ = nullptr;
};

}  // namespace

JNIEnv *currentEnv() {
    JNIEnv *env = nullptr;
    if (gVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    thread_local WorkerAttachment attachment;
    return attachment.env();
}

namespace {

class JniPlatform final : public runtime::Platform {
public:
    explicit JniPlatform(jobject host) : host_(host) {}

    // On the native UI thread.
    static void attach() {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "FilePan UI", nullptr};
        if (gVm->AttachCurrentThread(&tEnv, &args) != JNI_OK) {
            __android_log_print(ANDROID_LOG_ERROR, "FilePan", "Couldn't attach the UI thread");
            tEnv = nullptr;
        }
    }
    static void detach() {
        if (tEnv != nullptr) {
            gVm->DetachCurrentThread();
            tEnv = nullptr;
        }
    }

    bool hasAllFilesAccess() override {
        JNIEnv *env = tEnv;
        const jboolean granted = env->CallBooleanMethod(host_, gMethods.hasAllFilesAccess);
        return !clearException(env, "hasAllFilesAccess") && granted;
    }

    void requestAllFilesAccess() override { callVoid("requestAllFilesAccess", gMethods.requestAllFilesAccess); }

    std::vector<runtime::StorageVolume> storageVolumes() override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        auto array = static_cast<jbyteArray>(env->CallObjectMethod(host_, gMethods.storageVolumes));
        if (clearException(env, "storageVolumes") || array == nullptr) {
            return {};
        }
        const std::string bytes = bytesToString(env, array);
        Reader in(bytes);
        std::vector<runtime::StorageVolume> volumes(in.get<std::uint32_t>());
        for (auto &volume : volumes) {
            volume.path = in.str();
            volume.description = in.str();
            volume.primary = in.get<std::uint8_t>() != 0;
            volume.removable = in.get<std::uint8_t>() != 0;
            volume.state = in.str();
            volume.totalBytes = in.get<std::uint64_t>();
            volume.freeBytes = in.get<std::uint64_t>();
            if (!in.ok()) {
                return {};
            }
        }
        return volumes;
    }

    void openFile(const std::string &path, const std::string &mimeType) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        env->CallVoidMethod(host_, gMethods.openFile, newString(env, path), newString(env, mimeType));
        clearException(env, "openFile");
    }

    void shareFiles(const std::vector<std::string> &paths,
                    const std::vector<std::string> &mimeTypes) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env, static_cast<jint>(paths.size() + mimeTypes.size() + 4));
        jobjectArray jPaths = newStringArray(env, paths, "shareFiles");
        jobjectArray jMimes = newStringArray(env, mimeTypes, "shareFiles");
        if (jPaths == nullptr || jMimes == nullptr) {
            return;
        }
        env->CallVoidMethod(host_, gMethods.shareFiles, jPaths, jMimes);
        clearException(env, "shareFiles");
    }

    void answerDocumentRequest(const std::vector<std::string> &paths, const std::vector<std::string> &mimeTypes,
                               bool writable) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env, static_cast<jint>(paths.size() + mimeTypes.size() + 4));
        jobjectArray jPaths = newStringArray(env, paths, "answerDocumentRequest");
        jobjectArray jMimes = newStringArray(env, mimeTypes, "answerDocumentRequest");
        if (jPaths == nullptr || jMimes == nullptr) {
            return;
        }
        env->CallVoidMethod(host_, gMethods.answerDocumentRequest, jPaths, jMimes, writable ? JNI_TRUE : JNI_FALSE);
        clearException(env, "answerDocumentRequest");
    }

    void setClipboardText(const std::string &text) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        env->CallVoidMethod(host_, gMethods.setClipboardText, newString(env, text));
        clearException(env, "setClipboardText");
    }

    std::optional<std::string> clipboardText() override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        auto result = static_cast<jbyteArray>(env->CallObjectMethod(host_, gMethods.clipboardText));
        if (clearException(env, "clipboardText") || result == nullptr) {
            return std::nullopt;
        }
        return bytesToString(env, result);
    }

    void showIme(gfx::Rect caret, const runtime::TextInputState &state) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        const auto &value = state.value;
        const auto &config = state.config;
        const jint flags = config.multiline ? 1 : 0;
        env->CallVoidMethod(host_, gMethods.showIme, static_cast<jint>(caret.x),
                            static_cast<jint>(caret.y), static_cast<jint>(caret.x + caret.width),
                            static_cast<jint>(caret.y + caret.height), textBytes(env, value.text),
                            value.selection.start, value.selection.end, value.composing.start,
                            value.composing.end, static_cast<jint>(config.keyboard),
                            static_cast<jint>(config.action), flags, static_cast<jlong>(state.session));
        clearException(env, "showIme");
    }

    void hideIme() override { callVoid("hideIme", gMethods.hideIme); }

    void updateIme(const runtime::TextInputState &state) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        const auto &value = state.value;
        env->CallVoidMethod(host_, gMethods.updateIme, textBytes(env, value.text),
                            value.selection.start, value.selection.end, value.composing.start,
                            value.composing.end, static_cast<jlong>(state.session));
        clearException(env, "updateIme");
    }

    void updateSemantics(std::span<const std::uint8_t> bytes) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        env->CallVoidMethod(host_, gMethods.updateSemantics, newByteArray(env, bytes));
        clearException(env, "updateSemantics");
    }

    void announce(const std::string &text) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        env->CallVoidMethod(host_, gMethods.announce, newString(env, text));
        clearException(env, "announce");
    }

    void jobsActive(bool active) override {
        JNIEnv *env = tEnv;
        env->CallVoidMethod(host_, gMethods.jobsActive, static_cast<jboolean>(active));
        clearException(env, "jobsActive");
    }

    void updateJobNotification(const runtime::JobSummary &summary) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        env->CallVoidMethod(host_, gMethods.updateJobNotification, newString(env, summary.title),
                            newString(env, summary.text), summary.progressPermille);
        clearException(env, "updateJobNotification");
    }

    void performHaptic(runtime::HapticKind kind) override {
        JNIEnv *env = tEnv;
        env->CallVoidMethod(host_, gMethods.performHaptic, static_cast<jint>(kind));
        clearException(env, "performHaptic");
    }

    void finishActivity() override { callVoid("finishActivity", gMethods.finishActivity); }

    bool hasNotificationPermission() override {
        JNIEnv *env = tEnv;
        const jboolean granted = env->CallBooleanMethod(host_, gMethods.hasNotificationPermission);
        return !clearException(env, "hasNotificationPermission") && granted;
    }

    void requestNotificationPermission() override {
        callVoid("requestNotificationPermission", gMethods.requestNotificationPermission);
    }

    void openNotificationSettings() override { callVoid("openNotificationSettings", gMethods.openNotificationSettings); }

    void setLightSystemBars(bool light) override {
        JNIEnv *env = tEnv;
        env->CallVoidMethod(host_, gMethods.setLightSystemBars, static_cast<jboolean>(light));
        clearException(env, "setLightSystemBars");
    }

    bool isNetworkMetered() override {
        JNIEnv *env = tEnv;
        const jboolean metered = env->CallBooleanMethod(host_, gMethods.isNetworkMetered);
        return !clearException(env, "isNetworkMetered") && metered;
    }

    void openUrl(const std::string &url) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        env->CallVoidMethod(host_, gMethods.openUrl, newString(env, url));
        clearException(env, "openUrl");
    }

    std::optional<std::vector<std::uint8_t>> protectSecret(std::span<const std::uint8_t> plain) override {
        return callBytes("protectSecret", gMethods.protectSecret, plain);
    }

    std::optional<std::vector<std::uint8_t>> unprotectSecret(std::span<const std::uint8_t> blob) override {
        return callBytes("unprotectSecret", gMethods.unprotectSecret, blob);
    }

    bool pickDocument(std::uint64_t request, const runtime::DocumentPickOptions &options) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env, static_cast<jint>(options.mimeTypes.size() + 4));
        jobjectArray mimeTypes =
                env->NewObjectArray(static_cast<jsize>(options.mimeTypes.size()), gStringClass, nullptr);
        if (mimeTypes == nullptr) {
            clearException(env, "pickDocument");
            return false;
        }
        for (std::size_t i = 0; i < options.mimeTypes.size(); ++i) {
            jstring s = newString(env, options.mimeTypes[i]);
            env->SetObjectArrayElement(mimeTypes, static_cast<jsize>(i), s);
            env->DeleteLocalRef(s);
        }
        const auto maxBytes = static_cast<jint>(std::min<std::size_t>(options.maxBytes, 0x7FFFFFFF));
        env->CallVoidMethod(host_, gMethods.pickDocument, static_cast<jlong>(request), mimeTypes, maxBytes);
        return !clearException(env, "pickDocument");
    }

    bool saveDocument(std::uint64_t request, const runtime::DocumentSaveOptions &options) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        jbyteArray bytes = newByteArray(
                env, {reinterpret_cast<const std::uint8_t *>(options.bytes.data()), options.bytes.size()});
        if (bytes == nullptr) {
            clearException(env, "saveDocument");
            return false;
        }
        env->CallVoidMethod(host_, gMethods.saveDocument, static_cast<jlong>(request), newString(env, options.name),
                            newString(env, options.mimeType), bytes);
        return !clearException(env, "saveDocument");
    }

    // On an I/O pool thread, not the UI one: see currentEnv.
    std::optional<std::vector<std::uint8_t>> apkIcon(const std::string &path, int sizePx) override {
        JNIEnv *env = currentEnv();
        if (env == nullptr || sizePx <= 0) {
            return std::nullopt;
        }
        // The thread has no Java frame to free local references with.
        LocalFrame frame(env);
        auto result = static_cast<jbyteArray>(
                env->CallObjectMethod(host_, gMethods.apkIcon, newString(env, path), static_cast<jint>(sizePx)));
        if (clearException(env, "apkIcon") || result == nullptr) {
            return std::nullopt;
        }
        const jsize length = env->GetArrayLength(result);
        if (static_cast<std::int64_t>(length) != static_cast<std::int64_t>(sizePx) * sizePx * 4) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(length));
        env->GetByteArrayRegion(result, 0, length, reinterpret_cast<jbyte *>(pixels.data()));
        return pixels;
    }

    std::shared_ptr<vfs::DocumentProvider> documentProvider() override { return documents_; }
    std::shared_ptr<vfs::SecurityKeySource> securityKeySource() override { return securityKeys_; }

    bool openDocumentTree(std::uint64_t request) override {
        JNIEnv *env = tEnv;
        env->CallVoidMethod(host_, gMethods.openDocumentTree, static_cast<jlong>(request));
        return !clearException(env, "openDocumentTree");
    }

    void releaseDocumentTree(const std::string &uri) override {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        env->CallVoidMethod(host_, gMethods.releaseDocumentTree, newString(env, uri));
        clearException(env, "releaseDocumentTree");
    }

    void setDocumentProvider(std::shared_ptr<vfs::DocumentProvider> documents) { documents_ = std::move(documents); }
    void setSecurityKeySource(std::shared_ptr<vfs::SecurityKeySource> keys) { securityKeys_ = std::move(keys); }

private:
    // A byte[] -> byte[]? method; nullopt for null or an exception.
    std::optional<std::vector<std::uint8_t>> callBytes(const char *name, jmethodID method,
                                                       std::span<const std::uint8_t> bytes) {
        JNIEnv *env = tEnv;
        LocalFrame frame(env);
        auto result = static_cast<jbyteArray>(env->CallObjectMethod(host_, method, newByteArray(env, bytes)));
        if (clearException(env, name) || result == nullptr) {
            return std::nullopt;
        }
        const std::string out = bytesToString(env, result);
        return std::vector<std::uint8_t>(out.begin(), out.end());
    }

    void callVoid(const char *name, jmethodID method) {
        JNIEnv *env = tEnv;
        env->CallVoidMethod(host_, method);
        clearException(env, name);
    }

    static jbyteArray textBytes(JNIEnv *env, const std::string &text) {
        return newByteArray(env, {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
    }

    jobject host_;  // A global reference, kept for the process's life.
    std::shared_ptr<vfs::DocumentProvider> documents_;
    std::shared_ptr<vfs::SecurityKeySource> securityKeys_;
};

// What a NativeHost's handle points to. Never freed: it lives as long as the process.
struct Native {
    jobject host = nullptr;          // Global reference.
    jobject assetManager = nullptr;  // Global reference, keeping the AAssetManager valid.
    std::unique_ptr<JniPlatform> platform;
    std::unique_ptr<runtime::AppHost> app;
};

runtime::AppHost &app(jlong handle) { return *reinterpret_cast<Native *>(handle)->app; }

jlong nativeCreate(JNIEnv *env, jobject self, jobject assets, jstring homePath, jstring cacheDir,
                   jstring filesDir, jstring locale, jstring version, jstring build) {
    auto native = std::make_unique<Native>();
    native->host = env->NewGlobalRef(self);
    native->assetManager = env->NewGlobalRef(assets);
    native->platform = std::make_unique<JniPlatform>(native->host);
    {
        LocalFrame frame(env);
        jobject trees = env->CallObjectMethod(self, gMethods.documentTrees);
        if (!clearException(env, "documentTrees")) {
            native->platform->setDocumentProvider(createDocumentProvider(env, trees));
        }
        jobject keys = env->CallObjectMethod(self, gMethods.securityKeys);
        if (!clearException(env, "securityKeys")) {
            native->platform->setSecurityKeySource(createSecurityKeySource(env, keys));
        }
    }

    runtime::AppHostConfig config;
    config.fonts = text::createAndroidFontProvider(AAssetManager_fromJava(env, native->assetManager));
    config.locale = toUtf8(env, locale);
    config.homePath = toUtf8(env, homePath);
    config.cacheDir = toUtf8(env, cacheDir);
    config.filesDir = toUtf8(env, filesDir);
    config.assets = runtime::createAndroidAssetSource(AAssetManager_fromJava(env, native->assetManager));
    config.version = toUtf8(env, version);
    config.build = toUtf8(env, build);
    config.factory = app::createApp;
    config.onUiThreadStart = &JniPlatform::attach;
    config.onUiThreadExit = &JniPlatform::detach;
    auto created = runtime::AppHost::create(*native->platform, std::move(config));
    if (!created) {
        __android_log_print(ANDROID_LOG_ERROR, "FilePan", "%s", created.error().c_str());
        env->DeleteGlobalRef(native->host);
        env->DeleteGlobalRef(native->assetManager);
        return 0;
    }
    native->app = std::move(*created);
    return reinterpret_cast<jlong>(native.release());
}

void nativeSurfaceCreated(JNIEnv *env, jobject, jlong handle, jobject surface) {
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        return;
    }
    const int width = ANativeWindow_getWidth(window);
    const int height = ANativeWindow_getHeight(window);
    app(handle).surfaceCreated(
            {
                    .create = [window](gfx::vulkan::Renderer &renderer) {
                        return renderer.createSurfaceTarget(window);
                    },
                    .release = [window] { ANativeWindow_release(window); },
            },
            width, height);
}

void nativeSurfaceChanged(JNIEnv *, jobject, jlong handle, jint width, jint height) {
    app(handle).surfaceChanged(width, height);
}

void nativeSurfaceDestroyed(JNIEnv *, jobject, jlong handle) { app(handle).surfaceDestroyed(); }

void nativeRedraw(JNIEnv *, jobject, jlong handle) { app(handle).redraw(); }

// Per sample: ints (pointer id, action, kind), coords (x, y), times (ns).
void nativePointer(JNIEnv *env, jobject, jlong handle, jint count, jintArray ints,
                   jfloatArray coords, jlongArray times) {
    if (count <= 0) {
        return;
    }
    std::vector<jint> i(static_cast<std::size_t>(count) * 3);
    std::vector<jfloat> c(static_cast<std::size_t>(count) * 2);
    std::vector<jlong> t(static_cast<std::size_t>(count));
    env->GetIntArrayRegion(ints, 0, count * 3, i.data());
    env->GetFloatArrayRegion(coords, 0, count * 2, c.data());
    env->GetLongArrayRegion(times, 0, count, t.data());
    if (clearException(env, "nativePointer")) {
        return;
    }
    std::vector<ui::PointerEvent> events(static_cast<std::size_t>(count));
    for (std::size_t k = 0; k < events.size(); ++k) {
        events[k].pointer = i[k * 3];
        events[k].action = static_cast<ui::PointerAction>(i[k * 3 + 1]);
        events[k].kind = static_cast<ui::PointerKind>(i[k * 3 + 2]);
        events[k].position = {c[k * 2], c[k * 2 + 1]};
        events[k].timeNs = t[k];
    }
    app(handle).pointerEvents(std::move(events));
}

void nativeKey(JNIEnv *, jobject, jlong handle, jint keyCode, jint action, jint metaState,
               jint repeat, jint codePoint, jlong timeNs) {
    std::uint32_t modifiers = 0;
    if (metaState & kMetaShiftOn) modifiers |= ui::modifiers::kShift;
    if (metaState & kMetaCtrlOn) modifiers |= ui::modifiers::kCtrl;
    if (metaState & kMetaAltOn) modifiers |= ui::modifiers::kAlt;
    if (metaState & kMetaMetaOn) modifiers |= ui::modifiers::kMeta;
    app(handle).keyEvent({.keyCode = keyCode,
                          .action = action == 0 ? ui::KeyAction::down : ui::KeyAction::up,
                          .modifiers = modifiers,
                          .repeat = repeat,
                          .codePoint = static_cast<std::uint32_t>(codePoint),
                          .timeNs = timeNs});
}

void nativeBack(JNIEnv *, jobject, jlong handle) { app(handle).back(); }

// density, fontScale, safe area (l, t, r, b), IME insets (l, t, r, b), reducedMotion, powerSave,
// night.
void nativeEnvironment(JNIEnv *env, jobject, jlong handle, jfloatArray values) {
    jfloat v[13] = {};
    if (env->GetArrayLength(values) < 13) {
        return;
    }
    env->GetFloatArrayRegion(values, 0, 13, v);
    app(handle).setEnvironment({.density = v[0],
                                .fontScale = v[1],
                                .safeArea = {v[2], v[3], v[4], v[5]},
                                .imeInsets = {v[6], v[7], v[8], v[9]},
                                .reducedMotion = v[10] != 0,
                                .powerSave = v[11] != 0,
                                .night = v[12] != 0});
}

void nativeResumed(JNIEnv *, jobject, jlong handle, jboolean resumed) {
    app(handle).setResumed(resumed);
}

void nativeNetwork(JNIEnv *, jobject, jlong handle, jboolean metered) { app(handle).setNetwork(metered); }

void nativeImeValue(JNIEnv *env, jobject, jlong handle, jbyteArray text, jint selectionStart,
                    jint selectionEnd, jint composingStart, jint composingEnd, jlong session) {
    app(handle).imeEditingValue({.text = bytesToString(env, text),
                                 .selection = {selectionStart, selectionEnd},
                                 .composing = {composingStart, composingEnd}},
                                static_cast<std::uint64_t>(session));
}

void nativeImeAction(JNIEnv *, jobject, jlong handle, jint action, jlong session) {
    app(handle).imeAction(static_cast<ui::ImeAction>(action), static_cast<std::uint64_t>(session));
}

void nativeAccessibilityEnabled(JNIEnv *, jobject, jlong handle, jboolean enabled) {
    app(handle).setAccessibilityEnabled(enabled);
}

void nativeRequestFullSemantics(JNIEnv *, jobject, jlong handle) { app(handle).requestFullSemantics(); }

void nativeAccessibilityAction(JNIEnv *env, jobject, jlong handle, jint nodeId, jint action,
                               jint customId, jbyteArray text, jint selectionStart,
                               jint selectionEnd) {
    app(handle).accessibilityAction(static_cast<std::uint32_t>(nodeId),
                                    static_cast<ui::Action>(static_cast<std::uint32_t>(action)),
                                    {.text = bytesToString(env, text),
                                     .customId = static_cast<std::uint32_t>(customId),
                                     .selectionStart = selectionStart,
                                     .selectionEnd = selectionEnd});
}

// From any thread: PickedDocument::Status, the display name (or null) and the bytes (or null).
void nativeDocumentPicked(JNIEnv *env, jobject, jlong handle, jlong request, jint status, jstring name,
                          jbyteArray bytes) {
    using Status = runtime::PickedDocument::Status;
    const Status known = status >= 0 && status <= static_cast<jint>(Status::tooLarge) ? static_cast<Status>(status)
                                                                                     : Status::failed;
    app(handle).documentPicked(static_cast<std::uint64_t>(request),
                               {.status = known, .name = toUtf8(env, name), .bytes = bytesToString(env, bytes)});
}

void nativeDocumentSaved(JNIEnv *env, jobject, jlong handle, jlong request, jint status, jstring name) {
    using Status = runtime::SavedDocument::Status;
    const Status known = status >= 0 && status <= static_cast<jint>(Status::failed) ? static_cast<Status>(status)
                                                                                   : Status::failed;
    app(handle).documentSaved(static_cast<std::uint64_t>(request), {.status = known, .name = toUtf8(env, name)});
}

// From any thread: PickedTree::Status, and the tree's URI and display name (or null).
void nativeDocumentTreePicked(JNIEnv *env, jobject, jlong handle, jlong request, jint status, jstring uri,
                              jstring name) {
    using Status = runtime::PickedTree::Status;
    const Status known = status >= 0 && status <= static_cast<jint>(Status::failed) ? static_cast<Status>(status)
                                                                                   : Status::failed;
    app(handle).documentTreePicked(static_cast<std::uint64_t>(request),
                                   {.status = known, .uri = toUtf8(env, uri), .name = toUtf8(env, name)});
}

// From any thread: what another app shared. fds are open documents, which native code now owns,
// with their names and sizes (-1 for unknown); or, with no fds, text saved as textName.
void nativeShareReceived(JNIEnv *env, jobject, jlong handle, jintArray fds, jobjectArray names, jlongArray sizes,
                         jbyteArray text, jstring textName, jboolean fromOtherApp) {
    runtime::SharedItems share;
    share.fromOtherApp = fromOtherApp == JNI_TRUE;
    const jsize count = fds != nullptr ? env->GetArrayLength(fds) : 0;
    std::vector<jint> descriptors(static_cast<std::size_t>(count));
    std::vector<jlong> lengths(static_cast<std::size_t>(count), -1);
    if (count > 0) {
        env->GetIntArrayRegion(fds, 0, count, descriptors.data());
        if (sizes != nullptr && env->GetArrayLength(sizes) >= count) {
            env->GetLongArrayRegion(sizes, 0, count, lengths.data());
        }
    }
    for (jsize i = 0; i < count; ++i) {
        jobs::ImportItem item;
        item.fd = UniqueFd(descriptors[static_cast<std::size_t>(i)]);
        if (names != nullptr && i < env->GetArrayLength(names)) {
            auto name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
            item.name = toUtf8(env, name);
            env->DeleteLocalRef(name);
        }
        if (const jlong size = lengths[static_cast<std::size_t>(i)]; size >= 0) {
            item.size = static_cast<std::uint64_t>(size);
        }
        share.items.push_back(std::move(item));
    }
    if (count == 0 && text != nullptr) {
        share.items.push_back({.name = toUtf8(env, textName), .text = bytesToString(env, text)});
    }
    if (!share.items.empty()) {
        app(handle).shareReceived(std::move(share));
    }
}

// From the main thread: another app asking for a document (kind is runtime::DocumentRequest::Kind),
// or going away without an answer.
void nativeDocumentRequested(JNIEnv *env, jobject, jlong handle, jint kind, jstring name, jobjectArray mimeTypes,
                             jboolean multiple) {
    using Kind = runtime::DocumentRequest::Kind;
    const Kind known = kind >= 0 && kind <= static_cast<jint>(Kind::ended) ? static_cast<Kind>(kind) : Kind::ended;
    runtime::DocumentRequest request{.kind = known, .name = toUtf8(env, name), .multiple = multiple == JNI_TRUE};
    const jsize count = mimeTypes != nullptr ? env->GetArrayLength(mimeTypes) : 0;
    for (jsize i = 0; i < count; ++i) {
        auto type = static_cast<jstring>(env->GetObjectArrayElement(mimeTypes, i));
        request.mimeTypes.push_back(toUtf8(env, type));
        env->DeleteLocalRef(type);
    }
    app(handle).documentRequested(std::move(request));
}

#define FILEPAN_NATIVE(name, signature) \
    JNINativeMethod { #name, signature, reinterpret_cast<void *>(name) }

const JNINativeMethod kNativeMethods[] = {
        FILEPAN_NATIVE(nativeCreate,
                       "(Landroid/content/res/AssetManager;Ljava/lang/String;Ljava/lang/String;"
                       "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J"),
        FILEPAN_NATIVE(nativeSurfaceCreated, "(JLandroid/view/Surface;)V"),
        FILEPAN_NATIVE(nativeSurfaceChanged, "(JII)V"),
        FILEPAN_NATIVE(nativeSurfaceDestroyed, "(J)V"),
        FILEPAN_NATIVE(nativeRedraw, "(J)V"),
        FILEPAN_NATIVE(nativePointer, "(JI[I[F[J)V"),
        FILEPAN_NATIVE(nativeKey, "(JIIIIIJ)V"),
        FILEPAN_NATIVE(nativeBack, "(J)V"),
        FILEPAN_NATIVE(nativeEnvironment, "(J[F)V"),
        FILEPAN_NATIVE(nativeResumed, "(JZ)V"),
        FILEPAN_NATIVE(nativeNetwork, "(JZ)V"),
        FILEPAN_NATIVE(nativeImeValue, "(J[BIIIIJ)V"),
        FILEPAN_NATIVE(nativeImeAction, "(JIJ)V"),
        FILEPAN_NATIVE(nativeAccessibilityEnabled, "(JZ)V"),
        FILEPAN_NATIVE(nativeRequestFullSemantics, "(J)V"),
        FILEPAN_NATIVE(nativeAccessibilityAction, "(JIII[BII)V"),
        FILEPAN_NATIVE(nativeDocumentPicked, "(JJILjava/lang/String;[B)V"),
        FILEPAN_NATIVE(nativeDocumentSaved, "(JJILjava/lang/String;)V"),
        FILEPAN_NATIVE(nativeDocumentTreePicked, "(JJILjava/lang/String;Ljava/lang/String;)V"),
        FILEPAN_NATIVE(nativeShareReceived, "(J[I[Ljava/lang/String;[J[BLjava/lang/String;Z)V"),
        FILEPAN_NATIVE(nativeDocumentRequested, "(JILjava/lang/String;[Ljava/lang/String;Z)V"),
};

#undef FILEPAN_NATIVE

}  // namespace

jint registerNativeHost(JavaVM *vm, JNIEnv *env) {
    gVm = vm;
    jclass stringClass = env->FindClass("java/lang/String");
    jclass host = env->FindClass(kNativeHostClass);
    if (stringClass == nullptr || host == nullptr) {
        clearException(env, "FindClass");
        return JNI_ERR;
    }
    gStringClass = static_cast<jclass>(env->NewGlobalRef(stringClass));
    struct Lookup {
        jmethodID *id;
        const char *name;
        const char *signature;
    };
    const Lookup lookups[] = {
            {&gMethods.hasAllFilesAccess, "hasAllFilesAccess", "()Z"},
            {&gMethods.requestAllFilesAccess, "requestAllFilesAccess", "()V"},
            {&gMethods.storageVolumes, "storageVolumes", "()[B"},
            {&gMethods.openFile, "openFile", "(Ljava/lang/String;Ljava/lang/String;)V"},
            {&gMethods.shareFiles, "shareFiles", "([Ljava/lang/String;[Ljava/lang/String;)V"},
            {&gMethods.answerDocumentRequest, "answerDocumentRequest", "([Ljava/lang/String;[Ljava/lang/String;Z)V"},
            {&gMethods.setClipboardText, "setClipboardText", "(Ljava/lang/String;)V"},
            {&gMethods.clipboardText, "clipboardText", "()[B"},
            {&gMethods.showIme, "showIme", "(IIII[BIIIIIIIJ)V"},
            {&gMethods.hideIme, "hideIme", "()V"},
            {&gMethods.updateIme, "updateIme", "([BIIIIJ)V"},
            {&gMethods.updateSemantics, "updateSemantics", "([B)V"},
            {&gMethods.announce, "announce", "(Ljava/lang/String;)V"},
            {&gMethods.jobsActive, "jobsActive", "(Z)V"},
            {&gMethods.updateJobNotification, "updateJobNotification",
             "(Ljava/lang/String;Ljava/lang/String;I)V"},
            {&gMethods.performHaptic, "performHaptic", "(I)V"},
            {&gMethods.finishActivity, "finishActivity", "()V"},
            {&gMethods.openUrl, "openUrl", "(Ljava/lang/String;)V"},
            {&gMethods.isNetworkMetered, "isNetworkMetered", "()Z"},
            {&gMethods.hasNotificationPermission, "hasNotificationPermission", "()Z"},
            {&gMethods.requestNotificationPermission, "requestNotificationPermission", "()V"},
            {&gMethods.openNotificationSettings, "openNotificationSettings", "()V"},
            {&gMethods.setLightSystemBars, "setLightSystemBars", "(Z)V"},
            {&gMethods.protectSecret, "protectSecret", "([B)[B"},
            {&gMethods.unprotectSecret, "unprotectSecret", "([B)[B"},
            {&gMethods.pickDocument, "pickDocument", "(J[Ljava/lang/String;I)V"},
            {&gMethods.saveDocument, "saveDocument", "(JLjava/lang/String;Ljava/lang/String;[B)V"},
            {&gMethods.apkIcon, "apkIcon", "(Ljava/lang/String;I)[B"},
            {&gMethods.documentTrees, "getDocumentTrees", "()Lapp/phsp/filepan/DocumentTrees;"},
            {&gMethods.securityKeys, "getSecurityKeys", "()Lapp/phsp/filepan/SecurityKeys;"},
            {&gMethods.openDocumentTree, "openDocumentTree", "(J)V"},
            {&gMethods.releaseDocumentTree, "releaseDocumentTree", "(Ljava/lang/String;)V"},
    };
    for (const auto &lookup : lookups) {
        *lookup.id = env->GetMethodID(host, lookup.name, lookup.signature);
        if (*lookup.id == nullptr) {
            clearException(env, lookup.name);
            return JNI_ERR;
        }
    }
    if (env->RegisterNatives(host, kNativeMethods, std::size(kNativeMethods)) != JNI_OK) {
        clearException(env, "RegisterNatives");
        return JNI_ERR;
    }
    env->DeleteLocalRef(host);
    env->DeleteLocalRef(stringClass);
    return JNI_OK;
}

}  // namespace filepan::jni
