#include <Core/Settings.h>
#include <DataTypes/ObjectUtils.h>
#include <Storages/MergeTree/MergeTreeIOSettings.h>
#include <Storages/MergeTree/MergeTreeSettings.h>
#include <Storages/StorageInMemoryMetadata.h>

namespace DB
{
namespace Setting
{
    extern const SettingsUInt64 low_cardinality_max_dictionary_size;
    extern const SettingsBool low_cardinality_use_single_dictionary_for_part;
    extern const SettingsUInt64 min_compress_block_size;
    extern const SettingsUInt64 max_compress_block_size;
    extern const SettingsBool merge_tree_use_v1_object_and_dynamic_serialization;
}

namespace MergeTreeSetting
{
    extern const MergeTreeSettingsBool compress_primary_key;
    extern const MergeTreeSettingsBool use_adaptive_write_buffer_for_dynamic_subcolumns;
    extern const MergeTreeSettingsBool use_compact_variant_discriminators_serialization;
    extern const MergeTreeSettingsNonZeroUInt64 marks_compress_block_size;
    extern const MergeTreeSettingsString marks_compression_codec;
    extern const MergeTreeSettingsString primary_key_compression_codec;
    extern const MergeTreeSettingsUInt64 adaptive_write_buffer_initial_size;
    extern const MergeTreeSettingsUInt64 max_compress_block_size;
    extern const MergeTreeSettingsUInt64 min_compress_block_size;
    extern const MergeTreeSettingsNonZeroUInt64 primary_key_compress_block_size;
    extern const MergeTreeSettingsUInt64 text_index_sampling_threshold;
    extern const MergeTreeSettingsFloat text_index_sampling_rate;
}

MergeTreeWriterSettings::MergeTreeWriterSettings(
    const Settings & global_settings,
    const WriteSettings & query_write_settings_,
    const MergeTreeSettingsPtr & storage_settings,
    bool can_use_adaptive_granularity_,
    bool rewrite_primary_key_,
    bool save_marks_in_cache_,
    bool save_primary_index_in_memory_,
    bool blocks_are_granules_size_)
    : min_compress_block_size((*storage_settings)[MergeTreeSetting::min_compress_block_size] ? (*storage_settings)[MergeTreeSetting::min_compress_block_size] : global_settings[Setting::min_compress_block_size])
    , max_compress_block_size((*storage_settings)[MergeTreeSetting::max_compress_block_size] ? (*storage_settings)[MergeTreeSetting::max_compress_block_size] : global_settings[Setting::max_compress_block_size])
    , marks_compression_codec((*storage_settings)[MergeTreeSetting::marks_compression_codec])
    , marks_compress_block_size((*storage_settings)[MergeTreeSetting::marks_compress_block_size])
    , compress_primary_key((*storage_settings)[MergeTreeSetting::compress_primary_key])
    , primary_key_compression_codec((*storage_settings)[MergeTreeSetting::primary_key_compression_codec])
    , primary_key_compress_block_size((*storage_settings)[MergeTreeSetting::primary_key_compress_block_size])
    , can_use_adaptive_granularity(can_use_adaptive_granularity_)
    , rewrite_primary_key(rewrite_primary_key_)
    , save_marks_in_cache(save_marks_in_cache_)
    , save_primary_index_in_memory(save_primary_index_in_memory_)
    , blocks_are_granules_size(blocks_are_granules_size_)
    , query_write_settings(query_write_settings_)
    , low_cardinality_max_dictionary_size(global_settings[Setting::low_cardinality_max_dictionary_size])
    , low_cardinality_use_single_dictionary_for_part(global_settings[Setting::low_cardinality_use_single_dictionary_for_part] != 0)
    , use_compact_variant_discriminators_serialization((*storage_settings)[MergeTreeSetting::use_compact_variant_discriminators_serialization])
    , use_v1_object_and_dynamic_serialization(global_settings[Setting::merge_tree_use_v1_object_and_dynamic_serialization])
    , use_adaptive_write_buffer_for_dynamic_subcolumns((*storage_settings)[MergeTreeSetting::use_adaptive_write_buffer_for_dynamic_subcolumns])
    , adaptive_write_buffer_initial_size((*storage_settings)[MergeTreeSetting::adaptive_write_buffer_initial_size])
    , text_index_sampling_threshold((*storage_settings)[MergeTreeSetting::text_index_sampling_threshold])
    , text_index_sampling_rate((*storage_settings)[MergeTreeSetting::text_index_sampling_rate])
{
}

}
