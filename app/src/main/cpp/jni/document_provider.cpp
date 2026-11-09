// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "document_provider.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jni_util.hpp"

namespace filepan::jni {

namespace {

// Reads the records DocumentTrees writes: per document str id, str name, u8 directory, u64 size,
// i64 modified ms, u32 flags; strings are a u32 length and UTF-8, all little-endian.
class RecordReader {
public:
    explicit RecordReader(const std::string &bytes) : bytes_(bytes) {}

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

    vfs::DocumentInfo document() {
        vfs::DocumentInfo info;
        info.id = str();
        info.name = str();
        info.directory = get<std::uint8_t>() != 0;
        info.size = get<std::uint64_t>();
        info.modifiedMs = get<std::int64_t>();
        info.flags = get<std::uint32_t>();
        return info;
    }

    [[nodiscard]] bool ok() const { return at_ <= bytes_.size(); }

private:
    const std::string &bytes_;
    std::size_t at_ = 0;
};

class JniDocumentProvider final : public vfs::DocumentProvider {
public:
    JniDocumentProvider(JNIEnv *env, jobject trees) : trees_(env->NewGlobalRef(trees)) {
        jclass type = env->GetObjectClass(trees);
        root_ = env->GetMethodID(type, "root", "(Ljava/lang/String;)[B");
        children_ = env->GetMethodID(type, "children", "(Ljava/lang/String;Ljava/lang/String;)[B");
        create_ = env->GetMethodID(type, "create", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)[B");
        rename_ = env->GetMethodID(type, "rename", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        move_ = env->GetMethodID(type, "move",
                                 "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)"
                                 "Ljava/lang/String;");
        remove_ = env->GetMethodID(type, "remove", "(Ljava/lang/String;Ljava/lang/String;)V");
        open_ = env->GetMethodID(type, "open", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I");
        notFound_ = globalClass(env, "java/io/FileNotFoundException");
        security_ = globalClass(env, "java/lang/SecurityException");
        unsupported_ = globalClass(env, "java/lang/UnsupportedOperationException");
        env->DeleteLocalRef(type);
        clearException(env, "DocumentTrees");
    }

    [[nodiscard]] bool valid() const {
        return root_ && children_ && create_ && rename_ && move_ && remove_ && open_ && notFound_ && security_ &&
               unsupported_;
    }

    Result<vfs::DocumentInfo> root(std::string_view tree) override {
        return call<vfs::DocumentInfo>([&](JNIEnv *env) -> Result<vfs::DocumentInfo> {
            auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(trees_, root_, newString(env, tree)));
            if (auto failed = thrown(env)) {
                return std::unexpected(*failed);
            }
            return parseOne(env, bytes);
        });
    }

    Result<std::vector<vfs::DocumentInfo>> children(std::string_view tree, std::string_view documentId) override {
        return call<std::vector<vfs::DocumentInfo>>([&](JNIEnv *env) -> Result<std::vector<vfs::DocumentInfo>> {
            auto bytes = static_cast<jbyteArray>(
                    env->CallObjectMethod(trees_, children_, newString(env, tree), newString(env, documentId)));
            if (auto failed = thrown(env)) {
                return std::unexpected(*failed);
            }
            const std::string data = bytesToString(env, bytes);
            RecordReader reader(data);
            std::vector<vfs::DocumentInfo> out(reader.get<std::uint32_t>());
            for (auto &info : out) {
                if (!reader.ok()) {
                    break;
                }
                info = reader.document();
            }
            if (!reader.ok()) {
                return fail(EIO, "Bad listing");
            }
            return out;
        });
    }

    Result<vfs::DocumentInfo> create(std::string_view tree, std::string_view parentId, std::string_view name,
                                     bool directory) override {
        return call<vfs::DocumentInfo>([&](JNIEnv *env) -> Result<vfs::DocumentInfo> {
            auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(trees_, create_, newString(env, tree),
                                                                       newString(env, parentId),
                                                                       newString(env, name), directory));
            if (auto failed = thrown(env)) {
                return std::unexpected(*failed);
            }
            return parseOne(env, bytes);
        });
    }

    Result<std::string> rename(std::string_view tree, std::string_view documentId, std::string_view name) override {
        return call<std::string>([&](JNIEnv *env) -> Result<std::string> {
            auto id = static_cast<jstring>(env->CallObjectMethod(trees_, rename_, newString(env, tree),
                                                                 newString(env, documentId), newString(env, name)));
            if (auto failed = thrown(env)) {
                return std::unexpected(*failed);
            }
            return toUtf8(env, id);
        });
    }

    Result<std::string> move(std::string_view tree, std::string_view documentId, std::string_view fromParentId,
                             std::string_view toParentId) override {
        return call<std::string>([&](JNIEnv *env) -> Result<std::string> {
            auto id = static_cast<jstring>(env->CallObjectMethod(trees_, move_, newString(env, tree),
                                                                 newString(env, documentId),
                                                                 newString(env, fromParentId),
                                                                 newString(env, toParentId)));
            if (auto failed = thrown(env)) {
                return std::unexpected(*failed);
            }
            return toUtf8(env, id);
        });
    }

    Result<void> remove(std::string_view tree, std::string_view documentId) override {
        return call<void>([&](JNIEnv *env) -> Result<void> {
            env->CallVoidMethod(trees_, remove_, newString(env, tree), newString(env, documentId));
            if (auto failed = thrown(env)) {
                return std::unexpected(*failed);
            }
            return {};
        });
    }

    Result<int> open(std::string_view tree, std::string_view documentId, std::string_view mode) override {
        return call<int>([&](JNIEnv *env) -> Result<int> {
            const jint fd = env->CallIntMethod(trees_, open_, newString(env, tree), newString(env, documentId),
                                               newString(env, mode));
            if (auto failed = thrown(env)) {
                return std::unexpected(*failed);
            }
            if (fd < 0) {
                return fail(EIO);
            }
            return fd;
        });
    }

private:
    static jclass globalClass(JNIEnv *env, const char *name) {
        jclass local = env->FindClass(name);
        if (local == nullptr) {
            return nullptr;
        }
        auto global = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        return global;
    }

    // Runs f with this thread's env inside a local frame: I/O threads have no Java frame to free
    // local references with.
    template <typename T, typename F>
    Result<T> call(F &&f) {
        JNIEnv *env = currentEnv();
        if (env == nullptr) {
            return fail(EIO, "No JNI environment");
        }
        LocalFrame frame(env);
        return f(env);
    }

    // The pending exception as an errno, cleared; nullopt if there's none.
    std::optional<Error> thrown(JNIEnv *env) {
        jthrowable exception = env->ExceptionOccurred();
        if (exception == nullptr) {
            return std::nullopt;
        }
        env->ExceptionClear();
        const int code = env->IsInstanceOf(exception, notFound_)      ? ENOENT
                         : env->IsInstanceOf(exception, security_)    ? EACCES
                         : env->IsInstanceOf(exception, unsupported_) ? ENOTSUP
                                                                      : EIO;
        std::string message;
        jclass throwable = env->GetObjectClass(exception);
        if (jmethodID getMessage = env->GetMethodID(throwable, "getMessage", "()Ljava/lang/String;")) {
            message = toUtf8(env, static_cast<jstring>(env->CallObjectMethod(exception, getMessage)));
        }
        env->ExceptionClear();
        return Error{code, std::move(message)};
    }

    static Result<vfs::DocumentInfo> parseOne(JNIEnv *env, jbyteArray bytes) {
        if (bytes == nullptr) {
            return fail(ENOENT);
        }
        const std::string data = bytesToString(env, bytes);
        RecordReader reader(data);
        vfs::DocumentInfo info = reader.document();
        if (!reader.ok()) {
            return fail(EIO, "Bad document");
        }
        return info;
    }

    jobject trees_;  // A global reference, kept for the process's life.
    jmethodID root_ = nullptr;
    jmethodID children_ = nullptr;
    jmethodID create_ = nullptr;
    jmethodID rename_ = nullptr;
    jmethodID move_ = nullptr;
    jmethodID remove_ = nullptr;
    jmethodID open_ = nullptr;
    jclass notFound_ = nullptr;
    jclass security_ = nullptr;
    jclass unsupported_ = nullptr;
};

}  // namespace

std::shared_ptr<vfs::DocumentProvider> createDocumentProvider(JNIEnv *env, jobject trees) {
    if (trees == nullptr) {
        return nullptr;
    }
    auto provider = std::make_shared<JniDocumentProvider>(env, trees);
    return provider->valid() ? provider : nullptr;
}

}  // namespace filepan::jni
