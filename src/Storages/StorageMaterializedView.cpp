#include <Storages/StorageMaterializedView.h>

#include <Storages/MaterializedView/RefreshTask.h>

#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTCreateQuery.h>

#include <Interpreters/Context.h>
#include <Interpreters/InterpreterCreateQuery.h>
#include <Interpreters/InterpreterDropQuery.h>
#include <Interpreters/InterpreterRenameQuery.h>
#include <Interpreters/InterpreterInsertQuery.h>
#include <Interpreters/InterpreterSelectWithUnionQuery.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Interpreters/getTableExpressions.h>
#include <Interpreters/getHeaderForProcessingStage.h>
#include <Access/Common/AccessFlags.h>

#include <Storages/AlterCommands.h>
#include <Storages/ReadInOrderOptimizer.h>
#include <Storages/SelectQueryDescription.h>
#include <Storages/StorageFactory.h>

#include <Common/typeid_cast.h>
#include <Common/checkStackSize.h>
#include <Core/ServerSettings.h>
#include <QueryPipeline/Pipe.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/QueryPlan/BuildQueryPipelineSettings.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <Processors/Sinks/SinkToStorage.h>

#include <Backups/BackupEntriesCollector.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int NOT_IMPLEMENTED;
    extern const int INCORRECT_QUERY;
    extern const int TOO_MANY_MATERIALIZED_VIEWS;
    extern const int LOGICAL_ERROR;
    extern const int UNKNOWN_STORAGE;
}

namespace ActionLocks
{
    extern const StorageActionBlockType ViewRefresh;
}

static inline String generateInnerTableName(const StorageID & view_id, bool scratch)
{
    String res = ".inner";
    if (scratch)
        res += "_scratch";
    if (view_id.hasUUID())
        res += "_id." + toString(view_id.uuid);
    else
        res += "." + view_id.getTableName();
    return res;
}

/// Remove columns from target_header that does not exists in src_header
static void removeNonCommonColumns(const Block & src_header, Block & target_header)
{
    std::set<size_t> target_only_positions;
    for (const auto & column : target_header)
    {
        if (!src_header.has(column.name))
            target_only_positions.insert(target_header.getPositionByName(column.name));
    }
    target_header.erase(target_only_positions);
}

