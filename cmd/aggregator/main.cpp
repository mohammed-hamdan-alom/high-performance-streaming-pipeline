#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <hiredis/read.h>
#include <iostream>
#include <mutex>
#include <string>
#include <csignal>
#include <thread>
#include <queue>
#include <vector>
#include <chrono>
#include <atomic>
#include <librdkafka/rdkafka.h>
#include <hiredis/hiredis.h>
#include <google/protobuf/port.h>
#include <ostream>
#include <libpq-fe.h>
#include "market_data.pb.h"

static volatile sig_atomic_t run = 1;
std::atomic<long long> total_processed(0);
std::atomic<long long> total_latency_ns(0);
std::atomic<long long> min_latency_ns(LLONG_MAX);
std::atomic<long long> max_latency_ns(0);

struct MessageBatch {
    std::string ticker;
    double price;
    int volume;
    long long timestamp_ns;
    double latency_ms;
};

std::queue<MessageBatch> batch_queue;
std::mutex queue_mutex;

static void stop(int sig) {
    run = 0;
}

long long current_timestamp_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void update_latency_stats(long long latency_ns) {
    total_processed++;
    total_latency_ns += latency_ns;

    // Update min
    long long current_min = min_latency_ns.load();
    while (latency_ns < current_min &&
           !min_latency_ns.compare_exchange_weak(current_min, latency_ns));

    // Update max
    long long current_max = max_latency_ns.load();
    while (latency_ns > current_max &&
           !max_latency_ns.compare_exchange_weak(current_max, latency_ns));
}

void stats_reporter() {
    while (run) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        long long processed = total_processed.load();
        if (processed == 0) continue;

        long long total_lat = total_latency_ns.load();
        long long min_lat = min_latency_ns.load();
        long long max_lat = max_latency_ns.load();

        double avg_latency_ms = (total_lat / processed) / 1e6;
        double min_latency_ms = min_lat / 1e6;
        double max_latency_ms = max_lat / 1e6;

        int queue_size;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue_size = batch_queue.size();
        }

        std::cout << "\n=== Stats ===" << std::endl;
        std::cout << "Processed: " << processed << " | Queue: " << queue_size << std::endl;
        std::cout << "Latency (ms) - Avg: " << avg_latency_ms
                  << " | Min: " << min_latency_ms
                  << " | Max: " << max_latency_ms << std::endl;
        std::cout << "=============\n" << std::endl;
    }
}

PGconn* connect_to_timescale(const std::string& host) {
    std::string conninfo = "host=" + host + " port=5432 dbname=market_data user=postgres password=postgres";
    PGconn *conn = PQconnectdb(conninfo.c_str());

    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "Connection to TimescaleDB failed: " << PQerrorMessage(conn) << std::endl;
        PQfinish(conn);
        return NULL;
    }

    std::cout << "Connected to TimescaleDB successfully." << std::endl;
    // Create table if not exists
    const char* create_table = R"(
        CREATE TABLE IF NOT EXISTS market_updates (
            time TIMESTAMPTZ NOT NULL,
            ticker TEXT NOT NULL,
            price DOUBLE PRECISION NOT NULL,
            volume INTEGER NOT NULL,
            latency_ms DOUBLE PRECISION NOT NULL
        );

        SELECT create_hypertable('market_updates', 'time', if_not_exists => TRUE);

        CREATE INDEX IF NOT EXISTS idx_ticker_time ON market_updates (ticker, time DESC);
    )";
    PGresult *res = PQexec(conn, create_table);
    if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "Table creation failed: " << PQerrorMessage(conn) << std::endl;
        PQclear(res);
        PQfinish(conn);
        return NULL;
    }
    PQclear(res);

    std::cout << "TimescaleDB table ready." << std::endl;
    return conn;
}

