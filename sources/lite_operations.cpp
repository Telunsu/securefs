#include "lite_operations.h"
#include "lite_fs.h"
#include "lite_stream.h"
#include "logger.h"
#include "myutils.h"
#include "operations.h"
#include "platform.h"

#include <securefs_config.h>

#include <math.h>

#if !HAS_THREAD_LOCAL
#include <pthread.h>
#endif

namespace securefs
{
namespace lite
{


    struct BundledContext
    {
        ::securefs::operations::MountOptions* opt;
#if !HAS_THREAD_LOCAL
        ::pthread_key_t key;
#endif
    };

#if HAS_THREAD_LOCAL
    static thread_local optional<FileSystem> opt_fs;
#endif

    FileSystem* get_local_filesystem(void)
    {
#if HAS_THREAD_LOCAL
        if (opt_fs)
            return &(*opt_fs);
#endif

        auto ctx = static_cast<BundledContext*>(fuse_get_context()->private_data);

#if !HAS_THREAD_LOCAL
        auto fs = static_cast<FileSystem*>(::pthread_getspecific(ctx->key));
        if (fs)
            return fs;
#endif

        if (ctx->opt->version.value() != 4)
            throwInvalidArgumentException("This function only supports filesystem format 4");

        key_type name_key, content_key, xattr_key;
        if (ctx->opt->master_key.size() != 3 * KEY_LENGTH)
            throwInvalidArgumentException("Master key has wrong length");

        memcpy(name_key.data(), ctx->opt->master_key.data(), KEY_LENGTH);
        memcpy(content_key.data(), ctx->opt->master_key.data() + KEY_LENGTH, KEY_LENGTH);
        memcpy(xattr_key.data(), ctx->opt->master_key.data() + 2 * KEY_LENGTH, KEY_LENGTH);

        warn_if_key_not_random(name_key, __FILE__, __LINE__);
        warn_if_key_not_random(content_key, __FILE__, __LINE__);
        warn_if_key_not_random(xattr_key, __FILE__, __LINE__);

#if HAS_THREAD_LOCAL
        opt_fs.emplace(ctx->opt->root,
                       name_key,
                       content_key,
                       xattr_key,
                       ctx->opt->block_size.value(),
                       ctx->opt->iv_size.value(),
                       ctx->opt->flags.value());
        return &(*opt_fs);
#else
        std::unique_ptr<FileSystem> guard(new FileSystem(ctx->opt->root,
                                                         name_key,
                                                         content_key,
                                                         xattr_key,
                                                         ctx->opt->block_size.value(),
                                                         ctx->opt->iv_size.value(),
                                                         ctx->opt->flags.value()));
        int rc = ::pthread_setspecific(ctx->key, guard.get());
        if (rc)
            THROW_POSIX_EXCEPTION(rc, "pthread_setspecific");
        return guard.release();
#endif
    }

#define SINGLE_COMMON_PROLOGUE                                                                     \
    auto filesystem = get_local_filesystem();                                                      \
    OPT_TRACE_WITH_PATH;

#define SINGLE_COMMON_EPILOGUE OPT_CATCH_WITH_PATH

    void* init(struct fuse_conn_info* fsinfo)
    {
        //OperationLogger logger("operations-" + std::string(__FUNCTION__));
#ifdef FUSE_CAP_BIG_WRITES
        fsinfo->want |= FUSE_CAP_BIG_WRITES;
        fsinfo->max_write = static_cast<unsigned>(-1);
#endif
#ifdef FSP_FUSE_CAP_READDIR_PLUS
        fsinfo->want |= (fsinfo->capable & FSP_FUSE_CAP_READDIR_PLUS);
#endif
        void* args = fuse_get_context()->private_data;
        INFO_LOG("init");
        auto ctx = new BundledContext;
        ctx->opt = static_cast<operations::MountOptions*>(args);

#if !HAS_THREAD_LOCAL
        int rc = ::pthread_key_create(&ctx->key,
                                      [](void* p) { delete static_cast<lite::FileSystem*>(p); });
        if (rc)
            THROW_POSIX_EXCEPTION(rc, "pthread_key_create");
#endif
        return ctx;
    }

    void destroy(void*)
    {
        //OperationLogger logger("operations-" + std::string(__FUNCTION__));
        delete static_cast<BundledContext*>(fuse_get_context()->private_data);
        INFO_LOG("destroy");
    }

