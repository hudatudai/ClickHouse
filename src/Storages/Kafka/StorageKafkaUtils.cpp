#include <Storages/Kafka/StorageKafkaUtils.h>


#include <Core/Settings.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Databases/DatabaseReplicatedHelpers.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTCreateQuery.h>
#include <Storages/checkAndGetLiteralArgument.h>
#include <Storages/IStorage.h>
#include <Storages/Kafka/KafkaSettings.h>
#include <Storages/Kafka/StorageKafka.h>
#include <Storages/Kafka/StorageKafka2.h>
#include <Storages/Kafka/parseSyslogLevel.h>
#include <Storages/NamedCollectionsHelpers.h>
#include <Storages/StorageFactory.h>
#include <Storages/StorageMaterializedView.h>
#include <base/getFQDNOrHostName.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Common/CurrentMetrics.h>
#include <Common/NamedCollections/NamedCollectionsFactory.h>
#include <Common/ThreadPool.h>
#include <Common/ThreadStatus.h>
#include <Common/config_version.h>
#include <Common/getNumberOfCPUCoresToUse.h>
#include <Common/logger_useful.h>
#include <Common/setThreadName.h>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <cppkafka/cppkafka.h>
#include <librdkafka/rdkafka.h>

#if USE_KRB5
#    include <Access/KerberosInit.h>
#endif // USE_KRB5

namespace ProfileEvents
{
extern const Event KafkaConsumerErrors;
}

namespace DB
{
namespace Setting
{
    extern const SettingsBool allow_experimental_kafka_offsets_storage_in_keeper;
    extern const SettingsBool kafka_disable_num_consumers_limit;
}

namespace KafkaSetting
{
    extern const KafkaSettingsUInt64 input_format_allow_errors_num;
    extern const KafkaSettingsFloat input_format_allow_errors_ratio;
    extern const KafkaSettingsBool input_format_skip_unknown_fields;
    extern const KafkaSettingsString kafka_broker_list;
    extern const KafkaSettingsString kafka_client_id;
    extern const KafkaSettingsBool kafka_commit_every_batch;
    extern const KafkaSettingsBool kafka_commit_on_select;
    extern const KafkaSettingsMilliseconds kafka_flush_interval_ms;
    extern const KafkaSettingsString kafka_format;
    extern const KafkaSettingsString kafka_group_name;
    extern const KafkaSettingsStreamingHandleErrorMode kafka_handle_error_mode;
    extern const KafkaSettingsString kafka_keeper_path;
    extern const KafkaSettingsUInt64 kafka_max_block_size;
    extern const KafkaSettingsUInt64 kafka_max_rows_per_message;
    extern const KafkaSettingsUInt64 kafka_num_consumers;
    extern const KafkaSettingsUInt64 kafka_poll_max_batch_size;
    extern const KafkaSettingsMilliseconds kafka_poll_timeout_ms;
    extern const KafkaSettingsString kafka_replica_name;
    extern const KafkaSettingsString kafka_schema;
    extern const KafkaSettingsUInt64 kafka_skip_broken_messages;
    extern const KafkaSettingsBool kafka_thread_per_consumer;
    extern const KafkaSettingsString kafka_topic_list;
}

using namespace std::chrono_literals;

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int SUPPORT_IS_DISABLED;
}