StorageMaterializedView::StorageMaterializedView(
    const StorageID & table_id_,
    ContextPtr local_context,
    const ASTCreateQuery & query,
    const ColumnsDescription & columns_,
    bool attach_,
    const String & comment)
    : IStorage(table_id_), WithMutableContext(local_context->getGlobalContext())
{
    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(columns_);

    if (!query.select)
        throw Exception(ErrorCodes::INCORRECT_QUERY, "SELECT query is not specified for {}", getName());

    /// If the destination table is not set, use inner table
    has_inner_target_table = query.needsInnerTargetTable();
    if (has_inner_target_table && !query.storage)
        throw Exception(ErrorCodes::INCORRECT_QUERY,
                        "You must specify where to save results of a MaterializedView query: "
                        "either ENGINE or an existing table in a TO clause");

    has_scratch_table = query.needsScratchTable();

    auto select = SelectQueryDescription::getSelectQueryFromASTForMatView(query.select->clone(), query.refresh_strategy != nullptr, local_context);
    if (select.select_table_id)
    {
        auto select_table_dependent_views = DatabaseCatalog::instance().getDependentViews(select.select_table_id);

        auto max_materialized_views_count_for_table = getContext()->getServerSettings().max_materialized_views_count_for_table;
        if (max_materialized_views_count_for_table && select_table_dependent_views.size() >= max_materialized_views_count_for_table)
            throw Exception(ErrorCodes::TOO_MANY_MATERIALIZED_VIEWS,
                            "Too many materialized views, maximum: {}", max_materialized_views_count_for_table);
    }

    storage_metadata.setSelectQuery(select);
    if (!comment.empty())
        storage_metadata.setComment(comment);
    if (query.refresh_strategy)
        storage_metadata.setRefresh(query.refresh_strategy->clone());

    setInMemoryMetadata(storage_metadata);

    bool point_to_itself_by_uuid = std::any_of(query.to_inner_uuid.begin(), query.to_inner_uuid.end(), [&](const UUID & uuid) { return uuid == table_id_.uuid; });
    bool point_to_itself_by_name = query.to_table_id.database_name == table_id_.database_name
                                && query.to_table_id.table_name == table_id_.table_name;
    if (point_to_itself_by_uuid || point_to_itself_by_name)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Materialized view {} cannot point to itself", table_id_.getFullTableName());

    StorageID mv_storage_id = getStorageID();

    if (has_inner_target_table)
        target_table_id = StorageID(mv_storage_id.database_name, generateInnerTableName(mv_storage_id, false));
    else
        target_table_id = query.to_table_id;

    if (has_scratch_table)
        scratch_table_id = StorageID(mv_storage_id.database_name, generateInnerTableName(mv_storage_id, true));

    if (!query.to_inner_uuid.empty())
    {
        if (query.to_inner_uuid.size() != query.needsInnerTables())
            throw Exception(ErrorCodes::INCORRECT_QUERY, "Materialized view needs {} inner tables, but TO INNER UUID contains {} uuids", query.needsInnerTables(), query.to_inner_uuid.size());

        if (has_inner_target_table)
            target_table_id.uuid = query.to_inner_uuid[0];
        if (has_scratch_table)
            scratch_table_id.uuid = query.to_inner_uuid.back();
    }

    /// Prepare to create internal tables, if needed.
    std::shared_ptr<ASTCreateQuery> inner_target_create_query;
    std::shared_ptr<ASTCreateQuery> scratch_create_query;

    if (!attach_)
    {
        if (has_inner_target_table)
        {
            inner_target_create_query = std::make_shared<ASTCreateQuery>();

            auto new_columns_list = std::make_shared<ASTColumns>();
            new_columns_list->set(new_columns_list->columns, query.columns_list->columns->ptr());

            inner_target_create_query->set(inner_target_create_query->columns_list, new_columns_list);
            inner_target_create_query->set(inner_target_create_query->storage, query.storage->ptr());
        }

        if (has_scratch_table)
        {
            /// Scratch table's column list and engine must match the target table.
            ASTPtr query_ptr;
            if (inner_target_create_query)
                query_ptr = inner_target_create_query->clone();
            else
            {
                auto db = DatabaseCatalog::instance().getDatabase(target_table_id.database_name);
                query_ptr = db->getCreateTableQuery(target_table_id.table_name, getContext());
            }

            scratch_create_query = std::static_pointer_cast<ASTCreateQuery>(query_ptr);
            scratch_table_is_known_to_be_empty = true;

            if (scratch_create_query->is_dictionary || scratch_create_query->is_ordinary_view ||
                scratch_create_query->is_materialized_view || scratch_create_query->is_live_view ||
                scratch_create_query->is_window_view)
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Materialized view can only write to a regular table, not dictionary or view");

            const ASTStorage * storage = scratch_create_query->storage;
            if (!storage || !storage->engine)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Target table create query missing ENGINE");
            auto features = StorageFactory::instance().tryGetFeatures(storage->engine->name);
            if (!features)
                throw Exception(ErrorCodes::UNKNOWN_STORAGE, "Unknown engine for target table: {}", storage->engine->name);

            if (!features->supports_moving_data_between_tables)
                throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                    "Engine {} doesn't support moving data between tables as required by refreshable materialized view (without APPEND). Only the following engines support it: {}",
                    storage->engine->name,
                    StorageFactory::instance().getAllRegisteredNamesByFeatureMatcherFn([](const auto & f) { return f.supports_moving_data_between_tables; }));

            if (storage->partition_by)
                throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                    "Refreshable materialized views (without APPEND) don't support partitioned tables.");

            if (features->supports_replication)
                /// TODO: Enable coordinated refreshing in this case.
                throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                    "Refreshable materialized views (without APPEND) don't support replicated tables yet.");
        }
    }

    if (query.refresh_strategy)
    {
        chassert(has_scratch_table == !query.refresh_strategy->append);
        refresher = RefreshTask::create(this, getContext(), *query.refresh_strategy);
        refresh_on_start = !attach_ && !query.is_create_empty;
    }

    std::vector<StorageID> created_inner_tables;

    auto execute_inner_create = [&](std::shared_ptr<ASTCreateQuery> create_query, StorageID & id)
    {
        if (!create_query)
            return;
        create_query->setDatabase(id.database_name);
        create_query->setTable(id.table_name);
        create_query->uuid = id.uuid;

        auto create_context = Context::createCopy(local_context);
        InterpreterCreateQuery create_interpreter(create_query, create_context);
        create_interpreter.setInternal(true);
        create_interpreter.execute();

        id = DatabaseCatalog::instance().getTable(id, getContext())->getStorageID();
        created_inner_tables.push_back(id);
    };

    try
    {
        execute_inner_create(inner_target_create_query, target_table_id);
        execute_inner_create(scratch_create_query, scratch_table_id);
    }
    catch (...)
    {
        /// If we created one table but failed to create the other, try to drop it.
        for (const StorageID & id : created_inner_tables)
        {
            try
            {
                bool may_lock_ddl_guard = mv_storage_id.getQualifiedName() < id.getQualifiedName();
                InterpreterDropQuery::executeDropQuery(
                    ASTDropQuery::Kind::Drop, getContext(), local_context, id,
                    /*sync*/ false, /* ignore_sync_setting */ true, may_lock_ddl_guard);
            }
            catch (...)
            {
                tryLogCurrentException(&Poco::Logger::get("StorageMaterializedView"), "Failed to un-create inner table");
            }
        }
        throw;
    }
}

