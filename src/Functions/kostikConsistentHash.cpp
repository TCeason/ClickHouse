#include <Functions/FunctionsConsistentHashing.h>
#include <Functions/FunctionFactory.h>

#include <consistent_hashing.h>

namespace DB
{

/// An O(1) time and space consistent hash algorithm by Konstantin Oblakov
struct KostikConsistentHashImpl
{
    static constexpr auto name = "kostikConsistentHash";

    using HashType = UInt64;
    /// Actually it supports UInt64, but it is efficient only if n <= 32768
    using ResultType = UInt16;
    using BucketsType = ResultType;
    static constexpr auto max_buckets = 32768;

    static ResultType apply(UInt64 hash, BucketsType n)
    {
        return ConsistentHashing(hash, n);
    }
};

using FunctionKostikConsistentHash = FunctionConsistentHashImpl<KostikConsistentHashImpl>;

REGISTER_FUNCTION(KostikConsistentHash)
{
    factory.registerFunction<FunctionKostikConsistentHash>();
    factory.registerAlias("yandexConsistentHash", "kostikConsistentHash");
}

}