void registerStorageKafka(StorageFactory & factory)
{
    auto creator_fn = [](const StorageFactory::Arguments & args) -> std::shared_ptr<IStorage>
    {
        ASTs & engine_args = args.engine_args;
        size_t args_count = engine_args.size();
        const bool has_settings = args.storage_def->settings;

        auto kafka_settings = std::make_unique<KafkaSettings>();
        String collection_name;
        if (auto named_collection = tryGetNamedCollectionWithOverrides(args.engine_args, args.getLocalContext()))
        {
            kafka_settings->loadFromNamedCollection(named_collection);
            collection_name = assert_cast<const ASTIdentifier *>(args.engine_args[0].get())->name();
        }

        if (has_settings)
        {
            kafka_settings->loadFromQuery(*args.storage_def);
        }

// Check arguments and settings
#define CHECK_KAFKA_STORAGE_ARGUMENT(ARG_NUM, PAR_NAME, EVAL, TYPE) \
    /* One of the four required arguments is not specified */ \
    if (args_count < (ARG_NUM) && (ARG_NUM) <= 4 && !(*kafka_settings)[KafkaSetting::PAR_NAME].changed) \
    { \
        throw Exception( \
            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, \
            "Required parameter '{}' " \
            "for storage Kafka not specified", \
            #PAR_NAME); \
    } \
    if (args_count >= (ARG_NUM)) \
    { \
        /* The same argument is given in two places */ \
        if (has_settings && (*kafka_settings)[KafkaSetting::PAR_NAME].changed) \
            throw Exception( \
                ErrorCodes::BAD_ARGUMENTS, \
                "The argument №{} of storage Kafka " \
                "and the parameter '{}' " \
                "in SETTINGS cannot be specified at the same time", \
                #ARG_NUM, \
                #PAR_NAME); \
        /* move engine args to settings */ \
        if constexpr ((EVAL) == 1) \
        { \
            engine_args[(ARG_NUM)-1] = evaluateConstantExpressionAsLiteral(engine_args[(ARG_NUM)-1], args.getLocalContext()); \
        } \
        if constexpr ((EVAL) == 2) \
        { \
            engine_args[(ARG_NUM)-1] \
                = evaluateConstantExpressionOrIdentifierAsLiteral(engine_args[(ARG_NUM)-1], args.getLocalContext()); \
        } \
        (*kafka_settings)[KafkaSetting::PAR_NAME] = checkAndGetLiteralArgument<TYPE>(engine_args[(ARG_NUM)-1], #PAR_NAME); \
    }

        /** Arguments of engine is following:
          * - Kafka broker list
          * - List of topics
          * - Group ID (may be a constant expression with a string result)
          * - Message format (string)
          * - Row delimiter
          * - Schema (optional, if the format supports it)
          * - Number of consumers
          * - Max block size for background consumption
          * - Skip (at least) unreadable messages number
          * - Do intermediate commits when the batch consumed and handled
          */

        /* 0 = raw, 1 = evaluateConstantExpressionAsLiteral, 2=evaluateConstantExpressionOrIdentifierAsLiteral */
        /// In case of named collection we already validated the arguments.
        if (collection_name.empty())
        {
            CHECK_KAFKA_STORAGE_ARGUMENT(1, kafka_broker_list, 0, String)
            CHECK_KAFKA_STORAGE_ARGUMENT(2, kafka_topic_list, 1, String)
            CHECK_KAFKA_STORAGE_ARGUMENT(3, kafka_group_name, 2, String)
            CHECK_KAFKA_STORAGE_ARGUMENT(4, kafka_format, 2, String)
            CHECK_KAFKA_STORAGE_ARGUMENT(6, kafka_schema, 2, String)
            CHECK_KAFKA_STORAGE_ARGUMENT(7, kafka_num_consumers, 0, UInt64)
            CHECK_KAFKA_STORAGE_ARGUMENT(8, kafka_max_block_size, 0, UInt64)
            CHECK_KAFKA_STORAGE_ARGUMENT(9, kafka_skip_broken_messages, 0, UInt64)
            CHECK_KAFKA_STORAGE_ARGUMENT(10, kafka_commit_every_batch, 0, bool)
            CHECK_KAFKA_STORAGE_ARGUMENT(11, kafka_client_id, 2, String)
            CHECK_KAFKA_STORAGE_ARGUMENT(12, kafka_poll_timeout_ms, 0, UInt64)
            CHECK_KAFKA_STORAGE_ARGUMENT(13, kafka_flush_interval_ms, 0, UInt64)
            CHECK_KAFKA_STORAGE_ARGUMENT(14, kafka_thread_per_consumer, 0, bool)
            CHECK_KAFKA_STORAGE_ARGUMENT(15, kafka_handle_error_mode, 0, String)
            CHECK_KAFKA_STORAGE_ARGUMENT(16, kafka_commit_on_select, 0, bool)
            CHECK_KAFKA_STORAGE_ARGUMENT(17, kafka_max_rows_per_message, 0, UInt64)
        }

#undef CHECK_KAFKA_STORAGE_ARGUMENT

        auto num_consumers = (*kafka_settings)[KafkaSetting::kafka_num_consumers].value;
        auto max_consumers = std::max<uint32_t>(getNumberOfCPUCoresToUse(), 16);

        if (!args.getLocalContext()->getSettingsRef()[Setting::kafka_disable_num_consumers_limit] && num_consumers > max_consumers)
        {
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "The number of consumers can not be bigger than {}. "
                "A single consumer can read any number of partitions. "
                "Extra consumers are relatively expensive, "
                "and using a lot of them can lead to high memory and CPU usage. "
                "To achieve better performance "
                "of getting data from Kafka, consider using a setting kafka_thread_per_consumer=1, "
                "and ensure you have enough threads "
                "in MessageBrokerSchedulePool (background_message_broker_schedule_pool_size). "
                "See also https://clickhouse.com/docs/integrations/kafka/kafka-table-engine#tuning-performance",
                max_consumers);
        }
        if (num_consumers < 1)
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Number of consumers can not be lower than 1");
        }

        if ((*kafka_settings)[KafkaSetting::kafka_max_block_size].changed && (*kafka_settings)[KafkaSetting::kafka_max_block_size].value < 1)
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "kafka_max_block_size can not be lower than 1");
        }

        if ((*kafka_settings)[KafkaSetting::kafka_poll_max_batch_size].changed && (*kafka_settings)[KafkaSetting::kafka_poll_max_batch_size].value < 1)
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "kafka_poll_max_batch_size can not be lower than 1");
        }
        NamesAndTypesList supported_columns;
        for (const auto & column : args.columns)
        {
            if (column.default_desc.kind == ColumnDefaultKind::Alias)
                supported_columns.emplace_back(column.name, column.type);
            if (column.default_desc.kind == ColumnDefaultKind::Default && !column.default_desc.expression)
                supported_columns.emplace_back(column.name, column.type);
        }
        // Kafka engine allows only ordinary columns without default expression or alias columns.
        if (args.columns.getAll() != supported_columns)
        {
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "KafkaEngine doesn't support DEFAULT/MATERIALIZED/EPHEMERAL expressions for columns. "
                "See https://clickhouse.com/docs/engines/table-engines/integrations/kafka/#configuration");
        }

        const auto has_keeper_path = (*kafka_settings)[KafkaSetting::kafka_keeper_path].changed && !(*kafka_settings)[KafkaSetting::kafka_keeper_path].value.empty();
        const auto has_replica_name = (*kafka_settings)[KafkaSetting::kafka_replica_name].changed && !(*kafka_settings)[KafkaSetting::kafka_replica_name].value.empty();

        if (!has_keeper_path && !has_replica_name)
            return std::make_shared<StorageKafka>(
                args.table_id, args.getContext(), args.columns, args.comment, std::move(kafka_settings), collection_name);

        if (!args.getLocalContext()->getSettingsRef()[Setting::allow_experimental_kafka_offsets_storage_in_keeper] && !args.query.attach)
            throw Exception(
                ErrorCodes::SUPPORT_IS_DISABLED,
                "Storing the Kafka offsets in Keeper is experimental. Set `allow_experimental_kafka_offsets_storage_in_keeper` setting "
                "to enable it");

        if (!has_keeper_path || !has_replica_name)
            throw Exception(
        ErrorCodes::BAD_ARGUMENTS, "Either specify both zookeeper path and replica name or none of them");

        const auto is_on_cluster = args.getLocalContext()->getClientInfo().query_kind == ClientInfo::QueryKind::SECONDARY_QUERY;
        const auto is_replicated_database = args.getLocalContext()->getClientInfo().query_kind == ClientInfo::QueryKind::SECONDARY_QUERY
            && DatabaseCatalog::instance().getDatabase(args.table_id.database_name)->getEngineName() == "Replicated";

        // UUID macro is only allowed:
        // - with Atomic database only with ON CLUSTER queries, otherwise it is easy to misuse: each replica would have separate uuid generated.
        // - with Replicated database
        // - with attach queries, as those are used on server startup
        const auto allow_uuid_macro = is_on_cluster || is_replicated_database || args.query.attach;

        auto context = args.getContext();
        // Unfold {database} and {table} macro on table creation, so table can be renamed.
        if (args.mode < LoadingStrictnessLevel::ATTACH)
        {
            Macros::MacroExpansionInfo info;
            /// NOTE: it's not recursive
            info.expand_special_macros_only = true;
            info.table_id = args.table_id;
            // We could probably unfold UUID here too, but let's keep it similar to ReplicatedMergeTree, which doesn't do the unfolding.
            info.table_id.uuid = UUIDHelpers::Nil;
            (*kafka_settings)[KafkaSetting::kafka_keeper_path].value = context->getMacros()->expand((*kafka_settings)[KafkaSetting::kafka_keeper_path].value, info);

            info.level = 0;
            (*kafka_settings)[KafkaSetting::kafka_replica_name].value = context->getMacros()->expand((*kafka_settings)[KafkaSetting::kafka_replica_name].value, info);
        }


        auto * settings_query = args.storage_def->settings;
        chassert(has_settings && "Unexpected settings query in StorageKafka");

        settings_query->changes.setSetting("kafka_keeper_path", (*kafka_settings)[KafkaSetting::kafka_keeper_path].value);
        settings_query->changes.setSetting("kafka_replica_name", (*kafka_settings)[KafkaSetting::kafka_replica_name].value);

        // Expand other macros (such as {replica}). We do not expand them on previous step to make possible copying metadata files between replicas.
        // Disable expanding {shard} macro, because it can lead to incorrect behavior and it doesn't make sense to shard Kafka tables.
        Macros::MacroExpansionInfo info;
        info.table_id = args.table_id;
        if (is_replicated_database)
        {
            auto database = DatabaseCatalog::instance().getDatabase(args.table_id.database_name);
            info.shard.reset();
            info.replica = getReplicatedDatabaseReplicaName(database);
        }
        if (!allow_uuid_macro)
            info.table_id.uuid = UUIDHelpers::Nil;
        (*kafka_settings)[KafkaSetting::kafka_keeper_path].value = context->getMacros()->expand((*kafka_settings)[KafkaSetting::kafka_keeper_path].value, info);

        info.level = 0;
        info.table_id.uuid = UUIDHelpers::Nil;
        (*kafka_settings)[KafkaSetting::kafka_replica_name].value = context->getMacros()->expand((*kafka_settings)[KafkaSetting::kafka_replica_name].value, info);

        return std::make_shared<StorageKafka2>(
            args.table_id, args.getContext(), args.columns, args.comment, std::move(kafka_settings), collection_name);
    };

    factory.registerStorage(
        "Kafka",
        creator_fn,
        StorageFactory::StorageFeatures{
            .supports_settings = true,
            .source_access_type = AccessTypeObjects::Source::KAFKA,
            .has_builtin_setting_fn = KafkaSettings::hasBuiltin,
        });
}