QueryProcessingStage::Enum StorageMaterializedView::getQueryProcessingStage(
    ContextPtr local_context,
    QueryProcessingStage::Enum to_stage,
    const StorageSnapshotPtr &,
    SelectQueryInfo & query_info) const
{
    StoragePtr storage = getTargetTable();
    const auto & target_metadata = storage->getInMemoryMetadataPtr();
    return storage->getQueryProcessingStage(local_context, to_stage, storage->getStorageSnapshot(target_metadata, local_context), query_info);
}

void StorageMaterializedView::read(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr local_context,
    QueryProcessingStage::Enum processed_stage,
    const size_t max_block_size,
    const size_t num_streams)
{
    auto storage = getTargetTable();
    auto lock = storage->lockForShare(local_context->getCurrentQueryId(), local_context->getSettingsRef().lock_acquire_timeout);
    auto target_metadata_snapshot = storage->getInMemoryMetadataPtr();
    auto target_storage_snapshot = storage->getStorageSnapshot(target_metadata_snapshot, local_context);

    if (query_info.order_optimizer)
        query_info.input_order_info = query_info.order_optimizer->getInputOrder(target_metadata_snapshot, local_context);

    storage->read(query_plan, column_names, target_storage_snapshot, query_info, local_context, processed_stage, max_block_size, num_streams);

    if (query_plan.isInitialized())
    {
        auto mv_header = getHeaderForProcessingStage(column_names, storage_snapshot, query_info, local_context, processed_stage);
        auto target_header = query_plan.getCurrentDataStream().header;

        /// No need to convert columns that do not exists in MV
        removeNonCommonColumns(mv_header, target_header);

        /// No need to convert columns that does not exists in the result header.
        ///
        /// Distributed storage may process query up to the specific stage, and
        /// so the result header may not include all the columns from the
        /// materialized view.
        removeNonCommonColumns(target_header, mv_header);

        if (!blocksHaveEqualStructure(mv_header, target_header))
        {
            auto converting_actions = ActionsDAG::makeConvertingActions(target_header.getColumnsWithTypeAndName(),
                                                                        mv_header.getColumnsWithTypeAndName(),
                                                                        ActionsDAG::MatchColumnsMode::Name);
            /* Leave columns outside from materialized view structure as is.
             * They may be added in case of distributed query with JOIN.
             * In that case underlying table returns joined columns as well.
             */
            converting_actions->projectInput(false);
            auto converting_step = std::make_unique<ExpressionStep>(query_plan.getCurrentDataStream(), converting_actions);
            converting_step->setStepDescription("Convert target table structure to MaterializedView structure");
            query_plan.addStep(std::move(converting_step));
        }

        query_plan.addStorageHolder(storage);
        query_plan.addTableLock(std::move(lock));
    }
}

SinkToStoragePtr StorageMaterializedView::write(const ASTPtr & query, const StorageMetadataPtr & /*metadata_snapshot*/, ContextPtr local_context, bool async_insert)
{
    auto storage = getTargetTable();
    auto lock = storage->lockForShare(local_context->getCurrentQueryId(), local_context->getSettingsRef().lock_acquire_timeout);

    auto metadata_snapshot = storage->getInMemoryMetadataPtr();
    auto sink = storage->write(query, metadata_snapshot, local_context, async_insert);

    sink->addTableLock(lock);
    return sink;
}


