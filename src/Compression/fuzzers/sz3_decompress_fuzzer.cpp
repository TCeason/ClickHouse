#include <iostream>
#include <string>

#include <Compression/CompressedReadBuffer.h>
#include <Compression/ICompressionCodec.h>
#include <IO/BufferWithOwnMemory.h>
#include <IO/ReadBufferFromMemory.h>
#include <Interpreters/Context.h>
#include <base/types.h>
#include <Common/Arena.h>
#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/MemoryTracker.h>

using namespace DB;
ContextMutablePtr context;
extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
    if (context)
        return true;

    static SharedContextHolder shared_context = Context::createShared();
    context = Context::createGlobal(shared_context.get());
    context->makeGlobalContext();

    MainThreadStatus::getInstance();

    return 0;
}

namespace DB
{
CompressionCodecPtr getCompressionCodecSZ3(UInt8 gcd_bytes_size);
}

struct AuxiliaryRandomData
{
    UInt8 float_size_bytes;
    size_t decompressed_size;
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size)
try
{
    total_memory_tracker.resetCounters();
    total_memory_tracker.setHardLimit(1_GiB);
    CurrentThread::get().memory_tracker.resetCounters();
    CurrentThread::get().memory_tracker.setHardLimit(1_GiB);

    if (size < sizeof(AuxiliaryRandomData))
        return 0;

    const auto * p = reinterpret_cast<const AuxiliaryRandomData *>(data);
    auto codec = DB::getCompressionCodecSZ3(p->float_size_bytes % 2 == 0 ? 4 : 8);

    size_t output_buffer_size = p->decompressed_size % 65536;
    size -= sizeof(AuxiliaryRandomData);
    data += sizeof(AuxiliaryRandomData) / sizeof(uint8_t);

    // std::string input = std::string(reinterpret_cast<const char*>(data), size);
    // fmt::print(stderr, "Using input {} of size {}, output size is {}. \n", input, size, output_buffer_size);

    DB::Memory<> memory;
    memory.resize(output_buffer_size + codec->getAdditionalSizeAtTheEndOfBuffer());

    codec->doDecompressData(
        reinterpret_cast<const char *>(data), static_cast<UInt32>(size), memory.data(), static_cast<UInt32>(output_buffer_size));

    return 0;
}
catch (...)
{
    return 1;
}