template <typename RevocationCb, typename AssignmentCb>
void stopConsumerImpl(
    cppkafka::Consumer& consumer,
    RevocationCb revocation_cb,
    AssignmentCb assignment_cb,
    const std::chrono::milliseconds drain_timeout,
    const LoggerPtr& log,
    StorageKafkaUtils::ErrorHandler error_handler)
{
    consumer.set_revocation_callback(revocation_cb);

    consumer.set_assignment_callback(assignment_cb);

    try
    {
        auto assignment = consumer.get_assignment();

        if (!assignment.empty())
        {
            consumer.pause_partitions(assignment);

            for (const auto& partition : assignment)
            {
                // that call disables the forwarding of the messages to the customer queue
                consumer.get_partition_queue(partition);
            }
        }
    }
    catch (const cppkafka::HandleException & e)
    {
        LOG_ERROR(log, "Error during pause (stopConsumerImpl): {}", e.what());
    }

    StorageKafkaUtils::drainConsumer(consumer, drain_timeout, log, std::move(error_handler));
}

namespace StorageKafkaUtils
{
Names parseTopics(String topic_list)
{
    Names result;
    boost::split(result, topic_list, [](char c) { return c == ','; });
    for (String & topic : result)
        boost::trim(topic);
    return result;
}

String getDefaultClientId(const StorageID & table_id)
{
    return fmt::format("{}-{}-{}-{}", VERSION_NAME, getFQDNOrHostName(), table_id.database_name, table_id.table_name);
}

void consumerGracefulStop(
    cppkafka::Consumer & consumer, const std::chrono::milliseconds drain_timeout, const LoggerPtr & log, ErrorHandler error_handler)
{
    // Note: librdkafka is very sensitive to the proper termination sequence and have some race conditions there.
    // Before destruction, our objectives are:
    //   (1) Process all outstanding callbacks by polling the event queue.
    //   (2) Ensure that only special events (e.g. callbacks, rebalances) are polled (we don't want to poll regular messages).
    //
    // Previously, we performed an unsubscribe to stop message consumption and clear 'read' messages.
    // However, unsubscribe triggers a rebalance that schedules additional background tasks, such as locking
    // and removal of internal toppar queues. Meanwhile, polling to release callbacks may concurrently
    // cause those same queues to be destroyed.
    // This can lead to a situation where the background thread doing rebalance and the current thread doing polling access
    // the toppar queues simultaneously, potentially locking them in a different order, which risks a deadlock.
    //
    // To mitigate this, we now:
    //   (1) Avoid calling unsubscribe (letting rebalance occur naturally via consumer group timeout).
    //   (2) Set up different rebalance callbacks to repeat (3) if a rebalance will occur before consumer destruction.
    //   (3) Pause the consumer to stop processing new messages.
    //   (4) Disconnect the toppar queues to reduce the risk of lock inversion (less cascading locks).
    //   (5) Poll the event queue to process any remaining callbacks.

    stopConsumerImpl(
        consumer,
        /*revocation*/ [](const cppkafka::TopicPartitionList &)
        {
            // we don't care during the destruction
        },
        /*assignment*/ [&consumer](const cppkafka::TopicPartitionList & topic_partitions)
        {
            if (!topic_partitions.empty())
            {
                consumer.pause_partitions(topic_partitions);
            }

            // it's not clear if get_partition_queue will work in that context
            // as just after processing the callback cppkafka will call run assign
            // and that can reset the queues
        },
        drain_timeout, log, std::move(error_handler));
}

void consumerStopWithoutRebalance(
    cppkafka::Consumer & consumer, const std::chrono::milliseconds drain_timeout, const LoggerPtr & log, ErrorHandler error_handler)
{
    stopConsumerImpl(
        consumer,
        /*revocation*/ [](const cppkafka::TopicPartitionList &)
        {
            // we don't care during the destruction
        },
        /*assignment*/ [](const cppkafka::TopicPartitionList &)
        {
            // we don't care during the destruction
        },
        drain_timeout, log, std::move(error_handler));
}

// Needed to drain rest of the messages / queued callback calls from the consumer after unsubscribe, otherwise consumer
// will hang on destruction. Partition queues doesn't have to be attached as events are not handled by those queues.
// see https://github.com/edenhill/librdkafka/issues/2077
//     https://github.com/confluentinc/confluent-kafka-go/issues/189 etc.
void drainConsumer(
    cppkafka::Consumer & consumer, const std::chrono::milliseconds drain_timeout, const LoggerPtr & log, ErrorHandler error_handler)
{
    auto start_time = std::chrono::steady_clock::now();
    cppkafka::Error last_error(RD_KAFKA_RESP_ERR_NO_ERROR);

    while (true)
    {
        auto msg = consumer.poll(100ms);
        if (!msg)
            break;

        auto error = msg.get_error();

        if (error)
        {
            if (msg.is_eof() || error == last_error)
            {
                break;
            }

            LOG_ERROR(log, "Error during draining: {}", error);
            error_handler(error);
        }

        // i don't stop draining on first error,
        // only if it repeats once again sequentially
        last_error = error;

        auto ts = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(ts - start_time) > drain_timeout)
        {
            LOG_ERROR(log, "Timeout during draining.");
            break;
        }
    }
}