void batch_writer(PGconn *conn) {
    const int BATCH_SIZE = 5000;
    const int FLUSH_INTERVAL_MS = 100;

    std::vector<MessageBatch> local_batch;
    local_batch.reserve(BATCH_SIZE);

    auto last_flush = std::chrono::steady_clock::now();
    long long total_written = 0;

    while (run) {

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            while (!batch_queue.empty() && local_batch.size() < BATCH_SIZE) {
                local_batch.push_back(batch_queue.front());
                batch_queue.pop();
            }
        }


        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count();

        bool should_flush = !local_batch.empty() &&
                            (local_batch.size() >= BATCH_SIZE || elapsed >= FLUSH_INTERVAL_MS);

        if (should_flush) {
            std::string query = "INSERT INTO market_updates (time, ticker, price, volume, latency_ms) VALUES ";

            for (size_t i = 0; i < local_batch.size(); i++) {
                const auto& msg = local_batch[i];
                long long timestamp_ms = msg.timestamp_ns / 1000000;

                char value_str[256];
                snprintf(value_str, sizeof(value_str),
                    "(to_timestamp(%lld / 1000.0), '%s', %f, %d, %f)",
                    timestamp_ms, msg.ticker.c_str(), msg.price, msg.volume, msg.latency_ms);
                query += value_str;
                if (i < local_batch.size() - 1) query += ",";
            }

            PGresult *res = PQexec(conn, query.c_str());

            if (PQresultStatus(res) == PGRES_COMMAND_OK) {
                total_written += local_batch.size();
            } else {
                std::cerr << "Batch insert failed: " << PQerrorMessage(conn) << std::endl;
            }

            PQclear(res);
            local_batch.clear();
            last_flush = now;

        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    if (!local_batch.empty()) {
        std::cout << "Flushing " << local_batch.size() << " remaining DB writes..." << std::endl;
    }
    std::cout << "Batch writer: Total written to DB: " << total_written << std::endl;
}


int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <kafka_broker> <redis_host>" << std::endl;
        std::cerr << "Example: " << argv[0] << " localhost:9092 localhost" << std::endl;
        return 1;
    }

    std::string brokers = argv[1];
    std::string redis_host = argv[2];
    std::string timescale_host = redis_host;
    std::string topic = "market-updates";
    std::string group_id = "aggregator_group";

    char errstr[512];

    // --- 1. SETUP REDIS CONNECTION ---

    std::cout << "Connecting to Redis at " << redis_host << ":6379..." << std::endl;
    redisContext *redis = redisConnect(redis_host.c_str(), 6379);
    if (redis == NULL || redis->err) {
        if (redis) std::cerr << "Redis Error: " << redis->errstr << std::endl;
        else std::cerr << "Can't allocate Redis context" << std::endl;
        return 1;
    }

    // TEST REDIS CONNECTION
    redisReply *ping_reply = (redisReply *)redisCommand(redis, "PING");
    if (ping_reply) {
        std::cout << "Redis PING: " << ping_reply->str << std::endl;
        freeReplyObject(ping_reply);
    }

    PGconn *timescale = connect_to_timescale(timescale_host);
    if (!timescale) {
        redisFree(redis);
        return 1;
    }

    std::thread writer_thread(batch_writer, timescale);

    std::cout << "Connected to Redis successfully." << std::endl;

    // --- 2. SETUP KAFKA CONSUMER ---
    rd_kafka_conf_t *conf = rd_kafka_conf_new();

    // Set consumer group (Kafka tracks progress for this group)
    rd_kafka_conf_set(conf, "group.id", group_id.c_str(), errstr, sizeof(errstr));
    // Start reading from the latest message if no offset is found
    rd_kafka_conf_set(conf, "auto.offset.reset", "latest", errstr, sizeof(errstr));
    // Set the bootstrap broker
    rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, sizeof(errstr));

    rd_kafka_conf_set(conf, "enable.auto.commit", "true", errstr, sizeof(errstr));


    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::cerr << "Failed to create consumer: " << errstr << std::endl;
        return 1;
    }

    // Redirect logs/errors to standard output
    rd_kafka_poll_set_consumer(rk);

    // Subscribe to the topic
    rd_kafka_topic_partition_list_t *topics = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(topics, topic.c_str(), RD_KAFKA_PARTITION_UA);
    rd_kafka_resp_err_t err = rd_kafka_subscribe(rk, topics);
        if (err) {
            std::cerr << "Failed to subscribe to topic: " << rd_kafka_err2str(err) << std::endl;
            rd_kafka_topic_partition_list_destroy(topics);
            return 1;
        }
    rd_kafka_topic_partition_list_destroy(topics);

    signal(SIGINT, stop);

    std::cout << "Aggregator started with batching." << std::endl;

    // Start stats reporter
    std::thread stats_thread(stats_reporter);

    // --- 3. MAIN PROCESSING LOOP ---

    long long msg_count = 0;
    int redis_pipeline_count = 0;
    while (run) {

        rd_kafka_message_t *rkmessage = rd_kafka_consumer_poll(rk, 100);

        if (!rkmessage) {
            // Flush any pending Redis commands during idle time
            if (redis_pipeline_count > 0) {
                for (int i = 0; i < redis_pipeline_count; i++) {
                    redisReply *reply;
                    if (redisGetReply(redis, (void**)&reply) == REDIS_OK) {
                        freeReplyObject(reply);
                    }
                }
                redis_pipeline_count = 0;
            }
            continue;
        }

        if (rkmessage->err) {
            if (rkmessage->err != RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                std::cerr << "Consumer error: " << rd_kafka_message_errstr(rkmessage) << std::endl;
            }
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        msg_count++;

        long long arrival_timestamp = current_timestamp_ns();

        // DESERIALIZATION: Protobuf magic
        marketdata::MarketUpdate update;
        if (update.ParseFromArray(rkmessage->payload, rkmessage->len)) {

            // Calculate end-to-end latency
            long long latency_ns = arrival_timestamp - update.timestamp_ns();
            if (latency_ns < 0) latency_ns = 0;
            double latency_ms = latency_ns / 1e6;

            update_latency_stats(latency_ns);

            redisAppendCommand(redis, "SET %s %f", update.ticker().c_str(), update.price());
            redis_pipeline_count++;

            if (redis_pipeline_count >= 100) {
                for (int i = 0; i < redis_pipeline_count; i++) {
                    redisReply *reply;
                    if (redisGetReply(redis, (void**)&reply) == REDIS_OK) {
                        freeReplyObject(reply);
                    }
                }
                redis_pipeline_count = 0;
            }

            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                batch_queue.push({
                    update.ticker(),
                    update.price(),
                    update.volume(),
                    update.timestamp_ns(),
                    latency_ms
                });
            }


        }

        rd_kafka_message_destroy(rkmessage);
    }

    if (redis_pipeline_count > 0) {
        for (int i = 0; i < redis_pipeline_count; i++) {
            redisReply *reply;
            if (redisGetReply(redis, (void**)&reply) == REDIS_OK) {
                freeReplyObject(reply);
            }
        }
    }

    // --- 4. CLEANUP ---
    std::cout << "\nShutting down aggregator..." << std::endl;
    std::cout << "Total messages consumed: " << msg_count << std::endl;

    if (writer_thread.joinable()) writer_thread.join();
    if (stats_thread.joinable()) stats_thread.join();

    rd_kafka_consumer_close(rk);
    rd_kafka_destroy(rk);
    redisFree(redis);
    PQfinish(timescale);

    return 0;
}
