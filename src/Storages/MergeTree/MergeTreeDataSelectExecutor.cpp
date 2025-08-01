#include <optional>
#include <unordered_set>
#include <boost/rational.hpp> /// For calculations related to sampling coefficients.

#include <Storages/MergeTree/MergeTreeDataSelectExecutor.h>
#include <Storages/MergeTree/MergeTreeReadPool.h>
#include <Storages/MergeTree/MergeTreeIndices.h>
#include <Storages/MergeTree/MergeTreeIndexReader.h>
#include <Storages/MergeTree/MergeTreeSettings.h>
#include <Storages/MergeTree/KeyCondition.h>
#include <Storages/MergeTree/MergeTreeDataPartUUID.h>
#include <Storages/MergeTree/StorageFromMergeTreeDataPart.h>
#include <Storages/MergeTree/MergeTreeIndexGin.h>
#include <Storages/ReadInOrderOptimizer.h>
#include <Storages/VirtualColumnUtils.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTSampleRatio.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/parseIdentifierOrStringLiteral.h>
#include <Interpreters/ExpressionAnalyzer.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/Context.h>
#include <Interpreters/ProcessList.h>
#include <Interpreters/Cache/QueryConditionCache.h>
#include <Processors/ConcatProcessor.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/CreatingSetsStep.h>
#include <Processors/QueryPlan/FilterStep.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/QueryPlan/ReadFromPreparedSource.h>
#include <Processors/QueryPlan/ReadFromMergeTree.h>
#include <Processors/QueryPlan/UnionStep.h>
#include <Processors/QueryPlan/QueryIdHolder.h>
#include <Processors/QueryPlan/AggregatingStep.h>
#include <Processors/QueryPlan/SortingStep.h>
#include <Processors/QueryPlan/Optimizations/actionsDAGUtils.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <Processors/Transforms/AggregatingTransform.h>

#include <Core/Settings.h>
#include <Core/UUID.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeUUID.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionFactory.h>
#include <Functions/IFunction.h>
#include <base/sleep.h>
#include <Common/LoggingFormatStringHelpers.h>
#include <Common/CurrentMetrics.h>
#include <Common/ElapsedTimeProfileEventIncrement.h>
#include <Common/FailPoint.h>
#include <Common/ProfileEvents.h>
#include <Common/quoteString.h>

#include <IO/WriteBufferFromOStream.h>

namespace CurrentMetrics
{
    extern const Metric MergeTreeDataSelectExecutorThreads;
    extern const Metric MergeTreeDataSelectExecutorThreadsActive;
    extern const Metric MergeTreeDataSelectExecutorThreadsScheduled;
    extern const Metric FilteringMarksWithPrimaryKey;
    extern const Metric FilteringMarksWithSecondaryKeys;
}

namespace ProfileEvents
{
extern const Event FilteringMarksWithPrimaryKeyMicroseconds;
extern const Event FilteringMarksWithSecondaryKeysMicroseconds;
extern const Event IndexBinarySearchAlgorithm;
extern const Event IndexGenericExclusionSearchAlgorithm;
}

