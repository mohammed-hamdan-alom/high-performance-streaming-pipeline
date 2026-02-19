#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <thread>
#include <csignal>
#include <atomic>
#include <librdkafka/rdkafka.h>
#include "market_data.pb.h"

long long current_timestamp_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

static volatile sig_atomic_t run = 1;
std::atomic<long long> total_messages(0);
std::atomic<long long> total_errors(0);

void sigterm(int sig) {
    run = 0;
}

void status_reporter() {
    auto last_count = total_messages.load();
    auto last_time = std::chrono::steady_clock::now();

    while (run) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        auto now = std::chrono::steady_clock::now();
        auto current_count = total_messages.load();
        auto errors = total_errors.load();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();
        auto messages_sent = current_count - last_count;
        auto throughput = (messages_sent * 1000.0) / elapsed;

        std::cout << "\n=== Stats (last 5s) ===" << std::endl;
        std::cout << "Messages sent: " << messages_sent << std::endl;
        std::cout << "Throughput: " << static_cast<int>(throughput) << " msg/sec" << std::endl;
        std::cout << "Total messages: " << current_count << std::endl;
        std::cout << "Total errors: " << errors << std::endl;
        std::cout << "=====================\n" << std::endl;

        last_count = current_count;
        last_time = now;
    }

}

rd_kafka_t* create_kafka_producer(const std::string& brokers) {
    char errstr[512];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();

    // Critical: Set metadata timeout
    if (rd_kafka_conf_set(conf, "metadata.max.age.ms", "30000", errstr, 512) != RD_KAFKA_CONF_OK) {
        std::cerr << "Config error (metadata.max.age.ms): " << errstr << std::endl;
        return NULL;
    }

    // Allow time for metadata retrieval
    if (rd_kafka_conf_set(conf, "socket.timeout.ms", "60000", errstr, 512) != RD_KAFKA_CONF_OK) {
        std::cerr << "Config error (socket.timeout.ms): " << errstr << std::endl;
        return NULL;
    }


    if (rd_kafka_conf_set(conf, "linger.ms", "10", errstr, 512) != RD_KAFKA_CONF_OK) {
        std::cerr << "Config error (linger.ms): " << errstr << std::endl;
        return NULL;
    }

    if (rd_kafka_conf_set(conf, "compression.codec", "snappy", errstr, 512) != RD_KAFKA_CONF_OK) {
        std::cerr << "Config error (compression.codec): " << errstr << std::endl;
        return NULL;
    }

    // Important: Increase queue buffering
    if (rd_kafka_conf_set(conf, "queue.buffering.max.messages", "100000", errstr, 512) != RD_KAFKA_CONF_OK) {
        std::cerr << "Config error (queue.buffering.max.messages): " << errstr << std::endl;
        return NULL;
    }

    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, 512) != RD_KAFKA_CONF_OK) {
        std::cerr << "Config error (bootstrap.servers): " << errstr << std::endl;
        return NULL;
    }

    rd_kafka_t *producer = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, 512);
    if (!producer) {
        std::cerr << "Failed to create Kafka producer: " << errstr << std::endl;
        return NULL;
    }

    std::cout << "Kafka producer configured successfully." << std::endl;
    std::cout << "Waiting for metadata..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    return producer;
}

void produce_data(rd_kafka_t *producer, const std::string& topic, const std::vector<std::string>& tickers) {
    rd_kafka_topic_t *rkt = rd_kafka_topic_new(producer, topic.c_str(), NULL);
    if (!rkt) {
        std::cerr << "Failed to create topic handle: " << rd_kafka_err2str(rd_kafka_last_error()) << std::endl;
        return;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> ticker_dist(0, tickers.size() - 1);
    std::uniform_real_distribution<> price_base_dist(95.0, 105.0);
    std::uniform_real_distribution<> price_change_dist(-0.5, 0.5);
    std::uniform_int_distribution<> volume_dist(100, 10000);

    // long long msg_count = 0;
    std::string serialized_data;

    while (run) {
        std::string current_ticker = tickers[ticker_dist(gen)];
        double current_price = price_base_dist(gen) + price_change_dist(gen);
        int current_volume = volume_dist(gen);

        long long produce_timestamp = current_timestamp_ns();

        marketdata::MarketUpdate update;
        update.set_ticker(current_ticker);
        update.set_price(current_price);
        update.set_volume(current_volume);
        update.set_timestamp_ns(produce_timestamp);

        // Clear previous data before serializing
        serialized_data.clear();
        update.SerializeToString(&serialized_data);

        rd_kafka_resp_err_t err;
        err = (rd_kafka_resp_err_t)rd_kafka_produce(
            rkt,
            RD_KAFKA_PARTITION_UA,
            RD_KAFKA_MSG_F_COPY,
            (void *)serialized_data.c_str(),
            serialized_data.length(),
            current_ticker.c_str(),
            current_ticker.length(),
            NULL
        );

        if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
            // std::cerr << "Produce failed: " << rd_kafka_err2str(err) << std::endl;
            total_errors++;

            // If queue is full, poll and retry once
            if (err == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
                rd_kafka_poll(producer, 100);
                continue;
            }
        } else {
            total_messages++;
        }

        rd_kafka_poll(producer, 0);

        // Add small delay to avoid overwhelming the system
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    std::cout << "\nFlushing final messages..." << std::endl;
    rd_kafka_flush(producer, 10000);

    rd_kafka_topic_destroy(rkt);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <broker list (e.g., localhost:9092)>" << std::endl;
        return 1;
    }

    signal(SIGINT, sigterm);

    rd_kafka_t *producer = create_kafka_producer(argv[1]);
    if (!producer) return 1;

    // Fixed: Removed space from topic name
    const std::string topic = "market-updates";
    const std::vector<std::string> sample_tickers = {"AAPL", "GOOG", "MSFT", "AMZN", "TSLA", "NVDA", "JPM", "BAC"};

    const int num_threads = 4;
    std::vector<std::thread> producer_threads;

    std::cout << "Starting " << num_threads << " producer threads..." << std::endl;

    std::thread stats_thread(status_reporter);

    for (int i = 0; i < num_threads; ++i) {
        producer_threads.emplace_back(produce_data, producer, topic, sample_tickers);
    }

    for (auto& t : producer_threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    if (stats_thread.joinable()) {
        stats_thread.join();
    }

    rd_kafka_destroy(producer);
    std::cout << "Producer shut down cleanly" << std::endl;
    return 0;
}