void StorageMaterializedView::drop()
{
    auto table_id = getStorageID();
    const auto & select_query = getInMemoryMetadataPtr()->getSelectQuery();
    if (!select_query.select_table_id.empty())
        DatabaseCatalog::instance().removeViewDependency(select_query.select_table_id, table_id);

    /// Sync flag and the setting make sense for Atomic databases only.
    /// However, with Atomic databases, IStorage::drop() can be called only from a background task in DatabaseCatalog.
    /// Running synchronous DROP from that task leads to deadlock.
    /// Usually dropInnerTableIfAny is no-op, because the inner table is dropped before enqueueing a drop task for the MV itself.
    /// But there's a race condition with SYSTEM RESTART REPLICA: the inner table might be detached due to RESTART.
    /// In this case, dropInnerTableIfAny will not find the inner table and will not drop it during executions of DROP query for the MV itself.
    /// DDLGuard does not protect from that, because RESTART REPLICA acquires DDLGuard for the inner table name,
    /// but DROP acquires DDLGuard for the name of MV. And we cannot acquire second DDLGuard for the inner name in DROP,
    /// because it may lead to lock-order-inversion (DDLGuards must be acquired in lexicographical order).
    dropInnerTableIfAny(/* sync */ false, getContext());
}

void StorageMaterializedView::dropInnerTableIfAny(bool sync, ContextPtr local_context)
{
    auto drop = [&](StorageID inner_table_id)
    {
        /// We will use `sync` argument wneh this function is called from a DROP query
        /// and will ignore database_atomic_wait_for_drop_and_detach_synchronously when it's called from drop task.
        /// See the comment in StorageMaterializedView::drop.
        /// DDL queries with StorageMaterializedView are fundamentally broken.
        /// Best-effort to make them work: the inner table name is almost always less than the MV name (so it's safe to lock DDLGuard)
        bool may_lock_ddl_guard = getStorageID().getQualifiedName() < inner_table_id.getQualifiedName();
        if (DatabaseCatalog::instance().tryGetTable(inner_table_id, getContext()))
            InterpreterDropQuery::executeDropQuery(
                ASTDropQuery::Kind::Drop, getContext(), local_context, inner_table_id,
                sync, /* ignore_sync_setting */ true, may_lock_ddl_guard);
    };

    if (has_inner_target_table)
        drop(getTargetTableId());
    if (has_scratch_table)
        drop(getScratchTableId());
}

void StorageMaterializedView::truncate(const ASTPtr &, const StorageMetadataPtr &, ContextPtr local_context, TableExclusiveLockHolder &)
{
    if (has_inner_target_table)
        InterpreterDropQuery::executeDropQuery(ASTDropQuery::Kind::Truncate, getContext(), local_context, getTargetTableId(), true);
}

void StorageMaterializedView::checkStatementCanBeForwarded() const
{
    if (!has_inner_target_table)
        throw Exception(ErrorCodes::INCORRECT_QUERY, "MATERIALIZED VIEW targets existing table {}. "
            "Execute the statement directly on it.", getTargetTableId().getNameForLogs());
}

bool StorageMaterializedView::optimize(
    const ASTPtr & query,
    const StorageMetadataPtr & /*metadata_snapshot*/,
    const ASTPtr & partition,
    bool final,
    bool deduplicate,
    const Names & deduplicate_by_columns,
    bool cleanup,
    ContextPtr local_context)
{
    checkStatementCanBeForwarded();
    auto storage_ptr = getTargetTable();
    auto metadata_snapshot = storage_ptr->getInMemoryMetadataPtr();
    return storage_ptr->optimize(query, metadata_snapshot, partition, final, deduplicate, deduplicate_by_columns, cleanup, local_context);
}

ContextMutablePtr StorageMaterializedView::createRefreshContext() const
{
    auto refresh_context = Context::createCopy(getContext());
    refresh_context->setQueryKind(ClientInfo::QueryKind::INITIAL_QUERY);
    /// Generate a random query id.
    refresh_context->setCurrentQueryId("");
    /// TODO: Set view's definer as the current user in refresh_context, so that the correct user's
    ///       quotas and permissions apply for this query.
    return refresh_context;
}