namespace DB
{
namespace Setting
{
    extern const SettingsBool allow_experimental_query_deduplication;
    extern const SettingsUInt64 allow_experimental_parallel_reading_from_replicas;
    extern const SettingsString force_data_skipping_indices;
    extern const SettingsBool force_index_by_date;
    extern const SettingsSeconds lock_acquire_timeout;
    extern const SettingsInt64 max_partitions_to_read;
    extern const SettingsUInt64 max_threads_for_indexes;
    extern const SettingsNonZeroUInt64 max_parallel_replicas;
    extern const SettingsUInt64 merge_tree_coarse_index_granularity;
    extern const SettingsUInt64 merge_tree_min_bytes_for_seek;
    extern const SettingsUInt64 merge_tree_min_rows_for_seek;
    extern const SettingsUInt64 parallel_replica_offset;
    extern const SettingsUInt64 parallel_replicas_count;
    extern const SettingsParallelReplicasMode parallel_replicas_mode;
    extern const SettingsBool use_skip_indexes_if_final_exact_mode;
    extern const SettingsBool use_query_condition_cache;
    extern const SettingsBool allow_experimental_analyzer;
    extern const SettingsBool parallel_replicas_local_plan;
    extern const SettingsBool parallel_replicas_index_analysis_only_on_coordinator;
    extern const SettingsBool secondary_indices_enable_bulk_filtering;
    extern const SettingsBool vector_search_with_rescoring;
}

namespace MergeTreeSetting
{
    extern const MergeTreeSettingsUInt64 max_concurrent_queries;
    extern const MergeTreeSettingsInt64 max_partitions_to_read;
    extern const MergeTreeSettingsUInt64 min_marks_to_honor_max_concurrent_queries;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int INDEX_NOT_USED;
    extern const int ILLEGAL_TYPE_OF_COLUMN_FOR_FILTER;
    extern const int ILLEGAL_COLUMN;
    extern const int ARGUMENT_OUT_OF_BOUND;
    extern const int CANNOT_PARSE_TEXT;
    extern const int TOO_MANY_PARTITIONS;
    extern const int DUPLICATED_PART_UUIDS;
    extern const int INCORRECT_DATA;
}

namespace FailPoints
{
    extern const char slowdown_index_analysis[];
}


MergeTreeDataSelectExecutor::MergeTreeDataSelectExecutor(const MergeTreeData & data_)
    : data(data_), log(getLogger(data.getLogName() + " (SelectExecutor)"))
{
}

size_t MergeTreeDataSelectExecutor::getApproximateTotalRowsToRead(
    const RangesInDataParts & parts,
    const StorageMetadataPtr & metadata_snapshot,
    const KeyCondition & key_condition,
    const Settings & settings,
    LoggerPtr log)
{
    size_t rows_count = 0;

    /// We will find out how many rows we would have read without sampling.
    LOG_DEBUG(log, "Preliminary index scan with condition: {}", key_condition.toString());

    MarkRanges exact_ranges;
    for (const auto & part : parts)
    {
        MarkRanges part_ranges = markRangesFromPKRange(part, metadata_snapshot, key_condition, {}, {}, &exact_ranges, settings, log);
        for (const auto & range : part_ranges)
            rows_count += part.data_part->index_granularity->getRowsCountInRange(range);
    }
    UNUSED(exact_ranges);

    return rows_count;
}


using RelativeSize = boost::rational<ASTSampleRatio::BigNum>;

static std::string toString(const RelativeSize & x)
{
    return ASTSampleRatio::toString(x.numerator()) + "/" + ASTSampleRatio::toString(x.denominator());
}

/// Converts sample size to an approximate number of rows (ex. `SAMPLE 1000000`) to relative value (ex. `SAMPLE 0.1`).
static RelativeSize convertAbsoluteSampleSizeToRelative(const ASTSampleRatio::Rational & ratio, size_t approx_total_rows)
{
    if (approx_total_rows == 0)
        return 1;

    auto absolute_sample_size = ratio.numerator / ratio.denominator;
    return std::min(RelativeSize(1), RelativeSize(absolute_sample_size) / RelativeSize(approx_total_rows));
}

QueryPlanPtr MergeTreeDataSelectExecutor::read(
    const Names & column_names_to_return,
    const StorageSnapshotPtr & storage_snapshot,
    const SelectQueryInfo & query_info,
    ContextPtr context,
    const UInt64 max_block_size,
    const size_t num_streams,
    PartitionIdToMaxBlockPtr max_block_numbers_to_read,
    bool enable_parallel_reading) const
{
    const auto & snapshot_data = assert_cast<const MergeTreeData::SnapshotData &>(*storage_snapshot->data);

    auto step = readFromParts(
        snapshot_data.parts,
        snapshot_data.mutations_snapshot,
        column_names_to_return,
        storage_snapshot,
        query_info,
        context,
        max_block_size,
        num_streams,
        max_block_numbers_to_read,
        /*merge_tree_select_result_ptr=*/ nullptr,
        enable_parallel_reading);

    auto plan = std::make_unique<QueryPlan>();
    if (step)
        plan->addStep(std::move(step));
    return plan;
}

MergeTreeDataSelectSamplingData MergeTreeDataSelectExecutor::getSampling(
    const SelectQueryInfo & select_query_info,
    NamesAndTypesList available_real_columns,
    const RangesInDataParts & parts,
    KeyCondition & key_condition,
    const MergeTreeData & data,
    const StorageMetadataPtr & metadata_snapshot,
    ContextPtr context,
    LoggerPtr log)
{
    const Settings & settings = context->getSettingsRef();
    /// Sampling.
    MergeTreeDataSelectSamplingData sampling;

    RelativeSize relative_sample_size = 0;
    RelativeSize relative_sample_offset = 0;

    std::optional<ASTSampleRatio::Rational> sample_size_ratio;
    std::optional<ASTSampleRatio::Rational> sample_offset_ratio;

    if (select_query_info.table_expression_modifiers)
    {
        const auto & table_expression_modifiers = *select_query_info.table_expression_modifiers;
        sample_size_ratio = table_expression_modifiers.getSampleSizeRatio();
        sample_offset_ratio = table_expression_modifiers.getSampleOffsetRatio();
    }
    else
    {
        auto & select = select_query_info.query->as<ASTSelectQuery &>();

        auto select_sample_size = select.sampleSize();
        auto select_sample_offset = select.sampleOffset();

        if (select_sample_size)
            sample_size_ratio = select_sample_size->as<ASTSampleRatio &>().ratio;

        if (select_sample_offset)
            sample_offset_ratio = select_sample_offset->as<ASTSampleRatio &>().ratio;
    }

    if (sample_size_ratio)
    {
        relative_sample_size.assign(sample_size_ratio->numerator, sample_size_ratio->denominator);

        if (relative_sample_size < 0)
            throw Exception(ErrorCodes::ARGUMENT_OUT_OF_BOUND, "Negative sample size");

        relative_sample_offset = 0;
        if (sample_offset_ratio)
            relative_sample_offset.assign(sample_offset_ratio->numerator, sample_offset_ratio->denominator);

        if (relative_sample_offset < 0)
            throw Exception(ErrorCodes::ARGUMENT_OUT_OF_BOUND, "Negative sample offset");

        /// Convert absolute value of the sampling (in form `SAMPLE 1000000` - how many rows to
        /// read) into the relative `SAMPLE 0.1` (how much data to read).
        size_t approx_total_rows = 0;
        if (relative_sample_size > 1 || relative_sample_offset > 1)
            approx_total_rows = getApproximateTotalRowsToRead(parts, metadata_snapshot, key_condition, settings, log);

        if (relative_sample_size > 1)
        {
            relative_sample_size = convertAbsoluteSampleSizeToRelative(*sample_size_ratio, approx_total_rows);
            LOG_DEBUG(log, "Selected relative sample size: {}", toString(relative_sample_size));
        }

        /// SAMPLE 1 is the same as the absence of SAMPLE.
        if (relative_sample_size == RelativeSize(1))
            relative_sample_size = 0;

        if (relative_sample_offset > 0 && RelativeSize(0) == relative_sample_size)
            throw Exception(ErrorCodes::ARGUMENT_OUT_OF_BOUND, "Sampling offset is incorrect because no sampling");

        if (relative_sample_offset > 1)
        {
            relative_sample_offset = convertAbsoluteSampleSizeToRelative(*sample_offset_ratio, approx_total_rows);
            LOG_DEBUG(log, "Selected relative sample offset: {}", toString(relative_sample_offset));
        }
    }

    /** Which range of sampling key values do I need to read?
        * First, in the whole range ("universe") we select the interval
        *  of relative `relative_sample_size` size, offset from the beginning by `relative_sample_offset`.
        *
        * Example: SAMPLE 0.4 OFFSET 0.3
        *
        * [------********------]
        *        ^ - offset
        *        <------> - size
        *
        * If the interval passes through the end of the universe, then cut its right side.
        *
        * Example: SAMPLE 0.4 OFFSET 0.8
        *
        * [----------------****]
        *                  ^ - offset
        *                  <------> - size
        *
        * Next, if the `parallel_replicas_count`, `parallel_replica_offset` settings are set,
        *  then it is necessary to break the received interval into pieces of the number `parallel_replicas_count`,
        *  and select a piece with the number `parallel_replica_offset` (from zero).
        *
        * Example: SAMPLE 0.4 OFFSET 0.3, parallel_replicas_count = 2, parallel_replica_offset = 1
        *
        * [----------****------]
        *        ^ - offset
        *        <------> - size
        *        <--><--> - pieces for different `parallel_replica_offset`, select the second one.
        *
        * It is very important that the intervals for different `parallel_replica_offset` cover the entire range without gaps and overlaps.
        * It is also important that the entire universe can be covered using SAMPLE 0.1 OFFSET 0, ... OFFSET 0.9 and similar decimals.
        */

    const bool can_use_sampling_key_parallel_replicas =
        settings[Setting::allow_experimental_parallel_reading_from_replicas] > 0
        && settings[Setting::max_parallel_replicas] > 1
        && settings[Setting::parallel_replicas_mode] == ParallelReplicasMode::SAMPLING_KEY;

    /// Parallel replicas has been requested but there is no way to sample data.
    /// Select all data from first replica and no data from other replicas.
    if (can_use_sampling_key_parallel_replicas && settings[Setting::parallel_replicas_count] > 1
        && !data.supportsSampling() && settings[Setting::parallel_replica_offset] > 0)
    {
        LOG_DEBUG(
            log,
            "Will use no data on this replica because parallel replicas processing has been requested"
            " (the setting 'max_parallel_replicas') but the table does not support sampling and this replica is not the first.");
        sampling.read_nothing = true;
        return sampling;
    }

    sampling.use_sampling = relative_sample_size > 0 || (can_use_sampling_key_parallel_replicas && settings[Setting::parallel_replicas_count] > 1 && data.supportsSampling());
    bool no_data = false; /// There is nothing left after sampling.

    if (sampling.use_sampling)
    {
        if (relative_sample_size != RelativeSize(0))
            sampling.used_sample_factor = 1.0 / boost::rational_cast<Float64>(relative_sample_size);

        RelativeSize size_of_universum = 0;
        const auto & sampling_key = metadata_snapshot->getSamplingKey();
        DataTypePtr sampling_column_type = sampling_key.data_types.at(0);

        if (sampling_key.data_types.size() == 1)
        {
            if (typeid_cast<const DataTypeUInt64 *>(sampling_column_type.get()))
                size_of_universum = RelativeSize(std::numeric_limits<UInt64>::max()) + RelativeSize(1);
            else if (typeid_cast<const DataTypeUInt32 *>(sampling_column_type.get()))
                size_of_universum = RelativeSize(std::numeric_limits<UInt32>::max()) + RelativeSize(1);
            else if (typeid_cast<const DataTypeUInt16 *>(sampling_column_type.get()))
                size_of_universum = RelativeSize(std::numeric_limits<UInt16>::max()) + RelativeSize(1);
            else if (typeid_cast<const DataTypeUInt8 *>(sampling_column_type.get()))
                size_of_universum = RelativeSize(std::numeric_limits<UInt8>::max()) + RelativeSize(1);
        }

        if (size_of_universum == RelativeSize(0))
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_COLUMN_FOR_FILTER,
                "Invalid sampling column type in storage parameters: {}. Must be one unsigned integer type",
                sampling_column_type->getName());

        if (settings[Setting::parallel_replicas_count] > 1)
        {
            if (relative_sample_size == RelativeSize(0))
                relative_sample_size = 1;

            relative_sample_size /= settings[Setting::parallel_replicas_count].value;
            relative_sample_offset += relative_sample_size * RelativeSize(settings[Setting::parallel_replica_offset].value);
        }

        if (relative_sample_offset >= RelativeSize(1))
            no_data = true;

        /// Calculate the half-interval of `[lower, upper)` column values.
        bool has_lower_limit = false;
        bool has_upper_limit = false;

        RelativeSize lower_limit_rational = relative_sample_offset * size_of_universum;
        RelativeSize upper_limit_rational = (relative_sample_offset + relative_sample_size) * size_of_universum;

        UInt64 lower = boost::rational_cast<ASTSampleRatio::BigNum>(lower_limit_rational);
        UInt64 upper = boost::rational_cast<ASTSampleRatio::BigNum>(upper_limit_rational);

        if (lower > 0)
            has_lower_limit = true;

        if (upper_limit_rational < size_of_universum)
            has_upper_limit = true;

        /*std::cerr << std::fixed << std::setprecision(100)
            << "relative_sample_size: " << relative_sample_size << "\n"
            << "relative_sample_offset: " << relative_sample_offset << "\n"
            << "lower_limit_float: " << lower_limit_rational << "\n"
            << "upper_limit_float: " << upper_limit_rational << "\n"
            << "lower: " << lower << "\n"
            << "upper: " << upper << "\n";*/

        if ((has_upper_limit && upper == 0)
            || (has_lower_limit && has_upper_limit && lower == upper))
            no_data = true;

        if (no_data || (!has_lower_limit && !has_upper_limit))
        {
            sampling.use_sampling = false;
        }
        else
        {
            /// Let's add the conditions to cut off something else when the index is scanned again and when the request is processed.

            std::shared_ptr<ASTFunction> lower_function;
            std::shared_ptr<ASTFunction> upper_function;

            chassert(metadata_snapshot->getSamplingKeyAST() != nullptr);
            ASTPtr sampling_key_ast = metadata_snapshot->getSamplingKeyAST()->clone();

            if (has_lower_limit)
            {
                if (!key_condition.addCondition(
                        sampling_key.column_names[0],
                        Range::createLeftBounded(lower, true, isNullableOrLowCardinalityNullable(sampling_key.data_types[0]))))
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Sampling column not in primary key");

                ASTPtr args = std::make_shared<ASTExpressionList>();
                args->children.push_back(sampling_key_ast);
                args->children.push_back(std::make_shared<ASTLiteral>(lower));

                lower_function = std::make_shared<ASTFunction>();
                lower_function->name = "greaterOrEquals";
                lower_function->arguments = args;
                lower_function->children.push_back(lower_function->arguments);

                sampling.filter_function = lower_function;
            }

            if (has_upper_limit)
            {
                if (!key_condition.addCondition(
                        sampling_key.column_names[0],
                        Range::createRightBounded(upper, false, isNullableOrLowCardinalityNullable(sampling_key.data_types[0]))))
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Sampling column not in primary key");

                ASTPtr args = std::make_shared<ASTExpressionList>();
                args->children.push_back(sampling_key_ast);
                args->children.push_back(std::make_shared<ASTLiteral>(upper));

                upper_function = std::make_shared<ASTFunction>();
                upper_function->name = "less";
                upper_function->arguments = args;
                upper_function->children.push_back(upper_function->arguments);

                sampling.filter_function = upper_function;
            }

            if (has_lower_limit && has_upper_limit)
            {
                ASTPtr args = std::make_shared<ASTExpressionList>();
                args->children.push_back(lower_function);
                args->children.push_back(upper_function);

                sampling.filter_function = std::make_shared<ASTFunction>();
                sampling.filter_function->name = "and";
                sampling.filter_function->arguments = args;
                sampling.filter_function->children.push_back(sampling.filter_function->arguments);
            }

            ASTPtr query = sampling.filter_function;
            auto syntax_result = TreeRewriter(context).analyze(query, available_real_columns);
            sampling.filter_expression = std::make_shared<const ActionsDAG>(ExpressionAnalyzer(sampling.filter_function, syntax_result, context).getActionsDAG(false));
        }
    }

    if (no_data)
    {
        LOG_DEBUG(log, "Sampling yields no data.");
        sampling.read_nothing = true;
    }

    return sampling;
}

void MergeTreeDataSelectExecutor::buildKeyConditionFromPartOffset(
    std::optional<KeyCondition> & part_offset_condition, const ActionsDAG::Node * predicate, ContextPtr context)
{
    if (!predicate)
        return;

    auto part_offset_type = std::make_shared<DataTypeUInt64>();
    auto part_type = std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>());
    Block sample
        = {ColumnWithTypeAndName(part_offset_type->createColumn(), part_offset_type, "_part_offset"),
           ColumnWithTypeAndName(part_type->createColumn(), part_type, "_part")};

    auto dag = VirtualColumnUtils::splitFilterDagForAllowedInputs(predicate, &sample);
    if (!dag)
        return;

    /// The _part filter should only be effective in conjunction with the _part_offset filter.
    auto required_columns = dag->getRequiredColumnsNames();
    if (std::find(required_columns.begin(), required_columns.end(), "_part_offset") == required_columns.end())
        return;

    part_offset_condition.emplace(KeyCondition{
        ActionsDAGWithInversionPushDown(dag->getOutputs().front(), context),
        context,
        sample.getNames(),
        std::make_shared<ExpressionActions>(ActionsDAG(sample.getColumnsWithTypeAndName()), ExpressionActionsSettings{}),
        {}});
}

