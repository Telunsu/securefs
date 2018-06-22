#pragma once

#include "myutils.h"
#include "platform.h"

#include <cryptopp/secblock.h>
#include <string>

namespace securefs
{
int commands_main(int argc, const char* const* argv);

struct FSConfig
{
    CryptoPP::AlignedSecByteBlock master_key;
    unsigned block_size;
    unsigned iv_size;
    unsigned version;
};

class CommandBase
{
    DISABLE_COPY_MOVE(CommandBase)

protected:
    static std::shared_ptr<FileStream> open_config_stream(const std::string& full_path, int flags);
    static FSConfig read_config(FileStream*, const void* password, size_t pass_len);
    static void write_config(FileStream*,
                             const std::string& pbdkf_algorithm,
                             const FSConfig&,
                             const void* password,
                             size_t pass_len,
                             unsigned rounds);

public:
    CommandBase() {}
    virtual ~CommandBase() {}

    virtual const char* long_name() const noexcept = 0;
    virtual char short_name() const noexcept = 0;
    virtual const char* help_message() const noexcept = 0;

    virtual void parse_cmdline(int argc, const char* const* argv) = 0;
    virtual int execute() = 0;
};
}    // namespace securefs