    int statfs(const char* path, struct fuse_statvfs* buf)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            if (!buf)
                return -EFAULT;
            filesystem->statvfs(buf);
            // Due to the Base32 encoding and the extra 16 bytes of synthesized IV
            buf->f_namemax = buf->f_namemax * 5 / 8 - 16;
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int getattr(const char* path, struct fuse_stat* st)
    {
        //OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            if (!filesystem->stat(path, st))
                return -ENOENT;
            TRACE_LOG("stat (%s): mode=0%o, uid=%u, gid=%u, size=%zu",
                      path,
                      st->st_mode,
                      (unsigned)st->st_uid,
                      (unsigned)st->st_gid,
                      (size_t)st->st_size);
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int opendir(const char* path, struct fuse_file_info* info)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            auto traverser = filesystem->create_traverser(path);
            info->fh = reinterpret_cast<uintptr_t>(traverser.release());
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int releasedir(const char* path, struct fuse_file_info* info)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        TRACE_LOG("%s %s", __func__, path);
        try
        {
            delete reinterpret_cast<DirectoryTraverser*>(info->fh);
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int readdir(const char* path,
                void* buf,
                fuse_fill_dir_t filler,
                fuse_off_t,
                struct fuse_file_info* info)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        OPT_TRACE_WITH_PATH;
        try
        {
            auto traverser = reinterpret_cast<DirectoryTraverser*>(info->fh);
            if (!traverser)
                return -EFAULT;
            traverser->rewind();
            std::string name;
            struct fuse_stat stbuf;
            memset(&stbuf, 0, sizeof(stbuf));

            while (traverser->next(&name, &stbuf))
            {
#ifndef _WIN32
                if (name == "." || name == "..")
                {
                    continue;
                }
#endif
                int rc = filler(buf, name.c_str(), &stbuf, 0);
                if (rc != 0)
                    return -abs(rc);
            }
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int create(const char* path, fuse_mode_t mode, struct fuse_file_info* info)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            AutoClosedFile file = filesystem->open(path, O_RDWR | O_CREAT | O_EXCL, mode);
            info->fh = reinterpret_cast<uintptr_t>(file.release());
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int open(const char* path, struct fuse_file_info* info)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            AutoClosedFile file = filesystem->open(path, info->flags, 0644);
            info->fh = reinterpret_cast<uintptr_t>(file.release());
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int release(const char* path, struct fuse_file_info* info)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        TRACE_LOG("%s %s", __func__, path);
        try
        {
            delete reinterpret_cast<File*>(info->fh);
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int
    read(const char* path, char* buf, size_t size, fuse_off_t offset, struct fuse_file_info* info)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        OPT_TRACE_WITH_PATH_OFF_LEN(offset, size);
        auto fp = reinterpret_cast<File*>(info->fh);
        if (!fp)
            return -EFAULT;

        try
        {
            fp->lock(false);
            DEFER(fp->unlock());
            return static_cast<int>(fp->read(buf, offset, size));
        }
        OPT_CATCH_WITH_PATH_OFF_LEN(offset, size)
    }

    int write(const char* path,
              const char* buf,
              size_t size,
              fuse_off_t offset,
              struct fuse_file_info* info)
    {

        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        INFO_LOG("write path=[%s], size=[%d], offset=[%d]", path, size, offset);
        OPT_TRACE_WITH_PATH_OFF_LEN(offset, size);
        auto fp = reinterpret_cast<File*>(info->fh);
        if (!fp)
            return -EFAULT;

        try
        {
            fp->lock();
            DEFER(fp->unlock());
            fp->write(buf, offset, size);
            return static_cast<int>(size);
        }
        OPT_CATCH_WITH_PATH_OFF_LEN(offset, size)
    }

    int flush(const char* path, struct fuse_file_info* info)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        TRACE_LOG("%s %s", __func__, path);
        auto fp = reinterpret_cast<File*>(info->fh);
        if (!fp)
            return -EFAULT;

        try
        {
            fp->lock();
            DEFER(fp->unlock());
            fp->flush();
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int ftruncate(const char* path, fuse_off_t len, struct fuse_file_info* info)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        TRACE_LOG("%s %s with length=%lld", __func__, path, static_cast<long long>(len));
        auto fp = reinterpret_cast<File*>(info->fh);
        if (!fp)
            return -EFAULT;

        try
        {
            fp->lock();
            DEFER(fp->unlock());
            fp->resize(len);
            return 0;
        }
        catch (const std::exception& e)
        {
            auto ebase = dynamic_cast<const ExceptionBase*>(&e);
            auto code = ebase ? ebase->error_number() : EPERM;
            ERROR_LOG("%s %s (length=%lld) encounters exception %s (code=%d): %s",
                      __func__,
                      path,
                      static_cast<long long>(len),
                      get_type_name(e).get(),
                      code,
                      e.what());
            return -code;
        }
    }

    int unlink(const char* path)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            filesystem->unlink(path);
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int mkdir(const char* path, fuse_mode_t mode)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            filesystem->mkdir(path, mode);
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int rmdir(const char* path)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            filesystem->rmdir(path);
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int chmod(const char* path, fuse_mode_t mode)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            filesystem->chmod(path, mode);
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int chown(const char* path, fuse_uid_t uid, fuse_gid_t gid)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            filesystem->chown(path, uid, gid);
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int symlink(const char* to, const char* from)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        auto filesystem = get_local_filesystem();
        OPT_TRACE_WITH_TWO_PATHS(to, from);

        try
        {
            filesystem->symlink(to, from);
            return 0;
        }
        OPT_CATCH_WITH_TWO_PATHS(to, from)
    }

    int link(const char* src, const char* dest)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        auto filesystem = get_local_filesystem();
        OPT_TRACE_WITH_TWO_PATHS(src, dest);

        try
        {
            filesystem->link(src, dest);
            return 0;
        }
        OPT_CATCH_WITH_TWO_PATHS(src, dest)
    }

    int readlink(const char* path, char* buf, size_t size)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            (void)filesystem->readlink(path, buf, size);
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int rename(const char* from, const char* to)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        auto filesystem = get_local_filesystem();
        OPT_TRACE_WITH_TWO_PATHS(from, to);

        try
        {
            filesystem->rename(from, to);
            return 0;
        }
        OPT_CATCH_WITH_TWO_PATHS(from, to)
    }