std::shared_ptr<ASTInsertQuery> StorageMaterializedView::prepareRefresh(ContextMutablePtr refresh_context)
{
    StorageID target = has_scratch_table ? getScratchTableId() : getTargetTableId();

    if (has_scratch_table && !scratch_table_is_known_to_be_empty)
    {
        StoragePtr scratch = getScratchTable();
        auto lock = scratch->lockExclusively(refresh_context->getCurrentQueryId(), refresh_context->getSettingsRef().lock_acquire_timeout);
        auto metadata_snapshot = scratch->getInMemoryMetadataPtr();
        auto truncate_query = std::make_shared<ASTDropQuery>();
        truncate_query->kind = ASTDropQuery::Kind::Truncate;
        scratch->truncate(truncate_query, metadata_snapshot, refresh_context, lock);
    }

    auto insert_query = std::make_shared<ASTInsertQuery>();
    insert_query->select = getInMemoryMetadataPtr()->getSelectQuery().select_query;
    insert_query->setTable(target.table_name);
    insert_query->setDatabase(target.database_name);
    insert_query->table_id = target;

    Block header;
    if (refresh_context->getSettingsRef().allow_experimental_analyzer)
        header = InterpreterSelectQueryAnalyzer::getSampleBlock(insert_query->select, refresh_context);
    else
        header = InterpreterSelectWithUnionQuery(insert_query->select, refresh_context, SelectQueryOptions()).getSampleBlock();

    auto columns = std::make_shared<ASTExpressionList>(',');
    for (const String & name : header.getNames())
        columns->children.push_back(std::make_shared<ASTIdentifier>(name));
    insert_query->columns = std::move(columns);

    scratch_table_is_known_to_be_empty = false;
    return insert_query;
}

void StorageMaterializedView::transferRefreshedData(ContextPtr refresh_context)
{
    if (!has_scratch_table)
        return;

    getTargetTable()->transferAllDataFrom(getScratchTable(), /*remove_from_source*/ true, /*replace_at_destination*/ true, refresh_context);

    scratch_table_is_known_to_be_empty = true;
}

void StorageMaterializedView::alter(
    const AlterCommands & params,
    ContextPtr local_context,
    AlterLockHolder &)
{
    auto table_id = getStorageID();
    StorageInMemoryMetadata new_metadata = getInMemoryMetadata();
    StorageInMemoryMetadata old_metadata = getInMemoryMetadata();
    params.apply(new_metadata, local_context);

    /// start modify query
    const auto & new_select = new_metadata.select;
    const auto & old_select = old_metadata.getSelectQuery();

    DatabaseCatalog::instance().updateViewDependency(old_select.select_table_id, table_id, new_select.select_table_id, table_id);
    /// end modify query

    DatabaseCatalog::instance().getDatabase(table_id.database_name)->alterTable(local_context, table_id, new_metadata);
    setInMemoryMetadata(new_metadata);

    if (refresher)
        refresher->alterRefreshParams(new_metadata.refresh->as<const ASTRefreshStrategy &>());
}


void StorageMaterializedView::checkAlterIsPossible(const AlterCommands & commands, ContextPtr /*local_context*/) const
{
    for (const auto & command : commands)
    {
        if (command.isCommentAlter())
            continue;
        if (command.type == AlterCommand::MODIFY_QUERY)
            continue;
        if (command.type == AlterCommand::MODIFY_REFRESH)
        {
            if (!refresher)
                throw Exception(ErrorCodes::NOT_IMPLEMENTED, "MODIFY REFRESH is not supported by non-refreshable materialized views");
            if (command.refresh->as<const ASTRefreshStrategy &>().append != !has_scratch_table)
                throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Adding/removing APPEND is not supported by refreshable materialized views");
            continue;
        }
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Alter of type '{}' is not supported by storage {}",
            command.type, getName());
    }
}

void StorageMaterializedView::checkMutationIsPossible(const MutationCommands & commands, const Settings & settings) const
{
    checkStatementCanBeForwarded();
    getTargetTable()->checkMutationIsPossible(commands, settings);
}