void MergeTreeDataSelectExecutor::buildKeyConditionFromTotalOffset(
    std::optional<KeyCondition> & total_offset_condition, const ActionsDAG::Node * predicate, ContextPtr context)
{
    if (!predicate)
        return;

    auto part_offset_type = std::make_shared<DataTypeUInt64>();
    Block sample
        = {ColumnWithTypeAndName(part_offset_type->createColumn(), part_offset_type, "_part_offset"),
           ColumnWithTypeAndName(part_offset_type->createColumn(), part_offset_type, "_part_starting_offset")};

    auto dag = VirtualColumnUtils::splitFilterDagForAllowedInputs(predicate, &sample);
    if (!dag)
        return;

    /// Try to recognize and fold expressions of the form:
    ///     _part_offset + _part_starting_offset
    /// or  _part_starting_offset + _part_offset
    ActionsDAG total_offset;
    const auto * part_offset = &total_offset.addInput("_part_offset", part_offset_type);
    const auto * part_starting_offset = &total_offset.addInput("_part_starting_offset", part_offset_type);
    const auto * node1
        = &total_offset.addFunction(FunctionFactory::instance().get("plus", context), {part_offset, part_starting_offset}, {});
    const auto * node2
        = &total_offset.addFunction(FunctionFactory::instance().get("plus", context), {part_starting_offset, part_offset}, {});
    auto matches = matchTrees({node1, node2}, *dag, false /* check_monotonicity */);
    auto new_inputs = resolveMatchedInputs(matches, {node1, node2}, dag->getOutputs());
    if (!new_inputs)
        return;
    dag = ActionsDAG::foldActionsByProjection(*new_inputs, dag->getOutputs());

    /// total_offset_condition is only valid if _part_offset and _part_starting_offset are used *together*.
    /// After folding, we expect a single input representing their combination.
    /// If more than one input remains, it means either of them is used independently,
    /// and we should skip adding total_offset_condition in that case.
    if (dag->getInputs().size() != 1)
        return;

    auto required_columns = dag->getRequiredColumns();
    total_offset_condition.emplace(KeyCondition{
        ActionsDAGWithInversionPushDown(dag->getOutputs().front(), context),
        context,
        required_columns.getNames(),
        std::make_shared<ExpressionActions>(ActionsDAG(required_columns), ExpressionActionsSettings{}),
        {}});
}

std::optional<std::unordered_set<String>> MergeTreeDataSelectExecutor::filterPartsByVirtualColumns(
    const StorageMetadataPtr & metadata_snapshot,
    const MergeTreeData & data,
    const RangesInDataParts & parts,
    const ActionsDAG::Node * predicate,
    ContextPtr context)
{
    if (!predicate)
        return {};

    auto sample = data.getHeaderWithVirtualsForFilter(metadata_snapshot);
    auto dag = VirtualColumnUtils::splitFilterDagForAllowedInputs(predicate, &sample);
    if (!dag)
        return {};

    auto virtual_columns_block = data.getBlockWithVirtualsForFilter(metadata_snapshot, parts);
    VirtualColumnUtils::filterBlockWithExpression(VirtualColumnUtils::buildFilterExpression(std::move(*dag), context), virtual_columns_block);
    return VirtualColumnUtils::extractSingleValueFromBlock<String>(virtual_columns_block, "_part");
}

void MergeTreeDataSelectExecutor::filterPartsByPartition(
    RangesInDataParts & parts,
    const std::optional<PartitionPruner> & partition_pruner,
    const std::optional<KeyCondition> & minmax_idx_condition,
    const std::optional<std::unordered_set<String>> & part_values,
    const StorageMetadataPtr & metadata_snapshot,
    const MergeTreeData & data,
    const ContextPtr & context,
    const PartitionIdToMaxBlock * max_block_numbers_to_read,
    LoggerPtr log,
    ReadFromMergeTree::IndexStats & index_stats)
{
    const Settings & settings = context->getSettingsRef();
    DataTypes minmax_columns_types;

    if (metadata_snapshot->hasPartitionKey())
    {
        chassert(minmax_idx_condition && partition_pruner);
        const auto & partition_key = metadata_snapshot->getPartitionKey();
        minmax_columns_types = MergeTreeData::getMinMaxColumnsTypes(partition_key);

        if (settings[Setting::force_index_by_date] && (minmax_idx_condition->alwaysUnknownOrTrue() && partition_pruner->isUseless()))
        {
            auto minmax_columns_names = MergeTreeData::getMinMaxColumnsNames(partition_key);
            throw Exception(ErrorCodes::INDEX_NOT_USED,
                "Neither MinMax index by columns ({}) nor partition expr is used and setting 'force_index_by_date' is set",
                fmt::join(minmax_columns_names, ", "));
        }
    }

    auto query_context = context->hasQueryContext() ? context->getQueryContext() : context;
    QueryStatusPtr query_status = context->getProcessListElement();

    PartFilterCounters part_filter_counters;
    if (query_context->getSettingsRef()[Setting::allow_experimental_query_deduplication])
        selectPartsToReadWithUUIDFilter(
            parts,
            part_values,
            data.getPinnedPartUUIDs(),
            minmax_idx_condition,
            minmax_columns_types,
            partition_pruner,
            max_block_numbers_to_read,
            query_context,
            part_filter_counters,
            log);
    else
        selectPartsToRead(
            parts,
            part_values,
            minmax_idx_condition,
            minmax_columns_types,
            partition_pruner,
            max_block_numbers_to_read,
            part_filter_counters,
            query_status);

    index_stats.emplace_back(ReadFromMergeTree::IndexStat{
        .type = ReadFromMergeTree::IndexType::None,
        .num_parts_after = part_filter_counters.num_initial_selected_parts,
        .num_granules_after = part_filter_counters.num_initial_selected_granules});

    if (minmax_idx_condition)
    {
        auto description = minmax_idx_condition->getDescription();
        index_stats.emplace_back(ReadFromMergeTree::IndexStat{
            .type = ReadFromMergeTree::IndexType::MinMax,
            .condition = std::move(description.condition),
            .used_keys = std::move(description.used_keys),
            .num_parts_after = part_filter_counters.num_parts_after_minmax,
            .num_granules_after = part_filter_counters.num_granules_after_minmax});
        LOG_DEBUG(log, "MinMax index condition: {}", minmax_idx_condition->toString());
    }

    if (partition_pruner)
    {
        auto description = partition_pruner->getKeyCondition().getDescription();
        index_stats.emplace_back(ReadFromMergeTree::IndexStat{
            .type = ReadFromMergeTree::IndexType::Partition,
            .condition = std::move(description.condition),
            .used_keys = std::move(description.used_keys),
            .num_parts_after = part_filter_counters.num_parts_after_partition_pruner,
            .num_granules_after = part_filter_counters.num_granules_after_partition_pruner});
    }
}