size_t eraseMessageErrors(Messages & messages, const LoggerPtr & log, ErrorHandler error_handler)
{
    size_t skipped = std::erase_if(
        messages,
        [&](auto & message)
        {
            if (auto error = message.get_error())
            {
                ProfileEvents::increment(ProfileEvents::KafkaConsumerErrors);
                LOG_ERROR(log, "Consumer error: {}", error);
                error_handler(error);
                return true;
            }
            return false;
        });

    if (skipped)
        LOG_ERROR(log, "There were {} messages with an error", skipped);

    return skipped;
}

SettingsChanges createSettingsAdjustments(KafkaSettings & kafka_settings, const String & schema_name)
{
    SettingsChanges result;
    // Needed for backward compatibility
    if (!kafka_settings[KafkaSetting::input_format_skip_unknown_fields].changed)
    {
        // Always skip unknown fields regardless of the context (JSON or TSKV)
        kafka_settings[KafkaSetting::input_format_skip_unknown_fields] = true;
    }

    if (!kafka_settings[KafkaSetting::input_format_allow_errors_ratio].changed)
    {
        kafka_settings[KafkaSetting::input_format_allow_errors_ratio] = 0.;
    }

    if (!kafka_settings[KafkaSetting::input_format_allow_errors_num].changed)
    {
        kafka_settings[KafkaSetting::input_format_allow_errors_num] = kafka_settings[KafkaSetting::kafka_skip_broken_messages].value;
    }

    if (!schema_name.empty())
        result.emplace_back("format_schema", schema_name);

    auto kafka_format_settings = kafka_settings.getFormatSettings();
    result.insert(result.end(), kafka_format_settings.begin(), kafka_format_settings.end());

    /// It does not make sense to use auto detection here, since the format
    /// will be reset for each message, plus, auto detection takes CPU
    /// time.
    result.setSetting("input_format_csv_detect_header", false);
    result.setSetting("input_format_tsv_detect_header", false);
    result.setSetting("input_format_custom_detect_header", false);

    return result;
}