    int fsync(const char* path, int, struct fuse_file_info* info)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        TRACE_LOG("%s %s", __func__, path);
        auto fp = reinterpret_cast<File*>(info->fh);
        if (!fp)
            return -EFAULT;

        try
        {
            fp->lock();
            DEFER(fp->unlock());
            fp->fsync();
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int truncate(const char* path, fuse_off_t len)
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        if (len < 0)
            return -EINVAL;

        TRACE_LOG("%s %s (len=%lld)", __func__, path, static_cast<long long>(len));
        auto filesystem = get_local_filesystem();

        try
        {
            AutoClosedFile fp = filesystem->open(path, O_RDWR, 0644);
            fp->lock();
            DEFER(fp->unlock());
            fp->resize(static_cast<size_t>(len));
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

    int utimens(const char* path, const struct fuse_timespec ts[2])
    {
        OperationLogger logger("operations-" + std::string(__FUNCTION__));
        SINGLE_COMMON_PROLOGUE
        try
        {
            filesystem->utimens(path, ts);
            return 0;
        }
        SINGLE_COMMON_EPILOGUE
    }

#ifdef __APPLE__
    int listxattr(const char* path, char* list, size_t size)
    {
        auto filesystem = get_local_filesystem();
        return static_cast<int>(filesystem->listxattr(path, list, size));
    }

    int getxattr(const char* path, const char* name, char* value, size_t size, uint32_t position)
    {
        if (position != 0)
            return -EINVAL;
        if (strcmp(name, "com.apple.quarantine") == 0)
            return -ENOATTR;    // workaround for the "XXX is damaged" bug on OS X
        if (strcmp(name, "com.apple.FinderInfo") == 0)
            return -ENOATTR;    // stupid Apple hardcodes the size of xattr values

        auto filesystem = get_local_filesystem();
        return static_cast<int>(filesystem->getxattr(path, name, value, size));
    }

    int setxattr(const char* path,
                 const char* name,
                 const char* value,
                 size_t size,
                 int flags,
                 uint32_t position)
    {
        if (position != 0)
            return -EINVAL;
        if (strcmp(name, "com.apple.quarantine") == 0)
            return 0;    // workaround for the "XXX is damaged" bug on OS X
        if (strcmp(name, "com.apple.FinderInfo") == 0)
            return -EACCES;    // stupid Apple hardcodes the size of xattr values
        if (!value || size == 0)
            return 0;

        auto filesystem = get_local_filesystem();
        return filesystem->setxattr(path, name, const_cast<char*>(value), size, flags);
    }
    int removexattr(const char* path, const char* name)
    {
        auto filesystem = get_local_filesystem();
        return filesystem->removexattr(path, name);
    }
#endif

    void init_fuse_operations(struct fuse_operations* opt, bool xattr)
    {
        memset(opt, 0, sizeof(*opt));

        opt->init = &::securefs::lite::init;
        opt->destroy = &::securefs::lite::destroy;
        opt->statfs = &::securefs::lite::statfs;
        opt->getattr = &::securefs::lite::getattr;
        opt->opendir = &::securefs::lite::opendir;
        opt->releasedir = &::securefs::lite::releasedir;
        opt->readdir = &::securefs::lite::readdir;
        opt->create = &::securefs::lite::create;
        opt->open = &::securefs::lite::open;
        opt->release = &::securefs::lite::release;
        opt->read = &::securefs::lite::read;
        opt->write = &::securefs::lite::write;
        opt->flush = &::securefs::lite::flush;
        opt->truncate = &::securefs::lite::truncate;
        opt->ftruncate = &::securefs::lite::ftruncate;
        opt->unlink = &::securefs::lite::unlink;
        opt->mkdir = &::securefs::lite::mkdir;
        opt->rmdir = &::securefs::lite::rmdir;
        opt->chmod = &::securefs::lite::chmod;
        opt->chown = &::securefs::lite::chown;
        opt->symlink = &::securefs::lite::symlink;
        opt->link = &::securefs::lite::link;
        opt->readlink = &::securefs::lite::readlink;
        opt->rename = &::securefs::lite::rename;
        opt->fsync = &::securefs::lite::fsync;
        opt->utimens = &::securefs::lite::utimens;

        if (!xattr)
            return;

#ifdef __APPLE__
        opt->listxattr = &::securefs::lite::listxattr;
        opt->getxattr = &::securefs::lite::getxattr;
        opt->setxattr = &::securefs::lite::setxattr;
        opt->removexattr = &::securefs::lite::removexattr;
#endif
    }
}    // namespace lite
}    // namespace securefs