RangesInDataParts MergeTreeDataSelectExecutor::filterPartsByPrimaryKeyAndSkipIndexes(
    RangesInDataParts parts_with_ranges,
    StorageMetadataPtr metadata_snapshot,
    MergeTreeData::MutationsSnapshotPtr mutations_snapshot,
    const ContextPtr & context,
    const KeyCondition & key_condition,
    const std::optional<KeyCondition> & part_offset_condition,
    const std::optional<KeyCondition> & total_offset_condition,
    const UsefulSkipIndexes & skip_indexes,
    const MergeTreeReaderSettings & reader_settings,
    LoggerPtr log,
    size_t num_streams,
    ReadFromMergeTree::IndexStats & index_stats,
    bool use_skip_indexes,
    bool find_exact_ranges,
    bool is_final_query)
{
    const Settings & settings = context->getSettingsRef();

    if (context->canUseParallelReplicasOnFollower() && settings[Setting::parallel_replicas_local_plan]
        && settings[Setting::parallel_replicas_index_analysis_only_on_coordinator])
    {
        // Skip index analysis and return parts with all marks
        // The coordinator will chose ranges to read for workers based on index analysis on its side
        return parts_with_ranges;
    }

    if (use_skip_indexes && settings[Setting::force_data_skipping_indices].changed)
    {
        const auto & indices_str = settings[Setting::force_data_skipping_indices].toString();
        auto forced_indices = parseIdentifiersOrStringLiterals(indices_str, settings);

        if (forced_indices.empty())
            throw Exception(ErrorCodes::CANNOT_PARSE_TEXT, "No indices parsed from force_data_skipping_indices ('{}')", indices_str);

        std::unordered_set<std::string> useful_indices_names;
        for (const auto & useful_index : skip_indexes.useful_indices)
            useful_indices_names.insert(useful_index.index->index.name);

        for (const auto & index_name : forced_indices)
        {
            if (!useful_indices_names.contains(index_name))
            {
                throw Exception(
                    ErrorCodes::INDEX_NOT_USED,
                    "Index {} is not used and setting 'force_data_skipping_indices' contains it",
                    backQuote(index_name));
            }
        }
    }

    struct IndexStat
    {
        std::atomic<size_t> total_granules = 0;
        std::atomic<size_t> granules_dropped = 0;
        std::atomic<size_t> total_parts = 0;
        std::atomic<size_t> parts_dropped = 0;
        std::atomic<size_t> elapsed_us = 0;
        std::atomic<MarkRanges::SearchAlgorithm> search_algorithm = MarkRanges::SearchAlgorithm::Unknown;
    };

    IndexStat pk_stat;
    std::vector<IndexStat> useful_indices_stat(skip_indexes.useful_indices.size());
    std::vector<IndexStat> merged_indices_stat(skip_indexes.merged_indices.size());

    std::atomic<size_t> sum_marks_pk = 0;
    std::atomic<size_t> sum_parts_pk = 0;

    std::vector<size_t> skip_index_used_in_part(parts_with_ranges.size(), 0);

    size_t num_threads = std::min<size_t>(num_streams, parts_with_ranges.size());
    if (settings[Setting::max_threads_for_indexes])
    {
        num_threads = std::min<size_t>(num_streams, settings[Setting::max_threads_for_indexes]);
    }

    /// Let's find what range to read from each part.
    {
        auto mark_cache = context->getIndexMarkCache();
        auto uncompressed_cache = context->getIndexUncompressedCache();
        auto vector_similarity_index_cache = context->getVectorSimilarityIndexCache();

        auto query_status = context->getProcessListElement();

        auto process_part = [&](size_t part_index)
        {
            if (query_status)
                query_status->checkTimeLimit();

            auto & ranges = parts_with_ranges[part_index];
            if (metadata_snapshot->hasPrimaryKey() || part_offset_condition || total_offset_condition)
            {
                CurrentMetrics::Increment metric(CurrentMetrics::FilteringMarksWithPrimaryKey);
                ProfileEventTimeIncrement<Microseconds> watch(ProfileEvents::FilteringMarksWithPrimaryKeyMicroseconds);

                size_t total_marks_count = ranges.data_part->index_granularity->getMarksCountWithoutFinal();
                pk_stat.total_parts.fetch_add(1, std::memory_order_relaxed);
                pk_stat.total_granules.fetch_add(ranges.data_part->index_granularity->getMarksCountWithoutFinal(), std::memory_order_relaxed);

                ranges.ranges = markRangesFromPKRange(
                    ranges,
                    metadata_snapshot,
                    key_condition,
                    part_offset_condition,
                    total_offset_condition,
                    find_exact_ranges ? &ranges.exact_ranges : nullptr,
                    settings,
                    log);

                pk_stat.search_algorithm.store(ranges.ranges.search_algorithm, std::memory_order_relaxed);
                pk_stat.granules_dropped.fetch_add(total_marks_count - ranges.ranges.getNumberOfMarks(), std::memory_order_relaxed);
                if (ranges.ranges.empty())
                    pk_stat.parts_dropped.fetch_add(1, std::memory_order_relaxed);
                pk_stat.elapsed_us.fetch_add(watch.elapsed(), std::memory_order_relaxed);
            }

            sum_marks_pk.fetch_add(ranges.getMarksCount(), std::memory_order_relaxed);

            if (!ranges.ranges.empty())
                sum_parts_pk.fetch_add(1, std::memory_order_relaxed);

            CurrentMetrics::Increment metric(CurrentMetrics::FilteringMarksWithSecondaryKeys);
            auto alter_conversions = MergeTreeData::getAlterConversionsForPart(ranges.data_part, mutations_snapshot, context);
            const auto & all_updated_columns = alter_conversions->getAllUpdatedColumns();

            auto can_use_index = [&](const MergeTreeIndexPtr & index) -> std::expected<void, PreformattedMessage>
            {
                if (all_updated_columns.empty())
                    return {};

                auto options = GetColumnsOptions(GetColumnsOptions::Kind::All).withSubcolumns();
                auto required_columns_names = index ->getColumnsRequiredForIndexCalc();
                auto required_columns_list = metadata_snapshot->getColumns().getByNames(options, required_columns_names);

                auto it = std::ranges::find_if(required_columns_list, [&](const auto & column)
                {
                    return all_updated_columns.contains(column.getNameInStorage());
                });

                if (it == required_columns_list.end())
                    return {};

                return std::unexpected(PreformattedMessage::create(
                    "Index {} is not used for part {} because it depends on column {} which will be updated on fly",
                    index->index.name, index->index.name, it->getNameInStorage()));
            };

            auto can_use_merged_index = [&](const std::vector<MergeTreeIndexPtr> & indices) -> std::expected<void, PreformattedMessage>
            {
                for (const auto & index : indices)
                {
                    if (auto result = can_use_index(index); !result)
                        return result;
                }
                return {};
            };

            for (size_t idx = 0; idx < skip_indexes.useful_indices.size(); ++idx)
            {
                if (ranges.ranges.empty())
                    break;

                ProfileEventTimeIncrement<Microseconds> watch(ProfileEvents::FilteringMarksWithSecondaryKeysMicroseconds);

                const auto & index_and_condition = skip_indexes.useful_indices[idx];
                auto & stat = useful_indices_stat[idx];
                stat.total_parts.fetch_add(1, std::memory_order_relaxed);
                size_t total_granules = ranges.ranges.getNumberOfMarks();
                stat.total_granules.fetch_add(total_granules, std::memory_order_relaxed);

                if (auto result = can_use_index(index_and_condition.index); !result)
                {
                    LOG_TRACE(log, "{}", result.error().text);
                    continue;
                }

                std::tie(ranges.ranges, ranges.read_hints) = filterMarksUsingIndex(
                    index_and_condition.index,
                    index_and_condition.condition,
                    ranges.data_part,
                    ranges.ranges,
                    ranges.read_hints,
                    settings,
                    reader_settings,
                    mark_cache.get(),
                    uncompressed_cache.get(),
                    vector_similarity_index_cache.get(),
                    log);

                stat.granules_dropped.fetch_add(total_granules - ranges.ranges.getNumberOfMarks(), std::memory_order_relaxed);
                if (ranges.ranges.empty())
                    stat.parts_dropped.fetch_add(1, std::memory_order_relaxed);
                stat.elapsed_us.fetch_add(watch.elapsed(), std::memory_order_relaxed);
                skip_index_used_in_part[part_index] = 1; /// thread-safe
            }

            for (size_t idx = 0; idx < skip_indexes.merged_indices.size(); ++idx)
            {
                if (ranges.ranges.empty())
                    break;

                const auto & indices_and_condition = skip_indexes.merged_indices[idx];
                auto & stat = merged_indices_stat[idx];
                stat.total_parts.fetch_add(1, std::memory_order_relaxed);

                if (auto result = can_use_merged_index(indices_and_condition.indices); !result)
                {
                    LOG_TRACE(log, "{}", result.error().text);
                    continue;
                }

                size_t total_granules = ranges.ranges.getNumberOfMarks();
                ranges.ranges = filterMarksUsingMergedIndex(
                    indices_and_condition.indices, indices_and_condition.condition,
                    ranges.data_part, ranges.ranges,
                    settings, reader_settings,
                    mark_cache.get(), uncompressed_cache.get(), vector_similarity_index_cache.get(), log);

                stat.total_granules.fetch_add(total_granules, std::memory_order_relaxed);
                stat.granules_dropped.fetch_add(total_granules - ranges.ranges.getNumberOfMarks(), std::memory_order_relaxed);

                if (ranges.ranges.empty())
                    stat.parts_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        };

        LOG_TRACE(log, "Filtering marks by primary and secondary keys");

        if (num_threads <= 1)
        {
            for (size_t part_index = 0; part_index < parts_with_ranges.size(); ++part_index)
                process_part(part_index);
        }
        else
        {
            /// Parallel loading and filtering of data parts.
            ThreadPool pool(
                CurrentMetrics::MergeTreeDataSelectExecutorThreads,
                CurrentMetrics::MergeTreeDataSelectExecutorThreadsActive,
                CurrentMetrics::MergeTreeDataSelectExecutorThreadsScheduled,
                num_threads);


            /// Instances of ThreadPool "borrow" threads from the global thread pool.
            /// We intentionally use scheduleOrThrow here to avoid a deadlock.
            /// For example, queries can already be running with threads from the
            /// global pool, and if we saturate max_thread_pool_size whilst requesting
            /// more in this loop, queries will block infinitely.
            /// So we wait until lock_acquire_timeout, and then raise an exception.
            for (size_t part_index = 0; part_index < parts_with_ranges.size(); ++part_index)
            {
                pool.scheduleOrThrow(
                    [&, part_index, thread_group = CurrentThread::getGroup()]
                    {
                        ThreadGroupSwitcher switcher(thread_group, "MergeTreeIndex");

                        process_part(part_index);
                    },
                    Priority{},
                    context->getSettingsRef()[Setting::lock_acquire_timeout].totalMicroseconds());
            }

            pool.wait();
        }

        /// Skip empty ranges.
        std::erase_if(
            parts_with_ranges,
            [&](const auto & part)
            {
                size_t index = &part - parts_with_ranges.data();
                if (is_final_query && settings[Setting::use_skip_indexes_if_final_exact_mode] && skip_index_used_in_part[index])
                {
                    /// retain this part even if empty due to FINAL
                    return false;
                }

                return !part.data_part || part.ranges.empty();
            });
    }

    if (metadata_snapshot->hasPrimaryKey())
    {
        LOG_DEBUG(
            log,
            "PK index has dropped {}/{} granules, it took {}ms across {} threads.",
            pk_stat.granules_dropped.load(),
            pk_stat.total_granules.load(),
            pk_stat.elapsed_us.load() / 1000,
            num_threads);

        auto description = key_condition.getDescription();

        index_stats.emplace_back(ReadFromMergeTree::IndexStat{
            .type = ReadFromMergeTree::IndexType::PrimaryKey,
            .condition = std::move(description.condition),
            .used_keys = std::move(description.used_keys),
            .num_parts_after = sum_parts_pk.load(std::memory_order_relaxed),
            .num_granules_after = sum_marks_pk.load(std::memory_order_relaxed),
            .search_algorithm = pk_stat.search_algorithm.load(std::memory_order_relaxed)
        });
    }

    for (size_t idx = 0; idx < skip_indexes.useful_indices.size(); ++idx)
    {
        const auto & index_and_condition = skip_indexes.useful_indices[idx];
        const auto & stat = useful_indices_stat[idx];
        const auto & index_name = index_and_condition.index->index.name;
        LOG_DEBUG(
            log,
            "Index {} has dropped {}/{} granules, it took {}ms across {} threads.",
            backQuote(index_name),
            stat.granules_dropped.load(),
            stat.total_granules.load(),
            stat.elapsed_us.load() / 1000,
            num_threads);

        std::string description
            = index_and_condition.index->index.type + " GRANULARITY " + std::to_string(index_and_condition.index->index.granularity);

        index_stats.emplace_back(ReadFromMergeTree::IndexStat{
            .type = ReadFromMergeTree::IndexType::Skip,
            .name = index_name,
            .description = std::move(description),
            .num_parts_after = stat.total_parts - stat.parts_dropped,
            .num_granules_after = stat.total_granules - stat.granules_dropped});
    }

    for (size_t idx = 0; idx < skip_indexes.merged_indices.size(); ++idx)
    {
        const auto & index_and_condition = skip_indexes.merged_indices[idx];
        const auto & stat = merged_indices_stat[idx];
        const auto & index_name = "Merged";
        LOG_DEBUG(
            log,
            "Index {} has dropped {}/{} granules, it took {}ms across {} threads.",
            backQuote(index_name),
            stat.granules_dropped.load(),
            stat.total_granules.load(),
            stat.elapsed_us.load() / 1000,
            num_threads);

        std::string description = "MERGED GRANULARITY " + std::to_string(index_and_condition.indices.at(0)->index.granularity);

        index_stats.emplace_back(ReadFromMergeTree::IndexStat{
            .type = ReadFromMergeTree::IndexType::Skip,
            .name = index_name,
            .description = std::move(description),
            .num_parts_after = stat.total_parts - stat.parts_dropped,
            .num_granules_after = stat.total_granules - stat.granules_dropped});
    }

    return parts_with_ranges;
}

void MergeTreeDataSelectExecutor::filterPartsByQueryConditionCache(
    RangesInDataParts & parts_with_ranges,
    const SelectQueryInfo & select_query_info,
    const std::optional<VectorSearchParameters> & vector_search_parameters,
    const ContextPtr & context,
    LoggerPtr log)
{
    const auto & settings = context->getSettingsRef();
    if (!settings[Setting::use_query_condition_cache]
            || !settings[Setting::allow_experimental_analyzer]
            || (!select_query_info.prewhere_info && !select_query_info.filter_actions_dag)
            || (vector_search_parameters.has_value())) /// vector search has filter in the ORDER BY
        return;

    QueryConditionCachePtr query_condition_cache = context->getQueryConditionCache();

    struct Stats
    {
        size_t total_granules = 0;
        size_t granules_dropped = 0;
    };

    auto drop_mark_ranges = [&](const ActionsDAG::Node * dag)
    {
        UInt64 condition_hash = dag->getHash();
        Stats stats;
        for (auto it = parts_with_ranges.begin(); it != parts_with_ranges.end();)
        {
            auto & part_with_ranges = *it;
            stats.total_granules += part_with_ranges.getMarksCount();

            const auto & data_part = part_with_ranges.data_part;
            auto storage_id = data_part->storage.getStorageID();
            auto matching_marks_opt = query_condition_cache->read(storage_id.uuid, data_part->name, condition_hash);
            if (!matching_marks_opt)
            {
                ++it;
                continue;
            }

            auto & matching_marks = *matching_marks_opt;
            MarkRanges ranges;
            const auto & part = it->data_part;
            size_t min_marks_for_seek = roundRowsOrBytesToMarks(
                settings[Setting::merge_tree_min_rows_for_seek],
                settings[Setting::merge_tree_min_bytes_for_seek],
                part->index_granularity_info.fixed_index_granularity,
                part->index_granularity_info.index_granularity_bytes);

            for (const auto & mark_range : part_with_ranges.ranges)
            {
                size_t begin = mark_range.begin;
                for (size_t mark_it = begin; mark_it < mark_range.end;)
                {
                    if (!matching_marks[mark_it])
                    {
                        if (mark_it == begin)
                        {
                            /// mark_range.begin -> 0 0 0 1 x x x x. Need to skip starting zeros.
                            ++stats.granules_dropped;
                            ++begin;
                            ++mark_it;
                        }
                        else
                        {
                            size_t end = mark_it;
                            for (; end < mark_range.end && !matching_marks[end]; ++end)
                                ;

                            if (min_marks_for_seek && end != mark_range.end && end - mark_it <= min_marks_for_seek)
                            {
                                /// x x x 1 1 1 0 0 1 x x x. And gap is small enough to merge, skip gap.
                                mark_it = end + 1;
                            }
                            else
                            {
                                /// Case1: x x x 1 1 1 0 0 1 x x x. Gap is too big to merge, do not merge
                                /// Case2: x x x 1 1 1 0 0 0 0 -> mark_range.end. Reach the end of range, do not merge
                                stats.granules_dropped += end - mark_it;
                                ranges.emplace_back(begin, mark_it);
                                begin = end;

                                if (end == mark_range.end)
                                    break;

                                mark_it = end + 1;
                            }
                        }
                    }
                    else
                        ++mark_it;
                }

                if (begin != mark_range.begin && begin != mark_range.end)
                    ranges.emplace_back(begin, mark_range.end);
                else if (begin == mark_range.begin)
                    ranges.emplace_back(begin, mark_range.end);
            }

            if (ranges.empty())
                it = parts_with_ranges.erase(it);
            else
            {
                part_with_ranges.ranges = std::move(ranges);
                ++it;
            }
        }

        return stats;
    };

    if (const auto & prewhere_info = select_query_info.prewhere_info)
    {
        for (const auto * outputs : prewhere_info->prewhere_actions.getOutputs())
        {
            if (outputs->result_name == prewhere_info->prewhere_column_name)
            {
                auto stats = drop_mark_ranges(outputs);
                LOG_DEBUG(log,
                        "Query condition cache has dropped {}/{} granules for PREWHERE condition {}.",
                        stats.granules_dropped,
                        stats.total_granules,
                        prewhere_info->prewhere_column_name);
                break;
            }
        }
    }

    if (const auto & filter_actions_dag = select_query_info.filter_actions_dag)
    {
        const auto * output = filter_actions_dag->getOutputs().front();
        auto stats = drop_mark_ranges(output);
        LOG_DEBUG(log,
                "Query condition cache has dropped {}/{} granules for WHERE condition {}.",
                stats.granules_dropped,
                stats.total_granules,
                filter_actions_dag->getOutputs().front()->result_name);
    }
}


std::shared_ptr<QueryIdHolder> MergeTreeDataSelectExecutor::checkLimits(
    const MergeTreeData & data,
    const ReadFromMergeTree::AnalysisResult & result,
    const ContextPtr & context)
{
    const auto & settings = context->getSettingsRef();
    const auto data_settings = data.getSettings();
    auto max_partitions_to_read
        = settings[Setting::max_partitions_to_read].changed ? settings[Setting::max_partitions_to_read].value : (*data_settings)[MergeTreeSetting::max_partitions_to_read].value;
    if (max_partitions_to_read > 0)
    {
        std::set<String> partitions;
        for (const auto & part_with_ranges : result.parts_with_ranges)
            partitions.insert(part_with_ranges.data_part->info.getPartitionId());
        if (partitions.size() > static_cast<size_t>(max_partitions_to_read))
            throw Exception(
                ErrorCodes::TOO_MANY_PARTITIONS,
                "Too many partitions to read. Current {}, max {}",
                partitions.size(),
                max_partitions_to_read);
    }

    if ((*data_settings)[MergeTreeSetting::max_concurrent_queries] > 0 && (*data_settings)[MergeTreeSetting::min_marks_to_honor_max_concurrent_queries] > 0
        && result.selected_marks >= (*data_settings)[MergeTreeSetting::min_marks_to_honor_max_concurrent_queries])
    {
        auto query_id = context->getCurrentQueryId();
        if (!query_id.empty())
            return data.getQueryIdHolder(query_id, (*data_settings)[MergeTreeSetting::max_concurrent_queries]);
    }
    return nullptr;
}

ReadFromMergeTree::AnalysisResultPtr MergeTreeDataSelectExecutor::estimateNumMarksToRead(
    RangesInDataParts parts,
    MergeTreeData::MutationsSnapshotPtr mutations_snapshot,
    const Names & column_names_to_return,
    const StorageMetadataPtr & metadata_snapshot,
    const SelectQueryInfo & query_info,
    ContextPtr context,
    size_t num_streams,
    PartitionIdToMaxBlockPtr max_block_numbers_to_read) const
{
    size_t total_parts = parts.size();
    if (total_parts == 0)
        return std::make_shared<ReadFromMergeTree::AnalysisResult>();

    std::optional<ReadFromMergeTree::Indexes> indexes;
    return ReadFromMergeTree::selectRangesToRead(
        std::move(parts),
        mutations_snapshot,
        std::nullopt,
        metadata_snapshot,
        query_info,
        context,
        num_streams,
        max_block_numbers_to_read,
        data,
        column_names_to_return,
        log,
        indexes,
        /*find_exact_ranges*/false);
}

QueryPlanStepPtr MergeTreeDataSelectExecutor::readFromParts(
    RangesInDataParts parts,
    MergeTreeData::MutationsSnapshotPtr mutations_snapshot,
    const Names & column_names_to_return,
    const StorageSnapshotPtr & storage_snapshot,
    const SelectQueryInfo & query_info,
    ContextPtr context,
    const UInt64 max_block_size,
    const size_t num_streams,
    PartitionIdToMaxBlockPtr max_block_numbers_to_read,
    ReadFromMergeTree::AnalysisResultPtr merge_tree_select_result_ptr,
    bool enable_parallel_reading) const
{
    /// If merge_tree_select_result_ptr != nullptr, we use analyzed result so parts will always be empty.
    if (merge_tree_select_result_ptr)
    {
        if (merge_tree_select_result_ptr->selected_marks == 0)
            return {};
    }
    else if (parts.empty())
        return {};

    return std::make_unique<ReadFromMergeTree>(
        parts,
        std::move(mutations_snapshot),
        column_names_to_return,
        data,
        query_info,
        storage_snapshot,
        context,
        max_block_size,
        num_streams,
        max_block_numbers_to_read,
        log,
        merge_tree_select_result_ptr,
        enable_parallel_reading
    );
}


/// Marks are placed whenever threshold on rows or bytes is met.
/// So we have to return the number of marks on whatever estimate is higher - by rows or by bytes.
size_t MergeTreeDataSelectExecutor::roundRowsOrBytesToMarks(
    size_t rows_setting,
    size_t bytes_setting,
    size_t rows_granularity,
    size_t bytes_granularity)
{
    size_t res = (rows_setting + rows_granularity - 1) / rows_granularity;

    if (bytes_granularity == 0)
        return res;
    return std::max(res, (bytes_setting + bytes_granularity - 1) / bytes_granularity);
}

/// Same as roundRowsOrBytesToMarks() but do not return more then max_marks
size_t MergeTreeDataSelectExecutor::minMarksForConcurrentRead(
    size_t rows_setting, size_t bytes_setting, size_t rows_granularity, size_t bytes_granularity, size_t min_marks, size_t max_marks)
{
    size_t marks = 1;

    if (rows_setting + rows_granularity <= rows_setting) /// overflow
        marks = max_marks;
    else if (rows_setting)
        marks = (rows_setting + rows_granularity - 1) / rows_granularity;

    if (bytes_granularity)
    {
        /// Overflow
        if (bytes_setting + bytes_granularity <= bytes_setting) /// overflow
            marks = max_marks;
        else if (bytes_setting)
            marks = std::max(marks, (bytes_setting + bytes_granularity - 1) / bytes_granularity);
    }
    return std::max(marks, min_marks);
}

/// Calculates a set of mark ranges, that could possibly contain keys, required by condition.
/// In other words, it removes subranges from whole range, that definitely could not contain required keys.
/// If @exact_ranges is not null, fill it with ranges containing marks of fully matched records.
MarkRanges MergeTreeDataSelectExecutor::markRangesFromPKRange(
    const RangesInDataPart & part_with_ranges,
    const StorageMetadataPtr & metadata_snapshot,
    const KeyCondition & key_condition,
    const std::optional<KeyCondition> & part_offset_condition,
    const std::optional<KeyCondition> & total_offset_condition,
    MarkRanges * exact_ranges,
    const Settings & settings,
    LoggerPtr log)
{
    const auto & part = part_with_ranges.data_part;
    MarkRanges res;

    size_t marks_count = part->index_granularity->getMarksCount();
    if (marks_count == 0)
        return res;

    bool has_final_mark = part->index_granularity->hasFinalMark();

    bool key_condition_useful = !key_condition.alwaysUnknownOrTrue();
    bool part_offset_condition_useful = part_offset_condition && !part_offset_condition->alwaysUnknownOrTrue();
    bool total_offset_condition_useful = total_offset_condition && !total_offset_condition->alwaysUnknownOrTrue();

    /// If index is not used.
    if (!key_condition_useful && !part_offset_condition_useful && !total_offset_condition_useful)
    {
        if (has_final_mark)
            res.push_back(MarkRange(0, marks_count - 1));
        else
            res.push_back(MarkRange(0, marks_count));

        return res;
    }

    /// If conditions are relaxed, don't fill exact ranges.
    if (key_condition.isRelaxed() || (part_offset_condition && part_offset_condition->isRelaxed())
        || (total_offset_condition && total_offset_condition->isRelaxed()))
        exact_ranges = nullptr;

    const auto & primary_key = metadata_snapshot->getPrimaryKey();
    const auto & sorting_key = metadata_snapshot->getSortingKey();
    auto index_columns = std::make_shared<ColumnsWithTypeAndName>();
    std::vector<bool> reverse_flags;
    const auto & key_indices = key_condition.getKeyIndices();
    DataTypes key_types;
    if (!key_indices.empty())
    {
        const auto index = part->getIndex();

        for (size_t i : key_indices)
        {
            if (i < index->size())
            {
                index_columns->emplace_back(index->at(i), primary_key.data_types[i], primary_key.column_names[i]);
                reverse_flags.push_back(!sorting_key.reverse_flags.empty() && sorting_key.reverse_flags[i]);
            }
            else
            {
                index_columns->emplace_back(); /// The column of the primary key was not loaded in memory - we'll skip it.
                reverse_flags.push_back(false);
            }

            key_types.emplace_back(primary_key.data_types[i]);
        }
    }

    /// If there are no monotonic functions, there is no need to save block reference.
    /// Passing explicit field to FieldRef allows to optimize ranges and shows better performance.
    std::function<void(size_t, size_t, FieldRef &)> create_field_ref;
    if (key_condition.hasMonotonicFunctionsChain())
    {
        create_field_ref = [index_columns](size_t row, size_t column, FieldRef & field)
        {
            field = {index_columns.get(), row, column};
            // NULL_LAST
            if (field.isNull())
                field = POSITIVE_INFINITY;
        };
    }
    else
    {
        create_field_ref = [index_columns](size_t row, size_t column, FieldRef & field)
        {
            (*index_columns)[column].column->get(row, field);
            // NULL_LAST
            if (field.isNull())
                field = POSITIVE_INFINITY;
        };
    }

    /// NOTE Creating temporary Field objects to pass to KeyCondition.
    size_t used_key_size = key_indices.size();
    std::vector<FieldRef> index_left(used_key_size);
    std::vector<FieldRef> index_right(used_key_size);

    /// For _part_offset and _part virtual columns
    DataTypes part_offset_types
        = {std::make_shared<DataTypeUInt64>(), std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>())};
    std::vector<FieldRef> part_offset_left(2);
    std::vector<FieldRef> part_offset_right(2);

    auto check_in_range = [&](const MarkRange & range, BoolMask initial_mask = {})
    {
        auto check_key_condition = [&]()
        {
            if (range.end == marks_count)
            {
                for (size_t i = 0; i < used_key_size; ++i)
                {
                    auto & left = reverse_flags[i] ? index_right[i] : index_left[i];
                    auto & right = reverse_flags[i] ? index_left[i] : index_right[i];
                    if ((*index_columns)[i].column)
                        create_field_ref(range.begin, i, left);
                    else
                        left = NEGATIVE_INFINITY;

                    right = POSITIVE_INFINITY;
                }
            }
            else
            {
                for (size_t i = 0; i < used_key_size; ++i)
                {
                    auto & left = reverse_flags[i] ? index_right[i] : index_left[i];
                    auto & right = reverse_flags[i] ? index_left[i] : index_right[i];
                    if ((*index_columns)[i].column)
                    {
                        create_field_ref(range.begin, i, left);
                        create_field_ref(range.end, i, right);
                    }
                    else
                    {
                        /// If the PK column was not loaded in memory - exclude it from the analysis.
                        left = NEGATIVE_INFINITY;
                        right = POSITIVE_INFINITY;
                    }
                }
            }
            return key_condition.checkInRange(used_key_size, index_left.data(), index_right.data(), key_types, initial_mask);
        };

        auto check_part_offset_condition = [&]()
        {
            auto begin = part->index_granularity->getMarkStartingRow(range.begin);
            auto end = part->index_granularity->getMarkStartingRow(range.end) - 1;
            if (begin > end)
            {
                /// Empty mark (final mark)
                return BoolMask(false, true);
            }

            part_offset_left[0] = begin;
            part_offset_right[0] = end;

            part_offset_left[1] = part->name;
            part_offset_right[1] = part->name;

            return part_offset_condition->checkInRange(
                2, part_offset_left.data(), part_offset_right.data(), part_offset_types, initial_mask);
        };

        auto check_total_offset_condition = [&]()
        {
            auto begin = part->index_granularity->getMarkStartingRow(range.begin);
            auto end = part->index_granularity->getMarkStartingRow(range.end) - 1;
            if (begin > end)
            {
                /// Empty mark (final mark)
                return BoolMask(false, true);
            }

            part_offset_left[0] = begin + part_with_ranges.part_starting_offset_in_query;
            part_offset_right[0] = end + part_with_ranges.part_starting_offset_in_query;
            return total_offset_condition->checkInRange(
                1, part_offset_left.data(), part_offset_right.data(), part_offset_types, initial_mask);
        };

        BoolMask result(true, false);

        if (key_condition_useful)
            result = result & check_key_condition();

        if (part_offset_condition_useful)
            result = result & check_part_offset_condition();

        if (total_offset_condition_useful)
            result = result & check_total_offset_condition();

        return result;
    };

    bool key_condition_exact_range = !key_condition_useful || key_condition.matchesExactContinuousRange();
    bool part_offset_condition_exact_range = !part_offset_condition_useful || part_offset_condition->matchesExactContinuousRange();
    bool total_offset_condition_exact_range = !total_offset_condition_useful || total_offset_condition->matchesExactContinuousRange();
    const String & part_name = part->isProjectionPart() ? fmt::format("{}.{}", part->name, part->getParentPart()->name) : part->name;

    if (!key_condition_exact_range || !part_offset_condition_exact_range || !total_offset_condition_exact_range)
    {
        // Do exclusion search, where we drop ranges that do not match

        if (settings[Setting::merge_tree_coarse_index_granularity] <= 1)
            throw Exception(ErrorCodes::ARGUMENT_OUT_OF_BOUND, "Setting merge_tree_coarse_index_granularity should be greater than 1");

        size_t min_marks_for_seek = roundRowsOrBytesToMarks(
            settings[Setting::merge_tree_min_rows_for_seek],
            settings[Setting::merge_tree_min_bytes_for_seek],
            part->index_granularity_info.fixed_index_granularity,
            part->index_granularity_info.index_granularity_bytes);

        /// There will always be disjoint suspicious segments on the stack, the leftmost one at the top (back).
        /// At each step, take the left segment and check if it fits.
        /// If fits, split it into smaller ones and put them on the stack. If not, discard it.
        /// If the segment is already of one mark length, add it to response and discard it.
        std::vector<MarkRange> ranges_stack = { {0, marks_count - (has_final_mark ? 1 : 0)} };

        size_t steps = 0;

        while (!ranges_stack.empty())
        {
            MarkRange range = ranges_stack.back();
            ranges_stack.pop_back();

            ++steps;

            auto result
                = check_in_range(range, exact_ranges && range.end == range.begin + 1 ? BoolMask() : BoolMask::consider_only_can_be_true);
            if (!result.can_be_true)
                continue;

            if (range.end == range.begin + 1)
            {
                /// We saw a useful gap between neighboring marks. Either add it to the last range, or start a new range.
                if (res.empty() || range.begin - res.back().end > min_marks_for_seek)
                    res.push_back(range);
                else
                    res.back().end = range.end;

                if (exact_ranges && !result.can_be_false)
                {
                    if (exact_ranges->empty() || range.begin - exact_ranges->back().end > min_marks_for_seek)
                        exact_ranges->push_back(range);
                    else
                        exact_ranges->back().end = range.end;
                }
            }
            else
            {
                /// Break the segment and put the result on the stack from right to left.
                size_t step = (range.end - range.begin - 1) / settings[Setting::merge_tree_coarse_index_granularity] + 1;
                size_t end;

                for (end = range.end; end > range.begin + step; end -= step)
                    ranges_stack.emplace_back(end - step, end);

                ranges_stack.emplace_back(range.begin, end);
            }
        }

        res.search_algorithm = MarkRanges::SearchAlgorithm::GenericExclusionSearch;
        ProfileEvents::increment(ProfileEvents::IndexGenericExclusionSearchAlgorithm);
        LOG_TRACE(
            log,
            "Used generic exclusion search {}over index for part {} with {} steps",
            exact_ranges ? "with exact ranges " : "",
            part_name,
            steps);
    }
    else
    {
        /// In case when SELECT's predicate defines a single continuous interval of keys,
        /// we can use binary search algorithm to find the left and right endpoint key marks of such interval.
        /// The returned value is the minimum range of marks, containing all keys for which KeyCondition holds

        res.search_algorithm = MarkRanges::SearchAlgorithm::BinarySearch;
        ProfileEvents::increment(ProfileEvents::IndexBinarySearchAlgorithm);
        LOG_TRACE(log, "Running binary search on index range for part {} ({} marks)", part_name, marks_count);

        size_t steps = 0;

        MarkRange result_range;

        size_t last_mark = marks_count - (has_final_mark ? 1 : 0);
        size_t searched_left = 0;
        size_t searched_right = last_mark;

        bool check_left = false;
        bool check_right = false;
        while (searched_left + 1 < searched_right)
        {
            const size_t middle = (searched_left + searched_right) / 2;
            MarkRange range(0, middle);
            if (check_in_range(range, BoolMask::consider_only_can_be_true).can_be_true)
                searched_right = middle;
            else
                searched_left = middle;
            ++steps;
            check_left = true;
        }
        result_range.begin = searched_left;
        LOG_TRACE(log, "Found (LEFT) boundary mark: {}", searched_left);

        searched_right = last_mark;
        while (searched_left + 1 < searched_right)
        {
            const size_t middle = (searched_left + searched_right) / 2;
            MarkRange range(middle, last_mark);
            if (check_in_range(range, BoolMask::consider_only_can_be_true).can_be_true)
                searched_left = middle;
            else
                searched_right = middle;
            ++steps;
            check_right = true;
        }
        result_range.end = searched_right;
        LOG_TRACE(log, "Found (RIGHT) boundary mark: {}", searched_right);

        if (result_range.begin < result_range.end)
        {
            if (exact_ranges)
            {
                if (result_range.begin + 1 == result_range.end)
                {
                    auto check_result = check_in_range(result_range);
                    if (check_result.can_be_true)
                    {
                        if (!check_result.can_be_false)
                            exact_ranges->emplace_back(result_range);
                        res.emplace_back(std::move(result_range));
                    }
                }
                else
                {
                    /// Candidate range with size > 1 is already can_be_true
                    auto result_exact_range = result_range;
                    if (check_in_range({result_range.begin, result_range.begin + 1}, BoolMask::consider_only_can_be_false).can_be_false)
                        ++result_exact_range.begin;

                    if (check_in_range({result_range.end - 1, result_range.end}, BoolMask::consider_only_can_be_false).can_be_false)
                        --result_exact_range.end;

                    if (result_exact_range.begin < result_exact_range.end)
                    {
                        chassert(check_in_range(result_exact_range, BoolMask::consider_only_can_be_false) == BoolMask(true, false));
                        exact_ranges->emplace_back(std::move(result_exact_range));
                    }

                    res.emplace_back(std::move(result_range));
                }
            }
            else
            {
                /// Candidate range with both ends checked is already can_be_true
                if ((check_left && check_right) || check_in_range(result_range, BoolMask::consider_only_can_be_true).can_be_true)
                    res.emplace_back(std::move(result_range));
            }
        }

        LOG_TRACE(
            log, "Found {} range {}in {} steps", res.empty() ? "empty" : "continuous", exact_ranges ? "with exact range " : "", steps);
    }

    return res;
}