Pipe StorageMaterializedView::alterPartition(
    const StorageMetadataPtr & metadata_snapshot, const PartitionCommands & commands, ContextPtr local_context)
{
    checkStatementCanBeForwarded();
    return getTargetTable()->alterPartition(metadata_snapshot, commands, local_context);
}

void StorageMaterializedView::checkAlterPartitionIsPossible(
    const PartitionCommands & commands, const StorageMetadataPtr & metadata_snapshot,
    const Settings & settings, ContextPtr local_context) const
{
    checkStatementCanBeForwarded();
    getTargetTable()->checkAlterPartitionIsPossible(commands, metadata_snapshot, settings, local_context);
}

void StorageMaterializedView::mutate(const MutationCommands & commands, ContextPtr local_context)
{
    checkStatementCanBeForwarded();
    getTargetTable()->mutate(commands, local_context);
}

void StorageMaterializedView::renameInMemory(const StorageID & new_table_id)
{
    auto old_table_id = getStorageID();
    auto metadata_snapshot = getInMemoryMetadataPtr();

    bool rename_inner_tables = new_table_id.database_name != old_table_id.database_name || !old_table_id.hasUUID() || !new_table_id.hasUUID();
    if (rename_inner_tables)
    {
        auto rename = std::make_shared<ASTRenameQuery>();

        std::optional<String> new_target_table_name;
        std::optional<String> new_scratch_table_name;
        if (has_inner_target_table && tryGetTargetTable())
        {
            auto inner_table_id = getTargetTableId();
            chassert(inner_table_id.database_name == old_table_id.database_name);
            new_target_table_name = generateInnerTableName(new_table_id, /*scratch*/ false);
            rename->addElement(inner_table_id.database_name, inner_table_id.table_name, new_table_id.database_name, new_target_table_name.value());
        }
        if (has_scratch_table)
        {
            auto inner_table_id = getScratchTableId();
            chassert(inner_table_id.database_name == old_table_id.database_name);
            new_scratch_table_name = generateInnerTableName(new_table_id, /*scratch*/ true);
            rename->addElement(inner_table_id.database_name, inner_table_id.table_name, new_table_id.database_name, new_scratch_table_name.value());
        }

        InterpreterRenameQuery(rename, getContext()).execute();

        {
            std::lock_guard guard(inner_table_ids_mutex);
            if (new_target_table_name)
            {
                target_table_id.database_name = new_table_id.database_name;
                target_table_id.table_name = new_target_table_name.value();
            }
            if (new_scratch_table_name)
            {
                scratch_table_id.database_name = new_table_id.database_name;
                scratch_table_id.table_name = new_scratch_table_name.value();
            }
        }
    }

    IStorage::renameInMemory(new_table_id);

    const auto & select_query = metadata_snapshot->getSelectQuery();
    // TODO Actually we don't need to update dependency if MV has UUID, but then db and table name will be outdated
    DatabaseCatalog::instance().updateViewDependency(select_query.select_table_id, old_table_id, select_query.select_table_id, getStorageID());

    if (refresher)
        refresher->rename(new_table_id);
}

void StorageMaterializedView::startup()
{
    auto metadata_snapshot = getInMemoryMetadataPtr();
    const auto & select_query = metadata_snapshot->getSelectQuery();
    if (!select_query.select_table_id.empty())
        DatabaseCatalog::instance().addViewDependency(select_query.select_table_id, getStorageID());

    if (refresher)
    {
        refresher->initializeAndStart();

        if (refresh_on_start)
            refresher->run();
    }
}

void StorageMaterializedView::shutdown(bool)
{
    if (refresher)
        refresher->shutdown();

    auto metadata_snapshot = getInMemoryMetadataPtr();
    const auto & select_query = metadata_snapshot->getSelectQuery();
    /// Make sure the dependency is removed after DETACH TABLE
    if (!select_query.select_table_id.empty())
        DatabaseCatalog::instance().removeViewDependency(select_query.select_table_id, getStorageID());
}

StoragePtr StorageMaterializedView::getTargetTable() const
{
    checkStackSize();
    return DatabaseCatalog::instance().getTable(getTargetTableId(), getContext());
}

StoragePtr StorageMaterializedView::tryGetTargetTable() const
{
    checkStackSize();
    return DatabaseCatalog::instance().tryGetTable(getTargetTableId(), getContext());
}

