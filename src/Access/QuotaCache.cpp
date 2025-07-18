#include <Access/EnabledQuota.h>
#include <Access/Quota.h>
#include <Access/QuotaCache.h>
#include <Access/QuotaUsage.h>
#include <Access/AccessControl.h>
#include <Common/Exception.h>
#include <base/range.h>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <boost/range/algorithm/lower_bound.hpp>
#include <boost/range/algorithm/stable_sort.hpp>
#include <boost/smart_ptr/make_shared.hpp>


namespace DB
{
namespace ErrorCodes
{
    extern const int QUOTA_REQUIRES_CLIENT_KEY;
    extern const int LOGICAL_ERROR;
}


void QuotaCache::QuotaInfo::setQuota(const QuotaPtr & quota_, const UUID & quota_id_)
{
    quota = quota_;
    quota_id = quota_id_;
    roles = &quota->to_roles;
    rebuildAllIntervals();
}


String QuotaCache::QuotaInfo::calculateKey(const EnabledQuota & enabled, bool throw_if_client_key_empty) const
{
    const auto & params = enabled.params;
    switch (quota->key_type)
    {
        case QuotaKeyType::NONE:
        {
            return "";
        }
        case QuotaKeyType::USER_NAME:
        {
            return params.user_name;
        }
        case QuotaKeyType::IP_ADDRESS:
        {
            return params.client_address.toString();
        }
        case QuotaKeyType::FORWARDED_IP_ADDRESS:
        {
            return params.forwarded_address;
        }
        case QuotaKeyType::CLIENT_KEY:
        {
            if (!params.client_key.empty())
                return params.client_key;

            if (throw_if_client_key_empty)
                throw Exception(
                    ErrorCodes::QUOTA_REQUIRES_CLIENT_KEY,
                    "Quota {} (for user {}) requires a client supplied key.",
                    quota->getName(),
                    params.user_name);
            return ""; // Authentication quota has no client key at time of authentication.
        }
        case QuotaKeyType::CLIENT_KEY_OR_USER_NAME:
        {
            if (!params.client_key.empty())
                return params.client_key;
            return params.user_name;
        }
        case QuotaKeyType::CLIENT_KEY_OR_IP_ADDRESS:
        {
            if (!params.client_key.empty())
                return params.client_key;
            return params.client_address.toString();
        }
        case QuotaKeyType::MAX: break;
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected quota key type: {}", static_cast<int>(quota->key_type));
}


boost::shared_ptr<const EnabledQuota::Intervals> QuotaCache::QuotaInfo::getOrBuildIntervals(const String & key)
{
    auto it = key_to_intervals.find(key);
    if (it != key_to_intervals.end())
        return it->second;
    return rebuildIntervals(key, std::chrono::system_clock::now());
}


void QuotaCache::QuotaInfo::rebuildAllIntervals()
{
    if (key_to_intervals.empty())
        return;
    auto current_time = std::chrono::system_clock::now();
    for (const String & key : key_to_intervals | boost::adaptors::map_keys)
        rebuildIntervals(key, current_time);
}


boost::shared_ptr<const EnabledQuota::Intervals> QuotaCache::QuotaInfo::rebuildIntervals(const String & key, std::chrono::system_clock::time_point current_time)
{
    auto new_intervals = boost::make_shared<Intervals>();
    new_intervals->quota_name = quota->getName();
    new_intervals->quota_id = quota_id;
    new_intervals->quota_key = key;
    auto & intervals = new_intervals->intervals;
    intervals.reserve(quota->all_limits.size());
    for (const auto & limits : quota->all_limits)
    {
        intervals.emplace_back(limits.duration, limits.randomize_interval, current_time);
        auto & interval = intervals.back();
        for (auto quota_type : collections::range(QuotaType::MAX))
        {
            auto quota_type_i = static_cast<size_t>(quota_type);
            if (limits.max[quota_type_i])
                interval.max[quota_type_i] = *limits.max[quota_type_i];
            interval.used[quota_type_i] = 0;
        }
    }

    /// Order intervals by durations from largest to smallest.
    /// To report first about largest interval on what quota was exceeded.
    struct GreaterByDuration
    {
        bool operator()(const Interval & lhs, const Interval & rhs) const { return lhs.duration > rhs.duration; }
    };
    boost::range::stable_sort(intervals, GreaterByDuration{});

    auto it = key_to_intervals.find(key);
    if (it == key_to_intervals.end())
    {
        /// Just put new intervals into the map.
        key_to_intervals.try_emplace(key, new_intervals);
    }
    else
    {
        /// We need to keep usage information from the old intervals.
        const auto & old_intervals = it->second->intervals;
        for (auto & new_interval : new_intervals->intervals)
        {
            /// Check if an interval with the same duration is already in use.
            auto lower_bound = boost::range::lower_bound(old_intervals, new_interval, GreaterByDuration{});
            if ((lower_bound == old_intervals.end()) || (lower_bound->duration != new_interval.duration))
                continue;

            /// Found an interval with the same duration, we need to copy its usage information to `result`.
            const auto & current_interval = *lower_bound;
            for (auto quota_type : collections::range(QuotaType::MAX))
            {
                auto quota_type_i = static_cast<size_t>(quota_type);
                new_interval.used[quota_type_i].store(current_interval.used[quota_type_i].load());
                new_interval.end_of_interval.store(current_interval.end_of_interval.load());
            }
        }
        it->second = new_intervals;
    }

    return new_intervals;
}


QuotaCache::QuotaCache(const AccessControl & access_control_)
    : access_control(access_control_)
{
}

QuotaCache::~QuotaCache() = default;


std::shared_ptr<const EnabledQuota> QuotaCache::getEnabledQuota(
    const UUID & user_id,
    const String & user_name,
    const boost::container::flat_set<UUID> & enabled_roles,
    const std::shared_ptr<Poco::Net::IPAddress> & client_address,
    const String & forwarded_address,
    const String & client_key,
    bool throw_if_client_key_empty)
{
    std::lock_guard lock{mutex};
    ensureAllQuotasRead();

    EnabledQuota::Params params;
    params.user_id = user_id;
    params.user_name = user_name;
    params.enabled_roles = enabled_roles;
    params.client_address = *client_address;
    params.forwarded_address = forwarded_address;
    params.client_key = client_key;
    auto it = enabled_quotas.find(params);
    if (it != enabled_quotas.end())
    {
        auto from_cache = it->second.lock();
        if (from_cache)
            return from_cache;
        enabled_quotas.erase(it);
    }

    auto res = std::shared_ptr<EnabledQuota>(new EnabledQuota(params));
    enabled_quotas.emplace(std::move(params), res);
    chooseQuotaToConsumeFor(*res, throw_if_client_key_empty);
    return res;
}

void QuotaCache::ensureAllQuotasRead()
{
    /// `mutex` is already locked.
    if (all_quotas_read)
        return;
    all_quotas_read = true;

    subscription = access_control.subscribeForChanges<Quota>(
        [&](const UUID & id, const AccessEntityPtr & entity)
        {
            if (entity)
                quotaAddedOrChanged(id, typeid_cast<QuotaPtr>(entity));
            else
                quotaRemoved(id);
        });

    for (const UUID & quota_id : access_control.findAll<Quota>())
    {
        auto quota = access_control.tryRead<Quota>(quota_id);
        if (quota)
            all_quotas.emplace(quota_id, QuotaInfo(quota, quota_id));
    }
}


void QuotaCache::quotaAddedOrChanged(const UUID & quota_id, const std::shared_ptr<const Quota> & new_quota)
{
    std::lock_guard lock{mutex};
    auto it = all_quotas.find(quota_id);
    if (it == all_quotas.end())
    {
        it = all_quotas.emplace(quota_id, QuotaInfo(new_quota, quota_id)).first;
    }
    else
    {
        if (it->second.quota == new_quota)
            return;
    }

    auto & info = it->second;
    info.setQuota(new_quota, quota_id);
    chooseQuotaToConsume();
}


void QuotaCache::quotaRemoved(const UUID & quota_id)
{
    std::lock_guard lock{mutex};
    all_quotas.erase(quota_id);
    chooseQuotaToConsume();
}


void QuotaCache::chooseQuotaToConsume()
{
    /// `mutex` is already locked.

    for (auto i = enabled_quotas.begin(), e = enabled_quotas.end(); i != e;)
    {
        auto elem = i->second.lock();
        if (!elem)
            i = enabled_quotas.erase(i);
        else
        {
            chooseQuotaToConsumeFor(*elem, true);
            ++i;
        }
    }
}

void QuotaCache::chooseQuotaToConsumeFor(EnabledQuota & enabled, bool throw_if_client_key_empty)
{
    /// `mutex` is already locked.
    boost::shared_ptr<const Intervals> intervals;
    for (auto & info : all_quotas | boost::adaptors::map_values)
    {
        if (info.roles->match(enabled.params.user_id, enabled.params.enabled_roles))
        {
            String key = info.calculateKey(enabled, throw_if_client_key_empty);
            intervals = info.getOrBuildIntervals(key);
            break;
        }
    }

    if (!intervals)
    {
        enabled.empty = true;
        enabled.intervals = boost::make_shared<Intervals>(); /// No quota == no limits.
    }
    else
    {
        enabled.intervals.store(intervals);
        enabled.empty = false;
    }
}


std::vector<QuotaUsage> QuotaCache::getAllQuotasUsage() const
{
    std::lock_guard lock{mutex};
    std::vector<QuotaUsage> all_usage;
    auto current_time = std::chrono::system_clock::now();
    for (const auto & info : all_quotas | boost::adaptors::map_values)
    {
        for (const auto & intervals : info.key_to_intervals | boost::adaptors::map_values)
        {
            auto usage = intervals->getUsage(current_time);
            if (usage)
                all_usage.push_back(std::move(usage).value());
        }
    }
    return all_usage;
}

}