std::pair<MarkRanges, RangesInDataPartReadHints> MergeTreeDataSelectExecutor::filterMarksUsingIndex(
    MergeTreeIndexPtr index_helper,
    MergeTreeIndexConditionPtr condition,
    MergeTreeData::DataPartPtr part,
    const MarkRanges & ranges,
    const RangesInDataPartReadHints & in_read_hints,
    const Settings & settings,
    const MergeTreeReaderSettings & reader_settings,
    MarkCache * mark_cache,
    UncompressedCache * uncompressed_cache,
    VectorSimilarityIndexCache * vector_similarity_index_cache,
    LoggerPtr log)
{
    if (!index_helper->getDeserializedFormat(part->getDataPartStorage(), index_helper->getFileName()))
    {
        LOG_DEBUG(log, "File for index {} does not exist ({}.*). Skipping it.", backQuote(index_helper->index.name),
            (fs::path(part->getDataPartStorage().getFullPath()) / index_helper->getFileName()).string());
        return {ranges, in_read_hints};
    }

    /// Whether we should use a more optimal filtering.
    bool bulk_filtering = settings[Setting::secondary_indices_enable_bulk_filtering] && index_helper->supportsBulkFiltering();

    auto index_granularity = index_helper->index.granularity;

    const size_t min_marks_for_seek = roundRowsOrBytesToMarks(
        settings[Setting::merge_tree_min_rows_for_seek],
        settings[Setting::merge_tree_min_bytes_for_seek],
        part->index_granularity_info.fixed_index_granularity,
        part->index_granularity_info.index_granularity_bytes);

    size_t marks_count = part->index_granularity->getMarksCountWithoutFinal();
    size_t index_marks_count = (marks_count + index_granularity - 1) / index_granularity;

    /// The vector similarity index can only be used if the PK did not prune some ranges within the part.
    /// (the vector index is built on the entire part).
    const bool all_match  = (marks_count == ranges.getNumberOfMarks());
    if (index_helper->isVectorSimilarityIndex() && !all_match)
    {
        return {ranges, in_read_hints};
    }

    MarkRanges index_ranges;
    for (const auto & range : ranges)
    {
        MarkRange index_range(
                range.begin / index_granularity,
                (range.end + index_granularity - 1) / index_granularity);
        index_ranges.push_back(index_range);
    }

    MergeTreeIndexReader reader(
        index_helper, part,
        index_marks_count,
        index_ranges,
        mark_cache,
        uncompressed_cache,
        vector_similarity_index_cache,
        reader_settings);

    MarkRanges res;
    size_t ranges_size = ranges.size();
    RangesInDataPartReadHints read_hints = in_read_hints;

    if (bulk_filtering)
    {
        MergeTreeIndexBulkGranulesPtr granules;

        size_t current_granule_num = 0;
        for (size_t i = 0; i < ranges_size; ++i)
        {
            const MarkRange & index_range = index_ranges[i];

            for (size_t index_mark = index_range.begin; index_mark < index_range.end; ++index_mark)
            {
                reader.read(index_mark, current_granule_num, granules);
                ++current_granule_num;
            }
        }

        IMergeTreeIndexCondition::FilteredGranules filtered_granules = condition->getPossibleGranules(granules);
        if (filtered_granules.empty())
            return {res, read_hints};

        auto it = filtered_granules.begin();
        current_granule_num = 0;
        for (size_t i = 0; i < ranges_size; ++i)
        {
            const MarkRange & index_range = index_ranges[i];

            for (size_t index_mark = index_range.begin; index_mark < index_range.end; ++index_mark)
            {
                if (current_granule_num == *it)
                {
                    MarkRange data_range(
                        std::max(ranges[i].begin, index_mark * index_granularity),
                        std::min(ranges[i].end, (index_mark + 1) * index_granularity));

                    if (res.empty() || data_range.begin - res.back().end > min_marks_for_seek)
                        res.push_back(data_range);
                    else
                        res.back().end = data_range.end;

                    ++it;
                    if (it == filtered_granules.end())
                        break;
                }

                ++current_granule_num;
            }

            if (it == filtered_granules.end())
                break;
        }
    }
    else
    {
        /// Some granules can cover two or more ranges,
        /// this variable is stored to avoid reading the same granule twice.
        MergeTreeIndexGranulePtr granule = nullptr;
        size_t last_index_mark = 0;

        PostingsCacheForStore cache_in_store;
        if (dynamic_cast<const MergeTreeIndexGin *>(index_helper.get()))
            cache_in_store.store = GinIndexStoreFactory::instance().get(index_helper->getFileName(), part->getDataPartStoragePtr());

        for (size_t i = 0; i < ranges_size; ++i)
        {
            const MarkRange & index_range = index_ranges[i];

            for (size_t index_mark = index_range.begin; index_mark < index_range.end; ++index_mark)
            {
                if (index_mark != index_range.begin || !granule || last_index_mark != index_range.begin)
                    reader.read(index_mark, granule);

                if (index_helper->isVectorSimilarityIndex())
                {
                    read_hints.vector_search_results = condition->calculateApproximateNearestNeighbors(granule);

                    /// We need to sort the result ranges ascendingly
                    auto rows = read_hints.vector_search_results.value().rows;
                    std::sort(rows.begin(), rows.end());
#ifndef NDEBUG
                    /// Duplicates should in theory not be possible but better be safe than sorry ...
                    const bool has_duplicates = std::adjacent_find(rows.begin(), rows.end()) != rows.end();
                    if (has_duplicates)
                        throw Exception(ErrorCodes::INCORRECT_DATA, "Usearch returned duplicate row numbers");
#endif
                    if (!(read_hints.vector_search_results.value().distances.has_value()))
                        read_hints = {};

                    for (auto row : rows)
                    {
                        size_t num_marks = part->index_granularity->countMarksForRows(index_mark * index_granularity, row);

                        MarkRange data_range(
                            std::max(ranges[i].begin, (index_mark * index_granularity) + num_marks),
                            std::min(ranges[i].end, (index_mark * index_granularity) + num_marks + 1));

                        if (!res.empty() && data_range.end == res.back().end)
                            /// Vector search may return >1 hit within the same granule/mark. Don't add to the result twice.
                            continue;

                        if (res.empty() || data_range.begin - res.back().end > min_marks_for_seek)
                            res.push_back(data_range);
                        else
                            res.back().end = data_range.end;
                    }
                }
                else
                {
                    bool result = false;
                    if (const auto * gin_filter_condition = dynamic_cast<const MergeTreeIndexConditionGin *>(&*condition))
                        result = cache_in_store.store ? gin_filter_condition->mayBeTrueOnGranuleInPart(granule, cache_in_store) : true;
                    else
                        result = condition->mayBeTrueOnGranule(granule);

                    if (!result)
                        continue;

                    MarkRange data_range(
                        std::max(ranges[i].begin, index_mark * index_granularity),
                        std::min(ranges[i].end, (index_mark + 1) * index_granularity));

                    if (res.empty() || data_range.begin - res.back().end > min_marks_for_seek)
                        res.push_back(data_range);
                    else
                        res.back().end = data_range.end;
                }
            }

            last_index_mark = index_range.end - 1;
        }
    }

    return {res, read_hints};
}