StoragePtr StorageMaterializedView::getScratchTable() const
{
    checkStackSize();
    return DatabaseCatalog::instance().getTable(getScratchTableId(), getContext());
}

StoragePtr StorageMaterializedView::tryGetScratchTable() const
{
    checkStackSize();
    return DatabaseCatalog::instance().tryGetTable(getScratchTableId(), getContext());
}

NamesAndTypesList StorageMaterializedView::getVirtuals() const
{
    return getTargetTable()->getVirtuals();
}

Strings StorageMaterializedView::getDataPaths() const
{
    Strings res;
    if (auto table = tryGetTargetTable())
    {
        Strings p = table->getDataPaths();
        res.insert(res.end(), p.begin(), p.end());
    }
    if (auto table = tryGetScratchTable())
    {
        Strings p = table->getDataPaths();
        res.insert(res.end(), p.begin(), p.end());
    }
    return res;
}

void StorageMaterializedView::backupData(BackupEntriesCollector & backup_entries_collector, const String & data_path_in_backup, const std::optional<ASTs> & partitions)
{
    /// We backup the target table's data only if it's inner.
    if (has_inner_target_table)
    {
        if (auto table = tryGetTargetTable())
            table->backupData(backup_entries_collector, data_path_in_backup, partitions);
        else
            LOG_WARNING(getLogger("StorageMaterializedView"),
                        "Inner table does not exist, will not backup any data");
    }
}

void StorageMaterializedView::restoreDataFromBackup(RestorerFromBackup & restorer, const String & data_path_in_backup, const std::optional<ASTs> & partitions)
{
    if (has_inner_target_table)
        return getTargetTable()->restoreDataFromBackup(restorer, data_path_in_backup, partitions);
}

bool StorageMaterializedView::supportsBackupPartition() const
{
    if (has_inner_target_table)
        return getTargetTable()->supportsBackupPartition();
    return false;
}

std::optional<UInt64> StorageMaterializedView::totalRows(const Settings & settings) const
{
    if (has_inner_target_table)
    {
        if (auto table = tryGetTargetTable())
            return table->totalRows(settings);
    }
    return {};
}

std::optional<UInt64> StorageMaterializedView::totalBytes(const Settings & settings) const
{
    if (has_inner_target_table)
    {
        if (auto table = tryGetTargetTable())
            return table->totalBytes(settings);
    }
    return {};
}

std::optional<UInt64> StorageMaterializedView::totalBytesUncompressed(const Settings & settings) const
{
    if (has_inner_target_table)
    {
        if (auto table = tryGetTargetTable())
            return table->totalBytesUncompressed(settings);
    }
    return {};
}

ActionLock StorageMaterializedView::getActionLock(StorageActionBlockType type)
{
    if (type == ActionLocks::ViewRefresh && refresher)
        refresher->stop();
    if (has_inner_target_table)
    {
        if (auto target_table = tryGetTargetTable())
            return target_table->getActionLock(type);
    }
    return ActionLock{};
}

bool StorageMaterializedView::isRemote() const
{
    if (auto table = tryGetTargetTable())
        return table->isRemote();
    return false;
}

std::vector<StorageID> StorageMaterializedView::innerTables() const
{
    std::lock_guard guard(inner_table_ids_mutex);
    std::vector<StorageID> res;
    if (has_inner_target_table)
        res.push_back(target_table_id);
    if (has_scratch_table)
        res.push_back(scratch_table_id);
    return res;
}

void StorageMaterializedView::onActionLockRemove(StorageActionBlockType action_type)
{
    if (action_type == ActionLocks::ViewRefresh && refresher)
        refresher->start();
}

DB::StorageID StorageMaterializedView::getTargetTableId() const
{
    std::lock_guard guard(inner_table_ids_mutex);
    return target_table_id;
}

DB::StorageID StorageMaterializedView::getScratchTableId() const
{
    std::lock_guard guard(inner_table_ids_mutex);
    return scratch_table_id;
}

void registerStorageMaterializedView(StorageFactory & factory)
{
    factory.registerStorage("MaterializedView", [](const StorageFactory::Arguments & args)
    {
        /// Pass local_context here to convey setting for inner table
        return std::make_shared<StorageMaterializedView>(
            args.table_id, args.getLocalContext(), args.query,
            args.columns, args.attach, args.comment);
    });
}

}
