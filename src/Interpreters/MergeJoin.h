#pragma once

#include <Common/SharedMutex.h>
#include <Common/CacheBase.h>
#include <Core/Block.h>
#include <Core/SortDescription.h>
#include <Interpreters/IJoin.h>
#include <Interpreters/SortedBlocksWriter.h>
#include <QueryPipeline/SizeLimits.h>

namespace DB
{

class TableJoin;
class MergeJoinCursor;
struct MergeJoinEqualRange;
class RowBitmaps;
enum class JoinTableSide : uint8_t;

class MergeJoin : public IJoin
{
public:
    MergeJoin(std::shared_ptr<TableJoin> table_join_, SharedHeader right_sample_block);

    struct NotProcessed
    {
        Block block;
        size_t left_position;
        size_t left_key_tail;
        size_t right_position;
        size_t right_block;

        bool empty() const { return block.empty(); }
    };

    std::string getName() const override { return "PartialMergeJoin"; }
    const TableJoin & getTableJoin() const override { return *table_join; }
    bool addBlockToJoin(const Block & block, bool check_limits) override;
    void checkTypesOfKeys(const Block & block) const override;
    JoinResultPtr joinBlock(Block block) override;
    void joinBlock(Block & block, std::optional<MergeJoin::NotProcessed> & not_processed);

    void setTotals(const Block &) override;

    size_t getTotalRowCount() const override { return right_blocks.row_count; }
    size_t getTotalByteCount() const override { return right_blocks.bytes; }
    /// Has to be called only after setTotals()/mergeRightBlocks()
    bool alwaysReturnsEmptySet() const override { return (is_right || is_inner) && min_max_right_blocks.empty(); }

    IBlocksStreamPtr getNonJoinedBlocks(const Block & left_sample_block, const Block & result_sample_block, UInt64 max_block_size) const override;

    static bool isSupported(const std::shared_ptr<TableJoin> & table_join);

private:
    friend class NotJoinedMerge;

    struct RightBlockInfo
    {
        std::shared_ptr<Block> block;
        size_t block_number;
        size_t & skip;
        RowBitmaps * bitmaps;
        std::unique_ptr<std::vector<bool>> used_bitmap;

        RightBlockInfo(std::shared_ptr<Block> block_, size_t block_number, size_t & skip_, RowBitmaps * bitmaps);
        ~RightBlockInfo(); /// apply used bitmap
        void setUsed(size_t start, size_t length);
    };

    /// There're two size limits for right-hand table: max_rows_in_join, max_bytes_in_join.
    /// max_bytes is preferred. If it isn't set we approximate it as (max_rows * bytes/row).
    struct BlockByteWeight
    {
        size_t operator()(const Block & block) const { return block.bytes(); }
    };

    using Cache = CacheBase<size_t, Block, std::hash<size_t>, BlockByteWeight>;

    mutable SharedMutex rwlock;
    std::shared_ptr<TableJoin> table_join;
    SizeLimits size_limits;
    SortDescription left_sort_description;
    SortDescription right_sort_description;
    SortDescription left_merge_description;
    SortDescription right_merge_description;
    Block right_sample_block;
    Block right_table_keys;
    /// Columns from right side of join, both key and additional
    Block right_columns_to_add;
    SortedBlocksWriter::Blocks right_blocks;

    Names key_names_right;
    Names key_names_left;

    /// Additional conditions for rows to join from JOIN ON section.
    /// Only rows where conditions are met can be joined.
    String mask_column_name_left;
    String mask_column_name_right;

    /// Each block stores first and last row from corresponding sorted block on disk
    Blocks min_max_right_blocks;
    std::shared_ptr<SortedBlocksBuffer> left_blocks_buffer;
    std::shared_ptr<RowBitmaps> used_rows_bitmap;
    mutable std::unique_ptr<Cache> cached_right_blocks;
    std::vector<std::shared_ptr<Block>> loaded_right_blocks;
    std::unique_ptr<SortedBlocksWriter> disk_writer;
    /// Set of files with sorted blocks
    SortedBlocksWriter::SortedFiles flushed_right_blocks;
    std::atomic<bool> is_in_memory{true};
    const bool is_any_join;
    const bool is_all_join;
    const bool is_semi_join;
    const bool is_inner;
    const bool is_left;
    const bool is_right;
    const bool is_full;
    static constexpr const bool skip_not_intersected = true; /// skip index for right blocks
    const size_t max_joined_block_rows;
    const size_t max_rows_in_right_block;
    const size_t max_files_to_merge;

    Names lowcard_right_keys;

    LoggerPtr log;

    void changeLeftColumns(Block & block, MutableColumns && columns) const;
    void addRightColumns(Block & block, MutableColumns && columns);

    template <bool is_all>
    std::optional<NotProcessed> extraBlock(Block & processed, MutableColumns && left_columns, MutableColumns && right_columns,
                             size_t left_position, size_t left_key_tail, size_t right_position,
                             size_t right_block_number);

    void mergeRightBlocks();

    template <bool in_memory>
    size_t rightBlocksCount() const;
    template <bool in_memory, bool is_all>
    void joinSortedBlock(Block & block, std::optional<NotProcessed> & not_processed);
    template <bool in_memory>
    std::shared_ptr<Block> loadRightBlock(size_t pos) const;

    std::shared_ptr<Block> getRightBlock(size_t pos) const
    {
        if (is_in_memory)
            return loadRightBlock<true>(pos);
        return loadRightBlock<false>(pos);
    }

    size_t getRightBlocksCount() const
    {
        if (is_in_memory)
            return rightBlocksCount<true>();
        return rightBlocksCount<false>();
    }

    template <bool is_all> /// ALL or ANY
    bool leftJoin(MergeJoinCursor & left_cursor, const Block & left_block, RightBlockInfo & right_block_info,
                  MutableColumns & left_columns, MutableColumns & right_columns, size_t & left_key_tail);
    bool semiLeftJoin(MergeJoinCursor & left_cursor, const Block & left_block, const RightBlockInfo & right_block_info,
                  MutableColumns & left_columns, MutableColumns & right_columns);
    bool allInnerJoin(MergeJoinCursor & left_cursor, const Block & left_block, RightBlockInfo & right_block_info,
                  MutableColumns & left_columns, MutableColumns & right_columns, size_t & left_key_tail);

    Block modifyRightBlock(const Block & src_block) const;
    bool saveRightBlock(Block && block);

    void mergeInMemoryRightBlocks();
    void mergeFlushedRightBlocks();

    void initRightTableWriter();

    bool needConditionJoinColumn() const;
    void addConditionJoinColumn(Block & block, JoinTableSide block_side) const;
};

}