MarkRanges MergeTreeDataSelectExecutor::filterMarksUsingMergedIndex(
    MergeTreeIndices indices,
    MergeTreeIndexMergedConditionPtr condition,
    MergeTreeData::DataPartPtr part,
    const MarkRanges & ranges,
    const Settings & settings,
    const MergeTreeReaderSettings & reader_settings,
    MarkCache * mark_cache,
    UncompressedCache * uncompressed_cache,
    VectorSimilarityIndexCache * vector_similarity_index_cache,
    LoggerPtr log)
{
    for (const auto & index_helper : indices)
    {
        if (!part->getDataPartStorage().existsFile(index_helper->getFileName() + ".idx"))
        {
            LOG_DEBUG(log, "File for index {} does not exist. Skipping it.", backQuote(index_helper->index.name));
            return ranges;
        }
    }

    auto index_granularity = indices.front()->index.granularity;

    const size_t min_marks_for_seek = roundRowsOrBytesToMarks(
        settings[Setting::merge_tree_min_rows_for_seek],
        settings[Setting::merge_tree_min_bytes_for_seek],
        part->index_granularity_info.fixed_index_granularity,
        part->index_granularity_info.index_granularity_bytes);

    size_t marks_count = part->index_granularity->getMarksCountWithoutFinal();
    size_t index_marks_count = (marks_count + index_granularity - 1) / index_granularity;

    MarkRanges index_ranges;
    for (const auto & range : ranges)
    {
        MarkRange index_range(
                range.begin / index_granularity,
                (range.end + index_granularity - 1) / index_granularity);
        index_ranges.push_back(index_range);
    }

    std::vector<std::unique_ptr<MergeTreeIndexReader>> readers;
    for (const auto & index_helper : indices)
    {
        readers.emplace_back(
            std::make_unique<MergeTreeIndexReader>(
                index_helper,
                part,
                index_marks_count,
                index_ranges,
                mark_cache,
                uncompressed_cache,
                vector_similarity_index_cache,
                reader_settings));
    }

    MarkRanges res;

    /// Some granules can cover two or more ranges,
    /// this variable is stored to avoid reading the same granule twice.
    MergeTreeIndexGranules granules(indices.size(), nullptr);
    bool granules_filled = false;
    size_t last_index_mark = 0;
    for (const auto & range : ranges)
    {
        MarkRange index_range(
            range.begin / index_granularity,
            (range.end + index_granularity - 1) / index_granularity);

        for (size_t index_mark = index_range.begin; index_mark < index_range.end; ++index_mark)
        {
            if (index_mark != index_range.begin || !granules_filled || last_index_mark != index_range.begin)
            {
                for (size_t i = 0; i < readers.size(); ++i)
                {
                    readers[i]->read(index_mark, granules[i]);
                    granules_filled = true;
                }
            }

            if (!condition->mayBeTrueOnGranule(granules))
                continue;

            MarkRange data_range(
                std::max(range.begin, index_mark * index_granularity),
                std::min(range.end, (index_mark + 1) * index_granularity));

            if (res.empty() || data_range.begin - res.back().end > min_marks_for_seek)
                res.push_back(data_range);
            else
                res.back().end = data_range.end;
        }

        last_index_mark = index_range.end - 1;
    }

    return res;
}

