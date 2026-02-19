#include <iostream>
#include <hiredis/hiredis.h>
#include <vector>
#include <iomanip>

int main(int argc, char **argv) {
    const char* host = (argc > 1) ? argv[1] : "127.0.0.1";

    redisContext *c = redisConnect(host, 6379);
    if (c == nullptr || c->err) {
        std::cerr << "Connection error: " << (c ? c->errstr : "allocate context") << std::endl;
        return 1;
    }

    // 1. Get all keys (Tickers)
    redisReply *reply = (redisReply *)redisCommand(c, "KEYS *");
    if (reply->type != REDIS_REPLY_ARRAY) {
        std::cerr << "Unexpected reply type" << std::endl;
        return 1;
    }

    std::cout << std::setw(10) << "TICKER" << " | " << std::setw(10) << "PRICE" << std::endl;
    std::cout << "---------------------------" << std::endl;

    // 2. For each key, get the value
    for (size_t i = 0; i < reply->elements; i++) {
        std::string ticker = reply->element[i]->str;
        redisReply *valReply = (redisReply *)redisCommand(c, "GET %s", ticker.c_str());

        if (valReply->type == REDIS_REPLY_STRING) {
            std::cout << std::setw(10) << ticker << " | " << std::setw(10) << valReply->str << std::endl;
        }
        freeReplyObject(valReply);
    }

    freeReplyObject(reply);
    redisFree(c);
    return 0;
}
