#include <Storages/IStorage.h>
#include <Parsers/TablePropertiesQueriesASTs.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <QueryPipeline/BlockIO.h>
#include <DataTypes/DataTypesNumber.h>
#include <Columns/ColumnsNumber.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/InterpreterFactory.h>
#include <Interpreters/InterpreterExistsQuery.h>
#include <Access/Common/AccessFlags.h>
#include <Common/typeid_cast.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int SYNTAX_ERROR;
}

BlockIO InterpreterExistsQuery::execute()
{
    BlockIO res;
    res.pipeline = executeImpl();
    return res;
}


Block InterpreterExistsQuery::getSampleBlock()
{
    return Block{{
        ColumnUInt8::create(),
        std::make_shared<DataTypeUInt8>(),
        "result" }};
}


QueryPipeline InterpreterExistsQuery::executeImpl()
{
    ASTQueryWithTableAndOutput * exists_query;
    bool result = false;

    if ((exists_query = query_ptr->as<ASTExistsTableQuery>()))
    {
        if (exists_query->temporary)
        {
            result = static_cast<bool>(getContext()->tryResolveStorageID(
                {"", exists_query->getTable()}, Context::ResolveExternal));
        }
        else
        {
            String database = getContext()->resolveDatabase(exists_query->getDatabase());
            getContext()->checkAccess(AccessType::SHOW_TABLES, database, exists_query->getTable());
            result = DatabaseCatalog::instance().isTableExist({database, exists_query->getTable()}, getContext());
        }
    }
    else if ((exists_query = query_ptr->as<ASTExistsViewQuery>()))
    {
        String database = getContext()->resolveDatabase(exists_query->getDatabase());
        getContext()->checkAccess(AccessType::SHOW_TABLES, database, exists_query->getTable());
        auto table = DatabaseCatalog::instance().tryGetTable({database, exists_query->getTable()}, getContext());
        result = table && table->isView();
    }
    else if ((exists_query = query_ptr->as<ASTExistsDatabaseQuery>()))
    {
        String database = getContext()->resolveDatabase(exists_query->getDatabase());
        getContext()->checkAccess(AccessType::SHOW_DATABASES, database);
        result = DatabaseCatalog::instance().isDatabaseExist(database);
    }
    else if ((exists_query = query_ptr->as<ASTExistsDictionaryQuery>()))
    {
        if (exists_query->temporary)
            throw Exception(ErrorCodes::SYNTAX_ERROR, "Temporary dictionaries are not possible.");
        String database = getContext()->resolveDatabase(exists_query->getDatabase());
        getContext()->checkAccess(AccessType::SHOW_DICTIONARIES, database, exists_query->getTable());
        result = DatabaseCatalog::instance().isDictionaryExist({database, exists_query->getTable()});
    }

    return QueryPipeline(std::make_shared<SourceFromSingleChunk>(std::make_shared<const Block>(Block{{
        ColumnUInt8::create(1, result),
        std::make_shared<DataTypeUInt8>(),
        "result" }})));
}

void registerInterpreterExistsQuery(InterpreterFactory & factory)
{
    auto create_fn = [] (const InterpreterFactory::Arguments & args)
    {
        return std::make_unique<InterpreterExistsQuery>(args.query, args.context);
    };
    factory.registerInterpreter("InterpreterExistsQuery", create_fn);
}
}