void MergeTreeDataSelectExecutor::selectPartsToRead(
    RangesInDataParts & parts,
    const std::optional<std::unordered_set<String>> & part_values,
    const std::optional<KeyCondition> & minmax_idx_condition,
    const DataTypes & minmax_columns_types,
    const std::optional<PartitionPruner> & partition_pruner,
    const PartitionIdToMaxBlock * max_block_numbers_to_read,
    PartFilterCounters & counters,
    QueryStatusPtr query_status)
{
    RangesInDataParts prev_parts;
    std::swap(prev_parts, parts);

    for (const auto & prev_part : prev_parts)
    {
        const auto & part_or_projection = prev_part.data_part;

        if (query_status)
            query_status->checkTimeLimit();

        fiu_do_on(FailPoints::slowdown_index_analysis,
        {
            sleepForMilliseconds(1000);
        });

        const auto * part = part_or_projection->isProjectionPart() ? part_or_projection->getParentPart() : part_or_projection.get();
        if (part_values && part_values->find(part->name) == part_values->end())
            continue;

        if (part->isEmpty())
            continue;

        if (max_block_numbers_to_read)
        {
            auto blocks_iterator = max_block_numbers_to_read->find(part->info.getPartitionId());
            if (blocks_iterator == max_block_numbers_to_read->end() || part->info.max_block > blocks_iterator->second)
                continue;
        }

        size_t num_granules = part->index_granularity->getMarksCountWithoutFinal();

        counters.num_initial_selected_parts += 1;
        counters.num_initial_selected_granules += num_granules;

        if (minmax_idx_condition && !minmax_idx_condition->checkInHyperrectangle(
                part->minmax_idx->hyperrectangle, minmax_columns_types).can_be_true)
            continue;

        counters.num_parts_after_minmax += 1;
        counters.num_granules_after_minmax += num_granules;

        if (partition_pruner)
        {
            if (partition_pruner->canBePruned(*part))
                continue;
        }

        counters.num_parts_after_partition_pruner += 1;
        counters.num_granules_after_partition_pruner += num_granules;

        parts.push_back(prev_part);
    }
}