bool checkDependencies(const StorageID & table_id, const ContextPtr& context)
{
    // Check if all dependencies are attached
    auto view_ids = DatabaseCatalog::instance().getDependentViews(table_id);
    if (view_ids.empty())
        return false;

    // Check the dependencies are ready?
    for (const auto & view_id : view_ids)
    {
        auto view = DatabaseCatalog::instance().tryGetTable(view_id, context);
        if (!view)
            return false;

        // If it materialized view, check it's target table
        auto * materialized_view = dynamic_cast<StorageMaterializedView *>(view.get());
        if (materialized_view && !materialized_view->tryGetTargetTable())
            return false;
    }

    return true;
}

VirtualColumnsDescription createVirtuals(StreamingHandleErrorMode handle_error_mode)
{
    VirtualColumnsDescription desc;

    desc.addEphemeral("_topic", std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>()), "");
    desc.addEphemeral("_key", std::make_shared<DataTypeString>(), "");
    desc.addEphemeral("_offset", std::make_shared<DataTypeUInt64>(), "");
    desc.addEphemeral("_partition", std::make_shared<DataTypeUInt64>(), "");
    desc.addEphemeral("_timestamp", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeDateTime>()), "");
    desc.addEphemeral("_timestamp_ms", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeDateTime64>(3)), "");
    desc.addEphemeral("_headers.name", std::make_shared<DataTypeArray>(std::make_shared<DataTypeString>()), "");
    desc.addEphemeral("_headers.value", std::make_shared<DataTypeArray>(std::make_shared<DataTypeString>()), "");

    if (handle_error_mode == StreamingHandleErrorMode::STREAM)
    {
        desc.addEphemeral("_raw_message", std::make_shared<DataTypeString>(), "");
        desc.addEphemeral("_error", std::make_shared<DataTypeString>(), "");
    }

    return desc;
}
}
}