void MergeTreeDataSelectExecutor::selectPartsToReadWithUUIDFilter(
    RangesInDataParts & parts,
    const std::optional<std::unordered_set<String>> & part_values,
    MergeTreeData::PinnedPartUUIDsPtr pinned_part_uuids,
    const std::optional<KeyCondition> & minmax_idx_condition,
    const DataTypes & minmax_columns_types,
    const std::optional<PartitionPruner> & partition_pruner,
    const PartitionIdToMaxBlock * max_block_numbers_to_read,
    ContextPtr query_context,
    PartFilterCounters & counters,
    LoggerPtr log)
{
    /// process_parts prepare parts that have to be read for the query,
    /// returns false if duplicated parts' UUID have been met
    auto select_parts = [&](RangesInDataParts & selected_parts) -> bool
    {
        auto ignored_part_uuids = query_context->getIgnoredPartUUIDs();
        std::unordered_set<UUID> temp_part_uuids;

        RangesInDataParts prev_parts;
        std::swap(prev_parts, selected_parts);

        for (const auto & prev_part : prev_parts)
        {
            const auto & part_or_projection = prev_part.data_part;
            const auto * part = part_or_projection->isProjectionPart() ? part_or_projection->getParentPart() : part_or_projection.get();
            if (part_values && part_values->find(part->name) == part_values->end())
                continue;

            if (part->isEmpty())
                continue;

            if (max_block_numbers_to_read)
            {
                auto blocks_iterator = max_block_numbers_to_read->find(part->info.getPartitionId());
                if (blocks_iterator == max_block_numbers_to_read->end() || part->info.max_block > blocks_iterator->second)
                    continue;
            }

            /// Skip the part if its uuid is meant to be excluded
            if (part->uuid != UUIDHelpers::Nil && ignored_part_uuids->has(part->uuid))
                continue;

            size_t num_granules = part->index_granularity->getMarksCountWithoutFinal();

            counters.num_initial_selected_parts += 1;
            counters.num_initial_selected_granules += num_granules;

            if (minmax_idx_condition
                && !minmax_idx_condition->checkInHyperrectangle(part->minmax_idx->hyperrectangle, minmax_columns_types)
                        .can_be_true)
                continue;

            counters.num_parts_after_minmax += 1;
            counters.num_granules_after_minmax += num_granules;

            if (partition_pruner)
            {
                if (partition_pruner->canBePruned(*part))
                    continue;
            }

            counters.num_parts_after_partition_pruner += 1;
            counters.num_granules_after_partition_pruner += num_granules;

            /// populate UUIDs and exclude ignored parts if enabled
            if (part->uuid != UUIDHelpers::Nil && pinned_part_uuids->contains(part->uuid))
            {
                auto result = temp_part_uuids.insert(part->uuid);
                if (!result.second)
                    throw Exception(ErrorCodes::LOGICAL_ERROR, "Found a part with the same UUID on the same replica.");
            }

            selected_parts.push_back(prev_part);
        }

        if (!temp_part_uuids.empty())
        {
            auto duplicates = query_context->getPartUUIDs()->add(std::vector<UUID>{temp_part_uuids.begin(), temp_part_uuids.end()});
            if (!duplicates.empty())
            {
                /// on a local replica with prefer_localhost_replica=1 if any duplicates appeared during the first pass,
                /// adding them to the exclusion, so they will be skipped on second pass
                query_context->getIgnoredPartUUIDs()->add(duplicates);
                return false;
            }
        }

        return true;
    };

    /// Process parts that have to be read for a query.
    auto needs_retry = !select_parts(parts);

    /// If any duplicated part UUIDs met during the first step, try to ignore them in second pass.
    /// This may happen when `prefer_localhost_replica` is set and "distributed" stage runs in the same process with "remote" stage.
    if (needs_retry)
    {
        LOG_DEBUG(log, "Found duplicate uuids locally, will retry part selection without them");

        counters = PartFilterCounters();

        /// Second attempt didn't help, throw an exception
        if (!select_parts(parts))
            throw Exception(ErrorCodes::DUPLICATED_PART_UUIDS, "Found duplicate UUIDs while processing query.");
    }
}

}
